#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/importer/import.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <webp/encode.h>

namespace json = slash::json;
namespace command = slash::command;
namespace godot = slash::importer::godot;
namespace importer = slash::importer;
namespace fs = std::filesystem;

namespace {

using Files = std::map<std::string, std::string>;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

template <typename T>
T unwrap(slash::Result<T> result) {
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
    return std::move(result).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    return unwrap(slash::platform::read_file(path.string()));
}

// Little-endian numbers, for the files the tests lay out by hand.
void put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

std::uint32_t byte_at(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint8_t>(bytes[at]);
}

std::uint32_t u32_at(std::string_view bytes, std::size_t at) {
    return byte_at(bytes, at) | (byte_at(bytes, at + 1) << 8U) | (byte_at(bytes, at + 2) << 16U) |
           (byte_at(bytes, at + 3) << 24U);
}

godot::UnpackedGame unpack(const Files& files, const godot::PackOptions& options = {}) {
    return unwrap(godot::unpack_game(godot::write_pack(files, options)));
}

// A game exported from a project's files, unpacked again.
godot::UnpackedGame round_trip(const Files& project, const godot::ExportOptions& options = {}) {
    return unpack(unwrap(godot::export_project(project, options)));
}

// A .tscn or .tres file as Godot's text saver lays it out.
std::string canonical(std::string_view text) {
    return godot::write_resource(unwrap(godot::parse_resource(text)));
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

std::int64_t count(const json::Value& report, std::string_view key) {
    return report.find(key)->as_int();
}

// --- Textures and sounds laid out as Godot's importers write them ---

// A CompressedTexture2D file holding one image: PNG or WebP bytes, or raw pixels in one of
// Godot's Image formats.
std::string ctex(std::uint32_t width, std::uint32_t height, std::uint32_t data_format,
                 std::uint32_t format, std::string_view data) {
    std::string out = "GST2";
    put32(out, 1);
    put32(out, width);
    put32(out, height);
    put32(out, 0x0D000000);
    put32(out, 0xFFFFFFFF);
    for (int i = 0; i < 3; ++i) {
        put32(out, 0);
    }
    put32(out, data_format);
    put16(out, width);
    put16(out, height);
    put32(out, 0); // mipmaps
    put32(out, format);
    if (data_format != 0) {
        put32(out, static_cast<std::uint32_t>(data.size()));
    }
    out += data;
    return out;
}

// An imported asset's two files in a pack: its .import file and the file it names.
Files imported(const std::string& path, std::string_view type, std::string_view extension,
               std::string data) {
    const std::string target =
        ".godot/imported/" + path.substr(path.rfind('/') + 1) + "-0" + std::string(extension);
    return {{path + ".import", "[remap]\n\nimporter=\"x\"\ntype=\"" + std::string(type) +
                                   "\"\nuid=\"uid://basset\"\npath=\"res://" + target + "\"\n"},
            {target, std::move(data)}};
}

// The pixels of the PNG file a texture came back as.
slash::render::Image pixels(const godot::UnpackedGame& game, const std::string& path) {
    REQUIRE_MESSAGE(game.files.contains(path), path);
    return unwrap(slash::render::decode_image(game.files.at(path)));
}

std::array<std::uint8_t, 4> pixel(const slash::render::Image& image, std::uint32_t x,
                                  std::uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// A PCM WAV file, with its loop (Godot's loop mode, begin and end) in a "smpl" chunk.
std::string wav(std::uint32_t channels, std::uint32_t rate, std::uint32_t bits,
                std::string_view samples, std::array<std::uint32_t, 3> loop = {}) {
    std::string chunks = "fmt ";
    put32(chunks, 16);
    put16(chunks, 1);
    put16(chunks, channels);
    put32(chunks, rate);
    put32(chunks, rate * channels * bits / 8);
    put16(chunks, channels * bits / 8);
    put16(chunks, bits);
    chunks += "data";
    put32(chunks, static_cast<std::uint32_t>(samples.size()));
    chunks += samples;
    if (loop[0] > 0) {
        chunks += "smpl";
        put32(chunks, 60);
        for (int i = 0; i < 7; ++i) {
            put32(chunks, 0);
        }
        put32(chunks, 1);
        put32(chunks, 0);
        put32(chunks, 0);
        put32(chunks, loop[0] - 1);
        put32(chunks, loop[1]);
        put32(chunks, loop[2]);
        put32(chunks, 0);
        put32(chunks, 0);
    }
    std::string out = "RIFF";
    put32(out, static_cast<std::uint32_t>(chunks.size() + 4));
    out += "WAVE" + chunks;
    return out;
}

// A WAV file's 16-bit samples.
std::vector<std::int16_t> samples_of(std::string_view file) {
    const std::size_t at = file.find("data");
    REQUIRE(at != std::string_view::npos);
    const std::uint32_t size = u32_at(file, at + 4);
    std::vector<std::int16_t> samples;
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        samples.push_back(static_cast<std::int16_t>(byte_at(file, at + 8 + i) |
                                                    (byte_at(file, at + 9 + i) << 8U)));
    }
    return samples;
}

// An AudioStreamWAV resource (.sample) holding `data`, with more properties after it.
std::string sample(std::string_view data, std::string_view properties) {
    const std::string text = "[gd_resource type=\"AudioStreamWAV\" format=3]\n\n[resource]\n"
                             "data = PackedByteArray(\"" +
                             slash::base64_encode(data) + "\")\n" + std::string(properties);
    return unwrap(godot::write_binary_resource(unwrap(godot::parse_resource(text))));
}

// Ogg's page checksum, to check the pages a sound comes back in.
std::uint32_t ogg_crc(std::string_view bytes) {
    std::uint32_t crc = 0;
    for (const char byte : bytes) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(byte)) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
        }
    }
    return crc;
}

struct OggPage {
    std::uint32_t type = 0;
    std::int64_t granule = 0;
    std::vector<std::uint32_t> lacing;
    std::string body;
};

// An Ogg file's pages, checking each one's sequence number and checksum.
std::vector<OggPage> ogg_pages(std::string_view file) {
    std::vector<OggPage> pages;
    std::size_t at = 0;
    std::uint32_t sequence = 0;
    while (at < file.size()) {
        REQUIRE(file.substr(at, 4) == "OggS");
        OggPage page;
        page.type = byte_at(file, at + 5);
        for (std::size_t i = 0; i < 8; ++i) {
            page.granule = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(page.granule) |
                (static_cast<std::uint64_t>(byte_at(file, at + 6 + i)) << (8U * i)));
        }
        CHECK(u32_at(file, at + 18) == sequence++);
        const std::uint32_t segments = byte_at(file, at + 26);
        std::size_t body = 0;
        for (std::uint32_t i = 0; i < segments; ++i) {
            page.lacing.push_back(byte_at(file, at + 27 + i));
            body += page.lacing.back();
        }
        const std::size_t size = 27 + segments + body;
        std::string bytes(file.substr(at, size));
        bytes.replace(22, 4, std::string(4, '\0'));
        CHECK(ogg_crc(bytes) == u32_at(file, at + 22));
        page.body = std::string(file.substr(at + 27 + segments, body));
        pages.push_back(std::move(page));
        at += size;
    }
    return pages;
}

} // namespace

TEST_CASE("Godot packs hold their files in each pack format, alone or inside an executable") {
    const Files files = {
        {"notes.txt", "hello"}, {"data/level.json", std::string(37, 'x')}, {"empty.cfg", ""}};
    for (const std::uint32_t format : {2U, 3U, 4U}) {
        INFO(format);
        const auto game = unpack(files, {.format = format, .minor = format == 2 ? 4U : 5U});
        CHECK(game.files == files);
        CHECK(game.report.find("pack")->find("format")->as_int() == format);
        CHECK_FALSE(game.report.find("pack")->find("embedded")->as_bool());
    }
    CHECK(unpack(files).report.find("pack")->find("godot")->as_string() == "4.5.0");

    // The magic can turn up in an executable's code: the pack is found by its size at the end,
    // or, with nothing after it, by a header that makes sense.
    const std::string executable = "\x7F"
                                   "ELF GDPC" +
                                   std::string(1001, '\x90');
    const auto embedded = unpack(files, {.executable = executable});
    CHECK(embedded.files == files);
    CHECK(embedded.report.find("pack")->find("embedded")->as_bool());
    const auto in_section =
        unwrap(godot::unpack_game(executable + godot::write_pack(files) + "more sections"));
    CHECK(in_section.files == files);
}

TEST_CASE("packs that can't be unpacked are refused, and files that can't are listed") {
    const Files files = {{"a.txt", "a"}};
    const auto refused = [&](const godot::PackOptions& options) {
        auto game = godot::unpack_game(godot::write_pack(files, options));
        REQUIRE_FALSE(game.ok());
        return game.error();
    };
    const slash::Error encrypted = refused({.flags = 1});
    CHECK(encrypted.code == slash::ErrorCode::PermissionDenied);
    CHECK(contains(encrypted.message, "encrypted with the game's own key"));
    CHECK(contains(refused({.flags = 4}).message, "sparse pack"));
    CHECK(contains(refused({.major = 3}).message, "Godot 3.5.0"));
    CHECK(contains(refused({.format = 5}).message, "newer than this reads"));
    auto nothing = godot::unpack_game("MZ not a game");
    REQUIRE_FALSE(nothing.ok());
    CHECK(contains(nothing.error().message, "no Godot pack"));
    const std::string cut = godot::write_pack({{"big.bin", std::string(64, 'b')}});
    CHECK_FALSE(godot::unpack_game(std::string_view(cut).substr(0, 120)).ok());

    const auto game =
        unpack({{"keep.txt", "k"},
                {"secret.txt", "s"},
                {"gone.txt", "g"},
                {"patch.txt", "p"},
                {"enemy.gde", "e"}},
               {.file_flags = {{"secret.txt", 1}, {"gone.txt", 2}, {"patch.txt", 4}}});
    CHECK(game.files == Files{{"keep.txt", "k"}});
    CHECK(unsupported(game.report, "encrypted file").find("where")->as_string() == "secret.txt");
    CHECK(unsupported(game.report, "patch file").find("where")->as_string() == "patch.txt");
    CHECK(unsupported(game.report, "encrypted script").find("where")->as_string() == "enemy.gde");
}

TEST_CASE("binary resources read back into the text resources they were written from") {
    const std::string stats =
        R"([gd_resource type="Resource" script_class="Stats" load_steps=5 format=3 uid="uid://cstats0001"]

[ext_resource type="Script" uid="uid://bscript0001" path="res://stats.gd" id="1"]
[ext_resource type="Texture2D" path="res://icon.png" id="2"]

[sub_resource type="Gradient" id="Gradient_a"]
offsets = PackedFloat32Array(0, 0.25, 1)
colors = PackedColorArray(1, 0, 0, 1, 0, 0, 1, 0.5)

[sub_resource type="GradientTexture1D" id="GradientTexture1D_b"]
gradient = SubResource("Gradient_a")
width = 64

[resource]
script = ExtResource("1")
yes = true
small = -7
large = 5000000000
half = 0.5
tenth = 0.1
text = "tab	quote\" é"
name = &"walk"
path = NodePath("Player/Sprite:modulate:a")
absolute = NodePath("/root/Main")
v2 = Vector2(1.5, -2)
v2i = Vector2i(3, 4)
rect = Rect2(0, 0, 8, 8)
recti = Rect2i(1, 2, 3, 4)
v3 = Vector3(1, 2, 3)
v3i = Vector3i(-1, 0, 1)
t2d = Transform2D(1, 0, 0, 1, 10, 20)
v4 = Vector4(1, 2, 3, 4)
v4i = Vector4i(5, 6, 7, 8)
plane = Plane(0, 1, 0, 5)
quat = Quaternion(0, 0, 0, 1)
box = AABB(0, 0, 0, 1, 2, 3)
basis = Basis(1, 0, 0, 0, 1, 0, 0, 0, 1)
t3d = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 4, 5, 6)
proj = Projection(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
color = Color(0.1, 0.2, 0.3, 1)
list = [1, "two", 3.0, [4], ExtResource("2")]
map = {
"a": 1,
&"b": [2],
3: Vector2(1, 1),
"texture": SubResource("GradientTexture1D_b")
}
bytes = PackedByteArray("AQID")
ints = PackedInt32Array(1, -2)
longs = PackedInt64Array(5000000000)
floats = PackedFloat32Array(0.5)
doubles = PackedFloat64Array(0.1)
strings = PackedStringArray("a", "bc", "")
points = PackedVector2Array(1, 2, 3, 4)
points3 = PackedVector3Array(1, 2, 3)
colors = PackedColorArray(1, 1, 1, 1)
points4 = PackedVector4Array(1, 2, 3, 4)
after = 1
)";
    // A sub-resource nothing uses is left out, as Godot's saver leaves it out, and so is an empty
    // property, which Godot's text saver leaves out of objects and the binary one keeps.
    std::string with_unused = stats;
    with_unused.insert(with_unused.find("[resource]"),
                       "[sub_resource type=\"Gradient\" id=\"Gradient_unused\"]\n\n");
    with_unused.insert(with_unused.find("yes = true"), "material = null\n");
    const auto game = round_trip({{"stats.tres", with_unused}});
    CHECK(game.files.at("stats.tres") == canonical(stats));
    CHECK(count(game.report, "resources") == 1);
    CHECK(game.report.find("unsupported")->size() == 0);

    // Compressed as ResourceSaver's FLAG_COMPRESS writes, under a .res name.
    const std::string compressed = unwrap(godot::write_binary_resource(
        unwrap(godot::parse_resource(stats)), {.path = "res://stats.tres", .compress = true}));
    CHECK(compressed.starts_with("RSCC"));
    const auto plain = unpack({{"stats.res", compressed}});
    CHECK(plain.files.at("stats.tres") == canonical(stats));
    CHECK(plain.report.find("renamed")->find("stats.res")->as_string() == "stats.tres");

    // Typed arrays lose their type in binary resources, as in Godot's.
    const auto typed = round_trip({{"typed.tres", "[gd_resource type=\"Resource\" format=3]\n\n"
                                                  "[resource]\nlist = Array[int]([1, 2])\n"}});
    CHECK(contains(typed.files.at("typed.tres"), "list = [1, 2]\n"));

    CHECK_FALSE(godot::write_binary_resource(
                    unwrap(godot::parse_resource("[gd_resource type=\"Resource\" format=3]\n\n"
                                                 "[resource]\nv = Vector2(1)\n")))
                    .ok());
}

TEST_CASE("binary scenes read back into the scenes they were packed from") {
    const std::string level = R"([gd_scene load_steps=4 format=3 uid="uid://blevel00001"]

[ext_resource type="Script" path="res://level.gd" id="1"]
[ext_resource type="PackedScene" uid="uid://bhero000001" path="res://hero.tscn" id="2"]

[sub_resource type="RectangleShape2D" id="RectangleShape2D_floor"]
size = Vector2(64, 8)

[node name="Level" type="Node2D"]
script = ExtResource("1")
target = NodePath("Hero")
targets = [NodePath("Floor"), NodePath("Floor/Shape")]

[node name="Hero" parent="." instance=ExtResource("2")]
position = Vector2(10, 20)

[node name="Hat" type="Sprite2D" parent="Hero/Head" index="0"]

[node name="Floor" type="StaticBody2D" parent="." groups=["ground", "solid"]]

[node name="Shape" type="CollisionShape2D" parent="Floor"]
shape = SubResource("RectangleShape2D_floor")

[node name="Score" type="Label" parent="."]
unique_name_in_owner = true
text = "0"

[connection signal="body_entered" from="Floor" to="." method="_on_floor_body_entered" flags=3 binds=[1, "x"]]
[connection signal="timeout" from="Hero/Timer" to="." method="_on_timeout" unbinds=1]
[connection signal="ready" from="." to="Score" method="show"]

[editable path="Hero"]
)";
    const std::string inherited = R"([gd_scene load_steps=2 format=3]

[ext_resource type="PackedScene" path="res://hero.tscn" id="1"]

[node name="Hero" instance=ExtResource("1")]
speed = 2.0

[node name="Cape" type="Sprite2D" parent="Body"]

[node name="Spark" parent="." instance_placeholder="res://spark.tscn"]
)";
    const auto game = round_trip({{"level.tscn", level}, {"boss.tscn", inherited}});
    CHECK(game.files.at("level.tscn") == canonical(level));
    CHECK(game.files.at("boss.tscn") == canonical(inherited));
    CHECK(count(game.report, "scenes") == 2);

    // Exported with Godot's "convert text resources to binary" turned off, they stay as they are.
    const auto text = round_trip({{"level.tscn", level}}, {.binary_resources = false});
    CHECK(text.files.at("level.tscn") == level);
}

TEST_CASE("project.binary reads back into project.godot") {
    const std::string project = R"(config_version=5

[rendering]

textures/canvas_textures/default_texture_filter=0

[application]

config/name="Pack Test"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.5", "GL Compatibility")

[input]

jump={
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":0,"physical_keycode":32,"key_label":0,"unicode":32,"location":0,"echo":false,"script":null)
]
}
)";
    const auto game = round_trip({{"project.godot", project}});
    // Sections come back in name order, as Godot saves them.
    const std::string& back = game.files.at("project.godot");
    CHECK(contains(back, "config_version=5\n"));
    CHECK(back.find("[application]") < back.find("[input]"));
    CHECK(back.find("[input]") < back.find("[rendering]"));
    CHECK(contains(back, "config/name=\"Pack Test\"\n"));
    CHECK(contains(back, "\"physical_keycode\":32,"));
    CHECK(godot::write_project_settings(unwrap(godot::parse_project_settings(back))) == back);
}

TEST_CASE("GDScript tokens turn back into source with every line where it was") {
    const std::string script = "@tool\n"
                               "class_name Mover\n"
                               "extends CharacterBody2D\n"
                               "## A doc comment.\n"
                               "\n"
                               "signal hit(damage: int)\n"
                               "\n"
                               "enum State { IDLE, RUN = 4, JUMP }\n"
                               "const SPEED := 120.5\n"
                               "const NAMES: Array[String] = [\"a\", \"b\"]\n"
                               "\n"
                               "@export var health: int = 0x1F\n"
                               "@export_range(0, 1_000) var mask := 0b1010\n"
                               "@onready var sprite: Sprite2D = $Sprite2D\n"
                               "@onready var score = %Score\n"
                               "var path := ^\"Body/Shape\"\n"
                               "var tag := &\"enemy\"\n"
                               "var big := 1.5e3\n"
                               "var text := \"tab\\tquote\\\" \xC3\xA9\"\n"
                               "var raw := r\"C:\\games\"\n"
                               "var multi := \"\"\"first\n"
                               "second\"\"\"\n"
                               "var caf\xC3\xA9 := 1\n"
                               "\n"
                               "func _ready() -> void: # starts here\n"
                               "\tif health > 10 and not is_queued_for_deletion():\n"
                               "\t\tprint(\"ok\", health)\n"
                               "\telif health == 0:\n"
                               "\t\tpass\n"
                               "\telse:\n"
                               "\t\tfor i in range(3):\n"
                               "\t\t\tmatch i:\n"
                               "\t\t\t\t0, 1:\n"
                               "\t\t\t\t\tcontinue\n"
                               "\t\t\t\tvar other when other > 1:\n"
                               "\t\t\t\t\tbreak\n"
                               "\tvar total = (health\n"
                               "\t\t+ mask\n"
                               "\t\t- 2)\n"
                               "\tvar sum := 1 + \\\n"
                               "\t\t2\n"
                               "\tvar add := func(a, b): return a + b\n"
                               "\tawait get_tree().process_frame\n"
                               "\thit.emit(add.call(total, sum))\n"
                               "\n"
                               "\n"
                               "func _physics_process(delta: float) -> void:\n"
                               "\tvelocity.x = Input.get_axis(&\"left\", &\"right\") * SPEED\n"
                               "\tmove_and_slide()\n";
    const std::string tokens = unwrap(godot::tokenize_gdscript(script));
    CHECK(tokens.starts_with("GDSC"));
    const std::string source = unwrap(godot::detokenize_gdscript(tokens));
    // The source tokenizes to the same tokens: each token is on its line and column.
    CHECK(unwrap(godot::tokenize_gdscript(source)) == tokens);
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < source.size();) {
        const std::size_t end = source.find('\n', at);
        lines.push_back(source.substr(at, end - at));
        at = end + 1;
    }
    REQUIRE(lines.size() >= 49);
    CHECK(lines[24].starts_with("func _ready() -> void:"));
    CHECK(lines[25].starts_with("\tif health > 10 and not is_queued_for_deletion():"));
    CHECK(lines[35] == "\t\t\t\t\tbreak");
    CHECK(lines[39] == "\tvar sum := 1 + \\"); // continued after a backslash
    CHECK(lines[40] == "\t\t2");
    CHECK(lines[48] == "\tmove_and_slide()");
    CHECK_FALSE(contains(source, "starts here"));

    // Plain and compressed tokens, and both versions, read back the same.
    CHECK(unwrap(godot::detokenize_gdscript(
              unwrap(godot::tokenize_gdscript(script, {.compress = false})))) == source);
    CHECK(unwrap(godot::detokenize_gdscript(
              unwrap(godot::tokenize_gdscript(script, {.version = 100})))) == source);
    // Godot 4.7 counts a tab as one column.
    CHECK(unwrap(godot::detokenize_gdscript(
              unwrap(godot::tokenize_gdscript(script, {.tab_size = 1})), 1)) == source);

    // Godot 4.3 and 4.4's tokens have no "...", which variadic functions need.
    const std::string variadic = "func f(...args):\n\tpass\n";
    CHECK(godot::tokenize_gdscript(variadic).ok());
    CHECK_FALSE(godot::tokenize_gdscript(variadic, {.version = 100}).ok());

    auto broken = godot::tokenize_gdscript("extends Node\n\nvar s = \"unterminated\n");
    REQUIRE_FALSE(broken.ok());
    CHECK(contains(broken.error().message, "line 3"));
    CHECK_FALSE(godot::detokenize_gdscript("GDSC").ok());
}

TEST_CASE("exported scripts come back as source") {
    const Files project = {{"player.gd", "extends Node2D\n\nfunc _ready():\n\tprint(\"hi\")\n"},
                           {"player.gd.uid", "uid://bplayer0001\n"},
                           {"tools/raw.gd", "extends Node\n"}};
    const auto game = round_trip(project);
    CHECK(game.files.at("player.gd") == project.at("player.gd"));
    CHECK(game.files.at("player.gd.uid") == "uid://bplayer0001\n");
    CHECK(count(game.report, "scripts") == 2);
    CHECK(count(game.report, "scripts_from_tokens") == 2);
    const auto source = round_trip(project, {.script_tokens = false});
    CHECK(source.files.at("player.gd") == project.at("player.gd"));
    CHECK(source.report.find("scripts_from_tokens") == nullptr);
}

TEST_CASE("imported textures come back as PNG files") {
    slash::render::Image image{.width = 3, .height = 2, .pixels = {}};
    for (std::uint8_t i = 0; i < 24; ++i) {
        image.pixels.push_back(static_cast<std::uint8_t>(i * 10));
    }
    const std::string png = unwrap(slash::render::encode_png(image));
    const auto exported = round_trip({{"art/hero.png", png}});
    CHECK(pixels(exported, "art/hero.png").pixels == image.pixels);
    CHECK(exported.files.contains("art/hero.png.import"));
    CHECK(count(exported.report, "textures") == 1);

    std::uint8_t* webp_bytes = nullptr;
    const std::size_t webp_size =
        WebPEncodeLosslessRGBA(image.pixels.data(), 3, 2, 12, &webp_bytes);
    REQUIRE(webp_size > 0);
    const std::string webp(reinterpret_cast<const char*>(webp_bytes), webp_size);
    WebPFree(webp_bytes);

    Files files;
    const auto add = [&](const std::string& path, std::string data) {
        files.merge(imported(path, "CompressedTexture2D", ".ctex", std::move(data)));
    };
    add("webp.png", ctex(3, 2, 2, 5, webp));
    add("l8.png", ctex(2, 1, 0, 0, "\x40\xC0"));
    add("la8.png", ctex(1, 1, 0, 1, std::string("\x10\x80", 2)));
    add("rgb8.png", ctex(1, 1, 0, 4, "\x01\x02\x03"));
    add("rgba4444.png", ctex(1, 1, 0, 6, std::string("\x84\xF2", 2))); // 0xF284
    add("rgb565.png", ctex(1, 1, 0, 7, std::string("\x00\xF8", 2)));   // red
    // A 5x3 texture in two BC1 blocks: two colors and their mixes, then three colors and
    // transparent pixels.
    std::string bc1;
    put16(bc1, 0xF800);     // red
    put16(bc1, 0x001F);     // blue
    put32(bc1, 0xE4E4E4E4); // colors 0, 1, 2 and 3 across each row
    put16(bc1, 0x001F);
    put16(bc1, 0xF800); // the first color no greater than the second: index 3 is transparent
    put32(bc1, 0xFFFFFFFF);
    add("bc1.png", ctex(5, 3, 0, 17, bc1));
    std::string bc3 = {'\xFF', '\x00'};                // alpha 255 down to 0
    bc3 += std::string("\x08\x82\x20\x08\x82\x20", 6); // alpha indices 0 and 1 alternating
    put16(bc3, 0x07E0);                                // green
    put16(bc3, 0x07E0);
    put32(bc3, 0);
    add("bc3.png", ctex(4, 4, 0, 19, bc3));
    add("basis.png", ctex(4, 4, 3, 0, "basis"));
    const auto game = unpack(files);

    using Rgba = std::array<std::uint8_t, 4>;
    CHECK(pixels(game, "webp.png").pixels == image.pixels);
    CHECK(pixel(pixels(game, "l8.png"), 1, 0) == Rgba{0xC0, 0xC0, 0xC0, 255});
    CHECK(pixel(pixels(game, "la8.png"), 0, 0) == Rgba{16, 16, 16, 128});
    CHECK(pixel(pixels(game, "rgb8.png"), 0, 0) == Rgba{1, 2, 3, 255});
    CHECK(pixel(pixels(game, "rgba4444.png"), 0, 0) == Rgba{255, 34, 136, 68});
    CHECK(pixel(pixels(game, "rgb565.png"), 0, 0) == Rgba{255, 0, 0, 255});
    const auto bc1_image = pixels(game, "bc1.png");
    CHECK(bc1_image.width == 5);
    CHECK(bc1_image.height == 3);
    CHECK(pixel(bc1_image, 0, 0) == Rgba{255, 0, 0, 255});
    CHECK(pixel(bc1_image, 1, 0) == Rgba{0, 0, 255, 255});
    CHECK(pixel(bc1_image, 2, 0) == Rgba{170, 0, 85, 255});
    CHECK(pixel(bc1_image, 3, 2) == Rgba{85, 0, 170, 255});
    CHECK(pixel(bc1_image, 4, 0)[3] == 0);
    const auto bc3_image = pixels(game, "bc3.png");
    CHECK(pixel(bc3_image, 0, 0) == Rgba{0, 255, 0, 255});
    CHECK(pixel(bc3_image, 1, 0) == Rgba{0, 255, 0, 0});
    CHECK(count(game.report, "textures") == 8);
    CHECK(unsupported(game.report, "unreadable CompressedTexture2D").find("where")->as_string() ==
          "basis.png");
}

TEST_CASE("imported sounds and fonts come back as the files they were") {
    std::string pcm16;
    for (const int value : {0, 1000, -1000, 32767, -32768, 7}) {
        put16(pcm16, static_cast<std::uint16_t>(value));
    }
    const std::string loop = wav(2, 22050, 16, pcm16, {2, 1, 3});
    const std::string bytes8 = wav(1, 8000, 8, std::string("\x00\x7F\x80\xFF", 4));
    const std::string mp3 = "ID3\x04 not really an MP3 file";
    const std::string font = std::string("\x00\x01\x00\x00", 4) + " not really a font";
    const auto game = round_trip({{"sfx/loop.wav", loop},
                                  {"sfx/blip.wav", bytes8},
                                  {"music/theme.mp3", mp3},
                                  {"fonts/title.ttf", font}});
    CHECK(game.files.at("sfx/loop.wav") == loop);
    CHECK(game.files.at("sfx/blip.wav") == bytes8);
    CHECK(game.files.at("music/theme.mp3") == mp3);
    CHECK(game.files.at("fonts/title.ttf") == font);
    CHECK(count(game.report, "sounds") == 3);
    CHECK(count(game.report, "fonts") == 1);
    CHECK_FALSE(game.files.at("sfx/loop.wav.import").ends_with('\0'));

    Files files;
    // IMA-ADPCM: nibble 7 then 0, from a predictor of 0 at step 7.
    files.merge(imported("adpcm.wav", "AudioStreamWAV", ".sample",
                         sample("\x07", "format = 2\nmix_rate = 11025\n")));
    // QOA: one frame of one sample, scale factor 7 and a residual of 0.75 of it.
    std::string qoa = "qoaf";
    const auto be16 = [&](std::uint32_t value) {
        qoa += static_cast<char>((value >> 8U) & 0xFFU);
        qoa += static_cast<char>(value & 0xFFU);
    };
    be16(0);
    be16(1);                               // samples in the file
    qoa += '\x01';                         // channels
    qoa += std::string("\x00\xAC\x44", 3); // 44100 Hz
    be16(1);                               // samples in the frame
    be16(8 + 16 + 8);                      // frame size
    for (int i = 0; i < 8; ++i) {
        be16(0); // history and weights
    }
    qoa += std::string("\x10\x00\x00\x00\x00\x00\x00\x00", 8);
    files.merge(imported("qoa.wav", "AudioStreamWAV", ".sample", sample(qoa, "format = 3\n")));

    // Ogg Vorbis: pages of packets, one packet long enough to need two pages.
    const std::vector<std::vector<std::string>> pages = {{"\x01vorbis-id"},
                                                         {"\x03vorbis-comment", "\x05vorbis-setup"},
                                                         {std::string(70000, 'a'), "short"}};
    std::string packet_data = "packet_data = [";
    for (std::size_t p = 0; p < pages.size(); ++p) {
        packet_data += p == 0 ? "[" : ", [";
        for (std::size_t k = 0; k < pages[p].size(); ++k) {
            packet_data += (k == 0 ? "PackedByteArray(\"" : ", PackedByteArray(\"") +
                           slash::base64_encode(pages[p][k]) + "\")";
        }
        packet_data += "]";
    }
    packet_data += "]\n";
    const std::string ogg_text =
        "[gd_resource type=\"AudioStreamOggVorbis\" load_steps=2 format=3]\n\n"
        "[sub_resource type=\"OggPacketSequence\" id=\"OggPacketSequence_a\"]\n" +
        packet_data +
        "granule_positions = PackedInt64Array(0, 0, 4410)\nsampling_rate = 44100.0\n\n"
        "[resource]\npacket_sequence = SubResource(\"OggPacketSequence_a\")\n";
    files.merge(
        imported("music/song.ogg", "AudioStreamOggVorbis", ".oggvorbisstr",
                 unwrap(godot::write_binary_resource(unwrap(godot::parse_resource(ogg_text))))));
    const auto sounds = unpack(files);

    CHECK(samples_of(sounds.files.at("adpcm.wav")) == std::vector<std::int16_t>{11, 13});
    CHECK(samples_of(sounds.files.at("qoa.wav")) == std::vector<std::int16_t>{5});
    const auto ogg = ogg_pages(sounds.files.at("music/song.ogg"));
    REQUIRE(ogg.size() == 4);
    CHECK(ogg[0].type == 2); // the first page
    CHECK(ogg[2].type == 0);
    CHECK(ogg[2].lacing.size() == 255);
    CHECK(ogg[2].granule == -1); // no packet ends on it
    CHECK(ogg[3].type == 5);     // continued, and the last page
    CHECK(ogg[3].granule == 4410);
    // The packets read back from the pages' lacing values.
    std::vector<std::string> packets;
    std::string packet;
    for (const OggPage& page : ogg) {
        std::size_t at = 0;
        for (const std::uint32_t size : page.lacing) {
            packet += page.body.substr(at, size);
            at += size;
            if (size < 255) {
                packets.push_back(std::move(packet));
                packet.clear();
            }
        }
    }
    CHECK(packets == std::vector<std::string>{pages[0][0], pages[1][0], pages[1][1], pages[2][0],
                                              pages[2][1]});
    CHECK(count(sounds.report, "sounds") == 3);
}

TEST_CASE("importing an exported game converts what importing its project does") {
    const Files project = {
        {"project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"Packed\"\n"
                          "run/main_scene=\"res://level.tscn\"\n\n[display]\n\n"
                          "window/size/viewport_width=320\nwindow/size/viewport_height=180\n"},
        {"level.tscn", R"([gd_scene load_steps=3 format=3 uid="uid://blevel00001"]

[ext_resource type="Script" path="res://level.gd" id="1"]
[ext_resource type="Texture2D" uid="uid://bhero000001" path="res://art/hero.png" id="2"]

[node name="Level" type="Node2D"]
script = ExtResource("1")

[node name="Hero" type="Sprite2D" parent="."]
position = Vector2(40, 30)
texture = ExtResource("2")
)"},
        {"level.gd", "extends Node2D\n\nfunc _ready() -> void:\n\tpass\n"},
        {"art/hero.png.import", "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\n"
                                "uid=\"uid://bhero000001\"\n"},
    };
    slash::render::Image image{.width = 2, .height = 2, .pixels = {}};
    image.pixels.assign(16, 200);
    const std::string png = unwrap(slash::render::encode_png(image));

    const fs::path folder = "godot_pack_project";
    fs::remove_all(folder);
    for (const auto& [path, contents] : project) {
        write(folder / "source" / path, contents);
    }
    write(folder / "source/art/hero.png", png);
    Files with_png = project;
    with_png["art/hero.png"] = png;
    write(folder / "game.pck", godot::write_pack(unwrap(godot::export_project(with_png))));

    const auto from_folder = unwrap(importer::plan_import(
        {.source = (folder / "source").string(), .destination = (folder / "a").string()}));
    const auto from_game = unwrap(importer::plan_import(
        {.source = (folder / "game.pck").string(), .destination = (folder / "b").string()}));
    Files unpacked;
    for (const auto& [path, contents] : from_game.files) {
        unpacked[path] = contents;
    }
    for (const auto& [path, contents] : from_folder.files) {
        INFO(path);
        REQUIRE(unpacked.contains(path));
        if (path.ends_with(".png")) {
            CHECK(unwrap(slash::render::decode_image(unpacked.at(path))).pixels == image.pixels);
        } else {
            CHECK(unpacked.at(path) == contents);
        }
    }
    const json::Value& report = *from_game.report.find("unpacked");
    CHECK(count(report, "scenes") == 1);
    CHECK(count(report, "scripts_from_tokens") == 1);
    CHECK(report.find("folder") == nullptr);
}

TEST_CASE("project.import unpacks an exported game into the session's project") {
    const fs::path project = "godot_pack_command";
    fs::remove_all(project);
    const Files game = {
        {"project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"Packed\"\n"
                          "run/main_scene=\"res://main.tscn\"\n"},
        {"main.tscn", "[gd_scene format=3]\n\n[node name=\"Main\" type=\"Node2D\"]\n"},
        {"main.gd", "extends Node2D\n"},
    };
    write(project / "downloads/game.pck", godot::write_pack(unwrap(godot::export_project(game))));
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    importer::register_import_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);

    const command::Reply done = session.call(
        "project.import", parse(R"({"source": "downloads/game.pck", "unpack_to": "godot"})"));
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(done.value.find("unpacked")->find("folder")->as_string() == "godot");
    CHECK(read(project / "godot/main.gd") == "extends Node2D\n");
    CHECK(fs::exists(project / "godot/project.godot"));
    CHECK(fs::exists(project / "godot/main.tscn"));
    CHECK(fs::exists(project / "project.json"));
    CHECK(session.call("scene.open", parse(R"({"path": "godot/main.scene.json"})")).ok);

    const command::Reply outside = session.call(
        "project.import", parse(R"({"source": "downloads/game.pck", "unpack_to": "../out"})"));
    CHECK_FALSE(outside.ok);
    CHECK(outside.data.find("path")->as_string() == "/unpack_to");
    write(project / "downloads/notes.txt", "not a game");
    const command::Reply not_game =
        session.call("project.import", parse(R"({"source": "downloads/notes.txt"})"));
    CHECK_FALSE(not_game.ok);
    CHECK(contains(not_game.message, "no Godot pack"));
}
