#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <memory>

namespace slash::render::detail {

// A device over OpenGL ES 3.0, which is WebGL 2 on the web: Godot's compatibility renderer's
// tier. A window must be made for OpenGL (SDL_WINDOW_OPENGL); with none, the device draws into
// render targets through a hidden window of its own.
[[nodiscard]] Result<std::unique_ptr<RenderDevice>> create_gl_device(const DeviceOptions& options);

} // namespace slash::render::detail
