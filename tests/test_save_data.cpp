// Save data for game code (slash/platform/save_data.hpp): slots written whole and read back, the
// process's save folder a game and a session set, and the save.* commands over a project's.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/save_data.hpp"
#include "slash/runtime/game.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace command = slash::command;
namespace json = slash::json;
namespace platform = slash::platform;
namespace runtime = slash::runtime;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return parsed.value();
}

// An empty folder for the test.
std::string fresh(const std::string& name) {
    std::filesystem::remove_all(name);
    return name;
}

// A project folder with a main scene, at version.
std::string project(const std::string& root, std::string_view version) {
    std::filesystem::remove_all(root);
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(
        platform::write_file(root + "/main.scene.json", R"({"entities": {}, "format": 1})").ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "Saver", "version": ")" +
                                     std::string(version) +
                                     R"(", "main_scene": "main.scene.json"})")
                .ok());
    return root;
}

} // namespace

TEST_CASE("Save slots are written whole and read back with the game version") {
    const platform::SaveStore store{.folder = fresh("save_data_slots") + "/saves",
                                    .game_version = "1.2.0"};
    CHECK(platform::save_slots(store).empty());
    CHECK_FALSE(platform::has_save(store, "slot1"));
    CHECK(platform::load_game(store, "slot1").error().code == slash::ErrorCode::NotFound);

    const json::Value data = parse(R"({"level": 3, "hp": 72.5, "items": ["key", "map"]})");
    REQUIRE(platform::save_game(store, "slot1", data).ok());
    CHECK(platform::has_save(store, "slot1"));
    auto loaded = platform::load_game(store, "slot1");
    REQUIRE(loaded.ok());
    CHECK(loaded.value().data == data);
    CHECK(loaded.value().game_version == "1.2.0");
    // "2026-11-26T20:15:00Z"
    REQUIRE(loaded.value().saved.size() == 20);
    CHECK(loaded.value().saved[10] == 'T');
    CHECK(loaded.value().saved.back() == 'Z');
    // Nothing is left beside it.
    CHECK_FALSE(platform::exists(store.folder + "/slot1.save.json.new"));

    // A slot is replaced whole; slots list newest first, then by name.
    REQUIRE(platform::save_game(store, "slot1", parse(R"({"level": 4})")).ok());
    REQUIRE(platform::save_game(store, "auto-save", json::Value(true)).ok());
    CHECK(platform::load_game(store, "slot1").value().data == parse(R"({"level": 4})"));
    const auto slots = platform::save_slots(store);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].saved >= slots[1].saved);
    if (slots[0].saved == slots[1].saved) {
        CHECK(slots[0].name == "auto-save");
    }
    CHECK(slots[1].bytes > 0);

    // Deleting empties a slot, and an empty one too.
    REQUIRE(platform::delete_save(store, "slot1").ok());
    CHECK_FALSE(platform::has_save(store, "slot1"));
    CHECK(platform::delete_save(store, "slot1").ok());
    CHECK(platform::save_slots(store).size() == 1);
}

TEST_CASE("Save slots refuse other names, other files and later formats") {
    const platform::SaveStore store{.folder = fresh("save_data_bad") + "/saves",
                                    .game_version = "1.0.0"};
    for (const std::string_view slot : {"", "Slot", "a/b", "..", "a b", "save.json"}) {
        CHECK_FALSE(platform::valid_save_slot(slot));
        CHECK(platform::save_game(store, slot, json::Value(1)).error().code ==
              slash::ErrorCode::InvalidArgument);
    }
    CHECK(platform::valid_save_slot(std::string(64, 'a')));
    CHECK_FALSE(platform::valid_save_slot(std::string(65, 'a')));
    CHECK(platform::save_game({.folder = "", .game_version = ""}, "slot", json::Value(1))
              .error()
              .code == slash::ErrorCode::InvalidArgument);

    REQUIRE(platform::create_directories(store.folder).ok());
    REQUIRE(platform::write_file(store.folder + "/broken.save.json", "{").ok());
    REQUIRE(platform::write_file(store.folder + "/other.save.json", R"({"level": 1})").ok());
    REQUIRE(platform::write_file(store.folder + "/later.save.json", R"({"format": 2, "data": {}})")
                .ok());
    REQUIRE(platform::write_file(store.folder + "/notes.txt", "hi").ok());
    for (const std::string_view slot : {"broken", "other", "later"}) {
        CHECK(platform::load_game(store, slot).error().code == slash::ErrorCode::InvalidArgument);
    }
    CHECK(platform::load_game(store, "later").error().message.find("later save format") !=
          std::string::npos);
    CHECK(platform::save_slots(store).empty());
}

TEST_CASE("A headless game saves in the project's folder, the process's save folder") {
    const std::string root = project("save_data_game", "2.0.0");
    auto game = runtime::Game::start({.data_dir = root, .headless = true, .max_ticks = 1});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    CHECK(platform::save_store().folder == platform::project_save_folder(root));
    CHECK(platform::save_store().game_version == "2.0.0");
    CHECK(platform::save_data_ready());
    // Game code saves with the process's folder.
    REQUIRE(platform::save_game("quick", parse(R"({"x": 1})")).ok());
    CHECK(platform::has_save("quick"));
    CHECK(platform::load_game("quick").value().game_version == "2.0.0");
    CHECK(platform::exists(root + "/.slash/saves/quick.save.json"));
    REQUIRE(platform::save_slots().size() == 1);
    REQUIRE(platform::delete_save("quick").ok());
    CHECK(platform::save_slots().empty());
}

TEST_CASE("save.* commands read and write the project's save slots") {
    const std::string root = project("save_data_commands", "0.3.0");
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    {
        command::Session session(components, commands, {.project_root = root});
        // Game code in the session saves where the commands look.
        CHECK(platform::save_store().folder == platform::project_save_folder(root));
        const auto call = [&](std::string_view name, std::string_view params) {
            return session.call(name, parse(params));
        };
        CHECK(call("save.list", "{}").value == parse(R"({"slots": []})"));
        auto written = call("save.write", R"({"slot": "slot1", "data": {"coins": 12}})");
        REQUIRE_MESSAGE(written.ok, written.message);
        CHECK(written.value.find("game_version")->as_string() == "0.3.0");
        auto read = call("save.read", R"({"slot": "slot1"})");
        REQUIRE(read.ok);
        CHECK(*read.value.find("data") == parse(R"({"coins": 12})"));
        CHECK(read.value.find("saved")->as_string() == written.value.find("saved")->as_string());
        auto listed = call("save.list", "{}");
        REQUIRE(listed.value.find("slots")->size() == 1);
        CHECK(listed.value.find("slots")->as_array()[0].find("slot")->as_string() == "slot1");
        CHECK_FALSE(call("save.read", R"({"slot": "Slot1"})").ok);
        CHECK_FALSE(call("save.read", R"({"slot": "none"})").ok);
        CHECK(call("save.delete", R"({"slot": "slot1"})").value.find("deleted")->as_bool());
        CHECK_FALSE(call("save.delete", R"({"slot": "slot1"})").value.find("deleted")->as_bool());
    }
    // A test's session writes nothing.
    command::Session tests(components, commands, {.project_root = root, .write_files = false});
    CHECK_FALSE(tests.call("save.write", parse(R"({"slot": "a", "data": 1})")).ok);
    CHECK(tests.call("save.list", parse("{}")).ok);
}
