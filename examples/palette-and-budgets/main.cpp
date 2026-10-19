// Palette and budget checks: storyboard.check holds a game's textures to the style its storyboard
// records. A palette entry's colors are the only ones the textures in its links may use, and a
// budget's texture_size limits the textures it covers, every texture when it links nothing.
//
// "Moss Keep" is a pixel-art game drawn in the PICO-8 colors. main.cpp draws its textures in code,
// so the repository holds no binary files: a hero on the palette, a crate another agent drew in
// colors close to the palette but not on it, and a title screen wider than the budget. Then it
// makes the calls an agent would: records the palette and the budget, briefs before touching the
// crate, checks, snaps the crate's colors to the nearest palette colors, redraws the title smaller
// and checks again.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace platform = slash::platform;
namespace render = slash::render;
namespace world = slash::world;

using Rgb = std::array<std::uint8_t, 3>;

// The PICO-8 colors the game uses.
constexpr std::array<Rgb, 6> kPalette = {{
    {0x00, 0x00, 0x00}, // black
    {0x1d, 0x2b, 0x53}, // dark_blue
    {0x00, 0x87, 0x51}, // dark_green
    {0xab, 0x52, 0x36}, // brown
    {0xff, 0xec, 0x27}, // yellow
    {0xff, 0xf1, 0xe8}, // white
}};

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

// An image drawn by a function from (x, y) to a color, or to nothing for a clear pixel.
template <typename Draw>
render::Image draw(std::uint32_t width, std::uint32_t height, Draw&& color_at) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::optional<Rgb> color = color_at(x, y);
            const Rgb rgb = color.value_or(Rgb{});
            image.pixels.insert(image.pixels.end(), {rgb[0], rgb[1], rgb[2],
                                                     static_cast<std::uint8_t>(color ? 255 : 0)});
        }
    }
    return image;
}

void save(const std::string& path, const render::Image& image) {
    if (!platform::write_file(path, render::encode_png(image).value()).ok()) {
        fail("can't write " + path);
    }
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Pretty).c_str());
    }
};

} // namespace

int main() {
    if (!platform::create_directories("art/sprites").ok()) {
        fail("can't make art/sprites");
    }
    // The hero, 8 by 8: a yellow face in a dark blue hood, on a clear background.
    save("art/sprites/hero.png", draw(8, 8, [](std::uint32_t x, std::uint32_t y) {
             const bool hood = y < 7 && x > 0 && x < 7;
             const bool face = y > 1 && y < 6 && x > 1 && x < 6;
             const bool eye = y == 3 && (x == 3 || x == 4);
             if (eye) {
                 return std::optional(kPalette[0]);
             }
             if (face) {
                 return std::optional(kPalette[4]);
             }
             return hood ? std::optional(kPalette[1]) : std::nullopt;
         }));
    // The crate another agent drew: brown planks edged in near-black, close to the palette but off
    // it.
    save("art/sprites/crate.png", draw(8, 8, [](std::uint32_t x, std::uint32_t y) {
             const bool edge = x == 0 || y == 0 || x == 7 || y == 7 || x == y;
             return std::optional(edge ? Rgb{0x2a, 0x1a, 0x10} : Rgb{0xb0, 0x55, 0x30});
         }));
    // The title screen, 320 by 64: wider than the budget allows.
    save("art/title.png", draw(320, 64, [](std::uint32_t /*x*/, std::uint32_t y) {
             return std::optional(y < 32 ? kPalette[1] : kPalette[2]);
         }));

    const Agent agent;
    // The style, recorded once: the palette for the sprites, and a budget for every texture.
    agent.call("storyboard.edit", R"({"why": "Record the art style", "by": "art-director",
        "edits": [
        {"id": "palette/pico", "version": null, "set": {"name": "PICO-8",
         "summary": "The sprites use only these colors.", "links": ["art/sprites"],
         "colors": {"black": "#000000", "dark_blue": "#1d2b53", "dark_green": "#008751",
                    "brown": "#ab5236", "yellow": "#ffec27", "white": "#fff1e8"}}},
        {"id": "budget/textures", "version": null, "set": {"name": "Textures",
         "summary": "No texture over 256 pixels a side.", "texture_size": 256}}]})");
    // Before touching the crate, the brief brings the palette, since it links the crate's folder,
    // and the budget, which covers everything.
    agent.call("storyboard.brief", R"({"files": ["art/sprites/crate.png"]})");
    agent.call("storyboard.check", "{}");

    // The fix: each of the crate's pixels snapped to the nearest palette color.
    auto crate = render::decode_image(platform::read_file("art/sprites/crate.png").value());
    if (!crate) {
        fail(crate.error().message);
    }
    for (std::size_t i = 0; i + 3 < crate.value().pixels.size(); i += 4) {
        std::uint8_t* pixel = &crate.value().pixels[i];
        const Rgb* nearest = nullptr;
        int best = 0;
        for (const Rgb& color : kPalette) {
            int distance = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const int d = int{color[c]} - int{pixel[c]};
                distance += d * d;
            }
            if (nearest == nullptr || distance < best) {
                nearest = &color;
                best = distance;
            }
        }
        pixel[0] = (*nearest)[0];
        pixel[1] = (*nearest)[1];
        pixel[2] = (*nearest)[2];
    }
    save("art/sprites/crate.png", crate.value());
    // The title, redrawn at 256 by 64.
    save("art/title.png", draw(256, 64, [](std::uint32_t /*x*/, std::uint32_t y) {
             return std::optional(y < 32 ? kPalette[1] : kPalette[2]);
         }));
    agent.call("storyboard.check", "{}");
}
