#include "slash/world/vfx.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/expression.hpp"
#include "slash/core/log.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/character.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/creature.hpp"
#include "slash/world/foliage.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/vehicle.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine visual effect",
  "description": "A visual effect as layers on one timeline, after Unreal's Niagara systems: each layer an entity of components (particles, ribbons, mesh particles, flipbooks, decals, lights, sounds, screen flashes, camera shakes) that appears at its start, has fields moved along curves and goes at its end. Any value may be an expression ('=8 * intensity') over the parameters, detail and, in curves, t and life.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format", "layers"],
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "name": {"type": "string", "description": "What the effect is called."},
    "dimension": {"enum": ["3d", "2d"], "description": "Whether it plays in a 3D world (a VisualEffect3D) or a 2D one (a VisualEffect2D). Default 3d."},
    "duration": {"type": "number", "exclusiveMinimum": 0, "description": "Seconds the effect lasts. Default 1."},
    "loop": {"type": "boolean", "description": "Whether it starts again at its end. Default false."},
    "parameters": {"type": "object", "description": "Parameters by name with their defaults (numbers, booleans, strings or arrays such as colors), which expressions read and effects set."},
    "layers": {
      "type": "array", "minItems": 1,
      "description": "What plays, each as an entity '<effect>_<name>' under the effect while it lasts.",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["name", "components"],
        "properties": {
          "name": {"type": "string", "pattern": "^[A-Za-z0-9_]+$", "description": "The layer's name, letters, digits and underscores."},
          "start": {"type": "number", "minimum": 0, "description": "Seconds into the effect it appears. Default 0."},
          "duration": {"type": "number", "minimum": 0, "description": "Seconds it lasts; 0 lasts to the effect's end. Default 0."},
          "offset": {"type": "array", "minItems": 2, "maxItems": 3, "items": {"type": ["number", "string"]}, "description": "Where it sits in the effect's space: [x, y, z] in 3D, [x, y] in 2D."},
          "rotation": {"type": ["array", "number", "string"], "description": "Its turn: Euler radians [x, y, z] in 3D, radians in 2D."},
          "components": {"type": "object", "description": "Its entity's components by name, as a scene writes them; values may be expressions.", "additionalProperties": {"type": "object"}},
          "curves": {
            "type": "object",
            "description": "Fields moved while it lasts, by path ('OmniLight3D/light_energy', 'ColorRect/color/a'): keys of [seconds into the layer, value], or an object of keys and ease.",
            "additionalProperties": {
              "oneOf": [
                {"type": "array", "minItems": 1, "items": {"type": "array", "minItems": 2, "maxItems": 2, "prefixItems": [{"type": "number", "minimum": 0}, {}]}},
                {"type": "object", "additionalProperties": false, "required": ["keys"],
                 "properties": {
                   "keys": {"type": "array", "minItems": 1, "items": {"type": "array", "minItems": 2, "maxItems": 2, "prefixItems": [{"type": "number", "minimum": 0}, {}]}},
                   "ease": {"enum": ["linear", "smooth", "step"], "description": "How values go between keys. Default linear."}
                 }}
              ]
            }
          },
          "detail": {"type": "number", "minimum": 0, "maximum": 1, "description": "The least level of detail it plays at. Default 0, always."},
          "world": {"type": "boolean", "description": "Whether it stays where it appeared rather than moving with the effect. Default false."}
        }
      }
    },
    "lod": {
      "type": "array",
      "description": "Levels of detail, nearest first: from distance (meters, or pixels in 2D) from the current camera, layers needing more detail drop and particles thin to detail.",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["distance", "detail"],
        "properties": {
          "distance": {"type": "number", "minimum": 0},
          "detail": {"type": "number", "minimum": 0, "maximum": 1}
        }
      }
    },
    "budget": {
      "type": "object", "additionalProperties": false,
      "description": "What the effect may cost at its busiest at full detail; 0 is no limit. vfx.check reports what goes past it.",
      "properties": {
        "particles": {"type": "integer", "minimum": 0},
        "lights": {"type": "integer", "minimum": 0},
        "sounds": {"type": "integer", "minimum": 0},
        "decals": {"type": "integer", "minimum": 0},
        "layers": {"type": "integer", "minimum": 0}
      }
    }
  }
})";

constexpr std::array<std::string_view, 4> kEmitters = {"CPUParticles3D", "CPUParticles2D",
                                                       "GPUParticles3D", "GPUParticles2D"};
constexpr std::array<std::string_view, 5> kLights = {
    "OmniLight3D", "SpotLight3D", "DirectionalLight3D", "PointLight2D", "DirectionalLight2D"};
constexpr std::array<std::string_view, 3> kSounds = {"AudioStreamPlayer", "AudioStreamPlayer2D",
                                                     "AudioStreamPlayer3D"};

bool is_one_of(std::string_view name, std::span<const std::string_view> names) {
    return std::ranges::find(names, name) != names.end();
}

float number(const json::Value* value, float fallback) {
    return value != nullptr && value->is_number() ? static_cast<float>(value->as_double())
                                                  : fallback;
}

// Every expression in a value, with the JSON Pointer of each.
void expressions(const json::Value& value, const std::string& at,
                 std::vector<std::pair<std::string, std::string>>& out) {
    if (value.is_string() && value.as_string().starts_with('=')) {
        out.emplace_back(at, value.as_string().substr(1));
    } else if (value.is_array()) {
        const json::Array& items = value.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            expressions(items[i], at + "/" + std::to_string(i), out);
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.as_object()) {
            expressions(item, at + "/" + json::pointer_escape(key), out);
        }
    }
}

// A whole number as an integer, so integer fields take it.
json::Value tidy_number(json::Value value) {
    if (value.is_number() && !value.is_integral()) {
        const double x = value.as_double();
        constexpr double kLimit = 9007199254740992.0; // 2^53
        if (std::isfinite(x) && std::abs(x) < kLimit && std::floor(x) == x) {
            return static_cast<std::int64_t>(x);
        }
    }
    return value;
}

std::vector<VfxKey> parse_keys(const json::Value& keys) {
    std::vector<VfxKey> out;
    for (const json::Value& key : keys.as_array()) {
        const json::Array& pair = key.as_array();
        out.push_back({.time = static_cast<float>(pair[0].as_double()), .value = pair[1]});
    }
    return out;
}

struct VfxCache {
    std::mutex mutex;
    struct Entry {
        std::string text;
        std::shared_ptr<const VfxRecipe> recipe;
    };
    std::unordered_map<std::string, Entry> entries;
};
using VfxCacheHolder = std::shared_ptr<VfxCache>;

// The value of a part of a field ("a" of a color, "y" of a vector, a number of an array, a key
// of an object), made where it is missing.
json::Value* part_of(json::Value& node, std::string_view part) {
    if (node.is_array()) {
        std::size_t index = 0;
        static constexpr std::string_view kXyzw = "xyzw";
        static constexpr std::string_view kRgba = "rgba";
        if (part.size() == 1 && kXyzw.find(part[0]) != std::string_view::npos) {
            index = kXyzw.find(part[0]);
        } else if (part.size() == 1 && kRgba.find(part[0]) != std::string_view::npos) {
            index = kRgba.find(part[0]);
        } else {
            const auto parsed = std::from_chars(part.data(), part.data() + part.size(), index);
            if (parsed.ec != std::errc{}) {
                return nullptr;
            }
        }
        return index < node.size() ? &node.as_array()[index] : nullptr;
    }
    if (node.is_object() || node.is_null()) {
        return &node[part];
    }
    return nullptr;
}

std::vector<std::string_view> split(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        parts.push_back(path.substr(0, slash));
        if (slash == std::string_view::npos) {
            break;
        }
        path.remove_prefix(slash + 1);
    }
    return parts;
}

// Sets a component's field, or a part of one, by a curve's path.
Result<void> set_path(World& world, entt::entity entity, std::string_view path,
                      const json::Value& value) {
    const std::vector<std::string_view> parts = split(path);
    if (parts.size() < 2) {
        return make_error(ErrorCode::InvalidArgument,
                          "a curve's path names a component and a field: \"" + std::string(path) +
                              "\"");
    }
    const ComponentType* type = world.components().find(parts[0]);
    if (type == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "no component named \"" + std::string(parts[0]) + "\"");
    }
    json::Value set = value;
    if (parts.size() > 2) {
        const json::Value current =
            type->has(world, entity) ? type->get(world, entity) : type->defaults();
        const json::Value* found = current.find(parts[1]);
        set = found != nullptr ? *found : json::Value{};
        json::Value* node = &set;
        for (std::size_t i = 2; i < parts.size() && node != nullptr; ++i) {
            node = part_of(*node, parts[i]);
        }
        if (node == nullptr) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(path) + "\" names no part of the field");
        }
        *node = value;
    }
    json::Value fields = json::Object{};
    fields[parts[1]] = std::move(set);
    std::string at;
    if (auto assigned = type->assign(world, entity, fields, at); !assigned) {
        return make_error(assigned.error().code,
                          (at.empty() ? std::string() : at + ": ") + assigned.error().message);
    }
    return {};
}

// Smooth noise from -1 to 1 along a line, of whole numbers hashed with a seed.
float lattice(std::int64_t at, std::uint32_t seed) {
    std::uint32_t state = (static_cast<std::uint32_t>(at) * 747796405U) + 2891336453U + seed;
    state ^= static_cast<std::uint32_t>(static_cast<std::uint64_t>(at) >> 32U) * 2654435761U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return static_cast<float>(((word >> 22U) ^ word) >> 8U) * (1.0F / 16777216.0F);
}

float shake_noise(double x, std::uint32_t seed) {
    const double whole = std::floor(x);
    const auto t = static_cast<float>(x - whole);
    const auto at = static_cast<std::int64_t>(whole);
    const float a = lattice(at, seed);
    const float b = lattice(at + 1, seed);
    const float eased = t * t * (3.0F - (2.0F * t));
    return ((a + ((b - a) * eased)) * 2.0F) - 1.0F;
}

std::uint32_t hash_id(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

// What the system keeps between ticks outside components: the camera offsets shakes last wrote,
// so they come off again, and the effects whose recipe failed, so each says so once.
struct VfxState {
    entt::entity camera_3d = entt::null;
    Vec2 written_3d;
    Vec2 applied_3d;
    entt::entity camera_2d = entt::null;
    Vec2 written_2d;
    Vec2 applied_2d;
    double seconds = 0.0;
    std::set<std::string> failed;
};
using VfxStateHolder = std::shared_ptr<VfxState>;

VfxState& state_of(World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<VfxStateHolder>() == nullptr) {
        ctx.emplace<VfxStateHolder>(std::make_shared<VfxState>());
    }
    return *ctx.get<VfxStateHolder>();
}

// An effect, 3D or 2D, read and written through one shape.
struct Effect {
    entt::entity entity = entt::null;
    bool flat = false;
    std::string recipe;
    json::Value parameters;
    bool playing = true;
    float time = 0.0F;
    float speed_scale = 1.0F;
    float lod_bias = 1.0F;
    int seed = 0;
    bool free_on_finish = false;
};

template <typename T>
Effect effect_of(entt::entity entity, const T& effect, bool flat) {
    return {.entity = entity,
            .flat = flat,
            .recipe = effect.recipe,
            .parameters = effect.parameters,
            .playing = effect.playing,
            .time = effect.time,
            .speed_scale = effect.speed_scale,
            .lod_bias = effect.lod_bias,
            .seed = effect.seed,
            .free_on_finish = effect.free_on_finish};
}

// The parts playing each effect's layers, by effect ID.
struct Part {
    entt::entity entity = entt::null;
    std::string layer;
    int cycle = 0;
};

// Ends a layer's part: particles stop emitting and linger until their last has gone; anything
// else goes now.
void end_part(World& world, entt::entity entity, std::vector<entt::entity>& gone) {
    auto& ecs = world.ecs();
    auto& part = ecs.get<VfxPart>(entity);
    if (part.linger >= 0.0F) {
        return;
    }
    float linger = -1.0F;
    for (const std::string_view name : kEmitters) {
        const ComponentType* type = world.components().find(name);
        if (type == nullptr || !type->has(world, entity)) {
            continue;
        }
        const json::Value fields = type->get(world, entity);
        const float speed = std::max(number(fields.find("speed_scale"), 1.0F), 0.01F);
        const float life = number(fields.find("lifetime"), 1.0F) / speed;
        linger = std::max(linger, life + 0.05F);
        std::string at;
        json::Value stop = json::Object{};
        stop["emitting"] = false;
        (void)type->assign(world, entity, stop, at);
    }
    if (linger < 0.0F) {
        gone.push_back(entity);
    } else {
        ecs.get<VfxPart>(entity).linger = linger;
    }
}

// The entity a new part plays as: "<effect>_<layer>", numbered past those taken.
Result<entt::entity> make_part_entity(World& world, std::string_view effect,
                                      std::string_view layer) {
    const std::string base = std::string(effect) + "_" + std::string(layer);
    for (std::size_t n = 1;; ++n) {
        const std::string id = n == 1 ? base : base + "_" + std::to_string(n);
        if (world.find(id) == entt::null) {
            return world.create(id);
        }
    }
}

Vec3 vec3_of(const json::Value& value) {
    Vec3 out;
    if (value.is_array()) {
        const json::Array& items = value.as_array();
        out.x = !items.empty() ? number(items.data(), 0.0F) : 0.0F;
        out.y = items.size() > 1 ? number(&items[1], 0.0F) : 0.0F;
        out.z = items.size() > 2 ? number(&items[2], 0.0F) : 0.0F;
    }
    return out;
}

// Plays one layer as a new entity under the effect.
Result<entt::entity> spawn_part(World& world, const Effect& effect, const VfxLayer& layer,
                                const json::Value& parameters, float detail, int cycle) {
    auto components = vfx_components(layer, parameters, detail);
    if (!components) {
        return components.error();
    }
    auto offset = vfx_value(layer.offset, parameters, detail);
    if (!offset) {
        return offset.error();
    }
    auto rotation = vfx_value(layer.rotation, parameters, detail);
    if (!rotation) {
        return rotation.error();
    }
    const std::string id(world.id_of(effect.entity));
    auto made = make_part_entity(world, id, layer.name);
    if (!made) {
        return made.error();
    }
    const entt::entity entity = made.value();
    auto& ecs = world.ecs();
    // The emitters' seeds follow the effect's and the cycle's, so cycles differ but repeat.
    json::Value fields = std::move(components).value();
    for (const std::string_view name : kEmitters) {
        if (json::Value* emitter = fields.find(name); emitter != nullptr) {
            const std::int64_t seed =
                emitter->find("seed") != nullptr && emitter->find("seed")->is_integral()
                    ? emitter->find("seed")->as_int()
                    : 0;
            (*emitter)["seed"] = seed + effect.seed + (static_cast<std::int64_t>(cycle) * 7919);
        }
    }
    for (const auto& [name, values] : fields.as_object()) {
        const ComponentType* type = world.components().find(name);
        if (type == nullptr) {
            world.destroy(entity);
            return make_error(ErrorCode::InvalidArgument,
                              "layer " + layer.name + ": no component named \"" + name + "\"");
        }
        std::string at;
        if (auto assigned = type->assign(world, entity, values, at); !assigned) {
            world.destroy(entity);
            return make_error(assigned.error().code, "layer " + layer.name + ": " + name +
                                                         (at.empty() ? "" : at) + ": " +
                                                         assigned.error().message);
        }
    }
    ecs.emplace_or_replace<VfxPart>(
        entity, VfxPart{.effect = id, .layer = layer.name, .cycle = cycle, .linger = -1.0F});
    const bool control = fields.find("Control") != nullptr;
    if (control) {
        return entity; // a screen flash or overlay: placed on the screen, not in the world
    }
    if (effect.flat) {
        Transform2D place;
        const Vec3 at = vec3_of(offset.value());
        place.position = {at.x, at.y};
        place.rotation = number(&rotation.value(), 0.0F);
        if (layer.world) {
            const Transform2D global = Tree2D(world).global(effect.entity);
            place.position = affine(global).apply(place.position);
            place.rotation += global.rotation;
            place.top_level = true;
        }
        if (const auto* own = ecs.try_get<const Transform2D>(entity); own != nullptr) {
            place.scale = own->scale;
            place.skew = own->skew;
        }
        ecs.emplace_or_replace<Transform2D>(entity, place);
        if (ecs.all_of<Transform2D>(effect.entity)) {
            ecs.get<Transform2D>(effect.entity).children.push_back(entity);
        }
    } else {
        Transform3D place;
        place.position = vec3_of(offset.value());
        place.rotation = vec3_of(rotation.value());
        if (layer.world) {
            const Tree3D tree(world);
            place.position = tree.global(effect.entity).apply(place.position);
            if (const auto* own = ecs.try_get<const Transform3D>(effect.entity); own != nullptr) {
                place.rotation = place.rotation + own->rotation;
            }
            place.top_level = true;
        }
        if (const auto* own = ecs.try_get<const Transform3D>(entity); own != nullptr) {
            place.scale = own->scale;
        }
        ecs.emplace_or_replace<Transform3D>(entity, place);
        if (ecs.all_of<Transform3D>(effect.entity)) {
            ecs.get<Transform3D>(effect.entity).children.push_back(entity);
        }
    }
    return entity;
}

// The distance from the current camera to the effect.
float camera_distance(const World& world, const Effect& effect) {
    if (effect.flat) {
        const Tree2D tree(world);
        const entt::entity camera = current_camera(world, tree);
        if (camera == entt::null) {
            return 0.0F;
        }
        const Vec2 d = tree.global(effect.entity).position - tree.global(camera).position;
        return to_float(std::sqrt((d.x * d.x) + (d.y * d.y)));
    }
    const Tree3D tree(world);
    const entt::entity camera = current_camera(world, tree);
    if (camera == entt::null) {
        return 0.0F;
    }
    const Vec3 d = tree.global(effect.entity).origin - tree.global(camera).origin;
    return to_float(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)));
}

void write_back(World& world, const Effect& effect) {
    auto& ecs = world.ecs();
    if (effect.flat) {
        auto& own = ecs.get<VisualEffect2D>(effect.entity);
        own.playing = effect.playing;
        own.time = effect.time;
    } else {
        auto& own = ecs.get<VisualEffect3D>(effect.entity);
        own.playing = effect.playing;
        own.time = effect.time;
    }
}

// Moves one effect on a tick and keeps its layers' parts in step with its time.
void run_effect(World& world, Effect effect,
                std::unordered_map<std::string, std::vector<Part>>& parts_by_effect,
                std::vector<entt::entity>& gone, std::vector<entt::entity>& freed,
                double tick_seconds) {
    VfxState& state = state_of(world);
    const std::string id(world.id_of(effect.entity));
    auto loaded = load_vfx(world, effect.recipe);
    std::vector<Part>& parts = parts_by_effect[id];
    if (!loaded || loaded.value()->dimension != (effect.flat ? "2d" : "3d")) {
        const std::string message =
            !loaded ? loaded.error().message
                    : "\"" + effect.recipe + "\" is a " + loaded.value()->dimension +
                          " effect, which a " +
                          (effect.flat ? "VisualEffect2D" : "VisualEffect3D") + " can't play";
        if (state.failed.insert(id + "|" + effect.recipe).second) {
            json::Value data;
            data["recipe"] = effect.recipe;
            data["error"] = message;
            world.emit("vfx_failed", id, std::move(data));
            log::warn("a visual effect can't play", {{"id", id}, {"error", message}});
        }
        return;
    }
    const VfxRecipe& recipe = *loaded.value();
    const json::Value parameters = vfx_parameters(recipe, effect.parameters);
    const float detail =
        vfx_detail(recipe, camera_distance(world, effect) / std::max(effect.lod_bias, 0.001F));
    if (!effect.playing) {
        // Stopped: what still plays ends, its particles finishing.
        if (!parts.empty()) {
            for (const Part& part : parts) {
                end_part(world, part.entity, gone);
            }
            return;
        }
        // Finished one-shots, and loops stopped once started, that asked to go do once their
        // last particles have.
        const bool done = recipe.loop ? effect.time > 0.0F : effect.time >= recipe.duration;
        if (effect.free_on_finish && done) {
            freed.push_back(effect.entity);
        }
        return;
    }
    const float before = effect.time;
    effect.time += static_cast<float>(tick_seconds) * effect.speed_scale;
    if (before <= 0.0F) {
        json::Value data;
        data["recipe"] = effect.recipe;
        world.emit("vfx_started", id, std::move(data));
    }
    bool finished = false;
    int cycle = 0;
    float local = effect.time;
    if (recipe.loop) {
        cycle = static_cast<int>(std::floor(effect.time / recipe.duration));
        local = effect.time - (static_cast<float>(cycle) * recipe.duration);
    } else if (effect.time >= recipe.duration) {
        finished = true;
        local = recipe.duration;
    }
    for (const VfxLayer& layer : recipe.layers) {
        const float length = vfx_layer_length(recipe, layer);
        const bool active = !finished && local >= layer.start && local < layer.start + length &&
                            layer.detail <= detail;
        Part* playing = nullptr;
        for (Part& part : parts) {
            if (part.layer != layer.name) {
                continue;
            }
            if (active && part.cycle == cycle) {
                playing = &part;
            } else {
                end_part(world, part.entity, gone);
            }
        }
        if (active && playing == nullptr) {
            auto made = spawn_part(world, effect, layer, parameters, detail, cycle);
            if (!made) {
                if (state.failed.insert(id + "|" + layer.name).second) {
                    json::Value data;
                    data["recipe"] = effect.recipe;
                    data["error"] = made.error().message;
                    world.emit("vfx_failed", id, std::move(data));
                    log::warn("a visual effect's layer can't play",
                              {{"id", id}, {"error", made.error().message}});
                }
                continue;
            }
            parts.push_back({.entity = made.value(), .layer = layer.name, .cycle = cycle});
            playing = &parts.back();
        }
        if (playing != nullptr) {
            const float t = local - layer.start;
            for (const VfxCurve& curve : layer.curves) {
                auto value = vfx_curve_value(curve, t, length, parameters, detail);
                const Result<void> set =
                    value ? set_path(world, playing->entity, curve.path, value.value())
                          : Result<void>(value.error());
                if (!set && state.failed.insert(id + "|" + layer.name + "|" + curve.path).second) {
                    log::warn("a visual effect's curve can't be set",
                              {{"id", id}, {"path", curve.path}, {"error", set.error().message}});
                }
            }
        }
    }
    if (finished) {
        effect.playing = false;
        effect.time = recipe.duration;
        json::Value data;
        data["recipe"] = effect.recipe;
        world.emit("vfx_finished", id, std::move(data));
    }
    write_back(world, effect);
}

// Shakes the current cameras by the enabled CameraShakes, taking off what was added last tick.
void shake_cameras(World& world, double tick_seconds) {
    VfxState& state = state_of(world);
    state.seconds += tick_seconds;
    auto& ecs = world.ecs();
    const auto shake_at = [&](entt::entity entity, const CameraShake& shake, float distance) {
        if (!shake.enabled || shake.strength <= 0.0F) {
            return Vec2{};
        }
        float reach = 1.0F;
        if (shake.radius > 0.0F) {
            reach = std::max(0.0F, 1.0F - (distance / shake.radius));
        }
        const std::uint32_t seed = hash_id(world.id_of(entity));
        const double phase = state.seconds * static_cast<double>(shake.frequency);
        const float strength = shake.strength * reach;
        return Vec2{shake_noise(phase, seed) * strength,
                    shake_noise(phase + 57.0, seed ^ 0x9E3779B9U) * strength};
    };
    // 3D: the current Camera3D's h_offset and v_offset.
    {
        const Tree3D tree(world);
        const entt::entity camera = current_camera(world, tree);
        Vec2 offset;
        if (camera != entt::null) {
            const Vec3 eye = tree.global(camera).origin;
            for (const auto [entity, shake] : ecs.view<const CameraShake>().each()) {
                if (!ecs.all_of<Transform3D>(entity)) {
                    continue;
                }
                const Vec3 d = tree.global(entity).origin - eye;
                const auto distance = to_float(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)));
                offset = offset + shake_at(entity, shake, distance);
            }
            auto& view = ecs.get<Camera3D>(camera);
            Vec2 base{view.h_offset, view.v_offset};
            if (camera == state.camera_3d && base.x == state.written_3d.x &&
                base.y == state.written_3d.y) {
                base = base - state.applied_3d;
            }
            if (offset.x != 0.0F || offset.y != 0.0F || camera != state.camera_3d ||
                state.applied_3d.x != 0.0F || state.applied_3d.y != 0.0F) {
                view.h_offset = static_cast<float>(base.x + offset.x);
                view.v_offset = static_cast<float>(base.y + offset.y);
            }
            state.written_3d = {view.h_offset, view.v_offset};
        }
        state.camera_3d = camera;
        state.applied_3d = offset;
    }
    // 2D: the current Camera2D's offset.
    {
        const Tree2D tree(world);
        const entt::entity camera = current_camera(world, tree);
        Vec2 offset;
        if (camera != entt::null) {
            const Vec2 eye = tree.global(camera).position;
            for (const auto [entity, shake] : ecs.view<const CameraShake>().each()) {
                if (!ecs.all_of<Transform2D>(entity)) {
                    continue;
                }
                const Vec2 d = tree.global(entity).position - eye;
                const auto distance = to_float(std::sqrt((d.x * d.x) + (d.y * d.y)));
                offset = offset + shake_at(entity, shake, distance);
            }
            auto& view = ecs.get<Camera2D>(camera);
            Vec2 base = view.offset;
            if (camera == state.camera_2d && base.x == state.written_2d.x &&
                base.y == state.written_2d.y) {
                base = base - state.applied_2d;
            }
            view.offset = base + offset;
            state.written_2d = view.offset;
        }
        state.camera_2d = camera;
        state.applied_2d = offset;
    }
}

// The point an event's data names ([x, y] or [x, y, z]), if it names one.
std::optional<Vec3> event_position(const json::Value& data) {
    const json::Value* position = data.is_object() ? data.find("position") : nullptr;
    if (position == nullptr || !position->is_array()) {
        return std::nullopt;
    }
    const json::Array& items = position->as_array();
    if (items.size() < 2 || items.size() > 3) {
        return std::nullopt;
    }
    Vec3 out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].is_number()) {
            return std::nullopt;
        }
        const auto v = static_cast<real_t>(items[i].as_double());
        if (i == 0) {
            out.x = v;
        } else if (i == 1) {
            out.y = v;
        } else {
            out.z = v;
        }
    }
    return out;
}

// Plays what the tools' recipes say the last tick's events play, at most kMaxHooks a tick, and
// keeps each vehicle's "running" effect going while its throttle is open.
void play_hooks(World& world) {
    constexpr std::size_t kMaxHooks = 16;
    auto& ecs = world.ecs();
    std::optional<Tree3D> tree3;
    std::optional<Tree2D> tree2;
    const auto place_of = [&](entt::entity entity, bool flat) {
        if (flat) {
            if (!tree2) {
                tree2.emplace(world);
            }
            const Vec2 at = tree2->global(entity).position;
            return Vec3{at.x, at.y, 0.0F};
        }
        if (!tree3) {
            tree3.emplace(world);
        }
        return tree3->global(entity).origin;
    };
    std::size_t played = 0;
    // A copy: playing makes entities, and events stay the tick's.
    const std::vector<Event> events(world.events().begin(), world.events().end());
    for (const Event& event : events) {
        if (played >= kMaxHooks) {
            break;
        }
        const entt::entity entity = event.id.empty() ? entt::null : world.find(event.id);
        if (entity == entt::null) {
            continue;
        }
        VfxHooks hooks;
        bool flat = ecs.all_of<Transform2D>(entity) && !ecs.all_of<Transform3D>(entity);
        if (event.kind.starts_with("foliage_")) {
            const json::Value* type = event.data.is_object() ? event.data.find("type") : nullptr;
            if (type == nullptr || !type->is_string()) {
                continue;
            }
            hooks = foliage_vfx_hooks(world, type->as_string());
            flat = ecs.all_of<Foliage2D>(entity);
        } else {
            hooks = vfx_hooks_of(world, entity);
        }
        const auto found = hooks.find(event.kind);
        if (found == hooks.end() || found->second.empty()) {
            continue;
        }
        if (!ecs.any_of<Transform2D, Transform3D>(entity)) {
            continue;
        }
        const Vec3 at = event_position(event.data).value_or(place_of(entity, flat));
        if (play_vfx(world, found->second, event.id, event.kind, at, flat) != entt::null) {
            ++played;
        }
    }
    // Running vehicles: <vehicle>_running follows the vehicle while its throttle is open, then
    // stops and lets its particles finish.
    const auto keep_running = [&]<typename Vehicle, typename Effect>(bool flat) {
        std::vector<std::pair<entt::entity, bool>> vehicles;
        for (const auto [entity, vehicle] : ecs.view<const Vehicle>().each()) {
            vehicles.emplace_back(entity,
                                  vehicle.enabled && std::abs(vehicle.controls.throttle) > 0.01F);
        }
        for (const auto& [entity, running] : vehicles) {
            const VfxHooks hooks = vfx_hooks_of(world, entity);
            const auto found = hooks.find("running");
            if (found == hooks.end() || found->second.empty()) {
                continue;
            }
            const std::string id = std::string(world.id_of(entity)) + "_running";
            entt::entity effect = world.find(id);
            if (effect != entt::null && !ecs.all_of<Effect>(effect)) {
                continue;
            }
            if (!running) {
                if (effect != entt::null) {
                    auto& playing = ecs.get<Effect>(effect);
                    playing.playing = false;
                    playing.free_on_finish = true;
                }
                continue;
            }
            const Vec3 at = place_of(entity, flat);
            if (effect == entt::null) {
                auto made = world.create(id);
                if (!made) {
                    continue;
                }
                effect = made.value();
                Effect started;
                started.recipe = found->second;
                ecs.emplace<Effect>(effect, std::move(started));
            } else if (auto& playing = ecs.get<Effect>(effect); !playing.playing) {
                playing.playing = true;
                playing.free_on_finish = false;
                playing.time = 0.0F;
            }
            // Only its place: the effect's children are its parts.
            if (flat) {
                const Transform2D global = tree2->global(entity);
                auto& placed = ecs.get_or_emplace<Transform2D>(effect);
                placed.position = global.position;
                placed.rotation = global.rotation;
                placed.scale = global.scale;
            } else {
                auto& placed = ecs.get_or_emplace<Transform3D>(effect);
                placed.position = at;
                placed.rotation = euler(orthonormalized(tree3->global(entity)));
            }
        }
    };
    if (!ecs.view<const Vehicle3D>().empty()) {
        keep_running.template operator()<Vehicle3D, VisualEffect3D>(false);
    }
    if (!ecs.view<const Vehicle2D>().empty()) {
        keep_running.template operator()<Vehicle2D, VisualEffect2D>(true);
    }
}

void run(World& world, double tick_seconds) {
    play_hooks(world);
    auto& ecs = world.ecs();
    // Each effect's parts, and parts lingering or left by an effect that went.
    std::unordered_map<std::string, std::vector<Part>> parts_by_effect;
    std::vector<entt::entity> gone;
    for (const auto [entity, part] : ecs.view<VfxPart>().each()) {
        if (part.linger >= 0.0F) {
            part.linger -= static_cast<float>(tick_seconds);
            if (part.linger <= 0.0F) {
                gone.push_back(entity);
            }
            continue;
        }
        const entt::entity effect = world.find(part.effect);
        if (effect == entt::null || !ecs.any_of<VisualEffect3D, VisualEffect2D>(effect)) {
            gone.push_back(entity); // its effect went: it goes too, as a node under one would
            continue;
        }
        parts_by_effect[part.effect].push_back(
            {.entity = entity, .layer = part.layer, .cycle = part.cycle});
    }
    std::vector<entt::entity> freed;
    std::vector<Effect> effects;
    for (const auto [entity, effect] : ecs.view<const VisualEffect3D>().each()) {
        effects.push_back(effect_of(entity, effect, false));
    }
    for (const auto [entity, effect] : ecs.view<const VisualEffect2D>().each()) {
        effects.push_back(effect_of(entity, effect, true));
    }
    std::set<std::string> lingering;
    for (const auto [entity, part] : ecs.view<const VfxPart>().each()) {
        if (part.linger > 0.0F) {
            lingering.insert(part.effect);
        }
    }
    for (Effect& effect : effects) {
        if (!effect.playing && lingering.contains(std::string(world.id_of(effect.entity)))) {
            continue; // waits for its last particles before it may be freed
        }
        run_effect(world, std::move(effect), parts_by_effect, gone, freed, tick_seconds);
    }
    std::ranges::sort(gone);
    gone.erase(std::ranges::unique(gone).begin(), gone.end());
    for (const entt::entity entity : gone) {
        if (!world.valid(entity)) {
            continue;
        }
        // Off its effect's children first, so the effect lists no entity that has gone.
        if (const entt::entity effect = world.find(ecs.get<const VfxPart>(entity).effect);
            effect != entt::null) {
            if (auto* place = ecs.try_get<Transform3D>(effect); place != nullptr) {
                std::erase(place->children, entity);
            }
            if (auto* place = ecs.try_get<Transform2D>(effect); place != nullptr) {
                std::erase(place->children, entity);
            }
        }
        world.destroy(entity);
    }
    for (const entt::entity entity : freed) {
        if (!world.valid(entity)) {
            continue;
        }
        // Off whatever it was played under, so no parent lists an entity that has gone.
        for (auto [parent, place] : ecs.view<Transform3D>().each()) {
            std::erase(place.children, entity);
        }
        for (auto [parent, place] : ecs.view<Transform2D>().each()) {
            std::erase(place.children, entity);
        }
        world.destroy(entity);
    }
    shake_cameras(world, tick_seconds);
}

} // namespace

bool is_vfx_path(std::string_view path) {
    return path.ends_with(kVfxSuffix);
}

json::Value vfx_schema() {
    static const json::Value schema = [] {
        auto parsed = json::parse(kSchema);
        SLASH_ASSERT(parsed.ok(), "the vfx schema parses");
        return std::move(parsed).value();
    }();
    return schema;
}

std::vector<json::ValidationError> validate_vfx(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, vfx_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* dimension = doc.find("dimension");
    const bool flat = dimension != nullptr && dimension->as_string() == "2d";
    std::vector<std::pair<std::string, std::string>> found;
    std::set<std::string> names;
    const json::Array& layers = doc.find("layers")->as_array();
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const json::Value& layer = layers[i];
        const std::string at = "/layers/" + std::to_string(i);
        const std::string& name = layer.find("name")->as_string();
        const bool word = !name.empty() && std::ranges::all_of(name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_';
        });
        if (!word) {
            errors.push_back({at + "/name", "a layer's name is letters, digits and underscores, "
                                            "as it names an entity"});
        }
        if (!names.insert(name).second) {
            errors.push_back({at + "/name", "another layer is named \"" + name + "\""});
        }
        if (const json::Value* offset = layer.find("offset"); offset != nullptr) {
            const std::size_t wanted = flat ? 2 : 3;
            if (offset->size() != wanted) {
                errors.push_back({at + "/offset",
                                  std::string("expected ") + (flat ? "[x, y] in a 2D effect"
                                                                   : "[x, y, z] in a 3D effect")});
            }
        }
        if (const json::Value* rotation = layer.find("rotation"); rotation != nullptr) {
            if (flat && rotation->is_array()) {
                errors.push_back({at + "/rotation", "expected radians in a 2D effect"});
            } else if (!flat && rotation->is_array() && rotation->size() != 3) {
                errors.push_back({at + "/rotation", "expected [x, y, z] radians in a 3D effect"});
            }
        }
        if (const json::Value* curves = layer.find("curves"); curves != nullptr) {
            for (const auto& [path, curve] : curves->as_object()) {
                const json::Value& keys = curve.is_array() ? curve : *curve.find("keys");
                float last = -1.0F;
                for (const json::Value& key : keys.as_array()) {
                    const auto time = static_cast<float>(key.as_array()[0].as_double());
                    if (time < last) {
                        errors.push_back({at + "/curves/" + json::pointer_escape(path),
                                          "keys must go in order of time"});
                        break;
                    }
                    last = time;
                }
                if (split(path).size() < 2) {
                    errors.push_back({at + "/curves/" + json::pointer_escape(path),
                                      "a path names a component and a field, such as "
                                      "OmniLight3D/light_energy"});
                }
            }
        }
        expressions(layer, at, found);
    }
    if (const json::Value* lod = doc.find("lod"); lod != nullptr) {
        float distance = -1.0F;
        float detail = 2.0F;
        const json::Array& levels = lod->as_array();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto d = static_cast<float>(levels[i].find("distance")->as_double());
            const auto x = static_cast<float>(levels[i].find("detail")->as_double());
            if (d <= distance || x > detail) {
                errors.push_back({"/lod/" + std::to_string(i),
                                  "levels go nearest first, each with no more detail than the "
                                  "one before"});
            }
            distance = d;
            detail = x;
        }
    }
    for (const auto& [at, text] : found) {
        if (auto parsed = Expression::parse(text); !parsed) {
            errors.push_back({at, parsed.error().message});
        }
    }
    return errors;
}

Result<VfxRecipe> parse_vfx(const json::Value& doc) {
    if (const std::vector<json::ValidationError> errors = validate_vfx(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors[0].path.empty() ? "" : errors[0].path + ": ") +
                              errors[0].message);
    }
    VfxRecipe recipe;
    if (const json::Value* name = doc.find("name"); name != nullptr) {
        recipe.name = name->as_string();
    }
    if (const json::Value* dimension = doc.find("dimension"); dimension != nullptr) {
        recipe.dimension = dimension->as_string();
    }
    recipe.duration = number(doc.find("duration"), 1.0F);
    if (const json::Value* loop = doc.find("loop"); loop != nullptr) {
        recipe.loop = loop->as_bool();
    }
    const json::Value* parameters = doc.find("parameters");
    recipe.parameters = parameters != nullptr ? *parameters : json::Value(json::Object{});
    for (const json::Value& item : doc.find("layers")->as_array()) {
        VfxLayer layer;
        layer.name = item.find("name")->as_string();
        layer.start = number(item.find("start"), 0.0F);
        layer.duration = number(item.find("duration"), 0.0F);
        if (const json::Value* offset = item.find("offset"); offset != nullptr) {
            layer.offset = *offset;
        }
        if (const json::Value* rotation = item.find("rotation"); rotation != nullptr) {
            layer.rotation = *rotation;
        }
        layer.components = *item.find("components");
        if (const json::Value* curves = item.find("curves"); curves != nullptr) {
            for (const auto& [path, curve] : curves->as_object()) {
                VfxCurve out;
                out.path = path;
                if (curve.is_array()) {
                    out.keys = parse_keys(curve);
                } else {
                    out.keys = parse_keys(*curve.find("keys"));
                    if (const json::Value* ease = curve.find("ease"); ease != nullptr) {
                        out.ease = ease->as_string();
                    }
                }
                layer.curves.push_back(std::move(out));
            }
        }
        layer.detail = number(item.find("detail"), 0.0F);
        if (const json::Value* world = item.find("world"); world != nullptr) {
            layer.world = world->as_bool();
        }
        recipe.layers.push_back(std::move(layer));
    }
    if (const json::Value* lod = doc.find("lod"); lod != nullptr) {
        for (const json::Value& level : lod->as_array()) {
            recipe.lod.push_back({.distance = number(level.find("distance"), 0.0F),
                                  .detail = number(level.find("detail"), 1.0F)});
        }
    }
    if (const json::Value* budget = doc.find("budget"); budget != nullptr) {
        const auto count = [&](std::string_view name) {
            const json::Value* value = budget->find(name);
            return value != nullptr ? static_cast<int>(value->as_int()) : 0;
        };
        recipe.budget = {.particles = count("particles"),
                         .lights = count("lights"),
                         .sounds = count("sounds"),
                         .decals = count("decals"),
                         .layers = count("layers")};
    }
    return recipe;
}

Result<std::shared_ptr<const VfxRecipe>> load_vfx(const World& world, std::string_view path) {
    if (!is_vfx_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .vfx.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    Result<std::string> text =
        make_error(ErrorCode::PlatformFailure, "the world has no files to read effects from");
    if (assets != nullptr) {
        text = assets->read(path, "vfx");
    } else if (world.file_reader()) {
        text = world.file_reader()(path);
    }
    if (!text) {
        return text.error();
    }
    VfxCache* cache = nullptr;
    if (assets != nullptr) {
        auto& ctx =
            const_cast<World&>(world).ecs().ctx(); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (ctx.find<VfxCacheHolder>() == nullptr) {
            ctx.emplace<VfxCacheHolder>(std::make_shared<VfxCache>());
        }
        cache = ctx.get<VfxCacheHolder>().get();
    }
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->entries.find(std::string(path));
            found != cache->entries.end() && found->second.text == text.value()) {
            return found->second.recipe;
        }
    }
    auto doc = json::parse(text.value());
    if (!doc) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(path) + ": " + doc.error().message);
    }
    auto recipe = parse_vfx(doc.value());
    if (!recipe) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(path) + ": " + recipe.error().message);
    }
    auto shared = std::make_shared<const VfxRecipe>(std::move(recipe).value());
    if (cache != nullptr) {
        cache->entries[std::string(path)] = {.text = text.value(), .recipe = shared};
    }
    return shared;
}

json::Value vfx_parameters(const VfxRecipe& recipe, const json::Value& own) {
    json::Value out =
        recipe.parameters.is_object() ? recipe.parameters : json::Value(json::Object{});
    if (own.is_object()) {
        for (const auto& [name, value] : own.as_object()) {
            out[name] = value;
        }
    }
    return out;
}

Result<json::Value> vfx_value(const json::Value& value, const json::Value& parameters, float detail,
                              const json::Value& extra) {
    if (value.is_string() && value.as_string().starts_with('=')) {
        auto parsed = Expression::parse(std::string_view(value.as_string()).substr(1));
        if (!parsed) {
            return parsed.error();
        }
        auto result =
            parsed.value().execute([&](std::string_view name) -> std::optional<json::Value> {
                if (const json::Value* found = extra.find(name); found != nullptr) {
                    return *found;
                }
                if (const json::Value* found = parameters.find(name); found != nullptr) {
                    return *found;
                }
                if (name == "detail") {
                    return json::Value(static_cast<double>(detail));
                }
                return std::nullopt;
            });
        if (!result) {
            return make_error(result.error().code,
                              "\"" + value.as_string() + "\": " + result.error().message);
        }
        return tidy_number(std::move(result).value());
    }
    if (value.is_array()) {
        json::Array out;
        for (const json::Value& item : value.as_array()) {
            auto worked = vfx_value(item, parameters, detail, extra);
            if (!worked) {
                return worked.error();
            }
            out.push_back(std::move(worked).value());
        }
        return json::Value(std::move(out));
    }
    if (value.is_object()) {
        json::Value out = json::Object{};
        for (const auto& [key, item] : value.as_object()) {
            auto worked = vfx_value(item, parameters, detail, extra);
            if (!worked) {
                return worked.error();
            }
            out[key] = std::move(worked).value();
        }
        return out;
    }
    return value;
}

Result<json::Value> vfx_components(const VfxLayer& layer, const json::Value& parameters,
                                   float detail) {
    auto worked = vfx_value(layer.components, parameters, detail);
    if (!worked) {
        return make_error(worked.error().code,
                          "layer " + layer.name + ": " + worked.error().message);
    }
    json::Value out = std::move(worked).value();
    for (const std::string_view name : kEmitters) {
        json::Value* emitter = out.find(name);
        if (emitter == nullptr || detail >= 1.0F) {
            continue;
        }
        const json::Value* amount = emitter->find("amount");
        const float full = amount != nullptr && amount->is_number()
                               ? static_cast<float>(amount->as_double())
                               : 8.0F;
        (*emitter)["amount"] =
            static_cast<std::int64_t>(std::max(1.0F, std::floor((full * detail) + 0.5F)));
    }
    return out;
}

Result<json::Value> vfx_curve_value(const VfxCurve& curve, float t, float length,
                                    const json::Value& parameters, float detail) {
    json::Value extra = json::Object{};
    extra["t"] = static_cast<double>(t);
    extra["life"] = static_cast<double>(length > 0.0F ? std::clamp(t / length, 0.0F, 1.0F) : 0.0F);
    const auto value_at = [&](std::size_t index) {
        return vfx_value(curve.keys[index].value, parameters, detail, extra);
    };
    if (curve.keys.empty()) {
        return json::Value{};
    }
    if (t <= curve.keys.front().time || curve.keys.size() == 1) {
        return value_at(0);
    }
    if (t >= curve.keys.back().time) {
        return value_at(curve.keys.size() - 1);
    }
    std::size_t next = 1;
    while (next < curve.keys.size() && curve.keys[next].time <= t) {
        ++next;
    }
    const std::size_t prev = next - 1;
    auto a = value_at(prev);
    if (!a) {
        return a.error();
    }
    if (curve.ease == "step") {
        return a;
    }
    auto b = value_at(next);
    if (!b) {
        return b.error();
    }
    const float span = curve.keys[next].time - curve.keys[prev].time;
    float w = span > 0.0F ? (t - curve.keys[prev].time) / span : 1.0F;
    if (curve.ease == "smooth") {
        w = w * w * (3.0F - (2.0F * w));
    }
    // Numbers, and arrays of numbers (colors, vectors), blend; anything else steps.
    const auto blend = [w](const json::Value& x, const json::Value& y) -> json::Value {
        const double from = x.as_double();
        const double to = y.as_double();
        return from + ((to - from) * static_cast<double>(w));
    };
    if (a.value().is_number() && b.value().is_number()) {
        return blend(a.value(), b.value());
    }
    if (a.value().is_array() && b.value().is_array() && a.value().size() == b.value().size()) {
        json::Array out;
        for (std::size_t i = 0; i < a.value().size(); ++i) {
            const json::Value& x = a.value().as_array()[i];
            const json::Value& y = b.value().as_array()[i];
            if (!x.is_number() || !y.is_number()) {
                return a;
            }
            out.emplace_back(blend(x, y));
        }
        return json::Value(std::move(out));
    }
    return a;
}

float vfx_layer_length(const VfxRecipe& recipe, const VfxLayer& layer) {
    const float left = std::max(recipe.duration - layer.start, 0.0F);
    return layer.duration > 0.0F ? std::min(layer.duration, left) : left;
}

float vfx_detail(const VfxRecipe& recipe, float distance) {
    float detail = 1.0F;
    for (const VfxLod& level : recipe.lod) {
        if (distance >= level.distance) {
            detail = level.detail;
        }
    }
    return detail;
}

Result<VfxCost> vfx_cost(const VfxRecipe& recipe, const json::Value& parameters, float detail) {
    struct LayerCost {
        float start = 0.0F;
        float end = 0.0F;
        VfxCost cost;
    };
    std::vector<LayerCost> costs;
    for (const VfxLayer& layer : recipe.layers) {
        if (layer.detail > detail) {
            continue;
        }
        auto components = vfx_components(layer, parameters, detail);
        if (!components) {
            return components.error();
        }
        LayerCost out{.start = layer.start,
                      .end = layer.start + vfx_layer_length(recipe, layer),
                      .cost = {.layers = 1}};
        for (const auto& [name, fields] : components.value().as_object()) {
            if (is_one_of(name, kEmitters)) {
                const json::Value* amount = fields.find("amount");
                out.cost.particles += amount != nullptr && amount->is_number()
                                          ? static_cast<int>(amount->as_double())
                                          : 8;
            } else if (is_one_of(name, kLights)) {
                ++out.cost.lights;
            } else if (is_one_of(name, kSounds)) {
                ++out.cost.sounds;
            } else if (name == "Decal") {
                ++out.cost.decals;
            }
        }
        costs.push_back(out);
    }
    VfxCost busiest;
    for (const LayerCost& at : costs) {
        VfxCost now;
        for (const LayerCost& other : costs) {
            if (other.start <= at.start && at.start < std::max(other.end, other.start + 1e-4F)) {
                now.particles += other.cost.particles;
                now.lights += other.cost.lights;
                now.sounds += other.cost.sounds;
                now.decals += other.cost.decals;
                now.layers += other.cost.layers;
            }
        }
        busiest.particles = std::max(busiest.particles, now.particles);
        busiest.lights = std::max(busiest.lights, now.lights);
        busiest.sounds = std::max(busiest.sounds, now.sounds);
        busiest.decals = std::max(busiest.decals, now.decals);
        busiest.layers = std::max(busiest.layers, now.layers);
    }
    return busiest;
}

std::vector<std::string> vfx_over_budget(const VfxRecipe& recipe, const VfxCost& cost) {
    std::vector<std::string> out;
    const auto check = [&](std::string_view what, int spent, int limit) {
        if (limit > 0 && spent > limit) {
            out.push_back(std::string(what) + ": " + std::to_string(spent) + " of " +
                          std::to_string(limit));
        }
    };
    check("particles", cost.particles, recipe.budget.particles);
    check("lights", cost.lights, recipe.budget.lights);
    check("sounds", cost.sounds, recipe.budget.sounds);
    check("decals", cost.decals, recipe.budget.decals);
    check("layers", cost.layers, recipe.budget.layers);
    return out;
}

void add_vfx_system(World& world) {
    world.add_reset([](World& w) {
        if (auto* held = w.ecs().ctx().find<VfxStateHolder>(); held != nullptr) {
            **held = VfxState{};
        }
    });
    world.add_system("vfx", [](World& w, double tick_seconds) { run(w, tick_seconds); },
                     {.doc = "Plays each VisualEffect3D's and VisualEffect2D's recipe, its layers "
                             "as entities under it, and shakes the current cameras by "
                             "CameraShakes.",
                      .priority = 1090});
}

entt::entity vfx_layer_entity(const World& world, entt::entity effect, std::string_view layer) {
    const std::string_view id = world.id_of(effect);
    for (const auto [entity, part] : world.ecs().view<const VfxPart>().each()) {
        if (part.effect == id && part.layer == layer && part.linger < 0.0F) {
            return entity;
        }
    }
    return entt::null;
}

json::Value vfx_hooks_schema() {
    return json::parse(R"({"type": "object",
        "description": "Effects it plays, by the world event that plays them: each key an event kind about it (such as creature_attacked, vehicle_hit, killed or foliage_harvested; a vehicle's running plays while its throttle is open), each value a .vfx.json file.",
        "additionalProperties": {"type": "string", "pattern": "\\.vfx\\.json$"}})")
        .value();
}

VfxHooks read_vfx_hooks(const json::Value& doc) {
    VfxHooks out;
    const json::Value* hooks = doc.is_object() ? doc.find("vfx") : nullptr;
    if (hooks == nullptr || !hooks->is_object()) {
        return out;
    }
    for (const auto& [kind, recipe] : hooks->as_object()) {
        if (recipe.is_string()) {
            out.emplace(kind, recipe.as_string());
        }
    }
    return out;
}

VfxHooks vfx_hooks_of(const World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (!world.valid(entity)) {
        return {};
    }
    const auto hooks = [](const auto& loaded) { return loaded ? loaded.value()->vfx : VfxHooks{}; };
    if (const auto* c = ecs.try_get<Creature3D>(entity); c != nullptr && !c->recipe.empty()) {
        return hooks(load_creature(world, c->recipe));
    }
    if (const auto* c = ecs.try_get<Creature2D>(entity); c != nullptr && !c->recipe.empty()) {
        return hooks(load_creature(world, c->recipe));
    }
    if (const auto* v = ecs.try_get<Vehicle3D>(entity); v != nullptr && !v->recipe.empty()) {
        return hooks(load_vehicle(world, v->recipe));
    }
    if (const auto* v = ecs.try_get<Vehicle2D>(entity); v != nullptr && !v->recipe.empty()) {
        return hooks(load_vehicle(world, v->recipe));
    }
    if (const auto* c = ecs.try_get<Character2D>(entity); c != nullptr && !c->recipe.empty()) {
        return hooks(load_character(world, c->recipe));
    }
    if (const auto* m = ecs.try_get<MeshInstance3D>(entity);
        m != nullptr && is_character_path(m->mesh)) {
        return hooks(load_character(world, m->mesh));
    }
    return {};
}

VfxHooks foliage_vfx_hooks(const World& world, std::string_view path) {
    if (!is_foliage_path(path)) {
        return {};
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return {};
    }
    const auto doc = assets->load<json::Value>(path);
    if (!doc) {
        return {};
    }
    return read_vfx_hooks(*doc.value());
}

entt::entity play_vfx(World& world, std::string_view recipe, std::string_view near,
                      std::string_view what, Vec3 position, bool flat) {
    if (recipe.empty()) {
        return entt::null;
    }
    const std::string base =
        std::string(near.empty() ? "vfx" : near) + "_" + std::string(what.empty() ? "vfx" : what);
    entt::entity made = entt::null;
    for (std::size_t n = 1; n < 10000 && made == entt::null; ++n) {
        const std::string id = n == 1 ? base : base + "_" + std::to_string(n);
        if (world.find(id) == entt::null) {
            auto created = world.create(id);
            if (!created) {
                return entt::null;
            }
            made = created.value();
        }
    }
    if (made == entt::null) {
        return entt::null;
    }
    auto& ecs = world.ecs();
    if (flat) {
        Transform2D placed;
        placed.position = {position.x, position.y};
        ecs.emplace<Transform2D>(made, placed);
        VisualEffect2D effect;
        effect.recipe = std::string(recipe);
        effect.free_on_finish = true;
        ecs.emplace<VisualEffect2D>(made, std::move(effect));
    } else {
        Transform3D placed;
        placed.position = position;
        ecs.emplace<Transform3D>(made, placed);
        VisualEffect3D effect;
        effect.recipe = std::string(recipe);
        effect.free_on_finish = true;
        ecs.emplace<VisualEffect3D>(made, std::move(effect));
    }
    return made;
}

CameraShakeOffsets camera_shake_offsets(const World& world) {
    const auto* held = world.ecs().ctx().find<VfxStateHolder>();
    if (held == nullptr) {
        return {};
    }
    return {.camera_3d = (*held)->applied_3d, .camera_2d = (*held)->applied_2d};
}

} // namespace slash::world
