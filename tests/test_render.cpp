#include "devices.hpp"

#include "slash/platform/platform.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/material.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>
#include <doctest/doctest.h>

#if defined(_WIN32)
// The renderer's own headers, for its HLSL and Windows' compiler.
#include "../engine/render/src/hlsl_compiler.hpp"
#include "../engine/render/src/material_shaders.hpp"
#include "../engine/render/src/mesh_shaders.hpp"
#include "../engine/render/src/particles_draw_shaders.hpp"
#include "../engine/render/src/particles_emit_shaders.hpp"
#include "../engine/render/src/particles_process_shaders.hpp"
#include "../engine/render/src/shadow_shaders.hpp"
#include "../engine/render/src/sprite_shaders.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using slash::render::BlendMode;
using slash::render::Color;
using slash::render::create_device;
using slash::render::create_null_device;
using slash::render::distance_field_coverage;
using slash::render::DistanceField;
using slash::render::Filter;
using slash::render::Frame;
using slash::render::Image;
using slash::render::Instruction;
using slash::render::instruction;
using slash::render::Mat4;
using slash::render::Material;
using slash::render::MaterialProgram;
using slash::render::Op;
using slash::render::operand;
using slash::render::pixel_projection;
using slash::render::RenderDevice;
using slash::render::TextureDesc;
using slash::render::TextureId;
using slash::render::Vertex2D;
using slash::render::VsyncMode;
using slash::test::drawing_devices;
using slash::test::gl_device;
using slash::test::gpu_device;

namespace {

using Rgba = std::array<std::uint8_t, 4>;

// Two triangles covering a rectangle, with the whole texture stretched over it.
std::vector<Vertex2D> rect(float x0, float y0, float x1, float y1,
                           Rgba rgba = {255, 255, 255, 255}) {
    const Vertex2D top_left{.x = x0, .y = y0, .u = 0.0F, .v = 0.0F, .rgba = rgba};
    const Vertex2D top_right{.x = x1, .y = y0, .u = 1.0F, .v = 0.0F, .rgba = rgba};
    const Vertex2D bottom_left{.x = x0, .y = y1, .u = 0.0F, .v = 1.0F, .rgba = rgba};
    const Vertex2D bottom_right{.x = x1, .y = y1, .u = 1.0F, .v = 1.0F, .rgba = rgba};
    return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

TextureDesc target_desc(std::uint32_t size) {
    return {.width = size, .height = size, .filter = Filter::Linear, .render_target = true};
}

// "" when the pixel at (x, y) is within 1 of expected in every channel, else what differs.
std::string pixel_is(const Image& image, std::uint32_t x, std::uint32_t y, Rgba expected) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    std::string actual;
    bool close = true;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const int value = image.pixels.at(at + channel);
        close = close && std::abs(value - int{expected.at(channel)}) <= 1;
        actual += (channel == 0 ? "" : ",") + std::to_string(value);
    }
    if (close) {
        return "";
    }
    return "pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + actual +
           ", expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," +
           std::to_string(expected[2]) + "," + std::to_string(expected[3]);
}

// The error a call failed with, or "(succeeded)".
template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

Image render(RenderDevice& device, TextureId target, const Frame& frame,
             const std::vector<std::pair<TextureId, std::vector<Vertex2D>>>& draws) {
    Frame with_target = frame;
    with_target.target = target;
    auto began = device.begin_frame(with_target);
    REQUIRE(began.ok());
    REQUIRE(began.value());
    for (const auto& [texture, vertices] : draws) {
        REQUIRE(device.draw(texture, vertices).ok());
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target);
    REQUIRE(image.ok());
    return std::move(image).value();
}

// Both devices refuse the same mistakes with the same errors.
void check_refusals(RenderDevice& device) {
    CHECK(error_of(device.create_texture(
              {.width = 0, .height = 4, .filter = Filter::Linear, .render_target = false})) ==
          "a texture is 1 to 16384 pixels wide and high, not 0x4");
    const std::vector<std::uint8_t> short_pixels(15);
    CHECK(error_of(device.create_texture(target_desc(2), short_pixels)) ==
          "a 2x2 texture takes 16 bytes of RGBA, not 15");
    // Compressed textures are whole blocks, are never drawn into and start with their blocks.
    TextureDesc bc7{.width = 6,
                    .height = 4,
                    .filter = Filter::Linear,
                    .render_target = false,
                    .format = slash::render::TextureFormat::Bc7};
    CHECK(error_of(device.create_texture(bc7, std::vector<std::uint8_t>(24))) ==
          "a bc7 texture is whole 4x4 blocks, so its sides are multiples of 4, not 6x4");
    bc7.width = 8;
    CHECK(error_of(device.create_texture(bc7)) ==
          "a 8x4 texture takes 32 bytes of bc7 blocks, not 0");
    TextureDesc astc = target_desc(4);
    astc.format = slash::render::TextureFormat::Astc4x4;
    CHECK(error_of(device.create_texture(astc, std::vector<std::uint8_t>(16))) ==
          "only RGBA8 textures are render targets, not astc_4x4 ones");

    auto plain = device.create_texture(
        {.width = 2, .height = 2, .filter = Filter::Nearest, .render_target = false});
    REQUIRE(plain.ok());
    auto target = device.create_texture(target_desc(4));
    REQUIRE(target.ok());

    CHECK(error_of(device.draw(TextureId::None, rect(0, 0, 1, 1))) ==
          "draw needs a frame; call begin_frame first");
    CHECK(error_of(device.end_frame()) == "end_frame needs a frame; call begin_frame first");
    CHECK(error_of(device.begin_frame({.target = plain.value(), .clear = {}, .view = {}})) ==
          "texture 1 is not a render target; create it with render_target set");
    CHECK(error_of(device.begin_frame(
              {.target = static_cast<TextureId>(99), .clear = {}, .view = {}})) == "no texture 99");
    CHECK(error_of(device.read_texture(plain.value())) ==
          "texture 1 is not a render target; create it with render_target set");

    auto began = device.begin_frame({.target = target.value(), .clear = {}, .view = {}});
    REQUIRE(began.ok());
    CHECK(error_of(device.begin_frame({.target = target.value(), .clear = {}, .view = {}})) ==
          "begin_frame can't run during a frame; call end_frame first");
    std::vector<Vertex2D> four = rect(0, 0, 1, 1);
    four.resize(4);
    CHECK(error_of(device.draw(TextureId::None, four)) ==
          "every three vertices make a triangle, so 4 vertices is not a whole number of triangles");
    CHECK(error_of(device.draw(target.value(), rect(0, 0, 1, 1))) ==
          "texture 2 is this frame's target, so it can't be drawn with until the frame ends");
    CHECK(error_of(device.draw(static_cast<TextureId>(99), rect(0, 0, 1, 1))) == "no texture 99");
    CHECK(error_of(device.read_texture(target.value())) ==
          "read_texture can't run during a frame; call end_frame first");
    Material material;
    material.program.code = {instruction(Op::Mov, 0, operand(5))};
    CHECK(error_of(device.draw(TextureId::None, rect(0, 0, 1, 1), material)) ==
          "instruction 0 reads register 5 before anything writes it");
    material.program.code = {instruction(Op::Mov, 0, operand(1))};
    material.textures[1].texture = static_cast<TextureId>(99);
    CHECK(error_of(device.draw(TextureId::None, rect(0, 0, 1, 1), material)) == "no texture 99");
    material.textures[1].texture = target.value();
    CHECK(error_of(device.draw(TextureId::None, rect(0, 0, 1, 1), material)) ==
          "texture 2 is this frame's target, so it can't be drawn with until the frame ends");
    CHECK(device.end_frame().ok());

    CHECK(error_of(device.update_texture(plain.value(), short_pixels)) ==
          "a 2x2 texture takes 16 bytes of RGBA, not 15");
    device.destroy_texture(plain.value());
    CHECK(error_of(device.update_texture(plain.value(), {})) == "no texture 1");
    CHECK(device.stats().textures == 1);
    CHECK(device.stats().texture_bytes == slash::render::texture_bytes(target_desc(4)));
}

// A program that runs one op on the pixel's texture coordinate (u, v, u, v), a constant and the
// vertex color, and draws the result with alpha 1, so the target shows it unblended.
MaterialProgram one_op(Op op, std::uint32_t aux = 0) {
    MaterialProgram program;
    program.constants = {{0.3F, 0.6F, 0.5F, 0.2F}, {1.0F, 1.0F, 1.0F, 1.0F}};
    const std::uint32_t uv = operand(slash::render::kUvRegister, {0, 1, 0, 1});
    program.code = {
        instruction(op, 4, uv, operand(32), operand(slash::render::kColorRegister), aux),
        instruction(Op::Merge, 0, operand(4), operand(33), 0, 0b1000),
    };
    return program;
}

std::array<float, 4> saturated(std::array<float, 4> color) {
    for (float& channel : color) {
        channel = channel > 0.0F ? std::min(channel, 1.0F) : 0.0F;
    }
    return color;
}

} // namespace

TEST_CASE("material programs are checked before anything runs them") {
    using slash::render::check_program;
    MaterialProgram program;
    CHECK(check_program(program).ok());
    program.code.assign(65, instruction(Op::Mov, 0, operand(0)));
    CHECK(error_of(check_program(program)) ==
          "a material program has at most 64 instructions, not 65");
    program.code = {instruction(static_cast<Op>(99), 4, operand(0))};
    CHECK(error_of(check_program(program)) == "instruction 0 has no op 99");
    program.code = {instruction(Op::Mov, 4, operand(0)),
                    instruction(Op::Add, 5, operand(4), operand(6))};
    CHECK(error_of(check_program(program)) ==
          "instruction 1 reads register 6 before anything writes it");
    program.code = {instruction(Op::Mov, 4, operand(33))};
    program.constants = {{1.0F, 1.0F, 1.0F, 1.0F}};
    CHECK(error_of(check_program(program)) ==
          "instruction 0 reads constant 1, but the program has 1");
    program.code = {instruction(Op::Mov, 40, operand(32))};
    CHECK(error_of(check_program(program)) ==
          "instruction 0 writes register 40; registers are 0 to 31");
    program.code = {instruction(Op::Dot, 4, operand(0), operand(0), 0, 5)};
    CHECK(error_of(check_program(program)) == "instruction 0 uses 5 components; use 1 to 4");
    program.code = {instruction(Op::Sample, 4, operand(0), 0, 0, 4)};
    CHECK(error_of(check_program(program)) ==
          "instruction 0 samples texture slot 4; slots are 0 to 3");
    // Operands an op doesn't read aren't checked.
    program.code = {instruction(Op::Abs, 4, operand(0), operand(9), operand(63))};
    CHECK(check_program(program).ok());
    program.constants.assign(33, {});
    CHECK(error_of(check_program(program)) ==
          "a material program has at most 32 constants, not 33");
}

TEST_CASE("run_program runs a material program on the CPU") {
    using slash::render::run_program;
    const auto never = [](std::uint32_t, float, float) {
        FAIL("nothing samples");
        return std::array<float, 4>{};
    };
    const slash::render::PixelInput input{.uv = {0.25F, 0.75F},
                                          .color = {0.9F, 0.4F, 0.7F, 1.0F},
                                          .frag_coord = {3.5F, 1.5F},
                                          .texel = {0.5F, 0.25F}};
    // With no code, register 0 still holds the texture coordinate.
    CHECK(run_program({}, input, never) == std::array<float, 4>{0.25F, 0.75F, 0.0F, 0.0F});

    MaterialProgram program;
    program.constants = {{2.0F, 2.0F, 2.0F, 2.0F}, {-1.5F, 0.5F, 2.5F, -0.5F}};
    // Swizzles pick components: (v, u, u, u) times 2.
    program.code = {instruction(Op::Mul, 0, operand(0, {1, 0, 0, 0}), operand(32))};
    CHECK(run_program(program, input, never) == std::array<float, 4>{1.5F, 0.5F, 0.5F, 0.5F});
    // GLSL's mod, round halves up, and frag_coord and texel registers.
    program.code = {instruction(Op::Mod, 0, operand(33), operand(32))};
    CHECK(run_program(program, input, never) == std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.5F});
    program.code = {instruction(Op::Round, 0, operand(33))};
    CHECK(run_program(program, input, never) == std::array<float, 4>{-1.0F, 1.0F, 3.0F, 0.0F});
    program.code = {instruction(Op::Add, 0, operand(2), operand(3))};
    CHECK(run_program(program, input, never) == std::array<float, 4>{4.0F, 1.75F, 0.0F, 0.0F});
    // Dot over two components, in every component; merge takes b where the mask says.
    program.code = {instruction(Op::Dot, 0, operand(1), operand(33), 0, 2)};
    CHECK(run_program(program, input, never)[3] == doctest::Approx(-1.15F));
    program.code = {instruction(Op::Merge, 0, operand(1), operand(33), 0, 0b0110)};
    CHECK(run_program(program, input, never) == std::array<float, 4>{0.9F, 0.5F, 2.5F, 1.0F});
    // HSV round trips.
    program.code = {instruction(Op::RgbToHsv, 4, operand(1)),
                    instruction(Op::HsvToRgb, 0, operand(4))};
    const auto rgb = run_program(program, input, never);
    CHECK(rgb[0] == doctest::Approx(0.9F));
    CHECK(rgb[1] == doctest::Approx(0.4F));
    CHECK(rgb[2] == doctest::Approx(0.7F));
    // Sampling hands the slot and coordinate to the sampler.
    program.code = {instruction(Op::Sample, 0, operand(0, {1, 0, 0, 0}), 0, 0, 2)};
    const auto sampled = run_program(program, input, [](std::uint32_t slot, float u, float v) {
        return std::array<float, 4>{static_cast<float>(slot), u, v, 1.0F};
    });
    CHECK(sampled == std::array<float, 4>{2.0F, 0.75F, 0.25F, 1.0F});
}

TEST_CASE("sample_image filters and wraps as a GPU samples") {
    using slash::render::sample_image;
    // A 2x1 image: black, white.
    const Image image{.width = 2, .height = 1, .pixels = {0, 0, 0, 255, 255, 255, 255, 255}};
    CHECK(sample_image(image, Filter::Nearest, false, 0.4F, 0.5F)[0] == 0.0F);
    CHECK(sample_image(image, Filter::Nearest, false, 0.6F, 0.5F)[0] == 1.0F);
    CHECK(sample_image(image, Filter::Linear, false, 0.5F, 0.5F)[0] == doctest::Approx(0.5F));
    // At the texel centers, linear filtering gives the texels; past the edge they stretch on.
    CHECK(sample_image(image, Filter::Linear, false, 0.25F, 0.5F)[0] == 0.0F);
    CHECK(sample_image(image, Filter::Linear, false, 1.5F, 0.5F)[0] == 1.0F);
    // Repeating tiles: past the right edge comes the left texel again.
    CHECK(sample_image(image, Filter::Nearest, true, 1.25F, 0.5F)[0] == 0.0F);
    CHECK(sample_image(image, Filter::Linear, true, 1.0F, 0.5F)[0] == doctest::Approx(0.5F));
    CHECK(sample_image({}, Filter::Linear, false, 0.5F, 0.5F) ==
          std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});
}

TEST_CASE("draws with different materials are separate GPU draws") {
    auto device = create_null_device();
    auto target = device->create_texture(target_desc(4));
    REQUIRE(target.ok());
    Material glow;
    glow.blend = BlendMode::Add;
    Material tinted;
    tinted.program.code = {instruction(Op::Mul, 0, operand(1), operand(32))};
    tinted.program.constants = {{1.0F, 0.5F, 0.5F, 1.0F}};
    Material other_tint = tinted;
    other_tint.program.constants[0][1] = 0.25F;
    REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
    REQUIRE(device->draw(TextureId::None, rect(0, 0, 1, 1), glow).ok());
    REQUIRE(device->draw(TextureId::None, rect(1, 0, 2, 1), glow).ok());
    REQUIRE(device->draw(TextureId::None, rect(2, 0, 3, 1), tinted).ok());
    REQUIRE(device->draw(TextureId::None, rect(3, 0, 4, 1), tinted).ok());
    REQUIRE(device->draw(TextureId::None, rect(0, 1, 1, 2), other_tint).ok());
    REQUIRE(device->draw(TextureId::None, rect(1, 1, 2, 2)).ok());
    REQUIRE(device->end_frame().ok());
    CHECK(device->stats().draws == 4);

    for (const auto& gpu : drawing_devices()) {
        CAPTURE(slash::render::backend_name(gpu->backend()));
        auto gpu_target = gpu->create_texture(target_desc(4));
        REQUIRE(gpu_target.ok());
        REQUIRE(gpu->begin_frame({.target = gpu_target.value(), .clear = {}, .view = {}}).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(0, 0, 1, 1), glow).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(1, 0, 2, 1), glow).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(2, 0, 3, 1), tinted).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(3, 0, 4, 1), tinted).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(0, 1, 1, 2), other_tint).ok());
        REQUIRE(gpu->draw(TextureId::None, rect(1, 1, 2, 2)).ok());
        REQUIRE(gpu->end_frame().ok());
        CHECK(gpu->stats().draws == 4);
        auto drawn = gpu->read_texture(gpu_target.value());
        REQUIRE(drawn.ok());
        CHECK(pixel_is(drawn.value(), 3, 0, {255, 128, 128, 255}).empty());
        CHECK(pixel_is(drawn.value(), 0, 1, {255, 64, 128, 255}).empty());
    }
}

TEST_CASE("the software and GPU devices run every material op as run_program does") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        constexpr std::uint32_t kSize = 8;
        auto target = device->create_texture(target_desc(kSize));
        REQUIRE(target.ok());
        // Slot 0: red, green / blue, white. Slot 1: a 4x1 gradient that repeats.
        const Image quarters{
            .width = 2,
            .height = 2,
            .pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
        const Image gradient{
            .width = 4,
            .height = 1,
            .pixels = {0, 0, 0, 255, 85, 40, 20, 255, 170, 80, 40, 255, 255, 120, 60, 255}};
        auto texture0 = device->create_texture(
            {.width = 2, .height = 2, .filter = Filter::Nearest, .render_target = false},
            quarters.pixels);
        auto texture1 = device->create_texture(
            {.width = 4, .height = 1, .filter = Filter::Linear, .render_target = false},
            gradient.pixels);
        REQUIRE(texture0.ok());
        REQUIRE(texture1.ok());
        const auto sampler = [&](std::uint32_t slot, float u, float v) {
            return slot == 1 ? slash::render::sample_image(gradient, Filter::Linear, true, u, v)
                             : slash::render::sample_image(quarters, Filter::Nearest, false, u, v);
        };

        std::vector<std::pair<std::string, MaterialProgram>> programs;
        for (std::uint32_t number = 0; number < slash::render::kOpCount; ++number) {
            const auto op = static_cast<Op>(number);
            std::uint32_t aux = 0;
            if (op == Op::Dot || op == Op::Length || op == Op::Distance || op == Op::Normalize) {
                aux = 3;
            } else if (op == Op::Merge) {
                aux = 0b0101;
            }
            programs.emplace_back("op " + std::to_string(number), one_op(op, aux));
        }
        // Sampling both slots, the second at twice the coordinate so it repeats.
        MaterialProgram sample0 = one_op(Op::Sample, 0);
        MaterialProgram sample1 = one_op(Op::Sample, 1);
        sample1.code.insert(sample1.code.begin(), instruction(Op::Add, 5, operand(0), operand(0)));
        sample1.code[1].a = operand(5);
        programs.emplace_back("sample slot 0", sample0);
        programs.emplace_back("sample slot 1, repeating", sample1);
        // frag_coord over the target's size, and texel times 4.
        MaterialProgram frag;
        frag.constants = {{0.125F, 0.125F, 0.125F, 0.125F}, {1.0F, 1.0F, 1.0F, 1.0F}};
        frag.code = {instruction(Op::Mul, 4, operand(2), operand(32)),
                     instruction(Op::Mul, 5, operand(3), operand(32)),
                     instruction(Op::Merge, 4, operand(4), operand(5, {0, 0, 0, 0}), 0, 0b0100),
                     instruction(Op::Merge, 0, operand(4), operand(33), 0, 0b1000)};
        programs.emplace_back("frag_coord and texel", frag);

        for (const auto& [name, program] : programs) {
            CAPTURE(name);
            REQUIRE(slash::render::check_program(program).ok());
            Material material;
            material.program = program;
            material.textures[0] = {.texture = texture1.value(), .repeat = true};
            REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
            REQUIRE(device
                        ->draw(texture0.value(), rect(0, 0, kSize, kSize, {230, 102, 179, 255}),
                               material)
                        .ok());
            REQUIRE(device->end_frame().ok());
            auto drawn = device->read_texture(target.value());
            REQUIRE(drawn.ok());
            for (std::uint32_t y = 0; y < kSize; y += 3) {
                for (std::uint32_t x = 0; x < kSize; x += 3) {
                    const slash::render::PixelInput input{
                        .uv = {(static_cast<float>(x) + 0.5F) / kSize,
                               (static_cast<float>(y) + 0.5F) / kSize},
                        .color = {230.0F / 255.0F, 102.0F / 255.0F, 179.0F / 255.0F, 1.0F},
                        .frag_coord = {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F},
                        .texel = {0.5F, 0.5F}};
                    const auto expected =
                        saturated(slash::render::run_program(program, input, sampler));
                    const auto byte = [](float channel) {
                        return static_cast<std::uint8_t>(std::lround(channel * 255.0F));
                    };
                    CHECK(pixel_is(drawn.value(), x, y,
                                   {byte(expected[0]), byte(expected[1]), byte(expected[2]), 255})
                              .empty());
                }
            }
        }
    }
}

TEST_CASE("blend modes combine colors as Godot's do") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        auto target = device->create_texture(target_desc(2));
        REQUIRE(target.ok());
        const std::vector<std::pair<BlendMode, Rgba>> expected{
            {BlendMode::Mix, {90, 115, 204, 255}},
            {BlendMode::Add, {115, 166, 255, 255}},
            {BlendMode::Sub, {0, 38, 25, 191}},
            {BlendMode::Mul, {26, 51, 153, 128}},
            {BlendMode::PremulAlpha, {153, 179, 255, 255}},
        };
        for (const auto& [blend, rgba] : expected) {
            CAPTURE(slash::render::blend_mode_name(blend));
            Material material;
            material.blend = blend;
            REQUIRE(
                device
                    ->begin_frame(
                        {.target = target.value(), .clear = {0.2F, 0.4F, 0.6F, 1.0F}, .view = {}})
                    .ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 0, 2, 2, {128, 128, 255, 128}), material)
                        .ok());
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            CHECK(pixel_is(image.value(), 1, 1, rgba).empty());
        }
    }
}

TEST_CASE("blend modes have Godot's names") {
    using slash::render::parse_blend_mode;
    CHECK(slash::render::blend_mode_name(BlendMode::PremulAlpha) == "premul_alpha");
    CHECK(parse_blend_mode("add") == BlendMode::Add);
    CHECK_FALSE(parse_blend_mode("multiply").has_value());
}

TEST_CASE("create_device knows its drivers") {
    auto device = create_device({.window = nullptr, .driver = "vulkn", .debug = false});
    REQUIRE_FALSE(device.ok());
    CHECK(device.error().message ==
          "no render driver \"vulkn\"; use auto, null, vulkan, metal, d3d12, opengl, opengl_es or "
          "webgpu; did you mean \"vulkan\"?");
    CHECK(error_of(create_device({.window = nullptr, .driver = "gles", .debug = false}))
              .starts_with("no render driver \"gles\""));

    auto null = create_device({.window = nullptr, .driver = "null", .debug = false});
    REQUIRE(null.ok());
    CHECK(null.value()->backend() == slash::render::Backend::Null);
#ifndef __EMSCRIPTEN__
    CHECK(error_of(create_device({.window = nullptr, .driver = "webgpu", .debug = false})) ==
          "the webgpu driver is the browser's, so only web builds have it");
#endif
}

TEST_CASE("the null device keeps textures, counts batched draws and reads back its clear color") {
    auto device = create_null_device();
    auto texture = device->create_texture(
        {.width = 1, .height = 1, .filter = Filter::Nearest, .render_target = false},
        std::vector<std::uint8_t>{1, 2, 3, 4});
    REQUIRE(texture.ok());
    auto target = device->create_texture(target_desc(2));
    REQUIRE(target.ok());
    CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
          "the null device has no window; draw to a render-target texture");

    const Image image = render(*device, target.value(),
                               {.target = {}, .clear = {1.0F, 0.5F, 0.0F, 1.0F}, .view = {}},
                               {{TextureId::None, rect(0, 0, 1, 1)},
                                {TextureId::None, rect(1, 1, 2, 2)},
                                {texture.value(), rect(0, 0, 2, 2)},
                                {TextureId::None, {}}});
    CHECK(image.width == 2);
    CHECK(pixel_is(image, 1, 1, {255, 128, 0, 255}).empty());
    const auto stats = device->stats();
    CHECK(stats.frames == 1);
    CHECK(stats.draws == 2); // the two untextured draws share one
    CHECK(stats.triangles == 6);
    CHECK(stats.textures == 2);
    CHECK(stats.texture_bytes == 4 + 16); // 1x1 and 2x2, RGBA
}

TEST_CASE("the null device refuses mistakes") {
    auto device = create_null_device();
    check_refusals(*device);
}

TEST_CASE("pixel_projection maps a target's pixels to clip space") {
    const Mat4 m = pixel_projection(200.0F, 100.0F);
    const auto clip = [&](float x, float y) {
        return std::array<float, 2>{(m[0] * x) + (m[4] * y) + m[12],
                                    (m[1] * x) + (m[5] * y) + m[13]};
    };
    CHECK(clip(0, 0) == std::array<float, 2>{-1.0F, 1.0F});
    CHECK(clip(200, 100) == std::array<float, 2>{1.0F, -1.0F});
    CHECK(clip(100, 50) == std::array<float, 2>{0.0F, 0.0F});
}

TEST_CASE("multiply applies one matrix, then the other") {
    // Doubled, then moved 3 pixels right on a 200x100 target.
    Mat4 doubled{};
    doubled[0] = 2.0F;
    doubled[5] = 2.0F;
    doubled[10] = 1.0F;
    doubled[12] = 3.0F;
    doubled[15] = 1.0F;
    const Mat4 m = slash::render::multiply(pixel_projection(200.0F, 100.0F), doubled);
    const auto clip = [&](float x, float y) {
        return std::array<float, 2>{(m[0] * x) + (m[4] * y) + m[12],
                                    (m[1] * x) + (m[5] * y) + m[13]};
    };
    CHECK(clip(0, 0)[0] == doctest::Approx(-0.97F));
    CHECK(clip(0, 0)[1] == 1.0F);
    CHECK(clip(48.5F, 25)[0] == doctest::Approx(0.0F));
    CHECK(clip(48.5F, 25)[1] == doctest::Approx(0.0F));
    Mat4 identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0F;
    CHECK(slash::render::multiply(identity, doubled) == doubled);
}

TEST_CASE("the software and GPU devices draw, blend, sample and read back pixels") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        auto target = device->create_texture(target_desc(8));
        REQUIRE(target.ok());

        // A new render target is transparent black.
        {
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            CHECK(pixel_is(image.value(), 3, 3, {0, 0, 0, 0}).empty());
        }

        // Clearing and drawing plain color.
        {
            const Image image =
                render(*device, target.value(),
                       {.target = {}, .clear = {0.25F, 0.5F, 0.75F, 1.0F}, .view = {}},
                       {{TextureId::None, rect(0, 0, 4, 8, {255, 0, 0, 255})}});
            CHECK(pixel_is(image, 1, 1, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image, 3, 7, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image, 4, 0, {64, 128, 191, 255}).empty());
            CHECK(pixel_is(image, 7, 7, {64, 128, 191, 255}).empty());
        }

        // Blending straight alpha over the clear color.
        {
            const Image image =
                render(*device, target.value(),
                       {.target = {}, .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}},
                       {{TextureId::None, rect(0, 0, 8, 8, {255, 255, 255, 128})}});
            CHECK(pixel_is(image, 4, 4, {128, 128, 128, 255}).empty());
        }

        // Sampling a texture with nearest filtering.
        {
            // Red, green / blue, white: each texel fills a quarter of the target.
            const std::vector<std::uint8_t> texels{255, 0, 0,   255, 0,   255, 0,   255,
                                                   0,   0, 255, 255, 255, 255, 255, 255};
            auto texture = device->create_texture(
                {.width = 2, .height = 2, .filter = Filter::Nearest, .render_target = false},
                texels);
            REQUIRE(texture.ok());
            Image image =
                render(*device, target.value(), {}, {{texture.value(), rect(0, 0, 8, 8)}});
            CHECK(pixel_is(image, 1, 1, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image, 6, 1, {0, 255, 0, 255}).empty());
            CHECK(pixel_is(image, 1, 6, {0, 0, 255, 255}).empty());
            CHECK(pixel_is(image, 6, 6, {255, 255, 255, 255}).empty());

            // The vertex color multiplies the texture, and an update shows in the next frame.
            const std::vector<std::uint8_t> gray{200, 200, 200, 255, 200, 200, 200, 255,
                                                 200, 200, 200, 255, 200, 200, 200, 255};
            REQUIRE(device->update_texture(texture.value(), gray).ok());
            image = render(*device, target.value(), {},
                           {{texture.value(), rect(0, 0, 8, 8, {255, 128, 0, 255})}});
            CHECK(pixel_is(image, 5, 5, {200, 100, 0, 255}).empty());
        }

        // A view matrix that moves everything 4 pixels right.
        {
            // Not symmetric, so a transposed upload would put the shift in w and draw nothing here.
            Mat4 view = pixel_projection(8.0F, 8.0F);
            view[12] += view[0] * 4.0F;
            const Image image =
                render(*device, target.value(), {.target = {}, .clear = {}, .view = view},
                       {{TextureId::None, rect(0, 0, 4, 8, {0, 255, 0, 255})}});
            CHECK(pixel_is(image, 1, 4, {0, 0, 0, 255}).empty());
            CHECK(pixel_is(image, 6, 4, {0, 255, 0, 255}).empty());
        }

        // A draw's transform moves it before the view does, as a camera moves the world, and
        // the draws after it without one are where the view alone puts them.
        {
            Material moved;
            moved.transform = Mat4{2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F,
                                   0.0F, 0.0F, 1.0F, 0.0F, 4.0F, 0.0F, 0.0F, 1.0F};
            REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 0, 2, 2, {255, 0, 0, 255}), moved).ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 6, 2, 8, {0, 0, 255, 255})).ok());
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            CHECK(pixel_is(image.value(), 1, 1, {0, 0, 0, 255}).empty());
            CHECK(pixel_is(image.value(), 4, 0, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image.value(), 7, 3, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image.value(), 7, 4, {0, 0, 0, 255}).empty());
            CHECK(pixel_is(image.value(), 1, 7, {0, 0, 255, 255}).empty());
        }

        // Batching draws and destroying a texture mid-frame.
        {
            auto texture = device->create_texture(
                {.width = 1, .height = 1, .filter = Filter::Linear, .render_target = false},
                std::vector<std::uint8_t>{0, 0, 255, 255});
            REQUIRE(texture.ok());
            const slash::render::Stats before = device->stats();
            REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 0, 8, 4)).ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 4, 4, 8, {255, 0, 0, 255})).ok());
            REQUIRE(device->draw(texture.value(), rect(4, 4, 8, 8)).ok());
            device->destroy_texture(texture.value()); // still drawn: it goes after the frame
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            CHECK(pixel_is(image.value(), 2, 2, {255, 255, 255, 255}).empty());
            CHECK(pixel_is(image.value(), 2, 6, {255, 0, 0, 255}).empty());
            CHECK(pixel_is(image.value(), 6, 6, {0, 0, 255, 255}).empty());
            CHECK(device->stats().draws - before.draws == 2);
            CHECK(device->stats().triangles - before.triangles == 6);
            CHECK(device->stats().textures == before.textures - 1);
        }

        // Destroying the frame's own target mid-frame: the frame still ends, and the texture goes.
        {
            auto doomed = device->create_texture(target_desc(4));
            REQUIRE(doomed.ok());
            const std::uint32_t textures = device->stats().textures;
            REQUIRE(device->begin_frame({.target = doomed.value(), .clear = {}, .view = {}}).ok());
            REQUIRE(device->draw(TextureId::None, rect(0, 0, 4, 4)).ok());
            device->destroy_texture(doomed.value());
            CHECK(device->end_frame().ok());
            CHECK(device->stats().textures == textures - 1);
            CHECK(error_of(device->read_texture(doomed.value())).starts_with("no texture"));
        }
    }
}

// Direct3D 12 compiles the HLSL when it first needs a shader, with Windows' own compiler, so a
// shader that SPIR-V and MSL were made from could still fail there. CI runs this on Windows.
TEST_CASE("distance_field_coverage reads a multichannel distance field as Godot does") {
    const DistanceField field{.pixel_range = 4.0F, .outline = 0.0F};
    // The median of red, green and blue is the distance: 0.5 is the edge, half covered.
    CHECK(distance_field_coverage({0.6F, 0.5F, 0.4F, 0.0F}, field, 1.0F) == doctest::Approx(0.5F));
    CHECK(distance_field_coverage({0.9F, 0.7F, 0.1F, 0.0F}, field, 1.0F) == 1.0F);
    CHECK(distance_field_coverage({0.3F, 0.2F, 0.9F, 0.0F}, field, 1.0F) == 0.0F);
    // Drawn twice as big, the edge is twice as sharp: 4 texels of range over 8 pixels.
    CHECK(distance_field_coverage({0.55F, 0.55F, 0.55F, 0.0F}, field, 2.0F) ==
          doctest::Approx(0.9F));
    // An outline widens the shape, and takes the true distance in alpha into account.
    const DistanceField outlined{.pixel_range = 4.0F, .outline = 1.0F};
    CHECK(distance_field_coverage({0.4F, 0.4F, 0.4F, 0.4F}, outlined, 1.0F) ==
          doctest::Approx(0.6F));
    CHECK(distance_field_coverage({0.4F, 0.4F, 0.4F, 0.2F}, outlined, 1.0F) == 0.0F);
}

TEST_CASE("the software and GPU devices draw distance fields as distance_field_coverage does") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        constexpr std::uint32_t kSize = 8;
        auto target = device->create_texture(target_desc(kSize));
        REQUIRE(target.ok());
        // A field near the edge everywhere, so most pixels are partly covered; red, green and blue
        // differ so the median matters, and alpha is lower, for outlines.
        std::vector<std::uint8_t> texels;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const auto base = static_cast<std::uint8_t>(112 + (x * 3) + y);
                texels.insert(texels.end(), {static_cast<std::uint8_t>(base + 10), base,
                                             static_cast<std::uint8_t>(base - 10),
                                             static_cast<std::uint8_t>(base - 20)});
            }
        }
        auto texture = device->create_texture(
            {.width = kSize, .height = kSize, .filter = Filter::Nearest, .render_target = false},
            texels);
        REQUIRE(texture.ok());

        for (const float outline : {0.0F, 1.0F}) {
            CAPTURE(outline);
            Material material;
            material.field = {.pixel_range = 4.0F, .outline = outline};
            auto began = device->begin_frame(
                {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}});
            REQUIRE(began.ok());
            REQUIRE(device->draw(texture.value(), rect(0, 0, kSize, kSize), material).ok());
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            for (std::uint32_t y = 0; y < kSize; ++y) {
                for (std::uint32_t x = 0; x < kSize; ++x) {
                    const std::size_t at = (std::size_t{y} * kSize + x) * 4;
                    const std::array<float, 4> texel{static_cast<float>(texels[at]) / 255.0F,
                                                     static_cast<float>(texels[at + 1]) / 255.0F,
                                                     static_cast<float>(texels[at + 2]) / 255.0F,
                                                     static_cast<float>(texels[at + 3]) / 255.0F};
                    // White over black, so each pixel is the coverage.
                    const auto gray = static_cast<std::uint8_t>(
                        std::lround(distance_field_coverage(texel, material.field, 1.0F) * 255.0F));
                    CHECK(pixel_is(image.value(), x, y, {gray, gray, gray, 255}).empty());
                }
            }
        }
    }
}

TEST_CASE("every shader compiles with Windows' HLSL compiler") {
#if defined(_WIN32)
    namespace shaders = slash::render::shaders;
    const std::array<std::pair<const char*, std::string_view>, 4> sources{{
        {"sprite", shaders::kSpriteHlsl},
        {"material", shaders::kMaterialHlsl},
        {"mesh", shaders::kMeshHlsl},
        {"shadow", shaders::kShadowHlsl},
    }};
    for (const auto& [name, hlsl] : sources) {
        for (const auto& [entry, target] :
             {std::pair{"vs_main", "vs_5_1"}, {"fs_main", "ps_5_1"}}) {
            const auto compiled = slash::render::detail::compile_hlsl(hlsl, entry, target);
            CHECK_MESSAGE(compiled.ok(), name, " ", entry, ": ",
                          (compiled.ok() ? std::string() : compiled.error().message));
        }
    }
    for (const auto& [name, hlsl] : {std::pair{"particles_process", shaders::kParticlesProcessHlsl},
                                     std::pair{"particles_emit", shaders::kParticlesEmitHlsl},
                                     std::pair{"particles_draw", shaders::kParticlesDrawHlsl}}) {
        const auto compiled = slash::render::detail::compile_hlsl(hlsl, "cs_main", "cs_5_1");
        CHECK_MESSAGE(compiled.ok(), name, ": ",
                      (compiled.ok() ? std::string() : compiled.error().message));
    }
#else
    MESSAGE("HLSL compiles only on Windows");
#endif
}

TEST_CASE("the GPU device refuses the same mistakes as the null device") {
    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    check_refusals(*device);
    CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
          "this device has no window; draw to a render-target texture");
}

TEST_CASE("the OpenGL devices refuse the same mistakes as the null device") {
    for (const char* driver : {"opengl", "opengl_es"}) {
        CAPTURE(driver);
        auto device = gl_device(driver);
        if (device == nullptr) {
            continue;
        }
        check_refusals(*device);
        CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
              "this device has no window; draw to a render-target texture");
        CHECK(error_of(
                  device->create_texture(
                      {.width = 1, .height = 1, .filter = Filter::Linear, .render_target = false},
                      std::vector<std::uint8_t>(3)))
                  .starts_with("a 1x1 texture takes 4 bytes"));
    }
}

// Linux has both: Mesa's OpenGL 4.5 and OpenGL ES 3.2. macOS has OpenGL 4.1 and no OpenGL ES.
TEST_CASE("the opengl driver draws with OpenGL 3.3, and opengl_es with OpenGL ES 3.0") {
    if (auto device = gl_device("opengl"); device != nullptr) {
        CHECK(device->backend() == slash::render::Backend::OpenGl);
        CHECK(slash::render::backend_name(device->backend()) == "opengl");
    }
    if (auto device = gl_device("opengl_es"); device != nullptr) {
        CHECK(device->backend() == slash::render::Backend::OpenGlEs);
        CHECK(slash::render::backend_name(device->backend()) == "opengl_es");
    }
}

TEST_CASE("a GPU driver falls back to OpenGL where its backend doesn't work, as Godot's") {
    using slash::render::choose_driver;
    // A backend this system never has.
#ifdef __APPLE__
    const std::string missing = "d3d12";
#else
    const std::string missing = "metal";
#endif
    CHECK(choose_driver(missing, true) == "opengl");
    CHECK(choose_driver(missing, false) == missing);
    for (const std::string driver : {"opengl", "opengl_es", "null"}) {
        CHECK(choose_driver(driver, true) == driver);
    }
    // auto stays where a GPU backend works.
    if (gpu_device() != nullptr) {
        CHECK(choose_driver("auto", true) == "auto");
    }
    CHECK(slash::render::uses_opengl("opengl_es"));
    CHECK_FALSE(slash::render::uses_opengl("vulkan"));
    CHECK_FALSE(slash::render::uses_opengl("webgpu"));
#ifdef __EMSCRIPTEN__
    // WebGPU, where the browser has it, needs no fallback; auto is WebGL 2 where it doesn't.
    if (gpu_device() != nullptr) {
        CHECK(choose_driver("webgpu", true) == "webgpu");
        CHECK_FALSE(slash::render::uses_opengl("auto"));
    }
#else
    CHECK(choose_driver("webgpu", true) == "opengl");
#endif
}

TEST_CASE("the software device refuses the same mistakes as the null device") {
    auto device = slash::render::create_software_device();
    CHECK(device->backend() == slash::render::Backend::Software);
    CHECK(slash::render::backend_name(device->backend()) == "software");
    check_refusals(*device);
    CHECK(error_of(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}})) ==
          "the software device has no window; draw to a render-target texture");
}

TEST_CASE("the software device covers the pixels a GPU does") {
    auto device = slash::render::create_software_device();
    auto target = device->create_texture(target_desc(8));
    REQUIRE(target.ok());
    const Frame black{.target = {}, .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}};

    // Pixel centers on a top or left edge are inside; on a bottom or right edge, outside.
    Image image =
        render(*device, target.value(), black, {{TextureId::None, rect(0.5F, 0.5F, 2.5F, 2.5F)}});
    CHECK(pixel_is(image, 0, 0, {255, 255, 255, 255}).empty());
    CHECK(pixel_is(image, 1, 1, {255, 255, 255, 255}).empty());
    CHECK(pixel_is(image, 2, 1, {0, 0, 0, 255}).empty());
    CHECK(pixel_is(image, 1, 2, {0, 0, 0, 255}).empty());

    // Triangles that share an edge cover each pixel on it once: added at half alpha, nothing
    // is added twice.
    Material add;
    add.blend = BlendMode::Add;
    REQUIRE(device->begin_frame({.target = target.value(), .clear = black.clear, .view = {}}).ok());
    REQUIRE(device->draw(TextureId::None, rect(0, 0, 8, 8, {255, 255, 255, 128}), add).ok());
    REQUIRE(device->end_frame().ok());
    image = device->read_texture(target.value()).value();
    for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK(pixel_is(image, i, i, {128, 128, 128, 255}).empty());
        CHECK(pixel_is(image, 7 - i, i, {128, 128, 128, 255}).empty());
    }

    // Slivers and edges through pixel centers land on the same pixels as on a GPU.
    const auto corner = [](float x, float y) {
        return Vertex2D{.x = x, .y = y, .u = 0.0F, .v = 0.0F, .rgba = {255, 255, 255, 255}};
    };
    const std::vector<Vertex2D> triangles{
        corner(0.3F, 0.2F), corner(7.9F, 1.1F), corner(2.2F, 7.6F), // thin and slanted
        corner(5.5F, 0.5F), corner(7.5F, 7.5F), corner(5.5F, 7.5F), // edges through centers
        corner(1.0F, 4.5F), corner(1.0F, 7.0F), corner(4.0F, 4.5F), // wound the other way
        corner(6.1F, 0.1F), corner(6.3F, 0.1F), corner(6.2F, 3.9F), // narrower than a pixel
    };
    image = render(*device, target.value(), black, {{TextureId::None, triangles}});
    auto gpu = gpu_device();
    if (gpu == nullptr) {
        return;
    }
    auto gpu_target = gpu->create_texture(target_desc(8));
    REQUIRE(gpu_target.ok());
    const Image expected = render(*gpu, gpu_target.value(), black, {{TextureId::None, triangles}});
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t at = (std::size_t{y} * 8 + x) * 4;
            CHECK(pixel_is(image, x, y,
                           {expected.pixels[at], expected.pixels[at + 1], expected.pixels[at + 2],
                            expected.pixels[at + 3]})
                      .empty());
        }
    }
}

TEST_CASE("V-Sync modes have Godot's names") {
    for (const VsyncMode mode :
         {VsyncMode::Disabled, VsyncMode::Enabled, VsyncMode::Adaptive, VsyncMode::Mailbox}) {
        CHECK(slash::render::parse_vsync_mode(slash::render::vsync_mode_name(mode)) == mode);
    }
    CHECK(slash::render::vsync_mode_name(VsyncMode::Mailbox) == "mailbox");
    CHECK_FALSE(slash::render::parse_vsync_mode("on").has_value());
}

TEST_CASE("a device with no window records the V-Sync mode it is given") {
    auto null_device = create_null_device();
    CHECK(null_device->vsync() == VsyncMode::Enabled);
    CHECK(null_device->set_vsync(VsyncMode::Adaptive) == VsyncMode::Adaptive);
    CHECK(null_device->vsync() == VsyncMode::Adaptive);

    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    CHECK(device->vsync() == VsyncMode::Enabled);
    CHECK(device->set_vsync(VsyncMode::Disabled) == VsyncMode::Disabled);
    CHECK(device->vsync() == VsyncMode::Disabled);

    if (auto gl = gl_device(); gl != nullptr) {
        CHECK(gl->set_vsync(VsyncMode::Mailbox) == VsyncMode::Mailbox);
    }
}

// Windows need a page to draw in, which the web build, run in Node, has none of.
#ifndef __EMSCRIPTEN__
// A window on SDL's offscreen video driver, which can make a Vulkan surface: Linux CI runs this
// on software Vulkan. Elsewhere, or with no Vulkan driver, it is skipped.
TEST_CASE("a window's V-Sync mode changes its present mode, falling back to enabled") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    auto platform = slash::platform::Platform::create(
        {.title = "vsync test", .width = 64, .height = 64, .headless = false});
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    std::unique_ptr<RenderDevice> device;
    if (platform.ok()) {
        auto created =
            create_device({.window = platform.value().window(), .driver = "auto", .debug = false});
        if (created.ok()) {
            device = std::move(created).value();
        }
    }
    if (device == nullptr) {
#ifdef __linux__
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works with an offscreen window");
        }
#endif
        MESSAGE("no GPU window here, so present modes are not checked");
        return;
    }
    CHECK(device->vsync() == VsyncMode::Enabled);
    // SDL has no adaptive present mode; the others are used where the driver has them.
    CHECK(device->set_vsync(VsyncMode::Adaptive) == VsyncMode::Enabled);
    const VsyncMode mailbox = device->set_vsync(VsyncMode::Mailbox);
    CHECK((mailbox == VsyncMode::Mailbox || mailbox == VsyncMode::Enabled));
    CHECK(device->vsync() == mailbox);
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());
    const VsyncMode disabled = device->set_vsync(VsyncMode::Disabled);
    CHECK((disabled == VsyncMode::Disabled || disabled == VsyncMode::Enabled));
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());
    CHECK(device->set_vsync(VsyncMode::Enabled) == VsyncMode::Enabled);
    CHECK(device->stats().frames == 2);
}

// A window made for OpenGL on SDL's offscreen video driver, which draws with EGL: Linux CI runs
// this on Mesa, with OpenGL 3.3 and OpenGL ES 3.0. Frames draw into a texture and are copied to
// the window turned the right way up, so a frame that reaches the window has gone through every
// step.
namespace {
void check_gl_window(bool es) {
    const char* driver = es ? "opengl_es" : "opengl";
    CAPTURE(driver);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    auto platform = slash::platform::Platform::create({.title = "opengl test",
                                                       .width = 64,
                                                       .height = 48,
                                                       .headless = false,
                                                       .opengl = true,
                                                       .opengl_es = es});
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    std::unique_ptr<RenderDevice> device;
    std::string error = "no window";
    if (platform.ok()) {
        auto created =
            create_device({.window = platform.value().window(), .driver = driver, .debug = false});
        if (created.ok()) {
            device = std::move(created).value();
        } else {
            error = created.error().message;
        }
    }
    if (device == nullptr) {
#ifdef __linux__
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no OpenGL window works: " << error);
        }
#endif
        MESSAGE("no OpenGL window here, so it is not checked: " << error);
        return;
    }
    CHECK(device->backend() ==
          (es ? slash::render::Backend::OpenGlEs : slash::render::Backend::OpenGl));
    CHECK(device->vsync() == VsyncMode::Enabled);
    // GL has no mailbox mode; the others are used where the driver has them.
    CHECK(device->set_vsync(VsyncMode::Mailbox) == VsyncMode::Enabled);
    const VsyncMode adaptive = device->set_vsync(VsyncMode::Adaptive);
    CHECK((adaptive == VsyncMode::Adaptive || adaptive == VsyncMode::Enabled));
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());

    // A frame with draws, then a resized window, which gets a new texture to draw into.
    REQUIRE(device->begin_frame({.target = TextureId::None, .clear = {}, .view = {}}).value());
    REQUIRE(device->draw(TextureId::None, rect(0, 0, 32, 24, {255, 0, 0, 255})).ok());
    CHECK(device->end_frame().ok());
    SDL_SetWindowSize(platform.value().window(), 80, 60);
    CHECK(slash::render::draw_empty_frame(*device, slash::render::kDefaultClearColor).ok());
    CHECK(device->set_vsync(VsyncMode::Enabled) == VsyncMode::Enabled);
    CHECK(device->stats().frames == 3);
    CHECK(device->stats().draws == 1);
}

} // namespace

TEST_CASE("an OpenGL window draws frames and changes its swap interval") {
    check_gl_window(false);
    check_gl_window(true);
}

TEST_CASE("the OpenGL device needs a window made for OpenGL") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    auto platform = slash::platform::Platform::create(
        {.title = "not opengl", .width = 32, .height = 32, .headless = false, .opengl = false});
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    if (!platform.ok()) {
        MESSAGE("no offscreen window here: " << platform.error().message);
        return;
    }
    auto device =
        create_device({.window = platform.value().window(), .driver = "opengl", .debug = false});
    CHECK(error_of(device) ==
          "the window wasn't made for OpenGL; create it with SDL_WINDOW_OPENGL");
}
#endif
