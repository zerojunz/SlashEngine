// Character cloth, as Unreal's Chaos Cloth on a skeletal mesh (Godot has none): a Cloth3D's
// model section as a Jolt soft body whose skinned constraints carry its points along with the
// model's skin, each free up to its max distance and kept out of its backstop sphere, with long
// range attachments to the skinned points (Jolt's SoftBodySharedSettings::Skinned and LRA, the
// same ideas as Chaos's max distance, backstop and long range attachment constraints).
#include "space.hpp"

#include "slash/core/log.hpp"
#include "slash/world/wind.hpp"

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cfloat>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace slash::physics3d::detail {

namespace {

// The same as a SoftBody3D's.
constexpr float kPointRadius = 0.01F;
constexpr float kMaxLinearVelocity = 500.0F;

JPH::SoftBodyMotionProperties& motion_of(JPH::Body& body) {
    return static_cast<JPH::SoftBodyMotionProperties&>(*body.GetMotionProperties()); // NOLINT
}

const JPH::SoftBodyMotionProperties& motion_of(const JPH::Body& body) {
    return static_cast<const JPH::SoftBodyMotionProperties&>( // NOLINT
        *body.GetMotionProperties());
}

JPH::Mat44 to_jolt_matrix(const world::Affine3D& m) {
    return {JPH::Vec4(static_cast<float>(m.x.x), static_cast<float>(m.x.y),
                      static_cast<float>(m.x.z), 0.0F),
            JPH::Vec4(static_cast<float>(m.y.x), static_cast<float>(m.y.y),
                      static_cast<float>(m.y.z), 0.0F),
            JPH::Vec4(static_cast<float>(m.z.x), static_cast<float>(m.z.y),
                      static_cast<float>(m.z.z), 0.0F),
            JPH::Vec4(static_cast<float>(m.origin.x), static_cast<float>(m.origin.y),
                      static_cast<float>(m.origin.z), 1.0F)};
}

Vec3 scale_of(const world::Affine3D& m) {
    return {length(m.x), length(m.y), length(m.z)};
}

// Whether two components make the same body: everything but what a sync changes in place.
bool same_shape(world::Cloth3D a, world::Cloth3D b) {
    for (world::Cloth3D* cloth : {&a, &b}) {
        cloth->total_mass = 0.0F;
        cloth->gravity_scale = 0.0F;
        cloth->damping_coefficient = 0.0F;
        cloth->drag_coefficient = 0.0F;
        cloth->simulation_precision = 0;
        cloth->collision_layer = 0;
        cloth->collision_mask = 0;
    }
    return a == b;
}

// Godot's soft body stiffness mapping (see create_soft_body), as a compliance.
float compliance(float coefficient) {
    const float clamped = std::clamp(coefficient, 0.0F, 1.0F);
    return 1.0F / std::max(clamped * clamped * clamped * 100000.0F, 0.000001F);
}

// Each free point's inverse mass from the total mass; skinned points keep 0.
void set_masses(JPH::Array<JPH::SoftBodyVertex>& points, const std::vector<bool>& free,
                float total_mass) {
    const auto count = static_cast<float>(std::ranges::count(free, true));
    const float total = std::max(total_mass, 0.0F);
    const float inverse = total == 0.0F || count == 0.0F ? 1.0F : count / total;
    for (std::size_t i = 0; i < points.size() && i < free.size(); ++i) {
        points[i].mInvMass = free[i] ? inverse : 0.0F;
    }
}

// The joints where the skin puts them now, in the world, relative to the body's center of mass.
std::vector<JPH::Mat44> joint_matrices(const std::vector<world::Affine3D>& skin,
                                       JPH::RMat44Arg center) {
    const JPH::Mat44 offset = center.InversedRotationTranslation().ToMat44();
    std::vector<JPH::Mat44> out;
    out.reserve(skin.size());
    for (const world::Affine3D& joint : skin) {
        out.push_back(offset * to_jolt_matrix(joint));
    }
    return out;
}

} // namespace

void Space::create_cloth(SoftBody& soft, const world::Cloth3D& cloth, const world::Affine3D& place,
                         const std::vector<world::Affine3D>& joints) {
    const scene3d::ClothSection& section = *soft.section;
    // The creation settings own the shared settings alone, so they go when it does.
    JPH::SoftBodyCreationSettings settings;
    auto* shared = new JPH::SoftBodySharedSettings();
    settings.mSettings = shared;
    // Vertices at one place at rest are one point, as a SoftBody3D welds them; each point keeps
    // its first vertex's skin.
    soft.mesh_to_physics.assign(section.positions.size(), -1);
    std::map<std::tuple<real_t, real_t, real_t>, int> points;
    std::vector<std::size_t> first_vertex;
    for (std::size_t i = 0; i + 2 < section.indices.size(); i += 3) {
        std::array<int, 3> face{};
        for (std::size_t j = 0; j < 3; ++j) {
            const std::uint32_t index = section.indices[i + j];
            const Vec3 vertex = section.positions[index];
            const auto [found, added] = points.try_emplace({vertex.x, vertex.y, vertex.z},
                                                           static_cast<int>(first_vertex.size()));
            if (added) {
                const Vec3 placed = place.apply(vertex);
                shared->mVertices.emplace_back(JPH::Float3(static_cast<float>(placed.x),
                                                           static_cast<float>(placed.y),
                                                           static_cast<float>(placed.z)),
                                               JPH::Float3(0.0F, 0.0F, 0.0F), 1.0F);
                first_vertex.push_back(index);
            }
            face[j] = found->second;
            soft.mesh_to_physics[index] = found->second;
        }
        if (face[0] == face[1] || face[0] == face[2] || face[1] == face[2]) {
            continue;
        }
        // Jolt winds faces the other way.
        shared->mFaces.emplace_back(static_cast<JPH::uint32>(face[2]),
                                    static_cast<JPH::uint32>(face[1]),
                                    static_cast<JPH::uint32>(face[0]));
    }
    // A point's max distance is its smallest vertex's share of the component's: a vertex with
    // none skins it exactly.
    const std::vector<float> shares = scene3d::cloth_shares(cloth, section);
    std::vector<float> reach(first_vertex.size(), FLT_MAX);
    for (std::size_t v = 0; v < soft.mesh_to_physics.size(); ++v) {
        if (const int point = soft.mesh_to_physics[v]; point >= 0) {
            auto& at = reach[static_cast<std::size_t>(point)];
            at = std::min(at,
                          std::max(cloth.max_distance, 0.0F) * std::clamp(shares[v], 0.0F, 1.0F));
        }
    }
    std::vector<bool> free(first_vertex.size());
    for (std::size_t p = 0; p < free.size(); ++p) {
        free[p] = reach[p] > 0.0F;
        shared->mVertices[p].mInvMass = free[p] ? 1.0F : 0.0F;
    }
    JPH::SoftBodySharedSettings::VertexAttributes attributes;
    attributes.mCompliance = attributes.mShearCompliance = compliance(cloth.stiffness);
    attributes.mBendCompliance =
        cloth.bend_stiffness > 0.0F ? compliance(cloth.bend_stiffness) : FLT_MAX;
    attributes.mLRAType = cloth.long_range_attachment
                              ? JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance
                              : JPH::SoftBodySharedSettings::ELRAType::None;
    shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Dihedral);
    // Jolt skins a point's place at rest (its first position) by each joint's inverse bind: from
    // the world when it was made back to the model at rest, then by the joint's rest transform.
    for (std::size_t j = 0; j < section.rest.size(); ++j) {
        shared->mInvBindMatrices.emplace_back(static_cast<JPH::uint32>(j),
                                              to_jolt_matrix((place * section.rest[j]).inverse()));
    }
    const bool backstop = cloth.backstop_radius > 0.0F;
    for (std::size_t p = 0; p < first_vertex.size(); ++p) {
        JPH::SoftBodySharedSettings::Skinned skinned(
            static_cast<JPH::uint32>(p), free[p] ? reach[p] : 0.0F,
            backstop ? cloth.backstop_distance : FLT_MAX, backstop ? cloth.backstop_radius : 40.0F);
        const auto& weights = section.weights[first_vertex[p]];
        for (std::size_t k = 0; k < weights.size(); ++k) {
            skinned.mWeights[k] =
                JPH::SoftBodySharedSettings::SkinWeight(weights.at(k).first, weights.at(k).second);
        }
        skinned.NormalizeWeights();
        shared->mSkinnedConstraints.push_back(skinned);
    }
    shared->CalculateSkinnedConstraintNormals();
    shared->Optimize();

    soft.layer = cloth.collision_layer;
    soft.mask = cloth.collision_mask;
    soft.precision = std::max(cloth.simulation_precision, 0);
    soft.damping = std::max(cloth.damping_coefficient, 0.0F);
    soft.total_mass = cloth.total_mass;
    soft.drag = cloth.drag_coefficient;
    settings.mObjectLayer = layers_.to_object_layer(broad::kMoving, soft.layer, soft.mask);
    settings.mNumIterations = static_cast<JPH::uint32>(soft.precision);
    settings.mLinearDamping = soft.damping;
    settings.mGravityFactor = cloth.gravity_scale;
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
    set_masses(motion_of(*created).GetVertices(), free, soft.total_mass);
    // Every point starts where the skin puts it now.
    soft.skin.clear();
    for (const world::Affine3D& joint : joints) {
        soft.skin.push_back(place * joint);
    }
    const std::vector<JPH::Mat44> matrices =
        joint_matrices(soft.skin, created->GetCenterOfMassTransform());
    motion_of(*created).SkinVertices(created->GetCenterOfMassTransform(), matrices.data(),
                                     static_cast<JPH::uint>(matrices.size()), true, *temp_);
    interface.AddBody(soft.id, JPH::EActivation::Activate);
    ++added_;
}

void Space::sync_cloths(world::World& world, const world::Tree3D& tree) {
    const entt::registry& registry = world.ecs();
    cloths_.erase_if([&](entt::entity entity, const std::unique_ptr<SoftBody>& soft) {
        if (world.valid(entity) && registry.all_of<world::Cloth3D>(entity) &&
            !scene3d::cloth_key(world, entity).empty()) {
            return false;
        }
        destroy(*soft);
        return true;
    });
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    for (const entt::entity entity : registry.view<world::Cloth3D>()) {
        std::string key = scene3d::cloth_key(world, entity);
        if (key.empty()) {
            continue;
        }
        const auto& cloth = registry.get<world::Cloth3D>(entity);
        const world::Affine3D place = tree.global(entity);
        SoftBody* soft = nullptr;
        if (std::unique_ptr<SoftBody>* found = cloths_.find(entity); found != nullptr) {
            soft = found->get();
            if (soft->mesh_key != key || !soft->cloth || !same_shape(*soft->cloth, cloth) ||
                soft->scale != scale_of(place)) {
                destroy(*soft);
            }
        } else {
            auto made = std::make_unique<SoftBody>();
            made->entity = entity;
            made->name = std::string(world.id_of(entity));
            soft = cloths_.emplace(entity, std::move(made)).get();
        }
        if (soft->id.IsInvalid()) {
            if (soft->mesh_key == key && soft->cloth == cloth && soft->scale == scale_of(place)) {
                continue; // it couldn't be made, and nothing changed since
            }
            soft->mesh_key = std::move(key);
            soft->cloth = cloth;
            soft->scale = scale_of(place);
            auto section = scene3d::cloth_section(world, entity);
            if (!section) {
                continue; // scene validate reports it
            }
            soft->section = std::move(section).value();
            create_cloth(*soft, cloth, place,
                         scene3d::cloth_joints(world, tree, entity, *soft->section));
            soft->pose = nullptr;
            continue;
        }
        bool wake = false;
        if (cloth.collision_layer != soft->layer || cloth.collision_mask != soft->mask) {
            soft->layer = cloth.collision_layer;
            soft->mask = cloth.collision_mask;
            interface.SetObjectLayer(
                soft->id, layers_.to_object_layer(broad::kMoving, soft->layer, soft->mask));
        }
        if (!soft->cloth || cloth != *soft->cloth) {
            const JPH::BodyLockWrite lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
            if (lock.Succeeded()) {
                JPH::Body& body = lock.GetBody();
                JPH::SoftBodyMotionProperties& motion = motion_of(body);
                std::vector<bool> free;
                for (const JPH::SoftBodyVertex& point : motion.GetVertices()) {
                    free.push_back(point.mInvMass > 0.0F);
                }
                set_masses(motion.GetVertices(), free, cloth.total_mass);
                motion.SetNumIterations(
                    static_cast<JPH::uint32>(std::max(cloth.simulation_precision, 0)));
                motion.SetLinearDamping(std::max(cloth.damping_coefficient, 0.0F));
                motion.SetGravityFactor(cloth.gravity_scale);
            }
            soft->cloth = cloth;
            soft->total_mass = cloth.total_mass;
            soft->precision = std::max(cloth.simulation_precision, 0);
            soft->damping = std::max(cloth.damping_coefficient, 0.0F);
            soft->drag = cloth.drag_coefficient;
            wake = true;
        }
        if (wake) {
            interface.ActivateBody(soft->id);
        }
    }
}

void Space::pre_step_cloths(const world::World& world, const world::Tree3D& tree, float seconds) {
    if (cloths_.size() == 0) {
        return;
    }
    const world::WindField field(world, tree);
    const double time = world::wind_time(world);
    const std::vector<AreaWind> areas = area_winds(world, tree);
    for (auto& [entity, soft] : cloths_) {
        if (soft->id.IsInvalid()) {
            continue;
        }
        const world::Affine3D place = tree.global(entity);
        std::vector<world::Affine3D> skin;
        for (const world::Affine3D& joint :
             scene3d::cloth_joints(world, tree, entity, *soft->section)) {
            skin.push_back(place * joint);
        }
        const bool moved = skin != soft->skin;
        soft->skin = std::move(skin);
        JPH::BodyLockWrite lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
        if (!lock.Succeeded()) {
            continue;
        }
        JPH::Body& body = lock.GetBody();
        // The skinned points go where the skin is, and the free points' constraints with them.
        const std::vector<JPH::Mat44> matrices =
            joint_matrices(soft->skin, body.GetCenterOfMassTransform());
        motion_of(body).SkinVertices(body.GetCenterOfMassTransform(), matrices.data(),
                                     static_cast<JPH::uint>(matrices.size()), false, *temp_);
        const bool blown = blow_soft_body(*soft, soft->drag, body, field, time, areas, seconds);
        if (moved || blown) {
            lock.ReleaseLock();
            system_->GetBodyInterfaceNoLock().ActivateBody(soft->id);
        }
    }
}

void Space::read_cloths(const world::Tree3D& tree) {
    for (auto& [entity, soft] : cloths_) {
        if (soft->id.IsInvalid()) {
            continue;
        }
        const JPH::BodyLockRead lock(system_->GetBodyLockInterfaceNoLock(), soft->id);
        if (!lock.Succeeded() || (soft->pose != nullptr && !lock.GetBody().IsActive())) {
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        const JPH::SoftBodyMotionProperties& motion = motion_of(body);
        const JPH::Array<JPH::SoftBodyVertex>& points = motion.GetVertices();
        const JPH::RVec3 center = body.GetCenterOfMassPosition();
        // In the model's space, where its surfaces draw.
        const world::Affine3D model = tree.global(entity).inverse();
        std::vector<Vec3> positions(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            positions[i] = model.apply(from_jolt_place(center + points[i].mPosition));
        }
        std::vector<Vec3> normals(points.size());
        for (const JPH::SoftBodySharedSettings::Face& face : motion.GetFaces()) {
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
        // Its points in the world, for physics.soft_body.
        pose->positions.reserve(points.size());
        for (const JPH::SoftBodyVertex& point : points) {
            pose->positions.push_back(from_jolt_place(center + point.mPosition));
        }
        for (const scene3d::ClothSection::Piece& piece : soft->section->pieces) {
            scene3d::SoftBodyPose::Surface surface{
                .part = piece.part,
                .surface = piece.surface,
                .positions = std::vector<Vec3>(piece.count),
                .normals = std::vector<Vec3>(piece.count, Vec3{0.0F, 1.0F, 0.0F})};
            for (std::size_t v = 0; v < piece.count; ++v) {
                const std::size_t vertex = piece.first + v;
                if (const int point = soft->mesh_to_physics[vertex]; point >= 0) {
                    surface.positions[v] = positions[static_cast<std::size_t>(point)];
                    surface.normals[v] = normals[static_cast<std::size_t>(point)];
                } else {
                    surface.positions[v] = soft->section->positions[vertex];
                }
            }
            pose->surfaces.push_back(std::move(surface));
        }
        soft->pose = std::move(pose);
    }
}

} // namespace slash::physics3d::detail
