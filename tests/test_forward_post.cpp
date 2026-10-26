#include "../engine/render/src/forward_post.hpp"
#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::GlowBlendMode;
using render::Image;
using render::Material3D;
using render::PostProcess;
using render::RenderDevice;
using render::RenderMethod;
using render::Tonemapper;
using render::Vertex3D;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// A square facing the camera, half as wide as the view, its corners clockwise from the front.
std::vector<Vertex3D> square() {
    const auto corner = [](float x, float y) {
        return Vertex3D{.position = {x * 0.5F, y * 0.5F, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 1.0F) * 0.5F, (1.0F - y) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// A square of material in the middle of a frame size pixels across, over clear, seen by an
// orthogonal camera on +Z that sees from -1 to 1 both ways.
Image draw(RenderDevice& device, const PostProcess& post, const Material3D& material,
           render::Color clear = {0.0F, 0.0F, 0.0F, 1.0F}, std::uint32_t size = 32,
           RenderMethod method = RenderMethod::ForwardPlus) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(), {});
    REQUIRE(mesh.ok());
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(2.0F, 1.0F, 0.1F, 10.0F);
    scene.post = post;
    REQUIRE(device.begin_frame({.target = target.value(), .clear = clear, .scene = scene}).ok());
    constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(device.draw_mesh(mesh.value(), kIdentity, material).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

// A square that shines with light four times white's, lit by nothing else.
const Material3D kBright{
    .albedo = {0.0F, 0.0F, 0.0F, 1.0F}, .emission = {1.0F, 1.0F, 1.0F}, .emission_energy = 4.0F};

// A square two and a half times as bright as white, whose glow stays below white.
const Material3D kDim{
    .albedo = {0.0F, 0.0F, 0.0F, 1.0F}, .emission = {1.0F, 1.0F, 1.0F}, .emission_energy = 2.5F};

// The middle of a 32-pixel frame's square, and a pixel just outside its right edge.
constexpr std::uint32_t kInside = 16;
constexpr std::uint32_t kBeside = 26;

} // namespace

TEST_CASE("half floats round as a GPU stores them") {
    CHECK(detail::round_half(1.0F) == 1.0F);
    CHECK(detail::round_half(0.1F) == 0.0999755859375F);
    // Ties go to the even neighbor: from 2048 the step is 2.
    CHECK(detail::round_half(2049.0F) == 2048.0F);
    CHECK(detail::round_half(2051.0F) == 2052.0F);
    CHECK(detail::round_half(-2051.0F) == -2052.0F);
    // The largest half is 65504; from 65520 up, values round to infinity.
    CHECK(detail::round_half(65519.0F) == 65504.0F);
    CHECK(std::isinf(detail::round_half(65520.0F)));
    // Below 2^-14 halves step by 2^-24, and below half of that they are 0, keeping their sign.
    CHECK(detail::round_half(1.0e-5F) == std::ldexp(168.0F, -24));
    CHECK(detail::round_half(4.0e-8F) == std::ldexp(1.0F, -24));
    CHECK(detail::round_half(-2.0e-8F) == 0.0F);
    CHECK(std::signbit(detail::round_half(-2.0e-8F)));
    CHECK(std::isnan(detail::round_half(std::nanf(""))));
    // The clear color is made linear by the exact sRGB curve, then stored as halves.
    const detail::Float4 clear = detail::forward_clear({0.5F, 0.0F, 1.0F, 0.25F});
    CHECK(clear[0] == detail::round_half(0.21404114F));
    CHECK(clear[1] == 0.0F);
    CHECK(clear[2] == 1.0F);
    CHECK(clear[3] == 0.25F);
}

TEST_CASE("glow's levels halve from half the frame's size, as far as their weights reach") {
    PostProcess post{.glow = true};
    detail::GlowPlan plan = detail::forward_glow_plan(post, 64, 48);
    CHECK(plan.sizes[0].width == 32);
    CHECK(plan.sizes[0].height == 24);
    CHECK(plan.sizes[1].width == 16);
    CHECK(plan.sizes[3].height == 3);
    CHECK(plan.sizes[6].width == 1);
    CHECK(plan.sizes[6].height == 1);
    // Godot's default levels end at the fourth.
    CHECK(plan.count == 4);
    // A 64-pixel frame has six halvings, so glow draws at most five levels.
    post.glow_levels[6] = 1.0F;
    CHECK(detail::forward_glow_plan(post, 64, 48).count == 5);
    // Levels of 0.01 or less aren't drawn, but the first always is.
    post.glow_levels = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.01F, 0.0F};
    CHECK(detail::forward_glow_plan(post, 64, 48).count == 1);
    post.glow_levels = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    CHECK(detail::forward_glow_plan(post, 4, 4).count == 1);
    CHECK(detail::forward_glow_plan(post, 1024, 512).count == 7);
}

TEST_CASE("Forward+ tonemaps with Godot 4.6's whites") {
    const auto white = [](const PostProcess& post) {
        return detail::forward_post_uniforms(post, detail::ForwardPass::Tonemap, 8, 8, 8, 8)
            .tonemap[1];
    };
    // Linear ignores white; the others keep it at least 1, AgX at least 2.
    CHECK(white({.white = 6.0F}) == 1.0F);
    CHECK(white({.tonemapper = Tonemapper::Reinhard, .white = 0.5F}) == 1.0F);
    CHECK(white({.tonemapper = Tonemapper::Filmic, .white = 6.0F}) == 6.0F);
    CHECK(white({.tonemapper = Tonemapper::Agx, .white = 1.0F}) == 2.0F);
    CHECK(white({.tonemapper = Tonemapper::Agx, .white = 16.29F}) == 16.29F);
    // The mix blend mode blends by glow_mix in place of the intensity.
    const PostProcess mix{.glow = true,
                          .glow_intensity = 0.8F,
                          .glow_mix = 0.25F,
                          .glow_blend_mode = GlowBlendMode::Mix};
    CHECK(detail::forward_post_uniforms(mix, detail::ForwardPass::Tonemap, 8, 8, 8, 8)
              .levels_high[3] == 0.25F);
}

TEST_CASE("a Forward+ frame clears linear and comes back the color it cleared to") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const render::Color clear{0.2F, 0.4F, 0.6F, 1.0F};
        const std::array<int, 4> corner = pixel(draw(*device, {}, {}, clear), 1, 1);
        CHECK(std::abs(corner[0] - 51) <= 1);
        CHECK(std::abs(corner[1] - 102) <= 1);
        CHECK(std::abs(corner[2] - 153) <= 1);
        CHECK(corner[3] == 255);
    }
}

TEST_CASE("Forward+ blends alpha in linear colors, Compatibility in sRGB") {
    const Material3D half{.albedo = {1.0F, 1.0F, 1.0F, 0.5F},
                          .unshaded = true,
                          .transparency = render::Transparency::Alpha};
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // Half of linear white is 0.5, which is 188 in sRGB.
        CHECK(std::abs(pixel(draw(*device, {}, half), kInside, kInside)[0] - 188) <= 1);
        const Image compatibility =
            draw(*device, {}, half, {0.0F, 0.0F, 0.0F, 1.0F}, 32, RenderMethod::Compatibility);
        CHECK(std::abs(pixel(compatibility, kInside, kInside)[0] - 128) <= 1);
    }
}

TEST_CASE("Forward+ glows from colors brighter than white") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // The square is four times white: clamped to 255, and with no glow nothing is beside it.
        const Image plain = draw(*device, {}, kBright);
        CHECK(pixel(plain, kInside, kInside)[0] == 255);
        CHECK(pixel(plain, kBeside, kInside)[0] == 0);
        // Above the threshold of 1, it glows past its edge.
        const PostProcess glowing{.glow = true, .glow_intensity = 1.0F};
        const Image glow = draw(*device, glowing, kBright);
        CAPTURE(pixel(glow, kBeside, kInside)[0]);
        CHECK(pixel(glow, kBeside, kInside)[0] > 60);
        // Compatibility's colors stop at 1, so the same settings glow nowhere.
        const Image compatibility = draw(*device, glowing, kBright, {0.0F, 0.0F, 0.0F, 1.0F}, 32,
                                         RenderMethod::Compatibility);
        CHECK(pixel(compatibility, kBeside, kInside)[0] == 0);
        // A white square doesn't pass the threshold either, unless the threshold is lower.
        const Material3D white{.unshaded = true};
        CHECK(pixel(draw(*device, glowing, white), kBeside, kInside)[0] == 0);
        PostProcess low = glowing;
        low.glow_hdr_threshold = 0.5F;
        CHECK(pixel(draw(*device, low, white), kBeside, kInside)[0] > 20);
        // Strength multiplies each level as it blurs; a level of 0 adds nothing.
        PostProcess strong = glowing;
        strong.glow_strength = 2.0F;
        CHECK(pixel(draw(*device, strong, kDim), kBeside, kInside)[0] >
              pixel(draw(*device, glowing, kDim), kBeside, kInside)[0]);
        PostProcess none = glowing;
        none.glow_levels = {};
        CHECK(pixel(draw(*device, none, kBright), kBeside, kInside)[0] == 0);
    }
}

TEST_CASE("Forward+ blends glow in by its blend mode") {
    const render::Color gray{0.5F, 0.5F, 0.5F, 1.0F};
    // Only the sharpest level, so glow stays right by the square of a 64-pixel frame.
    PostProcess post{.glow = true,
                     .glow_intensity = 1.0F,
                     .glow_levels = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}};
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const auto beside = [&](GlowBlendMode mode, float mix = 0.05F) {
            PostProcess blended = post;
            blended.glow_blend_mode = mode;
            blended.glow_mix = mix;
            return draw(*device, blended, kDim, gray, 64);
        };
        // The first pixel past the square's right edge: the sharpest level reaches a few pixels.
        constexpr std::uint32_t kEdge = 48;
        constexpr std::uint32_t kMiddle = 32;
        const Image additive = beside(GlowBlendMode::Additive);
        const Image screen = beside(GlowBlendMode::Screen);
        const Image softlight = beside(GlowBlendMode::Softlight);
        const Image replace = beside(GlowBlendMode::Replace);
        // Adding is brighter than screening, which is brighter than the gray alone.
        CHECK(pixel(additive, kEdge, kMiddle)[0] > pixel(screen, kEdge, kMiddle)[0]);
        CHECK(pixel(screen, kEdge, kMiddle)[0] > 128);
        CHECK(pixel(softlight, kEdge, kMiddle)[0] > 128);
        // Far from the square, glow adds nothing to the gray, and replaces it with black.
        CHECK(std::abs(pixel(screen, 1, 1)[0] - 128) <= 1);
        CHECK(std::abs(pixel(additive, 1, 1)[0] - 128) <= 1);
        CHECK(pixel(replace, 1, 1)[0] == 0);
        CHECK(pixel(replace, kEdge, kMiddle)[0] > 0);
        // A mix of 0 leaves the frame as it was; a mix of 1 is glow alone.
        PostProcess off = post;
        off.glow = false;
        CHECK(difference(beside(GlowBlendMode::Mix, 0.0F), draw(*device, off, kDim, gray, 64)) <=
              1);
        CHECK(pixel(beside(GlowBlendMode::Mix, 1.0F), 1, 1)[0] == 0);
    }
}

TEST_CASE("GPUs post-process Forward+ as the software device does") {
    const std::array<PostProcess, 3> posts{
        PostProcess{.tonemapper = Tonemapper::Filmic,
                    .exposure = 1.5F,
                    .white = 4.0F,
                    .glow = true,
                    .glow_intensity = 0.6F,
                    .glow_blend_mode = GlowBlendMode::Additive,
                    .glow_bloom = 0.1F},
        PostProcess{.tonemapper = Tonemapper::Agx,
                    .white = 16.29F,
                    .glow = true,
                    .glow_levels = {0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F},
                    .glow_strength = 1.3F,
                    .glow_blend_mode = GlowBlendMode::Softlight,
                    .adjustments = true,
                    .brightness = 1.1F,
                    .contrast = 1.2F,
                    .saturation = 0.8F},
        PostProcess{.tonemapper = Tonemapper::Aces,
                    .white = 2.0F,
                    .glow = true,
                    .glow_mix = 0.3F,
                    .glow_blend_mode = GlowBlendMode::Mix,
                    .glow_hdr_threshold = 0.8F}};
    const render::Color clear{0.1F, 0.2F, 0.3F, 1.0F};
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        return;
    }
    CAPTURE(render::backend_name(gpu->backend()));
    for (std::size_t i = 0; i < posts.size(); ++i) {
        CAPTURE(i);
        const Image expected =
            draw(*render::create_software_device(), posts.at(i), kBright, clear, 48);
        const Image image = draw(*gpu, posts.at(i), kBright, clear, 48);
        CHECK(difference(image, expected) <= 3);
    }
}

TEST_CASE("auto exposure measures a frame in blocks of 8, as Godot's luminance_reduce") {
    using detail::PostSize;
    const auto sizes = [](std::uint32_t width, std::uint32_t height) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        for (const PostSize size : detail::luminance_plan(width, height)) {
            out.emplace_back(size.width, size.height);
        }
        return out;
    };
    using Sizes = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    CHECK(sizes(32, 32) == Sizes{{4, 4}, {1, 1}});
    CHECK(sizes(1000, 600) == Sizes{{125, 75}, {15, 9}, {1, 1}});
    CHECK(sizes(100, 30) == Sizes{{12, 3}, {1, 1}});
    CHECK(sizes(7, 5) == Sizes{{1, 1}});

    // A frame of 20 x 9 measures blocks of its first 16 x 8 pixels, each pixel by its brightest
    // channel; the rest are left out.
    detail::HdrImage world{
        .width = 20, .height = 9, .pixels = std::vector<float>(std::size_t{20} * 9 * 4)};
    for (std::uint32_t y = 0; y < 9; ++y) {
        for (std::uint32_t x = 0; x < 20; ++x) {
            const std::size_t at = ((std::size_t{y} * 20) + x) * 4;
            const bool left_out = x >= 16 || y == 8;
            world.pixels[at] = left_out ? 100.0F : (x < 8 ? 0.5F : 0.0F);
            world.pixels[at + 1] = x >= 8 && !left_out ? 2.0F : 0.0F;
            world.pixels[at + 2] = 0.1F;
            world.pixels[at + 3] = 1.0F;
        }
    }
    PostProcess post{.auto_exposure = true, .auto_exposure_max = 10.0F};
    detail::LuminanceImage blocks;
    detail::run_luminance_pass(
        detail::luminance_uniforms(post, detail::ForwardPass::LuminanceFirst, {20, 9}, {2, 1}),
        world, blocks);
    CHECK(blocks.values == std::vector<float>{0.5F, 2.0F});

    // The last pass moves auto_exposure_adjust of the way from the last frame's brightness to the
    // average, 1.25, and keeps it from the least to the most.
    const auto last = [&](float previous) {
        detail::LuminanceImage out;
        detail::run_luminance_pass(
            detail::luminance_uniforms(post, detail::ForwardPass::LuminanceFinal, {2, 1}, {1, 1}),
            blocks, previous, out);
        REQUIRE(out.values.size() == 1);
        return out.values[0];
    };
    post.auto_exposure_adjust = 0.5F;
    CHECK(last(0.25F) == 0.75F);
    post.auto_exposure_max = 0.6F;
    CHECK(last(0.25F) == 0.6F);
    post.auto_exposure_max = 10.0F;
    post.auto_exposure_min = 0.9F;
    CHECK(last(0.25F) == 0.9F);
    // A pass before the last averages alone.
    detail::LuminanceImage average;
    detail::run_luminance_pass(
        detail::luminance_uniforms(post, detail::ForwardPass::Luminance, {2, 1}, {1, 1}), blocks,
        0.25F, average);
    CHECK(average.values == std::vector<float>{1.25F});
}

TEST_CASE("Forward+ auto exposure brightens a dark frame and darkens a bright one") {
    // The 32-pixel frame's square covers a quarter of it, so the frame's brightness is a quarter
    // of the square's, and a scale of 0.1 shows the square at 0.4 whatever its brightness.
    const auto exposed = [](std::uint64_t version, float adjust = 1.0F) {
        return PostProcess{.auto_exposure = true,
                           .auto_exposure_scale = 0.1F,
                           .auto_exposure_max = 10.0F,
                           .auto_exposure_adjust = adjust,
                           .auto_exposure_version = version};
    };
    const auto shining = [](float energy) {
        return Material3D{.albedo = {0.0F, 0.0F, 0.0F, 1.0F},
                          .emission = {1.0F, 1.0F, 1.0F},
                          .emission_energy = energy};
    };
    const auto red = [](const Image& image) { return pixel(image, kInside, kInside)[0]; };
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        CHECK(red(draw(*device, {}, shining(0.5F))) == 188);
        CHECK(red(draw(*device, {}, shining(4.0F))) == 255);
        CHECK(std::abs(red(draw(*device, exposed(1), shining(0.5F))) - 170) <= 1);
        CHECK(std::abs(red(draw(*device, exposed(2), shining(4.0F))) - 170) <= 1);
        // The next frame of the same version moves half way from the bright frame's brightness,
        // 1, toward the dim one's, 0.125: 0.5625, which shows the square at 0.0889.
        CHECK(std::abs(red(draw(*device, exposed(2, 0.5F), shining(0.5F))) - 84) <= 1);
        // A new version takes the frame's own brightness, however little it would move.
        CHECK(std::abs(red(draw(*device, exposed(3, 0.0F), shining(0.5F))) - 170) <= 1);
        // The brightness stays from the least to the most, from the version's second frame.
        PostProcess capped = exposed(4);
        capped.auto_exposure_max = 0.05F;
        (void)draw(*device, capped, shining(4.0F));
        CHECK(red(draw(*device, capped, shining(4.0F))) == 255);
        PostProcess floored = exposed(5);
        floored.auto_exposure_min = 2.0F;
        (void)draw(*device, floored, shining(0.5F));
        CHECK(std::abs(red(draw(*device, floored, shining(0.5F))) - 44) <= 1);
        // Glow's first level is exposed the same way: four times white, over a scale of 0.1 by a
        // brightness of 1, is 0.4, below the threshold.
        PostProcess glowing = exposed(6);
        glowing.glow = true;
        glowing.glow_intensity = 1.0F;
        CHECK(pixel(draw(*device, glowing, kBright), kBeside, kInside)[0] == 0);
        glowing.auto_exposure = false;
        CHECK(pixel(draw(*device, glowing, kBright), kBeside, kInside)[0] > 60);
    }
}

TEST_CASE("GPUs measure auto exposure as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        return;
    }
    CAPTURE(render::backend_name(gpu->backend()));
    const auto software = render::create_software_device();
    const render::Color clear{0.1F, 0.2F, 0.3F, 1.0F};
    PostProcess post{.tonemapper = Tonemapper::Filmic,
                     .glow = true,
                     .glow_intensity = 0.6F,
                     .auto_exposure = true,
                     .auto_exposure_scale = 0.3F,
                     .auto_exposure_max = 4.0F,
                     .auto_exposure_version = 7};
    // A frame big enough for two luminance levels and a remainder, then frames that move toward a
    // dimmer square.
    const std::array<std::pair<float, float>, 3> frames{{{1.0F, 4.0F}, {0.3F, 1.5F}, {0.3F, 1.5F}}};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CAPTURE(i);
        post.auto_exposure_adjust = frames.at(i).first;
        const Material3D material{.albedo = {0.0F, 0.0F, 0.0F, 1.0F},
                                  .emission = {1.0F, 0.5F, 0.25F},
                                  .emission_energy = frames.at(i).second};
        const Image expected = draw(*software, post, material, clear, 75);
        const Image image = draw(*gpu, post, material, clear, 75);
        CHECK(difference(image, expected) <= 3);
    }
}
