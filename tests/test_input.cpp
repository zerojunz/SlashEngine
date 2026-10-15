#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/input/input.hpp"
#include "slash/input/sdl.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace runtime = slash::runtime;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), std::string(text));
    return std::move(parsed).value();
}

input::Event event(std::string_view text) {
    auto parsed = input::parse_event(parse(text), input::Use::Event);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
    return std::move(parsed).value();
}

input::Event binding(std::string_view text) {
    auto parsed = input::parse_event(parse(text), input::Use::Binding);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
    return std::move(parsed).value();
}

std::vector<std::string> messages(const std::vector<json::ValidationError>& errors) {
    std::vector<std::string> out;
    out.reserve(errors.size());
    for (const json::ValidationError& error : errors) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

// A world with input and the actions a platformer binds.
struct Input {
    world::ComponentRegistry components;
    world::World world{components};

    explicit Input(input::Settings settings = {}) {
        input::Map map = input::Map::with_defaults();
        map.add_action("jump");
        map.action_add_event("jump", binding(R"({"type": "key", "key": "Space"})"));
        map.action_add_event("jump", binding(R"({"type": "joypad_button", "button": "a"})"));
        map.add_action("left");
        map.action_add_event("left", binding(R"({"type": "key", "physical_key": "A"})"));
        map.action_add_event("left", binding(R"({"type": "joypad_motion", "axis": "left_x",
                                                 "axis_value": -1})"));
        map.add_action("right");
        map.action_add_event("right", binding(R"({"type": "key", "physical_key": "D"})"));
        map.action_add_event("right", binding(R"({"type": "joypad_motion", "axis": "left_x",
                                                  "axis_value": 1})"));
        map.add_action("save");
        map.action_add_event("save", binding(R"({"type": "key", "key": "S", "ctrl": true})"));
        input::install(world, std::move(map), settings);
    }

    void send(std::string_view text) { input::push(world, event(text)); }
    void step() { world.step(kTick); }
};

} // namespace

TEST_CASE("input events read and write JSON as input.inject takes them") {
    const input::Event key = event(R"({"type": "key", "key": "space", "pressed": true})");
    CHECK(key.type == input::EventType::Key);
    CHECK(key.key == input::Key::Space);
    // An event names its key both ways, as one from a keyboard does.
    CHECK(key.physical_key == input::Key::Space);
    CHECK(key.device == 0);
    CHECK(input::event_json(key, input::Use::Event) ==
          parse(R"({"type": "key", "key": "Space", "pressed": true})"));

    const input::Event click = event(R"({"type": "mouse_button", "button": "right",
        "position": [10, 20], "pressed": true, "shift": true, "double_click": true})");
    CHECK(click.button == input::MouseButton::Right);
    CHECK(click.position == Vec2{10.0F, 20.0F});
    CHECK(click.modifiers.shift);
    CHECK(input::event_json(click, input::Use::Event) ==
          parse(R"({"type": "mouse_button", "button": "right", "position": [10, 20],
                    "pressed": true, "shift": true, "double_click": true})"));

    const input::Event stick = binding(R"({"type": "joypad_motion", "axis": "left_y",
                                           "axis_value": 0.3})");
    // A binding's axis says only which way.
    CHECK(stick.axis_value == 1.0F);
    CHECK(stick.device == input::kAllDevices);
    CHECK(input::event_json(stick, input::Use::Binding) ==
          parse(R"({"type": "joypad_motion", "axis": "left_y", "axis_value": 1})"));

    CHECK(input::key_name(input::Key::KpEnter) == "Kp Enter");
    CHECK(input::parse_key("kp enter") == input::Key::KpEnter);
    CHECK(input::parse_key("1") == input::Key::Key1);
    CHECK_FALSE(input::parse_key("Spce").has_value());
    CHECK(input::key_names().size() == 149);
}

TEST_CASE("text events type characters, and ime events carry what an input method composes") {
    // A key can carry the character it types, or only that.
    const input::Event q = event(R"({"type": "key", "key": "Q", "unicode": "q", "pressed": true})");
    CHECK(q.unicode == U'q');
    CHECK(input::event_json(q, input::Use::Event) ==
          parse(R"({"type": "key", "key": "Q", "unicode": "q", "pressed": true})"));
    CHECK(event(R"({"type": "key", "unicode": "é", "pressed": true})").key == input::Key::None);

    // A text event is a key press with a character for each character.
    auto typed = input::parse_events(parse(R"({"type": "text", "text": "hé!"})"));
    REQUIRE(typed.ok());
    REQUIRE(typed.value().size() == 3);
    CHECK(typed.value()[1].unicode == U'é');
    CHECK(typed.value()[1].pressed);
    CHECK(typed.value()[1].key == input::Key::None);
    CHECK_FALSE(input::parse_event(parse(R"({"type": "text", "text": "x"})"), input::Use::Event));

    const input::Event composing = event(R"({"type": "ime", "text": "かな", "selection": [0, 2]})");
    CHECK(composing.type == input::EventType::Ime);
    CHECK(composing.text == "かな");
    CHECK(composing.selection_length == 2);
    CHECK(input::event_json(composing, input::Use::Event) ==
          parse(R"({"type": "ime", "text": "かな", "selection": [0, 2]})"));

    CHECK(messages(input::validate_event(parse(R"({"type": "text", "text": "a", "key": "A"})"),
                                         input::Use::Event)) ==
          std::vector<std::string>{R"(/key: text events have no "key")"});
    CHECK(messages(input::validate_event(parse(R"({"type": "ime", "text": "かな",
                                                   "selection": [1, 2]})"),
                                         input::Use::Event)) ==
          std::vector<std::string>{
              "/selection: the selection [start, length] is past the text's end (2 characters)"});
    CHECK(messages(input::validate_event(parse(R"({"type": "key", "unicode": "ab"})"),
                                         input::Use::Event)) ==
          std::vector<std::string>{"/unicode: unicode is one character"});
    // Bindings match keys, not what they type.
    CHECK_FALSE(
        input::validate_event(parse(R"({"type": "text", "text": "a"})"), input::Use::Binding)
            .empty());
}

TEST_CASE("text editing actions use Ctrl on a PC, and Command and Option on a Mac") {
    const input::Map pc = input::Map::with_defaults(input::Keyboard::Pc);
    const input::Map mac = input::Map::with_defaults(input::Keyboard::Mac);
    const auto key = [](input::Key name, input::Modifiers modifiers) {
        input::Event out;
        out.type = input::EventType::Key;
        out.key = name;
        out.modifiers = modifiers;
        out.pressed = true;
        return out;
    };
    using input::Key;
    CHECK(pc.find_event("ui_copy", key(Key::C, {.ctrl = true}), true));
    CHECK_FALSE(pc.find_event("ui_copy", key(Key::C, {.meta = true}), true));
    CHECK(mac.find_event("ui_copy", key(Key::C, {.meta = true}), true));
    CHECK(pc.find_event("ui_redo", key(Key::Y, {.ctrl = true}), true));
    CHECK(pc.find_event("ui_text_caret_word_left", key(Key::Left, {.ctrl = true}), true));
    CHECK(mac.find_event("ui_text_caret_word_left", key(Key::Left, {.alt = true}), true));
    CHECK(mac.find_event("ui_text_caret_line_start", key(Key::A, {.ctrl = true}), true));
    CHECK_FALSE(pc.find_event("ui_text_caret_line_start", key(Key::A, {.ctrl = true}), true));
    // Some have keys only on a Mac.
    CHECK(pc.find("ui_text_backspace_all_to_left")->events.empty());
    CHECK(
        mac.find_event("ui_text_backspace_all_to_left", key(Key::Backspace, {.meta = true}), true));
    CHECK(input::is_text_action("ui_text_submit"));
    CHECK(input::is_text_action("ui_paste"));
    CHECK_FALSE(input::is_text_action("ui_accept"));
    // project.json's actions go over the keyboard's.
    auto map = input::parse_map(parse(R"({"ui_copy": {"events": [
        {"type": "key", "key": "F3"}]}})"),
                                input::Keyboard::Mac);
    REQUIRE(map.ok());
    CHECK(map.value().find("ui_copy")->events.size() == 1);
    CHECK(map.value().find_event("ui_paste", key(Key::V, {.meta = true}), true));
}

TEST_CASE("input event errors give the path and the closest name") {
    CHECK(messages(input::validate_event(parse(R"({"type": "key", "key": "Spce"})"),
                                         input::Use::Event, "/events/0")) ==
          std::vector<std::string>{R"(/events/0/key: no key "Spce"; did you mean "Space"?)"});
    CHECK(messages(input::validate_event(parse(R"({"type": "mouse_button", "button": "lefft",
                                                   "key": "A"})"),
                                         input::Use::Event)) ==
          std::vector<std::string>{R"(/key: mouse_button events have no "key")",
                                   R"(/button: no mouse button "lefft"; did you mean "left"?)",
                                   R"(: mouse_button events need "position")"});
    CHECK(messages(input::validate_event(parse(R"({"type": "key"})"), input::Use::Binding)) ==
          std::vector<std::string>{R"(: key bindings need "key" or "physical_key")"});
    // Bindings say what presses an action, not where or when.
    CHECK(messages(input::validate_event(parse(R"({"type": "key", "key": "A", "pressed": true})"),
                                         input::Use::Binding))
              .size() == 1);
    CHECK_FALSE(input::validate_event(parse(R"({"type": "screen_touch", "position": [0, 0]})"),
                                      input::Use::Binding)
                    .empty());
    CHECK(messages(input::validate_event(parse(R"({"type": "joypad_motion", "axis": "left_x",
                                                   "axis_value": 0})"),
                                         input::Use::Binding)) ==
          std::vector<std::string>{"/axis_value: a binding's axis_value is -1 or 1"});
    CHECK(
        messages(input::validate_event(parse(R"({"type": "joypad_button", "button": "triangle"})"),
                                       input::Use::Event))
            .size() == 1);
}

TEST_CASE("bindings match events as Godot's action_match") {
    const input::Event save = binding(R"({"type": "key", "key": "S", "ctrl": true})");
    // The binding's modifiers must be held; more are fine unless the match is exact.
    CHECK(input::match(save, event(R"({"type": "key", "key": "S", "ctrl": true,
                                       "pressed": true})"),
                       0.5F)
              ->pressed);
    CHECK_FALSE(input::match(save, event(R"({"type": "key", "key": "S", "pressed": true})"), 0.5F));
    const input::Event with_shift =
        event(R"({"type": "key", "key": "S", "ctrl": true, "shift": true, "pressed": true})");
    CHECK(input::match(save, with_shift, 0.5F));
    CHECK_FALSE(input::match(save, with_shift, 0.5F, /*exact=*/true));

    // A physical binding matches where the key is, whatever it is labelled.
    const input::Event left = binding(R"({"type": "key", "physical_key": "A"})");
    CHECK(input::match(left, event(R"({"type": "key", "key": "Q", "physical_key": "A",
                                       "pressed": true})"),
                       0.5F));
    CHECK_FALSE(input::match(left, event(R"({"type": "key", "key": "A", "physical_key": "Q",
                                             "pressed": true})"),
                             0.5F));

    // Past the deadzone, strength runs from 0 to 1; the raw strength ignores the deadzone.
    const input::Event stick = binding(R"({"type": "joypad_motion", "axis": "left_x",
                                           "axis_value": 1})");
    const auto half = input::match(stick, event(R"({"type": "joypad_motion", "axis": "left_x",
                                                    "axis_value": 0.6})"),
                                   0.2F);
    REQUIRE(half);
    CHECK(half->pressed);
    CHECK(half->strength == doctest::Approx(0.5));
    CHECK(half->raw_strength == doctest::Approx(0.6));
    const auto inside = input::match(stick, event(R"({"type": "joypad_motion", "axis": "left_x",
                                                      "axis_value": 0.1})"),
                                     0.2F);
    REQUIRE(inside);
    CHECK_FALSE(inside->pressed);
    CHECK(inside->strength == 0.0F);
    // The other way matches too, so the action lets go, but reads as released.
    const auto other_way = input::match(stick, event(R"({"type": "joypad_motion",
                                                         "axis": "left_x", "axis_value": -1})"),
                                        0.2F);
    REQUIRE(other_way);
    CHECK_FALSE(other_way->pressed);
    CHECK(other_way->raw_strength == 0.0F);

    // A device binding matches only that gamepad.
    input::Map map;
    map.add_action("p2_jump");
    map.action_add_event("p2_jump",
                         binding(R"({"type": "joypad_button", "button": "a", "device": 1})"));
    CHECK_FALSE(map.find_event("p2_jump", event(R"({"type": "joypad_button", "button": "a",
                                                    "pressed": true})")));
    CHECK(map.find_event("p2_jump", event(R"({"type": "joypad_button", "button": "a",
                                              "device": 1, "pressed": true})")));
}

TEST_CASE("an action is pressed from the tick its events apply") {
    Input in;
    in.send(R"({"type": "key", "key": "Space", "pressed": true})");
    // Queued events wait for the next tick.
    CHECK(input::queued(in.world) == 1);
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));

    in.step();
    CHECK(input::queued(in.world) == 0);
    CHECK(input::is_action_pressed(in.world, "jump"));
    CHECK(input::is_action_just_pressed(in.world, "jump"));
    CHECK(input::get_action_strength(in.world, "jump") == 1.0F);
    CHECK(input::is_key_pressed(in.world, input::Key::Space));
    // Space is ui_accept and ui_select too.
    CHECK(input::is_action_just_pressed(in.world, "ui_accept"));
    REQUIRE(input::events(in.world).size() == 1);

    in.step();
    CHECK(input::is_action_pressed(in.world, "jump"));
    CHECK_FALSE(input::is_action_just_pressed(in.world, "jump"));
    CHECK(input::events(in.world).empty());

    // Two bindings hold it; it lets go when both do.
    in.send(R"({"type": "joypad_button", "button": "a", "pressed": true})");
    in.send(R"({"type": "key", "key": "Space", "pressed": false})");
    in.step();
    CHECK(input::is_action_pressed(in.world, "jump"));
    CHECK_FALSE(input::is_action_just_released(in.world, "jump"));
    in.send(R"({"type": "joypad_button", "button": "a", "pressed": false})");
    in.step();
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    CHECK(input::is_action_just_released(in.world, "jump"));
    in.step();
    CHECK_FALSE(input::is_action_just_released(in.world, "jump"));

    // Unknown actions read as released.
    CHECK_FALSE(input::is_action_pressed(in.world, "fly"));
    CHECK(input::get_action_strength(in.world, "fly") == 0.0F);
}

TEST_CASE("a tap within one tick is both just pressed and just released") {
    Input in;
    bool seen = false;
    in.world.add_system("player", [&](world::World& w, double /*dt*/) {
        seen = seen || input::is_action_just_pressed(w, "jump");
    });
    in.send(R"({"type": "key", "key": "Space", "pressed": true})");
    in.send(R"({"type": "key", "key": "Space", "pressed": false})");
    in.step();
    CHECK(seen);
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    CHECK(input::is_action_just_pressed(in.world, "jump"));
    CHECK(input::is_action_just_released(in.world, "jump"));
}

TEST_CASE("actions can be pressed by name, and modifiers make a match exact") {
    Input in;
    input::action_press(in.world, "jump", 0.5F);
    in.step();
    CHECK(input::is_action_pressed(in.world, "jump"));
    CHECK(input::get_action_strength(in.world, "jump") == 0.5F);
    input::action_release(in.world, "jump");
    in.step();
    CHECK(input::is_action_just_released(in.world, "jump"));

    in.send(R"({"type": "key", "key": "S", "ctrl": true, "shift": true, "pressed": true})");
    in.step();
    CHECK(input::is_action_pressed(in.world, "save"));
    CHECK_FALSE(input::is_action_pressed(in.world, "save", /*exact=*/true));
}

TEST_CASE("gamepad axes press actions past their deadzone") {
    Input in;
    in.send(R"({"type": "joypad_motion", "axis": "left_x", "axis_value": -0.6})");
    in.step();
    CHECK(input::is_action_pressed(in.world, "left"));
    CHECK(input::get_action_strength(in.world, "left") == doctest::Approx(0.5));
    CHECK(input::get_action_raw_strength(in.world, "left") == doctest::Approx(0.6));
    CHECK_FALSE(input::is_action_pressed(in.world, "right"));
    CHECK(input::get_axis(in.world, "left", "right") == doctest::Approx(-0.5));
    CHECK(input::get_joy_axis(in.world, 0, input::JoyAxis::LeftX) == doctest::Approx(-0.6));
    // ui_left's deadzone is Godot's 0.5.
    CHECK(input::is_action_pressed(in.world, "ui_left"));
    CHECK(input::get_action_strength(in.world, "ui_left") == doctest::Approx(0.2));

    in.send(R"({"type": "joypad_motion", "axis": "left_x", "axis_value": 0.1})");
    in.step();
    CHECK_FALSE(input::is_action_pressed(in.world, "left"));
    CHECK(input::is_action_just_released(in.world, "left"));
    CHECK(input::get_axis(in.world, "left", "right") == 0.0F);
}

TEST_CASE("get_vector has a round deadzone and is at most 1 long") {
    Input in;
    input::Map& map = *input::map(in.world);
    map.add_action("up", 0.0F);
    map.action_add_event("up", binding(R"({"type": "joypad_motion", "axis": "left_y",
                                           "axis_value": -1})"));
    map.add_action("down", 0.0F);
    map.action_add_event("down", binding(R"({"type": "joypad_motion", "axis": "left_y",
                                             "axis_value": 1})"));
    in.send(R"({"type": "key", "physical_key": "D", "pressed": true})");
    in.send(R"({"type": "joypad_motion", "axis": "left_y", "axis_value": 1})");
    in.step();
    const Vec2 diagonal = input::get_vector(in.world, "left", "right", "up", "down", 0.0F);
    CHECK(diagonal.x == doctest::Approx(0.70710678));
    CHECK(diagonal.y == doctest::Approx(0.70710678));
    // Inside the deadzone the vector is zero; past it lengths start from 0.
    in.send(R"({"type": "key", "physical_key": "D", "pressed": false})");
    in.send(R"({"type": "joypad_motion", "axis": "left_y", "axis_value": 0.3})");
    in.step();
    CHECK(input::get_vector(in.world, "left", "right", "up", "down", 0.5F) == Vec2{});
    in.send(R"({"type": "joypad_motion", "axis": "left_y", "axis_value": 0.75})");
    in.step();
    CHECK(input::get_vector(in.world, "left", "right", "up", "down", 0.5F).y ==
          doctest::Approx(0.5));
}

TEST_CASE("touches stand in for the mouse, and the mouse for touches if asked") {
    Input in;
    in.send(R"({"type": "screen_touch", "position": [100, 50], "pressed": true})");
    in.step();
    CHECK(input::is_mouse_button_pressed(in.world, input::MouseButton::Left));
    CHECK(input::mouse_position(in.world) == Vec2{100.0F, 50.0F});
    // The emulated click comes first, from the emulation device, then the touch.
    REQUIRE(input::events(in.world).size() == 2);
    CHECK(input::events(in.world)[0].type == input::EventType::MouseButton);
    CHECK(input::events(in.world)[0].device == input::kEmulatedDevice);
    CHECK(input::events(in.world)[1].type == input::EventType::ScreenTouch);

    // A second finger doesn't move the mouse; the first does.
    in.send(R"({"type": "screen_touch", "index": 1, "position": [300, 300], "pressed": true})");
    in.send(R"({"type": "screen_drag", "index": 1, "position": [310, 300]})");
    in.send(R"({"type": "screen_drag", "position": [120, 60]})");
    in.step();
    CHECK(input::mouse_position(in.world) == Vec2{120.0F, 60.0F});
    in.send(R"({"type": "screen_touch", "position": [120, 60], "pressed": false})");
    in.step();
    CHECK_FALSE(input::is_mouse_button_pressed(in.world, input::MouseButton::Left));

    Input mouse({.emulate_mouse_from_touch = false, .emulate_touch_from_mouse = true});
    mouse.send(R"({"type": "mouse_button", "button": "left", "position": [5, 5],
                   "pressed": true})");
    mouse.step();
    REQUIRE(input::events(mouse.world).size() == 2);
    CHECK(input::events(mouse.world)[0].type == input::EventType::ScreenTouch);
    CHECK(input::events(mouse.world)[0].position == Vec2{5.0F, 5.0F});
    mouse.send(R"({"type": "screen_touch", "position": [9, 9], "pressed": true})");
    mouse.step();
    CHECK(input::mouse_position(mouse.world) == Vec2{5.0F, 5.0F});
}

TEST_CASE("mouse motion is worked out from the last position and merges in the queue") {
    Input in;
    in.send(R"({"type": "mouse_motion", "position": [10, 10]})");
    in.send(R"({"type": "mouse_motion", "position": [15, 12]})");
    in.send(R"({"type": "joypad_motion", "axis": "left_x", "axis_value": 0.2})");
    in.send(R"({"type": "joypad_motion", "axis": "left_x", "axis_value": 0.4})");
    in.send(R"({"type": "mouse_motion", "position": [20, 20]})");
    // Motions merge until a press or release comes between.
    CHECK(input::queued(in.world) == 2);
    in.send(R"({"type": "mouse_button", "button": "left", "position": [20, 20],
                "pressed": true})");
    in.send(R"({"type": "mouse_motion", "position": [25, 20]})");
    CHECK(input::queued(in.world) == 4);
    in.step();
    const auto events = input::events(in.world);
    REQUIRE(events.size() == 4);
    REQUIRE(events[0].relative.has_value());
    CHECK(*events[0].relative == Vec2{20.0F, 20.0F});
    CHECK(events[1].axis_value == doctest::Approx(0.4));
    CHECK(*events[3].relative == Vec2{5.0F, 0.0F});
    // A motion knows the buttons held.
    CHECK(events[3].button_mask == 1);
    CHECK(input::get_joy_axis(in.world, 0, input::JoyAxis::LeftX) == doctest::Approx(0.4));
}

TEST_CASE("release_pressed lets go of every key, button and action") {
    Input in;
    in.send(R"({"type": "key", "key": "Space", "pressed": true})");
    in.send(R"({"type": "joypad_motion", "axis": "left_x", "axis_value": 1})");
    input::action_press(in.world, "save");
    in.step();
    CHECK(input::is_action_pressed(in.world, "right"));
    input::release_pressed(in.world);
    in.step();
    CHECK_FALSE(input::is_key_pressed(in.world, input::Key::Space));
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    CHECK_FALSE(input::is_action_pressed(in.world, "right"));
    CHECK_FALSE(input::is_action_pressed(in.world, "save"));
    CHECK(input::is_action_just_released(in.world, "save"));
    CHECK(input::get_joy_axis(in.world, 0, input::JoyAxis::LeftX) == 0.0F);
}

TEST_CASE("actions can be rebound while the game runs") {
    Input in;
    input::Map& map = *input::map(in.world);
    map.action_erase_events("jump");
    map.action_add_event("jump", binding(R"({"type": "key", "key": "W"})"));
    in.send(R"({"type": "key", "key": "Space", "pressed": true})");
    in.step();
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    in.send(R"({"type": "key", "key": "W", "pressed": true})");
    in.step();
    CHECK(input::is_action_pressed(in.world, "jump"));
    // Installing a new map keeps the keys held but starts its actions released.
    input::install(in.world);
    CHECK(input::is_key_pressed(in.world, input::Key::Space));
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    CHECK_FALSE(input::map(in.world)->has_action("jump"));
}

TEST_CASE("an input snapshot puts the queue and what is held back") {
    Input in;
    in.send(R"({"type": "key", "key": "Space", "pressed": true})");
    const input::Snapshot before = input::save(in.world);
    in.step();
    CHECK(input::is_action_pressed(in.world, "jump"));
    input::restore(in.world, before);
    CHECK_FALSE(input::is_action_pressed(in.world, "jump"));
    CHECK(input::queued(in.world) == 1);
}

TEST_CASE("project.json input adds actions to Godot's ui_* ones") {
    const json::Value project = parse(R"({
        "format": 1, "name": "Platformer", "main_scene": "main.scene.json",
        "input": {
            "jump": {"events": [{"type": "key", "key": "Space"},
                                {"type": "joypad_button", "button": "a"}]},
            "ui_accept": {"deadzone": 0.4, "events": [{"type": "key", "key": "Enter"}]}
        },
        "input_devices": {"pointing": {"emulate_touch_from_mouse": true}}
    })");
    CHECK(runtime::validate_project(project).empty());
    auto parsed = runtime::parse_project(project);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
    const input::Map& map = parsed.value().input;
    REQUIRE(map.find("jump") != nullptr);
    CHECK(map.find("jump")->events.size() == 2);
    CHECK(map.find("jump")->deadzone == doctest::Approx(input::kDefaultDeadzone));
    // An entry with a built-in action's name replaces it.
    CHECK(map.find("ui_accept")->events.size() == 1);
    CHECK(map.find("ui_accept")->deadzone == doctest::Approx(0.4));
    CHECK(map.has_action("ui_cancel"));
    CHECK(parsed.value().input_devices.emulate_touch_from_mouse);
    CHECK(parsed.value().input_devices.emulate_mouse_from_touch);

    const json::Value typo = parse(R"({
        "format": 1, "name": "Platformer", "main_scene": "main.scene.json",
        "input": {"jump": {"events": [{"type": "key", "key": "Spcae"},
                                      {"type": "joypad_button", "button": "triangle"}]}}
    })");
    CHECK(messages(runtime::validate_project(typo)) ==
          std::vector<std::string>{
              R"(/input/jump/events/0/key: no key "Spcae"; did you mean "Space"?)",
              R"(/input/jump/events/1/button: no gamepad button "triangle"; expected one of a, )"
              "b, x, y, back, guide, start, left_stick, right_stick, left_shoulder, "
              "right_shoulder, dpad_up, dpad_down, dpad_left, dpad_right, misc1, paddle1, "
              "paddle2, paddle3, paddle4, touchpad"});
    CHECK_FALSE(runtime::parse_project(typo).ok());
    // Bindings say what presses an action, not when.
    const json::Value pressed = parse(R"({
        "format": 1, "name": "Platformer", "main_scene": "main.scene.json",
        "input": {"jump": {"events": [{"type": "key", "key": "Space", "pressed": true}]}}
    })");
    CHECK(messages(runtime::validate_project(pressed)) ==
          std::vector<std::string>{R"(/input/jump/events/0/pressed: unknown property "pressed")"});
}

TEST_CASE("SdlReader turns SDL events into input events") {
    Input in;
    input::SdlReader reader;
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_Z;
    key.key.scancode = SDL_SCANCODE_W; // a French keyboard's Z
    key.key.mod = SDL_KMOD_LCTRL;
    key.key.down = true;
    reader.read(key, in.world);

    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.button = SDL_BUTTON_X1;
    click.button.down = true;
    click.button.clicks = 2;
    click.button.x = 40.0F;
    click.button.y = 30.0F;
    reader.read(click, in.world);

    // SDL's own clicks from touches are skipped: the input state emulates them.
    SDL_Event touch_click = click;
    touch_click.button.which = SDL_TOUCH_MOUSEID;
    reader.read(touch_click, in.world);

    SDL_Event scroll{};
    scroll.type = SDL_EVENT_MOUSE_WHEEL;
    scroll.wheel.y = -2.0F;
    scroll.wheel.mouse_x = 40.0F;
    scroll.wheel.mouse_y = 30.0F;
    reader.read(scroll, in.world);

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;

    // Text an input method commits comes as characters, and what it composes as an ime event.
    SDL_Event editing{};
    editing.type = SDL_EVENT_TEXT_EDITING;
    editing.edit.text = "かな";
    editing.edit.start = 1;
    editing.edit.length = 1;
    reader.read(editing, in.world);
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.text = "仮名";
    reader.read(typed, in.world);

    in.step();
    const auto events = input::events(in.world);
    REQUIRE(events.size() == 7);
    CHECK(events[4].type == input::EventType::Ime);
    CHECK(events[4].text == "かな");
    CHECK(events[4].selection_start == 1);
    CHECK(events[4].selection_length == 1);
    CHECK(events[5].unicode == U'仮');
    CHECK(events[6].unicode == U'名');
    CHECK(events[6].key == input::Key::None);
    CHECK(events[0].key == input::Key::Z);
    CHECK(events[0].physical_key == input::Key::W);
    CHECK(events[0].modifiers.ctrl);
    CHECK(events[0].pressed);
    CHECK(events[1].button == input::MouseButton::Xbutton1);
    CHECK(events[1].double_click);
    CHECK(events[1].position == Vec2{40.0F, 30.0F});
    // The wheel presses and releases its button, as Godot's does.
    CHECK(events[2].button == input::MouseButton::WheelDown);
    CHECK(events[2].pressed);
    CHECK(events[2].factor == 2.0F);
    CHECK_FALSE(events[3].pressed);
    CHECK(input::is_physical_key_pressed(in.world, input::Key::W));

    // A window losing focus lets go of everything.
    reader.read(focus, in.world);
    in.step();
    CHECK_FALSE(input::is_key_pressed(in.world, input::Key::Z));
    CHECK(reader.gamepads() == 0);
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
};

} // namespace

TEST_CASE("input.inject queues events that the next step applies") {
    Session s;
    CHECK(s.ok("input.inject", R"({"events": [
        {"type": "key", "key": "Enter", "pressed": true},
        {"type": "mouse_motion", "position": [64, 32]}
    ]})") == parse(R"({"queued": 2, "tick": 0})"));
    CHECK(s.ok("input.state") ==
          parse(R"({"actions": {}, "mouse": {"position": [0, 0]}, "queued": 2})"));
    s.ok("step");
    CHECK(s.ok("input.state") == parse(R"({"tick": 0,
        "actions": {"ui_accept": {"pressed": true, "just_pressed": true}},
        "keys": ["Enter"], "mouse": {"position": [64, 32]}})"));
    CHECK(s.ok("input.state", R"({"actions": ["ui_accept", "ui_cancel"]})") == parse(R"({"tick": 0,
        "actions": {"ui_accept": {"pressed": true, "just_pressed": true},
                    "ui_cancel": {"pressed": false}},
        "keys": ["Enter"], "mouse": {"position": [64, 32]}})"));

    // A dry run or a failed batch puts the queue back, so a later step still applies it.
    s.ok("input.inject", R"({"events": [{"type": "action", "action": "ui_cancel",
                                         "pressed": true}]})");
    s.ok("step", R"({"dry_run": true})");
    CHECK(s.ok("input.state").find("queued")->as_int() == 1);
    CHECK_FALSE(s.call("batch", R"({"commands": [{"command": "step"},
        {"command": "entity.destroy", "params": {"id": "nobody"}}]})")
                    .ok);
    CHECK(s.ok("input.state").find("queued")->as_int() == 1);
    s.ok("step");
    CHECK(*s.ok("input.state").find("actions") == parse(R"({"ui_accept": {"pressed": true},
                    "ui_cancel": {"pressed": true, "just_pressed": true}})"));
}

TEST_CASE("input.inject checks every event and names misspelled actions") {
    Session s;
    const command::Reply reply = s.call("input.inject", R"({"events": [
        {"type": "key", "key": "Entr", "pressed": true},
        {"type": "action", "action": "ui_acept", "pressed": true},
        {"type": "screen_touch"}
    ]})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidParams);
    CHECK(reply.message.find(R"(/events/0/key: no key "Entr"; did you mean "Enter"?)") !=
          std::string::npos);
    CHECK(json::write(reply.data, json::Style::Compact).find("ui_accept") != std::string::npos);
    CHECK(json::write(reply.data, json::Style::Compact).find("/events/2") != std::string::npos);
    // Nothing was queued.
    CHECK(s.ok("input.state").find("queued") == nullptr);
    CHECK_FALSE(s.call("input.state", R"({"actions": ["jmp"]})").ok);
}

TEST_CASE("input.actions lists actions and their bindings") {
    Session s;
    CHECK(s.ok("input.actions", R"({"names": ["ui_focus_prev", "ui_left"]})") == parse(R"({
        "actions": {
            "ui_focus_prev": {"deadzone": 0.5, "events": [
                {"type": "key", "key": "Tab", "shift": true}]},
            "ui_left": {"deadzone": 0.5, "events": [
                {"type": "key", "key": "Left"},
                {"type": "joypad_button", "button": "dpad_left"},
                {"type": "joypad_motion", "axis": "left_x", "axis_value": -1}]}
        }
    })"));
    CHECK(s.ok("input.actions").find("actions")->as_object().size() == 13);
    const command::Reply typo = s.call("input.actions", R"({"names": ["ui_lef"]})");
    REQUIRE_FALSE(typo.ok);
    CHECK(typo.message.find(R"(did you mean "ui_left"?)") != std::string::npos);
}
