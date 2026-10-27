#pragma once

#include "ray_shadows.hpp"

#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Ray tracing on the GPU device's Vulkan backend, which SDL3's GPU API has none of: the device
// is made with VK_KHR_acceleration_structure and VK_KHR_ray_query where they are
// (vulkan_ray_options), and the frame's rays are recorded into SDL's command buffer with Vulkan's
// own calls, as the temporal upscalers are. Each mesh a frame draws gets a bottom-level
// acceleration structure built from a copy of its SDL buffers, kept until the mesh changes, and
// each frame a top-level one with an instance a draw, placed by its transform; then
// shaders/ray_shadows.comp traces the shadows into two textures the GPU device copies into the
// screen texture's layers 2 and 3, and shaders/ray_light.comp the reflections, shading what they
// meet from the draws' materials, the copies of their vertices and their textures, into one the
// GPU device copies into layer 1. The software device's twin is src/ray_shadows.cpp.
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

// A texture a draw's rays sample: none reads white.
struct RayTexture {
    SDL_GPUTexture* texture = nullptr;
    bool nearest = false; // Filter::Nearest
    bool repeat = false;
};

// A draw as rays meet it: its mesh's SDL buffers and the triangles it draws, placed by transform,
// and how what meets it is shaded.
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
    // Rays from the camera's side see it unless it only casts shadows; shadow rays meet it
    // unless it casts none.
    ShadowCasting shadows = ShadowCasting::On;
    CullMode cull = CullMode::Back;
    MeshSurfaceUniforms surface{};
    Mat4 normal_model{};                  // its normals' matrix, in the upper 3x3
    Float4 uv1{};                         // scale, then offset
    std::array<RayTexture, 5> textures{}; // albedo, metallic, roughness, emission, normal
};

// What lights the surfaces reflection rays meet: the frame's lights, and the environment's light,
// the ambient light or with sky the sky's radiance cube (RayTargets::environment, sRGB as the
// mesh pass samples it) times sky_energy, over the ambient light by sky_share.
struct RayLighting {
    std::vector<PathLight> lights;
    Float3 ambient{};
    bool sky = false;
    std::array<Float3, 3> to_sky{}; // the world to the sky's space, a row each
    float sky_energy = 1.0F;
    float sky_share = 1.0F;
};

// What a frame's rays trace: the shadows of lights with a ray slot, the ambient occlusion and the
// reflections, which lighting lights.
struct RayWork {
    std::span<const RayShadowLight> lights;
    std::optional<RayOcclusion> occlusion;
    std::optional<RayReflection> reflection;
    const RayLighting* lighting = nullptr; // with reflection
};

// Where a frame's rays come from and go: the prepass's depth and normals, and four RGBA16F
// textures, sampled and written by compute, at the frame's render size: the visibilities of
// lights 0 to 3, then 4 to 7, then the ambient occlusion in red, then the reflections. With
// reflections, a white texture and a cube (the sky's radiance, or any) they sample.
struct RayTargets {
    SDL_GPUTexture* depth = nullptr;
    SDL_GPUTexture* normals = nullptr;
    std::array<SDL_GPUTexture*, 4> outputs{};
    SDL_GPUTexture* white = nullptr;
    SDL_GPUTexture* environment = nullptr;
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

    // Records the frame's rays into commands, between SDL's passes: the structures of draws, the
    // shader writing targets' visibilities of lights, as run_ray_shadows does on the CPU, and the
    // ambient occlusion, as run_ray_occlusion does, then the one writing the reflections, as
    // run_ray_reflections does.
    virtual Result<void> trace(SDL_GPUCommandBuffer* commands, std::span<const RayDraw> draws,
                               const RayWork& work, const RayShadowFrame& frame,
                               const RayTargets& targets) = 0;
};

// The rays of a device made with VulkanRayOptions, or null where it has no ray queries (another
// backend, or a device made without them).
[[nodiscard]] std::unique_ptr<VulkanRays> make_vulkan_rays(SDL_GPUDevice* device);

} // namespace slash::render::detail
