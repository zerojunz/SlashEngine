#include "slash/importer/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/importer/import.hpp"

#include <string>
#include <string_view>
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

Reply project_import(command::Session& session, const json::Value& params) {
    const std::string& source = params.find("source")->as_string();
    auto folder = session.resolve_path(source);
    if (!folder) {
        return failure(folder.error(), "/source");
    }
    const ImportOptions options{.source = folder.value(),
                                .destination = session.options().project_root};
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
    return Reply::success(std::move(result));
}

} // namespace

void register_import_commands(command::Registry& registry) {
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["required"] = json::Array{"source"};
    params["properties"]["source"] =
        property("string", "Folder holding a Godot 4 project (with project.godot) or a Unity "
                           "project (with Assets/ and ProjectSettings/).");
    params["properties"]["dry_run"] =
        property("boolean", "Return the report and the files it would write; write nothing.");

    registry.add({
        .name = "project.import",
        .description =
            "Converts a Godot 4 or Unity project's scenes, prefabs, resources, input and "
            "settings into this project, using its assets where they are. Each script "
            "becomes a storyboard task to port it to C++. Reports what didn't convert.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler = project_import,
    });
}

} // namespace slash::importer
