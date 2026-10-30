// The graphics extensions OpenXR sessions draw with, ported from Godot's modules/openxr/extensions
// (OpenXRVulkanExtension, OpenXRD3D12Extension): each function names the one it follows.
#include "openxr_graphics.hpp"

#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#include <d3d12.h>
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#elif defined(__linux__)
// EGL's types, from the headers SDL carries, without X11's; and the few of Xlib and GLX the Xlib
// binding names, as handles of their size, so building needs neither's headers.
#define EGL_NO_X11
#include <EGL/egl.h>
struct XlibDisplay;
struct GlxConfig;
struct GlxContext;
using Display = XlibDisplay;
using GLXDrawable = unsigned long; // NOLINT(google-runtime-int): Xlib's XID
using GLXFBConfig = GlxConfig*;
using GLXContext = GlxContext*;
#define XR_USE_PLATFORM_EGL
#define XR_USE_PLATFORM_XLIB
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN
#include <cstdint>
#include <openxr/openxr_platform.h>
#include <string>

namespace slash::xr::detail {
namespace {

template <typename T>
T made(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

// An instance function of the extension, or null.
template <typename F>
F instance_call(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function))) {
        return nullptr;
    }
    return reinterpret_cast<F>(function);
}

Error failed(const std::string& what, XrResult result) {
    return make_error(ErrorCode::PlatformFailure,
                      "OpenXR: " + what + " [XrResult " + std::to_string(result) + "]");
}

class Vulkan final : public OpenXRGraphics {
public:
    [[nodiscard]] std::string_view extension() const override {
        return XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    }
    [[nodiscard]] std::string_view driver() const override { return "vulkan"; }

    // OpenXRVulkanExtension::check_graphics_api_support, and the VulkanHooks it installs.
    Result<void> prepare(XrInstance instance, XrSystemId system,
                         render::XrDeviceHooks& hooks) override {
        instance_ = instance;
        system_ = system;
        const auto requirements = instance_call<PFN_xrGetVulkanGraphicsRequirements2KHR>(
            instance, "xrGetVulkanGraphicsRequirements2KHR");
        create_instance_ =
            instance_call<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
        create_device_ =
            instance_call<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
        graphics_device_ = instance_call<PFN_xrGetVulkanGraphicsDevice2KHR>(
            instance, "xrGetVulkanGraphicsDevice2KHR");
        if (requirements == nullptr || create_instance_ == nullptr || create_device_ == nullptr ||
            graphics_device_ == nullptr) {
            return make_error(ErrorCode::PlatformFailure,
                              "OpenXR: the runtime lacks XR_KHR_vulkan_enable2's functions");
        }
        auto needed =
            made<XrGraphicsRequirementsVulkan2KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
        if (const XrResult result = requirements(instance, system, &needed); XR_FAILED(result)) {
            return failed("the runtime's Vulkan requirements couldn't be read", result);
        }
        // SDL makes a Vulkan 1.0 instance at least, which every runtime takes.
        hooks.create_vulkan_instance = [this](const void* info, void* procs, void** made_instance) {
            auto create =
                made<XrVulkanInstanceCreateInfoKHR>(XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
            create.systemId = system_;
            create.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(procs);
            create.vulkanCreateInfo = static_cast<const VkInstanceCreateInfo*>(info);
            VkResult vulkan = VK_SUCCESS;
            const XrResult result = create_instance_(
                instance_, &create, reinterpret_cast<VkInstance*>(made_instance), &vulkan);
            return XR_FAILED(result) ? static_cast<int>(VK_ERROR_INITIALIZATION_FAILED)
                                     : static_cast<int>(vulkan);
        };
        hooks.vulkan_physical_device = [this](void* vulkan_instance, void** physical) {
            auto get =
                made<XrVulkanGraphicsDeviceGetInfoKHR>(XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
            get.systemId = system_;
            get.vulkanInstance = static_cast<VkInstance>(vulkan_instance);
            const XrResult result =
                graphics_device_(instance_, &get, reinterpret_cast<VkPhysicalDevice*>(physical));
            return XR_FAILED(result) ? static_cast<int>(VK_ERROR_INITIALIZATION_FAILED)
                                     : static_cast<int>(VK_SUCCESS);
        };
        hooks.create_vulkan_device = [this](void* physical, const void* info, void* procs,
                                            void** device) {
            auto create = made<XrVulkanDeviceCreateInfoKHR>(XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
            create.systemId = system_;
            create.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(procs);
            create.vulkanPhysicalDevice = static_cast<VkPhysicalDevice>(physical);
            create.vulkanCreateInfo = static_cast<const VkDeviceCreateInfo*>(info);
            VkResult vulkan = VK_SUCCESS;
            const XrResult result =
                create_device_(instance_, &create, reinterpret_cast<VkDevice*>(device), &vulkan);
            return XR_FAILED(result) ? static_cast<int>(VK_ERROR_INITIALIZATION_FAILED)
                                     : static_cast<int>(vulkan);
        };
        return {};
    }

    // OpenXRVulkanExtension::set_session_create_and_get_next_pointer.
    Result<const void*> binding(const render::NativeDevice& device,
                                const std::set<std::string, std::less<>>& /*enabled*/) override {
        binding_ = made<XrGraphicsBindingVulkan2KHR>(XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
        binding_.instance = static_cast<VkInstance>(device.instance);
        binding_.physicalDevice = static_cast<VkPhysicalDevice>(device.physical_device);
        binding_.device = static_cast<VkDevice>(device.device);
        binding_.queueFamilyIndex = device.queue_family;
        binding_.queueIndex = 0;
        return static_cast<const void*>(&binding_);
    }

    [[nodiscard]] std::vector<std::int64_t> formats() const override {
        return {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    }

    // OpenXRVulkanExtension::get_swapchain_image_data.
    Result<std::vector<void*>> images(XrSwapchain swapchain) override {
        std::uint32_t count = 0;
        if (const XrResult result = xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
            XR_FAILED(result)) {
            return failed("failed to get swapchain image count", result);
        }
        std::vector<XrSwapchainImageVulkan2KHR> images(
            count, made<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
        if (const XrResult result = xrEnumerateSwapchainImages(
                swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            XR_FAILED(result)) {
            return failed("failed to get swapchain images", result);
        }
        std::vector<void*> out;
        out.reserve(images.size());
        for (const XrSwapchainImageVulkan2KHR& image : images) {
            // A pointer on 64-bit machines, a 64-bit number on others.
            out.push_back(reinterpret_cast<void*>(image.image)); // NOLINT(*-reinterpret-cast)
        }
        return out;
    }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR graphics_device_ = nullptr;
    XrGraphicsBindingVulkan2KHR binding_ =
        made<XrGraphicsBindingVulkan2KHR>(XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
};

#if defined(_WIN32)
class D3D12 final : public OpenXRGraphics {
public:
    [[nodiscard]] std::string_view extension() const override {
        return XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
    }
    [[nodiscard]] std::string_view driver() const override { return "d3d12"; }

    // OpenXRD3D12Extension::check_graphics_api_support: the adapter the headset is on.
    Result<void> prepare(XrInstance instance, XrSystemId system,
                         render::XrDeviceHooks& hooks) override {
        const auto requirements = instance_call<PFN_xrGetD3D12GraphicsRequirementsKHR>(
            instance, "xrGetD3D12GraphicsRequirementsKHR");
        if (requirements == nullptr) {
            return make_error(ErrorCode::PlatformFailure,
                              "OpenXR: the runtime lacks XR_KHR_D3D12_enable's functions");
        }
        auto needed = made<XrGraphicsRequirementsD3D12KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR);
        if (const XrResult result = requirements(instance, system, &needed); XR_FAILED(result)) {
            return failed("the runtime's Direct3D 12 requirements couldn't be read", result);
        }
        // SDL makes its device at feature level 11_0, which every runtime takes.
        hooks.d3d12_adapter = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(needed.adapterLuid.HighPart))
             << 32U) |
            needed.adapterLuid.LowPart);
        if (hooks.d3d12_adapter == 0) {
            hooks.d3d12_adapter = 1; // a LUID of 0 would ask for SDL's choice
        }
        return {};
    }

    Result<const void*> binding(const render::NativeDevice& device,
                                const std::set<std::string, std::less<>>& /*enabled*/) override {
        binding_ = made<XrGraphicsBindingD3D12KHR>(XR_TYPE_GRAPHICS_BINDING_D3D12_KHR);
        binding_.device = static_cast<ID3D12Device*>(device.device);
        binding_.queue = static_cast<ID3D12CommandQueue*>(device.queue);
        return static_cast<const void*>(&binding_);
    }

    [[nodiscard]] std::vector<std::int64_t> formats() const override {
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    }

    // OpenXRD3D12Extension::get_swapchain_image_data.
    Result<std::vector<void*>> images(XrSwapchain swapchain) override {
        std::uint32_t count = 0;
        if (const XrResult result = xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
            XR_FAILED(result)) {
            return failed("failed to get swapchain image count", result);
        }
        std::vector<XrSwapchainImageD3D12KHR> images(
            count, made<XrSwapchainImageD3D12KHR>(XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR));
        if (const XrResult result = xrEnumerateSwapchainImages(
                swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            XR_FAILED(result)) {
            return failed("failed to get swapchain images", result);
        }
        std::vector<void*> out;
        out.reserve(images.size());
        for (const XrSwapchainImageD3D12KHR& image : images) {
            out.push_back(image.texture);
        }
        return out;
    }

private:
    XrGraphicsBindingD3D12KHR binding_ =
        made<XrGraphicsBindingD3D12KHR>(XR_TYPE_GRAPHICS_BINDING_D3D12_KHR);
};
#endif

#if defined(_WIN32) || defined(__linux__)
class OpenGL final : public OpenXRGraphics {
public:
    [[nodiscard]] std::string_view extension() const override {
        return XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    }
    [[nodiscard]] std::vector<std::string_view> optional_extensions() const override {
#if defined(__linux__)
        return {XR_MNDX_EGL_ENABLE_EXTENSION_NAME};
#else
        return {};
#endif
    }
    [[nodiscard]] std::string_view driver() const override { return "opengl"; }

    // OpenXROpenGLExtension::check_graphics_api_support: the runtime's OpenGL versions, which
    // OpenXR asks for before a session. The device's context is OpenGL 3.3 core or later.
    Result<void> prepare(XrInstance instance, XrSystemId system,
                         render::XrDeviceHooks& /*hooks*/) override {
        const auto requirements = instance_call<PFN_xrGetOpenGLGraphicsRequirementsKHR>(
            instance, "xrGetOpenGLGraphicsRequirementsKHR");
        if (requirements == nullptr) {
            return make_error(ErrorCode::PlatformFailure,
                              "OpenXR: the runtime lacks XR_KHR_opengl_enable's functions");
        }
        auto needed =
            made<XrGraphicsRequirementsOpenGLKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR);
        if (const XrResult result = requirements(instance, system, &needed); XR_FAILED(result)) {
            return failed("the runtime's OpenGL requirements couldn't be read", result);
        }
        if (needed.minApiVersionSupported > XR_MAKE_VERSION(4, 6, 0)) {
            return make_error(ErrorCode::PlatformFailure,
                              "OpenXR: the runtime needs an OpenGL newer than 4.6");
        }
        return {};
    }

    // OpenXROpenGLExtension::set_session_create_and_get_next_pointer, by the window system the
    // context is on.
    Result<const void*> binding(const render::NativeDevice& device,
                                const std::set<std::string, std::less<>>& enabled) override {
        using System = render::NativeDevice::GlSystem;
#if defined(_WIN32)
        if (device.gl == System::Wgl) {
            win32_ =
                made<XrGraphicsBindingOpenGLWin32KHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR);
            win32_.hDC = static_cast<HDC>(device.instance);
            win32_.hGLRC = static_cast<HGLRC>(device.device);
            return static_cast<const void*>(&win32_);
        }
#else
        if (device.gl == System::Egl) {
            if (!enabled.contains(XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
                return make_error(ErrorCode::PlatformFailure,
                                  "OpenXR: the context is EGL's, and the runtime lacks "
                                  "XR_MNDX_egl_enable");
            }
            egl_ = made<XrGraphicsBindingEGLMNDX>(XR_TYPE_GRAPHICS_BINDING_EGL_MNDX);
            egl_.getProcAddress =
                reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(device.get_proc_address);
            egl_.display = device.instance;
            egl_.config = device.physical_device;
            egl_.context = device.device;
            return static_cast<const void*>(&egl_);
        }
        if (device.gl == System::Glx) {
            xlib_ = made<XrGraphicsBindingOpenGLXlibKHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR);
            xlib_.xDisplay = static_cast<Display*>(device.instance);
            xlib_.visualid = device.visual;
            xlib_.glxFBConfig = static_cast<GLXFBConfig>(device.physical_device);
            xlib_.glxDrawable = static_cast<GLXDrawable>(device.drawable);
            xlib_.glxContext = static_cast<GLXContext>(device.device);
            return static_cast<const void*>(&xlib_);
        }
#endif
        return make_error(ErrorCode::PlatformFailure,
                          "OpenXR: the OpenGL context is on a window system OpenXR can't bind");
    }

    [[nodiscard]] std::vector<std::int64_t> formats() const override {
        constexpr std::int64_t kSrgb8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
        constexpr std::int64_t kRgba8 = 0x8058;       // GL_RGBA8
        return {kSrgb8Alpha8, kRgba8};
    }

    // OpenXROpenGLExtension::get_swapchain_image_data.
    Result<std::vector<void*>> images(XrSwapchain swapchain) override {
        std::uint32_t count = 0;
        if (const XrResult result = xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
            XR_FAILED(result)) {
            return failed("failed to get swapchain image count", result);
        }
        std::vector<XrSwapchainImageOpenGLKHR> images(
            count, made<XrSwapchainImageOpenGLKHR>(XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR));
        if (const XrResult result = xrEnumerateSwapchainImages(
                swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            XR_FAILED(result)) {
            return failed("failed to get swapchain images", result);
        }
        std::vector<void*> out;
        out.reserve(images.size());
        for (const XrSwapchainImageOpenGLKHR& image : images) {
            // A texture's name, carried as native images are.
            out.push_back(reinterpret_cast<void*>( // NOLINT(performance-no-int-to-ptr)
                std::uintptr_t{image.image}));
        }
        return out;
    }

private:
#if defined(_WIN32)
    XrGraphicsBindingOpenGLWin32KHR win32_ =
        made<XrGraphicsBindingOpenGLWin32KHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR);
#else
    XrGraphicsBindingEGLMNDX egl_ =
        made<XrGraphicsBindingEGLMNDX>(XR_TYPE_GRAPHICS_BINDING_EGL_MNDX);
    XrGraphicsBindingOpenGLXlibKHR xlib_ =
        made<XrGraphicsBindingOpenGLXlibKHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR);
#endif
};
#endif

} // namespace

std::unique_ptr<OpenXRGraphics> make_vulkan_graphics() {
    return std::make_unique<Vulkan>();
}

std::unique_ptr<OpenXRGraphics> make_opengl_graphics() {
#if defined(_WIN32) || defined(__linux__)
    return std::make_unique<OpenGL>();
#else
    return nullptr;
#endif
}

std::unique_ptr<OpenXRGraphics> make_d3d12_graphics() {
#if defined(_WIN32)
    return std::make_unique<D3D12>();
#else
    return nullptr;
#endif
}

} // namespace slash::xr::detail
