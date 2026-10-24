#include "mesh_shading.hpp"

#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

// trace_voxel_gis, the CPU twin of trace_voxel_gis in shaders/mesh.hlsl: Godot 4.6's
// voxel_gi_compute and its cone traces (servers/rendering/renderer_rd/shaders/environment/gi.glsl,
// MIT), over a VoxelGI's 3D texture laid out as tiles of a 2D atlas (VoxelGIShading).
namespace slash::render::detail {

namespace {

Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 scale(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}

float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

Float3 normalize(const Float3& v) {
    return scale(v, 1.0F / std::sqrt(dot(v, v)));
}

Float4 lerp(const Float4& a, const Float4& b, float t) {
    Float4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out.at(i) = a.at(i) + ((b.at(i) - a.at(i)) * t);
    }
    return out;
}

// What a VoxelGI's cone traces read.
struct Volume {
    const Image& atlas;
    const MeshVoxelGIUniforms& gi;
    Float3 cell_size{}; // 1 over the texels along each axis at level 0
    float max_distance = 0.0F;
};

// The texture's level at uvw, as a linear sampler that clamps reads one level of a 3D texture:
// bilinear in the two slices around uvw, each read inside its own tile, then between them.
Float4 sample_level(const Volume& volume, std::uint32_t level, const Float3& uvw) {
    const MeshVoxelGIUniforms& gi = volume.gi;
    const auto divisor = static_cast<float>(1U << level);
    const float sx = std::max(std::floor(gi.size[0] / divisor), 1.0F);
    const float sy = std::max(std::floor(gi.size[1] / divisor), 1.0F);
    const float sz = std::max(std::floor(gi.size[2] / divisor), 1.0F);
    const Float4& place = gi.levels.at(level);
    const auto columns = static_cast<std::uint32_t>(place[2]);
    const float x = std::clamp(uvw[0] * sx, 0.5F, sx - 0.5F);
    const float y = std::clamp(uvw[1] * sy, 0.5F, sy - 0.5F);
    const float z = std::clamp((uvw[2] * sz) - 0.5F, 0.0F, sz - 1.0F);
    const float z0 = std::floor(z);
    const auto width = static_cast<float>(volume.atlas.width);
    const auto height = static_cast<float>(volume.atlas.height);
    const auto slice = [&](float at) {
        const auto index = static_cast<std::uint32_t>(at);
        const float left = place[0] + (static_cast<float>(index % columns) * sx) + x;
        const std::uint32_t row = index / columns;
        const float top = place[1] + (static_cast<float>(row) * sy) + y;
        return sample_image(volume.atlas, Filter::Linear, false, left / width, top / height);
    };
    return lerp(slice(z0), slice(std::min(z0 + 1.0F, sz - 1.0F)), z - z0);
}

// textureLod with a linear sampler with mipmaps: between the two levels around lod.
Float4 sample_lod(const Volume& volume, const Float3& uvw, float lod) {
    lod = std::clamp(lod, 0.0F, volume.gi.size[3] - 1.0F);
    const float low = std::floor(lod);
    const Float4 out = sample_level(volume, static_cast<std::uint32_t>(low), uvw);
    if (lod == low) {
        return out;
    }
    return lerp(out, sample_level(volume, static_cast<std::uint32_t>(low) + 1, uvw), lod - low);
}

// Whether uvw is farther outside the texture than radius cells.
bool outside(const Volume& volume, const Float3& uvw, float radius) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(uvw.at(i) - 0.5F) > 0.5F + (radius * volume.cell_size.at(i))) {
            return true;
        }
    }
    return false;
}

Float3 cells(const Volume& volume, const Float3& pos, const Float3& direction, float dist) {
    const Float3 at = add(pos, scale(direction, dist));
    return {at[0] * volume.cell_size[0], at[1] * volume.cell_size[1], at[2] * volume.cell_size[2]};
}

// voxel_cone_trace: a cone from pos (in cells) along direction, its diameter growing by
// 2 tan_half_angle a cell, stepping half its diameter, until it is opaque or leaves the texture.
Float4 cone_trace(const Volume& volume, const Float3& pos, const Float3& direction,
                  float tan_half_angle, float bias) {
    float dist = bias;
    Float4 color{};
    while (dist < volume.max_distance && color[3] < 1.0F) {
        const float diameter = std::max(1.0F, 2.0F * tan_half_angle * dist);
        const Float3 uvw = cells(volume, pos, direction, dist);
        const float half_diameter = diameter * 0.5F;
        if (outside(volume, uvw, half_diameter)) {
            break;
        }
        const Float4 sample = sample_lod(volume, uvw, std::log2(diameter));
        const float a = 1.0F - color[3];
        for (std::size_t i = 0; i < 4; ++i) {
            color.at(i) += a * sample.at(i);
        }
        dist += half_diameter;
    }
    return color;
}

// voxel_cone_trace_45_degrees: a cone of 45 degrees each side, each step as far as its radius and
// a level coarser.
Float4 cone_trace_45_degrees(const Volume& volume, const Float3& pos, const Float3& direction,
                             float bias) {
    float dist = bias;
    Float4 color{};
    float radius = std::max(0.5F, dist);
    float lod = std::log2(radius * 2.0F);
    while (dist < volume.max_distance && color[3] < 1.0F) {
        const Float3 uvw = cells(volume, pos, direction, dist);
        if (outside(volume, uvw, radius)) {
            break;
        }
        const Float4 sample = sample_lod(volume, uvw, lod);
        lod += 1.0F;
        const float a = 1.0F - color[3];
        for (std::size_t i = 0; i < 4; ++i) {
            color.at(i) += sample.at(i) * a;
        }
        dist += radius;
        radius = std::max(0.5F, dist);
    }
    return color;
}

// gi.glsl's cone directions about +z, with their weights.
constexpr std::array<Float3, 6> kHighQualityCones{{{0.0F, 0.0F, 1.0F},
                                                   {0.866025F, 0.0F, 0.5F},
                                                   {0.267617F, 0.823639F, 0.5F},
                                                   {-0.700629F, 0.509037F, 0.5F},
                                                   {-0.700629F, -0.509037F, 0.5F},
                                                   {0.267617F, -0.823639F, 0.5F}}};
constexpr std::array<float, 6> kHighQualityWeights{0.25F, 0.15F, 0.15F, 0.15F, 0.15F, 0.15F};
// NOLINTNEXTLINE(modernize-use-std-numbers): Godot's tan(30 degrees).
constexpr float kHighQualityTan = 0.577F;
constexpr std::array<Float3, 4> kCones{{{0.707107F, 0.0F, 0.707107F},
                                        {0.0F, 0.707107F, 0.707107F},
                                        {-0.707107F, 0.0F, 0.707107F},
                                        {0.0F, -0.707107F, 0.707107F}}};
constexpr float kConeWeight = 0.25F;

// What voxel_gi_compute adds up over a pixel's VoxelGIs.
struct Accumulated {
    Float4 ambient{};
    Float4 reflection{};
    float blend = 0.0F;
};

// voxel_gi_compute: one VoxelGI's cones from a point, weighed by how far inside its box it is.
void compute(const Image& atlas, const MeshVoxelGIUniforms& gi, bool high_quality,
             const Float3& world, const Float3& normal, const Float3& reflection,
             const std::array<Float3, 3>& normal_mat, float roughness, Accumulated& out) {
    const auto direction = [&](const Float3& v) {
        return Float3{dot(xyz(gi.rows[0]), v), dot(xyz(gi.rows[1]), v), dot(xyz(gi.rows[2]), v)};
    };
    Float3 position = add(direction(world), {gi.rows[0][3], gi.rows[1][3], gi.rows[2][3]});
    const Float3 ref_vec = normalize(direction(reflection));
    const Float3 n = normalize(direction(normal));
    const float energy = gi.params[0];
    const float bias = gi.params[1];
    const bool blend_ambient = gi.params[3] != 0.0F;
    position = add(position, scale(n, gi.params[2]));
    const Float3 bounds{gi.size[0], gi.size[1], gi.size[2]};
    float most = 0.0F;
    for (std::size_t i = 0; i < 3; ++i) {
        if (position.at(i) < 0.0F || position.at(i) > bounds.at(i)) {
            return;
        }
        most = std::max(most, std::abs(((position.at(i) / bounds.at(i)) * 2.0F) - 1.0F));
    }
    const float blend = std::clamp(1.0F - most, 0.0F, 1.0F);
    const Volume volume{.atlas = atlas,
                        .gi = gi,
                        .cell_size = {1.0F / bounds[0], 1.0F / bounds[1], 1.0F / bounds[2]},
                        .max_distance = std::sqrt(dot(bounds, bounds))};
    // A cone about +z turned to about the normal (normal_mat), then into the cells' space.
    const auto cone = [&](const Float3& c) {
        return normalize(direction(add(add(scale(normal_mat[0], c[0]), scale(normal_mat[1], c[1])),
                                       scale(normal_mat[2], c[2]))));
    };
    Float4 light{};
    const auto gather = [&](float weight, const Float4& traced) {
        for (std::size_t i = 0; i < 4; ++i) {
            light.at(i) += weight * traced.at(i);
        }
    };
    if (high_quality) {
        for (std::size_t i = 0; i < kHighQualityCones.size(); ++i) {
            gather(
                kHighQualityWeights.at(i),
                cone_trace(volume, position, cone(kHighQualityCones.at(i)), kHighQualityTan, bias));
        }
    } else {
        for (const Float3& c : kCones) {
            gather(kConeWeight, cone_trace_45_degrees(volume, position, cone(c), bias));
        }
    }
    Float4 irradiance =
        cone_trace(volume, position, ref_vec,
                   std::tan(roughness * 0.5F * std::numbers::pi_v<float> * 0.99F), bias);
    for (Float4* traced : {&light, &irradiance}) {
        for (std::size_t i = 0; i < 3; ++i) {
            traced->at(i) *= energy;
        }
        if (!blend_ambient) {
            (*traced)[3] = 1.0F;
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        out.ambient.at(i) += light.at(i) * blend;
        out.reflection.at(i) += irradiance.at(i) * blend;
    }
    out.blend += blend;
}

} // namespace

VoxelGILight trace_voxel_gis(const MeshSurfaceUniforms& surface, bool high_quality,
                             const Image& atlas, const Float3& world, const Float3& normal,
                             const Float3& view, float roughness) {
    // process_gi: the reflection of the view, the roughness squared, and a frame about the normal.
    const Float3 reflection =
        normalize(add(scale(normal, 2.0F * dot(normal, view)), scale(view, -1.0F)));
    roughness *= roughness;
    const Float3 v0 =
        std::abs(normal[2]) < 0.999F ? Float3{0.0F, 0.0F, 1.0F} : Float3{0.0F, 1.0F, 0.0F};
    const Float3 tangent = normalize(cross(v0, normal));
    const Float3 bitangent = normalize(cross(tangent, normal));
    const std::array<Float3, 3> normal_mat{tangent, bitangent, normal};
    Accumulated sum;
    const auto count = std::min(static_cast<std::size_t>(surface.voxel_gi[0]), std::size_t{2});
    for (std::size_t i = 0; i < count; ++i) {
        compute(atlas, surface.voxel_gis.at(i), high_quality, world, normal, reflection, normal_mat,
                roughness, sum);
    }
    if (sum.blend > 0.0F) {
        for (std::size_t i = 0; i < 4; ++i) {
            sum.ambient.at(i) /= sum.blend;
            sum.reflection.at(i) /= sum.blend;
        }
    }
    return {.ambient = sum.ambient, .reflection = sum.reflection};
}

} // namespace slash::render::detail
