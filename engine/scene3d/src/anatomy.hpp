#pragma once

// A character's anatomy (slash/world/flesh.hpp) as meshes on its rig, for its model's
// "::skeleton" and "::muscles" forms (src/character.cpp).

#include "rig_builder.hpp"

#include "slash/world/flesh.hpp"

#include <vector>

namespace slash::scene3d::rig {

// Adds the anatomy's bones, drawn "bone", and with muscles its muscles, "muscle" with their
// tendons "tendon", each moved by the rig's bones of its name.
void add_anatomy(const world::Anatomy& anatomy, const std::vector<Bone>& bones, Builder& out,
                 bool muscles);

} // namespace slash::scene3d::rig
