#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

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
// ("major.minor.patch"), "window" ({"width", "height"}) and "tick_hz".
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
    double tick_hz = 60.0;

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

// Joins a folder and a project-relative path with '/'.
[[nodiscard]] std::string join_path(std::string_view folder, std::string_view relative);

} // namespace slash::runtime
