#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <memory>

namespace slash::render::detail {

// A device over desktop OpenGL 3.3 or OpenGL ES 3.0, which is WebGL 2 on the web: Godot's
// compatibility renderer's tier. driver is "opengl" (OpenGL 3.3, then OpenGL ES 3.0 if that
// doesn't start) or "opengl_es". A window must be made for OpenGL (SDL_WINDOW_OPENGL); with none,
// the device draws into render targets through a hidden window of its own.
[[nodiscard]] Result<std::unique_ptr<RenderDevice>> create_gl_device(const DeviceOptions& options);

// Whether the video driver has OpenGL at all: SDL loads its GL library. A driver with none, such
// as SDL's dummy one, can't make a window for OpenGL. Starts SDL's video for the check if nothing
// has, and leaves it as it was.
[[nodiscard]] bool gl_library_loads();

} // namespace slash::render::detail
