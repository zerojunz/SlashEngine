#include "slash/world/physics_material.hpp"

#include "slash/core/json_schema.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace slash::world {

namespace {

json::Value number(std::string_view doc, double minimum) {
    json::Value property;
    property["type"] = "number";
    property["minimum"] = minimum;
    property["description"] = std::string(doc);
    return property;
}

json::Value flag(std::string_view doc) {
    json::Value property;
    property["type"] = "boolean";
    property["description"] = std::string(doc);
    return property;
}

json::Value make_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine physics material";
    schema["description"] =
        "A material bodies share through their physics_material_override, as Godot's "
        "PhysicsMaterial, with Chaos's density, strength and surface name.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    json::Value format;
    format["const"] = 1;
    format["description"] = "Format version. Must be 1.";
    json::Value surface;
    surface["type"] = "string";
    surface["maxLength"] = 64;
    surface["description"] =
        "A name for the surface, such as \"wood\", that raycasts and contacts report.";
    schema["properties"] = json::Object{
        {"format", std::move(format)},
        {"friction", number("How much it grips, 0 for ice and 1 by default.", 0.0)},
        {"rough", flag("Its friction wins over a smoother material's.")},
        {"bounce", number("How much it bounces, 0 to 1.", 0.0)},
        {"absorbent", flag("Its bounce is taken from the other's instead of added.")},
        {"density", number("Kilograms a cubic meter (2D: a square meter of 64 pixels a side); a "
                           "rigid body made of it weighs its shapes' volume times this. 0 keeps "
                           "the body's mass.",
                           0.0)},
        {"strength", number("Kilogram-meters per second of one impact that breaks a "
                            "Destructible3D made of it whose damage_threshold is 0; 0 never.",
                            0.0)},
        {"surface", std::move(surface)},
    };
    schema["properties"]["bounce"]["maximum"] = 1.0;
    return schema;
}

} // namespace

float computed_friction(const PhysicsMaterial& material) {
    return material.rough ? -material.friction : material.friction;
}

float computed_bounce(const PhysicsMaterial& material) {
    return material.absorbent ? -material.bounce : material.bounce;
}

json::Value physics_material_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

Result<PhysicsMaterial> parse_physics_material(const json::Value& value) {
    const std::vector<json::ValidationError> errors =
        json::validate(value, physics_material_schema());
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    PhysicsMaterial material;
    const auto read = [&](std::string_view key, float& into) {
        if (const json::Value* member = value.find(key); member != nullptr) {
            into = static_cast<float>(member->as_double());
        }
    };
    read("friction", material.friction);
    read("bounce", material.bounce);
    read("density", material.density);
    read("strength", material.strength);
    if (const json::Value* rough = value.find("rough"); rough != nullptr) {
        material.rough = rough->as_bool();
    }
    if (const json::Value* absorbent = value.find("absorbent"); absorbent != nullptr) {
        material.absorbent = absorbent->as_bool();
    }
    if (const json::Value* surface = value.find("surface"); surface != nullptr) {
        material.surface = surface->as_string();
    }
    return material;
}

bool is_physics_material_path(std::string_view path) {
    return path.ends_with(".physics_material.json");
}

} // namespace slash::world
