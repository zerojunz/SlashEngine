#pragma once

#include "slash/core/math.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

#include <cstddef>
#include <entt/entity/entity.hpp>
#include <functional>
#include <unordered_map>
#include <vector>

// The canvases 2D entities and controls draw in, as Godot's viewport and its CanvasLayers, and
// Parallax2D, which scrolls with the camera. The math gives the same bits on every platform.
namespace slash::world {

class World;

// A canvas: the world, or a CanvasLayer's.
struct Canvas {
    entt::entity layer = entt::null; // the CanvasLayer, or null for the world
    int order = 0;                   // the CanvasLayer's layer; 0 for the world
    bool visible = true;
    // Its pixels to the screen's, as Godot's RendererViewport places a canvas: the camera's
    // canvas transform for the world, a CanvasLayer's own (canvas_layer_transform). Controls in
    // the world stay on the screen instead, as before canvas layers.
    Affine2D transform;
};

// Where a CanvasLayer draws, as Godot's RendererViewport draws its canvas: its offset, rotation
// and scale; following the viewport, after the world's canvas transform (the camera's) and then
// scaled by follow_viewport_scale around the middle of a screen of screen_size pixels.
[[nodiscard]] Affine2D canvas_layer_transform(const CanvasLayer& layer, const Affine2D& world,
                                              Vec2 screen_size);

// A world's canvases. Make one, use it and drop it, as a Tree2D; it keeps no reference to the
// tree it was made with.
class Canvases {
public:
    // The canvases as the view shows them on a screen of screen_size pixels.
    Canvases(const World& world, const Tree2D& tree, const CameraView& view, Vec2 screen_size);

    // In drawing order, as Godot stacks canvases: by layer, the world first among layer 0, and
    // CanvasLayers of one layer in the order their entities were made. The world is always
    // there.
    [[nodiscard]] const std::vector<Canvas>& all() const { return canvases_; }
    // The place in all() of the canvas an entity draws in: that of the CanvasLayer whose
    // children hold it or an entity above it (through Transform2D and Control children), else
    // the world's. An entity two CanvasLayers list draws in the first made.
    [[nodiscard]] std::size_t of(entt::entity entity) const;
    // The place in all() of the world's canvas.
    [[nodiscard]] std::size_t world_canvas() const { return world_; }
    // Whether any CanvasLayer holds anything: with none, everything is in the world.
    [[nodiscard]] bool layered() const { return !held_.empty(); }

private:
    std::vector<Canvas> canvases_;
    std::size_t world_ = 0;
    std::unordered_map<entt::entity, std::size_t> held_;     // listed by a layer
    std::unordered_map<entt::entity, entt::entity> parents_; // Transform2D's, then Control's
};

// The canvases of the world as they are now, for a screen of screen_size pixels: through the
// current camera with its state moved on by no time (camera_view).
[[nodiscard]] Canvases canvases(const World& world, const Tree2D& tree, Vec2 screen_size);

// The pixels an entity is placed in to the screen's: its canvas's transform, but none for a
// control in the world's canvas, which stays on the screen. The world's with entity null.
[[nodiscard]] Affine2D screen_transform(const World& world, const Canvases& canvases,
                                        entt::entity entity);

// Godot's Camera2D::_update_scroll: the screen_offset a view gives a Parallax2D, the middle of
// the screen in the world less half the screen.
[[nodiscard]] Vec2 parallax_screen_offset(const CameraView& view, Vec2 screen_size);

// Godot's Parallax2D::_update_scroll: the position a Parallax2D puts its entity at, whose own
// scale is scale, on a screen of screen_size pixels.
[[nodiscard]] Vec2 parallax_position(const Parallax2D& parallax, Vec2 scale, Vec2 screen_size);

// Adds the "parallax_2d" system, which each tick, right after "camera_2d", moves each
// Parallax2D's scroll_offset on by its autoscroll, takes screen_offset from the current camera
// for a screen of screen_size(world) pixels, and sets its Transform2D position.
void add_parallax_system(World& world, std::function<Vec2(const World&)> screen_size);

} // namespace slash::world
