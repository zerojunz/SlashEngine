// A frame generated between two drawn ones, as AMD's FSR 3 frame generation makes it: a white
// card slides right past a blue post, and after each frame the device makes the frame halfway
// between it and the one before from the prepass's depth and motion vectors and from the optical
// flow of the two pictures.
//
// This draws on the software device, which generates frames on the CPU and makes the same pixels
// on every machine, so the output below is the same everywhere.

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace render = slash::render;

constexpr std::uint32_t kWidth = 48;
constexpr std::uint32_t kHeight = 16;

// Stops with the error if a call fails, so the output never hides one.
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

// A rectangle w x h facing +Z at depth z, its corners clockwise from the front.
std::vector<render::Vertex3D> rectangle(float w, float h, float z) {
    const auto corner = [&](float cx, float cy) {
        return render::Vertex3D{.position = {cx * w * 0.5F, cy * h * 0.5F, z},
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

// The middle row of an image: # the card, | the post, + a blend of them or of the card and the
// background, . the background.
std::string row(const render::Image& image) {
    std::string out;
    const std::size_t y = kHeight / 2;
    for (std::uint32_t x = 0; x < kWidth; ++x) {
        const std::uint8_t* p = &image.pixels[((y * kWidth) + x) * 4];
        if (p[0] > 200 && p[2] > 200) {
            out += '#';
        } else if (p[2] > p[0] + 60) {
            out += '|';
        } else if (p[0] > 60) {
            out += '+';
        } else {
            out += '.';
        }
    }
    return out;
}

} // namespace

int main() {
    const auto device = render::create_software_device();
    const render::TextureId target =
        value(device->create_texture({.width = kWidth, .height = kHeight, .render_target = true}));
    const render::TextureId generated =
        value(device->create_texture({.width = kWidth, .height = kHeight, .render_target = true}));
    // The card, 6 pixels wide, in front of the post, 2 pixels wide (16 pixels a unit).
    const render::MeshId card = value(device->create_mesh(rectangle(0.375F, 0.5F, 0.1F), {}));
    const render::MeshId post = value(device->create_mesh(rectangle(0.125F, 0.75F, 0.0F), {}));
    const render::Material3D white{.albedo = {1.0F, 1.0F, 1.0F, 1.0F}, .unshaded = true};
    const render::Material3D blue{.albedo = {0.2F, 0.4F, 1.0F, 1.0F}, .unshaded = true};

    std::optional<float> before;
    for (int frame = 0; frame < 6; ++frame) {
        // It moves 6 pixels right a frame.
        const float x = -1.125F + (0.375F * static_cast<float>(frame));
        render::Scene3D scene;
        scene.method = render::RenderMethod::ForwardPlus;
        scene.frame_generation = true;
        scene.camera = render::look_at({0.0F, 0.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
        scene.projection = render::orthogonal(1.0F, 3.0F, 0.1F, 20.0F);
        if (!value(device->begin_frame(
                {.target = target, .clear = {0.05F, 0.05F, 0.05F, 1.0F}, .scene = scene}))) {
            return 1;
        }
        check(device->draw_mesh(post, at_x(0.5F), blue));
        std::optional<render::Mat4> moved;
        if (before) {
            moved = at_x(*before);
        }
        check(device->draw_mesh(card, at_x(x), white, 1, render::ShadowCasting::On, moved));
        check(device->end_frame());
        check(device->generate_frame(target, generated));
        if (frame > 0) {
            std::printf("generated  %s\n", row(value(device->read_texture(generated))).c_str());
        } else {
            // A first frame has none before it, so the generated frame is the frame itself.
            const bool same = value(device->read_texture(generated)).pixels ==
                              value(device->read_texture(target)).pixels;
            std::printf("first frame generated as drawn: %s\n", same ? "yes" : "no");
        }
        std::printf("frame %d    %s\n", frame, row(value(device->read_texture(target))).c_str());
        before = x;
    }
    return 0;
}
