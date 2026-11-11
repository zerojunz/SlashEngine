#include "slash/world/ragdoll.hpp"

#include "slash/core/json_schema.hpp"

#include <string>
#include <vector>

namespace slash::world {

namespace {

// The members a bone (and the file's defaults) may have.
constexpr std::string_view kBoneSchema = R"({
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "shape": {"enum": ["capsule", "sphere", "box", "circle", "rectangle"],
              "description": "3D: capsule, sphere or box; 2D: capsule, circle or rectangle."},
    "radius": {"type": "number", "exclusiveMinimum": 0,
               "description": "A capsule's, sphere's or circle's radius (meters; 2D pixels)."},
    "height": {"type": "number", "exclusiveMinimum": 0,
               "description": "A capsule's height, end to end, along the body's y."},
    "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0},
             "minItems": 2, "maxItems": 3, "description": "A box's or rectangle's size."},
    "offset": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3,
               "description": "Where the body is in the bone's space."},
    "rotation": {"anyOf": [{"type": "number"},
                           {"type": "array", "items": {"type": "number"},
                            "minItems": 3, "maxItems": 3}],
                 "description": "How the body is turned in the bone's space, radians: x, y and z as Transform3D's, or one angle in 2D."},
    "mass": {"type": "number", "exclusiveMinimum": 0, "description": "Kilograms."},
    "joint": {"enum": ["none", "pin", "cone", "hinge"],
              "description": "What holds the body to the body of the bone above it."},
    "joint_rotation": {"type": "array", "items": {"type": "number"},
                       "minItems": 3, "maxItems": 3,
                       "description": "The joint's axes in the body's space, radians."},
    "swing_span": {"type": "number", "minimum": 0,
                   "description": "How far a cone joint swings from its x axis, radians."},
    "twist_span": {"type": "number", "minimum": 0,
                   "description": "How far a cone joint twists about its x axis, radians."},
    "lower": {"type": "number", "description": "A hinge's lowest angle, radians."},
    "upper": {"type": "number", "description": "A hinge's highest angle, radians."},
    "drive": {"enum": ["off", "local", "world"],
              "description": "Which pose the muscle turns the body toward: the animated one in its parent bone's body (local) or in the world."},
    "stiffness": {"type": "number", "minimum": 0,
                  "description": "How hard the muscle turns the body toward the pose, per second squared."},
    "damping": {"type": "number", "minimum": 0,
                "description": "How much the muscle slows the body's turning, per second."},
    "strength": {"type": "number", "minimum": 0,
                 "description": "A multiplier on the muscle's stiffness and damping; 0 for none."},
    "max_acceleration": {"type": "number", "minimum": 0,
                         "description": "Radians a second squared the muscle turns the body by at most; 0 for no limit."},
    "pin": {"type": "number", "minimum": 0,
            "description": "How hard the muscle pins the body to the animated pose's place in the world, so it holds its own weight, per second squared."},
    "pin_damping": {"type": "number", "minimum": 0,
                    "description": "How much the pin slows the body's moving, per second."},
    "linear_damp": {"type": "number", "minimum": 0,
                    "description": "Added to the project's linear damping."},
    "angular_damp": {"type": "number", "minimum": 0,
                     "description": "Added to the project's angular damping."},
    "physics_material": {"type": "string",
                         "description": "A .physics_material.json file for the body."}
  }
})";

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine ragdoll profile",
  "description": "The bodies, joints and muscles of a character's or creature's ragdoll, after Unreal's physics assets and physical animation, with its balance, hit reactions and getting up.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format", "bones"],
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "model": {"type": "string", "description": "The model the profile was made for; checks name its bones from it."},
    "collision_layer": {"type": "integer", "minimum": 0, "maximum": 4294967295,
                        "description": "The bodies' collision layer bits."},
    "collision_mask": {"type": "integer", "minimum": 0, "maximum": 4294967295,
                       "description": "The bodies' collision mask bits."},
    "self_collision": {"type": "boolean", "description": "Whether its bodies collide with each other; joined ones never do. Default false."},
    "defaults": {"$ref": "#/$defs/bone"},
    "bones": {"type": "object", "additionalProperties": {"$ref": "#/$defs/bone"},
              "description": "Each bone's body, joint and muscle, by bone name (2D: by entity)."},
    "balance": {
      "type": "object", "additionalProperties": false,
      "description": "How an active ragdoll keeps its balance.",
      "properties": {
        "bone": {"type": "string", "description": "The bone whose body keeps its pose in the world, usually the hips."},
        "stiffness": {"type": "number", "minimum": 0, "description": "How hard it turns toward the pose."},
        "damping": {"type": "number", "minimum": 0, "description": "How much it slows its turning."},
        "position_stiffness": {"type": "number", "minimum": 0, "description": "How hard it moves toward the pose's place."},
        "position_damping": {"type": "number", "minimum": 0, "description": "How much it slows its moving."},
        "max_tilt": {"type": "number", "minimum": 0, "description": "Radians it leans from the pose before the ragdoll falls; 0 never."}
      }
    },
    "hit": {
      "type": "object", "additionalProperties": false,
      "description": "How hits weaken muscles and knock the ragdoll down.",
      "properties": {
        "weaken": {"type": "number", "minimum": 0, "description": "Strength a hit takes from its bone for each kilogram-meter per second."},
        "spread": {"type": "number", "minimum": 0, "maximum": 1, "description": "How much of that each bone passes to the bones under it."},
        "recovery": {"type": "number", "exclusiveMinimum": 0, "description": "Seconds muscles take to get their strength back."},
        "knockdown": {"type": "number", "minimum": 0, "description": "Kilogram-meters per second of one hit that knocks the ragdoll down; 0 never."}
      }
    },
    "get_up": {
      "type": "object", "additionalProperties": false,
      "description": "How a limp ragdoll gets back up.",
      "properties": {
        "enabled": {"type": "boolean", "description": "Whether it gets up at all."},
        "settle_speed": {"type": "number", "minimum": 0, "description": "Meters a second (2D pixels) every body moves slower than once it lies still."},
        "settle_time": {"type": "number", "minimum": 0, "description": "Seconds it lies still before it gets up."},
        "face_up": {"type": "string", "description": "The animation it gets up with from lying on its back."},
        "face_down": {"type": "string", "description": "The animation it gets up with from lying on its front."},
        "blend": {"type": "number", "exclusiveMinimum": 0, "description": "Seconds it blends from the bodies to the animation."},
        "forward": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                    "description": "Which way is the front of the balance bone, in its space."}
      }
    }
  }
})";

json::Value make_schema() {
    auto schema = json::parse(kSchema);
    auto bone = json::parse(kBoneSchema);
    json::Value out = std::move(schema.value());
    json::Value defaults = bone.value();
    out["$defs"]["bone"] = std::move(defaults);
    return out;
}

Vec3 vec3(const json::Value& value) {
    const json::Array& items = value.as_array();
    Vec3 out;
    out.x = static_cast<real_t>(items[0].as_double());
    out.y = static_cast<real_t>(items[1].as_double());
    if (items.size() > 2) {
        out.z = static_cast<real_t>(items[2].as_double());
    }
    return out;
}

void read(const json::Value& value, std::string_view key, float& into) {
    if (const json::Value* member = value.find(key); member != nullptr) {
        into = static_cast<float>(member->as_double());
    }
}

void read(const json::Value& value, std::string_view key, std::string& into) {
    if (const json::Value* member = value.find(key); member != nullptr) {
        into = member->as_string();
    }
}

void read(const json::Value& value, std::string_view key, Vec3& into) {
    if (const json::Value* member = value.find(key); member != nullptr) {
        into = vec3(*member);
    }
}

RagdollBone read_bone(const json::Value& value) {
    RagdollBone bone;
    read(value, "shape", bone.shape);
    read(value, "radius", bone.radius);
    read(value, "height", bone.height);
    if (const json::Value* size = value.find("size"); size != nullptr) {
        bone.size = vec3(*size);
        if (size->size() == 2) {
            bone.size.z = bone.size.y;
        }
    }
    read(value, "offset", bone.offset);
    if (const json::Value* rotation = value.find("rotation"); rotation != nullptr) {
        if (rotation->is_number()) {
            bone.rotation = {0.0F, 0.0F, static_cast<real_t>(rotation->as_double())};
        } else {
            bone.rotation = vec3(*rotation);
        }
    }
    read(value, "mass", bone.mass);
    read(value, "joint", bone.joint);
    read(value, "joint_rotation", bone.joint_rotation);
    read(value, "swing_span", bone.swing_span);
    read(value, "twist_span", bone.twist_span);
    read(value, "lower", bone.lower);
    read(value, "upper", bone.upper);
    read(value, "drive", bone.drive);
    read(value, "stiffness", bone.stiffness);
    read(value, "damping", bone.damping);
    read(value, "strength", bone.strength);
    read(value, "max_acceleration", bone.max_acceleration);
    read(value, "pin", bone.pin);
    read(value, "pin_damping", bone.pin_damping);
    read(value, "linear_damp", bone.linear_damp);
    read(value, "angular_damp", bone.angular_damp);
    read(value, "physics_material", bone.physics_material);
    return bone;
}

} // namespace

json::Value ragdoll_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

json::Value merge_patch(const json::Value& base, const json::Value& patch) {
    if (!patch.is_object()) {
        return patch;
    }
    json::Value out = base.is_object() ? base : json::Value(json::Object{});
    for (const auto& [key, member] : patch.as_object()) {
        if (member.is_null()) {
            out.as_object().erase(key);
        } else {
            const json::Value* existing = out.find(key);
            json::Value merged =
                merge_patch(existing != nullptr ? *existing : json::Value{}, member);
            out[key] = std::move(merged);
        }
    }
    return out;
}

Result<RagdollProfile> parse_ragdoll(const json::Value& file, const json::Value& overrides) {
    const json::Value merged = overrides.is_null() ? file : merge_patch(file, overrides);
    const std::vector<json::ValidationError> errors = json::validate(merged, ragdoll_schema());
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    RagdollProfile profile;
    read(merged, "model", profile.model);
    if (const json::Value* layer = merged.find("collision_layer"); layer != nullptr) {
        profile.collision_layer = static_cast<std::uint32_t>(layer->as_int());
    }
    if (const json::Value* mask = merged.find("collision_mask"); mask != nullptr) {
        profile.collision_mask = static_cast<std::uint32_t>(mask->as_int());
    }
    if (const json::Value* self = merged.find("self_collision"); self != nullptr) {
        profile.self_collision = self->as_bool();
    }
    const json::Value* defaults = merged.find("defaults");
    for (const auto& [name, bone] : merged.find("bones")->as_object()) {
        profile.bones.emplace(name,
                              read_bone(defaults != nullptr ? merge_patch(*defaults, bone) : bone));
    }
    if (const json::Value* balance = merged.find("balance"); balance != nullptr) {
        read(*balance, "bone", profile.balance.bone);
        read(*balance, "stiffness", profile.balance.stiffness);
        read(*balance, "damping", profile.balance.damping);
        read(*balance, "position_stiffness", profile.balance.position_stiffness);
        read(*balance, "position_damping", profile.balance.position_damping);
        read(*balance, "max_tilt", profile.balance.max_tilt);
        if (!profile.balance.bone.empty() && !profile.bones.contains(profile.balance.bone)) {
            return make_error(ErrorCode::InvalidArgument,
                              "/balance/bone: the profile has no bone \"" + profile.balance.bone +
                                  "\"");
        }
    }
    if (const json::Value* hit = merged.find("hit"); hit != nullptr) {
        read(*hit, "weaken", profile.hit.weaken);
        read(*hit, "spread", profile.hit.spread);
        read(*hit, "recovery", profile.hit.recovery);
        read(*hit, "knockdown", profile.hit.knockdown);
    }
    if (const json::Value* get_up = merged.find("get_up"); get_up != nullptr) {
        if (const json::Value* enabled = get_up->find("enabled"); enabled != nullptr) {
            profile.get_up.enabled = enabled->as_bool();
        }
        read(*get_up, "settle_speed", profile.get_up.settle_speed);
        read(*get_up, "settle_time", profile.get_up.settle_time);
        read(*get_up, "face_up", profile.get_up.face_up);
        read(*get_up, "face_down", profile.get_up.face_down);
        read(*get_up, "blend", profile.get_up.blend);
        read(*get_up, "forward", profile.get_up.forward);
    }
    return profile;
}

bool is_ragdoll_path(std::string_view path) {
    return path.ends_with(".ragdoll.json");
}

} // namespace slash::world
