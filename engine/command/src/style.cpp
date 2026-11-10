// style.mark and style.check: the storyboard's memory of a game's look. A style reference keeps
// how its frame was captured, what made it (lights, environment, cameras, materials and the
// project's rendering settings) and a fingerprint measured from the frame
// (slash/storyboard/fingerprint.hpp), so weeks of autonomous work can be held to one look by
// numbers, and by a vision model's words through storyboard.review where one is configured.
#include "slash/command/style.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/canvas/material.hpp"
#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/geometry.hpp"
#include "slash/scene3d/material.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/storyboard/fingerprint.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::string_view kKind = "style/";
constexpr std::string_view kFrames = "storyboard/style/";
constexpr std::string_view kStyleJournal = "storyboard/style_journal.jsonl";
constexpr std::string_view kReviewFrames = ".slash/style/";
// Scene frames are captured at most this wide, keeping the window's shape; models and materials
// are drawn on a stage this size.
constexpr double kMaxWidth = 320.0;
constexpr std::uint32_t kStageSize = 160;
constexpr std::size_t kMaxTargets = 32;
constexpr std::size_t kMaxChanged = 12;
constexpr std::size_t kMaxLookEntities = 32;
// A measure creeps when its drift grew in each of the last this many checks journaled, and again
// now, though no single check passed its tolerance.
constexpr std::size_t kCreep = 3;

// The components whose fields make a frame's look, kept as a reference's settings.
constexpr std::array<std::string_view, 15> kLookComponents = {"WorldEnvironment",
                                                              "CameraAttributesPractical",
                                                              "DirectionalLight3D",
                                                              "OmniLight3D",
                                                              "SpotLight3D",
                                                              "PointLight2D",
                                                              "DirectionalLight2D",
                                                              "CanvasModulate",
                                                              "Camera3D",
                                                              "Camera2D",
                                                              "ReflectionProbe",
                                                              "LightmapGI",
                                                              "VoxelGI",
                                                              "FogVolume",
                                                              "StandardMaterial3D"};

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value strings(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string text(const json::Value& params, std::string_view name) {
    const json::Value* value = params.find(name);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

bool flag(const json::Value& params, std::string_view name) {
    const json::Value* value = params.find(name);
    return value != nullptr && value->is_bool() && value->as_bool();
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

bool is_material_path(std::string_view path) {
    return path.ends_with(canvas::kMaterialSuffix) ||
           path.ends_with(scene3d::kStandardMaterialSuffix);
}

// What a file is, as style references capture it, or "" for a file they can't.
std::string what_of(std::string_view path) {
    if (path.ends_with(world::kSceneSuffix)) {
        return "scene";
    }
    if (is_material_path(path)) {
        return "material";
    }
    if (scene3d::is_model_path(path)) {
        return "model";
    }
    if (procedural::is_texture_path(path)) {
        return "image";
    }
    return "";
}

// How a frame is captured: what, from which file, through which camera, how big and after how
// many ticks. A reference keeps it so style.check can capture again the same way.
struct Plan {
    std::string what;
    std::string path;
    std::string camera;
    std::optional<std::array<std::uint32_t, 2>> size;
    std::int64_t ticks = 0;
};

json::Value plan_json(const Plan& plan, std::uint32_t width, std::uint32_t height) {
    json::Value out;
    out["what"] = plan.what;
    if (!plan.path.empty()) {
        out["path"] = plan.path;
    }
    if (!plan.camera.empty()) {
        out["camera"] = plan.camera;
    }
    out["size"] = json::Array{static_cast<std::int64_t>(width), static_cast<std::int64_t>(height)};
    if (plan.ticks > 0) {
        out["ticks"] = plan.ticks;
    }
    return out;
}

Plan plan_of(const json::Value& capture) {
    Plan plan;
    plan.what = text(capture, "what");
    plan.path = text(capture, "path");
    plan.camera = text(capture, "camera");
    if (const json::Value* size = capture.find("size"); size != nullptr && size->size() == 2) {
        plan.size =
            std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(size->as_array()[0].as_int()),
                                         static_cast<std::uint32_t>(size->as_array()[1].as_int())};
    }
    if (const json::Value* ticks = capture.find("ticks"); ticks != nullptr && ticks->is_int()) {
        plan.ticks = ticks->as_int();
    }
    return plan;
}

// A captured frame and what made it.
struct Shot {
    render::Image image;
    json::Value settings;
    std::optional<double> texel_density;
};

// What makes the world's look: the look components' fields and the transforms of the entities
// that have them, the material files drawn and their params, and the project's rendering
// settings.
json::Value look_of(Session& session, const world::World& world) {
    json::Value out = json::Object{};
    const json::Value saved = world::save_scene(world);
    json::Value entities = json::Object{};
    json::Value materials = json::Object{};
    std::size_t kept = 0;
    bool truncated = false;
    if (const json::Value* all = saved.find("entities"); all != nullptr && all->is_object()) {
        for (const auto& [id, entity] : all->as_object()) {
            const json::Value* components = entity.find("components");
            if (components == nullptr || !components->is_object()) {
                continue;
            }
            json::Value look = json::Object{};
            for (const auto& [name, fields] : components->as_object()) {
                if (std::ranges::find(kLookComponents, name) != kLookComponents.end()) {
                    look[name] = fields;
                }
                for (const std::string_view field : {"material_override", "material"}) {
                    const json::Value* material = fields.find(field);
                    if (material == nullptr || !material->is_string() ||
                        material->as_string().empty() ||
                        materials.find(material->as_string()) != nullptr) {
                        continue;
                    }
                    const json::Value* params = fields.find("material_params");
                    materials[material->as_string()] =
                        params != nullptr ? *params : json::Value(json::Object{});
                }
            }
            if (look.as_object().empty()) {
                continue;
            }
            if (kept == kMaxLookEntities) {
                truncated = true;
                continue;
            }
            for (const std::string_view transform : {"Transform3D", "Transform2D"}) {
                if (const json::Value* placed = components->find(transform); placed != nullptr) {
                    look[transform] = *placed;
                }
            }
            entities[id] = std::move(look);
            ++kept;
        }
    }
    if (!entities.as_object().empty()) {
        out["entities"] = std::move(entities);
    }
    if (truncated) {
        out["truncated"] = true;
    }
    if (!materials.as_object().empty()) {
        out["materials"] = std::move(materials);
    }
    if (auto project = session.read_project_file("project.json"); project && project.value()) {
        if (auto parsed = json::parse(*project.value()); parsed) {
            if (const json::Value* rendering = parsed.value().find("rendering");
                rendering != nullptr) {
                out["rendering"] = *rendering;
            }
        }
    }
    return out;
}

SessionOptions stage_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    return options;
}

Result<void> called(const Reply& reply, std::string_view what) {
    if (reply.ok) {
        return {};
    }
    return make_error(ErrorCode::InvalidArgument, std::string(what) + ": " + reply.message);
}

// Captures a world through its current camera: at size, or at most kMaxWidth wide.
Result<render::Image> capture_world(world::World& world, asset::Assets& assets,
                                    const std::optional<std::array<std::uint32_t, 2>>& size,
                                    bool stage) {
    canvas::CaptureOptions options;
    if (stage) {
        const auto side = static_cast<float>(size ? (*size)[0] : kStageSize);
        const auto other = static_cast<float>(size ? (*size)[1] : kStageSize);
        options.viewport = Vec2{side, other};
    } else {
        const Vec2 viewport = ui::settings(world).viewport;
        const double width =
            size ? static_cast<double>((*size)[0]) : std::min<double>(kMaxWidth, viewport.x);
        options.scale = static_cast<float>(width / std::max<double>(1.0, viewport.x));
    }
    auto captured = canvas::capture(world, assets, options);
    if (!captured) {
        return captured.error();
    }
    return std::move(captured.value().image);
}

// Euler angles (YXZ, radians) that turn a camera at from to look at to.
Vec3 looking(Vec3 from, Vec3 to) {
    const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    const float across = std::sqrt((d.x * d.x) + (d.z * d.z));
    return {slash::atan2(d.y, across), slash::atan2(-d.x, -d.z), 0.0F};
}

json::Value vec3(Vec3 v) {
    return json::Array{static_cast<double>(v.x), static_cast<double>(v.y),
                       static_cast<double>(v.z)};
}

// A stage for a model or a material: a gray background, a key light and a camera at three
// quarters, the model framed by its bounds or the material on a sphere.
Result<void> build_stage(Session& stage, const Plan& plan, Vec3 center, double radius) {
    // Along (0.6, 0.45, 0.66), near enough for the subject's bounding sphere to fill most of
    // Camera3D's default 75 degree view.
    const auto reach = static_cast<float>(std::max(0.01, radius) * 1.8);
    const Vec3 eye{center.x + (0.6F * reach), center.y + (0.45F * reach),
                   center.z + (0.66F * reach)};
    const std::array<std::pair<std::string, json::Value>, 4> entities = {{
        {"camera",
         json::Object{{"Camera3D", json::Object{{"current", true}}},
                      {"Transform3D", json::Object{{"position", vec3(eye)},
                                                   {"rotation", vec3(looking(eye, center))}}}}},
        {"key",
         json::Object{{"DirectionalLight3D", json::Object{}},
                      {"Transform3D", json::Object{{"rotation", json::Array{-0.8, 0.6, 0.0}}}}}},
        {"environment",
         json::Object{{"WorldEnvironment",
                       json::Object{{"background_mode", "color"},
                                    {"background_color", json::Array{0.5, 0.5, 0.5, 1.0}},
                                    {"ambient_light_source", "color"},
                                    {"ambient_light_color", json::Array{0.4, 0.4, 0.4, 1.0}}}}}},
        {"subject",
         plan.what == "model"
             ? json::Object{{"MeshInstance3D", json::Object{{"mesh", plan.path}}},
                            {"Transform3D", json::Object{}}}
             : json::Object{{"MeshInstance3D", json::Object{{"material_override", plan.path}}},
                            {"SphereMesh", json::Object{}},
                            {"Transform3D", json::Object{}}}},
    }};
    for (const auto& [id, components] : entities) {
        json::Value params;
        params["id"] = id;
        params["components"] = components;
        if (auto made = called(stage.call("entity.create", params), "making the stage"); !made) {
            return made.error();
        }
    }
    return {};
}

Result<std::string> read_file(Session& session, std::string_view path) {
    auto bytes = session.read_project_file(path);
    if (!bytes) {
        return bytes.error();
    }
    if (std::optional<std::string>& got = bytes.value(); got.has_value()) {
        return std::move(*got);
    }
    return make_error(ErrorCode::NotFound, quoted(path) + " doesn't exist");
}

// A canvas material, which draws on a sprite rather than a sphere.
bool is_canvas_material(Session& session, std::string_view path) {
    if (!path.ends_with(canvas::kMaterialSuffix)) {
        return false;
    }
    auto bytes = read_file(session, path);
    if (!bytes) {
        return false;
    }
    auto parsed = json::parse(bytes.value());
    return !parsed || text(parsed.value(), "shader_type") != "spatial";
}

Result<Shot> shoot(Session& host, const Plan& plan) {
    Shot shot;
    if (plan.what == "image") {
        auto bytes = read_file(host, plan.path);
        if (!bytes) {
            return bytes.error();
        }
        auto image = procedural::decode_texture(plan.path, bytes.value());
        if (!image) {
            return image.error();
        }
        shot.image = std::move(image.value());
        return shot;
    }
    Session stage(host.components(), host.commands(), stage_options(host));
    if (plan.what == "scene" || plan.what == "world") {
        json::Value params;
        params["path"] = plan.path;
        if (auto opened = called(stage.call("scene.open", params), "opening " + quoted(plan.path));
            !opened) {
            return opened.error();
        }
        if (!plan.camera.empty() && stage.world().find(plan.camera) != entt::null) {
            const entt::entity camera = stage.world().find(plan.camera);
            const char* type =
                stage.world().ecs().all_of<world::Camera3D>(camera) ? "Camera3D" : "Camera2D";
            json::Value set;
            set["id"] = plan.camera;
            set["components"][type]["current"] = true;
            if (auto made = called(stage.call("component.set", set), "choosing the camera");
                !made) {
                return made.error();
            }
        }
        if (plan.ticks > 0) {
            json::Value step;
            step["ticks"] = plan.ticks;
            if (auto stepped = called(stage.call("step", step), "stepping"); !stepped) {
                return stepped.error();
            }
        }
        auto image = capture_world(stage.world(), stage.assets(), plan.size, false);
        if (!image) {
            return image.error();
        }
        shot.image = std::move(image.value());
        shot.settings = look_of(host, stage.world());
        return shot;
    }
    if (plan.what == "material" && is_canvas_material(host, plan.path)) {
        // A canvas material draws on a white sprite filling the stage.
        render::Image white;
        white.width = 64;
        white.height = 64;
        white.pixels.assign(std::size_t{64} * 64 * 4, 255);
        auto texture = procedural::make_texture(stage.world(), "style_swatch", std::move(white));
        if (!texture) {
            return texture.error();
        }
        const std::uint32_t width = plan.size ? (*plan.size)[0] : kStageSize;
        const std::uint32_t height = plan.size ? (*plan.size)[1] : kStageSize;
        json::Value params;
        params["id"] = "subject";
        params["components"]["Sprite2D"] =
            json::Object{{"texture", texture.value()}, {"material", plan.path}};
        params["components"]["Transform2D"] =
            json::Object{{"position", json::Array{width / 2.0, height / 2.0}},
                         {"scale", json::Array{width / 64.0, height / 64.0}}};
        if (auto made = called(stage.call("entity.create", params), "making the stage"); !made) {
            return made.error();
        }
    } else {
        // A material's sphere is SphereMesh's default, half a meter round.
        Vec3 center;
        double radius = 0.5;
        if (plan.what == "model") {
            auto model = host.assets().load<scene3d::Model>(plan.path);
            if (!model) {
                return model.error();
            }
            const scene3d::Bounds bounds = scene3d::bounds(*model.value());
            center = {(bounds.min.x + bounds.max.x) / 2, (bounds.min.y + bounds.max.y) / 2,
                      (bounds.min.z + bounds.max.z) / 2};
            const Vec3 extent{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                              bounds.max.z - bounds.min.z};
            radius =
                std::sqrt((extent.x * extent.x) + (extent.y * extent.y) + (extent.z * extent.z)) /
                2;
            const auto read = [&host](std::string_view name) { return read_file(host, name); };
            if (auto geometry = scene3d::check_geometry(*model.value(), read); geometry) {
                shot.texel_density = geometry.value().texel_density;
            }
        }
        if (auto built = build_stage(stage, plan, center, radius); !built) {
            return built.error();
        }
    }
    auto image = capture_world(stage.world(), stage.assets(), plan.size, true);
    if (!image) {
        return image.error();
    }
    shot.image = std::move(image.value());
    shot.settings = look_of(host, stage.world());
    // The stage's own camera, light and environment are the same for every model.
    shot.settings.as_object().erase("entities");
    return shot;
}

storyboard::Fingerprint measure(const Shot& shot) {
    storyboard::Fingerprint out =
        storyboard::fingerprint(shot.image.pixels, shot.image.width, shot.image.height);
    out.texel_density = shot.texel_density;
    return out;
}

Result<json::Object> load_entries(Session& session) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return loaded.error();
    }
    if (!loaded.value().errors.empty()) {
        const storyboard::FileError& first = loaded.value().errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          first.file + (first.path.empty() ? "" : " at " + first.path) + ": " +
                              first.message + "; fix the storyboard file, then retry");
    }
    return std::move(loaded.value().entries);
}

// ---- style.mark ----

Reply style_mark(Session& session, const json::Value& params) {
    const std::string name = text(params, "name");
    const std::string id = std::string(kKind) + name;
    if (const std::string problem = storyboard::id_error(id); !problem.empty()) {
        return param_error(code::kInvalidParams, "/name", problem);
    }
    Plan plan;
    std::string at;
    for (const std::string_view what : {"scene", "image", "model", "material"}) {
        if (const std::string path = text(params, what); !path.empty()) {
            if (!plan.what.empty()) {
                return param_error(code::kInvalidParams, "/" + std::string(what),
                                   "mark one thing at a time; this also gives " + plan.what);
            }
            plan.what = what;
            plan.path = path;
            at = "/" + std::string(what);
        }
    }
    if (!plan.what.empty() && what_of(plan.path) != plan.what &&
        !(plan.what == "image" && what_of(plan.path).empty())) {
        return param_error(
            code::kInvalidParams, at,
            quoted(plan.path) + " isn't a " + plan.what + " file" +
                (what_of(plan.path).empty() ? "" : "; mark it as \"" + what_of(plan.path) + "\""));
    }
    plan.camera = text(params, "camera");
    if (const json::Value* size = params.find("size"); size != nullptr) {
        plan.size =
            std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(size->as_array()[0].as_int()),
                                         static_cast<std::uint32_t>(size->as_array()[1].as_int())};
    }
    if (const json::Value* ticks = params.find("ticks"); ticks != nullptr) {
        plan.ticks = ticks->as_int();
    }

    Shot shot;
    if (plan.what.empty()) {
        // The open world, as it looks now; style.check opens its scene again at this tick.
        if (!plan.camera.empty()) {
            return param_error(code::kInvalidParams, "/camera",
                               "the open world is seen through its current camera; give "
                               "\"scene\" to mark a scene file through another");
        }
        if (plan.ticks > 0) {
            return param_error(code::kInvalidParams, "/ticks",
                               "the open world is marked as it is now; step it first");
        }
        plan.what = "world";
        plan.path = session.scene_path();
        plan.ticks = static_cast<std::int64_t>(session.world().tick());
        auto image = capture_world(session.world(), session.assets(), plan.size, false);
        if (!image) {
            return Reply::failure(image.error());
        }
        shot.image = std::move(image.value());
        shot.settings = look_of(session, session.world());
    } else {
        auto taken = shoot(session, plan);
        if (!taken) {
            return param_error(code::kInvalidArgument, at, taken.error().message);
        }
        shot = std::move(taken.value());
    }
    if (shot.image.width == 0 || shot.image.height == 0) {
        return param_error(code::kInvalidArgument, at, "the frame has no pixels");
    }
    const storyboard::Fingerprint fingerprint = measure(shot);

    // The frame, kept beside the storyboard so a vision model can compare against it later.
    const std::string frame = std::string(kFrames) + name + ".png";
    auto png = render::encode_png(shot.image);
    if (!png) {
        return Reply::failure(png.error());
    }
    auto before = session.read_project_file(frame);
    if (!before) {
        return Reply::failure(before.error());
    }
    if (auto staged = session.stage_file(frame, png.value()); !staged) {
        return Reply::failure(staged.error());
    }
    session.add_undo_file(frame, before.value(), png.value());

    auto entries = load_entries(session);
    if (!entries) {
        return Reply::failure(entries.error());
    }
    json::Value set;
    json::Value capture = plan_json(plan, shot.image.width, shot.image.height);
    capture["frame"] = frame;
    set["capture"] = std::move(capture);
    set["settings"] = std::move(shot.settings);
    set["fingerprint"] = storyboard::to_json(fingerprint);
    for (const std::string_view field : {"summary", "text", "tolerances"}) {
        if (const json::Value* value = params.find(field); value != nullptr) {
            set[field] = *value;
        }
    }
    if (!entries.value().contains(id)) {
        set["name"] = name;
    }
    if (!entries.value().contains(id) && params.find("summary") == nullptr) {
        set["summary"] = "The look of " +
                         (plan.path.empty() ? std::string("the open world") : quoted(plan.path)) +
                         ".";
    }
    // What it governs: its own file and the paths given.
    json::Array links;
    if (!plan.path.empty()) {
        links.emplace_back(plan.path);
    }
    if (const json::Value* more = params.find("links"); more != nullptr) {
        for (const json::Value& link : more->as_array()) {
            if (std::ranges::find(links, link) == links.end()) {
                links.push_back(link);
            }
        }
    }
    if (!links.empty()) {
        set["links"] = std::move(links);
    }
    json::Value edit;
    edit["id"] = id;
    const auto existing = entries.value().find(id);
    edit["version"] = existing != entries.value().end()
                          ? json::Value(storyboard::version(existing->second))
                          : json::Value();
    edit["set"] = std::move(set);
    json::Value call;
    call["edits"] = json::Array{std::move(edit)};
    call["why"] = text(params, "why").empty() ? std::string("style.mark") : text(params, "why");
    Reply edited = session.call("storyboard.edit", call);
    if (!edited.ok) {
        return edited;
    }
    json::Value result;
    result["id"] = id;
    result["frame"] = frame;
    result["fingerprint"] = storyboard::to_json(fingerprint);
    return Reply::success(std::move(result));
}

// ---- style.check ----

// Every leaf of a settings object by its JSON Pointer, arrays whole.
void flatten(const json::Value& value, const std::string& at,
             std::map<std::string, std::string>& out) {
    if (value.is_object()) {
        for (const auto& [key, member] : value.as_object()) {
            flatten(member, at + "/" + json::pointer_escape(key), out);
        }
        return;
    }
    out[at] = json::write(value, json::Style::Compact);
}

// The entity a settings pointer is under ("/entities/<id>/..."), or "".
std::string_view entity_of(std::string_view pointer) {
    constexpr std::string_view kEntities = "/entities/";
    if (!pointer.starts_with(kEntities)) {
        return {};
    }
    pointer.remove_prefix(kEntities.size());
    return pointer.substr(0, pointer.find('/'));
}

// The settings that differ between a reference and now, by JSON Pointer. A file other than the
// reference's own has entities of its own, so only those both have are compared.
json::Array changed_settings(const json::Value& reference, const json::Value& now, bool same_file) {
    std::map<std::string, std::string> was;
    std::map<std::string, std::string> is;
    flatten(reference, "", was);
    flatten(now, "", is);
    const auto shared = [&](std::string_view pointer) {
        const std::string_view entity = entity_of(pointer);
        if (same_file || entity.empty()) {
            return true;
        }
        const json::Value* a = reference.find("entities");
        const json::Value* b = now.find("entities");
        std::string id;
        for (std::size_t i = 0; i < entity.size(); ++i) {
            if (entity[i] == '~' && i + 1 < entity.size()) {
                id += entity[++i] == '1' ? '/' : '~';
            } else {
                id += entity[i];
            }
        }
        return a != nullptr && b != nullptr && a->find(id) != nullptr && b->find(id) != nullptr;
    };
    std::set<std::string> pointers;
    for (const auto& [pointer, value] : was) {
        if (const auto it = is.find(pointer);
            (it == is.end() || it->second != value) && shared(pointer)) {
            pointers.insert(pointer);
        }
    }
    for (const auto& [pointer, value] : is) {
        if (!was.contains(pointer) && shared(pointer)) {
            pointers.insert(pointer);
        }
    }
    json::Array out;
    for (const std::string& pointer : pointers) {
        if (out.size() == kMaxChanged) {
            out.emplace_back("...");
            break;
        }
        out.emplace_back(pointer);
    }
    return out;
}

struct Reference {
    std::string id;
    const json::Value* entry = nullptr;
    Plan plan;
    storyboard::Fingerprint fingerprint;
};

double tolerance_of(const Reference& reference, std::string_view measure) {
    if (const json::Value* tolerances = reference.entry->find("tolerances");
        tolerances != nullptr) {
        if (const json::Value* given = tolerances->find(measure);
            given != nullptr && given->is_number()) {
            return given->as_double();
        }
    }
    return storyboard::default_tolerance(measure);
}

// The journaled fingerprints of a reference's target, oldest first.
std::vector<storyboard::Fingerprint> journaled(const std::string& journal, const std::string& id,
                                               const std::string& target) {
    std::vector<storyboard::Fingerprint> out;
    std::size_t start = 0;
    while (start < journal.size()) {
        std::size_t end = journal.find('\n', start);
        if (end == std::string::npos) {
            end = journal.size();
        }
        if (auto line = json::parse(std::string_view(journal).substr(start, end - start)); line) {
            if (text(line.value(), "reference") == id && text(line.value(), "target") == target) {
                if (const json::Value* kept = line.value().find("fingerprint"); kept != nullptr) {
                    if (auto fingerprint = storyboard::fingerprint_from_json(*kept); fingerprint) {
                        out.push_back(std::move(fingerprint.value()));
                    }
                }
            }
        }
        start = end + 1;
    }
    return out;
}

// Measures whose drift rose in each of the last kCreep journaled checks and again now.
json::Value creeping(const Reference& reference, const std::vector<storyboard::Fingerprint>& past,
                     const json::Object& now) {
    json::Value out = json::Object{};
    if (past.size() < kCreep) {
        return out;
    }
    std::vector<json::Object> drifts;
    for (std::size_t i = past.size() - kCreep; i < past.size(); ++i) {
        drifts.push_back(storyboard::drift(reference.fingerprint, past[i]));
    }
    drifts.push_back(now);
    for (const std::string_view measure : storyboard::fingerprint_measures()) {
        json::Array series;
        bool rising = true;
        double last = -1.0;
        for (const json::Object& drift : drifts) {
            const auto it = drift.find(measure);
            if (it == drift.end()) {
                rising = false;
                break;
            }
            const double value = it->second.as_double();
            rising = rising && value > last;
            last = value;
            series.emplace_back(value);
        }
        if (rising && last > 0.0) {
            out[measure] = std::move(series);
        }
    }
    return out;
}

std::string file_name(std::string_view path) {
    std::string out;
    for (const char c : path) {
        out += (c == '/' || c == '.') ? '_' : c;
    }
    return out;
}

Reply style_check(Session& session, const json::Value& params) {
    const bool record = flag(params, "record");
    const bool review = flag(params, "review");
    if ((record || review) && !session.allowed(Capability::Filesystem)) {
        return param_error(code::kPermissionDenied, record ? "/record" : "/review",
                           "writing the journal or the frames to review needs the "
                           "\"filesystem\" capability, which this session does not grant");
    }
    if (review && session.commands().find("storyboard.review") == nullptr) {
        return param_error(code::kInvalidParams, "/review",
                           "storyboard.review isn't registered here; slash-cli and slash-mcp "
                           "have it");
    }
    auto loaded = load_entries(session);
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    const json::Object& entries = loaded.value();

    // The references to check against.
    std::vector<Reference> references;
    const json::Value* ids = params.find("references");
    const auto add_reference = [&](const std::string& id, const json::Value& entry) {
        Reference reference;
        reference.id = id;
        reference.entry = &entry;
        if (const json::Value* capture = entry.find("capture"); capture != nullptr) {
            reference.plan = plan_of(*capture);
        }
        if (const json::Value* kept = entry.find("fingerprint"); kept != nullptr) {
            if (auto fingerprint = storyboard::fingerprint_from_json(*kept); fingerprint) {
                reference.fingerprint = std::move(fingerprint.value());
            }
        }
        references.push_back(std::move(reference));
    };
    if (ids != nullptr) {
        for (std::size_t i = 0; i < ids->size(); ++i) {
            const std::string& id = ids->as_array()[i].as_string();
            const auto it = entries.find(id);
            if (!id.starts_with(kKind) || it == entries.end()) {
                return param_error(code::kNotFound, "/references/" + std::to_string(i),
                                   it == entries.end() ? storyboard::unknown_entry(entries, id)
                                                       : quoted(id) + " isn't a style entry");
            }
            add_reference(id, it->second);
        }
    } else {
        for (const auto& [id, entry] : entries) {
            if (id.starts_with(kKind) && entry.find("fingerprint") != nullptr) {
                add_reference(id, entry);
            }
        }
    }

    // What each reference governs: the paths given, or its own file and the files of its kind
    // its links cover.
    std::vector<std::string> files;
    const json::Value* given = params.find("paths");
    if (given == nullptr && !references.empty()) {
        auto listed =
            platform::list_files(session.options().project_root, [](std::string_view folder) {
                return folder != "build/" && folder != "storyboard/";
            });
        if (!listed) {
            return Reply::failure(listed.error());
        }
        files = std::move(listed.value());
    }
    std::vector<std::pair<const Reference*, std::string>> targets;
    for (const Reference& reference : references) {
        std::vector<std::string> own;
        if (given != nullptr) {
            for (const json::Value& path : given->as_array()) {
                own.push_back(path.as_string());
            }
        } else {
            if (!reference.plan.path.empty()) {
                own.push_back(reference.plan.path);
            }
            const std::string kind =
                reference.plan.what == "world" ? std::string("scene") : reference.plan.what;
            const json::Value* links = reference.entry->find("links");
            for (const std::string& file : files) {
                if (links == nullptr || what_of(file) != kind) {
                    continue;
                }
                const bool covered = std::ranges::any_of(links->as_array(), [&](const auto& link) {
                    const std::string& folder = link.as_string();
                    return file.starts_with(folder) &&
                           (file.size() == folder.size() || folder.ends_with('/') ||
                            file[folder.size()] == '/');
                });
                if (covered && std::ranges::find(own, file) == own.end()) {
                    own.push_back(file);
                }
            }
        }
        for (std::string& path : own) {
            targets.emplace_back(&reference, std::move(path));
        }
    }
    if (targets.size() > kMaxTargets) {
        return param_error(code::kInvalidParams, given != nullptr ? "/paths" : "/references",
                           std::to_string(targets.size()) + " frames to capture; check at most " +
                               std::to_string(kMaxTargets) + " at a time");
    }
    if (given != nullptr) {
        for (std::size_t i = 0; i < given->size(); ++i) {
            const std::string& path = given->as_array()[i].as_string();
            if (what_of(path).empty()) {
                return param_error(code::kInvalidParams, "/paths/" + std::to_string(i),
                                   quoted(path) + " isn't a scene, image, model or material");
            }
        }
    }

    std::string journal;
    if (auto kept = session.read_project_file(kStyleJournal); kept) {
        if (std::optional<std::string>& got = kept.value(); got.has_value()) {
            journal = std::move(*got);
        }
    }
    std::string appended;
    json::Array checked;
    std::int64_t drifted = 0;
    for (const auto& [reference_of, target] : targets) {
        const Reference& reference = *reference_of;
        json::Value item;
        item["reference"] = reference.id;
        item["target"] = target;
        Plan plan = reference.plan;
        plan.what = what_of(target);
        plan.path = target;
        if (plan.what != "scene") {
            plan.camera.clear();
            plan.ticks = 0;
        }
        // A scene checked against a world's reference opens at the tick the world was marked at.
        auto shot = shoot(session, plan);
        if (!shot) {
            item["error"] = shot.error().message;
            ++drifted;
            checked.push_back(std::move(item));
            continue;
        }
        const storyboard::Fingerprint now = measure(shot.value());
        const json::Object drift = storyboard::drift(reference.fingerprint, now);
        json::Value over = json::Object{};
        for (const auto& [measure, value] : drift) {
            const double tolerance = tolerance_of(reference, measure);
            if (value.as_double() > tolerance) {
                over[measure] = storyboard::describe_measure(reference.fingerprint, measure) +
                                " -> " + storyboard::describe_measure(now, measure) + " (drift " +
                                json::write(value) + ", tolerance " +
                                json::write(json::Value(tolerance)) + ")";
            }
        }
        item["drift"] = drift;
        if (!over.as_object().empty()) {
            item["over"] = std::move(over);
            ++drifted;
        }
        if (const json::Value* settings = reference.entry->find("settings"); settings != nullptr) {
            if (json::Array changed = changed_settings(*settings, shot.value().settings,
                                                       target == reference.plan.path);
                !changed.empty()) {
                item["changed"] = std::move(changed);
            }
        }
        if (json::Value creep =
                creeping(reference, journaled(journal, reference.id, target), drift);
            !creep.as_object().empty()) {
            item["creeping"] = std::move(creep);
        }
        if (review) {
            // The model sees the reference's frame, then the target's, against the entry.
            const std::string frame = std::string(kReviewFrames) +
                                      reference.id.substr(kKind.size()) + "/" + file_name(target) +
                                      ".png";
            auto png = render::encode_png(shot.value().image);
            auto full = session.resolve_path(frame);
            if (!png || !full) {
                item["review"] = json::Object{{"error", "the frame couldn't be written"}};
            } else if (auto written = session.write_file(full.value(), png.value()); !written) {
                item["review"] = json::Object{{"error", written.error().message}};
            } else {
                json::Value ask;
                json::Array paths;
                if (const json::Value* capture = reference.entry->find("capture");
                    capture != nullptr && !text(*capture, "frame").empty()) {
                    paths.emplace_back(text(*capture, "frame"));
                }
                paths.emplace_back(frame);
                ask["paths"] = std::move(paths);
                // Naming the entry makes it one of the facts the model reads.
                const json::Value* named = reference.entry->find("name");
                ask["text"] =
                    "The last image is " + target + " now; the first is the style " + "reference " +
                    reference.id + " (" +
                    (named != nullptr && named->is_string() ? named->as_string() : reference.id) +
                    "). Name where the last strays from the reference's look.";
                Reply reviewed = session.call("storyboard.review", ask);
                item["review"] = reviewed.ok
                                     ? std::move(reviewed.value)
                                     : json::Value(json::Object{{"error", reviewed.message}});
            }
        }
        if (record) {
            json::Value line;
            line["time"] = storyboard::format_time(static_cast<std::int64_t>(std::time(nullptr)));
            line["fingerprint"] = storyboard::to_json(now);
            line["reference"] = reference.id;
            line["target"] = target;
            appended += json::write(line, json::Style::Compact) + "\n";
        }
        checked.push_back(std::move(item));
    }
    if (record && !appended.empty()) {
        auto full = session.resolve_path(kStyleJournal);
        if (!full) {
            return Reply::failure(full.error());
        }
        if (auto written = session.write_file(full.value(), journal + appended); !written) {
            return Reply::failure(written.error());
        }
    }
    json::Value result;
    result["checked"] = std::move(checked);
    result["drifted"] = drifted;
    return Reply::success(std::move(result));
}

} // namespace

void register_style_commands(Registry& registry) {
    json::Value size = property("array", "The frame's [width, height] in pixels. Default: the "
                                         "window's shape at most 320 wide, or 160 square for "
                                         "models and materials.");
    size["items"]["type"] = "integer";
    size["items"]["minimum"] = 1;
    size["items"]["maximum"] = 4096;
    size["minItems"] = 2;
    size["maxItems"] = 2;
    json::Value ticks = property("integer", "Ticks to step a scene before capturing it.");
    ticks["minimum"] = 0;
    ticks["maximum"] = 36000;
    json::Value name = property("string", "The reference's name: it becomes style/<name>, its "
                                          "frame storyboard/style/<name>.png.");
    json::Value mark;
    mark["type"] = "object";
    mark["additionalProperties"] = false;
    mark["required"] = json::Array{"name"};
    mark["properties"] = json::Object{
        {"name", std::move(name)},
        {"scene", property("string", "A scene file to mark, captured through its current "
                                     "camera. Give none of scene, image, model and material to "
                                     "mark the open world as it is now.")},
        {"image", property("string", "An image or texture file to mark.")},
        {"model", property("string", "A model to mark, drawn on a gray stage at three quarters.")},
        {"material", property("string", "A material to mark: a 3D one on a sphere, a canvas one "
                                        "on a sprite.")},
        {"camera", property("string", "The camera to see a scene through, by entity ID.")},
        {"size", size},
        {"ticks", ticks},
        {"summary", property("string", "One line on the look.")},
        {"text", property("string", "What to keep about it, in words, for agents and models.")},
        {"links", strings("More files and folders it governs, which style.check holds to it.")},
        {"tolerances", storyboard::tolerances_schema()},
        {"why", property("string", "Why, for the storyboard's journal.")},
    };
    registry.add({
        .name = "style.mark",
        .description =
            "Keeps the look of a scene, the open world, an image, a model or a material as the "
            "storyboard entry style/<name>: its frame, what made it and a fingerprint (palette, "
            "value, contrast, saturation, warmth, edges, line weight, flatness, texel density).",
        .capabilities = {Capability::Edit, Capability::Filesystem},
        .effect = Effect::Compound,
        .params = std::move(mark),
        .handler = style_mark,
    });

    json::Value check;
    check["type"] = "object";
    check["additionalProperties"] = false;
    check["properties"] = json::Object{
        {"references", strings("Style entries to check against. Default: every one.")},
        {"paths", strings("Scenes, images, models and materials to check against the "
                          "references. Default: each reference's own file and the files of its "
                          "kind its links cover.")},
        {"record", property("boolean", "Add each fingerprint to storyboard/"
                                       "style_journal.jsonl, whose history shows slow drift.")},
        {"review", property("boolean", "Also ask a vision model through storyboard.review "
                                       "where each frame strays from its reference's.")},
    };
    registry.add({
        .name = "style.check",
        .description =
            "Captures what style references govern as each was captured and reports drift from "
            "it: each measure's drift, those over tolerance, the settings that changed and "
            "measures creeping up check after check.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(check),
        .handler = style_check,
    });
}

} // namespace slash::command
