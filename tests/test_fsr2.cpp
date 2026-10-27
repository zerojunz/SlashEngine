#include "../engine/render/src/fsr2.hpp"
#include "../engine/render/src/taa.hpp"
#include "upscale_square.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using namespace slash::test::upscale;

TEST_CASE("FSR 2 draws at Godot's render size with its jitter phases") {
    render::Scene3D scene;
    scene.fsr2 = render::Fsr2Scaling{.scale = 0.77F, .sharpness = 0.2F};
    CHECK(detail::fsr2_render_size(scene, 1920, 1080) == std::array<std::uint32_t, 2>{1478, 831});
    scene.fsr2->scale = 0.0001F;
    CHECK(detail::fsr2_render_size(scene, 100, 100) == std::array<std::uint32_t, 2>{1, 1});
    // ffxFsr2GetJitterPhaseCount: 8 at the display size, 32 at half of it.
    CHECK(detail::fsr2_phase_count(1920, 1920) == 8);
    CHECK(detail::fsr2_phase_count(960, 1920) == 32);
    CHECK(detail::fsr2_phase_count(1478, 1920) == 13);
    // The jitter is the Halton sequence over the render size, within half a render pixel.
    for (std::uint64_t frame = 0; frame < 40; ++frame) {
        const std::array<float, 2> jitter = detail::camera_jitter(frame, 32, 16, 8);
        CHECK(std::abs(jitter[0]) <= 1.0F / 16.0F);
        CHECK(std::abs(jitter[1]) <= 1.0F / 8.0F);
        CHECK(jitter == detail::camera_jitter(frame + 32, 32, 16, 8));
    }
    CHECK(detail::camera_jitter(1, 32, 16, 8)[0] == 0.0F);
    CHECK(detail::fsr2_luma_mip_size(1478, 831) == std::array<std::uint32_t, 2>{46, 25});
    CHECK(detail::fsr2_luma_mip_size(16, 16) == std::array<std::uint32_t, 2>{1, 1});

    // Only Forward+ at a scale under 1, and never a probe's capture; TAA is off with it.
    scene.fsr2->scale = 0.5F;
    scene.use_taa = true;
    CHECK(detail::uses_fsr2(scene));
    CHECK_FALSE(detail::uses_taa(scene));
    CHECK(detail::uses_motion_vectors(scene));
    scene.fsr2->scale = 1.0F;
    CHECK_FALSE(detail::uses_fsr2(scene));
    CHECK(detail::uses_taa(scene));
    scene.fsr2->scale = 0.5F;
    scene.method = render::RenderMethod::Compatibility;
    CHECK_FALSE(detail::uses_fsr2(scene));
    scene.method = render::RenderMethod::ForwardPlus;
    scene.capture = render::ProbeCapture{};
    CHECK_FALSE(detail::uses_fsr2(scene));
}

TEST_CASE("FSR 2's uniforms follow AMD's dispatch from Godot's settings") {
    const std::array<float, 16> projection = render::perspective(60.0F, 2.0F, 0.1F, 100.0F);
    const detail::Fsr2Uniforms uniforms =
        detail::fsr2_uniforms({.render_width = 64,
                               .render_height = 32,
                               .display_width = 128,
                               .display_height = 64,
                               .jitter = {0.5F / 64.0F, -0.25F / 32.0F},
                               .projection = projection,
                               .sharpness = 0.2F,
                               .frame_index = 3,
                               .phases = 32.0F});
    CHECK(uniforms.sizes == std::array<std::int32_t, 4>{64, 32, 128, 64});
    // In render pixels, y down.
    CHECK(uniforms.jitter_scale[0] == doctest::Approx(0.25F));
    CHECK(uniforms.jitter_scale[1] == doctest::Approx(0.125F));
    CHECK(uniforms.jitter_scale[2] == 0.5F);
    CHECK(uniforms.jitter_scale[3] == 0.5F);
    // deviceToViewDepth turns reversed depth back into the distance: at the near plane (1) and
    // the far plane (0).
    const auto distance = [&](float depth) {
        return uniforms.view_depth[1] / (depth - uniforms.view_depth[0]);
    };
    CHECK(distance(1.0F) == doctest::Approx(0.1F));
    CHECK(distance(0.0F) == doctest::Approx(100.0F));
    CHECK(uniforms.view_depth[2] == doctest::Approx(1.0F / projection[0]));
    CHECK(uniforms.view_depth[3] == doctest::Approx(1.0F / projection[5]));
    CHECK(uniforms.options[3] == 0);
    // Godot's sharpness 0.2 is RCAS's 0.9: a lobe scale of 2^(2 * 0.9 - 2).
    CHECK(uniforms.frame[1] == 32.0F);
    CHECK(uniforms.frame[2] == 3.0F);
    CHECK(uniforms.frame[3] == doctest::Approx(std::exp2(-0.2F)));
    CHECK(uniforms.options[2] == 0);
    // 2 turns sharpening off: the last pass copies.
    CHECK(detail::fsr2_uniforms({.render_width = 64,
                                 .render_height = 32,
                                 .display_width = 128,
                                 .display_height = 64,
                                 .projection = projection,
                                 .sharpness = 2.0F})
              .options[2] == 1);
    // An orthogonal camera's depth is linear.
    const detail::Fsr2Uniforms ortho =
        detail::fsr2_uniforms({.render_width = 8,
                               .render_height = 8,
                               .display_width = 16,
                               .display_height = 16,
                               .projection = render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F)});
    CHECK(ortho.options[3] == 1);
}

TEST_CASE("FSR 2's jitter phases follow a change of size one a frame, as AMD's") {
    detail::Fsr2Clock clock;
    clock.advance(32);
    CHECK(clock.frames == 0);
    CHECK(clock.phases == 32.0F);
    clock.advance(32);
    CHECK(clock.frames == 1);
    clock.advance(29);
    clock.advance(29);
    CHECK(clock.phases == 30.0F);
    clock.advance(29);
    clock.advance(29);
    CHECK(clock.phases == 29.0F);
    CHECK(clock.frames == 5);
}

TEST_CASE("Forward+ FSR 2 rebuilds a still square's edges at the full size over its frames") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // Drawn at the full size without it, and with TAA, which smooths the edges.
        const Image plain = draw_square(*device, {.fsr2 = std::nullopt, .frames = 32});
        const Image taa = draw_square(*device, {.fsr2 = std::nullopt, .taa = true, .frames = 32});
        // A first frame is the render size's pixels made larger, then each frame fills in more.
        const Image first = draw_square(*device, {});
        const Image upscaled = draw_square(*device, {.frames = 32});
        CHECK(red(upscaled, 16, 16) > 250);
        CHECK(red(upscaled, 1, 1) == red(plain, 1, 1));
        CHECK(blended(upscaled) > 20);
        CHECK(differing(upscaled, taa, 40) < differing(first, taa, 40));
        CHECK(differing(upscaled, taa, 60) < 300);
        // TAA has no say with FSR 2, and Compatibility has neither.
        CHECK(difference(draw_square(*device, {.taa = true, .frames = 6}),
                         draw_square(*device, {.frames = 6})) == 0);
        const auto compatibility = render::RenderMethod::Compatibility;
        CHECK(difference(
                  draw_square(*device, {.method = compatibility, .frames = 3}),
                  draw_square(*device,
                              {.fsr2 = std::nullopt, .method = compatibility, .frames = 3})) == 0);
        // A scale of 1 draws as without it.
        CHECK(difference(
                  draw_square(*device, {.fsr2 = render::Fsr2Scaling{.scale = 1.0F}, .frames = 3}),
                  draw_square(*device, {.fsr2 = std::nullopt, .frames = 3})) == 0);
    }
}

TEST_CASE("Forward+ FSR 2 follows moving meshes and a moving camera by their motion vectors") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // Moved by its transform, the square ends where the still one is and looks as it does.
        const Image still = draw_square(*device, {.frames = 32});
        const Image moved = draw_square(*device, {.frames = 32, .step = 0.07F});
        CHECK(red(moved, 16, 16) > 250);
        CHECK(blended(moved) > 20);
        CHECK(differing(moved, still, 60) < 200);
        // Seen by a moving camera, it ends as TAA at the full size draws it.
        const Image panned = draw_square(*device, {.frames = 32, .turn = 0.05F});
        const Image taa_panned =
            draw_square(*device, {.fsr2 = std::nullopt, .taa = true, .frames = 32, .turn = 0.05F});
        CHECK(blended(panned) > 20);
        CHECK(differing(panned, taa_panned, 60) < 300);
        const Image seen = draw_square(*device, {.frames = 32, .perspective = true});
        CHECK(red(seen, 16, 16) > 250);
    }
}

TEST_CASE("FSR 2 works out the sky's motion vectors from the depth, as Godot's") {
    // Godot's derive_motion_vector, checked against where a point is now and was in the frame
    // before through a camera that turned and moved, as the prepass's motion vectors are.
    const std::array<float, 16> projection = render::perspective(60.0F, 2.0F, 0.1F, 100.0F);
    const std::array<float, 16> before = render::look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const std::array<float, 16> now = render::look_at({0.5F, 0.2F, 0}, {0.3F, 0.2F, -1}, {0, 1, 0});
    const auto multiply = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
        std::array<float, 16> m{};
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t r = 0; r < 4; ++r) {
                for (std::size_t k = 0; k < 4; ++k) {
                    m.at((c * 4) + r) += a.at((k * 4) + r) * b.at((c * 4) + k);
                }
            }
        }
        return m;
    };
    const auto clip = [](const std::array<float, 16>& m, const std::array<float, 4>& p) {
        std::array<float, 4> out{};
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t k = 0; k < 4; ++k) {
                out.at(r) += m.at((k * 4) + r) * p.at(k);
            }
        }
        return out;
    };
    const std::array<float, 16> view_projection = multiply(projection, now);
    const std::array<float, 16> previous = multiply(projection, before);
    const detail::Fsr2Uniforms uniforms = detail::fsr2_uniforms({.render_width = 64,
                                                                 .render_height = 32,
                                                                 .display_width = 128,
                                                                 .display_height = 64,
                                                                 .projection = projection,
                                                                 .view_projection = view_projection,
                                                                 .previous = previous});
    // A near point, a far one and one on the far plane, where the sky is.
    for (const std::array<float, 4>& point :
         {std::array<float, 4>{0.2F, 0.1F, -2.0F, 1.0F}, std::array<float, 4>{-3, 2, -40, 1},
          std::array<float, 4>{0.6F, 0.2F, -99.9F, 1.0F}}) {
        CAPTURE(point[2]);
        const std::array<float, 4> c = clip(view_projection, point);
        const std::array<float, 2> uv{(c[0] / c[3] * 0.5F) + 0.5F, 0.5F - (c[1] / c[3] * 0.5F)};
        const std::array<float, 2> derived =
            detail::derive_motion_vector(uniforms.reprojection, uv, c[2] / c[3]);
        const std::array<float, 2> expected = detail::motion_vector(c, clip(previous, point));
        CHECK(derived[0] == doctest::Approx(expected[0]).epsilon(1e-3));
        CHECK(derived[1] == doctest::Approx(expected[1]).epsilon(1e-3));
    }
    // A camera that didn't move gives no motion.
    const detail::Fsr2Uniforms still = detail::fsr2_uniforms({.render_width = 64,
                                                              .render_height = 32,
                                                              .display_width = 128,
                                                              .display_height = 64,
                                                              .projection = projection,
                                                              .view_projection = previous,
                                                              .previous = previous});
    const std::array<float, 2> none =
        detail::derive_motion_vector(still.reprojection, {0.3F, 0.7F}, 0.0F);
    CHECK(std::abs(none[0]) < 1e-6F);
    CHECK(std::abs(none[1]) < 1e-6F);

    // A camera turning over a striped sky ends as one that stood still: the frames before are
    // moved with the sky, so its stripes stay sharp.
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image still_sky =
            draw_square(*device, {.frames = 16, .perspective = true, .sky = true});
        const Image turned =
            draw_square(*device, {.frames = 16, .perspective = true, .yaw = 0.03F, .sky = true});
        // Some 1400 channels differ when the sky's motion vectors are left at 0, and the right
        // edge, which the frames before never saw, differs here too.
        CHECK(differing(turned, still_sky, 40) < 400);
    }
}

TEST_CASE("GPUs draw FSR 2 as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    const std::vector<Square> squares{
        {},
        {.frames = 2},
        {.frames = 16},
        {.fsr2 = render::Fsr2Scaling{.scale = 0.5F, .sharpness = 2.0F}, .frames = 8},
        {.frames = 12, .step = 0.07F},
        {.frames = 12, .turn = 0.05F},
        {.fsr2 = render::Fsr2Scaling{.scale = 0.77F, .sharpness = 0.0F},
         .frames = 8,
         .perspective = true},
        {.frames = 10, .perspective = true, .yaw = 0.03F, .sky = true},
    };
    for (std::size_t i = 0; i < squares.size(); ++i) {
        CAPTURE(i);
        const Image expected = draw_square(*software, squares[i]);
        const Image drawn = draw_square(*gpu, squares[i]);
        // Floats round differently on each GPU, which moves an edge's coverage now and then.
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 50);
        CHECK(difference(drawn, expected) <= 24);
    }
}
