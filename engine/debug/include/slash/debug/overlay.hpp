#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/input/input.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The debug overlay: a panel over a running game for the person watching it. It shows how the
// game runs (frame rate, tick, entities and what the canvas drew) and which entities' collision
// shapes are under the mouse, and its buttons pause, play and step the game and show the
// collision shapes. The buttons run the same commands an agent would (pause, play and step),
// through the game's session, so a recording keeps them and nothing changes the world another
// way. The panel is UI: controls in a world of its own, drawn over the game with
// CanvasOptions::overlay.
//
//   debug::Overlay overlay(session.assets());
//   reader.set_filter([&](const input::Event& event) { return overlay.handle(event); });
//   // each tick, before the game steps:
//   overlay.update(session, {.fps = stats.fps, .frame_ms = stats.frame_ms_avg}, canvas, size);
//   // each frame:
//   options.collisions = overlay.collisions();
//   options.overlay = overlay.world();
namespace slash::debug {

// What the host knows about frames.
struct FrameStats {
    double fps = 0.0;
    double frame_ms = 0.0;            // the average time between frames
    double tick_seconds = 1.0 / 60.0; // what Step steps by
};

class Overlay {
public:
    explicit Overlay(asset::Assets& assets);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) = delete;
    Overlay& operator=(Overlay&&) = delete;

    // Whether the panel shows. It starts hidden; F3 shows and hides it.
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void set_visible(bool visible);
    // Whether the canvas should draw collision shapes (CanvasOptions::collisions), which the
    // Shapes button toggles. Shapes show while the panel is hidden too.
    [[nodiscard]] bool collisions() const noexcept { return collisions_; }
    void set_collisions(bool collisions);

    // Sees an input event before the game does, and returns true when the overlay takes it: F3
    // always, and while the panel shows, mouse buttons and touches that press on it (and their
    // releases and drags) and the mouse moving over it.
    bool handle(const input::Event& event);

    // Runs the panel for one tick in a viewport of the window's size: presses its buttons
    // through the session, then fills in its text from the frame stats, the session's world and
    // the canvas's counts. Does nothing while hidden.
    void update(command::Session& session, const FrameStats& frames,
                const canvas::CanvasRenderer& canvas, Vec2 viewport);

    // The panel's world, to draw over the game (CanvasOptions::overlay); nullptr while hidden.
    [[nodiscard]] const world::World* world() const { return visible_ ? &world_ : nullptr; }

    // The panel's text, a line per label, as it shows now.
    [[nodiscard]] std::vector<std::string> lines() const;

private:
    // Whether a point is on the panel, as the last update laid it out.
    [[nodiscard]] bool on_panel(Vec2 point) const;
    // Whether the shown panel takes a pointer event, which it passes to its controls.
    bool take(const input::Event& event);
    void sync_buttons(bool playing);

    world::ComponentRegistry components_;
    world::World world_;
    bool visible_ = false;
    bool collisions_ = false;
    std::uint32_t pressing_ = 0; // mouse buttons and fingers that pressed on the panel, as bits
    Vec2 mouse_;                 // over the game
    bool has_mouse_ = false;
    // The panel's rect in the viewport, from the last update.
    Vec2 panel_position_;
    Vec2 panel_size_;
};

// Adds debug.overlay, which shows or hides a running game's overlay and collision shapes. The
// overlay must outlive the registry's use.
void register_commands(command::Registry& registry, Overlay& overlay);

} // namespace slash::debug
