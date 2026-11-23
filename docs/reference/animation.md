# Keyframe animation

Keyframe animation and animation trees.

Status: current · Updated: 2026-11-20

`engine/animation` is keyframe animation, as Godot's AnimationPlayer. A `.anim.json` file
(`$cli schema animation`) holds animations by name: a length, a loop mode (`none`, `linear`,
`pingpong`) and tracks. A value track's `path` names a component field or part of one
(`Sprite2D/modulate/a`, `Transform2D/position/y`) and its keys are values with Godot's
interpolation and easing; an event track emits a world event at each key. The
`animation_player` system (priority 900, before `animated_sprite_2d`) plays each entity's
`Animator` on its own components, through `ComponentType::assign`, so any reflected field
animates, a game's own included; a track with a `target` acts on the entity the player's
`targets` maps it to, as a Godot track's node path does, and `animation::check_players` (run by
`scene validate` and exports) catches targets missing from `targets`. A library can also be a
model file, and an animation can come `from` a model's animation, posing the entity's model
(see Rendering's skinned meshes). Keys are passed as Godot passes them: after where playback was,
up to and including where it got to. `animation.eval` returns a track's values at given times;
`examples/keyframe-animation` plays a walk cycle and a hit. An animation `from` a model takes a
`speed` and `layers` (`animation::AnimationLayer`: a weight, an amplitude, bones' scale, shift
and held angles and added waves, the pieces of a creature's motion), which `resolve_clip` bakes
into `Animation::layered`, a model of one animation keyed 60 times a second that players and
trees pose from; `animation.edit` (`engine/command/src/animation_layers.cpp`) writes them with
undo, and `animation.preview` and `animation.tune` take a `library` in place of a creature,
measuring a humanoid's gait from its feet and tuning a layer of its own.
`examples/animation-tuning` tunes a hero's walk this way too. Cinematics are Godot's timeline tracks: a `"type": "bezier"` track
eases one number along its keys' `in` and `out` handles (`bezier_value`, Godot's
`bezier_track_interpolate`), an `"animation"` track starts and stops the animations of another
`Animator` it targets (`play_started` in `src/player.cpp`, `[stop]` stopping it; trees
through `play_animation_track` in `src/tree_system.cpp`), a value
track that sets `Camera3D/current` cuts to that camera (`make_current_camera`), and an
animation's `markers` name times. `animation.seek` (`animation::seek`) moves a player to a time
or marker and sets what its tracks, and the players its animation tracks start, set there, as
Godot's `seek` with `update_only`; events wait for playback. An `"audio"` track starts its keys'
streams on the audio player it targets, as Godot's `AnimationMixer` (`src/audio_tracks.cpp`: the
streams each player and tree track keeps playing, and Godot's ending process with its blended
volumes); the audio module plays them on a polyphonic voice of the player's own through
`world::set_audio_track_sound` (`slash/world/audio_tracks.hpp`), since animation doesn't depend
on audio, and `audio.state` counts them as `track_streams`. `examples/cinematics` plays an
intro and `examples/cinematic-sound` its sounds.

`AnimationGraph` is Godot's AnimationTree: a `.animtree.json` file (`$cli schema animtree`;
`tree.hpp`) of blend tree, animation, Blend2/3, Add2/3, Sub2, OneShot, TimeScale, TimeSeek,
Transition, BlendSpace1D/2D and StateMachine nodes, played with the animations of the component's
`libraries` by the `animation_tree` system (priority 910). `src/tree_run.cpp` ports the nodes'
`_process` calls and the state machine playback's travel, keeping what they need in the
component's `state` (a state machine's playback as an object) and reading `parameters`;
`src/tree_system.cpp` ports `AnimationMixer`'s caches, blending and root motion, and hands
model poses to `scene3d` through the clip source, ahead of the `Animator`'s. Godot
compares weights in float, so do the same (`is_zero_approx(float)` in `src/internal.hpp`).
State machine transitions' `advance_expression`s are Godot's `Expression`, in
`slash/core/expression.hpp`, reading the file's `variables` and `triggers` as parameters.
`animation.tree` lists a tree's parameters and what plays, `animation.set_parameters` sets them,
and `check_trees` backs `scene validate` and exports; `examples/animation-tree` points a waving
arm.

IK follows Godot 4.6's skeleton modifiers (`engine/scene3d/src/ik.cpp`, ported from
`TwoBoneIK3D`, `ChainIK3D`, `IterateIK3D`, `FabrikIK3D`, `CcdIK3D`, `JacobianIK3D` and
`JointLimitationCone3D`). `TwoBoneIK3D`, `FabrikIK3D`, `CcdIK3D` and `JacobianIK3D` components on
a drawing entity or its `Transform3D` children turn the model's bones after `pose_of`:
`apply_modifiers` runs them where poses are made (drawing and `skeleton.get`), with targets and
poles placed by the caller so drawing between ticks moves them smoothly. A two-bone or
deterministic modifier solves from the pose each time; the others carry their chains from tick
to tick, as Godot's do from frame to frame, in a `ModifierState` in the world's context that the
`skeleton_modifiers` system (priority 920, added by `animation::add_systems`) steps once a
tick and `apply_modifiers` only shows. Angles go through `slash::atan2` and `cos_sin` and
scene3d builds with `-ffp-contract=off`, so chains converge the same everywhere.
`check_modifiers` backs `scene validate`; `examples/ik` reaches with an arm.
`BoneLookAt3D` and `BoneCopy3D` are Godot 4.6's too, ported line for line in
`src/bone_constraints.cpp` (its look-at with limits, damping and flip detection, and
`BoneConstraint3D`'s copies by axis), run after the chains; a look's easing over `duration` is
kept in the `ModifierState` and stepped with Godot's tween equations (`slash/core/easing.hpp`,
Penner's, through `slash::cos_sin`, `exp` and `ln`).

Ragdolls follow Godot 4.6's `BoneSimulation3D` and `SimulatedBone3D`
(`engine/scene3d/src/physical_bones.cpp`, `run_simulator` in `ik.cpp`, and the `Kind::Bone`
bodies and `sync_bone_joints` in `engine/physics3d`). The simulator is a skeleton modifier.
`physical_bone_links` works out each physical bone's mode (follow, simulate or off) and the body
its joint holds it to, and reaches physics3d through `world::set_physical_bone_linker`, since
physics3d doesn't depend on scene3d. The `skeleton_modifiers` system makes simulating bodies
`top_level` and moves following ones to their bones before the physics step moves them as
kinematic bodies; `apply_modifiers` puts simulating bones where their bodies are.
`examples/ragdoll` lets a waving arm go limp.

Characters' and creatures' own ragdolls are SlashEngine's, after Unreal's physics assets and
Physical Animation component: a `.ragdoll.json` profile (`slash/world/ragdoll.hpp`,
`engine/world/src/ragdoll.cpp`; `$cli schema ragdoll`) names each bone's body, joint and muscle
over its file's `defaults`, with its balance, hit reactions and getting up, and an instance's
`overrides` are a merge patch over it (`world::merge_patch`). `Ragdoll3D`'s system
(`engine/scene3d/src/ragdoll.cpp`, priority 915) keeps a simulator and a `SimulatedBone3D` per
bone, sets each body's `PhysicalBoneDrive` from its state (animated, active, limp, getting
up), weakens hit bones and those under them, makes a ragdoll that leans past its balance limp,
and once it lies still moves the entity to it and plays its get-up animation;
`skeleton_modifiers` keeps the animated pose in `world::PhysicalBoneTargets` before the
simulator replaces it, and physics3d's `apply_drives` (`src/drives.cpp`) turns and pins
simulating bodies toward it as implicit springs, a local drive pushing its parent back as hard.
`Ragdoll2D` (`engine/physics2d/src/ragdoll.cpp`, priorities 940 and 955) does the same over a
cut-out's `Transform2D` tree with `RigidBody2D`s, `Collider2D`s and `PinJoint2D`s, telling
the animation's pose from where physics left each bone. `ragdoll.generate`, `ragdoll.hit` and
`ragdoll.tune` (`engine/command/src/ragdoll.cpp`) make, hit and tune them; `generate_ragdoll`
fits a profile to a skeleton's skin or a 2D tree, or, given a character's `world::Anatomy`
(`RagdollOptions::anatomy`; `ragdoll.generate`'s `fit`), to the anatomy's bones each body's bone
carries, keeping the skin's mass. `examples/ragdoll-profiles` knocks a kid down.

Bone attachments follow Godot's `BoneAttachment3D` (`engine/scene3d/src/bone_attachment.cpp`):
at the end of each `skeleton_modifiers` run, `place_attachments` poses each followed model
(`pose_of`, then `apply_modifiers`) and writes the attachment's `Transform3D`, parents first, so
bodies under it reach physics where the bone is. `check_attachments` backs `scene validate`;
`examples/bone-attachment` puts a sword in a waving hand.
