#include "slash/command/model_cleanup.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/cleanup.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

json::Array numbers(Vec3 v) {
    // Rounded to a tenth of a millimeter, as summaries round sizes.
    const auto round = [](float x) {
        return static_cast<double>(static_cast<long long>((static_cast<double>(x) * 1e4) +
                                                          (x < 0.0F ? -0.5 : 0.5))) /
               1e4;
    };
    return json::Array{round(v.x), round(v.y), round(v.z)};
}

// A model file's path with its extensions replaced by ".glb": where a cleanup goes by default.
std::string glb_path(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find('.', name);
    return std::string(path.substr(0, dot)) + ".glb";
}

Reply model_cleanup(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    if (!scene3d::is_model_path(path) || path.find("::") != std::string::npos) {
        return param_error(code::kInvalidParams, "/path",
                           quoted(path) +
                               " isn't a model file: give a .gltf, .glb, .obj, .fbx, .blend or "
                               ".model.json file");
    }
    const json::Value* out_param = params.find("out");
    const std::string out = out_param != nullptr ? out_param->as_string() : glb_path(path);
    if (!out.ends_with(".glb")) {
        return param_error(code::kInvalidParams, "/out",
                           quoted(out) + " isn't a .glb file, which a cleanup writes");
    }
    if (auto full = session.resolve_path(out); !full) {
        return param_error(code::kInvalidParams, "/out", full.error().message);
    }

    scene3d::CleanupOptions options;
    if (const json::Value* up = params.find("up"); up != nullptr) {
        options.up = up->as_string();
    }
    if (const json::Value* scale = params.find("scale"); scale != nullptr) {
        options.scale = static_cast<float>(scale->as_double());
    }
    if (const json::Value* height = params.find("height"); height != nullptr) {
        if (params.find("scale") != nullptr) {
            return param_error(code::kInvalidParams, "/height", "give scale or height, not both");
        }
        options.height = static_cast<float>(height->as_double());
    }
    if (const json::Value* pivot = params.find("pivot"); pivot != nullptr) {
        const json::Array& at = pivot->as_array();
        options.pivot =
            Vec3{static_cast<float>(at[0].as_double()), static_cast<float>(at[1].as_double()),
                 static_cast<float>(at[2].as_double())};
    }
    // The budget: the call's, else the storyboard's tightest for the file written.
    std::optional<storyboard::BudgetLimit> budget;
    if (const json::Value* triangles = params.find("triangles"); triangles != nullptr) {
        options.triangles = static_cast<std::size_t>(triangles->as_int());
    } else {
        auto loaded = storyboard::load(
            [&session](std::string_view file) { return session.read_project_file(file); });
        if (loaded) {
            budget = storyboard::budget_for(loaded.value().entries, out, "triangles");
        }
        if (budget) {
            options.triangles = static_cast<std::size_t>(budget->most);
        }
    }
    const json::Value* uvs = params.find("uvs");
    options.uvs = uvs == nullptr || uvs->as_bool();
    if (const json::Value* collision = params.find("collision"); collision != nullptr) {
        options.collision = collision->as_string();
    }
    if (const json::Value* max_hulls = params.find("max_hulls"); max_hulls != nullptr) {
        if (options.collision != "decompose") {
            return param_error(code::kInvalidParams, "/max_hulls",
                               "max_hulls goes with collision \"decompose\"");
        }
        options.max_hulls = static_cast<std::size_t>(max_hulls->as_int());
    }

    const scene3d::ModelReader read = [&session](std::string_view file) -> Result<std::string> {
        auto bytes = session.read_project_file(file);
        if (!bytes) {
            return bytes.error();
        }
        if (!bytes.value()) {
            return make_error(ErrorCode::NotFound, quoted(file) + " doesn't exist");
        }
        return std::move(*bytes.value());
    };
    if (const json::Value* delight = params.find("delight"); delight != nullptr) {
        options.delight = static_cast<float>(delight->as_double());
        options.read = read;
    }
    auto loaded = session.assets().load<scene3d::Model>(path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/path", loaded.error().message);
    }
    scene3d::Model model = *loaded.value();
    auto report = scene3d::cleanup_model(model, options);
    if (!report) {
        std::string at = "/path";
        const std::string& message = report.error().message;
        if (message.starts_with("up ")) {
            at = "/up";
        } else if (message.starts_with("the simplifier")) {
            at = budget ? "/out" : "/triangles";
        } else if (options.delight > 0.0F && message.starts_with("\"")) {
            at = "/delight";
        } else if (message.starts_with("the model has no triangles") ||
                   message.starts_with("the model's triangles span")) {
            at = "/collision";
        } else if (message.find("height") != std::string::npos) {
            at = "/height";
        }
        std::string text = message;
        if (budget && at == "/out") {
            text += ", which " + budget->id + " allows; pass triangles to aim elsewhere";
        }
        return param_error(code::kInvalidParams, at, text);
    }
    auto glb = scene3d::write_glb(model, out, read);
    if (!glb) {
        return Reply::failure(glb.error());
    }

    const scene3d::ModelSummary summary = scene3d::summarize(model);
    json::Value result;
    result["out"] = out;
    result["triangles"] = json::Array{static_cast<std::int64_t>(report.value().triangles_before),
                                      static_cast<std::int64_t>(report.value().triangles)};
    if (budget) {
        result["budget"] = budget->id;
    }
    result["size"] = numbers(summary.bounds.max - summary.bounds.min);
    result["pivot"] = numbers(summary.pivot);
    if (report.value().unwrapped > 0) {
        result["unwrapped"] = static_cast<std::int64_t>(report.value().unwrapped);
    }
    if (report.value().delit > 0) {
        result["delit"] = static_cast<std::int64_t>(report.value().delit);
        if (const std::optional<Vec3>& light = report.value().light; light) {
            result["light"] = numbers(*light);
        }
    }
    if (!summary.collision.empty()) {
        // How many shapes of each kind it collides as.
        std::int64_t convex = 0;
        for (const scene3d::ModelSummary::Collision& shape : summary.collision) {
            convex += shape.convex ? 1 : 0;
        }
        json::Value shapes;
        if (convex > 0) {
            shapes["convex"] = convex;
        }
        if (convex < static_cast<std::int64_t>(summary.collision.size())) {
            shapes["trimesh"] = static_cast<std::int64_t>(summary.collision.size()) - convex;
        }
        result["collision"] = std::move(shapes);
    }
    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    if (before.value() == glb.value()) {
        session.mark_unchanged();
        result["unchanged"] = true;
        return Reply::success(std::move(result));
    }
    if (auto staged = session.stage_file(out, glb.value()); !staged) {
        return Reply::failure(staged.error());
    }
    session.add_undo_file(out, std::move(before).value(), std::move(glb).value());
    return Reply::success(std::move(result));
}

Reply model_retarget(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    if (!scene3d::is_model_path(path) || path.find("::") != std::string::npos) {
        return param_error(code::kInvalidParams, "/path",
                           quoted(path) +
                               " isn't a model file: give a .gltf, .glb, .fbx or .blend file");
    }
    const json::Value* out_param = params.find("out");
    const std::string out = out_param != nullptr ? out_param->as_string() : glb_path(path);
    if (!out.ends_with(".glb")) {
        return param_error(code::kInvalidParams, "/out",
                           quoted(out) + " isn't a .glb file, which a retarget writes");
    }
    if (auto full = session.resolve_path(out); !full) {
        return param_error(code::kInvalidParams, "/out", full.error().message);
    }
    auto loaded = session.assets().load<scene3d::Model>(path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/path", loaded.error().message);
    }
    scene3d::Model model = *loaded.value();
    if (model.skins.empty()) {
        return param_error(code::kInvalidParams, "/path",
                           quoted(path) + " has no skin: only a rigged model can be retargeted");
    }

    // The guessed map, then the call's changes to it.
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    if (const json::Value* map = params.find("bone_map"); map != nullptr) {
        std::vector<std::string_view> profile_names;
        profile_names.reserve(scene3d::humanoid_profile().size());
        for (const scene3d::ProfileBone& bone : scene3d::humanoid_profile()) {
            profile_names.push_back(bone.name);
        }
        std::vector<std::string_view> node_names;
        node_names.reserve(model.nodes.size());
        for (const scene3d::ModelNode& node : model.nodes) {
            node_names.push_back(node.name);
        }
        for (const auto& [profile_bone, value] : map->as_object()) {
            const std::string at = "/bone_map/" + json::pointer_escape(profile_bone);
            if (scene3d::find_profile_bone(profile_bone) < 0) {
                std::string message =
                    quoted(profile_bone) + " isn't a bone of the humanoid profile";
                if (const std::string_view near = closest_match(profile_bone, profile_names);
                    !near.empty()) {
                    message += "; did you mean " + quoted(near) + "?";
                }
                return param_error(code::kInvalidParams, at, message);
            }
            const std::string& bone = value.as_string();
            if (!bone.empty() && scene3d::find_node(model, bone) < 0) {
                std::string message = quoted(path) + " has no bone " + quoted(bone);
                if (const std::string_view near = closest_match(bone, node_names); !near.empty()) {
                    message += "; did you mean " + quoted(near) + "?";
                }
                return param_error(code::kInvalidParams, at, message);
            }
            options.bone_map[profile_bone] = bone;
        }
        std::erase_if(options.bone_map, [](const auto& entry) { return entry.second.empty(); });
    }
    if (const json::Value* fix = params.find("fix_silhouette"); fix != nullptr) {
        options.fix_silhouette = fix->as_bool();
    }
    const scene3d::BoneMap bone_map = options.bone_map;
    auto report = scene3d::retarget_model(model, options);
    if (!report) {
        return param_error(code::kInvalidParams,
                           params.find("bone_map") != nullptr ? "/bone_map" : "/path",
                           report.error().message);
    }
    const scene3d::ModelReader read = [&session](std::string_view file) -> Result<std::string> {
        auto bytes = session.read_project_file(file);
        if (!bytes) {
            return bytes.error();
        }
        if (!bytes.value()) {
            return make_error(ErrorCode::NotFound, quoted(file) + " doesn't exist");
        }
        return std::move(*bytes.value());
    };
    auto glb = scene3d::write_glb(model, out, read);
    if (!glb) {
        return Reply::failure(glb.error());
    }

    json::Value result;
    result["out"] = out;
    json::Value bones = json::Object{};
    for (const auto& [profile_bone, bone] : bone_map) {
        bones[profile_bone] = bone;
    }
    result["bone_map"] = std::move(bones);
    const auto names = [](const std::vector<std::string>& list) {
        json::Array out_list;
        for (const std::string& name : list) {
            out_list.emplace_back(name);
        }
        return out_list;
    };
    if (!report.value().missing.empty()) {
        result["missing"] = names(report.value().missing);
    }
    if (!report.value().unmapped.empty()) {
        result["unmapped"] = names(report.value().unmapped);
    }
    if (!report.value().moved_aside.empty()) {
        json::Value moved = json::Object{};
        for (const auto& [now, was] : report.value().moved_aside) {
            moved[now] = was;
        }
        result["moved_aside"] = std::move(moved);
    }
    result["motion_scale"] =
        std::round(static_cast<double>(report.value().motion_scale) * 1e4) / 1e4;
    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    if (before.value() == glb.value()) {
        session.mark_unchanged();
        result["unchanged"] = true;
        return Reply::success(std::move(result));
    }
    if (auto staged = session.stage_file(out, glb.value()); !staged) {
        return Reply::failure(staged.error());
    }
    session.add_undo_file(out, std::move(before).value(), std::move(glb).value());
    return Reply::success(std::move(result));
}

} // namespace

void register_model_cleanup_commands(Registry& registry) {
    json::Value path = property("string", "The model file: .gltf, .glb, .obj, .fbx, .blend or "
                                          ".model.json, project path.");
    path["minLength"] = 1;
    json::Value out = property("string", "The .glb file to write. Default: path as a .glb.");
    out["minLength"] = 1;
    json::Value up = property("string", "The axis the model's up is along in its file, turned to "
                                        "y: \"z\" for most CAD and Blender exports without "
                                        "glTF's conversion. Default \"y\".");
    up["enum"] = json::Array{"y", "-y", "x", "-x", "z", "-z"};
    json::Value scale = property("number", "Its size times this.");
    scale["exclusiveMinimum"] = 0.0;
    json::Value height = property("number", "Or make it this many meters tall.");
    height["exclusiveMinimum"] = 0.0;
    json::Value pivot = property("array", "Where its origin goes in its box, 0 to 1 on each axis: "
                                          "[0.5, 0, 0.5] stands it on the ground, centered.");
    pivot["items"]["type"] = "number";
    pivot["items"]["minimum"] = 0.0;
    pivot["items"]["maximum"] = 1.0;
    pivot["minItems"] = 3;
    pivot["maxItems"] = 3;
    json::Value triangles = property("integer", "The most triangles it may have, 0 for no limit. "
                                                "Default: the storyboard budget covering out.");
    triangles["minimum"] = 0;
    json::Value uvs = property("boolean", "Unwrap surfaces without texture coordinates. Default "
                                          "true.");
    json::Value collision = property(
        "string", "Give it collision from its triangles, replacing any it has, as Godot's "
                  "\"Generate > Physics\": \"trimesh\", \"convex\" (one hull) or \"decompose\" "
                  "(convex hulls). Default: keep its own.");
    collision["enum"] = json::Array{"trimesh", "convex", "decompose"};
    json::Value delight = property(
        "number", "Take this much of the light baked into its albedo textures out of them, 0 to "
                  "1: 1 for scans and generated models lit in their textures.");
    delight["minimum"] = 0.0;
    delight["maximum"] = 1.0;
    json::Value max_hulls = property("integer", "The most hulls \"decompose\" makes. Default 16.");
    max_hulls["minimum"] = 1;
    max_hulls["maximum"] = 256;
    registry.add({
        .name = "model.cleanup",
        .description = "Makes a model game-ready as a .glb file: turns its up to y, scales it to "
                       "real size, puts its origin at a pivot, simplifies it under its triangle "
                       "budget, unwraps surfaces without texture coordinates, takes baked light "
                       "out of its textures and makes its collision. Returns triangles before and "
                       "after, size and pivot.",
        .capabilities = {Capability::Edit, Capability::Filesystem},
        .effect = Effect::Edit,
        .params = params_schema({{"path", std::move(path)},
                                 {"out", std::move(out)},
                                 {"up", std::move(up)},
                                 {"scale", std::move(scale)},
                                 {"height", std::move(height)},
                                 {"pivot", std::move(pivot)},
                                 {"triangles", std::move(triangles)},
                                 {"uvs", std::move(uvs)},
                                 {"delight", std::move(delight)},
                                 {"collision", std::move(collision)},
                                 {"max_hulls", std::move(max_hulls)}},
                                {"path"}),
        .handler = model_cleanup,
    });

    json::Value source = property("string", "The rigged model: .gltf, .glb, .fbx or .blend, "
                                            "project path.");
    source["minLength"] = 1;
    json::Value target = property("string", "The .glb file to write. Default: path as a .glb.");
    target["minLength"] = 1;
    json::Value bone_map = property(
        "object", "Changes to the guessed map: humanoid bone (\"LeftUpperArm\") to the model's "
                  "bone, \"\" to leave one unmapped.");
    bone_map["additionalProperties"]["type"] = "string";
    json::Value fix_silhouette = property(
        "boolean", "Turn bones that point over 15 degrees from the profile's toward it, as for "
                   "an A-pose. Default false.");
    registry.add({
        .name = "model.retarget",
        .description = "Retargets a rigged humanoid to Godot's humanoid skeleton as a .glb file: "
                       "renames its bones (Hips, LeftUpperArm, ...) and turns their rests as "
                       "Godot's importer does, so animations of every retargeted model play on "
                       "it. Guesses the bone map; returns it, with bones missing or left over.",
        .capabilities = {Capability::Edit, Capability::Filesystem},
        .effect = Effect::Edit,
        .params = params_schema({{"path", std::move(source)},
                                 {"out", std::move(target)},
                                 {"bone_map", std::move(bone_map)},
                                 {"fix_silhouette", std::move(fix_silhouette)}},
                                {"path"}),
        .handler = model_retarget,
    });
}

} // namespace slash::command
