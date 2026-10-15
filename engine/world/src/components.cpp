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
}

} // namespace slash::world
