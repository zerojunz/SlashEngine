// Drawing the world at a lower resolution and scaling it up to the window, as Godot's
// rendering/scaling_3d settings, while the controls stay sharp at the window's size.
//
// project.json's rendering.scaling draws this game's world at half size and scales it up with
// AMD's FidelityFX Super Resolution 1. An agent changes the settings while the game runs with
// display.set, which slash-cli run --serve adds, and frame.capture shows what each draws. A game
// can add an upscaler of its own with render::add_upscaler: here a nearest-neighbor one for pixel
// art. Everything draws on the CPU, so the output is the same on every machine.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/upscaler.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

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

// A 40x40 planet: an orange disc with a dark band across it. Games ship PNG files made in an
// image editor or by a generator; this one is made here so the example has no binary file.
void write_planet(const char* path) {
    render::Image image{.width = 40, .height = 40, .pixels = {}};
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            // Distances from the center (19.5, 19.5), doubled to stay whole numbers.
            const int dx = (2 * x) - 39;
            const int dy = (2 * y) - 39;
            const bool inside = (dx * dx) + (dy * dy) <= 36 * 36;
            const bool band = y >= 16 && y < 22;
            const std::uint8_t red = band ? 120 : 240;
            const std::uint8_t green = band ? 50 : 140;
            const std::uint8_t blue = band ? 20 : 40;
            const std::uint8_t alpha = inside ? 255 : 0;
            image.pixels.insert(image.pixels.end(), {red, green, blue, alpha});
        }
    }
    if (auto written = slash::platform::write_file(path, render::encode_png(image).value());
        !written) {
        std::fprintf(stderr, "%s\n", written.error().message.c_str());
        std::exit(1);
    }
}

// Scales the render up with no filtering, so each of its pixels becomes a square of the
// window's: what pixel art wants. An upscaler makes its textures on the device it is given; a
// vendor's (DLSS, XeSS) would call the vendor's library here, from an SDK package's glue code.
class PixelUpscaler final : public render::Upscaler {
public:
    slash::Result<render::TextureId> input(render::RenderDevice& device,
                                           const render::UpscaleSize& size) override {
        if (input_ != render::TextureId::None && size.render_width == width_ &&
            size.render_height == height_) {
            return input_;
        }
        release(device);
        auto made = device.create_texture({.width = size.render_width,
                                           .height = size.render_height,
                                           .filter = render::Filter::Nearest,
                                           .render_target = true});
        if (!made) {
            return made.error();
        }
        input_ = made.value();
        width_ = size.render_width;
        height_ = size.render_height;
        return input_;
    }

    // The input, drawn over the window as it is: the nearest filter does the scaling.
    slash::Result<render::UpscaleDraw> upscale(render::RenderDevice& /*device*/,
                                               const render::UpscaleSize& /*size*/,
                                               const render::Scaling& /*scaling*/) override {
        return render::UpscaleDraw{.texture = input_, .material = {}};
    }

    void release(render::RenderDevice& device) override {
        if (input_ != render::TextureId::None) {
            device.destroy_texture(input_);
            input_ = render::TextureId::None;
        }
    }

private:
    render::TextureId input_ = render::TextureId::None;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace

int main() {
    write_planet("planet.png");

    // A game adds its upscalers before it draws, for project.json and display.set to name.
    // Temporal ones (FSR 2, DLSS, XeSS) need the 3D renderer's depth and motion vectors.
    if (auto added = render::add_upscaler({.name = "pixel",
                                           .temporal = false,
                                           .supports = {},
                                           .make =
                                               [] {
                                                   return std::unique_ptr<render::Upscaler>(
                                                       std::make_unique<PixelUpscaler>());
                                               }});
        !added) {
        std::fprintf(stderr, "%s\n", added.error().message.c_str());
        return 1;
    }
    const auto refused =
        render::add_upscaler({.name = "dlss", .temporal = true, .supports = {}, .make = [] {
                                  return std::unique_ptr<render::Upscaler>();
                              }});
    std::printf("add_upscaler dlss -> %s\n", refused.error().message.c_str());

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

    // Pixels across the planet's left edge (the band's row), then across the health bar's left
    // edge. The bar is a control: it draws at the window's size whatever the world's scale.
    const std::string_view points =
        R"({"points": [[11, 31], [12, 31], [13, 31], [14, 31], [15, 31], [63, 6], [64, 6]]})";

    // project.json: FSR from half size. The world draws at 48x32.
    call(session, "display.list", "{}");
    call(session, "frame.capture", points);

    // Full size: no upscaler runs, and the planet's edge is one sharp step.
    call(session, "display.set", R"({"scaling": {"mode": "bilinear", "scale": 1.0}})");
    call(session, "frame.capture", points);

    // Half size with a filtered stretch: the edge blurs over two pixels.
    call(session, "display.set", R"({"scaling": {"scale": 0.5}})");
    call(session, "frame.capture", points);

    // FSR at its softest (sharpness 2 stops): RCAS sharpens less than the project's 0.2.
    call(session, "display.set", R"({"scaling": {"mode": "fsr", "fsr_sharpness": 2.0}})");
    call(session, "frame.capture", points);

    // The game's own upscaler at a quarter size: blocks of four pixels.
    call(session, "display.set", R"({"scaling": {"upscaler": "pixel", "scale": 0.25}})");
    call(session, "frame.capture", points);

    // MetalFX isn't built; unknown upscalers get the closest name.
    call(session, "display.set", R"({"scaling": {"mode": "metalfx_temporal"}})");
    call(session, "display.set", R"({"scaling": {"upscaler": "pixle"}})");
    return 0;
}
