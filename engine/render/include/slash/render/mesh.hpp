#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 3D meshes and what a frame sees them from and lights them by, as Godot's Compatibility
// renderer: a forward pass over a depth buffer, before the frame's 2D draws, that lights each mesh
// with the frame's lights and a StandardMaterial3D (Material3D, in device.hpp).
//
//   auto mesh = device->create_mesh(vertices, indices);
//   render::Scene3D scene{.camera = render::look_at({0, 2, 5}, {0, 0, 0}, {0, 1, 0}),
//                         .projection = render::perspective(75.0F, aspect, 0.05F, 4000.0F),
//                         .lights = {{.type = render::LightType::Directional}}};
//   device->begin_frame({.clear = sky, .scene = scene});
//   device->draw_mesh(mesh, transform, {.albedo = {0.8F, 0.2F, 0.2F, 1.0F}});
//   device->end_frame();
//
// Space is Godot's: Y up, X right, and a camera looks down its -Z. Front faces wind clockwise as
// seen on the target, as Godot's do. Depth is reversed: projections map the near plane to 1 and
// the far plane to 0, and nearer pixels win.
//
// A light with shadow set casts shadows, as the Compatibility renderer casts them: each frame
// draws the meshes into shadow maps first, a directional light's into up to four cascades of one
// atlas and an omni or spot light's into tiles of another (six for an omni light, a cube's faces),
// and the forward pass compares each pixel with them, with Godot's biases and filters.
//
// A scene's sky (Sky3D) draws first, over the whole target, worked out for each pixel's view
// direction as Godot's sky shaders do; then fog (Fog3D) fades the meshes, and the sky by its sky
// affect, toward the fog's color with distance.
namespace slash::render {

// A mesh on the device. NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class MeshId : std::uint32_t { None = 0 };

// One corner of a mesh, in the mesh's own space: a position, a normal, a tangent and the sign of
// its binormal (cross(normal, tangent) times w, as Godot's), a texture coordinate from (0, 0) at
// the texture's top left, and a color.
struct Vertex3D {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0F, 1.0F, 0.0F};
    std::array<float, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    // The second texture coordinate, where a lightmap is sampled (LightmapShading), as Godot's
    // UV2: each triangle its own place in 0..1.
    std::array<float, 2> uv2{};
};

// The most corners and indices a mesh may have.
inline constexpr std::uint32_t kMaxMeshVertices = 1U << 24U;
inline constexpr std::uint32_t kMaxMeshIndices = 1U << 26U;
// Reads project.json's rendering object's mesh_lod.lod_change.threshold_pixels, as Godot's
// rendering/mesh_lod/lod_change/threshold_pixels: how many pixels a coarser LOD of a mesh may be
// off by where it is drawn; 0 draws every mesh in full. What is missing or not a number at or
// above 0 keeps the default, 1.
[[nodiscard]] float mesh_lod_threshold(const json::Value& rendering);
// Occlusion culling, as Godot's rendering/occlusion_culling settings: whether the camera's
// OccluderInstance3Ds hide meshes (use_occlusion_culling), the rays cast per thread into them
// (occlusion_rays_per_thread, which sizes the depth buffer) and whether the rays move a little
// each frame (jitter_projection), so thin gaps between occluders show.
struct OcclusionCulling {
    bool enabled = false;
    int rays_per_thread = 512;
    bool jitter = true;

    friend bool operator==(const OcclusionCulling&, const OcclusionCulling&) = default;
};
// Reads project.json's rendering object's occlusion_culling. What is missing or of the wrong type
// keeps its default.
[[nodiscard]] OcclusionCulling occlusion_culling(const json::Value& rendering);
// The most lights a frame's scene may have, Godot's Forward+ limits: directional lights
// (MAX_DIRECTIONAL_LIGHTS), and omni and spot lights (rendering/limits/cluster_builder/
// max_clustered_elements).
inline constexpr std::size_t kMaxDirectionalLights3D = 8;
inline constexpr std::size_t kMaxClusteredLights3D = 512;
// The lights the Compatibility method lights each pixel with: the first in a scene's list.
inline constexpr std::size_t kMaxLights3D = 64;
// The most lights whose shadows a frame draws: directional ones, then omni and spot ones. Lights
// past these shine without shadows.
inline constexpr std::size_t kMaxDirectionalShadows = 4;
inline constexpr std::size_t kMaxPositionalShadows = 16;

// The bits of a light's cull mask that count: Godot's 20 render layers. Shaders get them as a
// whole number in a float, which holds it exactly, since bits stored in a float can come back
// changed (Apple's OpenGL makes the NaN that 0xFFFFFFFF is canonical).
inline constexpr std::uint32_t kRenderLayers = 0xFFFFFU;

// Which faces a mesh hides, as Godot's cull_mode.
enum class CullMode : std::uint8_t {
    Back,     // faces seen from behind, Godot's default
    Front,    // faces seen from the front
    Disabled, // none; a face seen from behind is lit as if it faced the camera
};

// How a mesh's alpha shows, as Godot's transparency.
enum class Transparency : std::uint8_t {
    Disabled,     // opaque, Godot's default
    Alpha,        // blended, after the opaque meshes, without writing depth
    AlphaScissor, // opaque where alpha reaches alpha_scissor_threshold, and not drawn elsewhere
};

// Which channel of a texture a value is read from, as Godot's TextureChannel.
enum class TextureChannel : std::uint8_t { Red, Green, Blue, Alpha, Gray };

enum class LightType : std::uint8_t { Directional, Omni, Spot };

// Which shadow maps a mesh is drawn into, as Godot's GeometryInstance3D.cast_shadow.
enum class ShadowCasting : std::uint8_t {
    Off,         // none
    On,          // every light's whose shadow_caster_mask shares a bit with its layers, culled as
                 // its material culls
    DoubleSided, // the same, with no faces culled
    ShadowsOnly, // the same as On, and it isn't drawn in the frame: it casts but isn't seen
};

// How a directional light's shadow map covers what the camera sees, as Godot's
// DirectionalLight3D.directional_shadow_mode: in one piece, or in cascades split by distance.
enum class DirectionalShadowMode : std::uint8_t { Orthogonal, Parallel2Splits, Parallel4Splits };

// How soft shadows' edges are, as Godot's soft_shadow_filter_quality in the Compatibility
// renderer: hard and very low take one sample of the shadow map, low and medium five, high and
// ultra thirteen. Every sample filters four texels. An omni light's shadow takes one sample.
enum class ShadowQuality : std::uint8_t {
    Hard,
    SoftVeryLow,
    SoftLow,
    SoftMedium,
    SoftHigh,
    SoftUltra
};
// Their names in project.json, in order: "hard", "soft_very_low" and so on.
inline constexpr std::array<std::string_view, 6> kShadowQualityNames{
    "hard", "soft_very_low", "soft_low", "soft_medium", "soft_high", "soft_ultra"};

// A light in a frame's scene, with Godot's Light3D parameters and defaults.
struct Light3D {
    LightType type = LightType::Directional;
    std::array<float, 3> color{1.0F, 1.0F, 1.0F}; // sRGB
    float energy = 1.0F;
    float specular = 0.5F;
    // Where an omni or spot light is, in the world.
    std::array<float, 3> position{};
    // The way a directional or spot light shines: its node's -Z.
    std::array<float, 3> direction{0.0F, 0.0F, -1.0F};
    float range = 5.0F;       // omni and spot
    float attenuation = 1.0F; // how light falls off with distance, omni and spot
    float spot_angle = 45.0F; // degrees from direction to the cone's edge
    float spot_attenuation = 1.0F;
    // Lights the meshes whose layers share a bit with it, of the first kRenderLayers.
    std::uint32_t cull_mask = 0xFFFFFFFFU;
    // Its node's +Y, square to direction: how its shadow maps are turned.
    std::array<float, 3> up{0.0F, 1.0F, 0.0F};

    // Shadows, with Light3D's defaults (a DirectionalLight3D's normal bias is 2 and a
    // SpotLight3D's bias 0.03).
    bool shadow = false;
    float shadow_bias = 0.1F;              // pushes depths away from the light, against acne
    float shadow_normal_bias = 1.0F;       // moves pixels along their normals, in shadow map texels
    float shadow_opacity = 1.0F;           // 0 casts no shadow, 1 a full one
    bool shadow_reverse_cull_face = false; // draws the faces meshes cull, and culls the rest
    // Meshes whose layers share a bit with it cast its shadows.
    std::uint32_t shadow_caster_mask = 0xFFFFFFFFU;
    // Directional lights: how the shadow map covers the view, where the cascades split as
    // fractions of the distance shadows reach (max_distance, or the camera's far plane if
    // nearer), whether neighboring cascades blend, where shadows start fading out (a fraction of
    // that distance), and how far behind the view casters still cast (pancake size).
    DirectionalShadowMode shadow_mode = DirectionalShadowMode::Parallel4Splits;
    std::array<float, 3> shadow_splits{0.1F, 0.2F, 0.5F};
    bool shadow_blend_splits = false;
    float shadow_fade_start = 0.8F;
    float shadow_max_distance = 100.0F;
    float shadow_pancake_size = 20.0F;
    // Baked into lightmaps (Godot's BAKE_STATIC): meshes with a lightmap (LightmapShading) leave
    // it out, since theirs holds its light; the rest are lit by it as by any other.
    bool baked = false;
    // How much it lights volumetric fog, as Godot's light_volumetric_fog_energy.
    float volumetric_fog_energy = 1.0F;

    bool operator==(const Light3D&) const = default;
};

// The shadow atlases' sizes in texels, powers of two from 256 to 16384, and their filters, as
// Godot's rendering/lights_and_shadows/directional_shadow and positional_shadow settings. A
// device draws an atlas at most as large as its largest texture.
struct ShadowSettings {
    std::uint32_t directional_size = 4096;
    ShadowQuality directional_quality = ShadowQuality::SoftLow;
    std::uint32_t positional_size = 4096;
    ShadowQuality positional_quality = ShadowQuality::SoftLow;

    bool operator==(const ShadowSettings&) const = default;
};
// Reads project.json's rendering.lights_and_shadows: {"directional_shadow": {"size",
// "soft_shadow_filter_quality"}, "positional_shadow": {"atlas_size",
// "soft_shadow_filter_quality"}}. What is missing or not valid keeps its default.
[[nodiscard]] ShadowSettings shadow_settings_from_json(const json::Value& value);
// Whether size is an atlas size: a power of two from 256 to 16384.
[[nodiscard]] bool valid_shadow_atlas_size(std::uint32_t size);

// A texture on the device (device.hpp).
// NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class TextureId : std::uint32_t;

// How the lit 3D world's colors map to the screen's, as Godot's Environment.tonemap_mode.
enum class Tonemapper : std::uint8_t { Linear, Reinhard, Filmic, Aces, Agx };
inline constexpr std::array<std::string_view, 5> kTonemapperNames{"linear", "reinhard", "filmic",
                                                                  "aces", "agx"};

// How glow blends into the lit 3D world, as Godot's Environment.glow_blend_mode with its Forward+
// renderer: added, screened (never past white), soft light (after tonemapping), replacing it, or
// mixed in by glow_mix.
enum class GlowBlendMode : std::uint8_t { Additive, Screen, Softlight, Replace, Mix };
inline constexpr std::array<std::string_view, 5> kGlowBlendModeNames{"additive", "screen",
                                                                     "softlight", "replace", "mix"};

// Glow's levels with Forward+, each half the size of the last, as Godot's RS::MAX_GLOW_LEVELS.
inline constexpr std::size_t kMaxGlowLevels = 7;

// The shape of depth of field's blur, as Godot's RS::DOFBokehShape: a box (two straight passes),
// a hexagon (a straight pass, then two slanted ones) or a circle (one pass, at half size).
enum class BokehShape : std::uint8_t { Box, Hexagon, Circle };
inline constexpr std::array<std::string_view, 3> kBokehShapeNames{"box", "hexagon", "circle"};
// How many samples depth of field's blur takes, as Godot's RS::DOFBlurQuality: a box or hexagon
// blurs at half size with 6 or 12 samples a side for very_low and low, and at full size with 12
// or 24 for medium and high; a circle steps 8, 4, 1 or 0.5 pixels further each sample.
enum class BokehQuality : std::uint8_t { VeryLow, Low, Medium, High };
inline constexpr std::array<std::string_view, 4> kBokehQualityNames{"very_low", "low", "medium",
                                                                    "high"};

// Godot's rendering/camera/depth_of_field settings.
struct DepthOfFieldSettings {
    BokehShape bokeh_shape = BokehShape::Hexagon;
    BokehQuality bokeh_quality = BokehQuality::Low;
    // Moves each pixel's samples along the blur by a random part of a step, which smooths banding
    // into noise. Godot picks a new seed each frame; here it comes from the game time, so the
    // same frames come out the same.
    bool use_jitter = false;

    bool operator==(const DepthOfFieldSettings&) const = default;
};
// Reads project.json's rendering.camera.depth_of_field: {"depth_of_field_bokeh_shape",
// "depth_of_field_bokeh_quality", "depth_of_field_use_jitter"}. What is missing or not valid keeps
// its default.
[[nodiscard]] DepthOfFieldSettings depth_of_field_settings_from_json(const json::Value& value);

// How many samples screen-space ambient occlusion takes and how it smooths them, as Godot's
// RS::EnvironmentSSAOQuality: very_low takes 3 samples in half its slices and blurs plainly,
// low 5, medium 12, high 12 with depth mipmaps and edges from normals, and ultra 5 to 32 where
// an importance map asks for them.
enum class SsaoQuality : std::uint8_t { VeryLow, Low, Medium, High, Ultra };
inline constexpr std::array<std::string_view, 5> kSsaoQualityNames{"very_low", "low", "medium",
                                                                   "high", "ultra"};

// Godot's rendering/environment/ssao settings.
struct SsaoSettings {
    SsaoQuality quality = SsaoQuality::Medium;
    // Works at half the resolution in each direction again: a quarter of the frame's, rather
    // than half.
    bool half_size = true;
    float adaptive_target = 0.5F;  // ultra: how many of the extra samples to take, from 0 to 1
    std::uint32_t blur_passes = 2; // from 0 to 6; very_low always blurs once
    float fadeout_from = 50.0F;    // the distance where occlusion starts fading out
    float fadeout_to = 300.0F;     // and where it is gone

    bool operator==(const SsaoSettings&) const = default;
};
// Reads project.json's rendering.environment.ssao: {"quality", "half_size", "adaptive_target",
// "blur_passes", "fadeout_from", "fadeout_to"}. What is missing or not valid keeps its default.
[[nodiscard]] SsaoSettings ssao_settings_from_json(const json::Value& value);

// Forward+: screen-space ambient occlusion, as Godot's Environment.ssao_* with its Forward+
// renderer. A prepass draws the opaque meshes' depth and view-space normals, the occlusion of
// each pixel is worked out from the depths around it by Intel's ASSAO, as Godot's ssao.glsl,
// blurred along edges and put back together at the frame's size, and the meshes' color pass
// darkens their ambient light by it (and their direct light, by light_affect). Transparent meshes
// take none, as in Godot. Compatibility has no SSAO.
struct Ssao3D {
    bool enabled = false;
    float radius = 1.0F;     // how far around a pixel occludes it, in world units
    float intensity = 2.0F;  // how dark the occlusion is
    float power = 1.5F;      // the occlusion's curve
    float detail = 0.5F;     // how much the pixels right beside a pixel occlude it
    float horizon = 0.06F;   // samples below this angle above the surface don't occlude
    float sharpness = 0.98F; // how much the blur keeps to edges
    // How much it darkens direct light, from 0 to 1: light_affect times ao_channel_affect, as
    // Godot 4.6's scene shader mixes the two.
    float light_affect = 0.0F;
    float ao_channel_affect = 0.0F;
    SsaoSettings settings{};

    bool operator==(const Ssao3D&) const = default;
};

// Godot's rendering/environment/screen_space_reflection settings.
struct SsrSettings {
    // Traces at half the frame's resolution in each direction, then resolves the reflections to
    // its size along the depths and normals.
    bool half_size = true;

    bool operator==(const SsrSettings&) const = default;
};
// Reads project.json's rendering.environment.screen_space_reflection: {"half_size"}. What is
// missing or not valid keeps its default.
[[nodiscard]] SsrSettings ssr_settings_from_json(const json::Value& value);

// Forward+: screen-space reflections, as Godot 4.6's Environment.ssr_* with its Forward+
// renderer. From the prepass's depth and normals, each opaque pixel's reflected ray is traced
// across a pyramid of the frame's nearest depths (Hi-Z) until it hits something on the screen,
// which is read from the frame before, as it lands now; rougher pixels read blurrier levels of the
// reflections. The meshes' color pass puts them over the reflections from the sky's radiance.
// What the ray hits must be on the screen: things off it, or behind what is in front, are not
// reflected. Transparent meshes take none, and pixels from roughness 0.7 up none either, as in
// Godot. Compatibility has no SSR.
struct Ssr3D {
    bool enabled = false;
    std::uint32_t max_steps = 64; // how many steps a ray takes at most
    float fade_in = 0.15F;        // how reflections fade in near where they leave the surface
    float fade_out = 2.0F;        // and fade out toward the screen's edges, as an easing curve
    float depth_tolerance = 0.5F; // how far behind a surface a ray still hits it, in world units
    SsrSettings settings{};

    bool operator==(const Ssr3D&) const = default;
};

// What happens to the 3D world's colors after it is lit, as Godot's Environment's tonemap, glow
// and adjustments.
//
// Forward+ draws the sky and meshes linear into a texture of 16-bit floats the target's size, as
// Godot's Forward+ renderer does: glow blurs its parts brighter than glow_hdr_threshold into
// glow_levels textures, the first half the target's size and each next one half the last, and a
// pass over the target exposes the colors, blends the glow in by glow_blend_mode, tonemaps, then
// adjusts brightness, contrast and saturation and corrects colors, before the frame's 2D draws go
// over it.
//
// Compatibility follows Godot's Compatibility renderer. Without glow or adjustments, meshes
// tonemap as they draw. With either, they draw untonemapped into a texture of the target's size,
// glow blurs its bright parts into four textures each half the size of the last, and a pass over
// the target screens the glow in, tonemaps, adjusts and corrects colors. The textures are 8 bits a
// channel, so glow comes from colors above glow_hdr_threshold only when it is below 1, or from
// glow_bloom, and glow_levels, glow_strength, glow_mix and glow_blend_mode do nothing.
struct PostProcess {
    Tonemapper tonemapper = Tonemapper::Linear;
    float exposure = 1.0F; // multiplies the lit colors before they are tonemapped
    // The color tonemapped to white: with Forward+ at least 1 (2 for AgX), and Compatibility's
    // AgX ignores it.
    float white = 1.0F;
    float agx_contrast = 1.25F; // Forward+'s AgX: how steep its curve is, as tonemap_agx_contrast
    bool glow = false;
    float glow_intensity = 0.3F;
    // Forward+: how much each level adds, already divided by their sum when the environment
    // normalizes them. Levels at 0.01 or less are neither blurred nor added.
    std::array<float, kMaxGlowLevels> glow_levels{0.0F, 0.8F, 0.4F, 0.1F, 0.0F, 0.0F, 0.0F};
    float glow_strength = 1.0F; // Forward+: multiplies each level as it is blurred
    float glow_mix = 0.05F;     // Forward+: how much glow replaces the colors in the mix mode
    GlowBlendMode glow_blend_mode = GlowBlendMode::Screen; // Forward+
    float glow_bloom = 0.0F;              // how much of every color glows, from 0 to 1
    float glow_hdr_threshold = 1.0F;      // colors above it glow
    float glow_hdr_scale = 2.0F;          // how far above it they reach full glow
    float glow_hdr_luminance_cap = 12.0F; // the brightest glow
    bool adjustments = false;
    float brightness = 1.0F;
    float contrast = 1.0F;
    float saturation = 1.0F;
    // With adjustments, a lookup texture colors pass through: one texel tall, each channel read
    // at its own value along x (Godot's 1D LUT, a GradientTexture1D), or as wide as its height
    // squared, height slices of height texels side by side for blue, x in each red and y green
    // (a 3D LUT). TextureId{} for none.
    TextureId color_correction{};
    // Forward+: auto exposure, as Godot's CameraAttributes. Each frame's brightness is measured
    // as Godot's luminance_reduce.glsl does, averaging each pixel's brightest channel in blocks of
    // 8 x 8, then the blocks' averages, down to one value; it moves auto_exposure_adjust of the way
    // from the last frame's to the new one and is kept from auto_exposure_min to
    // auto_exposure_max. The colors are exposed by auto_exposure_scale over it (a brightness
    // below 0.0001 counts as 0.0001), and so is the glow's first level. A frame whose
    // auto_exposure_version isn't the last measured one's, or the first at its size, takes its own
    // brightness as it is, as Godot does when auto exposure starts.
    bool auto_exposure = false;
    float auto_exposure_scale = 0.4F;
    float auto_exposure_min = 0.0F;
    float auto_exposure_max = 1.0F;
    float auto_exposure_adjust = 1.0F; // from 0 (stays) to 1 (the new brightness)
    std::uint64_t auto_exposure_version = 0;
    // Forward+: depth of field, as Godot's CameraAttributes, before auto exposure and glow. Each
    // pixel's blur size comes from its distance from the camera along its view: past
    // dof_far_distance it grows to dof_amount times 64 pixels over dof_far_transition, and nearer
    // than dof_near_distance over dof_near_transition. A negative transition grows it the
    // physical way instead, as Godot's automatic transition. The world is blurred by the shape and
    // quality of depth_of_field and blended in by each pixel's blur size, as Godot's
    // bokeh_dof.glsl.
    bool dof_far = false;
    float dof_far_distance = 10.0F;
    float dof_far_transition = 5.0F;
    bool dof_near = false;
    float dof_near_distance = 2.0F;
    float dof_near_transition = 1.0F;
    float dof_amount = 0.1F;
    DepthOfFieldSettings depth_of_field{};
    float dof_jitter_seed = 0.0F; // with use_jitter: from 0 up to 1000

    bool operator==(const PostProcess&) const = default;
};

// What a frame's 3D world shows behind its meshes, as Godot's Environment.background_mode with
// its Compatibility renderer: nothing but the clear color, or a sky worked out for each pixel's
// view direction and drawn before the meshes.
enum class SkyKind : std::uint8_t {
    None,       // the clear color, untouched
    Color,      // color: the clear color or background color, fogged as a sky is
    Procedural, // a ProceduralSkyMaterial
    Physical,   // a PhysicalSkyMaterial
    Panorama,   // a PanoramaSkyMaterial
};

// A sky shows the sun of at most this many directional lights, as Godot's Compatibility renderer.
inline constexpr std::size_t kMaxSkyLights = 4;

// A DirectionalLight3D the sky shows: its sun's place and size in the sky.
struct SkyLight {
    std::array<float, 3> direction{0.0F, 0.0F, -1.0F}; // the way it shines, in the world
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};      // sRGB
    float energy = 1.0F;
    float angular_distance = 0.0F; // degrees across the sun's radius, as light_angular_distance

    bool operator==(const SkyLight&) const = default;
};

// Godot's ProceduralSkyMaterial: a gradient from the horizon up to the sky's top and down to the
// ground's bottom, with each light's sun on it. Colors are sRGB.
struct ProceduralSky {
    std::array<float, 3> sky_top_color{0.385F, 0.454F, 0.55F};
    std::array<float, 3> sky_horizon_color{0.6463F, 0.6558F, 0.6708F};
    float sky_curve = 0.15F;
    float sky_energy_multiplier = 1.0F;
    TextureId sky_cover{}; // a panorama over the sky, or TextureId{} for none
    std::array<float, 4> sky_cover_modulate{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> ground_bottom_color{0.2F, 0.169F, 0.133F};
    std::array<float, 3> ground_horizon_color{0.6463F, 0.6558F, 0.6708F};
    float ground_curve = 0.02F;
    float ground_energy_multiplier = 1.0F;
    float sun_angle_max = 30.0F; // degrees from a sun's edge that its glow reaches
    float sun_curve = 0.15F;
    bool use_debanding = true;
    float energy_multiplier = 1.0F;

    bool operator==(const ProceduralSky&) const = default;
};

// Godot's PhysicalSkyMaterial: Rayleigh and Mie scattering of the first light's sun, after
// Preetham. Colors are sRGB.
struct PhysicalSky {
    float rayleigh_coefficient = 2.0F;
    std::array<float, 3> rayleigh_color{0.3F, 0.405F, 0.6F};
    float mie_coefficient = 0.005F;
    float mie_eccentricity = 0.8F;
    std::array<float, 3> mie_color{0.69F, 0.729F, 0.812F};
    float turbidity = 10.0F;
    float sun_disk_scale = 1.0F;
    std::array<float, 3> ground_color{0.1F, 0.07F, 0.034F};
    float energy_multiplier = 1.0F;
    bool use_debanding = true;
    TextureId night_sky{}; // a panorama behind the sky, or TextureId{} for none

    bool operator==(const PhysicalSky&) const = default;
};

// Godot's PanoramaSkyMaterial: an equirectangular texture all round. Whether it filters is the
// texture's own sampler's choice.
struct PanoramaSky {
    TextureId panorama{};
    float energy_multiplier = 1.0F;

    bool operator==(const PanoramaSky&) const = default;
};

// What a frame draws behind its meshes. Only the material of kind is used.
struct Sky3D {
    SkyKind kind = SkyKind::None;
    std::array<float, 3> color{}; // SkyKind::Color's, sRGB
    ProceduralSky procedural;
    PhysicalSky physical;
    PanoramaSky panorama;
    float energy = 1.0F; // Godot's background_energy_multiplier
    // The world to the sky's space, column-major: the inverse of Environment.sky_rotation's turn.
    std::array<float, 16> orientation = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                         0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    // Degrees across the target's height the sky is seen with, or 0 for the camera's own field
    // of view, as Environment.sky_custom_fov.
    float custom_fov = 0.0F;
    std::vector<SkyLight> lights; // up to kMaxSkyLights

    bool operator==(const Sky3D&) const = default;
};

// Godot's Environment fog in its Compatibility renderer: meshes fade toward light_color with
// distance (and below height), and so does the sky, by sky_affect.
struct Fog3D {
    bool enabled = false;
    bool depth = false; // Godot's FOG_MODE_DEPTH, rather than exponential fog
    std::array<float, 3> light_color{0.518F, 0.553F, 0.608F}; // sRGB
    float light_energy = 1.0F;
    float sun_scatter = 0.0F; // how much directional lights brighten the fog they shine through
    float density = 0.01F;
    float aerial_perspective = 0.0F; // how much the sky colors the fog in front of it
    float sky_affect = 1.0F;
    float height = 0.0F;
    float height_density = 0.0F; // fog thickening below height; 0 for none
    float depth_curve = 1.0F;
    float depth_begin = 10.0F;
    float depth_end = 100.0F; // where depth fog is thickest; 0 or less for the far plane

    bool operator==(const Fog3D&) const = default;
};

// Godot's rendering/environment/volumetric_fog settings: the froxels across (times the frame's
// width over the mean of its width and height; down, over it) and deep, each from 16 to 512, and
// whether the fog is blurred across and down before it is gathered.
struct VolumetricFogSettings {
    std::uint32_t volume_size = 64;
    std::uint32_t volume_depth = 64;
    bool use_filter = true;

    bool operator==(const VolumetricFogSettings&) const = default;
};
// Reads project.json's rendering.environment.volumetric_fog: {"volume_size", "volume_depth",
// "use_filter"}. What is missing or not valid keeps its default.
[[nodiscard]] VolumetricFogSettings volumetric_fog_settings_from_json(const json::Value& value);

// The shapes of a FogVolume3D, Godot's RS::FogVolumeShape.
enum class FogVolumeShape : std::uint8_t { Ellipsoid, Cone, Cylinder, Box, World };

// A place where volumetric fog is thicker, thinner or glows, as Godot's FogVolume with its
// FogMaterial (material set) or its default fog material (density 1, white albedo, no fade).
struct FogVolume3D {
    // Its place, turn and scale in the world (column-major, as Mat4); a World shape's isn't used.
    std::array<float, 16> transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                       0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> size{2.0F, 2.0F, 2.0F};
    FogVolumeShape shape = FogVolumeShape::Box;
    bool material = false;
    float density = 1.0F;
    std::array<float, 3> albedo{1.0F, 1.0F, 1.0F};   // sRGB
    std::array<float, 3> emission{0.0F, 0.0F, 0.0F}; // sRGB
    float height_falloff = 0.0F;
    float edge_fade = 0.1F;

    bool operator==(const FogVolume3D&) const = default;
};
// The most fog volumes a frame fills its fog with; those past it are left out.
inline constexpr std::size_t kMaxFogVolumes = 32;

// The most levels a VoxelGI's light has: ten for Godot's 512 subdivision, from 512 texels across
// down to 1.
inline constexpr std::size_t kMaxVoxelGILevels = 10;
// The most texels along each axis of a VoxelGI's level 0 a device traces.
inline constexpr std::uint32_t kMaxVoxelGICells = 1024;

// A VoxelGI a mesh traces cones through for its ambient light and reflections, as Godot's
// Forward+ renderer pairs a VoxelGI with the meshes it reaches (servers/rendering/renderer_rd/
// shaders/environment/gi.glsl): its light, RGBA8 (the light over its dynamic range, and how much
// of each cell is solid), is a 3D texture of size texels at level 0 and half as many along each
// axis each level after (at least 1), kept in an atlas as each level's z slices side by side.
struct VoxelGIProbe {
    // The world to its cells' space, a row each of a 3x4 matrix: a unit a texel of level 0, its
    // box from 0 to size.
    std::array<float, 12> to_cell{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                  0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    std::array<std::uint32_t, 3> size{1, 1, 1};
    std::uint32_t levels = 1; // 1 to kMaxVoxelGILevels
    // Where each level is in the atlas: the left and top texels of its first slice, then how
    // many slices a row of it holds. Slice z is z % columns slices right and z / columns down.
    std::array<std::array<std::uint32_t, 3>, kMaxVoxelGILevels> placement{};
    float energy = 1.0F;       // the light's scale: its dynamic range, energy and exposure
    float bias = 1.5F;         // how far cones start from the surface, in texels
    float normal_bias = 0.0F;  // how far the surface moves along its normal first, in texels
    bool blend_ambient = true; // where cones see nothing, the ambient light and sky show through

    bool operator==(const VoxelGIProbe&) const = default;
};

// The most VoxelGIs whose light volumetric fog takes in, as Godot's MAX_VOXEL_GI_INSTANCES.
inline constexpr std::size_t kMaxFogVoxelGIs = 8;

// Forward+: volumetric fog, as Godot 4.6's Environment.volumetric_fog_* with its Forward+
// renderer. The view to length is cut into froxels (cells of the frustum, deeper further away
// by detail_spread), each lit by the lights (their volumetric_fog_energy, through their shadows,
// scattered forward or back by anisotropy) and the ambient light and sky (ambient_inject), and
// glowing with its emission; with temporal reprojection each is blended with where it was the
// frame before, jittered across the cell, so the fog is smooth over frames. The froxels are then
// gathered along each column into how much light the fog in front of a depth adds and lets
// through, which meshes and the sky (by sky_affect) are seen through. A target's first frame
// isn't blended with anything, and frames without meshes have none. Compatibility has none.
struct VolumetricFog3D {
    bool enabled = false;
    float density = 0.05F;
    std::array<float, 3> albedo{1.0F, 1.0F, 1.0F};   // sRGB: the color light scatters off it
    std::array<float, 3> emission{0.0F, 0.0F, 0.0F}; // sRGB
    float emission_energy = 1.0F;
    float anisotropy = 0.2F;    // from -0.9 (back toward lights) to 0.9 (on, away from them)
    float length = 64.0F;       // how far it reaches from the camera
    float detail_spread = 2.0F; // above 1, more froxels near the camera
    float ambient_inject = 0.0F;
    float sky_affect = 1.0F;
    bool temporal_reprojection = true;
    float temporal_reprojection_amount = 0.9F; // from 0.5 to 0.99
    VolumetricFogSettings settings{};
    // At most kMaxFogVolumes, filled in this order; initialized so that it may be left out.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<FogVolume3D> volumes{};
    // How much the VoxelGIs' light lights it (gi_inject), from 0 to 16, and the VoxelGIs, at most
    // kMaxFogVoxelGIs, with the atlas their light is in, as VoxelGIShading's (left out alike).
    float gi_inject = 1.0F;
    TextureId voxel_gi_atlas{};
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<VoxelGIProbe> voxel_gis{};

    bool operator==(const VolumetricFog3D&) const = default;
};

// Light a frame's sky gives its meshes, as Godot's radiance map in its Compatibility renderer: one
// of the sky's materials drawn into a cube map size texels across, then blurred more for each
// smaller level, which lit meshes take ambient light from (by their normal, the blurriest level)
// and reflect (by roughness, sharper for smoother surfaces). It is drawn again only when what it
// is drawn from changes.
struct Radiance3D {
    // Which of the sky's materials (Sky3D's, with its energy, orientation and lights) it is drawn
    // from, or None for no radiance. Usually the sky's own kind; it can differ from it when the
    // background isn't the sky.
    SkyKind kind = SkyKind::None;
    std::uint32_t size = 256; // a power of two from 32 to 2048, as Sky.radiance_size
    // Ambient light from it, mixed into Scene3D's ambient light by ambient_mix, as
    // Environment.ambient_light_sky_contribution.
    bool ambient = false;
    float ambient_mix = 1.0F;
    bool reflections = false; // lit meshes reflect it

    bool operator==(const Radiance3D&) const = default;
};

// The most cubes a reflection atlas holds: 42 cubes of six faces are the 256 texture layers every
// GPU and WebGPU take.
inline constexpr std::uint32_t kMaxReflectionCubes = 42;
// The level of an atlas cube the roughest surfaces reflect and ambient light is taken from, as
// Godot's MAX_ROUGHNESS_LOD with its 7 roughness layers.
inline constexpr float kReflectionMaxLevel = 6.0F;

// Godot's rendering/reflections/reflection_atlas settings: how many texels across each face of a
// reflection probe's cube is (a power of two from 32 to 2048), and how many probes a frame can
// reflect at most (1 to kMaxReflectionCubes; Godot takes up to 4096).
struct ReflectionAtlasSettings {
    std::uint32_t reflection_size = 256;
    std::uint32_t reflection_count = 64;

    bool operator==(const ReflectionAtlasSettings&) const = default;
};
// Reads project.json's rendering.reflections.reflection_atlas: {"reflection_size",
// "reflection_count"}. What is missing or not valid keeps its default.
[[nodiscard]] ReflectionAtlasSettings reflection_atlas_settings_from_json(const json::Value& value);
// Whether project.json's rendering object asks for high quality VoxelGIs:
// global_illumination.voxel_gi.quality "high" (Scene3D::voxel_gi_high_quality).
[[nodiscard]] bool voxel_gi_high_quality(const json::Value& rendering);
// Whether project.json's rendering object turns on temporal antialiasing:
// anti_aliasing.quality.use_taa, as Godot's rendering/anti_aliasing/quality/use_taa
// (Scene3D::use_taa).
[[nodiscard]] bool use_taa(const json::Value& rendering);

// How a reflection probe lights what is inside it, as Godot's ReflectionProbe.ambient_mode: not
// at all, by the blurriest level of its cube along each pixel's normal, or by a color.
enum class ReflectionAmbient : std::uint8_t { Disabled, Environment, Color };

// A reflection probe a frame's meshes reflect, as Godot's ReflectionProbe in its Forward+
// renderer: a box of the world whose surroundings were drawn into a cube of the frame's reflection
// atlas, from its origin_offset, by a capture (ProbeCapture). Meshes inside the box reflect the
// cube in place of the sky's radiance, by roughness, and may take ambient light from it, fading
// out over blend_distance toward its sides. Where probes overlap, the smallest is taken first and
// the larger ones fill in what it leaves.
struct ReflectionProbe3D {
    // The probe's place and turn in the world, without scale (column-major, as Mat4).
    std::array<float, 16> transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                       0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> size{20.0F, 20.0F, 20.0F};       // the box, centered on the probe
    std::array<float, 3> origin_offset{0.0F, 0.0F, 0.0F}; // where the cube was drawn from
    float intensity = 1.0F;
    float blend_distance = 1.0F;
    // Reflections are aimed at the box's walls from origin_offset, as for a room, rather than
    // taken as if what they show were far away.
    bool box_projection = false;
    std::uint32_t reflection_mask = 0xFFFFF; // the layers whose meshes reflect it
    ReflectionAmbient ambient_mode = ReflectionAmbient::Environment;
    std::array<float, 3> ambient_color{0.0F, 0.0F, 0.0F}; // sRGB, for ReflectionAmbient::Color
    float ambient_color_energy = 1.0F;
    std::uint32_t cube = 0; // the atlas cube it was drawn into

    bool operator==(const ReflectionProbe3D&) const = default;
};

// A frame's reflection probes, in Forward+: those meshes reflect, in the order Godot keeps them
// (largest first), and the atlas their cubes are in, a texture made with TextureDesc::cubes.
// Compatibility draws none yet.
struct Reflections3D {
    TextureId atlas{};
    std::vector<ReflectionProbe3D> probes; // at most the atlas's cubes
    // What their light is multiplied by, as Godot's exposure normalization: the camera's exposure
    // over the one they were drawn with.
    float exposure = 1.0F;

    bool operator==(const Reflections3D&) const = default;
};

struct Image; // in device.hpp

// The most decals a frame draws, Godot's rendering/limits/cluster_builder/max_clustered_elements:
// those past it are left out.
inline constexpr std::size_t kMaxDecals3D = 512;

// How decals' textures are filtered, as Godot's rendering/textures/decals/filter: by the nearest
// texel or between texels, and with mipmaps, levels of the atlas each half the size of the one
// before, between which minified decals are filtered too. The anisotropic modes filter as the
// mipmap ones.
enum class DecalFilter : std::uint8_t {
    Nearest,
    Linear,
    NearestMipmaps,
    LinearMipmaps,
    NearestMipmapsAnisotropic,
    LinearMipmapsAnisotropic
};
// Their names in project.json, in order: "nearest", "linear" and so on.
inline constexpr std::array<std::string_view, 6> kDecalFilterNames{"nearest",
                                                                   "linear",
                                                                   "nearest_mipmaps",
                                                                   "linear_mipmaps",
                                                                   "nearest_mipmaps_anisotropic",
                                                                   "linear_mipmaps_anisotropic"};

// A decal, as Godot's Decal in its Forward+ renderer: a box that projects its textures down its
// -Y onto the surfaces inside it, changing their albedo (by albedo_mix), normal, ambient
// occlusion, roughness and metallic (an ORM texture's red, green and blue) where the albedo
// texture's alpha says, and adding emission. It fades toward its top and bottom by upper_fade and
// lower_fade, where surfaces face away from its up by normal_fade, and with distance_fade from
// distance_fade_begin to distance_fade_begin + distance_fade_length from the camera. Decals are
// drawn nearest the camera last (on top), less their sorting_offset. A decal without an albedo or
// an emission texture draws nothing.
struct Decal3D {
    // Its place, turn and scale in the world (column-major, as Mat4).
    std::array<float, 16> transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                       0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> size{2.0F, 2.0F, 2.0F}; // the box, centered on it
    // The albedo (sRGB, with alpha), normal (Y down, as most tools draw them), ORM and emission
    // (sRGB) textures, each RGBA8 pixels or nullptr for none. Their pixels are copied into the
    // device's atlas of decal textures when a frame first draws them.
    // NOLINTBEGIN(readability-redundant-member-init)
    std::shared_ptr<const Image> albedo{};
    std::shared_ptr<const Image> normal{};
    std::shared_ptr<const Image> orm{};
    std::shared_ptr<const Image> emission{};
    // NOLINTEND(readability-redundant-member-init)
    float emission_energy = 1.0F;
    std::array<float, 4> modulate{1.0F, 1.0F, 1.0F, 1.0F}; // sRGB, times the albedo and emission
    float albedo_mix = 1.0F;
    float normal_fade = 0.0F;
    float upper_fade = 0.3F;
    float lower_fade = 0.3F;
    bool distance_fade = false;
    float distance_fade_begin = 40.0F;
    float distance_fade_length = 10.0F;
    std::uint32_t cull_mask = 0xFFFFF; // the layers whose meshes it draws on
    float sorting_offset = 0.0F;

    bool operator==(const Decal3D&) const = default;
};

// A frame's decals, in Forward+; Compatibility draws none, as Godot's.
struct Decals3D {
    std::vector<Decal3D> decals; // in any order
    DecalFilter filter = DecalFilter::LinearMipmaps;

    bool operator==(const Decals3D&) const = default;
};
// Reads project.json's rendering object's textures.decals.filter. What is missing or not valid
// keeps the default, linear_mipmaps.
[[nodiscard]] DecalFilter decal_filter(const json::Value& rendering);
// Fails unless a decal's numbers are usable and its textures are RGBA8 images of 1 to
// kMaxTextureSize texels across, saying which, after name ("the decal's size is above 0").
[[nodiscard]] Result<void> check_decal(const Decal3D& decal, std::string_view name = "the decal");
// Fails unless every decal passes check_decal, the filter is one of Godot's and the atlas of
// their textures fits in what devices hold (2^25 texels over all its levels, 128 MiB: a texture
// 2048 texels across and a few smaller ones). Devices refuse frames whose decals fail it.
[[nodiscard]] Result<void> check_decals(const Decals3D& decals);

// A frame that draws the world into a face of a reflection atlas's cube instead of its target, as
// Godot draws a ReflectionProbe's sides: linear and unprocessed, as a Forward+ frame's world is
// before its post passes, with no SSAO, SSR or probes. The frame's target is None and its size
// the atlas's. After a cube's sixth face, its levels are filtered from it as the sky's radiance
// is (shaders/radiance.hlsl), for rougher surfaces.
struct ProbeCapture {
    TextureId atlas{};
    std::uint32_t cube = 0;
    std::uint32_t face = 0; // +x, -x, +y, -y, +z, -z, as cube textures lay them out

    bool operator==(const ProbeCapture&) const = default;
};

// A capture's camera for a face of a probe at transform (its place and turn, without scale), drawn
// from origin_offset: the world to the camera's space, and its projection, which reaches as far
// as Godot's does (the box's side the face looks at, or max_distance when that is further). A
// cube's faces hold a probe's surroundings mirrored along z, so that each face is seen through a
// camera that turns without mirroring: the face +z holds what is toward -z.
[[nodiscard]] std::array<float, 16> probe_face_view(const std::array<float, 16>& transform,
                                                    const std::array<float, 3>& origin_offset,
                                                    std::uint32_t face);
[[nodiscard]] std::array<float, 16> probe_face_projection(const std::array<float, 3>& size,
                                                          const std::array<float, 3>& origin_offset,
                                                          float max_distance, std::uint32_t face);

// How meshes are lit, as Godot's rendering/renderer/rendering_method. Forward+ sorts the omni and
// spot lights into clusters of the screen and lights each pixel with those that reach it, so a
// scene can have all kMaxClusteredLights3D of them; Compatibility lights every pixel with the
// first kMaxLights3D lights, as Godot's Compatibility renderer, and is how the OpenGL devices
// always draw, since OpenGL ES 3.0 has no storage buffers to hold clusters.
enum class RenderMethod : std::uint8_t { ForwardPlus, Compatibility };
inline constexpr std::array<std::string_view, 2> kRenderMethodNames{"forward_plus",
                                                                    "gl_compatibility"};
// A method by its Godot name, or nullopt.
[[nodiscard]] std::optional<RenderMethod> parse_render_method(std::string_view name);

// A frame's 3D drawn smaller and upscaled to the target with AMD's FidelityFX Super Resolution 2,
// as Godot's scaling_3d mode "fsr2" (Forward+ only): the meshes are drawn at the target's size
// times scale with the camera moved across a render pixel each frame, and FSR 2 rebuilds the
// target's size from them and the target's frames before, before the post passes. sharpness is
// Godot's fsr_sharpness: 0 is the sharpest and 2 turns sharpening off. fsr3 rebuilds it with
// FidelityFX Super Resolution 3.1's upscaler instead, AMD's later passes over the same inputs
// (scaling_3d mode "fsr3", which Godot doesn't have), and metalfx with Apple's MetalFX temporal
// scaler (mode "metalfx_temporal"), on a device whose metalfx() has it; it doesn't sharpen.
// upscaler names a temporal upscaler the game added (render::add_upscaler), which takes the same
// inputs in their place.
struct Fsr2Scaling {
    float scale = 0.77F;
    float sharpness = 0.2F;
    bool fsr3 = false;
    bool metalfx = false;
    std::string upscaler{}; // NOLINT(readability-redundant-member-init)

    bool operator==(const Fsr2Scaling&) const = default;
};

// What a frame's meshes are seen from and lit by. Matrices are column-major, as Mat4.
struct Scene3D {
    // The world to the camera's space: the inverse of the camera's transform.
    std::array<float, 16> camera = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                    0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    // The camera's space to clip space, with reversed depth: perspective or orthogonal.
    std::array<float, 16> projection = perspective_default();
    // Light every lit pixel gets, times its albedo: sRGB, times ambient_energy.
    std::array<float, 3> ambient{0.0F, 0.0F, 0.0F};
    float ambient_energy = 1.0F;
    // Up to kMaxDirectionalLights3D directional lights and kMaxClusteredLights3D omni and spot
    // lights, in any order.
    std::vector<Light3D> lights;
    RenderMethod method = RenderMethod::ForwardPlus;
    ShadowSettings shadows;
    PostProcess post;
    Sky3D sky; // drawn behind the meshes, over the clear color
    Fog3D fog;
    VolumetricFog3D volumetric_fog;
    Radiance3D radiance;
    Ssao3D ssao;
    Ssr3D ssr;
    Reflections3D reflections;
    Decals3D decals;
    // How meshes trace their VoxelGIs (Material3D::voxel_gi), as Godot's rendering/
    // global_illumination/voxel_gi/quality: four cones of 45 degrees for their ambient light, or
    // with high quality six narrower ones.
    bool voxel_gi_high_quality = false;
    // Temporal antialiasing, as Godot's use_taa (Forward+ only): the meshes are drawn with the
    // camera moved across a pixel by the next of 16 points each frame, and each frame is blended
    // with the target's frames before it, moved by how far its opaque meshes moved (draw_mesh's
    // previous transform) and the camera turned. A target's first frame with it is drawn as it is.
    bool use_taa = false;
    // FSR 2 upscaling, which takes TAA's place, as Godot turns TAA off with it. Only a scale under
    // 1 upscales with it; the canvas renderer scales the frame bilinearly otherwise.
    std::optional<Fsr2Scaling> fsr2{}; // NOLINT(readability-redundant-member-init)
    // Set for a frame that draws a face of a reflection probe (ProbeCapture).
    std::optional<ProbeCapture> capture{}; // NOLINT(readability-redundant-member-init)

    // Godot's default Camera3D: 75 degrees across a square target, from 0.05 to 4000.
    static std::array<float, 16> perspective_default();
};

// Godot's Projection::set_perspective, with reversed depth: fov_y degrees across the target's
// height, aspect its width over its height, from near to far.
[[nodiscard]] std::array<float, 16> perspective(float fov_y, float aspect, float near, float far);
// Godot's Projection::set_orthogonal, with reversed depth: size across the target's height.
[[nodiscard]] std::array<float, 16> orthogonal(float size, float aspect, float near, float far);
// The world to the space of a camera at eye looking at target, with up as near its up as it can
// be: the inverse of Godot's Transform3D::looking_at.
[[nodiscard]] std::array<float, 16> look_at(const std::array<float, 3>& eye,
                                            const std::array<float, 3>& target,
                                            const std::array<float, 3>& up);

} // namespace slash::render
