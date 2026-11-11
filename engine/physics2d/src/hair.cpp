// Hair and fur in 2D physics, as the 3D space steps a groom's guides: each Hair2D's guide strands
// are carried by its entity's transform and stepped after the bodies have moved, by gravity,
// WindSource2Ds' wind and PhysicsField2Ds, kept off the circles, capsules and rectangles of the
// body they grow on, all at z 0.
#include "slash/world/hair.hpp"

#include "space.hpp"

#include "slash/world/physics_field.hpp"
#include "slash/world/wind.hpp"

#include <cstdint>
#include <string_view>

namespace slash::physics2d::detail {

namespace {

std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

Vec3 lift(Vec2 v) {
    return {v.x, v.y, 0.0F};
}

// An entity's transform as a 3D one in the plane z = 0.
world::Affine3D frame_of(const world::Affine2D& place) {
    world::Affine3D out;
    out.x = lift(place.x);
    out.y = lift(place.y);
    out.z = {0.0F, 0.0F, 1.0F};
    out.origin = lift(place.origin);
    return out;
}

} // namespace

void Space::step_hair_2d(world::World& world, float seconds) {
    const entt::registry& registry = world.ecs();
    hairs_.erase_if([&](entt::entity entity, const std::unique_ptr<Hair2DBody>& /*hair*/) {
        return !world.valid(entity) || !registry.all_of<world::Hair2D>(entity);
    });
    const auto view = registry.view<const world::Hair2D>();
    if (view.empty()) {
        return;
    }
    // Where the bodies are now that they have moved.
    const world::Tree2D tree(world);
    const world::PhysicsFields2D fields(world, tree);
    const world::WindField2D wind(world, tree);
    const double wind_time = world::wind_time(world);
    const double field_time = static_cast<double>(world.tick()) * static_cast<double>(seconds);
    const Vec2 gravity = settings_.gravity_vector * settings_.gravity;
    for (const entt::entity entity : view) {
        const auto& hair = registry.get<const world::Hair2D>(entity);
        Hair2DBody* body = nullptr;
        if (std::unique_ptr<Hair2DBody>* found = hairs_.find(entity); found != nullptr) {
            body = found->get();
        } else {
            auto made = std::make_unique<Hair2DBody>();
            made->name = std::string(world.id_of(entity));
            body = hairs_.emplace(entity, std::move(made)).get();
        }
        auto grown = world::hair_growth_2d(world, entity);
        if (!grown) {
            body->key.clear();
            body->out = {};
            continue;
        }
        if (body->key != grown.value().key) {
            body->key = grown.value().key;
            body->strands = {};
        }
        const world::Groom& groom = *grown.value().groom;
        const world::HairGrowth2D& growth = *grown.value().growth;
        const int points = std::max(groom.segments, 1) + 1;
        const world::Affine2D place = tree.global_affine(entity);
        std::vector<Vec3> targets;
        targets.reserve(growth.guides.size() * static_cast<std::size_t>(points));
        for (const world::HairGuide2D& guide : growth.guides) {
            for (const Vec2 point : guide.rest) {
                targets.push_back(lift(place.apply(point)));
            }
        }
        const bool moving = hair.simulate && groom.simulate;
        const std::vector<world::StrandCollider> colliders =
            moving && hair.collide
                ? world::strand_colliders_2d(world, tree, world::tree_root_2d(tree, entity))
                : std::vector<world::StrandCollider>{};
        const std::uint32_t seed = seed_of(body->name);
        world::StrandStep step{
            .seconds = moving ? seconds : 0.0F,
            .steps = groom.precision,
            .gravity = lift(gravity) * groom.gravity_scale,
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
            step.wind = [&wind, wind_time](Vec3 at) {
                return lift(wind.at({at.x, at.y}, wind_time));
            };
        }
        if (!fields.empty()) {
            // A point of hair is pushed as a particle of a kilogram is.
            step.force = [&fields, seed, field_time](Vec3 at) {
                const world::FieldEffect effect =
                    fields.at({at.x, at.y}, ~std::uint32_t{0}, seed, field_time);
                return Vec3{effect.force.x, effect.force.y, 0.0F};
            };
        }
        world::step_strands(body->strands, points, targets, step);
        const world::Affine3D frame = frame_of(place);
        body->out.points = points;
        body->out.previous = body->strands.previous;
        body->out.current = body->strands.current;
        body->out.previous_frames = body->out.frames.size() == growth.guides.size()
                                        ? body->out.frames
                                        : std::vector<world::Affine3D>(growth.guides.size(), frame);
        body->out.frames.assign(growth.guides.size(), frame);
    }
}

const world::HairStrands* Space::hair_strands(entt::entity entity) const {
    const std::unique_ptr<Hair2DBody>* found = hairs_.find(entity);
    return found != nullptr && !(*found)->key.empty() ? &(*found)->out : nullptr;
}

} // namespace slash::physics2d::detail
