#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <map>
#include <numbers>
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
    // How it takes part in global illumination, as Godot's GeometryInstance3D.gi_mode: "static"
    // meshes are baked into a LightmapGI's lightmap (and lit by it), "dynamic" ones take their
    // ambient light from its light probes, and "disabled" ones from neither.
    // gi_lightmap_texel_scale scales its lightmap's texels across.
    std::string gi_mode = "static";
    float gi_lightmap_texel_scale = 1.0F;

    friend bool operator==(const MeshInstance3D&, const MeshInstance3D&) = default;
};
SLASH_COMPONENT(MeshInstance3D, mesh, layers, cast_shadow, gi_mode, gi_lightmap_texel_scale)

// Poses the skeleton of the model the entity's MeshInstance3D draws by hand, as Godot's Skeleton3D
// bone poses: each bone, or other node of the model, named in these maps takes the position, the
// rotation (radians about each axis, in Transform3D's order) or the scale given, in its parent's
// space, in place of what its animation or its rest pose gives it.
struct Skeleton3D {
    std::map<std::string, Vec3> bone_positions;
    std::map<std::string, Vec3> bone_rotations;
    std::map<std::string, Vec3> bone_scales;
    // Shows the rest pose, leaving out animations and these maps, as Godot's show_rest_only.
    bool show_rest_only = false;

    friend bool operator==(const Skeleton3D&, const Skeleton3D&) = default;
};
SLASH_COMPONENT(Skeleton3D, bone_positions, bone_rotations, bone_scales, show_rest_only)

// Skeleton modifiers turn bones of the model an entity's MeshInstance3D draws after its
// animations and Skeleton3D, as Godot's SkeletonModifier3D children of a Skeleton3D do: those on
// the entity, then those on each entity in its Transform3D children, in list order; on each,
// TwoBoneIK3D, FABRIK3D, CCDIK3D, JacobianIK3D, then PhysicalBoneSimulator3D. Each moves the
// pose influence of the way.

// One arm or leg a TwoBoneIK3D bends, as a Godot TwoBoneIK3D setting: from root_bone through
// middle_bone to end_bone (or, with use_virtual_end, to a point end_bone_length past middle_bone),
// the end reaching for the target entity and bending toward the pole entity.
struct TwoBoneIKSetting {
    std::string root_bone;
    std::string middle_bone;
    std::string end_bone;
    // "from_parent" (the way from its parent to it), or "plus_x", "minus_x", "plus_y",
    // "minus_y", "plus_z" or "minus_z" in the bone's space.
    std::string end_bone_direction = "from_parent";
    // The middle bone's axis rolled toward the pole: "none", "plus_x", "minus_x", "plus_y",
    // "minus_y", "plus_z", "minus_z" or "custom" (pole_direction_vector).
    std::string pole_direction = "none";
    Vec3 pole_direction_vector;
    float end_bone_length = 0.0F;
    entt::entity target{entt::null};
    entt::entity pole{entt::null};
    bool use_virtual_end = false;
    // Reaches with a point end_bone_length along end_bone_direction from the end bone.
    bool extend_end_bone = false;

    friend bool operator==(const TwoBoneIKSetting&, const TwoBoneIKSetting&) = default;
};
SLASH_REFLECT(TwoBoneIKSetting, root_bone, middle_bone, end_bone, use_virtual_end, extend_end_bone,
              end_bone_direction, end_bone_length, target, pole, pole_direction,
              pole_direction_vector)

// Bends two bones so the end of the second reaches a target, as Godot's TwoBoneIK3D.
struct TwoBoneIK3D {
    std::vector<TwoBoneIKSetting> settings;
    float influence = 1.0F;
    bool active = true;
    // Bone lengths come from where the bones are posed, not where they rest.
    bool mutable_bone_axes = true;

    friend bool operator==(const TwoBoneIK3D&, const TwoBoneIK3D&) = default;
};
SLASH_COMPONENT(TwoBoneIK3D, settings, active, influence, mutable_bone_axes)

// How one bone of a chain may turn, as a Godot IterateIK3D joint: only about rotation_axis, and
// within a cone around where it points at rest (limitation "cone", limitation_angle radians
// across).
struct ChainIKJoint {
    // "all", "x", "y", "z" or "custom" (rotation_axis_vector), in the bone's space.
    std::string rotation_axis = "all";
    std::string limitation; // "" or "cone"
    // Which way is right in the cone's space: "none", "plus_x", ... or "custom".
    std::string limitation_right_axis = "none";
    Vec3 rotation_axis_vector{1.0F, 0.0F, 0.0F};
    Vec3 limitation_right_axis_vector{1.0F, 0.0F, 0.0F};
    // The cone turned by radians about each axis, as Transform3D's rotation.
    Vec3 limitation_rotation_offset;
    float limitation_angle = 1.5707964F; // a quarter turn

    friend bool operator==(const ChainIKJoint&, const ChainIKJoint&) = default;
};
SLASH_REFLECT(ChainIKJoint, rotation_axis, rotation_axis_vector, limitation, limitation_angle,
              limitation_right_axis, limitation_right_axis_vector, limitation_rotation_offset)

// One chain an iterating IK solver turns, as a Godot ChainIK3D setting: every bone from root_bone
// down to end_bone, the end reaching for the target entity. joints limits bones, by name.
struct ChainIKSetting {
    std::string root_bone;
    std::string end_bone;
    std::string end_bone_direction = "from_parent";
    std::map<std::string, ChainIKJoint> joints;
    float end_bone_length = 0.0F;
    entt::entity target{entt::null};
    bool extend_end_bone = false;

    friend bool operator==(const ChainIKSetting&, const ChainIKSetting&) = default;
};
SLASH_REFLECT(ChainIKSetting, root_bone, end_bone, extend_end_bone, end_bone_direction,
              end_bone_length, target, joints)

// Godot's iterating IK solvers, FABRIK3D, CCDIK3D and JacobianIK3D: each tick takes up to
// max_iterations steps, each turning a bone at most angular_delta_limit radians, until the end is
// within min_distance of the target. Unless deterministic, a chain carries on from where the last
// tick left it, so it eases toward a target that jumps.
struct FABRIK3D {
    std::vector<ChainIKSetting> settings;
    double min_distance = 0.001;
    double angular_delta_limit = 0.03490658503988659; // 2 degrees
    int max_iterations = 4;
    float influence = 1.0F;
    bool active = true;
    bool mutable_bone_axes = true;
    bool deterministic = false;

    friend bool operator==(const FABRIK3D&, const FABRIK3D&) = default;
};
SLASH_COMPONENT(FABRIK3D, settings, active, influence, mutable_bone_axes, max_iterations,
                min_distance, angular_delta_limit, deterministic)

struct CCDIK3D {
    std::vector<ChainIKSetting> settings;
    double min_distance = 0.001;
    double angular_delta_limit = 0.03490658503988659;
    int max_iterations = 4;
    float influence = 1.0F;
    bool active = true;
    bool mutable_bone_axes = true;
    bool deterministic = false;

    friend bool operator==(const CCDIK3D&, const CCDIK3D&) = default;
};
SLASH_COMPONENT(CCDIK3D, settings, active, influence, mutable_bone_axes, max_iterations,
                min_distance, angular_delta_limit, deterministic)

struct JacobianIK3D {
    std::vector<ChainIKSetting> settings;
    double min_distance = 0.001;
    double angular_delta_limit = 0.03490658503988659;
    int max_iterations = 4;
    float influence = 1.0F;
    bool active = true;
    bool mutable_bone_axes = true;
    bool deterministic = false;

    friend bool operator==(const JacobianIK3D&, const JacobianIK3D&) = default;
};
SLASH_COMPONENT(JacobianIK3D, settings, active, influence, mutable_bone_axes, max_iterations,
                min_distance, angular_delta_limit, deterministic)

// Follows a bone of a model, as Godot's BoneAttachment3D: each tick, after the model's animations
// and skeleton modifiers, the entity goes where the bone is posed, so what hangs under it in its
// Transform3D children (a sword in a hand, a hit box on a head) moves with the bone. The model is
// the one its Transform3D parent draws, or external_skeleton's, as Godot's use_external_skeleton.
struct BoneAttachment3D {
    std::string bone_name;
    entt::entity external_skeleton{entt::null};

    friend bool operator==(const BoneAttachment3D&, const BoneAttachment3D&) = default;
};
SLASH_COMPONENT(BoneAttachment3D, bone_name, external_skeleton)

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
    // Whether 3D sounds' doppler follows its speed while it is the listener: "disabled",
    // "idle_step" or "physics_step".
    std::string doppler_tracking = "disabled";

    friend bool operator==(const Camera3D&, const Camera3D&) = default;
};
SLASH_COMPONENT(Camera3D, projection, keep_aspect, fov, size, near, far, h_offset, v_offset,
                current, cull_mask, doppler_tracking)

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
    // Baking into a LightmapGI, as Godot's Light3D: how much it lights what bounces, whether it is
    // left out ("disabled"), baked for bounces only and lit live ("dynamic") or baked whole and not
    // lit live on lightmapped meshes ("static"), and how far its soft shadows blur.
    float light_indirect_energy = 1.0F;
    std::string light_bake_mode = "dynamic";
    float shadow_blur = 1.0F;
    float light_volumetric_fog_energy = 1.0F; // Forward+: how much it lights volumetric fog

    friend bool operator==(const DirectionalLight3D&, const DirectionalLight3D&) = default;
};
SLASH_COMPONENT(DirectionalLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, shadow_enabled, shadow_bias, shadow_normal_bias,
                shadow_reverse_cull_face, shadow_opacity, shadow_caster_mask,
                directional_shadow_mode, directional_shadow_split_1, directional_shadow_split_2,
                directional_shadow_split_3, directional_shadow_blend_splits,
                directional_shadow_fade_start, directional_shadow_max_distance,
                directional_shadow_pancake_size, light_angular_distance, sky_mode,
                light_indirect_energy, light_bake_mode, shadow_blur, light_volumetric_fog_energy)

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
    // Baking into a LightmapGI, as Godot's Light3D: how much it lights what bounces, its radius
    // for soft shadows, whether it is left out ("disabled"), baked for bounces only and lit live
    // ("dynamic") or baked whole and not lit live on lightmapped meshes ("static"), and how far
    // its soft shadows blur.
    float light_indirect_energy = 1.0F;
    float light_size = 0.0F;
    std::string light_bake_mode = "dynamic";
    float shadow_blur = 1.0F;
    float light_volumetric_fog_energy = 1.0F; // Forward+: how much it lights volumetric fog

    friend bool operator==(const OmniLight3D&, const OmniLight3D&) = default;
};
SLASH_COMPONENT(OmniLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, omni_range, omni_attenuation, shadow_enabled, shadow_bias,
                shadow_normal_bias, shadow_reverse_cull_face, shadow_opacity, shadow_caster_mask,
                light_indirect_energy, light_size, light_bake_mode, shadow_blur,
                light_volumetric_fog_energy)

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
    // Baking into a LightmapGI, as Godot's Light3D: how much it lights what bounces, its radius
    // for soft shadows, whether it is left out ("disabled"), baked for bounces only and lit live
    // ("dynamic") or baked whole and not lit live on lightmapped meshes ("static"), and how far
    // its soft shadows blur.
    float light_indirect_energy = 1.0F;
    float light_size = 0.0F;
    std::string light_bake_mode = "dynamic";
    float shadow_blur = 1.0F;
    float light_volumetric_fog_energy = 1.0F; // Forward+: how much it lights volumetric fog

    friend bool operator==(const SpotLight3D&, const SpotLight3D&) = default;
};
SLASH_COMPONENT(SpotLight3D, light_color, light_energy, light_specular, light_negative,
                light_cull_mask, spot_range, spot_attenuation, spot_angle, spot_angle_attenuation,
                shadow_enabled, shadow_bias, shadow_normal_bias, shadow_reverse_cull_face,
                shadow_opacity, shadow_caster_mask, light_indirect_energy, light_size,
                light_bake_mode, shadow_blur, light_volumetric_fog_energy)

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

// A box that reflects its surroundings, as Godot's ReflectionProbe: in Forward+, meshes inside it
// reflect a cube drawn from origin_offset in place of the sky's, by roughness, and may take their
// ambient light from it, fading out over blend_distance toward its sides. Where probes overlap,
// the smallest is taken first. Its scale is ignored.
struct ReflectionProbe {
    std::string update_mode = "once"; // "once" (when it or its settings change) or "always"
    float intensity = 1.0F;
    float blend_distance = 1.0F;
    float max_distance = 0.0F; // how far the cube sees, when further than the box's sides
    Vec3 size{20.0F, 20.0F, 20.0F};
    Vec3 origin_offset;          // where the cube is drawn from, inside the box
    bool box_projection = false; // aim reflections at the box's walls, as for a room
    bool interior = false;       // draw the cube without the sky, fog or ambient light
    bool enable_shadows = false;
    std::uint32_t cull_mask = 0xFFFFF;       // the layers the cube shows
    std::uint32_t reflection_mask = 0xFFFFF; // the layers whose meshes reflect it
    float mesh_lod_threshold = 1.0F;
    std::string ambient_mode = "environment"; // "disabled", "environment" or "color"
    Vec4 ambient_color{0.0F, 0.0F, 0.0F, 1.0F};
    float ambient_color_energy = 1.0F;
};
SLASH_COMPONENT(ReflectionProbe, update_mode, intensity, blend_distance, max_distance, size,
                origin_offset, box_projection, interior, enable_shadows, cull_mask, reflection_mask,
                mesh_lod_threshold, ambient_mode, ambient_color, ambient_color_energy)

// Light baked into lightmaps, as Godot's LightmapGI: lightmap.bake traces the light of the static
// MeshInstance3Ds and the lights that bake (light_bake_mode) under its entity's parent (the whole
// world for a root) into light_data, and lightmapped meshes then take their ambient light from it,
// in place of the environment's and reflection probes'. Its own place doesn't matter.
struct LightmapGI {
    std::string quality = "medium"; // "low", "medium", "high" or "ultra": rays traced a texel
    bool supersampling = false;     // bake at supersampling_factor times the texels, then shrink
    float supersampling_factor = 2.0F;
    int bounces = 3;
    float bounce_indirect_energy = 1.0F;
    bool use_texture_for_bounces = true; // bounces take direct light from the lightmap
    // Moving meshes inside its light probes take only interior lightmaps' light, as Godot's.
    bool interior = false;
    bool use_denoiser = true;
    float denoiser_strength = 0.1F;
    int denoiser_range = 10;
    float bias = 0.0005F;
    float texel_scale = 1.0F;
    int max_texture_size = 16384;
    // The light rays that leave the world bring: "disabled" (none), "scene" (the
    // WorldEnvironment's, as meshes would take ambient light from it) or "custom_color".
    std::string environment_mode = "scene";
    Vec4 environment_custom_color{1.0F, 1.0F, 1.0F, 1.0F};
    float environment_custom_energy = 1.0F;
    // The entity whose CameraAttributesPractical's exposure_multiplier the light is baked with,
    // or "" for 1.
    std::string camera_attributes;
    // Light probes for moving meshes, as Godot's: "disabled" for 8 at the bounds' corners, or
    // "4", "8", "16" or "32" cells along the longest side of an octree over the static meshes.
    std::string generate_probes_subdiv = "8";
    // The .lightmap.json file lightmap.bake writes, project path; "" before a bake.
    std::string light_data;

    friend bool operator==(const LightmapGI&, const LightmapGI&) = default;
};
SLASH_COMPONENT(LightmapGI, quality, supersampling, supersampling_factor, bounces,
                bounce_indirect_energy, use_texture_for_bounces, interior, use_denoiser,
                denoiser_strength, denoiser_range, bias, texel_scale, max_texture_size,
                environment_mode, environment_custom_color, environment_custom_energy,
                camera_attributes, generate_probes_subdiv, light_data)

// A place a LightmapGI bakes a light probe, besides those it places itself, as Godot's
// LightmapProbe: where moving meshes' light changes quickly, as in a doorway. It counts under the
// LightmapGI's parent, as meshes do.
struct LightmapProbe {
    friend bool operator==(const LightmapProbe&, const LightmapProbe&) = default;
};
SLASH_COMPONENT(LightmapProbe)

// Light bounced around a box of the world in real time, as Godot's VoxelGI and its VoxelGIData:
// voxel_gi.bake voxelizes the static MeshInstance3Ds under its entity's parent (the whole world
// for a root) that touch the box of size centered on it into data, and the lights are then
// injected into those cells whenever they change, so lights may move while the meshes stay put.
// Meshes in the box take their ambient light and reflections from the cells, in place of the
// environment's, unless they are lightmapped.
struct VoxelGI {
    std::string subdiv = "128"; // cells along the box's longest side: "64", "128", "256" or "512"
    Vec3 size{20.0F, 20.0F, 20.0F};
    // The entity whose CameraAttributesPractical's exposure_multiplier emission is baked with,
    // or "" for 1.
    std::string camera_attributes;
    // The .voxelgi.json file voxel_gi.bake writes, project path; "" before a bake.
    std::string data;
    float dynamic_range = 2.0F; // the brightest light the cells hold
    float energy = 1.0F;
    float bias = 1.5F;        // cells a surface's cone starts out from, against self-lighting
    float normal_bias = 0.0F; // cells a surface's cones start out along its normal
    float propagation = 0.5F; // how much light spreads up into bigger cells
    bool use_two_bounces = true;
    bool interior = false; // the environment's light doesn't come in

    friend bool operator==(const VoxelGI&, const VoxelGI&) = default;
};
SLASH_COMPONENT(VoxelGI, subdiv, size, camera_attributes, data, dynamic_range, energy, bias,
                normal_bias, propagation, use_two_bounces, interior)

// A box that projects textures onto the meshes inside it, as Godot's Decal, in Forward+: down its
// -Y, its albedo texture's color mixed into theirs by albedo_mix where its alpha says, with its
// normal and ORM (ambient occlusion, roughness, metallic) textures there too, and its emission
// texture added. It fades toward its top and bottom (upper_fade, lower_fade), on surfaces that
// face away from its up (normal_fade) and, with distance_fade_enabled, away from the camera.
// Nearer decals draw over further ones, less their sorting_offset. Compatibility draws none.
struct Decal {
    Vec3 size{2.0F, 2.0F, 2.0F}; // the box, centered on the entity, in its space
    std::string texture_albedo;
    std::string texture_normal;
    std::string texture_orm;
    std::string texture_emission;
    float emission_energy = 1.0F;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F}; // sRGB, times the albedo and emission
    float albedo_mix = 1.0F;
    float normal_fade = 0.0F;
    float upper_fade = 0.3F;
    float lower_fade = 0.3F;
    bool distance_fade_enabled = false;
    float distance_fade_begin = 40.0F;
    float distance_fade_length = 10.0F;
    std::uint32_t cull_mask = 0xFFFFF; // the layers of the meshes it draws on
    std::uint32_t layers = 1;          // as MeshInstance3D's: the cameras that see it
    float sorting_offset = 0.0F;

    friend bool operator==(const Decal&, const Decal&) = default;
};
SLASH_COMPONENT(Decal, size, texture_albedo, texture_normal, texture_orm, texture_emission,
                emission_energy, modulate, albedo_mix, normal_fade, upper_fade, lower_fade,
                distance_fade_enabled, distance_fade_begin, distance_fade_length, cull_mask, layers,
                sorting_offset)

// The 3D world's background, ambient light, tonemap, reflections, ambient occlusion, glow, fog,
// volumetric fog and adjustments, as Godot's WorldEnvironment and its Environment. The first one
// made applies; without one the background is the project's clear color, there is no ambient light
// and colors are left as they are. A "sky" background is the sky material on the same entity: a
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
    float tonemap_agx_white = 16.29F;   // agx's white with Forward+, as Blender's AgX
    float tonemap_agx_contrast = 1.25F; // how steep agx's curve is with Forward+
    // Forward+: screen-space reflections, which reflect what is on the screen in shiny surfaces.
    bool ssr_enabled = false;
    std::uint32_t ssr_max_steps = 64;
    float ssr_fade_in = 0.15F;
    float ssr_fade_out = 2.0F;
    float ssr_depth_tolerance = 0.5F;
    // Forward+: screen-space ambient occlusion, which darkens creases, corners and what is close to
    // a surface.
    bool ssao_enabled = false;
    float ssao_radius = 1.0F;
    float ssao_intensity = 2.0F;
    float ssao_power = 1.5F;
    float ssao_detail = 0.5F;
    float ssao_horizon = 0.06F;
    float ssao_sharpness = 0.98F;
    float ssao_light_affect = 0.0F;
    float ssao_ao_channel_affect = 0.0F;
    bool glow_enabled = false;
    // Forward+: how much each of glow's seven levels adds, from the sharpest; Godot's
    // glow_levels/1 to glow_levels/7.
    std::vector<float> glow_levels{0.0F, 0.8F, 0.4F, 0.1F, 0.0F, 0.0F, 0.0F};
    bool glow_normalized = false; // Forward+: the levels are divided by their sum
    float glow_intensity = 0.3F;
    float glow_strength = 1.0F; // Forward+
    float glow_mix = 0.05F;     // Forward+, with the "mix" blend mode
    // Forward+: "additive", "screen", "softlight", "replace" or "mix"
    std::string glow_blend_mode = "screen";
    float glow_bloom = 0.0F;
    float glow_hdr_threshold = 1.0F;
    float glow_hdr_scale = 2.0F;
    float glow_hdr_luminance_cap = 12.0F;
    bool fog_enabled = false;
    // Forward+: volumetric fog, as Godot's, which fills the view up to volumetric_fog_length
    // meters with fog the lights shine through and the shadows fall in (its fields are below).
    bool volumetric_fog_enabled = false;
    // Whether each frame keeps volumetric_fog_temporal_reprojection_amount of the frame before's
    // fog, to smooth it.
    bool volumetric_fog_temporal_reprojection_enabled = true;
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
    float volumetric_fog_density = 0.05F;
    Vec4 volumetric_fog_albedo{1.0F, 1.0F, 1.0F, 1.0F}; // the color it scatters light with
    Vec4 volumetric_fog_emission{0.0F, 0.0F, 0.0F, 1.0F};
    float volumetric_fog_emission_energy = 1.0F;
    // -1 scatters light back toward it, 0 every way and 1 on, as through a haze.
    float volumetric_fog_anisotropy = 0.2F;
    float volumetric_fog_length = 64.0F;
    float volumetric_fog_detail_spread = 2.0F;  // above 1, more detail near the camera
    float volumetric_fog_ambient_inject = 0.0F; // how much it is lit by the ambient light
    float volumetric_fog_sky_affect = 1.0F;     // how much it covers the sky
    float volumetric_fog_temporal_reprojection_amount = 0.9F;
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
                reflected_light_source, tonemap_mode, tonemap_exposure, tonemap_white,
                tonemap_agx_white, tonemap_agx_contrast, ssr_enabled, ssr_max_steps, ssr_fade_in,
                ssr_fade_out, ssr_depth_tolerance, ssao_enabled, ssao_radius, ssao_intensity,
                ssao_power, ssao_detail, ssao_horizon, ssao_sharpness, ssao_light_affect,
                ssao_ao_channel_affect, glow_enabled, glow_levels, glow_normalized, glow_intensity,
                glow_strength, glow_mix, glow_blend_mode, glow_bloom, glow_hdr_threshold,
                glow_hdr_scale, glow_hdr_luminance_cap, fog_enabled, fog_mode, fog_light_color,
                fog_light_energy, fog_sun_scatter, fog_density, fog_aerial_perspective,
                fog_sky_affect, fog_height, fog_height_density, fog_depth_curve, fog_depth_begin,
                fog_depth_end, volumetric_fog_enabled, volumetric_fog_density,
                volumetric_fog_albedo, volumetric_fog_emission, volumetric_fog_emission_energy,
                volumetric_fog_anisotropy, volumetric_fog_length, volumetric_fog_detail_spread,
                volumetric_fog_ambient_inject, volumetric_fog_sky_affect,
                volumetric_fog_temporal_reprojection_enabled,
                volumetric_fog_temporal_reprojection_amount, adjustment_enabled,
                adjustment_brightness, adjustment_contrast, adjustment_saturation,
                adjustment_color_correction)

// How the 3D world is exposed and focused, as Godot's CameraAttributesPractical: on a Camera3D's
// entity for that camera, or on the WorldEnvironment's entity for cameras without their own.
// exposure_multiplier brightens the lights, emission, background and the sky's light, as Godot's
// exposure normalization does. With auto exposure, Forward+ measures each frame's brightness and
// exposes it so that brightness comes out at auto_exposure_scale, moving toward it as an eye
// adapts, at auto_exposure_speed. With far or near blur, Forward+ blurs what is past
// dof_blur_far_distance or nearer than dof_blur_near_distance, up to dof_blur_amount times 64
// pixels, as Godot's depth of field.
struct CameraAttributesPractical {
    float exposure_sensitivity = 100.0F; // ISO; the auto exposure sensitivities are over it
    float exposure_multiplier = 1.0F;
    bool auto_exposure_enabled = false;
    float auto_exposure_scale = 0.4F;
    float auto_exposure_speed = 0.5F;
    // The darkest and brightest scenes it adapts to, in ISO: a brightness of
    // sensitivity * 0.125 / exposure_sensitivity.
    float auto_exposure_min_sensitivity = 0.0F;
    float auto_exposure_max_sensitivity = 800.0F;
    // Blur grows over the transition (in meters; below 0, the physical way) from the distance on.
    bool dof_blur_far_enabled = false;
    float dof_blur_far_distance = 10.0F;
    float dof_blur_far_transition = 5.0F;
    bool dof_blur_near_enabled = false;
    float dof_blur_near_distance = 2.0F;
    float dof_blur_near_transition = 1.0F;
    float dof_blur_amount = 0.1F;

    friend bool operator==(const CameraAttributesPractical&,
                           const CameraAttributesPractical&) = default;
};
SLASH_COMPONENT(CameraAttributesPractical, exposure_sensitivity, exposure_multiplier,
                auto_exposure_enabled, auto_exposure_scale, auto_exposure_speed,
                auto_exposure_min_sensitivity, auto_exposure_max_sensitivity, dof_blur_far_enabled,
                dof_blur_far_distance, dof_blur_far_transition, dof_blur_near_enabled,
                dof_blur_near_distance, dof_blur_near_transition, dof_blur_amount)

// ---- Paths, as Godot's Path2D and Path3D with their curves (path.hpp) ----
//
// A path is a curve of Bézier segments from each point to the next in the entity's space, as
// Godot's Curve2D and Curve3D: each point has control handles before (in) and after (out) it,
// relative to it, and both zero make the segments straight lines. A PathFollow2D or PathFollow3D
// child is placed on its parent's curve each tick by the path_follow system.

// A point of a Path2D's curve, as Godot's Curve2D keeps them.
struct CurvePoint2D {
    Vec2 position;
    Vec2 in;
    Vec2 out;

    friend bool operator==(const CurvePoint2D&, const CurvePoint2D&) = default;
};
SLASH_REFLECT(CurvePoint2D, position, in, out)

// Godot's Path2D with its Curve2D.
struct Path2D {
    std::vector<CurvePoint2D> points;
    float bake_interval = 5.0F; // pixels between the points it is baked into

    friend bool operator==(const Path2D&, const Path2D&) = default;
};
SLASH_COMPONENT(Path2D, points, bake_interval)

// Godot's PathFollow2D: places the entity's Transform2D on its parent's Path2D curve.
struct PathFollow2D {
    float progress = 0.0F; // pixels along the curve
    float h_offset = 0.0F; // along the curve
    float v_offset = 0.0F; // across it
    bool rotates = true;
    bool cubic_interp = true;
    bool loop = true;

    friend bool operator==(const PathFollow2D&, const PathFollow2D&) = default;
};
SLASH_COMPONENT(PathFollow2D, progress, h_offset, v_offset, rotates, cubic_interp, loop)

// A point of a Path3D's curve, as Godot's Curve3D keeps them, with its tilt in radians.
struct CurvePoint3D {
    Vec3 position;
    Vec3 in;
    Vec3 out;
    float tilt = 0.0F;

    friend bool operator==(const CurvePoint3D&, const CurvePoint3D&) = default;
};
SLASH_REFLECT(CurvePoint3D, position, in, out, tilt)

// Godot's Path3D with its Curve3D.
struct Path3D {
    std::vector<CurvePoint3D> points;
    bool closed = false;        // a segment from the last point back to the first
    float bake_interval = 0.2F; // meters between the points it is baked into
    bool up_vector_enabled = true;

    friend bool operator==(const Path3D&, const Path3D&) = default;
};
SLASH_COMPONENT(Path3D, points, closed, bake_interval, up_vector_enabled)

// Godot's PathFollow3D: places the entity's Transform3D on its parent's Path3D curve.
struct PathFollow3D {
    float progress = 0.0F;             // meters along the curve
    float h_offset = 0.0F;             // to the side
    float v_offset = 0.0F;             // up
    std::string rotation_mode = "xyz"; // "none", "y", "xy", "xyz" or "oriented"
    bool use_model_front = false;
    bool cubic_interp = true;
    bool loop = true;
    bool tilt_enabled = true;

    friend bool operator==(const PathFollow3D&, const PathFollow3D&) = default;
};
SLASH_COMPONENT(PathFollow3D, progress, h_offset, v_offset, rotation_mode, use_model_front,
                cubic_interp, loop, tilt_enabled)

// ---- CSG shapes, as Godot's CSG nodes (engine/scene3d/src/csg.cpp) ----
//
// An entity with one of the components below is a CSG shape. A shape whose Transform3D parent is
// a CSG shape too is combined into it, and the outermost one, the root, draws the result. Each
// child, in its parent's children order and placed by its own Transform3D, is added to what
// comes before it (union), keeps only where they overlap (intersection) or is cut out of it
// (subtraction); hidden children are left out. A shape's StandardMaterial3D colors its faces, as
// Godot's material property, wherever they end up. Every CSG component ends with Godot's
// CSGShape3D fields: operation, calculate_tangents (tangents for normal maps), use_collision,
// collision_layer and collision_mask (the root is a static body of its triangles), and
// MeshInstance3D's layers and cast_shadow; only the root's collision and drawing fields count.

// Godot's CSGCombiner3D: no shape of its own, only its children's.
struct CSGCombiner3D {
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGCombiner3D&, const CSGCombiner3D&) = default;
};
SLASH_COMPONENT(CSGCombiner3D, operation, calculate_tangents, use_collision, collision_layer,
                collision_mask, layers, cast_shadow)

// Godot's CSGBox3D: a box of size centered on the entity.
struct CSGBox3D {
    Vec3 size{1.0F, 1.0F, 1.0F};
    bool flip_faces = false; // its faces point in, as Godot's CSGPrimitive3D.flip_faces
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGBox3D&, const CSGBox3D&) = default;
};
SLASH_COMPONENT(CSGBox3D, size, flip_faces, operation, calculate_tangents, use_collision,
                collision_layer, collision_mask, layers, cast_shadow)

// Godot's CSGSphere3D: a sphere around the entity, radial_segments around y and rings from pole
// to pole.
struct CSGSphere3D {
    float radius = 0.5F;
    int radial_segments = 12;
    int rings = 6;
    bool smooth_faces = true;
    bool flip_faces = false;
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGSphere3D&, const CSGSphere3D&) = default;
};
SLASH_COMPONENT(CSGSphere3D, radius, radial_segments, rings, smooth_faces, flip_faces, operation,
                calculate_tangents, use_collision, collision_layer, collision_mask, layers,
                cast_shadow)

// Godot's CSGCylinder3D: a cylinder of height along y centered on the entity, or a cone with its
// point up.
struct CSGCylinder3D {
    float radius = 0.5F;
    float height = 2.0F;
    int sides = 8;
    bool cone = false;
    bool smooth_faces = true;
    bool flip_faces = false;
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGCylinder3D&, const CSGCylinder3D&) = default;
};
SLASH_COMPONENT(CSGCylinder3D, radius, height, sides, cone, smooth_faces, flip_faces, operation,
                calculate_tangents, use_collision, collision_layer, collision_mask, layers,
                cast_shadow)

// Godot's CSGTorus3D: a ring around y between inner_radius and outer_radius.
struct CSGTorus3D {
    float inner_radius = 0.5F;
    float outer_radius = 1.0F;
    int sides = 8;
    int ring_sides = 6;
    bool smooth_faces = true;
    bool flip_faces = false;
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGTorus3D&, const CSGTorus3D&) = default;
};
SLASH_COMPONENT(CSGTorus3D, inner_radius, outer_radius, sides, ring_sides, smooth_faces, flip_faces,
                operation, calculate_tangents, use_collision, collision_layer, collision_mask,
                layers, cast_shadow)

// Godot's CSGPolygon3D: a polygon in the entity's xy plane, pushed depth along -z ("depth"),
// turned spin_degrees around y in spin_sides steps ("spin") or pushed along a Path3D's curve
// ("path"), where the path's place in the world is where it starts unless path_local.
struct CSGPolygon3D {
    std::vector<Vec2> polygon{{0.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}};
    std::string mode = "depth"; // "depth", "spin" or "path"
    float depth = 1.0F;
    float spin_degrees = 360.0F;
    int spin_sides = 8;
    entt::entity path_node{entt::null};          // the Path3D it is pushed along, in path mode
    std::string path_interval_type = "distance"; // or "subdivide"
    float path_interval = 1.0F;
    float path_simplify_angle = 0.0F;
    std::string path_rotation = "path_follow"; // "polygon", "path" or "path_follow"
    bool path_rotation_accurate = false;
    bool path_local = false;
    bool path_continuous_u = true;
    float path_u_distance = 1.0F;
    bool path_joined = false;
    bool smooth_faces = false;
    bool flip_faces = false;
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGPolygon3D&, const CSGPolygon3D&) = default;
};
SLASH_COMPONENT(CSGPolygon3D, polygon, mode, depth, spin_degrees, spin_sides, path_node,
                path_interval_type, path_interval, path_simplify_angle, path_rotation,
                path_rotation_accurate, path_local, path_continuous_u, path_u_distance, path_joined,
                smooth_faces, flip_faces, operation, calculate_tangents, use_collision,
                collision_layer, collision_mask, layers, cast_shadow)

// Godot's CSGMesh3D: a closed mesh as a shape, a model's meshes or the entity's primitive mesh.
struct CSGMesh3D {
    // A .gltf, .glb, .obj, .fbx or .blend file's meshes, placed as its nodes place them at rest;
    // "" for the entity's BoxMesh, SphereMesh, CylinderMesh, CapsuleMesh, PrismMesh or TorusMesh.
    std::string mesh;
    bool flip_faces = false;
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";

    friend bool operator==(const CSGMesh3D&, const CSGMesh3D&) = default;
};
SLASH_COMPONENT(CSGMesh3D, mesh, flip_faces, operation, calculate_tangents, use_collision,
                collision_layer, collision_mask, layers, cast_shadow)

// ---- Grid maps, as Godot's GridMap (engine/scene3d/src/gridmap.cpp) ----

// One floor of a GridMap's cells: the rows of legend keys at height y, as TileMapLayer's rows.
// Each row is a string of keys for the cells from origin's x toward +x, and each row after the
// first is one cell further toward +z.
struct GridFloor {
    int y = 0;
    Vec2i origin; // [x, z] of the cell the first key of the first row stands for
    std::vector<std::string> rows;

    friend bool operator==(const GridFloor&, const GridFloor&) = default;
};
SLASH_REFLECT(GridFloor, y, origin, rows)

// Godot's GridMap: items of a .meshlib.json mesh library placed in a grid of cells in the
// entity's Transform3D space, one item per cell, each turned one of Godot's 24 ways. Cells draw
// their items' meshes, collide as a static body of their shapes and, with bake_navigation, are
// navigation regions of their items' navigation meshes. A cell's center is cell_size times the
// cell, moved half a cell along the axes cell_center_* name.
struct GridMap {
    std::string mesh_library; // project path of a .meshlib.json file
    Vec3 cell_size{2.0F, 2.0F, 2.0F};
    int cell_octant_size = 8; // cells along each side of the blocks drawn and rebuilt together
    bool cell_center_x = true;
    bool cell_center_y = true;
    bool cell_center_z = true;
    float cell_scale = 1.0F; // scales each cell's item
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F; // its physics material's
    float bounce = 0.0F;
    bool bake_navigation = false;
    // key -> item: the item's name, then ":orientation" (0 to 23) if it is turned
    std::map<std::string, std::string> legend;
    std::vector<GridFloor> floors;

    friend bool operator==(const GridMap&, const GridMap&) = default;
};
SLASH_COMPONENT(GridMap, mesh_library, cell_size, cell_octant_size, cell_center_x, cell_center_y,
                cell_center_z, cell_scale, collision_layer, collision_mask, friction, bounce,
                bake_navigation, legend, floors)

// One of an HTerrain's four ground textures, as the HTerrain plugin's texture set: an albedo with
// a bump (its height, for depth blending) in alpha, a normal map with roughness in alpha, how many
// cells one repeat covers and how much the terrain's color map tints it.
struct TerrainTexture {
    std::string albedo_bump;      // project path of an image, sRGB; "" is white
    std::string normal_roughness; // project path of an image, OpenGL style; "" is flat, rough
    float uv_scale = 20.0F;
    float colormap_opacity = 1.0F;

    friend bool operator==(const TerrainTexture&, const TerrainTexture&) = default;
};
SLASH_REFLECT(TerrainTexture, albedo_bump, normal_roughness, uv_scale, colormap_opacity)

// A heightmap terrain, as the HTerrain plugin for Godot (Godot has no terrain node of its own):
// resolution x resolution heights, one a cell apart, in the entity's Transform3D space from its
// origin toward +X and +Z (or around it, centered), scaled by map_scale. The heightmap is a 16-bit
// gray PNG whose 0 and 65535 are height_range's ends; the splat map weighs the four textures in
// its RGBA channels and the color map tints them, its alpha under 0.5 cutting holes. A map file
// that doesn't exist yet reads as flat at height_range's low end, the first texture everywhere and
// white. It draws in chunks of chunk_size cells, coarser with distance as HTerrain's quad tree
// makes them, collides as a static height field and is walked on by navigation bakes.
struct HTerrain {
    std::string heightmap; // project path of a .height.png file
    int resolution = 513;  // heights along each side: a power of two plus one, 65 to 4097
    Vec2 height_range{0.0F, 100.0F};
    std::string splatmap;                 // project path of a PNG file
    std::string colormap;                 // project path of a PNG file
    std::vector<TerrainTexture> textures; // up to 4
    bool depth_blending = true;
    float specular = 0.5F;
    Vec3 map_scale{1.0F, 1.0F, 1.0F};
    bool centered = false;
    int chunk_size = 32;    // cells along each side of a chunk: 16, 32 or 64
    float lod_scale = 2.0F; // how far each level of detail reaches, in chunks
    bool collision_enabled = true;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F; // its physics material's
    float bounce = 0.0F;
    std::uint32_t render_layers = 1; // as MeshInstance3D's layers
    std::string cast_shadow = "on";  // as MeshInstance3D's

    friend bool operator==(const HTerrain&, const HTerrain&) = default;
};
SLASH_COMPONENT(HTerrain, heightmap, resolution, height_range, splatmap, colormap, textures,
                depth_blending, specular, map_scale, centered, chunk_size, lod_scale,
                collision_enabled, collision_layer, collision_mask, friction, bounce, render_layers,
                cast_shadow)

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
// components or, for a track with a target, on the entity targets maps that name to. A model
// file's animations, played from the file or from a .anim.json file that names them, pose the
// skeleton of the model the entity's MeshInstance3D draws.
struct AnimationPlayer {
    std::string library; // project path of a .anim.json file, or a model file
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

// An animation an AnimationTree played on its last tick: how far in it was and how much it
// counted, as the most any of its tracks did, and the model nodes that counted otherwise, as
// filters leave them.
struct TreeAnimation {
    std::string animation; // its name in the tree: "Walk", or "library/Walk"
    double time = 0.0;
    float weight = 0.0F;
    std::map<std::string, float> node_weights{}; // NOLINT(readability-redundant-member-init)
};
SLASH_REFLECT(TreeAnimation, animation, time, weight, node_weights)

// Blends animations as a .animtree.json file's tree of animation nodes says, as Godot's
// AnimationTree: each tick it works out how much each animation of its libraries counts, track by
// track, and mixes them into the components their tracks name and the pose of the model the
// entity's MeshInstance3D draws. Games set its parameters (a Blend2's blend_amount, a OneShot's
// request); the tree keeps the rest of what it needs as it plays in state.
struct AnimationTree {
    std::string tree_root; // project path of a .animtree.json file
    // The .anim.json files or models its animations come from, by library name. The tree names
    // the animations of the library named "" by themselves and others' as "library/animation".
    std::map<std::string, std::string> libraries;
    std::map<std::string, entt::entity> targets; // as AnimationPlayer's
    bool active = true;
    // Tracks no animation plays go back to their RESET values; otherwise each track's weights
    // are scaled to add up to 1, as Godot's deterministic.
    bool deterministic = true;
    // A model node whose motion is reported in root_motion_* rather than posed.
    std::string root_motion_track;
    bool root_motion_local = false; // report root motion position in the node's own frame
    // Parameters set by the game, by their names in the tree, as "speed/blend_amount"; the tree
    // sets requests back to their defaults once it has acted on them.
    json::Object parameters;
    // Parameters the tree keeps as it plays: times, fades, the current input. Empty starts the
    // tree over.
    json::Object state;
    std::vector<TreeAnimation> playing; // what the last tick played
    std::string invalid;                // why the last tick played nothing; "" when it played
    // The root motion track's motion on the last tick, and where its animations put the node.
    Vec3 root_motion_position;
    Quat root_motion_rotation;
    Vec3 root_motion_scale; // the change from 1
    Vec3 root_motion_position_accumulator;
    Quat root_motion_rotation_accumulator;
    Vec3 root_motion_scale_accumulator{1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(AnimationTree, tree_root, libraries, targets, active, deterministic,
                root_motion_track, root_motion_local, parameters, state, playing, invalid,
                root_motion_position, root_motion_rotation, root_motion_scale,
                root_motion_position_accumulator, root_motion_rotation_accumulator,
                root_motion_scale_accumulator)

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
    bool navigation_enabled = true; // each cell is a region of its tile's navigation polygons
};
SLASH_COMPONENT(TileMapLayer, tile_set, origin, legend, rows, z_index, z_as_relative, visible,
                modulate, texture_filter, light_mask, collision_enabled, occlusion_enabled,
                navigation_enabled)

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

// ---- 3D physics, as Godot's (engine/physics3d, on Jolt Physics as Godot 4.4's Jolt module): a
// body component makes its entity a physics body, placed by its Transform3D, and the entity's
// CollisionShape3D gives it a shape, as do those of the entities under it that are no body or
// area themselves. Units are meters, seconds, radians and kilograms; layers and masks are as in
// 2D.

// A shape of the entity's body or area, as Godot's CollisionShape3D with its Shape3D, moved and
// turned from the entity by position and rotation and scaled by its Transform3D's scale.
struct CollisionShape3D {
    // "box", "sphere", "capsule", "cylinder", "convex_polygon", "concave_polygon" or
    // "world_boundary".
    std::string shape = "box";
    Vec3 size{1.0F, 1.0F, 1.0F};     // box
    float radius = 0.5F;             // sphere, capsule and cylinder
    float height = 2.0F;             // capsule (end to end) and cylinder, along y
    std::vector<Vec3> points;        // convex_polygon: its convex hull is the shape
    std::vector<Vec3> faces;         // concave_polygon: triangles, three points each
    bool backface_collision = false; // concave_polygon: its faces are solid from behind too
    // world_boundary: the plane [x, y, z, d], its normal and how far along it from the entity;
    // everything below it is solid. Static bodies only.
    Vec4 plane{0.0F, 1.0F, 0.0F, 0.0F};
    float margin = 0.04F; // how much of a box, cylinder or convex polygon is rounded
    Vec3 position;
    Vec3 rotation; // radians, as Transform3D's
    bool disabled = false;

    friend bool operator==(const CollisionShape3D&, const CollisionShape3D&) = default;
};
SLASH_COMPONENT(CollisionShape3D, shape, size, radius, height, points, faces, backface_collision,
                plane, margin, position, rotation, disabled)

// A body that doesn't move on its own, as Godot's StaticBody3D. Changing its Transform3D moves it
// there at once. Its constant velocities don't move it but carry what touches it as if it moved.
struct StaticBody3D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    Vec3 constant_linear_velocity;  // meters per second
    Vec3 constant_angular_velocity; // radians per second, about each axis
};
SLASH_COMPONENT(StaticBody3D, collision_layer, collision_mask, friction, bounce,
                constant_linear_velocity, constant_angular_velocity)

// A body that goes where its Transform3D says, as Godot's AnimatableBody3D. With sync_to_physics
// it moves there during the physics step, pushing rigid bodies in its way and carrying those on
// it.
struct AnimatableBody3D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    Vec3 constant_linear_velocity; // carries what touches it, as StaticBody3D's
    Vec3 constant_angular_velocity;
    bool sync_to_physics = true;
};
SLASH_COMPONENT(AnimatableBody3D, collision_layer, collision_mask, friction, bounce,
                constant_linear_velocity, constant_angular_velocity, sync_to_physics)

// A body that gravity, collisions and forces move, as Godot's RigidBody3D. The physics step
// writes its Transform3D, velocities and sleeping back each tick.
struct RigidBody3D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    float mass = 1.0F;
    Vec3 inertia; // about each axis; 0 computes that axis's from the shapes and the mass
    float gravity_scale = 1.0F;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    std::string linear_damp_mode = "combine"; // "combine" adds to the project's; "replace"
    float linear_damp = 0.0F;
    std::string angular_damp_mode = "combine";
    float angular_damp = 0.0F;
    Vec3 constant_force;
    Vec3 constant_torque;
    bool lock_rotation = false;
    bool axis_lock_linear_x = false;
    bool axis_lock_linear_y = false;
    bool axis_lock_linear_z = false;
    bool axis_lock_angular_x = false;
    bool axis_lock_angular_y = false;
    bool axis_lock_angular_z = false;
    bool freeze = false;
    std::string freeze_mode = "static"; // "static" or "kinematic"
    bool continuous_cd = false;
    bool can_sleep = true;
    bool sleeping = false;
    bool contact_monitor = false; // emit body_entered and body_exited
};
SLASH_COMPONENT(RigidBody3D, collision_layer, collision_mask, friction, bounce, mass, inertia,
                gravity_scale, linear_velocity, angular_velocity, linear_damp_mode, linear_damp,
                angular_damp_mode, angular_damp, constant_force, constant_torque, lock_rotation,
                axis_lock_linear_x, axis_lock_linear_y, axis_lock_linear_z, axis_lock_angular_x,
                axis_lock_angular_y, axis_lock_angular_z, freeze, freeze_mode, continuous_cd,
                can_sleep, sleeping, contact_monitor)

// A body that gameplay code moves by setting its velocity, as Godot's CharacterBody3D: each tick
// the physics step runs move_and_slide, sliding it along what it hits, then writes its
// Transform3D, velocity and what it touches.
struct CharacterBody3D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::string motion_mode = "grounded"; // "grounded" (floors, walls, ceilings) or "floating"
    Vec3 up_direction{0.0F, 1.0F, 0.0F};
    Vec3 velocity;
    int max_slides = 6;
    bool slide_on_ceiling = true;
    float wall_min_slide_angle = 0.261799F; // radians
    bool floor_stop_on_slope = true;
    bool floor_constant_speed = false;
    bool floor_block_on_wall = true;
    float floor_max_angle = 0.785398F; // radians from up_direction
    float floor_snap_length = 0.1F;
    std::string platform_on_leave = "add_velocity"; // or "add_upward_velocity", "do_nothing"
    std::uint32_t platform_floor_layers = 0xFFFFFFFFU;
    std::uint32_t platform_wall_layers = 0;
    float safe_margin = 0.001F; // meters
    // What the last move found.
    bool on_floor = false;
    bool on_wall = false;
    bool on_ceiling = false;
    Vec3 floor_normal;
    Vec3 wall_normal;
    Vec3 platform_velocity; // of the moving body it stands on
};
SLASH_COMPONENT(CharacterBody3D, collision_layer, collision_mask, motion_mode, up_direction,
                velocity, max_slides, slide_on_ceiling, wall_min_slide_angle, floor_stop_on_slope,
                floor_constant_speed, floor_block_on_wall, floor_max_angle, floor_snap_length,
                platform_on_leave, platform_floor_layers, platform_wall_layers, safe_margin,
                on_floor, on_wall, on_ceiling, floor_normal, wall_normal, platform_velocity)

// A region that reports bodies and areas entering and leaving its shapes, as Godot's Area3D, and
// whose space overrides change the gravity and damping of rigid bodies in it, as Area2D's.
struct Area3D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    bool monitoring = true;  // report what enters
    bool monitorable = true; // other areas see it
    int priority = 0;        // higher overrides first
    std::string gravity_space_override = "disabled";
    bool gravity_point = false;                   // pull toward gravity_point_center
    float gravity_point_unit_distance = 0.0F;     // meters; above 0, weakens with distance squared
    Vec3 gravity_point_center{0.0F, -1.0F, 0.0F}; // in the area's space
    Vec3 gravity_direction{0.0F, -1.0F, 0.0F};
    float gravity = 9.8F; // meters per second squared
    std::string linear_damp_space_override = "disabled";
    float linear_damp = 0.1F;
    std::string angular_damp_space_override = "disabled";
    float angular_damp = 0.1F;
    // What happens to AudioStreamPlayer3D sounds in it, as Godot's Area3D: they play through
    // audio_bus_name and send to reverb_bus_name.
    bool audio_bus_override = false;
    std::string audio_bus_name = "Master";
    bool reverb_bus_enabled = false;
    std::string reverb_bus_name = "Master";
    float reverb_bus_amount = 0.0F;
    float reverb_bus_uniformity = 0.0F;
};
SLASH_COMPONENT(Area3D, collision_layer, collision_mask, monitoring, monitorable, priority,
                gravity_space_override, gravity_point, gravity_point_unit_distance,
                gravity_point_center, gravity_direction, gravity, linear_damp_space_override,
                linear_damp, angular_damp_space_override, angular_damp, audio_bus_override,
                audio_bus_name, reverb_bus_enabled, reverb_bus_name, reverb_bus_amount,
                reverb_bus_uniformity)

// 3D joints, as Godot's Joint3D nodes on its Jolt module: node_a and node_b name two bodies, and
// a missing one is the world. The joint is made at the entity's Transform3D, after its parents',
// once both bodies exist, and stays there when a parent carries the entity away; changing the
// entity's own Transform3D or bodies makes it again. Parameters Godot's Jolt module ignores
// (bias, softness, relaxation and the like) are left out.

// Pins two bodies together at a point, letting them turn every way, as Godot's PinJoint3D.
struct PinJoint3D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool exclude_nodes_from_collision = true;

    friend bool operator==(const PinJoint3D&, const PinJoint3D&) = default;
};
SLASH_COMPONENT(PinJoint3D, node_a, node_b, exclude_nodes_from_collision)

// Lets two bodies turn about the entity's z axis only, as a door's hinge, as Godot's
// HingeJoint3D.
struct HingeJoint3D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool exclude_nodes_from_collision = true;
    bool angular_limit_enabled = false;
    float angular_limit_upper = 1.5707964F;  // radians
    float angular_limit_lower = -1.5707964F; // radians
    bool motor_enabled = false;
    float motor_target_velocity = 1.0F; // radians per second
    float motor_max_impulse = 1.0F;     // per tick

    friend bool operator==(const HingeJoint3D&, const HingeJoint3D&) = default;
};
SLASH_COMPONENT(HingeJoint3D, node_a, node_b, exclude_nodes_from_collision, angular_limit_enabled,
                angular_limit_upper, angular_limit_lower, motor_enabled, motor_target_velocity,
                motor_max_impulse)

// Lets two bodies slide along the entity's x axis only, as Godot's SliderJoint3D.
struct SliderJoint3D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool exclude_nodes_from_collision = true;
    float linear_limit_upper = 1.0F;  // meters
    float linear_limit_lower = -1.0F; // meters

    friend bool operator==(const SliderJoint3D&, const SliderJoint3D&) = default;
};
SLASH_COMPONENT(SliderJoint3D, node_a, node_b, exclude_nodes_from_collision, linear_limit_upper,
                linear_limit_lower)

// Lets two bodies swing in a cone about the entity's x axis and twist about it, as a shoulder,
// as Godot's ConeTwistJoint3D.
struct ConeTwistJoint3D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool exclude_nodes_from_collision = true;
    float swing_span = 0.7853982F;                // radians
    float twist_span = std::numbers::pi_v<float>; // radians

    friend bool operator==(const ConeTwistJoint3D&, const ConeTwistJoint3D&) = default;
};
SLASH_COMPONENT(ConeTwistJoint3D, node_a, node_b, exclude_nodes_from_collision, swing_span,
                twist_span)

// One axis of a Generic6DOFJoint3D along which the bodies move.
struct Joint3DLinearAxis {
    bool limit_enabled = true;
    float limit_upper = 0.0F; // meters
    float limit_lower = 0.0F; // meters
    bool motor_enabled = false;
    float motor_target_velocity = 0.0F; // meters per second
    float motor_force_limit = 0.0F;     // newtons
    bool spring_enabled = false;
    float spring_stiffness = 0.01F;
    float spring_damping = 0.01F;
    float spring_equilibrium_point = 0.0F; // meters

    friend bool operator==(const Joint3DLinearAxis&, const Joint3DLinearAxis&) = default;
};
SLASH_REFLECT(Joint3DLinearAxis, limit_enabled, limit_upper, limit_lower, motor_enabled,
              motor_target_velocity, motor_force_limit, spring_enabled, spring_stiffness,
              spring_damping, spring_equilibrium_point)

// One axis of a Generic6DOFJoint3D about which the bodies turn.
struct Joint3DAngularAxis {
    bool limit_enabled = true;
    float limit_upper = 0.0F; // radians
    float limit_lower = 0.0F; // radians
    bool motor_enabled = false;
    float motor_target_velocity = 0.0F; // radians per second
    float motor_force_limit = 300.0F;   // newton-meters
    bool spring_enabled = false;
    float spring_stiffness = 0.0F;
    float spring_damping = 0.0F;
    float spring_equilibrium_point = 0.0F; // radians

    friend bool operator==(const Joint3DAngularAxis&, const Joint3DAngularAxis&) = default;
};
SLASH_REFLECT(Joint3DAngularAxis, limit_enabled, limit_upper, limit_lower, motor_enabled,
              motor_target_velocity, motor_force_limit, spring_enabled, spring_stiffness,
              spring_damping, spring_equilibrium_point)

// Limits, drives or springs each of the six ways two bodies move apart, along and about the
// entity's axes, as Godot's Generic6DOFJoint3D. By default every axis is locked.
struct Generic6DOFJoint3D {
    entt::entity node_a{entt::null};
    entt::entity node_b{entt::null};
    bool exclude_nodes_from_collision = true;
    Joint3DLinearAxis linear_x;
    Joint3DLinearAxis linear_y;
    Joint3DLinearAxis linear_z;
    Joint3DAngularAxis angular_x;
    Joint3DAngularAxis angular_y;
    Joint3DAngularAxis angular_z;

    friend bool operator==(const Generic6DOFJoint3D&, const Generic6DOFJoint3D&) = default;
};
SLASH_COMPONENT(Generic6DOFJoint3D, node_a, node_b, exclude_nodes_from_collision, linear_x,
                linear_y, linear_z, angular_x, angular_y, angular_z)

// A car, as Godot's VehicleBody3D: the RigidBody3D on the same entity rolls on the
// VehicleWheel3D entities right under it. Its engine force, brake and steering go to its wheels
// when they change. Godot's VehicleBody3D is a rigid body of 40 kilograms.
struct VehicleBody3D {
    float engine_force = 0.0F; // newtons, to the wheels used as traction
    float brake = 0.0F;        // to every wheel
    float steering = 0.0F;     // radians, to the wheels used as steering; positive turns left
};
SLASH_COMPONENT(VehicleBody3D, engine_force, brake, steering)

// A wheel of the VehicleBody3D its entity is right under, as Godot's VehicleWheel3D: a ray cast
// down its y axis is its suspension, and its x axis is its axle. The physics step rolls, steers
// and lifts it from where it is mounted, writing its Transform3D, and writes what it found.
struct VehicleWheel3D {
    float engine_force = 0.0F; // newtons
    float brake = 0.0F;
    float steering = 0.0F; // radians
    bool use_as_traction = false;
    bool use_as_steering = false;
    float wheel_roll_influence = 0.1F;
    float wheel_radius = 0.5F;       // meters
    float wheel_rest_length = 0.15F; // meters
    float wheel_friction_slip = 10.5F;
    float suspension_travel = 0.2F; // meters
    float suspension_stiffness = 5.88F;
    float suspension_max_force = 6000.0F; // newtons
    float damping_compression = 0.83F;
    float damping_relaxation = 0.88F;
    // What the last step found.
    bool in_contact = false;
    Vec3 contact_point;
    Vec3 contact_normal;
    entt::entity contact_body{entt::null};
    float suspension_length = 0.0F; // meters
    float rotation = 0.0F;          // radians it has rolled
    float rpm = 0.0F;
    float skid_info = 0.0F; // 1 grips, toward 0 skids
};
SLASH_COMPONENT(VehicleWheel3D, engine_force, brake, steering, use_as_traction, use_as_steering,
                wheel_roll_influence, wheel_radius, wheel_rest_length, wheel_friction_slip,
                suspension_travel, suspension_stiffness, suspension_max_force, damping_compression,
                damping_relaxation, in_contact, contact_point, contact_normal, contact_body,
                suspension_length, rotation, rpm, skid_info)

// The ragdoll of a model, as Godot 4.6's PhysicalBoneSimulator3D: a skeleton modifier on the entity
// that draws the model or on one of its Transform3D children, whose PhysicalBone3D children are
// bodies for the model's bones. While it simulates, physics moves the bones it names (and those
// under them, or every bone when it names none) and they pose the model; the other bones' bodies
// follow the model's pose as kinematic bodies, while it is active.
struct PhysicalBoneSimulator3D {
    bool active = true;
    float influence = 1.0F; // how far simulated bones replace the pose, 0 to 1
    // Godot's physical_bones_start_simulation and physical_bones_stop_simulation.
    bool simulating = false;
    std::vector<std::string> simulated_bones;       // with the bones under them; empty for all
    std::vector<entt::entity> collision_exceptions; // bodies its bones never collide with

    friend bool operator==(const PhysicalBoneSimulator3D&,
                           const PhysicalBoneSimulator3D&) = default;
};
SLASH_COMPONENT(PhysicalBoneSimulator3D, active, influence, simulating, simulated_bones,
                collision_exceptions)

// One axis of a PhysicalBone3D's "6dof" joint, along and about which the bodies move.
struct PhysicalBone3DAxis {
    bool linear_limit_enabled = true;
    float linear_limit_upper = 0.0F; // meters
    float linear_limit_lower = 0.0F; // meters
    bool linear_spring_enabled = false;
    float linear_spring_stiffness = 0.0F;
    float linear_spring_damping = 0.0F;
    float linear_equilibrium_point = 0.0F; // meters
    bool angular_limit_enabled = true;
    float angular_limit_upper = 0.0F; // radians
    float angular_limit_lower = 0.0F; // radians
    bool angular_spring_enabled = false;
    float angular_spring_stiffness = 0.0F;
    float angular_spring_damping = 0.0F;
    float angular_equilibrium_point = 0.0F; // radians

    friend bool operator==(const PhysicalBone3DAxis&, const PhysicalBone3DAxis&) = default;
};
SLASH_REFLECT(PhysicalBone3DAxis, linear_limit_enabled, linear_limit_upper, linear_limit_lower,
              linear_spring_enabled, linear_spring_stiffness, linear_spring_damping,
              linear_equilibrium_point, angular_limit_enabled, angular_limit_upper,
              angular_limit_lower, angular_spring_enabled, angular_spring_stiffness,
              angular_spring_damping, angular_equilibrium_point)

// A body for one bone of a model, as Godot's PhysicalBone3D: a Transform3D child of a
// PhysicalBoneSimulator3D's entity, shaped by its CollisionShape3D or those of entities under it,
// placed at the bone times body_offset. Its joint holds it to the body of the nearest bone above
// its own that has one, at its bone's origin. The physics step writes its Transform3D (top_level
// while it simulates) and velocities, and the skeleton_modifiers system its Transform3D while it
// follows the pose. Joint parameters Godot's Jolt module ignores are left out.
struct PhysicalBone3D {
    std::string bone_name;
    Vec3 body_offset_position;       // where the body is in the bone's space
    Vec3 body_offset_rotation;       // radians, as Transform3D's
    std::string joint_type = "none"; // "none", "pin", "cone", "hinge", "slider" or "6dof"
    Vec3 joint_rotation;             // the joint's axes in the body's space, radians
    float swing_span = 0.7853982F;   // cone, radians
    float twist_span = std::numbers::pi_v<float>; // cone, radians
    bool angular_limit_enabled = false;           // hinge
    float angular_limit_upper = 1.5707964F;       // hinge, radians
    float angular_limit_lower = -1.5707964F;      // hinge, radians
    float linear_limit_upper = 1.0F;              // slider, meters
    float linear_limit_lower = -1.0F;             // slider, meters
    PhysicalBone3DAxis x;                         // 6dof
    PhysicalBone3DAxis y;                         // 6dof
    PhysicalBone3DAxis z;                         // 6dof
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float mass = 1.0F;
    float friction = 1.0F;
    float bounce = 0.0F;
    float gravity_scale = 1.0F;
    std::string linear_damp_mode = "combine"; // "combine" adds to the project's; "replace"
    float linear_damp = 0.0F;
    std::string angular_damp_mode = "combine";
    float angular_damp = 0.0F;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    bool can_sleep = true;

    friend bool operator==(const PhysicalBone3D&, const PhysicalBone3D&) = default;
};
SLASH_COMPONENT(PhysicalBone3D, bone_name, body_offset_position, body_offset_rotation, joint_type,
                joint_rotation, swing_span, twist_span, angular_limit_enabled, angular_limit_upper,
                angular_limit_lower, linear_limit_upper, linear_limit_lower, x, y, z,
                collision_layer, collision_mask, mass, friction, bounce, gravity_scale,
                linear_damp_mode, linear_damp, angular_damp_mode, angular_damp, linear_velocity,
                angular_velocity, can_sleep)

// ---- Navigation, as Godot's (engine/navigation): regions of navigation mesh, links between
// places and agents that follow paths over them, on one 2D and one 3D map per world.

// Places a .navpoly.json navigation polygon on the 2D map at the entity's Transform2D, as Godot's
// NavigationRegion2D. Regions join where their edges meet or come within the map's
// edge_connection_margin.
struct NavigationRegion2D {
    std::string navigation_polygon; // a .navpoly.json file
    bool enabled = true;
    bool use_edge_connections = true; // join edges near other regions' edges
    std::uint32_t navigation_layers = 1;
    float enter_cost = 0.0F;  // added to a path that enters it from another region
    float travel_cost = 1.0F; // what each unit of distance in it costs

    friend bool operator==(const NavigationRegion2D&, const NavigationRegion2D&) = default;
};
SLASH_COMPONENT(NavigationRegion2D, navigation_polygon, enabled, use_edge_connections,
                navigation_layers, enter_cost, travel_cost)

// Places a .navmesh.json navigation mesh on the 3D map at the entity's Transform3D, as Godot's
// NavigationRegion3D.
struct NavigationRegion3D {
    std::string navigation_mesh; // a .navmesh.json file
    bool enabled = true;
    bool use_edge_connections = true;
    std::uint32_t navigation_layers = 1;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;

    friend bool operator==(const NavigationRegion3D&, const NavigationRegion3D&) = default;
};
SLASH_COMPONENT(NavigationRegion3D, navigation_mesh, enabled, use_edge_connections,
                navigation_layers, enter_cost, travel_cost)

// A way between two places on the 2D map that the regions don't join, such as a jump or a
// ladder, as Godot's NavigationLink2D: start_position and end_position are in the entity's
// Transform2D space, and each end joins the closest region polygon within the map's
// link_connection_radius.
struct NavigationLink2D {
    bool enabled = true;
    bool bidirectional = true; // false: only from start to end
    std::uint32_t navigation_layers = 1;
    Vec2 start_position;
    Vec2 end_position;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;

    friend bool operator==(const NavigationLink2D&, const NavigationLink2D&) = default;
};
SLASH_COMPONENT(NavigationLink2D, enabled, bidirectional, navigation_layers, start_position,
                end_position, enter_cost, travel_cost)

// The same on the 3D map, as Godot's NavigationLink3D.
struct NavigationLink3D {
    bool enabled = true;
    bool bidirectional = true;
    std::uint32_t navigation_layers = 1;
    Vec3 start_position;
    Vec3 end_position;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;

    friend bool operator==(const NavigationLink3D&, const NavigationLink3D&) = default;
};
SLASH_COMPONENT(NavigationLink3D, enabled, bidirectional, navigation_layers, start_position,
                end_position, enter_cost, travel_cost)

// Finds a path on the 2D map from the entity's Transform2D to target_position and follows its
// progress, as Godot's NavigationAgent2D. Setting target_position asks for a path; gameplay code
// moves the entity toward navigation::next_path_position and the agent emits path_changed,
// waypoint_reached, link_reached, target_reached and navigation_finished events as it goes. It
// moves nothing itself.
//
// With avoidance_enabled, each tick the navigation system turns velocity, the velocity gameplay
// code wants it to move at, into a safe velocity that keeps it clear of other agents and of
// obstacles, with RVO2 as Godot's, and emits it as velocity_computed (also
// navigation::safe_velocity_2d). velocity counts only while the agent follows a path.
struct NavigationAgent2D {
    Vec2 target_position;
    Vec2 velocity;
    float path_desired_distance = 20.0F;   // how near a waypoint counts as reaching it
    float target_desired_distance = 10.0F; // how near target_position counts as reaching it
    float path_max_distance = 100.0F;      // how far off the path before it is found again
    std::uint32_t navigation_layers = 1;
    std::string path_postprocessing = "corridorfunnel"; // "corridorfunnel", "edgecentered", "none"
    bool simplify_path = false;
    bool avoidance_enabled = false;
    float simplify_epsilon = 0.0F;
    float path_return_max_length = 0.0F;   // 0: no limit
    float path_return_max_radius = 0.0F;   // 0: no limit
    int path_search_max_polygons = 4096;   // 0: no limit
    float path_search_max_distance = 0.0F; // 0: no limit
    float radius = 10.0F;
    float neighbor_distance = 500.0F;    // how far it looks for other agents
    int max_neighbors = 10;              // how many of them it avoids at most
    float time_horizon_agents = 1.0F;    // how many seconds ahead it avoids other agents
    float time_horizon_obstacles = 0.0F; // and obstacles' outlines
    float max_speed = 100.0F;
    std::uint32_t avoidance_layers = 1; // the layers it is on, for others' avoidance_mask
    std::uint32_t avoidance_mask = 1;   // the layers of what it avoids
    float avoidance_priority = 1.0F;    // from 0 to 1: it leaves agents of higher ones alone

    friend bool operator==(const NavigationAgent2D&, const NavigationAgent2D&) = default;
};
SLASH_COMPONENT(NavigationAgent2D, target_position, path_desired_distance, target_desired_distance,
                path_max_distance, navigation_layers, path_postprocessing, simplify_path,
                simplify_epsilon, path_return_max_length, path_return_max_radius,
                path_search_max_polygons, path_search_max_distance, avoidance_enabled, velocity,
                radius, neighbor_distance, max_neighbors, time_horizon_agents,
                time_horizon_obstacles, max_speed, avoidance_layers, avoidance_mask,
                avoidance_priority)

// The same on the 3D map from the entity's Transform3D, as Godot's NavigationAgent3D. Its
// avoidance is on the ground (x and z) unless use_3d_avoidance: there it is kept from what is
// below or above it by height, and with keep_y_velocity the safe velocity keeps velocity's y.
struct NavigationAgent3D {
    Vec3 target_position;
    Vec3 velocity;
    float path_desired_distance = 1.0F;
    float target_desired_distance = 1.0F;
    float path_height_offset = 0.0F; // how far the entity's origin is above the mesh
    float path_max_distance = 5.0F;
    std::uint32_t navigation_layers = 1;
    std::string path_postprocessing = "corridorfunnel";
    bool simplify_path = false;
    bool avoidance_enabled = false;
    bool use_3d_avoidance = false;
    bool keep_y_velocity = true;
    float simplify_epsilon = 0.0F;
    float path_return_max_length = 0.0F;
    float path_return_max_radius = 0.0F;
    int path_search_max_polygons = 4096;
    float path_search_max_distance = 0.0F;
    float height = 1.0F;
    float radius = 0.5F;
    float neighbor_distance = 50.0F;
    int max_neighbors = 10;
    float time_horizon_agents = 1.0F;
    float time_horizon_obstacles = 0.0F;
    float max_speed = 10.0F;
    std::uint32_t avoidance_layers = 1;
    std::uint32_t avoidance_mask = 1;
    float avoidance_priority = 1.0F;

    friend bool operator==(const NavigationAgent3D&, const NavigationAgent3D&) = default;
};
SLASH_COMPONENT(NavigationAgent3D, target_position, path_desired_distance, target_desired_distance,
                path_height_offset, path_max_distance, navigation_layers, path_postprocessing,
                simplify_path, simplify_epsilon, path_return_max_length, path_return_max_radius,
                path_search_max_polygons, path_search_max_distance, avoidance_enabled, velocity,
                height, radius, neighbor_distance, max_neighbors, time_horizon_agents,
                time_horizon_obstacles, max_speed, use_3d_avoidance, keep_y_velocity,
                avoidance_layers, avoidance_mask, avoidance_priority)

// Something agents keep clear of on the 2D map, at the entity's Transform2D, as Godot's
// NavigationObstacle2D: a circle of radius that moves at velocity, avoided as an agent that
// doesn't avoid back, and an outline of vertices that agents keep out of (or, wound the other
// way, in). With affect_navigation_mesh, navigation.bake also takes it out of the regions it is
// under: shrunk around by their agent_radius, or exactly as it is with carve_navigation_mesh.
struct NavigationObstacle2D {
    std::vector<Vec2> vertices;
    Vec2 velocity;
    float radius = 0.0F;
    std::uint32_t avoidance_layers = 1;
    bool avoidance_enabled = true;
    bool affect_navigation_mesh = false;
    bool carve_navigation_mesh = false;

    friend bool operator==(const NavigationObstacle2D&, const NavigationObstacle2D&) = default;
};
SLASH_COMPONENT(NavigationObstacle2D, radius, vertices, avoidance_enabled, velocity,
                avoidance_layers, affect_navigation_mesh, carve_navigation_mesh)

// The same on the 3D map, as Godot's NavigationObstacle3D. Its outline is on the ground: the
// vertices' y is not used, only the entity's turn about y counts, and it reaches height up from
// the entity. The circle is a sphere with use_3d_avoidance, for agents that use it too.
struct NavigationObstacle3D {
    std::vector<Vec3> vertices;
    Vec3 velocity;
    float radius = 0.0F;
    float height = 1.0F;
    std::uint32_t avoidance_layers = 1;
    bool avoidance_enabled = true;
    bool use_3d_avoidance = false;
    bool affect_navigation_mesh = false;
    bool carve_navigation_mesh = false;

    friend bool operator==(const NavigationObstacle3D&, const NavigationObstacle3D&) = default;
};
SLASH_COMPONENT(NavigationObstacle3D, radius, height, vertices, avoidance_enabled, velocity,
                avoidance_layers, use_3d_avoidance, affect_navigation_mesh, carve_navigation_mesh)

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
    // Which speakers play it in surround, as Godot's mix_target: "stereo" (the front pair),
    // "surround" (every speaker) or "center".
    std::string mix_target = "stereo";
    // The stream's parameters, as Godot's parameters/*: "switch_to_clip" for interactive music.
    std::map<std::string, json::Value> parameters;

    friend bool operator==(const AudioStreamPlayer&, const AudioStreamPlayer&) = default;
};
SLASH_COMPONENT(AudioStreamPlayer, stream, volume_db, pitch_scale, playing, autoplay, stream_paused,
                max_polyphony, bus, mix_target, parameters)

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

// Plays a sound at the entity's Transform3D, as Godot's AudioStreamPlayer3D: quieter with
// distance from the listener, panned by where it is around it, muffled with distance and
// outside its emission cone, pitched by doppler, and sent to the buses of an Area3D it is in.
struct AudioStreamPlayer3D {
    std::string stream; // project path of a .wav, .ogg, .mp3 or .audio.json file
    // "inverse_distance", "inverse_square_distance", "logarithmic" or "disabled".
    std::string attenuation_model = "inverse_distance";
    float volume_db = 0.0F;
    float unit_size = 10.0F; // meters at which it is at volume_db
    float max_db = 3.0F;     // the loudest it gets, however close
    float pitch_scale = 1.0F;
    bool playing = false;
    bool autoplay = false;
    bool stream_paused = false;
    float max_distance = 0.0F; // meters; silent beyond; 0 for no limit
    int max_polyphony = 1;
    float panning_strength = 1.0F;
    std::string bus = "Master";
    std::uint32_t area_mask = 1; // Area3D layers whose bus override and reverb apply; 0 for none
    bool emission_angle_enabled = false;
    float emission_angle_degrees = 45.0F; // half the cone it faces down its -z
    float emission_angle_filter_attenuation_db = -12.0F;
    float attenuation_filter_cutoff_hz = 5000.0F;
    float attenuation_filter_db = -24.0F;
    std::string doppler_tracking = "disabled"; // "disabled", "idle_step" or "physics_step"
    std::map<std::string, json::Value> parameters;

    friend bool operator==(const AudioStreamPlayer3D&, const AudioStreamPlayer3D&) = default;
};
SLASH_COMPONENT(AudioStreamPlayer3D, stream, attenuation_model, volume_db, unit_size, max_db,
                pitch_scale, playing, autoplay, stream_paused, max_distance, max_polyphony,
                panning_strength, bus, area_mask, emission_angle_enabled, emission_angle_degrees,
                emission_angle_filter_attenuation_db, attenuation_filter_cutoff_hz,
                attenuation_filter_db, doppler_tracking, parameters)

// Where 3D sounds are heard from, as Godot's AudioListener3D, in place of the current Camera3D.
struct AudioListener3D {
    bool current = false;
    std::string doppler_tracking = "disabled"; // "disabled", "idle_step" or "physics_step"

    friend bool operator==(const AudioListener3D&, const AudioListener3D&) = default;
};
SLASH_COMPONENT(AudioListener3D, current, doppler_tracking)

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
