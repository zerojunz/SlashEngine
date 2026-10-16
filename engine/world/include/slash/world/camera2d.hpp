#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

#include <entt/entity/entity.hpp>
#include <functional>

// What part of the 2D world a screen shows, as Godot's Camera2D and its viewport's canvas
// transform. The math gives the same bits on every platform.
namespace slash::world {

class World;

// What a camera keeps between ticks, as Godot's Camera2D: where its drag margins hold it, where
// smoothing has brought it and the angle it shows.
struct CameraState {
    bool first = true; // not updated yet: the next update puts it where its entity is
    Vec2 camera_pos;
    Vec2 smoothed_camera_pos;
    float camera_angle = 0.0F;
    // The drag offsets at the last update; a change moves the camera to them at once, as
    // Godot's setters do.
    float drag_horizontal_offset = 0.0F;
    float drag_vertical_offset = 0.0F;
};

// Godot's Camera2D::get_camera_transform: the transform from world pixels to the pixels of a
// screen_size screen, for a camera whose entity's global transform is global. It moves the state
// on: drag margins and limit smoothing, and position and rotation smoothing by delta seconds (0
// leaves smoothing where it is).
[[nodiscard]] Affine2D camera_transform(const Camera2D& camera, CameraState& state,
                                        const Transform2D& global, Vec2 screen_size, float delta);

// The camera the world is seen through, as Godot's Viewport::get_camera_2d: the first enabled
// Camera2D with a Transform2D in drawing order (Tree2D::draw_key), or null.
[[nodiscard]] entt::entity current_camera(const World& world, const Tree2D& tree);

// What a screen shows of the world.
struct CameraView {
    entt::entity camera = entt::null; // the current camera, or null with none
    // World pixels to screen pixels, as Godot's canvas transform: the identity with no camera.
    // Its inverse takes a point on the screen, such as the mouse's, into the world.
    Affine2D canvas;
};

// The view through the current camera as it is now: its state moved on by no time, so an edit
// since the last tick shows at once.
[[nodiscard]] CameraView camera_view(const World& world, const Tree2D& tree, Vec2 screen_size);

// Adds the "camera_2d" system, which each tick moves the current camera's state on by the tick,
// for a screen of screen_size(world) pixels. States are kept with the world by entity ID, so
// restoring a copy of the world leaves them alone, and reset_state forgets them.
void add_camera_system(World& world, std::function<Vec2(const World&)> screen_size);

// Godot's Camera2D::reset_smoothing: the camera catches up with its smoothing at once, so one
// that was moved far doesn't glide there.
void reset_camera_smoothing(World& world, entt::entity camera);

} // namespace slash::world
