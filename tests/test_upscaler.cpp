#include "../engine/render/src/fsr_shaders.hpp"
#include "devices.hpp"

#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"
#ifndef __EMSCRIPTEN__
#include "slash/shader_compiler/shader_compiler.hpp"
#endif

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using slash::render::Filter;
using slash::render::Frame;
using slash::render::Image;
using slash::render::RenderDevice;
using slash::render::Scaling;
using slash::render::TextureId;
using slash::render::UpscaleSize;
using slash::render::Vertex2D;

namespace {

std::vector<Vertex2D> rect(float width, float height) {
    const Vertex2D top_left{.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F};
    const Vertex2D top_right{.x = width, .y = 0.0F, .u = 1.0F, .v = 0.0F};
    const Vertex2D bottom_left{.x = 0.0F, .y = height, .u = 0.0F, .v = 1.0F};
    const Vertex2D bottom_right{.x = width, .y = height, .u = 1.0F, .v = 1.0F};
    return {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left};
}

// An 8x8 picture with a diagonal edge between dark and light, and one green texel.
constexpr std::uint32_t kSize = 8;
std::vector<std::uint8_t> picture() {
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            std::array<std::uint8_t, 4> rgba{30, 40, 50, 255};
            if (static_cast<float>(x) + (static_cast<float>(y) * 0.5F) >= 4.5F) {
                rgba = {220, 200, 180, 255};
            }
            if (x == 6 && y == 1) {
                rgba = {0, 255, 0, 255};
            }
            pixels.insert(pixels.end(), rgba.begin(), rgba.end());
        }
    }
    return pixels;
}

using Rgb = std::array<float, 3>;

// AMD's FsrEasuF and FsrRcasF (ffx_fsr1.h, 32-bit) on the CPU, to check the shaders against.
struct Reference {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgb> pixels;

    [[nodiscard]] Rgb at(int x, int y) const {
        x = std::clamp(x, 0, static_cast<int>(width) - 1);
        y = std::clamp(y, 0, static_cast<int>(height) - 1);
        return pixels[(static_cast<std::size_t>(y) * width) + static_cast<std::size_t>(x)];
    }
};

float rcp_lo(float a) {
    return std::bit_cast<float>(0x7ef07ebbU - std::bit_cast<std::uint32_t>(a));
}
float rsq_lo(float a) {
    return std::bit_cast<float>(0x5f347d74U - (std::bit_cast<std::uint32_t>(a) >> 1U));
}
float rcp_med(float a) {
    const float b = std::bit_cast<float>(0x7ef19fffU - std::bit_cast<std::uint32_t>(a));
    return b * (-b * a + 2.0F);
}
float luma(const Rgb& c) {
    return c[2] * 0.5F + (c[0] * 0.5F + c[1]);
}

void easu_set(float& dir_x_sum, float& dir_y_sum, float& len, float w, float la, float lb, float lc,
              float ld, float le) {
    float len_x = rcp_lo(std::max(std::abs(ld - lc), std::abs(lc - lb)));
    const float dir_x = ld - lb;
    dir_x_sum += dir_x * w;
    len_x = std::clamp(std::abs(dir_x) * len_x, 0.0F, 1.0F);
    len += len_x * len_x * w;
    float len_y = rcp_lo(std::max(std::abs(le - lc), std::abs(lc - la)));
    const float dir_y = le - la;
    dir_y_sum += dir_y * w;
    len_y = std::clamp(std::abs(dir_y) * len_y, 0.0F, 1.0F);
    len += len_y * len_y * w;
}

void easu_tap(Rgb& color, float& weight, float off_x, float off_y, float dir_x, float dir_y,
              float len_x, float len_y, float lob, float clp, const Rgb& c) {
    const float vx = ((off_x * dir_x) + (off_y * dir_y)) * len_x;
    const float vy = ((off_x * -dir_y) + (off_y * dir_x)) * len_y;
    const float d2 = std::min((vx * vx) + (vy * vy), clp);
    float wb = (2.0F / 5.0F) * d2 - 1.0F;
    float wa = lob * d2 - 1.0F;
    wb *= wb;
    wa *= wa;
    wb = (25.0F / 16.0F) * wb - (25.0F / 16.0F - 1.0F);
    const float w = wb * wa;
    for (std::size_t i = 0; i < 3; ++i) {
        color[i] += c[i] * w;
    }
    weight += w;
}

Rgb easu(const Reference& in, std::uint32_t out_width, std::uint32_t out_height, int ox, int oy) {
    const float px = ((static_cast<float>(ox) + 0.5F) * static_cast<float>(in.width) /
                      static_cast<float>(out_width)) -
                     0.5F;
    const float py = ((static_cast<float>(oy) + 0.5F) * static_cast<float>(in.height) /
                      static_cast<float>(out_height)) -
                     0.5F;
    const float fx = std::floor(px);
    const float fy = std::floor(py);
    const float ppx = px - fx;
    const float ppy = py - fy;
    const int x = static_cast<int>(fx);
    const int y = static_cast<int>(fy);
    const Rgb b = in.at(x, y - 1);
    const Rgb c = in.at(x + 1, y - 1);
    const Rgb e = in.at(x - 1, y);
    const Rgb f = in.at(x, y);
    const Rgb g = in.at(x + 1, y);
    const Rgb h = in.at(x + 2, y);
    const Rgb i = in.at(x - 1, y + 1);
    const Rgb j = in.at(x, y + 1);
    const Rgb k = in.at(x + 1, y + 1);
    const Rgb l = in.at(x + 2, y + 1);
    const Rgb n = in.at(x, y + 2);
    const Rgb o = in.at(x + 1, y + 2);
    float dir_x = 0.0F;
    float dir_y = 0.0F;
    float len = 0.0F;
    easu_set(dir_x, dir_y, len, (1.0F - ppx) * (1.0F - ppy), luma(b), luma(e), luma(f), luma(g),
             luma(j));
    easu_set(dir_x, dir_y, len, ppx * (1.0F - ppy), luma(c), luma(f), luma(g), luma(h), luma(k));
    easu_set(dir_x, dir_y, len, (1.0F - ppx) * ppy, luma(f), luma(i), luma(j), luma(k), luma(n));
    easu_set(dir_x, dir_y, len, ppx * ppy, luma(g), luma(j), luma(k), luma(l), luma(o));
    float dir_r = (dir_x * dir_x) + (dir_y * dir_y);
    const bool zero = dir_r < 1.0F / 32768.0F;
    dir_r = zero ? 1.0F : rsq_lo(dir_r);
    dir_x = zero ? 1.0F : dir_x;
    dir_x *= dir_r;
    dir_y *= dir_r;
    len = len * 0.5F;
    len *= len;
    const float stretch =
        ((dir_x * dir_x) + (dir_y * dir_y)) * rcp_lo(std::max(std::abs(dir_x), std::abs(dir_y)));
    const float len_x = 1.0F + (stretch - 1.0F) * len;
    const float len_y = 1.0F - 0.5F * len;
    const float lob = 0.5F + ((1.0F / 4.0F - 0.04F) - 0.5F) * len;
    const float clp = rcp_lo(lob);
    Rgb color{};
    float weight = 0.0F;
    const auto tap = [&](float dx, float dy, const Rgb& texel) {
        easu_tap(color, weight, dx - ppx, dy - ppy, dir_x, dir_y, len_x, len_y, lob, clp, texel);
    };
    tap(0, -1, b);
    tap(1, -1, c);
    tap(-1, 1, i);
    tap(0, 1, j);
    tap(0, 0, f);
    tap(-1, 0, e);
    tap(1, 1, k);
    tap(2, 1, l);
    tap(2, 0, h);
    tap(1, 0, g);
    tap(1, 2, o);
    tap(0, 2, n);
    Rgb out{};
    for (std::size_t ch = 0; ch < 3; ++ch) {
        const float lo = std::min({f[ch], g[ch], j[ch], k[ch]});
        const float hi = std::max({f[ch], g[ch], j[ch], k[ch]});
        out[ch] = std::min(hi, std::max(lo, color[ch] * (1.0F / weight)));
    }
    return out;
}

Rgb rcas(const Reference& in, int x, int y, float sharpness) {
    const Rgb b = in.at(x, y - 1);
    const Rgb d = in.at(x - 1, y);
    const Rgb e = in.at(x, y);
    const Rgb f = in.at(x + 1, y);
    const Rgb h = in.at(x, y + 1);
    float lobe = -1.0F;
    for (std::size_t ch = 0; ch < 3; ++ch) {
        const float mn4 = std::min({b[ch], d[ch], f[ch], h[ch]});
        const float mx4 = std::max({b[ch], d[ch], f[ch], h[ch]});
        const float hit_min = std::min(mn4, e[ch]) / (4.0F * mx4);
        const float hit_max = (1.0F - std::max(mx4, e[ch])) / (4.0F * mn4 - 4.0F);
        // A GPU's max leaves out the NaN 0/0 makes, as the shader does.
        float channel = std::max(-hit_min, hit_max);
        if (std::isnan(hit_min)) {
            channel = hit_max;
        } else if (std::isnan(hit_max)) {
            channel = -hit_min;
        }
        lobe = std::max(lobe, channel);
    }
    lobe = std::max(-(0.25F - (1.0F / 16.0F)), std::min(lobe, 0.0F)) * sharpness;
    const float rcp_l = rcp_med(4.0F * lobe + 1.0F);
    Rgb out{};
    for (std::size_t ch = 0; ch < 3; ++ch) {
        out[ch] = (lobe * b[ch] + lobe * d[ch] + lobe * h[ch] + lobe * f[ch] + e[ch]) * rcp_l;
    }
    return out;
}

float to_unorm(float value) {
    return std::round(std::clamp(value, 0.0F, 1.0F) * 255.0F) / 255.0F;
}

// The picture scaled to out_size by EASU, stored in 8 bits as the device's texture is, then
// sharpened by RCAS.
Reference fsr_reference(std::uint32_t out_size, float sharpness_stops) {
    Reference in{.width = kSize, .height = kSize, .pixels = {}};
    const std::vector<std::uint8_t> pixels = picture();
    for (std::size_t p = 0; p < pixels.size(); p += 4) {
        in.pixels.push_back({static_cast<float>(pixels[p]) / 255.0F,
                             static_cast<float>(pixels[p + 1]) / 255.0F,
                             static_cast<float>(pixels[p + 2]) / 255.0F});
    }
    Reference scaled{.width = out_size, .height = out_size, .pixels = {}};
    for (int y = 0; y < static_cast<int>(out_size); ++y) {
        for (int x = 0; x < static_cast<int>(out_size); ++x) {
            Rgb c = easu(in, out_size, out_size, x, y);
            for (float& channel : c) {
                channel = to_unorm(channel);
            }
            scaled.pixels.push_back(c);
        }
    }
    Reference out{.width = out_size, .height = out_size, .pixels = {}};
    const float sharpness = std::exp2(-sharpness_stops);
    for (int y = 0; y < static_cast<int>(out_size); ++y) {
        for (int x = 0; x < static_cast<int>(out_size); ++x) {
            out.pixels.push_back(rcas(scaled, x, y, sharpness));
        }
    }
    return out;
}

// The picture drawn into an upscaler's input at 8x8, scaled to out_size and drawn over a target
// of that size.
Image upscale(RenderDevice& device, std::string_view name, std::uint32_t out_size,
              const Scaling& scaling) {
    auto upscaler = slash::render::find_upscaler(name);
    REQUIRE(upscaler.has_value());
    std::unique_ptr<slash::render::Upscaler> made = upscaler->make();
    const UpscaleSize size{.render_width = kSize,
                           .render_height = kSize,
                           .output_width = out_size,
                           .output_height = out_size};
    auto input = made->input(device, size);
    REQUIRE(input.ok());
    auto source = device.create_texture(
        {.width = kSize, .height = kSize, .filter = Filter::Nearest, .render_target = false},
        picture());
    REQUIRE(source.ok());
    Frame frame;
    frame.target = input.value();
    REQUIRE(device.begin_frame(frame).ok());
    REQUIRE(device.draw(source.value(), rect(kSize, kSize)).ok());
    REQUIRE(device.end_frame().ok());
    auto drawn = made->upscale(device, size, scaling);
    REQUIRE(drawn.ok());
    auto target = device.create_texture(
        {.width = out_size, .height = out_size, .filter = Filter::Nearest, .render_target = true});
    REQUIRE(target.ok());
    frame.target = target.value();
    REQUIRE(device.begin_frame(frame).ok());
    REQUIRE(device
                .draw(drawn.value().texture,
                      rect(static_cast<float>(out_size), static_cast<float>(out_size)),
                      drawn.value().material)
                .ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    made->release(device);
    device.destroy_texture(source.value());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

// The largest difference, in 8-bit steps, between an image's colors and the reference's.
int largest_difference(const Image& image, const Reference& reference) {
    int largest = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const Rgb want = reference.at(static_cast<int>(x), static_cast<int>(y));
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const int got = image.pixels[((std::size_t{y} * image.width + x) * 4) + ch];
                const int expected = static_cast<int>(std::lround(want[ch] * 255.0F));
                largest = std::max(largest, std::abs(got - expected));
            }
        }
    }
    return largest;
}

} // namespace

TEST_CASE("upscalers are built in or added by name, and temporal ones are refused for now") {
    slash::render::clear_added_upscalers();
    CHECK(slash::render::upscaler_names() == std::vector<std::string>{"bilinear", "fsr"});
    CHECK(slash::render::find_upscaler("fsr").has_value());
    CHECK_FALSE(slash::render::find_upscaler("dlss").has_value());

    struct Copy final : slash::render::Upscaler {
        slash::Result<TextureId> input(RenderDevice& /*device*/,
                                       const UpscaleSize& /*size*/) override {
            return TextureId::None;
        }
        slash::Result<slash::render::UpscaleDraw> upscale(RenderDevice& /*device*/,
                                                          const UpscaleSize& /*size*/,
                                                          const Scaling& /*scaling*/) override {
            return slash::render::UpscaleDraw{};
        }
        void release(RenderDevice& /*device*/) override {}
    };
    const auto make = [] {
        return std::unique_ptr<slash::render::Upscaler>(std::make_unique<Copy>());
    };
    auto temporal = slash::render::add_upscaler(
        {.name = "dlss", .temporal = true, .supports = {}, .make = make});
    REQUIRE_FALSE(temporal.ok());
    CHECK(temporal.error().message ==
          "upscaler dlss is temporal: it needs depth, motion vectors and a jittered camera, which "
          "only the 3D renderer will make");
    CHECK_FALSE(slash::render::add_upscaler(
                    {.name = "fsr", .temporal = false, .supports = {}, .make = make})
                    .ok());
    CHECK_FALSE(slash::render::add_upscaler(
                    {.name = "Bad Name", .temporal = false, .supports = {}, .make = make})
                    .ok());
    // Only on the null device, standing in for a vendor's GPUs.
    REQUIRE(slash::render::add_upscaler({.name = "vendor",
                                         .temporal = false,
                                         .supports =
                                             [](const RenderDevice& device) {
                                                 return device.backend() ==
                                                        slash::render::Backend::Null;
                                             },
                                         .make = make})
                .ok());
    CHECK(slash::render::upscaler_names() == std::vector<std::string>{"bilinear", "fsr", "vendor"});

    // Godot's choices: nothing scales at 1 with bilinear, FSR doesn't scale down, and a game's
    // own upscaler is used where it runs.
    const auto null = slash::render::create_null_device();
    const auto software = slash::render::create_software_device();
    const auto choose = [&](const Scaling& scaling, const RenderDevice& device) {
        return slash::render::choose_upscaler(scaling, device);
    };
    CHECK(choose({}, *null).empty());
    CHECK(choose({.mode = "bilinear", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = {}},
                 *null) == "bilinear");
    CHECK(choose({.mode = "fsr", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = {}}, *null) ==
          "fsr");
    CHECK(choose({.mode = "fsr", .scale = 1.5F, .fsr_sharpness = 0.2F, .upscaler = {}}, *null) ==
          "bilinear");
    const Scaling vendor{.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = "vendor"};
    CHECK(choose(vendor, *null) == "vendor");
    CHECK(choose(vendor, *software) == "fsr");
    slash::render::clear_added_upscalers();

    CHECK(slash::render::render_size(1280, 0.5F) == 640);
    CHECK(slash::render::render_size(1281, 0.77F) == 986);
    CHECK(slash::render::render_size(3, 0.25F) == 1);
    CHECK(slash::render::render_size(10000, 2.0F) == slash::render::kMaxTextureSize);
    CHECK(slash::render::scaling_mode_problem("fsr").empty());
    CHECK(slash::render::scaling_mode_problem("fsr2").empty());
    CHECK(slash::render::scaling_mode_problem("fsr3").empty());
    CHECK(slash::render::scaling_mode_problem("fsr1") ==
          "unknown scaling mode fsr1; expected bilinear, fsr, fsr2 or fsr3 (did you mean fsr?)");
    CHECK(slash::render::scaling_mode_problem("metalfx_temporal") ==
          "metalfx_temporal needs Apple's MetalFX, which isn't built yet; use bilinear, fsr, "
          "fsr2 or fsr3");

    // FSR 2 upscales a Forward+ frame's 3D in the device, at a scale under 1; bilinear scaling
    // stands in for it otherwise, as Godot's.
    const Scaling fsr2{.mode = "fsr2", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = {}};
    using slash::render::RenderMethod;
    CHECK(slash::render::uses_fsr2(fsr2, RenderMethod::ForwardPlus, *software));
    CHECK(slash::render::uses_fsr2(fsr2, RenderMethod::ForwardPlus, *null));
    CHECK_FALSE(slash::render::uses_fsr2(fsr2, RenderMethod::Compatibility, *software));
    CHECK_FALSE(slash::render::uses_fsr2(
        {.mode = "fsr2", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = {}},
        RenderMethod::ForwardPlus, *software));
    CHECK_FALSE(slash::render::uses_fsr2(
        {.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = {}},
        RenderMethod::ForwardPlus, *software));
    CHECK(choose(fsr2, *software) == "bilinear");
    CHECK(choose({.mode = "fsr2", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = {}}, *software)
              .empty());
}

// The web build of the tests (tests/web) has no shader tools.
#ifndef __EMSCRIPTEN__
TEST_CASE("the FSR shaders committed in engine/render/src match their HLSL") {
    // Run "cmake --build --preset dev --target shaders" to make them again. MSL and GLSL ES are
    // left out: SPIRV-Cross writes the same SPIR-V's numbers differently on some platforms. Tint
    // writes WGSL the same everywhere.
    const std::string root = std::string(SLASH_TEST_DATA_DIR) + "/../..";
    const auto compile = [&](const std::string& path,
                             std::vector<slash::render::ShaderSource::Param> params) {
        auto code = slash::platform::read_file(root + "/" + path);
        REQUIRE(code.ok());
        auto compiled = slash::shader_compiler::compile(
            {.path = path, .code = std::move(code).value(), .params = std::move(params)});
        REQUIRE_MESSAGE(compiled.ok(), (compiled.ok() ? "" : compiled.error().message));
        return std::move(compiled).value();
    };
    const auto same = [](const slash::render::CompiledShader& made,
                         const slash::render::CompiledShader& committed) {
        CHECK(made.spirv_vertex == committed.spirv_vertex);
        CHECK(made.spirv_fragment == committed.spirv_fragment);
        CHECK(made.cpu_vertex == committed.cpu_vertex);
        CHECK(made.cpu_fragment == committed.cpu_fragment);
        CHECK(made.wgsl_vertex == committed.wgsl_vertex);
        CHECK(made.wgsl_fragment == committed.wgsl_fragment);
        CHECK(made.hlsl == committed.hlsl);
        CHECK(made.normal_map_used == committed.normal_map_used);
        CHECK(made.specular_used == committed.specular_used);
        CHECK_FALSE(committed.msl_fragment.empty());
        CHECK_FALSE(committed.glsl_fragment.empty());
    };
    same(compile("engine/render/shaders/fsr_easu.hlsl", {}), slash::render::shaders::fsr_easu());
    same(compile("engine/render/shaders/fsr_rcas.hlsl",
                 {{.name = "sharpness", .components = 1, .slot = 0}}),
         slash::render::shaders::fsr_rcas());
}
#endif

TEST_CASE("FSR scales up as AMD's EASU and RCAS do, on the software and GPU devices") {
    for (const float stops : {0.2F, 0.0F, 2.0F}) {
        CAPTURE(stops);
        const Reference reference = fsr_reference(16, stops);
        const Scaling scaling{.mode = "fsr", .scale = 0.5F, .fsr_sharpness = stops, .upscaler = {}};
        for (const auto& device : slash::test::drawing_devices()) {
            CAPTURE(slash::render::backend_name(device->backend()));
            const Image image = upscale(*device, "fsr", 16, scaling);
            REQUIRE(image.width == 16);
            // The software device runs the shaders' SPIR-V on the CPU; GPUs round a little
            // differently.
            const bool software = device->backend() == slash::render::Backend::Software;
            CHECK(largest_difference(image, reference) <= (software ? 1 : 2));
            // Flat corners stay their color, and the alpha passes through.
            CHECK(image.pixels[0] == 30);
            CHECK(image.pixels[3] == 255);
        }
    }
}

TEST_CASE("bilinear scaling stretches the render, filtered") {
    for (const auto& device : slash::test::drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        const Image image = upscale(*device, "bilinear", 16, {});
        // Each pixel mixes the four texels around its center: (13, 3) is 9/16 of the green
        // texel and 7/16 of the light ones beside and below it.
        const std::size_t at = ((std::size_t{3} * 16) + 13) * 4;
        CHECK(std::abs(image.pixels[at] - 96) <= 1);
        CHECK(std::abs(image.pixels[at + 1] - 231) <= 1);
    }
}
