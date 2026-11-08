#pragma once

#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Gaussian splats, a research spike of SlashEngine's own (Godot has none): scenery captured as
// clouds of 3D Gaussians, after Kerbl et al.'s "3D Gaussian Splatting for Real-Time Radiance
// Field Rendering" (2023), as the .ply files its trainer and the tools after it (Postshot,
// Polycam, Luma, nerfstudio's splatfacto) write. The software device blends them over the world
// (see docs/reference/gaussian-splats.md); other devices draw none yet.
namespace slash::render {

// A cloud of 3D Gaussians, in the cloud's own space, values as they are drawn: each splat's
// center, the standard deviations along its axes (the file's are logarithms), its turn as a unit
// quaternion (w, x, y, z), its opacity from 0 to 1 (the file's goes through a sigmoid) and its
// color as spherical harmonics of the given degree (0 to 3), (degree + 1)^2 coefficients a
// channel, in sRGB from 0 to 1 once evaluated and 0.5 added, as the trainer's are.
struct GaussianSplats {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> scales;
    std::vector<std::array<float, 4>> rotations;
    std::vector<float> opacities;
    std::uint32_t sh_degree = 0;
    // Coefficient k of splat i in channel c (red, green, blue) at
    // ((i * (sh_degree + 1)^2) + k) * 3 + c.
    std::vector<float> sh;

    [[nodiscard]] std::size_t size() const noexcept { return positions.size(); }
    // How many coefficients a channel has: 1, 4, 9 or 16.
    [[nodiscard]] std::uint32_t coefficients() const noexcept {
        return (sh_degree + 1) * (sh_degree + 1);
    }
};

// Reads a 3D Gaussian Splatting .ply file: binary (little or big endian) or ASCII, whose vertex
// element has the float properties x, y, z, f_dc_0 to f_dc_2, opacity, scale_0 to scale_2 and
// rot_0 to rot_3, and 0, 9, 24 or 45 f_rest_ ones for degrees 0 to 3 (other properties, such as
// normals, are skipped). Errors name what is wrong.
[[nodiscard]] Result<GaussianSplats> read_splat_ply(std::string_view bytes);

// Writes splats as a binary little-endian .ply file that read_splat_ply reads back as written,
// the values as the trainer stores them (logarithms of scales, logits of opacities).
[[nodiscard]] std::string write_splat_ply(const GaussianSplats& splats);

// A cloud a frame draws: its splats, placed by transform (column-major, as Mat4), its colors
// evaluated up to sh_degree, and its splats' opacities times opacity.
struct SplatCloud3D {
    std::shared_ptr<const GaussianSplats> splats;
    std::array<float, 16> transform{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                    0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::uint32_t sh_degree = 3;
    float opacity = 1.0F;
};

// The splat clouds a frame blends over its opaque meshes, Scene3D::splats.
struct Splats3D {
    std::vector<SplatCloud3D> clouds;
};

} // namespace slash::render
