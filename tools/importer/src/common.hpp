#pragma once

#include "slash/core/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the Godot and Unity importers share: paths and names, reports and storyboard tasks.
namespace slash::importer {

// --- Paths and names ---

[[nodiscard]] std::string lowercase(std::string_view text);
// Where a path's extension starts, or its size when it has none.
[[nodiscard]] std::size_t extension_start(std::string_view path);
// ".png" for "a/b.PNG", "" with none.
[[nodiscard]] std::string extension_of(std::string_view path);
[[nodiscard]] std::string without_extension(std::string_view path);
// "b" for "a/b.png".
[[nodiscard]] std::string file_stem(std::string_view path);
// A node or object name as an entity ID: "PlayerSprite" -> "player_sprite", "HUDLabel" ->
// "hud_label", "Sprite2D" -> "sprite2d", "Enemy 3" -> "enemy_3".
[[nodiscard]] std::string snake_case(std::string_view name, std::size_t limit = 64);
// A whole number, all of text, or nullopt.
[[nodiscard]] std::optional<std::int64_t> parse_int(std::string_view text);
// text split at each separator.
[[nodiscard]] std::vector<std::string_view> split_path(std::string_view text, char separator = '/');
[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view separator);

template <typename Range, typename T>
[[nodiscard]] bool contains(const Range& range, const T& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

[[nodiscard]] bool is_texture_file(std::string_view extension);
[[nodiscard]] bool is_sound_file(std::string_view extension);
[[nodiscard]] bool is_font_file(std::string_view extension);
[[nodiscard]] bool is_data_file(std::string_view extension);
// Why a file of another engine's project can't come along, by extension, when no importer takes
// it: other image, font, 3D and video formats. "" for any other extension.
[[nodiscard]] std::string unsupported_asset(std::string_view extension);

// A 3D model file a MeshInstance3D draws: glTF, OBJ, FBX or .blend.
[[nodiscard]] bool is_model_file(std::string_view extension);

// The pixel size of a PNG file, from its IHDR chunk.
[[nodiscard]] std::optional<std::pair<int, int>> png_size(std::string_view bytes);

// Removes the value at a JSON Pointer; false if there is none to remove.
bool remove_pointer(json::Value& root, std::string_view pointer);

// Whether a JSON Pointer is one of those given or inside one: a value already removed, when
// several errors name it.
[[nodiscard]] bool pointer_within(const std::vector<std::string>& pointers,
                                  std::string_view pointer);

// --- 3D math ---

// Rounds an angle or scale made from a matrix, so 90 degrees is 1.570796 rather than
// 1.5707963267948966 or 1.5707962513.
[[nodiscard]] double tidy_number(double value);

// A rotation as Godot's Quaternion keeps it: x, y, z and w.
using Quaternion = std::array<double, 4>;

// The Euler angles of an orthonormal basis, given by rows, as Godot's Basis::get_euler for
// EulerOrder::YXZ.
[[nodiscard]] std::array<double, 3> basis_euler(const std::array<std::array<double, 3>, 3>& m);
// The YXZ Euler angles of a rotation's quaternion, as Godot's Basis(Quaternion) then
// Basis::get_euler find them, tidied.
[[nodiscard]] std::array<double, 3> quaternion_euler(const Quaternion& q);
// The quaternion of YXZ Euler angles, as Godot's Quaternion::from_euler.
[[nodiscard]] Quaternion euler_quaternion(const std::array<double, 3>& euler);

// --- The destination ---

// What the destination project already has that an import builds on.
struct Existing {
    std::optional<std::string> project; // project.json
    std::optional<std::string> work;    // storyboard/work.json
    // The C++ files of its gameplay code, and of the folder C++ skeletons go to, by path relative
    // to it, with their text.
    std::map<std::string, std::string> code;
};

// The folder an import writes C++ skeletons for scripts under: the first folder of project.json's
// gameplay sources, or "src".
[[nodiscard]] std::string gameplay_folder(const std::optional<std::string>& project);

// --- Reports ---

// What couldn't come along, counted by what it is, with the first place it was seen.
class Problems {
public:
    void add(const std::string& what, const std::string& where, const std::string& why = {});
    [[nodiscard]] json::Array to_json() const;

private:
    struct Entry {
        int count = 0;
        std::string where;
        std::string why;
    };
    std::map<std::string, Entry> entries_;
};

// --- Storyboard tasks ---

// A storyboard task to port one script to C++.
struct PortTask {
    std::string link; // the script's path in the project: the task's first link
    std::string name;
    std::string summary;
    std::string text;
    std::vector<std::string> links; // more files it touches, such as the scenes using the script
    std::string engine;             // a tag: "godot" or "unity"
};

struct PortTasks {
    std::string file;             // storyboard/work.json's text, or "" with no tasks
    std::vector<std::string> ids; // each task's ID, in the order given
};

// Adds a task per script to storyboard/work.json's entries, or updates the one an earlier import
// added for it (the entry whose first link is the script), keeping its status. existing is the
// destination's work.json, when it has one.
[[nodiscard]] PortTasks write_port_tasks(const std::optional<std::string>& existing,
                                         const std::vector<PortTask>& tasks);

} // namespace slash::importer
