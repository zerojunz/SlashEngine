#include "slash/procedural/foliage.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/wind.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

namespace slash::procedural {

namespace {

constexpr float kPi = 3.14159265358979F;
constexpr std::string_view kSystem = "foliage";

// A pushed instance's spring: how far it leans and how fast that changes, in radians.
struct Spring {
    Vec2 lean;
    Vec2 speed;
};

using Key = std::tuple<std::uint32_t, std::size_t, std::size_t>;

struct CachedType {
    std::uint64_t version = 0;
    std::shared_ptr<const world::FoliageType> type;
    std::string problem;
};

// What the foliage system and the types read through assets keep, in the world's context.
struct FoliageState {
    std::mutex mutex; // types are asked for from drawing and threaded systems
    std::map<std::string, CachedType, std::less<>> types;
    std::map<Key, Spring> springs;
};

using StateHolder = std::shared_ptr<FoliageState>;

float round_thousandth(float value) {
    return std::round(value * 1000.0F) / 1000.0F;
}

float length2(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

Vec2 capped(Vec2 v, float most) {
    const float length = length2(v);
    if (length > most && length > 0.0F) {
        return {v.x * (most / length), v.y * (most / length)};
    }
    return v;
}

// A phase of its own for each instance, from its entity, group and index.
float phase_of(entt::entity entity, std::size_t group, std::size_t index) {
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            hash = (hash ^ ((value >> (i * 8)) & 0xFFU)) * 16777619U;
        }
    };
    mix(static_cast<std::uint32_t>(entity));
    mix(static_cast<std::uint32_t>(group));
    mix(static_cast<std::uint32_t>(index));
    return static_cast<float>(hash % 65536U) / 65536.0F * 2.0F * kPi;
}

// The wind's lean on an instance before characters push it: along the wind, by its speed,
// rocked by sway.
Vec2 wind_lean(const world::FoliageType& type, Vec2 wind, float phase, double time) {
    if (type.wind_bend == 0.0F) {
        return {};
    }
    float rock = 1.0F;
    if (type.sway != 0.0F) {
        const auto turns =
            static_cast<float>(std::fmod(time * static_cast<double>(type.sway_frequency), 1.0));
        rock += type.sway * cos_sin((turns * 2.0F * kPi) + phase).y;
    }
    return {wind.x * type.wind_bend * rock, wind.y * type.wind_bend * rock};
}

const FoliageState* state_of(const world::World& world) {
    const auto* holder = world.ecs().ctx().find<StateHolder>();
    return holder != nullptr ? holder->get() : nullptr;
}

Vec2 pushed(const world::World& world, entt::entity entity, std::size_t group, std::size_t index) {
    const FoliageState* state = state_of(world);
    if (state == nullptr) {
        return {};
    }
    const auto found = state->springs.find(Key{static_cast<std::uint32_t>(entity), group, index});
    return found != state->springs.end() ? found->second.lean : Vec2{};
}

// Steps a spring toward target by tick seconds, in small enough steps to stay steady.
void step_spring(Spring& spring, Vec2 target, const world::FoliageType& type, float tick) {
    const int steps = std::clamp(static_cast<int>(std::ceil(tick * 240.0F)), 1, 16);
    const float dt = tick / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        const Vec2 pull{
            (type.stiffness * (target.x - spring.lean.x)) - (type.damping * spring.speed.x),
            (type.stiffness * (target.y - spring.lean.y)) - (type.damping * spring.speed.y)};
        spring.speed = {spring.speed.x + (pull.x * dt), spring.speed.y + (pull.y * dt)};
        spring.lean = {spring.lean.x + (spring.speed.x * dt),
                       spring.lean.y + (spring.speed.y * dt)};
    }
}

// The push of the nearest interactor within radius: away from it, harder the closer it is.
template <typename Point>
std::optional<Vec2> push_from(const std::vector<Point>& movers, Vec2 base, float radius,
                              float bend) {
    std::optional<Vec2> best;
    float nearest = radius;
    for (const Point& at : movers) {
        const Vec2 away{base.x - at.x, base.y - at.y};
        const float distance = length2(away);
        if (distance >= nearest) {
            continue;
        }
        nearest = distance;
        const float strength = bend * (1.0F - (distance / radius));
        if (distance > 1e-4F) {
            best = Vec2{away.x / distance * strength, away.y / distance * strength};
        } else {
            best = Vec2{strength, 0.0F};
        }
    }
    return best;
}

struct Mover2 {
    float x = 0.0F;
    float y = 0.0F;
};

void run_foliage(world::World& world, FoliageState& state, double tick_seconds) {
    const auto tick = static_cast<float>(tick_seconds);
    entt::registry& ecs = world.ecs();
    std::map<Key, Spring> next;
    // Each Foliage3D's instances near a mover, in world x and z.
    if (!ecs.view<world::Foliage3D>().empty()) {
        const world::Tree3D tree(world);
        std::vector<Vec3> movers;
        const auto gather = [&]<typename Body>() {
            for (const entt::entity e : ecs.view<Body>()) {
                movers.push_back(tree.global(e).origin);
            }
        };
        gather.template operator()<world::CharacterBody3D>();
        gather.template operator()<world::RigidBody3D>();
        for (const auto [entity, foliage] : ecs.view<world::Foliage3D>().each()) {
            const world::Affine3D place = tree.global(entity);
            for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
                const auto type = foliage_type(world, foliage.groups[g].type);
                if (type == nullptr) {
                    continue;
                }
                const std::size_t count = foliage_count(foliage.groups[g]);
                for (std::size_t i = 0; i < count; ++i) {
                    const Key key{static_cast<std::uint32_t>(entity), g, i};
                    const auto had = state.springs.find(key);
                    std::optional<Vec2> target;
                    if (type->interaction_radius > 0.0F && !movers.empty()) {
                        const Vec3 base = place.apply(foliage_place(foliage.groups[g], i).position);
                        std::vector<Mover2> near;
                        for (const Vec3& mover : movers) {
                            if (std::abs(mover.y - base.y) < type->interaction_radius * 4.0F) {
                                near.push_back(
                                    {static_cast<float>(mover.x), static_cast<float>(mover.z)});
                            }
                        }
                        target = push_from(near, {base.x, base.z}, type->interaction_radius,
                                           type->interaction_bend);
                    }
                    if (!target && had == state.springs.end()) {
                        continue;
                    }
                    Spring spring = had != state.springs.end() ? had->second : Spring{};
                    step_spring(spring, target.value_or(Vec2{}), *type, tick);
                    if (target || length2(spring.lean) > 1e-3F || length2(spring.speed) > 1e-3F) {
                        next.emplace(key, spring);
                    }
                }
            }
        }
    }
    if (!ecs.view<world::Foliage2D>().empty()) {
        const world::Tree2D tree(world);
        std::vector<Mover2> movers;
        const auto gather = [&]<typename Body>() {
            for (const entt::entity e : ecs.view<Body>()) {
                const Vec2 at = tree.global_affine(e).origin;
                movers.push_back({static_cast<float>(at.x), static_cast<float>(at.y)});
            }
        };
        gather.template operator()<world::CharacterBody2D>();
        gather.template operator()<world::RigidBody2D>();
        for (const auto [entity, foliage] : ecs.view<world::Foliage2D>().each()) {
            const world::Affine2D place = tree.global_affine(entity);
            for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
                const auto type = foliage_type(world, foliage.groups[g].type);
                if (type == nullptr) {
                    continue;
                }
                const std::size_t count = foliage_count(foliage.groups[g]);
                for (std::size_t i = 0; i < count; ++i) {
                    const Key key{static_cast<std::uint32_t>(entity), g, i};
                    const auto had = state.springs.find(key);
                    std::optional<Vec2> target;
                    if (type->interaction_radius > 0.0F && !movers.empty()) {
                        const Vec2 base = place.apply(foliage_place(foliage.groups[g], i).position);
                        // Pushed sideways, away from the mover, by how near it is.
                        std::vector<Mover2> near;
                        for (const Mover2& mover : movers) {
                            if (std::abs(mover.y - base.y) < type->interaction_radius) {
                                near.push_back({mover.x, 0.0F});
                            }
                        }
                        target = push_from(near, {base.x, 0.0F}, type->interaction_radius,
                                           type->interaction_bend);
                        if (target) {
                            target->y = 0.0F;
                        }
                    }
                    if (!target && had == state.springs.end()) {
                        continue;
                    }
                    Spring spring = had != state.springs.end() ? had->second : Spring{};
                    step_spring(spring, target.value_or(Vec2{}), *type, tick);
                    if (target || length2(spring.lean) > 1e-3F || length2(spring.speed) > 1e-3F) {
                        next.emplace(key, spring);
                    }
                }
            }
        }
    }
    state.springs = std::move(next);
}

} // namespace

std::size_t foliage_count(const world::FoliageGroup3D& group) {
    return group.instances.size() / kFoliageFloats3D;
}

std::size_t foliage_count(const world::FoliageGroup2D& group) {
    return group.instances.size() / kFoliageFloats2D;
}

FoliagePlace3D foliage_place(const world::FoliageGroup3D& group, std::size_t index) {
    const float* v = group.instances.data() + (index * kFoliageFloats3D);
    return {.position = {v[0], v[1], v[2]}, .rotation = {v[3], v[4], v[5]}, .scale = v[6]};
}

FoliagePlace2D foliage_place(const world::FoliageGroup2D& group, std::size_t index) {
    const float* v = group.instances.data() + (index * kFoliageFloats2D);
    return {.position = {v[0], v[1]}, .rotation = v[2], .scale = v[3]};
}

void add_foliage(world::FoliageGroup3D& group, const FoliagePlace3D& place) {
    for (const real_t value :
         {place.position.x, place.position.y, place.position.z,
          static_cast<real_t>(place.rotation.x), static_cast<real_t>(place.rotation.y),
          static_cast<real_t>(place.rotation.z), static_cast<real_t>(place.scale)}) {
        group.instances.push_back(round_thousandth(static_cast<float>(value)));
    }
}

void add_foliage(world::FoliageGroup2D& group, const FoliagePlace2D& place) {
    for (const float value : {static_cast<float>(place.position.x),
                              static_cast<float>(place.position.y), place.rotation, place.scale}) {
        group.instances.push_back(round_thousandth(value));
    }
}

void remove_foliage(world::FoliageGroup3D& group, std::size_t index) {
    const auto at = static_cast<std::ptrdiff_t>(index * kFoliageFloats3D);
    group.instances.erase(group.instances.begin() + at,
                          group.instances.begin() + at +
                              static_cast<std::ptrdiff_t>(kFoliageFloats3D));
}

void remove_foliage(world::FoliageGroup2D& group, std::size_t index) {
    const auto at = static_cast<std::ptrdiff_t>(index * kFoliageFloats2D);
    group.instances.erase(group.instances.begin() + at,
                          group.instances.begin() + at +
                              static_cast<std::ptrdiff_t>(kFoliageFloats2D));
}

std::shared_ptr<const world::FoliageType>
foliage_type(const world::World& world, const std::string& path, std::string* problem) {
    const auto fail = [problem](std::string why) -> std::shared_ptr<const world::FoliageType> {
        if (problem != nullptr) {
            *problem = std::move(why);
        }
        return nullptr;
    };
    if (path.empty()) {
        return fail("names no .foliage.json file");
    }
    if (!world::is_foliage_path(path)) {
        return fail("\"" + path + "\" is not a .foliage.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return fail("foliage types can't be read here");
    }
    auto loaded = assets->load<json::Value>(path);
    if (!loaded) {
        return fail(loaded.error().message);
    }
    const std::uint64_t version = loaded.value().version();
    const auto* holder = world.ecs().ctx().find<StateHolder>();
    FoliageState* state = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (state != nullptr) {
        lock = std::unique_lock<std::mutex>(state->mutex);
        const auto found = state->types.find(path);
        if (found != state->types.end() && found->second.version == version) {
            if (found->second.type == nullptr) {
                return fail(found->second.problem);
            }
            return found->second.type;
        }
    }
    CachedType cached;
    cached.version = version;
    auto parsed = world::parse_foliage(*loaded.value());
    if (parsed) {
        cached.type = std::make_shared<const world::FoliageType>(std::move(parsed).value());
    } else {
        cached.problem = parsed.error().message;
    }
    if (state != nullptr) {
        state->types.insert_or_assign(path, cached);
    }
    if (cached.type == nullptr) {
        return fail(cached.problem);
    }
    return cached.type;
}

FoliagePainted paint_foliage(const world::FoliageType& type, const FoliageStroke& stroke,
                             std::span<const ScatterTriangle> ground, std::span<const Vec3> avoid) {
    const bool flat = type.space == "2d";
    ScatterLayer layer;
    layer.name = "foliage";
    const Vec3 c = stroke.center;
    const float r = stroke.radius;
    // The circle's bounding square at the type's density, whose corners are dropped after.
    if (flat) {
        layer.area = std::array<float, 4>{static_cast<float>(c.x - r), static_cast<float>(c.y - r),
                                          static_cast<float>(c.x + r), static_cast<float>(c.y + r)};
    } else {
        layer.area = std::array<float, 4>{static_cast<float>(c.x - r), static_cast<float>(c.z - r),
                                          static_cast<float>(c.x + r), static_cast<float>(c.z + r)};
    }
    layer.density = type.density * std::clamp(stroke.strength, 0.0F, 1.0F);
    layer.spacing = type.spacing;
    layer.slope = type.slope;
    layer.height = type.height;
    layer.align_to_normal = type.align_to_normal;
    layer.rotate_random = flat || !type.random_yaw ? 0.0F : 1.0F;
    layer.tilt_random = flat ? 0.0F : type.tilt / kPi;
    layer.scale = (type.scale.x + type.scale.y) * 0.5F;
    layer.scale_random = (type.scale.y - type.scale.x) * 0.5F;
    layer.offset = flat ? 0.0F : -type.sink;
    const ScatterResult result = scatter(layer, 0, stroke.seed, flat, ground, avoid);
    FoliagePainted out;
    out.wanted = result.wanted;
    out.slope = result.slope;
    out.height = result.height;
    out.spacing = result.spacing;
    std::uint32_t flip = stroke.seed * 2654435761U;
    for (const ScatterPlace& place : result.places) {
        const float dx = place.position.x - c.x;
        const float dy = flat ? place.position.y - c.y : place.position.z - c.z;
        if ((dx * dx) + (dy * dy) > r * r) {
            continue;
        }
        FoliagePlace3D painted{
            .position = place.position, .rotation = place.rotation, .scale = place.scale};
        if (flat) {
            painted.rotation = {};
            painted.position.y += type.sink;
            flip = (flip * 1664525U) + 1013904223U;
            if (type.random_flip && (flip >> 31U) != 0U) {
                painted.scale = -painted.scale;
            }
        }
        out.places.push_back(painted);
    }
    return out;
}

Vec2 foliage_lean_3d(const world::World& world, entt::entity entity, std::size_t group,
                     std::size_t index, const world::FoliageType& type, Vec3 /*base*/, Vec3 wind) {
    const Vec2 blown =
        wind_lean(type, {wind.x, wind.z}, phase_of(entity, group, index), world::wind_time(world));
    const Vec2 push = pushed(world, entity, group, index);
    return capped({blown.x + push.x, blown.y + push.y}, type.max_bend);
}

float foliage_lean_2d(const world::World& world, entt::entity entity, std::size_t group,
                      std::size_t index, const world::FoliageType& type, Vec2 /*base*/, Vec2 wind) {
    // Wind of 100 pixels a second counts as 1 meter a second does in 3D.
    const Vec2 blown = wind_lean(type, {wind.x / 100.0F, 0.0F}, phase_of(entity, group, index),
                                 world::wind_time(world));
    const Vec2 push = pushed(world, entity, group, index);
    return static_cast<float>(std::clamp<real_t>(blown.x + push.x, -type.max_bend, type.max_bend));
}

std::vector<FoliagePush> foliage_pushes(const world::World& world) {
    std::vector<FoliagePush> out;
    if (const FoliageState* state = state_of(world); state != nullptr) {
        for (const auto& [key, spring] : state->springs) {
            out.push_back({.entity = static_cast<entt::entity>(std::get<0>(key)),
                           .group = std::get<1>(key),
                           .index = std::get<2>(key),
                           .lean = spring.lean});
        }
    }
    return out;
}

void install_foliage(world::World& world) {
    const std::vector<std::string_view> names = world.system_names();
    if (std::ranges::find(names, kSystem) != names.end()) {
        return;
    }
    auto state = std::make_shared<FoliageState>();
    world.ecs().ctx().insert_or_assign(StateHolder{state});
    world.add_reset([state](world::World& /*world*/) { state->springs.clear(); });
    world.add_system(
        std::string(kSystem),
        [state](world::World& w, double tick_seconds) { run_foliage(w, *state, tick_seconds); },
        {.doc = "Bends Foliage3D and Foliage2D instances away from character and rigid bodies "
                "within their type's interaction_radius and springs them back.",
         .priority = 970});
}

} // namespace slash::procedural
