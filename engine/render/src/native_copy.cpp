#include "native_copy.hpp"

#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace slash::render::detail {

Result<void> copy_native_vulkan(const NativeDevice& device, void* commands,
                                const NativeTexture& source, const NativeTexture& target) {
    const auto instance_calls =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (instance_calls == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "Vulkan's loader isn't loaded");
    }
    auto* const instance = static_cast<VkInstance>(device.instance);
    auto* const logical = static_cast<VkDevice>(device.device);
    const auto device_calls =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance_calls(instance, "vkGetDeviceProcAddr"));
    if (device_calls == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "Vulkan has no vkGetDeviceProcAddr");
    }
    const auto barrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(device_calls(logical, "vkCmdPipelineBarrier"));
    const auto copy = reinterpret_cast<PFN_vkCmdCopyImage>(device_calls(logical, "vkCmdCopyImage"));
    if (barrier == nullptr || copy == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "Vulkan's copy commands are missing");
    }
    auto* const list = static_cast<VkCommandBuffer>(commands);
    // A pointer on 64-bit machines, a 64-bit number on others.
    const auto from = reinterpret_cast<VkImage>(source.image); // NOLINT(*-qualified-auto)
    const auto to = reinterpret_cast<VkImage>(target.image);   // NOLINT(*-qualified-auto)
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto move = [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags before, VkAccessFlags after) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = before;
        b.dstAccessMask = after;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = whole;
        barrier(list, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                nullptr, 0, nullptr, 1, &b);
    };
    // A render target rests where shaders sample it; a swapchain image where it is drawn to.
    move(from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    move(to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {source.width, source.height, 1};
    copy(list, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         1, &region);
    move(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    move(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    return {};
}

Result<void> copy_native_d3d12([[maybe_unused]] void* commands,
                               [[maybe_unused]] const NativeTexture& source,
                               [[maybe_unused]] const NativeTexture& target) {
#if defined(_WIN32)
    auto* const list = static_cast<ID3D12GraphicsCommandList*>(commands);
    auto* const from = static_cast<ID3D12Resource*>(source.image);
    auto* const to = static_cast<ID3D12Resource*>(target.image);
    const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    };
    // A render target rests where shaders sample it; a swapchain image where it is drawn to.
    move(from, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    move(to, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = to;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = from;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, source.width, source.height, 1};
    list->CopyTextureRegion(&destination, 0, 0, 0, &origin, &box);
    move(from, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    move(to, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return {};
#else
    return make_error(ErrorCode::PlatformFailure, "Direct3D 12 is only on Windows");
#endif
}

} // namespace slash::render::detail
