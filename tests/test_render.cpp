#include "slash/platform/platform.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"

#include <SDL3/SDL_hints.h>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using slash::render::Color;
using slash::render::create_device;
using slash::render::create_null_device;
using slash::render::Filter;
using slash::render::Frame;
using slash::render::Image;
using slash::render::Mat4;
using slash::render::pixel_projection;
using slash::render::RenderDevice;
using slash::render::TextureDesc;
using slash::render::TextureId;
using slash::render::Vertex2D;
using slash::render::VsyncMode;

namespace {

using Rgba = std::array<std::uint8_t, 4>;

// Two triangles covering a rectangle, with the whole texture stretched over it.
std::vector<Vertex2D> rect(float x0, float y0, float x1, float y1,
                           Rgba rgba = {255, 255, 255, 255}) {
    const Vertex2D top_left{.x = x0, .y = y0, .u = 0.0F, .v = 0.0F, .rgba = rgba};
    const Vertex2D top_right{.x = x1, .y = y0, .u = 1.0F, .v = 0.0F, .rgba = rgba};
    const Vertex2D bottom_left{.x = x0, .y = y1, .u = 0.0F, .v = 1.0F, .rgba = rgba};
    const Vertex2D bottom_right{.x = x1, .y = y1, .u = 1.0F, .v = 1.0F, .rgba = rgba};
    return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

TextureDesc target_desc(std::uint32_t size) {
    return {.width = size, .height = size, .filter = Filter::Linear, .render_target = true};
}

// "" when the pixel at (x, y) is within 1 of expected in every channel, else what differs.
std::string pixel_is(const Image& image, std::uint32_t x, std::uint32_t y, Rgba expected) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    std::string actual;
    bool close = true;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const int value = image.pixels.at(at + channel);
        close = close && std::abs(value - int{expected.at(channel)}) <= 1;
        actual += (channel == 0 ? "" : ",") + std::to_string(value);
    }
    if (close) {
        return "";
    }
    return "pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + actual +
           ", expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," +
           std::to_string(expected[2]) + "," + std::to_string(expected[3]);
}

// The error a call failed with, or "(succeeded)".
template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

// A GPU device with no window, or null when this machine has no GPU backend. CI sets
// SLASH_EXPECT_GPU where one works (software Vulkan on Linux), so a broken GPU path fails there
// instead of being skipped.
std::unique_ptr<RenderDevice> gpu_device() {
    auto device = create_device({.window = nullptr, .driver = "auto", .debug = false});
    if (!device) {
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works: " << device.error().message);
        }
        MESSAGE("no GPU here, so GPU checks are skipped: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

Image render(RenderDevice& device, TextureId target, const Frame& frame,
             const std::vector<std::pair<TextureId, std::vector<Vertex2D>>>& draws) {
    Frame with_target = frame;
    with_target.target = target;
    auto began = device.begin_frame(with_target);
    REQUIRE(began.ok());
    REQUIRE(began.value());
    for (const auto& [texture, vertices] : draws) {
        REQUIRE(device.draw(texture, vertices).ok());
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target);
    REQUIRE(image.ok());
    return std::move(image).value();
}

// Both devices refuse the same mistakes with the same errors.
void check_refusals(RenderDevice& device) {
    CHECK(error_of(device.create_texture(
              {.width = 0, .height = 4, .filter = Filter::Linear, .render_target = false})) ==
          "a texture is 1 to 16384 pixels wide and high, not 0x4");
    const std::vector<std::uint8_t> short_pixels(15);
    CHECK(error_of(device.create_texture(target_desc(2), short_pixels)) ==
          "a 2x2 texture takes 16 bytes of RGBA, not 15");

    auto plain = device.create_texture(
        {.width = 2, .height = 2, .filter = Filter::Nearest, .render_target = false});
    REQUIRE(plain.ok());
    auto target = device.create_texture(target_desc(4));
    REQUIRE(target.ok());

    CHECK(error_of(device.draw(TextureId::None, rect(0, 0, 1, 1))) ==
          "draw needs a frame; call begin_frame first");
    CHECK(error_of(device.end_frame()) == "end_frame needs a frame; call begin_frame first");
    CHECK(error_of(device.begin_frame({.target = plain.value(), .clear = {}, .view = {}})) ==
          "texture 1 is not a render target; create it with render_target set");
    CHECK(error_of(device.begin_frame(
              {.target = static_cast<TextureId>(99), .clear = {}, .view = {}})) == "no texture 99");
    CHECK(error_of(device.read_texture(plain.value())) ==
          "texture 1 is not a render target; create it with render_target set");

    auto began = device.begin_frame({.target = target.value(), .clear = {}, .view = {}});
    REQUIRE(began.ok());
    CHECK(error_of(device.begin_frame({.target = target.value(), .clear = {}, .view = {}})) ==
          "begin_frame can't run during a frame; call end_frame first");
    std::vector<Vertex2D> four = rect(0, 0, 1, 1);
    four.resize(4);
    CHECK(error_of(device.draw(TextureId::None, four)) ==
          "every three vertices make a triangle, so 4 vertices is not a whole number of triangles");
    CHECK(error_of(device.draw(target.value(), rect(0, 0, 1, 1))) ==
          "texture 2 is this frame's target, so it can't be drawn with until the frame ends");
    CHECK(error_of(device.draw(static_cast<TextureId>(99), rect(0, 0, 1, 1))) == "no texture 99");
    CHECK(error_of(device.read_texture(target.value())) ==
          "read_texture can't run during a frame; call end_frame first");
    CHECK(device.end_frame().ok());

    CHECK(error_of(device.update_texture(plain.value(), short_pixels)) ==
          "a 2x2 texture takes 16 bytes of RGBA, not 15");
    device.destroy_texture(plain.value());
    CHECK(error_of(device.update_texture(plain.value(), {})) == "no texture 1");
    CHECK(device.stats().textures == 1);
}

} // namespace

TEST_CASE("create_device knows its drivers") {
    auto device = create_device({.window = nullptr, .driver = "vulkn", .debug = false});
    REQUIRE_FALSE(device.ok());
    CHECK(device.error().message ==
          "no render driver \"vulkn\"; use auto, null, vulkan, metal or d3d12; did you mean "
          "\"vulkan\"?");

    auto null = create_device({.window = nullptr, .driver = "null", .debug = false});
    REQUIRE(null.ok());
    CHECK(null.value()->backend() == slash::render::Backend::Null);
}

TEST_CASE("the null device keeps textures, counts batched draws and reads back its clear color") {
    auto device = create_null_device();
    auto texture = device->create_texture(
        {.width = 1, .height = 1, .filter = Filter::Nearest, .render_target = false},
        std::vector<std::uint8_t>{1, 2, 3, 4});
    REQUIRE(texture.ok());
    auto target = device->create_texture(target_desc(2));
    REQUIRE(target.ok());
    CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
          "the null device has no window; draw to a render-target texture");

    const Image image = render(*device, target.value(),
                               {.target = {}, .clear = {1.0F, 0.5F, 0.0F, 1.0F}, .view = {}},
                               {{TextureId::None, rect(0, 0, 1, 1)},
                                {TextureId::None, rect(1, 1, 2, 2)},
                                {texture.value(), rect(0, 0, 2, 2)},
                                {TextureId::None, {}}});
    CHECK(image.width == 2);
    CHECK(pixel_is(image, 1, 1, {255, 128, 0, 255}).empty());
    const auto stats = device->stats();
    CHECK(stats.frames == 1);
    CHECK(stats.draws == 2); // the two untextured draws share one
    CHECK(stats.triangles == 6);
    CHECK(stats.textures == 2);
}

TEST_CASE("the null device refuses mistakes") {
    auto device = create_null_device();
    check_refusals(*device);
}

TEST_CASE("pixel_projection maps a target's pixels to clip space") {
    const Mat4 m = pixel_projection(200.0F, 100.0F);
    const auto clip = [&](float x, float y) {
        return std::array<float, 2>{(m[0] * x) + (m[4] * y) + m[12],
                                    (m[1] * x) + (m[5] * y) + m[13]};
    };
    CHECK(clip(0, 0) == std::array<float, 2>{-1.0F, 1.0F});
    CHECK(clip(200, 100) == std::array<float, 2>{1.0F, -1.0F});
    CHECK(clip(100, 50) == std::array<float, 2>{0.0F, 0.0F});
}

TEST_CASE("the GPU device draws, blends, samples and reads back pixels") {
    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    auto target = device->create_texture(target_desc(8));
    REQUIRE(target.ok());

    SUBCASE("a new render target is transparent black") {
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel_is(image.value(), 3, 3, {0, 0, 0, 0}).empty());
    }

    SUBCASE("clearing and drawing plain color") {
        const Image image = render(*device, target.value(),
                                   {.target = {}, .clear = {0.25F, 0.5F, 0.75F, 1.0F}, .view = {}},
                                   {{TextureId::None, rect(0, 0, 4, 8, {255, 0, 0, 255})}});
        CHECK(pixel_is(image, 1, 1, {255, 0, 0, 255}).empty());
        CHECK(pixel_is(image, 3, 7, {255, 0, 0, 255}).empty());
        CHECK(pixel_is(image, 4, 0, {64, 128, 191, 255}).empty());
        CHECK(pixel_is(image, 7, 7, {64, 128, 191, 255}).empty());
    }

    SUBCASE("blending straight alpha over the clear color") {
        const Image image = render(*device, target.value(),
                                   {.target = {}, .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}},
                                   {{TextureId::None, rect(0, 0, 8, 8, {255, 255, 255, 128})}});
        CHECK(pixel_is(image, 4, 4, {128, 128, 128, 255}).empty());
    }

    SUBCASE("sampling a texture with nearest filtering") {
        // Red, green / blue, white: each texel fills a quarter of the target.
        const std::vector<std::uint8_t> texels{255, 0, 0,   255, 0,   255, 0,   255,
                                               0,   0, 255, 255, 255, 255, 255, 255};
        auto texture = device->create_texture(
            {.width = 2, .height = 2, .filter = Filter::Nearest, .render_target = false}, texels);
        REQUIRE(texture.ok());
        Image image = render(*device, target.value(), {}, {{texture.value(), rect(0, 0, 8, 8)}});
        CHECK(pixel_is(image, 1, 1, {255, 0, 0, 255}).empty());
        CHECK(pixel_is(image, 6, 1, {0, 255, 0, 255}).empty());
        CHECK(pixel_is(image, 1, 6, {0, 0, 255, 255}).empty());
        CHECK(pixel_is(image, 6, 6, {255, 255, 255, 255}).empty());

        // The vertex color multiplies the texture, and an update shows in the next frame.
        const std::vector<std::uint8_t> gray{200, 200, 200, 255, 200, 200, 200, 255,
                                             200, 200, 200, 255, 200, 200, 200, 255};
        REQUIRE(device->update_texture(texture.value(), gray).ok());
        image = render(*device, target.value(), {},
                       {{texture.value(), rect(0, 0, 8, 8, {255, 128, 0, 255})}});
        CHECK(pixel_is(image, 5, 5, {200, 100, 0, 255}).empty());
    }

    SUBCASE("a view matrix that moves everything 4 pixels right") {
        // Not symmetric, so a transposed upload would put the shift in w and draw nothing here.
        Mat4 view = pixel_projection(8.0F, 8.0F);
        view[12] += view[0] * 4.0F;
        const Image image =
            render(*device, target.value(), {.target = {}, .clear = {}, .view = view},
                   {{TextureId::None, rect(0, 0, 4, 8, {0, 255, 0, 255})}});
        CHECK(pixel_is(image, 1, 4, {0, 0, 0, 255}).empty());
        CHECK(pixel_is(image, 6, 4, {0, 255, 0, 255}).empty());
    }

    SUBCASE("batching draws and destroying a texture mid-frame") {
        auto texture = device->create_texture(
            {.width = 1, .height = 1, .filter = Filter::Linear, .render_target = false},
            std::vector<std::uint8_t>{0, 0, 255, 255});
        REQUIRE(texture.ok());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
        REQUIRE(device->draw(TextureId::None, rect(0, 0, 8, 4)).ok());
        REQUIRE(device->draw(TextureId::None, rect(0, 4, 4, 8, {255, 0, 0, 255})).ok());
        REQUIRE(device->draw(texture.value(), rect(4, 4, 8, 8)).ok());
        device->destroy_texture(texture.value()); // still drawn: it goes after the frame
        REQUIRE(device->end_frame().ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel_is(image.value(), 2, 2, {255, 255, 255, 255}).empty());
        CHECK(pixel_is(image.value(), 2, 6, {255, 0, 0, 255}).empty());
        CHECK(pixel_is(image.value(), 6, 6, {0, 0, 255, 255}).empty());
        CHECK(device->stats().draws == 2);
        CHECK(device->stats().triangles == 6);
        CHECK(device->stats().textures == 1);
    }
}

TEST_CASE("the GPU device refuses the same mistakes as the null device") {
    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    check_refusals(*device);
    CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
          "this device has no window; draw to a render-target texture");
}

TEST_CASE("V-Sync modes have Godot's names") {
    for (const VsyncMode mode :
         {VsyncMode::Disabled, VsyncMode::Enabled, VsyncMode::Adaptive, VsyncMode::Mailbox}) {
        CHECK(slash::render::parse_vsync_mode(slash::render::vsync_mode_name(mode)) == mode);
    }
    CHECK(slash::render::vsync_mode_name(VsyncMode::Mailbox) == "mailbox");
    CHECK_FALSE(slash::render::parse_vsync_mode("on").has_value());
}

TEST_CASE("a device with no window records the V-Sync mode it is given") {
    auto null_device = create_null_device();
    CHECK(null_device->vsync() == VsyncMode::Enabled);
    CHECK(null_device->set_vsync(VsyncMode::Adaptive) == VsyncMode::Adaptive);
    CHECK(null_device->vsync() == VsyncMode::Adaptive);

    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    CHECK(device->vsync() == VsyncMode::Enabled);
    CHECK(device->set_vsync(VsyncMode::Disabled) == VsyncMode::Disabled);
    CHECK(device->vsync() == VsyncMode::Disabled);
}

// A window on SDL's offscreen video driver, which can make a Vulkan surface: Linux CI runs this
// on software Vulkan. Elsewhere, or with no Vulkan driver, it is skipped.
TEST_CASE("a window's V-Sync mode changes its present mode, falling back to enabled") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    auto platform = slash::platform::Platform::create(
        {.title = "vsync test", .width = 64, .height = 64, .headless = false});
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    std::unique_ptr<RenderDevice> device;
    if (platform.ok()) {
        auto created =
            create_device({.window = platform.value().window(), .driver = "auto", .debug = false});
        if (created.ok()) {
            device = std::move(created).value();
        }
    }
    if (device == nullptr) {
#ifdef __linux__
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works with an offscreen window");
        }
#endif
        MESSAGE("no GPU window here, so present modes are not checked");
        return;
    }
    CHECK(device->vsync() == VsyncMode::Enabled);
    // SDL has no adaptive present mode; the others are used where the driver has them.
    CHECK(device->set_vsync(VsyncMode::Adaptive) == VsyncMode::Enabled);
    const VsyncMode mailbox = device->set_vsync(VsyncMode::Mailbox);
    CHECK((mailbox == VsyncMode::Mailbox || mailbox == VsyncMode::Enabled));
    CHECK(device->vsync() == mailbox);
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());
    const VsyncMode disabled = device->set_vsync(VsyncMode::Disabled);
    CHECK((disabled == VsyncMode::Disabled || disabled == VsyncMode::Enabled));
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());
    CHECK(device->set_vsync(VsyncMode::Enabled) == VsyncMode::Enabled);
    CHECK(device->stats().frames == 2);
}
