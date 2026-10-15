#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Keyframe animation of any component field, as Godot's AnimationPlayer: .anim.json files hold
// named animations, like an AnimationLibrary of Animation resources, and the AnimationPlayer
// component plays one of them on its entity.
//
//   {
//     "animations": {
//       "bob": {
//         "length": 1,
//         "loop": "linear",
//         "tracks": [
//           {"interpolation": "cubic", "path": "Transform2D/position/y",
//            "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": -8}]},
//           {"event": "footstep", "keys": [{"time": 0.25}]}
//         ]
//       }
//     },
//     "format": 1
//   }
//
// A value track's path is a component and one of its fields, then optionally a part of the
// field: "Transform2D/position", "Transform2D/position/y", "Sprite2D/modulate/a". Its keys are
// values that field takes. An event track emits a world event when playback passes each key,
// with the key's "data".
namespace slash::animation {

inline constexpr std::string_view kAnimationSuffix = ".anim.json";

// What happens at the end, as Godot's loop modes.
enum class LoopMode : std::uint8_t { None, Linear, PingPong };

// How values between keys are found, as Godot's interpolation types. The angle types turn the
// short way round, for rotations in radians.
enum class Interpolation : std::uint8_t { Nearest, Linear, Cubic, LinearAngle, CubicAngle };

// Whether a value track changes smoothly or jumps from key to key. Auto, the default, is
// continuous for float fields and discrete for others: integers, strings, booleans and entity
// references, as Godot's editor sets keys of those up.
enum class UpdateMode : std::uint8_t { Auto, Continuous, Discrete };

struct Key {
    double time = 0.0;
    // A value track's value, or an event track's data.
    json::Value value;
    // Godot's easing curve on the way to the next key: 1 is linear, above 1 eases in, between 0
    // and 1 eases out, below 0 eases in and out, and 0 holds this key's value until the next.
    double transition = 1.0;
};

struct Track {
    std::string path;  // a value track's "Component/field[/part...]"
    std::string event; // an event track's event kind
    Interpolation interpolation = Interpolation::Linear;
    UpdateMode update = UpdateMode::Auto;
    // In a looping animation, move from the last key to the first across the loop's end.
    bool loop_wrap = true;
    std::vector<Key> keys; // by time, earliest first
};

struct Animation {
    double length = 1.0; // seconds
    LoopMode loop = LoopMode::None;
    std::vector<Track> tracks;
};

struct AnimationLibrary {
    std::map<std::string, Animation, std::less<>> animations;
};

[[nodiscard]] std::string_view loop_mode_name(LoopMode mode);
[[nodiscard]] std::string_view interpolation_name(Interpolation interpolation);

// JSON Schema for .anim.json files.
[[nodiscard]] json::Value animation_schema();

// Every error in a .anim.json document, each with the JSON Pointer of the bad value. Given the
// component types, value tracks are checked against them too: an unknown component or field,
// with "did you mean", and a key value the field doesn't take.
[[nodiscard]] std::vector<json::ValidationError>
validate_animation_library(const json::Value& doc,
                           const world::ComponentRegistry* components = nullptr);

// The animations in a .anim.json document, or its first error.
[[nodiscard]] Result<AnimationLibrary> parse_animation_library(const json::Value& doc);

// Where a value track's path leads: a component's field and the part of it.
struct Target {
    const world::ComponentType* component = nullptr;
    std::string field;
    std::vector<std::string> part; // the rest of the path, as JSON Pointer tokens
    json::Value schema;            // what the value must be, with the component schema's $defs
    bool discrete = false;         // what UpdateMode::Auto means for it
};

// Resolves a value track's path against the component types; the error says what is wrong,
// with "did you mean".
[[nodiscard]] Result<Target> resolve_target(std::string_view path,
                                            const world::ComponentRegistry& components);

// A value track's value at a time, from 0 to the animation's length.
[[nodiscard]] json::Value sample(const Animation& animation, const Track& track, double time,
                                 bool discrete);

// Sets a target's value on an entity, adding the component if the entity has none.
[[nodiscard]] Result<void> apply(world::World& world, entt::entity entity, const Target& target,
                                 const json::Value& value);

// Asset type "animation" (.anim.json files as AnimationLibrary).
void add_asset_types(asset::Assets& assets);

// The "animation_player" system, which plays each AnimationPlayer's animation. It reads
// .anim.json files through assets, which must outlive the world.
void add_systems(world::World& world, asset::Assets& assets);

// Both of the above.
void install(world::World& world, asset::Assets& assets);

} // namespace slash::animation
