#pragma once

#include "slash/world/world.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <utility>
#include <vector>

// How each PhysicalBone3D takes part in physics, as its PhysicalBoneSimulator3D and the bones of
// the model it poses decide: engine/scene3d, which knows models, works it out, and
// engine/physics3d, which knows none, builds the bodies and joints from it.
namespace slash::world {

// What a physical bone's body is, as Godot's PhysicalBone3D sets its body mode.
enum class PhysicalBoneMode : std::uint8_t {
    // A static body that collides: a bone with no simulator.
    Static,
    // A static body that collides with nothing: its simulator is inactive, or its model has no
    // such bone.
    Off,
    // A kinematic body that follows its bone, as its simulator is active and it doesn't simulate.
    Follow,
    // A rigid body that physics moves, and whose bone follows it.
    Simulate,
};

struct PhysicalBoneLink {
    PhysicalBoneMode mode = PhysicalBoneMode::Static;
    // The physical bone its joint holds it to: that of the nearest bone above its own that has
    // one under the same simulator, or null.
    entt::entity parent{entt::null};
    // Bodies it never collides with.
    std::vector<entt::entity> exceptions{}; // NOLINT(readability-redundant-member-init)

    friend bool operator==(const PhysicalBoneLink&, const PhysicalBoneLink&) = default;
};

// Each physical bone under a simulator with its link; those left out are PhysicalBoneMode::Static
// with no parent.
using PhysicalBoneLinks = std::vector<std::pair<entt::entity, PhysicalBoneLink>>;
using PhysicalBoneLinker = std::function<PhysicalBoneLinks(const World& world)>;

// Sets what works the links out for a world, kept in its context.
void set_physical_bone_linker(World& world, PhysicalBoneLinker linker);
// The links as they are now; none when no linker was set.
[[nodiscard]] PhysicalBoneLinks physical_bone_links(const World& world);

} // namespace slash::world
