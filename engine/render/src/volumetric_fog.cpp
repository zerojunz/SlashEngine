// Volumetric fog, as Godot 4.6's Forward+ renderer makes it: the froxels' size and uniforms as
// Fog::volumetric_fog_update sets them, and the CPU twins of volumetric_fog_process.glsl's density,
// filter and fog passes, which shaders/volumetric_fog.hlsl follows line for line; a change to one
// goes in the other. pow is exp(y * ln(x)) through core/math, so the software device's fog is the
// same on every machine. Float math, built with -ffp-contract=off.

#include "volumetric_fog.hpp"

#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "sky_shading.hpp"

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace slash::render {

VolumetricFogSettings volumetric_fog_settings_from_json(const json::Value& value) {
    VolumetricFogSettings out;
    if (!value.is_object()) {
        return out;
    }
    const auto size = [&](std::string_view key, std::uint32_t& field) {
        if (const json::Value* found = value.find(key); found != nullptr && found->is_number()) {
            const double read = found->as_double();
            if (read >= 16.0 && read <= 512.0 && read == std::floor(read)) {
                field = static_cast<std::uint32_t>(read);
            }
        }
    };
    size("volume_size", out.volume_size);
    size("volume_depth", out.volume_depth);
    if (const json::Value* found = value.find("use_filter"); found != nullptr && found->is_bool()) {
        out.use_filter = found->as_bool();
    }
    return out;
}

} // namespace slash::render

namespace slash::render::detail {

namespace {

// Godot's halton_map: each frame's point in its froxels, when the fog reprojects.
constexpr std::array<Float3, kFogTemporalFrames> kHalton{{{0.5F, 0.33333333F, 0.2F},
                                                          {0.25F, 0.66666667F, 0.4F},
                                                          {0.75F, 0.11111111F, 0.6F},
                                                          {0.125F, 0.44444444F, 0.8F},
                                                          {0.625F, 0.77777778F, 0.04F},
                                                          {0.375F, 0.22222222F, 0.24F},
                                                          {0.875F, 0.55555556F, 0.44F},
                                                          {0.0625F, 0.88888889F, 0.64F},
                                                          {0.5625F, 0.03703704F, 0.84F},
                                                          {0.3125F, 0.37037037F, 0.08F},
                                                          {0.8125F, 0.7037037F, 0.28F},
                                                          {0.1875F, 0.14814815F, 0.48F},
                                                          {0.6875F, 0.48148148F, 0.68F},
                                                          {0.4375F, 0.81481481F, 0.88F},
                                                          {0.9375F, 0.25925926F, 0.12F},
                                                          {0.03125F, 0.59259259F, 0.32F}}};

// Godot's INV_FOG_FADE: how soon light in the fog fades where a shadow occludes it.
constexpr float kInvFogFade = 10.0F;
// Godot's gauss weights for the blur.
constexpr std::array<float, 7> kGauss{0.071303F, 0.131514F, 0.189879F, 0.214607F,
                                      0.189879F, 0.131514F, 0.071303F};

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

Float3 mul(const Float3& a, const Float3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

float length(const Float3& a) {
    return std::sqrt(dot3(a, a));
}

Float3 normalize(const Float3& a) {
    return scale(a, 1.0F / length(a));
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}

Float4 lerp4(const Float4& a, const Float4& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

// The shader's smooth: smoothstep for edges in either order.
float smooth(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// The matrix times a point or direction (w 1 or 0), column-major.
Float4 transform(const Mat4& m, const Float4& v) {
    Float4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                      (m.at(12 + row) * v[3]);
    }
    return out;
}

Mat4 to_float(const std::array<double, 16>& m) {
    Mat4 out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out.at(i) = static_cast<float>(m.at(i));
    }
    return out;
}

std::size_t froxel(const std::array<std::uint32_t, 4>& size, std::uint32_t x, std::uint32_t y,
                   std::uint32_t z) {
    return (((std::size_t{z} * size[1]) + y) * size[0]) + x;
}

// A volume of size read at a texture coordinate as a 3D texture with a linear sampler that
// clamps to the edge reads it.
Float4 sample_volume(std::span<const Float4> volume, const std::array<std::uint32_t, 3>& size,
                     const Float3& uvw) {
    std::array<std::uint32_t, 3> low{};
    std::array<std::uint32_t, 3> high{};
    Float3 t{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto n = static_cast<float>(size.at(axis));
        const float p = std::clamp((uvw.at(axis) * n) - 0.5F, 0.0F, n - 1.0F);
        const float base = std::floor(p);
        low.at(axis) = static_cast<std::uint32_t>(base);
        high.at(axis) = std::min(low.at(axis) + 1, size.at(axis) - 1);
        t.at(axis) = p - base;
    }
    const auto at = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return volume[(((std::size_t{z} * size[1]) + y) * size[0]) + x];
    };
    const Float4 a = lerp4(at(low[0], low[1], low[2]), at(high[0], low[1], low[2]), t[0]);
    const Float4 b = lerp4(at(low[0], high[1], low[2]), at(high[0], high[1], low[2]), t[0]);
    const Float4 c = lerp4(at(low[0], low[1], high[2]), at(high[0], low[1], high[2]), t[0]);
    const Float4 d = lerp4(at(low[0], high[1], high[2]), at(high[0], high[1], high[2]), t[0]);
    return lerp4(lerp4(a, b, t[1]), lerp4(c, d, t[1]), t[2]);
}

// A shadow atlas's depth at a texture coordinate, filtered linearly from its four texels, as
// Godot's fog reads its atlases with a linear sampler.
float atlas_depth(const ShadowAtlas& atlas, float u, float v) {
    if (atlas.size == 0) {
        return 0.0F;
    }
    const auto s = static_cast<float>(atlas.size);
    const float x = (std::clamp(u, -1.0F, 2.0F) * s) - 0.5F;
    const float y = (std::clamp(v, -1.0F, 2.0F) * s) - 0.5F;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto last = static_cast<std::int64_t>(atlas.size) - 1;
    const auto texel = [&](float column, float row) {
        const auto cx = std::clamp(static_cast<std::int64_t>(column), std::int64_t{0}, last);
        const auto cy = std::clamp(static_cast<std::int64_t>(row), std::int64_t{0}, last);
        return atlas
            .depths[(static_cast<std::size_t>(cy) * atlas.size) + static_cast<std::size_t>(cx)];
    };
    const float top = lerp(texel(x0, y0), texel(x0 + 1.0F, y0), fx);
    const float bottom = lerp(texel(x0, y0 + 1.0F), texel(x0 + 1.0F, y0 + 1.0F), fx);
    return lerp(top, bottom, fy);
}

// Godot's get_omni_attenuation.
float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = std::max(1.0F - nd, 0.0F);
    nd *= nd;
    return nd * power(std::max(distance, 0.0001F), -decay);
}

// Godot's henyey_greenstein, its power of 1.5 as a root.
float henyey_greenstein(float cos_theta, float g) {
    const float k = 0.0795774715459F; // 1 / (4 * pi)
    const float base = 1.0F + (g * g) - (2.0F * g * cos_theta);
    return k * (1.0F - (g * g)) / (base * std::sqrt(base));
}

// The place in the camera's space of a point of the fog's unit volume (its depth spread), as
// Godot's density pass places it.
Float3 view_place(const FogUniforms& u, const Float3& unit) {
    const float t = unit[2];
    Float3 view{((unit[0] * 2.0F) - 1.0F) * lerp(u.frustum[0], u.frustum[2], t),
                ((unit[1] * 2.0F) - 1.0F) * lerp(u.frustum[1], u.frustum[3], t), -u.params[0] * t};
    view[1] = -view[1];
    return view;
}

// Godot's directional shadow in the fog: its cascade's atlas, faded softly behind what it
// occludes, then faded out with distance and by its opacity.
float directional_fog_shadow(const MeshDirectionalShadowUniforms& shadow, const ShadowAtlas& atlas,
                             const Float3& world, float view_z) {
    const float depth_z = -view_z;
    const auto count = static_cast<std::size_t>(shadow.params[3]);
    std::size_t cascade = 0;
    while (cascade + 1 < count && !(depth_z < shadow.splits.at(cascade))) {
        ++cascade;
    }
    const Mat4& m = shadow.matrices.at(cascade);
    Float4 at = transform(m, {world[0], world[1], world[2], 1.0F});
    at = {at[0] / at[3], at[1] / at[3], at[2] / at[3], 1.0F};
    // The cascade's depth range in world units: its matrix's z row takes a unit of depth to one
    // over it.
    const float z_range = 1.0F / length({m[2], m[6], m[10]});
    const float depth = atlas_depth(atlas, at[0], at[1]);
    float lit = slash::exp(std::min(0.0F, at[2] - depth) * z_range * kInvFogFade);
    lit = lerp(lit, 1.0F, smooth(shadow.params[0], shadow.params[1], view_z));
    return lerp(1.0F - shadow.params[2], 1.0F, lit);
}

// An omni or spot light's shadow in the fog: its face's atlas tile, the depths there and the
// point's made distances from the light (its projection's near plane is 0.025 or its range), so
// that the fog fades behind what it occludes over meters, as Godot's.
float positional_fog_shadow(const MeshPositionalShadowUniforms& shadow, const ShadowAtlas& atlas,
                            const Float3& world) {
    std::size_t face = 0;
    if (shadow.params[3] > 1.5F) {
        const Float4 local = transform(shadow.light, {world[0], world[1], world[2], 1.0F});
        const Float3 a{std::abs(local[0]), std::abs(local[1]), std::abs(local[2])};
        if (a[0] >= a[1] && a[0] >= a[2]) {
            face = local[0] > 0.0F ? 0 : 1;
        } else if (a[1] >= a[2]) {
            face = local[1] < 0.0F ? 2 : 3;
        } else {
            face = local[2] > 0.0F ? 4 : 5;
        }
    }
    const Float4 at = transform(shadow.faces.at(face), {world[0], world[1], world[2], 1.0F});
    const Float4& rect = shadow.rects.at(face);
    const float u = std::clamp(at[0] / at[3], rect[0], rect[2]);
    const float v = std::clamp(at[1] / at[3], rect[1], rect[3]);
    const float reference = at[2] / at[3];
    const float depth = atlas_depth(atlas, u, v);
    const float far = shadow.position[3];
    const float near = std::min(0.025F, far);
    const auto distance = [&](float z) { return near * far / ((z * (far - near)) + near); };
    const float lit =
        slash::exp(std::min(0.0F, distance(depth) - distance(reference)) * kInvFogFade);
    return lerp(1.0F - shadow.params[1], 1.0F, lit);
}

} // namespace

std::array<std::uint32_t, 3> fog_volume_size(const VolumetricFogSettings& settings,
                                             std::uint32_t width, std::uint32_t height) {
    const float ratio = static_cast<float>(width) /
                        static_cast<float>(std::max<std::uint32_t>((width + height) / 2, 1));
    const auto size = static_cast<float>(settings.volume_size);
    const std::uint32_t across = std::max(static_cast<std::uint32_t>(size * ratio), 1U);
    const std::uint32_t down = std::max(static_cast<std::uint32_t>(size / ratio), 1U);
    const auto most = static_cast<std::uint32_t>(
        std::max<std::size_t>(kMaxFogFroxels / (std::size_t{across} * down), 1));
    return {across, down, std::max(std::min(settings.volume_depth, most), 1U)};
}

bool uses_volumetric_fog(const Scene3D& scene) {
    return scene.volumetric_fog.enabled && scene.method == RenderMethod::ForwardPlus &&
           !scene.capture;
}

void set_volumetric_fog(MeshSceneUniforms& lights, const Scene3D& scene,
                        const std::array<std::uint32_t, 3>& size) {
    const VolumetricFog3D& fog = scene.volumetric_fog;
    lights.volumetric_fog = {1.0F, fog.length > 0.0F ? 1.0F / fog.length : 1.0F,
                             fog.detail_spread > 0.0F ? 1.0F / fog.detail_spread : 1.0F,
                             fog.sky_affect};
    lights.fog_volume = {static_cast<float>(size[0]), static_cast<float>(size[1]),
                         static_cast<float>(size[2]), 0.0F};
}

namespace {

// The camera's frustum as Godot's volumetric_fog_update takes it: Projection's near plane and its
// get_viewport_half_extents and get_far_plane_half_extents, then the fog's own half extents at
// the near plane and at length, as it lerps them.
struct FogFrustum {
    float near = 0.0F;
    float far = 0.0F;
    std::array<float, 2> near_size{};
    std::array<float, 2> fog_near{};
    std::array<float, 2> fog_far{};
};

FogFrustum fog_frustum(const Scene3D& scene) {
    const Mat4& p = scene.projection;
    const bool orthogonal = p[11] == 0.0F && p[15] == 1.0F;
    FogFrustum out;
    out.far = far_plane(p);
    out.near = orthogonal ? (p[14] - 1.0F) / p[10] : p[14] / (p[10] + 1.0F);
    out.near_size = orthogonal ? std::array{1.0F / p[0], 1.0F / p[5]}
                               : std::array{out.near / p[0], out.near / p[5]};
    const std::array<float, 2> far_size =
        orthogonal ? out.near_size : std::array{out.far / p[0], out.far / p[5]};
    const float t = (scene.volumetric_fog.length - out.near) / (out.far - out.near);
    out.fog_far = {lerp(out.near_size[0], far_size[0], t), lerp(out.near_size[1], far_size[1], t)};
    out.fog_near = orthogonal ? out.fog_far
                              : std::array{std::max(out.near_size[0], 0.001F),
                                           std::max(out.near_size[1], 0.001F)};
    return out;
}

// Godot's Fog::_point_get_position_in_froxel_volume: the froxel a point of the world is in, held
// to the volume (and in front of the camera).
std::array<std::int32_t, 3> froxel_of(const Scene3D& scene, const FogFrustum& frustum,
                                      const std::array<std::uint32_t, 3>& size, const Float3& at) {
    const VolumetricFog3D& fog = scene.volumetric_fog;
    Float4 view = transform(scene.camera, {at[0], at[1], at[2], 1.0F});
    view[2] = std::min(view[2], -0.01F);
    view[1] = -view[1];
    Float3 place{};
    place[2] = -view[2] / fog.length;
    place[0] =
        (view[0] /
         (2.0F * ((frustum.fog_near[0] * (1.0F - place[2])) + (frustum.fog_far[0] * place[2])))) +
        0.5F;
    place[1] =
        (view[1] /
         (2.0F * ((frustum.fog_near[1] * (1.0F - place[2])) + (frustum.fog_far[1] * place[2])))) +
        0.5F;
    place[2] = power(place[2], 1.0F / fog.detail_spread);
    std::array<std::int32_t, 3> out{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto n = static_cast<float>(size.at(axis));
        out.at(axis) = static_cast<std::int32_t>(std::clamp((place.at(axis) * n) - 0.5F, 0.0F, n));
    }
    return out;
}

std::array<std::array<std::int32_t, 3>, 2>
volume_froxels(const Scene3D& scene, const FogFrustum& frustum, const FogVolume3D& volume,
               const std::array<std::uint32_t, 3>& size) {
    const auto across = static_cast<std::int32_t>(size[0]);
    const auto down = static_cast<std::int32_t>(size[1]);
    const auto deep = static_cast<std::int32_t>(size[2]);
    if (volume.shape == FogVolumeShape::World) {
        return {{{0, 0, 0}, {across, down, deep}}};
    }
    const Mat4& m = volume.transform;
    const Float3 half{volume.size[0] / 2.0F, volume.size[1] / 2.0F, volume.size[2] / 2.0F};
    std::array<std::int32_t, 3> low{across - 1, down - 1, deep - 1};
    std::array<std::int32_t, 3> high{1, 1, 1};
    std::array<std::array<std::int32_t, 3>, 8> froxels{};
    Float3 corner_min{};
    Float3 corner_max{};
    for (std::size_t j = 0; j < 8; ++j) {
        // Godot's order: x, then y, then z flips.
        const Float3 local{(j & 1U) != 0 ? -half[0] : half[0], (j & 2U) != 0 ? -half[1] : half[1],
                           (j & 4U) != 0 ? -half[2] : half[2]};
        const Float3 corner = xyz(transform(m, {local[0], local[1], local[2], 1.0F}));
        froxels.at(j) = froxel_of(scene, frustum, size, corner);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            corner_min.at(axis) =
                j == 0 ? corner.at(axis) : std::min(corner_min.at(axis), corner.at(axis));
            corner_max.at(axis) =
                j == 0 ? corner.at(axis) : std::max(corner_max.at(axis), corner.at(axis));
        }
    }
    // With the camera's near plane in the volume's box, the whole view across is filled.
    const Float3 camera = xyz(transform(to_float(inverse_matrix(scene.camera)), {0, 0, 0, 1.0F}));
    const float expand = length({frustum.near_size[0], frustum.near_size[1], frustum.near});
    bool near = true;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        near = near && camera.at(axis) > corner_min.at(axis) - expand &&
               camera.at(axis) < corner_max.at(axis) + expand;
    }
    if (near) {
        low = {0, 0, 0};
        high = {across, down, high[2]};
        for (const auto& froxel_at : froxels) {
            high[2] = std::max(high[2], froxel_at[2]);
        }
    } else {
        for (const auto& froxel_at : froxels) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                low.at(axis) = std::min(low.at(axis), froxel_at.at(axis));
                high.at(axis) = std::max(high.at(axis), froxel_at.at(axis));
            }
        }
    }
    return {low, high};
}

} // namespace

std::array<std::array<std::int32_t, 3>, 2>
fog_volume_froxels(const Scene3D& scene, const FogVolume3D& volume,
                   const std::array<std::uint32_t, 3>& size) {
    return volume_froxels(scene, fog_frustum(scene), volume, size);
}

FogUniforms volumetric_fog_uniforms(const Scene3D& scene, const MeshSceneUniforms& lights,
                                    std::uint32_t width, std::uint32_t height, FogClock& clock) {
    const VolumetricFog3D& fog = scene.volumetric_fog;
    const std::array<std::uint32_t, 3> size = fog_volume_size(fog.settings, width, height);
    FogUniforms out;
    out.size = {size[0], size[1], size[2], static_cast<std::uint32_t>(FogPass::Density)};

    const FogFrustum frustum = fog_frustum(scene);
    out.frustum = {frustum.fog_near[0], frustum.fog_near[1], frustum.fog_far[0],
                   frustum.fog_far[1]};
    out.params = {fog.length, fog.detail_spread, frustum.far, fog.anisotropy};

    const float density = fog.density;
    out.scattering = {srgb_to_linear_exact(fog.albedo[0]) * density,
                      srgb_to_linear_exact(fog.albedo[1]) * density,
                      srgb_to_linear_exact(fog.albedo[2]) * density, density};
    const float glow = fog.emission_energy * density;
    out.emission = {srgb_to_linear_exact(fog.emission[0]) * glow,
                    srgb_to_linear_exact(fog.emission[1]) * glow,
                    srgb_to_linear_exact(fog.emission[2]) * glow, 0.0F};
    out.ambient = {srgb_to_linear_exact(scene.ambient[0]), srgb_to_linear_exact(scene.ambient[1]),
                   srgb_to_linear_exact(scene.ambient[2]),
                   fog.ambient_inject * scene.ambient_energy};
    // The sky's light, as the meshes' ambient light takes it.
    const bool sky = lights.sky[0] != 0.0F;
    out.sky = {sky ? 1.0F : 0.0F, sky ? lights.sky[3] : 0.0F, lights.sky[2], 0.0F};
    out.sky_rows = lights.sky_rows;
    std::size_t directional = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(lights.camera[3]); ++i) {
        const MeshLightUniforms& light = lights.lights.at(i);
        if (light.direction[3] == 0.0F && directional < out.lights.size()) {
            out.lights.at(directional++) = light;
        }
    }
    out.sky[3] = static_cast<float>(directional);
    out.screen = {static_cast<float>(width), static_cast<float>(height), 0.0F, 0.0F};
    out.cluster = lights.cluster;
    out.view_to_world = to_float(inverse_matrix(scene.camera));
    std::uint32_t volumes = 0;
    for (const FogVolume3D& volume : fog.volumes) {
        if (volumes == kMaxFogVolumes) {
            break;
        }
        const auto [low, high] = volume_froxels(scene, frustum, volume, size);
        if (high[0] <= low[0] || high[1] <= low[1] || high[2] <= low[2]) {
            continue;
        }
        FogVolumeUniforms& v = out.volumes.at(volumes++);
        const bool world = volume.shape == FogVolumeShape::World;
        v.to_local = world ? Mat4{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                  0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}
                           : to_float(inverse_matrix(volume.transform));
        v.position = {volume.transform[12], volume.transform[13], volume.transform[14],
                      static_cast<float>(volume.shape)};
        // Godot hands a world volume the fog's froxels as its extents.
        v.size =
            world ? Float4{2.0F * static_cast<float>(size[0]), 2.0F * static_cast<float>(size[1]),
                           2.0F * static_cast<float>(size[2]), 0.0F}
                  : Float4{volume.size[0], volume.size[1], volume.size[2], 0.0F};
        v.size[3] = volume.material ? 1.0F : 0.0F;
        v.corner = {low[0], low[1], low[2], 0};
        v.corner_end = {high[0], high[1], high[2], 0};
        v.albedo = {srgb_to_linear_exact(volume.albedo[0]), srgb_to_linear_exact(volume.albedo[1]),
                    srgb_to_linear_exact(volume.albedo[2]), volume.density};
        v.glow = {srgb_to_linear_exact(volume.emission[0]),
                  srgb_to_linear_exact(volume.emission[1]),
                  srgb_to_linear_exact(volume.emission[2]), volume.height_falloff};
        v.fade = {volume.edge_fade, 0.0F, 0.0F, 0.0F};
    }
    out.volume_count = {volumes, 0, 0, 0};

    // The frame before's density, when it had the same volume, moved to where it was seen, with
    // this frame's point in its froxels.
    if (fog.temporal_reprojection && clock.frames > 0 && clock.size == size) {
        out.emission[3] = 1.0F;
        const Float3& point = kHalton.at(clock.frames % kFogTemporalFrames);
        out.jitter = {point[0], point[1], point[2], fog.temporal_reprojection_amount};
        out.to_previous = multiply(clock.camera, out.view_to_world);
    }
    clock.frames += 1;
    clock.camera = scene.camera;
    clock.size = size;
    return out;
}

namespace {

// What the fog volumes add to a froxel at (x, y, z), whose point is world, as the shader's
// fog_volumes: the density in 1024ths, then the scattering and emission in Godot's channel units.
struct VolumeSums {
    std::int32_t density = 0;
    std::array<std::uint32_t, 3> scattering{};
    std::array<std::uint32_t, 3> emission{};
};

float volume_sdf(const FogVolumeUniforms& v, const Float3& local) {
    const Float3 half = scale(xyz(v.size), 0.5F);
    switch (static_cast<std::uint32_t>(v.position[3])) {
    case 0: {
        const float k0 = length({local[0] / half[0], local[1] / half[1], local[2] / half[2]});
        const float k1 = length({local[0] / (half[0] * half[0]), local[1] / (half[1] * half[1]),
                                 local[2] / (half[2] * half[2])});
        return k0 * (k0 - 1.0F) / k1;
    }
    case 1: {
        const float inv_height = 1.0F / std::max(0.001F, half[1]);
        const float radius = 1.0F / std::max(0.001F, std::min(half[0], half[2]) * 0.5F);
        const float hypotenuse = std::sqrt((radius * radius) + (inv_height * inv_height));
        const float q = std::sqrt((local[0] * local[0]) + (local[2] * local[2]));
        return std::max(((radius / hypotenuse) * q) +
                            ((inv_height / hypotenuse) * (local[1] - half[1])),
                        -half[1] - local[1]);
    }
    case 2: {
        const float dx = std::abs(std::sqrt((local[0] * local[0]) + (local[2] * local[2]))) -
                         std::min(half[0], half[2]);
        const float dy = std::abs(local[1]) - half[1];
        const float ox = std::max(dx, 0.0F);
        const float oy = std::max(dy, 0.0F);
        return std::min(std::max(dx, dy), 0.0F) + std::sqrt((ox * ox) + (oy * oy));
    }
    case 3: {
        const Float3 q{std::abs(local[0]) - half[0], std::abs(local[1]) - half[1],
                       std::abs(local[2]) - half[2]};
        const Float3 outside{std::max(q[0], 0.0F), std::max(q[1], 0.0F), std::max(q[2], 0.0F)};
        return length(outside) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.0F);
    }
    default:
        return -1.0F;
    }
}

VolumeSums fog_volumes(const FogUniforms& u, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                       const Float3& world) {
    constexpr std::array<std::uint32_t, 3> kLimits{2047, 2047, 1023};
    constexpr Float3 kScatterUnits{2047.0F, 2047.0F, 1023.0F};
    constexpr Float3 kGlowUnits{511.0F, 511.0F, 255.0F};
    constexpr float kLn2 = std::numbers::ln2_v<float>;
    const std::array<std::int32_t, 3> at{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                         static_cast<std::int32_t>(z)};
    VolumeSums sums;
    for (std::size_t i = 0; i < u.volume_count[0] && i < u.volumes.size(); ++i) {
        const FogVolumeUniforms& v = u.volumes.at(i);
        bool inside = true;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            inside =
                inside && at.at(axis) >= v.corner.at(axis) && at.at(axis) < v.corner_end.at(axis);
        }
        if (!inside) {
            continue;
        }
        const Float3 local = xyz(transform(v.to_local, {world[0], world[1], world[2], 1.0F}));
        const auto shape = static_cast<std::uint32_t>(v.position[3]);
        const bool material = v.size[3] != 0.0F;
        const float sdf = volume_sdf(v, local);
        // Godot's default material doesn't read the distance, so its cells outside are culled.
        float cull_mask = 1.0F;
        if (shape <= 3 && !material) {
            cull_mask = 1.0F - smooth(-0.1F, 0.0F, sdf);
        }
        if (!(cull_mask > 0.0F)) {
            continue;
        }
        float density = 1.0F;
        Float3 albedo{1.0F, 1.0F, 1.0F};
        Float3 emission{};
        if (material) {
            const float height = world[1] - v.position[1];
            density = v.albedo[3] * std::clamp(slash::exp(-v.glow[3] * height * kLn2), 0.0F, 1.0F);
            const float edge = std::clamp(
                -2.0F * sdf / std::min(std::min(v.size[0], v.size[1]), v.size[2]), 0.0F, 1.0F);
            density *= power(edge, v.fade[0]);
            albedo = xyz(v.albedo);
            emission = xyz(v.glow);
        }
        density *= cull_mask;
        if (!(std::abs(density) > 0.001F)) {
            continue;
        }
        sums.density += static_cast<std::int32_t>(density * 1024.0F);
        const float amount = std::clamp(density, 0.0F, 1.0F);
        for (std::size_t c = 0; c < 3; ++c) {
            if (material) {
                const float glow = std::clamp(emission.at(c) * amount, 0.0F, 4.0F);
                sums.emission.at(c) = std::min(
                    sums.emission.at(c) + static_cast<std::uint32_t>(glow * kGlowUnits.at(c)),
                    kLimits.at(c));
            }
            const float scattered = std::clamp(albedo.at(c) * amount, 0.0F, 1.0F);
            sums.scattering.at(c) = std::min(
                sums.scattering.at(c) + static_cast<std::uint32_t>(scattered * kScatterUnits.at(c)),
                kLimits.at(c));
        }
    }
    return sums;
}

} // namespace

Float4 fog_density(const FogUniforms& u, const FogMaps& maps, std::span<const Float4> previous,
                   std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const Float3 cell{1.0F / static_cast<float>(u.size[0]), 1.0F / static_cast<float>(u.size[1]),
                      1.0F / static_cast<float>(u.size[2])};
    const Float3 place{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    const float spread = u.params[1];
    Float3 unit = add(mul(place, cell), scale(cell, 0.5F)); // the froxel's center
    unit[2] = power(unit[2], spread);
    Float3 view = view_place(u, unit);

    Float4 reprojected{};
    float reproject_amount = 0.0F;
    if (u.emission[3] != 0.0F && !previous.empty()) {
        // Where the point was in the frame before's unit volume.
        Float4 before = transform(u.to_previous, {view[0], view[1], view[2], 1.0F});
        before[1] = -before[1];
        before[2] /= -u.params[0];
        const float t = before[2];
        before[0] /= lerp(u.frustum[0], u.frustum[2], t);
        before[1] /= lerp(u.frustum[1], u.frustum[3], t);
        before[0] = (before[0] * 0.5F) + 0.5F;
        before[1] = (before[1] * 0.5F) + 0.5F;
        before[2] = power(before[2], 1.0F / spread);
        if (before[0] > 0.0F && before[1] > 0.0F && before[2] > 0.0F && before[0] < 1.0F &&
            before[1] < 1.0F && before[2] < 1.0F) {
            reprojected = sample_volume(previous, {u.size[0], u.size[1], u.size[2]}, xyz(before));
            reproject_amount = u.jitter[3];
            // A froxel that reprojects is seen at the frame's point in it.
            unit = add(mul(place, cell), mul(cell, xyz(u.jitter)));
            unit[2] = power(unit[2], spread);
            view = view_place(u, unit);
        }
    }

    const Float4 world4 = transform(u.view_to_world, {view[0], view[1], view[2], 1.0F});
    const Float3 world = xyz(world4);
    const Float3 camera{u.view_to_world[12], u.view_to_world[13], u.view_to_world[14]};
    const Float3 view_dir = normalize(sub(world, camera));
    const float g = u.params[3];

    // The fog volumes there, over the fog's own density, scattering and emission.
    const VolumeSums sums = fog_volumes(u, x, y, z, world);
    const float total_density =
        std::max(0.0F, u.scattering[3] + (static_cast<float>(sums.density) / 1024.0F));
    Float3 scattering = xyz(u.scattering);
    Float3 emission = xyz(u.emission);
    constexpr Float3 kScatterUnits{2047.0F, 2047.0F, 1023.0F};
    constexpr Float3 kGlowUnits{511.0F, 511.0F, 255.0F};
    for (std::size_t c = 0; c < 3; ++c) {
        scattering.at(c) += static_cast<float>(sums.scattering.at(c)) / kScatterUnits.at(c);
        emission.at(c) += static_cast<float>(sums.emission.at(c)) / kGlowUnits.at(c);
    }
    Float3 total_light{};
    if (total_density > 0.00005F) {
        const auto directional = static_cast<std::size_t>(u.sky[3]);
        for (std::size_t i = 0; i < directional; ++i) {
            const MeshLightUniforms& light = u.lights.at(i);
            const float energy = light.shadow[2];
            if (!(energy > 0.001F)) {
                continue;
            }
            float shadow = 1.0F;
            if (light.shadow[0] >= 0.0F && maps.shadows != nullptr) {
                const MeshDirectionalShadowUniforms& cascades =
                    maps.shadows->directional.at(static_cast<std::size_t>(light.shadow[0]));
                if (cascades.params[2] > 0.001F) {
                    shadow = directional_fog_shadow(cascades, maps.directional, world, view[2]);
                }
            }
            const float phase =
                henyey_greenstein(dot3(view_dir, normalize(xyz(light.direction))), g);
            total_light = add(total_light, scale(xyz(light.color), shadow * phase * energy));
        }

        // The ambient light and the sky's, more so in the direction light scatters.
        if (u.ambient[3] > 0.0F) {
            Float3 isotropic{};
            Float3 anisotropic{};
            if (u.sky[0] != 0.0F && u.sky[1] > 0.0F && maps.radiance != nullptr) {
                const float level = std::min(2.0F + (total_density * (kRadianceMaxLevel - 2.0F)),
                                             kRadianceMaxLevel);
                isotropic =
                    scale(srgb_to_linear(xyz(maps.radiance->sample({0.0F, 1.0F, 0.0F}, level))),
                          u.sky[2]);
                anisotropic = isotropic;
                if (g != 0.0F) {
                    const Float3 ray = scale(view_dir, g > 0.0F ? 1.0F : -1.0F);
                    const Float3 turned{dot3(xyz(u.sky_rows[0]), ray),
                                        dot3(xyz(u.sky_rows[1]), ray),
                                        dot3(xyz(u.sky_rows[2]), ray)};
                    anisotropic =
                        scale(srgb_to_linear(xyz(maps.radiance->sample(turned, level))), u.sky[2]);
                }
            }
            total_light = add(
                total_light,
                scale(lerp3(xyz(u.ambient), lerp3(isotropic, anisotropic, std::abs(g)), u.sky[1]),
                      u.ambient[3]));
        }

        // The omni and spot lights of the froxel's cluster, as the meshes' pixels find theirs.
        if (maps.clusters != nullptr && !maps.clusters->cells.empty()) {
            const LightClusters& clusters = *maps.clusters;
            const auto tile_x = static_cast<std::uint32_t>(unit[0] * u.screen[0]) / kClusterSize;
            const auto tile_y = static_cast<std::uint32_t>(unit[1] * u.screen[1]) / kClusterSize;
            const auto words = static_cast<std::uint32_t>(u.cluster[1]);
            const auto decal_words = static_cast<std::uint32_t>(u.cluster[3]);
            const std::uint32_t stride =
                words + kClusterSlices + (decal_words > 0 ? decal_words + kClusterSlices : 0);
            const std::size_t cell_at =
                ((std::size_t{tile_y} * static_cast<std::uint32_t>(u.cluster[0])) + tile_x) *
                stride;
            const auto slice = static_cast<std::uint32_t>(
                std::clamp(-view[2] * u.cluster[2], 0.0F, static_cast<float>(kClusterSlices - 1)));
            const std::uint32_t range = clusters.cells.at(cell_at + words + slice);
            const std::uint32_t first = range & 0xFFFFU;
            const std::uint32_t end = range >> 16U;
            for (std::uint32_t word = first >> 5U; word < (end + 31U) >> 5U; ++word) {
                std::uint32_t mask =
                    clusters.cells.at(cell_at + word) & range_mask(word, first, end);
                while (mask != 0) {
                    const auto bit = static_cast<std::uint32_t>(std::countr_zero(mask));
                    mask &= mask - 1;
                    const MeshLightUniforms& light = clusters.lights.at((word * 32U) + bit);
                    const Float3 to_light = sub(xyz(light.position), world);
                    const float d = length(to_light);
                    const float energy = light.shadow[2];
                    if (!(energy > 0.001F) || !(d * light.position[3] < 1.0F)) {
                        continue;
                    }
                    float attenuation = omni_attenuation(d, light.position[3], light.params[0]);
                    if (light.direction[3] == 2.0F) {
                        const float cone = light.params[1];
                        const float scos = std::max(
                            dot3(scale(normalize(to_light), -1.0F), xyz(light.direction)), cone);
                        const float rim = std::max(0.0001F, (1.0F - scos) / (1.0F - cone));
                        attenuation *= 1.0F - power(rim, light.params[2]);
                    }
                    float shadow = 1.0F;
                    if (light.shadow[0] >= 0.0F && maps.shadows != nullptr) {
                        const MeshPositionalShadowUniforms& positional =
                            maps.shadows->positional.at(static_cast<std::size_t>(light.shadow[0]));
                        if (positional.params[1] > 0.001F) {
                            shadow = positional_fog_shadow(positional, maps.positional, world);
                        }
                    }
                    const float phase = henyey_greenstein(dot3(normalize(to_light), view_dir), g);
                    total_light = add(total_light, scale(xyz(light.color),
                                                         attenuation * shadow * phase * energy));
                }
            }
        }
    }

    const Float3 lit = add(mul(total_light, scattering), emission);
    return lerp4({lit[0], lit[1], lit[2], total_density}, reprojected, reproject_amount);
}

Float4 fog_filter(const FogUniforms& u, std::span<const Float4> source, std::uint32_t x,
                  std::uint32_t y, std::uint32_t z) {
    const std::array<std::uint32_t, 3> at{x, y, z};
    const std::size_t axis = u.size[3] == static_cast<std::uint32_t>(FogPass::FilterX) ? 0 : 1;
    Float4 sum{};
    for (std::size_t k = 0; k < kGauss.size(); ++k) {
        std::array<std::uint32_t, 3> tap = at;
        const auto moved =
            static_cast<std::int64_t>(at.at(axis)) + static_cast<std::int64_t>(k) - 3;
        tap.at(axis) = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(moved, 0, static_cast<std::int64_t>(u.size.at(axis)) - 1));
        const Float4& value = source[froxel(u.size, tap[0], tap[1], tap[2])];
        const float weight = kGauss.at(k);
        for (std::size_t c = 0; c < 4; ++c) {
            sum.at(c) += value.at(c) * weight;
        }
    }
    return sum;
}

void fog_integrate(const FogUniforms& u, std::span<const Float4> source, std::span<Float4> target,
                   std::uint32_t x, std::uint32_t y) {
    const float cell = 1.0F / static_cast<float>(u.size[2]);
    Float4 accum{0.0F, 0.0F, 0.0F, 1.0F};
    float previous_z = 0.0F;
    for (std::uint32_t i = 0; i < u.size[2]; ++i) {
        const std::size_t at = froxel(u.size, x, y, i);
        const Float4 fog = source[at];
        // get_depth_at_pos: the froxel's center's depth.
        const float z =
            u.params[0] * power((static_cast<float>(i) * cell) + (cell * 0.5F), u.params[1]);
        const float d = std::abs(previous_z - z);
        // Beer's law.
        const float transmittance = slash::exp(-d * fog[3]);
        const float share = accum[3] / std::max(fog[3], 0.00001F);
        for (std::size_t c = 0; c < 3; ++c) {
            accum.at(c) += (fog.at(c) - (fog.at(c) * transmittance)) * share;
        }
        accum[3] *= transmittance;
        previous_z = z;
        target[at] = accum;
    }
}

FogVolume run_volumetric_fog(FogUniforms uniforms, const FogMaps& maps,
                             std::span<const Float4> previous, bool filter) {
    const std::array<std::uint32_t, 4> size = uniforms.size;
    const std::size_t count = std::size_t{size[0]} * size[1] * size[2];
    FogVolume out;
    out.density.resize(count);
    for (std::uint32_t z = 0; z < size[2]; ++z) {
        for (std::uint32_t y = 0; y < size[1]; ++y) {
            for (std::uint32_t x = 0; x < size[0]; ++x) {
                out.density[froxel(size, x, y, z)] = fog_density(uniforms, maps, previous, x, y, z);
            }
        }
    }
    std::vector<Float4> filtered;
    if (filter) {
        std::vector<Float4> across(count);
        filtered.resize(count);
        for (const FogPass pass : {FogPass::FilterX, FogPass::FilterY}) {
            uniforms.size[3] = static_cast<std::uint32_t>(pass);
            const std::vector<Float4>& source = pass == FogPass::FilterX ? out.density : across;
            std::vector<Float4>& target = pass == FogPass::FilterX ? across : filtered;
            for (std::uint32_t z = 0; z < size[2]; ++z) {
                for (std::uint32_t y = 0; y < size[1]; ++y) {
                    for (std::uint32_t x = 0; x < size[0]; ++x) {
                        target[froxel(size, x, y, z)] = fog_filter(uniforms, source, x, y, z);
                    }
                }
            }
        }
    }
    uniforms.size[3] = static_cast<std::uint32_t>(FogPass::Integrate);
    out.fog.resize(count);
    for (std::uint32_t y = 0; y < size[1]; ++y) {
        for (std::uint32_t x = 0; x < size[0]; ++x) {
            fog_integrate(uniforms, filter ? filtered : out.density, out.fog, x, y);
        }
    }
    return out;
}

Float4 sample_fog_volume(std::span<const Float4> fog, const std::array<std::uint32_t, 3>& size,
                         const Float4& volumetric_fog, std::array<float, 2> uv, float view_depth) {
    if (fog.empty()) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    float z = view_depth * volumetric_fog[1];
    if (z < 0.0F) {
        return {};
    }
    if (z < 1.0F) {
        z = power(z, volumetric_fog[2]);
    }
    return sample_volume(fog, size, {uv[0], uv[1], z});
}

Float3 fog_sky(std::span<const Float4> fog, const std::array<std::uint32_t, 3>& size,
               const Float4& volumetric_fog, std::array<float, 2> uv, const Float3& sky) {
    if (fog.empty()) {
        return sky;
    }
    const Float4 seen = sample_volume(fog, size, {uv[0], uv[1], 1.0F});
    const Float3 fogged = add(scale(sky, seen[3]), xyz(seen));
    return lerp3(sky, fogged, volumetric_fog[3]);
}

FogSkyUniforms fog_sky_uniforms(const MeshSceneUniforms& lights) {
    return {.volumetric_fog = lights.volumetric_fog,
            .volume = lights.fog_volume,
            .screen = {lights.screen[0], lights.screen[1], 0.0F, 0.0F}};
}

} // namespace slash::render::detail
