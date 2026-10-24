#include "../engine/render/src/mesh_shading.hpp"
#include "devices.hpp"

#include "slash/render/device.hpp"
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
#include <vector>

namespace render = slash::render;
using render::Image;
using render::Light3D;
using render::LightType;
using render::RenderDevice;
using render::RenderMethod;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Float3 = std::array<float, 3>;

namespace {

// Whether light (its index in the clusters) may reach tile (x, y), and the slice range there.
bool reaches(const render::detail::LightClusters& clusters, std::uint32_t x, std::uint32_t y,
             std::uint32_t light) {
    const std::size_t cell = ((std::size_t{y} * clusters.tiles_across) + x) *
                             (clusters.words + render::detail::kClusterSlices);
    return (clusters.cells.at(cell + (light / 32)) & (1U << (light % 32))) != 0;
}
std::uint32_t slice_range(const render::detail::LightClusters& clusters, std::uint32_t x,
                          std::uint32_t y, std::uint32_t slice) {
    const std::size_t cell = ((std::size_t{y} * clusters.tiles_across) + x) *
                             (clusters.words + render::detail::kClusterSlices);
    return clusters.cells.at(cell + clusters.words + slice);
}

// A floor of side 10 around the origin, facing up, its corners clockwise from above.
std::vector<Vertex3D> floor_vertices() {
    const auto corner = [](float x, float z) {
        return Vertex3D{.position = {x, 0.0F, z},
                        .normal = {0.0F, 1.0F, 0.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {0.0F, 0.0F}};
    };
    return {corner(-5.0F, -5.0F), corner(5.0F, -5.0F), corner(5.0F, 5.0F),
            corner(-5.0F, -5.0F), corner(5.0F, 5.0F),  corner(-5.0F, 5.0F)};
}

constexpr std::uint32_t kSize = 128;
constexpr int kGrid = 10;

// The floor seen from above with a dim sun and a grid of 100 small lamps, one per square of the
// floor, each a color of its own: more than Compatibility's 64.
render::Scene3D lamp_grid(RenderMethod method) {
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({0.0F, 10.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 50.0F);
    scene.ambient = {0.1F, 0.1F, 0.1F};
    scene.lights.push_back({.type = LightType::Directional,
                            .color = {1.0F, 1.0F, 1.0F},
                            .energy = 0.1F,
                            .direction = {0.0F, -1.0F, 0.0F}});
    for (int i = 0; i < kGrid * kGrid; ++i) {
        const auto column = static_cast<float>(i % kGrid);
        const auto row = static_cast<float>(i / kGrid);
        scene.lights.push_back(
            {.type = LightType::Omni,
             .color = {0.3F + (0.07F * column), 0.3F + (0.07F * row), 1.0F - (0.07F * column)},
             .energy = 3.0F,
             .position = {-4.5F + column, 0.3F, -4.5F + row},
             .range = 0.45F});
    }
    return scene;
}

// The pixel the point of the floor under lamp i (from 0) is drawn at.
std::array<std::uint32_t, 2> under_lamp(const render::Scene3D& scene, int i) {
    const Float3 floor{-4.5F + static_cast<float>(i % kGrid), 0.0F,
                       -4.5F + static_cast<float>(i / kGrid)};
    const auto apply = [](const Mat4& m, const std::array<float, 4>& v) {
        std::array<float, 4> out{};
        for (std::size_t row = 0; row < 4; ++row) {
            out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                          (m.at(12 + row) * v[3]);
        }
        return out;
    };
    const std::array<float, 4> clip =
        apply(scene.projection, apply(scene.camera, {floor[0], floor[1], floor[2], 1.0F}));
    const float x = ((clip[0] / clip[3]) + 1.0F) * 0.5F * static_cast<float>(kSize);
    const float y = (1.0F - (clip[1] / clip[3])) * 0.5F * static_cast<float>(kSize);
    return {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)};
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
    const Mat4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK(device.draw_mesh(floor.value(), identity, {.roughness = 0.6F}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

// The sum of a pixel's color channels.
int brightness(const Image& image, std::array<std::uint32_t, 2> at) {
    const std::size_t i = ((std::size_t{at[1]} * image.width) + at[0]) * 4;
    return image.pixels.at(i) + image.pixels.at(i + 1) + image.pixels.at(i + 2);
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

TEST_CASE("clusters hold each light in the tiles and depth slices its range reaches") {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(90.0F, 1.0F, 0.1F, 100.0F);
    scene.lights = {
        {.type = LightType::Directional},
        // Ahead, 10 away: the four tiles around the middle, 3.125 units of depth a slice.
        {.type = LightType::Omni, .position = {0.0F, 0.0F, -10.0F}, .range = 1.0F},
        // Behind the camera: nowhere.
        {.type = LightType::Omni, .position = {0.0F, 0.0F, 5.0F}, .range = 1.0F},
        // Around the camera's eye: every tile.
        {.type = LightType::Spot, .position = {0.0F, 0.0F, -0.5F}, .range = 2.0F},
    };
    const render::detail::LightClusters clusters = render::detail::build_clusters(scene, 128, 128);
    CHECK(clusters.tiles_across == 4);
    CHECK(clusters.tiles_down == 4);
    CHECK(clusters.words == 1);
    CHECK(clusters.slices_per_depth == doctest::Approx(0.32));
    CHECK(clusters.uniform()[0] == 4.0F);
    CHECK(clusters.uniform()[1] == 1.0F);
    CHECK(clusters.uniform()[2] == clusters.slices_per_depth);
    // Only the omni and spot lights, nearest first.
    REQUIRE(clusters.lights.size() == 3);
    CHECK(clusters.lights[0].position[2] == 5.0F);
    CHECK(clusters.lights[1].position[2] == -0.5F);
    CHECK(clusters.lights[2].position[2] == -10.0F);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK_FALSE(reaches(clusters, x, y, 0));
            CHECK(reaches(clusters, x, y, 1));
            CHECK(reaches(clusters, x, y, 2) == (x >= 1 && x <= 2 && y >= 1 && y <= 2));
        }
    }
    // The spot light reaches from the eye to slice 0; the omni light from 8.999 to 11.001,
    // slices 2 and 3. Each slice holds the range of lights from its first to one past its last.
    CHECK(slice_range(clusters, 1, 1, 0) == (1U | (2U << 16U)));
    CHECK(slice_range(clusters, 1, 1, 1) == 0);
    CHECK(slice_range(clusters, 1, 1, 2) == (2U | (3U << 16U)));
    CHECK(slice_range(clusters, 1, 1, 3) == (2U | (3U << 16U)));
    CHECK(slice_range(clusters, 1, 1, 4) == 0);
    CHECK(slice_range(clusters, 0, 0, 2) == 0);

    // Compatibility puts every light in the uniforms, Forward+ only the directional ones.
    CHECK(render::detail::mesh_scene_uniforms(scene).camera[3] == 1.0F);
    scene.method = RenderMethod::Compatibility;
    CHECK(render::detail::mesh_scene_uniforms(scene).camera[3] == 4.0F);
}

TEST_CASE("Forward+ lights every pixel as Compatibility does when there are 64 lights or fewer") {
    render::Scene3D forward = lamp_grid(RenderMethod::ForwardPlus);
    forward.lights.resize(render::kMaxLights3D);
    render::Scene3D compatibility = forward;
    compatibility.method = RenderMethod::Compatibility;
    const auto device = render::create_software_device();
    const auto [differing, worst] =
        differences(draw(*device, forward), draw(*device, compatibility));
    CAPTURE(worst);
    // Only the order the lights are added in differs.
    CHECK(differing == 0);
    CHECK(worst <= 1);
}

TEST_CASE("Forward+ draws every lamp; Compatibility and the OpenGL devices the first 64") {
    const render::Scene3D forward = lamp_grid(RenderMethod::ForwardPlus);
    render::Scene3D compatibility = forward;
    compatibility.method = RenderMethod::Compatibility;
    const auto software = render::create_software_device();
    const Image all = draw(*software, forward);
    const Image first = draw(*software, compatibility);
    // The scene's 64 lights are the sun and the first 63 lamps.
    const std::array<std::uint32_t, 2> lit = under_lamp(forward, 62);
    const std::array<std::uint32_t, 2> unlit = under_lamp(forward, 63);
    const std::array<std::uint32_t, 2> last = under_lamp(forward, 99);
    // Without its lamp, a square of the floor has only the sun and the ambient light.
    const int dark = brightness(first, unlit);
    const auto near_dark = [&](int value) { return std::abs(value - dark) <= 6; };
    CHECK(brightness(all, lit) > dark + 100);
    CHECK(brightness(all, unlit) > dark + 100);
    CHECK(brightness(all, last) > dark + 100);
    CHECK(brightness(first, lit) == brightness(all, lit));
    CHECK(near_dark(brightness(first, last)));

    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        const Image image = draw(*device, forward);
        const auto [differing, worst] = differences(image, opengl ? first : all);
        CAPTURE(worst);
        CHECK(differing <= 20);
        CHECK(opengl == near_dark(brightness(image, last)));
    }
}
