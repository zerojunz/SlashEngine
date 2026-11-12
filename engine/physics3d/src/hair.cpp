// Hair and fur in the one simulation, as Chaos steps a groom's guides with the rest of the
// world: each HairInstance3D's guide strands are carried by its model's skin and stepped after
// the bodies have moved, by gravity, WindSource3Ds' wind and PhysicsField3Ds, kept off the
// spheres, capsules and boxes of the body they grow on.
#include "slash/scene3d/hair.hpp"

#include "space.hpp"

#include "slash/world/hair.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/wind.hpp"

#include <cstdint>
#include <string_view>

namespace slash::physics3d::detail {

namespace {

std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

} // namespace

void Space::step_hair(world::World& world, float seconds) {
    const entt::registry& registry = world.ecs();
    hairs_.erase_if([&](entt::entity entity, const std::unique_ptr<Hair3DBody>& /*hair*/) {
        return !world.valid(entity) || !registry.all_of<world::HairInstance3D>(entity);
    });
    const auto view = registry.view<const world::HairInstance3D>();
    if (view.empty()) {
        return;
    }
    // Where the bodies are now that they have moved.
    const world::Tree3D tree(world);
    const world::PhysicsFields3D fields(world, tree);
    const world::WindField wind(world, tree);
    const double wind_time = world::wind_time(world);
    const double field_time = static_cast<double>(world.tick()) * static_cast<double>(seconds);
    const Vec3 gravity = settings_.gravity_vector * settings_.gravity;
    for (const entt::entity entity : view) {
        const auto& hair = registry.get<const world::HairInstance3D>(entity);
        Hair3DBody* body = nullptr;
        if (std::unique_ptr<Hair3DBody>* found = hairs_.find(entity); found != nullptr) {
            body = found->get();
        } else {
            auto made = std::make_unique<Hair3DBody>();
            made->name = std::string(world.id_of(entity));
            body = hairs_.emplace(entity, std::move(made)).get();
        }
        // Grown again when its model, groom or maps change.
        const std::string key = scene3d::hair_key(world, entity);
        if (key.empty()) {
            body->growth = nullptr;
            body->out = {};
            continue;
        }
        if (body->growth == nullptr || body->growth->key != key) {
            auto growth = scene3d::hair_growth(world, entity);
            body->growth = growth ? growth.value() : nullptr;
            body->strands = {};
        }
        if (body->growth == nullptr) {
            body->out = {};
            continue;
        }
        const scene3d::HairGrowth3D& growth = *body->growth;
        const world::Groom& groom = *growth.groom;
        std::vector<world::Affine3D> frames = scene3d::hair_frames(world, tree, entity, growth);
        const std::vector<Vec3> targets = scene3d::hair_targets(growth, frames);
        const bool moving = hair.simulate && groom.simulate;
        const std::vector<world::StrandCollider> colliders =
            moving && hair.collide
                ? world::strand_colliders_3d(world, tree, world::tree_root_3d(tree, entity))
                : std::vector<world::StrandCollider>{};
        const std::uint32_t seed = seed_of(body->name);
        world::StrandStep step{
            .seconds = moving ? seconds : 0.0F,
            .steps = groom.precision,
            .gravity = gravity * groom.gravity_scale,
            .stiffness = groom.stiffness,
            .tip_stiffness = groom.tip_stiffness,
            .bend_stiffness = groom.bend_stiffness,
            .damping = groom.damping,
            .drag_coefficient = groom.drag_coefficient,
            .point_radius = groom.point_radius,
            .wind = {},
            .force = {},
            .colliders = colliders,
        };
        if (!wind.empty()) {
            step.wind = [&wind, wind_time](Vec3 at) { return wind.at(at, wind_time); };
        }
        if (!fields.empty()) {
            // A point of hair is pushed as a particle of a kilogram is.
            step.force = [&fields, seed, field_time](Vec3 at) {
                const world::FieldEffect effect =
                    fields.at(at, ~std::uint32_t{0}, seed, field_time);
                return effect.force;
            };
        }
        world::step_strands(body->strands, growth.points, targets, step);
        body->out.points = growth.points;
        body->out.previous = body->strands.previous;
        body->out.current = body->strands.current;
        body->out.previous_frames =
            body->out.frames.size() == frames.size() ? body->out.frames : frames;
        body->out.frames = std::move(frames);
    }
}

const world::HairStrands* Space::hair_strands(entt::entity entity) const {
    const std::unique_ptr<Hair3DBody>* found = hairs_.find(entity);
    return found != nullptr && (*found)->growth != nullptr ? &(*found)->out : nullptr;
}

} // namespace slash::physics3d::detail
