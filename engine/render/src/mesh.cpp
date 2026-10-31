// Projections, the mesh shader's uniforms and its CPU twin (shade_mesh), which follows
// shaders/mesh.hlsl line for line in float math. pow is exp(y * ln(x)) through core/math, so the
// software device's meshes are the same pixels on every machine.

#include "checks.hpp"
#include "decal.hpp"
#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "reflection.hpp"
#include "sky_shading.hpp"
#include "volumetric_fog.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <string>

namespace slash::render {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

using detail::Float3;
using detail::Float4;

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
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

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// fog_process in shaders/mesh.hlsl: the fog's color at a point and how much of it covers it.
Float4 fog_process(const detail::MeshSceneUniforms& scene, const Float3& world) {
    const Float3 to_point = sub(world, {scene.camera[0], scene.camera[1], scene.camera[2]});
    const float distance = length(to_point);
    Float3 color{scene.fog_color[0], scene.fog_color[1], scene.fog_color[2]};
    const Float4& fog = scene.fog;
    if (fog[1] > 0.001F) {
        const Float3 view = distance > 0.0F ? scale(to_point, 1.0F / distance) : Float3{};
        const auto count = static_cast<std::size_t>(scene.camera[3]);
        for (std::size_t i = 0; i < count; ++i) {
            const detail::MeshLightUniforms& light = scene.lights.at(i);
            if (light.direction[3] == 0.0F) {
                const float amount =
                    power(std::max(dot3(view, normalize({light.direction[0], light.direction[1],
                                                         light.direction[2]})),
                                   0.0F),
                          8.0F);
                color = add(color, scale({light.color[0], light.color[1], light.color[2]},
                                         amount * fog[1]));
            }
        }
    }
    float amount = 0.0F;
    if (scene.fog_color[3] == 2.0F) {
        const float z = smoothstep(scene.fog_depth[0], scene.fog_depth[1], distance);
        amount = power(z, scene.fog_depth[2]) * fog[0];
    } else {
        amount = 1.0F - slash::exp(std::min(0.0F, -distance * fog[0]));
    }
    if (std::abs(fog[3]) >= 0.0001F) {
        amount = std::max(amount, 1.0F - slash::exp(std::min(0.0F, (world[1] - fog[2]) * fog[3])));
    }
    return {color[0], color[1], color[2], amount};
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

// The upper 3x3 of the matrix times a direction, as HLSL's (float3x3) cast.
Float3 transform3(const Mat4& m, const Float3& v) {
    const Float4 out = transform(m, {v[0], v[1], v[2], 0.0F});
    return {out[0], out[1], out[2]};
}

// shaders/mesh.hlsl's helpers.
float schlick_fresnel(float u) {
    const float m = 1.0F - u;
    const float m2 = m * m;
    return m2 * m2 * m;
}

float d_ggx(float cos_theta_m, float alpha) {
    const float a = cos_theta_m * alpha;
    const float k = alpha / (1.0F - (cos_theta_m * cos_theta_m) + (a * a));
    return k * k * std::numbers::inv_pi_v<float>;
}

float v_ggx(float n_dot_l, float n_dot_v, float alpha) {
    return 0.5F / lerp(2.0F * n_dot_l * n_dot_v, n_dot_l + n_dot_v, alpha);
}

// The light a pixel gets, summed over the lights.
struct LightSum {
    Float3 diffuse{};
    Float3 specular{};
};

void light_compute(const Float3& n, const Float3& l, const Float3& v, const Float3& light_color,
                   float attenuation, const Float3& f0, float roughness, float metallic,
                   float specular_amount, LightSum& out) {
    const float n_dot_l = std::min(dot3(n, l), 1.0F);
    const float c_n_dot_l = std::max(n_dot_l, 0.0F);
    const float c_n_dot_v = std::max(dot3(n, v), 1e-4F);
    const Float3 h = normalize(add(v, l));
    const float c_n_dot_h = std::clamp(dot3(n, h), 0.0F, 1.0F);
    const float c_l_dot_h = std::clamp(dot3(l, h), 0.0F, 1.0F);
    if (metallic < 1.0F) {
        const float fd90_minus_1 = (2.0F * c_l_dot_h * c_l_dot_h * roughness) - 0.5F;
        const float fd_v = 1.0F + (fd90_minus_1 * schlick_fresnel(c_n_dot_v));
        const float fd_l = 1.0F + (fd90_minus_1 * schlick_fresnel(c_n_dot_l));
        const float diffuse_brdf_nl = std::numbers::inv_pi_v<float> * fd_v * fd_l * c_n_dot_l;
        out.diffuse = add(out.diffuse, scale(light_color, diffuse_brdf_nl * attenuation));
    }
    if (roughness > 0.0F) {
        const float alpha_ggx = roughness * roughness;
        const float d = d_ggx(c_n_dot_h, alpha_ggx);
        const float g = v_ggx(c_n_dot_l, c_n_dot_v, alpha_ggx);
        const float c_l_dot_h5 = schlick_fresnel(c_l_dot_h);
        const float f90 = std::clamp(50.0F * f0[1], 0.0F, 1.0F);
        Float3 f{};
        for (std::size_t i = 0; i < 3; ++i) {
            f.at(i) = f0.at(i) + ((f90 - f0.at(i)) * c_l_dot_h5);
        }
        const Float3 specular_brdf_nl = scale(f, c_n_dot_l * d * g);
        out.specular = add(
            out.specular, scale(mul(specular_brdf_nl, light_color), attenuation * specular_amount));
    }
}

float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = std::max(1.0F - nd, 0.0F);
    nd *= nd;
    return nd * power(std::max(distance, 0.0001F), -decay);
}

bool finite(std::initializer_list<float> values) {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

bool finite(const std::array<float, 16>& matrix) {
    return std::ranges::all_of(matrix, [](float value) { return std::isfinite(value); });
}

Float4 channel_vector(TextureChannel channel) {
    switch (channel) {
    case TextureChannel::Red:
        return {1.0F, 0.0F, 0.0F, 0.0F};
    case TextureChannel::Green:
        return {0.0F, 1.0F, 0.0F, 0.0F};
    case TextureChannel::Blue:
        return {0.0F, 0.0F, 1.0F, 0.0F};
    case TextureChannel::Alpha:
        return {0.0F, 0.0F, 0.0F, 1.0F};
    case TextureChannel::Gray:
        break;
    }
    return {0.3333333F, 0.3333333F, 0.3333333F, 0.0F};
}

// smoothstep, written out, since its result is undefined for edges in falling order (the fade's)
// in some shading languages.
float smooth(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// sample_shadow in shaders/mesh.hlsl, Godot's: the shadow at a point through a matrix into an
// atlas, from 1, 5 or 13 samples a texel apart; each sample's coordinate kept within clamp's
// low and high corners when it is given.
float sample_shadow(const detail::ShadowAtlas& atlas, const Mat4& matrix, const Float3& point,
                    float texel, float taps, const Float4* clamp) {
    const Float4 at = transform(matrix, {point[0], point[1], point[2], 1.0F});
    const float u = at[0] / at[3];
    const float v = at[1] / at[3];
    const float reference = std::clamp(at[2] / at[3], 0.0F, 1.0F);
    const auto tap = [&](float du, float dv) {
        float su = u + (du * texel);
        float sv = v + (dv * texel);
        if (clamp != nullptr) {
            su = std::clamp(su, (*clamp)[0], (*clamp)[2]);
            sv = std::clamp(sv, (*clamp)[1], (*clamp)[3]);
        }
        return atlas.compare(su, sv, reference);
    };
    float sum = tap(0.0F, 0.0F);
    if (taps > 12.0F) {
        sum += tap(2.0F, 0.0F) + tap(-2.0F, 0.0F) + tap(0.0F, 2.0F) + tap(0.0F, -2.0F);
        // Godot stops early where the far samples agree.
        if (sum <= 0.000001F) {
            return 0.0F;
        }
        if (sum >= 4.999999F) {
            return 1.0F;
        }
        sum += tap(1.0F, 0.0F) + tap(-1.0F, 0.0F) + tap(0.0F, 1.0F) + tap(0.0F, -1.0F);
        sum += tap(1.0F, 1.0F) + tap(-1.0F, 1.0F) + tap(1.0F, -1.0F) + tap(-1.0F, -1.0F);
        return sum * (1.0F / 13.0F);
    }
    if (taps > 4.0F) {
        sum += tap(1.0F, 0.0F) + tap(-1.0F, 0.0F) + tap(0.0F, 1.0F) + tap(0.0F, -1.0F);
        return sum * (1.0F / 5.0F);
    }
    return sum;
}

// directional_shadow in shaders/mesh.hlsl: a directional light's shadow at a pixel, its cascade
// picked by view depth, as Godot's scene.glsl picks it.
float directional_shadow(const detail::MeshMaps& maps, std::size_t slot, const Float3& world,
                         const Float3& normal, const Float3& toward_light) {
    const detail::MeshShadowUniforms& u = *maps.uniforms;
    const detail::MeshDirectionalShadowUniforms& s = u.directional.at(slot);
    const float view_z = dot3(world, {u.camera_z[0], u.camera_z[1], u.camera_z[2]}) + u.camera_z[3];
    const Float3 base_bias = scale(normal, 1.0F - std::max(0.0F, -dot3(toward_light, normal)));
    const auto cascade = [&](std::size_t i) {
        return sample_shadow(maps.directional, s.matrices.at(i),
                             add(world, scale(base_bias, s.normal_bias.at(i))), s.options[1],
                             u.filters[0], nullptr);
    };
    const auto count = static_cast<std::size_t>(s.params[3]);
    const float depth_z = -view_z;
    float shadow = 1.0F;
    if (count <= 1) {
        shadow = cascade(0);
    } else if (depth_z < s.splits.at(count - 1)) {
        std::size_t i = 0;
        while (i + 1 < count && !(depth_z < s.splits.at(i))) {
            ++i;
        }
        shadow = cascade(i);
        if (s.options[0] != 0.0F && i + 1 < count) {
            const float from = i == 0 ? 0.0F : s.splits.at(i - 1);
            shadow = lerp(shadow, cascade(i + 1), smooth(from, s.splits.at(i), depth_z));
        }
    }
    shadow = lerp(shadow, 1.0F, smooth(s.params[0], s.params[1], view_z));
    return lerp(1.0F, shadow, s.params[2]);
}

// positional_shadow in shaders/mesh.hlsl: an omni or spot light's shadow at a pixel, through
// the cube face its ray from the light leaves by for an omni light.
float positional_shadow(const detail::MeshMaps& maps, std::size_t slot, const Float3& world,
                        const Float3& normal) {
    const detail::MeshShadowUniforms& u = *maps.uniforms;
    const detail::MeshPositionalShadowUniforms& s = u.positional.at(slot);
    const Float3 to_light = sub({s.position[0], s.position[1], s.position[2]}, world);
    const float light_length = length(to_light);
    const float a_n_dot_l = std::abs(dot3(normal, scale(to_light, 1.0F / light_length)));
    const Float3 point = add(world, scale(normal, (1.0F - a_n_dot_l) * s.params[0] * light_length));
    std::size_t face = 0;
    float taps = u.filters[1];
    if (s.params[3] > 1.5F) {
        const Float4 local = transform(s.light, {point[0], point[1], point[2], 1.0F});
        const float ax = std::abs(local[0]);
        const float ay = std::abs(local[1]);
        const float az = std::abs(local[2]);
        if (ax >= ay && ax >= az) {
            face = local[0] > 0.0F ? 0 : 1;
        } else if (ay >= az) {
            face = local[1] < 0.0F ? 2 : 3;
        } else {
            face = local[2] > 0.0F ? 4 : 5;
        }
        taps = 1.0F; // Godot samples an omni light's cube once
    }
    const float shadow = sample_shadow(maps.positional, s.faces.at(face), point, s.params[2], taps,
                                       &s.rects.at(face));
    return lerp(1.0F, shadow, s.params[1]);
}

// What terrain shading gives a pixel, as the HTerrain plugin's Classic4 shader: the ground's
// albedo (linear), roughness, and normal and the terrain's own normal, in the mesh's space.
struct TerrainTexel {
    Float3 albedo{};
    float roughness = 1.0F;
    Float3 normal{0.0F, 1.0F, 0.0F};
    Float3 shape{0.0F, 1.0F, 0.0F};
};

// get_depth_blended_weights in HTerrain's shader: the splat map's weights, sharpened where one
// layer's bump stands above the others.
Float4 depth_blended_weights(const Float4& splat, const Float4& bumps) {
    constexpr float kDh = 0.2F;
    Float4 h{};
    for (std::size_t i = 0; i < 4; ++i) {
        h.at(i) = (bumps.at(i) + splat.at(i)) * smoothstep(0.0F, 0.05F, splat.at(i));
    }
    Float4 d{};
    for (std::size_t i = 0; i < 4; ++i) {
        float highest = 0.0F;
        bool first = true;
        for (std::size_t j = 0; j < 4; ++j) {
            if (j != i) {
                highest = first ? h.at(j) : std::max(highest, h.at(j));
                first = false;
            }
        }
        d.at(i) = std::clamp(h.at(i) + kDh - highest, 0.0F, 1.0F);
    }
    return d;
}

TerrainTexel terrain_texel(const detail::MeshSurfaceUniforms& surface,
                           const std::array<Float4, detail::kMeshTextures>& texels) {
    const std::uint32_t flags = surface.options[3];
    const Float3 tint = xyz(texels[0]);
    const Float4& splat = texels[1];
    std::array<Float3, 4> colors{};
    std::array<Float3, 4> normals{};
    Float4 bumps{};
    Float4 roughness{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Float4& albedo_bump = texels.at(3 + i);
        // A layer without a normal_roughness texture is flat, with roughness 1.
        const Float4 normal_roughness = (flags & (detail::kMeshTerrainNormals << i)) != 0
                                            ? texels.at(7 + i)
                                            : Float4{0.5F, 0.5F, 1.0F, 1.0F};
        colors.at(i) = mul(detail::srgb_to_linear(xyz(albedo_bump)),
                           lerp3({1.0F, 1.0F, 1.0F}, tint, surface.terrain_opacity.at(i)));
        bumps.at(i) = albedo_bump[3];
        roughness.at(i) = normal_roughness[3];
        // unpack_normal: OpenGL style, its green along the terrain's -Z and blue its +Y.
        normals.at(i) = {(normal_roughness[0] * 2.0F) - 1.0F, (normal_roughness[2] * 2.0F) - 1.0F,
                         -((normal_roughness[1] * 2.0F) - 1.0F)};
    }
    const Float4 weights = (flags & detail::kMeshTerrainDepthBlending) != 0
                               ? depth_blended_weights(splat, bumps)
                               : splat;
    // Splat weights that are all 0 leave the ground black rather than undefined.
    const float sum = std::max(weights[0] + weights[1] + weights[2] + weights[3], 1.0e-6F);
    TerrainTexel out;
    out.albedo = {};
    out.roughness = 0.0F;
    Float3 ground{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float share = weights.at(i) / sum;
        out.albedo = add(out.albedo, scale(colors.at(i), share));
        out.roughness += roughness.at(i) * share;
        ground = add(ground, scale(normals.at(i), share));
    }
    const Float4& mapped = texels[2];
    out.shape = normalize(
        {(mapped[0] * 2.0F) - 1.0F, (mapped[1] * 2.0F) - 1.0F, (mapped[2] * 2.0F) - 1.0F});
    out.normal = normalize({out.shape[0] + ground[0], out.shape[1], out.shape[2] + ground[2]});
    return out;
}

// A direction in the mesh's space, in the world through the pixel's tangent frame: a terrain's
// vertices have the normal +Y and the tangent +X, so its binormal is -Z.
Float3 frame_direction(const detail::MeshPixel& pixel, const Float3& v) {
    return normalize(add(add(scale(pixel.tangent, v[0]), scale(pixel.normal, v[1])),
                         scale(pixel.binormal, -v[2])));
}

} // namespace

std::array<float, 16> perspective(float fov_y, float aspect, float near, float far) {
    const Vec2 turn = slash::cos_sin(fov_y * (kPi / 180.0F) * 0.5F);
    const float cotangent = turn.x / turn.y;
    std::array<float, 16> m{};
    m[0] = cotangent / aspect;
    m[5] = cotangent;
    m[10] = near / (far - near);
    m[11] = -1.0F;
    m[14] = (near * far) / (far - near);
    return m;
}

std::array<float, 16> orthogonal(float size, float aspect, float near, float far) {
    std::array<float, 16> m{};
    m[0] = 2.0F / (size * aspect);
    m[5] = 2.0F / size;
    m[10] = 1.0F / (far - near);
    m[14] = far / (far - near);
    m[15] = 1.0F;
    return m;
}

std::array<float, 16> look_at(const std::array<float, 3>& eye, const std::array<float, 3>& target,
                              const std::array<float, 3>& up) {
    // Basis::looking_at: the camera's z points away from the target.
    const Float3 z = normalize(sub(eye, target));
    const Float3 x = normalize(cross(up, z));
    const Float3 y = cross(z, x);
    const std::array<Float3, 3> axes{x, y, z};
    std::array<float, 16> m{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            m.at((column * 4) + row) = axes.at(row).at(column);
        }
        m.at(12 + row) = -dot3(axes.at(row), eye);
    }
    m[15] = 1.0F;
    return m;
}

std::optional<RenderMethod> parse_render_method(std::string_view name) {
    const auto index = static_cast<std::size_t>(std::ranges::find(kRenderMethodNames, name) -
                                                kRenderMethodNames.begin());
    if (index == kRenderMethodNames.size()) {
        return std::nullopt;
    }
    return static_cast<RenderMethod>(index);
}

float mesh_lod_threshold(const json::Value& rendering) {
    const json::Value* found = rendering.is_object() ? rendering.find("mesh_lod") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("lod_change") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("threshold_pixels") : nullptr;
    if (found != nullptr && found->is_number() && found->as_double() >= 0.0) {
        return static_cast<float>(found->as_double());
    }
    return 1.0F;
}

RayTracing3D ray_tracing(const json::Value& rendering) {
    RayTracing3D out;
    const json::Value* found = rendering.is_object() ? rendering.find("ray_tracing") : nullptr;
    if (found == nullptr || !found->is_object()) {
        return out;
    }
    if (const json::Value* shadows = found->find("shadows");
        shadows != nullptr && shadows->is_bool()) {
        out.shadows = shadows->as_bool();
    }
    if (const json::Value* samples = found->find("shadow_samples");
        samples != nullptr && samples->is_number()) {
        out.shadow_samples = static_cast<std::uint32_t>(
            std::clamp(samples->as_double(), 1.0, static_cast<double>(kMaxRayShadowSamples)));
    }
    if (const json::Value* occlusion = found->find("ambient_occlusion");
        occlusion != nullptr && occlusion->is_bool()) {
        out.ambient_occlusion = occlusion->as_bool();
    }
    if (const json::Value* samples = found->find("ambient_occlusion_samples");
        samples != nullptr && samples->is_number()) {
        out.ambient_occlusion_samples = static_cast<std::uint32_t>(
            std::clamp(samples->as_double(), 1.0, static_cast<double>(kMaxRayShadowSamples)));
    }
    if (const json::Value* reflections = found->find("reflections");
        reflections != nullptr && reflections->is_bool()) {
        out.reflections = reflections->as_bool();
    }
    if (const json::Value* samples = found->find("reflection_samples");
        samples != nullptr && samples->is_number()) {
        out.reflection_samples = static_cast<std::uint32_t>(
            std::clamp(samples->as_double(), 1.0, static_cast<double>(kMaxRayShadowSamples)));
    }
    if (const json::Value* gi = found->find("global_illumination");
        gi != nullptr && gi->is_bool()) {
        out.global_illumination = gi->as_bool();
    }
    if (const json::Value* samples = found->find("global_illumination_samples");
        samples != nullptr && samples->is_number()) {
        out.global_illumination_samples = static_cast<std::uint32_t>(
            std::clamp(samples->as_double(), 1.0, static_cast<double>(kMaxRayShadowSamples)));
    }
    return out;
}

OcclusionCulling occlusion_culling(const json::Value& rendering) {
    OcclusionCulling out;
    const json::Value* found =
        rendering.is_object() ? rendering.find("occlusion_culling") : nullptr;
    if (found == nullptr || !found->is_object()) {
        return out;
    }
    if (const json::Value* use = found->find("use_occlusion_culling");
        use != nullptr && use->is_bool()) {
        out.enabled = use->as_bool();
    }
    if (const json::Value* rays = found->find("occlusion_rays_per_thread");
        rays != nullptr && rays->is_number() && rays->as_double() >= 1.0 &&
        rays->as_double() <= 65536.0) {
        out.rays_per_thread = static_cast<int>(rays->as_double());
    }
    if (const json::Value* jitter = found->find("jitter_projection");
        jitter != nullptr && jitter->is_bool()) {
        out.jitter = jitter->as_bool();
    }
    return out;
}

std::array<float, 16> Scene3D::perspective_default() {
    return perspective(75.0F, 1.0F, 0.05F, 4000.0F);
}

} // namespace slash::render

namespace slash::render::detail {

float srgb_to_linear_exact(float value) {
    return value < 0.04045F ? value * (1.0F / 12.92F)
                            : power((value + 0.055F) * (1.0F / 1.055F), 2.4F);
}

MeshCameraUniforms mesh_camera_uniforms(const Scene3D& scene) {
    const Mat4 view_projection = multiply(scene.projection, scene.camera);
    return {.view_projection = view_projection,
            .previous_view_projection = view_projection,
            .jitter = {}};
}

MeshInstanceUniforms mesh_instance_uniforms(const Mat4& transform, const Material3D& material) {
    return mesh_instance_uniforms(transform, material, transform);
}

MeshInstanceUniforms mesh_instance_uniforms(const Mat4& transform, const Material3D& material,
                                            const Mat4& previous) {
    MeshInstanceUniforms out;
    out.model = transform;
    out.previous_model = previous;
    // The cofactors of the upper 3x3 are its inverse transpose times its determinant, whose sign
    // keeps a mirrored mesh's normals pointing out; the shader normalizes them.
    const auto a = [&](std::size_t row, std::size_t column) {
        return transform.at((column * 4) + row);
    };
    std::array<float, 9> cofactor{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const std::size_t r0 = (row + 1) % 3;
            const std::size_t r1 = (row + 2) % 3;
            const std::size_t c0 = (column + 1) % 3;
            const std::size_t c1 = (column + 2) % 3;
            cofactor.at((row * 3) + column) = (a(r0, c0) * a(r1, c1)) - (a(r0, c1) * a(r1, c0));
        }
    }
    const float determinant =
        (a(0, 0) * cofactor[0]) + (a(0, 1) * cofactor[1]) + (a(0, 2) * cofactor[2]);
    const float sign = determinant < 0.0F ? -1.0F : 1.0F;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            out.normal_model.at((column * 4) + row) = cofactor.at((row * 3) + column) * sign;
        }
    }
    out.normal_model[15] = 1.0F;
    out.uv1 = {material.uv1_scale[0], material.uv1_scale[1], material.uv1_offset[0],
               material.uv1_offset[1]};
    out.lightmap_uv = material.lightmap.uv_rect;
    return out;
}

MeshVoxelGIUniforms voxel_gi_uniforms(const VoxelGIProbe& probe) {
    MeshVoxelGIUniforms out;
    const std::array<float, 12>& m = probe.to_cell;
    out.rows = {Float4{m[0], m[1], m[2], m[3]}, Float4{m[4], m[5], m[6], m[7]},
                Float4{m[8], m[9], m[10], m[11]}};
    const std::uint32_t levels =
        std::clamp<std::uint32_t>(probe.levels, 1, static_cast<std::uint32_t>(kMaxVoxelGILevels));
    out.size = {static_cast<float>(probe.size[0]), static_cast<float>(probe.size[1]),
                static_cast<float>(probe.size[2]), static_cast<float>(levels)};
    out.params = {probe.energy, probe.bias, probe.normal_bias, probe.blend_ambient ? 1.0F : 0.0F};
    for (std::size_t i = 0; i < levels; ++i) {
        const std::array<std::uint32_t, 3>& place = probe.placement.at(i);
        out.levels.at(i) = {static_cast<float>(place[0]), static_cast<float>(place[1]),
                            static_cast<float>(std::max<std::uint32_t>(place[2], 1)), 0.0F};
    }
    return out;
}

std::array<MaterialTexture, kMeshTextures> mesh_texture_slots(const Material3D& material) {
    std::array<MaterialTexture, kMeshTextures> slots{};
    if (!material.terrain.enabled) {
        slots[0] = material.albedo_texture;
        slots[1] = material.metallic_texture;
        slots[2] = material.roughness_texture;
        slots[3] = material.emission_texture;
        slots[4] = material.normal_texture;
        slots[5] = {.texture = material.voxel_gi.atlas, .repeat = false};
        slots[10] = {.texture = material.lightmap.texture, .repeat = false};
        return slots;
    }
    const TerrainShading& terrain = material.terrain;
    slots[0] = terrain.colormap;
    slots[1] = terrain.splatmap;
    slots[2] = terrain.normalmap;
    for (std::size_t i = 0; i < 4; ++i) {
        slots.at(3 + i) = terrain.layers.at(i).albedo_bump;
        slots.at(7 + i) = terrain.layers.at(i).normal_roughness;
    }
    return slots;
}

std::array<float, 2> mesh_texture_uv(const MeshSurfaceUniforms& surface, std::size_t slot,
                                     std::array<float, 2> uv) {
    if (slot < 3 || (surface.options[3] & kMeshTerrain) == 0) {
        return uv;
    }
    const float tiles = surface.terrain_uv.at((slot - 3) % 4);
    return {uv[0] * tiles, uv[1] * tiles};
}

MeshSurfaceUniforms mesh_surface_uniforms(const Material3D& material, std::uint32_t layers) {
    MeshSurfaceUniforms out;
    out.albedo = material.albedo;
    out.emission = {material.emission[0], material.emission[1], material.emission[2],
                    material.emission_energy};
    out.surface = {material.metallic, material.roughness, material.specular, material.normal_scale};
    out.metallic_channel = channel_vector(material.metallic_channel);
    out.roughness_channel = channel_vector(material.roughness_channel);
    std::uint32_t flags = 0;
    if (material.unshaded) {
        flags |= kMeshUnshaded;
    }
    if (material.vertex_color_use_as_albedo) {
        flags |= kMeshVertexColorAlbedo;
    }
    if (material.transparency == Transparency::AlphaScissor) {
        flags |= kMeshAlphaScissor;
    }
    if (material.emission_texture.texture != TextureId::None) {
        flags |= kMeshEmissionTexture;
    }
    if (material.normal_texture.texture != TextureId::None) {
        flags |= kMeshNormalMap;
    }
    if (material.cull == CullMode::Disabled) {
        flags |= kMeshSideCheck;
    }
    if (material.transparency != Transparency::Disabled) {
        flags |= kMeshHasAlpha;
    }
    if (material.disable_fog) {
        flags |= kMeshDisableFog;
    }
    if (material.lightmap.texture != TextureId::None && !material.terrain.enabled) {
        flags |= kMeshLightmap;
        out.lightmap = {material.lightmap.exposure, material.lightmap.bicubic ? 1.0F : 0.0F, 0.0F,
                        0.0F};
    } else if (material.capture.enabled && !material.terrain.enabled) {
        flags |= kMeshLightmapCapture;
        for (std::size_t i = 0; i < 9; ++i) {
            const std::array<float, 3>& c = material.capture.sh.at(i);
            out.capture.at(i) = {c[0], c[1], c[2], 0.0F};
        }
    } else if (const VoxelGIShading& gi = material.voxel_gi;
               gi.atlas != TextureId::None && !material.terrain.enabled) {
        const std::uint32_t count = std::min<std::uint32_t>(gi.count, 2);
        out.voxel_gi = {static_cast<float>(count), 0.0F, 0.0F, 0.0F};
        for (std::size_t i = 0; i < count; ++i) {
            out.voxel_gis.at(i) = voxel_gi_uniforms(gi.probes.at(i));
        }
    }
    float threshold = material.alpha_scissor_threshold;
    std::uint32_t terrain = 0;
    if (material.terrain.enabled) {
        // The color map's alpha cuts holes, in the mesh and its shadows, as HTerrain's shader
        // discards pixels under 0.5. The emission and normal textures' slots hold ground layers.
        flags |= kMeshAlphaScissor | kMeshHasAlpha;
        flags &= ~(kMeshEmissionTexture | kMeshNormalMap);
        threshold = 0.5F;
        terrain = kMeshTerrain;
        if (material.terrain.depth_blending) {
            terrain |= kMeshTerrainDepthBlending;
        }
        const auto resolution = static_cast<float>(material.terrain.resolution);
        for (std::size_t i = 0; i < 4; ++i) {
            const TerrainLayer& layer = material.terrain.layers.at(i);
            if (layer.normal_roughness.texture != TextureId::None) {
                terrain |= kMeshTerrainNormals << i;
            }
            out.terrain_uv.at(i) = resolution / layer.uv_scale;
            out.terrain_opacity.at(i) = layer.colormap_opacity;
        }
    }
    out.options = {flags, same_bits<std::uint32_t>(threshold), layers, terrain};
    return out;
}

Float3 camera_place(const Mat4& c) {
    // -R^T t for its rotation R and translation t, scaled back by the rows' lengths for a camera
    // matrix that scales.
    Float3 place{};
    for (std::size_t row = 0; row < 3; ++row) {
        const Float3 axis{c.at(row), c.at(4 + row), c.at(8 + row)};
        const float squared = dot3(axis, axis);
        place = add(place, scale(axis, -c.at(12 + row) / squared));
    }
    return place;
}

MeshSceneUniforms mesh_scene_uniforms(const Scene3D& scene) {
    MeshSceneUniforms out;
    const Mat4& c = scene.camera;
    const Float3 place = camera_place(c);
    out.camera = {place[0], place[1], place[2], 0.0F};
    const Mat4 clip = multiply(scene.projection, c);
    out.clip_rows = {Float4{clip[0], clip[4], clip[8], clip[12]},
                     Float4{clip[1], clip[5], clip[9], clip[13]},
                     Float4{clip[3], clip[7], clip[11], clip[15]}};
    out.view_rows = {Float4{c[0], c[4], c[8], c[12]}, Float4{c[1], c[5], c[9], c[13]},
                     Float4{c[2], c[6], c[10], c[14]}};
    if (const Ssao3D& ssao = scene.ssao;
        ssao.enabled && scene.method == RenderMethod::ForwardPlus) {
        out.ssao = {1.0F, ssao.light_affect, ssao.ao_channel_affect, 0.0F};
    }
    if (scene.ssr.enabled && scene.method == RenderMethod::ForwardPlus) {
        out.ssao[3] = 1.0F;
    }
    if (scene.method == RenderMethod::ForwardPlus && !scene.capture) {
        out.reflections = reflections_uniform(scene.reflections);
        out.voxel_gi = {1.0F, scene.voxel_gi_high_quality ? 1.0F : 0.0F, 0.0F, 0.0F};
    }
    const float energy = scene.ambient_energy;
    out.ambient = {srgb_to_linear_exact(scene.ambient[0]) * energy,
                   srgb_to_linear_exact(scene.ambient[1]) * energy,
                   srgb_to_linear_exact(scene.ambient[2]) * energy, 0.0F};
    out.tonemap = {scene.post.exposure, scene.post.white, static_cast<float>(scene.post.tonemapper),
                   color_stage(scene)};
    if (const Fog3D& fog = scene.fog; fog.enabled) {
        out.fog_color = {srgb_to_linear_exact(fog.light_color[0]) * fog.light_energy,
                         srgb_to_linear_exact(fog.light_color[1]) * fog.light_energy,
                         srgb_to_linear_exact(fog.light_color[2]) * fog.light_energy,
                         fog.depth ? 2.0F : 1.0F};
        out.fog = {fog.density, fog.sun_scatter, fog.height, fog.height_density};
        // As Godot's: depth fog ends at the far plane when its end is 0 or less, and begins
        // before it ends.
        const float far = far_plane(scene.projection);
        const float end = fog.depth_end > 0.0F ? fog.depth_end : far;
        out.fog_depth = {std::min(fog.depth_begin, end - 0.001F), end, fog.depth_curve, 0.0F};
    }
    if (radiance_used(scene)) {
        const Radiance3D& radiance = scene.radiance;
        out.sky = {radiance.ambient ? 1.0F : 0.0F, radiance.reflections ? 1.0F : 0.0F,
                   scene.sky.energy, radiance.ambient_mix};
        const Mat4& o = scene.sky.orientation;
        out.sky_rows = {Float4{o[0], o[4], o[8], 0.0F}, Float4{o[1], o[5], o[9], 0.0F},
                        Float4{o[2], o[6], o[10], 0.0F}};
    }
    // Every pixel takes these lights; Forward+'s omni and spot lights are in its clusters.
    const std::vector<int> slots = shadow_slots(scene);
    const std::vector<int> rays = ray_shadow_slots(scene);
    std::size_t count = 0;
    for (std::size_t i = 0; i < lights_drawn(scene); ++i) {
        const Light3D& light = scene.lights[i];
        if (scene.method == RenderMethod::ForwardPlus && light.type != LightType::Directional) {
            continue;
        }
        out.lights.at(count++) = mesh_light_uniforms(light, slots.at(i), rays.at(i));
    }
    out.camera[3] = static_cast<float>(count);
    return out;
}

MeshLightUniforms mesh_light_uniforms(const Light3D& light, int shadow_slot, int ray_slot) {
    MeshLightUniforms packed;
    packed.position = {light.position[0], light.position[1], light.position[2],
                       1.0F / std::max(0.001F, light.range)};
    Float3 direction = normalize(light.direction);
    float type = 0.0F;
    if (light.type == LightType::Directional) {
        direction = scale(direction, -1.0F); // toward the light, as Godot's node's +Z
    } else {
        type = light.type == LightType::Omni ? 1.0F : 2.0F;
    }
    packed.direction = {direction[0], direction[1], direction[2], type};
    const float scaled = light.energy * kPi;
    const float specular =
        light.type == LightType::Directional ? light.specular : light.specular * 2.0F;
    packed.color = {srgb_to_linear_exact(light.color[0]) * scaled,
                    srgb_to_linear_exact(light.color[1]) * scaled,
                    srgb_to_linear_exact(light.color[2]) * scaled, specular};
    const Vec2 cone = slash::cos_sin(light.spot_angle * (kPi / 180.0F));
    packed.params = {light.attenuation, cone.x, 1.0F / light.spot_attenuation,
                     static_cast<float>(light.cull_mask & kRenderLayers)};
    packed.shadow = {static_cast<float>(shadow_slot), light.baked ? 1.0F : 0.0F,
                     light.volumetric_fog_energy, static_cast<float>(ray_slot + 1)};
    return packed;
}

std::size_t lights_drawn(const Scene3D& scene) {
    return scene.method == RenderMethod::Compatibility ? std::min(scene.lights.size(), kMaxLights3D)
                                                       : scene.lights.size();
}

Bounds mesh_bounds(std::span<const Vertex3D> vertices) {
    if (vertices.empty()) {
        return {};
    }
    Bounds bounds{.low = vertices[0].position, .high = vertices[0].position};
    for (const Vertex3D& vertex : vertices) {
        for (std::size_t i = 0; i < 3; ++i) {
            bounds.low.at(i) = std::min(bounds.low.at(i), vertex.position.at(i));
            bounds.high.at(i) = std::max(bounds.high.at(i), vertex.position.at(i));
        }
    }
    return bounds;
}

float mesh_draw_depth(const MeshDraw& draw, const Bounds& bounds, const Scene3D& scene) {
    const Float3 center = scale(add(bounds.low, bounds.high), 0.5F);
    const Float4 world = transform(draw.transform, {center[0], center[1], center[2], 1.0F});
    const Float4 seen = transform(scene.camera, world);
    return -seen[2];
}

namespace {

Result<void> check_vertices(std::span<const Vertex3D> vertices) {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vertex3D& v = vertices[i];
        if (!finite({v.position[0], v.position[1], v.position[2], v.normal[0], v.normal[1],
                     v.normal[2], v.tangent[0], v.tangent[1], v.tangent[2], v.tangent[3], v.uv[0],
                     v.uv[1]})) {
            return make_error(ErrorCode::InvalidArgument,
                              "vertex " + std::to_string(i) + "'s numbers are finite");
        }
    }
    return {};
}

} // namespace

Result<void> check_mesh(std::span<const Vertex3D> vertices, std::span<const std::uint32_t> indices,
                        std::span<const std::span<const std::uint32_t>> lods) {
    std::size_t all_indices = indices.empty() ? vertices.size() : indices.size();
    for (std::size_t lod = 0; lod < lods.size(); ++lod) {
        const std::span<const std::uint32_t> lod_indices = lods[lod];
        all_indices += lod_indices.size();
        if (lod_indices.empty() || lod_indices.size() % 3 != 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "a mesh's LOD " + std::to_string(lod + 1) +
                                  " has a multiple of 3 indices, more than none, not " +
                                  std::to_string(lod_indices.size()));
        }
        for (std::size_t i = 0; i < lod_indices.size(); ++i) {
            if (lod_indices[i] >= vertices.size()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "index " + std::to_string(i) + " of LOD " +
                                      std::to_string(lod + 1) + " is " +
                                      std::to_string(lod_indices[i]) + ", past the mesh's " +
                                      std::to_string(vertices.size()) + " vertices");
            }
        }
    }
    if (all_indices > kMaxMeshIndices) {
        return make_error(ErrorCode::InvalidArgument,
                          "a mesh has at most " + std::to_string(kMaxMeshIndices) +
                              " indices with its LODs', not " + std::to_string(all_indices));
    }
    if (vertices.size() > kMaxMeshVertices) {
        return make_error(ErrorCode::InvalidArgument,
                          "a mesh has at most " + std::to_string(kMaxMeshVertices) +
                              " vertices, not " + std::to_string(vertices.size()));
    }
    if (indices.size() > kMaxMeshIndices) {
        return make_error(ErrorCode::InvalidArgument,
                          "a mesh has at most " + std::to_string(kMaxMeshIndices) +
                              " indices, not " + std::to_string(indices.size()));
    }
    const std::size_t corners = indices.empty() ? vertices.size() : indices.size();
    if (corners == 0 || corners % 3 != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          indices.empty()
                              ? "a mesh without indices has a multiple of 3 vertices, not " +
                                    std::to_string(vertices.size())
                              : "a mesh has a multiple of 3 indices, not " +
                                    std::to_string(indices.size()));
    }
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertices.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "index " + std::to_string(i) + " is " + std::to_string(indices[i]) +
                                  ", past the mesh's " + std::to_string(vertices.size()) +
                                  " vertices");
        }
    }
    return check_vertices(vertices);
}

std::vector<std::uint32_t> mesh_index_buffer(std::size_t vertex_count,
                                             std::span<const std::uint32_t> indices,
                                             std::span<const std::span<const std::uint32_t>> lods,
                                             std::vector<IndexRange>& ranges) {
    std::vector<std::uint32_t> out;
    ranges.clear();
    if (indices.empty()) {
        out.resize(vertex_count);
        std::iota(out.begin(), out.end(), 0U);
    } else {
        out.assign(indices.begin(), indices.end());
    }
    ranges.push_back({.first = 0, .count = static_cast<std::uint32_t>(out.size())});
    for (const std::span<const std::uint32_t> lod : lods) {
        ranges.push_back({.first = static_cast<std::uint32_t>(out.size()),
                          .count = static_cast<std::uint32_t>(lod.size())});
        out.insert(out.end(), lod.begin(), lod.end());
    }
    return out;
}

Result<void> check_lod(std::uint32_t lod, std::size_t lod_count) {
    if (lod > lod_count) {
        return make_error(ErrorCode::InvalidArgument, "a mesh with " + std::to_string(lod_count) +
                                                          " LODs has no LOD " +
                                                          std::to_string(lod));
    }
    return {};
}

Result<void> check_mesh_update(std::span<const Vertex3D> vertices, std::size_t count) {
    if (vertices.size() != count) {
        return make_error(ErrorCode::InvalidArgument,
                          "a mesh's new vertices are as many as it has, " + std::to_string(count) +
                              ", not " + std::to_string(vertices.size()));
    }
    return check_vertices(vertices);
}

Result<void> check_scene(const Scene3D& scene) {
    if (!finite(scene.camera) || !finite(scene.projection) ||
        !finite({scene.ambient[0], scene.ambient[1], scene.ambient[2], scene.ambient_energy})) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene's camera, projection and ambient light are finite");
    }
    if (const std::optional<PathTrace3D>& paths = scene.path_trace;
        paths && (paths->samples < 1 || paths->samples > kMaxPathSamples ||
                  paths->bounces > kMaxPathBounces)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a path-traced frame takes 1 to " + std::to_string(kMaxPathSamples) +
                              " samples and up to " + std::to_string(kMaxPathBounces) + " bounces");
    }
    const auto directional = static_cast<std::size_t>(
        std::ranges::count(scene.lights, LightType::Directional, &Light3D::type));
    if (directional > kMaxDirectionalLights3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene takes up to " + std::to_string(kMaxDirectionalLights3D) +
                              " directional lights, not " + std::to_string(directional));
    }
    if (scene.lights.size() - directional > kMaxClusteredLights3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene takes up to " + std::to_string(kMaxClusteredLights3D) +
                              " omni and spot lights, not " +
                              std::to_string(scene.lights.size() - directional));
    }
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        const Light3D& l = scene.lights[i];
        if (!finite({l.color[0], l.color[1], l.color[2], l.energy, l.specular, l.position[0],
                     l.position[1], l.position[2], l.direction[0], l.direction[1], l.direction[2],
                     l.range, l.attenuation, l.spot_angle, l.spot_attenuation})) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) + "'s numbers are finite");
        }
        if (l.type != LightType::Omni && dot3(l.direction, l.direction) == 0.0F) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) + " has a direction");
        }
        if (!finite({l.up[0], l.up[1], l.up[2], l.shadow_bias, l.shadow_normal_bias,
                     l.shadow_opacity, l.shadow_splits[0], l.shadow_splits[1], l.shadow_splits[2],
                     l.shadow_fade_start, l.shadow_max_distance, l.shadow_pancake_size})) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) + "'s shadow numbers are finite");
        }
    }
    if (!valid_shadow_atlas_size(scene.shadows.directional_size) ||
        !valid_shadow_atlas_size(scene.shadows.positional_size)) {
        return make_error(ErrorCode::InvalidArgument,
                          "shadow atlases are powers of two from 256 to " +
                              std::to_string(kMaxTextureSize) + " texels across");
    }
    return check_decals(scene.decals);
}

Error unknown_mesh(MeshId mesh) {
    return make_error(ErrorCode::NotFound,
                      "no mesh " + std::to_string(static_cast<std::uint32_t>(mesh)));
}

Error no_scene() {
    return make_error(ErrorCode::InvalidArgument,
                      "draw_mesh needs a frame with a scene (Frame::scene)");
}

Result<void> check_previous_transform(const std::optional<Mat4>& previous) {
    if (previous &&
        !std::ranges::all_of(*previous, [](float value) { return std::isfinite(value); })) {
        return make_error(ErrorCode::InvalidArgument, "a mesh's previous transform is finite");
    }
    return {};
}

MeshCorner mesh_vertex(const MeshCameraUniforms& camera, const MeshInstanceUniforms& instance,
                       const Vertex3D& vertex) {
    return mesh_vertex(camera, instance, vertex, vertex.position);
}

MeshCorner mesh_vertex(const MeshCameraUniforms& camera, const MeshInstanceUniforms& instance,
                       const Vertex3D& vertex, const Float3& previous) {
    MeshCorner out;
    const Float4 world = transform(
        instance.model, {vertex.position[0], vertex.position[1], vertex.position[2], 1.0F});
    out.world = {world[0], world[1], world[2]};
    out.clip = transform(camera.view_projection, world);
    out.motion_clip = out.clip;
    out.motion_clip[0] -= camera.jitter[0] * out.clip[3];
    out.motion_clip[1] -= camera.jitter[1] * out.clip[3];
    out.previous_clip = transform(
        camera.previous_view_projection,
        transform(instance.previous_model, {previous[0], previous[1], previous[2], 1.0F}));
    const Float3 tangent{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]};
    const Float3 binormal = normalize(scale(cross(vertex.normal, tangent), vertex.tangent[3]));
    out.normal = normalize(transform3(instance.normal_model, vertex.normal));
    out.tangent = normalize(transform3(instance.normal_model, tangent));
    out.binormal = normalize(transform3(instance.normal_model, binormal));
    out.uv = {(vertex.uv[0] * instance.uv1[0]) + instance.uv1[2],
              (vertex.uv[1] * instance.uv1[1]) + instance.uv1[3]};
    constexpr float kByte = 255.0F;
    out.color = {
        static_cast<float>(vertex.rgba[0]) / kByte, static_cast<float>(vertex.rgba[1]) / kByte,
        static_cast<float>(vertex.rgba[2]) / kByte, static_cast<float>(vertex.rgba[3]) / kByte};
    out.uv2 = {(vertex.uv2[0] * instance.lightmap_uv[2]) + instance.lightmap_uv[0],
               (vertex.uv2[1] * instance.lightmap_uv[3]) + instance.lightmap_uv[1]};
    return out;
}

Float4 sample_linear(const FloatImage& image, std::array<float, 2> uv) {
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() != std::size_t{image.width} * image.height * 4) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    const auto texel = [&](float x, float y) {
        const auto column =
            static_cast<std::size_t>(std::clamp(x, 0.0F, static_cast<float>(image.width - 1)));
        const auto row =
            static_cast<std::size_t>(std::clamp(y, 0.0F, static_cast<float>(image.height - 1)));
        const std::size_t at = ((row * image.width) + column) * 4;
        return Float4{image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]};
    };
    // Texel centers are at half-texel coordinates, as on the GPU.
    const float x = (uv[0] * static_cast<float>(image.width)) - 0.5F;
    const float y = (uv[1] * static_cast<float>(image.height)) - 0.5F;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float tx = x - x0;
    const float ty = y - y0;
    const Float4 top_left = texel(x0, y0);
    const Float4 top_right = texel(x0 + 1.0F, y0);
    const Float4 bottom_left = texel(x0, y0 + 1.0F);
    const Float4 bottom_right = texel(x0 + 1.0F, y0 + 1.0F);
    Float4 color{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const float top =
            top_left.at(channel) + ((top_right.at(channel) - top_left.at(channel)) * tx);
        const float bottom =
            bottom_left.at(channel) + ((bottom_right.at(channel) - bottom_left.at(channel)) * tx);
        color.at(channel) = top + ((bottom - top) * ty);
    }
    return color;
}

Float3 lightmap_light(const MeshSurfaceUniforms& surface, const FloatImage& image,
                      std::array<float, 2> uv) {
    Float3 light{};
    if (surface.lightmap[1] != 0.0F && image.width > 0 && image.height > 0) {
        // Godot's textureArray_bicubic: a cubic B-spline as four bilinear taps.
        const auto w0 = [](float a) {
            return (1.0F / 6.0F) * ((a * ((a * (-a + 3.0F)) - 3.0F)) + 1.0F);
        };
        const auto w1 = [](float a) {
            return (1.0F / 6.0F) * ((a * a * ((3.0F * a) - 6.0F)) + 4.0F);
        };
        const auto w2 = [](float a) {
            return (1.0F / 6.0F) * ((a * ((a * ((-3.0F * a) + 3.0F)) + 3.0F)) + 1.0F);
        };
        const auto w3 = [](float a) { return (1.0F / 6.0F) * (a * a * a); };
        const std::array<float, 2> size{static_cast<float>(image.width),
                                        static_cast<float>(image.height)};
        std::array<float, 2> iuv{};
        std::array<float, 2> g0{};
        std::array<float, 2> g1{};
        std::array<float, 2> h0{};
        std::array<float, 2> h1{};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const float at = (uv.at(axis) * size.at(axis)) + 0.5F;
            iuv.at(axis) = std::floor(at);
            const float f = at - iuv.at(axis);
            g0.at(axis) = w0(f) + w1(f);
            g1.at(axis) = w2(f) + w3(f);
            h0.at(axis) = -1.0F + (w1(f) / g0.at(axis));
            h1.at(axis) = 1.0F + (w3(f) / g1.at(axis));
        }
        const auto tap = [&](float hx, float hy) {
            return xyz(sample_linear(
                image, {(iuv[0] + hx - 0.5F) / size[0], (iuv[1] + hy - 0.5F) / size[1]}));
        };
        light = add(
            scale(add(scale(tap(h0[0], h0[1]), g0[0]), scale(tap(h1[0], h0[1]), g1[0])), g0[1]),
            scale(add(scale(tap(h0[0], h1[1]), g0[0]), scale(tap(h1[0], h1[1]), g1[0])), g1[1]));
    } else {
        light = xyz(sample_linear(image, uv));
    }
    return scale(light, surface.lightmap[0]);
}

Float3 capture_light(const MeshSurfaceUniforms& surface, const Float3& n) {
    const std::array<Float4, 9>& c = surface.capture;
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        // In the shader's order.
        float sum = 0.886227F * c[0].at(i);
        sum += 1.023327F * c[1].at(i) * n[1];
        sum += 1.023327F * c[2].at(i) * n[2];
        sum += 1.023327F * c[3].at(i) * n[0];
        sum += 0.858086F * c[4].at(i) * (n[0] * n[1]);
        sum += 0.858086F * c[5].at(i) * (n[1] * n[2]);
        sum += 0.247708F * c[6].at(i) * ((3.0F * n[2] * n[2]) - 1.0F);
        sum += 0.858086F * c[7].at(i) * (n[0] * n[2]);
        sum += 0.429043F * c[8].at(i) * ((n[0] * n[0]) - (n[1] * n[1]));
        out.at(i) = sum;
    }
    return out;
}

std::uint32_t range_mask(std::uint32_t word, std::uint32_t first, std::uint32_t end) {
    const auto at = static_cast<std::int64_t>(word) * 32;
    const std::int64_t low =
        std::clamp(static_cast<std::int64_t>(first) - at, std::int64_t{0}, std::int64_t{32});
    const std::int64_t high =
        std::clamp(static_cast<std::int64_t>(end) - at, std::int64_t{0}, std::int64_t{32});
    const std::uint32_t below_high = high >= 32 ? 0xFFFFFFFFU : (1U << high) - 1U;
    const std::uint32_t below_low = low >= 32 ? 0xFFFFFFFFU : (1U << low) - 1U;
    return below_high & ~below_low;
}

namespace {

// The clustered loop of lit_color in shaders/mesh.hlsl: each of the pixel's tile's lights within
// its slice's range, nearest first.
template <typename AddLight>
void add_cluster_lights(const LightClusters& clusters, const MeshShadowUniforms& shadows,
                        const MeshPixel& pixel, const AddLight& add_light) {
    const std::uint32_t tile_x = std::min(pixel.x / kClusterSize, clusters.tiles_across - 1);
    const std::uint32_t tile_y = std::min(pixel.y / kClusterSize, clusters.tiles_down - 1);
    const std::uint32_t words = clusters.words;
    const std::size_t cell =
        ((std::size_t{tile_y} * clusters.tiles_across) + tile_x) * clusters.stride();
    const float depth =
        -(dot3(pixel.world, {shadows.camera_z[0], shadows.camera_z[1], shadows.camera_z[2]}) +
          shadows.camera_z[3]);
    const auto slice = static_cast<std::uint32_t>(std::clamp(
        depth * clusters.slices_per_depth, 0.0F, static_cast<float>(kClusterSlices - 1)));
    const std::uint32_t range = clusters.cells[cell + words + slice];
    const std::uint32_t first = range & 0xFFFFU;
    const std::uint32_t end = range >> 16U;
    for (std::uint32_t word = first >> 5U; word < (end + 31U) >> 5U; ++word) {
        std::uint32_t mask = clusters.cells[cell + word] & range_mask(word, first, end);
        while (mask != 0) {
            const auto bit = static_cast<std::uint32_t>(std::countr_zero(mask));
            mask &= mask - 1U;
            add_light(clusters.lights[(word * 32U) + bit]);
        }
    }
}

// What fs_main works out before it lights a pixel: its albedo, linear, and alpha, its metallic and
// roughness, and its shading normal.
struct SurfacePixel {
    Float3 base{};
    float alpha = 1.0F;
    float metallic = 0.0F;
    float roughness = 0.0F;
    Float3 normal{};
    bool terrain = false;
    TerrainTexel ground;
};

// The surface at a pixel, or nullopt where the alpha scissor discards it.
std::optional<SurfacePixel> surface_pixel(const MeshSurfaceUniforms& surface,
                                          const MeshPixel& pixel) {
    const std::uint32_t flags = surface.options[0];
    Float4 color{};
    for (std::size_t i = 0; i < 4; ++i) {
        color.at(i) = surface.albedo.at(i) * pixel.texels[0].at(i);
    }
    if ((flags & kMeshVertexColorAlbedo) != 0) {
        for (std::size_t i = 0; i < 4; ++i) {
            color.at(i) *= pixel.color.at(i);
        }
    }
    SurfacePixel out;
    out.alpha = (flags & kMeshHasAlpha) != 0 ? color[3] : 1.0F;
    if ((flags & kMeshAlphaScissor) != 0) {
        if (out.alpha < same_bits<float>(surface.options[1])) {
            return std::nullopt;
        }
        out.alpha = 1.0F;
    }
    out.terrain = (surface.options[3] & kMeshTerrain) != 0;
    if (out.terrain) {
        out.ground = terrain_texel(surface, pixel.texels);
    }
    out.base = out.terrain ? mul(srgb_to_linear(xyz(surface.albedo)), out.ground.albedo)
                           : srgb_to_linear({color[0], color[1], color[2]});
    const auto dot4 = [](const Float4& a, const Float4& b) {
        return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
    };
    out.metallic = out.terrain
                       ? surface.surface[0]
                       : surface.surface[0] * dot4(pixel.texels[1], surface.metallic_channel);
    out.roughness = out.terrain
                        ? surface.surface[1] * out.ground.roughness
                        : surface.surface[1] * dot4(pixel.texels[2], surface.roughness_channel);
    Float3 normal = out.terrain ? frame_direction(pixel, out.ground.normal) : pixel.normal;
    if ((flags & kMeshSideCheck) != 0 && !pixel.front) {
        normal = scale(normal, -1.0F);
    }
    if ((flags & kMeshNormalMap) != 0) {
        Float3 mapped = xyz(pixel.texels[4]);
        mapped[0] = (mapped[0] * 2.0F) - 1.0F;
        mapped[1] = (mapped[1] * 2.0F) - 1.0F;
        mapped[2] =
            std::sqrt(std::max(0.0F, 1.0F - ((mapped[0] * mapped[0]) + (mapped[1] * mapped[1]))));
        const Float3 bent =
            add(add(scale(pixel.tangent, mapped[0]), scale(pixel.binormal, mapped[1])),
                scale(normal, mapped[2]));
        out.normal = normalize(lerp3(normal, bent, surface.surface[3]));
    } else {
        out.normal = normalize(normal);
    }
    return out;
}

} // namespace

std::optional<PathSurface> path_surface(const MeshSurfaceUniforms& surface,
                                        const MeshPixel& pixel) {
    const std::optional<SurfacePixel> found = surface_pixel(surface, pixel);
    if (!found) {
        return std::nullopt;
    }
    const std::uint32_t flags = surface.options[0];
    PathSurface out{.base = found->base,
                    .alpha = found->alpha,
                    .metallic = found->metallic,
                    .roughness = found->roughness,
                    .normal = found->normal,
                    .emitted = {},
                    .unshaded = (flags & kMeshUnshaded) != 0};
    if (!out.unshaded) {
        Float3 emitted{surface.emission[0], surface.emission[1], surface.emission[2]};
        if ((flags & kMeshEmissionTexture) != 0) {
            emitted = add(emitted, xyz(pixel.texels[3]));
        }
        out.emitted = scale(srgb_to_linear(emitted), surface.emission[3]);
    }
    return out;
}

LightResponse light_response(const Float3& n, const Float3& l, const Float3& v,
                             const Float3& light_color, float attenuation, const Float3& f0,
                             float roughness, float metallic, float specular_amount) {
    LightSum sum;
    light_compute(n, l, v, light_color, attenuation, f0, roughness, metallic, specular_amount, sum);
    return {.diffuse = sum.diffuse, .specular = sum.specular};
}

std::optional<LightReach> light_reach(const MeshLightUniforms& light, const Float3& world) {
    const float type = light.direction[3];
    if (type == 0.0F) {
        return LightReach{
            .toward = normalize({light.direction[0], light.direction[1], light.direction[2]}),
            .distance = std::numeric_limits<float>::infinity(),
            .attenuation = 1.0F};
    }
    const Float3 to_light = sub({light.position[0], light.position[1], light.position[2]}, world);
    const float distance = length(to_light);
    if (!(distance > 0.0F)) {
        return std::nullopt;
    }
    const Float3 toward = scale(to_light, 1.0F / distance);
    float attenuation = omni_attenuation(distance, light.position[3], light.params[0]);
    if (type == 2.0F) {
        const float cone = light.params[1];
        const float scos =
            std::max(dot3(scale(toward, -1.0F),
                          {light.direction[0], light.direction[1], light.direction[2]}),
                     cone);
        const float rim = std::max(0.0001F, (1.0F - scos) / (1.0F - cone));
        attenuation *= 1.0F - power(rim, light.params[2]);
    }
    if (!(attenuation > 0.0F)) {
        return std::nullopt;
    }
    return LightReach{.toward = toward, .distance = distance, .attenuation = attenuation};
}

float ray_shadow(const MeshMaps& maps, std::size_t slot, std::uint32_t x, std::uint32_t y) {
    return maps.ray_shadows[(((std::size_t{y} * maps.ao_width) + x) * kMaxRayShadowLights) + slot];
}

std::optional<Float4> mesh_normal_roughness(const MeshSurfaceUniforms& surface,
                                            const MeshSceneUniforms& scene,
                                            const MeshPixel& pixel) {
    const std::optional<SurfacePixel> found = surface_pixel(surface, pixel);
    if (!found) {
        return std::nullopt;
    }
    const Float3& n = found->normal;
    const Float3 view =
        normalize({dot3(xyz(scene.view_rows[0]), n), dot3(xyz(scene.view_rows[1]), n),
                   dot3(xyz(scene.view_rows[2]), n)});
    return Float4{(view[0] * 0.5F) + 0.5F, (view[1] * 0.5F) + 0.5F, (view[2] * 0.5F) + 0.5F,
                  found->roughness * (127.0F / 255.0F)};
}

std::optional<Float4> shade_mesh(const MeshSurfaceUniforms& surface, const MeshSceneUniforms& scene,
                                 const MeshMaps& maps, const MeshPixel& pixel) {
    const std::uint32_t flags = surface.options[0];
    const std::optional<SurfacePixel> found = surface_pixel(surface, pixel);
    if (!found) {
        return std::nullopt;
    }
    const float alpha = found->alpha;
    // Fogged unless the material disables it, then linear for Forward+, or times the exposure,
    // tonemapped unless the post pass does it, and as sRGB.
    const auto finish = [&](Float3 lit) {
        const bool fog_on = (flags & kMeshDisableFog) == 0;
        if (scene.volumetric_fog[0] != 0.0F && fog_on && maps.uniforms != nullptr) {
            // Forward+'s volumetric fog, as Godot's scene_forward_clustered.glsl blends it: the
            // plain fog as the light it adds and how much it lets through, seen through the
            // volumetric fog in front.
            Float4 fog{0.0F, 0.0F, 0.0F, 1.0F};
            if (scene.fog_color[3] != 0.0F) {
                const Float4 plain = fog_process(scene, pixel.world);
                fog = {plain[0] * plain[3], plain[1] * plain[3], plain[2] * plain[3],
                       1.0F - plain[3]};
            }
            const Float4& z = maps.uniforms->camera_z;
            const float depth = -((pixel.world[0] * z[0]) + (pixel.world[1] * z[1]) +
                                  (pixel.world[2] * z[2]) + z[3]);
            const Float4 volume =
                sample_fog_volume(maps.fog,
                                  {static_cast<std::uint32_t>(scene.fog_volume[0]),
                                   static_cast<std::uint32_t>(scene.fog_volume[1]),
                                   static_cast<std::uint32_t>(scene.fog_volume[2])},
                                  scene.volumetric_fog,
                                  {(static_cast<float>(pixel.x) + 0.5F) / scene.screen[0],
                                   (static_cast<float>(pixel.y) + 0.5F) / scene.screen[1]},
                                  depth);
            fog = {(fog[0] * volume[3]) + volume[0], (fog[1] * volume[3]) + volume[1],
                   (fog[2] * volume[3]) + volume[2], fog[3] * volume[3]};
            lit = add(scale(lit, fog[3]), {fog[0], fog[1], fog[2]});
        } else if (scene.fog_color[3] != 0.0F && fog_on) {
            const Float4 fogged = fog_process(scene, pixel.world);
            lit = lerp3(lit, {fogged[0], fogged[1], fogged[2]}, fogged[3]);
        }
        if (scene.tonemap[3] == 2.0F) {
            return Float4{lit[0], lit[1], lit[2], alpha};
        }
        Float3 exposed = scale(lit, scene.tonemap[0]);
        if (scene.tonemap[3] != 0.0F) {
            exposed =
                tonemap(exposed, scene.tonemap[1],
                        static_cast<Tonemapper>(static_cast<std::uint32_t>(scene.tonemap[2])));
        }
        const Float3 out = linear_to_srgb(exposed);
        return Float4{out[0], out[1], out[2], alpha};
    };
    const bool terrain = found->terrain;
    const TerrainTexel& ground = found->ground;
    Float3 emitted{};
    if ((flags & kMeshUnshaded) == 0) {
        emitted = {surface.emission[0], surface.emission[1], surface.emission[2]};
        if ((flags & kMeshEmissionTexture) != 0) {
            emitted = add(emitted, xyz(pixel.texels[3]));
        }
        emitted = scale(srgb_to_linear(emitted), surface.emission[3]);
    }
    // Shadows offset the point they sample along the vertex normal, as Godot's normal_interp; a
    // terrain's along its shape's.
    const Float3 geometry =
        terrain ? frame_direction(pixel, ground.shape) : normalize(pixel.normal);
    // Forward+'s decals change the surface before it is lit, as Godot's, with the vertex normal
    // turned to the pixel's side.
    DecalSurface decaled{.base = found->base,
                         .normal = found->normal,
                         .ao = 1.0F,
                         .roughness = found->roughness,
                         .metallic = found->metallic,
                         .emitted = emitted};
    if (maps.decal_atlas != nullptr) {
        const bool flipped = (flags & kMeshSideCheck) != 0 && !pixel.front;
        apply_decals(scene, maps, pixel, surface.options[2],
                     flipped ? scale(geometry, -1.0F) : geometry, decaled);
    }
    const Float3& base = decaled.base;
    if ((flags & kMeshUnshaded) != 0) {
        return finish(base);
    }

    const float metallic = decaled.metallic;
    const float roughness = decaled.roughness;
    const Float3& normal = decaled.normal;
    emitted = decaled.emitted;
    // Godot's SSAO: the frame's occlusion at the pixel darkens the opaque meshes' ambient light,
    // with the decals' ambient occlusion, and their direct light by its light affect times its AO
    // channel affect.
    float ao = decaled.ao;
    float ao_direct = 1.0F;
    const bool opaque = (flags & kMeshHasAlpha) == 0 || (flags & kMeshAlphaScissor) != 0;
    if (scene.ssao[0] != 0.0F && opaque && !maps.ao.empty()) {
        ao = std::min(ao, maps.ao[(std::size_t{pixel.y} * maps.ao_width) + pixel.x]);
        const float affect = std::max(0.0F, scene.ssao[1]) * scene.ssao[2];
        ao_direct = 1.0F + ((ao - 1.0F) * affect);
    }
    // An opaque pixel of a frame with ray-traced shadows takes its lights' from them.
    const bool traced = scene.screen[2] != 0.0F && opaque && !maps.ray_shadows.empty();
    // Its SSR (alpha below 0 for none) goes over their reflections of the sky.
    Float4 ssr{0.0F, 0.0F, 0.0F, -1.0F};
    if (scene.ssao[3] != 0.0F && opaque && !maps.ssr.empty()) {
        const std::size_t at = ((std::size_t{pixel.y} * maps.ao_width) + pixel.x) * 4;
        ssr = {maps.ssr[at], maps.ssr[at + 1], maps.ssr[at + 2], maps.ssr[at + 3]};
    }
    const float specular = surface.surface[2];

    const Float3 view =
        normalize(sub({scene.camera[0], scene.camera[1], scene.camera[2]}, pixel.world));
    const float dielectric = 0.16F * specular * specular;
    const Float3 f0 = lerp3({dielectric, dielectric, dielectric}, base, metallic);

    LightSum light_sum;
    // add_light in shaders/mesh.hlsl.
    const bool lightmapped = (flags & kMeshLightmap) != 0;
    const bool captured = (flags & kMeshLightmapCapture) != 0;
    const auto add_light = [&](const MeshLightUniforms& light) {
        if ((static_cast<std::uint32_t>(light.params[3]) & surface.options[2]) == 0) {
            return;
        }
        // A baked light's light is in the lightmap already, as Godot skips BAKE_STATIC lights.
        if (light.shadow[1] != 0.0F && lightmapped) {
            return;
        }
        const float type = light.direction[3];
        const Float3 light_color{light.color[0], light.color[1], light.color[2]};
        float slot = light.shadow[0];
        float traced_shadow = 1.0F;
        if (traced && light.shadow[3] > 0.0F) {
            traced_shadow =
                ray_shadow(maps, static_cast<std::size_t>(light.shadow[3]) - 1, pixel.x, pixel.y);
            slot = -1.0F;
        }
        if (type == 0.0F) {
            const Float3 toward =
                normalize({light.direction[0], light.direction[1], light.direction[2]});
            const float shadow = slot >= 0.0F && maps.uniforms != nullptr
                                     ? directional_shadow(maps, static_cast<std::size_t>(slot),
                                                          pixel.world, geometry, toward)
                                     : traced_shadow;
            light_compute(normal, toward, view, light_color, shadow, f0, roughness, metallic,
                          light.color[3], light_sum);
            return;
        }
        const Float3 to_light =
            sub({light.position[0], light.position[1], light.position[2]}, pixel.world);
        const float distance = length(to_light);
        float attenuation = omni_attenuation(distance, light.position[3], light.params[0]);
        if (type == 2.0F) {
            const float cone = light.params[1];
            const float scos =
                std::max(dot3(scale(normalize(to_light), -1.0F),
                              {light.direction[0], light.direction[1], light.direction[2]}),
                         cone);
            const float rim = std::max(0.0001F, (1.0F - scos) / (1.0F - cone));
            attenuation *= 1.0F - power(rim, light.params[2]);
        }
        if (slot >= 0.0F && maps.uniforms != nullptr) {
            attenuation *=
                positional_shadow(maps, static_cast<std::size_t>(slot), pixel.world, geometry);
        }
        attenuation *= traced_shadow;
        light_compute(normal, normalize(to_light), view, light_color, attenuation, f0, roughness,
                      metallic, light.color[3], light_sum);
    };
    const auto count = static_cast<std::size_t>(scene.camera[3]);
    for (std::size_t i = 0; i < count && i < kMaxLights3D; ++i) {
        add_light(scene.lights.at(i));
    }
    if (maps.clusters != nullptr && maps.uniforms != nullptr) {
        add_cluster_lights(*maps.clusters, *maps.uniforms, pixel, add_light);
    }

    // Light from the sky's radiance, as Godot's scene.glsl takes it: reflections by roughness
    // and ambient light by the normal, then specular occlusion and Lazarov's environment BRDF.
    // A lightmapped pixel's ambient light is its lightmap's, and a captured one's its light
    // probes', either standing in for the scene's and the probes', as Godot's USE_LIGHTMAP.
    auto ambient_light = Float3{scene.ambient[0], scene.ambient[1], scene.ambient[2]};
    if (lightmapped) {
        ambient_light = pixel.lightmap;
    } else if (captured) {
        ambient_light = capture_light(surface, normal);
    }
    Float3 reflected{};
    if (maps.radiance != nullptr) {
        const auto to_sky = [&](const Float3& v) {
            return Float3{dot3(xyz(scene.sky_rows[0]), v), dot3(xyz(scene.sky_rows[1]), v),
                          dot3(xyz(scene.sky_rows[2]), v)};
        };
        if (scene.sky[1] != 0.0F) {
            Float3 ray = sub(scale(normal, 2.0F * dot3(normal, view)), view);
            ray = lerp3(ray, normal, roughness * roughness);
            const float horizon = std::min(1.0F + dot3(ray, normal), 1.0F);
            const Float4 texel =
                maps.radiance->sample(to_sky(ray), std::sqrt(roughness) * kRadianceMaxLevel);
            reflected = scale(srgb_to_linear(xyz(texel)), horizon * horizon * scene.sky[2]);
        }
        if (scene.sky[0] != 0.0F && !lightmapped && !captured) {
            const Float4 texel = maps.radiance->sample(to_sky(normal), kRadianceMaxLevel);
            ambient_light =
                lerp3(ambient_light, scale(srgb_to_linear(xyz(texel)), scene.sky[2]), scene.sky[3]);
        }
    }
    // Forward+'s VoxelGIs, as Godot's GI buffers light a pixel: their ambient light and
    // reflections go over the sky's by how much of their cones met something.
    if (scene.voxel_gi[0] != 0.0F && surface.voxel_gi[0] > 0.0F && pixel.voxel_gi != nullptr &&
        !lightmapped && !captured) {
        const VoxelGILight gi = trace_voxel_gis(surface, scene.voxel_gi[1] != 0.0F, *pixel.voxel_gi,
                                                pixel.world, normal, view, roughness);
        ambient_light = lerp3(ambient_light, xyz(gi.ambient), gi.ambient[3]);
        reflected = lerp3(reflected, xyz(gi.reflection), gi.reflection[3]);
    }
    // Forward+'s reflection probes, as Godot's scene_forward_clustered.glsl takes them: where
    // they reach, their reflections and ambient light go over the sky's by how much they cover.
    if (scene.reflections[0] > 0.0F && !maps.probes.empty() && maps.probe_cubes != nullptr) {
        const ProbeLight probe =
            reflect_probes(maps.probes, *maps.probe_cubes, scene.reflections[2], pixel.world,
                           normal, view, roughness, surface.options[2]);
        const Float4& r = probe.reflection;
        const Float4& a = probe.ambient;
        for (std::size_t i = 0; i < 3; ++i) {
            if (r[3] > 0.0F) {
                reflected.at(i) =
                    r[3] < 1.0F ? (reflected.at(i) * (1.0F - r[3])) + r.at(i) : r.at(i);
            }
            if (a[3] > 0.0F && !lightmapped && !captured) {
                ambient_light.at(i) =
                    a[3] < 1.0F ? (ambient_light.at(i) * (1.0F - a[3])) + a.at(i) : a.at(i);
            }
        }
    }
    // Its ray-traced ambient light stands in for all of those but a lightmap's.
    if (scene.screen[3] != 0.0F && opaque && !maps.gi.empty() && !lightmapped) {
        const std::size_t at = ((std::size_t{pixel.y} * maps.ao_width) + pixel.x) * 3;
        ambient_light = {maps.gi[at], maps.gi[at + 1], maps.gi[at + 2]};
    }
    ambient_light = scale(ambient_light, ao);
    float occlusion =
        ((ambient_light[0] * 0.3F) + (ambient_light[1] * 0.59F) + (ambient_light[2] * 0.11F)) *
        2.0F;
    occlusion = std::min(occlusion * 4.0F, 1.0F);
    const float reflective = (1.0F - roughness) * metallic;
    occlusion = std::max(std::min(reflective * occlusion * 10.0F, 1.0F), occlusion);
    const float n_dot_v = std::clamp(dot3(normal, view), 0.0F, 1.0F);
    const Float4 r{1.0F - roughness, (roughness * -0.0275F) + 0.0425F,
                   (roughness * -0.572F) + 1.04F, (roughness * 0.022F) - 0.04F};
    const float a004 =
        (std::min(r[0] * r[0], slash::exp(-9.28F * n_dot_v * std::numbers::ln2_v<float>)) * r[0]) +
        r[1];
    const float env_x = (-1.04F * a004) + r[2];
    const float env_y = (1.04F * a004) + r[3];
    const float env_g = std::min(std::max(50.0F * f0[1], metallic), 1.0F);
    if (ssr[3] < 0.0F) {
        for (std::size_t i = 0; i < 3; ++i) {
            reflected.at(i) *= occlusion * ((env_x * f0.at(i)) + (env_y * env_g));
        }
    } else {
        // Godot's SSR: the reflection, faded out toward the trace's roughness cutoff, put over
        // the sky's by its premultiplied alpha.
        const float fade =
            smoothstep(0.0F, 1.0F, 1.0F - std::clamp((roughness - 0.6F) / 0.1F, 0.0F, 1.0F));
        const float cover = ssr[3] * fade;
        for (std::size_t i = 0; i < 3; ++i) {
            reflected.at(i) *= occlusion;
            reflected.at(i) = (reflected.at(i) * (1.0F - cover)) + (ssr.at(i) * fade);
            reflected.at(i) *= (env_x * f0.at(i)) + (env_y * env_g);
        }
    }

    ambient_light = scale(mul(ambient_light, base), 1.0F - metallic);
    const Float3 diffuse_light =
        mul(light_sum.diffuse, scale(scale(base, ao_direct), 1.0F - metallic));
    const Float3 specular_light = scale(light_sum.specular, ao_direct);
    const Float3 lit =
        add(add(add(add(diffuse_light, specular_light), reflected), emitted), ambient_light);
    return finish(lit);
}

} // namespace slash::render::detail
