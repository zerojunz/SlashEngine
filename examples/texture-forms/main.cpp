// Textures in the forms each platform's GPUs sample, made once and cached.
//
// project.json's rendering.textures says how exports ship textures, as Godot's compress/mode:
// as their files ("lossless"), as BC7 or ASTC 4x4 blocks ("vram_compressed") or as RGBA8 texels
// ("vram_uncompressed"). texture.forms makes each texture's forms, keeps them in the asset
// library's cache, where exports find them, and reports their sizes and how close their colors
// stay. An exported game then reads, for each texture, the form its GPU samples.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/forms.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;

// Prints the call as an agent would make it, then the result or the error.
command::Reply call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    command::Reply reply = session.call(name, value);
    if (reply.ok) {
        std::puts(json::write(reply.value, json::Style::Compact).c_str());
    } else {
        std::printf("error: %s\n", reply.message.c_str());
    }
    return reply;
}

void write(const std::string& path, std::string_view text) {
    if (!slash::platform::write_file(path, text)) {
        std::printf("could not write %s\n", path.c_str());
        std::exit(1);
    }
}

// A sprite drawn in code: a soft round blob in two colors, with smooth edges in its alpha.
render::Image blob(std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    const std::int64_t center = size / 2;
    const std::int64_t radius = center * center;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::int64_t dx = std::int64_t{x} - center;
            const std::int64_t dy = std::int64_t{y} - center;
            const std::int64_t distance = (dx * dx) + (dy * dy);
            const std::int64_t alpha = distance >= radius ? 0 : 255 - (255 * distance / radius);
            image.pixels.push_back(static_cast<std::uint8_t>(64 + (x * 191 / size)));
            image.pixels.push_back(static_cast<std::uint8_t>(160 - (y * 96 / size)));
            image.pixels.push_back(static_cast<std::uint8_t>(96 + (y * 128 / size)));
            image.pixels.push_back(static_cast<std::uint8_t>(alpha));
        }
    }
    return image;
}

std::string png(const render::Image& image) {
    return render::encode_png(image).value();
}

} // namespace

int main() {
    // A project with a sprite, a UI icon that stays exact, and a texture blocks don't fit.
    (void)slash::platform::create_directories("game/sprites");
    (void)slash::platform::create_directories("game/ui");
    write("game/project.json", R"({"format": 1, "name": "Forms", "main_scene": "main.scene.json",
        "rendering": {"textures": {"compress_mode": "vram_compressed",
                                   "paths": {"ui/": "lossless"}}}})");
    write("game/main.scene.json", R"({"format": 1, "entities": {"hero": {"components": {
        "Transform2D": {}, "Sprite2D": {"texture": "sprites/hero.png"}}}}})");
    write("game/sprites/hero.png", png(blob(32)));
    write("game/sprites/spark.png", png(blob(6)));
    write("game/ui/icon.png", png(blob(16)));

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::exporter::register_export_commands(commands);
    command::SessionOptions options;
    options.project_root = "game";
    options.library_dir = "library"; // where the cache is, as SLASH_LIBRARY_DIR says
    options.capture_logs = false;
    command::Session session(components, commands, std::move(options));

    std::puts("--- the forms a desktop, a phone and the web ship, made and cached");
    call(session, "texture.forms", R"({"platforms": ["linux", "android", "web"]})");

    std::puts("--- the next time, read from the cache");
    call(session, "texture.forms", R"({"paths": ["sprites/hero.png"], "platforms": ["web"]})");

    // What a web export ships in place of hero.png: one form for each format a browser's GPU
    // may sample, beside where the file was. Scenes still name sprites/hero.png.
    std::puts("--- a game reads the form its GPU samples");
    (void)slash::platform::create_directories("data/sprites");
    const std::string hero = slash::platform::read_file("game/sprites/hero.png").value();
    const render::Image image = render::decode_image(hero).value();
    for (const render::TextureFormat format : slash::exporter::platform_formats("web")) {
        const auto form = slash::exporter::texture_form(hero, image, format, "library").value();
        write("data/" + render::texture_form_path("sprites/hero.png", format), form.ktx2);
    }
    for (const auto& [gpu, samples] :
         {std::pair{"a desktop GPU", render::TextureFormat::Bc7},
          std::pair{"a phone GPU", render::TextureFormat::Astc4x4},
          std::pair{"a GPU with neither", render::TextureFormat::Rgba8}}) {
        const slash::asset::FileSource files = slash::canvas::with_texture_forms(
            slash::asset::folder_source("data"),
            [samples](render::TextureFormat format) { return format == samples; });
        const render::Image read =
            render::decode_image(files.read("sprites/hero.png").value()).value();
        std::printf("%s reads sprites/hero.png as %s blocks\n", gpu,
                    std::string(render::texture_format_name(read.block_format)).c_str());
    }
    return 0;
}
