// Copies into an OpenXR swapchain's Metal texture (native_copy.hpp). Built with ARC.
#include "native_copy.hpp"

#import <Metal/Metal.h>
#include <string>

namespace slash::render::detail {

Result<void> copy_native_metal(void* commands, const NativeTexture& source,
                               const NativeTexture& target) {
    id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)commands;
    id<MTLTexture> from = (__bridge id<MTLTexture>)source.image;
    id<MTLTexture> to = (__bridge id<MTLTexture>)target.image;
    if (buffer == nil || from == nil || to == nil) {
        return make_error(ErrorCode::InvalidArgument, "Metal's copy needs a command buffer and "
                                                      "two textures");
    }
    // A render target's bytes hold sRGB values already, so an sRGB swapchain image takes them as
    // they are, through a view of its format without sRGB where the two differ.
    id<MTLTexture> into = to;
    if (to.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB &&
        from.pixelFormat == MTLPixelFormatRGBA8Unorm) {
        into = [to newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    } else if (to.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB &&
               from.pixelFormat == MTLPixelFormatBGRA8Unorm) {
        into = [to newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    }
    if (into == nil || into.pixelFormat != from.pixelFormat) {
        return make_error(ErrorCode::InvalidArgument,
                          "Metal copies only between textures of one pixel format, not " +
                              std::to_string(static_cast<unsigned>(from.pixelFormat)) + " and " +
                              std::to_string(static_cast<unsigned>(to.pixelFormat)));
    }
    id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
    [blit copyFromTexture:from
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source.width, source.height, 1)
                toTexture:into
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return {};
}

} // namespace slash::render::detail
