#include "slash/world/fluid.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/log.hpp"
#include "slash/world/particle_sim.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/wind.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine fluid",
  "description": "A liquid or gas simulated as particles with no grid, after Niagara's fluids.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "properties": {
    "format": {"const": 1},
    "extends": {"type": "string", "pattern": "\\.fluid\\.json$", "description": "A .fluid.json file this one changes, as a JSON Merge Patch."},
    "name": {"type": "string", "description": "What it is called."},
    "kind": {"enum": ["liquid", "gas"], "description": "liquid (water, oil, honey) or gas (smoke, steam, fire). Default liquid."},
    "space": {"enum": ["3d", "2d"], "description": "3d (meters) or 2d (pixels, seen from the side). Default 3d."},
    "particle_radius": {"type": "number", "exclusiveMinimum": 0, "description": "Each particle's radius; particles sit twice this apart at rest. Default 0.05 m, or 4 px in 2D."},
    "rest_density": {"type": "number", "exclusiveMinimum": 0, "description": "Mass a cubic meter (a square meter in 2D, at 100 px a meter) at rest. Default 1000 for a liquid, 1.2 for a gas."},
    "iterations": {"type": "integer", "minimum": 1, "maximum": 16, "description": "Density solver passes a step. Default 3."},
    "substeps": {"type": "integer", "minimum": 1, "maximum": 8, "description": "Steps a tick. Default 1 (2 for a liquid in 2D)."},
    "relaxation": {"type": "number", "exclusiveMinimum": 0, "description": "How soft the density constraint is; more is softer and steadier. Default 0.2."},
    "viscosity": {"type": "number", "minimum": 0, "maximum": 1, "description": "The share of neighbors' relative velocity taken each step (XSPH): 0.01 water, 0.3 honey. Default 0.02."},
    "surface_tension": {"type": "number", "minimum": 0, "description": "How hard particles pull together, beading drops. Default 0.2."},
    "vorticity": {"type": "number", "minimum": 0, "description": "Vorticity confinement, which keeps swirls the solver damps. Default 0.05."},
    "gravity_scale": {"type": "number", "description": "How much gravity pulls it. Default 1 for a liquid, 0 for a gas."},
    "drag": {"type": "number", "minimum": 0, "description": "The share of velocity lost a second (and, in a gas, how hard the wind carries it). Default 0, or 2 for a gas."},
    "lifetime": {"type": "number", "minimum": 0, "description": "Seconds a particle lives; 0 for ever. Default 0, or 4 for a gas."},
    "max_particles": {"type": "integer", "minimum": 1, "maximum": 65536, "description": "The most particles it holds; emitters stop at it. Default 4096."},
    "bounce": {"type": "number", "minimum": 0, "maximum": 1, "description": "The share of speed into a surface sent back. Default 0."},
    "friction": {"type": "number", "minimum": 0, "maximum": 1, "description": "The share of speed along a surface lost on touching it. Default 0.1."},
    "gas": {
      "type": "object", "additionalProperties": false,
      "description": "How a gas rises, cools, burns and swirls.",
      "properties": {
        "buoyancy": {"type": "number", "description": "Lift as a share of gravity, times how much lighter the gas is than the air for its heat (its rise over its absolute temperature). Default 1.5."},
        "ambient_temperature": {"type": "number", "description": "Degrees of the air round it. Default 20."},
        "cooling": {"type": "number", "minimum": 0, "description": "The share of its heat above ambient lost a second. Default 0.6."},
        "diffusion": {"type": "number", "minimum": 0, "description": "How fast heat and smoke spread to neighbors, a second. Default 0.3."},
        "dissipation": {"type": "number", "minimum": 0, "description": "The share of smoke that thins away a second. Default 0.15."},
        "ignition": {"type": "number", "description": "Degrees above which fuel burns. Default 250."},
        "burn_rate": {"type": "number", "minimum": 0, "description": "Fuel burned a second. Default 1.5."},
        "heat": {"type": "number", "minimum": 0, "description": "Degrees a unit of fuel burned adds. Default 600."},
        "smoke_yield": {"type": "number", "minimum": 0, "description": "Smoke a unit of fuel burned leaves. Default 1."},
        "vortex_share": {"type": "number", "minimum": 0, "maximum": 1, "description": "The share of particles that are vortex particles. Default 0.1."},
        "vortex_strength": {"type": "number", "minimum": 0, "description": "How hard a vortex particle swirls its neighbors. Default 0.5."},
        "spread": {"type": "number", "minimum": 0, "maximum": 1, "description": "How hard crowded particles push apart. Default 0.05."}
      }
    },
    "look": {
      "type": "object", "additionalProperties": false,
      "description": "How it draws.",
      "properties": {
        "color": {"$ref": "#/$defs/color", "description": "A liquid's color. Default [0.2, 0.45, 0.85, 0.85]."},
        "foam": {"$ref": "#/$defs/color", "description": "Foam and spray's color. Default white."},
        "smoke": {"$ref": "#/$defs/color", "description": "Smoke's color at full density. Default [0.35, 0.35, 0.38, 0.6]."},
        "fire": {"$ref": "#/$defs/color", "description": "Fire's color, as hot as burning. Default [1, 0.55, 0.15, 1]."},
        "surface": {"type": "boolean", "description": "A liquid draws as one smoothed surface, with spray as discs; false draws every particle as a disc. Default true."},
        "depth": {"type": "number", "exclusiveMinimum": 0, "description": "How deep a liquid is where it shows its color's full alpha, in meters (pixels in 2D); shallower water shows through. Default 8 particle radii."},
        "shadow": {"type": "number", "minimum": 0, "description": "How dark a gas's smoke shades the smoke behind it from the light. Default 1; 0 lights every puff alike."}
      }
    },
    "budget": {
      "type": "object", "additionalProperties": false,
      "description": "What it may cost; fluid.check reports what goes past.",
      "properties": {
        "particles": {"type": "integer", "minimum": 0, "description": "The most particles."},
        "work": {"type": "integer", "minimum": 0, "description": "The most neighbor visits a tick (the solver's work, the same on every machine)."}
      }
    }
  },
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 4}
  }
})";

struct FluidCacheEntry {
    std::string key;
    std::shared_ptr<const FluidRecipe> recipe;
};

struct FluidCache {
    std::mutex mutex;
    std::map<std::string, FluidCacheEntry, std::less<>> entries;
};
using FluidCacheHolder = std::shared_ptr<FluidCache>;

// What an emitter carries from tick to tick for a fluid: the particles it owes and where it
// was, to pour with its own velocity.
struct EmitterState {
    double owed = 0.0;
    Vec3f origin;
    bool placed = false;
    bool seen = false; // poured this tick
};

// Each fluid entity's particles, by entity ID, its emitters' states by emitter and fluid ID,
// and what it said once.
struct FluidStore {
    std::map<std::string, FluidParticles, std::less<>> fluids;
    std::map<std::string, EmitterState, std::less<>> emitters;
    std::set<std::string, std::less<>> warned;
};
using FluidStoreHolder = std::shared_ptr<FluidStore>;

struct FluidHost3DHolder {
    FluidHost host;
};
struct FluidHost2DHolder {
    FluidHost host;
};

float number(const json::Value* value, float fallback) {
    return value != nullptr && value->is_number() ? static_cast<float>(value->as_double())
                                                  : fallback;
}

Vec4 color_of(const json::Value* value, Vec4 fallback) {
    if (value == nullptr || !value->is_array()) {
        return fallback;
    }
    const json::Array& items = value->as_array();
    Vec4 out = fallback;
    out.x = static_cast<float>(items[0].as_double());
    out.y = static_cast<float>(items[1].as_double());
    out.z = static_cast<float>(items[2].as_double());
    out.w = items.size() > 3 ? static_cast<float>(items[3].as_double()) : 1.0F;
    return out;
}

float dot(Vec3f a, Vec3f b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3f cross(Vec3f a, Vec3f b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3f v) {
    return std::sqrt(dot(v, v));
}

std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// A value from 0 to 1 from a hash.
float unit(std::uint32_t h) {
    return static_cast<float>(mix(h) >> 8U) * (1.0F / 16777216.0F);
}

std::uint32_t hash_text(std::string_view text) {
    std::uint32_t h = 2166136261U;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return h;
}

// The kernels over a support of h: poly6 for densities, the spiky kernel's gradient for
// pressure, in 3D or 2D.
struct Kernels {
    float h = 0.2F;
    float h2 = 0.04F;
    float poly6 = 0.0F;
    float spiky = 0.0F;

    Kernels(float support, bool flat) : h(support), h2(support * support) {
        constexpr float kPi = std::numbers::pi_v<float>;
        const float h4 = h2 * h2;
        if (flat) {
            poly6 = 4.0F / (kPi * h4 * h4);
            spiky = -30.0F / (kPi * h4 * h);
        } else {
            poly6 = 315.0F / (64.0F * kPi * h4 * h4 * h);
            spiky = -45.0F / (kPi * h4 * h2);
        }
    }

    [[nodiscard]] float w(float r2) const {
        if (r2 >= h2) {
            return 0.0F;
        }
        const float d = h2 - r2;
        return poly6 * d * d * d;
    }

    [[nodiscard]] Vec3f grad(Vec3f r, float distance) const {
        if (distance >= h || distance <= 1e-9F) {
            return {};
        }
        const float d = h - distance;
        return r * (spiky * d * d / distance);
    }
};

constexpr std::size_t kMaxNeighbors = 48;

// Each particle's neighbors within the support, the first kMaxNeighbors in the index's order.
struct Neighbors {
    std::vector<std::uint32_t> list;
    std::vector<std::uint8_t> count;

    void build(const NeighborIndex& index, std::span<const Vec3f> points, float h,
               jobs::Pool* pool) {
        const std::size_t n = points.size();
        list.assign(n * kMaxNeighbors, 0);
        count.assign(n, 0);
        const float h2 = h * h;
        const auto gather = [&](std::size_t i) {
            std::size_t found = 0;
            const Vec3f p = points[i];
            index.around(p, [&](std::uint32_t j) {
                if (found >= kMaxNeighbors || j == i) {
                    return;
                }
                const Vec3f d = p - points[j];
                if (dot(d, d) < h2) {
                    list[(i * kMaxNeighbors) + found] = j;
                    ++found;
                }
            });
            count[i] = static_cast<std::uint8_t>(found);
        };
        run(n, pool, gather);
    }

    [[nodiscard]] std::span<const std::uint32_t> of(std::size_t i) const {
        return {list.data() + (i * kMaxNeighbors), count[i]};
    }

    template <typename Task>
    static void run(std::size_t n, jobs::Pool* pool, const Task& task) {
        if (pool != nullptr && n > 256) {
            pool->parallel_for(n, task);
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                task(i);
            }
        }
    }
};

template <typename Task>
void each(std::size_t n, jobs::Pool* pool, const Task& task) {
    Neighbors::run(n, pool, task);
}

// Keeps a point in the bounds' box, returning the contact when it was moved.
std::optional<FluidContact> keep_in(const FluidStepInputs& inputs, Vec3f& p, float radius,
                                    bool flat) {
    if (!inputs.bounds) {
        return std::nullopt;
    }
    const Affine3D& place = *inputs.bounds;
    const Affine3D to_local = place.inverse();
    const Vec3f local = to_float(to_local.apply(Vec3{p.x, p.y, p.z}));
    const Vec3f half = inputs.bounds_half;
    Vec3f moved = local;
    Vec3f normal;
    float depth = 0.0F;
    const auto clamp_axis = [&](float& value, float extent, Vec3f axis) {
        const float limit = std::max(extent - radius, 0.0F);
        if (value > limit) {
            depth = std::max(depth, value - limit);
            value = limit;
            normal = normal - axis;
        } else if (value < -limit) {
            depth = std::max(depth, -limit - value);
            value = -limit;
            normal = normal + axis;
        }
    };
    clamp_axis(moved.x, half.x, {1.0F, 0.0F, 0.0F});
    clamp_axis(moved.y, half.y, {0.0F, 1.0F, 0.0F});
    if (!flat) {
        clamp_axis(moved.z, half.z, {0.0F, 0.0F, 1.0F});
    }
    if (depth <= 0.0F) {
        return std::nullopt;
    }
    const Vec3f world_point = to_float(place.apply(Vec3{moved.x, moved.y, moved.z}));
    const Vec3f world_normal =
        to_float(place.apply(Vec3{normal.x, normal.y, normal.z})) - to_float(place.apply(Vec3{}));
    p = world_point;
    if (flat) {
        p.z = 0.0F;
    }
    const float n = length(world_normal);
    return FluidContact{.normal = n > 1e-9F ? world_normal * (1.0F / n) : Vec3f{},
                        .depth = depth,
                        .velocity = {},
                        .body = entt::null};
}

void substep(const FluidRecipe& recipe, FluidParticles& particles, const FluidStepInputs& inputs,
             float dt) {
    const std::size_t n = particles.size();
    if (n == 0) {
        return;
    }
    const bool flat = recipe.flat;
    const float spacing = 2.0F * recipe.radius;
    const float h = 2.0F * spacing;
    const Kernels kernels(h, flat);
    const float volume = fluid_particle_volume(recipe);
    const float g = length(inputs.gravity);
    const float accel_scale = std::max(g, 1.0F);
    const Vec3f up = g > 1e-6F ? inputs.gravity * (-1.0F / g) : Vec3f{0.0F, 1.0F, 0.0F};
    jobs::Pool* pool = inputs.pool;

    // Forces, then where each particle would go.
    std::vector<Vec3f> predicted(n);
    each(n, pool, [&](std::size_t i) {
        Vec3f v = particles.velocity[i];
        Vec3f a = inputs.gravity * recipe.gravity_scale;
        if (recipe.gas) {
            // Boussinesq: hot air is lighter by its rise over its absolute temperature.
            const float above = particles.temperature[i] - recipe.ambient_temperature;
            const float kelvin = std::max(particles.temperature[i] + 273.15F, 1.0F);
            a = a + (up * (recipe.buoyancy * (above / kelvin) * accel_scale));
        }
        if (inputs.acceleration) {
            a = a + inputs.acceleration(particles.position[i]);
        }
        v = v + (a * dt);
        v = v * std::max(0.0F, 1.0F - (recipe.drag * dt));
        if (flat) {
            v.z = 0.0F;
        }
        particles.velocity[i] = v;
        predicted[i] = particles.position[i] + (v * dt);
    });

    NeighborIndex index;
    index.build(predicted, h, flat);
    Neighbors neighbors;
    neighbors.build(index, predicted, h, pool);
    std::size_t visits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        visits += neighbors.count[i];
    }

    // The density constraint (gases only push apart when crowded, by spread).
    const float softness = recipe.relaxation / (h * h);
    const float strength = recipe.gas ? recipe.spread : 1.0F;
    std::vector<float> lambda(n, 0.0F);
    std::vector<Vec3f> delta(n);
    // Particles the bounds held in while the constraint moved them.
    std::vector<std::uint8_t> walled(n, 0);
    for (int iteration = 0; iteration < recipe.iterations && strength > 0.0F; ++iteration) {
        each(n, pool, [&](std::size_t i) {
            const Vec3f p = predicted[i];
            float density = volume * kernels.w(0.0F);
            Vec3f own;
            float sum = 0.0F;
            for (const std::uint32_t j : neighbors.of(i)) {
                const Vec3f r = p - predicted[j];
                const float distance = length(r);
                density += volume * kernels.w(distance * distance);
                const Vec3f grad = kernels.grad(r, distance) * volume;
                own = own + grad;
                sum += dot(grad, grad);
            }
            sum += dot(own, own);
            particles.density[i] = density;
            const float c = std::max(density - 1.0F, 0.0F);
            lambda[i] = -c / (sum + softness);
        });
        each(n, pool, [&](std::size_t i) {
            const Vec3f p = predicted[i];
            Vec3f d;
            for (const std::uint32_t j : neighbors.of(i)) {
                const Vec3f r = p - predicted[j];
                d = d + (kernels.grad(r, length(r)) * (volume * (lambda[i] + lambda[j])));
            }
            delta[i] = d * strength;
        });
        each(n, pool, [&](std::size_t i) {
            predicted[i] = predicted[i] + delta[i];
            if (flat) {
                predicted[i].z = 0.0F;
            }
            if (keep_in(inputs, predicted[i], recipe.radius, flat)) {
                walled[i] = 1;
            }
        });
    }

    // What the particles touch: the bounds, then the host's colliders.
    std::vector<std::optional<FluidContact>> contacts(n);
    const FluidHost* host = inputs.host;
    if (host != nullptr && host->collide) {
        host->collide(predicted, recipe.radius, contacts);
        contacts.resize(n);
    }
    const float mass = fluid_particle_mass(recipe);
    std::size_t touching = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (const std::optional<FluidContact>& hit = contacts[i]; hit && hit->depth > 0.0F) {
            const FluidContact& contact = *hit;
            predicted[i] = predicted[i] + (contact.normal * contact.depth);
            if (flat) {
                predicted[i].z = 0.0F;
            }
            if (contact.body != entt::null && host != nullptr && host->push) {
                // What pushing the particle out took, given back to the body.
                host->push(contact.body, contact.normal * (-mass * contact.depth / dt),
                           predicted[i], mass);
            }
        }
        if (auto bound = keep_in(inputs, predicted[i], recipe.radius, flat); bound) {
            contacts[i] = bound;
        }
        if (contacts[i] || walled[i] != 0) {
            ++touching;
        }
    }

    // Velocities from where they went, then off what they touch.
    each(n, pool, [&](std::size_t i) {
        Vec3f v = (predicted[i] - particles.position[i]) * (1.0F / dt);
        if (contacts[i]) {
            const FluidContact& contact = *contacts[i];
            const Vec3f relative = v - contact.velocity;
            const float into = dot(relative, contact.normal);
            if (into < 0.0F) {
                const Vec3f normal_part = contact.normal * into;
                const Vec3f along = relative - normal_part;
                v = contact.velocity + (along * (1.0F - recipe.friction)) -
                    (normal_part * recipe.bounce);
            }
        }
        particles.velocity[i] = v;
    });

    // Viscosity, surface tension and vorticity from the neighbors' new velocities; gases swirl
    // round vortex particles and share heat and smoke.
    std::vector<Vec3f> curl(n);
    if (!recipe.gas && recipe.vorticity > 0.0F) {
        each(n, pool, [&](std::size_t i) {
            Vec3f w;
            for (const std::uint32_t j : neighbors.of(i)) {
                const Vec3f r = predicted[i] - predicted[j];
                const Vec3f vij = particles.velocity[j] - particles.velocity[i];
                w = w + cross(vij, kernels.grad(r, length(r)) * volume);
            }
            curl[i] = w;
        });
    }
    std::vector<Vec3f> velocity(n);
    std::vector<float> temperature;
    std::vector<float> smoke;
    if (recipe.gas) {
        temperature.resize(n);
        smoke.resize(n);
    }
    const float surface = recipe.surface_tension * accel_scale / h;
    const float vortex = recipe.vortex_strength / h;
    each(n, pool, [&](std::size_t i) {
        const Vec3f p = predicted[i];
        const Vec3f v = particles.velocity[i];
        Vec3f viscous;
        Vec3f pull;
        Vec3f eta;
        Vec3f swirl;
        float heat = 0.0F;
        float soot = 0.0F;
        for (const std::uint32_t j : neighbors.of(i)) {
            const Vec3f r = p - predicted[j];
            const float distance = length(r);
            const float w = volume * kernels.w(distance * distance);
            viscous = viscous + ((particles.velocity[j] - v) * w);
            pull = pull - (r * w);
            if (!recipe.gas && recipe.vorticity > 0.0F) {
                eta = eta + (kernels.grad(r, distance) * (volume * length(curl[j])));
            }
            if (recipe.gas) {
                heat += (particles.temperature[j] - particles.temperature[i]) * w;
                soot += (particles.smoke[j] - particles.smoke[i]) * w;
                // A vortex particle j swirls i round its axis.
                if (unit(particles.id[j] * 0x9E3779B9U) < recipe.vortex_share) {
                    Vec3f axis = cross(particles.velocity[j], up);
                    if (flat) {
                        axis = {0.0F, 0.0F, (particles.id[j] & 1U) != 0U ? 1.0F : -1.0F};
                    }
                    const float a = length(axis);
                    if (a > 1e-6F) {
                        swirl = swirl + (cross(axis * (1.0F / a), r) * (vortex * w));
                    }
                }
            }
        }
        Vec3f out = v + (viscous * recipe.viscosity);
        if (!recipe.gas) {
            out = out + (pull * (surface * dt));
            if (recipe.vorticity > 0.0F) {
                const float e = length(eta);
                if (e > 1e-9F) {
                    out = out + (cross(eta * (1.0F / e), curl[i]) * (recipe.vorticity * dt));
                }
            }
        } else {
            out = out + (swirl * (accel_scale * dt));
            temperature[i] = particles.temperature[i] + (heat * recipe.diffusion * dt * 4.0F);
            smoke[i] = particles.smoke[i] + (soot * recipe.diffusion * dt * 4.0F);
        }
        if (flat) {
            out.z = 0.0F;
        }
        velocity[i] = out;
    });

    particles.velocity = std::move(velocity);
    particles.position = std::move(predicted);
    if (recipe.gas) {
        particles.temperature = std::move(temperature);
        particles.smoke = std::move(smoke);
        each(n, pool, [&](std::size_t i) {
            float& t = particles.temperature[i];
            float& fuel = particles.fuel[i];
            float& s = particles.smoke[i];
            if (fuel > 0.0F && t >= recipe.ignition) {
                const float burned = std::min(fuel, recipe.burn_rate * dt);
                fuel -= burned;
                t += recipe.heat * burned;
                s += recipe.smoke_yield * burned;
            }
            t += (recipe.ambient_temperature - t) * std::min(recipe.cooling * dt, 1.0F);
            s = std::max(0.0F, s - (s * std::min(recipe.dissipation * dt, 1.0F)));
        });
    }
    particles.contacts = touching;
    particles.work += visits * static_cast<std::size_t>(1 + recipe.iterations);
}

Vec3f vec3f(Vec3 v) {
    return to_float(v);
}

FluidStore& store_of(World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<FluidStoreHolder>() == nullptr) {
        ctx.emplace<FluidStoreHolder>(std::make_shared<FluidStore>());
    }
    return *ctx.get<FluidStoreHolder>();
}

void warn_once(World& world, FluidStore& store, std::string_view id, const std::string& message) {
    if (store.warned.insert(std::string(id) + "|" + message).second) {
        json::Value data;
        data["error"] = message;
        world.emit("fluid_failed", id, std::move(data));
        log::warn("a fluid can't step", {{"id", std::string(id)}, {"error", message}});
    }
}

// A direction turned from dir by up to spread, from two hashed numbers.
Vec3f spread_direction(Vec3f dir, float spread, std::uint32_t seed, bool flat) {
    const float d = length(dir);
    const Vec3f axis = d > 1e-6F ? dir * (1.0F / d) : Vec3f{0.0F, -1.0F, 0.0F};
    if (spread <= 0.0F) {
        return axis;
    }
    if (flat) {
        const float angle = ((unit(seed) * 2.0F) - 1.0F) * spread;
        const Vec2f cs = cos_sin(angle);
        return {(axis.x * cs.x) - (axis.y * cs.y), (axis.x * cs.y) + (axis.y * cs.x), 0.0F};
    }
    const Vec3f other = std::abs(axis.y) < 0.9F ? Vec3f{0.0F, 1.0F, 0.0F} : Vec3f{1.0F, 0.0F, 0.0F};
    Vec3f side = cross(axis, other);
    side = side * (1.0F / length(side));
    const Vec3f lift = cross(axis, side);
    const float tilt = unit(seed) * spread;
    const Vec2f around = cos_sin(unit(seed ^ 0x51ED270BU) * 6.28318530718F);
    const Vec2f bend = cos_sin(tilt);
    return (axis * bend.x) + (side * (bend.y * around.x)) + (lift * (bend.y * around.y));
}

struct Placed {
    Affine3D place;
    bool ok = false;
};

void run_fluid(World& world, FluidStore& store, entt::entity entity, bool flat, double tick_seconds,
               std::optional<Tree3D>& tree3, std::optional<Tree2D>& tree2) {
    auto& ecs = world.ecs();
    const std::string id(world.id_of(entity));
    if (id.empty()) {
        return;
    }
    std::string recipe_path;
    json::Value overrides;
    bool enabled = true;
    Vec3f fill_size;
    Vec3f fill_velocity;
    Vec3f bounds_size;
    if (flat) {
        const auto& fluid = ecs.get<const Fluid2D>(entity);
        recipe_path = fluid.recipe;
        overrides = fluid.overrides;
        enabled = fluid.enabled;
        fill_size = {to_float(fluid.fill_size.x), to_float(fluid.fill_size.y), 0.0F};
        fill_velocity = {to_float(fluid.fill_velocity.x), to_float(fluid.fill_velocity.y), 0.0F};
        bounds_size = {to_float(fluid.bounds_size.x), to_float(fluid.bounds_size.y), 0.0F};
    } else {
        const auto& fluid = ecs.get<const Fluid3D>(entity);
        recipe_path = fluid.recipe;
        overrides = fluid.overrides;
        enabled = fluid.enabled;
        fill_size = vec3f(fluid.fill_size);
        fill_velocity = vec3f(fluid.fill_velocity);
        bounds_size = vec3f(fluid.bounds_size);
    }
    if (!enabled) {
        return;
    }
    auto loaded = load_fluid(world, recipe_path, overrides);
    if (!loaded) {
        warn_once(world, store, id, loaded.error().message);
        return;
    }
    const FluidRecipe& recipe = *loaded.value();
    if (recipe.flat != flat) {
        warn_once(world, store, id,
                  "\"" + recipe_path + "\" is a " + (recipe.flat ? "2d" : "3d") +
                      " fluid, which a " + (flat ? "Fluid2D" : "Fluid3D") + " can't step");
        return;
    }
    // The entity's place in the world: 2D as a turn about z.
    Affine3D place;
    if (flat) {
        if (!tree2) {
            tree2.emplace(world);
        }
        const Transform2D global = tree2->global(entity);
        const Vec2f cs = cos_sin(global.rotation);
        place.x = Vec3{cs.x, cs.y, 0.0F};
        place.y = Vec3{-cs.y, cs.x, 0.0F};
        place.origin = Vec3{global.position.x, global.position.y, 0.0F};
    } else {
        if (!tree3) {
            tree3.emplace(world);
        }
        place = orthonormalized(tree3->global(entity));
    }
    FluidParticles& particles = store.fluids[id];
    particles.work = 0;
    const std::size_t limit = std::min(recipe.max_particles, kMaxFluidParticles);
    const std::uint32_t seed = hash_text(id);
    // Fill once.
    if (!particles.filled) {
        particles.filled = true;
        const float spacing = 2.0F * recipe.radius;
        const auto count = [&](float extent) {
            return extent > 0.0F ? static_cast<int>(std::floor(extent / spacing)) : 0;
        };
        const int nx = count(fill_size.x);
        const int ny = count(fill_size.y);
        const int nz = flat ? 1 : count(fill_size.z);
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx && particles.size() < limit; ++i) {
                    const std::uint32_t h = seed ^ (particles.next_id * 0x85EBCA6BU);
                    const float jitter = spacing * 0.02F;
                    const Vec3f local{
                        (-fill_size.x * 0.5F) + ((static_cast<float>(i) + 0.5F) * spacing) +
                            ((unit(h) - 0.5F) * jitter),
                        (-fill_size.y * 0.5F) + ((static_cast<float>(j) + 0.5F) * spacing) +
                            ((unit(h ^ 1U) - 0.5F) * jitter),
                        flat ? 0.0F
                             : (-fill_size.z * 0.5F) + ((static_cast<float>(k) + 0.5F) * spacing) +
                                   ((unit(h ^ 2U) - 0.5F) * jitter)};
                    Vec3f at = to_float(place.apply(Vec3{local.x, local.y, local.z}));
                    if (flat) {
                        at.z = 0.0F;
                    }
                    particles.add(at, fill_velocity, recipe.ambient_temperature, 0.0F, 0.0F);
                }
            }
        }
    }
    // Emitters that pour into it: those naming it, and those naming none on it or under it.
    const auto pours_here = [&](entt::entity emitter_entity, const FluidEmitter& emitter) {
        if (!emitter.enabled || emitter.rate <= 0.0F) {
            return false;
        }
        if (!emitter.fluid.empty()) {
            return emitter.fluid == id;
        }
        for (entt::entity at = emitter_entity; at != entt::null;
             at = flat ? tree2->parent(at) : tree3->parent(at)) {
            if (flat ? ecs.all_of<Fluid2D>(at) : ecs.all_of<Fluid3D>(at)) {
                return at == entity;
            }
        }
        return false;
    };
    const auto emit = [&](entt::entity emitter_entity, const FluidEmitter& emitter, Vec3f dir) {
        if (!pours_here(emitter_entity, emitter)) {
            return;
        }
        Affine3D at;
        if (flat) {
            const Transform2D global = tree2->global(emitter_entity);
            const Vec2f cs = cos_sin(global.rotation);
            at.x = Vec3{cs.x, cs.y, 0.0F};
            at.y = Vec3{-cs.y, cs.x, 0.0F};
            at.origin = Vec3{global.position.x, global.position.y, 0.0F};
        } else {
            at = orthonormalized(tree3->global(emitter_entity));
        }
        const Vec3f origin = to_float(at.apply(Vec3{}));
        const Vec3f world_dir = to_float(at.apply(Vec3{dir.x, dir.y, dir.z})) - origin;
        const std::string emitter_id(world.id_of(emitter_entity));
        EmitterState& state = store.emitters[emitter_id + ">" + id];
        state.seen = true;
        // The emitter's own velocity, from where it was the tick before, which its particles
        // carry by inherit_velocity: a moving tap trails its water.
        Vec3f carried;
        if (state.placed && tick_seconds > 0.0) {
            carried = (origin - state.origin) *
                      (emitter.inherit_velocity / static_cast<float>(tick_seconds));
        }
        state.origin = origin;
        state.placed = true;
        const double owed = (static_cast<double>(emitter.rate) * tick_seconds) + state.owed;
        const auto whole = static_cast<std::size_t>(std::floor(owed));
        state.owed = owed - static_cast<double>(whole);
        const std::uint32_t emitter_seed = hash_text(emitter_id);
        for (std::size_t k = 0; k < whole && particles.size() < limit; ++k) {
            const std::uint32_t h = mix(emitter_seed ^ (particles.next_id * 0x27D4EB2FU));
            const Vec3f way = spread_direction(world_dir, emitter.spread, h, flat);
            // A point on the disc across the way it pours.
            Vec3f offset;
            if (emitter.radius > 0.0F) {
                const float r = emitter.radius * std::sqrt(unit(h ^ 0x1234567U));
                if (flat) {
                    const Vec3f across{-way.y, way.x, 0.0F};
                    offset = across * (r * ((unit(h ^ 0x7654321U) * 2.0F) - 1.0F));
                } else {
                    const Vec3f other =
                        std::abs(way.y) < 0.9F ? Vec3f{0.0F, 1.0F, 0.0F} : Vec3f{1.0F, 0.0F, 0.0F};
                    Vec3f side = cross(way, other);
                    side = side * (1.0F / length(side));
                    const Vec3f lift = cross(way, side);
                    const Vec2f round = cos_sin(unit(h ^ 0x7654321U) * 6.28318530718F);
                    offset = (side * (r * round.x)) + (lift * (r * round.y));
                }
            }
            // Spread along the tick so a stream doesn't come out in clumps.
            const float along = unit(h ^ 0xABCDEFU) * static_cast<float>(tick_seconds);
            const Vec3f velocity = (way * emitter.speed) + carried;
            Vec3f p = origin + offset + (velocity * along);
            if (flat) {
                p.z = 0.0F;
            }
            particles.add(p, velocity, emitter.temperature, emitter.smoke, emitter.fuel);
        }
    };
    if (flat) {
        for (const auto [e, emitter] : ecs.view<const FluidEmitter2D>().each()) {
            if (ecs.all_of<Transform2D>(e)) {
                emit(e, emitter,
                     {to_float(emitter.direction.x), to_float(emitter.direction.y), 0.0F});
            }
        }
    } else {
        for (const auto [e, emitter] : ecs.view<const FluidEmitter3D>().each()) {
            if (ecs.all_of<Transform3D>(e)) {
                emit(e, emitter, vec3f(emitter.direction));
            }
        }
    }
    // Physics fields with particles on act on it as on CPU particles: their forces push each
    // particle as a kilogram while it steps, velocity fields set its velocity, anchors hold it
    // and kill fields take it away, so one field acts on bodies, particles and fluids alike.
    std::optional<PhysicsFields3D> fields3;
    std::optional<PhysicsFields2D> fields2;
    if (flat && !ecs.view<const PhysicsField2D>().empty()) {
        fields2.emplace(world, *tree2);
    } else if (!flat && !ecs.view<const PhysicsField3D>().empty()) {
        fields3.emplace(world, *tree3);
    }
    const bool fielded = (fields2 && !fields2->empty()) || (fields3 && !fields3->empty());
    const double field_time = static_cast<double>(world.tick()) * tick_seconds;
    const auto field_at = [&](Vec3f p) {
        if (flat) {
            return fields2->particle_effect(Vec2{p.x, p.y}, seed, field_time);
        }
        return fields3->particle_effect(Vec3{p.x, p.y, p.z}, seed, field_time);
    };
    std::vector<std::pair<std::size_t, Vec3f>> anchored;
    if (fielded) {
        for (std::size_t i = 0; i < particles.size(); ++i) {
            const FieldEffect effect = field_at(particles.position[i]);
            if (effect.velocity) {
                particles.velocity[i] = vec3f(*effect.velocity);
            }
            if (effect.anchored) {
                anchored.emplace_back(i, particles.position[i]);
            }
        }
    }
    // Step.
    FluidStepInputs inputs;
    inputs.seconds = static_cast<float>(tick_seconds);
    inputs.pool = &world.pool();
    if (bounds_size.x > 0.0F && bounds_size.y > 0.0F && (flat || bounds_size.z > 0.0F)) {
        inputs.bounds = place;
        inputs.bounds_half = bounds_size * 0.5F;
    }
    inputs.gravity = flat ? Vec3f{0.0F, 980.0F, 0.0F} : Vec3f{0.0F, -9.8F, 0.0F};
    if (flat) {
        if (const auto* host = ecs.ctx().find<FluidHost2DHolder>(); host != nullptr) {
            inputs.host = &host->host;
        }
    } else if (const auto* host = ecs.ctx().find<FluidHost3DHolder>(); host != nullptr) {
        inputs.host = &host->host;
    }
    if (inputs.host != nullptr && inputs.host->gravity) {
        inputs.gravity = inputs.host->gravity();
    }
    // A gas drifts with the wind; fields push liquids and gases.
    std::optional<WindField> wind3;
    std::optional<WindField2D> wind2;
    const double time = wind_time(world);
    if (recipe.gas && recipe.drag > 0.0F) {
        if (flat && !ecs.view<const WindSource2D>().empty()) {
            wind2.emplace(world, *tree2);
        } else if (!flat && !ecs.view<const WindSource3D>().empty()) {
            wind3.emplace(world, *tree3);
        }
    }
    if (wind2 || wind3 || fielded) {
        inputs.acceleration = [&, time](Vec3f p) {
            Vec3f a;
            if (wind2) {
                const Vec2 w = wind2->at(Vec2{p.x, p.y}, time);
                a = Vec3f{to_float(w.x), to_float(w.y), 0.0F} * recipe.drag;
            } else if (wind3) {
                a = vec3f(wind3->at(Vec3{p.x, p.y, p.z}, time)) * recipe.drag;
            }
            if (fields2) {
                const Vec2 push = fields2->particle_force(Vec2{p.x, p.y}, seed, field_time);
                a = a + Vec3f{to_float(push.x), to_float(push.y), 0.0F};
            } else if (fields3) {
                a = a + vec3f(fields3->particle_force(Vec3{p.x, p.y, p.z}, seed, field_time));
            }
            return a;
        };
    }
    step_fluid(recipe, particles, inputs);
    for (const auto& [i, held] : anchored) {
        particles.position[i] = held;
        particles.velocity[i] = {};
    }
    // Sinks drain it; old particles go.
    std::vector<std::pair<Affine3D, Vec3f>> sinks;
    if (flat) {
        for (const auto [e, sink] : ecs.view<const FluidSink2D>().each()) {
            if (sink.enabled && (sink.fluid.empty() || sink.fluid == id) &&
                ecs.all_of<Transform2D>(e)) {
                const Transform2D global = tree2->global(e);
                Affine3D at;
                at.origin = Vec3{global.position.x, global.position.y, 0.0F};
                sinks.emplace_back(
                    at, Vec3f{to_float(sink.size.x) * 0.5F, to_float(sink.size.y) * 0.5F, 1.0F});
            }
        }
    } else {
        for (const auto [e, sink] : ecs.view<const FluidSink3D>().each()) {
            if (sink.enabled && (sink.fluid.empty() || sink.fluid == id) &&
                ecs.all_of<Transform3D>(e)) {
                sinks.emplace_back(orthonormalized(tree3->global(e)), vec3f(sink.size) * 0.5F);
            }
        }
    }
    std::vector<Affine3D> to_sink;
    to_sink.reserve(sinks.size());
    for (const auto& [at, half] : sinks) {
        to_sink.push_back(at.inverse());
    }
    particles.remove_if([&](std::size_t i) {
        if (recipe.lifetime > 0.0F && particles.age[i] >= recipe.lifetime) {
            return true;
        }
        const Vec3f p = particles.position[i];
        for (std::size_t s = 0; s < sinks.size(); ++s) {
            const Vec3f local = to_float(to_sink[s].apply(Vec3{p.x, p.y, p.z}));
            const Vec3f half = sinks[s].second;
            if (std::abs(local.x) <= half.x && std::abs(local.y) <= half.y &&
                (flat || std::abs(local.z) <= half.z)) {
                return true;
            }
        }
        if (fielded && field_at(p).killed) {
            return true;
        }
        return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z);
    });
}

void run(World& world, double tick_seconds) {
    auto& ecs = world.ecs();
    if (ecs.view<const Fluid3D>().empty() && ecs.view<const Fluid2D>().empty()) {
        return;
    }
    FluidStore& store = store_of(world);
    std::optional<Tree3D> tree3;
    std::optional<Tree2D> tree2;
    std::vector<entt::entity> fluids3;
    for (const auto [entity, fluid] : ecs.view<const Fluid3D>().each()) {
        fluids3.push_back(entity);
    }
    std::vector<entt::entity> fluids2;
    for (const auto [entity, fluid] : ecs.view<const Fluid2D>().each()) {
        fluids2.push_back(entity);
    }
    for (auto& [key, state] : store.emitters) {
        state.seen = false;
    }
    std::set<std::string, std::less<>> live;
    for (const entt::entity entity : fluids3) {
        live.emplace(world.id_of(entity));
        if (!tree3) {
            tree3.emplace(world);
        }
        run_fluid(world, store, entity, false, tick_seconds, tree3, tree2);
    }
    for (const entt::entity entity : fluids2) {
        live.emplace(world.id_of(entity));
        if (!tree2) {
            tree2.emplace(world);
        }
        run_fluid(world, store, entity, true, tick_seconds, tree3, tree2);
    }
    // Fluids that went take their particles with them.
    std::erase_if(store.fluids, [&](const auto& entry) { return !live.contains(entry.first); });
    // An emitter that stopped pouring starts again from where it is.
    std::erase_if(store.emitters, [](const auto& entry) { return !entry.second.seen; });
}

} // namespace

bool is_fluid_path(std::string_view path) {
    return path.ends_with(kFluidSuffix);
}

json::Value fluid_schema() {
    static const json::Value schema = [] {
        auto parsed = json::parse(kSchema);
        SLASH_ASSERT(parsed.ok(), "the fluid schema parses");
        return std::move(parsed).value();
    }();
    return schema;
}

std::vector<json::ValidationError> validate_fluid(const json::Value& doc) {
    return json::validate(doc, fluid_schema());
}

Result<FluidRecipe> parse_fluid(const json::Value& doc) {
    if (const auto errors = validate_fluid(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    FluidRecipe out;
    if (const json::Value* name = doc.find("name"); name != nullptr) {
        out.name = name->as_string();
    }
    const json::Value* kind = doc.find("kind");
    out.gas = kind != nullptr && kind->as_string() == "gas";
    const json::Value* space = doc.find("space");
    out.flat = space != nullptr && space->as_string() == "2d";
    out.radius = number(doc.find("particle_radius"), out.flat ? 4.0F : 0.05F);
    out.rest_density = number(doc.find("rest_density"), out.gas ? 1.2F : 1000.0F);
    out.iterations = static_cast<int>(number(doc.find("iterations"), 3.0F));
    out.substeps =
        static_cast<int>(number(doc.find("substeps"), out.flat && !out.gas ? 2.0F : 1.0F));
    out.relaxation = number(doc.find("relaxation"), 0.2F);
    out.viscosity = number(doc.find("viscosity"), 0.02F);
    out.surface_tension = number(doc.find("surface_tension"), out.gas ? 0.0F : 0.2F);
    out.vorticity = number(doc.find("vorticity"), 0.05F);
    out.gravity_scale = number(doc.find("gravity_scale"), out.gas ? 0.0F : 1.0F);
    out.drag = number(doc.find("drag"), out.gas ? 2.0F : 0.0F);
    out.lifetime = number(doc.find("lifetime"), out.gas ? 4.0F : 0.0F);
    out.max_particles = static_cast<std::size_t>(number(doc.find("max_particles"), 4096.0F));
    out.bounce = number(doc.find("bounce"), 0.0F);
    out.friction = number(doc.find("friction"), 0.1F);
    if (const json::Value* gas = doc.find("gas"); gas != nullptr) {
        out.buoyancy = number(gas->find("buoyancy"), out.buoyancy);
        out.ambient_temperature = number(gas->find("ambient_temperature"), out.ambient_temperature);
        out.cooling = number(gas->find("cooling"), out.cooling);
        out.diffusion = number(gas->find("diffusion"), out.diffusion);
        out.dissipation = number(gas->find("dissipation"), out.dissipation);
        out.ignition = number(gas->find("ignition"), out.ignition);
        out.burn_rate = number(gas->find("burn_rate"), out.burn_rate);
        out.heat = number(gas->find("heat"), out.heat);
        out.smoke_yield = number(gas->find("smoke_yield"), out.smoke_yield);
        out.vortex_share = number(gas->find("vortex_share"), out.vortex_share);
        out.vortex_strength = number(gas->find("vortex_strength"), out.vortex_strength);
        out.spread = number(gas->find("spread"), out.spread);
    }
    if (const json::Value* look = doc.find("look"); look != nullptr) {
        out.color = color_of(look->find("color"), out.color);
        out.foam = color_of(look->find("foam"), out.foam);
        out.smoke = color_of(look->find("smoke"), out.smoke);
        out.fire = color_of(look->find("fire"), out.fire);
        if (const json::Value* surface = look->find("surface"); surface != nullptr) {
            out.surface = surface->as_bool();
        }
        out.depth = number(look->find("depth"), 0.0F);
        out.self_shadow = number(look->find("shadow"), out.self_shadow);
    }
    if (out.depth <= 0.0F) {
        out.depth = 8.0F * out.radius;
    }
    if (const json::Value* budget = doc.find("budget"); budget != nullptr) {
        out.budget_particles = static_cast<std::size_t>(number(budget->find("particles"), 0.0F));
        out.budget_work = static_cast<std::size_t>(number(budget->find("work"), 0.0F));
    }
    return out;
}

Result<std::shared_ptr<const FluidRecipe>> load_fluid(const World& world, std::string_view path,
                                                      const json::Value& overrides) {
    if (!is_fluid_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .fluid.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    const auto read = [&](std::string_view file) -> Result<std::string> {
        if (assets != nullptr) {
            return assets->read(file, "fluid");
        }
        if (world.file_reader()) {
            return world.file_reader()(file);
        }
        return make_error(ErrorCode::PlatformFailure, "the world has no files to read fluids from");
    };
    // The chain of files, each over the one it extends.
    std::vector<std::pair<std::string, json::Value>> chain;
    std::string key;
    std::string file(path);
    for (int depth = 0; depth < 8; ++depth) {
        auto text = read(file);
        if (!text) {
            return make_error(text.error().code, file + ": " + text.error().message);
        }
        key += file + "\n" + text.value() + "\n";
        auto doc = json::parse(text.value());
        if (!doc) {
            return make_error(ErrorCode::InvalidArgument, file + ": " + doc.error().message);
        }
        const json::Value* base = doc.value().find("extends");
        std::string next = base != nullptr && base->is_string() ? base->as_string() : "";
        chain.emplace_back(file, std::move(doc).value());
        if (next.empty()) {
            break;
        }
        if (depth == 7) {
            return make_error(ErrorCode::InvalidArgument, file + ": extends goes too deep");
        }
        file = std::move(next);
    }
    if (!overrides.is_null()) {
        key += json::write(overrides, json::Style::Compact);
    }
    FluidCache* cache = nullptr;
    if (assets != nullptr) {
        auto& ctx =
            const_cast<World&>(world).ecs().ctx(); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (ctx.find<FluidCacheHolder>() == nullptr) {
            ctx.emplace<FluidCacheHolder>(std::make_shared<FluidCache>());
        }
        cache = ctx.get<FluidCacheHolder>().get();
    }
    std::unique_lock<std::mutex> lock;
    const std::string cache_key =
        std::string(path) + "|" +
        (overrides.is_null() ? "" : json::write(overrides, json::Style::Compact));
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->entries.find(cache_key);
            found != cache->entries.end() && found->second.key == key) {
            return found->second.recipe;
        }
    }
    json::Value merged = chain.back().second;
    for (std::size_t i = chain.size() - 1; i-- > 0;) {
        merged = merge_patch(merged, chain[i].second);
    }
    if (!overrides.is_null()) {
        merged = merge_patch(merged, overrides);
    }
    if (json::Object* object = merged.is_object() ? &merged.as_object() : nullptr;
        object != nullptr) {
        object->erase("extends");
    }
    auto recipe = parse_fluid(merged);
    if (!recipe) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(path) + ": " + recipe.error().message);
    }
    auto made = std::make_shared<const FluidRecipe>(std::move(recipe).value());
    if (cache != nullptr) {
        cache->entries[cache_key] = FluidCacheEntry{.key = std::move(key), .recipe = made};
    }
    return made;
}

void FluidParticles::add(Vec3f p, Vec3f v, float t, float s, float f) {
    position.push_back(p);
    velocity.push_back(v);
    age.push_back(0.0F);
    temperature.push_back(t);
    smoke.push_back(s);
    fuel.push_back(f);
    density.push_back(1.0F);
    id.push_back(next_id++);
}

void FluidParticles::remove_if(const std::function<bool(std::size_t)>& doomed) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < position.size(); ++i) {
        if (doomed(i)) {
            continue;
        }
        if (kept != i) {
            position[kept] = position[i];
            velocity[kept] = velocity[i];
            age[kept] = age[i];
            temperature[kept] = temperature[i];
            smoke[kept] = smoke[i];
            fuel[kept] = fuel[i];
            density[kept] = density[i];
            id[kept] = id[i];
        }
        ++kept;
    }
    position.resize(kept);
    velocity.resize(kept);
    age.resize(kept);
    temperature.resize(kept);
    smoke.resize(kept);
    fuel.resize(kept);
    density.resize(kept);
    id.resize(kept);
}

void set_fluid_host_3d(World& world, FluidHost host) {
    world.ecs().ctx().insert_or_assign(FluidHost3DHolder{std::move(host)});
}

void set_fluid_host_2d(World& world, FluidHost host) {
    world.ecs().ctx().insert_or_assign(FluidHost2DHolder{std::move(host)});
}

float fluid_particle_volume(const FluidRecipe& recipe) {
    const float spacing = 2.0F * recipe.radius;
    return recipe.flat ? spacing * spacing : spacing * spacing * spacing;
}

float fluid_particle_mass(const FluidRecipe& recipe) {
    // 2D is in pixels at 100 a meter, a meter deep.
    const float scale = recipe.flat ? 0.0001F : 1.0F;
    return recipe.rest_density * fluid_particle_volume(recipe) * scale;
}

void step_fluid(const FluidRecipe& recipe, FluidParticles& particles,
                const FluidStepInputs& inputs) {
    const int steps = std::max(recipe.substeps, 1);
    const float dt = inputs.seconds / static_cast<float>(steps);
    if (dt <= 0.0F) {
        return;
    }
    for (int s = 0; s < steps; ++s) {
        substep(recipe, particles, inputs, dt);
    }
    for (float& age : particles.age) {
        age += inputs.seconds;
    }
}

Vec4 fluid_color(const FluidRecipe& recipe, const FluidParticles& particles, std::size_t i,
                 float light) {
    const auto mix = [](Vec4 a, Vec4 b, float t) {
        return Vec4{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t),
                    a.w + ((b.w - a.w) * t)};
    };
    if (!recipe.gas) {
        // Foam from a speed of 40 particle radii a second, all foam at twice it.
        const Vec3f v = particles.velocity[i];
        const float speed = length(v);
        const float calm = 40.0F * recipe.radius;
        const float foam = std::clamp((speed - calm) / calm, 0.0F, 1.0F);
        return mix(recipe.color, recipe.foam, foam * 0.8F);
    }
    const float density = std::clamp(particles.smoke[i], 0.0F, 1.0F);
    Vec4 smoke = recipe.smoke;
    smoke.w *= density;
    smoke.x *= light;
    smoke.y *= light;
    smoke.z *= light;
    // Glowing from four fifths of ignition up to half again past it.
    const float glow_from = recipe.ignition * 0.8F;
    const float span = std::max(recipe.ignition * 0.7F, 1.0F);
    const float glow = std::clamp((particles.temperature[i] - glow_from) / span, 0.0F, 1.0F);
    return mix(smoke, recipe.fire, glow);
}

bool fluid_is_spray(const FluidRecipe& recipe, const FluidParticles& particles, std::size_t i) {
    if (recipe.gas || i >= particles.density.size()) {
        return false;
    }
    // Few neighbors: a drop flying alone, or the thin edge of a splash.
    return particles.density[i] < 0.35F;
}

std::vector<float> fluid_transmittance(const FluidRecipe& recipe, const FluidParticles& particles,
                                       Vec3f toward) {
    const std::size_t n = particles.size();
    std::vector<float> out(n, 1.0F);
    const float l = length(toward);
    if (!recipe.gas || n == 0 || l < 1e-6F || recipe.self_shadow <= 0.0F) {
        return out;
    }
    const Vec3f to_light = toward * (1.0F / l);
    // Two axes square to the light: particles are found by where they fall on that plane, so
    // those between one and the light are its neighbors there, with no grid.
    Vec3f side = std::abs(to_light.y) < 0.9F ? cross(to_light, Vec3f{0.0F, 1.0F, 0.0F})
                                             : cross(to_light, Vec3f{1.0F, 0.0F, 0.0F});
    side = side * (1.0F / length(side));
    const Vec3f other = cross(to_light, side);
    std::vector<Vec3f> plane(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f p = particles.position[i];
        plane[i] = {dot(p, side), recipe.flat ? 0.0F : dot(p, other), 0.0F};
    }
    // Each puff draws three spacings wide, so it shades what is within half that across.
    const float reach = fluid_draw_size(recipe) * 0.5F;
    NeighborIndex index;
    index.build(plane, reach, true);
    const float reach2 = reach * reach;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f p = particles.position[i];
        float sum = 0.0F;
        index.around(plane[i], [&](std::uint32_t j) {
            if (j == i) {
                return;
            }
            const Vec3f across = plane[i] - plane[j];
            const float d2 = dot(across, across);
            if (d2 >= reach2 || dot(particles.position[j] - p, to_light) <= 0.0F) {
                return;
            }
            const float w = 1.0F - (d2 / reach2);
            sum += std::clamp(particles.smoke[j], 0.0F, 1.0F) * w * w;
        });
        out[i] = slash::exp(-recipe.self_shadow * 0.5F * sum);
    }
    return out;
}

float fluid_draw_size(const FluidRecipe& recipe) {
    return recipe.radius * (recipe.gas ? 6.0F : 2.6F);
}

const FluidParticles* fluid_particles(const World& world, entt::entity entity) {
    const auto* holder = world.ecs().ctx().find<FluidStoreHolder>();
    if (holder == nullptr || !world.valid(entity)) {
        return nullptr;
    }
    const auto found = (*holder)->fluids.find(world.id_of(entity));
    return found != (*holder)->fluids.end() ? &found->second : nullptr;
}

FluidParticles* fluid_particles_mut(World& world, entt::entity entity) {
    return const_cast<FluidParticles*>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
        fluid_particles(world, entity));
}

void install_fluids(World& world) {
    if (world.ecs().ctx().find<FluidStoreHolder>() != nullptr) {
        return;
    }
    store_of(world);
    world.add_reset([](World& w) {
        if (auto* holder = w.ecs().ctx().find<FluidStoreHolder>(); holder != nullptr) {
            (*holder)->fluids.clear();
            (*holder)->warned.clear();
        }
    });
    world.add_memory_source("fluids", [](const World& w) {
        MemoryPart part;
        part.name = "fluids";
        std::uint64_t bytes = 0;
        std::uint64_t count = 0;
        if (const auto* holder = w.ecs().ctx().find<FluidStoreHolder>(); holder != nullptr) {
            for (const auto& [id, particles] : (*holder)->fluids) {
                bytes += (particles.position.capacity() + particles.velocity.capacity()) *
                             sizeof(Vec3f) +
                         (particles.age.capacity() + particles.temperature.capacity() +
                          particles.smoke.capacity() + particles.fuel.capacity() +
                          particles.density.capacity()) *
                             sizeof(float) +
                         particles.id.capacity() * sizeof(std::uint32_t);
                count += particles.size();
            }
            part.counts["fluids"] = (*holder)->fluids.size();
        }
        part.bytes = bytes;
        part.counts["particles"] = count;
        return part;
    });
    world.add_system("fluids", run,
                     {.doc = "Fills, pours into, steps and drains each Fluid3D and Fluid2D: "
                             "liquids and gases as particles with no grid.",
                      .priority = 958});
}

} // namespace slash::world
