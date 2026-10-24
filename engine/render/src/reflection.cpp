#include "reflection.hpp"

#include "mesh_shading.hpp"

#include "slash/core/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace slash::render {

namespace {

using detail::Float3;

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

// What a cube's face looks along, and the directions its columns and rows run.
struct FaceAxes {
    Float3 n;
    Float3 a;
    Float3 b;
};
FaceAxes face_axes(std::uint32_t face) {
    const Float3 n = detail::cube_direction(face, 0.0F, 0.0F);
    return {.n = n,
            .a = sub(detail::cube_direction(face, 1.0F, 0.0F), n),
            .b = sub(detail::cube_direction(face, 0.0F, 1.0F), n)};
}

Float3 mirror(const Float3& v) {
    return {v[0], v[1], -v[2]};
}

} // namespace

std::array<float, 16> probe_face_view(const std::array<float, 16>& transform,
                                      const std::array<float, 3>& origin_offset,
                                      std::uint32_t face) {
    // The face's pixel at s, t (across its columns and rows) must hold what lies toward
    // mirror(n + s a + t b) from the probe, and a camera sees (s, -t, -1) there: so the camera's
    // x, y and z axes are mirror(a), mirror(-b) and mirror(-n) in the probe's space, a turn with
    // no mirroring.
    const FaceAxes axes = face_axes(face);
    const std::array<Float3, 3> local{mirror(axes.a), mirror({-axes.b[0], -axes.b[1], -axes.b[2]}),
                                      mirror({-axes.n[0], -axes.n[1], -axes.n[2]})};
    // The camera's axes and place in the world.
    std::array<Float3, 3> world{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t row = 0; row < 3; ++row) {
            world.at(axis).at(row) = (transform.at(row) * local.at(axis)[0]) +
                                     (transform.at(4 + row) * local.at(axis)[1]) +
                                     (transform.at(8 + row) * local.at(axis)[2]);
        }
    }
    Float3 eye{};
    for (std::size_t row = 0; row < 3; ++row) {
        eye.at(row) = (transform.at(row) * origin_offset[0]) +
                      (transform.at(4 + row) * origin_offset[1]) +
                      (transform.at(8 + row) * origin_offset[2]) + transform.at(12 + row);
    }
    // Its inverse: the axes as rows, and the place moved back.
    std::array<float, 16> out{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t column = 0; column < 3; ++column) {
            out.at((column * 4) + axis) = world.at(axis).at(column);
        }
        out.at(12 + axis) = -dot(world.at(axis), eye);
    }
    out[15] = 1.0F;
    return out;
}

std::array<float, 16> probe_face_projection(const std::array<float, 3>& size,
                                            const std::array<float, 3>& origin_offset,
                                            float max_distance, std::uint32_t face) {
    // Godot's _render_reflection_probe_step keeps the furthest of max_distance and each side it
    // has drawn so far, in its order +x, -x, +y, -y, +z, -z. Since our faces are mirrored along z,
    // face +z is its -z.
    constexpr std::array<std::uint32_t, 6> kGodotStep{0, 1, 2, 3, 5, 4};
    constexpr std::array<Float3, 6> kNormals{{{1.0F, 0.0F, 0.0F},
                                              {-1.0F, 0.0F, 0.0F},
                                              {0.0F, 1.0F, 0.0F},
                                              {0.0F, -1.0F, 0.0F},
                                              {0.0F, 0.0F, 1.0F},
                                              {0.0F, 0.0F, -1.0F}}};
    const Float3 extents{size[0] * 0.5F, size[1] * 0.5F, size[2] * 0.5F};
    float far = max_distance;
    for (std::uint32_t step = 0; step <= kGodotStep.at(face); ++step) {
        const Float3& normal = kNormals.at(step);
        const Float3 edge{normal[0] * extents[0], normal[1] * extents[1], normal[2] * extents[2]};
        far = std::max(far, std::abs(dot(normal, edge) - dot(normal, origin_offset)));
    }
    return perspective(90.0F, 1.0F, 0.01F, far);
}

ReflectionAtlasSettings reflection_atlas_settings_from_json(const json::Value& value) {
    ReflectionAtlasSettings out;
    if (!value.is_object()) {
        return out;
    }
    if (const json::Value* found = value.find("reflection_size");
        found != nullptr && found->is_int()) {
        const std::int64_t size = found->as_int();
        if (size >= 32 && size <= 2048 && std::has_single_bit(static_cast<std::uint64_t>(size))) {
            out.reflection_size = static_cast<std::uint32_t>(size);
        }
    }
    if (const json::Value* found = value.find("reflection_count");
        found != nullptr && found->is_int()) {
        const std::int64_t count = found->as_int();
        if (count >= 1 && count <= 4096) {
            out.reflection_count = static_cast<std::uint32_t>(count);
        }
    }
    return out;
}

bool voxel_gi_high_quality(const json::Value& rendering) {
    const json::Value* found =
        rendering.is_object() ? rendering.find("global_illumination") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("voxel_gi") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("quality") : nullptr;
    return found != nullptr && found->is_string() && found->as_string() == "high";
}

} // namespace slash::render

namespace slash::render::detail {

namespace {

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 normalized(const Float3& v) {
    const float length = std::sqrt(dot3(v, v));
    return length > 0.0F ? Float3{v[0] / length, v[1] / length, v[2] / length} : v;
}

// rows times (v, w).
Float3 local(const std::array<Float4, 3>& rows, const Float3& v, float w) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = (rows.at(i)[0] * v[0]) + (rows.at(i)[1] * v[1]) + (rows.at(i)[2] * v[2]) +
                    (rows.at(i)[3] * w);
    }
    return out;
}

// A probe's cube read along a direction in its space: the cube holds it mirrored along z.
Float3 sample_probe(const CubeImage& cube, const Float3& direction, float level) {
    const Float4 texel = cube.sample({direction[0], direction[1], -direction[2]}, level);
    return {texel[0], texel[1], texel[2]};
}

} // namespace

std::vector<MeshProbeUniforms> probe_uniforms(const Reflections3D& reflections) {
    std::vector<MeshProbeUniforms> out;
    out.reserve(reflections.probes.size());
    for (const ReflectionProbe3D& probe : reflections.probes) {
        MeshProbeUniforms u;
        const std::array<float, 16>& m = probe.transform;
        const Float3 place{m[12], m[13], m[14]};
        for (std::size_t row = 0; row < 3; ++row) {
            const Float3 axis{m.at(row * 4), m.at((row * 4) + 1), m.at((row * 4) + 2)};
            u.rows.at(row) = {axis[0], axis[1], axis[2], -dot3(axis, place)};
        }
        u.extents = {probe.size[0] * 0.5F, probe.size[1] * 0.5F, probe.size[2] * 0.5F,
                     probe.intensity};
        u.offset = {probe.origin_offset[0], probe.origin_offset[1], probe.origin_offset[2],
                    probe.blend_distance};
        u.ambient = {srgb_to_linear_exact(probe.ambient_color[0]) * probe.ambient_color_energy,
                     srgb_to_linear_exact(probe.ambient_color[1]) * probe.ambient_color_energy,
                     srgb_to_linear_exact(probe.ambient_color[2]) * probe.ambient_color_energy,
                     static_cast<float>(probe.ambient_mode)};
        u.options = {probe.box_projection ? 1.0F : 0.0F, static_cast<float>(probe.cube),
                     static_cast<float>(probe.reflection_mask), 0.0F};
        out.push_back(u);
    }
    return out;
}

Float4 reflections_uniform(const Reflections3D& reflections) {
    return {static_cast<float>(reflections.probes.size()), kReflectionMaxLevel,
            reflections.exposure, 0.0F};
}

std::uint32_t probe_levels(std::uint32_t size) {
    return radiance_levels(size);
}

ProbeLight reflect_probes(std::span<const MeshProbeUniforms> probes,
                          std::span<const CubeImage> cubes, float exposure, const Float3& world,
                          const Float3& normal, const Float3& view, float roughness,
                          std::uint32_t layers) {
    ProbeLight out;
    Float4& reflection_accum = out.reflection;
    Float4& ambient_accum = out.ambient;
    // Godot's ref_vec for probes: the reflection, bent toward the normal by roughness⁴.
    const float d = dot3(view, normal);
    Float3 ref_vec = normalized({(2.0F * d * normal[0]) - view[0], (2.0F * d * normal[1]) - view[1],
                                 (2.0F * d * normal[2]) - view[2]});
    const float bend = roughness * roughness * roughness * roughness;
    for (std::size_t i = 0; i < 3; ++i) {
        ref_vec.at(i) += (normal.at(i) - ref_vec.at(i)) * bend;
    }
    // Godot walks its cluster's words of probe bits from the first, each from its highest bit:
    // the frame's probes are largest first, so the smallest of each 32 is taken first.
    const std::size_t count = probes.size();
    for (std::size_t index = 0; index < ((count + 31) / 32) * 32; ++index) {
        const std::size_t at = ((index / 32) * 32) + (31 - (index % 32));
        if (at >= count) {
            continue;
        }
        const MeshProbeUniforms& probe = probes[at];
        if ((static_cast<std::uint32_t>(probe.options[2]) & layers) == 0) {
            continue;
        }
        if (reflection_accum[3] >= 1.0F && ambient_accum[3] >= 1.0F) {
            break;
        }
        // Godot's reflection_process.
        const Float3 extents{probe.extents[0], probe.extents[1], probe.extents[2]};
        const Float3 local_pos = local(probe.rows, world, 1.0F);
        if (std::abs(local_pos[0]) > extents[0] || std::abs(local_pos[1]) > extents[1] ||
            std::abs(local_pos[2]) > extents[2]) {
            continue;
        }
        float blend = 1.0F;
        const float blend_distance = probe.offset[3];
        if (blend_distance != 0.0F) {
            float product = 1.0F;
            for (std::size_t i = 0; i < 3; ++i) {
                const float axis = std::min(blend_distance, extents.at(i));
                const float t = (std::abs(local_pos.at(i)) - extents.at(i) + axis) / axis;
                product *= std::clamp(1.0F - t, 0.0F, 1.0F);
            }
            blend = product * product;
        }
        const CubeImage& cube = cubes[static_cast<std::size_t>(probe.options[1])];
        const float intensity = probe.extents[3];
        if (intensity > 0.0F && reflection_accum[3] < 1.0F) {
            Float3 local_ref_vec = local(probe.rows, ref_vec, 0.0F);
            if (probe.options[0] != 0.0F) {
                const Float3 nrdir = normalized(local_ref_vec);
                float fa = 0.0F;
                for (std::size_t i = 0; i < 3; ++i) {
                    const float rbmax = (extents.at(i) - local_pos.at(i)) / nrdir.at(i);
                    const float rbmin = (-extents.at(i) - local_pos.at(i)) / nrdir.at(i);
                    // A direction along an axis's walls never meets them (mesh.hlsl).
                    float rb = std::numeric_limits<float>::max();
                    if (nrdir.at(i) > 0.0F) {
                        rb = rbmax;
                    } else if (nrdir.at(i) < 0.0F) {
                        rb = rbmin;
                    }
                    fa = i == 0 ? rb : std::min(fa, rb);
                }
                for (std::size_t i = 0; i < 3; ++i) {
                    local_ref_vec.at(i) = local_pos.at(i) + (nrdir.at(i) * fa) - probe.offset.at(i);
                }
            }
            const float weight = std::max(0.0F, blend - reflection_accum[3]);
            const Float3 texel =
                sample_probe(cube, local_ref_vec, std::sqrt(roughness) * kReflectionMaxLevel);
            for (std::size_t i = 0; i < 3; ++i) {
                reflection_accum.at(i) += texel.at(i) * exposure * intensity * weight;
            }
            reflection_accum[3] += weight;
        }
        if (ambient_accum[3] >= 1.0F) {
            continue;
        }
        const auto mode =
            static_cast<ReflectionAmbient>(static_cast<std::uint32_t>(probe.ambient[3]));
        const float weight = std::max(0.0F, blend - ambient_accum[3]);
        if (mode == ReflectionAmbient::Environment) {
            const Float3 texel =
                sample_probe(cube, local(probe.rows, normal, 0.0F), kReflectionMaxLevel);
            for (std::size_t i = 0; i < 3; ++i) {
                ambient_accum.at(i) += texel.at(i) * exposure * weight;
            }
            ambient_accum[3] += weight;
        } else if (mode == ReflectionAmbient::Color) {
            for (std::size_t i = 0; i < 3; ++i) {
                ambient_accum.at(i) += probe.ambient.at(i) * weight;
            }
            ambient_accum[3] += weight;
        }
    }
    return out;
}

} // namespace slash::render::detail
