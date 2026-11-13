// Face recipes and the sculpted head both dimensions draw: a grid round the unit sphere, denser
// over the face, each vertex moved out or in by the face's features (soft bumps and grooves at
// its landmarks, sized by the controls), the eyelids wrapped over the eyeballs with the eyes'
// openings tucked behind them, and the lips' seam tucked into the mouth so the jaw opens it.
// Built with -ffp-contract=off and slash::exp, so heads are the same bits everywhere.
#include "slash/world/face.hpp"

#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace slash::world {

namespace {

constexpr real_t kPi = std::numbers::pi_v<real_t>;
constexpr int kMaxExtends = 8;

const std::vector<FaceControl> kControls = {
    {"head.width", "A wider skull."},
    {"head.length", "A longer skull, front to back."},
    {"head.height", "A taller crown."},
    {"forehead.height", "A taller forehead."},
    {"forehead.slope", "A forehead sloping back."},
    {"brow.height", "Brows set higher."},
    {"brow.ridge", "A heavier brow ridge."},
    {"brow.tilt", "Brows rising toward their outer ends."},
    {"eyes.size", "Bigger eyes."},
    {"eyes.spacing", "Eyes set further apart."},
    {"eyes.height", "Eyes set higher."},
    {"eyes.depth", "Deeper set eyes."},
    {"eyes.tilt", "Eyes whose outer corners rise."},
    {"eyes.openness", "Eyes opened wider."},
    {"eyes.hooded", "Hooded upper lids."},
    {"nose.length", "A longer nose."},
    {"nose.width", "A wider nose."},
    {"nose.bridge", "A higher bridge."},
    {"nose.tip", "A fuller tip."},
    {"nose.upturn", "A tip turned up."},
    {"nose.nostrils", "Flared nostrils."},
    {"nose.hook", "A hooked bridge."},
    {"cheeks.bones", "Higher, sharper cheekbones."},
    {"cheeks.fullness", "Fuller cheeks."},
    {"cheeks.hollow", "Hollow cheeks."},
    {"mouth.width", "A wider mouth."},
    {"mouth.height", "A mouth set higher."},
    {"mouth.corners", "Corners turned up (a resting smile); -1 turns them down."},
    {"lips.upper", "A fuller upper lip."},
    {"lips.lower", "A fuller lower lip."},
    {"lips.protrusion", "Lips standing further out."},
    {"philtrum.depth", "A deeper groove above the upper lip."},
    {"jaw.width", "A wider jaw."},
    {"jaw.angle", "Sharper corners of the jaw."},
    {"chin.length", "A longer chin."},
    {"chin.width", "A wider chin."},
    {"chin.protrusion", "A chin standing further forward."},
    {"chin.cleft", "A cleft chin."},
    {"ears.size", "Bigger ears."},
    {"ears.angle", "Ears standing out from the head."},
    {"ears.height", "Ears set higher."},
    {"face.length", "A longer face below the eyes."},
    {"face.width", "A wider middle face."},
    {"temples", "Hollower temples."},
    {"definition", "Angular, defined features; -1 softens them all."},
    {"neck.width", "A thicker neck where it meets the head."},
};

const std::vector<std::string_view> kLandmarks = {
    "crown",         "forehead",          "glabella",
    "brow_left",     "brow_right",        "eye_left",
    "eye_right",     "temple_left",       "temple_right",
    "nose_bridge",   "nose_tip",          "nostril_left",
    "nostril_right", "cheekbone_left",    "cheekbone_right",
    "cheek_left",    "cheek_right",       "upper_lip",
    "lower_lip",     "mouth_corner_left", "mouth_corner_right",
    "chin",          "jaw_left",          "jaw_right",
    "ear_left",      "ear_right",         "back",
};

const std::vector<std::string_view> kExpressions = {
    "eyeBlinkLeft",      "eyeLookDownLeft",    "eyeLookInLeft",
    "eyeLookOutLeft",    "eyeLookUpLeft",      "eyeSquintLeft",
    "eyeWideLeft",       "eyeBlinkRight",      "eyeLookDownRight",
    "eyeLookInRight",    "eyeLookOutRight",    "eyeLookUpRight",
    "eyeSquintRight",    "eyeWideRight",       "jawForward",
    "jawLeft",           "jawRight",           "jawOpen",
    "mouthClose",        "mouthFunnel",        "mouthPucker",
    "mouthLeft",         "mouthRight",         "mouthSmileLeft",
    "mouthSmileRight",   "mouthFrownLeft",     "mouthFrownRight",
    "mouthDimpleLeft",   "mouthDimpleRight",   "mouthStretchLeft",
    "mouthStretchRight", "mouthRollLower",     "mouthRollUpper",
    "mouthShrugLower",   "mouthShrugUpper",    "mouthPressLeft",
    "mouthPressRight",   "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthUpperUpLeft",  "mouthUpperUpRight",  "browDownLeft",
    "browDownRight",     "browInnerUp",        "browOuterUpLeft",
    "browOuterUpRight",  "cheekPuff",          "cheekSquintLeft",
    "cheekSquintRight",  "noseSneerLeft",      "noseSneerRight",
    "tongueOut",
};

std::string face_fields() {
    std::string controls;
    for (const FaceControl& control : kControls) {
        if (!controls.empty()) {
            controls += ",\n";
        }
        controls += R"(        ")" + std::string(control.name) +
                    R"(": {"type": "number", "minimum": -1, "maximum": 1, "description": ")" +
                    std::string(control.doc) + R"("})";
    }
    std::string landmarks;
    for (const std::string_view name : kLandmarks) {
        landmarks += (landmarks.empty() ? "\"" : ", \"") + std::string(name) + "\"";
    }
    return R"(
    "eyes": {"$ref": "#/$defs/color", "description": "The irises. Default [0.3, 0.22, 0.12]."},
    "eye_size": {"$ref": "#/$defs/scale", "description": "Default 1."},
    "eye_spacing": {"$ref": "#/$defs/scale", "description": "Default 1."},
    "nose": {"$ref": "#/$defs/scale", "description": "The nose's size. Default 1."},
    "ears": {"$ref": "#/$defs/scale", "description": "The ears' size. Default 1."},
    "lips": {"$ref": "#/$defs/color", "description": "Default [0.7, 0.42, 0.38]."},
    "beard": {"enum": ["none", "stubble", "short", "long"], "description": "In the hair's color, grown over the sculpted jaw and lip. Default none."},
    "style": {"enum": ["realistic", "stylized", "toon", "painterly", "low_poly"], "description": "How the head is sculpted: realistic (fine detail), stylized and painterly (features exaggerated, smoother), toon (big eyes, small nose, little detail) or low_poly (a few flat-shaded faces). Default realistic."},
    "controls": {
      "type": "object", "additionalProperties": false,
      "description": "Sculpt controls, each -1 to 1; 0 (the default) is an average adult's.",
      "properties": {
)" + controls +
           R"(
      }
    },
    "carves": {
      "type": "array", "maxItems": 32,
      "description": "Carves into the sculpt at its landmarks: dents, bumps, creases and ridges.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["kind", "at"],
        "properties": {
          "kind": {"enum": ["dent", "bump", "crease", "ridge"], "description": "A dent or bump round a point, or a crease or ridge along a line."},
          "at": {"enum": [)" +
           landmarks + R"(], "description": "The landmark it is carved at."},
          "offset": {"type": "array", "items": {"type": "number", "minimum": -1, "maximum": 1}, "minItems": 2, "maxItems": 2, "description": "Moved from the landmark across (toward the face's left) and up, in half the head's width and height. Default [0, 0]."},
          "radius": {"type": "number", "exclusiveMinimum": 0, "maximum": 0.5, "description": "Its size, in half the head's width. Default 0.06."},
          "depth": {"type": "number", "minimum": -1, "maximum": 1, "description": "How deep (or, for bumps and ridges, how high): 1 is 5% of the head's depth. Default 0.5."},
          "length": {"type": "number", "minimum": 0, "maximum": 1, "description": "A crease's or ridge's length, in half the head's width. Default 0."},
          "angle": {"type": "number", "description": "A crease's or ridge's direction, degrees from across. Default 0."},
          "mirror": {"type": "boolean", "description": "Carve the other side the same. Default false."}
        }
      }
    },
    "skin": {
      "type": "object", "additionalProperties": false,
      "description": "The skin's own detail in a 3D head's maps, each 0 to 1. Unset ones are the style's: realistic heads have pores, blotches and oil, stylized ones a little, toon, painterly and low_poly ones none.",
      "properties": {
        "pores": {"$ref": "#/$defs/share", "description": "Pores' fine relief, most over the nose and cheeks."},
        "wrinkles": {"$ref": "#/$defs/share", "description": "Lines across the forehead, crow's feet, frown lines and the folds from the nose and under the eyes. Default 0."},
        "blotches": {"$ref": "#/$defs/share", "description": "Uneven color: flush on the cheeks, nose and ears, and blotches."},
        "oil": {"$ref": "#/$defs/share", "description": "A shine over the forehead, nose and chin."},
        "resolution": {"enum": [256, 512, 1024, 2048], "description": "The maps' width; their height is half. Default the style's: 1024 realistic, 512 stylized and painterly, 256 toon and low_poly."}
      }
    },
    "marks": {
      "type": "array", "maxItems": 64,
      "description": "Marks on the skin, painted into a 3D head's skin maps and drawn over a 2D profile where it shows.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["kind", "at"],
        "properties": {
          "kind": {"enum": ["mole", "freckles", "scar", "wound", "burn", "bruise", "rash", "birthmark", "tattoo", "paint"], "description": "What it is: a mole, a scatter of freckles, a scar or wound along a line, a burn, bruise, rash or birthmark over a patch, or a tattoo or face paint in a pattern."},
          "at": {"enum": [)" +
           landmarks + R"(], "description": "The landmark it is at."},
          "offset": {"type": "array", "items": {"type": "number", "minimum": -1, "maximum": 1}, "minItems": 2, "maxItems": 2, "description": "Moved from the landmark across (toward the face's left) and up, in half the head's width and height. Default [0, 0]."},
          "size": {"type": "number", "minimum": 0, "maximum": 1, "description": "How big, in half the head's width: a mole's or freckles' patch's radius, a scar's width, a pattern's size. Default the kind's own."},
          "length": {"type": "number", "minimum": 0, "maximum": 1, "description": "How long a scar, wound or a pattern that runs is, in half the head's width. Default the kind's own."},
          "angle": {"type": "number", "description": "Its direction, degrees from across toward up. Default 0."},
          "color": {"$ref": "#/$defs/color", "description": "Default the kind's own: a mole's brown, a scar's pale pink, a tattoo's ink, paint's red."},
          "amount": {"$ref": "#/$defs/share", "description": "How strong: its opacity, a freckles' patch's density, a scar's or wound's depth. Default 1."},
          "age": {"$ref": "#/$defs/share", "description": "0 fresh to 1 old: a wound scabs over, a bruise turns from purple to green and yellow, a scar and a burn pale and a tattoo blurs. Default 0."},
          "pattern": {"enum": ["", "stitched", "stripes", "chevron", "dots", "ring", "star", "spiral", "teardrop", "cross", "lines", "band", "mask", "half"], "description": "A tattoo's or paint's shape (default lines for tattoos, band for paint), or stitched for a scar or wound's stitches."},
          "mirror": {"type": "boolean", "description": "The same mark on the other side too. Default false."},
          "seed": {"type": "integer", "minimum": 0, "maximum": 4294967295, "description": "Picks where freckles fall and how a patch's edge runs. Default 0."}
        }
      }
    },
    "brows": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "color": {"$ref": "#/$defs/color", "description": "Default the hair's color, darkened."},
        "thickness": {"$ref": "#/$defs/share", "description": "How thick, 0 for none. Default 0.5."},
        "shape": {"enum": ["soft", "straight", "arched", "angled"], "description": "Default soft."}
      }
    },
    "asymmetry": {"$ref": "#/$defs/share", "description": "How far the face's sides differ, 0 to 1, as seed picks: eyes, brows, nose and cheeks a little off. Default 0."},
    "seed": {"type": "integer", "minimum": 0, "maximum": 4294967295, "description": "Picks the asymmetry. Default 0."},
    "blend": {
      "type": "array", "maxItems": 8,
      "description": "Faces whose sculpt controls mix by weight under this face's own, which add to them, as MetaHuman Creator blends presets.",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["face", "weight"],
        "properties": {
          "face": {"type": "string", "pattern": "\\.face\\.json$", "description": "A .face.json file."},
          "weight": {"type": "number", "minimum": 0, "maximum": 1, "description": "Its share; shares summing over 1 are divided by their sum."}
        }
      }
    })";
}

constexpr std::string_view kDefs = R"(
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 3, "maxItems": 4},
    "scale": {"type": "number", "minimum": 0.5, "maximum": 1.5},
    "share": {"type": "number", "minimum": 0, "maximum": 1}
  })";

const json::Value& file_schema() {
    static const json::Value schema =
        json::parse(R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine face",
  "description": "A face, after Unreal's MetaHuman Creator: sculpt controls, carves at landmarks, blends of other faces, its eyes, lips, brows and beard and the style it is sculpted in. A character recipe's face names one with from.",
  "type": "object", "additionalProperties": false,
  "required": ["format"],
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "name": {"type": "string", "description": "What it is called."},
    "extends": {"type": "string", "pattern": "\\.face\\.json$", "description": "A .face.json file this one is a merge patch over."},)" +
                    face_fields() + "\n  },\n" + std::string(kDefs.substr(1)) + "\n}")
            .value();
    return schema;
}

const json::Value& inline_schema() {
    static const json::Value schema =
        json::parse(R"({
  "type": "object", "additionalProperties": false,
  "description": "The face: a .face.json file's fields, as a merge patch over the file from names.",
  "properties": {
    "from": {"type": "string", "pattern": "\\.face\\.json$", "description": "A .face.json file these fields are a merge patch over."},)" +
                    face_fields() + "\n  },\n" + std::string(kDefs.substr(1)) + "\n}")
            .value();
    return schema;
}

void read(const json::Value& doc, std::string_view key, float& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<float>(value->as_double());
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

Result<json::Value> read_face_file(const std::string& path, const FileReader& read_file) {
    if (!read_file) {
        return make_error(ErrorCode::InvalidArgument,
                          "a face needs the project's files to read \"" + path + "\"");
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
    if (const auto errors = validate_face(parsed.value()); !errors.empty()) {
        return make_error(
            ErrorCode::InvalidArgument,
            "\"" + path + "\": " + (errors.front().path.empty() ? "root" : errors.front().path) +
                ": " + errors.front().message);
    }
    return std::move(parsed).value();
}

// The JSON a face's "extends" (or "from") chain merges to, without it.
Result<json::Value> resolve(const json::Value& doc, const FileReader& read_file,
                            std::vector<std::string>& chain) {
    const json::Value* parent_path = doc.find("extends");
    std::string_view key = "extends";
    if (parent_path == nullptr) {
        parent_path = doc.find("from");
        key = "from";
    }
    if (parent_path == nullptr) {
        return doc;
    }
    const std::string& path = parent_path->as_string();
    if (std::ranges::find(chain, path) != chain.end()) {
        return make_error(ErrorCode::InvalidArgument, "\"" + path + "\" extends itself");
    }
    if (chain.size() >= kMaxExtends) {
        return make_error(ErrorCode::InvalidArgument,
                          "faces extend others at most " + std::to_string(kMaxExtends) + " deep");
    }
    auto parsed = read_face_file(path, read_file);
    if (!parsed) {
        return parsed.error();
    }
    chain.push_back(path);
    auto parent = resolve(parsed.value(), read_file, chain);
    if (!parent) {
        return parent.error();
    }
    json::Value patch = doc;
    patch.as_object().erase(std::string(key));
    return merge_patch(parent.value(), patch);
}

Result<CharacterFace> parse_resolved(const json::Value& source, const FileReader& read_file,
                                     std::vector<std::string>& chain) {
    auto resolved = resolve(source, read_file, chain);
    if (!resolved) {
        return resolved.error();
    }
    const json::Value& doc = resolved.value();
    CharacterFace face;
    read(doc, "eyes", face.eyes);
    read(doc, "eye_size", face.eye_size);
    read(doc, "eye_spacing", face.eye_spacing);
    read(doc, "nose", face.nose);
    read(doc, "ears", face.ears);
    read(doc, "lips", face.lips);
    read(doc, "beard", face.beard);
    read(doc, "style", face.style);
    read(doc, "asymmetry", face.asymmetry);
    if (const json::Value* seed = doc.find("seed"); seed != nullptr) {
        face.seed = static_cast<std::uint32_t>(seed->as_double());
    }
    face.controls.assign(kControls.size(), 0.0F);
    if (const json::Value* controls = doc.find("controls"); controls != nullptr) {
        for (const auto& [name, value] : controls->as_object()) {
            if (const auto index = face_control_index(name)) {
                face.controls[*index] = static_cast<float>(value.as_double());
            }
        }
    }
    if (const json::Value* carves = doc.find("carves"); carves != nullptr) {
        face.carves.reserve(carves->as_array().size());
        for (const json::Value& item : carves->as_array()) {
            FaceCarve carve;
            read(item, "kind", carve.kind);
            read(item, "at", carve.at);
            if (const json::Value* offset = item.find("offset"); offset != nullptr) {
                carve.offset = {static_cast<real_t>(offset->as_array()[0].as_double()),
                                static_cast<real_t>(offset->as_array()[1].as_double())};
            }
            read(item, "radius", carve.radius);
            read(item, "depth", carve.depth);
            read(item, "length", carve.length);
            read(item, "angle", carve.angle);
            if (const json::Value* mirror = item.find("mirror"); mirror != nullptr) {
                carve.mirror = mirror->as_bool();
            }
            face.carves.push_back(std::move(carve));
        }
    }
    if (const json::Value* skin = doc.find("skin"); skin != nullptr) {
        read(*skin, "pores", face.skin.pores);
        read(*skin, "wrinkles", face.skin.wrinkles);
        read(*skin, "blotches", face.skin.blotches);
        read(*skin, "oil", face.skin.oil);
        if (const json::Value* resolution = skin->find("resolution"); resolution != nullptr) {
            face.skin.resolution = static_cast<std::uint32_t>(resolution->as_double());
        }
    }
    if (const json::Value* marks = doc.find("marks"); marks != nullptr) {
        face.marks.reserve(marks->as_array().size());
        for (const json::Value& item : marks->as_array()) {
            FaceMark mark;
            read(item, "kind", mark.kind);
            read(item, "at", mark.at);
            if (const json::Value* offset = item.find("offset"); offset != nullptr) {
                mark.offset = {static_cast<real_t>(offset->as_array()[0].as_double()),
                               static_cast<real_t>(offset->as_array()[1].as_double())};
            }
            read(item, "size", mark.size);
            read(item, "length", mark.length);
            read(item, "angle", mark.angle);
            read(item, "color", mark.color);
            read(item, "amount", mark.amount);
            read(item, "age", mark.age);
            read(item, "pattern", mark.pattern);
            if (const json::Value* mirror = item.find("mirror"); mirror != nullptr) {
                mark.mirror = mirror->as_bool();
            }
            if (const json::Value* seed = item.find("seed"); seed != nullptr) {
                mark.seed = static_cast<std::uint32_t>(seed->as_double());
            }
            face.marks.push_back(std::move(mark));
        }
    }
    if (const json::Value* brows = doc.find("brows"); brows != nullptr) {
        read(*brows, "color", face.brows.color);
        read(*brows, "thickness", face.brows.thickness);
        read(*brows, "shape", face.brows.shape);
    }
    // Presets' controls mixed by weight under the face's own.
    if (const json::Value* blend = doc.find("blend"); blend != nullptr) {
        std::vector<float> mixed(kControls.size(), 0.0F);
        float total = 0.0F;
        for (const json::Value& item : blend->as_array()) {
            const std::string& path = item.find("face")->as_string();
            const auto weight = static_cast<float>(item.find("weight")->as_double());
            if (std::ranges::find(chain, path) != chain.end()) {
                return make_error(ErrorCode::InvalidArgument, "\"" + path + "\" blends itself in");
            }
            auto preset_doc = read_face_file(path, read_file);
            if (!preset_doc) {
                return preset_doc.error();
            }
            std::vector<std::string> inner = chain;
            inner.push_back(path);
            auto preset = parse_resolved(preset_doc.value(), read_file, inner);
            if (!preset) {
                return preset.error();
            }
            for (std::size_t i = 0; i < mixed.size(); ++i) {
                mixed[i] += weight * preset.value().control(i);
            }
            total += weight;
        }
        const float divide = total > 1.0F ? 1.0F / total : 1.0F;
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            face.controls[i] = std::clamp(face.controls[i] + (mixed[i] * divide), -1.0F, 1.0F);
        }
    }
    return face;
}

// --- Sculpting --------------------------------------------------------------------------------

real_t clamp01(real_t x) {
    return std::clamp<real_t>(x, 0.0F, 1.0F);
}

// A smooth step from 0 at a to 1 at b (b may be below a).
real_t smooth(real_t a, real_t b, real_t x) {
    const real_t t = clamp01((x - a) / (b - a));
    return t * t * (3.0F - (2.0F * t));
}

real_t gauss(real_t q) {
    return q > 30.0F ? 0.0F : static_cast<real_t>(slash::exp(-to_float(q)));
}

real_t length3(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

Vec3 unit(Vec3 v) {
    const real_t l = length3(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{0.0F, 0.0F, 1.0F};
}

Vec3 cross3(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t dot3(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t asin_of(real_t y) {
    return static_cast<real_t>(
        slash::atan2(to_float(y), to_float(std::sqrt(std::max<real_t>(1.0F - (y * y), 0.0F)))));
}

// A point on the unit sphere's front at x and y.
Vec3 front_at(real_t x, real_t y) {
    return {x, y, std::sqrt(std::max<real_t>(1.0F - (x * x) - (y * y), 0.0F))};
}

// p turned by angle about the line through pivot along x: a positive angle takes what is in front
// of the pivot down.
Vec3 turned_down(Vec3 p, Vec3 pivot, real_t angle) {
    const Vec2f turn = cos_sin(to_float(angle));
    const real_t c = turn.x;
    const real_t s = turn.y;
    const real_t y = p.y - pivot.y;
    const real_t z = p.z - pivot.z;
    return {p.x, pivot.y + (y * c) - (z * s), pivot.z + (y * s) + (z * c)};
}

// A small whole-number hash, for the asymmetry's picks: -1 to 1.
real_t pick(std::uint32_t seed, std::uint32_t salt) {
    std::uint32_t h = seed ^ (salt * 0x9E3779B9U);
    h ^= h >> 16U;
    h *= 0x7FEB352DU;
    h ^= h >> 15U;
    h *= 0x846CA68BU;
    h ^= h >> 16U;
    return (static_cast<real_t>(h & 0xFFFFU) / 32767.5F) - 1.0F;
}

struct Style {
    real_t exaggerate = 1.0F;
    real_t eyes = 1.0F;
    real_t nose = 1.0F;
    real_t bones = 1.0F; // brow ridge and cheekbones
    real_t fine = 1.0F;  // small grooves: philtrum, folds, under the eyes
    std::size_t rings = 80;
    std::size_t segments = 112;
    bool flat = false;
    bool tucks = true; // eyelids wrapped and the lips' seam tucked in
};

Style style_of(std::string_view name) {
    if (name == "stylized") {
        return {.exaggerate = 1.35F,
                .eyes = 1.15F,
                .nose = 0.9F,
                .bones = 0.9F,
                .fine = 0.5F,
                .rings = 64,
                .segments = 88,
                .flat = false,
                .tucks = true};
    }
    if (name == "painterly") {
        return {.exaggerate = 1.2F,
                .eyes = 1.08F,
                .nose = 1.0F,
                .bones = 1.0F,
                .fine = 0.7F,
                .rings = 64,
                .segments = 88,
                .flat = false,
                .tucks = true};
    }
    if (name == "toon") {
        return {.exaggerate = 1.25F,
                .eyes = 1.55F,
                .nose = 0.5F,
                .bones = 0.35F,
                .fine = 0.0F,
                .rings = 48,
                .segments = 64,
                .flat = false,
                .tucks = true};
    }
    if (name == "low_poly") {
        return {.exaggerate = 1.0F,
                .eyes = 1.0F,
                .nose = 1.0F,
                .bones = 1.0F,
                .fine = 0.0F,
                .rings = 14,
                .segments = 20,
                .flat = true,
                .tucks = false};
    }
    return {};
}

struct Carve {
    std::string_view kind;
    Vec3 center; // on the unit sphere
    real_t radius = 0.06F;
    real_t amount = 0.0F;
    Vec2 along; // a crease's direction across and up
    real_t half = 0.0F;
};

class Sculptor {
public:
    Sculptor(const CharacterFace& face, const HeadShape& head)
        : face_(face), head_(head), style_(style_of(face.style)) {
        const real_t asym = std::clamp<real_t>(face.asymmetry, 0.0F, 1.0F);
        for (std::size_t s = 0; s < 2; ++s) {
            eye_dy_[s] = asym * 0.025F * pick(face.seed, 1U + static_cast<std::uint32_t>(s));
            brow_dy_[s] = asym * 0.035F * pick(face.seed, 3U + static_cast<std::uint32_t>(s));
            cheek_[s] = 1.0F + (asym * 0.25F * pick(face.seed, 5U + static_cast<std::uint32_t>(s)));
        }
        nose_dx_ = asym * 0.02F * pick(face.seed, 7U);
        mouth_tilt_ = asym * 0.02F * pick(face.seed, 8U);
        // The eyes, as face_eyes places them.
        eye_size_ =
            static_cast<real_t>(face.eye_size) * (1.0F + (0.18F * c("eyes.size"))) * style_.eyes;
        eye_x_ = 0.38F * static_cast<real_t>(face.eye_spacing) *
                 (1.0F + (0.12F * c("eyes.spacing"))) * std::max<real_t>(1.0F, style_.eyes * 0.85F);
        eye_y_ = 0.12F + (0.06F * c("eyes.height"));
        eye_radius_ = head.radii.x * 0.16F * eye_size_;
        for (std::size_t s = 0; s < 2; ++s) {
            const real_t side = s == 0 ? 1.0F : -1.0F;
            const real_t y = eye_y_ + eye_dy_[s];
            eye_center_[s] = {
                head.center.x + (side * eye_x_ * head.radii.x), head.center.y + (y * head.radii.y),
                head.center.z + (head.radii.z * (0.893F - (0.05F * c("eyes.depth")))) -
                    (eye_radius_ * 0.8F)};
            eye_dir_[s] = unit(normalized_of(eye_center_[s]));
        }
        mouth_y_ = -0.46F + (0.06F * c("mouth.height"));
        mouth_w_ = 0.23F * (1.0F + (0.22F * c("mouth.width")));
        nose_top_ = eye_y_ + 0.04F;
        nose_scale_ = static_cast<real_t>(face.nose) * style_.nose;
        nose_tip_ = nose_top_ - ((nose_top_ + 0.2F) * (1.0F + (0.18F * c("nose.length"))) *
                                 std::sqrt(nose_scale_));
        nose_w_ = std::sqrt(nose_scale_) * (1.0F + (0.3F * c("nose.width")));
        chin_y_ = -0.8F - (0.06F * c("chin.length"));
        // Carves, at their landmarks.
        for (const FaceCarve& carve : face.carves) {
            const Vec3 at = landmark_direction(carve.at);
            const Vec2f turn = cos_sin(carve.angle * std::numbers::pi_v<float> / 180.0F);
            for (int copy = 0; copy < (carve.mirror ? 2 : 1); ++copy) {
                const real_t flip = copy == 0 ? 1.0F : -1.0F;
                const real_t x = (at.x * flip) + (carve.offset.x * flip);
                const real_t y = at.y + carve.offset.y;
                const bool raised = carve.kind == "bump" || carve.kind == "ridge";
                Carve made;
                made.kind = carve.kind;
                made.center = at.z >= 0.0F ? front_at(std::clamp<real_t>(x, -0.99F, 0.99F),
                                                      std::clamp<real_t>(y, -0.99F, 0.99F))
                                           : unit({x, y, at.z});
                made.radius = carve.radius;
                made.amount = 0.05F * carve.depth * (raised ? 1.0F : -1.0F);
                made.along = {static_cast<real_t>(turn.x) * flip, static_cast<real_t>(turn.y)};
                made.half = carve.length * 0.5F;
                carves_.push_back(made);
            }
        }
    }

    // A control's value as the style exaggerates it.
    [[nodiscard]] real_t c(std::string_view name) const {
        return std::clamp<real_t>(face_.control(name) * style_.exaggerate, -1.6F, 1.6F);
    }

    [[nodiscard]] const Style& style() const { return style_; }
    [[nodiscard]] real_t mouth_y() const { return mouth_y_; }
    [[nodiscard]] FaceEyes eyes() const {
        return {.left = eye_center_[0], .right = eye_center_[1], .radius = eye_radius_};
    }

    // A point in meters in the head's unit space.
    [[nodiscard]] Vec3 normalized_of(Vec3 p) const {
        return {(p.x - head_.center.x) / head_.radii.x, (p.y - head_.center.y) / head_.radii.y,
                (p.z - head_.center.z) / head_.radii.z};
    }
    [[nodiscard]] Vec3 meters_of(Vec3 q) const {
        return {head_.center.x + (q.x * head_.radii.x), head_.center.y + (q.y * head_.radii.y),
                head_.center.z + (q.z * head_.radii.z)};
    }

    // Where the latitude of the lips' seam is on the unit sphere.
    [[nodiscard]] real_t mouth_latitude() const { return asin_of(mouth_y_); }

    // The direction a landmark is sculpted from.
    [[nodiscard]] Vec3 landmark_direction(std::string_view name) const {
        const real_t brow_y = brow_line(0.4F, 0) + 0.02F;
        const auto side_of = [&](std::string_view n) {
            return n.ends_with("_right") ? -1.0F : 1.0F;
        };
        const real_t side = side_of(name);
        if (name == "crown") {
            return {0.0F, 1.0F, 0.0F};
        }
        if (name == "back") {
            return unit({0.0F, 0.1F, -1.0F});
        }
        if (name == "forehead") {
            return front_at(0.0F, eye_y_ + 0.45F);
        }
        if (name == "glabella") {
            return front_at(0.0F, eye_y_ + 0.15F);
        }
        if (name.starts_with("brow_")) {
            return front_at(side * 0.3F, brow_y);
        }
        if (name.starts_with("eye_")) {
            return eye_dir_[side > 0.0F ? 0 : 1];
        }
        if (name.starts_with("temple_")) {
            return unit({side * 0.82F, eye_y_ + 0.18F, 0.45F});
        }
        if (name == "nose_bridge") {
            return front_at(0.0F, eye_y_);
        }
        if (name == "nose_tip") {
            return front_at(nose_dx_, nose_tip_ + 0.015F);
        }
        if (name.starts_with("nostril_")) {
            return front_at(side * 0.075F * nose_w_, nose_tip_ + 0.005F);
        }
        if (name.starts_with("cheekbone_")) {
            return front_at(side * 0.52F, eye_y_ - 0.17F);
        }
        if (name.starts_with("cheek_")) {
            return front_at(side * 0.45F, mouth_y_ + 0.1F);
        }
        if (name == "upper_lip") {
            return front_at(0.0F, mouth_y_ + 0.03F);
        }
        if (name == "lower_lip") {
            return front_at(0.0F, mouth_y_ - 0.04F);
        }
        if (name.starts_with("mouth_corner_")) {
            return front_at(side * mouth_w_, mouth_y_ + (0.015F * c("mouth.corners")));
        }
        if (name == "chin") {
            return front_at(0.0F, chin_y_);
        }
        if (name.starts_with("jaw_")) {
            return unit({side * 0.7F, -0.6F, 0.35F});
        }
        if (name.starts_with("ear_")) {
            return unit({side, 0.08F * c("ears.height"), -0.05F});
        }
        return {0.0F, 0.0F, 1.0F};
    }

    // The brows' middle line at u (0 inner end to 1 outer) on a side (0 left, 1 right).
    [[nodiscard]] real_t brow_line(real_t u, std::size_t side) const {
        real_t y = eye_y_ + 0.2F + (0.05F * c("brow.height")) + brow_dy_[side] + eye_dy_[side];
        const std::string& shape = face_.brows.shape;
        const real_t arch = cos_sin(to_float(kPi * u)).y;
        if (shape == "arched") {
            y += 0.06F * arch;
        } else if (shape == "angled") {
            y += 0.05F * (u < 0.65F ? u / 0.65F : (1.0F - u) / 0.35F);
        } else if (shape == "soft") {
            y += 0.03F * cos_sin(to_float(kPi * u * 0.9F)).y;
        }
        return y + (0.05F * c("brow.tilt") * (u - 0.5F));
    }

    // The head's shape at a direction, before the eyelids and the mouth: in its unit space.
    [[nodiscard]] Vec3 shape(Vec3 d) const {
        Vec3 q = d;
        const real_t front = smooth(-0.1F, 0.35F, d.z);
        // The lower face narrows toward the chin; the jaw widens it.
        const real_t low = smooth(-0.15F, -0.9F, d.y);
        q.x *= 1.0F - (low * (0.15F - (0.09F * c("jaw.width")))) - (0.1F * low * front);
        q.x *= 1.0F + (0.07F * c("head.width") * smooth(-0.4F, 0.2F, d.y));
        q.x *= 1.0F + (0.05F * c("face.width") *
                       gauss(((d.y + 0.1F) / 0.35F) * ((d.y + 0.1F) / 0.35F)) * front);
        // Where the neck meets the head, it is as wide as neck.width says.
        q.x *= 1.0F + (0.08F * c("neck.width") * smooth(-0.6F, -1.0F, d.y) * (1.0F - front));
        if (d.z < 0.0F) {
            q.z *= 1.0F + (0.08F * c("head.length"));
        }
        if (d.y > 0.0F) {
            q.y *= 1.0F + (0.07F * c("head.height")) +
                   (0.06F * c("forehead.height") * front * smooth(0.2F, 0.8F, d.y));
        }
        if (d.y > 0.25F) {
            q.z -= 0.12F * c("forehead.slope") * (d.y - 0.25F) * front;
        }
        const real_t below_eyes = eye_y_ - 0.02F;
        if (d.y < below_eyes) {
            q.y = below_eyes + ((d.y - below_eyes) * (1.0F + (0.07F * c("face.length"))));
        }
        const real_t below_mouth = mouth_y_ - 0.1F;
        if (d.y < below_mouth) {
            q.y -= (below_mouth - d.y) * 0.06F * c("chin.length") * front;
        }
        return q * (1.0F + displacement(d));
    }

    // How far out (or in) the features move the head at a direction, in its unit space.
    [[nodiscard]] real_t displacement(Vec3 d) const {
        real_t r = 0.0F;
        const real_t def = c("definition");
        const real_t fine = style_.fine;
        const real_t bones = style_.bones;
        const auto blob = [&](Vec3 at, Vec3 sigma) {
            const real_t dx = (d.x - at.x) / sigma.x;
            const real_t dy = (d.y - at.y) / sigma.y;
            const real_t dz = (d.z - at.z) / sigma.z;
            return gauss((dx * dx) + (dy * dy) + (dz * dz));
        };
        const auto front_blob = [&](real_t x, real_t y, real_t sx, real_t sy) {
            return d.z <= 0.0F ? 0.0F : blob(front_at(x, y), {sx, sy, 0.45F});
        };
        // The back of the skull and the temples.
        r += 0.035F * blob(unit({0.0F, 0.1F, -0.95F}), {0.6F, 0.45F, 0.4F});
        for (std::size_t s = 0; s < 2; ++s) {
            const real_t side = s == 0 ? 1.0F : -1.0F;
            r -= 0.015F * (1.0F + c("temples")) *
                 blob(unit({side * 0.82F, eye_y_ + 0.18F, 0.45F}), {0.15F, 0.15F, 0.2F});
        }
        // The brow ridge, and the dip between the brows.
        const real_t brow_y = eye_y_ + 0.2F + (0.05F * c("brow.height"));
        r += 0.022F * bones *
             std::max<real_t>(1.0F + (0.8F * c("brow.ridge")) + (0.5F * def), 0.0F) *
             front_blob(0.0F, brow_y, 0.55F, 0.07F);
        r -= 0.008F * fine * front_blob(0.0F, eye_y_ + 0.13F, 0.06F, 0.04F);
        // Eye sockets, the fold of a hooded lid and the trough under each eye.
        const real_t rn = eye_radius_ / head_.radii.x;
        for (std::size_t s = 0; s < 2; ++s) {
            const Vec3 e = eye_dir_[s];
            r -= 0.07F * (1.0F + (0.5F * c("eyes.depth"))) * front_blob(e.x, e.y, 0.17F, 0.11F);
            r += 0.012F * std::max<real_t>(1.0F + c("eyes.hooded"), 0.0F) *
                 front_blob(e.x, e.y + (rn * 0.62F), rn * 0.9F, rn * 0.25F);
            r -= 0.008F * fine * front_blob(e.x, e.y - (rn * 1.25F), 0.09F, 0.035F);
        }
        // The nose: a ridge from the bridge widening down to the tip, the tip and nostrils, and
        // the hollow under it.
        if (d.z > 0.0F) {
            const real_t span = nose_top_ - nose_tip_;
            const real_t t = (nose_top_ - d.y) / span;
            const real_t along = clamp01(t);
            real_t ends = 1.0F;
            if (t < 0.0F) {
                ends = gauss(((d.y - nose_top_) / 0.05F) * ((d.y - nose_top_) / 0.05F));
            } else if (t > 1.0F) {
                const real_t below =
                    ((t - 1.0F) * span) /
                    (0.035F * (1.0F - (0.35F * std::max<real_t>(c("nose.upturn"), 0.0F))));
                ends = gauss(below * below);
            }
            const real_t sx = (0.04F + (0.035F * along * std::sqrt(along))) * nose_w_;
            const real_t dx = (d.x - nose_dx_ * along) / sx;
            const real_t bridge = 0.03F * (1.0F + (0.7F * c("nose.bridge")));
            const real_t rise = along * std::sqrt(along);
            real_t amount = bridge + ((0.13F - bridge) * rise);
            amount += 0.035F * c("nose.hook") * cos_sin(to_float(kPi * along)).y;
            r += amount * nose_scale_ * ends * gauss(dx * dx) * (0.6F + (0.4F * nose_scale_));
        }
        const real_t tip_y = nose_tip_ + 0.015F + (0.025F * c("nose.upturn"));
        r += 0.06F * (1.0F + (0.5F * c("nose.tip"))) * nose_scale_ *
             front_blob(nose_dx_, tip_y, 0.055F * nose_w_ * (1.0F + (0.25F * c("nose.tip"))),
                        0.045F);
        const real_t flare = 1.0F + (0.3F * c("nose.nostrils"));
        for (const real_t side : {1.0F, -1.0F}) {
            r += 0.055F * nose_scale_ * flare *
                 front_blob(nose_dx_ + (side * 0.075F * nose_w_ * flare), nose_tip_ + 0.005F, 0.04F,
                            0.035F);
        }
        r -= 0.01F * front_blob(0.0F, nose_tip_ - 0.035F, 0.07F, 0.02F);
        // The philtrum's groove and ridges, between the nose and the upper lip.
        const real_t philtrum_y = (nose_tip_ + mouth_y_) * 0.5F;
        const real_t philtrum_h = std::max<real_t>((nose_tip_ - mouth_y_) * 0.35F, 0.01F);
        r -= 0.008F * fine * (1.0F + c("philtrum.depth")) *
             front_blob(0.0F, philtrum_y, 0.018F, philtrum_h);
        for (const real_t side : {1.0F, -1.0F}) {
            r += 0.006F * fine * front_blob(side * 0.032F, philtrum_y, 0.012F, philtrum_h);
        }
        // Cheekbones, cheeks and their hollows, the fold from nose to mouth.
        for (std::size_t s = 0; s < 2; ++s) {
            const real_t side = s == 0 ? 1.0F : -1.0F;
            r += 0.035F * bones * cheek_[s] *
                 std::max<real_t>(1.0F + (0.8F * c("cheeks.bones")) + (0.4F * def), 0.0F) *
                 front_blob(side * 0.52F * (1.0F + (0.05F * c("face.width"))), eye_y_ - 0.17F,
                            0.17F, 0.08F);
            r += (0.012F + (0.03F * c("cheeks.fullness"))) *
                 front_blob(side * 0.45F, mouth_y_ + 0.1F, 0.2F, 0.15F);
            r -= 0.035F *
                 (std::max<real_t>(c("cheeks.hollow"), 0.0F) +
                  (0.3F * std::max<real_t>(def, 0.0F))) *
                 front_blob(side * 0.6F, mouth_y_ + 0.08F, 0.12F, 0.1F);
            r -= 0.006F * fine *
                 crease(d, {side * 0.1F * nose_w_, nose_tip_},
                        {side * (mouth_w_ + 0.06F), mouth_y_ - 0.04F}, 0.012F);
        }
        // The lips, the seam between them and the dimples at their corners.
        const real_t protrude = 0.5F * c("lips.protrusion");
        r += 0.03F * std::max<real_t>(1.0F + (0.6F * c("lips.upper")) + protrude, 0.1F) *
             front_blob(0.0F, mouth_y_ + 0.025F, mouth_w_ * 0.85F, 0.03F);
        r += 0.035F * std::max<real_t>(1.0F + (0.6F * c("lips.lower")) + protrude, 0.1F) *
             front_blob(0.0F, mouth_y_ - 0.035F, mouth_w_ * 0.8F, 0.035F);
        if (d.z > 0.0F && std::abs(d.x) < mouth_w_ * 1.1F) {
            const real_t dy = (d.y - mouth_y_ - (mouth_tilt_ * d.x)) / 0.012F;
            const real_t taper = 1.0F - smooth(mouth_w_ * 0.8F, mouth_w_ * 1.1F, std::abs(d.x));
            r -= 0.012F * taper * gauss(dy * dy);
        }
        for (const real_t side : {1.0F, -1.0F}) {
            r -= 0.012F * std::max<real_t>(fine, 0.4F) *
                 front_blob(side * mouth_w_ * 1.02F, mouth_y_ + (0.015F * c("mouth.corners")),
                            0.02F, 0.02F);
        }
        // The chin, the groove above it and a cleft.
        r += 0.04F * std::max<real_t>(1.0F + (0.8F * c("chin.protrusion")) + (0.3F * def), 0.0F) *
             front_blob(0.0F, chin_y_, 0.15F * (1.0F + (0.3F * c("chin.width"))), 0.1F);
        r -= 0.012F * fine * front_blob(0.0F, mouth_y_ - 0.12F, 0.13F, 0.025F);
        r -= 0.015F * std::max<real_t>(c("chin.cleft"), 0.0F) *
             front_blob(0.0F, chin_y_, 0.012F, 0.05F);
        // The corners of the jaw.
        for (const real_t side : {1.0F, -1.0F}) {
            r += 0.025F * std::max<real_t>(1.0F + c("jaw.angle") + (0.5F * def), 0.0F) *
                 blob(unit({side * 0.7F * (1.0F + (0.08F * c("jaw.width"))), -0.6F, 0.3F}),
                      {0.15F, 0.12F, 0.2F});
        }
        // Carves.
        for (const Carve& carve : carves_) {
            if (dot3(d, carve.center) < 0.5F) {
                continue;
            }
            const real_t dx = d.x - carve.center.x;
            const real_t dy = d.y - carve.center.y;
            if (carve.kind == "dent" || carve.kind == "bump") {
                r += carve.amount * gauss(((dx * dx) + (dy * dy)) / (carve.radius * carve.radius));
            } else {
                const real_t t = std::clamp<real_t>((dx * carve.along.x) + (dy * carve.along.y),
                                                    -carve.half, carve.half);
                const real_t ox = dx - (carve.along.x * t);
                const real_t oy = dy - (carve.along.y * t);
                const real_t width = carve.radius * 0.45F;
                r += carve.amount * gauss(((ox * ox) + (oy * oy)) / (width * width));
            }
        }
        return r;
    }

    // A crease's depth along the segment from a to b on the face (x and y), 0 to 1.
    [[nodiscard]] static real_t crease(Vec3 d, Vec2 a, Vec2 b, real_t width) {
        if (d.z <= 0.0F) {
            return 0.0F;
        }
        const Vec2 ab = b - a;
        const real_t len2 = std::max<real_t>((ab.x * ab.x) + (ab.y * ab.y), 1e-9F);
        const real_t t = clamp01((((d.x - a.x) * ab.x) + ((d.y - a.y) * ab.y)) / len2);
        const real_t ox = d.x - (a.x + (ab.x * t));
        const real_t oy = d.y - (a.y + (ab.y * t));
        return gauss(((ox * ox) + (oy * oy)) / (width * width));
    }

    // Where a direction is about eye s: across its opening (u, -1 to 1 inside it), up from its
    // middle line (dy, in the unit sphere's units), the opening's top and bottom edges there
    // (above and below the line) and the eye's radius in those units.
    struct EyeFrame {
        real_t u = 0.0F;
        real_t dy = 0.0F;
        real_t upper = 0.0F;
        real_t lower = 0.0F;
        real_t rn = 0.0F;
    };
    [[nodiscard]] EyeFrame eye_frame(std::size_t s, Vec3 d) const {
        const real_t side = s == 0 ? 1.0F : -1.0F;
        const real_t rn = eye_radius_ / head_.radii.x;
        const real_t openness = std::clamp<real_t>(
            1.0F + (0.35F * c("eyes.openness")) - (0.15F * c("eyes.hooded")), 0.3F, 1.6F);
        const Vec3 e = eye_dir_[s];
        const real_t dx = (d.x - e.x) * side;
        const real_t u = dx / (rn * 0.92F);
        const real_t edge = std::clamp<real_t>(u, -0.999F, 0.999F);
        const real_t span = 1.0F - (edge * edge);
        return {.u = u,
                .dy = (d.y - e.y) - ((0.04F + (0.18F * c("eyes.tilt"))) * dx),
                .upper = rn * 0.33F * openness * span * (1.0F - (0.15F * edge)),
                .lower = rn * 0.24F * openness * span * (1.0F + (0.1F * edge)),
                .rn = rn};
    }

    // Whether a direction is in an eye's opening (behind which it hides), and in its lids.
    struct EyeZone {
        int side = -1;
        bool open = false;
        bool lid = false;
        bool rim = false;
    };
    [[nodiscard]] EyeZone eye_zone(Vec3 d) const {
        EyeZone zone;
        if (!style_.tucks || d.z <= 0.0F) {
            return zone;
        }
        for (std::size_t s = 0; s < 2; ++s) {
            const EyeFrame f = eye_frame(s, d);
            if (std::abs(f.u) > 1.35F || std::abs(f.dy) > f.rn * 1.2F) {
                continue;
            }
            zone.side = static_cast<int>(s);
            zone.lid = true;
            if (std::abs(f.u) < 1.0F) {
                zone.open = f.dy < f.upper && f.dy > -f.lower;
                zone.rim = !zone.open && f.dy < f.upper + (f.rn * 0.12F) &&
                           f.dy > -f.lower - (f.rn * 0.1F);
            }
            return zone;
        }
        return zone;
    }

    // Whether a direction on the lips' seam ring is in the mouth's opening.
    [[nodiscard]] bool in_mouth(Vec3 d) const {
        return style_.tucks && d.z > 0.0F && std::abs(d.x) < mouth_w_ * 0.92F;
    }

    // The head's point at a direction, in meters, with the eyelids over the eyes; on_seam says
    // the direction is on the lips' seam, which tuck pushes into the mouth (1 leaves it).
    [[nodiscard]] Vec3 place(Vec3 d, bool on_seam, real_t tuck, int* hidden) const {
        Vec3 q = shape(d);
        if (on_seam && in_mouth(d)) {
            q = q * tuck;
            if (hidden != nullptr) {
                *hidden = 2;
            }
            return meters_of(q);
        }
        Vec3 p = meters_of(q);
        const EyeZone zone = eye_zone(d);
        if (zone.side >= 0) {
            const Vec3 e = eye_center_[static_cast<std::size_t>(zone.side)];
            const Vec3 way = unit(p - e);
            const real_t dist = length3(p - e);
            if (zone.open) {
                p = e + (way * (eye_radius_ * 0.8F));
                if (hidden != nullptr) {
                    *hidden = 1;
                }
            } else if (zone.rim) {
                p = e + (way * std::max<real_t>(dist, eye_radius_ * 1.08F));
            } else if (zone.lid && dist < eye_radius_ * 1.05F) {
                p = e + (way * (eye_radius_ * 1.05F));
            }
        }
        // Skin anywhere else keeps clear of both eyeballs, so a big or shallow eye never shows
        // through the temple or the cheek.
        if (!zone.open && style_.tucks) {
            for (const Vec3& e : eye_center_) {
                const real_t dist = length3(p - e);
                if (dist < eye_radius_ * 1.04F && dist > 1e-6F) {
                    p = e + ((p - e) * (eye_radius_ * 1.04F / dist));
                }
            }
        }
        return p;
    }

    // What a triangle whose corners' directions average to d is.
    [[nodiscard]] FaceArea area(Vec3 d) const {
        if (d.z <= 0.0F) {
            return FaceArea::Skin;
        }
        const real_t t = d.x / mouth_w_;
        if (std::abs(t) < 1.0F) {
            const real_t span = std::sqrt(1.0F - (t * t));
            const real_t corners = 0.02F * c("mouth.corners") * t * t;
            const real_t seam = mouth_y_ + (mouth_tilt_ * d.x) + corners;
            const real_t upper = 0.05F * (1.0F + (0.5F * c("lips.upper"))) * span *
                                 (1.0F - (0.25F * gauss((t / 0.12F) * (t / 0.12F))));
            const real_t lower = 0.06F * (1.0F + (0.5F * c("lips.lower"))) * span;
            if (d.y < seam + upper && d.y > seam - lower) {
                return FaceArea::Lips;
            }
        }
        if (face_.brows.thickness > 0.0F) {
            for (std::size_t s = 0; s < 2; ++s) {
                const real_t side = s == 0 ? 1.0F : -1.0F;
                const real_t u = ((d.x * side) - 0.1F) / 0.5F;
                if (u < 0.0F || u > 1.0F) {
                    continue;
                }
                const real_t half = 0.02F *
                                    (0.3F + (static_cast<real_t>(face_.brows.thickness) * 1.4F)) *
                                    (1.0F - (0.6F * u));
                if (std::abs(d.y - brow_line(u, s)) < half) {
                    return FaceArea::Brows;
                }
            }
        }
        return FaceArea::Skin;
    }

    // How expression index moves the head's point p at direction d fully on, in meters.
    [[nodiscard]] Vec3 expression(std::size_t index, Vec3 d, Vec3 p) const {
        const std::string_view name = kExpressions[index];
        const real_t front = smooth(-0.2F, 0.25F, d.z);
        if (front <= 0.0F || name == "tongueOut") {
            return {};
        }
        real_t side = 0.0F;
        if (name.ends_with("Left")) {
            side = 1.0F;
        } else if (name.ends_with("Right")) {
            side = -1.0F;
        }
        // The share of a one-sided shape a direction takes, fading across the middle.
        const real_t half = side == 0.0F ? 1.0F : smooth(-0.06F, 0.06F, side * d.x);
        if (name.starts_with("eye")) {
            return eye_expression(name, side > 0.0F ? 0 : 1, d, p) * front;
        }
        if (name.starts_with("jaw") || name == "mouthClose") {
            return jaw_expression(name, d, p) * front;
        }
        if (name.starts_with("mouth")) {
            return mouth_expression(name, side, half, d, p) * front;
        }
        return upper_face_expression(name, side, half, d, p) * front;
    }

    // Where the jaw turns, in meters: in front of the ears, below the eyes.
    [[nodiscard]] Vec3 jaw_pivot() const { return meters_of({0.0F, -0.22F, -0.05F}); }

    // How the jaw's expressions move what moves with the jaw wholly (weight 1).
    [[nodiscard]] Vec3 jaw_motion(std::string_view name, Vec3 p) const {
        const real_t m = head_.radii.y;
        if (name == "jawOpen") {
            return turned_down(p, jaw_pivot(), 0.32F) - p;
        }
        if (name == "jawForward") {
            return {0.0F, 0.0F, 0.07F * m};
        }
        if (name == "jawLeft") {
            return {0.07F * m, 0.0F, 0.0F};
        }
        if (name == "jawRight") {
            return {-0.07F * m, 0.0F, 0.0F};
        }
        return {};
    }

    // Whether a beard grows at a direction: the jaw, chin and cheeks below a line from the ear
    // to the nose's side, and the lip under the nose.
    [[nodiscard]] bool bearded(Vec3 d) const {
        if (d.z < -0.25F || d.y < -0.98F) {
            return false;
        }
        const real_t ax = std::abs(d.x);
        // Bare round the lips, so the beard's thickness doesn't cover them.
        if (const real_t t = d.x / mouth_w_; std::abs(t) < 1.2F && d.z > 0.0F) {
            const real_t seam = mouth_y_ + (mouth_tilt_ * d.x);
            if (d.y < seam + 0.075F && d.y > seam - 0.1F) {
                return false;
            }
        }
        const real_t moustache = nose_tip_ - 0.02F;
        if (ax < mouth_w_ * 1.25F) {
            return d.y < moustache;
        }
        // From beside the mouth up to the cheek, then down to below the ear.
        const real_t line = ax < 0.55F ? mouth_y_ + 0.12F + ((ax - mouth_w_) * 0.4F)
                                       : mouth_y_ + 0.25F - ((ax - 0.55F) * 0.9F);
        return d.y < line;
    }

    // Where the face's features are on the unit sphere, for the skin's maps and marks.
    struct Features {
        std::array<Vec3, 2> eye_dir;
        real_t eye_y = 0.0F;
        real_t eye_x = 0.0F;
        real_t eye_rn = 0.0F; // an eye's radius in the unit sphere's units
        real_t brow_y = 0.0F;
        real_t mouth_y = 0.0F;
        real_t mouth_w = 0.0F;
        real_t nose_top = 0.0F;
        real_t nose_tip = 0.0F;
        real_t nose_w = 0.0F;
        real_t chin_y = 0.0F;
    };
    [[nodiscard]] Features features() const {
        return {.eye_dir = eye_dir_,
                .eye_y = eye_y_,
                .eye_x = eye_x_,
                .eye_rn = eye_radius_ / head_.radii.x,
                .brow_y = brow_line(0.4F, 0) + 0.02F,
                .mouth_y = mouth_y_,
                .mouth_w = mouth_w_,
                .nose_top = nose_top_,
                .nose_tip = nose_tip_,
                .nose_w = nose_w_,
                .chin_y = chin_y_};
    }

private:
    // The lips' seam's height at x, its corners raised or lowered.
    [[nodiscard]] real_t seam_at(real_t x) const {
        const real_t t = std::min<real_t>(std::abs(x) / mouth_w_, 1.0F);
        return mouth_y_ + (mouth_tilt_ * x) + (0.02F * c("mouth.corners") * t * t);
    }

    // How much a direction is the upper lip (and the skin above it) and the lower lip (and the
    // skin below it), each 0 to 1.
    [[nodiscard]] Vec2 lips_at(Vec3 d) const {
        const real_t lw = 1.0F - smooth(0.85F, 1.3F, std::abs(d.x) / mouth_w_);
        const real_t dy = d.y - seam_at(d.x);
        return {lw * smooth(-0.004F, 0.006F, dy) * (1.0F - smooth(0.05F, 0.12F, dy)),
                lw * smooth(0.004F, -0.006F, dy) * (1.0F - smooth(-0.06F, -0.14F, dy))};
    }

    // How much a direction moves with the jaw: below the seam, its line rising toward the ears.
    [[nodiscard]] real_t jaw_share(Vec3 d) const {
        const real_t ax = std::abs(d.x);
        const real_t line = seam_at(d.x) + (std::max<real_t>(ax - mouth_w_, 0.0F) * 0.5F);
        return smooth(0.006F, -0.02F, d.y - line) * smooth(-0.35F, 0.1F, d.z) *
               smooth(-0.97F, -0.88F, d.y);
    }

    // How much a direction is round the mouth's corner on a side (1 the face's left).
    [[nodiscard]] real_t corner_at(real_t side, Vec3 d) const {
        const real_t x = d.x - (side * mouth_w_);
        const real_t dy = d.y - seam_at(d.x);
        const real_t q = ((x * x) + (dy * dy));
        return std::min<real_t>(gauss(q / (0.07F * 0.07F)) + (0.5F * gauss(q / (0.14F * 0.14F))),
                                1.0F);
    }

    [[nodiscard]] real_t blob_at(Vec3 d, real_t x, real_t y, real_t rx, real_t ry) const {
        const real_t qx = (d.x - x) / rx;
        const real_t qy = (d.y - y) / ry;
        return gauss((qx * qx) + (qy * qy));
    }

    [[nodiscard]] Vec3 eye_expression(std::string_view name, std::size_t s, Vec3 d, Vec3 p) const {
        const EyeFrame f = eye_frame(s, d);
        if (std::abs(f.u) > 1.4F || f.dy > f.rn * 2.0F || f.dy < -f.rn * 1.8F) {
            return {};
        }
        const real_t across = 1.0F - smooth(0.95F, 1.35F, std::abs(f.u));
        const real_t edge = std::clamp<real_t>(f.u, -0.99F, 0.99F);
        const real_t span = 1.0F - (edge * edge);
        const real_t rn = f.rn;
        // The opening's height in turns of the eye, at its middle.
        const real_t open = 1.6F * (rn * 0.57F) * std::max<real_t>(span, 0.6F) * head_.radii.y /
                            (eye_radius_ * 1.05F);
        const real_t upper_lid =
            across * smooth(-0.05F * rn, 0.05F * rn, f.dy) *
            (1.0F - smooth(f.upper + (rn * 0.15F), f.upper + (rn * 0.65F), f.dy));
        const real_t lower_lid =
            across * smooth(0.05F * rn, -0.05F * rn, f.dy) *
            (1.0F - smooth(-f.lower - (rn * 0.2F), -f.lower - (rn * 0.8F), f.dy));
        const Vec3 e = eye_center_[s];
        // A closing lid is held out over the cornea, which stands a tenth of the eye proud of the
        // ball (the iris and pupil reach 1.1 of its radius).
        const auto clear = [&](Vec3 q, real_t closing) {
            const Vec3 v = q - e;
            const real_t r = length3(v);
            const real_t want = eye_radius_ * 1.14F;
            if (closing <= 0.0F || r <= 1e-6F || r >= want) {
                return q;
            }
            // Only over the front of the eye, where the cornea is.
            const real_t front = smooth(0.55F, 0.85F, v.z / r);
            return q + (v * ((want - r) / r * std::min<real_t>(closing * 4.0F, 1.0F) * front));
        };
        const auto lids = [&](real_t upper, real_t lower) {
            const Vec3 top = clear(turned_down(p, e, open * upper * upper_lid), upper * upper_lid);
            const Vec3 bottom =
                clear(turned_down(p, e, open * lower * lower_lid), -lower * lower_lid);
            return (top - p) + (bottom - p);
        };
        const real_t side = s == 0 ? 1.0F : -1.0F;
        const real_t unit_m = head_.radii.y;
        if (name.starts_with("eyeBlink")) {
            return lids(0.9F, -0.12F);
        }
        if (name.starts_with("eyeWide")) {
            return lids(-0.3F, 0.05F);
        }
        if (name.starts_with("eyeSquint")) {
            const real_t cheek =
                blob_at(d, eye_dir_[s].x, eye_dir_[s].y - (rn * 2.5F), rn * 2.0F, rn * 1.5F);
            return lids(0.12F, -0.35F) + (Vec3{0.0F, 0.03F * unit_m, 0.01F * unit_m} * cheek);
        }
        if (name.starts_with("eyeLookUp")) {
            return lids(-0.15F, -0.08F);
        }
        if (name.starts_with("eyeLookDown")) {
            return lids(0.35F, 0.1F);
        }
        // Looking in (toward the nose) or out, the lids follow the eye a little.
        const real_t way = name.starts_with("eyeLookIn") ? -side : side;
        return Vec3{way * 0.012F * unit_m, 0.0F, 0.0F} *
               std::min<real_t>(upper_lid + lower_lid, 1.0F);
    }

    [[nodiscard]] Vec3 jaw_expression(std::string_view name, Vec3 d, Vec3 p) const {
        if (name == "mouthClose") {
            // The lower lip back up as the jaw stays open, the upper lip down a little.
            const Vec2 lips = lips_at(d);
            return (jaw_motion("jawOpen", p) * (-0.85F * lips.y)) +
                   (Vec3{0.0F, -0.015F * head_.radii.y, 0.0F} * lips.x);
        }
        return jaw_motion(name, p) * jaw_share(d);
    }

    [[nodiscard]] Vec3 mouth_expression(std::string_view name, real_t side, real_t half, Vec3 d,
                                        Vec3 p) const {
        const real_t m = head_.radii.y;
        const Vec2 lips = lips_at(d);
        const real_t upper = lips.x;
        const real_t lower = lips.y;
        const real_t both = std::min<real_t>(upper + lower, 1.0F);
        const real_t across = p.x - head_.center.x;
        if (name == "mouthFunnel") {
            return (Vec3{-0.2F * across, 0.0F, 0.07F * m} * both) +
                   (Vec3{0.0F, 0.025F * m, 0.0F} * upper) + (Vec3{0.0F, -0.03F * m, 0.0F} * lower);
        }
        if (name == "mouthPucker") {
            return Vec3{-0.35F * across, 0.0F, 0.09F * m} * both;
        }
        if (name == "mouthLeft" || name == "mouthRight") {
            const real_t wide = (1.0F - smooth(0.9F, 1.6F, std::abs(d.x) / mouth_w_)) *
                                (1.0F - smooth(0.12F, 0.24F, std::abs(d.y - seam_at(d.x))));
            return Vec3{side * 0.08F * m, 0.0F, 0.0F} * wide;
        }
        if (name == "mouthRollLower") {
            return Vec3{0.0F, 0.02F * m, -0.035F * m} * lower;
        }
        if (name == "mouthRollUpper") {
            return Vec3{0.0F, -0.02F * m, -0.035F * m} * upper;
        }
        if (name == "mouthShrugLower") {
            const real_t chin = (1.0F - smooth(0.9F, 1.5F, std::abs(d.x) / mouth_w_)) *
                                gauss(((d.y - seam_at(d.x) + 0.22F) / 0.1F) *
                                      ((d.y - seam_at(d.x) + 0.22F) / 0.1F));
            return Vec3{0.0F, 0.04F * m, 0.015F * m} * std::min<real_t>(lower + chin, 1.0F);
        }
        if (name == "mouthShrugUpper") {
            return Vec3{0.0F, 0.035F * m, 0.01F * m} * upper;
        }
        if (name.starts_with("mouthPress")) {
            return ((Vec3{0.0F, -0.015F * m, -0.01F * m} * upper) +
                    (Vec3{0.0F, 0.015F * m, -0.01F * m} * lower)) *
                   half;
        }
        if (name.starts_with("mouthLowerDown")) {
            return Vec3{0.0F, -0.05F * m, 0.0F} * (lower * half);
        }
        if (name.starts_with("mouthUpperUp")) {
            return Vec3{0.0F, 0.05F * m, 0.0F} * (upper * half);
        }
        const real_t corner = corner_at(side, d);
        if (name.starts_with("mouthSmile")) {
            const real_t cheek = blob_at(d, side * 0.4F, mouth_y_ + 0.15F, 0.14F, 0.14F);
            return (Vec3{side * 0.03F * m, 0.065F * m, -0.02F * m} * corner) +
                   (Vec3{0.0F, 0.025F * m, 0.02F * m} * cheek);
        }
        if (name.starts_with("mouthFrown")) {
            return Vec3{side * 0.005F * m, -0.06F * m, 0.0F} * corner;
        }
        if (name.starts_with("mouthDimple")) {
            return Vec3{side * 0.015F * m, 0.0F, -0.035F * m} * corner;
        }
        if (name.starts_with("mouthStretch")) {
            return Vec3{side * 0.065F * m, -0.025F * m, -0.015F * m} * corner;
        }
        return {};
    }

    [[nodiscard]] Vec3 upper_face_expression(std::string_view name, real_t side, real_t half,
                                             Vec3 d, Vec3 p) const {
        const real_t m = head_.radii.y;
        // How much a direction is a brow on a side, and how far out along it (0 inner, 1 outer).
        const auto brow = [&](real_t on) -> Vec2 {
            const real_t u = ((d.x * on) - 0.1F) / 0.5F;
            const real_t inside = smooth(-0.3F, 0.0F, u) * (1.0F - smooth(1.0F, 1.3F, u));
            const real_t line = brow_line(std::clamp<real_t>(u, 0.0F, 1.0F), on > 0.0F ? 0 : 1);
            const real_t dy = (d.y - line) / 0.12F;
            return {inside * gauss(dy * dy), std::clamp<real_t>(u, 0.0F, 1.0F)};
        };
        if (name.starts_with("browDown")) {
            const Vec2 b = brow(side);
            return Vec3{-side * 0.02F * m * (1.0F - b.y), -0.07F * m, 0.01F * m} * b.x;
        }
        if (name == "browInnerUp") {
            const Vec2 b = brow(d.x >= 0.0F ? 1.0F : -1.0F);
            return Vec3{0.0F, 0.08F * m, 0.0F} * (b.x * (1.0F - smooth(0.0F, 0.6F, b.y)));
        }
        if (name.starts_with("browOuterUp")) {
            const Vec2 b = brow(side);
            return Vec3{0.0F, 0.08F * m, 0.0F} * (b.x * smooth(0.3F, 0.9F, b.y));
        }
        if (name == "cheekPuff") {
            const real_t cheeks = blob_at(d, 0.45F, mouth_y_ + 0.1F, 0.17F, 0.17F) +
                                  blob_at(d, -0.45F, mouth_y_ + 0.1F, 0.17F, 0.17F);
            return unit(p - head_.center) * (0.09F * m * std::min<real_t>(cheeks, 1.0F));
        }
        if (name.starts_with("cheekSquint")) {
            return Vec3{0.0F, 0.04F * m, 0.015F * m} *
                   blob_at(d, side * 0.42F, eye_y_ - 0.22F, 0.12F, 0.12F);
        }
        if (name.starts_with("noseSneer")) {
            const real_t nose = blob_at(d, side * 0.09F * nose_w_, nose_tip_ + 0.07F, 0.06F, 0.07F);
            return Vec3{side * 0.005F * m, 0.045F * m, 0.005F * m} * (nose * half);
        }
        return {};
    }

    const CharacterFace& face_;
    HeadShape head_;
    Style style_;
    std::array<real_t, 2> eye_dy_{};
    std::array<real_t, 2> brow_dy_{};
    std::array<real_t, 2> cheek_{1.0F, 1.0F};
    real_t nose_dx_ = 0.0F;
    real_t mouth_tilt_ = 0.0F;
    real_t eye_size_ = 1.0F;
    real_t eye_x_ = 0.38F;
    real_t eye_y_ = 0.12F;
    real_t eye_radius_ = 0.0F;
    std::array<Vec3, 2> eye_center_{};
    std::array<Vec3, 2> eye_dir_{};
    real_t mouth_y_ = -0.46F;
    real_t mouth_w_ = 0.23F;
    real_t nose_top_ = 0.16F;
    real_t nose_tip_ = -0.2F;
    real_t nose_scale_ = 1.0F;
    real_t nose_w_ = 1.0F;
    real_t chin_y_ = -0.8F;
    std::vector<Carve> carves_;
};

// Spacing that gathers samples near 0: t from -1 to 1 to the same range, a times denser at 0.
real_t gather(real_t t, real_t a) {
    return (t + (a * t * t * t)) / (1.0F + a);
}

// The latitudes of a head's rings, bottom to top, denser over the face, one of them on the lips'
// seam.
std::vector<real_t> latitudes(std::size_t rings, real_t seam) {
    constexpr real_t kCenter = -0.2F;
    std::vector<real_t> out(rings + 1);
    for (std::size_t i = 0; i <= rings; ++i) {
        const real_t t = ((2.0F * static_cast<real_t>(i)) / static_cast<real_t>(rings)) - 1.0F;
        const real_t g = gather(t, 2.0F);
        const real_t span = g < 0.0F ? (kPi * 0.5F) + kCenter : (kPi * 0.5F) - kCenter;
        out[i] = kCenter + (g * span);
    }
    out.front() = -kPi * 0.5F;
    out.back() = kPi * 0.5F;
    std::size_t nearest = 1;
    for (std::size_t i = 1; i < rings; ++i) {
        if (std::abs(out[i] - seam) < std::abs(out[nearest] - seam)) {
            nearest = i;
        }
    }
    out[nearest] = seam;
    return out;
}

// --- Skin maps and marks ----------------------------------------------------------------------

// Colors mixed and scaled channel by channel.
Vec4 mix4(Vec4 a, Vec4 b, real_t t) {
    const auto f = static_cast<float>(t);
    return {a.x + ((b.x - a.x) * f), a.y + ((b.y - a.y) * f), a.z + ((b.z - a.z) * f),
            a.w + ((b.w - a.w) * f)};
}

// A lattice value of a seed, 0 to 1, and value noise over it, smooth and the same everywhere.
real_t lattice(std::int64_t x, std::int64_t y, std::int64_t z, std::uint32_t seed) {
    std::uint32_t h = seed ^ (static_cast<std::uint32_t>(x) * 0x8DA6B343U) ^
                      (static_cast<std::uint32_t>(y) * 0xD8163841U) ^
                      (static_cast<std::uint32_t>(z) * 0xCB1AB31FU);
    h ^= h >> 16U;
    h *= 0x7FEB352DU;
    h ^= h >> 15U;
    h *= 0x846CA68BU;
    h ^= h >> 16U;
    return static_cast<real_t>(h & 0xFFFFU) / 65535.0F;
}

real_t value_noise(Vec3 p, std::uint32_t seed) {
    const real_t fx = std::floor(p.x);
    const real_t fy = std::floor(p.y);
    const real_t fz = std::floor(p.z);
    const auto x = static_cast<std::int64_t>(fx);
    const auto y = static_cast<std::int64_t>(fy);
    const auto z = static_cast<std::int64_t>(fz);
    const auto fade = [](real_t t) { return t * t * (3.0F - (2.0F * t)); };
    const real_t u = fade(p.x - fx);
    const real_t v = fade(p.y - fy);
    const real_t w = fade(p.z - fz);
    const auto mix = [](real_t a, real_t b, real_t t) { return a + ((b - a) * t); };
    const auto at = [&](std::int64_t i, std::int64_t j, std::int64_t k) {
        return lattice(x + i, y + j, z + k, seed);
    };
    return mix(mix(mix(at(0, 0, 0), at(1, 0, 0), u), mix(at(0, 1, 0), at(1, 1, 0), u), v),
               mix(mix(at(0, 0, 1), at(1, 0, 1), u), mix(at(0, 1, 1), at(1, 1, 1), u), v), w);
}

// How much of the skin's detail a style keeps, and the maps' size.
struct SkinStyle {
    real_t pores = 0.0F;
    real_t blotches = 0.0F;
    real_t oil = 0.0F;
    real_t relief = 1.0F; // marks' relief
    real_t soft = 1.0F;   // how soft marks' edges are
    std::uint32_t width = 256;
};

SkinStyle skin_style(std::string_view style) {
    if (style == "realistic") {
        return {.pores = 0.5F, .blotches = 0.35F, .oil = 0.35F, .width = 1024};
    }
    if (style == "stylized") {
        return {.pores = 0.15F, .blotches = 0.25F, .oil = 0.2F, .relief = 0.6F, .width = 512};
    }
    if (style == "painterly") {
        return {.blotches = 0.2F, .relief = 0.3F, .soft = 2.5F, .width = 512};
    }
    return {.relief = 0.0F, .soft = 0.3F, .width = 256}; // toon and low_poly: flat marks
}

// A mark's own sizes and colors where the face leaves them out.
struct MarkKind {
    real_t size = 0.02F;
    real_t length = 0.0F;
    Vec4 color{0.0F, 0.0F, 0.0F, 1.0F};
};

MarkKind mark_kind(const FaceMark& mark) {
    const std::string& k = mark.kind;
    const real_t age = clamp01(mark.age);
    const auto mixed = [](Vec4 a, Vec4 b, real_t t) { return mix4(a, b, t); };
    if (k == "mole") {
        return {.size = 0.012F, .color = {0.24F, 0.15F, 0.11F, 1.0F}};
    }
    if (k == "freckles") {
        return {.size = 0.14F, .color = {0.6F, 0.38F, 0.24F, 1.0F}};
    }
    if (k == "scar") {
        return {.size = 0.012F,
                .length = 0.12F,
                .color = mixed({0.86F, 0.46F, 0.46F, 1.0F}, {0.95F, 0.84F, 0.8F, 1.0F}, age)};
    }
    if (k == "wound") {
        return {.size = 0.01F,
                .length = 0.07F,
                .color = mixed({0.36F, 0.02F, 0.03F, 1.0F}, {0.3F, 0.13F, 0.08F, 1.0F}, age)};
    }
    if (k == "burn") {
        return {.size = 0.07F,
                .color = mixed({0.8F, 0.3F, 0.24F, 1.0F}, {0.88F, 0.72F, 0.68F, 1.0F}, age)};
    }
    if (k == "bruise") {
        // Purple, then blue and green, then yellow as it heals.
        const Vec4 purple{0.36F, 0.12F, 0.3F, 1.0F};
        const Vec4 blue{0.3F, 0.24F, 0.42F, 1.0F};
        const Vec4 green{0.48F, 0.52F, 0.3F, 1.0F};
        const Vec4 yellow{0.8F, 0.7F, 0.36F, 1.0F};
        Vec4 color = mixed(green, yellow, (age - 0.7F) / 0.3F);
        if (age < 0.35F) {
            color = mixed(purple, blue, age / 0.35F);
        } else if (age < 0.7F) {
            color = mixed(blue, green, (age - 0.35F) / 0.35F);
        }
        return {.size = 0.05F, .color = color};
    }
    if (k == "rash") {
        return {.size = 0.09F, .color = {0.85F, 0.3F, 0.28F, 1.0F}};
    }
    if (k == "birthmark") {
        return {.size = 0.06F, .color = {0.62F, 0.18F, 0.25F, 1.0F}};
    }
    if (k == "tattoo") {
        return {.size = 0.05F,
                .color = mixed({0.08F, 0.1F, 0.18F, 1.0F}, {0.16F, 0.26F, 0.3F, 1.0F}, age)};
    }
    // Paint.
    return {.size = 0.12F, .color = {0.75F, 0.08F, 0.08F, 1.0F}};
}

std::string_view mark_pattern(const FaceMark& mark) {
    if (!mark.pattern.empty() && mark.pattern != "stitched") {
        return mark.pattern;
    }
    if (mark.kind == "tattoo") {
        return "lines";
    }
    return mark.kind == "paint" ? "band" : "";
}

// A mark placed on the head: its center on the unit sphere, a frame across and up there, its
// sizes and turn, and which copy it is (a mirrored one is flipped across).
struct PlacedMark {
    const FaceMark* mark = nullptr;
    Vec3 center;
    Vec3 across;
    Vec3 up;
    real_t size = 0.0F;
    real_t half = 0.0F; // half its length
    Vec4 color;
    Vec2 turn{1.0F, 0.0F};
    real_t flip = 1.0F;
    real_t reach = 0.0F; // how far from the center it can cover
    std::string_view pattern;
};

std::vector<PlacedMark> place_marks(const CharacterFace& face, const Sculptor& sculptor) {
    std::vector<PlacedMark> out;
    for (const FaceMark& mark : face.marks) {
        const MarkKind kind = mark_kind(mark);
        const Vec3 at = sculptor.landmark_direction(mark.at);
        const Vec2f turn = cos_sin(mark.angle * std::numbers::pi_v<float> / 180.0F);
        for (int copy = 0; copy < (mark.mirror ? 2 : 1); ++copy) {
            const real_t flip = copy == 0 ? 1.0F : -1.0F;
            const real_t x = (at.x * flip) + (mark.offset.x * flip);
            const real_t y = at.y + mark.offset.y;
            PlacedMark made;
            made.mark = &mark;
            made.center = at.z >= 0.0F ? front_at(std::clamp<real_t>(x, -0.99F, 0.99F),
                                                  std::clamp<real_t>(y, -0.99F, 0.99F))
                                       : unit({x, y, at.z});
            const Vec3 c = made.center;
            made.across = std::abs(c.y) > 0.98F ? Vec3{1.0F, 0.0F, 0.0F} : unit({c.z, 0.0F, -c.x});
            made.up = cross3(c, made.across);
            made.size = mark.size > 0.0F ? mark.size : kind.size;
            const real_t length = mark.length > 0.0F ? mark.length : kind.length;
            made.pattern = mark_pattern(mark);
            const bool long_pattern = made.pattern == "band" || made.pattern == "mask";
            made.half = made.size * (long_pattern ? 3.0F : 1.2F);
            if (length > 0.0F) {
                made.half = length * 0.5F;
            }
            made.color = mark.color.w > 0.0F ? mark.color : kind.color;
            made.turn = {static_cast<real_t>(turn.x), static_cast<real_t>(turn.y)};
            made.flip = flip;
            made.reach = (std::max(made.size, made.half) * 1.8F) + 0.02F;
            out.push_back(made);
        }
    }
    return out;
}

// A direction's place in a mark's own frame: along it (x) and across it (y), in the unit
// sphere's units. False when it is too far to be in it.
bool mark_coords(const PlacedMark& m, Vec3 d, Vec2& local) {
    if (dot3(d, m.center) < 0.2F) {
        return false;
    }
    const Vec3 v = d - m.center;
    if (std::abs(v.x) > m.reach || std::abs(v.y) > m.reach || std::abs(v.z) > m.reach) {
        return false;
    }
    const real_t ax = dot3(v, m.across) * m.flip;
    const real_t ay = dot3(v, m.up);
    local = {(m.turn.x * ax) + (m.turn.y * ay), (-m.turn.y * ax) + (m.turn.x * ay)};
    return true;
}

// The distance from p to the segment from a to b.
real_t segment_distance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const real_t len2 = std::max<real_t>((ab.x * ab.x) + (ab.y * ab.y), 1e-12F);
    const real_t t = clamp01((((p.x - a.x) * ab.x) + ((p.y - a.y) * ab.y)) / len2);
    const Vec2 q = p - (a + (ab * t));
    return std::sqrt((q.x * q.x) + (q.y * q.y));
}

// A pattern's signed distance at a point in its frame: below 0 inside, in its units.
real_t pattern_distance(std::string_view pattern, Vec2 p, real_t s, real_t half) {
    const real_t r = std::sqrt((p.x * p.x) + (p.y * p.y));
    const real_t big = 1.0F;
    if (pattern == "stripes") {
        real_t best = big;
        for (const real_t row : {-0.45F, 0.0F, 0.45F}) {
            best =
                std::min(best, segment_distance(p, {-half, row * s}, {half, row * s}) - (0.1F * s));
        }
        return best;
    }
    if (pattern == "chevron") {
        const Vec2 tip{0.0F, -0.25F * s};
        return std::min(segment_distance(p, {-0.6F * s, 0.35F * s}, tip),
                        segment_distance(p, tip, {0.6F * s, 0.35F * s})) -
               (0.1F * s);
    }
    if (pattern == "dots") {
        real_t best = big;
        for (const real_t at : {-0.6F, 0.0F, 0.6F}) {
            const Vec2 q = p - Vec2{at * s, 0.0F};
            best = std::min(best, std::sqrt((q.x * q.x) + (q.y * q.y)) - (0.16F * s));
        }
        return best;
    }
    if (pattern == "ring") {
        return std::abs(r - (0.6F * s)) - (0.1F * s);
    }
    if (pattern == "star" || pattern == "spiral") {
        const real_t theta = slash::atan2(to_float(p.y), to_float(p.x));
        if (pattern == "star") {
            constexpr real_t kPoint = kPi / 5.0F;
            real_t folded = std::fmod(theta + (kPi * 2.5F), 2.0F * kPoint) - kPoint;
            folded = std::abs(folded) / kPoint;
            return r - (s * (0.4F + (0.6F * (1.0F - folded))) * 0.85F);
        }
        // An Archimedean spiral of two and a half turns, its arm 0.06 of the size wide.
        const real_t pitch = 0.22F * s;
        const real_t wound = (theta + kPi) / (2.0F * kPi) * pitch;
        real_t best = big;
        for (int k = 0; k < 5; ++k) {
            const real_t arm = wound + (static_cast<real_t>(k) * pitch);
            if (arm <= s) {
                best = std::min(best, std::abs(r - arm));
            }
        }
        return std::max(best - (0.06F * s), r - s);
    }
    if (pattern == "teardrop") {
        const real_t drop = 0.3F * s;
        const Vec2 q = p - Vec2{0.0F, -drop};
        const real_t round = std::sqrt((q.x * q.x) + (q.y * q.y)) - drop;
        if (p.y < -drop || p.y > 0.5F * s) {
            return p.y < -drop ? round : std::max(round, p.y - (0.5F * s));
        }
        const real_t width = drop * (1.0F - ((p.y + drop) / (0.8F * s)));
        return std::min(round, std::abs(p.x) - width);
    }
    if (pattern == "cross") {
        const auto box = [&](real_t w, real_t h) {
            return std::max(std::abs(p.x) - w, std::abs(p.y) - h);
        };
        return std::min(box(0.12F * s, 0.6F * s), box(0.6F * s, 0.12F * s));
    }
    if (pattern == "band" || pattern == "mask") {
        const real_t along = clamp01(std::abs(p.x) / std::max<real_t>(half, 1e-6F));
        const real_t width =
            pattern == "band" ? 0.35F * s : 0.4F * s * (1.0F - (0.55F * along * along));
        return std::max(std::abs(p.y) - width, std::abs(p.x) - half);
    }
    if (pattern == "half") {
        return std::max(-p.x, r - s);
    }
    // Lines: a sweep along it that waves and tapers at both ends.
    if (std::abs(p.x) > half) {
        return std::abs(p.x) - half;
    }
    const real_t along = p.x / std::max<real_t>(half, 1e-6F);
    const real_t wave = 0.35F * s * cos_sin(to_float(kPi * along)).y;
    return std::abs(p.y - wave) - (0.14F * s * (1.0F - (along * along)));
}

// What a mark does at a texel: how much of its color covers the skin (and the color), how it
// raises (meters) and how shiny it makes it (a roughness and how much of it).
struct MarkTexel {
    real_t cover = 0.0F;
    Vec4 color;
    real_t height = 0.0F;
    real_t roughness = 0.0F;
    real_t shine = 0.0F;
};

MarkTexel mark_texel(const PlacedMark& m, Vec2 p, Vec3 d, const SkinStyle& style) {
    const FaceMark& mark = *m.mark;
    const std::string& kind = mark.kind;
    const real_t amount = clamp01(mark.amount);
    const real_t age = clamp01(mark.age);
    const real_t s = m.size;
    const real_t r = std::sqrt((p.x * p.x) + (p.y * p.y));
    const real_t relief = style.relief;
    const real_t edge = 0.0025F * style.soft;
    MarkTexel out;
    out.color = m.color;
    // A line's distance and its width there, tapered toward its ends.
    const auto line = [&](real_t width, real_t& dist) {
        const real_t t = std::clamp(p.x, -m.half, m.half);
        const real_t taper = m.half > 0.0F ? t / m.half : 0.0F;
        dist = std::sqrt(((p.x - t) * (p.x - t)) + (p.y * p.y));
        return width * (1.0F - (0.7F * taper * taper));
    };
    const auto stitches = [&](real_t width) -> real_t {
        if (mark.pattern != "stitched") {
            return 0.0F;
        }
        const real_t spacing = std::max<real_t>(0.018F, width * 1.8F);
        const real_t k = std::round(p.x / spacing);
        if (std::abs(k * spacing) > m.half) {
            return 0.0F;
        }
        const real_t across = std::abs(p.x - (k * spacing));
        return (1.0F - smooth(0.0015F, 0.0025F, across)) *
               (1.0F - smooth(width * 2.0F, width * 2.4F, std::abs(p.y)));
    };
    // A patch's rim: its radius wanders as noise over the head says.
    const auto patch = [&](real_t wander, real_t frequency) {
        const real_t n = value_noise(d * frequency, mark.seed + 17U);
        return r / (s * (1.0F - (wander * 0.5F) + (wander * n)));
    };
    if (kind == "mole") {
        out.cover = amount * (1.0F - smooth(s * 0.75F, s * 0.75F + edge, r));
        out.height = 0.4e-3F * relief * amount * gauss((r * r) / (s * s * 0.5F));
    } else if (kind == "freckles") {
        const real_t inside = 1.0F - smooth(s * 0.6F, s, r);
        if (inside > 0.0F) {
            constexpr real_t kCell = 0.014F;
            const auto cx = static_cast<std::int64_t>(std::floor(p.x / kCell));
            const auto cy = static_cast<std::int64_t>(std::floor(p.y / kCell));
            real_t dot = 0.0F;
            for (std::int64_t i = -1; i <= 1; ++i) {
                for (std::int64_t j = -1; j <= 1; ++j) {
                    const std::int64_t gx = cx + i;
                    const std::int64_t gy = cy + j;
                    if (lattice(gx, gy, 0, mark.seed) > 0.25F + (0.6F * amount)) {
                        continue;
                    }
                    const Vec2 at{(static_cast<real_t>(gx) + lattice(gx, gy, 1, mark.seed)) * kCell,
                                  (static_cast<real_t>(gy) + lattice(gx, gy, 2, mark.seed)) *
                                      kCell};
                    const real_t radius = 0.0028F + (0.0025F * lattice(gx, gy, 3, mark.seed));
                    const Vec2 q = p - at;
                    const real_t dist = std::sqrt((q.x * q.x) + (q.y * q.y));
                    dot = std::max(dot, 1.0F - smooth(radius * 0.6F, radius + (edge * 0.4F), dist));
                }
            }
            out.cover = 0.75F * inside * dot;
        }
    } else if (kind == "scar") {
        real_t dist = 0.0F;
        const real_t width = line(s, dist);
        const real_t on = std::abs(p.x) <= m.half + width
                              ? 1.0F - smooth(width * 0.6F, width + edge, dist)
                              : 0.0F;
        out.cover = amount * 0.85F * on;
        out.height = 0.5e-3F * relief * amount * (1.0F - age * 0.5F) *
                     std::max<real_t>(1.0F - (dist / std::max<real_t>(width, 1e-6F)), 0.0F);
        out.roughness = 0.38F;
        out.shine = on * amount;
        if (const real_t stitch = stitches(width); stitch > 0.0F) {
            out.cover = std::max(out.cover, stitch * amount);
            out.color = mix4({0.0F, 0.0F, 0.0F, 1.0F}, out.color, 0.55F);
            out.height -= 0.2e-3F * relief * stitch;
        }
    } else if (kind == "wound") {
        real_t dist = 0.0F;
        const real_t width = line(s, dist);
        if (std::abs(p.x) <= m.half + (width * 2.0F)) {
            const real_t core = 1.0F - smooth(width * 0.5F, width + edge, dist);
            const real_t rim = (1.0F - smooth(width, width * 2.6F, dist)) * (1.0F - age);
            const Vec4 swollen{0.72F, 0.24F, 0.22F, 1.0F};
            out.color = mix4(swollen, m.color, core);
            out.cover = amount * std::max(core, rim * 0.6F);
            out.height = relief * amount * ((-0.8e-3F * (1.0F - age) * core) + (0.2e-3F * rim));
            out.roughness = age > 0.5F ? 0.85F : 0.15F;
            out.shine = core * amount;
        }
        if (const real_t stitch = stitches(width); stitch > 0.0F) {
            out.cover = std::max(out.cover, stitch * amount);
            out.color = {0.08F, 0.08F, 0.1F, 1.0F};
        }
    } else if (kind == "burn" || kind == "birthmark") {
        const bool burn = kind == "burn";
        const real_t q = patch(burn ? 0.4F : 0.6F, burn ? 22.0F : 9.0F);
        const real_t on = 1.0F - smooth(0.75F, 1.0F + (edge * 10.0F), q);
        if (on > 0.0F) {
            const real_t mottle = value_noise(d * 60.0F, mark.seed + 3U);
            if (burn) {
                const Vec4 deep =
                    age < 0.5F ? Vec4{0.55F, 0.12F, 0.1F, 1.0F} : Vec4{0.58F, 0.38F, 0.34F, 1.0F};
                out.color = mix4(m.color, deep,
                                 smooth(0.45F, 0.8F, mottle) * (1.0F - smooth(0.4F, 0.9F, q)));
                out.cover = amount * on;
                out.height = 0.4e-3F * relief * amount * (mottle - 0.5F) * on;
                out.roughness = 0.3F;
                out.shine = on * amount;
            } else {
                out.cover = amount * 0.7F * on * (0.85F + (0.15F * mottle));
            }
        }
    } else if (kind == "bruise") {
        const real_t q = patch(0.35F, 14.0F);
        const real_t on = gauss(q * q * 1.6F);
        out.cover = amount * 0.8F * on;
        out.height = 0.6e-3F * relief * amount * (1.0F - age) * on;
    } else if (kind == "rash") {
        const real_t q = patch(0.5F, 10.0F);
        const real_t on = 1.0F - smooth(0.6F, 1.0F, q);
        if (on > 0.0F) {
            const real_t bumps = value_noise(d * 140.0F, mark.seed + 5U);
            const real_t blotch = smooth(0.35F, 0.7F, value_noise(d * 35.0F, mark.seed + 9U));
            out.cover = amount * on * (0.35F + (0.45F * blotch) + (0.2F * bumps));
            out.height = 0.15e-3F * relief * amount * on * smooth(0.5F, 0.9F, bumps);
        }
    } else {
        // Tattoos and paint: a pattern, blurred as a tattoo ages.
        const bool paint = kind == "paint";
        const real_t blur = edge + (paint ? 0.0F : 0.004F * age);
        const real_t sd = pattern_distance(m.pattern, p, s, m.half);
        const real_t on = 1.0F - smooth(-blur, blur, sd);
        out.cover = amount * on * (paint ? 0.95F : 1.0F - (0.35F * age));
        if (paint) {
            out.roughness = 0.85F;
            out.shine = on * amount;
        }
    }
    return out;
}

// The skin's wrinkles at a direction, 0 to 1: lines across the forehead, frown lines between
// the brows, crow's feet, the folds from the nose and the lines under the eyes.
real_t wrinkle_depth(const Sculptor::Features& f, Vec3 d) {
    if (d.z <= 0.0F) {
        return 0.0F;
    }
    real_t depth = 0.0F;
    const auto add = [&](Vec2 a, Vec2 b, real_t width, real_t weight) {
        depth = std::max(depth, weight * Sculptor::crease(d, a, b, width));
    };
    for (int k = 0; k < 3; ++k) {
        const real_t y = f.brow_y + 0.1F + (0.07F * static_cast<real_t>(k));
        add({-0.32F, y}, {0.32F, y + 0.01F}, 0.008F, 0.8F - (0.15F * static_cast<real_t>(k)));
    }
    for (const real_t side : {1.0F, -1.0F}) {
        add({side * 0.05F, f.brow_y - 0.02F}, {side * 0.07F, f.brow_y + 0.07F}, 0.007F, 0.7F);
        const Vec3 eye = f.eye_dir[side > 0.0F ? 0 : 1];
        const real_t outer = eye.x + (side * f.eye_rn * 1.2F);
        for (const real_t spread : {-0.05F, 0.0F, 0.05F}) {
            add({outer, eye.y}, {outer + (side * 0.09F), eye.y + spread}, 0.006F, 0.7F);
        }
        add({eye.x - (side * f.eye_rn * 0.6F), eye.y - (f.eye_rn * 1.3F)},
            {eye.x + (side * f.eye_rn * 0.9F), eye.y - (f.eye_rn * 1.2F)}, 0.006F, 0.5F);
        add({side * f.nose_w * 0.13F, f.nose_tip + 0.03F},
            {side * f.mouth_w * 1.15F, f.mouth_y - 0.03F}, 0.012F, 1.0F);
    }
    return depth;
}

// Shares of the face that flush (cheeks, nose, ears) and shine (the forehead, nose and chin).
real_t flush_share(const Sculptor::Features& f, Vec3 d) {
    real_t share = 0.0F;
    for (const real_t side : {1.0F, -1.0F}) {
        const real_t dx = d.x - (side * 0.45F);
        const real_t dy = d.y - (f.mouth_y + 0.25F);
        share = std::max(share, gauss(((dx * dx) + (dy * dy)) / 0.03F));
    }
    const real_t ny = d.y - f.nose_tip;
    share = std::max(share, gauss(((d.x * d.x) + (ny * ny)) / 0.006F));
    share = std::max(share, smooth(0.85F, 0.97F, std::abs(d.x)) * smooth(-0.4F, 0.0F, d.z));
    return d.z < -0.4F ? 0.0F : share;
}

real_t shine_share(const Sculptor::Features& f, Vec3 d) {
    if (d.z <= 0.0F) {
        return 0.0F;
    }
    const real_t forehead = (1.0F - smooth(0.25F, 0.4F, std::abs(d.x))) *
                            smooth(f.brow_y + 0.02F, f.brow_y + 0.1F, d.y);
    const real_t nose = (1.0F - smooth(f.nose_w * 0.1F, f.nose_w * 0.2F, std::abs(d.x))) *
                        smooth(f.nose_tip - 0.04F, f.nose_tip, d.y) *
                        (1.0F - smooth(f.eye_y, f.eye_y + 0.06F, d.y));
    const real_t cy = d.y - f.chin_y - 0.08F;
    const real_t chin = gauss(((d.x * d.x) + (cy * cy)) / 0.006F);
    return std::max({forehead, nose, chin});
}

} // namespace

bool is_face_path(std::string_view path) {
    return path.ends_with(kFaceSuffix);
}

const std::vector<FaceControl>& face_controls() {
    return kControls;
}

std::optional<std::size_t> face_control_index(std::string_view name) {
    for (std::size_t i = 0; i < kControls.size(); ++i) {
        if (kControls[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

const std::vector<std::string_view>& face_landmark_names() {
    return kLandmarks;
}

float CharacterFace::control(std::size_t index) const {
    return index < controls.size() ? controls[index] : 0.0F;
}

float CharacterFace::control(std::string_view name) const {
    const auto index = face_control_index(name);
    return index ? control(*index) : 0.0F;
}

json::Value face_schema() {
    return file_schema();
}

json::Value inline_face_schema() {
    json::Value schema = inline_schema();
    schema.as_object().erase("$defs");
    return schema;
}

std::vector<json::ValidationError> validate_face(const json::Value& doc) {
    return json::validate(doc, file_schema());
}

Result<CharacterFace> parse_face(const json::Value& doc, const FileReader& read_file) {
    std::vector<std::string> chain;
    return parse_resolved(doc, read_file, chain);
}

FaceEyes face_eyes(const CharacterFace& face, const HeadShape& head) {
    return Sculptor(face, head).eyes();
}

real_t face_mouth(const CharacterFace& face, const HeadShape& head) {
    const Sculptor sculptor(face, head);
    return sculptor.meters_of(sculptor.shape(front_at(0.0F, sculptor.mouth_y()))).y;
}

const std::vector<std::string_view>& face_expression_names() {
    return kExpressions;
}

std::optional<std::size_t> face_expression_index(std::string_view name) {
    for (std::size_t i = 0; i < kExpressions.size(); ++i) {
        if (kExpressions[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::vector<Vec3>> face_expressions(const CharacterFace& face, const HeadShape& head,
                                                const SculptedHead& sculpt) {
    const Sculptor sculptor(face, head);
    std::vector<std::vector<Vec3>> out(kExpressions.size());
    for (std::size_t e = 0; e < kExpressions.size(); ++e) {
        out[e].reserve(sculpt.positions.size());
        for (std::size_t v = 0; v < sculpt.positions.size(); ++v) {
            out[e].push_back(sculptor.expression(e, sculpt.directions[v], sculpt.positions[v]));
        }
    }
    return out;
}

Vec3 face_jaw_offset(const CharacterFace& face, const HeadShape& head, std::size_t expression,
                     Vec3 point) {
    if (expression >= kExpressions.size()) {
        return {};
    }
    return Sculptor(face, head).jaw_motion(kExpressions[expression], point);
}

SculptedHead sculpt_head(const CharacterFace& face, const HeadShape& head) {
    const Sculptor sculptor(face, head);
    const Style& style = sculptor.style();
    SculptedHead out;
    out.rings = style.rings;
    out.segments = style.segments;
    out.flat = style.flat;
    out.eyes = sculptor.eyes();
    out.mouth = face_mouth(face, head);
    const real_t seam = sculptor.mouth_latitude();
    const std::vector<real_t> lat = latitudes(style.rings, seam);
    const std::size_t columns = style.segments + 1;
    std::vector<int> hidden;
    const std::size_t count = (style.rings + 1) * columns;
    out.positions.reserve(count);
    out.directions.reserve(count);
    out.uvs.reserve(count);
    hidden.reserve(count);
    for (std::size_t i = 0; i <= style.rings; ++i) {
        const Vec2f ring = cos_sin(to_float(lat[i]));
        const bool on_seam = lat[i] == seam;
        for (std::size_t j = 0; j < columns; ++j) {
            const real_t t =
                ((2.0F * static_cast<real_t>(j)) / static_cast<real_t>(style.segments)) - 1.0F;
            const Vec2f around = cos_sin(to_float(kPi * gather(t, 1.5F)));
            const Vec3 d = unit({static_cast<real_t>(around.y) * static_cast<real_t>(ring.x),
                                 static_cast<real_t>(ring.y),
                                 static_cast<real_t>(around.x) * static_cast<real_t>(ring.x)});
            int tucked = 0;
            out.positions.push_back(sculptor.place(d, on_seam, 0.9F, &tucked));
            out.directions.push_back(d);
            out.uvs.push_back({static_cast<real_t>(j) / static_cast<real_t>(style.segments),
                               1.0F - (static_cast<real_t>(i) / static_cast<real_t>(style.rings))});
            hidden.push_back(tucked);
        }
    }
    // Triangles, as Godot's sphere winds them: the row above first, then round from the back.
    const auto at = [&](std::size_t i, std::size_t j) {
        return static_cast<std::uint32_t>((i * columns) + j);
    };
    // The seam's tucked corners split in two: the upper lip keeps the grid's, the lower lip
    // takes copies after it, so the jaw parts them. Their directions are nudged off the seam,
    // so expressions and weights tell the two lips apart.
    const auto seam_ring =
        static_cast<std::size_t>(std::find(lat.begin(), lat.end(), seam) - lat.begin());
    std::vector<std::uint32_t> lower(columns, UINT32_MAX);
    out.seam.assign(out.positions.size(), 0);
    if (seam_ring > 0 && seam_ring < style.rings) {
        for (std::size_t j = 0; j < columns; ++j) {
            const std::uint32_t upper = at(seam_ring, j);
            if (hidden[upper] != 2) {
                continue;
            }
            const Vec3 d = out.directions[upper];
            lower[j] = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(out.positions[upper]);
            out.directions.push_back(unit(d + Vec3{0.0F, -0.025F, 0.0F}));
            out.uvs.push_back(out.uvs[upper]);
            hidden.push_back(2);
            out.seam.push_back(2);
            out.directions[upper] = unit(d + Vec3{0.0F, 0.01F, 0.0F});
            out.seam[upper] = 1;
        }
    }
    const auto below_seam = [&](std::size_t i, std::size_t j) {
        return i == seam_ring && lower[j] != UINT32_MAX ? lower[j] : at(i, j);
    };
    // A quad's two triangles take the same part, by how many of its corners the mouth's seam
    // tucked in, so the lips' line inside is one band rather than teeth of two colors.
    const auto triangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, int tucked_mouth) {
        const Vec3 da = out.directions[a];
        const Vec3 db = out.directions[b];
        const Vec3 dc = out.directions[c];
        const Vec3 middle = unit(da + db + dc);
        FaceArea area = sculptor.area(middle);
        if (tucked_mouth >= 2) {
            area = FaceArea::Mouth;
        } else if (tucked_mouth == 1) {
            area = FaceArea::Lips;
        } else if (hidden[a] == 1 || hidden[b] == 1 || hidden[c] == 1) {
            area = FaceArea::Hidden;
        }
        out.indices.insert(out.indices.end(), {a, b, c});
        out.areas.push_back(area);
        out.bearded.push_back(area == FaceArea::Skin && sculptor.bearded(middle) ? std::uint8_t{1}
                                                                                 : std::uint8_t{0});
    };
    out.indices.reserve(style.rings * style.segments * 6);
    for (std::size_t i = 0; i < style.rings; ++i) {
        for (std::size_t j = 0; j < style.segments; ++j) {
            const std::uint32_t up_a = below_seam(i + 1, j);
            const std::uint32_t up_b = below_seam(i + 1, j + 1);
            const std::uint32_t low_a = at(i, j);
            const std::uint32_t low_b = at(i, j + 1);
            int tucked_mouth = 0;
            for (const std::uint32_t corner : {up_a, up_b, low_a, low_b}) {
                tucked_mouth += hidden[corner] == 2 ? 1 : 0;
            }
            if (i + 1 < style.rings) {
                triangle(up_a, up_b, low_a, tucked_mouth);
            }
            if (i > 0) {
                triangle(up_b, low_b, low_a, tucked_mouth);
            }
        }
    }
    // Normals: each vertex's triangles' (their corners turn clockwise seen from outside),
    // weighted by area, the seam's two columns sharing theirs.
    std::vector<Vec3> sums(out.positions.size());
    for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        const Vec3 a = out.positions[out.indices[t]];
        const Vec3 b = out.positions[out.indices[t + 1]];
        const Vec3 c = out.positions[out.indices[t + 2]];
        const Vec3 n = cross3(c - a, b - a);
        for (std::size_t k = 0; k < 3; ++k) {
            sums[out.indices[t + k]] = sums[out.indices[t + k]] + n;
        }
    }
    for (std::size_t i = 0; i <= style.rings; ++i) {
        const Vec3 both = sums[at(i, 0)] + sums[at(i, style.segments)];
        sums[at(i, 0)] = both;
        sums[at(i, style.segments)] = both;
    }
    for (const std::size_t pole : {std::size_t{0}, style.rings}) {
        Vec3 all;
        for (std::size_t j = 0; j < columns; ++j) {
            all = all + sums[at(pole, j)];
        }
        for (std::size_t j = 0; j < columns; ++j) {
            sums[at(pole, j)] = all;
        }
    }
    out.normals.reserve(sums.size());
    for (std::size_t v = 0; v < sums.size(); ++v) {
        out.normals.push_back(length3(sums[v]) > 0.0F ? unit(sums[v]) : out.directions[v]);
    }
    if (style.flat) {
        // Low poly: each triangle's own corners, facing its way.
        SculptedHead flat = out;
        flat.positions.clear();
        flat.normals.clear();
        flat.directions.clear();
        flat.uvs.clear();
        flat.seam.clear();
        flat.indices.clear();
        for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
            const Vec3 a = out.positions[out.indices[t]];
            const Vec3 b = out.positions[out.indices[t + 1]];
            const Vec3 c = out.positions[out.indices[t + 2]];
            const Vec3 n = unit(cross3(c - a, b - a));
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t index = out.indices[t + k];
                flat.indices.push_back(static_cast<std::uint32_t>(flat.positions.size()));
                flat.positions.push_back(out.positions[index]);
                flat.normals.push_back(n);
                flat.directions.push_back(out.directions[index]);
                flat.uvs.push_back(out.uvs[index]);
                flat.seam.push_back(out.seam[index]);
            }
        }
        out = std::move(flat);
    }
    // Landmarks where the sculpt put them, facing the way the surface does there.
    out.landmarks.reserve(kLandmarks.size());
    for (const std::string_view name : kLandmarks) {
        const Vec3 d = sculptor.landmark_direction(name);
        const Vec3 p = sculptor.place(d, false, 1.0F, nullptr);
        Vec3 normal = unit(p - head.center);
        if (name.starts_with("eye_")) {
            const Vec3 e = name == "eye_left" ? out.eyes.left : out.eyes.right;
            out.landmarks.push_back({.name = name,
                                     .position = e + Vec3{0.0F, 0.0F, out.eyes.radius},
                                     .normal = {0.0F, 0.0F, 1.0F}});
            continue;
        }
        const Vec3 across = unit(cross3({0.0F, 1.0F, 0.0F}, d));
        const Vec3 up = unit(cross3(d, across));
        constexpr real_t kStep = 0.01F;
        const Vec3 pa = sculptor.place(unit(d + (across * kStep)), false, 1.0F, nullptr);
        const Vec3 pb = sculptor.place(unit(d + (up * kStep)), false, 1.0F, nullptr);
        const Vec3 n = cross3(pa - p, pb - p);
        if (length3(n) > 0.0F) {
            normal = unit(n);
            if (dot3(normal, p - head.center) < 0.0F) {
                normal = normal * -1.0F;
            }
        }
        out.landmarks.push_back({.name = name, .position = p, .normal = normal});
    }
    return out;
}

FaceProfile face_profile(const CharacterFace& face, const HeadShape& head,
                         std::span<const float> expressions) {
    const Sculptor sculptor(face, head);
    FaceProfile out;
    // A point at a direction, moved by the expressions (the jaw's turn is its bone's).
    const auto moved = [&](Vec3 d, Vec3 p) {
        Vec3 sum = p;
        for (std::size_t e = 0; e < expressions.size() && e < kExpressions.size(); ++e) {
            if (expressions[e] != 0.0F && kExpressions[e] != "jawOpen") {
                sum = sum + (sculptor.expression(e, d, p) * static_cast<real_t>(expressions[e]));
            }
        }
        return sum;
    };
    const auto weight = [&](std::string_view name) -> real_t {
        const auto index = face_expression_index(name);
        return index && *index < expressions.size() ? static_cast<real_t>(expressions[*index])
                                                    : 0.0F;
    };
    const real_t seam = sculptor.mouth_latitude();
    std::vector<real_t> lat = latitudes(48, seam);
    // The lips' edges as points of their own, so the profile shows them.
    for (const real_t y : {sculptor.mouth_y() + 0.045F, sculptor.mouth_y() - 0.055F}) {
        lat.push_back(asin_of(y));
    }
    std::ranges::sort(lat);
    lat.erase(std::ranges::unique(lat).begin(), lat.end());
    const auto side_view = [&](Vec3 p) { return Vec2{p.z - head.center.z, p.y - head.center.y}; };
    for (const real_t l : lat) {
        const Vec2f ring = cos_sin(to_float(l));
        const Vec3 d{0.0F, static_cast<real_t>(ring.y), static_cast<real_t>(ring.x)};
        if (l == seam) {
            out.mouth_index = out.points.size();
        }
        out.points.push_back(side_view(moved(d, sculptor.place(d, l == seam, 0.965F, nullptr))));
    }
    out.front_count = out.points.size();
    for (auto it = lat.rbegin() + 1; it != lat.rend(); ++it) {
        const Vec2f ring = cos_sin(to_float(*it));
        const Vec3 d{0.0F, static_cast<real_t>(ring.y), -static_cast<real_t>(ring.x)};
        out.points.push_back(side_view(moved(d, sculptor.place(d, false, 1.0F, nullptr))));
    }
    // The bottom's point was the first; leave its copy at the end off.
    out.points.pop_back();
    const FaceEyes eyes = sculptor.eyes();
    out.eye = {eyes.left.z - head.center.z + (eyes.radius * 0.45F), eyes.left.y - head.center.y};
    out.eye_radius = eyes.radius * 0.7F;
    const Vec3 brow = sculptor.landmark_direction("brow_left");
    out.brow = side_view(moved(brow, sculptor.place(brow, false, 1.0F, nullptr)));
    out.eye_open = std::clamp<real_t>(
        1.0F - (0.95F * weight("eyeBlinkLeft")) + (0.3F * weight("eyeWideLeft")) -
            (0.35F * weight("eyeSquintLeft")) - (0.25F * weight("eyeLookDownLeft")),
        0.0F, 1.3F);
    const Vec3 ear = sculptor.place(sculptor.landmark_direction("ear_left"), false, 1.0F, nullptr);
    out.ear = side_view(ear);
    return out;
}

FaceSkinMaps face_skin_maps(const CharacterFace& face, const HeadShape& head, Vec4 skin) {
    const SkinStyle style = skin_style(face.style);
    const auto pick_share = [](float own, real_t style_share) {
        return own >= 0.0F ? clamp01(own) : style_share;
    };
    const real_t pores = pick_share(face.skin.pores, style.pores);
    const real_t blotches = pick_share(face.skin.blotches, style.blotches);
    const real_t oil = pick_share(face.skin.oil, style.oil);
    const real_t wrinkles = clamp01(face.skin.wrinkles);
    FaceSkinMaps out;
    if (pores + blotches + oil + wrinkles <= 0.0F && face.marks.empty()) {
        return out;
    }
    const Sculptor sculptor(face, head);
    const Style& sculpt_style = sculptor.style();
    const Sculptor::Features features = sculptor.features();
    const std::vector<PlacedMark> marks = place_marks(face, sculptor);
    out.width = face.skin.resolution != 0 ? face.skin.resolution : style.width;
    out.height = out.width / 2;
    const std::size_t w = out.width;
    const std::size_t h = out.height;
    // Each column's turn round the head and each row's latitude, as the sculpt's uvs lay them.
    const std::vector<real_t> lat = latitudes(sculpt_style.rings, sculptor.mouth_latitude());
    std::vector<Vec2f> around(w);
    for (std::size_t x = 0; x < w; ++x) {
        const real_t u = (static_cast<real_t>(x) + 0.5F) / static_cast<real_t>(w);
        around[x] = cos_sin(to_float(kPi * gather((2.0F * u) - 1.0F, 1.5F)));
    }
    std::vector<Vec2f> rows(h);
    for (std::size_t y = 0; y < h; ++y) {
        const real_t v = (static_cast<real_t>(y) + 0.5F) / static_cast<real_t>(h);
        const real_t ring = (1.0F - v) * static_cast<real_t>(sculpt_style.rings);
        const auto below = std::min(static_cast<std::size_t>(ring), sculpt_style.rings - 1);
        const real_t t = ring - static_cast<real_t>(below);
        rows[y] = cos_sin(to_float(lat[below] + ((lat[below + 1] - lat[below]) * t)));
    }
    const auto direction = [&](std::size_t x, std::size_t y) {
        return Vec3{around[x].y * rows[y].x, rows[y].y, around[x].x * rows[y].x};
    };
    // A direction between texels' centers, for sampling the lips' outline finer than texels.
    const auto direction_at = [&](real_t fx, real_t fy) {
        const real_t u = fx / static_cast<real_t>(w);
        const Vec2f a = cos_sin(to_float(kPi * gather((2.0F * u) - 1.0F, 1.5F)));
        const real_t ring =
            (1.0F - (fy / static_cast<real_t>(h))) * static_cast<real_t>(sculpt_style.rings);
        const auto below = std::min(static_cast<std::size_t>(std::max<real_t>(ring, 0.0F)),
                                    sculpt_style.rings - 1);
        const real_t t = ring - static_cast<real_t>(below);
        const Vec2f r = cos_sin(to_float(lat[below] + ((lat[below + 1] - lat[below]) * t)));
        return Vec3{static_cast<real_t>(a.y) * static_cast<real_t>(r.x), static_cast<real_t>(r.y),
                    static_cast<real_t>(a.x) * static_cast<real_t>(r.x)};
    };
    // How much of a texel the lips cover, from 16 samples, so their outline is smooth however
    // coarse the sculpt's triangles are.
    const auto lip_cover = [&](std::size_t x, std::size_t y, Vec3 d) -> real_t {
        if (d.z <= 0.0F || std::abs(d.x) > features.mouth_w * 1.1F ||
            std::abs(d.y - features.mouth_y) > 0.16F) {
            return 0.0F;
        }
        int inside = 0;
        for (int sy = 0; sy < 4; ++sy) {
            for (int sx = 0; sx < 4; ++sx) {
                const Vec3 at = direction_at(
                    static_cast<real_t>(x) + ((static_cast<real_t>(sx) + 0.5F) / 4.0F),
                    static_cast<real_t>(y) + ((static_cast<real_t>(sy) + 0.5F) / 4.0F));
                inside += sculptor.area(at) == FaceArea::Lips ? 1 : 0;
            }
        }
        return static_cast<real_t>(inside) / 16.0F;
    };
    // The lips' inner walls, the sculpt's quads beside the seam whose corners on it are tucked
    // in: lip colored at the lips, darkening into the mouth.
    const auto seam_ring = static_cast<real_t>(
        std::find(lat.begin(), lat.end(), sculptor.mouth_latitude()) - lat.begin());
    std::vector<std::uint8_t> tucked(sculpt_style.segments + 1, 0);
    {
        const Vec2f ring = cos_sin(to_float(sculptor.mouth_latitude()));
        for (std::size_t j = 0; j <= sculpt_style.segments; ++j) {
            const real_t t =
                ((2.0F * static_cast<real_t>(j)) / static_cast<real_t>(sculpt_style.segments)) -
                1.0F;
            const Vec2f a = cos_sin(to_float(kPi * gather(t, 1.5F)));
            int hidden = 0;
            static_cast<void>(
                sculptor.place(unit({static_cast<real_t>(a.y) * static_cast<real_t>(ring.x),
                                     static_cast<real_t>(ring.y),
                                     static_cast<real_t>(a.x) * static_cast<real_t>(ring.x)}),
                               true, 0.9F, &hidden));
            tucked[j] = hidden == 2 ? 1 : 0;
        }
    }
    const auto mouth_wall = [&](std::size_t x, std::size_t y) -> real_t {
        const real_t ring = (1.0F - ((static_cast<real_t>(y) + 0.5F) / static_cast<real_t>(h))) *
                            static_cast<real_t>(sculpt_style.rings);
        const real_t near = 1.0F - std::abs(ring - seam_ring);
        if (near <= 0.0F) {
            return 0.0F;
        }
        const auto j = std::min(
            static_cast<std::size_t>(((static_cast<real_t>(x) + 0.5F) / static_cast<real_t>(w)) *
                                     static_cast<real_t>(sculpt_style.segments)),
            sculpt_style.segments - 1);
        return tucked[j] != 0 || tucked[j + 1] != 0 ? near : 0.0F;
    };
    constexpr Vec4 kInside{0.25F, 0.08F, 0.08F, 1.0F};
    std::vector<real_t> heights(w * h, 0.0F);
    out.albedo.resize(w * h * 4);
    out.roughness.resize(w * h * 4);
    const auto byte = [](real_t value) {
        return static_cast<std::uint8_t>(std::lround(clamp01(value) * 255.0F));
    };
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const Vec3 d = direction(x, y);
            Vec4 color = skin;
            real_t height = 0.0F;
            real_t rough = 0.6F - (0.28F * oil * shine_share(features, d));
            if (blotches > 0.0F) {
                const real_t broad = value_noise(d * 7.0F, face.seed + 101U) - 0.5F;
                const real_t fine = value_noise(d * 24.0F, face.seed + 202U) - 0.5F;
                const real_t flush =
                    blotches * ((0.6F * flush_share(features, d)) + (0.5F * broad));
                const real_t light = blotches * 0.06F * fine;
                color.x += (0.07F * flush) + light;
                color.y += (-0.035F * flush) + light;
                color.z += (-0.03F * flush) + light;
            }
            if (pores > 0.0F && d.z > -0.5F) {
                const real_t where =
                    0.5F + (0.5F * std::max(flush_share(features, d), shine_share(features, d)));
                const real_t pit = smooth(0.55F, 0.85F, value_noise(d * 320.0F, face.seed + 7U));
                height -= pores * where * 0.08e-3F * pit;
                rough += pores * 0.05F * pit;
            }
            if (wrinkles > 0.0F) {
                height -= wrinkles * 0.45e-3F * wrinkle_depth(features, d) * sculpt_style.fine;
            }
            // The lips: their color, shine and the fine lines that run up them.
            if (const real_t lip = lip_cover(x, y, d); lip > 0.0F) {
                color = mix4(color, face.lips, lip);
                rough += (0.4F - rough) * lip;
                const Vec2f line = cos_sin(to_float(d.x * 260.0F));
                height -= lip * pores * 0.05e-3F * (0.5F + (0.5F * static_cast<real_t>(line.x)));
            }
            if (const real_t wall = mouth_wall(x, y); wall > 0.0F) {
                color = mix4(face.lips, kInside, smooth(0.3F, 1.0F, wall));
                rough = 0.45F;
            }
            for (const PlacedMark& mark : marks) {
                Vec2 local;
                if (!mark_coords(mark, d, local)) {
                    continue;
                }
                const MarkTexel hit = mark_texel(mark, local, d, style);
                if (hit.cover > 0.0F) {
                    color = mix4(color, hit.color, clamp01(hit.cover));
                }
                height += hit.height;
                rough += (hit.roughness - rough) * clamp01(hit.shine);
            }
            const std::size_t at = ((y * w) + x) * 4;
            out.albedo[at] = byte(color.x);
            out.albedo[at + 1] = byte(color.y);
            out.albedo[at + 2] = byte(color.z);
            out.albedo[at + 3] = 255;
            out.roughness[at] = byte(rough);
            out.roughness[at + 1] = out.roughness[at];
            out.roughness[at + 2] = out.roughness[at];
            out.roughness[at + 3] = 255;
            heights[(y * w) + x] = height;
        }
    }
    // The relief's slopes round the head (its tangent) and up it (its binormal), by how far apart
    // the neighboring texels are on the head.
    const auto place = [&](std::size_t x, std::size_t y) {
        const Vec3 d = direction(x, y);
        return Vec3{d.x * head.radii.x, d.y * head.radii.y, d.z * head.radii.z};
    };
    out.normal.resize(w * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        const std::size_t up = y == 0 ? 0 : y - 1;
        const std::size_t down = std::min(y + 1, h - 1);
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t left = (x + w - 1) % w;
            const std::size_t right = (x + 1) % w;
            const real_t across =
                std::max<real_t>(length3(place(right, y) - place(left, y)), 1e-6F);
            const real_t along = std::max<real_t>(length3(place(x, up) - place(x, down)), 1e-6F);
            const real_t tx = (heights[(y * w) + right] - heights[(y * w) + left]) / across;
            const real_t ty = (heights[(up * w) + x] - heights[(down * w) + x]) / along;
            const Vec3 n = unit({-tx, -ty, 1.0F});
            const std::size_t at = ((y * w) + x) * 4;
            out.normal[at] = byte((n.x * 0.5F) + 0.5F);
            out.normal[at + 1] = byte((n.y * 0.5F) + 0.5F);
            out.normal[at + 2] = byte((n.z * 0.5F) + 0.5F);
            out.normal[at + 3] = 255;
        }
    }
    return out;
}

std::vector<FaceMarkShape> face_marks_2d(const CharacterFace& face, const HeadShape& head) {
    std::vector<FaceMarkShape> out;
    if (face.marks.empty()) {
        return out;
    }
    const Sculptor sculptor(face, head);
    const Sculptor::Features features = sculptor.features();
    // How far forward the profile's face reaches at a height, so marks near its edge stay on
    // it rather than sticking out past the outline.
    const FaceProfile profile = face_profile(face, head);
    const auto front_at = [&profile](real_t y) {
        real_t most = -1e9F;
        for (std::size_t i = 0; i + 1 < profile.front_count; ++i) {
            const Vec2 a = profile.points[i];
            const Vec2 b = profile.points[i + 1];
            if ((y - a.y) * (y - b.y) > 0.0F || a.y == b.y) {
                continue;
            }
            most = std::max(most, a.x + ((b.x - a.x) * (y - a.y) / (b.y - a.y)));
        }
        return most > -1e8F ? most - 0.0005F : 1e9F;
    };
    for (const PlacedMark& m : place_marks(face, sculptor)) {
        if (m.center.x < -0.2F) {
            continue; // on the far side
        }
        const FaceMark& mark = *m.mark;
        const Vec3 at = sculptor.place(m.center, false, 1.0F, nullptr);
        const Vec2 middle{at.z - head.center.z, at.y - head.center.y};
        // The mark's frame as the side view sees it, foreshortened.
        const Vec2 across{m.across.z * head.radii.z, m.across.y * head.radii.y};
        const Vec2 upward{m.up.z * head.radii.z, m.up.y * head.radii.y};
        const auto side = [&](Vec2 p) {
            const real_t ax = ((m.turn.x * p.x) - (m.turn.y * p.y)) * m.flip;
            const real_t ay = (m.turn.y * p.x) + (m.turn.x * p.y);
            return middle + (across * ax) + (upward * ay);
        };
        const bool on_jaw = m.center.y < features.mouth_y - 0.03F && m.center.z > 0.0F;
        const real_t amount = clamp01(mark.amount);
        const auto shape = [&](std::vector<Vec2> points, Vec4 color, real_t alpha) {
            for (Vec2& point : points) {
                point = side(point);
                point.x = std::min(point.x, front_at(point.y));
            }
            color.w = alpha;
            out.push_back({.points = std::move(points), .color = color, .on_jaw = on_jaw});
        };
        const auto ellipse = [](Vec2 c, real_t rx, real_t ry, int count) {
            std::vector<Vec2> points;
            points.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const Vec2f t = cos_sin(2.0F * std::numbers::pi_v<float> * static_cast<float>(i) /
                                        static_cast<float>(count));
                points.push_back(
                    {c.x + (rx * static_cast<real_t>(t.x)), c.y + (ry * static_cast<real_t>(t.y))});
            }
            return points;
        };
        // A strip of half width along a run of points.
        const auto strip = [](const std::vector<Vec2>& run, const std::vector<real_t>& widths) {
            std::vector<Vec2> points;
            points.reserve(run.size() * 2);
            std::vector<Vec2> lower;
            lower.reserve(run.size());
            for (std::size_t i = 0; i < run.size(); ++i) {
                const Vec2 a = run[i == 0 ? 0 : i - 1];
                const Vec2 b = run[std::min(i + 1, run.size() - 1)];
                const Vec2 way = b - a;
                const real_t len =
                    std::max<real_t>(std::sqrt((way.x * way.x) + (way.y * way.y)), 1e-9F);
                const Vec2 normal{-way.y / len, way.x / len};
                points.push_back(run[i] + (normal * widths[i]));
                lower.push_back(run[i] - (normal * widths[i]));
            }
            points.insert(points.end(), lower.rbegin(), lower.rend());
            return points;
        };
        const auto line = [&](real_t width) {
            std::vector<Vec2> run;
            std::vector<real_t> widths;
            for (int i = 0; i <= 8; ++i) {
                const real_t t = (static_cast<real_t>(i) / 4.0F) - 1.0F;
                run.push_back({t * m.half, 0.0F});
                widths.push_back(width * (1.0F - (0.7F * t * t)));
            }
            return strip(run, widths);
        };
        const real_t s = m.size;
        const std::string& kind = mark.kind;
        const real_t age = clamp01(mark.age);
        if (kind == "mole") {
            shape(ellipse({}, s * 0.75F, s * 0.75F, 10), m.color, amount);
        } else if (kind == "freckles") {
            constexpr real_t kCell = 0.014F;
            const auto cells = static_cast<std::int64_t>(std::ceil(s / kCell));
            for (std::int64_t gx = -cells; gx <= cells; ++gx) {
                for (std::int64_t gy = -cells; gy <= cells; ++gy) {
                    if (lattice(gx, gy, 0, mark.seed) > 0.25F + (0.6F * amount)) {
                        continue;
                    }
                    const Vec2 c{(static_cast<real_t>(gx) + lattice(gx, gy, 1, mark.seed)) * kCell,
                                 (static_cast<real_t>(gy) + lattice(gx, gy, 2, mark.seed)) * kCell};
                    if ((c.x * c.x) + (c.y * c.y) > s * s * 0.64F) {
                        continue;
                    }
                    const real_t radius = 0.0028F + (0.0025F * lattice(gx, gy, 3, mark.seed));
                    shape(ellipse(c, radius, radius, 6), m.color, 0.75F);
                }
            }
        } else if (kind == "scar" || kind == "wound") {
            if (kind == "wound") {
                shape(line(s * 2.0F), {0.72F, 0.24F, 0.22F, 1.0F}, amount * 0.6F * (1.0F - age));
            }
            shape(line(s * (kind == "wound" ? 0.7F : 0.8F)), m.color, amount * 0.85F);
            if (mark.pattern == "stitched") {
                const real_t spacing = std::max<real_t>(0.018F, s * 1.8F);
                const auto stitches = static_cast<int>(std::floor(m.half / spacing));
                for (int k = -stitches; k <= stitches; ++k) {
                    const real_t x = static_cast<real_t>(k) * spacing;
                    shape({{x - 0.002F, -s * 2.2F},
                           {x + 0.002F, -s * 2.2F},
                           {x + 0.002F, s * 2.2F},
                           {x - 0.002F, s * 2.2F}},
                          {0.08F, 0.08F, 0.1F, 1.0F}, amount);
                }
            }
        } else if (kind == "burn" || kind == "birthmark" || kind == "bruise" || kind == "rash") {
            // A patch whose rim wanders as its seed says.
            std::vector<Vec2> rim;
            constexpr int kRim = 16;
            for (int i = 0; i < kRim; ++i) {
                const Vec2f t = cos_sin(2.0F * std::numbers::pi_v<float> * static_cast<float>(i) /
                                        static_cast<float>(kRim));
                const real_t wander = 0.8F + (0.4F * lattice(i, 0, 7, mark.seed));
                rim.push_back(
                    {s * wander * static_cast<real_t>(t.x), s * wander * static_cast<real_t>(t.y)});
            }
            const real_t alpha = kind == "burn" ? amount : amount * 0.65F;
            shape(rim, m.color, alpha);
            if (kind == "burn") {
                const Vec4 deep =
                    age < 0.5F ? Vec4{0.55F, 0.12F, 0.1F, 1.0F} : Vec4{0.58F, 0.38F, 0.34F, 1.0F};
                shape(ellipse({s * 0.1F, -s * 0.05F}, s * 0.45F, s * 0.35F, 10), deep,
                      amount * 0.6F);
            }
        } else {
            const real_t alpha = kind == "paint" ? amount * 0.95F : amount * (1.0F - (0.35F * age));
            const std::string_view pattern = m.pattern;
            const real_t half = m.half;
            const auto capsule = [&](Vec2 a, Vec2 b, real_t width) {
                shape(strip({a, (a + b) * 0.5F, b}, {width, width, width}), m.color, alpha);
            };
            if (pattern == "stripes") {
                for (const real_t row : {-0.45F, 0.0F, 0.45F}) {
                    capsule({-half, row * s}, {half, row * s}, 0.1F * s);
                }
            } else if (pattern == "chevron") {
                capsule({-0.6F * s, 0.35F * s}, {0.0F, -0.25F * s}, 0.1F * s);
                capsule({0.0F, -0.25F * s}, {0.6F * s, 0.35F * s}, 0.1F * s);
            } else if (pattern == "dots") {
                for (const real_t x : {-0.6F, 0.0F, 0.6F}) {
                    shape(ellipse({x * s, 0.0F}, 0.16F * s, 0.16F * s, 8), m.color, alpha);
                }
            } else if (pattern == "ring" || pattern == "spiral") {
                std::vector<Vec2> run;
                std::vector<real_t> widths;
                constexpr int kSteps = 48;
                for (int i = 0; i <= kSteps; ++i) {
                    const real_t t = static_cast<real_t>(i) / static_cast<real_t>(kSteps);
                    const real_t turn = pattern == "ring" ? t : t * 2.5F;
                    const real_t radius =
                        pattern == "ring" ? 0.6F * s : (0.22F * s * turn) + (0.02F * s);
                    const Vec2f a = cos_sin(2.0F * std::numbers::pi_v<float> * to_float(turn));
                    run.push_back(
                        {radius * static_cast<real_t>(a.x), radius * static_cast<real_t>(a.y)});
                    widths.push_back((pattern == "ring" ? 0.1F : 0.06F) * s);
                }
                shape(strip(run, widths), m.color, alpha);
            } else if (pattern == "star") {
                std::vector<Vec2> points;
                for (int i = 0; i < 10; ++i) {
                    const real_t radius = s * 0.85F * (i % 2 == 0 ? 1.0F : 0.4F);
                    const Vec2f a =
                        cos_sin((std::numbers::pi_v<float> * 0.5F) +
                                (std::numbers::pi_v<float> * static_cast<float>(i) / 5.0F));
                    points.push_back(
                        {radius * static_cast<real_t>(a.x), radius * static_cast<real_t>(a.y)});
                }
                shape(points, m.color, alpha);
            } else if (pattern == "teardrop") {
                std::vector<Vec2> points = ellipse({0.0F, -0.3F * s}, 0.3F * s, 0.3F * s, 12);
                points.erase(points.begin() + 1, points.begin() + 6);
                points.insert(points.begin() + 1, Vec2{0.0F, 0.5F * s});
                shape(points, m.color, alpha);
            } else if (pattern == "cross") {
                capsule({0.0F, -0.6F * s}, {0.0F, 0.6F * s}, 0.12F * s);
                capsule({-0.6F * s, 0.0F}, {0.6F * s, 0.0F}, 0.12F * s);
            } else if (pattern == "band" || pattern == "mask" || pattern == "lines") {
                std::vector<Vec2> run;
                std::vector<real_t> widths;
                for (int i = 0; i <= 16; ++i) {
                    const real_t t = (static_cast<real_t>(i) / 8.0F) - 1.0F;
                    const real_t wave =
                        pattern == "lines" ? 0.35F * s * cos_sin(to_float(kPi * t)).y : 0.0F;
                    run.push_back({t * half, wave});
                    real_t width = (0.14F * s * (1.0F - (t * t))) + 0.002F;
                    if (pattern == "band") {
                        width = 0.35F * s;
                    } else if (pattern == "mask") {
                        width = 0.4F * s * (1.0F - (0.55F * t * t));
                    }
                    widths.push_back(width);
                }
                shape(strip(run, widths), m.color, alpha);
            } else if (pattern == "half") {
                std::vector<Vec2> points = ellipse({}, s, s, 24);
                std::erase_if(points, [](Vec2 p) { return p.x < -1e-6F; });
                shape(points, m.color, alpha);
            }
        }
    }
    return out;
}

} // namespace slash::world
