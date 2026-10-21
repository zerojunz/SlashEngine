#include "slash/canvas/gpu_particles.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace slash::canvas {
namespace {

using render::EmissionShape;
using render::ParticleParam;

constexpr std::array<std::pair<std::string_view, EmissionShape>, 7> kShapes{{
    {"point", EmissionShape::Point},
    {"sphere", EmissionShape::Sphere},
    {"sphere_surface", EmissionShape::SphereSurface},
    {"box", EmissionShape::Box},
    {"points", EmissionShape::Points},
    {"directed_points", EmissionShape::DirectedPoints},
    {"ring", EmissionShape::Ring},
}};

constexpr std::array<std::pair<std::string_view, render::ParticleCollision>, 3> kCollisionModes{{
    {"disabled", render::ParticleCollision::Disabled},
    {"rigid", render::ParticleCollision::Rigid},
    {"hide_on_contact", render::ParticleCollision::HideOnContact},
}};

constexpr std::array<std::pair<std::string_view, render::SubEmitterMode>, 5> kSubEmitterModes{{
    {"disabled", render::SubEmitterMode::Disabled},
    {"constant", render::SubEmitterMode::Constant},
    {"at_end", render::SubEmitterMode::AtEnd},
    {"at_collision", render::SubEmitterMode::AtCollision},
    {"at_start", render::SubEmitterMode::AtStart},
}};

// Godot's names for the parameters, by ParticleParam, with what each is and its range.
struct ParamInfo {
    std::string_view name;
    std::string_view what;
    std::optional<double> min;
    std::optional<double> max;
    bool curve = true;
    std::string_view curve_name; // its curve's name, when not name + "_curve"
};
constexpr std::array<ParamInfo, render::kParticleParams> kParams{{
    {"initial_velocity", "Starting speed in pixels per second along the spread direction",
     std::nullopt, std::nullopt, false, ""},
    {"angular_velocity", "Degrees per second it turns", std::nullopt, std::nullopt, true, ""},
    {"orbit_velocity", "Turns per second round the emitter", std::nullopt, std::nullopt, true, ""},
    {"linear_accel", "Pixels per second squared along its velocity", std::nullopt, std::nullopt,
     true, ""},
    {"radial_accel", "Pixels per second squared away from the emitter", std::nullopt, std::nullopt,
     true, ""},
    {"tangential_accel", "Pixels per second squared round the emitter", std::nullopt, std::nullopt,
     true, ""},
    {"damping", "Pixels per second squared it slows by", 0.0, std::nullopt, true, ""},
    {"angle", "Rotation in degrees", std::nullopt, std::nullopt, true, ""},
    {"scale", "Size, times the texture's", std::nullopt, std::nullopt, true, ""},
    {"hue_variation", "Turns its hue, as a fraction of the color wheel", -1.0, 1.0, true, ""},
    {"anim_speed", "Times its frames play over its life", 0.0, std::nullopt, true, ""},
    {"anim_offset", "The frame it starts on, as a fraction of the frames; its curve adds", 0.0, 1.0,
     true, ""},
    {"radial_velocity", "Pixels per second away from velocity_pivot", std::nullopt, std::nullopt,
     true, ""},
    {"turbulence_influence", "How far turbulence turns its velocity, from 0 to 1", 0.0, 1.0, true,
     "turbulence_influence_over_life"},
    {"turbulence_initial_displacement", "Pixels turbulence moves it at the start", std::nullopt,
     std::nullopt, false, ""},
}};

json::Value number(std::string_view description, double fallback,
                   std::optional<double> minimum = std::nullopt,
                   std::optional<double> maximum = std::nullopt) {
    json::Value schema;
    schema["type"] = "number";
    if (minimum) {
        schema["minimum"] = *minimum;
    }
    if (maximum) {
        schema["maximum"] = *maximum;
    }
    schema["default"] = fallback;
    schema["description"] = description;
    return schema;
}

json::Value numbers(std::size_t count, std::optional<double> minimum = std::nullopt) {
    json::Value schema;
    schema["type"] = "array";
    schema["minItems"] = static_cast<std::int64_t>(count);
    schema["maxItems"] = static_cast<std::int64_t>(count);
    schema["items"]["type"] = "number";
    if (minimum) {
        schema["items"]["minimum"] = *minimum;
    }
    return schema;
}

json::Value vector(std::string_view description, Vec2 fallback,
                   std::optional<double> minimum = std::nullopt) {
    json::Value schema = numbers(2, minimum);
    schema["default"] = json::Array{fallback.x, fallback.y};
    schema["description"] = description;
    return schema;
}

json::Value color_schema(std::string_view description) {
    json::Value schema = numbers(4);
    schema["items"]["minimum"] = 0;
    schema["description"] = description;
    return schema;
}

json::Value curve_schema(std::string_view description) {
    json::Value point = numbers(2);
    point["prefixItems"] = json::Array{number("Offset over the particle's life.", 0.0, 0.0, 1.0),
                                       number("Value there.", 0.0)};
    json::Value schema;
    schema["type"] = "array";
    schema["maxItems"] = static_cast<std::int64_t>(render::kMaxParticleCurvePoints);
    schema["items"] = point;
    schema["description"] = description;
    return schema;
}

json::Value gradient_schema(std::string_view description) {
    json::Value stop;
    stop["type"] = "object";
    stop["additionalProperties"] = false;
    stop["required"] = json::Array{"offset", "color"};
    stop["properties"]["offset"] = number("Where on the gradient, 0 to 1.", 0.0, 0.0, 1.0);
    stop["properties"]["color"] = color_schema("[r, g, b, a], from 0 to 1.");
    json::Value schema;
    schema["type"] = "array";
    schema["maxItems"] = static_cast<std::int64_t>(render::kMaxParticleCurvePoints);
    schema["items"] = stop;
    schema["description"] = description;
    return schema;
}

Vec2 vec2(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

Vec4 vec4(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double()),
            static_cast<float>(items[2].as_double()), static_cast<float>(items[3].as_double())};
}

std::string param_name(std::size_t param, std::string_view end) {
    return std::string(kParams.at(param).name) + std::string(end);
}

std::string curve_name(std::size_t param) {
    const ParamInfo& info = kParams.at(param);
    return info.curve_name.empty() ? param_name(param, "_curve") : std::string(info.curve_name);
}

} // namespace

json::Value process_material_schema() {
    json::Value shape;
    shape["type"] = "string";
    json::Array shapes;
    for (const auto& [name, value] : kShapes) {
        shapes.emplace_back(std::string(name));
    }
    shape["enum"] = std::move(shapes);
    shape["default"] = "point";
    shape["description"] =
        "Where particles start: at the emitter, in a circle (sphere), on its edge, in a box, at "
        "one of emission_points, moving along its emission_normals (directed_points), or in a "
        "ring.";

    json::Value points;
    points["type"] = "array";
    points["maxItems"] = 65536;
    points["items"] = numbers(2);
    points["description"] = "Points particles start at, in pixels; each takes one at random.";
    json::Value normals = points;
    normals["description"] = "For directed_points, the direction each point emits along: none, "
                             "or one per point.";
    json::Value colors;
    colors["type"] = "array";
    colors["maxItems"] = 65536;
    colors["items"] = color_schema("[r, g, b, a], from 0 to 1.");
    colors["description"] = "Colors multiplying each point's particles: none, or one per point.";

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine particle process material";
    schema["description"] = "How a GPUParticles2D's particles start and move: Godot's "
                            "ParticleProcessMaterial in 2D, with its property names. Angles "
                            "are in degrees.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    const render::ParticleProcess defaults;
    properties["lifetime_randomness"] =
        number("Shortens each particle's life by up to this fraction.", 0.0, 0.0, 1.0);
    properties["particle_flag_align_y"]["type"] = "boolean";
    properties["particle_flag_align_y"]["default"] = false;
    properties["particle_flag_align_y"]["description"] =
        "Turn each particle so its y axis points along its velocity.";
    properties["particle_flag_damping_as_friction"]["type"] = "boolean";
    properties["particle_flag_damping_as_friction"]["default"] = false;
    properties["particle_flag_damping_as_friction"]["description"] =
        "Damping grows with speed, as friction.";
    properties["emission_shape"] = std::move(shape);
    properties["emission_shape_offset"] =
        vector("Moves the emission shape, in pixels.", defaults.emission_shape_offset);
    properties["emission_shape_scale"] =
        vector("Scales the emission shape.", defaults.emission_shape_scale);
    properties["emission_sphere_radius"] =
        number("The sphere's radius in pixels.", defaults.emission_sphere_radius, 0.0);
    properties["emission_box_extents"] =
        vector("Half the box's width and height, in pixels.", defaults.emission_box_extents, 0.0);
    properties["emission_points"] = std::move(points);
    properties["emission_normals"] = std::move(normals);
    properties["emission_colors"] = std::move(colors);
    properties["emission_ring_radius"] =
        number("The ring's outer radius in pixels.", defaults.emission_ring_radius, 0.0);
    properties["emission_ring_inner_radius"] =
        number("The ring's inner radius in pixels.", defaults.emission_ring_inner_radius, 0.0);
    properties["inherit_velocity_ratio"] =
        number("How much of the emitter's velocity particles start with.", 0.0);
    properties["velocity_pivot"] =
        vector("What radial_velocity pushes away from, in pixels.", defaults.velocity_pivot);
    properties["direction"] = vector("Which way particles go, as [x, y].", defaults.direction);
    properties["spread"] = number("Degrees either side of direction.", defaults.spread, 0.0, 180.0);
    properties["gravity"] = vector("In pixels per second squared, as [x, y].", defaults.gravity);
    for (std::size_t i = 0; i < kParams.size(); ++i) {
        const ParamInfo& info = kParams.at(i);
        const std::string what(info.what);
        properties[param_name(i, "_min")] =
            number(what + ", at least.", defaults.param_min.at(i), info.min, info.max);
        properties[param_name(i, "_max")] =
            number(what + ", at most.", defaults.param_max.at(i), info.min, info.max);
        if (info.curve) {
            properties[curve_name(i)] =
                curve_schema(i == static_cast<std::size_t>(ParticleParam::AnimOffset)
                                 ? "Adds to it over each particle's life: points [offset, value]."
                                 : "Multiplies it over each particle's life: points [offset, "
                                   "value], joined by straight lines. None is 1.");
        }
    }
    properties["velocity_limit_curve"] =
        curve_schema("The fastest a particle moves over its life, in pixels per second.");
    properties["alpha_curve"] = curve_schema("Multiplies alpha over each particle's life.");
    properties["emission_curve"] =
        curve_schema("Brightens each particle over its life: its color times 1 plus this.");
    properties["color"] = color_schema("Multiplies the texture, as [r, g, b, a] from 0 to 1.");
    properties["color"]["default"] = json::Array{1, 1, 1, 1};
    properties["color_ramp"] = gradient_schema(
        "Colors over each particle's life, multiplying color: stops {offset, color}, in order.");
    properties["color_initial_ramp"] = gradient_schema(
        "Each particle multiplies its color by one picked at random from this gradient.");
    properties["turbulence_enabled"]["type"] = "boolean";
    properties["turbulence_enabled"]["default"] = false;
    properties["turbulence_enabled"]["description"] =
        "Turn particles with moving curl noise, by turbulence_influence.";
    properties["turbulence_noise_strength"] =
        number("How sharply the noise turns.", defaults.turbulence_noise_strength, 0.0, 20.0);
    properties["turbulence_noise_scale"] =
        number("The noise's size: higher is larger swirls, 10 the largest Godot's slider gives.",
               defaults.turbulence_noise_scale, 0.0);
    properties["turbulence_noise_speed"] = numbers(3);
    properties["turbulence_noise_speed"]["default"] = json::Array{0, 0, 0};
    properties["turbulence_noise_speed"]["description"] =
        "How the noise moves, per second, as [x, y, z].";
    properties["turbulence_noise_speed_random"] =
        number("How fast the noise changes as it moves.", defaults.turbulence_noise_speed_random,
               0.0, 4.0);
    json::Array modes;
    for (const auto& [name, value] : kCollisionModes) {
        modes.emplace_back(std::string(name));
    }
    properties["collision_mode"]["enum"] = std::move(modes);
    properties["collision_mode"]["default"] = "disabled";
    properties["collision_mode"]["description"] =
        "What particles do on touching a LightOccluder2D with sdf_collision: bounce off "
        "(rigid) or die (hide_on_contact).";
    properties["collision_friction"] =
        number("For rigid, the fraction of the speed along the surface lost.", 0.0, 0.0, 1.0);
    properties["collision_bounce"] =
        number("For rigid, the fraction of the speed into the surface sent back.", 0.0, 0.0, 1.0);
    properties["collision_use_scale"]["type"] = "boolean";
    properties["collision_use_scale"]["default"] = false;
    properties["collision_use_scale"]["description"] =
        "Multiply the emitter's collision_base_size by each particle's scale.";
    json::Array sub_modes;
    for (const auto& [name, value] : kSubEmitterModes) {
        sub_modes.emplace_back(std::string(name));
    }
    properties["sub_emitter_mode"]["enum"] = std::move(sub_modes);
    properties["sub_emitter_mode"]["default"] = "disabled";
    properties["sub_emitter_mode"]["description"] =
        "When each particle starts particles in the emitter's sub_emitter: constant, at_end of "
        "its life, at_collision or at_start.";
    properties["sub_emitter_frequency"]["type"] = "number";
    properties["sub_emitter_frequency"]["exclusiveMinimum"] = 0.0;
    properties["sub_emitter_frequency"]["default"] = defaults.sub_emitter_frequency;
    properties["sub_emitter_frequency"]["description"] =
        "For constant, particles started a second of each particle's life.";
    for (const auto& [name, what] :
         {std::pair{"sub_emitter_amount_at_end", "For at_end, particles started at its end."},
          std::pair{"sub_emitter_amount_at_collision",
                    "For at_collision, particles started each time it touches a shape."},
          std::pair{"sub_emitter_amount_at_start", "For at_start, particles started."}}) {
        properties[name]["type"] = "integer";
        properties[name]["minimum"] = 1;
        properties[name]["default"] = 1;
        properties[name]["description"] = what;
    }
    properties["sub_emitter_keep_velocity"]["type"] = "boolean";
    properties["sub_emitter_keep_velocity"]["default"] = false;
    properties["sub_emitter_keep_velocity"]["description"] =
        "Particles started move as the particle starting them does, instead of as the "
        "sub-emitter's process material says.";
    return schema;
}

std::vector<json::ValidationError> validate_process_material(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, process_material_schema());
    if (!errors.empty()) {
        return errors;
    }
    // Curves and gradients go along their offsets.
    for (const auto& [name, value] : doc.as_object()) {
        const bool curve = name.ends_with("_curve") || name == "turbulence_influence_over_life";
        const bool gradient = name == "color_ramp" || name == "color_initial_ramp";
        if (!curve && !gradient) {
            continue;
        }
        const json::Array& points = value.as_array();
        const auto offset = [&](std::size_t i) -> const json::Value& {
            return curve ? points[i].as_array()[0] : *points[i].find("offset");
        };
        for (std::size_t i = 1; i < points.size(); ++i) {
            if (offset(i).as_double() < offset(i - 1).as_double()) {
                errors.push_back({"/" + name + "/" + std::to_string(i) + (curve ? "/0" : "/offset"),
                                  "offsets go in order: " + json::write(offset(i)) +
                                      " comes after " + json::write(offset(i - 1))});
            }
        }
    }
    const json::Value* shape = doc.find("emission_shape");
    const json::Value* points = doc.find("emission_points");
    const std::size_t count = points != nullptr ? points->as_array().size() : 0;
    if (shape != nullptr &&
        (shape->as_string() == "points" || shape->as_string() == "directed_points") && count == 0) {
        errors.push_back({"/emission_shape", "\"" + shape->as_string() +
                                                 "\" needs emission_points to start particles at"});
    }
    for (const std::string_view field : {"emission_normals", "emission_colors"}) {
        const json::Value* list = doc.find(field);
        if (list != nullptr && !list->as_array().empty() && list->as_array().size() != count) {
            errors.push_back(
                {"/" + std::string(field), "has " + std::to_string(list->as_array().size()) +
                                               " items for " + std::to_string(count) +
                                               " emission_points; give none, or one per point"});
        }
    }
    return errors;
}

Result<render::ParticleProcess> parse_process_material(const json::Value& doc) {
    if (const auto errors = validate_process_material(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    render::ParticleProcess out;
    const auto real = [&](std::string_view key, float& field) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            field = static_cast<float>(value->as_double());
        }
    };
    const auto pair = [&](std::string_view key, Vec2& field) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            field = vec2(*value);
        }
    };
    const auto flag = [&](std::string_view key, bool& field) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            field = value->as_bool();
        }
    };
    const auto curve = [&](std::string_view key, render::ParticleCurve& field) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            for (const json::Value& point : value->as_array()) {
                field.push_back(vec2(point));
            }
        }
    };
    const auto gradient = [&](std::string_view key, render::ParticleGradient& field) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            for (const json::Value& stop : value->as_array()) {
                field.push_back({.offset = static_cast<float>(stop.find("offset")->as_double()),
                                 .color = vec4(*stop.find("color"))});
            }
        }
    };
    if (const json::Value* shape = doc.find("emission_shape"); shape != nullptr) {
        for (const auto& [name, value] : kShapes) {
            if (shape->as_string() == name) {
                out.emission_shape = value;
            }
        }
    }
    pair("emission_shape_offset", out.emission_shape_offset);
    pair("emission_shape_scale", out.emission_shape_scale);
    real("emission_sphere_radius", out.emission_sphere_radius);
    pair("emission_box_extents", out.emission_box_extents);
    for (const auto& [key, field] : {std::pair{"emission_points", &out.emission_points},
                                     std::pair{"emission_normals", &out.emission_normals}}) {
        if (const json::Value* list = doc.find(key); list != nullptr) {
            for (const json::Value& point : list->as_array()) {
                field->push_back(vec2(point));
            }
        }
    }
    if (const json::Value* list = doc.find("emission_colors"); list != nullptr) {
        for (const json::Value& color : list->as_array()) {
            out.emission_colors.push_back(vec4(color));
        }
    }
    real("emission_ring_radius", out.emission_ring_radius);
    real("emission_ring_inner_radius", out.emission_ring_inner_radius);
    pair("direction", out.direction);
    real("spread", out.spread);
    real("inherit_velocity_ratio", out.inherit_velocity_ratio);
    pair("velocity_pivot", out.velocity_pivot);
    pair("gravity", out.gravity);
    for (std::size_t i = 0; i < kParams.size(); ++i) {
        real(param_name(i, "_min"), out.param_min.at(i));
        real(param_name(i, "_max"), out.param_max.at(i));
        if (kParams.at(i).curve) {
            curve(curve_name(i), out.param_curve.at(i));
        }
    }
    curve("velocity_limit_curve", out.velocity_limit_curve);
    curve("alpha_curve", out.alpha_curve);
    curve("emission_curve", out.emission_curve);
    if (const json::Value* color = doc.find("color"); color != nullptr) {
        out.color = vec4(*color);
    }
    gradient("color_ramp", out.color_ramp);
    gradient("color_initial_ramp", out.color_initial_ramp);
    real("lifetime_randomness", out.lifetime_randomness);
    flag("particle_flag_align_y", out.align_y);
    flag("particle_flag_damping_as_friction", out.damping_as_friction);
    flag("turbulence_enabled", out.turbulence_enabled);
    real("turbulence_noise_strength", out.turbulence_noise_strength);
    real("turbulence_noise_scale", out.turbulence_noise_scale);
    if (const json::Value* speed = doc.find("turbulence_noise_speed"); speed != nullptr) {
        const json::Array& items = speed->as_array();
        out.turbulence_noise_speed = {static_cast<float>(items[0].as_double()),
                                      static_cast<float>(items[1].as_double()),
                                      static_cast<float>(items[2].as_double())};
    }
    real("turbulence_noise_speed_random", out.turbulence_noise_speed_random);
    if (const json::Value* mode = doc.find("collision_mode"); mode != nullptr) {
        for (const auto& [name, value] : kCollisionModes) {
            if (mode->as_string() == name) {
                out.collision_mode = value;
            }
        }
    }
    real("collision_friction", out.collision_friction);
    real("collision_bounce", out.collision_bounce);
    flag("collision_use_scale", out.collision_use_scale);
    if (const json::Value* mode = doc.find("sub_emitter_mode"); mode != nullptr) {
        for (const auto& [name, value] : kSubEmitterModes) {
            if (mode->as_string() == name) {
                out.sub_emitter_mode = value;
            }
        }
    }
    real("sub_emitter_frequency", out.sub_emitter_frequency);
    for (const auto& [key, field] :
         {std::pair{"sub_emitter_amount_at_end", &out.sub_emitter_amount_at_end},
          std::pair{"sub_emitter_amount_at_collision", &out.sub_emitter_amount_at_collision},
          std::pair{"sub_emitter_amount_at_start", &out.sub_emitter_amount_at_start}}) {
        if (const json::Value* value = doc.find(key); value != nullptr) {
            *field = static_cast<std::uint32_t>(
                std::min<std::int64_t>(value->as_int(), std::numeric_limits<std::uint32_t>::max()));
        }
    }
    flag("sub_emitter_keep_velocity", out.sub_emitter_keep_velocity);
    return out;
}

} // namespace slash::canvas
