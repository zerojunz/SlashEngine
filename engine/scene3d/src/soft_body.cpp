#include "slash/scene3d/soft_body.hpp"

#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"

#include <string>
#include <utility>

namespace slash::scene3d {

namespace {

struct SoftBodySourceHolder {
    SoftBodySource source;
};

} // namespace

void set_soft_body_source(world::World& world, SoftBodySource source) {
    world.ecs().ctx().insert_or_assign(SoftBodySourceHolder{std::move(source)});
}

std::shared_ptr<const SoftBodyPose> soft_body_pose(const world::World& world, entt::entity entity) {
    const auto* holder = world.ecs().ctx().find<SoftBodySourceHolder>();
    if (holder == nullptr || !holder->source) {
        return nullptr;
    }
    return holder->source(world, entity);
}

std::vector<Problem> check_soft_bodies(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<const world::SoftBody3D>()) {
        const auto* instance = ecs.try_get<const world::MeshInstance3D>(entity);
        if (instance == nullptr) {
            // As Godot's warning: "This body will be ignored until you set a mesh."
            problems.push_back({entity, "/components/SoftBody3D",
                                "a soft body needs a MeshInstance3D drawing its mesh; it is "
                                "ignored until it has one"});
            continue;
        }
        if (!instance->mesh.empty()) {
            problems.push_back({entity, "/components/MeshInstance3D/mesh",
                                "a soft body simulates a primitive mesh component, not a model "
                                "file, yet"});
            continue;
        }
        auto primitive = primitive_of(world, entity);
        if (!primitive || !primitive.value()) {
            continue; // check_meshes reports it
        }
        auto mesh = make_mesh(*primitive.value());
        if (!mesh) {
            continue; // the same
        }
        const auto& body = ecs.get<const world::SoftBody3D>(entity);
        const std::size_t vertices = mesh.value().vertices.size();
        for (std::size_t i = 0; i < body.pinned_points.size(); ++i) {
            const world::SoftBodyPin& pin = body.pinned_points[i];
            const std::string path = "/components/SoftBody3D/pinned_points/" + std::to_string(i);
            if (pin.point < 0 || static_cast<std::size_t>(pin.point) >= vertices) {
                // As Godot's pin_vertices error.
                problems.push_back({entity, path + "/point",
                                    "point " + std::to_string(pin.point) +
                                        " is out of bounds: the mesh has " +
                                        std::to_string(vertices) + " vertices"});
            }
            if (pin.attachment != entt::null &&
                (!world.valid(pin.attachment) || !ecs.all_of<world::Transform3D>(pin.attachment))) {
                problems.push_back(
                    {entity, path + "/attachment", "the attachment has no Transform3D to follow"});
            }
        }
    }
    return problems;
}

} // namespace slash::scene3d
