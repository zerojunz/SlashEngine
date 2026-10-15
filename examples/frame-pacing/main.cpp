// V-Sync, the frame cap and frame pacing, read and changed while a game runs.
//
// slash-cli run --serve registers display.list, display.set and perf.stats, so an agent can
// tune a running game over MCP or REST the same way. A game's options menu calls the functions
// underneath: RenderDevice::set_vsync and Loop::set_max_fps. This example runs headless, where
// there are no displays and frames never wait, so its output is the same on every machine.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Calls a command and prints its result or error, as an agent would see it.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const command::Reply reply = session.call(name, json::parse(params).value());
    const std::string shown =
        reply.ok ? json::write(reply.value, json::Style::Compact) : "error: " + reply.message;
    std::printf("%.*s %.*s\n  -> %s\n", static_cast<int>(name.size()), name.data(),
                static_cast<int>(params.size()), params.data(), shown.c_str());
}

} // namespace

int main() {
    auto platform = slash::platform::Platform::create({.headless = true});
    // The settings project.json would give: "max_fps": 60 and "window": {"vsync_mode": ...}.
    auto loop = slash::platform::Loop::create(platform.value(),
                                              {.tick_hz = 60.0, .max_ticks = 600, .max_fps = 60});
    // A window would get a GPU device; set_vsync is how a game applies its V-Sync setting.
    const auto renderer = slash::render::create_null_device();
    renderer->set_vsync(slash::render::VsyncMode::Enabled);

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::register_live_commands(
        commands,
        {.platform = &platform.value(), .loop = &loop.value(), .renderer = renderer.get()});
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    // Headless runs have no displays; a window lists each display with its refresh rates.
    call(session, "display.list", "{}");

    // Mailbox shows the newest frame at each refresh: low latency with no tearing. A GPU that
    // lacks a mode falls back to enabled, and the reply says which mode is in effect.
    call(session, "display.set", R"({"vsync_mode": "mailbox", "max_fps": 120})");
    call(session, "display.set", R"({"max_fps": 5000})");

    for (int frame = 0; frame < 90; ++frame) {
        loop.value().frame(nullptr);
    }
    // perf.stats also returns fps and frame times (avg, min and max over the last 120 frames),
    // which depend on the machine, so only the counts are printed here.
    const command::Reply stats = session.call("perf.stats", json::Value());
    std::printf("perf.stats {}\n  -> frames %lld, ticks %lld, catch-up frames %lld, vsync_mode %s, "
                "max_fps %lld, renderer %s\n",
                static_cast<long long>(stats.value.find("frames")->as_int()),
                static_cast<long long>(stats.value.find("ticks")->as_int()),
                static_cast<long long>(stats.value.find("catch_up_frames")->as_int()),
                stats.value.find("vsync_mode")->as_string().c_str(),
                static_cast<long long>(stats.value.find("max_fps")->as_int()),
                stats.value.find("renderer")->as_string().c_str());
    return 0;
}
