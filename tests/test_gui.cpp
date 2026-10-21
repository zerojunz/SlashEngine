#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/render/image.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Buttons, focus and hover. The gui system sends input events to controls as Godot's Viewport
// does (scene/main/viewport.cpp), and buttons answer them as Godot's BaseButton
// (scene/gui/base_button.cpp); the events expected here follow those.

namespace asset = slash::asset;
namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace render = slash::render;
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

// A PNG of one size.
std::string png(std::uint32_t width, std::uint32_t height) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.pixels.insert(image.pixels.end(), {255, 255, 255, 255});
    }
    auto encoded = render::encode_png(image);
    REQUIRE(encoded.ok());
    return encoded.value();
}

std::string number(float value) {
    std::string out = std::to_string(static_cast<int>(value));
    return out;
}

// Mouse and touch events at a point, as input.inject takes them.
std::string motion(float x, float y) {
    return R"({"type": "mouse_motion", "position": [)" + number(x) + ", " + number(y) + "]}";
}
std::string click(float x, float y, bool pressed, std::string_view button = "left") {
    return R"({"type": "mouse_button", "button": ")" + std::string(button) + R"(", "position": [)" +
           number(x) + ", " + number(y) + "], \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string touch(float x, float y, bool pressed) {
    return R"({"type": "screen_touch", "position": [)" + number(x) + ", " + number(y) +
           "], \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string key(std::string_view name, bool pressed, std::string_view modifiers = "") {
    std::string out = R"({"type": "key", "key": ")" + std::string(name) +
                      "\", \"pressed\": " + (pressed ? "true" : "false");
    if (!modifiers.empty()) {
        out += ", " + std::string(modifiers);
    }
    return out + "}";
}

// A world with the built-in components, ui and input, in a 400 x 300 viewport.
struct Gui {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> emitted;

    explicit Gui(input::Settings settings = {}) {
        world::register_builtin_components(components);
        ui::install(world, assets, {.viewport = {400.0F, 300.0F}, .theme = ""});
        input::install(world, input::Map::with_defaults(), settings);
        world.set_event_listener([this](const world::Event& event) {
            if (!event.kind.starts_with("entity.")) {
                emitted.push_back(event);
            }
        });
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

    // A button at [x, y, w, h] in the viewport, or in its parent's rect.
    entt::entity button(std::string_view id, std::array<int, 4> rect,
                        std::string_view button_fields = "", std::string_view control_fields = "") {
        std::string control = "{\"offset_left\": " + std::to_string(rect[0]) +
                              ", \"offset_top\": " + std::to_string(rect[1]) +
                              ", \"offset_right\": " + std::to_string(rect[0] + rect[2]) +
                              ", \"offset_bottom\": " + std::to_string(rect[1] + rect[3]);
        if (!control_fields.empty()) {
            control += ", " + std::string(control_fields);
        }
        return create(id, "{\"Control\": " + control + "}, \"Button\": {" +
                              std::string(button_fields) + "}}");
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
    std::string tap(std::string_view name, std::string_view modifiers = "") {
        return send({key(name, true, modifiers), key(name, false, modifiers)});
    }

    std::string step() {
        world.step(kTick);
        return events();
    }

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
        }
        return out;
    }

    [[nodiscard]] std::string mode(std::string_view id) const {
        return std::string(ui::draw_mode_name(ui::draw_mode(world, world.find(id))));
    }
    [[nodiscard]] std::string focus() const {
        const entt::entity owner = ui::focus_owner(world);
        return owner != entt::null ? std::string(world.id_of(owner)) : std::string();
    }
    [[nodiscard]] std::string hovered() const {
        const entt::entity over = ui::hovered(world);
        return over != entt::null ? std::string(world.id_of(over)) : std::string();
    }
    [[nodiscard]] bool pressed(std::string_view id) const {
        return world.ecs().get<world::Button>(world.find(id)).button_pressed;
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    // The textures and style box colors a control draws, in order: a texture's path, or
    // "flat" for a flat style box.
    std::string boxes(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.boxes.find(world.find(id));
        std::string out;
        if (found == placed.boxes.end()) {
            return out;
        }
        for (const ui::Box& box : found->second) {
            if (!out.empty()) {
                out += ", ";
            }
            out += box.style.type == ui::StyleBox::Type::Texture ? box.style.texture : "flat";
        }
        return out;
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

} // namespace

TEST_CASE("a click released over a button presses it, and the pointer hovers it") {
    Gui gui;
    gui.button("ok", {20, 20, 100, 40}, R"("text": "OK")");
    CHECK(gui.mode("ok") == "normal");
    CHECK(gui.send(motion(50, 40)) == "mouse_entered ok");
    CHECK(gui.hovered() == "ok");
    CHECK(gui.mode("ok") == "hover");

    // A left click gives it focus, then presses it down.
    CHECK(gui.send(click(50, 40, true)) == "focus_entered ok, button_down ok");
    CHECK(gui.focus() == "ok");
    CHECK(gui.mode("ok") == "pressed");
    CHECK(gui.send(click(50, 40, false)) == "pressed ok, button_up ok");
    CHECK(ui::was_pressed(gui.world, gui.world.find("ok")));
    CHECK(gui.mode("ok") == "hover");
    // Each UI event is a world event too.
    REQUIRE(gui.emitted.size() == 5);
    CHECK(gui.emitted[3].kind == "pressed");
    CHECK(gui.emitted[3].id == "ok");
    CHECK(gui.step().empty());
    CHECK_FALSE(ui::was_pressed(gui.world, gui.world.find("ok")));

    // Let go outside, it isn't pressed.
    CHECK(gui.send(click(50, 40, true)) == "button_down ok");
    CHECK(gui.send(motion(300, 200)) == "mouse_exited ok");
    CHECK(gui.mode("ok") == "normal");
    CHECK(gui.send(click(300, 200, false)) == "button_up ok");

    // Dragged out and back in, it is.
    CHECK(gui.send({motion(50, 40), click(50, 40, true)}) == "mouse_entered ok, button_down ok");
    CHECK(gui.send(motion(300, 200)) == "mouse_exited ok");
    CHECK(gui.send(motion(60, 50)) == "mouse_entered ok");
    CHECK(gui.mode("ok") == "pressed");
    CHECK(gui.send(click(60, 50, false)) == "pressed ok, button_up ok");

    // keep_pressed_outside draws it pressed while the pointer is away.
    gui.set("ok", R"({"Button": {"keep_pressed_outside": true}})");
    gui.send({click(50, 40, true), motion(300, 200)});
    CHECK(gui.mode("ok") == "pressed");
    CHECK(gui.send(click(300, 200, false)) == "button_up ok");
}

TEST_CASE("action_mode press presses as the click starts, and button_mask picks mouse buttons") {
    Gui gui;
    gui.button("ok", {20, 20, 100, 40}, R"("action_mode": "press")");
    CHECK(gui.send(click(50, 40, true)) ==
          "mouse_entered ok, focus_entered ok, button_down ok, pressed ok");
    CHECK(gui.send(click(50, 40, false)) == "button_up ok");

    // The right button doesn't press it, and doesn't give focus.
    gui.button("other", {200, 20, 100, 40});
    CHECK(gui.send({click(250, 40, true, "right"), click(250, 40, false, "right")}) ==
          "mouse_exited ok, mouse_entered other");
    CHECK(gui.focus() == "ok");
    gui.set("other", R"({"Button": {"button_mask": ["right"]}})");
    CHECK(gui.send({click(250, 40, true, "right"), click(250, 40, false, "right")}) ==
          "button_down other, pressed other, button_up other");
}

TEST_CASE("a tap presses a button, and the click it emulates doesn't press it twice") {
    Gui gui;
    gui.button("ok", {20, 20, 100, 40});
    // Touches stand in for the mouse too (emulate_mouse_from_touch): the emulated click hovers
    // and focuses the button, the touch presses it.
    CHECK(gui.send(touch(50, 40, true)) == "mouse_entered ok, focus_entered ok, button_down ok");
    CHECK(gui.send(touch(50, 40, false)) == "pressed ok, button_up ok");

    // Without the mouse, a touch that starts inside presses it, and one dragged off doesn't.
    Gui fingers(input::Settings{.emulate_mouse_from_touch = false});
    fingers.button("ok", {20, 20, 100, 40});
    CHECK(fingers.send(touch(50, 40, true)) == "button_down ok");
    CHECK(fingers.send(touch(50, 40, false)) == "pressed ok, button_up ok");
    CHECK(fingers.send(touch(50, 40, true)) == "button_down ok");
    CHECK(fingers.send(R"({"type": "screen_drag", "position": [300, 200]})").empty());
    CHECK(fingers.mode("ok") == "normal");
    CHECK(fingers.send(touch(300, 200, false)) == "button_up ok");
    CHECK(fingers.focus().empty());
    CHECK(fingers.hovered().empty());
}

namespace {

// A menu of four buttons and a title, the title and three buttons in a column and the fourth
// beside the second:
//   title
//   a
//   b  d
//   c
void make_menu(Gui& gui) {
    gui.create("title", R"({"Control": {"offset_left": 20}, "Label": {"text": "Menu"}})");
    gui.button("a", {20, 30, 100, 40}, R"("text": "A")");
    gui.button("b", {20, 80, 100, 40}, R"("text": "B")");
    gui.button("c", {20, 140, 100, 40}, R"("text": "C")");
    gui.button("d", {160, 80, 100, 40}, R"("text": "D")");
    gui.create("menu", R"({"Control": {"anchor_right": 1, "anchor_bottom": 1,
                                       "mouse_filter": "ignore",
                                       "children": ["title", "a", "b", "c", "d"]}})");
}

} // namespace

TEST_CASE(
    "Tab, the arrow keys and a gamepad move focus, and ui_accept presses the focused button") {
    Gui gui;
    make_menu(gui);
    // With nothing focused, Tab starts from the first control and skips the title, which can't
    // take focus.
    CHECK(gui.tap("Tab") == "focus_entered a");
    // Focus the keyboard gave is shown.
    CHECK(gui.boxes("a") == "flat, flat");
    CHECK(gui.tap("Down") == "focus_exited a, focus_entered b");
    CHECK(gui.tap("Right") == "focus_exited b, focus_entered d");
    CHECK(gui.tap("Left") == "focus_exited d, focus_entered b");
    CHECK(gui.tap("Tab") == "focus_exited b, focus_entered c");
    CHECK(gui.tap("Tab") == "focus_exited c, focus_entered d");
    // Round to the first.
    CHECK(gui.tap("Tab") == "focus_exited d, focus_entered a");
    CHECK(gui.tap("Tab", R"("shift": true)") == "focus_exited a, focus_entered d");
    CHECK(gui.tap("Up") == "focus_exited d, focus_entered a");
    // Nothing is above the first.
    CHECK(gui.tap("Up").empty());
    CHECK(gui.focus() == "a");

    // Enter and Space are ui_accept: down as the key goes down, pressed as it comes up.
    CHECK(gui.send(key("Enter", true)) == "button_down a");
    CHECK(gui.mode("a") == "pressed");
    CHECK(gui.send(key("Enter", false)) == "pressed a, button_up a");
    CHECK(gui.tap("Space") == "button_down a, pressed a, button_up a");

    // A stick moves focus once each time it is pushed.
    const std::string stick = R"({"type": "joypad_motion", "axis": "left_y", "axis_value": )";
    CHECK(gui.send(stick + "1}") == "focus_exited a, focus_entered b");
    CHECK(gui.send(stick + "0.9}").empty());
    CHECK(gui.send(stick + "0}").empty());
    CHECK(gui.send(stick + "1}") == "focus_exited b, focus_entered c");
    CHECK(gui.send({R"({"type": "joypad_button", "button": "dpad_up", "pressed": true})",
                    R"({"type": "joypad_button", "button": "dpad_up", "pressed": false})"}) ==
          "focus_exited c, focus_entered b");

    // The focus fields choose for themselves.
    gui.set("b", R"({"Control": {"focus_neighbor_bottom": "a", "focus_next": "d"}})");
    CHECK(gui.tap("Down") == "focus_exited b, focus_entered a");
    CHECK(gui.tap("Down") == "focus_exited a, focus_entered b");
    CHECK(gui.tap("Tab") == "focus_exited b, focus_entered d");
    // A control with focus_mode none is passed over, and loses focus.
    gui.set("c", R"({"Control": {"focus_mode": "none"}})");
    CHECK(gui.tap("Tab") == "focus_exited d, focus_entered a");
    CHECK(gui.tap("Tab", R"("shift": true)") == "focus_exited a, focus_entered d");
    CHECK(gui.tap("Tab", R"("shift": true)") == "focus_exited d, focus_entered b");
    CHECK(ui::grab_focus(gui.world, gui.world.find("d")));
    gui.set("d", R"({"Control": {"focus_mode": "none"}})");
    CHECK(gui.step() == "focus_exited b, focus_entered d, focus_exited d");
    CHECK(gui.focus().empty());
    CHECK_FALSE(ui::grab_focus(gui.world, gui.world.find("d")));
    CHECK_FALSE(ui::grab_focus(gui.world, gui.world.find("title")));
}

TEST_CASE("a click gives focus without showing it, until a key moves or shows it") {
    Gui gui;
    make_menu(gui);
    CHECK(gui.send({click(50, 50, true), click(50, 50, false)}) ==
          "mouse_entered a, focus_entered a, button_down a, pressed a, button_up a");
    CHECK(gui.boxes("a") == "flat");
    // A move that goes nowhere still shows where focus is.
    CHECK(gui.tap("Up").empty());
    CHECK(gui.boxes("a") == "flat, flat");
    // A click on nothing hides it again, and keeps it.
    CHECK(gui.send({click(380, 280, true), click(380, 280, false)}) == "mouse_exited a");
    CHECK(gui.focus() == "a");
    CHECK(gui.boxes("a") == "flat");
    // focus_mode click takes focus from clicks only.
    gui.set("b", R"({"Control": {"focus_mode": "click"}})");
    // Down from a passes b over for the closest that takes the keyboard's focus: d is 40
    // pixels across and 10 down from a, c 70 down.
    CHECK(gui.tap("Down") == "focus_exited a, focus_entered d");
    CHECK(gui.send({click(50, 100, true), click(50, 100, false)}) ==
          "mouse_entered b, focus_exited d, focus_entered b, button_down b, pressed b, "
          "button_up b");
}

TEST_CASE("toggle buttons switch, and at most one button of a group is on") {
    Gui gui;
    gui.button("easy", {20, 20, 100, 40},
               R"("toggle_mode": true, "button_group": "difficulty", "button_pressed": true)");
    gui.button("hard", {20, 80, 100, 40}, R"("toggle_mode": true, "button_group": "difficulty")");
    gui.button("mute", {200, 20, 100, 40}, R"("toggle_mode": true)");
    CHECK(gui.mode("easy") == "pressed");

    CHECK(gui.send({click(50, 100, true), click(50, 100, false)}) ==
          "mouse_entered hard, focus_entered hard, button_down hard, toggled easy off, "
          "toggled hard on, pressed hard, button_up hard");
    CHECK_FALSE(gui.pressed("easy"));
    CHECK(gui.pressed("hard"));
    CHECK(gui.mode("hard") == "hover_pressed");
    CHECK(gui.mode("easy") == "normal");
    // Pressing the one that is on leaves it on.
    CHECK(gui.send({click(50, 100, true), click(50, 100, false)}) ==
          "button_down hard, toggled hard on, pressed hard, button_up hard");
    CHECK(gui.pressed("hard"));

    // Outside a group, each press switches it.
    CHECK(gui.send({click(250, 40, true), click(250, 40, false)}) ==
          "mouse_exited hard, mouse_entered mute, focus_exited hard, focus_entered mute, "
          "button_down mute, toggled mute on, pressed mute, button_up mute");
    CHECK(gui.send({click(250, 40, true), click(250, 40, false)}) ==
          "button_down mute, toggled mute off, pressed mute, button_up mute");
    // Held down, a button that is on draws as off, as Godot's.
    gui.send(click(50, 100, true));
    CHECK(gui.mode("hard") == "normal");
    gui.send(click(50, 100, false));

    // ui::press presses at the next tick, as a click would.
    ui::press(gui.world, gui.world.find("easy"));
    CHECK(gui.step() == "toggled hard off, toggled easy on, pressed easy");
    CHECK(gui.pressed("easy"));
    CHECK_FALSE(gui.pressed("hard"));
}

TEST_CASE("shortcuts press buttons from anywhere and light them up for a moment") {
    Gui gui;
    gui.button("save", {20, 20, 100, 40},
               R"("shortcut": [{"type": "key", "key": "S", "ctrl": true}])");
    gui.button("quit", {20, 80, 100, 40},
               R"("shortcut": [{"type": "action", "action": "ui_cancel"}])");
    gui.button("locked", {20, 140, 100, 40},
               R"("disabled": true, "shortcut": [{"type": "key", "key": "L"}])");
    CHECK(gui.tap("S", R"("ctrl": true)") == "pressed save");
    CHECK(gui.mode("save") == "hover_pressed");
    for (int i = 0; i < 11; ++i) {
        gui.step();
    }
    CHECK(gui.mode("save") == "hover_pressed");
    gui.step();
    CHECK(gui.mode("save") == "normal");
    // Bindings match exactly: S alone isn't Ctrl+S.
    CHECK(gui.tap("S").empty());
    CHECK(gui.tap("Escape") == "pressed quit");
    CHECK(gui.mode("quit") == "hover_pressed");
    for (int i = 0; i < 12; ++i) {
        gui.step();
    }
    // A disabled button's shortcut does nothing.
    CHECK(gui.tap("L").empty());
    CHECK(gui.mode("locked") == "disabled");
    // shortcut_feedback false doesn't light it up.
    gui.set("quit", R"({"Button": {"shortcut_feedback": false}})");
    CHECK(gui.tap("Escape") == "pressed quit");
    CHECK(gui.mode("quit") == "normal");
}

TEST_CASE("disabled buttons aren't pressed, and a held button lets go when disabled or hidden") {
    Gui gui;
    gui.button("ok", {20, 20, 100, 40});
    CHECK(gui.send(click(50, 40, true)) == "mouse_entered ok, focus_entered ok, button_down ok");
    gui.set("ok", R"({"Button": {"disabled": true}})");
    CHECK(gui.step() == "button_up ok");
    CHECK(gui.mode("ok") == "disabled");
    CHECK(gui.send(click(50, 40, false)).empty());
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}).empty());
    ui::press(gui.world, gui.world.find("ok"));
    CHECK(gui.step().empty());

    gui.set("ok", R"({"Button": {"disabled": false}})");
    CHECK(gui.send(click(50, 40, true)) == "button_down ok");
    // Losing focus lets go of it, as Godot's, then the pointer leaves.
    gui.set("ok", R"({"Control": {"visible": false}})");
    CHECK(gui.step() == "focus_exited ok, button_up ok, mouse_exited ok");
    CHECK(gui.focus().empty());
    CHECK(gui.hovered().empty());
    CHECK(gui.send(click(50, 40, false)).empty());

    // A destroyed button goes quietly.
    gui.set("ok", R"({"Control": {"visible": true}})");
    CHECK(gui.send(click(50, 40, true)) == "mouse_entered ok, focus_entered ok, button_down ok");
    gui.world.destroy(gui.world.find("ok"));
    CHECK(gui.step().empty());
    CHECK(gui.focus().empty());
    CHECK(gui.send(click(50, 40, false)).empty());
}

TEST_CASE("mouse_filter decides which control a click reaches") {
    Gui gui;
    gui.create("caption", R"({"Control": {"anchor_right": 1, "anchor_bottom": 1},
                              "Label": {"text": "OK"}})");
    gui.button("ok", {20, 20, 100, 40}, "", R"("children": ["caption"])");
    // A Label lets clicks by (ignore), so the button under it is pressed.
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}) ==
          "mouse_entered ok, focus_entered ok, button_down ok, pressed ok, button_up ok");

    // One that stops them takes them itself, and the pointer leaves the button.
    gui.set("caption", R"({"Control": {"mouse_filter": "stop"}})");
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}) ==
          "mouse_exited ok, mouse_entered caption");
    CHECK(gui.hovered() == "caption");

    // One that passes them hands them on to its parent. (Hover is worked out as the pointer
    // moves onto a control, so it leaves and comes back.)
    gui.set("caption", R"({"Control": {"mouse_filter": "pass"}})");
    CHECK(gui.send(motion(300, 200)) == "mouse_exited caption");
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}) ==
          "mouse_entered ok, mouse_entered caption, button_down ok, pressed ok, button_up ok");

    // A control drawn over the button takes clicks first, unless it ignores them.
    gui.create("cover", R"({"Control": {"anchor_right": 1, "anchor_bottom": 1},
                            "ColorRect": {"color": [0, 0, 0, 0.5]}})");
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}) ==
          "mouse_exited caption, mouse_exited ok, mouse_entered cover");
    CHECK(gui.hovered() == "cover");
    gui.set("cover", R"({"Control": {"mouse_filter": "ignore"}})");
    CHECK(gui.send(motion(300, 200)) == "mouse_exited cover");
    CHECK(gui.send({click(50, 40, true), click(50, 40, false)}) ==
          "mouse_entered ok, mouse_entered caption, button_down ok, pressed ok, button_up ok");
}

TEST_CASE("a top-level control takes clicks over its parent's tree and outside its clip") {
    Gui gui;
    gui.button("pop", {150, 100, 100, 40});
    gui.button("cover", {100, 80, 200, 100});
    gui.create("menu", R"({"Control": {"offset_right": 100, "offset_bottom": 100,
                                       "children": ["pop", "cover"]}})");
    // As one of menu's children, pop is under cover, the one after it.
    CHECK(gui.send({click(200, 120, true), click(200, 120, false)}) ==
          "mouse_entered cover, focus_entered cover, button_down cover, pressed cover, "
          "button_up cover");

    // A top-level control comes after the rest of its tree, as Godot's top_level.
    gui.set("pop", R"({"Control": {"top_level": true}})");
    CHECK(gui.send({click(200, 120, true), click(200, 120, false)}) ==
          "mouse_exited cover, mouse_entered pop, focus_exited cover, focus_entered pop, "
          "button_down pop, pressed pop, button_up pop");

    // When menu clips, the pointer reaches neither of its children there, but pop isn't clipped.
    gui.set("menu", R"({"Control": {"clip_contents": true}})");
    gui.set("pop", R"({"Control": {"top_level": false}})");
    CHECK(gui.send({click(200, 120, true), click(200, 120, false)}) == "mouse_exited pop");
    gui.set("pop", R"({"Control": {"top_level": true}})");
    CHECK(gui.send({click(200, 120, true), click(200, 120, false)}) ==
          "mouse_entered pop, button_down pop, pressed pop, button_up pop");
}

TEST_CASE("a TextureButton draws the texture for its state and is as big as its texture") {
    Gui gui;
    for (const std::string_view name : {"normal", "hover", "pressed", "focused"}) {
        gui.files.files[std::string(name) + ".png"] = png(32, 16);
    }
    gui.create("play", R"({"Control": {"offset_left": 10, "offset_top": 10},
        "TextureButton": {"texture_normal": "normal.png", "texture_hover": "hover.png",
                          "texture_pressed": "pressed.png", "texture_focused": "focused.png"}})");
    ui::Layout layout = gui.layout();
    const ui::Placed* placed = layout.find(gui.world.find("play"));
    REQUIRE(placed != nullptr);
    CHECK(placed->size == Vec2{32.0F, 16.0F});
    CHECK(gui.boxes("play") == "normal.png");
    gui.send(motion(20, 15));
    CHECK(gui.boxes("play") == "hover.png");
    gui.send(click(20, 15, true));
    CHECK(gui.boxes("play") == "pressed.png");
    gui.send(click(20, 15, false));
    // The focused texture goes over the others once focus shows.
    gui.tap("Up");
    CHECK(gui.boxes("play") == "hover.png, focused.png");
    // Disabled with no disabled texture, it draws the normal one, and keeps focus.
    gui.set("play", R"({"TextureButton": {"disabled": true}})");
    CHECK(gui.boxes("play") == "normal.png, focused.png");

    // ignore_texture_size lets it be smaller, and scale stretches the texture over it.
    gui.set("play", R"({"Control": {"offset_left": 10, "offset_top": 10, "offset_right": 30,
                                    "offset_bottom": 18},
        "TextureButton": {"ignore_texture_size": true, "stretch_mode": "scale",
                          "disabled": false, "texture_focused": ""}})");
    layout = gui.layout();
    CHECK(layout.find(gui.world.find("play"))->size == Vec2{20.0F, 8.0F});
    CHECK(layout.boxes.at(gui.world.find("play")).at(0).size == Vec2{20.0F, 8.0F});
}

TEST_CASE("a button is as big as its text and style margins, and aligns its text") {
    Gui gui;
    gui.create("label", R"({"Control": {}, "Label": {"text": "Play"}})");
    gui.create("play", R"({"Control": {"offset_top": 100}, "Button": {"text": "Play"}})");
    ui::Layout layout = gui.layout();
    const Vec2 text = layout.find(gui.world.find("label"))->minimum_size;
    const ui::Placed* play = layout.find(gui.world.find("play"));
    // Godot's default button styles keep 4 pixels on each side.
    CHECK(play->minimum_size == Vec2{text.x + 8.0F, text.y + 8.0F});
    CHECK(play->size == play->minimum_size);
    CHECK(layout.buttons.at(gui.world.find("play")).mode == ui::DrawMode::Normal);

    // In a wider button the text goes left, center or right.
    const auto pen_x = [&](std::string_view alignment) {
        gui.set("play", R"({"Control": {"offset_top": 100, "offset_right": 200},
                            "Button": {"text": "Play", "alignment": ")" +
                            std::string(alignment) + "\"}}");
        const ui::Layout wide = gui.layout();
        return wide.label(gui.world.find("play"))->pens.at(0).x;
    };
    CHECK(pen_x("left") == 4.0F);
    CHECK(pen_x("center") == std::floor((192.0F - text.x) / 2.0F) + 4.0F);
    CHECK(pen_x("right") == 196.0F - text.x);

    // Text too wide for a clip_text button is cut with an ellipsis.
    gui.set("play", R"({"Control": {"offset_top": 100, "offset_right": 60},
                        "Button": {"text": "Play the game", "clip_text": true,
                                   "text_overrun_behavior": "trim_ellipsis"}})");
    layout = gui.layout();
    play = layout.find(gui.world.find("play"));
    CHECK(play->size.x == 60.0F);
    const ui::LabelText* shown = layout.label(gui.world.find("play"));
    REQUIRE(shown != nullptr);
    CHECK(shown->ellipsis == "…");
    CHECK(shown->trimmed == std::vector<std::size_t>{0});
}

TEST_CASE("check finds wrong shortcuts, groups on buttons that don't toggle and focus fields") {
    Gui gui;
    gui.create("thing", R"({"Transform2D": {}})");
    gui.create("ok", R"({"Control": {"focus_next": "thing"}, "Label": {},
        "Button": {"button_group": "g", "shortcut": [
            {"type": "action", "action": "ui_acept"}, {"type": "key", "key": "Entr"},
            {"type": "action", "action": "ui_cancel", "pressed": true}]}})");
    gui.create("both", R"({"Control": {}, "Button": {}, "TextureButton": {}})");
    CHECK(problem_lines(ui::check(gui.world, gui.fonts())) ==
          std::vector<std::string>{
              "both/components/TextureButton: an entity is one button; it has a Button too",
              "ok/components/Button/shortcut/0/action: no action \"ui_acept\"; did you mean "
              "\"ui_accept\"?",
              "ok/components/Button/shortcut/1/key: no key \"Entr\"; did you mean \"Enter\"?",
              "ok/components/Button/shortcut/2/pressed: an action shortcut has only \"type\" and "
              "\"action\"",
              "ok/components/Button/button_group: button groups are for toggle buttons; set "
              "toggle_mode too",
              "ok/components/Label: a button draws its own text; give it to the button, or put the "
              "Label in a child",
              "ok/components/Control/focus_next: names no control"});

    // Scene files are checked the same way, against the actions their project.json adds.
    const json::Value scene = parse(R"({"format": 1, "entities": {"ok": {"components": {
        "Control": {}, "Button": {"shortcut": [{"type": "action", "action": "pause"},
                                              {"type": "action", "action": "paws"}]}}}}})");
    const world::FileReader reader = [](std::string_view path) -> slash::Result<std::string> {
        if (path != "project.json") {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return std::string(R"({"input": {"pause": {"events": [{"type": "key", "key": "P"}]}}})");
    };
    const std::vector<json::ValidationError> errors =
        ui::check_scene(gui.components, scene, reader);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entities/ok/components/Button/shortcut/1/action");
    CHECK(errors[0].message == "no action \"paws\"; did you mean \"pause\"?");

    // Mouse buttons are named in the schema.
    const json::Value schema = world::scene_schema(gui.components);
    CHECK(*schema.find("$defs")
               ->find("Button")
               ->find("properties")
               ->find("button_mask")
               ->find("items") ==
          parse(R"({"type": "string", "enum": ["left", "right", "middle"]})"));
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

TEST_CASE("ui.press, ui.focus and ui.layout drive and report buttons") {
    Session s;
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "start", "components": {
            "Control": {"offset_left": 20, "offset_top": 20, "offset_right": 120,
                        "offset_bottom": 60},
            "Button": {"text": "Start"}}}},
        {"command": "entity.create", "params": {"id": "sound", "components": {
            "Control": {"offset_left": 20, "offset_top": 80, "offset_right": 120,
                        "offset_bottom": 120},
            "Button": {"text": "Sound", "toggle_mode": true}}}},
        {"command": "entity.create", "params": {"id": "locked", "components": {
            "Control": {"offset_left": 20, "offset_top": 140, "offset_right": 120,
                        "offset_bottom": 180},
            "Button": {"text": "Locked", "disabled": true}}}},
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Control": {}, "Label": {"text": "Menu"}}}}]})");
    CHECK(s.ok("ui.focus", R"({"id": "start"})") == parse(R"({"focused": "start"})"));
    CHECK(s.ok("ui.press", R"({"id": "sound"})") == parse(R"({"tick": 0})"));
    s.ok("input.inject", R"({"events": [{"type": "mouse_motion", "position": [50, 160]}]})");
    s.ok("step");
    CHECK(s.ok("ui.layout", R"({"ids": ["start", "sound", "locked"]})") ==
          parse(R"({"viewport": [1280, 720], "controls": [
              {"id": "start", "rect": [20, 20, 100, 40], "draw_mode": "normal",
               "focused": true, "lines": ["Start"]},
              {"id": "sound", "rect": [20, 80, 100, 40], "draw_mode": "pressed",
               "lines": ["Sound"]},
              {"id": "locked", "rect": [20, 140, 100, 40], "draw_mode": "disabled",
               "hovered": true, "lines": ["Locked"]}]})"));

    // A failed batch leaves focus as it was.
    CHECK_FALSE(s.call("batch", R"({"commands": [
        {"command": "ui.focus", "params": {"id": "sound"}},
        {"command": "entity.destroy", "params": {"id": "nobody"}}]})")
                    .ok);
    CHECK(s.ok("ui.focus", R"({"id": "start"})") == parse(R"({"focused": "start"})"));
    CHECK(s.ok("ui.focus", R"({"id": null})") == parse(R"({"focused": null})"));

    CHECK(s.fails("ui.press", R"({"id": "strat"})") ==
          "/id: no entity has the id \"strat\"; did you mean \"start\"?");
    CHECK(s.fails("ui.press", R"({"id": "title"})") ==
          "/id: \"title\" isn't a button: it has no Button, CheckBox, CheckButton, "
          "OptionButton, MenuButton or TextureButton");
    CHECK(s.fails("ui.press", R"({"id": "locked"})") ==
          "/id: \"locked\" is disabled, so it can't be pressed");
    CHECK(s.fails("ui.focus", R"({"id": "title"})") ==
          "/id: \"title\" can't take focus: it is hidden, or its focus_mode is \"none\" (the "
          "default for controls that aren't buttons or LineEdits)");
}
