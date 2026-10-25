#pragma once

#include "mesh_shading.hpp"

#include "slash/render/mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// What every Forward+ device shares to make volumetric fog (VolumetricFog3D, in mesh.hpp) as
// Godot 4.6's Fog::volumetric_fog_update makes it: the froxel volume's size, the uniforms of
// shaders/volumetric_fog.hlsl's passes, and their CPU twins, which the software device runs and
// which follow the shader line for line in float math. The volume is a storage buffer of a float4
// a froxel, in rows across, then down, then slices deeper, rather than Godot's 3D textures, so
// that the meshes' fragment stage, whose sixteen samplers are taken, reads it as a buffer and
// filters it itself, as every device does the same way. Built with -ffp-contract=off.
namespace slash::render::detail {

// The most froxels a volume holds: 16 bytes each, a storage buffer of kMaxStorageBytes.
inline constexpr std::size_t kMaxFogFroxels = std::size_t{1} << 23U;
// The fog passes' threads in a group, across and down (one deep).
inline constexpr std::uint32_t kFogThreads = 8;
// Godot's TEMPORAL_FRAMES: the Halton points the froxels are jittered through.
inline constexpr std::uint32_t kFogTemporalFrames = 16;

// The passes of shaders/volumetric_fog.hlsl, in the order a frame runs them: each froxel's light
// and density, the blur across and down (with use_filter), and the gathering along each column.
enum class FogPass : std::uint8_t { Density, FilterX, FilterY, Integrate };

// The froxels a frame of width x height takes across, down and deep, as Godot's
// RenderForwardClustered::_update_volumetric_fog sizes them (depth trimmed to kMaxFogFroxels).
[[nodiscard]] std::array<std::uint32_t, 3>
fog_volume_size(const VolumetricFogSettings& settings, std::uint32_t width, std::uint32_t height);

// Whether a frame of the scene has volumetric fog: Forward+ with it on, and not a reflection
// probe's capture, as Godot's probes are drawn without it.
[[nodiscard]] bool uses_volumetric_fog(const Scene3D& scene);
// Sets the meshes' uniforms for a frame whose fog volume is size.
void set_volumetric_fog(MeshSceneUniforms& lights, const Scene3D& scene,
                        const std::array<std::uint32_t, 3>& size);

// A target's volumetric fog over its frames: how many it was drawn with, the camera of the last
// and the volume's size then. A frame reprojects the density of the one before only when it had
// the same size.
struct FogClock {
    std::uint32_t frames = 0;
    Mat4 camera{};
    std::array<std::uint32_t, 3> size{};
};

// The passes' uniforms: b0 in space2 of shaders/volumetric_fog.hlsl (b0 in Metal), whose b1 is
// MeshShadowUniforms.
struct FogUniforms {
    // Froxels across, down and deep, then the pass (FogPass).
    std::array<std::uint32_t, 4> size{};
    // The view's half extents at the camera's near plane (x, y), as Godot's fog_frustum_size_begin,
    // and at length (z, w).
    Float4 frustum{};
    Float4 params{};     // length, detail spread, the far plane's distance, anisotropy
    Float4 scattering{}; // the albedo, linear, times density; then density
    // The emission, linear, times its energy and density; then 1 when the frame reprojects the
    // density of the frame before.
    Float4 emission{};
    // The ambient light's color, linear, without its energy; then ambient_inject times its energy.
    Float4 ambient{};
    // The frame's Halton point in its froxel when it reprojects, then how much of the frame
    // before's density it keeps.
    Float4 jitter{};
    // 1 when the sky's radiance lights the fog, how much (the ambient light's sky contribution),
    // the background's energy, then how many directional lights.
    Float4 sky{};
    // The frame's width and height in pixels, then 0, 0.
    Float4 screen{};
    Float4 cluster{};     // MeshSceneUniforms::cluster
    Mat4 view_to_world{}; // the camera's space to the world
    Mat4 to_previous{};   // the camera's space to the camera's space of the frame before
    std::array<Float4, 3> sky_rows{}; // the world to the sky's space, a row each
    std::array<MeshLightUniforms, kMaxDirectionalLights3D> lights{}; // the directional ones
};
static_assert(sizeof(FogUniforms) == 336 + (80 * kMaxDirectionalLights3D));

// The fog's uniforms for a frame of the scene drawn at width x height, whose meshes take lights
// and clusters, as Godot's volumetric_fog_update sets them for the density pass, and moves clock
// on to the frame. A frame reprojects the clock's last when its fog asks to and the clock has
// one of the same size; its Halton point is the clock's frame's.
[[nodiscard]] FogUniforms volumetric_fog_uniforms(const Scene3D& scene,
                                                  const MeshSceneUniforms& lights,
                                                  std::uint32_t width, std::uint32_t height,
                                                  FogClock& clock);

// What the density pass reads besides its uniforms, as the mesh shader's maps: the shadows, the
// sky's radiance (nullptr when the fog takes none) and the clusters of the omni and spot lights.
struct FogMaps {
    const MeshShadowUniforms* shadows = nullptr;
    ShadowAtlas directional;
    ShadowAtlas positional;
    const CubeImage* radiance = nullptr;
    const LightClusters* clusters = nullptr;
};

// The CPU twins of the passes, a froxel (or, integrating, a column) at x, y and z of the volume:
// its density pass's light and density from the frame before's (empty when it reprojects none),
// its blur along the pass's axis, and the gathering of the column at x, y from source into target.
[[nodiscard]] Float4 fog_density(const FogUniforms& uniforms, const FogMaps& maps,
                                 std::span<const Float4> previous, std::uint32_t x, std::uint32_t y,
                                 std::uint32_t z);
[[nodiscard]] Float4 fog_filter(const FogUniforms& uniforms, std::span<const Float4> source,
                                std::uint32_t x, std::uint32_t y, std::uint32_t z);
void fog_integrate(const FogUniforms& uniforms, std::span<const Float4> source,
                   std::span<Float4> target, std::uint32_t x, std::uint32_t y);

// A frame's volume on the CPU: the density, which the next frame reprojects, and the fog the
// meshes and sky are seen through.
struct FogVolume {
    std::vector<Float4> density;
    std::vector<Float4> fog;
};
// Runs a frame's passes as the GPU devices run them, with use_filter's blur when filter is set.
[[nodiscard]] FogVolume run_volumetric_fog(FogUniforms uniforms, const FogMaps& maps,
                                           std::span<const Float4> previous, bool filter);

// The fog in front of a point view_depth away (along the camera's -Z) at a place on the screen
// (its texture coordinate, y down), read linearly from the volume of size as a 3D texture is:
// the light it adds, then how much of the point's it lets through. volumetric_fog is
// MeshSceneUniforms::volumetric_fog.
[[nodiscard]] Float4 sample_fog_volume(std::span<const Float4> fog,
                                       const std::array<std::uint32_t, 3>& size,
                                       const Float4& volumetric_fog, std::array<float, 2> uv,
                                       float view_depth);

// The sky's color seen through the fog's whole length at a place on the screen, as Godot's sky
// shader takes volumetric fog, by sky_affect (volumetric_fog's w).
[[nodiscard]] Float3 fog_sky(std::span<const Float4> fog, const std::array<std::uint32_t, 3>& size,
                             const Float4& volumetric_fog, std::array<float, 2> uv,
                             const Float3& sky);

// The sky pass's uniforms over the fog: b0 in space3 of shaders/volumetric_fog_sky.hlsl.
struct FogSkyUniforms {
    Float4 volumetric_fog{}; // MeshSceneUniforms::volumetric_fog
    Float4 volume{};         // MeshSceneUniforms::fog_volume
    Float4 screen{};         // the target's width and height in pixels, then 0, 0
};
[[nodiscard]] FogSkyUniforms fog_sky_uniforms(const MeshSceneUniforms& lights);

} // namespace slash::render::detail
