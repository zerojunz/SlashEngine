// Draw into a texture and read its pixels back: how a test, or an agent, checks what a game
// draws without a window or a screenshot.
//
// The renderer's device layer is a dozen calls over SDL3's GPU API. create_device_or_null picks
// the GPU backend that works here (Vulkan, Metal or Direct3D 12) and falls back to the null
// device where none does, such as a server with no GPU. Both refuse the same mistakes, so code
// checked on one works on the other.

#include "slash/render/device.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace render = slash::render;

using Rgba = std::array<std::uint8_t, 4>;

// Two triangles covering a rectangle, in pixels from the top left, with the whole texture on it.
std::vector<render::Vertex2D> rect(float x0, float y0, float x1, float y1, Rgba rgba) {
    const render::Vertex2D top_left{.x = x0, .y = y0, .u = 0.0F, .v = 0.0F, .rgba = rgba};
    const render::Vertex2D top_right{.x = x1, .y = y0, .u = 1.0F, .v = 0.0F, .rgba = rgba};
    const render::Vertex2D bottom_left{.x = x0, .y = y1, .u = 0.0F, .v = 1.0F, .rgba = rgba};
    const render::Vertex2D bottom_right{.x = x1, .y = y1, .u = 1.0F, .v = 1.0F, .rgba = rgba};
    return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

} // namespace

int main() {
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});

    // A render target to draw into, and a 2x2 checker drawn with sharp texels.
    const render::TextureId target = device
                                         ->create_texture({.width = 64,
                                                           .height = 64,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    const std::vector<std::uint8_t> checker_texels{
        255, 255, 255, 255, 0,   0,   0,   255, // white, black
        0,   0,   0,   255, 255, 255, 255, 255, // black, white
    };
    const render::TextureId checker = device
                                          ->create_texture({.width = 2,
                                                            .height = 2,
                                                            .filter = render::Filter::Nearest,
                                                            .render_target = false},
                                                           checker_texels)
                                          .value();

    // One frame: two plain squares, which share a GPU draw, then the checker.
    const render::Color clear{.r = 0.2F, .g = 0.4F, .b = 0.6F, .a = 1.0F};
    if (!device->begin_frame({.target = target, .clear = clear, .view = {}}).value()) {
        return 1;
    }
    check(device->draw(render::TextureId::None, rect(8, 8, 24, 24, {255, 0, 0, 255})));
    check(device->draw(render::TextureId::None, rect(40, 8, 56, 24, {0, 255, 0, 255})));
    check(device->draw(checker, rect(8, 32, 56, 56, {255, 255, 255, 255})));

    // Every three vertices make a triangle, so four is refused and nothing is drawn.
    std::vector<render::Vertex2D> four = rect(0, 0, 4, 4, {255, 255, 255, 255});
    four.resize(4);
    std::printf("draw 4 vertices: %s\n",
                device->draw(render::TextureId::None, four).error().message.c_str());
    check(device->end_frame());

    // Read the pixels back. A corner nothing covers shows the clear color on every device; on a
    // GPU the squares and the checker are there too, and tests/test_render.cpp checks them.
    const render::Image image = device->read_texture(target).value();
    const std::size_t corner = ((std::size_t{60} * image.width) + 60) * 4;
    std::printf("image: %ux%u, pixel (60, 60) = %u,%u,%u,%u\n", image.width, image.height,
                image.pixels[corner], image.pixels[corner + 1], image.pixels[corner + 2],
                image.pixels[corner + 3]);

    const render::Stats stats = device->stats();
    std::printf("frames: %llu, GPU draws: %llu, triangles: %llu, textures: %u\n",
                static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles), stats.textures);
    return 0;
}
