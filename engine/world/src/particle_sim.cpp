#include "slash/world/particle_sim.hpp"

#include "slash/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace slash::world {
namespace {

float dot(Vec3f a, Vec3f b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

float length(Vec3f v) {
    return std::sqrt(dot(v, v));
}

Vec3f flattened(Vec3f v, bool flat) {
    return flat ? Vec3f{v.x, v.y, 0.0F} : v;
}

// Spreads a cell coordinate's low 21 bits out to every third bit.
std::uint64_t spread(std::uint32_t v) {
    std::uint64_t x = v & 0x1FFFFFU;
    x = (x | (x << 32U)) & 0x1F00000000FFFFULL;
    x = (x | (x << 16U)) & 0x1F0000FF0000FFULL;
    x = (x | (x << 8U)) & 0x100F00F00F00F00FULL;
    x = (x | (x << 4U)) & 0x10C30C30C30C30C3ULL;
    x = (x | (x << 2U)) & 0x1249249249249249ULL;
    return x;
}

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// A lattice point's value from -1 to 1.
float lattice(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = hash(seed ^ 0x9E3779B9U);
    h = hash(h ^ static_cast<std::uint32_t>(x));
    h = hash(h ^ static_cast<std::uint32_t>(y));
    h = hash(h ^ static_cast<std::uint32_t>(z));
    return (static_cast<float>(h >> 8U) * (2.0F / 16777216.0F)) - 1.0F;
}

// Quintic smoothing and its slope, so the noise's slope is smooth too.
float fade(float t) {
    return t * t * t * ((t * ((t * 6.0F) - 15.0F)) + 10.0F);
}
float fade_slope(float t) {
    return 30.0F * t * t * ((t * (t - 2.0F)) + 1.0F);
}

// Smooth value noise at p and its slope along each axis.
struct NoiseSample {
    float value = 0.0F;
    Vec3f slope;
};

NoiseSample value_noise(Vec3f p, std::uint32_t seed, bool flat) {
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = flat ? 0.0F : std::floor(p.z);
    const auto x0 = static_cast<std::int32_t>(fx);
    const auto y0 = static_cast<std::int32_t>(fy);
    const auto z0 = static_cast<std::int32_t>(fz);
    const float tx = p.x - fx;
    const float ty = p.y - fy;
    const float tz = flat ? 0.0F : p.z - fz;
    const float u = fade(tx);
    const float v = fade(ty);
    const float w = fade(tz);
    const float du = fade_slope(tx);
    const float dv = fade_slope(ty);
    const float dw = flat ? 0.0F : fade_slope(tz);
    const float a = lattice(x0, y0, z0, seed);
    const float b = lattice(x0 + 1, y0, z0, seed);
    const float c = lattice(x0, y0 + 1, z0, seed);
    const float d = lattice(x0 + 1, y0 + 1, z0, seed);
    float e = 0.0F;
    float f = 0.0F;
    float g = 0.0F;
    float h = 0.0F;
    if (!flat) {
        e = lattice(x0, y0, z0 + 1, seed);
        f = lattice(x0 + 1, y0, z0 + 1, seed);
        g = lattice(x0, y0 + 1, z0 + 1, seed);
        h = lattice(x0 + 1, y0 + 1, z0 + 1, seed);
    }
    // The trilinear blend written out in its terms, so its slope comes from the same terms.
    const float k1 = b - a;
    const float k2 = c - a;
    const float k3 = e - a;
    const float k4 = a - b - c + d;
    const float k5 = a - c - e + g;
    const float k6 = a - b - e + f;
    const float k7 = -a + b + c - d + e - f - g + h;
    NoiseSample out;
    out.value = a + (k1 * u) + (k2 * v) + (k3 * w) + (k4 * u * v) + (k5 * v * w) + (k6 * w * u) +
                (k7 * u * v * w);
    out.slope = {du * (k1 + (k4 * v) + (k6 * w) + (k7 * v * w)),
                 dv * (k2 + (k5 * w) + (k4 * u) + (k7 * w * u)),
                 dw * (k3 + (k6 * u) + (k5 * v) + (k7 * u * v))};
    return out;
}

// The names an attribute's Expression reads besides the attributes before it.
constexpr std::array<std::string_view, 16> kNames{
    "age", "life", "lifetime",  "speed", "x",        "y",      "z",    "vx",
    "vy",  "vz",   "neighbors", "index", "collided", "random", "time", "delta"};

// Works out a particle's attributes that run when every_tick says: at the start, or each tick.
void work_out(const ParticleStages& stages, SimParticle& p, const SimInputs& inputs,
              bool every_tick, std::string& failed) {
    const auto& list = stages.settings.attributes;
    for (std::size_t i = 0; i < stages.attributes.size(); ++i) {
        if (list[i].every_tick != every_tick) {
            continue;
        }
        const auto lookup = [&](std::string_view name) -> std::optional<json::Value> {
            const float speed = length(p.velocity);
            if (name == "age") {
                return json::Value(static_cast<double>(p.age));
            }
            if (name == "life") {
                return json::Value(static_cast<double>(
                    p.lifetime > 0.0F ? std::clamp(p.age / p.lifetime, 0.0F, 1.0F) : 1.0F));
            }
            if (name == "lifetime") {
                return json::Value(static_cast<double>(p.lifetime));
            }
            if (name == "speed") {
                return json::Value(static_cast<double>(speed));
            }
            if (name == "x") {
                return json::Value(static_cast<double>(p.position.x));
            }
            if (name == "y") {
                return json::Value(static_cast<double>(p.position.y));
            }
            if (name == "z") {
                return json::Value(static_cast<double>(p.position.z));
            }
            if (name == "vx") {
                return json::Value(static_cast<double>(p.velocity.x));
            }
            if (name == "vy") {
                return json::Value(static_cast<double>(p.velocity.y));
            }
            if (name == "vz") {
                return json::Value(static_cast<double>(p.velocity.z));
            }
            if (name == "neighbors") {
                return json::Value(static_cast<std::int64_t>(p.neighbors));
            }
            if (name == "index") {
                return json::Value(static_cast<std::int64_t>(p.index));
            }
            if (name == "collided") {
                return json::Value(p.collided);
            }
            if (name == "random") {
                return json::Value(static_cast<double>(p.random));
            }
            if (name == "time") {
                return json::Value(inputs.time);
            }
            if (name == "delta") {
                return json::Value(static_cast<double>(inputs.delta));
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (list[j].name == name) {
                    return json::Value(static_cast<double>(p.attributes.at(j)));
                }
            }
            return std::nullopt;
        };
        auto value = stages.attributes[i].execute(lookup);
        if (!value) {
            if (failed.empty()) {
                failed = "attribute " + list[i].name + ": " + value.error().message;
            }
            continue;
        }
        const json::Value& v = value.value();
        float number = 0.0F;
        if (v.is_bool()) {
            number = v.as_bool() ? 1.0F : 0.0F;
        } else if (v.is_number()) {
            number = static_cast<float>(v.as_double());
        } else if (failed.empty()) {
            failed = "attribute " + list[i].name + " isn't a number";
        }
        p.attributes.at(i) = std::isfinite(number) ? number : 0.0F;
    }
}

// Adds the events rule r fires for a particle, drawing its chance from the particle and the
// tick; whether it fires.
bool fire(const ParticleStages& stages, std::size_t r, const SimParticle& p,
          const SimInputs& inputs, std::vector<SimEvent>& events) {
    const ParticleEventRule& rule = stages.settings.events[r];
    if (rule.chance < 1.0F &&
        sim_random(inputs.seed ^ static_cast<std::uint32_t>(r), p.index,
                   static_cast<std::uint32_t>(inputs.time * 1000.0)) >= rule.chance) {
        return false;
    }
    events.push_back(
        {.rule = r, .position = p.position, .velocity = p.velocity, .particle = p.index});
    return true;
}

// A simulation's error at the JSON Pointer under its component.
Error problem(const std::string& at, const std::string& message) {
    return {.code = ErrorCode::InvalidArgument, .message = at + ": " + message};
}

} // namespace

std::uint64_t morton_code(std::int32_t x, std::int32_t y, std::int32_t z) {
    // Offset so cells either side of 0 keep their order within 21 bits.
    constexpr std::uint32_t kOffset = 1U << 20U;
    return spread(static_cast<std::uint32_t>(x) + kOffset) |
           (spread(static_cast<std::uint32_t>(y) + kOffset) << 1U) |
           (spread(static_cast<std::uint32_t>(z) + kOffset) << 2U);
}

void NeighborIndex::build(std::span<const Vec3f> points, float radius, bool flat) {
    flat_ = flat;
    inverse_ = radius > 0.0F ? 1.0F / radius : 1.0F;
    sorted_.clear();
    sorted_.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::array<std::int32_t, 3> c = cell(points[i]);
        sorted_.emplace_back(morton_code(c[0], c[1], c[2]), static_cast<std::uint32_t>(i));
    }
    // Ties keep the order the points came in, so neighbors are visited the same way everywhere.
    std::ranges::sort(sorted_);
}

std::vector<std::uint32_t> NeighborIndex::order() const {
    std::vector<std::uint32_t> out;
    out.reserve(sorted_.size());
    for (const auto& [code, index] : sorted_) {
        out.push_back(index);
    }
    return out;
}

std::array<std::int32_t, 3> NeighborIndex::cell(Vec3f p) const {
    constexpr float kLimit = 1048575.0F;
    const auto axis = [&](float v) {
        const float c = std::floor(v * inverse_);
        return static_cast<std::int32_t>(std::isfinite(c) ? std::clamp(c, -kLimit, kLimit) : 0.0F);
    };
    return {axis(p.x), axis(p.y), flat_ ? 0 : axis(p.z)};
}

std::pair<std::size_t, std::size_t> NeighborIndex::range(std::uint64_t code) const {
    const auto first = std::ranges::lower_bound(
        sorted_, code, {},
        [](const std::pair<std::uint64_t, std::uint32_t>& e) { return e.first; });
    const auto last = std::ranges::upper_bound(
        first, sorted_.end(), code, {},
        [](const std::pair<std::uint64_t, std::uint32_t>& e) { return e.first; });
    return {static_cast<std::size_t>(first - sorted_.begin()),
            static_cast<std::size_t>(last - sorted_.begin())};
}

Vec3f curl_noise(Vec3f p, float time, std::uint32_t seed, bool flat) {
    // The noise moves along a slanted axis, so no axis repeats.
    const Vec3f at = p + (Vec3f{0.37F, 0.71F, 0.53F} * time);
    if (flat) {
        const NoiseSample n = value_noise({at.x, at.y, 0.0F}, seed, true);
        return {n.slope.y, -n.slope.x, 0.0F};
    }
    const NoiseSample a = value_noise(at, seed, false);
    const NoiseSample b = value_noise(at + Vec3f{31.416F, 0.0F, 0.0F}, seed + 1U, false);
    const NoiseSample c = value_noise(at + Vec3f{0.0F, 47.853F, 0.0F}, seed + 2U, false);
    // The curl of (a, b, c).
    return {c.slope.y - b.slope.z, a.slope.z - c.slope.x, b.slope.x - a.slope.y};
}

float sim_random(std::uint32_t seed, std::uint32_t a, std::uint32_t b) {
    const std::uint32_t h = hash(hash(hash(seed) ^ a) ^ (b * 0x85EBCA6BU));
    return static_cast<float>(h >> 8U) / 16777216.0F;
}

Vec3f sim_direction(float u, float v, bool flat) {
    const Vec2f turn = cos_sin(u * 6.28318530718F);
    if (flat) {
        return {turn.x, turn.y, 0.0F};
    }
    const float z = (v * 2.0F) - 1.0F;
    const float across = std::sqrt(std::max(0.0F, 1.0F - (z * z)));
    return {turn.x * across, turn.y * across, z};
}

Result<ParticleStages> compile_stages(const ParticleSimulation& settings) {
    ParticleStages out;
    out.settings = settings;
    if (settings.attributes.size() > kMaxParticleAttributes) {
        return problem("/attributes", "a particle carries at most " +
                                          std::to_string(kMaxParticleAttributes) + " attributes");
    }
    for (std::size_t i = 0; i < settings.attributes.size(); ++i) {
        const ParticleAttribute& attribute = settings.attributes[i];
        const std::string at = "/attributes/" + std::to_string(i);
        if (attribute.name.empty()) {
            return problem(at + "/name", "an attribute needs a name");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (settings.attributes[j].name == attribute.name) {
                return problem(at + "/name", "\"" + attribute.name + "\" is named twice");
            }
        }
        if (std::ranges::find(kNames, attribute.name) != kNames.end()) {
            return problem(at + "/name", "\"" + attribute.name + "\" is a particle's own value");
        }
        auto parsed = Expression::parse(attribute.value);
        if (!parsed) {
            return problem(at + "/value", parsed.error().message);
        }
        for (const std::string& name : parsed.value().names()) {
            const bool own = std::ranges::find(kNames, name) != kNames.end();
            const bool before =
                std::any_of(settings.attributes.begin(),
                            settings.attributes.begin() + static_cast<std::ptrdiff_t>(i),
                            [&](const ParticleAttribute& a) { return a.name == name; });
            if (!own && !before) {
                return problem(at + "/value", "it reads \"" + name +
                                                  "\", which is neither a particle's value nor "
                                                  "an attribute before it");
            }
        }
        out.attributes.push_back(std::move(parsed).value());
        if (attribute.name == "size") {
            out.size_attribute = static_cast<int>(i);
        } else if (attribute.name == "alpha") {
            out.alpha_attribute = static_cast<int>(i);
        } else if (attribute.name == "kill") {
            out.kill_attribute = static_cast<int>(i);
        }
    }
    static constexpr std::array<std::string_view, 3> kOn{"collision", "death", "location"};
    for (std::size_t i = 0; i < settings.events.size(); ++i) {
        if (std::ranges::find(kOn, settings.events[i].on) == kOn.end()) {
            return problem("/events/" + std::to_string(i) + "/on",
                           "\"" + settings.events[i].on + "\" isn't collision, death or location");
        }
    }
    return out;
}

void add_death_events(const ParticleStages& stages, const SimParticle& particle,
                      const SimInputs& inputs, std::vector<SimEvent>& events) {
    for (std::size_t r = 0; r < stages.settings.events.size(); ++r) {
        if (stages.settings.events[r].on == "death") {
            fire(stages, r, particle, inputs, events);
        }
    }
}

float attribute_or(const ParticleStages& /*stages*/, const SimParticle& particle, int attribute,
                   float fallback) {
    return attribute >= 0 ? particle.attributes.at(static_cast<std::size_t>(attribute)) : fallback;
}

void run_stages(const ParticleStages& stages, std::span<SimParticle> particles,
                const SimInputs& inputs, std::vector<SimEvent>& events, std::string& failed) {
    const ParticleSimulation& s = stages.settings;
    const float dt = inputs.delta;
    const bool flat = inputs.flat;
    // New particles take their attributes.
    for (SimParticle& p : particles) {
        p.collided = false;
        if (p.alive && p.born) {
            work_out(stages, p, inputs, false, failed);
        }
    }
    std::vector<Vec3f> change(particles.size());
    // Neighbors: separation, cohesion and alignment from those within the radius, the nearest
    // max_neighbors of them.
    if (s.neighbor_radius > 0.0F && dt > 0.0F) {
        std::vector<Vec3f> points;
        std::vector<std::uint32_t> live;
        for (std::size_t i = 0; i < particles.size(); ++i) {
            if (particles[i].alive) {
                points.push_back(flattened(particles[i].position, flat));
                live.push_back(static_cast<std::uint32_t>(i));
            }
        }
        NeighborIndex index;
        index.build(points, s.neighbor_radius, flat);
        const float radius = s.neighbor_radius;
        const auto limit = static_cast<std::size_t>(std::clamp(s.max_neighbors, 1, 256));
        std::vector<std::pair<float, std::uint32_t>> near;
        for (std::size_t n = 0; n < live.size(); ++n) {
            SimParticle& p = particles[live[n]];
            near.clear();
            index.around(points[n], [&](std::uint32_t other) {
                if (other == n) {
                    return;
                }
                const Vec3f apart = points[n] - points[other];
                const float d = length(apart);
                if (d < radius) {
                    near.emplace_back(d, other);
                }
            });
            // The nearest first; ties by their order along the curve's input.
            std::ranges::sort(near);
            if (near.size() > limit) {
                near.resize(limit);
            }
            p.neighbors = static_cast<int>(near.size());
            if (near.empty()) {
                continue;
            }
            Vec3f push;
            Vec3f center;
            Vec3f heading;
            for (const auto& [d, other] : near) {
                const Vec3f apart = points[n] - points[other];
                if (d > 1e-6F) {
                    push = push + (apart * ((1.0F - (d / radius)) / d));
                }
                center = center + points[other];
                heading = heading + particles[live[other]].velocity;
            }
            const auto count = static_cast<float>(near.size());
            center = center * (1.0F / count);
            heading = heading * (1.0F / count);
            Vec3f dv = push * (s.separation * radius);
            dv = dv + ((center - points[n]) * s.cohesion);
            dv = dv + ((heading - p.velocity) * std::min(s.alignment * dt, 1.0F) * (1.0F / dt));
            change[live[n]] = flattened(dv * dt, flat);
        }
    }
    for (std::size_t i = 0; i < particles.size(); ++i) {
        SimParticle& p = particles[i];
        if (!p.alive) {
            continue;
        }
        const Vec3f before = p.velocity;
        Vec3f v = p.velocity + change[i];
        if (s.curl_strength != 0.0F && dt > 0.0F) {
            const float scale = std::max(s.curl_scale, 0.0001F);
            const Vec3f swirl =
                curl_noise(p.position * (1.0F / scale),
                           static_cast<float>(inputs.time) * s.curl_speed, inputs.seed, flat);
            v = v + (swirl * (s.curl_strength * dt));
        }
        if (inputs.force) {
            v = v + (flattened(inputs.force(p.position), flat) * dt);
        }
        if (s.drag > 0.0F) {
            v = v * std::max(0.0F, 1.0F - (s.drag * dt));
        }
        const float speed = length(v);
        if (s.max_speed > 0.0F && speed > s.max_speed) {
            v = v * (s.max_speed / speed);
        } else if (s.min_speed > 0.0F && speed > 1e-6F && speed < s.min_speed) {
            v = v * (s.min_speed / speed);
        }
        v = flattened(v, flat);
        // The change moves it this tick too, as its emitter already moved it by the old one.
        p.position = p.position + ((v - before) * dt);
        p.velocity = v;
        if (inputs.contact && s.collide != "none") {
            const SimContact contact = inputs.contact(p.position, s.collision_radius);
            if (contact.collided) {
                p.collided = true;
                const Vec3f normal = flattened(contact.normal, flat);
                p.position = p.position + (normal * contact.depth);
                if (s.response == "stick") {
                    p.velocity = {};
                } else if (s.response == "kill") {
                    p.alive = false;
                } else {
                    const float into = dot(p.velocity, normal);
                    if (into < 0.0F) {
                        const Vec3f across = p.velocity - (normal * into);
                        p.velocity = (across * (1.0F - std::clamp(s.friction, 0.0F, 1.0F))) -
                                     (normal * (into * std::clamp(s.bounce, 0.0F, 1.0F)));
                    }
                }
            }
        }
        work_out(stages, p, inputs, true, failed);
        if (stages.kill_attribute >= 0 &&
            p.attributes.at(static_cast<std::size_t>(stages.kill_attribute)) > 0.5F) {
            p.alive = false;
        }
    }
    // Events, particle by particle; a rule's chance is drawn from the particle and the tick.
    for (SimParticle& p : particles) {
        for (std::size_t r = 0; r < s.events.size(); ++r) {
            const ParticleEventRule& rule = s.events[r];
            bool fires = false;
            if (rule.on == "collision") {
                fires = p.collided;
            } else if (rule.on == "location") {
                const Vec3f at = inputs.to_world ? inputs.to_world(p.position) : p.position;
                const Vec3f low = to_float(rule.region_position);
                const Vec3f high = low + to_float(rule.region_size);
                fires = p.alive && at.x >= low.x && at.x <= high.x && at.y >= low.y &&
                        at.y <= high.y && (flat || (at.z >= low.z && at.z <= high.z));
            }
            if (fires && fire(stages, r, p, inputs, events) && rule.kill) {
                p.alive = false;
            }
        }
    }
}

} // namespace slash::world
