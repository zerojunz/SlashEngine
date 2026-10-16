#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/debug/overlay.hpp"
#include "slash/input/input.hpp"
#include "slash/render/device.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace debug = slash::debug;
namespace input = slash::input;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using slash::Vec2;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

constexpr Vec2 kViewport{640.0F, 360.0F};

// A running game's session, canvas and overlay, as slash-cli run has them.
struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::unique_ptr<render::RenderDevice> device = render::create_null_device();
    std::unique_ptr<canvas::CanvasRenderer> canvas;
    std::unique_ptr<debug::Overlay> overlay;

    Game() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
        session->set_playing(true);
        canvas = std::make_unique<canvas::CanvasRenderer>(*device, session->assets());
        overlay = std::make_unique<debug::Overlay>(session->assets());
        debug::register_commands(commands, *overlay);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    // One host tick: the overlay, then the game if it plays, then a frame.
    void tick() {
        overlay->update(*session, {.fps = 60.0, .frame_ms = 16.67}, *canvas, kViewport);
        if (session->playing()) {
            session->world().step(1.0 / 60.0);
        }
        canvas::CanvasOptions options;
        options.collisions = overlay->collisions();
        options.overlay = overlay->world();
        REQUIRE(canvas->draw(session->world(), options).ok());
    }

    // The middle of a control of the panel, in the viewport.
    [[nodiscard]] Vec2 center_of(std::string_view id) const {
        const world::World& panel = *overlay->world();
        const slash::ui::Layout placed =
            slash::ui::layout(panel, *slash::ui::fonts(panel), kViewport);
        const slash::ui::Placed* control = placed.find(panel.find(id));
        REQUIRE(control != nullptr);
        return control->transform.apply(control->size * 0.5F);
    }

    // Clicks the left mouse button at a point, as SdlReader would hand the events over; returns
    // whether the overlay took them.
    bool click(Vec2 at) {
        input::Event press;
        press.type = input::EventType::MouseButton;
        press.button = input::MouseButton::Left;
        press.position = at;
        press.pressed = true;
        const bool took = overlay->handle(press);
        input::Event release = press;
        release.pressed = false;
        CHECK(overlay->handle(release) == took);
        return took;
    }

    void press_key(input::Key key, bool pressed) {
        input::Event event;
        event.type = input::EventType::Key;
        event.key = key;
        event.pressed = pressed;
        CHECK(overlay->handle(event));
    }
};

} // namespace

TEST_CASE("F3 shows and hides the debug overlay, and the game never sees it") {
    Game game;
    CHECK_FALSE(game.overlay->visible());
    CHECK(game.overlay->world() == nullptr);
    game.press_key(input::Key::F3, true);
    CHECK(game.overlay->visible());
    CHECK(game.overlay->world() != nullptr);
    game.press_key(input::Key::F3, false);
    CHECK(game.overlay->visible());
    game.press_key(input::Key::F3, true);
    CHECK_FALSE(game.overlay->visible());

    // Other keys are the game's.
    input::Event key;
    key.type = input::EventType::Key;
    key.key = input::Key::Space;
    key.pressed = true;
    CHECK_FALSE(game.overlay->handle(key));
}

TEST_CASE("the debug overlay shows frame stats, the tick and what was drawn") {
    Game game;
    game.ok("entity.create", R"({"id": "a"})");
    game.ok("entity.create", R"({"id": "b", "components": {"Label": {"text": "Score"}}})");
    game.overlay->set_visible(true);
    game.tick();
    game.tick();
    const std::vector<std::string> lines = game.overlay->lines();
    REQUIRE(lines.size() == 2); // the mouse hasn't been seen yet
    CHECK(lines[0] == "60 fps · 16.7 ms · tick 1 · playing");
    CHECK(lines[1] == "2 entities · 1 control");
    // The canvas drew the game's control; the overlay's controls aren't counted.
    CHECK(game.canvas->controls_drawn() == 1);
}

TEST_CASE("the debug overlay's buttons pause, step and play through the session's commands") {
    Game game;
    game.overlay->set_visible(true);
    game.tick();
    CHECK(game.session->world().tick() == 1);

    // Pause: the game stops, and Step becomes a button that does something.
    CHECK(game.click(game.center_of("play")));
    game.tick();
    CHECK_FALSE(game.session->playing());
    const std::uint64_t paused = game.session->world().tick();
    game.tick();
    CHECK(game.session->world().tick() == paused);
    CHECK(game.overlay->lines()[0].ends_with("paused"));

    // Step runs one tick, as the step command, so undo takes it back.
    CHECK(game.click(game.center_of("step")));
    game.tick();
    CHECK(game.session->world().tick() == paused + 1);
    const json::Value history = game.ok("history.get");
    REQUIRE_FALSE(history.find("undo")->as_array().empty());
    CHECK(history.find("undo")->as_array().back().as_string() == "step");

    // Play goes on.
    CHECK(game.click(game.center_of("play")));
    game.tick();
    CHECK(game.session->playing());
    CHECK(game.session->world().tick() == paused + 2);

    // Step does nothing while the game plays.
    CHECK(game.click(game.center_of("step")));
    game.tick();
    CHECK(game.session->world().tick() == paused + 3);
}

TEST_CASE("the Shapes button and debug.overlay turn collision shapes on and off") {
    Game game;
    game.ok("entity.create", R"({"id": "wall", "components": {
                                   "Transform2D": {"position": [320, 200]},
                                   "StaticBody2D": {},
                                   "CollisionShape2D": {"size": [40, 40]}}})");
    game.overlay->set_visible(true);
    game.tick();
    CHECK_FALSE(game.overlay->collisions());
    CHECK(game.canvas->shapes_drawn() == 0);

    CHECK(game.click(game.center_of("shapes")));
    game.tick();
    CHECK(game.overlay->collisions());
    game.tick();
    CHECK(game.canvas->shapes_drawn() == 1);
    CHECK(game.overlay->lines()[1] == "1 entity · 1 shape");

    // An agent turns them off and hides the panel; shapes stay off when it shows again.
    CHECK(json::write(game.ok("debug.overlay", R"({"collisions": false, "visible": false})"),
                      json::Style::Compact) == R"({"collisions":false,"visible":false})");
    CHECK(game.overlay->world() == nullptr);
    game.tick();
    CHECK(game.canvas->shapes_drawn() == 0);
    CHECK(json::write(game.ok("debug.overlay", R"({"visible": true})"), json::Style::Compact) ==
          R"({"collisions":false,"visible":true})");
    game.tick();
    CHECK(game.click(game.center_of("shapes")));
    game.tick();
    CHECK(game.overlay->collisions());
}

TEST_CASE("the debug overlay takes the pointer on its panel and names what is under the mouse") {
    Game game;
    game.ok("entity.create", R"({"id": "wall", "components": {
                                   "Transform2D": {"position": [320, 200]},
                                   "StaticBody2D": {},
                                   "CollisionShape2D": {"size": [40, 40]}}})");
    game.ok("entity.create", R"({"id": "zone", "components": {
                                   "Transform2D": {"position": [330, 200]},
                                   "Area2D": {},
                                   "CollisionShape2D": {"shape": "circle", "radius": 30}}})");
    game.overlay->set_visible(true);
    game.tick();

    // Off the panel, clicks are the game's; on it, they are the overlay's.
    CHECK_FALSE(game.click({320.0F, 200.0F}));
    CHECK(game.click(game.center_of("stats")));

    // A press on the panel keeps its release and drags, wherever they land.
    input::Event press;
    press.type = input::EventType::MouseButton;
    press.button = input::MouseButton::Left;
    press.position = game.center_of("stats");
    press.pressed = true;
    CHECK(game.overlay->handle(press));
    input::Event drag;
    drag.type = input::EventType::MouseMotion;
    drag.position = {600.0F, 300.0F};
    CHECK(game.overlay->handle(drag));
    input::Event release = press;
    release.position = drag.position;
    release.pressed = false;
    CHECK(game.overlay->handle(release));

    // The mouse over the game: its position and the bodies and areas under it, by ID.
    input::Event motion;
    motion.type = input::EventType::MouseMotion;
    motion.position = {321.5F, 200.0F};
    CHECK_FALSE(game.overlay->handle(motion));
    game.tick();
    REQUIRE(game.overlay->lines().size() == 3);
    CHECK(game.overlay->lines()[2] == "mouse 321, 200: wall, zone");
    motion.position = {100.0F, 320.0F};
    CHECK_FALSE(game.overlay->handle(motion));
    game.tick();
    CHECK(game.overlay->lines()[2] == "mouse 100, 320");

    // Hidden, it takes nothing but F3.
    const Vec2 stats = game.center_of("stats");
    game.overlay->set_visible(false);
    CHECK_FALSE(game.click(stats));
}

TEST_CASE("the canvas draws the debug panel over the game while it shows") {
    Game game;
    const std::unique_ptr<render::RenderDevice> device = render::create_software_device();
    canvas::CanvasRenderer renderer(*device, game.session->assets());
    auto target = device->create_texture({.width = 640, .height = 360, .render_target = true});
    REQUIRE(target.ok());
    const auto corner = [&] {
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};
        options.width = kViewport.x;
        options.height = kViewport.y;
        options.overlay = game.overlay->world();
        REQUIRE(renderer.draw(game.session->world(), options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        const std::size_t at = ((12 * std::size_t{image.value().width}) + 12) * 4;
        return std::vector<std::uint8_t>(image.value().pixels.begin() + static_cast<long>(at),
                                         image.value().pixels.begin() + static_cast<long>(at) + 3);
    };
    CHECK(corner() == std::vector<std::uint8_t>{255, 255, 255});
    game.overlay->set_visible(true);
    game.tick();
    // The panel's dark background, at 85% over white.
    const std::vector<std::uint8_t> shown = corner();
    CHECK(shown[0] < 60);
    CHECK(shown[2] < 70);
    CHECK(renderer.controls_drawn() == 0); // the panel isn't the game's
}
