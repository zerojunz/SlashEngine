# Hands SlashEngine Metal's objects behind SDL3's GPU API, which doesn't show them: the device's
# MTLDevice, the texture an SDL texture draws into now and an SDL command buffer's
# MTLCommandBuffer, each unretained; the device's is null where it isn't Metal's, and the others
# take only a Metal device's objects. Apple's MetalFX
# scalers (engine/render/src/metalfx.mm) take them, as Godot's take its own Metal driver's.
# slash_add_dependency(SDL3) runs it in SDL's source folder after the Vulkan patch.

set(file src/gpu/metal/SDL_gpu_metal.m)
file(READ ${file} text)
if(text MATCHES "SDL_SlashMetalDevice")
    return()
endif()
set(end "#endif // SDL_GPU_METAL")
string(FIND "${text}" "${end}" at REVERSE)
if(at EQUAL -1 OR NOT text MATCHES "MetalTexture \\*activeTexture;"
        OR NOT text MATCHES "id<MTLCommandBuffer> handle;"
        OR NOT text MATCHES "id<MTLDevice> device;")
    message(FATAL_ERROR "${file} no longer has the Metal objects the patch hands over")
endif()
set(calls [=[
// SlashEngine (cmake/patches/sdl-metal-handles.cmake): Metal's objects, for MetalFX.
void *SDL_SlashMetalDevice(SDL_GPUDevice *device);
void *SDL_SlashMetalTexture(SDL_GPUTexture *texture);
void *SDL_SlashMetalCommandBuffer(SDL_GPUCommandBuffer *commandBuffer);

void *SDL_SlashMetalDevice(SDL_GPUDevice *device)
{
    if (device == NULL || SDL_strcmp(device->backend, "metal") != 0) {
        return NULL;
    }
    return (__bridge void *)((MetalRenderer *)device->driverData)->device;
}

void *SDL_SlashMetalTexture(SDL_GPUTexture *texture)
{
    if (texture == NULL) {
        return NULL;
    }
    return (__bridge void *)((MetalTextureContainer *)texture)->activeTexture->handle;
}

void *SDL_SlashMetalCommandBuffer(SDL_GPUCommandBuffer *commandBuffer)
{
    if (commandBuffer == NULL) {
        return NULL;
    }
    return (__bridge void *)((MetalCommandBuffer *)commandBuffer)->handle;
}

]=])
string(SUBSTRING "${text}" 0 ${at} before)
string(SUBSTRING "${text}" ${at} -1 after)
file(WRITE ${file} "${before}${calls}${after}")
