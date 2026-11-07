#pragma once

#include "mesh_shading.hpp"
#include "path_trace.hpp"
#include "ray_shadows.hpp"

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

// ReSTIR direct lighting (RayTracing3D::many_lights), a research spike on the software device,
// after Bitterli et al.'s "Spatiotemporal reservoir resampling for real-time ray tracing with
// dynamic direct lighting" (2020). Instead of looping over every omni and spot light its cluster
// holds, each opaque pixel keeps reservoirs of one light each: it draws many_light_candidates
// lights from its cluster, keeps one by resampled importance sampling against how much light
// each would bring with no shadow, merges the reservoir it kept last frame where its surface was
// (reprojected through last frame's camera, if the surface is alike), then those of a few
// neighbors on the same surface, and traces one shadow ray to the light it ends with. The mesh
// pass shades each pixel with its reservoirs' lights, each weighed by its reservoir, in place of
// its cluster's lights (shade_mesh, MeshMaps::restir). Reservoirs are combined as the paper's
// biased variant does, by their sample counts, with last frame's held to 20 times a pixel's
// candidates. Random numbers are hashes of the pixel, the frame's number, the reservoir and the
// step, so a frame is the same on any number of threads.
namespace slash::render::detail {

inline constexpr std::uint32_t kMaxManyLightCandidates = 32;
inline constexpr std::uint32_t kMaxManyLightSamples = 4;
// How many times a pixel's candidates a reservoir kept from last frame may count for.
inline constexpr float kRestirHistory = 20.0F;
// Neighbors a pixel merges, and how far they may be, in pixels.
inline constexpr std::uint32_t kRestirNeighbors = 3;
inline constexpr float kRestirRadius = 12.0F;

// One light a pixel keeps: its index into Scene3D::lights (-1 for none), the sum of the
// candidates' weights, how many candidates it stands for and its weight when shading.
struct Reservoir {
    std::int32_t light = -1;
    float weight_sum = 0.0F;
    float count = 0.0F;
    float weight = 0.0F;
};

// What a target keeps from frame to frame: last frame's reservoirs and surfaces, its camera and
// which lights it had.
struct RestirState {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t samples = 0;
    std::uint32_t frame = 0;
    std::uint64_t lights_key = 0;
    Mat4 view_projection{};
    std::vector<Reservoir> reservoirs; // samples a pixel
    std::vector<Float3> positions;     // where each pixel's surface was
    std::vector<Float3> normals;       // its normal, zero where there was none
};

// Whether a frame lights its omni and spot lights with ReSTIR, and how.
struct ManyLights {
    std::uint32_t candidates = 8;
    std::uint32_t samples = 1;
};
[[nodiscard]] std::optional<ManyLights> many_lights(const Scene3D& scene);

// A key of the scene's omni and spot lights (their count, places, colors and ranges), which
// starts a target's history over when it changes.
[[nodiscard]] std::uint64_t restir_lights_key(const Scene3D& scene);

// The frame's reservoirs: samples pairs a pixel in rows from the top, the light's index into
// clusters.lights (or -1) and its weight with what its shadow ray found, as MeshMaps::restir
// holds them. view_projection is the prepass's, which state keeps for the next frame; scene,
// textures, frame, depth and normals are as run_ray_shadows takes them.
[[nodiscard]] std::vector<float>
run_restir(RestirState& state, const ManyLights& settings, const Scene3D& lights_scene,
           const LightClusters& clusters, const Mat4& view_projection, const PathScene& scene,
           const PathTextures* textures, const RayShadowFrame& frame, std::span<const float> depth,
           std::span<const float> normals, jobs::Pool* pool);

} // namespace slash::render::detail
