// Apple's MetalFX scalers in the GPU device on Metal (metalfx.hpp), after Godot's
// servers/rendering/renderer_rd/effects/metal_fx.mm and its Metal driver's MetalFX limits.
// Built with ARC.
#include "metalfx.hpp"

#include "native.hpp"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <algorithm>
#include <string>

namespace slash::render::detail {

struct MetalFxScaler {
    // An id<MTLFXSpatialScaler> or id<MTLFXTemporalScaler>.
    id scaler = nil;
    bool temporal = false;
    NSUInteger input_width = 0;
    NSUInteger input_height = 0;
    NSUInteger output_width = 0;
    NSUInteger output_height = 0;
    MTLPixelFormat color = MTLPixelFormatInvalid;
    MTLPixelFormat depth = MTLPixelFormatInvalid;
    MTLPixelFormat motion = MTLPixelFormatInvalid;
    MTLPixelFormat output = MTLPixelFormatInvalid;
    bool perceptual = false;
};

void MetalFxScalerDeleter::operator()(MetalFxScaler* scaler) const {
    delete scaler;
}

namespace {

id<MTLDevice> metal_device(SDL_GPUDevice* device) {
    void* instance = nullptr;
    void* physical = nullptr;
    void* logical = nullptr;
    void* queue = nullptr;
    Uint32 family = 0;
    if (!SDL_SlashGPUNativeDevice(device, &instance, &physical, &logical, &queue, &family)) {
        return nil;
    }
    return (__bridge id<MTLDevice>)logical;
}

id<MTLTexture> metal_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture) {
    void* view = nullptr;
    Uint32 format = 0;
    return (__bridge id<MTLTexture>)SDL_SlashGPUNativeTexture(device, texture, &view, &format);
}

id<MTLCommandBuffer> metal_commands(SDL_GPUCommandBuffer* commands) {
    return (__bridge id<MTLCommandBuffer>)SDL_SlashGPUNativeCommandBuffer(commands);
}

Result<void> missing(const char* what) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string("MetalFX needs Metal's ") + what + ", which this device lacks");
}

// Keeps the scaler and its textures alive until the GPU has run what it encoded: SDL's command
// buffers don't retain what they use, as Godot's comment on its own says.
void retain_until_done(id<MTLCommandBuffer> buffer, id scaler, NSArray* textures) {
    __block id held_scaler = scaler;
    __block NSArray* held_textures = textures;
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> /*done*/) {
      held_scaler = nil;
      held_textures = nil;
    }];
}

} // namespace

MetalFxSupport metalfx_support(SDL_GPUDevice* device) {
    MetalFxSupport support;
    id<MTLDevice> metal = metal_device(device);
    if (metal == nil) {
        return support;
    }
    if (@available(macOS 13.0, iOS 16.0, *)) {
        support.spatial = [MTLFXSpatialScalerDescriptor supportsDevice:metal];
        support.temporal = [MTLFXTemporalScalerDescriptor supportsDevice:metal];
        // The render scale is the inverse of MetalFX's input content scale: Godot's limits.
        support.min_scale = 0.5F;
        support.max_scale = 1.0F;
        if (@available(macOS 14.0, iOS 17.0, *)) {
            const float most =
                [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:metal];
            const float least =
                [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:metal];
            if (most > 0.0F && least > 0.0F) {
                support.min_scale = 1.0F / most;
                support.max_scale = 1.0F / least;
            }
        }
    }
    return support;
}

Result<void> encode_metalfx_spatial(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commands,
                                    MetalFxScalerPtr& scaler, SDL_GPUTexture* color,
                                    SDL_GPUTexture* output, bool perceptual) {
    if (@available(macOS 13.0, iOS 16.0, *)) {
        id<MTLDevice> metal = metal_device(device);
        id<MTLCommandBuffer> buffer = metal_commands(commands);
        id<MTLTexture> in = metal_texture(device, color);
        id<MTLTexture> out = metal_texture(device, output);
        if (metal == nil || buffer == nil || in == nil || out == nil) {
            return missing("device, command buffer and textures");
        }
        const bool same = scaler && !scaler->temporal && scaler->input_width == in.width &&
                          scaler->input_height == in.height && scaler->output_width == out.width &&
                          scaler->output_height == out.height && scaler->color == in.pixelFormat &&
                          scaler->output == out.pixelFormat && scaler->perceptual == perceptual;
        if (!same) {
            MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
            desc.inputWidth = in.width;
            desc.inputHeight = in.height;
            desc.outputWidth = out.width;
            desc.outputHeight = out.height;
            desc.colorTextureFormat = in.pixelFormat;
            desc.outputTextureFormat = out.pixelFormat;
            desc.colorProcessingMode = perceptual ? MTLFXSpatialScalerColorProcessingModePerceptual
                                                  : MTLFXSpatialScalerColorProcessingModeLinear;
            id<MTLFXSpatialScaler> made = [desc newSpatialScalerWithDevice:metal];
            if (made == nil) {
                return make_error(ErrorCode::PlatformFailure,
                                  "making MetalFX's spatial scaler failed");
            }
            scaler.reset(new MetalFxScaler{.scaler = made,
                                           .temporal = false,
                                           .input_width = in.width,
                                           .input_height = in.height,
                                           .output_width = out.width,
                                           .output_height = out.height,
                                           .color = in.pixelFormat,
                                           .depth = MTLPixelFormatInvalid,
                                           .motion = MTLPixelFormatInvalid,
                                           .output = out.pixelFormat,
                                           .perceptual = perceptual});
        }
        id<MTLFXSpatialScaler> spatial = scaler->scaler;
        spatial.colorTexture = in;
        spatial.outputTexture = out;
        spatial.inputContentWidth = in.width;
        spatial.inputContentHeight = in.height;
        [spatial encodeToCommandBuffer:buffer];
        retain_until_done(buffer, spatial, @[ in, out ]);
        return {};
    }
    return make_error(ErrorCode::InvalidArgument, "MetalFX needs macOS 13 or iOS 16");
}

Result<void> encode_metalfx_temporal(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commands,
                                     MetalFxScalerPtr& scaler, const MetalFxTemporalFrame& frame) {
    if (@available(macOS 13.0, iOS 16.0, *)) {
        id<MTLDevice> metal = metal_device(device);
        id<MTLCommandBuffer> buffer = metal_commands(commands);
        id<MTLTexture> color = metal_texture(device, frame.color);
        id<MTLTexture> depth = metal_texture(device, frame.depth);
        id<MTLTexture> motion = metal_texture(device, frame.motion);
        id<MTLTexture> out = metal_texture(device, frame.output);
        if (metal == nil || buffer == nil || color == nil || depth == nil || motion == nil ||
            out == nil) {
            return missing("device, command buffer and textures");
        }
        const bool same =
            scaler && scaler->temporal && scaler->input_width == color.width &&
            scaler->input_height == color.height && scaler->output_width == out.width &&
            scaler->output_height == out.height && scaler->color == color.pixelFormat &&
            scaler->depth == depth.pixelFormat && scaler->motion == motion.pixelFormat &&
            scaler->output == out.pixelFormat;
        bool reset = frame.reset;
        if (!same) {
            MTLFXTemporalScalerDescriptor* desc = [MTLFXTemporalScalerDescriptor new];
            desc.inputWidth = color.width;
            desc.inputHeight = color.height;
            desc.outputWidth = out.width;
            desc.outputHeight = out.height;
            desc.colorTextureFormat = color.pixelFormat;
            desc.depthTextureFormat = depth.pixelFormat;
            desc.motionTextureFormat = motion.pixelFormat;
            desc.outputTextureFormat = out.pixelFormat;
            desc.autoExposureEnabled = NO;
            id<MTLFXTemporalScaler> made = [desc newTemporalScalerWithDevice:metal];
            if (made == nil) {
                return make_error(ErrorCode::PlatformFailure,
                                  "making MetalFX's temporal scaler failed");
            }
            // The motion vectors are in texture coordinates; MetalFX takes render pixels.
            made.motionVectorScaleX = static_cast<float>(color.width);
            made.motionVectorScaleY = static_cast<float>(color.height);
            // Depth is reversed, as Godot's.
            made.depthReversed = YES;
            scaler.reset(new MetalFxScaler{.scaler = made,
                                           .temporal = true,
                                           .input_width = color.width,
                                           .input_height = color.height,
                                           .output_width = out.width,
                                           .output_height = out.height,
                                           .color = color.pixelFormat,
                                           .depth = depth.pixelFormat,
                                           .motion = motion.pixelFormat,
                                           .output = out.pixelFormat,
                                           .perceptual = false});
            reset = true;
        }
        id<MTLFXTemporalScaler> temporal = scaler->scaler;
        temporal.reset = reset;
        temporal.colorTexture = color;
        temporal.depthTexture = depth;
        temporal.motionTexture = motion;
        temporal.exposureTexture = nil;
        temporal.outputTexture = out;
        temporal.inputContentWidth = color.width;
        temporal.inputContentHeight = color.height;
        temporal.jitterOffsetX = frame.jitter[0];
        temporal.jitterOffsetY = frame.jitter[1];
        [temporal encodeToCommandBuffer:buffer];
        retain_until_done(buffer, temporal, @[ color, depth, motion, out ]);
        return {};
    }
    return make_error(ErrorCode::InvalidArgument, "MetalFX needs macOS 13 or iOS 16");
}

} // namespace slash::render::detail
