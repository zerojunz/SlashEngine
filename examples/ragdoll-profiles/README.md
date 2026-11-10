# Ragdoll profiles

Characters' own ragdolls, after Unreal's physics assets and Physical Animation component:
`.ragdoll.json` profiles name each bone's body, joint and muscle, and how the whole keeps its
balance, takes hits and gets back up. [main.scene.json](main.scene.json) has a cut-out kid
standing on a floor, a `Transform2D` tree of a pelvis, chest, head, arm, hand, thigh, shin and
foot under `kid`, and the skinned arm of [skinned-meshes](../skinned-meshes/).

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `ragdoll.generate` fits a profile to the kid's tree (`ragdolls/kid.ragdoll.json`: capsules
  from each bone to its child, a rectangle round the pelvis's two, circles at the tips, hinges
  between them and balance on the pelvis) and gives `kid` a `Ragdoll2D`; for the arm it fits
  capsules to the bones its skin moves most and gives it a `Ragdoll3D`.
- Each bone becomes a `RigidBody2D` with a `CollisionShape2D`, frozen while the ragdoll is
  `animated`, and a `PinJoint2D` (`arm_joint`) holds the arm to the chest.
- `ragdoll.hit` on the arm weakens it fully and the hand under it half as much; the kid reacts,
  `active`, its muscles driving its bodies toward its pose, and back `animated` once they
  recover.
- `ragdoll.tune` sets this kid's knockdown, and a hard hit to the chest knocks it down. It
  falls and lies on the floor; once it has lain still, `run.until` finds it `getting_up`, its
  entity moved across to where it lies, then `animated` again.
- `events.read` lists the hits, the fall and the getting up, and a misspelled bone is an error
  with the closest name.
