# OpenXR runtime

A headset's own OpenXR runtime driving a session, as Godot's `OpenXRInterface` runs one.
[project.json](project.json) enables XR, so a session starts on the test headset;
[calls.jsonl](calls.jsonl) switches to the machine's OpenXR runtime over JSON-RPC (MCP tool
`xr_set`), and [results.jsonl](results.jsonl) holds the replies. The runtime here is the engine's
stand-in (`tests/fake_openxr_runtime.cpp`), which the example's runner names in
`XR_RUNTIME_JSON` as the Khronos loader reads it, playing [openxr_script.txt](openxr_script.txt):
a Meta Touch controller aimed ahead of a head 1.65 m up, its trigger and A button pressed at the
third frame, and the runtime ending the session at the eighth.

```text
xr.set {"interface": "openxr"}
step {"ticks": 4}
xr.state {"trackers": ["right_hand"]}
{"interfaces":[...,{"name":"OpenXR","openxr":{"session_state":"focused","frames":4,...},...}],...}
```

- `interface` switches on its own, so a command that also sets the test headset is an error at
  `/interface`.
- The session goes from ready to focused at the first tick (`session_begun`,
  `session_focussed`), and the controller's interaction profile reaches its node as
  `profile_changed`.
- Godot's action map binds the runtime's paths to the tracker's inputs: `trigger` is 0.9 and
  `ax_button` the A button. The aim pose places the `XRController3D`.
- When the runtime stops the session, `session_stopping` and `instance_exiting` follow, and
  `interface` `"test"` goes back to the test headset.

A session has no window, so its OpenXR session is headless and needs a runtime with
`XR_MND_headless`; a windowed game draws each eye into the headset instead, on Vulkan or
Direct3D 12 (`tests/test_openxr.cpp` draws a post into the stand-in's Vulkan swapchains). [XR](../../docs/reference/scene-format.md#xr) in the scene format
reference explains the interface, and [commands](../../docs/reference/commands.md#xrset)
`xr.set`.
