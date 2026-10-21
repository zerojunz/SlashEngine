#pragma once

#include "slash/core/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

// The most corners and indices a mesh may have, and the most lights a frame's scene may have.
inline constexpr std::uint32_t kMaxMeshVertices = 1U << 24U;
inline constexpr std::uint32_t kMaxMeshIndices = 1U << 26U;
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

// What happens to the 3D world's colors after it is lit, as Godot's Environment's tonemap, glow
// and adjustments in its Compatibility renderer. Without glow or adjustments, meshes tonemap as
// they draw. With either, they draw untonemapped into a texture of the target's size, glow blurs
// its bright parts into four textures each half the size of the last, and a pass over the target
// blends the glow in, tonemaps, then adjusts brightness, contrast and saturation and corrects
// colors, before the frame's 2D draws go over it. The textures are 8 bits a channel, as the
// Compatibility renderer's are, so glow comes from colors above glow_hdr_threshold only when it
// is below 1, or from glow_bloom.
struct PostProcess {
    Tonemapper tonemapper = Tonemapper::Linear;
    float exposure = 1.0F; // multiplies the lit colors before they are tonemapped
    float white = 1.0F;    // the color tonemapped to white; AgX ignores it
    bool glow = false;
    float glow_intensity = 0.8F;
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

    bool operator==(const PostProcess&) const = default;
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
    std::vector<Light3D> lights; // up to kMaxLights3D
    ShadowSettings shadows;
    PostProcess post;

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
