#include "slash/accessibility/accessibility.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/ui/accessibility.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#if __has_include(<accesskit.h>)
#include "slash/accessibility/accesskit.hpp"
#define SLASH_TEST_ACCESSKIT 1
#endif

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The screen reader backend, as Godot 4.5's AccessibilityDriverAccessKit
// (drivers/accesskit/accessibility_driver_accesskit.cpp): the controls' accessibility tree as
// AccessKit's nodes, the updates that send what changed, and screen readers' requests run on
// the controls.

namespace accessibility = slash::accessibility;
namespace command = slash::command;
namespace json = slash::json;
namespace ui = slash::ui;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view source) {
    auto parsed = json::parse(source);
    REQUIRE_MESSAGE(parsed.ok(), std::string(source));
    return std::move(parsed).value();
}

struct Session {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Session() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ui::set_viewport(session->world(), {400.0F, 300.0F});
    }

    world::World& world() { return session->world(); }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    std::uint64_t id(std::string_view control) {
        const entt::entity entity = world().find(control);
        REQUIRE(world().valid(entity));
        return accessibility::node_id(entity);
    }
};

// A pause menu: a title, a button, a check box and a slider the title labels, in a box.
void make_menu(Session& s) {
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Label": {"text": "Paused"}}}},
        {"command": "entity.create", "params": {"id": "resume", "components": {
            "Button": {"text": "Resume"},
            "Control": {"tooltip_text": "Back to it",
                        "accessibility_description": "Back to the game"}}}},
        {"command": "entity.create", "params": {"id": "music", "components": {
            "CheckBox": {"text": "Music", "button_pressed": true}}}},
        {"command": "entity.create", "params": {"id": "volume", "components": {
            "Slider": {"value": 50, "step": 5},
            "Control": {"accessibility_name": "Volume", "custom_minimum_size": [100, 0],
                        "accessibility_labeled_by_nodes": ["title"]}}}},
        {"command": "entity.create", "params": {"id": "menu", "components": {
            "BoxContainer": {"vertical": true},
            "Control": {"children": ["title", "resume", "music", "volume"],
                        "accessibility_live": "polite"}}}}
    ]})");
}

const accessibility::Node& node_of(const accessibility::Snapshot& snapshot, std::uint64_t id) {
    const auto found =
        std::ranges::find_if(snapshot.nodes, [id](const auto& node) { return node.first == id; });
    REQUIRE(found != snapshot.nodes.end());
    return found->second;
}

} // namespace

TEST_CASE("Accessibility node IDs name controls, menus and items, never the window") {
    const auto entity = static_cast<entt::entity>(41);
    const std::uint64_t control = accessibility::node_id(entity);
    const std::uint64_t menu = accessibility::node_id(entity, true);
    const std::uint64_t item = accessibility::node_id(entity, false, 3);
    CHECK(control != accessibility::kWindowNode);
    CHECK(control != menu);
    CHECK(menu != item);
    CHECK_FALSE(accessibility::target_of(accessibility::kWindowNode).has_value());
    const auto back = accessibility::target_of(item);
    REQUIRE(back.has_value());
    CHECK(back->entity == entity);
    CHECK_FALSE(back->menu);
    CHECK(back->item == 3);
    CHECK(accessibility::target_of(menu)->menu);
    CHECK(accessibility::target_of(control)->item == -1);
}

TEST_CASE("A snapshot is the window and the controls' tree as AccessKit gets it") {
    Session s;
    make_menu(s);
    s.ok("ui.accessibility_action", R"({"id": "music", "action": "focus"})");
    // On a display of twice the density, bounds are in its pixels.
    const accessibility::Snapshot snapshot = accessibility::snapshot(s.world(), "Pause", 2.0);
    REQUIRE(snapshot.nodes.size() == 6);
    const auto& [window_id, window] = snapshot.nodes[0];
    CHECK(window_id == accessibility::kWindowNode);
    CHECK(window.role == "window");
    CHECK(window.name == "Pause");
    CHECK(window.children == std::vector<std::uint64_t>{s.id("menu")});
    CHECK(snapshot.focus == s.id("music"));

    const accessibility::Node& menu = node_of(snapshot, s.id("menu"));
    CHECK(menu.live == "polite");
    CHECK(menu.children ==
          std::vector<std::uint64_t>{s.id("title"), s.id("resume"), s.id("music"), s.id("volume")});
    const accessibility::Node& title = node_of(snapshot, s.id("title"));
    CHECK(title.role == "static_text");
    CHECK(title.value == "Paused");
    REQUIRE(title.bounds.has_value());
    CHECK(*title.bounds == std::array<double, 4>{0.0, 0.0, 200.0, 44.0});
    // Every control takes the common actions, then its own.
    const accessibility::Node& resume = node_of(snapshot, s.id("resume"));
    CHECK(resume.name == "Resume");
    CHECK(resume.description == "Back to the game");
    CHECK(resume.tooltip == "Back to it");
    CHECK(resume.actions == std::vector<std::string>{"focus", "blur", "show_tooltip",
                                                     "hide_tooltip", "scroll_into_view", "click"});
    CHECK(node_of(snapshot, s.id("music")).checked == true);
    const accessibility::Node& volume = node_of(snapshot, s.id("volume"));
    CHECK(volume.range == std::array<double, 4>{50.0, 0.0, 100.0, 5.0});
    CHECK(volume.labeled_by == std::vector<std::uint64_t>{s.id("title")});
}

TEST_CASE("An update sends only the nodes that changed") {
    Session s;
    make_menu(s);
    const accessibility::Snapshot before = accessibility::snapshot(s.world(), "Pause");
    CHECK(accessibility::changed(before, before).empty());
    // The first update sends every node.
    CHECK(accessibility::changed({}, before).size() == before.nodes.size());

    s.ok("ui.accessibility_action", R"({"id": "volume", "action": "increment"})");
    const accessibility::Snapshot stepped = accessibility::snapshot(s.world(), "Pause");
    const std::vector<std::size_t> one = accessibility::changed(before, stepped);
    REQUIRE(one.size() == 1);
    CHECK(stepped.nodes[one[0]].first == s.id("volume"));

    // A new control is sent with its parent, whose children changed.
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "quit", "components": {
            "Button": {"text": "Quit"}}}},
        {"command": "component.set", "params": {"id": "menu", "components": {
            "Control": {"children": ["title", "resume", "music", "volume", "quit"]}}}}
    ]})");
    const accessibility::Snapshot grown = accessibility::snapshot(s.world(), "Pause");
    std::vector<std::uint64_t> sent;
    for (const std::size_t index : accessibility::changed(stepped, grown)) {
        sent.push_back(grown.nodes[index].first);
    }
    CHECK(sent == std::vector<std::uint64_t>{s.id("menu"), s.id("quit")});
}

TEST_CASE("Screen readers' requests run on the controls they name") {
    Session s;
    make_menu(s);
    REQUIRE(accessibility::perform(s.world(),
                                   {.node = s.id("volume"), .action = "increment", .data = {}}));
    REQUIRE(accessibility::perform(
        s.world(), {.node = s.id("volume"), .action = "set_value", .data = json::Value(70.0)}));
    const accessibility::Snapshot snapshot = accessibility::snapshot(s.world(), "Pause");
    CHECK(node_of(snapshot, s.id("volume")).range == std::array<double, 4>{70.0, 0.0, 100.0, 5.0});
    // The window takes no actions, and a control only those it has.
    CHECK_FALSE(accessibility::perform(
        s.world(), {.node = accessibility::kWindowNode, .action = "click", .data = {}}));
    CHECK_FALSE(accessibility::perform(s.world(),
                                       {.node = s.id("title"), .action = "increment", .data = {}}));
}

#ifdef SLASH_TEST_ACCESSKIT

namespace {

std::string take(char* text) {
    std::string out = text != nullptr ? text : "";
    accesskit_string_free(text);
    return out;
}

std::vector<std::uint64_t> ids(accesskit_node_ids list) {
    return {list.values, list.values + list.length};
}

} // namespace

TEST_CASE("Nodes become AccessKit's, as Godot's driver fills them") {
    REQUIRE(accessibility::available());
    Session s;
    make_menu(s);
    const accessibility::Snapshot snapshot = accessibility::snapshot(s.world(), "Pause");

    accesskit_node* window = accessibility::make_node(snapshot.nodes[0].second);
    CHECK(accesskit_node_role(window) == ACCESSKIT_ROLE_WINDOW);
    CHECK(take(accesskit_node_label(window)) == "Pause");
    CHECK(ids(accesskit_node_children(window)) == std::vector<std::uint64_t>{s.id("menu")});
    accesskit_node_free(window);

    accesskit_node* menu = accessibility::make_node(node_of(snapshot, s.id("menu")));
    CHECK(accesskit_node_role(menu) == ACCESSKIT_ROLE_GENERIC_CONTAINER);
    CHECK(accesskit_node_live(menu).value == ACCESSKIT_LIVE_POLITE);
    CHECK(accesskit_node_is_touch_transparent(menu));
    CHECK(ids(accesskit_node_children(menu)).size() == 4);
    accesskit_node_free(menu);

    accesskit_node* title = accessibility::make_node(node_of(snapshot, s.id("title")));
    // Godot's ROLE_STATIC_TEXT is AccessKit's label, with the text as its value.
    CHECK(accesskit_node_role(title) == ACCESSKIT_ROLE_LABEL);
    CHECK(take(accesskit_node_value(title)) == "Paused");
    const accesskit_opt_rect bounds = accesskit_node_bounds(title);
    REQUIRE(bounds.has_value);
    CHECK(bounds.value.x1 == 100.0);
    CHECK(bounds.value.y1 == 22.0);
    accesskit_node_free(title);

    accesskit_node* resume = accessibility::make_node(node_of(snapshot, s.id("resume")));
    CHECK(accesskit_node_role(resume) == ACCESSKIT_ROLE_BUTTON);
    CHECK(take(accesskit_node_label(resume)) == "Resume");
    CHECK(take(accesskit_node_description(resume)) == "Back to the game");
    CHECK(take(accesskit_node_tooltip(resume)) == "Back to it");
    CHECK(accesskit_node_supports_action(resume, ACCESSKIT_ACTION_CLICK));
    CHECK(accesskit_node_supports_action(resume, ACCESSKIT_ACTION_FOCUS));
    CHECK_FALSE(accesskit_node_supports_action(resume, ACCESSKIT_ACTION_INCREMENT));
    accesskit_node_free(resume);

    accesskit_node* music = accessibility::make_node(node_of(snapshot, s.id("music")));
    CHECK(accesskit_node_role(music) == ACCESSKIT_ROLE_CHECK_BOX);
    CHECK(accesskit_node_toggled(music).value == ACCESSKIT_TOGGLED_TRUE);
    accesskit_node_free(music);

    accesskit_node* volume = accessibility::make_node(node_of(snapshot, s.id("volume")));
    CHECK(accesskit_node_role(volume) == ACCESSKIT_ROLE_SLIDER);
    CHECK(accesskit_node_numeric_value(volume).value == 50.0);
    CHECK(accesskit_node_max_numeric_value(volume).value == 100.0);
    CHECK(accesskit_node_numeric_value_step(volume).value == 5.0);
    CHECK(accesskit_node_supports_action(volume, ACCESSKIT_ACTION_SET_VALUE));
    CHECK(ids(accesskit_node_labelled_by(volume)) == std::vector<std::uint64_t>{s.id("title")});
    accesskit_node_free(volume);

    // A first update names the window as the root, with the focus.
    accesskit_tree_update* update = accessibility::make_update(snapshot, nullptr, true);
    const std::string text = take(accesskit_tree_update_debug(update));
    CHECK(text.find("SlashEngine") != std::string::npos);
    CHECK(text.find("Resume") != std::string::npos);
    accesskit_tree_update_free(update);
}

TEST_CASE("AccessKit's action requests become ours") {
    accesskit_action_request request{};
    request.action = ACCESSKIT_ACTION_INCREMENT;
    request.target_node = 77;
    auto ours = accessibility::request_of(request);
    REQUIRE(ours.has_value());
    CHECK(ours->node == 77);
    CHECK(ours->action == "increment");
    CHECK(ours->data.is_null());

    request.action = ACCESSKIT_ACTION_SET_VALUE;
    request.data.has_value = true;
    request.data.value.tag = ACCESSKIT_ACTION_DATA_NUMERIC_VALUE;
    request.data.value.numeric_value = 0.25;
    CHECK(accessibility::request_of(request)->data.as_double() == 0.25);

    // Scrolling by an item, as a ScrollContainer's "item" data.
    request.action = ACCESSKIT_ACTION_SCROLL_DOWN;
    request.data.value.tag = ACCESSKIT_ACTION_DATA_SCROLL_UNIT;
    request.data.value.scroll_unit = ACCESSKIT_SCROLL_UNIT_ITEM;
    CHECK(accessibility::request_of(request)->data.as_string() == "item");

    // Actions the controls don't take are dropped.
    request.action = ACCESSKIT_ACTION_SHOW_CONTEXT_MENU;
    request.data.has_value = false;
    CHECK_FALSE(accessibility::request_of(request).has_value());
}

#endif

#if defined(SLASH_TEST_ACCESSKIT) && defined(__linux__)

namespace {

// A command's output, as gdbus prints it.
std::string run(const std::string& command_line) {
    std::string out;
    FILE* pipe = popen((command_line + " 2>&1").c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
    pclose(pipe);
    return out;
}

// The quoted strings in gdbus's output, in order.
std::vector<std::string> quoted(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = text.find('\'', at)) != std::string::npos) {
        const std::size_t end = text.find('\'', at + 1);
        if (end == std::string::npos) {
            break;
        }
        out.push_back(text.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return out;
}

struct Accessible {
    std::string bus;
    std::string path;
    std::string name;
};

} // namespace

// Orca's view of a game: under a D-Bus session with AT-SPI's bus (ctest's atspi_screen_reader
// runs it with dbus-run-session and SLASH_ATSPI_TEST set), the AT-SPI registry lists the game,
// its window and its controls by name, and an action asked through AT-SPI presses a button.
TEST_CASE("Screen readers read and press a game's controls through AT-SPI") {
    if (std::getenv("SLASH_ATSPI_TEST") == nullptr) {
        MESSAGE("skipped: run by the atspi_screen_reader test, under a D-Bus session");
        return;
    }
    // AT-SPI's bus starts on demand; screen readers turn it on as Orca does.
    const std::string bus = "gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus ";
    run(bus + "--method org.freedesktop.DBus.Properties.Set org.a11y.Status IsEnabled '<true>'");
    run(bus + "--method org.freedesktop.DBus.Properties.Set org.a11y.Status "
              "ScreenReaderEnabled '<true>'");
    const std::vector<std::string> address = quoted(run(bus + "--method org.a11y.Bus.GetAddress"));
    REQUIRE(address.size() == 1);

    Session s;
    make_menu(s);
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window* window = SDL_CreateWindow("Pause test", 400, 300, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    {
        auto backend = accessibility::Backend::attach(window, "Pause test");
        REQUIRE(backend != nullptr);
        SDL_ShowWindow(window);
        const auto pump = [&] {
            SDL_PumpEvents();
            backend->update(s.world());
        };
        for (int i = 0; i < 200 && !backend->active(); ++i) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        REQUIRE(backend->active());
        // AT-SPI is on, so the controls serve a screen reader.
        pump();
        CHECK(ui::accessibility_enabled(s.world()));

        const std::string atspi = "gdbus call --address '" + address[0] + "' ";
        const auto children = [&](const Accessible& of) {
            const std::vector<std::string> found =
                quoted(run(atspi + "--dest " + of.bus + " --object-path " + of.path +
                           " --method org.a11y.atspi.Accessible.GetChildren"));
            std::vector<Accessible> out;
            for (std::size_t i = 0; i + 1 < found.size(); i += 2) {
                Accessible child{.bus = found[i], .path = found[i + 1], .name = ""};
                const std::vector<std::string> name = quoted(
                    run(atspi + "--dest " + child.bus + " --object-path " + child.path +
                        " --method org.freedesktop.DBus.Properties.Get org.a11y.atspi.Accessible "
                        "Name"));
                child.name = name.empty() ? "" : name[0];
                out.push_back(std::move(child));
            }
            return out;
        };
        // The registry's applications, then each one's tree, by name.
        std::vector<Accessible> seen;
        std::vector<Accessible> queue = children({.bus = "org.a11y.atspi.Registry",
                                                  .path = "/org/a11y/atspi/accessible/root",
                                                  .name = ""});
        while (!queue.empty() && seen.size() < 64) {
            Accessible next = queue.back();
            queue.pop_back();
            for (Accessible& child : children(next)) {
                queue.push_back(std::move(child));
            }
            seen.push_back(std::move(next));
        }
        const auto named = [&](std::string_view name) {
            return std::ranges::find_if(seen, [&](const Accessible& a) { return a.name == name; });
        };
        CHECK(named("Pause test") != seen.end());
        CHECK(named("Music") != seen.end());
        CHECK(named("Volume") != seen.end());
        const auto resume = named("Resume");
        REQUIRE(resume != seen.end());

        // Pressed as Orca presses it: AT-SPI's Action interface, the button's first action.
        const std::string subscription =
            s.ok("events.subscribe", R"({"kinds": ["pressed"]})")["subscription"].as_string();
        run(atspi + "--dest " + resume->bus + " --object-path " + resume->path +
            " --method org.a11y.atspi.Action.DoAction 0");
        for (int i = 0; i < 40; ++i) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        s.ok("step");
        const json::Value events =
            s.ok("events.read", R"({"subscription": ")" + subscription + R"("})");
        CHECK(json::write(events, json::Style::Compact).find("resume") != std::string::npos);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
}

#endif
