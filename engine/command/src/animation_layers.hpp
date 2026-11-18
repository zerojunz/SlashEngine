#pragma once

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"

// Tuning any model's animations through .anim.json layers (slash/animation/animation.hpp's
// AnimationLayer): animation.edit, and animation.preview and animation.tune given a library.
// creature.cpp registers them beside the creature tools.
namespace slash::command::detail {

// animation.preview of a library's animation: its bones' turns and, for a humanoid, its gait.
[[nodiscard]] Reply library_preview(Session& session, const json::Value& params);
// animation.tune of a library's animation: set, layer, ranges and slide, written to the library.
[[nodiscard]] Reply library_tune(Session& session, const json::Value& params);
// Registers animation.edit.
void register_animation_edit(Registry& registry);

} // namespace slash::command::detail
