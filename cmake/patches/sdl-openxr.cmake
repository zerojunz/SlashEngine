# What drawing into an OpenXR headset (engine/xr/src/openxr_graphics.cpp) needs of SDL3: the
# runtime picks the GPU and, on Vulkan, makes the instance and the device, as Godot's
# OpenXRVulkanExtension does through its VulkanHooks and its OpenXRD3D12Extension through the
# adapter's LUID. SDL 3.6 makes the OpenXR instance itself (SDL_openxr.h); until SlashEngine moves
# to it, these hooks keep the instance the engine's.
#
# On Vulkan, the "SlashEngine.gpu.vulkan.xr_hooks" pointer property is an SDL_SlashVulkanXrHooks:
# SDL makes its instance with createInstance (xrCreateVulkanInstanceKHR), takes the physical
# device choosePhysicalDevice names (xrGetVulkanGraphicsDevice2KHR) when it is suitable, and makes
# its device with createDevice (xrCreateVulkanDeviceKHR). On Direct3D 12, the
# "SlashEngine.gpu.d3d12.adapter_luid" number property, when not 0, is the LUID of the adapter to
# use (xrGetD3D12GraphicsRequirementsKHR's), its HighPart in the upper 32 bits.
# slash_add_dependency(SDL3) runs it in SDL's source folder after the other SDL patches.

function(patch_file file marker)
    file(READ ${file} text)
    if(text MATCHES "${marker}")
        return()
    endif()
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 2 ${last} 2)
        math(EXPR j "${i} + 1")
        set(original "${ARGV${i}}")
        set(patched "${ARGV${j}}")
        string(FIND "${text}" "${original}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${file} no longer has the code the OpenXR patch changes:\n${original}")
        endif()
        string(REPLACE "${original}" "${patched}" text "${text}")
    endforeach()
    file(WRITE ${file} "${text}")
endfunction()

patch_file(src/gpu/vulkan/SDL_gpu_vulkan.c "SDL_SlashVulkanXrHooks"
[=[
struct VulkanRenderer
{
]=] [=[
// SlashEngine (cmake/patches/sdl-openxr.cmake): an OpenXR runtime's part in making the device.
typedef struct SDL_SlashVulkanXrHooks
{
    void *userdata;
    VkResult (*createInstance)(void *userdata, const VkInstanceCreateInfo *createInfo,
                               PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance *instance);
    VkResult (*choosePhysicalDevice)(void *userdata, VkInstance instance,
                                     VkPhysicalDevice *physicalDevice);
    VkResult (*createDevice)(void *userdata, VkPhysicalDevice physicalDevice,
                             const VkDeviceCreateInfo *createInfo,
                             PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkDevice *device);
} SDL_SlashVulkanXrHooks;

struct VulkanRenderer
{
]=]
[=[
    bool requireHardwareAcceleration;
]=] [=[
    bool requireHardwareAcceleration;
    const SDL_SlashVulkanXrHooks *slashXrHooks; // SlashEngine: an OpenXR runtime's, or NULL
]=]
[=[
    renderer->requireHardwareAcceleration = SDL_GetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, false);
]=] [=[
    renderer->requireHardwareAcceleration = SDL_GetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, false);
    renderer->slashXrHooks = (const SDL_SlashVulkanXrHooks *)SDL_GetPointerProperty(props, "SlashEngine.gpu.vulkan.xr_hooks", NULL);
]=]
[=[
    vulkanResult = vkCreateInstance(&createInfo, NULL, &renderer->instance);
]=] [=[
    if (renderer->slashXrHooks) {
        // SlashEngine: the OpenXR runtime adds what it needs.
        vulkanResult = renderer->slashXrHooks->createInstance(renderer->slashXrHooks->userdata, &createInfo, vkGetInstanceProcAddr, &renderer->instance);
    } else {
        vulkanResult = vkCreateInstance(&createInfo, NULL, &renderer->instance);
    }
]=]
[=[
    // Any suitable device will do, but we'd like the best
    suitableIndex = -1;
]=] [=[
    // SlashEngine: an OpenXR runtime draws with the GPU it names, or none.
    VkPhysicalDevice slashXrDevice = VK_NULL_HANDLE;
    if (renderer->slashXrHooks &&
        renderer->slashXrHooks->choosePhysicalDevice(renderer->slashXrHooks->userdata, renderer->instance, &slashXrDevice) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "The OpenXR runtime named no GPU");
        SDL_stack_free(physicalDevices);
        SDL_stack_free(physicalDeviceExtensions);
        return 0;
    }

    // Any suitable device will do, but we'd like the best
    suitableIndex = -1;
]=]
[=[
        Uint32 queueFamilyIndex;
        Uint64 deviceRank;

        if (!VULKAN_INTERNAL_IsDeviceSuitable(
]=] [=[
        Uint32 queueFamilyIndex;
        Uint64 deviceRank;

        if (slashXrDevice != VK_NULL_HANDLE && physicalDevices[i] != slashXrDevice) {
            continue;
        }

        if (!VULKAN_INTERNAL_IsDeviceSuitable(
]=]
[=[
    vulkanResult = renderer->vkCreateDevice(
        renderer->physicalDevice,
        &deviceCreateInfo,
        NULL,
        &renderer->logicalDevice);
]=] [=[
    if (renderer->slashXrHooks) {
        // SlashEngine: the OpenXR runtime adds what it needs.
        vulkanResult = renderer->slashXrHooks->createDevice(
            renderer->slashXrHooks->userdata,
            renderer->physicalDevice,
            &deviceCreateInfo,
            vkGetInstanceProcAddr,
            &renderer->logicalDevice);
    } else {
        vulkanResult = renderer->vkCreateDevice(
            renderer->physicalDevice,
            &deviceCreateInfo,
            NULL,
            &renderer->logicalDevice);
    }
]=])

patch_file(src/gpu/d3d12/SDL_gpu_d3d12.c "SlashEngine.gpu.d3d12.adapter_luid"
[=[
    // Select the appropriate device for rendering
    res = IDXGIFactory4_QueryInterface(
        renderer->factory,
        D3D_GUID(D3D_IID_IDXGIFactory6),
        (void **)&factory6);
    if (SUCCEEDED(res)) {
]=] [=[
    // Select the appropriate device for rendering
    // SlashEngine (cmake/patches/sdl-openxr.cmake): an OpenXR runtime's adapter, by its LUID.
    const Sint64 slashLuid = SDL_GetNumberProperty(props, "SlashEngine.gpu.d3d12.adapter_luid", 0);
    if (slashLuid != 0) {
        LUID luid;
        luid.LowPart = (DWORD)((Uint64)slashLuid & 0xFFFFFFFFu);
        luid.HighPart = (LONG)(slashLuid >> 32);
        res = IDXGIFactory4_EnumAdapterByLuid(
            renderer->factory,
            luid,
            D3D_GUID(D3D_IID_IDXGIAdapter1),
            (void **)&renderer->adapter);
    } else if (SUCCEEDED(res = IDXGIFactory4_QueryInterface(
        renderer->factory,
        D3D_GUID(D3D_IID_IDXGIFactory6),
        (void **)&factory6))) {
]=])
