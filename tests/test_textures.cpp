#include "blocks.hpp"
#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/texture_encoder.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
using render::Filter;
using render::Image;
using render::TextureFormat;
using render::Vertex2D;
using slash::test::astc_block;
using slash::test::bc7_block;
using slash::test::quadrant_blocks;
using slash::test::quadrant_pixels;
using slash::test::Rgba;

namespace {

// Every channel odd, as a hand-written BC7 block needs.
constexpr std::array<Rgba, 4> kQuadrants{
    {{255, 1, 1, 255}, {1, 255, 1, 255}, {1, 1, 255, 255}, {201, 101, 51, 129}}};

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

// A 16x16 picture with smooth color and alpha, as sprites have.
std::vector<std::uint8_t> gradient() {
    std::vector<std::uint8_t> out;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            out.push_back(static_cast<std::uint8_t>(x * 16));
            out.push_back(static_cast<std::uint8_t>(y * 16));
            out.push_back(static_cast<std::uint8_t>(255 - (x * 8) - (y * 4)));
            out.push_back(static_cast<std::uint8_t>(128 + (x * 4) + (y * 3)));
        }
    }
    return out;
}

int largest_difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    int largest = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        largest = std::max(largest, std::abs(int{a[i]} - int{b[i]}));
    }
    return largest;
}

// The peak signal-to-noise ratio of b against a, in decibels.
double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    double squared = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double difference = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        squared += difference * difference;
    }
    return 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(a.size()) / squared);
}

std::string ktx2_of(const Image& image) {
    auto file = render::encode_ktx2(image);
    REQUIRE(file.ok());
    return std::move(file).value();
}

void put32(std::string& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<char>((value >> (8 * i)) & 0xFFU);
    }
}

std::vector<Vertex2D> whole(float size) {
    const Vertex2D a{.x = 0, .y = 0, .u = 0, .v = 0, .rgba = {255, 255, 255, 255}};
    const Vertex2D b{.x = size, .y = 0, .u = 1, .v = 0, .rgba = {255, 255, 255, 255}};
    const Vertex2D c{.x = 0, .y = size, .u = 0, .v = 1, .rgba = {255, 255, 255, 255}};
    const Vertex2D d{.x = size, .y = size, .u = 1, .v = 1, .rgba = {255, 255, 255, 255}};
    return {a, b, c, b, d, c};
}

// The texture drawn over a transparent target its size.
std::vector<std::uint8_t> drawn(render::RenderDevice& device, render::TextureId texture,
                                std::uint32_t size) {
    auto target = device.create_texture(
        {.width = size, .height = size, .filter = Filter::Nearest, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(device.begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
    REQUIRE(device.draw(texture, whole(static_cast<float>(size))).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_texture(target.value());
    return std::move(image).value().pixels;
}

// RGBA8 texels drawn as drawn draws a texture.
std::vector<std::uint8_t> drawn_rgba(render::RenderDevice& device,
                                     const std::vector<std::uint8_t>& rgba, std::uint32_t size) {
    auto texture = device.create_texture(
        {.width = size, .height = size, .filter = Filter::Nearest, .render_target = false}, rgba);
    REQUIRE(texture.ok());
    std::vector<std::uint8_t> out = drawn(device, texture.value(), size);
    device.destroy_texture(texture.value());
    return out;
}

} // namespace

TEST_CASE("texture formats have names") {
    CHECK(render::texture_format_name(TextureFormat::Rgba8) == "rgba8");
    CHECK(render::texture_format_name(TextureFormat::Bc7) == "bc7");
    CHECK(render::texture_format_name(TextureFormat::Astc4x4) == "astc_4x4");
    CHECK(render::parse_texture_format("astc_4x4") == TextureFormat::Astc4x4);
    CHECK_FALSE(render::parse_texture_format("etc2").has_value());
    CHECK(render::texture_bytes({.width = 8,
                                 .height = 4,
                                 .filter = Filter::Linear,
                                 .render_target = false,
                                 .format = TextureFormat::Bc7}) == 32);
    CHECK(render::texture_bytes(
              {.width = 8, .height = 4, .filter = Filter::Linear, .render_target = false}) == 128);
}

TEST_CASE("BC7 and ASTC blocks decode as their formats specify") {
    for (const TextureFormat format : {TextureFormat::Bc7, TextureFormat::Astc4x4}) {
        CAPTURE(render::texture_format_name(format));
        auto pixels = render::decode_texels(format, 8, 8, quadrant_blocks(format, kQuadrants));
        REQUIRE(pixels.ok());
        CHECK(pixels.value() == quadrant_pixels(kQuadrants));

        const std::string name(render::texture_format_name(format));
        CHECK(error_of(render::decode_texels(format, 6, 4, std::vector<std::uint8_t>(24))) ==
              "a " + name +
                  " texture is whole 4x4 blocks, so its sides are multiples of 4, not 6x4");
        CHECK(error_of(render::decode_texels(format, 8, 8, std::vector<std::uint8_t>(63))) ==
              "a 8x8 texture takes 64 bytes of " + name + " blocks, not 63");
    }
    // A BC7 block in no mode decodes as transparent black, as GPUs decode it.
    auto invalid = render::decode_texels(TextureFormat::Bc7, 4, 4, std::vector<std::uint8_t>(16));
    REQUIRE(invalid.ok());
    CHECK(invalid.value() == std::vector<std::uint8_t>(64, 0));
    // RGBA8 is as it is.
    const std::vector<std::uint8_t> rgba = quadrant_pixels(kQuadrants);
    CHECK(render::decode_texels(TextureFormat::Rgba8, 8, 8, rgba).value() == rgba);
}

TEST_CASE("encode_texels compresses texels close to what they were, the same each time") {
    const std::vector<std::uint8_t> source = gradient();
    for (const TextureFormat format : {TextureFormat::Bc7, TextureFormat::Astc4x4}) {
        CAPTURE(render::texture_format_name(format));
        auto blocks = render::encode_texels(format, 16, 16, source);
        REQUIRE(blocks.ok());
        CHECK(blocks.value().size() == 256); // a quarter of RGBA8's 1024 bytes
        CHECK(render::encode_texels(format, 16, 16, source).value() == blocks.value());
        auto decoded = render::decode_texels(format, 16, 16, blocks.value());
        REQUIRE(decoded.ok());
        // Color and alpha changing every way at once in each block is hard for 4x4 blocks: BC7
        // stays within about 34 dB of it, ASTC 38.
        CHECK(largest_difference(decoded.value(), source) <= 16);
        CHECK(psnr(decoded.value(), source) >= 33.0);

        // One color a block comes back as it was.
        const std::vector<std::uint8_t> flat = quadrant_pixels(kQuadrants);
        auto flat_blocks = render::encode_texels(format, 8, 8, flat);
        REQUIRE(flat_blocks.ok());
        CHECK(largest_difference(render::decode_texels(format, 8, 8, flat_blocks.value()).value(),
                                 flat) <= 1);
    }
    CHECK(render::encode_texels(TextureFormat::Rgba8, 16, 16, source).value() == source);
    CHECK(
        error_of(render::encode_texels(TextureFormat::Bc7, 6, 4, std::vector<std::uint8_t>(96))) ==
        "a bc7 texture is whole 4x4 blocks, so its sides are multiples of 4, not 6x4");
    CHECK(error_of(
              render::encode_texels(TextureFormat::Astc4x4, 4, 4, std::vector<std::uint8_t>(16))) ==
          "a 4x4 texture takes 64 bytes of RGBA, not 16");
}

TEST_CASE("KTX2 files hold RGBA8, BC7 and ASTC textures, and other KTX2 files are refused") {
    for (const TextureFormat format : {TextureFormat::Bc7, TextureFormat::Astc4x4}) {
        CAPTURE(render::texture_format_name(format));
        Image image{.width = 8, .height = 8, .pixels = {}};
        image.block_format = format;
        image.blocks = quadrant_blocks(format, kQuadrants);
        const std::string file = ktx2_of(image);
        CHECK(render::is_ktx2(file));
        auto read = render::decode_image(file);
        REQUIRE(read.ok());
        CHECK(read.value().width == 8);
        CHECK(read.value().height == 8);
        CHECK(read.value().block_format == format);
        CHECK(read.value().blocks == image.blocks);
        CHECK(read.value().pixels == quadrant_pixels(kQuadrants));
        // Writing what was read gives the same file.
        CHECK(ktx2_of(read.value()) == file);
    }

    // RGBA8, from an image with no blocks.
    const Image plain{.width = 8, .height = 8, .pixels = quadrant_pixels(kQuadrants)};
    const std::string rgba8 = ktx2_of(plain);
    auto read = render::decode_image(rgba8);
    REQUIRE(read.ok());
    CHECK(read.value().block_format == TextureFormat::Rgba8);
    CHECK(read.value().blocks.empty());
    CHECK(read.value().pixels == plain.pixels);

    // The header names the Vulkan format, BC7's here, and the level starts on a whole block.
    Image bc7{.width = 8, .height = 8, .pixels = {}};
    bc7.block_format = TextureFormat::Bc7;
    bc7.blocks = quadrant_blocks(TextureFormat::Bc7, kQuadrants);
    const std::string file = ktx2_of(bc7);
    CHECK(static_cast<std::uint8_t>(file.at(12)) == 145);
    const auto level = static_cast<std::uint8_t>(file.at(80));
    CHECK(level % 16 == 0);
    CHECK(file.size() == level + 64U);

    const std::string refused = "not a KTX2 texture SlashEngine reads: ";
    CHECK(error_of(render::decode_image(file.substr(0, 90))) ==
          refused + "its header is cut short");
    std::string other = file;
    put32(other, 12, 147); // ETC2
    CHECK(error_of(render::decode_image(other)) ==
          refused + "its Vulkan format 147 is not RGBA8, BC7 or ASTC 4x4");
    other = file;
    put32(other, 32, 2); // two layers
    CHECK(error_of(render::decode_image(other)) ==
          refused + "it is a 3D texture, an array or a cube map, not one 2D texture");
    other = file;
    put32(other, 44, 2); // Zstandard
    CHECK(error_of(render::decode_image(other)) ==
          refused + "its levels are supercompressed (scheme 2)");
    other = file;
    put32(other, 88, 48);
    CHECK(error_of(render::decode_image(other)) ==
          refused + "its first level is 48 bytes, not the 64 a 8x8 bc7 texture takes");
    CHECK(error_of(render::decode_image(file.substr(0, file.size() - 1))) ==
          refused + "its first level runs past the end of the file");
    other = file;
    put32(other, 20, 6); // 6 wide
    CHECK(error_of(render::decode_image(other)) ==
          "a bc7 texture is whole 4x4 blocks, so its sides are multiples of 4, not 6x8");

    // Writing checks the blocks too.
    bc7.blocks.pop_back();
    CHECK(error_of(render::encode_ktx2(bc7)) ==
          "a 8x8 texture takes 64 bytes of bc7 blocks, not 63");
}

TEST_CASE("every device draws BC7 and ASTC textures as their decoded texels") {
    std::vector<std::unique_ptr<render::RenderDevice>> devices = slash::test::drawing_devices();
    const std::vector<std::uint8_t> source = gradient();
    for (const auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        CHECK(device->supports(TextureFormat::Rgba8));
        if (device->backend() == render::Backend::Software) {
            CHECK_FALSE(device->supports(TextureFormat::Bc7));
            CHECK_FALSE(device->supports(TextureFormat::Astc4x4));
        }
        for (const TextureFormat format : {TextureFormat::Bc7, TextureFormat::Astc4x4}) {
            CAPTURE(render::texture_format_name(format));
            CAPTURE(device->supports(format));
            const render::TextureDesc desc{.width = 8,
                                           .height = 8,
                                           .filter = Filter::Nearest,
                                           .render_target = false,
                                           .format = format};
            const std::uint64_t before = device->stats().texture_bytes;
            auto texture = device->create_texture(desc, quadrant_blocks(format, kQuadrants));
            REQUIRE(texture.ok());
            // A quarter of RGBA8's bytes where the device samples the format, else decoded.
            CHECK(device->stats().texture_bytes - before ==
                  (device->supports(format) ? 64U : 256U));
            CHECK(largest_difference(drawn(*device, texture.value(), 8),
                                     drawn_rgba(*device, quadrant_pixels(kQuadrants), 8)) <= 1);

            // An update takes the same format.
            const std::array<Rgba, 4> swapped{kQuadrants[3], kQuadrants[2], kQuadrants[1],
                                              kQuadrants[0]};
            REQUIRE(device->update_texture(texture.value(), quadrant_blocks(format, swapped)).ok());
            CHECK(largest_difference(drawn(*device, texture.value(), 8),
                                     drawn_rgba(*device, quadrant_pixels(swapped), 8)) <= 1);
            CHECK(error_of(device->update_texture(texture.value(), quadrant_pixels(swapped))) ==
                  "a 8x8 texture takes 64 bytes of " +
                      std::string(render::texture_format_name(format)) + " blocks, not 256");
            device->destroy_texture(texture.value());

            // Encoded texels draw as decode_texels decodes them, whoever decodes them.
            auto blocks = render::encode_texels(format, 16, 16, source);
            REQUIRE(blocks.ok());
            auto encoded = device->create_texture({.width = 16,
                                                   .height = 16,
                                                   .filter = Filter::Nearest,
                                                   .render_target = false,
                                                   .format = format},
                                                  blocks.value());
            REQUIRE(encoded.ok());
            const std::vector<std::uint8_t> decoded =
                render::decode_texels(format, 16, 16, blocks.value()).value();
            CHECK(largest_difference(drawn(*device, encoded.value(), 16),
                                     drawn_rgba(*device, decoded, 16)) <= 1);
            device->destroy_texture(encoded.value());
        }
    }
    // The null device stands in for any GPU, so it samples every format.
    auto null = render::create_null_device();
    CHECK(null->supports(TextureFormat::Bc7));
    CHECK(null->supports(TextureFormat::Astc4x4));
}
