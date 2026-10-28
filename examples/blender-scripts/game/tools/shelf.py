# A bookshelf: two sides, a back and evenly spaced boards, in meters, standing on the ground.
# Run by SlashEngine's "blender" generator with params and seed; what it leaves in the scene is
# the model.
import bpy

width = params["width"]
depth = params["depth"]
height = params["height"]
boards = int(params["boards"])
thick = 0.02


def box(name, size, location):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    part = bpy.context.object
    part.name = name
    part.scale = size
    bpy.ops.object.transform_apply(scale=True)
    return part


parts = [
    box("Left", (thick, depth, height), (-width / 2 + thick / 2, 0, height / 2)),
    box("Right", (thick, depth, height), (width / 2 - thick / 2, 0, height / 2)),
    box("Back", (width, thick, height), (0, depth / 2 - thick / 2, height / 2)),
]
for i in range(boards):
    z = thick / 2 + i * (height - thick) / (boards - 1)
    parts.append(box(f"Board{i}", (width - 2 * thick, depth, thick), (0, 0, z)))

# One object, named as the params say, so the game finds it by name.
bpy.ops.object.select_all(action="DESELECT")
for part in parts:
    part.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
bpy.context.object.name = params["name"]
