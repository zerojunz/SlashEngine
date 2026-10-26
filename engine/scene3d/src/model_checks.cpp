#include "model_internal.hpp"

#include "slash/core/text.hpp"
#include "slash/scene3d/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slash::scene3d {

namespace {

constexpr std::array<std::string_view, 3> kAxes = {"x", "y", "z"};
// How far a size may pass its bound and a pivot stray, so numbers rounded as summaries round
// them still pass.
constexpr float kSizeSlack = 1e-4F;
constexpr float kPivotSlack = 0.01F;

json::Value vector3(std::string_view doc, bool unit) {
    json::Value out;
    out["type"] = "array";
    out["minItems"] = 3;
    out["maxItems"] = 3;
    out["items"]["type"] = "number";
    out["items"]["minimum"] = 0.0;
    if (unit) {
        out["items"]["maximum"] = 1.0;
    }
    out["description"] = doc;
    return out;
}

json::Value names(std::string_view doc) {
    json::Value out;
    out["type"] = "array";
    out["items"]["type"] = "string";
    out["items"]["minLength"] = 1;
    out["description"] = doc;
    return out;
}

// A number as messages give it: to a ten-thousandth, with no trailing zeros.
std::string number(float value) {
    const double tenths = std::round(static_cast<double>(value) * 1e4);
    if (std::fmod(tenths, 1e4) == 0.0) {
        return std::to_string(static_cast<std::int64_t>(tenths / 1e4));
    }
    return json::write(json::Value(tenths / 1e4), json::Style::Compact);
}

std::string at(std::string_view list, std::size_t index) {
    std::string out = "/";
    out += list;
    out += "/";
    out += std::to_string(index);
    return out;
}

// Each name of list the model lacks, with the closest one it has; a bone that is a node of the
// model but no skin's joint is named so.
void require(const json::Value& checks, std::string_view list, std::string_view what,
             const std::vector<std::string>& have, std::vector<json::ValidationError>& errors,
             const std::vector<std::string>& nodes = {}) {
    const json::Value* wanted = checks.find(list);
    if (wanted == nullptr) {
        return;
    }
    const json::Array& items = wanted->as_array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string& name = items[i].as_string();
        if (std::ranges::find(have, name) != have.end()) {
            continue;
        }
        if (std::ranges::find(nodes, name) != nodes.end()) {
            errors.push_back({at(list, i), quoted(name) + " is a node of the model but no skin's " +
                                               std::string(what)});
            continue;
        }
        std::string message = "the model has no ";
        message += what;
        message += " ";
        message += quoted(name);
        if (have.empty()) {
            message += "; it has none";
        } else if (const std::string_view near = closest_match(name, have); !near.empty()) {
            message += " (did you mean " + quoted(near) + "?)";
        }
        errors.push_back({at(list, i), std::move(message)});
    }
}

} // namespace

json::Value model_checks_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] =
        "What the model must be, checked whenever it is validated: its size, pivot, triangle "
        "budget, and the materials, nodes, bones and animations other files name.";
    json::Value& properties = schema["properties"];
    properties["size"]["type"] = "object";
    properties["size"]["additionalProperties"] = false;
    properties["size"]["description"] = "Bounds on its size, the box its vertices fill at rest.";
    properties["size"]["properties"]["min"] =
        vector3("At least these meters, as [x, y, z].", false);
    properties["size"]["properties"]["max"] = vector3("At most these meters, as [x, y, z].", false);
    properties["pivot"] = vector3("Where its origin is in its box, within 0.01 on each axis: 0 at "
                                  "the low side, 1 at the high one. [0.5, 0, 0.5] stands on the "
                                  "ground, centered.",
                                  true);
    properties["max_triangles"]["type"] = "integer";
    properties["max_triangles"]["minimum"] = 0;
    properties["max_triangles"]["description"] = "The most triangles it may draw.";
    properties["materials"] = names("Material names it must have, as other files set them by.");
    properties["nodes"] =
        names("Node names it must have: parts that move or attach, attachment points.");
    properties["bones"] = names("Bones a skin of it must have, as animations and "
                                "BoneAttachment3D name them.");
    properties["animations"] = names("Animations it must have, as AnimationPlayer plays them.");
    return schema;
}

std::vector<json::ValidationError> check_model_against(const Model& model,
                                                       const json::Value& checks) {
    std::vector<json::ValidationError> errors;
    if (!checks.is_object()) {
        return errors;
    }
    const ModelSummary summary = summarize(model);
    const Vec3 size = summary.bounds.max - summary.bounds.min;
    const std::array<float, 3> sizes = {size.x, size.y, size.z};
    if (const json::Value* bounds = checks.find("size"); bounds != nullptr) {
        for (const std::string_view side : {"min", "max"}) {
            const json::Value* limit = bounds->find(side);
            if (limit == nullptr) {
                continue;
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto bound = static_cast<float>(limit->as_array()[axis].as_double());
                const bool fails = side == "min" ? sizes[axis] < bound - kSizeSlack
                                                 : sizes[axis] > bound + kSizeSlack;
                if (!fails) {
                    continue;
                }
                std::string where = "/size/";
                where += side;
                where += "/";
                where += std::to_string(axis);
                std::string message = "the model is ";
                message += number(sizes[axis]);
                message += " m along ";
                message += kAxes[axis];
                message += side == "min" ? ", less than " : ", more than ";
                message += number(bound);
                errors.push_back({std::move(where), std::move(message)});
            }
        }
    }
    if (const json::Value* pivot = checks.find("pivot"); pivot != nullptr) {
        const std::array<float, 3> pivots = {summary.pivot.x, summary.pivot.y, summary.pivot.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto wanted = static_cast<float>(pivot->as_array()[axis].as_double());
            if (std::abs(pivots[axis] - wanted) <= kPivotSlack) {
                continue;
            }
            std::string message = "the model's origin is ";
            message += number(pivots[axis]);
            message += " of the way along its box in ";
            message += kAxes[axis];
            message += ", not ";
            message += number(wanted);
            errors.push_back({at("pivot", axis), std::move(message)});
        }
    }
    if (const json::Value* budget = checks.find("max_triangles"); budget != nullptr) {
        const auto most = static_cast<std::size_t>(budget->as_int());
        if (summary.triangles > most) {
            errors.push_back(
                {"/max_triangles", "the model has " + std::to_string(summary.triangles) +
                                       " triangles, more than " + std::to_string(most)});
        }
    }
    std::vector<std::string> materials;
    for (const std::string& name : summary.materials) {
        if (!name.empty() && std::ranges::find(materials, name) == materials.end()) {
            materials.push_back(name);
        }
    }
    require(checks, "materials", "material", materials, errors);
    // An OBJ model has no nodes, only the parts its objects make.
    std::vector<std::string> nodes;
    nodes.reserve(model.nodes.size() + model.parts.size());
    for (const ModelNode& node : model.nodes) {
        nodes.push_back(node.name);
    }
    for (const ModelPart& part : model.parts) {
        if (part.node_index < 0) {
            nodes.push_back(part.node);
        }
    }
    require(checks, "nodes", "node", nodes, errors);
    std::vector<std::string> bones;
    for (const ModelSkin& skin : model.skins) {
        for (const std::size_t joint : skin.joints) {
            const std::string& name = model.nodes[joint].name;
            if (std::ranges::find(bones, name) == bones.end()) {
                bones.push_back(name);
            }
        }
    }
    require(checks, "bones", "bone", bones, errors, nodes);
    require(checks, "animations", "animation", summary.animations, errors);
    return errors;
}

} // namespace slash::scene3d
