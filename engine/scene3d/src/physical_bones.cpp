#include "slash/scene3d/physical_bones.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

// Godot 4.6's PhysicalBoneSimulator3D and the binding half of PhysicalBone3D
// (scene/3d/physics/physical_bone_simulator_3d.cpp, physical_bone_3d.cpp). The bodies and joints
// are engine/physics3d's.
namespace slash::scene3d {
namespace {

// Whether ancestor is above bone in the model, as Skeleton3D::is_bone_parent_of.
bool is_bone_parent_of(const Model& model, int bone, int ancestor) {
    for (int up = model.nodes[static_cast<std::size_t>(bone)].parent; up >= 0;
         up = model.nodes[static_cast<std::size_t>(up)].parent) {
        if (up == ancestor) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<PhysicalBone> physical_bones(const world::World& world, entt::entity entity,
                                         const Model* model) {
    std::vector<PhysicalBone> out;
    const auto& ecs = world.ecs();
    const auto* transform = ecs.try_get<const world::Transform3D>(entity);
    if (transform == nullptr) {
        return out;
    }
    const std::size_t nodes = model != nullptr ? model->nodes.size() : 0;
    // PhysicalBone3D::update_bone_id and PhysicalBoneSimulator3D::bind_physical_bone_to_bone:
    // the first physical bone to name a bone has it.
    std::vector<entt::entity> bound(nodes, entt::entity{entt::null});
    for (const entt::entity child : transform->children) {
        if (child == entt::null || !world.valid(child) ||
            !ecs.all_of<world::PhysicalBone3D, world::Transform3D>(child) ||
            std::ranges::any_of(out, [&](const PhysicalBone& b) { return b.entity == child; })) {
            continue;
        }
        PhysicalBone bone{.entity = child};
        if (model != nullptr) {
            bone.bone = find_node(*model, ecs.get<const world::PhysicalBone3D>(child).bone_name);
            if (bone.bone >= 0 && bound[static_cast<std::size_t>(bone.bone)] != entt::null) {
                bone.bone = -1;
            } else if (bone.bone >= 0) {
                bound[static_cast<std::size_t>(bone.bone)] = child;
            }
        }
        out.push_back(bone);
    }
    if (model == nullptr) {
        return out;
    }

    // physical_bones_start_simulation_on: the named bones and those under them, or every bone
    // when it names none it can find.
    const auto* simulator = ecs.try_get<const world::PhysicalBoneSimulator3D>(entity);
    std::vector<int> simulated;
    if (simulator != nullptr) {
        for (const std::string& name : simulator->simulated_bones) {
            if (const int found = find_node(*model, name); found >= 0) {
                simulated.push_back(found);
            }
        }
    }
    for (PhysicalBone& bone : out) {
        if (bone.bone < 0) {
            continue;
        }
        // PhysicalBoneSimulator3D::_get_physical_bone_parent.
        for (int up = model->nodes[static_cast<std::size_t>(bone.bone)].parent; up >= 0;
             up = model->nodes[static_cast<std::size_t>(up)].parent) {
            if (bound[static_cast<std::size_t>(up)] != entt::null) {
                bone.parent = bound[static_cast<std::size_t>(up)];
                break;
            }
        }
        bone.simulating =
            simulator != nullptr && simulator->simulating &&
            (simulated.empty() || std::ranges::any_of(simulated, [&](int each) {
                 return each == bone.bone || is_bone_parent_of(*model, bone.bone, each);
             }));
    }
    return out;
}

std::vector<std::pair<entt::entity, entt::entity>> simulators(const world::World& world) {
    std::vector<std::pair<entt::entity, entt::entity>> out;
    const auto& ecs = world.ecs();
    const auto view = ecs.view<const world::PhysicalBoneSimulator3D>();
    if (view.empty()) {
        return out;
    }
    const world::Tree3D tree(world);
    for (const entt::entity entity : view) {
        entt::entity drawn{entt::null};
        if (ecs.all_of<world::MeshInstance3D>(entity)) {
            drawn = entity;
        } else if (const entt::entity up = tree.parent(entity);
                   up != entt::null && ecs.all_of<world::MeshInstance3D>(up)) {
            drawn = up;
        }
        out.emplace_back(entity, drawn);
    }
    // In the order entities were made, the same in every session.
    std::ranges::sort(out, {}, [](const auto& entry) { return entt::to_entity(entry.first); });
    return out;
}

world::Affine3D body_offset(const world::PhysicalBone3D& bone) {
    world::Transform3D offset;
    offset.position = bone.body_offset_position;
    offset.rotation = bone.body_offset_rotation;
    return world::affine(offset);
}

world::PhysicalBoneLinks physical_bone_links(const world::World& world, const ModelLoader& load) {
    world::PhysicalBoneLinks links;
    const auto& ecs = world.ecs();
    for (const auto& [entity, drawn] : simulators(world)) {
        std::shared_ptr<const Model> model;
        if (drawn != entt::null && load) {
            const std::string& path = ecs.get<const world::MeshInstance3D>(drawn).mesh;
            if (!path.empty()) {
                if (auto loaded = load(path); loaded) {
                    model = loaded.value();
                }
            }
        }
        const auto& simulator = ecs.get<const world::PhysicalBoneSimulator3D>(entity);
        for (const PhysicalBone& bone : physical_bones(world, entity, model.get())) {
            // PhysicalBone3D::_start_physics_simulation and _stop_physics_simulation.
            auto mode = world::PhysicalBoneMode::Off;
            if (bone.bone >= 0 && bone.simulating) {
                mode = world::PhysicalBoneMode::Simulate;
            } else if (bone.bone >= 0 && simulator.active) {
                mode = world::PhysicalBoneMode::Follow;
            }
            links.emplace_back(
                bone.entity, world::PhysicalBoneLink{.mode = mode,
                                                     .parent = bone.parent,
                                                     .exceptions = simulator.collision_exceptions});
        }
    }
    return links;
}

void set_global_transform(world::World& world, const world::Tree3D& tree, entt::entity entity,
                          const world::Affine3D& global) {
    auto& transform = world.ecs().get_or_emplace<world::Transform3D>(entity);
    const entt::entity up = tree.transform_parent(entity);
    const world::Affine3D local = up == entt::null ? global : tree.global(up).inverse() * global;
    const NodePose pose = decompose(local);
    const Vec3 rotation = world::euler(world::rotation_basis(pose.rotation));
    // Its scale stays: a body's shape is rebuilt when its scale changes.
    if (transform.position != pose.position) {
        transform.position = pose.position;
    }
    if (transform.rotation != rotation) {
        transform.rotation = rotation;
    }
}

void set_top_level(world::World& world, const world::Tree3D& tree, entt::entity entity,
                   bool top_level) {
    auto* transform = world.ecs().try_get<world::Transform3D>(entity);
    if (transform == nullptr || transform->top_level == top_level) {
        return;
    }
    const world::Affine3D global = tree.global(entity);
    transform->top_level = top_level;
    set_global_transform(world, tree, entity, global);
}

} // namespace slash::scene3d
