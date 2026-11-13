#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds face.check, face.generate and face.portrait, over .face.json files and the faces of
// .character.json recipes (slash/world/face.hpp).
void register_face_commands(Registry& registry);

} // namespace slash::command
