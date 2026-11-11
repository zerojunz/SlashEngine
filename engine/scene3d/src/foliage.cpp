#include "slash/scene3d/foliage.hpp"

#include "slash/procedural/foliage.hpp"
#include "slash/world/world.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace slash::scene3d {

namespace {

struct Kept {
    world::Foliage3D foliage;
    std::vector<std::shared_ptr<const world::FoliageType>> types;
    std::shared_ptr<const std::vector<GridShape>> shapes;
};

struct FoliageCollisionCache {
    std::mutex mutex;
    std::map<entt::entity, Kept> kept;
};

using CacheHolder = std::shared_ptr<FoliageCollisionCache>;

// A type's collision as a shape in an instance's space: a capsule or cylinder standing up, a
// sphere or a box, its middle at offset.
std::optional<world::CollisionShape3D> shape_of(const world::FoliageCollision& collision) {
    world::CollisionShape3D shape;
    if (collision.shape == "capsule" || collision.shape == "cylinder" ||
        collision.shape == "sphere") {
        shape.shape = collision.shape;
        shape.radius = collision.radius;
        shape.height = collision.height;
    } else if (collision.shape == "box") {
        shape.shape = "box";
        shape.size = collision.size;
    } else {
        return std::nullopt;
    }
    shape.position = collision.offset;
    return shape;
}

std::shared_ptr<const std::vector<GridShape>>
make_shapes(const world::Foliage3D& foliage,
            const std::vector<std::shared_ptr<const world::FoliageType>>& types) {
    auto shapes = std::make_shared<std::vector<GridShape>>();
    for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
        if (types[g] == nullptr || types[g]->space != "3d") {
            continue;
        }
        const std::optional<world::CollisionShape3D> shape = shape_of(types[g]->collision);
        if (!shape.has_value()) {
            continue;
        }
        const world::CollisionShape3D& base = shape.value();
        world::Transform3D offset;
        offset.position = base.position;
        const std::size_t count = procedural::foliage_count(foliage.groups[g]);
        for (std::size_t i = 0; i < count; ++i) {
            const procedural::FoliagePlace3D place =
                procedural::foliage_place(foliage.groups[g], i);
            world::Transform3D local;
            local.position = place.position;
            local.rotation = place.rotation;
            local.scale = {place.scale, place.scale, place.scale};
            GridShape made;
            made.cell = {static_cast<int>(g), static_cast<int>(i), 0};
            made.shape = base;
            made.shape.position = {};
            made.local = world::affine(local) * world::affine(offset);
            shapes->push_back(std::move(made));
        }
    }
    return shapes;
}

} // namespace

void install_foliage_collision(world::World& world) {
    if (world.ecs().ctx().find<CacheHolder>() != nullptr) {
        return;
    }
    auto cache = std::make_shared<FoliageCollisionCache>();
    world.ecs().ctx().emplace<CacheHolder>(cache);
}

std::vector<GridBody> foliage_bodies(const world::World& world) {
    std::vector<GridBody> bodies;
    const auto& ecs = world.ecs();
    const auto view = ecs.view<const world::Foliage3D>();
    if (view.empty()) {
        return bodies;
    }
    const auto* holder = ecs.ctx().find<CacheHolder>();
    FoliageCollisionCache* cache = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        std::erase_if(cache->kept, [&](const auto& each) {
            return !ecs.valid(each.first) || !ecs.all_of<world::Foliage3D>(each.first);
        });
    }
    for (const entt::entity entity : view) {
        const auto& foliage = view.get<const world::Foliage3D>(entity);
        std::vector<std::shared_ptr<const world::FoliageType>> types;
        types.reserve(foliage.groups.size());
        for (const world::FoliageGroup3D& group : foliage.groups) {
            types.push_back(procedural::foliage_type(world, group.type));
        }
        std::shared_ptr<const std::vector<GridShape>> shapes;
        if (cache != nullptr) {
            Kept& kept = cache->kept[entity];
            if (kept.shapes == nullptr || kept.foliage != foliage || kept.types != types) {
                kept = {.foliage = foliage, .types = types, .shapes = make_shapes(foliage, types)};
            }
            shapes = kept.shapes;
        } else {
            shapes = make_shapes(foliage, types);
        }
        if (shapes->empty()) {
            continue;
        }
        bodies.push_back({.entity = entity,
                          .layer = foliage.collision_layer,
                          .mask = foliage.collision_mask,
                          .friction = 1.0F,
                          .bounce = 0.0F,
                          .shapes = std::move(shapes)});
    }
    return bodies;
}

} // namespace slash::scene3d
