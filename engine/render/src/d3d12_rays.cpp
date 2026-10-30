// Ray tracing on Direct3D 12 (gpu_rays.hpp): the shaders' HLSL, made by slash-ray-msl --hlsl,
// compiled to signed DXIL by the "DXIL shaders" workflow (cmake/compile_ray_dxil.cmake) and run
// as inline ray queries (DXR 1.1) in SDL's command lists with Direct3D 12's own calls. Each
// shader's root signature, in its DXIL, is a descriptor table of its views and one of its
// samplers, in heaps of the rays' own that are bound around the dispatches; SDL's are bound again
// after (SDL_SlashGPURestoreState).
#include "gpu_rays.hpp"

#if defined(_WIN32)

#include "lightmap_light_hlsl.hpp"
#include "native.hpp"
#include "ray_light_hlsl.hpp"
#include "ray_records.hpp"
#include "ray_shadows_hlsl.hpp"

#include "slash/core/log.hpp"

// The DXIL comes from the "DXIL shaders" workflow; a build without it has no rays on Direct3D 12.
#if __has_include("ray_shadows_dxil.hpp") &&                                                       \
                  __has_include("ray_light_dxil.hpp") && __has_include("lightmap_light_dxil.hpp")
#include "lightmap_light_dxil.hpp"
#include "ray_light_dxil.hpp"
#include "ray_shadows_dxil.hpp"
#define SLASH_RAY_DXIL 1
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// What cmake/patches/sdl-ray-tracing.cmake adds to SDL3's GPU API: a buffer's ID3D12Resource.
extern "C" {
void* SDL_SlashGPUNativeBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer);
}

namespace slash::render::detail {

namespace {

// Frames whose rays can be in flight at once, each with its own heaps, uniforms and top-level
// structure; a frame waits for the GPU when all are busy.
constexpr std::size_t kSlots = 4;
// Acceleration structures and their scratch start on these.
constexpr std::uint64_t kStructureAlignment =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

// A COM pointer.
template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com& other) : pointer_(other.pointer_) {
        if (pointer_ != nullptr) {
            pointer_->AddRef();
        }
    }
    Com& operator=(const Com& other) {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            if (pointer_ != nullptr) {
                pointer_->AddRef();
            }
        }
        return *this;
    }
    Com(Com&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }
    ~Com() { reset(); }

    void reset() {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }
    [[nodiscard]] T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    T** put() {
        reset();
        return &pointer_;
    }
    explicit operator bool() const { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
};

Error d3d12_error(const std::string& what, HRESULT result) {
    std::array<char, 16> code{};
    std::snprintf(code.data(), code.size(), "0x%08lX", static_cast<unsigned long>(result));
    return Error{ErrorCode::PlatformFailure, what + " failed (" + code.data() + ")"};
}

// The size of a shader's table of views, from the places slash-ray-msl gave its bindings
// (k<Name>HlslSlots): one past the last place, an unbounded array's first.
template <std::size_t N>
std::uint32_t table_size(const int (&slots)[N][2]) {
    int last = -1;
    for (const auto& slot : slots) {
        last = std::max(last, slot[0]);
    }
    return static_cast<std::uint32_t>(last + 1);
}

// A buffer in memory the CPU writes and the GPU reads, mapped while it lives.
struct Upload {
    Com<ID3D12Resource> buffer;
    void* mapped = nullptr;
    std::uint64_t bytes = 0;
};

// A buffer in the GPU's own memory.
struct Local {
    Com<ID3D12Resource> buffer;
    std::uint64_t bytes = 0;
};

// A mesh's bottom-level structure, with the copies of its triangles it was built from.
struct MeshStructure {
    std::uint64_t version = 0;
    Local built;
    Local vertices;
    Local indices;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangles = 0;
    std::uint64_t used = 0; // the frame that last used it
};

// What one frame's rays use that the next may not change while they run.
struct Slot {
    std::uint64_t frame = 0; // the frame recorded with it, which its marker reaches when done
    bool recorded = false;
    Com<ID3D12DescriptorHeap> views; // the shadows' table, then the light's
    std::uint32_t view_capacity = 0;
    Com<ID3D12DescriptorHeap> samplers; // likewise
    Upload uniforms;                    // a RayShadowsUniforms
    Upload light_frame;                 // a RayLightFrame
    Upload layers;                      // a uint a draw
    Upload instances;                   // a D3D12_RAYTRACING_INSTANCE_DESC a draw
    Upload draw_records;                // a RayDrawRecord a draw
    Upload light_records;               // a RayLightRecord a light
    Local top;
    Local scratch; // the top level's build's
};

// Made where the device has inline ray queries, shader model 6.5 and the DXIL (make_d3d12_rays).
class D3D12RaysImpl final : public GpuRays {
public:
    D3D12RaysImpl(SDL_GPUDevice* sdl, Com<ID3D12Device5> device)
        : sdl_(sdl), device_(std::move(device)) {}

    D3D12RaysImpl(const D3D12RaysImpl&) = delete;
    D3D12RaysImpl& operator=(const D3D12RaysImpl&) = delete;
    D3D12RaysImpl(D3D12RaysImpl&&) = delete;
    D3D12RaysImpl& operator=(D3D12RaysImpl&&) = delete;

    // The GPU is done with the rays' objects before they go.
    ~D3D12RaysImpl() override {
        SDL_WaitForGPUIdle(sdl_);
        if (markers_.buffer) {
            markers_.buffer->Unmap(0, nullptr);
        }
        for (Slot& slot : slots_) {
            unmap_all(slot);
        }
    }

    Result<void> init() {
        view_step_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sampler_step_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
#ifdef SLASH_RAY_DXIL
        if (auto made = make_pipelines(shaders::kRayShadowsDxil, shadows_signature_,
                                       std::span(&shadows_, 1), "ray shadows'");
            !made) {
            return made;
        }
        if (auto made = make_pipelines(shaders::kRayLightDxil, light_signature_,
                                       std::span(light_pipelines_), "ray light's");
            !made) {
            return made;
        }
#endif
        shadow_views_ = table_size(shaders::kRayShadowsHlslSlots);
        light_views_ = table_size(shaders::kRayLightHlslSlots);
        shadow_samplers_ = shaders::kRayShadowsHlslSamplers;
        for (Slot& slot : slots_) {
            if (auto made = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                      shadow_samplers_ + shaders::kRayLightHlslSamplers, true,
                                      slot.samplers);
                !made) {
                return made;
            }
            if (!fit(slot.uniforms, round_up(sizeof(RayShadowsUniforms), 256)) ||
                !fit(slot.light_frame, round_up(sizeof(RayLightFrame), 256))) {
                return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
            }
        }
        // Where each slot's command list marks that it ran, read back on the CPU.
        if (auto made = make_buffer(sizeof(std::uint32_t) * kSlots, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_STATE_COPY_DEST, false, markers_.buffer);
            !made) {
            return made;
        }
        markers_.bytes = sizeof(std::uint32_t) * kSlots;
        if (const HRESULT mapped = markers_.buffer->Map(0, nullptr, &markers_.mapped);
            FAILED(mapped)) {
            return d3d12_error("mapping the rays' markers", mapped);
        }
        if (auto made = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, false, clears_);
            !made) {
            return made;
        }
        return {};
    }

    Result<void> trace(SDL_GPUCommandBuffer* commands, std::span<const RayDraw> draws,
                       const RayWork& work, const RayShadowFrame& frame,
                       const RayTargets& targets) override {
        auto* const native =
            static_cast<ID3D12GraphicsCommandList*>(SDL_SlashGPUNativeCommandBuffer(commands));
        Com<ID3D12GraphicsCommandList4> list;
        if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(list.put())))) {
            return Error{ErrorCode::PlatformFailure,
                         "the frame's command buffer has no Direct3D 12 command list that "
                         "builds acceleration structures"};
        }
        ++frame_;
        Slot& slot = free_slot();
        release_unused();
        auto traced = record(list.get(), slot, draws, work, frame, targets);
        // The slot is the GPU's until it has run everything before this, whatever was recorded.
        slot.frame = frame_;
        slot.recorded = true;
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{
            markers_.buffer->GetGPUVirtualAddress() +
                (sizeof(std::uint32_t) * static_cast<std::size_t>(&slot - slots_.data())),
            static_cast<UINT>(frame_)};
        const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
        list->WriteBufferImmediate(1, &marker, &mode);
        SDL_SlashGPURestoreState(commands);
        return traced;
    }

    Result<LightmapTraced> trace_lightmap(const LightmapTrace& trace) override {
        if (!lightmap_trace_fits(trace)) {
            return Error{ErrorCode::InvalidArgument, "a lightmap trace's layers don't fit it"};
        }
#ifdef SLASH_RAY_DXIL
        if (auto made = make_pipelines(shaders::kLightmapLightDxil, bake_signature_,
                                       std::span(bake_pipelines_), "lightmap light's");
            !made) {
            return made.error();
        }
#endif
        const std::size_t texel_bytes = lightmap_texel_bytes(trace);
        const std::size_t probe_bytes = lightmap_probe_bytes(trace);

        // The inputs, in memory the GPU reads from the CPU's writes; the outputs, in its own,
        // which starts out zero.
        LightmapInputs given;
        lightmap_inputs(trace, given);
        std::array<Upload, 10> in{};
        for (std::size_t i = 0; i < in.size(); ++i) {
            const LightmapInputs::Input& input = given.inputs.at(i);
            if (!fit(in.at(i), input.bytes)) {
                return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
            }
            if (input.bytes > 0) {
                std::memcpy(in.at(i).mapped, input.data, input.bytes);
            }
        }
        // light_source, accum, probe sums, and where they are read back.
        const std::array<std::size_t, 3> output_bytes{texel_bytes, texel_bytes, probe_bytes};
        std::array<Local, 3> out{};
        std::array<Upload, 3> back{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!fit_local(out.at(i), output_bytes.at(i), true) ||
                !make_buffer(std::max<std::size_t>(output_bytes.at(i), 16),
                             D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false,
                             back.at(i).buffer)) {
                return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
            }
            back.at(i).bytes = output_bytes.at(i);
        }
        Upload params_buffer;
        Upload instance_buffer;
        if (!fit(params_buffer, sizeof(LightmapParams)) ||
            !fit(instance_buffer, sizeof(D3D12_RAYTRACING_INSTANCE_DESC))) {
            return Error{ErrorCode::PlatformFailure, "making a bake's buffers failed"};
        }

        // One bottom-level structure of every triangle, and a top-level one with it once.
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // so the shader tests each triangle
        geometry.Triangles.VertexBuffer.StartAddress = in[0].buffer->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(BakeVertex);
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = static_cast<UINT>(trace.vertices.size());
        geometry.Triangles.IndexBuffer = in[1].buffer->GetGPUVirtualAddress();
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry.Triangles.IndexCount = static_cast<UINT>(trace.indices.size());
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
        bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bottom.NumDescs = 1;
        bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottom.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_sizes{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottom_sizes);
        Local bottom_structure;
        if (!make_structure(bottom_sizes.ResultDataMaxSizeInBytes, bottom_structure)) {
            return Error{ErrorCode::PlatformFailure,
                         "making a bake's acceleration structures failed"};
        }

        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = 1.0F;
        instance.Transform[1][1] = 1.0F;
        instance.Transform[2][2] = 1.0F;
        instance.InstanceMask = 0xFF;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = bottom_structure.buffer->GetGPUVirtualAddress();
        std::memcpy(instance_buffer.mapped, &instance, sizeof(instance));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        top.NumDescs = 1;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.InstanceDescs = instance_buffer.buffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_sizes{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&top, &top_sizes);
        Local top_structure;
        Local scratch;
        if (!make_structure(top_sizes.ResultDataMaxSizeInBytes, top_structure) ||
            !fit_local(
                scratch,
                std::max(bottom_sizes.ScratchDataSizeInBytes, top_sizes.ScratchDataSizeInBytes),
                true)) {
            return Error{ErrorCode::PlatformFailure,
                         "making a bake's acceleration structures failed"};
        }
        if (auto set_up = run_commands([&](ID3D12GraphicsCommandList4* list) -> Result<void> {
                const D3D12_RESOURCE_BARRIER to_scratch =
                    transition(scratch.buffer.get(), D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->ResourceBarrier(1, &to_scratch);
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs = bottom;
                build.DestAccelerationStructureData =
                    bottom_structure.buffer->GetGPUVirtualAddress();
                build.ScratchAccelerationStructureData = scratch.buffer->GetGPUVirtualAddress();
                list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                // The top level's build waits for the bottom's, and reuses its scratch.
                const D3D12_RESOURCE_BARRIER built = uav_barrier(nullptr);
                list->ResourceBarrier(1, &built);
                build.Inputs = top;
                build.DestAccelerationStructureData = top_structure.buffer->GetGPUVirtualAddress();
                list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                return {};
            });
            !set_up) {
            return set_up.error();
        }

        // The table: the structure, Params, the inputs and the outputs, by binding.
        Com<ID3D12DescriptorHeap> heap;
        const std::uint32_t views = table_size(shaders::kLightmapLightHlslSlots);
        if (auto made = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, views, true, heap);
            !made) {
            return made.error();
        }
        const auto& slots = shaders::kLightmapLightHlslSlots;
        const auto at = [&](std::size_t binding) {
            return view_at(heap.get(), static_cast<std::uint32_t>(slots[binding][0]));
        };
        structure_view(at(0), top_structure.buffer->GetGPUVirtualAddress());
        // Bindings 1 to 14: Params, the inputs, light_source, accum, the probes, the probe sums.
        const std::array<std::pair<ID3D12Resource*, std::uint64_t>, 14> bound{
            {{params_buffer.buffer.get(), params_buffer.bytes},
             {in[0].buffer.get(), in[0].bytes},
             {in[1].buffer.get(), in[1].bytes},
             {in[2].buffer.get(), in[2].bytes},
             {in[3].buffer.get(), in[3].bytes},
             {in[4].buffer.get(), in[4].bytes},
             {in[5].buffer.get(), in[5].bytes},
             {in[6].buffer.get(), in[6].bytes},
             {in[7].buffer.get(), in[7].bytes},
             {in[8].buffer.get(), in[8].bytes},
             {out[0].buffer.get(), out[0].bytes},
             {out[1].buffer.get(), out[1].bytes},
             {in[9].buffer.get(), in[9].bytes},
             {out[2].buffer.get(), out[2].bytes}}};
        for (std::size_t i = 0; i < bound.size(); ++i) {
            const std::size_t binding = 1 + i;
            if (binding == 11 || binding == 12 || binding == 14) {
                raw_uav(at(binding), bound.at(i).first, bound.at(i).second);
            } else {
                raw_srv(at(binding), bound.at(i).first, bound.at(i).second);
            }
        }

        if (auto ran = run_lightmap_passes(
                trace, given.lights.size(),
                [&](std::size_t pass, std::uint32_t count,
                    const LightmapParams& params) -> Result<void> {
                    std::memcpy(params_buffer.mapped, &params, sizeof(params));
                    return run_commands([&](ID3D12GraphicsCommandList4* list) -> Result<void> {
                        ID3D12DescriptorHeap* table = heap.get();
                        list->SetDescriptorHeaps(1, &table);
                        list->SetComputeRootSignature(bake_signature_.get());
                        list->SetComputeRootDescriptorTable(
                            0, table->GetGPUDescriptorHandleForHeapStart());
                        list->SetPipelineState(bake_pipelines_.at(pass).get());
                        list->Dispatch((count + kBakeGroupSize - 1) / kBakeGroupSize, 1, 1);
                        return {};
                    });
                });
            !ran) {
            return ran.error();
        }
        // The outputs, back where the CPU reads them.
        if (auto copied = run_commands([&](ID3D12GraphicsCommandList4* list) -> Result<void> {
                for (std::size_t i = 0; i < out.size(); ++i) {
                    list->CopyBufferRegion(back.at(i).buffer.get(), 0, out.at(i).buffer.get(), 0,
                                           output_bytes.at(i));
                }
                return {};
            });
            !copied) {
            return copied.error();
        }
        std::array<void*, 3> read{};
        for (std::size_t i = 0; i < back.size(); ++i) {
            const D3D12_RANGE range{0, output_bytes.at(i)};
            if (const HRESULT mapped = back.at(i).buffer->Map(0, &range, &read.at(i));
                FAILED(mapped)) {
                for (std::size_t j = 0; j < i; ++j) {
                    back.at(j).buffer->Unmap(0, nullptr);
                }
                return d3d12_error("reading a bake's light back", mapped);
            }
        }
        LightmapTraced traced = lightmap_traced(trace, read[0], read[1], read[2]);
        const D3D12_RANGE none{0, 0};
        for (Upload& buffer : back) {
            buffer.buffer->Unmap(0, &none);
        }
        for (Upload& buffer : in) {
            unmap(buffer);
        }
        unmap(params_buffer);
        unmap(instance_buffer);
        return traced;
    }

private:
    using MeshKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;

    struct PathSums {
        Local buffer;
        std::uint64_t used = 0;
    };

    // Records a frame's rays with slot.
    Result<void> record(ID3D12GraphicsCommandList4* list, Slot& slot,
                        std::span<const RayDraw> draws, const RayWork& work,
                        const RayShadowFrame& frame, const RayTargets& targets) {
        // Whether rays from the camera's side are traced, which shade what they meet.
        const bool lit = (work.reflection || work.gi || work.paths) && work.lighting != nullptr;

        ID3D12Resource* depth = native_texture(targets.depth).first;
        ID3D12Resource* normals = native_texture(targets.normals).first;
        if (depth == nullptr || normals == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         "the prepass's textures have no Direct3D 12 resources"};
        }

        // The meshes' structures: built again for meshes that are new or changed, from copies
        // of their buffers. Without rays from the camera's side only the draws that cast shadows
        // take part.
        std::vector<MeshStructure*> placed;
        placed.reserve(draws.size());
        std::vector<MeshStructure*> to_build;
        for (const RayDraw& draw : draws) {
            if (draw.index_count < 3 || draw.vertex_count == 0 || ray_mask(draw, lit) == 0) {
                placed.push_back(nullptr);
                continue;
            }
            const auto key = std::make_tuple(draw.mesh, draw.first_index, draw.index_count);
            auto found = meshes_.find(key);
            if (found != meshes_.end() && found->second.version != draw.version) {
                retired_.push_back(std::move(found->second));
                meshes_.erase(found);
                found = meshes_.end();
            }
            if (found == meshes_.end()) {
                auto copy = copy_mesh(list, draw);
                if (!copy) {
                    return copy.error();
                }
                found = meshes_.emplace(key, std::move(copy.value())).first;
                to_build.push_back(&found->second);
            }
            found->second.used = frame_;
            placed.push_back(&found->second);
        }
        if (!to_build.empty()) {
            // The copies, read by the builds and by the light's shader from now on.
            std::vector<D3D12_RESOURCE_BARRIER> moves;
            for (MeshStructure* mesh : to_build) {
                for (ID3D12Resource* copied :
                     {mesh->vertices.buffer.get(), mesh->indices.buffer.get()}) {
                    moves.push_back(transition(copied, D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                }
            }
            list->ResourceBarrier(static_cast<UINT>(moves.size()), moves.data());
            if (auto built = build_meshes(list, to_build); !built) {
                return built;
            }
        }

        // The frame's instances, a draw each, and its top-level structure.
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        std::vector<std::uint32_t> layers;
        std::vector<std::size_t> instanced; // each instance's draw
        bool every_layer = true;            // every draw that casts shadows does for every light
        for (std::size_t i = 0; i < draws.size(); ++i) {
            if (placed[i] == nullptr) {
                continue;
            }
            const Mat4& m = draws[i].transform;
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    instance.Transform[row][column] = m.at((column * 4) + row);
                }
            }
            instance.InstanceID = static_cast<UINT>(layers.size() & 0xFFFFFFU);
            instance.InstanceMask = ray_mask(draws[i], lit);
            instance.InstanceContributionToHitGroupIndex = 0;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure = placed[i]->built.buffer->GetGPUVirtualAddress();
            instances.push_back(instance);
            layers.push_back(draws[i].layers);
            instanced.push_back(i);
            if ((instance.InstanceMask & kRayCastMask) != 0) {
                every_layer = every_layer && draws[i].layers != 0;
            }
        }
        if (auto built = build_top(list, slot, instances, layers); !built) {
            return built;
        }

        // The views each table holds: the shadows', then the light's, its meshes' after.
        std::map<const MeshStructure*, std::uint32_t> mesh_places;
        if (lit) {
            for (const std::size_t i : instanced) {
                mesh_places.emplace(placed[i], static_cast<std::uint32_t>(mesh_places.size()));
            }
        }
        const std::uint32_t views =
            shadow_views_ + light_views_ + (2 * static_cast<std::uint32_t>(mesh_places.size()));
        if (slot.view_capacity < views) {
            if (slot.views) {
                retired_heaps_.push_back(std::move(slot.views));
            }
            const std::uint32_t capacity = std::max(views, slot.view_capacity * 2);
            if (auto made =
                    make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity, true, slot.views);
                !made) {
                slot.view_capacity = 0;
                return made;
            }
            slot.view_capacity = capacity;
        }
        std::array<ID3D12DescriptorHeap*, 2> heaps{slot.views.get(), slot.samplers.get()};
        list->SetDescriptorHeaps(2, heaps.data());

        if (!work.lights.empty() || work.occlusion) {
            if (auto traced =
                    trace_shadows(list, slot, work, frame, targets, depth, normals, every_layer);
                !traced) {
                return traced;
            }
        }
        if (lit) {
            return trace_light(list, slot, draws, placed, instanced, mesh_places, work, frame,
                               targets, depth, normals);
        }
        return {};
    }

    static std::uint64_t round_up(std::uint64_t bytes, std::uint64_t to) {
        return (bytes + to - 1) / to * to;
    }

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    // Waits for the writes to resource (every resource's with null) before what follows.
    static D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        return barrier;
    }

    Result<void> make_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t count, bool visible,
                           Com<ID3D12DescriptorHeap>& heap) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = std::max<std::uint32_t>(count, 1);
        desc.Flags =
            visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (const HRESULT made = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
            FAILED(made)) {
            return d3d12_error("making the rays' descriptor heap", made);
        }
        return {};
    }

    // A buffer of bytes in heap, made in state (which Direct3D 12 keeps only for acceleration
    // structures and the CPU's heaps; others start in COMMON), writable by shaders with uav.
    Result<void> make_buffer(std::uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                             bool uav, Com<ID3D12Resource>& buffer) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = std::max<std::uint64_t>(bytes, 16);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        if (const HRESULT made =
                device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                 nullptr, IID_PPV_ARGS(buffer.put()));
            FAILED(made)) {
            return d3d12_error("making a buffer for rays", made);
        }
        return {};
    }

    // Grows an upload buffer to hold bytes, mapped.
    bool fit(Upload& upload, std::uint64_t bytes) {
        if (upload.buffer && upload.bytes >= bytes) {
            return true;
        }
        const std::uint64_t size = std::max<std::uint64_t>({bytes, 16, upload.bytes * 2});
        if (upload.buffer) {
            unmap(upload);
            retired_buffers_.push_back(std::move(upload.buffer));
        }
        upload.bytes = 0;
        if (!make_buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false,
                         upload.buffer)) {
            return false;
        }
        const D3D12_RANGE none{0, 0};
        if (FAILED(upload.buffer->Map(0, &none, &upload.mapped))) {
            upload.buffer.reset();
            return false;
        }
        upload.bytes = size;
        return true;
    }

    // Grows a buffer in the GPU's memory to hold bytes.
    bool fit_local(Local& local, std::uint64_t bytes, bool uav) {
        if (local.buffer && local.bytes >= bytes) {
            return true;
        }
        const std::uint64_t size = std::max<std::uint64_t>({bytes, 16, local.bytes * 2});
        if (local.buffer) {
            retired_buffers_.push_back(std::move(local.buffer));
        }
        local.bytes = 0;
        if (!make_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, uav,
                         local.buffer)) {
            return false;
        }
        local.bytes = size;
        return true;
    }

    bool make_structure(std::uint64_t bytes, Local& structure) {
        structure.bytes = 0;
        if (!make_buffer(round_up(bytes, kStructureAlignment), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true,
                         structure.buffer)) {
            return false;
        }
        structure.bytes = bytes;
        return true;
    }

    static void unmap(Upload& upload) {
        if (upload.buffer && upload.mapped != nullptr) {
            upload.buffer->Unmap(0, nullptr);
            upload.mapped = nullptr;
        }
    }

    static void unmap_all(Slot& slot) {
        for (Upload* upload : {&slot.uniforms, &slot.light_frame, &slot.layers, &slot.instances,
                               &slot.draw_records, &slot.light_records}) {
            unmap(*upload);
        }
    }

#ifdef SLASH_RAY_DXIL
    // A shader's root signature, from its DXIL, and a pipeline of each of its passes. Made once.
    template <std::size_t N>
    Result<void> make_pipelines(const std::array<std::span<const unsigned char>, N>& passes,
                                Com<ID3D12RootSignature>& signature,
                                std::span<Com<ID3D12PipelineState>> pipelines, const char* what) {
        if (pipelines[0]) {
            return {};
        }
        if (const HRESULT made = device_->CreateRootSignature(0, passes[0].data(), passes[0].size(),
                                                              IID_PPV_ARGS(signature.put()));
            FAILED(made)) {
            return d3d12_error(std::string("making the ") + what + " root signature", made);
        }
        for (std::size_t pass = 0; pass < pipelines.size() && pass < N; ++pass) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = signature.get();
            desc.CS.pShaderBytecode = passes[pass].data();
            desc.CS.BytecodeLength = passes[pass].size();
            if (const HRESULT made =
                    device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipelines[pass].put()));
                FAILED(made)) {
                return d3d12_error(std::string("making the ") + what + " pipeline", made);
            }
        }
        return {};
    }
#endif

    // The resource an SDL texture holds now, and its format.
    std::pair<ID3D12Resource*, DXGI_FORMAT> native_texture(SDL_GPUTexture* texture) {
        if (texture == nullptr) {
            return {nullptr, DXGI_FORMAT_UNKNOWN};
        }
        void* view = nullptr;
        Uint32 format = 0;
        auto* const resource =
            static_cast<ID3D12Resource*>(SDL_SlashGPUNativeTexture(sdl_, texture, &view, &format));
        return {resource, static_cast<DXGI_FORMAT>(format)};
    }

    D3D12_CPU_DESCRIPTOR_HANDLE view_at(ID3D12DescriptorHeap* heap, std::uint32_t place) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t{place} * view_step_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_view_at(ID3D12DescriptorHeap* heap, std::uint32_t place) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += std::uint64_t{place} * view_step_;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_at(ID3D12DescriptorHeap* heap, std::uint32_t place) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t{place} * sampler_step_;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_sampler_at(ID3D12DescriptorHeap* heap,
                                               std::uint32_t place) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += std::uint64_t{place} * sampler_step_;
        return handle;
    }

    void structure_view(D3D12_CPU_DESCRIPTOR_HANDLE at, D3D12_GPU_VIRTUAL_ADDRESS structure) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.RaytracingAccelerationStructure.Location = structure;
        device_->CreateShaderResourceView(nullptr, &desc, at);
    }

    // A texture's view as Texture2D (or TextureCube) of every level; a depth format's as the
    // depth it holds.
    void texture_view(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource* resource, DXGI_FORMAT format,
                      bool cube) {
        switch (format) {
        case DXGI_FORMAT_D16_UNORM:
            format = DXGI_FORMAT_R16_UNORM;
            break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_D32_FLOAT:
            format = DXGI_FORMAT_R32_FLOAT;
            break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
        default:
            break;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (cube) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube.MipLevels = static_cast<UINT>(-1);
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = static_cast<UINT>(-1);
        }
        device_->CreateShaderResourceView(resource, &desc, at);
    }

    void storage_view(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource* resource) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(resource, nullptr, &desc, at);
    }

    // A buffer's view as a ByteAddressBuffer, or RWByteAddressBuffer; none without a buffer.
    void raw_srv(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource* buffer, std::uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.NumElements = buffer != nullptr ? static_cast<UINT>(bytes / 4) : 1;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device_->CreateShaderResourceView(buffer, &desc, at);
    }
    void raw_uav(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource* buffer, std::uint64_t bytes) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = buffer != nullptr ? static_cast<UINT>(bytes / 4) : 1;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(buffer, nullptr, &desc, at);
    }

    void constants_view(D3D12_CPU_DESCRIPTOR_HANDLE at, const Upload& upload, std::size_t bytes) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = upload.buffer->GetGPUVirtualAddress();
        desc.SizeInBytes = static_cast<UINT>(round_up(bytes, 256));
        device_->CreateConstantBufferView(&desc, at);
    }

    // A sampler: nearest or linear, clamped or repeating, at the first level, as the software
    // device's rays sample.
    void sampler(D3D12_CPU_DESCRIPTOR_HANDLE at, bool nearest, bool repeat) {
        D3D12_SAMPLER_DESC desc{};
        desc.Filter =
            nearest ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        const D3D12_TEXTURE_ADDRESS_MODE wrap =
            repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressU = wrap;
        desc.AddressV = wrap;
        desc.AddressW = wrap;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        desc.MinLOD = 0.0F;
        desc.MaxLOD = 0.0F;
        device_->CreateSampler(&desc, at);
    }

    // A slot whose last frame the GPU has run.
    Slot& free_slot() {
        const auto* const markers = static_cast<const volatile std::uint32_t*>(markers_.mapped);
        const auto done = [&](std::size_t i) {
            const Slot& slot = slots_.at(i);
            return !slot.recorded || markers[i] == static_cast<std::uint32_t>(slot.frame);
        };
        for (int attempt = 0; attempt < 2; ++attempt) {
            for (std::size_t i = 0; i < kSlots; ++i) {
                const std::size_t at = (next_slot_ + i) % kSlots;
                if (done(at)) {
                    next_slot_ = (at + 1) % kSlots;
                    return slots_.at(at);
                }
            }
            SDL_WaitForGPUIdle(sdl_);
        }
        // Recorded into a command buffer that was never submitted: nothing uses it.
        Slot& slot = slots_.at(next_slot_);
        next_slot_ = (next_slot_ + 1) % kSlots;
        slot.recorded = false;
        return slot;
    }

    bool idle() const {
        const auto* const markers = static_cast<const volatile std::uint32_t*>(markers_.mapped);
        for (std::size_t i = 0; i < kSlots; ++i) {
            const Slot& slot = slots_.at(i);
            if (slot.recorded && markers[i] != static_cast<std::uint32_t>(slot.frame)) {
                return false;
            }
        }
        return true;
    }

    // Lets go of meshes' structures no frame in flight can use: those replaced or unused for as
    // many frames as there are slots.
    void release_unused() {
        for (auto it = meshes_.begin(); it != meshes_.end();) {
            if (it->second.used + (2 * kSlots) < frame_) {
                retired_.push_back(std::move(it->second));
                it = meshes_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = path_sums_.begin(); it != path_sums_.end();) {
            if (it->second.used + (2 * kSlots) < frame_) {
                retired_buffers_.push_back(std::move(it->second.buffer.buffer));
                it = path_sums_.erase(it);
            } else {
                ++it;
            }
        }
        if (idle()) {
            retired_.clear();
            retired_buffers_.clear();
            retired_heaps_.clear();
        }
    }

    // Copies a draw's triangles out of its mesh's SDL buffers, which rest as vertex and index
    // buffers between SDL's passes.
    Result<MeshStructure> copy_mesh(ID3D12GraphicsCommandList4* list, const RayDraw& draw) {
        auto* const vertices =
            static_cast<ID3D12Resource*>(SDL_SlashGPUNativeBuffer(sdl_, draw.vertices));
        auto* const indices =
            static_cast<ID3D12Resource*>(SDL_SlashGPUNativeBuffer(sdl_, draw.indices));
        if (vertices == nullptr || indices == nullptr) {
            return Error{ErrorCode::PlatformFailure,
                         "a mesh's buffers have no Direct3D 12 resources"};
        }
        MeshStructure out;
        out.version = draw.version;
        out.vertex_count = draw.vertex_count;
        out.triangles = draw.index_count / 3;
        const std::uint64_t vertex_bytes = std::uint64_t{draw.vertex_count} * sizeof(Vertex3D);
        const std::uint64_t index_bytes = std::uint64_t{draw.index_count} * sizeof(std::uint32_t);
        if (!fit_local(out.vertices, vertex_bytes, false) ||
            !fit_local(out.indices, index_bytes, false)) {
            return Error{ErrorCode::PlatformFailure, "copying a mesh for rays failed"};
        }
        std::array<D3D12_RESOURCE_BARRIER, 2> moves{
            transition(vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(indices, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                       D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(2, moves.data());
        list->CopyBufferRegion(out.vertices.buffer.get(), 0, vertices, 0, vertex_bytes);
        list->CopyBufferRegion(out.indices.buffer.get(), 0, indices,
                               std::uint64_t{draw.first_index} * sizeof(std::uint32_t),
                               index_bytes);
        for (D3D12_RESOURCE_BARRIER& move : moves) {
            std::swap(move.Transition.StateBefore, move.Transition.StateAfter);
        }
        list->ResourceBarrier(2, moves.data());
        return out;
    }

    // Builds the bottom-level structures of meshes copied this frame.
    Result<void> build_meshes(ID3D12GraphicsCommandList4* list,
                              std::span<MeshStructure* const> meshes) {
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(meshes.size());
        std::vector<std::uint64_t> scratch_at;
        std::uint64_t scratch = 0;
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            MeshStructure& mesh = *meshes[i];
            D3D12_RAYTRACING_GEOMETRY_DESC& geometry = geometries[i];
            geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            // Not opaque, so rays can ask which draw casts for which light.
            geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            geometry.Triangles.VertexBuffer.StartAddress =
                mesh.vertices.buffer->GetGPUVirtualAddress();
            geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex3D);
            geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry.Triangles.VertexCount = mesh.vertex_count;
            geometry.Triangles.IndexBuffer = mesh.indices.buffer->GetGPUVirtualAddress();
            geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometry.Triangles.IndexCount = mesh.triangles * 3;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = bottom_inputs(geometry);
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
            device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
            if (!make_structure(sizes.ResultDataMaxSizeInBytes, mesh.built)) {
                return Error{ErrorCode::PlatformFailure,
                             "making a mesh's acceleration structure failed"};
            }
            scratch_at.push_back(scratch);
            scratch += round_up(sizes.ScratchDataSizeInBytes, kStructureAlignment);
        }
        if (!fit_local(mesh_scratch_, scratch, true)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' scratch buffer failed"};
        }
        D3D12_RESOURCE_BARRIER move =
            transition(mesh_scratch_.buffer.get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &move);
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = bottom_inputs(geometries[i]);
            build.DestAccelerationStructureData = meshes[i]->built.buffer->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData =
                mesh_scratch_.buffer->GetGPUVirtualAddress() + scratch_at[i];
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        }
        // The top level's build reads them.
        const D3D12_RESOURCE_BARRIER built = uav_barrier(nullptr);
        list->ResourceBarrier(1, &built);
        std::swap(move.Transition.StateBefore, move.Transition.StateAfter);
        list->ResourceBarrier(1, &move);
        return {};
    }

    static D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
    bottom_inputs(const D3D12_RAYTRACING_GEOMETRY_DESC& geometry) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pGeometryDescs = &geometry;
        return inputs;
    }

    // Builds slot's top-level structure over instances, and fills its draws' layers. A frame
    // without instances gets one with none, which no ray meets.
    Result<void> build_top(ID3D12GraphicsCommandList4* list, Slot& slot,
                           std::span<const D3D12_RAYTRACING_INSTANCE_DESC> instances,
                           std::span<const std::uint32_t> layers) {
        if (!fit(slot.instances, std::max<std::size_t>(1, instances.size()) *
                                     sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) ||
            !fit(slot.layers, std::max<std::size_t>(1, layers.size()) * sizeof(std::uint32_t))) {
            return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
        }
        if (!instances.empty()) {
            std::memcpy(slot.instances.mapped, instances.data(), instances.size_bytes());
        }
        if (!layers.empty()) {
            std::memcpy(slot.layers.mapped, layers.data(), layers.size_bytes());
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = static_cast<UINT>(instances.size());
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.InstanceDescs =
            instances.empty() ? 0 : slot.instances.buffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        if (!slot.top.buffer || slot.top.bytes < sizes.ResultDataMaxSizeInBytes) {
            if (slot.top.buffer) {
                retired_buffers_.push_back(std::move(slot.top.buffer));
            }
            if (!make_structure(std::max(sizes.ResultDataMaxSizeInBytes, slot.top.bytes * 2),
                                slot.top)) {
                return Error{ErrorCode::PlatformFailure,
                             "making the rays' top-level structure failed"};
            }
        }
        if (!fit_local(slot.scratch, sizes.ScratchDataSizeInBytes, true)) {
            return Error{ErrorCode::PlatformFailure, "making the rays' scratch buffer failed"};
        }
        D3D12_RESOURCE_BARRIER move =
            transition(slot.scratch.buffer.get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &move);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = slot.top.buffer->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = slot.scratch.buffer->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        // The traces read it.
        std::array<D3D12_RESOURCE_BARRIER, 2> after{uav_barrier(slot.top.buffer.get()), move};
        std::swap(after[1].Transition.StateBefore, after[1].Transition.StateAfter);
        list->ResourceBarrier(2, after.data());
        return {};
    }

    // SDL's textures rest where shaders sample them between its passes; the rays write some.
    static void move_outputs(ID3D12GraphicsCommandList4* list,
                             std::span<ID3D12Resource* const> outputs, bool to_write) {
        std::vector<D3D12_RESOURCE_BARRIER> moves;
        for (ID3D12Resource* output : outputs) {
            moves.push_back(to_write ? transition(output, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                                     : transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE));
        }
        list->ResourceBarrier(static_cast<UINT>(moves.size()), moves.data());
    }

    // The visibilities of the lights with ray slots and the ambient occlusion, into targets'
    // first three outputs.
    Result<void> trace_shadows(ID3D12GraphicsCommandList4* list, Slot& slot, const RayWork& work,
                               const RayShadowFrame& frame, const RayTargets& targets,
                               ID3D12Resource* depth, ID3D12Resource* normals, bool every_layer) {
        const RayShadowsUniforms uniforms = ray_shadows_uniforms(work, frame, every_layer);
        std::memcpy(slot.uniforms.mapped, &uniforms, sizeof(uniforms));
        std::array<ID3D12Resource*, kRayShadowOutputs> outputs{};
        for (std::size_t i = 0; i < kRayShadowOutputs; ++i) {
            outputs.at(i) = native_texture(targets.outputs.at(i)).first;
            if (outputs.at(i) == nullptr) {
                return Error{ErrorCode::PlatformFailure,
                             "a texture rays use has no Direct3D 12 resource"};
            }
        }
        // By binding: the structure, depth, normals, two visibilities, the uniforms, the draws'
        // layers and the occlusion.
        const auto& slots = shaders::kRayShadowsHlslSlots;
        ID3D12DescriptorHeap* views = slot.views.get();
        ID3D12DescriptorHeap* samplers = slot.samplers.get();
        const auto view = [&](std::size_t binding) {
            return view_at(views, static_cast<std::uint32_t>(slots[binding][0]));
        };
        const auto sampler_of = [&](std::size_t binding) {
            return sampler_at(samplers, static_cast<std::uint32_t>(slots[binding][1]));
        };
        structure_view(view(0), slot.top.buffer->GetGPUVirtualAddress());
        texture_view(view(1), depth, native_texture(targets.depth).second, false);
        sampler(sampler_of(1), true, false);
        texture_view(view(2), normals, native_texture(targets.normals).second, false);
        sampler(sampler_of(2), true, false);
        storage_view(view(3), outputs[0]);
        storage_view(view(4), outputs[1]);
        constants_view(view(5), slot.uniforms, sizeof(RayShadowsUniforms));
        raw_srv(view(6), slot.layers.buffer.get(), slot.layers.bytes);
        storage_view(view(7), outputs[2]);

        move_outputs(list, outputs, true);
        list->SetComputeRootSignature(shadows_signature_.get());
        list->SetComputeRootDescriptorTable(0, gpu_view_at(views, 0));
        list->SetComputeRootDescriptorTable(1, gpu_sampler_at(samplers, 0));
        list->SetPipelineState(shadows_.get());
        dispatch_frame(list, frame);
        move_outputs(list, outputs, false);
        return {};
    }

    // The reflections and the ambient light, into targets' last three outputs: the draws each
    // instance stands for as records the shader shades them by, their textures, the lights and
    // the environment.
    Result<void> trace_light(ID3D12GraphicsCommandList4* list, Slot& slot,
                             std::span<const RayDraw> draws, std::span<MeshStructure* const> placed,
                             std::span<const std::size_t> instanced,
                             const std::map<const MeshStructure*, std::uint32_t>& mesh_places,
                             const RayWork& work, const RayShadowFrame& frame,
                             const RayTargets& targets, ID3D12Resource* depth,
                             ID3D12Resource* normals) {
        const RayLighting& lighting = *work.lighting;
        // Its meshes' views follow the table's own; a record names them by their places there.
        RayTextureTable textures;
        std::vector<RayDrawRecord> records;
        records.reserve(instanced.size());
        for (const std::size_t i : instanced) {
            const std::uint32_t place = mesh_places.at(placed[i]);
            records.push_back(ray_draw_record(draws[i], std::uint64_t{2} * place,
                                              (std::uint64_t{2} * place) + 1, textures));
        }
        const std::vector<RayLightRecord> light_records = ray_light_records(lighting);
        if (!fit(slot.draw_records,
                 std::max<std::size_t>(1, records.size()) * sizeof(RayDrawRecord)) ||
            !fit(slot.light_records,
                 std::max<std::size_t>(1, light_records.size()) * sizeof(RayLightRecord))) {
            return Error{ErrorCode::PlatformFailure, "making the rays' buffers failed"};
        }
        if (!records.empty()) {
            std::memcpy(slot.draw_records.mapped, records.data(),
                        records.size() * sizeof(RayDrawRecord));
        }
        if (!light_records.empty()) {
            std::memcpy(slot.light_records.mapped, light_records.data(),
                        light_records.size() * sizeof(RayLightRecord));
        }
        const RayLightFrame uniforms = ray_light_frame(work, frame, light_records.size());
        std::memcpy(slot.light_frame.mapped, &uniforms, sizeof(uniforms));

        std::array<ID3D12Resource*, kRayLightOutputs> outputs{};
        for (std::size_t i = 0; i < kRayLightOutputs; ++i) {
            outputs.at(i) = native_texture(targets.outputs.at(kRayShadowOutputs + i)).first;
        }
        const auto environment = native_texture(targets.environment);
        const auto white = native_texture(targets.white);
        const auto background =
            native_texture(targets.background != nullptr ? targets.background : targets.white);
        if (environment.first == nullptr || white.first == nullptr || background.first == nullptr ||
            std::any_of(outputs.begin(), outputs.end(),
                        [](ID3D12Resource* output) { return output == nullptr; })) {
            return Error{ErrorCode::PlatformFailure,
                         "a texture rays use has no Direct3D 12 resource"};
        }

        // A path-traced target's sums, kept from frame to frame, started over at zero when
        // asked; other frames have none.
        Local* sums = nullptr;
        bool clear_sums = false;
        if (work.paths) {
            const std::uint64_t bytes = std::uint64_t{frame.width} * frame.height * 16;
            PathSums& kept = path_sums_[work.paths->target];
            clear_sums = work.paths->restart;
            if (kept.buffer.bytes != bytes) {
                // New buffers start out zero.
                if (kept.buffer.buffer) {
                    retired_buffers_.push_back(std::move(kept.buffer.buffer));
                }
                kept.buffer.bytes = 0;
                if (!make_buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true,
                                 kept.buffer.buffer)) {
                    return Error{ErrorCode::PlatformFailure, "making a path trace's sums failed"};
                }
                kept.buffer.bytes = bytes;
                clear_sums = false;
            }
            kept.used = frame_;
            sums = &kept.buffer;
        }

        // By binding: the structure, depth, normals, the reflections, the frame, the draws, the
        // lights, the environment, the textures, the ambient light as traced and as smoothed,
        // the paths' sums, the background, then the meshes' vertices and indices.
        const auto& slots = shaders::kRayLightHlslSlots;
        ID3D12DescriptorHeap* views = slot.views.get();
        ID3D12DescriptorHeap* samplers = slot.samplers.get();
        const std::uint32_t view_base = shadow_views_;
        const std::uint32_t sampler_base = shadow_samplers_;
        const auto view = [&](std::size_t binding, std::uint32_t offset = 0) {
            return view_at(views,
                           view_base + static_cast<std::uint32_t>(slots[binding][0]) + offset);
        };
        const auto sampler_of = [&](std::size_t binding, std::uint32_t offset = 0) {
            return sampler_at(samplers, sampler_base +
                                            static_cast<std::uint32_t>(slots[binding][1]) + offset);
        };
        structure_view(view(0), slot.top.buffer->GetGPUVirtualAddress());
        texture_view(view(1), depth, native_texture(targets.depth).second, false);
        sampler(sampler_of(1), true, false);
        texture_view(view(2), normals, native_texture(targets.normals).second, false);
        sampler(sampler_of(2), true, false);
        storage_view(view(3), outputs[0]);
        constants_view(view(4), slot.light_frame, sizeof(RayLightFrame));
        raw_srv(view(5), slot.draw_records.buffer.get(), slot.draw_records.bytes);
        raw_srv(view(6), slot.light_records.buffer.get(), slot.light_records.bytes);
        texture_view(view(7), environment.first, environment.second, true);
        sampler(sampler_of(7), false, false);
        const auto& used = textures.used();
        for (std::uint32_t i = 0; i < kRayTextures; ++i) {
            if (i < used.size()) {
                const auto sampled = native_texture(used[i].first);
                if (sampled.first == nullptr) {
                    return Error{ErrorCode::PlatformFailure,
                                 "a texture rays use has no Direct3D 12 resource"};
                }
                texture_view(view(8, i), sampled.first, sampled.second, false);
                sampler(sampler_of(8, i), (used[i].second & 1U) != 0, (used[i].second & 2U) != 0);
            } else {
                texture_view(view(8, i), white.first, white.second, false);
                sampler(sampler_of(8, i), false, false);
            }
        }
        storage_view(view(9), outputs[1]);
        storage_view(view(10), outputs[2]);
        raw_uav(view(11), sums != nullptr ? sums->buffer.get() : nullptr,
                sums != nullptr ? sums->bytes : 0);
        texture_view(view(12), background.first, background.second, false);
        sampler(sampler_of(12), true, false);
        for (const auto& [mesh, place] : mesh_places) {
            raw_srv(view(13, 2 * place), mesh->vertices.buffer.get(),
                    std::uint64_t{mesh->vertex_count} * sizeof(Vertex3D));
            raw_srv(view(13, (2 * place) + 1), mesh->indices.buffer.get(),
                    std::uint64_t{mesh->triangles} * 3 * sizeof(std::uint32_t));
        }

        move_outputs(list, outputs, true);
        if (clear_sums) {
            // A clear takes the view in the bound heap and the same view in one the CPU reads.
            const std::uint32_t place = view_base + static_cast<std::uint32_t>(slots[11][0]);
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                view_at(clears_.get(), static_cast<std::uint32_t>(&slot - slots_.data()));
            raw_uav(cpu, sums->buffer.get(), sums->bytes);
            const std::array<UINT, 4> zero{};
            list->ClearUnorderedAccessViewUint(gpu_view_at(views, place), cpu, sums->buffer.get(),
                                               zero.data(), 0, nullptr);
            const D3D12_RESOURCE_BARRIER cleared = uav_barrier(sums->buffer.get());
            list->ResourceBarrier(1, &cleared);
        }
        list->SetComputeRootSignature(light_signature_.get());
        list->SetComputeRootDescriptorTable(0, gpu_view_at(views, view_base));
        list->SetComputeRootDescriptorTable(1, gpu_sampler_at(samplers, sampler_base));
        const auto run = [&](std::size_t pass) {
            list->SetPipelineState(light_pipelines_.at(pass).get());
            dispatch_frame(list, frame);
        };
        // The smoothing reads what each pixel's neighbors traced, and the average the sums.
        const auto wait = [&] {
            const D3D12_RESOURCE_BARRIER written = uav_barrier(nullptr);
            list->ResourceBarrier(1, &written);
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
        move_outputs(list, outputs, false);
        return {};
    }

    static void dispatch_frame(ID3D12GraphicsCommandList4* list, const RayShadowFrame& frame) {
        list->Dispatch((frame.width + kRayGroupSize - 1) / kRayGroupSize,
                       (frame.height + kRayGroupSize - 1) / kRayGroupSize, 1);
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
        auto* const native =
            static_cast<ID3D12GraphicsCommandList*>(SDL_SlashGPUNativeCommandBuffer(commands));
        Com<ID3D12GraphicsCommandList4> list;
        if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(list.put())))) {
            SDL_CancelGPUCommandBuffer(commands);
            return Error{ErrorCode::PlatformFailure,
                         "a bake's command buffer has no Direct3D 12 command list"};
        }
        auto recorded = record(list.get());
        SDL_SlashGPURestoreState(commands);
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
        if (const HRESULT removed = device_->GetDeviceRemovedReason(); FAILED(removed)) {
            return d3d12_error("a bake's commands", removed);
        }
        return {};
    }

    SDL_GPUDevice* sdl_;
    Com<ID3D12Device5> device_;
    std::uint32_t view_step_ = 0;
    std::uint32_t sampler_step_ = 0;
    std::uint32_t shadow_views_ = 0;    // the shadows' table's size, where the light's starts
    std::uint32_t light_views_ = 0;     // the light's, before its meshes
    std::uint32_t shadow_samplers_ = 0; // likewise for samplers
    Com<ID3D12RootSignature> shadows_signature_;
    Com<ID3D12PipelineState> shadows_;
    Com<ID3D12RootSignature> light_signature_;
    std::array<Com<ID3D12PipelineState>, kRayLightPasses> light_pipelines_{};
    Com<ID3D12RootSignature> bake_signature_;
    std::array<Com<ID3D12PipelineState>, kBakePasses> bake_pipelines_{};
    std::array<Slot, kSlots> slots_{};
    Upload markers_;                   // each slot's last frame the GPU ran, in readback memory
    Com<ID3D12DescriptorHeap> clears_; // the CPU's copies of views cleared, a slot each
    std::size_t next_slot_ = 0;
    std::uint64_t frame_ = 0;
    std::map<MeshKey, MeshStructure> meshes_;
    std::vector<MeshStructure> retired_;
    std::vector<Com<ID3D12Resource>> retired_buffers_;
    std::vector<Com<ID3D12DescriptorHeap>> retired_heaps_;
    std::map<std::uint64_t, PathSums> path_sums_;
    Local mesh_scratch_;
};

} // namespace

std::unique_ptr<GpuRays> make_d3d12_rays(SDL_GPUDevice* device) {
#ifdef SLASH_RAY_DXIL
    void* instance = nullptr;
    void* physical = nullptr;
    void* logical = nullptr;
    void* queue = nullptr;
    Uint32 family = 0;
    if (!SDL_SlashGPUNativeDevice(device, &instance, &physical, &logical, &queue, &family) ||
        logical == nullptr) {
        return nullptr;
    }
    Com<ID3D12Device5> d3d12;
    if (FAILED(static_cast<ID3D12Device*>(logical)->QueryInterface(IID_PPV_ARGS(d3d12.put())))) {
        return nullptr;
    }
    // Inline ray queries, shader model 6.5 and descriptor tables as large as the light's.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (FAILED(d3d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                          sizeof(options5))) ||
        options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
        FAILED(
            d3d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 ||
        !options.TypedUAVLoadAdditionalFormats ||
        FAILED(d3d12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
        return nullptr;
    }
    auto rays = std::make_unique<D3D12RaysImpl>(device, std::move(d3d12));
    if (auto ready = rays->init(); !ready) {
        log::warn("ray tracing on Direct3D 12 doesn't start; drawing shadows from shadow maps",
                  {{"error", ready.error().message}});
        return nullptr;
    }
    return rays;
#else
    (void)device;
    return nullptr;
#endif
}

} // namespace slash::render::detail

#else

namespace slash::render::detail {

std::unique_ptr<GpuRays> make_d3d12_rays(SDL_GPUDevice* /*device*/) {
    return nullptr;
}

} // namespace slash::render::detail

#endif
