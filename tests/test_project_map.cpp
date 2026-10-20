#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/cpp_outline.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

const command::CppSymbol* symbol(const command::CppOutline& outline, std::string_view name) {
    const auto found = std::ranges::find_if(
        outline.symbols, [&](const command::CppSymbol& item) { return item.name == name; });
    return found == outline.symbols.end() ? nullptr : &*found;
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

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params, int expected_code) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == expected_code, reply.message);
        return reply;
    }
};

} // namespace

TEST_CASE("outline_cpp reads declarations, macros, calls and comments without compiling") {
    const command::CppOutline outline = command::outline_cpp(R"cpp(// The player.
#include "player.hpp"
#include <vector>
#define HELPER(x) void x()

namespace game {

// Coins an entity holds.
// Never negative.
struct Wallet {
    int coins = 0;
};
SLASH_COMPONENT(Wallet, coins)

enum class Mood : int { Calm, Angry };
using Purse = Wallet;

/** Moves the player.
 *  Runs every tick. */
void move(slash::world::World& world, double /*tick_seconds*/) {
    if (slash::input::is_action_just_pressed(world, "jump")) {
        world.emit("jumped", "player", {{"height", 2}});
    }
    for (const auto& event : world.events()) {
        if (event.kind == "coin_collected") {}
    }
    const char* text = R"(not "a" call)";
    const char* joined = "one " "two";
}

class Player : public Base {
public:
    // Starts a jump.
    [[nodiscard]] bool jump(int height = {}) const;
    Player(int speed) : speed_(speed) {}
    bool operator==(const Player& other) const = default;

private:
    int speed_ = 0;
};

template <typename T>
T twice(T value) { return value + value; }

bool Player::jump(int height) const { return height > 0; }

const auto kLambda = [](int x) { return x; };
extern "C" { int c_function(void); }

} // namespace game

SLASH_GAME_MODULE(game) {
    game.add_system("move", game::move, {.doc = "Moves.", .priority = 20});
}
)cpp");

    CHECK(outline.lines == 54);
    CHECK(outline.includes == std::vector<std::string>{"player.hpp"});

    const command::CppSymbol* wallet = symbol(outline, "Wallet");
    REQUIRE(wallet != nullptr);
    CHECK(wallet->kind == "struct");
    CHECK(wallet->scope == "game");
    CHECK(wallet->line == 10);
    CHECK(wallet->doc == "Coins an entity holds. Never negative.");
    REQUIRE(wallet->fields.size() == 1);
    CHECK(wallet->fields[0].declaration == "int coins = 0");
    CHECK(wallet->fields[0].access == "public");
    CHECK(wallet->fields[0].line == 11);
    CHECK(wallet->signature == "struct Wallet");
    const command::CppSymbol* mood = symbol(outline, "Mood");
    CHECK(mood->kind == "enum");
    CHECK(mood->signature == "enum class Mood : int");
    REQUIRE(mood->fields.size() == 2);
    CHECK(mood->fields[0].declaration == "Calm");
    CHECK(mood->fields[1].declaration == "Angry");
    CHECK(symbol(outline, "Purse")->kind == "alias");
    CHECK(symbol(outline, "Purse")->signature == "using Purse = Wallet");

    const command::CppSymbol* move = symbol(outline, "move");
    REQUIRE(move != nullptr);
    CHECK(move->definition);
    CHECK(move->line == 20);
    CHECK(move->doc == "Moves the player. Runs every tick.");
    CHECK(move->signature == "void move(slash::world::World& world, double)");
    CHECK(std::ranges::binary_search(move->body_identifiers, "event"));

    // Members keep their class; one defined outside it is named with it.
    const command::CppSymbol* jump = symbol(outline, "jump");
    REQUIRE(jump != nullptr);
    CHECK_FALSE(jump->definition);
    CHECK(jump->member_of == "Player");
    CHECK(jump->scope == "game::Player");
    CHECK(jump->doc == "Starts a jump.");
    CHECK(jump->signature == "bool jump(int height = {}) const");
    CHECK(jump->access == "public");
    const command::CppSymbol* player = symbol(outline, "Player");
    CHECK(player->kind == "class");
    CHECK(player->signature == "class Player : public Base");
    // Data members keep their access and initializer.
    REQUIRE(player->fields.size() == 1);
    CHECK(player->fields[0].declaration == "int speed_ = 0");
    CHECK(player->fields[0].access == "private");
    CHECK(symbol(outline, "Player::jump")->definition);
    // A constructor's signature leaves out its member initializers.
    const auto constructor = std::ranges::find_if(outline.symbols, [](const auto& item) {
        return item.name == "Player" && item.kind == "function";
    });
    REQUIRE(constructor != outline.symbols.end());
    CHECK(constructor->signature == "Player(int speed)");
    CHECK(symbol(outline, "operator==") != nullptr);
    CHECK(symbol(outline, "twice")->signature == "template <typename T> T twice(T value)");
    CHECK(symbol(outline, "c_function") != nullptr);
    CHECK(symbol(outline, "kLambda") == nullptr);
    CHECK(symbol(outline, "x") == nullptr); // in the #define
    REQUIRE(outline.defines.size() == 1);
    CHECK(outline.defines[0].name == "HELPER");
    CHECK(outline.defines[0].params == "(x)");
    CHECK(outline.defines[0].line == 4);

    REQUIRE(outline.macros.size() == 2);
    CHECK(outline.macros[0].name == "SLASH_COMPONENT");
    CHECK(outline.macros[0].args == std::vector<std::string>{"Wallet", "coins"});
    CHECK(outline.macros[1].name == "SLASH_GAME_MODULE");

    const auto literal = [&](std::string_view value) -> const command::CppString& {
        const auto found = std::ranges::find_if(
            outline.strings, [&](const command::CppString& item) { return item.value == value; });
        REQUIRE(found != outline.strings.end());
        return *found;
    };
    CHECK(literal("jump").callee == "is_action_just_pressed");
    CHECK(literal("jump").arg == 1);
    CHECK(literal("jump").function == "move");
    CHECK(literal("jumped").callee == "emit");
    CHECK(literal("jumped").args ==
          std::vector<std::string>{R"("jumped")", R"("player")", R"({{"height", 2}})"});
    CHECK(literal("coin_collected").compared == "kind");
    CHECK(literal(R"(not "a" call)").callee.empty());
    CHECK(literal("one two").line == 28);
    const command::CppString& system = literal("move");
    CHECK(system.callee == "add_system");
    CHECK(system.function == "SLASH_GAME_MODULE(game)");
    CHECK(system.args == std::vector<std::string>{R"("move")", "game::move",
                                                  R"({.doc = "Moves.", .priority = 20})"});
    CHECK(std::ranges::binary_search(outline.identifiers, "Wallet"));
}

TEST_CASE("project.map says where scenes, code, components, systems, events and actions are") {
    const std::string root = "project_map_project";
    std::filesystem::remove_all(root);
    for (const char* folder : {"/art", "/build", "/prefabs", "/scenes", "/src", "/storyboard"}) {
        REQUIRE(slash::platform::create_directories(root + folder).ok());
    }
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Coins", "main_scene": "scenes/main.scene.json",
        "gameplay": {"sources": ["src"]},
        "input": {"jump": {"events": []}, "dash": {"events": []}}})");
    write("art/coin.png", "png");
    write("art/hud.png", "png");
    write("build/stale.scene.json", R"({"entities": {}, "format": 1})");
    write("prefabs/coin.prefab.json", R"({"components": {
        "Sprite2D": {"texture": "art/coin.png"}, "Transform2D": {}}, "format": 1})");
    write("scenes/main.scene.json", R"({"entities": {
        "player": {"components": {"Transform2D": {}, "Wallet": {"coins": 3},
                                  "StoryRef": {"ids": ["character/pilot"]}}},
        "coin_1": {"extends": "prefabs/coin.prefab.json"},
        "coin_2": {"extends": "prefabs/coin.prefab.json"},
        "hud": {"components": {"Sprite2D": {"texture": "art/hud.png"}}}}, "format": 1})");
    write("storyboard/world.json", R"({"entries": {
        "character/pilot": {"name": "Pilot", "links": ["src/player.cpp"]},
        "place/town": {"name": "Town", "links": ["scenes"]}}, "format": 1})");
    write("src/game.cpp", R"(#include "player.hpp"

namespace {

struct Wallet {
    int coins = 0;
};
SLASH_COMPONENT(Wallet, coins)

void collect(slash::world::World& world, double) {
    world.ecs().view<Wallet>().each([&](Wallet& wallet) {
        ++wallet.coins;
        world.emit("coin_collected");
    });
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<Wallet>();
    game.add_system("collect", collect, {.doc = "Collects coins.", .priority = 20});
    game.add_system("move_player", game::move_player);
}
)");
    write("src/player.hpp", R"(#pragma once

namespace game {

void move_player(slash::world::World& world, double tick_seconds);

class Player {
public:
    void jump();
};

} // namespace game
)");
    write("src/player.cpp", R"(#include "player.hpp"

namespace game {

void move_player(slash::world::World& world, double) {
    if (slash::input::is_action_just_pressed(world, "jump")) {
        world.emit("jumped");
    }
    for (const auto& event : world.events()) {
        if (event.kind == "coin_collected") {
            show("art/hud.png");
        }
    }
}

void Player::jump() {}

} // namespace game
)");

    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    // Every part, systems aside, which also hold the engine's own.
    const json::Value map = engine.ok("project.map", R"({"sections": ["scenes", "prefabs",
        "sources", "components", "events", "actions"]})");
    CHECK(map == parse(R"({
        "actions": {"dash": [], "jump": ["src/player.cpp:6"]},
        "components": {
            "Sprite2D": {"prefabs": ["prefabs/coin.prefab.json"], "scenes": ["scenes/main.scene.json"]},
            "StoryRef": {"scenes": ["scenes/main.scene.json"]},
            "Transform2D": {"prefabs": ["prefabs/coin.prefab.json"],
                            "scenes": ["scenes/main.scene.json"]},
            "Wallet": {"declared": "src/game.cpp:8", "scenes": ["scenes/main.scene.json"],
                       "sources": ["src/game.cpp"]}},
        "events": {
            "coin_collected": {"emitted": ["src/game.cpp:13"], "read": ["src/player.cpp:10"]},
            "jumped": {"emitted": ["src/player.cpp:7"]}},
        "prefabs": {"prefabs/coin.prefab.json": {"components": {"Sprite2D": 1, "Transform2D": 1},
                                                 "uses": ["art/coin.png"]}},
        "project": {"main_scene": "scenes/main.scene.json", "name": "Coins", "sources": ["src"]},
        "scenes": {"scenes/main.scene.json": {
            "components": {"Sprite2D": 3, "StoryRef": 1, "Transform2D": 3, "Wallet": 1},
            "entities": 4, "prefabs": ["prefabs/coin.prefab.json"], "uses": ["art/hud.png"],
            "story": ["character/pilot", "place/town"]}},
        "sources": {
            "src/game.cpp": {"components": ["Wallet"], "events": ["coin_collected"],
                             "functions": {"collect": 10}, "includes": ["src/player.hpp"],
                             "lines": 23, "systems": ["collect", "move_player"],
                             "types": {"Wallet": 5}},
            "src/player.cpp": {"actions": ["jump"], "events": ["coin_collected", "jumped"],
                               "functions": {"Player::jump": 16, "move_player": 5},
                               "includes": ["src/player.hpp"], "lines": 18,
                               "story": ["character/pilot"], "uses": ["art/hud.png"]},
            "src/player.hpp": {"functions": {"Player::jump": 9, "move_player": 5}, "lines": 12,
                               "types": {"Player": 7}}}})"));

    // The game's systems are listed where their functions are, even before the code is built.
    json::Value systems = engine.ok("project.map", R"({"find": "e", "sections": ["systems"]})");
    CHECK(systems == parse(R"({"project": {"main_scene": "scenes/main.scene.json", "name": "Coins",
        "sources": ["src"]}, "systems": [
        {"components": ["Wallet"], "loaded": false, "name": "collect", "priority": 20,
         "source": "src/game.cpp:10"},
        {"loaded": false, "name": "move_player", "source": "src/player.cpp:5"}]})"));
    engine.session->world().add_system("collect", [](world::World&, double) {},
                                       {.doc = "Collects coins.", .priority = 20});
    systems = engine.ok("project.map", R"({"find": "collect", "sections": ["systems"]})");
    CHECK(*systems.find("systems") == parse(R"([{"components": ["Wallet"], "doc": "Collects coins.",
        "name": "collect", "priority": 20, "source": "src/game.cpp:10"}])"));
    // Without a filter the engine's systems come too, in the order they run, by name and priority.
    const json::Value all = engine.ok("project.map", R"({"sections": ["systems"]})");
    CHECK(all.find("systems")->size() > 2);

    // What uses a file: the prefab that draws it, and what it touches.
    CHECK(engine.ok("project.map", R"({"paths": ["art/coin.png"]})") == parse(R"({
        "components": {"Sprite2D": {"prefabs": ["prefabs/coin.prefab.json"]},
                       "Transform2D": {"prefabs": ["prefabs/coin.prefab.json"]}},
        "prefabs": {"prefabs/coin.prefab.json": {"components": {"Sprite2D": 1, "Transform2D": 1},
                                                 "uses": ["art/coin.png"]}},
        "project": {"main_scene": "scenes/main.scene.json", "name": "Coins", "sources": ["src"]}})"));

    // Where a name is: each list keeps only what matches.
    const json::Value coin = engine.ok("project.map", R"({"find": "Coin_Coll"})");
    CHECK(*coin.find("events") == parse(R"({"coin_collected": {"emitted": ["src/game.cpp:13"],
        "read": ["src/player.cpp:10"]}})"));
    CHECK(*coin.find("sources") == parse(R"({
        "src/game.cpp": {"events": ["coin_collected"], "lines": 23},
        "src/player.cpp": {"events": ["coin_collected"], "lines": 18}})"));
    CHECK(coin.find("scenes") == nullptr);

    command::Reply missing = engine.fails("project.map", R"({"paths": ["art/coins.png"]})", -32001);
    CHECK(missing.message == R"(no file or folder "art/coins.png"; did you mean "art/coin.png"?)");
    CHECK(*missing.data.find("path") == "/paths/0");
    engine.fails("project.map", R"({"paths": ["../outside"]})", -32002);
}
