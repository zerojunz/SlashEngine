#include "../engine/render/src/ssr.hpp"
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
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using detail::SsrPass;
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no SSR.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

int channel(const Image& image, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return image.pixels.at(((std::size_t{y} * image.width + x) * 4) + c);
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

// A square facing +Z, one unit across, its corners clockwise from the front.
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

// What draw_mirror draws.
struct Mirror {
    render::Ssr3D ssr{};
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    float roughness = 0.1F;   // the floor's
    Material3D standing{};    // the square standing on the floor in front of the wall
    std::uint32_t size = 64;  // the frame's width and height
    std::uint32_t frames = 1; // drawn in a row on one target
    bool orthogonal = false;  // seen in an orthogonal projection
    float turn = 0.0F;        // how far the camera moves right each frame
    bool glass_floor = false; // the floor drawn as transparent, at 90% alpha
};

// A shiny metal floor before a red wall half a meter behind the middle, lit by grey ambient light,
// seen in perspective from above the floor, and a square standing on the floor in front of the
// wall, to the left: SSR reflects the wall and the square in the floor, which has no other
// reflection to show, since the scene has no sky.
Image draw_mirror(RenderDevice& device, const Mirror& mirror) {
    auto target =
        device.create_texture({.width = mirror.size, .height = mirror.size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(), {});
    REQUIRE(mesh.ok());
    for (std::uint32_t frame = 0; frame < mirror.frames; ++frame) {
        render::Scene3D scene;
        scene.method = mirror.method;
        const float x = mirror.turn * static_cast<float>(frame);
        scene.camera = render::look_at({x, 1.0F, 2.5F}, {x * 0.5F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F});
        scene.projection = mirror.orthogonal ? render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F)
                                             : render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
        scene.ambient = {0.6F, 0.6F, 0.6F};
        scene.ssr = mirror.ssr;
        REQUIRE(
            device
                .begin_frame(
                    {.target = target.value(), .clear = {0.1F, 0.2F, 0.3F, 1.0F}, .scene = scene})
                .ok());
        // The square laid down 4 times as large at the origin, one standing 4 wide and 2 tall
        // behind it, and one standing half a meter across on the floor in front of the wall.
        constexpr std::array<float, 16> kFloor{4, 0, 0, 0, 0, 0, -4, 0, 0, 4, 0, 0, 0, 0, 0, 1};
        constexpr std::array<float, 16> kWall{4, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 1, -0.5F, 1};
        constexpr std::array<float, 16> kStanding{0.5F, 0, 0, 0, 0,     0.5F,  0,    0,
                                                  0,    0, 1, 0, -0.6F, 0.25F, 0.2F, 1};
        const Material3D floor{.albedo = {1.0F, 1.0F, 1.0F, mirror.glass_floor ? 0.9F : 1.0F},
                               .metallic = 1.0F,
                               .roughness = mirror.roughness,
                               .transparency = mirror.glass_floor ? render::Transparency::Alpha
                                                                  : render::Transparency::Disabled,
                               .cull = render::CullMode::Disabled};
        const Material3D wall{.albedo = {1.0F, 0.2F, 0.1F, 1.0F},
                              .roughness = 1.0F,
                              .cull = render::CullMode::Disabled};
        REQUIRE(device.draw_mesh(mesh.value(), kFloor, floor).ok());
        REQUIRE(device.draw_mesh(mesh.value(), kWall, wall).ok());
        Material3D standing = mirror.standing;
        standing.cull = render::CullMode::Disabled;
        REQUIRE(device.draw_mesh(mesh.value(), kStanding, standing).ok());
        REQUIRE(device.end_frame().ok());
    }
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

// The red channel at a point.
int red(const Image& image, std::array<std::uint32_t, 2> point) {
    return channel(image, point[0], point[1], 0);
}

// Points in draw_mirror's 64x64 frame: the floor where it reflects the wall left and right of
// the square, and where it reflects the square; the wall above the floor.
constexpr std::array<std::uint32_t, 2> kWallLeft{6, 40};
constexpr std::array<std::uint32_t, 2> kWallRight{50, 40};
constexpr std::array<std::uint32_t, 2> kSquare{18, 44};
constexpr std::array<std::uint32_t, 2> kWall{40, 10};

bool sized(detail::PostSize size, std::uint32_t width, std::uint32_t height) {
    return size.width == width && size.height == height;
}

std::size_t count(const detail::SsrPlan& plan, SsrPass pass) {
    return static_cast<std::size_t>(std::ranges::count_if(
        plan.steps, [&](const detail::SsrStep& step) { return step.pass == pass; }));
}

const detail::SsrStep& step_of(const detail::SsrPlan& plan, SsrPass pass) {
    return *std::ranges::find_if(plan.steps,
                                 [&](const detail::SsrStep& step) { return step.pass == pass; });
}

// A point times a matrix of columns, divided by w.
std::array<float, 3> project(const std::array<float, 16>& m, std::array<float, 3> p) {
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) =
            (m.at(row) * p[0]) + (m.at(4 + row) * p[1]) + (m.at(8 + row) * p[2]) + m.at(12 + row);
    }
    return {out[0] / out[3], out[1] / out[3], out[2] / out[3]};
}

std::array<float, 16> times(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> out{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a.at((k * 4) + row) * b.at((column * 4) + k);
            }
            out.at((column * 4) + row) = sum;
        }
    }
    return out;
}

} // namespace

TEST_CASE("SSR plans Godot's passes at half and full size") {
    using detail::kSsrColor;
    using detail::kSsrFinal;
    using detail::kSsrHiz;
    using detail::kSsrRows;
    // At half size a 64x48 frame traces at 32x24, with levels down to 2x1.
    const detail::SsrPlan half = detail::ssr_plan({}, 64, 48);
    CHECK(sized(half.size, 32, 24));
    CHECK(half.levels == 5);
    CHECK(sized(half.sizes.at(kSsrHiz), 32, 24));
    CHECK(sized(half.sizes.at(kSsrHiz + 4), 2, 1));
    CHECK(sized(half.sizes.at(kSsrColor + 1), 16, 12));
    // The filter's rows keep three more above and below, for the blur along columns.
    CHECK(sized(half.sizes.at(kSsrRows + 1), 16, 18));
    CHECK(half.sizes.at(kSsrRows).width == 0);
    CHECK(sized(half.sizes.at(detail::kSsrNormalsHalf), 32, 24));
    CHECK(sized(half.sizes.at(detail::kSsrMipLevel), 32, 24));
    CHECK(sized(half.sizes.at(kSsrFinal), 64, 48));
    CHECK(count(half, SsrPass::DownsampleDepth) == 1);
    CHECK(count(half, SsrPass::DownsampleNormals) == 1);
    CHECK(count(half, SsrPass::Hiz) == 4);
    CHECK(count(half, SsrPass::CopyLevels) == 2);
    CHECK(count(half, SsrPass::FilterRows) == 4);
    CHECK(count(half, SsrPass::FilterColumns) == 4);
    CHECK(half.steps.back().pass == SsrPass::Resolve);
    CHECK(half.steps.back().target == kSsrFinal);
    // The trace reads the normals at its size, Hi-Z's levels and the frame before.
    const detail::SsrStep& trace = step_of(half, SsrPass::Trace);
    CHECK(trace.target == kSsrColor);
    CHECK(trace.slots.at(1) == detail::kSsrNormalsHalf);
    CHECK(trace.slots.at(2) == detail::kSsrHizMips);
    CHECK(trace.slots.at(3) == detail::kSsrLastFrame);
    // At full size the depth buffer is Hi-Z's first level and each pixel reads its mip level.
    const detail::SsrPlan full = detail::ssr_plan({.half_size = false}, 64, 48);
    CHECK(sized(full.size, 64, 48));
    CHECK(full.levels == 6);
    CHECK(count(full, SsrPass::CopyDepth) == 1);
    CHECK(count(full, SsrPass::DownsampleDepth) == 0);
    CHECK(full.sizes.at(detail::kSsrNormalsHalf).width == 0);
    CHECK(step_of(full, SsrPass::Trace).slots.at(1) == detail::kSsrNormals);
    CHECK(full.steps.back().pass == SsrPass::Mip);
    // The frame before is kept from the world's colors, at SSR's size.
    const detail::SsrStep last = detail::ssr_last_frame_step();
    CHECK(last.target == detail::kSsrLastFrame);
    CHECK(last.slots.at(0) == detail::kSsrWorld);
}

TEST_CASE("SSR's uniforms are Godot's push constants and scene data") {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 1.0F, 2.5F}, {0.0F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    const detail::SsrCamera camera = detail::ssr_camera(scene);
    // Godot's corrected projection: y runs down, as texture coordinates do.
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(camera.projection.at(i) ==
              (i % 4 == 1 ? -scene.projection.at(i) : scene.projection.at(i)));
    }
    render::Ssr3D ssr{.enabled = true,
                      .max_steps = 32,
                      .fade_in = 0.5F,
                      .fade_out = 3.0F,
                      .depth_tolerance = 0.25F};
    const detail::SsrPlan plan = detail::ssr_plan(ssr.settings, 64, 48);
    const detail::SsrUniforms u =
        detail::ssr_uniforms(ssr, step_of(plan, SsrPass::Trace), camera, camera, 64, 48);
    CHECK(u.options ==
          std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(SsrPass::Trace), 0, 0, 5});
    CHECK(u.sizes == std::array<std::uint32_t, 4>{32, 24, 32, 24});
    CHECK(u.frame == std::array<std::uint32_t, 4>{64, 48, 32, 24});
    CHECK(u.steps[0] == 32);
    CHECK(u.fade == detail::Float4{3.0F, 0.5F, 0.25F, 0.0F});
    // The inverse projection undoes the projection, and seen from where it was last frame, a
    // point lands where it was.
    const std::array<float, 16> round = times(u.projection, u.inv_projection);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(round.at(i) == doctest::Approx(i % 5 == 0 ? 1.0F : 0.0F).epsilon(1e-4));
    }
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(u.reprojection.at(i) == doctest::Approx(i % 5 == 0 ? 1.0F : 0.0F).epsilon(1e-4));
    }
    // When the camera has moved, a point's clip position now goes to where it was before.
    render::Scene3D before = scene;
    before.camera = render::look_at({0.3F, 1.0F, 2.5F}, {0.0F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F});
    const detail::SsrCamera last = detail::ssr_camera(before);
    const detail::SsrUniforms moved =
        detail::ssr_uniforms(ssr, step_of(plan, SsrPass::Trace), camera, last, 64, 48);
    const std::array<float, 3> point{0.2F, 0.1F, -0.4F};
    const std::array<float, 3> now = project(times(camera.projection, camera.camera), point);
    const std::array<float, 3> then = project(times(last.projection, last.camera), point);
    const std::array<float, 3> reprojected = project(moved.reprojection, now);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(reprojected.at(i) == doctest::Approx(then.at(i)).epsilon(1e-3));
    }
    // A Hi-Z level from a source of odd size reads its third column and row too.
    const detail::SsrPlan odd = detail::ssr_plan({}, 90, 46);
    const detail::SsrStep& hiz = step_of(odd, SsrPass::Hiz);
    const detail::SsrUniforms odd_u = detail::ssr_uniforms(ssr, hiz, camera, camera, 90, 46);
    CHECK(odd_u.sizes == std::array<std::uint32_t, 4>{22, 11, 45, 23});
    CHECK(odd_u.options[2] == (detail::kSsrOddWidth | detail::kSsrOddHeight));
    // An orthogonal camera is flagged.
    scene.projection = render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F);
    const detail::SsrUniforms flat = detail::ssr_uniforms(
        ssr, step_of(plan, SsrPass::Trace), detail::ssr_camera(scene), camera, 64, 48);
    CHECK(flat.options[2] == detail::kSsrOrthogonal);
}

TEST_CASE("SSR's pyramid keeps the nearest depth, as Godot's downsample and Hi-Z") {
    // Reversed depth: the nearest is the largest. A 5x3 frame halves to 2x1, each texel taking
    // the nearest of the pixels it covers.
    const std::vector<float> depth{0.1F, 0.2F, 0.3F, 0.4F, 0.9F, //
                                   0.5F, 0.1F, 0.1F, 0.2F, 0.1F, //
                                   0.1F, 0.1F, 0.8F, 0.1F, 0.1F};
    const render::Ssr3D ssr{.enabled = true};
    const detail::SsrPlan plan = detail::ssr_plan(ssr.settings, 5, 3);
    REQUIRE(sized(plan.size, 2, 1));
    const detail::SsrStep& down = step_of(plan, SsrPass::DownsampleDepth);
    const detail::SsrUniforms u = detail::ssr_uniforms(ssr, down, {}, {}, 5, 3);
    CHECK(u.options[2] == (detail::kSsrOddWidth | detail::kSsrOddHeight));
    detail::SsrImage target{.width = 2, .height = 1, .channels = 1, .values = {}};
    detail::run_ssr_pass(u, down, {}, {.depth = depth}, target);
    // Since the source is odd, each texel covers a third column and row past its 2x2, as Godot's
    // do: the first takes 0.8 from the second's corner.
    CHECK(target.values == std::vector<float>{0.8F, 0.9F});
}

TEST_CASE("SSR settings come from project.json's rendering.environment.screen_space_reflection") {
    auto parsed = slash::json::parse(R"({"half_size": false})");
    REQUIRE(parsed.ok());
    CHECK_FALSE(render::ssr_settings_from_json(parsed.value()).half_size);
    // What is missing or not valid keeps its default.
    auto wrong = slash::json::parse(R"({"half_size": 1})");
    REQUIRE(wrong.ok());
    CHECK(render::ssr_settings_from_json(wrong.value()) == render::SsrSettings{});
}

TEST_CASE("Forward+ SSR reflects the wall and a square standing on a shiny floor") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // The scene has no sky, so without SSR the metal floor reflects nothing and stays black.
        const Image plain = draw_mirror(*device, {});
        CHECK(red(plain, kWallLeft) < 3);
        CHECK(red(plain, kSquare) < 3);
        // With it, the floor shows the red wall behind and the white square standing on it.
        const Image on = draw_mirror(*device, {.ssr = {.enabled = true}});
        for (const auto point : {kWallLeft, kWallRight}) {
            CHECK(red(on, point) > 90);
            CHECK(red(on, point) > 4 * channel(on, point[0], point[1], 1));
        }
        CHECK(red(on, kSquare) > 90);
        CHECK(std::abs(red(on, kSquare) - channel(on, kSquare[0], kSquare[1], 2)) < 12);
        // The rough wall takes no reflections, as in Godot, which fades them out from roughness
        // 0.6 to 0.7.
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(channel(on, kWall[0], kWall[1], c) == channel(plain, kWall[0], kWall[1], c));
        }
        // At full size the reflections are the same, sharper at their edges.
        render::Ssr3D full{.enabled = true};
        full.settings.half_size = false;
        const Image sharp = draw_mirror(*device, {.ssr = full});
        for (const auto point : {kWallLeft, kWallRight, kSquare}) {
            CHECK(std::abs(red(sharp, point) - red(on, point)) <= 4);
        }
        // In an orthogonal projection, and with the camera moving between frames, the floor
        // reflects as well.
        const Image flat = draw_mirror(*device, {.ssr = {.enabled = true}, .orthogonal = true});
        CHECK(red(flat, kWallLeft) > 90);
        CHECK(red(flat, kSquare) > 90);
        const Image moving =
            draw_mirror(*device, {.ssr = {.enabled = true}, .frames = 3, .turn = 0.2F});
        CHECK(red(moving, kWallLeft) > 90);
        CHECK(red(moving, kWallRight) > 90);
        // A ray of 4 steps reaches only part of the way to the wall, as Godot's.
        const Image short_rays = draw_mirror(*device, {.ssr = {.enabled = true, .max_steps = 4}});
        CHECK(red(short_rays, kWallLeft) < red(on, kWallLeft) - 20);
    }
}

TEST_CASE("SSR dims on rough floors and leaves transparent meshes and Compatibility alone") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // At roughness 0.5 the reflection reads a blurrier level, a little dimmer.
        const Image smooth = draw_mirror(*device, {.ssr = {.enabled = true}});
        const Image rough = draw_mirror(*device, {.ssr = {.enabled = true}, .roughness = 0.5F});
        CHECK(red(rough, kWallLeft) > 60);
        CHECK(red(rough, kWallLeft) < red(smooth, kWallLeft));
        // From roughness 0.7 there is none.
        CHECK(difference(draw_mirror(*device, {.ssr = {.enabled = true}, .roughness = 0.8F}),
                         draw_mirror(*device, {.roughness = 0.8F})) == 0);
        // A transparent floor takes none, as in Godot.
        CHECK(difference(draw_mirror(*device, {.ssr = {.enabled = true}, .glass_floor = true}),
                         draw_mirror(*device, {.glass_floor = true})) == 0);
        // Godot's Compatibility renderer has no SSR.
        const auto compatibility = render::RenderMethod::Compatibility;
        CHECK(difference(draw_mirror(*device, {.ssr = {.enabled = true}, .method = compatibility}),
                         draw_mirror(*device, {.method = compatibility})) == 0);
    }
}

TEST_CASE("GPUs draw SSR as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    render::Ssr3D full{.enabled = true};
    full.settings.half_size = false;
    const std::vector<Mirror> mirrors{
        {.ssr = {.enabled = true}},
        {.ssr = full},
        {.ssr = {.enabled = true}, .roughness = 0.5F},
        {.ssr = {.enabled = true}, .size = 45},
        {.ssr = full, .size = 45},
        {.ssr = {.enabled = true}, .orthogonal = true},
        {.ssr = {.enabled = true}, .frames = 3, .turn = 0.2F},
        {.ssr = {.enabled = true,
                 .max_steps = 8,
                 .fade_in = 0.5F,
                 .fade_out = 4.0F,
                 .depth_tolerance = 0.1F}},
    };
    for (std::size_t i = 0; i < mirrors.size(); ++i) {
        CAPTURE(i);
        const Image expected = draw_mirror(*software, mirrors[i]);
        const Image drawn = draw_mirror(*gpu, mirrors[i]);
        // Floats round differently on each GPU, which moves a ray's hit by a cell now and then.
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 100);
        CHECK(difference(drawn, expected) <= 40);
    }
}
