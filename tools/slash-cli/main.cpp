// slash-cli: the command-line entry point agents use to drive SlashEngine.
// Results go to stdout as JSON; logs go to stderr as JSON lines.

#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr std::string_view kUsage = R"(slash-cli: the SlashEngine command-line tool

Usage:
  slash-cli version          Print the engine version as JSON
  slash-cli run [options]    Start the engine and tick it
      --headless             Run with no window or GPU
      --ticks <n>            Stop after n fixed ticks (required with --headless)
      --hz <rate>            Fixed tick rate in Hz (default 60)
  slash-cli help             Show this message
)";

void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

int usage_error(std::string_view message) {
    slash::log::error("usage error", {{"detail", message}});
    std::fwrite(kUsage.data(), 1, kUsage.size(), stderr);
    return kExitUsage;
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

int cmd_version() {
    print(R"({"name":"SlashEngine","version":")" + slash::log::escape_json(slash::version()) +
          "\"}\n");
    return kExitOk;
}

int cmd_run(const std::vector<std::string>& args) {
    slash::platform::Config config;
    config.title = "SlashEngine";
    slash::platform::RunOptions options;

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
        } else {
            return usage_error("unknown option for run: " + arg);
        }
    }

    auto platform = slash::platform::Platform::create(config);
    if (!platform) {
        slash::log::error("could not start the platform",
                          {{"code", slash::to_string(platform.error().code)},
                           {"detail", platform.error().message}});
        return kExitFailure;
    }

    auto stats = slash::platform::run(platform.value(), options, nullptr);
    if (!stats) {
        slash::log::error("run failed", {{"code", slash::to_string(stats.error().code)},
                                         {"detail", stats.error().message}});
        return kExitFailure;
    }

    print("{\"ticks\":" + std::to_string(stats.value().ticks) +
          ",\"quit_requested\":" + (stats.value().quit_requested ? "true" : "false") +
          ",\"headless\":" + (config.headless ? "true" : "false") + "}\n");
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print(kUsage);
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
    return usage_error("unknown command: " + command);
}
