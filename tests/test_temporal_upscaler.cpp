#include "../engine/render/src/fsr2.hpp"
#include "upscale_square.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/upscaler.hpp"

#include <SDL3/SDL_vulkan.h>
#include <doctest/doctest.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::TemporalFrame;
using namespace slash::test::upscale;

namespace {

// What the test upscalers saw.
struct Seen {
    std::vector<TemporalFrame> frames;
    std::vector<std::vector<float>> motion;
    std::vector<std::vector<float>> depth;
};

Seen& seen() {
    static Seen value;
    return value;
}

// On the software device, writes each output pixel from the color's nearest render pixel.
class Nearest final : public render::TemporalUpscaler {
public:
    slash::Result<void> upscale(const TemporalFrame& frame) override {
        seen().frames.push_back(frame);
        seen().motion.emplace_back(frame.motion.pixels.begin(), frame.motion.pixels.end());
        seen().depth.emplace_back(frame.depth.pixels.begin(), frame.depth.pixels.end());
        if (frame.device.backend != render::Backend::Software) {
            return slash::make_error(slash::ErrorCode::InvalidArgument, "only on the CPU");
        }
        for (std::uint32_t y = 0; y < frame.output.height; ++y) {
            for (std::uint32_t x = 0; x < frame.output.width; ++x) {
                const std::uint32_t sx = x * frame.color.width / frame.output.width;
                const std::uint32_t sy = y * frame.color.height / frame.output.height;
                for (std::size_t c = 0; c < 4; ++c) {
                    frame.output.pixels[((std::size_t{y} * frame.output.width + x) * 4) + c] =
                        frame.color.pixels[((std::size_t{sy} * frame.color.width + sx) * 4) + c];
                }
            }
        }
        return {};
    }
};

// Copies the color into the output's top left corner with the native API's own calls, as a
// vendor's library works on the device's objects: Vulkan's and Direct3D 12's, and the software
// device's floats.
class Corner final : public render::TemporalUpscaler {
public:
    slash::Result<void> upscale(const TemporalFrame& frame) override {
        seen().frames.push_back(frame);
        switch (frame.device.backend) {
        case render::Backend::Software:
            for (std::uint32_t y = 0; y < frame.color.height; ++y) {
                std::copy_n(
                    frame.color.pixels.begin() +
                        static_cast<std::ptrdiff_t>(std::size_t{y} * frame.color.width * 4),
                    std::size_t{frame.color.width} * 4,
                    frame.output.pixels.begin() +
                        static_cast<std::ptrdiff_t>(std::size_t{y} * frame.output.width * 4));
            }
            return {};
        case render::Backend::Vulkan:
            return vulkan(frame);
#ifdef _WIN32
        case render::Backend::D3D12:
            return d3d12(frame);
#endif
        default:
            return slash::make_error(slash::ErrorCode::InvalidArgument, "not on this backend");
        }
    }

private:
    static slash::Result<void> vulkan(const TemporalFrame& frame) {
        const auto instance_calls =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (instance_calls == nullptr) {
            return slash::make_error(slash::ErrorCode::PlatformFailure, "no Vulkan loader");
        }
        auto* const instance = static_cast<VkInstance>(frame.device.instance);
        auto* const device = static_cast<VkDevice>(frame.device.device);
        const auto device_calls = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            instance_calls(instance, "vkGetDeviceProcAddr"));
        const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
            device_calls(device, "vkCmdPipelineBarrier"));
        const auto clear = reinterpret_cast<PFN_vkCmdClearColorImage>(
            device_calls(device, "vkCmdClearColorImage"));
        const auto copy =
            reinterpret_cast<PFN_vkCmdCopyImage>(device_calls(device, "vkCmdCopyImage"));
        auto* const commands = static_cast<VkCommandBuffer>(frame.commands);
        // A pointer on 64-bit machines, a 64-bit number on others.
        const auto color = reinterpret_cast<VkImage>(frame.color.image);
        const auto output = reinterpret_cast<VkImage>(frame.output.image);
        const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto move = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                              VkAccessFlags before, VkAccessFlags after) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = before;
            b.dstAccessMask = after;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = whole;
            barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        move(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        move(output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{};
        clear(commands, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
        move(output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {frame.color.width, frame.color.height, 1};
        copy(commands, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        move(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
        move(output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        return {};
    }

#ifdef _WIN32
    static slash::Result<void> d3d12(const TemporalFrame& frame) {
        auto* const list = static_cast<ID3D12GraphicsCommandList*>(frame.commands);
        auto* const color = static_cast<ID3D12Resource*>(frame.color.image);
        auto* const output = static_cast<ID3D12Resource*>(frame.output.image);
        const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                              D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            list->ResourceBarrier(1, &b);
        };
        move(color, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        move(output, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = output;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = color;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, frame.color.width, frame.color.height, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        move(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        move(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        return {};
    }
#endif
};

void add(const char* name, bool corner) {
    REQUIRE(render::add_upscaler({.name = name,
                                  .temporal = true,
                                  .supports = {},
                                  .make = {},
                                  .make_temporal =
                                      [corner] {
                                          return corner ? std::unique_ptr<render::TemporalUpscaler>(
                                                              std::make_unique<Corner>())
                                                        : std::unique_ptr<render::TemporalUpscaler>(
                                                              std::make_unique<Nearest>());
                                      }})
                .ok());
}

render::Scaling scaling(float scale, const char* upscaler) {
    return {.mode = "bilinear", .scale = scale, .fsr_sharpness = 0.2F, .upscaler = upscaler};
}

// The top left corner of image, side pixels square.
Image corner(const Image& image, std::uint32_t side) {
    Image out{.width = side, .height = side, .pixels = {}};
    for (std::uint32_t y = 0; y < side; ++y) {
        const auto row =
            image.pixels.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * image.width * 4);
        out.pixels.insert(out.pixels.end(), row, row + static_cast<std::ptrdiff_t>(side * 4));
    }
    return out;
}

} // namespace

TEST_CASE("a game's temporal upscaler runs in place of FSR 2 where a frame would run it") {
    render::clear_added_upscalers();
    add("nearest", false);
    const auto software = render::create_software_device();
    const auto null = render::create_null_device();
    using render::RenderMethod;
    CHECK(render::temporal_upscaler_on(scaling(0.5F, "nearest"), RenderMethod::ForwardPlus,
                                       *software) == "nearest");
    CHECK(render::uses_fsr2(scaling(0.5F, "nearest"), RenderMethod::ForwardPlus, *software));
    // Not at a scale of 1, in Compatibility or on a device without native objects or the CPU's.
    CHECK(
        render::temporal_upscaler_on(scaling(1.0F, "nearest"), RenderMethod::ForwardPlus, *software)
            .empty());
    CHECK(render::temporal_upscaler_on(scaling(0.5F, "nearest"), RenderMethod::Compatibility,
                                       *software)
              .empty());
    CHECK(render::temporal_upscaler_on(scaling(0.5F, "nearest"), RenderMethod::ForwardPlus, *null)
              .empty());
    CHECK_FALSE(render::uses_fsr2(scaling(0.5F, "nearest"), RenderMethod::ForwardPlus, *null));
    // Where it doesn't run, the mode scales the world.
    CHECK(render::choose_upscaler(scaling(0.5F, "nearest"), *null) == "bilinear");
    CHECK(render::upscaler_names() ==
          std::vector<std::string>{"bilinear", "fsr", "metalfx_spatial", "nearest"});

    // One whose supports says no doesn't run either.
    REQUIRE(render::add_upscaler({.name = "vendor",
                                  .temporal = true,
                                  .supports = [](const render::RenderDevice&) { return false; },
                                  .make = {},
                                  .make_temporal =
                                      [] {
                                          return std::unique_ptr<render::TemporalUpscaler>(
                                              std::make_unique<Nearest>());
                                      }})
                .ok());
    CHECK(
        render::temporal_upscaler_on(scaling(0.5F, "vendor"), RenderMethod::ForwardPlus, *software)
            .empty());
    render::clear_added_upscalers();
}

TEST_CASE("the software device hands a temporal upscaler its frame and shows what it writes") {
    render::clear_added_upscalers();
    add("nearest", false);
    seen() = {};
    const auto software = render::create_software_device();
    Square settings;
    settings.fsr2->upscaler = "nearest";
    settings.frames = 3;
    settings.sky = true;
    settings.perspective = true;
    settings.yaw = 0.03F;
    const Image drawn = draw_square(*software, settings);

    REQUIRE(seen().frames.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        const TemporalFrame& frame = seen().frames[i];
        CHECK(frame.device.backend == render::Backend::Software);
        CHECK(frame.commands == nullptr);
        CHECK(frame.size == render::UpscaleSize{.render_width = 16,
                                                .render_height = 16,
                                                .output_width = 32,
                                                .output_height = 32});
        CHECK(frame.reset == (i == 0));
        // The jitter in render pixels, as FSR 2 takes it.
        const std::array<float, 2> clip =
            render::detail::camera_jitter(i, render::detail::fsr2_phase_count(16, 32), 16, 16);
        CHECK(frame.jitter[0] == doctest::Approx(clip[0] * 8.0F));
        CHECK(frame.jitter[1] == doctest::Approx(-clip[1] * 8.0F));
        CHECK(frame.sharpness == doctest::Approx(0.2F));
        // The sky's motion vectors are worked out from the depth: none is left invalid.
        REQUIRE(seen().motion[i].size() == 16 * 16 * 2);
        CHECK(std::ranges::none_of(seen().motion[i], [](float v) {
            return v <= render::detail::kInvalidMotion || !std::isfinite(v);
        }));
        // The square is nearer than the sky.
        CHECK(std::ranges::any_of(seen().depth[i], [](float d) { return d > 0.0F; }));
        CHECK(std::ranges::any_of(seen().depth[i], [](float d) { return d == 0.0F; }));
    }
    // The camera turns, so the sky moves.
    CHECK(std::ranges::any_of(seen().motion[1], [](float v) { return v != 0.0F; }));
    // What it wrote shows: the square's middle is white, nearest pixels come in pairs.
    CHECK(red(drawn, 16, 16) == 255);
    for (std::uint32_t y = 0; y < 32; y += 2) {
        for (std::uint32_t x = 0; x < 32; x += 2) {
            CHECK(red(drawn, x, y) == red(drawn, x + 1, y + 1));
        }
    }
    render::clear_added_upscalers();
}

TEST_CASE("GPU devices hand a temporal upscaler their native objects") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    if (gpu->backend() != render::Backend::Vulkan && gpu->backend() != render::Backend::D3D12) {
        MESSAGE("the test copies with Vulkan's and Direct3D 12's calls only; skipped on "
                << render::backend_name(gpu->backend()));
        return;
    }
    render::clear_added_upscalers();
    add("corner", true);
    const auto software = render::create_software_device();
    Square settings;
    settings.fsr2->upscaler = "corner";
    settings.frames = 2;
    seen() = {};
    const Image expected = draw_square(*software, settings);
    const Image drawn = draw_square(*gpu, settings);
    REQUIRE(seen().frames.size() == 4);
    const TemporalFrame& frame = seen().frames.back();
    CHECK(frame.device.backend == gpu->backend());
    CHECK(frame.device.device != nullptr);
    CHECK(frame.device.queue != nullptr);
    CHECK(frame.commands != nullptr);
    for (const render::NativeTexture* texture :
         {&frame.color, &frame.depth, &frame.motion, &frame.output}) {
        CHECK(texture->image != nullptr);
        CHECK(texture->format != 0);
        CHECK(texture->pixels.empty());
    }
    CHECK(frame.color.width == 16);
    CHECK(frame.output.width == 32);
    if (gpu->backend() == render::Backend::Vulkan) {
        CHECK(frame.device.instance != nullptr);
        CHECK(frame.device.physical_device != nullptr);
        CHECK(frame.color.view != nullptr);
        CHECK(frame.color.format == VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    // The world drawn at the render size lands in the top left corner, as on the CPU.
    const Image want = corner(expected, 16);
    const Image got = corner(drawn, 16);
    CHECK(std::ranges::any_of(got.pixels, [](std::uint8_t v) { return v == 255; }));
    CHECK(differing(got, want, 3) <= got.pixels.size() / 50);
    CHECK(difference(got, want) <= 24);
    render::clear_added_upscalers();
}
