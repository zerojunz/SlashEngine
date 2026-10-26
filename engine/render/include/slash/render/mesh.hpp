#pragma once

#include "slash/core/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
};

// The most corners and indices a mesh may have.
inline constexpr std::uint32_t kMaxMeshVertices = 1U << 24U;
inline constexpr std::uint32_t kMaxMeshIndices = 1U << 26U;
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
    Radiance3D radiance;

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
