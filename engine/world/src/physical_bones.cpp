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

} // namespace slash::world
