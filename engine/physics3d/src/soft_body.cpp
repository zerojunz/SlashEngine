// Soft bodies, as Godot's SoftBody3D on its Jolt module (scene/3d/physics/soft_body_3d.cpp and
// modules/jolt_physics/objects/jolt_soft_body_3d.cpp): each function names the one it follows.
#include "space.hpp"

#include "slash/core/log.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/wind.hpp"

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
// clang-format on

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace slash::physics3d::detail {

namespace {

using world::SoftBody3D;

// Godot's physics/jolt_physics_3d/simulation/soft_body_point_radius.
constexpr float kPointRadius = 0.01F;
// Godot's physics/jolt_physics_3d/limits/max_linear_velocity.
constexpr float kMaxLinearVelocity = 500.0F;
// Kilograms a cubic meter of air weighs at sea level, for the drag of WindSource3Ds' wind.
constexpr float kAirDensity = 1.225F;

// The primitive mesh a soft body simulates, with its key, or nullopt when the entity draws none.
std::optional<std::pair<scene3d::Primitive, std::string>> mesh_of(const world::World& world,
                                                                  entt::entity entity) {
    const auto* instance = world.ecs().try_get<world::MeshInstance3D>(entity);
    if (instance == nullptr || !instance->mesh.empty()) {
        return std::nullopt;
    }
    auto primitive = scene3d::primitive_of(world, entity);
    if (!primitive) {
        return std::nullopt;
    }
    const std::optional<scene3d::Primitive>& drawn = primitive.value();
    if (!drawn) {
        return std::nullopt;
    }
    const scene3d::Primitive& found = *drawn;
    std::string key(scene3d::kPrimitiveNames[found.index()]);
    key += std::visit(
        [&](const auto& mesh) {
            return json::write(world::to_json(world, mesh), json::Style::Compact);
        },
        found);
    return std::pair{found, std::move(key)};
}

// The points pinned, as Godot's pin_vertices finds them: each pinned mesh vertex's point, those
// out of range left out.
std::vector<int> pinned_points(const SoftBody& soft, const SoftBody3D& component) {
    std::vector<int> pinned;
    for (const world::SoftBodyPin& pin : component.pinned_points) {
        if (pin.point >= 0 && static_cast<std::size_t>(pin.point) < soft.mesh_to_physics.size()) {
            if (const int point = soft.mesh_to_physics[static_cast<std::size_t>(pin.point)];
                point >= 0) {
                pinned.push_back(point);
            }
        }
    }
    std::sort(pinned.begin(), pinned.end());
    pinned.erase(std::unique(pinned.begin(), pinned.end()), pinned.end());
    return pinned;
}

JPH::SoftBodyMotionProperties& motion_of(JPH::Body& body) {
    return static_cast<JPH::SoftBodyMotionProperties&>(*body.GetMotionPropertiesUnchecked());
}

const JPH::SoftBodyMotionProperties& motion_of(const JPH::Body& body) {
    return static_cast<const JPH::SoftBodyMotionProperties&>(*body.GetMotionPropertiesUnchecked());
}

// Where a soft body's mesh is, as JoltSoftBody3D::update_rendering_server: each vertex at its
// point, with the normalized sum of the normals of the faces around the point.
std::shared_ptr<const scene3d::SoftBodyPose> pose_of(const SoftBody& soft, const JPH::Body& body) {
    const JPH::SoftBodyMotionProperties& motion = motion_of(body);
    const JPH::Array<JPH::SoftBodyVertex>& points = motion.GetVertices();
    const JPH::RVec3 center = body.GetCenterOfMassPosition();
    std::vector<Vec3> positions(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        positions[i] = from_jolt_place(center + points[i].mPosition);
    }
    std::vector<Vec3> normals(points.size());
    for (const JPH::SoftBodySharedSettings::Face& face : motion.GetFaces()) {
        // Jolt winds faces the other way.
        const std::uint32_t i0 = face.mVertex[2];
        const std::uint32_t i1 = face.mVertex[1];
        const std::uint32_t i2 = face.mVertex[0];
        const Vec3 normal =
            normalized(cross(positions[i2] - positions[i0], positions[i1] - positions[i0]));
        normals[i0] = normals[i0] + normal;
        normals[i1] = normals[i1] + normal;
        normals[i2] = normals[i2] + normal;
    }
    for (Vec3& normal : normals) {
        if (const float size = length(normal); size > kEpsilon) {
            normal = normal * (1.0F / size);
        }
    }
    auto pose = std::make_shared<scene3d::SoftBodyPose>();
    pose->positions.resize(soft.mesh_to_physics.size());
    pose->normals.resize(soft.mesh_to_physics.size());
    for (std::size_t i = 0; i < soft.mesh_to_physics.size(); ++i) {
        if (const int point = soft.mesh_to_physics[i]; point >= 0) {
            pose->positions[i] = positions[static_cast<std::size_t>(point)];
            pose->normals[i] = normals[static_cast<std::size_t>(point)];
        } else {
            // A vertex no face uses stays where the mesh put it.
            pose->positions[i] = soft.placed.apply(soft.mesh_positions[i]);
            pose->normals[i] = Vec3{0.0F, 1.0F, 0.0F};
        }
    }
    return pose;
}

} // namespace

// Godot's JoltSoftBody3D::_ref_shared_data and _add_to_space: the mesh's vertices welded by
// position into points, its faces and edge springs, then the body with the component's settings,
// its points placed by the entity's transform without its scale.
void Space::create_soft_body(SoftBody& soft, const scene3d::MeshData& mesh,
                             const SoftBody3D& component) {
    const JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    soft.mesh_to_physics.assign(mesh.vertices.size(), -1);
    soft.mesh_positions.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& [x, y, z] = mesh.vertices[i].position;
        soft.mesh_positions[i] = Vec3{x, y, z};
    }
    // Godot's HashMap<Vector3, int>: vertices at equal positions are one point.
    std::map<std::tuple<float, float, float>, int> points;
    int count = 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::array<int, 3> face{};
        for (std::size_t j = 0; j < 3; ++j) {
            const std::uint32_t index = mesh.indices[i + j];
            const Vec3 vertex = soft.mesh_positions[index];
            const auto [found, added] = points.try_emplace({vertex.x, vertex.y, vertex.z}, count);
            if (added) {
                const Vec3 placed = soft.placed.apply(vertex);
                shared->mVertices.emplace_back(JPH::Float3(placed.x, placed.y, placed.z),
                                               JPH::Float3(0.0F, 0.0F, 0.0F), 1.0F);
                ++count;
            }
            face[j] = found->second;
            soft.mesh_to_physics[index] = found->second;
        }
        if (face[0] == face[1] || face[0] == face[2] || face[1] == face[2]) {
            continue; // degenerate faces are left out, as Jolt refuses them
        }
        // Jolt winds faces the other way.
        shared->mFaces.emplace_back(static_cast<JPH::uint32>(face[2]),
                                    static_cast<JPH::uint32>(face[1]),
                                    static_cast<JPH::uint32>(face[0]));
    }
    // Pinned points order the constraints Optimize makes.
    for (const int point : pinned_points(soft, component)) {
        shared->mVertices[static_cast<std::size_t>(point)].mInvMass = 0.0F;
    }
    // Godot maps its stiffness coefficient, 0 to 1, to Jolt's edge stiffness with an arbitrary
    // constant.
    const float coefficient = std::clamp(component.linear_stiffness, 0.0F, 1.0F);
    const float stiffness =
        std::max(coefficient * coefficient * coefficient * 100000.0F, 0.000001F);
    JPH::SoftBodySharedSettings::VertexAttributes attributes;
    attributes.mCompliance = attributes.mShearCompliance = 1.0F / stiffness;
    shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::None);
    const float multiplier = 1.0F - component.shrinking_factor;
    for (JPH::SoftBodySharedSettings::Edge& edge : shared->mEdgeConstraints) {
        edge.mRestLength *= multiplier;
    }
    shared->Optimize();

    soft.layer = component.collision_layer;
    soft.mask = component.collision_mask;
    soft.precision = std::max(component.simulation_precision, 0);
    soft.pressure = std::max(component.pressure_coefficient, 0.0F);
    soft.damping = std::max(component.damping_coefficient, 0.0F);
    JPH::SoftBodyCreationSettings settings(
        shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
        layers_.to_object_layer(broad::kMoving, soft.layer, soft.mask));
    settings.mNumIterations = static_cast<JPH::uint32>(soft.precision);
    settings.mLinearDamping = soft.damping;
    settings.mPressure = soft.pressure;
    settings.mVertexRadius = kPointRadius;
    settings.mMaxLinearVelocity = kMaxLinearVelocity;
    settings.mRestitution = 0.0F;
    settings.mFriction = 1.0F;
    settings.mUpdatePosition = false;
    settings.mMakeRotationIdentity = false;
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    JPH::Body* created = interface.CreateSoftBody(settings);
    if (created == nullptr) {
        log::warn("3D physics body left out",
                  {{"entity", soft.name}, {"problem", "too many 3D physics bodies"}});
        return;
    }
    soft.id = created->GetID();
    interface.AddBody(soft.id, JPH::EActivation::Activate);
    soft.total_mass = component.total_mass;
    soft.pinned = pinned_points(soft, component);
    update_masses(soft);
    soft.pose = pose_of(soft, *created);
    ++added_;
}

// Godot's JoltSoftBody3D::_update_mass.
void Space::update_masses(SoftBody& soft) {
    const JPH::BodyLockWrite lock(system_->GetBodyLockInterfaceNoLock(), soft.id);
    if (!lock.Succeeded()) {
        return;
    }
    JPH::Array<JPH::SoftBodyVertex>& points = motion_of(lock.GetBody()).GetVertices();
    const float total = std::max(soft.total_mass, 0.0F);
    const float inverse = total == 0.0F ? 1.0F : static_cast<float>(points.size()) / total;
    for (JPH::SoftBodyVertex& point : points) {
        point.mInvMass = inverse;
    }
    for (const int point : soft.pinned) {
        points[static_cast<std::size_t>(point)].mInvMass = 0.0F;
    }
}

void Space::destroy(SoftBody& soft) {
    if (system_ != nullptr && !soft.id.IsInvalid()) {
        JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
        interface.RemoveBody(soft.id);
        interface.DestroyBody(soft.id);
    }
    soft.id = JPH::BodyID();
}

void Space::sync_soft_bodies(world::World& world, const world::Tree3D& tree) {
    const entt::registry& registry = world.ecs();
    soft_bodies_.erase_if([&](entt::entity entity, const std::unique_ptr<SoftBody>& soft) {
        if (world.valid(entity) && registry.all_of<SoftBody3D>(entity) &&
            mesh_of(world, entity).has_value()) {
            return false;
        }
        destroy(*soft);
        return true;
    });
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    for (const entt::entity entity : registry.view<SoftBody3D>()) {
        auto mesh = mesh_of(world, entity);
        if (!mesh) {
            continue;
        }
        const auto& component = registry.get<SoftBody3D>(entity);
        const world::Affine3D placed = unscale(tree.global(entity)).transform;
        SoftBody* soft = nullptr;
        if (std::unique_ptr<SoftBody>* found = soft_bodies_.find(entity); found != nullptr) {
            soft = found->get();
            // Godot keeps the springs it made for a mesh; a new mesh or springs make it again.
            if (soft->mesh_key != mesh->second ||
                soft->linear_stiffness != component.linear_stiffness ||
                soft->shrinking_factor != component.shrinking_factor) {
                destroy(*soft);
            }
        } else {
            auto made = std::make_unique<SoftBody>();
            made->entity = entity;
            made->name = std::string(world.id_of(entity));
            soft = soft_bodies_.emplace(entity, std::move(made)).get();
        }
        if (soft->id.IsInvalid()) {
            auto data = scene3d::make_mesh(mesh->first);
            if (!data) {
                continue; // scene validate reports it
            }
            soft->mesh_key = mesh->second;
            soft->linear_stiffness = component.linear_stiffness;
            soft->shrinking_factor = component.shrinking_factor;
            soft->placed = placed;
            soft->ignore = component.parent_collision_ignore;
            create_soft_body(*soft, data.value(), component);
            continue;
        }
        soft->ignore = component.parent_collision_ignore;
        bool wake = false;
        if (component.collision_layer != soft->layer || component.collision_mask != soft->mask) {
            soft->layer = component.collision_layer;
            soft->mask = component.collision_mask;
            interface.SetObjectLayer(
                soft->id, layers_.to_object_layer(broad::kMoving, soft->layer, soft->mask));
        }
        std::vector<int> pinned = pinned_points(*soft, component);
        if (component.total_mass != soft->total_mass || pinned != soft->pinned) {
            soft->total_mass = component.total_mass;
            soft->pinned = std::move(pinned);
            update_masses(*soft);
            wake = true;
        }
        const int precision = std::max(component.simulation_precision, 0);
        const float pressure = std::max(component.pressure_coefficient, 0.0F);
        const float damping = std::max(component.damping_coefficient, 0.0F);
        if (precision != soft->precision || pressure != soft->pressure ||
            damping != soft->damping || placed != soft->placed) {
            const JPH::BodyLockWrite lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
            if (lock.Succeeded()) {
                JPH::SoftBodyMotionProperties& motion = motion_of(lock.GetBody());
                motion.SetNumIterations(static_cast<JPH::uint32>(precision));
                motion.SetPressure(pressure);
                motion.SetLinearDamping(damping);
                if (placed != soft->placed) {
                    // Godot's JoltSoftBody3D::set_transform: the change moves every point, which
                    // stops.
                    const world::Affine3D relative = placed * soft->placed.inverse();
                    for (JPH::SoftBodyVertex& point : motion.GetVertices()) {
                        point.mPosition = point.mPreviousPosition =
                            to_jolt(relative.apply(from_jolt(point.mPosition)));
                        point.mVelocity = JPH::Vec3::sZero();
                    }
                }
            }
            soft->placed = placed;
            soft->precision = precision;
            soft->pressure = pressure;
            soft->damping = damping;
            wake = true;
        }
        if (wake) {
            interface.ActivateBody(soft->id);
        }
    }
}

std::vector<AreaWind> Space::area_winds(const world::World& world,
                                        const world::Tree3D& tree) const {
    std::vector<AreaWind> winds;
    for (const auto& [entity, area] : bodies_) {
        if (area->kind != Kind::Area || area->id.IsInvalid()) {
            continue;
        }
        const auto& component = world.ecs().get<world::Area3D>(entity);
        if (component.wind_force_magnitude == 0.0F || component.wind_source_path == entt::null ||
            !world.valid(component.wind_source_path)) {
            continue;
        }
        const world::Affine3D source = tree.global(component.wind_source_path);
        const JPH::Vec3 z = to_jolt(source.z);
        if (z.LengthSq() <= 0.0F) {
            continue;
        }
        winds.push_back({.area = area.get(),
                         .magnitude = component.wind_force_magnitude,
                         .attenuation = component.wind_attenuation_factor,
                         .direction = -z.Normalized(),
                         .source = to_jolt_place(source.origin)});
    }
    return winds;
}

// The wind on each face pushes its three points: WindSource3Ds' wind as air drags a sheet,
// by the body's drag_coefficient, and the wind of the Area3Ds whose shapes hold the face's
// middle, as GodotSoftBody3D::apply_forces and _compute_area_windforce.
bool Space::blow_soft_body(const SoftBody& soft, float drag_coefficient, JPH::Body& body,
                           const world::WindField& field, double time,
                           const std::vector<AreaWind>& areas, float seconds) const {
    const bool drag = drag_coefficient > 0.0F && !field.empty();
    std::vector<const AreaWind*> felt;
    for (const AreaWind& wind : areas) {
        if ((wind.area->mask & soft.layer) != 0) {
            felt.push_back(&wind);
        }
    }
    if (!drag && felt.empty()) {
        return false;
    }
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    JPH::SoftBodyMotionProperties& motion = motion_of(body);
    JPH::Array<JPH::SoftBodyVertex>& points = motion.GetVertices();
    const JPH::RVec3 center = body.GetCenterOfMassPosition();
    std::vector<JPH::Vec3> forces(points.size(), JPH::Vec3::sZero());
    bool pushed = false;
    for (const JPH::SoftBodySharedSettings::Face& face : motion.GetSettings()->mFaces) {
        const JPH::SoftBodyVertex& a = points[face.mVertex[0]];
        const JPH::SoftBodyVertex& b = points[face.mVertex[1]];
        const JPH::SoftBodyVertex& c = points[face.mVertex[2]];
        const JPH::Vec3 twice = (b.mPosition - a.mPosition).Cross(c.mPosition - a.mPosition);
        const float doubled = twice.Length();
        if (doubled <= 0.0F) {
            continue;
        }
        const JPH::Vec3 normal = twice / doubled;
        const float area = 0.5F * doubled;
        const JPH::RVec3 middle =
            center + ((a.mPosition + b.mPosition + c.mPosition) * (1.0F / 3.0F));
        JPH::Vec3 force = JPH::Vec3::sZero();
        if (drag) {
            const JPH::Vec3 air = to_jolt(field.at(from_jolt_place(middle), time));
            const JPH::Vec3 moving = (a.mVelocity + b.mVelocity + c.mVelocity) * (1.0F / 3.0F);
            const float across = normal.Dot(air - moving);
            force +=
                normal * (0.5F * kAirDensity * drag_coefficient * area * across * std::abs(across));
        }
        for (const AreaWind* wind : felt) {
            const JPH::Body* shape_body = locks.TryGetBody(wind->area->id);
            if (shape_body == nullptr) {
                continue;
            }
            JPH::AnyHitCollisionCollector<JPH::CollidePointCollector> inside;
            shape_body->GetTransformedShape().CollidePoint(middle, inside);
            if (!inside.HadHit()) {
                continue;
            }
            const float toward_normal = normal.Dot(wind->direction);
            const float toward_middle = JPH::Vec3(middle - wind->source).Dot(wind->direction);
            // pow(toward_middle, -attenuation), which Godot leaves undefined behind the source.
            float attenuated = 1.0F;
            if (wind->attenuation != 0.0F) {
                if (toward_middle <= 0.0F) {
                    continue;
                }
                attenuated = slash::exp(-wind->attenuation * slash::ln(toward_middle));
            }
            force += normal * (wind->magnitude * area * toward_normal * attenuated);
        }
        if (force.IsNearZero(0.0F)) {
            continue;
        }
        // A third of the face's force to each of its points.
        const JPH::Vec3 share = force * (1.0F / 3.0F);
        for (const JPH::uint32 point : face.mVertex) {
            forces[point] += share;
        }
        pushed = true;
    }
    if (!pushed) {
        return false;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        points[i].mVelocity += forces[i] * (points[i].mInvMass * seconds);
    }
    return true;
}

void Space::pre_step_soft_bodies(const world::World& world, const world::Tree3D& tree,
                                 float seconds) {
    const entt::registry& registry = world.ecs();
    const world::WindField field(world, tree);
    const double time = world::wind_time(world);
    const std::vector<AreaWind> areas = area_winds(world, tree);
    for (auto& [entity, soft] : soft_bodies_) {
        if (soft->id.IsInvalid()) {
            continue;
        }
        const auto& component = registry.get<SoftBody3D>(entity);
        bool moved = false;
        JPH::BodyLockWrite lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
        if (!lock.Succeeded()) {
            continue;
        }
        JPH::Body& body = lock.GetBody();
        JPH::Array<JPH::SoftBodyVertex>& points = motion_of(body).GetVertices();
        const JPH::RVec3 center = body.GetCenterOfMassPosition();
        // Godot's SoftBody3D::_update_physics_server: each pinned point with an attachment goes
        // where the attachment puts its offset, as JoltSoftBody3D::set_vertex_position.
        for (const world::SoftBodyPin& pin : component.pinned_points) {
            if (pin.attachment == entt::null || !world.valid(pin.attachment) ||
                !registry.all_of<world::Transform3D>(pin.attachment) || pin.point < 0 ||
                static_cast<std::size_t>(pin.point) >= soft->mesh_to_physics.size()) {
                continue;
            }
            const int point = soft->mesh_to_physics[static_cast<std::size_t>(pin.point)];
            if (point < 0) {
                continue;
            }
            const Vec3 target = tree.global(pin.attachment).apply(pin.offset);
            const JPH::Vec3 position(to_jolt(target) - JPH::Vec3(center));
            JPH::SoftBodyVertex& vertex = points[static_cast<std::size_t>(point)];
            if (vertex.mPosition != position) {
                vertex.mPosition = position;
                moved = true;
            }
        }
        if (blow_soft_body(*soft, component.drag_coefficient, body, field, time, areas, seconds)) {
            moved = true;
        }
        if (moved) {
            lock.ReleaseLock();
            system_->GetBodyInterfaceNoLock().ActivateBody(soft->id);
        }
    }
}

void Space::read_soft_bodies() {
    for (auto& [entity, soft] : soft_bodies_) {
        if (soft->id.IsInvalid()) {
            continue;
        }
        const JPH::BodyLockRead lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
        if (lock.Succeeded() && lock.GetBody().IsActive()) {
            soft->pose = pose_of(*soft, lock.GetBody());
        }
    }
}

const SoftBody* Space::soft_body(entt::entity entity) const {
    const std::unique_ptr<SoftBody>* found = soft_bodies_.find(entity);
    if (found == nullptr) {
        found = cloths_.find(entity);
    }
    return found != nullptr && !(*found)->id.IsInvalid() ? found->get() : nullptr;
}

const SoftBody* Space::soft_body(const JPH::BodyID& id) const {
    for (const auto* bodies : {&soft_bodies_, &cloths_}) {
        for (const auto& [entity, soft] : *bodies) {
            if (soft->id == id) {
                return soft.get();
            }
        }
    }
    return nullptr;
}

bool Space::sleeping(const SoftBody& soft) const {
    return system_ == nullptr || !system_->GetBodyInterfaceNoLock().IsActive(soft.id);
}

// Godot's JoltContactListener3D::OnSoftBodyContactValidate and
// _try_override_collision_response, with parent_collision_ignore as its collision exception.
JPH::SoftBodyValidateResult
ContactListener::OnSoftBodyContactValidate(const JPH::Body& soft, const JPH::Body& other,
                                           JPH::SoftBodyContactSettings& settings) {
    const SoftBody* body = space_->soft_body(soft.GetID());
    const Body* touched = Space::body_of(other);
    if (body == nullptr || touched == nullptr) {
        return JPH::SoftBodyValidateResult::AcceptContact;
    }
    if (touched->entity == body->ignore) {
        return JPH::SoftBodyValidateResult::RejectContact;
    }
    if (other.IsSensor()) {
        return JPH::SoftBodyValidateResult::AcceptContact;
    }
    const bool collides = (body->mask & touched->layer) != 0;
    const bool collided = (touched->mask & body->layer) != 0;
    if (collides && !collided) {
        settings.mInvMassScale2 = 0.0F;
        settings.mInvInertiaScale2 = 0.0F;
    } else if (collided && !collides) {
        settings.mInvMassScale1 = 0.0F;
    }
    return JPH::SoftBodyValidateResult::AcceptContact;
}

} // namespace slash::physics3d::detail
