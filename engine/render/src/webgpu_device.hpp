#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <memory>

// Only web builds have this device (webgpu_device.cpp).
namespace slash::render::detail {

// Whether the page has a WebGPU device for the engine: web/webgpu_device.js asks the browser for
// one before main runs, since WebGPU hands devices out only asynchronously.
[[nodiscard]] bool webgpu_available();

// A device over the browser's WebGPU, through webgpu.h (Dawn's emdawnwebgpu). Fails where the
// browser gave no device. With a window, it draws to the window's canvas.
[[nodiscard]] Result<std::unique_ptr<RenderDevice>>
create_webgpu_device(const DeviceOptions& options);

} // namespace slash::render::detail
