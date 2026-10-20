#pragma once

#include "slash/audio/audio.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/services/services.hpp"
#include "slash/world/scene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A game project is a folder with a project.json at its root, like Godot's project.godot:
//
//   {
//     "format": 1,
//     "main_scene": "scenes/main.scene.json",
//     "name": "Pong"
//   }
//
// Optional fields: "id" (reverse-DNS identifier for bundles and stores), "version"
// ("major.minor.patch"), "window" ({"width", "height", "vsync_mode"}), "tick_hz", "max_fps",
// "rendering" ({"clear_color", "driver", "texture_filter", "interpolation"}), "physics_2d",
// "gui" ({"theme"}), "input" (actions by name), "input_devices" ({"pointing"}), "audio"
// ({"bus_layout", "mix_rate", "output_latency", "panning_strength_2d"}), "services" (ad
// placements, products, analytics and consent), "gameplay" ({"sources"}, the game's own C++
// code) and "sdks" (folders of SDK packages).
namespace slash::runtime {

inline constexpr std::string_view kProjectFile = "project.json";
inline constexpr std::int64_t kProjectFormat = 1;

struct Project {
    std::string name;
    std::string id; // empty means com.example.<binary name, lowercased>
    std::string version = "0.1.0";
    std::string main_scene;
    int window_width = 1280;
    int window_height = 720;
    // "disabled", "enabled", "adaptive" or "mailbox", as in Godot.
    std::string vsync_mode = "enabled";
    double tick_hz = 60.0;
    // Frames per second at most; 0 means no cap.
    int max_fps = 0;
    // What the window shows behind everything, as RGBA from 0 to 1: Godot's default gray.
    std::array<float, 4> clear_color{0.3F, 0.3F, 0.3F, 1.0F};
    // How sprites that inherit their filter are sampled: "linear" or "nearest".
    std::string texture_filter = "linear";
    // Draw sprites between fixed ticks, interpolated.
    bool interpolation = true;
    // The GPU backend: "auto", "vulkan", "metal", "d3d12" or "null" (draws nothing).
    std::string rendering_driver = "auto";
    // Gravity and default damping for 2D physics.
    physics2d::Settings physics_2d;
    // A .theme.json file every control uses after its own themes, as Godot's gui/theme/custom;
    // empty for none.
    std::string gui_theme;
    // Seconds the mouse rests on a control before its tooltip shows.
    double tooltip_delay_sec = 0.5;
    // Where a tooltip shows from the pointer, in pixels.
    Vec2 tooltip_position_offset{10.0F, 10.0F};
    // Pixels the mouse moves with the left button held before a drag starts.
    int drag_threshold = 10;
    // Text may use fonts installed on the machine for characters the project's fonts lack.
    bool allow_system_fallback = true;
    // Input actions and their bindings: Godot's ui_* actions, then the project's.
    input::Map input = input::Map::with_defaults();
    // How touches and the mouse stand in for each other.
    input::Settings input_devices;
    // The bus layout, mix rate, output latency and 2D panning strength.
    audio::Settings audio;
    // Ad placements, products, analytics and the player's consent until the game sets it.
    services::Settings services;
    // Folders and .cpp files of the game's own C++ code (slash/world/game_module.hpp); empty if
    // it has none.
    std::vector<std::string> gameplay_sources;
    // Folders of SDK packages (tools/exporter's slash/exporter/sdk.hpp), which exports link.
    std::vector<std::string> sdks;

    // The identifier to use: id, or the default derived from the name.
    [[nodiscard]] std::string bundle_id() const;
    // The name with only ASCII letters and digits kept, for executables and bundles, e.g.
    // "Space Pong 2" -> "SpacePong2". "Game" if nothing is left.
    [[nodiscard]] std::string binary_name() const;
};

// The JSON Schema for project.json.
[[nodiscard]] json::Value project_schema();

// Checks a parsed project.json against the schema and the rules it can't express (a relative
// main scene path, the id and version formats). Returns every error.
[[nodiscard]] std::vector<json::ValidationError> validate_project(const json::Value& project);

// Reads a validated project. The error names the first problem with its JSON Pointer.
[[nodiscard]] Result<Project> parse_project(const json::Value& project);

// Reads and parses <folder>/project.json.
[[nodiscard]] Result<Project> load_project(std::string_view folder);

// A new project with this name and main scene, written in canonical form.
[[nodiscard]] json::Value new_project(std::string_view name, std::string_view main_scene);

// Whether path names a file inside a project (see slash/core/text.hpp).
using slash::is_project_path;

// Joins a folder and a project-relative path with '/'.
[[nodiscard]] std::string join_path(std::string_view folder, std::string_view relative);

// Reads files by project path (is_project_path) from a folder, for World::set_file_reader.
[[nodiscard]] world::FileReader folder_reader(std::string folder);

} // namespace slash::runtime
