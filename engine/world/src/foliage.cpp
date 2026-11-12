#include "slash/world/foliage.hpp"

#include "slash/world/vfx.hpp"

#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine foliage type",
  "description": "One kind of plant or rock that foliage.paint paints, after Unreal's Foliage tool: what it draws, how thickly it grows, how it bends in the wind and parts for characters, what it blocks and how far it is drawn.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "space": {"enum": ["3d", "2d"], "description": "Whether it grows in a 3D or a 2D world. Default 3d."},
    "kind": {"enum": ["grass", "flower", "shrub", "bush", "tree", "moss", "rock", "pebble"],
             "description": "What it is. Default grass."},
    "mesh": {"type": "string", "description": "3D: the model file drawn at each instance (.gltf, .glb, .obj, .fbx, .blend or .model.json)."},
    "texture": {"type": "string", "description": "2D: the image drawn at each instance."},
    "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 2, "maxItems": 2,
             "description": "2D: pixels across and up at scale 1. Default [16, 16]."},
    "origin": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2,
               "description": "2D: the point of the image on the ground, as shares of size. Default [0.5, 1], the bottom's middle."},
    "density": {"type": "number", "minimum": 0,
                "description": "Instances a brush paints per square meter (3D) or square pixel (2D) at full strength. Default 1."},
    "spacing": {"type": "number", "minimum": 0,
                "description": "The least distance between two instances of this type. Default 0."},
    "scale": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 2, "maxItems": 2,
              "description": "[min, max] each instance is scaled between. Default [1, 1]."},
    "random_yaw": {"type": "boolean", "description": "3D: each turns about its up axis at random. Default true."},
    "random_flip": {"type": "boolean", "description": "2D: each is mirrored or not at random. Default true."},
    "align_to_normal": {"type": "boolean", "description": "3D: stands along the ground's normal rather than upright. Default false."},
    "tilt": {"type": "number", "minimum": 0, "description": "3D: radians each leans at random, either way. Default 0."},
    "slope": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 90}, "minItems": 2, "maxItems": 2,
              "description": "3D: degrees from flat the ground may be, [min, max]. Default [0, 90]."},
    "height": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2,
               "description": "3D: the world heights it grows between, [min, max]. Default anywhere."},
    "sink": {"type": "number", "description": "How far into the ground it stands. Default 0."},
    "cull_distance": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 2, "maxItems": 2,
                      "description": "3D: meters from the camera at which it starts to fade and is gone. Default [0, 0], never."},
    "impostor_distance": {"type": "number", "minimum": 0,
                          "description": "3D: meters past which it draws as an impostor, a picture of it facing the camera. Default 0, never."},
    "impostor": {"type": "string", "description": "3D: the impostor's image; without one, far instances draw the model's last LOD."},
    "cast_shadow": {"enum": ["off", "on", "double_sided", "shadows_only"], "description": "3D: as MeshInstance3D's. Default on."},
    "wind": {
      "type": "object", "additionalProperties": false,
      "description": "How it bends in WindSource3Ds' and WindSource2Ds' wind.",
      "properties": {
        "bend": {"type": "number", "minimum": 0, "description": "Radians it leans per meter a second (3D) or 100 pixels a second (2D) of wind. Default 0."},
        "sway": {"type": "number", "minimum": 0, "description": "The share of its lean it rocks by. Default 0."},
        "frequency": {"type": "number", "minimum": 0, "description": "Times a second it rocks. Default 0.5."},
        "max": {"type": "number", "minimum": 0, "description": "Radians it leans at most, wind and characters together. Default 0.8."}
      }
    },
    "interaction": {
      "type": "object", "additionalProperties": false,
      "description": "How it parts for characters and bodies passing through, and springs back.",
      "properties": {
        "radius": {"type": "number", "minimum": 0, "description": "How near a body's middle has to come to push it over. Default 0, never."},
        "bend": {"type": "number", "minimum": 0, "description": "Radians a body at its foot pushes it over by. Default 0.8."},
        "stiffness": {"type": "number", "minimum": 0, "description": "How hard it springs back, per second squared. Default 40."},
        "damping": {"type": "number", "minimum": 0, "description": "How much its springing slows, per second. Default 8."}
      }
    },
    "collision": {
      "type": "object", "additionalProperties": false,
      "description": "The shape each instance blocks bodies with, in its own space and scaled with it.",
      "properties": {
        "shape": {"enum": ["none", "capsule", "sphere", "cylinder", "box", "circle", "rectangle"],
                  "description": "3D: capsule, sphere, cylinder or box; 2D: circle, rectangle or capsule. Default none."},
        "radius": {"type": "number", "exclusiveMinimum": 0, "description": "Capsule, sphere, cylinder and circle. Default 0.25."},
        "height": {"type": "number", "exclusiveMinimum": 0, "description": "Capsule and cylinder, end to end, standing up. Default 2."},
        "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 2, "maxItems": 3,
                 "description": "Box [x, y, z] or rectangle [x, y]."},
        "offset": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3,
                   "description": "Where the shape's middle is. Default: standing on the ground (half its height up)."}
      }
    },
    "climbable": {"type": "boolean", "description": "Characters may climb it; foliage.get and physics.raycast say so. Default false."},
    "harvest": {
      "type": "object", "additionalProperties": false, "required": ["item"],
      "description": "What harvesting an instance gives: foliage.harvest removes it and emits foliage_harvested.",
      "properties": {
        "item": {"type": "string", "minLength": 1, "description": "The item's name."},
        "amount": {"type": "integer", "minimum": 1, "description": "How many. Default 1."}
      }
    }
  }
})";

json::Value make_schema() {
    json::Value made = json::parse(kSchema).value();
    made["properties"]["vfx"] = vfx_hooks_schema();
    return made;
}

Vec2 vec2(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<real_t>(items[0].as_double()), static_cast<real_t>(items[1].as_double())};
}

Vec3 vec3(const json::Value& value, float missing_z) {
    const json::Array& items = value.as_array();
    return {static_cast<real_t>(items[0].as_double()), static_cast<real_t>(items[1].as_double()),
            items.size() > 2 ? static_cast<real_t>(items[2].as_double()) : missing_z};
}

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

void read(const json::Value& doc, std::string_view key, Vec2& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = vec2(*value);
    }
}

bool is_3d_shape(std::string_view shape) {
    return shape == "capsule" || shape == "sphere" || shape == "cylinder" || shape == "box";
}

bool is_2d_shape(std::string_view shape) {
    return shape == "circle" || shape == "rectangle" || shape == "capsule";
}

} // namespace

bool is_foliage_path(std::string_view path) {
    return path.ends_with(kFoliageSuffix);
}

json::Value foliage_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

std::vector<json::ValidationError> validate_foliage(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, foliage_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* space = doc.find("space");
    const bool flat = space != nullptr && space->as_string() == "2d";
    if (!flat && doc.find("mesh") == nullptr) {
        errors.push_back({"/mesh", "a 3D foliage type needs a mesh to draw"});
    }
    if (flat && doc.find("texture") == nullptr) {
        errors.push_back({"/texture", "a 2D foliage type needs a texture to draw"});
    }
    for (const std::string_view range : {"scale", "slope", "height", "cull_distance"}) {
        if (const json::Value* value = doc.find(range); value != nullptr) {
            const Vec2 pair = vec2(*value);
            if (pair.x > pair.y) {
                errors.push_back(
                    {"/" + std::string(range), "expected [min, max] with min at most max"});
            }
        }
    }
    if (const json::Value* collision = doc.find("collision"); collision != nullptr) {
        if (const json::Value* shape = collision->find("shape"); shape != nullptr) {
            const std::string& name = shape->as_string();
            if (name != "none" && (flat ? !is_2d_shape(name) : !is_3d_shape(name))) {
                errors.push_back(
                    {"/collision/shape", "\"" + name + "\" isn't a " + (flat ? "2D" : "3D") +
                                             " shape: use " +
                                             (flat ? "circle, rectangle or capsule"
                                                   : "capsule, sphere, cylinder or box")});
            }
        }
    }
    return errors;
}

Result<FoliageType> parse_foliage(const json::Value& doc) {
    const std::vector<json::ValidationError> errors = validate_foliage(doc);
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    FoliageType type;
    read(doc, "space", type.space);
    read(doc, "kind", type.kind);
    read(doc, "mesh", type.mesh);
    read(doc, "texture", type.texture);
    read(doc, "size", type.size);
    read(doc, "origin", type.origin);
    read(doc, "density", type.density);
    read(doc, "spacing", type.spacing);
    read(doc, "scale", type.scale);
    read(doc, "random_yaw", type.random_yaw);
    read(doc, "random_flip", type.random_flip);
    read(doc, "align_to_normal", type.align_to_normal);
    read(doc, "tilt", type.tilt);
    read(doc, "slope", type.slope);
    if (const json::Value* height = doc.find("height"); height != nullptr) {
        type.height = vec2(*height);
    }
    read(doc, "sink", type.sink);
    read(doc, "cull_distance", type.cull_distance);
    read(doc, "impostor_distance", type.impostor_distance);
    read(doc, "impostor", type.impostor);
    read(doc, "cast_shadow", type.cast_shadow);
    if (const json::Value* wind = doc.find("wind"); wind != nullptr) {
        read(*wind, "bend", type.wind_bend);
        read(*wind, "sway", type.sway);
        read(*wind, "frequency", type.sway_frequency);
        read(*wind, "max", type.max_bend);
    }
    if (const json::Value* interaction = doc.find("interaction"); interaction != nullptr) {
        read(*interaction, "radius", type.interaction_radius);
        read(*interaction, "bend", type.interaction_bend);
        read(*interaction, "stiffness", type.stiffness);
        read(*interaction, "damping", type.damping);
    }
    if (const json::Value* collision = doc.find("collision"); collision != nullptr) {
        FoliageCollision& shape = type.collision;
        read(*collision, "shape", shape.shape);
        read(*collision, "radius", shape.radius);
        read(*collision, "height", shape.height);
        if (const json::Value* size = collision->find("size"); size != nullptr) {
            shape.size = vec3(*size, 1.0F);
        }
        const bool flat = type.space == "2d";
        if (const json::Value* offset = collision->find("offset"); offset != nullptr) {
            shape.offset = vec3(*offset, 0.0F);
        } else {
            // Standing on the ground: up is +y in 3D and -y (up the screen) in 2D.
            float up = 0.0F;
            if (shape.shape == "capsule" || shape.shape == "cylinder") {
                up = shape.height * 0.5F;
            } else if (shape.shape == "sphere" || shape.shape == "circle") {
                up = shape.radius;
            } else if (shape.shape == "box" || shape.shape == "rectangle") {
                up = static_cast<float>(shape.size.y) * 0.5F;
            }
            shape.offset = flat ? Vec3{0.0F, -up, 0.0F} : Vec3{0.0F, up, 0.0F};
        }
    }
    read(doc, "climbable", type.climbable);
    if (const json::Value* harvest = doc.find("harvest"); harvest != nullptr) {
        read(*harvest, "item", type.harvest_item);
        if (const json::Value* amount = harvest->find("amount"); amount != nullptr) {
            type.harvest_amount = static_cast<int>(amount->as_int());
        }
    }
    type.vfx = read_vfx_hooks(doc);
    return type;
}

} // namespace slash::world
