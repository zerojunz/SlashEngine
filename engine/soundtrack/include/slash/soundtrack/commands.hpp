#pragma once

#include "slash/command/command.hpp"

// The soundtrack's commands: score.import and score.export, which turn Standard MIDI Files into
// .score.json scores and back.
namespace slash::soundtrack {

void register_commands(command::Registry& registry);

} // namespace slash::soundtrack
