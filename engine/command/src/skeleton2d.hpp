#pragma once

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"

// 2D skeletons (slash/world/skeleton2d.hpp): skeleton.bind, and skeleton.get given a Skeleton2D.
namespace slash::command::detail {

// skeleton.get of a Skeleton2D: its bones, each with its parent bone, and for those named in
// "bones" their rest, pose and tip in the skeleton's space; and the polygons it bends.
[[nodiscard]] Reply skeleton_get_2d(Session& session, entt::entity skeleton,
                                    const json::Value& params);
// Registers skeleton.bind.
void register_skeleton2d_commands(Registry& registry);

} // namespace slash::command::detail
