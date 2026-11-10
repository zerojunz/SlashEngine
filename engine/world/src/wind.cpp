#include "slash/world/wind.hpp"

#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>

namespace slash::world {

namespace {

constexpr std::string_view kSystem = "wind";
constexpr real_t kTiny = 1e-6F;

struct WindClock {
    double seconds = 0.0;
};

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// FNV-1a of an entity's ID, so a source's gusts follow it from session to session.
std::uint32_t hash_id(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

// A whole number's value from 0 to 1, as PCG's output permutation of it with the seed.
float lattice(std::int64_t at, std::uint32_t seed) {
    std::uint32_t state = (static_cast<std::uint32_t>(at) * 747796405U) + 2891336453U + seed;
    state ^= static_cast<std::uint32_t>(static_cast<std::uint64_t>(at) >> 32U) * 2654435761U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return static_cast<float>(((word >> 22U) ^ word) >> 8U) * (1.0F / 16777216.0F);
}

// Smooth noise from 0 to 1 along a line: the lattice values on each side of x, eased together.
float noise(double x, std::uint32_t seed) {
    const double whole = std::floor(x);
    const auto t = static_cast<float>(x - whole);
    const auto at = static_cast<std::int64_t>(whole);
    const float a = lattice(at, seed);
    const float b = lattice(at + 1, seed);
    const float eased = t * t * (3.0F - (2.0F * t));
    return a + ((b - a) * eased);
}

} // namespace

WindField::WindField(const World& world, const Tree3D& tree) {
    // In the registry's storage order, so sums come out the same in every session.
    std::vector<entt::entity> entities;
    for (const auto [entity, source] : world.ecs().view<const WindSource3D>().each()) {
        if (source.enabled && tree.visible(entity)) {
            entities.push_back(entity);
        }
    }
    for (const entt::entity entity : entities) {
        const Affine3D global = tree.global(entity);
        Vec3 direction = global.basis({0.0F, 0.0F, -1.0F});
        const real_t size = length(direction);
        if (size <= kTiny) {
            continue;
        }
        direction = direction * (1.0F / size);
        sources_.push_back({.settings = world.ecs().get<const WindSource3D>(entity),
                            .origin = global.origin,
                            .direction = direction,
                            .seed = hash_id(world.id_of(entity))});
    }
}

Vec3 WindField::at(Vec3 position, double time) const {
    Vec3 total;
    for (const Source& source : sources_) {
        const WindSource3D& wind = source.settings;
        Vec3 direction = source.direction;
        real_t along = dot(position - source.origin, direction);
        real_t falloff = 1.0F;
        if (wind.point_wind) {
            const Vec3 out = position - source.origin;
            const real_t distance = length(out);
            if (distance <= kTiny || distance >= wind.radius) {
                continue;
            }
            direction = out * (1.0F / distance);
            along = distance;
            falloff = 1.0F - (distance / wind.radius);
        }
        // Gusts travel with the wind at its speed, so a gust reaches a point downwind later.
        double phase = static_cast<double>(wind.gust_frequency) * time;
        if (wind.strength > 0.0F) {
            phase -= static_cast<double>(along) * static_cast<double>(wind.gust_frequency) /
                     static_cast<double>(wind.strength);
        }
        const float gust = noise(phase, source.seed);
        const float amount =
            wind.min_gust_amount + ((wind.max_gust_amount - wind.min_gust_amount) * gust);
        Vec3 blow = direction * (wind.strength * (1.0F + amount));
        if (wind.turbulence > 0.0F) {
            const Vec3 up =
                std::abs(direction.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
            Vec3 side = cross(direction, up);
            side = side * (1.0F / length(side));
            const Vec3 lift = cross(side, direction);
            const double quick = (phase * 2.3) + 17.0;
            const float across = (noise(quick, source.seed ^ 0x9E3779B9U) * 2.0F) - 1.0F;
            const float rise = (noise(quick + 31.0, source.seed ^ 0x7F4A7C15U) * 2.0F) - 1.0F;
            blow = blow + (((side * across) + (lift * rise)) * (wind.strength * wind.turbulence));
        }
        total = total + (blow * falloff);
    }
    return total;
}

WindField2D::WindField2D(const World& world, const Tree2D& tree) {
    for (const auto [entity, source] : world.ecs().view<const WindSource2D>().each()) {
        if (!source.enabled) {
            continue;
        }
        const Affine2D global = tree.global_affine(entity);
        const real_t size = std::sqrt((global.x.x * global.x.x) + (global.x.y * global.x.y));
        if (size <= kTiny) {
            continue;
        }
        sources_.push_back({.settings = source,
                            .origin = global.origin,
                            .direction = global.x * (1.0F / size),
                            .seed = hash_id(world.id_of(entity))});
    }
}

Vec2 WindField2D::at(Vec2 position, double time) const {
    Vec2 total;
    for (const Source& source : sources_) {
        const WindSource2D& wind = source.settings;
        Vec2 direction = source.direction;
        const Vec2 out = position - source.origin;
        real_t along = (out.x * direction.x) + (out.y * direction.y);
        real_t falloff = 1.0F;
        if (wind.point_wind) {
            const real_t distance = std::sqrt((out.x * out.x) + (out.y * out.y));
            if (distance <= kTiny || distance >= wind.radius) {
                continue;
            }
            direction = out * (1.0F / distance);
            along = distance;
            falloff = 1.0F - (distance / wind.radius);
        }
        double phase = static_cast<double>(wind.gust_frequency) * time;
        if (wind.strength > 0.0F) {
            phase -= static_cast<double>(along) * static_cast<double>(wind.gust_frequency) /
                     static_cast<double>(wind.strength);
        }
        const float gust = noise(phase, source.seed);
        const float amount =
            wind.min_gust_amount + ((wind.max_gust_amount - wind.min_gust_amount) * gust);
        Vec2 blow = direction * (wind.strength * (1.0F + amount));
        if (wind.turbulence > 0.0F) {
            const double quick = (phase * 2.3) + 17.0;
            const float across = (noise(quick, source.seed ^ 0x9E3779B9U) * 2.0F) - 1.0F;
            const Vec2 side{-direction.y, direction.x};
            blow = blow + (side * (wind.strength * wind.turbulence * across));
        }
        total = total + (blow * falloff);
    }
    return total;
}

double wind_time(const World& world) {
    const auto* clock = world.ecs().ctx().find<std::shared_ptr<WindClock>>();
    return clock != nullptr ? (*clock)->seconds : 0.0;
}

void add_wind_system(World& world) {
    const std::vector<std::string_view> names = world.system_names();
    if (std::ranges::find(names, kSystem) != names.end()) {
        return;
    }
    auto clock = std::make_shared<WindClock>();
    world.ecs().ctx().insert_or_assign(clock);
    world.add_reset([clock](World& /*world*/) { clock->seconds = 0.0; });
    world.add_system(
        std::string(kSystem),
        [clock](World& /*world*/, double tick_seconds) { clock->seconds += tick_seconds; },
        {.doc = "Counts the seconds the wind has blown, which WindSource3Ds' gusts follow.",
         .priority = -650});
}

} // namespace slash::world
