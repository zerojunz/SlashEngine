// slash-cli: the command-line entry point agents use to drive SlashEngine.
// Results go to stdout as JSON; logs go to stderr as JSON lines.

#include "init.hpp"

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/rpc.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr std::string_view kUsage = R"(slash-cli: the SlashEngine command-line tool

Usage:
  slash-cli version                  Print the engine version
  slash-cli run [options]            Start the engine and tick it
      --headless                     Run with no window or GPU
      --ticks <n>                    Stop after n fixed ticks (required with --headless)
      --hz <rate>                    Fixed tick rate in Hz (default 60)
      --scene <file>                 Load a .scene.json file before the first tick
      --save <file>                  Save the scene to this file after the last tick
  slash-cli scene validate <file>    Check a scene against the schema; lists every error
  slash-cli scene format <file>      Rewrite a scene in canonical form
      --check                        Only report whether it is canonical; change nothing
  slash-cli schema scene             Print the JSON Schema for scene files
  slash-cli call <command> [params]  Run one engine command and print its result
                                     params is a JSON object, or - to read it from stdin
      --scene <file>                 Open this scene first (or start a new one saved there)
      --save                         Save the scene after the command succeeds
  slash-cli rpc                      Serve engine commands as JSON-RPC 2.0, one message per
                                     line on stdin and stdout
      --scene <file>                 Open this scene first
  slash-cli init [dir]               Set a project up for Claude Code, Codex and Cursor
      --force                        Replace files that already exist
      --mcp-command <cmd>            How agents launch slash-mcp (default "slash-mcp")
  slash-cli help                     Show this message

call and rpc also take:
      --project <dir>                Folder that scene paths are relative to (default ".")
      --allow <list>                 Capabilities: read, edit, run, filesystem or all (default)

slash-cli call engine.describe lists every command. Agents with MCP should use slash-mcp.

Results are JSON on stdout. Exit codes: 0 success, 1 failure, 2 usage error.
)";

void print(const json::Value& result) {
    const std::string text = json::write(result, json::Style::Compact) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
}

int usage_error(std::string_view message) {
    slash::log::error("usage error", {{"detail", message}});
    std::fwrite(kUsage.data(), 1, kUsage.size(), stderr);
    return kExitUsage;
}

int failure(std::string_view message, const slash::Error& error) {
    slash::log::error(message, {{"code", slash::to_string(error.code)}, {"detail", error.message}});
    return kExitFailure;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end;
}

bool parse_double(const std::string& text, double& out) {
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return !text.empty() && end == text.c_str() + text.size();
}

world::ComponentRegistry make_registry() {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    return registry;
}

json::Value errors_to_json(const std::vector<json::ValidationError>& errors) {
    json::Array items;
    for (const json::ValidationError& error : errors) {
        json::Value item;
        item["path"] = error.path;
        item["message"] = error.message;
        items.push_back(std::move(item));
    }
    return items;
}

int cmd_version() {
    json::Value result;
    result["name"] = "SlashEngine";
    result["version"] = slash::version();
    print(result);
    return kExitOk;
}

int cmd_run(const std::vector<std::string>& args) {
    slash::platform::Config config;
    config.title = "SlashEngine";
    slash::platform::RunOptions options;
    std::string scene_path;
    std::string save_path;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--headless") {
            config.headless = true;
        } else if (arg == "--ticks") {
            if (!has_value || !parse_u64(args[++i], options.max_ticks)) {
                return usage_error("--ticks needs a whole number");
            }
        } else if (arg == "--hz") {
            if (!has_value || !parse_double(args[++i], options.tick_hz)) {
                return usage_error("--hz needs a number");
            }
        } else if (arg == "--scene" || arg == "--save") {
            if (!has_value) {
                return usage_error(arg + " needs a file path");
            }
            (arg == "--scene" ? scene_path : save_path) = args[++i];
        } else {
            return usage_error("unknown option for run: " + arg);
        }
    }

    const world::ComponentRegistry registry = make_registry();
    world::World game(registry);
    if (!scene_path.empty()) {
        auto text = slash::platform::read_file(scene_path);
        if (!text) {
            return failure("could not load the scene", text.error());
        }
        auto parsed = json::parse(text.value());
        if (!parsed) {
            return failure("could not parse the scene", parsed.error());
        }
        if (auto loaded = world::load_scene(game, parsed.value()); !loaded) {
            return failure("could not load the scene", loaded.error());
        }
    }

    auto platform = slash::platform::Platform::create(config);
    if (!platform) {
        return failure("could not start the platform", platform.error());
    }

    auto stats = slash::platform::run(
        platform.value(), options,
        [&game](std::uint64_t /*tick*/, double tick_seconds) { game.step(tick_seconds); });
    if (!stats) {
        return failure("run failed", stats.error());
    }

    if (!save_path.empty()) {
        const std::string text = json::write(world::save_scene(game)) + "\n";
        if (auto written = slash::platform::write_file(save_path, text); !written) {
            return failure("could not save the scene", written.error());
        }
    }

    json::Value result;
    result["ticks"] = stats.value().ticks;
    result["quit_requested"] = stats.value().quit_requested;
    result["headless"] = config.headless;
    result["entities"] = game.size();
    print(result);
    return kExitOk;
}

int cmd_scene_validate(const std::string& path) {
    auto text = slash::platform::read_file(path);
    if (!text) {
        return failure("could not read the scene", text.error());
    }
    const world::ComponentRegistry registry = make_registry();
    json::Value result;
    auto parsed = json::parse(text.value());
    std::vector<json::ValidationError> errors;
    if (!parsed) {
        errors.push_back({"", parsed.error().message});
    } else {
        errors = world::validate_scene(registry, parsed.value());
    }
    result["valid"] = errors.empty();
    if (errors.empty()) {
        result["entities"] = parsed.value().find("entities")->size();
    } else {
        result["errors"] = errors_to_json(errors);
    }
    print(result);
    return errors.empty() ? kExitOk : kExitFailure;
}

int cmd_scene_format(const std::string& path, bool check) {
    auto text = slash::platform::read_file(path);
    if (!text) {
        return failure("could not read the scene", text.error());
    }
    const world::ComponentRegistry registry = make_registry();
    auto formatted = world::format_scene(registry, text.value());
    if (!formatted) {
        return failure("could not format the scene", formatted.error());
    }
    const bool canonical = formatted.value() == text.value();
    json::Value result;
    if (check) {
        result["canonical"] = canonical;
        print(result);
        return canonical ? kExitOk : kExitFailure;
    }
    if (!canonical) {
        if (auto written = slash::platform::write_file(path, formatted.value()); !written) {
            return failure("could not write the scene", written.error());
        }
    }
    result["changed"] = !canonical;
    print(result);
    return kExitOk;
}

int cmd_scene(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage_error("scene needs a subcommand: validate or format");
    }
    const std::string& action = args[0];
    std::string path;
    bool check = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--check" && action == "format") {
            check = true;
        } else if (path.empty() && !args[i].starts_with("--")) {
            path = args[i];
        } else {
            return usage_error("unexpected argument for scene " + action + ": " + args[i]);
        }
    }
    if (action != "validate" && action != "format") {
        return usage_error("unknown scene subcommand: " + action);
    }
    if (path.empty()) {
        return usage_error("scene " + action + " needs a file path");
    }
    return action == "validate" ? cmd_scene_validate(path) : cmd_scene_format(path, check);
}

int cmd_schema(const std::vector<std::string>& args) {
    if (args.size() != 1 || args[0] != "scene") {
        return usage_error("schema takes one argument: scene");
    }
    const std::string text = json::write(world::scene_schema(make_registry())) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    return kExitOk;
}

// Options shared by call and rpc.
struct SessionArgs {
    command::SessionOptions options;
    std::string scene;
    bool save = false;
};

// Parses a session option at args[i]. Returns false if args[i] is not one; sets error if it is
// one but malformed.
bool parse_session_arg(const std::vector<std::string>& args, std::size_t& i, SessionArgs& out,
                       std::string& error) {
    const std::string& arg = args[i];
    const bool has_value = i + 1 < args.size();
    if (arg == "--project" || arg == "--scene" || arg == "--allow") {
        if (!has_value) {
            error = arg + " needs a value";
            return true;
        }
        const std::string& value = args[++i];
        if (arg == "--project") {
            out.options.project_root = value;
        } else if (arg == "--scene") {
            out.scene = value;
        } else if (auto capabilities = command::parse_capabilities(value); capabilities) {
            out.options.capabilities = std::move(capabilities).value();
        } else {
            error = capabilities.error().message;
        }
        return true;
    }
    return false;
}

json::Value error_json(const command::Reply& reply) {
    json::Value error;
    error["code"] = reply.code;
    error["message"] = reply.message;
    if (!reply.data.is_null()) {
        error["data"] = reply.data;
    }
    json::Value out;
    out["error"] = std::move(error);
    return out;
}

// The engine's components and commands, and a session over them.
struct Engine {
    world::ComponentRegistry components = make_registry();
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(const command::SessionOptions& options) {
        command::register_builtin_commands(commands);
        command::SessionOptions session_options = options;
        session_options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, session_options);
    }
};

int cmd_call(const std::vector<std::string>& args) {
    SessionArgs session_args;
    std::string name;
    std::string params_text;
    bool have_params = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string error;
        if (parse_session_arg(args, i, session_args, error)) {
            if (!error.empty()) {
                return usage_error(error);
            }
        } else if (args[i] == "--save") {
            session_args.save = true;
        } else if (name.empty() && !args[i].starts_with("--")) {
            name = args[i];
        } else if (!have_params && (args[i] == "-" || !args[i].starts_with("--"))) {
            params_text = args[i];
            have_params = true;
        } else {
            return usage_error("unexpected argument for call: " + args[i]);
        }
    }
    if (name.empty()) {
        return usage_error("call needs a command name, e.g. slash-cli call engine.describe");
    }
    if (params_text == "-") {
        params_text.assign(std::istreambuf_iterator<char>(std::cin), {});
    }
    json::Value params = json::Object{};
    if (have_params) {
        auto parsed = json::parse(params_text);
        if (!parsed) {
            return usage_error("params are not valid JSON: " + parsed.error().message);
        }
        params = std::move(parsed).value();
    }

    Engine engine(session_args.options);
    command::Session& session = *engine.session;
    if (!session_args.scene.empty()) {
        if (const auto opened = command::open_or_new_scene(session, session_args.scene);
            !opened.ok) {
            print(error_json(opened));
            return kExitFailure;
        }
    }
    const command::Reply reply = session.call(name, params);
    if (!reply.ok) {
        print(error_json(reply));
        return kExitFailure;
    }
    if (session_args.save) {
        if (const auto saved = session.call("scene.save", json::Object{}); !saved.ok) {
            print(error_json(saved));
            return kExitFailure;
        }
    }
    print(reply.value);
    return kExitOk;
}

int cmd_rpc(const std::vector<std::string>& args) {
    SessionArgs session_args;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string error;
        if (!parse_session_arg(args, i, session_args, error)) {
            return usage_error("unexpected argument for rpc: " + args[i]);
        }
        if (!error.empty()) {
            return usage_error(error);
        }
    }
    Engine engine(session_args.options);
    command::Session& session = *engine.session;
    if (!session_args.scene.empty()) {
        if (const auto opened = command::open_or_new_scene(session, session_args.scene);
            !opened.ok) {
            print(error_json(opened));
            return kExitFailure;
        }
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        if (const std::string response = command::rpc::handle(session, line); !response.empty()) {
            const std::string out = response + "\n";
            std::fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
        }
    }
    return kExitOk;
}

int cmd_init(const std::vector<std::string>& args) {
    slash::cli::InitOptions options;
    bool have_directory = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--force") {
            options.force = true;
        } else if (args[i] == "--mcp-command") {
            if (i + 1 >= args.size()) {
                return usage_error("--mcp-command needs a value");
            }
            options.mcp_command = args[++i];
        } else if (!have_directory && !args[i].starts_with("--")) {
            options.directory = args[i];
            have_directory = true;
        } else {
            return usage_error("unexpected argument for init: " + args[i]);
        }
    }
    auto result = slash::cli::init_project(options);
    if (!result) {
        return failure("could not set up the project", result.error());
    }
    print(result.value());
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::fwrite(kUsage.data(), 1, kUsage.size(), stdout);
        return args.empty() ? kExitUsage : kExitOk;
    }

    const std::string name = args[0];
    args.erase(args.begin());
    if (name == "version" || name == "--version") {
        return cmd_version();
    }
    if (name == "run") {
        return cmd_run(args);
    }
    if (name == "scene") {
        return cmd_scene(args);
    }
    if (name == "schema") {
        return cmd_schema(args);
    }
    if (name == "call") {
        return cmd_call(args);
    }
    if (name == "rpc") {
        return cmd_rpc(args);
    }
    if (name == "init") {
        return cmd_init(args);
    }
    return usage_error("unknown command: " + name);
}
