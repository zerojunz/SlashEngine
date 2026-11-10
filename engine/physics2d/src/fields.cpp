// Physics materials and fields on 2D bodies: the .physics_material.json files bodies name, and
// what PhysicsField2Ds do to rigid bodies each step, as Unreal's Chaos physical materials and
// fields.

#include "space.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/log.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace slash::physics2d::detail {

namespace {

// FNV-1a of a body's ID, so its noise follows it from session to session.
std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

} // namespace

std::optional<world::PhysicsMaterial> Space::material(const world::World& world,
                                                      const std::string& path) {
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return std::nullopt;
    }
    auto loaded = assets->load<json::Value>(path);
    CachedMaterial& cached = materials_[path];
    if (!loaded) {
        if (cached.version != 1) {
            log::warn("physics material can't be read",
                      {{"path", path}, {"problem", loaded.error().message}});
            cached = {.version = 1, .material = std::nullopt};
        }
        return std::nullopt;
    }
    // Versions start at 1, so 1 above is never a loaded one's; keep them apart by two.
    const std::uint64_t version = loaded.value().version() + 1;
    if (cached.version != version) {
        auto parsed = world::parse_physics_material(*loaded.value());
        if (!parsed) {
            log::warn("physics material is invalid",
                      {{"path", path}, {"problem", parsed.error().message}});
            cached = {.version = version, .material = std::nullopt};
        } else {
            cached = {.version = version, .material = std::move(parsed.value())};
        }
    }
    return cached.material;
}

void Space::apply_fields(world::World& world, const world::Tree2D& tree, float seconds) {
    const world::PhysicsFields2D fields(world, tree);
    // Whole ticks of game time, so noise is the same at the same tick in every session.
    const double time = static_cast<double>(world.tick()) * static_cast<double>(seconds);
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid) {
            continue;
        }
        const world::FieldEffect effect =
            fields.empty() ? world::FieldEffect{}
                           : fields.at(from_b2(b2Body_GetWorldCenterOfMass(body->id)),
                                       body->shape_key.layer, seed_of(body->name), time);
        // Anchors hold dynamic bodies as kinematic ones, and let go when they leave.
        if (effect.anchored && !body->anchored && b2Body_GetType(body->id) == b2_dynamicBody) {
            body->anchored = true;
            body->anchored_mass = b2Body_GetMassData(body->id);
            b2Body_SetType(body->id, b2_kinematicBody);
            b2Body_SetLinearVelocity(body->id, b2Vec2_zero);
            b2Body_SetAngularVelocity(body->id, 0.0F);
            body->rigid_key.type = b2_kinematicBody;
            continue;
        }
        if (!effect.anchored && body->anchored) {
            body->anchored = false;
            const auto& rigid = world.ecs().get<world::RigidBody2D>(entity);
            b2BodyType type = b2_dynamicBody;
            if (rigid.freeze) {
                type = rigid.freeze_mode == "kinematic" ? b2_kinematicBody : b2_staticBody;
            }
            body->rigid_key.type = type;
            if (type != b2_kinematicBody) {
                b2Body_SetType(body->id, type);
                if (type == b2_dynamicBody) {
                    b2Body_SetMassData(body->id, body->anchored_mass);
                    // It goes on as its components last said.
                    b2Body_SetLinearVelocity(body->id, to_b2(body->linear_velocity));
                    b2Body_SetAngularVelocity(body->id, body->angular_velocity);
                    b2Body_SetAwake(body->id, true);
                }
            }
        }
        if (body->anchored) {
            // Held where it is, whatever velocity its components were given since.
            b2Body_SetLinearVelocity(body->id, b2Vec2_zero);
            b2Body_SetAngularVelocity(body->id, 0.0F);
            continue;
        }
        if (effect.empty() || b2Body_GetType(body->id) != b2_dynamicBody) {
            continue;
        }
        if (effect.killed) {
            doomed_.push_back(entity);
            continue;
        }
        const Vec2 force{effect.force.x, effect.force.y};
        if (force != Vec2{}) {
            b2Body_ApplyForceToCenter(body->id, to_b2(force), true);
        }
        if (effect.velocity) {
            b2Body_SetLinearVelocity(body->id, {effect.velocity->x, effect.velocity->y});
            b2Body_SetAwake(body->id, true);
        }
        if (effect.sleep_below && b2Body_IsAwake(body->id) && force == Vec2{} && !effect.velocity &&
            b2Length(b2Body_GetLinearVelocity(body->id)) < *effect.sleep_below) {
            b2Body_SetAwake(body->id, false);
        }
    }
}

void Space::destroy_doomed(world::World& world) {
    for (const entt::entity entity : std::exchange(doomed_, {})) {
        if (world.valid(entity)) {
            world.emit("field_killed", world.id_of(entity));
            world.destroy(entity);
        }
    }
}

} // namespace slash::physics2d::detail
