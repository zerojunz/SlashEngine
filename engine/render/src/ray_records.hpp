#pragma once

#include "gpu_rays.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// What the GPU device's rays hand their shaders, the same on Vulkan and Metal: the uniforms and
// records of shaders/ray_shadows.comp, ray_light.comp and lightmap_light.comp, packed from a
// frame's draws and work, and the order a lightmap bake's dispatches run in.
namespace slash::render::detail {

inline constexpr std::uint32_t kRayGroupSize = 8;
// The textures the shadows' shader writes: two of lights' visibilities, then the ambient
// occlusion.
inline constexpr std::size_t kRayShadowOutputs = 3;
// The passes of the light's shader (ray_light.comp's kPass): the reflections, the global
// illumination's rays and their smoothing, the paths and their average.
inline constexpr std::size_t kRayLightPasses = 5;
inline constexpr std::size_t kRayReflectPass = 0;
inline constexpr std::size_t kRayGiPass = 1;
inline constexpr std::size_t kRaySmoothPass = 2;
inline constexpr std::size_t kRayTracePathsPass = 3;
inline constexpr std::size_t kRayAveragePathsPass = 4;
// The textures the light's shader writes: the reflections (or the paths' average), then the
// ambient light as traced and as smoothed.
inline constexpr std::size_t kRayLightOutputs = 3;
// The most textures the light's shader samples (its textures array); the rest read white.
inline constexpr std::uint32_t kRayTextures = 64;
inline constexpr std::uint32_t kRayNoTexture = 0xFFFFFFFFU;
// Instance masks: the draws rays from the camera's side see, and those that cast shadows.
inline constexpr std::uint8_t kRaySeeMask = 1;
inline constexpr std::uint8_t kRayCastMask = 2;

// shaders/lightmap_light.comp's passes (its kPass): the direct light, a pass of bounces, a pass
// of the probes' rays.
inline constexpr std::size_t kBakePasses = 3;
inline constexpr std::uint32_t kBakeGroupSize = 64;

// The uniforms of shaders/ray_shadows.comp's Frame, in std140.
struct RayLightUniforms {
    std::array<float, 4> place{};
    std::array<float, 4> params{};
    std::array<std::uint32_t, 4> mask{};
};
struct RayShadowsUniforms {
    std::array<float, 16> to_world{};
    std::array<std::array<float, 4>, 3> view_rows{};
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> options{};
    std::array<float, 4> occlusion{}; // radius, power, samples, then intensity
    std::array<RayLightUniforms, kMaxRayShadowLights> lights{};
};
static_assert(sizeof(RayShadowsUniforms) == 64 + 48 + 16 + 16 + 16 + (48 * kMaxRayShadowLights));

// shaders/ray_light.comp's Frame, in std140.
struct RayLightFrame {
    std::array<float, 16> to_world{};
    std::array<std::array<float, 4>, 3> view_rows{};
    std::array<std::uint32_t, 4> size{};
    Float4 ambient{};
    std::array<Float4, 3> sky_rows{};
    std::array<std::uint32_t, 4> gi{};
    std::array<std::uint32_t, 4> paths{}; // the first path's number, paths a pixel, bounces
};
static_assert(sizeof(RayLightFrame) == 64 + 48 + 16 + 16 + 48 + 16 + 16);

// Its Draw and Light, in std430.
struct RayDrawRecord {
    Float4 albedo{};
    Float4 emission{};
    Float4 surface{};
    Float4 metallic_channel{};
    Float4 roughness_channel{};
    std::array<std::uint32_t, 4> options{};
    std::array<Float4, 3> normal_rows{};
    Float4 uv1{};
    std::array<std::uint32_t, 4> buffers{}; // the vertices' and indices' GPU addresses
    std::array<std::uint32_t, 4> textures{};
    std::array<std::uint32_t, 4> more{};
};
static_assert(sizeof(RayDrawRecord) == std::size_t{13} * 16);
struct RayLightRecord {
    Float4 position{};
    Float4 direction{};
    Float4 color{};
    Float4 params{};
    std::array<std::uint32_t, 4> masks{};
    Float4 shadow{};
};
static_assert(sizeof(RayLightRecord) == std::size_t{6} * 16);

// shaders/lightmap_light.comp's Params and Light, in std430.
struct LightmapParams {
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> counts{};
    std::array<std::uint32_t, 4> step{};
    std::array<std::uint32_t, 4> flags{};
    std::array<float, 4> values{};
    std::array<float, 4> more{};
    std::array<std::array<float, 4>, 3> space{};
};
static_assert(sizeof(LightmapParams) == std::size_t{9} * 16);
struct LightmapLightRecord {
    std::array<float, 4> position{};
    std::array<float, 4> direction{};
    std::array<float, 4> color{};
    std::array<float, 4> shape{};
    std::array<float, 4> more{};
};
static_assert(sizeof(LightmapLightRecord) == std::size_t{5} * 16);

// Which rays meet a draw: those from the camera's side, when lit, unless it only casts
// shadows, and shadow rays unless it casts none.
[[nodiscard]] std::uint8_t ray_mask(const RayDraw& draw, bool lit);

// The shadows' shader's uniforms; every_layer when every draw that casts shadows does for every
// light, so rays need not ask which draw they meet.
[[nodiscard]] RayShadowsUniforms
ray_shadows_uniforms(const RayWork& work, const RayShadowFrame& frame, bool every_layer);

// The textures a frame's draws sample, each once with the sampler its draws sample it with
// (linear or (1) nearest, clamped or (2) repeating), at most kRayTextures.
class RayTextureTable {
public:
    // A texture's place in the light's shader's textures array, or kRayNoTexture for none or
    // one past the last place, which reads white.
    std::uint32_t index(const RayTexture& texture);
    [[nodiscard]] const std::vector<std::pair<SDL_GPUTexture*, std::size_t>>& used() const {
        return used_;
    }

private:
    std::vector<std::pair<SDL_GPUTexture*, std::size_t>> used_;
};

// A draw's record, its copies of the mesh's vertices and indices at those GPU addresses.
[[nodiscard]] RayDrawRecord ray_draw_record(const RayDraw& draw, std::uint64_t vertices,
                                            std::uint64_t indices, RayTextureTable& textures);
[[nodiscard]] std::vector<RayLightRecord> ray_light_records(const RayLighting& lighting);
[[nodiscard]] RayLightFrame ray_light_frame(const RayWork& work, const RayShadowFrame& frame,
                                            std::size_t light_count);

// A lightmap trace's inputs as the shader reads them, by its buffers' bindings from 2: vertices,
// indices, triangles, lights, albedo, emission, positions, normals, the environment, then 13:
// the probes.
struct LightmapInputs {
    std::vector<std::array<float, 4>> probes;
    std::vector<std::array<std::uint32_t, 2>> triangles;
    std::vector<LightmapLightRecord> lights;
    struct Input {
        const void* data = nullptr;
        std::size_t bytes = 0;
    };
    std::array<Input, 10> inputs{};
};
// Whether a trace's layers fit it, and its inputs, which point into trace.
[[nodiscard]] bool lightmap_trace_fits(const LightmapTrace& trace);
void lightmap_inputs(const LightmapTrace& trace, LightmapInputs& out);
[[nodiscard]] std::size_t lightmap_texel_bytes(const LightmapTrace& trace);
[[nodiscard]] std::size_t lightmap_probe_bytes(const LightmapTrace& trace);
// Runs a trace's dispatches in order, each pass over count invocations with the params set for
// it: each slice's texels a region at a time for the direct light, then each pass of bounces,
// then each pass of the probes' rays.
Result<void>
run_lightmap_passes(const LightmapTrace& trace, std::size_t light_count,
                    const std::function<Result<void>(std::size_t pass, std::uint32_t count,
                                                     const LightmapParams& params)>& dispatch);
// The traced light, back from the outputs: light_source, accum and the probes' sums.
[[nodiscard]] LightmapTraced lightmap_traced(const LightmapTrace& trace, const void* light_source,
                                             const void* accum, const void* probe_sums);

} // namespace slash::render::detail
