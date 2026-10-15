#include "slash/world/components.hpp"

#include "slash/world/component_registry.hpp"

namespace slash::world {

void register_builtin_components(ComponentRegistry& registry) {
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
                   .choices = {"inherit", "linear", "nearest"}});

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
}

} // namespace slash::world
