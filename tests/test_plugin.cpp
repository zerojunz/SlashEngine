#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/exporter/plugins.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/plugin/plugins.hpp"
#include "slash/plugin/slash_plugin.h"
#include "slash/runtime/plugins.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace plugin = slash::plugin;
namespace input = slash::input;
namespace fs = std::filesystem;

// The plugins example's plugin, linked into the tests as an iOS or web export links a plugin.
extern "C" int orbit_plugin_entry(SlashGetProc get_proc, SlashPlugin* plugin,
                                  SlashPluginInit* init);

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// A scene file of entities written as {"id": {"Component": {...}}}.
json::Value scene(std::string_view entities) {
    json::Value out = json::Object{};
    out["format"] = 1;
    out["entities"] = json::Object{};
    const json::Value written = parse(entities);
    for (const auto& [id, components] : written.as_object()) {
        out["entities"][id] = json::Object{};
        if (!components.as_object().empty()) {
            out["entities"][id]["components"] = components;
        }
    }
    return out;
}

// Takes a world's plugin systems out when it goes, before the plugins do.
struct Attached {
    plugin::Plugins& plugins;
    world::World& world;

    Attached(plugin::Plugins& p, world::World& w) : plugins(p), world(w) { plugins.attach(world); }
    Attached(const Attached&) = delete;
    Attached& operator=(const Attached&) = delete;
    Attached(Attached&&) = delete;
    Attached& operator=(Attached&&) = delete;
    ~Attached() { plugins.detach(world); }
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

template <class F>
F proc(const char* name) {
    const SlashProc found = plugin::get_proc(name);
    REQUIRE_MESSAGE(found != nullptr, name);
    return reinterpret_cast<F>(found);
}

SlashWorld* handle(world::World& w) {
    return reinterpret_cast<SlashWorld*>(&w);
}

// The engine's components and a plugin's, with a world that runs the plugin's systems.
struct Game {
    world::ComponentRegistry components;
    plugin::Plugins plugins{components};

    Game() { world::register_builtin_components(components); }
};

void load_orbit(plugin::Plugins& plugins) {
#if defined(SLASH_TEST_PLUGIN)
    const auto loaded = plugins.load(SLASH_TEST_PLUGIN, "orbit_plugin_entry", "orbit");
#else
    const auto loaded = plugins.load_linked(&orbit_plugin_entry, "orbit");
#endif
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
}

// A plugin that registers what a test hands it.
SlashComponentInfo g_component{};
SlashSystemInfo g_system{};
int g_refuse = 0;

int test_entry(SlashGetProc get_proc, SlashPlugin* plugin, SlashPluginInit* /*init*/) {
    if (g_refuse != 0) {
        return 0;
    }
    const auto add = reinterpret_cast<SlashRegisterComponent>(get_proc("slash_register_component"));
    const auto run = reinterpret_cast<SlashRegisterSystem>(get_proc("slash_register_system"));
    if (g_component.name != nullptr) {
        (void)add(plugin, &g_component);
    }
    if (g_system.name != nullptr) {
        (void)run(plugin, &g_system);
    }
    return 1;
}

void no_op(void* /*userdata*/, SlashWorld* /*world*/, double /*seconds*/) {}

struct Probe {
    float value = 0.0F;
    std::int32_t count = 0;
};

const std::array<SlashField, 2> kProbeFields{{
    {"value", SLASH_FIELD_FLOAT, offsetof(Probe, value), 0, nullptr, 0, 0, 0, 0},
    {"count", SLASH_FIELD_INT32, offsetof(Probe, count), 0, nullptr, 0, 0, 0, 0},
}};

std::string load_error(plugin::Plugins& plugins, std::string name) {
    const auto loaded = plugins.load_linked(&test_entry, std::move(name));
    return loaded.ok() ? std::string() : loaded.error().message;
}

} // namespace

TEST_CASE("a plugin's components load, save and validate as an engine component's do") {
    Game game;
    load_orbit(game.plugins);
    const world::ComponentType* orbit = game.components.find("Orbit");
    REQUIRE(orbit != nullptr);
    CHECK(game.plugins.status() ==
          parse(R"([{"name": "orbit", "components": ["Orbit"], "systems": ["orbit"]}])"));

    world::World w(game.components);
    const auto loaded = world::load_scene(w, scene(R"({
        "sun": {},
        "moon": {"Orbit": {"center": [100, 50], "radius": 10, "label": "moon",
                                 "notify": "sun"}}})"));
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
    const json::Value fields = orbit->get(w, w.find("moon"));
    CHECK(fields == parse(R"({"center": [100, 50], "radius": 10, "speed": 1, "angle": 0,
                              "laps": 0, "active": true, "label": "moon", "notify": "sun"})"));
    // Only what differs from the defaults is saved.
    CHECK(world::save_scene(w)["entities"]["moon"]["components"]["Orbit"] ==
          parse(R"({"center": [100, 50], "radius": 10, "label": "moon", "notify": "sun"})"));

    // The scene schema checks its fields: ranges, string capacity, entity references.
    const auto check = [&](std::string_view text) {
        world::World other(game.components);
        const auto result = world::load_scene(other, scene(text));
        return result.ok() ? std::string() : result.error().message;
    };
    CHECK(contains(check(R"({"a": {"Orbit": {"radius": -1}}})"),
                   "/entities/a/components/Orbit/radius"));
    CHECK(
        contains(check(R"({"a": {"Orbit": {"label": "a label longer than twenty-three bytes"}}})"),
                 "/entities/a/components/Orbit/label"));
    CHECK(contains(check(R"({"a": {"Orbit": {"speeed": 2}}})"), "speed"));
    CHECK(contains(check(R"({"a": {"Orbit": {"center": [1]}}})"),
                   "/entities/a/components/Orbit/center"));
    CHECK(contains(check(R"({"a": {"Orbit": {"notify": "nobody"}}})"), "nobody"));

    // Its schema names its fields with their docs.
    world::SchemaBuilder builder;
    const json::Value schema = orbit->schema(builder);
    CHECK(schema == world::SchemaBuilder::reference("Orbit"));
    const std::string definitions =
        json::write(json::Value(builder.definitions()), json::Style::Compact);
    CHECK(contains(definitions, "\"label\""));
    CHECK(contains(definitions, "How far from the center"));
}

TEST_CASE("a plugin's systems run in every world it is attached to") {
    Game game;
    load_orbit(game.plugins);
    world::World w(game.components);
    input::install(w);
    const Attached attached(game.plugins, w);
    REQUIRE(world::load_scene(w, scene(R"({
        "sun": {},
        "moon": {"Orbit": {"radius": 10, "speed": 3.14159265, "label": "moon",
                                 "notify": "sun"}}})"))
                .ok());
    const auto systems = w.system_names();
    CHECK(std::ranges::find(systems, std::string_view("orbit")) != systems.end());

    // Half a second at pi radians a second: a quarter turn, from (10, 0) to about (0, 10).
    for (int i = 0; i < 30; ++i) {
        w.step(1.0 / 60.0);
    }
    const json::Value position =
        w.components().find("Transform2D")->get(w, w.find("moon"))["position"];
    CHECK(position.as_array()[0].as_double() == doctest::Approx(0.0).epsilon(0.01));
    CHECK(position.as_array()[1].as_double() == doctest::Approx(10.0).epsilon(0.01));

    // A full lap emits an event about the entity it names.
    std::vector<world::Event> laps;
    for (int i = 0; i < 100 && laps.empty(); ++i) {
        w.step(1.0 / 60.0);
        for (const world::Event& event : w.events()) {
            if (event.kind == "lap") {
                laps.push_back(event);
            }
        }
    }
    REQUIRE(laps.size() == 1);
    CHECK(laps[0].id == "sun");
    CHECK(laps[0].data == parse(R"({"laps": 1, "label": "moon"})"));

    // Input reaches it: ui_accept turns it round.
    auto press = input::parse_event(parse(R"({"type": "action", "action": "ui_accept",
                                               "pressed": true})"),
                                    input::Use::Event);
    REQUIRE(press.ok());
    input::push(w, std::move(press).value());
    w.step(1.0 / 60.0);
    CHECK(w.components().find("Orbit")->get(w, w.find("moon"))["speed"].as_double() < 0.0);
}

TEST_CASE("the same plugin loads again for another registry, keeping its component's ID") {
    Game first;
    Game second;
    load_orbit(first.plugins);
    load_orbit(second.plugins);
    const auto find = proc<SlashComponentFind>("slash_component_find");
    CHECK(find(nullptr, "Orbit") != 0);
    CHECK(second.components.find("Orbit") != nullptr);
    // Twice in one registry is refused.
    const auto again = first.plugins.load_linked(&orbit_plugin_entry, "orbit");
    REQUIRE_FALSE(again.ok());
    CHECK(contains(again.error().message, "loaded already"));
}

TEST_CASE("a plugin that registers anything wrong adds nothing") {
    Game game;
    const std::size_t before = game.components.types().size();
    Probe defaults{};
    g_component = {.name = "TestProbe",
                   .doc = nullptr,
                   .size = sizeof(Probe),
                   .align = alignof(Probe),
                   .defaults = &defaults,
                   .fields = kProbeFields.data(),
                   .field_count = kProbeFields.size()};
    g_system = {
        .name = "test_probe", .doc = nullptr, .priority = 0, .run = &no_op, .userdata = nullptr};

    // A component whose fields overlap.
    std::array<SlashField, 2> overlapping = kProbeFields;
    overlapping[1].offset = 2;
    g_component.fields = overlapping.data();
    CHECK(contains(load_error(game.plugins, "overlap"), "count"));
    g_component.fields = kProbeFields.data();

    // An engine component's name, a bad field name, a struct too big.
    g_component.name = "Transform2D";
    CHECK(contains(load_error(game.plugins, "clash"), "Transform2D"));
    g_component.name = "TestProbe";
    std::array<SlashField, 2> renamed = kProbeFields;
    renamed[0].name = "Value";
    g_component.fields = renamed.data();
    CHECK(contains(load_error(game.plugins, "names"), "Value"));
    g_component.fields = kProbeFields.data();
    g_component.size = 8192;
    CHECK(contains(load_error(game.plugins, "size"), "4096"));
    g_component.size = sizeof(Probe);

    // A system with the name of a world's own, once a world is attached.
    world::World w(game.components);
    w.add_system("test_probe", [](world::World&, double) {});
    const Attached attached(game.plugins, w);
    CHECK(contains(load_error(game.plugins, "system"), "test_probe"));

    // An entry function that refuses.
    g_refuse = 1;
    CHECK(contains(load_error(game.plugins, "refused"), "refused"));
    g_refuse = 0;

    CHECK(game.components.types().size() == before);
    CHECK(game.plugins.size() == 0);

    // Then a good one loads, and its system runs.
    g_system.name = "test_probe_2";
    CHECK(load_error(game.plugins, "good").empty());
    CHECK(game.components.types().size() == before + 1);
    const auto systems = w.system_names();
    CHECK(std::ranges::find(systems, std::string_view("test_probe_2")) != systems.end());
    g_component = {};
    g_system = {};
}

TEST_CASE("plugins read and write any component through the C interface") {
    Game game;
    load_orbit(game.plugins);
    world::World w(game.components);
    SlashWorld* world = handle(w);
    const auto create = proc<SlashEntityCreate>("slash_entity_create");
    const auto find = proc<SlashEntityFind>("slash_entity_find");
    const auto id_of = proc<SlashEntityId>("slash_entity_id");
    const auto destroy = proc<SlashEntityDestroy>("slash_entity_destroy");
    const auto valid = proc<SlashEntityValid>("slash_entity_valid");
    const auto component = proc<SlashComponentFind>("slash_component_find")(world, "Orbit");
    const auto add = proc<SlashComponentAdd>("slash_component_add");
    const auto get = proc<SlashComponentGet>("slash_component_get");
    const auto remove = proc<SlashComponentRemove>("slash_component_remove");
    const auto get_json = proc<SlashComponentGetJson>("slash_component_get_json");
    const auto set_json = proc<SlashComponentSetJson>("slash_component_set_json");
    const auto each = proc<SlashWorldEach>("slash_world_each");

    const SlashEntity a = create(world, "a");
    const SlashEntity b = create(world, nullptr);
    CHECK(create(world, "a") == SLASH_NO_ENTITY);
    CHECK(find(world, "a") == a);
    std::array<char, 4> small{};
    CHECK(id_of(world, a, small.data(), small.size()) == 1);
    CHECK(std::string_view(small.data()) == "a");

    // Added at its defaults, written in place, read as JSON.
    void* orbit = add(world, a, component);
    REQUIRE(orbit != nullptr);
    CHECK(get(world, a, component) == orbit);
    CHECK(get(world, b, component) == nullptr);
    float radius = 0.0F;
    std::memcpy(&radius, static_cast<std::byte*>(orbit) + 8, sizeof radius);
    CHECK(radius == 32.0F);
    radius = 5.0F;
    std::memcpy(static_cast<std::byte*>(orbit) + 8, &radius, sizeof radius);
    std::array<char, 256> text{};
    const std::int64_t length = get_json(world, a, "Orbit", text.data(), text.size());
    REQUIRE(length > 0);
    CHECK(parse(text.data())["radius"].as_double() == 5.0);
    // A length past the buffer says how much it needs.
    CHECK(get_json(world, a, "Orbit", small.data(), small.size()) == length);
    CHECK(get_json(world, b, "Orbit", text.data(), text.size()) == -1);

    // Any component, the engine's too, by JSON; errors name the field.
    std::array<char, 128> error{};
    CHECK(set_json(world, b, "Transform2D", R"({"position": [3, 4]})", error.data(),
                   error.size()) == 1);
    CHECK(w.components()
              .find("Transform2D")
              ->get(w, w.find(std::string(w.id_of(static_cast<entt::entity>(b)))))["position"] ==
          parse("[3, 4]"));
    CHECK(set_json(world, b, "Orbit", R"({"radius": -2})", error.data(), error.size()) == 0);
    CHECK(contains(error.data(), "radius"));
    CHECK(set_json(world, b, "Orbit", R"({"label": "b", "notify": "a"})", error.data(),
                   error.size()) == 1);

    // Each entity with every component named, in the order the engine's own views walk them.
    std::vector<SlashEntity> seen;
    each(
        world, &component, 1,
        [](void* userdata, SlashEntity entity, void** components) {
            CHECK(components[0] != nullptr);
            static_cast<std::vector<SlashEntity>*>(userdata)->push_back(entity);
        },
        &seen);
    CHECK(seen == std::vector<SlashEntity>{b, a});

    // An entity reference follows a rename, and saving names it by ID.
    CHECK(world::save_scene(w)["entities"][std::string(w.id_of(static_cast<entt::entity>(b)))]
                              ["components"]["Orbit"]["notify"] == "a");
    CHECK(remove(world, a, component) == 1);
    CHECK(remove(world, a, component) == 0);
    destroy(world, a);
    CHECK(valid(world, a) == 0);
}

TEST_CASE("plugin files name an entry and a library per platform") {
    CHECK(plugin::validate_plugin_file(parse(R"({"entry": "e", "libraries": {"linux": "a.so"}})"))
              .empty());
    const auto errors = plugin::validate_plugin_file(
        parse(R"({"entry": "e", "abi": 2, "libraries": {"linux.mips": "a.so"}})"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/libraries/linux.mips");
    CHECK(errors[1].path == "/abi");
    CHECK_FALSE(plugin::validate_plugin_file(parse(R"({"entry": "1e", "libraries": {}})")).empty());
    CHECK(plugin::validate_plugin_file(parse(R"({"entry": "e"})")).at(0).message ==
          "expected \"libraries\", \"sources\" or both");
    CHECK(plugin::validate_plugin_file(parse(R"({"entry": "e", "sources": ["e.c"]})")).empty());

    const auto file = plugin::parse_plugin_file(
        parse(R"({"entry": "e", "libraries": {"linux": "bin/a.so", "linux.arm64": "bin/arm/a.so",
                                             "windows.x86_64": "bin/a.dll"}})"),
        "addons/a/a.plugin.json");
    REQUIRE(file.ok());
    CHECK(plugin::library_for(file.value(), "linux", "x86_64") == "addons/a/bin/a.so");
    CHECK(plugin::library_for(file.value(), "linux", "arm64") == "addons/a/bin/arm/a.so");
    CHECK(plugin::library_for(file.value(), "windows", "x86_64") == "addons/a/bin/a.dll");
    CHECK(plugin::library_for(file.value(), "macos", "arm64").empty());
}

#if defined(SLASH_TEST_PLUGIN)
TEST_CASE("a project's plugin files load for this platform") {
    const fs::path root = fs::absolute("plugin_project");
    fs::remove_all(root);
    fs::create_directories(root / "addons/orbit/bin");
    const fs::path library = fs::path(SLASH_TEST_PLUGIN);
    fs::copy_file(library, root / "addons/orbit/bin" / library.filename());
    const std::string key =
        std::string(slash::platform::os_name()) + "." + std::string(plugin::cpu_name());
    json::Value file = parse(R"({"entry": "orbit_plugin_entry", "libraries": {}})");
    file["libraries"][key] = "bin/" + library.filename().string();
    REQUIRE(slash::platform::write_file((root / "addons/orbit/orbit.plugin.json").string(),
                                        json::write(file))
                .ok());
    REQUIRE(slash::platform::write_file((root / "addons/other.plugin.json").string(),
                                        R"({"entry": "e", "libraries": {"web": "x.wasm"}})")
                .ok());

    Game game;
    const std::vector<std::string> files{"addons/orbit/orbit.plugin.json",
                                         "addons/other.plugin.json", "addons/missing.plugin.json"};
    const auto errors = game.plugins.load_project(root.string(), files);
    CHECK(errors.size() == 2);
    CHECK(contains(errors.at("addons/other.plugin.json"), "no library for"));
    CHECK(errors.contains("addons/missing.plugin.json"));
    CHECK(game.plugins.status().as_array().at(0)["name"].as_string() == "orbit");
    CHECK(game.components.find("Orbit") != nullptr);

    // A session over a project that lists the plugin runs its systems and describes it.
    REQUIRE(slash::platform::write_file(
                (root / "project.json").string(),
                R"({"format": 1, "name": "Orbits", "main_scene": "main.scene.json",
                    "plugins": ["addons/orbit/orbit.plugin.json"]})")
                .ok());
    auto project = slash::runtime::load_project(root.string());
    REQUIRE(project.ok());
    CHECK(project.value().plugins == std::vector<std::string>{"addons/orbit/orbit.plugin.json"});
    Game session_game;
    CHECK(slash::runtime::load_plugins(session_game.plugins, root.string()).empty());
    slash::command::Registry commands;
    slash::command::register_builtin_commands(commands);
    slash::command::SessionOptions options;
    options.project_root = root.string();
    options.plugins = &session_game.plugins;
    {
        slash::command::Session session(session_game.components, commands, options);
        const auto ok = [&](std::string_view name, std::string_view params) {
            slash::command::Reply reply = session.call(name, parse(params));
            REQUIRE_MESSAGE(reply.ok, reply.message);
            return std::move(reply.value);
        };
        CHECK(ok("engine.describe", "{}")["plugins"] ==
              parse(R"([{"name": "orbit", "components": ["Orbit"],
                         "systems": ["orbit"]}])"));
        (void)ok("entity.create", R"({"id": "moon", "components": {"Orbit": {"radius": 8}}})");
        (void)ok("step", R"({"ticks": 2})");
        const json::Value moon = ok("entity.get", R"({"id": "moon"})");
        CHECK(contains(json::write(moon), "Transform2D"));
        // Undo puts the plugin's component back as it was before the step.
        (void)ok("undo", "{}");
        CHECK(session.world().components().find("Orbit")->get(
                  session.world(), session.world().find("moon"))["angle"] == 0.0);
    }
    // project validate refuses a plugin entry that isn't a plugin file.
    CHECK_FALSE(slash::runtime::validate_project(
                    parse(
                        R"({"format": 1, "name": "Orbits", "main_scene": "main.scene.json",
                        "plugins": ["addons/orbit.json"]})"))
                    .empty());
    fs::remove_all(root);
}
#endif

TEST_CASE("games enter linked plugins and find libraries where exports install them") {
    const fs::path root = fs::absolute("plugin_linked");
    fs::remove_all(root);
    fs::create_directories(root / "addons/orbit");
    REQUIRE(
        slash::platform::write_file((root / "addons/orbit/orbit.plugin.json").string(),
                                    R"({"entry": "orbit_plugin_entry", "sources": ["orbit.c"]})")
            .ok());
    const std::vector<std::string> files{"addons/orbit/orbit.plugin.json"};

    // With only sources, the tools have nothing to load; a game that linked them enters them.
    Game tools;
    CHECK(
        contains(tools.plugins.load_project(root.string(), files).at(files[0]), "no library for"));
    Game game;
    plugin::LoadOptions options;
    options.linked.emplace("orbit_plugin_entry", &orbit_plugin_entry);
    CHECK(game.plugins.load_project(root.string(), files, options).empty());
    CHECK(game.components.find("Orbit") != nullptr);

#if defined(SLASH_TEST_PLUGIN)
    // A library that isn't where its file says is found by its name in the folders given, as a
    // macOS game's Frameworks.
    const fs::path library(SLASH_TEST_PLUGIN);
    fs::create_directories(root / "Frameworks");
    fs::copy_file(library, root / "Frameworks" / library.filename());
    json::Value file = parse(R"({"entry": "orbit_plugin_entry", "libraries": {}})");
    file["libraries"][std::string(slash::platform::os_name())] =
        "bin/" + library.filename().string();
    REQUIRE(slash::platform::write_file((root / "addons/orbit/orbit.plugin.json").string(),
                                        json::write(file))
                .ok());
    Game installed;
    CHECK(contains(installed.plugins.load_project(root.string(), files).at(files[0]),
                   library.filename().string()));
    Game bundled;
    CHECK(bundled.plugins
              .load_project(root.string(), files,
                            {.linked = {}, .library_folders = {(root / "Frameworks").string()}})
              .empty());
#endif
    fs::remove_all(root);
}

TEST_CASE("exports compile plugins in from their sources, or ship their libraries") {
    namespace exporter = slash::exporter;
    const fs::path root = fs::absolute("plugin_export");
    fs::remove_all(root);
    fs::create_directories(root / "addons/orbit/src/apple");
    fs::create_directories(root / "addons/vendor/lib");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((root / path).string(), text).ok());
    };
    write("addons/orbit/orbit.plugin.json",
          R"({"entry": "orbit_entry", "libraries": {"linux": "x.so"}, "sources": ["src"]})");
    write("addons/orbit/src/orbit.c", "");
    write("addons/orbit/src/apple/bridge.m", "");
    write("addons/orbit/src/notes.txt", "");
    write("addons/vendor/vendor.plugin.json", R"({"entry": "vendor_entry", "libraries": {
        "linux.x86_64": "lib/libvendor.so", "linux.arm64": "lib/libvendor_arm.so",
        "android.arm64": "lib/libvendor.so", "android.x86_64": "lib/vendor.so",
        "ios": "lib/libvendor.a", "web": "lib/vendor.so", "macos": "lib/libvendor.dylib"}})");
    write("addons/vendor/lib/libvendor.so", "");
    write("addons/vendor/lib/vendor.so", "");
    write("addons/vendor/lib/libvendor.a", "");
    slash::runtime::Project project;
    project.plugins = {"addons/orbit/orbit.plugin.json", "addons/vendor/vendor.plugin.json"};

    std::vector<exporter::PluginProblem> problems;
    auto linux = exporter::plan_plugins(root, project, "linux", "x86_64", problems);
    CHECK(problems.empty());
    REQUIRE(linux.size() == 2);
    // Sources are compiled in on every platform, Objective-C only on Apple's.
    CHECK(linux[0].name == "orbit");
    CHECK(linux[0].linked());
    REQUIRE(linux[0].sources.size() == 1);
    CHECK(linux[0].sources[0].filename() == "orbit.c");
    CHECK(exporter::plan_plugins(root, project, "macos", "universal", problems)[0].sources.size() ==
          2);
    // A library ships as it is, found by the build's CPU.
    CHECK_FALSE(linux[1].linked());
    CHECK(linux[1].library_path == "addons/vendor/lib/libvendor.so");
    CHECK(exporter::plugin_arch("android", "arm64-v8a") == "arm64");

    const std::string source = exporter::plugins_register_source(linux);
    CHECK(contains(source, "extern \"C\" int orbit_entry("));
    CHECK(contains(source, "plugins.emplace(\"orbit_entry\", &orbit_entry);"));
    CHECK_FALSE(contains(source, "vendor_entry"));
    const std::string cmake = exporter::plugins_cmake(linux, "/build/plugins.cpp");
    CHECK(contains(cmake, "slash_game_plugin([[orbit]]\n    SOURCES [["));
    CHECK(contains(cmake, "slash_game_plugin([[vendor]]\n    SHARED [["));
    CHECK(contains(cmake, "slash_game_plugins_register([[/build/plugins.cpp]])"));

    // iOS links a static library and enters it; the web needs one too; Android needs lib*.so;
    // a platform with no library and no sources is refused.
    problems.clear();
    auto ios = exporter::plan_plugins(root, project, "ios", "simulator", problems);
    CHECK(problems.empty());
    REQUIRE(ios.size() == 2);
    CHECK(ios[1].linked());
    CHECK(contains(exporter::plugins_cmake(ios, ""), "STATIC [["));
    CHECK(contains(exporter::plugins_register_source(ios), "vendor_entry"));
    const auto refused = [&](std::string_view platform, std::string_view arch) {
        problems.clear();
        (void)exporter::plan_plugins(root, project, platform, arch, problems);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].file == "addons/vendor/vendor.plugin.json");
        CHECK(problems[0].path == "/libraries");
        return problems[0].message;
    };
    CHECK(contains(refused("web", "wasm32"), "static library"));
    CHECK(contains(refused("android", "x86_64"), "lib<name>.so"));
    CHECK(contains(refused("macos", "universal"), "no file \"addons/vendor/lib/libvendor.dylib\""));
    CHECK(contains(refused("windows", "x86_64"), "no library for windows.x86_64"));
    problems.clear();
    CHECK(exporter::plan_plugins(root, project, "android", "arm64-v8a", problems)[1].library_path ==
          "addons/vendor/lib/libvendor.so");
    CHECK(problems.empty());
    fs::remove_all(root);
}
