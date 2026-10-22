#include "../engine/render/src/post_shading.hpp"
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
#include <string>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::PostProcess;
using render::RenderDevice;
using render::Tonemapper;
using render::Vertex3D;
using Float3 = std::array<float, 3>;

namespace {

void check_color(const Float3& got, const Float3& expected) {
    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(got.at(i) == doctest::Approx(expected.at(i)).epsilon(1e-4));
    }
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// A square facing the camera, half as wide as size, its corners clockwise from the front.
std::vector<Vertex3D> square(float half) {
    const auto corner = [&](float x, float y) {
        return Vertex3D{.position = {x * half, y * half, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 1.0F) * 0.5F, (1.0F - y) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// An orthogonal camera on +Z looking at the origin, seeing from -1 to 1 both ways.
render::Scene3D facing(const PostProcess& post) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(2.0F, 1.0F, 0.1F, 10.0F);
    scene.post = post;
    return scene;
}

// An unshaded square of color, half as wide as the view, over clear, size pixels across.
Image draw_post(RenderDevice& device, const PostProcess& post, const std::array<float, 4>& color,
                render::Color clear = {0.0F, 0.0F, 0.0F, 1.0F}, std::uint32_t size = 32) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(0.5F), {});
    REQUIRE(mesh.ok());
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = clear, .scene = facing(post)}).ok());
    constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(device.draw_mesh(mesh.value(), kIdentity, {.albedo = color, .unshaded = true}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? std::string() : image.error().message));
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

// A frame with no meshes, so the post pass draws over the clear color alone.
Image draw_clear(RenderDevice& device, const PostProcess& post, render::Color clear) {
    auto target = device.create_texture({.width = 8, .height = 8, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = clear, .scene = facing(post)}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? std::string() : image.error().message));
    device.destroy_texture(target.value());
    return image.value();
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// The middle of the square, and a pixel just outside its right edge.
constexpr std::uint32_t kInside = 16;
constexpr std::uint32_t kBeside = 26;

} // namespace

TEST_CASE("tonemappers map colors as Godot's do") {
    namespace detail = render::detail;
    CHECK(detail::srgb_to_linear({0.5F, 0.0F, 1.0F})[0] == doctest::Approx(0.21496747F));
    CHECK(detail::linear_to_srgb({0.5F, 0.0F, 1.0F})[0] == doctest::Approx(0.73535698F));
    CHECK(detail::linear_to_srgb({0.5F, 0.0F, 1.0F})[2] == doctest::Approx(1.0F).epsilon(1e-3));
    check_color(detail::tonemap({0.5F, 2.0F, -1.0F}, 1.0F, Tonemapper::Linear),
                {0.5F, 2.0F, -1.0F});
    check_color(detail::tonemap({1.0F, 0.5F, 4.0F}, 6.0F, Tonemapper::Reinhard),
                {0.51388889F, 0.33796296F, 0.88888889F});
    check_color(detail::tonemap({0.5F, 1.0F, 2.0F}, 1.0F, Tonemapper::Filmic),
                {0.69360417F, 1.0F, 1.25976499F});
    check_color(detail::tonemap({0.5F, 1.0F, 2.0F}, 4.0F, Tonemapper::Filmic),
                {0.48182210F, 0.69466436F, 0.87511384F});
    check_color(detail::tonemap({0.5F, 1.0F, 2.0F}, 1.0F, Tonemapper::Aces),
                {0.84624496F, 1.00136008F, 1.14465467F});
    check_color(detail::tonemap({0.18F, 1.0F, 4.0F}, 1.0F, Tonemapper::Agx),
                {0.3819249F, 0.62236737F, 0.8834042F});
}

TEST_CASE("glow's textures halve, and color correction textures are 1D or 3D") {
    namespace detail = render::detail;
    const auto sizes = detail::glow_sizes(64, 20);
    CHECK(sizes[0].width == 32);
    CHECK(sizes[0].height == 10);
    CHECK(sizes[1].height == 5);
    CHECK(sizes[2].height == 4);
    CHECK(sizes[3].width == 4);
    CHECK(sizes[3].height == 4);
    CHECK(detail::correction_kind(256, 1) == detail::kCorrection1D);
    CHECK(detail::correction_kind(256, 16) == detail::kCorrection3D);
    CHECK(detail::correction_kind(16, 16) == 0);
    CHECK_FALSE(detail::post_processes({}));
    CHECK_FALSE(detail::post_processes({.tonemapper = Tonemapper::Aces, .exposure = 2.0F}));
    CHECK(detail::post_processes({.glow = true}));
    CHECK(detail::post_processes({.adjustments = true}));
}

TEST_CASE("every device tonemaps and exposes meshes as they draw") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const std::array<float, 4> gray{0.6F, 0.6F, 0.6F, 1.0F};
        const Image linear = draw_post(*device, {}, gray);
        CHECK(std::abs(pixel(linear, kInside, kInside)[1] - 153) <= 1);
        // Twice the light is brighter; ACES maps 0.6 in sRGB (0.31 linear) up, Reinhard at a
        // white of 1 not at all.
        const Image exposed = draw_post(*device, {.exposure = 2.0F}, gray);
        CHECK(pixel(exposed, kInside, kInside)[1] > 190);
        const Image aces = draw_post(*device, {.tonemapper = Tonemapper::Aces}, gray);
        const Float3 expected = render::detail::linear_to_srgb(render::detail::tonemap(
            render::detail::srgb_to_linear({0.6F, 0.6F, 0.6F}), 1.0F, Tonemapper::Aces));
        CHECK(std::abs(pixel(aces, kInside, kInside)[1] -
                       static_cast<int>(std::lround(expected[1] * 255.0F))) <= 1);
        const Image reinhard = draw_post(*device, {.tonemapper = Tonemapper::Reinhard}, gray);
        CHECK(difference(reinhard, linear) <= 1);
        // The clear color isn't tonemapped.
        CHECK(pixel(exposed, 1, 1) == std::array<int, 4>{0, 0, 0, 255});
    }
}

TEST_CASE("every device blurs glow around what is bright") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const std::array<float, 4> white{1.0F, 1.0F, 1.0F, 1.0F};
        // Adjustments that change nothing go through the post pass and match drawing without it.
        const PostProcess neutral{.adjustments = true};
        const Image plain = draw_post(*device, {}, white);
        const Image passed = draw_post(*device, neutral, white);
        CHECK(difference(plain, passed) <= 2);
        CHECK(pixel(passed, kBeside, kInside)[0] == 0);

        // Glow from colors above a threshold under 1 spills past the square's edge.
        PostProcess glowing{.glow = true,
                            .glow_intensity = 1.0F,
                            .glow_hdr_threshold = 0.5F,
                            .glow_hdr_scale = 0.1F};
        const Image glow = draw_post(*device, glowing, white);
        CAPTURE(pixel(glow, kBeside, kInside)[0]);
        CHECK(pixel(glow, kBeside, kInside)[0] > 20);
        CHECK(pixel(glow, kInside, kInside)[0] == 255);
        // Nothing reaches the threshold of 1, so nothing glows; bloom makes everything glow.
        glowing.glow_hdr_threshold = 1.0F;
        CHECK(pixel(draw_post(*device, glowing, white), kBeside, kInside)[0] == 0);
        glowing.glow_bloom = 1.0F;
        CHECK(pixel(draw_post(*device, glowing, white), kBeside, kInside)[0] > 20);
    }
}

TEST_CASE("every device adjusts and corrects colors after tonemapping") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const render::Color color{0.75F, 0.25F, 0.5F, 1.0F};
        const auto middle = [](const Image& image) { return pixel(image, 4, 4); };
        // Brightness scales, saturation 0 is gray and contrast 0 is middle gray.
        const std::array<int, 4> half =
            middle(draw_clear(*device, {.adjustments = true, .brightness = 0.5F}, color));
        CHECK(std::abs(half[0] - 96) <= 2);
        CHECK(std::abs(half[1] - 32) <= 2);
        const std::array<int, 4> gray =
            middle(draw_clear(*device, {.adjustments = true, .saturation = 0.0F}, color));
        CHECK(std::abs(gray[0] - 128) <= 2);
        CHECK(std::abs(gray[1] - gray[0]) <= 1);
        CHECK(std::abs(gray[2] - gray[0]) <= 1);
        const std::array<int, 4> flat =
            middle(draw_clear(*device, {.adjustments = true, .contrast = 0.0F}, color));
        CHECK(std::abs(flat[0] - 128) <= 1);
        CHECK(std::abs(flat[2] - 128) <= 1);

        // A 1D lookup texture reads each channel at its own value: red rises, green falls and
        // blue is 0.
        auto lut_1d =
            device->create_texture({.width = 2, .height = 1, .filter = render::Filter::Linear},
                                   std::vector<std::uint8_t>{0, 255, 0, 255, 255, 0, 0, 255});
        REQUIRE(lut_1d.ok());
        const std::array<int, 4> mapped = middle(
            draw_clear(*device, {.adjustments = true, .color_correction = lut_1d.value()}, color));
        CHECK(mapped[0] >= 253);
        CHECK(mapped[1] >= 253);
        CHECK(mapped[2] == 0);
        // Without adjustments, the texture is left alone.
        CHECK(middle(draw_clear(*device, {.glow = true, .color_correction = lut_1d.value()},
                                color))[1] < 70);

        // A 3D lookup texture of size 2 whose texels are their own coordinates: red along x,
        // green along y and blue across the slices.
        std::vector<std::uint8_t> texels;
        for (int y = 0; y < 2; ++y) {
            for (int slice = 0; slice < 2; ++slice) {
                for (int x = 0; x < 2; ++x) {
                    for (const int value : {x * 255, y * 255, slice * 255, 255}) {
                        texels.push_back(static_cast<std::uint8_t>(value));
                    }
                }
            }
        }
        auto lut_3d = device->create_texture(
            {.width = 4, .height = 2, .filter = render::Filter::Linear}, texels);
        REQUIRE(lut_3d.ok());
        const std::array<int, 4> looked = middle(
            draw_clear(*device, {.adjustments = true, .color_correction = lut_3d.value()}, color));
        // Each channel c is read at texel 2c - 0.5: 0.75 at the last, 0.25 at the first.
        CHECK(looked[0] >= 252);
        CHECK(looked[1] <= 3);
        CHECK(std::abs(looked[2] - 128) <= 3);
        device->destroy_texture(lut_1d.value());
        device->destroy_texture(lut_3d.value());
    }
}

TEST_CASE("GPUs post-process as the software device does") {
    PostProcess post{.tonemapper = Tonemapper::Filmic,
                     .exposure = 1.5F,
                     .white = 2.0F,
                     .glow = true,
                     .glow_intensity = 0.6F,
                     .glow_bloom = 0.1F,
                     .glow_hdr_threshold = 0.6F,
                     .adjustments = true,
                     .brightness = 1.1F,
                     .contrast = 1.2F,
                     .saturation = 0.8F};
    const std::array<float, 4> color{0.9F, 0.7F, 0.3F, 1.0F};
    const render::Color clear{0.1F, 0.2F, 0.3F, 1.0F};
    const Image expected = draw_post(*render::create_software_device(), post, color, clear, 48);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image image = draw_post(*device, post, color, clear, 48);
        CHECK(difference(image, expected) <= 3);
    }
}

TEST_CASE("devices check post-processing settings") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto target = device->create_texture({.width = 4, .height = 4, .render_target = true});
        REQUIRE(target.ok());
        const auto refused = [&](const PostProcess& post) {
            auto began =
                device->begin_frame({.target = target.value(), .clear = {}, .scene = facing(post)});
            REQUIRE_FALSE(began.ok());
            return began.error().message;
        };
        CHECK(refused({.tonemapper = static_cast<Tonemapper>(9)}).find("one of five") !=
              std::string::npos);
        CHECK(refused({.exposure = std::nanf("")}).find("finite") != std::string::npos);
        CHECK(
            refused({.adjustments = true, .color_correction = render::TextureId{77}}).find("77") !=
            std::string::npos);
        auto square_lut = device->create_texture({.width = 4, .height = 4});
        REQUIRE(square_lut.ok());
        CHECK(refused({.adjustments = true, .color_correction = square_lut.value()})
                  .find("not 4x4") != std::string::npos);
        device->destroy_texture(square_lut.value());
        device->destroy_texture(target.value());
    }
}
