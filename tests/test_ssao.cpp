#include "../engine/render/src/ssao.hpp"
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
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using detail::SsaoPass;
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::SsaoQuality;
using render::Vertex3D;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no SSAO.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

int red(const Image& image, std::uint32_t x, std::uint32_t y) {
    return image.pixels.at((std::size_t{y} * image.width + x) * 4);
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

// What draw_corner draws.
struct Corner {
    render::Ssao3D ssao{};
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    Material3D floating{};   // the square in front of the wall
    bool light = false;      // a directional light shining down the wall
    std::uint32_t size = 64; // the frame's width and height
};

// A grey floor meeting a grey wall half a meter behind the middle, lit by grey ambient light, seen
// in perspective from above the floor, and a square floating in front of the wall, to the right:
// SSAO darkens the corner where floor and wall meet and the wall around the square.
Image draw_corner(RenderDevice& device, const Corner& corner) {
    auto target =
        device.create_texture({.width = corner.size, .height = corner.size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(), {});
    REQUIRE(mesh.ok());
    render::Scene3D scene;
    scene.method = corner.method;
    scene.camera = render::look_at({0.0F, 1.2F, 2.5F}, {0.0F, 0.4F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.6F, 0.6F, 0.6F};
    scene.ssao = corner.ssao;
    if (corner.light) {
        scene.lights.push_back({.energy = 0.5F, .direction = {0.0F, -1.0F, -0.2F}});
    }
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.1F, 0.2F, 0.3F, 1.0F}, .scene = scene})
                .ok());
    // The square laid down 4 times as large at the origin, one standing 4 wide and 2 tall on it,
    // and one a meter across floating in front of the wall.
    constexpr std::array<float, 16> kFloor{4, 0, 0, 0, 0, 0, -4, 0, 0, 4, 0, 0, 0, 0, 0, 1};
    constexpr std::array<float, 16> kWall{4, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 1, -0.5F, 1};
    constexpr std::array<float, 16> kFloating{1, 0, 0, 0, 0,    1,    0,     0,
                                              0, 0, 1, 0, 0.9F, 0.9F, -0.3F, 1};
    const Material3D grey{.roughness = 1.0F, .cull = render::CullMode::Disabled};
    REQUIRE(device.draw_mesh(mesh.value(), kFloor, grey).ok());
    REQUIRE(device.draw_mesh(mesh.value(), kWall, grey).ok());
    Material3D floating = corner.floating;
    floating.roughness = 1.0F;
    floating.cull = render::CullMode::Disabled;
    REQUIRE(device.draw_mesh(mesh.value(), kFloating, floating).ok());
    const auto ended = device.end_frame();
    REQUIRE_MESSAGE(ended.ok(), (ended.ok() ? std::string() : ended.error().message));
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? std::string() : image.error().message));
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

// Pixels of a 64-pixel frame of draw_corner: in the corner below the middle, on the floor in
// front of it, on the wall just left of the floating square, and on the floating square.
constexpr std::array<std::uint32_t, 2> kInCorner{20, 34};
constexpr std::array<std::uint32_t, 2> kOnFloor{20, 56};
constexpr std::array<std::uint32_t, 2> kBesideSquare{37, 18};
constexpr std::array<std::uint32_t, 2> kOnSquare{52, 18};

int at(const Image& image, std::array<std::uint32_t, 2> where) {
    return red(image, where[0], where[1]);
}

render::Ssao3D with_quality(SsaoQuality quality) {
    render::Ssao3D ssao{.enabled = true};
    ssao.settings.quality = quality;
    return ssao;
}

constexpr std::array<SsaoQuality, 5> kQualities{SsaoQuality::VeryLow, SsaoQuality::Low,
                                                SsaoQuality::Medium, SsaoQuality::High,
                                                SsaoQuality::Ultra};

bool sized(detail::PostSize size, std::uint32_t width, std::uint32_t height) {
    return size.width == width && size.height == height;
}

std::size_t count(const detail::SsaoPlan& plan, SsaoPass pass) {
    return static_cast<std::size_t>(std::ranges::count_if(
        plan.steps, [&](const detail::SsaoStep& step) { return step.pass == pass; }));
}

} // namespace

TEST_CASE("SSAO plans Godot's passes for each quality") {
    using detail::kSsaoAo;
    using detail::kSsaoFinal;
    using detail::kSsaoPong;
    render::SsaoSettings settings;
    // At half size a 64x48 frame's depth and occlusion are a quarter of its size, 16x12.
    const auto check = [&](SsaoQuality quality, std::size_t depths, std::size_t gathers,
                           std::size_t blurs, SsaoPass interleave) {
        CAPTURE(static_cast<int>(quality));
        settings.quality = quality;
        const detail::SsaoPlan plan = detail::ssao_plan(settings, 64, 48);
        CHECK(count(plan, SsaoPass::Depth) == depths);
        CHECK(count(plan, SsaoPass::Gather) + count(plan, SsaoPass::GatherAdaptive) == gathers);
        CHECK(count(plan, SsaoPass::BlurPlain) + count(plan, SsaoPass::BlurSmart) +
                  count(plan, SsaoPass::BlurWide) ==
              blurs);
        CHECK(plan.steps.back().pass == interleave);
        CHECK(plan.steps.back().target == kSsaoFinal);
        CHECK(sized(plan.sizes.at(kSsaoFinal), 64, 48));
        CHECK(sized(plan.sizes.at(detail::ssao_depth(0, 0)), 16, 12));
        CHECK(sized(plan.sizes.at(kSsaoAo), 16, 12));
        return plan;
    };
    // Very low reads two of the four slices and blurs them once, plainly.
    const detail::SsaoPlan very_low =
        check(SsaoQuality::VeryLow, 2, 2, 2, SsaoPass::InterleaveHalf);
    CHECK(count(very_low, SsaoPass::BlurPlain) == 2);
    CHECK(very_low.sizes.at(detail::ssao_depth(1, 0)).width == 0);
    check(SsaoQuality::Low, 4, 4, 8, SsaoPass::InterleavePlain);
    const detail::SsaoPlan medium = check(SsaoQuality::Medium, 4, 4, 8, SsaoPass::InterleaveSmart);
    CHECK(medium.levels == 1);
    // High reads four levels of depth.
    const detail::SsaoPlan high = check(SsaoQuality::High, 16, 4, 8, SsaoPass::InterleaveSmart);
    CHECK(high.levels == 4);
    CHECK(sized(high.sizes.at(detail::ssao_depth(3, 3)), 2, 1));
    // Ultra gathers a base pass, makes an importance map at half the occlusion's size, blurs it
    // and counts it, then gathers again as it says.
    const detail::SsaoPlan ultra = check(SsaoQuality::Ultra, 16, 4, 8, SsaoPass::InterleaveSmart);
    CHECK(count(ultra, SsaoPass::GatherBase) == 4);
    for (const SsaoPass pass :
         {SsaoPass::ImportanceGenerate, SsaoPass::ImportanceA, SsaoPass::ImportanceB,
          SsaoPass::ImportanceRows, SsaoPass::ImportanceSum}) {
        CHECK(count(ultra, pass) == 1);
    }
    CHECK(sized(ultra.sizes.at(detail::kSsaoImportance), 8, 6));
    CHECK(sized(ultra.sizes.at(detail::kSsaoRows), 1, 2));
    // Blur passes but the last two are wide, and the last pass leaves the occlusion in the slices
    // the interleave reads.
    settings.blur_passes = 4;
    const detail::SsaoPlan four = check(SsaoQuality::Medium, 4, 4, 16, SsaoPass::InterleaveSmart);
    CHECK(count(four, SsaoPass::BlurWide) == 8);
    CHECK(four.steps.back().slots.at(8) == kSsaoAo);
    settings.blur_passes = 3;
    const detail::SsaoPlan three = check(SsaoQuality::Medium, 4, 4, 12, SsaoPass::InterleaveSmart);
    CHECK(three.steps.back().slots.at(8) == kSsaoPong);
    settings.blur_passes = 0;
    check(SsaoQuality::Medium, 4, 4, 0, SsaoPass::InterleaveSmart);
    // At full size they are half the frame's.
    settings.half_size = false;
    const detail::SsaoPlan full = detail::ssao_plan(settings, 64, 48);
    CHECK(sized(full.sizes.at(kSsaoAo), 32, 24));
}

TEST_CASE("SSAO's uniforms are Godot's push constants") {
    const std::array<float, 16> projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    render::Ssao3D ssao{.enabled = true};
    const detail::SsaoPlan plan = detail::ssao_plan(ssao.settings, 64, 48);
    const detail::SsaoStep& gather = *std::ranges::find_if(
        plan.steps, [](const detail::SsaoStep& step) { return step.pass == SsaoPass::Gather; });
    const detail::SsaoUniforms u = detail::ssao_uniforms(ssao, gather, projection, 64, 48);
    // Godot's shader quality is one less than the setting.
    CHECK(u.options[1] == 1);
    CHECK(u.buffers == std::array<std::uint32_t, 4>{16, 12, 8, 6});
    // half_screen_pixel_size, doubled at half size.
    CHECK(u.pixel[0] == doctest::Approx(4.0F / 64.0F));
    CHECK(u.pixel[1] == doctest::Approx(4.0F / 48.0F));
    // NDC_to_view_mul and NDC_to_view_add from the field of view's tangent.
    const float tangent = std::tan(30.0F * 3.14159265F / 180.0F);
    CHECK(u.ndc[0] == doctest::Approx(tangent * 2.0F));
    CHECK(u.ndc[1] == doctest::Approx(tangent * -2.0F));
    CHECK(u.ndc[2] == doctest::Approx(-tangent));
    CHECK(u.ndc[3] == doctest::Approx(tangent));
    CHECK(u.shadow == detail::Float4{1.0F, 2.0F, 1.5F, 0.98F});
    // fade_out_mul and fade_out_add from 50 to 300, the horizon, and inv_radius_near_limit.
    CHECK(u.fade[0] == doctest::Approx(-1.0F / 250.0F));
    CHECK(u.fade[1] == doctest::Approx(1.2F));
    CHECK(u.fade[2] == doctest::Approx(0.06F));
    CHECK(u.fade[3] == doctest::Approx(tangent / 1.2F));
    // neg_inv_radius, load_counter_avg_div, adaptive_sample_limit and detail_intensity.
    CHECK(u.adaptive[0] == doctest::Approx(-1.0F));
    CHECK(u.adaptive[1] == doctest::Approx(9.0F / (8.0F * 6.0F * 255.0F)));
    CHECK(u.adaptive[2] == doctest::Approx(0.5F));
    CHECK(u.adaptive[3] == doctest::Approx(0.5F));
    CHECK(u.blur[0] == doctest::Approx(0.02F));
    // Low quality limits near occlusion more, and very low shrinks the radius too.
    ssao.settings.quality = SsaoQuality::VeryLow;
    const detail::SsaoUniforms very_low = detail::ssao_uniforms(ssao, gather, projection, 64, 48);
    CHECK(very_low.options[1] == 0);
    CHECK(very_low.shadow[0] == doctest::Approx(0.8F));
    CHECK(very_low.fade[3] == doctest::Approx(tangent / 1.8F));
    // ss_effects.cpp's first rotation: no turn, scaled by 1 + (-1.5 - 0.4) * 0.07.
    CHECK(u.rotation[0][0] == doctest::Approx(0.867F));
    CHECK(u.rotation[0][1] == doctest::Approx(0.0F));
    CHECK(u.rotation[0][3] == doctest::Approx(-0.867F));
    // The slice's offset into the frame.
    const detail::SsaoStep& last_slice =
        *std::ranges::find_if(plan.steps, [](const detail::SsaoStep& step) {
            return step.pass == SsaoPass::Gather && step.slice == 3;
        });
    const detail::SsaoUniforms slice = detail::ssao_uniforms(ssao, last_slice, projection, 64, 48);
    CHECK(slice.pass_offset[0] == 1.0F);
    CHECK(slice.pass_offset[1] == 1.0F);
}

TEST_CASE("SSAO's depth pass makes depth linear, as Godot's downsample_depth") {
    // A wall 3 meters in front of a perspective camera, then of an orthogonal one.
    for (const std::array<float, 16>& projection : {render::perspective(60.0F, 1.0F, 0.1F, 20.0F),
                                                    render::orthogonal(2.0F, 1.0F, 0.5F, 10.0F)}) {
        const float clip_z = (projection[10] * -3.0F) + projection[14];
        const float clip_w = (projection[11] * -3.0F) + projection[15];
        const std::vector<float> depth(std::size_t{16} * 16, clip_z / clip_w);
        render::Ssao3D ssao{.enabled = true};
        ssao.settings.quality = SsaoQuality::High;
        const detail::SsaoPlan plan = detail::ssao_plan(ssao.settings, 16, 16);
        for (const detail::SsaoStep& step : plan.steps) {
            if (step.pass != SsaoPass::Depth) {
                continue;
            }
            CAPTURE(step.level);
            const detail::SsaoUniforms u = detail::ssao_uniforms(ssao, step, projection, 16, 16);
            const detail::PostSize size = plan.sizes.at(step.target);
            detail::SsaoImage target{.width = size.width,
                                     .height = size.height,
                                     .channels = 1,
                                     .values =
                                         std::vector<float>(std::size_t{size.width} * size.height)};
            detail::run_ssao_pass(u, step, {}, {.depth = depth}, target);
            for (const float value : target.values) {
                CHECK(value == doctest::Approx(3.0F).epsilon(0.002));
            }
        }
    }
}

TEST_CASE("SSAO settings come from project.json's rendering.environment.ssao") {
    auto parsed = slash::json::parse(R"({"quality": "ultra", "half_size": false,
        "adaptive_target": 0.75, "blur_passes": 4, "fadeout_from": 10, "fadeout_to": 20})");
    REQUIRE(parsed.ok());
    const render::SsaoSettings settings = render::ssao_settings_from_json(parsed.value());
    CHECK(settings.quality == SsaoQuality::Ultra);
    CHECK_FALSE(settings.half_size);
    CHECK(settings.adaptive_target == 0.75F);
    CHECK(settings.blur_passes == 4);
    CHECK(settings.fadeout_from == 10.0F);
    CHECK(settings.fadeout_to == 20.0F);
    // What is missing or not valid keeps its default.
    auto wrong = slash::json::parse(R"({"quality": "best", "blur_passes": 7, "half_size": 1})");
    REQUIRE(wrong.ok());
    CHECK(render::ssao_settings_from_json(wrong.value()) == render::SsaoSettings{});
}

TEST_CASE("Forward+ SSAO darkens corners and what is close to a surface") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image plain = draw_corner(*device, {});
        for (const SsaoQuality quality : kQualities) {
            CAPTURE(static_cast<int>(quality));
            const Image occluded = draw_corner(*device, {.ssao = with_quality(quality)});
            CHECK(at(occluded, kInCorner) < at(plain, kInCorner) - 8);
            CHECK(at(occluded, kBesideSquare) < at(plain, kBesideSquare) - 4);
            // The open floor in front keeps its light.
            CHECK(at(occluded, kOnFloor) >= at(plain, kOnFloor) - 2);
        }
        // No intensity, no occlusion.
        render::Ssao3D none = with_quality(SsaoQuality::Medium);
        none.intensity = 0.0F;
        CHECK(draw_corner(*device, {.ssao = none}).pixels == plain.pixels);
        // A transparent square isn't drawn in the prepass, so the wall behind it isn't darkened,
        // and it takes no occlusion itself, as in Godot.
        const Material3D glass{.albedo = {1.0F, 1.0F, 1.0F, 0.5F},
                               .transparency = render::Transparency::Alpha};
        const Image clear = draw_corner(*device, {.floating = glass});
        const Image clear_occluded =
            draw_corner(*device, {.ssao = with_quality(SsaoQuality::Medium), .floating = glass});
        CHECK(at(clear_occluded, kOnSquare) >= at(clear, kOnSquare) - 1);
        CHECK(at(clear_occluded, kBesideSquare) > at(plain, kBesideSquare) - 4);
        // Direct light is darkened by light_affect times ao_channel_affect.
        const Image lit =
            draw_corner(*device, {.ssao = with_quality(SsaoQuality::Medium), .light = true});
        render::Ssao3D affecting = with_quality(SsaoQuality::Medium);
        affecting.light_affect = 1.0F;
        affecting.ao_channel_affect = 1.0F;
        const Image darker = draw_corner(*device, {.ssao = affecting, .light = true});
        CHECK(at(darker, kInCorner) < at(lit, kInCorner));
        affecting.ao_channel_affect = 0.0F;
        CHECK(draw_corner(*device, {.ssao = affecting, .light = true}).pixels == lit.pixels);
        // Compatibility has no SSAO.
        const Image compatibility =
            draw_corner(*device, {.method = render::RenderMethod::Compatibility});
        CHECK(draw_corner(*device, {.ssao = with_quality(SsaoQuality::Medium),
                                    .method = render::RenderMethod::Compatibility})
                  .pixels == compatibility.pixels);
    }
}

TEST_CASE("GPUs draw SSAO as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        return;
    }
    CAPTURE(render::backend_name(gpu->backend()));
    const auto software = render::create_software_device();
    std::vector<Corner> corners;
    for (const SsaoQuality quality : kQualities) {
        corners.push_back({.ssao = with_quality(quality)});
    }
    Corner full{.ssao = with_quality(SsaoQuality::High)};
    full.ssao.settings.half_size = false;
    corners.push_back(full);
    Corner wide{.ssao = with_quality(SsaoQuality::Medium)};
    wide.ssao.settings.blur_passes = 5;
    wide.ssao.sharpness = 0.5F;
    corners.push_back(wide);
    Corner unblurred{.ssao = with_quality(SsaoQuality::Ultra)};
    unblurred.ssao.settings.blur_passes = 0;
    unblurred.ssao.settings.adaptive_target = 1.0F;
    corners.push_back(unblurred);
    Corner strong{.ssao = with_quality(SsaoQuality::Medium), .light = true};
    strong.ssao.radius = 2.0F;
    strong.ssao.intensity = 4.0F;
    strong.ssao.power = 2.0F;
    strong.ssao.detail = 2.0F;
    strong.ssao.light_affect = 0.5F;
    strong.ssao.ao_channel_affect = 1.0F;
    corners.push_back(strong);
    Corner odd{.ssao = with_quality(SsaoQuality::Ultra), .size = 45};
    corners.push_back(odd);
    for (std::size_t i = 0; i < corners.size(); ++i) {
        CAPTURE(i);
        const Image expected = draw_corner(*software, corners.at(i));
        const Image image = draw_corner(*gpu, corners.at(i));
        CHECK(differing(image, expected, 3) <= image.pixels.size() / 100);
        CHECK(difference(image, expected) <= 40);
    }
}
