// Forward+'s light clusters, as Godot's ClusterBuilderRD lays them out (servers/rendering/
// renderer_rd/cluster_builder_rd.cpp and its cluster_render and cluster_store shaders), but built
// on the CPU from each light's bounding sphere instead of rasterized on the GPU, so the software
// device and every GPU read the same clusters. Float math, built with -ffp-contract=off.

#include "decal.hpp"
#include "mesh_shading.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace slash::render::detail {

namespace {

Float4 transform(const Mat4& m, const Float4& v) {
    Float4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                      (m.at(12 + row) * v[3]);
    }
    return out;
}

// A light's sphere in the camera's space, and its place in the scene's lights.
struct Sphere {
    std::size_t light = 0;
    Float3 center{};
    float depth = 0.0F; // in front of the camera: -center's z
    float radius = 0.0F;
};

// The tile a pixel coordinate falls in, clamped to tiles.
std::uint32_t tile_of(float pixel, std::uint32_t tiles) {
    const float tile = std::floor(pixel / static_cast<float>(kClusterSize));
    return static_cast<std::uint32_t>(std::clamp(tile, 0.0F, static_cast<float>(tiles - 1)));
}

// The slice a view depth falls in, as the shader picks it.
std::uint32_t slice_of(float depth, float slices_per_depth) {
    return static_cast<std::uint32_t>(
        std::clamp(depth * slices_per_depth, 0.0F, static_cast<float>(kClusterSlices - 1)));
}

// What an item reaches: a screen rect in pixels from the top left and a range of view depths.
struct Reach {
    float left = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    float bottom = 0.0F;
    float nearest = 0.0F;
    float farthest = 0.0F;
};

// Sets an item's bit and slice ranges in every tile and slice it reaches, in the section of each
// tile's cells that starts at offset and has words words of bits.
void mark(LightClusters& out, std::size_t offset, std::uint32_t words, std::uint32_t index,
          const Reach& reach) {
    const auto w = static_cast<float>(out.width);
    const auto h = static_cast<float>(out.height);
    if (reach.right < 0.0F || reach.left > w || reach.bottom < 0.0F || reach.top > h) {
        return; // off the screen
    }
    const std::uint32_t tile_left = tile_of(reach.left, out.tiles_across);
    const std::uint32_t tile_right = tile_of(reach.right, out.tiles_across);
    const std::uint32_t tile_top = tile_of(reach.top, out.tiles_down);
    const std::uint32_t tile_bottom = tile_of(reach.bottom, out.tiles_down);
    const std::uint32_t first_slice = slice_of(reach.nearest, out.slices_per_depth);
    const std::uint32_t last_slice = slice_of(reach.farthest, out.slices_per_depth);
    const std::size_t stride = out.stride();
    for (std::uint32_t y = tile_top; y <= tile_bottom; ++y) {
        for (std::uint32_t x = tile_left; x <= tile_right; ++x) {
            const std::size_t cell = ((std::size_t{y} * out.tiles_across) + x) * stride + offset;
            out.cells[cell + (index / 32)] |= 1U << (index % 32);
            for (std::uint32_t slice = first_slice; slice <= last_slice; ++slice) {
                std::uint32_t& range = out.cells[cell + words + slice];
                // Items come in order, so the first to reach a slice is its lowest.
                const std::uint32_t first = range == 0 ? index : range & 0xFFFFU;
                range = first | ((index + 1) << 16U);
            }
        }
    }
}

// The screen rect of points in the camera's space, or the whole screen when one is behind the
// camera's eye.
template <typename Points>
void screen_rect(const Mat4& projection, const Points& points, float w, float h, Reach& reach) {
    reach.left = 0.0F;
    reach.right = w;
    reach.top = 0.0F;
    reach.bottom = h;
    float low_x = std::numeric_limits<float>::max();
    float high_x = std::numeric_limits<float>::lowest();
    float low_y = low_x;
    float high_y = high_x;
    for (const Float4& point : points) {
        const Float4 clip = transform(projection, point);
        if (!(clip[3] > 1.0e-6F)) {
            return;
        }
        const float x = clip[0] / clip[3];
        const float y = clip[1] / clip[3];
        low_x = std::min(low_x, x);
        high_x = std::max(high_x, x);
        low_y = std::min(low_y, y);
        high_y = std::max(high_y, y);
    }
    reach.left = (low_x + 1.0F) * 0.5F * w;
    reach.right = (high_x + 1.0F) * 0.5F * w;
    reach.top = (1.0F - high_y) * 0.5F * h;
    reach.bottom = (1.0F - low_y) * 0.5F * h;
}

} // namespace

float far_plane(const Mat4& projection) {
    // Both projections put the far plane at [14] / [10]; Godot's default camera's otherwise.
    return projection[10] != 0.0F ? projection[14] / projection[10] : 4000.0F;
}

Float4 LightClusters::uniform() const {
    return {static_cast<float>(tiles_across), static_cast<float>(words), slices_per_depth,
            static_cast<float>(decal_words)};
}

std::uint32_t LightClusters::stride() const {
    return words + kClusterSlices + (decal_words > 0 ? decal_words + kClusterSlices : 0);
}

LightClusters build_clusters(const Scene3D& scene, std::uint32_t width, std::uint32_t height,
                             const DecalAtlas* decal_atlas) {
    LightClusters out;
    out.width = width;
    out.height = height;
    out.tiles_across = std::max(1U, (width + kClusterSize - 1) / kClusterSize);
    out.tiles_down = std::max(1U, (height + kClusterSize - 1) / kClusterSize);
    const float far = far_plane(scene.projection);
    out.slices_per_depth = far > 0.0F ? static_cast<float>(kClusterSlices) / far : 0.0F;

    // A camera matrix that scales stretches distances by up to its longest row.
    const Mat4& c = scene.camera;
    float stretch = 0.0F;
    for (std::size_t row = 0; row < 3; ++row) {
        stretch =
            std::max(stretch, std::sqrt((c.at(row) * c.at(row)) + (c.at(4 + row) * c.at(4 + row)) +
                                        (c.at(8 + row) * c.at(8 + row))));
    }
    std::vector<Sphere> spheres;
    for (std::size_t i = 0; i < lights_drawn(scene); ++i) {
        const Light3D& light = scene.lights[i];
        if (light.type == LightType::Directional) {
            continue;
        }
        const Float4 center =
            transform(c, {light.position[0], light.position[1], light.position[2], 1.0F});
        // The range the shader divides by, rounded out by a thousandth so that float rounding
        // never leaves out a pixel the light reaches.
        const float radius = (std::max(0.001F, light.range) * stretch * 1.001F) + 0.0001F;
        spheres.push_back({.light = i,
                           .center = {center[0], center[1], center[2]},
                           .depth = -center[2],
                           .radius = radius});
    }
    std::ranges::stable_sort(spheres, {}, &Sphere::depth);

    out.words = std::max<std::uint32_t>(1, static_cast<std::uint32_t>((spheres.size() + 31) / 32));
    PlacedDecals decals;
    if (decal_atlas != nullptr) {
        decals = place_decals(scene, *decal_atlas);
        out.decal_words = static_cast<std::uint32_t>((decals.uniforms.size() + 31) / 32);
        out.decals = std::move(decals.uniforms);
    }
    out.cells.assign(std::size_t{out.tiles_across} * out.tiles_down * out.stride(), 0);
    const std::vector<int> slots = shadow_slots(scene);
    const std::vector<int> rays = ray_shadow_slots(scene);
    out.lights.reserve(spheres.size());
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    for (std::size_t k = 0; k < spheres.size(); ++k) {
        const Sphere& sphere = spheres[k];
        out.lights.push_back(mesh_light_uniforms(scene.lights[sphere.light], slots[sphere.light],
                                                 rays[sphere.light]));
        Reach reach{.nearest = sphere.depth - sphere.radius,
                    .farthest = sphere.depth + sphere.radius};
        if (reach.farthest < 0.0F || reach.nearest > far) {
            continue; // wholly behind the camera or past the far plane
        }
        // The screen rect of the box around the sphere.
        std::array<Float4, 8> corners{};
        for (std::size_t corner = 0; corner < 8; ++corner) {
            const auto side = [&](std::size_t axis) {
                return ((corner >> axis) & 1U) != 0 ? sphere.radius : -sphere.radius;
            };
            corners.at(corner) = {sphere.center[0] + side(0), sphere.center[1] + side(1),
                                  sphere.center[2] + side(2), 1.0F};
        }
        screen_rect(scene.projection, corners, w, h, reach);
        mark(out, 0, out.words, static_cast<std::uint32_t>(k), reach);
    }
    // Each decal reaches the screen rect and depths of its box's corners, padded by a pixel and a
    // thousandth of their depth so that float rounding never leaves out a pixel inside it.
    for (std::size_t k = 0; k < decals.boxes.size(); ++k) {
        const Mat4& box = decals.boxes[k];
        std::array<Float4, 8> corners{};
        Reach reach{.nearest = std::numeric_limits<float>::max(),
                    .farthest = std::numeric_limits<float>::lowest()};
        for (std::size_t corner = 0; corner < 8; ++corner) {
            const Float4 local{(corner & 1U) != 0 ? 1.0F : 0.0F, (corner & 2U) != 0 ? 1.0F : -1.0F,
                               (corner & 4U) != 0 ? 1.0F : 0.0F, 1.0F};
            const Float4 seen = transform(c, transform(box, local));
            corners.at(corner) = seen;
            const float depth = -seen[2];
            const float pad = (std::abs(depth) * 0.001F) + 0.0001F;
            reach.nearest = std::min(reach.nearest, depth - pad);
            reach.farthest = std::max(reach.farthest, depth + pad);
        }
        if (reach.farthest < 0.0F || reach.nearest > far) {
            continue;
        }
        screen_rect(scene.projection, corners, w, h, reach);
        reach.left -= 1.0F;
        reach.right += 1.0F;
        reach.top -= 1.0F;
        reach.bottom += 1.0F;
        mark(out, out.words + kClusterSlices, out.decal_words, static_cast<std::uint32_t>(k),
             reach);
    }
    return out;
}

} // namespace slash::render::detail
