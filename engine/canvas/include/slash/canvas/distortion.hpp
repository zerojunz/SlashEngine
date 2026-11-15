#pragma once

#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Screen distortion: ScreenDistortion3D and ScreenDistortion2D components bend the picture of the
// world as CanvasRenderer shows it, through a grid of triangles whose texture coordinates are
// moved (no screen-reading shader, so every device draws it the same). Built with
// -ffp-contract=off and with no libm but square roots, so a grid is the same bits everywhere.
namespace slash::canvas {

enum class DistortionMode : std::uint8_t { Heat, Shockwave, Ripple, Lens };

// One distortion as the screen sees it, in the view's pixels.
struct Distortion {
    Vec2f center;
    float radius = 0.0F; // pixels
    DistortionMode mode = DistortionMode::Heat;
    float strength = 0.0F;
    float progress = 0.5F;
    float width = 0.25F;
    float frequency = 3.0F;
    float speed = 1.0F;
    std::uint32_t seed = 0; // from the entity's ID, for heat's noise
};

// The enabled distortions that touch a view of size pixels: 2D ones placed through their
// canvas's screen transform, 3D ones (with_3d) through the current Camera3D, in entity order.
[[nodiscard]] std::vector<Distortion> distortions(const world::World& world, Vec2f size,
                                                  bool with_3d);

// How far the picture at a point of the view moves, in pixels: what the screen shows there is
// what was drawn at point + the offset.
[[nodiscard]] Vec2f distortion_offset(std::span<const Distortion> distortions, Vec2f point,
                                      double time);

// The triangles that show a picture of the view's size bent by distortions: a grid of columns
// cells across (rows to keep them square) over the view, each corner's texture coordinate
// moved by distortion_offset. Empty when there are none.
[[nodiscard]] std::vector<render::Vertex2D> distortion_mesh(std::span<const Distortion> distortions,
                                                            Vec2f size, double time,
                                                            std::uint32_t columns = 96);

} // namespace slash::canvas
