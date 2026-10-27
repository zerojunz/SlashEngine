// A game's own temporal upscaler, as DLSS, XeSS or FSR 4 come in from a vendor's SDK.
//
// render::add_upscaler takes one marked temporal with a make_temporal function. Where the project
// names it and FSR 2 would run (Forward+, a scale under 1), the device runs it in FSR 2's place
// and hands it each frame's color, depth and motion vectors at the render size, the output at the
// window's size, the camera's jitter and whether its history starts over. On a GPU those are the
// native API's objects (a VkImage, an ID3D12Resource, an MTLTexture) and the command buffer, which
// a vendor's library takes; on the software device, which frame.capture draws with, they are
// float pixels, so a game sees what its upscaler gets on any machine.
//
// This one splats: each frame it moves its history by the motion vectors, then writes each render
// pixel's color into the window pixel its jittered sample fell in. Over a few frames the jitter
// visits every window pixel, so edges come out at the window's resolution from frames drawn at a
// quarter of its pixels. Everything draws on the CPU, so the output is the same on every machine.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;

// Calls a command and prints its result or error, as an agent would see it.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const command::Reply reply = session.call(name, json::parse(params).value());
    const std::string shown =
        reply.ok ? json::write(reply.value, json::Style::Compact) : "error: " + reply.message;
    std::printf("%.*s %.*s\n  -> %s\n", static_cast<int>(name.size()), name.data(),
                static_cast<int>(params.size()), params.data(), shown.c_str());
}

// Where pixel (x, y)'s first channel is in an image width pixels wide of four channels.
std::ptrdiff_t offset(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return static_cast<std::ptrdiff_t>(((std::size_t{y} * width) + x) * 4);
}

// Moves its history along the motion vectors, then splats the frame's samples into it.
class Splat final : public render::TemporalUpscaler {
public:
    slash::Result<void> upscale(const render::TemporalFrame& frame) override {
        if (frame.device.backend != render::Backend::Software) {
            // A vendor's library would take frame.device, frame.commands and the textures'
            // images here; this example only runs on the CPU.
            return slash::make_error(slash::ErrorCode::InvalidArgument, "splat runs on the CPU");
        }
        const render::NativeTexture& color = frame.color;
        const render::NativeTexture& out = frame.output;
        const std::size_t pixels = std::size_t{out.width} * out.height;
        const auto nearest = [&](float u, float v) {
            const auto x = static_cast<std::uint32_t>(std::clamp(
                u * static_cast<float>(color.width), 0.0F, static_cast<float>(color.width - 1)));
            const auto y = static_cast<std::uint32_t>(std::clamp(
                v * static_cast<float>(color.height), 0.0F, static_cast<float>(color.height - 1)));
            return (std::size_t{y} * color.width) + x;
        };

        // A first frame, or a new size, starts from the render pixels stretched.
        if (frame.reset || history_.size() != pixels * 4) {
            history_.assign(pixels * 4, 0.0F);
            for (std::uint32_t y = 0; y < out.height; ++y) {
                for (std::uint32_t x = 0; x < out.width; ++x) {
                    const std::size_t from =
                        nearest((static_cast<float>(x) + 0.5F) / static_cast<float>(out.width),
                                (static_cast<float>(y) + 0.5F) / static_cast<float>(out.height));
                    std::copy_n(color.pixels.begin() + static_cast<std::ptrdiff_t>(from) * 4, 4,
                                history_.begin() + offset(x, y, out.width));
                }
            }
        }

        // Each window pixel was at its place plus its motion a frame before (in texture
        // coordinates), so it takes the history from there.
        std::vector<float> moved(history_.size());
        for (std::uint32_t y = 0; y < out.height; ++y) {
            for (std::uint32_t x = 0; x < out.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(out.width);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(out.height);
                const std::size_t at = nearest(u, v);
                const float was_u = u + frame.motion.pixels[at * 2];
                const float was_v = v + frame.motion.pixels[(at * 2) + 1];
                const auto hx =
                    static_cast<std::uint32_t>(std::clamp(was_u * static_cast<float>(out.width),
                                                          0.0F, static_cast<float>(out.width - 1)));
                const auto hy = static_cast<std::uint32_t>(
                    std::clamp(was_v * static_cast<float>(out.height), 0.0F,
                               static_cast<float>(out.height - 1)));
                std::copy_n(history_.begin() + offset(hx, hy, out.width), 4,
                            moved.begin() + offset(x, y, out.width));
            }
        }
        history_ = std::move(moved);

        // The jitter moved the picture by jitter render pixels, so a render pixel's center saw
        // the point jitter to its left and above: that window pixel takes its color.
        const float scale_x = static_cast<float>(out.width) / static_cast<float>(color.width);
        const float scale_y = static_cast<float>(out.height) / static_cast<float>(color.height);
        for (std::uint32_t y = 0; y < color.height; ++y) {
            for (std::uint32_t x = 0; x < color.width; ++x) {
                const float sx = (static_cast<float>(x) + 0.5F - frame.jitter[0]) * scale_x;
                const float sy = (static_cast<float>(y) + 0.5F - frame.jitter[1]) * scale_y;
                if (sx < 0.0F || sy < 0.0F || sx >= static_cast<float>(out.width) ||
                    sy >= static_cast<float>(out.height)) {
                    continue;
                }
                const auto ox = static_cast<std::uint32_t>(sx);
                const auto oy = static_cast<std::uint32_t>(sy);
                std::copy_n(color.pixels.begin() + offset(x, y, color.width), 4,
                            history_.begin() + offset(ox, oy, out.width));
            }
        }
        std::copy(history_.begin(), history_.end(), out.pixels.begin());

        // What the device handed over, for the example to print: the farthest anything moved
        // is in window pixels.
        float moved_most = 0.0F;
        for (std::size_t i = 0; i + 1 < frame.motion.pixels.size(); i += 2) {
            moved_most =
                std::max(moved_most,
                         std::hypot(frame.motion.pixels[i] * static_cast<float>(out.width),
                                    frame.motion.pixels[i + 1] * static_cast<float>(out.height)));
        }
        std::printf("  splat: frame %d, %ux%u to %ux%u, jitter (%.3f, %.3f), moved %.1f%s\n",
                    ++frames_, color.width, color.height, out.width, out.height,
                    static_cast<double>(frame.jitter[0]), static_cast<double>(frame.jitter[1]),
                    static_cast<double>(moved_most), frame.reset ? ", reset" : "");
        return {};
    }

private:
    std::vector<float> history_;
    int frames_ = 0;
};

} // namespace

int main() {
    // A game adds its upscalers before it draws, for project.json and display.set to name. An
    // SDK package's register function would add the vendor's, and its supports function would
    // check the GPU and driver.
    if (auto added = render::add_upscaler({.name = "splat",
                                           .temporal = true,
                                           .supports = {},
                                           .make = {},
                                           .make_temporal =
                                               [] {
                                                   return std::unique_ptr<render::TemporalUpscaler>(
                                                       std::make_unique<Splat>());
                                               }});
        !added) {
        std::fprintf(stderr, "%s\n", added.error().message.c_str());
        return 1;
    }

    auto platform = slash::platform::Platform::create({.headless = true});
    auto loop =
        slash::platform::Loop::create(platform.value(), {.tick_hz = 60.0, .max_ticks = 600});
    const auto renderer = slash::render::create_null_device();
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
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Pixels across the card's left edge, which steps two window pixels at a time while each
    // render pixel covers four of them.
    const std::string_view points =
        R"({"points": [[59, 45], [60, 45], [61, 45], [62, 45], [63, 45], [64, 45]]})";
    const std::string_view settle =
        R"({"points": [[59, 45], [60, 45], [61, 45], [62, 45], [63, 45], [64, 45]], "frames": 7})";

    // The first frame starts the history over: the card at 80x45, stretched.
    call(session, "frame.capture", points);
    // Seven more frames splat samples between the render pixels' centers.
    call(session, "frame.capture", settle);
    // At full size nothing upscales, so the splat isn't called: the edge is where it settled.
    call(session, "display.set", R"({"scaling": {"scale": 1.0}})");
    call(session, "frame.capture", points);

    // Back at half size, a new splat starts its history over, then settles again.
    call(session, "display.set", R"({"scaling": {"scale": 0.5}})");
    call(session, "frame.capture", points);
    call(session, "frame.capture", settle);

    // The card turns: motion vectors carry the history to where the card went, and the frame's
    // samples land on the new edge.
    call(session, "component.set",
         R"({"id": "card", "components": {"Transform3D": {"rotation": [0.0, 0.0, 0.6]}}})");
    call(session, "frame.capture", points);
    call(session, "display.set", R"({"scaling": {"scale": 1.0}})");
    call(session, "frame.capture", points);
    return 0;
}
