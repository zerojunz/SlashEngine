#pragma once

#include "checks.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

// What every device shares to draw a frame's sky (Sky3D, in mesh.hpp): the uniforms of
// shaders/sky.hlsl and the CPU twin of its fragment stage, which the software device draws with.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;
using Float3 = std::array<float, 3>;

// sky.hlsl's options.z.
inline constexpr std::uint32_t kSkyDeband = 1;
inline constexpr std::uint32_t kSkyTexture = 2;

struct SkyLightUniforms {
    Float4 direction_energy{}; // toward the light; its energy
    Float4 color_size{};       // sRGB; the cosine of its angular radius
    Float4 disk{};             // the cosines of a physical sky's sun disk's radius and half that
};

// The sky pass's uniforms: b0 in space3.
struct SkyUniforms {
    std::array<float, 16> orientation{}; // the camera's space to the sky's, column-major
    Float4 projection{};                 // the projection's [2][0], [0][0], [2][1] and [1][1]
    Float4 pixel{};     // one over the target's size, then the background's energy multiplier
    Float4 tonemap{};   // exposure, white, the tonemapper, 1 to tonemap (0 when post passes do)
    Float4 fog_color{}; // linear, times its energy; then 1 when fog is on
    Float4 fog{};       // aerial perspective, sun scatter, sky affect
    std::array<Float4, 6> params{}; // the material's, by kind
    std::array<SkyLightUniforms, kMaxSkyLights> lights{};
    std::array<std::uint32_t, 4> options{}; // the kind, how many lights, the flags above, 0
};
static_assert(sizeof(SkyUniforms) == 448);

// Whether a scene draws a sky behind its meshes.
[[nodiscard]] bool sky_draws(const Scene3D& scene);

// The texture the sky samples (its material's sky cover, night sky or panorama), or None.
[[nodiscard]] TextureId sky_texture(const Sky3D& sky);

// The sky pass's uniforms, for a target of width x height.
[[nodiscard]] SkyUniforms sky_uniforms(const Scene3D& scene, std::uint32_t width,
                                       std::uint32_t height);

// Where the pixel at x, y (its center, from the top left) looks: its direction in the sky's
// space, and the panorama coordinate it samples.
struct SkyRay {
    Float3 eye{};
    std::array<float, 2> coords{};
};
[[nodiscard]] SkyRay sky_ray(const SkyUniforms& uniforms, float x, float y);

// The CPU twin of sky.hlsl's fragment stage: the pixel at x, y's color, sRGB, from its ray and the
// texel the sky's texture has at the ray's coordinate.
[[nodiscard]] Float4 shade_sky(const SkyUniforms& uniforms, const SkyRay& ray, const Float4& texel,
                               float x, float y);

// Checks a scene's sky and fog: a kind that is one, finite numbers, at most kMaxSkyLights lights,
// and a texture the device has (known).
template <typename Known>
[[nodiscard]] Result<void> check_sky(const Scene3D& scene, const Known& known) {
    const Sky3D& sky = scene.sky;
    if (static_cast<std::size_t>(sky.kind) > static_cast<std::size_t>(SkyKind::Panorama)) {
        return make_error(ErrorCode::InvalidArgument, "a scene's sky kind is one of five");
    }
    if (sky.lights.size() > kMaxSkyLights) {
        return make_error(ErrorCode::InvalidArgument,
                          "a sky shows at most " + std::to_string(kMaxSkyLights) + " lights, not " +
                              std::to_string(sky.lights.size()));
    }
    const ProceduralSky& p = sky.procedural;
    const PhysicalSky& ph = sky.physical;
    const Fog3D& fog = scene.fog;
    for (const float value : {sky.energy,           sky.custom_fov,
                              p.sky_curve,          p.sky_energy_multiplier,
                              p.ground_curve,       p.ground_energy_multiplier,
                              p.sun_angle_max,      p.sun_curve,
                              p.energy_multiplier,  ph.rayleigh_coefficient,
                              ph.mie_coefficient,   ph.mie_eccentricity,
                              ph.turbidity,         ph.sun_disk_scale,
                              ph.energy_multiplier, sky.panorama.energy_multiplier,
                              fog.light_energy,     fog.sun_scatter,
                              fog.density,          fog.aerial_perspective,
                              fog.sky_affect,       fog.height,
                              fog.height_density,   fog.depth_curve,
                              fog.depth_begin,      fog.depth_end}) {
        if (!std::isfinite(value)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a scene's sky and fog numbers are finite");
        }
    }
    if (const TextureId texture = sky_texture(sky); texture != TextureId::None && !known(texture)) {
        return unknown_texture(texture);
    }
    return {};
}

} // namespace slash::render::detail
