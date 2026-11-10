#include "../engine/render/src/mesh_shading.hpp"
#include "devices.hpp"

#include "slash/core/bits.hpp"
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
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;

namespace {

// The share of thresholds alpha reaches over a grid of places, each a pixel step apart.
float kept_share(float alpha, float step) {
    int kept = 0;
    int count = 0;
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            const detail::Float3 at{(static_cast<float>(x) + 0.5F) * step,
                                    (static_cast<float>(y) + 0.5F) * step, 0.25F};
            const float threshold =
                detail::alpha_hash_threshold(at, {step, 0.0F, 0.0F}, {0.0F, step, 0.0F}, 1.0F);
            REQUIRE(threshold >= 0.00001F);
            REQUIRE(threshold <= 1.0F);
            kept += alpha < threshold ? 0 : 1;
            ++count;
        }
    }
    return static_cast<float>(kept) / static_cast<float>(count);
}

// A square of side 2 facing +Z, with uv over it.
std::vector<Vertex3D> card() {
    const auto corner = [](float x, float y) {
        return Vertex3D{.position = {x, y, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 1.0F) * 0.5F, (1.0F - y) * 0.5F}};
    };
    const Vertex3D a = corner(-1.0F, 1.0F);
    const Vertex3D b = corner(1.0F, 1.0F);
    const Vertex3D c = corner(1.0F, -1.0F);
    const Vertex3D d = corner(-1.0F, -1.0F);
    return {a, b, c, a, c, d};
}

// What draw_card draws.
struct Card {
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    float alpha = 0.5F;
    float scale = 1.0F;
    bool shadow = false;
};

// A half-transparent card, alpha hashed, turned away a little over a dark floor; with shadow, lit
// from in front so the card's dithered shadow falls on a wall behind it.
Image draw_card(RenderDevice& device, const Card& card_drawn) {
    constexpr std::uint32_t kSize = 64;
    auto target = device.create_texture({.width = kSize, .height = kSize, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(card(), {});
    REQUIRE(mesh.ok());
    render::Scene3D scene;
    scene.method = card_drawn.method;
    scene.camera = render::look_at({0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(50.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 1.0F, 1.0F},
                     .direction = {0.0F, 0.0F, -1.0F},
                     .up = {0.0F, 1.0F, 0.0F},
                     .shadow = card_drawn.shadow,
                     .shadow_normal_bias = 0.0F}};
    scene.shadows = {.directional_size = 1024, .positional_size = 1024};
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}, .scene = scene}).ok());
    if (card_drawn.shadow) {
        // A white wall behind, which takes the card's shadow; the card itself casts it only.
        constexpr Mat4 kWall{3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
        REQUIRE(device.draw_mesh(mesh.value(), kWall, {.albedo = {1.0F, 1.0F, 1.0F, 1.0F}}).ok());
    }
    // Turned 25 degrees about y, so its pixels' steps along it change across it.
    const float c = std::cos(0.436F);
    const float s = std::sin(0.436F);
    const Mat4 turned{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    const Material3D material{.albedo = {0.9F, 0.4F, 0.2F, card_drawn.alpha},
                              .transparency = render::Transparency::AlphaHash,
                              .alpha_hash_scale = card_drawn.scale};
    const auto drawn = device.draw_mesh(mesh.value(), turned, material, 1,
                                        card_drawn.shadow ? render::ShadowCasting::ShadowsOnly
                                                          : render::ShadowCasting::On);
    REQUIRE_MESSAGE(drawn.ok(), (drawn.ok() ? std::string() : drawn.error().message));
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// How many pixels are lit, red over 40, and how many are not black.
std::pair<int, int> card_pixels(const Image& image) {
    int lit = 0;
    int drawn = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::array<int, 4> p = pixel(image, x, y);
            lit += p[0] > 40 ? 1 : 0;
            drawn += (p[0] + p[1] + p[2]) > 0 ? 1 : 0;
        }
    }
    return {lit, drawn};
}

// How many pixels of a and b differ by more than by in a channel.
int differing(const Image& a, const Image& b, int by) {
    int count = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::array<int, 4> p = pixel(a, x, y);
            const std::array<int, 4> q = pixel(b, x, y);
            int most = 0;
            for (std::size_t ch = 0; ch < 4; ++ch) {
                most = std::max(most, std::abs(p.at(ch) - q.at(ch)));
            }
            count += most > by ? 1 : 0;
        }
    }
    return count;
}

} // namespace

TEST_CASE("alpha hash's noise is whole-number math, the same on every machine") {
    // PCG3D of the cell, its top 24 bits over 2^24.
    CHECK(detail::alpha_hash_value({0.0F, 0.0F, 0.0F}) == detail::alpha_hash_value({0, 0, 0}));
    const float first = detail::alpha_hash_value({3.0F, -7.0F, 12.0F});
    CHECK(first >= 0.0F);
    CHECK(first < 1.0F);
    CHECK(first * 16777216.0F == std::floor(first * 16777216.0F));
    CHECK(first != detail::alpha_hash_value({3.0F, -7.0F, 13.0F}));
    // Cells far out are held to 2^30, and -0 is 0.
    CHECK(detail::alpha_hash_value({-0.0F, 0.0F, 0.0F}) ==
          detail::alpha_hash_value({0.0F, 0.0F, 0.0F}));
    CHECK(detail::alpha_hash_value({1e20F, 0.0F, 0.0F}) ==
          detail::alpha_hash_value({1073741824.0F, 0.0F, 0.0F}));
}

TEST_CASE("alpha hash keeps the share of pixels a surface's alpha says, at any scale") {
    // A step of 1/64 is a whole level, which hashes cells a pixel wide; 1/90 blends two levels,
    // which Godot's three cases keep even.
    for (const float step : {1.0F / 64.0F, 1.0F / 90.0F, 0.37F}) {
        CAPTURE(step);
        for (const float alpha : {0.1F, 0.3F, 0.5F, 0.8F}) {
            CAPTURE(alpha);
            CHECK(kept_share(alpha, step) == doctest::Approx(alpha).epsilon(0.08));
        }
        CHECK(kept_share(1.0F, step) == 1.0F);
        CHECK(kept_share(0.0F, step) == 0.0F);
    }
    // A place on the mesh with no step across it (a pixel the mesh is edge on to) still gets a
    // threshold.
    const float flat = detail::alpha_hash_threshold({0.5F, 0.5F, 0.5F}, {}, {}, 1.0F);
    CHECK(flat >= 0.00001F);
    CHECK(flat <= 1.0F);
}

TEST_CASE("alpha hash cuts as alpha scissor does, at a hashed threshold; rays take it as alpha") {
    const Material3D material{.albedo = {1.0F, 1.0F, 1.0F, 0.4F},
                              .transparency = render::Transparency::AlphaHash,
                              .alpha_hash_scale = 2.0F};
    const detail::MeshSurfaceUniforms surface = detail::mesh_surface_uniforms(material, 1);
    CHECK((surface.options[0] & detail::kMeshAlphaScissor) != 0);
    CHECK((surface.options[0] & detail::kMeshAlphaHash) != 0);
    CHECK((surface.options[0] & detail::kMeshHasAlpha) != 0);
    CHECK(slash::bit_cast<float>(surface.options[1]) == 2.0F);
    const detail::MeshSurfaceUniforms traced = detail::traced_surface(surface);
    CHECK((traced.options[0] & (detail::kMeshAlphaScissor | detail::kMeshAlphaHash)) == 0);
    CHECK((traced.options[0] & detail::kMeshHasAlpha) != 0);
    // Alpha scissor stays as it was for rays.
    const Material3D scissor{.transparency = render::Transparency::AlphaScissor};
    const detail::MeshSurfaceUniforms cut =
        detail::traced_surface(detail::mesh_surface_uniforms(scissor, 1));
    CHECK((cut.options[0] & detail::kMeshAlphaScissor) != 0);
    // Shadows keep a pixel whose alpha reaches its hashed threshold.
    CHECK(detail::shadow_keeps(surface, {1, 1, 1, 1}, {1, 1, 1, 1}, 0.3F));
    CHECK_FALSE(detail::shadow_keeps(surface, {1, 1, 1, 1}, {1, 1, 1, 1}, 0.5F));
}

TEST_CASE("every device dithers an alpha-hashed card, and its shadow, by its alpha") {
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::drawing_devices();
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        const auto [solid, solid_drawn] = card_pixels(draw_card(*device, {.alpha = 1.0F}));
        const auto [half, half_drawn] = card_pixels(draw_card(*device, {.alpha = 0.5F}));
        const auto [none, none_drawn] = card_pixels(draw_card(*device, {.alpha = 0.0F}));
        CHECK(solid > 400);
        CHECK(none == 0);
        // About half the card's pixels, each drawn whole.
        CHECK(half > solid * 2 / 5);
        CHECK(half < solid * 3 / 5);
        // Its shadow is dithered too: the wall is lit where the card lets the light through.
        const Image shadowed = draw_card(*device, {.alpha = 0.5F, .shadow = true});
        const Image dark = draw_card(*device, {.alpha = 1.0F, .shadow = true});
        const Image clear = draw_card(*device, {.alpha = 0.0F, .shadow = true});
        const auto brightness = [](const Image& image) {
            long sum = 0;
            for (std::uint32_t y = 16; y < 48; ++y) {
                for (std::uint32_t x = 16; x < 48; ++x) {
                    sum += pixel(image, x, y)[1];
                }
            }
            return sum;
        };
        CHECK(brightness(dark) < brightness(shadowed));
        CHECK(brightness(shadowed) < brightness(clear));
    }
}

TEST_CASE("GPUs dither an alpha-hashed card as the software device does") {
    const auto software = render::create_software_device();
    const std::array<Card, 3> cards{
        Card{}, Card{.method = render::RenderMethod::Compatibility, .alpha = 0.3F, .scale = 0.5F},
        Card{.alpha = 0.6F, .shadow = true}};
    for (const Card& drawn : cards) {
        const Image expected = draw_card(*software, drawn);
        for (auto& device : slash::test::hardware_devices()) {
            CAPTURE(render::backend_name(device->backend()));
            CAPTURE(drawn.alpha);
            // Where a pixel's threshold is within rounding of its alpha, or a GPU's derivatives
            // are fine where the software device's are coarse, a few pixels may differ.
            CHECK(differing(draw_card(*device, drawn), expected, 3) <= 40);
        }
    }
}
