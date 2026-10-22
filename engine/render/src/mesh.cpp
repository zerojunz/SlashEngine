// Projections, the mesh shader's uniforms and its CPU twin (shade_mesh), which follows
// shaders/mesh.hlsl line for line in float math. pow is exp(y * ln(x)) through core/math, so the
// software device's meshes are the same pixels on every machine.

#include "checks.hpp"
#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "sky_shading.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
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
    return {.view_projection = multiply(scene.projection, scene.camera)};
}

MeshInstanceUniforms mesh_instance_uniforms(const Mat4& transform, const Material3D& material) {
    MeshInstanceUniforms out;
    out.model = transform;
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
    return out;
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
    out.options = {flags, same_bits<std::uint32_t>(material.alpha_scissor_threshold), layers, 0};
    return out;
}

MeshSceneUniforms mesh_scene_uniforms(const Scene3D& scene) {
    MeshSceneUniforms out;
    // The camera's place: the inverse of the camera matrix applied to the origin, -R^T t for its
    // rotation R and translation t, scaled back by the rows' lengths for a camera matrix that
    // scales.
    const Mat4& c = scene.camera;
    Float3 place{};
    for (std::size_t row = 0; row < 3; ++row) {
        const Float3 axis{c.at(row), c.at(4 + row), c.at(8 + row)};
        const float squared = dot3(axis, axis);
        place = add(place, scale(axis, -c.at(12 + row) / squared));
    }
    const std::size_t count = std::min(scene.lights.size(), kMaxLights3D);
    out.camera = {place[0], place[1], place[2], static_cast<float>(count)};
    const std::array<int, kMaxLights3D> slots = shadow_slots(scene);
    const float energy = scene.ambient_energy;
    out.ambient = {srgb_to_linear_exact(scene.ambient[0]) * energy,
                   srgb_to_linear_exact(scene.ambient[1]) * energy,
                   srgb_to_linear_exact(scene.ambient[2]) * energy, 0.0F};
    out.tonemap = {scene.post.exposure, scene.post.white, static_cast<float>(scene.post.tonemapper),
                   post_processes(scene.post) ? 0.0F : 1.0F};
    if (const Fog3D& fog = scene.fog; fog.enabled) {
        out.fog_color = {srgb_to_linear_exact(fog.light_color[0]) * fog.light_energy,
                         srgb_to_linear_exact(fog.light_color[1]) * fog.light_energy,
                         srgb_to_linear_exact(fog.light_color[2]) * fog.light_energy,
                         fog.depth ? 2.0F : 1.0F};
        out.fog = {fog.density, fog.sun_scatter, fog.height, fog.height_density};
        // As Godot's: depth fog ends at the far plane when its end is 0 or less, and begins
        // before it ends. Both projections put the far plane at [14] / [10].
        const Mat4& p = scene.projection;
        const float far = p[10] != 0.0F ? p[14] / p[10] : 4000.0F;
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
    for (std::size_t i = 0; i < count; ++i) {
        const Light3D& light = scene.lights[i];
        MeshLightUniforms& packed = out.lights.at(i);
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
        packed.shadow = {static_cast<float>(slots.at(i)), 0.0F, 0.0F, 0.0F};
    }
    return out;
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

Result<void> check_mesh(std::span<const Vertex3D> vertices,
                        std::span<const std::uint32_t> indices) {
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

Result<void> check_scene(const Scene3D& scene) {
    if (!finite(scene.camera) || !finite(scene.projection) ||
        !finite({scene.ambient[0], scene.ambient[1], scene.ambient[2], scene.ambient_energy})) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene's camera, projection and ambient light are finite");
    }
    if (scene.lights.size() > kMaxLights3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene takes up to " + std::to_string(kMaxLights3D) + " lights, not " +
                              std::to_string(scene.lights.size()));
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
    return {};
}

Error unknown_mesh(MeshId mesh) {
    return make_error(ErrorCode::NotFound,
                      "no mesh " + std::to_string(static_cast<std::uint32_t>(mesh)));
}

Error no_scene() {
    return make_error(ErrorCode::InvalidArgument,
                      "draw_mesh needs a frame with a scene (Frame::scene)");
}

MeshCorner mesh_vertex(const MeshCameraUniforms& camera, const MeshInstanceUniforms& instance,
                       const Vertex3D& vertex) {
    MeshCorner out;
    const Float4 world = transform(
        instance.model, {vertex.position[0], vertex.position[1], vertex.position[2], 1.0F});
    out.world = {world[0], world[1], world[2]};
    out.clip = transform(camera.view_projection, world);
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
    return out;
}

std::optional<Float4> shade_mesh(const MeshSurfaceUniforms& surface, const MeshSceneUniforms& scene,
                                 const MeshMaps& maps, const MeshPixel& pixel) {
    const std::uint32_t flags = surface.options[0];
    Float4 color{};
    for (std::size_t i = 0; i < 4; ++i) {
        color.at(i) = surface.albedo.at(i) * pixel.albedo_texel.at(i);
    }
    if ((flags & kMeshVertexColorAlbedo) != 0) {
        for (std::size_t i = 0; i < 4; ++i) {
            color.at(i) *= pixel.color.at(i);
        }
    }
    float alpha = (flags & kMeshHasAlpha) != 0 ? color[3] : 1.0F;
    if ((flags & kMeshAlphaScissor) != 0) {
        if (alpha < same_bits<float>(surface.options[1])) {
            return std::nullopt;
        }
        alpha = 1.0F;
    }
    // Fogged unless the material disables it, times the exposure, tonemapped unless the post pass
    // does it, and as sRGB.
    const auto finish = [&](Float3 lit) {
        if (scene.fog_color[3] != 0.0F && (flags & kMeshDisableFog) == 0) {
            const Float4 fogged = fog_process(scene, pixel.world);
            lit = lerp3(lit, {fogged[0], fogged[1], fogged[2]}, fogged[3]);
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
    const Float3 base = srgb_to_linear({color[0], color[1], color[2]});
    if ((flags & kMeshUnshaded) != 0) {
        return finish(base);
    }

    const auto dot4 = [](const Float4& a, const Float4& b) {
        return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
    };
    const float metallic =
        surface.surface[0] * dot4(pixel.metallic_texel, surface.metallic_channel);
    const float roughness =
        surface.surface[1] * dot4(pixel.roughness_texel, surface.roughness_channel);
    const float specular = surface.surface[2];
    Float3 emitted{surface.emission[0], surface.emission[1], surface.emission[2]};
    if ((flags & kMeshEmissionTexture) != 0) {
        emitted = add(emitted, pixel.emission_texel);
    }
    emitted = srgb_to_linear(scale(emitted, surface.emission[3]));

    Float3 normal = pixel.normal;
    if ((flags & kMeshSideCheck) != 0 && !pixel.front) {
        normal = scale(normal, -1.0F);
    }
    if ((flags & kMeshNormalMap) != 0) {
        Float3 mapped = pixel.normal_texel;
        mapped[0] = (mapped[0] * 2.0F) - 1.0F;
        mapped[1] = (mapped[1] * 2.0F) - 1.0F;
        mapped[2] =
            std::sqrt(std::max(0.0F, 1.0F - ((mapped[0] * mapped[0]) + (mapped[1] * mapped[1]))));
        const Float3 bent =
            add(add(scale(pixel.tangent, mapped[0]), scale(pixel.binormal, mapped[1])),
                scale(normal, mapped[2]));
        normal = normalize(lerp3(normal, bent, surface.surface[3]));
    } else {
        normal = normalize(normal);
    }
    const Float3 view =
        normalize(sub({scene.camera[0], scene.camera[1], scene.camera[2]}, pixel.world));
    const float dielectric = 0.16F * specular * specular;
    const Float3 f0 = lerp3({dielectric, dielectric, dielectric}, base, metallic);

    // Shadows offset the point they sample along the vertex normal, as Godot's normal_interp.
    const Float3 geometry = normalize(pixel.normal);
    LightSum light_sum;
    const auto count = static_cast<std::size_t>(scene.camera[3]);
    for (std::size_t i = 0; i < count && i < kMaxLights3D; ++i) {
        const MeshLightUniforms& light = scene.lights.at(i);
        if ((static_cast<std::uint32_t>(light.params[3]) & surface.options[2]) == 0) {
            continue;
        }
        const float type = light.direction[3];
        const Float3 light_color{light.color[0], light.color[1], light.color[2]};
        const float slot = light.shadow[0];
        if (type == 0.0F) {
            const Float3 toward =
                normalize({light.direction[0], light.direction[1], light.direction[2]});
            const float shadow = slot >= 0.0F && maps.uniforms != nullptr
                                     ? directional_shadow(maps, static_cast<std::size_t>(slot),
                                                          pixel.world, geometry, toward)
                                     : 1.0F;
            light_compute(normal, toward, view, light_color, shadow, f0, roughness, metallic,
                          light.color[3], light_sum);
            continue;
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
        light_compute(normal, normalize(to_light), view, light_color, attenuation, f0, roughness,
                      metallic, light.color[3], light_sum);
    }

    // Light from the sky's radiance, as Godot's scene.glsl takes it: reflections by roughness
    // and ambient light by the normal, then specular occlusion and Lazarov's environment BRDF.
    Float3 ambient_light{scene.ambient[0], scene.ambient[1], scene.ambient[2]};
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
        if (scene.sky[0] != 0.0F) {
            const Float4 texel = maps.radiance->sample(to_sky(normal), kRadianceMaxLevel);
            ambient_light =
                lerp3(ambient_light, scale(srgb_to_linear(xyz(texel)), scene.sky[2]), scene.sky[3]);
        }
    }
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
    for (std::size_t i = 0; i < 3; ++i) {
        reflected.at(i) *= occlusion * ((env_x * f0.at(i)) + (env_y * env_g));
    }

    ambient_light = scale(mul(ambient_light, base), 1.0F - metallic);
    const Float3 diffuse_light = mul(light_sum.diffuse, scale(base, 1.0F - metallic));
    const Float3 lit =
        add(add(add(add(diffuse_light, light_sum.specular), reflected), emitted), ambient_light);
    return finish(lit);
}

} // namespace slash::render::detail
