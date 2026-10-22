#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/forms.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace exporter = slash::exporter;
namespace render = slash::render;
using render::TextureFormat;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// A picture with smooth color and alpha, as sprites have.
render::Image gradient(std::uint32_t width, std::uint32_t height) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            image.pixels.push_back(static_cast<std::uint8_t>(x * 255 / width));
            image.pixels.push_back(static_cast<std::uint8_t>(y * 255 / height));
            image.pixels.push_back(static_cast<std::uint8_t>(255 - (x * 4) - (y * 2)));
            image.pixels.push_back(static_cast<std::uint8_t>(128 + (x * 2) + y));
        }
    }
    return image;
}

std::string png_of(const render::Image& image) {
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    return std::move(png).value();
}

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        exporter::register_export_commands(commands);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }
};

} // namespace

TEST_CASE("project.json's rendering.textures is read and checked, the longest path winning") {
    json::Value project = parse(R"({"format": 1, "name": "Forms", "main_scene": "main.scene.json",
        "rendering": {"textures": {"compress_mode": "vram_compressed",
            "paths": {"ui/": "lossless", "ui/atlas/": "vram_uncompressed",
                      "ui/logo.png": "vram_compressed"}}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    const slash::runtime::TextureImport textures =
        slash::runtime::parse_project(project).value().texture_import;
    CHECK(textures.mode_for("sprites/hero.png") == "vram_compressed");
    CHECK(textures.mode_for("ui/button.png") == "lossless");
    CHECK(textures.mode_for("ui/atlas/icons.png") == "vram_uncompressed");
    CHECK(textures.mode_for("ui/logo.png") == "vram_compressed");
    CHECK(textures.mode_for("uix/button.png") == "vram_compressed"); // folders end in '/'
    // Lossless unless a project says otherwise, as Godot imports 2D textures.
    json::Value plain = parse(R"({"format": 1, "name": "Plain", "main_scene": "m.scene.json"})");
    CHECK(slash::runtime::parse_project(plain).value().texture_import.mode_for("a.png") ==
          "lossless");

    project["rendering"]["textures"]["paths"] = parse(R"({"../shared/": "lossless"})");
    auto errors = slash::runtime::validate_project(project);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/rendering/textures/paths/..~1shared~1");
    CHECK(contains(errors[0].message, "inside the project"));
    project["rendering"]["textures"] = parse(R"({"compress_mode": "lossy"})");
    errors = slash::runtime::validate_project(project);
    REQUIRE_FALSE(errors.empty());
    CHECK(errors[0].path == "/rendering/textures/compress_mode");
}

TEST_CASE("each platform ships the compressed formats its GPUs sample") {
    using V = std::vector<TextureFormat>;
    CHECK(exporter::shipped_formats("vram_compressed", "macos") == V{TextureFormat::Bc7});
    CHECK(exporter::shipped_formats("vram_compressed", "windows") == V{TextureFormat::Bc7});
    CHECK(exporter::shipped_formats("vram_compressed", "linux") == V{TextureFormat::Bc7});
    CHECK(exporter::shipped_formats("vram_compressed", "android") == V{TextureFormat::Astc4x4});
    CHECK(exporter::shipped_formats("vram_compressed", "ios") == V{TextureFormat::Astc4x4});
    CHECK(exporter::shipped_formats("vram_compressed", "web") ==
          V{TextureFormat::Bc7, TextureFormat::Astc4x4});
    CHECK(exporter::shipped_formats("vram_uncompressed", "ios") == V{TextureFormat::Rgba8});
    CHECK(exporter::shipped_formats("lossless", "web").empty());
    CHECK(render::texture_form_path("ui/icon.png", TextureFormat::Astc4x4) ==
          "ui/icon.png.astc_4x4.ktx2");
}

TEST_CASE("texture forms are made once and then read from the library's cache") {
    const std::filesystem::path library = "texture_forms_library";
    std::filesystem::remove_all(library);
    const render::Image image = gradient(16, 8);
    const std::string png = png_of(image);

    for (const TextureFormat format :
         {TextureFormat::Bc7, TextureFormat::Astc4x4, TextureFormat::Rgba8}) {
        INFO(render::texture_format_name(format));
        auto made = exporter::texture_form(png, image, format, library.string());
        REQUIRE(made.ok());
        CHECK_FALSE(made.value().cached);
        auto decoded = render::decode_ktx2(made.value().ktx2);
        REQUIRE(decoded.ok());
        CHECK(decoded.value().width == 16);
        CHECK(decoded.value().height == 8);
        CHECK(decoded.value().block_format == format);

        auto again = exporter::texture_form(png, image, format, library.string());
        REQUIRE(again.ok());
        CHECK(again.value().cached);
        CHECK(again.value().ktx2 == made.value().ktx2);

        // RGBA8 is the texels as they were; the blocks come close.
        const auto psnr = exporter::form_psnr(image, made.value());
        REQUIRE(psnr.ok());
        if (format == TextureFormat::Rgba8) {
            CHECK_FALSE(psnr.value().has_value());
        } else {
            REQUIRE(psnr.value().has_value());
            CHECK(*psnr.value() >= 35);
        }
    }
    // Keyed by the file's bytes and the format, under a folder of the hash's first two digits.
    std::size_t cached = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(library / "forms")) {
        cached += entry.is_regular_file() ? 1U : 0U;
    }
    CHECK(cached == 3);

    // Blocks are 4x4 texels, so sides that aren't multiples of 4 can't take them.
    const render::Image odd = gradient(6, 4);
    auto refused = exporter::texture_form(png_of(odd), odd, TextureFormat::Bc7, library.string());
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message ==
          "its sides, 6x4, aren't multiples of 4, as BC7 and ASTC 4x4 blocks need");
    CHECK(exporter::texture_form(png_of(odd), odd, TextureFormat::Rgba8, "").ok());
    std::filesystem::remove_all(library);
}

TEST_CASE("games read the texture form their device samples, else RGBA8, else any") {
    std::map<std::string, std::string, std::less<>> files{
        {"a.png.bc7.ktx2", "bc7"},       {"a.png.astc_4x4.ktx2", "astc"},
        {"b.png.astc_4x4.ktx2", "astc"}, {"c.png.rgba8.ktx2", "rgba8"},
        {"c.png.bc7.ktx2", "bc7"},       {"d.png", "png"},
        {"d.png.bc7.ktx2", "bc7"},       {"notes.txt", "text"}};
    const slash::asset::FileSource source{
        .read = [&](std::string_view path) -> slash::Result<std::string> {
            if (auto found = files.find(path); found != files.end()) {
                return found->second;
            }
            return slash::make_error(slash::ErrorCode::NotFound, std::string(path));
        },
        .stamp = [&](std::string_view path) -> std::optional<slash::asset::FileStamp> {
            if (files.contains(path)) {
                return slash::asset::FileStamp{};
            }
            return std::nullopt;
        },
    };
    std::optional<TextureFormat> sampled;
    const slash::asset::FileSource forms = slash::canvas::with_texture_forms(
        source, [&](TextureFormat format) { return sampled == format; });
    const auto read = [&](std::string_view path) {
        auto text = forms.read(path);
        return text ? text.value() : "(" + text.error().message + ")";
    };

    sampled = TextureFormat::Astc4x4;
    CHECK(read("a.png") == "astc");
    CHECK(read("c.png") == "rgba8");
    sampled = TextureFormat::Bc7;
    CHECK(read("a.png") == "bc7");
    CHECK(read("b.png") == "astc"); // decoded when it loads
    CHECK(read("c.png") == "bc7");
    sampled.reset();
    CHECK(read("a.png") == "bc7");
    CHECK(read("c.png") == "rgba8");
    // A file that is there as it was is read as it is, and other files are left alone.
    CHECK(read("d.png") == "png");
    CHECK(read("notes.txt") == "text");
    CHECK(read("e.png") == "(e.png)");
    CHECK(forms.stamp("b.png").has_value());
    CHECK_FALSE(forms.stamp("e.png").has_value());
}

TEST_CASE("exports ship textures in the forms project.json asks for, in place of their files") {
    const std::filesystem::path folder = "texture_forms_project";
    const std::filesystem::path library = "texture_forms_export_library";
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(library);
    std::filesystem::create_directories(folder / "ui");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Forms", "main_scene": "main.scene.json",
        "rendering": {"textures": {"compress_mode": "vram_compressed",
                                   "paths": {"ui/": "lossless",
                                             "ui/atlas.png": "vram_uncompressed"}}}})");
    // A scene the export refuses, so it stops once the data is staged.
    write("main.scene.json", R"({"format": 1, "entities": {"a": {"components": {"Sprite2D":
        {"texture": 7}}}}})");
    write("hero.png", png_of(gradient(16, 16)));
    write("odd.png", png_of(gradient(6, 6)));
    write("ui/button.png", png_of(gradient(8, 8)));
    write("ui/atlas.png", png_of(gradient(8, 8)));

    // Desktops ship BC7. (A web export, which ships ASTC too, needs Emscripten before it stages.)
#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    options.library = library.string();
    const exporter::ExportResult result = exporter::export_game(options);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.json.find("error") != nullptr);
    INFO(json::write(result.json));
    CHECK(result.json.find("error")->find("step")->as_string() == "validate");

    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK_FALSE(std::filesystem::exists(data / "hero.png"));
    CHECK(std::filesystem::exists(data / "hero.png.bc7.ktx2"));
    CHECK_FALSE(std::filesystem::exists(data / "hero.png.astc_4x4.ktx2"));
    // Sides that blocks don't fit ship as they are.
    CHECK(std::filesystem::exists(data / "odd.png"));
    CHECK_FALSE(std::filesystem::exists(data / "odd.png.bc7.ktx2"));
    CHECK(std::filesystem::exists(data / "ui" / "button.png"));
    CHECK_FALSE(std::filesystem::exists(data / "ui" / "button.png.bc7.ktx2"));
    CHECK(std::filesystem::exists(data / "ui" / "atlas.png.rgba8.ktx2"));
    CHECK_FALSE(std::filesystem::exists(data / "ui" / "atlas.png"));
    // The forms were kept in the library for the next export.
    CHECK(std::filesystem::exists(library / "forms"));
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(library);
}

TEST_CASE("texture.forms makes each texture's forms and reports their sizes and quality") {
    const std::filesystem::path folder = "texture_forms_command";
    const std::filesystem::path library = "texture_forms_command_library";
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(library);
    std::filesystem::create_directories(folder / "ui");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Forms", "main_scene": "main.scene.json",
        "rendering": {"textures": {"compress_mode": "vram_compressed",
                                   "paths": {"ui/": "lossless"}}}})");
    write("hero.png", png_of(gradient(16, 16)));
    write("odd.png", png_of(gradient(6, 6)));
    write("ui/button.png", png_of(gradient(8, 8)));
    write("broken.png", "not an image");

    command::SessionOptions options;
    options.project_root = folder.string();
    options.library_dir = library.string();
    Engine engine(options);
    const command::Command* forms = engine.commands.find("texture.forms");
    REQUIRE(forms != nullptr);
    CHECK(forms->capabilities == std::vector<command::Capability>{command::Capability::Read,
                                                                  command::Capability::Filesystem});

    command::Reply reply =
        engine.session->call("texture.forms", parse(R"({"platforms": ["linux", "ios"]})"));
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("made")->as_int() == 2);
    const json::Array& textures = reply.value.find("textures")->as_array();
    REQUIRE(textures.size() == 4);
    CHECK(textures[0].find("path")->as_string() == "broken.png");
    CHECK(contains(textures[0].find("error")->as_string(), "not a PNG, BMP, JPEG or KTX2 image"));
    const json::Value& hero = textures[1];
    CHECK(hero.find("path")->as_string() == "hero.png");
    CHECK(hero.find("mode")->as_string() == "vram_compressed");
    CHECK(hero.find("size")->as_array() == json::Array{std::int64_t{16}, std::int64_t{16}});
    const json::Array& hero_forms = hero.find("forms")->as_array();
    REQUIRE(hero_forms.size() == 2);
    CHECK(hero_forms[0].find("format")->as_string() == "bc7");
    CHECK(hero_forms[0].find("gpu_bytes")->as_int() == 256); // a byte a texel
    CHECK(hero_forms[0].find("psnr")->as_int() >= 35);
    CHECK_FALSE(hero_forms[0].find("cached")->as_bool());
    CHECK(hero_forms[1].find("format")->as_string() == "astc_4x4");
    CHECK(textures[2].find("path")->as_string() == "odd.png");
    CHECK(textures[2].find("forms")->as_array().empty());
    CHECK(contains(textures[2].find("note")->as_string(), "aren't multiples of 4"));
    CHECK(textures[3].find("mode")->as_string() == "lossless");
    CHECK(textures[3].find("forms")->as_array().empty());
    // Linux ships hero.png in BC7 and the rest as they are, decoded to 4 bytes a texel.
    const json::Value& linux_total = *reply.value.find("platforms")->find("linux");
    CHECK(linux_total.find("gpu_bytes")->as_int() == 256 + (6 * 6 * 4) + (8 * 8 * 4));
    CHECK(reply.value.find("platforms")->find("macos") == nullptr);

    // Made once: the next call reads them from the library.
    reply = engine.session->call("texture.forms", parse(R"({"paths": ["hero.png"]})"));
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("made")->as_int() == 0);
    REQUIRE(reply.value.find("textures")->as_array().size() == 1);
    CHECK(reply.value.find("textures")
              ->as_array()[0]
              .find("forms")
              ->as_array()[0]
              .find("cached")
              ->as_bool());
    CHECK(reply.value.find("platforms")->as_object().size() == 6);

    reply = engine.session->call("texture.forms", parse(R"({"paths": ["ui/", "../x.png"]})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidArgument);
    CHECK(reply.data.find("path")->as_string() == "/paths/1");
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(library);
}
