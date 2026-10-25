// Mesh LODs, as Godot 4.6's importer generates them (ImporterMesh::generate_lods in
// scene/resources/3d/importer_mesh.cpp), with meshoptimizer 1.0's simplifier, and as its renderers
// pick one (RenderForwardClustered::_fill_render_list and MeshStorage::mesh_surface_get_lod).

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <meshoptimizer.h>
#include <numbers>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kCmpEpsilon = 0.00001F;                   // Godot's CMP_EPSILON
constexpr float kCmpEpsilon2 = kCmpEpsilon * kCmpEpsilon; // and CMP_EPSILON2

// Godot's Vector3 as a HashMap key: equal when each part is, so 0 and -0 are one key.
struct PositionHash {
    std::size_t operator()(const std::array<float, 3>& p) const noexcept {
        std::size_t hash = 0;
        for (const float value : p) {
            const auto bits = slash::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value);
            hash = (hash * 1000003U) ^ bits;
        }
        return hash;
    }
};

// Math::is_equal_approx.
bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const float tolerance = std::max(kCmpEpsilon * std::abs(a), kCmpEpsilon);
    return std::abs(a - b) < tolerance;
}

bool colors_close(const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (!equal_approx(static_cast<float>(a.at(c)) / 255.0F,
                          static_cast<float>(b.at(c)) / 255.0F)) {
            return false;
        }
    }
    return true;
}

float distance_squared(const std::array<float, 2>& a, const std::array<float, 2>& b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    return (x * x) + (y * y);
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

} // namespace

void generate_lods(ModelSurface& surface, std::span<const world::Affine3D> bones,
                   float normal_merge_angle) {
    surface.lods.clear();
    const std::vector<render::Vertex3D>& vertices = surface.vertices;
    const std::vector<std::uint32_t>& indices = surface.indices;
    const std::size_t index_count = indices.size();
    const std::size_t vertex_count = vertices.size();
    if (index_count == 0 || index_count % 3 != 0) {
        return;
    }

    // A skinned surface is simplified as its skin's rest pose places it (VERTEX_SKIN_FUNC): each
    // position by its bones' transforms, weighted. Normals stay as they are.
    std::vector<std::array<float, 3>> positions(vertex_count);
    const bool deformable = !surface.weights.empty();
    for (std::size_t j = 0; j < vertex_count; ++j) {
        positions[j] = vertices[j].position;
        if (!deformable || bones.empty() || j >= surface.weights.size()) {
            continue;
        }
        const Vec3 p{vertices[j].position[0], vertices[j].position[1], vertices[j].position[2]};
        Vec3 skinned;
        const VertexWeights& w = surface.weights[j];
        for (std::size_t k = 0; k < w.weights.size(); ++k) {
            const float weight = w.weights.at(k);
            if (weight < std::numeric_limits<float>::epsilon() || w.joints.at(k) >= bones.size()) {
                continue;
            }
            skinned = skinned + (bones[w.joints.at(k)].apply(p) * weight);
        }
        positions[j] = {skinned.x, skinned.y, skinned.z};
    }

    // Godot's importer keeps colors only where the file has them; a surface all white has none.
    const bool colors = std::ranges::any_of(vertices, [](const render::Vertex3D& v) {
        return v.rgba != std::array<std::uint8_t, 4>{255, 255, 255, 255};
    });

    // Corners at one position merge when they agree on everything else that shows, so the
    // simplifier sees one vertex there.
    const float normal_merge_threshold = cos_sin(normal_merge_angle * (kPi / 180.0F)).x;
    std::unordered_map<std::array<float, 3>, std::vector<std::pair<std::uint32_t, std::uint32_t>>,
                       PositionHash>
        unique_vertices;
    std::vector<std::uint32_t> vertex_remap;
    std::vector<std::uint32_t> vertex_inverse_remap;
    std::vector<std::array<float, 3>> merged_vertices;
    std::vector<std::array<float, 3>> merged_normals;
    std::vector<int> merged_normals_counts;
    vertex_remap.reserve(vertex_count);
    for (std::size_t j = 0; j < vertex_count; ++j) {
        const std::array<float, 3>& v = positions[j];
        const render::Vertex3D& vertex = vertices[j];
        auto& close_verts = unique_vertices[v];
        bool found = false;
        for (const auto& [first, second] : close_verts) {
            const render::Vertex3D& other = vertices[second];
            const bool is_uvs_close = distance_squared(vertex.uv, other.uv) < kCmpEpsilon2;
            const bool is_uv2s_close = distance_squared(vertex.uv2, other.uv2) < kCmpEpsilon2;
            const bool is_tang_aligned = (vertex.tangent[3] < 0.0F) == (other.tangent[3] < 0.0F);
            const bool is_normals_close = dot(other.normal, vertex.normal) > normal_merge_threshold;
            const bool is_col_close = !colors || colors_close(vertex.rgba, other.rgba);
            if (is_uvs_close && is_uv2s_close && is_normals_close && is_tang_aligned &&
                is_col_close) {
                vertex_remap.push_back(first);
                // As Godot adds them: the normal of the corner merged into, again.
                for (std::size_t c = 0; c < 3; ++c) {
                    merged_normals[first].at(c) += other.normal.at(c);
                }
                ++merged_normals_counts[first];
                found = true;
                break;
            }
        }
        if (!found) {
            const auto vcount = static_cast<std::uint32_t>(merged_vertices.size());
            close_verts.emplace_back(vcount, static_cast<std::uint32_t>(j));
            vertex_inverse_remap.push_back(static_cast<std::uint32_t>(j));
            merged_vertices.push_back(v);
            vertex_remap.push_back(vcount);
            merged_normals.push_back(vertex.normal);
            merged_normals_counts.push_back(1);
        }
    }

    std::vector<std::uint32_t> merged_indices(index_count);
    for (std::size_t j = 0; j < index_count; ++j) {
        merged_indices[j] = vertex_remap[indices[j]];
    }
    const std::size_t merged_vertex_count = merged_vertices.size();
    for (std::size_t j = 0; j < merged_vertex_count; ++j) {
        for (float& n : merged_normals[j]) {
            n /= static_cast<float>(merged_normals_counts[j]);
        }
    }

    const float scale = meshopt_simplifyScale(merged_vertices.front().data(), merged_vertex_count,
                                              sizeof(float) * 3);

    // Normals weigh in, and colors where the surface has them.
    constexpr std::size_t kAttribCount = 6;
    std::array<float, kAttribCount> attrib_weights{1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F};
    if (colors) {
        attrib_weights[3] = attrib_weights[4] = attrib_weights[5] = 1.0F;
    }
    std::vector<float> merged_attribs(merged_vertex_count * kAttribCount, 0.0F);
    for (std::size_t j = 0; j < merged_vertex_count; ++j) {
        float* attribs = merged_attribs.data() + (j * kAttribCount);
        attribs[0] = merged_normals[j][0];
        attribs[1] = merged_normals[j][1];
        attribs[2] = merged_normals[j][2];
        if (colors) {
            const render::Vertex3D& vertex = vertices[vertex_inverse_remap[j]];
            attribs[3] = static_cast<float>(vertex.rgba[0]) / 255.0F;
            attribs[4] = static_cast<float>(vertex.rgba[1]) / 255.0F;
            attribs[5] = static_cast<float>(vertex.rgba[2]) / 255.0F;
        }
    }

    // Each LOD halves the last one's triangles, until the simplifier can't or the error grows
    // past what an LOD could be picked at.
    constexpr float kMaxMeshError = 1.0F;
    constexpr std::size_t kMinTargetIndices = 12;
    std::vector<std::uint32_t> current_indices = merged_indices;
    float current_error = 0.0F;
    bool allow_prune = true;
    while (current_indices.size() > kMinTargetIndices * 2) {
        const std::size_t current_index_count = current_indices.size();
        const std::size_t target_index_count =
            std::max(((current_index_count / 3) / 2) * 3, kMinTargetIndices);
        std::vector<std::uint32_t> new_indices(current_index_count);
        // Sparse doesn't change the result but speeds up later steps; the border is locked in
        // case the mesh is one of several material subsets.
        unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyLockBorder;
        if (allow_prune) {
            options |= meshopt_SimplifyPrune; // small disconnected parts go
        }
        if (deformable) {
            options |= meshopt_SimplifyRegularize; // more regular triangles deform better
        }
        float step_error = 0.0F;
        const std::size_t new_index_count = meshopt_simplifyWithAttributes(
            new_indices.data(), current_indices.data(), current_index_count,
            merged_vertices.front().data(), merged_vertex_count, sizeof(float) * 3,
            merged_attribs.data(), sizeof(float) * kAttribCount, attrib_weights.data(),
            kAttribCount, nullptr, target_index_count, kMaxMeshError, options, &step_error);
        if (new_index_count == 0 && allow_prune) {
            // Pruning took everything; without it there may still be one coarser LOD.
            allow_prune = false;
            continue;
        }
        // The error only grows, by at least half each step, which limits how far LODs pop.
        current_error = std::max(current_error * 1.5F, step_error);
        new_indices.resize(new_index_count);
        current_indices = new_indices;
        if (new_index_count == 0 || static_cast<float>(new_index_count) >=
                                        static_cast<float>(current_index_count) * 0.75F) {
            break;
        }
        if (current_error > kMaxMeshError) {
            break;
        }
        for (std::uint32_t& index : new_indices) {
            index = vertex_inverse_remap[index];
        }
        surface.lods.push_back({.distance = std::max(current_error * scale, kCmpEpsilon2),
                                .indices = std::move(new_indices)});
    }
    std::ranges::stable_sort(surface.lods, {}, &SurfaceLod::distance);
}

std::uint32_t pick_lod(std::span<const SurfaceLod> lods, float model_scale, float distance,
                       float threshold) {
    std::uint32_t current = 0;
    for (std::size_t i = 0; i < lods.size(); ++i) {
        const float screen_size = lods[i].distance * model_scale / distance;
        if (screen_size > threshold) {
            break;
        }
        current = static_cast<std::uint32_t>(i + 1);
    }
    return current;
}

} // namespace slash::scene3d
