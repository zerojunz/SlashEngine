// Turntable views of a model and a vision model's verdict on them, for asset.review (review.hpp).

#include "review.hpp"

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/math.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace slash::library::review {

namespace {

using command::Capability;
using command::Session;

constexpr float kDegrees = 0.017453292F; // radians in a degree
// The camera's vertical field of view, narrow so the model's proportions show.
constexpr float kFov = 30.0F;
// How far above the model the camera looks down, and where the first view starts round it.
constexpr float kElevation = 20.0F * kDegrees;
constexpr float kFirstView = 30.0F * kDegrees;

constexpr std::string_view kInstructions =
    "You review a 3D model made for a game. You see it from several places round it, each view "
    "turned further round, all from a little above, on a plain backdrop. Say whether the views "
    "show what the request asks for: the right kind of thing, its parts, proportions, colors "
    "and materials. List each problem you can see, with the number of a view that shows it: a "
    "missing or extra part, a wrong shape or color, holes, parts floating or intersecting, "
    "stretched or blurry textures. Do not list matters of taste, or what the request does not "
    "ask about. If the model matches the request, say so and list no problems.";

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

json::Value text_property(std::string_view type, std::string_view description) {
    json::Value value;
    value["type"] = type;
    value["description"] = description;
    return value;
}

json::Value output_schema() {
    json::Value problem;
    problem["type"] = "object";
    problem["additionalProperties"] = false;
    problem["required"] = json::Array{"view", "problem"};
    problem["properties"]["view"] = text_property("integer", "The number of a view showing it.");
    problem["properties"]["problem"] = text_property("string", "What is wrong, in a sentence.");
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"matches", "problems"};
    schema["properties"]["matches"] =
        text_property("boolean", "Whether the model is what the request asks for.");
    schema["properties"]["problems"]["type"] = "array";
    schema["properties"]["problems"]["items"] = std::move(problem);
    return schema;
}

} // namespace

Result<std::vector<render::Image>> turntable(Session& session, const std::string& root,
                                             const std::string& path, const scene3d::Bounds& bounds,
                                             const Views& views) {
    // A session of its own over the folder, which writes nothing, as tests run.
    command::SessionOptions options = session.options();
    options.project_root = root;
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    Session shown(session.components(), session.commands(), options);
    world::World& world = shown.world();
    entt::registry& ecs = world.ecs();
    ui::set_viewport(world, {static_cast<float>(views.size), static_cast<float>(views.size)});

    const Vec3 center = (bounds.min + bounds.max) * 0.5F;
    const float radius = std::max(length(bounds.max - bounds.min) * 0.5F, 0.01F);
    // Far enough that the bounding sphere fills the view, with a little room round it.
    const float distance = radius / cos_sin(kFov * 0.5F * kDegrees).y * 1.1F;

    const entt::entity model = world.create();
    ecs.emplace<world::Transform3D>(model);
    ecs.emplace<world::MeshInstance3D>(model).mesh = path;
    const entt::entity backdrop = world.create();
    world::WorldEnvironment environment;
    environment.background_mode = "color";
    environment.background_color = {0.32F, 0.34F, 0.38F, 1.0F};
    environment.ambient_light_source = "color";
    environment.ambient_light_color = {1.0F, 1.0F, 1.0F, 1.0F};
    environment.ambient_light_energy = 0.25F;
    ecs.emplace<world::WorldEnvironment>(backdrop, environment);
    const entt::entity camera = world.create();
    ecs.emplace<world::Transform3D>(camera);
    world::Camera3D lens;
    lens.fov = kFov;
    lens.near = std::max(distance - (radius * 2.0F), distance * 0.01F);
    lens.far = distance + (radius * 2.0F);
    lens.current = true;
    ecs.emplace<world::Camera3D>(camera, lens);
    const entt::entity key = world.create();
    ecs.emplace<world::Transform3D>(key);
    ecs.emplace<world::DirectionalLight3D>(key);
    const entt::entity fill = world.create();
    ecs.emplace<world::Transform3D>(fill);
    world::DirectionalLight3D fill_light;
    fill_light.light_energy = 0.3F;
    ecs.emplace<world::DirectionalLight3D>(fill, fill_light);

    std::vector<render::Image> out;
    const Vec2 up = cos_sin(kElevation);
    for (std::uint32_t i = 0; i < views.count; ++i) {
        const float turn =
            kFirstView + (6.2831853F * static_cast<float>(i) / static_cast<float>(views.count));
        const Vec2 round = cos_sin(turn);
        const Vec3 back{round.y * up.x, up.y, round.x * up.x};
        auto& placed = ecs.get<world::Transform3D>(camera);
        placed.position = center + (back * distance);
        placed.rotation = {-kElevation, turn, 0.0F};
        // The key light from above the camera's left, the fill from its right and lower.
        ecs.get<world::Transform3D>(key).rotation = {-45.0F * kDegrees, turn - (35.0F * kDegrees),
                                                     0.0F};
        ecs.get<world::Transform3D>(fill).rotation = {-10.0F * kDegrees, turn + (60.0F * kDegrees),
                                                      0.0F};
        auto captured = canvas::capture(world, shown.assets());
        if (!captured) {
            return captured.error();
        }
        out.push_back(std::move(captured.value().image));
    }
    return out;
}

Result<Verdict> ask(Session& session, const std::string& role, const std::string& request,
                    const std::string& facts, const std::vector<render::Image>& views) {
    const std::string& root = session.options().project_root;
    auto config = ai::load_config(root);
    if (!config) {
        return config.error();
    }
    auto opened = ai::open(config.value(), role,
                           {.project_root = root, .write_replays = session.options().write_files});
    if (!opened) {
        return opened.error();
    }
    ai::Message user{.role = ai::Role::User};
    user.text = "Request: " + request + "\n";
    if (!facts.empty()) {
        user.text += "What is known about the model: " + facts + "\n";
    }
    user.text += "Views 1 to " + std::to_string(views.size()) + " follow, in order.";
    for (const render::Image& view : views) {
        auto png = render::encode_png(view);
        if (!png) {
            return png.error();
        }
        user.images.push_back({"image/png", std::move(png).value()});
    }
    ai::Request call;
    call.messages.push_back({.role = ai::Role::System, .text = std::string(kInstructions)});
    call.messages.push_back(std::move(user));
    call.output_schema = output_schema();
    auto response = opened.value().provider->complete(call);
    if (!response) {
        return response.error();
    }
    const json::Value& output = response.value().output;
    if (const std::vector<json::ValidationError> errors =
            json::validate(output, call.output_schema);
        !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the model's reply does not give a verdict as asked: " +
                              errors.front().message);
    }
    Verdict verdict;
    verdict.matches = output.find("matches")->as_bool();
    for (const json::Value& problem : output.find("problems")->as_array()) {
        const std::int64_t view = problem.find("view")->as_int();
        if (view < 1 || std::cmp_greater(view, views.size())) {
            ++verdict.unverified;
            continue;
        }
        verdict.problems.push_back(problem);
    }
    verdict.model = response.value().model.empty() ? opened.value().model : response.value().model;
    verdict.input_tokens = response.value().usage.input_tokens;
    verdict.output_tokens = response.value().usage.output_tokens;
    verdict.replayed = response.value().replayed;
    return verdict;
}

} // namespace slash::library::review
