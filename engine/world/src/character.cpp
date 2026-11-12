// Character recipes: their files, their measures and garments' coverage, which 3D builds and 2D
// cut-outs share, and the 2D cut-out with the system that keeps it.
#include "slash/world/character.hpp"

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
#include <optional>
#include <set>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine character",
  "description": "A person built from a recipe, after Unreal's MetaHuman Creator and Mutable: their body, face, hair, clothing and props. A MeshInstance3D draws it as a rigged humanoid model; a Character2D as a cut-out of Polygon2Ds. Colors are [r, g, b] or [r, g, b, a] from 0 to 1.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 3, "maxItems": 4},
    "scale": {"type": "number", "minimum": 0.5, "maximum": 1.5},
    "share": {"type": "number", "minimum": 0, "maximum": 1}
  },
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "extends": {"type": "string", "pattern": "\\.character\\.json$", "description": "A .character.json file this one changes, as a JSON Merge Patch: objects merge, other values (clothing and props included) replace."},
    "name": {"type": "string", "description": "Who it is, for checks and reports."},
    "role": {"enum": ["player", "npc"], "description": "Default npc."},
    "faction": {"type": "string", "description": "A storyboard faction's ID (faction/...)."},
    "body": {
      "type": "object", "additionalProperties": false,
      "description": "The build. Proportions are 1 for an average adult's.",
      "properties": {
        "height": {"type": "number", "minimum": 0.4, "maximum": 3, "description": "Meters to the top of the head. Default 1.75."},
        "build": {"$ref": "#/$defs/share", "description": "0 slight to 1 heavy. Default 0.5."},
        "head": {"$ref": "#/$defs/scale", "description": "The head's size. Default 1."},
        "neck": {"$ref": "#/$defs/scale", "description": "The neck's length. Default 1."},
        "shoulders": {"$ref": "#/$defs/scale", "description": "The shoulders' width. Default 1."},
        "hips": {"$ref": "#/$defs/scale", "description": "The hips' width. Default 1."},
        "arms": {"$ref": "#/$defs/scale", "description": "The arms' length. Default 1."},
        "legs": {"$ref": "#/$defs/scale", "description": "The legs' length. Default 1."},
        "skin": {"$ref": "#/$defs/color", "description": "Default [0.87, 0.68, 0.55]."}
      }
    },
    "face": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "eyes": {"$ref": "#/$defs/color", "description": "The irises. Default [0.3, 0.22, 0.12]."},
        "eye_size": {"$ref": "#/$defs/scale", "description": "Default 1."},
        "eye_spacing": {"$ref": "#/$defs/scale", "description": "Default 1."},
        "nose": {"$ref": "#/$defs/scale", "description": "The nose's size. Default 1."},
        "ears": {"$ref": "#/$defs/scale", "description": "The ears' size. Default 1."},
        "lips": {"$ref": "#/$defs/color", "description": "Default [0.7, 0.42, 0.38]."},
        "beard": {"enum": ["none", "stubble", "short", "long"], "description": "In the hair's color. Default none."}
      }
    },
    "hair": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "style": {"enum": ["none", "short", "long", "ponytail", "bun", "mohawk"], "description": "Default short."},
        "color": {"$ref": "#/$defs/color", "description": "Default [0.18, 0.12, 0.08]."},
        "length": {"$ref": "#/$defs/share", "description": "How far long hair and ponytails hang, 0 to 1. Default 0.5."},
        "groom": {"type": "string", "pattern": "\\.groom\\.json$", "description": "A .groom.json file grown over the scalp (the model's \"scalp\" section, which a HairInstance3D names) in place of the style's shape. 3D only; 2D draws the style."}
      }
    },
    "clothing": {
      "type": "array", "maxItems": 16,
      "description": "What it wears, from the skin out: each later garment over the ones before.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["kind"],
        "properties": {
          "kind": {"enum": ["shirt", "jacket", "coat", "robe", "dress", "skirt", "pants", "shorts", "boots", "shoes", "gloves", "hat", "hood", "helmet", "cape", "belt", "armor", "scarf"], "description": "What it is, which says what it covers."},
          "color": {"$ref": "#/$defs/color", "description": "Default [0.5, 0.5, 0.5]."},
          "material": {"enum": ["cloth", "leather", "metal", "silk", "fur"], "description": "How it shines. Default cloth."},
          "length": {"$ref": "#/$defs/share", "description": "How far down it reaches: sleeves, legs, hems, boots' shafts. Default 1."},
          "loose": {"$ref": "#/$defs/share", "description": "How far it stands off the body. Default 0."},
          "cloth": {"type": "boolean", "description": "Capes, coats, robes, dresses and skirts: simulated as cloth on the garment's section (its kind's name). Default false."}
        }
      }
    },
    "props": {
      "type": "array", "maxItems": 8,
      "description": "Models held or worn at the character's points, which are also the model's attachment points.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["model", "point"],
        "properties": {
          "model": {"type": "string", "description": "A model file: glTF, OBJ, FBX or a .model.json recipe."},
          "point": {"enum": ["right_hand", "left_hand", "back", "hip_left", "hip_right", "head_top"], "description": "Where it is held or worn."},
          "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": "Meters from the point. Default [0, 0, 0]."},
          "rotation": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": "Radians, Godot's YXZ Euler angles. Default [0, 0, 0]."},
          "scale": {"type": "number", "exclusiveMinimum": 0, "description": "Default 1."}
        }
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

void read(const json::Value& doc, std::string_view key, Vec3& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        const json::Array& items = value->as_array();
        out = {static_cast<real_t>(items[0].as_double()), static_cast<real_t>(items[1].as_double()),
               static_cast<real_t>(items[2].as_double())};
    }
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
        return make_error(ErrorCode::InvalidArgument, "characters extend others at most " +
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
    if (const auto errors = validate_character(parsed.value()); !errors.empty()) {
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

// Which garments share a place on the body, so two of them can't both be worn.
std::string_view slot_of(std::string_view kind) {
    if (kind == "shirt" || kind == "dress") {
        return "top";
    }
    if (kind == "jacket" || kind == "coat" || kind == "robe") {
        return "outer";
    }
    if (kind == "pants" || kind == "shorts" || kind == "skirt") {
        return "legs";
    }
    if (kind == "boots" || kind == "shoes") {
        return "feet";
    }
    if (kind == "hat" || kind == "hood" || kind == "helmet") {
        return "head";
    }
    return kind;
}

Vec4 darker(Vec4 color, float by) {
    return {color.x * by, color.y * by, color.z * by, color.w};
}

// A rounded bar from (0, 0) down to (0, -length), radius wide, as a polygon.
std::vector<Vec2> capsule_points(real_t length, real_t radius, real_t from = 0.0F) {
    std::vector<Vec2> out;
    constexpr int kArc = 6;
    for (int i = 0; i <= kArc; ++i) {
        const auto [c, s] = slash::cos_sin(kPi * static_cast<float>(i) / kArc);
        out.push_back({radius * c, -from + (radius * s)});
    }
    for (int i = 0; i <= kArc; ++i) {
        const auto [c, s] = slash::cos_sin(kPi + (kPi * static_cast<float>(i) / kArc));
        out.push_back({radius * c, -length + (radius * s)});
    }
    return out;
}

// An ellipse about center.
std::vector<Vec2> ellipse_points(Vec2 center, real_t rx, real_t ry, int sides = 16) {
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const auto [c, s] =
            slash::cos_sin(2.0F * kPi * static_cast<float>(i) / static_cast<float>(sides));
        out.push_back({center.x + (rx * c), center.y + (ry * s)});
    }
    return out;
}

// A quad from four corners.
std::vector<Vec2> quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    return {a, b, c, d};
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

// A recipe and the text of each file it was made from (the recipe, then what it extends), so
// it is parsed again only when one of them changes.
struct CacheEntry {
    std::vector<std::pair<std::string, std::string>> files;
    std::shared_ptr<const CharacterRecipe> recipe;
};

struct CharacterCache {
    std::mutex mutex;
    std::map<std::string, CacheEntry, std::less<>> recipes;
};
using CharacterCacheHolder = std::shared_ptr<CharacterCache>;

// What the system last built each Character2D's parts from, so they change only when it does.
struct CutoutState {
    std::map<entt::entity, std::string> built;
    std::map<entt::entity, std::string> warned;
};
using CutoutStateHolder = std::shared_ptr<CutoutState>;

std::string cutout_key(const CharacterRecipe& recipe, const Character2D& character) {
    std::string key =
        std::to_string(character.pixels_per_meter) + (character.flip_h ? "/flip" : "/plain");
    for (const CutoutBone& bone : character_cutout(recipe)) {
        key += "|" + bone.name;
        for (const CutoutShape& shape : bone.shapes) {
            key += ":" + std::to_string(shape.points.size()) + "," + std::to_string(shape.color.x) +
                   "," + std::to_string(shape.color.y) + "," + std::to_string(shape.color.z) + "," +
                   std::to_string(shape.points[0].x) + "," + std::to_string(shape.points[0].y);
        }
    }
    return key;
}

std::string bone_entity_id(std::string_view owner, std::string_view bone) {
    std::string id(owner);
    id += '_';
    id += bone;
    if (id.size() > 64) {
        id.resize(64);
    }
    return id;
}

void run_character(World& world, entt::entity entity, CutoutState& state) {
    auto& ecs = world.ecs();
    const Character2D character = ecs.get<const Character2D>(entity);
    auto recipe = load_character(world, character.recipe);
    if (!recipe) {
        std::string& warned = state.warned[entity];
        if (warned != recipe.error().message) {
            warned = recipe.error().message;
            log::warn("character can't be built",
                      {{"id", std::string(world.id_of(entity))}, {"problem", warned}});
        }
        return;
    }
    state.warned.erase(entity);
    const std::string key = cutout_key(*recipe.value(), character) + "@" + character.recipe;
    if (const auto built = state.built.find(entity);
        built != state.built.end() && built->second == key) {
        return;
    }
    state.built[entity] = key;
    if (!ecs.all_of<Transform2D>(entity)) {
        ecs.emplace<Transform2D>(entity);
    }
    const std::vector<CutoutBone> bones = character_cutout(*recipe.value());
    const std::string owner(world.id_of(entity));
    const real_t ppm = character.pixels_per_meter;
    const real_t facing = character.flip_h ? -1.0F : 1.0F;
    const auto pixels = [&](Vec2 p) { return Vec2{p.x * ppm * facing, -p.y * ppm}; };
    std::vector<entt::entity> made(bones.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const CutoutBone& bone = bones[i];
        const std::string id = bone_entity_id(owner, bone.name);
        entt::entity part = world.find(id);
        const entt::entity parent =
            bone.parent < 0 ? entity : made[static_cast<std::size_t>(bone.parent)];
        if (part == entt::null) {
            auto created = world.create(id);
            if (!created) {
                continue;
            }
            part = created.value();
            Transform2D transform;
            transform.position = pixels(bone.position);
            ecs.emplace<Transform2D>(part, transform);
            ecs.get<Transform2D>(parent).children.push_back(part);
        } else if (!ecs.all_of<Transform2D>(part)) {
            Transform2D transform;
            transform.position = pixels(bone.position);
            ecs.emplace<Transform2D>(part, transform);
        }
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

bool is_character_path(std::string_view path) {
    return path.ends_with(kCharacterSuffix);
}

json::Value character_schema() {
    static const json::Value schema = json::parse(kSchema).value();
    return schema;
}

const std::vector<std::string_view>& garment_kinds() {
    static const std::vector<std::string_view> kinds{
        "shirt", "jacket", "coat", "robe", "dress",  "skirt", "pants", "shorts", "boots",
        "shoes", "gloves", "hat",  "hood", "helmet", "cape",  "belt",  "armor",  "scarf"};
    return kinds;
}

std::vector<json::ValidationError> validate_character(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, character_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* clothing = doc.find("clothing"); clothing != nullptr) {
        std::map<std::string_view, std::size_t> slots;
        std::optional<std::size_t> hanging;
        const json::Array& items = clothing->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string_view kind = items[i].find("kind")->as_string();
            const std::string_view slot = slot_of(kind);
            if (const auto [at, fresh] = slots.emplace(slot, i); !fresh) {
                errors.push_back({"/clothing/" + std::to_string(i) + "/kind",
                                  "garment " + std::to_string(at->second) +
                                      " already covers where a " + std::string(kind) + " goes"});
            }
            if (const json::Value* cloth = items[i].find("cloth");
                cloth != nullptr && cloth->as_bool() && kind != "cape" && kind != "coat" &&
                kind != "robe" && kind != "dress" && kind != "skirt") {
                errors.push_back({"/clothing/" + std::to_string(i) + "/cloth",
                                  "only capes, coats, robes, dresses and skirts hang as cloth"});
            } else if (cloth != nullptr && cloth->as_bool()) {
                // A character's Cloth3D simulates one section.
                if (hanging) {
                    errors.push_back({"/clothing/" + std::to_string(i) + "/cloth",
                                      "garment " + std::to_string(*hanging) +
                                          " already hangs as cloth; one garment may"});
                }
                hanging = i;
            }
        }
    }
    if (const json::Value* props = doc.find("props"); props != nullptr) {
        std::set<std::string_view> points;
        const json::Array& items = props->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!points.insert(items[i].find("point")->as_string()).second) {
                errors.push_back(
                    {"/props/" + std::to_string(i) + "/point", "another prop is at this point"});
            }
        }
    }
    return errors;
}

Result<json::Value> resolve_character(const json::Value& doc, const FileReader& read_file) {
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

Result<CharacterRecipe> parse_character(const json::Value& source, const FileReader& read_file) {
    if (const auto errors = validate_character(source); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    auto resolved = resolve_character(source, read_file);
    if (!resolved) {
        return resolved.error();
    }
    const json::Value& doc = resolved.value();
    if (const auto errors = validate_character(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message + " (with what it extends)");
    }
    CharacterRecipe recipe;
    read(doc, "name", recipe.name);
    read(doc, "role", recipe.role);
    read(doc, "faction", recipe.faction);
    if (const json::Value* body = doc.find("body"); body != nullptr) {
        CharacterBody& b = recipe.body;
        read(*body, "height", b.height);
        read(*body, "build", b.build);
        read(*body, "head", b.head);
        read(*body, "neck", b.neck);
        read(*body, "shoulders", b.shoulders);
        read(*body, "hips", b.hips);
        read(*body, "arms", b.arms);
        read(*body, "legs", b.legs);
        read(*body, "skin", b.skin);
    }
    if (const json::Value* face = doc.find("face"); face != nullptr) {
        CharacterFace& f = recipe.face;
        read(*face, "eyes", f.eyes);
        read(*face, "eye_size", f.eye_size);
        read(*face, "eye_spacing", f.eye_spacing);
        read(*face, "nose", f.nose);
        read(*face, "ears", f.ears);
        read(*face, "lips", f.lips);
        read(*face, "beard", f.beard);
    }
    if (const json::Value* hair = doc.find("hair"); hair != nullptr) {
        read(*hair, "style", recipe.hair.style);
        read(*hair, "color", recipe.hair.color);
        read(*hair, "length", recipe.hair.length);
        read(*hair, "groom", recipe.hair.groom);
    }
    if (const json::Value* clothing = doc.find("clothing"); clothing != nullptr) {
        for (const json::Value& item : clothing->as_array()) {
            Garment garment;
            garment.length = 1.0F;
            read(item, "kind", garment.kind);
            read(item, "color", garment.color);
            read(item, "material", garment.material);
            read(item, "length", garment.length);
            read(item, "loose", garment.loose);
            read(item, "cloth", garment.cloth);
            recipe.clothing.push_back(std::move(garment));
        }
    }
    if (const json::Value* props = doc.find("props"); props != nullptr) {
        for (const json::Value& item : props->as_array()) {
            CharacterProp prop;
            read(item, "model", prop.model);
            read(item, "point", prop.point);
            read(item, "position", prop.position);
            read(item, "rotation", prop.rotation);
            read(item, "scale", prop.scale);
            recipe.props.push_back(std::move(prop));
        }
    }
    return recipe;
}

CharacterMeasures character_measures(const CharacterRecipe& recipe) {
    const CharacterBody& body = recipe.body;
    // An average adult's proportions in units of their height, scaled to the recipe's.
    const float legs = 0.47F * body.legs;
    const float torso = 0.33F;
    const float neck = 0.04F * body.neck;
    const float head = 0.135F * body.head;
    const float units = legs + torso + neck + head;
    const float s = body.height / units;
    const float heavy = 0.75F + (0.6F * body.build);
    CharacterMeasures m;
    m.scale = s;
    m.ankle = legs * 0.06F * s;
    m.knee = m.ankle + (legs * 0.42F * s);
    m.hip = legs * s;
    m.waist = m.hip + (0.06F * s);
    m.chest = m.hip + (0.2F * s);
    m.shoulder = m.hip + (torso * s);
    m.head_base = m.shoulder + (neck * s);
    m.top = body.height;
    m.head_height = head * s;
    m.head_radius = 0.06F * body.head * s;
    m.upper_arm = 0.19F * body.arms * s;
    m.lower_arm = 0.16F * body.arms * s;
    m.hand = 0.09F * body.arms * s;
    m.upper_leg = m.hip - m.knee;
    m.lower_leg = m.knee - m.ankle;
    m.foot_length = 0.15F * s;
    m.shoulder_half_width = 0.11F * body.shoulders * (0.9F + (0.2F * body.build)) * s;
    m.chest_half_width = 0.085F * body.shoulders * heavy * s;
    m.waist_half_width = 0.07F * body.hips * heavy * s;
    m.hip_half_width = 0.09F * body.hips * heavy * s;
    m.leg_offset = 0.055F * body.hips * s;
    m.chest_depth = 0.06F * heavy * s;
    m.hip_depth = 0.055F * heavy * s;
    m.arm_radius = 0.028F * heavy * s;
    m.forearm_radius = 0.024F * heavy * s;
    m.thigh_radius = 0.045F * heavy * s;
    m.calf_radius = 0.032F * heavy * s;
    m.neck_radius = 0.028F * heavy * s;
    return m;
}

std::vector<Covering> garment_coverage(const Garment& garment) {
    const std::string_view kind = garment.kind;
    const float length = std::clamp(garment.length, 0.0F, 1.0F);
    std::vector<Covering> out;
    const auto add = [&](Region region, float from = 0.0F, float to = 1.0F) {
        if (to > from) {
            out.push_back({.region = region, .from = from, .to = to});
        }
    };
    if (kind == "shirt") {
        add(Region::Torso);
        add(Region::UpperArms, 0.0F, std::min(1.0F, 0.3F + (1.4F * length)));
        add(Region::LowerArms, 0.0F, std::clamp((2.0F * length) - 1.0F, 0.0F, 0.95F));
    } else if (kind == "jacket" || kind == "coat" || kind == "robe") {
        add(Region::Torso);
        add(Region::UpperArms);
        add(Region::LowerArms, 0.0F, 0.95F);
        if (kind != "jacket") {
            add(Region::Skirt, 0.0F, std::max(length, 0.05F));
        }
    } else if (kind == "dress") {
        add(Region::Torso);
        add(Region::UpperArms, 0.0F, 0.35F);
        add(Region::Skirt, 0.0F, std::max(length, 0.05F));
    } else if (kind == "skirt") {
        add(Region::Skirt, 0.0F, std::max(length, 0.05F));
    } else if (kind == "pants") {
        add(Region::Pelvis);
        add(Region::UpperLegs);
        add(Region::LowerLegs, 0.0F, length);
    } else if (kind == "shorts") {
        add(Region::Pelvis);
        add(Region::UpperLegs, 0.0F, 0.3F + (0.6F * length));
    } else if (kind == "boots") {
        add(Region::Feet);
        add(Region::LowerLegs, 1.0F - (0.8F * length), 1.0F);
    } else if (kind == "shoes") {
        add(Region::Feet);
    } else if (kind == "gloves") {
        add(Region::Hands);
        add(Region::LowerArms, 1.0F - (0.5F * length), 1.0F);
    } else if (kind == "hat") {
        add(Region::Hat);
    } else if (kind == "hood") {
        add(Region::Hood);
    } else if (kind == "helmet") {
        add(Region::Helmet);
    } else if (kind == "cape") {
        add(Region::Cape, 0.0F, std::max(length, 0.1F));
    } else if (kind == "belt") {
        add(Region::Belt);
    } else if (kind == "armor") {
        add(Region::Plate);
        add(Region::Shoulders);
    } else if (kind == "scarf") {
        add(Region::Scarf);
    }
    return out;
}

float garment_offset(const CharacterRecipe& recipe, std::size_t index) {
    const CharacterMeasures m = character_measures(recipe);
    const Garment& garment = recipe.clothing[index];
    return m.scale * (0.006F + (0.03F * garment.loose) + (0.004F * static_cast<float>(index)));
}

std::vector<CutoutBone> character_cutout(const CharacterRecipe& recipe) {
    const CharacterMeasures m = character_measures(recipe);
    const real_t s = m.scale;
    std::vector<CutoutBone> bones;
    const auto bone = [&](std::string name, int parent, Vec2 position, int z) {
        bones.push_back({.name = std::move(name),
                         .parent = parent,
                         .position = position,
                         .z_index = z,
                         .shapes = {}});
        return static_cast<int>(bones.size()) - 1;
    };
    // Seen from the side, facing +x: the far limbs behind the body, the near ones before it.
    const int pelvis = bone("pelvis", -1, {0.0F, m.hip}, 0);
    const int chest = bone("chest", pelvis, {0.0F, m.waist - m.hip}, 0);
    bone("head", chest, {0.0F, m.shoulder - m.waist}, 0);
    std::array<int, 2> upper_arm{};
    std::array<int, 2> lower_arm{};
    std::array<int, 2> hand{};
    std::array<int, 2> upper_leg{};
    std::array<int, 2> lower_leg{};
    std::array<int, 2> foot{};
    for (const std::size_t side : {std::size_t{0}, std::size_t{1}}) {
        const std::string tag = side == 0 ? "_back" : "_front";
        const int z = side == 0 ? -1 : 1;
        upper_leg[side] = bone("upper_leg" + tag, pelvis, {0.0F, 0.0F}, side == 0 ? -1 : 0);
        lower_leg[side] =
            bone("lower_leg" + tag, upper_leg[side], {0.0F, -m.upper_leg}, side == 0 ? -1 : 0);
        foot[side] = bone("foot" + tag, lower_leg[side], {0.0F, -m.lower_leg}, side == 0 ? -1 : 0);
        upper_arm[side] =
            bone("upper_arm" + tag, chest, {0.0F, m.shoulder - m.waist - (0.025F * s)}, z);
        lower_arm[side] = bone("lower_arm" + tag, upper_arm[side], {0.0F, -m.upper_arm}, z);
        hand[side] = bone("hand" + tag, lower_arm[side], {0.0F, -m.lower_arm}, z);
    }
    ShapeSink sink{bones, {}};
    for (std::size_t i = 0; i < bones.size(); ++i) {
        sink.index[bones[i].name] = static_cast<int>(i);
    }
    const auto name = [&](int index) -> std::string_view {
        return bones[static_cast<std::size_t>(index)].name;
    };
    const Vec4 skin = recipe.body.skin;
    const Vec4 far_skin = darker(skin, 0.82F);
    // The body.
    for (const std::size_t side : {std::size_t{0}, std::size_t{1}}) {
        const Vec4 tone = side == 0 ? far_skin : skin;
        sink.add(name(upper_leg[side]), capsule_points(m.upper_leg, m.thigh_radius), tone);
        sink.add(name(lower_leg[side]), capsule_points(m.lower_leg, m.calf_radius), tone);
        sink.add(name(foot[side]),
                 quad({-m.calf_radius, 0.0F}, {m.foot_length * 0.8F, -m.ankle * 0.4F},
                      {m.foot_length * 0.8F, -m.ankle}, {-m.calf_radius, -m.ankle}),
                 tone);
        sink.add(name(upper_arm[side]), capsule_points(m.upper_arm, m.arm_radius), tone);
        sink.add(name(lower_arm[side]), capsule_points(m.lower_arm, m.forearm_radius), tone);
        sink.add(
            name(hand[side]),
            ellipse_points({0.0F, -m.hand * 0.45F}, m.forearm_radius * 1.1F, m.hand * 0.5F, 10),
            tone);
    }
    sink.add("pelvis",
             quad({-m.hip_depth * 1.1F, 0.04F * s}, {m.hip_depth, 0.04F * s},
                  {m.hip_depth * 0.9F, -0.06F * s}, {-m.hip_depth, -0.06F * s}),
             skin);
    const real_t torso = m.shoulder - m.waist;
    sink.add("chest",
             quad({-m.hip_depth, 0.0F}, {m.hip_depth * 0.95F, 0.0F},
                  {m.chest_depth * 1.15F, torso * 0.7F}, {-m.chest_depth, torso}),
             skin);
    sink.add("chest",
             quad({-m.chest_depth, torso * 0.65F}, {m.chest_depth * 1.15F, torso * 0.65F},
                  {m.chest_depth * 0.6F, torso}, {-m.chest_depth, torso}),
             skin);
    // The head: the neck, the skull and the face, facing +x.
    const real_t neck = m.head_base - m.shoulder;
    const Vec2 face_center{0.01F * s, neck + (m.head_height * 0.5F)};
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height * 0.5F;
    sink.add("head",
             quad({-m.neck_radius, -0.01F * s}, {m.neck_radius, -0.01F * s},
                  {m.neck_radius, neck + (0.02F * s)}, {-m.neck_radius, neck + (0.02F * s)}),
             skin);
    sink.add("head", ellipse_points(face_center, hr, hh, 20), skin);
    const real_t nose = 0.014F * recipe.face.nose * s;
    sink.add("head",
             {{face_center.x + hr * 0.9F, face_center.y + hh * 0.05F},
              {face_center.x + hr + nose, face_center.y - hh * 0.2F},
              {face_center.x + hr * 0.9F, face_center.y - hh * 0.3F}},
             skin);
    sink.add("head",
             ellipse_points({face_center.x - (hr * 0.1F), face_center.y},
                            0.012F * recipe.face.ears * s, 0.022F * recipe.face.ears * s, 10),
             darker(skin, 0.9F));
    const real_t eye = 0.008F * recipe.face.eye_size * s;
    sink.add("head",
             ellipse_points({face_center.x + (hr * 0.62F), face_center.y + (hh * 0.15F)}, eye,
                            eye * 1.2F, 10),
             {0.95F, 0.95F, 0.95F, 1.0F});
    sink.add("head",
             ellipse_points({face_center.x + (hr * 0.68F), face_center.y + (hh * 0.15F)},
                            eye * 0.55F, eye * 0.8F, 8),
             recipe.face.eyes);
    sink.add("head",
             quad({face_center.x + (hr * 0.7F), face_center.y - (hh * 0.5F)},
                  {face_center.x + (hr * 0.96F), face_center.y - (hh * 0.5F)},
                  {face_center.x + (hr * 0.96F), face_center.y - (hh * 0.56F)},
                  {face_center.x + (hr * 0.7F), face_center.y - (hh * 0.56F)}),
             recipe.face.lips);
    const Vec4 hair = recipe.hair.color;
    if (recipe.face.beard != "none") {
        const real_t reach = recipe.face.beard == "long" ? hh * 0.9F : hh * 0.25F;
        const Vec4 tone = recipe.face.beard == "stubble"
                              ? Vec4{(hair.x + skin.x) * 0.5F, (hair.y + skin.y) * 0.5F,
                                     (hair.z + skin.z) * 0.5F, 1.0F}
                              : hair;
        sink.add("head",
                 {{face_center.x - (hr * 0.2F), face_center.y - (hh * 0.15F)},
                  {face_center.x + (hr * 0.95F), face_center.y - (hh * 0.4F)},
                  {face_center.x + (hr * 0.8F), face_center.y - hh - reach},
                  {face_center.x + (hr * 0.1F), face_center.y - (hh * 0.85F)}},
                 tone);
    }
    // The hair's shape.
    const std::string_view style = recipe.hair.style;
    if (style != "none") {
        if (style == "mohawk") {
            sink.add("head",
                     quad({face_center.x - (hr * 0.8F), face_center.y + (hh * 0.6F)},
                          {face_center.x + (hr * 0.5F), face_center.y + (hh * 0.85F)},
                          {face_center.x + (hr * 0.3F), face_center.y + (hh * 1.35F)},
                          {face_center.x - (hr * 0.9F), face_center.y + (hh * 1.1F)}),
                     hair);
        } else {
            std::vector<Vec2> cap;
            for (int i = 0; i <= 12; ++i) {
                const auto [c, s2] =
                    slash::cos_sin((kPi * 0.05F) + (kPi * 1.05F * static_cast<float>(i) / 12.0F));
                cap.push_back({face_center.x + (hr * 1.08F * c), face_center.y + (hh * 1.1F * s2)});
            }
            cap.push_back({face_center.x - (hr * 1.05F), face_center.y - (hh * 0.3F)});
            cap.push_back({face_center.x - (hr * 0.6F), face_center.y + (hh * 0.1F)});
            sink.add("head", std::move(cap), hair);
            const real_t hang = recipe.hair.length * 0.35F * s;
            if (style == "long") {
                sink.add("head",
                         quad({face_center.x - (hr * 1.1F), face_center.y + (hh * 0.4F)},
                              {face_center.x - (hr * 0.4F), face_center.y + (hh * 0.4F)},
                              {face_center.x - (hr * 0.5F), face_center.y - hh - hang},
                              {face_center.x - (hr * 1.15F), face_center.y - hh - hang}),
                         hair);
            } else if (style == "ponytail") {
                std::vector<Vec2> tail = capsule_points(hang + hh, 0.02F * s);
                for (Vec2& point : tail) {
                    point = {point.x + face_center.x - (hr * 1.15F),
                             point.y + face_center.y + (hh * 0.5F)};
                }
                sink.add("head", std::move(tail), hair);
            } else if (style == "bun") {
                sink.add("head",
                         ellipse_points({face_center.x - (hr * 0.9F), face_center.y + (hh * 0.9F)},
                                        hr * 0.45F, hr * 0.45F, 12),
                         hair);
            }
        }
    }
    // The clothing, from the skin out.
    for (std::size_t g = 0; g < recipe.clothing.size(); ++g) {
        const Garment& garment = recipe.clothing[g];
        const real_t out = garment_offset(recipe, g);
        const Vec4 near_color = garment.color;
        const Vec4 far_color = darker(garment.color, 0.82F);
        for (const Covering& cover : garment_coverage(garment)) {
            const auto along = [&](std::string_view part, real_t length, real_t radius,
                                   Vec4 color) {
                std::vector<Vec2> points =
                    capsule_points(length * cover.to, radius + out, length * cover.from);
                sink.add(part, std::move(points), color);
            };
            for (const std::size_t side : {std::size_t{0}, std::size_t{1}}) {
                const Vec4 color = side == 0 ? far_color : near_color;
                switch (cover.region) {
                case Region::UpperArms:
                    along(name(upper_arm[side]), m.upper_arm, m.arm_radius, color);
                    break;
                case Region::LowerArms:
                    along(name(lower_arm[side]), m.lower_arm, m.forearm_radius, color);
                    break;
                case Region::Hands:
                    sink.add(name(hand[side]),
                             ellipse_points({0.0F, -m.hand * 0.45F},
                                            (m.forearm_radius * 1.1F) + out, (m.hand * 0.5F) + out,
                                            10),
                             color);
                    break;
                case Region::UpperLegs:
                    along(name(upper_leg[side]), m.upper_leg, m.thigh_radius, color);
                    break;
                case Region::LowerLegs:
                    along(name(lower_leg[side]), m.lower_leg, m.calf_radius, color);
                    break;
                case Region::Feet:
                    sink.add(name(foot[side]),
                             quad({-m.calf_radius - out, out},
                                  {(m.foot_length * 0.85F) + out, -m.ankle * 0.35F},
                                  {(m.foot_length * 0.85F) + out, -m.ankle - (0.2F * out)},
                                  {-m.calf_radius - out, -m.ankle - (0.2F * out)}),
                             color);
                    break;
                case Region::Shoulders:
                    sink.add(
                        name(upper_arm[side]),
                        ellipse_points({0.0F, 0.0F}, m.arm_radius * 1.6F, m.arm_radius * 1.3F, 12),
                        darker(color, 0.95F));
                    break;
                default:
                    break;
                }
                if (cover.region != Region::UpperArms && cover.region != Region::LowerArms &&
                    cover.region != Region::Hands && cover.region != Region::UpperLegs &&
                    cover.region != Region::LowerLegs && cover.region != Region::Feet &&
                    cover.region != Region::Shoulders) {
                    break; // the rest are on the body, not on a side
                }
            }
            switch (cover.region) {
            case Region::Torso:
                sink.add("chest",
                         quad({-m.hip_depth - out, -0.02F * s},
                              {(m.hip_depth * 0.95F) + out, -0.02F * s},
                              {(m.chest_depth * 1.15F) + out, torso * 0.7F},
                              {-m.chest_depth - out, torso + (0.5F * out)}),
                         near_color);
                sink.add("chest",
                         quad({-m.chest_depth - out, torso * 0.65F},
                              {(m.chest_depth * 1.15F) + out, torso * 0.65F},
                              {(m.chest_depth * 0.6F) + out, torso + (0.5F * out)},
                              {-m.chest_depth - out, torso + (0.5F * out)}),
                         near_color);
                break;
            case Region::Pelvis:
                sink.add("pelvis",
                         quad({(-m.hip_depth * 1.1F) - out, (0.06F * s) + out},
                              {m.hip_depth + out, (0.06F * s) + out},
                              {(m.hip_depth * 0.95F) + out, -0.08F * s},
                              {-m.hip_depth - out, -0.08F * s}),
                         near_color);
                break;
            case Region::Skirt: {
                const real_t hem = (m.hip - m.ankle) * cover.to;
                const real_t flare = (m.hip_depth * 1.2F) + out + (0.1F * s * cover.to);
                sink.add("pelvis",
                         quad({-m.hip_depth - out, 0.06F * s}, {m.hip_depth + out, 0.06F * s},
                              {flare, -hem}, {-flare, -hem}),
                         near_color);
                break;
            }
            case Region::Cape: {
                const real_t hem = (m.shoulder - m.ankle) * cover.to;
                std::vector<Vec2> cape =
                    quad({-m.chest_depth - out, torso}, {-m.chest_depth * 0.2F, torso},
                         {(-m.chest_depth * 1.4F) - (0.05F * s), torso - hem},
                         {(-m.chest_depth * 2.0F) - (0.08F * s), torso - hem});
                // Behind everything else of the chest: drawn first.
                CutoutBone& on = bones[static_cast<std::size_t>(chest)];
                on.shapes.insert(on.shapes.begin(),
                                 {.points = std::move(cape), .color = darker(near_color, 0.85F)});
                break;
            }
            case Region::Belt:
                sink.add("pelvis",
                         quad({-m.hip_depth - out, 0.075F * s}, {m.hip_depth + out, 0.075F * s},
                              {m.hip_depth + out, 0.045F * s}, {-m.hip_depth - out, 0.045F * s}),
                         near_color);
                break;
            case Region::Plate:
                sink.add("chest",
                         quad({-m.chest_depth - out, torso * 0.3F},
                              {(m.chest_depth * 1.3F) + out, torso * 0.3F},
                              {(m.chest_depth * 1.2F) + out, torso}, {-m.chest_depth - out, torso}),
                         near_color);
                break;
            case Region::Hat:
                sink.add("head",
                         quad({face_center.x - (hr * 1.5F), face_center.y + (hh * 0.7F)},
                              {face_center.x + (hr * 1.5F), face_center.y + (hh * 0.7F)},
                              {face_center.x + (hr * 1.5F), face_center.y + (hh * 0.8F)},
                              {face_center.x - (hr * 1.5F), face_center.y + (hh * 0.8F)}),
                         near_color);
                sink.add("head",
                         quad({face_center.x - (hr * 0.95F), face_center.y + (hh * 0.8F)},
                              {face_center.x + (hr * 0.95F), face_center.y + (hh * 0.8F)},
                              {face_center.x + (hr * 0.8F), face_center.y + (hh * 1.5F)},
                              {face_center.x - (hr * 0.8F), face_center.y + (hh * 1.5F)}),
                         near_color);
                break;
            case Region::Hood:
            case Region::Helmet: {
                std::vector<Vec2> cover_points;
                const bool hood = cover.region == Region::Hood;
                for (int i = 0; i <= 14; ++i) {
                    const float from = hood ? 0.15F : 0.0F;
                    const auto [c, s2] = slash::cos_sin(
                        (kPi * from) + (kPi * (1.2F - from) * static_cast<float>(i) / 14.0F));
                    cover_points.push_back({face_center.x + ((hr + out) * 1.15F * c),
                                            face_center.y + ((hh + out) * 1.12F * s2)});
                }
                cover_points.push_back({face_center.x - (hr * 0.4F), face_center.y - hh});
                if (hood) {
                    cover_points.push_back({face_center.x + (hr * 0.3F), face_center.y});
                }
                sink.add("head", std::move(cover_points), near_color);
                break;
            }
            case Region::Scarf:
                sink.add("head",
                         quad({-m.neck_radius - out, 0.0F}, {m.neck_radius + out, 0.0F},
                              {m.neck_radius + out, neck * 0.8F},
                              {-m.neck_radius - out, neck * 0.8F}),
                         near_color);
                break;
            default:
                break;
            }
        }
    }
    return bones;
}

Result<std::shared_ptr<const CharacterRecipe>> load_character(const World& world,
                                                              std::string_view path) {
    if (!is_character_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .character.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    const FileReader& reader = world.file_reader();
    if (assets == nullptr) {
        if (!reader) {
            return make_error(ErrorCode::PlatformFailure,
                              "the world has no files to read characters from");
        }
        auto bytes = reader(path);
        if (!bytes) {
            return bytes.error();
        }
        auto doc = json::parse(bytes.value());
        if (!doc) {
            return doc.error();
        }
        auto recipe = parse_character(doc.value(), reader);
        if (!recipe) {
            return recipe.error();
        }
        return std::shared_ptr<const CharacterRecipe>(
            std::make_shared<const CharacterRecipe>(std::move(recipe).value()));
    }
    // The model type loads .character.json files as models (slash/scene3d/character.hpp), so
    // the recipe is read as a watched file, and parsed again only when a file of it changed.
    const auto* holder = world.ecs().ctx().find<CharacterCacheHolder>();
    CharacterCache* cache = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->recipes.find(path); found != cache->recipes.end()) {
            const bool same = std::ranges::all_of(found->second.files, [&](const auto& file) {
                auto text = assets->read(file.first, "character");
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
        auto text = assets->read(file, "character");
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
    auto recipe = parse_character(doc.value(), through_assets);
    if (!recipe) {
        return recipe.error();
    }
    auto shared = std::make_shared<const CharacterRecipe>(std::move(recipe).value());
    if (cache != nullptr) {
        cache->recipes[std::string(path)] = {.files = std::move(files), .recipe = shared};
    }
    return std::shared_ptr<const CharacterRecipe>(shared);
}

void add_character_system(World& world) {
    detail::prepare_creature_cutouts(world);
    if (world.ecs().ctx().find<CharacterCacheHolder>() == nullptr) {
        world.ecs().ctx().emplace<CharacterCacheHolder>(std::make_shared<CharacterCache>());
    }
    if (world.ecs().ctx().find<CutoutStateHolder>() != nullptr) {
        return;
    }
    auto state = std::make_shared<CutoutState>();
    world.ecs().ctx().emplace<CutoutStateHolder>(state);
    world.add_reset([](World& w) {
        if (auto* held = w.ecs().ctx().find<CutoutStateHolder>(); held != nullptr) {
            (*held)->built.clear();
            (*held)->warned.clear();
        }
    });
    world.add_system(
        "character_2d",
        [state](World& w, double /*seconds*/) {
            std::vector<entt::entity> characters;
            for (const entt::entity entity : w.ecs().view<const Character2D>()) {
                characters.push_back(entity);
            }
            for (const entt::entity entity : characters) {
                run_character(w, entity, *state);
            }
            std::erase_if(state->built, [&](const auto& e) {
                return !w.valid(e.first) || !w.ecs().all_of<Character2D>(e.first);
            });
            std::erase_if(state->warned, [&](const auto& e) { return !w.valid(e.first); });
            detail::keep_creature_cutouts(w);
        },
        {.doc = "Keeps each Character2D's and Creature2D's cut-out: a child per bone with its "
                "Transform2D and a Polygon2D of what its recipe draws there.",
         .priority = 880});
}

Vec3 oklab(Vec4 rgb) {
    const auto linear = [](float c) {
        c = std::clamp(c, 0.0F, 1.0F);
        return c <= 0.04045F ? c / 12.92F : slash::exp(2.4F * slash::ln((c + 0.055F) / 1.055F));
    };
    const float r = linear(rgb.x);
    const float g = linear(rgb.y);
    const float b = linear(rgb.z);
    const auto cube_root = [](float v) {
        return v <= 0.0F ? 0.0F : slash::exp(slash::ln(v) / 3.0F);
    };
    const float l = cube_root((0.4122214708F * r) + (0.5363325363F * g) + (0.0514459929F * b));
    const float m = cube_root((0.2119034982F * r) + (0.6806995451F * g) + (0.1073969566F * b));
    const float s = cube_root((0.0883024619F * r) + (0.2817188376F * g) + (0.6299787005F * b));
    return {(0.2104542553F * l) + (0.7936177850F * m) - (0.0040720468F * s),
            (1.9779984951F * l) - (2.4285922050F * m) + (0.4505937099F * s),
            (0.0259040371F * l) + (0.7827717662F * m) - (0.8086757660F * s)};
}

float color_difference(Vec4 a, Vec4 b) {
    const Vec3 x = oklab(a);
    const Vec3 y = oklab(b);
    const Vec3f d = to_float(x - y);
    return 100.0F * std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

} // namespace slash::world
