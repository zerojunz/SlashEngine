#include "gltf.hpp"

#include "slash/animation/animation.hpp"
#include "slash/animation/tree.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/scripts.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/project.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace godot = slash::importer::godot;
namespace importer = slash::importer;
namespace fs = std::filesystem;

namespace {

godot::Value value_of(std::string_view text) {
    auto parsed = godot::parse_value(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE(text.ok());
    return std::move(text).value();
}

void write_png(const fs::path& path, std::uint32_t width, std::uint32_t height) {
    slash::render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.assign(std::size_t{width} * height * 4, 255);
    write(path, slash::render::encode_png(image).value());
}

// The files a plan writes, by path.
std::map<std::string, std::string> files_of(const importer::ImportPlan& plan) {
    std::map<std::string, std::string> files;
    for (const auto& [path, contents] : plan.files) {
        files[path] = contents;
    }
    return files;
}

// Reads the files a plan writes, so its scenes can instance the scenes it writes.
slash::world::FileReader reader_of(const std::map<std::string, std::string>& files) {
    return [&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(std::string(path));
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
}

// The engine's components, and stand-ins for those of the C++ skeletons a plan writes, as the
// tools have them before the game's code is built.
void add_types(slash::world::ComponentRegistry& components,
               const std::map<std::string, std::string>& files) {
    slash::world::register_builtin_components(components);
    for (const auto& [path, contents] : files) {
        if (!path.ends_with(".cpp")) {
            continue;
        }
        for (slash::runtime::DeclaredComponent& declared : slash::runtime::declared_in(contents)) {
            if (components.find(declared.name) == nullptr) {
                components.add_stand_in(std::move(declared.name), std::move(declared.fields));
            }
        }
    }
}

// The components one entity of a planned scene ends up with, with what it extends and its
// overrides applied.
json::Value entity(const std::map<std::string, std::string>& files, const std::string& scene,
                   std::string_view id) {
    REQUIRE(files.contains(scene));
    slash::world::ComponentRegistry components;
    add_types(components, files);
    slash::world::World world(components);
    world.set_file_reader(reader_of(files));
    REQUIRE(slash::world::load_scene(world, parse(files.at(scene))));
    const entt::entity found = world.find(id);
    REQUIRE_MESSAGE(world.valid(found), id);
    json::Value result = json::Object{};
    for (const auto& type : components.types()) {
        if (type->has(world, found)) {
            result[type->name()] = type->save(world, found);
        }
    }
    return result;
}

// One entity's entry in a planned scene's file.
json::Value entry(const std::map<std::string, std::string>& files, const std::string& scene,
                  std::string_view id) {
    REQUIRE(files.contains(scene));
    const json::Value document = parse(files.at(scene));
    const json::Value* found = document.find("entities")->find(id);
    REQUIRE_MESSAGE(found != nullptr, id);
    return *found;
}

// The report's unsupported entry for what, or null.
json::Value unsupported(const json::Value& report, std::string_view what) {
    for (const json::Value& item : report.find("unsupported")->as_array()) {
        if (item.find("what")->as_string() == what) {
            return item;
        }
    }
    return nullptr;
}

// A converted tile map layer's cells, as "source:column,row" and any suffix by "x,y".
std::map<std::string, std::string> cells_of(const std::map<std::string, std::string>& files,
                                            const json::Value& component) {
    const std::string& path = component.find("tile_set")->as_string();
    REQUIRE(files.contains(path));
    auto set = slash::canvas::parse_tileset(parse(files.at(path)));
    REQUIRE(set.ok());
    slash::world::TileMapLayer layer;
    if (const json::Value* origin = component.find("origin"); origin != nullptr) {
        layer.origin = {static_cast<int>(origin->as_array().at(0).as_int()),
                        static_cast<int>(origin->as_array().at(1).as_int())};
    }
    for (const auto& [key, tile] : component.find("legend")->as_object()) {
        layer.legend[key] = tile.as_string();
    }
    for (const json::Value& row : component.find("rows")->as_array()) {
        layer.rows.push_back(row.as_string());
    }
    auto cells = slash::canvas::decode_cells(layer, set.value());
    REQUIRE(cells.ok());
    std::map<std::string, std::string> out;
    for (const auto& [cell, tile] : cells.value()) {
        // As tile_text writes an unnamed tile, with its alternative and turns.
        out[std::to_string(cell.x) + "," + std::to_string(cell.y)] =
            tile.source + ":" + std::to_string(tile.atlas.x) + "," + std::to_string(tile.atlas.y) +
            slash::canvas::tile_text(set.value(), tile)
                .substr(slash::canvas::tile_text(set.value(),
                                                 {.source = tile.source, .atlas = tile.atlas})
                            .size());
    }
    return out;
}

constexpr std::string_view kProject = R"(; Engine configuration file.

config_version=5

[application]

config/name="Test Game"
run/main_scene="res://scenes/level.tscn"
run/max_fps=120
config/features=PackedStringArray("4.4", "Forward Plus")

[autoload]

Music="*res://autoload/music.gd"

[audio]

driver/mix_rate=48000
driver/enable_input=true
general/2d_panning_strength=0.8

[display]

display_server/driver.linuxbsd="wayland"
window/size/viewport_width=640
window/size/viewport_height=360
window/vsync/vsync_mode=2
window/stretch/mode="viewport"

[gui]

theme/custom="res://ui/theme.tres"

[input]

shoot={
"deadzone": 0.5,
"events": [Object(InputEventMouseButton,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":true,"ctrl_pressed":false,"meta_pressed":false,"button_mask":0,"position":Vector2(0, 0),"global_position":Vector2(0, 0),"factor":1.0,"button_index":1,"canceled":false,"pressed":false,"double_click":false,"script":null)
, Object(InputEventJoypadMotion,"resource_local_to_scene":false,"resource_name":"","device":0,"axis":5,"axis_value":1.0,"script":null)
, Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":4194309,"physical_keycode":0,"key_label":0,"unicode":0,"location":0,"echo":false,"script":null)
, Object(InputEventScreenTouch,"resource_local_to_scene":false,"resource_name":"","device":0,"window_id":0,"index":0,"position":Vector2(0, 0),"canceled":false,"pressed":false,"double_tap":false,"script":null)
]
}

[physics]

common/physics_ticks_per_second=30
2d/default_gravity=490.0
2d/default_gravity_vector=Vector2(0, 1)
2d/default_linear_damp=0.5

[rendering]

textures/canvas_textures/default_texture_filter=3
environment/defaults/default_clear_color=Color(0, 0, 0, 1)
renderer/rendering_method="gl_compatibility"
rendering_device/fallback_to_opengl3=false
)";

constexpr std::string_view kEnemy = R"([gd_scene load_steps=4 format=3 uid="uid://enemy0scene"]

[ext_resource type="Script" path="res://enemy.gd" id="1_script"]
[ext_resource type="Texture2D" uid="uid://enemy0texture" path="res://art/enemy.png" id="2_tex"]

[sub_resource type="CircleShape2D" id="CircleShape2D_1"]
radius = 6.0

[node name="Enemy" type="RigidBody2D"]
mass = 2.0
script = ExtResource("1_script")
health = 3
loot = ["coin", "gem"]

[node name="Sprite" type="Sprite2D" parent="."]
texture = ExtResource("2_tex")
region_enabled = true
region_rect = Rect2(0, 0, 16, 16)
modulate = Color(1, 0.5, 0.5, 1)
self_modulate = Color(1, 1, 1, 0.5)
texture_filter = 1

[node name="Shape" type="CollisionShape2D" parent="."]
shape = SubResource("CircleShape2D_1")
)";

// The level instances enemy.tscn by a stale path; the UID finds it.
constexpr std::string_view kLevel = R"([gd_scene load_steps=12 format=3 uid="uid://level0scene"]

[ext_resource type="PackedScene" uid="uid://enemy0scene" path="res://scenes/old_enemy_path.tscn" id="1_enemy"]
[ext_resource type="Script" path="res://level.gd" id="2_script"]
[ext_resource type="Texture2D" path="res://art/sheet.png" id="3_sheet"]
[ext_resource type="TileSet" path="res://tiles/terrain.tres" id="4_tiles"]
[ext_resource type="Theme" path="res://ui/theme.tres" id="5_theme"]

[sub_resource type="PhysicsMaterial" id="PhysicsMaterial_1"]
friction = 0.2
bounce = 0.6

[sub_resource type="SegmentShape2D" id="SegmentShape2D_1"]
a = Vector2(-10, 0)
b = Vector2(10, 0)

[sub_resource type="ConcavePolygonShape2D" id="ConcavePolygonShape2D_1"]
segments = PackedVector2Array(0, 0, 10, 0)

[sub_resource type="OccluderPolygon2D" id="OccluderPolygon2D_1"]
closed = false
cull_mode = 1
polygon = PackedVector2Array(0, 0, 16, 0, 16, 16)

[sub_resource type="Curve" id="Curve_1"]
_data = [Vector2(0, 1), 0.0, 0.0, 0, 0, Vector2(1, 0), 0.0, 0.0, 0, 0]
point_count = 2

[sub_resource type="Gradient" id="Gradient_1"]
offsets = PackedFloat32Array(0, 1)
colors = PackedColorArray(1, 1, 0, 1, 1, 0, 0, 0)

[sub_resource type="StyleBoxFlat" id="StyleBoxFlat_1"]
bg_color = Color(0.1, 0.1, 0.2, 0.9)
corner_radius_top_left = 6
border_width_bottom = 2

[sub_resource type="LabelSettings" id="LabelSettings_1"]
font_size = 20
font_color = Color(1, 1, 0, 1)
shadow_offset = Vector2(2, 3)

[node name="Level" type="Node"]
script = ExtResource("2_script")

[node name="World" type="Node2D" parent="."]

[node name="Floor" type="StaticBody2D" parent="World"]
physics_material_override = SubResource("PhysicsMaterial_1")

[node name="Edge" type="CollisionShape2D" parent="World/Floor"]
position = Vector2(4, 5)
rotation = 0.5
shape = SubResource("SegmentShape2D_1")

[node name="Ramp" type="CollisionShape2D" parent="World/Floor"]
shape = SubResource("ConcavePolygonShape2D_1")

[node name="Enemy" parent="World" instance=ExtResource("1_enemy")]
position = Vector2(100, 50)
health = 5

[node name="Sprite" parent="World/Enemy" index="0"]
flip_h = true

[node name="Enemy2" parent="World" instance=ExtResource("1_enemy")]
position = Vector2(200, 50)

[node name="Pin" type="PinJoint2D" parent="World"]
node_a = NodePath("../Floor")
node_b = NodePath("../Enemy")

[node name="Crate" type="LightOccluder2D" parent="World"]
occluder = SubResource("OccluderPolygon2D_1")

[node name="Torch" type="PointLight2D" parent="World"]
energy = 1.5
texture = ExtResource("3_sheet")
blend_mode = 2

[node name="Sparks" type="CPUParticles2D" parent="World"]
amount = 16
emission_shape = 6
scale_amount_curve = SubResource("Curve_1")
color_ramp = SubResource("Gradient_1")

[node name="Tiles" type="TileMapLayer" parent="World"]
tile_set = ExtResource("4_tiles")
tile_map_data = PackedByteArray("AAAAAAAAAAAAAAAAAAABAAAAAAABAAAAAAD//wIAAAABAAAAAQADAAAAAAAAAAEAAFA=")
self_modulate = Color(1, 1, 1, 0.5)

[node name="Old" type="TileMap" parent="World"]
tile_set = ExtResource("4_tiles")
format = 2
layer_0/name = "Ground"
layer_0/tile_data = PackedInt32Array(0, 0, 0, 65538, 131072, 0)
layer_1/name = "Top"
layer_1/modulate = Color(1, 1, 1, 0.5)
layer_1/z_index = 2
layer_1/y_sort_enabled = true
layer_1/navigation_enabled = false
layer_1/tile_data = PackedInt32Array(-65536, 65536, 0)

[node name="Music" type="AudioStreamPlayer" parent="."]
volume_db = -6.0
bus = &"Music"

[node name="UI" type="CanvasLayer" parent="."]

[node name="Menu" type="VBoxContainer" parent="UI"]
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
alignment = 1
theme = ExtResource("5_theme")
theme_override_constants/separation = 8

[node name="Title" type="Label" parent="UI/Menu"]
layout_mode = 2
size_flags_horizontal = 3
size_flags_vertical = 4
text = "Title"
label_settings = SubResource("LabelSettings_1")
horizontal_alignment = 1
mouse_filter = 2

[node name="Play" type="Button" parent="UI/Menu"]
layout_mode = 2
tooltip_text = "Start a new game"
focus_mode = 2
theme_override_styles/normal = SubResource("StyleBoxFlat_1")
text = "Play"
button_mask = 3
alignment = 3

[node name="Volume" type="VSlider" parent="UI/Menu"]
layout_mode = 2
max_value = 10.0

[node name="Marker" type="Marker2D" parent="UI/Menu"]

[node name="Items" type="ItemList" parent="UI/Menu"]
layout_mode = 2
max_columns = 2

[node name="Size" type="OptionButton" parent="UI"]
offset_right = 90.0
offset_bottom = 31.0
text = "Medium"
selected = 1
fit_to_longest_item = false
item_count = 3
popup/item_0/text = "Small"
popup/item_0/id = 0
popup/item_1/text = "Medium"
popup/item_1/id = 5
popup/item_2/separator = true
popup/item_2/id = 2

[node name="File" type="MenuButton" parent="UI"]
offset_left = 100.0
offset_right = 140.0
offset_bottom = 31.0
text = "File"
switch_on_hover = true
item_count = 2
popup/item_0/text = "New"
popup/item_0/icon = ExtResource("3_sheet")
popup/item_0/id = 0
popup/item_1/text = "Grid"
popup/item_1/checkable = 1
popup/item_1/checked = true
popup/item_1/id = 1

[node name="Context" type="PopupMenu" parent="UI"]
position = Vector2i(10, 20)
size = Vector2i(100, 80)
hide_on_checkable_item_selection = false
item_count = 2
item_0/text = "Copy"
item_0/id = 7
item_1/text = "Mode"
item_1/checkable = 2
item_1/disabled = true
item_1/id = 1
item_1/shortcut = null

[connection signal="pressed" from="UI/Menu/Play" to="." method="_on_play_pressed"]
)";

// A tile set with two terrains connected by their sides, an alternative tile, collision, a tile
// two cells wide and an animated tile.
constexpr std::string_view kTileSet = R"([gd_resource type="TileSet" load_steps=4 format=3]

[ext_resource type="Texture2D" path="res://art/tiles.png" id="1_tex"]

[sub_resource type="PhysicsMaterial" id="PhysicsMaterial_1"]
friction = 0.25
bounce = 0.5

[sub_resource type="TileSetAtlasSource" id="TileSetAtlasSource_1"]
texture = ExtResource("1_tex")
0:0/0 = 0
0:0/0/terrain_set = 0
0:0/0/terrain = 0
0:0/0/terrains_peering_bit/right_side = 0
0:0/0/terrains_peering_bit/bottom_side = 1
1:0/0 = 0
1:0/0/terrain_set = 0
1:0/0/terrain = 1
1:0/0/terrains_peering_bit/right_side = 1
1:0/0/terrains_peering_bit/bottom_side = 1
1:0/0/terrains_peering_bit/left_side = 1
1:0/0/terrains_peering_bit/top_side = 1
1:0/1 = 1
1:0/1/flip_v = true
1:0/1/modulate = Color(1, 0.5, 0.5, 1)
1:0/1/texture_offset = Vector2i(1, 2)
1:0/1/z_index = 1
1:0/1/physics_layer_0/angular_velocity = 0.0
2:0/0 = 0
2:0/0/probability = 0.5
2:0/0/texture_origin = Vector2i(0, 4)
3:0/size_in_atlas = Vector2i(2, 1)
3:0/0 = 0
0:2/animation_separation = Vector2i(1, 0)
0:2/animation_speed = 2.0
0:2/animation_mode = 1
0:2/animation_frame_0/duration = 0.5
0:2/animation_frame_1/duration = 0.25
0:2/0 = 0
0:1/0 = 0
0:1/0/physics_layer_0/polygon_0/points = PackedVector2Array(-8, -8, 8, -8, 8, 8, -8, 8)
0:1/0/physics_layer_1/polygon_0/points = PackedVector2Array(-8, -8, 8, -8, 8, -4, -8, -4)
0:1/0/physics_layer_1/polygon_0/one_way = true
0:1/0/physics_layer_0/linear_velocity = Vector2(0, 0)
0:1/0/physics_layer_1/angular_velocity = 1.0

[resource]
terrain_set_0/mode = 2
terrain_set_0/terrain_0/name = "Grass"
terrain_set_0/terrain_0/color = Color(0.2, 0.8, 0.2, 1)
terrain_set_0/terrain_1/name = "Water"
terrain_set_0/terrain_1/color = Color(0.2, 0.2, 0.8, 1)
physics_layer_0/collision_layer = 1
physics_layer_1/collision_layer = 4
physics_layer_1/collision_mask = 3
physics_layer_1/physics_material = SubResource("PhysicsMaterial_1")
sources/0 = SubResource("TileSetAtlasSource_1")
)";

constexpr std::string_view kTheme = R"([gd_resource type="Theme" load_steps=3 format=3]

[sub_resource type="StyleBoxFlat" id="StyleBoxFlat_b"]
bg_color = Color(0.2, 0.3, 0.4, 1)
corner_radius_top_left = 4

[sub_resource type="StyleBoxLine" id="StyleBoxLine_1"]
thickness = 2

[resource]
default_font_size = 18
Button/colors/font_color = Color(1, 0.9, 0.5, 1)
Button/constants/h_separation = 6
Button/styles/normal = SubResource("StyleBoxFlat_b")
Panel/styles/panel = SubResource("StyleBoxLine_1")
Title/base_type = &"Label"
Title/font_sizes/font_size = 24
)";

// Godot uses res://default_bus_layout.tres with no setting naming it.
constexpr std::string_view kBusLayout = R"([gd_resource type="AudioBusLayout" load_steps=6 format=3]

[sub_resource type="AudioEffectReverb" id="AudioEffectReverb_1"]
resource_name = "Reverb"
room_size = 0.6
wet = 0.4

[sub_resource type="AudioEffectLowPassFilter" id="AudioEffectLowPassFilter_1"]
resource_name = "LowPassFilter"
cutoff_hz = 1200.0
db = 1

[sub_resource type="AudioEffectChorus" id="AudioEffectChorus_1"]
resource_name = "Chorus"
voice_count = 3
voice/1/delay_ms = 10.0
voice/3/pan = 0.25

[sub_resource type="AudioEffectEQ6" id="AudioEffectEQ6_1"]
resource_name = "EQ6"
band_db/100_hz = -3.0

[sub_resource type="AudioEffectRecord" id="AudioEffectRecord_1"]
resource_name = "Record"
format = 0

[sub_resource type="AudioEffectPitchShift" id="AudioEffectPitchShift_1"]
resource_name = "PitchShift"
pitch_scale = 1.5
oversampling = 8
fft_size = 2

[sub_resource type="AudioEffectLimiter" id="AudioEffectLimiter_1"]
resource_name = "Limiter"

[resource]
bus/0/name = &"Master"
bus/0/solo = false
bus/0/mute = false
bus/0/bypass_fx = false
bus/0/volume_db = -1.0
bus/0/send = &""
bus/1/name = &"Music"
bus/1/solo = false
bus/1/mute = false
bus/1/bypass_fx = false
bus/1/volume_db = -3.0
bus/1/send = &"Master"
bus/1/effect/0/effect = SubResource("AudioEffectReverb_1")
bus/1/effect/0/enabled = true
bus/1/effect/1/effect = SubResource("AudioEffectLowPassFilter_1")
bus/1/effect/1/enabled = false
bus/2/name = &"SFX"
bus/2/solo = false
bus/2/mute = true
bus/2/bypass_fx = false
bus/2/volume_db = 0.0
bus/2/send = &"Music"
bus/2/effect/0/effect = SubResource("AudioEffectChorus_1")
bus/2/effect/0/enabled = true
bus/2/effect/1/effect = SubResource("AudioEffectEQ6_1")
bus/2/effect/1/enabled = true
bus/2/effect/2/effect = SubResource("AudioEffectRecord_1")
bus/2/effect/2/enabled = true
bus/2/effect/3/effect = SubResource("AudioEffectPitchShift_1")
bus/2/effect/3/enabled = true
bus/2/effect/4/effect = SubResource("AudioEffectLimiter_1")
bus/2/effect/4/enabled = true
)";

// A Godot project in a folder of the build tree, with its textures.
fs::path make_project(const fs::path& folder) {
    fs::remove_all(folder);
    write(folder / "project.godot", kProject);
    write(folder / "scenes/enemy.tscn", kEnemy);
    write(folder / "scenes/enemy.tscn.uid", "uid://enemy0scene\n");
    write(folder / "scenes/level.tscn", kLevel);
    write(folder / "enemy.gd",
          "extends RigidBody2D\n\n@export var health := 3\n@export var loot: Array[String] = []\n"
          "\nfunc hit(amount: int) -> void:\n\thealth -= amount\n");
    write(folder / "level.gd", "extends Node\n\nfunc _on_play_pressed() -> void:\n\tpass\n");
    write(folder / "autoload/music.gd", "extends Node\n");
    write(folder / "ui/theme.tres", kTheme);
    write(folder / "tiles/terrain.tres", kTileSet);
    write(folder / "default_bus_layout.tres", kBusLayout);
    write(folder / "shaders/glow.gdshader", "shader_type canvas_item;\n");
    write(folder / ".godot/imported/enemy.png-1234.ctex", "binary");
    write(folder / "addons/plugin/plugin.gd", "extends EditorPlugin\n");
    write_png(folder / "art/enemy.png", 16, 16);
    write_png(folder / "art/sheet.png", 32, 16);
    write_png(folder / "art/tiles.png", 64, 32);
    return folder;
}

// Plans importing project/godot into project.
importer::ImportPlan plan_inside(const fs::path& project) {
    auto plan = importer::plan_import(
        {.source = (project / "godot").string(), .destination = project.string()});
    REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
    return std::move(plan).value();
}

} // namespace

TEST_CASE("Godot values parse as Godot's VariantParser reads them") {
    CHECK(value_of("42").kind == godot::Value::Kind::Int);
    CHECK(value_of("-7").integer == -7);
    CHECK(value_of("1.5e2").real == 150.0);
    CHECK(value_of("1.0").kind == godot::Value::Kind::Float);
    CHECK(value_of("true").boolean);
    CHECK(value_of("null").kind == godot::Value::Kind::Nil);
    CHECK(value_of(R"("a\"b\né")").text == "a\"b\n\xC3\xA9");
    CHECK(value_of(R"("😀")").text == "\xF0\x9F\x98\x80"); // a surrogate pair
    const godot::Value name = value_of("&\"idle\"");
    CHECK(name.kind == godot::Value::Kind::StringName);
    CHECK(name.is_string());

    const godot::Value vector = value_of("Vector2(1, -2.5)");
    CHECK(vector.is_call("Vector2"));
    CHECK(vector.numbers().value() == std::vector<double>{1.0, -2.5});
    CHECK(value_of("#ff800080").numbers().value() ==
          std::vector<double>{1.0, 128.0 / 255.0, 0.0, 128.0 / 255.0});
    CHECK(value_of("ExtResource(\"1_abc\")").string_argument().value() == "1_abc");
    CHECK(value_of("NodePath(\"../Body:position\")").string_argument().value() ==
          "../Body:position");
    CHECK(value_of("PackedVector2Array(0, 0, 4, 5)").numbers().value().size() == 4);
    CHECK(value_of("Array[int]([1, 2, 3])").items.size() == 3);
    const godot::Value typed = value_of("Dictionary[String, int]({\"a\": 1})");
    REQUIRE(typed.get("a") != nullptr);
    CHECK(typed.get("a")->integer == 1);

    const godot::Value dictionary = value_of(R"({
"name": &"walk",
"frames": [{"duration": 1.0}, {"duration": 2.0}],
"nested": {"x": [1, [2, 3]]}
})");
    CHECK(dictionary.get("name")->text == "walk");
    CHECK(dictionary.get("frames")->items.at(1).get("duration")->number() == 2.0);
    CHECK(dictionary.get("nested")->get("x")->items.at(1).items.size() == 2);

    const godot::Value object =
        value_of(R"(Object(InputEventKey,"keycode":65,"shift_pressed":true,"script":null))");
    CHECK(object.is_call("Object"));
    CHECK(object.items.front().text == "InputEventKey");
    CHECK(object.get("keycode")->integer == 65);
    CHECK(object.get("shift_pressed")->boolean);

    CHECK_FALSE(godot::parse_value("Vector2(1, 2").ok());
    CHECK_FALSE(godot::parse_value("\"unterminated").ok());
}

TEST_CASE("Godot text resources and project settings parse into sections") {
    auto sections = godot::parse_resource(R"([gd_scene load_steps=2 format=3 uid="uid://x"]

; a comment
[ext_resource type="Texture2D" path="res://a.png" id="1_a"]

[node name="Root" type="Node2D" groups=["enemies", "solid"]]
position = Vector2(1, 2)
metadata/_edit_lock_ = true

[node name="Child" parent="." instance=ExtResource("1_a")]

[connection signal="pressed" from="Child" to="." method="_on_pressed" flags=3]
)");
    REQUIRE(sections.ok());
    const std::vector<godot::Section>& list = sections.value();
    REQUIRE(list.size() == 5);
    CHECK(list[0].tag == "gd_scene");
    CHECK(list[0].field_string("uid") == "uid://x");
    CHECK(list[1].field_string("path") == "res://a.png");
    CHECK(list[2].line == 6);
    CHECK(list[2].field("groups")->items.size() == 2);
    CHECK(list[2].property("position")->is_call("Vector2"));
    CHECK(list[2].property("metadata/_edit_lock_")->boolean);
    CHECK(list[3].field("instance")->is_call("ExtResource"));
    CHECK(list[4].field("flags")->integer == 3);

    auto broken = godot::parse_resource("[node name=\"A\"]\nposition = Vector2(1,\n\n[node]\n");
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message.find("line") != std::string::npos);

    auto settings = godot::parse_project_settings(R"(config_version=5

[application]

config/name="Game"
renderer/rendering_method.mobile="gl_compatibility"

[input]

jump={
"deadzone": 0.5,
"events": []
}
)");
    REQUIRE(settings.ok());
    REQUIRE(settings.value().size() == 3);
    CHECK(settings.value()[0].tag.empty());
    CHECK(settings.value()[0].property("config_version")->integer == 5);
    CHECK(settings.value()[1].property("config/name")->text == "Game");
    CHECK(settings.value()[1].property("renderer/rendering_method.mobile") != nullptr);
    CHECK(settings.value()[2].property("jump")->get("deadzone")->number() == 0.5);
}

TEST_CASE("importing a Godot project converts its settings and input map") {
    const fs::path project = "import_godot_settings";
    fs::remove_all(project);
    make_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);

    REQUIRE(files.contains("project.json"));
    const json::Value settings = parse(files.at("project.json"));
    CHECK(slash::runtime::validate_project(settings).empty());
    CHECK(settings.find("name")->as_string() == "Test Game");
    CHECK(settings.find("main_scene")->as_string() == "godot/scenes/level.scene.json");
    CHECK(settings.find("window")->find("width")->as_int() == 640);
    CHECK(settings.find("window")->find("height")->as_int() == 360);
    CHECK(settings.find("window")->find("vsync_mode")->as_string() == "adaptive");
    CHECK(settings.find("window")->find("linux_display_driver")->as_string() == "wayland");
    CHECK(settings.find("max_fps")->as_int() == 120);
    CHECK(settings.find("tick_hz")->as_double() == 30.0);
    CHECK(settings.find("rendering")->find("texture_filter")->as_string() == "linear");
    CHECK(settings.find("rendering")->find("driver")->as_string() == "opengl");
    CHECK_FALSE(settings.find("rendering")->find("fallback_to_opengl")->as_bool());
    CHECK(settings.find("physics_2d")->find("gravity")->as_double() == 490.0);
    CHECK(settings.find("physics_2d")->find("linear_damp")->as_double() == 0.5);

    const json::Value& shoot = *settings.find("input")->find("shoot");
    CHECK(shoot.find("deadzone")->as_double() == 0.5);
    CHECK(*shoot.find("events") ==
          parse(R"([{"type": "mouse_button", "button": "left", "shift": true},
                    {"type": "joypad_motion", "axis": "trigger_right", "axis_value": 1,
                     "device": 0},
                    {"type": "key", "key": "Enter"}])"));

    const json::Value& report = plan.report;
    CHECK(report.find("converted")->find("input_actions")->as_int() == 1);
    CHECK(unsupported(report, "input binding").find("why")->as_string() ==
          "InputEventScreenTouch bindings");
    CHECK(!unsupported(report, "setting display/window/stretch/mode").is_null());
    CHECK(settings.find("gui")->find("theme")->as_string() == "godot/ui/theme.theme.json");
    CHECK(*settings.find("audio") ==
          parse(R"({"bus_layout": "godot/default_bus_layout.bus_layout.json",
                     "mix_rate": 48000, "panning_strength_2d": 0.8, "enable_input": true})"));
    CHECK(unsupported(report, "file type .gdshader").find("where")->as_string() ==
          "shaders/glow.gdshader");
    // Godot's own folders and add-ons stay out.
    for (const auto& [path, contents] : files) {
        CHECK(path.find(".godot") == std::string::npos);
        CHECK(path.find("addons") == std::string::npos);
    }
}

TEST_CASE("importing a Godot scene maps its nodes to entities and components") {
    const fs::path project = "import_godot_scenes";
    fs::remove_all(project);
    make_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/scenes/level.scene.json";
    REQUIRE(files.contains(level));
    REQUIRE(files.contains("godot/scenes/enemy.scene.json"));

    // Every scene it writes validates and is in canonical form.
    slash::world::ComponentRegistry components;
    add_types(components, files);
    for (const auto& [path, contents] : files) {
        if (path.ends_with(slash::world::kSceneSuffix)) {
            CHECK_MESSAGE(
                slash::world::validate_scene(components, parse(contents), reader_of(files)).empty(),
                path);
            CHECK(slash::world::format_scene(components, contents, reader_of(files)).value() ==
                  contents);
        }
    }

    // Plain nodes under a plain root keep their children in place, so Godot's drawing order
    // holds; Node2D children hang from their parent's Transform2D. A CanvasLayer holds what
    // draws at the top of its canvas.
    CHECK(*entity(files, level, "level").find("Transform2D")->find("children") ==
          parse(R"(["world"])"));
    CHECK(*entity(files, level, "ui").find("CanvasLayer") ==
          parse(R"({"children": ["menu", "marker", "size", "file"]})"));
    CHECK(*entity(files, level, "world").find("Transform2D")->find("children") ==
          parse(R"(["floor", "enemy", "enemy2", "pin", "crate", "torch", "sparks", "tiles",
                    "old"])"));

    // Physics materials, shapes and their nodes' transforms.
    const json::Value floor = entity(files, level, "floor");
    CHECK(floor.find("StaticBody2D")->find("friction")->as_double() == doctest::Approx(0.2));
    CHECK(floor.find("StaticBody2D")->find("bounce")->as_double() == doctest::Approx(0.6));
    const json::Value edge = entity(files, level, "edge");
    CHECK(*edge.find("CollisionShape2D") ==
          parse(R"({"shape": "segment", "a": [-10.0, 0.0], "b": [10.0, 0.0]})"));
    CHECK(*edge.find("Transform2D") == parse(R"({"position": [4.0, 5.0], "rotation": 0.5})"));
    CHECK(unsupported(plan.report, "collision shape ConcavePolygonShape2D")
              .find("why")
              ->as_string()
              .find("CollisionPolygon2D") != std::string::npos);

    // An instanced scene's root extends the scene's file, and the nodes it brings have IDs under
    // the instance's; the level holds only its overrides.
    CHECK(entry(files, level, "enemy") == parse(R"({"extends": "godot/scenes/enemy.scene.json",
                    "components": {"Enemy": {"health": 5},
                                   "Transform2D": {"position": [100.0, 50.0]}}})"));
    CHECK(entry(files, level, "enemy/sprite") ==
          parse(R"({"components": {"Sprite2D": {"flip_h": true}}})"));
    const json::Value level_file = parse(files.at(level));
    CHECK(level_file.find("entities")->find("enemy2/sprite") == nullptr);
    const json::Value enemy_file = parse(files.at("godot/scenes/enemy.scene.json"));
    CHECK(enemy_file.find("root")->as_string() == "enemy");
    const json::Value enemy = entity(files, level, "enemy");
    CHECK(enemy.find("RigidBody2D")->find("mass")->as_double() == 2.0);
    CHECK(*enemy.find("Transform2D")->find("position") == parse("[100.0, 50.0]"));
    CHECK(*enemy.find("Transform2D")->find("children") ==
          parse(R"(["enemy/sprite", "enemy/shape"])"));
    const json::Value sprite = entity(files, level, "enemy/sprite");
    CHECK(sprite.find("Sprite2D")->find("flip_h")->as_bool());
    CHECK(sprite.find("Sprite2D")->find("texture")->as_string() == "godot/art/enemy.png");
    CHECK(*sprite.find("Sprite2D")->find("region") == parse("[0.0, 0.0, 16.0, 16.0]"));
    CHECK(sprite.find("Sprite2D")->find("texture_filter")->as_string() == "nearest");
    CHECK(*sprite.find("Sprite2D")->find("modulate") == parse("[1.0, 0.5, 0.5, 0.5]"));
    CHECK(entity(files, level, "enemy2/sprite").find("Sprite2D")->find("flip_h") == nullptr);
    CHECK(
        entity(files, level, "enemy/shape").find("CollisionShape2D")->find("shape")->as_string() ==
        "circle");

    // Node paths become entity IDs.
    const json::Value pin = entity(files, level, "pin");
    CHECK(pin.find("PinJoint2D")->find("node_a")->as_string() == "floor");
    CHECK(pin.find("PinJoint2D")->find("node_b")->as_string() == "enemy");

    // Resources inside fields: occluders, curves and color ramps.
    CHECK(*entity(files, level, "crate").find("LightOccluder2D") ==
          parse(R"({"closed": false, "cull_mode": "clockwise",
                    "polygon": [[0.0, 0.0], [16.0, 0.0], [16.0, 16.0]]})"));
    const json::Value torch = entity(files, level, "torch");
    CHECK(torch.find("PointLight2D")->find("blend_mode")->as_string() == "mix");
    CHECK(torch.find("PointLight2D")->find("texture")->as_string() == "godot/art/sheet.png");
    const json::Value sparks = entity(files, level, "sparks");
    CHECK(sparks.find("CPUParticles2D")->find("emission_shape")->as_string() == "ring");
    CHECK(*sparks.find("CPUParticles2D")->find("scale_amount_curve") ==
          parse("[[0.0, 1.0], [1.0, 0.0]]"));
    CHECK(*sparks.find("CPUParticles2D")->find("color_ramp") ==
          parse(R"([{"color": [1.0, 1.0, 0.0, 1.0]},
                    {"offset": 1.0, "color": [1.0, 0.0, 0.0, 0.0]}])"));

    // Sounds and controls.
    const json::Value music = entity(files, level, "music");
    CHECK(music.find("Transform2D") == nullptr);
    CHECK(music.find("AudioStreamPlayer")->find("bus")->as_string() == "Music");
    const json::Value menu = entity(files, level, "menu");
    CHECK(*menu.find("BoxContainer") == parse(R"({"alignment": "center", "vertical": true})"));
    CHECK(menu.find("Control")->find("anchor_right")->as_double() == 1.0);
    CHECK(menu.find("Control")->find("grow_horizontal")->as_string() == "both");
    CHECK(menu.find("Control")->find("theme_override_constants")->find("separation")->as_int() ==
          8);
    CHECK(*menu.find("Control")->find("children") ==
          parse(R"(["title", "play", "volume", "items"])"));
    const json::Value title = entity(files, level, "title");
    CHECK(title.find("Control")->find("size_flags_horizontal")->as_string() == "expand_fill");
    CHECK(title.find("Control")->find("size_flags_vertical")->as_string() == "shrink_center");
    CHECK(title.find("Control")->find("mouse_filter")->as_string() == "ignore");
    CHECK(title.find("Label")->find("horizontal_alignment")->as_string() == "center");
    CHECK(title.find("Control")->find("theme_override_font_sizes")->find("font_size")->as_int() ==
          20);
    CHECK(title.find("Control")
              ->find("theme_override_constants")
              ->find("shadow_offset_y")
              ->as_int() == 3);
    const json::Value play = entity(files, level, "play");
    CHECK(play.find("Control")->find("focus_mode")->as_string() == "all");
    CHECK(play.find("Control")->find("tooltip_text")->as_string() == "Start a new game");
    CHECK(*play.find("Button")->find("button_mask") == parse(R"(["left", "right"])"));
    CHECK(play.find("Button")->find("alignment") == nullptr); // fill has no equivalent
    CHECK(*play.find("Control")->find("theme_override_styles")->find("normal") ==
          parse(R"({"type": "flat", "bg_color": [0.1, 0.1, 0.2, 0.9],
                    "corner_radius_top_left": 6, "border_width_bottom": 2})"));
    CHECK(*entity(files, level, "volume").find("Slider") ==
          parse(R"({"max_value": 10.0, "vertical": true})"));

    // Option buttons, menu buttons and popup menus keep their items; an item's ID is left out
    // when it is its index, and an option button's text comes from the option selected.
    CHECK(*entity(files, level, "size").find("OptionButton") ==
          parse(R"({"selected": 1, "fit_to_longest_item": false, "items": [
              {"text": "Small"}, {"text": "Medium", "id": 5}, {"separator": true}]})"));
    CHECK(*entity(files, level, "file").find("MenuButton") ==
          parse(R"({"text": "File", "switch_on_hover": true, "items": [
              {"text": "New", "icon": "godot/art/sheet.png"},
              {"text": "Grid", "checkable": "check", "checked": true}]})"));
    const json::Value context = entity(files, level, "context");
    CHECK(context.find("Control") == nullptr);
    CHECK(context.find("Transform2D") == nullptr);
    CHECK(*context.find("PopupMenu") ==
          parse(R"({"position": [10.0, 20.0], "hide_on_checkable_item_selection": false,
                    "items": [{"text": "Copy", "id": 7},
                              {"text": "Mode", "checkable": "radio", "disabled": true}]})"));
    CHECK(unsupported(plan.report, "PopupMenu.size").find("why")->as_string() ==
          "a popup menu is as big as its items");
    CHECK(!unsupported(plan.report, "PopupMenu.item_*/shortcut").is_null());

    // What doesn't convert is reported once by kind, and kept in the report for porting.
    CHECK(!unsupported(plan.report, "node type ItemList").is_null());
    CHECK(unsupported(plan.report, "ItemList.max_columns").is_null());
    CHECK(!unsupported(plan.report, "Node2D under a Control").is_null());
    CHECK(unsupported(plan.report, "Button.alignment").find("why")->as_string() ==
          "value 3 has no equivalent");
    const json::Value report = parse(files.at("import/godot.json"));
    const json::Value& record = *report.find("scenes")->find("scenes/level.tscn");
    CHECK(record.find("entities")->find("UI/Menu/Play")->as_string() == "play");
    CHECK(*record.find("unconverted")->find("items") ==
          parse(R"({"type": "ItemList", "values": {"max_columns": 2}})"));
}

TEST_CASE("importing Godot tile maps, themes and bus layouts writes their SlashEngine files") {
    const fs::path project = "import_godot_resources";
    fs::remove_all(project);
    make_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    CHECK(*plan.report.find("converted") ==
          parse(R"({"scenes": 2, "sprite_frames": 0, "tile_sets": 1, "themes": 1,
                    "bus_layouts": 1, "animation_libraries": 0, "animation_trees": 0,
                    "process_materials": 0, "mesh_libraries": 0, "navigation_meshes": 0,
                    "audio_streams": 0, "input_actions": 1, "assets": 3})"));
    const std::string level = "godot/scenes/level.scene.json";

    // The tile set: its terrains by set, and each tile's terrain and the terrain each side must
    // touch, left out where it is the tile's own.
    const std::string tileset = "godot/tiles/terrain.tileset.json";
    REQUIRE(files.contains(tileset));
    CHECK(slash::canvas::validate_tileset(parse(files.at(tileset))).empty());
    CHECK(parse(files.at(tileset)) == parse(R"({
        "format": 1,
        "terrain_sets": {"set_0": {"mode": "match_sides", "terrains": ["Grass", "Water"]}},
        "physics_layers": [{"collision_layer": 1},
                           {"collision_layer": 4, "collision_mask": 3, "friction": 0.25,
                            "bounce": 0.5}],
        "sources": {"tiles": {"texture": "godot/art/tiles.png", "tiles": {
            "0,0": {"terrain": "Grass", "peering": {"n": null, "s": "Water", "w": null}},
            "1,0": {"terrain": "Water",
                    "alternatives": {"1": {"flip_v": true, "modulate": [1.0, 0.5, 0.5, 1.0],
                                           "texture_origin": [1, 2]}}},
            "2,0": {"probability": 0.5, "texture_origin": [0, 4]},
            "3,0": {"size_in_atlas": [2, 1]},
            "0,2": {"animation_frames": [0.5, 0.25], "animation_separation": [1, 0],
                    "animation_speed": 2.0, "animation_mode": "random_start_times"},
            "0,1": {"collision": [
                {"points": [[-8.0, -8.0], [8.0, -8.0], [8.0, 8.0], [-8.0, 8.0]]},
                {"layer": 1, "one_way": true,
                 "points": [[-8.0, -8.0], [8.0, -8.0], [8.0, -4.0], [-8.0, -4.0]]}]}}}}})"));
    CHECK(
        unsupported(plan.report, "alternative tile property z_index").find("where")->as_string() ==
        "tiles/terrain.tres: TileSetAtlasSource_1");
    // Godot 4.0 and 4.1 save zero velocities too; only others are reported.
    CHECK(unsupported(plan.report, "tile constant velocities").find("count")->as_int() == 1);

    // A TileMapLayer's cells, with an alternative tile and a cell flipped and transposed.
    const json::Value tiles = entity(files, level, "tiles");
    CHECK(tiles.find("TileMapLayer")->find("tile_set")->as_string() == tileset);
    CHECK(*tiles.find("TileMapLayer")->find("modulate") == parse("[1.0, 1.0, 1.0, 0.5]"));
    CHECK(cells_of(files, *tiles.find("TileMapLayer")) ==
          std::map<std::string, std::string>{{"0,0", "tiles:0,0"},
                                             {"1,0", "tiles:1,0"},
                                             {"-1,2", "tiles:1,0:1"},
                                             {"3,0", "tiles:0,1:ht"}});

    // An old TileMap's layers are entities under it, each with its own settings.
    const json::Value old = entity(files, level, "old");
    CHECK(*old.find("TileMapLayer") ==
          parse(R"({"tile_set": "godot/tiles/terrain.tileset.json"})"));
    CHECK(*old.find("Transform2D")->find("children") == parse(R"(["old_ground", "old_top"])"));
    CHECK(cells_of(files, *entity(files, level, "old_ground").find("TileMapLayer")) ==
          std::map<std::string, std::string>{{"0,0", "tiles:0,0"}, {"2,1", "tiles:2,0"}});
    const json::Value top = entity(files, level, "old_top");
    CHECK(top.find("TileMapLayer")->find("z_index")->as_int() == 2);
    CHECK(*top.find("TileMapLayer")->find("modulate") == parse("[1.0, 1.0, 1.0, 0.5]"));
    CHECK_FALSE(top.find("TileMapLayer")->find("navigation_enabled")->as_bool());
    CHECK(entity(files, level, "old_ground").find("TileMapLayer")->find("navigation_enabled") ==
          nullptr);
    CHECK(cells_of(files, *top.find("TileMapLayer")) ==
          std::map<std::string, std::string>{{"0,-1", "tiles:1,0"}});
    CHECK(!unsupported(plan.report, "TileMap layer y_sort_enabled").is_null());
    // Every scene's tile map layers fit their tile sets.
    CHECK(slash::canvas::check_tile_layers(parse(files.at(level)), reader_of(files)).empty());

    // The theme, used by the project and by a control.
    const std::string theme = "godot/ui/theme.theme.json";
    REQUIRE(files.contains(theme));
    CHECK(parse(files.at(theme)) == parse(R"({
        "format": 1,
        "default_font_size": 18,
        "types": {
            "Button": {"colors": {"font_color": [1.0, 0.9, 0.5, 1.0]},
                       "constants": {"h_separation": 6},
                       "styles": {"normal": {"type": "flat", "bg_color": [0.2, 0.3, 0.4, 1.0],
                                             "corner_radius_top_left": 4}}},
            "Title": {"base_type": "Label", "font_sizes": {"font_size": 24}}}})"));
    CHECK(unsupported(plan.report, "theme item styles").find("why")->as_string() ==
          "StyleBoxLine style boxes");
    CHECK(entity(files, level, "menu").find("Control")->find("theme")->as_string() == theme);

    // The bus layout, with each effect in SlashEngine's form.
    const std::string buses = "godot/default_bus_layout.bus_layout.json";
    REQUIRE(files.contains(buses));
    CHECK(parse(files.at(buses)) == parse(R"({
        "format": 1,
        "buses": [
            {"name": "Master", "volume_db": -1.0},
            {"name": "Music", "volume_db": -3.0, "effects": [
                {"type": "reverb", "room_size": 0.6, "wet": 0.4},
                {"type": "low_pass_filter", "cutoff_hz": 1200.0, "db": 12, "enabled": false}]},
            {"name": "SFX", "mute": true, "send": "Music", "effects": [
                {"type": "chorus", "voice_count": 3,
                 "voices": [{"delay_ms": 10.0}, {}, {"pan": 0.25}]},
                {"type": "eq6", "band_db": {"100_hz": -3.0}},
                {"type": "record", "format": "8_bits"},
                {"type": "pitch_shift", "pitch_scale": 1.5, "oversampling": 8,
                 "fft_size": 1024}]}]})"));
    CHECK(unsupported(plan.report, "audio effect").find("why")->as_string() ==
          "AudioEffectLimiter has no SlashEngine equivalent yet");
    // Nothing these files hold is reported as an unconverted resource.
    CHECK(unsupported(plan.report, "resource Theme").is_null());
    CHECK(unsupported(plan.report, "resource TileSet").is_null());
    CHECK(unsupported(plan.report, "resource AudioBusLayout").is_null());
}

TEST_CASE("importing a Godot hexagon tile set keeps its shape, layout and corners") {
    const fs::path project = "import_godot_hex_tiles";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Hexes\"\n");
    write_png(project / "godot/art/hex.png", 64, 28);
    // Columns of hexagons in a diamond layout. The right corner, a corner of a hexagon in
    // offset columns, is on the shape's e; the bottom right corner, between its se and s sides,
    // is sse. Godot's bottom corner is no corner of this shape, so it goes.
    write(project / "godot/tiles/hex.tres", R"([gd_resource type="TileSet" load_steps=3 format=3]

[ext_resource type="Texture2D" path="res://art/hex.png" id="1_tex"]

[sub_resource type="TileSetAtlasSource" id="TileSetAtlasSource_1"]
texture = ExtResource("1_tex")
texture_region_size = Vector2i(32, 28)
0:0/0 = 0
0:0/0/terrain_set = 0
0:0/0/terrain = 0
0:0/0/terrains_peering_bit/right_corner = 1
0:0/0/terrains_peering_bit/bottom_right_side = 1
0:0/0/terrains_peering_bit/bottom_right_corner = 1
0:0/0/terrains_peering_bit/bottom_corner = 1
0:0/0/terrains_peering_bit/bottom_side = 0
0:0/0/terrains_peering_bit/bottom_left_corner = 0
0:0/0/terrains_peering_bit/bottom_left_side = 0
0:0/0/terrains_peering_bit/left_corner = 0
0:0/0/terrains_peering_bit/top_left_side = 0
0:0/0/terrains_peering_bit/top_left_corner = 0
0:0/0/terrains_peering_bit/top_side = 0
0:0/0/terrains_peering_bit/top_right_corner = 0
0:0/0/terrains_peering_bit/top_right_side = 0
1:0/0 = 0

[resource]
tile_shape = 3
tile_layout = 5
tile_offset_axis = 1
tile_size = Vector2i(32, 28)
terrain_set_0/terrain_0/name = "Grass"
terrain_set_0/terrain_1/name = "Water"
sources/0 = SubResource("TileSetAtlasSource_1")
)");
    write(project / "godot/odd.tres", R"([gd_resource type="TileSet" format=3]

[resource]
tile_shape = 9
tile_layout = -1
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string tileset = "godot/tiles/hex.tileset.json";
    REQUIRE(files.contains(tileset));
    CHECK(slash::canvas::validate_tileset(parse(files.at(tileset))).empty());
    CHECK(parse(files.at(tileset)) == parse(R"({
        "format": 1, "tile_shape": "hexagon", "tile_layout": "diamond_down",
        "tile_offset_axis": "vertical", "tile_size": [32, 28],
        "terrain_sets": {"set_0": {"terrains": ["Grass", "Water"]}},
        "sources": {"hex": {"texture": "godot/art/hex.png", "tiles": {
            "0,0": {"terrain": "Grass", "peering": {"e": "Water", "se": "Water",
                                                    "sse": "Water"}},
            "1,0": {}}}}})"));
    CHECK(unsupported(plan.report, "TileSet.tile_shape").find("where")->as_string() == "odd.tres");
    CHECK(unsupported(plan.report, "TileSet.tile_layout").find("where")->as_string() == "odd.tres");
}

TEST_CASE("importing a Godot tile set keeps its occluders and custom data") {
    const fs::path project = "import_godot_tile_data";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Dungeon\"\n");
    write_png(project / "godot/art/dungeon.png", 48, 16);
    // Godot 4.4 saves each occluder as polygon_M/polygon; before, a tile had one, as polygon.
    // Every tile saves a value for each custom data layer; those equal to the type's default
    // go, as do occluders on layers the tile set lacks, which Godot drops too.
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[ext_resource type="Texture2D" path="res://art/dungeon.png" id="1_tex"]

[sub_resource type="OccluderPolygon2D" id="OccluderPolygon2D_wall"]
polygon = PackedVector2Array(-8, -8, 8, -8, 8, 8, -8, 8)
cull_mode = 1

[sub_resource type="OccluderPolygon2D" id="OccluderPolygon2D_rail"]
closed = false
polygon = PackedVector2Array(-8, 0, 8, 0)

[sub_resource type="OccluderPolygon2D" id="OccluderPolygon2D_dot"]
polygon = PackedVector2Array(0, 0, 1, 1)

[sub_resource type="TileSetAtlasSource" id="TileSetAtlasSource_1"]
texture = ExtResource("1_tex")
0:0/0 = 0
0:0/0/occlusion_layer_0/polygon_0/polygon = SubResource("OccluderPolygon2D_wall")
0:0/0/occlusion_layer_1/polygon_0/polygon = SubResource("OccluderPolygon2D_rail")
0:0/0/occlusion_layer_4/polygon_0/polygon = SubResource("OccluderPolygon2D_wall")
0:0/0/custom_data_0 = 0
0:0/0/custom_data_1 = "wall"
0:0/0/custom_data_2 = Vector2(0, 0)
0:0/0/custom_data_3 = Color(0, 0, 0, 1)
0:0/0/custom_data_4 = {}
0:0/0/custom_data_5 = false
0:0/1 = 1
0:0/1/flip_h = true
0:0/1/custom_data_0 = 2.7
1:0/0 = 0
1:0/0/occlusion_layer_0/polygon = SubResource("OccluderPolygon2D_rail")
1:0/0/custom_data_0 = 5
1:0/0/custom_data_1 = &"lava"
1:0/0/custom_data_2 = Vector2(1.5, 0)
1:0/0/custom_data_3 = Color(1, 0.5, 0, 1)
1:0/0/custom_data_5 = true
2:0/0 = 0
2:0/0/occlusion_layer_0/polygon_0/polygon = SubResource("OccluderPolygon2D_dot")
2:0/0/custom_data_1 = 7

[sub_resource type="TileSet" id="TileSet_1"]
occlusion_layer_0/light_mask = 1
occlusion_layer_1/light_mask = 6
occlusion_layer_1/sdf_collision = true
custom_data_layer_0/name = "damage"
custom_data_layer_0/type = 2
custom_data_layer_1/name = "kind"
custom_data_layer_1/type = 4
custom_data_layer_2/name = "push"
custom_data_layer_2/type = 5
custom_data_layer_3/name = "tint"
custom_data_layer_3/type = 20
custom_data_layer_4/name = "loot"
custom_data_layer_4/type = 27
custom_data_layer_5/name = "hot"
custom_data_layer_5/type = 1
sources/0 = SubResource("TileSetAtlasSource_1")

[node name="Level" type="Node2D"]

[node name="Walls" type="TileMapLayer" parent="."]
tile_set = SubResource("TileSet_1")
occlusion_enabled = false
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    const json::Value walls = *entity(files, level, "walls").find("TileMapLayer");
    CHECK_FALSE(walls.find("occlusion_enabled")->as_bool());
    const std::string tileset = walls.find("tile_set")->as_string();
    REQUIRE(files.contains(tileset));
    CHECK(slash::canvas::validate_tileset(parse(files.at(tileset))).empty());
    CHECK(parse(files.at(tileset)) == parse(R"({
        "format": 1,
        "occlusion_layers": [{}, {"light_mask": 6, "sdf_collision": true}],
        "custom_data_layers": [{"name": "damage", "type": "int"},
                               {"name": "kind", "type": "string"},
                               {"name": "push", "type": "vector2"},
                               {"name": "tint", "type": "color"},
                               {"name": "hot", "type": "bool"}],
        "sources": {"dungeon": {"texture": "godot/art/dungeon.png", "tiles": {
            "0,0": {"occlusion": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]],
                                   "cull_mode": "clockwise"},
                                  {"layer": 1, "points": [[-8, 0], [8, 0]], "closed": false}],
                    "custom_data": {"kind": "wall"},
                    "alternatives": {"1": {"flip_h": true, "custom_data": {"damage": 2}}}},
            "1,0": {"occlusion": [{"points": [[-8, 0], [8, 0]], "closed": false}],
                    "custom_data": {"damage": 5, "kind": "lava", "push": [1.5, 0],
                                    "tint": [1, 0.5, 0, 1], "hot": true}},
            "2,0": {}}}}})"));
    CHECK(unsupported(plan.report, "tile set occlusion SDF collision").is_null());
    const auto why = [&](std::string_view what) {
        const json::Value item = unsupported(plan.report, what);
        REQUIRE_MESSAGE(item.is_object(), what);
        return item.find("why")->as_string();
    };
    CHECK(why("tile set custom data layer type Dictionary").starts_with("layer 4 left out"));
    CHECK(why("tile custom data value") ==
          "a value that isn't string in layer \"kind\" takes the type's default");
    fs::remove_all(project);
}

TEST_CASE("importing a Godot tile set keeps its navigation layers and tiles' outlines") {
    const fs::path project = "import_godot_tile_navigation";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Maze\"\n");
    write_png(project / "godot/art/maze.png", 48, 16);
    // The tile set editor saves outlines; a polygon with only vertices and polygons covers the
    // same area. Outlines on layers the tile set lacks go, as Godot drops them.
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[ext_resource type="Texture2D" path="res://art/maze.png" id="1_tex"]

[sub_resource type="NavigationPolygon" id="NavigationPolygon_floor"]
vertices = PackedVector2Array(8, 8, -8, 8, -8, -8, 8, -8)
polygons = Array[PackedInt32Array]([PackedInt32Array(0, 1, 2, 3)])
outlines = Array[PackedVector2Array]([PackedVector2Array(-8, -8, 8, -8, 8, 8, -8, 8)])
agent_radius = 0.0

[sub_resource type="NavigationPolygon" id="NavigationPolygon_half"]
vertices = PackedVector2Array(-8, -8, 8, -8, 8, 0, -8, 0)
polygons = Array[PackedInt32Array]([PackedInt32Array(0, 1, 2, 3)])

[sub_resource type="NavigationPolygon" id="NavigationPolygon_line"]
outlines = Array[PackedVector2Array]([PackedVector2Array(0, 0, 4, 0)])

[sub_resource type="TileSetAtlasSource" id="TileSetAtlasSource_1"]
texture = ExtResource("1_tex")
0:0/0 = 0
0:0/0/navigation_layer_0/polygon = SubResource("NavigationPolygon_floor")
0:0/0/navigation_layer_2/polygon = SubResource("NavigationPolygon_floor")
0:0/1 = 1
0:0/1/navigation_layer_1/polygon = SubResource("NavigationPolygon_half")
1:0/0 = 0
1:0/0/navigation_layer_1/polygon = SubResource("NavigationPolygon_half")
2:0/0 = 0
2:0/0/navigation_layer_0/polygon = SubResource("NavigationPolygon_line")

[sub_resource type="TileSet" id="TileSet_1"]
navigation_layer_0/layers = 1
navigation_layer_1/layers = 6
sources/0 = SubResource("TileSetAtlasSource_1")

[node name="Level" type="Node2D"]

[node name="Paths" type="TileMapLayer" parent="."]
tile_set = SubResource("TileSet_1")
navigation_enabled = false
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    const json::Value paths = *entity(files, level, "paths").find("TileMapLayer");
    CHECK_FALSE(paths.find("navigation_enabled")->as_bool());
    const std::string tileset = paths.find("tile_set")->as_string();
    REQUIRE(files.contains(tileset));
    CHECK(slash::canvas::validate_tileset(parse(files.at(tileset))).empty());
    CHECK(parse(files.at(tileset)) == parse(R"({
        "format": 1,
        "navigation_layers": [{}, {"layers": 6}],
        "sources": {"maze": {"texture": "godot/art/maze.png", "tiles": {
            "0,0": {"navigation": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}],
                    "alternatives": {"1": {"navigation": [{"layer": 1,
                        "points": [[-8, -8], [8, -8], [8, 0], [-8, 0]]}]}}},
            "1,0": {"navigation": [{"layer": 1, "points": [[-8, -8], [8, -8], [8, 0], [-8, 0]]}]},
            "2,0": {}}}}})"));
    CHECK(unsupported(plan.report, "tile set navigation layers").is_null());
    const json::Value line = unsupported(plan.report, "tile navigation outline with no area");
    REQUIRE(line.is_object());
    CHECK(line.find("why")->as_string().starts_with("left out"));
    fs::remove_all(project);
}

TEST_CASE("an instanced Godot TileMap keeps its layers' entities under the instance") {
    const fs::path project = "import_godot_tile_instances";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Rooms\"\n");
    write(project / "godot/tiles/terrain.tres", kTileSet);
    write_png(project / "godot/art/tiles.png", 64, 32);
    write(project / "godot/room.tscn", R"([gd_scene format=3]

[ext_resource type="TileSet" path="res://tiles/terrain.tres" id="1_tiles"]

[node name="Room" type="TileMap"]
tile_set = ExtResource("1_tiles")
format = 2
layer_0/name = "Ground"
layer_0/tile_data = PackedInt32Array(0, 0, 0)
layer_1/name = "Top"
layer_1/tile_data = PackedInt32Array(65536, 65536, 0)
)");
    write(project / "godot/house.tscn", R"([gd_scene format=3]

[ext_resource type="PackedScene" path="res://room.tscn" id="1_room"]

[node name="House" type="Node2D"]

[node name="Room1" parent="." instance=ExtResource("1_room")]
layer_1/modulate = Color(1, 0, 0, 1)

[node name="Room2" parent="." instance=ExtResource("1_room")]
position = Vector2(64, 0)
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string house = "godot/house.scene.json";
    REQUIRE(files.contains(house));
    CHECK(*entity(files, "godot/room.scene.json", "room").find("Transform2D")->find("children") ==
          parse(R"(["room_ground", "room_top"])"));
    // The layers are the instance's parts; the house holds only what it changes.
    CHECK(*entity(files, house, "room1").find("Transform2D")->find("children") ==
          parse(R"(["room1/room_ground", "room1/room_top"])"));
    CHECK(entry(files, house, "room1/room_top") ==
          parse(R"({"components": {"TileMapLayer": {"modulate": [1.0, 0.0, 0.0, 1.0]}}})"));
    CHECK(parse(files.at(house)).find("entities")->find("room2/room_top") == nullptr);
    CHECK(cells_of(files, *entity(files, house, "room2/room_top").find("TileMapLayer")) ==
          std::map<std::string, std::string>{{"0,1", "tiles:1,0"}});
}

TEST_CASE("importing Godot canvas layers and parallax converts them as Godot's converter does") {
    const fs::path project = "import_godot_canvas_layers";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Layers\"\n");
    write_png(project / "godot/art/sky.png", 16, 16);
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[ext_resource type="Texture2D" path="res://art/sky.png" id="1_sky"]

[node name="Level" type="Node2D"]

[node name="Back" type="ParallaxBackground" parent="."]
scroll_offset = Vector2(-50, 0)
scroll_base_offset = Vector2(10, 0)
scroll_base_scale = Vector2(0.5, 1)
scroll_limit_end = Vector2(2000, 0)
scroll_ignore_camera_zoom = true

[node name="Clouds" type="ParallaxLayer" parent="Back"]
position = Vector2(0, 20)
motion_scale = Vector2(0.5, 0.25)
motion_offset = Vector2(3, 4)
motion_mirroring = Vector2(320, 0)

[node name="Sky" type="Sprite2D" parent="Back/Clouds"]
texture = ExtResource("1_sky")

[node name="Hills" type="ParallaxBackground" parent="."]
layer = -5

[node name="Far" type="ParallaxLayer" parent="Hills"]

[node name="Trees" type="Parallax2D" parent="."]
scroll_scale = Vector2(1.5, 1)
repeat_size = Vector2(256, 0)
repeat_times = 3
autoscroll = Vector2(-20, 0)

[node name="HUD" type="CanvasLayer" parent="."]
layer = 2
offset = Vector2(4, 8)
transform = Transform2D(1, 0, 0, 1, 4, 8)
follow_viewport_enabled = true
follow_viewport_scale = 0.5

[node name="Group" type="Node" parent="HUD"]

[node name="Heart" type="Sprite2D" parent="HUD/Group"]
texture = ExtResource("1_sky")

[node name="Score" type="Label" parent="HUD"]
text = "0"
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    slash::world::ComponentRegistry components;
    add_types(components, files);
    CHECK(
        slash::world::validate_scene(components, parse(files.at(level)), reader_of(files)).empty());

    // A ParallaxBackground that ignores the camera's zoom is a CanvasLayer at its layer, -100,
    // holding its ParallaxLayers, which are Parallax2Ds.
    const json::Value back = entity(files, level, "back");
    CHECK(back.find("Transform2D") == nullptr);
    CHECK(*back.find("CanvasLayer") == parse(R"({"layer": -100, "children": ["clouds"]})"));
    const json::Value clouds = entity(files, level, "clouds");
    CHECK(*clouds.find("Parallax2D") == parse(R"({"scroll_scale": [0.25, 0.25],
        "scroll_offset": [8.0, 24.0], "repeat_size": [320.0, 0.0],
        "limit_begin": [0.0, -10000000.0], "limit_end": [2000.0, 10000000.0],
        "follow_viewport": false})"));
    CHECK(*clouds.find("Transform2D") == parse(R"({"children": ["sky"]})"));
    // One that follows the zoom is drawn with the world, as a Node2D.
    const json::Value hills = entity(files, level, "hills");
    CHECK(hills.find("CanvasLayer") == nullptr);
    CHECK(*hills.find("Transform2D") == parse(R"({"children": ["far"]})"));
    CHECK(*entity(files, level, "far").find("Parallax2D") == parse("{}"));
    CHECK(*entity(files, level, "trees").find("Parallax2D") ==
          parse(R"({"scroll_scale": [1.5, 1.0], "repeat_size": [256.0, 0.0],
                    "autoscroll": [-20.0, 0.0], "repeat_times": 3})"));
    CHECK(*entity(files, level, "level").find("Transform2D")->find("children") ==
          parse(R"(["hills", "trees"])"));

    // A CanvasLayer keeps its fields; a plain node in it holds its children in place.
    const json::Value hud = entity(files, level, "hud");
    CHECK(hud.find("Transform2D") == nullptr);
    CHECK(*hud.find("CanvasLayer") ==
          parse(R"({"layer": 2, "offset": [4.0, 8.0], "follow_viewport_enabled": true,
                    "follow_viewport_scale": 0.5, "children": ["group", "score"]})"));
    CHECK(*entity(files, level, "group").find("Transform2D") ==
          parse(R"({"children": ["heart"]})"));

    CHECK(unsupported(plan.report, "CanvasLayer.transform").is_null());
    CHECK(unsupported(plan.report, "ParallaxBackground.scroll_offset").is_null());
    CHECK(unsupported(plan.report, "ParallaxLayer.position").is_null());
    CHECK(unsupported(plan.report, "node type ParallaxLayer").is_null());
    CHECK(unsupported(plan.report, "ParallaxBackground.layer")
              .find("why")
              ->as_string()
              .find("with the world") != std::string::npos);
}

constexpr std::string_view kHero = R"([gd_scene load_steps=5 format=3]

[ext_resource type="Texture2D" path="res://art/hero.png" id="1_tex"]
[ext_resource type="AnimationLibrary" path="res://anim/extra.tres" id="2_extra"]

[sub_resource type="Animation" id="Animation_reset"]
resource_name = "RESET"
length = 0.001
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Sprite2D:frame")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [0]
}

[sub_resource type="Animation" id="Animation_walk"]
resource_name = "walk"
length = 0.4
loop_mode = 1
step = 0.1
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Sprite2D:frame")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0, 0.2),
"transitions": PackedFloat32Array(1, 1),
"update": 0,
"values": [0, 1]
}
tracks/1/type = "value"
tracks/1/imported = false
tracks/1/enabled = true
tracks/1/path = NodePath(".:position")
tracks/1/interp = 2
tracks/1/loop_wrap = true
tracks/1/keys = {
"times": PackedFloat32Array(0, 0.2, 0.6),
"transitions": PackedFloat32Array(1, 0.5, 1),
"update": 0,
"values": [Vector2(0, 0), Vector2(0, -4), Vector2(0, 0)]
}
tracks/2/type = "method"
tracks/2/imported = false
tracks/2/enabled = true
tracks/2/path = NodePath(".")
tracks/2/interp = 1
tracks/2/loop_wrap = true
tracks/2/keys = {
"times": PackedFloat32Array(0, 0.2),
"transitions": PackedFloat32Array(1, 1),
"values": [{
"args": [&"left"],
"method": &"footstep"
}, {
"args": [],
"method": &"blink"
}]
}
tracks/3/type = "bezier"
tracks/3/imported = false
tracks/3/enabled = true
tracks/3/path = NodePath("Sprite2D:modulate:a")
tracks/3/interp = 1
tracks/3/loop_wrap = true
tracks/3/keys = {
"handle_modes": PackedInt32Array(0, 0),
"points": PackedFloat32Array(1, -0.1, 0, 0.1, 0, 0.5, -0.1, 0, 0.1, 0),
"times": PackedFloat32Array(0, 0.4)
}
tracks/4/type = "audio"
tracks/4/imported = false
tracks/4/enabled = true
tracks/4/path = NodePath("Sprite2D")
tracks/4/keys = {
"clips": [],
"times": PackedFloat32Array()
}
tracks/4/use_blend = true
tracks/5/type = "value"
tracks/5/imported = false
tracks/5/enabled = false
tracks/5/path = NodePath("Sprite2D:visible")
tracks/5/interp = 1
tracks/5/loop_wrap = true
tracks/5/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [false]
}
tracks/6/type = "value"
tracks/6/imported = false
tracks/6/enabled = true
tracks/6/path = NodePath("Missing:visible")
tracks/6/interp = 1
tracks/6/loop_wrap = true
tracks/6/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [false]
}
tracks/7/type = "value"
tracks/7/imported = false
tracks/7/enabled = true
tracks/7/path = NodePath("Sprite2D:texture")
tracks/7/interp = 1
tracks/7/loop_wrap = true
tracks/7/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [ExtResource("1_tex")]
}
tracks/8/type = "value"
tracks/8/imported = false
tracks/8/enabled = true
tracks/8/path = NodePath("Sprite2D:offset:y")
tracks/8/interp = 1
tracks/8/loop_wrap = true
tracks/8/keys = {
"times": PackedFloat32Array(0, 0.2),
"transitions": PackedFloat32Array(1, 1),
"update": 1,
"values": [0.0, -2.0]
}

[sub_resource type="AnimationLibrary" id="AnimationLibrary_hero"]
_data = {
&"RESET": SubResource("Animation_reset"),
&"walk": SubResource("Animation_walk")
}

[node name="Hero" type="Node2D"]

[node name="Sprite2D" type="Sprite2D" parent="."]
texture = ExtResource("1_tex")
hframes = 2

[node name="AnimationPlayer" type="AnimationPlayer" parent="."]
libraries = {
&"": SubResource("AnimationLibrary_hero"),
&"extra": ExtResource("2_extra")
}
autoplay = "walk"
speed_scale = 2.0
)";

constexpr std::string_view kExtraAnimations =
    R"([gd_resource type="AnimationLibrary" load_steps=2 format=3]

[sub_resource type="Animation" id="Animation_spin"]
resource_name = "spin"
loop_mode = 2
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Sprite2D:rotation")
tracks/0/interp = 3
tracks/0/loop_wrap = false
tracks/0/keys = {
"times": PackedFloat32Array(0, 1),
"transitions": PackedFloat32Array(1, 1),
"update": 0,
"values": [0.0, 3.14]
}

[resource]
_data = {
"spin": SubResource("Animation_spin")
}
)";

TEST_CASE("importing Godot CanvasTextures and light layer ranges keeps the lighting") {
    const fs::path project = "import_godot_lit";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Lit\"\n");
    for (const std::string_view name : {"wall", "wall_n", "wall_s"}) {
        write_png(project / "godot/art" / (std::string(name) + ".png"), 16, 16);
    }
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[ext_resource type="Texture2D" path="res://art/wall.png" id="1_wall"]
[ext_resource type="Texture2D" path="res://art/wall_n.png" id="2_normal"]
[ext_resource type="Texture2D" path="res://art/wall_s.png" id="3_specular"]

[sub_resource type="CanvasTexture" id="CanvasTexture_1"]
diffuse_texture = ExtResource("1_wall")
normal_texture = ExtResource("2_normal")
specular_texture = ExtResource("3_specular")
specular_color = Color(1, 0.5, 0.25, 1)
specular_shininess = 0.5
texture_filter = 1

[sub_resource type="TileSetAtlasSource" id="TileSetAtlasSource_1"]
texture = SubResource("CanvasTexture_1")
0:0/0 = 0

[sub_resource type="TileSet" id="TileSet_1"]
sources/0 = SubResource("TileSetAtlasSource_1")

[node name="Level" type="Node2D"]

[node name="Wall" type="Sprite2D" parent="."]
texture = SubResource("CanvasTexture_1")

[node name="Ground" type="TileMapLayer" parent="."]
tile_set = SubResource("TileSet_1")

[node name="Torch" type="PointLight2D" parent="."]
texture = ExtResource("1_wall")
range_layer_min = -1
range_layer_max = 2
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));

    // A sprite's CanvasTexture becomes its texture, normal and specular maps.
    CHECK(*entity(files, level, "wall").find("Sprite2D") == parse(R"({
        "texture": "godot/art/wall.png", "normal_texture": "godot/art/wall_n.png",
        "specular_texture": "godot/art/wall_s.png", "specular_color": [1.0, 0.5, 0.25, 1.0],
        "specular_shininess": 0.5})"));
    // So does a tile set source's.
    const std::string tileset =
        entity(files, level, "ground").find("TileMapLayer")->find("tile_set")->as_string();
    REQUIRE(files.contains(tileset));
    CHECK(slash::canvas::validate_tileset(parse(files.at(tileset))).empty());
    CHECK(*parse(files.at(tileset)).find("sources")->find("wall") == parse(R"({
        "texture": "godot/art/wall.png", "normal_texture": "godot/art/wall_n.png",
        "specular_texture": "godot/art/wall_s.png", "specular_color": [1.0, 0.5, 0.25, 1.0],
        "specular_shininess": 0.5, "tiles": {"0,0": {}}})"));
    CHECK(unsupported(plan.report, "CanvasTexture texture_filter").find("count")->as_int() == 2);
    // Light layer ranges come as they are.
    const json::Value torch = entity(files, level, "torch");
    CHECK(torch.find("PointLight2D")->find("range_layer_min")->as_int() == -1);
    CHECK(torch.find("PointLight2D")->find("range_layer_max")->as_int() == 2);
    fs::remove_all(project);
}

TEST_CASE("importing Godot gravity areas, constant velocities and character options keeps them") {
    const fs::path project = "import_godot_physics";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Planets\"\n");
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[node name="Level" type="Node2D"]

[node name="Planet" type="Area2D" parent="."]
priority = 2
gravity_space_override = 3
gravity_point = true
gravity_point_unit_distance = 64.0
gravity_point_center = Vector2(0, 0)
gravity_direction = Vector2(0, 0)
gravity = 400.0
linear_damp_space_override = 1
linear_damp = 0.5
angular_damp_space_override = 4
angular_damp = 3.0

[node name="Belt" type="StaticBody2D" parent="."]
constant_linear_velocity = Vector2(60, 0)
constant_angular_velocity = 0.5

[node name="Lift" type="AnimatableBody2D" parent="."]
constant_linear_velocity = Vector2(0, -30)

[node name="Hero" type="CharacterBody2D" parent="."]
wall_min_slide_angle = 0.5
floor_constant_speed = true
floor_block_on_wall = false
platform_floor_layers = 4294967294
platform_wall_layers = 2
safe_margin = 0.5
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    CHECK(*entity(files, level, "planet").find("Area2D") == parse(R"({
        "priority": 2, "gravity_space_override": "replace", "gravity_point": true,
        "gravity_point_unit_distance": 64.0, "gravity_point_center": [0.0, 0.0],
        "gravity_direction": [0.0, 0.0], "gravity": 400.0,
        "linear_damp_space_override": "combine", "linear_damp": 0.5,
        "angular_damp_space_override": "replace_combine", "angular_damp": 3.0})"));
    CHECK(*entity(files, level, "belt").find("StaticBody2D") ==
          parse(R"({"constant_linear_velocity": [60.0, 0.0], "constant_angular_velocity": 0.5})"));
    CHECK(*entity(files, level, "lift").find("AnimatableBody2D") ==
          parse(R"({"constant_linear_velocity": [0.0, -30.0]})"));
    CHECK(*entity(files, level, "hero").find("CharacterBody2D") == parse(R"({
        "wall_min_slide_angle": 0.5, "floor_constant_speed": true,
        "floor_block_on_wall": false, "platform_floor_layers": 4294967294,
        "platform_wall_layers": 2, "safe_margin": 0.5})"));
    for (const json::Value& item : plan.report.find("unsupported")->as_array()) {
        CHECK_MESSAGE(item.find("what")->as_string().find("2D.") == std::string::npos,
                      item.find("what")->as_string());
    }
    fs::remove_all(project);
}

// A Godot 3D project: a level of meshes, models, lights, a sky and bodies.
fs::path make_3d_project(const fs::path& folder) {
    fs::remove_all(folder);
    write(folder / "project.godot", R"(config_version=5

[application]

config/name="Garden"
run/main_scene="res://level.tscn"

[audio]

general/3d_panning_strength=0.75

[physics]

3d/default_gravity=12.0
3d/default_gravity_vector=Vector3(0, -1, 0)
3d/default_linear_damp=0.2
jolt_physics_3d/simulation/areas_detect_static_bodies=true
)");
    write(folder / "level.tscn", R"([gd_scene load_steps=16 format=3]

[ext_resource type="Texture2D" path="res://art/bricks.jpg" id="1_bricks"]
[ext_resource type="ArrayMesh" path="res://models/rock.obj" id="2_rock"]
[ext_resource type="PackedScene" path="res://models/cart.gltf" id="3_cart"]

[sub_resource type="StandardMaterial3D" id="StandardMaterial3D_wall"]
transparency = 3
albedo_color = Color(0.8, 0.4, 0.2, 1)
albedo_texture = ExtResource("1_bricks")
roughness = 0.6
texture_filter = 0
uv1_scale = Vector3(2, 2, 1)
rim_enabled = true

[sub_resource type="BoxMesh" id="BoxMesh_wall"]
material = SubResource("StandardMaterial3D_wall")
size = Vector3(4, 2, 0.5)

[sub_resource type="ORMMaterial3D" id="ORMMaterial3D_metal"]
metallic = 1.0
orm_texture = ExtResource("1_bricks")

[sub_resource type="SphereMesh" id="SphereMesh_ball"]
material = SubResource("StandardMaterial3D_wall")
radius = 0.25
height = 0.5

[sub_resource type="ArrayMesh" id="ArrayMesh_blob"]

[sub_resource type="ProceduralSkyMaterial" id="ProceduralSkyMaterial_1"]
sky_top_color = Color(0.2, 0.4, 0.8, 1)
sun_angle_max = 20.0

[sub_resource type="Sky" id="Sky_1"]
sky_material = SubResource("ProceduralSkyMaterial_1")
radiance_size = 2
process_mode = 3

[sub_resource type="Environment" id="Environment_1"]
background_mode = 2
sky = SubResource("Sky_1")
ambient_light_source = 3
tonemap_mode = 3
glow_enabled = true
glow_levels/2 = 0.5
ssao_enabled = true
fog_enabled = true
fog_density = 0.02

[sub_resource type="BoxShape3D" id="BoxShape3D_1"]
size = Vector3(1, 2, 1)

[sub_resource type="ConvexPolygonShape3D" id="ConvexPolygonShape3D_1"]
points = PackedVector3Array(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)

[sub_resource type="WorldBoundaryShape3D" id="WorldBoundaryShape3D_1"]
plane = Plane(0, 1, 0, -2)

[sub_resource type="PhysicsMaterial" id="PhysicsMaterial_1"]
friction = 0.3
bounce = 0.5

[node name="Level" type="Node3D"]

[node name="Wall" type="MeshInstance3D" parent="."]
transform = Transform3D(0, 0, 1, 0, 1, 0, -1, 0, 0, 3, 1, -2)
mesh = SubResource("BoxMesh_wall")
cast_shadow = 2

[node name="Ball" type="MeshInstance3D" parent="Wall"]
transform = Transform3D(2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 1.5, 0)
mesh = SubResource("SphereMesh_ball")
surface_material_override/0 = SubResource("ORMMaterial3D_metal")

[node name="Rock" type="MeshInstance3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 0.5, -0.866025, 0, 0.866025, 0.5, 0, 0, 5)
mesh = ExtResource("2_rock")

[node name="Blob" type="MeshInstance3D" parent="."]
mesh = SubResource("ArrayMesh_blob")

[node name="Cart" parent="." instance=ExtResource("3_cart")]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, -4, 0, 0)

[node name="Wheel" parent="Cart"]
visible = false

[node name="Hitbox" type="Area3D" parent="Cart"]
monitorable = false

[node name="Camera" type="Camera3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 3, 8)
fov = 60.0
keep_aspect = 0
current = true

[node name="Sun" type="DirectionalLight3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 0.707107, 0.707107, 0, -0.707107, 0.707107, 0, 10, 0)
light_energy = 1.5
shadow_enabled = true
directional_shadow_mode = 1
sky_mode = 1

[node name="Lamp" type="OmniLight3D" parent="."]
light_color = Color(1, 0.8, 0.6, 1)
omni_range = 8.0

[node name="Torch" type="SpotLight3D" parent="."]
light_indirect_energy = 2.0
spot_angle = 30.0

[node name="Environment" type="WorldEnvironment" parent="."]
environment = SubResource("Environment_1")

[node name="Crate" type="RigidBody3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 4, 0)
mass = 5.0
physics_material_override = SubResource("PhysicsMaterial_1")
freeze_mode = 1
linear_damp_mode = 1

[node name="Shape" type="CollisionShape3D" parent="Crate"]
shape = SubResource("BoxShape3D_1")

[node name="Ground" type="StaticBody3D" parent="."]

[node name="Hull" type="CollisionShape3D" parent="Ground"]
shape = SubResource("ConvexPolygonShape3D_1")

[node name="Floor" type="CollisionShape3D" parent="Ground"]
shape = SubResource("WorldBoundaryShape3D_1")

[node name="Hero" type="CharacterBody3D" parent="."]
motion_mode = 1
floor_max_angle = 0.5

[node name="Hum" type="AudioStreamPlayer3D" parent="Hero"]
attenuation_model = 2
unit_size = 4.0
autoplay = true
doppler_tracking = 2

[node name="Enemies" type="Node" parent="."]

[node name="Bat" type="Node3D" parent="Enemies"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3)

[node name="Mirror" type="Node3D" parent="."]
transform = Transform3D(-1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)

[node name="Sparks" type="GPUParticles3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0)
amount = 16

[node name="Rig" type="Skeleton3D" parent="."]
)");
    write_png(folder / "art/bricks.jpg", 8, 8);
    write(folder / "models/rock.obj", "mtllib rock.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    write(folder / "models/rock.mtl", "newmtl stone\nKd 0.5 0.5 0.5\n");
    write(folder / "models/cart.gltf",
          R"({"asset": {"version": "2.0"}, "buffers": [{"uri": "cart.bin", "byteLength": 4}],
              "images": [{"uri": "cart.png"}, {"uri": "data:image/png;base64,AAAA"}]})");
    write(folder / "models/cart.bin", "abcd");
    write_png(folder / "models/cart.png", 4, 4);
    write(folder / "models/cart.gltf.import", R"([remap]

importer="scene"
type="PackedScene"

[params]

nodes/root_type="VehicleBody3D"
nodes/root_scale=0.5
)");
    write(folder / "models/statue.dae", "<COLLADA/>");
    return folder;
}

TEST_CASE("importing a Godot 3D scene keeps its nodes, meshes, materials, lights and bodies") {
    const fs::path project = "import_godot_3d";
    make_3d_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));

    // Transforms decompose as Godot's Node3D does, and Node3D children nest.
    CHECK(*entity(files, level, "level").find("Transform3D") == parse(R"({
        "children": ["wall", "rock", "blob", "cart", "camera", "sun", "lamp", "torch", "crate",
                     "ground", "hero", "mirror", "sparks", "rig"]})"));
    CHECK(*entity(files, level, "wall").find("Transform3D") ==
          parse(R"({"position": [3.0, 1.0, -2.0], "rotation": [0.0, 1.570796, 0.0],
                    "children": ["ball"]})"));
    CHECK(*entity(files, level, "ball").find("Transform3D") ==
          parse(R"({"position": [0.0, 1.5, 0.0], "scale": [2.0, 2.0, 2.0]})"));
    CHECK(*entity(files, level, "rock").find("Transform3D") ==
          parse(R"({"position": [0.0, 0.0, 5.0], "rotation": [1.047197, 0.0, 0.0]})"));
    CHECK(*entity(files, level, "sun").find("Transform3D") ==
          parse(R"({"position": [0.0, 10.0, 0.0], "rotation": [-0.785398, 0.0, 0.0]})"));
    // A mirrored basis, as Godot reads it: turned half round x with every axis flipped.
    CHECK(*entity(files, level, "mirror").find("Transform3D") ==
          parse(R"({"rotation": [3.141593, 0.0, 0.0], "scale": [-1.0, -1.0, -1.0]})"));
    // A plain Node breaks the chain: what is under it is placed in the world.
    CHECK(entity(files, level, "enemies").size() == 0);
    CHECK(*entity(files, level, "bat").find("Transform3D") ==
          parse(R"({"position": [1.0, 2.0, 3.0]})"));

    // Primitive meshes and their materials; a surface override beats the mesh's own material.
    const json::Value wall = entity(files, level, "wall");
    CHECK(*wall.find("BoxMesh") == parse(R"({"size": [4.0, 2.0, 0.5]})"));
    CHECK(*wall.find("MeshInstance3D") == parse(R"({"cast_shadow": "double_sided"})"));
    CHECK(*wall.find("StandardMaterial3D") == parse(R"({
        "transparency": "alpha_scissor", "albedo_color": [0.8, 0.4, 0.2, 1.0],
        "albedo_texture": "godot/art/bricks.jpg", "roughness": 0.6, "texture_filter": "nearest",
        "uv1_scale": [2.0, 2.0, 1.0]})"));
    const json::Value ball = entity(files, level, "ball");
    CHECK(*ball.find("SphereMesh") == parse(R"({"radius": 0.25, "height": 0.5})"));
    CHECK(*ball.find("StandardMaterial3D") == parse(R"({
        "metallic": 1.0, "metallic_texture": "godot/art/bricks.jpg",
        "metallic_texture_channel": "blue", "roughness_texture": "godot/art/bricks.jpg",
        "roughness_texture_channel": "green"})"));
    // A model Godot imports as a mesh, and one it imports as a scene.
    CHECK(*entity(files, level, "rock").find("MeshInstance3D") ==
          parse(R"({"mesh": "godot/models/rock.obj"})"));
    const json::Value cart = entity(files, level, "cart");
    CHECK(*cart.find("MeshInstance3D") == parse(R"({"mesh": "godot/models/cart.gltf"})"));
    CHECK(*cart.find("Transform3D") ==
          parse(R"({"position": [-4.0, 0.0, 0.0], "children": ["hitbox"]})"));
    CHECK(*entity(files, level, "hitbox").find("Area3D") == parse(R"({"monitorable": false})"));
    CHECK(entity(files, level, "blob").find("ArrayMesh") == nullptr);

    // Cameras, lights and the environment with its sky.
    CHECK(*entity(files, level, "camera").find("Camera3D") ==
          parse(R"({"fov": 60.0, "keep_aspect": "width", "current": true})"));
    CHECK(*entity(files, level, "sun").find("DirectionalLight3D") == parse(R"({
        "light_energy": 1.5, "shadow_enabled": true,
        "directional_shadow_mode": "parallel_2_splits", "sky_mode": "light_only"})"));
    CHECK(*entity(files, level, "lamp").find("OmniLight3D") ==
          parse(R"({"light_color": [1.0, 0.8, 0.6, 1.0], "omni_range": 8.0})"));
    CHECK(*entity(files, level, "torch").find("SpotLight3D") == parse(R"({"spot_angle": 30.0})"));
    const json::Value environment = entity(files, level, "environment");
    CHECK(environment.find("Transform2D") == nullptr);
    CHECK(*environment.find("WorldEnvironment") == parse(R"({
        "background_mode": "sky", "ambient_light_source": "sky", "tonemap_mode": "aces",
        "glow_enabled": true, "fog_enabled": true, "fog_density": 0.02,
        "sky_radiance_size": 128})"));
    CHECK(*environment.find("ProceduralSkyMaterial") ==
          parse(R"({"sky_top_color": [0.2, 0.4, 0.8, 1.0], "sun_angle_max": 20.0})"));

    // Bodies and their shapes.
    CHECK(*entity(files, level, "crate").find("RigidBody3D") == parse(R"({
        "mass": 5.0, "friction": 0.3, "bounce": 0.5, "freeze_mode": "kinematic",
        "linear_damp_mode": "replace"})"));
    CHECK(*entity(files, level, "shape").find("CollisionShape3D") ==
          parse(R"({"size": [1.0, 2.0, 1.0]})"));
    CHECK(*entity(files, level, "hull").find("CollisionShape3D") == parse(R"({
        "shape": "convex_polygon",
        "points": [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]})"));
    CHECK(*entity(files, level, "floor").find("CollisionShape3D") ==
          parse(R"({"shape": "world_boundary", "plane": [0.0, 1.0, 0.0, -2.0]})"));
    CHECK(*entity(files, level, "hero").find("CharacterBody3D") ==
          parse(R"({"motion_mode": "floating", "floor_max_angle": 0.5})"));
    CHECK(*entity(files, level, "hum").find("AudioStreamPlayer3D") == parse(R"({
        "attenuation_model": "logarithmic", "unit_size": 4.0, "autoplay": true,
        "doppler_tracking": "physics_step"})"));
    // A 3D node that doesn't convert still places what is under it.
    CHECK(*entity(files, level, "sparks").find("Transform3D") ==
          parse(R"({"position": [0.0, 1.0, 0.0]})"));

    const json::Value project_file = parse(files.at("project.json"));
    CHECK(*project_file.find("physics_3d") == parse(R"({
        "gravity": 12.0, "gravity_vector": [0.0, -1.0, 0.0], "linear_damp": 0.2,
        "areas_detect_static_bodies": true})"));
    CHECK(project_file.find("audio")->find("panning_strength_3d")->as_double() == 0.75);

    const json::Value& report = plan.report;
    const auto why = [&](std::string_view what) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        return item.find("why")->as_string();
    };
    CHECK(why("StandardMaterial3D.transparency") ==
          "alpha hash dithers; it cuts at alpha_scissor_threshold instead");
    CHECK(!unsupported(report, "StandardMaterial3D.rim_enabled").is_null());
    CHECK(why("MeshInstance3D.mesh") ==
          "a mesh saved in Godot's own format; export it as a glTF file");
    CHECK(unsupported(report, "node in an imported model").find("where")->as_string() ==
          "level.tscn: Cart/Wheel");
    CHECK(why("model root scale") ==
          "the model draws at its file's own scale; scale the entities that draw it instead");
    CHECK(why("model root type") == "its instances become Node3D entities that draw it; add the "
                                    "VehicleBody3D component to them by hand");
    CHECK(!unsupported(report, "Environment.glow_levels").is_null());
    CHECK(!unsupported(report, "Environment.ssao_enabled").is_null());
    CHECK(!unsupported(report, "SpotLight3D.light_indirect_energy").is_null());
    CHECK(why("node type GPUParticles3D") == "no SlashEngine component for it yet");
    CHECK(why("node type Skeleton3D") ==
          "a skeleton a scene builds itself doesn't convert yet; one "
          "an instance of a model file brings does");
    CHECK(why("file type .dae") == "models are glTF, OBJ, FBX or .blend files; export it as glTF");
    CHECK(unsupported(report, "file type .jpg").is_null());
    CHECK(unsupported(report, "Sky.process_mode").is_null());
    fs::remove_all(project);
}

TEST_CASE("a Godot 3D project outside the destination brings its models along") {
    const fs::path source = make_3d_project("import_godot_3d_source");
    const fs::path destination = "import_godot_3d_destination";
    fs::remove_all(destination);
    auto result =
        importer::import_project({.source = source.string(), .destination = destination.string()});
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
    for (const char* file : {"models/rock.obj", "models/rock.mtl", "models/cart.gltf",
                             "models/cart.bin", "models/cart.png", "art/bricks.jpg"}) {
        CHECK_MESSAGE(fs::exists(destination / file), file);
    }
    CHECK_FALSE(fs::exists(destination / "models/statue.dae"));
    fs::remove_all(source);
    fs::remove_all(destination);
}

// A Godot 3D level built from CSG, a path, grid maps, joints, a vehicle and navigation, and a 2D
// map with a path and navigation.
fs::path make_level_project(const fs::path& folder) {
    fs::remove_all(folder);
    write(folder / "project.godot", R"(config_version=5

[application]

config/name="Fort"
run/main_scene="res://level.tscn"
)");
    write(folder / "blocks.tres", R"([gd_resource type="MeshLibrary" load_steps=6 format=3]

[sub_resource type="StandardMaterial3D" id="stone"]
albedo_color = Color(0.5, 0.5, 0.45, 1)

[sub_resource type="BoxMesh" id="block"]
material = SubResource("stone")
size = Vector3(2, 2, 2)

[sub_resource type="PrismMesh" id="ramp"]
size = Vector3(2, 2, 2)

[sub_resource type="BoxShape3D" id="block_shape"]
size = Vector3(2, 2, 2)

[resource]
item/0/name = "Floor"
item/0/mesh = SubResource("block")
item/0/mesh_transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.5, 0)
item/0/mesh_cast_shadow = 0
item/0/shapes = [SubResource("block_shape"), Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.5, 0)]
item/0/navigation_mesh = ExtResource("1_nav")
item/0/navigation_mesh_transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.5, 0)
item/0/navigation_layers = 3
item/1/name = "Ramp:steep"
item/1/mesh = SubResource("ramp")
item/1/mesh_transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
item/1/preview = null
item/2/name = "Floor"
item/2/mesh = ExtResource("2_rock")
item/2/shapes = [SubResource("block_shape"), Transform3D(2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0)]
item/3/mesh = SubResource("block")
)");
    // The ExtResources the library names, declared as Godot declares them.
    {
        std::string text = read(folder / "blocks.tres");
        text.insert(text.find("\n\n") + 2,
                    "[ext_resource type=\"NavigationMesh\" path=\"res://nav/square.tres\" "
                    "id=\"1_nav\"]\n[ext_resource type=\"ArrayMesh\" path=\"res://rock.obj\" "
                    "id=\"2_rock\"]\n\n");
        write(folder / "blocks.tres", text);
    }
    write(folder / "rock.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    write(folder / "rock.obj.import", R"([remap]

importer="wavefront_obj"
type="Mesh"
)");
    write(folder / "nav/square.tres", R"([gd_resource type="NavigationMesh" format=3]

[resource]
vertices = PackedVector3Array(-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1)
polygons = [PackedInt32Array(3, 2, 1, 0)]
cell_size = 0.1
agent_radius = 0.25
)");
    // A mesh library saved in Godot's binary form.
    {
        const std::string text = R"([gd_resource type="MeshLibrary" load_steps=2 format=3]

[sub_resource type="CylinderMesh" id="pillar"]
top_radius = 0.4
bottom_radius = 0.4
height = 4.0

[resource]
item/5/name = "Pillar"
item/5/mesh = SubResource("pillar")
)";
        auto sections = godot::parse_resource(text);
        REQUIRE(sections.ok());
        auto binary =
            godot::write_binary_resource(sections.value(), {.path = "res://ruins.meshlib"});
        REQUIRE(binary.ok());
        write(folder / "ruins.meshlib", binary.value());
    }
    write(folder / "level.tscn", R"([gd_scene load_steps=12 format=3]

[ext_resource type="MeshLibrary" path="res://blocks.tres" id="1_blocks"]
[ext_resource type="MeshLibrary" path="res://ruins.meshlib" id="2_ruins"]
[ext_resource type="NavigationMesh" path="res://nav/square.tres" id="3_square"]

[sub_resource type="StandardMaterial3D" id="brick"]
albedo_color = Color(0.7, 0.3, 0.2, 1)

[sub_resource type="StandardMaterial3D" id="gold"]
metallic = 1.0

[sub_resource type="PrismMesh" id="roof"]
material = SubResource("gold")
size = Vector3(4, 1, 4)

[sub_resource type="PlaneMesh" id="rug"]

[sub_resource type="Curve3D" id="track"]
bake_interval = 0.5
_data = {
"points": PackedVector3Array(0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, -2, 0, 0, 0, 10, 0, 0),
"tilts": PackedFloat32Array(0, 0.5)
}
point_count = 2

[sub_resource type="PhysicsMaterial" id="grip"]
friction = 0.5
bounce = 0.25

[sub_resource type="NavigationMesh" id="yard"]
vertices = PackedVector3Array(-8, 0, -8, 8, 0, -8, 8, 0, 8, -8, 0, 8)
polygons = [PackedInt32Array(3, 2, 1, 0)]
geometry_parsed_geometry_type = 1
geometry_source_geometry_mode = 1
sample_partition_type = 2
filter_baking_aabb = AABB(-10, -1, -10, 20, 5, 20)
agent_max_slope = 30.0
filter_ledge_spans = true
border_size = 1.5

[node name="Level" type="Node3D"]

[node name="House" type="CSGCombiner3D" parent="."]
use_collision = true
snap = 0.001

[node name="Walls" type="CSGBox3D" parent="House"]
size = Vector3(4, 3, 4)
material = SubResource("brick")

[node name="Door" type="CSGBox3D" parent="House"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.5, 2)
operation = 2
size = Vector3(1, 2, 1)

[node name="Chimney" type="CSGCylinder3D" parent="House"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 1)
radius = 0.3
sides = 12
cone = true

[node name="Dome" type="CSGSphere3D" parent="House"]
radius = 1.0
rings = 8
material_override = SubResource("gold")

[node name="Ring" type="CSGTorus3D" parent="House"]
inner_radius = 0.2
outer_radius = 0.5

[node name="Roof" type="CSGMesh3D" parent="House"]
mesh = SubResource("roof")

[node name="Rug" type="CSGMesh3D" parent="House"]
mesh = SubResource("rug")

[node name="Track" type="Path3D" parent="."]
curve = SubResource("track")

[node name="Cart" type="PathFollow3D" parent="Track"]
progress = 2.0
rotation_mode = 4
loop = false

[node name="Rail" type="CSGPolygon3D" parent="."]
polygon = PackedVector2Array(0, 0, 0, 0.2, 0.2, 0.2, 0.2, 0)
mode = 2
path_node = NodePath("../Track")
path_interval_type = 1
path_rotation = 1

[node name="Blocks" type="GridMap" parent="."]
mesh_library = ExtResource("1_blocks")
physics_material = SubResource("grip")
cell_size = Vector3(2, 1, 2)
data = {
"cells": PackedInt32Array(0, 0, 0, 1, 0, 0, 2, 0, 1048577, 65535, 1, 2, 65536, 0, 3, -65536, 0, 0, 0, 65534, 0, 5, 5, 7)
}

[node name="Ruins" type="GridMap" parent="."]
mesh_library = ExtResource("2_ruins")
data = {
"cells": PackedInt32Array(3, 3, 5)
}

[node name="Post" type="StaticBody3D" parent="."]

[node name="Gate" type="RigidBody3D" parent="."]

[node name="Hinge" type="HingeJoint3D" parent="."]
node_a = NodePath("../Post")
node_b = NodePath("../Gate")
solver_priority = 2
angular_limit/enable = true
angular_limit/upper = 1.2
angular_limit/softness = 0.5
motor/enable = true
motor/target_velocity = 2.0

[node name="Hitch" type="Generic6DOFJoint3D" parent="."]
node_a = NodePath("../Gate")
linear_limit_x/upper_distance = 0.5
linear_limit_x/softness = 0.3
angular_spring_y/enabled = true
angular_spring_y/stiffness = 20.0
angular_motor_z/enabled = true
angular_motor_z/target_velocity = 1.5

[node name="Slide" type="SliderJoint3D" parent="."]
linear_limit/upper_distance = 2.0
linear_motion/softness = 0.5

[node name="Shoulder" type="ConeTwistJoint3D" parent="."]
swing_span = 0.5
twist_span = 1.0
bias = 0.4

[node name="Buggy" type="VehicleBody3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 10)
engine_force = 50.0

[node name="Wheel" type="VehicleWheel3D" parent="Buggy"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1.5)
use_as_steering = true
wheel_radius = 0.4

[node name="Yard" type="NavigationRegion3D" parent="."]
navigation_mesh = SubResource("yard")
travel_cost = 2.0

[node name="Square" type="NavigationRegion3D" parent="."]
navigation_mesh = ExtResource("3_square")

[node name="Walker" type="CharacterBody3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 3, 0, 3)

[node name="Guide" type="NavigationAgent3D" parent="Walker"]
target_position = Vector3(5, 0, 5)
max_speed = 4.0
avoidance_enabled = true
debug_enabled = true

[node name="Pillar" type="NavigationObstacle3D" parent="."]
vertices = PackedVector3Array(0, 0, 0, 1, 0, 0, 1, 0, 1)
radius = 0.5

[node name="Ladder" type="NavigationLink3D" parent="."]
start_position = Vector3(0, 0, 0)
end_position = Vector3(0, 3, 1)
bidirectional = false
)");
    write(folder / "map.tscn", R"([gd_scene load_steps=3 format=3]

[sub_resource type="Curve2D" id="road"]
_data = {
"points": PackedVector2Array(0, 0, 0, 0, 0, 0, -20, 0, 20, 0, 100, 50)
}
point_count = 2

[sub_resource type="NavigationPolygon" id="ground"]
vertices = PackedVector2Array(0, 0, 100, 0, 100, 100, 0, 100)
polygons = [PackedInt32Array(0, 1, 2, 3)]
outlines = [PackedVector2Array(0, 0, 100, 0, 100, 100, 0, 100)]
parsed_geometry_type = 1
source_geometry_mode = 2
baking_rect = Rect2(0, 0, 200, 200)
agent_radius = 8.0

[node name="Map" type="Node2D"]

[node name="Road" type="Path2D" parent="."]
curve = SubResource("road")

[node name="Car" type="PathFollow2D" parent="Road"]
progress = 30.0
rotates = false

[node name="Ground" type="NavigationRegion2D" parent="."]
navigation_polygon = SubResource("ground")

[node name="Bot" type="CharacterBody2D" parent="."]
position = Vector2(10, 10)

[node name="Brain" type="NavigationAgent2D" parent="Bot"]
path_desired_distance = 4.0
max_speed = 120.0

[node name="Rock" type="NavigationObstacle2D" parent="."]
vertices = PackedVector2Array(40, 40, 60, 40, 50, 60)
affect_navigation_mesh = true

[node name="Bridge" type="NavigationLink2D" parent="."]
end_position = Vector2(50, 0)
bidirectional = false
)");
    return folder;
}

TEST_CASE("importing a Godot level converts its CSG, paths, grid maps, joints and navigation") {
    const fs::path project = "import_godot_level";
    make_level_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    CHECK(*plan.report.find("converted")->find("mesh_libraries") == 2);
    CHECK(*plan.report.find("converted")->find("navigation_meshes") == 3);

    // CSG shapes keep their fields and materials; a CSGMesh3D's primitive mesh is the entity's,
    // colored by the mesh's own material.
    CHECK(*entity(files, level, "house").find("CSGCombiner3D") ==
          parse(R"({"use_collision": true})"));
    const json::Value walls = entity(files, level, "walls");
    CHECK(*walls.find("CSGBox3D") == parse(R"({"size": [4.0, 3.0, 4.0]})"));
    CHECK(*walls.find("StandardMaterial3D") == parse(R"({"albedo_color": [0.7, 0.3, 0.2, 1.0]})"));
    CHECK(*entity(files, level, "door").find("CSGBox3D") ==
          parse(R"({"operation": "subtraction", "size": [1.0, 2.0, 1.0]})"));
    CHECK(*entity(files, level, "chimney").find("CSGCylinder3D") ==
          parse(R"({"radius": 0.3, "sides": 12, "cone": true})"));
    CHECK(*entity(files, level, "dome").find("CSGSphere3D") ==
          parse(R"({"radius": 1.0, "rings": 8})"));
    CHECK(*entity(files, level, "ring").find("CSGTorus3D") ==
          parse(R"({"inner_radius": 0.2, "outer_radius": 0.5})"));
    const json::Value roof = entity(files, level, "roof");
    CHECK(roof.find("CSGMesh3D")->size() == 0);
    CHECK(*roof.find("PrismMesh") == parse(R"({"size": [4.0, 1.0, 4.0]})"));
    CHECK(*roof.find("StandardMaterial3D") == parse(R"({"metallic": 1.0})"));
    CHECK(entity(files, level, "rug").find("PlaneMesh") == nullptr);

    // Paths keep their points, the follower its settings, and the polygon pushed along one names
    // its entity.
    CHECK(*entity(files, level, "track").find("Path3D") == parse(R"({
        "bake_interval": 0.5,
        "points": [{"out": [0.0, 0.0, 2.0]},
                   {"position": [10.0, 0.0, 0.0], "in": [0.0, 0.0, -2.0], "tilt": 0.5}]})"));
    CHECK(*entity(files, level, "cart").find("PathFollow3D") ==
          parse(R"({"progress": 2.0, "rotation_mode": "oriented", "loop": false})"));
    CHECK(*entity(files, level, "rail").find("CSGPolygon3D") == parse(R"({
        "polygon": [[0.0, 0.0], [0.0, 0.2], [0.2, 0.2], [0.2, 0.0]], "mode": "path",
        "path_node": "track", "path_interval_type": "subdivide", "path_rotation": "path"})"));

    // Grid maps: Godot's cells, x and y in one number and z in the next, become floors of keys
    // for the library's items, turned items keep their orientation, and the physics material
    // gives friction and bounce.
    CHECK(*entity(files, level, "blocks").find("GridMap") == parse(R"({
        "mesh_library": "godot/blocks.meshlib.json", "cell_size": [2.0, 1.0, 2.0],
        "friction": 0.5, "bounce": 0.25,
        "legend": {"F": "Floor", "R": "Ramp_steep:16", "a": "Floor_2", "i": "item_3"},
        "floors": [{"y": -1, "rows": ["F"]},
                   {"origin": [-1, -2], "rows": [".F", "", ".FFR", "a"]},
                   {"y": 1, "rows": ["i"]}]})"));
    CHECK(parse(files.at("godot/blocks.meshlib.json")) == parse(R"({"format": 1, "items": {
        "Floor": {
            "primitive": {"BoxMesh": {"size": [2.0, 2.0, 2.0]}},
            "material": {"albedo_color": [0.5, 0.5, 0.45, 1.0]},
            "mesh_transform": {"position": [0.0, -0.5, 0.0]}, "mesh_cast_shadow": "off",
            "shapes": [{"shape": "box", "size": [2.0, 2.0, 2.0], "position": [0.0, -0.5, 0.0]}],
            "navigation_mesh": "godot/nav/square.navmesh.json",
            "navigation_mesh_transform": {"position": [0.0, 0.5, 0.0]}, "navigation_layers": 3},
        "Ramp_steep": {"primitive": {"PrismMesh": {"size": [2.0, 2.0, 2.0]}}},
        "Floor_2": {"mesh": "godot/rock.obj", "shapes": [{"shape": "box", "size": [2.0, 2.0, 2.0]}]},
        "item_3": {"primitive": {"BoxMesh": {"size": [2.0, 2.0, 2.0]}},
                   "material": {"albedo_color": [0.5, 0.5, 0.45, 1.0]}}}})"));
    // A library saved in Godot's binary form converts the same way.
    CHECK(*entity(files, level, "ruins").find("GridMap") == parse(R"({
        "mesh_library": "godot/ruins.meshlib.json", "legend": {"P": "Pillar"},
        "floors": [{"origin": [3, 3], "rows": ["P"]}]})"));
    CHECK(parse(files.at("godot/ruins.meshlib.json")) == parse(R"({"format": 1, "items": {
        "Pillar": {"primitive": {"CylinderMesh": {
            "top_radius": 0.4, "bottom_radius": 0.4, "height": 4.0}}}}})"));

    // Joints take Godot's parameters under SlashEngine's names; vehicles weigh Godot's 40 kg.
    CHECK(*entity(files, level, "hinge").find("HingeJoint3D") == parse(R"({
        "node_a": "post", "node_b": "gate", "angular_limit_enabled": true,
        "angular_limit_upper": 1.2, "motor_enabled": true, "motor_target_velocity": 2.0})"));
    CHECK(*entity(files, level, "hitch").find("Generic6DOFJoint3D") == parse(R"({
        "node_a": "gate", "linear_x": {"limit_upper": 0.5},
        "angular_y": {"spring_enabled": true, "spring_stiffness": 20.0},
        "angular_z": {"motor_enabled": true, "motor_target_velocity": 1.5}})"));
    CHECK(*entity(files, level, "slide").find("SliderJoint3D") ==
          parse(R"({"linear_limit_upper": 2.0})"));
    CHECK(*entity(files, level, "shoulder").find("ConeTwistJoint3D") ==
          parse(R"({"swing_span": 0.5, "twist_span": 1.0})"));
    const json::Value buggy = entity(files, level, "buggy");
    CHECK(*buggy.find("RigidBody3D") == parse(R"({"mass": 40.0})"));
    CHECK(*buggy.find("VehicleBody3D") == parse(R"({"engine_force": 50.0})"));
    CHECK(*entity(files, level, "wheel").find("VehicleWheel3D") ==
          parse(R"({"use_as_steering": true, "wheel_radius": 0.4})"));

    // Navigation: regions name the files their meshes become, and an agent is an entity at its
    // parent's origin.
    CHECK(*entity(files, level, "yard").find("NavigationRegion3D") == parse(R"({
        "navigation_mesh": "godot/level_yard.navmesh.json", "travel_cost": 2.0})"));
    CHECK(parse(files.at("godot/level_yard.navmesh.json")) == parse(R"({
        "format": 1, "vertices": [[-8.0, 0.0, -8.0], [8.0, 0.0, -8.0], [8.0, 0.0, 8.0],
                                  [-8.0, 0.0, 8.0]],
        "polygons": [[3, 2, 1, 0]], "geometry_parsed_geometry_type": "static_colliders",
        "sample_partition_type": "layers", "agent_max_slope": 30.0, "border_size": 1.5,
        "filter_ledge_spans": true,
        "filter_baking_aabb": {"position": [-10.0, -1.0, -10.0], "size": [20.0, 5.0, 20.0]}})"));
    // Godot's navigation meshes bake only meshes unless told otherwise.
    CHECK(*entity(files, level, "square").find("NavigationRegion3D") ==
          parse(R"({"navigation_mesh": "godot/nav/square.navmesh.json"})"));
    CHECK(parse(files.at("godot/nav/square.navmesh.json")) == parse(R"({
        "format": 1, "vertices": [[-1.0, 0.0, -1.0], [1.0, 0.0, -1.0], [1.0, 0.0, 1.0],
                                  [-1.0, 0.0, 1.0]],
        "polygons": [[3, 2, 1, 0]], "cell_size": 0.1, "agent_radius": 0.25,
        "geometry_parsed_geometry_type": "mesh_instances"})"));
    CHECK(*entity(files, level, "walker").find("Transform3D") ==
          parse(R"({"position": [3.0, 0.0, 3.0], "children": ["guide"]})"));
    CHECK(*entity(files, level, "guide").find("NavigationAgent3D") == parse(R"({
        "target_position": [5.0, 0.0, 5.0], "max_speed": 4.0, "avoidance_enabled": true})"));
    CHECK(*entity(files, level, "pillar").find("NavigationObstacle3D") == parse(R"({
        "vertices": [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 0.0, 1.0]], "radius": 0.5})"));
    CHECK(*entity(files, level, "ladder").find("NavigationLink3D") ==
          parse(R"({"end_position": [0.0, 3.0, 1.0], "bidirectional": false})"));

    // The same in 2D.
    const std::string map = "godot/map.scene.json";
    CHECK(*entity(files, map, "road").find("Path2D") == parse(R"({"points": [
        {}, {"position": [100.0, 50.0], "in": [-20.0, 0.0], "out": [20.0, 0.0]}]})"));
    CHECK(*entity(files, map, "car").find("PathFollow2D") ==
          parse(R"({"progress": 30.0, "rotates": false})"));
    CHECK(*entity(files, map, "ground").find("NavigationRegion2D") ==
          parse(R"({"navigation_polygon": "godot/map_ground.navpoly.json"})"));
    CHECK(parse(files.at("godot/map_ground.navpoly.json")) == parse(R"({
        "format": 1, "vertices": [[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [0.0, 100.0]],
        "polygons": [[0, 1, 2, 3]],
        "outlines": [[[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [0.0, 100.0]]],
        "parsed_geometry_type": "static_colliders", "agent_radius": 8.0,
        "baking_rect": {"position": [0.0, 0.0], "size": [200.0, 200.0]}})"));
    CHECK(*entity(files, map, "brain").find("NavigationAgent2D") ==
          parse(R"({"path_desired_distance": 4.0, "max_speed": 120.0})"));
    CHECK(*entity(files, map, "rock").find("NavigationObstacle2D") == parse(R"({
        "vertices": [[40.0, 40.0], [60.0, 40.0], [50.0, 60.0]], "affect_navigation_mesh": true})"));
    CHECK(*entity(files, map, "bridge").find("NavigationLink2D") ==
          parse(R"({"end_position": [50.0, 0.0], "bidirectional": false})"));

    const json::Value& report = plan.report;
    const auto why = [&](std::string_view what) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        return item.find("why")->as_string();
    };
    CHECK(why("CSGMesh3D.mesh") == "a PlaneMesh encloses nothing, so it can't be a CSG shape");
    CHECK(why("CSGSphere3D.material_override") ==
          "it colors the whole result; give each shape its material instead");
    CHECK(unsupported(report, "CSGCombiner3D.snap").is_null());
    CHECK(why("GridMap cells") == "item 7 isn't in the mesh library");
    CHECK(why("MeshLibrary item shape scale") ==
          "shapes are placed by position and rotation; set the shape's size instead");
    for (const char* ignored :
         {"HingeJoint3D.solver_priority", "HingeJoint3D.angular_limit/softness",
          "Generic6DOFJoint3D.linear_limit_x/softness", "SliderJoint3D.linear_motion/softness",
          "ConeTwistJoint3D.bias"}) {
        CHECK(why(ignored) == "Godot's Jolt physics, which SlashEngine follows, ignores it");
    }
    CHECK(why("NavigationMesh.geometry_source_geometry_mode") ==
          "a bake takes the region's entity and those under it");
    CHECK(why("NavigationPolygon.source_geometry_mode") ==
          "a bake takes the region's entity and those under it");
    CHECK(unsupported(report, "NavigationAgent3D.debug_enabled").is_null());
    CHECK(unsupported(report, "invalid value").is_null());
    fs::remove_all(project);
}

TEST_CASE("importing Godot's top_level and z_as_relative keeps them") {
    const fs::path project = "import_godot_top_level";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Flares\"\n");
    write(project / "godot/level.tscn", R"([gd_scene format=3]

[node name="Level" type="Node2D"]

[node name="Ship" type="Sprite2D" parent="."]
z_index = 2

[node name="Flare" type="Sprite2D" parent="Ship"]
top_level = true
position = Vector2(40, 10)

[node name="Wake" type="Sprite2D" parent="Ship"]
z_index = -1
z_as_relative = false

[node name="Hud" type="Label" parent="."]
z_as_relative = false

[node name="Tip" type="Label" parent="Hud"]
top_level = true
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string level = "godot/level.scene.json";
    REQUIRE(files.contains(level));
    CHECK(*entity(files, level, "flare").find("Transform2D") ==
          parse(R"({"position": [40.0, 10.0], "top_level": true})"));
    CHECK(*entity(files, level, "wake").find("Sprite2D") ==
          parse(R"({"z_index": -1, "z_as_relative": false})"));
    CHECK(*entity(files, level, "hud").find("Control") ==
          parse(R"({"children": ["tip"], "z_as_relative": false})"));
    CHECK(*entity(files, level, "tip").find("Control") == parse(R"({"top_level": true})"));
    for (const json::Value& item : plan.report.find("unsupported")->as_array()) {
        const std::string& what = item.find("what")->as_string();
        CHECK_MESSAGE(what.find("top_level") == std::string::npos, what);
        CHECK_MESSAGE(what.find("z_as_relative") == std::string::npos, what);
    }
    fs::remove_all(project);
}

TEST_CASE("importing a Godot CodeEdit keeps its text and gutters") {
    const fs::path project = "import_godot_code_edit";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Editor\"\n");
    write(project / "godot/editor.tscn", R"([gd_scene format=3]

[node name="Editor" type="CodeEdit"]
offset_right = 400.0
offset_bottom = 300.0
text = "func _ready():\n\tpass"
line_length_guidelines = PackedInt32Array(80, 100)
gutters_draw_breakpoints_gutter = true
gutters_draw_line_numbers = true
gutters_zero_pad_line_numbers = true
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string scene = "godot/editor.scene.json";
    REQUIRE(files.contains(scene));
    CHECK(*entity(files, scene, "editor").find("TextEdit") ==
          parse(R"({"text": "func _ready():\n\tpass"})"));
    CHECK(*entity(files, scene, "editor").find("CodeEdit") ==
          parse(R"({"gutters_draw_breakpoints_gutter": true, "gutters_draw_line_numbers": true,
                    "gutters_zero_pad_line_numbers": true, "line_length_guidelines": [80, 100]})"));
    for (const json::Value& item : plan.report.find("unsupported")->as_array()) {
        CHECK_MESSAGE(item.find("what")->as_string().find("CodeEdit") == std::string::npos,
                      item.find("what")->as_string());
    }
    fs::remove_all(project);
}

TEST_CASE("importing Godot's GPUParticles2D writes its process material") {
    const fs::path project = "import_godot_gpu_particles";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Fire\"\n");
    write_png(project / "godot/spark.png", 32, 16);
    // A ParticleProcessMaterial in a file of its own, made for 3D: Godot gives it 2D's gravity.
    write(project / "godot/smoke.tres", R"([gd_resource type="ParticleProcessMaterial" format=3]

[resource]
spread = 180.0
initial_velocity_max = 10.0
)");
    write(project / "godot/level.tscn", R"([gd_scene load_steps=8 format=3]

[ext_resource type="Texture2D" path="res://spark.png" id="1_spark"]
[ext_resource type="ParticleProcessMaterial" path="res://smoke.tres" id="2_smoke"]

[sub_resource type="Curve" id="Curve_1"]
_data = [Vector2(0, 1), 0.0, 0.0, 0, 0, Vector2(1, 0), 0.0, 0.0, 0, 0]
point_count = 2

[sub_resource type="CurveTexture" id="CurveTexture_1"]
curve = SubResource("Curve_1")

[sub_resource type="Gradient" id="Gradient_1"]
offsets = PackedFloat32Array(0, 1)
colors = PackedColorArray(1, 0.8, 0.2, 1, 1, 0.2, 0, 0)

[sub_resource type="GradientTexture1D" id="GradientTexture1D_1"]
gradient = SubResource("Gradient_1")

[sub_resource type="ParticleProcessMaterial" id="ParticleProcessMaterial_1"]
particle_flag_disable_z = true
emission_shape = 1
emission_sphere_radius = 6.0
direction = Vector3(0, -1, 0)
spread = 20.0
initial_velocity_min = 60.0
initial_velocity_max = 90.0
gravity = Vector3(0, 98, 0)
scale_curve = SubResource("CurveTexture_1")
color_ramp = SubResource("GradientTexture1D_1")
turbulence_enabled = true
turbulence_noise_strength = 2.5
turbulence_noise_speed = Vector3(0.5, 0, 0.25)
turbulence_influence_min = 0.2
turbulence_influence_max = 0.4
turbulence_influence_over_life = SubResource("CurveTexture_1")
turbulence_initial_displacement_max = 5.0
collision_mode = 1
collision_bounce = 0.5
collision_friction = 1.5
collision_use_scale = true
sub_emitter_mode = 2
sub_emitter_frequency = 6.0
sub_emitter_amount_at_end = 5
sub_emitter_keep_velocity = true

[sub_resource type="CanvasItemMaterial" id="CanvasItemMaterial_1"]
particles_animation = true
particles_anim_h_frames = 4
particles_anim_v_frames = 2
particles_anim_loop = true

[node name="Level" type="Node2D"]

[node name="Fire" type="GPUParticles2D" parent="."]
material = SubResource("CanvasItemMaterial_1")
amount = 32
process_material = SubResource("ParticleProcessMaterial_1")
texture = ExtResource("1_spark")
lifetime = 0.8
use_fixed_seed = true
seed = 4000000000
draw_order = 0
collision_base_size = 3.0
visibility_rect = Rect2(-50, -50, 100, 100)
sub_emitter = NodePath("../Smoke")

[node name="Smoke" type="GPUParticles2D" parent="."]
process_material = ExtResource("2_smoke")
seed = 12
trail_enabled = true
trail_lifetime = 0.5
trail_sections = 12
trail_section_subdivisions = 2
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    CHECK(plan.report.find("converted")->find("process_materials")->as_int() == 2);

    // The scene's own material: its 2D vectors, curve and gradient.
    const std::string fire_material =
        "godot/level_particle_process_material_1.process_material.json";
    REQUIRE(files.contains(fire_material));
    CHECK(slash::canvas::validate_process_material(parse(files.at(fire_material))).empty());
    CHECK(parse(files.at(fire_material)) == parse(R"({
        "format": 1,
        "emission_shape": "sphere",
        "emission_sphere_radius": 6.0,
        "direction": [0.0, -1.0],
        "spread": 20.0,
        "initial_velocity_min": 60.0,
        "initial_velocity_max": 90.0,
        "gravity": [0.0, 98.0],
        "scale_curve": [[0.0, 1.0], [1.0, 0.0]],
        "color_ramp": [{"offset": 0.0, "color": [1.0, 0.8, 0.2, 1.0]},
                       {"offset": 1.0, "color": [1.0, 0.2, 0.0, 0.0]}],
        "turbulence_enabled": true,
        "turbulence_noise_strength": 2.5,
        "turbulence_noise_speed": [0.5, 0.0, 0.25],
        "turbulence_influence_min": 0.2,
        "turbulence_influence_max": 0.4,
        "turbulence_influence_over_life": [[0.0, 1.0], [1.0, 0.0]],
        "turbulence_initial_displacement_max": 5.0,
        "collision_mode": "rigid",
        "collision_bounce": 0.5,
        "collision_friction": 1.0,
        "collision_use_scale": true,
        "sub_emitter_mode": "at_end",
        "sub_emitter_frequency": 6.0,
        "sub_emitter_amount_at_end": 5,
        "sub_emitter_keep_velocity": true})"));
    CHECK(unsupported(plan.report, "ParticleProcessMaterial.collision_mode").is_null());
    CHECK(unsupported(plan.report, "ParticleProcessMaterial.sub_emitter_mode").is_null());
    CHECK(parse(files.at("godot/smoke.process_material.json")) ==
          parse(R"({"format": 1, "spread": 180.0, "initial_velocity_max": 10.0,
                    "gravity": [0.0, 98.0]})"));

    // The emitters: the material's animation frames, and the seed only where it is fixed.
    const std::string level = "godot/level.scene.json";
    CHECK(*entity(files, level, "fire").find("GPUParticles2D") == parse(R"({
        "amount": 32, "process_material": "godot/level_particle_process_material_1.process_material.json",
        "texture": "godot/spark.png", "lifetime": 0.8, "use_fixed_seed": true,
        "seed": -294967296, "draw_order": "index", "collision_base_size": 3.0, "hframes": 4,
        "vframes": 2, "anim_loop": true, "sub_emitter": "smoke"})"));
    CHECK(*entity(files, level, "smoke").find("GPUParticles2D") ==
          parse(R"({"process_material": "godot/smoke.process_material.json",
                    "trail_enabled": true, "trail_lifetime": 0.5, "trail_sections": 12,
                    "trail_section_subdivisions": 2})"));
    CHECK(unsupported(plan.report, "GPUParticles2D.trail_enabled").is_null());
    CHECK(unsupported(plan.report, "resource ParticleProcessMaterial").is_null());
    fs::remove_all(project);
}

TEST_CASE("importing Godot's music resources and sound import settings writes .audio.json files") {
    const fs::path project = "import_godot_music";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Score\"\n");
    // An Ogg Vorbis file whose import settings give it beats and a loop (its bytes don't matter
    // here), and one imported as it is.
    write(project / "godot/music/explore.ogg", "OggS");
    write(project / "godot/music/explore.ogg.import", R"([remap]

importer="oggvorbisstr"
type="AudioStreamOggVorbis"
uid="uid://explore"

[params]

loop=true
loop_offset=0
bpm=120.0
beat_count=8
bar_beats=4
)");
    write(project / "godot/music/combat.ogg", "OggS");
    // A WAV file looping from its own "smpl" chunk, as Godot detects by default, and one whose
    // import settings loop it from frame 10 to the last frame.
    const std::vector<slash::audio::Frame> frames(1000, slash::audio::Frame{0.25F, 0.25F});
    std::string hit = slash::audio::encode_wav(frames, 44100);
    std::string smpl = "smpl";
    const auto word = [](std::string& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
        }
    };
    word(smpl, 60);
    for (const std::uint32_t value : {0U, 0U, 0U, 60U, 0U, 0U, 0U, 1U, 0U}) {
        word(smpl, value); // the chunk's header, one loop
    }
    for (const std::uint32_t value : {0U, 1U, 100U, 400U, 0U, 0U}) {
        word(smpl, value); // cue point, ping-pong, start, end, fraction, play count
    }
    hit += smpl;
    std::string riff_size;
    word(riff_size, static_cast<std::uint32_t>(hit.size() - 8));
    hit.replace(4, 4, riff_size);
    write(project / "godot/sfx/hit.wav", hit);
    write(project / "godot/sfx/step.wav", slash::audio::encode_wav(frames, 44100));
    write(project / "godot/sfx/step.wav.import", R"([remap]

importer="wav"
type="AudioStreamWAV"

[params]

edit/trim=true
edit/loop_mode=2
edit/loop_begin=10
edit/loop_end=-1
)");
    write(project / "godot/sfx/steps.tres",
          R"([gd_resource type="AudioStreamRandomizer" load_steps=3 format=3]

[ext_resource type="AudioStream" path="res://sfx/step.wav" id="1_step"]
[ext_resource type="AudioStream" path="res://sfx/hit.wav" id="2_hit"]

[resource]
playback_mode = 2
random_pitch = 1.2
streams_count = 2
stream_0/stream = ExtResource("1_step")
stream_1/stream = ExtResource("2_hit")
stream_1/weight = 2.0
)");
    // Interactive music whose combat clip is layers, and whose two last clips share a name.
    write(project / "godot/music/score.tres",
          R"([gd_resource type="AudioStreamInteractive" load_steps=5 format=3]

[ext_resource type="AudioStream" path="res://music/explore.ogg" id="1_explore"]
[ext_resource type="AudioStream" path="res://music/combat.ogg" id="2_combat"]

[sub_resource type="AudioStreamSynchronized" id="AudioStreamSynchronized_1"]
stream_count = 2
stream_0/stream = ExtResource("2_combat")
stream_1/stream = ExtResource("1_explore")
stream_1/volume = -80.0

[sub_resource type="AudioStreamPlaylist" id="AudioStreamPlaylist_1"]
shuffle = true
loop = false
fade_time = 0.5
stream_count = 2
stream_0 = ExtResource("1_explore")
stream_1 = ExtResource("2_combat")

[resource]
clip_count = 4
clip_0/name = &"explore"
clip_0/stream = ExtResource("1_explore")
clip_0/auto_advance = 0
clip_1/name = &"combat"
clip_1/stream = SubResource("AudioStreamSynchronized_1")
clip_1/auto_advance = 0
clip_2/name = &"sting"
clip_2/stream = SubResource("AudioStreamPlaylist_1")
clip_2/auto_advance = 1
clip_2/next_clip = 0
clip_3/name = &"sting"
clip_3/stream = ExtResource("1_explore")
clip_3/auto_advance = 2
initial_clip = 1
_transitions = {
Vector2i(-1, -1): {
"fade_beats": 1.0,
"fade_mode": 4,
"from_time": 1,
"to_time": 1
},
Vector2i(0, 1): {
"fade_beats": 2.0,
"fade_mode": 3,
"filler_clip": 2,
"from_time": 2,
"hold_previous": true,
"to_time": 0,
"use_filler_clip": true
}
}
)");
    write(project / "godot/main.tscn", R"([gd_scene load_steps=5 format=3]

[ext_resource type="AudioStreamInteractive" path="res://music/score.tres" id="1_score"]
[ext_resource type="AudioStreamRandomizer" path="res://sfx/steps.tres" id="2_steps"]

[sub_resource type="AudioStreamGenerator" id="AudioStreamGenerator_1"]
mix_rate_mode = 0
buffer_length = 0.1

[sub_resource type="AudioStreamPolyphonic" id="AudioStreamPolyphonic_1"]
polyphony = 8

[sub_resource type="AudioStreamMicrophone" id="AudioStreamMicrophone_1"]

[node name="Main" type="Node2D"]

[node name="Music" type="AudioStreamPlayer" parent="."]
stream = ExtResource("1_score")
autoplay = true

[node name="Steps" type="AudioStreamPlayer2D" parent="."]
stream = ExtResource("2_steps")

[node name="Synth" type="AudioStreamPlayer" parent="."]
stream = SubResource("AudioStreamGenerator_1")

[node name="Sfx" type="AudioStreamPlayer" parent="."]
stream = SubResource("AudioStreamPolyphonic_1")

[node name="Voice" type="AudioStreamPlayer" parent="."]
stream = SubResource("AudioStreamMicrophone_1")
autoplay = true
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const auto valid = [&](const std::string& path) {
        REQUIRE_MESSAGE(files.contains(path), path);
        const json::Value doc = parse(files.at(path));
        CHECK_MESSAGE(slash::audio::validate_stream_file(doc).empty(), path);
        return doc;
    };

    // Sounds whose import settings matter get a .audio.json file beside them.
    CHECK(valid("godot/music/explore.ogg.audio.json") ==
          parse(R"({"format": 1, "source": "godot/music/explore.ogg", "loop": true,
                    "bpm": 120.0, "beat_count": 8})"));
    CHECK_FALSE(files.contains("godot/music/combat.ogg.audio.json"));
    CHECK(valid("godot/sfx/hit.wav.audio.json") ==
          parse(R"({"format": 1, "source": "godot/sfx/hit.wav", "loop_mode": "pingpong",
                    "loop_begin": 100, "loop_end": 400})"));
    CHECK(valid("godot/sfx/step.wav.audio.json") ==
          parse(R"({"format": 1, "source": "godot/sfx/step.wav", "loop_mode": "forward",
                    "loop_begin": 10, "loop_end": 999})"));
    CHECK(unsupported(plan.report, "WAV import option edit/trim").find("why")->as_string() ==
          "sounds play as they are in the file");

    // The randomizer, and the music with its layers and playlist, each as Godot has it.
    CHECK(valid("godot/sfx/steps.audio.json") ==
          parse(R"({"format": 1, "playback_mode": "sequential", "random_pitch": 1.2,
                    "streams": [{"stream": "godot/sfx/step.wav.audio.json"},
                                {"stream": "godot/sfx/hit.wav.audio.json", "weight": 2.0}]})"));
    CHECK(valid("godot/music/score_audio_stream_synchronized_1.audio.json") ==
          parse(R"({"format": 1, "synchronized": [
                    {"stream": "godot/music/combat.ogg"},
                    {"stream": "godot/music/explore.ogg.audio.json", "volume_db": -80.0}]})"));
    CHECK(valid("godot/music/score_audio_stream_playlist_1.audio.json") ==
          parse(R"({"format": 1, "shuffle": true, "loop": false, "fade_time": 0.5,
                    "playlist": [{"stream": "godot/music/explore.ogg.audio.json"},
                                 {"stream": "godot/music/combat.ogg"}]})"));
    CHECK(valid("godot/music/score.audio.json") == parse(R"({
        "format": 1,
        "clips": [
            {"name": "explore", "stream": "godot/music/explore.ogg.audio.json"},
            {"name": "combat",
             "stream": "godot/music/score_audio_stream_synchronized_1.audio.json"},
            {"name": "sting", "stream": "godot/music/score_audio_stream_playlist_1.audio.json",
             "auto_advance": "enabled", "next_clip": "explore"},
            {"name": "clip_3", "stream": "godot/music/explore.ogg.audio.json",
             "auto_advance": "return_to_hold"}],
        "initial_clip": "combat",
        "transitions": [
            {},
            {"from": "explore", "to": "combat", "from_time": "next_bar",
             "to_time": "same_position", "fade_mode": "cross", "fade_beats": 2.0,
             "filler_clip": "sting", "hold_previous": true}]})"));
    CHECK(unsupported(plan.report, "AudioStreamInteractive clip 3").find("why")->as_string() ==
          R"(another clip is named "sting"; it is named "clip_3")");
    // A generator and a polyphonic stream, which a game's code plays frames and sounds on, and
    // the microphone.
    CHECK(valid("godot/main_audio_stream_generator_1.audio.json") ==
          parse(R"({"format": 1, "generator": {"mix_rate_mode": "output",
                                               "buffer_length": 0.1}})"));
    CHECK(valid("godot/main_audio_stream_polyphonic_1.audio.json") ==
          parse(R"({"format": 1, "polyphonic": {"polyphony": 8}})"));
    CHECK(valid("godot/main_audio_stream_microphone_1.audio.json") ==
          parse(R"({"format": 1, "microphone": {}})"));
    CHECK(plan.report.find("converted")->find("audio_streams")->as_int() == 10);
    CHECK(unsupported(plan.report, "resource AudioStreamInteractive").is_null());

    // The players play them.
    const std::string main = "godot/main.scene.json";
    CHECK(*entity(files, main, "music").find("AudioStreamPlayer") ==
          parse(R"({"stream": "godot/music/score.audio.json", "autoplay": true})"));
    CHECK(*entity(files, main, "steps").find("AudioStreamPlayer2D") ==
          parse(R"({"stream": "godot/sfx/steps.audio.json"})"));
    CHECK(*entity(files, main, "synth").find("AudioStreamPlayer") ==
          parse(R"({"stream": "godot/main_audio_stream_generator_1.audio.json"})"));
    CHECK(*entity(files, main, "voice").find("AudioStreamPlayer") ==
          parse(R"({"stream": "godot/main_audio_stream_microphone_1.audio.json",
                    "autoplay": true})"));
    fs::remove_all(project);
}

TEST_CASE("importing a Godot AnimationPlayer writes its libraries and targets the nodes they "
          "animate") {
    const fs::path project = "import_godot_animation";
    fs::remove_all(project);
    // No main scene: the first scene becomes it.
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Hero\"\n");
    write(project / "godot/hero.tscn", kHero);
    write(project / "godot/anim/extra.tres", kExtraAnimations);
    write(project / "godot/house.tscn", R"([gd_scene load_steps=2 format=3]

[ext_resource type="PackedScene" path="res://hero.tscn" id="1_hero"]

[node name="House" type="Node2D"]

[node name="Hero1" parent="." instance=ExtResource("1_hero")]

[node name="Hero2" parent="." instance=ExtResource("1_hero")]
position = Vector2(64, 0)

[node name="AnimationPlayer" parent="Hero2" index="1"]
autoplay = "extra/spin"
)");
    write_png(project / "godot/art/hero.png", 32, 16);
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    CHECK(plan.report.find("converted")->find("animation_libraries")->as_int() == 1);
    CHECK(parse(files.at("project.json")).find("main_scene")->as_string() ==
          "godot/hero.scene.json");
    CHECK(unsupported(plan.report, "main scene").find("why")->as_string() ==
          "it names none, so the first scene, hero.tscn, is the main scene");

    // The player's two libraries go in one file, the named one's animations under its name.
    const std::string library = "godot/hero_animation_player.anim.json";
    REQUIRE(files.contains(library));
    const json::Value animations = *parse(files.at(library)).find("animations");
    CHECK(animations.find("RESET") != nullptr);
    CHECK(*animations.find("extra/spin") == parse(R"({"length": 1, "loop": "pingpong",
        "tracks": [{"target": "Sprite2D", "path": "Transform2D/rotation",
                    "interpolation": "linear_angle", "loop_wrap": false,
                    "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 3.14}]}]})"));
    // Tracks name nodes from the root node, the player's parent. A method track becomes an
    // event track per method; a bezier track's keys play cubic; keys past the end go.
    CHECK(*animations.find("walk") == parse(R"({"length": 0.4, "loop": "linear", "tracks": [
        {"target": "Sprite2D", "path": "Sprite2D/frame", "update": "continuous",
         "keys": [{"time": 0, "value": 0}, {"time": 0.2, "value": 1}]},
        {"target": ".", "path": "Transform2D/position", "interpolation": "cubic",
         "keys": [{"time": 0, "value": [0, 0]},
                  {"time": 0.2, "transition": 0.5, "value": [0, -4]}]},
        {"target": ".", "event": "footstep",
         "keys": [{"time": 0, "data": {"args": ["left"]}}]},
        {"target": ".", "event": "blink", "keys": [{"time": 0.2}]},
        {"target": "Sprite2D", "path": "Sprite2D/modulate/a", "interpolation": "cubic",
         "keys": [{"time": 0, "value": 1}, {"time": 0.4, "value": 0.5}]},
        {"target": "Sprite2D", "path": "Sprite2D/texture",
         "keys": [{"time": 0, "value": "godot/art/hero.png"}]},
        {"target": "Sprite2D", "path": "Sprite2D/offset/y", "update": "discrete",
         "keys": [{"time": 0, "value": 0}, {"time": 0.2, "value": -2}]}]})"));
    CHECK(unsupported(plan.report, "animation audio track").find("why")->as_string() ==
          "play the sound from gameplay code, on an event track's event");
    CHECK(unsupported(plan.report, "animation bezier track") != nullptr);
    CHECK(unsupported(plan.report, "disabled animation track") != nullptr);
    CHECK(unsupported(plan.report, "animation track target").find("why")->as_string() ==
          "node path Missing names no node under the root node");
    CHECK(unsupported(plan.report, "node type AnimationPlayer") == nullptr);
    CHECK(unsupported(plan.report, "resource AnimationLibrary") == nullptr);

    const std::string hero = "godot/hero.scene.json";
    CHECK(entity(files, hero, "animation_player") == parse(R"({"AnimationPlayer": {
        "library": "godot/hero_animation_player.anim.json", "animation": "walk",
        "speed_scale": 2.0, "targets": {".": "hero", "Sprite2D": "sprite2d"}}})"));
    CHECK(slash::animation::check_players(parse(files.at(hero)), reader_of(files)).empty());

    // Instances keep the scene's library; their targets follow their IDs.
    const std::string house = "godot/house.scene.json";
    CHECK(entry(files, house, "hero2/animation_player") ==
          parse(R"({"components": {"AnimationPlayer": {"animation": "extra/spin"}}})"));
    CHECK(parse(files.at(house)).find("entities")->find("hero1/animation_player") == nullptr);
    CHECK(
        *entity(files, house, "hero2/animation_player").find("AnimationPlayer")->find("targets") ==
        parse(R"({".": "hero2", "Sprite2D": "hero2/sprite2d"})"));
}

// A Godot scene with four AnimationTrees: one playing a model's own animations through a blend
// tree saved as its own resource, one playing its own libraries through a state machine, and two
// whose anim_player names nothing they can play.
fs::path make_tree_project(const fs::path& folder) {
    fs::remove_all(folder);
    write(folder / "project.godot", R"(config_version=5

[application]

config/name="Trees"
run/main_scene="res://hero.tscn"
)");
    write(folder / "models/arm.glb", slash::test::arm().glb());
    write(folder / "trees/moves.tres",
          R"([gd_resource type="AnimationNodeBlendTree" load_steps=14 format=3]

[sub_resource type="AnimationNodeAnimation" id="lean_idle"]
animation = &"Idle"

[sub_resource type="AnimationNodeAnimation" id="lean_bend"]
animation = &"Bend"
play_mode = 1
use_custom_timeline = true
timeline_length = 2.0
loop_mode = 2

[sub_resource type="AnimationNodeBlendSpace1D" id="lean"]
blend_point_0/node = SubResource("lean_idle")
blend_point_0/pos = -1.0
blend_point_1/node = SubResource("lean_bend")
blend_point_1/pos = 1.0
min_space = -2.0
snap = 0.5
value_label = "lean"
blend_mode = 1

[sub_resource type="AnimationNodeAnimation" id="reach"]
animation = &"Bend"

[sub_resource type="AnimationNodeTimeSeek" id="seek"]
explicit_elapse = false

[sub_resource type="Curve" id="ease"]
_data = [Vector2(0, 0), 0.0, 0.0, 0, 0, Vector2(1, 1), 0.0, 0.0, 0, 0]
point_count = 2

[sub_resource type="AnimationNodeTransition" id="pick"]
xfade_time = 0.3
xfade_curve = SubResource("ease")
input_count = 2
input_0/name = "rest"
input_0/auto_advance = false
input_0/break_loop_at_end = false
input_0/reset = true
input_1/name = "move"
input_1/auto_advance = true
input_1/break_loop_at_end = false
input_1/reset = false

[sub_resource type="AnimationNodeAnimation" id="wave"]
animation = &"Idle"

[sub_resource type="AnimationNodeAnimation" id="aim_middle"]
animation = &"Idle"

[sub_resource type="AnimationNodeAnimation" id="aim_side"]
animation = &"Bend"

[sub_resource type="AnimationNodeAnimation" id="aim_up"]
animation = &"Missing"

[sub_resource type="AnimationNodeBlendSpace2D" id="aim"]
blend_point_0/node = SubResource("aim_middle")
blend_point_0/pos = Vector2(0, 0)
blend_point_1/node = SubResource("aim_side")
blend_point_1/pos = Vector2(1, 0)
blend_point_2/node = SubResource("aim_up")
blend_point_2/pos = Vector2(0, 1)
triangles = PackedInt32Array(0, 1, 2)
auto_triangles = false
min_space = Vector2(-1, -1)
snap = Vector2(0.1, 0.1)
x_label = "yaw"
y_label = "pitch"

[sub_resource type="AnimationNodeAnimation" id="spare"]
animation = &"Bend"

[sub_resource type="AnimationNodeSub2" id="sub"]

[sub_resource type="AnimationNodeAdd3" id="layer"]
filter_enabled = true
filters = ["Armature/Skeleton3D:Lower", "Armature/Skeleton3D:Elbow"]

[sub_resource type="AnimationNodeExtension" id="custom"]

[resource]
graph_offset = Vector2(0, 0)
nodes/aim/node = SubResource("aim")
nodes/aim/position = Vector2(200, 300)
nodes/custom/node = SubResource("custom")
nodes/custom/position = Vector2(0, 500)
nodes/layer/node = SubResource("layer")
nodes/layer/position = Vector2(600, 100)
nodes/lean/node = SubResource("lean")
nodes/lean/position = Vector2(200, 0)
nodes/output/position = Vector2(800, 100)
nodes/pick/node = SubResource("pick")
nodes/pick/position = Vector2(400, 100)
nodes/reach/node = SubResource("reach")
nodes/reach/position = Vector2(0, 200)
nodes/seek/node = SubResource("seek")
nodes/seek/position = Vector2(200, 200)
nodes/spare/node = SubResource("spare")
nodes/spare/position = Vector2(200, 400)
nodes/sub/node = SubResource("sub")
nodes/sub/position = Vector2(400, 400)
nodes/wave/node = SubResource("wave")
nodes/wave/position = Vector2(400, 0)
node_connections = [&"output", 0, &"layer", &"layer", 0, &"wave", &"layer", 1, &"pick", &"layer", 2, &"aim", &"pick", 0, &"lean", &"pick", 1, &"seek", &"seek", 0, &"reach", &"sub", 0, &"spare", &"sub", 1, &"custom"]
)");
    write(folder / "hero.tscn", R"tscn([gd_scene load_steps=20 format=3]

[ext_resource type="PackedScene" path="res://models/arm.glb" id="1_arm"]
[ext_resource type="AnimationNodeBlendTree" path="res://trees/moves.tres" id="2_moves"]

[sub_resource type="Animation" id="Animation_glow"]
resource_name = "glow"
length = 1.0
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Lamp:light_energy")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0, 1),
"transitions": PackedFloat32Array(1, 1),
"update": 0,
"values": [1.0, 3.0]
}
tracks/1/type = "method"
tracks/1/imported = false
tracks/1/enabled = true
tracks/1/path = NodePath("Lamp")
tracks/1/interp = 1
tracks/1/loop_wrap = true
tracks/1/keys = {
"times": PackedFloat32Array(0.5),
"transitions": PackedFloat32Array(1),
"values": [{
"args": [],
"method": &"flicker"
}]
}

[sub_resource type="Animation" id="Animation_fade"]
resource_name = "fade"
length = 1.0
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Lamp:light_energy")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0, 1),
"transitions": PackedFloat32Array(1, 1),
"update": 0,
"values": [3.0, 0.0]
}

[sub_resource type="AnimationLibrary" id="AnimationLibrary_lamp"]
_data = {
&"fade": SubResource("Animation_fade"),
&"glow": SubResource("Animation_glow")
}

[sub_resource type="AnimationNodeAnimation" id="glow"]
animation = &"glow"

[sub_resource type="AnimationNodeAnimation" id="fade"]
animation = &"fade"

[sub_resource type="Curve" id="slow"]
_data = [Vector2(0, 0), 0.0, 0.0, 0, 0, Vector2(0.5, 0.2), 0.0, 0.0, 0, 0, Vector2(1, 1), 0.0, 0.0, 0, 0]
point_count = 3

[sub_resource type="AnimationNodeStateMachineTransition" id="into_glow"]
advance_mode = 2

[sub_resource type="AnimationNodeStateMachineTransition" id="glow_to_fade"]
xfade_time = 0.25
xfade_curve = SubResource("slow")
switch_mode = 2
advance_mode = 2
advance_expression = "is_dark()"
priority = 2

[sub_resource type="AnimationNodeStateMachine" id="lit"]
state_machine_type = 1
states/Start/position = Vector2(0, 0)
states/glow/node = SubResource("glow")
states/glow/position = Vector2(200, 0)
states/fade/node = SubResource("fade")
states/fade/position = Vector2(400, 0)
transitions = [&"Start", &"glow", SubResource("into_glow"), &"glow", &"fade", SubResource("glow_to_fade")]

[sub_resource type="AnimationNodeAnimation" id="group_fade"]
animation = &"fade"

[sub_resource type="AnimationNodeStateMachineTransition" id="into_group_fade"]
advance_mode = 2

[sub_resource type="AnimationNodeStateMachine" id="group"]
state_machine_type = 2
states/group_fade/node = SubResource("group_fade")
transitions = [&"Start", &"group_fade", SubResource("into_group_fade")]

[sub_resource type="AnimationNodeAnimation" id="off"]
animation = &"missing"

[sub_resource type="AnimationNodeStateMachineTransition" id="into_lit"]
advance_mode = 2

[sub_resource type="AnimationNodeStateMachineTransition" id="lit_to_group"]
advance_mode = 2
advance_condition = &"dark"

[sub_resource type="AnimationNodeStateMachineTransition" id="lit_to_off"]

[sub_resource type="AnimationNodeStateMachine" id="machine"]
allow_transition_to_self = true
states/Start/position = Vector2(0, 0)
states/lit/node = SubResource("lit")
states/group/node = SubResource("group")
states/off/node = SubResource("off")
transitions = [&"Start", &"lit", SubResource("into_lit"), &"lit", &"group", SubResource("lit_to_group"), &"lit", &"off", SubResource("lit_to_off")]

[sub_resource type="AnimationNodeAnimation" id="flash"]
animation = &"glow"

[sub_resource type="AnimationNodeAnimation" id="dim"]
animation = &"fade"

[sub_resource type="AnimationNodeAnimation" id="spark"]
animation = &"glow"

[sub_resource type="AnimationNodeBlend2" id="gate"]
filter_enabled = true
filters = ["Lamp:light_energy", "Lamp", "Ghost:visible"]

[sub_resource type="AnimationNodeBlend3" id="mix"]
filter_enabled = true
sync = true

[sub_resource type="AnimationNodeBlendTree" id="glow_tree"]
nodes/machine/node = SubResource("machine")
nodes/mix/node = SubResource("mix")
nodes/flash/node = SubResource("flash")
nodes/dim/node = SubResource("dim")
nodes/gate/node = SubResource("gate")
nodes/spark/node = SubResource("spark")
node_connections = [&"output", 0, &"mix", &"mix", 0, &"dim", &"mix", 1, &"gate", &"mix", 2, &"flash", &"gate", 0, &"machine", &"gate", 1, &"spark"]

[sub_resource type="AnimationNodeAnimation" id="lone"]
animation = &"Bend"

[node name="Hero" type="Node3D"]

[node name="Arm" parent="." instance=ExtResource("1_arm")]

[node name="ArmTree" type="AnimationTree" parent="."]
tree_root = ExtResource("2_moves")
anim_player = NodePath("../Arm/AnimationPlayer")
active = false
callback_mode_process = 2
root_motion_track = NodePath("Armature/Skeleton3D:Upper")
root_motion_local = true
parameters/pick/transition_request = "move"
parameters/pick/current_state = "rest"
parameters/lean/blend_position = 0.5
parameters/aim/blend_position = Vector2(0.25, 0.5)
parameters/seek/seek_request = -1.0
parameters/layer/add_amount = 1.5

[node name="Lamp" type="OmniLight3D" parent="."]

[node name="GlowTree" type="AnimationTree" parent="."]
root_node = NodePath("..")
libraries = {
&"": SubResource("AnimationLibrary_lamp")
}
tree_root = SubResource("glow_tree")
deterministic = false
parameters/mix/blend_amount = 0.5
parameters/machine/conditions/dark = true
parameters/machine/playback = null
parameters/machine/lit/playback = null
parameters/mix/nonsense = 1

[node name="LostTree" type="AnimationTree" parent="."]
tree_root = SubResource("lone")
anim_player = NodePath("../Nowhere")

[node name="LampTree" type="AnimationTree" parent="."]
tree_root = SubResource("lone")
anim_player = NodePath("../Lamp")
)tscn");
    return folder;
}

TEST_CASE("importing Godot AnimationTrees writes their trees and plays them where Godot's do") {
    const fs::path project = "import_godot_trees";
    make_tree_project(project / "godot");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    CHECK(plan.report.find("converted")->find("animation_trees")->as_int() == 2);
    const std::string hero = "godot/hero.scene.json";

    // A tree that plays a model's own animations, through the AnimationPlayer Godot's importer
    // gives the model, goes on the entity that draws the model, whose nodes it poses. Its
    // parameters keep what callers set and leave what the tree keeps as it plays.
    CHECK(*entry(files, hero, "arm").find("components")->find("AnimationTree") == parse(R"({
        "tree_root": "godot/trees/moves.animtree.json",
        "libraries": {"": "godot/models/arm.glb"}, "active": false,
        "root_motion_track": "Upper", "root_motion_local": true,
        "parameters": {"aim/blend_position": [0.25, 0.5], "layer/add_amount": 1.5,
                       "lean/blend_position": 0.5, "pick/transition_request": "move"}})"));
    CHECK(unsupported(plan.report, "AnimationTree").find("why")->as_string() ==
          "it plays on arm, the entity that draws the model whose animations it plays");
    CHECK(unsupported(plan.report, "AnimationTree.callback_mode_process") != nullptr);
    CHECK(entry(files, hero, "arm_tree").find("components") == nullptr);

    // The tree saved as its own resource becomes a file beside it. Godot's importer names the
    // model's "Idle-loop" "Idle"; the file names it as the model does. Bones in filters and the
    // root motion track become the model's nodes.
    const std::string moves = "godot/trees/moves.animtree.json";
    REQUIRE(files.contains(moves));
    const json::Value moves_root = *parse(files.at(moves)).find("root");
    CHECK(moves_root.find("output")->as_string() == "layer");
    const json::Value& blend_nodes = *moves_root.find("nodes");
    std::vector<std::string> names;
    for (const auto& [name, node] : blend_nodes.as_object()) {
        names.push_back(name);
    }
    CHECK(names == std::vector<std::string>{"aim", "layer", "lean", "pick", "reach", "seek",
                                            "spare", "wave"});
    CHECK(*blend_nodes.find("layer") == parse(R"({"type": "Add3", "filter_enabled": true,
        "filters": ["Lower"], "inputs": {"-add": "wave", "in": "pick", "+add": "aim"}})"));
    CHECK(*blend_nodes.find("wave") == parse(R"({"type": "Animation", "animation": "Idle-loop"})"));
    CHECK(*blend_nodes.find("lean") == parse(R"({"type": "BlendSpace1D", "min_space": -2.0,
        "snap": 0.5, "value_label": "lean", "blend_mode": "discrete", "points": [
            {"position": -1.0, "node": {"type": "Animation", "animation": "Idle-loop"}},
            {"position": 1.0, "node": {"type": "Animation", "animation": "Bend",
                                       "play_mode": "backward", "use_custom_timeline": true,
                                       "timeline_length": 2.0, "loop_mode": "pingpong"}}]})"));
    CHECK(*blend_nodes.find("pick") == parse(R"({"type": "Transition", "xfade_time": 0.3,
        "xfade_curve": [[0.0, 0.0], [1.0, 1.0]], "inputs": [
            {"name": "rest", "node": "lean", "auto_advance": false, "break_loop_at_end": false,
             "reset": true},
            {"name": "move", "node": "seek", "auto_advance": true, "break_loop_at_end": false,
             "reset": false}]})"));
    CHECK(*blend_nodes.find("seek") ==
          parse(R"({"type": "TimeSeek", "explicit_elapse": false, "inputs": {"in": "reach"}})"));
    // An animation the model lacks plays nothing in Godot; its point goes, and the triangle
    // through it.
    CHECK(*blend_nodes.find("aim") == parse(R"({"type": "BlendSpace2D", "min_space": [-1.0, -1.0],
        "snap": [0.1, 0.1], "x_label": "yaw", "y_label": "pitch", "auto_triangles": false,
        "triangles": [], "points": [
            {"position": [0.0, 0.0], "node": {"type": "Animation", "animation": "Idle-loop"}},
            {"position": [1.0, 0.0], "node": {"type": "Animation", "animation": "Bend"}}]})"));
    CHECK(unsupported(plan.report, "AnimationNodeAnimation.animation").find("why")->as_string() ==
          "the libraries have no animation \"Missing\", so it plays nothing");
    CHECK(unsupported(plan.report, "BlendSpace2D point").find("why")->as_string() ==
          "point 2 didn't convert");
    CHECK(unsupported(plan.report, "BlendSpace2D triangle").find("why")->as_string() ==
          "triangle 0 has a point that didn't convert");
    CHECK(unsupported(plan.report, "Add3.filters").find("why")->as_string() ==
          "no converted animation has a track for Armature/Skeleton3D:Elbow");
    // A node type that doesn't convert takes the node with an input it fed with it.
    CHECK(unsupported(plan.report, "AnimationTree.tree_root/custom").find("why")->as_string() ==
          "AnimationNodeExtension doesn't convert");
    CHECK(unsupported(plan.report, "BlendTree node").find("why")->as_string() ==
          "sub has nothing that converted connected to its input sub, so it plays nothing");
    CHECK(unsupported(plan.report, "Transition.xfade_curve").find("why")->as_string() ==
          "its points are joined by straight lines rather than Godot's curve");

    // A tree with libraries of its own plays them from its root node, with their targets.
    CHECK(*entry(files, hero, "glow_tree").find("components")->find("AnimationTree") ==
          parse(R"({"tree_root": "godot/hero_glow_tree.animtree.json",
                    "libraries": {"": "godot/hero_animation_library_lamp.anim.json"},
                    "targets": {"Lamp": "lamp"}, "deterministic": false,
                    "parameters": {"machine/conditions/dark": true, "mix/blend_amount": 0.5}})"));
    CHECK(
        unsupported(plan.report, "AnimationTree parameter mix/nonsense").find("why")->as_string() ==
        "the tree has no such parameter");
    // A tree that isn't deterministic leaves the library without a RESET animation.
    CHECK(parse(files.at("godot/hero_animation_library_lamp.anim.json"))
              .find("animations")
              ->find("RESET") == nullptr);

    const std::string glow = "godot/hero_glow_tree.animtree.json";
    REQUIRE(files.contains(glow));
    const json::Value glow_nodes = *parse(files.at(glow)).find("root")->find("nodes");
    // Filters name converted tracks: a property's value track, a node's method tracks.
    CHECK(*glow_nodes.find("gate") == parse(R"({"type": "Blend2", "filter_enabled": true,
        "filters": ["Lamp:OmniLight3D/light_energy", "Lamp:@flicker"],
        "inputs": {"in": "machine", "blend": "spark"}})"));
    CHECK(unsupported(plan.report, "Blend2.filters").find("why")->as_string() ==
          "no converted animation has a track for Ghost:visible");
    CHECK(*glow_nodes.find("mix") == parse(R"({"type": "Blend3", "sync": true,
        "inputs": {"-blend": "dim", "in": "gate", "+blend": "flash"}})"));
    CHECK(unsupported(plan.report, "Blend3.filter_enabled").find("why")->as_string() ==
          "Blend3 has no filters, as in Godot, where they do nothing");
    // State machines nest and group; a state whose animation is missing goes with its
    // transitions.
    const json::Value& machine = *glow_nodes.find("machine");
    CHECK(machine.find("allow_transition_to_self")->as_bool());
    CHECK(machine.find("states")->find("off") == nullptr);
    CHECK(*machine.find("transitions") == parse(R"([
        {"from": "Start", "to": "lit", "advance_mode": "auto"},
        {"from": "lit", "to": "group", "advance_mode": "auto", "advance_condition": "dark"}])"));
    CHECK(unsupported(plan.report, "StateMachine transition").find("why")->as_string() ==
          "from lit to off, a state that didn't convert");
    const json::Value& lit = *machine.find("states")->find("lit")->find("node");
    CHECK(lit.find("state_machine_type")->as_string() == "nested");
    CHECK(lit.find("transitions")->as_array().at(1) == parse(R"({"from": "glow", "to": "fade",
        "xfade_time": 0.25, "xfade_curve": [[0.0, 0.0], [0.5, 0.2], [1.0, 1.0]],
        "switch_mode": "at_end", "advance_mode": "auto", "priority": 2})"));
    CHECK(unsupported(plan.report, "StateMachine transition advance_expression")
              .find("why")
              ->as_string() ==
          "expressions don't convert; set a condition from gameplay code instead");
    // A grouped state machine's Start needn't be written out, as Godot's always has one.
    const json::Value& group = *machine.find("states")->find("group")->find("node");
    CHECK(group == parse(R"({"type": "StateMachine", "state_machine_type": "grouped",
        "states": {"group_fade": {"node": {"type": "Animation", "animation": "fade"}}},
        "transitions": [{"from": "Start", "to": "group_fade", "advance_mode": "auto"}]})"));

    // Trees whose anim_player names nothing they can play don't convert.
    const json::Value lost = unsupported(plan.report, "AnimationTree.anim_player");
    CHECK(lost.find("count")->as_int() == 2);
    CHECK(lost.find("why")->as_string() == "node path ../Nowhere names no node in the scene");
    CHECK(entry(files, hero, "lost_tree").find("components") == nullptr);
    CHECK(entry(files, hero, "lamp_tree").find("components") == nullptr);

    // What converted checks out as a scene's trees do.
    const slash::world::FileReader reader =
        [&](std::string_view path) -> slash::Result<std::string> {
        if (const auto it = files.find(std::string(path)); it != files.end()) {
            return it->second;
        }
        return slash::platform::read_file((project / path).string());
    };
    for (const json::ValidationError& error :
         slash::animation::check_trees(parse(files.at(hero)), reader)) {
        FAIL_CHECK(error.path << ": " << error.message);
    }
}

TEST_CASE("importing Godot's 3D tracks moves a model's nodes and a scene's 3D nodes as Godot's "
          "do") {
    const fs::path project = "import_godot_tracks";
    fs::remove_all(project);
    write(project / "godot/project.godot", R"(config_version=5

[application]

config/name="Tracks"
run/main_scene="res://stage.tscn"
)");
    write(project / "godot/models/arm.glb", slash::test::arm().glb());
    write(project / "godot/stage.tscn", R"tscn([gd_scene load_steps=5 format=3]

[ext_resource type="PackedScene" path="res://models/arm.glb" id="1_arm"]

[sub_resource type="Animation" id="Animation_wave"]
resource_name = "wave"
length = 1.0
loop_mode = 1
tracks/0/type = "rotation_3d"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Arm/Armature/Skeleton3D:Lower")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1, 0.5, 1, 0, 0, 0.7071068, 0.7071068)
tracks/1/type = "position_3d"
tracks/1/imported = false
tracks/1/enabled = true
tracks/1/path = NodePath("Arm/Armature/Skeleton3D:Upper")
tracks/1/interp = 2
tracks/1/loop_wrap = false
tracks/1/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1, 1, 0, 0.5, 0)
tracks/2/type = "rotation_3d"
tracks/2/imported = false
tracks/2/enabled = true
tracks/2/path = NodePath("Box")
tracks/2/interp = 1
tracks/2/loop_wrap = true
tracks/2/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1, 0.5, 1, 0, 1, 0, 0)
tracks/3/type = "scale_3d"
tracks/3/imported = false
tracks/3/enabled = true
tracks/3/path = NodePath("Box")
tracks/3/interp = 0
tracks/3/loop_wrap = true
tracks/3/keys = PackedFloat32Array(0, 1, 1, 1, 1, 0.5, 1, 2, 2, 2)
tracks/4/type = "position_3d"
tracks/4/imported = false
tracks/4/enabled = true
tracks/4/path = NodePath("Box")
tracks/4/interp = 1
tracks/4/loop_wrap = true
tracks/4/keys = PackedFloat32Array(0, 0.5, 0, 0, 0, 1, 1, 0, 1, 0)
tracks/5/type = "blend_shape"
tracks/5/imported = false
tracks/5/enabled = true
tracks/5/path = NodePath("Arm/Armature/Skeleton3D/Arm:smile")
tracks/5/interp = 1
tracks/5/loop_wrap = true
tracks/5/keys = PackedFloat32Array(0, 1, 0)
tracks/6/type = "rotation_3d"
tracks/6/imported = false
tracks/6/enabled = true
tracks/6/path = NodePath("Arm/Armature/Skeleton3D:Lowr")
tracks/6/interp = 1
tracks/6/loop_wrap = true
tracks/6/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1)
tracks/7/type = "scale_3d"
tracks/7/imported = false
tracks/7/enabled = true
tracks/7/path = NodePath("Rig:Hip")
tracks/7/interp = 1
tracks/7/loop_wrap = true
tracks/7/keys = PackedFloat32Array(0, 1, 1, 1, 1)

[sub_resource type="AnimationLibrary" id="AnimationLibrary_stage"]
_data = {
&"wave": SubResource("Animation_wave")
}

[sub_resource type="AnimationNodeAnimation" id="AnimationNodeAnimation_wave"]
animation = &"wave"

[node name="Stage" type="Node3D"]

[node name="Arm" parent="." instance=ExtResource("1_arm")]

[node name="Box" type="MeshInstance3D" parent="."]

[node name="Rig" type="Skeleton3D" parent="."]

[node name="Mover" type="AnimationPlayer" parent="."]
libraries = {
&"": SubResource("AnimationLibrary_stage")
}
autoplay = &"wave"

[node name="MoverTree" type="AnimationTree" parent="."]
anim_player = NodePath("../Mover")
root_motion_track = NodePath("Arm/Armature/Skeleton3D:Upper")
tree_root = SubResource("AnimationNodeAnimation_wave")
)tscn");
    write(project / "godot/hand.tscn", R"tscn([gd_scene load_steps=9 format=3]

[ext_resource type="PackedScene" path="res://models/arm.glb" id="1_arm"]

[sub_resource type="Animation" id="Animation_bend"]
resource_name = "bend"
length = 1.0
tracks/0/type = "rotation_3d"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Arm/Armature/Skeleton3D:Lower")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0.7071068, 0.7071068)
tracks/1/type = "position_3d"
tracks/1/imported = false
tracks/1/enabled = true
tracks/1/path = NodePath("Lamp")
tracks/1/interp = 1
tracks/1/loop_wrap = true
tracks/1/keys = PackedFloat32Array(0, 1, 0, 2, 0, 1, 1, 0, 3, 0)

[sub_resource type="Animation" id="Animation_still"]
resource_name = "still"
length = 1.0
tracks/0/type = "rotation_3d"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Arm/Armature/Skeleton3D:Lower")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = PackedFloat32Array(0, 1, 0, 0, 0, 1)

[sub_resource type="AnimationLibrary" id="AnimationLibrary_hand"]
_data = {
&"bend": SubResource("Animation_bend"),
&"still": SubResource("Animation_still")
}

[sub_resource type="AnimationNodeAnimation" id="AnimationNodeAnimation_bend"]
animation = &"bend"

[sub_resource type="AnimationNodeAnimation" id="AnimationNodeAnimation_still"]
animation = &"still"

[sub_resource type="AnimationNodeBlend2" id="AnimationNodeBlend2_mix"]
filter_enabled = true
filters = ["Arm/Armature/Skeleton3D:Lower", "Lamp"]

[sub_resource type="AnimationNodeBlendTree" id="AnimationNodeBlendTree_hand"]
nodes/bend/node = SubResource("AnimationNodeAnimation_bend")
nodes/still/node = SubResource("AnimationNodeAnimation_still")
nodes/mix/node = SubResource("AnimationNodeBlend2_mix")
node_connections = [&"output", 0, &"mix", &"mix", 0, &"still", &"mix", 1, &"bend"]

[node name="Hand" type="Node3D"]

[node name="Arm" parent="." instance=ExtResource("1_arm")]

[node name="Lamp" type="OmniLight3D" parent="."]

[node name="HandTree" type="AnimationTree" parent="."]
root_node = NodePath("..")
libraries = {
&"": SubResource("AnimationLibrary_hand")
}
tree_root = SubResource("AnimationNodeBlendTree_hand")
parameters/mix/blend_amount = 0.5
)tscn");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);

    // Tracks that move a model's bones become node tracks, so the player plays on the entity
    // that draws the model, and its tree with it. A track that moves a 3D node of the scene
    // becomes a Transform3D value track, which the player's targets reach.
    const std::string stage = "godot/stage.scene.json";
    const std::string wave = "godot/stage_animation_library_stage.anim.json";
    CHECK(entity(files, stage, "arm") == parse(R"({
        "AnimationPlayer": {"animation": "wave", "library": "godot/stage_animation_library_stage.anim.json",
                            "targets": {"Box": "box"}},
        "AnimationTree": {"tree_root": "godot/stage_mover_tree.animtree.json",
                          "libraries": {"": "godot/stage_animation_library_stage.anim.json"},
                          "targets": {"Box": "box"}, "root_motion_track": "Upper"},
        "MeshInstance3D": {"mesh": "godot/models/arm.glb"}, "Transform3D": {}})"));
    CHECK(entry(files, stage, "mover").find("components") == nullptr);
    CHECK(unsupported(plan.report, "AnimationPlayer").find("why")->as_string() ==
          "it plays on arm, the entity that draws the model whose nodes its tracks move");
    REQUIRE(files.contains(wave));
    const json::Value animations = *parse(files.at(wave)).find("animations");
    // Rotations are Euler angles. Godot slerps between keys, so a Transform3D track turning
    // half round gets keys in between, the turn round the loop to the first key included;
    // a node track slerps as Godot's does.
    CHECK(*animations.find("wave") == parse(R"({"length": 1.0, "loop": "linear", "tracks": [
        {"node": "Lower", "property": "rotation",
         "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 0, 1.570796]}]},
        {"node": "Upper", "property": "position", "loop_wrap": false,
         "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 0.5, 0]}]},
        {"target": "Box", "path": "Transform3D/rotation", "interpolation": "linear_angle",
         "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.25, "value": [0, 1.570796, 0]},
                  {"time": 0.5, "value": [0, 3.141593, 0]},
                  {"time": 0.75, "value": [0, 1.570796, 0]}, {"time": 1, "value": [0, 0, 0]}]},
        {"target": "Box", "path": "Transform3D/scale", "interpolation": "nearest",
         "keys": [{"time": 0, "value": [1, 1, 1]}, {"time": 0.5, "value": [2, 2, 2]}]},
        {"target": "Box", "path": "Transform3D/position",
         "keys": [{"time": 0, "value": [0, 0, 0], "transition": 0.5},
                  {"time": 1, "value": [0, 1, 0]}]}]})"));
    // A deterministic tree starts a 3D node's scale at one, as Godot's does.
    CHECK(*animations.find("RESET")->find("tracks") == parse(R"([
        {"target": "Box", "path": "Transform3D/rotation", "keys": [{"time": 0, "value": [0, 0, 0]}]},
        {"target": "Box", "path": "Transform3D/scale", "keys": [{"time": 0, "value": [1, 1, 1]}]},
        {"target": "Box", "path": "Transform3D/position", "keys": [{"time": 0, "value": [0, 0, 0]}]}])"));
    CHECK(unsupported(plan.report, "animation position_3d track").find("why")->as_string() ==
          "node tracks interpolate linearly, so it does");
    CHECK(unsupported(plan.report, "animation rotation_3d track").find("why")->as_string() ==
          "\"Lowr\" isn't a node of models/arm.glb; did you mean \"Lower\"?");
    CHECK(unsupported(plan.report, "animation scale_3d track").find("why")->as_string() ==
          "bones of a Skeleton3D in a scene don't convert yet; a model's do");
    CHECK(unsupported(plan.report, "animation blend_shape track").find("why")->as_string() ==
          "SlashEngine models have no blend shapes yet");

    // A tree with libraries of its own goes on the model's entity too. Filters name a model's
    // bones by their nodes, and a 3D node by the Transform3D tracks its tracks became.
    const std::string hand = "godot/hand.scene.json";
    CHECK(*entity(files, hand, "arm").find("AnimationTree") ==
          parse(R"({"tree_root": "godot/hand_hand_tree.animtree.json",
                    "libraries": {"": "godot/hand_animation_library_hand.anim.json"},
                    "targets": {"Lamp": "lamp"}, "parameters": {"mix/blend_amount": 0.5}})"));
    REQUIRE(files.contains("godot/hand_hand_tree.animtree.json"));
    CHECK(*parse(files.at("godot/hand_hand_tree.animtree.json"))
               .find("root")
               ->find("nodes")
               ->find("mix")
               ->find("filters") == parse(R"(["Lower", "Lamp:Transform3D/position"])"));

    // What converted checks out as a scene's players and trees do, the bones node tracks move
    // included.
    const slash::world::FileReader reader =
        [&](std::string_view path) -> slash::Result<std::string> {
        if (const auto it = files.find(std::string(path)); it != files.end()) {
            return it->second;
        }
        return slash::platform::read_file((project / path).string());
    };
    CHECK(reader("godot/models/arm.glb").ok());
    for (const std::string& scene : {stage, hand}) {
        for (const json::ValidationError& error :
             slash::animation::check_players(parse(files.at(scene)), reader)) {
            FAIL_CHECK(scene << error.path << ": " << error.message);
        }
        for (const json::ValidationError& error :
             slash::animation::check_trees(parse(files.at(scene)), reader)) {
            FAIL_CHECK(scene << error.path << ": " << error.message);
        }
    }
}

namespace {

// A figure whose skin's bones aren't in the file in Godot's order: under Root, the joints Tail and
// Hips; under Hips, Spine then Leg.L in its children but Leg.L first in the file; Head.Top under
// Spine. Godot's importer makes them one Skeleton3D under Root, its bones Tail, Hips, Leg.L,
// Spine and Head.Top. Its one animation, "Wag", turns Tail.
slash::test::Gltf figure() {
    slash::test::Gltf out;
    json::Value attributes;
    attributes["POSITION"] = out.floats(
        {-0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.5F, 2.0F, 0.0F, -0.5F, 2.0F, 0.0F}, "VEC3");
    attributes["JOINTS_0"] = out.joints({1, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0});
    attributes["WEIGHTS_0"] = out.floats({1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                          0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F},
                                         "VEC4");
    json::Value primitive;
    primitive["attributes"] = std::move(attributes);
    primitive["indices"] = out.indices({0, 1, 2, 0, 2, 3});
    json::Value mesh;
    mesh["name"] = "Skin";
    mesh["primitives"].push_back(std::move(primitive));
    out.document["meshes"].push_back(std::move(mesh));
    json::Value skin;
    skin["joints"] = slash::test::gltf_json("[3, 5, 4, 2, 1]");
    std::vector<float> binds;
    for (int joint = 0; joint < 5; ++joint) {
        binds.insert(binds.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    }
    skin["inverseBindMatrices"] = out.floats(binds, "MAT4");
    out.document["skins"].push_back(std::move(skin));
    out.document["nodes"] = slash::test::gltf_json(R"([
        {"name": "Root", "children": [3, 1, 6]},
        {"name": "Tail", "translation": [0, 0.8, -0.3]},
        {"name": "Leg.L", "translation": [0.2, -0.5, 0]},
        {"name": "Hips", "translation": [0, 1, 0], "children": [5, 2]},
        {"name": "Head.Top", "translation": [0, 0.5, 0]},
        {"name": "Spine", "translation": [0, 0.5, 0], "children": [4]},
        {"name": "Skin", "mesh": 0, "skin": 0}
    ])");
    json::Value sampler;
    sampler["input"] = out.floats({0.0F, 1.0F}, "SCALAR");
    sampler["output"] = out.floats({0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F}, "VEC4");
    json::Value animation;
    animation["name"] = "Wag";
    animation["samplers"].push_back(std::move(sampler));
    animation["channels"] =
        slash::test::gltf_json(R"([{"sampler": 0, "target": {"node": 1, "path": "rotation"}}])");
    out.document["animations"].push_back(std::move(animation));
    out.document["scenes"] = slash::test::gltf_json(R"([{"nodes": [0]}])");
    out.document["scene"] = 0;
    return out;
}

} // namespace

TEST_CASE("Godot's Skeleton3D poses, skeleton modifiers, ragdolls and bone attachments convert "
          "with the model they're under") {
    const fs::path project = "import_godot_skeletons";
    fs::remove_all(project);
    write(project / "godot/project.godot", R"(config_version=5

[application]

config/name="Skeletons"
run/main_scene="res://fighter.tscn"
)");
    write(project / "godot/models/figure.gltf", figure().gltf());
    write(project / "godot/fighter.tscn", R"tscn([gd_scene load_steps=4 format=3]

[ext_resource type="PackedScene" path="res://models/figure.gltf" id="1_figure"]

[sub_resource type="JointLimitationCone3D" id="JointLimitationCone3D_spine"]
angle = 0.5

[sub_resource type="BoxShape3D" id="BoxShape3D_hips"]
size = Vector3(0.4, 0.3, 0.2)

[sub_resource type="CylinderMesh" id="CylinderMesh_brim"]
top_radius = 0.3
bottom_radius = 0.3
height = 0.05

[sub_resource type="Animation" id="Animation_kick"]
resource_name = "kick"
length = 1.0
tracks/0/type = "position_3d"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Body/Root/Skeleton3D:Leg.L")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = PackedFloat32Array(0, 1, 0.2, -0.5, 0, 1, 1, 0.2, -0.3, 0.4)

[sub_resource type="AnimationLibrary" id="AnimationLibrary_fighter"]
_data = {
&"kick": SubResource("Animation_kick")
}

[node name="Fighter" type="Node3D"]

[node name="Moves" type="AnimationPlayer" parent="."]
libraries = {
&"": SubResource("AnimationLibrary_fighter")
}

[node name="Goal" type="Marker3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 1)

[node name="Pole" type="Marker3D" parent="."]

[node name="Body" parent="." instance=ExtResource("1_figure")]

[node name="Skeleton3D" parent="Body/Root" index="0"]
bones/0/rotation = Quaternion(1, 0, 0, 0)
bones/1/rotation = Quaternion(0, 0, 0.38268343, 0.9238795)
bones/2/position = Vector3(0.25, -0.5, 0)
bones/3/position = Vector3(0, 0.5, 0)
bones/4/name = "Crown"
motion_scale = 2.0

[node name="Skin" parent="Body/Root/Skeleton3D" index="0"]
visible = false

[node name="BodyIK" type="TwoBoneIK3D" parent="Body/Root/Skeleton3D"]
influence = 0.75
setting_count = 1
settings/0/target_node = NodePath("../../../../Goal")
settings/0/pole_node = NodePath("../../../../Pole")
settings/0/root_bone_name = "Hips"
settings/0/root_bone = 1
settings/0/middle_bone_name = ""
settings/0/middle_bone = 3
settings/0/pole_direction = 5
settings/0/pole_direction_vector = Vector3(0, 0, 0)
settings/0/end_bone_name = "Head.Top"
settings/0/end_bone = 4
settings/0/use_virtual_end = false
settings/0/extend_end_bone = true
settings/0/end_bone/direction = 2
settings/0/end_bone/length = 0.1

[node name="SpineIK" type="FABRIK3D" parent="Body/Root/Skeleton3D"]
max_iterations = 8
deterministic = true
setting_count = 2
settings/0/root_bone_name = "Hips"
settings/0/root_bone = 1
settings/0/end_bone_name = "Head.Top"
settings/0/end_bone = 4
settings/0/target_node = NodePath("../../../../Goal")
settings/0/joint_count = 3
settings/0/joints/0/bone_name = "Hips"
settings/0/joints/0/bone = 1
settings/0/joints/0/rotation_axis = 3
settings/0/joints/1/rotation_axis = 0
settings/0/joints/1/limitation = SubResource("JointLimitationCone3D_spine")
settings/0/joints/1/limitation/right_axis = 3
settings/0/joints/3/rotation_axis = 1
settings/1/root_bone_name = "Leg.L"
settings/1/end_bone_name = "Spine"

[node name="Simulator" type="PhysicalBoneSimulator3D" parent="Body/Root/Skeleton3D"]
influence = 0.5

[node name="Physical Bone Hips" type="PhysicalBone3D" parent="Body/Root/Skeleton3D/Simulator"]
joint_type = 2
joint_offset = Transform3D(1, 0, 0, 0, 0, -1, 0, 1, 0, 0, 0.1, 0)
body_offset = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.1, 0)
bone_name = &"Hips"
mass = 3.0
joint_constraints/swing_span = 30.0
joint_constraints/twist_span = 90.0
joint_constraints/bias = 0.3
joint_constraints/softness = 0.5
joint_constraints/relaxation = 1.0

[node name="CollisionShape3D" type="CollisionShape3D" parent="Body/Root/Skeleton3D/Simulator/Physical Bone Hips"]
shape = SubResource("BoxShape3D_hips")

[node name="Physical Bone Leg_L" type="PhysicalBone3D" parent="Body/Root/Skeleton3D/Simulator"]
joint_type = 3
bone_name = &"Leg.L"
joint_constraints/angular_limit_enabled = true
joint_constraints/angular_limit_upper = 45.0
joint_constraints/angular_limit_lower = -10.0
joint_constraints/angular_limit_bias = 0.3
joint_constraints/angular_limit_softness = 0.9
joint_constraints/angular_limit_relaxation = 1.0
linear_damp_mode = 1
custom_integrator = true

[node name="Physical Bone Spine" type="PhysicalBone3D" parent="Body/Root/Skeleton3D/Simulator"]
joint_type = 5
bone_name = &"Spine"
joint_constraints/x/angular_limit_upper = 20.0
joint_constraints/x/angular_limit_lower = -20.0
joint_constraints/y/linear_limit_enabled = false
joint_constraints/z/erp = 0.5

[node name="Hat" type="BoneAttachment3D" parent="Body/Root/Skeleton3D"]
bone_name = "Head.Top"
bone_idx = 4

[node name="Brim" type="MeshInstance3D" parent="Body/Root/Skeleton3D/Hat"]
mesh = SubResource("CylinderMesh_brim")

[node name="Looker" type="LookAtModifier3D" parent="Body/Root/Skeleton3D"]

[node name="Stray" type="TwoBoneIK3D" parent="Body/Root"]

[node name="Holder" type="BoneAttachment3D" parent="."]
bone_name = "Tail"
bone_idx = 2
use_external_skeleton = true
external_skeleton = NodePath("../Body/Root/Skeleton3D")

[node name="Dummy" parent="." instance=ExtResource("1_figure")]

[node name="Physical Bone Hips" type="PhysicalBone3D" parent="Dummy/Root/Skeleton3D"]
bone_name = &"Hips"

[node name="Rig" type="Skeleton3D" parent="."]

[node name="Grip" type="BoneAttachment3D" parent="Rig"]
bone_name = "Hand"
)tscn");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string scene = "godot/fighter.scene.json";
    REQUIRE(files.contains(scene));

    // The Skeleton3D's bone poses pose the model's nodes, found by Godot's bone order: bone 1 is
    // Hips and bone 2 Leg.L, whose '.' a model's node names make '_'. A pose the model already
    // has is left out, and one of a bone the model's own animations move is reported, since a
    // pose would hold it still.
    const json::Value body = entity(files, scene, "body");
    CHECK(*body.find("Skeleton3D") == parse(R"({"bone_positions": {"Leg_L": [0.25, -0.5, 0.0]},
                                               "bone_rotations": {"Hips": [0.0, 0.0, 0.785398]}})"));
    CHECK(unsupported(plan.report, "Skeleton3D.bones/0/rotation").find("why")->as_string() ==
          "Tail is moved by the model's own animations, where SlashEngine's Skeleton3D pose "
          "would hold it still; pose it in the animations instead");
    CHECK(unsupported(plan.report, "Skeleton3D.bones/4/name").find("why")->as_string() ==
          "a model's bones are as its file has them; edit the model");
    CHECK(unsupported(plan.report, "Skeleton3D.motion_scale") != nullptr);
    CHECK(unsupported(plan.report, "node in an imported model").find("count")->as_int() == 2);
    // A track names the bone by Godot's name, with its '.'.
    const std::string kick = "godot/fighter_animation_library_fighter.anim.json";
    REQUIRE(files.contains(kick));
    const json::Value tracks =
        *parse(files.at(kick)).find("animations")->find("kick")->find("tracks");
    CHECK(*tracks.as_array().at(0).find("node") == "Leg_L");

    // What the scene adds under the Skeleton3D hangs from the model's entity, in Godot's order,
    // so its modifiers run in that order.
    CHECK(*body.find("Transform3D") ==
          parse(R"({"children": ["body_ik", "spine_ik", "simulator", "hat", "stray"]})"));
    // Bones are found by name, as Godot's modifiers find them, or by index where the name is
    // empty.
    CHECK(*entity(files, scene, "body_ik").find("TwoBoneIK3D") == parse(R"({
        "influence": 0.75,
        "settings": [{"root_bone": "Hips", "middle_bone": "Spine", "end_bone": "Head_Top",
                      "target": "goal", "pole": "pole", "pole_direction": "plus_z",
                      "extend_end_bone": true, "end_bone_direction": "plus_y",
                      "end_bone_length": 0.1}]})"));
    // A chain's joints are its bones from the root down, by name.
    CHECK(*entity(files, scene, "spine_ik").find("FABRIK3D") == parse(R"({
        "max_iterations": 8, "deterministic": true,
        "settings": [{"root_bone": "Hips", "end_bone": "Head_Top", "target": "goal",
                      "joints": {"Spine": {"rotation_axis": "x", "limitation": "cone",
                                           "limitation_angle": 0.5,
                                           "limitation_right_axis": "plus_y"}}},
                     {"root_bone": "Leg_L", "end_bone": "Spine"}]})"));
    CHECK(unsupported(plan.report, "FABRIK3D.settings/0/joints/3/rotation_axis")
              .find("why")
              ->as_string() == "the chain has 3 bones");
    CHECK(unsupported(plan.report, "FABRIK3D.settings/1/end_bone_name").find("why")->as_string() ==
          "Spine isn't Leg_L or a bone under it");

    // Physical bones keep their joints, Godot's degrees made radians; joint_offset gives the
    // joint's rotation, its origin being the bone's whatever it says.
    CHECK(*entity(files, scene, "simulator").find("PhysicalBoneSimulator3D") ==
          parse(R"({"influence": 0.5})"));
    CHECK(*entity(files, scene, "physical_bone_hips").find("PhysicalBone3D") == parse(R"({
        "bone_name": "Hips", "joint_type": "cone", "joint_rotation": [1.570796, 0.0, 0.0],
        "body_offset_position": [0.0, -0.1, 0.0], "mass": 3.0, "swing_span": 0.523599,
        "twist_span": 1.570796})"));
    CHECK(*entity(files, scene, "physical_bone_hips").find("Transform3D") ==
          parse(R"({"children": ["collision_shape3d"]})"));
    CHECK(*entity(files, scene, "physical_bone_leg_l").find("PhysicalBone3D") == parse(R"({
        "bone_name": "Leg_L", "joint_type": "hinge", "angular_limit_enabled": true,
        "angular_limit_upper": 0.785398, "angular_limit_lower": -0.174533,
        "linear_damp_mode": "replace"})"));
    CHECK(*entity(files, scene, "physical_bone_spine").find("PhysicalBone3D") == parse(R"({
        "bone_name": "Spine", "joint_type": "6dof",
        "x": {"angular_limit_upper": 0.349066, "angular_limit_lower": -0.349066},
        "y": {"linear_limit_enabled": false}})"));
    // Of what Godot's Jolt module ignores, only values other than Godot's defaults are reported.
    CHECK(unsupported(plan.report, "PhysicalBone3D.joint_constraints/softness") != nullptr);
    CHECK(unsupported(plan.report, "PhysicalBone3D.joint_constraints/bias") == nullptr);
    CHECK(unsupported(plan.report, "PhysicalBone3D.joint_constraints/z/erp") == nullptr);
    CHECK(unsupported(plan.report, "PhysicalBone3D.custom_integrator") != nullptr);
    // Physical bones right under a Skeleton3D, as Godot 4.2 put them, get the simulator Godot
    // 4.3 makes for them.
    const json::Value dummy = entity(files, scene, "dummy");
    CHECK(*dummy.find("PhysicalBoneSimulator3D") == parse("{}"));
    CHECK(*dummy.find("Transform3D") == parse(R"({"children": ["dummy_physical_bone_hips"]})"));

    // A bone attachment follows its bone with what hangs under it; one with an external
    // skeleton follows that model's entity, by bone_idx first as Godot's does.
    CHECK(*entity(files, scene, "hat").find("BoneAttachment3D") ==
          parse(R"({"bone_name": "Head_Top"})"));
    CHECK(*entity(files, scene, "hat").find("Transform3D") == parse(R"({"children": ["brim"]})"));
    CHECK(*entity(files, scene, "holder").find("BoneAttachment3D") ==
          parse(R"({"bone_name": "Leg_L", "external_skeleton": "body"})"));

    // Modifiers anywhere but under a model's Skeleton3D, and modifiers SlashEngine hasn't, are
    // reported.
    CHECK(unsupported(plan.report, "node type TwoBoneIK3D").find("why")->as_string() ==
          "it is under Root, which isn't a Skeleton3D Godot makes of models/figure.gltf, so "
          "Godot's does nothing there");
    CHECK(entity(files, scene, "stray").find("TwoBoneIK3D") == nullptr);
    CHECK(unsupported(plan.report, "node type BoneAttachment3D").find("why")->as_string() ==
          "SlashEngine's BoneAttachment3D works on the model an instance of a model file draws; "
          "add it under that model's Skeleton3D");
    CHECK(entity(files, scene, "grip").find("BoneAttachment3D") == nullptr);
    CHECK(unsupported(plan.report, "node type Skeleton3D").find("why")->as_string() ==
          "a skeleton a scene builds itself doesn't convert yet; one an instance of a model file "
          "brings does");

    // The engine finds every bone the scene names in the model, and the chain the importer
    // reported.
    const slash::world::FileReader reader =
        [&](std::string_view path) -> slash::Result<std::string> {
        if (const auto it = files.find(std::string(path)); it != files.end()) {
            return it->second;
        }
        return slash::platform::read_file((project / path).string());
    };
    slash::world::ComponentRegistry components;
    add_types(components, files);
    CHECK(slash::world::validate_scene(components, parse(files.at(scene)), reader).empty());
    std::vector<std::string> problems;
    for (const json::ValidationError& error :
         slash::scene3d::check_meshes(components, parse(files.at(scene)), reader)) {
        problems.push_back(error.path + ": " + error.message);
    }
    CHECK(problems == std::vector<std::string>{
                          "/entities/spine_ik/components/FABRIK3D/settings/1/end_bone: \"Spine\" "
                          "isn't \"Leg_L\" or under it",
                          "/entities/spine_ik/components/FABRIK3D/settings/1/target: names no "
                          "target: name the entity the end reaches for"});
}

TEST_CASE("importing a Godot project turns each script into a storyboard task") {
    const fs::path project = "import_godot_scripts";
    fs::remove_all(project);
    make_project(project / "godot");
    importer::ImportPlan plan = plan_inside(project);
    auto files = files_of(plan);
    REQUIRE(files.contains("storyboard/work.json"));
    const json::Value work = parse(files.at("storyboard/work.json"));
    CHECK(slash::storyboard::validate_file("work", work).empty());
    const json::Value& tasks = *work.find("entries");
    CHECK(tasks.size() == 3); // enemy.gd, level.gd and the autoload; not the add-on's
    const json::Value& enemy = *tasks.find("task/port-enemy");
    CHECK(enemy.find("status")->as_string() == "open");
    CHECK(*enemy.find("links") == parse(R"(["godot/enemy.gd", "godot/scenes/enemy.scene.json",
                                             "godot/scenes/level.scene.json",
                                             "src/imported/enemy.cpp"])"));
    const std::string& text = enemy.find("text")->as_string();
    CHECK(text.find("Extends RigidBody2D.") != std::string::npos);
    CHECK(text.find("Its C++ skeleton is src/imported/enemy.cpp: the Enemy component and the "
                    "enemy system.") != std::string::npos);
    CHECK(text.find("Exported: health := 3; loot: Array[String] = [].") != std::string::npos);
    CHECK(text.find("Functions: hit(amount: int).") != std::string::npos);
    CHECK(
        tasks.find("task/port-music")->find("text")->as_string().find("An autoload named Music") !=
        std::string::npos);

    // The values each node gave the script's variables, and the signals connected to it.
    const json::Value report = parse(files.at("import/godot.json"));
    const json::Value& uses = *report.find("scripts")->find("godot/enemy.gd")->find("used_by");
    REQUIRE(uses.size() == 3);
    CHECK(uses.as_array().at(1) ==
          parse(R"({"scene": "godot/scenes/level.scene.json", "entity": "enemy",
                    "values": {"health": 5, "loot": ["coin", "gem"]}})"));
    CHECK(*report.find("scripts")->find("godot/level.gd")->find("connections") ==
          parse(R"([{"signal": "pressed", "from": "play", "to": "level",
                     "method": "_on_play_pressed", "scene": "godot/scenes/level.scene.json"}])"));
    CHECK(*report.find("autoloads") == parse(R"({"Music": "godot/autoload/music.gd"})"));

    // Each GDScript file is a C++ skeleton in the gameplay sources, registered by scripts.cpp,
    // which a game module calls; scenes give its entities the component, with their values.
    CHECK(*parse(files.at("project.json")).find("gameplay") == parse(R"({"sources": ["src"]})"));
    const std::string& skeleton = files.at("src/imported/enemy.cpp");
    CHECK(skeleton.find("SLASH_COMPONENT(Enemy, health, loot)") != std::string::npos);
    CHECK(skeleton.find("void hit(World& /*world*/, entt::entity /*entity*/, Enemy& /*self*/, "
                        "int /*amount*/) {\n    // health -= amount\n}") != std::string::npos);
    const std::string& music = files.at("src/imported/music.cpp");
    CHECK(music.find("autoload") != std::string::npos);
    CHECK(music.find("Music") != std::string::npos);
    const std::string& scripts = files.at("src/imported/scripts.cpp");
    CHECK(scripts.find("    register_music(game);\n    register_enemy(game);\n    "
                       "register_level(game);\n") != std::string::npos);
    CHECK(files.at("src/game.cpp") == importer::module_source());
    CHECK(
        *entry(files, "godot/scenes/enemy.scene.json", "enemy").find("components")->find("Enemy") ==
        parse(R"({"health": 3, "loot": ["coin", "gem"]})"));
    // An instance's component holds what its scene's file doesn't: what the level set.
    CHECK(
        *entry(files, "godot/scenes/level.scene.json", "enemy").find("components")->find("Enemy") ==
        parse(R"({"health": 5})"));
    CHECK(
        entry(files, "godot/scenes/level.scene.json", "enemy2").find("components")->find("Enemy") ==
        nullptr);
    CHECK(plan.report.find("module")->as_string() == "src/game.cpp");
    CHECK(plan.report.find("scripts")->as_array().at(1).find("code")->as_string() ==
          "src/imported/enemy.cpp");
    CHECK(report.find("scripts")->find("godot/level.gd")->find("system")->as_string() == "level");

    // Importing again keeps each task's status, other tasks and the project's other settings.
    REQUIRE(importer::import_project(
                {.source = (project / "godot").string(), .destination = project.string()})
                .ok());
    json::Value changed = parse(read(project / "storyboard/work.json"));
    changed["entries"]["task/port-enemy"]["status"] = "done";
    changed["entries"]["task/write-intro"] =
        parse(R"({"name": "Write the intro", "status": "open"})");
    write(project / "storyboard/work.json",
          slash::storyboard::format_file(changed.find("entries")->as_object()));
    json::Value settings = parse(read(project / "project.json"));
    settings["version"] = "2.0.0";
    write(project / "project.json", json::write(settings));

    // The agent's work on a skeleton stays, its component taking the fields it still has, and
    // the game module it now has is left alone.
    write(project / "src/imported/enemy.cpp",
          "struct Enemy { int health = 3; };\nSLASH_COMPONENT(Enemy, health)\n"
          "void register_enemy(slash::world::GameModule& game) {}\n");
    write(project / "src/game.cpp", "SLASH_GAME_MODULE(game) {}\n");

    plan = plan_inside(project);
    files = files_of(plan);
    const json::Value again = parse(files.at("storyboard/work.json"));
    CHECK(again.find("entries")->find("task/port-enemy")->find("status")->as_string() == "done");
    CHECK(again.find("entries")->find("task/write-intro") != nullptr);
    CHECK(again.find("entries")->find("task/port-enemy-2") == nullptr);
    CHECK(parse(files.at("project.json")).find("version")->as_string() == "2.0.0");
    CHECK_FALSE(files.contains("src/imported/enemy.cpp"));
    CHECK_FALSE(files.contains("src/game.cpp"));
    CHECK_FALSE(files.contains("src/imported/level.cpp"));
    CHECK(files.contains("src/imported/scripts.cpp")); // always rewritten
    CHECK(
        *entry(files, "godot/scenes/enemy.scene.json", "enemy").find("components")->find("Enemy") ==
        parse(R"({"health": 3})"));
    CHECK(plan.report.find("scripts")->as_array().at(1).find("kept")->as_bool());
    CHECK(unsupported(plan.report, "game module").find("where")->as_string() == "src/game.cpp");
}

TEST_CASE("a Godot project's C# scripts become C++ skeletons too") {
    const fs::path project = "import_godot_csharp";
    fs::remove_all(project);
    write(project / "godot/project.godot",
          "config_version=5\n\n[application]\n\nconfig/name=\"Hopper\"\n"
          "run/main_scene=\"res://main.tscn\"\n\n[dotnet]\n\nproject/assembly_name=\"Hopper\"\n");
    write(project / "godot/Player.cs",
          "using Godot;\n\npublic partial class Player : CharacterBody2D\n{\n"
          "    [Signal] public delegate void JumpedEventHandler(float height);\n\n"
          "    [Export] public float Speed { get; set; } = 120.0f;\n"
          "    [Export] public int Lives = 3;\n\n"
          "    public override void _PhysicsProcess(double delta)\n    {\n"
          "        MoveAndSlide();\n    }\n\n"
          "    private void OnPlayPressed()\n    {\n"
          "        EmitSignal(SignalName.Jumped, 2.0f);\n    }\n}\n");
    write(project / "godot/main.tscn", R"([gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://Player.cs" id="1_player"]

[node name="Main" type="Node2D"]

[node name="Player" type="CharacterBody2D" parent="."]
script = ExtResource("1_player")
Speed = 300.0

[node name="Play" type="Button" parent="."]

[connection signal="pressed" from="Play" to="Player" method="OnPlayPressed"]
)");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string& skeleton = files.at("src/imported/player.cpp");
    for (const std::string_view part : {
             std::string_view("from the Godot script godot/Player.cs (extends CharacterBody2D)"),
             std::string_view("SLASH_COMPONENT(Player, speed, lives)"),
             std::string_view(
                 "void emit_jumped(World& world, entt::entity entity, float height) {"),
             std::string_view("world.emit(\"Jumped\", world.id_of(entity), std::move(data));"),
             std::string_view("physics_process(world, entity, self, tick_seconds);"),
             std::string_view("if (event.kind == \"pressed\" && event.id == \"play\") {"),
             std::string_view("on_play_pressed(world, entity, *self, event);"),
             std::string_view("    // EmitSignal(SignalName.Jumped, 2.0f);"),
         }) {
        CHECK_MESSAGE(skeleton.find(part) != std::string::npos, part);
    }
    // The node's exported property is its entity's field.
    CHECK(*entry(files, "godot/main.scene.json", "player").find("components")->find("Player") ==
          parse(R"({"speed": 300.0})"));
    CHECK(files.at("src/imported/scripts.cpp").find("register_player(game);") != std::string::npos);
    const json::Value work = parse(files.at("storyboard/work.json"));
    const std::string& text =
        work.find("entries")->find("task/port-player")->find("text")->as_string();
    CHECK(text.find("Its C++ skeleton is src/imported/player.cpp") != std::string::npos);
}

TEST_CASE("a GDScript file reads into a C++ skeleton that calls its functions as Godot does") {
    const std::string text = R"(extends CharacterBody2D
class_name Player
## The hero.

signal hit(damage: int)
signal died

enum State { IDLE, RUN, JUMP = 5 }

const MAX_HEALTH := 100
const BULLET = preload("res://bullet.tscn")

## Pixels a second.
@export var speed: float = 200.0
@export var health: int = MAX_HEALTH
@export var tint: Color = Color(1, 0.5, 0.25)
@export var target: Node2D
@export var allies: Array[Node2D] = []
@export var bullet_scene: PackedScene
@export_range(0, 10) var lives = 3
@export var tags: Array[String] = ["hero", 'player']
@export
var points: PackedVector2Array
var state: State = State.IDLE
var _coins := 0
var history = [1, 2]
@onready var sprite: Sprite2D = $Sprite2D

func _ready() -> void:
	health = MAX_HEALTH

func _physics_process(delta: float) -> void:
	move_and_slide()

func score() -> int: return _coins * 10

func _on_coin_body_entered(body: Node2D) -> void:
	_coins += 1

static func clamp_speed(value: float, limit := 300.0) -> float:
	return min(value, limit)

func _unhandled_input(event: InputEvent) -> void:
	pass

class Bullet:
	var speed = 1
)";
    importer::GdscriptOptions options;
    options.resource_path = [](std::string_view path) {
        return "godot/" + std::string(path.substr(6));
    };
    importer::Script script = importer::read_gdscript("godot/player.gd", text, options);
    CHECK(script.extends == "CharacterBody2D");
    CHECK(script.component == "Player");
    CHECK(script.ns == "player");
    CHECK(script.doc == "The hero.");
    CHECK(script.node);
    REQUIRE(script.fields.size() == 12);
    const auto field = [&](std::string_view name) -> const importer::ScriptField& {
        const auto found = std::ranges::find(script.fields, name, &importer::ScriptField::name);
        REQUIRE_MESSAGE(found != script.fields.end(), name);
        return *found;
    };
    CHECK(field("speed").init == " = 200.0F");
    CHECK(field("speed").doc == "Pixels a second.");
    CHECK(field("health").init == " = kMaxHealth");
    CHECK(field("tint").type == "slash::Vec4");
    CHECK(field("tint").init == "{1.0F, 0.5F, 0.25F, 1.0F}");
    CHECK(field("target").kind == importer::FieldKind::Entity);
    CHECK(field("allies").type == "std::vector<entt::entity>");
    CHECK(field("allies").kind == importer::FieldKind::Entities);
    CHECK(field("bullet_scene").type == "std::string");
    CHECK(field("lives").min == 0.0);
    CHECK(field("lives").max == 10.0);
    CHECK(field("tags").init == R"({"hero", "player"})");
    CHECK(field("points").exported); // its @export is on the line before
    CHECK(field("state").init == " = State::kIdle");
    CHECK_FALSE(field("state").exported);
    CHECK(field("coins").source_name == "_coins");
    CHECK(field("history").comment == "= [1, 2] in Godot");
    REQUIRE(script.constants.size() == 2);
    CHECK(script.constants[1].value == R"("godot/bullet.tscn")");
    REQUIRE(script.enums.size() == 1);
    CHECK(script.enums[0].values == std::vector<std::string>{"kIdle", "kRun", "kJump = 5"});
    REQUIRE(script.signals.size() == 2);
    CHECK(script.signals[0].params.at(0).type == "int");
    REQUIRE(script.functions.size() == 6);
    CHECK(script.functions[2].body == std::vector<std::string>{"return _coins * 10"});
    CHECK(script.functions[4].is_static);
    CHECK(script.functions[4].params.at(1).type == "float");
    CHECK(script.notes.size() == 2); // @onready and the inner class

    script.connections.push_back({.scene = "godot/main.scene.json",
                                  .signal = "body_entered",
                                  .from = "coin",
                                  .to = "player",
                                  .method = "_on_coin_body_entered",
                                  .argument = {}});
    const std::string code = importer::skeleton(script);
    for (const std::string_view part : {
             std::string_view("SLASH_COMPONENT(Player, speed, health, tint, target, allies, "
                              "bullet_scene, lives, tags, points, state, coins, history)"),
             std::string_view("void emit_hit(World& world, entt::entity entity, int damage) {"),
             std::string_view("void physics_process(World& /*world*/, entt::entity /*entity*/, "
                              "Player& /*self*/,\n                     double /*delta*/) {\n"
                              "    // move_and_slide()\n}"),
             std::string_view("float clamp_speed(float /*value*/, float /*limit*/) {"),
             std::string_view("const slash::input::Event& /*event*/"),
             std::string_view("        if (event.kind == \"body_entered\" && event.id == \"coin\") "
                              "{\n            const entt::entity entity = world.find(\"player\");"),
             std::string_view("on_coin_body_entered(world, entity, *self, event);"),
             std::string_view("physics_process(world, entity, self, tick_seconds);"),
             std::string_view(".describe(\"lives\", {.min = 0.0, .max = 10.0})"),
             std::string_view("#include \"slash/input/input.hpp\""),
         }) {
        CHECK_MESSAGE(code.find(part) != std::string::npos, part);
    }
    // A script with no variables still marks its entities, with a tag component.
    const importer::Script empty =
        importer::read_gdscript("main.gd", "extends Node\n\nfunc _process(_delta):\n\tpass\n");
    const std::string tag = importer::skeleton(empty);
    CHECK(tag.find("ecs.view<Main>().each([&](entt::entity entity) {") != std::string::npos);
    CHECK(tag.find("process(world, entity, tick_seconds);") != std::string::npos);
    // One that extends no node is a reflected struct, with no system.
    const importer::Script data =
        importer::read_gdscript("stats.gd", "extends Resource\n\n@export var power := 2\n");
    CHECK_FALSE(data.node);
    CHECK(importer::skeleton(data).find("SLASH_REFLECT(Stats, power)") != std::string::npos);
    CHECK(importer::skeleton(data).find("add_system") == std::string::npos);
    CHECK(importer::registration({"player"}).find("register_player(game);") != std::string::npos);
}

TEST_CASE("a Godot C# script reads as its GDScript would") {
    const std::string text = R"(using Godot;
using System;

public partial class Player : CharacterBody2D
{
    [Signal]
    public delegate void HitEventHandler(int damage);

    [Export] public float Speed { get; set; } = 200.0f;
    [Export(PropertyHint.Range, "0,10")] public int Lives = 3;
    [Export] public Node2D Target;
    [Export] public Color Tint = Colors.Red;
    [Export] public Vector2 Home = Vector2.Zero;
    private int _coins;

    public override void _Ready()
    {
        _coins = 0;
    }

    public override void _PhysicsProcess(double delta)
    {
        MoveAndSlide();
    }

    private void OnCoinBodyEntered(Node2D body)
    {
        _coins += 1;
        EmitSignal(SignalName.Hit, 1);
    }
}
)";
    importer::Script script =
        importer::read_csharp("godot/Player.cs", text, importer::ScriptSource::GodotCsharp);
    CHECK(script.extends == "CharacterBody2D");
    CHECK(script.component == "Player");
    CHECK(script.ns == "player");
    const auto field = [&](std::string_view name) -> const importer::ScriptField& {
        const auto found = std::ranges::find(script.fields, name, &importer::ScriptField::name);
        REQUIRE_MESSAGE(found != script.fields.end(), name);
        return *found;
    };
    CHECK(field("speed").exported);
    CHECK(field("speed").source_name == "Speed");
    CHECK(field("speed").init == " = 200.0F");
    CHECK(field("lives").min == 0.0);
    CHECK(field("lives").max == 10.0);
    CHECK(field("target").kind == importer::FieldKind::Entity);
    CHECK(field("tint").init == "{1.0F, 0.0F, 0.0F, 1.0F}");
    CHECK(field("home").init == "{0.0F, 0.0F}");
    CHECK_FALSE(field("coins").exported);
    CHECK(field("coins").source_name == "_coins");
    REQUIRE(script.signals.size() == 1);
    CHECK(script.signals[0].name == "Hit"); // as Godot names it: SignalName.Hit
    CHECK(script.signals[0].params.at(0).type == "int");
    REQUIRE(script.functions.size() == 3);

    script.connections.push_back({.scene = "godot/main.scene.json",
                                  .signal = "body_entered",
                                  .from = "coin",
                                  .to = "player",
                                  .method = "OnCoinBodyEntered",
                                  .argument = {}});
    const std::string code = importer::skeleton(script);
    MESSAGE(code);
    for (const std::string_view part : {
             std::string_view("SLASH_COMPONENT(Player, speed, lives, target, tint, home, coins)"),
             std::string_view("void emit_hit(World& world, entt::entity entity, int damage) {"),
             std::string_view("physics_process(world, entity, self, tick_seconds);"),
             std::string_view("on_coin_body_entered(world, entity, *self, event);"),
             std::string_view("    // MoveAndSlide();"),
         }) {
        CHECK_MESSAGE(code.find(part) != std::string::npos, part);
    }
}

TEST_CASE("skeleton systems never take the name of a system the engine adds") {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    const command::Session session(components, commands, options);
    const std::vector<std::string_view> systems = session.world().system_names();
    CHECK(systems.size() > 5);
    for (const std::string_view name : systems) {
        CHECK_MESSAGE(importer::is_engine_system(name), name);
    }
    CHECK(importer::cpp_identifier("delete") == "delete_");
    CHECK(importer::pascal_case("enemy_ai") == "EnemyAi");
}

TEST_CASE("a Godot project outside the destination is copied in") {
    const fs::path source = make_project("import_godot_outside_source");
    const fs::path destination = "import_godot_outside_destination";
    fs::remove_all(destination);
    auto result =
        importer::import_project({.source = source.string(), .destination = destination.string()});
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
    CHECK(result.value().report.find("files")->as_int() > 0);
    CHECK(result.value().report.find("main_scene")->as_string() == "scenes/level.scene.json");
    CHECK(fs::exists(destination / "art/enemy.png"));
    CHECK(fs::exists(destination / "enemy.gd"));
    CHECK(fs::exists(destination / "scenes/level.scene.json"));
    CHECK_FALSE(fs::exists(destination / "scenes/level.tscn"));
    CHECK(slash::runtime::load_project(destination.string()).ok());
}

TEST_CASE("only Godot 4 projects import") {
    const fs::path folder = "import_godot_errors";
    fs::remove_all(folder);
    fs::create_directories(folder / "empty");
    auto none = importer::plan_import(
        {.source = (folder / "empty").string(), .destination = folder.string()});
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().message.find("project.godot") != std::string::npos);

    write(folder / "old/project.godot",
          "config_version=4\n\n[application]\n\nconfig/name=\"Old\"\n");
    auto old = importer::plan_import(
        {.source = (folder / "old").string(), .destination = folder.string()});
    REQUIRE_FALSE(old.ok());
    CHECK(old.error().message.find("Godot 3") != std::string::npos);
}

TEST_CASE("project.import converts a Godot project in the session's project") {
    const fs::path project = "import_godot_command";
    fs::remove_all(project);
    make_project(project / "godot");
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    importer::register_import_commands(commands);
    // The game's code, which the import writes: until it is built, its components are stand-ins.
    slash::gameplay::Host code(components, slash::gameplay::library_path(project.string()));
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    options.gameplay = &code;
    command::Session session(components, commands, options);

    const command::Reply dry =
        session.call("project.import", parse(R"({"source": "godot", "dry_run": true})"));
    REQUIRE_MESSAGE(dry.ok, dry.message);
    CHECK(dry.value.find("dry_run")->as_bool());
    CHECK(dry.value.find("files")->size() > 0);
    CHECK_FALSE(fs::exists(project / "project.json"));

    const command::Reply done = session.call("project.import", parse(R"({"source": "godot"})"));
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(done.value.find("files")->as_int() ==
          static_cast<std::int64_t>(dry.value.find("files")->size()));
    CHECK(fs::exists(project / "project.json"));
    CHECK(session.call("scene.open", parse(R"({"path": "godot/scenes/level.scene.json"})")).ok);
    // The enemy's script values, which its instance and its scene's file give it.
    const slash::world::World& opened = session.world();
    REQUIRE(components.find("Enemy") != nullptr);
    CHECK(components.find("Enemy")->get(opened, opened.find("enemy")) ==
          parse(R"({"health": 5, "loot": ["coin", "gem"]})"));
    CHECK(session.call("storyboard.check", parse("{}")).value.find("valid")->as_bool());

    const command::Reply outside =
        session.call("project.import", parse(R"({"source": "../elsewhere"})"));
    CHECK_FALSE(outside.ok);
    CHECK(outside.data.find("path")->as_string() == "/source");
    CHECK_FALSE(session.call("project.import", parse(R"({"source": "nothing"})")).ok);

    options.capabilities = {command::Capability::Read};
    command::Session reader(components, commands, options);
    CHECK_FALSE(reader.call("project.import", parse(R"({"source": "godot"})")).ok);
}
