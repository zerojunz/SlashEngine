#include "slash/world/damage.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/text.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>
#include <numbers>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine damage profile",
  "description": "How a body takes damage and breaks, after Chaos Destruction: its material (whose defaults fill what is left out), health, resistances, damage states, fracture pattern and levels, and whether its pieces stand linked until what holds them goes. Lengths are meters (2D: pixels), weights kilograms, colors [r, g, b] or [r, g, b, a] from 0 to 1.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 4}
  },
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "extends": {"type": "string", "pattern": "\\.damage\\.json$", "description": "A .damage.json file this one changes, as a JSON Merge Patch."},
    "name": {"type": "string", "description": "What it is called, for checks and reports."},
    "material": {"enum": ["wood", "glass", "metal", "stone", "concrete", "brick", "ceramic", "ice", "plastic", "flesh", "cloth"], "description": "What it is made of, whose defaults fill what is left out. Default stone."},
    "health": {"type": "number", "exclusiveMinimum": 0, "description": "Damage it takes before it breaks."},
    "resist": {
      "type": "object", "additionalProperties": false,
      "description": "What each kind of damage is multiplied by; 1 for a kind left out, 0 for one it ignores.",
      "properties": {
        "blunt": {"type": "number", "minimum": 0}, "pierce": {"type": "number", "minimum": 0},
        "cut": {"type": "number", "minimum": 0}, "explosive": {"type": "number", "minimum": 0},
        "fire": {"type": "number", "minimum": 0}, "impact": {"type": "number", "minimum": 0}
      }
    },
    "impact": {
      "type": "object", "additionalProperties": false,
      "description": "What collisions do: damage for each kilogram-meter per second (2D: kilogram-pixel) of an impact past the threshold.",
      "properties": {
        "threshold": {"type": "number", "minimum": 0},
        "damage": {"type": "number", "minimum": 0}
      }
    },
    "states": {
      "type": "array", "maxItems": 8,
      "description": "How it looks as it is damaged, highest below first.",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["below"],
        "properties": {
          "below": {"type": "number", "exclusiveMinimum": 0, "exclusiveMaximum": 1, "description": "The share of its health below which it looks so."},
          "tint": {"$ref": "#/$defs/color", "description": "Multiplies its StandardMaterial3D's albedo (2D: its modulate)."},
          "material": {"type": "string", "minLength": 1, "description": "A .standard_material.json file it draws with in its place."},
          "mesh": {"type": "string", "minLength": 1, "description": "A model file it draws in place of its own (2D: a texture)."}
        }
      }
    },
    "fracture": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "pattern": {"enum": ["voronoi", "impact", "radial", "splinters", "slices", "clusters", "none"], "description": "Where its cells' points go: anywhere, fine near the hit and coarse away, rings and spokes around the hit (glass), long splinters (wood), slabs, chunks of small cells (stone), or none (it never breaks)."},
        "pieces": {"type": "integer", "minimum": 2, "maximum": 256},
        "seed": {"type": "integer", "minimum": 0, "maximum": 4294967295},
        "levels": {"type": "integer", "minimum": 1, "maximum": 4, "description": "How many times pieces break; 1: its pieces don't break again."},
        "level_pieces": {"type": "integer", "minimum": 2, "maximum": 64, "description": "Cells a piece breaks into when it breaks again."},
        "fine": {"type": "number", "minimum": 0, "description": "How far around the hit the fine pieces reach; 0 for a quarter of its size."},
        "min_size": {"type": "number", "minimum": 0, "description": "Pieces smaller across than this break no more."}
      }
    },
    "density": {"type": "number", "exclusiveMinimum": 0, "description": "Kilograms a cubic meter (2D: a square meter of 64 pixels a side) its pieces weigh."},
    "connected": {"type": "boolean", "description": "Its pieces stand linked when it breaks, until a link gives way, and fall when no link holds them to an anchored piece."},
    "strength": {"type": "number", "exclusiveMinimum": 0, "description": "Newtons a square meter of a link holds (2D: a pixel of edge holds a 64th of it)."},
    "compression": {"type": "number", "minimum": 1, "description": "How many times more a link holds the piece standing on it."},
    "anchors": {"enum": ["bottom", "ends", "all", "none"], "description": "Which pieces are held in place: those at its bottom, at both ends of its longest level axis (a bridge's), all or none."},
    "debris": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "lifetime": {"type": "number", "minimum": 0, "description": "Seconds before loose pieces go; 0 keeps them."},
        "push": {"type": "number", "minimum": 0, "description": "Speed loose pieces fly from the hit at."}
      }
    }
  }
})";

constexpr int kMaxExtends = 8;

// What each material starts from.
constexpr std::string_view kDefaults = R"({
  "wood": {"health": 60, "resist": {"pierce": 0.6, "cut": 1.2, "fire": 2, "explosive": 1.2},
           "impact": {"threshold": 30, "damage": 0.5},
           "fracture": {"pattern": "splinters", "pieces": 10, "levels": 2, "level_pieces": 3},
           "density": 600, "strength": 80000, "compression": 8, "debris": {"push": 1}},
  "glass": {"health": 10, "resist": {"pierce": 1.5, "blunt": 1.5, "fire": 0.2},
            "impact": {"threshold": 5, "damage": 2},
            "fracture": {"pattern": "radial", "pieces": 24, "levels": 1},
            "density": 2500, "strength": 30000, "compression": 4,
            "debris": {"lifetime": 6, "push": 1.5}},
  "metal": {"health": 400, "resist": {"pierce": 0.4, "blunt": 0.5, "cut": 0.3, "fire": 0.2, "explosive": 0.8},
            "impact": {"threshold": 400, "damage": 0.2},
            "fracture": {"pattern": "slices", "pieces": 4, "levels": 1},
            "density": 7800, "strength": 2000000, "compression": 2},
  "stone": {"health": 200, "resist": {"pierce": 0.3, "cut": 0.3, "fire": 0, "explosive": 1.5},
            "impact": {"threshold": 150, "damage": 0.4},
            "fracture": {"pattern": "clusters", "pieces": 8, "levels": 2, "level_pieces": 4},
            "density": 2600, "strength": 150000, "compression": 20, "debris": {"push": 0.5}},
  "concrete": {"health": 250, "resist": {"pierce": 0.3, "cut": 0.2, "fire": 0, "explosive": 1.5},
               "impact": {"threshold": 200, "damage": 0.4},
               "fracture": {"pattern": "impact", "pieces": 12, "levels": 2, "level_pieces": 4},
               "density": 2400, "connected": true, "strength": 200000, "compression": 20,
               "debris": {"push": 0.5}},
  "brick": {"health": 150, "resist": {"pierce": 0.4, "cut": 0.3, "fire": 0, "explosive": 1.5},
            "impact": {"threshold": 120, "damage": 0.5},
            "fracture": {"pattern": "clusters", "pieces": 10, "levels": 2, "level_pieces": 3},
            "density": 1900, "connected": true, "strength": 100000, "compression": 15,
            "debris": {"push": 0.5}},
  "ceramic": {"health": 15, "resist": {"blunt": 1.5, "fire": 0},
              "impact": {"threshold": 8, "damage": 1.5},
              "fracture": {"pattern": "impact", "pieces": 10, "levels": 1},
              "density": 2300, "strength": 50000, "compression": 6,
              "debris": {"lifetime": 8, "push": 1}},
  "ice": {"health": 40, "resist": {"fire": 3, "pierce": 0.8},
          "impact": {"threshold": 20, "damage": 1},
          "fracture": {"pattern": "impact", "pieces": 12, "levels": 2, "level_pieces": 3},
          "density": 917, "strength": 40000, "compression": 6, "debris": {"push": 1}},
  "plastic": {"health": 50, "resist": {"pierce": 0.8, "fire": 1.5},
              "impact": {"threshold": 40, "damage": 0.5},
              "fracture": {"pattern": "slices", "pieces": 4, "levels": 1},
              "density": 950, "strength": 60000, "compression": 4},
  "flesh": {"health": 100, "resist": {"fire": 1.5, "explosive": 1.2},
            "impact": {"threshold": 60, "damage": 0.3},
            "fracture": {"pattern": "none"}, "density": 1000},
  "cloth": {"health": 20, "resist": {"cut": 3, "pierce": 1.5, "blunt": 0.2, "fire": 2},
            "impact": {"threshold": 1000, "damage": 0},
            "fracture": {"pattern": "none"}, "density": 300}
})";

const json::Value& defaults() {
    static const json::Value table = json::parse(kDefaults).value();
    return table;
}

std::string pointer_message(const std::vector<json::ValidationError>& errors) {
    return (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
           errors.front().message;
}

Result<json::Value> resolve(const json::Value& doc, const FileReader& read_file,
                            std::vector<std::string>& chain) {
    const json::Value* extends = doc.find("extends");
    if (extends == nullptr) {
        return doc;
    }
    const std::string& path = extends->as_string();
    if (std::ranges::find(chain, path) != chain.end()) {
        return make_error(ErrorCode::InvalidArgument, "\"" + path + "\" extends itself");
    }
    if (chain.size() >= kMaxExtends) {
        return make_error(ErrorCode::InvalidArgument, "damage profiles extend others at most " +
                                                          std::to_string(kMaxExtends) + " deep");
    }
    if (!read_file) {
        return make_error(ErrorCode::InvalidArgument,
                          "extends needs the project's files to read \"" + path + "\"");
    }
    auto text = read_file(path);
    if (!text) {
        return make_error(text.error().code, "\"" + path + "\": " + text.error().message);
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + path + "\": " + parsed.error().message);
    }
    if (const auto errors = validate_damage(parsed.value()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + path + "\": " + pointer_message(errors));
    }
    chain.push_back(path);
    auto parent = resolve(parsed.value(), read_file, chain);
    if (!parent) {
        return parent.error();
    }
    json::Value patch = doc;
    patch.as_object().erase("extends");
    return merge_patch(parent.value(), patch);
}

float number_or(const json::Value& object, std::string_view key, float fallback) {
    const json::Value* member = object.is_object() ? object.find(key) : nullptr;
    return member != nullptr && member->is_number() ? static_cast<float>(member->as_double())
                                                    : fallback;
}

// PCG32 (O'Neill, pcg-random.org, Apache 2.0), as scene3d's fracture had it, so a voronoi cut
// keeps the points it always had.
class Random {
public:
    Random(std::uint64_t seed, std::uint64_t stream) : increment_((stream << 1U) | 1U) {
        next();
        state_ += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = (old * 6364136223846793005ULL) + increment_;
        const auto shifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rotation = static_cast<std::uint32_t>(old >> 59U);
        return (shifted >> rotation) | (shifted << ((32U - rotation) & 31U));
    }
    // From 0 up to 1, in steps of 2^-24.
    double unit() { return static_cast<double>(next() >> 8U) * (1.0 / 16777216.0); }

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_;
};

constexpr std::uint64_t kFractureStream = 0x66726163U; // "frac"

using Point = std::array<double, 3>;

// The axes of a box, longest first; ties keep x, y, z order.
std::array<int, 3> axes_by_length(const Point& size) {
    std::array<int, 3> axes{0, 1, 2};
    std::ranges::stable_sort(axes, [&](int a, int b) {
        return size[static_cast<std::size_t>(a)] > size[static_cast<std::size_t>(b)];
    });
    return axes;
}

struct DamageCache {
    struct Entry {
        std::vector<std::pair<std::string, std::string>> files;
        std::shared_ptr<const DamageProfile> profile;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
};
using DamageCacheHolder = std::shared_ptr<DamageCache>;

} // namespace

bool is_damage_path(std::string_view path) {
    return path.ends_with(kDamageSuffix) && path.size() > kDamageSuffix.size();
}

const std::vector<std::string>& damage_kinds() {
    static const std::vector<std::string> kinds{"blunt",     "pierce", "cut",
                                                "explosive", "fire",   "impact"};
    return kinds;
}

const std::vector<std::string>& damage_materials() {
    static const std::vector<std::string> materials{"wood",     "glass", "metal",   "stone",
                                                    "concrete", "brick", "ceramic", "ice",
                                                    "plastic",  "flesh", "cloth"};
    return materials;
}

json::Value damage_material_defaults(std::string_view material) {
    const json::Value* found = defaults().find(material);
    return found != nullptr ? *found : json::Value(json::Object{});
}

json::Value damage_schema() {
    static const json::Value schema = json::parse(kSchema).value();
    return schema;
}

std::vector<json::ValidationError> validate_damage(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, damage_schema());
    if (!errors.empty()) {
        // A misspelled kind of damage says which one was meant.
        if (const json::Value* resist = doc.find("resist");
            resist != nullptr && resist->is_object()) {
            for (const auto& [kind, value] : resist->as_object()) {
                if (std::ranges::find(damage_kinds(), kind) != damage_kinds().end()) {
                    continue;
                }
                std::string message = "\"" + kind + "\" isn't a kind of damage";
                if (const std::string_view hint = closest_match(kind, damage_kinds());
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return {{.path = "/resist/" + kind, .message = message}};
            }
        }
        return errors;
    }
    if (const json::Value* states = doc.find("states"); states != nullptr) {
        double last = 1.0;
        for (std::size_t i = 0; i < states->as_array().size(); ++i) {
            const double below = states->as_array()[i].find("below")->as_double();
            if (below >= last) {
                errors.push_back({.path = "/states/" + std::to_string(i) + "/below",
                                  .message = "must be lower than the state before's"});
            }
            last = below;
        }
    }
    return errors;
}

Result<json::Value> resolve_damage(const json::Value& doc, const FileReader& read_file) {
    std::vector<std::string> chain;
    return resolve(doc, read_file, chain);
}

Result<DamageProfile> parse_damage(const json::Value& resolved) {
    if (const auto errors = validate_damage(resolved); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument, pointer_message(errors));
    }
    DamageProfile out;
    if (const json::Value* material = resolved.find("material"); material != nullptr) {
        out.material = material->as_string();
    }
    const json::Value doc = merge_patch(damage_material_defaults(out.material), resolved);
    if (const json::Value* name = doc.find("name"); name != nullptr) {
        out.name = name->as_string();
    }
    out.health = number_or(doc, "health", out.health);
    if (const json::Value* resist = doc.find("resist"); resist != nullptr) {
        for (const auto& [kind, value] : resist->as_object()) {
            out.resist[kind] = static_cast<float>(value.as_double());
        }
    }
    if (const json::Value* impact = doc.find("impact"); impact != nullptr) {
        out.impact_threshold = number_or(*impact, "threshold", out.impact_threshold);
        out.impact_damage = number_or(*impact, "damage", out.impact_damage);
    }
    if (const json::Value* states = doc.find("states"); states != nullptr) {
        for (const json::Value& state : states->as_array()) {
            DamageState made;
            made.below = number_or(state, "below", made.below);
            if (const json::Value* tint = state.find("tint"); tint != nullptr) {
                made.tinted = true;
                const json::Array& c = tint->as_array();
                made.tint = {static_cast<float>(c[0].as_double()),
                             static_cast<float>(c[1].as_double()),
                             static_cast<float>(c[2].as_double()),
                             c.size() > 3 ? static_cast<float>(c[3].as_double()) : 1.0F};
            }
            if (const json::Value* material = state.find("material"); material != nullptr) {
                made.material = material->as_string();
            }
            if (const json::Value* mesh = state.find("mesh"); mesh != nullptr) {
                made.mesh = mesh->as_string();
            }
            out.states.push_back(std::move(made));
        }
    }
    if (const json::Value* fracture = doc.find("fracture"); fracture != nullptr) {
        FractureSettings& f = out.fracture;
        if (const json::Value* pattern = fracture->find("pattern"); pattern != nullptr) {
            f.pattern = pattern->as_string();
        }
        if (const json::Value* pieces = fracture->find("pieces"); pieces != nullptr) {
            f.pieces = static_cast<int>(pieces->as_int());
        }
        if (const json::Value* seed = fracture->find("seed"); seed != nullptr) {
            f.seed = static_cast<std::uint32_t>(seed->as_int());
        }
        if (const json::Value* levels = fracture->find("levels"); levels != nullptr) {
            f.levels = static_cast<int>(levels->as_int());
        }
        if (const json::Value* pieces = fracture->find("level_pieces"); pieces != nullptr) {
            f.level_pieces = static_cast<int>(pieces->as_int());
        }
        f.fine = number_or(*fracture, "fine", f.fine);
        f.min_size = number_or(*fracture, "min_size", f.min_size);
    }
    out.density = number_or(doc, "density", out.density);
    if (const json::Value* connected = doc.find("connected"); connected != nullptr) {
        out.connected = connected->as_bool();
    }
    out.strength = number_or(doc, "strength", out.strength);
    out.compression = number_or(doc, "compression", out.compression);
    if (const json::Value* anchors = doc.find("anchors"); anchors != nullptr) {
        out.anchors = anchors->as_string();
    }
    if (const json::Value* debris = doc.find("debris"); debris != nullptr) {
        out.debris.lifetime = number_or(*debris, "lifetime", out.debris.lifetime);
        out.debris.push = number_or(*debris, "push", out.debris.push);
    }
    return out;
}

Result<std::shared_ptr<const DamageProfile>> load_damage(const World& world, std::string_view path,
                                                         const json::Value& overrides) {
    if (!is_damage_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .damage.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    std::vector<std::pair<std::string, std::string>> files;
    FileReader reader;
    if (assets != nullptr) {
        reader = [assets, &files](std::string_view file) -> Result<std::string> {
            auto text = assets->read(file, "damage");
            if (text) {
                files.emplace_back(std::string(file), text.value());
            }
            return text;
        };
    } else if (world.file_reader()) {
        reader = world.file_reader();
    } else {
        return make_error(ErrorCode::PlatformFailure,
                          "the world has no files to read damage profiles from");
    }
    const bool patched = overrides.is_object() && !overrides.as_object().empty();
    const std::string key =
        std::string(path) + (patched ? "|" + json::write(overrides, json::Style::Compact) : "");
    DamageCache* cache = nullptr;
    if (assets != nullptr) {
        // The cache lives in the world's context; a const world still keeps one.
        auto& ctx =
            const_cast<World&>(world).ecs().ctx(); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (ctx.find<DamageCacheHolder>() == nullptr) {
            ctx.emplace<DamageCacheHolder>(std::make_shared<DamageCache>());
        }
        cache = ctx.get<DamageCacheHolder>().get();
    }
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->entries.find(key); found != cache->entries.end()) {
            const bool same = std::ranges::all_of(found->second.files, [&](const auto& file) {
                auto text = assets->read(file.first, "damage");
                return text && text.value() == file.second;
            });
            if (same) {
                return found->second.profile;
            }
        }
    }
    auto bytes = reader(path);
    if (!bytes) {
        return bytes.error();
    }
    auto doc = json::parse(bytes.value());
    if (!doc) {
        return doc.error();
    }
    if (const auto errors = validate_damage(doc.value()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument, pointer_message(errors));
    }
    auto resolved = resolve_damage(doc.value(), reader);
    if (!resolved) {
        return resolved.error();
    }
    const json::Value merged =
        patched ? merge_patch(resolved.value(), overrides) : resolved.value();
    if (const auto errors = validate_damage(merged); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          pointer_message(errors) +
                              (patched ? " (with its overrides)" : " (with what it extends)"));
    }
    auto parsed = parse_damage(merged);
    if (!parsed) {
        return parsed.error();
    }
    auto profile = std::make_shared<const DamageProfile>(std::move(parsed.value()));
    if (cache != nullptr) {
        cache->entries[key] = {.files = std::move(files), .profile = profile};
    }
    return profile;
}

float damage_taken(const DamageProfile& profile, float amount, std::string_view kind) {
    const auto found = profile.resist.find(std::string(kind));
    return std::max(amount, 0.0F) * (found != profile.resist.end() ? found->second : 1.0F);
}

int damage_state(const DamageProfile& profile, float health) {
    int state = 0;
    const float share = profile.health > 0.0F ? health / profile.health : 0.0F;
    for (std::size_t i = 0; i < profile.states.size(); ++i) {
        if (share < profile.states[i].below) {
            state = static_cast<int>(i) + 1;
        }
    }
    return state;
}

FractureSites fracture_sites(const FractureCut& cut, Vec3 low, Vec3 high, bool flat) {
    FractureSites out;
    const Point min{low.x, low.y, low.z};
    const Point size{static_cast<double>(high.x) - low.x, static_cast<double>(high.y) - low.y,
                     flat ? 0.0 : static_cast<double>(high.z) - low.z};
    const std::size_t count = static_cast<std::size_t>(std::max(cut.cells, 0));
    Random random(cut.seed, kFractureStream);
    const auto anywhere = [&] {
        const double x = random.unit();
        const double y = random.unit();
        const double z = random.unit();
        return Point{min[0] + (x * size[0]), min[1] + (y * size[1]), min[2] + (z * size[2])};
    };
    const auto clamp_in = [&](Point p) {
        for (std::size_t a = 0; a < 3; ++a) {
            p[a] = std::clamp(p[a], min[a], min[a] + size[a]);
        }
        return p;
    };
    const double largest = std::max({size[0], size[1], size[2]});
    const Point at = clamp_in({cut.at.x, cut.at.y, flat ? min[2] : static_cast<double>(cut.at.z)});
    const double fine = cut.fine > 0.0F ? static_cast<double>(cut.fine) : 0.25 * largest;
    const std::array<int, 3> axes = axes_by_length(size);
    if (cut.pattern == "impact") {
        // Two thirds of the points near the hit, nearer more often, the rest anywhere.
        const std::size_t near = ((count * 2) + 2) / 3;
        for (std::size_t i = 0; i < count; ++i) {
            if (i >= near) {
                out.points.push_back(anywhere());
                continue;
            }
            Point v{};
            double length = 0.0;
            for (int tries = 0; tries < 16; ++tries) {
                v = {(random.unit() * 2.0) - 1.0, (random.unit() * 2.0) - 1.0,
                     flat ? 0.0 : (random.unit() * 2.0) - 1.0};
                length = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
                if (length <= 1.0) {
                    break;
                }
            }
            const double reach = fine * std::min(length, 1.0);
            out.points.push_back(
                clamp_in({at[0] + (v[0] * reach), at[1] + (v[1] * reach), at[2] + (v[2] * reach)}));
        }
    } else if (cut.pattern == "radial") {
        // Rings of spokes in the plane of the two longest axes, through the whole thickness, as
        // a pane shatters: rings closer together near the hit.
        const std::size_t first = flat ? 0 : static_cast<std::size_t>(axes[0]);
        const std::size_t second = flat ? 1 : static_cast<std::size_t>(axes[1]);
        double reach = 0.0;
        for (const double cx : {min[first], min[first] + size[first]}) {
            for (const double cy : {min[second], min[second] + size[second]}) {
                const double dx = cx - at[first];
                const double dy = cy - at[second];
                reach = std::max(reach, std::sqrt((dx * dx) + (dy * dy)));
            }
        }
        const std::size_t rings = std::max<std::size_t>(1, (count + 4) / 8);
        std::size_t made = 0;
        for (std::size_t ring = 0; ring < rings; ++ring) {
            const std::size_t left = count - made;
            const std::size_t here =
                ring + 1 == rings ? left : std::max<std::size_t>(left / (rings - ring), 3);
            const double t = (static_cast<double>(ring) + 0.6) / static_cast<double>(rings);
            const double radius = reach * t * t;
            const double turn = random.unit();
            for (std::size_t j = 0; j < here && made < count; ++j, ++made) {
                const double share =
                    (static_cast<double>(j) + turn + ((random.unit() - 0.5) * 0.4)) /
                    static_cast<double>(here);
                const Vec2f cs = cos_sin(static_cast<float>(share * 2.0 * std::numbers::pi));
                const double r = radius * (0.85 + (random.unit() * 0.3));
                Point p = at;
                p[first] += r * cs.x;
                p[second] += r * cs.y;
                out.points.push_back(clamp_in(p));
            }
        }
    } else if (cut.pattern == "splinters" || cut.pattern == "slices") {
        const auto grain = static_cast<std::size_t>(axes[0]);
        const bool splinters = cut.pattern == "splinters";
        for (std::size_t i = 0; i < count; ++i) {
            Point p = anywhere();
            if (splinters) {
                // Across the grain anywhere, along it near the middle: cells run its length.
                p[grain] = min[grain] + (size[grain] * (0.35 + (random.unit() * 0.3)));
            } else {
                // Evenly along it, a little off: slabs across it.
                for (std::size_t a = 0; a < 3; ++a) {
                    p[a] = min[a] + (size[a] * 0.5);
                }
                const double step =
                    size[grain] / static_cast<double>(std::max<std::size_t>(count, 1));
                p[grain] = min[grain] +
                           (step * (static_cast<double>(i) + 0.5 + ((random.unit() - 0.5) * 0.4)));
            }
            out.points.push_back(p);
        }
    } else if (cut.pattern == "clusters") {
        // Four small cells a piece, each joining the piece whose first cell is nearest.
        constexpr std::size_t kCellsAPiece = 4;
        for (std::size_t i = 0; i < count * kCellsAPiece; ++i) {
            out.points.push_back(anywhere());
        }
        out.groups.resize(out.points.size());
        for (std::size_t i = 0; i < out.points.size(); ++i) {
            std::size_t best = 0;
            double nearest = std::numeric_limits<double>::max();
            for (std::size_t c = 0; c < count; ++c) {
                double d = 0.0;
                for (std::size_t a = 0; a < 3; ++a) {
                    const double e = out.points[i][a] - out.points[c][a];
                    d += e * e;
                }
                if (d < nearest) {
                    nearest = d;
                    best = c;
                }
            }
            out.groups[i] = static_cast<int>(best);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            out.points.push_back(anywhere());
        }
    }
    return out;
}

FractureCut damage_cut(const DamageProfile& profile, Vec3 at, int level, std::uint32_t seed) {
    FractureCut cut;
    cut.pattern = profile.fracture.pattern == "none" ? "voronoi" : profile.fracture.pattern;
    cut.cells = level == 0 ? profile.fracture.pieces : profile.fracture.level_pieces;
    cut.seed = profile.fracture.seed ^ (seed * 0x9E3779B9U) ^ static_cast<std::uint32_t>(level);
    cut.at = at;
    cut.fine = profile.fracture.fine;
    return cut;
}

StressResult settle_stress(const std::vector<StressPiece>& pieces, float strength,
                           float compression, Vec3 up) {
    StressResult out;
    const std::size_t count = pieces.size();
    out.falls.assign(count, false);
    out.loads.assign(count, 0.0F);
    // The links as lists of other piece and area, both ways.
    std::vector<std::vector<std::pair<std::size_t, float>>> links(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto& [other, area] : pieces[i].links) {
            if (other >= count || other == i) {
                continue;
            }
            const auto has = [&](std::size_t from, std::size_t to) {
                return std::ranges::any_of(links[from],
                                           [&](const auto& l) { return l.first == to; });
            };
            if (!has(i, other)) {
                links[i].emplace_back(other, area);
            }
            if (!has(other, i)) {
                links[other].emplace_back(i, area);
            }
        }
    }
    constexpr int kUnreached = std::numeric_limits<int>::max();
    std::vector<int> depth(count, kUnreached);
    for (std::size_t round = 0; round <= count * 8; ++round) {
        // How many links each piece is from an anchor, nearest first.
        std::ranges::fill(depth, kUnreached);
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < count; ++i) {
            if (pieces[i].anchored) {
                depth[i] = 0;
                order.push_back(i);
            }
        }
        for (std::size_t next = 0; next < order.size(); ++next) {
            const std::size_t i = order[next];
            for (const auto& [other, area] : links[i]) {
                if (depth[other] == kUnreached) {
                    depth[other] = depth[i] + 1;
                    order.push_back(other);
                }
            }
        }
        // Each piece's load, farthest first, goes down the links to the pieces one nearer an
        // anchor, shared by their areas.
        std::vector<double> load(count, 0.0);
        for (std::size_t i = 0; i < count; ++i) {
            load[i] = pieces[i].weight;
        }
        double worst = 1.0;
        std::pair<std::size_t, std::size_t> worst_link{count, count};
        for (const std::size_t i : std::ranges::reverse_view(order)) {
            if (depth[i] == 0) {
                continue;
            }
            double area_down = 0.0;
            for (const auto& [other, area] : links[i]) {
                if (depth[other] == depth[i] - 1) {
                    area_down += std::max(area, 1e-9F);
                }
            }
            for (const auto& [other, area] : links[i]) {
                if (depth[other] != depth[i] - 1) {
                    continue;
                }
                const double share = load[i] * (std::max(area, 1e-9F) / area_down);
                load[other] += share;
                const Vec3 rise = pieces[i].center - pieces[other].center;
                const double above = (rise.x * up.x) + (rise.y * up.y) + (rise.z * up.z);
                const double holds = static_cast<double>(strength) * std::max(area, 1e-9F) *
                                     (above > 0.0 ? static_cast<double>(compression) : 1.0);
                out.loads[i] = std::max(out.loads[i], static_cast<float>(share));
                out.loads[other] = std::max(out.loads[other], static_cast<float>(share));
                const double ratio = share / holds;
                if (ratio > worst) {
                    worst = ratio;
                    worst_link = {std::min(i, other), std::max(i, other)};
                }
            }
        }
        if (worst_link.first == count) {
            break;
        }
        const std::size_t first = worst_link.first;
        const std::size_t second = worst_link.second;
        std::erase_if(links[first], [second](const auto& l) { return l.first == second; });
        std::erase_if(links[second], [first](const auto& l) { return l.first == first; });
        out.broken.push_back(worst_link);
        std::ranges::fill(out.loads, 0.0F);
    }
    for (std::size_t i = 0; i < count; ++i) {
        out.falls[i] = depth[i] == kUnreached;
    }
    return out;
}

} // namespace slash::world
