#include "slash/world/components.hpp"

#include "slash/world/component_registry.hpp"

#include <numbers>
#include <optional>
#include <string>

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
                 "and scaled by its Transform2D. On an entity with no body or area, a shape of "
                 "its parent's.")
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
        .set_doc("A polygon shape of the entity's physics body or area (its parent's, on an "
                 "entity with neither): any simple polygon, filled or only its edges.")
        .describe("polygon", {.doc = "Points as [x, y] in pixels, from the entity."})
        .describe("build_mode", {.doc = "\"solids\" fills it, split into convex parts; "
                                        "\"segments\" makes only its edges solid.",
                                 .choices = {"solids", "segments"}})
        .describe("disabled", {.doc = "Leave the shape out."})
        .describe("one_way_collision", one_way)
        .describe("one_way_collision_margin", one_way_margin);

    const FieldMeta constant_linear{.doc = "Carries what touches it as if it moved at this "
                                           "many pixels per second, as [x, y], as a conveyor "
                                           "belt."};
    const FieldMeta constant_angular{.doc = "Carries what touches it as if it turned at this "
                                            "many radians per second, clockwise."};

    registry.add<StaticBody2D>()
        .set_doc("A physics body that doesn't move on its own, such as a wall. Changing its "
                 "Transform2D moves it at once.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("friction", friction)
        .describe("bounce", bounce)
        .describe("constant_linear_velocity", constant_linear)
        .describe("constant_angular_velocity", constant_angular);

    registry.add<AnimatableBody2D>()
        .set_doc("A physics body that goes where its Transform2D says, such as a moving "
                 "platform, pushing bodies in its way.")
        .describe("collision_layer", layer)
        .describe("collision_mask", mask)
        .describe("friction", friction)
        .describe("bounce", bounce)
        .describe("constant_linear_velocity",
                  {.doc = "Added to how it moves, in pixels per second, as [x, y]."})
        .describe("constant_angular_velocity",
                  {.doc = "Added to how it turns, in radians per second, clockwise."})
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
        .describe("slide_on_ceiling", {.doc = "Slide along ceilings; false stops and falls."})
        .describe("wall_min_slide_angle", {.doc = "Floating: a wall hit more head-on than this, "
                                                  "in radians, stops it instead of sliding.",
                                           .min = 0,
                                           .max = std::numbers::pi})
        .describe("floor_stop_on_slope", {.doc = "Don't slide down slopes while standing."})
        .describe("floor_constant_speed", {.doc = "Walk up and down slopes at the speed it walks "
                                                  "on flat floors."})
        .describe("floor_block_on_wall", {.doc = "Walls too steep to be floors stop it instead "
                                                 "of letting it climb them."})
        .describe("floor_max_angle", {.doc = "Steepest floor, in radians from up_direction.",
                                      .min = 0,
                                      .max = std::numbers::pi})
        .describe("floor_snap_length", {.doc = "How far down it keeps to a floor while walking "
                                               "off steps and slopes, in pixels.",
                                        .min = 0})
        .describe("platform_on_leave",
                  {.doc = "What it keeps of a moving floor's velocity on leaving it.",
                   .choices = {"add_velocity", "add_upward_velocity", "do_nothing"}})
        .describe("platform_floor_layers", {.doc = "Floors in these layers carry it as they "
                                                   "move, as bits."})
        .describe("platform_wall_layers", {.doc = "Walls in these layers carry it as they move, "
                                                  "as bits."})
        .describe("safe_margin", {.doc = "What is this close, in pixels, counts as touching; at "
                                         "least Box2D's 0.32.",
                                  .min = 0.001})
        .describe("on_floor", {.doc = "Touched a floor in the last move."})
        .describe("on_wall", {.doc = "Touched a wall in the last move."})
        .describe("on_ceiling", {.doc = "Touched a ceiling in the last move."})
        .describe("floor_normal", {.doc = "The floor's normal in the last move."})
        .describe("wall_normal", {.doc = "The wall's normal in the last move."})
        .describe("platform_velocity", {.doc = "The velocity of the floor it stands on."});

    const FieldMeta override_mode{
        .doc = "How it changes the bodies in it, by priority: \"combine\" adds its own to "
               "higher areas' and lets lower areas and the project add theirs, \"replace\" uses "
               "only its own, \"combine_replace\" adds and stops lower ones, "
               "\"replace_combine\" drops higher ones' and lets lower ones add.",
        .choices = {"disabled", "combine", "combine_replace", "replace", "replace_combine"}};

    registry.add<Area2D>()
        .set_doc("A region that emits body_entered, body_exited, area_entered and area_exited "
                 "events, and can change the gravity and damping of rigid bodies in it. It goes "
                 "where its Transform2D says.")
        .describe("collision_layer", layer)
        .describe("collision_mask", {.doc = "The layers of bodies and areas it detects, as "
                                            "bits."})
        .describe("monitoring", {.doc = "Report bodies and areas entering it."})
        .describe("monitorable", {.doc = "Other areas detect it."})
        .describe("priority", {.doc = "Areas of higher priority override first."})
        .describe("gravity_space_override", override_mode)
        .describe("gravity_point", {.doc = "Pull toward gravity_point_center instead of along "
                                           "gravity_direction."})
        .describe("gravity_point_unit_distance",
                  {.doc = "Above 0, the pull is gravity this many pixels from the center and "
                          "falls off with distance squared; 0 pulls the same everywhere.",
                   .min = 0})
        .describe("gravity_point_center", {.doc = "Where a point pulls toward, in the area's "
                                                  "space."})
        .describe("gravity_direction", {.doc = "Which way gravity pulls, as a unit [x, y]."})
        .describe("gravity", {.doc = "How hard, in pixels per second squared."})
        .describe("linear_damp_space_override", override_mode)
        .describe("linear_damp", {.doc = "Slows movement in it, per second.", .min = 0})
        .describe("angular_damp_space_override", override_mode)
        .describe("angular_damp", {.doc = "Slows turning in it, per second.", .min = 0})
        .describe("audio_bus_override",
                  {.doc = "AudioStreamPlayer2D sounds in it play through audio_bus_name, when "
                          "its collision_layer is in their area_mask."})
        .describe("audio_bus_name", {.doc = "The bus sounds in it play through, from the "
                                            "project's bus layout."});

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
        .describe("initial_offset",
                  {.doc = "Where node_b starts along the groove, in pixels.", .min = 0});
}

} // namespace

// The particles component: Godot's CPUParticles2D.
// The UI components, as Godot's Control nodes.
void register_ui_components(ComponentRegistry& registry) {
    const FieldMeta anchor{.doc = "Where the edge is anchored in the parent's rect: 0 at its left "
                                  "or top, 1 at its right or bottom, and past them outside 0 to "
                                  "1, as Godot allows."};
    const FieldMeta offset{.doc = "Pixels from the edge's anchor to the edge."};
    const FieldMeta grow{.doc = "Which way it grows when its minimum size is bigger than its "
                                "anchors and offsets make it.",
                         .choices = {"begin", "end", "both"}};
    const FieldMeta size_flags{
        .doc = "How a container sizes it on this axis: fill takes the space it is given, "
               "expand also takes a share of the space left over, and shrink_* keep it at its "
               "minimum size at the start, center or end.",
        .choices = {"fill", "expand_fill", "expand", "shrink_begin", "shrink_center", "shrink_end",
                    "expand_shrink_center", "expand_shrink_end"}};
    const FieldMeta axis_stretch{.doc = "How the middle parts fill the rect: stretched, tiled, or "
                                        "tiled with tiles stretched to fit a whole number.",
                                 .choices = {"stretch", "tile", "tile_fit"}};
    const FieldMeta alignment{.doc = "Where the children go when they don't fill it.",
                              .choices = {"begin", "center", "end"}};
    const FieldMeta focus_neighbor{
        .doc = "The control the ui_left, ui_up, ui_right or ui_down action moves focus to from "
               "this one; null for the closest one that way."};
    registry.add<Control>()
        .set_doc("A UI element's rect, as Godot's Control: placed in its parent's rect by "
                 "anchors and offsets, or in the viewport when no Control lists it in children. "
                 "Label, ColorRect and the other UI components draw in it; they lay out with "
                 "these defaults when the entity has no Control. A container component places "
                 "its children by size flags instead of their anchors and offsets. Controls "
                 "draw above the world, or in the CanvasLayer that holds them, parents before "
                 "children.")
        .describe("children",
                  {.doc = "Controls laid out in its rect, drawn over it in this order."})
        .describe("anchor_left", anchor)
        .describe("anchor_top", anchor)
        .describe("anchor_right", anchor)
        .describe("anchor_bottom", anchor)
        .describe("offset_left", offset)
        .describe("offset_top", offset)
        .describe("offset_right", offset)
        .describe("offset_bottom", offset)
        .describe("grow_horizontal", grow)
        .describe("grow_vertical", grow)
        .describe("custom_minimum_size",
                  {.doc = "Its smallest size, [width, height]; it can be bigger to fit its "
                          "content.",
                   .min = 0})
        .describe("rotation", {.doc = "Radians, around pivot_offset; its children turn with it."})
        .describe("scale", {.doc = "Drawn scaled around pivot_offset; layout doesn't change."})
        .describe("pivot_offset",
                  {.doc = "The point it turns and scales around, from its top-left corner."})
        .describe("visible", {.doc = "Draw it and its children."})
        .describe("clip_contents",
                  {.doc = "Cut off its drawing and its children's at its rect; the pointer "
                          "reaches only the part of them inside it."})
        .describe("modulate", {.doc = "Tints it and its children, as [r, g, b, a].", .min = 0})
        .describe("self_modulate", {.doc = "Tints it only.", .min = 0})
        .describe("top_level",
                  {.doc = "Placed in the viewport instead of its parent's rect, left out by "
                          "containers, with none of its parent's transform, modulate, z_index "
                          "or clip, and drawn after its parent's tree. A hidden parent still "
                          "hides it."})
        .describe("z_index", {.doc = "Draws over controls with a lower one, added to its parent's.",
                              .min = -4096,
                              .max = 4096})
        .describe("z_as_relative",
                  {.doc = "Add z_index to its parent's; false makes it its own, as Godot's."})
        .describe("theme_override_colors",
                  {.doc = "Theme colors for this control by name, e.g. {\"font_color\": "
                          "[1, 0.8, 0.2, 1]}.",
                   .min = 0})
        .describe("theme_override_constants",
                  {.doc = "Theme constants by name, e.g. {\"outline_size\": 4}."})
        .describe("theme_override_fonts",
                  {.doc = "Theme fonts by name: a .ttf, .otf or .font.json file, e.g. "
                          "{\"font\": \"fonts/title.ttf\"}."})
        .describe(
            "theme_override_font_sizes",
            {.doc = "Theme font sizes by name, e.g. {\"font_size\": 32}.", .min = 1, .max = 1024})
        .describe("theme_override_icons",
                  {.doc = "Theme icons by name: a PNG, BMP or KTX2 file, a built-in icon such as "
                          "\"builtin:radio_checked\", or \"\" for none, e.g. "
                          "{\"checked\": \"ui/tick.png\"}."})
        .describe("theme_override_styles",
                  {.doc = "Theme style boxes by name, as in a .theme.json file, e.g. "
                          "{\"panel\": {\"bg_color\": [0.1, 0.1, 0.2, 1], "
                          "\"corner_radius\": 8}}."})
        .describe("theme",
                  {.doc = "A .theme.json file whose items it and its children use before the "
                          "project's theme and the default one."})
        .describe("theme_type_variation",
                  {.doc = "A theme type to look its items up in before its own, e.g. "
                          "\"TitleLabel\" with base_type Label."})
        .describe("size_flags_horizontal", size_flags)
        .describe("size_flags_vertical", size_flags)
        .describe(
            "size_flags_stretch_ratio",
            {.doc = "Its share of the space expanding children split in a container.", .min = 0})
        .describe("texture_filter",
                  {.doc = "How its textures are sampled when scaled; inherit takes its parent's, "
                          "then the project's.",
                   .choices = {"inherit", "linear", "nearest"}})
        .describe("mouse_filter",
                  {.doc = "Whether clicks and touches stop at it, go on to its parent too (pass) "
                          "or pass it by (ignore); \"\" takes its type's: ignore for Label and "
                          "NinePatchRect, pass for containers and TextureRect, stop for the rest.",
                   .choices = {"", "stop", "pass", "ignore"}})
        .describe("tooltip_text",
                  {.doc = "Shown in a box beside the pointer once it rests on the control for "
                          "the project's gui.tooltip_delay_sec, or on a child without one; \"\" "
                          "for none. Theme types TooltipPanel and TooltipLabel style it."})
        .describe("focus_mode",
                  {.doc = "Whether clicks and the keyboard or gamepad give it focus (all), only "
                          "clicks (click) or nothing (none); \"\" takes its type's: all for "
                          "buttons, none for the rest.",
                   .choices = {"", "none", "click", "all"}})
        .describe("layout_direction",
                  {.doc = "Which way it lays out; rtl mirrors its anchors, offsets, grow "
                          "directions, its children's order in containers and its text's "
                          "alignment. inherited takes its parent's, left to right at the root.",
                   .choices = {"inherited", "ltr", "rtl"}})
        .describe("focus_neighbor_left", focus_neighbor)
        .describe("focus_neighbor_top", focus_neighbor)
        .describe("focus_neighbor_right", focus_neighbor)
        .describe("focus_neighbor_bottom", focus_neighbor)
        .describe("focus_next",
                  {.doc = "The control ui_focus_next (Tab) moves focus to; null for the next in "
                          "the tree."})
        .describe("focus_previous",
                  {.doc = "The control ui_focus_prev (Shift+Tab) moves focus to; null for the "
                          "previous in the tree."});
    registry.add<DragAndDrop>()
        .set_doc("Lets a control be dragged and take drops, as a Godot script's _get_drag_data, "
                 "_can_drop_data and _drop_data. Moving the mouse past the project's "
                 "gui.drag_threshold with the left button held on it drags drag_data; letting "
                 "go over a control whose can_drop takes the data emits drop_data there (data "
                 "{\"data\", \"from\", \"position\"}). The control dragged emits drag_begin and "
                 "drag_end (data {\"successful\"}); a right click or ui_cancel cancels.")
        .describe("drag_data", {.doc = "What a drag from it carries, any JSON; null: it can't "
                                       "be dragged."})
        .describe("can_drop",
                  {.doc = "What it takes: data matching one of these, where an object needs "
                          "only the members given, e.g. [{\"kind\": \"item\"}]; anything else "
                          "must be equal. Empty takes nothing."})
        .describe("drag_preview", {.doc = "A control with no parent shown at the pointer while "
                                          "a drag from it goes on, even if hidden."});

    const FieldMeta text_direction{
        .doc = "The text's base direction: auto takes its first letter's (right to left for "
               "Arabic or Hebrew), inherited the control's layout_direction.",
        .choices = {"auto", "ltr", "rtl", "inherited"}};
    const FieldMeta language{.doc = "The text's language (BCP 47, such as \"ar\" or "
                                    "\"zh-Hant\"), which picks fallback fonts and the glyphs "
                                    "a font has for it; \"\" for none."};
    registry.add<Label>()
        .set_doc("Text in the control's rect, as Godot's Label. Theme items: font, font_size, "
                 "font_color, font_outline_color, outline_size, font_shadow_color, "
                 "shadow_offset_x, shadow_offset_y, shadow_outline_size, line_spacing and "
                 "paragraph_spacing.")
        .describe("text", {.doc = R"(Lines break at "\n".)"})
        .describe("horizontal_alignment",
                  {.doc = "Each line's place across the rect; fill stretches spaces to its "
                          "width, except on a paragraph's last line.",
                   .choices = {"left", "center", "right", "fill"}})
        .describe("vertical_alignment",
                  {.doc = "The lines' place down the rect; fill spreads them over it.",
                   .choices = {"top", "center", "bottom", "fill"}})
        .describe("autowrap_mode",
                  {.doc = "Wrap lines to the rect's width: between words, inside words too "
                          "(word_smart), or anywhere.",
                   .choices = {"off", "arbitrary", "word", "word_smart"}})
        .describe("clip_text", {.doc = "Cut off text outside the rect."})
        .describe("text_overrun_behavior",
                  {.doc = "Cut lines wider than the rect, at a character or word, with or "
                          "without the ellipsis.",
                   .choices = {"no_trimming", "trim_char", "trim_word", "trim_ellipsis",
                               "trim_word_ellipsis"}})
        .describe("ellipsis_char", {.doc = "What a trimmed line ends with."})
        .describe("uppercase", {.doc = "Draw the text in capitals."})
        .describe("lines_skipped", {.doc = "Lines left out from the top.", .min = 0})
        .describe("max_lines_visible", {.doc = "Lines shown at most; -1 for all.", .min = -1})
        .describe(
            "visible_characters",
            {.doc = "Characters shown from the start, for typing effects; -1 for all.", .min = -1})
        .describe("visible_characters_behavior",
                  {.doc = "Lay out only the visible characters, or all of them and hide the "
                          "rest so lines don't move as more show.",
                   .choices = {"chars_before_shaping", "chars_after_shaping"}})
        .describe("text_direction", text_direction)
        .describe("language", language);

    registry.add<ColorRect>()
        .set_doc("Fills the control's rect with one color, as Godot's ColorRect.")
        .describe("color", {.doc = "As [r, g, b, a] from 0 to 1.", .min = 0, .max = 1});

    registry.add<Panel>().set_doc(
        "Draws the theme's panel style box in the control's rect, as Godot's Panel.");

    registry.add<TextureRect>()
        .set_doc("Draws a texture in the control's rect, as Godot's TextureRect.")
        .describe("texture",
                  {.doc = "Project path of a PNG, BMP or KTX2 file; \"\" draws nothing."})
        .describe("expand_mode",
                  {.doc = "Its minimum size: the texture's size, none, or the rect's height as "
                          "its width (fit_width) or its width as its height (fit_height), "
                          "_proportional keeping the texture's aspect.",
                   .choices = {"keep_size", "ignore_size", "fit_width", "fit_width_proportional",
                               "fit_height", "fit_height_proportional"}})
        .describe("stretch_mode",
                  {.doc = "How the texture fills the rect: scaled to it, tiled, at its own size "
                          "(keep), or scaled keeping its aspect to fit inside (keep_aspect) or "
                          "to cover it (keep_aspect_covered).",
                   .choices = {"scale", "tile", "keep", "keep_centered", "keep_aspect",
                               "keep_aspect_centered", "keep_aspect_covered"}})
        .describe("flip_h", {.doc = "Draw it mirrored left to right."})
        .describe("flip_v", {.doc = "Draw it upside down."});

    const FieldMeta patch_margin{.doc = "Pixels of the texture on this side that don't stretch.",
                                 .min = 0};
    registry.add<NinePatchRect>()
        .set_doc("Draws a texture cut into nine parts in the control's rect, as Godot's "
                 "NinePatchRect: the corners keep their size, the sides stretch along and the "
                 "middle both ways. Its minimum size is its margins.")
        .describe("texture",
                  {.doc = "Project path of a PNG, BMP or KTX2 file; \"\" draws nothing."})
        .describe("region_rect",
                  {.doc = "The part of the texture to use, [x, y, width, height] in pixels; a "
                          "zero size uses the whole texture.",
                   .min = 0})
        .describe("patch_margin_left", patch_margin)
        .describe("patch_margin_top", patch_margin)
        .describe("patch_margin_right", patch_margin)
        .describe("patch_margin_bottom", patch_margin)
        .describe("axis_stretch_horizontal", axis_stretch)
        .describe("axis_stretch_vertical", axis_stretch)
        .describe("draw_center", {.doc = "Draw the middle part."});

    registry.add<ProgressBar>()
        .set_doc("A bar filled to show value between min_value and max_value, with the percentage "
                 "on it, as Godot's ProgressBar. Theme items: background, fill, font, font_size, "
                 "font_color, font_outline_color and outline_size.")
        .describe("min_value", {.doc = "The value of an empty bar."})
        .describe("max_value", {.doc = "The value of a full bar."})
        .describe("value", {.doc = "Kept between min_value and max_value unless allow_greater or "
                                   "allow_lesser."})
        .describe("step",
                  {.doc = "The value is rounded to a multiple of it; 0 for none.", .min = 0})
        .describe("allow_greater", {.doc = "Let value go above max_value."})
        .describe("allow_lesser", {.doc = "Let value go below min_value."})
        .describe("fill_mode",
                  {.doc = "Which way the bar fills.",
                   .choices = {"begin_to_end", "end_to_begin", "top_to_bottom", "bottom_to_top"}})
        .describe("show_percentage", {.doc = "Draw the percentage on the bar."});

    registry.add<Slider>()
        .set_doc("A grabber dragged along a track to pick a value, as Godot's HSlider, or "
                 "VSlider when vertical: a click on the track moves the grabber there, and the "
                 "mouse wheel, ui_left and ui_right (ui_up and ui_down when vertical), ui_home "
                 "and ui_end step it while it has focus. Emits value_changed {\"value\"}, "
                 "drag_started and drag_ended {\"value_changed\"}. Theme items: slider, "
                 "grabber_area and grabber_area_highlight styles, the icons grabber, "
                 "grabber_highlight, grabber_disabled and tick, center_grabber, grabber_offset, "
                 "grabber_max_size, tick_offset and tick_max_size.")
        .describe("min_value", {.doc = "The value at the start: the left, or the bottom."})
        .describe("max_value", {.doc = "The value at the end; at least min_value."})
        .describe("step", {.doc = "The value is rounded to min_value plus a multiple of it; 0 for "
                                  "none.",
                           .min = 0})
        .describe("page", {.doc = "The value stays this far below max_value.", .min = 0})
        .describe("value", {.doc = "Kept between min_value and max_value less page unless "
                                   "allow_greater or allow_lesser."})
        .describe("rounded", {.doc = "Round the value to a whole number."})
        .describe("allow_greater", {.doc = "Let value go above max_value."})
        .describe("allow_lesser", {.doc = "Let value go below min_value."})
        .describe("editable", {.doc = "The pointer and keys move it; off, it draws dimmed."})
        .describe("scrollable", {.doc = "The mouse wheel moves it."})
        .describe("tick_count",
                  {.doc = "Marks drawn at even steps along the track; 0 or 1 for none.",
                   .min = 0,
                   .max = 4096})
        .describe("ticks_on_borders", {.doc = "Draw the first and last marks, at the ends."})
        .describe("ticks_position",
                  {.doc = "Which side of the track the marks go: bottom_right is below a "
                          "horizontal slider and right of a vertical one.",
                   .choices = {"bottom_right", "top_left", "both", "center"}})
        .describe("vertical", {.doc = "Run from bottom to top, as VSlider."});

    registry.add<ScrollBar>()
        .set_doc("A bar with a grabber that shows how far along a value is and moves it, as "
                 "Godot's HScrollBar, or VScrollBar when vertical: the grabber's length is "
                 "page's share of the range. A click before or after the grabber moves the "
                 "value a page, a drag moves the grabber, the wheel moves it an eighth of a "
                 "page, and with focus ui_left and ui_right (ui_up and ui_down when vertical), "
                 "ui_home and ui_end step it. Emits value_changed {\"value\"} and scrolling. "
                 "Theme items: scroll, scroll_focus, grabber, grabber_highlight and "
                 "grabber_pressed styles, the icons increment and decrement (and their "
                 "_highlight and _pressed ones), icon_max_size, and padding constants.")
        .describe("min_value", {.doc = "The value at the start: the left, or the top."})
        .describe("max_value", {.doc = "The value at the end; at least min_value."})
        .describe("step", {.doc = "The value is rounded to min_value plus a multiple of it; 0 for "
                                  "none.",
                           .min = 0})
        .describe("page", {.doc = "The part of the range in view, which the grabber's length "
                                  "shows; the value stays this far below max_value.",
                           .min = 0})
        .describe("value", {.doc = "Kept between min_value and max_value less page unless "
                                   "allow_greater or allow_lesser."})
        .describe("rounded", {.doc = "Round the value to a whole number."})
        .describe("allow_greater", {.doc = "Let value go above max_value."})
        .describe("allow_lesser", {.doc = "Let value go below min_value."})
        .describe("custom_step",
                  {.doc = "What the ui_* actions and its arrows step the value by; -1 for step.",
                   .min = -1})
        .describe("vertical", {.doc = "Run from top to bottom, as VScrollBar."});

    const FieldMeta button_mask{.doc = "The mouse buttons that press it.",
                                .choices = {"left", "right", "middle"}};
    const FieldMeta shortcut{.doc =
                                 "What else presses it: bindings as in project.json's input, e.g. "
                                 "{\"type\": \"key\", \"key\": \"Escape\"}, or actions, e.g. "
                                 "{\"type\": \"action\", \"action\": \"ui_cancel\"}."};
    const FieldMeta action_mode{
        .doc = "Press it when the click or tap starts, or when it ends over the button.",
        .choices = {"press", "release"}};
    const FieldMeta button_group{.doc = "Toggle buttons with the same group name act as one "
                                        "choice: pressing one releases the others."};
    const auto describe_base = [&](ComponentType& type) -> ComponentType& {
        return type.describe("disabled", {.doc = "Can't be pressed, and draws dimmed."})
            .describe("toggle_mode",
                      {.doc = "Each press turns button_pressed on or off, as a switch."})
            .describe("button_pressed", {.doc = "Toggled on, for a toggle button."})
            .describe("action_mode", action_mode)
            .describe("button_mask", button_mask)
            .describe("keep_pressed_outside",
                      {.doc = "Draw it pressed while the pointer that pressed it is outside."})
            .describe("button_group", button_group)
            .describe("shortcut", shortcut)
            .describe("shortcut_feedback",
                      {.doc = "Draw it pressed for a moment when its shortcut presses it."});
    };
    auto& button = registry.add<Button>().set_doc(
        "A button with text and an icon, as Godot's Button: a click or tap released over it, "
        "ui_accept while it has focus, or its shortcut presses it, emitting pressed (and "
        "toggled, data {\"toggled_on\": bool}, for toggle buttons), button_down and button_up "
        "events. Theme items: normal, hover, pressed, hover_pressed, disabled and focus styles, "
        "font_color, font_hover_color, font_pressed_color, font_hover_pressed_color, "
        "font_focus_color, font_disabled_color, icon_*_color for the same states, font, "
        "font_size, outline_size, font_outline_color, h_separation, icon_max_width, "
        "align_to_largest_stylebox, line_spacing and click_margin.");
    // A Button's fields but its text and icon, which an OptionButton takes from its options.
    const auto describe_button_look = [&](ComponentType& type) -> ComponentType& {
        return describe_base(type)
            .describe("flat", {.doc = "Draw no style box, only the text and icon."})
            .describe("alignment", {.doc = "The text's place across the button.",
                                    .choices = {"left", "center", "right"}})
            .describe("text_overrun_behavior",
                      {.doc = "Cut text wider than the button, at a character or word, with or "
                              "without an ellipsis.",
                       .choices = {"no_trimming", "trim_char", "trim_word", "trim_ellipsis",
                                   "trim_word_ellipsis"}})
            .describe("autowrap_mode", {.doc = "Wrap the text to the button's width.",
                                        .choices = {"off", "arbitrary", "word", "word_smart"}})
            .describe("clip_text",
                      {.doc = "Let the button be narrower than its text, cutting the text off."})
            .describe("icon_alignment",
                      {.doc = "The icon's place across the button; center puts it behind the text.",
                       .choices = {"left", "center", "right"}})
            .describe("vertical_icon_alignment",
                      {.doc = "The icon's place down the button; top and bottom put the text above "
                              "or below it.",
                       .choices = {"top", "center", "bottom"}})
            .describe("expand_icon",
                      {.doc = "Scale the icon to fit the button, keeping its aspect."})
            .describe("text_direction", text_direction)
            .describe("language", language);
    };
    const auto describe_button = [&](ComponentType& type) -> ComponentType& {
        return describe_button_look(type)
            .describe("text", {.doc = R"(Lines break at "\n".)"})
            .describe("icon",
                      {.doc = "Project path of a PNG, BMP or KTX2 file drawn beside the text; "
                              "\"\" for none."});
    };
    describe_button(button);
    describe_button(registry.add<CheckBox>().set_doc(
        "A toggle button with a box beside its text showing whether it is on, as Godot's "
        "CheckBox; in a button_group the box is round, as a radio button's. Pressed as Button "
        "is, with the same events. Theme items: Button's, plus the icons checked, unchecked, "
        "radio_checked, radio_unchecked and each one's _disabled, checkbox_checked_color, "
        "checkbox_unchecked_color and check_v_offset."));
    describe_button(registry.add<CheckButton>().set_doc(
        "A toggle button with a switch at its end showing whether it is on, as Godot's "
        "CheckButton. Pressed as Button is, with the same events. Theme items: Button's, plus "
        "the icons checked, unchecked, checked_disabled and unchecked_disabled and each one's "
        "_mirrored (drawn right to left), button_checked_color, button_unchecked_color and "
        "check_v_offset."));

    const FieldMeta items{.doc = "The items, in order: {\"text\", \"icon\", \"checkable\": "
                                 "\"none\", \"check\" or \"radio\", \"checked\", \"id\" (-1 for "
                                 "its index), \"disabled\", \"separator\", \"indent\", "
                                 "\"tooltip\"}."};
    registry.add<PopupMenu>()
        .set_doc("A list of items shown over everything at a point of the viewport while "
                 "visible, as Godot's PopupMenu: picked with a click, the arrow keys and "
                 "ui_accept, or by typing an item's first letters. Picking an item emits "
                 "id_pressed and index_pressed (data {\"id\", \"index\"}) and closes it; so do "
                 "a click outside and ui_cancel. Opening and closing emit about_to_popup and "
                 "popup_hide, and moving to an item with the keys id_focused. Theme items "
                 "(type PopupMenu): panel, hover, separator, labeled_separator_left and "
                 "labeled_separator_right styles, the check box and radio icons, font, "
                 "font_size, font_separator, font_separator_size, font_color, "
                 "font_hover_color, font_disabled_color, font_separator_color, the outline "
                 "colors and sizes, v_separation, h_separation, indent, item_start_padding, "
                 "item_end_padding and icon_max_width.")
        .describe("items", items)
        .describe("position", {.doc = "Its top left corner in the viewport, in pixels; it is "
                                      "moved to fit inside the viewport."})
        .describe("visible", {.doc = "Shown. Set it to open the menu; closing it clears it."})
        .describe("hide_on_item_selection",
                  {.doc = "Close when an item that isn't checkable is picked."})
        .describe("hide_on_checkable_item_selection",
                  {.doc = "Close when a checkable item is picked."})
        .describe("allow_search", {.doc = "Typing the start of an item's text moves to the item."})
        .describe("theme", {.doc = "Project path of a .theme.json file to take theme items "
                                   "from before the project's; \"\" for none."})
        .describe("layout_direction", {.doc = "Which way it lays out; inherited is left to right.",
                                       .choices = {"inherited", "ltr", "rtl"}});
    describe_button_look(registry.add<OptionButton>().set_doc(
                             "A button showing the option picked, which opens a list of its "
                             "options below it when pressed, as Godot's OptionButton. Picking "
                             "one sets selected and emits item_selected (data {\"index\"}); "
                             "moving to one with the keys emits item_focused. Theme items: "
                             "Button's, plus the arrow icon, arrow_margin and modulate_arrow, "
                             "and PopupMenu's for the list."))
        .describe("items", {.doc = "The options, in order: {\"text\", \"icon\", \"id\" (-1 "
                                   "for its index), \"disabled\", \"separator\", "
                                   "\"tooltip\"}."})
        .describe("selected", {.doc = "The option shown, by index; -1 for none.", .min = -1})
        .describe("fit_to_longest_item",
                  {.doc = "Be as wide as the widest option, not only the one shown."})
        .describe("allow_reselect",
                  {.doc = "Picking the option already shown emits item_selected again."});
    describe_button(registry.add<MenuButton>().set_doc(
                        "A flat button that opens a menu of items below it when pressed, as "
                        "Godot's MenuButton. Picking an item emits id_pressed and index_pressed "
                        "(data {\"id\", \"index\"}) from the button; it also emits "
                        "about_to_popup and popup_hide as the menu opens and closes, and "
                        "id_focused when the keys move to an item. Theme items: Button's, and "
                        "PopupMenu's for the menu."))
        .describe("items", items)
        .describe("switch_on_hover",
                  {.doc = "While another MenuButton's menu is open, moving onto this one opens "
                          "its menu instead, as in a menu bar."});

    const FieldMeta button_texture{.doc =
                                       "Project path of a PNG, BMP or KTX2 file; \"\" for none."};
    auto& texture_button = registry.add<TextureButton>().set_doc(
        "A button drawn with a texture per state, as Godot's TextureButton: texture_normal, "
        "then texture_pressed, texture_hover and texture_disabled when they are set, and "
        "texture_focused over them while it has focus. Pressed as Button is, with the same "
        "events. Its minimum size is its texture's.");
    describe_base(texture_button)
        .describe("texture_normal", button_texture)
        .describe("texture_pressed", button_texture)
        .describe("texture_hover", button_texture)
        .describe("texture_disabled", button_texture)
        .describe("texture_focused", button_texture)
        .describe("ignore_texture_size", {.doc = "Need no space for the texture."})
        .describe("stretch_mode",
                  {.doc = "How the texture fills the button: scaled to it, tiled, at its own "
                          "size (keep), or scaled keeping its aspect to fit inside "
                          "(keep_aspect) or to cover it (keep_aspect_covered).",
                   .choices = {"scale", "tile", "keep", "keep_centered", "keep_aspect",
                               "keep_aspect_centered", "keep_aspect_covered"}})
        .describe("flip_h", {.doc = "Draw it mirrored left to right."})
        .describe("flip_v", {.doc = "Draw it upside down."});

    registry.add<LineEdit>()
        .set_doc("A one-line text field, as Godot's LineEdit: a click, focus or ui_text_submit "
                 "starts editing; typing, an input method and the on-screen keyboard insert at "
                 "the caret; ui_text_* actions move the caret, select and delete; ui_cut, "
                 "ui_copy, ui_paste, ui_undo and ui_redo work the clipboard and undo. Emits "
                 "text_changed {\"text\"}, text_submitted {\"text\"} (ui_text_submit), "
                 "text_change_rejected {\"rejected_substring\"} and editing_toggled "
                 "{\"toggled_on\"}. Theme items: normal, read_only and focus styles, font, "
                 "font_size, font_color, font_selected_color, font_uneditable_color, "
                 "font_placeholder_color, font_outline_color, outline_size, caret_color, "
                 "caret_width, selection_color, clear_button_color, clear_button_color_pressed "
                 "and minimum_character_width.")
        .describe("text", {.doc = "One line; line breaks show as spaces."})
        .describe("placeholder_text", {.doc = "Shown, dimmed, while the text is empty."})
        .describe("alignment", {.doc = "The text's place across the field; fill acts as left.",
                                .choices = {"left", "center", "right", "fill"}})
        .describe("max_length",
                  {.doc = "Most characters it takes; 0 for no limit. Typing or pasting more "
                          "emits text_change_rejected with what didn't fit.",
                   .min = 0})
        .describe("editable", {.doc = "Typing changes the text; off, it can still be selected "
                                      "and copied."})
        .describe("keep_editing_on_text_submit",
                  {.doc = "Keep editing after ui_text_submit (Enter) instead of stopping."})
        .describe("expand_to_text_length", {.doc = "Its minimum width grows to fit the text."})
        .describe("backspace_deletes_composite_character_enabled",
                  {.doc = "Backspace deletes a whole character with its accents or an emoji "
                          "sequence, not one code point."})
        .describe("virtual_keyboard_enabled",
                  {.doc = "Show the on-screen keyboard while editing, on phones and tablets."})
        .describe("virtual_keyboard_show_on_focus",
                  {.doc = "Show it as soon as the field takes focus, not only when tapped."})
        .describe("virtual_keyboard_type",
                  {.doc = "The kind of on-screen keyboard.",
                   .choices = {"default", "multiline", "number", "number_decimal", "phone",
                               "email_address", "password", "url"}})
        .describe("clear_button_enabled", {.doc = "Show a button at the end that clears the text."})
        .describe("shortcut_keys_enabled",
                  {.doc = "Cut, copy, paste, select all, undo and redo by keyboard shortcut."})
        .describe("selecting_enabled", {.doc = "Text can be selected."})
        .describe("deselect_on_focus_loss_enabled", {.doc = "Losing focus drops the selection."})
        .describe("drag_and_drop_selection_enabled",
                  {.doc = "The selected text can be dragged into another text field, or moved "
                          "within this one."})
        .describe("right_icon", {.doc = "Project path of a PNG, BMP or KTX2 file drawn at the end; "
                                        "\"\" for none."})
        .describe("icon_expand_mode",
                  {.doc = "The right icon's size: its own, the font's height, or the field's "
                          "height times right_icon_scale.",
                   .choices = {"original_size", "fit_to_text", "fit_to_line_edit"}})
        .describe("right_icon_scale", {.doc = "For fit_to_line_edit.", .min = 0.0})
        .describe("flat", {.doc = "Draw no style box."})
        .describe("select_all_on_focus", {.doc = "Select all the text when it takes focus."})
        .describe("caret_blink", {.doc = "The caret blinks."})
        .describe("caret_blink_interval",
                  {.doc = "Seconds the caret is shown, then hidden, when it blinks.", .min = 0.001})
        .describe("caret_column", {.doc = "Characters before the caret.", .min = 0})
        .describe("caret_force_displayed", {.doc = "Draw the caret even when not editing."})
        .describe("caret_mid_grapheme",
                  {.doc = "Let the caret stop between a letter and its accents, or inside an "
                          "emoji sequence."})
        .describe("secret", {.doc = "Show each character as secret_character, for passwords; "
                                    "copying and cutting are off."})
        .describe("secret_character", {.doc = "One character."})
        .describe("text_direction", text_direction)
        .describe("language", language);

    registry.add<TextEdit>()
        .set_doc("A text field of several lines, as Godot's TextEdit: a click or focus puts the "
                 "caret in it; typing, an input method and the on-screen keyboard insert at the "
                 "caret; ui_text_newline breaks the line; ui_text_* actions move the caret, "
                 "select, delete and scroll; ui_cut, ui_copy, ui_paste, ui_undo and ui_redo "
                 "work the clipboard and undo; its scroll bars page, drag and take the wheel. "
                 "A CodeHighlighter on the entity colors its text and a CodeEdit makes it a code "
                 "editor. Emits text_changed, text_set, lines_edited_from {\"from_line\", "
                 "\"to_line\"}, caret_changed and, for a click on a gutter, gutter_clicked "
                 "{\"line\", \"gutter\"} with the gutter's name. Theme "
                 "items: normal, read_only and focus styles, font, font_size, font_color, "
                 "font_readonly_color, font_selected_color, font_placeholder_color, "
                 "font_outline_color, outline_size, caret_color, caret_background_color, "
                 "caret_width, selection_color, current_line_color, word_highlighted_color, "
                 "search_result_color, search_result_border_color, background_color and "
                 "line_spacing, and the scroll bars' ScrollBar items.")
        .describe("text", {.doc = R"(Lines break at "\n".)"})
        .describe("placeholder_text", {.doc = "Shown, dimmed, while the text is empty."})
        .describe("editable", {.doc = "Typing changes the text; off, it can still be selected "
                                      "and copied."})
        .describe("backspace_deletes_composite_character_enabled",
                  {.doc = "Backspace deletes a whole character with its accents or an emoji "
                          "sequence, not one code point."})
        .describe("shortcut_keys_enabled",
                  {.doc = "Cut, copy, paste, select all, undo and redo by keyboard shortcut."})
        .describe("selecting_enabled", {.doc = "Text can be selected."})
        .describe("deselect_on_focus_loss_enabled", {.doc = "Losing focus drops the selection."})
        .describe("drag_and_drop_selection_enabled",
                  {.doc = "The selected text can be dragged into another text field, or moved "
                          "within this one."})
        .describe("empty_selection_clipboard_enabled",
                  {.doc = "Cut and copy with nothing selected take the caret's line."})
        .describe("wrap_mode", {.doc = "Wrap lines wider than the field (boundary).",
                                .choices = {"none", "boundary"}})
        .describe("autowrap_mode",
                  {.doc = "Where wrapped lines break: between words, inside words too "
                          "(word_smart), or anywhere.",
                   .choices = {"arbitrary", "word", "word_smart"}})
        .describe("tab_input_mode",
                  {.doc = "Tab types a tab; off, Tab moves focus. ui_focus_mode (Ctrl+M) "
                          "switches it."})
        .describe("virtual_keyboard_enabled",
                  {.doc = "Show the on-screen keyboard while it has focus, on phones and "
                          "tablets."})
        .describe("virtual_keyboard_show_on_focus",
                  {.doc = "Show it as soon as the field takes focus, not only when tapped."})
        .describe("scroll_past_end_of_file",
                  {.doc = "Let the last line scroll up to the top of the field."})
        .describe("scroll_vertical",
                  {.doc = "Lines scrolled down, wrapped rows counted.", .min = 0.0})
        .describe("scroll_horizontal", {.doc = "Pixels scrolled right.", .min = 0})
        .describe("scroll_fit_content_height",
                  {.doc = "Its minimum height grows to fit every line."})
        .describe("scroll_fit_content_width",
                  {.doc = "Its minimum width grows to fit the widest line."})
        .describe("caret_type", {.doc = "A line between characters, or a block over one.",
                                 .choices = {"line", "block"}})
        .describe("caret_blink", {.doc = "The caret blinks."})
        .describe("caret_blink_interval",
                  {.doc = "Seconds the caret is shown, then hidden, when it blinks.", .min = 0.001})
        .describe("caret_draw_when_editable_disabled", {.doc = "Draw the caret when not editable."})
        .describe("caret_move_on_right_click",
                  {.doc = "A right click outside the selection moves the caret."})
        .describe("caret_mid_grapheme",
                  {.doc = "Let the caret stop between a letter and its accents, or inside an "
                          "emoji sequence."})
        .describe("caret_line", {.doc = "The caret's line, from 0.", .min = 0})
        .describe("caret_column", {.doc = "Characters before the caret in its line.", .min = 0})
        .describe("highlight_current_line",
                  {.doc = "Draw current_line_color behind the caret's line."})
        .describe("draw_tabs", {.doc = "Draw an arrow for each tab."})
        .describe("draw_spaces", {.doc = "Draw a dot for each space."})
        .describe("highlight_all_occurrences",
                  {.doc = "Mark the other copies of the selected text in view, whole words of "
                          "the same case, in word_highlighted_color."})
        .describe("search_text",
                  {.doc = "Marked wherever it occurs in view, in search_result_color with a "
                          "search_result_border_color border. ui.search finds it."})
        .describe("search_flags",
                  {.doc = "How search_text matches.", .choices = {"match_case", "whole_words"}})
        .describe("text_direction", text_direction)
        .describe("language", language)
        .describe("gutters",
                  {.doc = "Columns left of the text, after a CodeEdit's: {\"name\", \"type\" "
                          "(\"string\" or \"icon\"), \"width\" (24), \"draw\", \"lines\": "
                          "[{\"line\", \"text\", \"icon\", \"color\"}]}. Entries move with "
                          "their lines as the text is typed in, and go with a deleted line."});

    registry.add<CodeHighlighter>()
        .set_doc("Colors the text of the TextEdit on its entity as code, as Godot's "
                 "CodeHighlighter: keywords, then members not after a '.', function names before "
                 "'(', member variables after '.', symbols and numbers (hex, floats, exponents) "
                 "in their colors, the rest in the TextEdit's font_color. A color region colors "
                 "from its start key to its end key, over several lines, or to the line's end; a "
                 "'\\' escapes the next character in a region that starts with a quote.")
        .describe("number_color", {.doc = "Numbers."})
        .describe("symbol_color", {.doc = "Punctuation and operators, and escapes in strings."})
        .describe("function_color", {.doc = "A word before '('."})
        .describe("member_variable_color", {.doc = "A word after '.'."})
        .describe("keyword_colors", {.doc = "Words and their colors, e.g. {\"func\": [1, 0.4, 0.4, "
                                            "1]}; opaque black colors nothing."})
        .describe("member_keyword_colors",
                  {.doc = "Words colored as keywords unless they come after a '.'."})
        .describe("color_regions",
                  {.doc = "Regions by \"start end\" keys of symbols, e.g. {\"\\\" \\\"\": "
                          "string color, \"/* */\": comment color}; a key with no end, e.g. "
                          "\"#\", runs to the line's end. Longer start keys are tried first."});

    registry.add<CodeEdit>()
        .set_doc("Makes the TextEdit on its entity a code editor, as Godot's CodeEdit: line "
                 "numbers, and a gutter left of them for breakpoints, bookmarks and executing "
                 "lines, which move with their lines as the text is typed in. A click on that "
                 "gutter toggles a breakpoint (with Shift, a bookmark) and emits "
                 "breakpoint_toggled {\"line\"}, as an edit moving or deleting one does; a "
                 "click on a line number selects the line. Theme items (type CodeEdit, then "
                 "TextEdit's): breakpoint, bookmark and executing_line icons, breakpoint_color, "
                 "bookmark_color, executing_line_color, line_number_color and "
                 "line_length_guideline_color.")
        .describe("gutters_draw_breakpoints_gutter",
                  {.doc = "Draw breakpoints, and toggle them with a click."})
        .describe("gutters_draw_bookmarks",
                  {.doc = "Draw bookmarks, and toggle them with Shift and a click."})
        .describe("gutters_draw_executing_lines", {.doc = "Draw an arrow on executing lines."})
        .describe("gutters_draw_line_numbers", {.doc = "Number the lines, from 1."})
        .describe("gutters_zero_pad_line_numbers",
                  {.doc = "Pad line numbers with zeros instead of spaces."})
        .describe(
            "gutters_line_numbers_min_digits",
            {.doc = "Digits the line numbers' gutter has room for at least.", .min = 1, .max = 5})
        .describe("line_length_guidelines",
                  {.doc = "Columns to draw a line at, the first brighter than the rest.", .min = 0})
        .describe("breakpointed_lines", {.doc = "Lines with a breakpoint, from 0.", .min = 0})
        .describe("bookmarked_lines", {.doc = "Lines with a bookmark, from 0.", .min = 0})
        .describe("executing_lines", {.doc = "Lines marked as executing, from 0.", .min = 0});

    registry.add<BoxContainer>()
        .set_doc("Lays out its children in a row, or a column when vertical, as Godot's "
                 "HBoxContainer and VBoxContainer: each at its minimum size along it, plus a share "
                 "of the space left for those that expand. Theme constant: separation.")
        .describe("vertical", {.doc = "A column instead of a row."})
        .describe("alignment", alignment);

    registry.add<GridContainer>()
        .set_doc("Lays out its children in a grid, as Godot's GridContainer: left to right, a new "
                 "row every columns children. Theme constants: h_separation and v_separation.")
        .describe("columns", {.doc = "Children per row.", .min = 1, .max = 1024});

    registry.add<MarginContainer>().set_doc(
        "Fits its children in its rect less margins, as Godot's MarginContainer. Theme "
        "constants: margin_left, margin_top, margin_right and margin_bottom.");

    registry.add<CenterContainer>()
        .set_doc("Keeps its children at their minimum sizes in its center, as Godot's "
                 "CenterContainer.")
        .describe("use_top_left",
                  {.doc = "Center them on its top-left corner, and need no space for them."});

    registry.add<FlowContainer>()
        .set_doc("Lays out its children in rows that wrap to its width, or columns that wrap to "
                 "its height when vertical, as Godot's HFlowContainer and VFlowContainer. Theme "
                 "constants: h_separation and v_separation.")
        .describe("vertical", {.doc = "Columns instead of rows."})
        .describe("alignment", alignment)
        .describe("last_wrap_alignment",
                  {.doc = "Where the last line goes when it isn't full; inherit uses alignment.",
                   .choices = {"inherit", "begin", "center", "end"}})
        .describe("reverse_fill",
                  {.doc = "Fill rows from the bottom up, or columns from the right."});

    registry.add<PanelContainer>().set_doc(
        "Draws the theme's panel style box and fits its children inside the style's margins, "
        "as Godot's PanelContainer.");

    const FieldMeta scroll_mode{
        .doc = "disabled: no scrolling, it is as big as its children that way; auto: a scroll bar "
               "when they don't fit; always; never: it scrolls with no bar; reserve: as auto, "
               "with room kept for the bar.",
        .choices = {"disabled", "auto", "always", "never", "reserve"}};
    const FieldMeta custom_step{.doc = "What its bar's arrows step by; -1 for none.", .min = -1};
    registry.add<ScrollContainer>()
        .set_doc("Shows its children through its rect, scrolled, with scroll bars when they "
                 "don't fit, as Godot's ScrollContainer. Each child takes its minimum size, or "
                 "the whole visible width or height on an axis its size flags expand. The "
                 "wheel, the scroll bars and touch drags scroll it, and it always clips its "
                 "children. Emits scroll_started and scroll_ended around a touch drag. Theme "
                 "items: panel and focus styles, scrollbar_margin_left, _top, _right and "
                 "_bottom (-1 for the panel's margins), scrollbar_h_separation and "
                 "scrollbar_v_separation; its bars use HScrollBar's and VScrollBar's.")
        .describe("horizontal_scroll_mode", scroll_mode)
        .describe("vertical_scroll_mode", scroll_mode)
        .describe("scroll_horizontal", {.doc = "Pixels scrolled right; kept in range."})
        .describe("scroll_vertical", {.doc = "Pixels scrolled down; kept in range."})
        .describe("scroll_horizontal_custom_step", custom_step)
        .describe("scroll_vertical_custom_step", custom_step)
        .describe("scroll_deadzone",
                  {.doc = "Pixels a touch moves before it scrolls, so taps reach the children.",
                   .min = 0})
        .describe("follow_focus", {.doc = "Scroll to show a control inside it when it gets focus."})
        .describe("scroll_horizontal_by_default",
                  {.doc = "The wheel scrolls across, and Shift with the wheel scrolls down."})
        .describe("draw_focus_border",
                  {.doc = "Draw the focus style over it while it or a control inside it has "
                          "focus, keeping its children inside the style's margins."});
}

void register_particle_components(ComponentRegistry& registry) {
    const FieldMeta curve{.doc = "Multiplies it over each particle's life: points [offset, value], "
                                 "offsets from 0 to 1, joined by straight lines. None is 1."};
    const auto range = [&](ComponentType& type, const std::string& name, const std::string& what,
                           std::optional<double> min = std::nullopt,
                           std::optional<double> max = std::nullopt) -> ComponentType& {
        return type.describe(name + "_min", {.doc = what + ", at least.", .min = min, .max = max})
            .describe(name + "_max", {.doc = what + ", at most.", .min = min, .max = max});
    };
    ComponentType& type =
        registry.add<CPUParticles2D>()
            .set_doc("Emits particles from the entity's Transform2D, moves and draws them, as "
                     "Godot's CPUParticles2D. Each takes a random value between each _min and "
                     "_max when emitted; a _curve multiplies it as it ages. Emits finished when "
                     "a one_shot's last particle dies.")
            .describe("emitting", {.doc = "Emit new particles. Setting it again restarts a "
                                          "one_shot."})
            .describe("amount", {.doc = "Particles alive at most: one cycle emits this many, "
                                        "spread over lifetime.",
                                 .min = 1,
                                 .max = 65536})
            .describe("lifetime",
                      {.doc = "Seconds each particle lives and each cycle lasts.", .min = 0.001})
            .describe("one_shot", {.doc = "Emit one cycle, then stop."})
            .describe("preprocess",
                      {.doc = "Seconds it runs ahead when it starts, so it starts full.", .min = 0})
            .describe("speed_scale", {.doc = "Multiplies time for its particles.", .min = 0})
            .describe("explosiveness",
                      {.doc = "0 spreads emission over the cycle; 1 emits all at once.",
                       .min = 0,
                       .max = 1})
            .describe(
                "randomness",
                {.doc = "Randomizes when each particle is emitted, 0 to 1.", .min = 0, .max = 1})
            .describe("lifetime_randomness",
                      {.doc = "Shortens each particle's life by up to this fraction.",
                       .min = 0,
                       .max = 1})
            .describe("seed", {.doc = "Varies the random values; the same seed and entity ID "
                                      "always give the same particles."})
            .describe("local_coords", {.doc = "Particles move with the entity; false leaves them "
                                              "where they were emitted."})
            .describe("draw_order", {.doc = "\"index\" draws in emission slots' order; "
                                            "\"lifetime\" draws the youngest on top.",
                                     .choices = {"index", "lifetime"}})
            .describe("texture",
                      {.doc = "Project path of a PNG, BMP or KTX2 file each particle draws; "
                              "\"\" draws a white pixel."})
            .describe("texture_filter",
                      {.doc = "As Sprite2D's.", .choices = {"inherit", "linear", "nearest"}})
            .describe("hframes",
                      {.doc = "Columns of frames in the texture.", .min = 1, .max = 16384})
            .describe("vframes", {.doc = "Rows of frames in the texture.", .min = 1, .max = 16384})
            .describe("anim_loop", {.doc = "Frames past the last start again from the first; "
                                           "false stays on the last."})
            .describe("z_index", {.doc = "As Sprite2D's.", .min = -4096, .max = 4096})
            .describe("z_as_relative", {.doc = "As Sprite2D's."})
            .describe("visible", {.doc = "Draw the particles and its children; they move either "
                                         "way."})
            .describe("material", {.doc = "Project path of a .material.json file; \"\" for none."})
            .describe("material_params", {.doc = "Values for the material's params by name."})
            .describe("light_mask",
                      {.doc = "Lit by lights whose range_item_cull_mask shares a bit.",
                       .min = 0,
                       .max = kMaxMask})
            .describe("emission_shape",
                      {.doc = "Where particles start: at the entity, in a circle (sphere), on "
                              "its edge, in a rectangle, in a ring, or at emission_points.",
                       .choices = {"point", "sphere", "sphere_surface", "rectangle", "ring",
                                   "points", "directed_points"}})
            .describe("emission_sphere_radius", {.doc = "In pixels.", .min = 0})
            .describe("emission_rect_extents",
                      {.doc = "Half the rectangle's width and height, in pixels.", .min = 0})
            .describe("emission_ring_radius", {.doc = "In pixels.", .min = 0})
            .describe("emission_ring_inner_radius", {.doc = "In pixels.", .min = 0})
            .describe("emission_points", {.doc = "Points particles start at, in pixels from "
                                                 "the entity; each takes one at random."})
            .describe("emission_normals",
                      {.doc = "For directed_points, the direction each point emits along."})
            .describe("emission_colors", {.doc = "Colors multiplying each point's particles."})
            .describe("particle_flag_align_y",
                      {.doc = "Turn each particle so its y axis points along its velocity."})
            .describe("direction", {.doc = "Which way particles go, as [x, y]."})
            .describe("spread", {.doc = "Degrees either side of direction.", .min = 0, .max = 180})
            .describe("gravity", {.doc = "In pixels per second squared, as [x, y]."});
    range(type, "initial_velocity", "Starting speed in pixels per second");
    range(type, "angular_velocity", "Degrees per second it turns")
        .describe("angular_velocity_curve", curve);
    range(type, "orbit_velocity", "Turns per second round the entity")
        .describe("orbit_velocity_curve", curve);
    range(type, "linear_accel", "Pixels per second squared along its velocity")
        .describe("linear_accel_curve", curve);
    range(type, "radial_accel", "Pixels per second squared away from the entity")
        .describe("radial_accel_curve", curve);
    range(type, "tangential_accel", "Pixels per second squared round the entity")
        .describe("tangential_accel_curve", curve);
    range(type, "damping", "Pixels per second squared it slows by", 0)
        .describe("damping_curve", curve);
    range(type, "angle", "Starting rotation in degrees").describe("angle_curve", curve);
    range(type, "scale_amount", "Size, times the texture's").describe("scale_amount_curve", curve);
    type.describe("color", {.doc = "Multiplies the texture, as [r, g, b, a] from 0 to 1."})
        .describe("color_ramp", {.doc = "Colors over each particle's life, multiplying color: "
                                        "points {offset, color}, blended between."})
        .describe("color_initial_ramp", {.doc = "Each particle multiplies its color by one "
                                                "picked at random from this gradient."});
    range(type, "hue_variation", "Turns its hue, as a fraction of the color wheel", -1, 1)
        .describe("hue_variation_curve", curve);
    range(type, "anim_speed", "Times its frames play over its life", 0)
        .describe("anim_speed_curve", curve);
    range(type, "anim_offset", "The frame it starts on, as a fraction of the frames", 0, 1)
        .describe("anim_offset_curve", curve);

    registry.add<GPUParticles2D>()
        .set_doc("Emits particles from the entity's Transform2D and moves them on the GPU, as "
                 "Godot's GPUParticles2D, as its process_material file says. Emits finished "
                 "when a one_shot's cycle has had time to end.")
        .describe("emitting", {.doc = "Emit new particles. Setting it again restarts a one_shot, "
                                      "which sets it back to false after one lifetime."})
        .describe("amount", {.doc = "Particles alive at most: one cycle emits this many, spread "
                                    "over lifetime.",
                             .min = 1,
                             .max = 1000000})
        .describe("amount_ratio", {.doc = "The share of amount it emits.", .min = 0, .max = 1})
        .describe("sub_emitter",
                  {.doc = "ID of another GPUParticles2D its particles start particles in, as "
                          "its process material's sub_emitter_mode says; that one then emits "
                          "only those, stepping with this one. null for none."})
        .describe("process_material",
                  {.doc = "Project path of a .process_material.json file: how particles start "
                          "and move. \"\" emits still white pixels."})
        .describe("texture", {.doc = "Project path of a PNG, BMP or KTX2 file each particle draws; "
                                     "\"\" draws a white pixel."})
        .describe("lifetime",
                  {.doc = "Seconds each particle lives and each cycle lasts.", .min = 0.001})
        .describe(
            "interp_to_end",
            {.doc = "Ages every particle toward the end of its life, 0 to 1.", .min = 0, .max = 1})
        .describe("one_shot", {.doc = "Emit one cycle, then stop."})
        .describe("preprocess",
                  {.doc = "Seconds it runs ahead when it starts, so it starts full.", .min = 0})
        .describe("speed_scale", {.doc = "Multiplies time for its particles.", .min = 0})
        .describe(
            "explosiveness",
            {.doc = "0 spreads emission over the cycle; 1 emits all at once.", .min = 0, .max = 1})
        .describe("randomness",
                  {.doc = "Randomizes when each particle is emitted, 0 to 1.", .min = 0, .max = 1})
        .describe("use_fixed_seed",
                  {.doc = "Use seed as it is, so every restart repeats; otherwise seed and the "
                          "entity's ID vary the particles, and each one_shot restart differs."})
        .describe("seed", {.doc = "The random seed; see use_fixed_seed."})
        .describe("fixed_fps", {.doc = "Steps per second the particles move in; 0 steps once a "
                                       "tick.",
                                .min = 0,
                                .max = 1000})
        .describe("interpolate", {.doc = "Draw particles where they are between steps."})
        .describe("fract_delta", {.doc = "A particle emitted partway through a step moves for "
                                         "that part only, so emission is smooth."})
        .describe("collision_base_size",
                  {.doc = "How near, in pixels, a particle comes to a LightOccluder2D before it "
                          "collides, when its process material collides; times half the "
                          "texture's average side.",
                   .min = 0.0})
        .describe("local_coords", {.doc = "Particles move with the entity; false leaves them "
                                          "where they were emitted."})
        .describe("draw_order",
                  {.doc = "\"index\" draws in emission slots' order; \"lifetime\" draws the "
                          "youngest on top, \"reverse_lifetime\" the oldest.",
                   .choices = {"index", "lifetime", "reverse_lifetime"}})
        .describe("trail_enabled",
                  {.doc = "Draw each particle as a ribbon along the path it took, the texture "
                          "running down it. Steps at 60 a second when fixed_fps is 0."})
        .describe("trail_lifetime",
                  {.doc = "Seconds of each particle's path its trail shows.", .min = 0.01})
        .describe("trail_sections",
                  {.doc = "Points along the path the ribbon bends at.", .min = 2, .max = 128})
        .describe("trail_section_subdivisions",
                  {.doc = "Segments each section's curve is drawn with.", .min = 1, .max = 1024})
        .describe("texture_filter",
                  {.doc = "As Sprite2D's.", .choices = {"inherit", "linear", "nearest"}})
        .describe("hframes", {.doc = "Columns of frames in the texture.", .min = 1, .max = 16384})
        .describe("vframes", {.doc = "Rows of frames in the texture.", .min = 1, .max = 16384})
        .describe("anim_loop", {.doc = "Frames past the last start again from the first; false "
                                       "stays on the last."})
        .describe("z_index", {.doc = "As Sprite2D's.", .min = -4096, .max = 4096})
        .describe("z_as_relative", {.doc = "As Sprite2D's."})
        .describe("visible", {.doc = "Draw the particles and its children; they move either way."})
        .describe("material", {.doc = "Project path of a .material.json file; \"\" for none."})
        .describe("material_params", {.doc = "Values for the material's params by name."})
        .describe("light_mask", {.doc = "Lit by lights whose range_item_cull_mask shares a bit.",
                                 .min = 0,
                                 .max = kMaxMask});
}

void register_audio_components(ComponentRegistry& registry) {
    const FieldMeta stream{.doc = "Project path of a .wav, .ogg, .mp3 or .audio.json file; \"\" "
                                  "plays nothing."};
    const FieldMeta volume{.doc = "Volume in dB.", .min = -80};
    const FieldMeta pitch{.doc = "Speed and pitch, times the sound's own.", .min = 0.01, .max = 16};
    const FieldMeta playing{.doc = "Whether it plays. Setting it starts the sound from the "
                                   "beginning or stops it; the sound ending sets it to false."};
    const FieldMeta autoplay{.doc = "Plays once the audio system first sees it."};
    const FieldMeta paused{.doc = "Holds its sounds where they are; clearing it resumes them."};
    const FieldMeta polyphony{
        .doc = "Sounds it may play at once; playing again past this stops the oldest.",
        .min = 1,
        .max = 64};
    const FieldMeta bus{.doc = "The bus it plays through, from the project's bus layout; Master "
                               "if there is no such bus."};
    const FieldMeta parameters{
        .doc = "The stream's parameters, as Godot's parameters/*. Interactive music: "
               "switch_to_clip, a clip's name, which the music moves to as its transition says "
               "and which then follows the clip it goes to. Synchronized streams: "
               "stream_<index>_volume_db, a layer's volume."};
    registry.add<AudioStreamPlayer>()
        .set_doc("Plays a sound through a bus, as Godot's AudioStreamPlayer, the same wherever "
                 "the entity is. Emits finished when a sound ends.")
        .describe("stream", stream)
        .describe("volume_db", volume)
        .describe("pitch_scale", pitch)
        .describe("playing", playing)
        .describe("autoplay", autoplay)
        .describe("stream_paused", paused)
        .describe("max_polyphony", polyphony)
        .describe("bus", bus)
        .describe("parameters", parameters);
    registry.add<AudioStreamPlayer2D>()
        .set_doc("Plays a sound at the entity's Transform2D, as Godot's AudioStreamPlayer2D: "
                 "fading with distance from the AudioListener2D (or the middle of the screen) "
                 "and panned by its offset across the screen. Emits finished when a sound ends.")
        .describe("stream", stream)
        .describe("volume_db", volume)
        .describe("pitch_scale", pitch)
        .describe("playing", playing)
        .describe("autoplay", autoplay)
        .describe("stream_paused", paused)
        .describe("max_distance",
                  {.doc = "Pixels from the listener beyond which it is silent.", .min = 1})
        .describe("attenuation", {.doc = "How fast it fades with distance: volume is (1 - "
                                         "distance / max_distance) to this power.",
                                  .min = 0})
        .describe("max_polyphony", polyphony)
        .describe(
            "panning_strength",
            {.doc = "Times the project's audio.panning_strength_2d; 0 for no panning.", .min = 0})
        .describe("bus", bus)
        .describe("area_mask", {.doc = "Area2D layers, as bits, whose audio_bus_override "
                                       "changes its bus while it is inside; the first such area "
                                       "by ID wins. 0 ignores areas."})
        .describe("parameters", parameters);
    registry.add<AudioListener2D>()
        .set_doc("Where AudioStreamPlayer2D sounds are heard from, as Godot's AudioListener2D, "
                 "at its Transform2D and turned with it. The first current one by ID is used; "
                 "with none, the middle of the viewport.")
        .describe("current", {.doc = "Whether it is the listener."});
}

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
    const FieldMeta layer_min{.doc = "Lights canvases with at least this layer: the world is 0, "
                                     "a CanvasLayer its layer.",
                              .min = -512,
                              .max = 512};
    const FieldMeta layer_max{
        .doc = "Lights canvases with at most this layer.", .min = -512, .max = 512};
    const FieldMeta cull_mask{.doc = "Lights sprites and layers whose light_mask shares a bit.",
                              .min = 0,
                              .max = kMaxMask};
    const FieldMeta shadow_enabled{.doc = "LightOccluder2D polygons and tiles' occluders cast "
                                          "shadows from it."};
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
        .set_doc("Position, rotation and scale in 2D. Units are pixels and y points down. The "
                 "entities in children are placed in its space: they move, turn and scale with "
                 "it, and draw after it.")
        .describe("position", {.doc = "Position in pixels, as [x, y], in its parent's space."})
        .describe("rotation", {.doc = "Rotation in radians; positive turns clockwise."})
        .describe("scale", {.doc = "Scale factor on each axis, as [x, y]."})
        .describe("skew", {.doc = "Radians its y axis leans from square to its x axis, "
                                  "under 89.9 degrees either way.",
                           .min = -1.569,
                           .max = 1.569})
        .describe("top_level", {.doc = "Placed in the world rather than its parent's space, and "
                                       "drawn after its parent's tree without its parent's tint "
                                       "or z_index. A hidden parent still hides it."})
        .describe("children", {.doc = "Entities with a Transform2D placed in its space, in "
                                      "drawing order. Each has one parent."});

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
        .describe("texture",
                  {.doc = "Project path of a PNG, BMP or KTX2 file; \"\" draws nothing."})
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
        .describe("modulate", {.doc = "Color the texture and its children are multiplied by, as "
                                      "[r, g, b, a] from 0 to 1.",
                               .min = 0,
                               .max = 1})
        .describe("z_index",
                  {.doc = "Higher draws on top; added to its parents'.", .min = -4096, .max = 4096})
        .describe("z_as_relative", {.doc = "Add z_index to its parents'; false makes it its own, "
                                           "as Godot's."})
        .describe("visible", {.doc = "Draw it and its children."})
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
        .set_doc("Plays a keyframe animation from a .anim.json file on the entity's components, "
                 "and on the entities its targets name. Emits animation_finished when an "
                 "animation that doesn't loop ends, and the events of its event tracks.")
        .describe("library", {.doc = "Project path of a .anim.json file."})
        .describe("animation", {.doc = "Name of the animation to play."})
        .describe("playing", {.doc = "Advance the animation each tick."})
        .describe("speed_scale", {.doc = "Multiplies the playback speed; negative plays "
                                         "backwards."})
        .describe("time", {.doc = "Seconds into the animation, from 0 to its length.", .min = 0})
        .describe("backwards", {.doc = "Play toward the start, as Godot's play_backwards. "
                                       "Pingpong loops flip it at each end."})
        .describe("targets", {.doc = "The entity each track target names, by name; tracks with "
                                     "no target animate this entity."});

    registry.add<TileMapLayer>()
        .set_doc("A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D "
                 "if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them "
                 "with tilemap.paint and tilemap.set.")
        .describe("tile_set", {.doc = "Project path of a .tileset.json file."})
        .describe("origin", {.doc = "The cell the first key of the first row stands for, as "
                                    "[column, row]."})
        .describe("legend", {.doc = "The tile each key stands for: a tile's name or "
                                    "\"source:column,row\", then optionally \":\" and an "
                                    "alternative's number and \":\" and turns, letters h, v, "
                                    "t (flip horizontally, vertically, transpose), as "
                                    "\"crate:1:hv\". Keys are 1 to 3 characters, all the same "
                                    "length."})
        .describe("rows", {.doc = "Cells from the origin, one string of keys per row, \".\" for "
                                  "an empty cell."})
        .describe("z_index",
                  {.doc = "Higher draws on top; added to its parents'.", .min = -4096, .max = 4096})
        .describe("z_as_relative", {.doc = "Add z_index to its parents'; false makes it its own, "
                                           "as Godot's."})
        .describe("visible", {.doc = "Draw it and its children."})
        .describe("modulate", {.doc = "Color every tile and its children are multiplied by, as "
                                      "[r, g, b, a] from 0 to 1.",
                               .min = 0,
                               .max = 1})
        .describe("texture_filter",
                  {.doc = "How tiles are sampled when scaled: \"nearest\" keeps pixel art "
                          "sharp. \"inherit\" uses the project's rendering.texture_filter.",
                   .choices = {"inherit", "linear", "nearest"}})
        .describe("light_mask", light_mask)
        .describe("collision_enabled", {.doc = "Its tiles' collision polygons are solid in 2D "
                                               "physics."})
        .describe("occlusion_enabled",
                  {.doc = "Its tiles' occluders cast shadows from 2D lights while it is visible."});

    registry.add<PointLight2D>()
        .set_doc("A light shining from the entity's Transform2D through a texture. It lights "
                 "sprites and tile map layers whose light_mask shares a bit with "
                 "range_item_cull_mask, in canvases its layer range holds; with shadow_enabled, "
                 "LightOccluder2D polygons and tiles' occluders cast shadows from it.")
        .describe("enabled", {.doc = "Shine."})
        .describe("texture",
                  {.doc = "Project path of a PNG, BMP or KTX2 file whose color and alpha "
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
        .describe("range_layer_min", layer_min)
        .describe("range_layer_max", layer_max)
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
        .describe("range_layer_min", layer_min)
        .describe("range_layer_max", layer_max)
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
        .describe("sdf_collision",
                  {.doc = "GPU particles whose process material collides bounce off it or die."})
        .describe("occluder_light_mask",
                  {.doc = "Casts shadows from lights whose shadow_item_cull_mask shares a bit.",
                   .min = 0,
                   .max = kMaxMask});

    registry.add<CanvasModulate>()
        .set_doc("Tints everything lit in its canvas: the light the world, or the CanvasLayer "
                 "it is in, has where no light reaches. One per canvas.")
        .describe("color", {.doc = "As [r, g, b, a] from 0 to 1.", .min = 0, .max = 1});

    const FieldMeta limit{.doc = "Edge of the world the view stays inside, in pixels."};
    const FieldMeta drag_margin{
        .doc = "How far the entity moves from the center before the camera follows, as a share "
               "of half the screen.",
        .min = 0,
        .max = 1};
    const FieldMeta drag_offset{
        .doc = "Where the camera sits within its drag margins, from -1 to 1.", .min = -1, .max = 1};
    registry.add<Camera2D>()
        .set_doc("The part of the world the window shows, seen from the entity's Transform2D, as "
                 "Godot's Camera2D. The first enabled one in drawing order is current. Controls "
                 "stay put on the screen.")
        .describe("enabled", {.doc = "Whether it can be the current camera."})
        .describe("anchor_mode", {.doc = "\"drag_center\" puts the middle of the screen at the "
                                         "camera; \"fixed_top_left\" its top left corner.",
                                  .choices = {"drag_center", "fixed_top_left"}})
        .describe("ignore_rotation", {.doc = "Keep the view upright when the camera turns."})
        .describe("offset", {.doc = "Moves the view, in pixels, as [x, y]."})
        .describe("zoom", {.doc = "[2, 2] shows things twice as big; 0 counts as 1."})
        .describe("limit_enabled", {.doc = "Keep the view inside the limits."})
        .describe("limit_left", limit)
        .describe("limit_top", limit)
        .describe("limit_right", limit)
        .describe("limit_bottom", limit)
        .describe("limit_smoothed",
                  {.doc = "With position smoothing, glide to the limits instead of stopping."})
        .describe("position_smoothing_enabled",
                  {.doc = "Glide after the entity instead of moving with it."})
        .describe(
            "position_smoothing_speed",
            {.doc = "Each tick it closes this times the tick's seconds of the gap.", .min = 0})
        .describe("rotation_smoothing_enabled",
                  {.doc = "Turn smoothly with the entity, when not ignoring rotation."})
        .describe("rotation_smoothing_speed",
                  {.doc = "Each tick it turns this times the tick's seconds of the way.", .min = 0})
        .describe("drag_horizontal_enabled",
                  {.doc = "Follow the entity across only when it leaves the drag margins."})
        .describe("drag_vertical_enabled",
                  {.doc = "Follow the entity up and down only when it leaves the drag margins."})
        .describe("drag_horizontal_offset", drag_offset)
        .describe("drag_vertical_offset", drag_offset)
        .describe("drag_left_margin", drag_margin)
        .describe("drag_top_margin", drag_margin)
        .describe("drag_right_margin", drag_margin)
        .describe("drag_bottom_margin", drag_margin);

    registry.add<CanvasLayer>()
        .set_doc("A canvas of its own over or under the world, as Godot's CanvasLayer: the "
                 "entities in its children, and theirs, draw in it, moved by its offset, "
                 "rotation and scale instead of the camera. Canvases draw in order of layer; the "
                 "world is layer 0. Its controls lay out in the viewport. Lights whose layer "
                 "range holds its layer shade it; a CanvasModulate in it tints only it.")
        .describe("layer", {.doc = "Draws over canvases with a lower layer; the world is 0, under "
                                   "a CanvasLayer of 0. Negative layers draw behind the world."})
        .describe("visible", {.doc = "Hides everything in it when false."})
        .describe("offset", {.doc = "Moves it, in pixels, as [x, y]."})
        .describe("rotation", {.doc = "Turns it around its top left, in radians."})
        .describe("scale", {.doc = "Scales it from its top left, as [x, y]."})
        .describe("follow_viewport_enabled",
                  {.doc = "Seen through the camera too, as part of the world in front of it or "
                          "behind it."})
        .describe("follow_viewport_scale",
                  {.doc = "Following the viewport, scales it around the middle of the screen: "
                          "below 1 looks farther than the world, above 1 nearer."})
        .describe("children",
                  {.doc = "Entities that draw in it, with their own children: 2D entities and "
                          "controls with no parent."});

    registry.add<Parallax2D>()
        .set_doc("Scrolls the entity's children at a different speed from the camera, as Godot's "
                 "Parallax2D: each tick it sets the entity's Transform2D position from the "
                 "camera's view. With a repeat_size its children draw again every repeat_size, "
                 "so a background goes on forever.")
        .describe("scroll_scale",
                  {.doc = "How fast it scrolls as the camera moves, as [x, y]: 0 stays on the "
                          "screen, 1 moves with the world, 0.5 looks far away."})
        .describe("scroll_offset", {.doc = "Where it sits when the view is at [0, 0], in pixels."})
        .describe("repeat_size",
                  {.doc = "Draws the children again every this many pixels, as [x, y]; 0 for "
                          "none. At least the screen's size covers it.",
                   .min = 0})
        .describe("autoscroll",
                  {.doc = "Pixels a second scroll_offset moves by, as [x, y]; only with a "
                          "repeat_size, as in Godot."})
        .describe("repeat_times", {.doc = "Copies drawn each way past the first, half of them "
                                          "before it, for a view zoomed out.",
                                   .min = 1})
        .describe("limit_begin",
                  {.doc = "Top left of the views it scrolls for, in pixels; past it, it stops."})
        .describe("limit_end", {.doc = "Bottom right of the views it scrolls for, in pixels."})
        .describe("follow_viewport",
                  {.doc = "Off for one in a CanvasLayer that doesn't follow the viewport, so it "
                          "scrolls as if it did."})
        .describe("ignore_camera_scroll",
                  {.doc = "Leave screen_offset alone, so code or an animation scrolls it."})
        .describe("screen_offset",
                  {.doc = "The view it scrolls for, set each tick from the current camera: the "
                          "middle of the screen in the world less half the screen."});

    register_physics_components(registry);
    register_particle_components(registry);
    register_audio_components(registry);
    register_ui_components(registry);
}

} // namespace slash::world
