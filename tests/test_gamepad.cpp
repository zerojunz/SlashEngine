#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/input/input.hpp"
#include "slash/input/sdl.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace slash;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), text);
    return std::move(value).value();
}

input::Event event(std::string_view text) {
    auto parsed = input::parse_event(parse(text), input::Use::Event);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

input::Event binding(std::string_view text) {
    auto parsed = input::parse_event(parse(text), input::Use::Binding);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

struct Pads {
    world::ComponentRegistry components;
    world::World world{components};

    Pads() {
        input::Map map = input::Map::with_defaults();
        map.add_action("jump");
        map.action_add_event("jump", binding(R"({"type": "key", "key": "Space"})"));
        map.action_add_event("jump", binding(R"({"type": "joypad_button", "button": "a"})"));
        map.add_action("fire");
        map.action_add_event("fire", binding(R"({"type": "mouse_button", "button": "left"})"));
        map.action_add_event("fire", binding(R"({"type": "joypad_motion", "axis": "trigger_right",
                                         "axis_value": 1})"));
        input::install(world, std::move(map));
    }

    void send(std::string_view text) { input::push(world, event(text)); }
    void step() { world.step(kTick); }
};

} // namespace

TEST_CASE("gamepad connection and sensor events read and write as JSON") {
    const input::Event connected = event(R"({"type": "joypad_connection", "device": 1,
        "connected": true, "name": "DualSense", "gamepad": "ps5", "can_rumble": true,
        "has_motion": true})");
    CHECK(connected.type == input::EventType::JoypadConnection);
    CHECK(connected.pressed);
    CHECK(connected.text == "DualSense");
    CHECK(connected.gamepad == input::GamepadType::Ps5);
    CHECK(connected.can_rumble);
    CHECK(connected.has_motion);
    CHECK(input::event_json(connected, input::Use::Event) ==
          parse(R"({"type": "joypad_connection", "device": 1, "connected": true,
                    "name": "DualSense", "gamepad": "ps5", "can_rumble": true,
                    "has_motion": true})"));

    const input::Event tilted =
        event(R"({"type": "joypad_sensor", "gyro": [0.5, 0, 0], "accel": [0, 9.81, 0]})");
    CHECK(tilted.gyro == Vec3f{0.5F, 0.0F, 0.0F});
    CHECK(tilted.accel == Vec3f{0.0F, 9.81F, 0.0F});

    // What each needs, and what isn't theirs.
    CHECK_FALSE(input::validate_event(parse(R"({"type": "joypad_connection"})"), input::Use::Event)
                    .empty());
    CHECK_FALSE(
        input::validate_event(parse(R"({"type": "joypad_sensor"})"), input::Use::Event).empty());
    CHECK_FALSE(input::validate_event(
                    parse(R"({"type": "joypad_connection", "connected": true, "gamepad": "ps6"})"),
                    input::Use::Event)
                    .empty());
    CHECK_FALSE(input::validate_event(
                    parse(R"({"type": "joypad_sensor", "gyro": [0, 0, 0], "pressed": true})"),
                    input::Use::Event)
                    .empty());
    // Neither binds to an action.
    CHECK_FALSE(
        input::validate_event(parse(R"({"type": "joypad_sensor"})"), input::Use::Binding).empty());
    CHECK(input::parse_gamepad_type("switch_pro") == input::GamepadType::SwitchPro);
    CHECK_FALSE(input::parse_gamepad_type("Switch").has_value());
}

TEST_CASE("the input state keeps connected gamepads and what their sensors read") {
    Pads pads;
    pads.send(R"({"type": "joypad_connection", "device": 0, "connected": true,
                  "name": "Xbox Wireless Controller", "gamepad": "xbox_one",
                  "can_rumble": true})");
    pads.send(R"({"type": "joypad_connection", "device": 1, "connected": true,
                  "name": "DualSense", "gamepad": "ps5", "has_motion": true})");
    pads.send(R"({"type": "joypad_sensor", "device": 1, "gyro": [0, 1, 0], "accel": [0, 9, 0]})");
    // Readings before a tick merge into the last.
    pads.send(R"({"type": "joypad_sensor", "device": 1, "gyro": [0, 2, 0], "accel": [0, 9.8, 0]})");
    CHECK(input::queued(pads.world) == 3);
    pads.step();

    const std::vector<input::Gamepad> connected = input::gamepads(pads.world);
    REQUIRE(connected.size() == 2);
    CHECK(connected[0].name == "Xbox Wireless Controller");
    CHECK(connected[0].type == input::GamepadType::XboxOne);
    CHECK(connected[0].can_rumble);
    CHECK_FALSE(connected[0].has_motion);
    CHECK(connected[1].type == input::GamepadType::Ps5);
    CHECK(connected[1].gyro == Vec3f{0.0F, 2.0F, 0.0F});
    CHECK(input::gamepad_gyro(pads.world, 1) == Vec3f{0.0F, 2.0F, 0.0F});
    CHECK(input::gamepad_accelerometer(pads.world, 1) == Vec3f{0.0F, 9.8F, 0.0F});
    CHECK(input::gamepad_gyro(pads.world, 0) == Vec3f{});

    const json::Value state = input::state_json(pads.world);
    const json::Array& joypads = state.find("joypads")->as_array();
    REQUIRE(joypads.size() == 2);
    CHECK(joypads[1] == parse(R"({"device": 1, "name": "DualSense", "gamepad": "ps5",
                                  "has_motion": true, "gyro": [0, 2, 0],
                                  "accel": [0, 9.8, 0]})"));

    // Unplugged mid-press, a gamepad lets go of what it held.
    pads.send(R"({"type": "joypad_button", "device": 1, "button": "a", "pressed": true})");
    pads.step();
    CHECK(input::is_action_pressed(pads.world, "jump"));
    pads.send(R"({"type": "joypad_connection", "device": 1, "connected": false})");
    pads.step();
    CHECK_FALSE(input::is_action_pressed(pads.world, "jump"));
    CHECK(input::is_action_just_released(pads.world, "jump"));
    CHECK_FALSE(input::is_joy_button_pressed(pads.world, 1, input::JoyButton::A));
    CHECK_FALSE(input::gamepad(pads.world, 1).has_value());
    CHECK(input::gamepad_gyro(pads.world, 1) == Vec3f{});
    CHECK(input::gamepads(pads.world).size() == 1);
}

TEST_CASE("rumble runs for its seconds and the platform takes each change once") {
    Pads pads;
    input::rumble(pads.world, 0, {.low = 0.25F, .high = 1.5F, .seconds = 0.1F});
    // Strengths are clamped to 0 to 1.
    CHECK(input::rumble_of(pads.world, 0) ==
          input::Rumble{.low = 0.25F, .high = 1.0F, .seconds = 0.1F});
    auto changes = input::take_rumbles(pads.world);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].high == 1.0F);
    CHECK(input::take_rumbles(pads.world).empty());

    const json::Value state = input::state_json(pads.world);
    CHECK(*state.find("joypads")->as_array()[0].find("rumble") ==
          parse(R"({"low": 0.25, "high": 1, "seconds": 0.1})"));

    // Six ticks of 1/60 second wear it down to nothing.
    for (int i = 0; i < 6; ++i) {
        CHECK(input::rumble_of(pads.world, 0).low == 0.25F);
        pads.step();
    }
    CHECK(input::rumble_of(pads.world, 0) == input::Rumble{});
    // Running out needs no change: the motors stop on their own.
    CHECK(input::take_rumbles(pads.world).empty());

    input::rumble(pads.world, 2, {.left_trigger = 0.5F, .right_trigger = 0.5F, .seconds = 1.0F});
    input::stop_rumble(pads.world, 2);
    CHECK(input::rumble_of(pads.world, 2) == input::Rumble{});
    changes = input::take_rumbles(pads.world);
    REQUIRE(changes.size() == 1);
    CHECK(changes[2] == input::Rumble{});
}

TEST_CASE("button labels and glyphs follow the kind of gamepad") {
    using input::GamepadType;
    using input::JoyButton;
    const auto label = [](GamepadType type, JoyButton button) {
        return input::joy_button_label(type, button);
    };
    CHECK(label(GamepadType::XboxOne, JoyButton::A) == input::ButtonLabel{"A", "xbox_a"});
    CHECK(label(GamepadType::Ps5, JoyButton::A) ==
          input::ButtonLabel{"Cross", "playstation_cross"});
    // Nintendo's bottom face button is B, the right one A.
    CHECK(label(GamepadType::SwitchPro, JoyButton::A) == input::ButtonLabel{"B", "nintendo_b"});
    CHECK(label(GamepadType::SwitchPro, JoyButton::B) == input::ButtonLabel{"A", "nintendo_a"});
    CHECK(label(GamepadType::Standard, JoyButton::Y) ==
          input::ButtonLabel{"North", "gamepad_north"});
    CHECK(label(GamepadType::Ps5, JoyButton::Back).label == "Create");
    CHECK(label(GamepadType::Ps4, JoyButton::Back).label == "Share");
    CHECK(label(GamepadType::Xbox360, JoyButton::Back).label == "Back");
    CHECK(label(GamepadType::XboxOne, JoyButton::Start).label == "Menu");
    CHECK(label(GamepadType::Ps4, JoyButton::LeftShoulder).label == "L1");
    CHECK(label(GamepadType::Ps5, JoyButton::Guide) ==
          input::ButtonLabel{"PS", "playstation_guide"});
    CHECK(label(GamepadType::XboxOne, JoyButton::DpadUp).glyph == "xbox_dpad_up");
    CHECK(input::joy_axis_label(GamepadType::SwitchPro, input::JoyAxis::TriggerRight, 1.0F) ==
          input::ButtonLabel{"ZR", "nintendo_zr"});
    CHECK(input::joy_axis_label(GamepadType::XboxOne, input::JoyAxis::LeftY, -1.0F) ==
          input::ButtonLabel{"Left Stick Up", "xbox_left_stick_up"});
    CHECK(input::binding_label(binding(R"({"type": "key", "key": "S", "ctrl": true})")) ==
          input::ButtonLabel{"Ctrl+S", "key_s"});
    CHECK(input::binding_label(binding(R"({"type": "key", "key": "Kp Enter"})")).glyph ==
          "key_kp_enter");
    CHECK(input::binding_label(binding(R"({"type": "mouse_button", "button": "right"})")) ==
          input::ButtonLabel{"Right Click", "mouse_right"});
}

TEST_CASE("prompts follow the input the player last used") {
    Pads pads;
    CHECK(input::last_input(pads.world) == input::LastInput{});
    auto prompt = input::action_prompt(pads.world, "jump");
    REQUIRE(prompt.has_value());
    CHECK(prompt->label == input::ButtonLabel{"Space", "key_space"});
    // Keyboard and mouse prompts take a key first, then a mouse button.
    CHECK(input::action_prompt(pads.world, "fire")->label.label == "Left Click");

    pads.send(R"({"type": "joypad_connection", "device": 1, "connected": true, "gamepad": "ps4"})");
    // A stick that drifts isn't a choice to use the gamepad; a press is.
    pads.send(R"({"type": "joypad_motion", "device": 1, "axis": "left_x", "axis_value": 0.2})");
    pads.step();
    CHECK(input::last_input(pads.world).kind == input::InputKind::Keyboard);
    pads.send(R"({"type": "joypad_button", "device": 1, "button": "a", "pressed": true})");
    pads.step();
    CHECK(input::last_input(pads.world) ==
          input::LastInput{.kind = input::InputKind::Gamepad, .device = 1});
    CHECK(input::action_prompt(pads.world, "jump")->label.label == "Cross");
    CHECK(input::action_prompt(pads.world, "fire")->label ==
          input::ButtonLabel{"R2", "playstation_r2"});
    CHECK(*input::state_json(pads.world).find("last_input") ==
          parse(R"({"kind": "gamepad", "device": 1})"));

    pads.send(R"({"type": "mouse_button", "button": "left", "position": [1, 1], "pressed": true})");
    pads.step();
    CHECK(input::last_input(pads.world).kind == input::InputKind::Mouse);
    CHECK(input::action_prompt(pads.world, "jump")->label.label == "Space");
    // Asked for a kind of gamepad, wherever the player is.
    CHECK(input::action_prompt(*input::map(pads.world), "jump", input::InputKind::Gamepad,
                               input::GamepadType::SwitchPro)
              ->label.label == "B");
    CHECK_FALSE(input::action_prompt(pads.world, "dash").has_value());
}

TEST_CASE("input.actions adds prompts and input.inject connects gamepads") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::Session session(components, commands, command::SessionOptions{});
    const auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };
    // ui_select is the top face button, or Space.
    auto reply = call("input.actions", R"({"names": ["ui_select"], "prompt": "switch_pro"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(*reply.value.find("actions")->find("ui_select")->find("prompt") ==
          parse(R"({"label": "X", "glyph": "nintendo_x"})"));
    reply = call("input.actions", R"({"names": ["ui_select", "ui_cancel"], "prompt": "keyboard"})");
    const json::Value* actions = reply.value.find("actions");
    CHECK(actions->find("ui_select")->find("prompt")->find("glyph")->as_string() == "key_space");
    // Nothing on a gamepad cancels.
    reply = call("input.actions", R"({"names": ["ui_cancel"], "prompt": "ps5"})");
    CHECK(reply.value.find("actions")->find("ui_cancel")->find("prompt")->is_null());
    CHECK_FALSE(call("input.actions", R"({"prompt": "ps6"})").ok);

    reply = call("input.inject", R"({"events": [
        {"type": "joypad_connection", "connected": true, "gamepad": "xbox_one"},
        {"type": "joypad_button", "button": "y", "pressed": true}]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    session.world().step(kTick);
    reply = call("input.actions", R"({"names": ["ui_select"], "prompt": "last"})");
    CHECK(*reply.value.find("actions")->find("ui_select")->find("prompt") ==
          parse(R"({"label": "Y", "glyph": "xbox_y"})"));
}

TEST_CASE("SdlReader reads a virtual gamepad's type and sensors and plays rumble") {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        MESSAGE("no gamepad subsystem: " << SDL_GetError());
        return;
    }
    struct Felt {
        Uint16 low = 0;
        Uint16 high = 0;
        int calls = 0;
    } felt;
    std::array<SDL_VirtualJoystickSensorDesc, 2> sensors{
        {{SDL_SENSOR_GYRO, 200.0F}, {SDL_SENSOR_ACCEL, 200.0F}}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x054C;  // Sony
    desc.product_id = 0x0CE6; // DualSense
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors = static_cast<Uint16>(sensors.size());
    desc.sensors = sensors.data();
    desc.name = "Virtual DualSense";
    desc.userdata = &felt;
    desc.Rumble = [](void* userdata, Uint16 low, Uint16 high) {
        auto* into = static_cast<Felt*>(userdata);
        into->low = low;
        into->high = high;
        ++into->calls;
        return true;
    };
    desc.SetSensorsEnabled = [](void* /*userdata*/, bool /*enabled*/) { return true; };
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    REQUIRE_MESSAGE(id != 0, SDL_GetError());
    {
        Pads pads;
        input::SdlReader reader;
        const auto pump = [&] {
            SDL_UpdateJoysticks();
            SDL_PumpEvents();
            SDL_Event sdl{};
            while (SDL_PollEvent(&sdl)) {
                reader.read(sdl, pads.world);
            }
        };
        pump();
        REQUIRE(reader.gamepads() == 1);
        pads.step();
        const auto pad = input::gamepad(pads.world, 0);
        REQUIRE(pad.has_value());
        CHECK(pad->name == "Virtual DualSense");
        CHECK(pad->type == input::GamepadType::Ps5);
        CHECK(pad->can_rumble);
        CHECK(pad->has_motion);

        SDL_Joystick* joystick = SDL_GetJoystickFromID(id);
        REQUIRE(joystick != nullptr);
        const std::array<float, 3> gyro{0.0F, 1.5F, 0.0F};
        CHECK(SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, 1, gyro.data(), 3));
        pump();
        pads.step();
        CHECK(input::gamepad_gyro(pads.world, 0) == Vec3f{0.0F, 1.5F, 0.0F});

        input::rumble(pads.world, 0, {.low = 1.0F, .high = 0.5F, .seconds = 0.5F});
        reader.update(pads.world);
        CHECK(felt.calls >= 1);
        CHECK(felt.low == 65535);
        CHECK(felt.high == 32768);
    }
    CHECK(SDL_DetachVirtualJoystick(id));
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
