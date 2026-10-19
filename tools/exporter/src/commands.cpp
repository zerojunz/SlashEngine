#include "slash/exporter/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/gameplay.hpp"
#include "slash/exporter/sdk.hpp"
#include "slash/gameplay/host.hpp"

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

Reply gameplay_build(command::Session& session, const json::Value& /*params*/) {
    const GameplayBuild built = build_gameplay(session.options().project_root);
    json::Value result = built.to_json();
    if (!built.ok) {
        // Compiler errors are the caller's code to fix; a build that fails without any is not.
        const int code = built.step == "options" || !built.errors.empty()
                             ? command::code::kInvalidArgument
                             : command::code::kPlatformFailure;
        return Reply::failure(code, built.message, std::move(result));
    }
    // Swap the new code in now rather than before the next command.
    if (const std::optional<gameplay::Load> load = session.poll_gameplay(); load) {
        if (!load->error.empty()) {
            result["error"] = load->error;
            return Reply::failure(command::code::kInvalidArgument,
                                  "the game's code built but did not load: " + load->error,
                                  std::move(result));
        }
        result["reload"] = load->to_json();
    }
    return Reply::success(std::move(result));
}

Reply needs_build(std::string_view what, std::string_view pointer) {
    Reply reply = Reply::failure(command::code::kPermissionDenied,
                                 std::string(what) + " needs the \"build\" capability, which "
                                                     "this session does not grant");
    reply.data["path"] = pointer;
    reply.data["capability"] = "build";
    return reply;
}

Reply sdk_check(command::Session& session, const json::Value& params) {
    std::string platform;
    if (const json::Value* value = params.find("platform"); value != nullptr) {
        platform = value->as_string();
    }
    const json::Value* fetch = params.find("fetch");
    const bool fetching = fetch != nullptr && fetch->as_bool();
    if (fetching && !session.allowed(Capability::Build)) {
        return needs_build("downloading SDKs", "/fetch");
    }
    json::Value result = check_sdks(session.options().project_root, platform, fetching);
    result["ok"] = result.find("problems")->as_array().empty();
    return Reply::success(std::move(result));
}

Reply sdk_add(command::Session& session, const json::Value& params) {
    AddSdkOptions options;
    options.project = session.options().project_root;
    options.source = params.find("source")->as_string();
    const bool remote =
        options.source.find("://") != std::string::npos || options.source.starts_with("git@");
    if (remote) {
        if (!session.allowed(Capability::Build)) {
            return needs_build("adding an SDK from a URL", "/source");
        }
    } else if (auto folder = session.resolve_path(options.source); !folder) {
        Reply reply = Reply::failure(folder.error());
        reply.data["path"] = "/source";
        return reply;
    } else {
        options.source = folder.value();
    }
    if (const json::Value* name = params.find("name"); name != nullptr) {
        options.name = name->as_string();
    }
    if (const json::Value* ref = params.find("ref"); ref != nullptr) {
        options.ref = ref->as_string();
    }
    if (const json::Value* sha256 = params.find("sha256"); sha256 != nullptr) {
        options.sha256 = sha256->as_string();
    }
    const json::Value* dry_run = params.find("dry_run");
    options.dry_run = (dry_run != nullptr && dry_run->as_bool()) || !session.options().write_files;
    auto added = add_sdk(options);
    if (!added) {
        Reply reply = Reply::failure(added.error());
        reply.data["path"] = "/source";
        return reply;
    }
    return Reply::success(std::move(added).value());
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

    json::Value no_params;
    no_params["type"] = "object";
    no_params["additionalProperties"] = false;
    no_params["properties"] = json::Object{};
    registry.add({
        .name = "gameplay.build",
        .description = "Compiles the project's gameplay C++ (project.json gameplay.sources) and "
                       "swaps it into the world, keeping entities and the fields still in the "
                       "code. Returns compiler errors with file and line.",
        .capabilities = {Capability::Filesystem, Capability::Build},
        .effect = command::Effect::Read,
        .params = std::move(no_params),
        .handler = gameplay_build,
    });

    json::Value check_params;
    check_params["type"] = "object";
    check_params["additionalProperties"] = false;
    check_params["properties"]["platform"] = property(
        "string", "Check only this platform's builds: macos, windows, linux, web, android or ios.");
    check_params["properties"]["platform"]["enum"] =
        json::Array{"macos", "windows", "linux", "web", "android", "ios"};
    check_params["properties"]["fetch"] =
        property("boolean", "Download the packages' files not fetched yet, so they are checked "
                            "too (needs the build capability).");
    registry.add({
        .name = "sdk.check",
        .description = "Checks the project's SDK packages (project.json sdks): each sdk.json, "
                       "the presets' sdks, keys in the environment and that every file a "
                       "platform links exists. Returns the packages and every problem.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = std::move(check_params),
        .handler = sdk_check,
    });

    json::Value add_params;
    add_params["type"] = "object";
    add_params["additionalProperties"] = false;
    add_params["required"] = json::Array{"source"};
    add_params["properties"]["source"] =
        property("string", "A package folder in the project (added where it is), a git "
                           "repository URL, or the https:// URL of a .zip or .tar archive "
                           "(copied into sdks/; URLs need the build capability).");
    add_params["properties"]["name"] =
        property("string", "Folder under sdks/ for a copied package; default its name.");
    add_params["properties"]["ref"] = property("string", "Git branch or tag to clone.");
    add_params["properties"]["sha256"] =
        property("string", "The archive's expected SHA-256; a mismatch adds nothing.");
    add_params["properties"]["dry_run"] =
        property("boolean", "Check the package and return what would be added; change nothing.");
    registry.add({
        .name = "sdk.add",
        .description = "Adds a closed vendor SDK package (a folder with sdk.json) to "
                       "project.json's sdks, so exports link it. Returns its name, platforms "
                       "and the keys to set in the environment.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = std::move(add_params),
        .handler = sdk_add,
    });
}

} // namespace slash::exporter
