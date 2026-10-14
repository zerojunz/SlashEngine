// slash-cli: the command-line entry point agents use to drive SlashEngine.
// Results go to stdout as JSON; logs go to stderr as JSON lines.

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
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = slash::json;
namespace world = slash::world;

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
  slash-cli help                     Show this message

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

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::fwrite(kUsage.data(), 1, kUsage.size(), stdout);
        return args.empty() ? kExitUsage : kExitOk;
    }

    const std::string command = args[0];
    args.erase(args.begin());
    if (command == "version" || command == "--version") {
        return cmd_version();
    }
    if (command == "run") {
        return cmd_run(args);
    }
    if (command == "scene") {
        return cmd_scene(args);
    }
    if (command == "schema") {
        return cmd_schema(args);
    }
    return usage_error("unknown command: " + command);
}
