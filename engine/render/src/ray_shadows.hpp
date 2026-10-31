#pragma once

#include "mesh_shading.hpp"
#include "path_trace.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace slash::jobs {
class Pool;
} // namespace slash::jobs

// Ray-traced shadows (RayTracing3D::shadows), as every device that traces rays traces them, and
// their CPU twin. After Forward+'s prepass, each pixel it drew is put back in the world from its
// depth through the camera it was drawn with, moved off its surface along its normal, and sends
// shadow_samples rays toward each light with a ray slot (ray_shadow_slots), to points spread
// across the light by its size, each pixel's at points of its own; the share of rays nothing
// stops, through the light's shadow opacity, is the light's visibility there, which the mesh pass
// reads (add_light in shaders/mesh.hlsl, ray_shadow in mesh.cpp). Random numbers are hashes of
// the pixel, the light's slot and the sample's number, so a frame is the same on any number of
// threads.
namespace slash::render::detail {

// A light as its rays go toward it.
struct RayShadowLight {
    std::uint32_t slot = 0; // its ray slot
    bool directional = false;
    Float3 position{}; // an omni or spot light's place
    Float3 toward{};   // a directional light's way toward it, unit length
    // An omni or spot light's radius, or the tangent of half a directional light's angular
    // diameter; 0 for a point.
    float size = 0.0F;
    float opacity = 1.0F;
    std::uint32_t caster_mask = 0xFFFFFFFFU;
};
// The lights with a ray slot, in slot order.
[[nodiscard]] std::vector<RayShadowLight> ray_shadow_lights(const Scene3D& scene);

// What a frame's rays start from: the prepass's camera and the target's size.
struct RayShadowFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t samples = 1;
    // The prepass's clip space (its view-projection, with any jitter) to the world.
    std::array<double, 16> to_world{};
    // The world to the camera's space, a row each: the prepass's normals are in it.
    std::array<Float4, 3> view_rows{};
};
[[nodiscard]] RayShadowFrame ray_shadow_frame(const Scene3D& scene, const Mat4& view_projection,
                                              std::uint32_t width, std::uint32_t height);

// Where a pixel's rays start: its point in the world from its prepass depth (reversed: 0 where
// nothing was drawn, which has none), moved along its normal (the prepass's view-space normal
// texel, RGB from 0 to 1) to the camera's side by a share of its distance from the camera.
[[nodiscard]] std::optional<Float3> ray_shadow_origin(const RayShadowFrame& frame, std::uint32_t x,
                                                      std::uint32_t y, float depth,
                                                      const Float3& normal_texel);

// A pixel's surface as its rays leave it: ray_shadow_origin's point, and its normal in the world
// (unit length, facing the camera), or zero where the prepass wrote none.
struct RaySurface {
    Float3 origin{};
    Float3 normal{};
};
[[nodiscard]] std::optional<RaySurface> ray_surface(const RayShadowFrame& frame, std::uint32_t x,
                                                    std::uint32_t y, float depth,
                                                    const Float3& normal_texel);

// A pixel's sample-th ray toward a light from origin: its way (unit length) and how far it goes.
struct ShadowRay {
    Float3 direction{};
    float distance = 0.0F;
};
[[nodiscard]] std::optional<ShadowRay> ray_shadow_ray(const RayShadowFrame& frame,
                                                      const RayShadowLight& light,
                                                      const Float3& origin, std::uint32_t x,
                                                      std::uint32_t y, std::uint32_t sample);

// The frame's ray-traced shadows through scene's triangles (textures for their alpha, or none):
// kMaxRayShadowLights visibilities a pixel in rows from the top, as half floats, as the screen
// texture's layers hold them; 1 where a pixel has no ray or a slot no light. depth is the
// prepass's, a float a pixel, and normals its normal texels, four a pixel. Rows are shared out over
// pool's threads when there is one.
[[nodiscard]] std::vector<float>
run_ray_shadows(const PathScene& scene, const PathTextures* textures,
                std::span<const RayShadowLight> lights, const RayShadowFrame& frame,
                std::span<const float> depth, std::span<const float> normals, jobs::Pool* pool);

// Ray-traced ambient occlusion (RayTracing3D::ambient_occlusion), traced in place of the
// environment's SSAO: whether a frame asks for it (Forward+ with SSAO), and what its rays take
// from the SSAO.
struct RayOcclusion {
    std::uint32_t samples = 4;
    float radius = 1.0F;    // how far the rays go
    float intensity = 2.0F; // how dark the share they find blocked makes it
    float power = 1.5F;     // the occlusion's curve
};
[[nodiscard]] std::optional<RayOcclusion> ray_occlusion(const Scene3D& scene);

// A pixel's sample-th occlusion ray from a surface facing normal (unit length): cosine weighted
// over the half of the sphere it faces, by the R2 sequence from a hash of the pixel.
[[nodiscard]] Float3 occlusion_ray(const Float3& normal, std::uint32_t x, std::uint32_t y,
                                   std::uint32_t width, std::uint32_t sample);

// The frame's ray-traced ambient occlusion through scene's triangles (those that cast shadows),
// as half floats, a value a pixel in rows from the top, as Godot's SSAO makes its occlusion from
// the obscurance it gathers: 1 less intensity times the share of its rays that something stops
// within radius (at most 0.98), to the power; 1 where a pixel has no rays. As run_ray_shadows
// takes depth and normals.
[[nodiscard]] std::vector<float>
run_ray_occlusion(const PathScene& scene, const PathTextures* textures,
                  const RayOcclusion& occlusion, const RayShadowFrame& frame,
                  std::span<const float> depth, std::span<const float> normals, jobs::Pool* pool);

// Ray-traced reflections (RayTracing3D::reflections), traced in place of the environment's SSR:
// whether a frame asks for them (Forward+ with SSR) and how many rays a pixel reflects.
struct RayReflection {
    std::uint32_t samples = 1;
};
[[nodiscard]] std::optional<RayReflection> ray_reflection(const Scene3D& scene);

// Pixels this rough or rougher reflect nothing traced, as SSR's trace leaves them out (shade_mesh
// fades reflections out from 0.6 to it).
inline constexpr float kRayReflectionCutoff = 0.7F;

// The way from a pixel's surface toward the eye that sees it (unit length), for a pixel at depth.
[[nodiscard]] Float3 ray_view(const RayShadowFrame& frame, std::uint32_t x, std::uint32_t y,
                              const Float3& origin);

// A pixel's sample-th reflection ray off a surface facing normal seen along view, for roughness:
// path_reflection_ray's, picked by the R2 sequence from a hash of the pixel.
[[nodiscard]] Float3 reflection_ray(const Float3& normal, const Float3& view, float roughness,
                                    std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                    std::uint32_t sample);

// The frame's ray-traced reflections through scene, as half floats, four a pixel in rows from the
// top, as SSR's resolve gives them: the light the rays that meet something bring back
// (path_reflection), summed and divided by the rays sent, and the share of them that met
// something; nothing (0, 0, 0, 0) for a pixel with no surface or from kRayReflectionCutoff rough.
// The prepass's normals carry each pixel's roughness in their alpha. As run_ray_shadows takes
// depth and normals.
[[nodiscard]] std::vector<float>
run_ray_reflections(const PathScene& scene, const PathTextures& textures,
                    const RayReflection& reflection, const RayShadowFrame& frame,
                    std::span<const float> depth, std::span<const float> normals, jobs::Pool* pool);

// Ray-traced global illumination (RayTracing3D::global_illumination): whether a frame asks for it
// (Forward+) and how many rays a pixel sends.
struct RayGI {
    std::uint32_t samples = 4;
};
[[nodiscard]] std::optional<RayGI> ray_gi(const Scene3D& scene);

// A pixel's sample-th light ray off a surface facing normal (unit length): cosine weighted over
// the half of the sphere it faces, as occlusion_ray's, from a seed of its own.
[[nodiscard]] Float3 light_ray(const Float3& normal, std::uint32_t x, std::uint32_t y,
                               std::uint32_t width, std::uint32_t sample);

// The frame's ray-traced ambient light through scene, as half floats, three a pixel in rows from
// the top: the light its rays bring back (path_reflection where they meet something, else
// path_environment), averaged, then smoothed over the pixels within two of it, each weighed by how
// alike its normal is and how near it is to the pixel's plane; 0 where a pixel has no surface. As
// run_ray_shadows takes depth and normals.
[[nodiscard]] std::vector<float> run_ray_gi(const PathScene& scene, const PathTextures& textures,
                                            const RayGI& gi, const RayShadowFrame& frame,
                                            std::span<const float> depth,
                                            std::span<const float> normals, jobs::Pool* pool);

} // namespace slash::render::detail
