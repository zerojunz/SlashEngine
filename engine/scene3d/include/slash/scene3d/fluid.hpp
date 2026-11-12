#pragma once

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/world/fluid.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

// Drawing fluids (slash/world/fluid.hpp) as discs facing the camera, shared by 3D drawing and
// the canvas's 2D drawing.
namespace slash::scene3d {

// The soft round disc each fluid particle draws with, made when first wanted.
inline constexpr std::string_view kFluidDisc = "made:fluid/disc";
[[nodiscard]] render::Image fluid_disc_image();

// A quad facing the viewer (right and up, a unit apart) at each particle drawn, size
// fluid_draw_size wide and colored by fluid_color, moved by shift; particles with no alpha draw
// nothing. Facing the viewer, a liquid's normals bulge out round each disc so light rounds it.
void fluid_quads(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                 Vec3f right, Vec3f up, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                 std::vector<std::uint32_t>& indices);

} // namespace slash::scene3d
