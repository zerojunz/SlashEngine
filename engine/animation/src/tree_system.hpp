#pragma once

// The animation_tree system: plays each AnimationTree's tree (tree_run.hpp) and mixes the
// animations it plays as Godot's AnimationMixer does.

#include "slash/asset/assets.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/world.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace slash::animation::detail {

class TreePlayer {
public:
    explicit TreePlayer(asset::Assets& assets);
    ~TreePlayer();
    TreePlayer(const TreePlayer&) = delete;
    TreePlayer& operator=(const TreePlayer&) = delete;
    TreePlayer(TreePlayer&&) = delete;
    TreePlayer& operator=(TreePlayer&&) = delete;

    // One tick of every AnimationTree.
    void run(world::World& world, double tick_seconds);

    // The clips an entity's AnimationTree poses its model with, as its last tick left them, or
    // none when the entity has no AnimationTree playing a tree.
    [[nodiscard]] std::optional<std::vector<scene3d::ClipSample>> clips(const world::World& world,
                                                                        entt::entity entity);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace slash::animation::detail
