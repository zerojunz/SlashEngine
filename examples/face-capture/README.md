# Face capture

A face that follows a face tracker live, then plays a recorded performance, as Godot's
`XRFaceTracker` and `XRFaceModifier3D` drive a face. [models/face.gltf](models/face.gltf) is
[blend-shapes](../blend-shapes/)'s face with its morph targets named after face shapes:
`MouthSmile` (Unified Expressions' name) and `jawOpen` (ARKit's). [main.scene.json](main.scene.json)
draws it with an `XRFaceModifier3D`, which follows the tracker `/user/face_tracker`:

```json
"face": {
  "components": {
    "MeshInstance3D": {"mesh": "models/face.gltf"},
    "Transform3D": {},
    "XRFaceModifier3D": {}
  }
}
```

[project.json](project.json) turns XR on, so sessions start the test headset.
[capture/talk.csv](capture/talk.csv) is a recording as Live Link Face writes one: a timecode at 60
frames a second, a column per ARKit shape (blinks, the jaw and each side's smile) and the head's
turn. The jaw opens and closes over half a second while the smile grows.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `xr.set` gives the face tracker ARKit's `mouthSmileLeft` and `mouthSmileRight` at 1. The
  modifier finds which of the mesh's shapes stand for face shapes, as Godot's does, and sets them
  each tick. The mesh has only the whole smile, `MouthSmile`, which Unified Expressions makes of
  the two sides' corner pulls and slants. The sides imply their parts, so the whole smile is their
  mean, 1. `frame.capture` sees the mouth's corners up, as in blend-shapes.
- `animation.import_face_capture` turns the recording into `talk`, an animation of a track per
  blend shape of the model the recording drives, in `anim/face.anim.json`. The head's turn and
  the shape count aren't face shapes, so it says it skipped them, and the blinks drive nothing
  on this face. Keys a straight line passes within 0.005 of are dropped, so 31 frames make 22
  keys over the two tracks.
- With the modifier removed and an `AnimationPlayer` playing `talk`, after 15 ticks (a quarter of
  a second) the jaw is fully open and the smile at 0.855, the mean of the two sides' 0.9 and
  0.81 at that frame. Every point `frame.capture` reads is lips.
