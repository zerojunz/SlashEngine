#include "forward_post.hpp"
#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "sky_shading.hpp"

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

namespace slash::render::detail {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegrees = std::numbers::pi_v<float> / 180.0F;

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

float saturate(float x) {
    return std::clamp(x, 0.0F, 1.0F);
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}

Float3 scale(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 rgb(const Float4& v) {
    return {v[0], v[1], v[2]};
}

Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 cross3(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

Float3 normalize3(const Float3& v) {
    return scale(v, 1.0F / std::sqrt(dot3(v, v)));
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0F - (2.0F * t));
}

float fract(float x) {
    return x - std::floor(x);
}

// Eberly's approximations, as sky.hlsl's.
float acos_approx(float p_x) {
    const float x = std::abs(p_x);
    float res = (-0.156583F * x) + (kPi / 2.0F);
    res *= std::sqrt(1.0F - x);
    return p_x >= 0.0F ? res : kPi - res;
}

float atan2_approx(float y, float x) {
    const float a = std::min(std::abs(x), std::abs(y)) / std::max(std::abs(x), std::abs(y));
    const float s = a * a;
    float poly = 0.0872929F;
    poly = -0.301895F + (poly * s);
    poly = 1.0F + (poly * s);
    poly = poly * a;
    float r = std::abs(y) > std::abs(x) ? (kPi / 2.0F) - poly : poly;
    r = x < 0.0F ? kPi - r : r;
    r = y < 0.0F ? -r : r;
    return r;
}

// The 3x3 inverse of a column-major matrix's upper left, column-major in a 4x4.
std::array<float, 16> inverse3(const std::array<float, 16>& m) {
    const auto at = [&](std::size_t row, std::size_t column) { return m.at((column * 4) + row); };
    const float a = at(0, 0);
    const float b = at(0, 1);
    const float c = at(0, 2);
    const float d = at(1, 0);
    const float e = at(1, 1);
    const float f = at(1, 2);
    const float g = at(2, 0);
    const float h = at(2, 1);
    const float i = at(2, 2);
    const float co00 = (e * i) - (f * h);
    const float co01 = (f * g) - (d * i);
    const float co02 = (d * h) - (e * g);
    const float determinant = (a * co00) + (b * co01) + (c * co02);
    const float s = determinant != 0.0F ? 1.0F / determinant : 0.0F;
    std::array<float, 16> out{};
    const auto put = [&](std::size_t row, std::size_t column, float value) {
        out.at((column * 4) + row) = value * s;
    };
    put(0, 0, co00);
    put(0, 1, (c * h) - (b * i));
    put(0, 2, (b * f) - (c * e));
    put(1, 0, co01);
    put(1, 1, (a * i) - (c * g));
    put(1, 2, (c * d) - (a * f));
    put(2, 0, co02);
    put(2, 1, (b * g) - (a * h));
    put(2, 2, (a * e) - (b * d));
    out[15] = 1.0F;
    return out;
}

// a times b's upper left 3x3s, column-major.
std::array<float, 16> multiply3(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> out{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 3; ++k) {
                sum += a.at((k * 4) + row) * b.at((column * 4) + k);
            }
            out.at((column * 4) + row) = sum;
        }
    }
    out[15] = 1.0F;
    return out;
}

Float3 times3(const std::array<float, 16>& m, const Float3& v) {
    Float3 out{};
    for (std::size_t row = 0; row < 3; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]);
    }
    return out;
}

Float3 procedural_sky(const SkyUniforms& u, const Float3& eye, const Float4& cover) {
    const auto& p = u.params;
    const float v_angle = std::clamp(eye[1], -1.0F, 1.0F);
    Float3 sky = lerp3(rgb(p[0]), rgb(p[1]), saturate(power(1.0F - v_angle, p[0][3])));
    const float sun_angle_max = p[2][3];
    const float inv_sun_curve = p[3][3];
    for (std::uint32_t i = 0; i < u.options[1]; ++i) {
        const SkyLightUniforms& light = u.lights.at(i);
        const Float3 sun = scale(rgb(light.color_size), light.direction_energy[3]);
        const float sun_angle = dot3(xyz(light.direction_energy), eye);
        const float sun_size = light.color_size[3];
        if (sun_angle > sun_size) {
            sky = sun;
        } else if (sun_angle > sun_angle_max) {
            const float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
            sky = lerp3(sky, sun, saturate(power(1.0F - c2, inv_sun_curve)));
        }
    }
    if ((u.options[2] & kSkyTexture) != 0) {
        for (std::size_t i = 0; i < 3; ++i) {
            sky.at(i) += (cover.at(i) * p[4].at(i)) * cover[3] * p[4][3];
        }
    }
    const Float3 ground = lerp3(rgb(p[2]), rgb(p[3]), saturate(power(1.0F + v_angle, p[1][3])));
    return scale(eye[1] >= 0.0F ? sky : ground, p[5][0]);
}

float henyey_greenstein(float cos_theta, float g) {
    constexpr float kK = 0.0795774715459F;
    return kK * (1.0F - (g * g)) / power(1.0F + (g * g) - (2.0F * g * cos_theta), 1.5F);
}

Float3 physical_sky(const SkyUniforms& u, const Float3& eye, const Float4& night) {
    const auto& p = u.params;
    const bool use_night = (u.options[2] & kSkyTexture) != 0;
    const float exposure = p[3][2];
    if (u.options[1] == 0) {
        return use_night ? scale(rgb(night), exposure) : Float3{};
    }
    const SkyLightUniforms& light = u.lights[0];
    const Float3 toward = xyz(light.direction_energy);
    const float zenith_angle = std::clamp(toward[1], -1.0F, 1.0F);
    const float sun_energy = std::max(0.0F, 0.757F * zenith_angle) * light.direction_energy[3];
    const float sun_fade = 1.0F - saturate(1.0F - slash::exp(toward[1]));

    const float rayleigh_coefficient = p[0][3] - (1.0F * (1.0F - sun_fade));
    const Float3 rayleigh_beta = scale(rgb(p[0]), rayleigh_coefficient * 0.0001F);
    const Float3 mie_beta = scale(rgb(p[1]), p[3][0] * p[1][3] * 0.000434F);

    const float zenith = std::max(0.0F, eye[1]);
    const float optical_mass =
        1.0F / (zenith + (0.15F * power(3.885F + (54.5F * zenith), -1.253F)));
    const float rayleigh_scatter = 8.4e3F * optical_mass;
    const float mie_scatter = 1.25e3F * optical_mass;

    const float cos_theta = dot3(eye, toward);
    const float rayleigh_phase =
        (3.0F / (16.0F * kPi)) * (1.0F + power((cos_theta * 0.5F) + 0.5F, 2.0F));
    const float mie_phase = henyey_greenstein(cos_theta, p[2][3]);
    const float horizon_mix = saturate(power(1.0F - zenith_angle, 5.0F));
    const float ground_mix = smoothstep(-0.1F, 0.1F, eye[1]);
    const float sundisk = smoothstep(light.disk[0], light.disk[1], cos_theta);
    const float fade_power = 1.0F / (1.2F + (1.2F * sun_fade));
    Float3 color{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float extinction = slash::exp(
            -((rayleigh_beta.at(i) * rayleigh_scatter) + (mie_beta.at(i) * mie_scatter)));
        const float beta_r_theta = rayleigh_beta.at(i) * rayleigh_phase;
        const float beta_m_theta = mie_beta.at(i) * mie_phase;
        const float scattered =
            sun_energy * ((beta_r_theta + beta_m_theta) / (rayleigh_beta.at(i) + mie_beta.at(i)));
        float lin = power(scattered * (1.0F - extinction), 1.5F);
        lin *= lerp(1.0F, power(scattered * extinction, 0.5F), horizon_mix);
        lin *= lerp(p[2].at(i), 1.0F, ground_mix);
        float l0 = (sun_energy * extinction) * sundisk * light.color_size.at(i);
        if (use_night) {
            l0 += night.at(i) * extinction;
        }
        color.at(i) = power(lin + l0, fade_power) * exposure;
    }
    return color;
}

// A cube face's own axis (n), and the axes its s and t run along (a and b), as cube_direction
// takes them: the table every API selects cube faces by.
struct CubeFace {
    Float3 n;
    Float3 a;
    Float3 b;
};
constexpr std::array<CubeFace, kCubeFaces> kCubeFaceAxes{{
    {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F}},
    {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, -1.0F, 0.0F}},
    {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
    {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
    {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
    {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
}};

// A cube face's texel a direction falls in, and where in it: s and t from 0 to 1 across the face.
struct CubeCoord {
    std::uint32_t face = 0;
    float s = 0.0F;
    float t = 0.0F;
};
CubeCoord cube_coord(const Float3& d) {
    const float x = std::abs(d[0]);
    const float y = std::abs(d[1]);
    const float z = std::abs(d[2]);
    std::uint32_t face = 0;
    float major = 0.0F;
    if (x >= y && x >= z) {
        face = d[0] >= 0.0F ? 0 : 1;
        major = x;
    } else if (y >= z) {
        face = d[1] >= 0.0F ? 2 : 3;
        major = y;
    } else {
        face = d[2] >= 0.0F ? 4 : 5;
        major = z;
    }
    const CubeFace& axes = kCubeFaceAxes.at(face);
    const float inverse = major > 0.0F ? 1.0F / major : 0.0F;
    return {.face = face,
            .s = ((dot3(d, axes.a) * inverse) + 1.0F) * 0.5F,
            .t = ((dot3(d, axes.b) * inverse) + 1.0F) * 0.5F};
}

// A color as RGB10A2 holds it.
Float4 quantized(const Float4& color) {
    Float4 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = std::floor((saturate(color.at(i)) * kRadianceSteps) + 0.5F) / kRadianceSteps;
    }
    out[3] = 1.0F;
    return out;
}

// A color as RGBA16F holds it.
Float4 half_texel(const Float4& color) {
    return {round_half(color[0]), round_half(color[1]), round_half(color[2]), 1.0F};
}

// Godot's helpers for IBL filtering, in effects/cubemap_filter.cpp.
float radical_inverse_vdc(std::uint32_t bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Float3 importance_sample_ggx(float xi_x, float xi_y, float roughness4) {
    const float phi = 2.0F * kPi * xi_x;
    const float cos_theta = std::sqrt((1.0F - xi_y) / (1.0F + ((roughness4 - 1.0F) * xi_y)));
    const float sin_theta = std::sqrt(1.0F - (cos_theta * cos_theta));
    const slash::Vec2 turn = slash::cos_sin(phi);
    return {sin_theta * turn.x, sin_theta * turn.y, cos_theta};
}

float distribution_ggx(float n_dot_h, float roughness4) {
    const float n_dot_h2 = n_dot_h * n_dot_h;
    float denominator = (n_dot_h2 * (roughness4 - 1.0F)) + 1.0F;
    denominator = kPi * denominator * denominator;
    return roughness4 / denominator;
}

TextureId material_texture(const Sky3D& sky, SkyKind kind) {
    switch (kind) {
    case SkyKind::Procedural:
        return sky.procedural.sky_cover;
    case SkyKind::Physical:
        return sky.physical.night_sky;
    case SkyKind::Panorama:
        return sky.panorama.panorama;
    default:
        return TextureId::None;
    }
}

} // namespace

bool sky_draws(const Scene3D& scene) {
    return scene.sky.kind != SkyKind::None;
}

TextureId sky_texture(const Sky3D& sky) {
    return material_texture(sky, sky.kind);
}

SkyUniforms sky_uniforms(const Scene3D& scene, std::uint32_t width, std::uint32_t height) {
    const Sky3D& sky = scene.sky;
    SkyUniforms out;
    out.orientation = multiply3(sky.orientation, inverse3(scene.camera));
    std::array<float, 16> projection = scene.projection;
    if (sky.custom_fov > 0.0F) {
        // Godot's Projection::get_aspect, then a perspective of the custom field of view; its
        // near and far planes don't change the entries the sky reads.
        projection = perspective(sky.custom_fov, projection[5] / projection[0], 0.05F, 4000.0F);
    }
    out.projection = {projection[8], projection[0], projection[9], projection[5]};
    out.pixel = {1.0F / static_cast<float>(std::max(width, 1U)),
                 1.0F / static_cast<float>(std::max(height, 1U)), sky.energy, 0.0F};
    out.tonemap = {scene.post.exposure, scene.post.white, static_cast<float>(scene.post.tonemapper),
                   color_stage(scene)};
    const Fog3D& fog = scene.fog;
    out.fog_color = {srgb_to_linear_exact(fog.light_color[0]) * fog.light_energy,
                     srgb_to_linear_exact(fog.light_color[1]) * fog.light_energy,
                     srgb_to_linear_exact(fog.light_color[2]) * fog.light_energy,
                     fog.enabled ? 1.0F : 0.0F};
    out.fog = {fog.aerial_perspective, fog.sun_scatter, fog.sky_affect, 0.0F};

    const std::size_t count = std::min(sky.lights.size(), kMaxSkyLights);
    for (std::size_t i = 0; i < count; ++i) {
        const SkyLight& light = sky.lights[i];
        Float3 toward = {-light.direction[0], -light.direction[1], -light.direction[2]};
        if (dot3(toward, toward) > 0.0F) {
            toward = normalize3(toward);
        }
        // The angular distance in radians, which the sky takes as the disk's radius, as Godot's.
        const float size = light.angular_distance * kDegrees;
        const float disk = size * sky.physical.sun_disk_scale;
        SkyLightUniforms& packed = out.lights.at(i);
        packed.direction_energy = {toward[0], toward[1], toward[2], light.energy};
        packed.color_size = {light.color[0], light.color[1], light.color[2],
                             slash::cos_sin(size).x};
        packed.disk = {slash::cos_sin(disk).x, slash::cos_sin(disk * 0.5F).x, 0.0F, 0.0F};
    }

    std::uint32_t flags = sky_texture(sky) != TextureId::None ? kSkyTexture : 0;
    auto& p = out.params;
    switch (sky.kind) {
    case SkyKind::Color:
        p[0] = {sky.color[0], sky.color[1], sky.color[2], 0.0F};
        break;
    case SkyKind::Procedural: {
        // As ProceduralSkyMaterial's setters turn its properties into its shader's uniforms.
        const ProceduralSky& m = sky.procedural;
        const float sky_energy = m.sky_energy_multiplier;
        const float ground_energy = m.ground_energy_multiplier;
        p[0] = {m.sky_top_color[0] * sky_energy, m.sky_top_color[1] * sky_energy,
                m.sky_top_color[2] * sky_energy, 0.6F / m.sky_curve};
        p[1] = {m.sky_horizon_color[0] * sky_energy, m.sky_horizon_color[1] * sky_energy,
                m.sky_horizon_color[2] * sky_energy, 0.6F / m.ground_curve};
        p[2] = {m.ground_bottom_color[0] * ground_energy, m.ground_bottom_color[1] * ground_energy,
                m.ground_bottom_color[2] * ground_energy,
                slash::cos_sin(m.sun_angle_max * kDegrees).x};
        p[3] = {m.ground_horizon_color[0] * ground_energy,
                m.ground_horizon_color[1] * ground_energy,
                m.ground_horizon_color[2] * ground_energy, 1.6F / power(m.sun_curve, 1.4F)};
        p[4] = {m.sky_cover_modulate[0], m.sky_cover_modulate[1], m.sky_cover_modulate[2],
                m.sky_cover_modulate[3] * sky_energy};
        p[5] = {m.energy_multiplier, 0.0F, 0.0F, 0.0F};
        if (m.use_debanding) {
            flags |= kSkyDeband;
        }
        break;
    }
    case SkyKind::Physical: {
        const PhysicalSky& m = sky.physical;
        p[0] = {m.rayleigh_color[0], m.rayleigh_color[1], m.rayleigh_color[2],
                m.rayleigh_coefficient};
        p[1] = {m.mie_color[0], m.mie_color[1], m.mie_color[2], m.mie_coefficient};
        p[2] = {m.ground_color[0], m.ground_color[1], m.ground_color[2], m.mie_eccentricity};
        p[3] = {m.turbidity, m.sun_disk_scale, m.energy_multiplier, 0.0F};
        if (m.use_debanding) {
            flags |= kSkyDeband;
        }
        break;
    }
    case SkyKind::Panorama:
        p[0] = {sky.panorama.energy_multiplier, 0.0F, 0.0F, 0.0F};
        break;
    default:
        break;
    }
    out.options = {static_cast<std::uint32_t>(sky.kind), static_cast<std::uint32_t>(count), flags,
                   0};
    return out;
}

SkyRay sky_ray(const SkyUniforms& uniforms, float x, float y) {
    const float u = x * uniforms.pixel[0];
    const float v = y * uniforms.pixel[1];
    const float ndc_x = (u * 2.0F) - 1.0F;
    const float ndc_y = 1.0F - (v * 2.0F);
    const Float4& p = uniforms.projection;
    const Float3 eye = normalize3(
        times3(uniforms.orientation, {(ndc_x + p[0]) / p[1], (ndc_y + p[2]) / p[3], -1.0F}));
    std::array<float, 2> coords{atan2_approx(eye[0], -eye[2]), acos_approx(eye[1])};
    if (coords[0] < 0.0F) {
        coords[0] += kPi * 2.0F;
    }
    coords[0] /= kPi * 2.0F;
    coords[1] /= kPi;
    return {.eye = eye, .coords = coords};
}

Float4 shade_sky(const SkyUniforms& uniforms, const SkyRay& ray, const Float4& texel, float x,
                 float y) {
    const Float3& eye = ray.eye;
    Float3 color{};
    switch (static_cast<SkyKind>(uniforms.options[0])) {
    case SkyKind::Color:
        color = rgb(uniforms.params[0]);
        break;
    case SkyKind::Procedural:
        color = procedural_sky(uniforms, eye, texel);
        break;
    case SkyKind::Physical:
        color = physical_sky(uniforms, eye, texel);
        break;
    case SkyKind::Panorama:
        if ((uniforms.options[2] & kSkyTexture) != 0) {
            color = scale(rgb(texel), uniforms.params[0][0]);
        }
        break;
    default:
        break;
    }
    color = srgb_to_linear(scale(color, uniforms.pixel[2]));

    if (uniforms.fog_color[3] != 0.0F) {
        const Float4& fog = uniforms.fog;
        Float3 fogged = lerp3(rgb(uniforms.fog_color), color, fog[0]);
        if (fog[1] > 0.001F) {
            for (std::uint32_t i = 0; i < uniforms.options[1]; ++i) {
                const SkyLightUniforms& light = uniforms.lights.at(i);
                const Float3 light_color =
                    scale(srgb_to_linear(rgb(light.color_size)), light.direction_energy[3]);
                const float amount =
                    power(std::max(dot3(eye, xyz(light.direction_energy)), 0.0F), 8.0F) * kPi;
                fogged = add(fogged, scale(light_color, amount * fog[1]));
            }
        }
        color = lerp3(color, fogged, fog[2]);
    }

    if (uniforms.tonemap[3] == 2.0F) {
        return {color[0], color[1], color[2], 1.0F};
    }
    color = scale(color, uniforms.tonemap[0]);
    if (uniforms.tonemap[3] != 0.0F) {
        color = tonemap(color, uniforms.tonemap[1],
                        static_cast<Tonemapper>(static_cast<std::uint32_t>(uniforms.tonemap[2])));
    }
    color = linear_to_srgb(color);
    if ((uniforms.options[2] & kSkyDeband) != 0) {
        const float res =
            (fract(52.9829189F * fract((x * 0.06711056F) + (y * 0.00583715F))) * 2.0F) - 1.0F;
        const float noise = res / 255.0F * uniforms.pixel[2];
        color = {color[0] + noise, color[1] - noise, color[2] + noise};
    }
    return {color[0], color[1], color[2], 1.0F};
}

std::uint32_t radiance_levels(std::uint32_t size) {
    std::uint32_t levels = 1;
    while ((size >> levels) >= 4) {
        ++levels;
    }
    return levels;
}

Float3 cube_direction(std::uint32_t face, float s, float t) {
    const CubeFace& axes = kCubeFaceAxes.at(face);
    return add(add(axes.n, scale(axes.a, s)), scale(axes.b, t));
}

bool radiance_used(const Scene3D& scene) {
    const Radiance3D& radiance = scene.radiance;
    return radiance.kind != SkyKind::None && (radiance.ambient || radiance.reflections);
}

TextureId radiance_texture(const Scene3D& scene) {
    return material_texture(scene.sky, scene.radiance.kind);
}

SkyUniforms radiance_face_uniforms(const Scene3D& scene, std::uint32_t face) {
    const std::uint32_t size = scene.radiance.size;
    Scene3D cube;
    cube.sky = scene.sky;
    cube.sky.kind = scene.radiance.kind;
    cube.sky.custom_fov = 0.0F;
    cube.post = scene.post;
    SkyUniforms out = sky_uniforms(cube, size, size);
    // A pixel's direction is (s, -t, -1) from its place across the face, -1 to 1; the face's
    // matrix turns that into its own s and t axes and out along its axis.
    const CubeFace& axes = kCubeFaceAxes.at(face);
    out.orientation = {axes.a[0],  axes.a[1], axes.a[2],  0.0F,       -axes.b[0], -axes.b[1],
                       -axes.b[2], 0.0F,      -axes.n[0], -axes.n[1], -axes.n[2], 0.0F,
                       0.0F,       0.0F,      0.0F,       1.0F};
    out.projection = {0.0F, 1.0F, 0.0F, 1.0F};
    out.tonemap[3] = 0.0F;
    out.fog_color[3] = 0.0F;
    return out;
}

RadianceUniforms radiance_uniforms(std::uint32_t size, std::uint32_t levels, std::uint32_t level) {
    RadianceUniforms out;
    const std::uint32_t level_size = std::max(size >> level, 1U);
    out.params = {0.0F, 1.0F / static_cast<float>(level_size), 0.0F, 0.0F};
    if (level == 0) {
        return out;
    }
    constexpr std::array<std::uint32_t, 5> kSampleCounts{
        1, kRadianceSamples / 16, kRadianceSamples / 8, kRadianceSamples / 4, kRadianceSamples};
    const std::uint32_t count = kSampleCounts.at(std::min(level, 4U));
    float roughness = static_cast<float>(level) / static_cast<float>(std::max(levels, 2U) - 1);
    roughness *= roughness; // non-perceptual
    float roughness4 = roughness * roughness;
    roughness4 *= roughness4;
    const float solid_angle_texel =
        4.0F * kPi / (6.0F * static_cast<float>(level_size) * static_cast<float>(level_size));
    const float base_level = static_cast<float>(std::max(1, static_cast<int>(level) - 3));

    std::uint32_t index = 0;
    float weight = 0.0F;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Float3 dir = importance_sample_ggx(static_cast<float>(i) / static_cast<float>(count),
                                                 radical_inverse_vdc(i), roughness4);
        const Float3 light = {2.0F * dir[2] * dir[0], 2.0F * dir[2] * dir[1],
                              (2.0F * dir[2] * dir[2]) - 1.0F};
        if (light[2] <= 0.0F) {
            continue;
        }
        const float d = distribution_ggx(dir[2], roughness4);
        const float pdf = (d * dir[2] / (4.0F * dir[2])) + 0.0001F;
        const float solid_angle_sample = 1.0F / ((static_cast<float>(count) * pdf) + 0.0001F);
        const float mip = std::max((0.5F * slash::ln(solid_angle_sample / solid_angle_texel) /
                                    std::numbers::ln2_v<float>)+base_level,
                                   1.0F);
        out.samples.at(index) = {light[0], light[1], light[2], mip};
        weight += light[2];
        ++index;
    }
    out.params[2] = weight;
    out.params[3] = static_cast<float>(index);
    return out;
}

bool RadianceKey::operator==(const RadianceKey& other) const {
    return texture == other.texture && size == other.size &&
           std::ranges::equal(std::as_bytes(std::span(&face, 1)),
                              std::as_bytes(std::span(&other.face, 1)));
}

RadianceKey radiance_key(const Scene3D& scene) {
    return {.face = radiance_face_uniforms(scene, 0),
            .texture = radiance_texture(scene),
            .size = scene.radiance.size};
}

const Float4& CubeImage::texel(std::uint32_t level, std::uint32_t face, std::uint32_t x,
                               std::uint32_t y) const {
    const std::uint32_t n = std::max(size >> level, 1U);
    return levels.at(level).at((((std::size_t{face} * n) + y) * n) + x);
}

Float4 CubeImage::sample(const Float3& direction, float level) const {
    const auto top = static_cast<float>(levels.size() - 1);
    const float clamped = std::clamp(level, 0.0F, top);
    const auto bilinear = [&](std::uint32_t at) {
        const std::uint32_t n = std::max(size >> at, 1U);
        const auto texels = static_cast<float>(n);
        // A texel of the face or, past its edges, the one of the next face that its center's
        // direction falls in, as cube textures filter across faces.
        const auto fetch = [&](std::uint32_t face, int x, int y) -> const Float4& {
            if (x >= 0 && y >= 0 && x < static_cast<int>(n) && y < static_cast<int>(n)) {
                return texel(at, face, static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y));
            }
            const CubeCoord next = cube_coord(
                cube_direction(face, (2.0F * (static_cast<float>(x) + 0.5F) / texels) - 1.0F,
                               (2.0F * (static_cast<float>(y) + 0.5F) / texels) - 1.0F));
            const auto place = [&](float s) {
                return std::min(static_cast<std::uint32_t>(std::max(s * texels, 0.0F)), n - 1);
            };
            return texel(at, next.face, place(next.s), place(next.t));
        };
        const CubeCoord coord = cube_coord(direction);
        const float u = (coord.s * texels) - 0.5F;
        const float v = (coord.t * texels) - 0.5F;
        const float x0 = std::floor(u);
        const float y0 = std::floor(v);
        const float fx = u - x0;
        const float fy = v - y0;
        const int x = static_cast<int>(x0);
        const int y = static_cast<int>(y0);
        const Float4& a = fetch(coord.face, x, y);
        const Float4& b = fetch(coord.face, x + 1, y);
        const Float4& c = fetch(coord.face, x, y + 1);
        const Float4& d = fetch(coord.face, x + 1, y + 1);
        Float4 out{};
        for (std::size_t i = 0; i < 4; ++i) {
            out.at(i) = lerp(lerp(a.at(i), b.at(i), fx), lerp(c.at(i), d.at(i), fx), fy);
        }
        return out;
    };
    const auto low = static_cast<std::uint32_t>(clamped);
    const float blend = clamped - static_cast<float>(low);
    const Float4 first = bilinear(low);
    if (blend <= 0.0F) {
        return first;
    }
    const Float4 second = bilinear(low + 1);
    Float4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out.at(i) = lerp(first.at(i), second.at(i), blend);
    }
    return out;
}

CubeImage make_radiance(const Scene3D& scene,
                        const std::function<Float4(const std::array<float, 2>&)>& texel) {
    const std::uint32_t size = scene.radiance.size;
    const std::uint32_t levels = radiance_levels(size);
    const auto empty = [&] {
        CubeImage cube{.size = size, .levels = {}};
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::size_t n = size >> level;
            cube.levels.emplace_back(kCubeFaces * n * n);
        }
        return cube;
    };

    // The sky into the raw cube's faces, then its mipmaps and the filtered levels.
    CubeImage raw = empty();
    for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
        const SkyUniforms uniforms = radiance_face_uniforms(scene, face);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const SkyRay ray = sky_ray(uniforms, px, py);
                raw.levels[0][(((std::size_t{face} * size) + y) * size) + x] =
                    quantized(shade_sky(uniforms, ray, texel(ray.coords), px, py));
            }
        }
    }
    cube_mipmaps(raw, false);
    return filter_cube(raw, false);
}

void cube_mipmaps(CubeImage& cube, bool linear) {
    const std::uint32_t size = cube.size;
    for (std::uint32_t level = 1; level < cube.levels.size(); ++level) {
        const std::uint32_t n = size >> level;
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            for (std::uint32_t y = 0; y < n; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    Float4 sum{};
                    for (const auto& [dx, dy] : {std::pair{0U, 0U}, std::pair{1U, 0U},
                                                 std::pair{0U, 1U}, std::pair{1U, 1U}}) {
                        const Float4& t = cube.texel(level - 1, face, (x * 2) + dx, (y * 2) + dy);
                        for (std::size_t i = 0; i < 4; ++i) {
                            sum.at(i) += t.at(i) * 0.25F;
                        }
                    }
                    cube.levels[level][(((std::size_t{face} * n) + y) * n) + x] =
                        linear ? half_texel(sum) : quantized(sum);
                }
            }
        }
    }
}

CubeImage filter_cube(const CubeImage& raw, bool linear) {
    // radiance.hlsl's twin: level 0 as it is, then each next level blurred from the raw cube.
    const std::uint32_t size = raw.size;
    const auto levels = static_cast<std::uint32_t>(raw.levels.size());
    CubeImage out{.size = size, .levels = {}};
    out.levels.push_back(raw.levels[0]);
    for (std::uint32_t level = 1; level < levels; ++level) {
        const RadianceUniforms uniforms = radiance_uniforms(size, levels, level);
        const std::uint32_t n = size >> level;
        out.levels.emplace_back(std::size_t{kCubeFaces} * n * n);
        const auto count = static_cast<std::uint32_t>(uniforms.params[3]);
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            for (std::uint32_t y = 0; y < n; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    const float s =
                        ((static_cast<float>(x) + 0.5F) * uniforms.params[1] * 2.0F) - 1.0F;
                    const float t =
                        ((static_cast<float>(y) + 0.5F) * uniforms.params[1] * 2.0F) - 1.0F;
                    const Float3 normal = normalize3(cube_direction(face, s, t));
                    const Float3 up = std::abs(normal[2]) < 0.999F ? Float3{0.0F, 0.0F, 1.0F}
                                                                   : Float3{1.0F, 0.0F, 0.0F};
                    const Float3 tangent = normalize3(cross3(up, normal));
                    const Float3 bitangent = cross3(normal, tangent);
                    Float3 sum{};
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const Float4& sample = uniforms.samples.at(i);
                        const Float3 l =
                            add(add(scale(tangent, sample[0]), scale(bitangent, sample[1])),
                                scale(normal, sample[2]));
                        const Float3 texel = rgb(raw.sample(l, sample[3]));
                        const Float3 value = linear ? texel : srgb_to_linear(texel);
                        sum = add(sum, scale(value, sample[2]));
                    }
                    const Float3 mean = scale(sum, 1.0F / uniforms.params[2]);
                    const Float3 color = linear ? mean : linear_to_srgb(mean);
                    const Float4 texel{color[0], color[1], color[2], 1.0F};
                    out.levels[level][(((std::size_t{face} * n) + y) * n) + x] =
                        linear ? half_texel(texel) : quantized(texel);
                }
            }
        }
    }
    return out;
}

} // namespace slash::render::detail

namespace slash::render {

HdrImage ambient_panorama(
    const Scene3D& scene, std::uint32_t width, std::uint32_t height,
    const std::function<std::array<float, 4>(const std::array<float, 2>&)>& sky_texel) {
    using detail::Float3;
    using detail::Float4;
    HdrImage out{.width = width, .height = height, .rgb = {}};
    out.rgb.resize(std::size_t{width} * height * 3);
    const float energy = scene.ambient_energy;
    const Float3 ambient{detail::srgb_to_linear_exact(scene.ambient[0]) * energy,
                         detail::srgb_to_linear_exact(scene.ambient[1]) * energy,
                         detail::srgb_to_linear_exact(scene.ambient[2]) * energy};
    const bool sky = detail::radiance_used(scene) && scene.radiance.ambient;
    const detail::CubeImage cube =
        sky ? detail::make_radiance(scene, sky_texel) : detail::CubeImage{};
    const std::array<float, 16>& o = scene.sky.orientation;
    for (std::uint32_t y = 0; y < height; ++y) {
        const float polar =
            std::numbers::pi_v<float> * (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
        const Vec2 polar_cs = cos_sin(polar);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float around = 2.0F * std::numbers::pi_v<float> * (static_cast<float>(x) + 0.5F) /
                                 static_cast<float>(width);
            const Vec2 around_cs = cos_sin(around);
            const Float3 d{polar_cs.y * around_cs.y, polar_cs.x, polar_cs.y * around_cs.x};
            Float3 light = ambient;
            if (sky) {
                // Into the sky's space, as the devices turn a normal (sky_rows).
                const Float3 turned{(o[0] * d[0]) + (o[4] * d[1]) + (o[8] * d[2]),
                                    (o[1] * d[0]) + (o[5] * d[1]) + (o[9] * d[2]),
                                    (o[2] * d[0]) + (o[6] * d[1]) + (o[10] * d[2])};
                const Float4 texel = cube.sample(turned, detail::kRadianceMaxLevel);
                light = detail::lerp3(
                    light,
                    detail::scale(detail::srgb_to_linear(detail::rgb(texel)), scene.sky.energy),
                    scene.radiance.ambient_mix);
            }
            float* at = &out.rgb[((std::size_t{y} * width) + x) * 3];
            at[0] = light[0];
            at[1] = light[1];
            at[2] = light[2];
        }
    }
    return out;
}

} // namespace slash::render
