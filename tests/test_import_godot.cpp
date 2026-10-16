#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/importer/import.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

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

// The components of one entity of a planned scene.
json::Value entity(const std::map<std::string, std::string>& files, const std::string& scene,
                   std::string_view id) {
    REQUIRE(files.contains(scene));
    const json::Value document = parse(files.at(scene));
    const json::Value* found = document.find("entities")->find(id);
    REQUIRE_MESSAGE(found != nullptr, id);
    const json::Value* components = found->find("components");
    return components != nullptr ? *components : json::Value(json::Object{});
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

constexpr std::string_view kProject = R"(; Engine configuration file.

config_version=5

[application]

config/name="Test Game"
run/main_scene="res://scenes/level.tscn"
run/max_fps=120
config/features=PackedStringArray("4.4", "Forward Plus")

[autoload]

Music="*res://autoload/music.gd"

[display]

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
tile_map_data = PackedByteArray(0, 0)

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

[connection signal="pressed" from="UI/Menu/Play" to="." method="_on_play_pressed"]
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
    write(folder / "ui/theme.tres", "[gd_resource type=\"Theme\" format=3]\n\n[resource]\n");
    write(folder / "shaders/glow.gdshader", "shader_type canvas_item;\n");
    write(folder / ".godot/imported/enemy.png-1234.ctex", "binary");
    write(folder / "addons/plugin/plugin.gd", "extends EditorPlugin\n");
    write_png(folder / "art/enemy.png", 16, 16);
    write_png(folder / "art/sheet.png", 32, 16);
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
    CHECK(settings.find("max_fps")->as_int() == 120);
    CHECK(settings.find("tick_hz")->as_double() == 30.0);
    CHECK(settings.find("rendering")->find("texture_filter")->as_string() == "linear");
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
    CHECK(!unsupported(report, "setting gui/theme/custom").is_null());
    CHECK(unsupported(report, "file type .gdshader").find("where")->as_string() ==
          "shaders/glow.gdshader");
    CHECK(unsupported(report, "resource Theme").find("where")->as_string() == "ui/theme.tres");
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
    slash::world::register_builtin_components(components);
    for (const auto& [path, contents] : files) {
        if (path.ends_with(slash::world::kSceneSuffix)) {
            CHECK_MESSAGE(slash::world::validate_scene(components, parse(contents)).empty(), path);
            CHECK(slash::world::format_scene(components, contents).value() == contents);
        }
    }

    // Plain nodes under a plain root keep their children in place, so Godot's drawing order
    // holds; Node2D children hang from their parent's Transform2D.
    CHECK(*entity(files, level, "level").find("Transform2D")->find("children") ==
          parse(R"(["world", "ui"])"));
    CHECK(*entity(files, level, "world").find("Transform2D")->find("children") ==
          parse(R"(["floor", "enemy", "enemy2", "pin", "crate", "torch", "sparks", "tiles"])"));

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

    // An instanced scene is inlined, with its overrides, under the instance's ID.
    const json::Value enemy = entity(files, level, "enemy");
    CHECK(enemy.find("RigidBody2D")->find("mass")->as_double() == 2.0);
    CHECK(*enemy.find("Transform2D")->find("position") == parse("[100.0, 50.0]"));
    CHECK(*enemy.find("Transform2D")->find("children") ==
          parse(R"(["enemy_sprite", "enemy_shape"])"));
    const json::Value sprite = entity(files, level, "enemy_sprite");
    CHECK(sprite.find("Sprite2D")->find("flip_h")->as_bool());
    CHECK(sprite.find("Sprite2D")->find("texture")->as_string() == "godot/art/enemy.png");
    CHECK(*sprite.find("Sprite2D")->find("region") == parse("[0.0, 0.0, 16.0, 16.0]"));
    CHECK(sprite.find("Sprite2D")->find("texture_filter")->as_string() == "nearest");
    CHECK(*sprite.find("Sprite2D")->find("modulate") == parse("[1.0, 0.5, 0.5, 0.5]"));
    CHECK(entity(files, level, "enemy2_sprite").find("Sprite2D")->find("flip_h") == nullptr);
    CHECK(
        entity(files, level, "enemy_shape").find("CollisionShape2D")->find("shape")->as_string() ==
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
          parse(R"([{"offset": 0.0, "color": [1.0, 1.0, 0.0, 1.0]},
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
    CHECK(*play.find("Button")->find("button_mask") == parse(R"(["left", "right"])"));
    CHECK(play.find("Button")->find("alignment") == nullptr); // fill has no equivalent
    CHECK(*play.find("Control")->find("theme_override_styles")->find("normal") ==
          parse(R"({"type": "flat", "bg_color": [0.1, 0.1, 0.2, 0.9],
                    "corner_radius_top_left": 6, "border_width_bottom": 2})"));
    CHECK(*entity(files, level, "volume").find("Slider") ==
          parse(R"({"max_value": 10.0, "vertical": true})"));

    // What doesn't convert is reported once by kind, and kept in the report for porting.
    CHECK(unsupported(plan.report, "node type TileMapLayer").find("where")->as_string() ==
          "scenes/level.tscn: World/Tiles");
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
                                             "godot/scenes/level.scene.json"])"));
    const std::string& text = enemy.find("text")->as_string();
    CHECK(text.find("Extends RigidBody2D.") != std::string::npos);
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

    plan = plan_inside(project);
    files = files_of(plan);
    const json::Value again = parse(files.at("storyboard/work.json"));
    CHECK(again.find("entries")->find("task/port-enemy")->find("status")->as_string() == "done");
    CHECK(again.find("entries")->find("task/write-intro") != nullptr);
    CHECK(again.find("entries")->find("task/port-enemy-2") == nullptr);
    CHECK(parse(files.at("project.json")).find("version")->as_string() == "2.0.0");
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
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
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
