#include "gltf.hpp"

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
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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
          ".model.json or .glb");
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
    CHECK(engine.fail("asset.generate", R"({"prompt": "a chair", "type": "model"})").data["path"] ==
          "/type");
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
