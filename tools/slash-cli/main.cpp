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
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/runtime/project.hpp"
#include "slash/server/server.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
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
// How often a running game checks for edited asset files: every quarter second at 60 Hz.
constexpr std::uint64_t kAssetPollTicks = 15;

constexpr std::string_view kUsage = R"(slash-cli: the SlashEngine command-line tool

Usage:
  slash-cli version                  Print the engine version
  slash-cli run [options]            Start the engine and tick it
      --headless                     Run with no window or GPU
      --ticks <n>                    Stop after n fixed ticks (required with --headless)
      --hz <rate>                    Fixed tick rate in Hz (default 60)
      --scene <file>                 Load a .scene.json file before the first tick
      --save <file>                  Save the scene to this file after the last tick
      --serve                        Serve the running game over HTTP (see serve)
      --port <n>                     Port for --serve (default: any free port)
      --token <token>                Bearer token for --serve (default: random)
      --project <dir>                Project folder for --serve (default ".")
  slash-cli scene validate <file>    Check a scene or .prefab.json file; lists every error
  slash-cli scene format <file>      Rewrite a scene or prefab in canonical form
      --check                        Only report whether it is canonical; change nothing
  slash-cli schema <name>            Print a JSON Schema: scene, prefab, storyboard, project
                                     (project.json) or presets (export_presets.json)
  slash-cli project validate [dir]   Check project.json and export_presets.json; lists every
                                     error
  slash-cli call <command> [params]  Run one engine command and print its result
                                     params is a JSON object, or - to read it from stdin
      --scene <file>                 Open this scene first (or start a new one saved there)
      --save                         Save the scene after the command succeeds
  slash-cli rpc                      Serve engine commands as JSON-RPC 2.0, one message per
                                     line on stdin and stdout
      --scene <file>                 Open this scene first
  slash-cli serve                    Serve engine commands over HTTP on this machine until
                                     stopped: REST at /v1/commands, OpenAPI at
                                     /v1/openapi.json, JSON-RPC at /rpc and MCP at /mcp
      --scene <file>                 Open this scene first
      --port <n>                     Port (default: any free port)
      --token <token>                Bearer token clients must send (default: random)
  slash-cli init [dir]               Set a project up for Claude Code, Codex and Cursor
      --force                        Replace files that already exist
      --mcp-command <cmd>            How agents launch slash-mcp (default "slash-mcp")
  slash-cli export <preset>          Build and sign the game with a preset from
                                     export_presets.json, or with a platform's defaults:
                                     macos, windows, linux, web, android or ios
      --project <dir>                Game folder with project.json (default ".")
      --out <dir>                    Output folder (default: the preset's, else
                                     <project>/build/export/<preset>)
      --debug                        Debug build instead of Release
      --test                         Run the exported game headless afterwards, if possible
      --engine <dir>                 Engine sources (default: SLASH_ENGINE_DIR or built in)
  slash-cli toolchain list           List the cross toolchains this machine can install
  slash-cli toolchain install [name] Download the toolchains exports use to build other
                                     platforms (default: all); also takes a platform name
                                     (windows, linux) or linux-arm64; folder:
                                     SLASH_TOOLCHAINS_DIR, else ~/.slash/toolchains
  slash-cli help                     Show this message

call, rpc and serve also take:
      --project <dir>                Folder that scene paths are relative to (default ".")
      --allow <list>                 Capabilities: read, edit, run, filesystem, build or all
                                     (default all)

slash-cli call engine.describe lists every command. Agents with MCP should use slash-mcp.
A server writes its URL and token to .slash/server.json in the project for other tools.

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

// The engine's components and commands, and a session over them. A running game leaves out
// export.run, which would stall its loop for minutes, and adds play and pause.
struct Engine {
    world::ComponentRegistry components = make_registry();
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(const command::SessionOptions& options, bool running = false) {
        command::register_builtin_commands(commands);
        if (running) {
            command::register_run_commands(commands);
        } else {
            slash::exporter::register_export_commands(commands);
        }
        command::SessionOptions session_options = options;
        session_options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, session_options);
    }
};

// Set by SIGINT and SIGTERM to stop slash-cli serve.
volatile std::sig_atomic_t g_stop_requested = 0;

void request_stop(int /*signal*/) {
    g_stop_requested = 1;
}

// Keeps .slash/server.json in the project folder while a server runs, so slash-mcp --live and
// other tools can find it. Only the owner may read it, because it holds the token.
class EndpointFile {
public:
    EndpointFile(const std::string& project, const slash::server::Endpoint& endpoint)
        : path_((std::filesystem::path(project) /
                 std::filesystem::path(std::string(slash::server::kEndpointFile)))
                    .lexically_normal()) {
        namespace fs = std::filesystem;
        std::error_code error;
        fs::create_directories(path_.parent_path(), error);
        std::ofstream(path_, std::ios::trunc).close();
        fs::permissions(path_, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, error);
        std::ofstream file(path_, std::ios::trunc);
        file << json::write(endpoint.to_json(), json::Style::Compact) << '\n';
        if (!file) {
            slash::log::warn("could not write the server file", {{"path", path()}});
        }
    }
    ~EndpointFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    EndpointFile(const EndpointFile&) = delete;
    EndpointFile& operator=(const EndpointFile&) = delete;
    EndpointFile(EndpointFile&&) = delete;
    EndpointFile& operator=(EndpointFile&&) = delete;

    [[nodiscard]] std::string path() const { return path_.generic_string(); }

private:
    std::filesystem::path path_;
};

// Parses a server option at args[i]. Returns false if args[i] is not one; sets error if it is
// one but malformed.
bool parse_server_arg(const std::vector<std::string>& args, std::size_t& i,
                      slash::server::ServerOptions& out, std::string& error) {
    const std::string& arg = args[i];
    if (arg != "--port" && arg != "--token") {
        return false;
    }
    if (i + 1 >= args.size()) {
        error = arg + " needs a value";
        return true;
    }
    const std::string& value = args[++i];
    if (arg == "--token") {
        out.token = value;
        return true;
    }
    std::uint64_t port = 0;
    constexpr std::uint64_t kMaxPort = 65535;
    if (!parse_u64(value, port) || port > kMaxPort) {
        error = "--port needs a number from 0 to 65535";
        return true;
    }
    out.port = static_cast<int>(port);
    return true;
}

int cmd_run(const std::vector<std::string>& args) {
    slash::platform::Config config;
    config.title = "SlashEngine";
    slash::platform::RunOptions options;
    std::string scene_path;
    std::string save_path;
    bool serve = false;
    bool server_args = false;
    command::SessionOptions session_options;
    slash::server::ServerOptions server_options;

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
        } else if (arg == "--serve") {
            serve = true;
        } else if (arg == "--project") {
            if (!has_value) {
                return usage_error("--project needs a folder");
            }
            session_options.project_root = args[++i];
        } else if (std::string error; parse_server_arg(args, i, server_options, error)) {
            if (!error.empty()) {
                return usage_error(error);
            }
            server_args = true;
        } else {
            return usage_error("unknown option for run: " + arg);
        }
    }
    if (server_args && !serve) {
        return usage_error("--port and --token need --serve");
    }

    Engine engine(session_options, /*running=*/true);
    world::World& game = engine.session->world();
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
        // Inside the project, scene.save with no path writes back to the loaded file.
        std::error_code error;
        const std::string relative =
            std::filesystem::relative(scene_path, session_options.project_root, error)
                .generic_string();
        if (!error && !relative.empty() && engine.session->resolve_path(relative)) {
            engine.session->set_scene_path(relative);
        }
    }

    auto platform = slash::platform::Platform::create(config);
    if (!platform) {
        return failure("could not start the platform", platform.error());
    }
    // Made after the platform, so it lets go of the window before the window closes.
    const std::unique_ptr<slash::render::RenderDevice> renderer =
        config.headless
            ? slash::render::create_null_device()
            : slash::render::create_device_or_null(
                  {.window = platform.value().window(), .driver = "auto", .debug = false});
    bool draw_failed = false;

    // With --serve, requests run on this thread between ticks, so they never race the game, and
    // pause stops the world while the loop keeps serving.
    engine.session->set_playing(true);
    std::unique_ptr<slash::server::Server> server;
    std::optional<EndpointFile> endpoint_file;
    if (serve) {
        auto started = slash::server::Server::start(*engine.session, server_options);
        if (!started) {
            return failure("could not start the server", started.error());
        }
        server = std::move(started).value();
        endpoint_file.emplace(session_options.project_root, server->endpoint());
    }

    auto stats = slash::platform::run(
        platform.value(), options,
        [&](std::uint64_t tick, double tick_seconds) {
            // Hot reload: pick up edited assets and prefabs.
            if (tick % kAssetPollTicks == 0) {
                engine.session->poll_assets();
            }
            if (server) {
                server->poll();
            }
            if (engine.session->playing()) {
                game.step(tick_seconds);
            }
        },
        [&] {
            auto drawn =
                slash::render::draw_empty_frame(*renderer, slash::render::kDefaultClearColor);
            if (!drawn && !draw_failed) {
                draw_failed = true;
                slash::log::warn("drawing a frame failed", {{"error", drawn.error().message}});
            }
        });
    if (server) {
        server->stop();
    }
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
    result["renderer"] = slash::render::backend_name(renderer->backend());
    result["entities"] = game.size();
    if (server) {
        result["url"] = server->endpoint().url();
    }
    print(result);
    return kExitOk;
}

// The project a file belongs to: the nearest folder above it with a project.json, else ".".
std::string project_folder(const std::string& file) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path folder = fs::absolute(file, error).parent_path();
    while (!error && !folder.empty()) {
        if (fs::exists(folder / slash::runtime::kProjectFile, error)) {
            return folder.string();
        }
        if (folder == folder.parent_path()) {
            break;
        }
        folder = folder.parent_path();
    }
    return ".";
}

// Reads the prefabs a scene or prefab extends from its project.
world::FileReader prefab_reader(const std::string& file) {
    return slash::runtime::folder_reader(project_folder(file));
}

// Errors for StoryRef IDs in a scene or prefab that name no entry in its project's storyboard.
void check_story_refs(const std::string& file, const json::Value& document,
                      std::vector<json::ValidationError>& errors) {
    const std::string folder = project_folder(file);
    auto loaded = slash::storyboard::load(
        [&](std::string_view path) -> slash::Result<std::optional<std::string>> {
            const std::string full = slash::runtime::join_path(folder, path);
            if (!slash::platform::exists(full)) {
                return std::optional<std::string>();
            }
            auto text = slash::platform::read_file(full);
            if (!text) {
                return text.error();
            }
            return std::optional<std::string>(std::move(text).value());
        });
    if (!loaded) {
        return;
    }
    std::vector<slash::storyboard::FileError> found;
    slash::storyboard::check_story_refs(loaded.value().entries, file, document, found);
    for (slash::storyboard::FileError& error : found) {
        errors.push_back({std::move(error.path), std::move(error.message)});
    }
}

int cmd_scene_validate(const std::string& path) {
    auto text = slash::platform::read_file(path);
    if (!text) {
        return failure("could not read the file", text.error());
    }
    const world::ComponentRegistry registry = make_registry();
    const bool prefab = path.ends_with(world::kPrefabSuffix);
    json::Value result;
    auto parsed = json::parse(text.value());
    std::vector<json::ValidationError> errors;
    if (!parsed) {
        errors.push_back({"", parsed.error().message});
    } else if (prefab) {
        errors = world::validate_prefab(registry, parsed.value(), prefab_reader(path));
    } else {
        errors = world::validate_scene(registry, parsed.value(), prefab_reader(path));
    }
    if (parsed) {
        check_story_refs(path, parsed.value(), errors);
    }
    result["valid"] = errors.empty();
    if (errors.empty()) {
        const json::Value* items = parsed.value().find(prefab ? "components" : "entities");
        result[prefab ? "components" : "entities"] = items != nullptr ? items->size() : 0;
    } else {
        result["errors"] = errors_to_json(errors);
    }
    print(result);
    return errors.empty() ? kExitOk : kExitFailure;
}

int cmd_scene_format(const std::string& path, bool check) {
    auto text = slash::platform::read_file(path);
    if (!text) {
        return failure("could not read the file", text.error());
    }
    const world::ComponentRegistry registry = make_registry();
    auto formatted = path.ends_with(world::kPrefabSuffix)
                         ? world::format_prefab(registry, text.value(), prefab_reader(path))
                         : world::format_scene(registry, text.value(), prefab_reader(path));
    if (!formatted) {
        return failure("could not format the file", formatted.error());
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
            return failure("could not write the file", written.error());
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
    constexpr std::array<std::string_view, 5> kKinds = {"scene", "prefab", "storyboard", "project",
                                                        "presets"};
    if (args.size() != 1 || std::ranges::find(kKinds, args[0]) == kKinds.end()) {
        return usage_error(
            "schema takes one argument: scene, prefab, storyboard, project or presets");
    }
    json::Value schema;
    if (args[0] == "scene") {
        schema = world::scene_schema(make_registry());
    } else if (args[0] == "prefab") {
        schema = world::prefab_schema(make_registry());
    } else if (args[0] == "storyboard") {
        schema = slash::storyboard::file_schema();
    } else if (args[0] == "project") {
        schema = slash::runtime::project_schema();
    } else {
        schema = slash::exporter::presets_schema();
    }
    const std::string text = json::write(schema) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    return kExitOk;
}

// Appends every error in one of a project's files, each with the file's name.
template <typename Validate>
void validate_file(const std::string& folder, std::string_view file, Validate validate,
                   json::Array& errors) {
    const std::string path = slash::runtime::join_path(folder, file);
    auto text = slash::platform::read_file(path);
    if (!text) {
        return;
    }
    auto parsed = json::parse(text.value());
    std::vector<json::ValidationError> problems;
    if (!parsed) {
        problems.push_back({"", parsed.error().message});
    } else {
        problems = validate(parsed.value());
    }
    for (const json::ValidationError& problem : problems) {
        json::Value item;
        item["file"] = std::string(file);
        item["path"] = problem.path;
        item["message"] = problem.message;
        errors.push_back(std::move(item));
    }
}

int cmd_project(const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "validate" || args.size() > 2) {
        return usage_error("project takes one subcommand: validate [dir]");
    }
    const std::string folder = args.size() == 2 ? args[1] : ".";
    const std::string path = slash::runtime::join_path(folder, slash::runtime::kProjectFile);
    if (!slash::platform::exists(path)) {
        return failure(
            "could not read the project",
            slash::make_error(slash::ErrorCode::NotFound, "no project.json in " + folder));
    }
    json::Array errors;
    validate_file(folder, slash::runtime::kProjectFile, slash::runtime::validate_project, errors);
    validate_file(folder, slash::exporter::kPresetsFile, slash::exporter::validate_presets, errors);
    // The storyboard, and the StoryRef IDs in every scene and prefab.
    command::SessionOptions options;
    options.project_root = folder;
    Engine engine(options);
    command::Reply checked = engine.session->call("storyboard.check", json::Object{});
    json::Value result;
    if (!checked.ok) {
        json::Value item;
        item["file"] = "storyboard";
        item["path"] = "";
        item["message"] = checked.message;
        errors.push_back(std::move(item));
    } else {
        if (json::Value* found = checked.value.find("errors"); found != nullptr) {
            for (json::Value& error : found->as_array()) {
                errors.push_back(std::move(error));
            }
        }
        if (json::Value* warnings = checked.value.find("warnings"); warnings != nullptr) {
            result["warnings"] = std::move(*warnings);
        }
    }
    result["valid"] = errors.empty();
    if (!errors.empty()) {
        result["errors"] = std::move(errors);
    }
    print(result);
    return result.find("errors") == nullptr ? kExitOk : kExitFailure;
}

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

int cmd_serve(const std::vector<std::string>& args) {
    SessionArgs session_args;
    slash::server::ServerOptions server_options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string error;
        if (!parse_session_arg(args, i, session_args, error) &&
            !parse_server_arg(args, i, server_options, error)) {
            return usage_error("unexpected argument for serve: " + args[i]);
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
    auto server = slash::server::Server::start(session, server_options);
    if (!server) {
        return failure("could not start the server", server.error());
    }
    const EndpointFile endpoint_file(session_args.options.project_root, server.value()->endpoint());
    json::Value started = server.value()->endpoint().to_json();
    started["endpoint_file"] = endpoint_file.path();
    print(started);
    std::fflush(stdout);

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    while (g_stop_requested == 0) {
        server.value()->poll(std::chrono::milliseconds(100));
        session.poll_assets();
    }
    server.value()->stop();
    slash::log::info("server stopped");
    return kExitOk;
}

int cmd_export(const std::vector<std::string>& args) {
    slash::exporter::ExportOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--debug") {
            options.debug = true;
        } else if (arg == "--test") {
            options.test = true;
        } else if (arg == "--project" && has_value) {
            options.project = args[++i];
        } else if (arg == "--out" && has_value) {
            options.output = args[++i];
        } else if (arg == "--engine" && has_value) {
            options.engine = args[++i];
        } else if (options.target.empty() && !arg.starts_with("--")) {
            options.target = arg;
        } else {
            return usage_error("unexpected argument for export: " + arg);
        }
    }
    if (options.target.empty()) {
        return usage_error(
            "export needs a preset or a platform: macos, windows, linux, web, android or ios");
    }
    const slash::exporter::ExportResult result = slash::exporter::export_game(options);
    print(result.json);
    if (const json::Value* error = result.json.find("error"); error != nullptr) {
        return error->find("step")->as_string() == "options" ? kExitUsage : kExitFailure;
    }
    return result.ok ? kExitOk : kExitFailure;
}

json::Value toolchain_json(const slash::exporter::Toolchain& toolchain) {
    json::Value item;
    item["name"] = toolchain.name;
    item["version"] = toolchain.version;
    return item;
}

int cmd_toolchain_list() {
    namespace exporter = slash::exporter;
    const std::filesystem::path dir = exporter::toolchains_dir();
    json::Array toolchains;
    for (const exporter::Toolchain& toolchain : exporter::host_toolchains()) {
        json::Value item = toolchain_json(toolchain);
        item["description"] = toolchain.description;
        item["installed"] = exporter::installed_toolchain(toolchain, dir).has_value();
        toolchains.push_back(std::move(item));
    }
    json::Value result;
    result["dir"] = dir.generic_string();
    result["toolchains"] = std::move(toolchains);
    print(result);
    return kExitOk;
}

int cmd_toolchain_install(const std::vector<std::string>& names) {
    namespace exporter = slash::exporter;
    const std::vector<exporter::Toolchain> available = exporter::host_toolchains();
    if (available.empty()) {
        return usage_error("this machine builds every platform it can with its own compilers; "
                           "there are no toolchains to install");
    }
    // Each name is a toolchain, a platform whose toolchains to install, or "all".
    std::vector<std::string> wanted;
    const auto want = [&wanted](const std::string& name) {
        if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
            wanted.push_back(name);
        }
    };
    for (const std::string& name : names.empty() ? std::vector<std::string>{"all"} : names) {
        const bool is_toolchain =
            std::any_of(available.begin(), available.end(),
                        [&name](const exporter::Toolchain& t) { return t.name == name; });
        std::vector<std::string> expanded;
        if (is_toolchain) {
            expanded = {name};
        } else if (name == "all") {
            for (const exporter::Toolchain& toolchain : available) {
                expanded.push_back(toolchain.name);
            }
        } else if (name == "linux-arm64") {
            expanded = exporter::toolchains_for("linux", exporter::kLinuxArm64);
        } else if (std::find(exporter::kPlatforms.begin(), exporter::kPlatforms.end(), name) !=
                   exporter::kPlatforms.end()) {
            expanded = exporter::toolchains_for(name); // none for platforms built natively
        } else {
            return usage_error("unknown toolchain or platform: " + name +
                               " (slash-cli toolchain list shows them)");
        }
        std::for_each(expanded.begin(), expanded.end(), want);
    }

    const std::filesystem::path dir = exporter::toolchains_dir();
    json::Array installed;
    json::Array already;
    for (const exporter::Toolchain& toolchain : available) {
        if (std::find(wanted.begin(), wanted.end(), toolchain.name) == wanted.end()) {
            continue;
        }
        json::Value item = toolchain_json(toolchain);
        if (const auto folder = exporter::installed_toolchain(toolchain, dir)) {
            item["path"] = folder->generic_string();
            already.push_back(std::move(item));
            continue;
        }
        slash::log::info("installing toolchain",
                         {{"name", toolchain.name}, {"version", toolchain.version}});
        auto folder = exporter::install_toolchain(toolchain, dir);
        if (!folder) {
            return failure("could not install toolchain", folder.error());
        }
        item["path"] = folder.value().generic_string();
        installed.push_back(std::move(item));
    }
    json::Value result;
    result["installed"] = std::move(installed);
    result["already_installed"] = std::move(already);
    print(result);
    return kExitOk;
}

int cmd_toolchain(const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "list" && args[0] != "install")) {
        return usage_error("toolchain needs an action: list or install");
    }
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    for (const std::string& arg : rest) {
        if (arg.starts_with("--")) {
            return usage_error("unexpected argument for toolchain: " + arg);
        }
    }
    if (args[0] == "list") {
        if (!rest.empty()) {
            return usage_error("toolchain list takes no arguments");
        }
        return cmd_toolchain_list();
    }
    return cmd_toolchain_install(rest);
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
    if (name == "project") {
        return cmd_project(args);
    }
    if (name == "call") {
        return cmd_call(args);
    }
    if (name == "rpc") {
        return cmd_rpc(args);
    }
    if (name == "serve") {
        return cmd_serve(args);
    }
    if (name == "init") {
        return cmd_init(args);
    }
    if (name == "export") {
        return cmd_export(args);
    }
    if (name == "toolchain") {
        return cmd_toolchain(args);
    }
    return usage_error("unknown command: " + name);
}
