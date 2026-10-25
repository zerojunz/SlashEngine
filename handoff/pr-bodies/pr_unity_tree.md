Before: a Unity `Animator` became an `AnimationPlayer` stuck on its controller's default state. The state machine (which state goes where, on which parameters) became a storyboard task in prose, so gameplay code had to rebuild every transition by hand.

After: the controller's base layer becomes an `.animtree.json` state machine over its animation library, and the `Animator` becomes an `AnimationTree` playing it, as Unity plays it. Gameplay code only sets the parameters, where Unity's `SetFloat`, `SetInteger`, `SetBool` and `SetTrigger` were called. Their names and defaults are in the controller's storyboard task.

```json
{"format": 1, "variables": {"Speed": 0.0, "Grounded": true}, "triggers": ["Jump"],
 "root": {"type": "StateMachine", "transitions": [
   {"from": "Start", "to": "Idle", "advance_mode": "auto", "priority": 0},
   {"from": "Idle", "to": "Jump", "advance_mode": "auto", "priority": 0,
    "xfade_time": 0.25, "advance_expression": "Jump"},
   {"from": "Jump", "to": "Idle", "advance_mode": "auto", "priority": 0,
    "xfade_time": 0.25, "switch_mode": "at_end"}]}}
```

How:

- **States:** each state is an `Animation` node at its place in Unity's graph.
- **Sub-state machines:** their states join the layer's machine. A transition into a sub-state machine goes through its entry transitions, then its default state. A transition to its exit goes through the parent's transitions from it, then the parent's entry. The base layer's exit starts over at its entry.
- **Start:** the entry's transitions become auto transitions from `Start`, conditional ones first.
- **Any State:** its transitions are copied to every state and tried first. A transition back to the same state is reported when `Can Transition To Self` is on, since a state machine's states don't go to themselves.
- **Conditions:** they become `advance_expression`, joined with `and` (`Speed > 0.5`, `not Is_Grounded`, `Hits == 3`). Float, int and bool parameters become the tree's `variables` and triggers its `triggers`, which a transition taken uses up, as Unity's are. A name an expression can't read is changed and reported (`Is Grounded` becomes `Is_Grounded`).
- **Timing:**
  - An exit time becomes `switch_mode: at_end`, with `break_loop_at_end` for a looping state. Unity's default exit time, where the cross-fade ends with the state, matches exactly; any other is reported.
  - Durations become `xfade_time`, a normalized one scaled by the state's length.
  - The order Unity tries transitions in becomes `priority`.
- **Unity cases with no direct counterpart:**
  - Muted transitions don't convert, and a solo transition mutes the state's others.
  - Two transitions between the same states become one that takes either's conditions. Godot's state machines hold one transition per pair. This is reported.
  - Transitions back to the same state, with no conditions and no exit time, offsets and interruption sources are reported.
- **No tree:** a controller with no transitions to convert still becomes an `AnimationPlayer` and its old task text.
- **Report and task:** the report's `animation_libraries` entries name the tree, `converted.animation_trees` counts trees, and the task links the tree file.
- **Tests:**
  - The Frog test now checks the tree, the `AnimationTree` components, the task and a `Jump` trigger that is taken and used up.
  - A new knight test covers a sub-state machine with entry and exit, Any State, mute and solo, merged transitions, a renamed parameter, int conditions and normalized durations.
- **Example:** `examples/import-unity` prints the tree, and the frog hops when the agent sets `Hop`.
- **Docs:** `docs/reference/unity-import.md` and the roadmap.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
