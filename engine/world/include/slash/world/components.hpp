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

// Position, rotation and scale in 2D. Units are pixels, and y points down, as in Godot.
struct Transform2D {
    Vec2 position;
    float rotation = 0.0F; // radians, clockwise
    Vec2 scale{1.0F, 1.0F};
};
SLASH_COMPONENT(Transform2D, position, rotation, scale)

// Position, rotation and scale in 3D. Units are meters, and y points up, as in Godot.
struct Transform3D {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(Transform3D, position, rotation, scale)

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
    std::string texture; // project path of a PNG or BMP file; "" draws nothing
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
                region, modulate, z_index, visible, texture_filter, material, material_params,
                light_mask, normal_texture, specular_texture, specular_color, specular_shininess)

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

// Plays keyframe animations from a .anim.json file on the entity's own components, as Godot's
// AnimationPlayer: each tick it moves the time on and sets every field the animation's tracks
// name.
struct AnimationPlayer {
    std::string library; // project path of a .anim.json file
    std::string animation;
    bool playing = true;
    float speed_scale = 1.0F;
    double time = 0.0;      // seconds into the animation
    bool backwards = false; // play toward the start; pingpong loops flip it at each end
};
SLASH_COMPONENT(AnimationPlayer, library, animation, playing, speed_scale, time, backwards)

// A grid of tiles from a .tileset.json file, as Godot's TileMapLayer, drawn at the entity's
// Transform2D with cell [0, 0]'s top left corner there. Its cells are text: each row is a string
// of keys, each key standing for the tile the legend names, and "." for an empty cell. The
// tilemap commands keep them in canonical form, so most edits go through them.
struct TileMapLayer {
    std::string tile_set;                      // project path of a .tileset.json file
    Vec2i origin;                              // the cell the first key of the first row stands for
    std::map<std::string, std::string> legend; // key -> tile: a tile's name or "source:x,y"
    std::vector<std::string> rows;
    int z_index = 0;
    bool visible = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    std::string texture_filter = "inherit"; // "linear", "nearest" or "inherit"
    int light_mask = 1;                     // lit by lights sharing a bit
};
SLASH_COMPONENT(TileMapLayer, tile_set, origin, legend, rows, z_index, visible, modulate,
                texture_filter, light_mask)

// A light shining from the entity's Transform2D (or the origin) through a texture, as Godot's
// PointLight2D. It lights the sprites and tile map layers whose light_mask shares a bit with
// range_item_cull_mask and whose z_index is in its range, and with shadow_enabled, the
// LightOccluder2D polygons whose occluder_light_mask shares a bit with shadow_item_cull_mask
// cast shadows from it.
struct PointLight2D {
    bool enabled = true;
    std::string texture; // project path of a PNG or BMP file; "" is a soft round light
    float texture_scale = 1.0F;
    Vec2 offset; // moves the texture from the light, in pixels
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float energy = 1.0F;
    std::string blend_mode = "add"; // "add", "sub" or "mix"
    float height = 0.0F;            // pixels above the plane, for normal maps
    int range_z_min = -1024;
    int range_z_max = 1024;
    int range_item_cull_mask = 1;
    bool shadow_enabled = false;
    Vec4 shadow_color{0.0F, 0.0F, 0.0F, 0.0F};
    std::string shadow_filter = "none"; // "none", "pcf5" or "pcf13"
    float shadow_filter_smooth = 0.0F;
    int shadow_item_cull_mask = 1;
};
SLASH_COMPONENT(PointLight2D, enabled, texture, texture_scale, offset, color, energy, blend_mode,
                height, range_z_min, range_z_max, range_item_cull_mask, shadow_enabled,
                shadow_color, shadow_filter, shadow_filter_smooth, shadow_item_cull_mask)

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
    int range_item_cull_mask = 1;
    bool shadow_enabled = false;
    Vec4 shadow_color{0.0F, 0.0F, 0.0F, 0.0F};
    std::string shadow_filter = "none"; // "none", "pcf5" or "pcf13"
    float shadow_filter_smooth = 0.0F;
    int shadow_item_cull_mask = 1;
};
SLASH_COMPONENT(DirectionalLight2D, enabled, color, energy, blend_mode, height, max_distance,
                range_z_min, range_z_max, range_item_cull_mask, shadow_enabled, shadow_color,
                shadow_filter, shadow_filter_smooth, shadow_item_cull_mask)

// A polygon that casts shadows from 2D lights, as Godot's LightOccluder2D with its
// OccluderPolygon2D, placed by the entity's Transform2D if it has one.
struct LightOccluder2D {
    std::vector<Vec2> polygon;          // points in pixels
    bool closed = true;                 // the last point joins the first
    std::string cull_mode = "disabled"; // "disabled", "clockwise" or "counter_clockwise"
    int occluder_light_mask = 1;
};
SLASH_COMPONENT(LightOccluder2D, polygon, closed, cull_mode, occluder_light_mask)

// Tints everything lit, as Godot's CanvasModulate: the light a scene has where no light reaches.
// One per scene.
struct CanvasModulate {
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(CanvasModulate, color)

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
// Changing its Transform2D moves it there at once.
struct StaticBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
};
SLASH_COMPONENT(StaticBody2D, collision_layer, collision_mask, friction, bounce)

// A body that goes where its Transform2D says, as Godot's AnimatableBody2D: moving platforms and
// doors that gameplay code or an AnimationPlayer moves. With sync_to_physics it moves there
// during the physics step, pushing bodies in its way and carrying those on it.
struct AnimatableBody2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    bool sync_to_physics = true;
};
SLASH_COMPONENT(AnimatableBody2D, collision_layer, collision_mask, friction, bounce,
                sync_to_physics)

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
    bool floor_stop_on_slope = true;
    float floor_max_angle = 0.785398F; // radians from up_direction
    float floor_snap_length = 1.0F;
    bool slide_on_ceiling = true;
    std::string platform_on_leave = "add_velocity"; // or "add_upward_velocity", "do_nothing"
    // What the last move found.
    bool on_floor = false;
    bool on_wall = false;
    bool on_ceiling = false;
    Vec2 floor_normal;
    Vec2 wall_normal;
    Vec2 platform_velocity; // of the moving body it stands on
};
SLASH_COMPONENT(CharacterBody2D, collision_layer, collision_mask, motion_mode, up_direction,
                velocity, max_slides, floor_stop_on_slope, floor_max_angle, floor_snap_length,
                slide_on_ceiling, platform_on_leave, on_floor, on_wall, on_ceiling, floor_normal,
                wall_normal, platform_velocity)

// A region that reports bodies and areas entering and leaving its shapes, as Godot's Area2D:
// body_entered, body_exited, area_entered and area_exited events. It goes where its Transform2D
// says and nothing collides with it.
struct Area2D {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    bool monitoring = true;  // report what enters
    bool monitorable = true; // other areas see it
};
SLASH_COMPONENT(Area2D, collision_layer, collision_mask, monitoring, monitorable)

// Pins two bodies together at the entity's Transform2D, letting them turn, as Godot's
// PinJoint2D. A missing node_a pins node_b to the world.
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

// A spring from node_a at the entity's Transform2D to node_b, length pixels along the entity's
// y axis, as Godot's DampedSpringJoint2D.
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

// Keeps node_b on a groove of node_a's, from the entity's Transform2D length pixels along its y
// axis, as Godot's GrooveJoint2D; node_b slides along it and turns freely.
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
    std::string texture;              // project path of a PNG or BMP file; "" draws pixels
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
                texture, texture_filter, hframes, vframes, anim_loop, z_index, visible, material,
                material_params, light_mask, emission_shape, emission_sphere_radius,
                emission_rect_extents, emission_ring_radius, emission_ring_inner_radius,
                emission_points, emission_normals, emission_colors, particle_flag_align_y,
                direction, spread, gravity, initial_velocity_min, initial_velocity_max,
                angular_velocity_min, angular_velocity_max, angular_velocity_curve,
                orbit_velocity_min, orbit_velocity_max, orbit_velocity_curve, linear_accel_min,
                linear_accel_max, linear_accel_curve, radial_accel_min, radial_accel_max,
                radial_accel_curve, tangential_accel_min, tangential_accel_max,
                tangential_accel_curve, damping_min, damping_max, damping_curve, angle_min,
                angle_max, angle_curve, scale_amount_min, scale_amount_max, scale_amount_curve,
                color, color_ramp, color_initial_ramp, hue_variation_min, hue_variation_max,
                hue_variation_curve, anim_speed_min, anim_speed_max, anim_speed_curve,
                anim_offset_min, anim_offset_max, anim_offset_curve)

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
    std::map<std::string, Vec4> theme_override_colors;
    std::map<std::string, int> theme_override_constants;
    std::map<std::string, std::string> theme_override_fonts;
    std::map<std::string, int> theme_override_font_sizes;
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
    int z_index = 0;
    bool visible = true;

    friend bool operator==(const Control&, const Control&) = default;
};
SLASH_COMPONENT(Control, children, anchor_left, anchor_top, anchor_right, anchor_bottom,
                offset_left, offset_top, offset_right, offset_bottom, grow_horizontal,
                grow_vertical, custom_minimum_size, rotation, scale, pivot_offset, visible,
                modulate, self_modulate, z_index, theme_override_colors, theme_override_constants,
                theme_override_fonts, theme_override_font_sizes)

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
    int lines_skipped = 0;
    int max_lines_visible = -1;  // -1 for all
    int visible_characters = -1; // -1 for all
    bool clip_text = false;
    bool uppercase = false;

    friend bool operator==(const Label&, const Label&) = default;
};
SLASH_COMPONENT(Label, text, horizontal_alignment, vertical_alignment, autowrap_mode, clip_text,
                text_overrun_behavior, ellipsis_char, uppercase, lines_skipped, max_lines_visible,
                visible_characters, visible_characters_behavior)

// A rect of one color, as Godot's ColorRect.
struct ColorRect {
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};

    friend bool operator==(const ColorRect&, const ColorRect&) = default;
};
SLASH_COMPONENT(ColorRect, color)

} // namespace slash::world
