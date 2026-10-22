#pragma once

#include "checks.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// What every device shares to draw a frame's sky (Sky3D, in mesh.hpp) and the radiance meshes are
// lit by (Radiance3D): the uniforms of shaders/sky.hlsl and shaders/radiance.hlsl, and the CPU
// twins of their fragment stages, which the software device draws with.
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

// A cube texture's faces, in the order every API holds them: +x, -x, +y, -y, +z, -z.
inline constexpr std::uint32_t kCubeFaces = 6;

// The radiance is RGB10A2, as Godot's: 10 bits a color channel.
inline constexpr float kRadianceSteps = 1023.0F;

// The level Godot's scene shader takes ambient light from, and the reflections of the roughest
// surfaces (RADIANCE_MAX_LOD).
inline constexpr float kRadianceMaxLevel = 5.0F;

// The samples the roughest levels are blurred with, as Godot's Compatibility renderer: four times
// rendering/reflections/sky_reflections/ggx_samples, 32 by default.
inline constexpr std::uint32_t kRadianceSamples = 128;

// How many levels a radiance cube size texels across has, as Godot's mipmap_count: down to faces
// 4 texels across.
[[nodiscard]] std::uint32_t radiance_levels(std::uint32_t size);

// The direction a point of a cube face looks along, unnormalized: s and t from -1 to 1 across the
// face, t from its first row to its last, as every API's cube textures lay faces out.
[[nodiscard]] Float3 cube_direction(std::uint32_t face, float s, float t);

// Whether a scene's meshes are lit by its radiance.
[[nodiscard]] bool radiance_used(const Scene3D& scene);

// The sky pass's uniforms to draw a face of the radiance's raw cube: the material looking out
// of the face, exposed but neither fogged nor tonemapped, as Godot's cubemap pass draws it.
[[nodiscard]] SkyUniforms radiance_face_uniforms(const Scene3D& scene, std::uint32_t face);

// The texture the raw cube's sky samples, or None.
[[nodiscard]] TextureId radiance_texture(const Scene3D& scene);

// The filter pass's uniforms: b0 in space3 of shaders/radiance.hlsl.
struct RadianceUniforms {
    // The face, one over the level's size, the samples' weight, and how many samples (0 copies
    // the source's level params[2], counted from the level its view starts at).
    Float4 params{};
    // GGX importance samples around +z: a direction, then the source level it is sampled at.
    std::array<Float4, kRadianceSamples> samples{};
};
static_assert(sizeof(RadianceUniforms) == 16 + (16 * kRadianceSamples));

// The filter pass's uniforms for a level of a radiance of size and levels, as Godot's
// CubemapFilter::filter_radiance: level 0 copies the raw cube's, and each next one blurs it for a
// rougher surface. The face is params[0]'s, for the device to set.
[[nodiscard]] RadianceUniforms radiance_uniforms(std::uint32_t size, std::uint32_t levels,
                                                 std::uint32_t level);

// Whether a radiance must be drawn again: what its raw cube was drawn from, which devices keep and
// compare with the next frame's.
struct RadianceKey {
    SkyUniforms face{};
    TextureId texture = TextureId::None;
    std::uint32_t size = 0;

    [[nodiscard]] bool operator==(const RadianceKey& other) const;
};
[[nodiscard]] RadianceKey radiance_key(const Scene3D& scene);

// A cube texture on the CPU: each level's six faces, each face's texels in rows from its first,
// rounded to RGB10A2 as GPUs store them.
struct CubeImage {
    std::uint32_t size = 0;
    std::vector<std::vector<Float4>> levels; // level i: kCubeFaces faces of (size >> i)² texels

    [[nodiscard]] const Float4& texel(std::uint32_t level, std::uint32_t face, std::uint32_t x,
                                      std::uint32_t y) const;
    // SampleLevel with a linear sampler that filters across faces' edges, as every API's cube
    // textures do, between the two levels around level.
    [[nodiscard]] Float4 sample(const Float3& direction, float level) const;
};

// A scene's radiance on the CPU, drawn as the GPU devices draw it: the sky into the raw cube's
// faces (texel(coords) samples the sky's texture at a panorama coordinate), its mipmaps by
// averaging, then each level filtered by radiance.hlsl's twin.
[[nodiscard]] CubeImage
make_radiance(const Scene3D& scene,
              const std::function<Float4(const std::array<float, 2>&)>& texel);

// Checks a scene's sky, fog and radiance: kinds that are one, finite numbers, at most
// kMaxSkyLights lights, a radiance size Godot takes, and textures the device has (known).
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
                              fog.depth_begin,      fog.depth_end,
                              scene.radiance.ambient_mix}) {
        if (!std::isfinite(value)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a scene's sky and fog numbers are finite");
        }
    }
    if (const TextureId texture = sky_texture(sky); texture != TextureId::None && !known(texture)) {
        return unknown_texture(texture);
    }
    const Radiance3D& radiance = scene.radiance;
    if (static_cast<std::size_t>(radiance.kind) > static_cast<std::size_t>(SkyKind::Panorama)) {
        return make_error(ErrorCode::InvalidArgument, "a scene's radiance kind is one of five");
    }
    if (radiance.size < 32 || radiance.size > 2048 || (radiance.size & (radiance.size - 1)) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a radiance's size is a power of two from 32 to 2048, not " +
                              std::to_string(radiance.size));
    }
    if (const TextureId texture = radiance_texture(scene);
        texture != TextureId::None && !known(texture)) {
        return unknown_texture(texture);
    }
    return {};
}

} // namespace slash::render::detail
