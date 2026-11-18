// Culling mesh draws on the GPU and drawing runs of them as instances (shaders/mesh_cull.hlsl and
// mesh_instanced.hlsl): its CPU twin, cull_meshes, box_in_view, which the software device skips
// draws by, and every device drawing the same pixels with the draws it leaves in view.

#include "../engine/render/src/mesh_cull.hpp"
#include "devices.hpp"

#include "slash/core/bits.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::Mat4;
using render::MeshId;
using render::RenderDevice;
using render::Vertex3D;
using render::detail::Float3;

namespace {

std::uint32_t word(float value) {
    return slash::bit_cast<std::uint32_t>(value);
}

Mat4 translation(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

// The camera of the tests: at (0, 0, 4) looking down -z, a square view 60 degrees across.
Mat4 view_projection() {
    return render::multiply(
        render::perspective(60.0F, 1.0F, 0.1F, 100.0F),
        render::look_at({0.0F, 0.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}));
}

// Whether the unit box around (x, y, z) may show to the tests' camera.
bool shows(float x, float y, float z, float half = 0.5F) {
    return render::detail::box_in_view(render::multiply(view_projection(), translation(x, y, z)),
                                       {-half, -half, -half}, {half, half, half});
}

// A square a unit across facing +z, both sides lit.
std::vector<Vertex3D> square() {
    std::vector<Vertex3D> out;
    for (const auto& [x, y] : std::array<std::array<float, 2>, 4>{
             {{-0.5F, -0.5F}, {0.5F, -0.5F}, {-0.5F, 0.5F}, {0.5F, 0.5F}}}) {
        out.push_back({.position = {x, y, 0.0F},
                       .normal = {0.0F, 0.0F, 1.0F},
                       .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                       .uv = {x + 0.5F, 0.5F - y},
                       .rgba = {255, 255, 255, 255}});
    }
    return out;
}

const std::vector<std::uint32_t> kSquareIndices{0, 2, 1, 1, 2, 3};

// The squares at places, drawn by the tests' camera into a 48 pixel target, each its own color or,
// with one_material, all one.
Image draw(RenderDevice& device, MeshId mesh, std::span<const Float3> places,
           bool one_material = false) {
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 1.0F, 1.0F},
                     .direction = {0.0F, 0.0F, -1.0F}}};
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}, .scene = scene}).ok());
    float hue = 0.2F;
    for (const Float3& at : places) {
        CHECK(device
                  .draw_mesh(mesh, translation(at[0], at[1], at[2]),
                             {.albedo = {0.9F, hue, 0.3F, 1.0F},
                              .roughness = 0.6F,
                              .cull = render::CullMode::Disabled})
                  .ok());
        if (!one_material) {
            hue += 0.15F;
        }
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_texture(target.value());
    return image.value();
}

// How many pixels of a and b differ by more than 2 in a channel.
int differing(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    int count = 0;
    for (std::size_t at = 0; at < a.pixels.size(); at += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int{a.pixels.at(at + c)} - int{b.pixels.at(at + c)}) > 2) {
                ++count;
                break;
            }
        }
    }
    return count;
}

// How many pixels of an image aren't the clear color's black.
int lit(const Image& image) {
    int count = 0;
    for (std::size_t at = 0; at < image.pixels.size(); at += 4) {
        if (image.pixels.at(at) > 4 || image.pixels.at(at + 1) > 4 || image.pixels.at(at + 2) > 4) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("a box is out of view only when all its corners are past one edge") {
    CHECK(shows(0.0F, 0.0F, 0.0F));
    // Across the left, right, bottom and top edges at a depth of 4, where the view is 2.3 wide.
    CHECK(shows(-2.6F, 0.0F, 0.0F));
    CHECK(shows(0.0F, 2.6F, 0.0F));
    CHECK_FALSE(shows(-3.5F, 0.0F, 0.0F));
    CHECK_FALSE(shows(3.5F, 0.0F, 0.0F));
    CHECK_FALSE(shows(0.0F, -3.5F, 0.0F));
    CHECK_FALSE(shows(0.0F, 3.5F, 0.0F));
    // Behind the camera and nearer than its near plane (render::perspective's far plane is at
    // infinity).
    CHECK_FALSE(shows(0.0F, 0.0F, 8.0F));
    CHECK_FALSE(shows(0.0F, 0.0F, 3.95F, 0.01F));
    CHECK(shows(0.0F, 0.0F, 3.0F));
    CHECK(shows(0.0F, 0.0F, -900.0F));
    // Off one corner of the view, past two edges but not wholly past either.
    CHECK(shows(2.7F, 2.7F, 0.0F));
    // A box that straddles the camera.
    CHECK(shows(0.0F, 0.0F, 4.0F, 2.0F));
}

TEST_CASE("the margin keeps boxes just past an edge, as jitter and rounding move them") {
    // The right edge at a depth of 4 is at x = 4 tan(30 degrees), 2.309; the margin is 1/64 of w.
    // A flat box from just past it rightward.
    const float edge = 4.0F * std::numbers::inv_sqrt3_v<float>;
    const Mat4 clip = view_projection();
    CHECK(
        render::detail::box_in_view(clip, {edge + 0.02F, -0.5F, 0.0F}, {edge + 1.0F, 0.5F, 0.0F}));
    CHECK_FALSE(
        render::detail::box_in_view(clip, {edge + 0.1F, -0.5F, 0.0F}, {edge + 1.0F, 0.5F, 0.0F}));
}

TEST_CASE("a box turned inside out is never culled") {
    const Mat4 far_away = render::multiply(view_projection(), translation(50.0F, 0.0F, 0.0F));
    CHECK_FALSE(render::detail::box_in_view(far_away, {-1, -1, -1}, {1, 1, 1}));
    CHECK(render::detail::box_in_view(far_away, {1, 1, 1}, {-1, -1, -1}));
    CHECK(render::detail::box_in_view(far_away, {-1, 1, -1}, {1, -1, 1}));
}

TEST_CASE(
    "a draw's record is its clip matrix, its box and its indices; kept draws turn inside out") {
    std::vector<std::uint32_t> records;
    const Mat4 transform = translation(1.0F, 2.0F, 3.0F);
    render::detail::push_cull_record(records, view_projection(), transform,
                                     {.low = {-1, -2, -3}, .high = {4, 5, 6}},
                                     {.first = 12, .count = 36}, false);
    REQUIRE(records.size() == render::detail::kCullRecordWords);
    const Mat4 clip = render::multiply(view_projection(), transform);
    for (std::size_t k = 0; k < 16; ++k) {
        CHECK(records.at(k) == word(clip.at(k)));
    }
    CHECK(records.at(16) == word(-1.0F));
    CHECK(records.at(18) == word(-3.0F));
    CHECK(records.at(19) == word(4.0F));
    CHECK(records.at(21) == word(6.0F));
    CHECK(records.at(22) == 12);
    CHECK(records.at(23) == 36);

    render::detail::push_cull_record(records, view_projection(), transform,
                                     {.low = {-1, -2, -3}, .high = {4, 5, 6}},
                                     {.first = 0, .count = 6}, true);
    REQUIRE(records.size() == 2 * render::detail::kCullRecordWords);
    CHECK(records.at(24 + 16) == word(1.0F));
    CHECK(records.at(24 + 19) == word(-1.0F));
}

TEST_CASE("culling lists each run's draws in view and gives it as many instances") {
    std::vector<std::uint32_t> records;
    const render::detail::Bounds unit{.low = {-0.5F, -0.5F, -0.5F}, .high = {0.5F, 0.5F, 0.5F}};
    // Draws 0, 2 and 3 are one mesh (indices from 6), 1 another; 2 is out of view and 3 kept.
    render::detail::push_cull_record(records, view_projection(), translation(0, 0, 0), unit,
                                     {.first = 6, .count = 30}, false);
    render::detail::push_cull_record(records, view_projection(), translation(9, 0, 0), unit,
                                     {.first = 36, .count = 12}, false);
    render::detail::push_cull_record(records, view_projection(), translation(9, 0, 0), unit,
                                     {.first = 6, .count = 30}, false);
    render::detail::push_cull_record(records, view_projection(), translation(0, 9, 0), unit,
                                     {.first = 6, .count = 30}, true);
    // The camera's order: 3, 2, 0 as one run, then 1.
    const std::vector<std::size_t> order{3, 2, 0, 1};
    const std::vector<render::detail::CullRun> runs = render::detail::cull_runs(
        std::span<const std::size_t>(order),
        [](std::size_t a, std::size_t b) { return (a == 1) == (b == 1); });
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].first == 0);
    CHECK(runs[0].count == 3);
    CHECK(runs[1].first == 3);
    CHECK(runs[1].count == 1);
    render::detail::push_cull_runs(records, runs, order);
    const std::vector<std::uint32_t> out = render::detail::cull_meshes(records, 4, 2);
    REQUIRE(out.size() == render::detail::cull_output_words(4, 2, 4));
    // The runs' arguments: run 0 draws its two draws in view, run 1 none.
    CHECK(std::vector<std::uint32_t>(out.begin(), out.begin() + 10) ==
          std::vector<std::uint32_t>{30, 2, 6, 0, 0, 12, 0, 36, 0, 0});
    // The flags, then run 0's draws in view in the camera's order.
    CHECK(std::vector<std::uint32_t>(out.begin() + 10, out.begin() + 14) ==
          std::vector<std::uint32_t>{1, 0, 0, 1});
    CHECK(out.at(render::detail::cull_list_at(4, 2)) == 3);
    CHECK(out.at(render::detail::cull_list_at(4, 2) + 1) == 0);
    // Kept, every draw shows.
    const std::vector<std::uint32_t> kept = render::detail::cull_meshes(records, 4, 2, true);
    CHECK(kept.at(1) == 3);
    CHECK(kept.at(6) == 1);
    CHECK(kept.at(render::detail::cull_list_at(4, 2) + 1) == 2);
    CHECK(render::detail::cull_runs(std::span<const std::size_t>(), [](auto, auto) {
              return true;
          }).empty());
}

TEST_CASE("an Instance is fourteen float4s, as mesh_instanced.hlsl reads them") {
    std::vector<render::detail::MeshInstanceUniforms> instances;
    render::detail::push_instance(instances, translation(1, 2, 3), {}, translation(4, 5, 6));
    REQUIRE(instances.size() == 1);
    const auto* floats = reinterpret_cast<const float*>(instances.data());
    // The model's last column, then the previous model's (the last four float4s).
    CHECK(floats[12] == 1.0F);
    CHECK(floats[14] == 3.0F);
    CHECK(floats[(10 * 4) + 12] == 4.0F);
    CHECK(floats[(10 * 4) + 14] == 6.0F);
}

TEST_CASE("opaque draws go by depth layer, then mesh, so a layer's draws of a mesh are runs") {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 160.0F);
    // A tenth of the way out is one layer: two meshes alternating at 1 to 4 units, then mesh 1
    // again at 50 units, and a blended draw.
    std::vector<render::detail::MeshDraw> draws;
    const auto add = [&](std::uint32_t mesh, float z, bool blended = false) {
        render::detail::MeshDraw draw;
        draw.mesh = static_cast<MeshId>(mesh);
        draw.transform = translation(0.0F, 0.0F, -z);
        if (blended) {
            draw.material.transparency = render::Transparency::Alpha;
        }
        draws.push_back(draw);
    };
    add(2, 1.0F);
    add(1, 2.0F);
    add(2, 3.0F);
    add(1, 4.0F);
    add(1, 50.0F);
    add(1, 0.5F, true);
    add(2, 5.0F, true);
    const render::detail::Bounds point{};
    const std::vector<std::size_t> order =
        render::detail::mesh_draw_order(std::span<const render::detail::MeshDraw>(draws), scene,
                                        [&](std::size_t) { return point; });
    CHECK(order == std::vector<std::size_t>{1, 3, 0, 2, 4, 6, 5});
}

TEST_CASE("a culling dispatch runs a thread a draw, 64 to a group") {
    using render::detail::cull_groups;
    CHECK(cull_groups(1) == 1);
    CHECK(cull_groups(64) == 1);
    CHECK(cull_groups(65) == 2);
    CHECK(cull_groups(1000) == 16);
}

TEST_CASE("every device draws the meshes culling leaves as the software device does") {
    const std::vector<Vertex3D> vertices = square();
    // In view, across the left edge, across the top, out to the right, below, behind the camera
    // and nearer than its near plane.
    const std::vector<Float3> places{{0.0F, 0.0F, 0.0F}, {-2.4F, 0.3F, 0.0F}, {0.6F, 2.4F, -0.5F},
                                     {3.4F, 0.0F, 0.0F}, {0.0F, -3.4F, 0.0F}, {0.0F, 0.0F, 6.0F},
                                     {0.0F, 0.0F, 3.99F}};
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::drawing_devices();
    REQUIRE(!devices.empty());
    std::optional<Image> expected;
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        auto mesh = device->create_mesh(vertices, kSquareIndices);
        REQUIRE(mesh.ok());
        const Image image = draw(*device, mesh.value(), places);
        // The software device comes first.
        if (!expected) {
            expected = image;
            CHECK(lit(image) > 100);
            // The ones in view and across the edges, as drawn alone.
            const std::vector<Float3> shown(places.begin(), places.begin() + 3);
            CHECK(differing(image, draw(*device, mesh.value(), shown)) == 0);
        } else {
            CHECK(differing(image, *expected) <= 8);
        }
        device->destroy_mesh(mesh.value());
    }
}

TEST_CASE("devices that cull on the GPU leave out a mesh whose box is out of view") {
    // A skinned mesh's box is the one skin_mesh is given, so a box out of view culls a mesh whose
    // vertices are in it: what shows that the GPU, not the CPU, chose.
    const std::vector<Vertex3D> vertices = square();
    std::vector<render::SkinWeights> weights(vertices.size());
    for (render::SkinWeights& w : weights) {
        w.joints = {0, 0, 0, 0, 0, 0, 0, 0};
        w.weights = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    }
    const std::vector<render::SkinJoint> joints{render::SkinJoint{}};
    const std::vector<Float3> center{{0.0F, 0.0F, 0.0F}};
    bool culled_any = false;
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        if (!device->gpu_skinning()) {
            continue;
        }
        culled_any = true;
        auto skin = device->create_skin({.vertices = vertices, .weights = weights});
        REQUIRE(skin.ok());
        auto mesh = device->create_mesh(vertices, kSquareIndices);
        REQUIRE(mesh.ok());
        REQUIRE(device
                    ->skin_mesh(mesh.value(), skin.value(), joints, {}, {-0.5F, -0.5F, -0.1F},
                                {0.5F, 0.5F, 0.1F})
                    .ok());
        CHECK(lit(draw(*device, mesh.value(), center)) > 50);
        REQUIRE(device
                    ->skin_mesh(mesh.value(), skin.value(), joints, {}, {20.0F, -0.5F, -0.1F},
                                {21.0F, 0.5F, 0.1F})
                    .ok());
        CHECK(lit(draw(*device, mesh.value(), center)) == 0);
        device->destroy_mesh(mesh.value());
        device->destroy_skin(skin.value());
    }
    if (!culled_any) {
        MESSAGE("no device here culls on the GPU, so GPU culling isn't checked");
    }
}

TEST_CASE("every device draws a run of one mesh's instances as the software device does") {
    // A grid of squares of one material, partly out of view: in the camera's order, a run of
    // instances a depth layer, culled one by one.
    const std::vector<Vertex3D> vertices = square();
    std::vector<Float3> places;
    for (int row = 0; row < 12; ++row) {
        for (int column = 0; column < 12; ++column) {
            places.push_back({(static_cast<float>(column) * 0.6F) - 3.3F,
                              (static_cast<float>(row) * 0.6F) - 3.3F,
                              -static_cast<float>((row + column) % 3)});
        }
    }
    std::optional<Image> expected;
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto mesh = device->create_mesh(vertices, kSquareIndices);
        REQUIRE(mesh.ok());
        const Image image = draw(*device, mesh.value(), places, true);
        if (!expected) {
            expected = image;
            CHECK(lit(image) > 1000);
        } else {
            CHECK(differing(image, *expected) <= 24);
        }
        device->destroy_mesh(mesh.value());
    }
}
