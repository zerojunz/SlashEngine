# XR

The XR server, interfaces, trackers and OpenXR.

Status: current · Updated: 2026-11-20

`engine/xr` is virtual and augmented reality, as Godot's `XRServer`, `XRInterface` and XR nodes
(`slash/xr/xr.hpp`). `xr::install` gives a world its server (in the world's context): interfaces
(`xr::Interface`, Godot's `XRInterface`), trackers (`xr::Tracker`, Godot's
`XRPositionalTracker`: poses and inputs by name), the world scale, origin and reference frame
(`center_on_hmd` ports Godot's). The `xr` system (priority -850) sets the origin and scale from
the current `XROrigin3D`, has each initialized interface `process` its trackers, then places
`XRCamera3D`, `XRController3D` and `XRAnchor3D` entities by their poses' adjusted transforms
under it and sends them their trackers' signals (kept as `detail::Signal`s in `src/xr.cpp`) as
world events, as Godot's `XRNode3D` binds to a tracker; each node's binding and tracking are
kept by entity ID. Sessions add `xr::TestInterface` (`src/test_interface.cpp`), a headset whose
trackers `xr.set` moves and presses (`engine/command/src/xr.cpp`), started by project.json's
`xr.openxr.enabled`, and put the server back with the world when a command fails or is a dry
run (`xr::save`, `restore`). `xr::check_nodes` holds Godot's configuration warnings, which
`xr.state` reports. `examples/xr-tracking` raises a sword and centers the play space.

Each eye is a view, as Godot's `XRInterface::get_transform_for_view` and
`get_projection_for_view`: `xr::views` gives the primary interface's views, each a place and a
field of view as its edges' tangents (`xr::Fov`), which `render::frustum` makes a projection of
(Godot's `Projection::set_frustum`). `scene3d::Renderer::set_view` (`CanvasOptions::view3d`,
`CaptureOptions::view3d` and `viewport`) draws the 3D world from one instead of the current
camera, which `frame.capture`'s `xr_view` does at the view's render target size.
`examples/xr-stereo-views` sees a post move between the eyes.

Face tracking is Godot's `XRFaceTracker` and `XRFaceModifier3D` (`slash/xr/face.hpp`,
`src/face.cpp`, its tables ported from Godot's): a face tracker's `Tracker::blend_shapes` holds
the 143 Unified Expressions weights (`set_face_blend_shapes`), `face_shape_named` reads a blend
shape's name in Unified Expressions', ARKit's, SRanipal's or Meta's standard, and
`face_blend_mapping` leaves out blended shapes whose parts the mesh has, as Godot's. The `xr`
system's `drive_faces` sets the `MeshInstance3D.blend_shapes` of each `XRFaceModifier3D`'s target
from its model's shape names. `fill_blended_shapes` (SlashEngine's own) fills in the shapes a
recording implies, for the test headset's face trackers (`xr.set`'s `blend_shapes`) and
`animation.import_face_capture` (`engine/command/src/face_capture.cpp`), which reads Live Link
Face's CSV into a `.anim.json` animation of blend shape tracks with keys reduced by Ramer,
Douglas and Peucker. `examples/face-capture` smiles live and plays a recording.

OpenXR is Godot's `OpenXRAPI` and `OpenXRInterface` (`slash/xr/openxr.hpp`, `src/openxr.cpp`),
on the Khronos loader built from its sources as `slash_openxr` (`cmake/Dependencies.cmake`; not
on the web or iOS yet, where `src/openxr_none.cpp` says so): the instance (1.1, then
1.0), the system, the session's state machine (begun on `READY`, ended on `STOPPING`), the play
space (`local_floor` emulated from the stage and local spaces), Godot's default action map
(`src/action_map.cpp`, `xr::default_action_map`; vendor profiles only with their extension),
poses, inputs, views and haptics, its session states as world events. `use_openxr_interface`
starts it; games do when `xr.openxr.enabled` is set and they have a window, and `xr.set`'s
`interface` switches a session to it. Windowed games draw into the headset as Godot's
`OpenXRVulkanExtension`, `OpenXRD3D12Extension` and `OpenXROpenGLExtension` do
(`src/openxr_graphics.cpp`; OpenGL binds the context SDL made on WGL, GLX or EGL, from the GL
device's `native_device`, and its copy turns the picture over, as OpenXR's GL rows go up):
`prepare_openxr_device` makes the instance with the graphics extension before the render device,
which `render::DeviceOptions::xr` makes as the runtime asks, through hooks
`cmake/patches/sdl-openxr.cmake` adds to SDL (the runtime makes Vulkan's instance and device and
names the GPU, or names Direct3D 12's adapter or Metal's `MTLDevice`); the session makes a
swapchain and a render
target per view, `begin_openxr_frame` waits, begins and acquires, `Game::draw_headset` draws each
eye with `scene3d::eye_view`, and `end_openxr_frame` copies the targets in with
`RenderDevice::copy_to_native` (`src/native_copy.cpp`: barriers and a copy recorded into SDL's
command buffer; a blit encoder on Metal, `src/native_copy_metal.mm`) and ends the frame with a projection layer. Without a device, sessions need
`XR_MND_headless` and end each frame with no layers. `tests/fake_openxr_runtime.cpp`
is a stand-in runtime the loader loads through the manifest the build writes
(`tests/fake_openxr.json`), handed to it with `xr::set_openxr_runtime` (slash-cli's
`--openxr-runtime`, as the loader's init properties, since it ignores `XR_RUNTIME_JSON` in the
elevated processes Windows CI runs), playing a script (`FAKE_OPENXR_SCRIPT`: poses and inputs by frame,
lost poses, recentering, exit, eye sizes and pixels to read back) and logging the calls it gets
(`FAKE_OPENXR_LOG`); offered `XR_KHR_vulkan_enable2`, or `XR_KHR_opengl_enable` and
`XR_MNDX_egl_enable`, or on macOS `XR_KHR_metal_enable` (`tests/fake_openxr_metal.mm`), it hands
out Vulkan, OpenGL or Metal swapchains and logs the probed pixels of each eye it is shown. `examples/openxr-runtime` runs a session on it.

On Android the loader is built with its JNI parts (jnipp and android-jni-wrappers, which need
exceptions) and asks the system's runtime broker for the runtime; `src/openxr_platform.cpp`
(Godot's `OpenXRAndroidExtension`) gives `xrInitializeLoaderKHR` and
`XR_KHR_android_create_instance` the Java VM and SDL's activity, and the `OpenGLES` graphics
binds the `opengl_es` driver's EGL context (`XR_KHR_opengl_es_enable`). Exports of games with
`xr.openxr.enabled` (`SLASH_GAME_XR`) add the loader's permissions and queries, head tracking and
the immersive categories to the manifest. No runtime reaches CI's emulator, so a headless game
only looks for one (`xr::openxr_runtime_extensions`) and reports it in its summary's `xr`.
