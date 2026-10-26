#include "slash/command/model_cleanup.hpp"

#include "slash/command/session.hpp"
#include "slash/scene3d/cleanup.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
        } else if (message.find("height") != std::string::npos) {
            at = "/height";
        }
        std::string text = message;
        if (budget && at == "/out") {
            text += ", which " + budget->id + " allows; pass triangles to aim elsewhere";
        }
        return param_error(code::kInvalidParams, at, text);
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
    registry.add({
        .name = "model.cleanup",
        .description = "Makes a model game-ready as a .glb file: turns its up to y, scales it to "
                       "real size, puts its origin at a pivot, simplifies it under its triangle "
                       "budget and unwraps surfaces without texture coordinates. Returns "
                       "triangles before and after, size and pivot.",
        .capabilities = {Capability::Edit, Capability::Filesystem},
        .effect = Effect::Edit,
        .params = params_schema({{"path", std::move(path)},
                                 {"out", std::move(out)},
                                 {"up", std::move(up)},
                                 {"scale", std::move(scale)},
                                 {"height", std::move(height)},
                                 {"pivot", std::move(pivot)},
                                 {"triangles", std::move(triangles)},
                                 {"uvs", std::move(uvs)}},
                                {"path"}),
        .handler = model_cleanup,
    });
}

} // namespace slash::command
