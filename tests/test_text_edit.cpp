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

// TextEdit, as Godot's (scene/gui/text_edit.cpp): the events, carets, selections, scrolling and
// undo steps expected here follow it. Text is drawn in a box font at 20 pixels, every character
// 12 pixels wide and a space 5, in rows 24 pixels apart (the font's 20 and line_spacing's 4),
// so where clicks land is easy to work out.

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
// Where text starts in a TextEdit: the default style's 4-pixel margins.
constexpr float kMargin = 4.0F;
constexpr float kBox = 12.0F;
constexpr float kRow = 24.0F;

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
std::string wheel(float x, float y, std::string_view button, std::string_view extra = "") {
    std::string out = R"({"type": "mouse_button", "button": ")" + std::string(button) +
                      R"(", "position": [)" + number(x) + ", " + number(y) + "], \"pressed\": true";
    if (!extra.empty()) {
        out += ", " + std::string(extra);
    }
    return out + "}";
}
// Mouse motion; the buttons held come from the input state.
std::string drag(float x, float y) {
    return R"({"type": "mouse_motion", "position": [)" + number(x) + ", " + number(y) + "]}";
}
std::string touch(float x, float y, bool pressed, std::string_view extra = "") {
    std::string out = R"({"type": "screen_touch", "index": 0, "position": [)" + number(x) + ", " +
                      number(y) + "], \"pressed\": " + (pressed ? "true" : "false");
    if (!extra.empty()) {
        out += ", " + std::string(extra);
    }
    return out + "}";
}
std::string touch_drag(float x, float y, float dx, float dy) {
    return R"({"type": "screen_drag", "index": 0, "position": [)" + number(x) + ", " + number(y) +
           "], \"relative\": [" + number(dx) + ", " + number(dy) + "]}";
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

using Caret = std::pair<int, int>; // line, column
using Selection = std::optional<std::array<int, 4>>;

// A world with the built-in components, ui and input with a PC keyboard's shortcuts, in a 400
// x 300 viewport, and a box font.
struct Editors {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Editors() {
        files.files["boxes.ttf"] = bytes_of(text::box_font(
            U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,-_!?かな仮名"));
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

    // A TextEdit at [x, y, w, h] drawn in the box font at 20 pixels.
    entt::entity editor(std::string_view id, std::array<int, 4> rect,
                        std::string_view text_edit_fields = "") {
        const entt::entity entity = world.create(id).value();
        set(id, "{\"Control\": {\"offset_left\": " + std::to_string(rect[0]) +
                    ", \"offset_top\": " + std::to_string(rect[1]) +
                    ", \"offset_right\": " + std::to_string(rect[0] + rect[2]) +
                    ", \"offset_bottom\": " + std::to_string(rect[1] + rect[3]) +
                    R"(, "theme_override_fonts": {"font": "boxes.ttf"},)" +
                    R"( "theme_override_font_sizes": {"font_size": 20}}, "TextEdit": {)" +
                    std::string(text_edit_fields) + "}}");
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
    // A click and its release where a line and column of a TextEdit at (20, 20) are.
    std::string click_at(int line, int column, std::string_view extra = "") {
        const float x = 20.0F + kMargin + (static_cast<float>(column) * kBox) + 2.0F;
        const float y = 20.0F + kMargin + (static_cast<float>(line) * kRow) + (kRow / 2.0F);
        return send({click(x, y, true, extra), click(x, y, false)});
    }

    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "lines_edited_from") {
                out += " " + std::to_string(event.from_line) + "-" + std::to_string(event.to_line);
            }
        }
        return out;
    }

    [[nodiscard]] const world::TextEdit& edit(std::string_view id) const {
        return world.ecs().get<world::TextEdit>(world.find(id));
    }
    [[nodiscard]] std::string text(std::string_view id) const { return edit(id).text; }
    [[nodiscard]] Caret caret(std::string_view id) const {
        return {edit(id).caret_line, edit(id).caret_column};
    }

    ui::Layout layout() {
        return ui::layout(world, *ui::fonts(world), ui::settings(world).viewport);
    }
    ui::TextEditView view(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.text_edits.find(world.find(id));
        REQUIRE(found != placed.text_edits.end());
        return found->second;
    }
    Selection selection(std::string_view id) { return view(id).selection; }
    // The rows a TextEdit shows, as ui.layout's lines.
    std::vector<std::string> rows(std::string_view id) {
        const ui::Layout placed = layout();
        const ui::LabelText* label = placed.label(world.find(id));
        REQUIRE(label != nullptr);
        std::vector<std::string> out;
        for (std::size_t l = label->first; l < label->last; ++l) {
            const text::Line& line = label->text.lines[l];
            out.push_back(label->source.substr(line.start, line.end - line.start));
        }
        return out;
    }
};

Selection range(int from_line, int from_column, int to_line, int to_column) {
    return std::array<int, 4>{from_line, from_column, to_line, to_column};
}

} // namespace

TEST_CASE("a click puts the caret in a TextEdit; typing and Enter edit its lines") {
    Editors f;
    f.editor("notes", {20, 20, 300, 120}, R"("placeholder_text": "Notes")");
    std::vector<world::Event> edited;
    f.world.set_event_listener([&](const world::Event& event) {
        if (event.kind == "lines_edited_from") {
            edited.push_back(event);
        }
    });
    CHECK(f.view("notes").placeholder);
    CHECK(f.rows("notes") == std::vector<std::string>{"Notes"});
    CHECK(f.send(click(40, 40, true)) == "mouse_entered notes, focus_entered notes");
    CHECK(f.send(click(40, 40, false)).empty());
    // The platform is asked for text input and the on-screen keyboard for several lines.
    const input::TextInput request = ui::text_input(f.world);
    CHECK(request.active);
    CHECK(request.show_keyboard);
    CHECK(request.keyboard == "multiline");
    CHECK(request.size.x == 300.0F);
    CHECK(request.caret == static_cast<int>(kMargin));

    // Each insertion edits lines; text_changed and caret_changed come once, at the end of the
    // tick, as Godot defers them.
    CHECK(f.send(typed("Hi")) == "lines_edited_from notes 0-0, lines_edited_from notes 0-0, "
                                 "text_changed notes, caret_changed notes");
    CHECK(f.text("notes") == "Hi");
    CHECK(f.caret("notes") == Caret{0, 2});
    CHECK_FALSE(f.view("notes").placeholder);
    CHECK(f.tap("Enter") == "lines_edited_from notes 0-1, text_changed notes, caret_changed notes");
    CHECK(f.text("notes") == "Hi\n");
    CHECK(f.caret("notes") == Caret{1, 0});
    f.send(typed("there"));
    CHECK(f.text("notes") == "Hi\nthere");
    CHECK(f.rows("notes") == std::vector<std::string>{"Hi", "there"});
    // The caret draws on its row: the second, 5 characters in.
    const ui::TextEditView view = f.view("notes");
    CHECK(view.caret_visible);
    CHECK(view.caret_position.x == kMargin + (5.0F * kBox));
    CHECK(view.caret_line == 1);
    CHECK(view.caret_column == 5);
    // A key with no character and no action does nothing.
    CHECK(f.tap("F5").empty());

    // Gameplay code gets the same events, lines_edited_from with its lines.
    REQUIRE(edited.size() == 8);
    CHECK(edited[2].id == "notes");
    CHECK(edited[2].data == parse(R"({"from_line": 0, "to_line": 1})"));

    // Focus gone, it asks for no text input.
    ui::release_focus(f.world);
    CHECK(f.step() == "focus_exited notes");
    CHECK_FALSE(ui::text_input(f.world).active);
}

TEST_CASE("arrow keys move between lines and keep the caret's x; Home and End go to the ends") {
    Editors f;
    f.editor("notes", {20, 20, 360, 200}, R"("text": "hello big world\nhi\nlonger line here")");
    ui::grab_focus(f.world, f.world.find("notes"));
    CHECK(f.step() == "focus_entered notes");
    CHECK(f.caret("notes") == Caret{0, 0});
    CHECK(f.tap("End") == "caret_changed notes");
    CHECK(f.caret("notes") == Caret{0, 15});
    // Down to a shorter line goes to its end; on to a longer one, back to the same x.
    f.tap("Down");
    CHECK(f.caret("notes") == Caret{1, 2});
    f.tap("Down");
    CHECK(f.caret("notes") == Caret{2, 15});
    f.tap("Home", kCtrl);
    CHECK(f.caret("notes") == Caret{0, 0});
    f.tap("End", kCtrl);
    CHECK(f.caret("notes") == Caret{2, 16});
    // Left at a line's start goes to the end of the line before.
    f.tap("Home");
    f.tap("Left");
    CHECK(f.caret("notes") == Caret{1, 2});
    f.tap("Right");
    CHECK(f.caret("notes") == Caret{2, 0});
    // Words.
    f.tap("Right", kCtrl);
    CHECK(f.caret("notes") == Caret{2, 6});
    f.tap("Right", kCtrl);
    CHECK(f.caret("notes") == Caret{2, 11});

    // Shift selects across lines; typing or Backspace replaces the selection.
    f.tap("End");
    f.tap("Up", kShift);
    CHECK(f.selection("notes") == range(1, 2, 2, 16));
    CHECK(f.caret("notes") == Caret{1, 2});
    // Removed lines are reported as Godot's are, from the last to the first.
    CHECK(f.tap("Backspace") == "lines_edited_from notes 2-1, text_changed notes");
    CHECK(f.text("notes") == "hello big world\nhi");
    CHECK(f.selection("notes") == Selection{});
    // Backspace at a line's start joins it to the line before, Delete at its end the next.
    f.tap("Home");
    f.tap("Backspace");
    CHECK(f.text("notes") == "hello big worldhi");
    CHECK(f.caret("notes") == Caret{0, 15});
    f.tap("Backspace", kCtrl);
    CHECK(f.text("notes") == "hello big hi");
    f.tap("Delete", kCtrl);
    CHECK(f.text("notes") == "hello big ");
    f.tap("Home");
    f.tap("Delete");
    CHECK(f.text("notes") == "ello big ");
    // Escape drops the selection; Ctrl+A selects everything.
    f.tap("A", kCtrl);
    CHECK(f.selection("notes") == range(0, 0, 0, 9));
    f.tap("Escape");
    CHECK(f.selection("notes") == Selection{});
}

TEST_CASE("Enter keeps the line's rest on a new line; Ctrl+Enter and Ctrl+Shift+Enter don't") {
    Editors f;
    f.editor("notes", {20, 20, 360, 200}, R"("text": "onetwo")");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.click_at(0, 3);
    CHECK(f.caret("notes") == Caret{0, 3});
    f.tap("Enter");
    CHECK(f.text("notes") == "one\ntwo");
    CHECK(f.caret("notes") == Caret{1, 0});
    // A blank line after the caret's line, leaving it as it is.
    f.tap("Enter", kCtrl);
    CHECK(f.text("notes") == "one\ntwo\n");
    CHECK(f.caret("notes") == Caret{2, 0});
    // And one above it.
    f.tap("Enter", kCtrlShift);
    CHECK(f.text("notes") == "one\ntwo\n\n");
    CHECK(f.caret("notes") == Caret{2, 0});
}

TEST_CASE("undo and redo step through typing, which merges until the typist pauses") {
    Editors f;
    f.editor("notes", {20, 20, 360, 200});
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.send(typed("ab"));
    f.send(typed("cd"));
    // Typing without a pause is one step, as Godot's: an insertion a character, chained.
    CHECK(f.tap("Z", kCtrl) ==
          "lines_edited_from notes 0-0, lines_edited_from notes 0-0, lines_edited_from notes 0-0, "
          "lines_edited_from notes 0-0, text_changed notes, caret_changed notes");
    CHECK(f.text("notes").empty());
    CHECK(f.tap("Z", kCtrl).empty());
    f.tap("Y", kCtrl);
    CHECK(f.text("notes") == "abcd");
    CHECK(f.caret("notes") == Caret{0, 4});
    // Three seconds without typing end the step.
    f.send(typed("x"));
    for (int i = 0; i < 200; ++i) {
        f.step();
    }
    f.send(typed("y"));
    f.tap("Z", kCtrl);
    CHECK(f.text("notes") == "abcdx");
    f.tap("Z", kCtrl);
    CHECK(f.text("notes") == "abcd");
    f.tap("Z", kCtrlShift);
    CHECK(f.text("notes") == "abcdx");
    // Typing drops what could be redone.
    f.send(typed("!"));
    CHECK(f.tap("Y", kCtrl).empty());
    CHECK(f.text("notes") == "abcdx!");

    // Text set from outside is set as set_text sets it: one step that undoes to what it was.
    f.set("notes", R"({"TextEdit": {"text": "fresh\ntext", "caret_line": 1, "caret_column": 4}})");
    // As set_text, it emits text_set, not text_changed.
    CHECK(f.step() == "lines_edited_from notes 0-0, lines_edited_from notes 0-1, text_set notes, "
                      "caret_changed notes");
    CHECK(f.caret("notes") == Caret{1, 4});
    f.tap("Z", kCtrl);
    CHECK(f.text("notes") == "abcdx!");
    f.tap("Y", kCtrl);
    CHECK(f.text("notes") == "fresh\ntext");
}

TEST_CASE("copy and cut take the caret's line when nothing is selected; paste puts it back") {
    Editors f;
    f.editor("notes", {20, 20, 360, 200}, R"("text": "one\ntwo")");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.click_at(1, 1);
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "two\n");
    // A copied line goes in above the caret's, wherever the caret is in it.
    f.click_at(0, 2);
    f.tap("V", kCtrl);
    CHECK(f.text("notes") == "two\none\ntwo");
    CHECK(f.caret("notes") == Caret{1, 2});
    f.tap("X", kCtrl);
    CHECK(f.text("notes") == "two\ntwo");
    CHECK(ui::clipboard(f.world) == "one\n");
    // A selection copies as it is, line breaks and all, and pastes at the caret.
    f.tap("Home", kCtrl);
    f.tap("End", kCtrlShift);
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "two\ntwo");
    f.tap("Right");
    f.tap("V", kCtrl);
    CHECK(f.text("notes") == "two\ntwotwo\ntwo");
    CHECK(f.caret("notes") == Caret{2, 3});
    // Without empty_selection_clipboard_enabled, nothing selected copies nothing.
    f.set("notes", R"({"TextEdit": {"empty_selection_clipboard_enabled": false}})");
    ui::set_clipboard(f.world, "kept");
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "kept");
}

TEST_CASE("clicks place the caret; double clicks select words, triple clicks lines, drags") {
    Editors f;
    f.editor("notes", {20, 20, 360, 200}, R"("text": "hello world\nsecond line\nthird")");
    f.click_at(1, 3);
    CHECK(f.caret("notes") == Caret{1, 3});
    // Past a line's end, its end; below the text, the last line.
    f.send({click(370, 50, true), click(370, 50, false)});
    CHECK(f.caret("notes") == Caret{1, 11});
    f.send({click(100, 200, true), click(100, 200, false)});
    CHECK(f.caret("notes").first == 2);
    // Shift-click selects from the caret.
    f.click_at(0, 2, kShift);
    CHECK(f.selection("notes") == range(0, 2, 2, 5));
    // A drag selects, across lines.
    const float x0 = 20.0F + kMargin + 2.0F;
    f.send(
        {click(x0, 36, true), drag(x0 + (4.0F * kBox), 60), click(x0 + (4.0F * kBox), 60, false)});
    CHECK(f.selection("notes") == range(0, 0, 1, 4));
    // A double click selects the word, a third click the line.
    f.click_at(1, 9);
    f.click_at(1, 9, R"("double_click": true)");
    CHECK(f.selection("notes") == range(1, 7, 1, 11));
    CHECK(f.caret("notes") == Caret{1, 11});
    f.click_at(1, 9);
    CHECK(f.selection("notes") == range(1, 0, 2, 0));
    // Long after a double click, a click is a click.
    f.click_at(1, 9, R"("double_click": true)");
    for (int i = 0; i < 40; ++i) {
        f.step();
    }
    f.click_at(1, 9);
    CHECK(f.selection("notes") == Selection{});
    // With selecting_enabled off, a drag only moves the caret.
    f.set("notes", R"({"TextEdit": {"selecting_enabled": false}})");
    f.send(
        {click(x0, 36, true), drag(x0 + (4.0F * kBox), 60), click(x0 + (4.0F * kBox), 60, false)});
    CHECK(f.selection("notes") == Selection{});
}

TEST_CASE("the wheel scrolls, the caret stays in view and a scroll bar shows when lines overflow") {
    Editors f;
    // Room for four rows: 8 pixels of margins and 4 rows of 24.
    std::string lines;
    for (int i = 0; i < 10; ++i) {
        lines += (i == 0 ? "" : "\\n") + std::string("line ") + std::to_string(i);
    }
    f.editor("notes", {20, 20, 300, 104}, "\"text\": \"" + lines + "\"");
    ui::TextEditView view = f.view("notes");
    CHECK(view.v_scroll_visible);
    CHECK_FALSE(view.h_scroll_visible);
    CHECK(view.total_rows == 10);
    CHECK(view.first_visible_line == 0);
    CHECK(view.last_visible_line == 3);
    CHECK(f.rows("notes") == std::vector<std::string>{"line 0", "line 1", "line 2", "line 3"});
    // Three lines a notch.
    f.send(wheel(100, 60, "wheel_down"));
    CHECK(f.edit("notes").scroll_vertical == 3.0);
    CHECK(f.view("notes").first_visible_line == 3);
    f.send(wheel(100, 60, "wheel_up"));
    CHECK(f.edit("notes").scroll_vertical == 0.0);
    // It goes no further than the last line at the bottom.
    for (int i = 0; i < 5; ++i) {
        f.send(wheel(100, 60, "wheel_down"));
    }
    CHECK(f.edit("notes").scroll_vertical == 6.0);
    // Ctrl+Up and Ctrl+Down scroll a line, moving the caret only to keep it in view.
    ui::grab_focus(f.world, f.world.find("notes"));
    f.tap("Home", kCtrl);
    CHECK(f.edit("notes").scroll_vertical == 0.0);
    f.tap("Down", kCtrl);
    CHECK(f.edit("notes").scroll_vertical == 1.0);
    CHECK(f.caret("notes") == Caret{1, 0});
    // The caret moved down past the bottom scrolls it into view.
    f.tap("End", kCtrl);
    view = f.view("notes");
    CHECK(f.caret("notes") == Caret{9, 6});
    CHECK(view.last_visible_line == 9);
    CHECK(f.edit("notes").scroll_vertical == 6.0);
    // Page Up moves the caret a page up, to the top row, and the view with it.
    f.tap("PageUp");
    CHECK(f.caret("notes").first == 6);
    // A scroll set from outside is kept, within what the text allows.
    f.set("notes", R"({"TextEdit": {"scroll_vertical": 2}})");
    CHECK(f.view("notes").first_visible_line == 2);
    // scroll_past_end_of_file lets the last line go up to the top.
    f.set("notes", R"({"TextEdit": {"scroll_past_end_of_file": true}})");
    for (int i = 0; i < 5; ++i) {
        f.send(wheel(100, 60, "wheel_down"));
    }
    CHECK(f.edit("notes").scroll_vertical == 9.0);
}

TEST_CASE("wrap_mode boundary wraps lines at words, and Up and Down move by rows") {
    Editors f;
    // 200 pixels: 8 of margins and 10 of wrap_offset leave 182, 15 characters.
    f.editor("notes", {20, 20, 200, 200},
             R"("text": "one two three four five six\nend", "wrap_mode": "boundary")");
    ui::TextEditView view = f.view("notes");
    CHECK(view.total_rows == 3);
    CHECK(f.rows("notes") == std::vector<std::string>{"one two three ", "four five six", "end"});
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.tap("Down");
    CHECK(f.caret("notes") == Caret{0, 14});
    f.tap("Down");
    CHECK(f.caret("notes") == Caret{1, 0});
    f.tap("Up");
    f.tap("Up");
    CHECK(f.caret("notes") == Caret{0, 0});
    // End goes to the end of the row, before its last character as Godot's, then of the line.
    f.tap("End");
    CHECK(f.caret("notes") == Caret{0, 13});
    f.tap("End");
    CHECK(f.caret("notes") == Caret{0, 27});
    // A click on the second row.
    f.click_at(1, 2);
    CHECK(f.caret("notes") == Caret{0, 16});
    // ui.layout counts rows, not lines.
    CHECK(f.view("notes").total_rows == 3);
}

TEST_CASE("Tab types a tab while tab_input_mode is on; ui_focus_mode turns it off") {
    Editors f;
    f.editor("notes", {20, 20, 360, 100});
    f.editor("other", {20, 140, 360, 100});
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.tap("Tab");
    CHECK(f.text("notes") == "\t");
    CHECK(ui::focus_owner(f.world) == f.world.find("notes"));
    // A tab is as wide as four spaces, from the line's start.
    f.send(typed("x"));
    CHECK(f.view("notes").caret_position.x == kMargin + (4.0F * 5.0F) + kBox);
    // Ctrl+M toggles it, as Godot's ui_focus_mode, and Tab then moves focus.
    f.tap("M", kCtrl);
    CHECK_FALSE(f.edit("notes").tab_input_mode);
    f.tap("Tab");
    CHECK(f.text("notes") == "\tx");
    CHECK(ui::focus_owner(f.world) == f.world.find("other"));
}

TEST_CASE("an input method's composition shows at the caret until it commits") {
    Editors f;
    f.editor("notes", {20, 20, 360, 100}, R"("text": "ab\ncd")");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.click_at(1, 1);
    // The line shows the composition, as an edit of it.
    CHECK(f.send(ime("かな", "[1, 0]")) == "lines_edited_from notes 1-1");
    ui::TextEditView view = f.view("notes");
    CHECK(view.ime == "かな");
    CHECK(f.rows("notes") == std::vector<std::string>{"ab", "cかなd"});
    CHECK(f.text("notes") == "ab\ncd");
    // The candidate window goes at the composition's start.
    CHECK(ui::text_input(f.world).caret == static_cast<int>(kMargin + kBox));
    // Committed text replaces the composition.
    CHECK(f.send({ime(""), typed("仮名")}) ==
          "lines_edited_from notes 1-1, lines_edited_from notes 1-1, lines_edited_from notes 1-1, "
          "text_changed notes, caret_changed notes");
    CHECK(f.text("notes") == "ab\nc仮名d");
    CHECK(f.caret("notes") == Caret{1, 3});
    CHECK(f.view("notes").ime.empty());
}

TEST_CASE("a TextEdit that isn't editable moves its caret and copies, but doesn't change") {
    Editors f;
    f.editor("notes", {20, 20, 360, 100}, R"("text": "fixed\ntext", "editable": false)");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    CHECK_FALSE(ui::text_input(f.world).active);
    f.send(typed("x"));
    f.tap("Enter");
    f.tap("Delete");
    CHECK(f.text("notes") == "fixed\ntext");
    f.tap("Down");
    f.tap("End", kShift);
    CHECK(f.selection("notes") == range(1, 0, 1, 4));
    f.tap("C", kCtrl);
    CHECK(ui::clipboard(f.world) == "text");
    f.tap("X", kCtrl);
    CHECK(f.text("notes") == "fixed\ntext");
    // It draws no caret.
    CHECK_FALSE(f.layout().overlays.contains(f.world.find("notes")));
}

TEST_CASE("a tap puts the caret where the finger lifts, and a drag scrolls") {
    Editors f;
    std::string lines;
    for (int i = 0; i < 10; ++i) {
        lines += (i == 0 ? "" : "\\n") + std::string("line ") + std::to_string(i);
    }
    f.editor("notes", {20, 20, 300, 104}, "\"text\": \"" + lines + "\"");
    const float x = 20.0F + kMargin + (2.0F * kBox) + 2.0F;
    const float y = 20.0F + kMargin + kRow + (kRow / 2.0F);
    f.send(touch(x, y, true));
    CHECK(f.caret("notes") == Caret{0, 0});
    f.send(touch(x, y, false));
    CHECK(f.caret("notes") == Caret{1, 2});
    CHECK(ui::focus_owner(f.world) == f.world.find("notes"));
    // A finger dragged up two rows scrolls two lines down, and doesn't move the caret.
    f.send(touch(x, y, true));
    f.send(touch_drag(x, y - kRow, 0.0F, -kRow));
    f.send(touch_drag(x, y - (2.0F * kRow), 0.0F, -kRow));
    f.send(touch(x, y - (2.0F * kRow), false));
    CHECK(f.edit("notes").scroll_vertical == 2.0);
    CHECK(f.caret("notes") == Caret{1, 2});
}

TEST_CASE("Insert types over characters; caret_blink blinks the caret") {
    Editors f;
    f.editor("notes", {20, 20, 360, 100}, R"("text": "abc", "caret_blink": true)");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.tap("Insert");
    f.send(typed("XY"));
    CHECK(f.text("notes") == "XYc");
    f.tap("Insert");
    f.send(typed("Z"));
    CHECK(f.text("notes") == "XYZc");
    // The caret shows, then hides after caret_blink_interval, then shows again.
    const auto overlays = [&] {
        const ui::Layout placed = f.layout();
        const auto found = placed.overlays.find(f.world.find("notes"));
        return found != placed.overlays.end() ? found->second.size() : std::size_t{0};
    };
    CHECK(overlays() == 1);
    for (int i = 0; i < 40; ++i) {
        f.step();
    }
    CHECK(overlays() == 0);
    for (int i = 0; i < 40; ++i) {
        f.step();
    }
    CHECK(overlays() == 1);
    // Typing shows it again at once.
    for (int i = 0; i < 40; ++i) {
        f.step();
    }
    f.send(typed("!"));
    CHECK(overlays() == 1);
}

TEST_CASE("a TextEdit draws its box, current line, selection, text, caret and scroll bar") {
    Editors f;
    std::string lines;
    for (int i = 0; i < 10; ++i) {
        lines += (i == 0 ? "" : "\\n") + std::string("line ") + std::to_string(i);
    }
    f.editor("notes", {20, 20, 300, 104},
             "\"text\": \"" + lines + "\", \"highlight_current_line\": true");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.tap("Right", kShift);
    const ui::Layout placed = f.layout();
    const entt::entity entity = f.world.find("notes");
    // Under the text: its style box, the focus box, the current line and the selection.
    const std::vector<ui::Box>& under = placed.boxes.at(entity);
    REQUIRE(under.size() == 4);
    CHECK(under[0].size == Vec2{300.0F, 104.0F});
    CHECK(under[1].style.border_width[0] == 2);
    CHECK(under[2].position == Vec2{0.0F, kMargin});
    CHECK(under[2].size.y == kRow);
    CHECK(under[2].style.bg_color == slash::Vec4{0.25F, 0.25F, 0.26F, 0.8F});
    CHECK(under[3].position == Vec2{kMargin, kMargin});
    CHECK(under[3].size == Vec2{kBox, kRow});
    CHECK(under[3].style.bg_color == slash::Vec4{0.5F, 0.5F, 0.5F, 1.0F});
    // The rows in view, clipped to the rect.
    const ui::LabelText* label = placed.label(entity);
    REQUIRE(label != nullptr);
    CHECK(label->clip);
    CHECK(label->text.lines.size() == 4);
    CHECK(label->pens[1].y - label->pens[0].y == kRow);
    // Over it: the caret, then the vertical scroll bar and its grabber at the right.
    const std::vector<ui::Box>& over = placed.overlays.at(entity);
    REQUIRE(over.size() == 3);
    CHECK(over[0].position.x == kMargin + kBox);
    // It ends both margins above the bottom, as Godot's _update_scrollbars places it.
    CHECK(over[1].position == Vec2{292.0F, kMargin});
    CHECK(over[1].size == Vec2{8.0F, 92.0F});
    CHECK(over[2].position.y == kMargin);
    CHECK(over[2].size.y > 8.0F);
    CHECK(over[2].size.y < 92.0F);
}

TEST_CASE("a TextEdit's minimum size is its style, or its text with scroll_fit_content") {
    Editors f;
    f.editor("notes", {20, 20, 0, 0}, R"("text": "abc\nde")");
    const auto minimum = [&] { return f.layout().find(f.world.find("notes"))->minimum_size; };
    CHECK(minimum() == Vec2{8.0F, 8.0F});
    f.set("notes", R"({"TextEdit": {"scroll_fit_content_height": true,
                                    "scroll_fit_content_width": true}})");
    // The widest line and 10 pixels, as Godot's.
    const Vec2 fit = minimum();
    CHECK(fit.y == 8.0F + (2.0F * kRow));
    CHECK(fit.x == 8.0F + (3.0F * kBox) + 10.0F);
    // It grows with its text.
    f.set("notes", R"({"TextEdit": {"text": "abc\nde\nf"}})");
    CHECK(minimum().y == 8.0F + (3.0F * kRow));
}

TEST_CASE("text and carets set from outside show at once and the history keeps up") {
    Editors f;
    f.editor("notes", {20, 20, 360, 100}, R"("text": "one")");
    ui::grab_focus(f.world, f.world.find("notes"));
    f.step();
    f.tap("End", kShift);
    CHECK(f.selection("notes") == range(0, 0, 0, 3));
    // A caret moved from outside drops the selection.
    f.set("notes", R"({"TextEdit": {"caret_column": 1}})");
    CHECK(f.view("notes").caret_column == 1);
    f.step();
    CHECK(f.selection("notes") == Selection{});
    // A caret past the end is drawn at the end, and moved there when the TextEdit next runs.
    f.set("notes", R"({"TextEdit": {"caret_line": 5, "caret_column": 9}})");
    CHECK(f.view("notes").caret_line == 0);
    CHECK(f.view("notes").caret_column == 3);
    f.step();
    CHECK(f.caret("notes") == Caret{0, 3});
}

TEST_CASE("check finds a Label on a TextEdit and a language that isn't one") {
    Editors f;
    f.editor("notes", {20, 20, 100, 40}, R"("language": "not a tag")");
    f.set("notes", R"({"Label": {"text": "x"}})");
    std::vector<std::string> lines;
    for (const ui::Problem& problem : ui::check(f.world, *ui::fonts(f.world))) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    CHECK(lines == std::vector<std::string>{
                       "notes/components/TextEdit/language: \"not a tag\" isn't a BCP 47 "
                       "language tag, such as \"ar\" or \"zh-Hant\"",
                       "notes/components/TextEdit: a TextEdit draws its own text and takes its "
                       "own input; put the Label, button or LineEdit in another control",
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

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("input.inject types into a TextEdit, and ui.layout reports its caret and lines") {
    Session s;
    s.ok("entity.create", R"({"id": "notes", "components": {
        "Control": {"offset_left": 20, "offset_top": 20, "offset_right": 320,
                    "offset_bottom": 120},
        "TextEdit": {"placeholder_text": "Notes"}}})");
    CHECK(s.ok("ui.layout", R"({"ids": ["notes"]})") ==
          parse(R"({"viewport": [1280, 720], "controls": [
              {"id": "notes", "rect": [20, 20, 300, 100],
               "text_edit": {"caret": [0, 0], "placeholder": true}, "lines": ["Notes"]}]})"));
    s.ok("ui.focus", R"({"id": "notes"})");
    // A word left with Shift selects it: Ctrl+Shift+Left on a PC, Option+Shift+Left on a Mac,
    // as the session binds the host's keyboard.
    const std::string word = input::host_keyboard() == input::Keyboard::Mac ? "alt" : "ctrl";
    s.ok("input.inject", R"({"events": [{"type": "text", "text": "Dear Mira,"},
                                       {"type": "action", "action": "ui_text_newline",
                                        "pressed": true},
                                       {"type": "text", "text": "See you"},
                                       {"type": "key", "key": "Left", ")" +
                             word + R"(": true, "shift": true, "pressed": true}]})");
    s.ok("step");
    CHECK(s.ok("ui.layout", R"({"ids": ["notes"]})") ==
          parse(R"({"viewport": [1280, 720], "controls": [
              {"id": "notes", "rect": [20, 20, 300, 100], "focused": true,
               "text_edit": {"caret": [1, 4], "selection": [[1, 4], [1, 7]]},
               "lines": ["Dear Mira,", "See you"]}]})"));
    json::Value got = s.ok("entity.get", R"({"id": "notes", "components": ["TextEdit"]})");
    json::Value& edit = got["components"]["TextEdit"];
    CHECK(edit["text"] == "Dear Mira,\nSee you");
    CHECK(edit["caret_line"] == 1);
    CHECK(edit["caret_column"] == 4);
}
