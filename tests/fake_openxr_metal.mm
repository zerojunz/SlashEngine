// The stand-in OpenXR runtime's Metal objects (fake_openxr_runtime.cpp), on macOS. Built with ARC.
#import <Metal/Metal.h>
#include <cstdint>
#include <cstring>
#include <vector>

// The GPU it names for sessions, kept for the process's life.
void* fake_metal_device() {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return (__bridge void*)device;
}

// count textures of a swapchain, each retained until fake_metal_release; none if one fails.
std::vector<void*> fake_metal_textures(void* device, std::uint32_t width, std::uint32_t height,
                                       std::int64_t format, int count) {
    id<MTLDevice> gpu = (__bridge id<MTLDevice>)device;
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:static_cast<MTLPixelFormat>(format)
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    // What runtimes ask for: drawn to and sampled, kept on the GPU.
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    std::vector<void*> out;
    for (int i = 0; i < count; ++i) {
        id<MTLTexture> texture = [gpu newTextureWithDescriptor:descriptor];
        if (texture == nil) {
            for (void* made : out) {
                (void)(__bridge_transfer id<MTLTexture>)made;
            }
            return {};
        }
        out.push_back((__bridge_retained void*)texture);
    }
    return out;
}

void fake_metal_release(std::vector<void*>& textures) {
    for (void* texture : textures) {
        (void)(__bridge_transfer id<MTLTexture>)texture;
    }
    textures.clear();
}

// Reads a texture's RGBA8 rows, top first, through a blit on the session's queue.
bool fake_metal_read(void* queue, void* texture, std::uint32_t width, std::uint32_t height,
                     std::vector<std::uint8_t>& pixels) {
    id<MTLCommandQueue> commands = (__bridge id<MTLCommandQueue>)queue;
    id<MTLTexture> image = (__bridge id<MTLTexture>)texture;
    const NSUInteger row = NSUInteger{width} * 4;
    id<MTLBuffer> buffer = [commands.device newBufferWithLength:row * height
                                                        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> buffer_commands = [commands commandBuffer];
    if (buffer == nil || buffer_commands == nil) {
        return false;
    }
    id<MTLBlitCommandEncoder> blit = [buffer_commands blitCommandEncoder];
    [blit copyFromTexture:image
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:row * height];
    [blit endEncoding];
    [buffer_commands commit];
    [buffer_commands waitUntilCompleted];
    if (buffer_commands.status != MTLCommandBufferStatusCompleted) {
        return false;
    }
    pixels.resize(row * height);
    std::memcpy(pixels.data(), buffer.contents, pixels.size());
    return true;
}
