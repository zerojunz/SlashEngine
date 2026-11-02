#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <openxr/openxr.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace slash::xr::detail {

// A graphics API's part in an OpenXR session, as Godot's OpenXRGraphicsExtensionWrapper: the
// runtime's say in making the render device, the session's graphics binding and the swapchains'
// images.
class OpenXRGraphics {
public:
    OpenXRGraphics() = default;
    OpenXRGraphics(const OpenXRGraphics&) = delete;
    OpenXRGraphics& operator=(const OpenXRGraphics&) = delete;
    OpenXRGraphics(OpenXRGraphics&&) = delete;
    OpenXRGraphics& operator=(OpenXRGraphics&&) = delete;
    virtual ~OpenXRGraphics() = default;

    // The instance extension it needs, the ones it uses where the runtime has them, and the
    // render driver it draws with.
    [[nodiscard]] virtual std::string_view extension() const = 0;
    [[nodiscard]] virtual std::vector<std::string_view> optional_extensions() const { return {}; }
    [[nodiscard]] virtual std::string_view driver() const = 0;
    // Once the instance and system exist: reads the runtime's graphics requirements, as OpenXR
    // asks before a session, and fills in what making the render device needs.
    virtual Result<void> prepare(XrInstance instance, XrSystemId system,
                                 render::XrDeviceHooks& hooks) = 0;
    // xrCreateSession's graphics binding for device's native objects, given the instance's
    // enabled extensions; it lives as long as this.
    [[nodiscard]] virtual Result<const void*>
    binding(const render::NativeDevice& device,
            const std::set<std::string, std::less<>>& enabled) = 0;
    // The swapchain formats it can copy into, the one to prefer first: 8-bit RGBA, sRGB first.
    [[nodiscard]] virtual std::vector<std::int64_t> formats() const = 0;
    // A swapchain's images, as native images (VkImage, ID3D12Resource, a GL texture's name).
    virtual Result<std::vector<void*>> images(XrSwapchain swapchain) = 0;
};

// XR_KHR_vulkan_enable2 (OpenXRVulkanExtension).
[[nodiscard]] std::unique_ptr<OpenXRGraphics> make_vulkan_graphics();
// XR_KHR_D3D12_enable (OpenXRD3D12Extension); null but on Windows.
[[nodiscard]] std::unique_ptr<OpenXRGraphics> make_d3d12_graphics();
// XR_KHR_opengl_enable, with XR_MNDX_egl_enable for EGL contexts (OpenXROpenGLExtension); null
// but on Windows and Linux.
[[nodiscard]] std::unique_ptr<OpenXRGraphics> make_opengl_graphics();

} // namespace slash::xr::detail
