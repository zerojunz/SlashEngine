#include "devices.hpp"

#include "slash/platform/platform.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/light.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using slash::render::BlendMode;
using slash::render::Filter;
using slash::render::Image;
using slash::render::Light;
using slash::render::LightHit;
using slash::render::LightKind;
using slash::render::LightMode;
using slash::render::Material;
using slash::render::RenderDevice;
using slash::render::ShadeItem;
using slash::render::ShadePixel;
using slash::render::ShadowFilter;
using slash::render::TextureId;
using slash::render::Vertex2D;
using slash::test::drawing_devices;
using slash::test::gl_device;
using slash::test::gpu_device;

namespace {

using Rgba = std::array<std::uint8_t, 4>;
using Float4 = std::array<float, 4>;

constexpr std::uint32_t kColumns = slash::render::kShadowMapWidth;

const slash::render::AtlasSampler kWhiteAtlas = [](float /*u*/, float /*v*/) {
    return Float4{1.0F, 1.0F, 1.0F, 1.0F};
};
const slash::render::ShadowTexel kNoShadows = [](std::uint32_t /*column*/, std::uint32_t /*row*/) {
    return slash::render::kNoShadow;
};

// A point light whose texture covers the square from (x0, y0) to (x1, y1).
Light point_light(float x0, float y0, float x1, float y1) {
    Light light;
    light.position = {(x0 + x1) / 2.0F, (y0 + y1) / 2.0F};
    light.texture_transform = {1.0F / (x1 - x0), 0.0F,           -x0 / (x1 - x0), 0.0F,
                               1.0F / (y1 - y0), -y0 / (y1 - y0)};
    return light;
}

// Close to within a 1/255 step.
bool near(const Float4& a, const Float4& b) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::abs(a.at(i) - b.at(i)) > 1.5F / 255.0F) {
            return false;
        }
    }
    return true;
}

std::string text(const Float4& c) {
    return std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + "," +
           std::to_string(c[3]);
}

// Two triangles covering a rectangle, the texture's u running from u0 to u1.
std::vector<Vertex2D> rect(float x0, float y0, float x1, float y1, float u0 = 0.0F, float u1 = 1.0F,
                           Rgba rgba = {255, 255, 255, 255}) {
    const Vertex2D top_left{.x = x0, .y = y0, .u = u0, .v = 0.0F, .rgba = rgba};
    const Vertex2D top_right{.x = x1, .y = y0, .u = u1, .v = 0.0F, .rgba = rgba};
    const Vertex2D bottom_left{.x = x0, .y = y1, .u = u0, .v = 1.0F, .rgba = rgba};
    const Vertex2D bottom_right{.x = x1, .y = y1, .u = u1, .v = 1.0F, .rgba = rgba};
    return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

} // namespace

TEST_CASE("light modes and shadow filters have Godot's names") {
    CHECK(slash::render::light_mode_name(LightMode::LightOnly) == "light_only");
    CHECK(slash::render::parse_light_mode("unshaded") == LightMode::Unshaded);
    CHECK_FALSE(slash::render::parse_light_mode("lit").has_value());
    CHECK(slash::render::shadow_filter_name(ShadowFilter::Pcf13) == "pcf13");
    CHECK(slash::render::parse_shadow_filter("pcf5") == ShadowFilter::Pcf5);
    CHECK_FALSE(slash::render::parse_shadow_filter("pcf9").has_value());
}

TEST_CASE("shadow map texels hold depths in 24 bits") {
    CHECK(slash::render::decode_shadow_depth(slash::render::kNoShadow) == 2.0F);
    CHECK(slash::render::encode_shadow_depth(0.0F) == Rgba{0, 0, 0, 255});
    CHECK(slash::render::encode_shadow_depth(1.0F) == Rgba{255, 255, 255, 255});
    for (const float depth : {0.0F, 0.25F, 0.3F, 0.5F, 0.999F}) {
        const float back =
            slash::render::decode_shadow_depth(slash::render::encode_shadow_depth(depth));
        CHECK(back <= depth);
        CHECK(depth - back < 1.0e-6F);
    }
}

TEST_CASE("points fall in their light's shadow row by angle or across the direction") {
    Light point = point_light(0, 0, 100, 100);
    point.shadow_far = 50.0F;
    // Straight left of the light is the row's start, straight right its middle.
    auto coord = slash::render::shadow_coord(point, {10.0F, 50.0F - 1.0e-4F});
    CHECK(coord.column == doctest::Approx(0.0).epsilon(0.001));
    CHECK(coord.depth == doctest::Approx(0.8));
    coord = slash::render::shadow_coord(point, {75.0F, 50.0F});
    CHECK(coord.column == doctest::Approx(kColumns / 2.0));
    CHECK(coord.depth == doctest::Approx(0.5));
    // Straight below, y down: a quarter of the way past the middle.
    coord = slash::render::shadow_coord(point, {50.0F, 60.0F});
    CHECK(coord.column == doctest::Approx(kColumns * 0.75));

    Light sun;
    sun.kind = LightKind::Directional;
    sun.position = {0.0F, 1.0F}; // shining down
    sun.shadow_origin = {-100.0F, 20.0F};
    sun.shadow_span = 200.0F;
    sun.shadow_far = 40.0F;
    // Across a downward light is (-1, 0): x = 50 is -50 across.
    coord = slash::render::shadow_coord(sun, {50.0F, 30.0F});
    CHECK(coord.column == doctest::Approx(kColumns / 4.0));
    CHECK(coord.depth == doctest::Approx(0.25));
}

TEST_CASE("shade mixes the ambient color and lights into a pixel as Godot does") {
    const Float4 ambient{0.2F, 0.2F, 0.4F, 1.0F};
    const ShadePixel pixel{.position = {50.0F, 50.0F},
                           .color = {0.5F, 1.0F, 1.0F, 1.0F},
                           .normal = std::nullopt,
                           .specular = std::nullopt};
    const ShadeItem item;

    SUBCASE("no lights: the ambient color multiplies") {
        CHECK(near(shade({}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
    }
    SUBCASE("unshaded draws keep their color") {
        CHECK(shade({}, ambient,
                    {.mode = LightMode::Unshaded, .light_mask = 1, .z_index = 0, .layer = 0}, pixel,
                    kWhiteAtlas, kNoShadows) == pixel.color);
    }

    Light light = point_light(0, 0, 100, 100);
    light.color = {1.0F, 0.5F, 0.0F, 1.0F};
    SUBCASE("add, sub and mix") {
        // Each light's color times the base color, times its alpha.
        std::vector<LightHit> hits;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows, &hits),
                   {0.6F, 0.7F, 0.4F, 1.0F}));
        REQUIRE(hits.size() == 1);
        CHECK(near(hits[0].color, {0.5F, 0.5F, 0.0F, 1.0F}));
        CHECK(hits[0].shadow == 0.0F);
        light.blend = BlendMode::Sub;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {-0.4F, -0.3F, 0.4F, 1.0F}));
        light.blend = BlendMode::Mix;
        light.color[3] = 0.5F;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {0.3F, 0.35F, 0.2F, 1.0F}));
    }
    SUBCASE("the light texture shapes the light, and nothing reaches outside it") {
        const slash::render::AtlasSampler half = [](float /*u*/, float /*v*/) {
            return Float4{1.0F, 1.0F, 1.0F, 0.5F};
        };
        CHECK(near(shade({&light, 1}, ambient, item, pixel, half, kNoShadows),
                   {0.35F, 0.45F, 0.4F, 1.0F}));
        ShadePixel outside = pixel;
        outside.position = {120.0F, 50.0F};
        CHECK(near(shade({&light, 1}, ambient, item, outside, half, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
    }
    SUBCASE("cull masks and z ranges pick the draws a light reaches") {
        light.item_cull_mask = 2;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
        light.item_cull_mask = 3;
        light.z_max = -1;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
    }
    SUBCASE("layer ranges pick the canvas layers a light reaches") {
        light.layer_min = 1;
        light.layer_max = 2;
        CHECK(near(shade({&light, 1}, ambient, item, pixel, kWhiteAtlas, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
        const ShadeItem hud{.mode = LightMode::Normal, .light_mask = 1, .z_index = 0, .layer = 2};
        CHECK(near(shade({&light, 1}, ambient, hud, pixel, kWhiteAtlas, kNoShadows),
                   {0.6F, 0.7F, 0.4F, 1.0F}));
    }
    SUBCASE("light_only shows the draw only where lights reach") {
        const ShadeItem light_only{
            .mode = LightMode::LightOnly, .light_mask = 1, .z_index = 0, .layer = 0};
        CHECK(near(shade({}, ambient, light_only, pixel, kWhiteAtlas, kNoShadows),
                   {0.5F, 1.0F, 1.0F, 0.0F}));
        light.color[3] = 0.25F;
        CHECK(near(shade({&light, 1}, ambient, light_only, pixel, kWhiteAtlas, kNoShadows),
                   {0.625F, 1.125F, 1.0F, 0.25F}));
    }
    SUBCASE("normal maps darken pixels that face away from the light") {
        light.height = 50.0F;
        ShadePixel lit = pixel;
        lit.position = {50.0F, 100.0F};
        lit.normal = std::array<float, 3>{0.0F, 0.0F, 1.0F};
        // The light is 50 pixels up and 50 above the plane: 45 degrees.
        const float n_dot_l = std::numbers::sqrt2_v<float> / 2.0F;
        Light big = point_light(-100, -100, 200, 200);
        big.position = {50.0F, 50.0F};
        big.height = 50.0F;
        big.color = light.color;
        CHECK(near(shade({&big, 1}, ambient, item, lit, kWhiteAtlas, kNoShadows),
                   {0.1F + (0.5F * n_dot_l), 0.2F + (0.5F * n_dot_l), 0.4F, 1.0F}));
        lit.normal = std::array<float, 3>{0.0F, 1.0F, 0.0F}; // facing away, down the screen
        CHECK(near(shade({&big, 1}, ambient, item, lit, kWhiteAtlas, kNoShadows),
                   {0.1F, 0.2F, 0.4F, 1.0F}));
    }
}

TEST_CASE("occluders' depths in the shadow map hide pixels behind them") {
    Light light = point_light(0, 0, 100, 100);
    light.shadow_row = 0;
    light.shadow_far = 50.0F;
    light.shadow_color = {0.0F, 0.0F, 1.0F, 1.0F};
    // An occluder 20 pixels to the right of the light, in every column from -45 to 45 degrees.
    std::vector<Rgba> row(kColumns, slash::render::kNoShadow);
    for (std::uint32_t column = kColumns * 3 / 8; column < kColumns * 5 / 8; ++column) {
        row.at(column) = slash::render::encode_shadow_depth(20.0F / 50.0F);
    }
    const slash::render::ShadowTexel texel = [&](std::uint32_t column, std::uint32_t r) {
        CHECK(r == 0);
        return row.at(column);
    };
    const Float4 white{1.0F, 1.0F, 1.0F, 1.0F};
    const auto at = [&](float x, float y) {
        std::vector<LightHit> hits;
        const ShadePixel pixel{
            .position = {x, y}, .color = white, .normal = std::nullopt, .specular = std::nullopt};
        const Float4 color =
            shade({&light, 1}, {0.0F, 0.0F, 0.0F, 1.0F}, {}, pixel, kWhiteAtlas, texel, &hits);
        REQUIRE(hits.size() == 1);
        return std::pair{color, hits[0].shadow};
    };
    CHECK(at(60.0F, 50.0F).second == 0.0F);                        // in front of it
    CHECK(at(90.0F, 50.0F).second == 1.0F);                        // behind it
    CHECK(at(50.0F, 90.0F).second == 0.0F);                        // off to the side
    CHECK(near(at(90.0F, 50.0F).first, {0.0F, 0.0F, 1.0F, 1.0F})); // the shadow color
    // A softer filter averages neighbors: at the edge column, some taps miss the occluder.
    light.shadow_filter = ShadowFilter::Pcf5;
    const float edge = 50.0F + (40.0F * std::cos(std::numbers::pi_v<float> / 4.0F + 0.001F));
    const float edge_y = 50.0F + (40.0F * std::sin(std::numbers::pi_v<float> / 4.0F + 0.001F));
    const float soft = at(edge, edge_y).second;
    CHECK(soft > 0.0F);
    CHECK(soft < 1.0F);

    // A directional light's shadow reaches only so far past its occluder.
    Light sun;
    sun.kind = LightKind::Directional;
    sun.position = {0.0F, 1.0F};
    sun.shadow_row = 0;
    sun.shadow_origin = {-100.0F, 0.0F};
    sun.shadow_span = 200.0F;
    sun.shadow_far = 100.0F;
    sun.shadow_reach = 0.3F;
    std::ranges::fill(row, slash::render::encode_shadow_depth(0.2F));
    const auto sun_at = [&](float y) {
        std::vector<LightHit> hits;
        const ShadePixel pixel{.position = {0.0F, y},
                               .color = white,
                               .normal = std::nullopt,
                               .specular = std::nullopt};
        (void)shade({&sun, 1}, {0.0F, 0.0F, 0.0F, 1.0F}, {}, pixel, kWhiteAtlas, texel, &hits);
        return hits.at(0).shadow;
    };
    CHECK(sun_at(10.0F) == 0.0F);
    CHECK(sun_at(30.0F) == 1.0F);
    CHECK(sun_at(60.0F) == 0.0F);
}

TEST_CASE("every device checks a frame's lights") {
    for (int kind = 0; kind < 4; ++kind) {
        std::unique_ptr<RenderDevice> device;
        if (kind == 0) {
            device = slash::render::create_null_device();
        } else if (kind == 1) {
            device = slash::render::create_software_device();
        } else {
            device = kind == 2 ? gpu_device() : gl_device();
        }
        if (device == nullptr) {
            continue;
        }
        CAPTURE(kind);
        auto target = device->create_texture(
            {.width = 4, .height = 4, .filter = Filter::Linear, .render_target = true});
        auto narrow = device->create_texture(
            {.width = 4, .height = 1, .filter = Filter::Nearest, .render_target = false});
        REQUIRE(target.ok());
        REQUIRE(narrow.ok());
        slash::render::Frame frame{.target = target.value(), .clear = {}, .view = {}};
        frame.lighting.lights.resize(slash::render::kMaxLights + 1);
        CHECK(device->begin_frame(frame).error().message ==
              "a frame takes up to 64 lights, not 65");
        frame.lighting.lights.resize(1);
        frame.lighting.lights[0].blend = BlendMode::Mul;
        CHECK(device->begin_frame(frame).error().message ==
              "light 0 blends with mul; lights add, sub or mix");
        frame.lighting.lights[0].blend = BlendMode::Add;
        frame.lighting.lights[0].shadow_row = 0;
        CHECK(device->begin_frame(frame).error().message ==
              "light 0 has shadows but the frame has no shadow map");
        frame.lighting.shadow_map = narrow.value();
        CHECK(device->begin_frame(frame).error().message ==
              "light 0's shadow row 0 is outside the 4x1 shadow map, which is 2048 texels wide");
        frame.lighting.shadow_map = target.value();
        CHECK(device->begin_frame(frame).error().message ==
              "texture 1 is this frame's target, so it can't be drawn with until the frame ends");
        frame.lighting = {};
        REQUIRE(device->begin_frame(frame).ok());
        Material material;
        material.shading.mode = LightMode::Normal;
        material.shading.normal_map = static_cast<TextureId>(99);
        CHECK(device->draw(TextureId::None, rect(0, 0, 1, 1), material).error().message ==
              "no texture 99");
        REQUIRE(device->end_frame().ok());
    }
}

TEST_CASE("the software and GPU devices light pixels as shade does") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        constexpr std::uint32_t kSize = 32;
        auto target = device->create_texture(
            {.width = kSize, .height = kSize, .filter = Filter::Linear, .render_target = true});
        REQUIRE(target.ok());

        // The atlas: a 4x4 light fading out to the right on the left, plain white on the right.
        Image atlas{.width = 8, .height = 4, .pixels = {}};
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const auto fade = static_cast<std::uint8_t>(x < 4 ? 255 - (x * 60) : 255);
                atlas.pixels.insert(atlas.pixels.end(), {255, 255, 255, fade});
            }
        }
        // Shadow rows: the point light's occluder covers its right side at 40% of its reach; the
        // directional light's covers the left half of the target at depth 0.25.
        std::vector<std::uint8_t> shadows(std::size_t{kColumns} * 2 * 4, 0);
        const auto put = [&](std::uint32_t column, std::uint32_t row, Rgba texel) {
            std::ranges::copy(texel, shadows.begin() + static_cast<std::ptrdiff_t>(
                                                           ((row * kColumns) + column) * 4));
        };
        for (std::uint32_t column = kColumns * 3 / 8; column < kColumns * 5 / 8; ++column) {
            put(column, 0, slash::render::encode_shadow_depth(0.4F));
        }
        for (std::uint32_t column = kColumns / 2; column < kColumns; ++column) {
            put(column, 1, slash::render::encode_shadow_depth(0.25F));
        }
        // A normal map leaning right on its left half and down on its right half, and a specular
        // map.
        const Image normals{
            .width = 2, .height = 1, .pixels = {218, 128, 218, 255, 128, 37, 218, 255}};
        const Image shine{.width = 1, .height = 1, .pixels = {255, 200, 100, 128}};
        auto atlas_texture = device->create_texture(
            {.width = 8, .height = 4, .filter = Filter::Linear, .render_target = false},
            atlas.pixels);
        auto shadow_texture = device->create_texture(
            {.width = kColumns, .height = 2, .filter = Filter::Nearest, .render_target = false},
            shadows);
        auto normal_texture = device->create_texture(
            {.width = 2, .height = 1, .filter = Filter::Nearest, .render_target = false},
            normals.pixels);
        auto shine_texture = device->create_texture(
            {.width = 1, .height = 1, .filter = Filter::Nearest, .render_target = false},
            shine.pixels);
        REQUIRE(atlas_texture.ok());
        REQUIRE(shadow_texture.ok());
        REQUIRE(normal_texture.ok());
        REQUIRE(shine_texture.ok());

        std::vector<Light> lights;
        Light torch = point_light(-8, -8, 40, 40);
        torch.position = {12.0F, 14.0F};
        torch.height = 10.0F;
        torch.color = {1.2F, 0.9F, 0.6F, 1.0F};
        torch.atlas_rect = {0.5F / 8.0F, 0.5F / 4.0F, 3.0F / 8.0F, 3.0F / 4.0F};
        torch.shadow_row = 0;
        torch.shadow_far = 30.0F;
        torch.shadow_filter = ShadowFilter::Pcf5;
        torch.shadow_color = {0.1F, 0.0F, 0.2F, 0.5F};
        lights.push_back(torch);
        Light sun;
        sun.kind = LightKind::Directional;
        sun.position = {std::numbers::sqrt2_v<float> / 2.0F, std::numbers::sqrt2_v<float> / 2.0F};
        sun.height = 0.4F;
        sun.color = {0.3F, 0.3F, 0.5F, 1.0F};
        sun.blend = BlendMode::Mix;
        sun.shadow_row = 1;
        sun.shadow_origin = {-23.0F, 0.0F};
        sun.shadow_span = 46.0F;
        sun.shadow_far = 46.0F;
        sun.shadow_reach = 0.5F;
        lights.push_back(sun);
        Light masked = point_light(0, 0, 32, 32);
        masked.item_cull_mask = 4; // reaches neither draw
        masked.atlas_rect = {4.5F / 8.0F, 0.5F / 4.0F, 3.0F / 8.0F, 3.0F / 4.0F};
        lights.push_back(masked);
        Light layered = point_light(0, 0, 32, 32);
        layered.color = {0.2F, 0.5F, 0.2F, 1.0F};
        layered.layer_min = 1; // reaches only the right draw, in layer 1
        layered.layer_max = 3;
        layered.atlas_rect = masked.atlas_rect;
        lights.push_back(layered);

        const slash::render::Frame frame{.target = target.value(),
                                         .clear = {},
                                         .view = {},
                                         .lighting = {.lights = lights,
                                                      .atlas = atlas_texture.value(),
                                                      .shadow_map = shadow_texture.value()}};
        REQUIRE(device->begin_frame(frame).ok());
        // Left: normal-mapped and shiny. Right: mirrored left to right, plain, light only, in
        // canvas layer 1, whose positions are 2 pixels left and 1 down among the lights'.
        Material left;
        left.shading = {.mode = LightMode::Normal,
                        .light_mask = 1,
                        .z_index = 0,
                        .layer = 0,
                        .ambient = {0.3F, 0.3F, 0.35F, 1.0F},
                        .to_lights = std::nullopt,
                        .normal_map = normal_texture.value(),
                        .specular_map = shine_texture.value(),
                        .specular = {1.0F, 1.0F, 1.0F, 0.8F}};
        Material right;
        right.shading = {.mode = LightMode::LightOnly,
                         .light_mask = 3,
                         .z_index = 2,
                         .layer = 1,
                         .ambient = {1.0F, 1.0F, 1.0F, 1.0F},
                         .to_lights = std::array{1.0F, 0.0F, -2.0F, 0.0F, 1.0F, 1.0F},
                         .normal_map = normal_texture.value(),
                         .specular_map = TextureId::None,
                         .specular = {1.0F, 1.0F, 1.0F, 1.0F}};
        constexpr Rgba kTint{200, 230, 255, 255};
        REQUIRE(device->draw(TextureId::None, rect(0, 0, 16, kSize, 0.0F, 1.0F, kTint), left).ok());
        REQUIRE(device->draw(TextureId::None, rect(16, 0, kSize, kSize, 1.0F, 0.0F, kTint), right)
                    .ok());
        REQUIRE(device->end_frame().ok());
        auto drawn = device->read_texture(target.value());
        REQUIRE(drawn.ok());

        const slash::render::AtlasSampler sample_atlas = [&](float u, float v) {
            return slash::render::sample_image(atlas, Filter::Linear, false, u, v);
        };
        const slash::render::ShadowTexel shadow_texel = [&](std::uint32_t column,
                                                            std::uint32_t row) {
            Rgba texel{};
            std::copy_n(shadows.begin() +
                            static_cast<std::ptrdiff_t>(((row * kColumns) + column) * 4),
                        4, texel.begin());
            return texel;
        };
        int compared = 0;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const bool is_left = x < 16;
                const std::array<float, 2> drawn_at{static_cast<float>(x) + 0.5F,
                                                    static_cast<float>(y) + 0.5F};
                // Where the lights see it.
                const std::array<float, 2> position =
                    is_left ? drawn_at : std::array{drawn_at[0] - 2.0F, drawn_at[1] + 1.0F};
                // Pixels on a shadow's edge may land either side of it.
                bool edge = false;
                for (const Light& light : lights) {
                    if (light.shadow_row < 0) {
                        continue;
                    }
                    const auto coord = slash::render::shadow_coord(light, position);
                    const float column = coord.column - std::floor(coord.column);
                    edge = edge || column < 0.02F || column > 0.98F ||
                           std::abs(coord.depth - 0.4F) < 0.02F ||
                           std::abs(coord.depth - 0.25F) < 0.02F;
                }
                if (edge) {
                    continue;
                }
                const float u =
                    is_left ? drawn_at[0] / 16.0F : 1.0F - ((drawn_at[0] - 16.0F) / 16.0F);
                const Float4 texel = slash::render::sample_image(normals, Filter::Nearest, false, u,
                                                                 drawn_at[1] / kSize);
                ShadePixel pixel{.position = position,
                                 .color = {200.0F / 255.0F, 230.0F / 255.0F, 1.0F, 1.0F},
                                 .normal = slash::render::map_normal(
                                     texel, {is_left ? 1.0F : -1.0F, 0.0F}, {0.0F, 1.0F}),
                                 .specular = std::nullopt};
                if (is_left) {
                    const Float4 s = slash::render::sample_image(shine, Filter::Nearest, false, u,
                                                                 drawn_at[1] / kSize);
                    pixel.specular = Float4{s[0], s[1], s[2], s[3] * 0.8F};
                }
                const Material& material = is_left ? left : right;
                Float4 expected = slash::render::shade(lights, material.shading.ambient,
                                                       {.mode = material.shading.mode,
                                                        .light_mask = material.shading.light_mask,
                                                        .z_index = material.shading.z_index,
                                                        .layer = material.shading.layer},
                                                       pixel, sample_atlas, shadow_texel);
                // The target keeps 0 to 1, blended over its clear black.
                for (float& channel : expected) {
                    channel = std::clamp(channel, 0.0F, 1.0F);
                }
                const float alpha = expected[3];
                const std::size_t at = ((std::size_t{y} * kSize) + x) * 4;
                Float4 actual{};
                for (std::size_t i = 0; i < 4; ++i) {
                    actual.at(i) = static_cast<float>(drawn.value().pixels.at(at + i)) / 255.0F;
                }
                // Mixed over the clear color, opaque black.
                const Float4 blended{expected[0] * alpha, expected[1] * alpha, expected[2] * alpha,
                                     1.0F};
                CAPTURE(x);
                CAPTURE(y);
                CHECK_MESSAGE(near(actual, blended), text(actual) << " vs " << text(blended));
                ++compared;
            }
        }
        CHECK(compared > 650);
    }
}
