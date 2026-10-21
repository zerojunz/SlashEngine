#pragma once

#include "slash/core/result.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/render/device.hpp"
#include "slash/runtime/project.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// App icons for every platform, made from one square image (project.json's "icon", or the
// engine's own), as Godot's exports make them from application/config/icon: an .icns file in
// macOS bundles, an .ico resource in Windows executables, an adaptive icon in Android APKs, an
// asset catalog in iOS apps and a favicon on the web. Linux games show the icon in their window.
namespace slash::exporter {

using Rgb = std::array<std::uint8_t, 3>;

struct IconSettings {
    render::Image icon; // square
    // Android's adaptive icon: its foreground layer, full bleed (108 dp, 432 pixels at the
    // largest density), and its background, an image or a color. With no foreground, the icon
    // fills the middle 72 dp, which launchers show through their mask.
    std::optional<render::Image> android_foreground;
    std::optional<render::Image> android_background_image;
    Rgb android_background{255, 255, 255};
    // What shows through transparent pixels where icons are opaque: iOS, and the web's Apple
    // touch icon.
    Rgb opaque_background{255, 255, 255};
};

struct IconFile {
    std::string path; // relative to the folder the platform's icons are written to
    std::string bytes;
};

// The platform's icon files: AppIcon.icns on macOS, icon.ico on Windows, favicon.png and
// apple-touch-icon.png on the web, res/ for Android's APK, Assets.xcassets/ for iOS's actool;
// none on Linux.
[[nodiscard]] Result<std::vector<IconFile>> platform_icons(std::string_view platform,
                                                           const IconSettings& settings);

// An .icns file of the icon at every size macOS asks for, as Godot's macOS export writes one:
// PNG images from 1024 to 32 pixels, and 32 and 16 pixel ones run-length encoded with masks.
[[nodiscard]] Result<std::string> encode_icns(const render::Image& icon);
// An .ico file of PNG images from 16 to 256 pixels, as Godot's Windows export writes one.
[[nodiscard]] Result<std::string> encode_ico(const render::Image& icon);
// The icon as macOS shows apps' icons: an opaque one drawn in Apple's rounded square, 824 of
// 1024 pixels with corners rounded 185 pixels, with clear margins; one with transparent pixels,
// which has its own shape, as it is. 1024 pixels on a side.
[[nodiscard]] render::Image macos_icon(const render::Image& icon);
// The icon over a color, with no transparent pixels left.
[[nodiscard]] render::Image flatten_icon(const render::Image& icon, Rgb background);

// A color written "#rrggbb"; nullopt if the text isn't one.
[[nodiscard]] std::optional<Rgb> parse_color(std::string_view text);

struct IconProblem {
    std::string file; // project.json or export_presets.json
    std::string path; // JSON Pointer of the setting
    std::string message;
};

// What is wrong with the icon files project.json and the presets name: missing, not images or
// not square.
[[nodiscard]] std::vector<IconProblem> check_icons(const std::filesystem::path& project_dir,
                                                   const runtime::Project& project,
                                                   const std::vector<const Preset*>& presets);

// The images and colors a preset's icons are made from, read from the project. source is the
// icon's project path, or "" for the engine's icon.
struct PresetIcons {
    IconSettings settings;
    std::string source;
};
[[nodiscard]] Result<PresetIcons> preset_icons(const std::filesystem::path& project_dir,
                                               const runtime::Project& project,
                                               const Preset& preset);

} // namespace slash::exporter
