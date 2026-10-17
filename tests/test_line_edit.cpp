#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// LineEdit, as Godot's (scene/gui/line_edit.cpp): the events, carets, selections and undo
// steps expected here follow it. Text is drawn in a box font, every character 12 pixels wide at
// 20 pixels and a space 5, so where clicks land is easy to work out.

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
// Where a field's text starts: the default style's 4-pixel margin.
constexpr float kLeft = 4.0F;
constexpr float kBox = 12.0F;

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

std::string bytes_of(const std::vector<std::uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}

std::string number(float value) {
    return std::to_string(static_cast<int>(value));
}

std::string click(float x, float y, bool pressed, std::string_view extra = "") {
    std::string out = R"({"type": "mouse_button", "button": "left", "position": [)" + number(x) +
                      ", " + number(y) + "], \"pressed\": " + (pressed ? "true" : "false");
    if (!extra.empty()) {
        out += ", " + std::string(extra);
    }
    return out + "}";
}
// Mouse motion; the buttons held come from the input state.
std::string drag(float x, float y) {
    return R"({"type": "mouse_motion", "position": [)" + number(x) + ", " + number(y) + "]}";
}
std::string key(std::string_view name, bool pressed, std::string_view modifiers = "") {
    std::string out = R"({"type": "key", "key": ")" + std::string(name) +
                      "\", \"pressed\": " + (pressed ? "true" : "false");
    if (!modifiers.empty()) {
        out += ", " + std::string(modifiers);
    }
    return out + "}";
}
std::string typed(std::string_view text) {
    return R"({"type": "text", "text": ")" + std::string(text) + "\"}";
}
std::string ime(std::string_view text, std::string_view selection = "") {
    std::string out = R"({"type": "ime", "text": ")" + std::string(text) + "\"";
    if (!selection.empty()) {
        out += ", \"selection\": " + std::string(selection);
    }
    return out + "}";
}

constexpr std::string_view kCtrl = R"("ctrl": true)";
constexpr std::string_view kShift = R"("shift": true)";
constexpr std::string_view kCtrlShift = R"("ctrl": true, "shift": true)";

// A world with the built-in components, ui and input with a PC keyboard's shortcuts, in a 400
// x 300 viewport, and a box font.
struct Fields {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Fields() {
        files.files["boxes.ttf"] = bytes_of(text::box_font(
            U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,-_!?かな仮名•"));
        world::register_builtin_components(components);
        ui::install(world, assets,
                    {.viewport = {400.0F, 300.0F}, .theme = "", .allow_system_fallback = false});
        input::install(world, input::Map::with_defaults(input::Keyboard::Pc), {});
    }

    void set(std::string_view id, std::string_view components_json) {
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            auto assigned = components.find(name)->assign(world, world.find(id), fields, path);
            REQUIRE_MESSAGE(assigned.ok(), assigned.error().message);
        }
    }

    // A LineEdit at [x, y, w, h] drawn in the box font at 20 pixels.
    entt::entity field(std::string_view id, std::array<int, 4> rect,
                       std::string_view line_edit_fields = "") {
        const entt::entity entity = world.create(id).value();
        set(id, "{\"Control\": {\"offset_left\": " + std::to_string(rect[0]) +
                    ", \"offset_top\": " + std::to_string(rect[1]) +
                    ", \"offset_right\": " + std::to_string(rect[0] + rect[2]) +
                    ", \"offset_bottom\": " + std::to_string(rect[1] + rect[3]) +
                    R"(, "theme_override_fonts": {"font": "boxes.ttf"},)" +
                    R"( "theme_override_font_sizes": {"font_size": 20}}, "LineEdit": {)" +
                    std::string(line_edit_fields) + "}}");
        return entity;
    }

    void push(std::string_view event_json) {
        auto parsed = input::parse_events(parse(event_json));
        REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
        for (input::Event& event : parsed.value()) {
            input::push(world, std::move(event));
        }
    }

    // Queues events, steps one tick and returns that tick's UI events.
    std::string send(std::initializer_list<std::string_view> events) {
        for (const std::string_view event : events) {
            push(event);
        }
        world.step(kTick);
        return this->events();
    }
    std::string send(std::string_view event) { return send({event}); }
    std::string tap(std::string_view name, std::string_view modifiers = "") {
        return send({key(name, true, modifiers), key(name, false, modifiers)});
    }
    std::string step() { return send({}); }

    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "editing_toggled") {
                out += event.toggled_on ? " on" : " off";
            }
            if (event.kind.starts_with("text_")) {
                out += " \"" + event.text + "\"";
            }
        }
        return out;
    }

    [[nodiscard]] const world::LineEdit& edit(std::string_view id) const {
        return world.ecs().get<world::LineEdit>(world.find(id));
    }
    [[nodiscard]] std::string text(std::string_view id) const { return edit(id).text; }
    [[nodiscard]] int caret(std::string_view id) const { return edit(id).caret_column; }
    [[nodiscard]] std::optional<std::pair<int, int>> selection(std::string_view id) const {
        return ui::selection(world, world.find(id));
    }
    [[nodiscard]] bool editing(std::string_view id) const {
        return ui::is_editing(world, world.find(id));
    }

    ui::Layout layout() {
        return ui::layout(world, *ui::fonts(world), ui::settings(world).viewport);
    }
    ui::LineEditView view(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.line_edits.find(world.find(id));
        REQUIRE(found != placed.line_edits.end());
        return found->second;
    }
};

using Selection = std::optional<std::pair<int, int>>;

} // namespace

TEST_CASE("a click starts editing a LineEdit, and typing emits text_changed once a tick") {
    Fields f;
    f.field("name", {20, 20, 300, 40}, R"("placeholder_text": "Your name")");
    CHECK_FALSE(f.editing("name"));
    CHECK(f.send(click(40, 40, true)) ==
          "mouse_entered name, focus_entered name, editing_toggled name on");
    CHECK(f.editing("name"));
    CHECK(f.send(click(40, 40, false)).empty());
    // The platform is asked for text input and the on-screen keyboard, next to the field.
    const input::TextInput request = ui::text_input(f.world);
    CHECK(request.active);
    CHECK(request.show_keyboard);
    CHECK(request.keyboard == "default");
    CHECK(request.position == Vec2{20.0F, 20.0F});
    CHECK(request.size == Vec2{300.0F, 40.0F});
    CHECK(request.caret == static_cast<int>(kLeft));

    // Typed characters go in at the caret; one text_changed for the tick.
    CHECK(f.send(typed("Mira")) == R"(text_changed name "Mira")");
    CHECK(f.text("name") == "Mira");
    CHECK(f.caret("name") == 4);
    CHECK(f.view("name").caret_x == kLeft + (4.0F * kBox));
    CHECK(f.send(typed(" K")) == R"(text_changed name "Mira K")");
    // A key with no character and no action does nothing.
    CHECK(f.tap("F5").empty());
    CHECK(f.text("name") == "Mira K");
    // A character typed with Ctrl isn't typed: shortcuts go first, and then it is, as
    // unhandled_key_input does.
    CHECK(f.send(R"({"type": "key", "key": "Q", "unicode": "q", "ctrl": true, "pressed": true})") ==
          R"(text_changed name "Mira Kq")");

    // Clicked away, it stops editing.
    CHECK(f.send({click(380, 280, true), click(380, 280, false)}) == "mouse_exited name");
    CHECK(f.editing("name"));
    ui::release_focus(f.world);
    CHECK(f.step() == "focus_exited name, editing_toggled name off");
    CHECK_FALSE(f.editing("name"));
    CHECK_FALSE(ui::text_input(f.world).active);
}

TEST_CASE("keys move the caret, select with shift and delete characters and words") {
    Fields f;
    f.field("line", {20, 20, 360, 40}, R"("text": "hello big world")");
    ui::grab_focus(f.world, f.world.find("line"));
    CHECK(f.step() == "focus_entered line, editing_toggled line on");
    CHECK(f.editing("line"));
    // The caret starts where the component has it: 0.
    CHECK(f.caret("line") == 0);
    f.tap("End");
    CHECK(f.caret("line") == 15);
    f.tap("Left", kCtrl);
    CHECK(f.caret("line") == 10);
    f.tap("Left", kCtrlShift);
    CHECK(f.caret("line") == 6);
    CHECK(f.selection("line") == Selection{{6, 10}});
    f.tap("Right", kShift);
    CHECK(f.selection("line") == Selection{{7, 10}});
    // Left without shift goes to the selection's start and drops it.
    f.tap("Left");
    CHECK(f.caret("line") == 7);
    CHECK(f.selection("line") == Selection{});
    f.tap("Home", kShift);
    CHECK(f.selection("line") == Selection{{0, 7}});
    // Typing replaces the selection.
    CHECK(f.send(typed("a ")) == R"(text_changed line "a ig world")");
    CHECK(f.caret("line") == 2);

    CHECK(f.tap("Backspace") == R"(text_changed line "aig world")");
    CHECK(f.tap("Delete") == R"(text_changed line "ag world")");
    f.tap("End");
    CHECK(f.tap("Backspace", kCtrl) == R"(text_changed line "ag ")");
    f.tap("Home");
    CHECK(f.tap("Delete", kCtrl) == R"(text_changed line " ")");
    // Nothing left of the caret: nothing to delete.
    CHECK(f.tap("Backspace").empty());
    CHECK(f.send(typed("x")) == R"(text_changed line "x ")");

    // Ctrl+A selects all; Up and Down go to the start and the end.
    f.tap("A", kCtrl);
    CHECK(f.selection("line") == Selection{{0, 2}});
    f.tap("Down");
    CHECK(f.caret("line") == 2);
    CHECK(f.selection("line") == Selection{});
    f.tap("Up");
    CHECK(f.caret("line") == 0);
}

TEST_CASE("characters made of several code points are one step for the caret") {
    Fields f;
    // e and a combining acute accent; a flag of two regional indicators.
    f.field("word", {20, 20, 360, 40}, "\"text\": \"e\\u0301x\\ud83c\\uddef\\ud83c\\uddf5\"");
    ui::grab_focus(f.world, f.world.find("word"));
    f.step();
    f.tap("Right");
    CHECK(f.caret("word") == 2);
    f.tap("End");
    CHECK(f.caret("word") == 5);
    f.tap("Left");
    CHECK(f.caret("word") == 3);
    // Backspace deletes one code point, unless backspace_deletes_composite_character_enabled.
    f.tap("End");
    f.tap("Backspace");
    CHECK(slash::to_utf32(f.text("word")).size() == 4);
    f.set("word", R"({"LineEdit": {"text": "aé", "caret_column": 3,
                                    "backspace_deletes_composite_character_enabled": true}})");
    f.tap("Backspace");
    CHECK(f.text("word") == "a");
}

TEST_CASE("undo and redo step through what each tick typed") {
    Fields f;
    f.field("line", {20, 20, 360, 40});
    ui::grab_focus(f.world, f.world.find("line"));
    f.step();
    f.send(typed("ab"));
    f.send(typed("cd"));
    CHECK(f.tap("Z", kCtrl) == R"(text_changed line "ab")");
    CHECK(f.caret("line") == 2);
    CHECK(f.tap("Z", kCtrl) == R"(text_changed line "")");
    // Nothing more to undo.
    CHECK(f.tap("Z", kCtrl).empty());
    CHECK(f.tap("Y", kCtrl) == R"(text_changed line "ab")");
    CHECK(f.tap("Z", kCtrlShift) == R"(text_changed line "abcd")");
    CHECK(f.tap("Z", kCtrl) == R"(text_changed line "ab")");
    // Typing drops what could be redone.
    f.send(typed("x"));
    CHECK(f.tap("Y", kCtrl).empty());
    CHECK(f.tap("Z", kCtrl) == R"(text_changed line "ab")");

    // Text set from outside starts the history again, from the text it had.
    f.set("line", R"({"LineEdit": {"text": "fresh", "caret_column": 5}})");
    f.tap("A", kCtrl);
    CHECK(f.selection("line") == Selection{{0, 5}});
    f.set("line", R"({"LineEdit": {"text": "newer"}})");
    CHECK(f.selection("line") == Selection{});
    CHECK(f.tap("Z", kCtrl) == R"(text_changed line "fresh")");
    CHECK(f.tap("Z", kCtrl).empty());
}

TEST_CASE("copy, cut and paste use the clipboard, and secret fields don't copy") {
    Fields f;
    f.field("line", {20, 20, 360, 40}, R"("text": "one two")");
    f.field("pin", {20, 80, 360, 40}, R"("text": "1234", "secret": true)");
    ui::grab_focus(f.world, f.world.find("line"));
    f.step();
    f.tap("End");
    f.tap("Left", kCtrlShift);
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "two");
    CHECK(f.tap("X", kCtrl) == R"(text_changed line "one ")");
    CHECK(ui::clipboard(f.world) == "two");
    f.tap("Home");
    CHECK(f.tap("V", kCtrl) == R"(text_changed line "twoone ")");
    // Line breaks and tabs are left out of what is pasted.
    ui::set_clipboard(f.world, "a\tb\nc");
    CHECK(f.tap("V", kCtrl) == R"(text_changed line "twoabcone ")");

    // A backend shares the system's clipboard.
    std::string system = "from the system";
    ui::set_clipboard_backend(f.world, {.get = [&] { return system; },
                                        .set = [&](const std::string& value) { system = value; }});
    CHECK(ui::clipboard(f.world) == "from the system");
    f.tap("A", kCtrl);
    f.tap("C", kCtrl);
    CHECK(system == "twoabcone ");
    ui::set_clipboard_backend(f.world, {});
    CHECK(ui::clipboard(f.world) == "twoabcone ");

    ui::grab_focus(f.world, f.world.find("pin"));
    f.step();
    f.tap("A", kCtrl);
    CHECK(f.selection("pin") == Selection{{0, 4}});
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "twoabcone ");
    // Secret text shows as dots.
    const ui::Layout placed = f.layout();
    CHECK(placed.label(f.world.find("pin"))->source == "••••");
}

TEST_CASE("max_length rejects what doesn't fit") {
    Fields f;
    f.field("code", {20, 20, 360, 40}, R"("max_length": 4, "text": "ab")");
    ui::grab_focus(f.world, f.world.find("code"));
    f.step();
    f.tap("End");
    CHECK(f.send(typed("cdef")) ==
          R"(text_change_rejected code "e", text_change_rejected code "f", )"
          R"(text_changed code "abcd")");
    ui::set_clipboard(f.world, "xyz");
    CHECK(f.tap("V", kCtrl) == R"(text_change_rejected code "xyz")");
    CHECK(f.text("code") == "abcd");
}

TEST_CASE("Enter submits and stops editing, Escape stops editing, Enter starts again") {
    Fields f;
    f.field("chat", {20, 20, 360, 40}, R"("text": "hi")");
    ui::grab_focus(f.world, f.world.find("chat"));
    f.step();
    CHECK(f.tap("Enter") == R"(text_submitted chat "hi", editing_toggled chat off)");
    CHECK_FALSE(f.editing("chat"));
    CHECK(ui::focus_owner(f.world) == f.world.find("chat"));
    // Not editing, typing does nothing; Enter starts editing again.
    CHECK(f.send(typed("x")).empty());
    CHECK(f.tap("Enter") == "editing_toggled chat on");
    CHECK(f.tap("Escape") == "editing_toggled chat off");
    CHECK(f.tap("Kp Enter") == "editing_toggled chat on");

    f.set("chat", R"({"LineEdit": {"keep_editing_on_text_submit": true}})");
    CHECK(f.tap("Enter") == R"(text_submitted chat "hi")");
    CHECK(f.editing("chat"));
    // Made read only, it stops editing.
    f.set("chat", R"({"LineEdit": {"editable": false}})");
    CHECK(f.step() == "editing_toggled chat off");
    CHECK(f.tap("Enter").empty());
}

TEST_CASE("Tab starts editing the LineEdit it focuses; arrow keys only focus it") {
    Fields f;
    REQUIRE(f.world.create("ok"));
    f.set("ok", R"({"Control": {"offset_left": 20, "offset_top": 80, "offset_right": 120,
                                "offset_bottom": 120}, "Button": {"text": "OK"}})");
    f.field("name", {20, 20, 300, 40});
    // Focus moves between the controls of one tree.
    REQUIRE(f.world.create("form"));
    f.set("form", R"({"Control": {"children": ["name", "ok"]}})");
    ui::grab_focus(f.world, f.world.find("ok"));
    f.step();
    CHECK(f.tap("Up") == "focus_exited ok, focus_entered name");
    CHECK_FALSE(f.editing("name"));
    // Not editing, Down moves focus on.
    CHECK(f.tap("Down") == "focus_exited name, focus_entered ok");
    CHECK(f.tap("Tab") == "focus_exited ok, focus_entered name, editing_toggled name on");
    // Editing, Up and Down move the caret, and Tab moves focus on.
    CHECK(f.tap("Down").empty());
    CHECK(f.tap("Tab") == "focus_exited name, editing_toggled name off, focus_entered ok");
}

TEST_CASE("an input method's text shows at the caret until it commits") {
    Fields f;
    f.field("name", {20, 20, 360, 40}, R"("text": "ab")");
    ui::grab_focus(f.world, f.world.find("name"));
    f.step();
    f.tap("End");
    f.tap("Left", kShift);
    CHECK(f.selection("name") == Selection{{1, 2}});
    // Composition replaces the selection and shows at the caret, underlined.
    CHECK(f.send(ime("かな", "[0, 2]")) == R"(text_changed name "a")");
    CHECK(ui::ime_text(f.world, f.world.find("name")) == "かな");
    CHECK(f.text("name") == "a");
    const ui::Layout placed = f.layout();
    CHECK(placed.label(f.world.find("name"))->source == "aかな");
    // No caret while part of it is being converted: a thin underline under the composition and
    // a thick one under that part.
    CHECK(placed.overlays.at(f.world.find("name")).size() == 2);
    // Keys belong to the input method while it composes.
    CHECK(f.tap("Left").empty());
    CHECK(f.caret("name") == 1);
    // It commits: the composition ends and the text comes as typed characters.
    CHECK(f.send({ime(""), typed("仮名")}) == R"(text_changed name "a仮名")");
    CHECK(ui::ime_text(f.world, f.world.find("name")).empty());
    CHECK(f.caret("name") == 3);
    // Focus lost while composing commits what was composed.
    f.send(ime("か"));
    ui::release_focus(f.world);
    CHECK(f.step() ==
          R"(focus_exited name, editing_toggled name off, text_changed name "a仮名か")");
}

TEST_CASE("clicks place the caret; double clicks select words and triple clicks all") {
    Fields f;
    f.field("line", {20, 20, 360, 40}, R"("text": "hello world")");
    // "hello world": a box of 12 pixels a letter, 5 for the space; a click goes to the nearer
    // side of the character it hits.
    const auto at = [](float x) { return 20.0F + kLeft + x; };
    f.send({click(at(2.0F * kBox + 2.0F), 40, true), click(at(2.0F * kBox + 2.0F), 40, false)});
    CHECK(f.caret("line") == 2);
    f.send({click(at(2.0F * kBox + 8.0F), 40, true), click(at(2.0F * kBox + 8.0F), 40, false)});
    CHECK(f.caret("line") == 3);
    // Past the end, the end.
    f.send({click(370, 40, true), click(370, 40, false)});
    CHECK(f.caret("line") == 11);
    // Shift-click selects from the caret.
    f.send({click(at(1.0F), 40, true, kShift), click(at(1.0F), 40, false, kShift)});
    CHECK(f.selection("line") == Selection{{0, 11}});
    // A click drops it; a drag selects. (A press on the selection and a drag would drag its
    // text instead, as test_ui_drag.cpp checks.)
    f.send({click(at(1.0F), 40, true), click(at(1.0F), 40, false)});
    CHECK(f.selection("line") == Selection{});
    f.send(
        {click(at(1.0F), 40, true), drag(at(3.0F * kBox), 40), click(at(3.0F * kBox), 40, false)});
    CHECK(f.selection("line") == Selection{{0, 3}});
    // A double click selects the word, a third click everything.
    const float second = at((5.0F * kBox) + 5.0F + (2.0F * kBox) + 1.0F);
    f.send({click(second, 40, true), click(second, 40, false)});
    f.send({click(second, 40, true, R"("double_click": true)"), click(second, 40, false)});
    CHECK(f.selection("line") == Selection{{6, 11}});
    CHECK(f.caret("line") == 11);
    f.send({click(second, 40, true), click(second, 40, false)});
    CHECK(f.selection("line") == Selection{{0, 11}});
    // Long after a double click, a click is a click.
    f.send({click(second, 40, true, R"("double_click": true)"), click(second, 40, false)});
    for (int i = 0; i < 40; ++i) {
        f.step();
    }
    f.send({click(second, 40, true), click(second, 40, false)});
    CHECK(f.selection("line") == Selection{});
}

TEST_CASE("select_all_on_focus selects when the click is let go; the clear button clears") {
    Fields f;
    f.field("search", {20, 20, 360, 40},
            R"("text": "query", "select_all_on_focus": true, "clear_button_enabled": true)");
    f.send(click(40, 40, true));
    CHECK(f.selection("search") == Selection{});
    f.send(click(40, 40, false));
    CHECK(f.selection("search") == Selection{{0, 5}});
    // The clear button is a 16-pixel icon at the right, inside the margin.
    const float button = 20.0F + 360.0F - 4.0F - 8.0F;
    CHECK(f.send(click(button, 40, true)).empty());
    CHECK(f.send(click(button, 40, false)) == R"(text_changed search "")");
    CHECK(f.text("search").empty());
    CHECK(f.editing("search"));
}

TEST_CASE("long text scrolls to keep the caret in view") {
    Fields f;
    // Room for 10 boxes between the margins.
    f.field("long", {20, 20, 128, 40}, R"("text": "abcdefghijklmnopqrstuvwxyz")");
    ui::grab_focus(f.world, f.world.find("long"));
    f.step();
    ui::LineEditView view = f.view("long");
    CHECK(view.scroll_offset == 0.0F);
    CHECK(view.visible_start == 0);
    CHECK(view.visible_end == 10);
    f.tap("End");
    view = f.view("long");
    CHECK(view.scroll_offset == 120.0F - (26.0F * kBox));
    CHECK(view.visible_start == 16);
    CHECK(view.visible_end == 26);
    CHECK(view.caret_x == kLeft + 120.0F);
    // The caret stays where it is, and the text with it, until it would leave.
    f.tap("Left");
    CHECK(f.view("long").scroll_offset == view.scroll_offset);
    f.tap("Home");
    CHECK(f.view("long").scroll_offset == 0.0F);
}

TEST_CASE("a LineEdit draws its box, focus, selection, text, caret and blinking caret") {
    Fields f;
    f.field("line", {20, 20, 360, 40}, R"("text": "hello", "caret_blink": true)");
    const entt::entity line = f.world.find("line");
    ui::Layout placed = f.layout();
    CHECK(placed.boxes.at(line).size() == 1);
    CHECK_FALSE(placed.overlays.contains(line));
    CHECK(placed.label(line)->font_color == ui::default_theme("LineEdit").colors.at("font_color"));

    ui::grab_focus(f.world, line);
    f.step();
    f.tap("A", kCtrl);
    placed = f.layout();
    // The style box, the focus box and the selection; then the caret over the text.
    CHECK(placed.boxes.at(line).size() == 3);
    CHECK(placed.overlays.at(line).size() == 1);
    const ui::LabelText& label = *placed.label(line);
    CHECK(label.selected_start == 0);
    CHECK(label.selected_end == 5);
    CHECK(placed.label(line)->pens.front() == Vec2{kLeft, 10.0F + 16.0F});
    // The caret blinks every 0.65 seconds, 39 ticks from when editing started.
    for (int i = 0; i < 36; ++i) {
        f.step();
    }
    CHECK(f.layout().overlays.contains(line));
    f.step();
    CHECK_FALSE(f.layout().overlays.contains(line));
    // Moving it shows it again; dropping the selection doesn't.
    f.tap("Right");
    CHECK_FALSE(f.layout().overlays.contains(line));
    f.tap("Left");
    CHECK(f.layout().overlays.contains(line));

    // Empty, it shows its placeholder in its color; read only, its text greyed.
    f.set("line", R"({"LineEdit": {"text": "", "placeholder_text": "Name"}})");
    placed = f.layout();
    CHECK(placed.label(line)->source == "Name");
    CHECK(placed.label(line)->font_color ==
          ui::default_theme("LineEdit").colors.at("font_placeholder_color"));
    f.set("line", R"({"LineEdit": {"text": "x", "editable": false}})");
    placed = f.layout();
    CHECK(placed.label(line)->font_color ==
          ui::default_theme("LineEdit").colors.at("font_uneditable_color"));
}

TEST_CASE("a LineEdit's minimum size is its style, four W's and its icons") {
    Fields f;
    REQUIRE(f.world.create("auto"));
    f.set("auto", R"({"Control": {"theme_override_fonts": {"font": "boxes.ttf"},
                                  "theme_override_font_sizes": {"font_size": 20}},
                      "LineEdit": {}})");
    ui::Layout placed = f.layout();
    CHECK(placed.find(f.world.find("auto"))->size == Vec2{8.0F + (4.0F * kBox), 8.0F + 20.0F});
    f.set("auto", R"({"LineEdit": {"clear_button_enabled": true}})");
    placed = f.layout();
    CHECK(placed.find(f.world.find("auto"))->size ==
          Vec2{8.0F + (4.0F * kBox) + 16.0F, 8.0F + 20.0F});
    f.set("auto", R"({"LineEdit": {"clear_button_enabled": false, "expand_to_text_length": true,
                                   "text": "abcdefgh"}})");
    placed = f.layout();
    CHECK(placed.find(f.world.find("auto"))->size == Vec2{8.0F + (8.0F * kBox) + 1.0F, 28.0F});
}

TEST_CASE("check finds secret characters that are too long and a Label on a LineEdit") {
    Fields f;
    f.field("pin", {20, 20, 100, 40}, R"("secret": true, "secret_character": "**")");
    f.set("pin", R"({"Label": {"text": "x"}})");
    std::vector<std::string> lines;
    for (const ui::Problem& problem : ui::check(f.world, *ui::fonts(f.world))) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    CHECK(lines == std::vector<std::string>{
                       "pin/components/LineEdit/secret_character: a secret character is one "
                       "character; the others are left out",
                       "pin/components/LineEdit: a LineEdit draws its own text and takes its own "
                       "input; put the Label or button in another control",
                   });
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

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }
    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("input.inject types into a LineEdit, and ui.layout and ui.clipboard report it") {
    Session s;
    s.ok("entity.create", R"({"id": "name", "components": {
        "Control": {"offset_left": 20, "offset_top": 20, "offset_right": 320,
                    "offset_bottom": 60},
        "LineEdit": {"placeholder_text": "Your name"}}})");
    CHECK(s.ok("ui.layout", R"({"ids": ["name"]})") ==
          parse(R"({"viewport": [1280, 720], "controls": [
              {"id": "name", "rect": [20, 20, 300, 40],
               "line_edit": {"caret_column": 0, "placeholder": true}, "lines": ["Your name"]}]})"));
    CHECK(s.ok("ui.focus", R"({"id": "name"})") == parse(R"({"focused": "name"})"));
    s.ok("input.inject", R"({"events": [{"type": "text", "text": "Mira"},
                                       {"type": "action", "action": "ui_text_caret_word_left",
                                        "pressed": true},
                                       {"type": "ime", "text": "x"}]})");
    s.ok("step");
    CHECK(s.ok("ui.layout", R"({"ids": ["name"]})") ==
          parse(R"({"viewport": [1280, 720], "controls": [
              {"id": "name", "rect": [20, 20, 300, 40], "focused": true,
               "line_edit": {"editing": true, "caret_column": 0, "ime": "x"},
               "lines": ["xMira"]}]})"));
    CHECK(s.ok("ui.clipboard") == parse(R"({"text": ""})"));
    CHECK(s.ok("ui.clipboard", R"({"text": "Kai"})") == parse(R"({"text": "Kai"})"));
    s.ok("input.inject", R"({"events": [{"type": "ime", "text": ""},
                                       {"type": "action", "action": "ui_paste",
                                        "pressed": true}]})");
    s.ok("step");
    json::Value got = s.ok("entity.get", R"({"id": "name", "components": ["LineEdit"]})");
    json::Value& edit = got["components"]["LineEdit"];
    CHECK(edit["text"] == "KaiMira");
    CHECK(edit["caret_column"] == 3);
}
