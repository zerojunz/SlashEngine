#include "slash/world/components.hpp"

#include "slash/world/component_registry.hpp"

namespace slash::world {

namespace {

// Godot's 20 render layers.
constexpr double kMaxMask = 1048575;

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
}

} // namespace slash::world
