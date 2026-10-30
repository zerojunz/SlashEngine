#include "../engine/render/src/frame_generation.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using render::RenderDevice;
using render::TextureId;
using render::Vertex3D;

namespace {

// A gray value for the point (x, y) of a blocky pattern of 4 x 4 pixel cells, from an integer
// hash, so every block of 8 x 8 pixels looks unlike its neighbors.
std::uint8_t pattern(int x, int y) {
    const auto cell_x = static_cast<std::uint32_t>(x >> 2);
    const auto cell_y = static_cast<std::uint32_t>(y >> 2);
    std::uint32_t h = (cell_x * 73856093U) ^ (cell_y * 19349663U);
    h ^= h >> 13U;
    h *= 0x5bd1e995U;
    h ^= h >> 15U;
    return static_cast<std::uint8_t>(h & 0xFFU);
}

// The pattern moved right by dx and down by dy, as RGBA.
std::vector<std::uint8_t> moved_pattern(std::uint32_t width, std::uint32_t height, int dx, int dy) {
    std::vector<std::uint8_t> rgba(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t value =
                pattern(static_cast<int>(x) - dx + 64, static_cast<int>(y) - dy + 64);
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            rgba[at] = value;
            rgba[at + 1] = value;
            rgba[at + 2] = value;
            rgba[at + 3] = 255;
        }
    }
    return rgba;
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// How many of the channels of a and b differ by more than by.
std::size_t differing(const Image& a, const Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
}

// The mean x of the pixels whose red is over half.
double bright_x(const Image& image) {
    double sum = 0.0;
    double count = 0.0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            if (image.pixels[((std::size_t{y} * image.width) + x) * 4] > 128) {
                sum += static_cast<double>(x);
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? sum / count : -1.0;
}

// A square facing +Z, one unit across, its corners clockwise from the front.
std::vector<Vertex3D> square() {
    const auto corner = [](float cx, float cy) {
        return Vertex3D{.position = {cx * 0.5F, cy * 0.5F, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

std::array<float, 16> moved_to(float x) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
}

// Draws a white square at x over a dark red background into target, seen in an orthogonal
// projection three units across, its motion from previous when there is one.
void draw_square_frame(RenderDevice& device, TextureId target, render::MeshId mesh, float x,
                       const std::optional<float>& previous, bool generation) {
    render::Scene3D scene;
    scene.method = render::RenderMethod::ForwardPlus;
    scene.frame_generation = generation;
    scene.camera = render::look_at({0.0F, 0.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F);
    REQUIRE(
        device.begin_frame({.target = target, .clear = {0.15F, 0.02F, 0.02F, 1.0F}, .scene = scene})
            .ok());
    const render::Material3D white{.unshaded = true, .cull = render::CullMode::Disabled};
    std::optional<std::array<float, 16>> before;
    if (previous) {
        before = moved_to(*previous);
    }
    REQUIRE(device.draw_mesh(mesh, moved_to(x), white, 1, render::ShadowCasting::On, before).ok());
    REQUIRE(device.end_frame().ok());
}

// Draws the pattern moved right by dx into target, as 2D draws with no scene.
void draw_pattern_frame(RenderDevice& device, TextureId target, TextureId texture, float dx) {
    REQUIRE(device.begin_frame({.target = target, .clear = {0.0F, 0.0F, 0.0F, 1.0F}}).ok());
    const std::array<render::Vertex2D, 6> quad{
        render::Vertex2D{.x = dx, .y = 0.0F, .u = 0.0F, .v = 0.0F},
        render::Vertex2D{.x = dx + 64.0F, .y = 0.0F, .u = 1.0F, .v = 0.0F},
        render::Vertex2D{.x = dx + 64.0F, .y = 64.0F, .u = 1.0F, .v = 1.0F},
        render::Vertex2D{.x = dx, .y = 0.0F, .u = 0.0F, .v = 0.0F},
        render::Vertex2D{.x = dx + 64.0F, .y = 64.0F, .u = 1.0F, .v = 1.0F},
        render::Vertex2D{.x = dx, .y = 64.0F, .u = 0.0F, .v = 1.0F}};
    REQUIRE(device.draw(texture, quad).ok());
    REQUIRE(device.end_frame().ok());
}

Image read(RenderDevice& device, TextureId texture) {
    auto image = device.read_texture(texture);
    REQUIRE(image.ok());
    return image.value();
}

} // namespace

TEST_CASE("The optical flow finds where each block of a moving picture was") {
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 64;
    CHECK(detail::optical_flow_size(kWidth, kHeight, 0) == std::array<std::uint32_t, 2>{12, 8});
    CHECK(detail::optical_flow_size(kWidth, kHeight, 2) == std::array<std::uint32_t, 2>{3, 2});
    CHECK(detail::optical_flow_size(1, 1, 6) == std::array<std::uint32_t, 2>{1, 1});

    detail::OpticalFlowHistory history;
    detail::OpticalFlow flow;
    // It moves 3 pixels right and 2 down a frame. The first frames count as scene changes, so
    // the search starts once there are frames enough.
    for (int frame = 0; frame < 8; ++frame) {
        const std::vector<std::uint8_t> rgba = moved_pattern(kWidth, kHeight, frame * 3, frame * 2);
        flow = detail::run_optical_flow(rgba, kWidth, kHeight, frame == 0, history);
    }
    REQUIRE(flow.width == 12);
    REQUIRE(flow.height == 8);
    REQUIRE(flow.vectors.size() == std::size_t{12} * 8 * 2);
    // The blocks away from the edges were 3 left and 2 up in the frame before.
    std::map<std::pair<int, int>, int> votes;
    for (std::uint32_t y = 1; y + 1 < flow.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < flow.width; ++x) {
            const std::size_t at = ((std::size_t{y} * flow.width) + x) * 2;
            ++votes[{flow.vectors[at], flow.vectors[at + 1]}];
        }
    }
    const int inner = 10 * 6;
    CHECK(votes[{-3, -2}] > inner * 3 / 4);
    // No scene change since the first frames.
    CHECK((flow.scene_history & 0xFU) == 0);

    // A different picture is a scene change.
    std::vector<std::uint8_t> other = moved_pattern(kWidth, kHeight, 0, 0);
    for (std::size_t i = 0; i < other.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            other[i + c] = static_cast<std::uint8_t>(other[i + c] / 4);
        }
    }
    flow = detail::run_optical_flow(other, kWidth, kHeight, false, history);
    CHECK((flow.scene_history & 1U) == 1);
}

TEST_CASE("Frame generation on the software device puts a moving square halfway") {
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 64, .height = 64, .render_target = true});
    auto into = device->create_texture({.width = 64, .height = 64, .render_target = true});
    auto mesh = device->create_mesh(square(), {});
    REQUIRE(target.ok());
    REQUIRE(into.ok());
    REQUIRE(mesh.ok());

    // A first frame has nothing to generate from, so it gives the frame as it is.
    draw_square_frame(*device, target.value(), mesh.value(), -0.75F, std::nullopt, true);
    REQUIRE(device->generate_frame(target.value(), into.value()).ok());
    CHECK(difference(read(*device, into.value()), read(*device, target.value())) == 0);

    // It moves 8 pixels right a frame (three eighths of a unit, at 64 pixels for 3 units).
    float x = -0.75F;
    Image before = read(*device, target.value());
    Image generated;
    for (int frame = 1; frame < 5; ++frame) {
        const float last = x;
        x += 0.375F;
        draw_square_frame(*device, target.value(), mesh.value(), x, last, true);
        REQUIRE(device->generate_frame(target.value(), into.value()).ok());
        generated = read(*device, into.value());
        const Image now = read(*device, target.value());
        CAPTURE(frame);
        const double halfway = (bright_x(before) + bright_x(now)) / 2.0;
        CHECK(std::abs(bright_x(generated) - halfway) < 1.0);
        CHECK(std::abs(bright_x(now) - bright_x(before) - 8.0) < 0.5);
        before = now;
    }
    // It is the square drawn halfway, near enough.
    const float last = x;
    x += 0.375F;
    draw_square_frame(*device, target.value(), mesh.value(), x, last, true);
    REQUIRE(device->generate_frame(target.value(), into.value()).ok());
    generated = read(*device, into.value());
    auto halfway = device->create_texture({.width = 64, .height = 64, .render_target = true});
    REQUIRE(halfway.ok());
    draw_square_frame(*device, halfway.value(), mesh.value(), x - 0.1875F, std::nullopt, false);
    const Image expected = read(*device, halfway.value());
    CHECK(differing(generated, expected, 60) < 16);
    CHECK(differing(generated, read(*device, target.value()), 60) > 40);

    // Generating again from the same frame has no frame before to generate from.
    REQUIRE(device->generate_frame(target.value(), into.value()).ok());
    CHECK(difference(read(*device, into.value()), read(*device, target.value())) == 0);

    // The same frames twice make the same frame.
    const auto again = render::create_software_device();
    auto target2 = again->create_texture({.width = 64, .height = 64, .render_target = true});
    auto into2 = again->create_texture({.width = 64, .height = 64, .render_target = true});
    auto mesh2 = again->create_mesh(square(), {});
    REQUIRE(target2.ok());
    REQUIRE(into2.ok());
    REQUIRE(mesh2.ok());
    draw_square_frame(*again, target2.value(), mesh2.value(), x - 0.375F, std::nullopt, true);
    REQUIRE(again->generate_frame(target2.value(), into2.value()).ok());
    draw_square_frame(*again, target2.value(), mesh2.value(), x, x - 0.375F, true);
    REQUIRE(again->generate_frame(target2.value(), into2.value()).ok());
    CHECK(difference(read(*again, into2.value()), generated) == 0);
}

TEST_CASE("Frame generation follows 2D draws by their optical flow") {
    const auto device = render::create_software_device();
    std::vector<std::uint8_t> texels = moved_pattern(64, 64, 0, 0);
    auto texture = device->create_texture({.width = 64, .height = 64}, texels);
    auto target = device->create_texture({.width = 64, .height = 64, .render_target = true});
    auto into = device->create_texture({.width = 64, .height = 64, .render_target = true});
    auto halfway = device->create_texture({.width = 64, .height = 64, .render_target = true});
    REQUIRE(texture.ok());
    REQUIRE(target.ok());
    REQUIRE(into.ok());
    REQUIRE(halfway.ok());
    // The picture moves 4 pixels right a frame, with no depth or motion vectors to go by. As
    // AMD's, it goes by the game's vectors (none here) until ten frames after the optical flow's
    // first frames, which count as scene changes, so it draws a while first.
    for (int frame = 0; frame < 24; ++frame) {
        draw_pattern_frame(*device, target.value(), texture.value(),
                           static_cast<float>(frame * 4) - 96.0F);
        REQUIRE(device->generate_frame(target.value(), into.value()).ok());
    }
    const Image generated = read(*device, into.value());
    const Image now = read(*device, target.value());
    draw_pattern_frame(*device, halfway.value(), texture.value(), (23.0F * 4.0F) - 98.0F);
    const Image expected = read(*device, halfway.value());
    draw_pattern_frame(*device, halfway.value(), texture.value(), (22.0F * 4.0F) - 96.0F);
    const Image before = read(*device, halfway.value());
    const std::size_t off = differing(generated, expected, 40);
    CAPTURE(off);
    CHECK(off < 300);
    CHECK(off < differing(generated, now, 40) / 8);
    CHECK(off < differing(generated, before, 40) / 8);
}

TEST_CASE("Frame generation refuses what it can't generate from") {
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 16, .height = 16, .render_target = true});
    auto small = device->create_texture({.width = 8, .height = 8, .render_target = true});
    const std::vector<std::uint8_t> texels(16 * 16 * 4, 255);
    auto plain = device->create_texture({.width = 16, .height = 16}, texels);
    REQUIRE(target.ok());
    REQUIRE(small.ok());
    REQUIRE(plain.ok());
    CHECK_FALSE(device->generate_frame(target.value(), target.value()).ok());
    CHECK_FALSE(device->generate_frame(target.value(), small.value()).ok());
    CHECK_FALSE(device->generate_frame(target.value(), plain.value()).ok());
    CHECK_FALSE(device->generate_frame(TextureId{999}, small.value()).ok());
    REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
    CHECK_FALSE(device->generate_frame(small.value(), small.value()).ok());
    REQUIRE(device->end_frame().ok());
    // Devices that don't generate frames say so.
    const auto null = render::create_null_device();
    CHECK_FALSE(null->generate_frame(TextureId{1}, TextureId{2}).ok());

    // Only Forward+ frames that aren't captures or path traced keep their prepass for it.
    render::Scene3D scene;
    scene.frame_generation = true;
    CHECK(detail::uses_frame_generation(scene));
    scene.method = render::RenderMethod::Compatibility;
    CHECK_FALSE(detail::uses_frame_generation(scene));
    scene.method = render::RenderMethod::ForwardPlus;
    scene.path_trace = render::PathTrace3D{};
    CHECK_FALSE(detail::uses_frame_generation(scene));
}
