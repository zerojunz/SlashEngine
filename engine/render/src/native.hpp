#pragma once

#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

// What cmake/patches/sdl-native-handles.cmake adds to SDL3's GPU API, which doesn't show the
// native API's objects: the device's, the image an SDL texture draws into now and an SDL command
// buffer's, each unretained and null where the device's backend has none; and binding SDL's own
// Direct3D 12 descriptor heaps again after other code set its own.
extern "C" {
bool SDL_SlashGPUNativeDevice(SDL_GPUDevice* device, void** instance, void** physical_device,
                              void** logical_device, void** queue, Uint32* queue_family);
void* SDL_SlashGPUNativeTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, void** view,
                                Uint32* format);
void* SDL_SlashGPUNativeCommandBuffer(SDL_GPUCommandBuffer* commands);
void SDL_SlashGPURestoreState(SDL_GPUCommandBuffer* commands);
}

namespace slash::render::detail {

// The GPU device's native objects, for a temporal upscaler (TemporalFrame::device).
[[nodiscard]] inline NativeDevice native_device(SDL_GPUDevice* device, Backend backend) {
    NativeDevice native{.backend = backend};
    Uint32 family = 0;
    if (SDL_SlashGPUNativeDevice(device, &native.instance, &native.physical_device, &native.device,
                                 &native.queue, &family)) {
        native.queue_family = family;
    }
    return native;
}

// texture's native image, view and format, width x height.
[[nodiscard]] inline NativeTexture native_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture,
                                                  std::uint32_t width, std::uint32_t height) {
    NativeTexture native{.width = width, .height = height, .pixels = {}};
    Uint32 format = 0;
    native.image = SDL_SlashGPUNativeTexture(device, texture, &native.view, &format);
    native.format = format;
    return native;
}

} // namespace slash::render::detail
