// Ray tracing on Metal (gpu_rays.hpp): the shaders' MSL, made by slash-ray-msl with their set 0
// as an argument buffer, run with Metal's own encoders in SDL's command buffers. Built with ARC.
#include "gpu_rays.hpp"
#include "lightmap_light_msl.hpp"
#include "native.hpp"
#include "ray_light_msl.hpp"
#include "ray_records.hpp"
#include "ray_shadows_msl.hpp"

#include "slash/core/log.hpp"

#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

// What cmake/patches/sdl-ray-tracing.cmake adds to SDL3's GPU API: a buffer's MTLBuffer.
extern "C" {
void* SDL_SlashGPUNativeBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer);
}

namespace slash::render::detail {

namespace {

// Frames whose rays can be in flight at once, each with its own argument buffers, uniforms and
// top-level structure; a frame waits for the GPU when all are busy.
constexpr std::size_t kSlots = 4;

Error metal_error(const std::string& what, NSError* error) {
    std::string message = what + " failed";
    if (error != nil) {
        message += ": ";
        message += error.localizedDescription.UTF8String;
    }
    return Error{ErrorCode::PlatformFailure, message};
}

// A binding's place in a shader's argument buffer, and its sampler's, from the ids slash-ray-msl
// gave them (k<Name>MslIds, by binding).
template <std::size_t N>
NSUInteger id_of(const int (&ids)[N][2], std::size_t binding, std::size_t which = 0) {
    return static_cast<NSUInteger>(ids[binding][which]);
}

// A mesh's bottom-level structure, with the copies of its triangles it was built from.
struct MeshStructure {
    std::uint64_t version = 0;
    id<MTLAccelerationStructure> built = nil;
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> indices = nil;
    std::uint32_t triangles = 0;
    std::uint64_t used = 0; // the frame that last used it
};

// What one frame's rays use that the next may not change while they run.
struct Slot {
    // Set while the command buffer it was recorded into runs; a buffer never submitted leaves
    // it set, which free_slot gets past after waiting for the GPU.
    std::shared_ptr<std::atomic<bool>> busy = std::make_shared<std::atomic<bool>>(false);
    id<MTLBuffer> arguments = nil;       // the shadows' shader's argument buffer
    id<MTLBuffer> uniforms = nil;        // a RayShadowsUniforms
    id<MTLBuffer> layers = nil;          // a uint a draw
    id<MTLBuffer> instances = nil;       // a user ID instance descriptor a draw
    id<MTLBuffer> scratch = nil;         // the top level's build's
    id<MTLBuffer> light_arguments = nil; // the light's shader's
    id<MTLBuffer> light_frame = nil;     // a RayLightFrame
    id<MTLBuffer> draw_records = nil;    // a RayDrawRecord a draw
    id<MTLBuffer> light_records = nil;   // a RayLightRecord a light
    id<MTLAccelerationStructure> top = nil;
    NSArray<id<MTLAccelerationStructure>>* bottoms = nil; // what top instances
};

// Made only on macOS 13 or later (make_metal_rays), where buffers have GPU addresses.
class MetalRaysImpl final : public GpuRays {
public:
    MetalRaysImpl(SDL_GPUDevice* sdl, id<MTLDevice> device) : sdl_(sdl), device_(device) {}

    MetalRaysImpl(const MetalRaysImpl&) = delete;
    MetalRaysImpl& operator=(const MetalRaysImpl&) = delete;
    MetalRaysImpl(MetalRaysImpl&&) = delete;
    MetalRaysImpl& operator=(MetalRaysImpl&&) = delete;

    // Metal's objects are released by ARC; the GPU is done with them first.
    ~MetalRaysImpl() override { SDL_WaitForGPUIdle(sdl_); }

    Result<void> init() {
        // The prepass's texels, read whole; the draws' textures, filtered and wrapped as their
        // draws sample them, at their first level as the software device's rays sample them;
        // and the sky's radiance.
        point_ = make_sampler(MTLSamplerMinMagFilterNearest, MTLSamplerAddressModeClampToEdge);
        for (std::size_t i = 0; i < texture_samplers_.size(); ++i) {
            texture_samplers_.at(i) = make_sampler(
                (i & 1U) != 0 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear,
                (i & 2U) != 0 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge);
        }
        environment_sampler_ =
            make_sampler(MTLSamplerMinMagFilterLinear, MTLSamplerAddressModeClampToEdge);
        if (point_ == nil || environment_sampler_ == nil ||
            std::any_of(texture_samplers_.begin(), texture_samplers_.end(),
                        [](id<MTLSamplerState> sampler) { return sampler == nil; })) {
            return Error{ErrorCode::PlatformFailure, "making the rays' samplers failed"};
        }

        if (auto made = make_pipelines(shaders::kRayShadowsMsl, false, &shadows_, 1,
                                       shadows_encoder_, "ray shadows'");
            !made) {
            return made;
        }
        if (auto made = make_pipelines(shaders::kRayLightMsl, true, light_pipelines_.data(),
                                       light_pipelines_.size(), light_encoder_, "ray light's");
            !made) {
            return made;
        }
        for (Slot& slot : slots_) {
            slot.arguments = shared_buffer(shadows_encoder_.encodedLength);
            slot.light_arguments = shared_buffer(light_encoder_.encodedLength);
            slot.uniforms = shared_buffer(sizeof(RayShadowsUniforms));
            slot.light_frame = shared_buffer(sizeof(RayLightFrame));
            if (slot.arguments == nil || slot.light_arguments == nil || slot.uniforms == nil ||
                slot.light_frame == nil) {
                return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
            }
        }
        return {};
    }

    Result<void> trace(SDL_GPUCommandBuffer* commands, std::span<const RayDraw> draws,
                       const RayWork& work, const RayShadowFrame& frame,
                       const RayTargets& targets) override {
        id<MTLCommandBuffer> buffer =
            (__bridge id<MTLCommandBuffer>)SDL_SlashGPUNativeCommandBuffer(commands);
        if (buffer == nil) {
            return Error{ErrorCode::PlatformFailure,
                         "the frame's command buffer has no Metal command buffer"};
        }
        // Whether rays from the camera's side are traced, which shade what they meet.
        const bool lit = (work.reflection || work.gi || work.paths) && work.lighting != nullptr;
        ++frame_;
        Slot& slot = free_slot();
        release_unused();
        // The slot is the GPU's until the command buffer has run, whatever is recorded into it.
        slot.busy->store(true);
        std::shared_ptr<std::atomic<bool>> busy = slot.busy;
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> /*done*/) {
          busy->store(false);
        }];

        id<MTLTexture> depth = native_texture(targets.depth);
        id<MTLTexture> normals = native_texture(targets.normals);
        if (depth == nil || normals == nil) {
            return Error{ErrorCode::PlatformFailure,
                         "the prepass's textures have no Metal textures"};
        }

        // The meshes' structures: built again for meshes that are new or changed, from copies
        // of their buffers. Without rays from the camera's side only the draws that cast shadows
        // take part. Metal tracks SDL's buffers, so the copies wait for the passes that wrote
        // them, and the builds and traces for the copies.
        std::vector<const MeshStructure*> placed;
        placed.reserve(draws.size());
        std::vector<std::pair<MeshStructure*, const RayDraw*>> to_build;
        id<MTLBlitCommandEncoder> blit = nil;
        for (const RayDraw& draw : draws) {
            if (draw.index_count < 3 || draw.vertex_count == 0 || ray_mask(draw, lit) == 0) {
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
                if (blit == nil) {
                    blit = [buffer blitCommandEncoder];
                }
                auto copy = copy_mesh(blit, draw);
                if (!copy) {
                    [blit endEncoding];
                    return copy.error();
                }
                found = meshes_.emplace(key, copy.value()).first;
                to_build.emplace_back(&found->second, &draw);
            }
            found->second.used = frame_;
            placed.push_back(&found->second);
        }
        // A path-traced target's sums, kept from frame to frame, started over at zero when
        // asked; other frames bind a buffer the shader leaves alone.
        id<MTLBuffer> sums = nil;
        if (lit && work.paths) {
            const NSUInteger bytes = NSUInteger{frame.width} * frame.height * 16;
            PathSums& kept = path_sums_[work.paths->target];
            bool cleared = work.paths->restart;
            if (kept.buffer == nil || kept.buffer.length != bytes) {
                if (kept.buffer != nil) {
                    retired_buffers_.push_back(kept.buffer);
                }
                kept.buffer = [device_ newBufferWithLength:bytes
                                                   options:MTLResourceStorageModePrivate];
                if (kept.buffer == nil) {
                    if (blit != nil) {
                        [blit endEncoding];
                    }
                    return Error{ErrorCode::PlatformFailure, "making a path trace's sums failed"};
                }
                cleared = true;
            }
            if (cleared) {
                if (blit == nil) {
                    blit = [buffer blitCommandEncoder];
                }
                [blit fillBuffer:kept.buffer range:NSMakeRange(0, bytes) value:0];
            }
            kept.used = frame_;
            sums = kept.buffer;
        }
        if (blit != nil) {
            [blit endEncoding];
        }

        id<MTLAccelerationStructureCommandEncoder> builds =
            [buffer accelerationStructureCommandEncoder];
        if (auto built = build_meshes(builds, to_build); !built) {
            [builds endEncoding];
            return built;
        }

        // The frame's instances, a draw each, and its top-level structure.
        std::vector<MTLAccelerationStructureUserIDInstanceDescriptor> instances;
        std::vector<std::uint32_t> layers;
        std::vector<std::size_t> instanced; // each instance's draw
        NSMutableArray<id<MTLAccelerationStructure>>* bottoms = [NSMutableArray array];
        std::map<const MeshStructure*, std::uint32_t> bottom_index;
        bool every_layer = true; // every draw that casts shadows does for every light
        for (std::size_t i = 0; i < draws.size(); ++i) {
            if (placed[i] == nullptr) {
                continue;
            }
            const auto [at, added] =
                bottom_index.emplace(placed[i], static_cast<std::uint32_t>(bottom_index.size()));
            if (added) {
                [bottoms addObject:placed[i]->built];
            }
            const Mat4& m = draws[i].transform;
            MTLAccelerationStructureUserIDInstanceDescriptor instance{};
            for (std::size_t column = 0; column < 4; ++column) {
                for (std::size_t row = 0; row < 3; ++row) {
                    instance.transformationMatrix.columns[column].elements[row] =
                        m.at((column * 4) + row);
                }
            }
            instance.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
            instance.mask = ray_mask(draws[i], lit);
            instance.intersectionFunctionTableOffset = 0;
            instance.accelerationStructureIndex = at->second;
            instance.userID = static_cast<std::uint32_t>(layers.size());
            instances.push_back(instance);
            layers.push_back(draws[i].layers);
            instanced.push_back(i);
            if ((instance.mask & kRayCastMask) != 0) {
                every_layer = every_layer && draws[i].layers != 0;
            }
        }
        const auto built = build_top(builds, slot, instances, layers, bottoms);
        [builds endEncoding];
        if (!built) {
            return built;
        }

        id<MTLComputeCommandEncoder> compute = [buffer computeCommandEncoder];
        // Everything the argument buffers reach, and the meshes' copies the light's shader reads
        // at their GPU addresses.
        [compute useResource:slot.top usage:MTLResourceUsageRead];
        for (id<MTLAccelerationStructure> bottom in slot.bottoms) {
            [compute useResource:bottom usage:MTLResourceUsageRead];
        }
        [compute useResource:depth usage:MTLResourceUsageRead];
        [compute useResource:normals usage:MTLResourceUsageRead];
        Result<void> traced{};
        if (!work.lights.empty() || work.occlusion) {
            traced =
                trace_shadows(compute, slot, work, frame, targets, depth, normals, every_layer);
        }
        if (traced && lit) {
            traced = trace_light(compute, slot, draws, placed, instanced, work, frame, targets,
                                 depth, normals, sums);
        }
        [compute endEncoding];
        if (!traced) {
            return traced;
        }
        return {};
    }

    Result<LightmapTraced> trace_lightmap(const LightmapTrace& trace) override {
        if (!lightmap_trace_fits(trace)) {
            return Error{ErrorCode::InvalidArgument, "a lightmap trace's layers don't fit it"};
        }
        const std::size_t texel_bytes = lightmap_texel_bytes(trace);
        const std::size_t probe_bytes = lightmap_probe_bytes(trace);
        if (texel_bytes > device_.maxBufferLength || probe_bytes > device_.maxBufferLength) {
            return Error{ErrorCode::PlatformFailure,
                         "the lightmap's atlas is larger than this GPU's buffers"};
        }
        if (auto made = make_pipelines(shaders::kLightmapLightMsl, true, bake_pipelines_.data(),
                                       bake_pipelines_.size(), bake_encoder_, "lightmap light's");
            !made) {
            return made.error();
        }

        // The inputs, in memory the CPU and GPU share; the outputs, cleared.
        LightmapInputs given;
        lightmap_inputs(trace, given);
        std::array<id<MTLBuffer>, 10> in{};
        for (std::size_t i = 0; i < in.size(); ++i) {
            const LightmapInputs::Input& input = given.inputs.at(i);
            in.at(i) = shared_buffer(input.bytes);
            if (in.at(i) == nil) {
                return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
            }
            if (input.bytes > 0) {
                std::memcpy(in.at(i).contents, input.data, input.bytes);
            }
        }
        // light_source, accum, probe sums.
        const std::array<std::size_t, 3> output_bytes{texel_bytes, texel_bytes, probe_bytes};
        std::array<id<MTLBuffer>, 3> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out.at(i) = shared_buffer(output_bytes.at(i));
            if (out.at(i) == nil) {
                return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
            }
            std::memset(out.at(i).contents, 0, out.at(i).length);
        }
        id<MTLBuffer> params_buffer = shared_buffer(sizeof(LightmapParams));
        id<MTLBuffer> instance_buffer =
            shared_buffer(sizeof(MTLAccelerationStructureUserIDInstanceDescriptor));
        id<MTLBuffer> arguments = shared_buffer(bake_encoder_.encodedLength);
        if (params_buffer == nil || instance_buffer == nil || arguments == nil) {
            return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
        }

        // One bottom-level structure of every triangle, and a top-level one with it once.
        MTLAccelerationStructureTriangleGeometryDescriptor* geometry =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        geometry.vertexBuffer = in[0];
        geometry.vertexStride = sizeof(BakeVertex);
        geometry.indexBuffer = in[1];
        geometry.indexType = MTLIndexTypeUInt32;
        geometry.triangleCount = trace.triangles.size();
        geometry.opaque = NO; // so the shader tests each triangle as the CPU does
        MTLPrimitiveAccelerationStructureDescriptor* bottom =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        bottom.geometryDescriptors = @[ geometry ];
        const MTLAccelerationStructureSizes bottom_sizes =
            [device_ accelerationStructureSizesWithDescriptor:bottom];
        id<MTLAccelerationStructure> bottom_structure =
            [device_ newAccelerationStructureWithSize:bottom_sizes.accelerationStructureSize];

        MTLAccelerationStructureUserIDInstanceDescriptor instance{};
        instance.transformationMatrix.columns[0].elements[0] = 1.0F;
        instance.transformationMatrix.columns[1].elements[1] = 1.0F;
        instance.transformationMatrix.columns[2].elements[2] = 1.0F;
        instance.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
        instance.mask = 0xFF;
        std::memcpy(instance_buffer.contents, &instance, sizeof(instance));
        MTLInstanceAccelerationStructureDescriptor* top =
            [MTLInstanceAccelerationStructureDescriptor descriptor];
        top.instancedAccelerationStructures = @[ bottom_structure ];
        top.instanceCount = 1;
        top.instanceDescriptorBuffer = instance_buffer;
        top.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
        const MTLAccelerationStructureSizes top_sizes =
            [device_ accelerationStructureSizesWithDescriptor:top];
        id<MTLAccelerationStructure> top_structure =
            [device_ newAccelerationStructureWithSize:top_sizes.accelerationStructureSize];
        id<MTLBuffer> scratch = [device_
            newBufferWithLength:std::max<NSUInteger>(16,
                                                     std::max(bottom_sizes.buildScratchBufferSize,
                                                              top_sizes.buildScratchBufferSize))
                        options:MTLResourceStorageModePrivate];
        if (bottom_structure == nil || top_structure == nil || scratch == nil) {
            return Error{ErrorCode::PlatformFailure,
                         "making a bake's acceleration structures failed"};
        }
        if (auto set_up = run_commands([&](id<MTLCommandBuffer> commands) -> Result<void> {
                id<MTLAccelerationStructureCommandEncoder> builds =
                    [commands accelerationStructureCommandEncoder];
                [builds buildAccelerationStructure:bottom_structure
                                        descriptor:bottom
                                     scratchBuffer:scratch
                               scratchBufferOffset:0];
                // The top level's build waits for the bottom's, which Metal tracks, and reuses
                // its scratch.
                [builds buildAccelerationStructure:top_structure
                                        descriptor:top
                                     scratchBuffer:scratch
                               scratchBufferOffset:0];
                [builds endEncoding];
                return {};
            });
            !set_up) {
            return set_up.error();
        }

        // The argument buffer: the structure, Params, the inputs and the outputs, by binding.
        const auto& ids = shaders::kLightmapLightMslIds;
        [bake_encoder_ setArgumentBuffer:arguments offset:0];
        [bake_encoder_ setAccelerationStructure:top_structure atIndex:id_of(ids, 0)];
        const std::array<id<MTLBuffer>, 14> bound{params_buffer, in[0],  in[1], in[2], in[3],
                                                  in[4],         in[5],  in[6], in[7], in[8],
                                                  out[0],        out[1], in[9], out[2]};
        for (std::size_t i = 0; i < bound.size(); ++i) {
            [bake_encoder_ setBuffer:bound.at(i) offset:0 atIndex:id_of(ids, 1 + i)];
        }

        if (auto ran = run_lightmap_passes(
                trace, given.lights.size(),
                [&](std::size_t pass, std::uint32_t count,
                    const LightmapParams& params) -> Result<void> {
                    std::memcpy(params_buffer.contents, &params, sizeof(params));
                    return run_commands([&](id<MTLCommandBuffer> commands) -> Result<void> {
                        id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
                        [compute setComputePipelineState:bake_pipelines_.at(pass)];
                        [compute setBuffer:arguments offset:0 atIndex:0];
                        [compute useResource:top_structure usage:MTLResourceUsageRead];
                        [compute useResource:bottom_structure usage:MTLResourceUsageRead];
                        for (id<MTLBuffer> used : bound) {
                            [compute useResource:used
                                           usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                        }
                        [compute dispatchThreadgroups:MTLSizeMake((count + kBakeGroupSize - 1) /
                                                                      kBakeGroupSize,
                                                                  1, 1)
                                threadsPerThreadgroup:MTLSizeMake(kBakeGroupSize, 1, 1)];
                        [compute endEncoding];
                        return {};
                    });
                });
            !ran) {
            return ran.error();
        }
        return lightmap_traced(trace, out[0].contents, out[1].contents, out[2].contents);
    }

private:
    using MeshKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;

    struct PathSums {
        id<MTLBuffer> buffer = nil;
        std::uint64_t used = 0;
    };

    id<MTLSamplerState> make_sampler(MTLSamplerMinMagFilter filter, MTLSamplerAddressMode wrap) {
        MTLSamplerDescriptor* info = [MTLSamplerDescriptor new];
        info.minFilter = filter;
        info.magFilter = filter;
        info.mipFilter = MTLSamplerMipFilterNotMipmapped;
        info.sAddressMode = wrap;
        info.tAddressMode = wrap;
        info.rAddressMode = wrap;
        info.supportArgumentBuffers = YES;
        return [device_ newSamplerStateWithDescriptor:info];
    }

    id<MTLBuffer> shared_buffer(std::size_t bytes) {
        return [device_ newBufferWithLength:std::max<std::size_t>(bytes, 16)
                                    options:MTLResourceStorageModeShared];
    }

    // Grows buffer to hold bytes, in shared or private memory.
    bool fit(id<MTLBuffer> __strong& buffer, std::size_t bytes, MTLResourceOptions options) {
        if (buffer != nil && buffer.length >= bytes) {
            return true;
        }
        const std::size_t size =
            std::max<std::size_t>({bytes, 16, buffer != nil ? buffer.length * 2 : 0});
        if (buffer != nil) {
            retired_buffers_.push_back(buffer);
        }
        buffer = [device_ newBufferWithLength:size options:options];
        return buffer != nil;
    }

    // The library of a shader's MSL, and a pipeline of its main0 for each value of its kPass
    // constant (or one without), with the encoder of its argument buffer. Made once.
    Result<void> make_pipelines(const char* source, bool passes,
                                id<MTLComputePipelineState> __strong* pipelines, std::size_t count,
                                id<MTLArgumentEncoder> __strong& encoder, const char* what) {
        if (pipelines[0] != nil) {
            return {};
        }
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        // The CPU twins' math, unfused and exact.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        NSError* error = nil;
        id<MTLLibrary> library = [device_ newLibraryWithSource:@(source)
                                                       options:options
                                                         error:&error];
        if (library == nil) {
            return metal_error(std::string("compiling the ") + what + " shader", error);
        }
        for (std::size_t pass = 0; pass < count; ++pass) {
            id<MTLFunction> function = nil;
            if (passes) {
                MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
                const auto value = static_cast<std::uint32_t>(pass);
                [values setConstantValue:&value type:MTLDataTypeUInt atIndex:0];
                function = [library newFunctionWithName:@"main0"
                                         constantValues:values
                                                  error:&error];
            } else {
                function = [library newFunctionWithName:@"main0"];
            }
            if (function == nil) {
                return metal_error(std::string("making the ") + what + " shader's function", error);
            }
            pipelines[pass] = [device_ newComputePipelineStateWithFunction:function error:&error];
            if (pipelines[pass] == nil) {
                return metal_error(std::string("making the ") + what + " pipeline", error);
            }
            if (encoder == nil) {
                encoder = [function newArgumentEncoderWithBufferIndex:0];
                if (encoder == nil) {
                    return Error{ErrorCode::PlatformFailure,
                                 std::string("the ") + what + " shader has no argument buffer"};
                }
            }
        }
        return {};
    }

    id<MTLTexture> native_texture(SDL_GPUTexture* texture) {
        if (texture == nullptr) {
            return nil;
        }
        void* view = nullptr;
        Uint32 format = 0;
        return (__bridge id<MTLTexture>)SDL_SlashGPUNativeTexture(sdl_, texture, &view, &format);
    }

    Slot& free_slot() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (std::size_t i = 0; i < kSlots; ++i) {
                Slot& slot = slots_.at((next_slot_ + i) % kSlots);
                if (!slot.busy->load()) {
                    next_slot_ = (next_slot_ + i + 1) % kSlots;
                    return slot;
                }
            }
            SDL_WaitForGPUIdle(sdl_);
        }
        // Recorded into a command buffer that was never submitted: nothing uses it.
        Slot& slot = slots_.at(next_slot_);
        next_slot_ = (next_slot_ + 1) % kSlots;
        slot.busy = std::make_shared<std::atomic<bool>>(false);
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
        const bool idle = std::none_of(slots_.begin(), slots_.end(),
                                       [](const Slot& slot) { return slot.busy->load(); });
        if (idle) {
            retired_.clear();
            retired_buffers_.clear();
        }
    }

    Result<MeshStructure> copy_mesh(id<MTLBlitCommandEncoder> blit, const RayDraw& draw) {
        id<MTLBuffer> vertices =
            (__bridge id<MTLBuffer>)SDL_SlashGPUNativeBuffer(sdl_, draw.vertices);
        id<MTLBuffer> indices =
            (__bridge id<MTLBuffer>)SDL_SlashGPUNativeBuffer(sdl_, draw.indices);
        if (vertices == nil || indices == nil) {
            return Error{ErrorCode::PlatformFailure, "a mesh's buffers have no Metal buffers"};
        }
        MeshStructure out;
        out.version = draw.version;
        out.triangles = draw.index_count / 3;
        // Read by the structure's build, and by the light's shader at their addresses.
        const NSUInteger vertex_bytes = NSUInteger{draw.vertex_count} * sizeof(Vertex3D);
        const NSUInteger index_bytes = NSUInteger{draw.index_count} * sizeof(std::uint32_t);
        out.vertices = [device_ newBufferWithLength:vertex_bytes
                                            options:MTLResourceStorageModePrivate];
        out.indices = [device_ newBufferWithLength:index_bytes
                                           options:MTLResourceStorageModePrivate];
        if (out.vertices == nil || out.indices == nil) {
            return Error{ErrorCode::PlatformFailure, "copying a mesh for rays failed"};
        }
        [blit copyFromBuffer:vertices
                 sourceOffset:0
                     toBuffer:out.vertices
            destinationOffset:0
                         size:vertex_bytes];
        [blit copyFromBuffer:indices
                 sourceOffset:NSUInteger{draw.first_index} * sizeof(std::uint32_t)
                     toBuffer:out.indices
            destinationOffset:0
                         size:index_bytes];
        return out;
    }

    // Builds the bottom-level structures of meshes copied this frame.
    Result<void> build_meshes(id<MTLAccelerationStructureCommandEncoder> builds,
                              std::span<const std::pair<MeshStructure*, const RayDraw*>> meshes) {
        if (meshes.empty()) {
            return {};
        }
        std::vector<MTLPrimitiveAccelerationStructureDescriptor*> descriptors;
        std::vector<NSUInteger> scratch_at;
        NSUInteger scratch = 0;
        for (const auto& [mesh, draw] : meshes) {
            MTLAccelerationStructureTriangleGeometryDescriptor* geometry =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            geometry.vertexBuffer = mesh->vertices;
            geometry.vertexStride = sizeof(Vertex3D);
            geometry.indexBuffer = mesh->indices;
            geometry.indexType = MTLIndexTypeUInt32;
            geometry.triangleCount = mesh->triangles;
            geometry.opaque = NO; // so rays can ask which draw casts for which light
            MTLPrimitiveAccelerationStructureDescriptor* descriptor =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            descriptor.geometryDescriptors = @[ geometry ];
            const MTLAccelerationStructureSizes sizes =
                [device_ accelerationStructureSizesWithDescriptor:descriptor];
            mesh->built =
                [device_ newAccelerationStructureWithSize:sizes.accelerationStructureSize];
            if (mesh->built == nil) {
                return Error{ErrorCode::PlatformFailure,
                             "making a mesh's acceleration structure failed"};
            }
            descriptors.push_back(descriptor);
            scratch_at.push_back(scratch);
            scratch += (sizes.buildScratchBufferSize + 255) & ~NSUInteger{255};
        }
        if (!fit(mesh_scratch_, scratch, MTLResourceStorageModePrivate)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' scratch buffer failed"};
        }
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            [builds buildAccelerationStructure:meshes[i].first->built
                                    descriptor:descriptors[i]
                                 scratchBuffer:mesh_scratch_
                           scratchBufferOffset:scratch_at[i]];
        }
        return {};
    }

    // Builds slot's top-level structure over instances, and fills its draws' layers. A frame
    // without instances gets one that no ray meets, so the shaders always have a structure.
    Result<void> build_top(id<MTLAccelerationStructureCommandEncoder> builds, Slot& slot,
                           std::vector<MTLAccelerationStructureUserIDInstanceDescriptor>& instances,
                           std::span<const std::uint32_t> layers,
                           NSMutableArray<id<MTLAccelerationStructure>>* bottoms) {
        if (instances.empty()) {
            if (auto empty = empty_structure(builds); !empty) {
                return empty;
            }
            MTLAccelerationStructureUserIDInstanceDescriptor none{};
            none.mask = 0;
            none.accelerationStructureIndex = 0;
            instances.push_back(none);
            [bottoms addObject:empty_];
        }
        const std::size_t instance_bytes =
            instances.size() * sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
        if (!fit(slot.instances, instance_bytes, MTLResourceStorageModeShared) ||
            !fit(slot.layers, std::max<std::size_t>(1, layers.size()) * sizeof(std::uint32_t),
                 MTLResourceStorageModeShared)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
        }
        std::memcpy(slot.instances.contents, instances.data(), instance_bytes);
        if (!layers.empty()) {
            std::memcpy(slot.layers.contents, layers.data(), layers.size_bytes());
        }
        MTLInstanceAccelerationStructureDescriptor* descriptor =
            [MTLInstanceAccelerationStructureDescriptor descriptor];
        descriptor.instancedAccelerationStructures = bottoms;
        descriptor.instanceCount = instances.size();
        descriptor.instanceDescriptorBuffer = slot.instances;
        descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
        const MTLAccelerationStructureSizes sizes =
            [device_ accelerationStructureSizesWithDescriptor:descriptor];
        if (slot.top == nil || slot.top.size < sizes.accelerationStructureSize) {
            slot.top = [device_ newAccelerationStructureWithSize:sizes.accelerationStructureSize];
            if (slot.top == nil) {
                return Error{ErrorCode::PlatformFailure,
                             "making the rays' top-level structure failed"};
            }
        }
        if (!fit(slot.scratch, sizes.buildScratchBufferSize, MTLResourceStorageModePrivate)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' scratch buffer failed"};
        }
        slot.bottoms = [bottoms copy];
        [builds buildAccelerationStructure:slot.top
                                descriptor:descriptor
                             scratchBuffer:slot.scratch
                       scratchBufferOffset:0];
        return {};
    }

    // A structure of one triangle with no area, built once.
    Result<void> empty_structure(id<MTLAccelerationStructureCommandEncoder> builds) {
        if (empty_ != nil) {
            return {};
        }
        id<MTLBuffer> points = shared_buffer(sizeof(float) * 9);
        id<MTLBuffer> corners = shared_buffer(sizeof(std::uint32_t) * 3);
        if (points == nil || corners == nil) {
            return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
        }
        std::memset(points.contents, 0, points.length);
        std::memset(corners.contents, 0, corners.length);
        MTLAccelerationStructureTriangleGeometryDescriptor* geometry =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        geometry.vertexBuffer = points;
        geometry.vertexStride = sizeof(float) * 3;
        geometry.indexBuffer = corners;
        geometry.indexType = MTLIndexTypeUInt32;
        geometry.triangleCount = 1;
        MTLPrimitiveAccelerationStructureDescriptor* descriptor =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        descriptor.geometryDescriptors = @[ geometry ];
        const MTLAccelerationStructureSizes sizes =
            [device_ accelerationStructureSizesWithDescriptor:descriptor];
        id<MTLAccelerationStructure> built =
            [device_ newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        id<MTLBuffer> scratch =
            [device_ newBufferWithLength:std::max<NSUInteger>(16, sizes.buildScratchBufferSize)
                                 options:MTLResourceStorageModePrivate];
        if (built == nil || scratch == nil) {
            return Error{ErrorCode::PlatformFailure,
                         "making the rays' empty acceleration structure failed"};
        }
        [builds buildAccelerationStructure:built
                                descriptor:descriptor
                             scratchBuffer:scratch
                       scratchBufferOffset:0];
        empty_ = built;
        empty_buffers_ = @[ points, corners, scratch ];
        return {};
    }

    // The visibilities of the lights with ray slots and the ambient occlusion, into targets'
    // first three outputs.
    Result<void> trace_shadows(id<MTLComputeCommandEncoder> compute, Slot& slot,
                               const RayWork& work, const RayShadowFrame& frame,
                               const RayTargets& targets, id<MTLTexture> depth,
                               id<MTLTexture> normals, bool every_layer) {
        const RayShadowsUniforms uniforms = ray_shadows_uniforms(work, frame, every_layer);
        std::memcpy(slot.uniforms.contents, &uniforms, sizeof(uniforms));
        std::array<id<MTLTexture>, kRayShadowOutputs> outputs{};
        for (std::size_t i = 0; i < kRayShadowOutputs; ++i) {
            outputs.at(i) = native_texture(targets.outputs.at(i));
            if (outputs.at(i) == nil) {
                return Error{ErrorCode::PlatformFailure, "a texture rays use has no Metal texture"};
            }
        }
        // By binding: the structure, depth, normals, two visibilities, the uniforms, the draws'
        // layers and the occlusion.
        const auto& ids = shaders::kRayShadowsMslIds;
        [shadows_encoder_ setArgumentBuffer:slot.arguments offset:0];
        [shadows_encoder_ setAccelerationStructure:slot.top atIndex:id_of(ids, 0)];
        [shadows_encoder_ setTexture:depth atIndex:id_of(ids, 1)];
        [shadows_encoder_ setSamplerState:point_ atIndex:id_of(ids, 1, 1)];
        [shadows_encoder_ setTexture:normals atIndex:id_of(ids, 2)];
        [shadows_encoder_ setSamplerState:point_ atIndex:id_of(ids, 2, 1)];
        [shadows_encoder_ setTexture:outputs[0] atIndex:id_of(ids, 3)];
        [shadows_encoder_ setTexture:outputs[1] atIndex:id_of(ids, 4)];
        [shadows_encoder_ setBuffer:slot.uniforms offset:0 atIndex:id_of(ids, 5)];
        [shadows_encoder_ setBuffer:slot.layers offset:0 atIndex:id_of(ids, 6)];
        [shadows_encoder_ setTexture:outputs[2] atIndex:id_of(ids, 7)];

        [compute useResource:slot.uniforms usage:MTLResourceUsageRead];
        [compute useResource:slot.layers usage:MTLResourceUsageRead];
        for (id<MTLTexture> output : outputs) {
            [compute useResource:output usage:MTLResourceUsageWrite];
        }
        [compute setComputePipelineState:shadows_];
        [compute setBuffer:slot.arguments offset:0 atIndex:0];
        dispatch_frame(compute, frame);
        return {};
    }

    // The reflections and the ambient light, into targets' last three outputs: the draws each
    // instance stands for as records the shader shades them by, their textures, the lights and
    // the environment.
    Result<void> trace_light(id<MTLComputeCommandEncoder> compute, Slot& slot,
                             std::span<const RayDraw> draws,
                             std::span<const MeshStructure* const> placed,
                             std::span<const std::size_t> instanced, const RayWork& work,
                             const RayShadowFrame& frame, const RayTargets& targets,
                             id<MTLTexture> depth, id<MTLTexture> normals, id<MTLBuffer> sums) {
        const RayLighting& lighting = *work.lighting;
        RayTextureTable textures;
        std::vector<RayDrawRecord> records;
        records.reserve(instanced.size());
        for (const std::size_t i : instanced) {
            records.push_back(ray_draw_record(draws[i], placed[i]->vertices.gpuAddress,
                                              placed[i]->indices.gpuAddress, textures));
        }
        const std::vector<RayLightRecord> light_records = ray_light_records(lighting);
        if (!fit(slot.draw_records,
                 std::max<std::size_t>(1, records.size()) * sizeof(RayDrawRecord),
                 MTLResourceStorageModeShared) ||
            !fit(slot.light_records,
                 std::max<std::size_t>(1, light_records.size()) * sizeof(RayLightRecord),
                 MTLResourceStorageModeShared)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
        }
        if (!records.empty()) {
            std::memcpy(slot.draw_records.contents, records.data(),
                        records.size() * sizeof(RayDrawRecord));
        }
        if (!light_records.empty()) {
            std::memcpy(slot.light_records.contents, light_records.data(),
                        light_records.size() * sizeof(RayLightRecord));
        }
        const RayLightFrame uniforms = ray_light_frame(work, frame, light_records.size());
        std::memcpy(slot.light_frame.contents, &uniforms, sizeof(uniforms));

        std::array<id<MTLTexture>, kRayLightOutputs> outputs{};
        for (std::size_t i = 0; i < kRayLightOutputs; ++i) {
            outputs.at(i) = native_texture(targets.outputs.at(kRayShadowOutputs + i));
        }
        id<MTLTexture> environment = native_texture(targets.environment);
        id<MTLTexture> white = native_texture(targets.white);
        id<MTLTexture> background =
            native_texture(targets.background != nullptr ? targets.background : targets.white);
        if (environment == nil || white == nil || background == nil ||
            std::any_of(outputs.begin(), outputs.end(),
                        [](id<MTLTexture> output) { return output == nil; })) {
            return Error{ErrorCode::PlatformFailure, "a texture rays use has no Metal texture"};
        }
        std::array<id<MTLTexture>, kRayTextures> sampled{};
        std::array<id<MTLSamplerState>, kRayTextures> samplers{};
        sampled.fill(white);
        samplers.fill(texture_samplers_[0]);
        const auto& used = textures.used();
        for (std::size_t i = 0; i < used.size(); ++i) {
            sampled.at(i) = native_texture(used[i].first);
            if (sampled.at(i) == nil) {
                return Error{ErrorCode::PlatformFailure, "a texture rays use has no Metal texture"};
            }
            samplers.at(i) = texture_samplers_.at(used[i].second);
        }

        // By binding: the structure, depth, normals, the reflections, the frame, the draws, the
        // lights, the environment, the textures, the ambient light as traced and as smoothed,
        // the paths' sums and the background.
        const auto& ids = shaders::kRayLightMslIds;
        [light_encoder_ setArgumentBuffer:slot.light_arguments offset:0];
        [light_encoder_ setAccelerationStructure:slot.top atIndex:id_of(ids, 0)];
        [light_encoder_ setTexture:depth atIndex:id_of(ids, 1)];
        [light_encoder_ setSamplerState:point_ atIndex:id_of(ids, 1, 1)];
        [light_encoder_ setTexture:normals atIndex:id_of(ids, 2)];
        [light_encoder_ setSamplerState:point_ atIndex:id_of(ids, 2, 1)];
        [light_encoder_ setTexture:outputs[0] atIndex:id_of(ids, 3)];
        [light_encoder_ setBuffer:slot.light_frame offset:0 atIndex:id_of(ids, 4)];
        [light_encoder_ setBuffer:slot.draw_records offset:0 atIndex:id_of(ids, 5)];
        [light_encoder_ setBuffer:slot.light_records offset:0 atIndex:id_of(ids, 6)];
        [light_encoder_ setTexture:environment atIndex:id_of(ids, 7)];
        [light_encoder_ setSamplerState:environment_sampler_ atIndex:id_of(ids, 7, 1)];
        [light_encoder_ setTextures:sampled.data()
                          withRange:NSMakeRange(id_of(ids, 8), kRayTextures)];
        [light_encoder_ setSamplerStates:samplers.data()
                               withRange:NSMakeRange(id_of(ids, 8, 1), kRayTextures)];
        [light_encoder_ setTexture:outputs[1] atIndex:id_of(ids, 9)];
        [light_encoder_ setTexture:outputs[2] atIndex:id_of(ids, 10)];
        [light_encoder_ setBuffer:(sums != nil ? sums : slot.draw_records)
                           offset:0
                          atIndex:id_of(ids, 11)];
        [light_encoder_ setTexture:background atIndex:id_of(ids, 12)];
        [light_encoder_ setSamplerState:point_ atIndex:id_of(ids, 12, 1)];

        for (id<MTLBuffer> read in @[ slot.light_frame, slot.draw_records, slot.light_records ]) {
            [compute useResource:read usage:MTLResourceUsageRead];
        }
        if (sums != nil) {
            [compute useResource:sums usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
        for (id<MTLTexture> read in @[ environment, white, background ]) {
            [compute useResource:read usage:MTLResourceUsageRead];
        }
        for (std::size_t i = 0; i < used.size(); ++i) {
            [compute useResource:sampled.at(i) usage:MTLResourceUsageRead];
        }
        for (id<MTLTexture> output : outputs) {
            [compute useResource:output usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
        for (const std::size_t i : instanced) {
            [compute useResource:placed[i]->vertices usage:MTLResourceUsageRead];
            [compute useResource:placed[i]->indices usage:MTLResourceUsageRead];
        }
        [compute setBuffer:slot.light_arguments offset:0 atIndex:0];
        const auto run = [&](std::size_t pass) {
            [compute setComputePipelineState:light_pipelines_.at(pass)];
            dispatch_frame(compute, frame);
        };
        // The smoothing reads what each pixel's neighbors traced, and the average the sums.
        const auto wait = [&] {
            [compute memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        };
        if (work.reflection) {
            run(kRayReflectPass);
        }
        if (work.gi) {
            run(kRayGiPass);
            wait();
            run(kRaySmoothPass);
        }
        if (work.paths) {
            run(kRayTracePathsPass);
            wait();
            run(kRayAveragePathsPass);
        }
        return {};
    }

    static void dispatch_frame(id<MTLComputeCommandEncoder> compute, const RayShadowFrame& frame) {
        [compute
             dispatchThreadgroups:MTLSizeMake((frame.width + kRayGroupSize - 1) / kRayGroupSize,
                                              (frame.height + kRayGroupSize - 1) / kRayGroupSize, 1)
            threadsPerThreadgroup:MTLSizeMake(kRayGroupSize, kRayGroupSize, 1)];
    }

    // Records into a command buffer of its own, submits it and waits for it.
    template <typename F>
    Result<void> run_commands(F&& record) {
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(sdl_);
        if (commands == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("acquiring a command buffer for a bake failed: ") +
                             SDL_GetError()};
        }
        id<MTLCommandBuffer> native =
            (__bridge id<MTLCommandBuffer>)SDL_SlashGPUNativeCommandBuffer(commands);
        if (native == nil) {
            SDL_CancelGPUCommandBuffer(commands);
            return Error{ErrorCode::PlatformFailure,
                         "a bake's command buffer has no Metal command buffer"};
        }
        auto recorded = record(native);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (fence == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("submitting a bake's commands failed: ") + SDL_GetError()};
        }
        const bool waited = SDL_WaitForGPUFences(sdl_, true, &fence, 1);
        SDL_ReleaseGPUFence(sdl_, fence);
        if (!recorded) {
            return recorded;
        }
        if (!waited) {
            return Error{ErrorCode::PlatformFailure,
                         std::string("waiting for a bake's commands failed: ") + SDL_GetError()};
        }
        if (native.status == MTLCommandBufferStatusError) {
            return metal_error("a bake's commands", native.error);
        }
        return {};
    }

    SDL_GPUDevice* sdl_;
    id<MTLDevice> device_;
    id<MTLSamplerState> point_ = nil;
    std::array<id<MTLSamplerState>, 4> texture_samplers_{};
    id<MTLSamplerState> environment_sampler_ = nil;
    id<MTLComputePipelineState> shadows_ = nil;
    id<MTLArgumentEncoder> shadows_encoder_ = nil;
    std::array<id<MTLComputePipelineState>, kRayLightPasses> light_pipelines_{};
    id<MTLArgumentEncoder> light_encoder_ = nil;
    std::array<id<MTLComputePipelineState>, kBakePasses> bake_pipelines_{};
    id<MTLArgumentEncoder> bake_encoder_ = nil;
    std::array<Slot, kSlots> slots_{};
    std::size_t next_slot_ = 0;
    std::uint64_t frame_ = 0;
    std::map<MeshKey, MeshStructure> meshes_;
    std::vector<MeshStructure> retired_;
    std::vector<id<MTLBuffer>> retired_buffers_;
    std::map<std::uint64_t, PathSums> path_sums_;
    id<MTLBuffer> mesh_scratch_ = nil;
    id<MTLAccelerationStructure> empty_ = nil;
    NSArray<id<MTLBuffer>>* empty_buffers_ = nil;
};

} // namespace

std::unique_ptr<GpuRays> make_metal_rays(SDL_GPUDevice* device) {
    if (@available(macOS 13.0, *)) {
        void* instance = nullptr;
        void* physical = nullptr;
        void* logical = nullptr;
        void* queue = nullptr;
        Uint32 family = 0;
        if (!SDL_SlashGPUNativeDevice(device, &instance, &physical, &logical, &queue, &family)) {
            return nullptr;
        }
        id<MTLDevice> metal = (__bridge id<MTLDevice>)logical;
        if (metal == nil || !metal.supportsRaytracing ||
            metal.argumentBuffersSupport != MTLArgumentBuffersTier2) {
            return nullptr;
        }
        auto rays = std::make_unique<MetalRaysImpl>(device, metal);
        if (auto ready = rays->init(); !ready) {
            log::warn("ray tracing on Metal doesn't start; drawing shadows from shadow maps",
                      {{"error", ready.error().message}});
            return nullptr;
        }
        return rays;
    }
    return nullptr;
}

} // namespace slash::render::detail
