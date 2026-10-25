#include "../engine/render/src/fsr2.hpp"
#include "../engine/render/src/fsr3.hpp"
#include "upscale_square.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/upscaler.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using namespace slash::test::upscale;

namespace {

// FSR 3 at half the size, as the FSR 2 tests' default.
constexpr render::Fsr2Scaling kFsr3{.scale = 0.5F, .sharpness = 0.2F, .fsr3 = true};

} // namespace

TEST_CASE("FSR 3 is a scaling mode that runs where FSR 2 does") {
    CHECK(render::scaling_mode_problem("fsr3").empty());
    const auto software = render::create_software_device();
    render::Scaling scaling{.mode = "fsr3", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = {}};
    CHECK(render::uses_fsr2(scaling, render::RenderMethod::ForwardPlus, *software));
    CHECK_FALSE(render::uses_fsr2(scaling, render::RenderMethod::Compatibility, *software));
    // Elsewhere the world scales with bilinear, as FSR 2's does.
    CHECK(render::choose_upscaler(scaling, *software) == "bilinear");
    scaling.scale = 1.0F;
    CHECK_FALSE(render::uses_fsr2(scaling, render::RenderMethod::ForwardPlus, *software));
    CHECK(render::choose_upscaler(scaling, *software).empty());

    render::Scene3D scene;
    scene.fsr2 = kFsr3;
    CHECK(detail::uses_fsr2(scene));
    CHECK(detail::uses_fsr3(scene));
    scene.fsr2->fsr3 = false;
    CHECK_FALSE(detail::uses_fsr3(scene));

    // Its sizes: half the render size, then each level of the shading change pyramid half that.
    CHECK(detail::fsr3_half_size(1478, 831) == std::array<std::uint32_t, 2>{739, 415});
    CHECK(detail::fsr3_half_size(1, 1) == std::array<std::uint32_t, 2>{1, 1});
    CHECK(detail::fsr3_level_size(1478, 831, 2) == std::array<std::uint32_t, 2>{184, 103});
    CHECK(detail::fsr3_level_size(4, 4, 2) == std::array<std::uint32_t, 2>{1, 1});

    // Its uniforms are FSR 2's, with the frame before's jitter in render pixels as FSR 2's jitter.
    const detail::Fsr2Frame frame{.render_width = 64,
                                  .render_height = 32,
                                  .display_width = 128,
                                  .display_height = 64,
                                  .jitter = {0.5F / 64.0F, -0.25F / 32.0F},
                                  .projection = render::perspective(60.0F, 2.0F, 0.1F, 100.0F)};
    const detail::Fsr3Uniforms uniforms =
        detail::fsr3_uniforms(frame, {-0.25F / 64.0F, 0.5F / 32.0F});
    const detail::Fsr2Uniforms fsr2 = detail::fsr2_uniforms(frame);
    CHECK(uniforms.fsr2.sizes == fsr2.sizes);
    CHECK(uniforms.fsr2.jitter_scale == fsr2.jitter_scale);
    CHECK(uniforms.fsr2.view_depth == fsr2.view_depth);
    CHECK(uniforms.previous[0] == doctest::Approx(-0.125F));
    CHECK(uniforms.previous[1] == doctest::Approx(-0.25F));
}

TEST_CASE("Forward+ FSR 3 rebuilds a still square's edges at the full size over its frames") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image taa = draw_square(*device, {.fsr2 = std::nullopt, .taa = true, .frames = 32});
        const Image plain = draw_square(*device, {.fsr2 = std::nullopt, .frames = 32});
        const Image first = draw_square(*device, {.fsr2 = kFsr3});
        const Image upscaled = draw_square(*device, {.fsr2 = kFsr3, .frames = 32});
        CHECK(red(upscaled, 16, 16) > 250);
        CHECK(red(upscaled, 1, 1) == red(plain, 1, 1));
        CHECK(blended(upscaled) > 20);
        CHECK(differing(upscaled, taa, 40) < differing(first, taa, 40));
        CHECK(differing(upscaled, taa, 60) < 300);
        // It isn't FSR 2.
        CHECK(difference(upscaled, draw_square(*device, {.frames = 32})) > 0);
        // A scale of 1 draws as without it.
        CHECK(difference(
                  draw_square(*device, {.fsr2 = render::Fsr2Scaling{.scale = 1.0F, .fsr3 = true},
                                        .frames = 3}),
                  draw_square(*device, {.fsr2 = std::nullopt, .frames = 3})) == 0);
    }
}

TEST_CASE("Forward+ FSR 3 follows moving meshes, a moving camera and the sky") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image still = draw_square(*device, {.fsr2 = kFsr3, .frames = 32});
        const Image moved = draw_square(*device, {.fsr2 = kFsr3, .frames = 32, .step = 0.07F});
        CHECK(red(moved, 16, 16) > 250);
        CHECK(blended(moved) > 20);
        CHECK(differing(moved, still, 60) < 200);
        const Image panned = draw_square(*device, {.fsr2 = kFsr3, .frames = 32, .turn = 0.05F});
        const Image taa_panned =
            draw_square(*device, {.fsr2 = std::nullopt, .taa = true, .frames = 32, .turn = 0.05F});
        CHECK(blended(panned) > 20);
        CHECK(differing(panned, taa_panned, 60) < 300);
        // A camera turning over a striped sky keeps its frames before, moved with the sky by the
        // motion vectors worked out from the depth: it ends nearer the full size's frame than a
        // first frame, which has none. FSR 3 softens other stripes' edges than FSR 2, so it
        // differs more from the camera that stood still than FSR 2 does.
        const Image plain = draw_square(
            *device, {.fsr2 = std::nullopt, .frames = 16, .perspective = true, .sky = true});
        const Image first = draw_square(*device, {.fsr2 = kFsr3, .perspective = true, .sky = true});
        const Image still_sky =
            draw_square(*device, {.fsr2 = kFsr3, .frames = 16, .perspective = true, .sky = true});
        const Image turned = draw_square(
            *device, {.fsr2 = kFsr3, .frames = 16, .perspective = true, .yaw = 0.03F, .sky = true});
        CHECK(differing(turned, plain, 40) < differing(first, plain, 40) * 3 / 4);
        CHECK(differing(turned, still_sky, 40) < 700);
    }
}

TEST_CASE("GPUs draw FSR 3 as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    const std::vector<Square> squares{
        {.fsr2 = kFsr3},
        {.fsr2 = kFsr3, .frames = 2},
        {.fsr2 = kFsr3, .frames = 16},
        {.fsr2 = render::Fsr2Scaling{.scale = 0.5F, .sharpness = 2.0F, .fsr3 = true}, .frames = 8},
        {.fsr2 = kFsr3, .frames = 12, .step = 0.07F},
        {.fsr2 = kFsr3, .frames = 12, .turn = 0.05F},
        {.fsr2 = render::Fsr2Scaling{.scale = 0.77F, .sharpness = 0.0F, .fsr3 = true},
         .frames = 8,
         .perspective = true},
        {.fsr2 = kFsr3, .frames = 10, .perspective = true, .yaw = 0.03F, .sky = true},
    };
    for (std::size_t i = 0; i < squares.size(); ++i) {
        CAPTURE(i);
        const Image expected = draw_square(*software, squares[i]);
        const Image drawn = draw_square(*gpu, squares[i]);
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 50);
        CHECK(difference(drawn, expected) <= 24);
    }
}
