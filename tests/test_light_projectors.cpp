// Light projectors as Godot 4.6's Forward+ casts them (a texture through a spot light's cone or
// an omni light's dual paraboloid, from the decal atlas), SlashEngine's scrolling light functions,
// and IES profiles read from LM-63 files and baked into projector textures.

#include "../engine/render/src/decal.hpp"
#include "../engine/render/src/mesh_shading.hpp"
#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/ies.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = slash::render::detail;
using render::Image;
using render::RenderDevice;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Rgba = std::array<std::uint8_t, 4>;

namespace {

// A downlight's profile: 1000 candelas straight down, fading to none level with the lamp.
constexpr std::string_view kDownlight = R"(IESNA:LM-63-2002
[TEST] downlight
[MANUFAC] SlashEngine
TILT=NONE
1 1000 1 5 1 1 2 0.1 0.1 0
1 1 50
0 22.5 45 67.5 90
0
1000 800 400 100 0
)";

// A wall washer's: brighter toward its 0 degree side than its 90 degree side, measured over one
// quadrant, with a tilt table to skip.
constexpr std::string_view kQuadrant = R"(IESNA91
TILT=INCLUDE
1
3
0 45 90
1 0.9 0.8
1 -1 1 3 2 1 1 0.1 0.1 0
1 1 30
0 90 180
0 90
600 300 0
200 100 0
)";

template <typename Color>
std::shared_ptr<const Image> image_of(std::uint32_t width, std::uint32_t height, Color color) {
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Rgba texel = color(x, y);
            image->pixels.insert(image->pixels.end(), texel.begin(), texel.end());
        }
    }
    return image;
}

Rgba texel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

Mat4 identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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

constexpr std::uint32_t kSize = 64;

// Red on the left half, green on the right.
std::shared_ptr<const Image> halves() {
    return image_of(16, 16, [](std::uint32_t x, std::uint32_t /*y*/) {
        return x < 8 ? Rgba{255, 0, 0, 255} : Rgba{0, 255, 0, 255};
    });
}

// The floor seen from above, lit only by a spot light 3 meters up shining down through a
// projector of two halves, its +Y toward -Z, so the texture's left falls on the floor's -X.
render::Scene3D spot_scene() {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.01F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 50.0F);
    scene.ambient = {0.0F, 0.0F, 0.0F};
    render::Light3D spot{.type = render::LightType::Spot,
                         .color = {1.0F, 1.0F, 1.0F},
                         .energy = 4.0F,
                         .position = {0.0F, 3.0F, 0.0F},
                         .direction = {0.0F, -1.0F, 0.0F},
                         .range = 8.0F,
                         .spot_angle = 40.0F,
                         .up = {0.0F, 0.0F, -1.0F}};
    spot.projector = halves();
    scene.lights.push_back(spot);
    return scene;
}

// The same floor under an omni light 2 meters up whose projector is red over green: its top half
// faces the light's -Z and its bottom half +Z.
render::Scene3D omni_scene() {
    render::Scene3D scene = spot_scene();
    render::Light3D omni{.type = render::LightType::Omni,
                         .color = {1.0F, 1.0F, 1.0F},
                         .energy = 4.0F,
                         .position = {0.0F, 2.0F, 0.0F},
                         .range = 8.0F};
    omni.projector = image_of(8, 16, [](std::uint32_t /*x*/, std::uint32_t y) {
        return y < 8 ? Rgba{255, 0, 0, 255} : Rgba{0, 255, 0, 255};
    });
    scene.lights = {omni};
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
    CHECK(device.draw_mesh(floor.value(), identity(), {.roughness = 0.9F}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

// The pixel a point of the floor is drawn at.
Rgba seen(const Image& image, const render::Scene3D& scene, std::array<float, 3> point) {
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
    return texel(image, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
}

// How many pixels differ by more than 2 in a channel.
int differing(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    int count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        int most = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            most = std::max(most, std::abs(a.pixels.at(i + c) - b.pixels.at(i + c)));
        }
        count += most > 2 ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("IES profiles are read from LM-63 files, with their symmetry") {
    auto read = render::parse_ies(kDownlight);
    REQUIRE_MESSAGE(read.ok(), read.error().message);
    const render::IesProfile& down = read.value();
    CHECK(down.vertical.size() == 5);
    CHECK(down.horizontal.size() == 1);
    CHECK(down.lumens == 1000.0F);
    CHECK(render::ies_candela(down, 0.0F, 0.0F) == 1000.0F);
    CHECK(render::ies_candela(down, 11.25F, 123.0F) == doctest::Approx(900.0F));
    CHECK(render::ies_candela(down, 90.0F, 0.0F) == 0.0F);
    CHECK(render::ies_candela(down, 120.0F, 0.0F) == 0.0F); // past what was measured

    // A tilt table is skipped, -1 lumens means absolute photometry, and a quadrant is mirrored
    // the whole way round.
    auto quadrant = render::parse_ies(kQuadrant);
    REQUIRE_MESSAGE(quadrant.ok(), quadrant.error().message);
    CHECK(quadrant.value().lumens == 0.0F);
    CHECK(quadrant.value().candela.size() == 6);
    CHECK(render::ies_candela(quadrant.value(), 0.0F, 0.0F) == 600.0F);
    CHECK(render::ies_candela(quadrant.value(), 0.0F, 90.0F) == 200.0F);
    CHECK(render::ies_candela(quadrant.value(), 0.0F, 270.0F) == 200.0F);
    CHECK(render::ies_candela(quadrant.value(), 0.0F, 180.0F) == 600.0F);
    CHECK(render::ies_candela(quadrant.value(), 0.0F, 135.0F) == doctest::Approx(400.0F));

    // What can't be read says why.
    const auto refused = [](std::string_view text, std::string_view words) {
        auto parsed = render::parse_ies(text);
        REQUIRE_FALSE(parsed.ok());
        CHECK_MESSAGE(parsed.error().message.find(words) != std::string::npos,
                      parsed.error().message);
    };
    refused("no tilt here", "not an IES LM-63 file");
    refused("TILT=lamp.tilt\n1 1000 1", "names a tilt file");
    refused("TILT=NONE\n1 1000 1 5 1 3 2 0.1 0.1 0 1 1 50", "only type C");
    refused("TILT=NONE\n1 1000 1 5 1 1 2 0.1 0.1 0 1 1 50 0 22.5", "end early");
    refused("TILT=NONE\n1 1000 1 2 1 1 2 0.1 0.1 0 1 1 50 0 x 0 1 1", "\"x\" is not a number");
}

TEST_CASE("IES profiles bake into spot and omni projector textures") {
    const render::IesProfile down = render::parse_ies(kDownlight).value();
    const Image spot = render::bake_ies_spot(down, 45.0F, 32);
    CHECK(spot.width == 32);
    CHECK(spot.height == 32);
    // Brightest along the axis, fading toward the cone's edge, round about the middle.
    CHECK(texel(spot, 16, 16)[0] > 250);
    CHECK(texel(spot, 16, 16)[3] == 255);
    CHECK(texel(spot, 0, 16)[0] < texel(spot, 8, 16)[0]);
    CHECK(texel(spot, 8, 16)[0] < texel(spot, 16, 16)[0]);
    CHECK(texel(spot, 16, 0) == texel(spot, 0, 16));
    CHECK(render::bake_ies_spot(down, 45.0F, 32).pixels == spot.pixels);

    // An omni light's: the profile's axis down its -Y, which each half's top row faces.
    const Image omni = render::bake_ies_omni(down, 16);
    CHECK(omni.width == 16);
    CHECK(omni.height == 32);
    CHECK(texel(omni, 8, 0)[0] > 200);
    CHECK(texel(omni, 8, 16)[0] > 200);
    CHECK(texel(omni, 8, 15)[0] < 10);
    CHECK(texel(omni, 8, 8)[0] < 10); // level with the lamp, where it gives no light
}

TEST_CASE("clusters carry each light's projector after the frame's decals") {
    render::Scene3D scene = spot_scene();
    render::Decal3D decal;
    decal.size = {1.0F, 1.0F, 1.0F};
    decal.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 0, 4, 1};
    decal.albedo =
        image_of(4, 4, [](std::uint32_t, std::uint32_t) { return Rgba{0, 0, 255, 255}; });
    scene.decals.decals.push_back(decal);
    detail::DecalAtlasCache cache;
    cache.update(scene);
    const detail::DecalAtlas& atlas = cache.atlas();
    REQUIRE(atlas.images.size() == 2); // the decal's texture, then the projector
    const detail::LightClusters clusters = detail::build_clusters(scene, kSize, kSize, &atlas);
    REQUIRE(clusters.lights.size() == 1);
    REQUIRE(clusters.decals.size() == 2);
    CHECK(clusters.decal_count == 1);
    // Not baked, with the first record after the decals.
    CHECK(clusters.lights[0].shadow[1] == 2.0F);
    const detail::MeshDecalUniforms& record = clusters.decals[1];
    CHECK(record.rects[0] == atlas.rect(scene.lights[0].projector));
    CHECK(record.params[0] == 2.0F); // a spot light
    // The world to the light's space: its place is the origin and down, where it shines, is its
    // -Z, so the Z row is the world's up and a point 3 below the lamp is at z -3.
    CHECK(record.rows[2][1] == doctest::Approx(1.0F));
    CHECK(record.rows[2][3] == doctest::Approx(-3.0F));
    detail::MeshSceneUniforms uniforms;
    detail::set_cluster_uniforms(uniforms, clusters, &atlas, scene.decals.filter);
    CHECK(uniforms.decal_info[0] == 1.0F);

    // A projector alone still has the atlas, with no decals to apply.
    scene.decals.decals.clear();
    cache.update(scene);
    const detail::LightClusters alone = detail::build_clusters(scene, kSize, kSize, &cache.atlas());
    CHECK(alone.decal_count == 0);
    CHECK(alone.decal_words == 0);
    CHECK(alone.decals.size() == 1);
    detail::MeshSceneUniforms single;
    detail::set_cluster_uniforms(single, alone, &cache.atlas(), scene.decals.filter);
    CHECK(single.decal_info[0] == 0.0F);
    CHECK(single.decal_info[2] == static_cast<float>(cache.atlas().width));
}

TEST_CASE("spot and omni lights cast their projectors, scrolled, on every device") {
    const auto software = render::create_software_device();
    render::Scene3D spot = spot_scene();
    const Image cast = draw(*software, spot);
    // The texture's red left on the floor's -X, its green right on +X.
    const Rgba left = seen(cast, spot, {-1.0F, 0.0F, 0.0F});
    const Rgba right = seen(cast, spot, {1.0F, 0.0F, 0.0F});
    CHECK(left[0] > 60);
    CHECK(left[1] < 5);
    CHECK(right[1] > 60);
    CHECK(right[0] < 5);
    // Scrolled half way across, the halves swap.
    render::Scene3D scrolled = spot;
    scrolled.lights[0].projector_offset = {0.5F, 0.0F};
    const Image swapped = draw(*software, scrolled);
    CHECK(seen(swapped, spot, {-1.0F, 0.0F, 0.0F})[1] > 60);
    CHECK(seen(swapped, spot, {1.0F, 0.0F, 0.0F})[0] > 60);

    // An omni light's top half falls toward its -Z, the bottom half toward +Z.
    const render::Scene3D omni = omni_scene();
    const Image round = draw(*software, omni);
    const Rgba back = seen(round, omni, {0.0F, 0.0F, -1.0F});
    const Rgba front = seen(round, omni, {0.0F, 0.0F, 1.0F});
    CHECK(back[0] > 60);
    CHECK(back[1] < 5);
    CHECK(front[1] > 60);
    CHECK(front[0] < 5);

    // Without a projector the light is white; Compatibility casts none, as Godot's.
    render::Scene3D plain = spot;
    plain.lights[0].projector = nullptr;
    const Image white = draw(*software, plain);
    const Rgba lit = seen(white, plain, {-1.0F, 0.0F, 0.0F});
    CHECK(lit[0] == lit[1]);
    render::Scene3D compatibility = spot;
    compatibility.method = render::RenderMethod::Compatibility;
    plain.method = render::RenderMethod::Compatibility;
    CHECK(draw(*software, compatibility).pixels == draw(*software, plain).pixels);

    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        if (opengl) {
            continue; // Compatibility, held to the software device above
        }
        CHECK(differing(draw(*device, spot), cast) <= 8);
        CHECK(differing(draw(*device, scrolled), swapped) <= 8);
        CHECK(differing(draw(*device, omni), round) <= 8);
    }
}
