# Hands SlashEngine the native objects behind SDL3's GPU API, which doesn't show them, as Godot's
# RenderingDevice::get_driver_resource hands over its own: the device's (VkInstance,
# VkPhysicalDevice, VkDevice, VkQueue and its family on Vulkan; ID3D12Device and
# ID3D12CommandQueue on Direct3D 12; MTLDevice and MTLCommandQueue on Metal), the image an SDL
# texture draws into now (with its full VkImageView and VkFormat on Vulkan, DXGI_FORMAT on
# Direct3D 12, MTLPixelFormat on Metal) and an SDL command buffer's (VkCommandBuffer,
# ID3D12GraphicsCommandList or MTLCommandBuffer), each unretained. Apple's MetalFX scalers
# (engine/render/src/metalfx.mm) and a game's temporal upscalers (render::TemporalUpscaler) take
# them. SDL_SlashGPURestoreState binds SDL's Direct3D 12 descriptor heaps again after other code
# set its own. slash_add_dependency(SDL3) runs it in SDL's source folder after the Vulkan patch.

function(slash_patch file marker end checks calls)
    file(READ ${file} text)
    if(text MATCHES "${marker}")
        return()
    endif()
    string(FIND "${text}" "${end}" at REVERSE)
    if(at EQUAL -1)
        message(FATAL_ERROR "${file} no longer ends with ${end}")
    endif()
    foreach(check IN LISTS checks)
        string(FIND "${text}" "${check}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${file} no longer has ${check}, which the patch hands over")
        endif()
    endforeach()
    string(SUBSTRING "${text}" 0 ${at} before)
    string(SUBSTRING "${text}" ${at} -1 after)
    file(WRITE ${file} "${before}${calls}${after}")
endfunction()

slash_patch(src/gpu/metal/SDL_gpu_metal.m SDL_SlashMetalNativeDevice "#endif // SDL_GPU_METAL"
    "MetalTexture *activeTexture;id<MTLCommandBuffer> handle;id<MTLDevice> device;id<MTLCommandQueue> queue;"
    [=[
// SlashEngine (cmake/patches/sdl-native-handles.cmake): Metal's objects.
bool SDL_SlashMetalNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                void **logicalDevice, void **queue, Uint32 *queueFamily);
void *SDL_SlashMetalNativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format);
void *SDL_SlashMetalNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);

bool SDL_SlashMetalNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                void **logicalDevice, void **queue, Uint32 *queueFamily)
{
    MetalRenderer *renderer = (MetalRenderer *)device->driverData;
    *instance = NULL;
    *physicalDevice = NULL;
    *logicalDevice = (__bridge void *)renderer->device;
    *queue = (__bridge void *)renderer->queue;
    *queueFamily = 0;
    return true;
}

void *SDL_SlashMetalNativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format)
{
    id<MTLTexture> handle = ((MetalTextureContainer *)texture)->activeTexture->handle;
    *view = NULL;
    *format = (Uint32)handle.pixelFormat;
    return (__bridge void *)handle;
}

void *SDL_SlashMetalNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    return (__bridge void *)((MetalCommandBuffer *)commandBuffer)->handle;
}

]=])

slash_patch(src/gpu/vulkan/SDL_gpu_vulkan.c SDL_SlashVulkanNativeDevice "#endif // SDL_GPU_VULKAN"
    "VulkanTexture *activeTexture;VkImageView fullView;VkCommandBuffer commandBuffer;Uint32 queueFamilyIndex;VkQueue unifiedQueue;static VkFormat SDLToVK_TextureFormat"
    [=[
// SlashEngine (cmake/patches/sdl-native-handles.cmake): Vulkan's objects.
bool SDL_SlashVulkanNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                 void **logicalDevice, void **queue, Uint32 *queueFamily);
void *SDL_SlashVulkanNativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format);
void *SDL_SlashVulkanNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);

bool SDL_SlashVulkanNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                 void **logicalDevice, void **queue, Uint32 *queueFamily)
{
    VulkanRenderer *renderer = (VulkanRenderer *)device->driverData;
    *instance = (void *)renderer->instance;
    *physicalDevice = (void *)renderer->physicalDevice;
    *logicalDevice = (void *)renderer->logicalDevice;
    *queue = (void *)renderer->unifiedQueue;
    *queueFamily = renderer->queueFamilyIndex;
    return true;
}

void *SDL_SlashVulkanNativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format)
{
    VulkanTextureContainer *container = (VulkanTextureContainer *)texture;
    *view = (void *)(uintptr_t)container->activeTexture->fullView;
    *format = (Uint32)SDLToVK_TextureFormat[container->header.info.format];
    return (void *)(uintptr_t)container->activeTexture->image;
}

void *SDL_SlashVulkanNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    return (void *)((VulkanCommandBuffer *)commandBuffer)->commandBuffer;
}

]=])

slash_patch(src/gpu/d3d12/SDL_gpu_d3d12.c SDL_SlashD3D12NativeDevice "#endif // SDL_GPU_D3D12"
    "D3D12Texture *activeTexture;ID3D12Resource *resource;ID3D12GraphicsCommandList *graphicsCommandList;ID3D12CommandQueue *commandQueue;D3D12DescriptorHeap *gpuDescriptorHeaps;static DXGI_FORMAT SDLToD3D12_TextureFormat"
    [=[
// SlashEngine (cmake/patches/sdl-native-handles.cmake): Direct3D 12's objects.
bool SDL_SlashD3D12NativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                void **logicalDevice, void **queue, Uint32 *queueFamily);
void *SDL_SlashD3D12NativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format);
void *SDL_SlashD3D12NativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);
void SDL_SlashD3D12RestoreState(SDL_GPUCommandBuffer *commandBuffer);

bool SDL_SlashD3D12NativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                                void **logicalDevice, void **queue, Uint32 *queueFamily)
{
    D3D12Renderer *renderer = (D3D12Renderer *)device->driverData;
    *instance = NULL;
    *physicalDevice = NULL;
    *logicalDevice = (void *)renderer->device;
    *queue = (void *)renderer->commandQueue;
    *queueFamily = 0;
    return true;
}

void *SDL_SlashD3D12NativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format)
{
    D3D12TextureContainer *container = (D3D12TextureContainer *)texture;
    *view = NULL;
    *format = (Uint32)SDLToD3D12_TextureFormat[container->header.info.format];
    return (void *)container->activeTexture->resource;
}

void *SDL_SlashD3D12NativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    return (void *)((D3D12CommandBuffer *)commandBuffer)->graphicsCommandList;
}

void SDL_SlashD3D12RestoreState(SDL_GPUCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    ID3D12DescriptorHeap *heaps[2];
    if (d3d12CommandBuffer->gpuDescriptorHeaps[0] == NULL ||
        d3d12CommandBuffer->gpuDescriptorHeaps[1] == NULL) {
        return;
    }
    heaps[0] = d3d12CommandBuffer->gpuDescriptorHeaps[0]->handle;
    heaps[1] = d3d12CommandBuffer->gpuDescriptorHeaps[1]->handle;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(d3d12CommandBuffer->graphicsCommandList, 2, heaps);
}

]=])

# What SlashEngine calls: each by the device's backend, so a platform without a backend links.
file(READ src/gpu/SDL_gpu.c text)
if(NOT text MATCHES "SDL_SlashGPUNativeDevice")
    string(FIND "${text}" "#include \"SDL_sysgpu.h\"" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "src/gpu/SDL_gpu.c no longer includes SDL_sysgpu.h")
    endif()
    file(APPEND src/gpu/SDL_gpu.c [=[

// SlashEngine (cmake/patches/sdl-native-handles.cmake): the native objects behind the GPU API.
#define SLASH_NATIVE_CALLS(name)                                                                   \
    bool SDL_Slash##name##NativeDevice(SDL_GPUDevice *device, void **instance,                    \
                                       void **physicalDevice, void **logicalDevice, void **queue, \
                                       Uint32 *queueFamily);                                      \
    void *SDL_Slash##name##NativeTexture(SDL_GPUTexture *texture, void **view, Uint32 *format);   \
    void *SDL_Slash##name##NativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);
#ifdef SDL_GPU_VULKAN
SLASH_NATIVE_CALLS(Vulkan)
#endif
#ifdef SDL_GPU_D3D12
SLASH_NATIVE_CALLS(D3D12)
void SDL_SlashD3D12RestoreState(SDL_GPUCommandBuffer *commandBuffer);
#endif
#ifdef SDL_GPU_METAL
SLASH_NATIVE_CALLS(Metal)
#endif

bool SDL_SlashGPUNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                              void **logicalDevice, void **queue, Uint32 *queueFamily);
void *SDL_SlashGPUNativeTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture, void **view,
                                Uint32 *format);
void *SDL_SlashGPUNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);
void SDL_SlashGPURestoreState(SDL_GPUCommandBuffer *commandBuffer);

bool SDL_SlashGPUNativeDevice(SDL_GPUDevice *device, void **instance, void **physicalDevice,
                              void **logicalDevice, void **queue, Uint32 *queueFamily)
{
    *instance = NULL;
    *physicalDevice = NULL;
    *logicalDevice = NULL;
    *queue = NULL;
    *queueFamily = 0;
    if (device == NULL) {
        return false;
    }
#ifdef SDL_GPU_VULKAN
    if (SDL_strcmp(device->backend, "vulkan") == 0) {
        return SDL_SlashVulkanNativeDevice(device, instance, physicalDevice, logicalDevice, queue, queueFamily);
    }
#endif
#ifdef SDL_GPU_D3D12
    if (SDL_strcmp(device->backend, "direct3d12") == 0) {
        return SDL_SlashD3D12NativeDevice(device, instance, physicalDevice, logicalDevice, queue, queueFamily);
    }
#endif
#ifdef SDL_GPU_METAL
    if (SDL_strcmp(device->backend, "metal") == 0) {
        return SDL_SlashMetalNativeDevice(device, instance, physicalDevice, logicalDevice, queue, queueFamily);
    }
#endif
    return false;
}

void *SDL_SlashGPUNativeTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture, void **view,
                                Uint32 *format)
{
    *view = NULL;
    *format = 0;
    if (device == NULL || texture == NULL) {
        return NULL;
    }
#ifdef SDL_GPU_VULKAN
    if (SDL_strcmp(device->backend, "vulkan") == 0) {
        return SDL_SlashVulkanNativeTexture(texture, view, format);
    }
#endif
#ifdef SDL_GPU_D3D12
    if (SDL_strcmp(device->backend, "direct3d12") == 0) {
        return SDL_SlashD3D12NativeTexture(texture, view, format);
    }
#endif
#ifdef SDL_GPU_METAL
    if (SDL_strcmp(device->backend, "metal") == 0) {
        return SDL_SlashMetalNativeTexture(texture, view, format);
    }
#endif
    return NULL;
}

void *SDL_SlashGPUNativeCommandBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    SDL_GPUDevice *device;
    if (commandBuffer == NULL) {
        return NULL;
    }
    device = ((CommandBufferCommonHeader *)commandBuffer)->device;
#ifdef SDL_GPU_VULKAN
    if (SDL_strcmp(device->backend, "vulkan") == 0) {
        return SDL_SlashVulkanNativeCommandBuffer(commandBuffer);
    }
#endif
#ifdef SDL_GPU_D3D12
    if (SDL_strcmp(device->backend, "direct3d12") == 0) {
        return SDL_SlashD3D12NativeCommandBuffer(commandBuffer);
    }
#endif
#ifdef SDL_GPU_METAL
    if (SDL_strcmp(device->backend, "metal") == 0) {
        return SDL_SlashMetalNativeCommandBuffer(commandBuffer);
    }
#endif
    return NULL;
}

void SDL_SlashGPURestoreState(SDL_GPUCommandBuffer *commandBuffer)
{
    if (commandBuffer == NULL) {
        return;
    }
#ifdef SDL_GPU_D3D12
    if (SDL_strcmp(((CommandBufferCommonHeader *)commandBuffer)->device->backend, "direct3d12") == 0) {
        SDL_SlashD3D12RestoreState(commandBuffer);
    }
#endif
}
]=])
endif()
