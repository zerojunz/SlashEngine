// Commands that reach a running game beyond its world: its displays, how it presents frames and
// how evenly they arrive. slash-cli run --serve adds them.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/perf.hpp"
#include "slash/command/session.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

// The settings that shape frames: V-Sync, the frame cap and the render scale, with the upscaler
// and render size the last frame drew with.
json::Value frame_settings(const LiveGame& game, const Session& session) {
    json::Value out;
    out["vsync_mode"] = render::vsync_mode_name(game.renderer->vsync());
    out["max_fps"] = game.loop->max_fps();
    json::Value scaling = render::scaling_json(canvas::settings(session.world()).scaling);
    if (game.canvas != nullptr) {
        const canvas::CanvasRenderer::Scaled& scaled = game.canvas->scaled();
        scaling["in_effect"] = scaled.upscaler.empty() ? "none" : scaled.upscaler;
        if (!scaled.upscaler.empty()) {
            scaling["render_size"] =
                json::Array{scaled.size.render_width, scaled.size.render_height};
        }
    }
    out["scaling"] = std::move(scaling);
    return out;
}

// display.set's scaling, over the settings in effect: a mode, a scale, FSR's sharpness or a
// game's own upscaler.
Reply set_scaling(Session& session, const json::Value& params) {
    canvas::Settings settings = canvas::settings(session.world());
    render::Scaling& scaling = settings.scaling;
    if (const json::Value* mode = params.find("mode"); mode != nullptr) {
        if (const std::string problem = render::scaling_mode_problem(mode->as_string());
            !problem.empty()) {
            return param_error(code::kInvalidArgument, "/scaling/mode", problem);
        }
        scaling.mode = mode->as_string();
    }
    if (const json::Value* scale = params.find("scale"); scale != nullptr) {
        scaling.scale = static_cast<float>(scale->as_double());
    }
    if (const json::Value* sharpness = params.find("fsr_sharpness"); sharpness != nullptr) {
        scaling.fsr_sharpness = static_cast<float>(sharpness->as_double());
    }
    if (const json::Value* upscaler = params.find("upscaler"); upscaler != nullptr) {
        const std::string& name = upscaler->as_string();
        if (!name.empty() && !render::find_upscaler(name)) {
            const std::vector<std::string> names = render::upscaler_names();
            std::string message = "the game has no upscaler named " + name + "; it has";
            for (std::size_t i = 0; i < names.size(); ++i) {
                message += (i == 0 ? " " : ", ") + names[i];
            }
            if (const std::string_view near = closest_match(name, names); !near.empty()) {
                message += " (did you mean " + std::string(near) + "?)";
            }
            return param_error(code::kNotFound, "/scaling/upscaler", message);
        }
        scaling.upscaler = name;
    }
    canvas::set_settings(session.world(), settings);
    return Reply::success(json::Value());
}

Reply display_list(const LiveGame& game, const Session& session, const json::Value& params) {
    const json::Value* modes = params.find("modes");
    const bool with_modes = modes != nullptr && modes->as_bool();
    json::Array displays;
    for (const platform::Display& display : game.platform->displays()) {
        displays.push_back(display_json(display, with_modes));
    }
    json::Value out = frame_settings(game, session);
    out["displays"] = std::move(displays);
    return Reply::success(std::move(out));
}

Reply display_set(const LiveGame& game, Session& session, const json::Value& params) {
    // The scaling first, as it is the one that can fail; the schema has checked the rest.
    if (const json::Value* scaling = params.find("scaling"); scaling != nullptr) {
        if (Reply set = set_scaling(session, *scaling); !set.ok) {
            return set;
        }
    }
    if (const json::Value* vsync = params.find("vsync_mode"); vsync != nullptr) {
        game.renderer->set_vsync(
            render::parse_vsync_mode(vsync->as_string()).value_or(render::VsyncMode::Enabled));
    }
    if (const json::Value* max_fps = params.find("max_fps"); max_fps != nullptr) {
        game.loop->set_max_fps(static_cast<int>(max_fps->as_int()));
    }
    return Reply::success(frame_settings(game, session));
}

Reply perf_stats(const LiveGame& game, const Session& session, const json::Value& params) {
    const platform::FrameStats frames = game.loop->frame_stats();
    json::Value out = world_perf_stats(session, params);
    json::Value settings = frame_settings(game, session);
    for (auto& [key, value] : settings.as_object()) {
        out[key] = std::move(value);
    }
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
    if (json::Value* memory = out.find("memory"); memory != nullptr) {
        // The renderer's textures: sprites', glyph atlases' and render targets'.
        const render::Stats device = game.renderer->stats();
        (*memory)["gpu"]["bytes"] = static_cast<std::int64_t>(device.texture_bytes);
        (*memory)["gpu"]["textures"] = static_cast<std::int64_t>(device.textures);
    }
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
    json::Value scale = property("number", "The world's size over the screen's.");
    scale["minimum"] = render::kMinRenderScale;
    scale["maximum"] = render::kMaxRenderScale;
    json::Value sharpness = property("number", "FSR's sharpening in stops; 0 is the sharpest.");
    sharpness["minimum"] = 0;
    sharpness["maximum"] = render::kMaxFsrSharpness;
    json::Value scaling = params_schema({
        {"mode", property("string", "bilinear or fsr.")},
        {"scale", std::move(scale)},
        {"fsr_sharpness", std::move(sharpness)},
        {"upscaler", property("string", "An upscaler the game added; \"\" for none.")},
    });
    scaling["description"] = "The render scale, as project.json's rendering.scaling; fields left "
                             "out stay as they are.";

    registry.add({
        .name = "display.list",
        .description = "Lists the displays with their refresh rates, and the game's V-Sync mode, "
                       "frame cap and render scale.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"modes", property("boolean", "Also list each display's fullscreen modes.")},
        }),
        .handler =
            [game](Session& session, const json::Value& params) {
                return display_list(game, session, params);
            },
    });
    registry.add({
        .name = "display.set",
        .description = "Changes the game's V-Sync mode, frame cap or render scale while it runs, "
                       "and returns the settings in effect.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({
            {"vsync_mode", std::move(vsync_mode)},
            {"max_fps", std::move(max_fps)},
            {"scaling", std::move(scaling)},
        }),
        .handler = [game](Session& session,
                          const json::Value& params) { return display_set(game, session, params); },
    });
    // The world's perf.stats, with frame pacing.
    if (registry.find("perf.stats") == nullptr) {
        register_perf_commands(registry);
    }
    Command stats = *registry.find("perf.stats");
    stats.description = "Frame pacing over the last 120 frames (fps, frame times in ms and "
                        "catch-up frames), tick and system times over the last 120 ticks, the "
                        "slowest systems first, the V-Sync mode, frame cap and render scale, and "
                        "with memory, what each part of the engine holds, GPU textures too.";
    stats.handler = [game](Session& session, const json::Value& params) {
        return perf_stats(game, session, params);
    };
    registry.replace(std::move(stats));
}

} // namespace slash::command
