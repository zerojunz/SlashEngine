#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <memory>

namespace slash::render::detail {

// A device over SDL's GPU API. driver is "auto", "vulkan", "metal" or "d3d12".
[[nodiscard]] Result<std::unique_ptr<RenderDevice>> create_gpu_device(const DeviceOptions& options);

} // namespace slash::render::detail
