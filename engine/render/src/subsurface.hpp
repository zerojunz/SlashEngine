#pragma once

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to scatter light under the surface of a Forward+ frame's opaque meshes,
// as Godot 4.6's subsurface scattering does: which frames and materials take part, the uniforms
// of shaders/subsurface.hlsl, and the CPU twin of its two passes, which the software device runs.
//
// After the opaque meshes, the meshes with Material3D::subsurface draw again, where they are seen,
// with mesh_subsurface.hlsl into a texture of half floats: each pixel's emitted, diffuse and
// ambient light, and its strength in alpha (below 0 for skin). The blur across reads it into a
// second such texture, and the blur down reads that and gives each pixel the change to its
// diffuse light, which is added to the frame's world: the light Godot keeps apart from the
// specular and adds back.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;

// Whether a scene's frame scatters light under its meshes' surface: in Forward+, with a quality
// other than disabled, when it is no probe's capture and isn't path traced.
[[nodiscard]] bool uses_subsurface(const Scene3D& scene);

// Whether a material draws in the subsurface pass: a lit, opaque StandardMaterial3D with a
// subsurface strength above 0. Games' shaders don't.
[[nodiscard]] bool draws_subsurface(const Material3D& material);

// How many samples each side of the blur's kernel holds, the center included: 6, 9 or 13 for 11,
// 17 or 25 across.
[[nodiscard]] std::uint32_t subsurface_samples(SubsurfaceQuality quality);

// subsurface.hlsl's uniforms: b0 in space3.
struct SubsurfaceUniforms {
    // The frame's width and height, 1 for the pass down, 1 for an orthogonal camera.
    std::array<std::uint32_t, 4> size{};
    // Godot's unit_size, the scale, the depth scale and the kernel's samples.
    Float4 params{};
    // The projection's m10, m11, m14 and m15, from which a depth is made linear.
    Float4 projection{};
};
static_assert(sizeof(SubsurfaceUniforms) == 48);

// The uniforms of the blur across (down false) or down, for a frame of width x height.
[[nodiscard]] SubsurfaceUniforms subsurface_uniforms(const Scene3D& scene, std::uint32_t width,
                                                     std::uint32_t height, bool down);

// One pass of subsurface.hlsl over RGBA pixels, rows top to bottom: source is the subsurface
// pass's light (across) or the blur across (down), depth the frame's reversed depth, one value a
// pixel, and diffuse the subsurface pass's light, which the pass down reads. Returns the pass's
// RGBA pixels, unrounded.
[[nodiscard]] std::vector<float> run_subsurface_pass(const SubsurfaceUniforms& uniforms,
                                                     std::span<const float> source,
                                                     std::span<const float> depth,
                                                     std::span<const float> diffuse);

// Both passes over the subsurface pass's light (half floats), the blur across kept as half floats
// as the GPU devices keep it: the change to add to each pixel of the frame's world, RGBA.
[[nodiscard]] std::vector<float> run_subsurface(const Scene3D& scene, std::uint32_t width,
                                                std::uint32_t height,
                                                std::span<const float> diffuse,
                                                std::span<const float> depth);

} // namespace slash::render::detail
