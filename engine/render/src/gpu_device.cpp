#include "gpu_device.hpp"

#include "checks.hpp"
#include "hlsl_compiler.hpp"
#include "material_shaders.hpp"
#include "particle_layout.hpp"
#include "particles_draw_shaders.hpp"
#include "particles_process_shaders.hpp"
#include "sprite_shaders.hpp"
#include "uniforms.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render::detail {
namespace {

// Textures are 8-bit RGBA, the one color format every backend can sample, draw into and read.
constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
// Direct3D 12 takes DXBC compiled from the HLSL at startup, which only Windows can do.
#ifdef _WIN32
constexpr SDL_GPUShaderFormat kHlslFormats = SDL_GPU_SHADERFORMAT_DXBC;
#else
constexpr SDL_GPUShaderFormat kHlslFormats = 0;
#endif
// The vertex buffer's first size: 3,276 vertices.
constexpr std::uint32_t kMinVertexBytes = 64 * 1024;
// Threads in a group of the particle shaders, as their [numthreads].
constexpr std::uint32_t kParticleThreads = 64;

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };
// The compute shaders: running particle steps, and turning particles into quads.
enum class ComputeKind : std::uint8_t { Process, Draw };

struct ShaderCode {
    std::span<const unsigned char> vertex_spirv;
    std::span<const unsigned char> fragment_spirv;
    std::string_view vertex_msl;
    std::string_view fragment_msl;
    std::string_view hlsl;
};

ShaderCode shader_code(ShaderKind kind) {
    if (kind == ShaderKind::Material) {
        return {.vertex_spirv = shaders::kMaterialVertexSpirv,
                .fragment_spirv = shaders::kMaterialFragmentSpirv,
                .vertex_msl = shaders::kMaterialVertexMsl,
                .fragment_msl = shaders::kMaterialFragmentMsl,
                .hlsl = shaders::kMaterialHlsl};
    }
    return {.vertex_spirv = shaders::kSpriteVertexSpirv,
            .fragment_spirv = shaders::kSpriteFragmentSpirv,
            .vertex_msl = shaders::kSpriteVertexMsl,
            .fragment_msl = shaders::kSpriteFragmentMsl,
            .hlsl = shaders::kSpriteHlsl};
}

// Godot's canvas blend modes, for straight-alpha colors.
SDL_GPUColorTargetBlendState blend_state(BlendMode mode) {
    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = true;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (mode) {
    case BlendMode::Mix:
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Add:
    case BlendMode::Sub:
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        if (mode == BlendMode::Sub) {
            blend.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            blend.alpha_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        }
        break;
    case BlendMode::Mul:
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        break;
    case BlendMode::PremulAlpha:
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
    return blend;
}

Error sdl_error(std::string_view what) {
    return make_error(ErrorCode::PlatformFailure, std::string(what) + ": " + SDL_GetError());
}

Backend backend_of(SDL_GPUDevice* device) {
    const std::string_view name = SDL_GetGPUDeviceDriver(device);
    if (name == "metal") {
        return Backend::Metal;
    }
    if (name == "direct3d12") {
        return Backend::D3D12;
    }
    return Backend::Vulkan;
}

// A buffer the CPU writes and the GPU reads from, or the other way round.
class TransferBuffer {
public:
    TransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBufferUsage usage, std::uint32_t size)
        : device_(device) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = usage;
        info.size = size;
        buffer_ = SDL_CreateGPUTransferBuffer(device, &info);
    }
    TransferBuffer(const TransferBuffer&) = delete;
    TransferBuffer& operator=(const TransferBuffer&) = delete;
    TransferBuffer(TransferBuffer&&) = delete;
    TransferBuffer& operator=(TransferBuffer&&) = delete;
    // SDL releases it once the GPU has finished with it.
    ~TransferBuffer() {
        if (buffer_ != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, buffer_);
        }
    }

    [[nodiscard]] SDL_GPUTransferBuffer* get() const { return buffer_; }

private:
    SDL_GPUDevice* device_;
    SDL_GPUTransferBuffer* buffer_ = nullptr;
};

class GpuDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    GpuDevice(SDL_GPUDevice* device, SDL_Window* window, bool owns_video)
        : device_(device), window_(window), owns_video_(owns_video), backend_(backend_of(device)) {}

    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&&) = delete;
    GpuDevice& operator=(GpuDevice&&) = delete;

    ~GpuDevice() override {
        if (frame_commands_ != nullptr) {
            SDL_SubmitGPUCommandBuffer(frame_commands_);
        }
        SDL_WaitForGPUIdle(device_);
        release_pending();
        for (const auto& [id, texture] : textures_) {
            SDL_ReleaseGPUTexture(device_, texture.gpu);
        }
        if (white_ != nullptr) {
            SDL_ReleaseGPUTexture(device_, white_);
        }
        for (SDL_GPUSampler* sampler : samplers_) {
            if (sampler != nullptr) {
                SDL_ReleaseGPUSampler(device_, sampler);
            }
        }
        for (const Pipeline& pipeline : pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline.pipeline);
        }
        for (SDL_GPUComputePipeline* pipeline : compute_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUComputePipeline(device_, pipeline);
            }
        }
        for (const auto& [id, system] : particle_systems_) {
            SDL_ReleaseGPUBuffer(device_, system.buffer);
        }
        if (particle_data_ != nullptr) {
            SDL_ReleaseGPUBuffer(device_, particle_data_);
            SDL_ReleaseGPUTransferBuffer(device_, particle_transfer_);
        }
        if (vertex_buffer_ != nullptr) {
            SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        }
        if (vertex_transfer_ != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer_);
        }
        for (SDL_GPUShader* shader : shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [id, game] : game_shaders_) {
            SDL_ReleaseGPUShader(device_, game.vertex);
            SDL_ReleaseGPUShader(device_, game.fragment);
        }
        if (window_ != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(device_, window_);
        }
        SDL_DestroyGPUDevice(device_);
        if (owns_video_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    // Claims the window, then makes the sprite shaders, samplers and the white texture. The
    // material shaders wait for the first material, since Direct3D 12 compiles them at run time.
    Result<void> init() {
        if (window_ != nullptr && !SDL_ClaimWindowForGPUDevice(device_, window_)) {
            window_ = nullptr;
            return sdl_error("the GPU device couldn't draw to the window");
        }
        if (auto made = make_shaders(ShaderKind::Sprite); !made) {
            return made;
        }
        for (const Filter filter : {Filter::Linear, Filter::Nearest}) {
            for (const bool repeat : {false, true}) {
                SDL_GPUSamplerCreateInfo info{};
                info.min_filter =
                    filter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
                info.mag_filter = info.min_filter;
                info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
                info.address_mode_u = repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                                             : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                info.address_mode_v = info.address_mode_u;
                info.address_mode_w = info.address_mode_u;
                SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &info);
                if (sampler == nullptr) {
                    return sdl_error("creating a sampler failed");
                }
                samplers_.at(sampler_index(filter, repeat)) = sampler;
            }
        }
        const TextureDesc white_desc{
            .width = 1, .height = 1, .filter = Filter::Linear, .render_target = false};
        auto white = create_gpu_texture(white_desc);
        if (!white) {
            return white.error();
        }
        white_ = white.value();
        constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};
        return upload(white_, white_desc, kWhite);
    }

    [[nodiscard]] Backend backend() const override { return backend_; }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> rgba) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, rgba, true); !checked) {
            return checked.error();
        }
        auto gpu = create_gpu_texture(desc);
        if (!gpu) {
            return gpu.error();
        }
        // New GPU memory holds whatever was there before, so it starts as transparent black.
        Result<void> filled;
        if (!rgba.empty()) {
            filled = upload(gpu.value(), desc, rgba);
        } else if (desc.render_target) {
            filled = clear(gpu.value());
        } else {
            const std::vector<std::uint8_t> zeros(std::size_t{desc.width} * desc.height * 4);
            filled = upload(gpu.value(), desc, zeros);
        }
        if (!filled) {
            SDL_ReleaseGPUTexture(device_, gpu.value());
            return filled.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.gpu = gpu.value(), .desc = desc};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> rgba) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        if (auto checked = check_pixels(it->second.desc, rgba, false); !checked) {
            return checked;
        }
        return upload(it->second.gpu, it->second.desc, rgba);
    }

    void destroy_texture(TextureId texture) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return;
        }
        // The frame being built may still draw with it, so it goes once the frame is sent.
        if (in_frame_) {
            pending_release_.push_back(it->second.gpu);
        } else {
            SDL_ReleaseGPUTexture(device_, it->second.gpu);
        }
        textures_.erase(it);
    }

    Result<ShaderId> create_shader(const CompiledShader& shader) override {
        if (auto checked = check_compiled_shader(shader, backend_); !checked) {
            return checked.error();
        }
        const auto bytes = [](const std::vector<std::uint32_t>& words) {
            return std::span<const unsigned char>(
                reinterpret_cast<const unsigned char*>(words.data()), words.size() * 4);
        };
        auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX,
                                      {.vertex_spirv = bytes(shader.spirv_vertex),
                                       .fragment_spirv = bytes(shader.spirv_fragment),
                                       .vertex_msl = shader.msl_vertex,
                                       .fragment_msl = shader.msl_fragment,
                                       .hlsl = shader.hlsl},
                                      0, 2);
        if (!vertex) {
            return vertex.error();
        }
        auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT,
                                        {.vertex_spirv = bytes(shader.spirv_vertex),
                                         .fragment_spirv = bytes(shader.spirv_fragment),
                                         .vertex_msl = shader.msl_vertex,
                                         .fragment_msl = shader.msl_fragment,
                                         .hlsl = shader.hlsl},
                                        static_cast<std::uint32_t>(kMaterialSamplers), 2);
        if (!fragment) {
            SDL_ReleaseGPUShader(device_, vertex.value());
            return fragment.error();
        }
        const auto id = static_cast<ShaderId>(next_shader_++);
        game_shaders_[id] = {.vertex = vertex.value(), .fragment = fragment.value()};
        return id;
    }

    void destroy_shader(ShaderId shader) override {
        const auto it = game_shaders_.find(shader);
        if (it == game_shaders_.end()) {
            return;
        }
        // The frame being built may still draw with its pipelines, so they go once it is sent.
        std::erase_if(pipelines_, [&](const Pipeline& pipeline) {
            if (pipeline.game != shader) {
                return false;
            }
            if (in_frame_) {
                pending_pipelines_.push_back(pipeline.pipeline);
            } else {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline.pipeline);
            }
            return true;
        });
        SDL_ReleaseGPUShader(device_, it->second.vertex);
        SDL_ReleaseGPUShader(device_, it->second.fragment);
        game_shaders_.erase(it);
    }

    Result<ParticlesId> create_particles(std::uint32_t amount) override {
        if (auto checked = check_particle_amount(amount); !checked) {
            return checked.error();
        }
        if (auto pipeline = compute_pipeline(ComputeKind::Process); !pipeline) {
            return pipeline.error();
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage =
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        info.size = amount * static_cast<std::uint32_t>(sizeof(Particle));
        SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
        if (buffer == nullptr) {
            return sdl_error("creating a system of " + std::to_string(amount) +
                             " particles failed");
        }
        // New GPU memory holds whatever was there before, so the system starts by clearing it.
        if (auto queued =
                queue_particle_job({.particles = buffer, .amount = amount, .reset = true});
            !queued) {
            SDL_ReleaseGPUBuffer(device_, buffer);
            return queued.error();
        }
        const auto id = static_cast<ParticlesId>(next_particles_++);
        particle_systems_[id] = {.buffer = buffer, .amount = amount};
        return id;
    }

    void destroy_particles(ParticlesId particles) override {
        const auto it = particle_systems_.find(particles);
        if (it == particle_systems_.end()) {
            return;
        }
        // Jobs waiting for the frame may still use it, so it goes once they are sent.
        if (particle_jobs_.empty()) {
            SDL_ReleaseGPUBuffer(device_, it->second.buffer);
        } else {
            pending_buffers_.push_back(it->second.buffer);
        }
        particle_systems_.erase(it);
    }

    Result<void> process_particles(ParticlesId particles, const ParticleProcess& process,
                                   std::span<const ParticleStep> steps) override {
        const auto it = particle_systems_.find(particles);
        if (it == particle_systems_.end()) {
            return unknown_particles(particles);
        }
        if (auto checked = check_particle_process(process, steps); !checked) {
            return checked;
        }
        if (steps.empty()) {
            return {};
        }
        ParticleJob job{.particles = it->second.buffer, .amount = it->second.amount};
        job.material = material_words(process);
        job.steps = static_cast<std::uint32_t>(particle_words_.size());
        job.step_count = static_cast<std::uint32_t>(steps.size());
        pack_particle_steps(steps, particle_words_);
        return queue_particle_job(job);
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
        if (auto checked = check_particle_draw(look, amount, frame_vertex_count()); !checked) {
            return checked;
        }
        if (auto checked = check_draw(texture, material); !checked) {
            return checked;
        }
        if (frame_commands_ == nullptr) {
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
        const std::uint32_t count = amount * 6;
        batch.value().particles = true;
        batch.value().count = count;
        batches_.push_back(std::move(batch).value());
        ++stats_.draws;
        stats_.triangles += count / 3;
        particle_jobs_.push_back({.particles = it->second.buffer,
                                  .amount = amount,
                                  .draw = true,
                                  .look = draw_uniforms(look, amount, particle_vertices_)});
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
        const std::uint32_t size = it->second.amount * static_cast<std::uint32_t>(sizeof(Particle));
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size);
        if (transfer.get() == nullptr) {
            return sdl_error("making room to read the particles failed");
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("reading the particles failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUBufferRegion source{.buffer = it->second.buffer, .offset = 0, .size = size};
        const SDL_GPUTransferBufferLocation destination{.transfer_buffer = transfer.get(),
                                                        .offset = 0};
        SDL_DownloadFromGPUBuffer(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (fence == nullptr) {
            return sdl_error("reading the particles failed");
        }
        const bool finished = SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        if (!finished) {
            return sdl_error("waiting for the GPU failed");
        }
        const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer.get(), false);
        if (mapped == nullptr) {
            return sdl_error("reading the particles failed");
        }
        std::vector<Particle> out(it->second.amount);
        std::memcpy(out.data(), mapped, size);
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        return out;
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        SDL_GPUTexture* target = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SDL_GPUTextureFormat format = kTextureFormat;
        if (frame.target == TextureId::None) {
            if (window_ == nullptr) {
                return make_error(ErrorCode::InvalidArgument,
                                  "this device has no window; draw to a render-target texture");
            }
            format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        } else {
            const auto it = textures_.find(frame.target);
            if (it == textures_.end()) {
                return unknown_texture(frame.target);
            }
            if (!it->second.desc.render_target) {
                return not_a_render_target(frame.target);
            }
            target = it->second.gpu;
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
        // Made before the command buffer, so a failure leaves nothing half started. Pipelines for
        // materials are made when a draw first needs them.
        auto pipeline = pipeline_for(format, ShaderKind::Sprite, BlendMode::Mix);
        if (!pipeline) {
            return pipeline.error();
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("starting a frame failed");
        }
        if (frame.target == TextureId::None) {
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window_, &target, &width,
                                                       &height)) {
                SDL_CancelGPUCommandBuffer(commands);
                return sdl_error("getting the window's next image failed");
            }
            if (target == nullptr) {
                // Minimized or hidden: there is nothing to draw to this time.
                SDL_CancelGPUCommandBuffer(commands);
                start_frame(frame, nullptr, nullptr, 0, 0);
                return false;
            }
        }
        start_frame(frame, commands, target, width, height);
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
        if (vertices.empty() || frame_commands_ == nullptr) {
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
        if (frame_commands_ == nullptr) {
            // A skipped frame draws nothing, but its particles still run.
            Result<void> flushed = flush_particle_jobs();
            finish_frame();
            return flushed;
        }
        SDL_GPUCommandBuffer* commands = std::exchange(frame_commands_, nullptr);
        Result<void> uploaded = upload_frame(commands);
        if (!uploaded) {
            particle_jobs_.clear();
            SDL_SubmitGPUCommandBuffer(commands);
            finish_frame();
            return uploaded;
        }
        // Particles run and write their quads ahead of the render pass that draws them.
        const auto base = static_cast<std::uint32_t>(vertices_.size());
        dispatch_particle_jobs(commands, base, vertices_.empty());

        SDL_GPUColorTargetInfo color{};
        color.texture = frame_texture_;
        color.clear_color = {frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        if (!batches_.empty()) {
            const Mat4 view = frame_.view.value_or(pixel_projection(
                static_cast<float>(frame_width_), static_cast<float>(frame_height_)));
            if (std::ranges::any_of(batches_, [](const Batch& batch) {
                    return batch.kind == ShaderKind::Material;
                })) {
                const auto lights =
                    std::make_unique<LightsUniforms>(lights_uniforms(frame_.lighting));
                SDL_PushGPUFragmentUniformData(commands, 1, lights.get(),
                                               static_cast<std::uint32_t>(sizeof(LightsUniforms)));
            }
            const SDL_GPUBufferBinding vertices{.buffer = vertex_buffer_, .offset = 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_GPUGraphicsPipeline* bound = nullptr;
            std::optional<Mat4> pushed;
            for (const Batch& batch : batches_) {
                // The view, times the draw's transform if it has one, pushed when it changes.
                const Mat4 matrix =
                    batch.material.transform ? multiply(view, *batch.material.transform) : view;
                if (pushed != matrix) {
                    SDL_PushGPUVertexUniformData(commands, 0, matrix.data(),
                                                 static_cast<std::uint32_t>(sizeof(matrix)));
                    pushed = matrix;
                }
                if (batch.pipeline != bound) {
                    SDL_BindGPUGraphicsPipeline(pass, batch.pipeline);
                    bound = batch.pipeline;
                }
                if (batch.material.shader != ShaderId::None) {
                    const VertexProgramUniforms uniforms =
                        vertex_program_uniforms(batch.material, batch.texel);
                    SDL_PushGPUVertexUniformData(commands, 1, &uniforms,
                                                 static_cast<std::uint32_t>(sizeof(uniforms)));
                }
                if (batch.kind == ShaderKind::Sprite) {
                    SDL_BindGPUFragmentSamplers(pass, 0, batch.samplers.data(), 1);
                } else {
                    push_program(commands, batch);
                    SDL_BindGPUFragmentSamplers(pass, 0, batch.samplers.data(),
                                                static_cast<std::uint32_t>(batch.samplers.size()));
                }
                SDL_DrawGPUPrimitives(pass, batch.count, 1,
                                      batch.particles ? base + batch.first : batch.first, 0);
            }
        }
        SDL_EndGPURenderPass(pass);
        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        ++stats_.frames;
        finish_frame();
        if (!submitted) {
            return sdl_error("sending the frame to the GPU failed");
        }
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
        const std::size_t size = std::size_t{desc.width} * desc.height * 4;
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                      static_cast<std::uint32_t>(size));
        if (transfer.get() == nullptr) {
            return sdl_error("making room to read the texture failed");
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("reading the texture failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureRegion region = whole(it->second.gpu, desc);
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = transfer.get();
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (fence == nullptr) {
            return sdl_error("reading the texture failed");
        }
        const bool finished = SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        if (!finished) {
            return sdl_error("waiting for the GPU failed");
        }
        const auto* mapped = static_cast<const std::uint8_t*>(
            SDL_MapGPUTransferBuffer(device_, transfer.get(), false));
        if (mapped == nullptr) {
            return sdl_error("reading the texture failed");
        }
        Image image{.width = desc.width, .height = desc.height, .pixels = {}};
        image.pixels.assign(mapped, mapped + size);
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        return image;
    }

    VsyncMode set_vsync(VsyncMode mode) override {
        if (window_ == nullptr) {
            vsync_ = mode;
            return vsync_;
        }
        // SDL has no adaptive (relaxed FIFO) present mode, so it, like an unsupported mode,
        // falls back to waiting for every refresh, which every backend supports.
        SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
        VsyncMode effective = VsyncMode::Enabled;
        if (mode == VsyncMode::Disabled &&
            SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
            present = SDL_GPU_PRESENTMODE_IMMEDIATE;
            effective = VsyncMode::Disabled;
        } else if (mode == VsyncMode::Mailbox &&
                   SDL_WindowSupportsGPUPresentMode(device_, window_,
                                                    SDL_GPU_PRESENTMODE_MAILBOX)) {
            present = SDL_GPU_PRESENTMODE_MAILBOX;
            effective = VsyncMode::Mailbox;
        }
        if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                           present)) {
            log::warn("changing V-Sync failed", {{"error", SDL_GetError()}});
            return vsync_;
        }
        vsync_ = effective;
        return vsync_;
    }
    [[nodiscard]] VsyncMode vsync() const override { return vsync_; }

    [[nodiscard]] Stats stats() const override {
        Stats stats = stats_;
        stats.textures = static_cast<std::uint32_t>(textures_.size());
        return stats;
    }

private:
    struct Texture {
        SDL_GPUTexture* gpu = nullptr;
        TextureDesc desc;
    };

    // Draws in a row with the same texture and material, sent as one GPU draw.
    struct Batch {
        TextureId texture = TextureId::None;
        Material material;
        ShaderKind kind = ShaderKind::Sprite;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        // The draw's texture, the material's, the shading's and the frame's lighting textures.
        std::array<SDL_GPUTextureSamplerBinding, kMaterialSamplers> samplers{};
        std::array<float, 4> texel{};
        // Its first vertex: in the frame's particle quads, after the other vertices, for
        // particles.
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        bool particles = false; // a draw of particles, which is never batched with others
    };

    struct Pipeline {
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        ShaderKind kind = ShaderKind::Sprite;
        BlendMode blend = BlendMode::Mix;
        ShaderId game = ShaderId::None; // a game's shader, drawn instead of the kind's
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
    };

    // A game's shader (create_shader).
    struct GameShader {
        SDL_GPUShader* vertex = nullptr;
        SDL_GPUShader* fragment = nullptr;
    };

    struct ParticleSystem {
        SDL_GPUBuffer* buffer = nullptr;
        std::uint32_t amount = 0;
    };

    // A compute pass for the GPU to run ahead of the frame's draws, in the order called: steps
    // of a system, or a draw turning it into quads.
    struct ParticleJob {
        SDL_GPUBuffer* particles = nullptr;
        std::uint32_t amount = 0;
        bool draw = false;
        bool reset = false;         // clears every particle first
        std::uint32_t material = 0; // where its process material is in particle_words_
        std::uint32_t steps = 0;    // and its steps
        std::uint32_t step_count = 0;
        DrawUniforms look{}; // for a draw; its first vertex is in the particle quads
    };

    static std::size_t shader_index(ShaderKind kind, SDL_GPUShaderStage stage) {
        return (static_cast<std::size_t>(kind) * 2) + (stage == SDL_GPU_SHADERSTAGE_VERTEX ? 0 : 1);
    }

    static std::size_t sampler_index(Filter filter, bool repeat) {
        return (static_cast<std::size_t>(filter) * 2) + (repeat ? 1 : 0);
    }

    // A texture and its sampler; None is white. The texture must exist.
    SDL_GPUTextureSamplerBinding binding(TextureId texture, bool repeat) const {
        if (texture == TextureId::None) {
            return {.texture = white_,
                    .sampler = samplers_.at(sampler_index(Filter::Linear, repeat))};
        }
        const Texture& found = textures_.at(texture);
        return {.texture = found.gpu,
                .sampler = samplers_.at(sampler_index(found.desc.filter, repeat))};
    }

    static void push_program(SDL_GPUCommandBuffer* commands, const Batch& batch) {
        const ProgramUniforms uniforms = program_uniforms(batch.material, batch.texel);
        SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                       static_cast<std::uint32_t>(sizeof(uniforms)));
    }

    static SDL_GPUTextureRegion whole(SDL_GPUTexture* texture, const TextureDesc& desc) {
        SDL_GPUTextureRegion region{};
        region.texture = texture;
        region.w = desc.width;
        region.h = desc.height;
        region.d = 1;
        return region;
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
                    .samplers = {},
                    .texel = {1.0F, 1.0F, 0.0F, 0.0F},
                    .first = first,
                    .count = 0};
        batch.samplers.at(0) = binding(texture, false);
        for (std::size_t slot = 1; slot < kMaterialTextureSlots; ++slot) {
            const MaterialTexture& bound = material.textures.at(slot - 1);
            batch.samplers.at(slot) = binding(bound.texture, bound.repeat);
        }
        batch.samplers.at(kMaterialTextureSlots) = binding(material.shading.normal_map, false);
        batch.samplers.at(kMaterialTextureSlots + 1) =
            binding(material.shading.specular_map, false);
        batch.samplers.at(kMaterialTextureSlots + 2) = binding(frame_.lighting.atlas, false);
        batch.samplers.at(kMaterialTextureSlots + 3) = binding(frame_.lighting.shadow_map, false);
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

    // A compute shader's pipeline, made the first time it is needed.
    Result<SDL_GPUComputePipeline*> compute_pipeline(ComputeKind kind) {
        SDL_GPUComputePipeline*& slot = compute_pipelines_.at(static_cast<std::size_t>(kind));
        if (slot != nullptr) {
            return slot;
        }
        const bool process = kind == ComputeKind::Process;
        SDL_GPUComputePipelineCreateInfo info{};
        info.entrypoint = "cs_main";
        info.num_readonly_storage_buffers = 1;
        info.num_readwrite_storage_buffers = 1;
        info.num_uniform_buffers = 1;
        info.threadcount_x = kParticleThreads;
        info.threadcount_y = 1;
        info.threadcount_z = 1;
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
        std::vector<std::uint8_t> compiled;
        std::string msl;
        if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
            const std::span<const unsigned char> spirv =
                process ? std::span<const unsigned char>(shaders::kParticlesProcessComputeSpirv)
                        : std::span<const unsigned char>(shaders::kParticlesDrawComputeSpirv);
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.code = spirv.data();
            info.code_size = spirv.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            // In pieces, since no string literal may be as long as the whole.
            const std::span<const char* const> pieces =
                process ? std::span<const char* const>(shaders::kParticlesProcessComputeMsl)
                        : std::span<const char* const>(shaders::kParticlesDrawComputeMsl);
            for (const char* piece : pieces) {
                msl += piece;
            }
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.code = reinterpret_cast<const std::uint8_t*>(msl.data());
            info.code_size = msl.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_DXBC) != 0) {
            auto bytecode =
                compile_hlsl(process ? shaders::kParticlesProcessHlsl : shaders::kParticlesDrawHlsl,
                             "cs_main", "cs_5_1");
            if (!bytecode) {
                return bytecode.error();
            }
            compiled = std::move(bytecode).value();
            info.format = SDL_GPU_SHADERFORMAT_DXBC;
            info.code = compiled.data();
            info.code_size = compiled.size();
        } else {
            return make_error(ErrorCode::PlatformFailure,
                              "the GPU backend takes no shader format SlashEngine ships");
        }
        slot = SDL_CreateGPUComputePipeline(device_, &info);
        if (slot == nullptr) {
            return sdl_error("creating the particle pipeline failed");
        }
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
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            clear_particle_jobs();
            return sdl_error("running particles failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        Result<void> uploaded = upload_particle_words(copy);
        SDL_EndGPUCopyPass(copy);
        if (uploaded) {
            dispatch_particle_jobs(commands, 0, false);
        }
        clear_particle_jobs();
        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        release_pending_buffers();
        if (!uploaded) {
            return uploaded;
        }
        if (!submitted) {
            return sdl_error("running particles failed");
        }
        return {};
    }

    // Copies what the queued jobs read to the GPU.
    Result<void> upload_particle_words(SDL_GPUCopyPass* copy) {
        if (particle_jobs_.empty()) {
            return {};
        }
        if (particle_words_.empty()) {
            particle_words_.push_back({}); // the process shader always reads a buffer
        }
        const auto size = static_cast<std::uint32_t>(particle_words_.size() * sizeof(Word));
        if (size > particle_capacity_) {
            const std::uint32_t capacity =
                std::max({size, particle_capacity_ * 2, kMinVertexBytes});
            if (particle_data_ != nullptr) {
                SDL_ReleaseGPUBuffer(device_, particle_data_);
                SDL_ReleaseGPUTransferBuffer(device_, particle_transfer_);
                particle_data_ = nullptr;
                particle_transfer_ = nullptr;
                particle_capacity_ = 0;
            }
            SDL_GPUBufferCreateInfo buffer{};
            buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
            buffer.size = capacity;
            particle_data_ = SDL_CreateGPUBuffer(device_, &buffer);
            SDL_GPUTransferBufferCreateInfo transfer{};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer.size = capacity;
            particle_transfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer);
            if (particle_data_ == nullptr || particle_transfer_ == nullptr) {
                return sdl_error("making room for the particles' steps failed");
            }
            particle_capacity_ = capacity;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, particle_transfer_, true);
        if (mapped == nullptr) {
            return sdl_error("uploading the particles' steps failed");
        }
        std::memcpy(mapped, particle_words_.data(), size);
        SDL_UnmapGPUTransferBuffer(device_, particle_transfer_);
        const SDL_GPUTransferBufferLocation source{.transfer_buffer = particle_transfer_,
                                                   .offset = 0};
        const SDL_GPUBufferRegion destination{.buffer = particle_data_, .offset = 0, .size = size};
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        return {};
    }

    // Records the queued jobs as compute passes, in order. Draws write their quads into the
    // vertex buffer from vertex base, cycling it first when nothing has written it this frame.
    void dispatch_particle_jobs(SDL_GPUCommandBuffer* commands, std::uint32_t base,
                                bool cycle_vertices) {
        for (const ParticleJob& job : particle_jobs_) {
            const std::uint32_t groups = (job.amount + kParticleThreads - 1) / kParticleThreads;
            SDL_GPUStorageBufferReadWriteBinding output{};
            output.buffer = job.draw ? vertex_buffer_ : job.particles;
            output.cycle = job.draw && cycle_vertices;
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &output, 1);
            if (job.draw) {
                cycle_vertices = false;
                SDL_BindGPUComputePipeline(
                    pass, compute_pipelines_.at(static_cast<std::size_t>(ComputeKind::Draw)));
                SDL_BindGPUComputeStorageBuffers(pass, 0, &job.particles, 1);
                DrawUniforms uniforms = job.look;
                uniforms.counts[3] += base;
                SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                              static_cast<std::uint32_t>(sizeof(uniforms)));
            } else {
                SDL_BindGPUComputePipeline(
                    pass, compute_pipelines_.at(static_cast<std::size_t>(ComputeKind::Process)));
                SDL_BindGPUComputeStorageBuffers(pass, 0, &particle_data_, 1);
                const ProcessUniforms uniforms{.amount = job.amount,
                                               .material = job.material,
                                               .steps = job.steps,
                                               .step_count = job.step_count,
                                               .reset = job.reset ? 1U : 0U};
                SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                              static_cast<std::uint32_t>(sizeof(uniforms)));
            }
            SDL_DispatchGPUCompute(pass, groups, 1, 1);
            SDL_EndGPUComputePass(pass);
        }
    }

    void clear_particle_jobs() {
        particle_jobs_.clear();
        particle_words_.clear();
        last_material_.reset();
    }

    void release_pending_buffers() {
        for (SDL_GPUBuffer* buffer : pending_buffers_) {
            SDL_ReleaseGPUBuffer(device_, buffer);
        }
        pending_buffers_.clear();
    }

    // Both of a kind's shaders, unless they are made already.
    Result<void> make_shaders(ShaderKind kind) {
        for (const SDL_GPUShaderStage stage :
             {SDL_GPU_SHADERSTAGE_VERTEX, SDL_GPU_SHADERSTAGE_FRAGMENT}) {
            SDL_GPUShader*& slot = shaders_.at(shader_index(kind, stage));
            if (slot != nullptr) {
                continue;
            }
            const bool material = kind == ShaderKind::Material;
            const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
            std::uint32_t samplers = 0;
            std::uint32_t uniforms = 1;
            if (!vertex) {
                samplers = material ? static_cast<std::uint32_t>(kMaterialSamplers) : 1;
                uniforms = material ? 2 : 0;
            }
            auto shader = make_gpu_shader(stage, shader_code(kind), samplers, uniforms);
            if (!shader) {
                return shader.error();
            }
            slot = shader.value();
        }
        return {};
    }

    // One stage of a shader, in the format the backend takes.
    Result<SDL_GPUShader*> make_gpu_shader(SDL_GPUShaderStage stage, const ShaderCode& code,
                                           std::uint32_t samplers, std::uint32_t uniforms) {
        const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo info{};
        info.stage = stage;
        info.entrypoint = vertex ? "vs_main" : "fs_main";
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
        std::vector<std::uint8_t> compiled;
        if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
            const std::span<const unsigned char> spirv =
                vertex ? code.vertex_spirv : code.fragment_spirv;
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.code = spirv.data();
            info.code_size = spirv.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            const std::string_view msl = vertex ? code.vertex_msl : code.fragment_msl;
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.code = reinterpret_cast<const std::uint8_t*>(msl.data());
            info.code_size = msl.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_DXBC) != 0) {
            auto bytecode = compile_hlsl(code.hlsl, info.entrypoint, vertex ? "vs_5_1" : "ps_5_1");
            if (!bytecode) {
                return bytecode.error();
            }
            compiled = std::move(bytecode).value();
            info.format = SDL_GPU_SHADERFORMAT_DXBC;
            info.code = compiled.data();
            info.code_size = compiled.size();
        } else {
            return make_error(ErrorCode::PlatformFailure,
                              "the GPU backend takes no shader format SlashEngine ships");
        }
        SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
        if (shader == nullptr) {
            return sdl_error(vertex ? "creating the vertex shader failed"
                                    : "creating the fragment shader failed");
        }
        return shader;
    }

    // The 2D pipeline for a target format, shader and blend mode, made the first time a frame
    // needs it.
    Result<SDL_GPUGraphicsPipeline*> pipeline_for(SDL_GPUTextureFormat format, ShaderKind kind,
                                                  BlendMode blend, ShaderId game = ShaderId::None) {
        for (const Pipeline& made : pipelines_) {
            if (made.format == format && made.kind == kind && made.blend == blend &&
                made.game == game) {
                return made.pipeline;
            }
        }
        if (game == ShaderId::None) {
            if (auto made = make_shaders(kind); !made) {
                return made.error();
            }
        }
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(Vertex2D);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        std::array<SDL_GPUVertexAttribute, 3> attributes{};
        attributes[0] = {.location = 0,
                         .buffer_slot = 0,
                         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                         .offset = offsetof(Vertex2D, x)};
        attributes[1] = {.location = 1,
                         .buffer_slot = 0,
                         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                         .offset = offsetof(Vertex2D, u)};
        attributes[2] = {.location = 2,
                         .buffer_slot = 0,
                         .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                         .offset = offsetof(Vertex2D, rgba)};

        SDL_GPUColorTargetDescription color{};
        color.format = format;
        color.blend_state = blend_state(blend);

        SDL_GPUGraphicsPipelineCreateInfo info{};
        if (game == ShaderId::None) {
            info.vertex_shader = shaders_.at(shader_index(kind, SDL_GPU_SHADERSTAGE_VERTEX));
            info.fragment_shader = shaders_.at(shader_index(kind, SDL_GPU_SHADERSTAGE_FRAGMENT));
        } else {
            const GameShader& shaders = game_shaders_.at(game);
            info.vertex_shader = shaders.vertex;
            info.fragment_shader = shaders.fragment;
        }
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating the 2D pipeline failed");
        }
        pipelines_.push_back(
            {.format = format, .kind = kind, .blend = blend, .game = game, .pipeline = pipeline});
        return pipeline;
    }

    Result<SDL_GPUTexture*> create_gpu_texture(const TextureDesc& desc) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kTextureFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (desc.render_target) {
            info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        }
        info.width = desc.width;
        info.height = desc.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
        if (texture == nullptr) {
            return sdl_error("creating a " + std::to_string(desc.width) + "x" +
                             std::to_string(desc.height) + " texture failed");
        }
        return texture;
    }

    // Replaces every pixel. It runs ahead of the frame being built, whose draws are sent at
    // end_frame, so they all see the new pixels.
    Result<void> upload(SDL_GPUTexture* texture, const TextureDesc& desc,
                        std::span<const std::uint8_t> rgba) {
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                      static_cast<std::uint32_t>(rgba.size()));
        if (transfer.get() == nullptr) {
            return sdl_error("making room to upload the texture failed");
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer.get(), false);
        if (mapped == nullptr) {
            return sdl_error("uploading the texture failed");
        }
        std::memcpy(mapped, rgba.data(), rgba.size());
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("uploading the texture failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer.get();
        const SDL_GPUTextureRegion region = whole(texture, desc);
        // Every pixel is replaced, so a texture still in use by an earlier frame can be swapped
        // for fresh memory instead of waiting for that frame.
        SDL_UploadToGPUTexture(copy, &source, &region, true);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            return sdl_error("uploading the texture failed");
        }
        return {};
    }

    // Clears a render target to transparent black.
    Result<void> clear(SDL_GPUTexture* texture) {
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("clearing the texture failed");
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = texture;
        color.clear_color = {0.0F, 0.0F, 0.0F, 0.0F};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &color, 1, nullptr));
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            return sdl_error("clearing the texture failed");
        }
        return {};
    }

    // Copies the frame's vertices, and what its particle jobs read, to the GPU ahead of its
    // passes, with room after the vertices for the particles' quads.
    Result<void> upload_frame(SDL_GPUCommandBuffer* commands) {
        const auto total = static_cast<std::uint32_t>(frame_vertex_count() * sizeof(Vertex2D));
        if (total == 0 && particle_jobs_.empty()) {
            return {};
        }
        const auto size = static_cast<std::uint32_t>(vertices_.size() * sizeof(Vertex2D));
        if (total > vertex_capacity_) {
            // Grows by doubling, so a busy game settles on one size.
            const std::uint32_t capacity = std::max({total, vertex_capacity_ * 2, kMinVertexBytes});
            if (vertex_buffer_ != nullptr) {
                SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
                SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer_);
                vertex_buffer_ = nullptr;
                vertex_transfer_ = nullptr;
                vertex_capacity_ = 0;
            }
            SDL_GPUBufferCreateInfo buffer{};
            buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            buffer.size = capacity;
            vertex_buffer_ = SDL_CreateGPUBuffer(device_, &buffer);
            SDL_GPUTransferBufferCreateInfo transfer{};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer.size = capacity;
            vertex_transfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer);
            if (vertex_buffer_ == nullptr || vertex_transfer_ == nullptr) {
                return sdl_error("making room for the frame's vertices failed");
            }
            vertex_capacity_ = capacity;
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        if (size > 0) {
            // Cycling hands over fresh memory while earlier frames still read the old.
            void* mapped = SDL_MapGPUTransferBuffer(device_, vertex_transfer_, true);
            if (mapped == nullptr) {
                SDL_EndGPUCopyPass(copy);
                return sdl_error("uploading the frame's vertices failed");
            }
            std::memcpy(mapped, vertices_.data(), size);
            SDL_UnmapGPUTransferBuffer(device_, vertex_transfer_);
            const SDL_GPUTransferBufferLocation source{.transfer_buffer = vertex_transfer_,
                                                       .offset = 0};
            const SDL_GPUBufferRegion destination{
                .buffer = vertex_buffer_, .offset = 0, .size = size};
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        Result<void> uploaded = upload_particle_words(copy);
        SDL_EndGPUCopyPass(copy);
        return uploaded;
    }

    void start_frame(const Frame& frame, SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                     std::uint32_t width, std::uint32_t height) {
        in_frame_ = true;
        frame_ = frame;
        frame_commands_ = commands;
        frame_texture_ = target;
        frame_width_ = width;
        frame_height_ = height;
        vertices_.clear();
        batches_.clear();
        particle_vertices_ = 0;
    }

    void finish_frame() {
        frame_commands_ = nullptr;
        frame_texture_ = nullptr;
        vertices_.clear();
        batches_.clear();
        particle_vertices_ = 0;
        clear_particle_jobs();
        release_pending();
    }

    void release_pending() {
        for (SDL_GPUTexture* texture : pending_release_) {
            SDL_ReleaseGPUTexture(device_, texture);
        }
        pending_release_.clear();
        for (SDL_GPUGraphicsPipeline* pipeline : pending_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        pending_pipelines_.clear();
        release_pending_buffers();
    }

    SDL_GPUDevice* device_;
    SDL_Window* window_;
    bool owns_video_;
    Backend backend_;
    std::array<SDL_GPUShader*, 4> shaders_{}; // by shader_index
    std::vector<Pipeline> pipelines_;
    std::array<SDL_GPUSampler*, 4> samplers_{}; // by sampler_index
    SDL_GPUTexture* white_ = nullptr;
    SDL_GPUBuffer* vertex_buffer_ = nullptr;
    SDL_GPUTransferBuffer* vertex_transfer_ = nullptr;
    std::uint32_t vertex_capacity_ = 0;
    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    std::vector<SDL_GPUTexture*> pending_release_;
    std::unordered_map<ShaderId, GameShader> game_shaders_;
    std::uint32_t next_shader_ = 1;
    std::vector<SDL_GPUGraphicsPipeline*> pending_pipelines_;
    std::array<SDL_GPUComputePipeline*, 2> compute_pipelines_{}; // by ComputeKind
    std::unordered_map<ParticlesId, ParticleSystem> particle_systems_;
    std::uint32_t next_particles_ = 1;
    std::vector<SDL_GPUBuffer*> pending_buffers_;
    // Jobs waiting to run, and the words they read.
    std::vector<ParticleJob> particle_jobs_;
    std::vector<Word> particle_words_;
    std::optional<std::pair<ParticleProcess, std::uint32_t>> last_material_;
    SDL_GPUBuffer* particle_data_ = nullptr;
    SDL_GPUTransferBuffer* particle_transfer_ = nullptr;
    std::uint32_t particle_capacity_ = 0;

    // The frame being built. A skipped frame has no command buffer.
    bool in_frame_ = false;
    Frame frame_;
    SDL_GPUCommandBuffer* frame_commands_ = nullptr;
    SDL_GPUTexture* frame_texture_ = nullptr;
    SDL_GPUTextureFormat frame_format_ = kTextureFormat;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    std::uint32_t particle_vertices_ = 0;  // the particles' quads, after vertices_
    VsyncMode vsync_ = VsyncMode::Enabled; // SDL's default present mode
    Stats stats_;
};

} // namespace

Result<void> init_video() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return {};
    }
    const std::string first_error = SDL_GetError();
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    const bool started = SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    if (!started) {
        return make_error(ErrorCode::PlatformFailure,
                          "SDL's video subsystem didn't start: " + first_error);
    }
    return {};
}

Result<std::unique_ptr<RenderDevice>> create_gpu_device(const DeviceOptions& options) {
    const bool owns_video = options.window == nullptr;
    if (owns_video) {
        if (auto started = init_video(); !started) {
            return started.error();
        }
    }
    const SDL_GPUShaderFormat formats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | kHlslFormats;
    const char* name = nullptr;
    if (options.driver == "d3d12") {
        name = "direct3d12";
    } else if (options.driver != "auto") {
        name = options.driver.c_str();
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, options.debug, name);
    if (device == nullptr) {
        Error error = sdl_error(options.driver == "auto"
                                    ? std::string("no GPU backend works here")
                                    : "the " + options.driver + " backend doesn't work here");
        if (owns_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        return error;
    }
    auto gpu = std::make_unique<GpuDevice>(device, options.window, owns_video);
    if (auto ready = gpu->init(); !ready) {
        return ready.error();
    }
    log::info("render device created",
              {{"backend", backend_name(gpu->backend())}, {"window", options.window != nullptr}});
    return std::unique_ptr<RenderDevice>(std::move(gpu));
}

} // namespace slash::render::detail
