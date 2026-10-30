#pragma once

#include "mesh_shading.hpp"
#include "sky_shading.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::jobs {
class Pool;
} // namespace slash::jobs

// The path tracer (Scene3D::path_trace): a frame's meshes, lights and sky put in a bounding volume
// hierarchy of their triangles in the world, and paths traced through it from the camera, shaded
// with what shaders/mesh.hlsl shades with (path_surface, light_response and light_reach) so that a
// path traced still differs from the real-time frame only by the light the real-time frame works
// out another way. Random numbers come from hashes of the pixel, the sample's number and the
// bounce, and nothing depends on the order work is done in, so a still is the same on any number
// of threads and samples summed over several frames are the same as summed in one.
namespace slash::render::detail {

// A draw as paths meet it.
struct PathDraw {
    MeshSurfaceUniforms surface;
    CullMode cull = CullMode::Back;
    ShadowCasting shadows = ShadowCasting::On;
    std::uint32_t layers = 1;
    // Its alpha decides whether a ray meets a point (alpha scissor, or blended, which lets
    // through the share of rays its alpha doesn't take).
    bool alpha_tested = false;
};

// A triangle: its corners in PathScene::corners and its draw.
struct PathTriangle {
    std::array<std::uint32_t, 3> corners{};
    std::uint32_t draw = 0;
};

// A node of the hierarchy: its box, then either its first child (the second follows it) or, in a
// leaf, its first triangle and how many there are.
struct PathNode {
    Float3 low{};
    Float3 high{};
    std::uint32_t first = 0;
    std::uint32_t count = 0; // 0 for a node with children
};

// A light as paths take it.
struct PathLight {
    MeshLightUniforms uniforms;
    bool shadow = false;
    float shadow_opacity = 1.0F;
    std::uint32_t caster_mask = 0xFFFFFFFFU;
};

// A mesh drawn in the frame: its draw, its vertices and the triangles of the LOD drawn.
struct PathMesh {
    const MeshDraw* draw = nullptr;
    std::span<const Vertex3D> vertices;
    std::span<const std::uint32_t> triangles;
};

// What a frame's paths meet and are lit by.
struct PathScene {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bounces = 0;
    std::vector<MeshCorner> corners;
    std::vector<PathTriangle> triangles; // in the hierarchy's leaves' order
    std::vector<PathDraw> draws;
    std::vector<PathNode> nodes; // the root first; none without triangles
    std::vector<PathLight> lights;
    // The camera's clip space to the world, and where its near plane's rays start.
    std::array<double, 16> to_world{};
    // What a ray from the camera that meets nothing shows: the sky the frame draws, or the
    // clear color (linear, as forward_clear gives it).
    bool sky = false;
    SkyUniforms sky_uniforms;
    Float4 clear{};
    // The light from what a bounce meets nothing at: the sky's along it times environment_scale,
    // mixed over ambient by sky_share, when meshes are lit by the sky's radiance; else ambient.
    bool environment_sky = false;
    SkyUniforms environment_uniforms;
    std::array<Float3, 3> to_sky{}; // the world to the sky's space, a row each
    float environment_scale = 1.0F;
    float sky_share = 1.0F;
    Float3 ambient{};
};

// Builds a frame's path scene for a target width by height pixels.
[[nodiscard]] PathScene build_path_scene(const Scene3D& scene, std::span<const PathMesh> meshes,
                                         std::uint32_t width, std::uint32_t height,
                                         const Float4& clear);

// A hash of everything a path scene's paths see but its textures, which a target's frames sum
// their samples only while it stays the same.
[[nodiscard]] std::uint64_t path_scene_key(const PathScene& scene);

// Where the path tracer's textures come from: each draw's mesh_texture_slots, sampled at uv as
// the draw samples them, and the sky's texture (the frame's sky, or its radiance's) at a ray's
// panorama coordinate; white where there is none.
class PathTextures {
public:
    PathTextures() = default;
    PathTextures(const PathTextures&) = delete;
    PathTextures& operator=(const PathTextures&) = delete;
    PathTextures(PathTextures&&) = delete;
    PathTextures& operator=(PathTextures&&) = delete;
    virtual ~PathTextures() = default;

    [[nodiscard]] virtual Float4 sample(std::size_t draw, std::size_t slot,
                                        std::array<float, 2> uv) const = 0;
    [[nodiscard]] virtual Float4 sky(bool radiance, std::array<float, 2> coords) const = 0;
};

// Traces samples more paths through each pixel, numbered from first, and adds the light each
// brings (linear RGB) to sums, three floats a pixel in rows from the top, one sample after
// another in order. Rows are shared out over pool's threads when there is one.
void trace_paths(const PathScene& scene, const PathTextures& textures, std::uint32_t first,
                 std::uint32_t samples, std::span<float> sums, jobs::Pool* pool);

// What a ray meets: the triangle (index into PathScene::triangles), how far along it, and where
// on the triangle (the second and third corners' shares).
struct PathHit {
    std::uint32_t triangle = 0;
    float distance = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
};
// The nearest triangle a camera or bounce ray from origin along direction (unit length) meets
// within distance, as trace_paths finds it, leaving out alpha tests; for tests.
[[nodiscard]] std::optional<PathHit> path_hit(const PathScene& scene, const Float3& origin,
                                              const Float3& direction, float distance);

// Whether a shadow ray from origin along direction (unit length) meets a triangle within distance
// that casts shadows for caster_mask, as trace_paths's shadow rays find one: alpha tested, with
// textures, through alpha scissors and by seed through blended alpha.
[[nodiscard]] bool path_blocked(const PathScene& scene, const PathTextures* textures,
                                const Float3& origin, const Float3& direction, float distance,
                                std::uint32_t caster_mask, std::uint32_t seed);

} // namespace slash::render::detail
