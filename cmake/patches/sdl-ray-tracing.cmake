# What ray tracing on the GPU device (engine/render/src/vulkan_rays.cpp) needs of SDL3 beyond the
# native objects cmake/patches/sdl-native-handles.cmake hands over.
#
# SDL_SlashGPUNativeBuffer hands over the buffer an SDL buffer holds now (VkBuffer on Vulkan, null
# elsewhere for now), unretained, which acceleration structures are built from.
#
# And SDL_GPUVulkanOptions' feature_list turns on features of device extensions, which SDL3's
# Vulkan device drops: it folds the list's Vulkan 1.1 to 1.3 feature structures into its own and
# ignores the rest, so VK_KHR_acceleration_structure's and VK_KHR_ray_query's features stay off
# even when their extensions are asked for. With the patch, the first structure in the list that
# SDL doesn't fold, and every one after it, is chained after SDL's own when it makes the device
# (Vulkan 1.2 or later), so a list puts the core structures first and the extensions' after them.
# slash_add_dependency(SDL3) runs it in SDL's source folder after the other SDL patches.

set(file src/gpu/vulkan/SDL_gpu_vulkan.c)
file(READ ${file} text)
if(text MATCHES "slashExtensionFeatures")
    return()
endif()

function(replace_once original patched)
    string(FIND "${text}" "${original}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${file} no longer has the code the extension features patch changes:\n${original}")
    endif()
    string(REPLACE "${original}" "${patched}" changed "${text}")
    set(text "${changed}" PARENT_SCOPE)
endfunction()

replace_once([=[
    bool usesCustomVulkanOptions;
]=] [=[
    bool usesCustomVulkanOptions;
    void *slashExtensionFeatures; // SlashEngine: the extensions' structures, chained after SDL's
]=])

replace_once([=[
                    while (nextStructure) {
                        VULKAN_INTERNAL_TryAddDeviceFeatures_Vulkan_12_Or_Later(vk10Features,
                                                                                vk11Features,
                                                                                vk12Features,
                                                                                vk13Features,
                                                                                features->desiredApiVersion,
                                                                                nextStructure);
                        nextStructure = nextStructure->pNext;
                    }
]=] [=[
                    while (nextStructure) {
                        if (!VULKAN_INTERNAL_TryAddDeviceFeatures_Vulkan_12_Or_Later(vk10Features,
                                                                                     vk11Features,
                                                                                     vk12Features,
                                                                                     vk13Features,
                                                                                     features->desiredApiVersion,
                                                                                     nextStructure)) {
                            // SlashEngine: an extension's, with the rest of the list after it.
                            features->slashExtensionFeatures = nextStructure;
                            break;
                        }
                        nextStructure = nextStructure->pNext;
                    }
]=])

replace_once([=[
            features->desiredVulkan12DeviceFeatures.pNext = minor > 2 ? &features->desiredVulkan13DeviceFeatures : NULL;
            features->desiredVulkan13DeviceFeatures.pNext = NULL;
]=] [=[
            features->desiredVulkan12DeviceFeatures.pNext = minor > 2 ? &features->desiredVulkan13DeviceFeatures : features->slashExtensionFeatures;
            features->desiredVulkan13DeviceFeatures.pNext = features->slashExtensionFeatures;
]=])

string(FIND "${text}" "#endif // SDL_GPU_VULKAN" at REVERSE)
if(at EQUAL -1)
    message(FATAL_ERROR "${file} no longer ends with #endif // SDL_GPU_VULKAN")
endif()
string(SUBSTRING "${text}" 0 ${at} before)
string(SUBSTRING "${text}" ${at} -1 after)
set(buffer_call [=[
// SlashEngine (cmake/patches/sdl-ray-tracing.cmake): the buffer an SDL buffer holds now.
void *SDL_SlashVulkanNativeBuffer(SDL_GPUBuffer *buffer);

void *SDL_SlashVulkanNativeBuffer(SDL_GPUBuffer *buffer)
{
    return (void *)(uintptr_t)((VulkanBufferContainer *)buffer)->activeBuffer->buffer;
}

]=])
set(text "${before}${buffer_call}${after}")
file(WRITE ${file} "${text}")

set(file src/gpu/SDL_gpu.c)
file(READ ${file} text)
if(NOT text MATCHES "SDL_SlashGPUNativeBuffer")
    file(APPEND ${file} [=[

// SlashEngine (cmake/patches/sdl-ray-tracing.cmake): the native buffer behind an SDL buffer.
#ifdef SDL_GPU_VULKAN
void *SDL_SlashVulkanNativeBuffer(SDL_GPUBuffer *buffer);
#endif
void *SDL_SlashGPUNativeBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *buffer);

void *SDL_SlashGPUNativeBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *buffer)
{
    if (device == NULL || buffer == NULL) {
        return NULL;
    }
#ifdef SDL_GPU_VULKAN
    if (SDL_strcmp(device->backend, "vulkan") == 0) {
        return SDL_SlashVulkanNativeBuffer(buffer);
    }
#endif
    return NULL;
}
]=])
endif()
