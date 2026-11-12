#pragma once

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/world/fluid.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
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
// keep, when given, picks which particles draw; light, when given, is fluid_color's light
// for each particle (a gas's fluid_transmittance and the scene's light on it).
void fluid_quads(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                 Vec3f right, Vec3f up, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                 std::vector<std::uint32_t>& indices,
                 const std::function<bool(std::size_t)>& keep = {},
                 std::span<const float> light = {});

// Where a liquid's surface is seen from: the camera's place (eye, its x right, y up and z back)
// and its projection (column-major, as render::Scene3D's), both after the shift drawing moves
// the world by.
struct FluidEye {
    world::Affine3D eye;
    std::array<float, 16> projection{};
};

// A liquid's surface as the camera sees it, made as screen-space fluid rendering makes it but
// on the CPU, into a mesh every device draws: the particles' spheres splatted into a depth
// image over the part of the screen they cover (the nearest sphere's front at each texel, and
// how much liquid lies behind it), the depths smoothed by a bilateral filter that keeps edges,
// and a vertex put back in the world at each covered texel, joined into triangles where
// neighbors are near in depth, with normals from the smoothed depths. Each vertex is the
// liquid's color, its alpha by how deep the liquid is behind it (the recipe's look depth), and
// whiter where fast particles foam. Spray (world::fluid_is_spray) is left out, for fluid_quads
// to draw. shift moves particles into the drawing's space.
void fluid_surface(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                   const FluidEye& eye, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                   std::vector<std::uint32_t>& indices);

// One corner of a 2D liquid's surface: where (pixels) and its color.
struct FluidCorner2D {
    Vec2f position;
    Vec4 color;
};
// A side-on liquid's surface in 2D: the particles summed into a smooth field over the pixels
// they cover and its inside traced as triangles by marching squares, colored the liquid's
// color, foam where fast particles are and lighter toward the surface, its alpha by how deep it
// is; spray is left out. Corners come three to a triangle.
void fluid_surface_2d(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                      std::vector<FluidCorner2D>& corners);

} // namespace slash::scene3d
