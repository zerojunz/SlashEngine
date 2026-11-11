#include "slash/world/physical_bones.hpp"

#include <utility>

namespace slash::world {

void set_physical_bone_linker(World& world, PhysicalBoneLinker linker) {
    world.ecs().ctx().insert_or_assign<PhysicalBoneLinker>(std::move(linker));
}

PhysicalBoneLinks physical_bone_links(const World& world) {
    const auto* linker = world.ecs().ctx().find<PhysicalBoneLinker>();
    return linker != nullptr && *linker ? (*linker)(world) : PhysicalBoneLinks{};
}

void set_physical_bone_targets(World& world, PhysicalBoneTargets targets) {
    world.ecs().ctx().insert_or_assign<PhysicalBoneTargets>(std::move(targets));
}

const PhysicalBoneTargets* physical_bone_targets(const World& world) {
    return world.ecs().ctx().find<PhysicalBoneTargets>();
}

} // namespace slash::world
