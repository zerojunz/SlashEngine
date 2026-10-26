#include "slash/importer/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/unity_build.hpp"
#include "slash/importer/unreal.hpp"
#include "slash/runtime/gameplay.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace slash::importer {
namespace {

using command::Capability;
using command::Reply;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply failure(const Error& error, std::string_view pointer) {
    Reply reply = Reply::failure(error);
    reply.data["path"] = pointer;
    return reply;
}

Reply project_import(command::Session& session, const json::Value& params,
                     const ImportCommandOptions& command_options) {
    const std::string& source = params.find("source")->as_string();
    auto folder = session.resolve_path(source);
    if (!folder) {
        return failure(folder.error(), "/source");
    }
    const std::filesystem::path chosen(folder.value());
    std::error_code error;
    const bool unreal = chosen.extension() == ".uproject" ||
                        (!std::filesystem::exists(chosen / "project.godot", error) &&
                         !std::filesystem::exists(chosen / "ProjectSettings", error) &&
                         unreal::find_project(chosen).has_value());
    if (unreal) {
        if (!command_options.unpack_builds) {
            Reply reply = Reply::failure(command::code::kPermissionDenied,
                                         "a running game doesn't import Unreal projects, which "
                                         "runs Unreal's editor for minutes; use slash-cli import");
            reply.data["path"] = "/source";
            return reply;
        }
        if (!session.allowed(Capability::Build)) {
            Reply reply = Reply::failure(command::code::kPermissionDenied,
                                         "importing an Unreal project runs Unreal's editor, which "
                                         "needs the \"build\" capability, which this session "
                                         "does not grant");
            reply.data["path"] = "/source";
            reply.data["capability"] = "build";
            return reply;
        }
    } else if (unity::find_build(folder.value())) {
        if (!command_options.unpack_builds) {
            Reply reply = Reply::failure(command::code::kPermissionDenied,
                                         "a running game doesn't unpack Unity builds, which "
                                         "takes minutes; use slash-cli import");
            reply.data["path"] = "/source";
            return reply;
        }
        if (!session.allowed(Capability::Build)) {
            Reply reply = Reply::failure(command::code::kPermissionDenied,
                                         "unpacking a Unity build runs AssetRipper, which needs "
                                         "the \"build\" capability, which this session does not "
                                         "grant");
            reply.data["path"] = "/source";
            reply.data["capability"] = "build";
            return reply;
        }
    }
    ImportOptions options{
        .source = folder.value(), .destination = session.options().project_root, .unpack_to = {}};
    if (const json::Value* unpack_to = params.find("unpack_to"); unpack_to != nullptr) {
        // Checked here so the error names the parameter; the import takes it relative to the
        // project.
        if (auto unpack_folder = session.resolve_path(unpack_to->as_string()); !unpack_folder) {
            return failure(unpack_folder.error(), "/unpack_to");
        }
        options.unpack_to = unpack_to->as_string();
    }
    auto plan = plan_import(options);
    if (!plan) {
        return failure(plan.error(), "/source");
    }
    json::Value result = std::move(plan.value().report);
    const json::Value* dry_run = params.find("dry_run");
    if (dry_run != nullptr && dry_run->as_bool()) {
        json::Array paths;
        for (const auto& [path, contents] : plan.value().files) {
            paths.emplace_back(path);
        }
        result["dry_run"] = true;
        result["files"] = std::move(paths);
        return Reply::success(std::move(result));
    }
    for (const auto& [path, contents] : plan.value().files) {
        auto full = session.resolve_path(path);
        if (!full) {
            return failure(full.error(), "/source");
        }
        if (auto written = session.write_file(full.value(), contents); !written) {
            return Reply::failure(command::code::kPlatformFailure,
                                  "could not write " + path + ": " + written.error().message);
        }
    }
    result["files"] = plan.value().files.size();
    // Stand-ins for the skeletons' components, so the scenes using them open before the code is
    // built.
    if (gameplay::Host* host = session.options().gameplay; host != nullptr) {
        runtime::add_stand_ins(*host, session.options().project_root);
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_import_commands(command::Registry& registry, ImportCommandOptions options) {
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["required"] = json::Array{"source"};
    params["properties"]["source"] =
        property("string", "Folder holding a Godot 4 project (with project.godot), a Unity "
                           "project (with Assets/ and ProjectSettings/) or an Unreal Engine 5 "
                           "project (with a .uproject file), a game exported "
                           "with Godot 4 (its .pck file or executable), or a game built with "
                           "Unity (its folder, .apk or .ipa).");
    params["properties"]["unpack_to"] =
        property("string", "For an exported or built game: folder to write the Godot or Unity "
                           "project unpacked from it to, whose assets the import then uses.");
    params["properties"]["dry_run"] =
        property("boolean", "Return the report and the files it would write; write nothing.");

    registry.add({
        .name = "project.import",
        .description =
            "Converts a Godot 4, Unity or Unreal project's scenes, prefabs, resources, input and "
            "settings into this project, using its assets where they are; a game exported "
            "with Godot 4 or built with Unity is unpacked first. Unity builds (AssetRipper) "
            "and Unreal projects (Unreal's editor) need the build capability. Each script "
            "becomes a storyboard task to port it to C++. Reports what didn't convert.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler =
            [options](command::Session& session, const json::Value& args) {
                return project_import(session, args, options);
            },
    });
}

} // namespace slash::importer
