#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <map>
#include <string>
#include <vector>

// Components every SlashEngine world understands. Names follow Godot (Node2D and Node3D carry
// the same transforms).
namespace slash::world {

class ComponentRegistry;

// Position, rotation and scale in 2D. Units are pixels, and y points down, as in Godot. An
// entity listed in another's children is placed in its parent's space, as Godot's Node2D: its
// global transform is its parent's global transform, then its own (see tree2d.hpp).
struct Transform2D {
    Vec2 position;
    float rotation = 0.0F; // radians, clockwise
    Vec2 scale{1.0F, 1.0F};
    float skew = 0.0F; // radians the y axis leans from square to the x axis, as Godot's skew
    // Placed in the world, not its parent's space, and drawn after its parent's tree without its
    // parent's tint or z_index, as Godot's top_level; a hidden parent still hides it.
    bool top_level = false;
    std::vector<entt::entity> children; // entities placed in its space, in drawing order
};
SLASH_COMPONENT(Transform2D, position, rotation, scale, skew, top_level, children)

// Position, rotation and scale in 3D, as Godot's Node3D. Units are meters, and y points up. An
// entity listed in another's children is placed in its parent's space (see tree3d.hpp).
struct Transform3D {
    Vec3 position;
    // Radians about each axis, applied in Godot's default rotation_order (YXZ): z, then x, then y.
    Vec3 rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};
    bool visible = true; // false hides it and the entities under it
    // Placed in the world, not its parent's space, as Godot's top_level; a hidden parent still
    // hides it.
    bool top_level = false;
    std::vector<entt::entity> children; // entities placed in its space

    friend bool operator==(const Transform3D&, const Transform3D&) = default;
};
SLASH_COMPONENT(Transform3D, position, rotation, scale, visible, top_level, children)

// ---- 3D drawing, as Godot's 3D nodes (engine/scene3d) ----

// Draws the entity's mesh at its Transform3D, as Godot's MeshInstance3D: its primitive mesh
// component (BoxMesh, SphereMesh and the rest), colored by its StandardMaterial3D, or Godot's
// default material without one; or a model file, with its own materials.
struct MeshInstance3D {
    // A .gltf, .glb, .obj, .fbx or .blend file's meshes, placed as its nodes place them, in place
    // of a primitive mesh component; "" for none. A StandardMaterial3D on the entity replaces the
    // file's materials, as Godot's material_override.
    std::string mesh;
    // The render layers it is on: cameras whose cull_mask and lights whose light_cull_mask share
    // a bit with them see it and light it.
    std::uint32_t layers = 1;
    // Which lights' shadows it casts, as Godot's GeometryInstance3D.cast_shadow: "off", "on",
    // "double_sided" (with no faces culled) or "shadows_only" (it casts but isn't seen).
    std::string cast_shadow = "on";

    friend bool operator==(const MeshInstance3D&, const MeshInstance3D&) = default;
};
SLASH_COMPONENT(MeshInstance3D, mesh, layers, cast_shadow)

// Godot's BoxMesh: a box of size centered on the entity, each face cut into subdivisions + 1
// strips along each axis.
struct BoxMesh {
    Vec3 size{1.0F, 1.0F, 1.0F};
    int subdivide_width = 0;
    int subdivide_height = 0;
    int subdivide_depth = 0;
    bool flip_faces = false; // its inside shows, as Godot's PrimitiveMesh.flip_faces

    friend bool operator==(const BoxMesh&, const BoxMesh&) = default;
};
SLASH_COMPONENT(BoxMesh, size, subdivide_width, subdivide_height, subdivide_depth, flip_faces)

// Godot's SphereMesh: an ellipsoid of radius around y and height along it, or its top half.
struct SphereMesh {
    float radius = 0.5F;
    float height = 1.0F;
    int radial_segments = 64;
    int rings = 32;
    bool is_hemisphere = false;
    bool flip_faces = false;

    friend bool operator==(const SphereMesh&, const SphereMesh&) = default;
};
SLASH_COMPONENT(SphereMesh, radius, height, radial_segments, rings, is_hemisphere, flip_faces)

// Godot's CylinderMesh: a cylinder or cone along y, with caps.
struct CylinderMesh {
    float top_radius = 0.5F;
    float bottom_radius = 0.5F;
    float height = 2.0F;
    int radial_segments = 64;
    int rings = 4;
    bool cap_top = true;
    bool cap_bottom = true;
    bool flip_faces = false;

    friend bool operator==(const CylinderMesh&, const CylinderMesh&) = default;
};
SLASH_COMPONENT(CylinderMesh, top_radius, bottom_radius, height, radial_segments, rings, cap_top,
                cap_bottom, flip_faces)

// Godot's CapsuleMesh: a cylinder along y with half spheres on its ends, height end to end.
struct CapsuleMesh {
    float radius = 0.5F;
    float height = 2.0F;
    int radial_segments = 64;
    int rings = 8;
    bool flip_faces = false;

    friend bool operator==(const CapsuleMesh&, const CapsuleMesh&) = default;
};
SLASH_COMPONENT(CapsuleMesh, radius, height, radial_segments, rings, flip_faces)

// Godot's PlaneMesh: a flat rectangle facing up y (or x or z, by orientation).
struct PlaneMesh {
    Vec2 size{2.0F, 2.0F};
    int subdivide_width = 0;
    int subdivide_depth = 0;
    Vec3 center_offset;
    std::string orientation = "y"; // the axis it faces: "x", "y" or "z"
    bool flip_faces = false;

    friend bool operator==(const PlaneMesh&, const PlaneMesh&) = default;
};
SLASH_COMPONENT(PlaneMesh, size, subdivide_width, subdivide_depth, center_offset, orientation,
                flip_faces)

// Godot's QuadMesh: a PlaneMesh that faces z and is 1 by 1.
struct QuadMesh {
    Vec2 size{1.0F, 1.0F};
    int subdivide_width = 0;
    int subdivide_depth = 0;
    Vec3 center_offset;
    std::string orientation = "z";
    bool flip_faces = false;

    friend bool operator==(const QuadMesh&, const QuadMesh&) = default;
};
SLASH_COMPONENT(QuadMesh, size, subdivide_width, subdivide_depth, center_offset, orientation,
                flip_faces)

// Godot's PrismMesh: a wedge of size whose top edge sits left_to_right of the way across.
struct PrismMesh {
    float left_to_right = 0.5F;
    Vec3 size{1.0F, 1.0F, 1.0F};
    int subdivide_width = 0;
    int subdivide_height = 0;
    int subdivide_depth = 0;
    bool flip_faces = false;

    friend bool operator==(const PrismMesh&, const PrismMesh&) = default;
};
SLASH_COMPONENT(PrismMesh, left_to_right, size, subdivide_width, subdivide_height, subdivide_depth,
                flip_faces)

// Godot's TorusMesh: a ring around y between inner_radius and outer_radius.
struct TorusMesh {
    float inner_radius = 0.5F;
    float outer_radius = 1.0F;
    int rings = 64;
    int ring_segments = 32;
    bool flip_faces = false;

    friend bool operator==(const TorusMesh&, const TorusMesh&) = default;
};
SLASH_COMPONENT(TorusMesh, inner_radius, outer_radius, rings, ring_segments, flip_faces)

// What the entity's mesh is made of, as Godot's StandardMaterial3D set as its material_override.
// Colors are sRGB. Textures are project paths of PNG, BMP or KTX2 files; "" leaves one out.
struct StandardMaterial3D {
    std::string transparency = "disabled"; // "disabled", "alpha" or "alpha_scissor"
    float alpha_scissor_threshold = 0.5F;
    std::string blend_mode = "mix"; // how an alpha mesh blends: "mix", "add", "sub" or "mul"
    std::string cull_mode = "back"; // "back", "front" or "disabled"
    std::string shading_mode = "per_pixel"; // "per_pixel" or "unshaded"
    bool vertex_color_use_as_albedo = false;
    Vec4 albedo_color{1.0F, 1.0F, 1.0F, 1.0F};
    std::string albedo_texture;
    float metallic = 0.0F;
    float metallic_specular = 0.5F;
    std::string metallic_texture;
    std::string metallic_texture_channel = "red"; // "red", "green", "blue", "alpha" or "gray"
    float roughness = 1.0F;
    std::string roughness_texture;
    std::string roughness_texture_channel = "red";
    bool emission_enabled = false;
    Vec4 emission{0.0F, 0.0F, 0.0F, 1.0F};
    float emission_energy_multiplier = 1.0F;
    std::string emission_texture;
    bool normal_enabled = false;
    float normal_scale = 1.0F;
    std::string normal_texture;
    Vec3 uv1_scale{1.0F, 1.0F, 1.0F};
    Vec3 uv1_offset;
    // "nearest", "linear", "nearest_with_mipmaps" or "linear_with_mipmaps"
    std::string texture_filter = "linear_with_mipmaps";
    bool texture_repeat = true; // textures tile past 0..1; false stretches their edges
    bool disable_fog = false;   // the WorldEnvironment's fog leaves it alone

    friend bool operator==(const StandardMaterial3D&, const StandardMaterial3D&) = default;
};
SLASH_COMPONENT(StandardMaterial3D, transparency, alpha_scissor_threshold, blend_mode, cull_mode,
                shading_mode, vertex_color_use_as_albedo, albedo_color, albedo_texture, metallic,
                metallic_specular, metallic_texture, metallic_texture_channel, roughness,
                roughness_texture, roughness_texture_channel, emission_enabled, emission,
                emission_energy_multiplier, emission_texture, normal_enabled, normal_scale,
                normal_texture, uv1_scale, uv1_offset, texture_filter, texture_repeat, disable_fog)

// What the 3D world is seen through, as Godot's Camera3D: it looks down its entity's -z. The
// world is seen through the current camera: the first with current set, else the first one,
// in tree order (tree3d.hpp).
struct Camera3D {
    std::string projection = "perspective"; // "perspective" or "orthogonal"
    std::string keep_aspect = "height";     // the axis fov or size spans: "height" or "width"
    float fov = 75.0F;                      // degrees, perspective
    float size = 1.0F;                      // meters, orthogonal
    float near = 0.05F;
    float far = 4000.0F;
    float h_offset = 0.0F; // moves the view along its x, in meters
    float v_offset = 0.0F; // and its y
    bool current = false;
    std::uint32_t cull_mask = 0xFFFFFU; // the render layers it sees

    friend bool operator==(const Camera3D&, const Camera3D&) = default;
};
SLASH_COMPONENT(Camera3D, projection, keep_aspect, fov, size, near, far, h_offset, v_offset,
                current, cull_mask)

// Light from one direction over the whole world, as Godot's DirectionalLight3D, such as the sun:
// it shines down its entity's -z.
struct DirectionalLight3D {
    Vec4 light_color{1.0F, 1.0F, 1.0F, 1.0F};
    float light_energy = 1.0F;
    float light_specular = 0.5F;
    bool light_negative = false;                 // takes light away instead
    std::uint32_t light_cull_mask = 0xFFFFFFFFU; // the render layers it lights
    // Shadows, as Godot's Light3D: what pushes depths away from the light and moves pixels along
    // their normals against acne, whether shadow maps draw the faces meshes cull instead, how
    // dark the shadow is, and the render layers that cast it.
    bool shadow_enabled = false;
    float shadow_bias = 0.1F;
    float shadow_normal_bias = 2.0F;
    bool shadow_reverse_cull_face = false;
    float shadow_opacity = 1.0F;
    std::uint32_t shadow_caster_mask = 0xFFFFFFFFU;
    // How its shadow map covers the view: "orthogonal" in one piece, or "parallel_2_splits" and
    // "parallel_4_splits" in cascades, which split where the splits say, as fractions of
    // max_distance (or the camera's far plane, if nearer). Shadows fade out from fade_start of
    // that distance, neighboring cascades blend when blend_splits is set, and casters up to
    // pancake_size behind the view still cast.
    std::string directional_shadow_mode = "parallel_4_splits";
    float directional_shadow_split_1 = 0.1F;
    float directional_shadow_split_2 = 0.2F;
    float directional_shadow_split_3 = 0.5F;
    bool directional_shadow_blend_splits = false;
    float directional_shadow_fade_start = 0.8F;
    float directional_shadow_max_distance = 100.0F;
    float directional_shadow_pancake_size = 20.0F;
    // Its sun in a WorldEnvironment's sky: degrees across its radius there, and whether it lights
    // meshes and shows in the sky ("light_and_sky"), only lights ("light_only") or only shows
    // ("sky_only").
    float light_angular_distance = 0.0F;
    std::string sky_mode = "light_and_sky";

    friend bool operator==(const DirectionalLight3D&, const DirectionalLight3D&) = default;
};
SLASH_COMPONENT(DirectionalLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, shadow_enabled, shadow_bias, shadow_normal_bias,
                shadow_reverse_cull_face, shadow_opacity, shadow_caster_mask,
                directional_shadow_mode, directional_shadow_split_1, directional_shadow_split_2,
                directional_shadow_split_3, directional_shadow_blend_splits,
                directional_shadow_fade_start, directional_shadow_max_distance,
                directional_shadow_pancake_size, light_angular_distance, sky_mode)

// Light from a point in every direction, as Godot's OmniLight3D, such as a lamp.
struct OmniLight3D {
    Vec4 light_color{1.0F, 1.0F, 1.0F, 1.0F};
    float light_energy = 1.0F;
    float light_specular = 0.5F;
    bool light_negative = false;
    std::uint32_t light_cull_mask = 0xFFFFFFFFU;
    float omni_range = 5.0F;       // meters
    float omni_attenuation = 1.0F; // how it falls off with distance
    // Shadows, as Godot's Light3D: what pushes depths away from the light and moves pixels along
    // their normals against acne, whether shadow maps draw the faces meshes cull instead, how
    // dark the shadow is, and the render layers that cast it.
    bool shadow_enabled = false;
    float shadow_bias = 0.1F;
    float shadow_normal_bias = 1.0F;
    bool shadow_reverse_cull_face = false;
    float shadow_opacity = 1.0F;
    std::uint32_t shadow_caster_mask = 0xFFFFFFFFU;

    friend bool operator==(const OmniLight3D&, const OmniLight3D&) = default;
};
SLASH_COMPONENT(OmniLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, omni_range, omni_attenuation, shadow_enabled, shadow_bias,
                shadow_normal_bias, shadow_reverse_cull_face, shadow_opacity, shadow_caster_mask)

// Light from a point in a cone down its entity's -z, as Godot's SpotLight3D, such as a torch.
struct SpotLight3D {
    Vec4 light_color{1.0F, 1.0F, 1.0F, 1.0F};
    float light_energy = 1.0F;
    float light_specular = 0.5F;
    bool light_negative = false;
    std::uint32_t light_cull_mask = 0xFFFFFFFFU;
    float spot_range = 5.0F;
    float spot_attenuation = 1.0F;
    float spot_angle = 45.0F; // degrees from its axis to the cone's edge
    float spot_angle_attenuation = 1.0F;
    // Shadows, as Godot's Light3D: what pushes depths away from the light and moves pixels along
    // their normals against acne, whether shadow maps draw the faces meshes cull instead, how
    // dark the shadow is, and the render layers that cast it.
    bool shadow_enabled = false;
    float shadow_bias = 0.03F;
    float shadow_normal_bias = 1.0F;
    bool shadow_reverse_cull_face = false;
    float shadow_opacity = 1.0F;
    std::uint32_t shadow_caster_mask = 0xFFFFFFFFU;

    friend bool operator==(const SpotLight3D&, const SpotLight3D&) = default;
};
SLASH_COMPONENT(SpotLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, spot_range, spot_attenuation, spot_angle, spot_angle_attenuation,
                shadow_enabled, shadow_bias, shadow_normal_bias, shadow_reverse_cull_face,
                shadow_opacity, shadow_caster_mask)

// Godot's ProceduralSkyMaterial, on a WorldEnvironment's entity: a sky from its top through the
// horizon to the ground's bottom, with each DirectionalLight3D's sun on it. Colors are sRGB;
// sky_cover is the project path of a panorama over the sky, or "".
struct ProceduralSkyMaterial {
    Vec4 sky_top_color{0.385F, 0.454F, 0.55F, 1.0F};
    Vec4 sky_horizon_color{0.6463F, 0.6558F, 0.6708F, 1.0F};
    float sky_curve = 0.15F;
    float sky_energy_multiplier = 1.0F;
    std::string sky_cover;
    Vec4 sky_cover_modulate{1.0F, 1.0F, 1.0F, 1.0F};
    Vec4 ground_bottom_color{0.2F, 0.169F, 0.133F, 1.0F};
    Vec4 ground_horizon_color{0.6463F, 0.6558F, 0.6708F, 1.0F};
    float ground_curve = 0.02F;
    float ground_energy_multiplier = 1.0F;
    float sun_angle_max = 30.0F; // degrees
    float sun_curve = 0.15F;
    bool use_debanding = true;
    float energy_multiplier = 1.0F;

    friend bool operator==(const ProceduralSkyMaterial&, const ProceduralSkyMaterial&) = default;
};
SLASH_COMPONENT(ProceduralSkyMaterial, sky_top_color, sky_horizon_color, sky_curve,
                sky_energy_multiplier, sky_cover, sky_cover_modulate, ground_bottom_color,
                ground_horizon_color, ground_curve, ground_energy_multiplier, sun_angle_max,
                sun_curve, use_debanding, energy_multiplier)

// Godot's PhysicalSkyMaterial, on a WorldEnvironment's entity: the first DirectionalLight3D's
// light scattered through the air. Colors are sRGB; night_sky is the project path of a panorama
// behind it, or "".
struct PhysicalSkyMaterial {
    float rayleigh_coefficient = 2.0F;
    Vec4 rayleigh_color{0.3F, 0.405F, 0.6F, 1.0F};
    float mie_coefficient = 0.005F;
    float mie_eccentricity = 0.8F;
    Vec4 mie_color{0.69F, 0.729F, 0.812F, 1.0F};
    float turbidity = 10.0F;
    float sun_disk_scale = 1.0F;
    Vec4 ground_color{0.1F, 0.07F, 0.034F, 1.0F};
    float energy_multiplier = 1.0F;
    bool use_debanding = true;
    std::string night_sky;

    friend bool operator==(const PhysicalSkyMaterial&, const PhysicalSkyMaterial&) = default;
};
SLASH_COMPONENT(PhysicalSkyMaterial, rayleigh_coefficient, rayleigh_color, mie_coefficient,
                mie_eccentricity, mie_color, turbidity, sun_disk_scale, ground_color,
                energy_multiplier, use_debanding, night_sky)

// Godot's PanoramaSkyMaterial, on a WorldEnvironment's entity: an equirectangular texture all
// round, by its project path.
struct PanoramaSkyMaterial {
    std::string panorama;
    bool filter = true; // samples it smoothly rather than by its nearest texel
    float energy_multiplier = 1.0F;

    friend bool operator==(const PanoramaSkyMaterial&, const PanoramaSkyMaterial&) = default;
};
SLASH_COMPONENT(PanoramaSkyMaterial, panorama, filter, energy_multiplier)

// The 3D world's background, ambient light, tonemap, glow, fog and adjustments, as Godot's
// WorldEnvironment and its Environment in the Compatibility renderer. The first one made applies;
// without one the background is the project's clear color, there is no ambient light and colors
// are left as they are. A "sky" background is the sky material on the same entity: a
// ProceduralSkyMaterial, PhysicalSkyMaterial or PanoramaSkyMaterial, or Godot's default
// procedural sky with none. That sky also lights and is reflected in lit surfaces, by
// ambient_light_source and reflected_light_source, whatever the background.
struct WorldEnvironment {
    std::string background_mode = "clear_color"; // "clear_color", "color" or "sky"
    Vec4 background_color{0.0F, 0.0F, 0.0F, 1.0F};
    float background_energy_multiplier = 1.0F;
    float sky_custom_fov = 0.0F; // degrees the sky is seen across; 0 for the camera's
    Vec3 sky_rotation;           // radians about x, y and z, turned as Transform3D's rotation
    // Texels across each face of the cube the sky's light is drawn into, as Sky.radiance_size:
    // 32, 64, 128, 256, 512, 1024 or 2048.
    std::uint32_t sky_radiance_size = 256;
    // "background" (the background's color, or the sky's light), "disabled", "color"
    // (ambient_light_color) or "sky" (the sky's light, mixed with ambient_light_color)
    std::string ambient_light_source = "background";
    Vec4 ambient_light_color{0.0F, 0.0F, 0.0F, 1.0F};
    float ambient_light_sky_contribution = 1.0F; // how much of the sky's light, over the color
    float ambient_light_energy = 1.0F;
    // What lit surfaces reflect: "background" (the sky, if it is the background), "disabled" or
    // "sky"
    std::string reflected_light_source = "background";
    std::string tonemap_mode = "linear"; // "linear", "reinhard", "filmic", "aces" or "agx"
    float tonemap_exposure = 1.0F;
    float tonemap_white = 1.0F;
    bool glow_enabled = false;
    float glow_intensity = 0.8F;
    float glow_bloom = 0.0F;
    float glow_hdr_threshold = 1.0F;
    float glow_hdr_scale = 2.0F;
    float glow_hdr_luminance_cap = 12.0F;
    bool fog_enabled = false;
    std::string fog_mode = "exponential"; // "exponential" or "depth"
    Vec4 fog_light_color{0.518F, 0.553F, 0.608F, 1.0F};
    float fog_light_energy = 1.0F;
    float fog_sun_scatter = 0.0F;
    float fog_density = 0.01F;
    float fog_aerial_perspective = 0.0F;
    float fog_sky_affect = 1.0F;
    float fog_height = 0.0F;
    float fog_height_density = 0.0F;
    float fog_depth_curve = 1.0F;
    float fog_depth_begin = 10.0F;
    float fog_depth_end = 100.0F;
    bool adjustment_enabled = false;
    float adjustment_brightness = 1.0F;
    float adjustment_contrast = 1.0F;
    float adjustment_saturation = 1.0F;
    std::string adjustment_color_correction; // project path of a lookup texture; "" for none

    friend bool operator==(const WorldEnvironment&, const WorldEnvironment&) = default;
};
SLASH_COMPONENT(WorldEnvironment, background_mode, background_color, background_energy_multiplier,
                sky_custom_fov, sky_rotation, sky_radiance_size, ambient_light_source,
                ambient_light_color, ambient_light_sky_contribution, ambient_light_energy,
                reflected_light_source, tonemap_mode, tonemap_exposure, tonemap_white, glow_enabled,
                glow_intensity, glow_bloom, glow_hdr_threshold, glow_hdr_scale,
                glow_hdr_luminance_cap, fog_enabled, fog_mode, fog_light_color, fog_light_energy,
                fog_sun_scatter, fog_density, fog_aerial_perspective, fog_sky_affect, fog_height,
                fog_height_density, fog_depth_curve, fog_depth_begin, fog_depth_end,
                adjustment_enabled, adjustment_brightness, adjustment_contrast,
                adjustment_saturation, adjustment_color_correction)

// Ties an entity to the storyboard entries it stands for, such as the character it plays or the
// place it is, so a brief for the scene includes them and checks catch a misspelled ID.
struct StoryRef {
    std::vector<std::string> ids;
};
SLASH_COMPONENT(StoryRef, ids)

// A texture drawn at the entity's Transform2D, as Godot's Sprite2D. A texture can be a sprite
// sheet of hframes x vframes equal frames, of which frame is drawn, and region picks a part of
// it first. Sprites draw in order of z_index, then in the order their entities were made. 2D
// lights shade them, with normal and specular maps laid out as the texture, as a CanvasTexture's
// in Godot.
struct Sprite2D {
    std::string texture; // project path of a PNG, BMP or KTX2 file; "" draws nothing
    bool centered = true;
    Vec2 offset;
    bool flip_h = false;
    bool flip_v = false;
    int hframes = 1;
    int vframes = 1;
    int frame = 0;
    Vec4 region; // [x, y, width, height] in pixels; a zero size uses the whole texture
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    int z_index = 0;
    bool z_as_relative = true; // z_index adds to its parents'; false makes it the whole of it
    bool visible = true;
    std::string texture_filter = "inherit"; // "inherit", "linear" or "nearest"
    std::string material;                   // project path of a .material.json file; "" for none
    std::map<std::string, json::Value> material_params; // values for the material's params
    int light_mask = 1;                                 // lit by lights sharing a bit
    std::string normal_texture;   // project path of a normal map; "" faces the viewer
    std::string specular_texture; // project path of a specular map; "" has no highlights
    Vec4 specular_color{1.0F, 1.0F, 1.0F, 1.0F};
    float specular_shininess = 1.0F;
};
SLASH_COMPONENT(Sprite2D, texture, centered, offset, flip_h, flip_v, hframes, vframes, frame,
                region, modulate, z_index, z_as_relative, visible, texture_filter, material,
                material_params, light_mask, normal_texture, specular_texture, specular_color,
                specular_shininess)

// Plays animations from a .frames.json file (Godot's SpriteFrames) on the entity's Sprite2D,
// as Godot's AnimatedSprite2D: each tick it sets the sprite's texture, sheet and frame.
struct AnimatedSprite2D {
    std::string sprite_frames; // project path of a .frames.json file
    std::string animation = "default";
    bool playing = true;
    float speed_scale = 1.0F;
    int frame = 0;               // the animation's current frame
    float frame_progress = 0.0F; // how far through the current frame, from 0 to 1
};
SLASH_COMPONENT(AnimatedSprite2D, sprite_frames, animation, playing, speed_scale, frame,
                frame_progress)

// Plays keyframe animations from a .anim.json file, as Godot's AnimationPlayer: each tick it
// moves the time on and sets every field the animation's tracks name, on the entity's own
// components or, for a track with a target, on the entity targets maps that name to.
struct AnimationPlayer {
    std::string library; // project path of a .anim.json file
    std::string animation;
    bool playing = true;
    float speed_scale = 1.0F;
    double time = 0.0;      // seconds into the animation
    bool backwards = false; // play toward the start; pingpong loops flip it at each end
    // The entities tracks' targets stand for, by target name, as Godot's node paths from the
    // root node; null leaves a target's tracks unplayed.
    std::map<std::string, entt::entity> targets;
};
SLASH_COMPONENT(AnimationPlayer, library, animation, playing, speed_scale, time, backwards, targets)

// A grid of tiles from a .tileset.json file, as Godot's TileMapLayer, drawn at the entity's
// Transform2D with cell [0, 0]'s top left corner there. Its cells are text: each row is a string
// of keys, each key standing for the tile the legend names, and "." for an empty cell. The
// tilemap commands keep them in canonical form, so most edits go through them.
struct TileMapLayer {
    std::string tile_set; // project path of a .tileset.json file
    Vec2i origin;         // the cell the first key of the first row stands for
    // key -> tile: a tile's name or "source:x,y", then ":alternative" and ":turns" if any
    std::map<std::string, std::string> legend;
    std::vector<std::string> rows;
    int z_index = 0;
    bool z_as_relative = true; // as Sprite2D's
    bool visible = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    std::string texture_filter = "inherit"; // "linear", "nearest" or "inherit"
    int light_mask = 1;                     // lit by lights sharing a bit
    bool collision_enabled = true;          // its tiles' collision polygons are solid
    bool occlusion_enabled = true;          // its tiles' occluders cast shadows
};
SLASH_COMPONENT(TileMapLayer, tile_set, origin, legend, rows, z_index, z_as_relative, visible,
                modulate, texture_filter, light_mask, collision_enabled, occlusion_enabled)

// A light shining from the entity's Transform2D (or the origin) through a texture, as Godot's
// PointLight2D. It lights the sprites and tile map layers whose light_mask shares a bit with
// range_item_cull_mask, whose z_index is in its range and whose canvas's layer is in its layer
// range, and with shadow_enabled, the LightOccluder2D polygons whose occluder_light_mask shares a
// bit with shadow_item_cull_mask cast shadows from it, as do tiles' occluders whose occlusion
// layer's light_mask does. In a CanvasLayer, it shines from where its canvas puts it on the
// screen.
struct PointLight2D {
    bool enabled = true;
    std::string texture; // project path of a PNG, BMP or KTX2 file; "" is a soft round light
    float texture_scale = 1.0F;
    Vec2 offset; // moves the texture from the light, in pixels
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float energy = 1.0F;
    std::string blend_mode = "add"; // "add", "sub" or "mix"
    float height = 0.0F;            // pixels above the plane, for normal maps
    int range_z_min = -1024;
    int range_z_max = 1024;
    int range_layer_min = 0; // the canvas layers it lights: the world is 0, a CanvasLayer its layer
    int range_layer_max = 0;
    int range_item_cull_mask = 1;
    bool shadow_enabled = false;
    Vec4 shadow_color{0.0F, 0.0F, 0.0F, 0.0F};
    std::string shadow_filter = "none"; // "none", "pcf5" or "pcf13"
    float shadow_filter_smooth = 0.0F;
    int shadow_item_cull_mask = 1;
};
SLASH_COMPONENT(PointLight2D, enabled, texture, texture_scale, offset, color, energy, blend_mode,
                height, range_z_min, range_z_max, range_layer_min, range_layer_max,
                range_item_cull_mask, shadow_enabled, shadow_color, shadow_filter,
                shadow_filter_smooth, shadow_item_cull_mask)

// Light from one direction over everything, as Godot's DirectionalLight2D, such as the sun or the
// moon. It shines down the screen, turned by the entity's Transform2D rotation if it has one.
// Masks, ranges and shadows work as PointLight2D's.
struct DirectionalLight2D {
    bool enabled = true;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float energy = 1.0F;
    std::string blend_mode = "add"; // "add", "sub" or "mix"
    float height = 0.0F;            // from 0, along the plane, to 1, straight down, for normal maps
    float max_distance = 10000.0F;  // how far shadows reach past their occluders, in pixels
    int range_z_min = -1024;
    int range_z_max = 1024;
    int range_layer_min = 0; // the canvas layers it lights: the world is 0, a CanvasLayer its layer
    int range_layer_max = 0;
    int range_item_cull_mask = 1;
    bool shadow_enabled = false;
    Vec4 shadow_color{0.0F, 0.0F, 0.0F, 0.0F};
    std::string shadow_filter = "none"; // "none", "pcf5" or "pcf13"
    float shadow_filter_smooth = 0.0F;
    int shadow_item_cull_mask = 1;
};
SLASH_COMPONENT(DirectionalLight2D, enabled, color, energy, blend_mode, height, max_distance,
                range_z_min, range_z_max, range_layer_min, range_layer_max, range_item_cull_mask,
                shadow_enabled, shadow_color, shadow_filter, shadow_filter_smooth,
                shadow_item_cull_mask)

// A polygon that casts shadows from 2D lights, as Godot's LightOccluder2D with its
// OccluderPolygon2D, placed by the entity's Transform2D if it has one.
struct LightOccluder2D {
    std::vector<Vec2> polygon;          // points in pixels
    bool closed = true;                 // the last point joins the first
    std::string cull_mode = "disabled"; // "disabled", "clockwise" or "counter_clockwise"
    bool sdf_collision = true;          // GPU particles collide with it
    int occluder_light_mask = 1;
};
SLASH_COMPONENT(LightOccluder2D, polygon, closed, cull_mode, sdf_collision, occluder_light_mask)

// Tints everything lit in its canvas, as Godot's CanvasModulate: the light the world, or a
// CanvasLayer it is in, has where no light reaches. One per canvas.
struct CanvasModulate {
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(CanvasModulate, color)

// The part of the 2D world the window shows, as Godot's Camera2D, from where its Transform2D
// places it (see camera2d.hpp). The first enabled one in drawing order is current; with none,
// the window shows world pixels from [0, 0]. Controls stay put on the screen, as in a Godot
// CanvasLayer.
struct Camera2D {
    bool enabled = true;
    std::string anchor_mode = "drag_center"; // or "fixed_top_left"
    bool ignore_rotation = true;
    Vec2 offset;
    Vec2 zoom{1.0F, 1.0F};
    bool limit_enabled = true;
    int limit_left = -10000000;
    int limit_top = -10000000;
    int limit_right = 10000000;
    int limit_bottom = 10000000;
    bool limit_smoothed = false;
    bool position_smoothing_enabled = false;
    float position_smoothing_speed = 5.0F;
    bool rotation_smoothing_enabled = false;
    float rotation_smoothing_speed = 5.0F;
    bool drag_horizontal_enabled = false;
    bool drag_vertical_enabled = false;
    float drag_horizontal_offset = 0.0F;
    float drag_vertical_offset = 0.0F;
    float drag_left_margin = 0.2F;
    float drag_top_margin = 0.2F;
    float drag_right_margin = 0.2F;
    float drag_bottom_margin = 0.2F;
};
SLASH_COMPONENT(Camera2D, enabled, anchor_mode, ignore_rotation, offset, zoom, limit_enabled,
                limit_left, limit_top, limit_right, limit_bottom, limit_smoothed,
                position_smoothing_enabled, position_smoothing_speed, rotation_smoothing_enabled,
                rotation_smoothing_speed, drag_horizontal_enabled, drag_vertical_enabled,
                drag_horizontal_offset, drag_vertical_offset, drag_left_margin, drag_top_margin,
                drag_right_margin, drag_bottom_margin)

// A canvas of its own over or under the world, as Godot's CanvasLayer (see canvas_layer.hpp): the
// entities in its children, and theirs, draw in it, moved by its offset, rotation and scale
// instead of the camera, and controls in it lay out in the viewport. Canvases draw in order of
// layer; the world is layer 0, under any CanvasLayer of layer 0. Lights whose layer range holds
// its layer shade it, and a CanvasModulate in it tints only it.
struct CanvasLayer {
    std::vector<entt::entity> children;
    Vec2 offset;
    Vec2 scale{1.0F, 1.0F};
    float rotation = 0.0F;
    float follow_viewport_scale = 1.0F;
    int layer = 1;
    bool visible = true;
    // Seen through the camera too, as a layer of the world in front of it or behind it, scaled by
    // follow_viewport_scale around the middle of the screen.
    bool follow_viewport_enabled = false;
};
SLASH_COMPONENT(CanvasLayer, layer, visible, offset, rotation, scale, follow_viewport_enabled,
                follow_viewport_scale, children)

// Scrolls the entity's children at a different speed from the camera, as Godot's Parallax2D:
// each tick the "parallax_2d" system sets its Transform2D position from the camera's view, and
// with a repeat_size its children draw again every repeat_size, so a background goes on forever.
struct Parallax2D {
    Vec2 scroll_scale{1.0F, 1.0F}; // how fast it scrolls with the camera: 0 stays, 1 moves with it
    Vec2 scroll_offset;            // where it starts, in pixels
    Vec2 repeat_size;              // draw the children again every this many pixels; 0 for none
    Vec2 autoscroll;               // pixels a second scroll_offset moves, with a repeat_size
    Vec2 limit_begin{-10000000.0F, -10000000.0F};
    Vec2 limit_end{10000000.0F, 10000000.0F};
    // The camera's view, as Godot's Camera2D sets it: the middle of the screen in the world less
    // half the screen.
    Vec2 screen_offset;
    int repeat_times = 1; // copies drawn each way past the first, half of them before it
    // Off for one in a CanvasLayer that doesn't follow the viewport, so the camera doesn't move it.
    bool follow_viewport = true;
    bool ignore_camera_scroll = false; // leave screen_offset as it is
};
SLASH_COMPONENT(Parallax2D, scroll_scale, scroll_offset, repeat_size, autoscroll, repeat_times,
                limit_begin, limit_end, follow_viewport, ignore_camera_scroll, screen_offset)

// ---- 2D physics, as Godot's: a body component makes its entity a physics body, placed by its
// Transform2D, and the entity's CollisionShape2D and CollisionPolygon2D give it shapes. Units are
// pixels, seconds, radians and kilograms; layers and masks are Godot's 32 physics layers, and two
// objects collide when either one's collision_mask shares a bit with the other's collision_layer.

// A shape of the entity's body or area, as Godot's CollisionShape2D with its Shape2D, moved and
// turned from the entity's Transform2D by position and rotation, and scaled by its scale.
struct CollisionShape2D {
    std::string shape = "rectangle"; // "rectangle", "circle", "capsule", "segment" or "polygon"
    Vec2 size{20.0F, 20.0F};         // rectangle
    float radius = 10.0F;            // circle and capsule
    float height = 30.0F;            // capsule, end to end along y
    Vec2 a;                          // segment
    Vec2 b{0.0F, 10.0F};             // segment
    std::vector<Vec2> points;        // polygon: its convex hull is the shape
    Vec2 position;
    float rotation = 0.0F;
    bool disabled = false;
    bool one_way_collision = false; // collide only with what comes from the shape's -y side
    float one_way_collision_margin = 1.0F;

    friend bool operator==(const CollisionShape2D&, const CollisionShape2D&) = default;
};
SLASH_COMPONENT(CollisionShape2D, shape, size, radius, height, a, b, points, position, rotation,
                disabled, one_way_collision, one_way_collision_margin)

// A polygon shape of the entity's body or area, as Godot's CollisionPolygon2D: any simple
// polygon, filled ("solids", split into convex parts) or only its edges ("segments").
struct CollisionPolygon2D {
    std::vector<Vec2> polygon;         // points in pixels, from the entity
    std::string build_mode = "solids"; // "solids" or "segments"
    bool disabled = false;
    bool one_way_collision = false;
    float one_way_collision_margin = 1.0F;

    friend bool operator==(const CollisionPolygon2D&, const CollisionPolygon2D&) = default;
};
SLASH_COMPONENT(CollisionPolygon2D, polygon, build_mode, disabled, one_way_collision,
                one_way_collision_margin)

// A body that doesn't move on its own, such as a wall or the ground, as Godot's StaticBody2D.
// Changing its Transform2D moves it there at once. Its constant velocities don't move it but
// carry what touches it as if it moved, as a conveyor belt or a turntable.
struct StaticBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    Vec2 constant_linear_velocity;          // pixels per second
    float constant_angular_velocity = 0.0F; // radians per second, clockwise
};
SLASH_COMPONENT(StaticBody2D, collision_layer, collision_mask, friction, bounce,
                constant_linear_velocity, constant_angular_velocity)

// A body that goes where its Transform2D says, as Godot's AnimatableBody2D: moving platforms and
// doors that gameplay code or an AnimationPlayer moves. With sync_to_physics it moves there
// during the physics step, pushing bodies in its way and carrying those on it.
struct AnimatableBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    Vec2 constant_linear_velocity; // added to how it moves, as StaticBody2D's
    float constant_angular_velocity = 0.0F;
    bool sync_to_physics = true;
};
SLASH_COMPONENT(AnimatableBody2D, collision_layer, collision_mask, friction, bounce,
                constant_linear_velocity, constant_angular_velocity, sync_to_physics)

// A body that gravity, collisions, forces and joints move, as Godot's RigidBody2D. The physics
// step writes its Transform2D, velocities and sleeping back each tick.
struct RigidBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    float mass = 1.0F;
    float inertia = 0.0F; // 0 computes it from the shapes and the mass
    float gravity_scale = 1.0F;
    Vec2 linear_velocity;
    float angular_velocity = 0.0F;
    std::string linear_damp_mode = "combine"; // "combine" adds to the project's; "replace"
    float linear_damp = 0.0F;
    std::string angular_damp_mode = "combine";
    float angular_damp = 0.0F;
    Vec2 constant_force;
    float constant_torque = 0.0F;
    bool lock_rotation = false;
    bool freeze = false;
    std::string freeze_mode = "static";     // "static" or "kinematic"
    std::string continuous_cd = "disabled"; // "disabled", "cast_ray" or "cast_shape"
    bool can_sleep = true;
    bool sleeping = false;
    bool contact_monitor = false; // emit body_entered and body_exited
};
SLASH_COMPONENT(RigidBody2D, collision_layer, collision_mask, friction, bounce, mass, inertia,
                gravity_scale, linear_velocity, angular_velocity, linear_damp_mode, linear_damp,
                angular_damp_mode, angular_damp, constant_force, constant_torque, lock_rotation,
                freeze, freeze_mode, continuous_cd, can_sleep, sleeping, contact_monitor)

// A body that gameplay code moves by setting its velocity, as Godot's CharacterBody2D: each tick
// the physics step runs move_and_slide, sliding it along what it hits, then writes its
// Transform2D, velocity and what it touches. It moves as its CollisionShape2D (a polygon as its
// convex hull, with corners rounded by a pixel), else as its CollisionPolygon2D's convex hull.
struct CharacterBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::string motion_mode = "grounded"; // "grounded" (floors, walls, ceilings) or "floating"
    Vec2 up_direction{0.0F, -1.0F};
    Vec2 velocity;
    int max_slides = 4;
    bool slide_on_ceiling = true;
    float wall_min_slide_angle = 0.261799F; // radians; floating characters
    bool floor_stop_on_slope = true;
    bool floor_constant_speed = false;
    bool floor_block_on_wall = true;
    float floor_max_angle = 0.785398F; // radians from up_direction
    float floor_snap_length = 1.0F;
    std::string platform_on_leave = "add_velocity"; // or "add_upward_velocity", "do_nothing"
    std::uint32_t platform_floor_layers = 0xFFFFFFFFU;
    std::uint32_t platform_wall_layers = 0;
    float safe_margin = 0.08F; // pixels
    // What the last move found.
    bool on_floor = false;
    bool on_wall = false;
    bool on_ceiling = false;
    Vec2 floor_normal;
    Vec2 wall_normal;
    Vec2 platform_velocity; // of the moving body it stands on
};
SLASH_COMPONENT(CharacterBody2D, collision_layer, collision_mask, motion_mode, up_direction,
                velocity, max_slides, slide_on_ceiling, wall_min_slide_angle, floor_stop_on_slope,
                floor_constant_speed, floor_block_on_wall, floor_max_angle, floor_snap_length,
                platform_on_leave, platform_floor_layers, platform_wall_layers, safe_margin,
                on_floor, on_wall, on_ceiling, floor_normal, wall_normal, platform_velocity)

// A region that reports bodies and areas entering and leaving its shapes, as Godot's Area2D:
// body_entered, body_exited, area_entered and area_exited events. It goes where its Transform2D
// says and nothing collides with it. Its space overrides change the gravity and damping of the
// rigid bodies in it, as Godot's, going from the highest priority down: "combine" adds its own
// and lets lower areas and the project add theirs, "replace" uses only its own,
// "combine_replace" adds its own and stops there, and "replace_combine" drops what higher areas
// gave and lets lower ones add.
struct Area2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    bool monitoring = true;  // report what enters
    bool monitorable = true; // other areas see it
    int priority = 0;        // higher overrides first
    std::string gravity_space_override = "disabled";
    bool gravity_point = false;               // pull toward gravity_point_center
    float gravity_point_unit_distance = 0.0F; // pixels; above 0, weakens with distance squared
    Vec2 gravity_point_center{0.0F, 1.0F};    // in the area's space
    Vec2 gravity_direction{0.0F, 1.0F};
    float gravity = 980.0F; // pixels per second squared
    std::string linear_damp_space_override = "disabled";
    float linear_damp = 0.1F;
    std::string angular_damp_space_override = "disabled";
    float angular_damp = 1.0F;
    bool audio_bus_override = false;       // AudioStreamPlayer2D sounds in it play through:
    std::string audio_bus_name = "Master"; // this bus
};
SLASH_COMPONENT(Area2D, collision_layer, collision_mask, monitoring, monitorable, priority,
                gravity_space_override, gravity_point, gravity_point_unit_distance,
                gravity_point_center, gravity_direction, gravity, linear_damp_space_override,
                linear_damp, angular_damp_space_override, angular_damp, audio_bus_override,
                audio_bus_name)

// Pins two bodies together at the entity's Transform2D, letting them turn, as Godot's
// PinJoint2D. A missing node_a pins node_b to the world. Every joint is made where its entity is
// once both bodies exist and stays there when a parent carries the entity away, as Godot's.
struct PinJoint2D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool disable_collision = true;
    bool angular_limit_enabled = false;
    float angular_limit_lower = 0.0F;
    float angular_limit_upper = 0.0F;
    bool motor_enabled = false;
    float motor_target_velocity = 0.0F;

    friend bool operator==(const PinJoint2D&, const PinJoint2D&) = default;
};
SLASH_COMPONENT(PinJoint2D, node_a, node_b, disable_collision, angular_limit_enabled,
                angular_limit_lower, angular_limit_upper, motor_enabled, motor_target_velocity)

// A spring from node_a at the entity's Transform2D to node_b, length of the entity's pixels
// along its y axis (so its scale stretches it), as Godot's DampedSpringJoint2D.
struct DampedSpringJoint2D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool disable_collision = true;
    float length = 50.0F;
    float rest_length = 0.0F; // 0 rests at length
    float stiffness = 20.0F;
    float damping = 1.0F;

    friend bool operator==(const DampedSpringJoint2D&, const DampedSpringJoint2D&) = default;
};
SLASH_COMPONENT(DampedSpringJoint2D, node_a, node_b, disable_collision, length, rest_length,
                stiffness, damping)

// Keeps node_b on a groove of node_a's, from the entity's Transform2D length of its pixels along
// its y axis, as Godot's GrooveJoint2D; node_b slides along it and turns freely.
struct GrooveJoint2D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool disable_collision = true;
    float length = 50.0F;
    float initial_offset = 25.0F; // where node_b starts along the groove

    friend bool operator==(const GrooveJoint2D&, const GrooveJoint2D&) = default;
};
SLASH_COMPONENT(GrooveJoint2D, node_a, node_b, disable_collision, length, initial_offset)

// ---- Particles, as Godot's CPUParticles2D.

// A color at a point of a gradient, as one stop of Godot's Gradient.
struct GradientPoint {
    float offset = 0.0F; // from 0 to 1
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};

    friend bool operator==(const GradientPoint&, const GradientPoint&) = default;
};
SLASH_REFLECT(GradientPoint, offset, color)

// Emits and moves particles from the entity's Transform2D and draws them, as Godot's
// CPUParticles2D. Each particle takes a random value between a parameter's _min and _max when
// it is emitted, and the parameter's _curve, points [offset, value] over the particle's life
// from 0 to 1, multiplies it as it ages (no points: 1). Angles are in degrees, as Godot's
// properties. The particles themselves are not saved.
struct CPUParticles2D {
    // Fields are grouped by type so the struct packs tightly; SLASH_COMPONENT lists them as
    // Godot groups them.
    std::vector<Vec2> emission_points;
    std::vector<Vec2> emission_normals;
    std::vector<Vec4> emission_colors;
    std::vector<Vec2> angular_velocity_curve;
    std::vector<Vec2> orbit_velocity_curve;
    std::vector<Vec2> linear_accel_curve;
    std::vector<Vec2> radial_accel_curve;
    std::vector<Vec2> tangential_accel_curve;
    std::vector<Vec2> damping_curve;
    std::vector<Vec2> angle_curve;
    std::vector<Vec2> scale_amount_curve;
    std::vector<Vec2> hue_variation_curve;
    std::vector<Vec2> anim_speed_curve;
    std::vector<Vec2> anim_offset_curve;
    std::vector<GradientPoint> color_ramp;
    std::vector<GradientPoint> color_initial_ramp;
    std::string draw_order = "index"; // "index" or "lifetime"
    std::string texture;              // project path of a PNG, BMP or KTX2 file; "" draws pixels
    std::string texture_filter = "inherit";
    std::string material;
    // "point", "sphere", "sphere_surface", "rectangle", "ring", "points" or "directed_points"
    std::string emission_shape = "point";
    std::map<std::string, json::Value> material_params;
    int amount = 8;
    int seed = 0;
    int hframes = 1; // the texture's frames, as Godot's CanvasItemMaterial particles_anim_*
    int vframes = 1;
    int z_index = 0;
    bool z_as_relative = true; // as Sprite2D's
    int light_mask = 1;
    float lifetime = 1.0F;
    float preprocess = 0.0F;
    float speed_scale = 1.0F;
    float explosiveness = 0.0F;
    float randomness = 0.0F;
    float lifetime_randomness = 0.0F;
    float emission_sphere_radius = 1.0F;
    float emission_ring_radius = 1.0F;
    float emission_ring_inner_radius = 0.0F;
    float spread = 45.0F;
    float initial_velocity_min = 0.0F;
    float initial_velocity_max = 0.0F;
    float angular_velocity_min = 0.0F;
    float angular_velocity_max = 0.0F;
    float orbit_velocity_min = 0.0F;
    float orbit_velocity_max = 0.0F;
    float linear_accel_min = 0.0F;
    float linear_accel_max = 0.0F;
    float radial_accel_min = 0.0F;
    float radial_accel_max = 0.0F;
    float tangential_accel_min = 0.0F;
    float tangential_accel_max = 0.0F;
    float damping_min = 0.0F;
    float damping_max = 0.0F;
    float angle_min = 0.0F;
    float angle_max = 0.0F;
    float scale_amount_min = 1.0F;
    float scale_amount_max = 1.0F;
    float hue_variation_min = 0.0F;
    float hue_variation_max = 0.0F;
    float anim_speed_min = 0.0F;
    float anim_speed_max = 0.0F;
    float anim_offset_min = 0.0F;
    float anim_offset_max = 0.0F;
    Vec2 emission_rect_extents{1.0F, 1.0F};
    Vec2 direction{1.0F, 0.0F};
    Vec2 gravity{0.0F, 980.0F};
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    bool emitting = true;
    bool one_shot = false;
    bool local_coords = false;
    bool anim_loop = false;
    bool visible = true;
    bool particle_flag_align_y = false;

    friend bool operator==(const CPUParticles2D&, const CPUParticles2D&) = default;
};
SLASH_COMPONENT(CPUParticles2D, emitting, amount, lifetime, one_shot, preprocess, speed_scale,
                explosiveness, randomness, lifetime_randomness, seed, local_coords, draw_order,
                texture, texture_filter, hframes, vframes, anim_loop, z_index, z_as_relative,
                visible, material, material_params, light_mask, emission_shape,
                emission_sphere_radius, emission_rect_extents, emission_ring_radius,
                emission_ring_inner_radius, emission_points, emission_normals, emission_colors,
                particle_flag_align_y, direction, spread, gravity, initial_velocity_min,
                initial_velocity_max, angular_velocity_min, angular_velocity_max,
                angular_velocity_curve, orbit_velocity_min, orbit_velocity_max,
                orbit_velocity_curve, linear_accel_min, linear_accel_max, linear_accel_curve,
                radial_accel_min, radial_accel_max, radial_accel_curve, tangential_accel_min,
                tangential_accel_max, tangential_accel_curve, damping_min, damping_max,
                damping_curve, angle_min, angle_max, angle_curve, scale_amount_min,
                scale_amount_max, scale_amount_curve, color, color_ramp, color_initial_ramp,
                hue_variation_min, hue_variation_max, hue_variation_curve, anim_speed_min,
                anim_speed_max, anim_speed_curve, anim_offset_min, anim_offset_max,
                anim_offset_curve)

// Emits particles from the entity's Transform2D and moves them on the GPU, as Godot's
// GPUParticles2D: process_material names a .process_material.json file (Godot's
// ParticleProcessMaterial) that says how they start and move. The gpu_particles_2d system steps
// each emitter's emission cycle every tick; renderers run the steps on their device and draw the
// particles, the same on every device. The particles themselves are not saved.
struct GPUParticles2D {
    // Fields are grouped by type so the struct packs tightly; SLASH_COMPONENT lists them as
    // Godot groups them.
    std::string process_material; // project path of a .process_material.json file
    std::string texture;          // project path of a PNG, BMP or KTX2 file; "" draws pixels
    std::string texture_filter = "inherit";
    std::string material;
    std::string draw_order = "lifetime"; // "index", "lifetime" or "reverse_lifetime"
    std::map<std::string, json::Value> material_params;
    int amount = 8;
    int seed = 0;
    int fixed_fps = 30;
    int hframes = 1; // the texture's frames, as Godot's CanvasItemMaterial particles_anim_*
    int vframes = 1;
    int z_index = 0;
    bool z_as_relative = true; // as Sprite2D's
    int light_mask = 1;
    int trail_sections = 8; // a trail draws as a ribbon through this many sections
    int trail_section_subdivisions = 4;
    entt::entity sub_emitter{entt::null}; // the GPUParticles2D its particles start particles in
    float amount_ratio = 1.0F;
    float lifetime = 1.0F;
    float interp_to_end = 0.0F;
    float preprocess = 0.0F;
    float speed_scale = 1.0F;
    float explosiveness = 0.0F;
    float randomness = 0.0F;
    float collision_base_size = 1.0F;
    float trail_lifetime = 0.3F; // seconds of each particle's path its trail shows
    bool emitting = true;
    bool one_shot = false;
    bool use_fixed_seed = false;
    bool interpolate = true;
    bool fract_delta = true;
    bool local_coords = false;
    bool anim_loop = false;
    bool visible = true;
    bool trail_enabled = false;

    friend bool operator==(const GPUParticles2D&, const GPUParticles2D&) = default;
};
SLASH_COMPONENT(GPUParticles2D, emitting, amount, amount_ratio, sub_emitter, process_material,
                texture, lifetime, interp_to_end, one_shot, preprocess, speed_scale, explosiveness,
                randomness, use_fixed_seed, seed, fixed_fps, interpolate, fract_delta,
                collision_base_size, local_coords, draw_order, trail_enabled, trail_lifetime,
                trail_sections, trail_section_subdivisions, texture_filter, hframes, vframes,
                anim_loop, z_index, z_as_relative, visible, material, material_params, light_mask)

// ---- Audio, as Godot's AudioStreamPlayer nodes.

// Plays a sound through a bus, as Godot's AudioStreamPlayer: the same wherever the entity is.
// Setting playing starts or stops it, and the audio system sets it back to false when the sound
// ends, emitting finished.
struct AudioStreamPlayer {
    std::string stream; // project path of a .wav, .ogg, .mp3 or .audio.json file
    float volume_db = 0.0F;
    float pitch_scale = 1.0F;
    bool playing = false;
    bool autoplay = false;      // play once the audio system first sees it
    bool stream_paused = false; // hold the sound where it is
    int max_polyphony = 1;      // sounds it may play at once
    std::string bus = "Master";
    // The stream's parameters, as Godot's parameters/*: "switch_to_clip" for interactive music.
    std::map<std::string, json::Value> parameters;

    friend bool operator==(const AudioStreamPlayer&, const AudioStreamPlayer&) = default;
};
SLASH_COMPONENT(AudioStreamPlayer, stream, volume_db, pitch_scale, playing, autoplay, stream_paused,
                max_polyphony, bus, parameters)

// Plays a sound at the entity's Transform2D, as Godot's AudioStreamPlayer2D: quieter with
// distance from the listener and panned by where it is on screen.
struct AudioStreamPlayer2D {
    std::string stream; // project path of a .wav, .ogg, .mp3 or .audio.json file
    float volume_db = 0.0F;
    float pitch_scale = 1.0F;
    bool playing = false;
    bool autoplay = false;
    bool stream_paused = false;
    float max_distance = 2000.0F; // pixels; silent beyond
    float attenuation = 1.0F;     // how fast it fades with distance, as an easing curve
    int max_polyphony = 1;
    float panning_strength = 1.0F;
    std::string bus = "Master";
    std::uint32_t area_mask = 1; // Area2D layers whose audio_bus_override applies; 0 for none
    std::map<std::string, json::Value> parameters;

    friend bool operator==(const AudioStreamPlayer2D&, const AudioStreamPlayer2D&) = default;
};
SLASH_COMPONENT(AudioStreamPlayer2D, stream, volume_db, pitch_scale, playing, autoplay,
                stream_paused, max_distance, attenuation, max_polyphony, panning_strength, bus,
                area_mask, parameters)

// Where 2D sounds are heard from, as Godot's AudioListener2D, while current; with none, the
// middle of the screen.
struct AudioListener2D {
    bool current = false;

    friend bool operator==(const AudioListener2D&, const AudioListener2D&) = default;
};
SLASH_COMPONENT(AudioListener2D, current)

// Registers the components above, with their docs.
void register_builtin_components(ComponentRegistry& registry);

// ---- UI, as Godot's Control nodes.

// A UI element, as Godot's Control: a rect placed in its parent's by anchors and offsets, and
// children, which lay out in its rect and draw over it in order. Label, ColorRect and the other
// UI components draw in the rect; an entity with one of them and no Control lays out with
// Control's defaults. A control in no Control's children is placed in the viewport. Controls
// draw above sprites, tile maps and particles, as on a CanvasLayer in Godot.
struct Control {
    // Fields are grouped by type so the struct packs tightly; SLASH_COMPONENT lists them as
    // Godot groups them.
    std::vector<entt::entity> children;
    std::string grow_horizontal = "end"; // "begin", "end" or "both"
    std::string grow_vertical = "end";
    // How a container sizes and places it: "fill", "expand_fill", "shrink_center" and so on.
    std::string size_flags_horizontal = "fill";
    std::string size_flags_vertical = "fill";
    std::string texture_filter = "inherit"; // "inherit", "linear" or "nearest"
    std::string theme;                      // project path of a .theme.json file; "" for none
    std::string theme_type_variation;       // a theme type to look items up in first
    // Shown in a box beside the pointer once it rests on the control, as Godot's tooltip_text.
    std::string tooltip_text;
    // Whether the pointer stops at it ("stop"), goes on to its parent too ("pass") or passes it
    // by ("ignore"); "" takes its type's, as in Godot: ignore for Label and NinePatchRect, pass
    // for containers and TextureRect, stop for the rest.
    std::string mouse_filter;
    // Whether it takes focus from clicks and the keyboard ("all"), clicks only ("click") or
    // never ("none"); "" takes its type's: all for buttons, none for the rest.
    std::string focus_mode;
    // Which way it lays out: "inherited" (its parent's; left to right at the root), "ltr" or
    // "rtl", which mirrors its anchors, offsets and grow directions, its children's order in
    // containers and its text's alignment, as Godot's.
    std::string layout_direction = "inherited";
    std::map<std::string, Vec4> theme_override_colors;
    std::map<std::string, int> theme_override_constants;
    std::map<std::string, std::string> theme_override_fonts;
    std::map<std::string, int> theme_override_font_sizes;
    std::map<std::string, std::string> theme_override_icons;  // as in a theme
    std::map<std::string, json::Value> theme_override_styles; // style boxes, as in a theme
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    Vec4 self_modulate{1.0F, 1.0F, 1.0F, 1.0F};
    Vec2 custom_minimum_size;
    Vec2 scale{1.0F, 1.0F};
    Vec2 pivot_offset;
    float anchor_left = 0.0F; // from 0 (the parent's left edge) to 1 (its right edge)
    float anchor_top = 0.0F;
    float anchor_right = 0.0F;
    float anchor_bottom = 0.0F;
    float offset_left = 0.0F; // pixels from the anchor
    float offset_top = 0.0F;
    float offset_right = 0.0F;
    float offset_bottom = 0.0F;
    float rotation = 0.0F; // radians, around pivot_offset
    float size_flags_stretch_ratio = 1.0F;
    int z_index = 0;
    bool z_as_relative = true; // z_index adds to its parent's; false makes it the whole of it
    // Placed in the viewport instead of its parent's rect, as Godot's top_level: containers
    // leave it out, it takes none of its parent's transform, modulate, z_index or clip, and it
    // draws after its parent's tree, getting the pointer before it. Its parent still hides it.
    bool top_level = false;
    // The controls focus moves to with ui_left, ui_up, ui_right and ui_down, and with
    // ui_focus_next and ui_focus_prev (Tab and Shift+Tab); null finds the closest one that way.
    entt::entity focus_neighbor_left = entt::null;
    entt::entity focus_neighbor_top = entt::null;
    entt::entity focus_neighbor_right = entt::null;
    entt::entity focus_neighbor_bottom = entt::null;
    entt::entity focus_next = entt::null;
    entt::entity focus_previous = entt::null;
    bool visible = true;
    // Cut off its drawing and its children's at its rect, and let the pointer reach only the
    // part of its children inside it, as Godot's clip_contents. A ScrollContainer always does.
    bool clip_contents = false;

    friend bool operator==(const Control&, const Control&) = default;
};
SLASH_COMPONENT(Control, children, anchor_left, anchor_top, anchor_right, anchor_bottom,
                offset_left, offset_top, offset_right, offset_bottom, grow_horizontal,
                grow_vertical, clip_contents, custom_minimum_size, rotation, scale, pivot_offset,
                size_flags_horizontal, size_flags_vertical, size_flags_stretch_ratio,
                focus_neighbor_left, focus_neighbor_top, focus_neighbor_right,
                focus_neighbor_bottom, focus_next, focus_previous, focus_mode, mouse_filter,
                tooltip_text, layout_direction, visible, modulate, self_modulate, top_level,
                z_index, z_as_relative, texture_filter, theme, theme_type_variation,
                theme_override_colors, theme_override_constants, theme_override_fonts,
                theme_override_font_sizes, theme_override_icons, theme_override_styles)

// Lets a control be dragged and take what is dropped on it, as a Godot script's _get_drag_data,
// _can_drop_data and _drop_data. Moving the mouse past the drag threshold with the left button
// held on it starts a drag that carries drag_data; letting go over a control whose can_drop
// takes the data drops it there, which emits drop_data from that control (data {"data",
// "from", "position"}). The control dragged emits drag_begin and drag_end (data
// {"successful"}).
struct DragAndDrop {
    json::Value drag_data; // what a drag from it carries; null: it can't be dragged
    // What it takes: data matching one of these, where an object needs only the members given
    // and anything else must be equal; empty takes nothing.
    std::vector<json::Value> can_drop;
    // A control shown at the pointer while a drag from it goes on, even if hidden; it must have
    // no parent. null shows nothing.
    entt::entity drag_preview = entt::null;

    friend bool operator==(const DragAndDrop&, const DragAndDrop&) = default;
};
SLASH_COMPONENT(DragAndDrop, drag_data, can_drop, drag_preview)

// Text in a control's rect, as Godot's Label. Its font, size, colors, outline and shadow are
// theme items, set per label with Control's theme_override_* fields.
struct Label {
    std::string text;
    std::string horizontal_alignment = "left"; // "left", "center", "right" or "fill"
    std::string vertical_alignment = "top";    // "top", "center", "bottom" or "fill"
    std::string autowrap_mode = "off";         // "off", "arbitrary", "word" or "word_smart"
    // "no_trimming", "trim_char", "trim_word", "trim_ellipsis" or "trim_word_ellipsis"
    std::string text_overrun_behavior = "no_trimming";
    std::string ellipsis_char = "\u2026";
    std::string visible_characters_behavior = "chars_before_shaping";
    // The text's base direction: "auto" (from its first strong character), "ltr", "rtl" or
    // "inherited" (the control's layout direction).
    std::string text_direction = "auto";
    // BCP 47, such as "ar" or "zh-Hant"; "" for none.
    std::string language{}; // NOLINT(readability-redundant-member-init)
    int lines_skipped = 0;
    int max_lines_visible = -1;  // -1 for all
    int visible_characters = -1; // -1 for all
    bool clip_text = false;
    bool uppercase = false;

    friend bool operator==(const Label&, const Label&) = default;
};
SLASH_COMPONENT(Label, text, horizontal_alignment, vertical_alignment, autowrap_mode, clip_text,
                text_overrun_behavior, ellipsis_char, uppercase, lines_skipped, max_lines_visible,
                visible_characters, visible_characters_behavior, text_direction, language)

// A rect of one color, as Godot's ColorRect.
struct ColorRect {
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};

    friend bool operator==(const ColorRect&, const ColorRect&) = default;
};
SLASH_COMPONENT(ColorRect, color)

// Draws the theme's "panel" style box in the control's rect, as Godot's Panel.
struct Panel {
    friend bool operator==(const Panel&, const Panel&) = default;
};
SLASH_COMPONENT(Panel)

// A texture in the control's rect, as Godot's TextureRect.
struct TextureRect {
    std::string texture; // project path of a PNG, BMP or KTX2 file; "" draws nothing
    std::string expand_mode = "keep_size"; // how the texture sets the minimum size
    std::string stretch_mode = "scale";    // how the texture fills the rect
    bool flip_h = false;
    bool flip_v = false;

    friend bool operator==(const TextureRect&, const TextureRect&) = default;
};
SLASH_COMPONENT(TextureRect, texture, expand_mode, stretch_mode, flip_h, flip_v)

// A texture cut into nine parts that stretch to the control's rect without stretching its
// corners, as Godot's NinePatchRect.
struct NinePatchRect {
    std::string texture;
    std::string axis_stretch_horizontal = "stretch"; // "stretch", "tile" or "tile_fit"
    std::string axis_stretch_vertical = "stretch";
    Vec4 region_rect; // [x, y, width, height] in pixels; a zero size uses the whole texture
    int patch_margin_left = 0;
    int patch_margin_top = 0;
    int patch_margin_right = 0;
    int patch_margin_bottom = 0;
    bool draw_center = true;

    friend bool operator==(const NinePatchRect&, const NinePatchRect&) = default;
};
SLASH_COMPONENT(NinePatchRect, texture, draw_center, region_rect, patch_margin_left,
                patch_margin_top, patch_margin_right, patch_margin_bottom, axis_stretch_horizontal,
                axis_stretch_vertical)

// A bar filled to show a value between a minimum and a maximum, as Godot's ProgressBar.
struct ProgressBar {
    std::string fill_mode = "begin_to_end"; // "begin_to_end", "end_to_begin", "top_to_bottom"
                                            // or "bottom_to_top"
    double min_value = 0.0;
    double max_value = 100.0;
    double step = 0.01; // the value is rounded to a multiple of this; 0 for none
    double value = 0.0;
    bool allow_greater = false;
    bool allow_lesser = false;
    bool show_percentage = true;

    friend bool operator==(const ProgressBar&, const ProgressBar&) = default;
};
SLASH_COMPONENT(ProgressBar, min_value, max_value, step, value, allow_greater, allow_lesser,
                fill_mode, show_percentage)

// A grabber dragged along a track to pick a value between a minimum and a maximum, as Godot's
// HSlider, or VSlider when vertical. Clicking the track moves the grabber there, the mouse wheel
// steps it, and so do the ui_* actions while it has focus. It emits value_changed (data
// {"value"}), drag_started and drag_ended (data {"value_changed": bool}).
struct Slider {
    std::string ticks_position = "bottom_right"; // "bottom_right", "top_left", "both" or "center"
    double min_value = 0.0;
    double max_value = 100.0;
    double step = 1.0; // the value is rounded to a multiple of this from min_value; 0 for none
    double page = 0.0; // the value stays this far below max_value
    double value = 0.0;
    int tick_count = 0;
    bool rounded = false; // round the value to a whole number
    bool allow_greater = false;
    bool allow_lesser = false;
    bool editable = true;   // the pointer and keys move it
    bool scrollable = true; // the mouse wheel moves it
    bool ticks_on_borders = false;
    bool vertical = false;

    friend bool operator==(const Slider&, const Slider&) = default;
};
SLASH_COMPONENT(Slider, min_value, max_value, step, page, value, rounded, allow_greater,
                allow_lesser, editable, scrollable, tick_count, ticks_on_borders, ticks_position,
                vertical)

// A bar with a grabber that shows how far along a value is and moves it, as Godot's
// HScrollBar, or VScrollBar when vertical: the grabber's length is page's share of the range. A
// click before or after the grabber moves the value a page, dragging the grabber moves it with
// the pointer, and the wheel moves it an eighth of a page; with focus, the ui_* actions step it.
// It emits value_changed (data {"value"}) and scrolling, when the pointer or keys move it.
struct ScrollBar {
    double min_value = 0.0;
    double max_value = 100.0;
    double step = 0.0; // the value is rounded to a multiple of this from min_value; 0 for none
    double page = 0.0; // the part of the range in view: the grabber's length
    double value = 0.0;
    float custom_step = -1.0F; // what the ui_* actions and its arrows step by; -1 for step
    bool rounded = false;
    bool allow_greater = false;
    bool allow_lesser = false;
    bool vertical = false;

    friend bool operator==(const ScrollBar&, const ScrollBar&) = default;
};
SLASH_COMPONENT(ScrollBar, min_value, max_value, step, page, value, rounded, allow_greater,
                allow_lesser, custom_step, vertical)

// Buttons, as Godot's BaseButton: pressed by a click or tap released over them (or as the press
// starts, with action_mode "press"), by ui_accept while focused, or by a shortcut. They emit
// world events: pressed, toggled (data {"toggled_on": bool}), button_down and button_up.

// A button with text and an icon, as Godot's Button. Its style boxes, font colors and icon
// colors per state (normal, hover, pressed, disabled, focus) are theme items.
struct Button {
    std::vector<std::string> button_mask{"left"}; // mouse buttons that press it
    std::vector<json::Value> shortcut;            // bindings or actions that press it
    std::string text;
    std::string icon;                 // project path of a PNG, BMP or KTX2 file; "" for none
    std::string alignment = "center"; // the text's: "left", "center" or "right"
    std::string text_overrun_behavior = "no_trimming";
    std::string autowrap_mode = "off";
    std::string icon_alignment = "left";            // "left", "center" or "right"
    std::string vertical_icon_alignment = "center"; // "top", "center" or "bottom"
    std::string action_mode = "release";            // "press" or "release"
    std::string button_group; // toggle buttons with the same group name: at most one is pressed
    std::string text_direction = "auto"; // as Label's
    std::string language{};              // NOLINT(readability-redundant-member-init)
    bool flat = false;                   // draw no style box, only the text and icon
    bool clip_text = false;
    bool expand_icon = false;
    bool disabled = false;
    bool toggle_mode = false;
    bool button_pressed = false; // toggled on, for toggle buttons
    bool keep_pressed_outside = false;
    bool shortcut_feedback = true;

    friend bool operator==(const Button&, const Button&) = default;
};
SLASH_COMPONENT(Button, text, icon, flat, alignment, text_overrun_behavior, autowrap_mode,
                clip_text, icon_alignment, vertical_icon_alignment, expand_icon, disabled,
                toggle_mode, button_pressed, action_mode, button_mask, keep_pressed_outside,
                button_group, shortcut, shortcut_feedback, text_direction, language)

// A toggle button with a box beside its text that shows whether it is on, as Godot's CheckBox:
// a round one when it is in a button_group, as a radio button. Its icons (checked, unchecked,
// radio_checked, radio_unchecked and their _disabled ones) are theme items.
struct CheckBox : Button {
    CheckBox() {
        toggle_mode = true;
        alignment = "left";
    }

    friend bool operator==(const CheckBox&, const CheckBox&) = default;
};
SLASH_COMPONENT(CheckBox, text, icon, flat, alignment, text_overrun_behavior, autowrap_mode,
                clip_text, icon_alignment, vertical_icon_alignment, expand_icon, disabled,
                toggle_mode, button_pressed, action_mode, button_mask, keep_pressed_outside,
                button_group, shortcut, shortcut_feedback, text_direction, language)

// A toggle button with a switch at its end that shows whether it is on, as Godot's
// CheckButton. Its icons (checked, unchecked, their _disabled ones and _mirrored ones for right
// to left) are theme items.
struct CheckButton : Button {
    CheckButton() {
        toggle_mode = true;
        alignment = "left";
    }

    friend bool operator==(const CheckButton&, const CheckButton&) = default;
};
SLASH_COMPONENT(CheckButton, text, icon, flat, alignment, text_overrun_behavior, autowrap_mode,
                clip_text, icon_alignment, vertical_icon_alignment, expand_icon, disabled,
                toggle_mode, button_pressed, action_mode, button_mask, keep_pressed_outside,
                button_group, shortcut, shortcut_feedback, text_direction, language)

// One item of a PopupMenu or MenuButton, as Godot's PopupMenu items: text with an icon, a
// check box or radio button beside it, or a separator line.
struct PopupMenuItem {
    std::string text;
    std::string icon;               // project path of a PNG, BMP or KTX2 file; "" for none
    std::string checkable = "none"; // "none", "check" (a check box) or "radio" (a radio button)
    std::string tooltip;            // shown once the pointer rests on the item
    int id = -1;                    // what id_pressed reports; -1 for the item's index
    int indent = 0;                 // indented this many times the theme's indent
    bool checked = false;
    bool disabled = false;
    bool separator = false; // a line across the menu, around the text if it has some

    friend bool operator==(const PopupMenuItem&, const PopupMenuItem&) = default;
};
SLASH_REFLECT(PopupMenuItem, text, icon, checkable, checked, id, disabled, separator, indent,
              tooltip)

// One option of an OptionButton, as Godot's OptionButton items.
struct OptionButtonItem {
    std::string text;
    std::string icon;    // project path of a PNG, BMP or KTX2 file; "" for none
    std::string tooltip; // shown once the pointer rests on the option in the list
    int id = -1;         // what the option is known by; -1 for its index
    bool disabled = false;
    bool separator = false; // a line between options, around the text if it has some

    friend bool operator==(const OptionButtonItem&, const OptionButtonItem&) = default;
};
SLASH_REFLECT(OptionButtonItem, text, icon, id, disabled, separator, tooltip)

// A list of items shown over everything at a point of the viewport, as Godot's PopupMenu:
// picked with a click, the arrow keys and ui_accept, or by typing the start of one. It shows
// while visible, which closes it again when an item is picked (as the hide_on_* fields say), a
// click lands outside it or ui_cancel is pressed. It emits about_to_popup, popup_hide,
// id_pressed and index_pressed (data {"id", "index"}) and id_focused (data {"id"}), when the
// keys move to an item. Its theme items are PopupMenu's in the project's theme or its own.
struct PopupMenu {
    std::vector<PopupMenuItem> items;
    std::string theme;                          // project path of a .theme.json file; "" for none
    std::string layout_direction = "inherited"; // "inherited" (left to right), "ltr" or "rtl"
    Vec2 position;                              // its top left corner in the viewport, in pixels
    bool visible = false;
    bool hide_on_item_selection = true;
    bool hide_on_checkable_item_selection = true;
    bool allow_search = true; // typing the start of an item's text moves to it

    friend bool operator==(const PopupMenu&, const PopupMenu&) = default;
};
SLASH_COMPONENT(PopupMenu, items, position, visible, hide_on_item_selection,
                hide_on_checkable_item_selection, allow_search, theme, layout_direction)

// A button that shows the option picked and opens a list of them below it when pressed, as
// Godot's OptionButton. Its text and icon are the selected option's. Picking one sets selected
// and emits item_selected (data {"index"}); moving to one with the keys emits item_focused.
struct OptionButton : Button {
    OptionButton() {
        toggle_mode = true;
        alignment = "left";
        action_mode = "press";
    }

    std::vector<OptionButtonItem> items;
    int selected = -1;               // the option shown, by index; -1 for none
    bool fit_to_longest_item = true; // as wide as its widest option, not only the one shown
    bool allow_reselect = false;     // picking the option already shown emits item_selected

    friend bool operator==(const OptionButton&, const OptionButton&) = default;
};
SLASH_COMPONENT(OptionButton, items, selected, fit_to_longest_item, allow_reselect, flat, alignment,
                text_overrun_behavior, autowrap_mode, clip_text, icon_alignment,
                vertical_icon_alignment, expand_icon, disabled, toggle_mode, button_pressed,
                action_mode, button_mask, keep_pressed_outside, button_group, shortcut,
                shortcut_feedback, text_direction, language)

// A flat button that opens a menu of items below it when pressed, as Godot's MenuButton. Picking
// an item emits id_pressed and index_pressed (data {"id", "index"}) from the button; it emits
// about_to_popup and popup_hide as the menu opens and closes, and id_focused (data {"id"}) when
// the keys move to an item. With switch_on_hover, moving onto it while another
// MenuButton's menu is open opens its menu instead, as a menu bar does.
struct MenuButton : Button {
    MenuButton() {
        flat = true;
        toggle_mode = true;
        action_mode = "press";
    }

    std::vector<PopupMenuItem> items;
    bool switch_on_hover = false;

    friend bool operator==(const MenuButton&, const MenuButton&) = default;
};
SLASH_COMPONENT(MenuButton, items, switch_on_hover, text, icon, flat, alignment,
                text_overrun_behavior, autowrap_mode, clip_text, icon_alignment,
                vertical_icon_alignment, expand_icon, disabled, toggle_mode, button_pressed,
                action_mode, button_mask, keep_pressed_outside, button_group, shortcut,
                shortcut_feedback, text_direction, language)

// A button drawn with a texture per state, as Godot's TextureButton.
struct TextureButton {
    std::vector<std::string> button_mask{"left"};
    std::vector<json::Value> shortcut;
    std::string texture_normal; // project paths of PNG, BMP or KTX2 files; "" for none
    std::string texture_pressed;
    std::string texture_hover;
    std::string texture_disabled;
    std::string texture_focused; // drawn over the others while it has focus
    std::string stretch_mode = "keep";
    std::string action_mode = "release";
    std::string button_group;
    bool ignore_texture_size = false;
    bool flip_h = false;
    bool flip_v = false;
    bool disabled = false;
    bool toggle_mode = false;
    bool button_pressed = false;
    bool keep_pressed_outside = false;
    bool shortcut_feedback = true;

    friend bool operator==(const TextureButton&, const TextureButton&) = default;
};
SLASH_COMPONENT(TextureButton, texture_normal, texture_pressed, texture_hover, texture_disabled,
                texture_focused, ignore_texture_size, stretch_mode, flip_h, flip_v, disabled,
                toggle_mode, button_pressed, action_mode, button_mask, keep_pressed_outside,
                button_group, shortcut, shortcut_feedback)

// A one-line text field, as Godot's LineEdit: typed into while it has focus, with a caret,
// selection, undo, the clipboard, an input method's composition and the on-screen keyboard.
struct LineEdit {
    std::string text;
    std::string placeholder_text;   // shown, dimmed, while the text is empty
    std::string alignment = "left"; // "left", "center", "right" or "fill"
    int max_length = 0;             // characters; 0 for no limit
    bool editable = true;
    bool keep_editing_on_text_submit = false;
    bool expand_to_text_length = false; // its minimum width grows with the text
    bool backspace_deletes_composite_character_enabled = false;
    bool virtual_keyboard_enabled = true;
    bool virtual_keyboard_show_on_focus = true;
    std::string virtual_keyboard_type = "default";
    bool clear_button_enabled = false;
    bool shortcut_keys_enabled = true;
    bool selecting_enabled = true;
    bool deselect_on_focus_loss_enabled = true;
    bool drag_and_drop_selection_enabled = true; // the selection can be dragged elsewhere
    std::string right_icon; // project path of a PNG, BMP or KTX2 file; "" for none
    std::string icon_expand_mode = "original_size";
    float right_icon_scale = 1.0F;
    bool flat = false;
    bool select_all_on_focus = false;
    bool caret_blink = false;
    float caret_blink_interval = 0.65F;
    int caret_column = 0; // characters before the caret
    bool caret_force_displayed = false;
    bool caret_mid_grapheme = false;
    bool secret = false; // show each character as secret_character
    std::string secret_character = "\u2022";
    std::string text_direction = "auto"; // as Label's
    std::string language{};              // NOLINT(readability-redundant-member-init)

    friend bool operator==(const LineEdit&, const LineEdit&) = default;
};
SLASH_COMPONENT(LineEdit, text, placeholder_text, alignment, max_length, editable,
                keep_editing_on_text_submit, expand_to_text_length,
                backspace_deletes_composite_character_enabled, virtual_keyboard_enabled,
                virtual_keyboard_show_on_focus, virtual_keyboard_type, clear_button_enabled,
                shortcut_keys_enabled, selecting_enabled, deselect_on_focus_loss_enabled,
                drag_and_drop_selection_enabled, right_icon, icon_expand_mode, right_icon_scale,
                flat, select_all_on_focus, caret_blink, caret_blink_interval, caret_column,
                caret_force_displayed, caret_mid_grapheme, secret, secret_character, text_direction,
                language)

// One line's entry in a TextEdit gutter, as Godot's line gutter text, icon and item color.
struct TextEditGutterLine {
    int line = 0;
    std::string text; // what a "string" gutter shows on the line
    std::string icon; // what an "icon" gutter shows: a PNG, BMP or KTX2 file, or a built-in icon
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};

    friend bool operator==(const TextEditGutterLine&, const TextEditGutterLine&) = default;
};
SLASH_REFLECT(TextEditGutterLine, line, text, icon, color)

// A column left of a TextEdit's text, as Godot's TextEdit gutters: text or an icon per line,
// kept with their lines as the text is edited.
struct TextEditGutter {
    std::string name;
    std::string type = "string"; // "string" or "icon"
    int width = 24;
    bool draw = true;
    std::vector<TextEditGutterLine> lines;

    friend bool operator==(const TextEditGutter&, const TextEditGutter&) = default;
};
SLASH_REFLECT(TextEditGutter, name, type, width, draw, lines)

// A text field of several lines, as Godot's TextEdit: typed into while it has focus, with one
// caret, selection, undo, the clipboard, line wrapping and scrolling.
struct TextEdit {
    std::string text;
    std::string placeholder_text; // shown, dimmed, while the text is empty
    bool editable = true;
    bool backspace_deletes_composite_character_enabled = false;
    bool shortcut_keys_enabled = true;
    bool selecting_enabled = true;
    bool deselect_on_focus_loss_enabled = true;
    bool drag_and_drop_selection_enabled = true;   // the selection can be dragged elsewhere
    bool empty_selection_clipboard_enabled = true; // cut and copy take the line
    std::string wrap_mode = "none";                // "none" or "boundary"
    std::string autowrap_mode = "word_smart";      // "arbitrary", "word" or "word_smart"
    bool tab_input_mode = true;                    // Tab types a tab instead of moving focus
    bool virtual_keyboard_enabled = true;
    bool virtual_keyboard_show_on_focus = true;
    bool scroll_past_end_of_file = false;
    double scroll_vertical = 0.0; // lines scrolled down, wrapped rows counted
    int scroll_horizontal = 0;    // pixels scrolled right
    bool scroll_fit_content_height = false;
    bool scroll_fit_content_width = false;
    std::string caret_type = "line"; // "line" or "block"
    bool caret_blink = false;
    float caret_blink_interval = 0.65F;
    bool caret_draw_when_editable_disabled = false;
    bool caret_move_on_right_click = true;
    bool caret_mid_grapheme = false;
    int caret_line = 0;   // the caret's line, from 0
    int caret_column = 0; // characters before the caret in its line
    bool highlight_current_line = false;
    bool draw_tabs = false;
    bool draw_spaces = false;
    bool highlight_all_occurrences = false; // mark the other copies of the selected text
    std::string search_text;                // marked wherever it occurs in view
    std::vector<std::string> search_flags;  // "match_case", "whole_words"
    std::string text_direction = "auto";    // as Label's
    std::string language{};                 // NOLINT(readability-redundant-member-init)
    std::vector<TextEditGutter> gutters;    // after a CodeEdit's, left to right

    friend bool operator==(const TextEdit&, const TextEdit&) = default;
};
SLASH_COMPONENT(TextEdit, text, placeholder_text, editable,
                backspace_deletes_composite_character_enabled, shortcut_keys_enabled,
                selecting_enabled, deselect_on_focus_loss_enabled, drag_and_drop_selection_enabled,
                empty_selection_clipboard_enabled, wrap_mode, autowrap_mode, tab_input_mode,
                virtual_keyboard_enabled, virtual_keyboard_show_on_focus, scroll_past_end_of_file,
                scroll_vertical, scroll_horizontal, scroll_fit_content_height,
                scroll_fit_content_width, caret_type, caret_blink, caret_blink_interval,
                caret_draw_when_editable_disabled, caret_move_on_right_click, caret_mid_grapheme,
                caret_line, caret_column, highlight_current_line, draw_tabs, draw_spaces,
                highlight_all_occurrences, search_text, search_flags, text_direction, language,
                gutters)

// Colors the text of the TextEdit on its entity as code, as Godot's CodeHighlighter set as a
// TextEdit's syntax_highlighter: keywords, numbers, symbols, function names, members after a
// '.', and regions such as strings and comments that may run over several lines.
struct CodeHighlighter {
    Vec4 number_color{0.0F, 0.0F, 0.0F, 1.0F};
    Vec4 symbol_color{0.0F, 0.0F, 0.0F, 1.0F};
    Vec4 function_color{0.0F, 0.0F, 0.0F, 1.0F};
    Vec4 member_variable_color{0.0F, 0.0F, 0.0F, 1.0F};
    std::map<std::string, Vec4> keyword_colors;
    std::map<std::string, Vec4> member_keyword_colors; // not after a '.'
    std::map<std::string, Vec4> color_regions;         // "start end", or "start" to the line's end

    friend bool operator==(const CodeHighlighter&, const CodeHighlighter&) = default;
};
SLASH_COMPONENT(CodeHighlighter, number_color, symbol_color, function_color, member_variable_color,
                keyword_colors, member_keyword_colors, color_regions)

// Makes the TextEdit on its entity a code editor, as Godot's CodeEdit: line numbers, and a
// gutter of breakpoints, bookmarks and executing lines, kept with their lines as the text is
// edited, and lines marking columns. Clicking the gutter toggles a breakpoint (a bookmark with
// Shift); clicking a line number selects the line.
struct CodeEdit {
    bool gutters_draw_breakpoints_gutter = false;
    bool gutters_draw_bookmarks = false;
    bool gutters_draw_executing_lines = false;
    bool gutters_draw_line_numbers = false;
    bool gutters_zero_pad_line_numbers = false;
    int gutters_line_numbers_min_digits = 3;
    std::vector<int> line_length_guidelines; // columns, the first drawn brighter
    std::vector<int> breakpointed_lines;
    std::vector<int> bookmarked_lines;
    std::vector<int> executing_lines;

    friend bool operator==(const CodeEdit&, const CodeEdit&) = default;
};
SLASH_COMPONENT(CodeEdit, gutters_draw_breakpoints_gutter, gutters_draw_bookmarks,
                gutters_draw_executing_lines, gutters_draw_line_numbers,
                gutters_zero_pad_line_numbers, gutters_line_numbers_min_digits,
                line_length_guidelines, breakpointed_lines, bookmarked_lines, executing_lines)

// Containers place the controls in their children lists, which ignore their anchors and
// offsets and are sized by their minimum sizes and size flags. An entity has at most one.

// Lays out its children in a row, or a column when vertical, as Godot's BoxContainer
// (HBoxContainer and VBoxContainer).
struct BoxContainer {
    std::string alignment = "begin"; // "begin", "center" or "end"
    bool vertical = false;

    friend bool operator==(const BoxContainer&, const BoxContainer&) = default;
};
SLASH_COMPONENT(BoxContainer, alignment, vertical)

// Lays out its children in a grid, left to right then top to bottom, as Godot's GridContainer.
struct GridContainer {
    int columns = 1;

    friend bool operator==(const GridContainer&, const GridContainer&) = default;
};
SLASH_COMPONENT(GridContainer, columns)

// Fits its children in its rect less the theme's margins, as Godot's MarginContainer.
struct MarginContainer {
    friend bool operator==(const MarginContainer&, const MarginContainer&) = default;
};
SLASH_COMPONENT(MarginContainer)

// Centers its children at their minimum sizes, as Godot's CenterContainer.
struct CenterContainer {
    bool use_top_left = false; // center them on its top-left corner instead

    friend bool operator==(const CenterContainer&, const CenterContainer&) = default;
};
SLASH_COMPONENT(CenterContainer, use_top_left)

// Lays out its children in rows that wrap to its width, or columns that wrap to its height when
// vertical, as Godot's FlowContainer (HFlowContainer and VFlowContainer).
struct FlowContainer {
    std::string alignment = "begin";             // "begin", "center" or "end"
    std::string last_wrap_alignment = "inherit"; // "inherit", "begin", "center" or "end"
    bool vertical = false;
    bool reverse_fill = false; // rows from the bottom up, or columns from the right

    friend bool operator==(const FlowContainer&, const FlowContainer&) = default;
};
SLASH_COMPONENT(FlowContainer, alignment, last_wrap_alignment, vertical, reverse_fill)

// Draws the theme's "panel" style box and fits its children inside the style's margins, as
// Godot's PanelContainer.
struct PanelContainer {
    friend bool operator==(const PanelContainer&, const PanelContainer&) = default;
};
SLASH_COMPONENT(PanelContainer)

// Shows its children through its rect, scrolled, with scroll bars when they don't fit, as
// Godot's ScrollContainer. Each child takes its minimum size, or the whole visible width or
// height on an axis its size flags expand. The wheel, the scroll bars and touch drags scroll
// it; it always clips its children. It emits scroll_started and scroll_ended around a drag.
struct ScrollContainer {
    // Each axis's: "disabled" (no scrolling: it is as wide as its children), "auto" (a scroll
    // bar when they don't fit), "always", "never" (scrolls, with no bar) or "reserve" (as auto,
    // with room kept for the bar).
    std::string horizontal_scroll_mode = "auto";
    std::string vertical_scroll_mode = "auto";
    int scroll_horizontal = 0;                   // pixels scrolled right
    int scroll_vertical = 0;                     // pixels scrolled down
    float scroll_horizontal_custom_step = -1.0F; // what its bars' arrows step by; -1 for none
    float scroll_vertical_custom_step = -1.0F;
    int scroll_deadzone = 0;                   // pixels a touch moves before it scrolls
    bool follow_focus = false;                 // scroll to show a control inside it that gets focus
    bool scroll_horizontal_by_default = false; // the wheel scrolls across, and Shift+wheel down
    bool draw_focus_border = false;            // draw the focus style while it or a child has focus

    friend bool operator==(const ScrollContainer&, const ScrollContainer&) = default;
};
SLASH_COMPONENT(ScrollContainer, follow_focus, draw_focus_border, scroll_horizontal,
                scroll_vertical, scroll_horizontal_custom_step, scroll_vertical_custom_step,
                horizontal_scroll_mode, vertical_scroll_mode, scroll_horizontal_by_default,
                scroll_deadzone)

} // namespace slash::world
