#pragma once

// One tick of an animation tree's nodes, as Godot's AnimationTree processes its AnimationNodes:
// each node works out its inputs' weights and times and passes them on, down to the animations,
// which the mixer then blends (tree_system.cpp).

#include "internal.hpp"

#include "slash/animation/tree.hpp"
#include "slash/core/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace slash::animation::detail {

// Godot's AnimationMixer::PlaybackInfo.
struct PlaybackInfo {
    double time = 0.0;
    double delta = 0.0;
    double start = 0.0;
    double end = 0.0;
    bool seeked = false;
    bool external = false; // is_external_seeking
    LoopedFlag looped = LoopedFlag::None;
    float weight = 0.0F;
};

// An animation the tree plays this tick, as Godot's AnimationMixer::AnimationInstance: how far in
// it is and how much each of the tree's tracks counts.
struct AnimationInstance {
    std::string name; // in the tree: "Walk", or "library/Walk"
    const Animation* animation = nullptr;
    PlaybackInfo info;
    std::vector<float> track_weights;
};

// The animation_started and animation_finished signals a tick emits, in order.
struct TreeSignal {
    std::string kind;
    std::string animation;
};

struct TreeTick {
    std::vector<AnimationInstance> instances;
    std::vector<TreeSignal> signals;
    std::string invalid; // why the tree plays nothing this tick; "" when it plays
};

// A tree's parameters as it plays: those callers set (AnimationTree.parameters) and those it
// keeps (AnimationTree.state), each left out while it has its default value.
class TreeParameters {
public:
    TreeParameters(const std::vector<TreeParameter>& list, json::Object& inputs,
                   json::Object& state);

    [[nodiscard]] json::Value get(const TreeNode& node, std::string_view name) const;
    void set(const TreeNode& node, std::string_view name, json::Value value);

private:
    std::map<std::string, const TreeParameter*, std::less<>> by_key_;
    json::Object& inputs_;
    json::Object& state_;
};

struct TreeContext {
    const AnimationTreeResource& tree;
    // The animation a name in the tree stands for, or null.
    std::function<const Animation*(const std::string& name)> animation;
    std::size_t track_count = 0;
    // For each node, the tracks its filters name, by index.
    const std::vector<std::vector<int>>& filters;
    // Mixed into OneShot's random restart delays, so each entity's differ but runs repeat.
    std::uint64_t seed = 0;
};

// Processes a tree for one tick of delta seconds, as Godot's AnimationTree::_blend_pre_process;
// started seeks every node to its start, as the first tick after the tree is turned on does.
[[nodiscard]] TreeTick run_tree(const TreeContext& context, TreeParameters& parameters,
                                double delta, bool started);

// A TreeCurve's value at an offset from 0 to 1.
[[nodiscard]] double sample_curve(const TreeCurve& curve, double offset);

} // namespace slash::animation::detail
