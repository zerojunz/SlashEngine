#include "slash/debug/overlay.hpp"

#include "slash/core/assert.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <array>
#include <cmath>
#include <string_view>
#include <tuple>
#include <utility>

namespace slash::debug {
namespace {

// The panel: stats, counts and what is under the mouse, over a row of buttons, in the top left
// corner. Its controls take no focus, so keys stay with the game.
constexpr std::string_view kPanel = R"({
  "format": 1,
  "entities": {
    "panel": {"components": {
      "Control": {"children": ["rows"], "offset_left": 8, "offset_top": 8,
                  "theme_override_styles": {"panel": {"bg_color": [0.06, 0.07, 0.09, 0.85],
                                                      "content_margin": 8,
                                                      "corner_radius": 4}}},
      "PanelContainer": {}}},
    "rows": {"components": {
      "BoxContainer": {"vertical": true},
      "Control": {"children": ["stats", "counts", "hover", "buttons"],
                  "theme_override_constants": {"separation": 4}}}},
    "stats": {"components": {"Label": {}}},
    "counts": {"components": {"Label": {}}},
    "hover": {"components": {"Control": {"visible": false}, "Label": {}}},
    "buttons": {"components": {
      "BoxContainer": {},
      "Control": {"children": ["play", "step", "shapes"]}}},
    "play": {"components": {"Button": {"text": "Pause"}, "Control": {"focus_mode": "none"}}},
    "step": {"components": {"Button": {"text": "Step", "disabled": true},
                            "Control": {"focus_mode": "none"}}},
    "shapes": {"components": {"Button": {"text": "Shapes", "toggle_mode": true},
                              "Control": {"focus_mode": "none"}}}
  }
})";

constexpr std::array<std::string_view, 3> kLabels = {"stats", "counts", "hover"};
// Fingers take the bits above the mouse buttons'.
constexpr unsigned kFingerBit = 16;
constexpr std::uint32_t kMouseBits = (1U << kFingerBit) - 1U;
// Entities named under the mouse, at most.
constexpr std::size_t kMaxHovered = 8;

// A number rounded to no decimals or one.
std::string fixed(double value, int decimals) {
    if (decimals == 0) {
        return std::to_string(std::llround(value));
    }
    const long long tenths = std::llround(value * 10.0);
    return std::to_string(tenths / 10) + "." + std::to_string(std::llabs(tenths % 10));
}

std::string count(std::size_t value, std::string_view one, std::string_view many) {
    return std::to_string(value) + " " + std::string(value == 1 ? one : many);
}

std::string name_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "entity " + std::to_string(entt::to_integral(entity)) : std::string(id);
}

} // namespace

Overlay::Overlay(asset::Assets& assets) : world_(components_) {
    world::register_builtin_components(components_);
    input::install(world_);
    // The panel's text is ASCII, in the engine's own font.
    ui::Settings settings;
    settings.allow_system_fallback = false;
    ui::install(world_, assets, settings);
    auto panel = json::parse(kPanel);
    SLASH_ASSERT(panel.ok(), "the debug overlay's panel parses");
    const auto loaded = world::load_scene(world_, panel.value());
    SLASH_ASSERT(loaded.ok(), "the debug overlay's panel loads");
}

Overlay::~Overlay() = default;

void Overlay::set_visible(bool visible) {
    visible_ = visible;
    pressing_ = 0;
}

void Overlay::set_collisions(bool collisions) {
    collisions_ = collisions;
    world_.ecs().get<world::Button>(world_.find("shapes")).button_pressed = collisions;
}

bool Overlay::on_panel(Vec2 point) const {
    return point.x >= panel_position_.x && point.y >= panel_position_.y &&
           point.x < panel_position_.x + panel_size_.x &&
           point.y < panel_position_.y + panel_size_.y;
}

bool Overlay::handle(const input::Event& event) {
    using input::EventType;
    if (event.type == EventType::Key &&
        (event.key == input::Key::F3 || event.physical_key == input::Key::F3)) {
        if (event.pressed && !event.echo) {
            set_visible(!visible_);
        }
        return true;
    }
    const bool taken = visible_ && take(event);
    // Where the mouse is over the game, for the hover line.
    if (!taken && (event.type == EventType::MouseButton || event.type == EventType::MouseMotion)) {
        mouse_ = event.position;
        has_mouse_ = true;
    }
    return taken;
}

bool Overlay::take(const input::Event& event) {
    using input::EventType;
    // A press on the panel is its own, and so are its release and the drags between.
    const auto press = [&](std::uint32_t bit) {
        bool taken = false;
        if (event.pressed) {
            taken = on_panel(event.position);
            pressing_ = taken ? pressing_ | bit : pressing_;
        } else {
            taken = (pressing_ & bit) != 0;
            pressing_ &= ~bit;
        }
        if (taken) {
            input::push(world_, event);
        }
        return taken;
    };
    switch (event.type) {
    case EventType::MouseButton:
        return press(1U << static_cast<unsigned>(event.button));
    case EventType::MouseMotion:
        // The panel's buttons see the mouse leave them too.
        input::push(world_, event);
        return on_panel(event.position) || (pressing_ & kMouseBits) != 0;
    case EventType::ScreenTouch:
        return event.index >= 0 && event.index < static_cast<int>(kFingerBit) &&
               press(1U << (kFingerBit + static_cast<unsigned>(event.index)));
    case EventType::ScreenDrag:
        if (event.index >= 0 && event.index < static_cast<int>(kFingerBit) &&
            (pressing_ & (1U << (kFingerBit + static_cast<unsigned>(event.index)))) != 0) {
            input::push(world_, event);
            return true;
        }
        return false;
    default:
        return false;
    }
}

void Overlay::sync_buttons(bool playing) {
    auto& ecs = world_.ecs();
    ecs.get<world::Button>(world_.find("play")).text = playing ? "Pause" : "Play";
    ecs.get<world::Button>(world_.find("step")).disabled = playing;
    ecs.get<world::Button>(world_.find("shapes")).button_pressed = collisions_;
}

void Overlay::update(command::Session& session, const FrameStats& frames,
                     const canvas::CanvasRenderer& canvas, Vec2 viewport) {
    if (!visible_) {
        return;
    }
    ui::set_viewport(world_, viewport);
    sync_buttons(session.playing());
    world_.step(frames.tick_seconds);

    // The buttons run the commands an agent would.
    const auto run = [&](std::string_view name, const json::Value& params) {
        const command::Reply reply = session.call(name, params);
        if (!reply.ok) {
            log::warn("a debug overlay button failed",
                      {{"command", name}, {"error", reply.message}});
        }
    };
    if (ui::was_pressed(world_, world_.find("play"))) {
        run(session.playing() ? "pause" : "play", json::Object{});
    }
    if (ui::was_pressed(world_, world_.find("step"))) {
        json::Value params;
        params["ticks"] = 1;
        params["tick_seconds"] = frames.tick_seconds;
        run("step", params);
    }
    if (ui::was_pressed(world_, world_.find("shapes"))) {
        collisions_ = world_.ecs().get<world::Button>(world_.find("shapes")).button_pressed;
    }
    sync_buttons(session.playing());

    world::World& game = session.world();
    auto& ecs = world_.ecs();
    ecs.get<world::Label>(world_.find("stats")).text =
        fixed(frames.fps, 0) + " fps · " + fixed(frames.frame_ms, 1) + " ms · tick " +
        std::to_string(game.tick()) + " · " + (session.playing() ? "playing" : "paused");
    std::string counts = count(game.size(), "entity", "entities");
    for (const auto& [drawn, one, many] :
         {std::tuple{canvas.sprites_drawn(), "sprite", "sprites"},
          std::tuple{canvas.tiles_drawn(), "tile", "tiles"},
          std::tuple{canvas.particles_drawn(), "particle", "particles"},
          std::tuple{canvas.lights_drawn(), "light", "lights"},
          std::tuple{canvas.controls_drawn(), "control", "controls"},
          std::tuple{canvas.shapes_drawn(), "shape", "shapes"},
          std::tuple{canvas.meshes_drawn(), "mesh", "meshes"}}) {
        if (drawn > 0) {
            counts += " · " + count(drawn, one, many);
        }
    }
    ecs.get<world::Label>(world_.find("counts")).text = std::move(counts);

    // What is under the mouse: its position, and the bodies and areas whose shapes hold it.
    const entt::entity hover = world_.find("hover");
    ecs.get<world::Control>(hover).visible = has_mouse_;
    if (has_mouse_) {
        std::string text = "mouse " + std::to_string(static_cast<long long>(std::floor(mouse_.x))) +
                           ", " + std::to_string(static_cast<long long>(std::floor(mouse_.y)));
        physics2d::QueryFilter filter;
        filter.collide_with_areas = true;
        const std::vector<entt::entity> under =
            physics2d::overlap_point(game, mouse_, filter, kMaxHovered);
        for (std::size_t i = 0; i < under.size(); ++i) {
            text += (i == 0 ? ": " : ", ") + name_of(game, under[i]);
        }
        ecs.get<world::Label>(hover).text = std::move(text);
    }

    // Where the panel is now, for the next events.
    const ui::Layout placed = ui::layout(world_, *ui::fonts(world_), viewport);
    if (const ui::Placed* panel = placed.find(world_.find("panel")); panel != nullptr) {
        panel_position_ = panel->transform.apply({0.0F, 0.0F});
        panel_size_ = panel->size;
    }
}

std::vector<std::string> Overlay::lines() const {
    std::vector<std::string> lines;
    const auto& ecs = world_.ecs();
    for (const std::string_view name : kLabels) {
        const entt::entity label = world_.find(name);
        if (const auto* control = ecs.try_get<world::Control>(label);
            control == nullptr || control->visible) {
            lines.push_back(ecs.get<world::Label>(label).text);
        }
    }
    return lines;
}

void register_commands(command::Registry& registry, Overlay& overlay) {
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["properties"]["visible"]["type"] = "boolean";
    params["properties"]["visible"]["description"] = "Show the panel.";
    params["properties"]["collisions"]["type"] = "boolean";
    params["properties"]["collisions"]["description"] = "Draw collision shapes.";
    registry.add({
        .name = "debug.overlay",
        .description = "Shows or hides the running game's debug panel (frame stats, pause, step, "
                       "what is under the mouse) and its collision shapes, for a person watching. "
                       "Returns both settings.",
        .capabilities = {command::Capability::Read},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler =
            [&overlay](command::Session& /*session*/, const json::Value& given) {
                if (const json::Value* visible = given.find("visible"); visible != nullptr) {
                    overlay.set_visible(visible->as_bool());
                }
                if (const json::Value* collisions = given.find("collisions");
                    collisions != nullptr) {
                    overlay.set_collisions(collisions->as_bool());
                }
                json::Value out;
                out["visible"] = overlay.visible();
                out["collisions"] = overlay.collisions();
                return command::Reply::success(std::move(out));
            },
    });
}

} // namespace slash::debug
