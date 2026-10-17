#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Option buttons, menu buttons and popup menus, as Godot's OptionButton, MenuButton and
// PopupMenu (scene/gui/option_button.cpp, menu_button.cpp and popup_menu.cpp) in a viewport
// that embeds its popups (scene/main/viewport.cpp and popup.cpp). The sizes expected here follow
// those files and Godot's default theme.

namespace asset = slash::asset;
namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr double kTick = 1.0 / 60.0;

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

json::Value parse(std::string_view source) {
    auto parsed = json::parse(source);
    REQUIRE_MESSAGE(parsed.ok(), std::string(source));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// A number as JSON writes it, whole numbers without ".0".
std::string number(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return compact(json::Value(value));
}
std::string numbers(std::initializer_list<double> values) {
    std::string out = "[";
    for (const double value : values) {
        out += (out.size() > 1 ? "," : "") + number(value);
    }
    return out + "]";
}

std::string point(int x, int y) {
    return "[" + std::to_string(x) + ", " + std::to_string(y) + "]";
}
std::string motion(int x, int y, int dx = 1) {
    return R"({"type": "mouse_motion", "position": )" + point(x, y) +
           ", \"relative\": " + point(dx, 0) + "}";
}
std::string click(int x, int y, bool pressed, std::string_view button = "left") {
    return R"({"type": "mouse_button", "button": ")" + std::string(button) + R"(", "position": )" +
           point(x, y) + ", \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string touch(int x, int y, bool pressed) {
    return R"({"type": "screen_touch", "index": 0, "position": )" + point(x, y) +
           ", \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string key(std::string_view name, bool pressed) {
    return R"({"type": "key", "key": ")" + std::string(name) +
           "\", \"pressed\": " + (pressed ? "true" : "false") + "}";
}
// A key typed: its press with the character it makes.
std::string typed(std::string_view name, char unicode) {
    return R"({"type": "key", "key": ")" + std::string(name) +
           R"(", "pressed": true, "unicode": ")" + std::string(1, unicode) + "\"}";
}

// A world with the built-in components, ui and input, in a 400 x 300 viewport.
struct Gui {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Gui() {
        world::register_builtin_components(components);
        ui::install(world, assets, {.viewport = {400.0F, 300.0F}, .theme = ""});
        input::install(world, input::Map::with_defaults(), {});
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        set(id, components_json);
        return entity;
    }

    void set(std::string_view id, std::string_view components_json) {
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            auto assigned = components.find(name)->assign(world, world.find(id), fields, path);
            REQUIRE_MESSAGE(assigned.ok(), assigned.error().message);
        }
    }

    void push(std::string_view event_json) {
        auto parsed = input::parse_event(parse(event_json), input::Use::Event);
        REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
        input::push(world, std::move(parsed).value());
    }

    // Queues events, steps one tick and returns that tick's UI events.
    std::string send(std::initializer_list<std::string_view> events) {
        for (const std::string_view event : events) {
            push(event);
        }
        return step();
    }
    std::string send(std::string_view event) { return send({event}); }
    // A key pressed and let go within one tick.
    std::string tap(std::string_view name) { return send({key(name, true), key(name, false)}); }
    // A click pressed and let go within one tick.
    std::string tap(int x, int y) { return send({click(x, y, true), click(x, y, false)}); }

    std::string step() {
        world.step(kTick);
        return events();
    }
    // Steps until the time a held click picks an item has gone.
    void wait() {
        for (int i = 0; i < 30; ++i) {
            world.step(kTick);
        }
    }

    // This tick's UI events, with an item's index and ID and a toggle's state.
    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "toggled") {
                out += event.toggled_on ? " on" : " off";
            }
            if (event.index >= 0) {
                out += " " + std::to_string(event.index);
            }
            if (event.kind == "id_pressed" || event.kind == "id_focused") {
                out += " #" + std::to_string(event.item_id);
            }
        }
        return out;
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    // The open popups, the last on top, by ID.
    [[nodiscard]] std::string open() const {
        std::string out;
        for (const entt::entity entity : ui::open_popups(world)) {
            out += (out.empty() ? "" : ", ") + std::string(world.id_of(entity));
        }
        return out;
    }

    // The top popup as laid out.
    ui::PopupView popup() {
        ui::Layout placed = layout();
        REQUIRE_FALSE(placed.popups.empty());
        return placed.popups.back();
    }
    // The top popup's rect as [x, y, w, h].
    std::string popup_rect() {
        const ui::PopupView view = popup();
        return numbers({view.position.x, view.position.y, view.size.x, view.size.y});
    }
    // An item's middle in the viewport, in whole pixels.
    std::pair<int, int> item_point(int index) {
        const ui::PopupView view = popup();
        const ui::PopupItemView& item = view.items.at(static_cast<std::size_t>(index));
        const float x = view.position.x + view.list_position.x + view.list_size.x / 2.0F;
        const float y =
            view.position.y + view.list_position.y - view.scroll + item.y + item.height / 2.0F;
        return {static_cast<int>(x), static_cast<int>(y)};
    }
    // A control's rect as [x, y, w, h].
    std::string rect(std::string_view id) {
        const ui::Layout placed = layout();
        const ui::Placed* found = placed.find(world.find(id));
        if (found == nullptr) {
            return "none";
        }
        const Vec2 origin = found->transform.apply({0.0F, 0.0F});
        return numbers({origin.x, origin.y, found->size.x, found->size.y});
    }
    [[nodiscard]] bool pressed(std::string_view id) const {
        const entt::entity entity = world.find(id);
        if (const auto* option = world.ecs().try_get<world::OptionButton>(entity)) {
            return option->button_pressed;
        }
        return world.ecs().get<world::MenuButton>(entity).button_pressed;
    }
    [[nodiscard]] int selected(std::string_view id = "size") const {
        return world.ecs().get<world::OptionButton>(world.find(id)).selected;
    }
};

std::vector<std::string> problem_lines(const std::vector<ui::Problem>& problems) {
    std::vector<std::string> lines;
    lines.reserve(problems.size());
    for (const ui::Problem& problem : problems) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    return lines;
}

// An OptionButton "size" at (20, 20) with three options, the second shown.
constexpr std::string_view kSizes = R"({"Control": {"offset_left": 20, "offset_top": 20},
    "OptionButton": {"selected": 1, "items": [
        {"text": "Small"}, {"text": "Medium"}, {"text": "Large"}]}})";

} // namespace

TEST_CASE("an option button lists its options below it, and a click picks one") {
    Gui gui;
    gui.create("size", kSizes);
    // As wide as its widest option, its margins and its arrow.
    CHECK(gui.rect("size") == "[20,20,90,30]");
    CHECK(gui.tap(30, 30) ==
          "mouse_entered size, focus_entered size, button_down size, toggled size on, "
          "pressed size, button_up size");
    CHECK(gui.open() == "size");
    CHECK(gui.pressed("size"));

    // The list is below the button and at least as wide: the panel's margins, the scroll bar
    // PopupMenu always leaves room for, a radio button and the widest text; each item is a line
    // of text with v_separation between them. Opened by the mouse, no item is focused.
    CHECK(gui.popup_rect() == "[20,50,98,86]");
    ui::PopupView view = gui.popup();
    CHECK(view.focused == -1);
    CHECK(view.bar_width == 0.0F);
    CHECK(numbers({view.list_position.x, view.list_position.y, view.list_size.x,
                   view.list_size.y}) == "[4,4,90,78]");
    REQUIRE(view.items.size() == 3);
    CHECK(view.items[0].y == 2.0F);
    CHECK(view.items[1].y == 28.0F);
    CHECK(view.items[2].y == 54.0F);
    CHECK(view.items[2].height == 22.0F);
    CHECK(view.items[1].text == "Medium");

    // The pointer over an item focuses it, and leaves the button.
    const auto [x, y] = gui.item_point(2);
    CHECK(gui.send(motion(x, y)) == "mouse_exited size");
    CHECK(gui.popup().focused == 2);
    CHECK(gui.tap(x, y) == "toggled size off, item_selected size 2");
    CHECK(gui.open().empty());
    CHECK(gui.selected() == 2);
    CHECK_FALSE(gui.pressed("size"));
    // The button shows the option picked.
    CHECK(gui.layout().label(gui.world.find("size"))->source == "Large");

    // Picking the option shown again emits nothing, unless allow_reselect says so.
    gui.tap(30, 30);
    CHECK(gui.tap(gui.item_point(2).first, gui.item_point(2).second) ==
          "mouse_exited size, toggled size off");
    gui.set("size", R"({"OptionButton": {"allow_reselect": true}})");
    gui.tap(30, 30);
    CHECK(gui.tap(gui.item_point(2).first, gui.item_point(2).second) ==
          "mouse_exited size, toggled size off, item_selected size 2");

    // A click on the panel's margin does nothing; one on the button closes the list.
    gui.tap(30, 30);
    const ui::PopupView again = gui.popup();
    CHECK(gui.tap(static_cast<int>(again.position.x) + 1, static_cast<int>(again.position.y) + 1) ==
          "mouse_exited size");
    CHECK(gui.open() == "size");
    CHECK(gui.tap(30, 30) ==
          "mouse_entered size, button_down size, toggled size off, pressed size, button_up size");
    CHECK(gui.open().empty());
}

TEST_CASE("a click held from the button picks the item it is let go over, after a moment") {
    Gui gui;
    gui.create("size", kSizes);
    CHECK(gui.send(click(30, 30, true)) ==
          "mouse_entered size, focus_entered size, button_down size, toggled size on, "
          "pressed size, button_up size");
    const auto [x, y] = gui.item_point(0);
    // Let go too soon, the list stays open, as PopupMenu ignores a release within 0.4 seconds
    // of a held click opening it.
    gui.send(motion(x, y));
    CHECK(gui.send(click(x, y, false)).empty());
    CHECK(gui.open() == "size");
    gui.tap(30, 30);

    // Held long enough, the release picks the item under it.
    gui.send(click(30, 30, true));
    gui.wait();
    gui.send(motion(x, y));
    CHECK(gui.send(click(x, y, false)) == "toggled size off, item_selected size 0");
    CHECK(gui.selected() == 0);

    // Let go away from the items, it stays open, then a click outside closes it.
    gui.send(click(30, 30, true));
    gui.wait();
    CHECK(gui.send(click(300, 250, false)) == "mouse_exited size");
    CHECK(gui.open() == "size");
    CHECK(gui.tap(300, 250) == "toggled size off");
    CHECK(gui.open().empty());
}

TEST_CASE("keys open the list on the option shown, skip separators and disabled items, and pick") {
    Gui gui;
    gui.create("size", R"({"Control": {"offset_left": 20, "offset_top": 20},
        "OptionButton": {"selected": 0, "items": [
            {"text": "Small"}, {"separator": true}, {"text": "Medium", "disabled": true},
            {"text": "Large", "id": 30}]}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("size")));
    gui.step();
    // Opened by a key, the option shown is focused.
    CHECK(gui.tap("Enter") == "button_down size, toggled size on, pressed size");
    CHECK(gui.popup().focused == 0);
    // Down goes past the separator and the disabled option, then round to the top.
    CHECK(gui.tap("Down") == "item_focused size 3");
    CHECK(gui.tap("Down") == "item_focused size 0");
    CHECK(gui.tap("Up") == "item_focused size 3");
    // The key's release reaches the button once the list has closed.
    CHECK(gui.tap("Enter") == "toggled size off, item_selected size 3, button_up size");
    CHECK(gui.selected() == 3);

    // ui_cancel closes the list and changes nothing.
    gui.tap("Enter");
    CHECK(gui.popup().focused == 3);
    CHECK(gui.tap("Escape") == "toggled size off");
    CHECK(gui.open().empty());
    CHECK(gui.selected() == 3);

    // With the option shown disabled, the list starts on the first that isn't.
    gui.set("size", R"({"OptionButton": {"selected": 2}})");
    gui.tap("Enter");
    CHECK(gui.popup().focused == 0);
}

TEST_CASE("a popup menu opens at its position, fits in the viewport and scrolls") {
    Gui gui;
    gui.create("menu", R"({"PopupMenu": {"position": [380, 10], "items": [
        {"text": "Copy", "id": 7}, {"text": "Paste"}]}})");
    CHECK(gui.open().empty());
    // Made visible, it opens at the next tick, moved left to fit.
    gui.set("menu", R"({"PopupMenu": {"visible": true}})");
    CHECK(gui.step() == "about_to_popup menu");
    CHECK(gui.open() == "menu");
    const ui::PopupView view = gui.popup();
    CHECK(view.position == Vec2{400.0F - view.size.x, 10.0F});
    // Picking an item emits its ID and index, and closes it.
    const auto [x, y] = gui.item_point(0);
    gui.send(motion(x, y));
    CHECK(gui.tap(x, y) == "popup_hide menu, id_pressed menu 0 #7, index_pressed menu 0");
    CHECK_FALSE(gui.world.ecs().get<world::PopupMenu>(gui.world.find("menu")).visible);

    // Taller than the viewport, it is as tall as the viewport and scrolls.
    std::string items;
    for (int i = 0; i < 20; ++i) {
        items += (items.empty() ? "" : ", ") + std::string(R"({"text": "Item )") +
                 std::to_string(i) + "\"}";
    }
    gui.set("menu",
            R"({"PopupMenu": {"visible": true, "position": [0, 0], "items": [)" + items + "]}}");
    CHECK(gui.step() == "about_to_popup menu");
    ui::PopupView tall = gui.popup();
    CHECK(tall.size.y == 300.0F);
    CHECK(tall.bar_width > 0.0F);
    CHECK(tall.scroll_max == 20.0F * 26.0F - 292.0F);
    CHECK(tall.scroll == 0.0F);
    // The wheel scrolls it an eighth of its page.
    gui.send({click(20, 100, true, "wheel_down"), click(20, 100, false, "wheel_down")});
    CHECK(gui.popup().scroll == std::round(292.0F / 8.0F));
    // The keys scroll to the item they move to: up goes round to the last.
    gui.tap("Up");
    tall = gui.popup();
    CHECK(tall.focused == 19);
    // Scrolled until the item is in view, without the half separation after it.
    CHECK(tall.scroll == tall.scroll_max - 2.0F);
    // A click outside closes it.
    CHECK(gui.tap(399, 299) == "popup_hide menu");
}

TEST_CASE("typing the start of an item's text moves to it") {
    Gui gui;
    gui.create("menu", R"({"PopupMenu": {"items": [{"text": "Apple"}, {"text": "Banana"},
        {"text": "blueberry"}, {"text": "Cherry"}]}})");
    REQUIRE(ui::open_popup(gui.world, gui.world.find("menu")));
    CHECK(gui.step() == "about_to_popup menu");
    // The letters typed within two seconds of each other make up what it looks for, in any
    // case.
    CHECK(gui.send(typed("B", 'b')) == "id_focused menu #1");
    CHECK(gui.send(typed("L", 'l')) == "id_focused menu #2");
    // Later, it looks from the item focused.
    for (int i = 0; i < 130; ++i) {
        gui.step();
    }
    CHECK(gui.send(typed("A", 'a')) == "id_focused menu #0");
    // The same letter again goes to the next item that starts with it.
    for (int i = 0; i < 130; ++i) {
        gui.step();
    }
    CHECK(gui.send(typed("B", 'b')) == "id_focused menu #1");
    CHECK(gui.send(typed("B", 'b')) == "id_focused menu #2");
    CHECK(gui.send(typed("B", 'b')) == "id_focused menu #1");

    // allow_search off, typing does nothing.
    gui.set("menu", R"({"PopupMenu": {"allow_search": false}})");
    CHECK(gui.send(typed("C", 'c')).empty());
}

TEST_CASE("a click outside a popup closes it and still reaches the control under it") {
    Gui gui;
    gui.create("ok", R"({"Control": {"offset_left": 300, "offset_top": 250, "offset_right": 380,
        "offset_bottom": 290}, "Button": {"text": "OK"}})");
    gui.create("menu", R"({"PopupMenu": {"position": [10, 10], "visible": true,
        "hide_on_checkable_item_selection": false, "items": [
            {"text": "Grid", "checkable": "check", "checked": true}, {"text": "Quit"}]}})");
    gui.step();
    // A checkable item picked keeps the menu open when hide_on_checkable_item_selection is off.
    const auto [x, y] = gui.item_point(0);
    gui.send(motion(x, y));
    CHECK(gui.tap(x, y) == "id_pressed menu 0 #0, index_pressed menu 0");
    CHECK(gui.open() == "menu");
    // The menu draws a check box beside each checkable item.
    CHECK(gui.popup().size.x > 8.0F + 16.0F + 4.0F + 4.0F);
    // A click outside closes it at the end of the tick, and presses what it lands on.
    CHECK(gui.tap(340, 270) ==
          "mouse_entered ok, focus_entered ok, button_down ok, pressed ok, button_up ok, "
          "popup_hide menu");
    CHECK(gui.open().empty());
}

TEST_CASE("menu buttons open their menus, and switch on hover as a menu bar") {
    Gui gui;
    gui.create("file", R"({"Control": {"offset_left": 0, "offset_top": 0},
        "MenuButton": {"text": "File", "switch_on_hover": true, "items": [
            {"text": "New"}, {"text": "Open", "id": 12}]}})");
    gui.create("edit", R"({"Control": {"offset_left": 60, "offset_top": 0},
        "MenuButton": {"text": "Edit", "switch_on_hover": true, "items": [{"text": "Undo"}]}})");
    gui.create("bar", R"({"Control": {"children": ["file", "edit"]}})");
    // A MenuButton takes no focus, and is pressed while its menu is open.
    CHECK(gui.tap(10, 10) == "mouse_entered file, button_down file, toggled file on, "
                             "about_to_popup file, pressed file, button_up file");
    CHECK(gui.open() == "file");
    // Its menu is below it, left aligned.
    const ui::Layout placed = gui.layout();
    const ui::Placed* file = placed.find(gui.world.find("file"));
    CHECK(placed.popups.back().position == Vec2{0.0F, file->size.y});
    // Moving onto the other button opens its menu instead.
    CHECK(gui.send(motion(70, 10)) ==
          "mouse_exited file, mouse_entered edit, popup_hide file, toggled file off, "
          "about_to_popup edit, toggled edit on");
    CHECK(gui.open() == "edit");
    CHECK(gui.popup().focused == -1);
    // An item picked comes from the button.
    gui.send(motion(70, 10));
    const auto [x, y] = gui.item_point(0);
    gui.send(motion(x, y));
    CHECK(gui.tap(x, y) == "popup_hide edit, toggled edit off, id_pressed edit 0 #0, "
                           "index_pressed edit 0");
    // Opened by keys, the first item is focused.
    gui.set("file", R"({"Control": {"focus_mode": "all"}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("file")));
    gui.tap("Enter");
    CHECK(gui.popup().focused == 0);
    CHECK(gui.tap("Down") == "id_focused file #12");
}

TEST_CASE("a tap opens a list and picks from it, and ui::press opens one as a key would") {
    Gui gui;
    gui.create("size", kSizes);
    // A touch counts as the mouse: no item is focused.
    CHECK(gui.send({touch(30, 30, true), touch(30, 30, false)}).find("pressed size") !=
          std::string::npos);
    CHECK(gui.open() == "size");
    CHECK(gui.popup().focused == -1);
    gui.wait();
    const auto [x, y] = gui.item_point(0);
    CHECK(gui.send({touch(x, y, true), touch(x, y, false)}).find("item_selected size 0") !=
          std::string::npos);
    CHECK(gui.selected() == 0);

    ui::press(gui.world, gui.world.find("size"));
    CHECK(gui.step() == "toggled size on, pressed size");
    CHECK(gui.popup().focused == 0);
    // Hidden, the button closes its list; destroyed, the list is gone too.
    gui.set("size", R"({"Control": {"visible": false}})");
    CHECK(gui.step() == "focus_exited size, toggled size off");
    CHECK(gui.open().empty());
    gui.set("size", R"({"Control": {"visible": true}})");
    REQUIRE(ui::open_popup(gui.world, gui.world.find("size")));
    gui.world.destroy(gui.world.find("size"));
    gui.step();
    CHECK(gui.open().empty());
    CHECK(gui.layout().popups.empty());
}

TEST_CASE("right to left, a popup menu puts its scroll bar on the left") {
    Gui gui;
    std::string items;
    for (int i = 0; i < 20; ++i) {
        items += (items.empty() ? "" : ", ") + std::string(R"({"text": "Item )") +
                 std::to_string(i) + "\"}";
    }
    gui.create("menu", R"({"PopupMenu": {"visible": true, "layout_direction": "rtl",
        "items": [)" + items +
                           "]}}");
    gui.step();
    const ui::PopupView view = gui.popup();
    CHECK(view.rtl);
    REQUIRE(view.bar_width > 0.0F);
    CHECK(view.list_position.x == view.margins[0] + view.bar_width);
    REQUIRE_FALSE(view.bar.empty());
    CHECK(view.bar.front().position.x == view.margins[0]);
    // Its text ends at the right.
    REQUIRE_FALSE(view.labels.empty());
    CHECK(view.labels.front().pens.at(0).x > view.list_position.x + 8.0F);
}

TEST_CASE("check finds wrong popup items, popup menus on controls and options out of range") {
    Gui gui;
    gui.create("menu", R"({"PopupMenu": {"items": [{"text": "A", "checkable": "radi"}]}})");
    gui.create("bad", R"({"Control": {}, "PopupMenu": {}})");
    gui.create("size", R"({"Control": {}, "OptionButton": {"selected": 3,
        "items": [{"text": "One"}]}})");
    gui.create("both", R"({"Control": {}, "OptionButton": {}, "MenuButton": {}})");
    CHECK(problem_lines(ui::check(gui.world, gui.fonts())) ==
          std::vector<std::string>{
              "bad/components/PopupMenu: a PopupMenu is a window of its own, not a control; give "
              "it an entity of its own",
              "both/components/MenuButton: an entity is one button; it has an OptionButton too",
              "menu/components/PopupMenu/items/0/checkable: \"radi\" isn't \"none\", \"check\" "
              "or \"radio\"; did you mean \"radio\"?",
              "size/components/OptionButton/selected: it has 1 option; -1 selects none"});
}

namespace {

struct Session {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Session() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        session =
            std::make_unique<command::Session>(components, commands, command::SessionOptions{});
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") {
        return session->call(name, parse(params));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    std::string fails(std::string_view name, std::string_view params) {
        const command::Reply reply = call(name, params);
        REQUIRE_FALSE(reply.ok);
        return reply.message;
    }
};

} // namespace

TEST_CASE("ui.popup, ui.select and ui.layout open, pick from and report popup menus") {
    Session s;
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "size", "components": {
            "Control": {"offset_left": 20, "offset_top": 20},
            "OptionButton": {"selected": 0, "items": [
                {"text": "Small"}, {"separator": true}, {"text": "Large", "disabled": true},
                {"text": "Huge", "id": 40}]}}}},
        {"command": "entity.create", "params": {"id": "menu", "components": {
            "PopupMenu": {"position": [200, 100], "items": [{"text": "Copy", "id": 7}]}}}},
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Control": {}, "Label": {"text": "Menu"}}}}]})");
    CHECK(s.ok("ui.popup", R"({"id": "size"})") == parse(R"({"open": ["size"]})"));
    // Opened by a command, not a click, the button isn't pressed and the option shown is
    // focused. Item rects are in the viewport; a separator with no text is a line of no height.
    CHECK(s.ok("ui.layout", R"({"ids": ["size"]})") == parse(R"({"viewport": [1280, 720],
        "controls": [{"id": "size", "rect": [20, 20, 73, 30], "draw_mode": "normal",
                      "lines": ["Small"]}],
        "popups": [{"id": "size", "rect": [20, 50, 81, 90], "focused_item": 0, "items": [
            {"text": "Small", "rect": [24, 56, 73, 22]},
            {"text": "", "rect": [24, 82, 73, 0], "selectable": false},
            {"text": "Large", "rect": [24, 86, 73, 22], "selectable": false},
            {"text": "Huge", "rect": [24, 112, 73, 22]}]}]})"));
    // frame.summary shows the list over the button, with its items.
    const json::Value summary = s.ok("frame.summary", R"({"ids": ["size"]})");
    CHECK(*summary.find("drawn") == parse(R"([
        {"id": "size", "kind": "OptionButton", "rect": [20, 20, 73, 30], "text": "Small"},
        {"id": "size", "kind": "PopupMenu", "rect": [20, 50, 81, 90], "focused_item": 0,
         "items": ["Small", "", "Large", "Huge"]}])"));
    // Opening another closes it.
    CHECK(s.ok("ui.popup", R"({"id": "menu"})") == parse(R"({"open": ["menu"]})"));
    // Opened again before the next tick, the first stays open and the other closes.
    CHECK(s.ok("ui.popup", R"({"id": "size"})") == parse(R"({"open": ["size"]})"));
    s.ok("step");
    CHECK(s.ok("ui.popup", R"({"id": "size", "open": false})") == parse(R"({"open": []})"));

    // Items are picked by index, ID or text, as a click would.
    CHECK(s.ok("ui.select", R"({"id": "size", "text": "Huge"})") ==
          parse(R"({"index": 3, "tick": 1})"));
    s.ok("step");
    CHECK(s.ok("entity.get", R"({"id": "size", "components": ["OptionButton"]})")
              .find("components")
              ->find("OptionButton")
              ->find("selected")
              ->as_int() == 3);
    CHECK(s.ok("ui.select", R"({"id": "menu", "item_id": 7})") ==
          parse(R"({"index": 0, "tick": 2})"));
    CHECK(s.fails("ui.select", R"({"id": "size", "text": "Hug"})") ==
          "/text: no item of \"size\" reads \"Hug\"; did you mean \"Huge\"?");
    CHECK(s.fails("ui.select", R"({"id": "size", "index": 1})") ==
          "/index: item 1 of \"size\" is a separator");
    CHECK(s.fails("ui.select", R"({"id": "size", "index": 2})") ==
          "/index: item 2 of \"size\" is disabled");
    CHECK(s.fails("ui.select", R"({"id": "size", "index": 9})") == "/index: \"size\" has 4 items");
    CHECK(s.fails("ui.select", R"({"id": "size"})") == ": give one of index, item_id or text");
    CHECK(s.fails("ui.popup", R"({"id": "title"})") ==
          "/id: \"title\" has no popup menu: it has no PopupMenu, OptionButton or MenuButton");
}
