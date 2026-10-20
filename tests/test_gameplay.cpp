#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/gameplay.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/library.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace gameplay = slash::gameplay;
namespace runtime = slash::runtime;
namespace command = slash::command;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

using Names = std::vector<std::string>;

Names names(const json::Value& list) {
    Names out;
    for (const json::Value& item : list.as_array()) {
        out.push_back(item.as_string());
    }
    return out;
}

// What a component of an entity holds.
json::Value fields(const world::World& w, std::string_view id, std::string_view type) {
    const world::ComponentType* found = w.components().find(type);
    REQUIRE(found != nullptr);
    return found->get(w, w.find(id));
}

world::ComponentRegistry builtin_components() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    return components;
}

bool has_system(const world::World& w, std::string_view name) {
    const std::vector<std::string_view> systems = w.system_names();
    return std::ranges::find(systems, name) != systems.end();
}

#if defined(SLASH_TEST_GAMEPLAY_DIR)

// A project folder whose gameplay library is one of the test libraries.
struct Project {
    std::string dir;

    explicit Project(std::string_view name) : dir("gameplay_" + std::string(name)) {
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    [[nodiscard]] std::string library() const { return gameplay::library_path(dir); }

    // "Builds" the code: puts a test library where the project's would go, newer than the last.
    void build(std::string_view variant) const {
        const fs::path to = library();
        fs::create_directories(to.parent_path());
        std::optional<fs::file_time_type> before;
        if (fs::exists(to)) {
            before = fs::last_write_time(to);
        }
        fs::copy_file(fs::path(SLASH_TEST_GAMEPLAY_DIR) /
                          ("gameplay_" + std::string(variant) + ".so"),
                      to, fs::copy_options::overwrite_existing);
        if (before && fs::last_write_time(to) <= *before) {
            fs::last_write_time(to, *before + std::chrono::seconds(1));
        }
    }
};

constexpr std::string_view kScene = R"({
    "entities": {
        "hero": {"components": {"Health": {"armor": 3, "current": 5}}},
        "rock": {"components": {"Transform2D": {}}}
    },
    "format": 1
})";

// A world with the engine's components and a project's code.
struct Game {
    world::ComponentRegistry components = builtin_components();
    world::World world{components};
    gameplay::Host host;

    explicit Game(const Project& project) : host(components, project.library()) {}
};

#endif

} // namespace

TEST_CASE("a game's code is found at .slash/gameplay in its project") {
#if defined(_WIN32)
    CHECK(gameplay::library_path("") == ".slash/gameplay/game.dll");
#else
    CHECK(gameplay::library_path("") == ".slash/gameplay/game.so");
    CHECK(gameplay::library_path(".") == ".slash/gameplay/game.so");
    CHECK(gameplay::library_path("games/pong") == "games/pong/.slash/gameplay/game.so");
    CHECK(gameplay::library_path("games/pong/") == "games/pong/.slash/gameplay/game.so");
#endif
}

TEST_CASE("a game module records registration mistakes") {
    world::GameModule module;
    struct Local {
        static void nothing(world::World& /*world*/, double /*seconds*/) {}
    };
    module.add_system("tick", Local::nothing);
    module.add_system("tick", Local::nothing);
    module.add_system("empty", nullptr);
    REQUIRE(module.errors().size() == 2);
    CHECK(module.errors()[0] == "the system tick is added twice");
    CHECK(module.errors()[1] == "the system empty has nothing to run");
    CHECK(module.systems().size() == 1);
}

namespace linked_game {

struct Coins {
    int count = 0;
};
SLASH_COMPONENT(Coins, count)

void module(world::GameModule& game) {
    game.add_component<Coins>().set_doc("Coins the player holds.");
    game.add_system("collect", [](world::World& w, double /*seconds*/) {
        for (const entt::entity entity : w.ecs().view<Coins>()) {
            ++w.ecs().get<Coins>(entity).count;
        }
    });
}

} // namespace linked_game

TEST_CASE("code linked into the program goes in when the host is attached") {
    world::ComponentRegistry components = builtin_components();
    world::World w(components);
    gameplay::Host host(components, linked_game::module);
    const gameplay::Load load = host.attach(w);
    CHECK(load.error.empty());
    CHECK(load.added == Names{"Coins"});
    CHECK(host.loaded());
    CHECK(host.library().empty());
    CHECK(components.find("Coins")->doc() == "Coins the player holds.");
    REQUIRE(world::load_scene(w, parse(R"({"entities": {"bag": {"components": {"Coins": {}}}},
                                           "format": 1})")));
    w.step(1.0 / 60.0);
    CHECK(fields(w, "bag", "Coins") == parse(R"({"count": 1})"));
    // Linked code never changes.
    CHECK_FALSE(host.changed());
    CHECK_FALSE(host.poll().has_value());
    const json::Value status = host.status();
    CHECK(status.find("library") == nullptr);
    CHECK(names(*status.find("components")) == Names{"Coins"});
    CHECK(names(*status.find("systems")) == Names{"collect"});
}

#if defined(SLASH_TEST_GAMEPLAY_DIR)

TEST_CASE("a project's gameplay library registers its components and systems") {
    const Project project("loads");
    project.build("v1");
    Game game(project);
    const gameplay::Load load = game.host.attach(game.world);
    CHECK_MESSAGE(load.error.empty(), load.error);
    CHECK(load.added == Names{"Health"});
    CHECK(game.host.loaded());
    CHECK(game.components.find("Health")->doc() == "Hit points.");
    CHECK(has_system(game.world, "regenerate"));
    REQUIRE(world::load_scene(game.world, parse(kScene)));
    game.world.step(1.0 / 60.0);
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"armor": 3, "current": 6})"));
    // Nothing more until the file changes.
    CHECK_FALSE(game.host.changed());
    CHECK_FALSE(game.host.poll().has_value());
    // The copy it loaded is gone.
    CHECK(fs::is_empty(fs::path(project.dir) / ".slash/gameplay/loaded"));
}

TEST_CASE("a project with no gameplay library built loads nothing until there is one") {
    const Project project("missing");
    Game game(project);
    const gameplay::Load load = game.host.attach(game.world);
    CHECK(load.error.empty());
    CHECK(load.added.empty());
    CHECK_FALSE(game.host.loaded());
    CHECK_FALSE(game.host.changed());
    CHECK_FALSE(game.host.poll().has_value());
    project.build("v1");
    CHECK(game.host.changed());
    const auto loaded = game.host.poll();
    REQUIRE(loaded.has_value());
    CHECK(loaded->added == Names{"Health"});
    CHECK(game.host.loaded());
}

TEST_CASE("until the code is built its components are stand-ins, which it replaces") {
    const Project project("stand_ins");
    Game game(project);
    REQUIRE(game.host.attach(game.world).error.empty());
    game.host.add_stand_ins({{.name = "Health", .fields = {"current", "armor", "old"}},
                             {.name = "Mana", .fields = {"value"}},
                             {.name = "Transform2D", .fields = {"x"}}}); // the engine's: skipped
    CHECK(game.components.find("Transform2D")->fields().size() > 1);
    REQUIRE(world::load_scene(game.world, parse(R"({"entities": {
        "hero": {"components": {"Health": {"current": 5, "old": true}, "Mana": {"value": 2}}}},
        "format": 1})")));
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"current": 5, "old": true})"));

    // The code's Health replaces its stand-in and takes the values its fields still have; Mana,
    // which the code doesn't register, stays a stand-in.
    project.build("v1");
    const auto load = game.host.poll();
    REQUIRE(load.has_value());
    CHECK_MESSAGE(load->error.empty(), load->error);
    CHECK(load->added == Names{"Health"});
    CHECK(load->changed.empty());
    REQUIRE(load->dropped.size() == 1);
    CHECK(load->dropped[0] == "Health.old: no longer in the code (1 entity)");
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"armor": 2, "current": 5})"));
    CHECK(fields(game.world, "hero", "Mana") == parse(R"({"value": 2})"));
    CHECK(names(*game.host.status().find("components")) == Names{"Health"});
}

TEST_CASE("rebuilt code is swapped in, and each entity keeps the fields the code still has") {
    const Project project("swap");
    project.build("v1");
    Game game(project);
    REQUIRE(game.host.attach(game.world).error.empty());
    REQUIRE(world::load_scene(game.world, parse(kScene)));
    game.world.step(1.0 / 60.0);

    // Version 2 drops Health.armor, adds Health.shield and Speed, and heals 2 a tick.
    project.build("v2");
    CHECK(game.host.changed());
    const auto load = game.host.poll();
    REQUIRE(load.has_value());
    CHECK_MESSAGE(load->error.empty(), load->error);
    CHECK(load->added == Names{"Speed"});
    CHECK(load->changed == Names{"Health"});
    CHECK(load->removed.empty());
    REQUIRE(load->dropped.size() == 2);
    CHECK(load->dropped[0] == "Health.armor: no longer in the code (1 entity)");
    // The old code kept a component it never registered.
    CHECK(contains(load->dropped[1], "Scratch: not registered, so reloading can't keep it"));
    CHECK(load->to_json() == parse(R"({"added": ["Speed"], "changed": ["Health"],
                                       "dropped": [")" +
                                   load->dropped[0] + R"(", ")" + load->dropped[1] + R"("]})"));

    CHECK(game.world.size() == 2);
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"current": 6, "shield": 0})"));
    CHECK(has_system(game.world, "haste"));
    game.world.step(1.0 / 60.0);
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"current": 8, "shield": 0})"));
    const json::Value status = game.host.status();
    CHECK(names(*status.find("components")) == Names{"Health", "Speed"});
    CHECK(names(*status.find("systems")) == Names{"haste", "regenerate"});
    // Scenes save the new fields.
    CHECK(world::save_scene(game.world)
              .find("entities")
              ->find("hero")
              ->find("components")
              ->find("Health")
              ->find("current")
              ->as_double() == doctest::Approx(8.0));

    // Version 3 holds text in Health.current, and has no Speed and no systems.
    project.build("v3");
    const auto third = game.host.poll();
    REQUIRE(third.has_value());
    CHECK_MESSAGE(third->error.empty(), third->error);
    CHECK(third->removed == Names{"Speed"});
    CHECK(third->changed == Names{"Health"});
    REQUIRE(third->dropped.size() == 3);
    CHECK(contains(third->dropped[0], "Health.current: "));
    CHECK(third->dropped[1] == "Health.shield: no longer in the code (1 entity)");
    CHECK(contains(third->dropped[2], "Scratch: not registered"));
    CHECK(fields(game.world, "hero", "Health") == parse(R"({"current": "full"})"));
    CHECK_FALSE(has_system(game.world, "regenerate"));
    CHECK_FALSE(has_system(game.world, "haste"));
    CHECK(game.components.find("Speed") == nullptr);
}

TEST_CASE("worlds that share component types share the host, and swaps reach each") {
    const Project project("worlds");
    project.build("v1");
    Game game(project);
    REQUIRE(game.host.attach(game.world).error.empty());
    world::World other(game.components);
    CHECK(game.host.attach(other).error.empty());
    CHECK(has_system(other, "regenerate"));
    REQUIRE(world::load_scene(game.world, parse(kScene)));
    REQUIRE(world::load_scene(other, parse(kScene)));

    project.build("v2");
    const auto load = game.host.poll();
    REQUIRE(load.has_value());
    CHECK(load->dropped.front() == "Health.armor: no longer in the code (2 entities)");
    for (world::World* w : {&game.world, &other}) {
        CHECK(fields(*w, "hero", "Health") == parse(R"({"current": 5, "shield": 0})"));
        CHECK(has_system(*w, "haste"));
    }
    game.host.detach(other);
    CHECK_FALSE(has_system(other, "haste"));
    CHECK_FALSE(has_system(other, "regenerate"));
    CHECK(has_system(game.world, "haste"));
}

TEST_CASE("code that can't go in changes nothing, and the old code keeps running") {
    const Project project("refused");
    project.build("v1");
    Game game(project);
    game.world.add_system("engine_system", [](world::World& /*w*/, double /*seconds*/) {});
    REQUIRE(game.host.attach(game.world).error.empty());
    REQUIRE(world::load_scene(game.world, parse(kScene)));

    struct Case {
        std::string_view variant;
        std::string_view error;
    };
    for (const Case& refused : {
             Case{"duplicate", "the component Health is registered twice"},
             Case{"engine_component", "the component Transform2D is already the engine's"},
             Case{"engine_system", "the system engine_system is already the engine's"},
             Case{"no_entry", "the gameplay library has no SLASH_GAME_MODULE function"},
             Case{"bad_abi", "the gameplay library was built against other engine headers; "
                             "build it again"},
         }) {
        CAPTURE(refused.variant);
        project.build(refused.variant);
        const auto load = game.host.poll();
        REQUIRE(load.has_value());
        CHECK(load->error == refused.error);
        CHECK(*game.host.status().find("error") == refused.error);
        CHECK(game.host.loaded());
        CHECK(names(*game.host.status().find("components")) == Names{"Health"});
        const double before = fields(game.world, "hero", "Health").find("current")->as_double();
        game.world.step(1.0 / 60.0);
        CHECK(fields(game.world, "hero", "Health").find("current")->as_double() ==
              doctest::Approx(before + 1.0));
    }
    // Fixed code goes in.
    project.build("v2");
    const auto fixed = game.host.poll();
    REQUIRE(fixed.has_value());
    CHECK(fixed->error.empty());
    CHECK(game.host.status().find("error") == nullptr);
}

TEST_CASE("sessions load a project's code, report reloads and undo across them") {
    const Project project("session");
    project.build("v1");
    world::ComponentRegistry components = builtin_components();
    command::Registry commands;
    command::register_builtin_commands(commands);
    gameplay::Host host(components, project.library());
    command::SessionOptions options;
    options.project_root = project.dir;
    options.gameplay = &host;
    command::Session session(components, commands, options);
    const auto ok = [&](std::string_view name, std::string_view params) {
        command::Reply reply = session.call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    };

    const json::Value described = ok("engine.describe", "{}");
    const json::Value* code = described.find("gameplay");
    REQUIRE(code != nullptr);
    CHECK(code->find("loaded")->as_bool());
    CHECK(names(*code->find("components")) == Names{"Health"});
    CHECK(names(*code->find("systems")) == Names{"regenerate"});

    ok("entity.create", R"({"id": "hero", "components": {"Health": {"armor": 3, "current": 5}}})");
    ok("component.set", R"({"id": "hero", "components": {"Health": {"current": 7}}})");
    const std::string subscription = ok("events.subscribe", R"({"kinds": ["gameplay.reloaded"]})")
                                         .find("subscription")
                                         ->as_string();

    // Any command swaps rebuilt code in first.
    project.build("v2");
    ok("world.query", "{}");
    const json::Value events = ok("events.read", R"({"subscription": ")" + subscription + R"("})");
    const json::Value& list = *events.find("events");
    REQUIRE(list.size() == 1);
    CHECK(list.as_array()[0].find("kind")->as_string() == "gameplay.reloaded");
    CHECK(*list.as_array()[0].find("data") ==
          parse(R"json({"added": ["Speed"], "changed": ["Health"],
                        "dropped": ["Health.armor: no longer in the code (1 entity)"]})json"));
    CHECK(names(*ok("engine.describe", "{}").find("gameplay")->find("components")) ==
          Names{"Health", "Speed"});

    // Undo goes back to a world saved under the old code: what no longer fits is left out.
    ok("undo", "{}");
    CHECK(fields(session.world(), "hero", "Health") == parse(R"({"current": 5, "shield": 0})"));

    // Tests run in sessions of their own, with the game's systems.
    fs::create_directories(fs::path(project.dir) / "tests");
    REQUIRE(slash::platform::write_file(project.dir + "/tests/heal.test.json", R"({
        "tests": {"heals": {"steps": [
            {"command": "entity.create", "params": {"id": "a", "components": {"Health": {}}}},
            {"command": "step", "params": {"ticks": 3}},
            {"command": "entity.get", "params": {"id": "a"},
             "expect": {"components": {"Health": {"current": 16}}}}
        ]}}
    })"));
    const json::Value tested = ok("test.run", "{}");
    CHECK(tested.find("passed")->as_int() == 1);
    CHECK(has_system(session.world(), "haste"));
}

TEST_CASE("projects with no gameplay code describe none") {
    const Project project("none");
    world::ComponentRegistry components = builtin_components();
    command::Registry commands;
    command::register_builtin_commands(commands);
    gameplay::Host host(components, project.library());
    command::SessionOptions options;
    options.project_root = project.dir;
    options.gameplay = &host;
    command::Session session(components, commands, options);
    const command::Reply described = session.call("engine.describe", json::Object{});
    REQUIRE(described.ok);
    CHECK(described.value.find("gameplay") == nullptr);
}

#endif

TEST_CASE("the components a project's sources declare are read from their text") {
    const std::string dir = "gameplay_declared";
    fs::remove_all(dir);
    fs::create_directories(dir + "/src/enemies");
    REQUIRE(slash::platform::write_file(dir + "/src/game.cpp", R"(
struct Health { float current = 10.0F; float armor = 0.0F; };
SLASH_COMPONENT(Health, current, armor)
struct Tag {};
SLASH_COMPONENT( Tag )
#define MY_SLASH_COMPONENT(Type, ...) SLASH_COMPONENT(Type, __VA_ARGS__)
MY_SLASH_COMPONENT(Hidden, value)
)"));
    REQUIRE(slash::platform::write_file(dir + "/src/enemies/bat.hpp",
                                        "SLASH_COMPONENT(Bat,\n    wings,\n    speed)\n"));
    REQUIRE(slash::platform::write_file(dir + "/src/notes.txt", "SLASH_COMPONENT(Notes, text)\n"));
    runtime::Project project;
    project.gameplay_sources = {"src"};
    const std::vector<runtime::DeclaredComponent> declared =
        runtime::declared_components(dir, project);
    REQUIRE(declared.size() == 3);
    CHECK(declared[0].name == "Bat");
    CHECK(declared[0].fields == Names{"wings", "speed"});
    CHECK(declared[1].name == "Health");
    CHECK(declared[1].fields == Names{"current", "armor"});
    CHECK(declared[2].name == "Tag");
    CHECK(declared[2].fields.empty());

    // Without the library, scenes are checked against stand-ins: names and fields.
    world::ComponentRegistry registry = builtin_components();
    CHECK(runtime::add_gameplay_types(registry, dir, project, false) ==
          runtime::GameplayTypes::Declared);
    CHECK(world::validate_scene(registry, parse(R"({"entities": {"a": {"components": {
                                    "Health": {"current": 3, "armor": "anything"}}}},
                                    "format": 1})"))
              .empty());
    const auto errors = world::validate_scene(
        registry,
        parse(R"({"entities": {"a": {"components": {"Health": {"curent": 3}}}}, "format": 1})"));
    REQUIRE_FALSE(errors.empty());
    CHECK(contains(errors.front().path, "/Health"));
    CHECK_FALSE(
        world::validate_scene(
            registry, parse(R"({"entities": {"a": {"components": {"Mana": {}}}}, "format": 1})"))
            .empty());
    // Loading such a scene keeps the stand-ins' values as written, and saving writes them back.
    world::World w(registry);
    const json::Value scene =
        parse(R"({"entities": {"a": {"components": {"Health": {"current": 3}}}}, "format": 1})");
    REQUIRE(world::load_scene(w, scene));
    CHECK(w.size() == 1);
    CHECK(fields(w, "a", "Health") == parse(R"({"current": 3})"));
    CHECK(*world::save_scene(w).find("entities") == *scene.find("entities"));
    std::string path;
    REQUIRE(registry.find("Health")->assign(w, w.find("a"), parse(R"({"armor": [1, 2]})"), path));
    CHECK(fields(w, "a", "Health") == parse(R"({"armor": [1, 2], "current": 3})"));
    registry.find("Health")->remove(w, w.find("a"));
    CHECK_FALSE(registry.find("Health")->has(w, w.find("a")));

    // A project with no gameplay code adds nothing.
    world::ComponentRegistry plain;
    CHECK(runtime::add_gameplay_types(plain, dir, runtime::Project{}, false) ==
          runtime::GameplayTypes::None);
    CHECK(plain.types().empty());
}

TEST_CASE("project.json names the gameplay sources") {
    const auto paths = [](std::string_view gameplay) {
        std::vector<std::string> out;
        const std::string text =
            R"({"format": 1, "name": "G", "main_scene": "a.scene.json", "gameplay": )" +
            std::string(gameplay) + "}";
        for (const auto& error : runtime::validate_project(parse(text))) {
            out.push_back(error.path);
        }
        return out;
    };
    CHECK(paths(R"({"sources": ["src", "shared/util.cpp"]})").empty());
    CHECK_FALSE(paths(R"({"sources": []})").empty());
    CHECK_FALSE(paths(R"({})").empty());
    CHECK(paths(R"({"sources": ["../outside"]})") ==
          std::vector<std::string>{"/gameplay/sources/0"});
    auto project = runtime::parse_project(parse(
        R"({"format": 1, "name": "G", "main_scene": "a.scene.json", "gameplay": {"sources": ["src"]}})"));
    REQUIRE(project.ok());
    CHECK(project.value().gameplay_sources == Names{"src"});
}

TEST_CASE("compiler errors are read from a build log") {
    const std::string log = "[1/2] Building CXX object CMakeFiles/game.dir/src/game.cpp.o\n"
                            "/work/pong/src/game.cpp:12:5: error: use of undeclared identifier "
                            "'speed'\n"
                            "    speed += 1;\n"
                            "    ^\n"
                            "/work/pong/src/game.cpp:12:5: error: use of undeclared identifier "
                            "'speed'\n"
                            "/work/pong/src/ball.hpp:3:10: fatal error: 'missing.hpp' file not "
                            "found\n"
                            "/usr/include/c++/v1/vector:40: error: something in a system header\n"
                            "C:\\work\\pong\\src\\paddle.cpp(7,15): error C2065: 'height': "
                            "undeclared identifier\r\n"
                            "src/game.cpp:20:1: warning: unused variable\n"
                            "ld: error: undefined symbol: slash::thing\n";
    const std::vector<slash::exporter::CompileError> errors =
        slash::exporter::read_compile_errors(log, "/work/pong");
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].file == "src/game.cpp");
    CHECK(errors[0].line == 12);
    CHECK(errors[0].column == 5);
    CHECK(errors[0].message == "use of undeclared identifier 'speed'");
    CHECK(errors[1].file == "src/ball.hpp");
    CHECK(errors[1].message == "'missing.hpp' file not found");
    CHECK(errors[2].file == "/usr/include/c++/v1/vector");
    CHECK(errors[2].line == 40);
    CHECK(errors[2].column == 0);
    CHECK(errors[3].file == "C:/work/pong/src/paddle.cpp");
    CHECK(errors[3].line == 7);
    CHECK(errors[3].column == 15);
    CHECK(errors[3].message == "'height': undeclared identifier");
    CHECK(slash::exporter::read_compile_errors(log, "/work/pong", 1).size() == 1);
    CHECK(slash::exporter::read_compile_errors(log, "C:/work/pong")[3].file == "src/paddle.cpp");

    slash::exporter::GameplayBuild failed;
    failed.step = "build";
    failed.message = "the game's code has 1 error";
    failed.errors = {errors[0]};
    failed.log = ".slash/gameplay/build.log";
    CHECK(failed.to_json() == parse(R"({"errors": [{"column": 5, "file": "src/game.cpp", "line": 12,
                                         "message": "use of undeclared identifier 'speed'"}],
                                        "log": ".slash/gameplay/build.log", "step": "build"})"));
}
