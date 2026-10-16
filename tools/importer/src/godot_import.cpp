#include "godot_import.hpp"

#include "slash/audio/audio.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

using godot::Section;
using godot::Value;

constexpr std::string_view kReportFile = "import/godot.json";
constexpr std::string_view kFramesSuffix = ".frames.json";
constexpr int kMaxInstanceDepth = 16;

// --- Paths and names ---------------------------------------------------------------------------

std::string strip_res(std::string_view path) {
    constexpr std::string_view kRes = "res://";
    if (path.starts_with(kRes)) {
        path.remove_prefix(kRes.size());
    }
    return std::string(path);
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::size_t extension_start(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return path.size();
    }
    return dot;
}

// ".png" for "a/b.PNG", "" with none.
std::string extension_of(std::string_view path) {
    return lowercase(path.substr(extension_start(path)));
}

std::string without_extension(std::string_view path) {
    return std::string(path.substr(0, extension_start(path)));
}

std::string file_stem(std::string_view path) {
    const std::string bare = without_extension(path);
    const std::size_t slash = bare.find_last_of('/');
    return slash == std::string::npos ? bare : bare.substr(slash + 1);
}

bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}
bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// A node name as an entity ID: "PlayerSprite" -> "player_sprite", "HUDLabel" -> "hud_label",
// "Sprite2D" -> "sprite2d", "Enemy 3" -> "enemy_3".
std::string snake_case(std::string_view name, std::size_t limit = 64) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (is_upper(c)) {
            const char previous = i > 0 ? name[i - 1] : '\0';
            const char next = i + 1 < name.size() ? name[i + 1] : '\0';
            const bool word = is_lower(previous) || (is_upper(previous) && is_lower(next));
            if (word && !out.empty() && out.back() != '_') {
                out += '_';
            }
            out += static_cast<char>(c - 'A' + 'a');
        } else if (is_lower(c) || is_digit(c) || c == '-') {
            out += c;
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    if (out.size() > limit) {
        out.resize(limit);
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out.empty() ? "node" : out;
}

template <typename Range, typename T>
bool contains(const Range& range, const T& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

bool is_texture_file(std::string_view extension) {
    return extension == ".png" || extension == ".bmp";
}
bool is_sound_file(std::string_view extension) {
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}
bool is_font_file(std::string_view extension) {
    return extension == ".ttf" || extension == ".otf";
}
bool is_data_file(std::string_view extension) {
    return extension == ".json" || extension == ".txt" || extension == ".csv";
}
bool is_script_file(std::string_view extension) {
    return extension == ".gd" || extension == ".cs";
}

// Why a file Godot uses can't come along, by extension, or "".
std::string unsupported_file(std::string_view extension) {
    if (extension == ".svg" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".webp" || extension == ".tga" || extension == ".exr" || extension == ".hdr" ||
        extension == ".ktx" || extension == ".dds") {
        return "textures are PNG or BMP files; save it as PNG";
    }
    if (extension == ".woff" || extension == ".woff2" || extension == ".fnt" ||
        extension == ".pfb" || extension == ".pfm") {
        return "fonts are TTF or OTF files";
    }
    if (extension == ".gdshader" || extension == ".gdshaderinc") {
        return "a game's own shaders are on the roadmap; canvas materials are .material.json "
               "graphs";
    }
    if (extension == ".glb" || extension == ".gltf" || extension == ".obj" || extension == ".fbx" ||
        extension == ".blend" || extension == ".dae") {
        return "3D comes with milestone 7";
    }
    if (extension == ".ogv") {
        return "video isn't supported yet";
    }
    return {};
}

// The pixel size of a PNG file, from its IHDR chunk.
std::optional<std::pair<int, int>> png_size(std::string_view bytes) {
    constexpr std::size_t kHeader = 24;
    if (bytes.size() < kHeader || bytes.substr(1, 3) != "PNG" || bytes.substr(12, 4) != "IHDR") {
        return std::nullopt;
    }
    const auto read = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
        }
        return static_cast<int>(value);
    };
    return std::pair{read(16), read(20)};
}

// A whole number, all of text, or nullopt.
std::optional<std::int64_t> parse_int(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// text split at each '/'.
std::vector<std::string_view> split_path(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('/', start), text.size());
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// --- Tile data ---------------------------------------------------------------------------------

// A cell as Godot stores it: where it is and which tile of which source fills it.
struct GodotCell {
    int x = 0;
    int y = 0;
    int source = 0;
    int atlas_x = 0;
    int atlas_y = 0;
    int alternative = 0;
};

std::uint16_t read_u16(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) |
                                      (static_cast<unsigned char>(bytes[at + 1]) << 8U));
}

// Cells of 12 bytes each, little-endian: x and y as signed 16-bit numbers, then the source ID,
// the atlas column and row and the alternative tile, as tile_map_layer.cpp and tile_map.cpp
// write them.
std::optional<std::vector<GodotCell>> decode_godot_cells(std::string_view bytes) {
    constexpr std::size_t kCellSize = 12;
    if (bytes.size() % kCellSize != 0) {
        return std::nullopt;
    }
    std::vector<GodotCell> cells;
    cells.reserve(bytes.size() / kCellSize);
    for (std::size_t at = 0; at < bytes.size(); at += kCellSize) {
        cells.push_back({.x = static_cast<std::int16_t>(read_u16(bytes, at)),
                         .y = static_cast<std::int16_t>(read_u16(bytes, at + 2)),
                         .source = read_u16(bytes, at + 4),
                         .atlas_x = read_u16(bytes, at + 6),
                         .atlas_y = read_u16(bytes, at + 8),
                         .alternative = read_u16(bytes, at + 10)});
    }
    return cells;
}

// A PackedByteArray's bytes: base64 text since Godot 4.3, numbers before it.
std::optional<std::string> byte_array(const Value& value) {
    if (!value.is_call("PackedByteArray")) {
        return std::nullopt;
    }
    if (value.items.empty()) {
        return std::string();
    }
    if (value.items.size() == 1 && value.items.front().is_string()) {
        return base64_decode(value.items.front().text);
    }
    std::string bytes;
    for (const Value& item : value.items) {
        if (item.kind != Value::Kind::Int || item.integer < 0 || item.integer > 255) {
            return std::nullopt;
        }
        bytes += static_cast<char>(item.integer);
    }
    return bytes;
}

// A TileMapLayer's tile_map_data: a 16-bit format number, then its cells.
std::optional<std::vector<GodotCell>> layer_cells(const Value& value) {
    const auto bytes = byte_array(value);
    if (!bytes) {
        return std::nullopt;
    }
    if (bytes->empty()) {
        return std::vector<GodotCell>{};
    }
    if (bytes->size() < 2 || read_u16(*bytes, 0) != 0) {
        return std::nullopt;
    }
    return decode_godot_cells(std::string_view(*bytes).substr(2));
}

// A TileMap layer's tile_data, in the format TileMap saves as format = 2: three 32-bit
// numbers per cell, holding the same 12 bytes.
std::optional<std::vector<GodotCell>> tile_map_cells(const Value& value) {
    const auto numbers = value.numbers();
    if (!numbers) {
        return std::nullopt;
    }
    std::string bytes;
    for (const double number : *numbers) {
        const auto word = static_cast<std::uint32_t>(static_cast<std::int64_t>(number));
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes += static_cast<char>((word >> shift) & 0xFFU);
        }
    }
    return decode_godot_cells(bytes);
}

// Godot's terrain peering bits of square tiles, as kNeighborNames indexes them.
std::optional<std::size_t> peering_direction(std::string_view godot) {
    constexpr std::array<std::string_view, 8> kBits{
        "top_side",    "top_right_corner",   "right_side", "bottom_right_corner",
        "bottom_side", "bottom_left_corner", "left_side",  "top_left_corner"};
    for (std::size_t direction = 0; direction < kBits.size(); ++direction) {
        if (kBits.at(direction) == godot) {
            return direction;
        }
    }
    return std::nullopt;
}

// Whether a terrain set's mode connects tiles by a direction, as canvas's terrain painting does:
// Godot's modes match corners and sides (0), corners (1) or sides (2).
bool connects(std::int64_t mode, std::size_t direction) {
    const bool corner = direction % 2 == 1;
    return mode == 1 ? corner : mode != 2 || !corner;
}

// --- Reports -----------------------------------------------------------------------------------

// What couldn't come along, counted by what it is, with the first place it was seen.
class Problems {
public:
    void add(const std::string& what, const std::string& where, const std::string& why = {}) {
        Entry& entry = entries_[what];
        if (entry.count++ == 0) {
            entry.where = where;
            entry.why = why;
        }
    }

    [[nodiscard]] json::Array to_json() const {
        std::vector<std::pair<std::string, const Entry*>> sorted;
        sorted.reserve(entries_.size());
        for (const auto& [what, entry] : entries_) {
            sorted.emplace_back(what, &entry);
        }
        std::ranges::stable_sort(
            sorted, [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
        json::Array out;
        for (const auto& [what, entry] : sorted) {
            json::Value item;
            item["what"] = what;
            item["count"] = entry->count;
            item["where"] = entry->where;
            if (!entry->why.empty()) {
                item["why"] = entry->why;
            }
            out.push_back(std::move(item));
        }
        return out;
    }

private:
    struct Entry {
        int count = 0;
        std::string where;
        std::string why;
    };
    std::map<std::string, Entry> entries_;
};

// --- Godot files -------------------------------------------------------------------------------

struct ExtResource {
    std::string type;
    std::string path; // res-relative, through the project's UIDs
};

// A parsed .tscn or .tres file.
struct TextFile {
    std::string path; // res-relative
    std::string type; // a .tres file's resource type
    std::vector<Section> sections;
    std::map<std::string, ExtResource, std::less<>> ext;
    std::map<std::string, const Section*, std::less<>> sub;
    const Section* resource = nullptr; // a .tres file's [resource]
};

struct Property {
    std::string name;
    const Value* value = nullptr;
    const TextFile* file = nullptr; // where the value's ExtResource and SubResource IDs point
};

// A node of a scene with the scenes it instances flattened into it.
struct Node {
    std::string path; // from the scene's root: "." or "Player/Sprite2D"
    std::string name;
    std::string type;
    std::string declared;             // the file and node path that declared it, for reports
    std::vector<Property> properties; // later ones win
    std::vector<std::string> groups;
    int parent = -1;
    int instance_root = -1;              // the root of the instanced scene it came with
    const TextFile* instanced = nullptr; // for an instance's root, the scene it instances
    std::vector<int> children;
    std::string id;

    [[nodiscard]] const Property* find(std::string_view property) const {
        for (const Property& candidate : std::views::reverse(properties)) {
            if (candidate.name == property) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

enum class Family : std::uint8_t { Plain, Node2D, Control };

struct NodeType {
    std::string_view godot;
    Family family;
    std::string_view first; // components besides Transform2D or Control
    std::string_view second;
    bool vertical = false; // VBoxContainer, VSlider and the like
};

// The node types that convert, with the components they become. AnimatedSprite2D's own
// component comes first so its frame and speed_scale win over Sprite2D's.
constexpr std::array kNodeTypes{
    NodeType{"Node", Family::Plain, "", ""},
    NodeType{"CanvasLayer", Family::Plain, "", ""},
    NodeType{"Node2D", Family::Node2D, "", ""},
    NodeType{"Marker2D", Family::Node2D, "", ""},
    NodeType{"Sprite2D", Family::Node2D, "Sprite2D", ""},
    NodeType{"AnimatedSprite2D", Family::Node2D, "AnimatedSprite2D", "Sprite2D"},
    NodeType{"Camera2D", Family::Node2D, "Camera2D", ""},
    NodeType{"CollisionShape2D", Family::Node2D, "CollisionShape2D", ""},
    NodeType{"CollisionPolygon2D", Family::Node2D, "CollisionPolygon2D", ""},
    NodeType{"StaticBody2D", Family::Node2D, "StaticBody2D", ""},
    NodeType{"AnimatableBody2D", Family::Node2D, "AnimatableBody2D", ""},
    NodeType{"RigidBody2D", Family::Node2D, "RigidBody2D", ""},
    NodeType{"CharacterBody2D", Family::Node2D, "CharacterBody2D", ""},
    NodeType{"Area2D", Family::Node2D, "Area2D", ""},
    NodeType{"PinJoint2D", Family::Node2D, "PinJoint2D", ""},
    NodeType{"DampedSpringJoint2D", Family::Node2D, "DampedSpringJoint2D", ""},
    NodeType{"GrooveJoint2D", Family::Node2D, "GrooveJoint2D", ""},
    NodeType{"PointLight2D", Family::Node2D, "PointLight2D", ""},
    NodeType{"DirectionalLight2D", Family::Node2D, "DirectionalLight2D", ""},
    NodeType{"LightOccluder2D", Family::Node2D, "LightOccluder2D", ""},
    NodeType{"CanvasModulate", Family::Node2D, "CanvasModulate", ""},
    NodeType{"CPUParticles2D", Family::Node2D, "CPUParticles2D", ""},
    NodeType{"AudioStreamPlayer2D", Family::Node2D, "AudioStreamPlayer2D", ""},
    NodeType{"AudioListener2D", Family::Node2D, "AudioListener2D", ""},
    NodeType{"AudioStreamPlayer", Family::Plain, "AudioStreamPlayer", ""},
    NodeType{"TileMapLayer", Family::Node2D, "TileMapLayer", ""},
    NodeType{"TileMap", Family::Node2D, "TileMapLayer", ""},
    NodeType{"Control", Family::Control, "", ""},
    NodeType{"Label", Family::Control, "Label", ""},
    NodeType{"ColorRect", Family::Control, "ColorRect", ""},
    NodeType{"Panel", Family::Control, "Panel", ""},
    NodeType{"TextureRect", Family::Control, "TextureRect", ""},
    NodeType{"NinePatchRect", Family::Control, "NinePatchRect", ""},
    NodeType{"ProgressBar", Family::Control, "ProgressBar", ""},
    NodeType{"Button", Family::Control, "Button", ""},
    NodeType{"CheckBox", Family::Control, "CheckBox", ""},
    NodeType{"CheckButton", Family::Control, "CheckButton", ""},
    NodeType{"TextureButton", Family::Control, "TextureButton", ""},
    NodeType{"LineEdit", Family::Control, "LineEdit", ""},
    NodeType{"TextEdit", Family::Control, "TextEdit", ""},
    NodeType{"ScrollContainer", Family::Control, "ScrollContainer", ""},
    NodeType{"MarginContainer", Family::Control, "MarginContainer", ""},
    NodeType{"CenterContainer", Family::Control, "CenterContainer", ""},
    NodeType{"PanelContainer", Family::Control, "PanelContainer", ""},
    NodeType{"GridContainer", Family::Control, "GridContainer", ""},
    NodeType{"BoxContainer", Family::Control, "BoxContainer", ""},
    NodeType{"HBoxContainer", Family::Control, "BoxContainer", ""},
    NodeType{"VBoxContainer", Family::Control, "BoxContainer", "", true},
    NodeType{"FlowContainer", Family::Control, "FlowContainer", ""},
    NodeType{"HFlowContainer", Family::Control, "FlowContainer", ""},
    NodeType{"VFlowContainer", Family::Control, "FlowContainer", "", true},
    NodeType{"HSlider", Family::Control, "Slider", ""},
    NodeType{"VSlider", Family::Control, "Slider", "", true},
    NodeType{"HScrollBar", Family::Control, "ScrollBar", ""},
    NodeType{"VScrollBar", Family::Control, "ScrollBar", "", true},
};

const NodeType* find_node_type(std::string_view godot) {
    for (const NodeType& type : kNodeTypes) {
        if (type.godot == godot) {
            return &type;
        }
    }
    return nullptr;
}

// The family of a node type that doesn't convert, by its name and the properties it sets.
Family guess_family(const Node& node) {
    const std::string_view type = node.type;
    for (const Property& property : node.properties) {
        if (property.name.starts_with("anchor_") || property.name.starts_with("offset_") ||
            property.name == "layout_mode") {
            return Family::Control;
        }
    }
    if (type.ends_with("2D") || type == "TileMapLayer" || type == "TileMap" ||
        type == "ParallaxLayer" || type == "BackBufferCopy" || type == "TouchScreenButton" ||
        type == "CanvasGroup") {
        return Family::Node2D;
    }
    if (type.ends_with("Container") || type.ends_with("Button") || type.ends_with("Label") ||
        type.ends_with("Rect") || type.ends_with("Edit") || type.ends_with("List") ||
        type == "Tree" || type == "TabBar" || type == "ColorPicker" || type == "MenuBar") {
        return Family::Control;
    }
    for (const Property& property : node.properties) {
        if (property.name == "position" || property.name == "rotation" ||
            property.name == "scale") {
            return Family::Node2D;
        }
    }
    return Family::Plain;
}

// Why a node type doesn't convert.
std::string unsupported_node(std::string_view type) {
    if (type.ends_with("3D") || type == "WorldEnvironment" || type == "GridMap" ||
        type == "CSGBox3D") {
        return "3D comes with milestone 7";
    }
    if (type == "AnimationPlayer" || type == "AnimationTree") {
        return "animations convert in the next importer step";
    }
    if (type == "Timer") {
        return "timers are gameplay code: count the ticks in a C++ system";
    }
    return "no SlashEngine component for it yet";
}

// Properties that change nothing a SlashEngine game shows or runs, or that Godot's editor keeps.
bool ignored_property(std::string_view name) {
    constexpr std::array kIgnored{
        std::string_view{"editor_description"},
        std::string_view{"unique_name_in_owner"},
        std::string_view{"process_mode"},
        std::string_view{"process_priority"},
        std::string_view{"process_physics_priority"},
        std::string_view{"physics_interpolation_mode"},
        std::string_view{"auto_translate_mode"},
        std::string_view{"layout_mode"},
        std::string_view{"anchors_preset"},
        std::string_view{"debug_color"},
        std::string_view{"process_callback"},
        std::string_view{"_import_path"},
        std::string_view{"script"},
        std::string_view{"rendering_quadrant_size"},
        std::string_view{"cell_quadrant_size"},
        std::string_view{"collision_visibility_mode"},
        std::string_view{"navigation_visibility_mode"},
    };
    return name.starts_with("metadata/") || name.starts_with("editor_") ||
           name.starts_with("process_thread_") || contains(kIgnored, name);
}

// Godot's built-in properties of CanvasItem, Node2D and Control, which a script's exported
// variables can't be.
bool built_in_property(std::string_view name) {
    constexpr std::array kBuiltIn{
        std::string_view{"position"},
        std::string_view{"rotation"},
        std::string_view{"scale"},
        std::string_view{"skew"},
        std::string_view{"visible"},
        std::string_view{"modulate"},
        std::string_view{"self_modulate"},
        std::string_view{"z_index"},
        std::string_view{"z_as_relative"},
        std::string_view{"y_sort_enabled"},
        std::string_view{"texture_filter"},
        std::string_view{"texture_repeat"},
        std::string_view{"light_mask"},
        std::string_view{"material"},
        std::string_view{"use_parent_material"},
        std::string_view{"show_behind_parent"},
        std::string_view{"top_level"},
        std::string_view{"clip_children"},
        std::string_view{"visibility_layer"},
        std::string_view{"rotation_degrees"},
        std::string_view{"tooltip_text"},
        std::string_view{"collision_layer"},
        std::string_view{"collision_mask"},
    };
    return name.starts_with("theme_override_") || name.starts_with("anchor_") ||
           name.starts_with("offset_") || name.starts_with("size_flags_") ||
           name.starts_with("focus_") || contains(kBuiltIn, name);
}

// --- Godot's enums -----------------------------------------------------------------------------

// Godot's enum values for fields whose choices aren't in Godot's order, by "Component.field" or
// "*.field"; "" marks a value with no SlashEngine equivalent. Other enums index the field's
// choices.
const std::vector<std::string_view>* enum_names(std::string_view component,
                                                std::string_view field) {
    static const std::map<std::string, std::vector<std::string_view>, std::less<>> kNames{
        {"*.texture_filter",
         {"inherit", "nearest", "linear", "nearest", "linear", "nearest", "linear"}},
        {"*.alignment", {"left", "center", "right", "fill"}},
        {"*.icon_alignment", {"left", "center", "right"}},
        {"BoxContainer.alignment", {"begin", "center", "end"}},
        {"FlowContainer.alignment", {"begin", "center", "end"}},
        {"Camera2D.anchor_mode", {"fixed_top_left", "drag_center"}},
        {"CPUParticles2D.emission_shape",
         {"point", "sphere", "sphere_surface", "rectangle", "points", "directed_points", "ring"}},
        {"Control.focus_mode", {"none", "click", "all", "none"}},
        {"Control.mouse_filter", {"stop", "pass", "ignore"}},
        {"Control.layout_direction", {"inherited", "inherited", "ltr", "rtl", "inherited"}},
        {"TextEdit.autowrap_mode", {"", "arbitrary", "word", "word_smart"}},
    };
    const std::string exact = std::string(component) + "." + std::string(field);
    if (const auto it = kNames.find(exact); it != kNames.end()) {
        return &it->second;
    }
    if (const auto it = kNames.find("*." + std::string(field)); it != kNames.end()) {
        return &it->second;
    }
    return nullptr;
}

// Control's size flags, a bitmask in Godot: fill 1, expand 2, shrink center 4, shrink end 8.
std::string size_flags(std::int64_t flags) {
    const bool expand = (flags & 2) != 0;
    if ((flags & 1) != 0) {
        return expand ? "expand_fill" : "fill";
    }
    if ((flags & 4) != 0) {
        return expand ? "expand_shrink_center" : "shrink_center";
    }
    if ((flags & 8) != 0) {
        return expand ? "expand_shrink_end" : "shrink_end";
    }
    return expand ? "expand" : "shrink_begin";
}

// --- Input -------------------------------------------------------------------------------------

// Godot's key names for its keycodes (core/os/keyboard.h and keyboard.cpp), as project.json takes
// them.
std::string key_name(std::int64_t code) {
    constexpr std::int64_t kSpecial = std::int64_t{1} << 22;
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) {
        return {static_cast<char>(code)};
    }
    static const std::map<std::int64_t, std::string_view> kPrintable{
        {32, "Space"},       {33, "Exclam"},       {34, "QuoteDbl"},    {35, "NumberSign"},
        {36, "Dollar"},      {37, "Percent"},      {38, "Ampersand"},   {39, "Apostrophe"},
        {40, "ParenLeft"},   {41, "ParenRight"},   {42, "Asterisk"},    {43, "Plus"},
        {44, "Comma"},       {45, "Minus"},        {46, "Period"},      {47, "Slash"},
        {58, "Colon"},       {59, "Semicolon"},    {60, "Less"},        {61, "Equal"},
        {62, "Greater"},     {63, "Question"},     {64, "At"},          {91, "BracketLeft"},
        {92, "BackSlash"},   {93, "BracketRight"}, {94, "AsciiCircum"}, {95, "UnderScore"},
        {96, "QuoteLeft"},   {123, "BraceLeft"},   {124, "Bar"},        {125, "BraceRight"},
        {126, "AsciiTilde"},
    };
    if (const auto it = kPrintable.find(code); it != kPrintable.end()) {
        return std::string(it->second);
    }
    if ((code & kSpecial) == 0) {
        return {};
    }
    const std::int64_t special = code & ~kSpecial;
    static const std::map<std::int64_t, std::string_view> kSpecialNames{
        {0x01, "Escape"},     {0x02, "Tab"},           {0x04, "Backspace"}, {0x05, "Enter"},
        {0x06, "Kp Enter"},   {0x07, "Insert"},        {0x08, "Delete"},    {0x09, "Pause"},
        {0x0A, "Print"},      {0x0B, "SysReq"},        {0x0C, "Clear"},     {0x0D, "Home"},
        {0x0E, "End"},        {0x0F, "Left"},          {0x10, "Up"},        {0x11, "Right"},
        {0x12, "Down"},       {0x13, "PageUp"},        {0x14, "PageDown"},  {0x15, "Shift"},
        {0x16, "Ctrl"},       {0x17, "Meta"},          {0x18, "Alt"},       {0x19, "CapsLock"},
        {0x1A, "NumLock"},    {0x1B, "ScrollLock"},    {0x42, "Menu"},      {0x45, "Help"},
        {0x48, "Back"},       {0x49, "Forward"},       {0x4A, "Stop"},      {0x4B, "Refresh"},
        {0x4C, "VolumeDown"}, {0x4D, "VolumeMute"},    {0x4E, "VolumeUp"},  {0x54, "MediaPlay"},
        {0x55, "MediaStop"},  {0x56, "MediaPrevious"}, {0x57, "MediaNext"}, {0x58, "MediaRecord"},
        {0x5B, "Search"},     {0x81, "Kp Multiply"},   {0x82, "Kp Divide"}, {0x83, "Kp Subtract"},
        {0x84, "Kp Period"},  {0x85, "Kp Add"},
    };
    if (const auto it = kSpecialNames.find(special); it != kSpecialNames.end()) {
        return std::string(it->second);
    }
    if (special >= 0x1C && special <= 0x33) { // F1 to F24
        return "F" + std::to_string(special - 0x1C + 1);
    }
    if (special >= 0x86 && special <= 0x8F) { // Kp 0 to Kp 9
        return "Kp " + std::to_string(special - 0x86);
    }
    return {};
}

// One of Godot's InputEvent objects as a project.json binding, or null with why.
json::Value input_binding(const Value& event, std::string& why) {
    const std::string type = event.items.empty() ? "" : event.items.front().text;
    const auto integer = [&](std::string_view name) {
        const Value* value = event.get(name);
        return value != nullptr && value->kind == Value::Kind::Int ? value->integer : 0;
    };
    const auto flag = [&](std::string_view name) {
        const Value* value = event.get(name);
        return value != nullptr && value->kind == Value::Kind::Bool && value->boolean;
    };
    json::Value binding;
    const auto modifiers = [&] {
        for (const auto& [godot, ours] :
             {std::pair{"shift_pressed", "shift"}, std::pair{"ctrl_pressed", "ctrl"},
              std::pair{"alt_pressed", "alt"}, std::pair{"meta_pressed", "meta"}}) {
            if (flag(godot)) {
                binding[ours] = true;
            }
        }
        if (flag("command_or_control_autoremap")) {
            binding["ctrl"] = true;
        }
    };
    if (const std::int64_t device = integer("device");
        device >= 0 && event.get("device") != nullptr) {
        binding["device"] = device;
    }
    if (type == "InputEventKey") {
        binding["type"] = "key";
        if (const std::int64_t physical = integer("physical_keycode"); physical != 0) {
            binding["physical_key"] = key_name(physical);
        } else if (const std::int64_t code = integer("keycode"); code != 0) {
            binding["key"] = key_name(code);
        } else {
            why = "a key event with no key";
            return nullptr;
        }
        modifiers();
        return binding;
    }
    if (type == "InputEventMouseButton") {
        static constexpr std::array<std::string_view, 10> kButtons{
            "",           "left",       "right",       "middle",   "wheel_up",
            "wheel_down", "wheel_left", "wheel_right", "xbutton1", "xbutton2"};
        const std::int64_t index = integer("button_index");
        if (index <= 0 || index >= static_cast<std::int64_t>(kButtons.size())) {
            why = "mouse button " + std::to_string(index);
            return nullptr;
        }
        binding["type"] = "mouse_button";
        binding["button"] = kButtons.at(static_cast<std::size_t>(index));
        modifiers();
        return binding;
    }
    if (type == "InputEventJoypadButton") {
        static constexpr std::array<std::string_view, 21> kButtons{"a",
                                                                   "b",
                                                                   "x",
                                                                   "y",
                                                                   "back",
                                                                   "guide",
                                                                   "start",
                                                                   "left_stick",
                                                                   "right_stick",
                                                                   "left_shoulder",
                                                                   "right_shoulder",
                                                                   "dpad_up",
                                                                   "dpad_down",
                                                                   "dpad_left",
                                                                   "dpad_right",
                                                                   "misc1",
                                                                   "paddle1",
                                                                   "paddle2",
                                                                   "paddle3",
                                                                   "paddle4",
                                                                   "touchpad"};
        const std::int64_t index = integer("button_index");
        if (index < 0 || index >= static_cast<std::int64_t>(kButtons.size())) {
            why = "gamepad button " + std::to_string(index);
            return nullptr;
        }
        binding["type"] = "joypad_button";
        binding["button"] = kButtons.at(static_cast<std::size_t>(index));
        return binding;
    }
    if (type == "InputEventJoypadMotion") {
        static constexpr std::array<std::string_view, 6> kAxes{
            "left_x", "left_y", "right_x", "right_y", "trigger_left", "trigger_right"};
        const std::int64_t axis = integer("axis");
        const Value* value = event.get("axis_value");
        if (axis < 0 || axis >= static_cast<std::int64_t>(kAxes.size()) || value == nullptr) {
            why = "gamepad axis " + std::to_string(axis);
            return nullptr;
        }
        binding["type"] = "joypad_motion";
        binding["axis"] = kAxes.at(static_cast<std::size_t>(axis));
        binding["axis_value"] = value->number() < 0.0 ? -1 : 1;
        return binding;
    }
    why = type.empty() ? "an input event that isn't an object" : type + " bindings";
    return nullptr;
}

// --- Scripts -----------------------------------------------------------------------------------

// What a GDScript file declares, from its lines: what it extends, its class name, signals,
// functions and exported variables.
struct ScriptOutline {
    std::string extends;
    std::string class_name;
    std::vector<std::string> signals;
    std::vector<std::string> functions;
    std::vector<std::string> exports;
};

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r:");
    return std::string(text.substr(first, last - first + 1));
}

ScriptOutline outline_script(std::string_view text) {
    ScriptOutline outline;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            continue; // only declarations at the top level
        }
        if (const auto comment = line.find('#'); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        const auto after = [&](std::string_view keyword) {
            return trim(line.substr(keyword.size()));
        };
        if (line.starts_with("extends ")) {
            outline.extends = after("extends ");
        } else if (line.starts_with("class_name ")) {
            outline.class_name = after("class_name ");
        } else if (line.starts_with("signal ")) {
            outline.signals.push_back(after("signal "));
        } else if (line.starts_with("func ") || line.starts_with("static func ")) {
            std::string declaration = after(line.starts_with("func ") ? "func " : "static func ");
            if (const auto arrow = declaration.find("->"); arrow != std::string::npos) {
                declaration = trim(declaration.substr(0, arrow));
            }
            outline.functions.push_back(std::move(declaration));
        } else if (line.starts_with("@export")) {
            if (const auto var = line.find("var "); var != std::string_view::npos) {
                outline.exports.push_back(trim(line.substr(var + 4)));
            }
        }
    }
    return outline;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += separator;
        }
        out += item;
    }
    return out;
}

// A scene's use of a script: the entity and the values its node set.
struct ScriptUse {
    std::string scene;
    std::string entity;
    json::Value values;
};

struct ScriptRecord {
    std::vector<ScriptUse> uses;
    json::Array connections; // signal connections whose method is in the script
};

// --- The importer ------------------------------------------------------------------------------

class GodotImporter {
public:
    GodotImporter(std::string source, std::string prefix, bool copy_assets)
        : source_(std::move(source)), prefix_(std::move(prefix)), copy_assets_(copy_assets) {
        world::register_builtin_components(components_);
    }

    Result<ImportPlan> run(const std::optional<std::string>& existing_project,
                           const std::optional<std::string>& existing_work) {
        auto project_text = platform::read_file(source_ + "project.godot");
        if (!project_text) {
            return Error{.code = ErrorCode::NotFound,
                         .message = "no project.godot in " + source_ +
                                    ": only Godot 4 projects can be imported for now"};
        }
        auto settings = godot::parse_project_settings(project_text.value());
        if (!settings) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "project.godot: " + settings.error().message};
        }
        if (const Value* version = find_setting(settings.value(), "", "config_version");
            version == nullptr || version->number() < 5) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "a Godot 3 project: open it in Godot 4 and save it there "
                                    "first (Godot converts it), then import it"};
        }
        auto listed = platform::list_files(source_, [](std::string_view folder) {
            return folder != "build/" && folder != "addons/";
        });
        if (!listed) {
            return listed.error();
        }
        files_ = std::move(listed.value());
        load_text_files();
        for (const std::string& file : files_) {
            take_file(file);
        }
        convert_project(settings.value(), existing_project);
        for (const auto& [path, text_file] : text_files_) {
            const std::string& type = text_file->type;
            if (extension_of(path) == ".tscn") {
                convert_scene(*text_file);
            } else if (text_file->resource != nullptr &&
                       (type == "SpriteFrames" || type == "TileSet" || type == "Theme")) {
                std::string why;
                (void)converted_resource(*text_file->resource, *text_file, why);
            } else if (text_file->resource != nullptr && type == "AudioBusLayout") {
                (void)bus_layout_file(*text_file);
            } else if (!type.empty()) {
                resources_seen_[type].push_back(path);
            }
        }
        // A resource file nothing converted used, of a type nothing converted.
        for (const auto& [type, paths] : resources_seen_) {
            if (resources_used_.contains(type)) {
                continue;
            }
            for (const std::string& path : paths) {
                if (!resources_read_.contains(path)) {
                    problems_.add("resource " + type, path, resource_why(type));
                }
            }
        }
        write_tasks(existing_work);
        write_report();
        return std::move(plan_);
    }

private:
    // --- Files ---

    static const Value* find_setting(const std::vector<Section>& settings, std::string_view tag,
                                     std::string_view key) {
        for (const Section& section : settings) {
            if (section.tag == tag) {
                if (const Value* value = section.property(key); value != nullptr) {
                    return value;
                }
            }
        }
        return nullptr;
    }

    void add_file(std::string path, std::string contents) {
        plan_.files.emplace_back(std::move(path), std::move(contents));
    }

    [[nodiscard]] std::string destination(std::string_view res) const {
        return prefix_ + strip_res(res);
    }

    // The parsed .tres or .tscn file a res:// or uid:// path names, with its [resource], or
    // nullptr.
    const TextFile* res_file(const std::string& reference) {
        std::string path = strip_res(reference);
        if (path.starts_with("uid://") && uids_.contains(path)) {
            path = uids_[path];
        }
        const auto it = text_files_.find(path);
        return it != text_files_.end() && it->second->resource != nullptr ? it->second.get()
                                                                          : nullptr;
    }

    // Parses every .tscn and .tres file and learns the UIDs Godot refers to files by.
    void load_text_files() {
        for (const std::string& file : files_) {
            const std::string extension = extension_of(file);
            if (extension == ".import" || extension == ".uid") {
                if (auto text = platform::read_file(source_ + file); text) {
                    learn_uid(text.value(), without_extension(file));
                }
                continue;
            }
            if (extension != ".tscn" && extension != ".tres") {
                continue;
            }
            auto text = platform::read_file(source_ + file);
            if (!text) {
                problems_.add("unreadable file", file, text.error().message);
                continue;
            }
            auto sections = godot::parse_resource(text.value());
            if (!sections) {
                problems_.add("unreadable file", file, sections.error().message);
                continue;
            }
            auto parsed = std::make_unique<TextFile>();
            parsed->path = file;
            parsed->sections = std::move(sections.value());
            if (!parsed->sections.empty()) {
                const Section& header = parsed->sections.front();
                if (header.tag == "gd_resource") {
                    parsed->type = header.field_string("type");
                }
                if (const std::string uid = header.field_string("uid"); !uid.empty()) {
                    uids_[uid] = file;
                }
            }
            text_files_.emplace(file, std::move(parsed));
        }
        for (auto& [path, file] : text_files_) {
            index_resources(*file);
        }
    }

    // A .uid file holds "uid://...", and an .import file a uid="uid://..." line.
    void learn_uid(std::string_view text, const std::string& file) {
        const std::size_t at = text.find("uid://");
        if (at == std::string_view::npos) {
            return;
        }
        std::size_t end = at;
        while (end < text.size() && text[end] != '"' && text[end] != '\n' && text[end] != '\r') {
            ++end;
        }
        uids_[std::string(text.substr(at, end - at))] = file;
    }

    void index_resources(TextFile& file) {
        for (const Section& section : file.sections) {
            if (section.tag == "ext_resource") {
                ExtResource ext{.type = section.field_string("type"),
                                .path = strip_res(section.field_string("path"))};
                const std::string uid = section.field_string("uid");
                if (!std::ranges::binary_search(files_, ext.path) && uids_.contains(uid)) {
                    ext.path = uids_[uid];
                }
                file.ext[section.field_string("id")] = std::move(ext);
            } else if (section.tag == "sub_resource") {
                file.sub[section.field_string("id")] = &section;
            } else if (section.tag == "resource") {
                file.resource = &section;
            }
        }
    }

    // Copies an asset, or notes one that can't come along.
    void take_file(const std::string& file) {
        const std::string extension = extension_of(file);
        if (is_texture_file(extension) || is_sound_file(extension) || is_font_file(extension) ||
            is_data_file(extension) || is_script_file(extension)) {
            if (is_texture_file(extension) || is_sound_file(extension) || is_font_file(extension)) {
                ++assets_;
            }
            if (is_script_file(extension)) {
                scripts_[file];
            }
            if (copy_assets_) {
                if (auto bytes = platform::read_file(source_ + file); bytes) {
                    add_file(destination(file), std::move(bytes.value()));
                } else {
                    problems_.add("unreadable file", file, bytes.error().message);
                }
            }
            return;
        }
        if (const std::string why = unsupported_file(extension); !why.empty()) {
            problems_.add("file type " + extension, file, why);
        }
    }

    static std::string resource_why(std::string_view type) {
        if (type == "Animation" || type == "AnimationLibrary") {
            return "animations convert in the next importer step";
        }
        return "no SlashEngine equivalent yet";
    }

    // --- Project settings and input ---

    void convert_project(const std::vector<Section>& settings,
                         const std::optional<std::string>& existing) {
        const auto setting = [&](std::string_view tag, std::string_view key) {
            return find_setting(settings, tag, key);
        };
        std::string name = "Imported Game";
        if (const Value* value = setting("application", "config/name");
            value != nullptr && value->is_string() && !value->text.empty()) {
            name = value->text;
        }
        project_name_ = name;
        if (const Value* value = setting("application", "run/main_scene");
            value != nullptr && value->is_string()) {
            std::string scene = value->text;
            if (scene.starts_with("uid://") && uids_.contains(scene)) {
                scene = uids_[scene];
            }
            main_scene_ =
                destination(without_extension(strip_res(scene)) + std::string(world::kSceneSuffix));
        }

        json::Value project = runtime::new_project(name, main_scene_);
        if (existing) {
            if (auto parsed = json::parse(*existing); parsed && parsed.value().is_object()) {
                json::Value merged = std::move(parsed.value());
                merged["name"] = name;
                if (!main_scene_.empty()) {
                    merged["main_scene"] = main_scene_;
                }
                project = std::move(merged);
            }
        }
        const auto number = [&](std::string_view tag, std::string_view key) -> const Value* {
            const Value* value = setting(tag, key);
            return value != nullptr && value->is_number() ? value : nullptr;
        };
        if (const Value* width = number("display", "window/size/viewport_width")) {
            project["window"]["width"] = static_cast<std::int64_t>(width->number());
        }
        if (const Value* height = number("display", "window/size/viewport_height")) {
            project["window"]["height"] = static_cast<std::int64_t>(height->number());
        }
        if (const Value* vsync = number("display", "window/vsync/vsync_mode")) {
            static constexpr std::array<std::string_view, 4> kModes{"disabled", "enabled",
                                                                    "adaptive", "mailbox"};
            const auto mode = static_cast<std::int64_t>(vsync->number());
            if (mode >= 0 && mode < 4) {
                project["window"]["vsync_mode"] = kModes.at(static_cast<std::size_t>(mode));
            }
        }
        if (const Value* fps = number("application", "run/max_fps")) {
            project["max_fps"] = static_cast<std::int64_t>(fps->number());
        }
        if (const Value* ticks = number("physics", "common/physics_ticks_per_second")) {
            project["tick_hz"] = ticks->number();
        }
        if (const Value* color = setting("rendering", "environment/defaults/default_clear_color");
            color != nullptr && color->is_call("Color")) {
            if (auto channels = color->numbers(); channels && channels->size() == 4) {
                project["rendering"]["clear_color"] =
                    json::Array{channels->at(0), channels->at(1), channels->at(2), channels->at(3)};
            }
        }
        if (const Value* filter =
                number("rendering", "textures/canvas_textures/default_texture_filter")) {
            project["rendering"]["texture_filter"] =
                static_cast<std::int64_t>(filter->number()) % 2 == 0 ? "nearest" : "linear";
        }
        if (const Value* gravity = number("physics", "2d/default_gravity")) {
            project["physics_2d"]["gravity"] = gravity->number();
        }
        if (const Value* vector = setting("physics", "2d/default_gravity_vector")) {
            if (auto xy = vector->numbers(); xy && xy->size() == 2) {
                project["physics_2d"]["gravity_vector"] = json::Array{xy->at(0), xy->at(1)};
            }
        }
        if (const Value* damp = number("physics", "2d/default_linear_damp")) {
            project["physics_2d"]["linear_damp"] = damp->number();
        }
        if (const Value* damp = number("physics", "2d/default_angular_damp")) {
            project["physics_2d"]["angular_damp"] = damp->number();
        }
        if (const Value* stretch = setting("display", "window/stretch/mode");
            stretch != nullptr && stretch->is_string() && stretch->text != "disabled") {
            problems_.add("setting display/window/stretch/mode", "project.godot",
                          "the window shows the game at its own size; stretching isn't "
                          "supported yet");
        }
        if (const Value* theme = setting("gui", "theme/custom");
            theme != nullptr && theme->is_string() && !theme->text.empty()) {
            const TextFile* file = res_file(theme->text);
            std::string why = "not a Theme resource";
            const std::string path = file != nullptr && file->type == "Theme"
                                         ? converted_resource(*file->resource, *file, why)
                                         : "";
            if (path.empty()) {
                problems_.add("setting gui/theme/custom", "project.godot", why);
            } else {
                project["gui"]["theme"] = path;
            }
        }
        if (setting("gui", "theme/custom_font") != nullptr) {
            problems_.add("setting gui/theme/custom_font", "project.godot",
                          "set default_font in the project's theme instead");
        }
        // Godot uses res://default_bus_layout.tres when the setting names no other file.
        const Value* layout = setting("audio", "buses/default_bus_layout");
        const TextFile* buses = res_file(layout != nullptr && layout->is_string()
                                             ? layout->text
                                             : std::string("res://default_bus_layout.tres"));
        if (buses != nullptr && buses->type == "AudioBusLayout") {
            project["audio"]["bus_layout"] = bus_layout_file(*buses);
        } else if (layout != nullptr) {
            problems_.add("setting audio/buses/default_bus_layout", "project.godot",
                          "not an AudioBusLayout resource");
        }
        if (const Value* rate = number("audio", "driver/mix_rate")) {
            project["audio"]["mix_rate"] = static_cast<std::int64_t>(rate->number());
        }
        if (const Value* latency = number("audio", "driver/output_latency")) {
            project["audio"]["output_latency"] = latency->number();
        }
        if (const Value* strength = number("audio", "general/2d_panning_strength")) {
            project["audio"]["panning_strength_2d"] = strength->number();
        }
        convert_input(settings, project);
        for (const Section& section : settings) {
            if (section.tag != "autoload") {
                continue;
            }
            for (const auto& [autoload, value] : section.properties) {
                if (!value.is_string()) {
                    continue;
                }
                std::string path = value.text;
                if (path.starts_with("*")) {
                    path.erase(0, 1);
                }
                path = strip_res(path);
                if (path.starts_with("uid://") && uids_.contains(path)) {
                    path = uids_[path];
                }
                autoloads_[autoload] = path;
                problems_.add("autoload", "project.godot: " + autoload,
                              "autoloads are gameplay code; see the storyboard task");
                if (is_script_file(extension_of(path))) {
                    scripts_[path];
                }
            }
        }

        // Keep only what validates, so a bad setting can't stop the rest.
        for (int pass = 0; pass < 32; ++pass) {
            const auto errors = runtime::validate_project(project);
            if (errors.empty()) {
                break;
            }
            for (const json::ValidationError& error : errors) {
                problems_.add("setting " + error.path, "project.godot", error.message);
                if (!remove_pointer(project, error.path)) {
                    project = runtime::new_project(name, main_scene_);
                    break;
                }
            }
        }
        add_file("project.json", json::write(project) + "\n");
    }

    void convert_input(const std::vector<Section>& settings, json::Value& project) {
        for (const Section& section : settings) {
            if (section.tag != "input") {
                continue;
            }
            for (const auto& [action, value] : section.properties) {
                if (value.kind != Value::Kind::Dictionary) {
                    continue;
                }
                json::Value converted;
                if (const Value* deadzone = value.get("deadzone");
                    deadzone != nullptr && deadzone->is_number()) {
                    converted["deadzone"] = deadzone->number();
                }
                json::Array events;
                if (const Value* list = value.get("events"); list != nullptr) {
                    for (const Value& event : list->items) {
                        std::string why;
                        json::Value binding = input_binding(event, why);
                        if (binding.is_null()) {
                            problems_.add("input binding", "project.godot: input/" + action, why);
                        } else {
                            events.push_back(std::move(binding));
                        }
                    }
                }
                converted["events"] = std::move(events);
                project["input"][action] = std::move(converted);
                ++input_actions_;
            }
        }
    }

    // An array index in a JSON Pointer, or nullopt.
    static std::optional<std::size_t> pointer_index(std::string_view part) {
        std::size_t index = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), index);
        if (part.empty() || error != std::errc() || end != part.data() + part.size()) {
            return std::nullopt;
        }
        return index;
    }

    // Removes the value at a JSON Pointer; false if there is none to remove.
    static bool remove_pointer(json::Value& root, std::string_view pointer) {
        if (pointer.empty() || pointer.front() != '/') {
            return false;
        }
        std::vector<std::string> parts;
        std::size_t start = 1;
        while (start <= pointer.size()) {
            const std::size_t end = std::min(pointer.find('/', start), pointer.size());
            std::string part(pointer.substr(start, end - start));
            for (std::size_t at = part.find('~'); at != std::string::npos;
                 at = part.find('~', at + 1)) {
                part.replace(at, 2, part.compare(at, 2, "~1") == 0 ? "/" : "~");
            }
            parts.push_back(std::move(part));
            start = end + 1;
        }
        json::Value* parent = &root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            if (parent->is_object()) {
                parent = parent->find(parts[i]);
            } else if (const auto index = pointer_index(parts[i]); parent->is_array() && index) {
                parent = *index < parent->as_array().size() ? &parent->as_array()[*index] : nullptr;
            } else {
                parent = nullptr;
            }
            if (parent == nullptr) {
                return false;
            }
        }
        const std::string& last = parts.back();
        if (parent->is_object()) {
            return parent->as_object().erase(last) > 0;
        }
        if (const auto index = pointer_index(last); parent->is_array() && index) {
            auto& items = parent->as_array();
            if (*index < items.size()) {
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(*index));
                return true;
            }
        }
        return false;
    }

    // --- Scenes ---

    // Adds the nodes of file to nodes, under prefix (a node path, "." for the scene's root),
    // instancing the scenes it instances. Returns the index of its root, or -1.
    int flatten(const TextFile& file, const std::string& prefix, std::vector<Node>& nodes,
                std::map<std::string, int>& by_path, std::vector<std::string>& stack) {
        if (contains(stack, file.path) || static_cast<int>(stack.size()) > kMaxInstanceDepth) {
            problems_.add("scene instancing itself", file.path);
            return -1;
        }
        stack.push_back(file.path);
        int root = -1;
        const auto join_path = [](const std::string& outer, const std::string& inner) {
            if (inner == ".") {
                return outer;
            }
            return outer == "." ? inner : outer + "/" + inner;
        };
        for (const Section& section : file.sections) {
            if (section.tag != "node") {
                continue;
            }
            const std::string name = section.field_string("name");
            const Value* parent_field = section.field("parent");
            std::string local = ".";
            std::string parent_local;
            if (parent_field != nullptr) {
                parent_local = parent_field->is_string()
                                   ? parent_field->text
                                   : parent_field->string_argument().value_or(".");
                local = join_path(parent_local, name);
            }
            const std::string full = join_path(prefix, local);
            int index = -1;
            if (const auto found = by_path.find(full); found != by_path.end()) {
                index = found->second; // an override of a node an instanced scene added
            } else if (const Value* instance = section.field("instance"); instance != nullptr) {
                const std::string id = instance->string_argument().value_or("");
                const auto ext = file.ext.find(id);
                const TextFile* instanced = nullptr;
                if (ext != file.ext.end()) {
                    if (const auto it = text_files_.find(ext->second.path);
                        it != text_files_.end()) {
                        instanced = it->second.get();
                    }
                }
                if (instanced == nullptr) {
                    problems_.add("missing instanced scene", file.path + ": " + local);
                    continue;
                }
                const int first = static_cast<int>(nodes.size());
                index = flatten(*instanced, full, nodes, by_path, stack);
                if (index < 0) {
                    continue;
                }
                for (auto i = static_cast<std::size_t>(first); i < nodes.size(); ++i) {
                    if (nodes[i].instance_root < 0 && std::cmp_not_equal(i, index)) {
                        nodes[i].instance_root = index;
                    }
                }
                Node& root_node = nodes[static_cast<std::size_t>(index)];
                if (!name.empty()) {
                    root_node.name = name;
                }
                root_node.declared = file.path + ": " + local;
                root_node.instanced = instanced;
            } else if (section.field("instance_placeholder") != nullptr) {
                problems_.add("instance placeholder", file.path + ": " + local);
                continue;
            } else {
                Node node;
                node.path = full;
                node.name = name;
                node.type = section.field_string("type");
                node.declared = file.path + ": " + local;
                index = static_cast<int>(nodes.size());
                nodes.push_back(std::move(node));
                by_path[full] = index;
            }
            Node& node = nodes[static_cast<std::size_t>(index)];
            for (const auto& [property, value] : section.properties) {
                node.properties.push_back(
                    Property{.name = property, .value = &value, .file = &file});
            }
            if (const Value* groups = section.field("groups"); groups != nullptr) {
                for (const Value& group : groups->items) {
                    if (group.is_string()) {
                        node.groups.push_back(group.text);
                    }
                }
            }
            if (local == ".") {
                root = index;
            } else if (node.parent < 0) {
                const std::string parent_full = join_path(prefix, parent_local);
                if (const auto parent = by_path.find(parent_full); parent != by_path.end()) {
                    node.parent = parent->second;
                    nodes[static_cast<std::size_t>(parent->second)].children.push_back(index);
                } else {
                    problems_.add("node with a missing parent", file.path + ": " + local);
                }
            }
        }
        stack.pop_back();
        return root;
    }

    void assign_ids(std::vector<Node>& nodes) {
        std::set<std::string> used;
        const auto take = [&](std::string id) {
            if (id.size() > 64) {
                id.resize(64);
            }
            if (used.insert(id).second) {
                return id;
            }
            for (int n = 2;; ++n) {
                std::string candidate = id.substr(0, 60) + "_" + std::to_string(n);
                if (used.insert(candidate).second) {
                    return candidate;
                }
            }
        };
        for (Node& node : nodes) {
            const std::string base = snake_case(node.name);
            if (node.instance_root >= 0) {
                // The instance's ID, '/' and the node's ID in the scene it instances.
                const Node& owner = nodes[static_cast<std::size_t>(node.instance_root)];
                const std::string relative =
                    owner.path == "." ? node.path : node.path.substr(owner.path.size() + 1);
                std::string local = base;
                if (owner.instanced != nullptr) {
                    if (const auto ids = scene_ids_.find(owner.instanced->path);
                        ids != scene_ids_.end()) {
                        if (const auto id = ids->second.find(relative); id != ids->second.end()) {
                            local = id->second;
                        }
                    }
                }
                node.id = owner.id + "/" + local;
                used.insert(node.id);
                continue;
            }
            if (!used.contains(base) || node.parent < 0) {
                node.id = take(base);
                continue;
            }
            node.id = take(nodes[static_cast<std::size_t>(node.parent)].id + "_" + base);
        }
    }

    // The entity a node path names, relative to a node, or "".
    static std::string resolve_node_path(const std::vector<Node>& nodes, const Node& from,
                                         std::string_view path) {
        if (path.empty() || path.front() == '/') {
            return {};
        }
        std::vector<std::string> parts;
        if (from.path != ".") {
            std::size_t start = 0;
            while (start <= from.path.size()) {
                const std::size_t end = std::min(from.path.find('/', start), from.path.size());
                parts.emplace_back(from.path.substr(start, end - start));
                start = end + 1;
            }
        }
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t end = std::min(path.find('/', start), path.size());
            const std::string_view part = path.substr(start, end - start);
            start = end + 1;
            if (part == "..") {
                if (parts.empty()) {
                    return {};
                }
                parts.pop_back();
            } else if (!part.empty() && part != ".") {
                if (const auto colon = part.find(':'); colon != std::string_view::npos) {
                    parts.emplace_back(part.substr(0, colon)); // a property path's node
                } else {
                    parts.emplace_back(part);
                }
            }
        }
        const std::string target = parts.empty() ? "." : join(parts, "/");
        for (const Node& node : nodes) {
            if (node.path == target) {
                return node.id;
            }
        }
        return {};
    }

    struct SceneContext {
        const std::vector<Node>* nodes = nullptr;
        const Node* node = nullptr;
        std::string scene; // the converted scene's path
        std::string stem;  // the converted scene's path without its suffix, for files it makes
    };

    // A value with no field to say its type: numbers, strings, vectors and colors as JSON.
    std::optional<json::Value> guess(const Value& value, const TextFile& file, std::string& why) {
        switch (value.kind) {
        case Value::Kind::Bool:
            return json::Value(value.boolean);
        case Value::Kind::Int:
            return json::Value(value.integer);
        case Value::Kind::Float:
            return json::Value(value.real);
        case Value::Kind::String:
        case Value::Kind::StringName:
            return json::Value(value.text);
        case Value::Kind::Array: {
            json::Array items;
            for (const Value& item : value.items) {
                auto converted = guess(item, file, why);
                if (!converted) {
                    return std::nullopt;
                }
                items.push_back(std::move(*converted));
            }
            return json::Value(std::move(items));
        }
        case Value::Kind::Dictionary: {
            json::Object members;
            for (const auto& [key, item] : value.entries) {
                auto converted = guess(item, file, why);
                if (!key.is_string() || !converted) {
                    return std::nullopt;
                }
                members.emplace(key.text, std::move(*converted));
            }
            return json::Value(std::move(members));
        }
        case Value::Kind::Call:
            break;
        case Value::Kind::Nil:
            return json::Value(nullptr);
        }
        if (value.is_call("ExtResource") || value.is_call("SubResource")) {
            std::string path = resource_path(value, file, why);
            return path.empty() ? std::nullopt : std::optional<json::Value>(std::move(path));
        }
        if (value.is_call("NodePath")) {
            return json::Value(value.string_argument().value_or(""));
        }
        const auto numbers = value.numbers();
        if (!numbers) {
            why = value.text + " values";
            return std::nullopt;
        }
        const auto group = [&](std::size_t size) -> std::optional<json::Value> {
            if (numbers->size() % size != 0) {
                why = "a " + value.text + " with " + std::to_string(numbers->size()) + " numbers";
                return std::nullopt;
            }
            json::Array out;
            for (std::size_t i = 0; i < numbers->size(); i += size) {
                json::Array item;
                for (std::size_t j = 0; j < size; ++j) {
                    item.emplace_back(numbers->at(i + j));
                }
                out.emplace_back(std::move(item));
            }
            return json::Value(std::move(out));
        };
        if (value.text == "PackedVector2Array") {
            return group(2);
        }
        if (value.text == "PackedColorArray") {
            return group(4);
        }
        json::Array out;
        for (const double number : *numbers) {
            out.emplace_back(number);
        }
        return json::Value(std::move(out));
    }

    // The project path a resource reference becomes, or "" with why.
    std::string resource_path(const Value& reference, const TextFile& file, std::string& why) {
        const std::string id = reference.string_argument().value_or("");
        if (reference.is_call("ExtResource")) {
            const auto ext = file.ext.find(id);
            if (ext == file.ext.end()) {
                why = "a missing resource";
                return {};
            }
            const std::string& path = ext->second.path;
            const std::string extension = extension_of(path);
            if (!std::ranges::binary_search(files_, path)) {
                why = "missing file " + path;
                return {};
            }
            if (is_texture_file(extension) || is_sound_file(extension) || is_font_file(extension)) {
                return destination(path);
            }
            if (extension == ".tscn") {
                return destination(without_extension(path) + std::string(world::kSceneSuffix));
            }
            if (extension == ".tres") {
                const auto it = text_files_.find(path);
                if (it != text_files_.end() && it->second->resource != nullptr) {
                    return converted_resource(*it->second->resource, *it->second, why);
                }
                why = it != text_files_.end() ? it->second->type + " resources"
                                              : "a missing resource";
                return {};
            }
            why = unsupported_file(extension);
            if (why.empty()) {
                why = ext->second.type + " resources";
            }
            return {};
        }
        if (reference.is_call("SubResource")) {
            const auto sub = file.sub.find(id);
            if (sub == file.sub.end()) {
                why = "a missing sub-resource";
                return {};
            }
            return converted_resource(*sub->second, file, why);
        }
        why = reference.text + " references";
        return {};
    }

    // The file a resource converts to, written once: sprite frames, a tile set or a theme. A
    // .tres file's becomes a file beside it, a scene's own one "<scene>_<id>". "" with why for
    // other types.
    std::string converted_resource(const Section& section, const TextFile& file, std::string& why) {
        const std::string type = resource_type(section, file);
        const std::string base = resource_base(section, file);
        if (type == "SpriteFrames") {
            return frames_file(file, section, base);
        }
        if (type == "TileSet") {
            std::string path = tile_set_file(file, section, base).path;
            if (path.empty()) {
                why = "a tile set with no atlas source that converts";
            }
            return path;
        }
        if (type == "Theme") {
            return theme_file(file, section, base);
        }
        why = type + " resources";
        return {};
    }

    // The path a resource's converted file is named after, without its suffix.
    static std::string resource_base(const Section& section, const TextFile& file) {
        return section.tag == "resource"
                   ? without_extension(file.path)
                   : without_extension(file.path) + "_" + snake_case(section.field_string("id"));
    }

    // Where a resource is declared, for reports.
    static std::string resource_where(const Section& section, const TextFile& file) {
        return section.tag == "resource" ? file.path
                                         : file.path + ": " + section.field_string("id");
    }

    // Keeps only what validates, reporting the rest, so one bad value can't stop a file.
    template <typename Validate>
    void keep_valid(json::Value& doc, const std::string& path, Validate validate) {
        for (int pass = 0; pass < 64; ++pass) {
            const std::vector<json::ValidationError> errors = validate(doc);
            if (errors.empty()) {
                return;
            }
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", path + ": " + error.path, error.message);
                if (!remove_pointer(doc, error.path)) {
                    return;
                }
            }
        }
    }

    // --- Tile sets and tile maps ---

    // A tile set as converted: its file, its sources' names by Godot's source IDs, and what it
    // holds, for writing cells.
    struct TileSetFile {
        std::string path;
        std::map<std::int64_t, std::string> sources;
        std::optional<canvas::TileSet> set;
    };

    // Writes a TileSet resource as a .tileset.json file once.
    TileSetFile& tile_set_file(const TextFile& file, const Section& resource,
                               const std::string& base) {
        const std::string key = file.path + "#" + base;
        if (const auto done = tile_sets_.find(key); done != tile_sets_.end()) {
            return done->second;
        }
        TileSetFile& out = tile_sets_[key];
        out.path = destination(base + std::string(canvas::kTileSetSuffix));
        resources_used_.insert("TileSet");
        const std::string where = resource_where(resource, file);
        json::Value doc;
        doc["format"] = 1;
        std::array<std::int64_t, 2> tile_size{16, 16};
        // Terrain sets by index: their modes and their terrains' names by index.
        std::map<std::int64_t, std::int64_t> modes;
        std::map<std::pair<std::int64_t, std::int64_t>, std::string> terrains;
        std::vector<std::pair<std::int64_t, const Value*>> sources;
        for (const auto& [name, value] : resource.properties) {
            const std::vector<std::string_view> parts = split_path(name);
            if (name == "resource_name" || name == "resource_local_to_scene") {
                continue;
            }
            if (name == "tile_size") {
                if (const auto xy = value.numbers(); xy && xy->size() == 2) {
                    tile_size = {static_cast<std::int64_t>(xy->at(0)),
                                 static_cast<std::int64_t>(xy->at(1))};
                }
            } else if (name == "tile_shape") {
                if (value.number() != 0.0) {
                    problems_.add("tile shape", where,
                                  "only square tiles convert; isometric and hexagonal tile maps "
                                  "are on the roadmap");
                }
            } else if (parts.size() == 2 && parts[0] == "sources") {
                if (const auto id = parse_int(parts[1])) {
                    sources.emplace_back(*id, &value);
                }
            } else if (parts[0].starts_with("terrain_set_")) {
                const std::int64_t set =
                    parse_int(parts[0].substr(std::string_view("terrain_set_").size()))
                        .value_or(-1);
                const bool named = parts.size() == 3 && parts[1].starts_with("terrain_");
                if (set >= 0 && parts.size() == 2 && parts[1] == "mode") {
                    modes[set] = static_cast<std::int64_t>(value.number());
                } else if (set >= 0 && named && parts[2] == "name") {
                    if (const auto terrain =
                            parse_int(parts[1].substr(std::string_view("terrain_").size()))) {
                        terrains[{set, *terrain}] = value.text;
                    }
                } else if (set < 0 || !named || parts[2] != "color") {
                    problems_.add("TileSet." + name, where);
                }
            } else if (parts[0].starts_with("physics_layer_")) {
                problems_.add("tile collision", where,
                              "tile set collision is on the roadmap; add bodies over the tiles "
                              "meanwhile");
            } else if (parts[0].starts_with("navigation_layer_") ||
                       parts[0].starts_with("occlusion_layer_") ||
                       parts[0].starts_with("custom_data_layer_")) {
                problems_.add("tile set " + std::string(parts[0].substr(0, parts[0].rfind('_'))) +
                                  "s",
                              where);
            } else {
                problems_.add("TileSet." + name, where);
            }
        }
        if (tile_size != std::array<std::int64_t, 2>{16, 16}) {
            doc["tile_size"] = json::Array{tile_size[0], tile_size[1]};
        }
        // Terrain names must be unique in the file; Godot's need only be unique in their set.
        std::set<std::string> taken;
        for (auto& [index, name] : terrains) {
            if (name.empty()) {
                name =
                    "terrain_" + std::to_string(index.first) + "_" + std::to_string(index.second);
            }
            if (!taken.insert(name).second) {
                name += "_" + std::to_string(index.first);
                taken.insert(name);
            }
            json::Value& set = doc["terrain_sets"]["set_" + std::to_string(index.first)];
            set["terrains"].push_back(name);
            static constexpr std::array<std::string_view, 3> kModes{"match_corners_and_sides",
                                                                    "match_corners", "match_sides"};
            const std::int64_t mode = modes[index.first];
            if (mode > 0 && mode < 3) {
                set["mode"] = kModes.at(static_cast<std::size_t>(mode));
            }
        }
        std::set<std::string> names;
        for (const auto& [id, reference] : sources) {
            const auto [section, owner] = resource_section(*reference, file);
            const std::string type = section != nullptr ? resource_type(*section, *owner) : "";
            if (type != "TileSetAtlasSource") {
                problems_.add("tile set source " + (type.empty() ? "missing" : type), where,
                              type == "TileSetScenesCollectionSource"
                                  ? "scene tiles: place the scenes as entities instead"
                                  : "");
                continue;
            }
            json::Value source = atlas_source(*section, *owner, tile_size, terrains, modes);
            if (source.is_null()) {
                continue;
            }
            std::string name = snake_case(file_stem(source.find("texture")->as_string()));
            if (name.empty() || names.contains(name)) {
                name = "source_" + std::to_string(id);
            }
            names.insert(name);
            out.sources[id] = name;
            doc["sources"][name] = std::move(source);
        }
        if (doc.find("sources") == nullptr) {
            problems_.add("tile set with no atlas sources", where);
            out.path.clear();
            return out;
        }
        keep_valid(doc, out.path,
                   [](const json::Value& value) { return canvas::validate_tileset(value); });
        if (auto parsed = canvas::parse_tileset(doc); parsed) {
            out.set = std::move(parsed.value());
        }
        add_file(out.path, json::write(doc) + "\n");
        ++tile_set_files_;
        return out;
    }

    // One of a tile set's atlas sources, or null if its texture can't come along.
    json::Value
    atlas_source(const Section& section, const TextFile& file,
                 const std::array<std::int64_t, 2>& tile_size,
                 const std::map<std::pair<std::int64_t, std::int64_t>, std::string>& terrains,
                 const std::map<std::int64_t, std::int64_t>& modes) {
        const std::string where = resource_where(section, file);
        json::Value source;
        std::string why;
        const Value* texture = section.property("texture");
        const std::string texture_path =
            texture != nullptr ? resource_path(*texture, file, why) : "";
        if (texture_path.empty()) {
            problems_.add("tile set source texture", where, why.empty() ? "no texture" : why);
            return nullptr;
        }
        source["texture"] = texture_path;
        std::array<std::int64_t, 2> region{16, 16}; // Godot's default, whatever the tile size
        struct Tile {
            json::Value json;
            std::int64_t set = -1;
            std::int64_t terrain = -1;
            std::map<std::size_t, std::int64_t> bits;
        };
        std::map<std::pair<std::int64_t, std::int64_t>, Tile> tiles; // by row, then column
        for (const auto& [name, value] : section.properties) {
            if (name == "texture" || name == "use_texture_padding" || name == "resource_name" ||
                name == "resource_local_to_scene") {
                continue;
            }
            if (name == "texture_region_size" || name == "margins" || name == "separation") {
                const auto xy = value.numbers();
                if (!xy || xy->size() != 2) {
                    continue;
                }
                const std::array<std::int64_t, 2> pair{static_cast<std::int64_t>(xy->at(0)),
                                                       static_cast<std::int64_t>(xy->at(1))};
                if (name == "texture_region_size") {
                    region = pair;
                } else if (pair[0] != 0 || pair[1] != 0) {
                    source[name] = json::Array{pair[0], pair[1]};
                }
                continue;
            }
            // "column:row/0/terrain" and the like.
            const std::size_t colon = name.find(':');
            const std::size_t slash = name.find('/');
            const auto column = parse_int(std::string_view(name).substr(0, colon));
            const auto row =
                colon < slash
                    ? parse_int(std::string_view(name).substr(colon + 1, slash - colon - 1))
                    : std::nullopt;
            if (!column || !row || slash == std::string::npos) {
                problems_.add("TileSetAtlasSource." + name, where);
                continue;
            }
            const std::string_view rest = std::string_view(name).substr(slash + 1);
            const std::vector<std::string_view> parts = split_path(rest);
            Tile& tile = tiles[{*row, *column}];
            if (rest == "size_in_atlas") {
                if (const auto xy = value.numbers(); xy && (xy->at(0) != 1.0 || xy->at(1) != 1.0)) {
                    problems_.add("tiles larger than one cell", where,
                                  "each tile covers one cell; split the tile in the texture");
                }
            } else if (rest.starts_with("animation_")) {
                problems_.add("animated tiles", where, "each tile shows its first frame");
            } else if (rest == "next_alternative_id" || parts[0] != "0") {
                // Alternative tiles: cells that use one get its base tile.
            } else if (parts.size() == 2 && parts[1] == "terrain_set") {
                tile.set = static_cast<std::int64_t>(value.number());
            } else if (parts.size() == 2 && parts[1] == "terrain") {
                tile.terrain = static_cast<std::int64_t>(value.number());
            } else if (parts.size() == 3 && parts[1] == "terrains_peering_bit") {
                if (const auto direction = peering_direction(parts[2])) {
                    tile.bits[*direction] = static_cast<std::int64_t>(value.number());
                }
            } else if (parts.size() == 2 && parts[1] == "probability") {
                tile.json["probability"] = value.number();
            } else if (parts.size() > 1 && parts[1].starts_with("physics_layer_")) {
                problems_.add("tile collision", where,
                              "tile set collision is on the roadmap; add bodies over the tiles "
                              "meanwhile");
            } else if (parts.size() > 1) {
                problems_.add("tile property " + std::string(parts[1]), where);
            }
        }
        if (region != tile_size) {
            source["region_size"] = json::Array{region[0], region[1]};
        }
        for (auto& [coords, tile] : tiles) {
            const auto own = terrains.find({tile.set, tile.terrain});
            if (own != terrains.end()) {
                tile.json["terrain"] = own->second;
                const auto mode = modes.find(tile.set);
                json::Value peering = json::Object{};
                for (std::size_t direction = 0; direction < canvas::kNeighborNames.size();
                     ++direction) {
                    if (!connects(mode != modes.end() ? mode->second : 0, direction)) {
                        continue;
                    }
                    const auto bit = tile.bits.find(direction);
                    const std::int64_t terrain = bit != tile.bits.end() ? bit->second : -1;
                    if (terrain == tile.terrain) {
                        continue; // left out: the tile's own terrain
                    }
                    const auto other = terrains.find({tile.set, terrain});
                    peering[canvas::kNeighborNames.at(direction)] =
                        other != terrains.end() ? json::Value(other->second) : json::Value(nullptr);
                }
                if (peering.size() > 0) {
                    tile.json["peering"] = std::move(peering);
                }
            }
            if (tile.json.is_null()) {
                tile.json = json::Object{}; // a plain tile
            }
            source["tiles"][std::to_string(coords.second) + "," + std::to_string(coords.first)] =
                std::move(tile.json);
        }
        return source;
    }

    // Godot's cells as a TileMapLayer's origin, legend and rows, through the tile set they use.
    void write_cells(const std::vector<GodotCell>& cells, const TileSetFile& tiles,
                     json::Value& layer, const std::string& where) {
        if (!tiles.set) {
            problems_.add("tile map cells", where, "its tile set didn't convert");
            return;
        }
        canvas::Cells converted;
        for (const GodotCell& cell : cells) {
            const auto source = tiles.sources.find(cell.source);
            const canvas::TileRef tile{.source = source != tiles.sources.end() ? source->second
                                                                               : std::string(),
                                       .atlas = {cell.atlas_x, cell.atlas_y}};
            if (source == tiles.sources.end() || tiles.set->find(tile) == nullptr) {
                problems_.add("cells with tiles missing from their tile set", where);
                continue;
            }
            if (cell.alternative != 0) {
                problems_.add("alternative tiles", where,
                              "flipped, turned and other alternative tiles draw as their base "
                              "tile");
            }
            converted[{cell.x, cell.y}] = tile;
        }
        world::TileMapLayer encoded;
        canvas::encode_cells(converted, *tiles.set, encoded);
        layer["origin"] = json::Array{encoded.origin.x, encoded.origin.y};
        json::Object legend;
        for (const auto& [key, tile] : encoded.legend) {
            legend.emplace(key, tile);
        }
        layer["legend"] = std::move(legend);
        json::Array rows;
        for (const std::string& row : encoded.rows) {
            rows.emplace_back(row);
        }
        layer["rows"] = std::move(rows);
    }

    // The tile set a node's tile_set property names, or nullptr.
    const TileSetFile* node_tile_set(const Node& node) {
        const Property* property = node.find("tile_set");
        if (property == nullptr) {
            problems_.add("tile map with no tile set", node.declared);
            return nullptr;
        }
        const auto [section, owner] = resource_section(*property->value, *property->file);
        if (section == nullptr || resource_type(*section, *owner) != "TileSet") {
            return nullptr; // reported with the tile_set property
        }
        const TileSetFile& tiles = tile_set_file(*owner, *section, resource_base(*section, *owner));
        return tiles.path.empty() ? nullptr : &tiles;
    }

    // A TileMap's layers. Each is an entity in the node's Transform2D children, drawn before
    // them as Godot draws its layers, with the node's tile set and its own settings; a layer
    // with no settings of its own that would reach the node's children goes on the node itself.
    // layer_id gives a layer's entity ID from its index and name.
    void convert_tile_map(const Node& node, json::Object& components,
                          std::vector<std::pair<std::string, json::Value>>& extra,
                          const std::function<std::string(std::int64_t, std::string)>& layer_id) {
        if (const Property* format = node.find("format");
            format != nullptr && format->value->number() != 2.0) {
            problems_.add("TileMap data format", node.declared,
                          "an old format: open the scene in Godot 4 and save it again");
            return;
        }
        std::map<std::int64_t, std::map<std::string, const Property*>> layers;
        for (const Property& property : node.properties) {
            const std::vector<std::string_view> parts = split_path(property.name);
            if (parts.size() == 2 && parts[0].starts_with("layer_") &&
                node.find(property.name) == &property) {
                if (const auto index = parse_int(parts[0].substr(6))) {
                    layers[*index][std::string(parts[1])] = &property;
                }
            }
        }
        const TileSetFile* tiles = nullptr;
        json::Value& own = components["TileMapLayer"];
        for (const auto& [index, values] : layers) {
            json::Value layer = json::Object{};
            const std::string where = node.declared + " layer " + std::to_string(index);
            for (const auto& [key, property] : values) {
                const Value& value = *property->value;
                if (key == "tile_data") {
                    const auto cells = tile_map_cells(value);
                    if (!cells) {
                        problems_.add("TileMap layer data", where, "not whole cells");
                        continue;
                    }
                    if (tiles == nullptr && !cells->empty()) {
                        tiles = node_tile_set(node);
                    }
                    if (tiles != nullptr) {
                        write_cells(*cells, *tiles, layer, where);
                    }
                } else if (key == "enabled") {
                    if (value.kind == Value::Kind::Bool && !value.boolean) {
                        layer["visible"] = false;
                    }
                } else if (key == "modulate") {
                    if (const auto color = value.numbers(); color && color->size() == 4) {
                        layer["modulate"] =
                            json::Array{color->at(0), color->at(1), color->at(2), color->at(3)};
                    }
                } else if (key == "z_index" && value.is_number()) {
                    layer["z_index"] = static_cast<std::int64_t>(value.number());
                } else if (key != "name" && key != "navigation_enabled") {
                    problems_.add("TileMap layer " + key, where);
                }
            }
            const bool settings = layer.find("visible") != nullptr ||
                                  layer.find("modulate") != nullptr ||
                                  layer.find("z_index") != nullptr;
            if (index == 0 && layers.size() == 1 && !settings) {
                for (auto& [field, item] : layer.as_object()) {
                    own[field] = std::move(item);
                }
                continue;
            }
            for (const char* shared : {"tile_set", "texture_filter", "light_mask"}) {
                if (const json::Value* item = own.find(shared); item != nullptr) {
                    layer[shared] = *item;
                }
            }
            std::string name = "layer_" + std::to_string(index);
            if (const auto found = values.find("name");
                found != values.end() && found->second->value->is_string() &&
                !snake_case(found->second->value->text).empty()) {
                name = snake_case(found->second->value->text);
            }
            std::string id = layer_id(index, node.id + "_" + name);
            json::Value entity;
            entity["components"]["Transform2D"] = json::Object{};
            entity["components"]["TileMapLayer"] = std::move(layer);
            components["Transform2D"]["children"].push_back(id);
            extra.emplace_back(std::move(id), std::move(entity));
        }
    }

    // --- Themes and bus layouts ---

    // Writes a Theme resource as a .theme.json file once and returns its path.
    std::string theme_file(const TextFile& file, const Section& resource, const std::string& base) {
        const std::string key = file.path + "#" + base;
        if (const auto done = themes_written_.find(key); done != themes_written_.end()) {
            return done->second;
        }
        const std::string path = destination(base + std::string(ui::kThemeSuffix));
        themes_written_[key] = path;
        resources_used_.insert("Theme");
        const std::string where = resource_where(resource, file);
        json::Value doc;
        doc["format"] = 1;
        for (const auto& [name, value] : resource.properties) {
            if (name == "resource_name" || name == "resource_local_to_scene") {
                continue;
            }
            std::string why;
            const std::vector<std::string_view> parts = split_path(name);
            if (name == "default_font") {
                if (std::string font = resource_path(value, file, why); !font.empty()) {
                    doc["default_font"] = std::move(font);
                } else {
                    problems_.add("Theme.default_font", where, why);
                }
            } else if (name == "default_font_size" && value.is_number()) {
                doc["default_font_size"] = static_cast<std::int64_t>(value.number());
            } else if (parts.size() == 2 && parts[1] == "base_type" && value.is_string()) {
                doc["types"][std::string(parts[0])]["base_type"] = value.text;
            } else if (parts.size() == 3) {
                const std::string kind(parts[1]);
                std::optional<json::Value> item;
                if (kind == "colors") {
                    item = guess(value, file, why);
                } else if ((kind == "constants" || kind == "font_sizes") && value.is_number()) {
                    item = json::Value(static_cast<std::int64_t>(value.number()));
                } else if (kind == "fonts" || kind == "icons") {
                    if (std::string item_path = resource_path(value, file, why);
                        !item_path.empty()) {
                        item = json::Value(std::move(item_path));
                    }
                } else if (kind == "styles") {
                    item = style_box(value, file, why);
                }
                if (item) {
                    doc["types"][std::string(parts[0])][kind][std::string(parts[2])] =
                        std::move(*item);
                } else {
                    std::string item_where = where;
                    item_where.append(": ").append(name);
                    problems_.add("theme item " + kind, item_where, why);
                }
            } else {
                problems_.add("Theme." + name, where);
            }
        }
        keep_valid(doc, path, [](const json::Value& value) { return ui::validate_theme(value); });
        add_file(path, json::write(doc) + "\n");
        ++theme_files_;
        return path;
    }

    // Writes an AudioBusLayout resource as a .bus_layout.json file once and returns its path.
    std::string bus_layout_file(const TextFile& file) {
        const std::string path =
            destination(without_extension(file.path) + std::string(audio::kBusLayoutSuffix));
        if (!bus_layouts_written_.insert(file.path).second) {
            return path;
        }
        resources_used_.insert("AudioBusLayout");
        std::map<std::int64_t, json::Value> buses;
        struct Slot {
            const Value* effect = nullptr;
            bool enabled = true;
        };
        std::map<std::int64_t, std::map<std::int64_t, Slot>> effects; // by bus, then slot
        for (const auto& [name, value] : file.resource->properties) {
            const std::vector<std::string_view> parts = split_path(name);
            const auto index =
                parts.size() >= 3 && parts[0] == "bus" ? parse_int(parts[1]) : std::nullopt;
            if (!index) {
                if (name != "resource_name" && name != "resource_local_to_scene") {
                    problems_.add("AudioBusLayout." + name, file.path);
                }
                continue;
            }
            json::Value& bus = buses[*index];
            const std::string_view field = parts[2];
            if (parts.size() == 5 && field == "effect") {
                const auto slot = parse_int(parts[3]);
                if (slot && parts[4] == "effect") {
                    effects[*index][*slot].effect = &value;
                } else if (slot && parts[4] == "enabled") {
                    effects[*index][*slot].enabled =
                        value.kind != Value::Kind::Bool || value.boolean;
                }
            } else if (field == "name" || field == "send") {
                if (value.is_string() && !value.text.empty()) {
                    bus[std::string(field)] = value.text;
                }
            } else if (field == "solo" || field == "mute" || field == "bypass_fx") {
                if (value.kind == Value::Kind::Bool && value.boolean) {
                    bus[field == "bypass_fx" ? "bypass_effects" : std::string(field)] = true;
                }
            } else if (field == "volume_db" && value.is_number()) {
                if (value.number() != 0.0) {
                    bus["volume_db"] = value.number();
                }
            } else if (parts.size() == 3) {
                problems_.add("bus property " + std::string(field), file.path);
            }
        }
        json::Array list;
        buses[0]; // Master, which Godot always has
        for (auto& [index, bus] : buses) {
            if (bus.find("name") == nullptr) {
                bus["name"] = index == 0 ? std::string("Master") : "Bus " + std::to_string(index);
            }
            const json::Value* send = bus.find("send");
            if (index == 0 || (send != nullptr && send->as_string() == "Master")) {
                bus.as_object().erase("send");
            }
            for (const auto& [slot, effect] : effects[index]) {
                std::string why = "no effect";
                auto converted = effect.effect != nullptr ? audio_effect(*effect.effect, file, why)
                                                          : std::nullopt;
                if (!converted) {
                    problems_.add("audio effect", file.path + ": bus " + std::to_string(index),
                                  why);
                    continue;
                }
                if (!effect.enabled) {
                    (*converted)["enabled"] = false;
                }
                bus["effects"].push_back(std::move(*converted));
            }
            list.push_back(std::move(bus));
        }
        json::Value doc;
        doc["format"] = 1;
        doc["buses"] = std::move(list);
        keep_valid(doc, path,
                   [](const json::Value& value) { return audio::validate_bus_layout(value); });
        add_file(path, json::write(doc) + "\n");
        ++bus_layout_files_;
        return path;
    }

    // One of Godot's audio effects as a bus layout's effect, or nullopt with why.
    std::optional<json::Value> audio_effect(const Value& reference, const TextFile& file,
                                            std::string& why) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr) {
            why = "a missing effect";
            return std::nullopt;
        }
        static const std::map<std::string, std::string, std::less<>> kTypes{
            {"AudioEffectAmplify", "amplify"},
            {"AudioEffectLowPassFilter", "low_pass_filter"},
            {"AudioEffectHighPassFilter", "high_pass_filter"},
            {"AudioEffectBandPassFilter", "band_pass_filter"},
            {"AudioEffectNotchFilter", "notch_filter"},
            {"AudioEffectBandLimitFilter", "band_limit_filter"},
            {"AudioEffectLowShelfFilter", "low_shelf_filter"},
            {"AudioEffectHighShelfFilter", "high_shelf_filter"},
            {"AudioEffectEQ6", "eq6"},
            {"AudioEffectEQ10", "eq10"},
            {"AudioEffectEQ21", "eq21"},
            {"AudioEffectDelay", "delay"},
            {"AudioEffectReverb", "reverb"},
            {"AudioEffectChorus", "chorus"},
            {"AudioEffectPhaser", "phaser"},
            {"AudioEffectDistortion", "distortion"},
            {"AudioEffectCompressor", "compressor"},
            {"AudioEffectHardLimiter", "hard_limiter"},
            {"AudioEffectPanner", "panner"},
            {"AudioEffectStereoEnhance", "stereo_enhance"},
        };
        const std::string type = resource_type(*section, *owner);
        const auto found = kTypes.find(type);
        if (found == kTypes.end()) {
            why = type + " has no SlashEngine equivalent yet";
            return std::nullopt;
        }
        json::Value effect;
        effect["type"] = found->second;
        static constexpr std::array<std::string_view, 5> kDistortion{"clip", "atan", "lofi",
                                                                     "overdrive", "waveshape"};
        for (const auto& [name, value] : section->properties) {
            const std::vector<std::string_view> parts = split_path(name);
            if (name == "resource_name" || name == "resource_local_to_scene") {
                continue;
            }
            if (name == "db" && value.kind == Value::Kind::Int) {
                effect["db"] = (value.integer + 1) * 6; // Godot's FilterDB counts stages from 0
            } else if (name == "mode" && type == "AudioEffectDistortion" &&
                       value.kind == Value::Kind::Int && value.integer >= 0 && value.integer < 5) {
                effect["mode"] = kDistortion.at(static_cast<std::size_t>(value.integer));
            } else if (parts.size() == 2 && parts[0] == "band_db") {
                effect["band_db"][std::string(parts[1])] = value.number();
            } else if (parts.size() == 3 && parts[0] == "voice") {
                const std::int64_t voice = parse_int(parts[1]).value_or(0); // from 1 to 4
                if (voice < 1 || voice > 4) {
                    continue;
                }
                json::Value& voices = effect["voices"];
                while (std::cmp_less(voices.size(), voice)) {
                    voices.push_back(json::Object{});
                }
                voices.as_array()[static_cast<std::size_t>(voice - 1)][std::string(parts[2])] =
                    value.number();
            } else if (value.kind == Value::Kind::Bool) {
                effect[name] = value.boolean;
            } else if (value.kind == Value::Kind::Int && name == "voice_count") {
                effect[name] = value.integer;
            } else if (value.is_number()) {
                effect[name] = value.number();
            } else if (value.is_string()) {
                effect[name] = value.text;
            } else {
                std::string what = type;
                what.append(".").append(name);
                problems_.add(what, resource_where(*section, *owner));
            }
        }
        return effect;
    }

    // The section a resource reference names: a sub-resource, or an external .tres file's
    // [resource], with the file its own references point into.
    std::pair<const Section*, const TextFile*> resource_section(const Value& reference,
                                                                const TextFile& file) {
        const std::string id = reference.string_argument().value_or("");
        if (reference.is_call("SubResource")) {
            const auto sub = file.sub.find(id);
            return sub != file.sub.end() ? std::pair{sub->second, &file}
                                         : std::pair<const Section*, const TextFile*>{};
        }
        if (reference.is_call("ExtResource")) {
            if (const auto ext = file.ext.find(id); ext != file.ext.end()) {
                if (const auto it = text_files_.find(ext->second.path);
                    it != text_files_.end() && it->second->resource != nullptr) {
                    resources_read_.insert(ext->second.path);
                    return {it->second->resource, it->second.get()};
                }
            }
        }
        return {};
    }

    static std::string resource_type(const Section& section, const TextFile& file) {
        return section.tag == "resource" ? file.type : section.field_string("type");
    }

    // Writes a SpriteFrames resource as a .frames.json file once and returns its path.
    std::string frames_file(const TextFile& file, const Section& resource,
                            const std::string& base) {
        const std::string key = file.path + "#" + base;
        if (const auto done = frames_written_.find(key); done != frames_written_.end()) {
            return done->second;
        }
        const std::string path = destination(base + std::string(kFramesSuffix));
        frames_written_[key] = path;
        resources_used_.insert("SpriteFrames");
        json::Object animations;
        const Value* list = resource.property("animations");
        for (const Value& animation : list != nullptr ? list->items : std::vector<Value>{}) {
            const Value* name = animation.get("name");
            if (name == nullptr || !name->is_string()) {
                continue;
            }
            json::Value converted;
            if (const Value* speed = animation.get("speed"); speed != nullptr) {
                converted["fps"] = speed->number();
            }
            if (const Value* loop = animation.get("loop");
                loop != nullptr && loop->kind == Value::Kind::Bool && !loop->boolean) {
                converted["loop"] = false;
            }
            std::string why;
            if (!frames_of(animation, file, converted, why)) {
                problems_.add("sprite frames animation", file.path + ": " + name->text, why);
                continue;
            }
            animations.emplace(name->text, std::move(converted));
        }
        json::Value frames;
        frames["format"] = 1;
        frames["animations"] = std::move(animations);
        add_file(path, json::write(frames) + "\n");
        ++frames_files_;
        return path;
    }

    // An animation's frames: whole textures, or regions of one sheet laid out as a grid.
    bool frames_of(const Value& animation, const TextFile& file, json::Value& out,
                   std::string& why) {
        const Value* frames = animation.get("frames");
        if (frames == nullptr || frames->items.empty()) {
            out["frames"] = json::Array{};
            why = "an animation with no frames";
            return false;
        }
        json::Array paths;
        std::string atlas;
        std::vector<std::array<double, 4>> regions;
        bool uneven = false;
        for (const Value& frame : frames->items) {
            const Value* texture = frame.get("texture");
            if (const Value* duration = frame.get("duration");
                duration != nullptr && duration->number() != 1.0) {
                uneven = true;
            }
            if (texture == nullptr) {
                why = "a frame with no texture";
                return false;
            }
            if (texture->is_call("ExtResource")) {
                std::string path = resource_path(*texture, file, why);
                if (path.empty()) {
                    return false;
                }
                paths.emplace_back(std::move(path));
                continue;
            }
            const auto [section, owner] = resource_section(*texture, file);
            if (section == nullptr || resource_type(*section, *owner) != "AtlasTexture") {
                why = "frames that aren't textures or AtlasTextures";
                return false;
            }
            const Value* sheet = section->property("atlas");
            const Value* region = section->property("region");
            const auto rect = region != nullptr ? region->numbers() : std::nullopt;
            if (sheet == nullptr || !rect || rect->size() != 4) {
                why = "an AtlasTexture with no atlas or region";
                return false;
            }
            std::string sheet_path = resource_path(*sheet, *owner, why);
            if (sheet_path.empty()) {
                return false;
            }
            if (!atlas.empty() && atlas != sheet_path) {
                why = "frames from more than one sheet";
                return false;
            }
            atlas = std::move(sheet_path);
            regions.push_back({rect->at(0), rect->at(1), rect->at(2), rect->at(3)});
        }
        if (uneven) {
            problems_.add("sprite frame durations", file.path,
                          "every frame shows for the same time; frames that last longer can be "
                          "listed more than once");
        }
        if (!paths.empty() && !regions.empty()) {
            why = "frames mixing whole textures and AtlasTextures";
            return false;
        }
        if (regions.empty()) {
            out["frames"] = std::move(paths);
            return true;
        }
        // Regions of one size on a grid from the sheet's corner become frame numbers.
        const double width = regions.front()[2];
        const double height = regions.front()[3];
        const std::string sheet_file = strip_res(atlas.substr(prefix_.size()));
        auto bytes = platform::read_file(source_ + sheet_file);
        const auto size = bytes ? png_size(bytes.value()) : std::nullopt;
        if (!size || width <= 0.0 || height <= 0.0) {
            why = "a sheet whose size can't be read";
            return false;
        }
        const int hframes = static_cast<int>(size->first / width);
        const int vframes = static_cast<int>(size->second / height);
        json::Array numbers;
        for (const auto& region : regions) {
            const double column = region[0] / width;
            const double row = region[1] / height;
            if (region[2] != width || region[3] != height || column != static_cast<int>(column) ||
                row != static_cast<int>(row) || column >= hframes || row >= vframes) {
                why = "AtlasTexture regions that aren't one grid";
                return false;
            }
            numbers.emplace_back(static_cast<int>(row) * hframes + static_cast<int>(column));
        }
        out["texture"] = atlas;
        if (hframes != 1) {
            out["hframes"] = hframes;
        }
        if (vframes != 1) {
            out["vframes"] = vframes;
        }
        out["frames"] = std::move(numbers);
        return true;
    }

    // A Curve resource as [offset, value] points.
    std::optional<json::Value> curve_points(const Value& reference, const TextFile& file,
                                            std::string& why) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr || resource_type(*section, *owner) != "Curve") {
            why = "a curve that isn't a Curve resource";
            return std::nullopt;
        }
        const Value* data = section->property("_data");
        json::Array points;
        // Each point is Vector2(offset, value), left and right tangents and their modes.
        for (const Value& item : data != nullptr ? data->items : std::vector<Value>{}) {
            if (item.is_call("Vector2")) {
                if (const auto xy = item.numbers(); xy.has_value() && xy->size() >= 2) {
                    points.emplace_back(json::Array{(*xy)[0], (*xy)[1]});
                }
            }
        }
        return json::Value(std::move(points));
    }

    // A Gradient resource as GradientPoint values.
    std::optional<json::Value> gradient_points(const Value& reference, const TextFile& file,
                                               std::string& why) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr || resource_type(*section, *owner) != "Gradient") {
            why = "a color ramp that isn't a Gradient";
            return std::nullopt;
        }
        std::vector<double> offsets{0.0, 1.0};
        std::vector<double> colors{0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0}; // Godot's default
        if (const Value* value = section->property("offsets"); value != nullptr) {
            offsets = value->numbers().value_or(std::vector<double>{});
        }
        if (const Value* value = section->property("colors"); value != nullptr) {
            colors = value->numbers().value_or(std::vector<double>{});
        }
        if (colors.size() != offsets.size() * 4) {
            why = "a Gradient whose offsets and colors don't match";
            return std::nullopt;
        }
        json::Array points;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            json::Value point;
            point["offset"] = offsets[i];
            point["color"] = json::Array{colors[i * 4], colors[(i * 4) + 1], colors[(i * 4) + 2],
                                         colors[(i * 4) + 3]};
            points.push_back(std::move(point));
        }
        return json::Value(std::move(points));
    }

    // A style box resource in a theme's form: {"type": "flat", ...Godot's properties}.
    std::optional<json::Value> style_box(const Value& reference, const TextFile& file,
                                         std::string& why) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr) {
            why = "a missing style box";
            return std::nullopt;
        }
        const std::string type = resource_type(*section, *owner);
        json::Value style;
        if (type == "StyleBoxFlat") {
            style["type"] = "flat";
        } else if (type == "StyleBoxTexture") {
            style["type"] = "texture";
        } else if (type == "StyleBoxEmpty") {
            style["type"] = "empty";
        } else {
            why = type + " style boxes";
            return std::nullopt;
        }
        static constexpr std::array<std::string_view, 3> kStretch{"stretch", "tile", "tile_fit"};
        for (const auto& [name, value] : section->properties) {
            if (ignored_property(name) || name == "resource_local_to_scene" ||
                name == "resource_name") {
                continue;
            }
            if ((name == "axis_stretch_horizontal" || name == "axis_stretch_vertical") &&
                value.kind == Value::Kind::Int && value.integer >= 0 && value.integer < 3) {
                style[name] = kStretch.at(static_cast<std::size_t>(value.integer));
                continue;
            }
            std::string field_why;
            if (auto converted = guess(value, *owner, field_why); converted) {
                style[name] = std::move(*converted);
            } else {
                problems_.add("style box property " + name, owner->path, field_why);
            }
        }
        return style;
    }

    // A value for a component field, given its default to say its type.
    std::optional<json::Value> field_value(std::string_view component, std::string_view field,
                                           const json::Value& fallback, const Value& value,
                                           const TextFile& file, const SceneContext& scene,
                                           std::string& why) {
        const world::ComponentType* type = components_.find(component);
        const world::FieldMeta* meta = type != nullptr ? type->field_meta(field) : nullptr;
        if (fallback.is_bool()) {
            if (value.kind != Value::Kind::Bool) {
                why = "not a bool";
                return std::nullopt;
            }
            return json::Value(value.boolean);
        }
        if (fallback.is_number()) {
            if (!value.is_number()) {
                why = "not a number";
                return std::nullopt;
            }
            if (fallback.is_int() && value.kind == Value::Kind::Int) {
                return json::Value(value.integer);
            }
            return json::Value(value.number());
        }
        if (fallback.is_string()) {
            if (value.kind == Value::Kind::Int) {
                if (field.starts_with("size_flags_")) {
                    return json::Value(size_flags(value.integer));
                }
                const std::vector<std::string_view>* names = enum_names(component, field);
                std::vector<std::string_view> choices;
                if (names == nullptr && meta != nullptr) {
                    choices.assign(meta->choices.begin(), meta->choices.end());
                    names = &choices;
                }
                if (names != nullptr && value.integer >= 0 &&
                    value.integer < static_cast<std::int64_t>(names->size())) {
                    const std::string_view name =
                        names->at(static_cast<std::size_t>(value.integer));
                    if (!name.empty() && (meta == nullptr || meta->choices.empty() ||
                                          contains(meta->choices, name))) {
                        return json::Value(name);
                    }
                }
                why = "value " + std::to_string(value.integer) + " has no equivalent";
                return std::nullopt;
            }
            if (value.is_string()) {
                return json::Value(value.text);
            }
            if (value.is_call("ExtResource") || value.is_call("SubResource")) {
                std::string path = resource_path(value, file, why);
                return path.empty() ? std::nullopt : std::optional<json::Value>(std::move(path));
            }
            why = "not a string or a resource";
            return std::nullopt;
        }
        if (fallback.is_null()) { // an entity
            std::string path;
            if (value.is_call("NodePath")) {
                path = value.string_argument().value_or("");
            } else if (value.is_string()) {
                path = value.text;
            }
            if (path.empty()) {
                return json::Value(nullptr);
            }
            std::string id = resolve_node_path(*scene.nodes, *scene.node, path);
            if (id.empty()) {
                why = "node path " + path + " names no node in the scene";
                return std::nullopt;
            }
            return json::Value(std::move(id));
        }
        if (fallback.is_array()) {
            if (field.ends_with("_curve") &&
                (value.is_call("SubResource") || value.is_call("ExtResource"))) {
                return curve_points(value, file, why);
            }
            if ((field == "color_ramp" || field == "color_initial_ramp") &&
                (value.is_call("SubResource") || value.is_call("ExtResource"))) {
                return gradient_points(value, file, why);
            }
            if (field == "button_mask" && value.kind == Value::Kind::Int) {
                json::Array buttons;
                for (const auto& [bit, name] :
                     {std::pair{1, "left"}, std::pair{2, "right"}, std::pair{4, "middle"}}) {
                    if ((value.integer & bit) != 0) {
                        buttons.emplace_back(name);
                    }
                }
                return json::Value(std::move(buttons));
            }
            auto converted = guess(value, file, why);
            if (converted && !fallback.as_array().empty() &&
                fallback.as_array().front().is_number() &&
                (!converted->is_array() ||
                 converted->as_array().size() != fallback.as_array().size())) {
                why = "a value of the wrong size";
                return std::nullopt;
            }
            return converted;
        }
        if (fallback.is_object()) {
            return guess(value, file, why);
        }
        why = "an unknown field type";
        return std::nullopt;
    }

    // Sets one Godot property on the entity's components; false if none takes it.
    bool set_property(const Property& property, json::Object& components,
                      const std::vector<std::string>& order, const SceneContext& scene,
                      std::string& why) {
        std::string field = property.name;
        std::string key; // for "theme_override_colors/font_color": the map's key
        if (const auto slash = field.find('/'); slash != std::string::npos) {
            key = field.substr(slash + 1);
            field.resize(slash);
        }
        for (const std::string& component : order) {
            const world::ComponentType* type = components_.find(component);
            if (type == nullptr || field == "children") {
                continue;
            }
            const json::Value* fallback = type->defaults().find(field);
            if (fallback == nullptr) {
                continue;
            }
            if (!key.empty()) {
                if (!fallback->is_object()) {
                    continue;
                }
                std::optional<json::Value> converted;
                if (field == "theme_override_styles") {
                    converted = style_box(*property.value, *property.file, why);
                } else {
                    converted = guess(*property.value, *property.file, why);
                }
                if (!converted) {
                    return true; // taken, but not convertible: reported by the caller
                }
                components[component][field][key] = std::move(*converted);
                return true;
            }
            auto converted = field_value(component, field, *fallback, *property.value,
                                         *property.file, scene, why);
            if (converted) {
                components[component][field] = std::move(*converted);
            }
            return true;
        }
        return false;
    }

    // Godot properties that don't match a field one to one.
    bool special_property(const Node& node, const Property& property, json::Object& components) {
        const std::string& name = property.name;
        const Value& value = *property.value;
        const TextFile& file = *property.file;
        std::string why;
        if (name == "region_enabled" || name == "region_rect" ||
            name == "region_filter_clip_enabled") {
            if (name == "region_rect" && components.contains("Sprite2D")) {
                const Property* enabled = node.find("region_enabled");
                const auto rect = value.numbers();
                if (enabled != nullptr && enabled->value->boolean && rect && rect->size() == 4) {
                    components["Sprite2D"]["region"] =
                        json::Array{rect->at(0), rect->at(1), rect->at(2), rect->at(3)};
                }
            }
            return components.contains("Sprite2D");
        }
        if (name == "shape" && components.contains("CollisionShape2D")) {
            convert_shape(value, file, components["CollisionShape2D"], node);
            return true;
        }
        if (name == "occluder" && components.contains("LightOccluder2D")) {
            const auto [section, owner] = resource_section(value, file);
            if (section == nullptr || resource_type(*section, *owner) != "OccluderPolygon2D") {
                problems_.add("LightOccluder2D occluder", node.declared,
                              "not an OccluderPolygon2D");
                return true;
            }
            json::Value& occluder = components["LightOccluder2D"];
            if (const Value* polygon = section->property("polygon"); polygon != nullptr) {
                if (auto points = guess(*polygon, *owner, why); points) {
                    occluder["polygon"] = std::move(*points);
                }
            }
            if (const Value* closed = section->property("closed");
                closed != nullptr && closed->kind == Value::Kind::Bool) {
                occluder["closed"] = closed->boolean;
            }
            static constexpr std::array<std::string_view, 3> kCull{"disabled", "clockwise",
                                                                   "counter_clockwise"};
            if (const Value* cull = section->property("cull_mode");
                cull != nullptr && cull->kind == Value::Kind::Int && cull->integer >= 0 &&
                cull->integer < 3) {
                occluder["cull_mode"] = kCull.at(static_cast<std::size_t>(cull->integer));
            }
            return true;
        }
        if (name == "autoplay" && components.contains("AnimatedSprite2D")) {
            if (value.is_string() && !value.text.empty()) {
                components["AnimatedSprite2D"]["animation"] = value.text;
                components["AnimatedSprite2D"]["playing"] = true;
            }
            return true;
        }
        if (name == "physics_material_override") {
            const auto [section, owner] = resource_section(value, file);
            for (const char* body : {"StaticBody2D", "AnimatableBody2D", "RigidBody2D"}) {
                if (!components.contains(body) || section == nullptr) {
                    continue;
                }
                for (const char* field : {"friction", "bounce"}) {
                    if (const Value* number = section->property(field);
                        number != nullptr && number->is_number()) {
                        components[body][field] = number->number();
                    }
                }
                return true;
            }
            return false;
        }
        if (name == "label_settings" && components.contains("Label")) {
            convert_label_settings(value, file, components["Control"], node);
            return true;
        }
        if (components.contains("TileMapLayer") && node.type == "TileMapLayer") {
            if (name == "tile_map_data") {
                const auto cells = layer_cells(value);
                if (!cells) {
                    problems_.add("TileMapLayer data", node.declared,
                                  "a format this importer can't read: save the scene in Godot 4.3 "
                                  "or later");
                } else if (!cells->empty()) {
                    if (const TileSetFile* tiles = node_tile_set(node); tiles != nullptr) {
                        write_cells(*cells, *tiles, components["TileMapLayer"], node.declared);
                    }
                }
                return true;
            }
            if (name == "enabled") {
                if (value.kind == Value::Kind::Bool && !value.boolean) {
                    components["TileMapLayer"]["visible"] = false;
                }
                return true;
            }
        }
        if (node.type == "TileMap" && (name == "format" || name.starts_with("layer_"))) {
            return true; // convert_tile_map reads them
        }
        const char* tinted = nullptr;
        if (components.contains("Sprite2D")) {
            tinted = "Sprite2D";
        } else if (components.contains("TileMapLayer")) {
            tinted = "TileMapLayer";
        }
        if (name == "self_modulate" && tinted != nullptr) {
            // Sprite2D and TileMapLayer have one tint; Godot multiplies the two for the node
            // itself.
            const auto color = value.numbers();
            if (color && color->size() == 4) {
                json::Value& sprite = components[tinted];
                json::Array tint{1.0, 1.0, 1.0, 1.0};
                if (const json::Value* modulate = sprite.find("modulate"); modulate != nullptr) {
                    tint = modulate->as_array();
                }
                for (std::size_t i = 0; i < 4; ++i) {
                    tint[i] = tint[i].as_double() * color->at(i);
                }
                sprite["modulate"] = std::move(tint);
            }
            return true;
        }
        return false;
    }

    void convert_shape(const Value& reference, const TextFile& file, json::Value& shape,
                       const Node& node) {
        const auto resource = resource_section(reference, file);
        const Section* section = resource.first;
        if (section == nullptr) {
            problems_.add("collision shape", node.declared, "a missing shape resource");
            return;
        }
        const std::string type = resource_type(*section, *resource.second);
        const auto vector = [&](std::string_view property) -> std::optional<json::Value> {
            const Value* value = section->property(property);
            const auto xy = value != nullptr ? value->numbers() : std::nullopt;
            if (!xy || xy->size() != 2) {
                return std::nullopt;
            }
            return json::Array{xy->at(0), xy->at(1)};
        };
        const auto number = [&](std::string_view property) -> std::optional<double> {
            const Value* value = section->property(property);
            return value != nullptr && value->is_number() ? std::optional(value->number())
                                                          : std::nullopt;
        };
        if (type == "RectangleShape2D") {
            shape["shape"] = "rectangle";
            if (auto size = vector("size")) {
                shape["size"] = std::move(*size);
            }
        } else if (type == "CircleShape2D") {
            shape["shape"] = "circle";
            if (auto radius = number("radius")) {
                shape["radius"] = *radius;
            }
        } else if (type == "CapsuleShape2D") {
            shape["shape"] = "capsule";
            if (auto radius = number("radius")) {
                shape["radius"] = *radius;
            }
            if (auto height = number("height")) {
                shape["height"] = *height;
            }
        } else if (type == "SegmentShape2D") {
            shape["shape"] = "segment";
            if (auto a = vector("a")) {
                shape["a"] = std::move(*a);
            }
            if (auto b = vector("b")) {
                shape["b"] = std::move(*b);
            }
        } else if (type == "ConvexPolygonShape2D") {
            shape["shape"] = "polygon";
            std::string why;
            if (const Value* points = section->property("points"); points != nullptr) {
                if (auto converted = guess(*points, *resource.second, why)) {
                    shape["points"] = std::move(*converted);
                }
            }
        } else {
            problems_.add("collision shape " + type, node.declared,
                          type == "ConcavePolygonShape2D"
                              ? "use a CollisionPolygon2D with build_mode segments"
                              : "no SlashEngine shape for it yet");
        }
    }

    // LabelSettings as the label's theme overrides.
    void convert_label_settings(const Value& reference, const TextFile& file, json::Value& control,
                                const Node& node) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr) {
            problems_.add("label settings", node.declared, "a missing LabelSettings resource");
            return;
        }
        resources_used_.insert("LabelSettings");
        static const std::map<std::string, std::pair<std::string, std::string>, std::less<>> kItems{
            {"font", {"theme_override_fonts", "font"}},
            {"font_size", {"theme_override_font_sizes", "font_size"}},
            {"font_color", {"theme_override_colors", "font_color"}},
            {"outline_size", {"theme_override_constants", "outline_size"}},
            {"outline_color", {"theme_override_colors", "font_outline_color"}},
            {"shadow_color", {"theme_override_colors", "font_shadow_color"}},
            {"shadow_size", {"theme_override_constants", "shadow_outline_size"}},
            {"line_spacing", {"theme_override_constants", "line_spacing"}},
        };
        for (const auto& [name, value] : section->properties) {
            std::string why;
            if (name == "shadow_offset") {
                if (const auto xy = value.numbers(); xy && xy->size() == 2) {
                    control["theme_override_constants"]["shadow_offset_x"] =
                        static_cast<std::int64_t>(xy->at(0));
                    control["theme_override_constants"]["shadow_offset_y"] =
                        static_cast<std::int64_t>(xy->at(1));
                }
                continue;
            }
            const auto item = kItems.find(name);
            if (item == kItems.end()) {
                if (name != "resource_local_to_scene" && name != "resource_name") {
                    problems_.add("LabelSettings." + name, node.declared);
                }
                continue;
            }
            if (auto converted = guess(value, *owner, why); converted) {
                if (converted->is_double() && item->second.first != "theme_override_colors") {
                    converted = json::Value(static_cast<std::int64_t>(converted->as_double()));
                }
                control[item->second.first][item->second.second] = std::move(*converted);
            } else {
                problems_.add("LabelSettings." + name, node.declared, why);
            }
        }
    }

    // The .scene.json file a .tscn file converts to.
    [[nodiscard]] std::string scene_destination(const TextFile& file) const {
        return destination(without_extension(file.path) + std::string(world::kSceneSuffix));
    }

    // Reads the files this import has written so far, for checking scenes that instance them.
    [[nodiscard]] world::FileReader planned_files() const {
        return [this](std::string_view path) -> Result<std::string> {
            for (const auto& [planned, contents] : plan_.files) {
                if (planned == path) {
                    return contents;
                }
            }
            return make_error(ErrorCode::NotFound, "not converted");
        };
    }

    // The ID of an entity a node adds beside its own, such as a TileMap's layer, by a key
    // unique in the node: for a node an instance brings, the ID the scene it instances gave it,
    // under the instance's; else wanted, made unique in the scene.
    std::string added_id(const std::vector<Node>& nodes, const Node& node, const std::string& key,
                         std::string wanted, std::set<std::string>& taken,
                         const std::string& scene) {
        const Node* owner = node.instanced != nullptr ? &node : nullptr;
        if (owner == nullptr && node.instance_root >= 0) {
            owner = &nodes[static_cast<std::size_t>(node.instance_root)];
        }
        std::string id;
        if (owner != nullptr && owner->instanced != nullptr) {
            std::string relative = ".";
            if (owner != &node) {
                relative =
                    owner->path == "." ? node.path : node.path.substr(owner->path.size() + 1);
            }
            if (const auto ids = scene_ids_.find(owner->instanced->path); ids != scene_ids_.end()) {
                if (const auto found = ids->second.find(relative + ":" + key);
                    found != ids->second.end()) {
                    id = owner->id + "/" + found->second;
                }
            }
        }
        if (id.empty()) {
            wanted.resize(std::min<std::size_t>(wanted.size(), 64));
            id = wanted;
            for (int n = 2; taken.contains(id); ++n) {
                id = wanted.substr(0, 60) + "_" + std::to_string(n);
            }
        }
        taken.insert(id);
        scene_ids_[scene][node.path + ":" + key] = id;
        return id;
    }

    void convert_scene(const TextFile& file) {
        if (!scenes_started_.insert(file.path).second) {
            return;
        }
        // The scenes it instances first, so it can extend their converted files.
        for (const Section& section : file.sections) {
            const Value* instance = section.tag == "node" ? section.field("instance") : nullptr;
            if (instance == nullptr) {
                continue;
            }
            if (const auto ext = file.ext.find(instance->string_argument().value_or(""));
                ext != file.ext.end()) {
                if (const auto it = text_files_.find(ext->second.path); it != text_files_.end()) {
                    convert_scene(*it->second);
                }
            }
        }
        std::vector<Node> nodes;
        std::map<std::string, int> by_path;
        std::vector<std::string> stack;
        const int root = flatten(file, ".", nodes, by_path, stack);
        if (root < 0 || nodes.empty()) {
            problems_.add("scene with no nodes", file.path);
            return;
        }
        assign_ids(nodes);
        for (const Node& node : nodes) {
            scene_ids_[file.path][node.path] = node.id;
        }
        const std::string scene_path = scene_destination(file);
        SceneContext scene{.nodes = &nodes,
                           .node = nullptr,
                           .scene = scene_path,
                           .stem = without_extension(file.path)};

        // The family of each node, and whether a plain node holds its children in place: the
        // root, and plain nodes under it, keep Godot's drawing order that way.
        std::vector<Family> families(nodes.size(), Family::Plain);
        std::vector<bool> has_transform(nodes.size(), false);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeType* type = find_node_type(nodes[i].type);
            families[i] = type != nullptr ? type->family : guess_family(nodes[i]);
            if (type == nullptr) {
                problems_.add("node type " + nodes[i].type, nodes[i].declared,
                              unsupported_node(nodes[i].type));
            }
            // An instance's root is placed as the root of the scene it instances is.
            const int parent = nodes[i].instanced != nullptr ? -1 : nodes[i].parent;
            has_transform[i] =
                families[i] == Family::Node2D ||
                (families[i] == Family::Plain && nodes[i].type != "AudioStreamPlayer" &&
                 (parent < 0 || (families[static_cast<std::size_t>(parent)] == Family::Plain &&
                                 has_transform[static_cast<std::size_t>(parent)])));
        }

        json::Object entities;
        json::Value record;
        std::set<std::string> taken;
        for (const Node& node : nodes) {
            taken.insert(node.id);
        }
        std::vector<std::pair<std::string, json::Value>> added; // entities beyond the nodes'
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Node& node = nodes[i];
            scene.node = &node;
            json::Object components;
            std::vector<std::string> order;
            // Transform2D first, so a node's position goes there rather than to a field of the
            // same name, such as CollisionShape2D's.
            if (has_transform[i]) {
                order.emplace_back("Transform2D");
                components["Transform2D"] = json::Object{};
            }
            const NodeType* type = find_node_type(node.type);
            if (type != nullptr) {
                for (const std::string_view component : {type->first, type->second}) {
                    if (!component.empty()) {
                        order.emplace_back(component);
                        components[std::string(component)] = json::Object{};
                    }
                }
                if (type->vertical) {
                    components[std::string(type->first)]["vertical"] = true;
                }
            }
            if (families[i] == Family::Control) {
                order.emplace_back("Control");
                components["Control"] = json::Object{};
            }
            if (components.contains("AnimatedSprite2D")) {
                // Godot's plays only with autoplay or when code calls play().
                components["AnimatedSprite2D"]["playing"] = false;
            }
            json::Value values = json::Object{};  // a script's exported variables
            json::Value unknown = json::Object{}; // what a node type that doesn't convert set
            std::string script;
            if (const Property* property = node.find("script"); property != nullptr) {
                if (const auto ext =
                        property->file->ext.find(property->value->string_argument().value_or(""));
                    ext != property->file->ext.end()) {
                    script = ext->second.path;
                }
            }
            for (const Property& property : node.properties) {
                if (ignored_property(property.name)) {
                    continue;
                }
                if (node.find(property.name) != &property) {
                    continue; // a later value overrides it
                }
                if (special_property(node, property, components)) {
                    continue;
                }
                std::string why;
                if (set_property(property, components, order, scene, why)) {
                    if (!why.empty()) {
                        problems_.add(node.type + "." + property.name, node.declared, why);
                    }
                    continue;
                }
                if (!script.empty() && !built_in_property(property.name)) {
                    std::string ignored;
                    if (auto converted = guess(*property.value, *property.file, ignored)) {
                        values[property.name] = std::move(*converted);
                    }
                    continue;
                }
                if (type == nullptr) {
                    std::string ignored;
                    if (auto converted = guess(*property.value, *property.file, ignored)) {
                        unknown[property.name] = std::move(*converted);
                    }
                    continue;
                }
                problems_.add(node.type + "." + property.name, node.declared);
            }
            if (node.type == "TileMap") {
                convert_tile_map(node, components, added,
                                 [&](std::int64_t index, std::string wanted) {
                                     return added_id(nodes, node, "layer_" + std::to_string(index),
                                                     std::move(wanted), taken, file.path);
                                 });
            }
            if (type == nullptr) {
                json::Value& kept = record["unconverted"][node.id];
                kept["type"] = node.type;
                if (unknown.size() > 0) {
                    kept["values"] = std::move(unknown);
                }
            }
            if (!script.empty()) {
                ScriptUse use{.scene = scene_path, .entity = node.id, .values = std::move(values)};
                scripts_[script].uses.push_back(std::move(use));
            }
            for (const std::string& group : node.groups) {
                groups_[group].emplace_back(scene_path + "#" + node.id);
            }
            record["entities"][node.path] = node.id;
            json::Value entity;
            entity["components"] = std::move(components);
            // An instance extends its scene's file; saving leaves out what matches it.
            if (node.instanced != nullptr) {
                entity["extends"] = scene_destination(*node.instanced);
            }
            entities.emplace(node.id, std::move(entity));
        }
        for (auto& [id, entity] : added) {
            entities.emplace(id, std::move(entity));
        }

        // Children: Node2D under Node2D through Transform2D, Control under Control through
        // Control. Anything else stays where Godot would draw it, at the top.
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (const int child : nodes[i].children) {
                const auto c = static_cast<std::size_t>(child);
                json::Value& parent = entities[nodes[i].id]["components"];
                if (has_transform[i] && has_transform[c]) {
                    parent["Transform2D"]["children"].push_back(nodes[c].id);
                } else if (families[i] == Family::Control && families[c] == Family::Control) {
                    parent["Control"]["children"].push_back(nodes[c].id);
                } else if (families[i] == Family::Control && families[c] == Family::Node2D) {
                    problems_.add("Node2D under a Control", nodes[c].declared,
                                  "placed in the world instead of in the control's rect");
                } else if (families[i] == Family::Node2D && families[c] == Family::Control) {
                    problems_.add("Control under a Node2D", nodes[c].declared,
                                  "drawn on the screen over the world instead of in it");
                }
                if (nodes[i].type == "CanvasLayer" && families[c] == Family::Node2D) {
                    problems_.add("Node2D under a CanvasLayer", nodes[c].declared,
                                  "drawn with the world, through the camera");
                }
            }
        }

        // Connections, by entity.
        json::Array connections;
        for (const Section& section : file.sections) {
            if (section.tag != "connection") {
                continue;
            }
            const auto entity_of = [&](std::string_view path) {
                const auto it = by_path.find(std::string(path));
                return it != by_path.end() ? nodes[static_cast<std::size_t>(it->second)].id
                                           : std::string(path);
            };
            json::Value connection;
            connection["signal"] = section.field_string("signal");
            connection["from"] = entity_of(section.field_string("from"));
            connection["to"] = entity_of(section.field_string("to"));
            connection["method"] = section.field_string("method");
            connections.push_back(connection);
            const auto to = by_path.find(section.field_string("to"));
            if (to != by_path.end()) {
                const Node& target = nodes[static_cast<std::size_t>(to->second)];
                if (const Property* script = target.find("script"); script != nullptr) {
                    if (const auto ext =
                            script->file->ext.find(script->value->string_argument().value_or(""));
                        ext != script->file->ext.end()) {
                        connection["scene"] = scene_path;
                        scripts_[ext->second.path].connections.push_back(connection);
                    }
                }
            }
        }
        record["scene"] = scene_path;
        if (!connections.empty()) {
            record["connections"] = std::move(connections);
        }

        json::Value converted;
        converted["format"] = 1;
        converted["entities"] = std::move(entities);
        converted["root"] = nodes[static_cast<std::size_t>(root)].id;
        const world::FileReader reader = planned_files();
        // Leave out what doesn't validate, so one bad value can't stop the scene.
        for (int pass = 0; pass < 64; ++pass) {
            auto errors = world::validate_scene(components_, converted, reader);
            if (errors.empty()) {
                // A layer whose cells don't fit its tile set loses the whole component, not a
                // row, which would move the rows after it.
                errors = canvas::check_tile_layers(converted, reader);
                for (json::ValidationError& error : errors) {
                    constexpr std::string_view kLayer = "/components/TileMapLayer";
                    if (const auto at = error.path.find(kLayer); at != std::string::npos) {
                        error.path.resize(at + kLayer.size());
                    }
                }
            }
            if (errors.empty()) {
                break;
            }
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", file.path + ": " + error.path, error.message);
                if (!remove_pointer(converted, error.path)) {
                    converted["entities"] = json::Object{};
                    break;
                }
            }
        }
        auto text = world::format_scene(components_, json::write(converted), reader);
        add_file(scene_path, text ? text.value() : json::write(converted) + "\n");
        report_scenes_[file.path] = std::move(record);
        ++scenes_;
    }

    // --- Tasks and the report ---

    std::string task_id(const std::string& script) {
        std::string base = "task/port-" + snake_case(file_stem(script), 56);
        for (char& c : base) {
            if (c == '_') {
                c = '-';
            }
        }
        std::string id = base;
        for (int n = 2; task_ids_.contains(id); ++n) {
            id = base + "-" + std::to_string(n);
        }
        task_ids_.insert(id);
        return id;
    }

    void write_tasks(const std::optional<std::string>& existing) {
        json::Object entries;
        if (existing) {
            if (auto parsed = json::parse(*existing); parsed && parsed.value().is_object()) {
                if (const json::Value* list = parsed.value().find("entries");
                    list != nullptr && list->is_object()) {
                    entries = list->as_object();
                }
            }
        }
        for (const auto& [id, entry] : entries) {
            task_ids_.insert(id);
        }
        for (auto& [script, record] : scripts_) {
            std::string id;
            for (const auto& [existing_id, entry] : entries) {
                const json::Value* links = entry.find("links");
                if (links != nullptr && links->is_array() && !links->as_array().empty() &&
                    links->as_array().front().is_string() &&
                    links->as_array().front().as_string() == destination(script)) {
                    id = existing_id; // imported before: update it, keeping its status
                }
            }
            if (id.empty()) {
                id = task_id(script);
            }
            task_of_[script] = id;
            json::Value task = entries.contains(id) ? entries[id] : json::Value(json::Object{});
            const bool csharp = extension_of(script) == ".cs";
            task["name"] = "Port " + script + " to C++";
            task["summary"] = std::string(csharp ? "C#" : "GDScript") +
                              " from Godot: write what it does as C++ components and systems.";
            std::string text;
            if (auto source = platform::read_file(source_ + script); source && !csharp) {
                const ScriptOutline outline = outline_script(source.value());
                if (!outline.extends.empty()) {
                    text += "Extends " + outline.extends + ".";
                }
                if (!outline.class_name.empty()) {
                    text += " Class " + outline.class_name + ".";
                }
                if (!outline.exports.empty()) {
                    text += " Exported: " + join(outline.exports, "; ") + ".";
                }
                if (!outline.signals.empty()) {
                    text += " Signals: " + join(outline.signals, "; ") + ".";
                }
                if (!outline.functions.empty()) {
                    text += " Functions: " + join(outline.functions, "; ") + ".";
                }
            }
            std::vector<std::string> uses;
            json::Array links{destination(script)};
            for (const ScriptUse& use : record.uses) {
                uses.push_back(use.entity + " in " + use.scene);
                if (!contains(links, json::Value(use.scene))) {
                    links.emplace_back(use.scene);
                }
            }
            if (!uses.empty()) {
                text += " Used by " + join(uses, ", ") + ".";
            }
            for (const auto& [name, path] : autoloads_) {
                if (path == script) {
                    text += " An autoload named " + name + ".";
                }
            }
            text += " The values each entity's node set, its signal connections and node paths "
                    "are in " +
                    std::string(kReportFile) + ".";
            task["text"] = trim(text);
            task["links"] = std::move(links);
            task["tags"] = json::Array{"godot", "import"};
            if (task.find("status") == nullptr) {
                task["status"] = "open";
            }
            entries[id] = std::move(task);
        }
        if (!scripts_.empty()) {
            add_file(storyboard::file_path("work"), storyboard::format_file(entries));
        }
    }

    void write_report() {
        json::Value report;
        report["format"] = 1;
        report["engine"] = "godot";
        report["scenes"] = json::Object{};
        for (auto& [path, record] : report_scenes_) {
            report["scenes"][path] = std::move(record);
        }
        json::Value scripts = json::Object{};
        json::Array summary;
        for (const auto& [script, record] : scripts_) {
            json::Value item;
            item["task"] = task_of_[script];
            json::Array uses;
            for (const ScriptUse& use : record.uses) {
                json::Value entry;
                entry["scene"] = use.scene;
                entry["entity"] = use.entity;
                if (!use.values.as_object().empty()) {
                    entry["values"] = use.values;
                }
                uses.push_back(std::move(entry));
            }
            item["used_by"] = std::move(uses);
            if (!record.connections.empty()) {
                item["connections"] = record.connections;
            }
            scripts[destination(script)] = item;
            json::Value brief;
            brief["script"] = destination(script);
            brief["task"] = task_of_[script];
            brief["entities"] = static_cast<std::int64_t>(record.uses.size());
            summary.push_back(std::move(brief));
        }
        report["scripts"] = std::move(scripts);
        if (!groups_.empty()) {
            json::Value groups = json::Object{};
            for (const auto& [group, members] : groups_) {
                json::Array list;
                for (const std::string& member : members) {
                    list.emplace_back(member);
                }
                groups[group] = std::move(list);
            }
            report["groups"] = std::move(groups);
        }
        if (!autoloads_.empty()) {
            for (const auto& [name, path] : autoloads_) {
                report["autoloads"][name] = destination(path);
            }
        }
        json::Array unsupported = problems_.to_json();
        report["unsupported"] = unsupported;
        add_file(std::string(kReportFile), json::write(report) + "\n");

        json::Value& out = plan_.report;
        out["engine"] = "godot";
        out["project"] = project_name_;
        if (!main_scene_.empty()) {
            out["main_scene"] = main_scene_;
        }
        out["converted"]["scenes"] = scenes_;
        out["converted"]["sprite_frames"] = frames_files_;
        out["converted"]["tile_sets"] = tile_set_files_;
        out["converted"]["themes"] = theme_files_;
        out["converted"]["bus_layouts"] = bus_layout_files_;
        out["converted"]["input_actions"] = input_actions_;
        out["converted"]["assets"] = assets_;
        out["scripts"] = std::move(summary);
        out["unsupported"] = std::move(unsupported);
        out["report"] = kReportFile;
    }

    std::string source_;
    std::string prefix_;
    bool copy_assets_ = false;
    world::ComponentRegistry components_;
    std::vector<std::string> files_; // sorted, relative to source_
    std::map<std::string, std::unique_ptr<TextFile>> text_files_;
    std::map<std::string, std::string> uids_;
    std::map<std::string, std::vector<std::string>> resources_seen_; // .tres files by type
    std::set<std::string> resources_used_;
    std::set<std::string> resources_read_; // .tres files a converted value read
    std::map<std::string, std::string> frames_written_;
    std::map<std::string, TileSetFile> tile_sets_;
    std::map<std::string, std::string> themes_written_;
    std::set<std::string> bus_layouts_written_;
    std::map<std::string, ScriptRecord> scripts_; // by res path
    std::map<std::string, std::string> task_of_;
    std::set<std::string> task_ids_;
    std::map<std::string, std::string> autoloads_;
    std::map<std::string, std::vector<std::string>> groups_;
    std::map<std::string, json::Value> report_scenes_;
    std::set<std::string> scenes_started_;
    // Each converted scene's entity IDs, by node path from its root.
    std::map<std::string, std::map<std::string, std::string>> scene_ids_;
    Problems problems_;
    ImportPlan plan_;
    std::string project_name_;
    std::string main_scene_;
    int scenes_ = 0;
    int frames_files_ = 0;
    int tile_set_files_ = 0;
    int theme_files_ = 0;
    int bus_layout_files_ = 0;
    int input_actions_ = 0;
    int assets_ = 0;
};

} // namespace

Result<ImportPlan> plan_godot(const std::string& source, const std::string& prefix,
                              bool copy_assets, const std::optional<std::string>& existing_project,
                              const std::optional<std::string>& existing_work) {
    GodotImporter importer(source, prefix, copy_assets);
    return importer.run(existing_project, existing_work);
}

} // namespace slash::importer
