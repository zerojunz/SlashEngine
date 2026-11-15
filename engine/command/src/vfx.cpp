// vfx.play, vfx.state, vfx.check and vfx.preview, over .vfx.json recipes
// (slash/world/vfx.hpp): play one where it is wanted, read what is playing, check recipes
// against their budgets and run one on a stage of its own, sampled as numbers.
#include "slash/world/vfx.hpp"

#include "procedural.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/particles.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value point_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = 2;
    schema["maxItems"] = 3;
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply unknown_id(const world::World& world, std::string_view id, const std::string& path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kInvalidParams, path, message);
}

double rounded(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json::Value cost_value(const world::VfxCost& cost) {
    json::Value out;
    out["particles"] = cost.particles;
    out["lights"] = cost.lights;
    out["sounds"] = cost.sounds;
    out["decals"] = cost.decals;
    out["layers"] = cost.layers;
    return out;
}

// The file's name without its folder and suffix: "fx/explosion.vfx.json" is "explosion".
std::string stem_of(std::string_view path) {
    if (const std::size_t slash = path.rfind('/'); slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    if (path.ends_with(world::kVfxSuffix)) {
        path.remove_suffix(world::kVfxSuffix.size());
    }
    std::string out;
    for (const char c : path) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(word ? c : '_');
    }
    return out.empty() ? "vfx" : out;
}

// The first of name, name_2, name_3... no entity has.
std::string free_id(const world::World& world, const std::string& name) {
    if (world.find(name) == entt::null) {
        return name;
    }
    for (int n = 2;; ++n) {
        std::string id = name + "_" + std::to_string(n);
        if (world.find(id) == entt::null) {
            return id;
        }
    }
}

// The recipe at a path, or the reply to fail with.
Result<std::shared_ptr<const world::VfxRecipe>> recipe_at(Session& session,
                                                          const std::string& path) {
    auto text = session.read_project_file(path);
    if (!text) {
        return text.error();
    }
    if (!text.value()) {
        return make_error(ErrorCode::NotFound, quoted(path) + " doesn't exist");
    }
    return world::load_vfx(session.world(), path);
}

// ---- vfx.play ----

Reply vfx_play(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto& ecs = world.ecs();
    const std::string& path = params.find("recipe")->as_string();
    auto loaded = recipe_at(session, path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    const world::VfxRecipe& recipe = *loaded.value();
    const bool flat = recipe.dimension == "2d";
    entt::entity parent = entt::null;
    if (const json::Value* given = params.find("parent"); given != nullptr) {
        parent = world.find(given->as_string());
        if (parent == entt::null) {
            return unknown_id(world, given->as_string(), "/parent");
        }
        if (flat ? !ecs.all_of<world::Transform2D>(parent)
                 : !ecs.all_of<world::Transform3D>(parent)) {
            return param_error(code::kInvalidParams, "/parent",
                               quoted(given->as_string()) + " has no " +
                                   (flat ? "Transform2D" : "Transform3D") + " for a " +
                                   recipe.dimension + " effect to go under");
        }
    }
    const json::Value* bone = params.find("bone");
    if (bone != nullptr && (flat || parent == entt::null)) {
        return param_error(code::kInvalidParams, "/bone",
                           "a bone needs a parent that draws a 3D model to follow");
    }
    const json::Value* at = params.find("at");
    if (at != nullptr && at->as_array().size() != (flat ? 2U : 3U)) {
        return param_error(code::kInvalidParams, "/at",
                           std::string("a ") + recipe.dimension + " effect is placed at " +
                               (flat ? "[x, y] pixels" : "[x, y, z] meters"));
    }
    json::Value own = json::Object{};
    if (const json::Value* given = params.find("parameters"); given != nullptr) {
        for (const auto& [name, value] : given->as_object()) {
            if (recipe.parameters.find(name) == nullptr) {
                std::string message = quoted(name) + " isn't one of the recipe's parameters";
                std::vector<std::string> names;
                for (const auto& [known, _] : recipe.parameters.as_object()) {
                    names.push_back(known);
                }
                if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, "/parameters/" + name, message);
            }
            own[name] = value;
        }
    }
    std::string id;
    if (const json::Value* given = params.find("id"); given != nullptr) {
        id = given->as_string();
        if (world.find(id) != entt::null) {
            return param_error(code::kInvalidParams, "/id", quoted(id) + " is taken");
        }
    } else {
        id = free_id(world, stem_of(path));
    }
    auto made = world.create(id);
    if (!made) {
        return param_error(code::kInvalidParams, "/id", made.error().message);
    }
    const entt::entity entity = made.value();
    const bool free_on_finish =
        params.find("free_on_finish") == nullptr || params.find("free_on_finish")->as_bool();
    const int seed =
        params.find("seed") != nullptr ? static_cast<int>(params.find("seed")->as_int()) : 0;
    if (flat) {
        world::Transform2D place;
        if (at != nullptr) {
            place.position = {static_cast<real_t>(at->as_array()[0].as_double()),
                              static_cast<real_t>(at->as_array()[1].as_double())};
        }
        ecs.emplace<world::Transform2D>(entity, place);
        world::VisualEffect2D effect;
        effect.recipe = path;
        effect.parameters = std::move(own);
        effect.seed = seed;
        effect.free_on_finish = free_on_finish;
        ecs.emplace<world::VisualEffect2D>(entity, std::move(effect));
        if (parent != entt::null) {
            ecs.get<world::Transform2D>(parent).children.push_back(entity);
        }
    } else {
        world::Transform3D place;
        if (at != nullptr) {
            place.position = {static_cast<real_t>(at->as_array()[0].as_double()),
                              static_cast<real_t>(at->as_array()[1].as_double()),
                              static_cast<real_t>(at->as_array()[2].as_double())};
        }
        ecs.emplace<world::Transform3D>(entity, place);
        world::VisualEffect3D effect;
        effect.recipe = path;
        effect.parameters = std::move(own);
        effect.seed = seed;
        effect.free_on_finish = free_on_finish;
        ecs.emplace<world::VisualEffect3D>(entity, std::move(effect));
        if (bone != nullptr) {
            world::BoneAttachment3D attachment;
            attachment.bone_name = bone->as_string();
            ecs.emplace<world::BoneAttachment3D>(entity, attachment);
        }
        if (parent != entt::null) {
            ecs.get<world::Transform3D>(parent).children.push_back(entity);
        }
    }
    json::Value result;
    result["id"] = id;
    result["dimension"] = recipe.dimension;
    result["duration"] = rounded(recipe.duration);
    if (recipe.loop) {
        result["loop"] = true;
    }
    result["layers"] = recipe.layers.size();
    return Reply::success(std::move(result));
}

// ---- vfx.state ----

struct EffectView {
    std::string recipe;
    float time = 0.0F;
    bool playing = false;
    float lod_bias = 1.0F;
    bool flat = false;
};

json::Value effect_state(const world::World& world, entt::entity entity, const EffectView& view) {
    json::Value out;
    out["recipe"] = view.recipe;
    out["time"] = rounded(view.time);
    out["playing"] = view.playing;
    auto loaded = world::load_vfx(world, view.recipe);
    if (!loaded) {
        out["error"] = loaded.error().message;
        return out;
    }
    const world::VfxRecipe& recipe = *loaded.value();
    out["finished"] = !recipe.loop && view.time >= recipe.duration;
    // The detail it plays at, as the system works it out from the current camera.
    float distance = 0.0F;
    if (view.flat) {
        const world::Tree2D tree(world);
        const entt::entity camera = world::current_camera(world, tree);
        if (camera != entt::null) {
            const Vec2 d = tree.global(entity).position - tree.global(camera).position;
            distance = to_float(std::sqrt((d.x * d.x) + (d.y * d.y)));
        }
    } else {
        const world::Tree3D tree(world);
        entt::entity camera = entt::null;
        for (const auto [candidate, c] : world.ecs().view<const world::Camera3D>().each()) {
            if (c.current) {
                camera = candidate;
                break;
            }
        }
        if (camera != entt::null) {
            const Vec3 d = tree.global(entity).origin - tree.global(camera).origin;
            distance = to_float(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)));
        }
    }
    out["detail"] = rounded(world::vfx_detail(recipe, distance / std::max(view.lod_bias, 0.001F)));
    const std::string_view id = world.id_of(entity);
    json::Value playing = json::Object{};
    int lingering = 0;
    for (const auto [part_entity, part] : world.ecs().view<const world::VfxPart>().each()) {
        if (part.effect != id) {
            continue;
        }
        if (part.linger >= 0.0F) {
            ++lingering;
        } else {
            playing[part.layer] = world.id_of(part_entity);
        }
    }
    out["layers"] = std::move(playing);
    if (lingering > 0) {
        out["lingering"] = lingering;
    }
    return out;
}

Reply vfx_state(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const auto& ecs = world.ecs();
    std::vector<std::string> wanted;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        for (std::size_t i = 0; i < ids->as_array().size(); ++i) {
            const std::string& id = ids->as_array()[i].as_string();
            if (world.find(id) == entt::null) {
                return unknown_id(world, id, "/ids/" + std::to_string(i));
            }
            wanted.push_back(id);
        }
    }
    const auto included = [&](std::string_view id) {
        return wanted.empty() || std::ranges::find(wanted, id) != wanted.end();
    };
    json::Value effects = json::Object{};
    for (const auto [entity, e] : ecs.view<const world::VisualEffect3D>().each()) {
        const std::string id(world.id_of(entity));
        if (!id.empty() && included(id)) {
            effects[id] = effect_state(world, entity,
                                       {.recipe = e.recipe,
                                        .time = e.time,
                                        .playing = e.playing,
                                        .lod_bias = e.lod_bias,
                                        .flat = false});
        }
    }
    for (const auto [entity, e] : ecs.view<const world::VisualEffect2D>().each()) {
        const std::string id(world.id_of(entity));
        if (!id.empty() && included(id)) {
            effects[id] = effect_state(world, entity,
                                       {.recipe = e.recipe,
                                        .time = e.time,
                                        .playing = e.playing,
                                        .lod_bias = e.lod_bias,
                                        .flat = true});
        }
    }
    for (const std::string& id : wanted) {
        if (effects.find(id) == nullptr) {
            return param_error(code::kInvalidParams, "/ids",
                               quoted(id) + " has no VisualEffect3D or VisualEffect2D");
        }
    }
    const world::CameraShakeOffsets shake = world::camera_shake_offsets(world);
    json::Value result;
    result["effects"] = std::move(effects);
    if (shake.camera_3d.x != 0.0F || shake.camera_3d.y != 0.0F) {
        result["shake_3d"] = json::Array{rounded(shake.camera_3d.x), rounded(shake.camera_3d.y)};
    }
    if (shake.camera_2d.x != 0.0F || shake.camera_2d.y != 0.0F) {
        result["shake_2d"] = json::Array{rounded(shake.camera_2d.x), rounded(shake.camera_2d.y)};
    }
    return Reply::success(std::move(result));
}

// ---- vfx.check ----

// What a recipe's layers name that the world doesn't know, and what can never play.
std::vector<std::string> recipe_warnings(const world::World& world, const world::VfxRecipe& recipe,
                                         const json::Value& parameters) {
    std::vector<std::string> warnings;
    float most = recipe.lod.empty() ? 1.0F : 0.0F;
    if (!recipe.lod.empty() && recipe.lod.front().distance > 0.0F) {
        most = 1.0F;
    }
    for (const world::VfxLod& level : recipe.lod) {
        most = std::max(most, level.detail);
    }
    for (const world::VfxLayer& layer : recipe.layers) {
        const std::string name = "layer " + quoted(layer.name);
        if (layer.start >= recipe.duration) {
            warnings.push_back(name + " starts at or after the effect ends, so it never plays");
        }
        if (layer.detail > most) {
            warnings.push_back(name + " needs more detail than any level of detail plays at");
        }
        std::set<std::string> components;
        if (layer.components.is_object()) {
            for (const auto& [component, _] : layer.components.as_object()) {
                components.insert(component);
                if (world.components().find(component) == nullptr) {
                    std::string message = name + " names no component " + quoted(component);
                    std::vector<std::string> known;
                    for (const auto& type : world.components().types()) {
                        known.push_back(type->name());
                    }
                    if (const std::string_view hint = closest_match(component, known);
                        !hint.empty()) {
                        message += "; did you mean " + quoted(hint) + "?";
                    }
                    warnings.push_back(std::move(message));
                }
            }
        }
        for (const world::VfxCurve& curve : layer.curves) {
            const std::string component = curve.path.substr(0, curve.path.find('/'));
            if (!components.contains(component)) {
                warnings.push_back(name + "'s curve " + quoted(curve.path) +
                                   " moves a component the layer doesn't have");
            }
        }
        if (auto worked = world::vfx_components(layer, parameters, 1.0F); !worked) {
            warnings.push_back(name + ": " + worked.error().message);
        }
    }
    return warnings;
}

json::Value recipe_check(Session& session, const std::string& path, const json::Value& own) {
    json::Value out;
    auto text = session.read_project_file(path);
    if (!text) {
        out["errors"] = json::Array{text.error().message};
        return out;
    }
    const std::string body = text.value().value_or(std::string{});
    if (!text.value().has_value()) {
        out["errors"] = json::Array{quoted(path) + " doesn't exist"};
        return out;
    }
    auto doc = json::parse(body);
    if (!doc) {
        out["errors"] = json::Array{doc.error().message};
        return out;
    }
    if (const auto problems = world::validate_vfx(doc.value()); !problems.empty()) {
        json::Array errors;
        for (const auto& problem : problems) {
            errors.emplace_back(problem.path + ": " + problem.message);
        }
        out["errors"] = std::move(errors);
        return out;
    }
    auto parsed = world::parse_vfx(doc.value());
    if (!parsed) {
        out["errors"] = json::Array{parsed.error().message};
        return out;
    }
    const world::VfxRecipe& recipe = parsed.value();
    const json::Value parameters = world::vfx_parameters(recipe, own);
    out["dimension"] = recipe.dimension;
    out["duration"] = rounded(recipe.duration);
    if (recipe.loop) {
        out["loop"] = true;
    }
    out["layers"] = recipe.layers.size();
    auto cost = world::vfx_cost(recipe, parameters, 1.0F);
    if (!cost) {
        out["errors"] = json::Array{cost.error().message};
        return out;
    }
    out["cost"] = cost_value(cost.value());
    if (const auto over = world::vfx_over_budget(recipe, cost.value()); !over.empty()) {
        json::Array list;
        for (const std::string& line : over) {
            list.emplace_back(line);
        }
        out["over_budget"] = std::move(list);
    }
    if (!recipe.lod.empty()) {
        json::Array levels;
        for (const world::VfxLod& level : recipe.lod) {
            auto at = world::vfx_cost(recipe, parameters, level.detail);
            if (!at) {
                continue;
            }
            json::Value entry = cost_value(at.value());
            entry["distance"] = rounded(level.distance);
            entry["detail"] = rounded(level.detail);
            levels.emplace_back(std::move(entry));
        }
        out["lod"] = std::move(levels);
    }
    if (const auto warnings = recipe_warnings(session.world(), recipe, parameters);
        !warnings.empty()) {
        json::Array list;
        for (const std::string& line : warnings) {
            list.emplace_back(line);
        }
        out["warnings"] = std::move(list);
    }
    return out;
}

Reply vfx_check(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const auto& ecs = world.ecs();
    json::Value own = json::Object{};
    if (const json::Value* given = params.find("parameters"); given != nullptr) {
        own = *given;
    }
    std::vector<std::string> paths;
    if (const json::Value* given = params.find("recipe"); given != nullptr) {
        if (!world::is_vfx_path(given->as_string())) {
            return param_error(code::kInvalidParams, "/recipe",
                               quoted(given->as_string()) + " is not a .vfx.json file");
        }
        paths.push_back(given->as_string());
    }
    // The world's effects: those whose recipe is missing, broken or of the other dimension.
    json::Array problems;
    const auto check_effect = [&](entt::entity entity, const std::string& recipe, bool flat) {
        if (params.find("recipe") == nullptr && std::ranges::find(paths, recipe) == paths.end() &&
            world::is_vfx_path(recipe)) {
            paths.push_back(recipe);
        }
        auto loaded = world::load_vfx(world, recipe);
        std::string message;
        if (!loaded) {
            message = loaded.error().message;
        } else if (loaded.value()->dimension != (flat ? "2d" : "3d")) {
            message = quoted(recipe) + " is a " + loaded.value()->dimension + " effect, which a " +
                      (flat ? "VisualEffect2D" : "VisualEffect3D") + " can't play";
        }
        if (!message.empty()) {
            json::Value problem;
            problem["id"] = world.id_of(entity);
            problem["error"] = std::move(message);
            problems.emplace_back(std::move(problem));
        }
    };
    for (const auto [entity, e] : ecs.view<const world::VisualEffect3D>().each()) {
        check_effect(entity, e.recipe, false);
    }
    for (const auto [entity, e] : ecs.view<const world::VisualEffect2D>().each()) {
        check_effect(entity, e.recipe, true);
    }
    json::Value recipes = json::Object{};
    for (const std::string& path : paths) {
        recipes[path] = recipe_check(session, path, own);
    }
    json::Value result;
    result["recipes"] = std::move(recipes);
    if (!problems.empty()) {
        result["effects"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

// ---- vfx.preview ----

SessionOptions stage_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    std::erase(options.capabilities, Capability::Network);
    return options;
}

// What a stage shows at one moment, summed over the effect's parts.
json::Value sample_of(const world::World& world, entt::entity effect, bool flat, double t) {
    const auto& ecs = world.ecs();
    const std::string_view id = world.id_of(effect);
    json::Value out;
    out["t"] = rounded(t);
    std::set<std::string> layers;
    std::int64_t particles = 0;
    double light = 0.0;
    double flash = 0.0;
    int lingering = 0;
    for (const auto [entity, part] : ecs.view<const world::VfxPart>().each()) {
        if (part.effect != id) {
            continue;
        }
        if (part.linger >= 0.0F) {
            ++lingering;
        } else {
            layers.insert(part.layer);
        }
        if (ecs.all_of<world::CPUParticles3D>(entity)) {
            particles += static_cast<std::int64_t>(scene3d::particles_3d(world, entity).size());
        }
        if (ecs.all_of<world::CPUParticles2D>(entity)) {
            particles += static_cast<std::int64_t>(canvas::particles(world, entity).size());
        }
        if (const auto* g = ecs.try_get<const world::GPUParticles3D>(entity);
            g != nullptr && g->emitting) {
            particles += g->amount;
        }
        if (const auto* g = ecs.try_get<const world::GPUParticles2D>(entity);
            g != nullptr && g->emitting) {
            particles += g->amount;
        }
        if (const auto* l = ecs.try_get<const world::OmniLight3D>(entity); l != nullptr) {
            light += l->light_energy;
        }
        if (const auto* l = ecs.try_get<const world::SpotLight3D>(entity); l != nullptr) {
            light += l->light_energy;
        }
        if (const auto* l = ecs.try_get<const world::PointLight2D>(entity);
            l != nullptr && l->enabled) {
            light += l->energy;
        }
        if (const auto* rect = ecs.try_get<const world::ColorRect>(entity); rect != nullptr) {
            flash = std::max(flash, static_cast<double>(rect->color.w));
        }
    }
    json::Array names;
    for (const std::string& layer : layers) {
        names.emplace_back(layer);
    }
    out["layers"] = std::move(names);
    out["particles"] = particles;
    if (light > 0.0) {
        out["light"] = rounded(light);
    }
    if (flash > 0.0) {
        out["flash"] = rounded(flash);
    }
    if (lingering > 0) {
        out["lingering"] = lingering;
    }
    const world::CameraShakeOffsets shake = world::camera_shake_offsets(world);
    const Vec2 offset = flat ? shake.camera_2d : shake.camera_3d;
    if (offset.x != 0.0F || offset.y != 0.0F) {
        out["shake"] = json::Array{rounded(offset.x), rounded(offset.y)};
    }
    return out;
}

Reply vfx_preview(Session& session, const json::Value& params) {
    const std::string& path = params.find("recipe")->as_string();
    auto loaded = recipe_at(session, path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    const world::VfxRecipe& recipe = *loaded.value();
    const bool flat = recipe.dimension == "2d";
    const float distance = params.find("distance") != nullptr
                               ? static_cast<float>(params.find("distance")->as_double())
                               : 0.0F;
    // When to look: each layer's start and the effect's end, unless times are given.
    std::vector<double> times;
    if (const json::Value* given = params.find("times"); given != nullptr) {
        for (const json::Value& t : given->as_array()) {
            times.push_back(t.as_double());
        }
    } else {
        for (const world::VfxLayer& layer : recipe.layers) {
            times.push_back(layer.start + 0.05);
        }
        times.push_back(recipe.duration);
    }
    std::ranges::sort(times);
    times.erase(std::ranges::unique(times).begin(), times.end());
    if (times.empty() || times.back() > 600.0) {
        return param_error(code::kInvalidParams, "/times",
                           "give times from 0 to 600 seconds into the effect");
    }
    const json::Value* capture = params.find("capture");
    std::string out;
    std::string full;
    double capture_at = 0.0;
    if (capture != nullptr) {
        out = capture->find("out") != nullptr ? capture->find("out")->as_string()
                                              : "previews/" + stem_of(path) + ".png";
        if (!out.ends_with(".png")) {
            return param_error(code::kInvalidParams, "/capture/out",
                               quoted(out) + " must end in .png");
        }
        auto resolved = session.resolve_path(out);
        if (!resolved) {
            return param_error(code::kInvalidParams, "/capture/out", resolved.error().message);
        }
        full = resolved.value();
        capture_at = capture->find("at")->as_double();
    }
    Session stage(session.components(), session.commands(), stage_options(session));
    world::World& world = stage.world();
    auto& ecs = world.ecs();
    auto made = world.create("preview");
    if (!made) {
        return Reply::failure(made.error());
    }
    const entt::entity effect = made.value();
    auto camera_made = world.create("preview_camera");
    if (!camera_made) {
        return Reply::failure(camera_made.error());
    }
    const entt::entity camera = camera_made.value();
    json::Value own = json::Object{};
    if (const json::Value* given = params.find("parameters"); given != nullptr) {
        own = *given;
    }
    if (flat) {
        ecs.emplace<world::Transform2D>(effect);
        world::VisualEffect2D v;
        v.recipe = path;
        v.parameters = own;
        ecs.emplace<world::VisualEffect2D>(effect, std::move(v));
        world::Transform2D place;
        place.position = {static_cast<real_t>(distance), 0.0F};
        ecs.emplace<world::Transform2D>(camera, place);
        ecs.emplace<world::Camera2D>(camera);
    } else {
        ecs.emplace<world::Transform3D>(effect);
        world::VisualEffect3D v;
        v.recipe = path;
        v.parameters = own;
        ecs.emplace<world::VisualEffect3D>(effect, std::move(v));
        world::Transform3D place;
        // Back along z and a little up, looking at the effect.
        const real_t back = std::max<real_t>(static_cast<real_t>(distance), 4.0F);
        place.position = {0.0F, back * 0.25F, back};
        place.rotation = {-0.245F, 0.0F, 0.0F};
        ecs.emplace<world::Transform3D>(camera, place);
        world::Camera3D c;
        c.current = true;
        ecs.emplace<world::Camera3D>(camera, c);
        const entt::entity sun = world.create();
        world::Transform3D sun_place;
        sun_place.rotation = {-0.9F, 0.4F, 0.0F};
        ecs.emplace<world::Transform3D>(sun, sun_place);
        ecs.emplace<world::DirectionalLight3D>(sun);
    }
    json::Array samples;
    std::size_t next = 0;
    double t = 0.0;
    std::vector<json::Value> events;
    bool captured_done = capture == nullptr;
    json::Value captured_size;
    const auto record_events = [&]() {
        // What this step emitted, which the world hands systems on the next.
        for (const world::Event& event : world.event_queues().next) {
            if (event.kind == "vfx_started" || event.kind == "vfx_finished" ||
                event.kind == "vfx_failed") {
                json::Value e;
                e["kind"] = event.kind;
                e["t"] = rounded(t);
                if (event.kind == "vfx_failed" && event.data.find("error") != nullptr) {
                    e["error"] = *event.data.find("error");
                }
                events.push_back(std::move(e));
            }
        }
    };
    const int last = static_cast<int>(std::ceil(std::max(times.back(), capture_at) / kTick));
    for (int tick = 0; tick <= last + 1; ++tick) {
        world.step(kTick);
        t = static_cast<double>(tick + 1) * kTick;
        record_events();
        // The step that ends at or just past each time is when it is looked at.
        while (next < times.size() && times[next] <= t + 1e-9) {
            samples.emplace_back(sample_of(world, effect, flat, times[next]));
            ++next;
        }
        if (!captured_done && capture_at <= t + 1e-9) {
            canvas::CaptureOptions options;
            std::array<real_t, 2> size{320.0F, 240.0F};
            if (const json::Value* given = capture->find("size"); given != nullptr) {
                size = {static_cast<real_t>(given->as_array()[0].as_double()),
                        static_cast<real_t>(given->as_array()[1].as_double())};
            }
            options.viewport = Vec2{size[0], size[1]};
            auto image = canvas::capture(world, stage.assets(), options);
            if (!image) {
                return Reply::failure(image.error());
            }
            auto png = render::encode_png(image.value().image);
            if (!png) {
                return Reply::failure(png.error());
            }
            if (auto written = session.write_file(full, png.value()); !written) {
                return Reply::failure(written.error());
            }
            captured_size = json::Array{static_cast<std::int64_t>(image.value().image.width),
                                        static_cast<std::int64_t>(image.value().image.height)};
            captured_done = true;
        }
        if (next >= times.size() && captured_done) {
            break;
        }
    }
    json::Value result;
    result["samples"] = std::move(samples);
    if (!events.empty()) {
        json::Array list;
        for (json::Value& e : events) {
            list.emplace_back(std::move(e));
        }
        result["events"] = std::move(list);
    }
    if (capture != nullptr) {
        result["capture"]["out"] = out;
        result["capture"]["size"] = std::move(captured_size);
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_vfx_commands(Registry& registry) {
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"recipe"};
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .vfx.json file.");
        p["id"] = property("string", "The new effect's ID; default the file's name, numbered.");
        p["at"] = point_property("Where: [x, y, z] meters for a 3D effect, [x, y] pixels for 2D; "
                                 "in the parent's space with a parent.");
        p["parent"] = property("string", "An entity to play it under, moving with it.");
        p["bone"] = property("string", "With a parent that draws a 3D model: a bone to follow, "
                                       "as a BoneAttachment3D.");
        p["parameters"] = property("object", "The recipe's parameters to set, by name.");
        p["seed"] = property("integer", "Added to its emitters' seeds.");
        p["free_on_finish"] =
            property("boolean", "Remove the effect once a one-shot has finished. Default true.");
        registry.add({
            .name = "vfx.play",
            .description = "Plays a .vfx.json effect as a new VisualEffect3D or VisualEffect2D "
                           "at a point, under an entity or on a bone, with parameters set.",
            .capabilities = {Capability::Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = vfx_play,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["ids"] = property("array", "Effects to report; default all.");
        p["ids"]["items"]["type"] = "string";
        registry.add({
            .name = "vfx.state",
            .description = "Each visual effect's time, detail and the layers playing as "
                           "entities, and the cameras' shake.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = vfx_state,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .vfx.json file; default every recipe the world's "
                                         "effects name.");
        p["parameters"] = property("object", "Parameters to cost it with.");
        registry.add({
            .name = "vfx.check",
            .description = "Checks .vfx.json recipes: errors, cost at its busiest (particles, "
                           "lights, sounds, decals, layers) at full detail and each level of "
                           "detail, what goes past its budget, layers that never play or name "
                           "unknown components; and effects that can't play. "
                           "$ slash-cli schema vfx.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = vfx_check,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"recipe"};
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .vfx.json file.");
        p["parameters"] = property("object", "The recipe's parameters to set.");
        p["times"] = property("array", "Seconds into the effect to sample; default each layer's "
                                       "start and the end.");
        p["times"]["items"]["type"] = "number";
        p["times"]["items"]["minimum"] = 0;
        p["distance"] = property("number", "The camera's distance (meters, 2D pixels), which "
                                           "picks the level of detail. Default 0.");
        p["distance"]["minimum"] = 0;
        json::Value& c = p["capture"];
        c = property("object", "Also draw it into a PNG at a time.");
        c["additionalProperties"] = false;
        c["required"] = json::Array{"at"};
        c["properties"]["at"] = property("number", "Seconds into the effect.");
        c["properties"]["at"]["minimum"] = 0;
        c["properties"]["out"] =
            property("string", "The PNG to write; default previews/<file>.png.");
        c["properties"]["size"] = point_property("[width, height] pixels; default [320, 240].");
        registry.add({
            .name = "vfx.preview",
            .description = "Plays a .vfx.json effect alone on a stage of its own and returns "
                           "what shows at times: layers, particles alive, light, screen flash "
                           "and camera shake, and its events; optionally draws a PNG.",
            .capabilities = {Capability::Read, Capability::Filesystem},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = vfx_preview,
        });
    }
}

} // namespace slash::command
