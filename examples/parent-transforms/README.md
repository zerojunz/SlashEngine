# Parent transforms

Entities placed in another's space, as Godot's Node2D children. In
[main.scene.json](main.scene.json) the boat is a static body whose `Transform2D` lists a deck
(its collision shape), a mast and a cannon as `children`, and the mast lists a flag.
[main.cpp](main.cpp) draws the textures, then makes the calls an agent would make over MCP or
JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
entity.get {"components":["Transform2D"],"id":"flag"}
{"components":{"Transform2D":{"position":[6.0,-9.0],...}},
 "global":{"position":[122,93],"rotation":0,"scale":[1,1]},"id":"flag","parent":"mast"}
```

- A child's `position`, `rotation`, `scale` and `skew` are in its parent's space: moving and
  turning the boat moves and turns everything in it. `entity.get` adds the `parent` and the
  `global` transform.
- Children draw after their parent, in the order listed, and before the roots made after it, so
  the rock covers the cannon. A parent's `z_index`, `visible` and `modulate` pass down: lifting
  the boat lifts its children over the rock.
- Bodies in a parent are where it puts them. A child with a shape and no body of its own is a
  shape of its parent's body, as a Godot `CollisionShape2D` node is: the ray finds the boat by
  its deck where the boat is now. The barrel, a body of its own, falls in the world while its
  own position stays in the boat's space.
- A child with `top_level` is placed in the world rather than its parent's space, as Godot's
  `top_level`: the shot stays where it was fired when the boat sails on. It draws after the
  rest of the boat's tree and before the rock made after the boat, and takes neither the boat's
  `z_index` nor its tint, though hiding the boat would still hide it.
- A `z_index` with `z_as_relative` off is the whole of it, not added to the parents': the
  cannon's -1 puts it under the boat and the rock whatever the boat's `z_index` is.
- An edit that would give a child two parents or make it its own ancestor fails, with the JSON
  Pointer of the entry at fault; `slash-cli scene validate` reports the same in a file.
- `entity.destroy` takes the entity's children with it and lists them as `descendants`.
