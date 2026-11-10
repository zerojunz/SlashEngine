#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds style.mark, which keeps the look of a scene, the open world, an image, a model or a
// material as a storyboard style reference with its fingerprint, and style.check, which captures
// what references govern the way they were captured and reports drift from them.
void register_style_commands(Registry& registry);

} // namespace slash::command
