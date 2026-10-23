#pragma once

#include "slash/animation/animation.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Blending animations, as Godot's AnimationTree: a .animtree.json file holds a tree of animation
// nodes, and the AnimationTree component plays it on its entity, with the animations of its
// libraries. A blend tree connects nodes that blend their inputs; blend spaces blend the points
// nearest a position; a Transition cross-fades between its inputs and a OneShot plays one over
// another. A state machine plays one state at a time and moves between them through transitions,
// on their own or by the shortest way to a state callers ask for. Each tick the tree works out how
// much each animation counts, track by track, and the animations are mixed into the entity's
// components and the pose of its model.
//
//   {
//     "format": 1,
//     "root": {
//       "type": "BlendTree",
//       "nodes": {
//         "walk": {"type": "Animation", "animation": "Walk"},
//         "run": {"type": "Animation", "animation": "Run"},
//         "speed": {"type": "Blend2", "inputs": {"in": "walk", "blend": "run"}},
//         "wave": {"type": "Animation", "animation": "Wave"},
//         "shot": {"type": "OneShot", "inputs": {"in": "speed", "shot": "wave"},
//                  "fadein_time": 0.2, "fadeout_time": 0.2,
//                  "filter_enabled": true, "filters": ["Elbow", "Hand"]}
//       },
//       "output": "shot"
//     }
//   }
//
// Node types are Godot's AnimationNode classes without the prefix, with their properties'
// names. A node's parameters, which the AnimationTree component keeps, are named by the path of
// node names to it, then the parameter, as Godot's without "parameters/": "speed/blend_amount",
// "shot/request". A blend space's points are named by their index, a state machine's states by
// their names: "move/travel", "move/run/blend_position".
namespace slash::animation {

inline constexpr std::string_view kTreeSuffix = ".animtree.json";

enum class TreeNodeType : std::uint8_t {
    Animation,
    BlendTree,
    BlendSpace1D,
    BlendSpace2D,
    OneShot,
    Add2,
    Add3,
    Blend2,
    Blend3,
    Sub2,
    TimeScale,
    TimeSeek,
    Transition,
    StateMachine,
    Output, // a blend tree's output, made by the blend tree
};

enum class PlayMode : std::uint8_t { Forward, Backward };
// How a blend space blends: the points around the position, or only the closest one, starting
// it over, or carrying on from where the last one was.
enum class BlendMode : std::uint8_t { Interpolated, Discrete, DiscreteCarry };
enum class MixMode : std::uint8_t { Blend, Add };

// A curve of [offset, value] points from offset 0 to 1, joined by straight lines, as particles'
// curves; empty is the line from [0, 0] to [1, 1].
using TreeCurve = std::vector<std::array<double, 2>>;

// A root state machine starts over when the tree does; a nested one carries on, starting over
// only once it ended; a grouped one is a state of its parent, which plays it as part of itself.
enum class StateMachineType : std::uint8_t { Root, Nested, Grouped };
// When a transition happens: at once, at once with the next state going on from where the last
// one was, or when the current state ends.
enum class SwitchMode : std::uint8_t { Immediate, Sync, AtEnd };
// Whether a transition can be taken: never, by travel, or by travel and on its own.
enum class AdvanceMode : std::uint8_t { Disabled, Enabled, Auto };

// One of a state machine's states: a node at a position, or its Start or End, which play nothing.
struct MachineState {
    std::string name;
    int node = -1; // -1 for Start and End
    Vec2 position;
};

// A transition between two of a state machine's states, as Godot's
// AnimationNodeStateMachineTransition.
struct MachineTransition {
    std::string from;
    std::string to;
    std::string advance_condition; // an auto transition waits for "conditions/<it>" to be true
    double xfade_time = 0.0;
    TreeCurve xfade_curve;
    int priority = 1; // lower goes first when several can be taken; travel counts it as cost
    SwitchMode switch_mode = SwitchMode::Immediate;
    AdvanceMode advance_mode = AdvanceMode::Enabled;
    bool break_loop_at_end = false; // with at_end, end a looping state at the end of its loop
    bool reset = true;              // start the next state over
};

// One of a Transition's inputs.
struct TransitionInput {
    std::string name;
    bool auto_advance = false;      // go on to the next input when this one ends
    bool break_loop_at_end = false; // and end a looping one at the end of its loop
    bool reset = true;              // start it over when it is switched to
};

// One node of a tree. Fields of 8 bytes come first and the small ones last, so it packs; each
// group says which node types use it.
struct TreeNode {
    std::string name; // in its parent: a blend tree's name for it or a point's index; "" for root
    std::string path; // its parameters' prefix: "" for the root, then "speed/", "space/0/"
    // A blend tree's nodes, its output last, a blend space's points or a state machine's states'
    // nodes.
    std::vector<int> children;
    // In a blend tree, the node connected to each of this one's inputs.
    std::vector<int> inputs;

    // Animation
    std::string animation; // "Walk", or "library/Walk" for one from another library
    double timeline_length = 1.0;
    double start_offset = 0.0;

    // OneShot, Add2, Add3, Blend2 and Sub2: the tracks the blend applies to, by track name.
    std::vector<std::string> filters;

    // OneShot
    double fadein_time = 0.0;
    double fadeout_time = 0.0;
    TreeCurve fadein_curve;
    TreeCurve fadeout_curve;
    double autorestart_delay = 1.0;
    double autorestart_random_delay = 0.0;

    // Transition
    double xfade_time = 0.0;
    TreeCurve xfade_curve;
    std::vector<TransitionInput> transitions; // one per input

    // BlendSpace1D (x only) and BlendSpace2D
    std::vector<Vec2> positions; // each point's
    std::string x_label = "x";   // BlendSpace1D's value_label
    std::string y_label = "y";
    std::vector<std::array<int, 3>> triangles; // points by index, each sorted

    // StateMachine
    std::vector<MachineState> states; // by name, Start and End among them
    std::vector<MachineTransition> machine_transitions;

    int parent = -1;
    // BlendSpace1D (x only) and BlendSpace2D
    Vec2 min_space{-1.0F, -1.0F};
    Vec2 max_space{1.0F, 1.0F};
    Vec2 snap{0.1F, 0.1F};

    TreeNodeType type = TreeNodeType::Animation;

    // BlendSpace1D and BlendSpace2D
    BlendMode blend_mode = BlendMode::Interpolated;
    bool auto_triangles = true;

    // Animation
    PlayMode play_mode = PlayMode::Forward;
    bool advance_on_start = false;
    bool use_custom_timeline = false;
    bool stretch_time_scale = true;
    LoopMode loop_mode = LoopMode::None;

    // OneShot, Add2, Add3, Blend2 and Sub2
    bool filter_enabled = false;
    bool sync = false; // keep inputs that don't count playing

    // OneShot
    bool break_loop_at_end = false;
    bool autorestart = false;
    MixMode mix_mode = MixMode::Blend;

    // TimeSeek
    bool explicit_elapse = true;

    // Transition and StateMachine
    bool allow_transition_to_self = false;

    // StateMachine
    StateMachineType state_machine_type = StateMachineType::Root;
    bool reset_ends = false; // count Start and End as states that play nothing
};

// A .animtree.json file: its nodes, the root first.
struct AnimationTreeResource {
    std::vector<TreeNode> nodes;
};

[[nodiscard]] std::string_view tree_node_type_name(TreeNodeType type);
// The names of a node type's inputs, as Godot's: Blend2's "in" and "blend".
[[nodiscard]] std::vector<std::string> tree_node_input_names(const TreeNode& node);
// Whether a node type has filters, as Godot's has_filter.
[[nodiscard]] bool tree_node_has_filter(TreeNodeType type);

// JSON Schema for .animtree.json files.
[[nodiscard]] json::Value tree_schema();

// Every error in a .animtree.json document, each with the JSON Pointer of the bad value: unknown
// types and fields, inputs connected to nothing or to two nodes, loops, bad blend space points.
[[nodiscard]] std::vector<json::ValidationError> validate_tree(const json::Value& doc);

// The tree in a .animtree.json document, or its first error.
[[nodiscard]] Result<AnimationTreeResource> parse_tree(const json::Value& doc);

// A node's parameter, as Godot's AnimationNode parameters.
struct TreeParameter {
    std::string key;  // its node's path then its name: "speed/blend_amount"
    std::string name; // "blend_amount"
    int node = 0;
    json::Value default_value;
    // Set by callers, in AnimationTree.parameters, rather than by the tree as it plays, in
    // AnimationTree.state.
    bool input = false;
    std::string doc;
};

// Every parameter of a tree's nodes, the root's first.
[[nodiscard]] std::vector<TreeParameter> tree_parameters(const AnimationTreeResource& tree);

// The error in a value for an input parameter, or "" for none: a number where one is wanted, a
// OneShot request of "none", "fire", "abort" or "fade_out", a Transition request naming one of
// its inputs or a state machine's travel or start naming one of its states, with "did you mean",
// a 2D blend position of [x, y].
[[nodiscard]] std::string check_parameter(const AnimationTreeResource& tree,
                                          const TreeParameter& parameter, const json::Value& value);

// The track names a filter can list for an animation: a model node's name for the tracks that
// move it, a value track's path, an event track's event after "@", each after its target and
// ":" if it has one, as "sword:Transform2D/rotation".
[[nodiscard]] std::string value_track_name(const Track& track);
[[nodiscard]] std::string event_track_name(const Track& track);

// Every error in a scene's or prefab's AnimationTrees, each with the JSON Pointer of the bad
// value: a tree or library that doesn't load, an animation the libraries haven't, with "did you
// mean", a root motion track no animation moves and parameters the tree hasn't. Files are read
// through reader.
[[nodiscard]] std::vector<json::ValidationError> check_trees(const json::Value& doc,
                                                             const world::FileReader& reader);

} // namespace slash::animation
