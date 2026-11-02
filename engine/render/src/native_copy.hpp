#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

namespace slash::render::detail {

// Records a copy of source, an SDL render target resting where shaders sample it, into target, a
// native image resting where it is drawn to (an OpenXR swapchain's), into commands, the
// recording VkCommandBuffer or ID3D12GraphicsCommandList; both are left as they were.
[[nodiscard]] Result<void> copy_native_vulkan(const NativeDevice& device, void* commands,
                                              const NativeTexture& source,
                                              const NativeTexture& target);
[[nodiscard]] Result<void> copy_native_d3d12(void* commands, const NativeTexture& source,
                                             const NativeTexture& target);

} // namespace slash::render::detail
