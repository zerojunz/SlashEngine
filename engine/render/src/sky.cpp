#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "sky_shading.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

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

} // namespace

bool sky_draws(const Scene3D& scene) {
    return scene.sky.kind != SkyKind::None;
}

TextureId sky_texture(const Sky3D& sky) {
    switch (sky.kind) {
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
                   post_processes(scene.post) ? 0.0F : 1.0F};
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

} // namespace slash::render::detail
