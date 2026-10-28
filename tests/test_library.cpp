#include "gltf.hpp"
#include "humanoid.hpp"

#include "slash/ai/commands.hpp"
#include "slash/ai/config.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/sha256.hpp"
#include "slash/library/commands.hpp"
#include "slash/library/generator.hpp"
#include "slash/library/library.hpp"
#include "slash/library/material.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace library = slash::library;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), std::string(text));
    return std::move(value).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE_MESSAGE(text.ok(), path.string());
    return std::move(text).value();
}

// A fresh folder for a test: name/project and name/library.
fs::path fresh(std::string_view name) {
    const fs::path root = fs::absolute(name);
    fs::remove_all(root);
    write(root / "project" / "project.json", R"({"format": 1, "name": "Library test"})");
    fs::create_directories(root / "library");
    return root;
}

library::GenerateRequest image(std::string prompt, int width = 16, int height = 0) {
    library::GenerateRequest request;
    request.type = "image";
    request.prompt = std::move(prompt);
    request.width = width;
    request.height = height;
    return request;
}

slash::render::Image decode(const std::string& png) {
    auto decoded = slash::render::decode_image(png);
    REQUIRE(decoded.ok());
    return std::move(decoded).value();
}

std::array<int, 4> pixel(const slash::render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(const fs::path& root, bool write_files = true) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::ai::register_commands(commands);
        library::register_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{.project_root = (root / "project").string(),
                                    .capture_logs = false,
                                    .write_files = write_files,
                                    .library_dir = (root / "library").string()});
    }

    json::Value call(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return reply.value;
    }

    command::Reply fail(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        return reply;
    }
};

} // namespace

TEST_CASE("SHA-256 in pieces matches SHA-256 at once") {
    CHECK(slash::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string text;
    for (int i = 0; i < 300; ++i) {
        text += static_cast<char>('a' + i % 26);
    }
    slash::Sha256 pieces;
    pieces.update(text.substr(0, 1));
    pieces.update(text.substr(1, 63));
    pieces.update(text.substr(64, 100));
    pieces.update(text.substr(164));
    CHECK(pieces.hex() == slash::sha256_hex(text));
}

TEST_CASE("library stores each asset once by the hash of its bytes") {
    const fs::path root = fresh("library_store");
    const library::Library store((root / "library").string());
    auto png = library::test_generator()->generate(image("red ball"));
    REQUIRE(png.ok());

    library::Record record;
    REQUIRE(library::describe_file(png.value().data, "ball.png", record).ok());
    CHECK(record.type == "image");
    CHECK(record.format == "png");
    CHECK(record.width == 16);
    CHECK(record.height == 16);
    record.description = "A red ball.";
    record.tags = {"ball"};
    auto stored = store.put(png.value().data, record);
    REQUIRE(stored.ok());
    const std::string id = stored.value().id;
    CHECK(id == slash::sha256_hex(png.value().data));
    CHECK(fs::exists(root / "library" / "files" / id.substr(0, 2) / (id + ".png")));
    CHECK(fs::exists(root / "library" / "assets" / id.substr(0, 2) / (id + ".json")));

    // The same bytes again keep the first record, with the new tags added.
    record.description = "Something else.";
    record.tags = {"toy", "ball"};
    auto again = store.put(png.value().data, record);
    REQUIRE(again.ok());
    CHECK(again.value().description == "A red ball.");
    CHECK(again.value().tags == std::vector<std::string>{"ball", "toy"});

    // A prefix finds it; a bad, unknown or short one doesn't.
    auto found = store.get(id.substr(0, 8));
    REQUIRE(found.ok());
    CHECK(found.value().id == id);
    CHECK(json::write(library::to_json(found.value())) ==
          json::write(library::to_json(again.value())));
    CHECK(store.read(found.value()).value() == png.value().data);
    CHECK(store.get("abc").error().message == "\"abc\" is not an asset ID: 6 to 64 hex digits");
    CHECK(store.get("zzzzzzzz").error().message ==
          "\"zzzzzzzz\" is not an asset ID: 6 to 64 hex digits");
    const std::string other = id[0] == '0' ? "1234567" : "0123456";
    CHECK(store.get(other).error().message == "no asset with ID \"" + other + "\" in the library");

    auto wav = library::test_generator()->generate({.type = "sound", .prompt = "beep"});
    REQUIRE(wav.ok());
    library::Record sound;
    REQUIRE(library::describe_file(wav.value().data, "beep.WAV", sound).ok());
    CHECK(sound.type == "sound");
    CHECK(sound.format == "wav");
    CHECK(sound.seconds == 0.5);
    CHECK(sound.channels == 1);
    CHECK(sound.sample_rate == 22050);
    REQUIRE(store.put(wav.value().data, sound).ok());
    auto all = store.all();
    REQUIRE(all.ok());
    CHECK(all.value().size() == 2);

    library::Record unknown;
    CHECK(library::describe_file("text", "notes.txt", unknown).error().message ==
          "\"notes.txt\" is not a file the library keeps: .png, .bmp, .wav, .ogg, .mp3, "
          ".model.json, .glb or .standard_material.json");
    CHECK_FALSE(library::describe_file("not a png", "broken.png", unknown).ok());

    // Requests remember what they made.
    json::Value request = parse(R"({"prompt": "red ball", "type": "image"})");
    const std::string key = library::request_key("test", "test", request);
    CHECK(key.size() == 64);
    CHECK(key != library::request_key("test", "other", request));
    CHECK_FALSE(store.find_request(key));
    REQUIRE(store.remember_request(key, id).ok());
    CHECK(store.find_request(key) == id);
}

TEST_CASE("library search ranks by the share of words found") {
    std::vector<library::Record> records(4);
    records[0].id = std::string(64, 'a');
    records[0].type = "image";
    records[0].description = "A green slime that bounces.";
    records[0].tags = {"enemy", "slime"};
    records[1].id = std::string(64, 'b');
    records[1].type = "image";
    records[1].description = "A red slime.";
    records[2].id = std::string(64, 'c');
    records[2].type = "sound";
    records[2].request = parse(R"({"prompt": "slime squish"})");
    records[3].id = std::string(64, 'd');
    records[3].type = "image";
    records[3].description = "Gold coins in a pile.";
    records[3].tags = {"pickup"};

    const auto ids = [](const std::vector<library::Match>& matches) {
        std::string out;
        for (const library::Match& match : matches) {
            out += match.record->id.substr(0, 1);
            out += ':';
            out += std::to_string(static_cast<int>(match.score * 100));
            out += ' ';
        }
        return out;
    };
    CHECK(ids(library::search(records, {.text = "green slimes"})) == "a:100 b:50 c:50 ");
    CHECK(ids(library::search(records, {.text = "Slime", .type = "image"})) == "a:100 b:100 ");
    CHECK(ids(library::search(records, {.text = "a coin"})) == "d:100 ");
    CHECK(ids(library::search(records, {.tags = {"enemy"}})) == "a:100 ");
    CHECK(ids(library::search(records, {.type = "image", .limit = 2})) == "a:100 b:100 ");
    CHECK(library::search(records, {.text = "dragon"}).empty());
}

TEST_CASE("test generator makes placeholders the same on every machine") {
    const std::shared_ptr<library::Generator> generator = library::test_generator();
    auto first = generator->generate(image("red ball sprite", 32));
    auto second = generator->generate(image("red ball sprite", 32));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.value().data == second.value().data);
    CHECK(first.value().format == "png");
    CHECK(first.value().model == "test");
    CHECK(first.value().seed == 0);

    // A red circle on a transparent background.
    const slash::render::Image ball = decode(first.value().data);
    CHECK(ball.width == 32);
    CHECK(ball.height == 32);
    CHECK(pixel(ball, 0, 0)[3] == 0);
    const std::array<int, 4> middle = pixel(ball, 18, 14);
    CHECK(middle[3] == 255);
    CHECK(middle[0] > middle[1] + 100);

    // A box fills its corner area; a seed or a new prompt changes the image.
    const slash::render::Image crate =
        decode(generator->generate(image("crate", 32, 16)).value().data);
    CHECK(crate.width == 32);
    CHECK(crate.height == 16);
    CHECK(pixel(crate, 5, 3)[3] == 255);
    library::GenerateRequest seeded = image("red ball sprite", 32);
    seeded.seed = 7;
    CHECK(generator->generate(seeded).value().data != first.value().data);
    CHECK(generator->generate(seeded).value().seed == 7);

    // Refining: recolored, mirrored, or varied when it understands none of the prompt.
    library::Record parent;
    library::GenerateRequest blue = image("make it blue");
    blue.parent = &parent;
    blue.parent_data = first.value().data;
    const slash::render::Image recolored = decode(generator->generate(blue).value().data);
    CHECK(pixel(recolored, 0, 0)[3] == 0);
    const std::array<int, 4> now = pixel(recolored, 18, 14);
    CHECK(now[2] > now[0] + 50);
    library::GenerateRequest mirror = blue;
    mirror.prompt = "mirror it";
    const slash::render::Image mirrored = decode(generator->generate(mirror).value().data);
    CHECK(pixel(mirrored, 31 - 18, 14) == middle);
    library::GenerateRequest vague = blue;
    vague.prompt = "more menacing";
    CHECK(generator->generate(vague).value().data != first.value().data);

    // Sounds: a tone as long as asked, then refined higher and shorter.
    auto tone = generator->generate({.type = "sound", .prompt = "coin", .seconds = 0.25});
    REQUIRE(tone.ok());
    CHECK(tone.value().format == "wav");
    CHECK(tone.value().data.size() == 44 + 2 * 5513);
    library::GenerateRequest higher{.type = "sound", .prompt = "higher and shorter"};
    higher.parent = &parent;
    higher.parent_data = tone.value().data;
    auto refined = generator->generate(higher);
    REQUIRE(refined.ok());
    CHECK(refined.value().data.size() == 44 + 2 * (5513 * 2 / 3 / 2));
    library::GenerateRequest not_wav = higher;
    not_wav.parent_data = "OggS";
    CHECK(generator->generate(not_wav).error().message ==
          "the test generator refines only the .wav sounds it makes");
}

TEST_CASE("providers.json names generators and open_generator picks one") {
    auto config = slash::ai::parse_config(parse(R"({"generators": {
        "placeholders": {"type": "test", "license": "Ours"}}})"));
    REQUIRE(config.ok());
    REQUIRE(config.value().generators.size() == 1);
    CHECK(config.value().generators.at("placeholders").license == "Ours");

    auto only = library::open_generator(config.value(), "", "image");
    REQUIRE(only.ok());
    CHECK(only.value().name == "placeholders");
    CHECK(only.value().type == "test");
    CHECK(only.value().model == "test");
    CHECK(only.value().license == "Ours");
    auto builtin = library::open_generator({}, "test", "sound");
    REQUIRE(builtin.ok());
    CHECK(builtin.value().license == "CC0-1.0");
    CHECK(library::open_generator({}, "", "image").error().message ==
          "providers.json names no generator that makes images; add one under \"generators\", or "
          "pass generator \"test\" for placeholders");
    CHECK(library::open_generator(config.value(), "placeholder", "image").error().message ==
          "no generator named \"placeholder\"; did you mean \"placeholders\"?");

    CHECK(slash::ai::parse_config(parse(R"({"generators": {"x": {"type": "dalle"}}})"))
              .error()
              .message.starts_with("providers.json /generators/x/type: "));
}

TEST_CASE("asset.generate keeps assets, reuses exact repeats and offers close matches") {
    const fs::path root = fresh("library_generate");
    const Engine engine(root);

    json::Value made = engine.call(
        "asset.generate", R"({"prompt": "green slime sprite", "generator": "test", "width": 24,
                              "tags": ["enemy"]})");
    const std::string id = made["id"].as_string();
    CHECK(id.size() == 12);
    CHECK(json::write(made, json::Style::Compact) ==
          R"({"height":24,"id":")" + id + R"(","width":24})");

    // The same request again calls no generator.
    CHECK(json::write(engine.call("asset.generate", R"({"prompt": "green slime sprite",
                                  "generator": "test", "width": 24, "tags": ["enemy"]})"),
                      json::Style::Compact) ==
          R"({"height":24,"id":")" + id + R"(","reused":true,"width":24})");

    // A close request gets the match back as a candidate, and makes nothing until asked.
    json::Value close =
        engine.call("asset.generate", R"({"prompt": "slime sprite, green", "generator": "test"})");
    CHECK(json::write(close, json::Style::Compact) ==
          R"({"candidates":[{"description":"green slime sprite","height":24,"id":")" + id +
              R"(","score":1.0,"tags":["enemy"],"type":"image","width":24}]})");
    json::Value forced =
        engine.call("asset.generate", R"({"prompt": "slime sprite, green", "generator": "test",
                                           "similar": false, "path": "art/slime.png"})");
    CHECK(forced["id"].as_string() != id);
    CHECK(forced["path"] == "art/slime.png");
    const std::string png = read(root / "project" / "art" / "slime.png");
    CHECK(slash::sha256_hex(png).starts_with(forced["id"].as_string()));
    json::Value sources = parse(read(root / "project" / "library.json"));
    CHECK(sources["assets"]["art/slime.png"]["id"] == slash::sha256_hex(png));
    CHECK(sources["assets"]["art/slime.png"]["generator"] == "test");
    CHECK(sources["assets"]["art/slime.png"]["license"] == "CC0-1.0");

    // A sound, written straight into the project.
    json::Value sound = engine.call("asset.generate",
                                    R"({"prompt": "laser zap", "type": "sound", "generator": "test",
                              "seconds": 0.2, "path": "sfx/zap.wav"})");
    CHECK(sound["seconds"] == 0.2);
    CHECK(fs::exists(root / "project" / "sfx" / "zap.wav"));

    json::Value found = engine.call("asset.search", R"({"text": "slime"})");
    REQUIRE(found["assets"].size() == 2);
    // Both match every word, so they come by ID.
    const std::string forced_id = forced["id"].as_string();
    CHECK(found["assets"].as_array()[0]["id"] == (id < forced_id ? id : forced_id));
    json::Value sounds = engine.call("asset.search", R"({"type": "sound"})");
    REQUIRE(sounds["assets"].size() == 1);
    CHECK(json::write(sounds["assets"].as_array()[0], json::Style::Compact) ==
          R"({"description":"laser zap","id":")" + sound["id"].as_string() +
              R"(","seconds":0.2,"type":"sound"})");
}

TEST_CASE("asset.refine makes versions that asset.info lists") {
    const fs::path root = fresh("library_refine");
    const Engine engine(root);
    const std::string first =
        engine
            .call("asset.generate",
                  R"({"prompt": "wooden crate", "generator": "test", "tags": ["prop"]})")["id"]
            .as_string();
    json::Value second =
        engine.call("asset.refine", R"({"id": ")" + first + R"(", "prompt": "darker"})");
    CHECK(second["parent"] == first);
    const std::string second_id = second["id"].as_string();
    json::Value third =
        engine.call("asset.refine", R"({"id": ")" + second_id.substr(0, 8) +
                                        R"(", "prompt": "red", "path": "props/crate.png"})");
    CHECK(third["parent"] == second_id);
    CHECK(fs::exists(root / "project" / "props" / "crate.png"));

    json::Value info = engine.call("asset.info", R"({"id": ")" + third["id"].as_string() + R"("})");
    CHECK(info["description"] == "wooden crate; darker; red");
    CHECK(info["tags"] == parse(R"(["prop"])"));
    CHECK(info["generator"] == "test");
    CHECK(info["license"] == "CC0-1.0");
    CHECK(info["request"]["prompt"] == "red");
    CHECK(info["versions"] == parse(R"([")" + first + R"(", ")" + second_id + R"("])"));
    json::Value root_info = engine.call("asset.info", R"({"id": ")" + first + R"("})");
    CHECK(root_info["children"] == parse(R"([")" + second_id + R"("])"));
    CHECK(root_info.find("versions") == nullptr);

    // Refining the same way again reuses the version already made.
    CHECK(engine.call("asset.refine",
                      R"({"id": ")" + first + R"(", "prompt": "darker"})")["reused"] == true);
}

TEST_CASE("asset.use and asset.add move files between the project and the library") {
    const fs::path root = fresh("library_use");
    const Engine engine(root);
    auto png = library::test_generator()->generate(image("blue gem icon", 8));
    REQUIRE(png.ok());
    write(root / "project" / "art" / "gem.png", png.value().data);

    json::Value added =
        engine.call("asset.add", R"({"path": "art/gem.png", "description": "A blue gem.",
                                     "tags": ["pickup"], "license": "CC-BY-4.0",
                                     "author": "Mira"})");
    CHECK(json::write(added, json::Style::Compact) ==
          R"({"height":8,"id":")" + slash::sha256_hex(png.value().data).substr(0, 12) +
              R"(","type":"image","width":8})");
    json::Value info = engine.call("asset.info", R"({"id": ")" + added["id"].as_string() + R"("})");
    CHECK(info["author"] == "Mira");
    CHECK(info["license"] == "CC-BY-4.0");
    CHECK(info.find("generator") == nullptr);

    // Another project uses it.
    write(root / "other" / "project.json", R"({"format": 1, "name": "Other"})");
    const Engine other(root);
    command::SessionOptions options = other.session->options();
    options.project_root = (root / "other").string();
    command::Session session(other.components, other.commands, options);
    const command::Reply used = session.call(
        "asset.use", parse(R"({"id": ")" + added["id"].as_string() + R"(", "path": "gem.png"})"));
    REQUIRE_MESSAGE(used.ok, used.message);
    CHECK(read(root / "other" / "gem.png") == png.value().data);
    json::Value sources = parse(read(root / "other" / "library.json"));
    CHECK(sources["assets"]["gem.png"]["author"] == "Mira");
    CHECK(sources["assets"]["gem.png"]["license"] == "CC-BY-4.0");

    CHECK(
        engine
            .fail("asset.use", R"({"id": ")" + added["id"].as_string() + R"(", "path": "gem.bmp"})")
            .message == "/path: the asset is a .png file; name the path so");
    CHECK(engine
              .fail("asset.use",
                    R"({"id": ")" + added["id"].as_string() + R"(", "path": "../gem.png"})")
              .data["path"] == "/path");
}

TEST_CASE("model recipes go into the library, and refining one changes its parameters") {
    const fs::path root = fresh("library_models");
    const Engine engine(root);
    const std::string recipe = R"({"format": 1, "parameters": {"width": 1, "height": 0.5},
        "materials": {"oak": {"albedo_color": [0.6, 0.4, 0.2, 1]}},
        "parts": [{"name": "crate", "shape": "box", "material": "oak",
                   "size": ["=width", "=height", 1], "position": [0, "=height / 2", 0]}],
        "points": [{"name": "lid", "part": "crate", "position": [0, "=height / 2", 0]}]})";
    write(root / "project" / "props" / "crate.model.json", recipe);
    json::Value added = engine.call(
        "asset.add", R"({"path": "props/crate.model.json", "description": "A wooden crate.",
                         "license": "CC0-1.0", "author": "Mira"})");
    CHECK(added["type"] == "model");
    CHECK(added["size"] == parse("[1, 0.5, 1]"));
    CHECK(added["triangles"] == 12);
    const std::string first = added["id"].as_string();
    CHECK(engine.call("asset.search", R"({"type": "model"})")["assets"].as_array()[0]["id"] ==
          first);

    // A wider version, with no generator, linked to the first and copied into the project.
    json::Value wider = engine.call(
        "asset.refine", R"({"id": ")" + first +
                            R"(", "parameters": {"width": 2}, "path": "props/wide.model.json"})");
    CHECK(wider["parent"] == first);
    CHECK(wider["size"] == parse("[2, 0.5, 1]"));
    json::Value written = parse(read(root / "project" / "props" / "wide.model.json"));
    CHECK(written["parameters"]["width"] == 2);
    CHECK(written["parameters"]["height"] == 0.5);
    json::Value info = engine.call("asset.info", R"({"id": ")" + wider["id"].as_string() + R"("})");
    CHECK(info["description"] == "A wooden crate.");
    CHECK(info["author"] == "Mira");
    CHECK(info["request"]["parameters"] == parse(R"({"width": 2})"));
    CHECK(info["versions"] == parse(R"([")" + first + R"("])"));
    // What an agent needs to place it: the origin on its floor, and its part and point.
    CHECK(info["pivot"] == parse("[0.5, 0, 0.5]"));
    CHECK(info["parts"] == parse(R"(["crate"])"));
    CHECK(info["points"] == parse(R"(["lid"])"));
    CHECK(info.find("generator") == nullptr);
    CHECK(engine.call("asset.refine", R"({"id": ")" + first +
                                          R"(", "parameters": {"width": 2}})")["reused"] == true);
    // Another project uses a version as any model file.
    CHECK(engine.call("asset.use",
                      R"({"id": ")" + first + R"(", "path": "props/again.model.json"})")["path"] ==
          "props/again.model.json");
    CHECK(read(root / "project" / "props" / "again.model.json") == recipe);

    CHECK(engine.fail("asset.refine", R"({"id": ")" + first + R"(", "parameters": {"widht": 2}})")
              .message ==
          "/parameters/widht: \"widht\" isn't one of the recipe's parameters (did you mean "
          "\"width\"?)");
    CHECK(
        engine
            .fail("asset.refine", R"({"id": ")" + first + R"(", "parameters": {"width": "=1 +"}})")
            .data["path"] == "/parameters");
    CHECK(engine.fail("asset.refine", R"({"id": ")" + first + R"("})").data["path"] ==
          "/parameters");
    CHECK(engine
              .fail("asset.refine",
                    R"({"id": ")" + first + R"(", "parameters": {"width": 2}, "seed": 1})")
              .data["path"] == "/seed");
    CHECK(engine.fail("asset.use", R"({"id": ")" + first + R"(", "path": "props/crate.json"})")
              .message == "/path: the asset is a .model.json file; name the path so");
    // A generated model is made from a concept image.
    CHECK(engine.fail("asset.generate", R"({"prompt": "a chair", "type": "model"})").data["path"] ==
          "/image");
    // Texture paths are a project's, so recipes that name them stay out of the library.
    write(root / "project" / "props" / "stone.model.json",
          R"({"format": 1, "materials": {"stone": {"albedo_texture": "stone.png"}},
              "parts": [{"shape": "sphere", "material": "stone"}]})");
    CHECK(
        engine.fail("asset.add", R"({"path": "props/stone.model.json", "description": "A ball."})")
            .message.starts_with("/path: /materials/stone/albedo_texture: "));
    // A recipe taken from the library and edited goes back in as a new version of it, with
    // what it was said to be.
    write(root / "project" / "props" / "again.model.json",
          R"({"format": 1, "parameters": {"width": 3},
              "parts": [{"name": "crate", "shape": "box", "size": ["=width", 1, 1]}]})");
    json::Value edited = engine.call("asset.add", R"({"path": "props/again.model.json"})");
    CHECK(edited["parent"] == first);
    CHECK(edited["size"] == parse("[3, 1, 1]"));
    info = engine.call("asset.info", R"({"id": ")" + edited["id"].as_string() + R"("})");
    CHECK(info["description"] == "A wooden crate.");
    CHECK(info["license"] == "CC0-1.0");
    CHECK(info["author"] == "Mira");
    // A file the library never gave needs saying what it is.
    write(root / "project" / "props" / "new.model.json",
          R"({"format": 1, "parts": [{"shape": "sphere"}]})");
    CHECK(engine.fail("asset.add", R"({"path": "props/new.model.json"})").data["path"] ==
          "/description");

    // A .glb model goes in with its summary, if it holds everything it reads.
    slash::test::Gltf glb = slash::test::quad();
    glb.document["nodes"] = parse(R"([{"name": "Quad", "mesh": 0, "children": [1]},
                                      {"name": "Hook", "translation": [0, 1, 0]}])");
    write(root / "project" / "props" / "sign.glb", glb.glb());
    json::Value sign =
        engine.call("asset.add", R"({"path": "props/sign.glb", "description": "A blank sign.",
                         "license": "CC-BY-4.0", "author": "Ana"})");
    CHECK(sign["type"] == "model");
    CHECK(sign["size"] == parse("[2, 2, 0]"));
    CHECK(sign["triangles"] == 2);
    info = engine.call("asset.info", R"({"id": ")" + sign["id"].as_string() + R"("})");
    CHECK(info["format"] == "glb");
    CHECK(info["parts"] == parse(R"(["Quad"])"));
    CHECK(info["points"] == parse(R"(["Hook"])"));
    CHECK(engine
              .fail("asset.refine",
                    R"({"id": ")" + sign["id"].as_string() + R"(", "prompt": "rounder"})")
              .data["path"] == "/id");
    slash::test::Gltf painted = slash::test::quad(0);
    painted.document["images"] = parse(R"([{"uri": "paint.png"}])");
    painted.document["textures"] = parse(R"([{"source": 0}])");
    painted.document["materials"] =
        parse(R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])");
    write(root / "project" / "props" / "painted.glb", painted.glb());
    CHECK(engine.fail("asset.add", R"({"path": "props/painted.glb", "description": "A sign."})")
              .message ==
          "/path: the library keeps .glb files that hold their own buffers and images, and this "
          "one reads \"props/paint.png\"");

    // An image's versions come from prompts, not parameters.
    const std::string icon =
        engine.call("asset.generate", R"({"prompt": "gem icon", "generator": "test"})")["id"]
            .as_string();
    CHECK(engine.fail("asset.refine", R"({"id": ")" + icon + R"(", "parameters": {"width": 2}})")
              .message == "/parameters: only a model takes parameters; the asset is an image");
    CHECK(engine.fail("asset.refine", R"({"id": ")" + icon + R"("})").data["path"] == "/prompt");
}

TEST_CASE("asset.refine rigs a figure with a generator and retargets it to the humanoid profile") {
    const fs::path root = fresh("library_rig");
    const Engine engine(root);
    auto glb = slash::scene3d::write_glb(test_humanoid::figure(true), "knight.glb", {});
    REQUIRE(glb.ok());
    write(root / "project" / "art" / "knight.glb", glb.value());
    const std::string figure =
        engine
            .call("asset.add", R"({"path": "art/knight.glb", "description": "A boxy knight.",
                                     "tags": ["character"]})")["id"]
            .as_string();

    // An added figure has no generator, so one that rigs is named.
    CHECK(
        engine.fail("asset.refine", R"({"id": ")" + figure + R"(", "rig": "humanoid"})").message ==
        "providers.json names no generator that makes rigs; add one under \"generators\", or "
        "pass generator \"test\" for placeholders");

    json::Value rigged =
        engine.call("asset.refine", R"({"id": ")" + figure +
                                        R"(", "rig": "humanoid", "generator": "test",
                                        "path": "characters/knight.glb"})");
    CHECK(rigged["parent"] == figure);
    CHECK(json::write(rigged["rig"], json::Style::Compact) ==
          R"({"mapped":23,"missing":[],"motion_scale":0.954})");
    auto model = slash::scene3d::load_model(read(root / "project" / "characters" / "knight.glb"),
                                            "characters/knight.glb", {});
    REQUIRE(model.ok());
    REQUIRE(model.value().skins.size() == 1);
    CHECK(slash::scene3d::find_node(model.value(), "LeftUpperArm") >= 0);
    CHECK(slash::scene3d::motion_scale(model.value()) == doctest::Approx(0.954F));

    json::Value info =
        engine.call("asset.info", R"({"id": ")" + rigged["id"].as_string() + R"("})");
    CHECK(info["description"] == "A boxy knight; rigged as a humanoid");
    CHECK(info["tags"].as_array().at(0) == "character");
    CHECK(info["request"]["rig"] == "humanoid");
    CHECK(info["generator"] == "test");

    // An exact repeat is the same asset, with no generator called.
    json::Value again = engine.call(
        "asset.refine", R"({"id": ")" + figure + R"(", "rig": "humanoid", "generator": "test"})");
    CHECK(again["id"] == rigged["id"]);
    CHECK(again["reused"] == true);

    // Only .glb figures are rigged, as they are; a rigged one isn't rigged again.
    CHECK(engine
              .fail("asset.refine", R"({"id": ")" + figure +
                                        R"(", "rig": "humanoid", "generator": "test",
                                        "width": 2})")
              .data["path"] == "/width");
    const std::string icon =
        engine
            .call("asset.generate",
                  R"({"prompt": "red gem", "generator": "test", "width": 8})")["id"]
            .as_string();
    CHECK(engine.fail("asset.refine", R"({"id": ")" + icon + R"(", "rig": "humanoid"})").message ==
          "/rig: only a model is rigged; the asset is an image");
    CHECK(engine
              .fail("asset.refine", R"({"id": ")" + rigged["id"].as_string() +
                                        R"(", "rig": "humanoid", "generator": "test"})")
              .message.find("the model is rigged already") != std::string::npos);
    CHECK(engine.fail("asset.refine", R"({"id": ")" + figure + R"(", "rig": "quadruped"})")
              .data["path"] == "/rig");
}

TEST_CASE("a recipe made of other model files goes into the library with them, and out again") {
    const fs::path root = fresh("library_composite");
    const Engine engine(root);
    const fs::path project = root / "project";
    write(project / "props" / "crate.model.json",
          R"({"format": 1, "parts": [{"name": "box", "shape": "box", "size": [1, 1, 1],
              "position": [0, 0.5, 0]}]})");
    slash::test::Gltf glb = slash::test::quad();
    write(project / "props" / "sign.glb", glb.glb());
    const std::string stall = R"({"format": 1, "parts": [
        {"name": "counter", "shape": "box", "size": [3, 1, 1], "position": [0, 0.5, 0]},
        {"name": "left", "model": "props/crate.model.json", "position": [-2, 0, 0]},
        {"name": "right", "model": "props/crate.model.json", "position": [2, 0, 0]},
        {"name": "sign", "model": "props/sign.glb", "position": [0, 2, 0]}]})";
    write(project / "props" / "stall.model.json", stall);

    // The part files go in with the recipe, which names them by their library IDs.
    json::Value added = engine.call(
        "asset.add", R"({"path": "props/stall.model.json", "description": "A market stall.",
                         "license": "CC0-1.0", "author": "Mira"})");
    CHECK(added["size"] == parse("[5, 3, 1]"));
    const std::string crate = added["model_parts"]["props/crate.model.json"].as_string();
    const std::string sign = added["model_parts"]["props/sign.glb"].as_string();
    const std::string first = added["id"].as_string();
    CHECK(read(project / "props" / "stall.model.json") == stall);
    json::Value info = engine.call("asset.info", R"({"id": ")" + first + R"("})");
    CHECK(info["uses"] == parse(R"([")" + crate + R"(", ")" + sign + R"("])"));
    info = engine.call("asset.info", R"({"id": ")" + crate + R"("})");
    CHECK(info["description"] == "crate, a part of: A market stall.");
    CHECK(info["license"] == "CC0-1.0");
    CHECK(info["author"] == "Mira");
    CHECK(info["used_by"] == parse(R"([")" + first + R"("])"));
    const library::Library kept((root / "library").string());
    const library::Record crate_record = kept.get(crate).value();
    const std::string in_library = kept.read(kept.get(first).value()).value();
    CHECK(in_library.find("\"asset:" + crate_record.id + ".model.json\"") != std::string::npos);
    CHECK(in_library.find("props/") == std::string::npos);
    json::Value list = parse(read(project / "library.json"));
    CHECK(list["assets"]["props/crate.model.json"]["id"] == crate_record.id);
    CHECK(list["assets"]["props/sign.glb"]["license"] == "CC0-1.0");

    // Using it copies its parts beside it; adding that copy again is the same asset.
    json::Value used = engine.call("asset.use", R"({"id": ")" + first +
                                                    R"(", "path": "market/stall.model.json"})");
    const std::string crate_copy = "market/parts/" + crate + ".model.json";
    const std::string sign_copy = "market/parts/" + sign + ".glb";
    CHECK(used["parts"] == parse(R"([")" + crate_copy + R"(", ")" + sign_copy + R"("])"));
    json::Value copied = parse(read(project / "market" / "stall.model.json"));
    CHECK(copied["parts"].as_array()[1]["model"] == crate_copy);
    CHECK(copied["parts"].as_array()[3]["model"] == sign_copy);
    CHECK(read(project / sign_copy) == glb.glb());
    json::Value again = engine.call("asset.add", R"({"path": "market/stall.model.json"})");
    CHECK(again["id"] == first);
    CHECK(again.find("parent") == nullptr);
    CHECK(again["model_parts"][crate_copy] == crate);

    // A part changed and added again is a new version, and says which recipes use the old one,
    // which asset.refine moves to it.
    write(project / "props" / "crate.model.json",
          R"({"format": 1, "parts": [{"name": "box", "shape": "box", "size": [1, 2, 1],
              "position": [0, 1, 0]}]})");
    json::Value taller = engine.call("asset.add", R"({"path": "props/crate.model.json"})");
    CHECK(taller["parent"] == crate);
    CHECK(taller["used_by"] == parse(R"([")" + first + R"("])"));
    const std::string tall = taller["id"].as_string();
    json::Value moved = engine.call("asset.refine", R"({"id": ")" + first + R"(", "parts": {")" +
                                                        crate + R"(": ")" + tall + R"("}})");
    CHECK(moved["parent"] == first);
    info = engine.call("asset.info", R"({"id": ")" + moved["id"].as_string() + R"("})");
    CHECK(info["uses"] == parse(R"([")" + tall + R"(", ")" + sign + R"("])"));
    CHECK(info["request"]["parts"] ==
          parse(R"({")" + crate_record.id + R"(": ")" + kept.get(tall).value().id + R"("})"));
    CHECK(info["description"] == "A market stall.");
    CHECK(engine
              .fail("asset.refine", R"({"id": ")" + first + R"(", "parts": {")" + tall + R"(": ")" +
                                        crate + R"("}})")
              .message == "/parts/" + tall + ": the recipe isn't made of \"" + tall +
                              "\"; it is made of \"" + crate + "\", \"" + sign + "\"");
    const std::string icon =
        engine.call("asset.generate", R"({"prompt": "gem icon", "generator": "test"})")["id"]
            .as_string();
    CHECK(engine
              .fail("asset.refine", R"({"id": ")" + first + R"(", "parts": {")" + crate +
                                        R"(": ")" + icon + R"("}})")
              .message ==
          "/parts/" + crate + ": a model part is a model; \"" + icon + "\" is an image");

    // Parts the library can't keep, and recipes that take in themselves.
    write(project / "props" / "lamp.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    write(project / "props" / "lit.model.json",
          R"({"format": 1, "parts": [{"model": "props/lamp.obj"}]})");
    CHECK(engine.fail("asset.add", R"({"path": "props/lit.model.json", "description": "A lamp."})")
              .message == "/path: the model part \"props/lamp.obj\": the library keeps model "
                          "parts that are .glb or .model.json files");
    write(project / "props" / "a.model.json",
          R"({"format": 1, "parts": [{"model": "props/b.model.json"}]})");
    write(project / "props" / "b.model.json",
          R"({"format": 1, "parts": [{"model": "props/a.model.json"}]})");
    CHECK(engine.fail("asset.add", R"({"path": "props/a.model.json", "description": "A loop."})")
              .message == "/path: \"props/a.model.json\" takes in itself through its model parts");

    // A library recipe names library models, never a project's files.
    library::Record record;
    CHECK(library::describe_file(stall, "stall.model.json", record).error().message ==
          "a model part names \"props/crate.model.json\": the library keeps recipes whose model "
          "parts are library models (\"asset:<id>.glb\"); asset.add puts a recipe's part files "
          "in with it");
    CHECK(library::ref_id("asset:" + crate_record.id + ".model.json") == crate_record.id);
    CHECK(library::ref_id("asset:" + crate_record.id + ".obj").empty());
    CHECK(library::ref_id("props/crate.model.json").empty());
}

TEST_CASE("asset commands report bad params with their paths") {
    const fs::path root = fresh("library_errors");
    const Engine engine(root);
    CHECK(engine.fail("asset.generate", R"({"prompt": "x"})").message ==
          "providers.json names no generator that makes images; add one under \"generators\", or "
          "pass generator \"test\" for placeholders");
    CHECK(engine.fail("asset.generate", R"({"prompt": "x", "generator": "tset"})").message ==
          "/generator: no generator named \"tset\"; did you mean \"test\"?");
    CHECK(engine
              .fail("asset.generate", R"({"prompt": "x", "generator": "test",
                                             "seconds": 1})")
              .message == "/seconds: images have a width and height, not seconds");
    CHECK(engine
              .fail("asset.generate", R"({"prompt": "x", "type": "sound", "generator": "test",
                                             "width": 4})")
              .message == "/width: sounds have seconds, not a width and height");
    CHECK(engine
              .fail("asset.generate", R"({"prompt": "x", "generator": "test",
                                             "path": "x.wav"})")
              .message == "/path: \"x.wav\" is not an image (.png)");
    CHECK(engine.fail("asset.info", R"({"id": "0123456789"})").message ==
          "/id: no asset with ID \"0123456789\" in the library");
    CHECK(engine.fail("asset.add", R"({"path": "missing.png", "description": "x"})").data["path"] ==
          "/path");

    // Sessions that run tests leave the library alone.
    const Engine tests(root, false);
    CHECK(tests.fail("asset.generate", R"({"prompt": "x", "generator": "test"})").message ==
          "tests don't write files, so the library stays as it was");

    // ai.providers lists the generators, never their keys.
    write(root / "project" / "providers.json",
          R"({"generators": {"art": {"type": "test", "api_key_env": "SLASH_TEST_UNSET_KEY"}}})");
    json::Value providers = engine.call("ai.providers", "{}");
    CHECK(json::write(providers["generators"], json::Style::Compact) ==
          R"({"art":{"api_key_env":"SLASH_TEST_UNSET_KEY","key_set":false,"type":"test"}})");
    CHECK(engine.call("asset.generate", R"({"prompt": "x"})")["id"].as_string().size() == 12);
}

TEST_CASE("asset.feedback ranks results, keeps rejected ones out of candidates and goes into "
          "prompts") {
    const fs::path root = fresh("library_feedback");
    const Engine engine(root);
    (void)engine.call("asset.generate", R"({"prompt": "slime sprite", "generator": "test",
                                            "width": 8, "seed": 1})");
    (void)engine.call("asset.generate", R"({"prompt": "slime sprite", "generator": "test",
                                            "width": 8, "seed": 2, "similar": false})");
    const auto ids = [&engine](std::string_view params) {
        std::vector<std::string> out;
        json::Value found = engine.call("asset.search", params);
        for (const json::Value& item : found["assets"].as_array()) {
            out.push_back(item.find("id")->as_string());
        }
        return out;
    };
    const std::vector<std::string> first_order = ids(R"({"text": "slime"})");
    const std::string first = first_order.front();
    const std::string second = first_order.back();

    // Rejecting the first moves it below the second.
    CHECK(json::write(engine.call("asset.feedback", R"({"id": ")" + first +
                                                        R"(", "verdict": "rejected",
                                                        "reason": "too dark to read."})"),
                      json::Style::Compact) == R"({"id":")" + first + R"(","rejected":1})");
    CHECK(ids(R"({"text": "slime"})") == std::vector<std::string>{second, first});
    json::Value found = engine.call("asset.search", R"({"text": "slime"})");
    CHECK(found["assets"].as_array()[1].find("score")->as_double() == doctest::Approx(0.95));
    CHECK(found["assets"].as_array()[1].find("rejected")->as_int() == 1);
    (void)engine.call("asset.feedback", R"({"id": ")" + second +
                                            R"(", "verdict": "accepted",
                                            "reason": "bright and readable", "source": "playtest"})");
    const json::Value info = engine.call("asset.info", R"({"id": ")" + second + R"("})");
    CHECK(json::write(*info.find("feedback"), json::Style::Compact) ==
          R"([{"reason":"bright and readable","source":"playtest","verdict":"accepted"}])");

    // A close request offers only the asset agents haven't rejected.
    json::Value close = engine.call("asset.generate", R"({"prompt": "a slime sprite",
        "generator": "test", "width": 8})");
    REQUIRE(close.find("candidates") != nullptr);
    REQUIRE(close["candidates"].as_array().size() == 1);
    CHECK(close["candidates"].as_array()[0].find("id")->as_string() == second);

    // What earlier feedback said goes along with the prompt of the next one like it.
    json::Value next = engine.call("asset.generate", R"({"prompt": "slime sprite, blue",
        "generator": "test", "width": 8, "similar": false})");
    const json::Value next_info =
        engine.call("asset.info", R"({"id": ")" + next["id"].as_string() + R"("})");
    CHECK(next_info.find("request")->find("context")->as_string() ==
          "What worked before: bright and readable.\nAvoid what was wrong before: too dark to "
          "read.");
    CHECK(next_info.find("request")->find("prompt")->as_string() == "slime sprite, blue");
    // context false sends the prompt alone.
    json::Value alone = engine.call("asset.generate", R"({"prompt": "slime sprite, red",
        "generator": "test", "width": 8, "similar": false, "context": false})");
    CHECK(engine.call("asset.info", R"({"id": ")" + alone["id"].as_string() + R"("})")
              .find("request")
              ->find("context") == nullptr);

    CHECK(engine.fail("asset.feedback", R"({"id": ")" + first + R"(", "verdict": "fine"})")
              .data["path"] == "/verdict");
}

TEST_CASE("asset.generate sends the storyboard's style and checks images against its palette") {
    const fs::path root = fresh("library_style");
    write(root / "project" / "storyboard" / "style.json", R"({"format": 1, "entries": {
        "tone/main": {"name": "Main", "summary": "Cozy pixel art with soft outlines"},
        "palette/art": {"name": "Art", "colors": {"sky": "#87ceeb"}, "links": ["art"]}
    }})");
    const Engine engine(root);

    // Under art/, the palette applies: the prompt names it, and the red sprite breaks it.
    json::Value made = engine.call("asset.generate", R"({"prompt": "red slime sprite",
        "generator": "test", "width": 8, "path": "art/slime.png"})");
    REQUIRE(made.find("problems") != nullptr);
    CHECK(made["problems"].as_array()[0].as_string().find("palette/art") != std::string::npos);
    json::Value info = engine.call("asset.info", R"({"id": ")" + made["id"].as_string() + R"("})");
    CHECK(info.find("request")->find("context")->as_string() ==
          "Style: Cozy pixel art with soft outlines.\nUse only these colors: sky #87ceeb.");
    const json::Value& feedback = info.find("feedback")->as_array()[0];
    CHECK(feedback.find("verdict")->as_string() == "rejected");
    CHECK(feedback.find("source")->as_string() == "storyboard.check");

    // Outside art/ only the tone applies, and nothing is checked.
    json::Value elsewhere = engine.call("asset.generate", R"({"prompt": "blue gem icon",
        "generator": "test", "width": 8, "path": "ui/gem.png"})");
    CHECK(elsewhere.find("problems") == nullptr);
    info = engine.call("asset.info", R"({"id": ")" + elsewhere["id"].as_string() + R"("})");
    CHECK(info.find("request")->find("context")->as_string() ==
          "Style: Cozy pixel art with soft outlines.");
    CHECK(info.find("feedback") == nullptr);

    // A palette the image fits records that it passed, once.
    write(root / "project" / "storyboard" / "style.json", R"({"format": 1, "entries": {
        "palette/art": {"name": "Art", "colors": {"sky": "#87ceeb"}, "tolerance": 255,
                        "links": ["art"]}
    }})");
    for (int i = 0; i < 2; ++i) {
        json::Value used = engine.call("asset.use", R"({"id": ")" + elsewhere["id"].as_string() +
                                                        R"(", "path": "art/gem.png"})");
        CHECK(used.find("problems") == nullptr);
    }
    info = engine.call("asset.info", R"({"id": ")" + elsewhere["id"].as_string() + R"("})");
    CHECK(json::write(*info.find("feedback"), json::Style::Compact) ==
          R"([{"reason":"fits the storyboard's palette and budget","source":"storyboard.check",)"
          R"("verdict":"accepted"}])");
}

namespace {

// A square image of one color, opaque inside a clear border a pixel wide.
std::string square(int size, std::array<std::uint8_t, 3> color) {
    slash::render::Image image;
    image.width = static_cast<std::uint32_t>(size);
    image.height = static_cast<std::uint32_t>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool inside = x > 0 && y > 0 && x < size - 1 && y < size - 1;
            image.pixels.insert(image.pixels.end(), {color[0], color[1], color[2],
                                                     inside ? std::uint8_t{255} : std::uint8_t{0}});
        }
    }
    return slash::render::encode_png(image).value();
}

} // namespace

TEST_CASE("asset.check finds library images hard to see or drawn too small, and test.run records "
          "it as feedback") {
    const fs::path root = fresh("library_check");
    const fs::path project = root / "project";
    write(project / "project.json", R"({"format": 1, "name": "Library check",
        "window": {"width": 160, "height": 90},
        "rendering": {"clear_color": [0.05, 0.05, 0.08, 1], "texture_filter": "nearest"}})");
    write(project / "art" / "slime.png", square(10, {30, 45, 30}));
    write(project / "art" / "heart.png", square(10, {230, 40, 60}));
    write(project / "art" / "sign.png", square(64, {200, 180, 120}));
    const Engine engine(root);
    std::array<std::string, 3> ids;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::array<std::string_view, 3> names{"slime", "heart", "sign"};
        ids.at(i) = engine
                        .call("asset.add", R"({"path": "art/)" + std::string(names.at(i)) +
                                               R"(.png", "description": "A test image"})")
                        .find("id")
                        ->as_string();
    }

    // The night level draws the slime dark on dark, the sign a fifth of its size and the heart,
    // through a prefab, where it stands out. The title screen draws none of them.
    write(project / "prefabs" / "heart.prefab.json", R"({"format": 1, "components": {
        "Sprite2D": {"texture": "art/heart.png"}, "Transform2D": {}}})");
    write(project / "levels" / "night.scene.json", R"({"format": 1, "entities": {
        "slime": {"components": {"Sprite2D": {"texture": "art/slime.png"},
                                 "Transform2D": {"position": [30, 45], "scale": [2, 2]}}},
        "sign": {"components": {"Sprite2D": {"texture": "art/sign.png"},
                                "Transform2D": {"position": [80, 45], "scale": [0.2, 0.2]}}},
        "heart": {"extends": "prefabs/heart.prefab.json",
                  "components": {"Transform2D": {"position": [130, 45]}}}}})");
    write(project / "title.scene.json", R"({"format": 1, "entities": {}})");

    const json::Value checked = engine.call("asset.check", "{}");
    CHECK(json::write(checked, json::Style::Compact) ==
          R"({"checked":3,"problems":[{"entity":"sign","id":")" + ids[2] +
              R"(","message":"drawn at 20% of its size, so an image 1/5 as wide and high would )"
              R"(look the same","path":"art/sign.png","scene":"levels/night.scene.json"},)"
              R"({"entity":"slime","id":")" +
              ids[0] +
              R"(","message":"hard to see: its edge has 1.3:1 contrast with the darker colors )"
              R"(around it","path":"art/slime.png","scene":"levels/night.scene.json"}],)"
              R"("scenes":1})");

    // What it found is each asset's feedback, recorded once however often it is checked.
    const json::Value tested = engine.call("test.run", "{}");
    CHECK(tested.find("assets")->as_array().size() == 2);
    const auto feedback = [&](const std::string& id) {
        return json::write(
            *engine.call("asset.info", R"({"id": ")" + id + R"("})").find("feedback"),
            json::Style::Compact);
    };
    CHECK(feedback(ids[0]) ==
          R"([{"reason":"hard to see: its edge has 1.3:1 contrast with the darker colors around )"
          R"x(it (levels/night.scene.json)","source":"asset.check","verdict":"rejected"}])x");
    CHECK(feedback(ids[1]) == R"([{"reason":"shows clearly in levels/night.scene.json",)"
                              R"("source":"asset.check","verdict":"accepted"}])");
    CHECK(engine.call("asset.search", R"({"text": "test image"})")
              .find("assets")
              ->as_array()[0]
              .find("id")
              ->as_string() == ids[1]);

    // A brighter slime passes; asked for one scene that doesn't draw the assets, nothing is seen.
    write(project / "art" / "slime.png", square(10, {120, 220, 120}));
    CHECK(engine.call("asset.check", "{}").find("problems")->as_array().size() == 1);
    CHECK(feedback(ids[0]).ends_with(R"({"reason":"shows clearly in levels/night.scene.json",)"
                                     R"("source":"asset.check","verdict":"accepted"}])"));
    CHECK(json::write(engine.call("asset.check", R"({"scenes": ["title.scene.json"], "ticks": 2})"),
                      json::Style::Compact) == R"({"checked":3,"problems":[],"scenes":1})");

    // Sessions that write nothing record nothing, and test.run skips the check where it isn't
    // allowed: here, with no read capability.
    const Engine reader(root, false);
    CHECK(reader.call("asset.check", "{}").find("problems")->as_array().size() == 1);
    CHECK(feedback(ids[2]).find("},{") == std::string::npos);
    command::SessionOptions options{
        .project_root = project.string(),
        .capabilities = {command::Capability::Run, command::Capability::Filesystem},
        .capture_logs = false,
        .library_dir = (root / "library").string()};
    command::Session limited(engine.components, engine.commands, options);
    const command::Reply run = limited.call("test.run", json::Object{});
    REQUIRE_MESSAGE(run.ok, run.message);
    CHECK(run.value.find("assets") == nullptr);
    CHECK(engine.fail("asset.check", R"({"scenes": ["../outside.scene.json"]})")
              .message.starts_with("/scenes/0: "));
}

TEST_CASE("a library recipe keeps the variants its model parts name") {
    const fs::path root = fresh("library_variants");
    const Engine engine(root);
    const fs::path project = root / "project";
    write(project / "props" / "crate.model.json",
          R"({"format": 1, "parts": [{"name": "box", "shape": "box"},
              {"name": "lid", "shape": "box", "size": [1, 0.1, 1], "position": [0, 0.55, 0]}],
              "variants": {"open": {"parts": {"lid": null}}}})");
    write(project / "props" / "pile.model.json", R"({"format": 1, "parts": [
        {"name": "shut", "model": "props/crate.model.json"},
        {"name": "open", "model": "props/crate.model.json::open", "position": [2, 0, 0]}]})");
    json::Value added = engine.call(
        "asset.add", R"({"path": "props/pile.model.json", "description": "Two crates."})");
    const std::string crate = added["model_parts"]["props/crate.model.json"].as_string();
    CHECK(added["model_parts"].as_object().size() == 1);
    const library::Library kept((root / "library").string());
    const library::Record crate_record = kept.get(crate).value();
    const std::string in_library = kept.read(kept.get(added["id"].as_string()).value()).value();
    CHECK(in_library.find("\"asset:" + crate_record.id + ".model.json::open\"") !=
          std::string::npos);
    json::Value info = engine.call("asset.info", R"({"id": ")" + added["id"].as_string() + R"("})");
    CHECK(info["uses"] == parse(R"([")" + crate + R"("])"));

    engine.call("asset.use",
                R"({"id": ")" + added["id"].as_string() + R"(", "path": "yard/pile.model.json"})");
    json::Value copied = parse(read(project / "yard" / "pile.model.json"));
    CHECK(copied["parts"].as_array()[1]["model"] == "yard/parts/" + crate + ".model.json::open");
}

TEST_CASE("library models keep to the storyboard's triangle budget, and asset.style drafts the "
          "style from accepted assets") {
    const fs::path root = fresh("library_model_style");
    const fs::path project = root / "project";
    write(project / "storyboard" / "style.json", R"({"format": 1, "entries": {
        "budget/props": {"name": "Props", "triangles": 1, "links": ["props"]}
    }})");
    write(project / "props" / "sign.glb", slash::test::quad().glb());
    write(project / "art" / "dot.png", square(4, {255, 255, 255}));
    const Engine engine(root);
    const std::string sign =
        engine.call("asset.add", R"({"path": "props/sign.glb", "description": "A sign"})")["id"]
            .as_string();
    const std::string dot =
        engine.call("asset.add", R"({"path": "art/dot.png", "description": "A dot"})")["id"]
            .as_string();
    engine.call("asset.feedback",
                R"({"id": ")" + sign + R"(", "verdict": "accepted", "reason": "reads from afar"})");
    engine.call("asset.feedback",
                R"({"id": ")" + dot + R"(", "verdict": "accepted", "reason": "clean edges"})");

    // Used again, the model breaks the budget: asset.use says so and records it, and asset.check
    // finds it in both places.
    json::Value used =
        engine.call("asset.use", R"({"id": ")" + sign + R"(", "path": "props/sign2.glb"})");
    CHECK(json::write(used["problems"], json::Style::Compact) ==
          R"(["has 2 triangles; budget/props allows 1"])");
    json::Value info = engine.call("asset.info", R"({"id": ")" + sign + R"("})");
    const json::Array& feedback = info["feedback"].as_array();
    REQUIRE(feedback.size() == 2);
    CHECK(json::write(feedback[1], json::Style::Compact) ==
          R"({"reason":"has 2 triangles; budget/props allows 1","source":"storyboard.check",)"
          R"("verdict":"rejected"})");
    json::Array problems = engine.call("asset.check", "{}")["problems"].as_array();
    std::vector<std::string> found;
    for (json::Value& problem : problems) {
        found.push_back(problem["path"].as_string() + " " + problem["message"].as_string());
    }
    CHECK(found == std::vector<std::string>{
                       "props/sign.glb has 2 triangles; budget/props allows 1",
                       "props/sign2.glb has 2 triangles; budget/props allows 1",
                   });
    json::Value checked = engine.call("storyboard.check", "{}");
    CHECK(checked["errors"].as_array().size() == 2);

    // The check's verdict isn't a person's: both assets were accepted, so asset.style drafts a
    // palette from the dot, a texture budget, and a reference for each, once.
    json::Value style = engine.call("asset.style", "{}");
    CHECK(style["accepted"] == 2);
    CHECK(json::write(style["edits"], json::Style::Compact) ==
          R"([{"id":"palette/main","set":{"colors":{"white":"#ffffff"},"links":["art"],)"
          R"("name":"Main palette","summary":"The colors of the accepted images.","tolerance":8},)"
          R"("version":null},{"id":"budget/assets","set":{"name":"Asset budgets","summary":)"
          R"("What the accepted assets need at most.","texture_size":4},"version":null},)"
          R"({"id":"reference/dot","set":{"links":["art/dot.png"],"name":"A dot","summary":)"
          R"("clean edges"},"version":null},{"id":"reference/sign","set":{"links":)"
          R"(["props/sign.glb"],"name":"A sign","summary":"reads from afar"},"version":null}])");
    json::Value edit;
    edit["edits"] = style["edits"];
    engine.call("storyboard.edit", json::write(edit, json::Style::Compact));
    CHECK(engine.call("asset.style", "{}")["edits"].as_array().empty());
}

TEST_CASE("a library model keeps its checks, every version passes them, and asset.check and "
          "test.run check the project's copy again") {
    const fs::path root = fresh("library_model_checks");
    const Engine engine(root);
    const fs::path project = root / "project";
    slash::test::Gltf glb = slash::test::quad();
    glb.document["nodes"] = parse(R"([{"name": "Quad", "mesh": 0, "children": [1]},
                                      {"name": "Hook", "translation": [0, 1, 0]}])");
    write(project / "props" / "sign.glb", glb.glb());
    const std::string add = R"({"path": "props/sign.glb", "description": "A sign.", "checks": )";
    CHECK(engine.fail("asset.add", add + R"({"nodes": ["Quad"], "max_triangles": 1}})").message ==
          "/checks/max_triangles: the model has 2 triangles, more than 1");
    CHECK(engine.fail("asset.add", add + R"({"nodes": "Quad"}})").data["path"] == "/checks/nodes");
    write(project / "art" / "dot.png", square(4, {255, 255, 255}));
    CHECK(
        engine
            .fail("asset.add", R"({"path": "art/dot.png", "description": "A dot.", "checks": {}})")
            .message == "/checks: only a model (.model.json or .glb) has checks");

    const std::string checks = R"({"size": {"max": [2, 2, 0.1]}, "nodes": ["Quad", "Hook"]})";
    json::Value added = engine.call("asset.add", add + checks + "}");
    CHECK(engine.call("asset.info", R"({"id": ")" + added["id"].as_string() + R"("})")["checks"] ==
          parse(checks));
    CHECK(parse(read(project / "library.json"))["assets"]["props/sign.glb"]["checks"] ==
          parse(checks));
    CHECK(engine.call("asset.check", "{}")["problems"].as_array().empty());

    // The project's copy loses its hook: asset.check and test.run say so, and the library won't
    // keep it as a version unless the call gives it checks it passes.
    write(project / "props" / "sign.glb", slash::test::quad().glb());
    json::Array problems = engine.call("asset.check", "{}")["problems"].as_array();
    REQUIRE(problems.size() == 1);
    CHECK(problems[0]["check"] == "/assets/props~1sign.glb/checks/nodes/1");
    CHECK(problems[0]["message"] == "the model has no node \"Hook\"");
    json::Value tested = engine.call("test.run", "{}");
    CHECK(json::write(tested["failed"], json::Style::Compact) ==
          R"([{"file":"library.json","message":"the model has no node \"Hook\"",)"
          R"("path":"/assets/props~1sign.glb/checks/nodes/1"}])");
    CHECK(engine.fail("asset.add", R"({"path": "props/sign.glb"})").message ==
          "/path: it fails the checks of the version before: checks/nodes/1: the model has no node "
          "\"Hook\"");
    json::Value version =
        engine.call("asset.add", R"({"path": "props/sign.glb", "checks": {"nodes": ["Quad"]}})");
    CHECK(version["parent"] == added["id"]);
    CHECK(engine.call("test.run", "{}")["failed"].as_array().empty());
}

namespace {

// An OpenAI-compatible chat server on the loopback interface that answers requests with its
// replies in turn, the last again after the rest, keeping each request's body.
class FakeVisionModel {
public:
    explicit FakeVisionModel(std::string answer)
        : FakeVisionModel(std::vector<std::string>{std::move(answer)}) {}
    explicit FakeVisionModel(std::vector<std::string> answers) : answers_(std::move(answers)) {
        server_.Post("/v1/chat/completions", [this](const httplib::Request& request,
                                                    httplib::Response& response) {
            const std::scoped_lock lock(mutex_);
            bodies_.push_back(parse(request.body));
            json::Value reply;
            reply["model"] = "vision-model";
            json::Value choice;
            choice["finish_reason"] = "stop";
            choice["message"]["role"] = "assistant";
            choice["message"]["content"] =
                answers_.at(std::min(bodies_.size(), answers_.size()) - 1);
            reply["choices"].push_back(std::move(choice));
            reply["usage"]["prompt_tokens"] = 900;
            reply["usage"]["completion_tokens"] = 40;
            response.set_content(json::write(reply, json::Style::Compact), "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~FakeVisionModel() {
        server_.stop();
        thread_.join();
    }

    FakeVisionModel(const FakeVisionModel&) = delete;
    FakeVisionModel& operator=(const FakeVisionModel&) = delete;

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

    [[nodiscard]] std::vector<json::Value> bodies() {
        const std::scoped_lock lock(mutex_);
        return bodies_;
    }

private:
    httplib::Server server_;
    std::vector<std::string> answers_;
    std::vector<json::Value> bodies_;
    std::mutex mutex_;
    int port_ = 0;
    std::thread thread_;
};

// A box recipe with a lid one meter across, and a lid alone, which is open.
constexpr std::string_view kChest = R"({"format": 1,
    "materials": {"oak": {"albedo_color": [0.6, 0.4, 0.2, 1]}},
    "parts": [{"name": "chest", "shape": "box", "material": "oak", "size": [1, 0.6, 0.6],
               "position": [0, 0.3, 0]}]})";

} // namespace

TEST_CASE("asset.review checks a model, draws it round and records what a vision model saw") {
    const fs::path root = fresh("library_review");
    const fs::path project = root / "project";
    write(project / "props" / "chest.model.json", kChest);
    write(project / "props" / "sign.glb", slash::test::quad().glb());
    write(project / "storyboard" / "style.json", R"({"format": 1, "entries": {
        "budget/props": {"name": "Props", "triangles": 4, "links": ["props"]}
    }})");
    FakeVisionModel vision(R"({"matches": false, "problems": [
        {"view": 2, "problem": "The lid has no hinges."},
        {"view": 9, "problem": "No such view."}]})");
    const Engine engine(root);
    const std::string chest =
        engine
            .call("asset.add",
                  R"({"path": "props/chest.model.json", "description": "An oak chest"})")["id"]
            .as_string();

    // No providers.json yet: the checks and the views, and no model asked. The chest's 12
    // triangles break the budget where it will go, so it is rejected, and that is its feedback.
    json::Value checked =
        engine.call("asset.review",
                    R"({"id": ")" + chest + R"(", "path": "props/chest2.model.json", "size": 64})");
    CHECK(checked == parse(R"({"verdict": "rejected",
        "errors": ["has 12 triangles; budget/props allows 4"],
        "size": [1.0, 0.6, 0.6], "triangles": 12, "holes": 0, "views": 4,
        "note": "no providers.json, so no model looked at the views", "recorded": true})"));
    json::Value info = engine.call("asset.info", R"({"id": ")" + chest + R"("})");
    CHECK(json::write(info["feedback"].as_array().back(), json::Style::Compact) ==
          R"({"reason":"has 12 triangles; budget/props allows 4","source":"asset.review",)"
          R"("verdict":"rejected"})");

    // A project model is reviewed where it is, and its views written; the sign is a quad, open
    // all round.
    json::Value sign =
        engine.call("asset.review",
                    R"({"path": "props/sign.glb", "size": 64, "views": 2, "out": "review/sign"})");
    CHECK(sign["warnings"] ==
          parse(R"(["has 1 hole (4 open edges), which show unless it is open on purpose"])"));
    CHECK(sign["views"] == parse(R"(["review/sign/view-1.png", "review/sign/view-2.png"])"));
    const slash::render::Image view = decode(read(project / "review" / "sign" / "view-1.png"));
    CHECK(view.width == 64);
    CHECK(view.height == 64);
    CHECK(sign.find("recorded") == nullptr);

    // With a model to ask, it sees the views and the request, and problems it gives no real
    // view for are left out.
    write(project / "providers.json", R"({"providers": {"local": {"type": "openai",
        "model": "vision-model", "url": ")" +
                                          vision.url() + R"("}}})");
    write(project / "storyboard" / "style.json", R"({"format": 1, "entries": {}})");
    json::Value seen = engine.call("asset.review",
                                   R"({"id": ")" + chest +
                                       R"(", "request": "A chest with brass hinges", "size": 64})");
    CHECK(seen["verdict"] == "rejected");
    CHECK(seen["review"] == parse(R"({"matches": false, "problems": [
        {"view": 2, "problem": "The lid has no hinges."}], "unverified": 1,
        "model": "vision-model"})"));
    const std::vector<json::Value> bodies = vision.bodies();
    REQUIRE(bodies.size() == 1);
    const json::Array& messages = bodies[0].find("messages")->as_array();
    const json::Array& content = messages[1].find("content")->as_array();
    CHECK(content.size() == 5); // the text and four views
    const std::string& asked = content.back().find("text")->as_string();
    CHECK(asked.find("Request: A chest with brass hinges") != std::string::npos);
    CHECK(asked.find("1 x 0.6 x 0.6 m, 12 triangles") != std::string::npos);
    CHECK(bodies[0].find("response_format") != nullptr);
    info = engine.call("asset.info", R"({"id": ")" + chest + R"("})");
    CHECK(info["feedback"].as_array().back()["reason"] == "The lid has no hinges.");

    // The same views are the same pixels, so a second look is the same request.
    engine.call("asset.review", R"({"id": ")" + chest +
                                    R"(", "request": "A chest with brass hinges", "size": 64,
                                    "record": false})");
    const std::vector<json::Value> again = vision.bodies();
    REQUIRE(again.size() == 2);
    CHECK(again[0] == again[1]);

    // What can't be reviewed.
    CHECK(engine.fail("asset.review", "{}").message.find("give the library model") !=
          std::string::npos);
    CHECK(engine.fail("asset.review", R"({"path": "props/sign.glb", "record": true})").data ==
          parse(R"({"path": "/record"})"));
    CHECK(engine.fail("asset.review", R"({"path": "props/sign.glb"})").data ==
          parse(R"({"path": "/request"})"));
    CHECK(engine.fail("asset.review", R"({"path": "props/none.glb", "ask": false})").data ==
          parse(R"({"path": "/path"})"));
    CHECK(engine
              .fail("asset.review", R"({"path": "props/sign.glb", "request": "A sign",
                                         "role": "critic"})")
              .data == parse(R"({"path": "/role"})"));
}

TEST_CASE("asset.generate makes a model from a concept image, reviewed until it passes") {
    const fs::path root = fresh("library_concept");
    const fs::path project = root / "project";
    write(project / "project.json", R"({"format": 1, "name": "Shelf"})");
    const Engine engine(root);
    const std::string concept_image =
        engine
            .call("asset.generate",
                  R"({"prompt": "a red ball, transparent sprite", "generator": "test",
                      "width": 32})")["id"]
            .as_string();
    const std::string beep =
        engine
            .call("asset.generate",
                  R"({"prompt": "beep", "type": "sound", "generator": "test"})")["id"]
            .as_string();

    // The test generator stands the image's shape on the ground, a meter across and a sixth of
    // that deep, the image kept as its parent; a repeat calls no generator.
    const std::string asked = R"({"type": "model", "prompt": "a red ball", "image": ")" +
                              concept_image + R"(", "generator": "test"})";
    json::Value ball = engine.call("asset.generate", asked);
    CHECK(ball["parent"] == concept_image);
    CHECK(ball["size"] == parse("[0.875, 0.875, 0.167]"));
    const std::string ball_id = ball["id"].as_string();
    CHECK(engine.call("asset.generate", asked)["reused"] == true);
    json::Value info = engine.call("asset.info", R"({"id": ")" + ball_id + R"("})");
    CHECK(info["format"] == "glb");
    CHECK(info["generator"] == "test");
    CHECK(info["request"]["image"].as_string().starts_with(concept_image));

    // The relief: the same image and prompt make the same bytes; thick is half its width deep,
    // thin one cell; and an image with nothing on it makes nothing.
    const std::shared_ptr<library::Generator> generator = library::test_generator();
    auto png = generator->generate(image("red ball, transparent sprite", 32));
    REQUIRE(png.ok());
    library::GenerateRequest relief;
    relief.type = "model";
    relief.prompt = "a thick red ball";
    relief.image_data = png.value().data;
    auto thick = generator->generate(relief);
    REQUIRE_MESSAGE(thick.ok(), thick.error().message);
    CHECK(thick.value().format == "glb");
    CHECK(generator->generate(relief).value().data == thick.value().data);
    library::Record described;
    REQUIRE(library::describe_file(thick.value().data, "ball.glb", described).ok());
    CHECK(described.size[0] == doctest::Approx(0.875));
    CHECK(described.size[2] == doctest::Approx(0.5));
    relief.prompt = "a thin red ball";
    REQUIRE(library::describe_file(generator->generate(relief).value().data, "ball.glb", described)
                .ok());
    CHECK(described.size[2] == doctest::Approx(0.042));
    slash::render::Image blank;
    blank.width = 4;
    blank.height = 4;
    blank.pixels.assign(64, 0);
    relief.image_data = slash::render::encode_png(blank).value();
    auto empty = generator->generate(relief);
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().message.find("shows nothing to make a model of") != std::string::npos);

    // Reviewed: the stand-in vision model finds the first too thin, so it is made again with
    // that in the prompt, thick now, accepted and copied to path.
    FakeVisionModel vision(std::vector<std::string>{
        R"({"matches": false, "problems": [{"view": 1, "problem": "It is too thin; make it thick."}]})",
        R"({"matches": true, "problems": []})",
        R"({"matches": false, "problems": [{"view": 3, "problem": "It has no stem."}]})"});
    write(project / "providers.json", R"({"providers": {"local": {"type": "openai",
        "model": "vision-model", "url": ")" +
                                          vision.url() + R"("}}})");
    json::Value reviewed =
        engine.call("asset.generate", R"({"type": "model", "prompt": "a red ball", "image": ")" +
                                          concept_image + R"(", "generator": "test", "review": 3,
                              "path": "props/ball.glb"})");
    REQUIRE(reviewed["attempts"].as_array().size() == 2);
    CHECK(reviewed["attempts"].as_array()[0] ==
          parse(R"({"id": ")" + ball_id + R"(", "verdict": "rejected",
        "reason": "It is too thin; make it thick"})"));
    CHECK(reviewed["attempts"].as_array()[1]["verdict"] == "accepted");
    CHECK(reviewed["id"] == reviewed["attempts"].as_array()[1]["id"]);
    CHECK(reviewed["review"]["verdict"] == "accepted");
    CHECK(reviewed["size"] == parse("[0.875, 0.875, 0.5]"));
    CHECK(reviewed["path"] == "props/ball.glb");
    CHECK(fs::exists(project / "props" / "ball.glb"));
    info = engine.call("asset.info", R"({"id": ")" + reviewed["id"].as_string() + R"("})");
    CHECK(info["request"]["prompt"] ==
          "a red ball\n\nThe last model was rejected: It is too thin; make it thick.");
    CHECK(info["feedback"].as_array().back()["verdict"] == "accepted");
    CHECK(vision.bodies().size() == 2);

    // None accepted: the last is returned and nothing is copied.
    json::Value refused = engine.call(
        "asset.generate", R"({"type": "model", "prompt": "a red mushroom", "image": ")" +
                              concept_image + R"(", "generator": "test", "review": 1,
                              "similar": false, "path": "props/mushroom.glb"})");
    CHECK(refused["review"]["verdict"] == "rejected");
    CHECK(refused.find("attempts") == nullptr);
    CHECK(refused["note"] == "no model made was accepted, so none was copied to path");
    CHECK_FALSE(fs::exists(project / "props" / "mushroom.glb"));

    // What a model can't be made from.
    CHECK(engine.fail("asset.generate", R"({"type": "model", "prompt": "a ball"})").data ==
          parse(R"({"path": "/image"})"));
    const command::Reply sound = engine.fail(
        "asset.generate", R"({"type": "model", "prompt": "a ball", "image": ")" + beep + R"("})");
    CHECK(sound.data == parse(R"({"path": "/image"})"));
    CHECK(sound.message.find("is a sound") != std::string::npos);
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a ball", "width": 4,
                                           "image": ")" +
                                          concept_image + R"("})")
              .data == parse(R"({"path": "/width"})"));
    CHECK(engine.fail("asset.generate", R"({"prompt": "a ball", "review": 2})").data ==
          parse(R"({"path": "/review"})"));
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a ball", "image": ")" +
                                          concept_image + R"(", "path": "props/ball.png"})")
              .data == parse(R"({"path": "/path"})"));
}

TEST_CASE("asset.generate makes a tileable material that library recipes share") {
    const fs::path root = fresh("library_material");
    const fs::path project = root / "project";
    write(project / "project.json", R"({"format": 1, "name": "Yard"})");
    const Engine engine(root);

    // The test generator draws rows of stones that don't tile; the material makes them tile,
    // with normal and roughness maps, each kept with the image as its parent.
    json::Value cobble = engine.call(
        "asset.generate", R"({"type": "material", "prompt": "grey cobblestones", "generator":
                              "test", "width": 128, "tags": ["stone"],
                              "path": "materials/cobble.standard_material.json"})");
    CHECK(cobble["type"] == "material");
    CHECK(cobble["image_seam"].as_double() > library::kSeamless);
    CHECK(cobble["seam"].as_double() < 1.0);
    CHECK(cobble["path"] == "materials/cobble.standard_material.json");
    CHECK(cobble["parts"].size() == 3);
    const std::string image_id = cobble["image"].as_string();
    for (const std::string_view map : {"albedo", "normal", "roughness"}) {
        json::Value info =
            engine.call("asset.info", R"({"id": ")" + cobble["maps"][map].as_string() + R"("})");
        CHECK(info["type"] == "image");
        CHECK(info["parent"].as_string().starts_with(image_id));
        CHECK(info["request"]["map"] == map);
    }
    json::Value made = engine.call("asset.info", R"({"id": ")" + image_id + R"("})");
    CHECK(made["request"]["prompt"].as_string().starts_with(
        "grey cobblestones, as a seamless tileable texture"));
    CHECK(made["description"] == "grey cobblestones");
    json::Value info =
        engine.call("asset.info", R"({"id": ")" + cobble["id"].as_string() + R"("})");
    CHECK(info["format"] == "standard_material.json");
    CHECK(info["uses"].size() == 3);
    CHECK(info["width"] == 128);
    CHECK(engine.call("asset.search", R"({"type": "material"})")["assets"].size() == 1);

    // The project's copy names its maps beside it.
    json::Value file = parse(read(project / "materials/cobble.standard_material.json"));
    CHECK(file["albedo_texture"] == "materials/cobble_albedo.png");
    CHECK(file["normal_texture"] == "materials/cobble_normal.png");
    CHECK(file["roughness_texture"] == "materials/cobble_roughness.png");
    CHECK(file["normal_enabled"] == true);
    CHECK(fs::exists(project / "materials/cobble_normal.png"));

    // A recipe naming the material goes into the library with it; copied into another project
    // path, it brings the material and its maps along.
    write(project / "props/wall.model.json",
          R"({"format": 1, "materials": {"cobble": {"extends":
              "materials/cobble.standard_material.json", "uv1_scale": [2, 2, 2]}},
              "parts": [{"name": "wall", "shape": "box", "size": [2, 1, 0.2],
                         "material": "cobble"}]})");
    write(project / "props/step.model.json",
          R"({"format": 1, "materials": {"cobble": "materials/cobble.standard_material.json"},
              "parts": [{"name": "step", "shape": "box", "size": [1, 0.2, 0.5],
                         "material": "cobble"}]})");
    json::Value wall = engine.call(
        "asset.add", R"({"path": "props/wall.model.json", "description": "a cobble wall"})");
    json::Value step = engine.call(
        "asset.add", R"({"path": "props/step.model.json", "description": "a cobble step"})");
    CHECK(wall["model_parts"]["materials/cobble.standard_material.json"] == cobble["id"]);
    CHECK(step["model_parts"]["materials/cobble.standard_material.json"] == cobble["id"]);
    json::Value shared =
        engine.call("asset.info", R"({"id": ")" + cobble["id"].as_string() + R"("})");
    CHECK(shared["used_by"].size() == 2);
    json::Value copied = engine.call("asset.use", R"({"id": ")" + wall["id"].as_string() +
                                                      R"(", "path": "town/wall.model.json"})");
    CHECK(copied["parts"].size() == 4);
    const std::string short_material = cobble["id"].as_string();
    json::Value recipe = parse(read(project / "town/wall.model.json"));
    CHECK(recipe["materials"]["cobble"]["extends"] ==
          "town/parts/" + short_material + ".standard_material.json");
    json::Value part_file =
        parse(read(project / ("town/parts/" + short_material + ".standard_material.json")));
    CHECK(part_file["albedo_texture"] == "town/parts/" + short_material + "_albedo.png");

    // Made of a library image instead, the image is the albedo when it tiles already.
    json::Value flat = engine.call(
        "asset.generate", R"({"prompt": "plain green", "generator": "test", "width": 16})");
    json::Value felt = engine.call("asset.generate", R"({"type": "material", "prompt": "felt",
                                                         "image": ")" +
                                                         flat["id"].as_string() + R"("})");
    CHECK(felt.find("image_seam") == nullptr);
    CHECK(felt["maps"]["albedo"] == flat["id"]);

    // Mistakes.
    CHECK(engine.fail("asset.generate", R"({"type": "material", "prompt": "x", "height": 32})")
              .message == "/height: a material's image is square: give its width");
    CHECK(engine
              .fail("asset.generate", R"({"type": "material", "prompt": "x", "width": 32,
                                          "image": ")" +
                                          flat["id"].as_string() + R"("})")
              .message == "/width: a material made of a library image keeps its size");
    CHECK(engine
              .fail("asset.generate", R"({"type": "material", "prompt": "x", "image": ")" +
                                          cobble["id"].as_string() + R"("})")
              .message ==
          "/image: a material is made of an image; " + cobble["id"].as_string() + " is a material");
    CHECK(engine
              .fail("asset.generate", R"({"type": "material", "prompt": "x", "generator": "test",
                                          "path": "materials/x.png"})")
              .message == "/path: \"materials/x.png\" is not a material (.standard_material.json)");
    CHECK(engine.fail("asset.generate", R"({"prompt": "x", "review": 2})").message ==
          "/review: only a model takes review; asked for an image");
    write(project / "materials/loose.standard_material.json",
          R"({"format": 1, "albedo_texture": "nowhere.png"})");
    CHECK(engine.fail("asset.add", R"({"path": "materials/loose.standard_material.json"})")
              .message.starts_with("/path: the texture \"nowhere.png\": "));
}

namespace {

// A box as an OBJ file, as CAD tools write one: in centimeters with z up, width wide and deep
// around the origin and height tall above it.
std::string obj_box(int width, int height) {
    const std::string w = std::to_string(width / 2);
    const std::string h = std::to_string(height);
    std::string out = "# a crate\n";
    for (const std::string& z : {std::string("0"), h}) {
        out += "v -" + w + " -" + w + " " + z + "\n";
        out += "v " + w + " -" + w + " " + z + "\n";
        out += "v " + w + " " + w + " " + z + "\n";
        out += "v -" + w + " " + w + " " + z + "\n";
    }
    return out + "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
}

} // namespace

TEST_CASE("asset.add imports models from other tools as .glb files, and edits as new versions") {
    const fs::path root = fresh("library_import");
    Engine engine(root);
    const fs::path project = root / "project";
    write(project / "props" / "crate.obj", obj_box(100, 80));

    // An OBJ file in centimeters with z up goes in cleaned up, as a .glb file.
    json::Value crate =
        engine.call("asset.add", R"({"path": "props/crate.obj", "description": "A wooden crate.",
                         "tags": ["prop"], "author": "Ana",
                         "cleanup": {"up": "z", "scale": 0.01, "pivot": [0.5, 0, 0.5],
                                     "collision": "convex"}})");
    CHECK(crate["type"] == "model");
    CHECK(crate["source"] == "crate.obj");
    CHECK(crate["size"] == parse("[1, 0.8, 1]"));
    CHECK(crate["triangles"] == 12);
    CHECK(crate["cleanup"]["triangles"] == parse("[12, 12]"));
    CHECK(crate["cleanup"]["pivot"] == parse("[0.5, 0, 0.5]"));
    CHECK(crate["cleanup"]["collision"] == parse(R"({"convex": 1})"));
    REQUIRE(crate.find("problems") != nullptr);
    CHECK(crate["problems"].as_array().at(0).as_string().starts_with("no license is recorded"));
    const std::string first = crate["id"].as_string();
    json::Value info = engine.call("asset.info", R"({"id": ")" + first + R"("})");
    CHECK(info["format"] == "glb");
    CHECK(info["source"] == "crate.obj");
    CHECK(info["source_sha256"] == slash::sha256_hex(obj_box(100, 80)).substr(0, 12));
    CHECK(info["cleanup"]["up"] == "z");
    json::Value list = parse(read(project / "library.json"));
    CHECK(list["assets"]["props/crate.obj"]["source_sha256"] ==
          slash::sha256_hex(obj_box(100, 80)));
    CHECK(engine.call("asset.check", "{}")["problems"] == parse("[]"));

    // The artist makes it taller: asset.check sees the change, and adding it again keeps a new
    // version, cleaned up as the last one was and reviewed.
    write(project / "props" / "crate.obj", obj_box(100, 120));
    json::Value checked = engine.call("asset.check", "{}");
    REQUIRE(checked["problems"].as_array().size() == 1);
    CHECK(checked["problems"].as_array().at(0)["path"] == "props/crate.obj");
    CHECK(checked["problems"].as_array().at(0)["message"] ==
          "changed since it was imported; asset.add it again to keep the change as a new version");
    json::Value taller = engine.call(
        "asset.add", R"({"path": "props/crate.obj", "license": "CC0-1.0", "review": true})");
    CHECK(taller["parent"] == first);
    CHECK(taller["size"] == parse("[1, 1.2, 1]"));
    CHECK(taller["cleanup"]["collision"] == parse(R"({"convex": 1})"));
    CHECK(taller.find("problems") == nullptr);
    CHECK(taller["review"]["verdict"] == "accepted");
    CHECK(taller["review"]["holes"] == 0);
    info = engine.call("asset.info", R"({"id": ")" + taller["id"].as_string() + R"("})");
    CHECK(info["description"] == "A wooden crate.");
    CHECK(info["tags"] == parse(R"(["prop"])"));
    CHECK(info["author"] == "Ana");
    CHECK(info["license"] == "CC0-1.0");
    CHECK(info["versions"] == parse(R"([")" + first + R"("])"));
    CHECK(info["feedback"].as_array().at(0)["source"] == "asset.review");
    CHECK(engine.call("asset.check", "{}")["problems"] == parse("[]"));

    // Another project takes the .glb file, which isn't a source.
    engine.call("asset.use",
                R"({"id": ")" + taller["id"].as_string() + R"(", "path": "levels/crate.glb"})");
    list = parse(read(project / "library.json"));
    CHECK(list["assets"]["levels/crate.glb"].find("source_sha256") == nullptr);
    auto model =
        slash::scene3d::load_model(read(project / "levels" / "crate.glb"), "crate.glb", {});
    REQUIRE(model.ok());
    CHECK_FALSE(model.value().collision.empty());

    // A .gltf file reading an image beside it goes in as a .glb file holding the image.
    slash::test::Gltf painted = slash::test::quad(0);
    painted.document["images"] = parse(R"([{"uri": "paint.png"}])");
    painted.document["textures"] = parse(R"([{"source": 0}])");
    painted.document["materials"] =
        parse(R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])");
    write(project / "props" / "sign.gltf", painted.gltf());
    write(project / "props" / "paint.png", square(4, {200, 40, 40}));
    json::Value sign = engine.call(
        "asset.add",
        R"({"path": "props/sign.gltf", "description": "A red sign.", "license": "CC0-1.0"})");
    CHECK(sign["source"] == "sign.gltf");
    CHECK(sign.find("cleanup") == nullptr);
    info = engine.call("asset.info", R"({"id": ")" + sign["id"].as_string() + R"("})");
    CHECK(info["format"] == "glb");
    CHECK(info.find("cleanup") == nullptr);

    // What can't be imported or cleaned up.
    write(project / "art" / "dot.png", square(4, {0, 0, 0}));
    CHECK(
        engine
            .fail("asset.add", R"({"path": "art/dot.png", "description": "A dot.", "cleanup": {}})")
            .message == "/cleanup: only a model is cleaned up");
    CHECK(engine
              .fail("asset.add",
                    R"({"path": "art/dot.png", "description": "A dot.", "review": true})")
              .message == "/review: only a model is reviewed");
    write(project / "props" / "ball.model.json",
          R"({"format": 1, "parts": [{"shape": "sphere"}]})");
    CHECK(engine
              .fail("asset.add", R"({"path": "props/ball.model.json", "description": "A ball.",
                                     "cleanup": {"scale": 2}})")
              .data["path"] == "/cleanup");
    write(project / "props" / "box.obj", obj_box(10, 10));
    CHECK(engine
              .fail("asset.add", R"({"path": "props/box.obj", "description": "A box.",
                                     "cleanup": {"scale": 2, "height": 1}})")
              .message == "/cleanup/height: give scale or height, not both");
    CHECK(engine
              .fail("asset.add", R"({"path": "props/box.obj", "description": "A box.",
                                     "cleanup": {"shrink": 2}})")
              .data["path"] == "/cleanup/shrink");
    write(project / "props" / "broken.fbx", "not an fbx file");
    CHECK(engine.fail("asset.add", R"({"path": "props/broken.fbx", "description": "Junk."})")
              .data["path"] == "/path");
    fs::remove_all(root);
}

TEST_CASE("asset.generate builds models with Blender scripts, kept with the script to build "
          "again") {
    const fs::path root = fresh("library_blender");
    const fs::path project = root / "project";
    json::Value providers;
    providers["generators"]["blender"]["type"] = "blender";
    providers["generators"]["blender"]["options"]["program"] = SLASH_FAKE_BLENDER;
    write(project / "providers.json", json::write(providers));
    const std::string script = "import bpy\n"
                               "bpy.ops.mesh.primitive_cube_add(size=1)\n"
                               "box = bpy.context.object\n"
                               "box.name = params['name']\n"
                               "box.scale = (params['width'], params['depth'], params['height'])\n"
                               "bpy.ops.object.transform_apply(scale=True)\n";
    write(project / "tools" / "crate.py", script);
    Engine engine(root);

    // The script is a project file; Blender's stand-in builds a box as its params say.
    json::Value crate =
        engine.call("asset.generate", R"({"type": "model", "prompt": "a shipping crate",
                              "script": "tools/crate.py", "tags": ["prop"],
                              "parameters": {"name": "Crate", "width": 1.2, "depth": 0.8,
                                             "height": 0.6},
                              "path": "props/crate.glb"})");
    CHECK(crate["size"] == parse("[1.2, 0.6, 0.8]"));
    CHECK(crate["triangles"] == 12);
    CHECK(crate["path"] == "props/crate.glb");
    const std::string first = crate["id"].as_string();
    json::Value info = engine.call("asset.info", R"({"id": ")" + first + R"("})");
    CHECK(info["generator"] == "blender");
    CHECK(info["model"] == "blender 4.2");
    CHECK(info["request"]["script"] == script);
    CHECK(info["request"]["parameters"]["width"] == 1.2);
    CHECK(info["parts"] == parse(R"(["Crate"])"));
    CHECK(info["description"] == "a shipping crate");

    // The same script and parameters give the same asset, with no Blender run.
    json::Value again =
        engine.call("asset.generate", R"({"type": "model", "prompt": "a shipping crate",
                              "script": "tools/crate.py",
                              "parameters": {"name": "Crate", "width": 1.2, "depth": 0.8,
                                             "height": 0.6}})");
    CHECK(again["id"] == first);
    CHECK(again["reused"] == true);

    // A version with a parameter changed is built again with the script it was made with.
    write(project / "tools" / "crate.py", "raise ValueError('edited since')\n");
    json::Value taller =
        engine.call("asset.refine", R"({"id": ")" + first + R"(", "parameters": {"height": 1.5}})");
    CHECK(taller["parent"] == first);
    CHECK(taller["size"] == parse("[1.2, 1.5, 0.8]"));
    info = engine.call("asset.info", R"({"id": ")" + taller["id"].as_string() + R"("})");
    CHECK(info["request"]["script"] == script);
    CHECK(info["request"]["parameters"] == parse(R"({"depth": 0.8, "height": 1.5,
                                                     "name": "Crate", "width": 1.2})"));
    CHECK(info["tags"] == parse(R"(["prop"])"));
    // Refining back to the first parameters is the first asset.
    CHECK(engine.call("asset.refine", R"({"id": ")" + taller["id"].as_string() +
                                          R"(", "parameters": {"height": 0.6}})")["id"] == first);

    // A script that raises fails with Blender's last lines; the edited file now does.
    command::Reply failed =
        engine.fail("asset.refine", R"({"id": ")" + first + R"(", "script": "tools/crate.py"})");
    CHECK(failed.message.starts_with("Blender couldn't run the script (exit code 1): "));
    CHECK(failed.message.find("ValueError: the script raised") != std::string::npos);
    CHECK(failed.message.find("; its log is build-") != std::string::npos);
    CHECK(fs::exists(project / ".slash" / "blender"));

    // What a script's model doesn't take, and who may run Blender.
    CHECK(engine.fail("asset.refine", R"({"id": ")" + first + R"(", "prompt": "make it taller"})")
              .message == "/prompt: a script built the model: give new parameters or a new script");
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a box",
                                          "script": "import bpy", "image": "abc123"})")
              .message == "/image: a script builds the model; it takes no image");
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a box",
                                          "script": "import bpy", "review": 2})")
              .data["path"] == "/review");
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a box",
                                             "parameters": {"width": 1}})")
              .message == "/parameters: only a model a script builds takes parameters");
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a box",
                                             "script": "tools/missing.py"})")
              .data["path"] == "/script");
    CHECK(engine
              .fail("asset.generate", R"({"type": "model", "prompt": "a box",
                                             "script": "import bpy", "generator": "test"})")
              .message == "/generator: the generator \"test\" makes no models from scripts");
    {
        Engine reading(root);
        command::SessionOptions options = reading.session->options();
        std::erase(options.capabilities, command::Capability::Build);
        command::Session session(reading.components, reading.commands, options);
        // A repeat needs no Blender; a new script does.
        CHECK(session
                  .call("asset.generate", parse(R"({"type": "model", "prompt": "a shipping crate",
                              "script": )" + json::write(json::Value(script)) +
                                                R"(, "parameters": {"name": "Crate", "width": 1.2,
                              "depth": 0.8, "height": 0.6}})"))
                  .value["id"] == first);
        command::Reply refused =
            session.call("asset.generate",
                         parse(R"({"type": "model", "prompt": "a box", "script": "import bpy"})"));
        CHECK(refused.code == command::code::kPermissionDenied);
        CHECK(refused.message ==
              "running a Blender script needs the build capability (--allow build)");
    }
    fs::remove_all(root);
}
