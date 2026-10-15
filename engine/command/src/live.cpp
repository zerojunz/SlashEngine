// Commands that reach a running game beyond its world: its displays, how it presents frames and
// how evenly they arrive. slash-cli run --serve adds them.

#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/assert.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

// Two decimals are plenty for rates and times, and keep replies short.
double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    if (!properties.empty()) {
        schema["properties"] = std::move(properties);
    }
    return schema;
}

json::Value mode_json(const platform::DisplayMode& mode) {
    json::Value out;
    out["width"] = mode.width;
    out["height"] = mode.height;
    out["refresh_rate"] = round2(mode.refresh_rate);
    out["pixel_density"] = round2(mode.pixel_density);
    return out;
}

json::Value display_json(const platform::Display& display, bool with_modes) {
    json::Value out;
    out["id"] = display.id;
    out["name"] = display.name;
    out["primary"] = display.primary;
    out["window"] = display.has_window;
    out["bounds"] = json::Array{display.x, display.y, display.width, display.height};
    out["scale"] = round2(display.content_scale);
    out["refresh_rate"] = round2(display.mode.refresh_rate);
    out["pixel_density"] = round2(display.mode.pixel_density);
    // Every rate a fullscreen mode offers, fastest first, so a game can offer them as choices.
    std::vector<double> rates;
    rates.reserve(display.modes.size());
    for (const platform::DisplayMode& mode : display.modes) {
        rates.push_back(round2(mode.refresh_rate));
    }
    std::ranges::sort(rates, std::greater<>());
    rates.erase(std::ranges::unique(rates).begin(), rates.end());
    json::Array rate_list;
    for (const double rate : rates) {
        rate_list.emplace_back(rate);
    }
    out["refresh_rates"] = std::move(rate_list);
    if (with_modes) {
        json::Array modes;
        for (const platform::DisplayMode& mode : display.modes) {
            modes.push_back(mode_json(mode));
        }
        out["modes"] = std::move(modes);
    }
    return out;
}

json::Value frame_settings(const LiveGame& game) {
    json::Value out;
    out["vsync_mode"] = render::vsync_mode_name(game.renderer->vsync());
    out["max_fps"] = game.loop->max_fps();
    return out;
}

Reply display_list(const LiveGame& game, const json::Value& params) {
    const json::Value* modes = params.find("modes");
    const bool with_modes = modes != nullptr && modes->as_bool();
    json::Array displays;
    for (const platform::Display& display : game.platform->displays()) {
        displays.push_back(display_json(display, with_modes));
    }
    json::Value out = frame_settings(game);
    out["displays"] = std::move(displays);
    return Reply::success(std::move(out));
}

Reply display_set(const LiveGame& game, const json::Value& params) {
    // The schema has already checked both values.
    if (const json::Value* vsync = params.find("vsync_mode"); vsync != nullptr) {
        game.renderer->set_vsync(
            render::parse_vsync_mode(vsync->as_string()).value_or(render::VsyncMode::Enabled));
    }
    if (const json::Value* max_fps = params.find("max_fps"); max_fps != nullptr) {
        game.loop->set_max_fps(static_cast<int>(max_fps->as_int()));
    }
    return Reply::success(frame_settings(game));
}

Reply perf_stats(const LiveGame& game, const json::Value& /*params*/) {
    const platform::FrameStats frames = game.loop->frame_stats();
    json::Value out = frame_settings(game);
    out["fps"] = round2(frames.fps);
    json::Value frame_ms;
    frame_ms["avg"] = round2(frames.frame_ms_avg);
    frame_ms["min"] = round2(frames.frame_ms_min);
    frame_ms["max"] = round2(frames.frame_ms_max);
    out["frame_ms"] = std::move(frame_ms);
    out["frames"] = frames.frames;
    out["catch_up_frames"] = frames.catch_up_frames;
    out["ticks"] = game.loop->stats().ticks;
    out["tick_hz"] = round2(game.loop->tick_hz());
    out["renderer"] = render::backend_name(game.renderer->backend());
    return Reply::success(std::move(out));
}

} // namespace

void register_live_commands(Registry& registry, const LiveGame& game) {
    SLASH_ASSERT(game.platform != nullptr && game.loop != nullptr && game.renderer != nullptr,
                 "register_live_commands needs the platform, loop and renderer");
    using enum Capability;

    json::Value vsync_mode = property("string", "How frames wait for the display. Unsupported "
                                                "modes fall back to enabled.");
    vsync_mode["enum"] = json::Array{"disabled", "enabled", "adaptive", "mailbox"};
    json::Value max_fps = property("integer", "Frames per second at most; 0 means no cap.");
    max_fps["minimum"] = 0;
    max_fps["maximum"] = platform::kMaxFps;

    registry.add({
        .name = "display.list",
        .description = "Lists the displays with their refresh rates, and the game's V-Sync mode "
                       "and frame cap.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"modes", property("boolean", "Also list each display's fullscreen modes.")},
        }),
        .handler = [game](Session& /*session*/,
                          const json::Value& params) { return display_list(game, params); },
    });
    registry.add({
        .name = "display.set",
        .description = "Changes the game's V-Sync mode or frame cap while it runs, and returns "
                       "the settings in effect.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({
            {"vsync_mode", std::move(vsync_mode)},
            {"max_fps", std::move(max_fps)},
        }),
        .handler = [game](Session& /*session*/,
                          const json::Value& params) { return display_set(game, params); },
    });
    registry.add({
        .name = "perf.stats",
        .description = "Frame pacing over the last 120 frames: fps, frame times and catch-up "
                       "frames, with the V-Sync mode and frame cap.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = [game](Session& /*session*/,
                          const json::Value& params) { return perf_stats(game, params); },
    });
}

} // namespace slash::command
