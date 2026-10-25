// Only web builds compile it (engine/render/CMakeLists.txt); the guard keeps tools that read every
// source, such as clang-tidy, from compiling it elsewhere.
#ifdef __EMSCRIPTEN__

#include "webgpu_device.hpp"

#include "checks.hpp"
#include "forward_post.hpp"
#include "forward_post_shaders.hpp"
#include "fsr2.hpp"
#include "fsr2_accumulate_shaders.hpp"
#include "fsr2_depth_clip_shaders.hpp"
#include "fsr2_dilate_shaders.hpp"
#include "fsr2_lock_shaders.hpp"
#include "fsr2_luma_shaders.hpp"
#include "fsr2_rcas_shaders.hpp"
#include "fsr2_reconstruct_shaders.hpp"
#include "material_shaders.hpp"
#include "mesh_clustered_shaders.hpp"
#include "mesh_motion_shaders.hpp"
#include "mesh_normal_shaders.hpp"
#include "mesh_shaders.hpp"
#include "mesh_shading.hpp"
#include "particle_layout.hpp"
#include "particles_draw_shaders.hpp"
#include "particles_emit_shaders.hpp"
#include "particles_process_shaders.hpp"
#include "post_shaders.hpp"
#include "post_shading.hpp"
#include "radiance_shaders.hpp"
#include "reflection.hpp"
#include "shader_code.hpp"
#include "shadow_shaders.hpp"
#include "sky_shaders.hpp"
#include "sky_shading.hpp"
#include "sprite_shaders.hpp"
#include "ssao.hpp"
#include "ssao_shaders.hpp"
#include "ssr.hpp"
#include "ssr_shaders.hpp"
#include "ssr_trace_shaders.hpp"
#include "taa.hpp"
#include "taa_shaders.hpp"
#include "uniforms.hpp"

#include "slash/core/log.hpp"
#include "slash/render/image.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <emscripten/em_js.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

// What web/webgpu.js got from the browser before main(), in Module.slashWebGpu.
EM_JS_DEPS(slash_webgpu, "$WebGPU,$addRunDependency,$removeRunDependency,$stringToNewUTF8");

// Whether there is a device.
EM_JS(int, slash_webgpu_ready, (), {
    const state = Module["slashWebGpu"];
    return state && state.device ? 1 : 0;
});

// Why there is no device, as a string to free.
EM_JS(char*, slash_webgpu_unavailable, (), {
    const state = Module["slashWebGpu"];
    return stringToNewUTF8(state ? state.unavailable : "web/webgpu.js wasn't linked");
});

// The device as a new handle whose futures belong to instance.
EM_JS(WGPUDevice, slash_webgpu_import_device, (WGPUInstance instance),
      { return WebGPU.importJsDevice(Module["slashWebGpu"].device, instance); });

// The oldest error the device reported that nothing has taken yet, as a string to free, or null.
EM_JS(char*, slash_webgpu_take_error, (), {
    const state = Module["slashWebGpu"];
    return state && state.errors.length > 0 ? stringToNewUTF8(state.errors.shift()) : 0;
});

namespace slash::render::detail {
namespace {

// Textures are 8-bit RGBA, or the compressed formats the GPU samples.
constexpr WGPUTextureFormat kTextureFormat = WGPUTextureFormat_RGBA8Unorm;
// A Forward+ frame's world and glow's levels are half floats, as Godot's Forward+ buffers.
constexpr WGPUTextureFormat kHdrFormat = WGPUTextureFormat_RGBA16Float;
// Auto exposure's luminance textures: one float a texel, as Godot's. WebGPU filters no floats, so
// passes only load them.
constexpr WGPUTextureFormat kLuminanceFormat = WGPUTextureFormat_R32Float;
// The radiance's cubes are RGB10A2, as Godot's.
constexpr WGPUTextureFormat kRadianceFormat = WGPUTextureFormat_RGB10A2Unorm;
// Meshes draw over 32-bit float depth, which every WebGPU device has and suits reversed depth.
constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth32Float;
// Forward+'s normal-roughness prepass draws into bytes.
constexpr WGPUTextureFormat kNormalFormat = WGPUTextureFormat_RGBA8Unorm;
// TAA's motion vectors are two half floats, as Godot's velocity buffer.
constexpr WGPUTextureFormat kVelocityFormat = WGPUTextureFormat_RG16Float;
// The largest shadow atlas: WebGPU's default maxTextureDimension2D.
constexpr std::uint32_t kMaxShadowAtlas = 8192;

// FSR 2's passes, other than the reconstruct pass, and the formats each draws into, as Godot's FSR
// 2 effect makes its textures.
enum class Fsr2Pass : std::uint8_t { Luma, Dilate, DepthClip, Lock, Accumulate, Rcas };
constexpr std::size_t kFsr2Passes = 6;
constexpr std::array kFsr2LumaTargets{WGPUTextureFormat_R16Float};
// The dilated depth, motion vectors and luma the locks are found in.
constexpr std::array kFsr2DilateTargets{WGPUTextureFormat_R32Float, WGPUTextureFormat_RG16Float,
                                        WGPUTextureFormat_R16Float};
// The prepared color and the reactive masks.
constexpr std::array kFsr2DepthClipTargets{WGPUTextureFormat_RGBA16Float,
                                           WGPUTextureFormat_RG8Unorm};
constexpr std::array kFsr2LockTargets{WGPUTextureFormat_R16Float};
// The color, the locks and the luma history.
constexpr std::array kFsr2AccumulateTargets{
    WGPUTextureFormat_RGBA16Float, WGPUTextureFormat_RG16Float, WGPUTextureFormat_RGBA8Unorm};
constexpr std::array kFsr2RcasTargets{WGPUTextureFormat_RGBA16Float};
// Threads in a group of the reconstruct pass, each way, as its [numthreads].
constexpr std::uint32_t kFsr2Threads = 8;

// A pass's WGSL, how many textures it loads and the formats it draws into.
struct Fsr2PassCode {
    std::string_view vertex;
    std::string_view fragment;
    std::uint32_t textures = 1;
    std::span<const WGPUTextureFormat> targets;
};

Fsr2PassCode fsr2_pass_code(Fsr2Pass pass) {
    switch (pass) {
    case Fsr2Pass::Luma:
        return {.vertex = shaders::kFsr2LumaVertexWgsl,
                .fragment = shaders::kFsr2LumaFragmentWgsl,
                .targets = kFsr2LumaTargets};
    case Fsr2Pass::Dilate:
        return {.vertex = shaders::kFsr2DilateVertexWgsl,
                .fragment = shaders::kFsr2DilateFragmentWgsl,
                .textures = 3,
                .targets = kFsr2DilateTargets};
    case Fsr2Pass::DepthClip:
        return {.vertex = shaders::kFsr2DepthClipVertexWgsl,
                .fragment = shaders::kFsr2DepthClipFragmentWgsl,
                .textures = 5,
                .targets = kFsr2DepthClipTargets};
    case Fsr2Pass::Lock:
        return {.vertex = shaders::kFsr2LockVertexWgsl,
                .fragment = shaders::kFsr2LockFragmentWgsl,
                .targets = kFsr2LockTargets};
    case Fsr2Pass::Accumulate:
        return {.vertex = shaders::kFsr2AccumulateVertexWgsl,
                .fragment = shaders::kFsr2AccumulateFragmentWgsl,
                .textures = 8,
                .targets = kFsr2AccumulateTargets};
    case Fsr2Pass::Rcas:
        break;
    }
    return {.vertex = shaders::kFsr2RcasVertexWgsl,
            .fragment = shaders::kFsr2RcasFragmentWgsl,
            .targets = kFsr2RcasTargets};
}

constexpr WGPUTextureFormat gpu_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::Bc7:
        return WGPUTextureFormat_BC7RGBAUnorm;
    case TextureFormat::Astc4x4:
        return WGPUTextureFormat_ASTC4x4Unorm;
    case TextureFormat::Rgba16F:
        return WGPUTextureFormat_RGBA16Float;
    case TextureFormat::Rgba8:
        break;
    }
    return kTextureFormat;
}

constexpr WGPUTextureFormat ssao_gpu_format(SsaoFormat format) {
    switch (format) {
    case SsaoFormat::R16F:
        return WGPUTextureFormat_R16Float;
    case SsaoFormat::RG8:
        return WGPUTextureFormat_RG8Unorm;
    case SsaoFormat::R8:
        return WGPUTextureFormat_R8Unorm;
    case SsaoFormat::R32F:
        break;
    }
    return WGPUTextureFormat_R32Float;
}

constexpr WGPUTextureFormat ssr_gpu_format(SsrFormat format) {
    switch (format) {
    case SsrFormat::RGBA8:
        return WGPUTextureFormat_RGBA8Unorm;
    case SsrFormat::RGBA16F:
        return WGPUTextureFormat_RGBA16Float;
    case SsrFormat::RGBA32F:
        return WGPUTextureFormat_RGBA32Float;
    case SsrFormat::R8:
        return WGPUTextureFormat_R8Unorm;
    case SsrFormat::R32F:
        break;
    }
    return WGPUTextureFormat_R32Float;
}

// The vertex buffer's first size: 3,276 vertices.
constexpr std::uint64_t kMinVertexBytes = 64 * 1024;
// Uniform blocks start this far apart, WebGPU's minUniformBufferOffsetAlignment.
constexpr std::uint64_t kUniformAlignment = 256;
// Rows of a texture copied to a buffer start this far apart.
constexpr std::uint32_t kCopyRowAlignment = 256;
// Threads in a group of the particle shaders, as their [numthreads].
constexpr std::uint32_t kParticleThreads = 64;
// The largest of the compute shaders' uniforms, which their binding reads.
constexpr std::uint64_t kComputeUniformBytes =
    std::max({sizeof(ProcessUniforms), sizeof(EmitUniforms), sizeof(DrawUniforms)});
// The most bytes any binding of the uniform buffer reads.
constexpr std::uint64_t kLargestUniforms = std::max<std::uint64_t>(
    {sizeof(Mat4), sizeof(VertexProgramUniforms), sizeof(ProgramUniforms), sizeof(LightsUniforms),
     kComputeUniformBytes, sizeof(MeshInstanceUniforms), sizeof(MeshSurfaceUniforms),
     sizeof(MeshSceneUniforms), sizeof(MeshShadowUniforms), sizeof(MeshShadowPassUniforms),
     sizeof(PostUniforms), sizeof(SkyUniforms), sizeof(RadianceUniforms), sizeof(SsaoUniforms),
     sizeof(SsrUniforms), sizeof(MeshCameraUniforms), sizeof(TaaUniforms), sizeof(Fsr2Uniforms)});
// The bind groups are SDL's GPU API's, which the HLSL's registers name and the WGSL keeps
// (cmake/compile_shaders.cmake): a vertex stage's uniforms in group 1, a fragment stage's
// textures in group 2 (samplers kSamplerBinding after their textures) and its uniforms in group
// 3; a compute shader's read-only buffers in group 0, read-write ones in 1 and uniforms in 2. The
// mesh shader's are the same: its camera and instance, its eleven textures, two shadow atlases,
// the radiance, the SSAO and the SSR, with their samplers kMeshSamplerBinding after them, its
// surface, scene and shadows; and the shadow shader's: its pass and instance, its albedo texture
// and its surface.
constexpr std::uint32_t kSamplerBinding = 8;
constexpr std::uint32_t kMeshSamplerBinding = 16;

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };
// The compute shaders: running particle steps, turning particles into quads, and numbering what a
// sub-emitter takes.
enum class ComputeKind : std::uint8_t { Process, Draw, Emit };

WGPUStringView text_view(std::string_view text) {
    return {.data = text.data(), .length = text.size()};
}

// Godot's canvas blend modes, for straight-alpha colors.
WGPUBlendState blend_state(BlendMode mode) {
    WGPUBlendState blend{};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.alpha.operation = WGPUBlendOperation_Add;
    switch (mode) {
    case BlendMode::Mix:
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        break;
    case BlendMode::Add:
    case BlendMode::Sub:
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_One;
        blend.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.alpha.dstFactor = WGPUBlendFactor_One;
        if (mode == BlendMode::Sub) {
            blend.color.operation = WGPUBlendOperation_ReverseSubtract;
            blend.alpha.operation = WGPUBlendOperation_ReverseSubtract;
        }
        break;
    case BlendMode::Mul:
        blend.color.srcFactor = WGPUBlendFactor_Dst;
        blend.color.dstFactor = WGPUBlendFactor_Zero;
        blend.alpha.srcFactor = WGPUBlendFactor_DstAlpha;
        blend.alpha.dstFactor = WGPUBlendFactor_Zero;
        break;
    case BlendMode::PremulAlpha:
        blend.color.srcFactor = WGPUBlendFactor_One;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        break;
    }
    return blend;
}

// A string webgpu.h passes, which may or may not end in a null.
std::string view_string(WGPUStringView text) {
    if (text.data == nullptr) {
        return {};
    }
    return {text.data, text.length == WGPU_STRLEN ? std::strlen(text.data) : text.length};
}

// The errors caught in error scopes, in the order they are pushed.
constexpr std::array kErrorFilters{WGPUErrorFilter_Validation, WGPUErrorFilter_OutOfMemory,
                                   WGPUErrorFilter_Internal};

// A string the JavaScript side made, freed once read.
std::string take_string(char* text) {
    std::string out = text != nullptr ? text : "";
    std::free(text); // NOLINT(cppcoreguidelines-no-malloc): allocated with malloc in JavaScript
    return out;
}

// Bind group layout entries.
WGPUBindGroupLayoutEntry uniform_entry(std::uint32_t binding, WGPUShaderStage stage,
                                       std::uint64_t size) {
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = binding;
    entry.visibility = stage;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.hasDynamicOffset = 1;
    entry.buffer.minBindingSize = size;
    return entry;
}

WGPUBindGroupLayoutEntry storage_entry(std::uint32_t binding, bool read_only,
                                       WGPUShaderStage stage = WGPUShaderStage_Compute) {
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = binding;
    entry.visibility = stage;
    entry.buffer.type =
        read_only ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
    return entry;
}

WGPUBindGroupLayoutEntry texture_entry(std::uint32_t binding) {
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Float;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    return entry;
}

WGPUBindGroupLayoutEntry sampler_entry(std::uint32_t binding) {
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.sampler.type = WGPUSamplerBindingType_Filtering;
    return entry;
}

// A cube texture, as the radiance.
WGPUBindGroupLayoutEntry cube_entry(std::uint32_t binding) {
    WGPUBindGroupLayoutEntry entry = texture_entry(binding);
    entry.texture.viewDimension = WGPUTextureViewDimension_Cube;
    return entry;
}

// A shadow atlas, and the comparison sampler it is read through.
WGPUBindGroupLayoutEntry depth_entry(std::uint32_t binding) {
    WGPUBindGroupLayoutEntry entry = texture_entry(binding);
    entry.texture.sampleType = WGPUTextureSampleType_Depth;
    return entry;
}

WGPUBindGroupLayoutEntry comparison_entry(std::uint32_t binding) {
    WGPUBindGroupLayoutEntry entry = sampler_entry(binding);
    entry.sampler.type = WGPUSamplerBindingType_Comparison;
    return entry;
}

// The view matrix as WGSL reads it. The HLSL's matrices are row major in SPIR-V, which Tint
// drops on the way to WGSL, whose matrices are always column major, so it reads them turned.
Mat4 transposed(const Mat4& matrix) {
    Mat4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            out.at((column * 4) + row) = matrix.at((row * 4) + column);
        }
    }
    return out;
}

// Bytes appended to a block of uniforms, each at a multiple of kUniformAlignment.
class UniformBlocks {
public:
    template <typename T>
    std::uint32_t add(const T& value) {
        const std::size_t at = bytes_.size();
        const std::size_t padded =
            (sizeof(T) + kUniformAlignment - 1) / kUniformAlignment * kUniformAlignment;
        bytes_.resize(at + padded);
        std::memcpy(bytes_.data() + at, &value, sizeof(T));
        return static_cast<std::uint32_t>(at);
    }
    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }
    [[nodiscard]] bool empty() const { return bytes_.empty(); }
    void clear() { bytes_.clear(); }

private:
    std::vector<std::byte> bytes_;
};

class WebGpuDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    WebGpuDevice(WGPUInstance instance, WGPUDevice device, SDL_Window* window)
        : instance_(instance), device_(device), queue_(wgpuDeviceGetQueue(device)),
          window_(window) {}

    WebGpuDevice(const WebGpuDevice&) = delete;
    WebGpuDevice& operator=(const WebGpuDevice&) = delete;
    WebGpuDevice(WebGpuDevice&&) = delete;
    WebGpuDevice& operator=(WebGpuDevice&&) = delete;

    ~WebGpuDevice() override {
        if (frame_texture_ != nullptr) {
            wgpuTextureViewRelease(frame_view_);
            wgpuTextureRelease(frame_texture_);
        }
        release_pending();
        clear_texture_groups();
        for (const auto& [id, texture] : textures_) {
            destroy_gpu_texture(texture.gpu);
        }
        destroy_gpu_texture(white_);
        for (WGPUSampler sampler : samplers_) {
            if (sampler != nullptr) {
                wgpuSamplerRelease(sampler);
            }
        }
        for (const Pipeline& pipeline : pipelines_) {
            wgpuRenderPipelineRelease(pipeline.pipeline);
        }
        for (const MeshPipeline& pipeline : mesh_pipelines_) {
            wgpuRenderPipelineRelease(pipeline.pipeline);
        }
        for (const auto& [id, mesh] : meshes_) {
            destroy_buffer(mesh.vertices);
            destroy_buffer(mesh.indices);
            destroy_buffer(mesh.previous);
        }
        destroy_gpu_texture(depth_);
        for (const auto& [format, pipeline] : post_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        for (const auto& [format, pipeline] : forward_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        for (const auto& [format, pipeline] : sky_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        for (WGPURenderPipeline pipeline : radiance_pipelines_) {
            if (pipeline != nullptr) {
                wgpuRenderPipelineRelease(pipeline);
            }
        }
        for (const CubeTexture& cube :
             {radiance_.raw, radiance_.filtered, no_radiance_, probe_raw_, no_probes_}) {
            destroy_cube(cube);
        }
        for (const auto& [id, atlas] : atlases_) {
            destroy_cube(atlas);
        }
        destroy_screen(screen_);
        destroy_screen(no_screen_);
        if (probe_pipeline_ != nullptr) {
            wgpuRenderPipelineRelease(probe_pipeline_);
        }
        if (radiance_sampler_ != nullptr) {
            wgpuSamplerRelease(radiance_sampler_);
        }
        for (WGPURenderPipeline pipeline : normal_pipelines_) {
            if (pipeline != nullptr) {
                wgpuRenderPipelineRelease(pipeline);
            }
        }
        for (WGPURenderPipeline pipeline : motion_pipelines_) {
            if (pipeline != nullptr) {
                wgpuRenderPipelineRelease(pipeline);
            }
        }
        if (taa_pipeline_ != nullptr) {
            wgpuRenderPipelineRelease(taa_pipeline_);
        }
        for (WGPURenderPipeline pipeline : fsr2_pipelines_) {
            if (pipeline != nullptr) {
                wgpuRenderPipelineRelease(pipeline);
            }
        }
        if (fsr2_reconstruct_ != nullptr) {
            wgpuComputePipelineRelease(fsr2_reconstruct_);
        }
        for (const auto& [format, pipeline] : ssao_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        for (const auto& [format, pipeline] : ssr_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        if (ssr_trace_pipeline_ != nullptr) {
            wgpuRenderPipelineRelease(ssr_trace_pipeline_);
        }
        release_post_targets();
        release_forward_targets();
        release_prepass();
        release_ssao_targets();
        release_ssr_targets();
        while (!ssr_histories_.empty()) {
            forget_ssr_history(ssr_histories_.back().first);
        }
        while (!taa_histories_.empty()) {
            forget_taa_history(taa_histories_.back().first);
        }
        while (!fsr2_frames_.empty()) {
            forget_fsr2_frames(fsr2_frames_.back().first);
        }
        release_fsr2_textures();
        release_pending();
        for (const GpuTexture& atlas :
             {shadow_atlases_[0].gpu, shadow_atlases_[1].gpu, no_shadows_}) {
            destroy_gpu_texture(atlas);
        }
        if (shadow_sampler_ != nullptr) {
            wgpuSamplerRelease(shadow_sampler_);
        }
        for (WGPURenderPipeline pipeline : shadow_pipelines_) {
            if (pipeline != nullptr) {
                wgpuRenderPipelineRelease(pipeline);
            }
        }
        for (WGPUComputePipeline pipeline : compute_pipelines_) {
            if (pipeline != nullptr) {
                wgpuComputePipelineRelease(pipeline);
            }
        }
        for (WGPUShaderModule module : modules_) {
            if (module != nullptr) {
                wgpuShaderModuleRelease(module);
            }
        }
        for (const auto& [id, shader] : game_shaders_) {
            release_game_shader(shader);
        }
        for (const auto& [id, system] : particle_systems_) {
            destroy_buffer(system.buffer);
            destroy_buffer(system.emissions);
        }
        for (WGPUBuffer buffer :
             {no_source_, no_target_, particle_data_, vertex_buffer_, uniform_buffer_,
              cluster_lights_.buffer, cluster_cells_.buffer, probe_buffer_.buffer}) {
            destroy_buffer(buffer);
        }
        release_uniform_groups();
        if (cluster_group_ != nullptr) {
            wgpuBindGroupRelease(cluster_group_);
        }
        for (WGPUBindGroupLayout layout : {empty_layout_,
                                           vertex_uniforms_layout_,
                                           sprite_textures_layout_,
                                           material_textures_layout_,
                                           fragment_uniforms_layout_,
                                           process_inputs_layout_,
                                           process_outputs_layout_,
                                           draw_inputs_layout_,
                                           draw_outputs_layout_,
                                           emit_outputs_layout_,
                                           compute_uniforms_layout_,
                                           mesh_vertex_layout_,
                                           mesh_textures_layout_,
                                           mesh_fragment_layout_,
                                           mesh_clusters_layout_,
                                           shadow_vertex_layout_,
                                           shadow_fragment_layout_,
                                           post_textures_layout_,
                                           post_uniforms_layout_,
                                           forward_textures_layout_,
                                           forward_uniforms_layout_,
                                           sky_uniforms_layout_,
                                           cube_texture_layout_,
                                           filter_uniforms_layout_,
                                           ssao_textures_layout_,
                                           ssao_uniforms_layout_,
                                           ssr_textures_layout_,
                                           ssr_uniforms_layout_,
                                           taa_textures_layout_,
                                           taa_uniforms_layout_,
                                           fsr2_uniforms_layout_,
                                           fsr2_storage_layout_,
                                           fsr2_compute_textures_layout_,
                                           fsr2_compute_storage_layout_,
                                           fsr2_compute_uniforms_layout_}) {
            if (layout != nullptr) {
                wgpuBindGroupLayoutRelease(layout);
            }
        }
        for (WGPUBindGroupLayout layout : fsr2_textures_layouts_) {
            if (layout != nullptr) {
                wgpuBindGroupLayoutRelease(layout);
            }
        }
        for (WGPUPipelineLayout layout : pipeline_layouts_) {
            if (layout != nullptr) {
                wgpuPipelineLayoutRelease(layout);
            }
        }
        if (surface_ != nullptr) {
            wgpuSurfaceUnconfigure(surface_);
            wgpuSurfaceRelease(surface_);
        }
        // The browser's device outlives this one, so its error scopes go with it.
        if (can_wait_) {
            for (std::size_t i = 0; i < kErrorFilters.size(); ++i) {
                WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
                callback.mode = WGPUCallbackMode_AllowSpontaneous;
                callback.callback = [](WGPUPopErrorScopeStatus /*status*/, WGPUErrorType /*type*/,
                                       WGPUStringView /*text*/, void* /*userdata1*/,
                                       void* /*userdata2*/) {};
                wgpuDevicePopErrorScope(device_, callback);
            }
        }
        wgpuQueueRelease(queue_);
        wgpuDeviceRelease(device_);
        wgpuInstanceRelease(instance_);
    }

    // Makes the canvas's surface, the bind group layouts, samplers and the white texture. Shaders
    // and pipelines wait until something draws with them.
    Result<void> init() {
        // Errors left from devices before this one on the same WebGPU device aren't this one's.
        while (char* error = slash_webgpu_take_error()) {
            log::warn("an earlier render device's WebGPU error", {{"error", take_string(error)}});
        }
        can_wait_ = wgpuHasInstanceFeature(WGPUInstanceFeatureName_TimedWaitAny) != 0;
        if (can_wait_) {
            push_error_scopes();
        }
        sampled_.at(static_cast<std::size_t>(TextureFormat::Bc7)) =
            wgpuDeviceHasFeature(device_, WGPUFeatureName_TextureCompressionBC) != 0;
        sampled_.at(static_cast<std::size_t>(TextureFormat::Astc4x4)) =
            wgpuDeviceHasFeature(device_, WGPUFeatureName_TextureCompressionASTC) != 0;
        if (window_ != nullptr) {
            const char* canvas =
                SDL_GetStringProperty(SDL_GetWindowProperties(window_),
                                      SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, "#canvas");
            WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source =
                WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
            source.selector = text_view(canvas);
            WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
            descriptor.nextInChain = &source.chain;
            surface_ = wgpuInstanceCreateSurface(instance_, &descriptor);
            if (surface_ == nullptr) {
                return make_error(ErrorCode::PlatformFailure,
                                  std::string("the canvas ") + canvas + " has no WebGPU context");
            }
            WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
            if (wgpuSurfaceGetCapabilities(surface_, nullptr, &capabilities) ==
                    WGPUStatus_Success &&
                capabilities.formatCount > 0) {
                surface_format_ = capabilities.formats[0];
            }
            wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        }
        make_layouts();
        for (const Filter filter : {Filter::Linear, Filter::Nearest}) {
            for (const bool repeat : {false, true}) {
                WGPUSamplerDescriptor info = WGPU_SAMPLER_DESCRIPTOR_INIT;
                info.minFilter =
                    filter == Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
                info.magFilter = info.minFilter;
                info.mipmapFilter = WGPUMipmapFilterMode_Nearest;
                info.addressModeU = repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
                info.addressModeV = info.addressModeU;
                info.addressModeW = info.addressModeU;
                samplers_.at(sampler_index(filter, repeat)) =
                    wgpuDeviceCreateSampler(device_, &info);
            }
        }
        const TextureDesc white_desc{
            .width = 1, .height = 1, .filter = Filter::Linear, .render_target = false};
        white_ = create_gpu_texture(white_desc, white_desc.format);
        constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};
        write_texels(white_.texture, white_desc, kWhite);
        return reported_errors();
    }

    [[nodiscard]] Backend backend() const override { return Backend::WebGpu; }

    [[nodiscard]] bool supports(TextureFormat format) const override {
        return sampled_.at(static_cast<std::size_t>(format));
    }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> texels) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, texels, true); !checked) {
            return checked.error();
        }
        if (desc.cubes > 0) {
            // A reflection atlas: half floats, with the levels a probe's cube is filtered into.
            const auto id = static_cast<TextureId>(next_texture_++);
            atlases_[id] = make_cube(desc.width, probe_levels(desc.width), kHdrFormat, desc.cubes);
            textures_[id] = Texture{.gpu = {}, .desc = desc, .stored = desc.format};
            return id;
        }
        // A format the GPU doesn't sample is kept as RGBA8. WebGPU starts textures as
        // transparent black.
        const TextureFormat stored = supports(desc.format) ? desc.format : TextureFormat::Rgba8;
        GpuTexture gpu = create_gpu_texture(desc, stored);
        if (!texels.empty()) {
            if (auto written = upload_texels(gpu.texture, desc, stored, texels); !written) {
                destroy_gpu_texture(gpu);
                return written.error();
            }
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.gpu = gpu, .desc = desc, .stored = stored};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> texels) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (auto checked = check_pixels(desc, texels, false); !checked) {
            return checked;
        }
        if (radiance_.key && radiance_.key->texture == texture) {
            radiance_.key.reset(); // drawn from the old texels
        }
        // Queue writes run ahead of the command buffers sent after them, so every draw of the
        // frame being built sees the new texels.
        return upload_texels(it->second.gpu.texture, desc, it->second.stored, texels);
    }

    void destroy_texture(TextureId texture) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return;
        }
        if (const auto atlas = atlases_.find(texture); atlas != atlases_.end()) {
            forget_texture_groups(atlas->second.view);
            if (in_frame_) {
                pending_cubes_.push_back(atlas->second);
            } else {
                destroy_cube(atlas->second);
            }
            atlases_.erase(atlas);
            textures_.erase(it);
            return;
        }
        forget_texture_groups(it->second.gpu.view);
        // The frame being built may still draw with it, so it goes once the frame is sent.
        if (in_frame_) {
            pending_textures_.push_back(it->second.gpu);
        } else {
            destroy_gpu_texture(it->second.gpu);
        }
        textures_.erase(it);
        forget_ssr_history(texture);
        forget_taa_history(texture);
        forget_fsr2_frames(texture);
    }

    Result<ShaderId> create_shader(const CompiledShader& shader) override {
        if (auto checked = check_compiled_shader(shader, Backend::WebGpu); !checked) {
            return checked.error();
        }
        const GameShader made{.vertex = create_module(shader.wgsl_vertex),
                              .fragment = create_module(shader.wgsl_fragment)};
        // Where the device can wait, WGSL the browser refuses fails here, not at the first draw.
        if (can_wait_) {
            if (auto errors = scoped_errors(); !errors) {
                release_game_shader(made);
                return errors.error();
            }
        }
        const auto id = static_cast<ShaderId>(next_shader_++);
        game_shaders_[id] = made;
        return id;
    }

    void destroy_shader(ShaderId shader) override {
        const auto it = game_shaders_.find(shader);
        if (it == game_shaders_.end()) {
            return;
        }
        // Its pipelines go with it, once the frame being built, which may draw with them, is sent.
        std::erase_if(pipelines_, [&](const Pipeline& made) {
            if (made.shader != shader) {
                return false;
            }
            if (in_frame_) {
                pending_pipelines_.push_back(made.pipeline);
            } else {
                wgpuRenderPipelineRelease(made.pipeline);
            }
            return true;
        });
        release_game_shader(it->second);
        game_shaders_.erase(it);
    }

    Result<ParticlesId> create_particles(std::uint32_t amount, std::uint32_t trail) override {
        if (auto checked = check_particle_amount(amount, trail); !checked) {
            return checked.error();
        }
        if (auto pipeline = compute_pipeline(ComputeKind::Process); !pipeline) {
            return pipeline.error();
        }
        make_unbound_emissions();
        WGPUBuffer buffer = create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
                                          std::uint64_t{amount} * trail * sizeof(Particle));
        // WebGPU starts buffers as zeros, but a system starts with the shader's own reset.
        if (auto queued = queue_particle_job(
                {.particles = buffer, .amount = amount, .trail = trail, .reset = true});
            !queued) {
            destroy_buffer(buffer);
            return queued.error();
        }
        const auto id = static_cast<ParticlesId>(next_particles_++);
        particle_systems_[id] = {.buffer = buffer, .amount = amount, .trail = trail};
        return id;
    }

    void destroy_particles(ParticlesId particles) override {
        const auto it = particle_systems_.find(particles);
        if (it == particle_systems_.end()) {
            return;
        }
        // Jobs waiting for the frame may still use it, so it goes once they are sent.
        for (WGPUBuffer buffer : {it->second.buffer, it->second.emissions}) {
            if (buffer == nullptr) {
                continue;
            }
            if (particle_jobs_.empty()) {
                destroy_buffer(buffer);
            } else {
                pending_buffers_.push_back(buffer);
            }
        }
        particle_systems_.erase(it);
    }

    Result<void> process_particle_chain(std::span<const ParticleRun> chain) override {
        if (auto checked = check_particle_chain(
                chain,
                [this](ParticlesId particles) { return particle_systems_.contains(particles); });
            !checked) {
            return checked;
        }
        const std::size_t steps = chain.front().steps.size() - chain.front().history;
        if (steps == 0) {
            return {};
        }
        if (chain.size() > 1) {
            if (auto pipeline = compute_pipeline(ComputeKind::Emit); !pipeline) {
                return pipeline.error();
            }
        }
        // Each system's process material, shapes and steps; a system before another writes what
        // it starts there to its emission buffer.
        std::vector<ParticleJob> links;
        links.reserve(chain.size());
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const ParticleRun& run = chain[k];
            ParticleSystem& system = particle_systems_.at(run.particles);
            if (k + 1 < chain.size() && system.emissions == nullptr) {
                system.emissions = create_buffer(
                    WGPUBufferUsage_Storage,
                    (1 + (std::uint64_t{system.amount} * kEmissionWords)) * sizeof(Word));
            }
            ParticleJob job{
                .particles = system.buffer, .amount = system.amount, .trail = system.trail};
            job.material = material_words(*run.process);
            const std::vector<std::uint32_t> shape_at =
                run.process->collision_mode == ParticleCollision::Disabled
                    ? std::vector<std::uint32_t>{}
                    : pack_particle_shapes(run.steps, run.shapes, particle_words_);
            job.steps = static_cast<std::uint32_t>(particle_words_.size());
            job.history = static_cast<std::uint32_t>(run.history);
            job.step_count = static_cast<std::uint32_t>(run.steps.size() - run.history);
            pack_particle_steps(run.steps, shape_at, particle_words_);
            if (k > 0) {
                job.source = links.back().target;
                job.chain |= kChainTakes;
            }
            if (k + 1 < chain.size()) {
                job.target = system.emissions;
                job.chain |= kChainEmits;
            }
            links.push_back(job);
        }
        if (links.size() == 1) {
            particle_jobs_.push_back(links.front());
        } else {
            // A step of each system in turn, each in a pass of its own so it sees what the one
            // before wrote, with the numbering of what a sub-emitter takes ahead of its step.
            for (std::size_t i = 0; i < steps; ++i) {
                for (std::size_t k = 0; k < links.size(); ++k) {
                    if (k > 0) {
                        const ParticleStep& step = chain[k].steps[chain[k].history + i];
                        particle_jobs_.push_back({.kind = ComputeKind::Emit,
                                                  .particles = links[k].particles,
                                                  .target = links[k].source,
                                                  .emit = {.parents = links[k - 1].amount,
                                                           .size = links[k].amount,
                                                           .tag = step.cycle + 1U,
                                                           .clear = step.clear ? 1U : 0U}});
                    }
                    ParticleJob job = links[k];
                    job.history += static_cast<std::uint32_t>(i);
                    job.step_count = 1;
                    particle_jobs_.push_back(job);
                }
            }
        }
        return in_frame_ ? Result<void>{} : flush_particle_jobs();
    }

    Result<void> draw_particles(ParticlesId particles, TextureId texture, const ParticleDraw& look,
                                const Material& material) override {
        if (!in_frame_) {
            return no_frame("draw_particles");
        }
        const auto it = particle_systems_.find(particles);
        if (it == particle_systems_.end()) {
            return unknown_particles(particles);
        }
        const std::uint32_t amount = it->second.amount;
        const std::uint32_t trail = it->second.trail;
        if (auto checked = check_particle_draw(look, amount, trail, frame_vertex_count());
            !checked) {
            return checked;
        }
        if (auto checked = check_draw(texture, material); !checked) {
            return checked;
        }
        if (!frame_drawn_) {
            return {}; // a skipped frame
        }
        if (auto pipeline = compute_pipeline(ComputeKind::Draw); !pipeline) {
            return pipeline.error();
        }
        // The quads go after the frame's other vertices, written there by the draw shader.
        auto batch = make_batch(texture, material, particle_vertices_);
        if (!batch) {
            return batch.error();
        }
        const std::uint32_t count = amount * particle_vertex_count(trail, look);
        batch.value().particles = true;
        batch.value().count = count;
        batches_.push_back(std::move(batch).value());
        ++stats_.draws;
        stats_.triangles += count / 3;
        // A thread a quad: a particle's, or a segment of its ribbon.
        particle_jobs_.push_back({.kind = ComputeKind::Draw,
                                  .particles = it->second.buffer,
                                  .amount = count / 6,
                                  .look = draw_uniforms(look, amount, trail, particle_vertices_)});
        particle_vertices_ += count;
        return {};
    }

    Result<std::vector<Particle>> read_particles(ParticlesId particles) override {
        if (in_frame_) {
            return frame_in_progress("read_particles");
        }
        const auto it = particle_systems_.find(particles);
        if (it == particle_systems_.end()) {
            return unknown_particles(particles);
        }
        const std::uint32_t count = it->second.amount * it->second.trail;
        const std::uint64_t size = std::uint64_t{count} * sizeof(Particle);
        WGPUBuffer readback =
            create_buffer(WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, size);
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, it->second.buffer, 0, readback, 0, size);
        submit(encoder);
        std::vector<Particle> out(count);
        auto mapped = map_and_read(readback, std::as_writable_bytes(std::span(out)));
        destroy_buffer(readback);
        if (!mapped) {
            return mapped.error();
        }
        return out;
    }

    Result<MeshId> create_mesh(std::span<const Vertex3D> vertices,
                               std::span<const std::uint32_t> indices) override {
        if (auto checked = check_mesh(vertices, indices); !checked) {
            return checked.error();
        }
        std::vector<std::uint32_t> implicit;
        if (indices.empty()) {
            implicit.resize(vertices.size());
            std::iota(implicit.begin(), implicit.end(), 0U);
            indices = implicit;
        }
        // Queue writes run ahead of the command buffers sent after them, so the next frame
        // draws the mesh.
        const Mesh mesh{.vertices = create_buffer(WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                                  vertices.size_bytes()),
                        .indices = create_buffer(WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
                                                 indices.size_bytes()),
                        .index_count = static_cast<std::uint32_t>(indices.size()),
                        .vertex_count = vertices.size(),
                        .bounds = mesh_bounds(vertices),
                        .previous = nullptr,
                        .moved = false};
        wgpuQueueWriteBuffer(queue_, mesh.vertices, 0, vertices.data(), vertices.size_bytes());
        wgpuQueueWriteBuffer(queue_, mesh.indices, 0, indices.data(), indices.size_bytes());
        const auto id = static_cast<MeshId>(next_mesh_++);
        meshes_[id] = mesh;
        return id;
    }

    Result<void> update_mesh(MeshId mesh, std::span<const Vertex3D> vertices) override {
        if (in_frame_) {
            return frame_in_progress("update_mesh");
        }
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return unknown_mesh(mesh);
        }
        if (auto checked = check_mesh_update(vertices, it->second.vertex_count); !checked) {
            return checked;
        }
        // The first update since a frame with TAA drew the mesh keeps the vertices it drew, for
        // the motion of the next one's: the buffers swap, and the new vertices go in the other.
        Mesh& found = it->second;
        if (!found.moved) {
            if (found.previous == nullptr) {
                found.previous = create_buffer(WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                               vertices.size_bytes());
            }
            std::swap(found.vertices, found.previous);
            found.moved = true;
        }
        // Queue writes land after the command buffers sent before them, so frames already sent
        // draw the old vertices.
        wgpuQueueWriteBuffer(queue_, found.vertices, 0, vertices.data(), vertices.size_bytes());
        found.bounds = mesh_bounds(vertices);
        return {};
    }

    void destroy_mesh(MeshId mesh) override {
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return;
        }
        for (WGPUBuffer buffer : {it->second.vertices, it->second.indices, it->second.previous}) {
            if (buffer == nullptr) {
                continue;
            }
            // The frame being built may still draw it, so it goes once the frame is sent.
            if (in_frame_) {
                pending_buffers_.push_back(buffer);
            } else {
                destroy_buffer(buffer);
            }
        }
        meshes_.erase(it);
    }

    // A texture draws can sample: one made, and not a reflection atlas.
    [[nodiscard]] bool usable(TextureId id) const {
        const auto it = textures_.find(id);
        return it != textures_.end() && it->second.desc.cubes == 0;
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        if (auto errors = reported_errors(); !errors) {
            return errors.error();
        }
        if (frame.scene) {
            if (auto checked = check_scene(*frame.scene); !checked) {
                return checked.error();
            }
            if (auto checked = check_post(
                    frame.scene->post, [&](TextureId id) { return usable(id); },
                    [&](TextureId id) {
                        const TextureDesc& desc = textures_.at(id).desc;
                        return std::pair{desc.width, desc.height};
                    });
                !checked) {
                return checked.error();
            }
            if (auto checked = check_sky(*frame.scene, [&](TextureId id) { return usable(id); });
                !checked) {
                return checked.error();
            }
            if (auto checked = check_reflections(*frame.scene,
                                                 [&](TextureId id) {
                                                     const auto found = textures_.find(id);
                                                     return found == textures_.end()
                                                                ? 0U
                                                                : found->second.desc.cubes;
                                                 });
                !checked) {
                return checked.error();
            }
            if (frame.scene->capture) {
                return begin_capture(frame);
            }
        }
        WGPUTexture target = nullptr;
        WGPUTextureView view = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        WGPUTextureFormat format = kTextureFormat;
        if (frame.target == TextureId::None) {
            if (surface_ == nullptr) {
                return make_error(ErrorCode::InvalidArgument,
                                  "this device has no window; draw to a render-target texture");
            }
            format = surface_format_;
        } else {
            const auto it = textures_.find(frame.target);
            if (it == textures_.end()) {
                return unknown_texture(frame.target);
            }
            if (!it->second.desc.render_target) {
                return not_a_render_target(frame.target);
            }
            view = it->second.gpu.view;
            width = it->second.desc.width;
            height = it->second.desc.height;
        }
        if (auto checked = check_lighting(
                frame.lighting, frame.target, [&](TextureId id) { return usable(id); },
                [&](TextureId id) {
                    const TextureDesc& desc = textures_.at(id).desc;
                    return std::pair{desc.width, desc.height};
                });
            !checked) {
            return checked.error();
        }
        // Made first, so a failure leaves nothing half started. Pipelines for materials are made
        // when a draw first needs them.
        if (auto pipeline = pipeline_for(format, ShaderKind::Sprite, BlendMode::Mix); !pipeline) {
            return pipeline.error();
        }
        // The frames before are kept only while the target's frames draw SSR, or TAA.
        if (!frame.scene || !uses_ssr(*frame.scene)) {
            forget_ssr_history(frame.target);
        }
        if (!frame.scene || !uses_taa(*frame.scene)) {
            forget_taa_history(frame.target);
        }
        if (!frame.scene || !uses_fsr2(*frame.scene)) {
            forget_fsr2_frames(frame.target);
        }
        if (frame.target == TextureId::None) {
            int pixels_wide = 0;
            int pixels_high = 0;
            SDL_GetWindowSizeInPixels(window_, &pixels_wide, &pixels_high);
            if (pixels_wide <= 0 || pixels_high <= 0) {
                // Hidden or zero sized: there is nothing to draw to this time.
                start_frame(frame, nullptr, 0, 0, false);
                return false;
            }
            width = static_cast<std::uint32_t>(pixels_wide);
            height = static_cast<std::uint32_t>(pixels_high);
            if (width != surface_width_ || height != surface_height_) {
                WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
                config.device = device_;
                config.format = surface_format_;
                config.usage = WGPUTextureUsage_RenderAttachment;
                config.width = width;
                config.height = height;
                config.alphaMode = WGPUCompositeAlphaMode_Opaque;
                config.presentMode = WGPUPresentMode_Fifo;
                wgpuSurfaceConfigure(surface_, &config);
                surface_width_ = width;
                surface_height_ = height;
            }
            WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
            wgpuSurfaceGetCurrentTexture(surface_, &current);
            if (current.texture == nullptr ||
                (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                 current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
                return make_error(ErrorCode::PlatformFailure,
                                  "getting the canvas's next image failed");
            }
            target = current.texture;
            view = wgpuTextureCreateView(target, nullptr);
        }
        start_frame(frame, view, width, height, true);
        frame_texture_ = target;
        frame_format_ = format;
        if (frame.scene && frame.scene->method == RenderMethod::ForwardPlus) {
            // The sky and meshes draw at FSR 2's render size, and the post passes at the frame's.
            prepare_forward(width, height, frame.scene->post.glow, frame.scene->post.auto_exposure,
                            uses_dof(frame.scene->post));
            if (uses_prepass(*frame.scene)) {
                prepare_prepass(scene_width_, scene_height_);
            }
            if (uses_ssao(*frame.scene)) {
                prepare_ssao(scene_width_, scene_height_, frame.scene->ssao.settings);
            }
            if (uses_ssr(*frame.scene)) {
                prepare_ssr(frame.target, scene_width_, scene_height_, frame.scene->ssr.settings);
            }
            if (uses_ssao(*frame.scene) || uses_ssr(*frame.scene)) {
                prepare_screen(scene_width_, scene_height_);
            }
            if (uses_taa(*frame.scene)) {
                prepare_taa(frame.target, width, height);
            }
            if (uses_fsr2(*frame.scene)) {
                prepare_fsr2(frame.target, {scene_width_, scene_height_, width, height});
            }
        } else if (frame.scene && post_processes(frame.scene->post)) {
            prepare_post(width, height);
        }
        return true;
    }

    Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices,
                      const Material& material) override {
        if (!in_frame_) {
            return no_frame("draw");
        }
        if (auto checked = check_draw(texture, material); !checked) {
            return checked;
        }
        if (auto checked = check_vertices(vertices, frame_vertex_count()); !checked) {
            return checked;
        }
        if (vertices.empty() || !frame_drawn_) {
            return {}; // nothing to draw, or a skipped frame
        }
        const auto count = static_cast<std::uint32_t>(vertices.size());
        if (batches_.empty() || batches_.back().particles || batches_.back().texture != texture ||
            batches_.back().material != material) {
            auto batch =
                make_batch(texture, material, static_cast<std::uint32_t>(vertices_.size()));
            if (!batch) {
                return batch.error();
            }
            batches_.push_back(std::move(batch).value());
            ++stats_.draws;
        }
        batches_.back().count += count;
        vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
        stats_.triangles += count / 3;
        return {};
    }

    Result<void> draw_mesh(MeshId mesh, const Mat4& transform, const Material3D& material,
                           std::uint32_t layers, ShadowCasting shadows,
                           const std::optional<Mat4>& previous) override {
        if (!in_frame_) {
            return no_frame("draw_mesh");
        }
        const auto it = meshes_.find(mesh);
        if (auto checked =
                check_mesh_draw(frame_.scene.has_value(), it != meshes_.end(), mesh, transform,
                                material, frame_.target, [&](TextureId id) { return usable(id); });
            !checked) {
            return checked;
        }
        if (auto checked = check_previous_transform(previous); !checked) {
            return checked;
        }
        if (!frame_drawn_) {
            return {}; // a skipped frame
        }
        make_depth_target(scene_width_, scene_height_);
        prepare_shadows(*frame_.scene);
        prepare_radiance(*frame_.scene);
        MeshBatch batch{
            .vertices = it->second.vertices,
            .indices = it->second.indices,
            .index_count = it->second.index_count,
            .bounds = it->second.bounds,
            .pipeline = mesh_pipeline_for(world_format(*frame_.scene, frame_format_), material,
                                          frame_.scene->method == RenderMethod::ForwardPlus),
            .normal_pipeline = nullptr,
            .bindings = {},
            .previous_vertices = it->second.moved ? it->second.previous : it->second.vertices};
        // SSAO's, SSR's, TAA's and FSR 2's prepass draws the opaque meshes' normals, and with TAA
        // or FSR 2 their motion vectors.
        if (uses_prepass(*frame_.scene) && material.transparency != Transparency::Alpha) {
            batch.normal_pipeline = uses_motion_vectors(*frame_.scene)
                                        ? motion_pipeline_for(material.cull)
                                        : normal_pipeline_for(material.cull);
        }
        const std::array<MaterialTexture, kMeshTextures> slots = mesh_texture_slots(material);
        for (std::size_t slot = 0; slot < kMeshTextures; ++slot) {
            batch.bindings.at(slot) = binding(slots.at(slot).texture, slots.at(slot).repeat);
        }
        mesh_draws_.push_back({.mesh = mesh,
                               .transform = transform,
                               .material = material,
                               .layers = layers,
                               .shadows = shadows,
                               .previous = previous.value_or(transform)});
        mesh_batches_.push_back(batch);
        ++stats_.draws;
        stats_.triangles += it->second.index_count / 3;
        return {};
    }

    Result<void> end_frame() override {
        if (!in_frame_) {
            return no_frame("end_frame");
        }
        in_frame_ = false;
        if (!frame_drawn_) {
            // A skipped frame draws nothing, but its particles still run.
            Result<void> flushed = flush_particle_jobs();
            finish_frame();
            return flushed;
        }
        // The uniforms every pass reads, then the buffers they need, all written ahead of the
        // command buffer that reads them.
        uniforms_.clear();
        const auto base = static_cast<std::uint32_t>(vertices_.size());
        std::vector<std::uint32_t> job_uniforms = particle_job_uniforms(base);
        const Mat4 view = frame_.view.value_or(
            pixel_projection(static_cast<float>(frame_width_), static_cast<float>(frame_height_)));
        std::optional<std::uint32_t> lights;
        if (std::ranges::any_of(
                batches_, [](const Batch& batch) { return batch.kind == ShaderKind::Material; })) {
            lights = uniforms_.add(lights_uniforms(frame_.lighting));
        }
        struct BatchUniforms {
            std::uint32_t view = 0;
            std::uint32_t vertex_program = 0; // a game shader's
            std::uint32_t program = 0;
        };
        std::vector<BatchUniforms> batch_uniforms;
        batch_uniforms.reserve(batches_.size());
        std::optional<std::pair<Mat4, std::uint32_t>> pushed;
        for (const Batch& batch : batches_) {
            // The view, times the draw's transform if it has one, added when it changes.
            const Mat4 matrix =
                batch.material.transform ? multiply(view, *batch.material.transform) : view;
            if (!pushed || pushed->first != matrix) {
                pushed.emplace(matrix, uniforms_.add(transposed(matrix)));
            }
            BatchUniforms at{.view = pushed->second, .vertex_program = 0, .program = 0};
            if (batch.kind == ShaderKind::Material) {
                at.program = uniforms_.add(program_uniforms(batch.material, batch.texel));
            }
            if (batch.material.shader != ShaderId::None) {
                at.vertex_program =
                    uniforms_.add(vertex_program_uniforms(batch.material, batch.texel));
            }
            batch_uniforms.push_back(at);
        }
        const bool forward = frame_.scene && frame_.scene->method == RenderMethod::ForwardPlus;
        // FSR 2's frame: its clock's next frame, whose point of the jitter phases moves the camera,
        // even with no meshes, as the software device's.
        Fsr2Frames* fsr2 =
            forward && uses_fsr2(*frame_.scene) ? find_fsr2_frames(frame_.target) : nullptr;
        std::array<float, 2> jitter{};
        if (fsr2 != nullptr) {
            const std::uint32_t phases = fsr2_phase_count(scene_width_, frame_width_);
            fsr2->clock.advance(phases);
            jitter = camera_jitter(fsr2->clock.frames, phases, scene_width_, scene_height_);
        }
        const MeshUniforms mesh_uniforms = add_mesh_uniforms(fsr2, jitter);
        const std::optional<ProbeCapture> capture =
            frame_.scene ? frame_.scene->capture : std::nullopt;
        const bool post = !forward && frame_.scene && post_processes(frame_.scene->post);
        const bool sky = frame_.scene && sky_draws(*frame_.scene);
        // Depth of field and FSR 2 need the depth buffer cleared, even with nothing drawn over it.
        const bool dof = forward && uses_dof(frame_.scene->post);
        if (dof || fsr2 != nullptr) {
            make_depth_target(scene_width_, scene_height_);
        }
        std::optional<std::uint32_t> sky_uniforms_at;
        if (sky) {
            SkyUniforms uniforms = sky_uniforms(*frame_.scene, scene_width_, scene_height_);
            uniforms.orientation = transposed(uniforms.orientation);
            sky_uniforms_at = uniforms_.add(uniforms);
            make_depth_target(scene_width_, scene_height_);
        }
        std::vector<PostDraw> post_draws;
        if (capture) {
            // drawn into the raw cube alone
        } else if (forward) {
            post_draws = forward_draws_for(*frame_.scene);
        } else if (post) {
            post_draws = post_draws_for(frame_.scene->post);
        }
        const RadianceOffsets radiance_offsets =
            radiance_draws_ ? add_radiance_uniforms(*frame_.scene) : RadianceOffsets{};
        // SSAO's passes and their uniforms, in a frame with meshes to light.
        std::optional<SsaoPlan> ssao;
        std::vector<std::uint32_t> ssao_uniforms_at;
        if (forward && !mesh_draws_.empty() && uses_ssao(*frame_.scene)) {
            ssao = ssao_plan(frame_.scene->ssao.settings, scene_width_, scene_height_);
            for (const SsaoStep& step : ssao->steps) {
                ssao_uniforms_at.push_back(
                    uniforms_.add(ssao_uniforms(frame_.scene->ssao, step, frame_.scene->projection,
                                                scene_width_, scene_height_)));
            }
        }
        // SSR's passes (none for the copies) and the last frame's, and their uniforms. SSR
        // reflects the frame before, which a target's first frame draws first without it.
        std::optional<SsrPlan> ssr;
        std::vector<std::uint32_t> ssr_uniforms_at;
        std::uint32_t last_frame_at = 0;
        SsrHistory* history = nullptr;
        SsrCamera seen;
        if (forward && !mesh_draws_.empty() && uses_ssr(*frame_.scene)) {
            const Scene3D& scene = *frame_.scene;
            history = &ssr_history(frame_.target);
            seen = ssr_camera(scene);
            const SsrCamera last = history->primed ? history->camera : seen;
            ssr = ssr_plan(scene.ssr.settings, scene_width_, scene_height_);
            for (const SsrStep& step : ssr->steps) {
                ssr_uniforms_at.push_back(
                    step.pass == SsrPass::CopyLevels
                        ? 0
                        : uniforms_.add(gpu_ssr_uniforms(ssr_uniforms(
                              scene.ssr, step, seen, last, scene_width_, scene_height_))));
            }
            last_frame_at = uniforms_.add(gpu_ssr_uniforms(ssr_uniforms(
                scene.ssr, ssr_last_frame_step(), seen, seen, scene_width_, scene_height_)));
        }
        // TAA's pass, in a frame with meshes to blend with the target's frames before.
        TaaHistory* taa = forward && !mesh_draws_.empty() && uses_taa(*frame_.scene)
                              ? find_taa_history(frame_.target)
                              : nullptr;
        const std::uint32_t taa_uniforms_at =
            taa != nullptr ? uniforms_.add(taa_uniforms(frame_width_, frame_height_)) : 0;
        // TAA blends frames that draw meshes; one without starts it again.
        if (mesh_draws_.empty() && frame_.scene && uses_taa(*frame_.scene)) {
            if (TaaHistory* frames = find_taa_history(frame_.target); frames != nullptr) {
                frames->primed = false;
            }
        }
        // FSR 2's passes' uniforms, then the reconstruct pass's: it clears the depths, then
        // scatters them.
        std::array<std::uint32_t, 3> fsr2_uniforms_at{};
        if (fsr2 != nullptr) {
            Fsr2Uniforms uniforms = fsr2_uniforms({.render_width = scene_width_,
                                                   .render_height = scene_height_,
                                                   .display_width = frame_width_,
                                                   .display_height = frame_height_,
                                                   .jitter = jitter,
                                                   .projection = frame_.scene->projection,
                                                   .sharpness = frame_.scene->fsr2->sharpness,
                                                   .frame_index = fsr2->clock.frames,
                                                   .phases = fsr2->clock.phases});
            fsr2_uniforms_at[0] = uniforms_.add(uniforms);
            for (const std::int32_t step : {0, 1}) {
                uniforms.options[2] = step;
                fsr2_uniforms_at.at(static_cast<std::size_t>(1 + step)) = uniforms_.add(uniforms);
            }
        }
        std::optional<ProbeOffsets> probe_offsets;
        if (capture && capture->face + 1 == kCubeFaces) {
            probe_offsets = add_probe_uniforms();
        }
        upload_frame();

        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        // Particles run and write their quads ahead of the render pass that draws them.
        encode_particle_jobs(encoder, job_uniforms);
        // The sky and meshes draw first, in a pass of their own, and the 2D draws go over them.
        if (!mesh_draws_.empty()) {
            encode_shadows(encoder, mesh_uniforms);
        }
        if (radiance_draws_) {
            encode_radiance(encoder, *frame_.scene, radiance_offsets);
        }
        // The prepass draws the motion vectors into TAA's history's next texture, or FSR 2's own.
        WGPUTextureView velocity = nullptr;
        if (taa != nullptr) {
            velocity = taa->velocity.at(1 - taa->last).view;
        } else if (fsr2 != nullptr && !mesh_draws_.empty()) {
            velocity = fsr2_textures_.velocity.view;
        }
        if (ssao || ssr || velocity != nullptr) {
            encode_prepass(encoder, mesh_uniforms, velocity);
        } else if (fsr2 != nullptr) {
            // Nothing moved in a frame with no meshes.
            clear_texture(encoder, fsr2_textures_.velocity.view);
        }
        if (ssao) {
            encode_ssao(encoder, *ssao, ssao_uniforms_at);
        }
        if (ssr && !history->primed) {
            encode_meshes(encoder, mesh_uniforms, sky_uniforms_at, ssao.has_value(), false);
            encode_ssr_step(encoder, ssr_last_frame_step(), last_frame_at, *history);
        }
        if (ssr) {
            encode_ssr(encoder, *ssr, ssr_uniforms_at, *history);
        }
        if (!mesh_draws_.empty() || sky || dof || capture || fsr2 != nullptr) {
            encode_meshes(encoder, mesh_uniforms, sky_uniforms_at, ssao.has_value(),
                          ssr.has_value());
        }
        if (taa != nullptr) {
            encode_taa(encoder, *taa, taa_uniforms_at,
                       multiply(frame_.scene->projection, frame_.scene->camera));
        }
        if (fsr2 != nullptr) {
            encode_fsr2(encoder, *fsr2, fsr2_uniforms_at,
                        multiply(frame_.scene->projection, frame_.scene->camera));
        }
        if (capture) {
            if (probe_offsets) {
                encode_probe(encoder, *capture, *probe_offsets);
            }
            submit(encoder);
            finish_frame();
            return {};
        }
        if (ssr) {
            encode_ssr_step(encoder, ssr_last_frame_step(), last_frame_at, *history);
            history->camera = seen;
            history->primed = true;
        }
        if (forward || post) {
            encode_post(encoder, post_draws);
        }
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = frame_view_;
        color.loadOp =
            mesh_draws_.empty() && !sky && !post && !forward ? WGPULoadOp_Clear : WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {
            .r = frame_.clear.r, .g = frame_.clear.g, .b = frame_.clear.b, .a = frame_.clear.a};
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (!batches_.empty()) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buffer_, 0, WGPU_WHOLE_SIZE);
            WGPURenderPipeline bound = nullptr;
            for (std::size_t i = 0; i < batches_.size(); ++i) {
                const Batch& batch = batches_[i];
                if (batch.pipeline != bound) {
                    wgpuRenderPassEncoderSetPipeline(pass, batch.pipeline);
                    bound = batch.pipeline;
                }
                const std::array<std::uint32_t, 2> vertex_offsets{batch_uniforms[i].view,
                                                                  batch_uniforms[i].vertex_program};
                wgpuRenderPassEncoderSetBindGroup(pass, 1, vertex_uniforms_group_,
                                                  vertex_offsets.size(), vertex_offsets.data());
                wgpuRenderPassEncoderSetBindGroup(pass, 2, texture_group(batch), 0, nullptr);
                if (batch.kind == ShaderKind::Material) {
                    const std::array<std::uint32_t, 2> fragment_offsets{batch_uniforms[i].program,
                                                                        lights.value_or(0)};
                    wgpuRenderPassEncoderSetBindGroup(pass, 3, fragment_uniforms_group_,
                                                      fragment_offsets.size(),
                                                      fragment_offsets.data());
                }
                wgpuRenderPassEncoderDraw(pass, batch.count, 1,
                                          batch.particles ? base + batch.first : batch.first, 0);
            }
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        submit(encoder);
        ++stats_.frames;
        finish_frame();
        return {};
    }

    Result<Image> read_texture(TextureId texture) override {
        if (in_frame_) {
            return frame_in_progress("read_texture");
        }
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (!desc.render_target) {
            return not_a_render_target(texture);
        }
        // A copy's rows start at multiples of 256 bytes, so they are read out one by one.
        const std::uint32_t row = desc.width * 4;
        const std::uint32_t stride =
            (row + kCopyRowAlignment - 1) / kCopyRowAlignment * kCopyRowAlignment;
        const std::uint64_t size = std::uint64_t{stride} * desc.height;
        WGPUBuffer readback =
            create_buffer(WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, size);
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        source.texture = it->second.gpu.texture;
        WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        destination.buffer = readback;
        destination.layout.bytesPerRow = stride;
        destination.layout.rowsPerImage = desc.height;
        const WGPUExtent3D extent{
            .width = desc.width, .height = desc.height, .depthOrArrayLayers = 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
        submit(encoder);
        std::vector<std::byte> rows(static_cast<std::size_t>(size));
        auto mapped = map_and_read(readback, rows);
        destroy_buffer(readback);
        if (!mapped) {
            return mapped.error();
        }
        Image image{.width = desc.width, .height = desc.height, .pixels = {}};
        image.pixels.resize(std::size_t{row} * desc.height);
        for (std::uint32_t y = 0; y < desc.height; ++y) {
            std::memcpy(image.pixels.data() + (std::size_t{y} * row),
                        rows.data() + (std::size_t{y} * stride), row);
        }
        return image;
    }

    // Browsers show a canvas's frames when the display refreshes, and WebGPU on the web presents
    // no other way, so V-Sync is always on there.
    VsyncMode set_vsync(VsyncMode mode) override {
        vsync_ = surface_ == nullptr ? mode : VsyncMode::Enabled;
        return vsync_;
    }
    [[nodiscard]] VsyncMode vsync() const override { return vsync_; }

    [[nodiscard]] Stats stats() const override {
        Stats stats = stats_;
        stats.textures = static_cast<std::uint32_t>(textures_.size());
        for (const auto& [id, texture] : textures_) {
            TextureDesc stored = texture.desc;
            stored.format = texture.stored;
            stats.texture_bytes += texture_bytes(stored);
        }
        return stats;
    }

private:
    struct GpuTexture {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        Filter filter = Filter::Linear;
    };

    struct Texture {
        GpuTexture gpu;
        // As it was made, in the format update_texture takes.
        TextureDesc desc;
        // The format on the GPU: desc's, or RGBA8 where the GPU doesn't sample that.
        TextureFormat stored = TextureFormat::Rgba8;
    };

    // The textures and samplers a batch binds: a key for the group that binds them.
    using TextureBindings = std::array<std::pair<WGPUTextureView, WGPUSampler>,
                                       std::max(kMaterialSamplers, kMeshTextures + 5)>;
    struct BindingsHash {
        std::size_t operator()(const TextureBindings& bindings) const {
            std::size_t hash = 0;
            for (const auto& [view, sampler] : bindings) {
                hash = (hash * 31) ^ std::hash<const void*>{}(view);
                hash = (hash * 31) ^ std::hash<const void*>{}(sampler);
            }
            return hash;
        }
    };

    // Draws in a row with the same texture and material, sent as one GPU draw.
    struct Batch {
        TextureId texture = TextureId::None;
        Material material;
        ShaderKind kind = ShaderKind::Sprite;
        WGPURenderPipeline pipeline = nullptr;
        // The draw's texture, the material's, the shading's and the frame's lighting textures,
        // with their samplers; a sprite has only the first.
        TextureBindings bindings{};
        std::array<float, 4> texel{};
        // Its first vertex: in the frame's particle quads, after the other vertices, for
        // particles.
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        bool particles = false; // a draw of particles, which is never batched with others
    };

    // The post passes' textures: the source, glow and the color correction.
    static constexpr std::uint32_t kPostTextures = 3;
    // A post pass: its target and pipeline, the textures it reads (post.hlsl's three or
    // forward_post.hlsl's nine) and its uniforms' offset.
    struct PostDraw {
        WGPUTextureView target = nullptr;
        WGPURenderPipeline pipeline = nullptr;
        std::vector<WGPUTextureView> textures;
        std::uint32_t uniforms = 0;
    };
    // What a Forward+ frame draws with: the world's texture of half floats, glow's levels, made
    // with the first frame that glows, and auto exposure's luminance textures and measured
    // brightness, made with the first frame that uses it: whether it holds one yet, and the version
    // it was measured for.
    struct ForwardTargets {
        GpuTexture world;
        std::array<GpuTexture, kMaxGlowLevels> glow{};
        std::vector<GpuTexture> luminance;
        GpuTexture brightness;
        bool measured = false;
        std::uint64_t version = 0;
        // Depth of field's textures, as DofTexture after World, made with the first frame that
        // uses it.
        std::array<GpuTexture, 5> dof{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    // What a frame that post-processes draws with: the meshes' texture and glow's.
    struct PostTargets {
        GpuTexture world;
        std::array<GpuTexture, kGlowLevels> glow{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // A mesh draw's textures and samplers, in mesh.hlsl's order, then the shadow atlases, the
    // radiance, the SSAO and the SSR.
    using MeshBindings = std::array<std::pair<WGPUTextureView, WGPUSampler>, kMeshTextures + 5>;
    // They share texture_groups_ with batches: a mesh's key binds a cube texture past every slot a
    // material batch's binds.
    static_assert(kMeshTextures + 2 >= kMaterialSamplers);
    static_assert(kMeshTextures + 5 <= kMeshSamplerBinding);

    // A cube texture of the radiance's format: a view of every level, which meshes and the
    // filter sample, a view of each level alone, which each next level's mipmaps are drawn from,
    // and a view of each face of each level to draw into (level * kCubeFaces + face). A
    // reflection atlas has no level views, and its faces are each cube's (level * its layers +
    // cube * kCubeFaces + face).
    struct CubeTexture {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        std::vector<WGPUTextureView> levels;
        std::vector<WGPUTextureView> faces;
    };
    // The array the mesh pass reads SSAO and SSR from: its view, and each layer's to draw into.
    struct ScreenArray {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        std::array<WGPUTextureView, 2> layers{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    // Where a capture's filter uniforms are, for each level and face: its mipmaps' (from the
    // second level), then its filters'.
    struct ProbeOffsets {
        std::vector<std::uint32_t> mipmaps;
        std::vector<std::uint32_t> filters;
    };
    // Where the radiance passes' uniforms are: the sky's for each face, then for each level and
    // face, the mipmap's and the filter's.
    struct RadianceOffsets {
        std::array<std::uint32_t, kCubeFaces> faces{};
        std::vector<std::uint32_t> mipmaps;
        std::vector<std::uint32_t> filters;
    };

    // A mesh's buffers; one without indices is given them in order. Once update_mesh moves its
    // vertices, previous holds them as the last frame with TAA that drew the mesh saw them,
    // until another such frame draws it.
    struct Mesh {
        WGPUBuffer vertices = nullptr;
        WGPUBuffer indices = nullptr;
        std::uint32_t index_count = 0;
        std::size_t vertex_count = 0;
        Bounds bounds;
        WGPUBuffer previous = nullptr;
        bool moved = false;
    };

    // What a mesh draw binds, found when it is made: its mesh may be destroyed before end_frame.
    struct MeshBatch {
        WGPUBuffer vertices = nullptr;
        WGPUBuffer indices = nullptr;
        std::uint32_t index_count = 0;
        Bounds bounds;
        WGPURenderPipeline pipeline = nullptr;
        // The prepass's, for an opaque mesh in a frame with SSAO, SSR or TAA.
        WGPURenderPipeline normal_pipeline = nullptr;
        MeshBindings bindings{};
        // Where its vertices were in the frame before, which TAA's prepass reads.
        WGPUBuffer previous_vertices = nullptr;
    };

    struct MeshPipeline {
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        CullMode cull = CullMode::Back;
        bool blended = false;             // an Alpha material's, which writes no depth
        BlendMode blend = BlendMode::Mix; // when blended
        bool clustered = false;           // Forward+'s, which reads the clusters
        WGPURenderPipeline pipeline = nullptr;
    };

    // A storage buffer the frame's data is written to, grown when it doesn't fit.
    struct StorageBuffer {
        WGPUBuffer buffer = nullptr;
        std::uint64_t capacity = 0;
    };

    // Where the frame's mesh uniforms are: the camera's, the scene's (and with SSR, the scene's
    // without it, for a target's first frame) and the shadows', each shadow pass's, then each
    // draw's instance and surface.
    struct MeshUniforms {
        std::uint32_t camera = 0;
        std::uint32_t scene = 0;
        std::uint32_t unreflected = 0;
        std::uint32_t shadows = 0;
        std::vector<std::uint32_t> passes;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> draws;
    };

    // A shadow atlas: a depth texture the shadow passes draw into and the mesh pass samples.
    struct ShadowAtlasTexture {
        GpuTexture gpu;
        std::uint32_t size = 0;
    };

    // A target's frame before, which SSR reflects, at SSR's size, and the camera it was seen
    // from; primed once it holds a frame.
    struct SsrHistory {
        GpuTexture texture;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SsrCamera camera;
        bool primed = false;
    };

    // A target's frames before for TAA, at the frame's size: how many it drew with TAA, which
    // picks the next jitter, the last one's camera without its jitter, and two results and two
    // motion vector textures that frames take turns to draw into, last the pair the last frame
    // drew.
    struct TaaHistory {
        std::uint64_t frames = 0;
        Mat4 view_projection{};
        std::array<GpuTexture, 2> color{};
        std::array<GpuTexture, 2> velocity{};
        std::size_t last = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool primed = false;
    };

    // A target's frames before for FSR 2: its clock, the last one's camera without its jitter, and
    // pairs of the accumulate pass's color, locks and luma history at the frame's size and the
    // dilate pass's motion vectors at the render size that frames take turns to draw into, last
    // the pair the last frame drew, made for sizes (the render size, then the frame's).
    struct Fsr2Frames {
        Fsr2Clock clock;
        Mat4 view_projection{};
        std::array<GpuTexture, 2> color{};
        std::array<GpuTexture, 2> lock_status{};
        std::array<GpuTexture, 2> luma_history{};
        std::array<GpuTexture, 2> dilated_motion{};
        std::size_t last = 0;
        std::array<std::uint32_t, 4> sizes{};
    };

    struct Pipeline {
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        ShaderKind kind = ShaderKind::Sprite;
        BlendMode blend = BlendMode::Mix;
        ShaderId shader = ShaderId::None; // a game's, which takes the material shader's place
        WGPURenderPipeline pipeline = nullptr;
    };

    // A game's shader: the material shader's stages with the game's code in them.
    struct GameShader {
        WGPUShaderModule vertex = nullptr;
        WGPUShaderModule fragment = nullptr;
    };

    struct ParticleSystem {
        WGPUBuffer buffer = nullptr; // its particles, then its trails' copies
        std::uint32_t amount = 0;
        std::uint32_t trail = 1;
        // What its particles start in the next system of a chain, made when it is in one.
        WGPUBuffer emissions = nullptr;
    };

    // A compute pass for the GPU to run ahead of the frame's draws, in the order called: steps
    // of a system, numbering what a sub-emitter takes, or a draw turning a system into quads.
    struct ParticleJob {
        ComputeKind kind = ComputeKind::Process;
        WGPUBuffer particles = nullptr;
        std::uint32_t amount = 0; // threads: particles, or a draw's quads
        std::uint32_t trail = 1;
        bool reset = false;         // clears every particle first
        std::uint32_t material = 0; // where its process material is in particle_words_
        std::uint32_t steps = 0;    // and its steps, from those already run
        std::uint32_t history = 0;  // how many of them were
        std::uint32_t step_count = 0;
        // The emissions a sub-emitter takes and those its particles make, in a chain; the
        // emissions an Emit job numbers are its target.
        WGPUBuffer source = nullptr;
        WGPUBuffer target = nullptr;
        std::uint32_t chain = 0; // kChainTakes and kChainEmits
        EmitUniforms emit{};
        DrawUniforms look{}; // for a draw; its first vertex is in the particle quads
    };

    static std::size_t sampler_index(Filter filter, bool repeat) {
        return (static_cast<std::size_t>(filter) * 2) + (repeat ? 1 : 0);
    }

    // A texture's view and sampler; None is white. The texture must exist.
    std::pair<WGPUTextureView, WGPUSampler> binding(TextureId texture, bool repeat) const {
        const GpuTexture& gpu = texture == TextureId::None ? white_ : textures_.at(texture).gpu;
        return {gpu.view, samplers_.at(sampler_index(gpu.filter, repeat))};
    }

    // The checks every draw makes: its texture, its material and the frame's lights.
    Result<void> check_draw(TextureId texture, const Material& material) const {
        if (texture != TextureId::None) {
            if (!textures_.contains(texture)) {
                return unknown_texture(texture);
            }
            if (texture == frame_.target) {
                return target_in_use(texture);
            }
        }
        if (auto checked = check_material(
                material, frame_.target, [&](TextureId id) { return usable(id); },
                [&](ShaderId id) { return game_shaders_.contains(id); });
            !checked) {
            return checked;
        }
        return check_lighting_textures(frame_.lighting, [&](TextureId id) { return usable(id); });
    }

    // A batch drawing with a texture and a material from a first vertex, with no vertices yet.
    Result<Batch> make_batch(TextureId texture, const Material& material, std::uint32_t first) {
        const ShaderKind kind = material.shader != ShaderId::None || uses_material_shader(material)
                                    ? ShaderKind::Material
                                    : ShaderKind::Sprite;
        auto pipeline = pipeline_for(frame_format_, kind, material.blend, material.shader);
        if (!pipeline) {
            return pipeline.error();
        }
        Batch batch{.texture = texture,
                    .material = material,
                    .kind = kind,
                    .pipeline = pipeline.value(),
                    .bindings = {},
                    .texel = {1.0F, 1.0F, 0.0F, 0.0F},
                    .first = first,
                    .count = 0};
        batch.bindings.at(0) = binding(texture, false);
        if (kind == ShaderKind::Material) {
            for (std::size_t slot = 1; slot < kMaterialTextureSlots; ++slot) {
                const MaterialTexture& bound = material.textures.at(slot - 1);
                batch.bindings.at(slot) = binding(bound.texture, bound.repeat);
            }
            batch.bindings.at(kMaterialTextureSlots) = binding(material.shading.normal_map, false);
            batch.bindings.at(kMaterialTextureSlots + 1) =
                binding(material.shading.specular_map, false);
            batch.bindings.at(kMaterialTextureSlots + 2) = binding(frame_.lighting.atlas, false);
            batch.bindings.at(kMaterialTextureSlots + 3) =
                binding(frame_.lighting.shadow_map, false);
        }
        if (texture != TextureId::None) {
            const TextureDesc& desc = textures_.at(texture).desc;
            batch.texel = {1.0F / static_cast<float>(desc.width),
                           1.0F / static_cast<float>(desc.height), 0.0F, 0.0F};
        }
        return batch;
    }

    // The vertices the frame draws so far, its particles' quads included.
    [[nodiscard]] std::size_t frame_vertex_count() const {
        return vertices_.size() + particle_vertices_;
    }

    // Where the device can wait for the GPU (Asyncify), errors are caught in error scopes, which
    // the next wait reports, so a call that reads back sees the errors the calls before it made.
    // Elsewhere they reach web/webgpu.js's listener, which reported_errors reads.
    void push_error_scopes() {
        for (const WGPUErrorFilter filter : kErrorFilters) {
            wgpuDevicePushErrorScope(device_, filter);
        }
    }

    // The errors caught since the scopes were pushed, logged, as one error, with new scopes
    // pushed.
    Result<void> scoped_errors() {
        std::string message;
        for (std::size_t i = 0; i < kErrorFilters.size(); ++i) {
            struct Popped {
                WGPUErrorType type = WGPUErrorType_Unknown;
                std::string message;
            } popped;
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_WaitAnyOnly;
            callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type,
                                   WGPUStringView text, void* userdata, void* /*unused*/) {
                auto* out = static_cast<Popped*>(userdata);
                out->type =
                    status == WGPUPopErrorScopeStatus_Success ? type : WGPUErrorType_Unknown;
                out->message = view_string(text);
            };
            callback.userdata1 = &popped;
            WGPUFutureWaitInfo wait = WGPU_FUTURE_WAIT_INFO_INIT;
            wait.future = wgpuDevicePopErrorScope(device_, callback);
            if (wgpuInstanceWaitAny(instance_, 1, &wait, UINT64_MAX) != WGPUWaitStatus_Success) {
                popped.message = "waiting for the device's errors failed";
            } else if (popped.type == WGPUErrorType_NoError) {
                continue;
            }
            log::error("WebGPU reported an error", {{"error", popped.message}});
            message += (message.empty() ? "" : "; ") + popped.message;
        }
        push_error_scopes();
        if (auto reported = reported_errors(); !reported) {
            return reported;
        }
        if (message.empty()) {
            return {};
        }
        return make_error(ErrorCode::PlatformFailure, "WebGPU reported: " + message);
    }

    // The errors the browser reported since the last call, logged, as one error.
    Result<void> reported_errors() {
        std::string message;
        while (char* error = slash_webgpu_take_error()) {
            const std::string text = take_string(error);
            log::error("WebGPU reported an error", {{"error", text}});
            message += (message.empty() ? "" : "; ") + text;
        }
        if (message.empty()) {
            return {};
        }
        return make_error(ErrorCode::PlatformFailure, "WebGPU reported: " + message);
    }

    WGPUBuffer create_buffer(WGPUBufferUsage usage, std::uint64_t size) {
        WGPUBufferDescriptor info = WGPU_BUFFER_DESCRIPTOR_INIT;
        info.usage = usage;
        info.size = std::max<std::uint64_t>(size, 4);
        return wgpuDeviceCreateBuffer(device_, &info);
    }

    static void destroy_buffer(WGPUBuffer buffer) {
        if (buffer != nullptr) {
            wgpuBufferDestroy(buffer);
            wgpuBufferRelease(buffer);
        }
    }

    void submit(WGPUCommandEncoder encoder) {
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(queue_, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
    }

    // Maps a buffer the GPU wrote and copies out its first bytes, once the GPU is done with it.
    // Waiting needs a build whose code can wait for the browser (Asyncify), which games don't
    // need: they never read back.
    Result<void> map_and_read(WGPUBuffer buffer, std::span<std::byte> out) {
        if (!can_wait_) {
            return make_error(ErrorCode::PlatformFailure,
                              "reading back waits for the GPU, which this web build can't: it "
                              "needs Asyncify (-sASYNCIFY)");
        }
        struct Mapping {
            bool done = false;
            WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
            std::string message;
        } mapping;
        WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata,
                               void* /*unused*/) {
            auto* result = static_cast<Mapping*>(userdata);
            result->done = true;
            result->status = status;
            result->message = view_string(message);
        };
        callback.userdata1 = &mapping;
        WGPUFutureWaitInfo wait = WGPU_FUTURE_WAIT_INFO_INIT;
        wait.future = wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, out.size(), callback);
        const WGPUWaitStatus waited = wgpuInstanceWaitAny(instance_, 1, &wait, UINT64_MAX);
        if (auto errors = scoped_errors(); !errors) {
            return errors;
        }
        if (waited != WGPUWaitStatus_Success || !mapping.done ||
            mapping.status != WGPUMapAsyncStatus_Success) {
            return make_error(ErrorCode::PlatformFailure,
                              "waiting for the GPU failed" +
                                  (mapping.message.empty() ? "" : ": " + mapping.message));
        }
        const WGPUStatus read = wgpuBufferReadMappedRange(buffer, 0, out.data(), out.size());
        wgpuBufferUnmap(buffer);
        if (read != WGPUStatus_Success) {
            return make_error(ErrorCode::PlatformFailure, "reading the GPU's results failed");
        }
        return {};
    }

    void make_layouts() {
        const auto layout = [&](std::span<const WGPUBindGroupLayoutEntry> entries) {
            WGPUBindGroupLayoutDescriptor info = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            info.entryCount = entries.size();
            info.entries = entries.data();
            return wgpuDeviceCreateBindGroupLayout(device_, &info);
        };
        empty_layout_ = layout({});
        const std::array vertex{
            uniform_entry(0, WGPUShaderStage_Vertex, sizeof(Mat4)),
            uniform_entry(1, WGPUShaderStage_Vertex, sizeof(VertexProgramUniforms))};
        vertex_uniforms_layout_ = layout(vertex);
        const std::array sprite{texture_entry(0), sampler_entry(kSamplerBinding)};
        sprite_textures_layout_ = layout(sprite);
        std::array<WGPUBindGroupLayoutEntry, kMaterialSamplers * 2> material{};
        for (std::uint32_t slot = 0; slot < kMaterialSamplers; ++slot) {
            material.at(slot) = texture_entry(slot);
            material.at(kMaterialSamplers + slot) = sampler_entry(kSamplerBinding + slot);
        }
        material_textures_layout_ = layout(material);
        const std::array fragment{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(ProgramUniforms)),
            uniform_entry(1, WGPUShaderStage_Fragment, sizeof(LightsUniforms))};
        fragment_uniforms_layout_ = layout(fragment);
        // The steps' words and a sub-emitter's source; the particles and what they start.
        const std::array process_inputs{storage_entry(0, true), storage_entry(1, true)};
        process_inputs_layout_ = layout(process_inputs);
        const std::array process_outputs{storage_entry(0, false), storage_entry(1, false)};
        process_outputs_layout_ = layout(process_outputs);
        const std::array draw_inputs{storage_entry(0, true)};
        draw_inputs_layout_ = layout(draw_inputs);
        const std::array draw_outputs{storage_entry(0, false)};
        draw_outputs_layout_ = layout(draw_outputs);
        // The emissions, and the sub-emitter whose first particle keeps its count.
        emit_outputs_layout_ = layout(process_outputs);
        const std::array compute{uniform_entry(0, WGPUShaderStage_Compute, kComputeUniformBytes)};
        compute_uniforms_layout_ = layout(compute);
        const std::array mesh_vertex{
            uniform_entry(0, WGPUShaderStage_Vertex, sizeof(MeshCameraUniforms)),
            uniform_entry(1, WGPUShaderStage_Vertex, sizeof(MeshInstanceUniforms))};
        mesh_vertex_layout_ = layout(mesh_vertex);
        constexpr std::uint32_t kMeshSlots = kMeshTextures + 5;
        std::array<WGPUBindGroupLayoutEntry, kMeshSlots * 2> mesh_textures{};
        for (std::uint32_t slot = 0; slot < kMeshSlots; ++slot) {
            const bool atlas = slot == kMeshTextures || slot == kMeshTextures + 1;
            mesh_textures.at(slot) = atlas ? depth_entry(slot) : texture_entry(slot);
            mesh_textures.at(kMeshSlots + slot) = atlas
                                                      ? comparison_entry(kMeshSamplerBinding + slot)
                                                      : sampler_entry(kMeshSamplerBinding + slot);
        }
        mesh_textures.at(kMeshTextures + 2) = cube_entry(kMeshTextures + 2);
        // The screen array of SSAO and SSR, and the reflection probes' cubes.
        mesh_textures.at(kMeshTextures + 3).texture.viewDimension =
            WGPUTextureViewDimension_2DArray;
        mesh_textures.at(kMeshTextures + 4).texture.viewDimension =
            WGPUTextureViewDimension_CubeArray;
        mesh_textures_layout_ = layout(mesh_textures);
        const std::array mesh_fragment{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(MeshSurfaceUniforms)),
            uniform_entry(1, WGPUShaderStage_Fragment, sizeof(MeshSceneUniforms)),
            uniform_entry(2, WGPUShaderStage_Fragment, sizeof(MeshShadowUniforms))};
        mesh_fragment_layout_ = layout(mesh_fragment);
        // Forward+'s clusters: the lights, then each tile's words, then the reflection probes.
        const std::array mesh_clusters{storage_entry(0, true, WGPUShaderStage_Fragment),
                                       storage_entry(1, true, WGPUShaderStage_Fragment),
                                       storage_entry(2, true, WGPUShaderStage_Fragment)};
        mesh_clusters_layout_ = layout(mesh_clusters);
        const std::array shadow_vertex{
            uniform_entry(0, WGPUShaderStage_Vertex, sizeof(MeshShadowPassUniforms)),
            uniform_entry(1, WGPUShaderStage_Vertex, sizeof(MeshInstanceUniforms))};
        shadow_vertex_layout_ = layout(shadow_vertex);
        const std::array shadow_fragment{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(MeshSurfaceUniforms))};
        shadow_fragment_layout_ = layout(shadow_fragment);
        std::array<WGPUBindGroupLayoutEntry, kPostTextures * 2> post_textures{};
        for (std::uint32_t slot = 0; slot < kPostTextures; ++slot) {
            post_textures.at(slot) = texture_entry(slot);
            post_textures.at(kPostTextures + slot) = sampler_entry(kSamplerBinding + slot);
        }
        post_textures_layout_ = layout(post_textures);
        const std::array post_uniforms{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(PostUniforms))};
        post_uniforms_layout_ = layout(post_uniforms);
        // Forward+'s post passes bind thirteen textures, so their samplers come 16 after them.
        // The brightness, luminance and depth textures are floats, which they load unfiltered.
        std::array<WGPUBindGroupLayoutEntry, kForwardPostTextures * 2> forward_textures{};
        for (std::uint32_t slot = 0; slot < kForwardPostTextures; ++slot) {
            forward_textures.at(slot) = texture_entry(slot);
            if (slot == kBrightnessSlot || slot == kLuminanceSourceSlot || slot == kDepthSlot) {
                forward_textures.at(slot).texture.sampleType =
                    WGPUTextureSampleType_UnfilterableFloat;
            }
            forward_textures.at(kForwardPostTextures + slot) =
                sampler_entry(kMeshSamplerBinding + slot);
        }
        forward_textures_layout_ = layout(forward_textures);
        const std::array forward_uniforms{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(ForwardPostUniforms))};
        forward_uniforms_layout_ = layout(forward_uniforms);
        const std::array sky_entries{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(SkyUniforms))};
        sky_uniforms_layout_ = layout(sky_entries);
        const std::array cube{cube_entry(0), sampler_entry(kSamplerBinding)};
        cube_texture_layout_ = layout(cube);
        // The SSAO passes load every texture: the depth buffer, its levels and the load counter
        // are floats, which WebGPU doesn't filter.
        std::array<WGPUBindGroupLayoutEntry, kSsaoSlots> ssao_textures{};
        for (std::uint32_t slot = 0; slot < kSsaoSlots; ++slot) {
            ssao_textures.at(slot) = texture_entry(slot);
            ssao_textures.at(slot).texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        }
        ssao_textures_layout_ = layout(ssao_textures);
        const std::array ssao_entries{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(SsaoUniforms))};
        ssao_uniforms_layout_ = layout(ssao_entries);
        // So do the SSR passes: Hi-Z's levels and the filter's rows are floats too.
        std::array<WGPUBindGroupLayoutEntry, kSsrSlots> ssr_textures{};
        for (std::uint32_t slot = 0; slot < kSsrSlots; ++slot) {
            ssr_textures.at(slot) = texture_entry(slot);
            ssr_textures.at(slot).texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        }
        ssr_textures_layout_ = layout(ssr_textures);
        const std::array ssr_entries{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(SsrUniforms))};
        ssr_uniforms_layout_ = layout(ssr_entries);
        // TAA's pass loads its five textures, as SSR's do.
        std::array<WGPUBindGroupLayoutEntry, kTaaSlots> taa_textures{};
        for (std::uint32_t slot = 0; slot < kTaaSlots; ++slot) {
            taa_textures.at(slot) = texture_entry(slot);
            taa_textures.at(slot).texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        }
        taa_textures_layout_ = layout(taa_textures);
        const std::array taa_entries{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(TaaUniforms))};
        taa_uniforms_layout_ = layout(taa_entries);
        const std::array filter{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(RadianceUniforms))};
        filter_uniforms_layout_ = layout(filter);
        // FSR 2's passes load their textures too; the depth clip pass reads the reconstructed
        // depths, which the reconstruct pass writes from the dilated depths and motion vectors.
        for (std::size_t pass = 0; pass < kFsr2Passes; ++pass) {
            std::array<WGPUBindGroupLayoutEntry, 8> textures{};
            const std::uint32_t count = fsr2_pass_code(static_cast<Fsr2Pass>(pass)).textures;
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                textures.at(slot) = texture_entry(slot);
                textures.at(slot).texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
            }
            fsr2_textures_layouts_.at(pass) =
                layout(std::span<const WGPUBindGroupLayoutEntry>(textures.data(), count));
        }
        const std::array fsr2_entries{
            uniform_entry(0, WGPUShaderStage_Fragment, sizeof(Fsr2Uniforms))};
        fsr2_uniforms_layout_ = layout(fsr2_entries);
        const std::array fsr2_storage{storage_entry(0, true, WGPUShaderStage_Fragment)};
        fsr2_storage_layout_ = layout(fsr2_storage);
        std::array<WGPUBindGroupLayoutEntry, 2> fsr2_compute_textures{};
        for (std::uint32_t slot = 0; slot < fsr2_compute_textures.size(); ++slot) {
            fsr2_compute_textures.at(slot) = texture_entry(slot);
            fsr2_compute_textures.at(slot).visibility = WGPUShaderStage_Compute;
            fsr2_compute_textures.at(slot).texture.sampleType =
                WGPUTextureSampleType_UnfilterableFloat;
        }
        fsr2_compute_textures_layout_ = layout(fsr2_compute_textures);
        const std::array fsr2_compute_storage{storage_entry(0, false)};
        fsr2_compute_storage_layout_ = layout(fsr2_compute_storage);
        const std::array fsr2_compute_entries{
            uniform_entry(0, WGPUShaderStage_Compute, sizeof(Fsr2Uniforms))};
        fsr2_compute_uniforms_layout_ = layout(fsr2_compute_entries);

        const auto pipeline_layout = [&](std::initializer_list<WGPUBindGroupLayout> groups) {
            const std::vector<WGPUBindGroupLayout> list(groups);
            WGPUPipelineLayoutDescriptor info = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            info.bindGroupLayoutCount = list.size();
            info.bindGroupLayouts = list.data();
            return wgpuDeviceCreatePipelineLayout(device_, &info);
        };
        pipeline_layouts_.at(kSpriteLayout) =
            pipeline_layout({empty_layout_, vertex_uniforms_layout_, sprite_textures_layout_});
        pipeline_layouts_.at(kMaterialLayout) =
            pipeline_layout({empty_layout_, vertex_uniforms_layout_, material_textures_layout_,
                             fragment_uniforms_layout_});
        pipeline_layouts_.at(kProcessLayout) = pipeline_layout(
            {process_inputs_layout_, process_outputs_layout_, compute_uniforms_layout_});
        pipeline_layouts_.at(kDrawLayout) =
            pipeline_layout({draw_inputs_layout_, draw_outputs_layout_, compute_uniforms_layout_});
        pipeline_layouts_.at(kEmitLayout) =
            pipeline_layout({empty_layout_, emit_outputs_layout_, compute_uniforms_layout_});
        pipeline_layouts_.at(kMeshLayout) = pipeline_layout(
            {empty_layout_, mesh_vertex_layout_, mesh_textures_layout_, mesh_fragment_layout_});
        pipeline_layouts_.at(kMeshClusteredLayout) =
            pipeline_layout({mesh_clusters_layout_, mesh_vertex_layout_, mesh_textures_layout_,
                             mesh_fragment_layout_});
        pipeline_layouts_.at(kShadowLayout) =
            pipeline_layout({empty_layout_, shadow_vertex_layout_, sprite_textures_layout_,
                             shadow_fragment_layout_});
        pipeline_layouts_.at(kPostLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, post_textures_layout_, post_uniforms_layout_});
        pipeline_layouts_.at(kForwardPostLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, forward_textures_layout_, forward_uniforms_layout_});
        pipeline_layouts_.at(kSkyLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, sprite_textures_layout_, sky_uniforms_layout_});
        pipeline_layouts_.at(kRadianceLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, cube_texture_layout_, filter_uniforms_layout_});
        pipeline_layouts_.at(kSsaoLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, ssao_textures_layout_, ssao_uniforms_layout_});
        pipeline_layouts_.at(kSsrLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, ssr_textures_layout_, ssr_uniforms_layout_});
        pipeline_layouts_.at(kTaaLayout) = pipeline_layout(
            {empty_layout_, empty_layout_, taa_textures_layout_, taa_uniforms_layout_});
        for (std::size_t pass = 0; pass < kFsr2Passes; ++pass) {
            pipeline_layouts_.at(kFsr2Layout + pass) = pipeline_layout(
                {static_cast<Fsr2Pass>(pass) == Fsr2Pass::DepthClip ? fsr2_storage_layout_
                                                                    : empty_layout_,
                 empty_layout_, fsr2_textures_layouts_.at(pass), fsr2_uniforms_layout_});
        }
        pipeline_layouts_.at(kFsr2ReconstructLayout) =
            pipeline_layout({fsr2_compute_textures_layout_, fsr2_compute_storage_layout_,
                             fsr2_compute_uniforms_layout_});
    }

    WGPUShaderModule create_module(std::string_view wgsl) {
        WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = text_view(wgsl);
        WGPUShaderModuleDescriptor info = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        info.nextInChain = &source.chain;
        return wgpuDeviceCreateShaderModule(device_, &info);
    }

    static void release_game_shader(const GameShader& shader) {
        wgpuShaderModuleRelease(shader.vertex);
        wgpuShaderModuleRelease(shader.fragment);
    }

    // A shader module, made the first time it is needed.
    WGPUShaderModule module(std::size_t index, std::string_view wgsl) {
        WGPUShaderModule& slot = modules_.at(index);
        if (slot == nullptr) {
            slot = create_module(wgsl);
        }
        return slot;
    }

    // A compute shader's pipeline, made the first time it is needed.
    Result<WGPUComputePipeline> compute_pipeline(ComputeKind kind) {
        WGPUComputePipeline& slot = compute_pipelines_.at(static_cast<std::size_t>(kind));
        if (slot != nullptr) {
            return slot;
        }
        WGPUComputePipelineDescriptor info = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        switch (kind) {
        case ComputeKind::Process:
            info.layout = pipeline_layouts_.at(kProcessLayout);
            info.compute.module =
                module(kProcessModule, joined(shaders::kParticlesProcessComputeWgsl));
            break;
        case ComputeKind::Draw:
            info.layout = pipeline_layouts_.at(kDrawLayout);
            info.compute.module = module(kDrawModule, joined(shaders::kParticlesDrawComputeWgsl));
            break;
        case ComputeKind::Emit:
            info.layout = pipeline_layouts_.at(kEmitLayout);
            info.compute.module = module(kEmitModule, joined(shaders::kParticlesEmitComputeWgsl));
            break;
        }
        info.compute.entryPoint = text_view("cs_main");
        slot = wgpuDeviceCreateComputePipeline(device_, &info);
        return slot;
    }

    // Where a process material's words are, packed after the others unless it is the one packed
    // last.
    std::uint32_t material_words(const ParticleProcess& process) {
        if (last_material_ && last_material_->first == process) {
            return last_material_->second;
        }
        const auto at = static_cast<std::uint32_t>(particle_words_.size());
        pack_particle_process(process, particle_words_);
        last_material_.emplace(process, at);
        return at;
    }

    // The buffers a process pass binds where it takes or writes no emissions, made once.
    void make_unbound_emissions() {
        for (WGPUBuffer* unbound : {&no_source_, &no_target_}) {
            if (*unbound == nullptr) {
                *unbound = create_buffer(WGPUBufferUsage_Storage, sizeof(Word));
            }
        }
    }

    // Queues a job, which runs at once outside a frame and with the frame inside one.
    Result<void> queue_particle_job(const ParticleJob& job) {
        particle_jobs_.push_back(job);
        return in_frame_ ? Result<void>{} : flush_particle_jobs();
    }

    // Sends the queued jobs to the GPU on their own, with nothing drawn.
    Result<void> flush_particle_jobs() {
        if (particle_jobs_.empty()) {
            return {};
        }
        uniforms_.clear();
        const std::vector<std::uint32_t> job_uniforms = particle_job_uniforms(0);
        upload_particle_words();
        upload_uniforms();
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        encode_particle_jobs(encoder, job_uniforms);
        submit(encoder);
        clear_particle_jobs();
        release_pending_buffers();
        return {};
    }

    // Adds each queued job's uniforms, returning where each is. Draws write their quads into the
    // vertex buffer from vertex base.
    std::vector<std::uint32_t> particle_job_uniforms(std::uint32_t base) {
        std::vector<std::uint32_t> at;
        at.reserve(particle_jobs_.size());
        for (const ParticleJob& job : particle_jobs_) {
            switch (job.kind) {
            case ComputeKind::Draw: {
                DrawUniforms uniforms = job.look;
                uniforms.counts[3] += base;
                at.push_back(uniforms_.add(uniforms));
                break;
            }
            case ComputeKind::Process:
                at.push_back(
                    uniforms_.add(ProcessUniforms{.amount = job.amount,
                                                  .material = job.material,
                                                  .steps = job.steps,
                                                  .step_count = job.step_count,
                                                  .reset = job.reset ? 1U : 0U,
                                                  .chain = job.chain,
                                                  .trail = job.trail,
                                                  .history = job.history,
                                                  .phase_step = particle_phase_step(job.amount)}));
                break;
            case ComputeKind::Emit:
                at.push_back(uniforms_.add(job.emit));
                break;
            }
        }
        return at;
    }

    // Copies what the queued jobs read to the GPU.
    void upload_particle_words() {
        if (particle_jobs_.empty()) {
            return;
        }
        if (particle_words_.empty()) {
            particle_words_.push_back({}); // the process shader always reads a buffer
        }
        const std::uint64_t size = particle_words_.size() * sizeof(Word);
        if (size > particle_capacity_) {
            destroy_buffer(particle_data_);
            particle_capacity_ = std::max({size, particle_capacity_ * 2, kMinVertexBytes});
            particle_data_ = create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                                           particle_capacity_);
        }
        wgpuQueueWriteBuffer(queue_, particle_data_, 0, particle_words_.data(),
                             particle_words_.size() * sizeof(Word));
    }

    // Copies the uniforms added so far to the GPU, growing the buffer, and the bind groups that
    // read it, when they don't fit.
    void upload_uniforms() {
        if (uniforms_.empty()) {
            return;
        }
        // The largest block a binding reads past its offset, so the last one fits too.
        const std::uint64_t size = uniforms_.bytes().size() + kLargestUniforms;
        if (size > uniform_capacity_) {
            release_uniform_groups();
            destroy_buffer(uniform_buffer_);
            uniform_capacity_ = std::max({size, uniform_capacity_ * 2, kMinVertexBytes});
            uniform_buffer_ =
                create_buffer(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, uniform_capacity_);
            make_uniform_groups();
        }
        wgpuQueueWriteBuffer(queue_, uniform_buffer_, 0, uniforms_.bytes().data(),
                             uniforms_.bytes().size());
    }

    WGPUBindGroup bind_group(WGPUBindGroupLayout layout,
                             std::span<const WGPUBindGroupEntry> entries) {
        WGPUBindGroupDescriptor info = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        info.layout = layout;
        info.entryCount = entries.size();
        info.entries = entries.data();
        return wgpuDeviceCreateBindGroup(device_, &info);
    }

    static WGPUBindGroupEntry buffer_entry(std::uint32_t binding, WGPUBuffer buffer,
                                           std::uint64_t size = WGPU_WHOLE_SIZE) {
        WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = binding;
        entry.buffer = buffer;
        entry.size = size;
        return entry;
    }

    // The groups that bind the uniform buffer at an offset each draw or pass picks.
    void make_uniform_groups() {
        const std::array vertex{buffer_entry(0, uniform_buffer_, sizeof(Mat4)),
                                buffer_entry(1, uniform_buffer_, sizeof(VertexProgramUniforms))};
        vertex_uniforms_group_ = bind_group(vertex_uniforms_layout_, vertex);
        const std::array fragment{buffer_entry(0, uniform_buffer_, sizeof(ProgramUniforms)),
                                  buffer_entry(1, uniform_buffer_, sizeof(LightsUniforms))};
        fragment_uniforms_group_ = bind_group(fragment_uniforms_layout_, fragment);
        const std::array compute{buffer_entry(0, uniform_buffer_, kComputeUniformBytes)};
        compute_uniforms_group_ = bind_group(compute_uniforms_layout_, compute);
        const std::array mesh_vertex{
            buffer_entry(0, uniform_buffer_, sizeof(MeshCameraUniforms)),
            buffer_entry(1, uniform_buffer_, sizeof(MeshInstanceUniforms))};
        mesh_vertex_group_ = bind_group(mesh_vertex_layout_, mesh_vertex);
        const std::array mesh_fragment{
            buffer_entry(0, uniform_buffer_, sizeof(MeshSurfaceUniforms)),
            buffer_entry(1, uniform_buffer_, sizeof(MeshSceneUniforms)),
            buffer_entry(2, uniform_buffer_, sizeof(MeshShadowUniforms))};
        mesh_fragment_group_ = bind_group(mesh_fragment_layout_, mesh_fragment);
        const std::array shadow_vertex{
            buffer_entry(0, uniform_buffer_, sizeof(MeshShadowPassUniforms)),
            buffer_entry(1, uniform_buffer_, sizeof(MeshInstanceUniforms))};
        shadow_vertex_group_ = bind_group(shadow_vertex_layout_, shadow_vertex);
        const std::array shadow_fragment{
            buffer_entry(0, uniform_buffer_, sizeof(MeshSurfaceUniforms))};
        shadow_fragment_group_ = bind_group(shadow_fragment_layout_, shadow_fragment);
        const std::array post{buffer_entry(0, uniform_buffer_, sizeof(PostUniforms))};
        post_uniforms_group_ = bind_group(post_uniforms_layout_, post);
        const std::array forward{buffer_entry(0, uniform_buffer_, sizeof(ForwardPostUniforms))};
        forward_uniforms_group_ = bind_group(forward_uniforms_layout_, forward);
        const std::array sky{buffer_entry(0, uniform_buffer_, sizeof(SkyUniforms))};
        sky_uniforms_group_ = bind_group(sky_uniforms_layout_, sky);
        const std::array filter{buffer_entry(0, uniform_buffer_, sizeof(RadianceUniforms))};
        filter_uniforms_group_ = bind_group(filter_uniforms_layout_, filter);
        const std::array ssao{buffer_entry(0, uniform_buffer_, sizeof(SsaoUniforms))};
        ssao_uniforms_group_ = bind_group(ssao_uniforms_layout_, ssao);
        const std::array ssr{buffer_entry(0, uniform_buffer_, sizeof(SsrUniforms))};
        ssr_uniforms_group_ = bind_group(ssr_uniforms_layout_, ssr);
        const std::array taa{buffer_entry(0, uniform_buffer_, sizeof(TaaUniforms))};
        taa_uniforms_group_ = bind_group(taa_uniforms_layout_, taa);
        const std::array fsr2{buffer_entry(0, uniform_buffer_, sizeof(Fsr2Uniforms))};
        fsr2_uniforms_group_ = bind_group(fsr2_uniforms_layout_, fsr2);
        fsr2_compute_uniforms_group_ = bind_group(fsr2_compute_uniforms_layout_, fsr2);
    }

    void release_uniform_groups() {
        for (WGPUBindGroup* group :
             {&vertex_uniforms_group_, &fragment_uniforms_group_, &compute_uniforms_group_,
              &mesh_vertex_group_, &mesh_fragment_group_, &shadow_vertex_group_,
              &shadow_fragment_group_, &post_uniforms_group_, &forward_uniforms_group_,
              &sky_uniforms_group_, &filter_uniforms_group_, &ssao_uniforms_group_,
              &ssr_uniforms_group_, &taa_uniforms_group_, &fsr2_uniforms_group_,
              &fsr2_compute_uniforms_group_}) {
            if (*group != nullptr) {
                wgpuBindGroupRelease(*group);
                *group = nullptr;
            }
        }
    }

    // Records the queued jobs as compute passes, in order, each with its uniforms at uniforms.
    void encode_particle_jobs(WGPUCommandEncoder encoder, std::span<const std::uint32_t> uniforms) {
        for (std::size_t i = 0; i < particle_jobs_.size(); ++i) {
            const ParticleJob& job = particle_jobs_[i];
            std::uint32_t groups = (job.amount + kParticleThreads - 1) / kParticleThreads;
            WGPUBindGroup inputs = nullptr;
            WGPUBindGroup outputs = nullptr;
            switch (job.kind) {
            case ComputeKind::Draw: {
                const std::array in{buffer_entry(0, job.particles)};
                inputs = bind_group(draw_inputs_layout_, in);
                const std::array out{buffer_entry(0, vertex_buffer_)};
                outputs = bind_group(draw_outputs_layout_, out);
                break;
            }
            case ComputeKind::Process: {
                // Passes that take or write nothing for another system still bind a buffer
                // there.
                const std::array in{
                    buffer_entry(0, particle_data_),
                    buffer_entry(1, job.source != nullptr ? job.source : no_source_)};
                inputs = bind_group(process_inputs_layout_, in);
                const std::array out{
                    buffer_entry(0, job.particles),
                    buffer_entry(1, job.target != nullptr ? job.target : no_target_)};
                outputs = bind_group(process_outputs_layout_, out);
                break;
            }
            case ComputeKind::Emit: {
                const std::array out{buffer_entry(0, job.target), buffer_entry(1, job.particles)};
                outputs = bind_group(emit_outputs_layout_, out);
                groups = 1;
                break;
            }
            }
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
            wgpuComputePassEncoderSetPipeline(
                pass, compute_pipelines_.at(static_cast<std::size_t>(job.kind)));
            if (inputs != nullptr) {
                wgpuComputePassEncoderSetBindGroup(pass, 0, inputs, 0, nullptr);
            }
            wgpuComputePassEncoderSetBindGroup(pass, 1, outputs, 0, nullptr);
            const std::uint32_t offset = uniforms[i];
            wgpuComputePassEncoderSetBindGroup(pass, 2, compute_uniforms_group_, 1, &offset);
            wgpuComputePassEncoderDispatchWorkgroups(pass, groups, 1, 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            if (inputs != nullptr) {
                wgpuBindGroupRelease(inputs);
            }
            wgpuBindGroupRelease(outputs);
        }
    }

    void clear_particle_jobs() {
        particle_jobs_.clear();
        particle_words_.clear();
        last_material_.reset();
    }

    void release_pending_buffers() {
        for (WGPUBuffer buffer : pending_buffers_) {
            destroy_buffer(buffer);
        }
        pending_buffers_.clear();
    }

    // The 2D pipeline for a target format, shader and blend mode, made the first time a frame
    // needs it; with a game's shader, its stages take the material shader's place.
    Result<WGPURenderPipeline> pipeline_for(WGPUTextureFormat format, ShaderKind kind,
                                            BlendMode blend, ShaderId shader = ShaderId::None) {
        for (const Pipeline& made : pipelines_) {
            if (made.format == format && made.kind == kind && made.blend == blend &&
                made.shader == shader) {
                return made.pipeline;
            }
        }
        const bool material = kind == ShaderKind::Material;
        WGPUShaderModule vertex_module =
            material ? module(kMaterialVertexModule, shaders::kMaterialVertexWgsl)
                     : module(kSpriteVertexModule, shaders::kSpriteVertexWgsl);
        WGPUShaderModule fragment_module =
            material ? module(kMaterialFragmentModule, shaders::kMaterialFragmentWgsl)
                     : module(kSpriteFragmentModule, shaders::kSpriteFragmentWgsl);
        if (shader != ShaderId::None) {
            const GameShader& game = game_shaders_.at(shader);
            vertex_module = game.vertex;
            fragment_module = game.fragment;
        }
        std::array<WGPUVertexAttribute, 3> attributes{};
        attributes[0] = {.nextInChain = nullptr,
                         .format = WGPUVertexFormat_Float32x2,
                         .offset = offsetof(Vertex2D, x),
                         .shaderLocation = 0};
        attributes[1] = {.nextInChain = nullptr,
                         .format = WGPUVertexFormat_Float32x2,
                         .offset = offsetof(Vertex2D, u),
                         .shaderLocation = 1};
        attributes[2] = {.nextInChain = nullptr,
                         .format = WGPUVertexFormat_Unorm8x4,
                         .offset = offsetof(Vertex2D, rgba),
                         .shaderLocation = 2};
        WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        buffer.stepMode = WGPUVertexStepMode_Vertex;
        buffer.arrayStride = sizeof(Vertex2D);
        buffer.attributeCount = attributes.size();
        buffer.attributes = attributes.data();

        const WGPUBlendState blending = blend_state(blend);
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        color.blend = &blending;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = fragment_module;
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;

        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(material ? kMaterialLayout : kSpriteLayout);
        info.vertex.module = vertex_module;
        info.vertex.entryPoint = text_view("vs_main");
        info.vertex.bufferCount = 1;
        info.vertex.buffers = &buffer;
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        pipelines_.push_back({.format = format,
                              .kind = kind,
                              .blend = blend,
                              .shader = shader,
                              .pipeline = pipeline});
        return pipeline;
    }

    // Vertex3D's fields as the mesh and shadow shaders' inputs.
    static std::array<WGPUVertexAttribute, 6> mesh_attributes() {
        const auto attribute = [](WGPUVertexFormat format, std::size_t offset,
                                  std::uint32_t location) {
            return WGPUVertexAttribute{.nextInChain = nullptr,
                                       .format = format,
                                       .offset = offset,
                                       .shaderLocation = location};
        };
        return {attribute(WGPUVertexFormat_Float32x3, offsetof(Vertex3D, position), 0),
                attribute(WGPUVertexFormat_Float32x3, offsetof(Vertex3D, normal), 1),
                attribute(WGPUVertexFormat_Float32x4, offsetof(Vertex3D, tangent), 2),
                attribute(WGPUVertexFormat_Float32x2, offsetof(Vertex3D, uv), 3),
                attribute(WGPUVertexFormat_Unorm8x4, offsetof(Vertex3D, rgba), 4),
                attribute(WGPUVertexFormat_Float32x2, offsetof(Vertex3D, uv2), 5)};
    }

    static WGPUCullMode gpu_cull(CullMode cull) {
        switch (cull) {
        case CullMode::Back:
            return WGPUCullMode_Back;
        case CullMode::Front:
            return WGPUCullMode_Front;
        case CullMode::Disabled:
            break;
        }
        return WGPUCullMode_None;
    }

    // The frame's shadow plan and the atlases it draws into, made at the frame's first mesh draw.
    void prepare_shadows(const Scene3D& scene) {
        if (shadow_plan_) {
            return;
        }
        if (shadow_sampler_ == nullptr) {
            // Each tap compares four texels and blends the results, as Godot's shadow sampler.
            WGPUSamplerDescriptor info = WGPU_SAMPLER_DESCRIPTOR_INIT;
            info.minFilter = WGPUFilterMode_Linear;
            info.magFilter = WGPUFilterMode_Linear;
            info.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            info.addressModeU = WGPUAddressMode_ClampToEdge;
            info.addressModeV = WGPUAddressMode_ClampToEdge;
            info.addressModeW = WGPUAddressMode_ClampToEdge;
            info.compare = WGPUCompareFunction_GreaterEqual;
            shadow_sampler_ = wgpuDeviceCreateSampler(device_, &info);
            // WebGPU clears new textures, so this one is never read uninitialized.
            no_shadows_ = make_shadow_atlas(1);
        }
        ShadowPlan plan = plan_shadows(scene, kMaxShadowAtlas);
        const std::array<std::uint32_t, 2> sizes{plan.directional_size, plan.positional_size};
        for (std::size_t i = 0; i < 2; ++i) {
            ShadowAtlasTexture& atlas = shadow_atlases_.at(i);
            if (sizes.at(i) == 0 || atlas.size == sizes.at(i)) {
                continue;
            }
            if (atlas.gpu.texture != nullptr) {
                forget_texture_groups(atlas.gpu.view);
                pending_textures_.push_back(std::exchange(atlas.gpu, {}));
            }
            atlas = {.gpu = make_shadow_atlas(sizes.at(i)), .size = sizes.at(i)};
        }
        shadow_plan_ = std::move(plan);
    }

    GpuTexture make_shadow_atlas(std::uint32_t size) {
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        info.size = {.width = size, .height = size, .depthOrArrayLayers = 1};
        info.format = kDepthFormat;
        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &info);
        return {.texture = texture,
                .view = wgpuTextureCreateView(texture, nullptr),
                .filter = Filter::Linear};
    }

    // A cube texture of format, size texels across with levels levels: the radiance's, or with
    // cubes, a reflection atlas's array of that many cubes.
    CubeTexture make_cube(std::uint32_t size, std::uint32_t levels,
                          WGPUTextureFormat format = kRadianceFormat, std::uint32_t cubes = 0) {
        const std::uint32_t layers = kCubeFaces * std::max(cubes, 1U);
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        info.size = {.width = size, .height = size, .depthOrArrayLayers = layers};
        info.format = format;
        info.mipLevelCount = levels;
        CubeTexture cube;
        cube.texture = wgpuDeviceCreateTexture(device_, &info);
        const auto view = [&](WGPUTextureViewDimension dimension, std::uint32_t level,
                              std::uint32_t level_count, std::uint32_t layer,
                              std::uint32_t layer_count) {
            WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            descriptor.dimension = dimension;
            descriptor.baseMipLevel = level;
            descriptor.mipLevelCount = level_count;
            descriptor.baseArrayLayer = layer;
            descriptor.arrayLayerCount = layer_count;
            return wgpuTextureCreateView(cube.texture, &descriptor);
        };
        cube.view =
            view(cubes > 0 ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube, 0,
                 levels, 0, layers);
        for (std::uint32_t level = 0; level < levels; ++level) {
            if (cubes == 0) {
                cube.levels.push_back(view(WGPUTextureViewDimension_Cube, level, 1, 0, kCubeFaces));
            }
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                cube.faces.push_back(view(WGPUTextureViewDimension_2D, level, 1, layer, 1));
            }
        }
        return cube;
    }

    void destroy_cube(const CubeTexture& cube) {
        if (cube.texture == nullptr) {
            return;
        }
        forget_texture_groups(cube.view);
        wgpuTextureViewRelease(cube.view);
        for (WGPUTextureView view : cube.levels) {
            wgpuTextureViewRelease(view);
        }
        for (WGPUTextureView view : cube.faces) {
            wgpuTextureViewRelease(view);
        }
        wgpuTextureDestroy(cube.texture);
        wgpuTextureRelease(cube.texture);
    }

    // The pipelines that draw into the radiance's cubes: one triangle over the target, the sky's
    // (0) or the filter's (1), made the first time a frame needs them.
    WGPURenderPipeline radiance_pipeline(std::size_t index) {
        WGPURenderPipeline& made = radiance_pipelines_.at(index);
        if (made == nullptr) {
            made = cube_pipeline(index == 0, kRadianceFormat);
        }
        return made;
    }

    // The filter's pipeline into a reflection probe's half floats, made the first time a capture
    // needs it.
    WGPURenderPipeline probe_pipeline() {
        if (probe_pipeline_ == nullptr) {
            probe_pipeline_ = cube_pipeline(false, kHdrFormat);
        }
        return probe_pipeline_;
    }

    WGPURenderPipeline cube_pipeline(bool sky, WGPUTextureFormat format) {
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = sky ? module(kSkyFragmentModule, shaders::kSkyFragmentWgsl)
                              : module(kRadianceFragmentModule, shaders::kRadianceFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(sky ? kSkyLayout : kRadianceLayout);
        info.vertex.module = sky ? module(kSkyVertexModule, shaders::kSkyVertexWgsl)
                                 : module(kRadianceVertexModule, shaders::kRadianceVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        return wgpuDeviceCreateRenderPipeline(device_, &info);
    }

    // What the frame's meshes take the radiance from, made at its first mesh draw: the sampler
    // and a black cube for meshes with none, and for a scene with radiance, its cubes, drawn
    // again at end_frame when what they are drawn from changed.
    void prepare_radiance(const Scene3D& scene) {
        if (radiance_prepared_) {
            return;
        }
        radiance_prepared_ = true;
        prepare_cube_sampler();
        if (!radiance_used(scene)) {
            return;
        }
        const std::uint32_t size = scene.radiance.size;
        if (radiance_.size != size) {
            for (CubeTexture* cube : {&radiance_.raw, &radiance_.filtered}) {
                if (cube->texture != nullptr) {
                    forget_texture_groups(cube->view);
                    pending_cubes_.push_back(std::exchange(*cube, {}));
                }
            }
            radiance_.raw = make_cube(size, radiance_levels(size));
            radiance_.filtered = make_cube(size, radiance_levels(size));
            radiance_.size = size;
            radiance_.key.reset();
        }
        const RadianceKey key = radiance_key(scene);
        radiance_draws_ = !radiance_.key || !(*radiance_.key == key);
        radiance_.key = key;
    }

    // The sampler cubes are read through, and what meshes bind with no radiance, probes or SSAO
    // and SSR: made once.
    void prepare_cube_sampler() {
        if (radiance_sampler_ != nullptr) {
            return;
        }
        WGPUSamplerDescriptor info = WGPU_SAMPLER_DESCRIPTOR_INIT;
        info.minFilter = WGPUFilterMode_Linear;
        info.magFilter = WGPUFilterMode_Linear;
        info.mipmapFilter = WGPUMipmapFilterMode_Linear;
        info.addressModeU = WGPUAddressMode_ClampToEdge;
        info.addressModeV = WGPUAddressMode_ClampToEdge;
        info.addressModeW = WGPUAddressMode_ClampToEdge;
        radiance_sampler_ = wgpuDeviceCreateSampler(device_, &info);
        // WebGPU clears new textures, so these are never read uninitialized.
        no_radiance_ = make_cube(1, 1);
        no_probes_ = make_cube(1, 1, kHdrFormat, 1);
        no_screen_ = make_screen(1, 1);
    }

    // The 2-layer array the mesh pass reads SSAO (layer 0) and SSR (layer 1) from, which their
    // last passes draw into, at width x height.
    ScreenArray make_screen(std::uint32_t width, std::uint32_t height) {
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        info.size = {.width = width, .height = height, .depthOrArrayLayers = 2};
        info.format = kHdrFormat;
        ScreenArray screen{.texture = wgpuDeviceCreateTexture(device_, &info),
                           .view = nullptr,
                           .layers = {},
                           .width = width,
                           .height = height};
        WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        descriptor.dimension = WGPUTextureViewDimension_2DArray;
        descriptor.arrayLayerCount = 2;
        screen.view = wgpuTextureCreateView(screen.texture, &descriptor);
        for (std::uint32_t layer = 0; layer < 2; ++layer) {
            descriptor.dimension = WGPUTextureViewDimension_2D;
            descriptor.baseArrayLayer = layer;
            descriptor.arrayLayerCount = 1;
            screen.layers.at(layer) = wgpuTextureCreateView(screen.texture, &descriptor);
        }
        return screen;
    }

    void destroy_screen(ScreenArray& screen) {
        if (screen.texture == nullptr) {
            return;
        }
        forget_texture_groups(screen.view);
        wgpuTextureViewRelease(screen.view);
        for (WGPUTextureView layer : screen.layers) {
            wgpuTextureViewRelease(layer);
        }
        wgpuTextureDestroy(screen.texture);
        wgpuTextureRelease(screen.texture);
        screen = {};
    }

    // The frame's screen array, at its size, made at begin_frame (before anything is recorded)
    // in a frame with SSAO or SSR.
    void prepare_screen(std::uint32_t width, std::uint32_t height) {
        if (screen_.texture != nullptr && screen_.width == width && screen_.height == height) {
            return;
        }
        destroy_screen(screen_);
        screen_ = make_screen(width, height);
    }

    // A capture's frame: the world alone, linear, drawn into a face of the raw cube, whose
    // mipmaps and filtered levels go into the atlas's cube after the sixth face.
    Result<bool> begin_capture(const Frame& frame) {
        if (frame.target != TextureId::None) {
            return make_error(ErrorCode::InvalidArgument,
                              "a capture frame draws into its atlas; its target is None");
        }
        Frame captured = frame;
        Scene3D& scene = *captured.scene;
        scene.method = RenderMethod::ForwardPlus;
        scene.ssao.enabled = false;
        scene.ssr.enabled = false;
        scene.reflections.probes.clear();
        scene.post = {};
        captured.lighting = {};
        prepare_cube_sampler();
        const std::uint32_t size = textures_.at(scene.capture->atlas).desc.width;
        if (probe_raw_.texture == nullptr || probe_raw_size_ != size) {
            if (probe_raw_.texture != nullptr) {
                forget_texture_groups(probe_raw_.view);
                pending_cubes_.push_back(std::exchange(probe_raw_, {}));
            }
            probe_raw_ = make_cube(size, probe_levels(size), kHdrFormat);
            probe_raw_size_ = size;
        }
        start_frame(captured, nullptr, size, size, true);
        frame_format_ = kTextureFormat;
        return true;
    }

    // Adds a capture's filter uniforms: for each level past the first and face, its mipmap's,
    // then for each level and face, its filter's, linear.
    ProbeOffsets add_probe_uniforms() {
        ProbeOffsets at;
        const std::uint32_t size = probe_raw_size_;
        const std::uint32_t levels = probe_levels(size);
        for (std::uint32_t level = 1; level < levels; ++level) {
            auto mipmap = std::make_unique<RadianceUniforms>();
            mipmap->params[1] = 1.0F / static_cast<float>(std::max(size >> level, 1U));
            mipmap->options = {1.0F, 0.0F, 0.0F, 0.0F};
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                mipmap->params[0] = static_cast<float>(face);
                at.mipmaps.push_back(uniforms_.add(*mipmap));
            }
        }
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto filter =
                std::make_unique<RadianceUniforms>(radiance_uniforms(size, levels, level));
            filter->options = {1.0F, 0.0F, 0.0F, 0.0F};
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                filter->params[0] = static_cast<float>(face);
                at.filters.push_back(uniforms_.add(*filter));
            }
        }
        return at;
    }

    // Records the raw cube's mipmaps, each texel the mean of four, then each level of each face
    // filtered from it into the capture's cube of its atlas.
    void encode_probe(WGPUCommandEncoder encoder, const ProbeCapture& capture,
                      const ProbeOffsets& at) {
        const std::uint32_t levels = probe_levels(probe_raw_size_);
        for (std::uint32_t level = 1; level < levels; ++level) {
            WGPUBindGroup source = pass_textures(
                cube_texture_layout_, probe_raw_.levels.at(level - 1), radiance_sampler_);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                encode_cube_pass(encoder,
                                 probe_raw_.faces.at((std::size_t{level} * kCubeFaces) + face),
                                 probe_pipeline(), source, filter_uniforms_group_,
                                 at.mipmaps.at((std::size_t{level - 1} * kCubeFaces) + face));
            }
            wgpuBindGroupRelease(source);
        }
        const CubeTexture& atlas = atlases_.at(capture.atlas);
        const std::size_t layers = atlas.faces.size() / levels;
        WGPUBindGroup source =
            pass_textures(cube_texture_layout_, probe_raw_.view, radiance_sampler_);
        for (std::uint32_t level = 0; level < levels; ++level) {
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                const std::size_t layer = (std::size_t{capture.cube} * kCubeFaces) + face;
                encode_cube_pass(encoder, atlas.faces.at((level * layers) + layer),
                                 probe_pipeline(), source, filter_uniforms_group_,
                                 at.filters.at((std::size_t{level} * kCubeFaces) + face));
            }
        }
        wgpuBindGroupRelease(source);
    }

    // Adds the radiance passes' uniforms. The sky's matrix goes in turned, as transposed says.
    RadianceOffsets add_radiance_uniforms(const Scene3D& scene) {
        RadianceOffsets at;
        const std::uint32_t size = radiance_.size;
        const std::uint32_t levels = radiance_levels(size);
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            SkyUniforms sky = radiance_face_uniforms(scene, face);
            sky.orientation = transposed(sky.orientation);
            at.faces.at(face) = uniforms_.add(sky);
        }
        for (std::uint32_t level = 0; level < levels; ++level) {
            auto mipmap = std::make_unique<RadianceUniforms>();
            mipmap->params[1] = 1.0F / static_cast<float>(std::max(size >> level, 1U));
            const auto filter =
                std::make_unique<RadianceUniforms>(radiance_uniforms(size, levels, level));
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                mipmap->params[0] = static_cast<float>(face);
                filter->params[0] = static_cast<float>(face);
                at.mipmaps.push_back(uniforms_.add(*mipmap));
                at.filters.push_back(uniforms_.add(*filter));
            }
        }
        return at;
    }

    // Records a pass drawing one triangle into a face of a cube with pipeline, its textures
    // bound by textures and its uniforms at offset in uniforms.
    static void encode_cube_pass(WGPUCommandEncoder encoder, WGPUTextureView target,
                                 WGPURenderPipeline pipeline, WGPUBindGroup textures,
                                 WGPUBindGroup uniforms, std::uint32_t offset) {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = target;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0};
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 2, textures, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(pass, 3, uniforms, 1, &offset);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    // A texture and its sampler bound for a pass, as group 2 of layout.
    WGPUBindGroup pass_textures(WGPUBindGroupLayout layout, WGPUTextureView view,
                                WGPUSampler sampler) {
        std::array<WGPUBindGroupEntry, 2> entries{};
        entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].textureView = view;
        entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[1].binding = kSamplerBinding;
        entries[1].sampler = sampler;
        return bind_group(layout, entries);
    }

    // Records the scene's radiance, as Godot's Compatibility renderer draws its radiance map: the
    // sky into each face of the raw cube, its mipmaps, each texel the mean of the four under it
    // (WebGPU makes none itself), then each level of each face filtered from it.
    void encode_radiance(WGPUCommandEncoder encoder, const Scene3D& scene,
                         const RadianceOffsets& at) {
        const std::uint32_t levels = radiance_levels(radiance_.size);
        const auto [view, sampler] = binding(radiance_texture(scene), true);
        WGPUBindGroup sky = pass_textures(sprite_textures_layout_, view, sampler);
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            encode_cube_pass(encoder, radiance_.raw.faces.at(face), radiance_pipeline(0), sky,
                             sky_uniforms_group_, at.faces.at(face));
        }
        wgpuBindGroupRelease(sky);
        for (std::uint32_t level = 1; level < levels; ++level) {
            WGPUBindGroup source = pass_textures(
                cube_texture_layout_, radiance_.raw.levels.at(level - 1), radiance_sampler_);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                const std::size_t i = (std::size_t{level} * kCubeFaces) + face;
                encode_cube_pass(encoder, radiance_.raw.faces.at(i), radiance_pipeline(1), source,
                                 filter_uniforms_group_, at.mipmaps.at(i));
            }
            wgpuBindGroupRelease(source);
        }
        WGPUBindGroup source =
            pass_textures(cube_texture_layout_, radiance_.raw.view, radiance_sampler_);
        for (std::uint32_t level = 0; level < levels; ++level) {
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                const std::size_t i = (std::size_t{level} * kCubeFaces) + face;
                encode_cube_pass(encoder, radiance_.filtered.faces.at(i), radiance_pipeline(1),
                                 source, filter_uniforms_group_, at.filters.at(i));
            }
        }
        wgpuBindGroupRelease(source);
    }

    // The shadow passes' pipeline for a cull mode: shaders/shadow.hlsl, depth only.
    WGPURenderPipeline shadow_pipeline(CullMode cull) {
        WGPURenderPipeline& made = shadow_pipelines_.at(static_cast<std::size_t>(cull));
        if (made != nullptr) {
            return made;
        }
        const std::array attributes = mesh_attributes();
        WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        buffer.stepMode = WGPUVertexStepMode_Vertex;
        buffer.arrayStride = sizeof(Vertex3D);
        buffer.attributeCount = attributes.size();
        buffer.attributes = attributes.data();
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kShadowFragmentModule, shaders::kShadowFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = WGPUOptionalBool_True;
        depth.depthCompare = WGPUCompareFunction_Greater;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kShadowLayout);
        info.vertex.module = module(kShadowVertexModule, shaders::kShadowVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.vertex.bufferCount = 1;
        info.vertex.buffers = &buffer;
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.frontFace = WGPUFrontFace_CW;
        info.primitive.cullMode = gpu_cull(cull);
        info.depthStencil = &depth;
        info.fragment = &fragment;
        made = wgpuDeviceCreateRenderPipeline(device_, &info);
        return made;
    }

    // A shadow pass's albedo texture, bound as a sprite's is.
    WGPUBindGroup albedo_group(const std::pair<WGPUTextureView, WGPUSampler>& albedo) {
        TextureBindings key{};
        key[0] = albedo;
        if (const auto it = texture_groups_.find(key); it != texture_groups_.end()) {
            return it->second;
        }
        std::array<WGPUBindGroupEntry, 2> entries{WGPU_BIND_GROUP_ENTRY_INIT,
                                                  WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].textureView = albedo.first;
        entries[1].binding = kSamplerBinding;
        entries[1].sampler = albedo.second;
        WGPUBindGroup group = bind_group(sprite_textures_layout_, entries);
        texture_groups_.emplace(key, group);
        return group;
    }

    // Records the frame's shadow passes: each atlas in a pass of its own, each light's view drawn
    // into its rect.
    void encode_shadows(WGPUCommandEncoder encoder, const MeshUniforms& at) {
        const ShadowPlan& plan = *shadow_plan_;
        for (std::size_t atlas = 0; atlas < 2; ++atlas) {
            const bool positional = atlas == 1;
            if (std::ranges::none_of(plan.passes, [&](const ShadowPass& pass) {
                    return pass.positional == positional;
                })) {
                continue;
            }
            WGPURenderPassDepthStencilAttachment depth =
                WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            depth.view = shadow_atlases_.at(atlas).gpu.view;
            depth.depthLoadOp = WGPULoadOp_Clear;
            depth.depthStoreOp = WGPUStoreOp_Store;
            depth.depthClearValue = 0.0F;
            WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            descriptor.depthStencilAttachment = &depth;
            WGPURenderPassEncoder render = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
            WGPURenderPipeline bound = nullptr;
            for (std::size_t p = 0; p < plan.passes.size(); ++p) {
                const ShadowPass& pass = plan.passes[p];
                if (pass.positional != positional) {
                    continue;
                }
                const auto x = static_cast<float>(pass.rect.x);
                const auto y = static_cast<float>(pass.rect.y);
                const auto w = static_cast<float>(pass.rect.width);
                const auto h = static_cast<float>(pass.rect.height);
                wgpuRenderPassEncoderSetViewport(render, x, y, w, h, 0.0F, 1.0F);
                wgpuRenderPassEncoderSetScissorRect(render, pass.rect.x, pass.rect.y,
                                                    pass.rect.width, pass.rect.height);
                for (std::size_t i = 0; i < mesh_draws_.size(); ++i) {
                    const MeshDraw& draw = mesh_draws_[i];
                    const MeshBatch& batch = mesh_batches_[i];
                    if (!shadow_pass_draws(pass, draw, batch.bounds)) {
                        continue;
                    }
                    WGPURenderPipeline pipeline = shadow_pipeline(shadow_cull(pass, draw));
                    if (pipeline != bound) {
                        wgpuRenderPassEncoderSetPipeline(render, pipeline);
                        bound = pipeline;
                    }
                    const std::array<std::uint32_t, 2> vertex_offsets{at.passes[p],
                                                                      at.draws[i].first};
                    wgpuRenderPassEncoderSetBindGroup(render, 1, shadow_vertex_group_,
                                                      vertex_offsets.size(), vertex_offsets.data());
                    wgpuRenderPassEncoderSetBindGroup(render, 2, albedo_group(batch.bindings[0]), 0,
                                                      nullptr);
                    const std::uint32_t surface = at.draws[i].second;
                    wgpuRenderPassEncoderSetBindGroup(render, 3, shadow_fragment_group_, 1,
                                                      &surface);
                    wgpuRenderPassEncoderSetVertexBuffer(render, 0, batch.vertices, 0,
                                                         WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderSetIndexBuffer(render, batch.indices,
                                                        WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
                    wgpuRenderPassEncoderDrawIndexed(render, batch.index_count, 1, 0, 0, 0);
                }
            }
            wgpuRenderPassEncoderEnd(render);
            wgpuRenderPassEncoderRelease(render);
        }
    }

    // The pipeline drawing a material's meshes into a target format, made the first time a frame
    // needs it: it culls as the material says, and an Alpha material's blends and writes no
    // depth. A clustered one lights them with Forward+'s clusters.
    WGPURenderPipeline mesh_pipeline_for(WGPUTextureFormat format, const Material3D& material,
                                         bool clustered) {
        const bool blended = material.transparency == Transparency::Alpha;
        const BlendMode blend = blended ? material.blend : BlendMode::Mix;
        for (const MeshPipeline& made : mesh_pipelines_) {
            if (made.format == format && made.cull == material.cull && made.blended == blended &&
                made.blend == blend && made.clustered == clustered) {
                return made.pipeline;
            }
        }
        const std::array attributes = mesh_attributes();
        WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        buffer.stepMode = WGPUVertexStepMode_Vertex;
        buffer.arrayStride = sizeof(Vertex3D);
        buffer.attributeCount = attributes.size();
        buffer.attributes = attributes.data();

        const WGPUBlendState blending = blend_state(blend);
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        color.blend = blended ? &blending : nullptr;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module =
            clustered
                ? module(kMeshClusteredFragmentModule, joined(shaders::kMeshClusteredFragmentWgsl))
                : module(kMeshFragmentModule, joined(shaders::kMeshFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        // Reversed depth: nearer is greater.
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = blended ? WGPUOptionalBool_False : WGPUOptionalBool_True;
        depth.depthCompare = WGPUCompareFunction_Greater;

        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(clustered ? kMeshClusteredLayout : kMeshLayout);
        info.vertex.module = module(kMeshVertexModule, joined(shaders::kMeshVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.vertex.bufferCount = 1;
        info.vertex.buffers = &buffer;
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        // Front faces wind clockwise on the target, whose y goes down.
        info.primitive.frontFace = WGPUFrontFace_CW;
        info.primitive.cullMode = gpu_cull(material.cull);
        info.depthStencil = &depth;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        mesh_pipelines_.push_back({.format = format,
                                   .cull = material.cull,
                                   .blended = blended,
                                   .blend = blend,
                                   .clustered = clustered,
                                   .pipeline = pipeline});
        return pipeline;
    }

    // The depth buffer the frame's meshes draw over, made again when the target's size changes.
    void make_depth_target(std::uint32_t width, std::uint32_t height) {
        if (depth_.texture != nullptr && depth_width_ == width && depth_height_ == height) {
            return;
        }
        if (depth_.texture != nullptr) {
            pending_textures_.push_back(std::exchange(depth_, {}));
        }
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        // Depth of field loads it.
        info.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        info.size = {.width = width, .height = height, .depthOrArrayLayers = 1};
        info.format = kDepthFormat;
        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &info);
        depth_ = {.texture = texture,
                  .view = wgpuTextureCreateView(texture, nullptr),
                  .filter = Filter::Nearest};
        depth_width_ = width;
        depth_height_ = height;
    }

    // A mesh draw's textures and samplers, bound as a group made the first time it is needed.
    WGPUBindGroup mesh_texture_group(const MeshBindings& bindings) {
        TextureBindings key{};
        std::ranges::copy(bindings, key.begin());
        if (const auto it = texture_groups_.find(key); it != texture_groups_.end()) {
            return it->second;
        }
        constexpr std::uint32_t kMeshSlots = kMeshTextures + 5;
        std::array<WGPUBindGroupEntry, kMeshSlots * 2> entries{};
        for (std::uint32_t slot = 0; slot < kMeshSlots; ++slot) {
            WGPUBindGroupEntry& texture = entries.at(slot);
            texture = WGPU_BIND_GROUP_ENTRY_INIT;
            texture.binding = slot;
            texture.textureView = bindings.at(slot).first;
            WGPUBindGroupEntry& sampler = entries.at(kMeshSlots + slot);
            sampler = WGPU_BIND_GROUP_ENTRY_INIT;
            sampler.binding = kMeshSamplerBinding + slot;
            sampler.sampler = bindings.at(slot).second;
        }
        WGPUBindGroup group = bind_group(mesh_textures_layout_, entries);
        texture_groups_.emplace(key, group);
        return group;
    }

    // Adds the frame's mesh uniforms, with FSR 2's frames before and the camera's jitter in a
    // frame with it. The matrices go in turned, as transposed says.
    MeshUniforms add_mesh_uniforms(const Fsr2Frames* fsr2, const std::array<float, 2>& jitter) {
        MeshUniforms at;
        if (mesh_draws_.empty()) {
            return at;
        }
        const Scene3D& scene = *frame_.scene;
        // TAA's frame: the next jitter, from the camera the target's frame before was seen from
        // (this one's, for its first). FSR 2's jitter is its clock's.
        const TaaHistory* taa = uses_taa(scene) ? find_taa_history(frame_.target) : nullptr;
        const Mat4 view_projection = multiply(scene.projection, scene.camera);
        MeshCameraUniforms camera = mesh_camera_uniforms(scene);
        if (taa != nullptr) {
            camera =
                taa_camera_uniforms(scene, taa_jitter(taa->frames, scene_width_, scene_height_),
                                    taa->primed ? taa->view_projection : view_projection);
        } else if (fsr2 != nullptr) {
            camera = taa_camera_uniforms(
                scene, jitter, fsr2->clock.frames == 0 ? view_projection : fsr2->view_projection);
        }
        camera.view_projection = transposed(camera.view_projection);
        camera.previous_view_projection = transposed(camera.previous_view_projection);
        at.camera = uniforms_.add(camera);
        const auto lights = std::make_unique<MeshSceneUniforms>(mesh_scene_uniforms(scene));
        if (scene.method == RenderMethod::ForwardPlus) {
            upload_clusters();
            lights->cluster = clusters_.uniform();
        }
        at.scene = uniforms_.add(*lights);
        if (uses_ssr(scene)) {
            lights->ssao[3] = 0.0F;
            at.unreflected = uniforms_.add(*lights);
        }
        const ShadowPlan& plan = *shadow_plan_;
        auto shadows = std::make_unique<MeshShadowUniforms>(*plan.uniforms);
        for (MeshDirectionalShadowUniforms& light : shadows->directional) {
            for (Mat4& matrix : light.matrices) {
                matrix = transposed(matrix);
            }
        }
        for (MeshPositionalShadowUniforms& light : shadows->positional) {
            light.light = transposed(light.light);
            for (Mat4& face : light.faces) {
                face = transposed(face);
            }
        }
        at.shadows = uniforms_.add(*shadows);
        for (const ShadowPass& pass : plan.passes) {
            MeshShadowPassUniforms uniforms = pass.uniforms;
            uniforms.view = transposed(uniforms.view);
            uniforms.projection = transposed(uniforms.projection);
            at.passes.push_back(uniforms_.add(uniforms));
        }
        at.draws.reserve(mesh_draws_.size());
        for (const MeshDraw& draw : mesh_draws_) {
            MeshInstanceUniforms instance =
                mesh_instance_uniforms(draw.transform, draw.material, draw.previous);
            instance.model = transposed(instance.model);
            instance.normal_model = transposed(instance.normal_model);
            instance.previous_model = transposed(instance.previous_model);
            at.draws.emplace_back(uniforms_.add(instance),
                                  uniforms_.add(mesh_surface_uniforms(draw.material, draw.layers)));
        }
        return at;
    }

    // Builds the frame's Forward+ clusters and copies them and the frame's reflection probes to
    // the GPU, bound as a group made again when a buffer grows.
    void upload_clusters() {
        clusters_ = build_clusters(*frame_.scene, scene_width_, scene_height_);
        if (clusters_.lights.empty()) {
            clusters_.lights.emplace_back(); // a storage buffer is never empty
        }
        const bool lights_grew =
            upload_storage(cluster_lights_, std::as_bytes(std::span(clusters_.lights)));
        const bool cells_grew =
            upload_storage(cluster_cells_, std::as_bytes(std::span(clusters_.cells)));
        std::vector<MeshProbeUniforms> probes = probe_uniforms(frame_.scene->reflections);
        if (probes.empty()) {
            probes.emplace_back();
        }
        const bool probes_grew = upload_storage(probe_buffer_, std::as_bytes(std::span(probes)));
        if (cluster_group_ != nullptr && !lights_grew && !cells_grew && !probes_grew) {
            return;
        }
        if (cluster_group_ != nullptr) {
            wgpuBindGroupRelease(cluster_group_);
        }
        std::array<WGPUBindGroupEntry, 3> entries{};
        for (std::uint32_t binding = 0; binding < entries.size(); ++binding) {
            const std::array<const StorageBuffer*, 3> buffers{&cluster_lights_, &cluster_cells_,
                                                              &probe_buffer_};
            const StorageBuffer& storage = *buffers.at(binding);
            entries.at(binding) = WGPU_BIND_GROUP_ENTRY_INIT;
            entries.at(binding).binding = binding;
            entries.at(binding).buffer = storage.buffer;
            entries.at(binding).size = storage.capacity;
        }
        cluster_group_ = bind_group(mesh_clusters_layout_, entries);
    }

    // Writes bytes to a storage buffer, growing it first if they don't fit; says if it grew.
    bool upload_storage(StorageBuffer& storage, std::span<const std::byte> bytes) {
        const std::uint64_t size = bytes.size();
        const bool grows = size > storage.capacity;
        if (grows) {
            destroy_buffer(storage.buffer);
            storage.capacity = std::max({size, storage.capacity * 2, kMinVertexBytes});
            storage.buffer =
                create_buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, storage.capacity);
        }
        wgpuQueueWriteBuffer(queue_, storage.buffer, 0, bytes.data(), bytes.size());
        return grows;
    }

    // Records the frame's sky, when it has one (its uniforms at sky), then its meshes, in a pass
    // that clears the target and the depth buffer to the far plane. With ssao they are lit with
    // its occlusion, and with ssr they reflect its reflections.
    void encode_meshes(WGPUCommandEncoder encoder, const MeshUniforms& at,
                       std::optional<std::uint32_t> sky, bool ssao, bool ssr) {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        // A scene that post-processes draws its meshes into a texture the post passes read.
        color.view = world_view(*frame_.scene);
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = world_clear(*frame_.scene);
        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depth.view = depth_.view;
        depth.depthLoadOp = WGPULoadOp_Clear;
        // Kept for depth of field and FSR 2, which read it after.
        depth.depthStoreOp = frame_.scene->method == RenderMethod::ForwardPlus &&
                                     (uses_dof(frame_.scene->post) || uses_fsr2(*frame_.scene))
                                 ? WGPUStoreOp_Store
                                 : WGPUStoreOp_Discard;
        depth.depthClearValue = 0.0F;
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        descriptor.depthStencilAttachment = &depth;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (sky) {
            const auto [view, sampler] = binding(sky_texture(frame_.scene->sky), true);
            std::array<WGPUBindGroupEntry, 2> entries{};
            entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[0].binding = 0;
            entries[0].textureView = view;
            entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[1].binding = kSamplerBinding;
            entries[1].sampler = sampler;
            WGPUBindGroup texture = bind_group(sprite_textures_layout_, entries);
            wgpuRenderPassEncoderSetPipeline(
                pass, sky_pipeline_for(world_format(*frame_.scene, frame_format_)));
            wgpuRenderPassEncoderSetBindGroup(pass, 2, texture, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(pass, 3, sky_uniforms_group_, 1, &*sky);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuBindGroupRelease(texture);
        }
        if (!mesh_draws_.empty() && frame_.scene->method == RenderMethod::ForwardPlus) {
            wgpuRenderPassEncoderSetBindGroup(pass, 0, cluster_group_, 0, nullptr);
        }
        encode_mesh_draws(pass, at, false, ssao, ssr);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    // Records the frame's meshes in pass, in the order mesh_draw_order gives: with their prepass
    // pipelines (the opaque ones') for the normal-roughness prepass, or lit, with SSAO's
    // occlusion if ssao and SSR's reflections if ssr (with the scene's uniforms without SSR if
    // not, in a frame with it).
    void encode_mesh_draws(WGPURenderPassEncoder pass, const MeshUniforms& at, bool prepass,
                           bool ssao, bool ssr) {
        const std::uint32_t scene =
            !prepass && !ssr && uses_ssr(*frame_.scene) ? at.unreflected : at.scene;
        // The motion vectors' prepass reads where the vertices were from a second buffer.
        const bool motion = prepass && uses_motion_vectors(*frame_.scene);
        WGPURenderPipeline bound = nullptr;
        for (const std::size_t i :
             mesh_draw_order(std::span<const MeshDraw>(mesh_draws_), *frame_.scene,
                             [&](std::size_t draw) { return mesh_batches_[draw].bounds; })) {
            const MeshBatch& batch = mesh_batches_[i];
            WGPURenderPipeline pipeline = prepass ? batch.normal_pipeline : batch.pipeline;
            if (mesh_draws_[i].shadows == ShadowCasting::ShadowsOnly || pipeline == nullptr) {
                continue;
            }
            if (pipeline != bound) {
                wgpuRenderPassEncoderSetPipeline(pass, pipeline);
                bound = pipeline;
            }
            const std::array<std::uint32_t, 2> vertex_offsets{at.camera, at.draws[i].first};
            wgpuRenderPassEncoderSetBindGroup(pass, 1, mesh_vertex_group_, vertex_offsets.size(),
                                              vertex_offsets.data());
            MeshBindings bindings = batch.bindings;
            for (std::size_t atlas = 0; atlas < 2; ++atlas) {
                const bool drawn = (atlas == 0 ? shadow_plan_->directional_size
                                               : shadow_plan_->positional_size) != 0;
                bindings.at(kMeshTextures + atlas) = {
                    drawn ? shadow_atlases_.at(atlas).gpu.view : no_shadows_.view, shadow_sampler_};
            }
            bindings.at(kMeshTextures + 2) = {radiance_used(*frame_.scene) ? radiance_.filtered.view
                                                                           : no_radiance_.view,
                                              radiance_sampler_};
            bindings.at(kMeshTextures + 3) = {(ssao || ssr) && !prepass ? screen_.view
                                                                        : no_screen_.view,
                                              samplers_.at(sampler_index(Filter::Nearest, false))};
            const Reflections3D& reflections = frame_.scene->reflections;
            bindings.at(kMeshTextures + 4) = {!reflections.probes.empty() && !prepass
                                                  ? atlases_.at(reflections.atlas).view
                                                  : no_probes_.view,
                                              radiance_sampler_};
            wgpuRenderPassEncoderSetBindGroup(pass, 2, mesh_texture_group(bindings), 0, nullptr);
            const std::array<std::uint32_t, 3> fragment_offsets{at.draws[i].second, scene,
                                                                at.shadows};
            wgpuRenderPassEncoderSetBindGroup(pass, 3, mesh_fragment_group_,
                                              fragment_offsets.size(), fragment_offsets.data());
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, batch.vertices, 0, WGPU_WHOLE_SIZE);
            if (motion) {
                wgpuRenderPassEncoderSetVertexBuffer(pass, 1, batch.previous_vertices, 0,
                                                     WGPU_WHOLE_SIZE);
            }
            wgpuRenderPassEncoderSetIndexBuffer(pass, batch.indices, WGPUIndexFormat_Uint32, 0,
                                                WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDrawIndexed(pass, batch.index_count, 1, 0, 0, 0);
        }
    }

    // The pipeline drawing opaque meshes that cull as cull says into the normal-roughness
    // prepass, made the first time a frame needs it.
    WGPURenderPipeline normal_pipeline_for(CullMode cull) {
        WGPURenderPipeline& made = normal_pipelines_.at(static_cast<std::size_t>(cull));
        if (made != nullptr) {
            return made;
        }
        const std::array attributes = mesh_attributes();
        WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        buffer.stepMode = WGPUVertexStepMode_Vertex;
        buffer.arrayStride = sizeof(Vertex3D);
        buffer.attributeCount = attributes.size();
        buffer.attributes = attributes.data();
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = kNormalFormat;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module =
            module(kMeshNormalFragmentModule, joined(shaders::kMeshNormalFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = WGPUOptionalBool_True;
        depth.depthCompare = WGPUCompareFunction_Greater;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kMeshLayout);
        info.vertex.module = module(kMeshVertexModule, joined(shaders::kMeshVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.vertex.bufferCount = 1;
        info.vertex.buffers = &buffer;
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.frontFace = WGPUFrontFace_CW;
        info.primitive.cullMode = gpu_cull(cull);
        info.depthStencil = &depth;
        info.fragment = &fragment;
        made = wgpuDeviceCreateRenderPipeline(device_, &info);
        return made;
    }

    // The pipeline drawing opaque meshes that cull as cull says into TAA's prepass, which draws
    // their normals and their motion vectors from a second vertex buffer of where their vertices
    // were in the frame before, made the first time a frame needs it.
    WGPURenderPipeline motion_pipeline_for(CullMode cull) {
        WGPURenderPipeline& made = motion_pipelines_.at(static_cast<std::size_t>(cull));
        if (made != nullptr) {
            return made;
        }
        const std::array attributes = mesh_attributes();
        const WGPUVertexAttribute previous_attribute{
            .nextInChain = nullptr,
            .format = WGPUVertexFormat_Float32x3,
            .offset = offsetof(Vertex3D, position),
            .shaderLocation = static_cast<std::uint32_t>(attributes.size())};
        std::array<WGPUVertexBufferLayout, 2> buffers{};
        for (WGPUVertexBufferLayout& buffer : buffers) {
            buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffer.stepMode = WGPUVertexStepMode_Vertex;
            buffer.arrayStride = sizeof(Vertex3D);
        }
        buffers[0].attributeCount = attributes.size();
        buffers[0].attributes = attributes.data();
        buffers[1].attributeCount = 1;
        buffers[1].attributes = &previous_attribute;
        std::array<WGPUColorTargetState, 2> colors{};
        colors[0] = WGPU_COLOR_TARGET_STATE_INIT;
        colors[0].format = kNormalFormat;
        colors[1] = WGPU_COLOR_TARGET_STATE_INIT;
        colors[1].format = kVelocityFormat;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module =
            module(kMeshMotionFragmentModule, joined(shaders::kMeshMotionFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = colors.size();
        fragment.targets = colors.data();
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = WGPUOptionalBool_True;
        depth.depthCompare = WGPUCompareFunction_Greater;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kMeshLayout);
        info.vertex.module =
            module(kMeshMotionVertexModule, joined(shaders::kMeshMotionVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.vertex.bufferCount = buffers.size();
        info.vertex.buffers = buffers.data();
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.frontFace = WGPUFrontFace_CW;
        info.primitive.cullMode = gpu_cull(cull);
        info.depthStencil = &depth;
        info.fragment = &fragment;
        made = wgpuDeviceCreateRenderPipeline(device_, &info);
        return made;
    }

    // SSAO's pipeline for a target format, made the first time a frame needs it.
    WGPURenderPipeline ssao_pipeline_for(WGPUTextureFormat format) {
        for (const auto& [made, pipeline] : ssao_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kSsaoFragmentModule, joined(shaders::kSsaoFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kSsaoLayout);
        info.vertex.module = module(kSsaoVertexModule, joined(shaders::kSsaoVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        ssao_pipelines_.emplace_back(format, pipeline);
        return pipeline;
    }

    // A texture of format w x h that passes draw into and read, and copies with copy_usage.
    GpuTexture pass_texture(std::uint32_t w, std::uint32_t h, WGPUTextureFormat format,
                            WGPUTextureUsage copy_usage = WGPUTextureUsage_None,
                            std::uint32_t levels = 1) {
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.usage = WGPUTextureUsage_TextureBinding | copy_usage;
        if (copy_usage != WGPUTextureUsage_CopyDst) {
            info.usage |= WGPUTextureUsage_RenderAttachment;
        }
        info.size = {.width = w, .height = h, .depthOrArrayLayers = 1};
        info.format = format;
        info.mipLevelCount = levels;
        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &info);
        return {.texture = texture,
                .view = wgpuTextureCreateView(texture, nullptr),
                .filter = Filter::Nearest};
    }

    // Drops a texture the passes draw with once the frame is sent.
    void release_pass_texture(GpuTexture& texture) {
        if (texture.texture != nullptr) {
            forget_texture_groups(texture.view);
            pending_textures_.push_back(std::exchange(texture, {}));
        }
    }

    // The normal-roughness prepass's normals and depth at the frame's size, made again when it
    // changes.
    void prepare_prepass(std::uint32_t width, std::uint32_t height) {
        if (prepass_.normals.texture != nullptr && prepass_.width == width &&
            prepass_.height == height) {
            return;
        }
        release_prepass();
        prepass_.normals = pass_texture(width, height, kNormalFormat);
        prepass_.depth = pass_texture(width, height, kDepthFormat);
        prepass_.width = width;
        prepass_.height = height;
    }

    void release_prepass() {
        release_pass_texture(prepass_.normals);
        release_pass_texture(prepass_.depth);
        prepass_.width = 0;
        prepass_.height = 0;
    }

    // The textures a frame of width x height with settings draws SSAO with: those ssao_plan
    // sizes. Made again when either changes.
    void prepare_ssao(std::uint32_t width, std::uint32_t height, const SsaoSettings& settings) {
        if (ssao_targets_.width == width && ssao_targets_.height == height &&
            ssao_targets_.settings == settings) {
            return;
        }
        release_ssao_targets();
        const SsaoPlan plan = ssao_plan(settings, width, height);
        // The last pass draws into the screen array instead.
        for (std::uint32_t i = 0; i < kSsaoTextures; ++i) {
            const PostSize size = plan.sizes.at(i);
            if (size.width != 0 && i != kSsaoFinal) {
                ssao_targets_.textures.at(i) =
                    pass_texture(size.width, size.height, ssao_gpu_format(ssao_format(i)));
            }
        }
        ssao_targets_.width = width;
        ssao_targets_.height = height;
        ssao_targets_.settings = settings;
    }

    void release_ssao_targets() {
        for (GpuTexture& texture : ssao_targets_.textures) {
            release_pass_texture(texture);
        }
        ssao_targets_.width = 0;
        ssao_targets_.height = 0;
    }

    // The prepass, which draws the opaque meshes' normals and depth, which SSAO, SSR and TAA
    // read, and their motion vectors into velocity, unless it is null.
    void encode_prepass(WGPUCommandEncoder encoder, const MeshUniforms& at,
                        WGPUTextureView velocity) {
        std::array<WGPURenderPassColorAttachment, 2> colors{};
        for (WGPURenderPassColorAttachment& color : colors) {
            color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 0.0};
        }
        colors[0].view = prepass_.normals.view;
        colors[1].view = velocity;
        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depth.view = prepass_.depth.view;
        depth.depthLoadOp = WGPULoadOp_Clear;
        depth.depthStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = 0.0F;
        WGPURenderPassDescriptor prepass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        prepass.colorAttachmentCount = velocity != nullptr ? 2 : 1;
        prepass.colorAttachments = colors.data();
        prepass.depthStencilAttachment = &depth;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &prepass);
        encode_mesh_draws(pass, at, true, false, false);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    // ssao.hlsl's passes from the prepass to the occlusion at the frame's size, in the order plan
    // gives, their uniforms at uniforms.
    void encode_ssao(WGPUCommandEncoder encoder, const SsaoPlan& plan,
                     std::span<const std::uint32_t> uniforms) {
        for (std::size_t i = 0; i < plan.steps.size(); ++i) {
            const SsaoStep& step = plan.steps[i];
            std::array<WGPUBindGroupEntry, kSsaoSlots> entries{};
            for (std::uint32_t slot = 0; slot < kSsaoSlots; ++slot) {
                const std::uint32_t index = step.slots.at(slot);
                WGPUTextureView view = white_.view;
                if (index == kSsaoDepthBuffer) {
                    view = prepass_.depth.view;
                } else if (index == kSsaoNormals) {
                    view = prepass_.normals.view;
                } else if (index < kSsaoTextures &&
                           ssao_targets_.textures.at(index).view != nullptr) {
                    view = ssao_targets_.textures.at(index).view;
                }
                entries.at(slot) = WGPU_BIND_GROUP_ENTRY_INIT;
                entries.at(slot).binding = slot;
                entries.at(slot).textureView = view;
            }
            WGPUBindGroup textures = bind_group(ssao_textures_layout_, entries);
            // The last pass draws into the screen array's first layer, which the mesh pass reads.
            const bool last = step.target == kSsaoFinal;
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = last ? screen_.layers[0] : ssao_targets_.textures.at(step.target).view;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            descriptor.colorAttachmentCount = 1;
            descriptor.colorAttachments = &color;
            WGPURenderPassEncoder draw = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
            wgpuRenderPassEncoderSetPipeline(
                draw,
                ssao_pipeline_for(last ? kHdrFormat : ssao_gpu_format(ssao_format(step.target))));
            wgpuRenderPassEncoderSetBindGroup(draw, 2, textures, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(draw, 3, ssao_uniforms_group_, 1, &uniforms[i]);
            wgpuRenderPassEncoderDraw(draw, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(draw);
            wgpuRenderPassEncoderRelease(draw);
            wgpuBindGroupRelease(textures);
        }
    }

    // SSR's uniforms as the GPU reads them: the matrices turned, as transposed says.
    static SsrUniforms gpu_ssr_uniforms(SsrUniforms uniforms) {
        uniforms.projection = transposed(uniforms.projection);
        uniforms.inv_projection = transposed(uniforms.inv_projection);
        uniforms.reprojection = transposed(uniforms.reprojection);
        return uniforms;
    }

    // SSR's pipeline for a target format, made the first time a frame needs it.
    WGPURenderPipeline ssr_pipeline_for(WGPUTextureFormat format) {
        for (const auto& [made, pipeline] : ssr_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kSsrFragmentModule, joined(shaders::kSsrFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kSsrLayout);
        info.vertex.module = module(kSsrVertexModule, joined(shaders::kSsrVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        ssr_pipelines_.emplace_back(format, pipeline);
        return pipeline;
    }

    // The trace's pipeline, which draws the reflections and their mip levels, made the first
    // time a frame needs it.
    WGPURenderPipeline ssr_trace_pipeline() {
        if (ssr_trace_pipeline_ != nullptr) {
            return ssr_trace_pipeline_;
        }
        std::array<WGPUColorTargetState, 2> colors{};
        colors[0] = WGPU_COLOR_TARGET_STATE_INIT;
        colors[0].format = ssr_gpu_format(SsrFormat::RGBA16F);
        colors[1] = WGPU_COLOR_TARGET_STATE_INIT;
        colors[1].format = ssr_gpu_format(SsrFormat::R8);
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kSsrTraceFragmentModule, joined(shaders::kSsrTraceFragmentWgsl));
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = colors.size();
        fragment.targets = colors.data();
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kSsrLayout);
        info.vertex.module = module(kSsrVertexModule, joined(shaders::kSsrVertexWgsl));
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        ssr_trace_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &info);
        return ssr_trace_pipeline_;
    }

    // The textures a frame of width x height with settings draws SSR with (those of ssr_plan,
    // and Hi-Z's and the reflections' levels with mipmaps), made again when either changes, and
    // target's frame before at SSR's size, made empty when it has none or its size changes.
    void prepare_ssr(TextureId target, std::uint32_t width, std::uint32_t height,
                     const SsrSettings& settings) {
        const SsrPlan plan = ssr_plan(settings, width, height);
        if (ssr_targets_.width != width || ssr_targets_.height != height ||
            !(ssr_targets_.settings == settings)) {
            release_ssr_targets();
            // The last pass draws into the screen array instead.
            for (std::uint32_t i = 0; i < kSsrTextures; ++i) {
                const PostSize size = plan.sizes.at(i);
                if (size.width != 0 && i != kSsrFinal) {
                    ssr_targets_.textures.at(i) =
                        pass_texture(size.width, size.height, ssr_gpu_format(ssr_format(i)),
                                     WGPUTextureUsage_CopySrc);
                }
            }
            ssr_targets_.hiz_mips =
                pass_texture(plan.size.width, plan.size.height, ssr_gpu_format(ssr_format(kSsrHiz)),
                             WGPUTextureUsage_CopyDst, plan.levels);
            ssr_targets_.color_mips = pass_texture(plan.size.width, plan.size.height,
                                                   ssr_gpu_format(ssr_format(kSsrColor)),
                                                   WGPUTextureUsage_CopyDst, plan.levels);
            ssr_targets_.width = width;
            ssr_targets_.height = height;
            ssr_targets_.settings = settings;
        }
        SsrHistory& history = ssr_history(target);
        if (history.texture.texture == nullptr || history.width != plan.size.width ||
            history.height != plan.size.height) {
            release_pass_texture(history.texture);
            history = {.texture = pass_texture(plan.size.width, plan.size.height,
                                               ssr_gpu_format(SsrFormat::RGBA16F)),
                       .width = plan.size.width,
                       .height = plan.size.height,
                       .camera = {},
                       .primed = false};
        }
    }

    void release_ssr_targets() {
        for (GpuTexture& texture : ssr_targets_.textures) {
            release_pass_texture(texture);
        }
        release_pass_texture(ssr_targets_.hiz_mips);
        release_pass_texture(ssr_targets_.color_mips);
        ssr_targets_.width = 0;
        ssr_targets_.height = 0;
    }

    // target's frame before, made empty when it has none.
    SsrHistory& ssr_history(TextureId target) {
        for (auto& [id, history] : ssr_histories_) {
            if (id == target) {
                return history;
            }
        }
        return ssr_histories_.emplace_back(target, SsrHistory{}).second;
    }

    void forget_ssr_history(TextureId target) {
        const auto it = std::ranges::find_if(
            ssr_histories_, [&](const auto& entry) { return entry.first == target; });
        if (it == ssr_histories_.end()) {
            return;
        }
        release_pass_texture(it->second.texture);
        ssr_histories_.erase(it);
    }

    // target's history for TAA at width x height, made empty when it has none or it was drawn
    // at another size.
    void prepare_taa(TextureId target, std::uint32_t width, std::uint32_t height) {
        TaaHistory* found = find_taa_history(target);
        if (found == nullptr) {
            found = &taa_histories_.emplace_back(target, TaaHistory{}).second;
        }
        TaaHistory& history = *found;
        if (history.color[0].texture != nullptr && history.width == width &&
            history.height == height) {
            return;
        }
        release_taa_textures(history);
        history = TaaHistory{};
        for (std::size_t i = 0; i < 2; ++i) {
            // Copied from the world, for a target's first frame, and back to it after.
            history.color.at(i) = pass_texture(width, height, kHdrFormat,
                                               WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst);
            history.velocity.at(i) = pass_texture(width, height, kVelocityFormat);
        }
        history.width = width;
        history.height = height;
    }

    // target's history for TAA, or null when it has none.
    TaaHistory* find_taa_history(TextureId target) {
        for (auto& [id, history] : taa_histories_) {
            if (id == target) {
                return &history;
            }
        }
        return nullptr;
    }

    void release_taa_textures(TaaHistory& history) {
        for (GpuTexture& texture : history.color) {
            release_pass_texture(texture);
        }
        for (GpuTexture& texture : history.velocity) {
            release_pass_texture(texture);
        }
    }

    void forget_taa_history(TextureId target) {
        const auto it = std::ranges::find_if(
            taa_histories_, [&](const auto& entry) { return entry.first == target; });
        if (it == taa_histories_.end()) {
            return;
        }
        release_taa_textures(it->second);
        taa_histories_.erase(it);
    }

    // TAA's pipeline, made the first time a frame needs it.
    WGPURenderPipeline taa_pipeline() {
        if (taa_pipeline_ != nullptr) {
            return taa_pipeline_;
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = kHdrFormat;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kTaaFragmentModule, shaders::kTaaFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kTaaLayout);
        info.vertex.module = module(kTaaVertexModule, shaders::kTaaVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        taa_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &info);
        return taa_pipeline_;
    }

    // TAA's pass, its uniforms at uniforms: the world, drawn with the frame's jitter, blended
    // with the history by the prepass's depth and motion vectors (the history's next texture)
    // into the history's next result, which is copied over the world; a target's first frame is
    // kept as it is. The history keeps the camera the frame was seen from without its jitter,
    // view_projection.
    void encode_taa(WGPUCommandEncoder encoder, TaaHistory& history, std::uint32_t uniforms,
                    const Mat4& view_projection) {
        const std::size_t next = 1 - history.last;
        const GpuTexture& resolved = history.color.at(next);
        if (history.primed) {
            const std::array<WGPUTextureView, kTaaSlots> views{
                forward_targets_.world.view, prepass_.depth.view, history.velocity.at(next).view,
                history.velocity.at(history.last).view, history.color.at(history.last).view};
            std::array<WGPUBindGroupEntry, kTaaSlots> entries{};
            for (std::uint32_t slot = 0; slot < kTaaSlots; ++slot) {
                entries.at(slot) = WGPU_BIND_GROUP_ENTRY_INIT;
                entries.at(slot).binding = slot;
                entries.at(slot).textureView = views.at(slot);
            }
            WGPUBindGroup textures = bind_group(taa_textures_layout_, entries);
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = resolved.view;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            descriptor.colorAttachmentCount = 1;
            descriptor.colorAttachments = &color;
            WGPURenderPassEncoder draw = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
            wgpuRenderPassEncoderSetPipeline(draw, taa_pipeline());
            wgpuRenderPassEncoderSetBindGroup(draw, 2, textures, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(draw, 3, taa_uniforms_group_, 1, &uniforms);
            wgpuRenderPassEncoderDraw(draw, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(draw);
            wgpuRenderPassEncoderRelease(draw);
            wgpuBindGroupRelease(textures);
        }
        WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = history.primed ? resolved.texture : forward_targets_.world.texture;
        WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        to.texture = history.primed ? forward_targets_.world.texture : resolved.texture;
        const WGPUExtent3D extent{
            .width = frame_width_, .height = frame_height_, .depthOrArrayLayers = 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
        history.last = next;
        history.view_projection = view_projection;
        history.primed = true;
        ++history.frames;
    }

    // The size a frame of width x height draws its sky and meshes at: FSR 2's render size, or its
    // own.
    static std::array<std::uint32_t, 2> scene_size(const Frame& frame, std::uint32_t width,
                                                   std::uint32_t height) {
        if (frame.scene && uses_fsr2(*frame.scene)) {
            return fsr2_render_size(*frame.scene, width, height);
        }
        return {width, height};
    }

    // The textures FSR 2's passes draw into at sizes (the render size, then the frame's) and
    // target's frames before, made empty when it has none or they were drawn at other sizes.
    void prepare_fsr2(TextureId target, const std::array<std::uint32_t, 4>& sizes) {
        const std::uint32_t render_width = sizes[0];
        const std::uint32_t render_height = sizes[1];
        const std::uint32_t width = sizes[2];
        const std::uint32_t height = sizes[3];
        Fsr2Textures& t = fsr2_textures_;
        if (t.color.texture == nullptr || t.sizes != sizes) {
            release_fsr2_textures();
            const auto [mip_width, mip_height] = fsr2_luma_mip_size(render_width, render_height);
            t.color = pass_texture(render_width, render_height, kHdrFormat);
            t.velocity = pass_texture(render_width, render_height, kVelocityFormat);
            t.luma_mip = pass_texture(mip_width, mip_height, kFsr2LumaTargets[0]);
            t.dilated_depth = pass_texture(render_width, render_height, kFsr2DilateTargets[0]);
            t.lock_luma = pass_texture(render_width, render_height, kFsr2DilateTargets[2]);
            t.prepared = pass_texture(render_width, render_height, kFsr2DepthClipTargets[0]);
            t.reactive = pass_texture(render_width, render_height, kFsr2DepthClipTargets[1]);
            t.new_locks = pass_texture(width, height, kFsr2LockTargets[0]);
            // A word a render pixel.
            t.reconstructed_bytes = std::uint64_t{render_width} * render_height * 4;
            t.reconstructed = create_buffer(WGPUBufferUsage_Storage, t.reconstructed_bytes);
            t.sizes = sizes;
        }
        Fsr2Frames* found = find_fsr2_frames(target);
        if (found == nullptr) {
            found = &fsr2_frames_.emplace_back(target, Fsr2Frames{}).second;
        }
        Fsr2Frames& frames = *found;
        if (frames.color[0].texture != nullptr && frames.sizes == sizes) {
            return;
        }
        release_fsr2_frames(frames);
        frames = Fsr2Frames{};
        for (std::size_t i = 0; i < 2; ++i) {
            frames.color.at(i) = pass_texture(width, height, kFsr2AccumulateTargets[0]);
            frames.lock_status.at(i) = pass_texture(width, height, kFsr2AccumulateTargets[1]);
            frames.luma_history.at(i) = pass_texture(width, height, kFsr2AccumulateTargets[2]);
            frames.dilated_motion.at(i) =
                pass_texture(render_width, render_height, kFsr2DilateTargets[1]);
        }
        frames.sizes = sizes;
    }

    // target's frames before for FSR 2, or null when it has none.
    Fsr2Frames* find_fsr2_frames(TextureId target) {
        for (auto& [id, frames] : fsr2_frames_) {
            if (id == target) {
                return &frames;
            }
        }
        return nullptr;
    }

    void release_fsr2_frames(Fsr2Frames& frames) {
        for (std::array<GpuTexture, 2>* pair :
             {&frames.color, &frames.lock_status, &frames.luma_history, &frames.dilated_motion}) {
            for (GpuTexture& texture : *pair) {
                release_pass_texture(texture);
            }
        }
    }

    void forget_fsr2_frames(TextureId target) {
        const auto it = std::ranges::find_if(
            fsr2_frames_, [&](const auto& entry) { return entry.first == target; });
        if (it == fsr2_frames_.end()) {
            return;
        }
        release_fsr2_frames(it->second);
        fsr2_frames_.erase(it);
    }

    void release_fsr2_textures() {
        Fsr2Textures& t = fsr2_textures_;
        for (GpuTexture* texture : {&t.color, &t.velocity, &t.luma_mip, &t.dilated_depth,
                                    &t.lock_luma, &t.prepared, &t.reactive, &t.new_locks}) {
            release_pass_texture(*texture);
        }
        if (t.reconstructed != nullptr) {
            pending_buffers_.push_back(std::exchange(t.reconstructed, nullptr));
        }
        t.reconstructed_bytes = 0;
        t.sizes = {};
    }

    // FSR 2's pipeline for a pass, made the first time a frame needs it.
    WGPURenderPipeline fsr2_pipeline(Fsr2Pass which) {
        const auto index = static_cast<std::size_t>(which);
        WGPURenderPipeline& made = fsr2_pipelines_.at(index);
        if (made != nullptr) {
            return made;
        }
        const Fsr2PassCode pass = fsr2_pass_code(which);
        std::array<WGPUColorTargetState, 3> colors{};
        for (std::size_t target = 0; target < pass.targets.size(); ++target) {
            colors.at(target) = WGPU_COLOR_TARGET_STATE_INIT;
            colors.at(target).format = pass.targets[target];
        }
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kFsr2Modules + (2 * index) + 1, pass.fragment);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = pass.targets.size();
        fragment.targets = colors.data();
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kFsr2Layout + index);
        info.vertex.module = module(kFsr2Modules + (2 * index), pass.vertex);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        made = wgpuDeviceCreateRenderPipeline(device_, &info);
        return made;
    }

    // The reconstruct pass's pipeline, made the first time a frame needs it.
    WGPUComputePipeline fsr2_reconstruct_pipeline() {
        if (fsr2_reconstruct_ == nullptr) {
            WGPUComputePipelineDescriptor info = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            info.layout = pipeline_layouts_.at(kFsr2ReconstructLayout);
            info.compute.module =
                module(kFsr2ReconstructModule, joined(shaders::kFsr2ReconstructComputeWgsl));
            info.compute.entryPoint = text_view("cs_main");
            fsr2_reconstruct_ = wgpuDeviceCreateComputePipeline(device_, &info);
        }
        return fsr2_reconstruct_;
    }

    // Clears a texture the frame draws into to transparent black.
    static void clear_texture(WGPUCommandEncoder encoder, WGPUTextureView view) {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = view;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 0.0};
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    // FSR 2's passes, as Godot's FSR 2 effect dispatches them, their uniforms at uniforms (the
    // passes', then the reconstruct pass's two steps'): the world, drawn at the render size with
    // jitter, upscaled by the depth buffer, the prepass's motion vectors and the target's frames
    // before into Forward+'s world at the frame's size, which the post passes read. The frames
    // before keep the camera the frame was seen from without its jitter, view_projection; a reset
    // frame starts them at 0, as the software device's.
    void encode_fsr2(WGPUCommandEncoder encoder, Fsr2Frames& frames,
                     const std::array<std::uint32_t, 3>& uniforms, const Mat4& view_projection) {
        const std::size_t last = frames.last;
        const std::size_t next = 1 - last;
        if (frames.clock.frames == 0) {
            for (const GpuTexture* texture :
                 {&frames.color.at(last), &frames.lock_status.at(last),
                  &frames.luma_history.at(last), &frames.dilated_motion.at(last)}) {
                clear_texture(encoder, texture->view);
            }
        }
        const Fsr2Textures& t = fsr2_textures_;
        // The depth clip pass's reconstructed depths.
        const std::array reconstructed{buffer_entry(0, t.reconstructed, t.reconstructed_bytes)};
        const auto pass = [&](Fsr2Pass which, std::initializer_list<WGPUTextureView> targets,
                              std::initializer_list<WGPUTextureView> sources) {
            const auto index = static_cast<std::size_t>(which);
            std::array<WGPUBindGroupEntry, 8> entries{};
            std::size_t bound = 0;
            for (WGPUTextureView source : sources) {
                WGPUBindGroupEntry& entry = entries.at(bound);
                entry = WGPU_BIND_GROUP_ENTRY_INIT;
                entry.binding = static_cast<std::uint32_t>(bound++);
                entry.textureView = source;
            }
            WGPUBindGroup textures =
                bind_group(fsr2_textures_layouts_.at(index),
                           std::span<const WGPUBindGroupEntry>(entries.data(), bound));
            std::array<WGPURenderPassColorAttachment, 3> colors{};
            std::size_t count = 0;
            for (WGPUTextureView target : targets) {
                WGPURenderPassColorAttachment& color = colors.at(count++);
                color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                color.view = target;
                color.loadOp = WGPULoadOp_Clear;
                color.storeOp = WGPUStoreOp_Store;
            }
            WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            descriptor.colorAttachmentCount = count;
            descriptor.colorAttachments = colors.data();
            WGPURenderPassEncoder draw = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
            wgpuRenderPassEncoderSetPipeline(draw, fsr2_pipeline(which));
            WGPUBindGroup storage = nullptr;
            if (which == Fsr2Pass::DepthClip) {
                storage = bind_group(fsr2_storage_layout_, reconstructed);
                wgpuRenderPassEncoderSetBindGroup(draw, 0, storage, 0, nullptr);
            }
            wgpuRenderPassEncoderSetBindGroup(draw, 2, textures, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(draw, 3, fsr2_uniforms_group_, 1, uniforms.data());
            wgpuRenderPassEncoderDraw(draw, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(draw);
            wgpuRenderPassEncoderRelease(draw);
            wgpuBindGroupRelease(textures);
            if (storage != nullptr) {
                wgpuBindGroupRelease(storage);
            }
        };
        pass(Fsr2Pass::Luma, {t.luma_mip.view}, {t.color.view});
        pass(Fsr2Pass::Dilate,
             {t.dilated_depth.view, frames.dilated_motion.at(next).view, t.lock_luma.view},
             {depth_.view, t.velocity.view, t.color.view});
        // The reconstruct pass clears the depths, then scatters them.
        std::array<WGPUBindGroupEntry, 2> dilated{};
        for (std::uint32_t slot = 0; slot < dilated.size(); ++slot) {
            dilated.at(slot) = WGPU_BIND_GROUP_ENTRY_INIT;
            dilated.at(slot).binding = slot;
        }
        dilated[0].textureView = t.dilated_depth.view;
        dilated[1].textureView = frames.dilated_motion.at(next).view;
        WGPUBindGroup inputs = bind_group(fsr2_compute_textures_layout_, dilated);
        WGPUBindGroup outputs = bind_group(fsr2_compute_storage_layout_, reconstructed);
        for (std::size_t step = 0; step < 2; ++step) {
            WGPUComputePassEncoder compute = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
            wgpuComputePassEncoderSetPipeline(compute, fsr2_reconstruct_pipeline());
            wgpuComputePassEncoderSetBindGroup(compute, 0, inputs, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(compute, 1, outputs, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(compute, 2, fsr2_compute_uniforms_group_, 1,
                                               &uniforms.at(1 + step));
            wgpuComputePassEncoderDispatchWorkgroups(
                compute, (scene_width_ + kFsr2Threads - 1) / kFsr2Threads,
                (scene_height_ + kFsr2Threads - 1) / kFsr2Threads, 1);
            wgpuComputePassEncoderEnd(compute);
            wgpuComputePassEncoderRelease(compute);
        }
        wgpuBindGroupRelease(inputs);
        wgpuBindGroupRelease(outputs);
        pass(Fsr2Pass::DepthClip, {t.prepared.view, t.reactive.view},
             {frames.dilated_motion.at(next).view, t.dilated_depth.view, t.color.view,
              frames.dilated_motion.at(last).view, t.velocity.view});
        pass(Fsr2Pass::Lock, {t.new_locks.view}, {t.lock_luma.view});
        pass(Fsr2Pass::Accumulate,
             {frames.color.at(next).view, frames.lock_status.at(next).view,
              frames.luma_history.at(next).view},
             {frames.dilated_motion.at(next).view, t.prepared.view, t.reactive.view,
              frames.color.at(last).view, frames.lock_status.at(last).view, t.new_locks.view,
              t.luma_mip.view, frames.luma_history.at(last).view});
        pass(Fsr2Pass::Rcas, {forward_targets_.world.view}, {frames.color.at(next).view});
        frames.last = next;
        frames.view_projection = view_projection;
    }

    // ssr.hlsl's passes, from the prepass's depth and normals and history, to the reflections at
    // the frame's size, in the order plan gives, their uniforms at uniforms.
    void encode_ssr(WGPUCommandEncoder encoder, const SsrPlan& plan,
                    std::span<const std::uint32_t> uniforms, const SsrHistory& history) {
        for (std::size_t i = 0; i < plan.steps.size(); ++i) {
            const SsrStep& step = plan.steps[i];
            if (step.pass != SsrPass::CopyLevels) {
                encode_ssr_step(encoder, step, uniforms[i], history);
                continue;
            }
            // GPUs can't draw into one level of a texture while reading another, so each level
            // is drawn into a texture of its own and copied into one with mipmaps for the passes
            // that read them all.
            const bool hiz = step.target == kSsrHizMips;
            for (std::uint32_t level = 0; level < plan.levels; ++level) {
                const std::uint32_t index = (hiz ? kSsrHiz : kSsrColor) + level;
                const PostSize size = plan.sizes.at(index);
                WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                from.texture = ssr_targets_.textures.at(index).texture;
                WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                to.texture = hiz ? ssr_targets_.hiz_mips.texture : ssr_targets_.color_mips.texture;
                to.mipLevel = level;
                const WGPUExtent3D extent{
                    .width = size.width, .height = size.height, .depthOrArrayLayers = 1};
                wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
            }
        }
    }

    // One of SSR's passes, other than a copy, over its target with its uniforms at uniforms: the
    // trace's over the reflections' first level and the mip levels, the last frame's over
    // history's texture.
    void encode_ssr_step(WGPUCommandEncoder encoder, const SsrStep& step, std::uint32_t uniforms,
                         const SsrHistory& history) {
        std::array<WGPUBindGroupEntry, kSsrSlots> entries{};
        for (std::uint32_t slot = 0; slot < kSsrSlots; ++slot) {
            const std::uint32_t index = step.slots.at(slot);
            WGPUTextureView view = white_.view;
            if (index == kSsrDepthBuffer) {
                view = prepass_.depth.view;
            } else if (index == kSsrNormals) {
                view = prepass_.normals.view;
            } else if (index == kSsrHizMips) {
                view = ssr_targets_.hiz_mips.view;
            } else if (index == kSsrColorMips) {
                view = ssr_targets_.color_mips.view;
            } else if (index == kSsrLastFrame) {
                view = history.texture.view;
            } else if (index == kSsrWorld) {
                view = world_view(*frame_.scene);
            } else if (index < kSsrTextures && ssr_targets_.textures.at(index).view != nullptr) {
                view = ssr_targets_.textures.at(index).view;
            }
            entries.at(slot) = WGPU_BIND_GROUP_ENTRY_INIT;
            entries.at(slot).binding = slot;
            entries.at(slot).textureView = view;
        }
        WGPUBindGroup textures = bind_group(ssr_textures_layout_, entries);
        const bool trace = step.pass == SsrPass::Trace;
        std::array<WGPURenderPassColorAttachment, 2> colors{};
        for (WGPURenderPassColorAttachment& color : colors) {
            color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
        }
        colors[0].view = step.target == kSsrLastFrame ? history.texture.view
                         : step.target == kSsrFinal   ? screen_.layers[1]
                                                      : ssr_targets_.textures.at(step.target).view;
        colors[1].view = ssr_targets_.textures.at(kSsrMipLevel).view;
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = trace ? 2 : 1;
        descriptor.colorAttachments = colors.data();
        WGPURenderPassEncoder draw = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderSetPipeline(
            draw, trace ? ssr_trace_pipeline()
                        : ssr_pipeline_for(ssr_gpu_format(ssr_format(step.target))));
        wgpuRenderPassEncoderSetBindGroup(draw, 2, textures, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(draw, 3, ssr_uniforms_group_, 1, &uniforms);
        wgpuRenderPassEncoderDraw(draw, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(draw);
        wgpuRenderPassEncoderRelease(draw);
        wgpuBindGroupRelease(textures);
    }

    // The textures the meshes and glow draw into, at the frame's size, made again when it
    // changes.
    void prepare_post(std::uint32_t width, std::uint32_t height) {
        if (post_targets_.world.texture != nullptr && post_targets_.width == width &&
            post_targets_.height == height) {
            return;
        }
        release_post_targets();
        const auto made = [&](std::uint32_t w, std::uint32_t h) {
            return create_gpu_texture(
                {.width = w, .height = h, .filter = Filter::Linear, .render_target = true},
                TextureFormat::Rgba8);
        };
        post_targets_.world = made(width, height);
        const std::array<PostSize, kGlowLevels> sizes = glow_sizes(width, height);
        for (std::size_t i = 0; i < kGlowLevels; ++i) {
            post_targets_.glow.at(i) = made(sizes.at(i).width, sizes.at(i).height);
        }
        post_targets_.width = width;
        post_targets_.height = height;
    }

    void release_post_targets() {
        for (GpuTexture* texture :
             {&post_targets_.world, &post_targets_.glow[0], &post_targets_.glow[1],
              &post_targets_.glow[2], &post_targets_.glow[3]}) {
            if (texture->texture != nullptr) {
                pending_textures_.push_back(std::exchange(*texture, {}));
            }
        }
        post_targets_.width = 0;
        post_targets_.height = 0;
    }

    // The format the sky and meshes draw in, for a frame whose target has frame's: half floats for
    // Forward+, whose post passes draw them over the target; RGBA8 for a Compatibility scene the
    // post passes process; otherwise the target's own.
    static WGPUTextureFormat world_format(const Scene3D& scene, WGPUTextureFormat frame) {
        if (scene.method == RenderMethod::ForwardPlus) {
            return kHdrFormat;
        }
        return post_processes(scene.post) ? kTextureFormat : frame;
    }

    // The texture the sky and meshes draw into, and what it is cleared to: for Forward+ the
    // clear color made linear.
    [[nodiscard]] WGPUTextureView world_view(const Scene3D& scene) const {
        if (scene.capture) {
            return probe_raw_.faces.at(scene.capture->face);
        }
        if (scene.method == RenderMethod::ForwardPlus) {
            return uses_fsr2(scene) ? fsr2_textures_.color.view : forward_targets_.world.view;
        }
        return post_processes(scene.post) ? post_targets_.world.view : frame_view_;
    }
    [[nodiscard]] WGPUColor world_clear(const Scene3D& scene) const {
        if (scene.method == RenderMethod::ForwardPlus) {
            const Float4 clear = forward_clear(frame_.clear);
            return {.r = clear[0], .g = clear[1], .b = clear[2], .a = clear[3]};
        }
        return {.r = frame_.clear.r, .g = frame_.clear.g, .b = frame_.clear.b, .a = frame_.clear.a};
    }

    // The world's texture of half floats at the frame's size and, with glow, glow's seven levels,
    // with auto exposure the luminance textures, and with depth of field its textures.
    void prepare_forward(std::uint32_t width, std::uint32_t height, bool glow, bool auto_exposure,
                         bool dof) {
        const auto made = [&](std::uint32_t w, std::uint32_t h,
                              WGPUTextureFormat format = kHdrFormat,
                              WGPUTextureUsage copies = WGPUTextureUsage_None) {
            WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
            info.usage =
                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | copies;
            info.size = {.width = w, .height = h, .depthOrArrayLayers = 1};
            info.format = format;
            WGPUTexture texture = wgpuDeviceCreateTexture(device_, &info);
            return GpuTexture{.texture = texture,
                              .view = wgpuTextureCreateView(texture, nullptr),
                              .filter = Filter::Linear};
        };
        if (forward_targets_.world.texture == nullptr || forward_targets_.width != width ||
            forward_targets_.height != height) {
            release_forward_targets();
            // TAA copies the world to its history and back.
            forward_targets_.world = made(width, height, kHdrFormat,
                                          WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst);
            forward_targets_.width = width;
            forward_targets_.height = height;
        }
        if (glow && forward_targets_.glow[0].texture == nullptr) {
            const GlowPlan plan = forward_glow_plan(PostProcess{}, width, height);
            for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
                forward_targets_.glow.at(i) = made(plan.sizes.at(i).width, plan.sizes.at(i).height);
            }
        }
        if (auto_exposure && forward_targets_.brightness.texture == nullptr) {
            for (const PostSize size : luminance_plan(width, height)) {
                forward_targets_.luminance.push_back(
                    made(size.width, size.height, kLuminanceFormat));
            }
            forward_targets_.brightness = made(1, 1, kLuminanceFormat);
            forward_targets_.measured = false;
        }
        if (dof && forward_targets_.dof[0].texture == nullptr) {
            // Sized, Secondary, Half0, Half1 and Out, as DofTexture after World
            const std::uint32_t half_width = std::max(width >> 1U, 1U);
            const std::uint32_t half_height = std::max(height >> 1U, 1U);
            for (std::size_t i = 0; i < forward_targets_.dof.size(); ++i) {
                const bool half = i == 2 || i == 3;
                forward_targets_.dof.at(i) =
                    made(half ? half_width : width, half ? half_height : height);
            }
        }
    }

    void release_forward_targets() {
        const auto release = [this](GpuTexture& texture) {
            if (texture.texture != nullptr) {
                pending_textures_.push_back(std::exchange(texture, {}));
            }
        };
        release(forward_targets_.world);
        for (GpuTexture& texture : forward_targets_.glow) {
            release(texture);
        }
        for (GpuTexture& texture : forward_targets_.luminance) {
            release(texture);
        }
        forward_targets_.luminance.clear();
        release(forward_targets_.brightness);
        for (GpuTexture& texture : forward_targets_.dof) {
            release(texture);
        }
        forward_targets_.measured = false;
        forward_targets_.width = 0;
        forward_targets_.height = 0;
    }

    // Forward+'s post pipeline for a target format, made the first time a frame needs it.
    WGPURenderPipeline forward_pipeline_for(WGPUTextureFormat format) {
        for (const auto& [made, pipeline] : forward_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kForwardPostFragmentModule, shaders::kForwardPostFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kForwardPostLayout);
        info.vertex.module = module(kForwardPostVertexModule, shaders::kForwardPostVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        forward_pipelines_.emplace_back(format, pipeline);
        return pipeline;
    }

    // forward_post.hlsl's passes, as the GPU device draws them: depth of field's, whose result the
    // rest read for the world; with auto exposure the luminance passes, then glow's levels, each
    // from the one before, then the tonemap pass from the world's texture over the frame's
    // target. Their uniforms are added to the frame's.
    std::vector<PostDraw> forward_draws_for(const Scene3D& scene) {
        const PostProcess& post = scene.post;
        std::vector<PostDraw> draws;
        // The world's texture, or after depth of field its result.
        WGPUTextureView scene_world = forward_targets_.world.view;
        // A glow pass reads only its source; the tonemap pass reads glow's levels too. A
        // luminance pass after the first loads the level before from a slot of its own. Passes
        // read the measured brightness given them, or white.
        const auto add = [&](WGPUTextureView target, WGPUTextureFormat format,
                             const ForwardPostUniforms& uniforms, WGPUTextureView source,
                             bool levels, WGPUTextureView lut, WGPUTextureView measured = nullptr,
                             WGPUTextureView depth = nullptr, WGPUTextureView world = nullptr) {
            std::vector<WGPUTextureView> textures(kForwardPostTextures, white_.view);
            const bool from_level = format == kLuminanceFormat && source != scene_world;
            textures.front() = from_level ? scene_world : source;
            for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
                if (levels && forward_targets_.glow.at(i).view != nullptr) {
                    textures.at(1 + i) = forward_targets_.glow.at(i).view;
                }
            }
            textures.at(1 + kMaxGlowLevels) = lut;
            if (measured != nullptr) {
                textures.at(kBrightnessSlot) = measured;
            }
            if (from_level) {
                textures.at(kLuminanceSourceSlot) = source;
            }
            if (depth != nullptr) {
                textures.at(kDepthSlot) = depth;
                textures.at(kDofWorldSlot) = world;
            }
            draws.push_back({.target = target,
                             .pipeline = forward_pipeline_for(format),
                             .textures = std::move(textures),
                             .uniforms = uniforms_.add(uniforms)});
        };
        if (uses_dof(post)) {
            const auto texture = [&](DofTexture which) {
                return which == DofTexture::World
                           ? forward_targets_.world.view
                           : forward_targets_.dof.at(static_cast<std::size_t>(which) - 1).view;
            };
            // With FSR 2 the depth buffer is at its render size.
            const PostSize depth = uses_fsr2(scene)
                                       ? PostSize{.width = scene_width_, .height = scene_height_}
                                       : PostSize{};
            for (const DofStep& step : dof_plan(post, frame_width_, frame_height_)) {
                add(texture(step.target), kHdrFormat,
                    dof_uniforms(post, step, scene.projection, depth), texture(step.source), false,
                    white_.view, nullptr, depth_.view, forward_targets_.world.view);
            }
            scene_world = texture(DofTexture::Out);
        }
        WGPUTextureView brightness = nullptr;
        if (post.auto_exposure) {
            // As Godot's Luminance::luminance_reduction: the last pass moves from the last
            // frame's brightness, then the brightness texture and the last level swap.
            const bool set = !forward_targets_.measured ||
                             forward_targets_.version != post.auto_exposure_version;
            std::vector<GpuTexture>& levels = forward_targets_.luminance;
            const std::vector<PostSize> plan = luminance_plan(frame_width_, frame_height_);
            PostSize source{.width = frame_width_, .height = frame_height_};
            for (std::size_t i = 0; i < levels.size(); ++i) {
                ForwardPass kind = i == 0 ? ForwardPass::LuminanceFirst : ForwardPass::Luminance;
                if (i > 0 && i + 1 == levels.size() && !set) {
                    kind = ForwardPass::LuminanceFinal;
                }
                add(levels.at(i).view, kLuminanceFormat,
                    luminance_uniforms(post, kind, source, plan.at(i)),
                    i == 0 ? scene_world : levels.at(i - 1).view, false, white_.view,
                    forward_targets_.brightness.view);
                source = plan.at(i);
            }
            std::swap(forward_targets_.brightness, levels.back());
            forward_targets_.measured = true;
            forward_targets_.version = post.auto_exposure_version;
            brightness = forward_targets_.brightness.view;
        }
        if (post.glow) {
            const GlowPlan plan = forward_glow_plan(post, frame_width_, frame_height_);
            for (std::size_t i = 0; i < plan.count; ++i) {
                const PostSize size = plan.sizes.at(i);
                add(forward_targets_.glow.at(i).view, kHdrFormat,
                    forward_post_uniforms(post,
                                          i == 0 ? ForwardPass::GlowFirst : ForwardPass::GlowNext,
                                          size.width, size.height, frame_width_, frame_height_),
                    i == 0 ? scene_world : forward_targets_.glow.at(i - 1).view, false, white_.view,
                    brightness);
            }
        }
        WGPUTextureView lut = white_.view;
        TextureDesc lut_desc{};
        if (post.adjustments && post.color_correction != TextureId::None) {
            const Texture& found = textures_.at(post.color_correction);
            lut = found.gpu.view;
            lut_desc = found.desc;
        }
        add(frame_view_, frame_format_,
            forward_post_uniforms(post, ForwardPass::Tonemap, frame_width_, frame_height_,
                                  frame_width_, frame_height_, lut_desc.width, lut_desc.height),
            scene_world, post.glow, lut, brightness);
        return draws;
    }

    // The sky's pipeline for a target format, made the first time a frame needs it: one triangle
    // over the target that neither tests nor writes depth.
    WGPURenderPipeline sky_pipeline_for(WGPUTextureFormat format) {
        for (const auto& [made, pipeline] : sky_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kSkyFragmentModule, shaders::kSkyFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = WGPUOptionalBool_False;
        depth.depthCompare = WGPUCompareFunction_Always;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kSkyLayout);
        info.vertex.module = module(kSkyVertexModule, shaders::kSkyVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.depthStencil = &depth;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        sky_pipelines_.emplace_back(format, pipeline);
        return pipeline;
    }

    // The post pipeline for a target format, made the first time a frame needs it.
    WGPURenderPipeline post_pipeline_for(WGPUTextureFormat format) {
        for (const auto& [made, pipeline] : post_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = module(kPostFragmentModule, shaders::kPostFragmentWgsl);
        fragment.entryPoint = text_view("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &color;
        WGPURenderPipelineDescriptor info = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        info.layout = pipeline_layouts_.at(kPostLayout);
        info.vertex.module = module(kPostVertexModule, shaders::kPostVertexWgsl);
        info.vertex.entryPoint = text_view("vs_main");
        info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        info.primitive.cullMode = WGPUCullMode_None;
        info.fragment = &fragment;
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &info);
        post_pipelines_.emplace_back(format, pipeline);
        return pipeline;
    }

    // post.hlsl's passes, as the GPU device draws them: glow's filter, downsampling and
    // upsampling through its four textures, then the post pass from the meshes' texture over the
    // frame's target. Their uniforms are added to the frame's.
    std::vector<PostDraw> post_draws_for(const PostProcess& post) {
        std::vector<PostDraw> draws;
        const auto add = [&](WGPUTextureView target, WGPUTextureFormat format,
                             const PostUniforms& uniforms,
                             std::array<WGPUTextureView, kPostTextures> textures) {
            draws.push_back({.target = target,
                             .pipeline = post_pipeline_for(format),
                             .textures = {textures.begin(), textures.end()},
                             .uniforms = uniforms_.add(uniforms)});
        };
        const std::array<PostSize, kGlowLevels> sizes = glow_sizes(frame_width_, frame_height_);
        if (post.glow) {
            for (std::size_t i = 0; i < kGlowLevels; ++i) {
                add(post_targets_.glow.at(i).view, kTextureFormat,
                    post_uniforms(post, i == 0 ? PostPass::GlowFilter : PostPass::GlowDownsample,
                                  sizes.at(i).width, sizes.at(i).height),
                    {i == 0 ? post_targets_.world.view : post_targets_.glow.at(i - 1).view,
                     white_.view, white_.view});
            }
            for (std::size_t i = kGlowLevels - 1; i-- > 0;) {
                add(post_targets_.glow.at(i).view, kTextureFormat,
                    post_uniforms(post, PostPass::GlowUpsample, sizes.at(i).width,
                                  sizes.at(i).height),
                    {post_targets_.glow.at(i + 1).view, white_.view, white_.view});
            }
        }
        WGPUTextureView lut = white_.view;
        TextureDesc lut_desc{};
        if (post.adjustments && post.color_correction != TextureId::None) {
            const Texture& found = textures_.at(post.color_correction);
            lut = found.gpu.view;
            lut_desc = found.desc;
        }
        add(frame_view_, frame_format_,
            post_uniforms(post, PostPass::Post, frame_width_, frame_height_, lut_desc.width,
                          lut_desc.height),
            {post_targets_.world.view, post.glow ? post_targets_.glow[0].view : white_.view, lut});
        return draws;
    }

    // Records the post passes, each over its whole target. A frame with no sky or mesh draws
    // clears the world's texture first.
    void encode_post(WGPUCommandEncoder encoder, std::span<const PostDraw> draws) {
        const auto begin = [&](WGPUTextureView target, WGPULoadOp load) {
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = target;
            color.loadOp = load;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = world_clear(*frame_.scene);
            WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            descriptor.colorAttachmentCount = 1;
            descriptor.colorAttachments = &color;
            return wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        };
        if (mesh_draws_.empty() && !sky_draws(*frame_.scene)) {
            WGPURenderPassEncoder pass = begin(world_view(*frame_.scene), WGPULoadOp_Clear);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
        }
        WGPUSampler sampler = samplers_.at(sampler_index(Filter::Linear, false));
        for (const PostDraw& draw : draws) {
            const bool forward = draw.textures.size() == kForwardPostTextures;
            const auto count = static_cast<std::uint32_t>(draw.textures.size());
            const std::uint32_t samplers = forward ? kMeshSamplerBinding : kSamplerBinding;
            std::vector<WGPUBindGroupEntry> entries(std::size_t{count} * 2);
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                WGPUBindGroupEntry& texture = entries.at(slot);
                texture = WGPU_BIND_GROUP_ENTRY_INIT;
                texture.binding = slot;
                texture.textureView = draw.textures.at(slot);
                WGPUBindGroupEntry& bound = entries.at(count + slot);
                bound = WGPU_BIND_GROUP_ENTRY_INIT;
                bound.binding = samplers + slot;
                bound.sampler = sampler;
            }
            WGPUBindGroup textures =
                bind_group(forward ? forward_textures_layout_ : post_textures_layout_, entries);
            WGPURenderPassEncoder pass = begin(draw.target, WGPULoadOp_Clear);
            wgpuRenderPassEncoderSetPipeline(pass, draw.pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 2, textures, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(
                pass, 3, forward ? forward_uniforms_group_ : post_uniforms_group_, 1,
                &draw.uniforms);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            wgpuBindGroupRelease(textures);
        }
    }

    GpuTexture create_gpu_texture(const TextureDesc& desc, TextureFormat stored) {
        WGPUTextureDescriptor info = WGPU_TEXTURE_DESCRIPTOR_INIT;
        info.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        if (desc.render_target) {
            info.usage |= WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        }
        info.size = {.width = desc.width, .height = desc.height, .depthOrArrayLayers = 1};
        info.format = gpu_format(stored);
        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &info);
        return {.texture = texture,
                .view = wgpuTextureCreateView(texture, nullptr),
                .filter = desc.filter};
    }

    static void destroy_gpu_texture(const GpuTexture& gpu) {
        if (gpu.texture == nullptr) {
            return;
        }
        wgpuTextureViewRelease(gpu.view);
        wgpuTextureDestroy(gpu.texture);
        wgpuTextureRelease(gpu.texture);
    }

    // Uploads texels given in desc's format to a texture stored in stored's, decoding them
    // when the two differ.
    Result<void> upload_texels(WGPUTexture texture, const TextureDesc& desc, TextureFormat stored,
                               std::span<const std::uint8_t> texels) {
        if (stored == desc.format) {
            write_texels(texture, desc, texels);
            return {};
        }
        auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
        if (!pixels) {
            return pixels.error();
        }
        TextureDesc decoded = desc;
        decoded.format = stored;
        write_texels(texture, decoded, pixels.value());
        return {};
    }

    // Replaces every texel, given in desc's format.
    void write_texels(WGPUTexture texture, const TextureDesc& desc,
                      std::span<const std::uint8_t> texels) {
        WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        destination.texture = texture;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        // A compressed row is a row of 4x4 blocks, 16 bytes each.
        layout.bytesPerRow = is_compressed(desc.format)
                                 ? desc.width / 4 * 16
                                 : static_cast<std::uint32_t>(texture_bytes(desc) / desc.height);
        layout.rowsPerImage = is_compressed(desc.format) ? desc.height / 4 : desc.height;
        const WGPUExtent3D extent{
            .width = desc.width, .height = desc.height, .depthOrArrayLayers = 1};
        wgpuQueueWriteTexture(queue_, &destination, texels.data(), texels.size(), &layout, &extent);
    }

    // A batch's textures and samplers, bound as a group made the first time it is needed.
    WGPUBindGroup texture_group(const Batch& batch) {
        const std::size_t count = batch.kind == ShaderKind::Material ? kMaterialSamplers : 1;
        TextureBindings key{};
        std::copy_n(batch.bindings.begin(), count, key.begin());
        if (const auto it = texture_groups_.find(key); it != texture_groups_.end()) {
            return it->second;
        }
        std::vector<WGPUBindGroupEntry> entries;
        entries.reserve(count * 2);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            WGPUBindGroupEntry texture = WGPU_BIND_GROUP_ENTRY_INIT;
            texture.binding = slot;
            texture.textureView = key.at(slot).first;
            entries.push_back(texture);
            WGPUBindGroupEntry sampler = WGPU_BIND_GROUP_ENTRY_INIT;
            sampler.binding = kSamplerBinding + slot;
            sampler.sampler = key.at(slot).second;
            entries.push_back(sampler);
        }
        WGPUBindGroup group =
            bind_group(batch.kind == ShaderKind::Material ? material_textures_layout_
                                                          : sprite_textures_layout_,
                       entries);
        texture_groups_.emplace(key, group);
        return group;
    }

    // Drops the groups that bind a texture that is going.
    void forget_texture_groups(WGPUTextureView view) {
        std::erase_if(texture_groups_, [&](const auto& entry) {
            const bool binds = std::ranges::any_of(
                entry.first, [&](const auto& bound) { return bound.first == view; });
            if (binds) {
                wgpuBindGroupRelease(entry.second);
            }
            return binds;
        });
    }

    void clear_texture_groups() {
        for (const auto& [key, group] : texture_groups_) {
            wgpuBindGroupRelease(group);
        }
        texture_groups_.clear();
    }

    // Copies the frame's vertices, what its particle jobs read and its uniforms to the GPU, with
    // room after the vertices for the particles' quads.
    void upload_frame() {
        const std::uint64_t total = frame_vertex_count() * sizeof(Vertex2D);
        if (total > vertex_capacity_) {
            // Grows by doubling, so a busy game settles on one size.
            destroy_buffer(vertex_buffer_);
            vertex_capacity_ = std::max({total, vertex_capacity_ * 2, kMinVertexBytes});
            vertex_buffer_ = create_buffer(WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage |
                                               WGPUBufferUsage_CopyDst,
                                           vertex_capacity_);
        }
        if (!vertices_.empty()) {
            wgpuQueueWriteBuffer(queue_, vertex_buffer_, 0, vertices_.data(),
                                 vertices_.size() * sizeof(Vertex2D));
        }
        upload_particle_words();
        upload_uniforms();
    }

    void start_frame(const Frame& frame, WGPUTextureView view, std::uint32_t width,
                     std::uint32_t height, bool drawn) {
        in_frame_ = true;
        frame_ = frame;
        frame_view_ = view;
        frame_width_ = width;
        frame_height_ = height;
        const auto [scene_width, scene_height] = scene_size(frame, width, height);
        scene_width_ = scene_width;
        scene_height_ = scene_height;
        frame_drawn_ = drawn;
        vertices_.clear();
        batches_.clear();
        mesh_draws_.clear();
        mesh_batches_.clear();
        shadow_plan_.reset();
        radiance_prepared_ = false;
        radiance_draws_ = false;
        particle_vertices_ = 0;
    }

    void finish_frame() {
        // A frame with TAA or FSR 2 saw its meshes' vertices as they are now.
        if (frame_.scene && uses_motion_vectors(*frame_.scene)) {
            for (const MeshDraw& draw : mesh_draws_) {
                if (const auto found = meshes_.find(draw.mesh); found != meshes_.end()) {
                    found->second.moved = false;
                }
            }
        }
        if (frame_texture_ != nullptr) {
            wgpuTextureViewRelease(frame_view_);
            wgpuTextureRelease(frame_texture_);
        }
        frame_texture_ = nullptr;
        frame_view_ = nullptr;
        frame_drawn_ = false;
        vertices_.clear();
        batches_.clear();
        mesh_draws_.clear();
        mesh_batches_.clear();
        shadow_plan_.reset();
        radiance_prepared_ = false;
        radiance_draws_ = false;
        particle_vertices_ = 0;
        clear_particle_jobs();
        release_pending();
    }

    void release_pending() {
        for (const GpuTexture& texture : pending_textures_) {
            destroy_gpu_texture(texture);
        }
        pending_textures_.clear();
        for (const CubeTexture& cube : pending_cubes_) {
            destroy_cube(cube);
        }
        pending_cubes_.clear();
        for (WGPURenderPipeline pipeline : pending_pipelines_) {
            wgpuRenderPipelineRelease(pipeline);
        }
        pending_pipelines_.clear();
        release_pending_buffers();
    }

    // Shader modules and pipeline layouts, by these indexes.
    static constexpr std::size_t kSpriteVertexModule = 0;
    static constexpr std::size_t kSpriteFragmentModule = 1;
    static constexpr std::size_t kMaterialVertexModule = 2;
    static constexpr std::size_t kMaterialFragmentModule = 3;
    static constexpr std::size_t kProcessModule = 4;
    static constexpr std::size_t kDrawModule = 5;
    static constexpr std::size_t kEmitModule = 6;
    static constexpr std::size_t kMeshVertexModule = 7;
    static constexpr std::size_t kMeshFragmentModule = 8;
    static constexpr std::size_t kShadowVertexModule = 9;
    static constexpr std::size_t kShadowFragmentModule = 10;
    static constexpr std::size_t kPostVertexModule = 11;
    static constexpr std::size_t kPostFragmentModule = 12;
    static constexpr std::size_t kSkyVertexModule = 13;
    static constexpr std::size_t kSkyFragmentModule = 14;
    static constexpr std::size_t kRadianceVertexModule = 15;
    static constexpr std::size_t kRadianceFragmentModule = 16;
    static constexpr std::size_t kMeshClusteredFragmentModule = 17;
    static constexpr std::size_t kForwardPostVertexModule = 18;
    static constexpr std::size_t kForwardPostFragmentModule = 19;
    static constexpr std::size_t kMeshNormalFragmentModule = 20;
    static constexpr std::size_t kSsaoVertexModule = 21;
    static constexpr std::size_t kSsaoFragmentModule = 22;
    static constexpr std::size_t kSsrVertexModule = 23;
    static constexpr std::size_t kSsrFragmentModule = 24;
    static constexpr std::size_t kSsrTraceFragmentModule = 25;
    static constexpr std::size_t kMeshMotionVertexModule = 26;
    static constexpr std::size_t kMeshMotionFragmentModule = 27;
    static constexpr std::size_t kTaaVertexModule = 28;
    static constexpr std::size_t kTaaFragmentModule = 29;
    // FSR 2's passes' vertex and fragment modules in Fsr2Pass's order, then its reconstruct pass's.
    static constexpr std::size_t kFsr2Modules = 30;
    static constexpr std::size_t kFsr2ReconstructModule = kFsr2Modules + (2 * kFsr2Passes);
    static constexpr std::size_t kSpriteLayout = 0;
    static constexpr std::size_t kMaterialLayout = 1;
    static constexpr std::size_t kProcessLayout = 2;
    static constexpr std::size_t kDrawLayout = 3;
    static constexpr std::size_t kEmitLayout = 4;
    static constexpr std::size_t kMeshLayout = 5;
    static constexpr std::size_t kShadowLayout = 6;
    static constexpr std::size_t kPostLayout = 7;
    static constexpr std::size_t kSkyLayout = 8;
    static constexpr std::size_t kRadianceLayout = 9;
    static constexpr std::size_t kMeshClusteredLayout = 10;
    static constexpr std::size_t kForwardPostLayout = 11;
    static constexpr std::size_t kSsaoLayout = 12;
    static constexpr std::size_t kSsrLayout = 13;
    static constexpr std::size_t kTaaLayout = 14;
    static constexpr std::size_t kFsr2Layout = 15; // one for each Fsr2Pass
    static constexpr std::size_t kFsr2ReconstructLayout = kFsr2Layout + kFsr2Passes;

    WGPUInstance instance_;
    WGPUDevice device_;
    WGPUQueue queue_;
    SDL_Window* window_;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;
    std::uint32_t surface_width_ = 0;
    std::uint32_t surface_height_ = 0;
    bool can_wait_ = false; // Asyncify lets read_texture wait for the GPU
    // Which formats the GPU samples, by TextureFormat.
    std::array<bool, 4> sampled_{true, false, false, true};
    std::array<WGPUShaderModule, kFsr2ReconstructModule + 1> modules_{};
    std::vector<Pipeline> pipelines_;
    std::unordered_map<ShaderId, GameShader> game_shaders_;
    std::uint32_t next_shader_ = 1;
    std::array<WGPUComputePipeline, 3> compute_pipelines_{}; // by ComputeKind
    WGPUBindGroupLayout empty_layout_ = nullptr;
    WGPUBindGroupLayout vertex_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout sprite_textures_layout_ = nullptr;
    WGPUBindGroupLayout material_textures_layout_ = nullptr;
    WGPUBindGroupLayout fragment_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout process_inputs_layout_ = nullptr;
    WGPUBindGroupLayout process_outputs_layout_ = nullptr;
    WGPUBindGroupLayout draw_inputs_layout_ = nullptr;
    WGPUBindGroupLayout draw_outputs_layout_ = nullptr;
    WGPUBindGroupLayout emit_outputs_layout_ = nullptr;
    WGPUBindGroupLayout compute_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout mesh_vertex_layout_ = nullptr;
    WGPUBindGroupLayout mesh_textures_layout_ = nullptr;
    WGPUBindGroupLayout mesh_fragment_layout_ = nullptr;
    WGPUBindGroupLayout mesh_clusters_layout_ = nullptr;
    WGPUBindGroupLayout shadow_vertex_layout_ = nullptr;
    WGPUBindGroupLayout shadow_fragment_layout_ = nullptr;
    WGPUBindGroupLayout post_textures_layout_ = nullptr;
    WGPUBindGroupLayout post_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout forward_textures_layout_ = nullptr;
    WGPUBindGroupLayout forward_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout sky_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout cube_texture_layout_ = nullptr;
    WGPUBindGroupLayout filter_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout ssao_textures_layout_ = nullptr;
    WGPUBindGroupLayout ssao_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout ssr_textures_layout_ = nullptr;
    WGPUBindGroupLayout ssr_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout taa_textures_layout_ = nullptr;
    WGPUBindGroupLayout taa_uniforms_layout_ = nullptr;
    std::array<WGPUBindGroupLayout, kFsr2Passes> fsr2_textures_layouts_{};
    WGPUBindGroupLayout fsr2_uniforms_layout_ = nullptr;
    WGPUBindGroupLayout fsr2_storage_layout_ = nullptr;
    WGPUBindGroupLayout fsr2_compute_textures_layout_ = nullptr;
    WGPUBindGroupLayout fsr2_compute_storage_layout_ = nullptr;
    WGPUBindGroupLayout fsr2_compute_uniforms_layout_ = nullptr;
    std::array<WGPUPipelineLayout, kFsr2ReconstructLayout + 1> pipeline_layouts_{};
    std::array<WGPUSampler, 4> samplers_{}; // by sampler_index
    GpuTexture white_;
    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    std::vector<GpuTexture> pending_textures_;
    std::vector<WGPURenderPipeline> pending_pipelines_;
    std::unordered_map<TextureBindings, WGPUBindGroup, BindingsHash> texture_groups_;
    WGPUBuffer vertex_buffer_ = nullptr;
    std::uint64_t vertex_capacity_ = 0;
    // The uniforms of what is being sent, and the buffer and groups they go through.
    UniformBlocks uniforms_;
    WGPUBuffer uniform_buffer_ = nullptr;
    std::uint64_t uniform_capacity_ = 0;
    WGPUBindGroup vertex_uniforms_group_ = nullptr;
    WGPUBindGroup fragment_uniforms_group_ = nullptr;
    WGPUBindGroup compute_uniforms_group_ = nullptr;
    WGPUBindGroup mesh_vertex_group_ = nullptr;
    WGPUBindGroup mesh_fragment_group_ = nullptr;
    WGPUBindGroup shadow_vertex_group_ = nullptr;
    WGPUBindGroup shadow_fragment_group_ = nullptr;
    WGPUBindGroup post_uniforms_group_ = nullptr;
    WGPUBindGroup forward_uniforms_group_ = nullptr;
    WGPUBindGroup sky_uniforms_group_ = nullptr;
    WGPUBindGroup filter_uniforms_group_ = nullptr;
    WGPUBindGroup ssao_uniforms_group_ = nullptr;
    WGPUBindGroup ssr_uniforms_group_ = nullptr;
    WGPUBindGroup taa_uniforms_group_ = nullptr;
    WGPUBindGroup fsr2_uniforms_group_ = nullptr;
    WGPUBindGroup fsr2_compute_uniforms_group_ = nullptr;
    std::unordered_map<ParticlesId, ParticleSystem> particle_systems_;
    std::uint32_t next_particles_ = 1;
    std::vector<WGPUBuffer> pending_buffers_;
    // Jobs waiting to run, and the words they read.
    std::vector<ParticleJob> particle_jobs_;
    std::vector<Word> particle_words_;
    std::optional<std::pair<ParticleProcess, std::uint32_t>> last_material_;
    WGPUBuffer particle_data_ = nullptr;
    std::uint64_t particle_capacity_ = 0;
    // Bound where a process pass takes or writes no emissions.
    WGPUBuffer no_source_ = nullptr;
    WGPUBuffer no_target_ = nullptr;
    std::unordered_map<MeshId, Mesh> meshes_;
    std::uint32_t next_mesh_ = 1;
    std::vector<MeshPipeline> mesh_pipelines_;
    // The frame's Forward+ clusters, the buffers they are copied to and the group binding them.
    LightClusters clusters_;
    StorageBuffer cluster_lights_;
    StorageBuffer cluster_cells_;
    WGPUBindGroup cluster_group_ = nullptr;
    GpuTexture depth_;
    std::uint32_t depth_width_ = 0;
    std::uint32_t depth_height_ = 0;
    // The frame's shadow plan, made at its first mesh draw, and what its passes draw with.
    std::optional<ShadowPlan> shadow_plan_;
    std::array<ShadowAtlasTexture, 2> shadow_atlases_{}; // directional, then positional
    GpuTexture no_shadows_; // bound for an atlas the frame doesn't draw
    WGPUSampler shadow_sampler_ = nullptr;
    std::array<WGPURenderPipeline, 3> shadow_pipelines_{}; // by CullMode
    std::vector<std::pair<WGPUTextureFormat, WGPURenderPipeline>> post_pipelines_;
    std::vector<std::pair<WGPUTextureFormat, WGPURenderPipeline>> sky_pipelines_;
    PostTargets post_targets_;
    std::vector<std::pair<WGPUTextureFormat, WGPURenderPipeline>> forward_pipelines_;
    ForwardTargets forward_targets_;
    // The normal-roughness prepass's normals and depth at the frame's size, which SSAO and SSR
    // read.
    struct PrepassTargets {
        GpuTexture normals;
        GpuTexture depth;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    // SSAO's textures for the frame's size and settings: those ssao_plan sizes (none for those it
    // doesn't draw).
    struct SsaoTargets {
        std::array<GpuTexture, kSsaoTextures> textures{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SsaoSettings settings;
    };
    // SSR's: those ssr_plan sizes, and Hi-Z's and the reflections' levels copied into a texture
    // with mipmaps each.
    struct SsrTargets {
        std::array<GpuTexture, kSsrTextures> textures{};
        GpuTexture hiz_mips;
        GpuTexture color_mips;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SsrSettings settings;
    };
    std::array<WGPURenderPipeline, 3> normal_pipelines_{}; // by CullMode
    PrepassTargets prepass_;
    std::vector<std::pair<WGPUTextureFormat, WGPURenderPipeline>> ssao_pipelines_;
    SsaoTargets ssao_targets_;
    std::vector<std::pair<WGPUTextureFormat, WGPURenderPipeline>> ssr_pipelines_;
    WGPURenderPipeline ssr_trace_pipeline_ = nullptr;
    SsrTargets ssr_targets_;
    // Each target's frame before, by the target (TextureId::None for the window).
    std::vector<std::pair<TextureId, SsrHistory>> ssr_histories_;
    std::array<WGPURenderPipeline, 3> motion_pipelines_{}; // TAA's prepass's, by CullMode
    WGPURenderPipeline taa_pipeline_ = nullptr;
    // Each target's frames before for TAA, by the target.
    std::vector<std::pair<TextureId, TaaHistory>> taa_histories_;
    // FSR 2's pipelines by Fsr2Pass, its reconstruct pass's, the textures a frame's passes draw
    // into (the world and motion vectors at the render size, which the sky and meshes draw, and
    // the passes' own), the reconstructed depths, the sizes they were made for (the render size,
    // then the frame's), and each target's frames before, by the target.
    struct Fsr2Textures {
        GpuTexture color;
        GpuTexture velocity;
        GpuTexture luma_mip;
        GpuTexture dilated_depth;
        GpuTexture lock_luma;
        GpuTexture prepared;
        GpuTexture reactive;
        GpuTexture new_locks;
        WGPUBuffer reconstructed = nullptr;
        std::uint64_t reconstructed_bytes = 0;
        std::array<std::uint32_t, 4> sizes{};
    };
    std::array<WGPURenderPipeline, kFsr2Passes> fsr2_pipelines_{};
    WGPUComputePipeline fsr2_reconstruct_ = nullptr;
    Fsr2Textures fsr2_textures_;
    std::vector<std::pair<TextureId, Fsr2Frames>> fsr2_frames_;
    // The radiance meshes are lit by: the raw cube the sky draws into and the one filtered from
    // it, and what they hold (nullopt to draw them again).
    struct RadianceCubes {
        CubeTexture raw;
        CubeTexture filtered;
        std::uint32_t size = 0;
        std::optional<RadianceKey> key;
    };
    RadianceCubes radiance_;
    std::vector<CubeTexture> pending_cubes_;
    std::array<WGPURenderPipeline, 2> radiance_pipelines_{}; // the sky's, then the filter's
    WGPUSampler radiance_sampler_ = nullptr;
    CubeTexture no_radiance_; // bound when the frame's meshes have none
    // Reflection probes: each atlas's cubes by its texture, the raw cube captures draw into and
    // its size, the filter's pipeline into half floats, and what meshes bind with no probes.
    std::unordered_map<TextureId, CubeTexture> atlases_;
    CubeTexture probe_raw_;
    std::uint32_t probe_raw_size_ = 0;
    WGPURenderPipeline probe_pipeline_ = nullptr;
    CubeTexture no_probes_;
    StorageBuffer probe_buffer_; // the frame's probes, as the mesh pass reads them
    // SSAO's and SSR's last passes draw into the screen array, which the mesh pass reads;
    // no_screen_ is bound in frames with neither.
    ScreenArray screen_;
    ScreenArray no_screen_;
    bool radiance_prepared_ = false; // at the frame's first mesh draw
    bool radiance_draws_ = false;    // whether end_frame draws the radiance again

    // The frame being built. A skipped frame draws nothing.
    bool in_frame_ = false;
    bool frame_drawn_ = false;
    Frame frame_;
    WGPUTexture frame_texture_ = nullptr; // the canvas's, when drawing to the window
    WGPUTextureView frame_view_ = nullptr;
    WGPUTextureFormat frame_format_ = kTextureFormat;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    // The size the sky and meshes draw at: FSR 2's render size, or the frame's.
    std::uint32_t scene_width_ = 0;
    std::uint32_t scene_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    std::uint32_t particle_vertices_ = 0; // the particles' quads, after vertices_
    // The frame's mesh draws, and what each binds.
    std::vector<MeshDraw> mesh_draws_;
    std::vector<MeshBatch> mesh_batches_;
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

} // namespace

bool webgpu_available() {
    return slash_webgpu_ready() != 0;
}

Result<std::unique_ptr<RenderDevice>> create_webgpu_device(const DeviceOptions& options) {
    if (!webgpu_available()) {
        return make_error(ErrorCode::PlatformFailure,
                          "WebGPU doesn't work here: " + take_string(slash_webgpu_unavailable()));
    }
    // Waiting for the GPU, which only reading back needs, takes Asyncify.
    const WGPUInstanceFeatureName wait = WGPUInstanceFeatureName_TimedWaitAny;
    WGPUInstanceDescriptor info = WGPU_INSTANCE_DESCRIPTOR_INIT;
    if (wgpuHasInstanceFeature(wait) != 0) {
        info.requiredFeatureCount = 1;
        info.requiredFeatures = &wait;
    }
    WGPUInstance instance = wgpuCreateInstance(&info);
    WGPUDevice device = slash_webgpu_import_device(instance);
    auto gpu = std::make_unique<WebGpuDevice>(instance, device, options.window);
    if (auto ready = gpu->init(); !ready) {
        return ready.error();
    }
    log::info("render device created",
              {{"backend", backend_name(gpu->backend())}, {"window", options.window != nullptr}});
    return std::unique_ptr<RenderDevice>(std::move(gpu));
}

} // namespace slash::render::detail

#endif // __EMSCRIPTEN__
