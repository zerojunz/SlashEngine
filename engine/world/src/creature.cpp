// Creature recipes: their files, their measures and skeleton, which 3D builds and 2D cut-outs
// share, and the 2D cut-out, which the character_2d system keeps.
#include "slash/world/creature.hpp"

#include "creature_cutout.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/log.hpp"
#include "slash/world/components.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine creature",
  "description": "An animal built from a recipe, after Unreal's Mutable and its animal AI: its body plan, look, parts and behavior. A MeshInstance3D draws it as a rigged, animated model; a Creature2D as a cut-out of Polygon2Ds; the fauna system moves either as its behavior says. Colors are [r, g, b] or [r, g, b, a] from 0 to 1, lengths meters, speeds meters a second.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 3, "maxItems": 4},
    "scale": {"type": "number", "minimum": 0.5, "maximum": 2},
    "share": {"type": "number", "minimum": 0, "maximum": 1},
    "names": {"type": "array", "items": {"type": "string", "minLength": 1}, "maxItems": 16, "uniqueItems": true}
  },
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "extends": {"type": "string", "pattern": "\\.creature\\.json$", "description": "A .creature.json file this one changes, as a JSON Merge Patch: objects merge, other values (lists included) replace."},
    "name": {"type": "string", "description": "What it is called, for checks and reports."},
    "species": {"type": "string", "description": "What it is, which herds keep to and hunters hunt: \"deer\", \"wolf\". Default its name."},
    "kind": {"enum": ["bird", "fish", "insect", "grazer", "predator", "pet"], "description": "What kind of animal it is, which hunters may hunt by. Default grazer."},
    "body": {
      "type": "object", "additionalProperties": false,
      "description": "The body. Proportions are 1 for the plan's usual ones.",
      "properties": {
        "plan": {"enum": ["quadruped", "bird", "fish", "insect"], "description": "How it is built. Default by kind: birds bird, fish fish, insects insect, the rest quadruped."},
        "length": {"type": "number", "minimum": 0.02, "maximum": 8, "description": "Meters from nose to rump (a fish's or bird's body, an insect's head to tail). Default 1."},
        "height": {"type": "number", "minimum": 0.01, "maximum": 6, "description": "Meters to the top of the back, standing. Default 0.8."},
        "build": {"$ref": "#/$defs/share", "description": "0 slight to 1 heavy. Default 0.5."},
        "head": {"$ref": "#/$defs/scale", "description": "The head's size. Default 1."},
        "neck": {"$ref": "#/$defs/scale", "description": "The neck's length. Default 1."},
        "legs": {"$ref": "#/$defs/scale", "description": "The legs' share of its height. Default 1."},
        "tail": {"type": "number", "minimum": 0, "maximum": 2, "description": "The tail's length; 0 for none. Default 1."}
      }
    },
    "look": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "coat": {"$ref": "#/$defs/color", "description": "Its fur, feathers, scales or shell. Default [0.55, 0.4, 0.25]."},
        "belly": {"$ref": "#/$defs/color", "description": "The underside. Default [0.85, 0.78, 0.65]."},
        "markings": {"$ref": "#/$defs/color", "description": "The pattern's color, and hooves, manes and fins. Default [0.25, 0.18, 0.1]."},
        "pattern": {"enum": ["plain", "spots", "stripes", "patches"], "description": "Default plain."},
        "eyes": {"$ref": "#/$defs/color", "description": "Default [0.08, 0.06, 0.04]."}
      }
    },
    "parts": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "ears": {"enum": ["none", "short", "long", "pointed"], "description": "Quadrupeds only. Default short."},
        "horns": {"enum": ["none", "horns", "antlers"], "description": "Quadrupeds only. Default none."},
        "mane": {"type": "boolean", "description": "Quadrupeds only: hair down the neck, in the markings' color. Default false."},
        "antennae": {"type": "boolean", "description": "Insects only. Default false."},
        "wings": {"type": "number", "minimum": 0, "maximum": 20, "description": "Meters from wing tip to wing tip; birds and insects only, which fly with them. Default 0, or twice its length for a bird."}
      }
    },
    "behavior": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "temperament": {"enum": ["timid", "wary", "calm", "aggressive", "friendly"], "description": "timid flees whoever it notices, wary whoever comes within flee, calm only what hunts it, aggressive attacks people it notices, friendly comes to them. Default wary."},
        "walk_speed": {"type": "number", "exclusiveMinimum": 0, "maximum": 50, "description": "Default 1.2."},
        "run_speed": {"type": "number", "exclusiveMinimum": 0, "maximum": 100, "description": "Default 4."},
        "notice": {"type": "number", "minimum": 0, "maximum": 500, "description": "How far it sees people and predators. Default 10."},
        "flee": {"type": "number", "minimum": 0, "maximum": 500, "description": "How near a wary one lets them come. Default 5."},
        "roam": {"type": "number", "minimum": 0, "maximum": 1000, "description": "How far it wanders from where it started. Default 12."},
        "herd": {"type": "boolean", "description": "Keeps with others of its species: a herd, flock or school. Default false."},
        "herd_radius": {"type": "number", "exclusiveMinimum": 0, "maximum": 500, "description": "How far from the herd's middle it strays. Default 6."},
        "hunts": {"$ref": "#/$defs/names", "description": "Kinds or species it hunts once hungry, as a predator."},
        "food": {"$ref": "#/$defs/names", "description": "What it eats from people's hands; empty for anything."},
        "tame": {"type": "integer", "minimum": 0, "maximum": 100, "description": "Feedings that tame it; 0 for never. Default 0."},
        "ride": {"type": "boolean", "description": "Its owner can ride it once tamed. Default false."},
        "damage": {"type": "number", "minimum": 0, "description": "What an attack does. Default 10."},
        "reach": {"type": "number", "exclusiveMinimum": 0, "maximum": 50, "description": "How near it must be to attack or catch. Default 1.2."},
        "attack_cooldown": {"type": "number", "minimum": 0, "maximum": 60, "description": "Seconds between attacks. Default 1."},
        "hunger_rate": {"type": "number", "minimum": 0, "maximum": 1, "description": "Hunger it gains a second, from 0 full to 1; a hunter hunts past 0.5. Default 0.02."},
        "altitude": {"type": "number", "minimum": 0, "maximum": 1000, "description": "How high above the ground creature.place and creature.populate put birds and insects that fly, how deep fish. Default 0: 6 for birds, 1 for insects, 1.5 for fish."}
      }
    }
  }
})";

constexpr int kMaxExtends = 8;
constexpr float kPi = std::numbers::pi_v<float>;

void read(const json::Value& doc, std::string_view key, float& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<float>(value->as_double());
    }
}

void read(const json::Value& doc, std::string_view key, int& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<int>(value->as_int());
    }
}

void read(const json::Value& doc, std::string_view key, bool& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = value->as_bool();
    }
}

void read(const json::Value& doc, std::string_view key, std::string& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = value->as_string();
    }
}

void read(const json::Value& doc, std::string_view key, Vec4& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        const json::Array& items = value->as_array();
        out = {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double()),
               static_cast<float>(items[2].as_double()),
               items.size() > 3 ? static_cast<float>(items[3].as_double()) : 1.0F};
    }
}

void read(const json::Value& doc, std::string_view key, std::vector<std::string>& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out.clear();
        for (const json::Value& item : value->as_array()) {
            out.push_back(item.as_string());
        }
    }
}

const json::Value* section(const json::Value& doc, std::string_view key) {
    const json::Value* value = doc.find(key);
    return value != nullptr && value->is_object() ? value : nullptr;
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
        return make_error(ErrorCode::InvalidArgument, "creatures extend others at most " +
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
    if (const auto errors = json::validate(parsed.value(), creature_schema()); !errors.empty()) {
        return make_error(
            ErrorCode::InvalidArgument,
            "\"" + path + "\": " + (errors.front().path.empty() ? "root" : errors.front().path) +
                ": " + errors.front().message);
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

// The plan a recipe's JSON says, or its kind's; "" when neither is in it.
std::string plan_of(const json::Value& doc) {
    if (const json::Value* body = section(doc, "body"); body != nullptr) {
        if (const json::Value* plan = body->find("plan"); plan != nullptr) {
            return plan->as_string();
        }
    }
    const json::Value* kind = doc.find("kind");
    if (kind == nullptr) {
        // A grazer's, unless what it extends says otherwise.
        return doc.find("extends") == nullptr ? "quadruped" : std::string();
    }
    const std::string& name = kind->as_string();
    return name == "bird" || name == "fish" || name == "insect" ? name : "quadruped";
}

Vec4 darker(Vec4 color, float by) {
    return {color.x * by, color.y * by, color.z * by, color.w};
}

Vec2 flat(Vec3 p) {
    return {p.z, p.y};
}

// A rounded bar from a to b, radius wide, as a polygon.
std::vector<Vec2> bar(Vec2 a, Vec2 b, real_t radius) {
    const float angle = slash::atan2(static_cast<float>(b.y - a.y), static_cast<float>(b.x - a.x));
    std::vector<Vec2> out;
    constexpr int kArc = 5;
    for (int i = 0; i <= kArc; ++i) {
        const auto [c, s] =
            slash::cos_sin(angle - (kPi * 0.5F) + (kPi * static_cast<float>(i) / kArc));
        out.push_back({b.x + (radius * c), b.y + (radius * s)});
    }
    for (int i = 0; i <= kArc; ++i) {
        const auto [c, s] =
            slash::cos_sin(angle + (kPi * 0.5F) + (kPi * static_cast<float>(i) / kArc));
        out.push_back({a.x + (radius * c), a.y + (radius * s)});
    }
    return out;
}

// An ellipse about center, turned by angle.
std::vector<Vec2> ellipse(Vec2 center, real_t rx, real_t ry, float angle = 0.0F, int sides = 16) {
    const auto [tc, ts] = slash::cos_sin(angle);
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const auto [c, s] =
            slash::cos_sin(2.0F * kPi * static_cast<float>(i) / static_cast<float>(sides));
        const real_t x = rx * c;
        const real_t y = ry * s;
        out.push_back({center.x + (x * tc) - (y * ts), center.y + (x * ts) + (y * tc)});
    }
    return out;
}

struct ShapeSink {
    std::vector<CutoutBone>& bones;
    std::map<std::string, int, std::less<>> index;

    void add(std::string_view bone, std::vector<Vec2> points, Vec4 color) {
        const auto found = index.find(bone);
        if (found == index.end()) {
            return;
        }
        bones[static_cast<std::size_t>(found->second)].shapes.push_back(
            {.points = std::move(points), .color = color});
    }
};

// Where the pattern's marks go on a body seen from the side, as shares of its half length and
// depth from its middle.
constexpr std::array<std::array<float, 3>, 6> kSpots{{{-0.55F, 0.35F, 0.16F},
                                                      {-0.2F, 0.55F, 0.13F},
                                                      {0.15F, 0.3F, 0.15F},
                                                      {0.45F, 0.5F, 0.12F},
                                                      {-0.35F, -0.05F, 0.12F},
                                                      {0.3F, 0.0F, 0.11F}}};

// The marks of a pattern on a body ellipse about center, half by depth.
void add_pattern(ShapeSink& sink, std::string_view bone, const CreatureRecipe& recipe, Vec2 center,
                 real_t half, real_t depth) {
    const std::string& pattern = recipe.look.pattern;
    const Vec4 color = recipe.look.markings;
    if (pattern == "spots") {
        for (const auto& spot : kSpots) {
            sink.add(bone,
                     ellipse({center.x + (half * spot[0]), center.y + (depth * spot[1])},
                             half * spot[2], depth * spot[2] * 1.2F, 0.0F, 10),
                     color);
        }
    } else if (pattern == "stripes") {
        for (int i = 0; i < 5; ++i) {
            const real_t x = center.x + (half * (-0.6F + (0.3F * static_cast<float>(i))));
            sink.add(
                bone,
                bar({x, center.y + (depth * 0.85F)}, {x - (half * 0.08F), center.y}, half * 0.05F),
                color);
        }
    } else if (pattern == "patches") {
        sink.add(bone,
                 ellipse({center.x - (half * 0.35F), center.y + (depth * 0.25F)}, half * 0.35F,
                         depth * 0.45F, 0.3F),
                 color);
        sink.add(bone,
                 ellipse({center.x + (half * 0.4F), center.y + (depth * 0.1F)}, half * 0.25F,
                         depth * 0.4F, -0.2F),
                 color);
    }
}

std::size_t bone_at(const std::vector<CreatureBone>& bones, std::string_view name) {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return i;
        }
    }
    return 0;
}

// A recipe and the text of each file it was made from (the recipe, then what it extends), so
// it is parsed again only when one of them changes.
struct CacheEntry {
    std::vector<std::pair<std::string, std::string>> files;
    std::shared_ptr<const CreatureRecipe> recipe;
};

struct CreatureCache {
    std::mutex mutex;
    std::map<std::string, CacheEntry, std::less<>> recipes;
};
using CreatureCacheHolder = std::shared_ptr<CreatureCache>;

// What each Creature2D's parts were last built from, so they change only when it does.
struct CreatureCutouts {
    std::map<entt::entity, std::string> built;
    std::map<entt::entity, std::string> warned;
};
using CreatureCutoutsHolder = std::shared_ptr<CreatureCutouts>;

std::string cutout_key(const std::vector<CutoutBone>& bones, const Creature2D& creature) {
    std::string key = std::to_string(creature.pixels_per_meter) + "@" + creature.recipe;
    for (const CutoutBone& bone : bones) {
        key += "|" + bone.name;
        for (const CutoutShape& shape : bone.shapes) {
            key += ":" + std::to_string(shape.points.size()) + "," + std::to_string(shape.color.x) +
                   "," + std::to_string(shape.color.y) + "," + std::to_string(shape.color.z) + "," +
                   std::to_string(shape.points[0].x) + "," + std::to_string(shape.points[0].y);
        }
    }
    return key;
}

void run_creature(World& world, entt::entity entity, CreatureCutouts& state) {
    auto& ecs = world.ecs();
    const Creature2D creature = ecs.get<const Creature2D>(entity);
    if (!ecs.all_of<Transform2D>(entity)) {
        ecs.emplace<Transform2D>(entity);
    }
    // It faces right; flip_h mirrors the whole cut-out, so turning needs no rebuilding.
    auto& root = ecs.get<Transform2D>(entity);
    const real_t sx = std::abs(root.scale.x) > 0.0F ? std::abs(root.scale.x) : 1.0F;
    if (const real_t want = creature.flip_h ? -sx : sx; root.scale.x != want) {
        ecs.patch<Transform2D>(entity, [want](Transform2D& t) { t.scale.x = want; });
    }
    auto recipe = load_creature(world, creature.recipe);
    if (!recipe) {
        std::string& warned = state.warned[entity];
        if (warned != recipe.error().message) {
            warned = recipe.error().message;
            log::warn("creature can't be built",
                      {{"id", std::string(world.id_of(entity))}, {"problem", warned}});
        }
        return;
    }
    state.warned.erase(entity);
    const std::vector<CutoutBone> bones = creature_cutout(*recipe.value());
    const std::string key = cutout_key(bones, creature);
    if (const auto built = state.built.find(entity);
        built != state.built.end() && built->second == key) {
        return;
    }
    state.built[entity] = key;
    const std::string owner(world.id_of(entity));
    const real_t ppm = creature.pixels_per_meter;
    const auto pixels = [ppm](Vec2 p) { return Vec2{p.x * ppm, -p.y * ppm}; };
    std::vector<entt::entity> made(bones.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const CutoutBone& bone = bones[i];
        const std::string id = creature_bone_id(owner, bone.name);
        entt::entity part = world.find(id);
        const entt::entity parent =
            bone.parent < 0 ? entity : made[static_cast<std::size_t>(bone.parent)];
        if (parent == entt::null) {
            continue;
        }
        if (part == entt::null) {
            auto created = world.create(id);
            if (!created) {
                continue;
            }
            part = created.value();
            ecs.get<Transform2D>(parent).children.push_back(part);
        }
        // Its place is the recipe's; the fauna system only turns it.
        Transform2D transform =
            ecs.all_of<Transform2D>(part) ? ecs.get<const Transform2D>(part) : Transform2D{};
        transform.position = pixels(bone.position);
        ecs.emplace_or_replace<Transform2D>(part, std::move(transform));
        made[i] = part;
        Polygon2D polygon =
            ecs.all_of<Polygon2D>(part) ? ecs.get<const Polygon2D>(part) : Polygon2D{};
        polygon.polygon.clear();
        polygon.polygons.clear();
        polygon.vertex_colors.clear();
        polygon.color = {1.0F, 1.0F, 1.0F, 1.0F};
        polygon.z_index = bone.z_index;
        for (const CutoutShape& shape : bone.shapes) {
            std::vector<int> indices;
            for (const Vec2 point : shape.points) {
                indices.push_back(static_cast<int>(polygon.polygon.size()));
                polygon.polygon.push_back(pixels(point));
                polygon.vertex_colors.push_back(shape.color);
            }
            polygon.polygons.push_back(std::move(indices));
        }
        ecs.emplace_or_replace<Polygon2D>(part, std::move(polygon));
    }
}

} // namespace

bool is_creature_path(std::string_view path) {
    return path.ends_with(kCreatureSuffix);
}

json::Value creature_schema() {
    static const json::Value schema = json::parse(kSchema).value();
    return schema;
}

std::vector<json::ValidationError> validate_creature(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, creature_schema());
    if (!errors.empty()) {
        return errors;
    }
    const std::string plan = plan_of(doc);
    if (const json::Value* parts = section(doc, "parts"); parts != nullptr && !plan.empty()) {
        const auto set = [&](std::string_view key) {
            const json::Value* value = parts->find(key);
            if (value == nullptr) {
                return false;
            }
            if (value->is_bool()) {
                return value->as_bool();
            }
            if (value->is_string()) {
                return value->as_string() != "none";
            }
            return value->as_double() > 0.0;
        };
        const auto refuse = [&](std::string_view key, std::string message) {
            errors.push_back({"/parts/" + std::string(key), std::move(message)});
        };
        if (plan != "quadruped") {
            for (const std::string_view key : {"ears", "horns", "mane"}) {
                if (set(key)) {
                    refuse(key, "a " + plan + " has no place for " + std::string(key));
                }
            }
        }
        if (plan != "insect" && set("antennae")) {
            refuse("antennae", "only insects have antennae");
        }
        if (plan != "bird" && plan != "insect" && set("wings")) {
            refuse("wings", "only birds and insects have wings");
        }
    }
    if (const json::Value* behavior = section(doc, "behavior"); behavior != nullptr) {
        const json::Value* ride = behavior->find("ride");
        const json::Value* tame = behavior->find("tame");
        if (ride != nullptr && ride->as_bool() && (tame == nullptr || tame->as_int() <= 0)) {
            errors.push_back({"/behavior/ride", "only a creature that can be tamed (tame above "
                                                "0) can be ridden, by its owner"});
        }
        const json::Value* walk = behavior->find("walk_speed");
        const json::Value* run = behavior->find("run_speed");
        if (walk != nullptr && run != nullptr && run->as_double() < walk->as_double()) {
            errors.push_back({"/behavior/run_speed", "runs slower than it walks"});
        }
    }
    return errors;
}

Result<json::Value> resolve_creature(const json::Value& doc, const FileReader& read_file) {
    std::vector<std::string> chain;
    auto resolved = resolve(doc, read_file, chain);
    if (!resolved) {
        return resolved.error();
    }
    json::Value out = std::move(resolved).value();
    if (out.is_object()) {
        out.as_object().erase("extends");
    }
    return out;
}

Result<CreatureRecipe> parse_creature(const json::Value& source, const FileReader& read_file) {
    if (const auto errors = json::validate(source, creature_schema()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    auto resolved = resolve_creature(source, read_file);
    if (!resolved) {
        return resolved.error();
    }
    const json::Value& doc = resolved.value();
    // A file and what it extends are checked together, so a plan from one and parts from the
    // other still have to fit.
    if (const auto errors = validate_creature(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    CreatureRecipe out;
    read(doc, "name", out.name);
    read(doc, "species", out.species);
    if (out.species.empty()) {
        out.species = out.name;
    }
    read(doc, "kind", out.kind);
    out.body.plan = plan_of(doc).empty() ? "quadruped" : plan_of(doc);
    if (const json::Value* body = section(doc, "body"); body != nullptr) {
        read(*body, "length", out.body.length);
        read(*body, "height", out.body.height);
        read(*body, "build", out.body.build);
        read(*body, "head", out.body.head);
        read(*body, "neck", out.body.neck);
        read(*body, "legs", out.body.legs);
        read(*body, "tail", out.body.tail);
    }
    if (const json::Value* look = section(doc, "look"); look != nullptr) {
        read(*look, "coat", out.look.coat);
        read(*look, "belly", out.look.belly);
        read(*look, "markings", out.look.markings);
        read(*look, "pattern", out.look.pattern);
        read(*look, "eyes", out.look.eyes);
    }
    const bool quadruped = out.body.plan == "quadruped";
    if (!quadruped) {
        out.parts.ears = "none";
    }
    if (out.body.plan == "bird") {
        out.parts.wings = 2.0F * out.body.length;
    }
    if (const json::Value* parts = section(doc, "parts"); parts != nullptr) {
        read(*parts, "ears", out.parts.ears);
        read(*parts, "horns", out.parts.horns);
        read(*parts, "mane", out.parts.mane);
        read(*parts, "antennae", out.parts.antennae);
        read(*parts, "wings", out.parts.wings);
    }
    if (out.body.plan == "bird") {
        out.behavior.altitude = 6.0F;
    } else if (out.body.plan == "insect" && out.parts.wings > 0.0F) {
        out.behavior.altitude = 1.0F;
    } else if (out.body.plan == "fish") {
        out.behavior.altitude = 1.5F;
    }
    if (const json::Value* behavior = section(doc, "behavior"); behavior != nullptr) {
        CreatureBehavior& b = out.behavior;
        read(*behavior, "temperament", b.temperament);
        read(*behavior, "walk_speed", b.walk_speed);
        read(*behavior, "run_speed", b.run_speed);
        read(*behavior, "notice", b.notice);
        read(*behavior, "flee", b.flee);
        read(*behavior, "roam", b.roam);
        read(*behavior, "herd", b.herd);
        read(*behavior, "herd_radius", b.herd_radius);
        read(*behavior, "hunts", b.hunts);
        read(*behavior, "food", b.food);
        read(*behavior, "tame", b.tame);
        read(*behavior, "ride", b.ride);
        read(*behavior, "damage", b.damage);
        read(*behavior, "reach", b.reach);
        read(*behavior, "attack_cooldown", b.attack_cooldown);
        read(*behavior, "hunger_rate", b.hunger_rate);
        read(*behavior, "altitude", b.altitude);
    }
    if (out.behavior.run_speed < out.behavior.walk_speed) {
        out.behavior.run_speed = out.behavior.walk_speed;
    }
    return out;
}

bool creature_flies(const CreatureRecipe& recipe) {
    return (recipe.body.plan == "bird" || recipe.body.plan == "insect") &&
           recipe.parts.wings > 0.0F;
}

bool creature_swims(const CreatureRecipe& recipe) {
    return recipe.body.plan == "fish";
}

CreatureMeasures creature_measures(const CreatureRecipe& recipe) {
    const CreatureBody& b = recipe.body;
    CreatureMeasures m;
    m.length = b.length;
    m.height = b.height;
    const real_t build = 0.75F + (0.5F * b.build);
    if (b.plan == "fish") {
        m.body_depth = b.height * 0.5F;
        m.back = m.body_depth;
        m.body_radius = m.body_depth * 0.35F * build;
        m.body_half = b.length * 0.36F;
        m.body_z = b.length * 0.04F;
        m.head_radius = m.body_depth * 0.55F * b.head;
        m.snout = m.head_radius * 0.5F;
        m.tail = b.length * 0.24F * b.tail;
        m.tail_radius = m.body_depth * 0.85F;
        return m;
    }
    if (b.plan == "insect") {
        m.leg = std::clamp<real_t>(b.height * 0.55F * b.legs, b.height * 0.2F, b.height * 0.85F);
        m.body_depth = (b.height - m.leg) * 0.5F;
        m.back = m.leg + m.body_depth;
        m.body_radius = m.body_depth * build;
        m.body_half = b.length * 0.16F;
        m.body_z = b.length * 0.06F;
        m.leg_radius = std::max<real_t>(m.body_depth * 0.12F, b.length * 0.008F);
        m.shoulder_z = m.body_z + (m.body_half * 0.6F);
        m.hip_z = m.body_z - (m.body_half * 0.6F);
        m.head_radius = b.length * 0.1F * b.head;
        m.tail = b.length * 0.44F * std::max(b.tail, 0.25F);
        m.tail_radius = m.body_depth * 1.25F * build;
        m.wing = recipe.parts.wings * 0.5F;
        return m;
    }
    if (b.plan == "bird") {
        m.leg = std::clamp<real_t>(b.height * 0.35F * b.legs, b.height * 0.1F, b.height * 0.7F);
        m.body_depth = (b.height - m.leg) * 0.5F;
        m.back = m.leg + m.body_depth;
        m.body_radius = m.body_depth * 0.85F * build;
        m.body_half = b.length * 0.3F;
        m.body_z = 0.0F;
        m.leg_radius = std::max<real_t>(m.body_depth * 0.07F, 0.004F);
        m.hip_z = 0.0F;
        m.shoulder_z = m.body_half * 0.3F;
        m.neck = b.length * 0.12F * b.neck;
        m.head_radius = b.length * 0.11F * b.head;
        m.snout = m.head_radius * 0.9F;
        m.tail = b.length * 0.32F * b.tail;
        m.tail_radius = m.body_depth * 0.35F;
        m.wing = recipe.parts.wings * 0.5F;
        return m;
    }
    // A quadruped's legs meet its body a little above its belly.
    const real_t leg_share = std::clamp<real_t>(0.55F * b.legs, 0.25F, 0.8F);
    m.body_depth = (b.height * (1.0F - leg_share)) * 0.5F;
    m.back = b.height - m.body_depth;
    m.leg = m.back - (m.body_depth * 0.4F);
    m.body_radius = m.body_depth * 0.85F * build;
    m.body_half = b.length * 0.34F;
    m.body_z = -b.length * 0.06F;
    m.leg_radius = m.body_radius * (0.16F + (0.1F * b.build));
    m.hip_z = m.body_z - (m.body_half * 0.72F);
    m.shoulder_z = m.body_z + (m.body_half * 0.68F);
    m.neck = b.length * 0.2F * b.neck;
    m.head_radius = std::min<real_t>(b.length * 0.1F, m.body_depth * 0.85F) * b.head;
    m.snout = m.head_radius * 0.9F;
    m.tail = b.length * 0.4F * b.tail;
    m.tail_radius = std::max<real_t>(m.body_radius * 0.14F, 0.005F);
    return m;
}

std::vector<CreatureBone> creature_skeleton(const CreatureRecipe& recipe) {
    const CreatureMeasures m = creature_measures(recipe);
    const std::string& plan = recipe.body.plan;
    std::vector<CreatureBone> bones;
    const auto add = [&](std::string name, int parent, Vec3 head) {
        bones.push_back({.name = std::move(name), .parent = parent, .head = head});
        return static_cast<int>(bones.size()) - 1;
    };
    const int body = add("body", -1, {0.0F, m.back, m.body_z});
    const real_t rump = m.body_z - m.body_half;
    if (plan == "fish") {
        add("head", body, {0.0F, m.back, m.body_z + (m.body_half * 0.55F)});
        add("tail", body, {0.0F, m.back, rump + (m.body_half * 0.15F)});
        return bones;
    }
    if (plan == "insect") {
        add("head", body, {0.0F, m.back + (m.body_depth * 0.15F), m.body_z + m.body_half});
        add("tail", body, {0.0F, m.back, rump});
        for (const char* row : {"front", "middle", "back"}) {
            const std::string_view r(row);
            real_t z = m.body_z;
            if (r == "front") {
                z = m.shoulder_z;
            } else if (r == "back") {
                z = m.hip_z;
            }
            for (const real_t side : {1.0F, -1.0F}) {
                add("leg_" + std::string(row) + (side > 0.0F ? "_near" : "_far"), body,
                    {side * m.body_radius * 0.6F, m.leg, z});
            }
        }
        if (m.wing > 0.0F) {
            for (const real_t side : {1.0F, -1.0F}) {
                add(side > 0.0F ? "wing_near" : "wing_far", body,
                    {side * m.body_radius * 0.5F, m.back + (m.body_depth * 0.8F), m.body_z});
            }
        }
        return bones;
    }
    const real_t front = m.body_z + m.body_half;
    if (plan == "bird") {
        const int neck = add("neck", body,
                             {0.0F, m.back + (m.body_depth * 0.45F), front - (m.body_half * 0.2F)});
        add("head", neck,
            bones[static_cast<std::size_t>(neck)].head + Vec3{0.0F, m.neck * 0.8F, m.neck * 0.6F});
        add("tail", body, {0.0F, m.back + (m.body_depth * 0.25F), rump + (m.body_half * 0.1F)});
        for (const real_t side : {1.0F, -1.0F}) {
            add(side > 0.0F ? "leg_near" : "leg_far", body,
                {side * m.body_radius * 0.45F, m.leg + (m.body_depth * 0.3F), m.hip_z});
        }
        for (const real_t side : {1.0F, -1.0F}) {
            add(side > 0.0F ? "wing_near" : "wing_far", body,
                {side * m.body_radius * 0.9F, m.back + (m.body_depth * 0.45F), m.shoulder_z});
        }
        return bones;
    }
    const int neck =
        add("neck", body, {0.0F, m.back + (m.body_depth * 0.35F), front - (m.body_half * 0.1F)});
    add("head", neck,
        bones[static_cast<std::size_t>(neck)].head + Vec3{0.0F, m.neck * 0.55F, m.neck * 0.85F});
    add("tail", body, {0.0F, m.back + (m.body_depth * 0.45F), rump + (m.body_half * 0.05F)});
    for (const char* row : {"front", "back"}) {
        const real_t z = std::string_view(row) == "front" ? m.shoulder_z : m.hip_z;
        for (const real_t side : {1.0F, -1.0F}) {
            add("leg_" + std::string(row) + (side > 0.0F ? "_near" : "_far"), body,
                {side * m.body_radius * 0.6F, m.leg, z});
        }
    }
    return bones;
}

std::vector<CreatureAnimation> creature_animations(const CreatureRecipe& recipe) {
    const CreatureMeasures m = creature_measures(recipe);
    const double stride = std::max<double>(m.leg * 1.6, m.length * 0.4);
    std::vector<CreatureAnimation> out{
        {.name = "idle", .length = 3.0, .loop = true},
        {.name = "walk", .length = stride / recipe.behavior.walk_speed, .loop = true},
        {.name = "run", .length = stride * 1.6 / recipe.behavior.run_speed, .loop = true},
        {.name = "eat", .length = 2.0, .loop = true},
        {.name = "attack", .length = 0.6, .loop = false},
    };
    if (creature_flies(recipe)) {
        out.push_back(
            {.name = "fly", .length = recipe.body.plan == "insect" ? 0.08 : 0.4, .loop = true});
    }
    if (creature_swims(recipe)) {
        out.push_back({.name = "swim",
                       .length = std::max(0.3, static_cast<double>(m.length) * 0.8),
                       .loop = true});
    }
    for (CreatureAnimation& animation : out) {
        animation.length = std::clamp(animation.length, 0.05, 10.0);
    }
    return out;
}

std::vector<CreatureTurn> creature_pose(const CreatureRecipe& recipe, std::string_view animation,
                                        double time) {
    const std::vector<CreatureAnimation> animations = creature_animations(recipe);
    const auto found = std::ranges::find_if(
        animations, [&](const CreatureAnimation& a) { return a.name == animation; });
    const CreatureAnimation& playing = found != animations.end() ? *found : animations.front();
    const std::string_view name = playing.name;
    const double length = playing.length;
    // How far through the animation it is, from 0 to 1.
    double at = time / length;
    at = playing.loop ? at - std::floor(at) : std::clamp(at, 0.0, 1.0);
    const auto wave = [at](double shift, double cycles = 1.0) {
        const double turns = (at * cycles) + shift;
        return slash::cos_sin(2.0F * kPi * static_cast<float>(turns - std::floor(turns))).y;
    };
    const std::vector<CreatureBone> bones = creature_skeleton(recipe);
    const auto has = [&](std::string_view bone) {
        return std::ranges::any_of(bones, [&](const CreatureBone& b) { return b.name == bone; });
    };
    std::vector<CreatureTurn> out;
    const auto turn = [&](std::string_view bone, Vec3 axis, float angle) {
        if (angle != 0.0F && has(bone)) {
            out.push_back({.bone = std::string(bone), .axis = axis, .angle = angle});
        }
    };
    const Vec3 pitch{1.0F, 0.0F, 0.0F};
    const Vec3 yaw{0.0F, 1.0F, 0.0F};
    const Vec3 roll{0.0F, 0.0F, 1.0F};
    const std::string& plan = recipe.body.plan;
    const bool moving = name == "walk" || name == "run";
    // Wings are spread at rest, as they fly; on the ground they fold back along the body.
    if (name != "fly") {
        turn("wing_near", yaw, 1.4F);
        turn("wing_far", yaw, -1.4F);
    }
    if (plan == "fish" || name == "swim") {
        float swish = 0.12F;
        if (name == "swim" || name == "run") {
            swish = 0.45F;
        } else if (moving) {
            swish = 0.3F;
        }
        turn("tail", yaw, swish * wave(0.0));
        turn("body", yaw, -swish * 0.18F * wave(0.0));
        if (name == "attack") {
            turn("head", pitch, -0.3F * wave(0.0, 0.5));
        }
        if (plan == "fish") {
            return out;
        }
    }
    if (name == "fly") {
        const float flap = plan == "insect" ? 0.6F : 0.9F;
        turn("wing_near", roll, flap * wave(0.0));
        turn("wing_far", roll, -flap * wave(0.0));
        for (const char* leg : {"leg_near", "leg_far"}) {
            turn(leg, pitch, -1.2F);
        }
        turn("tail", pitch, 0.08F * wave(0.25));
        return out;
    }
    if (moving) {
        const bool run = name == "run";
        if (plan == "insect") {
            // A tripod gait: front and back near with the middle far, then the others.
            const float swing = run ? 0.55F : 0.4F;
            for (const char* leg : {"leg_front_near", "leg_middle_far", "leg_back_near"}) {
                turn(leg, pitch, swing * wave(0.0));
            }
            for (const char* leg : {"leg_front_far", "leg_middle_near", "leg_back_far"}) {
                turn(leg, pitch, swing * wave(0.5));
            }
            return out;
        }
        if (plan == "bird") {
            const float swing = run ? 0.8F : 0.5F;
            turn("leg_near", pitch, swing * wave(0.0));
            turn("leg_far", pitch, swing * wave(0.5));
            turn("neck", pitch, 0.15F * wave(0.0, 2.0));
            turn("tail", pitch, 0.06F * wave(0.25, 2.0));
            return out;
        }
        // A walk sets each foot down a quarter cycle after the last; a gallop moves the front
        // legs together and the back legs together, half a cycle apart.
        const float swing = run ? 0.7F : 0.45F;
        const std::array<std::pair<const char*, double>, 4> walk{{{"leg_back_near", 0.0},
                                                                  {"leg_front_near", 0.25},
                                                                  {"leg_back_far", 0.5},
                                                                  {"leg_front_far", 0.75}}};
        const std::array<std::pair<const char*, double>, 4> gallop{{{"leg_front_near", 0.0},
                                                                    {"leg_front_far", 0.1},
                                                                    {"leg_back_near", 0.5},
                                                                    {"leg_back_far", 0.6}}};
        for (const auto& [leg, shift] : run ? gallop : walk) {
            turn(leg, pitch, swing * wave(shift));
        }
        turn("neck", pitch, (run ? 0.12F : 0.06F) * wave(0.0, 2.0));
        turn("tail", yaw, 0.15F * wave(0.0));
        turn("tail", pitch, run ? -0.35F : 0.0F);
        return out;
    }
    if (name == "eat") {
        if (plan == "bird") {
            turn("neck", pitch, 0.55F + (0.45F * wave(0.0, 2.0)));
        } else {
            turn("neck", pitch, 0.95F + (0.06F * wave(0.0, 2.0)));
            turn("head", pitch, 0.35F);
        }
        turn("tail", yaw, 0.2F * wave(0.0));
        return out;
    }
    if (name == "attack") {
        // Draws back, strikes and comes back.
        float strike = 0.85F * static_cast<float>(1.0 - ((at - 0.5) / 0.5));
        if (at < 0.3) {
            strike = -0.45F * static_cast<float>(at / 0.3);
        } else if (at < 0.5) {
            strike = -0.45F + (1.3F * static_cast<float>((at - 0.3) / 0.2));
        }
        turn("neck", pitch, strike);
        turn("head", pitch, strike * 0.4F);
        if (plan == "quadruped") {
            const float lift = -0.6F * wave(0.0, 0.5);
            turn("leg_front_near", pitch, lift);
            turn("leg_front_far", pitch, lift * 0.8F);
        }
        if (plan == "insect") {
            turn("leg_front_near", pitch, -0.5F * wave(0.0, 0.5));
            turn("leg_front_far", pitch, -0.5F * wave(0.0, 0.5));
        }
        return out;
    }
    // Idle: breathing, a tail that sways and wings that twitch.
    turn("tail", yaw, (plan == "quadruped" ? 0.25F : 0.08F) * wave(0.0));
    turn("neck", pitch, 0.04F * wave(0.3));
    if (plan == "insect") {
        turn("wing_near", roll, 0.08F * wave(0.0, 3.0));
        turn("wing_far", roll, -0.08F * wave(0.0, 3.0));
    }
    return out;
}

std::vector<CutoutBone> creature_cutout(const CreatureRecipe& recipe) {
    const CreatureMeasures m = creature_measures(recipe);
    const std::vector<CreatureBone> skeleton = creature_skeleton(recipe);
    const std::string& plan = recipe.body.plan;
    std::vector<CutoutBone> bones;
    ShapeSink sink{bones, {}};
    for (const CreatureBone& bone : skeleton) {
        const bool far = bone.name.ends_with("_far");
        const bool limb = bone.name.starts_with("leg") || bone.name.starts_with("wing");
        int z = 0;
        if (far || bone.name == "tail") {
            z = -1;
        } else if (limb) {
            z = 1;
        }
        const Vec3 parent =
            bone.parent < 0 ? Vec3{} : skeleton[static_cast<std::size_t>(bone.parent)].head;
        bones.push_back({.name = bone.name,
                         .parent = bone.parent,
                         .position = flat(bone.head - parent),
                         .z_index = z,
                         .shapes = {}});
        sink.index[bone.name] = static_cast<int>(bones.size()) - 1;
    }
    const CreatureLook& look = recipe.look;
    const Vec4 coat = look.coat;
    const Vec4 far_coat = darker(coat, 0.75F);
    const Vec4 marks = look.markings;
    // The body, about its own middle.
    if (plan == "insect") {
        sink.add("body", ellipse({}, m.body_half * 1.1F, m.body_depth), coat);
        const real_t abdomen = m.tail * 0.55F;
        sink.add("tail", ellipse({-abdomen, 0.0F}, abdomen, m.tail_radius, 0.08F), coat);
        sink.add("tail",
                 ellipse({-abdomen, -m.tail_radius * 0.35F}, abdomen * 0.8F, m.tail_radius * 0.5F,
                         0.08F),
                 look.belly);
        if (look.pattern != "plain") {
            for (int i = 0; i < 3; ++i) {
                const real_t x = -abdomen * (0.45F + (0.45F * static_cast<float>(i)));
                sink.add(
                    "tail",
                    bar({x, m.tail_radius * 0.9F}, {x, -m.tail_radius * 0.6F}, abdomen * 0.08F),
                    marks);
            }
        }
        const real_t hr = m.head_radius;
        sink.add("head", ellipse({hr * 0.6F, 0.0F}, hr, hr * 0.85F), coat);
        sink.add("head", ellipse({hr * 1.0F, hr * 0.2F}, hr * 0.4F, hr * 0.45F, 0.0F, 10),
                 look.eyes);
        if (recipe.parts.antennae) {
            sink.add("head", bar({hr * 0.9F, hr * 0.6F}, {hr * 2.6F, hr * 2.4F}, hr * 0.07F),
                     marks);
        }
        for (const CreatureBone& bone : skeleton) {
            if (!bone.name.starts_with("leg")) {
                continue;
            }
            const bool far = bone.name.ends_with("_far");
            real_t reach = 0.0F;
            if (bone.name.find("front") != std::string::npos) {
                reach = m.leg * 0.6F;
            } else if (bone.name.find("back") != std::string::npos) {
                reach = -m.leg * 0.6F;
            }
            const Vec2 knee{reach * 0.5F, m.leg * 0.35F};
            const Vec4 tone = far ? darker(marks, 0.75F) : marks;
            sink.add(bone.name, bar({}, knee, m.leg_radius), tone);
            sink.add(bone.name, bar(knee, {reach, -m.leg}, m.leg_radius * 0.8F), tone);
        }
        for (const char* wing : {"wing_far", "wing_near"}) {
            const Vec4 tone{0.85F, 0.9F, 0.95F,
                            std::string_view(wing) == "wing_far" ? 0.35F : 0.5F};
            sink.add(
                wing,
                ellipse({-m.wing * 0.45F, m.wing * 0.12F}, m.wing * 0.5F, m.wing * 0.16F, 0.25F),
                tone);
        }
        return bones;
    }
    if (plan == "fish") {
        sink.add("body", ellipse({}, m.body_half, m.body_depth), coat);
        sink.add("body",
                 ellipse({0.0F, -m.body_depth * 0.45F}, m.body_half * 0.85F, m.body_depth * 0.45F),
                 look.belly);
        add_pattern(sink, "body", recipe, {0.0F, m.body_depth * 0.1F}, m.body_half * 0.85F,
                    m.body_depth * 0.6F);
        // The dorsal and side fins.
        sink.add("body",
                 {{m.body_half * 0.2F, m.body_depth * 0.9F},
                  {-m.body_half * 0.45F, m.body_depth * 1.45F},
                  {-m.body_half * 0.4F, m.body_depth * 0.8F}},
                 marks);
        sink.add("body",
                 {{m.body_half * 0.35F, -m.body_depth * 0.2F},
                  {m.body_half * 0.05F, -m.body_depth * 0.6F},
                  {m.body_half * 0.1F, -m.body_depth * 0.15F}},
                 darker(marks, 0.9F));
        const real_t hr = m.head_radius;
        sink.add("head", ellipse({hr * 0.25F, hr * 0.3F}, hr * 0.22F, hr * 0.22F, 0.0F, 10),
                 Vec4{0.95F, 0.95F, 0.9F, 1.0F});
        sink.add("head", ellipse({hr * 0.28F, hr * 0.3F}, hr * 0.12F, hr * 0.12F, 0.0F, 10),
                 look.eyes);
        sink.add("tail",
                 {{0.0F, m.body_depth * 0.25F},
                  {-m.tail, m.tail_radius},
                  {-m.tail * 0.8F, 0.0F},
                  {-m.tail, -m.tail_radius},
                  {0.0F, -m.body_depth * 0.25F}},
                 marks);
        return bones;
    }
    sink.add("body", ellipse({}, m.body_half, m.body_depth), coat);
    sink.add("body",
             ellipse({0.0F, -m.body_depth * 0.45F}, m.body_half * 0.8F, m.body_depth * 0.5F),
             look.belly);
    add_pattern(sink, "body", recipe, {0.0F, m.body_depth * 0.15F}, m.body_half * 0.9F,
                m.body_depth * 0.7F);
    const real_t hr = m.head_radius;
    if (plan == "bird") {
        const Vec3 neck = skeleton[bone_at(skeleton, "neck")].head;
        const Vec3 head = skeleton[bone_at(skeleton, "head")].head;
        sink.add("neck", bar({}, flat(head - neck), hr * 0.6F), coat);
        sink.add("head", ellipse({}, hr, hr * 0.92F), coat);
        sink.add("head",
                 {{hr * 0.75F, hr * 0.25F}, {hr + m.snout, -hr * 0.05F}, {hr * 0.75F, -hr * 0.3F}},
                 Vec4{0.92F, 0.68F, 0.2F, 1.0F});
        sink.add("head", ellipse({hr * 0.35F, hr * 0.3F}, hr * 0.17F, hr * 0.17F, 0.0F, 10),
                 look.eyes);
        sink.add("tail",
                 {{0.0F, m.body_depth * 0.3F},
                  {-m.tail, m.body_depth * 0.55F},
                  {-m.tail, -m.body_depth * 0.15F},
                  {0.0F, -m.body_depth * 0.25F}},
                 darker(coat, 0.85F));
        for (const char* leg : {"leg_far", "leg_near"}) {
            const bool far = std::string_view(leg) == "leg_far";
            const Vec4 tone = far ? Vec4{0.65F, 0.45F, 0.2F, 1.0F} : Vec4{0.85F, 0.6F, 0.25F, 1.0F};
            const real_t depth = skeleton[bone_at(skeleton, leg)].head.y;
            sink.add(leg, bar({}, {0.0F, -depth}, m.leg_radius), tone);
            sink.add(leg,
                     bar({-m.leg_radius * 2.0F, -depth}, {m.body_half * 0.35F, -depth},
                         m.leg_radius * 0.8F),
                     tone);
        }
        for (const char* wing : {"wing_far", "wing_near"}) {
            const bool far = std::string_view(wing) == "wing_far";
            // Folded along the body; the fauna system raises and lowers it to fly.
            sink.add(wing,
                     ellipse({-m.wing * 0.3F, 0.0F}, m.wing * 0.34F, m.body_depth * 0.55F, 0.12F),
                     far ? darker(marks, 0.8F) : darker(coat, 0.8F));
        }
        return bones;
    }
    // A quadruped.
    const Vec3 neck = skeleton[bone_at(skeleton, "neck")].head;
    const Vec3 head = skeleton[bone_at(skeleton, "head")].head;
    sink.add("neck", bar({}, flat(head - neck), hr * 0.62F), coat);
    if (recipe.parts.mane) {
        sink.add("neck",
                 bar({-hr * 0.25F, hr * 0.45F}, flat(head - neck) + Vec2{-hr * 0.4F, hr * 0.5F},
                     hr * 0.3F),
                 marks);
    }
    const std::string& ears = recipe.parts.ears;
    if (ears == "short") {
        sink.add("head",
                 {{-hr * 0.3F, hr * 0.6F}, {-hr * 0.45F, hr * 1.25F}, {hr * 0.05F, hr * 0.75F}},
                 coat);
    } else if (ears == "long") {
        sink.add("head", ellipse({-hr * 0.5F, hr * 1.3F}, hr * 0.22F, hr * 0.8F, 0.35F, 12), coat);
    } else if (ears == "pointed") {
        sink.add("head",
                 {{-hr * 0.35F, hr * 0.55F}, {-hr * 0.3F, hr * 1.6F}, {hr * 0.15F, hr * 0.7F}},
                 coat);
    }
    const std::string& horns = recipe.parts.horns;
    const Vec4 horn_color{0.88F, 0.84F, 0.72F, 1.0F};
    if (horns == "horns") {
        sink.add("head", bar({-hr * 0.1F, hr * 0.7F}, {-hr * 0.7F, hr * 1.7F}, hr * 0.13F),
                 horn_color);
    } else if (horns == "antlers") {
        const Vec4 antler{0.5F, 0.38F, 0.25F, 1.0F};
        const Vec2 root{-hr * 0.1F, hr * 0.75F};
        const Vec2 top{-hr * 0.7F, hr * 3.0F};
        sink.add("head", bar(root, top, hr * 0.08F), antler);
        sink.add("head", bar({-hr * 0.3F, hr * 1.5F}, {hr * 0.4F, hr * 2.3F}, hr * 0.06F), antler);
        sink.add("head", bar({-hr * 0.55F, hr * 2.3F}, {hr * 0.1F, hr * 3.0F}, hr * 0.06F), antler);
    }
    sink.add("head", ellipse({}, hr, hr * 0.85F), coat);
    sink.add("head", ellipse({hr * 0.8F, -hr * 0.25F}, m.snout, hr * 0.5F, -0.15F), coat);
    sink.add("head", ellipse({hr * 1.55F, -hr * 0.15F}, hr * 0.14F, hr * 0.12F, 0.0F, 8),
             darker(marks, 0.6F));
    sink.add("head", ellipse({hr * 0.3F, hr * 0.25F}, hr * 0.16F, hr * 0.16F, 0.0F, 10), look.eyes);
    if (m.tail > 0.0F) {
        const Vec2 tip{-m.tail * 0.75F, -m.tail * 0.6F};
        sink.add("tail", bar({}, tip, m.tail_radius), coat);
        sink.add("tail", ellipse(tip, m.tail_radius * 2.2F, m.tail_radius * 2.2F, 0.0F, 10), marks);
    }
    for (const CreatureBone& bone : skeleton) {
        if (!bone.name.starts_with("leg")) {
            continue;
        }
        const bool far = bone.name.ends_with("_far");
        const real_t height = bone.head.y;
        sink.add(bone.name, bar({}, {0.0F, -height + m.leg_radius}, m.leg_radius),
                 far ? far_coat : coat);
        sink.add(bone.name,
                 ellipse({m.leg_radius * 0.2F, -height + (m.leg_radius * 0.5F)},
                         m.leg_radius * 1.1F, m.leg_radius * 0.6F, 0.0F, 8),
                 far ? darker(marks, 0.75F) : marks);
    }
    return bones;
}

Result<std::shared_ptr<const CreatureRecipe>> load_creature(const World& world,
                                                            std::string_view path) {
    if (!is_creature_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .creature.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    const FileReader& reader = world.file_reader();
    if (assets == nullptr) {
        if (!reader) {
            return make_error(ErrorCode::PlatformFailure,
                              "the world has no files to read creatures from");
        }
        auto bytes = reader(path);
        if (!bytes) {
            return bytes.error();
        }
        auto doc = json::parse(bytes.value());
        if (!doc) {
            return doc.error();
        }
        auto recipe = parse_creature(doc.value(), reader);
        if (!recipe) {
            return recipe.error();
        }
        return std::shared_ptr<const CreatureRecipe>(
            std::make_shared<const CreatureRecipe>(std::move(recipe).value()));
    }
    // The model type loads .creature.json files as models (slash/scene3d/creature.hpp), so the
    // recipe is read as a watched file, and parsed again only when a file of it changed.
    const auto* holder = world.ecs().ctx().find<CreatureCacheHolder>();
    CreatureCache* cache = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->recipes.find(path); found != cache->recipes.end()) {
            const bool same = std::ranges::all_of(found->second.files, [&](const auto& file) {
                auto text = assets->read(file.first, "creature");
                return text && text.value() == file.second;
            });
            if (same) {
                return found->second.recipe;
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> files;
    const FileReader through_assets = [assets,
                                       &files](std::string_view file) -> Result<std::string> {
        auto text = assets->read(file, "creature");
        if (text) {
            files.emplace_back(std::string(file), text.value());
        }
        return text;
    };
    auto bytes = through_assets(path);
    if (!bytes) {
        return bytes.error();
    }
    auto doc = json::parse(bytes.value());
    if (!doc) {
        return doc.error();
    }
    auto recipe = parse_creature(doc.value(), through_assets);
    if (!recipe) {
        return recipe.error();
    }
    auto shared = std::make_shared<const CreatureRecipe>(std::move(recipe).value());
    if (cache != nullptr) {
        cache->recipes[std::string(path)] = {.files = std::move(files), .recipe = shared};
    }
    return std::shared_ptr<const CreatureRecipe>(shared);
}

std::string creature_bone_id(std::string_view owner, std::string_view bone) {
    std::string id(owner);
    id += '_';
    id += bone;
    if (id.size() > 64) {
        id.resize(64);
    }
    return id;
}

namespace detail {

void prepare_creature_cutouts(World& world) {
    if (world.ecs().ctx().find<CreatureCacheHolder>() == nullptr) {
        world.ecs().ctx().emplace<CreatureCacheHolder>(std::make_shared<CreatureCache>());
    }
    if (world.ecs().ctx().find<CreatureCutoutsHolder>() != nullptr) {
        return;
    }
    world.ecs().ctx().emplace<CreatureCutoutsHolder>(std::make_shared<CreatureCutouts>());
    world.add_reset([](World& w) {
        if (auto* held = w.ecs().ctx().find<CreatureCutoutsHolder>(); held != nullptr) {
            (*held)->built.clear();
            (*held)->warned.clear();
        }
    });
}

void keep_creature_cutouts(World& world) {
    auto* held = world.ecs().ctx().find<CreatureCutoutsHolder>();
    if (held == nullptr) {
        return;
    }
    CreatureCutouts& state = **held;
    // Storage order, so parts are made in the same order every run.
    std::vector<entt::entity> creatures;
    for (const entt::entity entity : world.ecs().view<const Creature2D>()) {
        creatures.push_back(entity);
    }
    std::ranges::reverse(creatures);
    for (const entt::entity entity : creatures) {
        run_creature(world, entity, state);
    }
    std::erase_if(state.built, [&](const auto& e) {
        return !world.valid(e.first) || !world.ecs().all_of<Creature2D>(e.first);
    });
    std::erase_if(state.warned, [&](const auto& e) { return !world.valid(e.first); });
}

} // namespace detail

} // namespace slash::world
