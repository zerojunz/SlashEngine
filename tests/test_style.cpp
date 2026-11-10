#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/storyboard/fingerprint.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;
namespace storyboard = slash::storyboard;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params, int expected_code) {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == expected_code, reply.message);
        return reply;
    }
};

std::vector<std::uint8_t> filled(std::uint32_t width, std::uint32_t height,
                                 std::array<std::uint8_t, 4> color) {
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t i = 0; i < width * height; ++i) {
        pixels.insert(pixels.end(), color.begin(), color.end());
    }
    return pixels;
}

void write(const std::string& path, std::string_view text) {
    REQUIRE(slash::platform::write_file(path, text).ok());
}

// A 3D courtyard at dusk: a ground, a wall, a low warm sun and a blue-gray sky.
std::string courtyard(std::string_view sky, double energy) {
    return R"({"format": 1, "entities": {
        "camera": {"components": {"Camera3D": {"current": true},
            "Transform3D": {"position": [0, 1.5, 5], "rotation": [-0.2, 0, 0]}}},
        "environment": {"components": {"WorldEnvironment": {"background_mode": "color",
            "background_color": )" +
           std::string(sky) + R"(, "ambient_light_source": "color",
            "ambient_light_color": [0.4, 0.4, 0.5, 1]}}},
        "ground": {"components": {"MeshInstance3D": {}, "PlaneMesh": {"size": [20, 20]},
            "StandardMaterial3D": {"albedo_color": [0.45, 0.4, 0.35, 1]}, "Transform3D": {}}},
        "wall": {"components": {"MeshInstance3D": {}, "BoxMesh": {"size": [3, 2, 0.4]},
            "StandardMaterial3D": {"albedo_color": [0.7, 0.55, 0.45, 1]},
            "Transform3D": {"position": [0, 1, 0]}}},
        "sun": {"components": {"DirectionalLight3D": {"light_color": [1, 0.75, 0.55, 1],
            "light_energy": )" +
           std::to_string(energy) + R"(}, "Transform3D": {"rotation": [-0.4, 0.6, 0]}}}}})";
}

std::string project(const std::string& name) {
    const std::string root = "style_" + name + "_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/scenes").ok());
    REQUIRE(slash::platform::create_directories(root + "/props").ok());
    write(root + "/project.json", R"({"format": 1, "name": "Style", "version": "1.0.0",
        "main_scene": "scenes/yard.scene.json", "window": {"width": 64, "height": 36}})");
    return root;
}

} // namespace

TEST_CASE("fingerprints measure a picture's palette, values, edges and flatness") {
    // Black on the left, white on the right.
    std::vector<std::uint8_t> split;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const std::uint8_t v = x < 8 ? 0 : 255;
            split.insert(split.end(), {v, v, v, 255});
        }
    }
    const storyboard::Fingerprint halves = storyboard::fingerprint(split, 16, 16);
    CHECK(halves.pixels == 256);
    REQUIRE(halves.palette.size() == 2);
    CHECK(halves.palette[0].share == doctest::Approx(0.5));
    CHECK(halves.value == std::array<double, 3>{0.0, 0.0, 1.0});
    CHECK(halves.contrast == doctest::Approx(0.5));
    CHECK(halves.saturation == 0.0);
    CHECK(halves.warmth == 0.0);
    // Columns 7 and 8 of the 14 x 14 pixels inside are on the edge, two pixels wide; every
    // pixel but those of column 7 is the same as its right and lower neighbors.
    CHECK(halves.edges == doctest::Approx(28.0 / 196.0).epsilon(0.001));
    CHECK(halves.line_weight == 2.0);
    CHECK(halves.flat == doctest::Approx(210.0 / 225.0).epsilon(0.001));

    // Red is saturated and warm; transparent pixels aren't measured.
    std::vector<std::uint8_t> red = filled(8, 8, {255, 0, 0, 255});
    for (std::size_t i = 3; i < std::size_t{4 * 8 * 4}; i += 4) {
        red[i] = 0; // the top four rows
    }
    const storyboard::Fingerprint warm = storyboard::fingerprint(red, 8, 8);
    CHECK(warm.pixels == 32);
    CHECK(warm.saturation == 1.0);
    CHECK(warm.warmth == 1.0);
    CHECK(warm.flat == 1.0);
    CHECK(storyboard::describe_look(warm) ==
          "mostly #ff0000, warm, dark, low contrast, saturated, flat shading");

    // Kept as JSON, read back and compared: no drift from itself, the palette and values from a
    // gray picture.
    json::Value kept = storyboard::to_json(halves);
    CHECK(json::write(kept["palette"], json::Style::Compact) == R"({"#000000":0.5,"#ffffff":0.5})");
    auto read = storyboard::fingerprint_from_json(kept);
    REQUIRE(read.ok());
    for (const auto& [measure, drift] : storyboard::drift(halves, read.value())) {
        CHECK_MESSAGE(drift.as_double() == 0.0, measure);
    }
    const std::vector<std::uint8_t> gray = filled(16, 16, {128, 128, 128, 255});
    const json::Object drift = storyboard::drift(halves, storyboard::fingerprint(gray, 16, 16));
    CHECK(drift.at("palette").as_double() == doctest::Approx(0.8643).epsilon(0.001));
    CHECK(drift.at("value").as_double() == doctest::Approx(0.502).epsilon(0.001));
    CHECK(drift.at("contrast").as_double() == doctest::Approx(0.5));
    CHECK(storyboard::describe_measure(halves, "palette") == "#000000 50%, #ffffff 50%");
    CHECK(!storyboard::fingerprint_from_json(parse(R"({"value": [0, 1]})")).ok());
}

TEST_CASE("style.mark keeps a scene's look, and style.check finds drift and what changed") {
    const std::string root = project("mark");
    write(root + "/scenes/yard.scene.json", courtyard("[0.35, 0.4, 0.55, 1]", 1.2));
    write(root + "/scenes/well.scene.json", courtyard("[0.35, 0.4, 0.55, 1]", 1.2));
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    json::Value marked = engine.ok("style.mark", R"({"name": "dusk",
        "scene": "scenes/yard.scene.json", "links": ["scenes"],
        "text": "A low warm sun under a blue-gray sky."})");
    CHECK(marked["id"] == "style/dusk");
    CHECK(marked["frame"] == "storyboard/style/dusk.png");
    CHECK(marked["fingerprint"]["pixels"] == 64 * 36);
    CHECK(std::filesystem::exists(root + "/storyboard/style/dusk.png"));
    json::Value entry = engine.ok("storyboard.query", R"({"ids": ["style/dusk"], "full": true})");
    json::Value fields = entry["entries"].as_array()[0]["fields"];
    CHECK(fields["capture"] == parse(R"({"frame": "storyboard/style/dusk.png",
        "path": "scenes/yard.scene.json", "size": [64, 36], "what": "scene"})"));
    CHECK(fields["links"] == parse(R"(["scenes/yard.scene.json", "scenes"])"));
    CHECK(fields["settings"]["entities"]["sun"]["DirectionalLight3D"]["light_energy"].as_double() ==
          doctest::Approx(1.2));
    CHECK(fields["settings"]["entities"].find("wall") != nullptr); // its StandardMaterial3D

    // A brief for any scene its links cover hands over the reference.
    json::Value brief = engine.ok("storyboard.brief", R"({"scenes": ["scenes/well.scene.json"]})");
    CHECK(json::write(brief["entries"], json::Style::Compact).find("\"style/dusk\"") !=
          std::string::npos);

    // Both scenes look as it does.
    json::Value checked = engine.ok("style.check");
    REQUIRE(checked["checked"].size() == 2);
    CHECK(checked["drifted"] == 0);
    CHECK(checked["checked"].as_array()[0]["target"] == "scenes/yard.scene.json");
    CHECK(checked["checked"].as_array()[1]["target"] == "scenes/well.scene.json");
    CHECK(checked["checked"].as_array()[0]["drift"]["palette"] == 0.0);
    CHECK(checked["checked"].as_array()[1].find("over") == nullptr);

    // An agent lights the well at noon: it drifts, and the check says what changed.
    write(root + "/scenes/well.scene.json", courtyard("[0.55, 0.75, 1, 1]", 3.0));
    checked = engine.ok("style.check", R"({"paths": ["scenes/well.scene.json"]})");
    REQUIRE(checked["checked"].size() == 1);
    CHECK(checked["drifted"] == 1);
    json::Value well = checked["checked"].as_array()[0];
    CHECK(well.find("over") != nullptr);
    CHECK(well["over"].find("value") != nullptr);
    CHECK(well["changed"] == parse(R"(["/entities/environment/WorldEnvironment/background_color",
        "/entities/sun/DirectionalLight3D/light_energy"])"));

    // test.run fails the drift, pointing at the reference.
    json::Value tested = engine.ok("test.run");
    bool found = false;
    for (json::Value& failure : tested["failed"].as_array()) {
        if (failure["file"] == "storyboard/style.json") {
            CHECK(failure["path"] == "/entries/style~1dusk");
            CHECK(failure["message"].as_string().starts_with(
                "scenes/well.scene.json drifted from its look: "));
            CHECK(!found); // one failure for the frame, whatever drifted in it
            found = true;
        }
    }
    CHECK(found);

    // Undo takes the reference and its frame away again.
    engine.ok("undo");
    CHECK(!std::filesystem::exists(root + "/storyboard/style/dusk.png"));
    CHECK(engine.ok("style.check")["checked"].size() == 0);

    // The open world can be marked as it is now: style.check opens its scene at that tick.
    engine.ok("scene.open", R"({"path": "scenes/yard.scene.json"})");
    engine.ok("step", R"({"ticks": 3})");
    json::Value now = engine.ok("style.mark", R"({"name": "evening", "links": ["scenes"]})");
    CHECK(now["fingerprint"] == marked["fingerprint"]);
    json::Value evening =
        engine.ok("storyboard.query", R"({"ids": ["style/evening"], "full": true})");
    CHECK(evening["entries"].as_array()[0]["fields"]["capture"] ==
          parse(R"({"frame": "storyboard/style/evening.png", "path": "scenes/yard.scene.json",
                    "size": [64, 36], "ticks": 3, "what": "world"})"));
    // The yard opens at the third tick and looks the same; the noon well drifts from it too.
    json::Value held = engine.ok("style.check", R"({"references": ["style/evening"]})");
    REQUIRE(held["checked"].size() == 2);
    CHECK(held["checked"].as_array()[0]["target"] == "scenes/yard.scene.json");
    CHECK(held["checked"].as_array()[0].find("over") == nullptr);
    CHECK(held["drifted"] == 1);
    engine.ok("undo");

    std::filesystem::remove_all(root);
}

TEST_CASE("style.check's journal shows drift creeping up that no single check fails") {
    const std::string root = project("journal");
    write(root + "/scenes/yard.scene.json", courtyard("[0.35, 0.4, 0.55, 1]", 1.2));
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("style.mark", R"({"name": "dusk", "scene": "scenes/yard.scene.json"})");

    // The sun's energy creeps up a little each day of work.
    for (const double energy : {1.25, 1.3, 1.35}) {
        write(root + "/scenes/yard.scene.json", courtyard("[0.35, 0.4, 0.55, 1]", energy));
        json::Value checked = engine.ok("style.check", R"({"record": true})");
        CHECK(checked["drifted"] == 0);
        CHECK(checked["checked"].as_array()[0].find("creeping") == nullptr);
    }
    write(root + "/scenes/yard.scene.json", courtyard("[0.35, 0.4, 0.55, 1]", 1.4));
    json::Value checked = engine.ok("style.check");
    CHECK(checked["drifted"] == 0);
    const json::Value* creeping = checked["checked"].as_array()[0].find("creeping");
    REQUIRE(creeping != nullptr);
    REQUIRE(creeping->find("value") != nullptr);
    CHECK(creeping->find("value")->size() == 4);
    auto journal = slash::platform::read_file(root + "/storyboard/style_journal.jsonl");
    REQUIRE(journal.ok());
    CHECK(std::ranges::count(journal.value(), '\n') == 3);
    std::filesystem::remove_all(root);
}

TEST_CASE("style references keep 2D scenes, images, models and materials too") {
    const std::string root = project("kinds");
    write(root + "/dusk.texture.json", R"({"format": 1, "type": "GradientTexture1D",
        "width": 64, "gradient": {"points": [{"offset": 0, "color": [0.1, 0.1, 0.3, 1]},
        {"offset": 1, "color": [0.9, 0.4, 0.3, 1]}]}})");
    // A 2D scene: the gradient stretched over the window, under a CanvasModulate.
    const auto menu = [](std::string_view tint) {
        return R"({"format": 1, "entities": {
            "sky": {"components": {"Sprite2D": {"texture": "dusk.texture.json"},
                "Transform2D": {"position": [32, 18], "scale": [1, 36]}}},
            "tint": {"components": {"CanvasModulate": {"color": )" +
               std::string(tint) + R"(}}}}})";
    };
    write(root + "/scenes/menu.scene.json", menu("[1, 1, 1, 1]"));
    write(root + "/props/crate.model.json", R"({"format": 1,
        "materials": {"wood": {"albedo_color": [0.55, 0.36, 0.2, 1]}},
        "parts": [{"name": "box", "shape": "box", "size": [1, 1, 1], "material": "wood"}]})");
    write(root + "/props/brass.standard_material.json", R"({"format": 1,
        "albedo_color": [0.8, 0.6, 0.2, 1], "metallic": 1, "roughness": 0.3})");
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    json::Value menu_look = engine.ok("style.mark", R"({"name": "menu",
        "scene": "scenes/menu.scene.json"})");
    CHECK(menu_look["fingerprint"]["warmth"].as_double() > 0.0);
    json::Value sky = engine.ok("style.mark", R"({"name": "sky", "image": "dusk.texture.json"})");
    CHECK(sky["fingerprint"]["pixels"] == 64);
    json::Value crate = engine.ok("style.mark", R"({"name": "crate",
        "model": "props/crate.model.json"})");
    CHECK(crate["fingerprint"]["pixels"] == 160 * 160);
    json::Value brass = engine.ok("style.mark", R"({"name": "brass",
        "material": "props/brass.standard_material.json", "size": [64, 64]})");
    CHECK(brass["fingerprint"]["pixels"] == 64 * 64);
    CHECK(brass["fingerprint"]["warmth"].as_double() > 0.0);

    json::Value checked = engine.ok("style.check");
    CHECK(checked["checked"].size() == 4);
    CHECK(checked["drifted"] == 0);

    // A cold tint over the menu turns it blue.
    write(root + "/scenes/menu.scene.json", menu("[0.4, 0.6, 1, 1]"));
    checked = engine.ok("style.check", R"({"references": ["style/menu"]})");
    REQUIRE(checked["checked"].size() == 1);
    CHECK(checked["checked"].as_array()[0]["over"].find("warmth") != nullptr);
    CHECK(checked["checked"].as_array()[0]["changed"] ==
          parse(R"(["/entities/tint/CanvasModulate/color"])"));

    // Mistakes say what to give.
    CHECK(engine
              .fails("style.mark", R"({"name": "x", "scene": "scenes/menu.scene.json",
        "image": "dusk.texture.json"})",
                     command::code::kInvalidParams)
              .message == "/image: mark one thing at a time; this also gives scene");
    CHECK(engine
              .fails("style.mark", R"({"name": "x", "scene": "props/crate.model.json"})",
                     command::code::kInvalidParams)
              .message ==
          R"(/scene: "props/crate.model.json" isn't a scene file; mark it as "model")");
    CHECK(engine.fails("style.mark", R"({"name": "Bad Name"})", command::code::kInvalidParams)
              .message.starts_with("/name: "));
    CHECK(engine.fails("style.check", R"({"references": ["style/nope"]})", command::code::kNotFound)
              .message.starts_with("/references/0: "));
    CHECK(engine.fails("style.check", R"({"review": true})", command::code::kInvalidParams)
              .message.starts_with("/review: storyboard.review isn't registered here"));
    std::filesystem::remove_all(root);
}
