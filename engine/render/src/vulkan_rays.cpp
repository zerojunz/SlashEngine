#include "vulkan_rays.hpp"

#if !defined(__EMSCRIPTEN__)

#include "lightmap_light_shaders.hpp"
#include "ray_light_shaders.hpp"
#include "ray_shadows_shaders.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vulkan/vulkan_core.h>

// What cmake/patches/sdl-native-handles.cmake and sdl-ray-tracing.cmake add to SDL3's GPU API.
extern "C" {
bool SDL_SlashGPUNativeDevice(SDL_GPUDevice* device, void** instance, void** physical_device,
                              void** logical_device, void** queue, Uint32* queue_family);
void* SDL_SlashGPUNativeTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, void** view,
                                Uint32* format);
void* SDL_SlashGPUNativeCommandBuffer(SDL_GPUCommandBuffer* commands);
void* SDL_SlashGPUNativeBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer);
}

namespace slash::render::detail {

namespace {

constexpr std::uint32_t kGroupSize = 8;
// Frames whose rays can be in flight at once, each with its own descriptors, uniforms and
// top-level structure; a frame waits for the GPU when all are busy.
constexpr std::size_t kSlots = 4;
// The textures the shadows' shader writes: two of lights' visibilities, then the ambient
// occlusion.
constexpr std::size_t kOutputs = 3;
// The passes of the light's shader (ray_light.comp's kPass): the reflections, the global
// illumination's rays and their smoothing, the paths and their average.
constexpr std::size_t kLightPasses = 5;
constexpr std::size_t kTracePaths = 3;
constexpr std::size_t kAveragePaths = 4;
// The textures the light's shader writes: the reflections (or the paths' average), then the
// ambient light as traced and as smoothed.
constexpr std::size_t kLightOutputs = 3;
// The most textures the light's shader samples (its textures array); the rest read white.
constexpr std::uint32_t kRayTextures = 64;
constexpr std::uint32_t kNoTexture = 0xFFFFFFFFU;
// Instance masks: the draws rays from the camera's side see, and those that cast shadows.
constexpr std::uint8_t kSeeMask = 1;
constexpr std::uint8_t kCastMask = 2;

// The uniforms of shaders/ray_shadows.comp's Frame, in std140.
struct RayLightUniforms {
    std::array<float, 4> place{};
    std::array<float, 4> params{};
    std::array<std::uint32_t, 4> mask{};
};
struct RayShadowsUniforms {
    std::array<float, 16> to_world{};
    std::array<std::array<float, 4>, 3> view_rows{};
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> options{};
    std::array<float, 4> occlusion{}; // radius, power, samples, then intensity
    std::array<RayLightUniforms, kMaxRayShadowLights> lights{};
};
static_assert(sizeof(RayShadowsUniforms) == 64 + 48 + 16 + 16 + 16 + (48 * kMaxRayShadowLights));

// shaders/ray_light.comp's Frame, in std140.
struct RayLightFrame {
    std::array<float, 16> to_world{};
    std::array<std::array<float, 4>, 3> view_rows{};
    std::array<std::uint32_t, 4> size{};
    Float4 ambient{};
    std::array<Float4, 3> sky_rows{};
    std::array<std::uint32_t, 4> gi{};
    std::array<std::uint32_t, 4> paths{}; // the first path's number, paths a pixel, bounces
};
static_assert(sizeof(RayLightFrame) == 64 + 48 + 16 + 16 + 48 + 16 + 16);

// Its Draw and Light, in std430.
struct RayDrawRecord {
    Float4 albedo{};
    Float4 emission{};
    Float4 surface{};
    Float4 metallic_channel{};
    Float4 roughness_channel{};
    std::array<std::uint32_t, 4> options{};
    std::array<Float4, 3> normal_rows{};
    Float4 uv1{};
    std::array<std::uint32_t, 4> buffers{};
    std::array<std::uint32_t, 4> textures{};
    std::array<std::uint32_t, 4> more{};
};
static_assert(sizeof(RayDrawRecord) == std::size_t{13} * 16);
struct RayLightRecord {
    Float4 position{};
    Float4 direction{};
    Float4 color{};
    Float4 params{};
    std::array<std::uint32_t, 4> masks{};
    Float4 shadow{};
};
static_assert(sizeof(RayLightRecord) == std::size_t{6} * 16);

// shaders/lightmap_light.comp's passes (its kPass): the direct light, a pass of bounces, a pass
// of the probes' rays.
constexpr std::size_t kBakePasses = 3;
constexpr std::uint32_t kBakeGroupSize = 64;
// Godot's rendering/lightmapping/bake_performance/region_size, squared: the texels a dispatch
// lights at most; max_rays_per_pass and max_rays_per_probe_pass, the rays.
constexpr std::uint32_t kBakeRegionTexels = 512U * 512U;
constexpr std::uint32_t kBakeRaysPerPass = 32;
constexpr std::uint32_t kBakeRaysPerProbePass = 64;

// Its Params and Light, in std430.
struct LightmapParams {
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> counts{};
    std::array<std::uint32_t, 4> step{};
    std::array<std::uint32_t, 4> flags{};
    std::array<float, 4> values{};
    std::array<float, 4> more{};
    std::array<std::array<float, 4>, 3> space{};
};
static_assert(sizeof(LightmapParams) == std::size_t{9} * 16);
struct LightmapLightRecord {
    std::array<float, 4> position{};
    std::array<float, 4> direction{};
    std::array<float, 4> color{};
    std::array<float, 4> shape{};
    std::array<float, 4> more{};
};
static_assert(sizeof(LightmapLightRecord) == std::size_t{5} * 16);

// Vulkan's calls this needs, from the device SDL made.
struct Calls {
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements buffer_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkGetBufferDeviceAddress buffer_address = nullptr;
    PFN_vkCreateAccelerationStructureKHR create_structure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_structure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR structure_address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build_structures = nullptr;
    PFN_vkCmdCopyBuffer copy_buffer = nullptr;
    PFN_vkCmdUpdateBuffer update_buffer = nullptr;
    PFN_vkCmdFillBuffer fill_buffer = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCreateShaderModule create_shader = nullptr;
    PFN_vkDestroyShaderModule destroy_shader = nullptr;
    PFN_vkCreateDescriptorSetLayout create_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkCreateComputePipelines create_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateDescriptorPool create_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_pool = nullptr;
    PFN_vkAllocateDescriptorSets allocate_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_sets = nullptr;
    PFN_vkCmdBindPipeline bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets bind_sets = nullptr;
    PFN_vkCmdDispatch dispatch = nullptr;
    PFN_vkCreateSampler create_sampler = nullptr;
    PFN_vkDestroySampler destroy_sampler = nullptr;
    PFN_vkCreateEvent create_event = nullptr;
    PFN_vkDestroyEvent destroy_event = nullptr;
    PFN_vkGetEventStatus event_status = nullptr;
    PFN_vkResetEvent reset_event = nullptr;
    PFN_vkCmdSetEvent set_event = nullptr;
};

// Loads calls through SDL's Vulkan loader; false when one is missing, as on a device made
// without ray queries.
bool load_calls(VkInstance instance, VkDevice device, Calls& calls) {
    const auto instance_calls =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (instance_calls == nullptr) {
        return false;
    }
    const auto device_calls =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance_calls(instance, "vkGetDeviceProcAddr"));
    if (device_calls == nullptr) {
        return false;
    }
    bool all = true;
    const auto from_instance = [&](auto& call, const char* name) {
        call = reinterpret_cast<std::remove_reference_t<decltype(call)>>(
            instance_calls(instance, name));
        all = all && call != nullptr;
    };
    const auto from_device = [&](auto& call, const char* name) {
        call =
            reinterpret_cast<std::remove_reference_t<decltype(call)>>(device_calls(device, name));
        all = all && call != nullptr;
    };
    from_instance(calls.get_memory_properties, "vkGetPhysicalDeviceMemoryProperties");
    from_instance(calls.get_properties2, "vkGetPhysicalDeviceProperties2");
    from_device(calls.create_buffer, "vkCreateBuffer");
    from_device(calls.destroy_buffer, "vkDestroyBuffer");
    from_device(calls.buffer_requirements, "vkGetBufferMemoryRequirements");
    from_device(calls.allocate_memory, "vkAllocateMemory");
    from_device(calls.free_memory, "vkFreeMemory");
    from_device(calls.bind_buffer_memory, "vkBindBufferMemory");
    from_device(calls.map_memory, "vkMapMemory");
    from_device(calls.buffer_address, "vkGetBufferDeviceAddress");
    from_device(calls.create_structure, "vkCreateAccelerationStructureKHR");
    from_device(calls.destroy_structure, "vkDestroyAccelerationStructureKHR");
    from_device(calls.build_sizes, "vkGetAccelerationStructureBuildSizesKHR");
    from_device(calls.structure_address, "vkGetAccelerationStructureDeviceAddressKHR");
    from_device(calls.build_structures, "vkCmdBuildAccelerationStructuresKHR");
    from_device(calls.copy_buffer, "vkCmdCopyBuffer");
    from_device(calls.update_buffer, "vkCmdUpdateBuffer");
    from_device(calls.fill_buffer, "vkCmdFillBuffer");
    from_device(calls.barrier, "vkCmdPipelineBarrier");
    from_device(calls.create_shader, "vkCreateShaderModule");
    from_device(calls.destroy_shader, "vkDestroyShaderModule");
    from_device(calls.create_set_layout, "vkCreateDescriptorSetLayout");
    from_device(calls.destroy_set_layout, "vkDestroyDescriptorSetLayout");
    from_device(calls.create_pipeline_layout, "vkCreatePipelineLayout");
    from_device(calls.destroy_pipeline_layout, "vkDestroyPipelineLayout");
    from_device(calls.create_pipelines, "vkCreateComputePipelines");
    from_device(calls.destroy_pipeline, "vkDestroyPipeline");
    from_device(calls.create_pool, "vkCreateDescriptorPool");
    from_device(calls.destroy_pool, "vkDestroyDescriptorPool");
    from_device(calls.allocate_sets, "vkAllocateDescriptorSets");
    from_device(calls.update_sets, "vkUpdateDescriptorSets");
    from_device(calls.bind_pipeline, "vkCmdBindPipeline");
    from_device(calls.bind_sets, "vkCmdBindDescriptorSets");
    from_device(calls.dispatch, "vkCmdDispatch");
    from_device(calls.create_sampler, "vkCreateSampler");
    from_device(calls.destroy_sampler, "vkDestroySampler");
    from_device(calls.create_event, "vkCreateEvent");
    from_device(calls.destroy_event, "vkDestroyEvent");
    from_device(calls.event_status, "vkGetEventStatus");
    from_device(calls.reset_event, "vkResetEvent");
    from_device(calls.set_event, "vkCmdSetEvent");
    return all;
}

// A Vulkan handle from SDL's pointer: a pointer for dispatchable handles, a 64-bit number for
// the rest on 32-bit platforms.
template <typename Handle>
Handle handle_of(void* pointer) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<Handle>(pointer);
    } else {
        return static_cast<Handle>(reinterpret_cast<std::uintptr_t>(pointer));
    }
}

Error vulkan_error(const std::string& what, VkResult result) {
    return Error{ErrorCode::PlatformFailure,
                 what + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")"};
}

// A buffer with memory of its own.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr; // host-visible buffers, mapped for good
};

// An acceleration structure and the buffer it lives in.
struct Structure {
    VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};

// A mesh's bottom-level structure, with the copies of its triangles it was built from.
struct MeshStructure {
    std::uint64_t version = 0;
    Structure built;
    Buffer vertices;
    Buffer indices;
    std::uint64_t used = 0; // the frame that last used it
};

// What one frame's rays use that the next may not change while they run.
struct Slot {
    VkEvent done = VK_NULL_HANDLE; // set when its shader has run
    bool recorded = false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buffer uniforms;  // host-visible
    Buffer layers;    // host-visible, a uint a draw
    Buffer instances; // host-visible, a VkAccelerationStructureInstanceKHR a draw
    Buffer scratch;
    // The light's shader's.
    VkDescriptorSet light_set = VK_NULL_HANDLE;
    Buffer light_frame;   // host-visible
    Buffer draw_records;  // host-visible, a RayDrawRecord a draw
    Buffer light_records; // host-visible, a RayLightRecord a light
    Structure top;
    VkDeviceSize top_size = 0;
};

class VulkanRaysImpl final : public VulkanRays {
public:
    VulkanRaysImpl(SDL_GPUDevice* sdl, VkInstance instance, VkPhysicalDevice physical,
                   VkDevice device, const Calls& calls)
        : sdl_(sdl), physical_(physical), device_(device), calls_(calls) {
        (void)instance;
    }

    VulkanRaysImpl(const VulkanRaysImpl&) = delete;
    VulkanRaysImpl& operator=(const VulkanRaysImpl&) = delete;
    VulkanRaysImpl(VulkanRaysImpl&&) = delete;
    VulkanRaysImpl& operator=(VulkanRaysImpl&&) = delete;

    ~VulkanRaysImpl() override {
        SDL_WaitForGPUIdle(sdl_);
        for (auto& [key, mesh] : meshes_) {
            release(mesh);
        }
        for (MeshStructure& mesh : retired_) {
            release(mesh);
        }
        for (Slot& slot : slots_) {
            release(slot.uniforms);
            release(slot.layers);
            release(slot.instances);
            release(slot.scratch);
            release(slot.light_frame);
            release(slot.draw_records);
            release(slot.light_records);
            release(slot.top);
            if (slot.done != VK_NULL_HANDLE) {
                calls_.destroy_event(device_, slot.done, nullptr);
            }
        }
        if (pool_ != VK_NULL_HANDLE) {
            calls_.destroy_pool(device_, pool_, nullptr);
        }
        for (auto& [target, sums] : path_sums_) {
            release(sums.buffer);
        }
        for (Buffer& buffer : retired_buffers_) {
            release(buffer);
        }
        for (VkPipeline pipeline : light_pipelines_) {
            if (pipeline != VK_NULL_HANDLE) {
                calls_.destroy_pipeline(device_, pipeline, nullptr);
            }
        }
        for (VkPipeline pipeline : bake_pipelines_) {
            if (pipeline != VK_NULL_HANDLE) {
                calls_.destroy_pipeline(device_, pipeline, nullptr);
            }
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            calls_.destroy_pipeline(device_, pipeline_, nullptr);
        }
        for (VkPipelineLayout layout :
             {pipeline_layout_, light_pipeline_layout_, bake_pipeline_layout_}) {
            if (layout != VK_NULL_HANDLE) {
                calls_.destroy_pipeline_layout(device_, layout, nullptr);
            }
        }
        for (VkDescriptorSetLayout layout : {set_layout_, light_set_layout_, bake_set_layout_}) {
            if (layout != VK_NULL_HANDLE) {
                calls_.destroy_set_layout(device_, layout, nullptr);
            }
        }
        for (VkSampler sampler :
             {sampler_, texture_samplers_[0], texture_samplers_[1], texture_samplers_[2],
              texture_samplers_[3], environment_sampler_}) {
            if (sampler != VK_NULL_HANDLE) {
                calls_.destroy_sampler(device_, sampler, nullptr);
            }
        }
    }

    Result<void> init() {
        calls_.get_memory_properties(physical_, &memory_);
        VkPhysicalDeviceAccelerationStructurePropertiesKHR structures{};
        structures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &structures;
        calls_.get_properties2(physical_, &properties);
        scratch_alignment_ =
            std::max<VkDeviceSize>(1, structures.minAccelerationStructureScratchOffsetAlignment);
        max_storage_range_ = properties.properties.limits.maxStorageBufferRange;

        // The prepass's texels, read whole; the draws' textures, filtered and wrapped as their
        // draws sample them, at their first level as the software device's rays sample them;
        // and the sky's radiance.
        if (auto made = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            made) {
            sampler_ = made.value();
        } else {
            return made.error();
        }
        for (std::size_t i = 0; i < texture_samplers_.size(); ++i) {
            auto made = make_sampler((i & 1U) != 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                                     (i & 2U) != 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                   : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            if (!made) {
                return made.error();
            }
            texture_samplers_.at(i) = made.value();
        }
        if (auto made = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            made) {
            environment_sampler_ = made.value();
        } else {
            return made.error();
        }

        // By binding: shaders/ray_shadows.comp's, then shaders/ray_light.comp's.
        if (auto made =
                make_pipeline({{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                               {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                              shaders::kRayShadowsSpirv, sizeof(shaders::kRayShadowsSpirv),
                              set_layout_, pipeline_layout_, {&pipeline_, 1}, "ray shadows'");
            !made) {
            return made;
        }
        if (auto made = make_pipeline({{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRayTextures},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
                                      shaders::kRayLightSpirv, sizeof(shaders::kRayLightSpirv),
                                      light_set_layout_, light_pipeline_layout_, light_pipelines_,
                                      "ray light's");
            !made) {
            return made;
        }

        std::array<VkDescriptorPoolSize, 5> sizes{{
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2 * kSlots},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (2 + 4 + kRayTextures) * kSlots},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 * kSlots},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSlots},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kSlots},
        }};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 2 * kSlots;
        pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        if (const VkResult pooled = calls_.create_pool(device_, &pool, nullptr, &pool_);
            pooled != VK_SUCCESS) {
            return vulkan_error("making the rays' descriptor pool", pooled);
        }
        for (Slot& slot : slots_) {
            for (const auto& [set, layout] : {std::pair{&slot.set, &set_layout_},
                                              std::pair{&slot.light_set, &light_set_layout_}}) {
                VkDescriptorSetAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocate.descriptorPool = pool_;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = layout;
                if (const VkResult allocated = calls_.allocate_sets(device_, &allocate, set);
                    allocated != VK_SUCCESS) {
                    return vulkan_error("allocating the rays' descriptor sets", allocated);
                }
            }
            VkEventCreateInfo event{};
            event.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
            if (const VkResult created = calls_.create_event(device_, &event, nullptr, &slot.done);
                created != VK_SUCCESS) {
                return vulkan_error("making the rays' events", created);
            }
            for (const auto& [buffer, size] :
                 {std::pair{&slot.uniforms, sizeof(RayShadowsUniforms)},
                  std::pair{&slot.light_frame, sizeof(RayLightFrame)}}) {
                auto made_buffer = make_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
                if (!made_buffer) {
                    return made_buffer.error();
                }
                *buffer = made_buffer.value();
            }
        }
        return {};
    }

    Result<void> trace(SDL_GPUCommandBuffer* commands, std::span<const RayDraw> draws,
                       const RayWork& work, const RayShadowFrame& frame,
                       const RayTargets& targets) override {
        auto* native = static_cast<VkCommandBuffer>(SDL_SlashGPUNativeCommandBuffer(commands));
        if (native == VK_NULL_HANDLE) {
            return Error{ErrorCode::PlatformFailure,
                         "the frame's command buffer has no Vulkan command buffer"};
        }
        // Whether rays from the camera's side are traced, which shade what they meet.
        const bool lit = (work.reflection || work.gi || work.paths) && work.lighting != nullptr;
        ++frame_;
        Slot& slot = free_slot();
        release_unused();

        // Everything before (SDL's passes, which wrote the prepass and the meshes' buffers, and
        // the frames before, which read the structures this rebuilds) is done before this
        // copies, builds and traces.
        memory_barrier(native, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                           VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

        // The meshes' structures: built again for meshes that are new or changed. Without
        // rays from the camera's side only the draws that cast shadows take part.
        std::vector<const MeshStructure*> placed;
        placed.reserve(draws.size());
        bool copied = false;
        std::vector<std::pair<MeshStructure*, const RayDraw*>> to_build;
        for (const RayDraw& draw : draws) {
            if (draw.index_count < 3 || draw.vertex_count == 0 || mask_of(draw, lit) == 0) {
                placed.push_back(nullptr);
                continue;
            }
            const auto key = std::make_tuple(draw.mesh, draw.first_index, draw.index_count);
            auto found = meshes_.find(key);
            if (found != meshes_.end() && found->second.version != draw.version) {
                retired_.push_back(found->second);
                meshes_.erase(found);
                found = meshes_.end();
            }
            if (found == meshes_.end()) {
                auto copy = copy_mesh(native, draw);
                if (!copy) {
                    return copy.error();
                }
                found = meshes_.emplace(key, copy.value()).first;
                to_build.emplace_back(&found->second, &draw);
                copied = true;
            }
            found->second.used = frame_;
            placed.push_back(&found->second);
        }
        if (copied) {
            memory_barrier(native, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
        }
        if (auto built = build_meshes(native, to_build); !built) {
            return built;
        }

        // The frame's instances, a draw each, and its top-level structure.
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<std::uint32_t> layers;
        std::vector<std::size_t> instanced; // each instance's draw
        bool every_layer = true;            // every draw that casts shadows does for every light
        for (std::size_t i = 0; i < draws.size(); ++i) {
            if (placed[i] == nullptr) {
                continue;
            }
            const Mat4& m = draws[i].transform;
            VkAccelerationStructureInstanceKHR instance{};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    instance.transform.matrix[row][column] = m.at((column * 4) + row);
                }
            }
            instance.instanceCustomIndex = static_cast<std::uint32_t>(layers.size()) & 0xFFFFFFU;
            instance.mask = mask_of(draws[i], lit);
            instance.instanceShaderBindingTableRecordOffset = 0;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference = placed[i]->built.address;
            instances.push_back(instance);
            layers.push_back(draws[i].layers);
            instanced.push_back(i);
            if ((instance.mask & kCastMask) != 0) {
                every_layer = every_layer && draws[i].layers != 0;
            }
        }
        if (auto top = build_top(native, slot, instances, layers); !top) {
            return top;
        }

        void* depth_view = nullptr;
        void* normal_view = nullptr;
        Uint32 format = 0;
        SDL_SlashGPUNativeTexture(sdl_, targets.depth, &depth_view, &format);
        SDL_SlashGPUNativeTexture(sdl_, targets.normals, &normal_view, &format);
        if (depth_view == nullptr || normal_view == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         "the prepass's textures have no Vulkan images"};
        }
        // Pointers on 64-bit platforms and numbers on 32-bit ones, so not auto*.
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto depth = handle_of<VkImageView>(depth_view);
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto normals = handle_of<VkImageView>(normal_view);
        if (!work.lights.empty() || work.occlusion) {
            if (auto shadows =
                    trace_shadows(native, slot, work, frame, targets, depth, normals, every_layer);
                !shadows) {
                return shadows;
            }
        }
        if (lit) {
            if (auto traced = trace_light(native, slot, draws, placed, instanced, work, frame,
                                          targets, depth, normals);
                !traced) {
                return traced;
            }
        }
        calls_.set_event(native, slot.done, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        slot.recorded = true;
        return {};
    }

private:
    using MeshKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;

    // Which rays meet a draw: those from the camera's side, when lit, unless it only casts
    // shadows, and shadow rays unless it casts none.
    static std::uint8_t mask_of(const RayDraw& draw, bool lit) {
        std::uint8_t mask = draw.shadows != ShadowCasting::Off ? kCastMask : 0;
        if (lit && draw.shadows != ShadowCasting::ShadowsOnly) {
            mask |= kSeeMask;
        }
        return mask;
    }

    // The visibilities of the lights with ray slots and the ambient occlusion, into targets'
    // first three outputs.
    Result<void> trace_shadows(VkCommandBuffer native, Slot& slot, const RayWork& work,
                               const RayShadowFrame& frame, const RayTargets& targets,
                               VkImageView depth, VkImageView normals, bool every_layer) {
        RayShadowsUniforms uniforms{};
        for (std::size_t i = 0; i < 16; ++i) {
            uniforms.to_world.at(i) = static_cast<float>(frame.to_world.at(i));
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                uniforms.view_rows.at(row).at(column) = frame.view_rows.at(row).at(column);
            }
        }
        const auto light_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(work.lights.size(), kMaxRayShadowLights));
        uniforms.size = {frame.width, frame.height, frame.samples, light_count};
        bool every_mask = true;
        for (std::uint32_t k = 0; k < light_count; ++k) {
            const RayShadowLight& light = work.lights[k];
            const Float3& place = light.directional ? light.toward : light.position;
            uniforms.lights.at(k) = {.place = {place[0], place[1], place[2], light.size},
                                     .params = {light.opacity, light.directional ? 1.0F : 0.0F,
                                                static_cast<float>(light.slot), 0.0F},
                                     .mask = {light.caster_mask, 0, 0, 0}};
            every_mask = every_mask && light.caster_mask == 0xFFFFFFFFU;
        }
        // Every draw casts for every light: rays need not ask which draw they meet.
        uniforms.options = {every_mask && every_layer ? 1U : 0U, 0, 0, 0};
        if (work.occlusion) {
            uniforms.options[1] = 1;
            uniforms.occlusion = {work.occlusion->radius, work.occlusion->power,
                                  static_cast<float>(work.occlusion->samples),
                                  work.occlusion->intensity};
        }
        std::memcpy(slot.uniforms.mapped, &uniforms, sizeof(uniforms));

        std::array<VkImageView, kOutputs> outputs{};
        std::array<VkImage, kOutputs> output_images{};
        for (std::size_t i = 0; i < kOutputs; ++i) {
            auto found = native_texture(targets.outputs.at(i));
            if (!found) {
                return found.error();
            }
            output_images.at(i) = found.value().first;
            outputs.at(i) = found.value().second;
        }
        write_descriptors(slot, depth, normals, outputs);

        // The visibilities are written in the general layout, and go back to the one SDL keeps
        // sampled textures in.
        image_barriers(native, output_images, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        calls_.bind_pipeline(native, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        calls_.bind_sets(native, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &slot.set,
                         0, nullptr);
        calls_.dispatch(native, (frame.width + kGroupSize - 1) / kGroupSize,
                        (frame.height + kGroupSize - 1) / kGroupSize, 1);
        image_barriers(native, output_images, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        return {};
    }

    // The reflections and the ambient light, into targets' last three outputs: the draws each
    // instance stands for as records the shader shades them by, their textures, the lights and
    // the environment.
    Result<void> trace_light(VkCommandBuffer native, Slot& slot, std::span<const RayDraw> draws,
                             std::span<const MeshStructure* const> placed,
                             std::span<const std::size_t> instanced, const RayWork& work,
                             const RayShadowFrame& frame, const RayTargets& targets,
                             VkImageView depth, VkImageView normals) {
        const RayLighting& lighting = *work.lighting;
        // Each texture once, with the sampler its draws sample it with; past kRayTextures, white.
        std::vector<std::pair<SDL_GPUTexture*, std::size_t>> used;
        const auto texture_index = [&](const RayTexture& texture) {
            if (texture.texture == nullptr) {
                return kNoTexture;
            }
            const std::size_t sampler = (texture.nearest ? 1U : 0U) | (texture.repeat ? 2U : 0U);
            const std::pair<SDL_GPUTexture*, std::size_t> key{texture.texture, sampler};
            const auto found = std::find(used.begin(), used.end(), key);
            if (found != used.end()) {
                return static_cast<std::uint32_t>(found - used.begin());
            }
            if (used.size() >= kRayTextures) {
                return kNoTexture;
            }
            used.push_back(key);
            return static_cast<std::uint32_t>(used.size() - 1);
        };
        std::vector<RayDrawRecord> records;
        records.reserve(instanced.size());
        for (const std::size_t i : instanced) {
            const RayDraw& draw = draws[i];
            const MeshSurfaceUniforms& surface = draw.surface;
            RayDrawRecord& record = records.emplace_back();
            record.albedo = surface.albedo;
            record.emission = surface.emission;
            record.surface = surface.surface;
            record.metallic_channel = surface.metallic_channel;
            record.roughness_channel = surface.roughness_channel;
            record.options = {surface.options[0], surface.options[1], draw.layers, 0};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    record.normal_rows.at(row).at(column) =
                        draw.normal_model.at((column * 4) + row);
                }
            }
            record.uv1 = draw.uv1;
            const VkDeviceAddress vertices = placed[i]->vertices.address;
            const VkDeviceAddress indices = placed[i]->indices.address;
            record.buffers = {
                static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>(vertices >> 32U),
                static_cast<std::uint32_t>(indices), static_cast<std::uint32_t>(indices >> 32U)};
            record.textures = {texture_index(draw.textures[0]), texture_index(draw.textures[1]),
                               texture_index(draw.textures[2]), texture_index(draw.textures[3])};
            std::uint32_t cull = 2;
            if (draw.cull == CullMode::Back) {
                cull = 0;
            } else if (draw.cull == CullMode::Front) {
                cull = 1;
            }
            record.more = {texture_index(draw.textures[4]), cull, 0, 0};
        }
        std::vector<RayLightRecord> light_records;
        light_records.reserve(lighting.lights.size());
        for (const PathLight& light : lighting.lights) {
            light_records.push_back(
                {.position = light.uniforms.position,
                 .direction = light.uniforms.direction,
                 .color = light.uniforms.color,
                 .params = light.uniforms.params,
                 // As direct_light in src/path_trace.cpp reads the layers it lights.
                 .masks = {static_cast<std::uint32_t>(light.uniforms.params[3]), light.caster_mask,
                           light.shadow ? 1U : 0U, 0},
                 .shadow = {light.shadow_opacity, 0.0F, 0.0F, 0.0F}});
        }
        for (const auto& [buffer, bytes] :
             {std::pair{&slot.draw_records,
                        std::max<std::size_t>(1, records.size()) * sizeof(RayDrawRecord)},
              std::pair{&slot.light_records,
                        std::max<std::size_t>(1, light_records.size()) * sizeof(RayLightRecord)}}) {
            if (auto fitted = fit(*buffer, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
                !fitted) {
                return fitted;
            }
        }
        if (!records.empty()) {
            std::memcpy(slot.draw_records.mapped, records.data(),
                        records.size() * sizeof(RayDrawRecord));
        }
        if (!light_records.empty()) {
            std::memcpy(slot.light_records.mapped, light_records.data(),
                        light_records.size() * sizeof(RayLightRecord));
        }

        RayLightFrame uniforms{};
        for (std::size_t i = 0; i < 16; ++i) {
            uniforms.to_world.at(i) = static_cast<float>(frame.to_world.at(i));
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                uniforms.view_rows.at(row).at(column) = frame.view_rows.at(row).at(column);
            }
        }
        uniforms.size = {frame.width, frame.height, work.reflection ? work.reflection->samples : 0,
                         static_cast<std::uint32_t>(light_records.size())};
        uniforms.gi = {work.gi ? work.gi->samples : 0, 0, 0, 0};
        if (work.paths) {
            uniforms.paths = {work.paths->first, work.paths->samples, work.paths->bounces, 0};
        }
        uniforms.ambient = {lighting.ambient[0], lighting.ambient[1], lighting.ambient[2],
                            lighting.sky ? 1.0F : 0.0F};
        for (std::size_t row = 0; row < 3; ++row) {
            const Float3& r = lighting.to_sky.at(row);
            uniforms.sky_rows.at(row) = {r[0], r[1], r[2], 0.0F};
        }
        uniforms.sky_rows[0][3] = lighting.sky_energy;
        uniforms.sky_rows[1][3] = lighting.sky_share;
        std::memcpy(slot.light_frame.mapped, &uniforms, sizeof(uniforms));

        std::array<VkImageView, kLightOutputs> outputs{};
        std::array<VkImage, kLightOutputs> output_images{};
        for (std::size_t i = 0; i < kLightOutputs; ++i) {
            auto found = native_texture(targets.outputs.at(3 + i));
            if (!found) {
                return found.error();
            }
            output_images.at(i) = found.value().first;
            outputs.at(i) = found.value().second;
        }
        auto environment = native_texture(targets.environment);
        if (!environment) {
            return environment.error();
        }
        auto white = native_texture(targets.white);
        if (!white) {
            return white.error();
        }
        std::array<VkDescriptorImageInfo, kRayTextures> texture_infos{};
        for (std::size_t i = 0; i < kRayTextures; ++i) {
            texture_infos.at(i) = {.sampler = texture_samplers_[0],
                                   .imageView = white.value().second,
                                   .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        for (std::size_t i = 0; i < used.size(); ++i) {
            auto found = native_texture(used[i].first);
            if (!found) {
                return found.error();
            }
            texture_infos.at(i).imageView = found.value().second;
            texture_infos.at(i).sampler = texture_samplers_.at(used[i].second);
        }
        // A path-traced target's sums, kept from frame to frame, started over at zero when asked;
        // other frames bind a buffer the shader leaves alone.
        Buffer* sums = &slot.draw_records;
        if (work.paths) {
            const VkDeviceSize bytes = VkDeviceSize{frame.width} * frame.height * 16;
            PathSums& kept = path_sums_[work.paths->target];
            bool cleared = work.paths->restart;
            if (kept.buffer.buffer == VK_NULL_HANDLE || kept.buffer.size != bytes) {
                if (kept.buffer.buffer != VK_NULL_HANDLE) {
                    retired_buffers_.push_back(kept.buffer);
                }
                auto made = make_buffer(
                    bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    false);
                if (!made) {
                    kept.buffer = Buffer{};
                    return made.error();
                }
                kept.buffer = made.value();
                cleared = true;
            }
            if (cleared) {
                calls_.fill_buffer(native, kept.buffer.buffer, 0, VK_WHOLE_SIZE, 0);
                memory_barrier(native, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            }
            kept.used = frame_;
            sums = &kept.buffer;
        }
        auto background =
            native_texture(targets.background != nullptr ? targets.background : targets.white);
        if (!background) {
            return background.error();
        }
        write_light_descriptors(slot, depth, normals, outputs, environment.value().second,
                                texture_infos, sums->buffer, background.value().second);

        image_barriers(native, output_images, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        calls_.bind_sets(native, VK_PIPELINE_BIND_POINT_COMPUTE, light_pipeline_layout_, 0, 1,
                         &slot.light_set, 0, nullptr);
        const auto run = [&](std::size_t pass) {
            calls_.bind_pipeline(native, VK_PIPELINE_BIND_POINT_COMPUTE, light_pipelines_.at(pass));
            calls_.dispatch(native, (frame.width + kGroupSize - 1) / kGroupSize,
                            (frame.height + kGroupSize - 1) / kGroupSize, 1);
        };
        if (work.reflection) {
            run(0);
        }
        if (work.gi) {
            run(1);
            // The smoothing reads what each pixel's neighbors traced.
            memory_barrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            run(2);
        }
        if (work.paths) {
            run(kTracePaths);
            memory_barrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            run(kAveragePaths);
        }
        image_barriers(native, output_images, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        return {};
    }

    Result<LightmapTraced> trace_lightmap(const LightmapTrace& trace) override {
        const std::size_t texels = std::size_t{trace.width} * trace.height * trace.slices;
        if (trace.indices.empty() || trace.indices.size() != trace.triangles.size() * 3 ||
            texels == 0 || trace.albedo.size() != texels || trace.emission.size() != texels ||
            trace.position.size() != texels || trace.normal.size() != texels ||
            trace.environment.size() !=
                std::size_t{trace.environment_width} * trace.environment_height ||
            trace.environment.empty()) {
            return Error{ErrorCode::InvalidArgument, "a lightmap trace's layers don't fit it"};
        }
        const VkDeviceSize texel_bytes = VkDeviceSize{texels} * sizeof(std::array<float, 4>);
        const VkDeviceSize probe_bytes =
            VkDeviceSize{trace.probe_points.size()} * 9 * sizeof(std::array<float, 4>);
        if (texel_bytes > max_storage_range_ || probe_bytes > max_storage_range_) {
            return Error{ErrorCode::PlatformFailure,
                         "the lightmap's atlas is larger than this GPU's storage buffers"};
        }
        if (auto made = make_bake_pipelines(); !made) {
            return made.error();
        }
        Bake bake(*this);

        // The inputs, through staging buffers into the GPU's memory; the outputs, cleared.
        std::vector<std::array<float, 4>> probes(trace.probe_points.size());
        for (std::size_t i = 0; i < probes.size(); ++i) {
            probes[i] = {trace.probe_points[i][0], trace.probe_points[i][1],
                         trace.probe_points[i][2], 0.0F};
        }
        std::vector<std::array<std::uint32_t, 2>> triangles(trace.triangles.size());
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            const BakeTriangle& t = trace.triangles[i];
            // CullMode's order is the shader's: back, front, none.
            triangles[i] = {t.slice, static_cast<std::uint32_t>(t.cull)};
        }
        std::vector<LightmapLightRecord> lights(trace.lights.size());
        for (std::size_t i = 0; i < lights.size(); ++i) {
            const BakeLight& l = trace.lights[i];
            // LightType's order is the shader's: directional, omni, spot.
            const auto kind = static_cast<float>(l.type);
            lights[i] = {
                .position = {l.position[0], l.position[1], l.position[2], kind},
                .direction = {l.direction[0], l.direction[1], l.direction[2], l.energy},
                .color = {l.color[0], l.color[1], l.color[2], l.indirect_energy},
                .shape = {l.size, l.range, l.attenuation, l.cos_spot_angle},
                .more = {l.inv_spot_attenuation, l.shadow_blur, l.static_bake ? 1.0F : 0.0F, 0.0F}};
        }
        struct Input {
            const void* data;
            VkDeviceSize bytes;
        };
        // By binding, from 2: vertices, indices, triangles, lights, albedo, emission, positions,
        // normals, environment, then 13: the probes.
        const std::array<Input, 10> inputs{{
            {trace.vertices.data(), trace.vertices.size() * sizeof(BakeVertex)},
            {trace.indices.data(), trace.indices.size() * sizeof(std::uint32_t)},
            {triangles.data(), triangles.size() * sizeof(triangles[0])},
            {lights.data(), lights.size() * sizeof(LightmapLightRecord)},
            {trace.albedo.data(), trace.albedo.size_bytes()},
            {trace.emission.data(), trace.emission.size_bytes()},
            {trace.position.data(), trace.position.size_bytes()},
            {trace.normal.data(), trace.normal.size_bytes()},
            {trace.environment.data(), trace.environment.size_bytes()},
            {probes.data(), probes.size() * sizeof(probes[0])},
        }};
        static_assert(sizeof(BakeVertex) == 32);
        constexpr VkBufferUsageFlags kInput =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        constexpr VkBufferUsageFlags kOutput = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        std::array<Buffer, 10> in{};
        std::array<Buffer, 10> staging{};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto made = bake.buffer(inputs.at(i).bytes, kInput, false);
            if (!made) {
                return made.error();
            }
            in.at(i) = made.value();
            if (inputs.at(i).bytes == 0) {
                continue;
            }
            auto stage = bake.buffer(inputs.at(i).bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            if (!stage) {
                return stage.error();
            }
            staging.at(i) = stage.value();
            std::memcpy(staging.at(i).mapped, inputs.at(i).data, inputs.at(i).bytes);
        }
        // light_source, accum, probe sums.
        const std::array<VkDeviceSize, 3> output_bytes{texel_bytes, texel_bytes, probe_bytes};
        std::array<Buffer, 3> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            auto made = bake.buffer(output_bytes.at(i), kOutput, false);
            if (!made) {
                return made.error();
            }
            out.at(i) = made.value();
        }
        auto params_made =
            bake.buffer(sizeof(LightmapParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (!params_made) {
            return params_made.error();
        }
        const Buffer params_buffer = params_made.value();
        auto instance_made =
            bake.buffer(sizeof(VkAccelerationStructureInstanceKHR),
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        if (!instance_made) {
            return instance_made.error();
        }
        const Buffer instance_buffer = instance_made.value();

        // One bottom-level structure of every triangle, and a top-level one with it once.
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = 0; // not opaque, so the shader tests each triangle as the CPU does
        VkAccelerationStructureGeometryTrianglesDataKHR& data = geometry.geometry.triangles;
        data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data.vertexData.deviceAddress = in[0].address;
        data.vertexStride = sizeof(BakeVertex);
        data.maxVertex = static_cast<std::uint32_t>(trace.vertices.size() - 1);
        data.indexType = VK_INDEX_TYPE_UINT32;
        data.indexData.deviceAddress = in[1].address;
        VkAccelerationStructureBuildGeometryInfoKHR bottom{};
        bottom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bottom.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bottom.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bottom.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bottom.geometryCount = 1;
        bottom.pGeometries = &geometry;
        const auto triangle_count = static_cast<std::uint32_t>(trace.triangles.size());
        VkAccelerationStructureBuildSizesInfoKHR bottom_sizes{};
        bottom_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        calls_.build_sizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bottom,
                           &triangle_count, &bottom_sizes);
        auto bottom_made = make_structure(bottom_sizes.accelerationStructureSize,
                                          VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        if (!bottom_made) {
            return bottom_made.error();
        }
        bake.structures.push_back(bottom_made.value());
        const Structure bottom_structure = bottom_made.value();

        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = 1.0F;
        instance.transform.matrix[1][1] = 1.0F;
        instance.transform.matrix[2][2] = 1.0F;
        instance.mask = 0xFF;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottom_structure.address;
        std::memcpy(instance_buffer.mapped, &instance, sizeof(instance));
        VkAccelerationStructureGeometryKHR instances{};
        instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        instances.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instances.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        instances.geometry.instances.arrayOfPointers = VK_FALSE;
        instances.geometry.instances.data.deviceAddress = instance_buffer.address;
        VkAccelerationStructureBuildGeometryInfoKHR top{};
        top.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        top.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        top.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top.geometryCount = 1;
        top.pGeometries = &instances;
        const std::uint32_t one = 1;
        VkAccelerationStructureBuildSizesInfoKHR top_sizes{};
        top_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        calls_.build_sizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &top, &one,
                           &top_sizes);
        auto top_made = make_structure(top_sizes.accelerationStructureSize,
                                       VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        if (!top_made) {
            return top_made.error();
        }
        bake.structures.push_back(top_made.value());
        const Structure top_structure = top_made.value();
        auto scratch_made =
            bake.buffer(std::max(bottom_sizes.buildScratchSize, top_sizes.buildScratchSize) +
                            scratch_alignment_,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        if (!scratch_made) {
            return scratch_made.error();
        }
        const VkDeviceAddress scratch = aligned(scratch_made.value().address);
        bottom.dstAccelerationStructure = bottom_structure.structure;
        bottom.scratchData.deviceAddress = scratch;
        top.dstAccelerationStructure = top_structure.structure;
        top.scratchData.deviceAddress = scratch;

        if (auto set_up = run_commands([&](VkCommandBuffer native) -> Result<void> {
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    if (inputs.at(i).bytes > 0) {
                        const VkBufferCopy region{
                            .srcOffset = 0, .dstOffset = 0, .size = inputs.at(i).bytes};
                        calls_.copy_buffer(native, staging.at(i).buffer, in.at(i).buffer, 1,
                                           &region);
                    }
                }
                for (const Buffer& buffer : out) {
                    calls_.fill_buffer(native, buffer.buffer, 0, VK_WHOLE_SIZE, 0);
                }
                memory_barrier(native, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                const VkAccelerationStructureBuildRangeInfoKHR bottom_range{.primitiveCount =
                                                                                triangle_count,
                                                                            .primitiveOffset = 0,
                                                                            .firstVertex = 0,
                                                                            .transformOffset = 0};
                const VkAccelerationStructureBuildRangeInfoKHR* bottom_ranges = &bottom_range;
                calls_.build_structures(native, 1, &bottom, &bottom_ranges);
                // The top level's build waits for the bottom's, and reuses its scratch.
                memory_barrier(native, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                               VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                               VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                   VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
                const VkAccelerationStructureBuildRangeInfoKHR top_range{.primitiveCount = 1,
                                                                         .primitiveOffset = 0,
                                                                         .firstVertex = 0,
                                                                         .transformOffset = 0};
                const VkAccelerationStructureBuildRangeInfoKHR* top_ranges = &top_range;
                calls_.build_structures(native, 1, &top, &top_ranges);
                memory_barrier(native, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                               VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
                return {};
            });
            !set_up) {
            return set_up.error();
        }

        // The descriptors: the structure, Params, the inputs and the outputs.
        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14},
        }};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes.data();
        if (const VkResult pooled = calls_.create_pool(device_, &pool, nullptr, &bake.pool);
            pooled != VK_SUCCESS) {
            return vulkan_error("making a bake's descriptor pool", pooled);
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = bake.pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &bake_set_layout_;
        if (const VkResult allocated = calls_.allocate_sets(device_, &allocate, &set);
            allocated != VK_SUCCESS) {
            return vulkan_error("allocating a bake's descriptor set", allocated);
        }
        VkWriteDescriptorSetAccelerationStructureKHR structure{};
        structure.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        structure.accelerationStructureCount = 1;
        structure.pAccelerationStructures = &top_structure.structure;
        // Bindings 1 to 14.
        const std::array<VkBuffer, 14> bound{
            params_buffer.buffer, in[0].buffer,  in[1].buffer, in[2].buffer, in[3].buffer,
            in[4].buffer,         in[5].buffer,  in[6].buffer, in[7].buffer, in[8].buffer,
            out[0].buffer,        out[1].buffer, in[9].buffer, out[2].buffer};
        std::array<VkDescriptorBufferInfo, 14> buffers{};
        std::array<VkWriteDescriptorSet, 15> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            VkWriteDescriptorSet& write = writes.at(i);
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = i;
            write.descriptorCount = 1;
            if (i == 0) {
                write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                write.pNext = &structure;
                continue;
            }
            buffers.at(i - 1) = {.buffer = bound.at(i - 1), .offset = 0, .range = VK_WHOLE_SIZE};
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffers.at(i - 1);
        }
        calls_.update_sets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);

        LightmapParams params;
        params.size = {trace.width, trace.height, trace.slices, trace.environment_width};
        params.counts = {trace.environment_height, static_cast<std::uint32_t>(lights.size()),
                         trace.rays, trace.bounces};
        params.flags = {trace.use_texture_for_bounces ? 1U : 0U, 0, 0, 0};
        params.values = {trace.bias, trace.supersampling, trace.exposure, trace.world_size};
        params.more = {trace.bounce_indirect_energy, 0.0F, 0.0F, 0.0F};
        for (std::size_t c = 0; c < 3; ++c) {
            params.space.at(c) = {trace.probe_space.at(c)[0], trace.probe_space.at(c)[1],
                                  trace.probe_space.at(c)[2], 0.0F};
        }
        // One dispatch of a pass over count invocations, its Params set first.
        const auto dispatch = [&](std::size_t pass, std::uint32_t count) -> Result<void> {
            std::memcpy(params_buffer.mapped, &params, sizeof(params));
            return run_commands([&](VkCommandBuffer native) -> Result<void> {
                memory_barrier(native,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                calls_.bind_pipeline(native, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     bake_pipelines_.at(pass));
                calls_.bind_sets(native, VK_PIPELINE_BIND_POINT_COMPUTE, bake_pipeline_layout_, 0,
                                 1, &set, 0, nullptr);
                calls_.dispatch(native, (count + kBakeGroupSize - 1) / kBakeGroupSize, 1, 1);
                return {};
            });
        };
        // Each slice's texels a region at a time: the direct light, then each pass of bounces.
        const std::uint32_t slice_texels = trace.width * trace.height;
        const auto each_region = [&](std::size_t pass) -> Result<void> {
            for (std::uint32_t slice = 0; slice < trace.slices; ++slice) {
                for (std::uint32_t first = 0; first < slice_texels; first += kBakeRegionTexels) {
                    const std::uint32_t end = std::min(first + kBakeRegionTexels, slice_texels);
                    params.step[0] = slice;
                    params.flags[1] = first;
                    params.flags[2] = end;
                    if (auto ran = dispatch(pass, end - first); !ran) {
                        return ran;
                    }
                }
            }
            return {};
        };
        if (auto ran = each_region(0); !ran) {
            return ran.error();
        }
        if (trace.bounces > 0) {
            for (std::uint32_t from = 0; from < trace.rays; from += kBakeRaysPerPass) {
                params.step[1] = from;
                params.step[2] = std::min(from + kBakeRaysPerPass, trace.rays);
                if (auto ran = each_region(1); !ran) {
                    return ran.error();
                }
            }
        }
        const auto probe_count = static_cast<std::uint32_t>(trace.probe_points.size());
        const std::uint32_t probe_rays = std::clamp(trace.probe_rays, 16U, 8192U);
        if (probe_count > 0) {
            params.step[3] = probe_count;
            for (std::uint32_t from = 0; from < probe_rays; from += kBakeRaysPerProbePass) {
                params.step[1] = from;
                params.step[2] = std::min(from + kBakeRaysPerProbePass, probe_rays);
                if (auto ran = dispatch(2, probe_count); !ran) {
                    return ran.error();
                }
            }
        }

        // Back to the CPU.
        std::array<Buffer, 3> back{};
        for (std::size_t i = 0; i < back.size(); ++i) {
            auto made = bake.buffer(output_bytes.at(i), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            if (!made) {
                return made.error();
            }
            back.at(i) = made.value();
        }
        if (auto read = run_commands([&](VkCommandBuffer native) -> Result<void> {
                memory_barrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT);
                for (std::size_t i = 0; i < back.size(); ++i) {
                    if (output_bytes.at(i) > 0) {
                        const VkBufferCopy region{
                            .srcOffset = 0, .dstOffset = 0, .size = output_bytes.at(i)};
                        calls_.copy_buffer(native, out.at(i).buffer, back.at(i).buffer, 1, &region);
                    }
                }
                memory_barrier(native, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                return {};
            });
            !read) {
            return read.error();
        }
        LightmapTraced traced;
        traced.light_source.resize(texels);
        traced.accum.resize(texels);
        std::memcpy(traced.light_source.data(), back[0].mapped, texel_bytes);
        std::memcpy(traced.accum.data(), back[1].mapped, texel_bytes);
        traced.probe_sh.resize(probe_count);
        const float scale = 4.0F / static_cast<float>(probe_rays);
        const auto* sums = static_cast<const std::array<float, 4>*>(back[2].mapped);
        for (std::size_t probe = 0; probe < probe_count; ++probe) {
            for (std::size_t j = 0; j < 9; ++j) {
                const std::array<float, 4>& sum = sums[(probe * 9) + j];
                traced.probe_sh[probe].at(j) = {sum[0] * scale, sum[1] * scale, sum[2] * scale};
            }
        }
        return traced;
    }

    // An SDL texture's Vulkan image and view.
    Result<std::pair<VkImage, VkImageView>> native_texture(SDL_GPUTexture* texture) {
        void* view = nullptr;
        Uint32 format = 0;
        // NOLINTNEXTLINE(readability-qualified-auto): a number on 32-bit platforms
        const auto image = handle_of<VkImage>(
            texture != nullptr ? SDL_SlashGPUNativeTexture(sdl_, texture, &view, &format)
                               : nullptr);
        if (view == nullptr) {
            return Error{ErrorCode::PlatformFailure, "a texture rays use has no Vulkan image"};
        }
        return std::pair{image, handle_of<VkImageView>(view)};
    }

    Result<VkSampler> make_sampler(VkFilter filter, VkSamplerAddressMode wrap) {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = wrap;
        info.addressModeV = wrap;
        info.addressModeW = wrap;
        info.maxLod = 0.0F;
        VkSampler out = VK_NULL_HANDLE;
        if (const VkResult made = calls_.create_sampler(device_, &info, nullptr, &out);
            made != VK_SUCCESS) {
            return vulkan_error("making a sampler for rays", made);
        }
        return out;
    }

    // Compute pipelines of a SPIR-V shader whose set 0 has a binding of each type, of count
    // descriptors, in order: one for each of pipelines, its place there the shader's
    // specialization constant 0.
    Result<void>
    make_pipeline(std::initializer_list<std::pair<VkDescriptorType, std::uint32_t>> types,
                  const unsigned char* spirv, std::size_t spirv_size,
                  VkDescriptorSetLayout& set_layout, VkPipelineLayout& pipeline_layout,
                  std::span<VkPipeline> pipelines, const std::string& what) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& [type, count] : types) {
            VkDescriptorSetLayoutBinding& binding = bindings.emplace_back();
            binding.binding = static_cast<std::uint32_t>(bindings.size() - 1);
            binding.descriptorType = type;
            binding.descriptorCount = count;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        if (const VkResult made = calls_.create_set_layout(device_, &layout, nullptr, &set_layout);
            made != VK_SUCCESS) {
            return vulkan_error("making the " + what + " descriptor set layout", made);
        }
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout;
        if (const VkResult made =
                calls_.create_pipeline_layout(device_, &layout_info, nullptr, &pipeline_layout);
            made != VK_SUCCESS) {
            return vulkan_error("making the " + what + " pipeline layout", made);
        }
        VkShaderModuleCreateInfo module{};
        module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module.codeSize = spirv_size;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): SPIR-V is 4-byte aligned
        module.pCode = reinterpret_cast<const std::uint32_t*>(spirv);
        VkShaderModule shader = VK_NULL_HANDLE;
        if (const VkResult made = calls_.create_shader(device_, &module, nullptr, &shader);
            made != VK_SUCCESS) {
            return vulkan_error("making the " + what + " shader", made);
        }
        for (std::size_t i = 0; i < pipelines.size(); ++i) {
            const auto pass = static_cast<std::uint32_t>(i);
            const VkSpecializationMapEntry entry{
                .constantID = 0, .offset = 0, .size = sizeof(pass)};
            VkSpecializationInfo specialization{};
            specialization.mapEntryCount = 1;
            specialization.pMapEntries = &entry;
            specialization.dataSize = sizeof(pass);
            specialization.pData = &pass;
            VkComputePipelineCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = shader;
            info.stage.pName = "main";
            info.stage.pSpecializationInfo = &specialization;
            info.layout = pipeline_layout;
            const VkResult made =
                calls_.create_pipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[i]);
            if (made != VK_SUCCESS) {
                calls_.destroy_shader(device_, shader, nullptr);
                return vulkan_error("making the " + what + " pipeline", made);
            }
        }
        calls_.destroy_shader(device_, shader, nullptr);
        return {};
    }

    // A slot the GPU is done with, waiting for it when every one is busy.
    Slot& free_slot() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (std::size_t i = 0; i < kSlots; ++i) {
                Slot& slot = slots_.at((next_slot_ + i) % kSlots);
                if (!slot.recorded || calls_.event_status(device_, slot.done) == VK_EVENT_SET) {
                    next_slot_ = (next_slot_ + i + 1) % kSlots;
                    slot.recorded = false;
                    calls_.reset_event(device_, slot.done);
                    return slot;
                }
            }
            SDL_WaitForGPUIdle(sdl_);
        }
        // Recorded into a command buffer that was never submitted: nothing uses it.
        Slot& slot = slots_.at(next_slot_);
        next_slot_ = (next_slot_ + 1) % kSlots;
        slot.recorded = false;
        calls_.reset_event(device_, slot.done);
        return slot;
    }

    // Lets go of meshes' structures no frame in flight can use: those replaced or unused for as
    // many frames as there are slots.
    void release_unused() {
        for (auto it = meshes_.begin(); it != meshes_.end();) {
            if (it->second.used + (2 * kSlots) < frame_) {
                retired_.push_back(it->second);
                it = meshes_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = path_sums_.begin(); it != path_sums_.end();) {
            if (it->second.used + (2 * kSlots) < frame_) {
                retired_buffers_.push_back(it->second.buffer);
                it = path_sums_.erase(it);
            } else {
                ++it;
            }
        }
        bool idle = true;
        for (const Slot& slot : slots_) {
            idle =
                idle && (!slot.recorded || calls_.event_status(device_, slot.done) == VK_EVENT_SET);
        }
        if (idle) {
            for (MeshStructure& mesh : retired_) {
                release(mesh);
            }
            retired_.clear();
            for (Buffer& buffer : retired_buffers_) {
                release(buffer);
            }
            retired_buffers_.clear();
        }
    }

    Result<Buffer> make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        Buffer out;
        out.size = std::max<VkDeviceSize>(size, 16);
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = out.size;
        info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (const VkResult made = calls_.create_buffer(device_, &info, nullptr, &out.buffer);
            made != VK_SUCCESS) {
            return vulkan_error("making a buffer for rays", made);
        }
        VkMemoryRequirements needs{};
        calls_.buffer_requirements(device_, out.buffer, &needs);
        const VkMemoryPropertyFlags wanted =
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::uint32_t type = memory_.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if ((needs.memoryTypeBits & (1U << i)) != 0 &&
                (memory_.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        }
        if (type == memory_.memoryTypeCount) {
            release(out);
            return Error{ErrorCode::PlatformFailure, "no memory fits a buffer for rays"};
        }
        VkMemoryAllocateFlagsInfo flags{};
        flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &flags;
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (const VkResult allocated =
                calls_.allocate_memory(device_, &allocate, nullptr, &out.memory);
            allocated != VK_SUCCESS) {
            release(out);
            return vulkan_error("allocating memory for rays", allocated);
        }
        calls_.bind_buffer_memory(device_, out.buffer, out.memory, 0);
        if (host) {
            if (const VkResult mapped =
                    calls_.map_memory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
                mapped != VK_SUCCESS) {
                release(out);
                return vulkan_error("mapping memory for rays", mapped);
            }
        }
        VkBufferDeviceAddressInfo where{};
        where.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        where.buffer = out.buffer;
        out.address = calls_.buffer_address(device_, &where);
        return out;
    }

    // buffer, made again larger when it holds less than size.
    Result<void> fit(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= size) {
            return {};
        }
        release(buffer);
        auto made = make_buffer(std::max(size, buffer.size * 2), usage, host);
        if (!made) {
            return made.error();
        }
        buffer = made.value();
        return {};
    }

    Result<Structure> make_structure(VkDeviceSize size, VkAccelerationStructureTypeKHR type) {
        Structure out;
        auto storage =
            make_buffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        if (!storage) {
            return storage.error();
        }
        out.storage = storage.value();
        VkAccelerationStructureCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        info.buffer = out.storage.buffer;
        info.size = size;
        info.type = type;
        if (const VkResult made = calls_.create_structure(device_, &info, nullptr, &out.structure);
            made != VK_SUCCESS) {
            release(out);
            return vulkan_error("making an acceleration structure", made);
        }
        VkAccelerationStructureDeviceAddressInfoKHR where{};
        where.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        where.accelerationStructure = out.structure;
        out.address = calls_.structure_address(device_, &where);
        return out;
    }

    // Copies a draw's vertices and the indices it draws out of SDL's buffers, for its structure.
    Result<MeshStructure> copy_mesh(VkCommandBuffer native, const RayDraw& draw) {
        // A pointer on 64-bit platforms and a number on 32-bit ones, so not auto*.
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto vertices = handle_of<VkBuffer>(SDL_SlashGPUNativeBuffer(sdl_, draw.vertices));
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto indices = handle_of<VkBuffer>(SDL_SlashGPUNativeBuffer(sdl_, draw.indices));
        if (vertices == VK_NULL_HANDLE || indices == VK_NULL_HANDLE) {
            return Error{ErrorCode::PlatformFailure, "a mesh's buffers have no Vulkan buffers"};
        }
        MeshStructure out;
        out.version = draw.version;
        // Read by the structure's build, and by the light's shader at their addresses.
        constexpr VkBufferUsageFlags kInput =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        const VkDeviceSize vertex_bytes = VkDeviceSize{draw.vertex_count} * sizeof(Vertex3D);
        const VkDeviceSize index_bytes = VkDeviceSize{draw.index_count} * sizeof(std::uint32_t);
        auto vertex_copy = make_buffer(vertex_bytes, kInput, false);
        if (!vertex_copy) {
            return vertex_copy.error();
        }
        out.vertices = vertex_copy.value();
        auto index_copy = make_buffer(index_bytes, kInput, false);
        if (!index_copy) {
            release(out);
            return index_copy.error();
        }
        out.indices = index_copy.value();
        const VkBufferCopy vertex_region{.srcOffset = 0, .dstOffset = 0, .size = vertex_bytes};
        calls_.copy_buffer(native, vertices, out.vertices.buffer, 1, &vertex_region);
        const VkBufferCopy index_region{.srcOffset =
                                            VkDeviceSize{draw.first_index} * sizeof(std::uint32_t),
                                        .dstOffset = 0,
                                        .size = index_bytes};
        calls_.copy_buffer(native, indices, out.indices.buffer, 1, &index_region);
        return out;
    }

    static VkAccelerationStructureGeometryKHR triangles_of(const MeshStructure& mesh,
                                                           const RayDraw& draw) {
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = 0; // not opaque, so rays can ask which draw casts for which light
        VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = mesh.vertices.address;
        triangles.vertexStride = sizeof(Vertex3D);
        triangles.maxVertex = draw.vertex_count - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = mesh.indices.address;
        return geometry;
    }

    // Builds the bottom-level structures of meshes copied this frame.
    Result<void> build_meshes(VkCommandBuffer native,
                              std::span<const std::pair<MeshStructure*, const RayDraw*>> meshes) {
        if (meshes.empty()) {
            return {};
        }
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        geometries.reserve(meshes.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<VkDeviceSize> scratch_at;
        VkDeviceSize scratch = 0;
        for (const auto& [mesh, draw] : meshes) {
            geometries.push_back(triangles_of(*mesh, *draw));
        }
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const auto& [mesh, draw] = meshes[i];
            VkAccelerationStructureBuildGeometryInfoKHR build{};
            build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &geometries[i];
            const std::uint32_t triangles = draw->index_count / 3;
            VkAccelerationStructureBuildSizesInfoKHR sizes{};
            sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            calls_.build_sizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                               &triangles, &sizes);
            auto made = make_structure(sizes.accelerationStructureSize,
                                       VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
            if (!made) {
                return made.error();
            }
            mesh->built = made.value();
            build.dstAccelerationStructure = mesh->built.structure;
            scratch_at.push_back(scratch);
            scratch += aligned(sizes.buildScratchSize);
            builds.push_back(build);
            ranges.push_back({.primitiveCount = triangles,
                              .primitiveOffset = 0,
                              .firstVertex = 0,
                              .transformOffset = 0});
        }
        if (auto fitted = fit(mesh_scratch_, scratch + scratch_alignment_,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
            !fitted) {
            return fitted;
        }
        const VkDeviceAddress base = aligned(mesh_scratch_.address);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_pointers;
        for (std::size_t i = 0; i < builds.size(); ++i) {
            builds[i].scratchData.deviceAddress = base + scratch_at[i];
            range_pointers.push_back(&ranges[i]);
        }
        calls_.build_structures(native, static_cast<std::uint32_t>(builds.size()), builds.data(),
                                range_pointers.data());
        memory_barrier(native, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        return {};
    }

    // Builds slot's top-level structure over instances, and fills its draws' layers.
    Result<void> build_top(VkCommandBuffer native, Slot& slot,
                           std::span<const VkAccelerationStructureInstanceKHR> instances,
                           std::span<const std::uint32_t> layers) {
        const auto count = static_cast<std::uint32_t>(instances.size());
        if (auto fitted =
                fit(slot.instances,
                    std::max<VkDeviceSize>(1, count) * sizeof(VkAccelerationStructureInstanceKHR),
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
            !fitted) {
            return fitted;
        }
        if (auto fitted = fit(slot.layers, std::max<VkDeviceSize>(1, count) * sizeof(std::uint32_t),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            !fitted) {
            return fitted;
        }
        if (count > 0) {
            std::memcpy(slot.instances.mapped, instances.data(), instances.size_bytes());
            std::memcpy(slot.layers.mapped, layers.data(), layers.size_bytes());
        }
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = slot.instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        calls_.build_sizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count,
                           &sizes);
        if (slot.top.structure == VK_NULL_HANDLE ||
            slot.top_size < sizes.accelerationStructureSize) {
            release(slot.top);
            auto made = make_structure(sizes.accelerationStructureSize,
                                       VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
            if (!made) {
                return made.error();
            }
            slot.top = made.value();
            slot.top_size = sizes.accelerationStructureSize;
        }
        if (auto fitted = fit(slot.scratch, sizes.buildScratchSize + scratch_alignment_,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
            !fitted) {
            return fitted;
        }
        build.dstAccelerationStructure = slot.top.structure;
        build.scratchData.deviceAddress = aligned(slot.scratch.address);
        const VkAccelerationStructureBuildRangeInfoKHR range{
            .primitiveCount = count, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        calls_.build_structures(native, 1, &build, &ranges);
        memory_barrier(native, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        return {};
    }

    void write_descriptors(Slot& slot, VkImageView depth, VkImageView normals,
                           const std::array<VkImageView, kOutputs>& outputs) {
        VkWriteDescriptorSetAccelerationStructureKHR structure{};
        structure.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        structure.accelerationStructureCount = 1;
        structure.pAccelerationStructures = &slot.top.structure;
        const std::array<VkDescriptorImageInfo, 5> images{{
            {.sampler = sampler_,
             .imageView = depth,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = sampler_,
             .imageView = normals,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[0],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[1],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[2],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        }};
        const VkDescriptorBufferInfo uniforms{
            .buffer = slot.uniforms.buffer, .offset = 0, .range = sizeof(RayShadowsUniforms)};
        const VkDescriptorBufferInfo layers{
            .buffer = slot.layers.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 8> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            VkWriteDescriptorSet& write = writes.at(i);
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.set;
            write.dstBinding = i;
            write.descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[0].pNext = &structure;
        for (std::size_t i = 0; i < 4; ++i) {
            writes.at(1 + i).descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes.at(1 + i).pImageInfo = &images.at(i);
        }
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[5].pBufferInfo = &uniforms;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pBufferInfo = &layers;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[7].pImageInfo = &images[4];
        calls_.update_sets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    }

    void write_light_descriptors(Slot& slot, VkImageView depth, VkImageView normals,
                                 const std::array<VkImageView, kLightOutputs>& outputs,
                                 VkImageView environment,
                                 const std::array<VkDescriptorImageInfo, kRayTextures>& textures,
                                 VkBuffer sums, VkImageView background) {
        VkWriteDescriptorSetAccelerationStructureKHR structure{};
        structure.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        structure.accelerationStructureCount = 1;
        structure.pAccelerationStructures = &slot.top.structure;
        const std::array<VkDescriptorImageInfo, 7> images{{
            {.sampler = sampler_,
             .imageView = depth,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = sampler_,
             .imageView = normals,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[0],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = environment_sampler_,
             .imageView = environment,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[1],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE,
             .imageView = outputs[2],
             .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = sampler_,
             .imageView = background,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};
        const std::array<VkDescriptorBufferInfo, 4> buffers{{
            {.buffer = slot.light_frame.buffer, .offset = 0, .range = sizeof(RayLightFrame)},
            {.buffer = slot.draw_records.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
            {.buffer = slot.light_records.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
            {.buffer = sums, .offset = 0, .range = VK_WHOLE_SIZE},
        }};
        std::array<VkWriteDescriptorSet, 13> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            VkWriteDescriptorSet& write = writes.at(i);
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.light_set;
            write.dstBinding = i;
            write.descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[0].pNext = &structure;
        for (const std::size_t i : {1U, 2U}) {
            writes.at(i).descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes.at(i).pImageInfo = &images.at(i - 1);
        }
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &images[2];
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = buffers.data();
        for (const std::size_t i : {5U, 6U}) {
            writes.at(i).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes.at(i).pBufferInfo = &buffers.at(i - 4);
        }
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].pImageInfo = &images[3];
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8].descriptorCount = kRayTextures;
        writes[8].pImageInfo = textures.data();
        for (const std::size_t i : {9U, 10U}) {
            writes.at(i).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes.at(i).pImageInfo = &images.at(i - 5);
        }
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[11].pBufferInfo = &buffers[3];
        writes[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[12].pImageInfo = &images[6];
        calls_.update_sets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    }

    void memory_barrier(VkCommandBuffer native, VkPipelineStageFlags from,
                        VkAccessFlags from_access, VkPipelineStageFlags to,
                        VkAccessFlags to_access) const {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = from_access;
        barrier.dstAccessMask = to_access;
        calls_.barrier(native, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void image_barriers(VkCommandBuffer native, std::span<const VkImage> images, VkImageLayout from,
                        VkImageLayout to, VkPipelineStageFlags from_stage,
                        VkAccessFlags from_access, VkPipelineStageFlags to_stage,
                        VkAccessFlags to_access) const {
        std::vector<VkImageMemoryBarrier> barriers(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) {
            VkImageMemoryBarrier& barrier = barriers.at(i);
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = from_access;
            barrier.dstAccessMask = to_access;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[i];
            barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = 1,
                                        .baseArrayLayer = 0,
                                        .layerCount = 1};
        }
        calls_.barrier(native, from_stage, to_stage, 0, 0, nullptr, 0, nullptr,
                       static_cast<std::uint32_t>(barriers.size()), barriers.data());
    }

    [[nodiscard]] VkDeviceSize aligned(VkDeviceSize at) const {
        return (at + scratch_alignment_ - 1) / scratch_alignment_ * scratch_alignment_;
    }

    void release(Buffer& buffer) const {
        if (buffer.buffer != VK_NULL_HANDLE) {
            calls_.destroy_buffer(device_, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            calls_.free_memory(device_, buffer.memory, nullptr);
        }
        buffer = Buffer{};
    }

    void release(Structure& structure) const {
        if (structure.structure != VK_NULL_HANDLE) {
            calls_.destroy_structure(device_, structure.structure, nullptr);
        }
        release(structure.storage);
        structure = Structure{};
    }

    void release(MeshStructure& mesh) const {
        release(mesh.built);
        release(mesh.vertices);
        release(mesh.indices);
    }

    // A bake's buffers, structures and descriptors, let go of when it ends.
    struct Bake {
        explicit Bake(VulkanRaysImpl& rays) : rays_(rays) {}
        Bake(const Bake&) = delete;
        Bake& operator=(const Bake&) = delete;
        Bake(Bake&&) = delete;
        Bake& operator=(Bake&&) = delete;
        ~Bake() {
            SDL_WaitForGPUIdle(rays_.sdl_);
            for (Buffer& buffer : buffers) {
                rays_.release(buffer);
            }
            for (Structure& structure : structures) {
                rays_.release(structure);
            }
            if (pool != VK_NULL_HANDLE) {
                rays_.calls_.destroy_pool(rays_.device_, pool, nullptr);
            }
        }

        // A buffer the bake keeps until it ends.
        Result<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
            auto made = rays_.make_buffer(size, usage, host);
            if (made) {
                buffers.push_back(made.value());
            }
            return made;
        }

        VulkanRaysImpl& rays_;
        std::vector<Buffer> buffers;
        std::vector<Structure> structures;
        VkDescriptorPool pool = VK_NULL_HANDLE;
    };

    // Runs record on a command buffer of its own, submitted and waited for.
    template <typename F>
    Result<void> run_commands(F&& record) {
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(sdl_);
        if (commands == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("acquiring a command buffer for a bake failed: ") +
                             SDL_GetError()};
        }
        auto* native = static_cast<VkCommandBuffer>(SDL_SlashGPUNativeCommandBuffer(commands));
        if (native == VK_NULL_HANDLE) {
            SDL_CancelGPUCommandBuffer(commands);
            return Error{ErrorCode::PlatformFailure,
                         "a bake's command buffer has no Vulkan command buffer"};
        }
        if (auto recorded = record(native); !recorded) {
            // Commands recorded through Vulkan can't be taken back, so they run, harmlessly.
            if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands)) {
                SDL_WaitForGPUFences(sdl_, true, &fence, 1);
                SDL_ReleaseGPUFence(sdl_, fence);
            }
            return recorded;
        }
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (fence == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("submitting a bake's commands failed: ") + SDL_GetError()};
        }
        const bool waited = SDL_WaitForGPUFences(sdl_, true, &fence, 1);
        SDL_ReleaseGPUFence(sdl_, fence);
        if (!waited) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("waiting for a bake's commands failed: ") + SDL_GetError()};
        }
        return {};
    }

    Result<void> make_bake_pipelines() {
        if (bake_pipelines_[0] != VK_NULL_HANDLE) {
            return {};
        }
        // By binding: the structure, then shaders/lightmap_light.comp's buffers.
        constexpr std::pair<VkDescriptorType, std::uint32_t> kStorage{
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        return make_pipeline({{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage,
                              kStorage},
                             shaders::kLightmapLightSpirv, sizeof(shaders::kLightmapLightSpirv),
                             bake_set_layout_, bake_pipeline_layout_, bake_pipelines_,
                             "lightmap's");
    }

    SDL_GPUDevice* sdl_;
    VkPhysicalDevice physical_;
    VkDevice device_;
    Calls calls_;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkDeviceSize scratch_alignment_ = 256;
    VkDeviceSize max_storage_range_ = 1U << 27U;
    VkSampler sampler_ = VK_NULL_HANDLE;
    // Linear or (1) nearest, clamped or (2) repeating.
    std::array<VkSampler, 4> texture_samplers_{};
    VkSampler environment_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout light_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout light_pipeline_layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, kLightPasses> light_pipelines_{};
    VkDescriptorSetLayout bake_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout bake_pipeline_layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, kBakePasses> bake_pipelines_{};
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots_{};
    std::size_t next_slot_ = 0;
    std::uint64_t frame_ = 0;
    std::map<MeshKey, MeshStructure> meshes_;
    std::vector<MeshStructure> retired_;
    // Path-traced targets' sums, by RayPaths::target, and buffers frames in flight may still use.
    struct PathSums {
        Buffer buffer;
        std::uint64_t used = 0;
    };
    std::map<std::uint64_t, PathSums> path_sums_;
    std::vector<Buffer> retired_buffers_;
    Buffer mesh_scratch_;
};

} // namespace

struct VulkanRayOptions::Storage {
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR structures{};
    VkPhysicalDeviceRayQueryFeaturesKHR queries{};
    std::array<const char*, 3> extensions{VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                          VK_KHR_RAY_QUERY_EXTENSION_NAME};
    SDL_GPUVulkanOptions options{};
};

VulkanRayOptions::VulkanRayOptions() : storage_(std::make_unique<Storage>()) {
    Storage& s = *storage_;
    s.vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    s.vulkan12.bufferDeviceAddress = VK_TRUE;
    // The light's shader picks each draw's textures from an array.
    s.vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    s.vulkan12.pNext = &s.structures;
    s.structures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    s.structures.accelerationStructure = VK_TRUE;
    s.structures.pNext = &s.queries;
    s.queries.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    s.queries.rayQuery = VK_TRUE;
    s.options.vulkan_api_version = VK_API_VERSION_1_2;
    s.options.feature_list = &s.vulkan12;
    s.options.device_extension_count = static_cast<Uint32>(s.extensions.size());
    s.options.device_extension_names = s.extensions.data();
}

VulkanRayOptions::~VulkanRayOptions() = default;

void* VulkanRayOptions::options() const {
    return &storage_->options;
}

std::unique_ptr<VulkanRays> make_vulkan_rays(SDL_GPUDevice* device) {
    if (device == nullptr || std::strcmp(SDL_GetGPUDeviceDriver(device), "vulkan") != 0) {
        return nullptr;
    }
    void* instance = nullptr;
    void* physical = nullptr;
    void* logical = nullptr;
    void* queue = nullptr;
    Uint32 family = 0;
    if (!SDL_SlashGPUNativeDevice(device, &instance, &physical, &logical, &queue, &family)) {
        return nullptr;
    }
    Calls calls;
    if (!load_calls(static_cast<VkInstance>(instance), static_cast<VkDevice>(logical), calls)) {
        return nullptr;
    }
    auto rays = std::make_unique<VulkanRaysImpl>(device, static_cast<VkInstance>(instance),
                                                 static_cast<VkPhysicalDevice>(physical),
                                                 static_cast<VkDevice>(logical), calls);
    if (auto ready = rays->init(); !ready) {
        log::warn("ray tracing on Vulkan doesn't start; drawing shadows from shadow maps",
                  {{"error", ready.error().message}});
        return nullptr;
    }
    return rays;
}

} // namespace slash::render::detail

#else

namespace slash::render::detail {

struct VulkanRayOptions::Storage {};
VulkanRayOptions::VulkanRayOptions() = default;
VulkanRayOptions::~VulkanRayOptions() = default;
void* VulkanRayOptions::options() const {
    return nullptr;
}

std::unique_ptr<VulkanRays> make_vulkan_rays(SDL_GPUDevice* /*device*/) {
    return nullptr;
}

} // namespace slash::render::detail

#endif
