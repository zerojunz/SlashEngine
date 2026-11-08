#pragma once

#include "slash/render/device.hpp"
#include "slash/render/splats.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::jobs {
class Pool;
} // namespace slash::jobs

// Blending Gaussian splats over a frame on the CPU, as 3D Gaussian Splatting's rasterizer does:
// each splat's covariance projected to the screen (Zwicker et al.'s EWA splatting, with the
// Jacobian of the frame's whole view projection, so orthogonal and jittered projections work,
// and 0.3 pixels squared added as the trainer's low-pass filter), its color evaluated from its
// spherical harmonics toward the camera, then the splats sorted by their distance along the view,
// nearest first (ties by cloud, then splat), put in tiles of 16 pixels and blended front to back
// in each pixel until under 1/10000 of the light behind shows. A splat whose center is behind the
// opaque depth at a pixel leaves it alone. Built with -ffp-contract=off, with slash::exp, so a
// frame is the same everywhere and on any number of threads.
namespace slash::render::detail {

inline constexpr std::uint32_t kSplatTile = 16;

// What run_splats sees a frame through: the target's size, the view projection meshes draw
// with (with TAA's or FSR 2's jitter) and the camera's place in the world.
struct SplatFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Mat4 view_projection{};
    std::array<float, 3> camera{};
};

// The splats' blend, 4 floats a pixel in rows from the top: the light they add (linear, already
// weighed by how much shows of each) and how much of what is behind them still shows, 1 where no
// splat is. depth is the opaque meshes' reversed depth, a float a pixel (0 for none).
[[nodiscard]] std::vector<float> run_splats(const Splats3D& splats, const SplatFrame& frame,
                                            std::span<const float> depth, jobs::Pool* pool);

// A splat's color seen from direction (a unit vector from the camera toward it, in the cloud's
// space) with degree's coefficients, in sRGB, as the trainer's computeColorFromSH: 0.5 added and
// clamped at 0.
[[nodiscard]] std::array<float, 3> splat_color(const GaussianSplats& splats, std::size_t index,
                                               std::uint32_t degree,
                                               const std::array<float, 3>& direction);

} // namespace slash::render::detail
