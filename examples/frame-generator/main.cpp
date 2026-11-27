// A game's own frame generator, as DLSS Frame Generation, DLSS Multi Frame Generation or XeSS
// Frame Generation come in from a vendor's SDK, made with render::add_frame_generator and run by
// the device in place of the engine's FSR 3 frame generation.
//
// A frame whose Scene3D names the generator keeps its prepass's depth and motion vectors for it;
// RenderDevice::generate_frames then hands it the drawn frame, the depth and motion at the render
// size and the targets to write, earliest first. On a GPU those are the native API's objects (a
// VkImage, an ID3D12Resource, an MTLTexture) with the command buffer, which a vendor's library
// takes; on the software device, used here, they are float pixels, so the output below is the
// same on every machine.
//
// This one, "slide", makes three frames between two drawn ones, as Multi Frame Generation does:
// it moves each pixel the meshes drew back along its motion vector, by the part of the frame each
// generated frame is before the drawn one, over the background.

#include "slash/render/device.hpp"
#include "slash/render/frame_generator.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace render = slash::render;

constexpr std::uint32_t kWidth = 48;
constexpr std::uint32_t kHeight = 16;
// The background the frames clear to.
constexpr std::array<float, 4> kBackground{0.05F, 0.05F, 0.05F, 1.0F};

void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

template <typename T>
T value(slash::Result<T> result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
    return std::move(result.value());
}

// Moves what the meshes drew back along the motion vectors, for each frame between.
class Slide final : public render::FrameGenerator {
public:
    slash::Result<void> generate(const render::GeneratedFrames& frames) override {
        const render::NativeTexture& color = frames.color;
        const std::size_t count = frames.outputs.size();
        std::printf("  slide: %zu frames, %ux%u, depth and motion %s%s\n", count, color.width,
                    color.height, frames.depth.pixels.empty() ? "none" : "kept",
                    frames.reset ? ", reset" : "");
        for (std::size_t i = 0; i < count; ++i) {
            const std::span<float> out = frames.outputs[i].pixels;
            // The background, where nothing moves, is as drawn.
            std::copy(color.pixels.begin(), color.pixels.end(), out.begin());
            if (frames.reset || frames.depth.pixels.empty()) {
                continue;
            }
            // The output is this part of the way from the frame before to the drawn one.
            const float part = static_cast<float>(i + 1) / static_cast<float>(count + 1);
            const std::uint32_t w = frames.depth.width;
            const std::uint32_t h = frames.depth.height;
            // What the meshes drew leaves where it is now, then lands where it was then.
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    if (frames.depth.pixels[(std::size_t{y} * w) + x] != 0.0F) {
                        std::copy_n(kBackground.data(), 4, &out[((std::size_t{y} * w) + x) * 4]);
                    }
                }
            }
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t at = (std::size_t{y} * w) + x;
                    if (frames.depth.pixels[at] == 0.0F) {
                        continue;
                    }
                    // A pixel was at its own plus its motion a frame before.
                    const float back = 1.0F - part;
                    const long tx =
                        std::lround(static_cast<float>(x) +
                                    (frames.motion.pixels[at * 2] * static_cast<float>(w) * back));
                    const long ty =
                        std::lround(static_cast<float>(y) + (frames.motion.pixels[(at * 2) + 1] *
                                                             static_cast<float>(h) * back));
                    if (tx < 0 || ty < 0 || tx >= static_cast<long>(w) ||
                        ty >= static_cast<long>(h)) {
                        continue;
                    }
                    const std::size_t to =
                        (static_cast<std::size_t>(ty) * w) + static_cast<std::size_t>(tx);
                    std::copy_n(&color.pixels[at * 4], 4, &out[to * 4]);
                }
            }
        }
        return {};
    }
};

// A rectangle w x h facing +Z, its corners clockwise from the front.
std::vector<render::Vertex3D> rectangle(float w, float h) {
    const auto corner = [&](float cx, float cy) {
        return render::Vertex3D{.position = {cx * w * 0.5F, cy * h * 0.5F, 0.0F},
                                .normal = {0.0F, 0.0F, 1.0F},
                                .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                                .uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

render::Mat4 at_x(float x) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
}

// The middle row of an image: # the card, . the background.
std::string row(const render::Image& image) {
    std::string out;
    const std::size_t y = kHeight / 2;
    for (std::uint32_t x = 0; x < kWidth; ++x) {
        out += image.pixels[((y * kWidth) + x) * 4] > 128 ? '#' : '.';
    }
    return out;
}

} // namespace

int main() {
    // A game adds its frame generators before it draws, for project.json's
    // rendering.frame_generator to name; an SDK package's register function would add the
    // vendor's, with a supports function that checks the GPU and driver.
    check(render::add_frame_generator(
        {.name = "slide", .max_frames = 3, .supports = {}, .make = [] {
             return std::unique_ptr<render::FrameGenerator>(std::make_unique<Slide>());
         }}));

    const auto device = render::create_software_device();
    const render::TextureId target =
        value(device->create_texture({.width = kWidth, .height = kHeight, .render_target = true}));
    std::array<render::TextureId, 3> between{};
    for (render::TextureId& made : between) {
        made = value(
            device->create_texture({.width = kWidth, .height = kHeight, .render_target = true}));
    }
    // The card, 4 pixels wide (16 pixels a unit), moves 8 pixels right a frame.
    const render::MeshId card = value(device->create_mesh(rectangle(0.25F, 0.5F), {}));
    const render::Material3D white{.albedo = {1.0F, 1.0F, 1.0F, 1.0F}, .unshaded = true};
    std::optional<float> before;
    for (int frame = 0; frame < 4; ++frame) {
        const float x = -1.0F + (0.5F * static_cast<float>(frame));
        render::Scene3D scene;
        scene.method = render::RenderMethod::ForwardPlus;
        scene.frame_generation = true;
        scene.frame_generator = "slide";
        scene.camera = render::look_at({0.0F, 0.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
        scene.projection = render::orthogonal(1.0F, 3.0F, 0.1F, 20.0F);
        if (!value(device->begin_frame(
                {.target = target, .clear = {0.05F, 0.05F, 0.05F, 1.0F}, .scene = scene}))) {
            return 1;
        }
        std::optional<render::Mat4> moved;
        if (before) {
            moved = at_x(*before);
        }
        check(device->draw_mesh(card, at_x(x), white, 1, render::ShadowCasting::On, moved));
        check(device->end_frame());
        check(device->generate_frames(target, between, "slide"));
        for (const render::TextureId made : between) {
            std::printf("generated  %s\n", row(value(device->read_texture(made))).c_str());
        }
        std::printf("frame %d    %s\n", frame, row(value(device->read_texture(target))).c_str());
        before = x;
    }
    // The engine's own generation makes one frame; asking it for three is refused.
    const auto refused = device->generate_frames(target, between, "");
    std::printf("fsr3 with 3 targets: %s\n", refused ? "made" : refused.error().message.c_str());
    return 0;
}
