// Only web builds compile it (engine/render/CMakeLists.txt); the guard keeps tools that read every
// source, such as clang-tidy, from compiling it elsewhere.
#ifdef __EMSCRIPTEN__

#include "webgpu_device.hpp"

#include "checks.hpp"
#include "material_shaders.hpp"
#include "particle_layout.hpp"
#include "particles_draw_shaders.hpp"
#include "particles_emit_shaders.hpp"
#include "particles_process_shaders.hpp"
#include "sprite_shaders.hpp"
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

constexpr WGPUTextureFormat gpu_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::Bc7:
        return WGPUTextureFormat_BC7RGBAUnorm;
    case TextureFormat::Astc4x4:
        return WGPUTextureFormat_ASTC4x4Unorm;
    case TextureFormat::Rgba8:
        break;
    }
    return kTextureFormat;
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
constexpr std::uint64_t kLargestUniforms =
    std::max<std::uint64_t>({sizeof(Mat4), sizeof(VertexProgramUniforms), sizeof(ProgramUniforms),
                             sizeof(LightsUniforms), kComputeUniformBytes});
// The bind groups are SDL's GPU API's, which the HLSL's registers name and the WGSL keeps
// (cmake/compile_shaders.cmake): a vertex stage's uniforms in group 1, a fragment stage's
// textures in group 2 (samplers kSamplerBinding after their textures) and its uniforms in group
// 3; a compute shader's read-only buffers in group 0, read-write ones in 1 and uniforms in 2.
constexpr std::uint32_t kSamplerBinding = 8;

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };
// The compute shaders: running particle steps, turning particles into quads, and numbering what a
// sub-emitter takes.
enum class ComputeKind : std::uint8_t { Process, Draw, Emit };

WGPUStringView text_view(std::string_view text) {
    return {.data = text.data(), .length = text.size()};
}

// A compute shader's WGSL, which is in pieces since no string literal may be as long as it.
std::string joined(std::span<const char* const> pieces) {
    std::string text;
    for (const char* piece : pieces) {
        text += piece;
    }
    return text;
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

WGPUBindGroupLayoutEntry storage_entry(std::uint32_t binding, bool read_only) {
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Compute;
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
             {no_source_, no_target_, particle_data_, vertex_buffer_, uniform_buffer_}) {
            destroy_buffer(buffer);
        }
        release_uniform_groups();
        for (WGPUBindGroupLayout layout :
             {empty_layout_, vertex_uniforms_layout_, sprite_textures_layout_,
              material_textures_layout_, fragment_uniforms_layout_, process_inputs_layout_,
              process_outputs_layout_, draw_inputs_layout_, draw_outputs_layout_,
              emit_outputs_layout_, compute_uniforms_layout_}) {
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
        // Queue writes run ahead of the command buffers sent after them, so every draw of the
        // frame being built sees the new texels.
        return upload_texels(it->second.gpu.texture, desc, it->second.stored, texels);
    }

    void destroy_texture(TextureId texture) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
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

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        if (auto errors = reported_errors(); !errors) {
            return errors.error();
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
                frame.lighting, frame.target, [&](TextureId id) { return textures_.contains(id); },
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
        upload_frame();

        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        // Particles run and write their quads ahead of the render pass that draws them.
        encode_particle_jobs(encoder, job_uniforms);
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = frame_view_;
        color.loadOp = WGPULoadOp_Clear;
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
    using TextureBindings = std::array<std::pair<WGPUTextureView, WGPUSampler>, kMaterialSamplers>;
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
                material, frame_.target, [&](TextureId id) { return textures_.contains(id); },
                [&](ShaderId id) { return game_shaders_.contains(id); });
            !checked) {
            return checked;
        }
        return check_lighting_textures(frame_.lighting,
                                       [&](TextureId id) { return textures_.contains(id); });
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
    }

    void release_uniform_groups() {
        for (WGPUBindGroup* group :
             {&vertex_uniforms_group_, &fragment_uniforms_group_, &compute_uniforms_group_}) {
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
        layout.bytesPerRow = is_compressed(desc.format) ? desc.width / 4 * 16 : desc.width * 4;
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
        frame_drawn_ = drawn;
        vertices_.clear();
        batches_.clear();
        particle_vertices_ = 0;
    }

    void finish_frame() {
        if (frame_texture_ != nullptr) {
            wgpuTextureViewRelease(frame_view_);
            wgpuTextureRelease(frame_texture_);
        }
        frame_texture_ = nullptr;
        frame_view_ = nullptr;
        frame_drawn_ = false;
        vertices_.clear();
        batches_.clear();
        particle_vertices_ = 0;
        clear_particle_jobs();
        release_pending();
    }

    void release_pending() {
        for (const GpuTexture& texture : pending_textures_) {
            destroy_gpu_texture(texture);
        }
        pending_textures_.clear();
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
    static constexpr std::size_t kSpriteLayout = 0;
    static constexpr std::size_t kMaterialLayout = 1;
    static constexpr std::size_t kProcessLayout = 2;
    static constexpr std::size_t kDrawLayout = 3;
    static constexpr std::size_t kEmitLayout = 4;

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
    std::array<bool, 3> sampled_{true, false, false};
    std::array<WGPUShaderModule, 7> modules_{};
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
    std::array<WGPUPipelineLayout, 5> pipeline_layouts_{};
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

    // The frame being built. A skipped frame draws nothing.
    bool in_frame_ = false;
    bool frame_drawn_ = false;
    Frame frame_;
    WGPUTexture frame_texture_ = nullptr; // the canvas's, when drawing to the window
    WGPUTextureView frame_view_ = nullptr;
    WGPUTextureFormat frame_format_ = kTextureFormat;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    std::uint32_t particle_vertices_ = 0; // the particles' quads, after vertices_
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
