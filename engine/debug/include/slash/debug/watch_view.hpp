#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// What "slash-cli watch" draws over an agent's world: a bar saying whose session it is and what
// the agent did last, and an outline round each entity the agent's commands touched, where it
// shows in the window, fading a few seconds after. Outlines are green round entities the agent
// made and amber round those it changed, each named by its ID. Like the debug overlay, it is UI:
// controls in a world of its own, drawn over the watched world with CanvasOptions::overlay.
//
//   debug::WatchView view(session.assets());
//   for (const json::Value& line : new_journal_lines) view.touch(line, seconds);
//   view.set_status(work::describe(new_journal_lines.back()));
//   // each frame: draw the watched world with CanvasOptions::contents, then
//   view.update(watched, session.assets(), contents, size, seconds, world_changed);
//   options.overlay = view.world();
namespace slash::debug {

// An entity a watcher outlines, in window pixels.
struct Outline {
    std::string id;
    bool added = false; // made by the agent, rather than changed
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float alpha = 1.0F; // 1 when touched, down to 0 as it fades

    bool operator==(const Outline&) const = default;
};

// How long an outline shows at full strength, then how long it takes to fade, in seconds.
inline constexpr double kOutlineHold = 3.0;
inline constexpr double kOutlineFade = 2.0;

class WatchView {
public:
    explicit WatchView(asset::Assets& assets);
    ~WatchView();
    WatchView(const WatchView&) = delete;
    WatchView& operator=(const WatchView&) = delete;
    WatchView(WatchView&&) = delete;
    WatchView& operator=(WatchView&&) = delete;

    // Marks an entity as touched at `seconds`, on whatever clock the host keeps.
    void touch(std::string_view id, bool added, double seconds);
    // Touches the entities a journal line (slash/command/work.hpp) says a command or an event
    // added or changed; those it removed are gone, so there is nothing to outline.
    void touch(const json::Value& line, double seconds);
    // The bar's text, such as the session and the agent's last command.
    void set_status(std::string text);

    // Places each touched entity's outline where the frame drew it: a 2D entity's (and its
    // children's) sprites, tiles, polygons and particles in the contents CanvasOptions::contents
    // filled, else its origin on the screen; a 3D entity's (and its children's) meshes and shapes
    // as the current Camera3D sees them. Lays out the outlines and the bar in a viewport of this
    // size. world_changed says the watched world isn't the one the last call saw, so its 3D
    // geometry is read again.
    void update(const world::World& watched, asset::Assets& assets,
                const canvas::FrameContents& contents, Vec2 viewport, double seconds,
                bool world_changed);

    // The outlines the last update placed, oldest touch first.
    [[nodiscard]] const std::vector<Outline>& outlines() const noexcept { return outlines_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    // The view's controls, to draw over the watched world (CanvasOptions::overlay).
    [[nodiscard]] const world::World* world() const { return &world_; }

private:
    struct Touch {
        std::string id;
        bool added = false;
        double seconds = 0.0;
    };
    // Each 3D entity's own geometry's box in the world, as navigation::world_geometry reads it.
    struct Box3 {
        Vec3 min;
        Vec3 max;
    };

    void lay_out(Vec2 viewport);

    world::ComponentRegistry components_;
    world::World world_;
    std::vector<Touch> touches_;
    std::vector<Outline> outlines_;
    std::string status_;
    std::unordered_map<entt::entity, Box3> boxes_;
    std::size_t shown_ = 0; // outline controls made so far
};

} // namespace slash::debug
