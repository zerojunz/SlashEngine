#include "slash/core/json.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/icons.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/icon.hpp"
#include "slash/runtime/project.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace exporter = slash::exporter;
namespace render = slash::render;
namespace runtime = slash::runtime;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

render::Image solid(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g,
                    std::uint8_t b, std::uint8_t a) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.pixels.insert(image.pixels.end(), {r, g, b, a});
    }
    return image;
}

const std::uint8_t* pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    return &image.pixels[((static_cast<std::size_t>(y) * image.width) + x) * 4];
}

std::string png_of(const render::Image& image) {
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    return std::move(png).value();
}

render::Image decoded(std::string_view bytes) {
    auto image = render::decode_image(bytes);
    REQUIRE(image.ok());
    return std::move(image).value();
}

std::uint32_t be32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<std::uint8_t>(bytes[at + i]);
    }
    return value;
}

std::uint32_t le16(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint8_t>(bytes[at]) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 8U);
}

std::uint32_t le32(std::string_view bytes, std::size_t at) {
    return le16(bytes, at) | (le16(bytes, at + 2) << 16U);
}

// Undoes Apple's run-length encoding of count bytes from at, returning where it stopped.
std::size_t unpack_bits(std::string_view bytes, std::size_t at, std::size_t count,
                        std::vector<std::uint8_t>& out) {
    const std::size_t end = out.size() + count;
    while (out.size() < end) {
        const auto control = static_cast<std::uint8_t>(bytes[at++]);
        if (control < 0x80) {
            for (std::size_t i = 0; i <= control; ++i) {
                out.push_back(static_cast<std::uint8_t>(bytes[at++]));
            }
        } else {
            out.insert(out.end(), control - 0x80U + 3U, static_cast<std::uint8_t>(bytes[at++]));
        }
    }
    return at;
}

// A picture with detail everywhere: stripes of two colors on a transparent-edged disc.
render::Image picture(std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool stripe = ((x / 8) % 2) == 0;
            image.pixels.push_back(stripe ? 230 : 20);
            image.pixels.push_back(static_cast<std::uint8_t>(y * 255 / size));
            image.pixels.push_back(stripe ? 40 : 200);
            image.pixels.push_back(255);
        }
    }
    return image;
}

std::map<std::string, std::string> by_path(const std::vector<exporter::IconFile>& files) {
    std::map<std::string, std::string> out;
    for (const exporter::IconFile& file : files) {
        out[file.path] = file.bytes;
    }
    return out;
}

} // namespace

TEST_CASE("images resize with a Lanczos filter that keeps flat colors and transparent edges") {
    const render::Image flat = solid(40, 40, 10, 120, 240, 255);
    for (const std::uint32_t size : {3U, 16U, 40U, 97U}) {
        const render::Image resized = render::resize_image(flat, size, size);
        REQUIRE(resized.width == size);
        REQUIRE(resized.pixels.size() == static_cast<std::size_t>(size) * size * 4);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::uint8_t* p = pixel(resized, x, y);
                CHECK(p[0] == 10);
                CHECK(p[1] == 120);
                CHECK(p[2] == 240);
                CHECK(p[3] == 255);
            }
        }
    }
    // Transparent black around a red square: colors weighted by alpha stay red at the edge,
    // where a filter on straight colors would darken them.
    render::Image square = solid(32, 32, 0, 0, 0, 0);
    for (std::uint32_t y = 8; y < 24; ++y) {
        for (std::uint32_t x = 8; x < 24; ++x) {
            std::uint8_t* p = &square.pixels[((static_cast<std::size_t>(y) * 32) + x) * 4];
            p[0] = 255;
            p[3] = 255;
        }
    }
    const render::Image small = render::resize_image(square, 8, 8);
    const std::uint8_t* edge = pixel(small, 1, 3);
    CHECK(edge[3] > 0);
    CHECK(edge[3] < 255);
    CHECK(edge[0] == 255);
    CHECK(edge[1] == 0);
    // Shrinking stripes averages them.
    const render::Image stripes = render::resize_image(picture(64), 4, 4);
    CHECK(pixel(stripes, 1, 1)[0] > 110);
    CHECK(pixel(stripes, 1, 1)[0] < 140);
}

TEST_CASE("the engine's icon is an opaque dark square with a white slash, and icons are square") {
    const render::Image icon = runtime::default_icon(64);
    REQUIRE(icon.width == 64);
    REQUIRE(icon.height == 64);
    for (std::size_t i = 3; i < icon.pixels.size(); i += 4) {
        REQUIRE(icon.pixels[i] == 255);
    }
    CHECK(pixel(icon, 32, 32)[0] == 255); // the slash's middle
    CHECK(pixel(icon, 32, 32)[2] == 255);
    CHECK(pixel(icon, 2, 2)[0] < 64); // the dark corner
    CHECK(pixel(icon, 2, 2)[2] > pixel(icon, 2, 2)[0]);
    CHECK(runtime::default_icon(64).pixels == icon.pixels);

    CHECK(runtime::decode_icon(png_of(solid(8, 8, 1, 2, 3, 255))).ok());
    const auto oblong = runtime::decode_icon(png_of(solid(8, 4, 1, 2, 3, 255)));
    REQUIRE_FALSE(oblong.ok());
    CHECK(oblong.error().message == "icons are square, and this one is 8x4");
    CHECK_FALSE(runtime::decode_icon("not an image").ok());

    // Windows show at most 256 pixels; with no icon, or a broken one, the engine's.
    const render::Image big =
        runtime::window_icon("icon.png", [](std::string_view) -> slash::Result<std::string> {
            return png_of(solid(512, 512, 9, 9, 9, 255));
        });
    CHECK(big.width == runtime::kWindowIconSize);
    CHECK(pixel(big, 100, 100)[0] == 9);
    const render::Image none = runtime::window_icon("", nullptr);
    CHECK(none.pixels == runtime::default_icon(runtime::kWindowIconSize).pixels);
    const render::Image broken =
        runtime::window_icon("icon.png", [](std::string_view) -> slash::Result<std::string> {
            return std::string("junk");
        });
    CHECK(broken.pixels == none.pixels);
}

TEST_CASE("project.json and presets name icons, checked as project paths and colors") {
    auto project = runtime::parse_project(parse(
        R"({"format": 1, "name": "Icons", "main_scene": "main.scene.json", "icon": "art/icon.png"})"));
    REQUIRE(project.ok());
    CHECK(project.value().icon == "art/icon.png");
    const auto errors = runtime::validate_project(
        parse(R"({"format": 1, "name": "Icons", "main_scene": "main.scene.json",
                  "icon": "../icon.png"})"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/icon");

    auto presets = exporter::parse_presets(parse(R"({"format": 1, "presets": {
        "android": {"platform": "android", "icon": "art/phone.png",
                    "android": {"icon_foreground": "art/fg.png", "icon_background": "#1D2B53"}},
        "ios": {"platform": "ios", "ios": {"icon_background": "#000000"}}}})"));
    REQUIRE(presets.ok());
    const exporter::Preset& android = presets.value().at("android");
    CHECK(android.icon == "art/phone.png");
    CHECK(android.android.icon_foreground == "art/fg.png");
    CHECK(android.android.icon_background == "#1D2B53");
    CHECK(presets.value().at("ios").ios.icon_background == "#000000");
    CHECK(presets.value().at("ios").android.icon_background == "#ffffff");

    const auto bad = exporter::validate_presets(parse(R"({"format": 1, "presets": {
        "android": {"platform": "android", "icon": "/icon.png",
                    "android": {"icon_background": "#12345"}},
        "ios": {"platform": "ios", "ios": {"icon_background": "white"}}}})"));
    std::vector<std::string> paths;
    paths.reserve(bad.size());
    for (const auto& error : bad) {
        paths.push_back(error.path);
    }
    CHECK(paths == std::vector<std::string>{"/presets/android/icon",
                                            "/presets/android/android/icon_background",
                                            "/presets/ios/ios/icon_background"});

    CHECK(exporter::parse_color("#ff8000") == exporter::Rgb{255, 128, 0});
    CHECK(exporter::parse_color("#FF8000") == exporter::Rgb{255, 128, 0});
    CHECK_FALSE(exporter::parse_color("ff8000").has_value());
    CHECK_FALSE(exporter::parse_color("#ff80zz").has_value());
}

TEST_CASE("an .icns file holds the icon at every size macOS asks for, as Godot writes it") {
    const render::Image icon = picture(256);
    auto icns = exporter::encode_icns(icon);
    REQUIRE(icns.ok());
    const std::string& file = icns.value();
    CHECK(file.substr(0, 4) == "icns");
    CHECK(be32(file, 4) == file.size());
    std::vector<std::string> types;
    std::map<std::string, std::string> images;
    for (std::size_t at = 8; at < file.size();) {
        const std::string type = file.substr(at, 4);
        const std::uint32_t length = be32(file, at + 4);
        REQUIRE(length >= 8);
        types.push_back(type);
        images[type] = file.substr(at + 8, length - 8);
        at += length;
    }
    CHECK(types == std::vector<std::string>{"ic10", "ic09", "ic14", "ic08", "ic13", "ic07", "ic12",
                                            "ic11", "il32", "l8mk", "is32", "s8mk"});
    CHECK(decoded(images["ic10"]).width == 1024);
    CHECK(decoded(images["ic14"]).width == 512);
    CHECK(decoded(images["ic11"]).width == 32);
    // The smallest two: each channel run-length encoded, the alpha as it is.
    const render::Image sixteen = render::resize_image(icon, 16, 16);
    std::vector<std::uint8_t> channels;
    const std::size_t used = unpack_bits(images["is32"], 0, std::size_t{16} * 16 * 3, channels);
    CHECK(used == images["is32"].size());
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < 256; ++i) {
            REQUIRE(channels[(c * 256) + i] == sixteen.pixels[(i * 4) + c]);
        }
    }
    REQUIRE(images["s8mk"].size() == 256);
    CHECK(static_cast<std::uint8_t>(images["s8mk"][0]) == 255);
    channels.clear();
    CHECK(unpack_bits(images["il32"], 0, std::size_t{32} * 32 * 3, channels) ==
          images["il32"].size());
    CHECK(images["l8mk"].size() == 1024);
}

TEST_CASE("an .ico file holds PNG images from 16 to 256 pixels") {
    auto ico = exporter::encode_ico(picture(128));
    REQUIRE(ico.ok());
    const std::string& file = ico.value();
    CHECK(le16(file, 0) == 0);
    CHECK(le16(file, 2) == 1);
    REQUIRE(le16(file, 4) == 6);
    std::vector<std::uint32_t> sizes;
    for (std::size_t i = 0; i < 6; ++i) {
        const std::size_t entry = 6 + (i * 16);
        const auto width = static_cast<std::uint8_t>(file[entry]);
        CHECK(le16(file, entry + 4) == 1);
        CHECK(le16(file, entry + 6) == 32);
        const std::string png = file.substr(le32(file, entry + 12), le32(file, entry + 8));
        const render::Image image = decoded(png);
        CHECK(image.width == (width == 0 ? 256U : width));
        sizes.push_back(image.width);
    }
    CHECK(sizes == std::vector<std::uint32_t>{16, 32, 48, 64, 128, 256});
}

TEST_CASE("macOS gets an opaque icon in Apple's rounded square, and iOS no transparency") {
    const render::Image square = runtime::default_icon(512);
    const render::Image mac = exporter::macos_icon(square);
    REQUIRE(mac.width == 1024);
    CHECK(pixel(mac, 50, 512)[3] == 0);    // the margin
    CHECK(pixel(mac, 110, 110)[3] == 0);   // past the rounded corner
    CHECK(pixel(mac, 512, 150)[3] == 255); // inside, by the top edge
    CHECK(pixel(mac, 512, 512)[3] == 255);
    // An icon with its own shape is left as it is.
    render::Image round = solid(64, 64, 0, 0, 0, 0);
    round.pixels[((32 * 64) + 32) * 4 + 3] = 255;
    const render::Image kept = exporter::macos_icon(round);
    CHECK(kept.width == 1024);
    CHECK(pixel(kept, 0, 0)[3] == 0);

    const render::Image flat =
        exporter::flatten_icon(solid(2, 2, 255, 0, 0, 128), exporter::Rgb{0, 0, 255});
    CHECK(pixel(flat, 0, 0)[0] == 128);
    CHECK(pixel(flat, 0, 0)[2] == 127);
    CHECK(pixel(flat, 0, 0)[3] == 255);
}

TEST_CASE("each platform gets its icon files") {
    exporter::IconSettings settings;
    settings.icon = picture(256);
    settings.android_background = exporter::Rgb{0x1d, 0x2b, 0x53};

    auto mac = exporter::platform_icons("macos", settings);
    REQUIRE(mac.ok());
    REQUIRE(mac.value().size() == 1);
    CHECK(mac.value()[0].path == "AppIcon.icns");
    auto windows = exporter::platform_icons("windows", settings);
    REQUIRE(windows.ok());
    CHECK(windows.value()[0].path == "icon.ico");
    auto linux_icons = exporter::platform_icons("linux", settings);
    REQUIRE(linux_icons.ok());
    CHECK(linux_icons.value().empty());

    auto web = exporter::platform_icons("web", settings);
    REQUIRE(web.ok());
    const auto web_files = by_path(web.value());
    CHECK(decoded(web_files.at("favicon.png")).width == 192);
    CHECK(decoded(web_files.at("apple-touch-icon.png")).width == 180);

    auto android = exporter::platform_icons("android", settings);
    REQUIRE(android.ok());
    const auto android_files = by_path(android.value());
    CHECK(android_files.size() == 7);
    CHECK(android_files.at("res/mipmap-anydpi-v26/icon.xml").find("@color/icon_background") !=
          std::string::npos);
    CHECK(android_files.at("res/values/icon_background.xml").find("#1d2b53") != std::string::npos);
    // The icon fills the middle two thirds of the layer, which launchers show.
    const render::Image foreground =
        decoded(android_files.at("res/mipmap-xxxhdpi/icon_foreground.png"));
    CHECK(foreground.width == 432);
    CHECK(pixel(foreground, 60, 216)[3] == 0);
    CHECK(pixel(foreground, 80, 216)[3] == 255);
    CHECK(decoded(android_files.at("res/mipmap-mdpi/icon_foreground.png")).width == 108);
    // A background image instead of a color.
    settings.android_background_image = solid(64, 64, 1, 2, 3, 255);
    settings.android_foreground = solid(64, 64, 4, 5, 6, 255);
    auto layered = exporter::platform_icons("android", settings);
    REQUIRE(layered.ok());
    const auto layered_files = by_path(layered.value());
    CHECK(layered_files.contains("res/mipmap-hdpi/icon_background.png"));
    CHECK_FALSE(layered_files.contains("res/values/icon_background.xml"));
    CHECK(pixel(decoded(layered_files.at("res/mipmap-hdpi/icon_foreground.png")), 0, 0)[2] == 6);

    settings.icon = solid(64, 64, 255, 0, 0, 0);
    settings.opaque_background = exporter::Rgb{0, 255, 0};
    auto ios = exporter::platform_icons("ios", settings);
    REQUIRE(ios.ok());
    const auto ios_files = by_path(ios.value());
    CHECK(ios_files.contains("Assets.xcassets/Contents.json"));
    CHECK(parse(ios_files.at("Assets.xcassets/AppIcon.appiconset/Contents.json"))
              .find("images")
              ->as_array()[0]
              .find("size")
              ->as_string() == "1024x1024");
    const render::Image marketing =
        decoded(ios_files.at("Assets.xcassets/AppIcon.appiconset/icon.png"));
    CHECK(marketing.width == 1024);
    CHECK(pixel(marketing, 5, 5)[1] == 255);
    CHECK(pixel(marketing, 5, 5)[3] == 255);
}

TEST_CASE("exports check the icons they make, and make them from the preset's or the project's") {
    const std::filesystem::path folder = "icons_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "art");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Icons", "main_scene": "main.scene.json",
        "icon": "art/wide.png"})");
    write("main.scene.json", R"({"format": 1, "entities": {}})");
    write("art/wide.png", png_of(solid(16, 8, 1, 2, 3, 255)));
    write("art/icon.png", png_of(solid(16, 16, 200, 100, 50, 255)));
    write("export_presets.json", R"({"format": 1, "presets": {
        "phone": {"platform": "android", "icon": "art/icon.png",
                  "android": {"icon_foreground": "art/missing.png"}}}})");

    auto project = runtime::load_project(folder.string());
    REQUIRE(project.ok());
    auto presets = exporter::load_presets(folder.string());
    REQUIRE(presets.ok());
    const exporter::Preset& phone = presets.value().at("phone");
    const auto problems = exporter::check_icons(folder, project.value(), {&phone});
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].file == "project.json");
    CHECK(problems[0].path == "/icon");
    CHECK(problems[0].message == "\"art/wide.png\" can't be an icon: icons are square, and this "
                                 "one is 16x8");
    CHECK(problems[1].file == "export_presets.json");
    CHECK(problems[1].path == "/presets/phone/android/icon_foreground");
    CHECK(problems[1].message == "no file \"art/missing.png\"");

    // The preset's icon wins over the project's; with neither, the engine's.
    exporter::Preset plain;
    plain.icon = "art/icon.png";
    auto icons = exporter::preset_icons(folder, project.value(), plain);
    REQUIRE(icons.ok());
    CHECK(icons.value().source == "art/icon.png");
    CHECK(pixel(icons.value().settings.icon, 0, 0)[0] == 200);
    runtime::Project no_icon = project.value();
    no_icon.icon.clear();
    auto engine = exporter::preset_icons(folder, no_icon, exporter::Preset{});
    REQUIRE(engine.ok());
    CHECK(engine.value().source.empty());
    CHECK(engine.value().settings.icon.width == runtime::kIconSize);

    // An export stops at validate, naming the icon.
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
    const exporter::ExportResult result = exporter::export_game(options);
    REQUIRE_FALSE(result.ok);
    INFO(json::write(result.json));
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    CHECK(json::write(*error).find("/icon") != std::string::npos);
    std::filesystem::remove_all(folder);
}
