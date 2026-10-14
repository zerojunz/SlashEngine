#include "slash/exporter/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/exporter/export.hpp"

#include <string>
#include <utility>

namespace slash::exporter {
namespace {

using command::Capability;
using command::Reply;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply export_run(command::Session& session, const json::Value& params) {
    ExportOptions options;
    options.project = session.options().project_root;
    options.target = params.find("preset")->as_string();
    if (const json::Value* debug = params.find("debug"); debug != nullptr) {
        options.debug = debug->as_bool();
    }
    if (const json::Value* test = params.find("test"); test != nullptr) {
        options.test = test->as_bool();
    }
    ExportResult result = export_game(options);
    if (result.ok) {
        return Reply::success(std::move(result.json));
    }
    if (const json::Value* error = result.json.find("error"); error != nullptr) {
        // Bad requests and invalid projects are the caller's to fix; anything later is a
        // failed build.
        const std::string& step = error->find("step")->as_string();
        const int code = step == "options" || step == "validate" ? command::code::kInvalidArgument
                                                                 : command::code::kPlatformFailure;
        json::Value data = *error;
        std::string message = data.find("message")->as_string();
        data.as_object().erase("message");
        return Reply::failure(code, std::move(message), std::move(data));
    }
    // The export worked but the game failed its headless test.
    return Reply::failure(command::code::kPlatformFailure,
                          "the exported game failed its headless test", std::move(result.json));
}

} // namespace

void register_export_commands(command::Registry& registry) {
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["required"] = json::Array{"preset"};
    params["properties"]["preset"] =
        property("string", "Preset from export_presets.json, or macos, windows, linux, web, "
                           "android or ios for that platform's defaults.");
    params["properties"]["debug"] = property("boolean", "Debug build instead of Release.");
    params["properties"]["test"] =
        property("boolean", "Run the exported game headless afterwards, where this machine can.");

    registry.add({
        .name = "export.run",
        .description = "Builds the project for a platform into its output folder, signs it as the "
                       "preset says, and returns the output path. Takes minutes the first time.",
        .capabilities = {Capability::Filesystem, Capability::Build},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler = export_run,
    });
}

} // namespace slash::exporter
