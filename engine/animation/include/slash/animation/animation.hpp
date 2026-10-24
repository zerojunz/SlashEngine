#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <map>
#include <memory>
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
//           {"event": "footstep", "keys": [{"time": 0.25}]},
//           {"target": "sword", "path": "Transform2D/rotation",
//            "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 1.5}]}
//         ]
//       }
//     },
//     "format": 1
//   }
//
// A value track's path is a component and one of its fields, then optionally a part of the
// field: "Transform2D/position", "Transform2D/position/y", "Sprite2D/modulate/a". Its keys are
// values that field takes. An event track emits a world event when playback passes each key,
// with the key's "data". A track with a "target" acts on the entity the AnimationPlayer's
// targets map that name to, as a Godot track's node path does, rather than the player's own.
//
// An animation can come "from" a model file's, as Godot imports a glTF file's animations into an
// AnimationLibrary: it moves the nodes of the model the entity's MeshInstance3D draws, takes the
// model animation's length and loop unless it gives its own, and adds its tracks, here a footstep:
//
//   "walk": {"from": {"model": "models/hero.glb", "animation": "Walk"},
//            "tracks": [{"event": "footstep", "keys": [{"time": 0.4}, {"time": 0.9}]}]}
//
// An AnimationPlayer's library can also be a model file itself, whose animations it plays as
// they are.
//
// A node track moves a node of the model the entity's MeshInstance3D draws, by its name, as a
// Godot position_3d, rotation_3d or scale_3d track moves a bone: its keys' values are positions,
// rotations (Euler angles in radians, as Transform3D's) or scales, which turn the shorter way
// round between keys, as Godot's rotation tracks do:
//
//   "wave": {"length": 1, "loop": "linear",
//            "tracks": [{"node": "Elbow", "property": "rotation",
//                        "keys": [{"time": 0, "value": [0, 0, 0]},
//                                 {"time": 0.5, "value": [0, 0, 0.8]}]}]}
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
    std::string path;   // a value track's "Component/field[/part...]"
    std::string event;  // an event track's event kind
    std::string node;   // a node track's model node
    std::string target; // a name in AnimationPlayer::targets; "" for the player's own entity
    // What a node track moves; its keys' values are [x, y, z].
    scene3d::NodeProperty property = scene3d::NodeProperty::Position;
    Interpolation interpolation = Interpolation::Linear;
    UpdateMode update = UpdateMode::Auto;
    // In a looping animation, move from the last key to the first across the loop's end.
    bool loop_wrap = true;
    std::vector<Key> keys; // by time, earliest first
};

// A model file's animation that an animation plays on the nodes of the entity's model.
struct ModelClip {
    std::string model;     // a project path; "" for none
    std::string animation; // its name in the model
};

struct Animation {
    double length = 1.0; // seconds
    LoopMode loop = LoopMode::None;
    std::vector<Track> tracks;
    ModelClip from;
    // Whether length and loop were written, or are to come from the model's animation.
    bool own_length = true;
    bool own_loop = true;
};

struct AnimationLibrary {
    std::map<std::string, Animation, std::less<>> animations;
};

[[nodiscard]] std::string_view loop_mode_name(LoopMode mode);
[[nodiscard]] std::string_view interpolation_name(Interpolation interpolation);
[[nodiscard]] std::string_view node_property_name(scene3d::NodeProperty property);

// Whether an animation has node tracks.
[[nodiscard]] bool has_node_tracks(const Animation& animation);

// The node tracks of a library's animations as the animations of a model with no meshes, whose
// nodes are those the tracks name: how they pose the model an entity draws
// (scene3d::ClipSample::source), each animation with node tracks by its name. Null for a library
// with none.
[[nodiscard]] std::shared_ptr<const scene3d::Model>
node_animations(const AnimationLibrary& library);

// A node track's value at a time, from 0 to the animation's length, as [x, y, z]: a rotation as
// Euler angles in radians.
[[nodiscard]] json::Value sample_node(const Animation& animation, const Track& track, double time);

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

// Whether a library path names a model file, whose animations an AnimationPlayer plays as they are.
[[nodiscard]] bool is_model_library(std::string_view path);

// A model file's animations as a library: each from the model at path, with its length and its
// loop (linear when the model's animation loops).
[[nodiscard]] AnimationLibrary model_library(const scene3d::Model& model, std::string_view path);

// Gives an animation from a model the length and loop its model's animation has, where it gives
// none of its own; the error says the model has no such animation, with "did you mean".
[[nodiscard]] Result<void> resolve_clip(Animation& animation, const scene3d::Model& model);

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

// Every error in a scene's or prefab's AnimationPlayers, each with the JSON Pointer of the bad
// value: a library that doesn't load, an animation it doesn't have, with "did you mean", a track
// target the player's targets leave out, and a node track's node the entity's model hasn't.
// Libraries and models are read through reader.
[[nodiscard]] std::vector<json::ValidationError> check_players(const json::Value& doc,
                                                               const world::FileReader& reader);

// Asset types "animation" (.anim.json files as AnimationLibrary) and "animation_tree"
// (.animtree.json files as AnimationTreeResource, in tree.hpp).
void add_asset_types(asset::Assets& assets);

// The "animation_player" system, which plays each AnimationPlayer's animation, the
// "animation_tree" system, which blends each AnimationTree's (tree.hpp), and the clips that pose
// models (scene3d::set_clip_source): a tree's animations from models, or else a player's, playing
// or paused, pose its entity's MeshInstance3D model. They read .anim.json, .animtree.json and
// model files through assets, which must outlive the world.
void add_systems(world::World& world, asset::Assets& assets);

// Both of the above.
void install(world::World& world, asset::Assets& assets);

} // namespace slash::animation
