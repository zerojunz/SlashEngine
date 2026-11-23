#include "slash/world/water.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/log.hpp"
#include "slash/world/fluid.hpp"
#include "slash/world/path.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine water",
  "description": "A lake, ocean or river: Gerstner waves, ripples, foam and buoyancy, after Unreal's water bodies.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "properties": {
    "format": {"const": 1},
    "extends": {"type": "string", "pattern": "\\.water\\.json$", "description": "A .water.json file this one changes, as a JSON Merge Patch."},
    "name": {"type": "string", "description": "What it is called."},
    "kind": {"enum": ["lake", "ocean", "river"], "description": "lake (an area), ocean (a surface with no edge) or river (along a path). Default lake."},
    "space": {"enum": ["3d", "2d"], "description": "3d (meters) or 2d (pixels, seen from the side, waves along x). Default 3d."},
    "waves": {
      "type": "array", "maxItems": 16,
      "description": "Gerstner waves, summed; with wind, these come after the ones it raises.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["wavelength", "amplitude"],
        "properties": {
          "direction": {"type": "array", "items": {"type": "number"}, "minItems": 1, "maxItems": 2, "description": "Which way it travels: [x, z] (x alone in 2D). Default [1, 0]."},
          "wavelength": {"type": "number", "exclusiveMinimum": 0, "description": "Crest to crest, in meters (pixels in 2D)."},
          "amplitude": {"type": "number", "minimum": 0, "description": "How high a crest stands over the rest level."},
          "steepness": {"type": "number", "minimum": 0, "maximum": 1, "description": "0 round swells to 1 the sharpest crests that don't loop. Default 0.5."},
          "speed": {"type": "number", "minimum": 0, "description": "How fast crests travel; 0 for deep water's, sqrt(g wavelength / 2 pi). Default 0."},
          "phase": {"type": "number", "description": "Where in its cycle it starts, in radians. Default from its place in the list."}
        }
      }
    },
    "wind": {
      "type": "object", "additionalProperties": false,
      "description": "Waves a wind raises, as a fully developed sea (Pierson and Moskowitz): their wavelengths round the spectrum's peak and heights from its significant wave height.",
      "properties": {
        "speed": {"type": "number", "minimum": 0, "description": "The wind's speed in m/s (px/s in 2D). Default 6 m/s, or 300 px/s."},
        "direction": {"type": "array", "items": {"type": "number"}, "minItems": 1, "maxItems": 2, "description": "Which way it blows: [x, z]. Default [1, 0]."},
        "count": {"type": "integer", "minimum": 1, "maximum": 12, "description": "How many waves it raises. Default 6."},
        "spread": {"type": "number", "minimum": 0, "description": "The most the waves turn from the wind, in radians. Default 0.6."},
        "steepness": {"type": "number", "minimum": 0, "maximum": 1, "description": "The waves' steepness. Default 0.6."},
        "seed": {"type": "integer", "description": "Picks the waves' lengths, turns and phases. Default 1."}
      }
    },
    "density": {"type": "number", "exclusiveMinimum": 0, "description": "Mass a cubic meter (a square pixel in 2D), which lifts what it displaces. Default 1000, or 0.001 in 2D."},
    "buoyancy": {"type": "number", "minimum": 0, "description": "Scales the lift. Default 1."},
    "drag": {"type": "number", "minimum": 0, "description": "The share of a body's speed through the water lost a second, under water. Default 1."},
    "angular_drag": {"type": "number", "minimum": 0, "description": "The share of a body's spin lost a second, under water. Default 1."},
    "heave_damping": {"type": "number", "minimum": 0, "description": "The share of a body's speed up and down lost a second where it is wet, as the waves it makes carry its bobbing away. Default 8."},
    "current": {"type": "array", "items": {"type": "number"}, "minItems": 1, "maxItems": 2, "description": "Lakes and oceans: the water's drift [x, z] in its space, carrying waves and bodies. Default none."},
    "flow": {"type": "number", "description": "Rivers: how fast the current runs along the path (along x in 2D), fastest midstream. Default 1.5 m/s, or 60 px/s in 2D."},
    "ripples": {
      "type": "object", "additionalProperties": false,
      "description": "The rings bodies, drops and gameplay code leave.",
      "properties": {
        "wavelength": {"type": "number", "exclusiveMinimum": 0, "description": "Between a ring's crests. Default 0.5 m, or 30 px."},
        "speed": {"type": "number", "minimum": 0, "description": "How fast rings spread; 0 for waves that long. Default 0."},
        "decay": {"type": "number", "minimum": 0, "description": "How fast rings die away, as e^(-decay t). Default 1."},
        "splash": {"type": "number", "minimum": 0, "description": "Scales the rings bodies and liquids make. Default 1."}
      }
    },
    "look": {
      "type": "object", "additionalProperties": false,
      "description": "How it draws.",
      "properties": {
        "shallow": {"$ref": "#/$defs/color", "description": "Its color where shallow. Default [0.18, 0.55, 0.6, 0.55]."},
        "deep": {"$ref": "#/$defs/color", "description": "Its color where deep, and where the ground isn't measured. Default [0.02, 0.12, 0.25, 0.92]."},
        "foam": {"$ref": "#/$defs/color", "description": "Foam's color. Default [0.95, 0.97, 1, 1]."},
        "clarity": {"type": "number", "exclusiveMinimum": 0, "description": "How deep it is where it shows the deep color. Default 4 m, or 200 px."},
        "crest_foam": {"type": "number", "minimum": 0, "maximum": 1, "description": "Crests foam where the surface bunches up this much (its Jacobian, 1 at rest, below it); 0 for none. Default 0.85."},
        "shore_foam": {"type": "number", "minimum": 0, "description": "How deep the water foams to along the shore; 0 for none. Default 0.6 m, or 12 px."},
        "roughness": {"type": "number", "minimum": 0, "maximum": 1, "description": "As Surface3D's. Default 0.04."},
        "spacing": {"type": "number", "exclusiveMinimum": 0, "description": "How far apart the drawn surface's corners are. Default 0.5 m, or 8 px."},
        "view": {"type": "number", "exclusiveMinimum": 0, "description": "Oceans: how far round the camera the surface draws, in meters. Default 120."}
      }
    }
  },
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 4}
  }
})";

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;

struct WaterCacheEntry {
    std::string key;
    std::shared_ptr<const WaterRecipe> recipe;
};

struct WaterCache {
    std::mutex mutex;
    std::map<std::string, WaterCacheEntry, std::less<>> entries;
};
using WaterCacheHolder = std::shared_ptr<WaterCache>;

// What one body of water keeps between ticks, by entity ID.
struct WaterState {
    std::vector<WaterRipple> ripples;
    std::string ground_key;
    std::vector<float> ground;
    int ground_columns = 0;
    int ground_rows = 0;
    float ground_cell = 0.0F;
    std::map<std::string, bool, std::less<>> under;  // bodies by ID: whether under the surface
    std::map<std::string, double, std::less<>> wake; // when each last left a wake
};

struct WaterStore {
    double time = 0.0;
    float last_tick = 1.0F / 60.0F;
    std::map<std::string, WaterState, std::less<>> waters;
    std::map<std::string, bool, std::less<>> warned;
};
using WaterStoreHolder = std::shared_ptr<WaterStore>;

struct WaterInstalled {};

struct WaterHost3DHolder {
    WaterHost host;
};
struct WaterHost2DHolder {
    WaterHost host;
};

float number(const json::Value* value, float fallback) {
    return value != nullptr && value->is_number() ? static_cast<float>(value->as_double())
                                                  : fallback;
}

Vec4 color_of(const json::Value* value, Vec4 fallback) {
    if (value == nullptr || !value->is_array()) {
        return fallback;
    }
    const json::Array& a = value->as_array();
    const auto at = [&](std::size_t i, float f) {
        return i < a.size() && a[i].is_number() ? static_cast<float>(a[i].as_double()) : f;
    };
    return {at(0, fallback.x), at(1, fallback.y), at(2, fallback.z), at(3, 1.0F)};
}

// A direction from [x, z] (or [x]), made unit; fallback where it has no length.
Vec2f direction_of(const json::Value* value, Vec2f fallback, bool flat) {
    if (value == nullptr || !value->is_array() || value->as_array().empty()) {
        return fallback;
    }
    const json::Array& a = value->as_array();
    const auto x = static_cast<float>(a[0].as_double());
    const float z = !flat && a.size() > 1 ? static_cast<float>(a[1].as_double()) : 0.0F;
    const float n = std::sqrt((x * x) + (z * z));
    return n > 1e-6F ? Vec2f{x / n, z / n} : fallback;
}

std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// A number from 0 to 1 from a hash.
float unit(std::uint32_t h) {
    return static_cast<float>(mix(h) >> 8U) * (1.0F / 16777216.0F);
}

// The waves a wind raises: a Pierson-Moskowitz sea's peak (omega_p = 0.877 g / U) and
// significant height (Hs = 0.21 U^2 / g), shared out over count waves round the peak, longer
// ones taller, each turned from the wind by up to spread.
void raise_waves(WaterRecipe& out, const json::Value& wind) {
    const float g = water_gravity(out);
    const float speed = number(wind.find("speed"), out.flat ? 300.0F : 6.0F);
    if (speed <= 0.0F) {
        return;
    }
    const Vec2f toward = direction_of(wind.find("direction"), {1.0F, 0.0F}, out.flat);
    const auto count = static_cast<std::size_t>(number(wind.find("count"), 6.0F));
    const float spread = number(wind.find("spread"), 0.6F);
    const float steepness = number(wind.find("steepness"), 0.6F);
    const auto seed = static_cast<std::uint32_t>(number(wind.find("seed"), 1.0F));
    const float peak_frequency = 0.877F * g / speed;
    const float peak_length = kTau * g / (peak_frequency * peak_frequency);
    const float significant = 0.21F * speed * speed / g;
    std::vector<float> lengths;
    std::vector<float> weights;
    float total = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        const float share =
            count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.5F;
        const float jitter =
            (unit(seed * 0x9E3779B9U + static_cast<std::uint32_t>(i)) - 0.5F) * 0.25F;
        const float factor = 0.35F + (1.15F * share) + jitter;
        lengths.push_back(peak_length * factor);
        weights.push_back(factor * factor);
        total += factor * factor;
    }
    // Hs = 4 sqrt(m0) and m0 = sum(a^2) / 2, so the amplitudes' squares sum to Hs^2 / 8.
    const float energy = significant * significant / 8.0F;
    for (std::size_t i = 0; i < count && out.waves.size() < kMaxWaterWaves; ++i) {
        const auto salt = static_cast<std::uint32_t>(i) * 0x85EBCA77U;
        Vec2f direction = toward;
        if (!out.flat) {
            const float turn = ((unit(seed ^ salt ^ 0x27D4EB2FU) * 2.0F) - 1.0F) * spread;
            const Vec2f cs = cos_sin(turn);
            direction = {(toward.x * cs.x) - (toward.y * cs.y),
                         (toward.x * cs.y) + (toward.y * cs.x)};
        }
        out.waves.push_back(WaterWave{.direction = direction,
                                      .wavelength = lengths[i],
                                      .amplitude = std::sqrt(energy * weights[i] / total),
                                      .steepness = steepness,
                                      .speed = 0.0F,
                                      .phase = unit(seed ^ salt ^ 0x165667B1U) * kTau});
    }
}

WaterStore& store_of(World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<WaterStoreHolder>() == nullptr) {
        ctx.emplace<WaterStoreHolder>(std::make_shared<WaterStore>());
    }
    return *ctx.get<WaterStoreHolder>();
}

const WaterStore* find_store(const World& world) {
    const auto* holder = world.ecs().ctx().find<WaterStoreHolder>();
    return holder != nullptr ? holder->get() : nullptr;
}

const WaterHost* host_of(const World& world, bool flat) {
    const auto& ctx = world.ecs().ctx();
    if (flat) {
        const auto* holder = ctx.find<WaterHost2DHolder>();
        return holder != nullptr ? &holder->host : nullptr;
    }
    const auto* holder = ctx.find<WaterHost3DHolder>();
    return holder != nullptr ? &holder->host : nullptr;
}

void warn_once(World& world, std::string_view id, const std::string& message) {
    WaterStore& store = store_of(world);
    if (store.warned.emplace(std::string(id) + "|" + message, true).second) {
        json::Value data;
        data["error"] = message;
        world.emit("water_failed", id, std::move(data));
        log::warn("a body of water can't be made", {{"id", std::string(id)}, {"error", message}});
    }
}

// The rest level, the area's local place and the river's direction at a world point.
struct Spot {
    bool inside = false;
    Vec2f local;       // in the area's space (x, z; x in 2D)
    float rest = 0.0F; // the surface's height at rest: world y (y down in 2D)
    Vec3f flow;        // the river's or current's velocity there
    float depth = std::numeric_limits<float>::infinity();
};

float ground_at(const WaterBody& body, Vec2f local) {
    if (body.ground.empty() || body.ground_cell <= 0.0F) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float u = (local.x + body.half.x) / body.ground_cell;
    const float v = body.flat ? 0.0F : (local.y + body.half.y) / body.ground_cell;
    const int cols = body.ground_columns;
    const int rows = body.ground_rows;
    const float cu = std::clamp(u, 0.0F, static_cast<float>(cols - 1));
    const float cv = std::clamp(v, 0.0F, static_cast<float>(rows - 1));
    const int i0 = std::min(static_cast<int>(cu), cols - 1);
    const int j0 = std::min(static_cast<int>(cv), rows - 1);
    const int i1 = std::min(i0 + 1, cols - 1);
    const int j1 = std::min(j0 + 1, rows - 1);
    const float fu = cu - static_cast<float>(i0);
    const float fv = cv - static_cast<float>(j0);
    const auto at = [&](int i, int j) {
        return body.ground[(static_cast<std::size_t>(j) * static_cast<std::size_t>(cols)) +
                           static_cast<std::size_t>(i)];
    };
    const std::array<float, 4> corners{at(i0, j0), at(i1, j0), at(i0, j1), at(i1, j1)};
    const std::array<float, 4> weights{(1.0F - fu) * (1.0F - fv), fu * (1.0F - fv),
                                       (1.0F - fu) * fv, fu * fv};
    float sum = 0.0F;
    float weight = 0.0F;
    for (std::size_t c = 0; c < 4; ++c) {
        if (!std::isnan(corners.at(c))) {
            sum += corners.at(c) * weights.at(c);
            weight += weights.at(c);
        }
    }
    // Ground under only some corners is ground's edge; under none is none.
    return weight > 1e-4F ? sum / weight : std::numeric_limits<float>::quiet_NaN();
}

Spot spot_of(const WaterBody& body, Vec3f point) {
    const WaterRecipe& recipe = *body.recipe;
    Spot out;
    if (body.flat) {
        out.local = {point.x - to_float(body.place.origin.x), 0.0F};
        out.rest = to_float(body.place.origin.y);
        out.inside = std::abs(out.local.x) <= body.half.x;
        const float drift =
            recipe.kind == WaterRecipe::Kind::River ? recipe.flow : recipe.current.x;
        out.flow = {drift, 0.0F, 0.0F};
        out.depth = body.half.y;
        const float ground = ground_at(body, out.local);
        if (!std::isnan(ground)) {
            out.depth = std::min(out.depth, ground - out.rest);
        }
        return out;
    }
    const Vec3 local = body.inverse.apply(Vec3{point.x, point.y, point.z});
    out.local = {to_float(local.x), to_float(local.z)};
    if (recipe.kind == WaterRecipe::Kind::River) {
        // The nearest bit of the river's middle.
        float best = std::numeric_limits<float>::infinity();
        Vec3f middle;
        Vec2f along_dir;
        float lateral = 0.0F;
        bool within = false;
        for (std::size_t i = 0; i + 1 < body.river.size(); ++i) {
            const Vec3f a = body.river[i];
            const Vec3f b = body.river[i + 1];
            const float dx = b.x - a.x;
            const float dz = b.z - a.z;
            const float len2 = (dx * dx) + (dz * dz);
            const float raw =
                len2 > 1e-9F ? (((point.x - a.x) * dx) + ((point.z - a.z) * dz)) / len2 : 0.0F;
            const float t = std::clamp(raw, 0.0F, 1.0F);
            const float px = a.x + (dx * t);
            const float pz = a.z + (dz * t);
            const float d2 = ((point.x - px) * (point.x - px)) + ((point.z - pz) * (point.z - pz));
            if (d2 < best) {
                best = d2;
                middle = {px, a.y + ((b.y - a.y) * t), pz};
                const float len = std::sqrt(len2);
                along_dir = len > 1e-6F ? Vec2f{dx / len, dz / len} : Vec2f{1.0F, 0.0F};
                lateral = std::sqrt(d2);
                // Past either end of the river is out of it.
                within = (i != 0 || raw >= 0.0F) && (i + 2 != body.river.size() || raw <= 1.0F);
            }
        }
        if (body.river.size() < 2) {
            return out;
        }
        const float half_width = body.width * 0.5F;
        out.inside = within && lateral <= half_width;
        out.rest = middle.y;
        const float share = half_width > 0.0F ? std::min(lateral / half_width, 1.0F) : 1.0F;
        const float speed = recipe.flow * (1.0F - (share * share));
        out.flow = {along_dir.x * speed, 0.0F, along_dir.y * speed};
    } else {
        out.inside = !body.bounded ||
                     (std::abs(out.local.x) <= body.half.x && std::abs(out.local.y) <= body.half.y);
        out.rest = to_float(body.place.apply(Vec3{local.x, 0.0F, local.z}).y);
        const Vec3 drift = body.place.basis(Vec3{recipe.current.x, 0.0F, recipe.current.y});
        out.flow = to_float(drift);
    }
    const float ground = ground_at(body, out.local);
    if (!std::isnan(ground)) {
        out.depth = out.rest - ground;
    }
    return out;
}

// The waves' sums at a point at rest, in the area's space (x, z; x in 2D).
struct WaveSums {
    Vec2f shift; // sideways displacement
    float height = 0.0F;
    float sxx = 0.0F;
    float szz = 0.0F;
    float sxz = 0.0F;
    float gx = 0.0F;
    float gz = 0.0F;
    Vec3f velocity; // x, up, z
};

WaveSums wave_sums(const WaterRecipe& recipe, Vec2f at, float depth, double time,
                   bool derivatives) {
    WaveSums out;
    const auto count = static_cast<float>(std::max<std::size_t>(recipe.waves.size(), 1));
    for (const WaterWave& wave : recipe.waves) {
        const float k = kTau / wave.wavelength;
        const float omega = water_wave_frequency(recipe, wave);
        // Waves shrink as the water shoals under them.
        const float shoal =
            std::isfinite(depth) ? std::clamp(depth / (0.3F * wave.wavelength), 0.1F, 1.0F) : 1.0F;
        const float a = wave.amplitude * shoal;
        const float h = wave.steepness * std::min(a, 1.0F / (k * count));
        const Vec2f d = wave.direction;
        // The phase in whole cycles kept small, so float holds it far from the start.
        const double cycles = static_cast<double>(omega) * time / static_cast<double>(kTau);
        const auto turned = static_cast<float>(cycles - std::floor(cycles)) * kTau;
        const float theta = (k * ((d.x * at.x) + (d.y * at.y))) - turned + wave.phase;
        const Vec2f cs = cos_sin(theta);
        out.shift = out.shift + (d * (h * cs.x));
        out.height += a * cs.y;
        if (derivatives) {
            const float hks = h * k * cs.y;
            out.sxx += hks * d.x * d.x;
            out.szz += hks * d.y * d.y;
            out.sxz += hks * d.x * d.y;
            out.gx += a * k * d.x * cs.x;
            out.gz += a * k * d.y * cs.x;
            out.velocity.x += h * d.x * omega * cs.y;
            out.velocity.z += h * d.y * omega * cs.y;
            out.velocity.y -= a * omega * cs.x;
        }
    }
    return out;
}

// The rest place under a point: x0 with x0 + shift(x0) = x, by fixed-point iteration.
Vec2f invert(const WaterRecipe& recipe, Vec2f at, float depth, double time) {
    Vec2f x0 = at;
    for (int i = 0; i < 5; ++i) {
        const WaveSums sums = wave_sums(recipe, x0, depth, time, false);
        x0 = at - sums.shift;
    }
    return x0;
}

// The ripples' height at a world point (x, z; x in 2D), with its slope and the foam steep rings
// make.
struct RippleSum {
    float height = 0.0F;
    float dx = 0.0F;
    float dz = 0.0F;
    float foam = 0.0F;
};

RippleSum ripples_at(const WaterBody& body, Vec2f at, double time) {
    RippleSum out;
    const WaterRecipe& recipe = *body.recipe;
    const float lambda = recipe.ripple_wavelength;
    const float k = kTau / lambda;
    const float speed =
        recipe.ripple_speed > 0.0F ? recipe.ripple_speed : std::sqrt(water_gravity(recipe) / k);
    for (const WaterRipple& ripple : body.ripples) {
        const auto age = static_cast<float>(time - ripple.born);
        if (age < 0.0F) {
            continue;
        }
        const float envelope = ripple.strength * slash::exp(-recipe.ripple_decay * age);
        if (envelope < 1e-4F * lambda) {
            continue;
        }
        const float radius = speed * age;
        const float dx = at.x - ripple.at.x;
        const float dz = body.flat ? 0.0F : at.y - ripple.at.y;
        const float d = std::sqrt((dx * dx) + (dz * dz));
        const float u = (d - radius) / lambda;
        if (std::abs(u) > 3.0F) {
            continue;
        }
        // A ring spreads its height round itself as it grows; a 2D one runs both ways as is.
        const float spread = body.flat ? 1.0F : 1.0F / (1.0F + (radius / lambda));
        const float bell = slash::exp(-(u * u));
        const float amp = envelope * spread * bell;
        const Vec2f cs = cos_sin(k * (d - radius));
        out.height += amp * cs.x;
        const float slope = -amp * ((k * cs.y) + (2.0F * u / lambda * cs.x));
        if (d > 1e-6F) {
            out.dx += slope * dx / d;
            out.dz += slope * dz / d;
        }
        out.foam = std::max(out.foam, std::clamp(((amp * k) - 0.15F) / 0.35F, 0.0F, 1.0F));
    }
    return out;
}

Vec3f normalize(Vec3f v) {
    const float n = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return n > 1e-9F ? v * (1.0F / n) : Vec3f{0.0F, 1.0F, 0.0F};
}

// The point in the waves' space: the area's for lakes and oceans, carried back by the current,
// the world's x and z for rivers (their current carries the water, not the waves' pattern).
Vec2f wave_space(const WaterBody& body, const Spot& spot, Vec3f point, double time) {
    const WaterRecipe& recipe = *body.recipe;
    if (!body.flat && recipe.kind == WaterRecipe::Kind::River) {
        return {point.x, point.z};
    }
    const auto drift = static_cast<float>(time);
    if (body.flat) {
        const float along =
            recipe.kind == WaterRecipe::Kind::River ? recipe.flow : recipe.current.x;
        return {spot.local.x - (along * drift), 0.0F};
    }
    return {spot.local.x - (recipe.current.x * drift), spot.local.y - (recipe.current.y * drift)};
}

std::string body_key(const WaterBody& body, int columns, int rows, float cell) {
    std::string key = body.recipe->name + "|" + std::to_string(body.half.x) + "," +
                      std::to_string(body.half.y) + "|" + std::to_string(columns) + "x" +
                      std::to_string(rows) + "@" + std::to_string(cell);
    const auto add = [&](Vec3 v) {
        key += "|" + std::to_string(static_cast<double>(v.x)) + "," +
               std::to_string(static_cast<double>(v.y)) + "," +
               std::to_string(static_cast<double>(v.z));
    };
    add(body.place.x);
    add(body.place.z);
    add(body.place.origin);
    for (const Vec3f& p : body.river) {
        add(Vec3{p.x, p.y, p.z});
    }
    return key;
}

// Measures the ground under a body's area, or takes what was measured for the same placing.
void fill_ground(const World& world, World* writable, WaterBody& body, bool measure) {
    const WaterRecipe& recipe = *body.recipe;
    if (body.half.x <= 0.0F || (!body.flat && body.half.y <= 0.0F)) {
        return;
    }
    // Cells of the drawn spacing, at most 128 along a side.
    const float longest = std::max(body.half.x, body.flat ? 0.0F : body.half.y) * 2.0F;
    const float cell = std::max(recipe.spacing, longest / 128.0F);
    const int columns = static_cast<int>(std::ceil(body.half.x * 2.0F / cell)) + 1;
    const int rows = body.flat ? 1 : static_cast<int>(std::ceil(body.half.y * 2.0F / cell)) + 1;
    const std::string key = body_key(body, columns, rows, cell);
    if (writable == nullptr || !measure) {
        // Only what was measured for this placing.
        const WaterStore* store = find_store(world);
        if (store == nullptr) {
            return;
        }
        const auto found = store->waters.find(body.id);
        if (found == store->waters.end() || found->second.ground_key != key) {
            return;
        }
        body.ground = found->second.ground;
        body.ground_columns = found->second.ground_columns;
        body.ground_rows = found->second.ground_rows;
        body.ground_cell = found->second.ground_cell;
        return;
    }
    WaterState& state = store_of(*writable).waters[body.id];
    if (state.ground_key != key) {
        const WaterHost* host = host_of(world, body.flat);
        if (host == nullptr || !host->ground) {
            return;
        }
        state.ground_key = key;
        state.ground.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows),
                            std::numeric_limits<float>::quiet_NaN());
        state.ground_columns = columns;
        state.ground_rows = rows;
        state.ground_cell = cell;
        // From a little over the surface, as deep as the water could be seen to be.
        const float lift = body.flat ? 4.0F : 0.5F;
        const float reach =
            body.flat ? body.half.y + (2.0F * lift) : std::max(recipe.clarity * 4.0F, 20.0F);
        for (int j = 0; j < rows; ++j) {
            for (int i = 0; i < columns; ++i) {
                const float lx = (static_cast<float>(i) * cell) - body.half.x;
                const float lz = body.flat ? 0.0F : (static_cast<float>(j) * cell) - body.half.y;
                Vec3f from;
                if (body.flat) {
                    from = {to_float(body.place.origin.x) + lx,
                            to_float(body.place.origin.y) - lift, 0.0F};
                } else {
                    const Vec3 at = body.place.apply(Vec3{lx, 0.0F, lz});
                    from = {to_float(at.x), to_float(at.y) + lift, to_float(at.z)};
                    if (recipe.kind == WaterRecipe::Kind::River) {
                        const Spot spot = spot_of(body, from);
                        if (!spot.inside) {
                            continue;
                        }
                        from.y = spot.rest + lift;
                    }
                }
                if (const auto ground = host->ground(from, reach); ground) {
                    state.ground[(static_cast<std::size_t>(j) * static_cast<std::size_t>(columns)) +
                                 static_cast<std::size_t>(i)] = *ground;
                }
            }
        }
    }
    body.ground = state.ground;
    body.ground_columns = state.ground_columns;
    body.ground_rows = state.ground_rows;
    body.ground_cell = state.ground_cell;
}

std::optional<WaterBody> make_body(const World& world, World* writable, entt::entity entity,
                                   bool flat, std::optional<Tree3D>& tree3,
                                   std::optional<Tree2D>& tree2, bool measure) {
    const auto warn = [&](std::string_view id, const std::string& message) {
        if (writable != nullptr) {
            warn_once(*writable, id, message);
        }
    };
    const auto& ecs = world.ecs();
    WaterBody body;
    body.entity = entity;
    body.id = std::string(world.id_of(entity));
    body.flat = flat;
    std::string recipe_path;
    json::Value overrides;
    if (flat) {
        const auto& water = ecs.get<const Water2D>(entity);
        recipe_path = water.recipe;
        overrides = water.overrides;
        if (!tree2) {
            tree2.emplace(world);
        }
        const Transform2D placed = tree2->global(entity);
        body.place.origin = Vec3{placed.position.x, placed.position.y, 0.0F};
        body.inverse.origin = Vec3{} - body.place.origin;
        body.half = {water.width * 0.5F, water.depth};
        body.visible = water.visible;
        body.z_index = water.z_index;
        body.z_as_relative = water.z_as_relative;
        body.modulate = water.modulate;
        body.light_mask = water.light_mask;
    } else {
        const auto& water = ecs.get<const Water3D>(entity);
        recipe_path = water.recipe;
        overrides = water.overrides;
        if (!tree3) {
            tree3.emplace(world);
        }
        body.place = tree3->global(entity);
        body.inverse = body.place.inverse();
        body.half = to_float(Vec2{water.size.x * 0.5F, water.size.y * 0.5F});
        body.width = water.width;
        body.visible = water.visible;
        body.layers = water.layers;
        if (!water.path.empty()) {
            const entt::entity path = world.find(water.path);
            const auto curve = path != entt::null ? curve_3d(world, path) : nullptr;
            if (curve == nullptr || curve->length() <= 0.0F) {
                warn(body.id, "its path \"" + water.path + "\" is no Spline3D");
            } else {
                const Affine3D at = tree3->global(path);
                const float step = std::max(water.width * 0.25F, 0.25F);
                const int pieces =
                    std::clamp(static_cast<int>(std::ceil(curve->length() / step)), 1, 2048);
                for (int i = 0; i <= pieces; ++i) {
                    const float along =
                        curve->length() * static_cast<float>(i) / static_cast<float>(pieces);
                    body.river.push_back(to_float(at.apply(curve->sample(along))));
                    body.river_along.push_back(along);
                }
            }
        }
    }
    auto recipe = load_water(world, recipe_path, overrides);
    if (!recipe) {
        warn(body.id, recipe.error().message);
        return std::nullopt;
    }
    body.recipe = std::move(recipe).value();
    if (body.recipe->flat != flat) {
        warn(body.id, flat ? "a Water2D needs a 2d recipe" : "a Water3D needs a 3d recipe");
        return std::nullopt;
    }
    if (!flat && body.recipe->kind == WaterRecipe::Kind::River) {
        if (body.river.size() < 2) {
            if (ecs.get<const Water3D>(entity).path.empty()) {
                warn(body.id, "a river needs a path");
            }
            return std::nullopt;
        }
        // The area is the river's bounds, in the world's axes round their middle.
        Vec3f low = body.river.front();
        Vec3f high = low;
        for (const Vec3f& p : body.river) {
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        const float pad = body.width * 0.5F;
        body.place = Affine3D{};
        body.place.origin = Vec3{(low.x + high.x) * 0.5F, 0.0F, (low.z + high.z) * 0.5F};
        body.inverse = body.place.inverse();
        body.half = {((high.x - low.x) * 0.5F) + pad, ((high.z - low.z) * 0.5F) + pad};
    }
    body.bounded = flat || body.recipe->kind != WaterRecipe::Kind::Ocean;
    if (const WaterStore* store = find_store(world); store != nullptr) {
        if (const auto found = store->waters.find(body.id); found != store->waters.end()) {
            body.ripples = found->second.ripples;
        }
    }
    fill_ground(world, writable, body, measure);
    return body;
}

// Leaves a ring, adding to one started close by just before.
void ring(WaterState& state, const WaterRecipe& recipe, Vec2f at, double time, float strength) {
    if (strength <= 0.0F) {
        return;
    }
    const float near = recipe.ripple_wavelength;
    for (WaterRipple& ripple : state.ripples) {
        const float dx = ripple.at.x - at.x;
        const float dz = ripple.at.y - at.y;
        if (time - ripple.born < 0.1 && (dx * dx) + (dz * dz) < near * near) {
            ripple.strength = std::min(ripple.strength + strength, near * 0.6F);
            return;
        }
    }
    if (state.ripples.size() >= kMaxWaterRipples) {
        state.ripples.erase(state.ripples.begin());
    }
    state.ripples.push_back(
        WaterRipple{.at = at, .born = time, .strength = std::min(strength, near * 0.6F)});
}

struct Mover {
    std::string id;
    Vec3f at;
    Vec3f velocity;
};

void run_body(World& world, WaterBody& body, const std::vector<Mover>& movers, double time) {
    WaterStore& store = store_of(world);
    WaterState& state = store.waters[body.id];
    const WaterRecipe& recipe = *body.recipe;
    const float unit_length = body.flat ? 100.0F : 1.0F;
    // Rings die away.
    const float lambda = recipe.ripple_wavelength;
    std::erase_if(state.ripples, [&](const WaterRipple& ripple) {
        const auto age = static_cast<float>(time - ripple.born);
        return ripple.strength * slash::exp(-recipe.ripple_decay * age) < 1e-4F * lambda ||
               age > 60.0F;
    });
    // Bodies ring where they go in and come out, and leave wakes as they move through it.
    for (const Mover& mover : movers) {
        const WaterSample sample = sample_water(body, mover.at, time, false);
        if (!sample.inside) {
            state.under.erase(mover.id);
            continue;
        }
        const float up = body.flat ? sample.height - mover.at.y : mover.at.y - sample.height;
        // Above the surface is positive. A body goes in when its middle goes under, and comes out
        // when it is a quarter of a meter (25 px) clear, so one floating with its middle just over
        // the surface, or bobbing, doesn't go in and out each wave.
        const auto known = state.under.find(mover.id);
        const bool was = known != state.under.end() && known->second;
        const bool under = was ? up < 0.25F * unit_length : up < 0.0F;
        const Vec2f flat_at = body.flat ? Vec2f{mover.at.x, 0.0F} : Vec2f{mover.at.x, mover.at.z};
        const float vertical = std::abs(mover.velocity.y);
        if (known != state.under.end() && under != was) {
            json::Value data;
            data["water"] = body.id;
            data["speed"] = vertical;
            world.emit(under ? "water_entered" : "water_exited", mover.id, std::move(data));
            ring(state, recipe, flat_at, time,
                 recipe.splash * std::min(vertical * 0.04F, 0.4F * unit_length));
        }
        state.under[mover.id] = under;
        // Wakes: rings every eighth of a second behind what moves along the surface.
        const float sideways = std::sqrt((mover.velocity.x * mover.velocity.x) +
                                         (body.flat ? 0.0F : mover.velocity.z * mover.velocity.z));
        const float band = unit_length;
        if (std::abs(up) < band && sideways > 0.4F * unit_length) {
            const auto last = state.wake.find(mover.id);
            if (last == state.wake.end() || time - last->second >= 0.125) {
                state.wake[mover.id] = time;
                ring(state, recipe, flat_at, time,
                     recipe.splash * std::min(sideways * 0.015F, 0.15F * unit_length));
            }
        }
    }
    // Liquid falling into it becomes it, ringing where it lands.
    const auto take = [&](entt::entity fluid) {
        FluidParticles* particles = fluid_particles_mut(world, fluid);
        if (particles == nullptr || particles->size() == 0) {
            return;
        }
        particles->remove_if([&](std::size_t i) {
            const Vec3f p = particles->position[i];
            if (!particles->temperature.empty() && i < particles->temperature.size() &&
                particles->smoke[i] > 0.0F) {
                return false;
            }
            const WaterSample sample = sample_water(body, p, time);
            const bool sunk = body.flat ? p.y > sample.height : p.y < sample.height;
            if (!sample.inside || !sunk) {
                return false;
            }
            const Vec3f v = particles->velocity[i];
            ring(state, recipe, body.flat ? Vec2f{p.x, 0.0F} : Vec2f{p.x, p.z}, time,
                 recipe.splash * std::min(std::abs(v.y) * 0.01F, 0.1F * unit_length));
            return true;
        });
    };
    auto& ecs = world.ecs();
    if (body.flat) {
        for (const auto [entity, fluid] : ecs.view<const Fluid2D>().each()) {
            if (fluid.enabled) {
                auto recipe_of = load_fluid(world, fluid.recipe, fluid.overrides);
                if (recipe_of && !recipe_of.value()->gas) {
                    take(entity);
                }
            }
        }
    } else {
        for (const auto [entity, fluid] : ecs.view<const Fluid3D>().each()) {
            if (fluid.enabled) {
                auto recipe_of = load_fluid(world, fluid.recipe, fluid.overrides);
                if (recipe_of && !recipe_of.value()->gas) {
                    take(entity);
                }
            }
        }
    }
}

void run(World& world, double tick_seconds) {
    WaterStore& store = store_of(world);
    store.time += tick_seconds;
    store.last_tick = static_cast<float>(tick_seconds);
    auto& ecs = world.ecs();
    if (ecs.view<const Water3D>().empty() && ecs.view<const Water2D>().empty()) {
        store.waters.clear();
        return;
    }
    std::map<std::string, bool, std::less<>> live;
    for (const bool flat : {false, true}) {
        std::vector<WaterBody> bodies = water_bodies(world, flat, true);
        if (bodies.empty()) {
            continue;
        }
        std::vector<Mover> movers;
        if (flat) {
            const Tree2D tree(world);
            for (const auto [entity, rigid] : ecs.view<const RigidBody2D>().each()) {
                const Transform2D at = tree.global(entity);
                movers.push_back({.id = std::string(world.id_of(entity)),
                                  .at = {to_float(at.position.x), to_float(at.position.y), 0.0F},
                                  .velocity = {to_float(rigid.linear_velocity.x),
                                               to_float(rigid.linear_velocity.y), 0.0F}});
            }
            for (const auto [entity, character] : ecs.view<const CharacterMover2D>().each()) {
                const Transform2D at = tree.global(entity);
                movers.push_back({.id = std::string(world.id_of(entity)),
                                  .at = {to_float(at.position.x), to_float(at.position.y), 0.0F},
                                  .velocity = {to_float(character.velocity.x),
                                               to_float(character.velocity.y), 0.0F}});
            }
        } else {
            const Tree3D tree(world);
            for (const auto [entity, rigid] : ecs.view<const RigidBody3D>().each()) {
                movers.push_back({.id = std::string(world.id_of(entity)),
                                  .at = to_float(tree.global(entity).origin),
                                  .velocity = to_float(rigid.linear_velocity)});
            }
            for (const auto [entity, character] : ecs.view<const CharacterMover3D>().each()) {
                movers.push_back({.id = std::string(world.id_of(entity)),
                                  .at = to_float(tree.global(entity).origin),
                                  .velocity = to_float(character.velocity)});
            }
        }
        std::erase_if(movers, [](const Mover& m) { return m.id.empty(); });
        std::ranges::sort(movers, {}, &Mover::id);
        for (WaterBody& body : bodies) {
            live.emplace(body.id, true);
            run_body(world, body, movers, store.time);
        }
    }
    std::erase_if(store.waters, [&](const auto& entry) { return !live.contains(entry.first); });
}

} // namespace

bool is_water_path(std::string_view path) {
    return path.ends_with(kWaterSuffix);
}

json::Value water_schema() {
    static const json::Value schema = [] {
        auto parsed = json::parse(kSchema);
        SLASH_ASSERT(parsed.ok(), "the water schema parses");
        return std::move(parsed).value();
    }();
    return schema;
}

std::vector<json::ValidationError> validate_water(const json::Value& doc) {
    return json::validate(doc, water_schema());
}

float water_gravity(const WaterRecipe& recipe) {
    return recipe.flat ? 980.0F : 9.8F;
}

float water_wave_frequency(const WaterRecipe& recipe, const WaterWave& wave) {
    const float k = kTau / wave.wavelength;
    return wave.speed > 0.0F ? k * wave.speed : std::sqrt(water_gravity(recipe) * k);
}

Result<WaterRecipe> parse_water(const json::Value& doc) {
    if (const auto errors = validate_water(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    WaterRecipe out;
    if (const json::Value* name = doc.find("name"); name != nullptr) {
        out.name = name->as_string();
    }
    if (const json::Value* kind = doc.find("kind"); kind != nullptr) {
        if (kind->as_string() == "ocean") {
            out.kind = WaterRecipe::Kind::Ocean;
        } else if (kind->as_string() == "river") {
            out.kind = WaterRecipe::Kind::River;
        }
    }
    const json::Value* space = doc.find("space");
    out.flat = space != nullptr && space->as_string() == "2d";
    if (const json::Value* wind = doc.find("wind"); wind != nullptr) {
        raise_waves(out, *wind);
    }
    if (const json::Value* waves = doc.find("waves"); waves != nullptr) {
        std::size_t index = 0;
        for (const json::Value& wave : waves->as_array()) {
            if (out.waves.size() >= kMaxWaterWaves) {
                break;
            }
            out.waves.push_back(WaterWave{
                .direction = direction_of(wave.find("direction"), {1.0F, 0.0F}, out.flat),
                .wavelength = number(wave.find("wavelength"), 8.0F),
                .amplitude = number(wave.find("amplitude"), 0.2F),
                .steepness = number(wave.find("steepness"), 0.5F),
                .speed = number(wave.find("speed"), 0.0F),
                .phase = number(wave.find("phase"), static_cast<float>(index) * 2.39996F)});
            ++index;
        }
    }
    out.density = number(doc.find("density"), out.flat ? 0.001F : 1000.0F);
    out.buoyancy = number(doc.find("buoyancy"), 1.0F);
    out.drag = number(doc.find("drag"), 1.0F);
    out.angular_drag = number(doc.find("angular_drag"), 1.0F);
    out.heave_damping = number(doc.find("heave_damping"), 8.0F);
    if (const json::Value* current = doc.find("current"); current != nullptr) {
        const json::Array& a = current->as_array();
        out.current = {static_cast<float>(a[0].as_double()),
                       !out.flat && a.size() > 1 ? static_cast<float>(a[1].as_double()) : 0.0F};
    }
    out.flow = number(doc.find("flow"), out.flat ? 60.0F : 1.5F);
    if (out.kind != WaterRecipe::Kind::River) {
        out.flow = 0.0F;
    }
    out.ripple_wavelength = out.flat ? 30.0F : 0.5F;
    if (const json::Value* ripples = doc.find("ripples"); ripples != nullptr) {
        out.ripple_wavelength = number(ripples->find("wavelength"), out.ripple_wavelength);
        out.ripple_speed = number(ripples->find("speed"), 0.0F);
        out.ripple_decay = number(ripples->find("decay"), 1.0F);
        out.splash = number(ripples->find("splash"), 1.0F);
    }
    out.clarity = out.flat ? 200.0F : 4.0F;
    out.shore_foam = 0.6F * (out.flat ? 20.0F : 1.0F);
    out.spacing = 0.5F * (out.flat ? 16.0F : 1.0F);
    if (const json::Value* look = doc.find("look"); look != nullptr) {
        out.shallow = color_of(look->find("shallow"), out.shallow);
        out.deep = color_of(look->find("deep"), out.deep);
        out.foam = color_of(look->find("foam"), out.foam);
        out.clarity = number(look->find("clarity"), out.clarity);
        out.crest_foam = number(look->find("crest_foam"), out.crest_foam);
        out.shore_foam = number(look->find("shore_foam"), out.shore_foam);
        out.roughness = number(look->find("roughness"), out.roughness);
        out.spacing = number(look->find("spacing"), out.spacing);
        out.view = number(look->find("view"), out.view);
    }
    return out;
}

Result<std::shared_ptr<const WaterRecipe>> load_water(const World& world, std::string_view path,
                                                      const json::Value& overrides) {
    if (!is_water_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .water.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    const auto read = [&](std::string_view file) -> Result<std::string> {
        if (assets != nullptr) {
            return assets->read(file, "water");
        }
        if (world.file_reader()) {
            return world.file_reader()(file);
        }
        return make_error(ErrorCode::PlatformFailure, "the world has no files to read water from");
    };
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
    WaterCache* cache = nullptr;
    if (assets != nullptr) {
        auto& ctx =
            const_cast<World&>(world).ecs().ctx(); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (ctx.find<WaterCacheHolder>() == nullptr) {
            ctx.emplace<WaterCacheHolder>(std::make_shared<WaterCache>());
        }
        cache = ctx.get<WaterCacheHolder>().get();
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
    auto recipe = parse_water(merged);
    if (!recipe) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(path) + ": " + recipe.error().message);
    }
    auto made = std::make_shared<const WaterRecipe>(std::move(recipe).value());
    if (cache != nullptr) {
        cache->entries[cache_key] = WaterCacheEntry{.key = std::move(key), .recipe = made};
    }
    return made;
}

namespace {

// The sample of a bit of water whose waves' sums are sums, at the place it has moved to: local
// in the area's space, at in the world (x, z; x in 2D).
WaterSample finish(const WaterBody& body, const Spot& spot, const WaveSums& sums, Vec2f local,
                   Vec3f at, double time, bool with_ripples = true) {
    const WaterRecipe& recipe = *body.recipe;
    WaterSample out;
    out.inside = spot.inside;
    out.depth = spot.depth;
    const RippleSum ripples =
        with_ripples ? ripples_at(body, body.flat ? Vec2f{at.x, 0.0F} : Vec2f{at.x, at.z}, time)
                     : RippleSum{};
    const float jacobian = body.flat
                               ? 1.0F - sums.sxx
                               : ((1.0F - sums.sxx) * (1.0F - sums.szz)) - (sums.sxz * sums.sxz);
    float foam = 0.0F;
    if (recipe.crest_foam > 0.0F) {
        foam = std::clamp((recipe.crest_foam - jacobian) / recipe.crest_foam * 4.0F, 0.0F, 1.0F);
    }
    foam = std::max(foam, ripples.foam);
    if (recipe.shore_foam > 0.0F && std::isfinite(spot.depth)) {
        foam = std::max(foam, std::clamp(1.0F - (spot.depth / recipe.shore_foam), 0.0F, 1.0F));
    }
    out.foam = foam;
    if (body.flat) {
        // Up is -y in 2D.
        out.height = spot.rest - sums.height - ripples.height;
        out.position = {at.x, out.height, 0.0F};
        const Vec3f up = normalize(Vec3f{-(sums.gx + ripples.dx), 1.0F - sums.sxx, 0.0F});
        out.normal = {up.x, -up.y, 0.0F};
        out.velocity = Vec3f{sums.velocity.x, -sums.velocity.y, 0.0F} + spot.flow;
        return out;
    }
    const float gx = sums.gx;
    const float gz = sums.gz;
    const Vec3f wave_normal = normalize(Vec3f{(-gx * (1.0F - sums.szz)) - (gz * sums.sxz), jacobian,
                                              (-gz * (1.0F - sums.sxx)) - (gx * sums.sxz)});
    const Vec3f slope{-ripples.dx, 0.0F, -ripples.dz};
    if (recipe.kind == WaterRecipe::Kind::River) {
        out.height = spot.rest + sums.height + ripples.height;
        out.position = {at.x, out.height, at.z};
        out.normal = normalize(wave_normal + slope);
        out.velocity = sums.velocity + spot.flow;
        return out;
    }
    // The waves are in the area's space; the place turns them into the world's.
    out.position = to_float(body.place.apply(Vec3{local.x, sums.height, local.y}));
    out.position.y += ripples.height;
    out.height = out.position.y;
    const Vec3f turned =
        normalize(to_float(body.place.basis(Vec3{wave_normal.x, wave_normal.y, wave_normal.z})));
    out.normal = normalize(turned + slope);
    out.velocity =
        to_float(body.place.basis(Vec3{sums.velocity.x, sums.velocity.y, sums.velocity.z})) +
        spot.flow;
    return out;
}

} // namespace

WaterSample sample_water(const WaterBody& body, Vec3f point, double time, bool ripples) {
    if (body.recipe == nullptr) {
        return {};
    }
    const WaterRecipe& recipe = *body.recipe;
    const Spot spot = spot_of(body, point);
    const Vec2f at = wave_space(body, spot, point, time);
    const Vec2f rest = invert(recipe, at, spot.depth, time);
    const WaveSums sums = wave_sums(recipe, rest, spot.depth, time, true);
    return finish(body, spot, sums, spot.local, point, time, ripples);
}

WaterSample displace_water(const WaterBody& body, Vec3f rest, double time) {
    if (body.recipe == nullptr) {
        return {};
    }
    const WaterRecipe& recipe = *body.recipe;
    const Spot spot = spot_of(body, rest);
    const Vec2f at = wave_space(body, spot, rest, time);
    const WaveSums sums = wave_sums(recipe, at, spot.depth, time, true);
    const Vec2f local = spot.local + sums.shift;
    if (body.flat) {
        return finish(body, spot, sums, local, {rest.x + sums.shift.x, 0.0F, 0.0F}, time);
    }
    if (recipe.kind == WaterRecipe::Kind::River) {
        return finish(body, spot, sums, local,
                      {rest.x + sums.shift.x, rest.y, rest.z + sums.shift.y}, time);
    }
    return finish(body, spot, sums, local, to_float(body.place.apply(Vec3{local.x, 0.0F, local.y})),
                  time);
}

Vec4 water_color(const WaterRecipe& recipe, float depth, float foam) {
    const float share =
        std::isfinite(depth) ? std::clamp(depth / recipe.clarity, 0.0F, 1.0F) : 1.0F;
    const auto mix_of = [](Vec4 a, Vec4 b, float t) {
        return Vec4{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t),
                    a.w + ((b.w - a.w) * t)};
    };
    // Shallow water fades out at the shore's edge.
    Vec4 color = mix_of(recipe.shallow, recipe.deep, share);
    if (std::isfinite(depth)) {
        color.w *= std::clamp(depth / std::max(recipe.shore_foam * 0.25F, 1e-3F), 0.0F, 1.0F);
    }
    const float f = std::clamp(foam, 0.0F, 1.0F);
    Vec4 foamed = mix_of(color, recipe.foam, f * 0.85F);
    foamed.w = std::max(foamed.w, recipe.foam.w * f * 0.9F);
    return foamed;
}

namespace {

std::vector<WaterBody> bodies_of(const World& world, World* writable, bool flat, bool measure) {
    std::vector<WaterBody> out;
    std::optional<Tree3D> tree3;
    std::optional<Tree2D> tree2;
    std::vector<entt::entity> entities;
    if (flat) {
        for (const auto [entity, water] : world.ecs().view<const Water2D>().each()) {
            entities.push_back(entity);
        }
    } else {
        for (const auto [entity, water] : world.ecs().view<const Water3D>().each()) {
            entities.push_back(entity);
        }
    }
    // In the order entities were made, as the registry stores them.
    std::ranges::reverse(entities);
    for (const entt::entity entity : entities) {
        if (world.id_of(entity).empty()) {
            continue;
        }
        if (auto body = make_body(world, writable, entity, flat, tree3, tree2, measure); body) {
            out.push_back(std::move(*body));
        }
    }
    return out;
}

} // namespace

std::vector<WaterBody> water_bodies(World& world, bool flat, bool measure) {
    return bodies_of(world, &world, flat, measure);
}

std::vector<WaterBody> water_bodies(const World& world, bool flat) {
    return bodies_of(world, nullptr, flat, false);
}

std::optional<WaterBody> water_body(World& world, entt::entity entity, bool measure) {
    const bool flat = world.ecs().all_of<Water2D>(entity);
    if (!flat && !world.ecs().all_of<Water3D>(entity)) {
        return std::nullopt;
    }
    if (world.id_of(entity).empty()) {
        return std::nullopt;
    }
    std::optional<Tree3D> tree3;
    std::optional<Tree2D> tree2;
    return make_body(world, &world, entity, flat, tree3, tree2, measure);
}

void set_water_host_3d(World& world, WaterHost host) {
    world.ecs().ctx().insert_or_assign(WaterHost3DHolder{.host = std::move(host)});
}

void set_water_host_2d(World& world, WaterHost host) {
    world.ecs().ctx().insert_or_assign(WaterHost2DHolder{.host = std::move(host)});
}

double water_time(const World& world, float interpolation) {
    const WaterStore* store = find_store(world);
    if (store == nullptr) {
        return 0.0;
    }
    return store->time + static_cast<double>(interpolation * store->last_tick);
}

void add_water_ripple(World& world, entt::entity water, Vec3f at, float strength) {
    const auto body = water_body(world, water);
    if (!body) {
        return;
    }
    WaterStore& store = store_of(world);
    ring(store.waters[body->id], *body->recipe, body->flat ? Vec2f{at.x, 0.0F} : Vec2f{at.x, at.z},
         store.time, strength);
}

void install_water(World& world) {
    if (world.ecs().ctx().find<WaterInstalled>() != nullptr) {
        return;
    }
    world.ecs().ctx().emplace<WaterInstalled>();
    (void)store_of(world);
    world.add_reset([](World& w) {
        WaterStore& store = store_of(w);
        store.time = 0.0;
        store.waters.clear();
        store.warned.clear();
    });
    world.add_memory_source("water", [](const World& w) {
        MemoryPart part;
        std::uint64_t bytes = 0;
        std::uint64_t ripples = 0;
        if (const WaterStore* store = find_store(w); store != nullptr) {
            for (const auto& [id, state] : store->waters) {
                bytes += (state.ripples.capacity() * sizeof(WaterRipple)) +
                         (state.ground.capacity() * sizeof(float)) + state.ground_key.capacity();
                ripples += state.ripples.size();
            }
            part.counts["waters"] = store->waters.size();
        }
        part.bytes = bytes;
        part.counts["ripples"] = ripples;
        return part;
    });
    world.add_system("water", run,
                     {.doc = "Counts the water's clock, measures shores and rings each Water3D "
                             "and Water2D where bodies and liquids go in, come out and move "
                             "through it.",
                      .priority = 959});
}

} // namespace slash::world
