#include "godot_import.hpp"

#include "common.hpp"
#include "skeletons.hpp"

#include "slash/animation/animation.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/importer/scripts.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
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
// A tile set's most animation frames per tile.
constexpr std::int64_t kMaxAnimationFrames = 256;

// --- Paths and names ---------------------------------------------------------------------------

std::string strip_res(std::string_view path) {
    constexpr std::string_view kRes = "res://";
    if (path.starts_with(kRes)) {
        path.remove_prefix(kRes.size());
    }
    return std::string(path);
}

bool is_script_file(std::string_view extension) {
    return extension == ".gd" || extension == ".cs";
}

// Why a file Godot uses can't come along, by extension, or "".
std::string unsupported_file(std::string_view extension) {
    if (extension == ".gdshader" || extension == ".gdshaderinc") {
        return "a game's own shaders are on the roadmap; canvas materials are .material.json "
               "graphs";
    }
    return unsupported_asset(extension);
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

// A Godot terrain peering bit as the direction it faces in a tile set of this shape.
std::optional<canvas::Neighbor> peering_direction(std::string_view godot,
                                                  const canvas::TileSet& shape) {
    // Godot's CellNeighbor, in order.
    constexpr std::array<std::string_view, 16> kBits{
        "right_side",  "right_corner",  "bottom_right_side", "bottom_right_corner",
        "bottom_side", "bottom_corner", "bottom_left_side",  "bottom_left_corner",
        "left_side",   "left_corner",   "top_left_side",     "top_left_corner",
        "top_side",    "top_corner",    "top_right_side",    "top_right_corner"};
    const auto bit = std::ranges::find(kBits, godot) - kBits.begin();
    return canvas::godot_neighbor(shape.shape, shape.offset_axis, static_cast<int>(bit));
}

// Whether a terrain set's mode connects tiles by a direction, as canvas's terrain painting does:
// Godot's modes match corners and sides (0), corners (1) or sides (2).
bool connects(std::int64_t mode, const canvas::TileSet& shape, canvas::Neighbor direction) {
    const canvas::PeeringKind kind = canvas::peering_kind(shape, direction);
    return kind != canvas::PeeringKind::None &&
           (mode == 1 ? kind == canvas::PeeringKind::Corner
                      : mode != 2 || kind == canvas::PeeringKind::Side);
}

// A Godot custom data layer's Variant::Type as the type a tile set's layer holds, if it has one.
std::optional<canvas::CustomDataType> custom_data_type(std::int64_t variant) {
    switch (variant) {
    case 1:
        return canvas::CustomDataType::Bool;
    case 2:
        return canvas::CustomDataType::Int;
    case 3:
        return canvas::CustomDataType::Float;
    case 4:  // String
    case 21: // StringName
        return canvas::CustomDataType::String;
    case 5:
        return canvas::CustomDataType::Vector2;
    case 6:
        return canvas::CustomDataType::Vector2i;
    case 20:
        return canvas::CustomDataType::Color;
    default:
        return std::nullopt;
    }
}

// Godot's name for a Variant::Type, for the report.
std::string variant_type_name(std::int64_t variant) {
    constexpr std::array<std::string_view, 39> kNames{"Any",
                                                      "bool",
                                                      "int",
                                                      "float",
                                                      "String",
                                                      "Vector2",
                                                      "Vector2i",
                                                      "Rect2",
                                                      "Rect2i",
                                                      "Vector3",
                                                      "Vector3i",
                                                      "Transform2D",
                                                      "Vector4",
                                                      "Vector4i",
                                                      "Plane",
                                                      "Quaternion",
                                                      "AABB",
                                                      "Basis",
                                                      "Transform3D",
                                                      "Projection",
                                                      "Color",
                                                      "StringName",
                                                      "NodePath",
                                                      "RID",
                                                      "Object",
                                                      "Callable",
                                                      "Signal",
                                                      "Dictionary",
                                                      "Array",
                                                      "PackedByteArray",
                                                      "PackedInt32Array",
                                                      "PackedInt64Array",
                                                      "PackedFloat32Array",
                                                      "PackedFloat64Array",
                                                      "PackedStringArray",
                                                      "PackedVector2Array",
                                                      "PackedVector3Array",
                                                      "PackedColorArray",
                                                      "PackedVector4Array"};
    return variant >= 0 && static_cast<std::size_t>(variant) < kNames.size()
               ? std::string(kNames.at(static_cast<std::size_t>(variant)))
               : std::to_string(variant);
}

// A tile's custom data value as a layer of this type holds it, with numbers converted as Godot's
// Variant::construct converts them (truncated to whole numbers); nullopt when Godot couldn't
// convert it either. null is the type's default.
std::optional<json::Value> custom_data_value(const Value& value, canvas::CustomDataType type) {
    if (value.kind == Value::Kind::Nil) {
        return canvas::custom_data_default(type);
    }
    const bool scalar = value.is_number() || value.kind == Value::Kind::Bool;
    double real = value.number();
    if (value.kind == Value::Kind::Bool) {
        real = value.boolean ? 1.0 : 0.0;
    }
    // A float as a whole number, kept in range.
    const auto whole = [](double number) {
        constexpr double kLimit = 9.0e18;
        return static_cast<std::int64_t>(std::clamp(number, -kLimit, kLimit));
    };
    const auto numbers = value.numbers();
    switch (type) {
    case canvas::CustomDataType::Bool:
        return scalar ? std::optional<json::Value>(real != 0.0) : std::nullopt;
    case canvas::CustomDataType::Int:
        if (value.kind == Value::Kind::Int) {
            return json::Value(value.integer);
        }
        return scalar ? std::optional<json::Value>(whole(real)) : std::nullopt;
    case canvas::CustomDataType::Float:
        return scalar ? std::optional<json::Value>(real) : std::nullopt;
    case canvas::CustomDataType::String:
        return value.is_string() ? std::optional<json::Value>(value.text) : std::nullopt;
    case canvas::CustomDataType::Vector2:
    case canvas::CustomDataType::Vector2i:
        if ((!value.is_call("Vector2") && !value.is_call("Vector2i")) || !numbers ||
            numbers->size() != 2) {
            return std::nullopt;
        }
        if (type == canvas::CustomDataType::Vector2i) {
            constexpr double kLimit = 2147483647.0;
            return json::Value(json::Array{whole(std::clamp(numbers->at(0), -kLimit, kLimit)),
                                           whole(std::clamp(numbers->at(1), -kLimit, kLimit))});
        }
        return json::Value(json::Array{numbers->at(0), numbers->at(1)});
    case canvas::CustomDataType::Color:
        if (!value.is_call("Color") || !numbers || numbers->size() != 4) {
            return std::nullopt;
        }
        return json::Value(
            json::Array{numbers->at(0), numbers->at(1), numbers->at(2), numbers->at(3)});
    }
    return std::nullopt;
}

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
    NodeType{"CanvasLayer", Family::Plain, "CanvasLayer", ""},
    NodeType{"ParallaxBackground", Family::Plain, "CanvasLayer", ""},
    NodeType{"Node2D", Family::Node2D, "", ""},
    NodeType{"Parallax2D", Family::Node2D, "Parallax2D", ""},
    NodeType{"ParallaxLayer", Family::Node2D, "Parallax2D", ""},
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
    NodeType{"GPUParticles2D", Family::Node2D, "GPUParticles2D", ""},
    NodeType{"AudioStreamPlayer2D", Family::Node2D, "AudioStreamPlayer2D", ""},
    NodeType{"AudioListener2D", Family::Node2D, "AudioListener2D", ""},
    NodeType{"AudioStreamPlayer", Family::Plain, "AudioStreamPlayer", ""},
    NodeType{"AnimationPlayer", Family::Plain, "AnimationPlayer", ""},
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
    NodeType{"OptionButton", Family::Control, "OptionButton", ""},
    NodeType{"MenuButton", Family::Control, "MenuButton", ""},
    NodeType{"PopupMenu", Family::Plain, "PopupMenu", ""},
    NodeType{"TextureButton", Family::Control, "TextureButton", ""},
    NodeType{"LineEdit", Family::Control, "LineEdit", ""},
    NodeType{"TextEdit", Family::Control, "TextEdit", ""},
    NodeType{"CodeEdit", Family::Control, "TextEdit", "CodeEdit"},
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
        type == "BackBufferCopy" || type == "TouchScreenButton" || type == "CanvasGroup") {
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

// A node's bool property, false when it doesn't set one.
bool node_bool(const Node& node, std::string_view property) {
    const Property* found = node.find(property);
    return found != nullptr && found->value->kind == Value::Kind::Bool && found->value->boolean;
}

// A node's Vector2 property, or fallback when it doesn't set one.
std::array<double, 2> node_vector(const Node& node, std::string_view property,
                                  std::array<double, 2> fallback) {
    if (const Property* found = node.find(property); found != nullptr) {
        if (const auto xy = found->value->numbers(); xy && xy->size() == 2) {
            return {xy->at(0), xy->at(1)};
        }
    }
    return fallback;
}

// ParallaxBackground's own default layer, behind the world.
constexpr std::int64_t kParallaxBackgroundLayer = -100;

// A ParallaxLayer's Parallax2D, from it and the ParallaxBackground above it, as Godot's
// ParallaxBackgroundEditorPlugin::convert_to_parallax2d makes it. Only fields that differ from
// Parallax2D's defaults are set.
void convert_parallax_layer(const std::vector<Node>& nodes, const Node& layer,
                            json::Value& parallax) {
    const Node* background = nullptr;
    if (layer.parent >= 0 &&
        nodes[static_cast<std::size_t>(layer.parent)].type == "ParallaxBackground") {
        background = &nodes[static_cast<std::size_t>(layer.parent)];
    }
    const auto from_background = [&](std::string_view property, std::array<double, 2> fallback) {
        return background != nullptr ? node_vector(*background, property, fallback) : fallback;
    };
    constexpr std::array<double, 2> kZero{0.0, 0.0};
    constexpr std::array<double, 2> kOne{1.0, 1.0};
    const std::array<double, 2> base_offset = from_background("scroll_base_offset", kZero);
    const std::array<double, 2> base_scale = from_background("scroll_base_scale", kOne);
    const std::array<double, 2> limit_begin = from_background("scroll_limit_begin", kZero);
    const std::array<double, 2> limit_end = from_background("scroll_limit_end", kZero);
    const std::array<double, 2> motion_scale = node_vector(layer, "motion_scale", kOne);
    const std::array<double, 2> motion_offset = node_vector(layer, "motion_offset", kZero);
    const std::array<double, 2> mirroring = node_vector(layer, "motion_mirroring", kZero);
    const std::array<double, 2> position = node_vector(layer, "position", kZero);
    // Parallax2D's limits when there are none.
    constexpr double kNoLimit = 10000000.0;
    std::array<double, 2> offset{};
    std::array<double, 2> scale{};
    std::array<double, 2> begin{-kNoLimit, -kNoLimit};
    std::array<double, 2> end{kNoLimit, kNoLimit};
    bool limited = false;
    for (std::size_t i = 0; i < 2; ++i) {
        offset.at(i) =
            (base_offset.at(i) * motion_scale.at(i)) + motion_offset.at(i) + position.at(i);
        scale.at(i) = base_scale.at(i) * motion_scale.at(i);
        if (limit_begin.at(i) != 0.0 || limit_end.at(i) != 0.0) {
            begin.at(i) = limit_begin.at(i);
            end.at(i) = limit_end.at(i);
            limited = true;
        }
    }
    const auto pair = [](const std::array<double, 2>& xy) { return json::Array{xy[0], xy[1]}; };
    if (offset != kZero) {
        parallax["scroll_offset"] = pair(offset);
    }
    if (scale != kOne) {
        parallax["scroll_scale"] = pair(scale);
    }
    if (mirroring != kZero) {
        parallax["repeat_size"] = pair(mirroring);
    }
    if (limited) {
        parallax["limit_begin"] = pair(begin);
        parallax["limit_end"] = pair(end);
    }
    if (background != nullptr && node_bool(*background, "scroll_ignore_camera_zoom")) {
        parallax["follow_viewport"] = false;
    }
}

// Why a node type doesn't convert.
std::string unsupported_node(std::string_view type) {
    if (type.ends_with("3D") || type == "WorldEnvironment" || type == "GridMap" ||
        type == "CSGBox3D") {
        return "3D comes with milestone 7";
    }
    if (type == "AnimationTree") {
        return "blend trees and state machines come with milestone 7; play the animations with "
               "an AnimationPlayer";
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
        {"PopupMenu.layout_direction", {"inherited", "inherited", "ltr", "rtl", "inherited"}},
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

// --- Sounds ------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 3> kWavLoopModes{"forward", "pingpong", "backward"};
constexpr std::array<std::string_view, 3> kPlaybackModes{"random_no_repeats", "random",
                                                         "sequential"};
constexpr std::array<std::string_view, 4> kTransitionFromTimes{"immediate", "next_beat", "next_bar",
                                                               "end"};
constexpr std::array<std::string_view, 3> kTransitionToTimes{"same_position", "start",
                                                             "previous_position"};
constexpr std::array<std::string_view, 5> kFadeModes{"disabled", "in", "out", "cross", "automatic"};

// A WAV file's loop, as Godot's importer reads it from a "smpl" chunk when a sound's
// edit/loop_mode is "Detect From WAV".
struct WavLoop {
    std::string_view mode;
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

std::optional<WavLoop> wav_smpl_loop(std::string_view bytes) {
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i]))
                     << (8 * i);
        }
        return value;
    };
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") {
        return std::nullopt;
    }
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::size_t size = u32(at + 4);
        const std::size_t data = at + 8;
        if (size > bytes.size() - data) {
            break;
        }
        // Godot skips ten words, the chunk's header and the first loop's cue point, then reads
        // that loop's type (0 forward, 1 ping-pong, 2 backward), start and end.
        if (bytes.substr(at, 4) == "smpl" && size >= 52) {
            const std::uint32_t type = u32(data + 40);
            if (type >= kWavLoopModes.size()) {
                return std::nullopt;
            }
            return WavLoop{
                .mode = kWavLoopModes.at(type), .begin = u32(data + 44), .end = u32(data + 48)};
        }
        at = data + size + (size & 1U);
    }
    return std::nullopt;
}

// --- The importer ------------------------------------------------------------------------------

class GodotImporter {
public:
    GodotImporter(std::string source, const GodotFiles* unpacked, std::string prefix,
                  bool copy_assets)
        : source_(std::move(source)), unpacked_(unpacked), prefix_(std::move(prefix)),
          copy_assets_(copy_assets) {
        world::register_builtin_components(components_);
    }

    Result<ImportPlan> run(const Existing& existing) {
        auto project_text = read("project.godot");
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
        auto listed = list();
        if (!listed) {
            return listed.error();
        }
        files_ = std::move(listed.value());
        load_text_files();
        for (const std::string& file : files_) {
            take_file(file);
        }
        read_scripts(existing);
        convert_project(settings.value(), existing.project);
        for (const auto& [path, text_file] : text_files_) {
            const std::string& type = text_file->type;
            if (extension_of(path) == ".tscn") {
                convert_scene(*text_file);
            } else if (text_file->resource != nullptr &&
                       (type == "SpriteFrames" || type == "TileSet" || type == "Theme" ||
                        is_audio_stream_type(type))) {
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
        write_tasks(existing.work);
        write_skeletons();
        write_report();
        return std::move(plan_);
    }

private:
    // --- Files ---

    // A file of the source project, from its folder or the game it was unpacked from.
    [[nodiscard]] Result<std::string> read(const std::string& file) const {
        if (unpacked_ != nullptr) {
            const auto it = unpacked_->find(file);
            if (it == unpacked_->end()) {
                return Error{.code = ErrorCode::NotFound, .message = "no " + file + " in the game"};
            }
            return it->second;
        }
        return platform::read_file(source_ + file);
    }

    // Every file of the source project, sorted, but those in its build/ and addons/ folders.
    [[nodiscard]] Result<std::vector<std::string>> list() const {
        const auto descend = [](std::string_view folder) {
            return folder != "build/" && folder != "addons/";
        };
        if (unpacked_ == nullptr) {
            return platform::list_files(source_, descend);
        }
        std::vector<std::string> files;
        for (const auto& [path, contents] : *unpacked_) {
            const std::size_t slash = path.find('/');
            if (slash == std::string::npos ||
                descend(std::string_view(path).substr(0, slash + 1))) {
                files.push_back(path);
            }
        }
        return files;
    }

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
                if (auto text = read(file); text) {
                    learn_uid(text.value(), without_extension(file));
                    if (extension == ".import") {
                        learn_import_settings(text.value(), file);
                    }
                }
                continue;
            }
            if (extension != ".tscn" && extension != ".tres") {
                continue;
            }
            auto text = read(file);
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

    // Keeps a sound's import settings: its .import file's [params].
    void learn_import_settings(std::string_view text, const std::string& file) {
        const std::string sound = without_extension(file);
        if (!is_sound_file(extension_of(sound))) {
            return;
        }
        auto sections = godot::parse_project_settings(text);
        if (!sections) {
            problems_.add("unreadable file", file, sections.error().message);
            return;
        }
        for (Section& section : sections.value()) {
            if (section.tag == "params") {
                sound_settings_[sound] = std::move(section);
            }
        }
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
                if (auto bytes = read(file); bytes) {
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
            return "animations convert with an AnimationPlayer that plays them, which says what "
                   "their tracks animate; none does";
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
        // A project needs a main scene, which a Godot project may leave out.
        for (std::size_t i = 0; main_scene_.empty() && !existing && i < files_.size(); ++i) {
            if (extension_of(files_[i]) == ".tscn") {
                main_scene_ =
                    destination(without_extension(files_[i]) + std::string(world::kSceneSuffix));
                problems_.add("main scene", "project.godot",
                              "it names none, so the first scene, " + files_[i] +
                                  ", is the main "
                                  "scene");
            }
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
        // Godot's "default" and "x11" try X11 first, as SlashEngine does unless told otherwise.
        if (const Value* driver = setting("display", "display_server/driver.linuxbsd");
            driver != nullptr && driver->is_string() && driver->text == "wayland") {
            project["window"]["linux_display_driver"] = "wayland";
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
        if (const Value* input = setting("audio", "driver/enable_input");
            input != nullptr && input->kind == Value::Kind::Bool && input->boolean) {
            project["audio"]["enable_input"] = true;
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

        // The scripts' C++ skeletons are the game's code.
        if (skeletons_ && !skeletons_->empty()) {
            json::Value& gameplay = project["gameplay"];
            json::Value* sources = gameplay.is_object() ? gameplay.find("sources") : nullptr;
            if (sources == nullptr || !sources->is_array()) {
                gameplay = json::Object{};
                gameplay["sources"] = json::Array{skeletons_->folder()};
            } else if (!contains(sources->as_array(), json::Value(skeletons_->folder()))) {
                sources->as_array().emplace_back(skeletons_->folder());
            }
        }

        // Keep only what validates, so a bad setting can't stop the rest.
        for (int pass = 0; pass < 32; ++pass) {
            const auto errors = runtime::validate_project(project);
            if (errors.empty()) {
                break;
            }
            bool reset = false;
            for (const json::ValidationError& error : errors) {
                problems_.add("setting " + error.path, "project.godot", error.message);
                if (!remove_pointer(project, error.path)) {
                    project = runtime::new_project(name, main_scene_);
                    reset = true;
                    break;
                }
            }
            if (reset) {
                break;
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
        const Node* found = find_node(nodes, from, path);
        return found != nullptr ? found->id : std::string();
    }

    // The node a path relative to another names, or null; a property path's node for one such as
    // "Sprite2D:modulate".
    static const Node* find_node(const std::vector<Node>& nodes, const Node& from,
                                 std::string_view path) {
        if (path.empty() || path.front() == '/') {
            return nullptr;
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
                    return nullptr;
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
                return &node;
            }
        }
        return nullptr;
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
            if (is_sound_file(extension)) {
                return sound_path(path);
            }
            if (is_texture_file(extension) || is_font_file(extension)) {
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
        if (type == "ParticleProcessMaterial") {
            return process_material_file(file, section, base);
        }
        if (is_audio_stream_type(type)) {
            std::string path = audio_stream_file(file, section, type, base);
            if (path.empty()) {
                why = "a " + type + " with no stream that converts";
            }
            return path;
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

    // --- Sounds and music ---

    static bool is_audio_stream_type(std::string_view type) {
        return type == "AudioStreamRandomizer" || type == "AudioStreamPlaylist" ||
               type == "AudioStreamSynchronized" || type == "AudioStreamInteractive" ||
               type == "AudioStreamGenerator" || type == "AudioStreamPolyphonic" ||
               type == "AudioStreamMicrophone";
    }

    // The path a sound becomes: the file itself, or a .audio.json file beside it with the loop
    // and beats Godot imports it with, written once.
    std::string sound_path(const std::string& path) {
        if (const auto done = sounds_written_.find(path); done != sounds_written_.end()) {
            return done->second;
        }
        std::string& written = sounds_written_[path];
        written = destination(path);
        const auto found = sound_settings_.find(path);
        const Section* settings = found != sound_settings_.end() ? &found->second : nullptr;
        json::Value out = extension_of(path) == ".wav" ? wav_settings(path, settings)
                                                       : compressed_sound_settings(settings);
        if (out.is_null()) {
            return written;
        }
        out["format"] = 1;
        out["source"] = destination(path);
        written = destination(path) + std::string(audio::kStreamSuffix);
        keep_valid(out, written,
                   [](const json::Value& doc) { return audio::validate_stream_file(doc); });
        add_file(written, json::write(out) + "\n");
        ++stream_files_;
        return written;
    }

    // A WAV file's loop, from its import settings or, as Godot detects it by default, its own
    // "smpl" chunk; null for none.
    json::Value wav_settings(const std::string& path, const Section* settings) {
        std::int64_t mode = 0; // Detect From WAV
        std::int64_t begin = 0;
        std::int64_t end = -1;
        if (settings != nullptr) {
            for (const auto& [name, value] : settings->properties) {
                if (name == "edit/loop_mode" && value.kind == Value::Kind::Int) {
                    mode = value.integer;
                } else if (name == "edit/loop_begin" && value.kind == Value::Kind::Int) {
                    begin = value.integer;
                } else if (name == "edit/loop_end" && value.kind == Value::Kind::Int) {
                    end = value.integer;
                } else if ((name == "edit/trim" || name == "edit/normalize") &&
                           value.kind == Value::Kind::Bool && value.boolean) {
                    problems_.add("WAV import option " + name, path + ".import",
                                  "sounds play as they are in the file");
                }
            }
        }
        json::Value out;
        if (mode == 0) {
            const auto bytes = read(path);
            const auto loop = bytes ? wav_smpl_loop(bytes.value()) : std::nullopt;
            if (loop) {
                out["loop_mode"] = loop->mode;
                out["loop_begin"] = loop->begin;
                out["loop_end"] = loop->end;
            }
            return out;
        }
        if (mode < 2 || mode > 4) {
            return out; // disabled
        }
        // Godot counts a negative frame back from the end, so -1 is the last frame.
        if (begin < 0 || end < 0) {
            const auto bytes = read(path);
            if (!bytes) {
                problems_.add("WAV loop", path + ".import", bytes.error().message);
                return out;
            }
            const auto sound = audio::decode_sound(bytes.value(), path);
            if (!sound) {
                problems_.add("WAV loop", path + ".import", sound.error().message);
                return out;
            }
            const std::int64_t frames = sound.value().frames;
            const std::int64_t last = std::max<std::int64_t>(frames - 1, 0);
            begin = begin < 0 ? std::clamp<std::int64_t>(begin + frames, 0, last) : begin;
            end = end < 0 ? std::clamp<std::int64_t>(end + frames, 0, last) : end;
        }
        out["loop_mode"] = kWavLoopModes.at(static_cast<std::size_t>(mode - 2));
        if (begin != 0) {
            out["loop_begin"] = begin;
        }
        out["loop_end"] = end;
        return out;
    }

    // An Ogg Vorbis or MP3 file's loop and beats from its import settings; null for none.
    static json::Value compressed_sound_settings(const Section* settings) {
        json::Value out;
        if (settings == nullptr) {
            return out;
        }
        for (const auto& [name, value] : settings->properties) {
            if (name == "loop" && value.kind == Value::Kind::Bool && value.boolean) {
                out["loop"] = true;
            } else if ((name == "loop_offset" || name == "bpm") && value.is_number() &&
                       value.number() != 0.0) {
                out[name] = value.number();
            } else if ((name == "beat_count" || name == "bar_beats") &&
                       value.kind == Value::Kind::Int &&
                       value.integer != (name == "bar_beats" ? 4 : 0)) {
                out[name] = value.integer;
            }
        }
        return out;
    }

    // A property of a music resource, as Godot's default when it's missing or of another type.
    static double number_of(const Section& resource, const std::string& name, double fallback) {
        const Value* value = resource.property(name);
        return value != nullptr && value->is_number() ? value->number() : fallback;
    }
    static std::int64_t integer_of(const Section& resource, const std::string& name,
                                   std::int64_t fallback) {
        const Value* value = resource.property(name);
        return value != nullptr && value->kind == Value::Kind::Int ? value->integer : fallback;
    }
    static bool boolean_of(const Section& resource, const std::string& name, bool fallback) {
        const Value* value = resource.property(name);
        return value != nullptr && value->kind == Value::Kind::Bool ? value->boolean : fallback;
    }

    // An AudioStreamRandomizer, AudioStreamPlaylist, AudioStreamSynchronized,
    // AudioStreamInteractive, AudioStreamGenerator, AudioStreamPolyphonic or
    // AudioStreamMicrophone as a .audio.json file, written once; "" if it has streams and none of
    // them convert.
    std::string audio_stream_file(const TextFile& file, const Section& resource,
                                  const std::string& type, const std::string& base) {
        const std::string key = file.path + "#" + base;
        if (const auto done = streams_written_.find(key); done != streams_written_.end()) {
            return done->second;
        }
        streams_written_[key] = ""; // until it converts
        resources_used_.insert(type);
        const std::string where = resource_where(resource, file);
        // The file a stream property names, or "" after reporting why not.
        const auto stream = [&](const std::string& name) -> std::string {
            const Value* value = resource.property(name);
            if (value == nullptr || value->kind == Value::Kind::Nil) {
                return {};
            }
            std::string why;
            std::string converted = resource_path(*value, file, why);
            if (converted.empty()) {
                problems_.add(type + "." + name, where, why);
            }
            return converted;
        };
        json::Value out;
        out["format"] = 1;
        std::size_t streams = 0;
        if (type == "AudioStreamGenerator") {
            // Godot's MIX_RATE_OUTPUT, MIX_RATE_INPUT and MIX_RATE_CUSTOM, the default.
            json::Value generator = json::Object{};
            if (const std::int64_t mode = integer_of(resource, "mix_rate_mode", 2); mode == 0) {
                generator["mix_rate_mode"] = "output";
            } else if (mode == 1) {
                generator["mix_rate_mode"] = "input";
            }
            if (const double rate = number_of(resource, "mix_rate", 44100.0); rate != 44100.0) {
                generator["mix_rate"] = rate;
            }
            if (const double length = number_of(resource, "buffer_length", 0.5); length != 0.5) {
                generator["buffer_length"] = length;
            }
            out["generator"] = std::move(generator);
            streams = 1; // it needs none
        } else if (type == "AudioStreamPolyphonic") {
            json::Value polyphonic = json::Object{};
            if (const std::int64_t polyphony = integer_of(resource, "polyphony", 32);
                polyphony != 32) {
                polyphonic["polyphony"] = polyphony;
            }
            out["polyphonic"] = std::move(polyphonic);
            streams = 1;
        } else if (type == "AudioStreamMicrophone") {
            out["microphone"] = json::Object{};
            streams = 1;
        } else if (type == "AudioStreamInteractive") {
            streams = interactive_music(resource, where, stream, out);
        } else {
            // Godot's randomizer counts "streams_count", the others "stream_count".
            const bool randomizer = type == "AudioStreamRandomizer";
            const bool playlist = type == "AudioStreamPlaylist";
            const std::int64_t count =
                integer_of(resource, randomizer ? "streams_count" : "stream_count", 0);
            json::Array entries;
            for (std::int64_t i = 0; i < count; ++i) {
                const std::string prefix = "stream_" + std::to_string(i);
                std::string found = stream(playlist ? prefix : prefix + "/stream");
                if (found.empty()) {
                    continue;
                }
                json::Value entry;
                entry["stream"] = std::move(found);
                if (const double weight = number_of(resource, prefix + "/weight", 1.0);
                    randomizer && weight != 1.0) {
                    entry["weight"] = weight;
                }
                if (const double volume = number_of(resource, prefix + "/volume", 0.0);
                    !randomizer && !playlist && volume != 0.0) {
                    entry["volume_db"] = volume;
                }
                entries.push_back(std::move(entry));
            }
            streams = entries.size();
            std::string_view kind = "synchronized";
            if (randomizer) {
                kind = "streams";
            } else if (playlist) {
                kind = "playlist";
            }
            out[std::string(kind)] = std::move(entries);
            if (randomizer) {
                if (const std::int64_t mode = integer_of(resource, "playback_mode", 0);
                    mode > 0 && mode < static_cast<std::int64_t>(kPlaybackModes.size())) {
                    out["playback_mode"] = kPlaybackModes.at(static_cast<std::size_t>(mode));
                }
                if (const double pitch = number_of(resource, "random_pitch", 1.0); pitch != 1.0) {
                    out["random_pitch"] = pitch;
                }
                if (const double offset = number_of(resource, "random_volume_offset_db", 0.0);
                    offset != 0.0) {
                    out["random_volume_offset_db"] = offset;
                }
            } else if (playlist) {
                if (boolean_of(resource, "shuffle", false)) {
                    out["shuffle"] = true;
                }
                if (!boolean_of(resource, "loop", true)) {
                    out["loop"] = false;
                }
                if (const double fade = number_of(resource, "fade_time", 0.3); fade != 0.3) {
                    out["fade_time"] = fade;
                }
            }
        }
        if (streams == 0) {
            return {};
        }
        std::string path = destination(base + std::string(audio::kStreamSuffix));
        streams_written_[key] = path;
        keep_valid(out, path,
                   [](const json::Value& doc) { return audio::validate_stream_file(doc); });
        add_file(path, json::write(out) + "\n");
        ++stream_files_;
        return path;
    }

    // An AudioStreamInteractive's clips, initial clip and transitions, naming clips by name;
    // returns how many clips have a stream that converts.
    template <typename StreamOf>
    std::size_t interactive_music(const Section& resource, const std::string& where,
                                  const StreamOf& stream, json::Value& out) {
        constexpr std::int64_t kMaxClips = 63; // Godot's MAX_CLIPS
        const std::int64_t count =
            std::clamp<std::int64_t>(integer_of(resource, "clip_count", 0), 0, kMaxClips);
        // Godot lets clips share a name, or have none; switch_to_clip needs one each.
        std::vector<std::string> names;
        std::set<std::string> taken;
        for (std::int64_t i = 0; i < count; ++i) {
            std::string name = resource.field_string("clip_" + std::to_string(i) + "/name");
            if (name.empty() || taken.contains(name)) {
                std::string renamed = "clip_" + std::to_string(i);
                while (taken.contains(renamed)) {
                    renamed += "_";
                }
                std::string why = name.empty() ? std::string("it has no name")
                                               : "another clip is named \"" + name + "\"";
                why += "; it is named \"" + renamed + "\"";
                problems_.add("AudioStreamInteractive clip " + std::to_string(i), where, why);
                name = std::move(renamed);
            }
            taken.insert(name);
            names.push_back(std::move(name));
        }
        const auto clip_name = [&](std::int64_t index) -> const std::string* {
            return index >= 0 && index < count ? &names.at(static_cast<std::size_t>(index))
                                               : nullptr;
        };
        json::Array clips;
        std::size_t playable = 0;
        for (std::int64_t i = 0; i < count; ++i) {
            const std::string prefix = "clip_" + std::to_string(i) + "/";
            json::Value clip;
            clip["name"] = names.at(static_cast<std::size_t>(i));
            if (std::string found = stream(prefix + "stream"); !found.empty()) {
                clip["stream"] = std::move(found);
                ++playable;
            }
            // Godot ignores an enabled auto advance to the clip itself or to no clip.
            const std::int64_t advance = integer_of(resource, prefix + "auto_advance", 0);
            const std::int64_t next = integer_of(resource, prefix + "next_clip", 0);
            if (advance == 1 && next != i && clip_name(next) != nullptr) {
                clip["auto_advance"] = "enabled";
                clip["next_clip"] = *clip_name(next);
            } else if (advance == 2) {
                clip["auto_advance"] = "return_to_hold";
            }
            clips.push_back(std::move(clip));
        }
        out["clips"] = std::move(clips);
        if (const std::int64_t initial = integer_of(resource, "initial_clip", 0);
            initial != 0 && clip_name(initial) != nullptr) {
            out["initial_clip"] = *clip_name(initial);
        }
        const Value* map = resource.property("_transitions");
        if (map == nullptr || map->kind != Value::Kind::Dictionary) {
            return playable;
        }
        // Each entry is keyed by Vector2i(from, to), -1 for any clip.
        json::Array transitions;
        for (const Value::Entry& entry : map->entries) {
            const auto ends = entry.key.numbers();
            if (!ends || ends->size() != 2 || entry.value.kind != Value::Kind::Dictionary) {
                problems_.add("AudioStreamInteractive transition", where,
                              "not a Vector2i of two clips and a dictionary");
                continue;
            }
            json::Value transition = json::Object{};
            bool known = true;
            const std::array<std::pair<std::int64_t, std::string_view>, 2> sides{
                std::pair{static_cast<std::int64_t>(ends->at(0)), std::string_view("from")},
                std::pair{static_cast<std::int64_t>(ends->at(1)), std::string_view("to")}};
            for (const auto& [index, side] : sides) {
                if (const std::string* name = clip_name(index); name != nullptr) {
                    transition[std::string(side)] = *name;
                } else if (index != -1) {
                    known = false;
                }
            }
            if (!known) {
                problems_.add("AudioStreamInteractive transition", where,
                              "it names a clip past the last");
                continue;
            }
            const auto setting = [&](std::string_view name) -> std::int64_t {
                const Value* value = entry.value.get(name);
                return value != nullptr && value->kind == Value::Kind::Int ? value->integer : -1;
            };
            // An enum by name, left out at Godot's default.
            const auto named = [&](const std::string& field, std::span<const std::string_view> of,
                                   std::int64_t fallback) {
                const std::int64_t value = setting(field);
                if (value >= 0 && value < static_cast<std::int64_t>(of.size()) &&
                    value != fallback) {
                    transition[field] = of[static_cast<std::size_t>(value)];
                }
            };
            named("from_time", kTransitionFromTimes, 1);
            named("to_time", kTransitionToTimes, 1);
            named("fade_mode", kFadeModes, 4);
            if (const Value* beats = entry.value.get("fade_beats");
                beats != nullptr && beats->is_number() && beats->number() != 1.0) {
                transition["fade_beats"] = beats->number();
            }
            if (const Value* filler = entry.value.get("use_filler_clip");
                filler != nullptr && filler->kind == Value::Kind::Bool && filler->boolean) {
                if (const std::string* name = clip_name(setting("filler_clip")); name != nullptr) {
                    transition["filler_clip"] = *name;
                }
            }
            if (const Value* hold = entry.value.get("hold_previous");
                hold != nullptr && hold->kind == Value::Kind::Bool && hold->boolean) {
                transition["hold_previous"] = true;
            }
            transitions.push_back(std::move(transition));
        }
        if (!transitions.empty()) {
            out["transitions"] = std::move(transitions);
        }
        return playable;
    }

    // --- Tile sets and tile maps ---

    // A tile set as converted: its file, its sources' names by Godot's source IDs, and what it
    // holds, for writing cells.
    struct TileSetFile {
        std::string path;
        std::map<std::int64_t, std::string> sources;
        std::optional<canvas::TileSet> set;
    };

    // What a tile set's tiles may refer to: how many occlusion layers it has, and its custom
    // data layers that carry over, by Godot's index.
    struct TileLayers {
        std::size_t occlusion = 0;
        std::map<std::int64_t, canvas::CustomDataLayer> data;
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
        canvas::TileSet shape; // its shape, layout and offset axis
        // Terrain sets by index: their modes and their terrains' names by index.
        std::map<std::int64_t, std::int64_t> modes;
        std::map<std::pair<std::int64_t, std::int64_t>, std::string> terrains;
        std::map<std::int64_t, json::Value> physics;    // physics layers by index
        std::map<std::int64_t, std::int64_t> occlusion; // occlusion layers' light masks by index
        std::set<std::int64_t> sdf_collision;           // occlusion layers particles collide with
        // Custom data layers' names and Variant types by index.
        std::map<std::int64_t, std::pair<std::string, std::int64_t>> data;
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
            } else if (name == "tile_shape" || name == "tile_layout" ||
                       name == "tile_offset_axis") {
                // Enums, stored as numbers in Godot's order; 0, the default, is left out.
                const auto whole = static_cast<std::int64_t>(value.number());
                if (whole == 0) {
                    continue;
                }
                const std::size_t number = whole > 0 ? static_cast<std::size_t>(whole) : SIZE_MAX;
                if (name == "tile_shape" && number < canvas::kTileShapeNames.size()) {
                    shape.shape = static_cast<canvas::TileShape>(number);
                    doc["tile_shape"] = canvas::kTileShapeNames.at(number);
                } else if (name == "tile_layout" && number < canvas::kTileLayoutNames.size()) {
                    shape.layout = static_cast<canvas::TileLayout>(number);
                    doc["tile_layout"] = canvas::kTileLayoutNames.at(number);
                } else if (name == "tile_offset_axis" &&
                           number < canvas::kTileOffsetAxisNames.size()) {
                    shape.offset_axis = static_cast<canvas::TileOffsetAxis>(number);
                    doc["tile_offset_axis"] = canvas::kTileOffsetAxisNames.at(number);
                } else {
                    problems_.add("TileSet." + name, where);
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
                const auto index =
                    parse_int(parts[0].substr(std::string_view("physics_layer_").size()));
                const std::string_view field = parts.size() == 2 ? parts[1] : "";
                if (index && *index >= 0 &&
                    (field == "collision_layer" || field == "collision_mask")) {
                    physics[*index][std::string(field)] = static_cast<std::int64_t>(value.number());
                } else if (index && *index >= 0 && field == "physics_material") {
                    json::Value& layer = physics[*index];
                    if (const auto [material, owner] = resource_section(value, file);
                        material != nullptr) {
                        for (const char* number_field : {"friction", "bounce"}) {
                            if (const Value* number = material->property(number_field);
                                number != nullptr && number->is_number()) {
                                layer[number_field] = std::string_view(number_field) == "bounce"
                                                          ? std::clamp(number->number(), 0.0, 1.0)
                                                          : std::max(number->number(), 0.0);
                            }
                        }
                    }
                } else {
                    problems_.add("TileSet." + name, where);
                }
            } else if (parts[0].starts_with("occlusion_layer_")) {
                const auto index =
                    parse_int(parts[0].substr(std::string_view("occlusion_layer_").size()));
                const std::string_view field = parts.size() == 2 ? parts[1] : "";
                if (index && *index >= 0 && field == "light_mask") {
                    // Godot's int, as the bits it holds.
                    occlusion[*index] = static_cast<std::uint32_t>(
                        static_cast<std::int64_t>(value.number()) & 0xFFFFFFFF);
                } else if (index && *index >= 0 && field == "sdf_collision") {
                    occlusion.try_emplace(*index, 1);
                    if (value.boolean) {
                        sdf_collision.insert(*index);
                    }
                } else {
                    problems_.add("TileSet." + name, where);
                }
            } else if (parts[0].starts_with("custom_data_layer_")) {
                const auto index =
                    parse_int(parts[0].substr(std::string_view("custom_data_layer_").size()));
                const std::string_view field = parts.size() == 2 ? parts[1] : "";
                if (index && *index >= 0 && field == "name") {
                    data[*index].first = value.text;
                } else if (index && *index >= 0 && field == "type") {
                    data[*index].second = static_cast<std::int64_t>(value.number());
                } else {
                    problems_.add("TileSet." + name, where);
                }
            } else if (parts[0].starts_with("navigation_layer_")) {
                problems_.add("tile set navigation layers", where);
            } else {
                problems_.add("TileSet." + name, where);
            }
        }
        if (tile_size != std::array<std::int64_t, 2>{16, 16}) {
            doc["tile_size"] = json::Array{tile_size[0], tile_size[1]};
        }
        if (!physics.empty()) {
            json::Array layers;
            for (std::int64_t index = 0; index <= physics.rbegin()->first; ++index) {
                const auto found = physics.find(index);
                layers.push_back(found != physics.end() ? found->second
                                                        : json::Value(json::Object{}));
            }
            doc["physics_layers"] = std::move(layers);
        }
        TileLayers layers;
        if (!occlusion.empty()) {
            json::Array out_layers;
            for (std::int64_t index = 0; index <= occlusion.rbegin()->first; ++index) {
                const auto found = occlusion.find(index);
                json::Value layer = json::Object{};
                if (found != occlusion.end() && found->second != 1) {
                    layer["light_mask"] = found->second;
                }
                if (sdf_collision.contains(index)) {
                    layer["sdf_collision"] = true;
                }
                out_layers.push_back(std::move(layer));
            }
            layers.occlusion = out_layers.size();
            doc["occlusion_layers"] = std::move(out_layers);
        }
        // Custom data layers of the types a tile set holds; Godot's names are unique already.
        std::set<std::string> data_names;
        for (const auto& [index, layer] : data) {
            const auto type = custom_data_type(layer.second);
            if (!type) {
                problems_.add("tile set custom data layer type " +
                                  std::string(variant_type_name(layer.second)),
                              where,
                              "layer " + std::to_string(index) +
                                  " left out; layers hold bool, int, float, String, StringName, "
                                  "Vector2, Vector2i or Color");
                continue;
            }
            std::string layer_name = layer.first;
            if (layer_name.empty() || data_names.contains(layer_name)) {
                layer_name = "custom_data_" + std::to_string(index);
                problems_.add("unnamed or repeated custom data layer", where,
                              "named " + layer_name);
            }
            data_names.insert(layer_name);
            json::Value out_layer;
            out_layer["name"] = layer_name;
            out_layer["type"] = canvas::kCustomDataTypeNames.at(static_cast<std::size_t>(*type));
            doc["custom_data_layers"].push_back(std::move(out_layer));
            layers.data[index] = {.name = std::move(layer_name), .type = *type};
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
            json::Value source =
                atlas_source(*section, *owner, tile_size, shape, terrains, modes, layers);
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
                 const std::array<std::int64_t, 2>& tile_size, const canvas::TileSet& shape,
                 const std::map<std::pair<std::int64_t, std::int64_t>, std::string>& terrains,
                 const std::map<std::int64_t, std::int64_t>& modes, const TileLayers& layers) {
        const std::string where = resource_where(section, file);
        json::Value source;
        std::string why;
        const Value* texture = section.property("texture");
        std::string texture_path;
        if (texture != nullptr && canvas_texture(*texture, file, source, where)) {
            // Its normal and specular maps light the tiles, as in Godot.
            if (const json::Value* diffuse = source.find("texture"); diffuse != nullptr) {
                texture_path = diffuse->as_string();
            }
        } else if (texture != nullptr) {
            texture_path = resource_path(*texture, file, why);
        }
        if (texture_path.empty()) {
            problems_.add("tile set source texture", where, why.empty() ? "no texture" : why);
            return nullptr;
        }
        source["texture"] = texture_path;
        std::array<std::int64_t, 2> region{16, 16}; // Godot's default, whatever the tile size
        // A collision polygon by physics layer and polygon number.
        using Polygons = std::map<std::pair<std::int64_t, std::int64_t>, json::Value>;
        struct Tile {
            json::Value json;
            std::int64_t set = -1;
            std::int64_t terrain = -1;
            std::map<canvas::Neighbor, std::int64_t> bits;
            Polygons polygons;
            Polygons occluders; // by occlusion layer and polygon number
            std::map<std::int64_t, json::Value> alternatives;
            std::map<std::int64_t, Polygons> alternative_polygons;
            std::map<std::int64_t, Polygons> alternative_occluders;
            std::int64_t frames = 1;                  // animation_frames_count, if given
            std::map<std::int64_t, double> durations; // animation_frame_N/duration
        };
        // A tile's "physics_layer_N/..." property: polygons go into polygons, and constant
        // velocities other than zero are reported. False if it is another property.
        const auto physics_property = [&](std::span<const std::string_view> path,
                                          const Value& value, Polygons& polygons) {
            if (path.size() == 2 && path[0].starts_with("physics_layer_")) {
                if (path[1] == "linear_velocity" || path[1] == "angular_velocity") {
                    const auto numbers = value.numbers().value_or(std::vector<double>{});
                    if (value.number() != 0.0 ||
                        std::ranges::any_of(numbers, [](double n) { return n != 0.0; })) {
                        problems_.add("tile constant velocities", where,
                                      "tiles' bodies stand still; move the layer instead");
                    }
                    return true;
                }
                return path[1] == "polygons_count";
            }
            if (path.size() != 3 || !path[0].starts_with("physics_layer_") ||
                !path[1].starts_with("polygon_")) {
                return false;
            }
            const auto layer = parse_int(path[0].substr(std::string_view("physics_layer_").size()));
            const auto polygon = parse_int(path[1].substr(std::string_view("polygon_").size()));
            if (!layer || !polygon) {
                return false;
            }
            json::Value& out = polygons[{*layer, *polygon}];
            if (path[2] == "points") {
                json::Array points;
                const auto numbers = value.numbers().value_or(std::vector<double>{});
                for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
                    points.emplace_back(json::Array{numbers[i], numbers[i + 1]});
                }
                out["points"] = std::move(points);
            } else if (path[2] == "one_way") {
                out["one_way"] = value.boolean;
            } else if (path[2] == "one_way_margin") {
                out["one_way_margin"] = std::max(value.number(), 0.0);
            } else {
                return false;
            }
            if (*layer != 0) {
                out["layer"] = *layer;
            }
            return true;
        };
        // A tile's "occlusion_layer_N/polygon" (before Godot 4.4) or
        // "occlusion_layer_N/polygon_M/polygon": its OccluderPolygon2D goes into occluders. Godot
        // drops polygons on layers the tile set lacks, and so does this. False if it is another
        // property.
        const auto occlusion_property = [&](std::span<const std::string_view> path,
                                            const Value& value, Polygons& occluders) {
            if (path.empty() || !path[0].starts_with("occlusion_layer_")) {
                return false;
            }
            const auto layer =
                parse_int(path[0].substr(std::string_view("occlusion_layer_").size()));
            std::optional<std::int64_t> polygon;
            if (path.size() == 2 && path[1] == "polygon") {
                polygon = 0;
            } else if (path.size() == 3 && path[1].starts_with("polygon_") &&
                       path[2] == "polygon") {
                polygon = parse_int(path[1].substr(std::string_view("polygon_").size()));
            }
            if (!layer || !polygon || *layer < 0 || *polygon < 0) {
                return path.size() == 2 && path[1] == "polygons_count";
            }
            if (static_cast<std::uint64_t>(*layer) >= layers.occlusion) {
                return true;
            }
            const auto [occluder, owner] = resource_section(value, file);
            if (occluder == nullptr || resource_type(*occluder, *owner) != "OccluderPolygon2D") {
                return true; // no polygon
            }
            json::Value out;
            json::Array points;
            const Value* given = occluder->property("polygon");
            const auto numbers = given != nullptr ? given->numbers().value_or(std::vector<double>{})
                                                  : std::vector<double>{};
            for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
                points.emplace_back(json::Array{numbers[i], numbers[i + 1]});
            }
            const Value* closed = occluder->property("closed");
            const bool open =
                closed != nullptr && closed->kind == Value::Kind::Bool && !closed->boolean;
            // Godot draws no shadow from a closed polygon of fewer than three points.
            if (points.size() < (open ? 2U : 3U)) {
                return true;
            }
            out["points"] = std::move(points);
            if (*layer != 0) {
                out["layer"] = *layer;
            }
            if (open) {
                out["closed"] = false;
            }
            if (const Value* cull = occluder->property("cull_mode");
                cull != nullptr && cull->kind == Value::Kind::Int && cull->integer > 0 &&
                cull->integer < 3) {
                out["cull_mode"] =
                    canvas::kTileCullNames.at(static_cast<std::size_t>(cull->integer));
            }
            occluders[{*layer, *polygon}] = std::move(out);
            return true;
        };
        // A tile's or an alternative's "custom_data_N", into its custom_data by the layer's name,
        // converted to the layer's type as Godot converts it. Values equal to the type's default
        // are left out, as are layers the tile set lacks or that didn't carry over. False if it is
        // another property.
        const auto custom_data_property = [&](std::span<const std::string_view> path,
                                              const Value& value, json::Value& out) {
            if (path.size() != 1 || !path[0].starts_with("custom_data_")) {
                return false;
            }
            const auto index = parse_int(path[0].substr(std::string_view("custom_data_").size()));
            if (!index) {
                return false;
            }
            const auto layer = layers.data.find(*index);
            if (layer == layers.data.end()) {
                return true;
            }
            std::optional<json::Value> converted = custom_data_value(value, layer->second.type);
            if (!converted) {
                problems_.add("tile custom data value", where,
                              "a value that isn't " +
                                  std::string(canvas::kCustomDataTypeNames.at(
                                      static_cast<std::size_t>(layer->second.type))) +
                                  " in layer \"" + layer->second.name + "\"" +
                                  " takes the type's default");
                return true;
            }
            if (*converted != canvas::custom_data_default(layer->second.type)) {
                out["custom_data"][layer->second.name] = std::move(*converted);
            }
            return true;
        };
        // A tile's or an alternative's texture origin, a Vector2i, kept unless it is (0, 0).
        const auto texture_origin = [](const Value& value, json::Value& out) {
            constexpr double kLimit = 16384.0;
            if (const auto xy = value.numbers();
                xy && xy->size() == 2 && (xy->at(0) != 0.0 || xy->at(1) != 0.0)) {
                out["texture_origin"] =
                    json::Array{std::llround(std::clamp(xy->at(0), -kLimit, kLimit)),
                                std::llround(std::clamp(xy->at(1), -kLimit, kLimit))};
            }
        };
        // Polygons as a collision array, in Godot's order; those with no points are left out.
        const auto collision = [](const Polygons& polygons) {
            json::Array out;
            for (const auto& [index, polygon] : polygons) {
                if (polygon.find("points") != nullptr) {
                    out.push_back(polygon);
                }
            }
            return out;
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
            if (tile.json.is_null() &&
                (rest == "size_in_atlas" || rest.starts_with("animation_"))) {
                tile.json = json::Object{};
            }
            // Cell counts, kept to what a tile set takes.
            const auto cells = [](double number, double least) {
                return std::llround(std::clamp(number, least, 16384.0));
            };
            if (rest == "size_in_atlas") {
                if (const auto xy = value.numbers();
                    xy && xy->size() == 2 && (xy->at(0) != 1.0 || xy->at(1) != 1.0)) {
                    tile.json["size_in_atlas"] =
                        json::Array{cells(xy->at(0), 1.0), cells(xy->at(1), 1.0)};
                }
            } else if (rest == "animation_columns") {
                if (value.number() > 0.0) {
                    tile.json["animation_columns"] =
                        std::min<std::int64_t>(std::llround(value.number()), kMaxAnimationFrames);
                }
            } else if (rest == "animation_separation") {
                if (const auto xy = value.numbers();
                    xy && xy->size() == 2 && (xy->at(0) != 0.0 || xy->at(1) != 0.0)) {
                    tile.json["animation_separation"] =
                        json::Array{cells(xy->at(0), 0.0), cells(xy->at(1), 0.0)};
                }
            } else if (rest == "animation_speed") {
                if (value.number() > 0.0 && value.number() != 1.0) {
                    tile.json["animation_speed"] = value.number();
                }
            } else if (rest == "animation_mode") {
                if (value.number() == 1.0) {
                    tile.json["animation_mode"] = "random_start_times";
                }
            } else if (rest == "animation_frames_count") {
                tile.frames =
                    std::clamp<std::int64_t>(std::llround(value.number()), 1, kMaxAnimationFrames);
            } else if (parts.size() == 2 && parts[0].starts_with("animation_frame_") &&
                       parts[1] == "duration") {
                const auto frame =
                    parse_int(parts[0].substr(std::string_view("animation_frame_").size()));
                if (frame && *frame >= 0 && *frame < kMaxAnimationFrames) {
                    tile.durations[*frame] = value.number();
                }
            } else if (rest.starts_with("animation_")) {
                problems_.add("TileSetAtlasSource." + name, where);
            } else if (rest == "next_alternative_id") {
                continue;
            } else if (parts[0] != "0") {
                // An alternative tile: "column:row/1 = 1" makes it, then its own properties.
                const auto alternative = parse_int(parts[0]);
                if (!alternative || *alternative < 1 || *alternative > 4095) {
                    problems_.add("TileSetAtlasSource." + name, where);
                    continue;
                }
                json::Value& own = tile.alternatives[*alternative];
                if (own.is_null()) {
                    own = json::Object{};
                }
                const std::span<const std::string_view> path(parts.begin() + 1, parts.end());
                if (path.empty()) {
                    continue;
                }
                if (path.size() == 1 &&
                    (path[0] == "flip_h" || path[0] == "flip_v" || path[0] == "transpose")) {
                    if (value.boolean) {
                        own[std::string(path[0])] = true;
                    }
                } else if (path.size() == 1 && path[0] == "modulate") {
                    if (const auto rgba = value.numbers(); rgba && rgba->size() == 4) {
                        own["modulate"] =
                            json::Array{rgba->at(0), rgba->at(1), rgba->at(2), rgba->at(3)};
                    }
                } else if (path.size() == 1 &&
                           (path[0] == "texture_origin" || path[0] == "texture_offset")) {
                    texture_origin(value, own);
                } else if (!physics_property(path, value,
                                             tile.alternative_polygons[*alternative]) &&
                           !occlusion_property(path, value,
                                               tile.alternative_occluders[*alternative]) &&
                           !custom_data_property(path, value, own)) {
                    problems_.add("alternative tile property " + std::string(path[0]), where);
                }
            } else if (parts.size() == 2 && parts[1] == "terrain_set") {
                tile.set = static_cast<std::int64_t>(value.number());
            } else if (parts.size() == 2 && parts[1] == "terrain") {
                tile.terrain = static_cast<std::int64_t>(value.number());
            } else if (parts.size() == 3 && parts[1] == "terrains_peering_bit") {
                if (const auto direction = peering_direction(parts[2], shape)) {
                    tile.bits[*direction] = static_cast<std::int64_t>(value.number());
                }
            } else if (parts.size() == 2 && parts[1] == "probability") {
                tile.json["probability"] = value.number();
            } else if (parts.size() == 2 &&
                       (parts[1] == "texture_origin" || parts[1] == "texture_offset")) {
                // texture_offset is its old name, which Godot still reads.
                if (tile.json.is_null()) {
                    tile.json = json::Object{};
                }
                texture_origin(value, tile.json);
            } else if (parts.size() > 1 &&
                       !physics_property(std::span<const std::string_view>(parts).subspan(1), value,
                                         tile.polygons) &&
                       !occlusion_property(std::span<const std::string_view>(parts).subspan(1),
                                           value, tile.occluders) &&
                       !custom_data_property(std::span<const std::string_view>(parts).subspan(1),
                                             value, tile.json)) {
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
                for (std::size_t d = 0; d < canvas::kNeighborNames.size(); ++d) {
                    const auto direction = static_cast<canvas::Neighbor>(d);
                    if (!connects(mode != modes.end() ? mode->second : 0, shape, direction)) {
                        continue;
                    }
                    const auto bit = tile.bits.find(direction);
                    const std::int64_t terrain = bit != tile.bits.end() ? bit->second : -1;
                    if (terrain == tile.terrain) {
                        continue; // left out: the tile's own terrain
                    }
                    const auto other = terrains.find({tile.set, terrain});
                    peering[canvas::kNeighborNames.at(d)] =
                        other != terrains.end() ? json::Value(other->second) : json::Value(nullptr);
                }
                if (peering.size() > 0) {
                    tile.json["peering"] = std::move(peering);
                }
            }
            if (json::Array polygons = collision(tile.polygons); !polygons.empty()) {
                tile.json["collision"] = std::move(polygons);
            }
            if (json::Array occluders = collision(tile.occluders); !occluders.empty()) {
                tile.json["occlusion"] = std::move(occluders);
            }
            for (auto& [number, alternative] : tile.alternatives) {
                if (json::Array polygons = collision(tile.alternative_polygons[number]);
                    !polygons.empty()) {
                    alternative["collision"] = std::move(polygons);
                }
                if (json::Array occluders = collision(tile.alternative_occluders[number]);
                    !occluders.empty()) {
                    alternative["occlusion"] = std::move(occluders);
                }
                tile.json["alternatives"][std::to_string(number)] = std::move(alternative);
            }
            // Godot stores each frame's duration once there are two frames or more.
            std::int64_t frames = tile.frames;
            if (!tile.durations.empty()) {
                frames = std::max(frames, tile.durations.rbegin()->first + 1);
            }
            if (frames > 1) {
                json::Array durations;
                for (std::int64_t frame = 0; frame < frames; ++frame) {
                    const auto given = tile.durations.find(frame);
                    // Godot refuses durations of 0 or less; such a frame keeps the default.
                    durations.emplace_back(
                        given != tile.durations.end() && given->second > 0.0 ? given->second : 1.0);
                }
                tile.json["animation_frames"] = std::move(durations);
            } else if (tile.json.is_object()) {
                // One frame: where the others would go doesn't matter.
                for (const std::string_view key : {"animation_columns", "animation_separation",
                                                   "animation_speed", "animation_mode"}) {
                    tile.json.as_object().erase(std::string(key));
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
            // Godot keeps a cell's flips above its alternative's number: TRANSFORM_FLIP_H
            // (bit 12), TRANSFORM_FLIP_V and TRANSFORM_TRANSPOSE.
            canvas::TileRef tile{.source =
                                     source != tiles.sources.end() ? source->second : std::string(),
                                 .atlas = {cell.atlas_x, cell.atlas_y},
                                 .alternative = cell.alternative & 0x0FFF,
                                 .flip_h = (cell.alternative & 0x1000) != 0,
                                 .flip_v = (cell.alternative & 0x2000) != 0,
                                 .transpose = (cell.alternative & 0x4000) != 0};
            if (source == tiles.sources.end() || tiles.set->find(tile) == nullptr) {
                problems_.add("cells with tiles missing from their tile set", where);
                continue;
            }
            if (tile.alternative != 0 && tiles.set->alternative(tile) == nullptr) {
                problems_.add("cells with alternative tiles missing from their tile set", where,
                              "they show the base tile");
                tile.alternative = 0;
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
            {"AudioEffectPitchShift", "pitch_shift"},
            {"AudioEffectSpectrumAnalyzer", "spectrum_analyzer"},
            {"AudioEffectCapture", "capture"},
            {"AudioEffectRecord", "record"},
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
            } else if (name == "fft_size" && value.kind == Value::Kind::Int && value.integer >= 0 &&
                       value.integer < 5) {
                effect["fft_size"] = std::int64_t{256} << value.integer; // FFT_SIZE_256 is 0
            } else if (name == "format" && type == "AudioEffectRecord" &&
                       value.kind == Value::Kind::Int) {
                if (value.integer == 0 || value.integer == 1) {
                    effect["format"] = value.integer == 0 ? "8_bits" : "16_bits";
                } else {
                    problems_.add("AudioEffectRecord.format", resource_where(*section, *owner),
                                  "compressed formats record at 16 bits");
                }
            } else if (name == "oversampling" && value.is_number()) {
                effect["oversampling"] = static_cast<std::int64_t>(value.number());
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

    // A CanvasTexture, as the fields Sprite2D and tile set sources share: its diffuse texture
    // as texture, its normal and specular textures, and its specular color and shininess. False,
    // writing nothing, if the reference is to anything else.
    bool canvas_texture(const Value& reference, const TextFile& file, json::Value& into,
                        const std::string& where) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr || resource_type(*section, *owner) != "CanvasTexture") {
            return false;
        }
        for (const auto& [from, to] : {std::pair{"diffuse_texture", "texture"},
                                       std::pair{"normal_texture", "normal_texture"},
                                       std::pair{"specular_texture", "specular_texture"}}) {
            const Value* texture = section->property(from);
            if (texture == nullptr || texture->kind == Value::Kind::Nil) {
                continue;
            }
            std::string why;
            if (std::string path = resource_path(*texture, *owner, why); !path.empty()) {
                into[to] = std::move(path);
            } else {
                problems_.add(std::string("CanvasTexture ") + from, where, why);
            }
        }
        if (const Value* color = section->property("specular_color"); color != nullptr) {
            if (const auto rgba = color->numbers(); rgba && rgba->size() == 4) {
                into["specular_color"] =
                    json::Array{rgba->at(0), rgba->at(1), rgba->at(2), rgba->at(3)};
            }
        }
        if (const Value* shininess = section->property("specular_shininess");
            shininess != nullptr) {
            into["specular_shininess"] = shininess->number();
        }
        for (const char* setting : {"texture_filter", "texture_repeat"}) {
            if (section->property(setting) != nullptr) {
                problems_.add(std::string("CanvasTexture ") + setting, where,
                              "the node's own setting is used");
            }
        }
        return true;
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
        auto bytes = read(sheet_file);
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

    // A ParticleProcessMaterial as a .process_material.json file, its 3D vectors cut to 2D and
    // its CurveTextures and GradientTextures to their curves and gradients.
    std::string process_material_file(const TextFile& file, const Section& material,
                                      const std::string& base) {
        const std::string key = file.path + "#" + base;
        if (const auto done = process_written_.find(key); done != process_written_.end()) {
            return done->second;
        }
        const std::string path = destination(base + std::string(canvas::kProcessMaterialSuffix));
        process_written_[key] = path;
        resources_used_.insert("ParticleProcessMaterial");
        const std::string where = resource_where(material, file);
        static constexpr std::array<std::string_view, 7> kShapes{
            "point", "sphere", "sphere_surface", "box", "points", "directed_points", "ring"};
        static constexpr std::array<std::string_view, 15> kParams{
            "initial_velocity",
            "angular_velocity",
            "orbit_velocity",
            "radial_velocity",
            "linear_accel",
            "radial_accel",
            "tangential_accel",
            "damping",
            "angle",
            "scale",
            "hue_variation",
            "anim_speed",
            "anim_offset",
            "turbulence_influence",
            "turbulence_initial_displacement"};
        static constexpr std::array<std::string_view, 5> kVectors{
            "emission_shape_offset", "emission_shape_scale", "emission_box_extents", "direction",
            "velocity_pivot"};
        static constexpr std::array<std::string_view, 10> kNumbers{
            "emission_sphere_radius",        "emission_ring_radius",
            "emission_ring_inner_radius",    "spread",
            "inherit_velocity_ratio",        "lifetime_randomness",
            "turbulence_noise_strength",     "turbulence_noise_scale",
            "turbulence_noise_speed_random", "sub_emitter_frequency"};
        // What a 2D game can't see, or what particles do without.
        static constexpr std::array<std::string_view, 8> kIgnored{"resource_local_to_scene",
                                                                  "resource_name",
                                                                  "particle_flag_disable_z",
                                                                  "particle_flag_rotate_y",
                                                                  "flatness",
                                                                  "emission_ring_height",
                                                                  "emission_ring_cone_angle",
                                                                  "attractor_interaction_enabled"};
        static constexpr std::array<std::string_view, 3> kCollisionModes{"disabled", "rigid",
                                                                         "hide_on_contact"};
        static constexpr std::array<std::string_view, 5> kSubEmitterModes{
            "disabled", "constant", "at_end", "at_collision", "at_start"};
        json::Value out;
        out["format"] = 1;
        bool disable_z = false;
        const auto texture_resource = [&](const Value& value, std::string_view type,
                                          std::string_view property) -> const Value* {
            const auto [section, owner] = resource_section(value, file);
            if (section == nullptr || owner != &file || resource_type(*section, file) != type) {
                return nullptr;
            }
            return section->property(property);
        };
        for (const auto& [name, value] : material.properties) {
            std::string why;
            const auto vector = value.numbers();
            if (contains(kIgnored, std::string_view(name))) {
                disable_z = disable_z || (name == "particle_flag_disable_z" && value.boolean);
                continue;
            }
            if (name == "emission_shape" && value.kind == Value::Kind::Int && value.integer >= 0 &&
                value.integer < 7) {
                out[name] = kShapes.at(static_cast<std::size_t>(value.integer));
            } else if ((contains(kVectors, std::string_view(name)) || name == "gravity") &&
                       vector && vector->size() >= 2) {
                out[name] = json::Array{vector->at(0), vector->at(1)};
            } else if (name == "emission_ring_axis") {
                if (!vector || vector->size() != 3 || vector->at(0) != 0.0 ||
                    vector->at(1) != 0.0) {
                    problems_.add("ParticleProcessMaterial.emission_ring_axis", where,
                                  "rings face the screen");
                }
            } else if ((contains(kNumbers, std::string_view(name)) ||
                        std::ranges::any_of(kParams,
                                            [&](std::string_view param) {
                                                return name == std::string(param) + "_min" ||
                                                       name == std::string(param) + "_max";
                                            })) &&
                       value.is_number()) {
                out[name] = value.number();
            } else if ((name == "collision_friction" || name == "collision_bounce") &&
                       value.is_number()) {
                out[name] = std::clamp(value.number(), 0.0, 1.0);
            } else if (name == "collision_mode" && value.kind == Value::Kind::Int &&
                       value.integer >= 0 && value.integer < 3) {
                out[name] = kCollisionModes.at(static_cast<std::size_t>(value.integer));
            } else if ((name == "particle_flag_align_y" ||
                        name == "particle_flag_damping_as_friction" ||
                        name == "turbulence_enabled" || name == "collision_use_scale" ||
                        name == "sub_emitter_keep_velocity") &&
                       value.kind == Value::Kind::Bool) {
                out[name] = value.boolean;
            } else if (name == "turbulence_noise_speed" && vector && vector->size() == 3) {
                out[name] = json::Array{vector->at(0), vector->at(1), vector->at(2)};
            } else if (name == "color" && vector && vector->size() == 4) {
                out[name] = json::Array{vector->at(0), vector->at(1), vector->at(2), vector->at(3)};
            } else if (name == "turbulence_influence_over_life" ||
                       (name.ends_with("_curve") &&
                        (name == "velocity_limit_curve" || name == "alpha_curve" ||
                         name == "emission_curve" ||
                         std::ranges::any_of(kParams, [&](std::string_view param) {
                             return param != "initial_velocity" &&
                                    !param.starts_with("turbulence_") &&
                                    name == std::string(param) + "_curve";
                         })))) {
                const Value* curve = texture_resource(value, "CurveTexture", "curve");
                if (curve == nullptr) {
                    problems_.add("ParticleProcessMaterial." + name, where,
                                  "only a CurveTexture of one curve converts");
                } else if (auto points = curve_points(*curve, file, why); points) {
                    out[name] = std::move(*points);
                } else {
                    problems_.add("ParticleProcessMaterial." + name, where, why);
                }
            } else if (name == "color_ramp" || name == "color_initial_ramp") {
                const Value* gradient = texture_resource(value, "GradientTexture1D", "gradient");
                if (gradient == nullptr) {
                    problems_.add("ParticleProcessMaterial." + name, where,
                                  "only a GradientTexture1D converts");
                } else if (auto points = gradient_points(*gradient, file, why); points) {
                    out[name] = std::move(*points);
                } else {
                    problems_.add("ParticleProcessMaterial." + name, where, why);
                }
            } else if (name.starts_with("emission_point_") || name == "emission_normal_texture" ||
                       name == "emission_color_texture") {
                if (name == "emission_point_texture") {
                    problems_.add("ParticleProcessMaterial emission points", where,
                                  "points in a texture don't convert; list them as "
                                  "emission_points");
                }
            } else if (name == "sub_emitter_mode" && value.kind == Value::Kind::Int &&
                       value.integer >= 0 && value.integer < 5) {
                out[name] = kSubEmitterModes.at(static_cast<std::size_t>(value.integer));
            } else if ((name == "sub_emitter_amount_at_end" ||
                        name == "sub_emitter_amount_at_collision" ||
                        name == "sub_emitter_amount_at_start") &&
                       value.kind == Value::Kind::Int) {
                out[name] = std::max<std::int64_t>(value.integer, 1);
            } else {
                problems_.add("ParticleProcessMaterial." + name, where);
            }
        }
        // Godot makes a material with the 3D default gravity a 2D one with 98 pixels a second
        // squared down, unless it was already 2D.
        if (out.find("gravity") == nullptr) {
            out["gravity"] = disable_z ? json::Array{0.0, -9.8} : json::Array{0.0, 98.0};
        }
        for (const json::ValidationError& error : canvas::validate_process_material(out)) {
            problems_.add("ParticleProcessMaterial", where, error.path + ": " + error.message);
        }
        add_file(path, json::write(out) + "\n");
        ++process_files_;
        return path;
    }

    // A CPUParticles2D's or GPUParticles2D's CanvasItemMaterial: its particle animation's frames.
    void particle_material(const Value& reference, const TextFile& file, json::Value& emitter,
                           const Node& node) {
        const auto [section, owner] = resource_section(reference, file);
        if (section == nullptr || resource_type(*section, *owner) != "CanvasItemMaterial") {
            problems_.add(node.type + ".material", node.declared,
                          "only a CanvasItemMaterial's particle animation converts");
            return;
        }
        const Value* animated = section->property("particles_animation");
        for (const auto& [name, value] : section->properties) {
            if (name == "particles_anim_h_frames" || name == "particles_anim_v_frames") {
                if (animated != nullptr && animated->boolean && value.kind == Value::Kind::Int) {
                    emitter[name == "particles_anim_h_frames" ? "hframes" : "vframes"] =
                        value.integer;
                }
            } else if (name == "particles_anim_loop") {
                if (animated != nullptr && animated->boolean && value.kind == Value::Kind::Bool) {
                    emitter["anim_loop"] = value.boolean;
                }
            } else if ((name == "blend_mode" || name == "light_mode") && value.integer != 0) {
                problems_.add(node.type + ".material " + name, node.declared,
                              "write a .material.json material for it");
            } else if (name != "particles_animation" && name != "resource_local_to_scene" &&
                       name != "resource_name") {
                problems_.add(node.type + ".material " + name, node.declared);
            }
        }
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

    // A PopupMenu's, OptionButton's or MenuButton's items: item_count, then item_<n>/text, icon,
    // checkable, checked, id, disabled and separator, under "popup/" on a button.
    bool item_property(const Node& node, const Property& property, json::Object& components) {
        const char* owner = nullptr;
        for (const char* each : {"PopupMenu", "OptionButton", "MenuButton"}) {
            if (components.contains(each)) {
                owner = each;
                break;
            }
        }
        if (owner == nullptr) {
            return false;
        }
        const std::string_view kind = owner;
        std::string_view name = property.name;
        const Value& value = *property.value;
        json::Value& items = components[owner]["items"];
        if (items.is_null()) {
            items = json::Array{};
        }
        const auto grow = [&](std::size_t count) {
            while (items.size() < count) {
                items.push_back(json::Object{});
            }
        };
        if (name == "item_count") {
            if (value.kind == Value::Kind::Int && value.integer > 0) {
                grow(static_cast<std::size_t>(value.integer));
            }
            return true;
        }
        const std::string_view prefix = kind == "PopupMenu" ? "item_" : "popup/item_";
        if (!name.starts_with(prefix)) {
            return false;
        }
        name.remove_prefix(prefix.size());
        const auto slash = name.find('/');
        if (slash == std::string_view::npos) {
            return false;
        }
        std::size_t index = 0;
        const auto [end, error] = std::from_chars(name.data(), name.data() + slash, index);
        if (error != std::errc{} || end != name.data() + slash || index > 4096) {
            return false;
        }
        const std::string_view field = name.substr(slash + 1);
        grow(index + 1);
        json::Value& item = items.as_array()[index];
        // Reported once for every item.
        const std::string where = node.type + "." + std::string(prefix) + "*/" + std::string(field);
        const bool option = kind == "OptionButton";
        if (field == "text" && value.is_string()) {
            item["text"] = value.text;
        } else if (field == "icon") {
            std::string why;
            if (std::string path = resource_path(value, *property.file, why); !path.empty()) {
                item["icon"] = std::move(path);
            } else {
                problems_.add(where, node.declared, why);
            }
        } else if (field == "checkable" && !option && value.kind == Value::Kind::Int &&
                   value.integer >= 0 && value.integer <= 2) {
            static constexpr std::array<std::string_view, 3> kCheckable{"none", "check", "radio"};
            item["checkable"] = kCheckable.at(static_cast<std::size_t>(value.integer));
        } else if (field == "id" && value.kind == Value::Kind::Int) {
            // Godot gives an item its index as its ID; -1 says the same here.
            if (value.integer != static_cast<std::int64_t>(index)) {
                item["id"] = value.integer;
            }
        } else if ((field == "disabled" || field == "separator" ||
                    (field == "checked" && !option)) &&
                   value.kind == Value::Kind::Bool) {
            item[std::string(field)] = value.boolean;
        } else {
            problems_.add(where, node.declared);
        }
        return true;
    }

    // Godot properties that don't match a field one to one.
    bool special_property(const Node& node, const Property& property, json::Object& components) {
        const std::string& name = property.name;
        const Value& value = *property.value;
        const TextFile& file = *property.file;
        std::string why;
        if (item_property(node, property, components)) {
            return true;
        }
        // An OptionButton shows its selected option's text and icon.
        if (components.contains("OptionButton") && (name == "text" || name == "icon")) {
            return true;
        }
        if (components.contains("PopupMenu") && name == "size") {
            problems_.add("PopupMenu.size", node.declared, "a popup menu is as big as its items");
            return true;
        }
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
        if (name == "texture" && components.contains("Sprite2D") &&
            canvas_texture(value, file, components["Sprite2D"], node.declared)) {
            return true;
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
        if (components.contains("AnimationPlayer") && node.type == "AnimationPlayer") {
            json::Value& player = components["AnimationPlayer"];
            if (name == "autoplay") {
                if (value.is_string() && !value.text.empty()) {
                    player["animation"] = value.text;
                }
                return true;
            }
            if (name == "active") {
                if (value.kind == Value::Kind::Bool && !value.boolean) {
                    player["playing"] = false;
                }
                return true;
            }
            if (name == "playback_default_blend_time") {
                if (value.number() != 0.0) {
                    problems_.add("AnimationPlayer.playback_default_blend_time", node.declared,
                                  "animations switch at once, without blending");
                }
                return true;
            }
            // Read by convert_player, or with no effect on what plays.
            constexpr std::array kTaken{
                std::string_view{"libraries"},
                std::string_view{"root_node"},
                std::string_view{"current_animation"},
                std::string_view{"assigned_animation"},
                std::string_view{"deterministic"},
                std::string_view{"reset_on_save"},
                std::string_view{"callback_mode_process"},
                std::string_view{"callback_mode_method"},
                std::string_view{"callback_mode_discrete"},
                std::string_view{"movie_quit_on_finish"},
                std::string_view{"audio_max_polyphony"},
            };
            if (contains(kTaken, std::string_view(name)) ||
                name.starts_with("playback_auto_capture")) {
                return true;
            }
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
        const char* particles = nullptr;
        if (components.contains("GPUParticles2D") && node.type == "GPUParticles2D") {
            particles = "GPUParticles2D";
        } else if (components.contains("CPUParticles2D") && node.type == "CPUParticles2D") {
            particles = "CPUParticles2D";
        }
        if (particles != nullptr && name == "material") {
            particle_material(value, file, components[particles], node);
            return true;
        }
        if (particles != nullptr && std::string_view(particles) == "GPUParticles2D") {
            // visibility_rect only culls.
            if (name == "visibility_rect") {
                return true;
            }
            if (name == "seed") {
                // Godot's GPUParticles2D picks a seed at random unless use_fixed_seed.
                const Property* fixed = node.find("use_fixed_seed");
                if (fixed != nullptr && fixed->value->boolean && value.kind == Value::Kind::Int) {
                    components[particles]["seed"] =
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(value.integer));
                }
                return true;
            }
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
        // CanvasLayer saves its offset, rotation and scale as a transform too.
        if (components.contains("CanvasLayer") && name == "transform") {
            return true;
        }
        if (node.type == "ParallaxBackground" && !components.contains("CanvasLayer") &&
            (name == "layer" || name == "offset" || name == "transform" ||
             name.starts_with("follow_viewport"))) {
            if (name != "transform") {
                problems_.add("ParallaxBackground." + name, node.declared,
                              "drawn with the world, as Godot's converter makes a "
                              "ParallaxBackground that follows the camera's zoom");
            }
            return true;
        }
        // Read by convert_parallax_layer; the camera sets a ParallaxBackground's scroll_offset.
        if ((node.type == "ParallaxBackground" && name.starts_with("scroll_")) ||
            (node.type == "ParallaxLayer" && (name.starts_with("motion_") || name == "position"))) {
            return true;
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

    // --- Animations ---

    // An animation library as converted: its file, and the node paths its tracks name from the
    // player's root node.
    struct LibraryFile {
        std::string path;
        std::set<std::string> targets;
    };

    // One track of a Godot Animation: its "tracks/<n>/..." properties by name.
    struct GodotTrack {
        std::map<std::string, const Value*, std::less<>> fields;

        [[nodiscard]] const Value* get(std::string_view name) const {
            const auto it = fields.find(name);
            return it != fields.end() ? it->second : nullptr;
        }
    };

    static std::map<std::int64_t, GodotTrack> animation_tracks(const Section& section) {
        constexpr std::string_view kPrefix = "tracks/";
        std::map<std::int64_t, GodotTrack> tracks;
        for (const auto& [name, value] : section.properties) {
            const std::string_view property = name;
            if (!property.starts_with(kPrefix)) {
                continue;
            }
            const std::size_t slash = property.find('/', kPrefix.size());
            if (slash == std::string_view::npos) {
                continue;
            }
            if (const auto index =
                    parse_int(property.substr(kPrefix.size(), slash - kPrefix.size()))) {
                tracks[*index].fields.emplace(std::string(property.substr(slash + 1)), &value);
            }
        }
        return tracks;
    }

    // A path value's text: NodePath("a:b") or a plain string.
    static std::string path_text(const Value& value) {
        if (value.is_string()) {
            return value.text;
        }
        return value.string_argument().value_or("");
    }

    using OrderOf = std::function<std::vector<std::string>(const Node&)>;

    // Fills an AnimationPlayer from its node: its animation libraries as one .anim.json file, and
    // the entity each node path its tracks name stands for, from the player's root node.
    void convert_player(const std::vector<Node>& nodes, const Node& node, const OrderOf& order_of,
                        const SceneContext& scene, json::Value& player) {
        const Property* libraries = node.find("libraries");
        if (libraries == nullptr || libraries->value->kind != Value::Kind::Dictionary ||
            libraries->value->entries.empty()) {
            return;
        }
        std::string root_path = "..";
        if (const Property* root = node.find("root_node"); root != nullptr) {
            root_path = path_text(*root->value);
        }
        const Node* root = find_node(nodes, node, root_path);
        if (root == nullptr) {
            problems_.add("AnimationPlayer.root_node", node.declared,
                          "node path " + root_path + " names no node in the scene");
            return;
        }
        const LibraryFile* library = library_file(nodes, node, *libraries, *root, order_of, scene);
        if (library == nullptr) {
            return;
        }
        player["library"] = library->path;
        json::Object targets;
        for (const std::string& target : library->targets) {
            const Node* found = find_node(nodes, *root, target);
            if (found == nullptr) {
                problems_.add("animation track target", node.declared,
                              "node path " + target + " names no node under the root node");
                targets.emplace(target, nullptr);
            } else {
                targets.emplace(target, found->id);
            }
        }
        if (!targets.empty()) {
            player["targets"] = std::move(targets);
        }
    }

    // Writes the animations of a player's libraries as one .anim.json file once: a library's
    // own beside it, or several libraries' together after the player. Animations of a library
    // with a name are "<library>/<animation>", as Godot names them.
    const LibraryFile* library_file(const std::vector<Node>& nodes, const Node& node,
                                    const Property& libraries, const Node& root,
                                    const OrderOf& order_of, const SceneContext& scene) {
        struct Source {
            std::string name;
            const Section* section;
            const TextFile* file;
        };
        std::vector<Source> animations;
        std::string base;
        int count = 0;
        for (const Value::Entry& entry : libraries.value->entries) {
            const auto [section, owner] = resource_section(entry.value, *libraries.file);
            if (section == nullptr || resource_type(*section, *owner) != "AnimationLibrary") {
                problems_.add("animation library", node.declared,
                              section == nullptr ? "a missing resource"
                                                 : "not an AnimationLibrary");
                continue;
            }
            if (++count == 1) {
                base = resource_base(*section, *owner);
            }
            const Value* data = section->property("_data");
            if (data == nullptr || data->kind != Value::Kind::Dictionary) {
                continue;
            }
            for (const Value::Entry& item : data->entries) {
                const auto [animation, file] = resource_section(item.value, *owner);
                if (animation == nullptr || resource_type(*animation, *file) != "Animation") {
                    problems_.add("animation",
                                  resource_where(*section, *owner) + ": " + item.key.text,
                                  animation == nullptr ? "a missing resource" : "not an Animation");
                    continue;
                }
                std::string name;
                if (!entry.key.text.empty()) {
                    name.append(entry.key.text).append("/");
                }
                name += item.key.text;
                animations.push_back({std::move(name), animation, file});
            }
        }
        if (count == 0) {
            return nullptr;
        }
        if (count > 1) {
            base = without_extension(libraries.file->path) + "_" + snake_case(node.name);
        }
        if (const auto done = libraries_written_.find(base); done != libraries_written_.end()) {
            return &done->second;
        }
        LibraryFile& out = libraries_written_[base];
        out.path = destination(base + std::string(animation::kAnimationSuffix));
        json::Object converted;
        for (const Source& source : animations) {
            converted.emplace(source.name, convert_animation(*source.section, *source.file, nodes,
                                                             root, order_of, scene, out.targets));
        }
        json::Value doc;
        doc["format"] = 1;
        doc["animations"] = std::move(converted);
        keep_valid(doc, out.path, [this](const json::Value& value) {
            return animation::validate_animation_library(value, &components_);
        });
        add_file(out.path, json::write(doc) + "\n");
        ++animation_files_;
        return &out;
    }

    json::Value convert_animation(const Section& section, const TextFile& file,
                                  const std::vector<Node>& nodes, const Node& root,
                                  const OrderOf& order_of, const SceneContext& scene,
                                  std::set<std::string>& targets) {
        const std::string where = resource_where(section, file);
        json::Value out;
        double length = 1.0; // Godot's default
        if (const Value* value = section.property("length");
            value != nullptr && value->is_number()) {
            length = value->number();
        }
        out["length"] = length;
        if (const Value* loop = section.property("loop_mode");
            loop != nullptr && loop->kind == Value::Kind::Int &&
            (loop->integer == 1 || loop->integer == 2)) {
            out["loop"] = loop->integer == 1 ? "linear" : "pingpong";
        }
        if (section.property("marker_names") != nullptr) {
            problems_.add("animation markers", where, "play from a marker's time in gameplay code");
        }
        json::Array tracks;
        for (const auto& [index, track] : animation_tracks(section)) {
            convert_track(track, where + " track " + std::to_string(index), length, file, nodes,
                          root, order_of, scene, tracks, targets);
        }
        out["tracks"] = std::move(tracks);
        return out;
    }

    // Why a kind of Godot track doesn't convert.
    static std::string track_why(std::string_view type) {
        if (type == "audio") {
            return "play the sound from gameplay code, on an event track's event";
        }
        if (type == "animation") {
            return "start the other AnimationPlayer from gameplay code, on an event track's event";
        }
        if (type.ends_with("_3d") || type == "blend_shape") {
            return "3D comes with milestone 7";
        }
        return "no SlashEngine track of this kind";
    }

    // Adds a Godot track's equivalent to tracks: a value track for a value or bezier track, and
    // an event track for each method a method track calls.
    void convert_track(const GodotTrack& track, const std::string& where, double length,
                       const TextFile& file, const std::vector<Node>& nodes, const Node& root,
                       const OrderOf& order_of, const SceneContext& scene, json::Array& tracks,
                       std::set<std::string>& targets) {
        const Value* type_value = track.get("type");
        const std::string type =
            type_value != nullptr && type_value->is_string() ? type_value->text : "value";
        if (const Value* enabled = track.get("enabled");
            enabled != nullptr && enabled->kind == Value::Kind::Bool && !enabled->boolean) {
            problems_.add("disabled animation track", where, "Godot doesn't play it either");
            return;
        }
        if (type != "value" && type != "bezier" && type != "method") {
            problems_.add("animation " + type + " track", where, track_why(type));
            return;
        }
        const Value* path_value = track.get("path");
        const std::string path = path_value != nullptr ? path_text(*path_value) : std::string();
        const std::size_t colon = path.find(':');
        std::string node_path = path.substr(0, colon);
        if (node_path.empty()) {
            node_path = ".";
        }
        const Node* target = find_node(nodes, root, node_path);
        if (target == nullptr) {
            problems_.add("animation track target", where,
                          "node path " + node_path + " names no node under the root node");
            return;
        }
        const Value* keys = track.get("keys");
        const Value* times_value = keys != nullptr ? keys->get("times") : nullptr;
        const auto times =
            times_value != nullptr ? times_value->numbers() : std::optional<std::vector<double>>{};
        if (!times || times->empty()) {
            return;
        }
        // Keys past the end never play.
        constexpr double kPastEnd = 1.0e-9;
        const auto playable = [&](std::size_t i) { return times->at(i) <= length + kPastEnd; };
        const Value* values = keys->get("values");

        if (type == "method") {
            std::vector<json::Value> events; // one per method, in the order first called
            for (std::size_t i = 0; i < times->size(); ++i) {
                const Value* call =
                    values != nullptr && i < values->items.size() ? &values->items[i] : nullptr;
                const Value* method = call != nullptr ? call->get("method") : nullptr;
                if (!playable(i) || method == nullptr || !method->is_string() ||
                    method->text.empty()) {
                    continue;
                }
                json::Value key;
                key["time"] = times->at(i);
                if (const Value* args = call->get("args");
                    args != nullptr && args->kind == Value::Kind::Array && !args->items.empty()) {
                    json::Array converted;
                    for (const Value& arg : args->items) {
                        std::string why;
                        auto value = guess(arg, file, why);
                        converted.push_back(value ? std::move(*value) : json::Value());
                    }
                    key["data"]["args"] = std::move(converted);
                }
                json::Value* event = nullptr;
                for (json::Value& candidate : events) {
                    if (candidate.find("event")->as_string() == method->text) {
                        event = &candidate;
                    }
                }
                if (event == nullptr) {
                    event = &events.emplace_back();
                    (*event)["event"] = method->text;
                    (*event)["target"] = node_path;
                }
                (*event)["keys"].push_back(std::move(key));
            }
            for (json::Value& event : events) {
                tracks.push_back(std::move(event));
            }
            if (!events.empty()) {
                targets.insert(node_path);
            }
            return;
        }

        std::string property = colon == std::string::npos ? std::string() : path.substr(colon + 1);
        std::vector<std::string> sub;
        if (const std::size_t more = property.find(':'); more != std::string::npos) {
            for (const std::string_view part :
                 split_path(std::string_view(property).substr(more + 1), ':')) {
                sub.emplace_back(part);
            }
            property.resize(more);
        }
        if (property.empty()) {
            problems_.add("animation track", where, "a node path with no property");
            return;
        }
        const std::string what = "animation track " + target->type + "." + property;
        const std::vector<std::string> order = order_of(*target);
        const std::optional<FieldOf> field = property_field(order, property);
        if (!field) {
            const bool control = contains(order, std::string("Control"));
            problems_.add(what, where,
                          control && (property == "position" || property == "size" ||
                                      property == "global_position")
                              ? "animate the control's offsets instead"
                              : "no SlashEngine field for it");
            return;
        }
        std::string field_path = field->component + "/" + field->field;
        if (!sub.empty()) {
            constexpr std::array kParts{"x", "y", "z", "w", "r", "g", "b", "a"};
            if (sub.size() != 1 || !field->fallback.is_array() ||
                std::ranges::find(kParts, sub.front()) == kParts.end()) {
                problems_.add(what + ":" + join(sub, ":"), where, "no SlashEngine part for it");
                return;
            }
            field_path += "/" + sub.front();
        }
        auto resolved = animation::resolve_target(field_path, components_);
        if (!resolved) {
            problems_.add(what, where, resolved.error().message);
            return;
        }
        std::optional<std::vector<double>> points; // a bezier track's: value and handles per key
        if (const Value* list = keys->get("points"); type == "bezier" && list != nullptr) {
            points = list->numbers();
        }
        const Value* transitions_value = keys->get("transitions");
        const auto transitions = transitions_value != nullptr
                                     ? transitions_value->numbers()
                                     : std::optional<std::vector<double>>{};
        SceneContext context = scene;
        context.node = target;
        json::Array out_keys;
        bool numeric = true;
        for (std::size_t i = 0; i < times->size(); ++i) {
            if (!playable(i)) {
                continue;
            }
            json::Value key;
            key["time"] = times->at(i);
            constexpr std::size_t kPoint = 5;
            if (type == "bezier") {
                if (!points || points->size() < (i + 1) * kPoint) {
                    problems_.add(what, where, "a bezier key with no point");
                    return;
                }
                key["value"] = points->at(i * kPoint);
            } else if (values == nullptr || i >= values->items.size()) {
                problems_.add(what, where, "a key with no value");
                return;
            } else if (!sub.empty()) {
                if (!values->items[i].is_number()) {
                    problems_.add(what, where, "not a number");
                    return;
                }
                key["value"] = values->items[i].number();
            } else {
                std::string why;
                auto converted = field_value(field->component, field->field, field->fallback,
                                             values->items[i], file, context, why);
                if (!converted) {
                    problems_.add(what, where, why);
                    return;
                }
                const bool number_array =
                    converted->is_array() && !converted->as_array().empty() &&
                    std::ranges::all_of(converted->as_array(), &json::Value::is_number);
                numeric = numeric && (converted->is_number() || number_array);
                key["value"] = std::move(*converted);
            }
            if (type != "bezier" && transitions && i < transitions->size() &&
                transitions->at(i) != 1.0) {
                key["transition"] = transitions->at(i);
            }
            out_keys.push_back(std::move(key));
        }
        if (out_keys.empty()) {
            return;
        }
        json::Value out;
        out["target"] = node_path;
        out["path"] = field_path;
        std::int64_t interpolation = 1; // Godot's default, linear
        if (const Value* value = track.get("interp");
            value != nullptr && value->kind == Value::Kind::Int) {
            interpolation = value->integer;
        }
        if (type == "bezier") {
            interpolation = 2;
            problems_.add("animation bezier track", where,
                          "its curve plays as cubic interpolation through its keys");
        }
        static constexpr std::array<std::string_view, 5> kInterpolations{
            "nearest", "linear", "cubic", "linear_angle", "cubic_angle"};
        if (interpolation >= 0 && interpolation < 5 && interpolation != 1) {
            out["interpolation"] = kInterpolations.at(static_cast<std::size_t>(interpolation));
        }
        std::int64_t update = 0; // continuous
        if (const Value* value = keys->get("update");
            value != nullptr && value->kind == Value::Kind::Int) {
            update = value->integer;
        }
        if (update == 2) {
            problems_.add("animation track capture mode", where,
                          "plays as continuous, from its first key rather than the value before");
        }
        if (update == 1 && !resolved.value().discrete) {
            out["update"] = "discrete";
        } else if (update != 1 && resolved.value().discrete && numeric) {
            out["update"] = "continuous";
        }
        if (const Value* wrap = track.get("loop_wrap");
            wrap != nullptr && wrap->kind == Value::Kind::Bool && !wrap->boolean) {
            out["loop_wrap"] = false;
        }
        out["keys"] = std::move(out_keys);
        tracks.push_back(std::move(out));
        targets.insert(node_path);
    }

    // What a Godot property of a node with these components becomes: a component's field and
    // its default.
    struct FieldOf {
        std::string component;
        std::string field;
        json::Value fallback;
    };

    [[nodiscard]] std::optional<FieldOf> property_field(const std::vector<std::string>& order,
                                                        std::string_view property) const {
        const auto find =
            [&](std::string_view field,
                std::initializer_list<std::string_view> only) -> std::optional<FieldOf> {
            for (const std::string& component : order) {
                if (only.size() != 0 && std::ranges::find(only, component) == only.end()) {
                    continue;
                }
                const world::ComponentType* type = components_.find(component);
                const json::Value* fallback =
                    type != nullptr && field != "children" ? type->defaults().find(field) : nullptr;
                if (fallback != nullptr) {
                    return FieldOf{component, std::string(field), *fallback};
                }
            }
            return std::nullopt;
        };
        if (auto found = find(property, {}); found) {
            return found;
        }
        // Sprite2D and TileMapLayer have one tint; a sprite's region is its region_rect.
        if (property == "self_modulate") {
            return find("modulate", {"Sprite2D", "TileMapLayer"});
        }
        if (property == "region_rect") {
            return find("region", {"Sprite2D"});
        }
        return std::nullopt;
    }

    // The components a node becomes, in the order its properties look for a field: Transform2D
    // first, so a node's position goes there rather than to a field of the same name, such as
    // CollisionShape2D's.
    static std::vector<std::string> component_order(const Node& node, Family family,
                                                    bool transform) {
        std::vector<std::string> order;
        if (transform) {
            order.emplace_back("Transform2D");
        }
        // A ParallaxBackground in the world is a Node2D, as Godot's converter makes it.
        const bool background_in_world =
            node.type == "ParallaxBackground" && family == Family::Node2D;
        if (const NodeType* type = find_node_type(node.type);
            type != nullptr && !background_in_world) {
            for (const std::string_view component : {type->first, type->second}) {
                if (!component.empty()) {
                    order.emplace_back(component);
                }
            }
        }
        if (family == Family::Control) {
            order.emplace_back("Control");
        }
        return order;
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
        // A canvas layer holds its children in place as the root does.
        std::vector<Family> families(nodes.size(), Family::Plain);
        std::vector<bool> has_transform(nodes.size(), false);
        std::vector<bool> layers(nodes.size(), false);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeType* type = find_node_type(nodes[i].type);
            families[i] = type != nullptr ? type->family : guess_family(nodes[i]);
            if (type == nullptr) {
                problems_.add("node type " + nodes[i].type, nodes[i].declared,
                              unsupported_node(nodes[i].type));
            }
            // A ParallaxBackground that follows the camera's zoom is drawn with the world, as
            // Godot's converter turns it into a Node2D.
            if (nodes[i].type == "ParallaxBackground" &&
                !node_bool(nodes[i], "scroll_ignore_camera_zoom")) {
                families[i] = Family::Node2D;
            }
            layers[i] =
                families[i] == Family::Plain && type != nullptr && type->first == "CanvasLayer";
            // An instance's root is placed as the root of the scene it instances is.
            const int parent = nodes[i].instanced != nullptr ? -1 : nodes[i].parent;
            has_transform[i] =
                families[i] == Family::Node2D ||
                (families[i] == Family::Plain && !layers[i] &&
                 nodes[i].type != "AudioStreamPlayer" && nodes[i].type != "PopupMenu" &&
                 (parent < 0 || (families[static_cast<std::size_t>(parent)] == Family::Plain &&
                                 (has_transform[static_cast<std::size_t>(parent)] ||
                                  layers[static_cast<std::size_t>(parent)]))));
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
            const std::vector<std::string> order =
                component_order(node, families[i], has_transform[i]);
            for (const std::string& component : order) {
                components[component] = json::Object{};
            }
            const NodeType* type = find_node_type(node.type);
            if (type != nullptr && type->vertical) {
                components[std::string(type->first)]["vertical"] = true;
            }
            if (components.contains("AnimatedSprite2D")) {
                // Godot's plays only with autoplay or when code calls play().
                components["AnimatedSprite2D"]["playing"] = false;
            }
            if (node.type == "ParallaxBackground" && components.contains("CanvasLayer")) {
                components["CanvasLayer"]["layer"] = kParallaxBackgroundLayer;
            }
            if (node.type == "ParallaxLayer") {
                convert_parallax_layer(nodes, node, components["Parallax2D"]);
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
            // Formatting leaves out of an instance what the scene it instances gives it.
            if (const Skeleton* skeleton = skeletons().find(script);
                skeleton != nullptr && skeleton->script.node) {
                components[skeleton->script.component] =
                    script_component(skeleton->script, values, nodes, node);
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
        // Control. Anything else stays where Godot would draw it, at the top of its canvas.
        std::vector<bool> linked(nodes.size(), false);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (const int child : nodes[i].children) {
                const auto c = static_cast<std::size_t>(child);
                json::Value& parent = entities[nodes[i].id]["components"];
                if (has_transform[i] && has_transform[c]) {
                    parent["Transform2D"]["children"].push_back(nodes[c].id);
                    linked[c] = true;
                } else if (families[i] == Family::Control && families[c] == Family::Control) {
                    parent["Control"]["children"].push_back(nodes[c].id);
                    linked[c] = true;
                } else if (families[i] == Family::Control && families[c] == Family::Node2D) {
                    problems_.add("Node2D under a Control", nodes[c].declared,
                                  "placed in its canvas instead of in the control's rect");
                } else if (families[i] == Family::Node2D && families[c] == Family::Control) {
                    problems_.add("Control under a Node2D", nodes[c].declared,
                                  "drawn on the screen over its canvas instead of in it");
                }
            }
        }
        // What draws at the top of a canvas layer's canvas is in its children, in Godot's order.
        for (std::size_t c = 0; c < nodes.size(); ++c) {
            const bool draws = has_transform[c] || families[c] == Family::Control;
            if (linked[c] || layers[c] || !draws) {
                continue;
            }
            for (int up = nodes[c].parent; up >= 0;
                 up = nodes[static_cast<std::size_t>(up)].parent) {
                if (layers[static_cast<std::size_t>(up)]) {
                    entities[nodes[static_cast<std::size_t>(up)].id]["components"]["CanvasLayer"]
                            ["children"]
                                .push_back(nodes[c].id);
                    break;
                }
            }
        }

        // AnimationPlayers, whose tracks name nodes from their root node.
        const auto order_of = [&](const Node& target) {
            const auto t = static_cast<std::size_t>(&target - nodes.data());
            return component_order(target, families[t], has_transform[t]);
        };
        for (const Node& node : nodes) {
            if (node.type == "AnimationPlayer") {
                convert_player(nodes, node, order_of, scene,
                               entities[node.id]["components"]["AnimationPlayer"]);
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
                errors = animation::check_players(converted, reader);
            }
            if (errors.empty()) {
                errors = canvas::check_sub_emitters(components_, converted, reader);
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

    // --- Scripts as C++ ---

    // The project path a res:// or uid:// path in a script's preload() names, as the import
    // writes it: a scene's .scene.json file for a .tscn file.
    [[nodiscard]] std::string script_resource(std::string_view reference) const {
        std::string path = strip_res(reference);
        if (const auto uid = uids_.find(path); uid != uids_.end()) {
            path = uid->second;
        }
        if (extension_of(path) == ".tscn") {
            return destination(without_extension(path) + std::string(world::kSceneSuffix));
        }
        return destination(path);
    }

    // The scripts' skeletons, made once the project's files are listed.
    Skeletons& skeletons() {
        SLASH_ASSERT(skeletons_.has_value(), "the skeletons are made before they are used");
        return *skeletons_;
    }

    // Reads each script into the C++ skeleton the import writes for it, so scenes can give
    // entities its component.
    void read_scripts(const Existing& existing) {
        skeletons_.emplace(existing);
        GdscriptOptions options;
        options.resource_path = [this](std::string_view path) { return script_resource(path); };
        for (const auto& [script, record] : scripts_) {
            const std::string extension = extension_of(script);
            if (extension != ".gd" && extension != ".cs") {
                continue;
            }
            auto text = read(script);
            if (!text) {
                continue;
            }
            skeletons().add(
                script,
                extension == ".gd"
                    ? read_gdscript(destination(script), text.value(), options)
                    : read_csharp(destination(script), text.value(), ScriptSource::GodotCsharp),
                components_);
        }
    }

    // A script's component on a node's entity: the values the node gave its exported
    // variables, node paths as the IDs of the entities they name.
    static json::Value script_component(const Script& script, const json::Value& values,
                                        const std::vector<Node>& nodes, const Node& node) {
        json::Value component = json::Object{};
        for (const ScriptField& field : script.fields) {
            const json::Value* value = values.find(field.source_name);
            if (!field.exported || value == nullptr) {
                continue;
            }
            const auto entity = [&](const json::Value& path) -> json::Value {
                if (!path.is_string()) {
                    return nullptr;
                }
                const std::string id = resolve_node_path(nodes, node, path.as_string());
                return id.empty() ? json::Value(nullptr) : json::Value(id);
            };
            switch (field.kind) {
            case FieldKind::Value:
                component[field.name] = *value;
                break;
            case FieldKind::Entity:
                component[field.name] = entity(*value);
                break;
            case FieldKind::Entities: {
                json::Array ids;
                if (value->is_array()) {
                    for (const json::Value& path : value->as_array()) {
                        ids.push_back(entity(path));
                    }
                }
                component[field.name] = std::move(ids);
                break;
            }
            }
        }
        return component;
    }

    // Writes each script's skeleton but those the destination has, the file registering them
    // all and, when the game's code has none, a game module calling it.
    void write_skeletons() {
        for (auto& [script, entry] : skeletons().all()) {
            Script& code = entry.script;
            code.task = task_of_[script];
            for (const auto& [name, path] : autoloads_) {
                if (path == script) {
                    code.autoload = name;
                }
            }
            for (const json::Value& connection : scripts_[script].connections) {
                const auto text = [&](std::string_view key) {
                    const json::Value* value = connection.find(key);
                    return value != nullptr && value->is_string() ? value->as_string()
                                                                  : std::string();
                };
                code.connections.push_back({.scene = text("scene"),
                                            .signal = text("signal"),
                                            .from = text("from"),
                                            .to = text("to"),
                                            .method = text("method"),
                                            .argument = {}});
            }
        }
        module_ = skeletons().write(
            [this](std::string path, std::string text) {
                add_file(std::move(path), std::move(text));
            },
            problems_);
    }

    // --- Tasks and the report ---

    void write_tasks(const std::optional<std::string>& existing) {
        std::vector<PortTask> tasks;
        for (const auto& [script, record] : scripts_) {
            const bool csharp = extension_of(script) == ".cs";
            PortTask task;
            task.link = destination(script);
            task.name = "Port " + script + " to C++";
            task.summary = std::string(csharp ? "C#" : "GDScript") +
                           " from Godot: write what it does as C++ components and systems.";
            task.engine = "godot";
            std::string text;
            if (auto source = read(script); source && !csharp) {
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
            for (const ScriptUse& use : record.uses) {
                uses.push_back(use.entity + " in " + use.scene);
                task.links.push_back(use.scene);
            }
            if (!uses.empty()) {
                text += " Used by " + join(uses, ", ") + ".";
            }
            for (const auto& [name, path] : autoloads_) {
                if (path == script) {
                    text += " An autoload named " + name + ".";
                }
            }
            if (const Skeleton* skeleton = skeletons().find(script); skeleton != nullptr) {
                const Script& code = skeleton->script;
                text += " Its C++ skeleton is " + skeleton->code;
                text += code.node ? ": the " + code.component + " component and the " +
                                        code.system + " system."
                                  : ": the " + code.component + " struct.";
                task.links.push_back(skeleton->code);
            }
            text += " The values each entity's node set, its signal connections and node paths "
                    "are in " +
                    std::string(kReportFile) + ".";
            task.text = trim(text);
            tasks.push_back(std::move(task));
        }
        PortTasks written = write_port_tasks(existing, tasks);
        std::size_t index = 0;
        for (const auto& [script, record] : scripts_) {
            task_of_[script] = written.ids[index++];
        }
        if (!written.file.empty()) {
            add_file(storyboard::file_path("work"), std::move(written.file));
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
            json::Value brief;
            brief["script"] = destination(script);
            brief["task"] = task_of_[script];
            brief["entities"] = static_cast<std::int64_t>(record.uses.size());
            if (const Skeleton* skeleton = skeletons().find(script); skeleton != nullptr) {
                for (json::Value* entry : {&item, &brief}) {
                    (*entry)["code"] = skeleton->code;
                    (*entry)["component"] = skeleton->script.component;
                    if (skeleton->kept) {
                        (*entry)["kept"] = true;
                    }
                }
                if (skeleton->script.node) {
                    item["system"] = skeleton->script.system;
                }
            }
            scripts[destination(script)] = item;
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
        out["converted"]["animation_libraries"] = animation_files_;
        out["converted"]["process_materials"] = process_files_;
        out["converted"]["audio_streams"] = stream_files_;
        out["converted"]["input_actions"] = input_actions_;
        out["converted"]["assets"] = assets_;
        out["scripts"] = std::move(summary);
        if (!module_.empty()) {
            out["module"] = module_;
        }
        out["unsupported"] = std::move(unsupported);
        out["report"] = kReportFile;
    }

    std::string source_;
    const GodotFiles* unpacked_ = nullptr;
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
    std::map<std::string, std::string> process_written_;
    std::map<std::string, Section> sound_settings_;      // .import [params], by sound
    std::map<std::string, std::string> sounds_written_;  // by sound
    std::map<std::string, std::string> streams_written_; // by file and resource
    std::map<std::string, TileSetFile> tile_sets_;
    std::map<std::string, std::string> themes_written_;
    std::set<std::string> bus_layouts_written_;
    std::map<std::string, LibraryFile> libraries_written_; // by file name, without its suffix
    std::map<std::string, ScriptRecord> scripts_;          // by res path
    std::optional<Skeletons> skeletons_;                   // by res path
    std::string module_;                                   // the game module written, or ""
    std::map<std::string, std::string> task_of_;
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
    int animation_files_ = 0;
    int process_files_ = 0;
    int stream_files_ = 0;
    int input_actions_ = 0;
    int assets_ = 0;
};

} // namespace

Result<ImportPlan> plan_godot(const std::string& source, const GodotFiles* unpacked,
                              const std::string& prefix, bool copy_assets,
                              const Existing& existing) {
    GodotImporter importer(source, unpacked, prefix, copy_assets);
    return importer.run(existing);
}

} // namespace slash::importer
