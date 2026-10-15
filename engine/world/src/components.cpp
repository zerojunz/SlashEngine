#include "slash/world/components.hpp"

#include "slash/world/component_registry.hpp"

namespace slash::world {

namespace {

// Godot's 20 render layers.
constexpr double kMaxMask = 1048575;

// The 2D physics components. Fields every body has come first.
void register_physics_components(ComponentRegistry& registry) {
    const FieldMeta layer{.doc = "The physics layers it is in, as bits: layer 1 is 1, layer 2 is "
                                 "2, layer 3 is 4."};
    const FieldMeta mask{.doc = "The layers it collides with or detects, as bits."};
    const FieldMeta friction{.doc = "How much it resists sliding: 0 is ice. Two bodies use the "
                                    "smaller friction.",
                             .min = 0};
    const FieldMeta bounce{
        .doc = "How much it bounces: 0 not at all, 1 fully. Two bodies add theirs, up to 1.",
        .min = 0,
        .max = 1};
    const FieldMeta one_way{.doc = "Collide only with what comes from the shape's -y side, as "
                                   "platforms one can jump through from below."};
    const FieldMeta one_way_margin{
        .doc = "How deep something may be in a one-way shape and still land on it, in pixels.",
        .min = 0};
    const FieldMeta node_a{.doc = "ID of the first body; null attaches node_b to the world."};
    const FieldMeta node_b{.doc = "ID of the second body."};
    const FieldMeta disable_collision{.doc = "The two bodies don't collide with each other."};

    registry.add<CollisionShape2D>()
        .set_doc("A shape of the entity's physics body or area, moved by position and rotation "
                 "and scaled by its Transform2D.")
        .describe("shape", {.doc = "Which shape; the fields after it size it.",
                            .choices = {"rectangle", "circle", "capsule", "segment", "polygon"}})
        .describe("size", {.doc = "Rectangle width and height, in pixels.", .min = 0})
        .describe("radius", {.doc = "Circle and capsule radius, in pixels.", .min = 0})
        .describe("height", {.doc = "Capsule height end to end along y, in pixels.", .min = 0})
        .describe("a", {.doc = "Segment start, in pixels."})
        .describe("b", {.doc = "Segment end, in pixels."})
        .describe("points", {.doc = "Polygon points in pixels; the shape is their convex hull. "
                                    "Use CollisionPolygon2D for concave ones."})
        .describe("position", {.doc = "Moves the shape from the entity, in pixels."})
        .describe("rotation", {.doc = "Turns the shape, in radians clockwise."})
        .describe("disabled", {.doc = "Leave the shape out."})
        .describe("one_way_collision", one_way)
        .describe("one_way_collision_margin", one_way_margin);

    registry.add<CollisionPolygon2D>()
        .set_doc("A polygon shape of the entity's physics body or area: any simple polygon, "
                 "filled or only its edges.")
        .describe("polygon", {.doc = "Points as [x, y] in pixels, from the entity."})
        .describe("build_mode", {.doc = "\"solids\" fills it, split into convex parts; "
                                        "\"segments\" makes only its edges solid.",
                                 .choices = {"solids", "segments"}})
        .describe("disabled", {.doc = "Leave the shape out."})
        .describe("one_way_collision", one_way)
        .describe("one_way_collision_margin", one_way_margin);

    registry.add<StaticBody2D>()
        .set_doc("A physics body that doesn't move on its own, such as a wall. Changing its "
                 "Transform2D moves it at once.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("friction", friction)
        .describe("bounce", bounce);

    registry.add<AnimatableBody2D>()
        .set_doc("A physics body that goes where its Transform2D says, such as a moving "
                 "platform, pushing bodies in its way.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("friction", friction)
        .describe("bounce", bounce)
        .describe("sync_to_physics", {.doc = "Move during the physics step, pushing and carrying "
                                             "bodies; false jumps there."});

    registry.add<RigidBody2D>()
        .set_doc("A physics body moved by gravity, collisions, forces and joints. The physics "
                 "step writes its Transform2D, velocities and sleeping.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("friction", friction)
        .describe("bounce", bounce)
        .describe("mass", {.doc = "In kilograms.", .min = 0.001})
        .describe("inertia", {.doc = "Resistance to turning; 0 computes it from the shapes and "
                                     "the mass.",
                              .min = 0})
        .describe("gravity_scale", {.doc = "Multiplies gravity; 0 floats."})
        .describe("linear_velocity", {.doc = "In pixels per second, as [x, y]."})
        .describe("angular_velocity", {.doc = "In radians per second, clockwise."})
        .describe("linear_damp_mode",
                  {.doc = "\"combine\" adds linear_damp to the project's; \"replace\" uses "
                          "only it.",
                   .choices = {"combine", "replace"}})
        .describe("linear_damp", {.doc = "Slows movement, per second.", .min = 0})
        .describe("angular_damp_mode",
                  {.doc = "\"combine\" adds angular_damp to the project's; \"replace\" uses "
                          "only it.",
                   .choices = {"combine", "replace"}})
        .describe("angular_damp", {.doc = "Slows turning, per second.", .min = 0})
        .describe("constant_force", {.doc = "A force applied every tick, as [x, y]."})
        .describe("constant_torque", {.doc = "A torque applied every tick."})
        .describe("lock_rotation", {.doc = "Never turn."})
        .describe("freeze", {.doc = "Stop simulating it; it acts as freeze_mode says."})
        .describe("freeze_mode", {.doc = "What a frozen body acts as: \"static\" or "
                                         "\"kinematic\" (moved by its Transform2D).",
                                  .choices = {"static", "kinematic"}})
        .describe("continuous_cd", {.doc = "Sweep it against other moving bodies so it can't "
                                           "pass through them when fast.",
                                    .choices = {"disabled", "cast_ray", "cast_shape"}})
        .describe("can_sleep", {.doc = "Stop simulating it while it rests."})
        .describe("sleeping", {.doc = "Resting and not simulated; set false to wake it."})
        .describe("contact_monitor", {.doc = "Emit body_entered and body_exited events as "
                                             "bodies start and stop touching it."});

    registry.add<CharacterBody2D>()
        .set_doc("A physics body moved by setting its velocity: each tick it slides along what "
                 "it hits, as Godot's move_and_slide, and reports floors, walls and ceilings.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("motion_mode", {.doc = "\"grounded\" has floors, walls and ceilings, for "
                                         "side views; \"floating\" has only walls, for top "
                                         "down.",
                                  .choices = {"grounded", "floating"}})
        .describe("up_direction", {.doc = "Which way is up, as a unit [x, y]."})
        .describe("velocity", {.doc = "In pixels per second, as [x, y]. Gameplay code sets it; "
                                      "each move slides it along what it hits."})
        .describe("max_slides", {.doc = "Slides per move at most.", .min = 1, .max = 16})
        .describe("floor_stop_on_slope", {.doc = "Don't slide down slopes while standing."})
        .describe("floor_max_angle", {.doc = "Steepest floor, in radians from up_direction.",
                                      .min = 0,
                                      .max = 3.14159})
        .describe("floor_snap_length", {.doc = "How far down it keeps to a floor while walking "
                                               "off steps and slopes, in pixels.",
                                        .min = 0})
        .describe("slide_on_ceiling", {.doc = "Slide along ceilings; false stops and falls."})
        .describe("platform_on_leave",
                  {.doc = "What it keeps of a moving floor's velocity on leaving it.",
                   .choices = {"add_velocity", "add_upward_velocity", "do_nothing"}})
        .describe("on_floor", {.doc = "Touched a floor in the last move."})
        .describe("on_wall", {.doc = "Touched a wall in the last move."})
        .describe("on_ceiling", {.doc = "Touched a ceiling in the last move."})
        .describe("floor_normal", {.doc = "The floor's normal in the last move."})
        .describe("wall_normal", {.doc = "The wall's normal in the last move."})
        .describe("platform_velocity", {.doc = "The velocity of the floor it stands on."});

    registry.add<Area2D>()
        .set_doc("A region that emits body_entered, body_exited, area_entered and area_exited "
                 "events. It goes where its Transform2D says.")
        .describe("collision_layer", layer)
        .describe("collision_mask", {.doc = "The layers of bodies and areas it detects, as "
                                            "bits."})
        .describe("monitoring", {.doc = "Report bodies and areas entering it."})
        .describe("monitorable", {.doc = "Other areas detect it."});

    registry.add<PinJoint2D>()
        .set_doc("Pins two bodies together at the entity's Transform2D, letting them turn.")
        .describe("node_a", node_a)
        .describe("node_b", node_b)
        .describe("disable_collision", disable_collision)
        .describe("angular_limit_enabled", {.doc = "Limit how far node_b turns from node_a."})
        .describe("angular_limit_lower", {.doc = "In radians, from -3.1 to 0.", .min = -3.1})
        .describe("angular_limit_upper", {.doc = "In radians, from 0 to 3.1.", .max = 3.1})
        .describe("motor_enabled", {.doc = "Turn node_b at motor_target_velocity."})
        .describe("motor_target_velocity", {.doc = "In radians per second."});

    registry.add<DampedSpringJoint2D>()
        .set_doc("A spring from node_a at the entity's Transform2D to node_b, length pixels "
                 "along the entity's y axis.")
        .describe("node_a", node_a)
        .describe("node_b", node_b)
        .describe("disable_collision", disable_collision)
        .describe("length", {.doc = "Where node_b is attached, in pixels along y.", .min = 0})
        .describe("rest_length", {.doc = "The length it pulls toward; 0 is length.", .min = 0})
        .describe("stiffness", {.doc = "How hard it pulls.", .min = 0})
        .describe("damping", {.doc = "How fast it settles.", .min = 0});

    registry.add<GrooveJoint2D>()
        .set_doc("Keeps node_b on a groove of node_a's, from the entity's Transform2D length "
                 "pixels along its y axis; node_b slides along it and turns freely.")
        .describe("node_a", node_a)
        .describe("node_b", node_b)
        .describe("disable_collision", disable_collision)
        .describe("length", {.doc = "The groove's length, in pixels.", .min = 0})
        .describe("initial_offset", {.doc = "Where node_b starts along the groove, in pixels.",
                                     .min = 0});
}

} // namespace

void register_builtin_components(ComponentRegistry& registry) {
    // Fields lights, sprites and tile map layers share.
    const FieldMeta light_mask{
        .doc = "Lit by lights whose range_item_cull_mask shares a bit.", .min = 0, .max = kMaxMask};
    const FieldMeta light_color{
        .doc = "The light's color, as [r, g, b, a] from 0 to 1.", .min = 0, .max = 1};
    const FieldMeta energy{.doc = "Multiplies the color.", .min = 0};
    const FieldMeta light_blend{.doc = "How the light changes what it lights: add brightens, sub "
                                       "darkens and mix blends toward its color.",
                                .choices = {"add", "sub", "mix"}};
    const FieldMeta z_min{
        .doc = "Lights sprites and layers with at least this z_index.", .min = -4096, .max = 4096};
    const FieldMeta z_max{
        .doc = "Lights sprites and layers with at most this z_index.", .min = -4096, .max = 4096};
    const FieldMeta cull_mask{.doc = "Lights sprites and layers whose light_mask shares a bit.",
                              .min = 0,
                              .max = kMaxMask};
    const FieldMeta shadow_enabled{.doc = "LightOccluder2D polygons cast shadows from it."};
    const FieldMeta shadow_color{.doc = "What shadowed pixels get instead of the light, as [r, g, "
                                        "b, a]; alpha is times the light's.",
                                 .min = 0,
                                 .max = 1};
    const FieldMeta shadow_filter{.doc = "Shadow edges: hard (none), or softened over 5 or 13 "
                                         "samples.",
                                  .choices = {"none", "pcf5", "pcf13"}};
    const FieldMeta shadow_smooth{.doc = "Spreads the filter's samples further apart.", .min = 0};
    const FieldMeta shadow_mask{
        .doc = "Occluders whose occluder_light_mask shares a bit cast shadows.",
        .min = 0,
        .max = kMaxMask};

    registry.add<Transform2D>()
        .set_doc("Position, rotation and scale in 2D. Units are pixels and y points down.")
        .describe("position", {.doc = "Position in pixels, as [x, y]."})
        .describe("rotation", {.doc = "Rotation in radians; positive turns clockwise."})
        .describe("scale", {.doc = "Scale factor on each axis, as [x, y]."});

    registry.add<Transform3D>()
        .set_doc("Position, rotation and scale in 3D. Units are meters and y points up.")
        .describe("position", {.doc = "Position in meters, as [x, y, z]."})
        .describe("rotation", {.doc = "Rotation as a unit quaternion, as [x, y, z, w]. "
                                      "[0, 0, 0, 1] is no rotation.",
                               .min = -1.0,
                               .max = 1.0})
        .describe("scale", {.doc = "Scale factor on each axis, as [x, y, z]."});

    registry.add<StoryRef>()
        .set_doc("The storyboard entries this entity stands for.")
        .describe("ids", {.doc = "Storyboard entry IDs, e.g. [\"character/mira\"]."});

    registry.add<Sprite2D>()
        .set_doc("A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, "
                 "then in the order their entities were made.")
        .describe("texture", {.doc = "Project path of a PNG or BMP file; \"\" draws nothing."})
        .describe("centered", {.doc = "Center the texture on the position; false puts its top "
                                      "left corner there."})
        .describe("offset", {.doc = "Moves the texture from the position, in pixels."})
        .describe("flip_h", {.doc = "Mirror the texture left to right."})
        .describe("flip_v", {.doc = "Mirror the texture top to bottom."})
        .describe("hframes",
                  {.doc = "Columns of frames in a sprite sheet.", .min = 1, .max = 16384})
        .describe("vframes", {.doc = "Rows of frames in a sprite sheet.", .min = 1, .max = 16384})
        .describe("frame",
                  {.doc = "The sheet's frame to draw, counted along rows from 0.", .min = 0})
        .describe("region", {.doc = "Part of the texture to use, as [x, y, width, height] in "
                                    "pixels, before splitting it into frames. A zero size uses "
                                    "the whole texture.",
                             .min = 0})
        .describe("modulate", {.doc = "Color the texture is multiplied by, as [r, g, b, a] from "
                                      "0 to 1.",
                               .min = 0,
                               .max = 1})
        .describe("z_index", {.doc = "Higher draws on top.", .min = -4096, .max = 4096})
        .describe("visible", {.doc = "Draw it."})
        .describe("texture_filter",
                  {.doc = "How the texture is sampled when scaled: \"nearest\" keeps pixel art "
                          "sharp. \"inherit\" uses the project's rendering.texture_filter.",
                   .choices = {"inherit", "linear", "nearest"}})
        .describe("material", {.doc = "Project path of a .material.json file, which colors the "
                                      "sprite's pixels; \"\" draws the texture as it is."})
        .describe("material_params",
                  {.doc = "Values for the material's params by name, replacing their defaults, "
                          "e.g. {\"flash\": 1}."})
        .describe("light_mask", light_mask)
        .describe("normal_texture", {.doc = "Project path of a normal map laid out as the "
                                            "texture, OpenGL style (green up); \"\" faces the "
                                            "viewer. Lights need a height to show it."})
        .describe("specular_texture", {.doc = "Project path of a specular map laid out as the "
                                              "texture: highlight color and, in alpha, "
                                              "shininess. Needs a normal_texture."})
        .describe("specular_color",
                  {.doc = "Multiplies the specular map's color.", .min = 0, .max = 1})
        .describe("specular_shininess",
                  {.doc = "Multiplies the specular map's shininess.", .min = 0, .max = 1});

    registry.add<AnimatedSprite2D>()
        .set_doc("Plays an animation from a .frames.json file on the entity's Sprite2D, which it "
                 "adds if missing. Emits animation_finished when an animation that doesn't loop "
                 "ends.")
        .describe("sprite_frames", {.doc = "Project path of a .frames.json file."})
        .describe("animation", {.doc = "Name of the animation to play."})
        .describe("playing", {.doc = "Advance the animation each tick."})
        .describe("speed_scale", {.doc = "Multiplies the animation's frames per second; "
                                         "negative plays backwards."})
        .describe("frame", {.doc = "The animation's current frame, from 0.", .min = 0})
        .describe("frame_progress",
                  {.doc = "How far through the current frame, from 0 to 1.", .min = 0, .max = 1});

    registry.add<AnimationPlayer>()
        .set_doc("Plays a keyframe animation from a .anim.json file on the entity's components. "
                 "Emits animation_finished when an animation that doesn't loop ends, and the "
                 "events of its event tracks.")
        .describe("library", {.doc = "Project path of a .anim.json file."})
        .describe("animation", {.doc = "Name of the animation to play."})
        .describe("playing", {.doc = "Advance the animation each tick."})
        .describe("speed_scale", {.doc = "Multiplies the playback speed; negative plays "
                                         "backwards."})
        .describe("time", {.doc = "Seconds into the animation, from 0 to its length.", .min = 0})
        .describe("backwards", {.doc = "Play toward the start, as Godot's play_backwards. "
                                       "Pingpong loops flip it at each end."});

    registry.add<TileMapLayer>()
        .set_doc("A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D "
                 "if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them "
                 "with tilemap.paint and tilemap.set.")
        .describe("tile_set", {.doc = "Project path of a .tileset.json file."})
        .describe("origin", {.doc = "The cell the first key of the first row stands for, as "
                                    "[column, row]."})
        .describe("legend", {.doc = "The tile each key stands for: a tile's name or "
                                    "\"source:column,row\". Keys are 1 to 3 characters, all "
                                    "the same length."})
        .describe("rows", {.doc = "Cells from the origin, one string of keys per row, \".\" for "
                                  "an empty cell."})
        .describe("z_index", {.doc = "Higher draws on top.", .min = -4096, .max = 4096})
        .describe("visible", {.doc = "Draw it."})
        .describe("modulate", {.doc = "Color every tile is multiplied by, as [r, g, b, a] from 0 "
                                      "to 1.",
                               .min = 0,
                               .max = 1})
        .describe("texture_filter",
                  {.doc = "How tiles are sampled when scaled: \"nearest\" keeps pixel art "
                          "sharp. \"inherit\" uses the project's rendering.texture_filter.",
                   .choices = {"inherit", "linear", "nearest"}})
        .describe("light_mask", light_mask);

    registry.add<PointLight2D>()
        .set_doc("A light shining from the entity's Transform2D through a texture. It lights "
                 "sprites and tile map layers whose light_mask shares a bit with "
                 "range_item_cull_mask; with shadow_enabled, LightOccluder2D polygons cast "
                 "shadows from it.")
        .describe("enabled", {.doc = "Shine."})
        .describe("texture", {.doc = "Project path of a PNG or BMP file whose color and alpha "
                                     "shape the light; \"\" is a soft round light 256 pixels "
                                     "across."})
        .describe("texture_scale", {.doc = "Scales the texture.", .min = 0})
        .describe("offset", {.doc = "Moves the texture from the light, in pixels."})
        .describe("color", light_color)
        .describe("energy", energy)
        .describe("blend_mode", light_blend)
        .describe("height", {.doc = "Pixels above the plane, for normal maps: at 0 it lights "
                                    "them only from the side."})
        .describe("range_z_min", z_min)
        .describe("range_z_max", z_max)
        .describe("range_item_cull_mask", cull_mask)
        .describe("shadow_enabled", shadow_enabled)
        .describe("shadow_color", shadow_color)
        .describe("shadow_filter", shadow_filter)
        .describe("shadow_filter_smooth", shadow_smooth)
        .describe("shadow_item_cull_mask", shadow_mask);

    registry.add<DirectionalLight2D>()
        .set_doc("Light from one direction over everything, such as the sun. It shines down the "
                 "screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows "
                 "work as PointLight2D's.")
        .describe("enabled", {.doc = "Shine."})
        .describe("color", light_color)
        .describe("energy", energy)
        .describe("blend_mode", light_blend)
        .describe("height", {.doc = "For normal maps: from 0, along the plane, to 1, straight "
                                    "down onto it.",
                             .min = 0,
                             .max = 1})
        .describe("max_distance",
                  {.doc = "How far shadows reach past their occluders, in pixels.", .min = 0})
        .describe("range_z_min", z_min)
        .describe("range_z_max", z_max)
        .describe("range_item_cull_mask", cull_mask)
        .describe("shadow_enabled", shadow_enabled)
        .describe("shadow_color", shadow_color)
        .describe("shadow_filter", shadow_filter)
        .describe("shadow_filter_smooth", shadow_smooth)
        .describe("shadow_item_cull_mask", shadow_mask);

    registry.add<LightOccluder2D>()
        .set_doc("A polygon that casts shadows from lights with shadow_enabled, placed by the "
                 "entity's Transform2D.")
        .describe("polygon", {.doc = "Points as [x, y] in pixels."})
        .describe("closed", {.doc = "Join the last point to the first."})
        .describe("cull_mode",
                  {.doc = "Leave out edges that run clockwise or counter_clockwise around the "
                          "light, as seen on screen. For a polygon wound clockwise, "
                          "counter_clockwise lets light into it, lighting its own sprite.",
                   .choices = {"disabled", "clockwise", "counter_clockwise"}})
        .describe("occluder_light_mask",
                  {.doc = "Casts shadows from lights whose shadow_item_cull_mask shares a bit.",
                   .min = 0,
                   .max = kMaxMask});

    registry.add<CanvasModulate>()
        .set_doc("Tints everything lit: the light a scene has where no light reaches. One per "
                 "scene.")
        .describe("color", {.doc = "As [r, g, b, a] from 0 to 1.", .min = 0, .max = 1});

    register_physics_components(registry);
}

} // namespace slash::world
