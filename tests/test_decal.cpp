#include "../engine/render/src/decal.hpp"
#include "../engine/render/src/mesh_shading.hpp"
#include "devices.hpp"

#include "slash/core/json.hpp"
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
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace render = slash::render;
namespace detail = slash::render::detail;
using render::Decal3D;
using render::Decals3D;
using render::Image;
using render::RenderDevice;
using render::RenderMethod;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Size = std::array<std::uint32_t, 2>;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

using Rgba = std::array<std::uint8_t, 4>;

// An image whose texel at x, y is color(x, y).
template <typename Color>
std::shared_ptr<const Image> image_of(std::uint32_t width, std::uint32_t height, Color color) {
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    image->pixels.reserve(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Rgba texel = color(x, y);
            image->pixels.insert(image->pixels.end(), texel.begin(), texel.end());
        }
    }
    return image;
}

// A transparent black image, made quickly for big sizes.
std::shared_ptr<const Image> blank(std::uint32_t width, std::uint32_t height) {
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    image->pixels.resize(std::size_t{width} * height * 4);
    return image;
}

std::uint32_t word(Rgba texel) {
    return std::uint32_t{texel[0]} | (std::uint32_t{texel[1]} << 8U) |
           (std::uint32_t{texel[2]} << 16U) | (std::uint32_t{texel[3]} << 24U);
}

Mat4 translation(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

// A floor of side 10 around the origin, facing up.
std::vector<Vertex3D> floor_vertices() {
    const auto corner = [](float x, float z) {
        return Vertex3D{.position = {x, 0.0F, z},
                        .normal = {0.0F, 1.0F, 0.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 5.0F) / 10.0F, (z + 5.0F) / 10.0F}};
    };
    return {corner(-5.0F, -5.0F), corner(5.0F, -5.0F), corner(5.0F, 5.0F),
            corner(-5.0F, -5.0F), corner(5.0F, 5.0F),  corner(-5.0F, 5.0F)};
}

constexpr std::uint32_t kSize = 96;

// The floor seen at a slant, lit by a sun and ambient light, with a decal of 4 by 4 meters over
// its middle: its albedo red on the left and blue on the right (transparent in its top rows), a
// normal map of bumps, an ORM texture that makes its right half shinier and darker, and a green
// emission dot. The slant makes far texels smaller than pixels, so mipmapped filters blend levels.
render::Scene3D decal_scene(render::DecalFilter filter) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 4.0F, 4.5F}, {0.0F, 0.0F, -0.5F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 50.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    scene.lights.push_back({.type = render::LightType::Directional,
                            .color = {1.0F, 1.0F, 1.0F},
                            .energy = 1.0F,
                            .direction = {0.3F, -1.0F, -0.4F}});
    Decal3D decal;
    decal.size = {4.0F, 2.0F, 4.0F};
    decal.albedo = image_of(32, 32, [](std::uint32_t x, std::uint32_t y) {
        const std::uint8_t alpha = y < 4 ? 0 : 255;
        return x < 16 ? Rgba{220, 40, 30, alpha} : Rgba{30, 60, 230, alpha};
    });
    decal.normal = image_of(16, 16, [](std::uint32_t x, std::uint32_t y) {
        const auto nx = static_cast<std::uint8_t>(128 + (((x % 4) < 2) ? 60 : -60));
        const auto ny = static_cast<std::uint8_t>(128 + (((y % 4) < 2) ? 40 : -40));
        return Rgba{nx, ny, 255, 255};
    });
    decal.orm = image_of(8, 8, [](std::uint32_t x, std::uint32_t /*y*/) {
        return x < 4 ? Rgba{255, 230, 0, 255} : Rgba{180, 60, 90, 255};
    });
    decal.emission = image_of(16, 16, [](std::uint32_t x, std::uint32_t y) {
        const int dx = static_cast<int>(x) - 8;
        const int dy = static_cast<int>(y) - 8;
        return (dx * dx) + (dy * dy) < 9 ? Rgba{0, 255, 0, 255} : Rgba{0, 0, 0, 255};
    });
    decal.emission_energy = 2.0F;
    decal.modulate = {1.0F, 0.9F, 0.9F, 1.0F};
    scene.decals = {.decals = {decal}, .filter = filter};
    return scene;
}

Image draw(RenderDevice& device, const render::Scene3D& scene) {
    auto target = device.create_texture({.width = kSize, .height = kSize, .render_target = true});
    REQUIRE(target.ok());
    auto floor = device.create_mesh(floor_vertices(), {});
    REQUIRE(floor.ok());
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .scene = scene})
                .ok());
    CHECK(device.draw_mesh(floor.value(), translation(0, 0, 0), {.roughness = 0.7F}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

Rgba pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// The pixel a point of the world is drawn at.
std::array<std::uint32_t, 2> seen_at(const render::Scene3D& scene, std::array<float, 3> point) {
    const auto apply = [](const Mat4& m, const std::array<float, 4>& v) {
        std::array<float, 4> out{};
        for (std::size_t row = 0; row < 4; ++row) {
            out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                          (m.at(12 + row) * v[3]);
        }
        return out;
    };
    const std::array<float, 4> clip =
        apply(scene.projection, apply(scene.camera, {point[0], point[1], point[2], 1.0F}));
    const float x = ((clip[0] / clip[3]) + 1.0F) * 0.5F * static_cast<float>(kSize);
    const float y = (1.0F - (clip[1] / clip[3])) * 0.5F * static_cast<float>(kSize);
    return {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)};
}

// How many pixels differ by more than 2 in a channel, and the most any does.
std::pair<int, int> differences(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    int differing = 0;
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        int most = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            most = std::max(most, std::abs(a.pixels.at(i + c) - b.pixels.at(i + c)));
        }
        differing += most > 2 ? 1 : 0;
        worst = std::max(worst, most);
    }
    return {differing, worst};
}

} // namespace

TEST_CASE("the decal atlas lays textures out as Godot's update_decal_atlas packs them") {
    // Tallest first, then widest, each at the lowest, leftmost spot of a skyline 16 cells of 32
    // texels across, the power of two past the widest; the atlas's height a power of two too.
    const detail::DecalAtlasLayout mixed =
        detail::layout_decal_atlas({{64, 64}, {32, 128}, {16, 16}, {100, 20}, {300, 40}});
    CHECK(mixed.width == 512);
    CHECK(mixed.height == 256);
    CHECK(mixed.places == std::vector<Size>{{80, 16}, {16, 16}, {496, 16}, {176, 80}, {176, 16}});

    const detail::DecalAtlasLayout wide = detail::layout_decal_atlas({{1000, 8}});
    CHECK(wide.width == 1024);
    CHECK(wide.height == 32);
    CHECK(wide.places == std::vector<Size>{{16, 16}});

    // Equal textures fill rows of 8 cells, in order.
    const detail::DecalAtlasLayout rows =
        detail::layout_decal_atlas(std::vector<Size>(20, Size{8, 8}));
    CHECK(rows.width == 256);
    CHECK(rows.height == 128);
    CHECK(rows.places[7] == Size{240, 16});
    CHECK(rows.places[8] == Size{16, 48});
    CHECK(rows.places[19] == Size{112, 80});

    // With none, the atlas is a border across, so it has every level.
    const detail::DecalAtlasLayout none = detail::layout_decal_atlas({});
    CHECK(none.width == 32);
    CHECK(none.height == 32);
    CHECK(detail::decal_atlas_texels(32, 32) == 1024 + 256 + 64 + 16 + 4);
}

TEST_CASE("the decal atlas holds each texture once, its levels averaged, rects as fractions") {
    const auto a = image_of(2, 2, [](std::uint32_t x, std::uint32_t y) {
        return Rgba{static_cast<std::uint8_t>(10 + (x * 100)), static_cast<std::uint8_t>(y * 255),
                    7, 255};
    });
    const auto b = image_of(1, 1, [](std::uint32_t, std::uint32_t) { return Rgba{1, 2, 3, 4}; });
    Decals3D decals;
    decals.decals.push_back({.albedo = a, .emission = b});
    decals.decals.push_back({.albedo = a});
    const detail::DecalAtlas atlas = detail::pack_decal_atlas(decals);
    REQUIRE(atlas.images.size() == 2);
    CHECK(atlas.images[0] == a);
    CHECK(atlas.images[1] == b);
    CHECK(atlas.width == 256);
    CHECK(atlas.height == 32);
    REQUIRE(atlas.texels.size() == detail::decal_atlas_texels(256, 32));
    const auto at = [&](std::size_t offset, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
        return atlas.texels.at(offset + (y * width) + x);
    };
    // Level 0: each texture at its place, transparent black around it.
    CHECK(at(0, 256, 16, 16) == word({10, 0, 7, 255}));
    CHECK(at(0, 256, 17, 17) == word({110, 255, 7, 255}));
    CHECK(at(0, 256, 48, 16) == word({1, 2, 3, 4}));
    CHECK(at(0, 256, 15, 16) == 0);
    CHECK(at(0, 256, 18, 16) == 0);
    // Level 1: each 2x2 texels of level 0 averaged, rounded.
    CHECK(at(256 * 32, 128, 8, 8) == word({60, 128, 7, 255}));
    CHECK(at(256 * 32, 128, 24, 8) == word({0, 1, 1, 1}));
    CHECK(atlas.rect(a) == detail::Float4{16.0F / 256.0F, 0.5F, 2.0F / 256.0F, 2.0F / 32.0F});
    CHECK(atlas.rect(nullptr) == detail::Float4{});
    CHECK(atlas.rect(image_of(1, 1, [](std::uint32_t, std::uint32_t) { return Rgba{}; })) ==
          detail::Float4{});

    // A device's cache packs it again only when the textures change.
    detail::DecalAtlasCache cache;
    CHECK(cache.update(decals));
    CHECK(cache.atlas().generation == 1);
    CHECK(cache.atlas().texels == atlas.texels);
    decals.decals[1].albedo_mix = 0.5F;
    CHECK_FALSE(cache.update(decals));
    CHECK(cache.atlas().generation == 1);
    decals.decals[1].orm = b;
    CHECK_FALSE(cache.update(decals)); // b is in it already
    decals.decals[1].normal = image_of(4, 4, [](std::uint32_t, std::uint32_t) { return Rgba{}; });
    CHECK(cache.update(decals));
    CHECK(cache.atlas().generation == 2);
    CHECK(cache.atlas().images.size() == 3);
}

TEST_CASE("decals are checked: their numbers, their textures and the atlas they need") {
    const auto texel = image_of(1, 1, [](std::uint32_t, std::uint32_t) { return Rgba{}; });
    Decal3D decal{.albedo = texel};
    CHECK(render::check_decal(decal).ok());
    decal.size[1] = 0.0F;
    CHECK(render::check_decal(decal).error().message == "the decal's size is above 0");
    CHECK(render::check_decals({.decals = {{}, decal}}).error().message ==
          "decal 1's size is above 0");
    decal.size[1] = 2.0F;
    decal.normal_fade = 1.5F;
    CHECK(render::check_decal(decal).error().message ==
          "the decal's fades are 0 or more, and its normal fade at most 1");
    decal.normal_fade = 0.0F;
    decal.transform[13] = std::nanf("");
    CHECK(render::check_decal(decal).error().message == "the decal's numbers are finite");
    decal.transform[13] = 0.0F;
    auto short_image = std::make_shared<Image>();
    short_image->width = 3;
    short_image->height = 3;
    short_image->pixels.resize(8);
    decal.orm = short_image;
    CHECK(render::check_decal(decal).error().message ==
          "the decal's orm texture is RGBA8 pixels, 1 to 16384 texels across");
    decal.orm = nullptr;
    CHECK(render::check_decals({.decals = {decal}, .filter = static_cast<render::DecalFilter>(9)})
              .error()
              .message == "the decals' filter is not one of Godot's");

    // A texture 2048 texels across fits; forty 16384 across take a row each, 32768 by 2048 texels
    // with levels, past what the atlas holds.
    CHECK(render::check_decals({.decals = {{.albedo = blank(2048, 2048)}}}).ok());
    Decals3D strips;
    for (int i = 0; i < 10; ++i) {
        strips.decals.push_back({.albedo = blank(16384, 1),
                                 .normal = blank(16384, 1),
                                 .orm = blank(16384, 1),
                                 .emission = blank(16384, 1)});
    }
    CHECK(render::check_decals(strips).error().message ==
          "the decals' textures need an atlas of 89391104 texels, past the 33554432 it holds");
}

TEST_CASE("decals are placed as Godot's update_decal_buffer places them") {
    const auto texel = image_of(1, 1, [](std::uint32_t, std::uint32_t) { return Rgba{}; });
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    const auto at = [&](float z) {
        Decal3D decal{.albedo = texel};
        decal.transform = translation(0.0F, 0.0F, z);
        return decal;
    };
    Decal3D sorted = at(-20.0F);
    sorted.sorting_offset = 16.0F; // 4 meters on, nearer than the rest
    Decal3D normal_only = at(-3.0F);
    normal_only.albedo = nullptr;
    normal_only.normal = texel; // draws nothing without an albedo or an emission
    Decal3D gone = at(-60.0F);
    gone.distance_fade = true; // past 40 + 10 meters
    Decal3D fading = at(-45.0F);
    fading.distance_fade = true;
    fading.modulate = {0.5F, 1.0F, 0.0F, 0.8F};
    fading.emission_energy = 3.0F;
    Decal3D flat = at(-7.0F);
    flat.transform[5] = 0.0F; // a box with no height has no inside
    Decal3D glowing = at(-30.0F);
    glowing.albedo = nullptr;
    glowing.emission = texel;
    glowing.cull_mask = 0xFFFFFFFFU;
    scene.decals.decals = {at(-10.0F), at(-5.0F), sorted, normal_only, gone, fading, flat, glowing};
    const detail::DecalAtlas atlas = detail::pack_decal_atlas(scene.decals);
    const detail::PlacedDecals placed = detail::place_decals(scene, atlas);
    REQUIRE(placed.uniforms.size() == 5);
    REQUIRE(placed.boxes.size() == 5);
    // Nearest first, less the sorting offset: each box's corner at its z less half its size.
    const std::array<float, 5> corners{-21.0F, -6.0F, -11.0F, -31.0F, -46.0F};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        CAPTURE(i);
        CHECK(placed.boxes[i][14] == corners.at(i));
        // The box's middle is the middle of its space: x and z from 0 to 1, y from -1 to 1.
        const detail::MeshDecalUniforms& d = placed.uniforms[i];
        const float z = corners.at(i) + 1.0F;
        CHECK(d.rows[0][3] == doctest::Approx(0.5F));
        CHECK(d.rows[1][3] == doctest::Approx(0.0F));
        CHECK((d.rows[2][2] * z) + d.rows[2][3] == doctest::Approx(0.5F));
        CHECK(d.normal == detail::Float4{0.0F, 1.0F, 0.0F, 0.0F});
    }
    // Half faded, 5 meters into its 10: the albedo's alpha and the emission halved; its color
    // linear.
    const detail::MeshDecalUniforms& faded = placed.uniforms[4];
    CHECK(faded.modulate[0] == doctest::Approx(0.214041F));
    CHECK(faded.modulate[1] == 1.0F);
    CHECK(faded.modulate[3] == doctest::Approx(0.4F));
    CHECK(faded.params[0] == doctest::Approx(1.5F));
    CHECK(faded.params[2] == 0.3F);
    CHECK(faded.params[3] == 0.3F);
    CHECK(faded.rects[0] == atlas.rect(texel));
    // Cull masks keep the 20 render layers.
    CHECK(placed.uniforms[0].options[0] == 0xFFFFFU);
    CHECK(placed.uniforms[3].options[0] == 0xFFFFFU);

    // Compatibility draws none, as Godot's.
    scene.method = RenderMethod::Compatibility;
    CHECK(detail::place_decals(scene, atlas).uniforms.empty());
}

TEST_CASE("clusters hold each decal in the tiles and depth slices its box reaches") {
    const auto texel = image_of(1, 1, [](std::uint32_t, std::uint32_t) { return Rgba{}; });
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(90.0F, 1.0F, 0.1F, 100.0F);
    // A box of 2 meters 10 ahead, around the middle of the screen; another behind the camera.
    Decal3D ahead{.albedo = texel};
    ahead.transform = translation(0.0F, 0.0F, -10.0F);
    Decal3D behind{.albedo = texel};
    behind.transform = translation(0.0F, 0.0F, 10.0F);
    scene.decals.decals = {ahead, behind};
    const detail::DecalAtlas atlas = detail::pack_decal_atlas(scene.decals);
    const detail::LightClusters clusters = detail::build_clusters(scene, 128, 128, &atlas);
    CHECK(clusters.words == 1);
    CHECK(clusters.decal_words == 1);
    CHECK(clusters.decals.size() == 2);
    CHECK(clusters.stride() == 1 + 32 + 1 + 32);
    CHECK(clusters.uniform()[3] == 1.0F);
    const auto cell = [&](std::uint32_t x, std::uint32_t y) {
        return ((std::size_t{y} * clusters.tiles_across) + x) * clusters.stride() + 1 + 32;
    };
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const std::uint32_t bits = clusters.cells.at(cell(x, y));
            CHECK((bits & 1U) == ((x == 1 || x == 2) && (y == 1 || y == 2) ? 1U : 0U));
            CHECK((bits & 2U) == 0);
        }
    }
    // From 8.99 to 11.01 meters ahead: slices 2 and 3, each holding decal 0 alone.
    CHECK(clusters.cells.at(cell(1, 1) + 1 + 1) == 0);
    CHECK(clusters.cells.at(cell(1, 1) + 1 + 2) == (0U | (1U << 16U)));
    CHECK(clusters.cells.at(cell(1, 1) + 1 + 3) == (0U | (1U << 16U)));
    CHECK(clusters.cells.at(cell(1, 1) + 1 + 4) == 0);

    // Without decals, the clusters keep their old size.
    scene.decals.decals.clear();
    const detail::LightClusters plain = detail::build_clusters(scene, 128, 128, &atlas);
    CHECK(plain.decal_words == 0);
    CHECK(plain.stride() == 1 + 32);
}

TEST_CASE("decal filters come from project.json's rendering.textures.decals.filter") {
    CHECK(render::decal_filter(parse(R"({"textures": {"decals": {"filter": "nearest"}}})")) ==
          render::DecalFilter::Nearest);
    CHECK(render::decal_filter(
              parse(R"({"textures": {"decals": {"filter": "linear_mipmaps_anisotropic"}}})")) ==
          render::DecalFilter::LinearMipmapsAnisotropic);
    CHECK(render::decal_filter(parse(R"({"textures": {"decals": {"filter": "fuzzy"}}})")) ==
          render::DecalFilter::LinearMipmaps);
    CHECK(render::decal_filter(parse(R"({})")) == render::DecalFilter::LinearMipmaps);
}

TEST_CASE("decals change the albedo, normal, ORM and emission of what they cover, on every "
          "device") {
    const auto software = render::create_software_device();
    render::Scene3D plain_scene = decal_scene(render::DecalFilter::LinearMipmaps);
    plain_scene.decals.decals.clear();
    const Image plain = draw(*software, plain_scene);
    for (const render::DecalFilter filter :
         {render::DecalFilter::LinearMipmaps, render::DecalFilter::Nearest,
          render::DecalFilter::Linear, render::DecalFilter::NearestMipmaps}) {
        CAPTURE(static_cast<int>(filter));
        const render::Scene3D scene = decal_scene(filter);
        const Image decaled = draw(*software, scene);
        // Red on the decal's left, blue on its right (in front of the camera, at the bottom of
        // its texture), the floor as it was outside the box and where the albedo is transparent.
        const Rgba left = pixel(decaled, seen_at(scene, {-1.0F, 0.0F, 1.0F})[0],
                                seen_at(scene, {-1.0F, 0.0F, 1.0F})[1]);
        const Rgba right = pixel(decaled, seen_at(scene, {1.0F, 0.0F, 1.5F})[0],
                                 seen_at(scene, {1.0F, 0.0F, 1.5F})[1]);
        CHECK(left[0] > left[2] + 60);
        CHECK(right[2] > right[0] + 30);
        for (const std::array<float, 3> outside :
             {std::array<float, 3>{3.0F, 0.0F, 1.0F}, std::array<float, 3>{-1.0F, 0.0F, -1.95F}}) {
            const auto [x, y] = seen_at(scene, outside);
            CHECK(pixel(decaled, x, y) == pixel(plain, x, y));
        }
        // The emission dot glows green in the middle.
        const auto [mx, my] = seen_at(scene, {0.0F, 0.0F, 0.0F});
        CHECK(pixel(decaled, mx, my)[1] > 200);

        for (auto& device : slash::test::hardware_devices()) {
            CAPTURE(render::backend_name(device->backend()));
            // The OpenGL devices draw Compatibility, which has no decals, as Godot's.
            const bool opengl = device->backend() == render::Backend::OpenGl ||
                                device->backend() == render::Backend::OpenGlEs;
            const auto [differing, worst] =
                differences(draw(*device, scene), opengl ? plain : decaled);
            CAPTURE(worst);
            CHECK(differing <= 12);
        }
    }

    // A decal draws only on meshes whose layers its cull mask shares, and Compatibility none.
    render::Scene3D masked = decal_scene(render::DecalFilter::LinearMipmaps);
    masked.decals.decals[0].cull_mask = 2;
    CHECK(draw(*software, masked).pixels == plain.pixels);
    render::Scene3D compatibility = decal_scene(render::DecalFilter::LinearMipmaps);
    compatibility.method = RenderMethod::Compatibility;
    plain_scene.method = RenderMethod::Compatibility;
    CHECK(draw(*software, compatibility).pixels == draw(*software, plain_scene).pixels);
}
