#include "../engine/render/src/mesh_shading.hpp"
#include "../engine/render/src/taa.hpp"
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
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no TAA.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
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

int red(const Image& image, std::uint32_t x, std::uint32_t y) {
    return image.pixels.at((std::size_t{y} * image.width + x) * 4);
}

// How many pixels are neither the square's white nor the background's red: the square's edges
// where a pixel is partly covered.
std::size_t blended(const Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        count += image.pixels[i] > 40 && image.pixels[i] < 215 ? 1U : 0U;
    }
    return count;
}

// A square facing +Z, one unit across, its corners clockwise from the front, moved right by x.
std::vector<Vertex3D> square(float x = 0.0F) {
    const auto corner = [x](float cx, float cy) {
        return Vertex3D{.position = {x + (cx * 0.5F), cy * 0.5F, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// What draw_square draws.
struct Square {
    bool taa = true;
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    std::uint32_t frames = 1; // drawn in a row on one target
    float step = 0.0F;        // how far the square moves right each frame
    bool previous = true;     // whether its draws say where it was in the frame before
    bool update = false;      // moved by update_mesh instead of its transform
    float turn = 0.0F;        // how far the camera moves right each frame
    std::uint32_t empty = 0;  // a frame drawn without the square, counted from 1
};

// A white unshaded square turned 30 degrees, on a dark red background, seen in an orthogonal
// projection three units across into a 32x32 target, its edges stepping across the pixels.
// Moving, it ends at the same place as a still one: it starts as far to the left as it moves.
Image draw_square(RenderDevice& device, const Square& square_settings) {
    auto target = device.create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    const float start = -square_settings.step * static_cast<float>(square_settings.frames - 1);
    auto mesh = device.create_mesh(square(square_settings.update ? start : 0.0F), {});
    REQUIRE(mesh.ok());
    const float turn = 0.5235988F;
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    std::array<float, 16> last{};
    for (std::uint32_t frame = 0; frame < square_settings.frames; ++frame) {
        const float moved = start + (square_settings.step * static_cast<float>(frame));
        if (square_settings.update && frame > 0) {
            REQUIRE(device.update_mesh(mesh.value(), square(moved)).ok());
        }
        const float x = square_settings.update ? 0.0F : moved;
        const std::array<float, 16> transform{c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
        render::Scene3D scene;
        scene.method = square_settings.method;
        scene.use_taa = square_settings.taa;
        const float eye = square_settings.turn * static_cast<float>(frame);
        scene.camera = render::look_at({eye, 0.0F, 2.5F}, {eye, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
        scene.projection = render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F);
        REQUIRE(device
                    .begin_frame({.target = target.value(),
                                  .clear = {0.15F, 0.02F, 0.02F, 1.0F},
                                  .scene = scene})
                    .ok());
        if (frame + 1 != square_settings.empty) {
            const Material3D white{.unshaded = true, .cull = render::CullMode::Disabled};
            std::optional<std::array<float, 16>> previous;
            if (square_settings.previous && frame > 0) {
                previous = last;
            }
            REQUIRE(device
                        .draw_mesh(mesh.value(), transform, white, 1, render::ShadowCasting::On,
                                   previous)
                        .ok());
        }
        REQUIRE(device.end_frame().ok());
        last = transform;
    }
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

slash::json::Value parse(const char* text) {
    auto parsed = slash::json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

} // namespace

TEST_CASE("TAA's jitter is Godot's Halton sequence over 16 frames, within half a pixel") {
    CHECK(detail::halton(0, 2) == -1.0F);
    CHECK(detail::halton(1, 2) == 0.0F);
    CHECK(detail::halton(2, 2) == -0.5F);
    CHECK(detail::halton(3, 2) == 0.5F);
    CHECK(detail::halton(1, 3) == doctest::Approx(-1.0F / 3.0F));
    CHECK(detail::halton(2, 3) == doctest::Approx(1.0F / 3.0F));
    // A clip-space unit is half the target, so a jitter of 1 / width moves half a pixel.
    const std::array<float, 2> first = detail::taa_jitter(3, 64, 32);
    CHECK(first[0] == 0.5F / 64.0F);
    CHECK(first[1] == doctest::Approx((detail::halton(3, 3)) / 32.0F));
    CHECK(detail::taa_jitter(3 + detail::kTaaPhases, 64, 32) == first);
    for (std::uint32_t frame = 0; frame < detail::kTaaPhases; ++frame) {
        const std::array<float, 2> jitter = detail::taa_jitter(frame, 64, 32);
        CHECK(std::abs(jitter[0]) <= 1.0F / 64.0F);
        CHECK(std::abs(jitter[1]) <= 1.0F / 32.0F);
    }
    // The jitter moves every point by the same amount in clip space after the divide, in a
    // perspective projection as in an orthogonal one.
    for (const std::array<float, 16>& projection : {render::perspective(60.0F, 1.0F, 0.1F, 20.0F),
                                                    render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F)}) {
        const std::array<float, 16> moved = detail::jittered(projection, {0.25F, -0.125F});
        for (const std::array<float, 3>& point :
             {std::array{0.0F, 0.0F, -1.0F}, std::array{0.3F, -0.2F, -5.0F}}) {
            const auto clip = [&](const std::array<float, 16>& m) {
                std::array<float, 4> out{};
                for (std::size_t row = 0; row < 4; ++row) {
                    out.at(row) = (m.at(row) * point[0]) + (m.at(4 + row) * point[1]) +
                                  (m.at(8 + row) * point[2]) + m.at(12 + row);
                }
                return out;
            };
            const std::array<float, 4> plain = clip(projection);
            const std::array<float, 4> shifted = clip(moved);
            CHECK(shifted[0] / shifted[3] == doctest::Approx((plain[0] / plain[3]) + 0.25F));
            CHECK(shifted[1] / shifted[3] == doctest::Approx((plain[1] / plain[3]) - 0.125F));
            CHECK(shifted[2] == plain[2]);
        }
    }
}

TEST_CASE("TAA's motion vectors point from each pixel to where it was, without the jitter") {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(4.0F, 1.0F, 0.1F, 20.0F);
    const std::array<float, 16> view_projection = render::multiply(scene.projection, scene.camera);
    const detail::MeshCameraUniforms camera =
        detail::taa_camera_uniforms(scene, {0.01F, -0.02F}, view_projection);
    CHECK(camera.previous_view_projection == view_projection);
    CHECK(camera.jitter == detail::Float4{0.01F, -0.02F, 0.0F, 0.0F});
    // A mesh one unit right of where it was, and one unit lower: two units are half the frame,
    // a quarter of it in texture coordinates, which run down.
    const std::array<float, 16> now{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, -1, 0, 1};
    const std::array<float, 16> before{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const detail::MeshInstanceUniforms instance =
        detail::mesh_instance_uniforms(now, Material3D{}, before);
    const Vertex3D vertex{.position = {0.2F, 0.3F, 0.0F}};
    const detail::MeshCorner corner = detail::mesh_vertex(camera, instance, vertex);
    const std::array<float, 2> motion =
        detail::motion_vector(corner.motion_clip, corner.previous_clip);
    CHECK(motion[0] == doctest::Approx(-0.25F));
    CHECK(motion[1] == doctest::Approx(-0.25F));
    // The drawn position keeps the jitter; the motion vector's doesn't.
    CHECK(corner.clip[0] / corner.clip[3] ==
          doctest::Approx((corner.motion_clip[0] / corner.motion_clip[3]) + 0.01F));
    // A vertex update_mesh moved is moved by where it was in the mesh too.
    const detail::MeshCorner updated = detail::mesh_vertex(
        camera, detail::mesh_instance_uniforms(before, Material3D{}), vertex, {0.2F, 1.3F, 0.0F});
    const std::array<float, 2> down =
        detail::motion_vector(updated.motion_clip, updated.previous_clip);
    CHECK(down[0] == doctest::Approx(0.0F));
    CHECK(down[1] == doctest::Approx(-0.25F));
}

TEST_CASE("TAA's resolve blends the frame with its history, as Godot's taa_resolve") {
    constexpr std::uint32_t kSize = 8;
    const detail::TaaUniforms uniforms = detail::taa_uniforms(kSize, kSize);
    CHECK(uniforms.size[0] == kSize);
    CHECK(uniforms.params[0] == 2.5F);
    // Godot's variance clipping box: 1.1 at 1080 pixels high, kept from 0.75 to 1.
    CHECK(uniforms.params[1] == 1.0F);
    CHECK(detail::taa_uniforms(3840, 2160).params[1] == 0.75F);
    // A checkerboard of black and white, and a grey history.
    std::vector<float> color(std::size_t{kSize} * kSize * 4, 1.0F);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float value = (x + y) % 2 == 0 ? 1.0F : 0.0F;
            for (std::size_t c = 0; c < 3; ++c) {
                color.at((((std::size_t{y} * kSize) + x) * 4) + c) = value;
            }
        }
    }
    const std::vector<float> depth(std::size_t{kSize} * kSize, 0.5F);
    const std::vector<float> still(std::size_t{kSize} * kSize * 2, 0.0F);
    std::vector<float> grey(color.size(), 0.5F);
    std::vector<float> out(color.size(), 0.0F);
    detail::run_taa(uniforms,
                    {.color = color,
                     .depth = depth,
                     .velocity = still,
                     .last_velocity = still,
                     .history = grey},
                    out);
    // Each pixel takes a 16th of the frame at most, less where it flickers against the history:
    // white pixels stay close to the grey, a little lighter, and black ones a little darker.
    CHECK(out[0] > 0.5F);
    CHECK(out[0] < 0.55F);
    CHECK(out[4] < 0.5F);
    CHECK(out[4] > 0.45F);
    CHECK(out[3] == 1.0F);
    // History that moved off the frame is dropped for the frame, less the share Godot keeps where
    // the frame's luminance jumps from the history's.
    std::vector<float> away(still.size(), 0.0F);
    for (std::size_t i = 0; i < away.size(); i += 2) {
        away[i] = 2.0F;
    }
    detail::run_taa(
        uniforms,
        {.color = color, .depth = depth, .velocity = away, .last_velocity = away, .history = grey},
        out);
    CHECK(out[0] > 0.8F);
    CHECK(out[4] < 0.3F);
    // History the frame's neighborhood can't hold is clipped to it: a white history over a
    // black frame gives black.
    const std::vector<float> black(color.size(), 0.0F);
    const std::vector<float> white(color.size(), 1.0F);
    detail::run_taa(uniforms,
                    {.color = black,
                     .depth = depth,
                     .velocity = still,
                     .last_velocity = still,
                     .history = white},
                    out);
    CHECK(out[0] < 0.001F);
}

TEST_CASE("TAA settings come from project.json's rendering.anti_aliasing.quality.use_taa") {
    CHECK(render::use_taa(parse(R"({"anti_aliasing": {"quality": {"use_taa": true}}})")));
    CHECK_FALSE(render::use_taa(parse(R"({"anti_aliasing": {"quality": {"use_taa": false}}})")));
    CHECK_FALSE(render::use_taa(parse(R"({"anti_aliasing": {"quality": {}}})")));
    CHECK_FALSE(render::use_taa(parse(R"({"anti_aliasing": {"quality": {"use_taa": 1}}})")));
    CHECK_FALSE(render::use_taa(parse("{}")));
}

TEST_CASE("Forward+ TAA smooths a still square's edges over its frames") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // Without TAA, each pixel is the square's or the background's.
        const Image plain = draw_square(*device, {.taa = false, .frames = 16});
        CHECK(blended(plain) == 0);
        // A target's first frame with it is drawn as it is, its camera jittered.
        CHECK(blended(draw_square(*device, {})) == 0);
        // Over 16 frames the pixels the edges cross take the share of them the square covers.
        const Image smooth = draw_square(*device, {.frames = 16});
        CHECK(blended(smooth) > 20);
        CHECK(red(smooth, 16, 16) > 250);
        CHECK(red(smooth, 1, 1) == red(plain, 1, 1));
        CHECK(differing(smooth, plain, 40) < 200);
        // Godot's Compatibility renderer has no TAA.
        const auto compatibility = render::RenderMethod::Compatibility;
        CHECK(difference(
                  draw_square(*device, {.method = compatibility, .frames = 4}),
                  draw_square(*device, {.taa = false, .method = compatibility, .frames = 4})) == 0);
        // A frame without meshes starts the blending again.
        CHECK(blended(draw_square(*device, {.frames = 6, .empty = 5})) == 0);
    }
}

TEST_CASE("Forward+ TAA follows moving meshes and a moving camera by their motion vectors") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image still = draw_square(*device, {.frames = 24});
        // Moved by its transform, by update_mesh, or seen by a moving camera, the square ends
        // where the still one is and looks as it does, its edges as smooth. A moving edge's
        // history is resampled each frame, so it is a little softer than a still one's: of the
        // 4096 channels, about 75 differ by more than 60 where all four edges cross pixels, and
        // 25 where update_mesh moves the square along two of them.
        const Image moved = draw_square(*device, {.frames = 24, .step = 0.07F});
        const Image updated = draw_square(*device, {.frames = 24, .step = 0.07F, .update = true});
        for (const Image* drawn : {&moved, &updated}) {
            CHECK(blended(*drawn) > 20);
            CHECK(red(*drawn, 16, 16) > 250);
        }
        CHECK(differing(moved, still, 60) < 100);
        CHECK(differing(updated, still, 60) < 50);
        const Image panned = draw_square(*device, {.frames = 24, .turn = 0.05F});
        CHECK(blended(panned) > 20);
        // Without where it was, the history isn't moved with it, and the square's trailing edges
        // are left behind in it, as far as the neighborhood clipping lets them.
        const Image lost = draw_square(*device, {.frames = 24, .step = 0.07F, .previous = false});
        CHECK(differing(lost, still, 30) > differing(moved, still, 30) + 25);
    }
}

TEST_CASE("GPUs draw TAA as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    const std::vector<Square> squares{
        {.frames = 2},
        {.frames = 16},
        {.frames = 12, .step = 0.07F},
        {.frames = 12, .step = 0.07F, .update = true},
        {.frames = 12, .turn = 0.05F},
        {.frames = 6, .empty = 3},
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
