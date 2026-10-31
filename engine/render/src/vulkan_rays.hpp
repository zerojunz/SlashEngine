#pragma once

#include "ray_shadows.hpp"

#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Ray tracing on the GPU device's Vulkan backend, which SDL3's GPU API has none of: the device
// is made with VK_KHR_acceleration_structure and VK_KHR_ray_query where they are
// (vulkan_ray_options), and the frame's rays are recorded into SDL's command buffer with Vulkan's
// own calls, as the temporal upscalers are. Each mesh a frame draws gets a bottom-level
// acceleration structure built from a copy of its SDL buffers, kept until the mesh changes, and
// each frame a top-level one with an instance a draw, placed by its transform; then
// shaders/ray_shadows.comp traces the shadows into two textures the GPU device copies into the
// screen texture's layers 2 and 3. The software device's twin is src/ray_shadows.cpp.
namespace slash::render::detail {

// What SDL_GPUVulkanOptions points at to make a device with ray queries: Vulkan 1.2's buffer
// device addresses, then the extensions' features, as cmake/patches/sdl-ray-tracing.cmake
// chains them.
struct VulkanRayOptions {
    VulkanRayOptions();
    VulkanRayOptions(const VulkanRayOptions&) = delete;
    VulkanRayOptions& operator=(const VulkanRayOptions&) = delete;
    VulkanRayOptions(VulkanRayOptions&&) = delete;
    VulkanRayOptions& operator=(VulkanRayOptions&&) = delete;
    ~VulkanRayOptions();

    // An SDL_GPUVulkanOptions, for SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER.
    [[nodiscard]] void* options() const;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};

// A draw as rays meet it: its mesh's SDL buffers and the triangles it draws, placed by transform.
struct RayDraw {
    std::uint64_t mesh = 0;    // which mesh: the same mesh, version and triangles keep their
    std::uint64_t version = 0; // structure from frame to frame
    SDL_GPUBuffer* vertices = nullptr; // a Vertex3D a vertex, its position first
    SDL_GPUBuffer* indices = nullptr;  // 32-bit indices
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0; // the triangles it draws: its LOD's indices
    std::uint32_t index_count = 0;
    Mat4 transform{};
    std::uint32_t layers = 0; // its render layers, which lights' caster masks pick
};

// Where a frame's ray-traced shadows come from and go: the prepass's depth and normals, and the
// two RGBA16F textures of visibilities (lights 0 to 3, then 4 to 7), sampled and written by
// compute, at the frame's render size.
struct RayShadowTargets {
    SDL_GPUTexture* depth = nullptr;
    SDL_GPUTexture* normals = nullptr;
    std::array<SDL_GPUTexture*, 2> visibility{};
};

// The GPU device's rays on Vulkan.
class VulkanRays {
public:
    VulkanRays() = default;
    VulkanRays(const VulkanRays&) = delete;
    VulkanRays& operator=(const VulkanRays&) = delete;
    VulkanRays(VulkanRays&&) = delete;
    VulkanRays& operator=(VulkanRays&&) = delete;
    virtual ~VulkanRays() = default;

    // Records the frame's ray-traced shadows into commands, between SDL's passes: the
    // structures of draws (those that cast shadows) and the shader writing targets' visibilities
    // of lights, as run_ray_shadows does on the CPU.
    virtual Result<void> trace_shadows(SDL_GPUCommandBuffer* commands,
                                       std::span<const RayDraw> draws,
                                       std::span<const RayShadowLight> lights,
                                       const RayShadowFrame& frame,
                                       const RayShadowTargets& targets) = 0;
};

// The rays of a device made with VulkanRayOptions, or null where it has no ray queries (another
// backend, or a device made without them).
[[nodiscard]] std::unique_ptr<VulkanRays> make_vulkan_rays(SDL_GPUDevice* device);

} // namespace slash::render::detail
