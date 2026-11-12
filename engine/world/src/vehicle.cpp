// Vehicle recipes: their files, the defaults each kind starts from, the drivetrain both
// dimensions drive with, and the 2D side view.
#include "slash/world/vehicle.hpp"

#include "slash/asset/assets.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine vehicle",
  "description": "A vehicle built from a recipe, after Unreal's Chaos Vehicles: its body, seats, wheels or hull or wings, and its own physics. A MeshInstance3D draws it; a Vehicle3D or Vehicle2D drives it. Lengths are meters in its space (facing +z, y up, the ground at 0), speeds meters a second, forces newtons, colors [r, g, b] or [r, g, b, a] from 0 to 1. What is left out follows its kind.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "$defs": {
    "color": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 3, "maxItems": 4},
    "point": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
    "range": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 2, "maxItems": 2, "description": "[min, max]; 0 for no bound."},
    "share": {"type": "number", "minimum": 0, "maximum": 1}
  },
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "extends": {"type": "string", "pattern": "\\.vehicle\\.json$", "description": "A .vehicle.json file this one changes, as a JSON Merge Patch: objects merge, other values (lists included) replace."},
    "name": {"type": "string", "description": "What it is called, for checks and reports."},
    "kind": {"enum": ["car", "bike", "truck", "tracked", "boat", "aircraft"], "description": "What it is, which its defaults follow. Default car."},
    "body": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "length": {"type": "number", "minimum": 0.3, "maximum": 100, "description": "Nose to tail."},
        "width": {"type": "number", "minimum": 0.1, "maximum": 30},
        "height": {"type": "number", "minimum": 0.2, "maximum": 30, "description": "From the ground to the top."},
        "clearance": {"type": "number", "minimum": 0, "maximum": 10, "description": "From the ground to the body's bottom."},
        "cabin": {"$ref": "#/$defs/share", "description": "The cabin's share of the length; 0 for none."},
        "color": {"$ref": "#/$defs/color"},
        "trim": {"$ref": "#/$defs/color", "description": "Wheels, tracks and windows' frames."},
        "model": {"type": "string", "minLength": 1, "description": "A model file drawn as the body in place of the built one; it still collides as the built one."},
        "draw": {"type": "boolean", "description": "false: only its parts draw, though it still collides as its box. Default true."}
      }
    },
    "parts": {
      "type": "array", "maxItems": 64,
      "description": "Parts it is built of, on top of its body (or in place of it, with body.draw false): Godot's primitive shapes a meter across, stretched, turned and placed in its space, or model files (3D only).",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["name"],
        "properties": {
          "name": {"type": "string", "pattern": "^[A-Za-z0-9_]+$", "description": "Unique; its 2D piece is <vehicle>_<name>."},
          "shape": {"enum": ["box", "cylinder", "sphere", "prism"], "description": "A cylinder stands along y; a prism's ridge runs along z with its point up. Default box."},
          "model": {"type": "string", "minLength": 1, "description": "A model file in place of a shape; 3D only."},
          "position": {"$ref": "#/$defs/point", "description": "Its middle. Default [0, 0, 0]."},
          "size": {"type": "array", "items": {"type": "number", "minimum": 0.01, "maximum": 100}, "minItems": 3, "maxItems": 3, "description": "Meters along x, y and z (a model's scale). Default [1, 1, 1]."},
          "rotation": {"$ref": "#/$defs/point", "description": "Radians, in Godot's YXZ order. Default [0, 0, 0]."},
          "material": {"type": "string", "pattern": "^[A-Za-z0-9_]+$", "description": "paint, trim or glass (the body's), or a name of its own with color. Default paint."},
          "color": {"$ref": "#/$defs/color", "description": "Its own color, under the material's name."},
          "collide": {"type": "boolean", "description": "Its turned box joins the vehicle's collision. Default false."}
        }
      }
    },
    "seats": {
      "type": "array", "maxItems": 32,
      "description": "Where people sit. Default by kind: a driver's seat and one beside it for cars and trucks.",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["name", "position"],
        "properties": {
          "name": {"type": "string", "minLength": 1},
          "position": {"$ref": "#/$defs/point", "description": "Where the sitter's feet are."},
          "driver": {"type": "boolean", "description": "Whoever sits here drives it. Default false."},
          "door": {"type": "boolean", "description": "Has a door beside it. Default false."}
        }
      }
    },
    "wheels": {
      "type": "array", "maxItems": 32,
      "description": "Its wheels; none for boats. Default by kind.",
      "items": {
        "type": "object", "additionalProperties": false, "required": ["position"],
        "properties": {
          "position": {"$ref": "#/$defs/point", "description": "Its middle at rest."},
          "radius": {"type": "number", "minimum": 0.05, "maximum": 5, "description": "Default 0.35."},
          "width": {"type": "number", "minimum": 0.02, "maximum": 3, "description": "Default 0.25."},
          "steer": {"type": "boolean", "description": "Turns with the steering. Default false."},
          "drive": {"type": "boolean", "description": "The engine drives it; default by the differential."},
          "handbrake": {"type": "boolean", "description": "The handbrake holds it. Default the rear wheels."}
        }
      }
    },
    "wings": {
      "type": "object", "additionalProperties": false,
      "description": "Aircraft only.",
      "properties": {
        "span": {"type": "number", "minimum": 0.5, "maximum": 100},
        "chord": {"type": "number", "minimum": 0.1, "maximum": 20}
      }
    },
    "physics": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "mass": {"type": "number", "minimum": 1, "maximum": 1000000, "description": "Kilograms."},
        "center_of_mass": {"$ref": "#/$defs/point", "description": "In its space; inside the body."},
        "engine": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "max_torque": {"type": "number", "minimum": 0, "description": "Newton-meters."},
            "max_rpm": {"type": "number", "exclusiveMinimum": 0, "maximum": 30000},
            "idle_rpm": {"type": "number", "minimum": 0},
            "torque_curve": {"type": "array", "minItems": 1, "maxItems": 32, "description": "[rpm share of max_rpm, torque share of max_torque] points, rpm going up.", "items": {"type": "array", "items": {"type": "number", "minimum": 0, "maximum": 1}, "minItems": 2, "maxItems": 2}},
            "thrust": {"type": "number", "minimum": 0, "description": "Newtons a boat's propeller or an aircraft's engines push at full throttle."}
          }
        },
        "gears": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "ratios": {"type": "array", "minItems": 1, "maxItems": 16, "items": {"type": "number", "exclusiveMinimum": 0}, "description": "First gear's first, each lower than the one before."},
            "reverse": {"type": "number", "exclusiveMinimum": 0},
            "final_drive": {"type": "number", "exclusiveMinimum": 0},
            "shift_up": {"$ref": "#/$defs/share", "description": "The share of max_rpm it shifts up at."},
            "shift_down": {"$ref": "#/$defs/share", "description": "And down at."},
            "shift_time": {"type": "number", "minimum": 0, "maximum": 5, "description": "Seconds without torque while it shifts."},
            "efficiency": {"$ref": "#/$defs/share"}
          }
        },
        "differential": {"enum": ["rear", "front", "all"], "description": "Which wheels the engine drives when wheels don't say."},
        "front_share": {"$ref": "#/$defs/share", "description": "All-wheel drive: the front wheels' share of the force."},
        "steering": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "max_angle": {"type": "number", "minimum": 0, "maximum": 1.5, "description": "Radians."},
            "speed": {"type": "number", "exclusiveMinimum": 0, "description": "Radians a second the wheels turn."},
            "at_speed": {"$ref": "#/$defs/share", "description": "How much less they turn at 30 meters a second."}
          }
        },
        "brakes": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "force": {"type": "number", "minimum": 0, "description": "Newtons each wheel."},
            "handbrake": {"type": "number", "minimum": 0}
          }
        },
        "suspension": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "stiffness": {"type": "number", "minimum": 0, "description": "As Godot's VehicleWheel3D's."},
            "damping": {"type": "number", "minimum": 0, "description": "Compression."},
            "relaxation": {"type": "number", "minimum": 0},
            "travel": {"type": "number", "minimum": 0, "description": "Meters."},
            "rest": {"type": "number", "minimum": 0, "description": "Meters."},
            "max_force": {"type": "number", "minimum": 0, "description": "Newtons; 0 for four times its share of the weight."}
          }
        },
        "tires": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "friction": {"type": "number", "minimum": 0, "description": "As Godot's wheel_friction_slip."},
            "roll_influence": {"$ref": "#/$defs/share"}
          }
        },
        "drag": {"type": "number", "minimum": 0, "maximum": 5, "description": "Air drag coefficient over its front."},
        "balance": {"type": "number", "minimum": 0, "maximum": 10, "description": "How hard it keeps upright; bikes 1."},
        "water": {
          "type": "object", "additionalProperties": false,
          "description": "Boats.",
          "properties": {
            "level": {"type": "number", "description": "Where the water's surface is: meters up in 3D, pixels down in 2D."},
            "draft": {"type": "number", "minimum": 0, "description": "How deep the hull sits at rest."},
            "drag": {"type": "number", "minimum": 0},
            "rudder": {"type": "number", "minimum": 0, "description": "How hard it turns."}
          }
        },
        "flight": {
          "type": "object", "additionalProperties": false,
          "description": "Aircraft.",
          "properties": {
            "lift": {"type": "number", "minimum": 0, "description": "The wings' lift coefficient."},
            "control": {"type": "number", "minimum": 0, "description": "How hard its surfaces turn it."}
          }
        }
      }
    },
    "damage": {
      "type": "object", "additionalProperties": false,
      "description": "What it does to characters and creatures it hits.",
      "properties": {
        "per_impulse": {"type": "number", "minimum": 0, "description": "Damage a kilogram-meter a second of impulse."},
        "knockdown": {"type": "number", "minimum": 0, "description": "The impulse that knocks a ragdoll down."},
        "min_speed": {"type": "number", "minimum": 0, "description": "Meters a second it must close at to hurt."}
      }
    },
    "design": {
      "type": "object", "additionalProperties": false,
      "description": "What vehicle.check holds a test drive to.",
      "properties": {
        "top_speed": {"$ref": "#/$defs/range", "description": "Meters a second."},
        "acceleration": {"$ref": "#/$defs/range", "description": "Seconds from 0 to 100 km/h."},
        "braking_distance": {"$ref": "#/$defs/range", "description": "Meters to stop from 100 km/h, or its top speed when lower."},
        "turning_radius": {"$ref": "#/$defs/range", "description": "Meters, at 5 meters a second."}
      }
    }
  }
})";

constexpr int kMaxExtends = 8;

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

Vec3 point_of(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<real_t>(items[0].as_double()), static_cast<real_t>(items[1].as_double()),
            static_cast<real_t>(items[2].as_double())};
}

void read(const json::Value& doc, std::string_view key, Vec3& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = point_of(*value);
    }
}

void read(const json::Value& doc, std::string_view key, std::array<float, 2>& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = {static_cast<float>(value->as_array()[0].as_double()),
               static_cast<float>(value->as_array()[1].as_double())};
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
        return make_error(ErrorCode::InvalidArgument, "vehicles extend others at most " +
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
    if (const auto errors = json::validate(parsed.value(), vehicle_schema()); !errors.empty()) {
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

// What each kind starts from: its body, mass, engine and wheels' size.
struct KindDefaults {
    float length, width, height, clearance, cabin;
    float mass, torque, max_rpm, thrust, radius;
};

KindDefaults defaults_of(std::string_view kind) {
    if (kind == "bike") {
        return {.length = 2.1F,
                .width = 0.5F,
                .height = 1.1F,
                .clearance = 0.3F,
                .cabin = 0.0F,
                .mass = 220.0F,
                .torque = 90.0F,
                .max_rpm = 9000.0F,
                .thrust = 0.0F,
                .radius = 0.32F};
    }
    if (kind == "truck") {
        return {.length = 7.0F,
                .width = 2.4F,
                .height = 3.0F,
                .clearance = 0.5F,
                .cabin = 0.3F,
                .mass = 6000.0F,
                .torque = 1800.0F,
                .max_rpm = 3000.0F,
                .thrust = 0.0F,
                .radius = 0.5F};
    }
    if (kind == "tracked") {
        return {.length = 6.5F,
                .width = 3.2F,
                .height = 2.4F,
                .clearance = 0.45F,
                .cabin = 0.4F,
                .mass = 18000.0F,
                .torque = 6000.0F,
                .max_rpm = 2800.0F,
                .thrust = 0.0F,
                .radius = 0.4F};
    }
    if (kind == "boat") {
        return {.length = 5.0F,
                .width = 2.0F,
                .height = 1.4F,
                .clearance = 0.0F,
                .cabin = 0.35F,
                .mass = 800.0F,
                .torque = 0.0F,
                .max_rpm = 5000.0F,
                .thrust = 7000.0F,
                .radius = 0.0F};
    }
    if (kind == "aircraft") {
        return {.length = 8.0F,
                .width = 1.4F,
                .height = 2.2F,
                .clearance = 0.7F,
                .cabin = 0.25F,
                .mass = 1000.0F,
                .torque = 0.0F,
                .max_rpm = 2700.0F,
                .thrust = 5000.0F,
                .radius = 0.3F};
    }
    return {.length = 4.2F,
            .width = 1.8F,
            .height = 1.4F,
            .clearance = 0.3F,
            .cabin = 0.5F,
            .mass = 1200.0F,
            .torque = 300.0F,
            .max_rpm = 6500.0F,
            .thrust = 0.0F,
            .radius = 0.35F};
}

// The wheels a kind has when the recipe names none.
std::vector<VehicleWheel> default_wheels(const VehicleRecipe& recipe, float radius) {
    const VehicleBody& b = recipe.body;
    std::vector<VehicleWheel> out;
    const real_t side = (b.width * 0.5F) - 0.15F;
    const real_t front = (b.length * 0.5F) - (radius * 1.4F);
    if (recipe.kind == "bike") {
        out.push_back(
            {.position = {0.0F, radius, front}, .radius = radius, .width = 0.12F, .steer = true});
        out.push_back({.position = {0.0F, radius, -front},
                       .radius = radius,
                       .width = 0.15F,
                       .handbrake = true});
    } else if (recipe.kind == "truck") {
        for (const real_t x : {side, -side}) {
            out.push_back({.position = {x, radius, front}, .radius = radius, .steer = true});
            out.push_back(
                {.position = {x, radius, -front * 0.45F}, .radius = radius, .handbrake = true});
            out.push_back({.position = {x, radius, -front}, .radius = radius, .handbrake = true});
        }
    } else if (recipe.kind == "tracked") {
        // Road wheels along each side, all driven; it slews on its tracks rather than steering
        // them.
        constexpr int kRoad = 5;
        for (const real_t x : {side, -side}) {
            for (int i = 0; i < kRoad; ++i) {
                const real_t z = front - ((2.0F * front) * static_cast<real_t>(i) /
                                          static_cast<real_t>(kRoad - 1));
                out.push_back({.position = {x, radius, z},
                               .radius = radius,
                               .width = 0.5F,
                               .drive = true,
                               .handbrake = true});
            }
        }
    } else if (recipe.kind == "aircraft") {
        out.push_back({.position = {0.0F, radius, b.length * 0.35F},
                       .radius = radius * 0.8F,
                       .width = 0.15F,
                       .steer = true});
        for (const real_t x : {b.width * 1.2F, -b.width * 1.2F}) {
            out.push_back({.position = {x, radius, -b.length * 0.05F},
                           .radius = radius,
                           .width = 0.2F,
                           .handbrake = true});
        }
    } else if (recipe.kind != "boat") {
        for (const real_t x : {side, -side}) {
            out.push_back({.position = {x, radius, front}, .radius = radius, .steer = true});
            out.push_back({.position = {x, radius, -front}, .radius = radius, .handbrake = true});
        }
    }
    return out;
}

std::vector<VehicleSeat> default_seats(const VehicleRecipe& recipe) {
    const VehicleBody& b = recipe.body;
    const real_t floor = b.clearance + 0.1F;
    const real_t z = -b.length * 0.05F;
    if (recipe.kind == "bike") {
        return {{.name = "driver", .position = {0.0F, b.height * 0.55F, -0.1F}, .driver = true}};
    }
    if (recipe.kind == "boat") {
        return {{.name = "driver", .position = {0.0F, 0.3F, -b.length * 0.15F}, .driver = true},
                {.name = "passenger", .position = {0.0F, 0.3F, b.length * 0.1F}}};
    }
    if (recipe.kind == "aircraft") {
        return {{.name = "pilot", .position = {0.0F, floor, b.length * 0.2F}, .driver = true}};
    }
    if (recipe.kind == "tracked") {
        return {{.name = "driver", .position = {0.0F, b.height * 0.55F, 0.0F}, .driver = true}};
    }
    const real_t x = b.width * 0.22F;
    const real_t cab = recipe.kind == "truck" ? (b.length * 0.5F) - (b.length * b.cabin * 0.5F) : z;
    return {{.name = "driver", .position = {x, floor, cab}, .driver = true, .door = true},
            {.name = "passenger", .position = {-x, floor, cab}, .door = true}};
}

void read_physics(const json::Value& doc, VehicleRecipe& out) {
    VehiclePhysics& p = out.physics;
    read(doc, "mass", p.mass);
    read(doc, "center_of_mass", p.center_of_mass);
    if (const json::Value* engine = section(doc, "engine"); engine != nullptr) {
        read(*engine, "max_torque", p.engine.max_torque);
        read(*engine, "max_rpm", p.engine.max_rpm);
        read(*engine, "idle_rpm", p.engine.idle_rpm);
        read(*engine, "thrust", p.engine.thrust);
        if (const json::Value* curve = engine->find("torque_curve"); curve != nullptr) {
            p.engine.torque_curve.clear();
            for (const json::Value& point : curve->as_array()) {
                p.engine.torque_curve.emplace_back(
                    static_cast<float>(point.as_array()[0].as_double()),
                    static_cast<float>(point.as_array()[1].as_double()));
            }
        }
    }
    if (const json::Value* gears = section(doc, "gears"); gears != nullptr) {
        if (const json::Value* ratios = gears->find("ratios"); ratios != nullptr) {
            p.gears.ratios.clear();
            for (const json::Value& ratio : ratios->as_array()) {
                p.gears.ratios.push_back(static_cast<float>(ratio.as_double()));
            }
        }
        read(*gears, "reverse", p.gears.reverse);
        read(*gears, "final_drive", p.gears.final_drive);
        read(*gears, "shift_up", p.gears.shift_up);
        read(*gears, "shift_down", p.gears.shift_down);
        read(*gears, "shift_time", p.gears.shift_time);
        read(*gears, "efficiency", p.gears.efficiency);
    }
    read(doc, "differential", p.differential);
    read(doc, "front_share", p.front_share);
    if (const json::Value* steering = section(doc, "steering"); steering != nullptr) {
        read(*steering, "max_angle", p.max_steer);
        read(*steering, "speed", p.steer_speed);
        read(*steering, "at_speed", p.steer_at_speed);
    }
    if (const json::Value* brakes = section(doc, "brakes"); brakes != nullptr) {
        read(*brakes, "force", p.brake_force);
        read(*brakes, "handbrake", p.handbrake_force);
    }
    if (const json::Value* suspension = section(doc, "suspension"); suspension != nullptr) {
        read(*suspension, "stiffness", p.suspension_stiffness);
        read(*suspension, "damping", p.suspension_damping);
        read(*suspension, "relaxation", p.suspension_relaxation);
        read(*suspension, "travel", p.suspension_travel);
        read(*suspension, "rest", p.suspension_rest);
        read(*suspension, "max_force", p.suspension_max_force);
    }
    if (const json::Value* tires = section(doc, "tires"); tires != nullptr) {
        read(*tires, "friction", p.tire_friction);
        read(*tires, "roll_influence", p.roll_influence);
    }
    read(doc, "drag", p.drag);
    read(doc, "balance", p.balance);
    if (const json::Value* water = section(doc, "water"); water != nullptr) {
        read(*water, "level", p.water_level);
        read(*water, "draft", p.draft);
        read(*water, "drag", p.water_drag);
        read(*water, "rudder", p.rudder);
    }
    if (const json::Value* flight = section(doc, "flight"); flight != nullptr) {
        read(*flight, "lift", p.lift);
        read(*flight, "control", p.control);
    }
}

// The recipe a resolved doc (no "extends") makes.
VehicleRecipe recipe_of(const json::Value& doc) {
    VehicleRecipe out;
    read(doc, "name", out.name);
    read(doc, "kind", out.kind);
    const KindDefaults d = defaults_of(out.kind);
    VehicleBody& b = out.body;
    b.length = d.length;
    b.width = d.width;
    b.height = d.height;
    b.clearance = d.clearance;
    b.cabin = d.cabin;
    if (out.kind == "boat") {
        b.color = {0.92F, 0.92F, 0.88F, 1.0F};
    } else if (out.kind == "tracked") {
        b.color = {0.32F, 0.38F, 0.22F, 1.0F};
    } else if (out.kind == "aircraft") {
        b.color = {0.85F, 0.85F, 0.9F, 1.0F};
    }
    if (const json::Value* body = section(doc, "body"); body != nullptr) {
        read(*body, "length", b.length);
        read(*body, "width", b.width);
        read(*body, "height", b.height);
        read(*body, "clearance", b.clearance);
        read(*body, "cabin", b.cabin);
        read(*body, "color", b.color);
        read(*body, "trim", b.trim);
        read(*body, "model", b.model);
        read(*body, "draw", b.draw);
    }
    if (const json::Value* parts = doc.find("parts"); parts != nullptr) {
        for (const json::Value& part : parts->as_array()) {
            VehiclePart made;
            read(part, "name", made.name);
            read(part, "shape", made.shape);
            read(part, "model", made.model);
            read(part, "position", made.position);
            read(part, "size", made.size);
            read(part, "rotation", made.rotation);
            read(part, "material", made.material);
            if (part.find("color") != nullptr) {
                Vec4 color;
                read(part, "color", color);
                made.color = color;
            }
            read(part, "collide", made.collide);
            out.parts.push_back(std::move(made));
        }
    }
    if (out.kind == "aircraft") {
        out.wing_span = b.length * 1.25F;
        out.wing_chord = b.length * 0.18F;
        if (const json::Value* wings = section(doc, "wings"); wings != nullptr) {
            read(*wings, "span", out.wing_span);
            read(*wings, "chord", out.wing_chord);
        }
    }
    VehiclePhysics& p = out.physics;
    p.mass = d.mass;
    p.engine.max_torque = d.torque;
    p.engine.max_rpm = d.max_rpm;
    p.engine.thrust = d.thrust;
    p.center_of_mass = {0.0F, b.clearance + ((b.height - b.clearance) * 0.3F), 0.0F};
    if (out.kind == "bike") {
        p.balance = 1.0F;
        p.gears.ratios = {2.8F, 2.0F, 1.6F, 1.3F, 1.1F, 0.95F};
        p.gears.final_drive = 3.0F;
        p.max_steer = 0.5F;
    } else if (out.kind == "truck") {
        p.gears.ratios = {6.0F, 4.0F, 2.8F, 1.9F, 1.4F, 1.0F, 0.8F};
        p.gears.final_drive = 4.1F;
        p.brake_force = 12000.0F;
        p.handbrake_force = 16000.0F;
        p.suspension_stiffness = 60.0F;
        p.max_steer = 0.5F;
    } else if (out.kind == "tracked") {
        p.gears.ratios = {5.0F, 3.0F, 1.8F};
        p.gears.final_drive = 6.0F;
        p.brake_force = 30000.0F;
        p.handbrake_force = 40000.0F;
        p.suspension_stiffness = 80.0F;
        p.tire_friction = 0.8F; // tracks slide sideways as they slew it
        p.max_steer = 1.0F;     // how fast it slews
    } else if (out.kind == "boat") {
        p.drag = 0.2F;
        p.center_of_mass = {0.0F, b.height * 0.15F, -b.length * 0.05F};
    } else if (out.kind == "aircraft") {
        p.drag = 0.04F;
        p.brake_force = 2000.0F;
        p.center_of_mass = {0.0F, b.clearance + ((b.height - b.clearance) * 0.4F),
                            b.length * 0.05F};
    }
    if (const json::Value* physics = section(doc, "physics"); physics != nullptr) {
        read_physics(*physics, out);
    }
    if (const json::Value* damage = section(doc, "damage"); damage != nullptr) {
        read(*damage, "per_impulse", out.damage.per_impulse);
        read(*damage, "knockdown", out.damage.knockdown);
        read(*damage, "min_speed", out.damage.min_speed);
    }
    if (const json::Value* design = section(doc, "design"); design != nullptr) {
        read(*design, "top_speed", out.design.top_speed);
        read(*design, "acceleration", out.design.acceleration);
        read(*design, "braking_distance", out.design.braking_distance);
        read(*design, "turning_radius", out.design.turning_radius);
    }
    if (const json::Value* seats = doc.find("seats"); seats != nullptr) {
        for (const json::Value& seat : seats->as_array()) {
            VehicleSeat made;
            read(seat, "name", made.name);
            read(seat, "position", made.position);
            read(seat, "driver", made.driver);
            read(seat, "door", made.door);
            out.seats.push_back(std::move(made));
        }
    } else {
        out.seats = default_seats(out);
    }
    if (const json::Value* wheels = doc.find("wheels"); wheels != nullptr) {
        for (const json::Value& wheel : wheels->as_array()) {
            VehicleWheel made;
            made.radius = d.radius > 0.0F ? d.radius : made.radius;
            read(wheel, "position", made.position);
            read(wheel, "radius", made.radius);
            read(wheel, "width", made.width);
            read(wheel, "steer", made.steer);
            made.handbrake = made.position.z < 0.0F;
            read(wheel, "handbrake", made.handbrake);
            // The differential decides, unless the wheel does.
            const bool front = made.position.z >= 0.0F;
            made.drive = p.differential == "all" || (p.differential == "front") == front;
            read(wheel, "drive", made.drive);
            out.wheels.push_back(made);
        }
    } else {
        out.wheels = default_wheels(out, d.radius);
        if (out.kind == "car" || out.kind == "bike" || out.kind == "truck") {
            for (VehicleWheel& wheel : out.wheels) {
                const bool front = wheel.position.z >= 0.0F;
                wheel.drive = p.differential == "all" || (p.differential == "front") == front;
            }
        }
    }
    if (out.kind == "aircraft" && doc.find("wheels") == nullptr) {
        for (VehicleWheel& wheel : out.wheels) {
            wheel.drive = false; // its engines pull it
        }
    }
    return out;
}

// A recipe and the text of each file it was made from, so it is parsed again only when one of
// them changes.
struct CacheEntry {
    std::vector<std::pair<std::string, std::string>> files;
    json::Value resolved;
    std::shared_ptr<const VehicleRecipe> recipe;
};

struct VehicleCache {
    std::mutex mutex;
    std::map<std::string, CacheEntry, std::less<>> recipes;
};
using VehicleCacheHolder = std::shared_ptr<VehicleCache>;

Result<CacheEntry> load_entry(const World& world, std::string_view path) {
    if (!is_vehicle_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .vehicle.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    std::vector<std::pair<std::string, std::string>> files;
    FileReader reader;
    if (assets != nullptr) {
        reader = [assets, &files](std::string_view file) -> Result<std::string> {
            auto text = assets->read(file, "vehicle");
            if (text) {
                files.emplace_back(std::string(file), text.value());
            }
            return text;
        };
    } else if (world.file_reader()) {
        reader = world.file_reader();
    } else {
        return make_error(ErrorCode::PlatformFailure,
                          "the world has no files to read vehicles from");
    }
    const auto* holder = world.ecs().ctx().find<VehicleCacheHolder>();
    VehicleCache* cache = holder != nullptr && assets != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->recipes.find(path); found != cache->recipes.end()) {
            const bool same = std::ranges::all_of(found->second.files, [&](const auto& file) {
                auto text = assets->read(file.first, "vehicle");
                return text && text.value() == file.second;
            });
            if (same) {
                return found->second;
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
    if (const auto errors = validate_vehicle(doc.value()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
                              errors.front().message);
    }
    auto resolved = resolve_vehicle(doc.value(), reader);
    if (!resolved) {
        return resolved.error();
    }
    if (const auto errors = validate_vehicle(resolved.value()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
                              errors.front().message + " (with what it extends)");
    }
    CacheEntry entry{.files = std::move(files),
                     .resolved = resolved.value(),
                     .recipe = std::make_shared<const VehicleRecipe>(recipe_of(resolved.value()))};
    if (cache != nullptr) {
        cache->recipes[std::string(path)] = entry;
    }
    return entry;
}

std::vector<Vec2> box(real_t x0, real_t y0, real_t x1, real_t y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

// Points on a part's shape a meter across, as Godot's primitive meshes are made: enough of them
// that the outline of the turned shape seen from any side is its own.
std::vector<Vec3> shape_points(std::string_view shape) {
    std::vector<Vec3> out;
    constexpr int kAround = 16;
    if (shape == "cylinder" || shape == "sphere") {
        const int rings = shape == "sphere" ? 9 : 2;
        for (int ring = 0; ring < rings; ++ring) {
            real_t y = 0.5F;
            real_t radius = 0.5F;
            if (shape == "sphere") {
                const Vec2f angle =
                    cos_sin(std::numbers::pi_v<float> *
                            (static_cast<float>(ring) / static_cast<float>(rings - 1)));
                y = 0.5F * angle.x;
                radius = 0.5F * angle.y;
            } else if (ring == 1) {
                y = -0.5F;
            }
            for (int i = 0; i < kAround; ++i) {
                const Vec2f around = cos_sin(2.0F * std::numbers::pi_v<float> *
                                             static_cast<float>(i) / static_cast<float>(kAround));
                out.push_back({radius * around.x, y, radius * around.y});
            }
        }
    } else if (shape == "prism") {
        for (const real_t z : {-0.5F, 0.5F}) {
            out.push_back({-0.5F, -0.5F, z});
            out.push_back({0.5F, -0.5F, z});
            out.push_back({0.0F, 0.5F, z});
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            out.push_back({(i & 1) != 0 ? 0.5F : -0.5F, (i & 2) != 0 ? 0.5F : -0.5F,
                           (i & 4) != 0 ? 0.5F : -0.5F});
        }
    }
    return out;
}

// The convex hull of points, counterclockwise from the lowest leftmost (Andrew's monotone
// chain), ties kept in a fixed order so outlines are the same everywhere.
std::vector<Vec2> hull(std::vector<Vec2> points) {
    std::ranges::sort(points,
                      [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    const auto turn = [](Vec2 o, Vec2 a, Vec2 b) {
        return ((a.x - o.x) * (b.y - o.y)) - ((a.y - o.y) * (b.x - o.x));
    };
    std::vector<Vec2> out(points.size() * 2);
    std::size_t k = 0;
    for (const Vec2 p : points) {
        while (k >= 2 && turn(out[k - 2], out[k - 1], p) <= 0.0F) {
            --k;
        }
        out[k++] = p;
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = points.size(); i-- > 1;) {
        const Vec2 p = points[i - 1];
        while (k >= lower && turn(out[k - 2], out[k - 1], p) <= 0.0F) {
            --k;
        }
        out[k++] = p;
    }
    out.resize(k > 1 ? k - 1 : k);
    return out;
}

} // namespace

bool is_vehicle_path(std::string_view path) {
    return path.ends_with(kVehicleSuffix);
}

json::Value vehicle_schema() {
    static const json::Value schema = json::parse(kSchema).value();
    return schema;
}

std::vector<json::ValidationError> validate_vehicle(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, vehicle_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* kind_value = doc.find("kind");
    const bool has_extends = doc.find("extends") != nullptr;
    // What is left out is a car's, unless it comes from the file it extends.
    std::string kind = has_extends ? "" : "car";
    if (kind_value != nullptr) {
        kind = kind_value->as_string();
    }
    if (const json::Value* seats = doc.find("seats"); seats != nullptr) {
        bool driver = false;
        std::vector<std::string> names;
        for (std::size_t i = 0; i < seats->as_array().size(); ++i) {
            const json::Value& seat = seats->as_array()[i];
            const json::Value* is_driver = seat.find("driver");
            driver = driver || (is_driver != nullptr && is_driver->as_bool());
            const std::string& name = seat.find("name")->as_string();
            if (std::ranges::find(names, name) != names.end()) {
                errors.push_back({"/seats/" + std::to_string(i) + "/name",
                                  "two seats are called \"" + name + "\""});
            }
            names.push_back(name);
        }
        if (!driver && !seats->as_array().empty()) {
            errors.push_back({"/seats", "no seat is the driver's"});
        }
    }
    if (const json::Value* parts = doc.find("parts"); parts != nullptr) {
        // Its 2D pieces are named by its parts beside the body's own and its wheels'.
        constexpr std::array<std::string_view, 8> kTaken{"body", "cabin", "hull",  "frame",
                                                         "wing", "tail",  "track", "collision"};
        std::vector<std::string> names;
        for (std::size_t i = 0; i < parts->as_array().size(); ++i) {
            const json::Value& part = parts->as_array()[i];
            const std::string& name = part.find("name")->as_string();
            const std::string at = "/parts/" + std::to_string(i);
            if (std::ranges::find(names, name) != names.end()) {
                errors.push_back({at + "/name", "two parts are called \"" + name + "\""});
            } else if (std::ranges::find(kTaken, name) != kTaken.end() ||
                       name.starts_with("wheel_")) {
                errors.push_back({at + "/name", "\"" + name +
                                                    "\" is a name the vehicle's own "
                                                    "pieces take"});
            }
            names.push_back(name);
            if (part.find("model") != nullptr && part.find("shape") != nullptr) {
                errors.push_back({at, "a part is a shape or a model, not both"});
            }
            const json::Value* material = part.find("material");
            const std::string own = material != nullptr ? material->as_string() : "paint";
            const bool body_material = own == "paint" || own == "trim" || own == "glass";
            if (!body_material && part.find("color") == nullptr) {
                errors.push_back({at + "/material", "\"" + own +
                                                        "\" needs a color; the body's "
                                                        "are paint, trim and glass"});
            }
        }
    }
    if (kind == "boat") {
        if (const json::Value* wheels = doc.find("wheels");
            wheels != nullptr && !wheels->as_array().empty()) {
            errors.push_back({"/wheels", "a boat has no wheels"});
        }
    }
    if (!kind.empty() && kind != "aircraft" && doc.find("wings") != nullptr) {
        errors.push_back({"/wings", "only aircraft have wings"});
    }
    if (const json::Value* physics = doc.find("physics"); physics != nullptr) {
        if (const json::Value* engine = physics->find("engine"); engine != nullptr) {
            if (const json::Value* curve = engine->find("torque_curve"); curve != nullptr) {
                double last = -1.0;
                for (std::size_t i = 0; i < curve->as_array().size(); ++i) {
                    const double rpm = curve->as_array()[i].as_array()[0].as_double();
                    if (rpm <= last) {
                        errors.push_back({"/physics/engine/torque_curve/" + std::to_string(i),
                                          "the curve's rpm must go up"});
                        break;
                    }
                    last = rpm;
                }
            }
            const json::Value* idle = engine->find("idle_rpm");
            const json::Value* max = engine->find("max_rpm");
            if (idle != nullptr && max != nullptr && idle->as_double() >= max->as_double()) {
                errors.push_back({"/physics/engine/idle_rpm", "idle must be below max_rpm"});
            }
        }
        if (const json::Value* gears = physics->find("gears"); gears != nullptr) {
            if (const json::Value* ratios = gears->find("ratios"); ratios != nullptr) {
                for (std::size_t i = 1; i < ratios->as_array().size(); ++i) {
                    if (ratios->as_array()[i].as_double() >=
                        ratios->as_array()[i - 1].as_double()) {
                        errors.push_back({"/physics/gears/ratios/" + std::to_string(i),
                                          "each gear's ratio must be lower than the one before"});
                        break;
                    }
                }
            }
            const json::Value* up = gears->find("shift_up");
            const json::Value* down = gears->find("shift_down");
            if (up != nullptr && down != nullptr && down->as_double() >= up->as_double()) {
                errors.push_back({"/physics/gears/shift_down", "it must shift down below where "
                                                               "it shifts up"});
            }
        }
    }
    // A center of mass outside the body tips it over.
    if (!has_extends) {
        const VehicleRecipe recipe = recipe_of(doc);
        const Vec3 c = recipe.physics.center_of_mass;
        const VehicleBody& b = recipe.body;
        const real_t reach = recipe.kind == "aircraft" ? recipe.wing_span * 0.5F : b.width * 0.5F;
        if (std::abs(c.x) > reach || std::abs(c.z) > b.length * 0.5F || c.y < 0.0F ||
            c.y > b.height) {
            errors.push_back({"/physics/center_of_mass", "the center of mass is outside the body"});
        }
        if (vehicle_rolls(recipe) && recipe.wheels.empty()) {
            errors.push_back({"/wheels", "a " + recipe.kind + " needs wheels"});
        }
    }
    return errors;
}

Result<json::Value> resolve_vehicle(const json::Value& doc, const FileReader& read_file) {
    std::vector<std::string> chain;
    return resolve(doc, read_file, chain);
}

Result<VehicleRecipe> parse_vehicle(const json::Value& source, const FileReader& read_file) {
    if (const auto errors = validate_vehicle(source); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
                              errors.front().message);
    }
    auto resolved = resolve_vehicle(source, read_file);
    if (!resolved) {
        return resolved.error();
    }
    if (const auto errors = validate_vehicle(resolved.value()); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
                              errors.front().message + " (with what it extends)");
    }
    return recipe_of(resolved.value());
}

Result<VehicleRecipe> tuned_vehicle(const json::Value& resolved, const json::Value& overrides) {
    if (overrides.is_null() || (overrides.is_object() && overrides.as_object().empty())) {
        return recipe_of(resolved);
    }
    const json::Value merged = merge_patch(resolved, overrides);
    if (const auto errors = validate_vehicle(merged); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "root" : errors.front().path) + ": " +
                              errors.front().message);
    }
    return recipe_of(merged);
}

bool vehicle_rolls(const VehicleRecipe& recipe) {
    return recipe.kind != "boat";
}

Result<std::shared_ptr<const VehicleRecipe>> load_vehicle(const World& world,
                                                          std::string_view path) {
    auto entry = load_entry(world, path);
    if (!entry) {
        return entry.error();
    }
    return entry.value().recipe;
}

Result<json::Value> load_vehicle_json(const World& world, std::string_view path) {
    auto entry = load_entry(world, path);
    if (!entry) {
        return entry.error();
    }
    return entry.value().resolved;
}

void prepare_vehicles(World& world) {
    if (world.ecs().ctx().find<VehicleCacheHolder>() == nullptr) {
        world.ecs().ctx().emplace<VehicleCacheHolder>(std::make_shared<VehicleCache>());
    }
}

float torque_share(const VehicleEngine& engine, float rpm_share) {
    const auto& curve = engine.torque_curve;
    if (curve.empty()) {
        return 1.0F;
    }
    if (rpm_share <= curve.front().first) {
        return curve.front().second;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (rpm_share <= curve[i].first) {
            const float t = (rpm_share - curve[i - 1].first) /
                            std::max(curve[i].first - curve[i - 1].first, 1e-6F);
            return curve[i - 1].second + ((curve[i].second - curve[i - 1].second) * t);
        }
    }
    return curve.back().second;
}

DriveOutput drive_step(const VehiclePhysics& physics, DriveState& state, const DriveInput& input,
                       float wheel_rpm, float speed, float wheel_radius, float tick_seconds) {
    DriveOutput out;
    float throttle = std::clamp(input.throttle, -1.0F, 1.0F);
    float brake = std::clamp(input.brake, 0.0F, 1.0F);
    const VehicleGears& gears = physics.gears;
    const int top = static_cast<int>(gears.ratios.size());
    // Backing up takes reverse once it has (nearly) stopped; throttle against the way it rolls
    // brakes.
    if (throttle < -0.05F && speed < 0.5F) {
        state.gear = -1;
    } else if (throttle > 0.05F && speed > -0.5F && state.gear < 1) {
        state.gear = 1;
    }
    if (state.gear >= 1 && throttle < 0.0F) {
        brake = std::max(brake, -throttle);
        throttle = 0.0F;
    } else if (state.gear == -1 && throttle > 0.0F) {
        brake = std::max(brake, throttle);
        throttle = 0.0F;
    }
    state.gear = std::clamp(state.gear, -1, std::max(top, 1));
    float ratio = 0.0F;
    if (state.gear == -1) {
        ratio = gears.reverse;
    } else if (state.gear >= 1) {
        ratio = gears.ratios[static_cast<std::size_t>(state.gear - 1)];
    }
    const float total = ratio * gears.final_drive;
    const VehicleEngine& engine = physics.engine;
    const float turning = std::abs(wheel_rpm) * total;
    state.rpm = std::clamp(turning, engine.idle_rpm, engine.max_rpm);
    state.shift_timer = std::max(state.shift_timer - tick_seconds, 0.0F);
    if (state.gear >= 1 && state.shift_timer <= 0.0F) {
        if (state.rpm >= gears.shift_up * engine.max_rpm && state.gear < top) {
            ++state.gear;
            state.shift_timer = gears.shift_time;
        } else if (state.rpm <= gears.shift_down * engine.max_rpm && state.gear > 1) {
            --state.gear;
            state.shift_timer = gears.shift_time;
        }
    }
    // The rev limiter cuts torque at max_rpm.
    const bool limited = turning >= engine.max_rpm;
    const float torque = limited ? 0.0F
                                 : engine.max_torque *
                                       torque_share(engine, state.rpm / engine.max_rpm) *
                                       std::abs(throttle);
    if (state.shift_timer <= 0.0F && wheel_radius > 0.0F) {
        out.force = torque * total * gears.efficiency / wheel_radius;
        if (state.gear == -1) {
            out.force = -out.force;
        }
    }
    out.brake = brake * physics.brake_force;
    out.handbrake = std::clamp(input.handbrake, 0.0F, 1.0F) * physics.handbrake_force;
    const float reduce = 1.0F - (physics.steer_at_speed * std::min(std::abs(speed) / 30.0F, 1.0F));
    const float target = std::clamp(input.steer, -1.0F, 1.0F) * physics.max_steer * reduce;
    const float step = physics.steer_speed * tick_seconds;
    state.steer += std::clamp(target - state.steer, -step, step);
    out.steer = state.steer;
    return out;
}

VehicleSideView vehicle_side_view(const VehicleRecipe& recipe) {
    VehicleSideView view;
    const VehicleBody& b = recipe.body;
    const real_t half = b.length * 0.5F;
    const real_t low = b.clearance;
    const real_t waist = low + ((b.height - low) * (b.cabin > 0.0F ? 0.55F : 1.0F));
    const Vec4 glass{0.55F, 0.7F, 0.8F, 1.0F};
    if (!b.draw) {
        // Only its parts draw.
    } else if (recipe.kind == "boat") {
        // A hull narrowing to the keel, the bow raked forward.
        view.body.push_back({.name = "hull",
                             .points = {{-half, waist},
                                        {-half, low + (waist * 0.2F)},
                                        {-half * 0.6F, low},
                                        {half * 0.6F, low},
                                        {half, waist * 1.1F}},
                             .color = b.color});
    } else if (recipe.kind == "bike") {
        view.body.push_back({.name = "frame",
                             .points = {{-half * 0.55F, low + 0.15F},
                                        {half * 0.55F, low + 0.15F},
                                        {half * 0.7F, b.height * 0.8F},
                                        {half * 0.45F, b.height * 0.8F},
                                        {half * 0.2F, b.height * 0.55F},
                                        {-half * 0.6F, b.height * 0.55F}},
                             .color = b.color});
    } else {
        view.body.push_back(
            {.name = "body", .points = box(-half, low, half, waist), .color = b.color});
    }
    if (b.draw && b.cabin > 0.0F && recipe.kind != "bike") {
        const real_t cabin = b.length * b.cabin;
        real_t back = -cabin * 0.6F;
        if (recipe.kind == "truck") {
            back = half - cabin;
        } else if (recipe.kind == "aircraft") {
            back = half * 0.2F;
        }
        const real_t front = back + cabin;
        const real_t lean = cabin * 0.15F;
        view.body.push_back({.name = "cabin",
                             .points = {{back, waist},
                                        {front, waist},
                                        {front - lean, b.height},
                                        {back + lean, b.height}},
                             .color = recipe.kind == "tracked" ? b.color : glass});
    }
    if (b.draw && recipe.kind == "aircraft") {
        const real_t chord = recipe.wing_chord;
        view.body.push_back({.name = "wing",
                             .points = box(-chord * 0.5F, waist - 0.12F, chord * 0.5F, waist),
                             .color = b.trim});
        view.body.push_back({.name = "tail",
                             .points = {{-half, waist},
                                        {-half + (chord * 0.8F), waist},
                                        {-half, b.height + (b.height - low) * 0.4F}},
                             .color = b.color});
    }
    if (recipe.kind == "tracked") {
        const real_t r = recipe.wheels.empty() ? 0.4F : recipe.wheels.front().radius;
        view.body.push_back({.name = "track",
                             .points = box(-half + 0.1F, 0.0F, half - 0.1F, r * 2.0F),
                             .color = b.trim});
    }
    // Its parts, each the outline of its shape turned, stretched and placed, seen from the
    // side: z across, y up.
    for (const VehiclePart& part : recipe.parts) {
        if (!part.model.empty()) {
            continue; // models draw in 3D only
        }
        Transform3D place;
        place.position = part.position;
        place.rotation = part.rotation;
        place.scale = part.size;
        const Affine3D at = affine(place);
        std::vector<Vec2> seen;
        for (const Vec3 p : shape_points(part.shape)) {
            const Vec3 w = at.apply(p);
            seen.push_back({w.z, w.y});
        }
        Vec4 color = b.color;
        if (const std::optional<Vec4>& own = part.color; own.has_value()) {
            color = own.value();
        } else if (part.material == "trim") {
            color = b.trim;
        } else if (part.material == "glass") {
            color = glass;
        }
        view.body.push_back({.name = part.name, .points = hull(std::move(seen)), .color = color});
    }
    // One wheel a z: the near side's (x >= 0), or the only one there.
    for (std::size_t i = 0; i < recipe.wheels.size(); ++i) {
        const VehicleWheel& wheel = recipe.wheels[i];
        const bool taken = std::ranges::any_of(view.wheel_of, [&](std::size_t other) {
            return std::abs(recipe.wheels[other].position.z - wheel.position.z) < 0.05F;
        });
        if (taken) {
            continue;
        }
        view.wheels.emplace_back(Vec2{wheel.position.z, wheel.position.y}, wheel.radius);
        view.wheel_of.push_back(i);
    }
    return view;
}

} // namespace slash::world
