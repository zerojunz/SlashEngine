// App icons for every platform, made from one image.
//
// project.json's "icon" names a square image, as Godot's application/config/icon; exports make
// each platform's icons from it, and presets can change a few things per platform. This makes a
// project in code whose icon is a gold coin on a clear background, then the icon files each
// platform's export ships.

#include "slash/core/json.hpp"
#include "slash/exporter/icons.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/icon.hpp"
#include "slash/runtime/project.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace exporter = slash::exporter;
namespace render = slash::render;

void write(const std::string& path, std::string_view text) {
    if (!slash::platform::write_file(path, text)) {
        std::printf("could not write %s\n", path.c_str());
        std::exit(1);
    }
}

// A gold coin with a darker rim, on a clear background, size pixels on a side.
render::Image coin(std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    const std::int64_t center = size / 2;
    const std::int64_t outer = (center - 4) * (center - 4);
    const std::int64_t inner = (center - 40) * (center - 40);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::int64_t dx = std::int64_t{x} - center;
            const std::int64_t dy = std::int64_t{y} - center;
            const std::int64_t distance = (dx * dx) + (dy * dy);
            const bool rim = distance >= inner;
            image.pixels.push_back(rim ? 200 : 250);
            image.pixels.push_back(rim ? 140 : 200);
            image.pixels.push_back(rim ? 20 : 60);
            image.pixels.push_back(distance < outer ? 255 : 0);
        }
    }
    return image;
}

std::string color(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = &image.pixels[((static_cast<std::size_t>(y) * image.width) + x) * 4];
    char text[32];
    std::snprintf(text, sizeof(text), "%u,%u,%u,%u", p[0], p[1], p[2], p[3]);
    return text;
}

std::string size_of(std::string_view png) {
    const render::Image image = render::decode_image(png).value();
    return std::to_string(image.width) + "x" + std::to_string(image.height);
}

std::uint32_t be32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<std::uint8_t>(bytes[at + i]);
    }
    return value;
}

} // namespace

int main() {
    (void)slash::platform::create_directories("game/art");
    write("game/project.json", R"({"format": 1, "name": "Coins", "main_scene": "main.scene.json",
        "icon": "art/coin.png"})");
    write("game/art/coin.png", render::encode_png(coin(1024)).value());
    // Android's adaptive icon and iOS's opaque icon get a night-blue background.
    write("game/export_presets.json", R"({"format": 1, "presets": {
        "android": {"platform": "android", "android": {"icon_background": "#1d2b53"}},
        "ios": {"platform": "ios", "ios": {"icon_background": "#1d2b53"}}}})");

    const slash::runtime::Project project = slash::runtime::load_project("game").value();
    const exporter::Presets presets = exporter::load_presets("game").value();
    for (const std::string_view platform : exporter::kPlatforms) {
        const auto found = presets.find(platform);
        exporter::Preset preset;
        preset.platform = platform;
        if (found != presets.end()) {
            preset = found->second;
        }
        const exporter::PresetIcons icons = exporter::preset_icons("game", project, preset).value();
        const auto files = exporter::platform_icons(platform, icons.settings).value();
        std::printf("--- %s, from %s\n", preset.platform.c_str(), icons.source.c_str());
        if (files.empty()) {
            std::puts("no files: the game's window shows the icon");
        }
        for (const exporter::IconFile& file : files) {
            std::printf("%s", file.path.c_str());
            if (file.path.ends_with(".png")) {
                const render::Image image = render::decode_image(file.bytes).value();
                std::printf(" %s, corner %s, middle %s", size_of(file.bytes).c_str(),
                            color(image, 0, 0).c_str(),
                            color(image, image.width / 2, image.height / 2).c_str());
            } else if (file.path.ends_with(".icns")) {
                // Each image's type, then its PNG's size where it has one.
                for (std::size_t at = 8; at < file.bytes.size();) {
                    const std::string type = file.bytes.substr(at, 4);
                    const std::uint32_t length = be32(file.bytes, at + 4);
                    const std::string data = file.bytes.substr(at + 8, length - 8);
                    std::printf(" %s", type.c_str());
                    if (data.starts_with("\x89PNG")) {
                        std::printf(" %s", size_of(data).c_str());
                    }
                    at += length;
                }
            } else if (file.path.ends_with(".ico")) {
                std::printf(" %u images", static_cast<unsigned>(file.bytes[4]));
            } else {
                std::printf(":\n%s", file.bytes.c_str());
            }
            std::puts("");
        }
    }

    // Icons are square: a wide image is refused, by exports and by project validate.
    std::puts("--- a wide icon");
    render::Image wide = coin(64);
    wide.width = 128;
    wide.height = 32;
    write("game/art/wide.png", render::encode_png(wide).value());
    slash::runtime::Project wrong = project;
    wrong.icon = "art/wide.png";
    for (const exporter::IconProblem& problem : exporter::check_icons("game", wrong, {})) {
        std::printf("%s %s: %s\n", problem.file.c_str(), problem.path.c_str(),
                    problem.message.c_str());
    }
    return 0;
}
