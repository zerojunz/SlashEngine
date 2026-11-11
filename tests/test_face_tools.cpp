// The face tool's commands: face.check against the storyboard's face_rules and the cast,
// face.generate from a brief and face.portrait's 3D and 2D portraits.
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
    return std::move(parsed).value();
}

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, const std::map<std::string, std::string_view>& files)
        : root(std::move(name)) {
        std::filesystem::remove_all(root);
        for (const auto& [file, text] : files) {
            const std::string path = root + "/" + file;
            REQUIRE(slash::platform::create_directories(
                        std::filesystem::path(path).parent_path().string())
                        .ok());
            REQUIRE(slash::platform::write_file(path, text).ok());
        }
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {}})")
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }
};

constexpr std::string_view kBase = R"({"format": 1, "name": "Base",
    "controls": {"nose.length": 0.5, "jaw.width": 0.4}})";
constexpr std::string_view kCopy = R"({"format": 1, "name": "Copy",
    "controls": {"nose.length": 0.5, "jaw.width": 0.4}})";
constexpr std::string_view kWide = R"({"format": 1, "name": "Wide",
    "controls": {"nose.length": 0.5, "jaw.width": 0.4, "face.width": 0.9, "cheeks.fullness": 0.8}})";
constexpr std::string_view kBeak = R"({"format": 1, "name": "Beak",
    "controls": {"nose.length": 1, "nose.hook": 0.9, "chin.protrusion": 0.8, "jaw.width": 0.4}})";
constexpr std::string_view kToon = R"({"format": 1, "name": "Toon", "style": "toon",
    "controls": {"eyes.size": 0.8, "chin.length": -0.9},
    "marks": [{"kind": "tattoo", "at": "cheekbone_left"}, {"kind": "scar", "at": "eye_left"}]})";
constexpr std::string_view kPreset = R"({"format": 1, "name": "Preset",
    "controls": {"brow.ridge": 0.7, "cheeks.hollow": 0.6}})";
// A character made from the preset: the preset is left out of the cast.
constexpr std::string_view kHero = R"({"format": 1, "name": "Hero",
    "face": {"from": "faces/preset.face.json", "eyes": [0.2, 0.4, 0.7]}})";
// A generated character's face, which character.check tells apart, not face.check.
constexpr std::string_view kExtra = R"({"format": 1, "name": "Extra",
    "face": {"nose": 1.1}})";
constexpr std::string_view kRules = R"({"format": 1, "entries": {
    "face_rules/cast": {"name": "Cast", "shape_difference": 0.6, "profile_difference": 2,
        "styles": ["realistic"], "marks": ["scar", "freckles"], "max_marks": 1,
        "controls": {"chin.length": [-0.5, 0.5], "nose.lenght": [0, 1]},
        "links": ["faces", "characters"]}}})";

bool has(const json::Array& errors, std::string_view file, std::string_view path,
         std::string_view words) {
    return std::ranges::any_of(errors, [&](const json::Value& error) {
        return error.find("file")->as_string() == file && error.find("path")->as_string() == path &&
               error.find("message")->as_string().find(words) != std::string::npos;
    });
}

const json::Value* pair_of(const json::Array& pairs, std::string_view a, std::string_view b) {
    for (const json::Value& pair : pairs) {
        const std::string& first = pair.find("a")->as_string();
        const std::string& second = pair.find("b")->as_string();
        if ((first == a && second == b) || (first == b && second == a)) {
            return &pair;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("face tools: face.check measures the cast against each other and the face rules") {
    Project project("face_check_project", {{"faces/base.face.json", kBase},
                                           {"faces/copy.face.json", kCopy},
                                           {"faces/wide.face.json", kWide},
                                           {"faces/beak.face.json", kBeak},
                                           {"faces/toon.face.json", kToon},
                                           {"faces/preset.face.json", kPreset},
                                           {"characters/hero.character.json", kHero},
                                           {"characters/extra.character.json", kExtra},
                                           {"storyboard/design.json", kRules}});
    json::Value checked = project.ok("face.check", R"({"measures": true})");
    // base, copy, wide, beak, toon and the hero; the preset and the extra aren't measured.
    CHECK(checked["faces"].as_int() == 6);
    const json::Array& errors = checked["errors"].as_array();
    const json::Array& pairs = checked["pairs"].as_array();
    CHECK(pairs.size() == 15);
    // The same sculpt is no distance apart, front or side.
    const json::Value* same = pair_of(pairs, "faces/base.face.json", "faces/copy.face.json");
    REQUIRE(same != nullptr);
    CHECK(same->find("shape")->as_double() == 0.0);
    CHECK(same->find("profile")->as_double() == 0.0);
    CHECK(has(errors, "faces/base.face.json", "", "face is too like faces/copy.face.json"));
    // A wider face differs from the front but hardly from the side; a beak shows in both.
    const json::Value* wide = pair_of(pairs, "faces/base.face.json", "faces/wide.face.json");
    const json::Value* beak = pair_of(pairs, "faces/base.face.json", "faces/beak.face.json");
    REQUIRE(wide != nullptr);
    REQUIRE(beak != nullptr);
    CHECK(wide->find("shape")->as_double() > 0.6);
    CHECK(beak->find("shape")->as_double() > 0.6);
    CHECK(beak->find("profile")->as_double() > 2.0);
    CHECK(has(errors, "faces/base.face.json", "", "profile is too like faces/wide.face.json"));
    CHECK(wide->find("profile")->as_double() < beak->find("profile")->as_double());
    // The toon breaks the rules: its style, a tattoo, two marks and its chin.
    CHECK(has(errors, "faces/toon.face.json", "/style", "face_rules/cast asks realistic"));
    CHECK(has(errors, "faces/toon.face.json", "/marks/0/kind", "\"tattoo\" marks"));
    CHECK(has(errors, "faces/toon.face.json", "/marks", "2 marks"));
    CHECK(has(errors, "faces/toon.face.json", "/controls/chin.length", "-0.5 to 0.5"));
    // A misspelled control in the rules is named, with the one meant.
    CHECK(has(errors, "storyboard/design.json", "/entries/face_rules/cast/controls/nose.lenght",
              "did you mean \"nose.length\""));
    // The hero's face is its preset's; it is checked as a character's face.
    CHECK(pair_of(pairs, "characters/hero.character.json", "faces/base.face.json") != nullptr);
    CHECK(pair_of(pairs, "faces/preset.face.json", "faces/base.face.json") == nullptr);

    // Checking one face reports only its own problems.
    json::Value one = project.ok("face.check", R"({"paths": ["faces/toon.face.json"]})");
    for (const json::Value& error : one["errors"].as_array()) {
        CHECK(error.find("file")->as_string() == "faces/toon.face.json");
    }
    CHECK(project.call("face.check", R"({"paths": ["faces/toon.json"]})").ok == false);
    // test.run fails on them too.
    json::Value run = project.ok("test.run", "{}");
    CHECK(json::write(run).find("face is too like") != std::string::npos);
}

TEST_CASE("face tools: face.generate drafts a face from a brief, unlike the others") {
    Project project(
        "face_generate_project",
        {{"faces/base.face.json", kBase}, {"storyboard/design.json", R"({"format": 1, "entries": {
        "face_rules/cast": {"name": "Cast", "shape_difference": 0.6, "marks": ["scar"],
            "links": ["faces"]}}})"}});
    json::Value made = project.ok(
        "face.generate",
        R"({"name": "Old Brann", "brief": "A stern old soldier with a scar, freckles, a hooked nose, a strong jaw and a beard"})");
    CHECK(made["out"].as_string() == "faces/old_brann.face.json");
    CHECK(made["style"].as_string() == "realistic");
    const std::string understood = json::write(made["understood"]);
    for (const std::string_view word :
         {"stern", "old", "soldier", "scar", "hooked", "strong jaw", "beard"}) {
        CHECK_MESSAGE(understood.find(word) != std::string::npos, word);
    }
    CHECK(made.find("ignored") == nullptr);
    // Freckles aren't among the marks the rules allow.
    CHECK(made["marks_left_out"].as_int() == 1);
    CHECK(made["nearest"]["path"].as_string() == "faces/base.face.json");
    CHECK(made["nearest"]["shape"].as_double() >= 0.6);
    CHECK(made.find("warning") == nullptr);
    auto text = slash::platform::read_file(project.root + "/faces/old_brann.face.json");
    REQUIRE(text.ok());
    json::Value face = parse(text.value());
    CHECK(face["controls"]["nose.hook"].as_double() > 0.5);
    CHECK(face["controls"]["jaw.angle"].as_double() > 0.5);
    CHECK(face["skin"]["wrinkles"].as_double() > 0.5);
    CHECK(face["beard"].as_string() == "short");
    REQUIRE(face["marks"].as_array().size() == 1);
    CHECK(face["marks"].as_array()[0]["kind"].as_string() == "scar");
    // It keeps the rules: face.check passes on it.
    json::Value checked = project.ok("face.check", R"({"paths": ["faces/old_brann.face.json"]})");
    CHECK(checked["errors"].as_array().empty());
    // The same name, brief and seed make the same file; undo takes it away.
    project.ok("undo");
    CHECK_FALSE(slash::platform::exists(project.root + "/faces/old_brann.face.json"));
    project.ok(
        "face.generate",
        R"({"name": "Old Brann", "brief": "A stern old soldier with a scar, freckles, a hooked nose, a strong jaw and a beard"})");
    auto again = slash::platform::read_file(project.root + "/faces/old_brann.face.json");
    REQUIRE(again.ok());
    CHECK(again.value() == text.value());

    // Words it doesn't know come back; styles come from the brief; controls are kept.
    json::Value toon =
        project.ok("face.generate",
                   R"({"name": "Pip", "brief": "a cheerful cartoon kid with big eyes and wings",
            "controls": {"chin.cleft": 0.5}})");
    CHECK(toon["style"].as_string() == "toon");
    CHECK(json::write(toon["ignored"]) == R"(["wings"])");
    auto pip = slash::platform::read_file(project.root + "/faces/pip.face.json");
    REQUIRE(pip.ok());
    CHECK(parse(pip.value())["controls"]["chin.cleft"].as_double() == 0.5);
    command::Reply misspelled =
        project.call("face.generate", R"({"name": "X", "controls": {"nose.lenght": 0.2}})");
    CHECK_FALSE(misspelled.ok);
    CHECK(misspelled.message.find("did you mean \"nose.length\"") != std::string::npos);
}

TEST_CASE("face tools: face.portrait draws a face in 3D and as its 2D cut-out") {
    Project project("face_portrait_project", {{"faces/beak.face.json", kBeak},
                                              {"characters/hero.character.json", kHero},
                                              {"faces/preset.face.json", kPreset}});
    const auto pixels = [&](std::string_view file) {
        auto bytes = slash::platform::read_file(project.root + "/" + std::string(file));
        REQUIRE(bytes.ok());
        auto image = slash::render::decode_image(bytes.value());
        REQUIRE(image.ok());
        return std::move(image).value();
    };
    // How many pixels aren't the background.
    const auto drawn = [](const slash::render::Image& image) {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            const int r = image.pixels[i];
            const int g = image.pixels[i + 1];
            const int b = image.pixels[i + 2];
            if (std::abs(r - 56) + std::abs(g - 59) + std::abs(b - 66) > 30) {
                ++count;
            }
        }
        return count;
    };
    json::Value made =
        project.ok("face.portrait", R"({"face": "faces/beak.face.json", "size": [64, 80]})");
    CHECK(made["out"].as_string() == "portraits/beak_face_json.png");
    CHECK(made["width"].as_int() == 64);
    CHECK(made["height"].as_int() == 80);
    const slash::render::Image three_quarter = pixels("portraits/beak_face_json.png");
    CHECK(three_quarter.width == 64);
    // The head fills much of the frame, and the corners are the background.
    CHECK(drawn(three_quarter) > 64 * 80 / 4);
    CHECK(drawn(three_quarter) < 64 * 80 * 9 / 10);
    // A smile and the front view change it; the cut-out draws the side-on head in 2D.
    project.ok("face.portrait", R"({"face": "characters/hero.character.json", "out": "a.png",
        "view": "front", "size": [64, 64]})");
    project.ok("face.portrait", R"({"face": "characters/hero.character.json", "out": "b.png",
        "view": "front", "size": [64, 64],
        "expressions": {"mouthSmileLeft": 1, "mouthSmileRight": 1}})");
    CHECK(pixels("a.png").pixels != pixels("b.png").pixels);
    project.ok("face.portrait", R"({"face": "characters/hero.character.json", "out": "c.png",
        "view": "cutout", "size": [64, 64]})");
    const slash::render::Image cutout = pixels("c.png");
    CHECK(drawn(cutout) > 64 * 64 / 5);
    command::Reply wrong =
        project.call("face.portrait",
                     R"({"face": "faces/beak.face.json", "expressions": {"mouthSmilLeft": 1}})");
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.message.find("did you mean \"mouthSmileLeft\"") != std::string::npos);
}
