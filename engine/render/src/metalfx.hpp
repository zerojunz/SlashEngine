#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <memory>

// Apple's MetalFX scalers in the GPU device on Metal, as Godot's MFXSpatialEffect and
// MFXTemporalEffect (servers/rendering/renderer_rd/effects/metal_fx.mm) run them. SDL3's GPU API
// doesn't show Metal's objects, so the scalers take them through the calls
// cmake/patches/sdl-native-handles.cmake adds (native.hpp) and encode between SDL's passes in its
// command buffer. MetalFX is
// linked weakly, so games still start on macOS 11 and iOS 14, where it isn't. Off Apple's
// platforms (metalfx_none.cpp) there is none.
namespace slash::render::detail {

// One MetalFX scaler, made for its textures' sizes and formats: a spatial one, or a temporal one,
// which keeps the frames before.
struct MetalFxScaler;
struct MetalFxScalerDeleter {
    void operator()(MetalFxScaler* scaler) const;
};
using MetalFxScalerPtr = std::unique_ptr<MetalFxScaler, MetalFxScalerDeleter>;

// The scalers a device runs: none but on Metal, on macOS 13 and iOS 16 or later, with a GPU
// MetalFX takes.
[[nodiscard]] MetalFxSupport metalfx_support(SDL_GPUDevice* device);

// Encodes the spatial scaler from color into output (made with COMPUTE_STORAGE_WRITE) between
// passes of commands, making scaler first, or again when the textures' sizes or formats changed.
// perceptual says the colors are as they are drawn (sRGB) rather than linear.
[[nodiscard]] Result<void> encode_metalfx_spatial(SDL_GPUDevice* device,
                                                  SDL_GPUCommandBuffer* commands,
                                                  MetalFxScalerPtr& scaler, SDL_GPUTexture* color,
                                                  SDL_GPUTexture* output, bool perceptual);

// What the temporal scaler takes for a frame: the color, depth (reversed) and motion vectors
// (in texture coordinates, from where a pixel is to where it was) at the render size, the output
// at the display size (made with COMPUTE_STORAGE_WRITE), the camera's jitter in render pixels
// and whether the frames before are forgotten.
struct MetalFxTemporalFrame {
    SDL_GPUTexture* color = nullptr;
    SDL_GPUTexture* depth = nullptr;
    SDL_GPUTexture* motion = nullptr;
    SDL_GPUTexture* output = nullptr;
    std::array<float, 2> jitter{};
    bool reset = false;
};

// Encodes the temporal scaler between passes of commands, making scaler first, or again (which
// forgets the frames before) when the textures' sizes or formats changed.
[[nodiscard]] Result<void> encode_metalfx_temporal(SDL_GPUDevice* device,
                                                   SDL_GPUCommandBuffer* commands,
                                                   MetalFxScalerPtr& scaler,
                                                   const MetalFxTemporalFrame& frame);

// The temporal scaler's jitter for FSR 2's (Fsr2Uniforms::jitter_scale, render pixels with y
// down), as Godot hands it over: y flipped.
[[nodiscard]] constexpr std::array<float, 2> metalfx_jitter(const std::array<float, 2>& fsr2) {
    return {fsr2[0], -fsr2[1]};
}

} // namespace slash::render::detail
