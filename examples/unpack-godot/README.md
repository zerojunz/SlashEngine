# Import a shipped Godot game

`project.import` also takes a game exported with Godot 4: its `.pck` file, or its executable
with the pack inside. It unpacks the game back into the Godot project it was made from, then
converts that project as it would a project folder. [jumper/](jumper/) is "Jumper", a small game
as its maker keeps it in Godot: [project.godot](jumper/project.godot), a scene
([main.tscn](jumper/main.tscn)), a script ([player.gd](jumper/player.gd)) and the `.import`
files of a texture and a sound. [main.cpp](main.cpp) first plays the part of Godot's export and
writes `shipped/jumper.pck` as Godot 4.5's export lays it out, then makes the calls an agent would
make with only the shipped game; [expected.txt](expected.txt) holds the replies and the files the
import wrote. From a shell, `slash-cli import shipped/jumper.pck --unpack-to godot` does the
same.

```text
project.import {"source":"shipped/jumper.pck","unpack_to":"godot"}
{"converted": {"assets": 2, "input_actions": 1, "scenes": 1, ...},
 "files": 12, "main_scene": "godot/main.scene.json",
 "unpacked": {"folder": "godot", "pack": {"embedded": false, "files": 10, "format": 3,
                                          "godot": "4.5.0"},
              "scenes": 1, "scripts": 1, "scripts_from_tokens": 1, "sounds": 1,
              "textures": 1, "unsupported": []}, ...}
```

- Godot's export turns text scenes and resources into binary ones, so `main.tscn` comes back
  from `main.scn` as Godot's text saver would write it. Its external resources keep their paths
  and UIDs but are numbered `1`, `2` and `3`, since the binary form keeps no IDs.
- Scripts are exported as binary tokens (`player.gdc`), and come back as source with every line
  and indent where it was, so errors still point at the right lines. The comments are gone:
  tokens don't keep them.
- `project.binary` comes back as `project.godot`. Imported textures (`.ctex`) come back as PNG
  files and sounds (`.sample`) as WAV files, under the `.import` files that keep their UIDs:
  here the same pixels and the same sound the game was made with.
- `unpack_to` writes the recovered Godot project to `godot/`, and the import uses its assets
  there. Without it, only the assets come along, under the paths they had in the game.
- What can't come back is listed in the report's `"unpacked"` `"unsupported"`: a pack whose file
  list is encrypted with the game's own key is refused, and a file encrypted on its own is
  listed. Unpack only games you made or have the rights to.
