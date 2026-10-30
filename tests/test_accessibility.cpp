#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/ui/accessibility.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The accessibility tree, as Godot 4.5's controls fill DisplayServer's in
// NOTIFICATION_ACCESSIBILITY_UPDATE (scene/main/node.cpp, canvas_item.cpp, scene/gui/control.cpp,
// base_button.cpp, button.cpp, check_box.cpp, label.cpp, line_edit.cpp, range.cpp, slider.cpp,
// scroll_container.cpp, option_button.cpp and popup_menu.cpp), and the actions they take.

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

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
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

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    std::string fails(std::string_view name, std::string_view params) {
        const command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        return reply.message;
    }

    // The tree's element for a control, found anywhere in it.
    json::Value element(std::string_view id) {
        const json::Value tree = ok("ui.accessibility", R"({"ids": [")" + std::string(id) + "\"]}");
        REQUIRE(tree.find("tree")->as_array().size() == 1);
        return tree.find("tree")->as_array()[0];
    }
};

// A pause menu: a title, a button, a check box, a slider, a text field and an icon button with
// nothing to call it, in a box.
void make_menu(Session& s) {
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Label": {"text": "Paused"}}}},
        {"command": "entity.create", "params": {"id": "resume", "components": {
            "Button": {"text": "Resume"},
            "Control": {"accessibility_description": "Back to the game"}}}},
        {"command": "entity.create", "params": {"id": "music", "components": {
            "CheckBox": {"text": "Music", "button_pressed": true}}}},
        {"command": "entity.create", "params": {"id": "volume", "components": {
            "Slider": {"value": 50, "step": 5},
            "Control": {"accessibility_name": "Volume", "custom_minimum_size": [100, 0],
                        "accessibility_labeled_by_nodes": ["title"]}}}},
        {"command": "entity.create", "params": {"id": "player", "components": {
            "LineEdit": {"placeholder_text": "Your name"}}}},
        {"command": "entity.create", "params": {"id": "close", "components": {
            "TextureButton": {}, "Control": {"custom_minimum_size": [16, 16]}}}},
        {"command": "entity.create", "params": {"id": "secret", "components": {
            "Button": {"text": "Cheat"}, "Control": {"visible": false}}}},
        {"command": "entity.create", "params": {"id": "menu", "components": {
            "BoxContainer": {"vertical": true},
            "Control": {"children": ["title", "resume", "music", "volume", "player", "close",
                                     "secret"],
                        "accessibility_live": "polite"}}}}
    ]})");
}

} // namespace

TEST_CASE("ui.accessibility gives each control's role, name, value, state and actions") {
    Session s;
    make_menu(s);
    const json::Value result = s.ok("ui.accessibility");
    CHECK_FALSE(result.find("enabled")->as_bool());
    const json::Array& roots = result.find("tree")->as_array();
    REQUIRE(roots.size() == 1);
    const json::Value& menu = roots[0];
    CHECK(menu.find("id")->as_string() == "menu");
    CHECK(menu.find("role")->as_string() == "container");
    CHECK(menu.find("live")->as_string() == "polite");
    CHECK(compact(*menu.find("flags")) == R"(["touch_passthrough"])");
    // The hidden button is left out; the rest come in the box's order.
    const json::Array& children = menu.find("children")->as_array();
    REQUIRE(children.size() == 6);
    std::string order;
    for (const json::Value& child : children) {
        order += child.find("id")->as_string() + " ";
    }
    CHECK(order == "title resume music volume player close ");

    // Label: static text with its text as the value.
    CHECK(compact(children[0]) ==
          compact(parse(R"({"id": "title", "role": "static_text", "value": "Paused",
                            "rect": [0, 0, 100, 22]})")));
    // A Button is named by its text and says what it does.
    const json::Value& resume = children[1];
    CHECK(resume.find("role")->as_string() == "button");
    CHECK(resume.find("name")->as_string() == "Resume");
    CHECK(resume.find("description")->as_string() == "Back to the game");
    CHECK(compact(*resume.find("actions")) == R"(["click"])");
    CHECK(resume.find("checked") == nullptr);
    // A CheckBox is toggled on.
    const json::Value& music = children[2];
    CHECK(music.find("role")->as_string() == "check_box");
    CHECK(music.find("name")->as_string() == "Music");
    CHECK(music.find("checked")->as_bool());
    // A Slider is a Range: [value, min, max, step], and who labels it.
    const json::Value& volume = children[3];
    CHECK(volume.find("role")->as_string() == "slider");
    CHECK(volume.find("name")->as_string() == "Volume");
    CHECK(compact(*volume.find("range")) == "[50,0,100,5]");
    CHECK(compact(*volume.find("actions")) == R"(["decrement","increment","set_value"])");
    CHECK(compact(*volume.find("labeled_by")) == R"(["title"])");
    // An empty LineEdit is named by its placeholder, which it shows.
    const json::Value& player = children[4];
    CHECK(player.find("role")->as_string() == "text_field");
    CHECK(player.find("name")->as_string() == "Your name");
    CHECK(player.find("placeholder")->as_string() == "Your name");
    CHECK(player.find("value") == nullptr);
    CHECK(compact(*player.find("selection")) == "[0,0]");
    // A TextureButton has nothing to call it, which is a problem.
    CHECK(children[5].find("name") == nullptr);
    CHECK(compact(*result.find("problems")) ==
          compact(parse(R"([{"id": "close", "path": "/components/Control/accessibility_name",
                             "message": "a screen reader has no name for this button: give it text, accessibility_name or tooltip_text"}])")));

    // Hidden controls come when asked for, flagged.
    const json::Value all = s.ok("ui.accessibility", R"({"hidden": true})");
    const json::Value& secret = all.find("tree")->as_array()[0].find("children")->as_array()[6];
    CHECK(secret.find("id")->as_string() == "secret");
    CHECK(compact(*secret.find("flags")) == R"(["hidden"])");

    // A name and text both show; a tooltip stands in for neither.
    s.ok("component.set", R"({"id": "resume", "components": {
        "Control": {"accessibility_name": "Play"}}})");
    CHECK(s.element("resume").find("name")->as_string() == "Play: Resume");
    s.ok("component.set", R"({"id": "close", "components": {
        "Control": {"tooltip_text": "Close the menu"}}})");
    CHECK(s.element("close").find("tooltip")->as_string() == "Close the menu");
    // A TextureButton isn't a Button, so the tooltip doesn't name it.
    CHECK(s.element("close").find("name") == nullptr);
}

TEST_CASE("ui.accessibility_action clicks, steps, sets and focuses as a screen reader asks") {
    Session s;
    make_menu(s);
    // click presses at the next tick, as ui.press.
    s.ok("ui.accessibility_action", R"({"id": "resume", "action": "click"})");
    s.ok("events.subscribe", R"({"kinds": ["pressed"]})");
    s.ok("step");
    // increment and decrement step the Range; set_value keeps a number in range.
    s.ok("ui.accessibility_action", R"({"id": "volume", "action": "increment"})");
    CHECK(compact(*s.element("volume").find("range")) == "[55,0,100,5]");
    s.ok("ui.accessibility_action", R"({"id": "volume", "action": "set_value", "value": 140})");
    CHECK(compact(*s.element("volume").find("range")) == "[100,0,100,5]");
    s.ok("ui.accessibility_action", R"({"id": "volume", "action": "decrement"})");
    CHECK(compact(*s.element("volume").find("range")) == "[95,0,100,5]");
    // set_value types into a text field.
    s.ok("ui.accessibility_action", R"({"id": "player", "action": "set_value", "value": "Mira"})");
    const json::Value player = s.element("player");
    CHECK(player.find("value")->as_string() == "Mira");
    CHECK(player.find("placeholder") == nullptr);
    // focus and blur, which the tree shows.
    s.ok("ui.accessibility_action", R"({"id": "music", "action": "focus"})");
    CHECK(s.element("music").find("focused")->as_bool());
    s.ok("ui.accessibility_action", R"({"id": "music", "action": "blur"})");
    CHECK(s.element("music").find("focused") == nullptr);
    // Unsupported actions say why.
    CHECK(s.fails("ui.accessibility_action", R"({"id": "title", "action": "click"})") ==
          "/action: \"title\": a control that isn't a button doesn't take \"click\"");
    CHECK(s.fails("ui.accessibility_action", R"({"id": "volume", "action": "scroll_up"})") ==
          "/action: \"volume\": a control that isn't a ScrollContainer doesn't take \"scroll_up\"");
    s.ok("component.set", R"({"id": "resume", "components": {"Button": {"disabled": true}}})");
    CHECK(s.fails("ui.accessibility_action", R"({"id": "resume", "action": "click"})") ==
          "/action: \"resume\": the button is disabled");
    CHECK(compact(*s.element("resume").find("flags")) == R"(["disabled"])");
}

TEST_CASE("focus_mode accessibility takes focus only while a screen reader is served") {
    Session s;
    s.ok("entity.create", R"({"id": "file", "components": {
        "MenuButton": {"text": "File", "items": [{"text": "Open"}]}}})");
    CHECK(s.element("file").find("popup")->as_string() == "menu");
    // auto, with no screen reader active: a MenuButton can't take focus.
    CHECK_FALSE(ui::accessibility_enabled(s.session->world()));
    s.fails("ui.focus", R"({"id": "file"})");
    // always: it can.
    ui::Settings settings = ui::settings(s.session->world());
    settings.accessibility_support = "always";
    ui::set_settings(s.session->world(), settings);
    CHECK(s.ok("ui.accessibility").find("enabled")->as_bool());
    CHECK(s.ok("ui.focus", R"({"id": "file"})").find("focused")->as_string() == "file");
    // auto with a screen reader active, as a backend says.
    settings.accessibility_support = "auto";
    settings.screen_reader_active = true;
    ui::set_settings(s.session->world(), settings);
    CHECK(ui::accessibility_enabled(s.session->world()));
    settings.accessibility_support = "disabled";
    ui::set_settings(s.session->world(), settings);
    CHECK_FALSE(ui::accessibility_enabled(s.session->world()));
}

TEST_CASE("ui.accessibility shows an open menu's items, and click picks one") {
    Session s;
    s.ok("entity.create", R"({"id": "size", "components": {
        "OptionButton": {"selected": 1, "items": [{"text": "Small"}, {"text": "Large"},
                                                  {"text": "Huge", "disabled": true}]}}})");
    const json::Value closed = s.element("size");
    CHECK(closed.find("name")->as_string() == "Large");
    CHECK(closed.find("popup")->as_string() == "list");
    CHECK_FALSE(closed.find("checked")->as_bool());
    CHECK(closed.find("children") == nullptr);

    s.ok("ui.popup", R"({"id": "size"})");
    const json::Value open = s.element("size");
    const json::Value& menu = open.find("children")->as_array().at(0);
    CHECK(menu.find("role")->as_string() == "menu");
    const json::Array& items = menu.find("children")->as_array();
    REQUIRE(items.size() == 3);
    CHECK(items[1].find("role")->as_string() == "menu_item_radio");
    CHECK(items[1].find("name")->as_string() == "Large");
    CHECK(items[1].find("checked")->as_bool());
    CHECK(items[1].find("item")->as_double() == 1);
    CHECK(compact(*items[2].find("flags")) == R"(["disabled"])");
    // The menu has focus while it is open, on the option shown.
    CHECK(items[1].find("focused") != nullptr);
    CHECK(items[1].find("selected") != nullptr);
    CHECK(menu.find("focused") == nullptr);

    CHECK(s.fails("ui.accessibility_action", R"({"id": "size", "item": 2, "action": "click"})") ==
          "/item: \"size\": the item can't be picked: it doesn't exist, is a separator or is "
          "disabled");
    s.ok("ui.accessibility_action", R"({"id": "size", "item": 0, "action": "click"})");
    s.ok("step");
    CHECK(s.element("size").find("name")->as_string() == "Small");
}

TEST_CASE("ui.accessibility_action scrolls a ScrollContainer by pages and into view") {
    Session s;
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "a", "components": {
            "Control": {"custom_minimum_size": [50, 300]}}}},
        {"command": "entity.create", "params": {"id": "b", "components": {
            "Button": {"text": "Last"}, "Control": {"custom_minimum_size": [50, 40]}}}},
        {"command": "entity.create", "params": {"id": "list", "components": {
            "BoxContainer": {"vertical": true}, "Control": {"children": ["a", "b"]}}}},
        {"command": "entity.create", "params": {"id": "scroll", "components": {
            "ScrollContainer": {},
            "Control": {"children": ["list"], "offset_right": 100, "offset_bottom": 100}}}}
    ]})");
    const json::Value scroll = s.element("scroll");
    CHECK(scroll.find("role")->as_string() == "scroll_view");
    CHECK(compact(*scroll.find("flags")) == R"(["clips_children","touch_passthrough"])");
    const auto scrolled = [&s] {
        return s.ok("entity.get", R"({"id": "scroll"})")
            .find("components")
            ->find("ScrollContainer")
            ->find("scroll_vertical")
            ->as_double();
    };
    // A page down, then an item (an eighth of a page) up.
    s.ok("ui.accessibility_action", R"({"id": "scroll", "action": "scroll_down"})");
    CHECK(scrolled() == 100);
    s.ok("ui.accessibility_action", R"({"id": "scroll", "action": "scroll_up", "value": "item"})");
    CHECK(scrolled() == 87); // 100 less 12.5, in whole pixels
    s.ok("ui.accessibility_action",
         R"({"id": "scroll", "action": "set_scroll_offset", "value": [0, 0]})");
    CHECK(scrolled() == 0);
    // scroll_into_view scrolls only a ScrollContainer that is the control's parent, as Godot's:
    // "b" is the list's child, so nothing moves.
    s.ok("ui.accessibility_action", R"({"id": "b", "action": "scroll_into_view"})");
    CHECK(scrolled() == 0);
}
