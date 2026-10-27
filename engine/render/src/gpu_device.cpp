#include "gpu_device.hpp"

#include "checks.hpp"
#include "decal.hpp"
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
#include "fsr3.hpp"
#include "fsr3_accumulate_shaders.hpp"
#include "fsr3_instability_shaders.hpp"
#include "fsr3_lock_shaders.hpp"
#include "fsr3_prepare_shaders.hpp"
#include "fsr3_pyramid_shaders.hpp"
#include "fsr3_reactivity_shaders.hpp"
#include "fsr3_reconstruct_shaders.hpp"
#include "fsr3_reduce_shaders.hpp"
#include "fsr3_shading_change_shaders.hpp"
#include "hlsl_compiler.hpp"
#include "material_shaders.hpp"
#include "mesh_clustered_shaders.hpp"
#include "mesh_motion_shaders.hpp"
#include "mesh_normal_shaders.hpp"
#include "mesh_shaders.hpp"
#include "mesh_shading.hpp"
#include "metalfx.hpp"
#include "native.hpp"
#include "particle_layout.hpp"
#include "particle_layout_3d.hpp"
#include "particles_draw_shaders.hpp"
#include "particles_emit_shaders.hpp"
#include "particles_mesh_3d_shaders.hpp"
#include "particles_process_3d_shaders.hpp"
#include "particles_process_shaders.hpp"
#include "particles_sort_3d_shaders.hpp"
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
#include "volumetric_fog.hpp"
#include "volumetric_fog_shaders.hpp"
#include "volumetric_fog_sky_shaders.hpp"
#include "vulkan_rays.hpp"

#include "slash/core/log.hpp"
#include "slash/render/image.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render::detail {
namespace {

// Textures are 8-bit RGBA, the one color format every backend can sample, draw into and read,
// or the compressed formats the GPU samples.
constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
// The radiance's cubes are RGB10A2, as Godot's.
constexpr SDL_GPUTextureFormat kRadianceFormat = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
// A Forward+ frame's world and glow's levels are half floats, as Godot's Forward+ buffers.
constexpr SDL_GPUTextureFormat kHdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
// Auto exposure's luminance textures: one float a texel, as Godot's.
constexpr SDL_GPUTextureFormat kLuminanceFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
// Forward+'s normal-roughness prepass draws into bytes.
constexpr SDL_GPUTextureFormat kNormalFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
// TAA's motion vectors are two half floats, as Godot's velocity buffer.
constexpr SDL_GPUTextureFormat kVelocityFormat = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;

constexpr SDL_GPUTextureFormat ssao_gpu_format(SsaoFormat format) {
    switch (format) {
    case SsaoFormat::R16F:
        return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    case SsaoFormat::RG8:
        return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case SsaoFormat::R8:
        return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case SsaoFormat::R32F:
        break;
    }
    return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
}

// Direct3D 12's SSAO shaders, each for a group of ssao.hlsl's passes (SSAO_PASS_FIRST to
// SSAO_PASS_LAST), as Godot's are: the depth, the gather, the importance map, the blur and the
// interleave. WARP loses the device running the shader with every pass in it.
constexpr std::array<std::array<std::uint32_t, 2>, 5> kSsaoShaderGroups{
    {{0, 0}, {1, 3}, {4, 8}, {9, 11}, {12, 14}}};

constexpr std::size_t ssao_shader_group(SsaoPass pass) {
    const auto kind = static_cast<std::uint32_t>(pass);
    std::size_t group = 0;
    while (kSsaoShaderGroups.at(group)[1] < kind) {
        ++group;
    }
    return group;
}

constexpr SDL_GPUTextureFormat ssr_gpu_format(SsrFormat format) {
    switch (format) {
    case SsrFormat::RGBA8:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case SsrFormat::RGBA16F:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case SsrFormat::RGBA32F:
        return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    case SsrFormat::R8:
        return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case SsrFormat::R32F:
        break;
    }
    return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
}

// A target's frame before, which SSR reflects: at SSR's size, the camera it was seen from, and
// whether it holds one yet.
struct SsrHistory {
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    SsrCamera camera;
    bool primed = false;
};

// A target's frames before for TAA, at the frame's size: how many it drew with TAA, which picks
// the next jitter, the last one's camera without its jitter, and two results and two motion
// vector textures that frames take turns to draw into, last the pair the last frame drew.
struct TaaHistory {
    std::uint64_t frames = 0;
    Mat4 view_projection{};
    std::array<SDL_GPUTexture*, 2> color{};
    std::array<SDL_GPUTexture*, 2> velocity{};
    std::size_t last = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool primed = false;
};

// A target's frames before for FSR 2: its clock, the last one's camera without its jitter, and two
// of each texture the passes keep between frames, which frames take turns to draw into, last the
// ones the last frame drew: at the display size the accumulate pass's color, locks and luma
// history, at the render size the dilate pass's motion vectors. sizes are the render size, then
// the display size. With FSR 3 (fsr3) the frames keep the accumulate pass's color and lock at the
// display size and, at the render size, the luma, the frames each pixel gathered and the last
// four lumas, with the frame before's jitter.
struct Fsr2Frames {
    Fsr2Clock clock;
    Mat4 view_projection{};
    std::array<SDL_GPUTexture*, 2> color{};
    std::array<SDL_GPUTexture*, 2> lock_status{};
    std::array<SDL_GPUTexture*, 2> luma_history{};
    std::array<SDL_GPUTexture*, 2> dilated_motion{};
    std::array<SDL_GPUTexture*, 2> luma{};
    std::array<SDL_GPUTexture*, 2> accumulation{};
    std::array<SDL_GPUTexture*, 2> lumas{};
    std::array<float, 2> jitter{};
    std::size_t last = 0;
    std::array<std::uint32_t, 4> sizes{};
    bool fsr3 = false;
    // With MetalFX's temporal scaler (metalfx) or the game's temporal upscaler named added (made
    // as temporal), which keep the frames before themselves, the frames keep only the dilate
    // pass's motion vectors, which work the sky's out from the depth.
    bool metalfx = false;
    MetalFxScalerPtr scaler;
    std::string added;
    std::unique_ptr<TemporalUpscaler> temporal;
};

// A target's volumetric fog: its clock and the froxel buffers its frames draw with. Frames take
// turns to write the density into one of two (last the one the last frame wrote, which the next
// reprojects; the blur writes into the other), then gather it into fog, which the meshes and the
// sky read. size is the volume's.
struct FogFrames {
    FogClock clock;
    std::array<SDL_GPUBuffer*, 2> density{};
    SDL_GPUBuffer* scratch = nullptr;
    SDL_GPUBuffer* fog = nullptr;
    std::size_t last = 0;
    std::array<std::uint32_t, 3> size{};
};

constexpr SDL_GPUTextureFormat gpu_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::Bc7:
        return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TextureFormat::Astc4x4:
        return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
    case TextureFormat::Rgba16F:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Rgba8:
        break;
    }
    return kTextureFormat;
}
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
// Threads across and down a group of FSR 2's reconstruct pass, as its [numthreads].
constexpr std::uint32_t kFsr2Threads = 8;

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };
// The compute shaders: running particle steps, turning particles into quads, and numbering what a
// sub-emitter takes; then running 3D particle steps, sorting a 3D system by view depth and
// placing a draw pass's mesh at its particles.
enum class ComputeKind : std::uint8_t { Process, Draw, Emit, Process3D, Sort3D, Mesh3D };
constexpr std::size_t kComputeKinds = 6;

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

// FSR 2's passes that draw, in the order a frame draws them; its reconstruct pass is a compute
// shader.
enum class Fsr2Pass : std::uint8_t { Luma, Dilate, DepthClip, Lock, Accumulate, Rcas };
constexpr std::size_t kFsr2Passes = 6;

// What each pass draws into, in the formats of the CPU twin's values.
constexpr std::array kFsr2LumaTargets{SDL_GPU_TEXTUREFORMAT_R16_FLOAT};
constexpr std::array kFsr2DilateTargets{SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
                                        SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
                                        SDL_GPU_TEXTUREFORMAT_R16_FLOAT};
constexpr std::array kFsr2DepthClipTargets{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                           SDL_GPU_TEXTUREFORMAT_R8G8_UNORM};
constexpr std::array kFsr2LockTargets{SDL_GPU_TEXTUREFORMAT_R16_FLOAT};
constexpr std::array kFsr2AccumulateTargets{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                            SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
                                            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
constexpr std::array kFsr2RcasTargets{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};

// A pass's shaders, the textures it samples, the storage buffers it reads (the depth clip pass
// reads the reconstructed depths) and the formats it draws into.
struct Fsr2PassCode {
    ShaderCode code;
    std::uint32_t samplers = 1;
    std::uint32_t storage_buffers = 0;
    std::span<const SDL_GPUTextureFormat> targets;
};

Fsr2PassCode fsr2_pass_code(Fsr2Pass pass) {
    switch (pass) {
    case Fsr2Pass::Luma:
        return {.code = {.vertex_spirv = shaders::kFsr2LumaVertexSpirv,
                         .fragment_spirv = shaders::kFsr2LumaFragmentSpirv,
                         .vertex_msl = shaders::kFsr2LumaVertexMsl,
                         .fragment_msl = shaders::kFsr2LumaFragmentMsl,
                         .hlsl = shaders::kFsr2LumaHlsl},
                .targets = kFsr2LumaTargets};
    case Fsr2Pass::Dilate:
        return {.code = {.vertex_spirv = shaders::kFsr2DilateVertexSpirv,
                         .fragment_spirv = shaders::kFsr2DilateFragmentSpirv,
                         .vertex_msl = shaders::kFsr2DilateVertexMsl,
                         .fragment_msl = shaders::kFsr2DilateFragmentMsl,
                         .hlsl = shaders::kFsr2DilateHlsl},
                .samplers = 3,
                .targets = kFsr2DilateTargets};
    case Fsr2Pass::DepthClip:
        return {.code = {.vertex_spirv = shaders::kFsr2DepthClipVertexSpirv,
                         .fragment_spirv = shaders::kFsr2DepthClipFragmentSpirv,
                         .vertex_msl = shaders::kFsr2DepthClipVertexMsl,
                         .fragment_msl = shaders::kFsr2DepthClipFragmentMsl,
                         .hlsl = shaders::kFsr2DepthClipHlsl},
                .samplers = 6,
                .storage_buffers = 1,
                .targets = kFsr2DepthClipTargets};
    case Fsr2Pass::Lock:
        return {.code = {.vertex_spirv = shaders::kFsr2LockVertexSpirv,
                         .fragment_spirv = shaders::kFsr2LockFragmentSpirv,
                         .vertex_msl = shaders::kFsr2LockVertexMsl,
                         .fragment_msl = shaders::kFsr2LockFragmentMsl,
                         .hlsl = shaders::kFsr2LockHlsl},
                .targets = kFsr2LockTargets};
    case Fsr2Pass::Accumulate:
        return {.code = {.vertex_spirv = shaders::kFsr2AccumulateVertexSpirv,
                         .fragment_spirv = shaders::kFsr2AccumulateFragmentSpirv,
                         .vertex_msl = shaders::kFsr2AccumulateVertexMsl,
                         .fragment_msl = shaders::kFsr2AccumulateFragmentMsl,
                         .hlsl = shaders::kFsr2AccumulateHlsl},
                .samplers = 8,
                .targets = kFsr2AccumulateTargets};
    case Fsr2Pass::Rcas:
        break;
    }
    return {.code = {.vertex_spirv = shaders::kFsr2RcasVertexSpirv,
                     .fragment_spirv = shaders::kFsr2RcasFragmentSpirv,
                     .vertex_msl = shaders::kFsr2RcasVertexMsl,
                     .fragment_msl = shaders::kFsr2RcasFragmentMsl,
                     .hlsl = shaders::kFsr2RcasHlsl},
            .targets = kFsr2RcasTargets};
}

// FSR 3's passes that draw, in the order a frame draws them; its reconstruct pass is a compute
// shader and its last pass is FSR 2's Rcas.
enum class Fsr3Pass : std::uint8_t {
    Prepare,
    Pyramid,
    Reduce,
    ShadingChange,
    Reactivity,
    Lock,
    Instability,
    Accumulate
};
constexpr std::size_t kFsr3Passes = 8;

// What each pass draws into, in the formats of the CPU twin's values.
constexpr std::array kFsr3PrepareTargets{
    SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
    SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16_FLOAT};
constexpr std::array kFsr3PyramidTargets{SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
                                         SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT};
constexpr std::array kFsr3ReduceTargets{SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT};
constexpr std::array kFsr3ShadingChangeTargets{SDL_GPU_TEXTUREFORMAT_R8_UNORM};
constexpr std::array kFsr3ReactivityTargets{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                            SDL_GPU_TEXTUREFORMAT_R8_UNORM};
constexpr std::array kFsr3LockTargets{SDL_GPU_TEXTUREFORMAT_R8_UNORM};
constexpr std::array kFsr3InstabilityTargets{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                             SDL_GPU_TEXTUREFORMAT_R16_FLOAT};
constexpr std::array kFsr3AccumulateTargets{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT};

Fsr2PassCode fsr3_pass_code(Fsr3Pass pass) {
    switch (pass) {
    case Fsr3Pass::Prepare:
        return {.code = {.vertex_spirv = shaders::kFsr3PrepareVertexSpirv,
                         .fragment_spirv = shaders::kFsr3PrepareFragmentSpirv,
                         .vertex_msl = shaders::kFsr3PrepareVertexMsl,
                         .fragment_msl = shaders::kFsr3PrepareFragmentMsl,
                         .hlsl = shaders::kFsr3PrepareHlsl},
                .samplers = 3,
                .targets = kFsr3PrepareTargets};
    case Fsr3Pass::Pyramid:
        return {.code = {.vertex_spirv = shaders::kFsr3PyramidVertexSpirv,
                         .fragment_spirv = shaders::kFsr3PyramidFragmentSpirv,
                         .vertex_msl = shaders::kFsr3PyramidVertexMsl,
                         .fragment_msl = shaders::kFsr3PyramidFragmentMsl,
                         .hlsl = shaders::kFsr3PyramidHlsl},
                .samplers = 4,
                .targets = kFsr3PyramidTargets};
    case Fsr3Pass::Reduce:
        return {.code = {.vertex_spirv = shaders::kFsr3ReduceVertexSpirv,
                         .fragment_spirv = shaders::kFsr3ReduceFragmentSpirv,
                         .vertex_msl = shaders::kFsr3ReduceVertexMsl,
                         .fragment_msl = shaders::kFsr3ReduceFragmentMsl,
                         .hlsl = shaders::kFsr3ReduceHlsl},
                .targets = kFsr3ReduceTargets};
    case Fsr3Pass::ShadingChange:
        return {.code = {.vertex_spirv = shaders::kFsr3ShadingChangeVertexSpirv,
                         .fragment_spirv = shaders::kFsr3ShadingChangeFragmentSpirv,
                         .vertex_msl = shaders::kFsr3ShadingChangeVertexMsl,
                         .fragment_msl = shaders::kFsr3ShadingChangeFragmentMsl,
                         .hlsl = shaders::kFsr3ShadingChangeHlsl},
                .samplers = 3,
                .targets = kFsr3ShadingChangeTargets};
    case Fsr3Pass::Reactivity:
        return {.code = {.vertex_spirv = shaders::kFsr3ReactivityVertexSpirv,
                         .fragment_spirv = shaders::kFsr3ReactivityFragmentSpirv,
                         .vertex_msl = shaders::kFsr3ReactivityVertexMsl,
                         .fragment_msl = shaders::kFsr3ReactivityFragmentMsl,
                         .hlsl = shaders::kFsr3ReactivityHlsl},
                .samplers = 4,
                .storage_buffers = 1,
                .targets = kFsr3ReactivityTargets};
    case Fsr3Pass::Lock:
        return {.code = {.vertex_spirv = shaders::kFsr3LockVertexSpirv,
                         .fragment_spirv = shaders::kFsr3LockFragmentSpirv,
                         .vertex_msl = shaders::kFsr3LockVertexMsl,
                         .fragment_msl = shaders::kFsr3LockFragmentMsl,
                         .hlsl = shaders::kFsr3LockHlsl},
                .targets = kFsr3LockTargets};
    case Fsr3Pass::Instability:
        return {.code = {.vertex_spirv = shaders::kFsr3InstabilityVertexSpirv,
                         .fragment_spirv = shaders::kFsr3InstabilityFragmentSpirv,
                         .vertex_msl = shaders::kFsr3InstabilityVertexMsl,
                         .fragment_msl = shaders::kFsr3InstabilityFragmentMsl,
                         .hlsl = shaders::kFsr3InstabilityHlsl},
                .samplers = 4,
                .targets = kFsr3InstabilityTargets};
    case Fsr3Pass::Accumulate:
        break;
    }
    return {.code = {.vertex_spirv = shaders::kFsr3AccumulateVertexSpirv,
                     .fragment_spirv = shaders::kFsr3AccumulateFragmentSpirv,
                     .vertex_msl = shaders::kFsr3AccumulateVertexMsl,
                     .fragment_msl = shaders::kFsr3AccumulateFragmentMsl,
                     .hlsl = shaders::kFsr3AccumulateHlsl},
            .samplers = 7,
            .targets = kFsr3AccumulateTargets};
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
        : device_(device), window_(window), owns_video_(owns_video), backend_(backend_of(device)),
          metalfx_(backend_ == Backend::Metal ? metalfx_support(device) : MetalFxSupport{}) {}

    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&&) = delete;
    GpuDevice& operator=(GpuDevice&&) = delete;

    ~GpuDevice() override {
        if (frame_commands_ != nullptr) {
            SDL_SubmitGPUCommandBuffer(frame_commands_);
        }
        SDL_WaitForGPUIdle(device_);
        rays_.reset();
        release_ray_shadows();
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
        for (const MeshPipeline& pipeline : mesh_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline.pipeline);
        }
        for (const auto& [id, mesh] : meshes_) {
            release_mesh(mesh);
        }
        if (depth_ != nullptr) {
            SDL_ReleaseGPUTexture(device_, depth_);
        }
        for (SDL_GPUTexture* atlas :
             {shadow_atlases_[0].texture, shadow_atlases_[1].texture, no_shadows_}) {
            if (atlas != nullptr) {
                SDL_ReleaseGPUTexture(device_, atlas);
            }
        }
        if (shadow_sampler_ != nullptr) {
            SDL_ReleaseGPUSampler(device_, shadow_sampler_);
        }
        for (SDL_GPUGraphicsPipeline* pipeline : shadow_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        for (SDL_GPUComputePipeline* pipeline : compute_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUComputePipeline(device_, pipeline);
            }
        }
        for (const auto& [id, system] : particle_systems_) {
            SDL_ReleaseGPUBuffer(device_, system.buffer);
            if (system.emissions != nullptr) {
                SDL_ReleaseGPUBuffer(device_, system.emissions);
            }
        }
        for (SDL_GPUBuffer* unbound : {no_source_, no_target_}) {
            if (unbound != nullptr) {
                SDL_ReleaseGPUBuffer(device_, unbound);
            }
        }
        if (particle_data_ != nullptr) {
            SDL_ReleaseGPUBuffer(device_, particle_data_);
            SDL_ReleaseGPUTransferBuffer(device_, particle_transfer_);
        }
        for (const StorageBuffer& storage : {cluster_lights_, cluster_cells_, probe_buffer_,
                                             decal_buffer_, decal_atlas_buffer_, fog_items_}) {
            if (storage.buffer != nullptr) {
                SDL_ReleaseGPUBuffer(device_, storage.buffer);
                SDL_ReleaseGPUTransferBuffer(device_, storage.transfer);
            }
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
        for (SDL_GPUShader* shader : mesh_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        if (clustered_shader_ != nullptr) {
            SDL_ReleaseGPUShader(device_, clustered_shader_);
        }
        if (normal_shader_ != nullptr) {
            SDL_ReleaseGPUShader(device_, normal_shader_);
        }
        for (SDL_GPUGraphicsPipeline* pipeline : normal_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        for (SDL_GPUShader* shader : ssao_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& made : ssao_pipelines_) {
            for (const auto& [format, pipeline] : made) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        release_ssao_targets();
        for (SDL_GPUShader* shader : ssr_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : ssr_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        if (ssr_trace_shader_ != nullptr) {
            SDL_ReleaseGPUShader(device_, ssr_trace_shader_);
        }
        if (ssr_trace_pipeline_ != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, ssr_trace_pipeline_);
        }
        release_ssr_targets();
        for (auto& [target, history] : ssr_histories_) {
            pending_release_.push_back(std::exchange(history.texture, nullptr));
        }
        while (!taa_histories_.empty()) {
            forget_taa_history(taa_histories_.back().first);
        }
        if (motion_shaders_[0] != nullptr) {
            SDL_ReleaseGPUShader(device_, motion_shaders_[0]);
            SDL_ReleaseGPUShader(device_, motion_shaders_[1]);
        }
        for (SDL_GPUGraphicsPipeline* pipeline : motion_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        for (SDL_GPUShader* shader : taa_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : taa_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        while (!fsr2_frames_.empty()) {
            forget_fsr2_frames(fsr2_frames_.back().first);
        }
        while (!fog_frames_.empty()) {
            forget_fog_frames(fog_frames_.back().first);
        }
        if (fog_pipeline_ != nullptr) {
            SDL_ReleaseGPUComputePipeline(device_, fog_pipeline_);
        }
        for (SDL_GPUShader* shader : fog_sky_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : fog_sky_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        if (no_fog_ != nullptr) {
            SDL_ReleaseGPUBuffer(device_, no_fog_);
        }
        release_fsr2_textures();
        for (const std::array<SDL_GPUShader*, 2>& stages : fsr2_shaders_) {
            for (SDL_GPUShader* shader : stages) {
                if (shader != nullptr) {
                    SDL_ReleaseGPUShader(device_, shader);
                }
            }
        }
        for (SDL_GPUGraphicsPipeline* pipeline : fsr2_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        if (fsr2_reconstruct_ != nullptr) {
            SDL_ReleaseGPUComputePipeline(device_, fsr2_reconstruct_);
        }
        for (const std::array<SDL_GPUShader*, 2>& stages : fsr3_shaders_) {
            for (SDL_GPUShader* shader : stages) {
                if (shader != nullptr) {
                    SDL_ReleaseGPUShader(device_, shader);
                }
            }
        }
        for (SDL_GPUGraphicsPipeline* pipeline : fsr3_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        if (fsr3_reconstruct_ != nullptr) {
            SDL_ReleaseGPUComputePipeline(device_, fsr3_reconstruct_);
        }
        release_prepass();
        for (SDL_GPUShader* shader : shadow_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (SDL_GPUShader* shader : post_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : post_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        release_post_targets();
        for (SDL_GPUShader* shader : forward_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : forward_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        release_forward_targets();
        for (SDL_GPUShader* shader : sky_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (const auto& [format, pipeline] : sky_pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
        }
        for (SDL_GPUShader* shader : radiance_shaders_) {
            if (shader != nullptr) {
                SDL_ReleaseGPUShader(device_, shader);
            }
        }
        for (SDL_GPUGraphicsPipeline* pipeline : radiance_pipelines_) {
            if (pipeline != nullptr) {
                SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
            }
        }
        if (probe_pipeline_ != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, probe_pipeline_);
        }
        for (SDL_GPUTexture* cube : {radiance_.raw, radiance_.filtered, no_radiance_,
                                     screen_.texture, no_screen_, no_probes_, probe_raw_}) {
            if (cube != nullptr) {
                SDL_ReleaseGPUTexture(device_, cube);
            }
        }
        if (radiance_sampler_ != nullptr) {
            SDL_ReleaseGPUSampler(device_, radiance_sampler_);
        }
        for (const auto& [id, game] : game_shaders_) {
            SDL_ReleaseGPUShader(device_, game.vertex);
            SDL_ReleaseGPUShader(device_, game.fragment);
        }
        // What was forgotten above.
        release_pending();
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
        if (backend_ == Backend::Vulkan) {
            rays_ = make_vulkan_rays(device_);
        }
        if (auto made = make_shaders(ShaderKind::Sprite); !made) {
            return made;
        }
        for (const TextureFormat format : {TextureFormat::Bc7, TextureFormat::Astc4x4}) {
            sampled_.at(static_cast<std::size_t>(format)) = SDL_GPUTextureSupportsFormat(
                device_, gpu_format(format), SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
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
            auto atlas = make_cube(desc.width, probe_levels(desc.width), kHdrFormat, desc.cubes);
            if (!atlas) {
                return atlas.error();
            }
            const auto id = static_cast<TextureId>(next_texture_++);
            textures_[id] = Texture{.gpu = atlas.value(), .desc = desc, .stored = desc.format};
            return id;
        }
        // A format the GPU doesn't sample is kept as RGBA8.
        TextureDesc stored = desc;
        if (!supports(desc.format)) {
            stored.format = TextureFormat::Rgba8;
        }
        auto gpu = create_gpu_texture(stored);
        if (!gpu) {
            return gpu.error();
        }
        // New GPU memory holds whatever was there before, so it starts as transparent black.
        Result<void> filled;
        if (!texels.empty()) {
            filled = upload_texels(gpu.value(), desc, stored, texels);
        } else if (desc.render_target) {
            filled = clear(gpu.value());
        } else {
            const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(texture_bytes(stored)));
            filled = upload(gpu.value(), stored, zeros);
        }
        if (!filled) {
            SDL_ReleaseGPUTexture(device_, gpu.value());
            return filled.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.gpu = gpu.value(), .desc = desc, .stored = stored.format};
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
        TextureDesc stored = desc;
        stored.format = it->second.stored;
        if (radiance_.key && radiance_.key->texture == texture) {
            radiance_.key.reset(); // drawn from the old texels
        }
        return upload_texels(it->second.gpu, desc, stored, texels);
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
        forget_ssr_history(texture);
        forget_taa_history(texture);
        forget_fsr2_frames(texture);
        forget_fog_frames(texture);
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

    Result<ParticlesId> create_particles(std::uint32_t amount, std::uint32_t trail) override {
        if (auto checked = check_particle_amount(amount, trail); !checked) {
            return checked.error();
        }
        if (auto pipeline = compute_pipeline(ComputeKind::Process); !pipeline) {
            return pipeline.error();
        }
        if (auto made = make_unbound_emissions(); !made) {
            return made.error();
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage =
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        info.size = amount * trail * static_cast<std::uint32_t>(sizeof(Particle));
        SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
        if (buffer == nullptr) {
            return sdl_error("creating a system of " + std::to_string(amount * trail) +
                             " particles failed");
        }
        // New GPU memory holds whatever was there before, so the system starts by clearing it.
        if (auto queued = queue_particle_job(
                {.particles = buffer, .amount = amount, .trail = trail, .reset = true});
            !queued) {
            SDL_ReleaseGPUBuffer(device_, buffer);
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
        for (SDL_GPUBuffer* buffer : {it->second.buffer, it->second.emissions}) {
            if (buffer == nullptr) {
                continue;
            }
            if (particle_jobs_.empty()) {
                SDL_ReleaseGPUBuffer(device_, buffer);
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
                SDL_GPUBufferCreateInfo info{};
                info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                info.size = (1 + (system.amount * kEmissionWords)) *
                            static_cast<std::uint32_t>(sizeof(Word));
                system.emissions = SDL_CreateGPUBuffer(device_, &info);
                if (system.emissions == nullptr) {
                    return sdl_error("making room for what " + std::to_string(system.amount) +
                                     " particles start failed");
                }
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
        std::vector<Particle> out(count);
        if (auto read = download(it->second.buffer, std::as_writable_bytes(std::span(out)),
                                 "the particles");
            !read) {
            return read.error();
        }
        return out;
    }

    // 3D systems run on the GPU: process_particles_3d steps them with particles_process_3d.hlsl
    // and fill_particle_mesh places a draw pass's mesh at them with particles_mesh_3d.hlsl,
    // sorting them by view depth with particles_sort_3d.hlsl first when the pass asks.
    Result<Particles3DId> create_particles_3d(std::uint32_t amount) override {
        if (auto checked = check_particle_amount_3d(amount); !checked) {
            return checked.error();
        }
        if (auto pipeline = compute_pipeline(ComputeKind::Process3D); !pipeline) {
            return pipeline.error();
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage =
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        info.size = amount * static_cast<std::uint32_t>(sizeof(Particle3D));
        SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
        if (buffer == nullptr) {
            return sdl_error("creating a system of " + std::to_string(amount) +
                             " 3D particles failed");
        }
        // New GPU memory holds whatever was there before, so the system starts by clearing it.
        if (auto queued = queue_particle_job({.kind = ComputeKind::Process3D,
                                              .particles = buffer,
                                              .amount = amount,
                                              .reset = true});
            !queued) {
            SDL_ReleaseGPUBuffer(device_, buffer);
            return queued.error();
        }
        const auto id = static_cast<Particles3DId>(next_particles_3d_++);
        particle_systems_3d_[id] = {.buffer = buffer, .amount = amount};
        return id;
    }

    void destroy_particles_3d(Particles3DId particles) override {
        const auto it = particle_systems_3d_.find(particles);
        if (it == particle_systems_3d_.end()) {
            return;
        }
        // Jobs waiting for the frame may still use it, so it goes once they are sent.
        for (SDL_GPUBuffer* buffer : {it->second.buffer, it->second.keys}) {
            if (buffer == nullptr) {
                continue;
            }
            if (particle_jobs_.empty()) {
                SDL_ReleaseGPUBuffer(device_, buffer);
            } else {
                pending_buffers_.push_back(buffer);
            }
        }
        particle_systems_3d_.erase(it);
    }

    Result<void> process_particles_3d(Particles3DId particles, const ParticleProcess3D& process,
                                      std::span<const ParticleStep3D> steps) override {
        const auto it = particle_systems_3d_.find(particles);
        if (it == particle_systems_3d_.end()) {
            return unknown_particles_3d();
        }
        if (auto checked = check_particle_process_3d(process, steps); !checked) {
            return checked;
        }
        if (steps.empty()) {
            return {};
        }
        ParticleJob job{.kind = ComputeKind::Process3D,
                        .particles = it->second.buffer,
                        .amount = it->second.amount};
        job.material = material_words_3d(process);
        job.steps = static_cast<std::uint32_t>(particle_words_.size());
        job.step_count = static_cast<std::uint32_t>(steps.size());
        pack_particle_steps_3d(steps, particle_words_);
        return queue_particle_job(job);
    }

    Result<void> fill_particle_mesh(Particles3DId particles, MeshId mesh,
                                    std::span<const Vertex3D> source,
                                    const ParticleMeshDraw3D& draw) override {
        if (in_frame_) {
            return frame_in_progress("fill_particle_mesh");
        }
        const auto system = particle_systems_3d_.find(particles);
        if (system == particle_systems_3d_.end()) {
            return unknown_particles_3d();
        }
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return unknown_mesh(mesh);
        }
        Mesh& found = it->second;
        found.version = ++mesh_versions_;
        const std::uint32_t amount = system->second.amount;
        if (auto checked = check_particle_mesh(amount, source.size(), found.vertex_count);
            !checked) {
            return checked;
        }
        // Without a box to draw in, the vertices are placed on the CPU, which measures them. So
        // they are on Direct3D 12, where SDL's device is lost running particles_mesh_3d.hlsl's
        // pass (on WARP in CI, from its first draw, sorted or not), until that is found.
        if (!draw.has_box() || backend_ == Backend::D3D12) {
            auto live = read_particles_3d(particles);
            if (!live) {
                return live.error();
            }
            std::vector<Vertex3D> placed(found.vertex_count);
            particle_mesh_vertices_3d(live.value(), source, draw, placed);
            return update_mesh(mesh, placed);
        }
        const bool sorted = draw.order == ParticleOrder3D::ViewDepth;
        for (const ComputeKind kind : {ComputeKind::Mesh3D, ComputeKind::Sort3D}) {
            if (kind == ComputeKind::Sort3D && !sorted) {
                continue;
            }
            if (auto pipeline = compute_pipeline(kind); !pipeline) {
                return pipeline.error();
            }
        }
        // The vertices are written where update_mesh writes them, into a buffer the shader can
        // write, the ones drawn before kept for TAA as update_mesh keeps them.
        const auto bytes = static_cast<std::uint32_t>(found.vertex_count * sizeof(Vertex3D));
        if (!found.moved) {
            if (found.previous == nullptr) {
                found.previous = make_mesh_vertices(bytes);
                found.previous_storage = true;
                if (found.previous == nullptr) {
                    return sdl_error("making room for a mesh's vertices failed");
                }
            }
            std::swap(found.vertices, found.previous);
            std::swap(found.storage, found.previous_storage);
            found.moved = true;
        }
        if (!found.storage) {
            SDL_GPUBuffer* writable = make_mesh_vertices(bytes);
            if (writable == nullptr) {
                return sdl_error("making room for a mesh's vertices failed");
            }
            // SDL releases the old buffer once the frames that read it are done.
            SDL_ReleaseGPUBuffer(device_, found.vertices);
            found.vertices = writable;
            found.storage = true;
        }
        ParticleSystem3D& emitter = system->second;
        // Bound, but unread, unless the pass is sorted. Never the particles' own buffer: SDL's
        // Direct3D 12 device moves a buffer bound in two slots of a pass from its default
        // state twice, a barrier that loses the device.
        if (auto made = make_unbound_emissions(); !made) {
            return made;
        }
        SDL_GPUBuffer* keys = no_source_;
        if (sorted) {
            if (emitter.keys == nullptr) {
                SDL_GPUBufferCreateInfo info{};
                info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                info.size =
                    sort_size(amount) * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t));
                emitter.keys = SDL_CreateGPUBuffer(device_, &info);
                if (emitter.keys == nullptr) {
                    return sdl_error("making room to sort " + std::to_string(amount) +
                                     " particles failed");
                }
            }
            keys = emitter.keys;
            SortUniforms3D sort{.amount = amount, .size = sort_size(amount)};
            sort.sort_direction = {draw.sort_direction.x, draw.sort_direction.y,
                                   draw.sort_direction.z, 0.0F};
            particle_jobs_.push_back({.kind = ComputeKind::Sort3D,
                                      .particles = emitter.buffer,
                                      .target = keys,
                                      .sort = sort});
            for (const auto& [j, k] : bitonic_steps(sort.size)) {
                sort.j = j;
                sort.k = k;
                sort.stage = 1;
                particle_jobs_.push_back({.kind = ComputeKind::Sort3D,
                                          .particles = emitter.buffer,
                                          .target = keys,
                                          .sort = sort});
            }
        }
        const auto source_at = static_cast<std::uint32_t>(particle_words_.size());
        pack_particle_vertices_3d(source, particle_words_);
        particle_jobs_.push_back(
            {.kind = ComputeKind::Mesh3D,
             .particles = emitter.buffer,
             .source = keys,
             .target = found.vertices,
             .mesh = mesh_uniforms_3d(draw, amount, static_cast<std::uint32_t>(source.size()),
                                      source_at)});
        if (auto flushed = flush_particle_jobs(); !flushed) {
            return flushed;
        }
        found.bounds = {.low = draw.box_low, .high = draw.box_high};
        return {};
    }

    Result<std::vector<Particle3D>> read_particles_3d(Particles3DId particles) override {
        if (in_frame_) {
            return frame_in_progress("read_particles_3d");
        }
        const auto it = particle_systems_3d_.find(particles);
        if (it == particle_systems_3d_.end()) {
            return unknown_particles_3d();
        }
        std::vector<Particle3D> out(it->second.amount);
        if (auto read = download(it->second.buffer, std::as_writable_bytes(std::span(out)),
                                 "the 3D particles");
            !read) {
            return read.error();
        }
        return out;
    }

    Result<MeshId> create_mesh(std::span<const Vertex3D> vertices,
                               std::span<const std::uint32_t> indices,
                               std::span<const std::span<const std::uint32_t>> lods) override {
        if (auto checked = check_mesh(vertices, indices, lods); !checked) {
            return checked.error();
        }
        Mesh mesh{.vertex_count = vertices.size(), .bounds = mesh_bounds(vertices)};
        const std::vector<std::uint32_t> all =
            mesh_index_buffer(vertices.size(), indices, lods, mesh.ranges);
        indices = all;
        const auto vertex_bytes = static_cast<std::uint32_t>(vertices.size_bytes());
        const auto index_bytes = static_cast<std::uint32_t>(indices.size_bytes());
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = vertex_bytes;
        mesh.vertices = SDL_CreateGPUBuffer(device_, &info);
        info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        info.size = index_bytes;
        mesh.indices = SDL_CreateGPUBuffer(device_, &info);
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                      vertex_bytes + index_bytes);
        auto* mapped =
            mesh.vertices == nullptr || mesh.indices == nullptr || transfer.get() == nullptr
                ? nullptr
                : static_cast<std::uint8_t*>(
                      SDL_MapGPUTransferBuffer(device_, transfer.get(), false));
        if (mapped == nullptr) {
            release_mesh(mesh);
            return sdl_error("making room for a mesh failed");
        }
        std::memcpy(mapped, vertices.data(), vertex_bytes);
        std::memcpy(mapped + vertex_bytes, indices.data(), index_bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            release_mesh(mesh);
            return sdl_error("uploading a mesh failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTransferBufferLocation source{.transfer_buffer = transfer.get(), .offset = 0};
        SDL_GPUBufferRegion destination{.buffer = mesh.vertices, .offset = 0, .size = vertex_bytes};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        source.offset = vertex_bytes;
        destination = {.buffer = mesh.indices, .offset = 0, .size = index_bytes};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            release_mesh(mesh);
            return sdl_error("uploading a mesh failed");
        }
        const auto id = static_cast<MeshId>(next_mesh_++);
        mesh.version = ++mesh_versions_;
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
        const auto bytes = static_cast<std::uint32_t>(vertices.size_bytes());
        // The first update since a frame with TAA drew the mesh keeps the vertices it drew, for
        // the motion of the next one's: the buffers swap, and the new vertices go in the other.
        Mesh& found = it->second;
        found.version = ++mesh_versions_;
        if (!found.moved) {
            if (found.previous == nullptr) {
                SDL_GPUBufferCreateInfo info{};
                info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                info.size = bytes;
                found.previous = SDL_CreateGPUBuffer(device_, &info);
                if (found.previous == nullptr) {
                    return sdl_error("making room for a mesh's vertices failed");
                }
            }
            std::swap(found.vertices, found.previous);
            std::swap(found.storage, found.previous_storage);
            found.moved = true;
        }
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes);
        void* mapped = transfer.get() == nullptr
                           ? nullptr
                           : SDL_MapGPUTransferBuffer(device_, transfer.get(), false);
        if (mapped == nullptr) {
            return sdl_error("making room for a mesh's vertices failed");
        }
        std::memcpy(mapped, vertices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("uploading a mesh's vertices failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTransferBufferLocation source{.transfer_buffer = transfer.get(), .offset = 0};
        const SDL_GPUBufferRegion destination{
            .buffer = it->second.vertices, .offset = 0, .size = bytes};
        // Cycling gives the buffer new memory if a frame sent before still reads it.
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            return sdl_error("uploading a mesh's vertices failed");
        }
        it->second.bounds = mesh_bounds(vertices);
        return {};
    }

    void set_mesh_bounds(MeshId mesh, std::array<float, 3> low,
                         std::array<float, 3> high) override {
        if (const auto it = meshes_.find(mesh); it != meshes_.end()) {
            it->second.bounds = {.low = low, .high = high};
        }
    }

    void destroy_mesh(MeshId mesh) override {
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return;
        }
        if (in_frame_) {
            // The frame's draws still read its buffers when it is sent.
            pending_buffers_.push_back(it->second.vertices);
            pending_buffers_.push_back(it->second.indices);
            if (it->second.previous != nullptr) {
                pending_buffers_.push_back(it->second.previous);
            }
        } else {
            release_mesh(it->second);
        }
        meshes_.erase(it);
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        if (frame.scene) {
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
                frame.lighting, frame.target, [&](TextureId id) { return usable(id); },
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
        // The sky and meshes draw at FSR 2's render size, the post passes at the frame's.
        const auto [scene_width, scene_height] = scene_size(frame, width, height);
        traced_ = false;
        if (frame.scene && frame.scene->method == RenderMethod::ForwardPlus) {
            if (auto ready =
                    prepare_forward(format, width, height, frame.scene->post.glow,
                                    frame.scene->post.auto_exposure, uses_dof(frame.scene->post));
                !ready) {
                SDL_CancelGPUCommandBuffer(commands);
                return ready.error();
            }
            // Depth of field and FSR 2 read the depth buffer, which a frame without meshes clears.
            if (uses_dof(frame.scene->post) || uses_fsr2(*frame.scene)) {
                if (auto depth = depth_target(scene_width, scene_height); !depth) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return depth.error();
                }
            }
            traced_ = traces_shadows(*frame.scene);
            if (uses_prepass(*frame.scene) || traced_) {
                if (auto ready = prepare_prepass(scene_width, scene_height); !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_taa(*frame.scene)) {
                if (auto ready = prepare_taa(frame.target, scene_width, scene_height); !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_fsr2(*frame.scene)) {
                if (auto ready =
                        prepare_fsr2(frame.target, {scene_width, scene_height, width, height},
                                     uses_fsr3(*frame.scene),
                                     metalfx_.temporal && frame.scene->fsr2.has_value() &&
                                         frame.scene->fsr2->metalfx,
                                     frame.scene->fsr2.has_value() ? frame.scene->fsr2->upscaler
                                                                   : std::string{});
                    !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_volumetric_fog(*frame.scene)) {
                if (auto ready =
                        prepare_volumetric_fog(frame.target, world_format(*frame.scene, format),
                                               fog_volume_size(frame.scene->volumetric_fog.settings,
                                                               scene_width, scene_height));
                    !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_ssao(*frame.scene)) {
                if (auto ready =
                        prepare_ssao(scene_width, scene_height, frame.scene->ssao.settings);
                    !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_ssr(*frame.scene)) {
                if (auto ready = prepare_ssr(frame.target, scene_width, scene_height,
                                             frame.scene->ssr.settings);
                    !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (uses_ssao(*frame.scene) || uses_ssr(*frame.scene) || traced_) {
                if (auto ready = prepare_screen(scene_width, scene_height); !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
            if (traced_) {
                if (auto ready = prepare_ray_shadows(scene_width, scene_height); !ready) {
                    SDL_CancelGPUCommandBuffer(commands);
                    return ready.error();
                }
            }
        } else if (frame.scene && post_processes(frame.scene->post)) {
            if (auto ready = prepare_post(format, width, height); !ready) {
                SDL_CancelGPUCommandBuffer(commands);
                return ready.error();
            }
        }
        if (frame.scene && sky_draws(*frame.scene)) {
            // The sky draws where the meshes do.
            if (auto ready =
                    prepare_sky(world_format(*frame.scene, format), scene_width, scene_height);
                !ready) {
                SDL_CancelGPUCommandBuffer(commands);
                return ready.error();
            }
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
        if (!frame.scene || !uses_volumetric_fog(*frame.scene)) {
            forget_fog_frames(frame.target);
        }
        start_frame(frame, commands, target, width, height);
        frame_format_ = format;
        return true;
    }

    // A frame that draws a face of a reflection probe's cube: the world alone, linear, into the
    // Forward+ world target at the atlas's size, which end_frame copies into the raw cube.
    Result<bool> begin_capture(const Frame& frame) {
        if (frame.target != TextureId::None) {
            return make_error(ErrorCode::InvalidArgument,
                              "a capture frame draws into its atlas; its target is None");
        }
        Frame captured = frame;
        if (!captured.scene || !captured.scene->capture) {
            return make_error(ErrorCode::InvalidArgument, "a capture frame needs its capture");
        }
        Scene3D& scene = *captured.scene;
        const ProbeCapture capture = *scene.capture;
        if (auto checked = check_scene(scene); !checked) {
            return checked.error();
        }
        if (auto checked = check_sky(scene, [&](TextureId id) { return usable(id); }); !checked) {
            return checked.error();
        }
        scene.method = RenderMethod::ForwardPlus;
        scene.ssao.enabled = false;
        scene.ssr.enabled = false;
        scene.reflections.probes.clear();
        scene.post = {};
        captured.lighting = {};
        const std::uint32_t size = textures_.at(capture.atlas).desc.width;
        if (auto ready = prepare_forward(kTextureFormat, size, size, false, false, false); !ready) {
            return ready.error();
        }
        // Captures trace their shadows' rays as frames do.
        traced_ = traces_shadows(scene);
        if (traced_) {
            for (const auto& ready : {prepare_prepass(size, size), prepare_screen(size, size),
                                      prepare_ray_shadows(size, size)}) {
                if (!ready) {
                    return ready.error();
                }
            }
        }
        if (sky_draws(scene)) {
            if (auto ready = prepare_sky(kHdrFormat, size, size); !ready) {
                return ready.error();
            }
        }
        if (probe_raw_size_ != size) {
            if (probe_raw_ != nullptr) {
                pending_release_.push_back(std::exchange(probe_raw_, nullptr));
            }
            probe_raw_size_ = 0;
            auto raw = make_cube(size, probe_levels(size), kHdrFormat);
            if (!raw) {
                return raw.error();
            }
            probe_raw_ = raw.value();
            probe_raw_size_ = size;
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("starting a frame failed");
        }
        start_frame(captured, commands, nullptr, size, size);
        frame_format_ = kTextureFormat;
        return true;
    }

    // After a capture's world is drawn: its face copied into the raw cube and, after the sixth,
    // the raw cube's mipmaps and its filtered levels drawn into the atlas's cube.
    Result<void> finish_capture(SDL_GPUCommandBuffer* commands, const ProbeCapture& capture) {
        const std::uint32_t size = probe_raw_size_;
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureLocation from{
            .texture = forward_targets_.world, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
        const SDL_GPUTextureLocation to{
            .texture = probe_raw_, .mip_level = 0, .layer = capture.face, .x = 0, .y = 0, .z = 0};
        SDL_CopyGPUTextureToTexture(copy, &from, &to, size, size, 1, false);
        SDL_EndGPUCopyPass(copy);
        if (capture.face + 1 < kCubeFaces) {
            return {};
        }
        if (probe_pipeline_ == nullptr) {
            if (auto made = make_radiance_shaders(); !made) {
                return made;
            }
            auto pipeline = cube_pipeline(radiance_shaders_[0], radiance_shaders_[1], kHdrFormat);
            if (!pipeline) {
                return pipeline.error();
            }
            probe_pipeline_ = pipeline.value();
        }
        if (auto ready = prepare_radiance_sampler(); !ready) {
            return ready;
        }
        SDL_GPUTexture* atlas = textures_.at(capture.atlas).gpu;
        const std::uint32_t first = capture.cube * kCubeFaces;
        const std::uint32_t levels = probe_levels(size);
        const SDL_GPUTextureSamplerBinding source{.texture = probe_raw_,
                                                  .sampler = radiance_sampler_};
        const auto filter = [&](RadianceUniforms& uniforms, std::uint32_t level) {
            uniforms.options = {1.0F, 0.0F, 0.0F, 0.0F};
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                uniforms.params[0] = static_cast<float>(face);
                SDL_PushGPUFragmentUniformData(
                    commands, 0, &uniforms, static_cast<std::uint32_t>(sizeof(RadianceUniforms)));
                SDL_GPURenderPass* pass = begin_cube_pass(commands, atlas, first + face, level);
                SDL_BindGPUGraphicsPipeline(pass, probe_pipeline_);
                SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(pass);
            }
        };
        // The raw cube's mipmaps, as draw_radiance makes the sky's: each level drawn into the
        // atlas's cube and copied back.
        for (std::uint32_t level = 1; level < levels; ++level) {
            const std::uint32_t n = std::max(size >> level, 1U);
            const auto mipmap = std::make_unique<RadianceUniforms>();
            mipmap->params = {0.0F, 1.0F / static_cast<float>(n), static_cast<float>(level - 1),
                              0.0F};
            filter(*mipmap, level);
            SDL_GPUCopyPass* back = SDL_BeginGPUCopyPass(commands);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                const SDL_GPUTextureLocation level_from{.texture = atlas,
                                                        .mip_level = level,
                                                        .layer = first + face,
                                                        .x = 0,
                                                        .y = 0,
                                                        .z = 0};
                const SDL_GPUTextureLocation level_to{.texture = probe_raw_,
                                                      .mip_level = level,
                                                      .layer = face,
                                                      .x = 0,
                                                      .y = 0,
                                                      .z = 0};
                SDL_CopyGPUTextureToTexture(back, &level_from, &level_to, n, n, 1, false);
            }
            SDL_EndGPUCopyPass(back);
        }
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto uniforms =
                std::make_unique<RadianceUniforms>(radiance_uniforms(size, levels, level));
            filter(*uniforms, level);
        }
        return {};
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

    Result<void> draw_mesh(MeshId mesh, const Mat4& transform, const Material3D& material,
                           std::uint32_t layers, ShadowCasting shadows,
                           const std::optional<Mat4>& previous, std::uint32_t lod) override {
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
        if (auto checked = check_lod(lod, it->second.ranges.size() - 1); !checked) {
            return checked;
        }
        if (frame_commands_ == nullptr || !frame_.scene) {
            return {}; // a skipped frame (check_mesh_draw refused one with no scene)
        }
        // Made at the first draw that needs them, so a failure shows there.
        if (auto depth = depth_target(scene_width_, scene_height_); !depth) {
            return depth.error();
        }
        if (auto ready = prepare_shadows(*frame_.scene); !ready) {
            return ready;
        }
        if (auto ready = prepare_radiance(*frame_.scene); !ready) {
            return ready;
        }
        auto pipeline = mesh_pipeline_for(world_format(*frame_.scene, frame_format_), material,
                                          frame_.scene->method == RenderMethod::ForwardPlus);
        if (!pipeline) {
            return pipeline.error();
        }
        MeshBatch batch{.vertices = it->second.vertices,
                        .indices = it->second.indices,
                        .range = it->second.ranges[lod],
                        .bounds = it->second.bounds,
                        .pipeline = pipeline.value()};
        batch.previous_vertices = it->second.moved ? it->second.previous : it->second.vertices;
        batch.vertex_count = static_cast<std::uint32_t>(it->second.vertex_count);
        batch.version = it->second.version;
        // SSAO's, SSR's, ray-traced shadows', TAA's and FSR 2's prepass draws the opaque meshes'
        // normals, and with TAA or FSR 2 their motion vectors.
        if ((uses_prepass(*frame_.scene) || traced_) &&
            material.transparency != Transparency::Alpha) {
            auto normal = uses_motion_vectors(*frame_.scene) ? motion_pipeline_for(material.cull)
                                                             : normal_pipeline_for(material.cull);
            if (!normal) {
                return normal.error();
            }
            batch.normal_pipeline = normal.value();
        }
        const std::array<MaterialTexture, kMeshTextures> slots = mesh_texture_slots(material);
        for (std::size_t slot = 0; slot < kMeshTextures; ++slot) {
            batch.samplers.at(slot) = binding(slots.at(slot).texture, slots.at(slot).repeat);
        }
        mesh_draws_.push_back({.mesh = mesh,
                               .transform = transform,
                               .material = material,
                               .layers = layers,
                               .shadows = shadows,
                               .previous = previous.value_or(transform),
                               .lod = lod});
        mesh_batches_.push_back(batch);
        ++stats_.draws;
        stats_.triangles += batch.range.count / 3;
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
        // The sky and meshes draw first, in a pass of their own, and the 2D draws go over them.
        // Mesh draws need a scene, so a frame with them has one.
        const bool meshes = frame_.scene && shadow_plan_ && !mesh_draws_.empty();
        const bool forward = frame_.scene && frame_.scene->method == RenderMethod::ForwardPlus;
        const bool capture = frame_.scene && frame_.scene->capture;
        // Depth of field needs the depth buffer cleared, even with nothing drawn over it, and FSR 2
        // upscales the world whatever it holds.
        const bool world = meshes || capture || (frame_.scene && sky_draws(*frame_.scene)) ||
                           (forward && (uses_dof(frame_.scene->post) || uses_fsr2(*frame_.scene)));
        const bool post = !forward && frame_.scene && post_processes(frame_.scene->post);
        if (world) {
            draw_meshes(commands, *frame_.scene, meshes ? &*shadow_plan_ : nullptr);
        }
        // TAA blends frames that draw meshes; one without starts it again.
        if (!meshes && frame_.scene && uses_taa(*frame_.scene)) {
            if (TaaHistory* history = find_taa_history(frame_.target); history != nullptr) {
                history->primed = false;
            }
        }
        if (frame_.scene && frame_.scene->capture) {
            Result<void> captured = finish_capture(commands, *frame_.scene->capture);
            SDL_SubmitGPUCommandBuffer(commands);
            finish_frame();
            return captured;
        }
        if (forward) {
            draw_forward_post(commands, *frame_.scene, !world);
        } else if (post) {
            draw_post(commands, frame_.scene->post, !world);
        }

        SDL_GPUColorTargetInfo color{};
        color.texture = frame_texture_;
        color.clear_color = {frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a};
        color.load_op = world || post || forward ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
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
        for (const auto& [id, texture] : textures_) {
            TextureDesc stored = texture.desc;
            stored.format = texture.stored;
            stats.texture_bytes += texture_bytes(stored);
        }
        return stats;
    }

    [[nodiscard]] MetalFxSupport metalfx() const override { return metalfx_; }
    [[nodiscard]] bool ray_tracing() const override { return rays_ != nullptr; }

    Result<void> metalfx_spatial(TextureId source, TextureId target) override {
        if (!metalfx_.spatial) {
            return RenderDevice::metalfx_spatial(source, target);
        }
        if (in_frame_) {
            return frame_in_progress("metalfx_spatial");
        }
        const auto from = textures_.find(source);
        if (from == textures_.end()) {
            return unknown_texture(source);
        }
        const auto to = textures_.find(target);
        if (to == textures_.end()) {
            return unknown_texture(target);
        }
        const TextureDesc& in = from->second.desc;
        const TextureDesc& out = to->second.desc;
        if (!in.render_target || !out.scaled_into || out.width < in.width ||
            out.height < in.height) {
            return make_error(ErrorCode::InvalidArgument,
                              "MetalFX scales a render target up into a larger one made "
                              "scaled_into");
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("scaling with MetalFX failed");
        }
        if (auto encoded = encode_metalfx_spatial(device_, commands, metalfx_spatial_scaler_,
                                                  from->second.gpu, to->second.gpu, true);
            !encoded) {
            SDL_CancelGPUCommandBuffer(commands);
            return encoded;
        }
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            return sdl_error("scaling with MetalFX failed");
        }
        return {};
    }

private:
    struct Texture {
        SDL_GPUTexture* gpu = nullptr;
        // As it was made, in the format update_texture takes.
        TextureDesc desc;
        // The format on the GPU: desc's, or RGBA8 where the GPU doesn't sample that.
        TextureFormat stored = TextureFormat::Rgba8;
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

    // A mesh's buffers; one without indices is given them in order. Once update_mesh moves its
    // vertices, previous holds them as the last frame with TAA that drew the mesh saw them,
    // until another such frame draws it.
    struct Mesh {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        std::size_t vertex_count = 0;
        // Its own triangles' indices, then each LOD's.
        std::vector<IndexRange> ranges{}; // NOLINT(readability-redundant-member-init)
        Bounds bounds;
        SDL_GPUBuffer* previous = nullptr;
        bool moved = false;
        // Whether vertices and previous can be written by a compute shader, as
        // fill_particle_mesh makes them.
        bool storage = false;
        bool previous_storage = false;
        // Changes whenever its vertices do, so rays build its structure again.
        std::uint64_t version = 0;
    };

    // What a mesh draw binds, found when it is made: its mesh may be destroyed before end_frame.
    struct MeshBatch {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        IndexRange range; // the triangles it draws: the mesh's own or a LOD's
        Bounds bounds;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        // The prepass's, for an opaque mesh in a frame with SSAO, SSR or TAA.
        SDL_GPUGraphicsPipeline* normal_pipeline = nullptr;
        std::array<SDL_GPUTextureSamplerBinding, kMeshTextures> samplers{};
        // Where its vertices were in the frame before, which TAA's prepass reads.
        SDL_GPUBuffer* previous_vertices = nullptr;
        // Its mesh's vertices and their version, for rays.
        std::uint32_t vertex_count = 0;
        std::uint64_t version = 0;
    };

    // A shadow atlas: a depth texture the shadow passes draw into and the mesh pass samples.
    struct ShadowAtlasTexture {
        SDL_GPUTexture* texture = nullptr;
        std::uint32_t size = 0;
    };

    struct MeshPipeline {
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        CullMode cull = CullMode::Back;
        bool blended = false;             // an Alpha material's, which writes no depth
        BlendMode blend = BlendMode::Mix; // when blended
        bool clustered = false;           // Forward+'s fragment stage
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
    };

    // A storage buffer the fragment stage reads, filled each frame through its own transfer
    // buffer and grown by doubling, so a busy game settles on one size.
    struct StorageBuffer {
        SDL_GPUBuffer* buffer = nullptr;
        SDL_GPUTransferBuffer* transfer = nullptr;
        std::uint32_t capacity = 0;
    };

    // A game's shader (create_shader).
    struct GameShader {
        SDL_GPUShader* vertex = nullptr;
        SDL_GPUShader* fragment = nullptr;
    };

    struct ParticleSystem {
        SDL_GPUBuffer* buffer = nullptr; // its particles, then its trails' copies
        std::uint32_t amount = 0;
        std::uint32_t trail = 1;
        // What its particles start in the next system of a chain, made when it is in one.
        SDL_GPUBuffer* emissions = nullptr;
    };

    struct ParticleSystem3D {
        SDL_GPUBuffer* buffer = nullptr; // its particles
        // Its slots' keys sorted by view depth, made when a pass is first drawn in that order.
        SDL_GPUBuffer* keys = nullptr;
        std::uint32_t amount = 0;
    };

    // A compute pass for the GPU to run ahead of the frame's draws, in the order called: steps
    // of a system, numbering what a sub-emitter takes, or a draw turning a system into quads;
    // steps of a 3D system, a step of its sort, or a draw pass's mesh placed at it.
    struct ParticleJob {
        ComputeKind kind = ComputeKind::Process;
        SDL_GPUBuffer* particles = nullptr;
        std::uint32_t amount = 0; // threads: particles, or a draw's quads
        std::uint32_t trail = 1;
        bool reset = false;         // clears every particle first
        std::uint32_t material = 0; // where its process material is in particle_words_
        std::uint32_t steps = 0;    // and its steps, from those already run
        std::uint32_t history = 0;  // how many of them were
        std::uint32_t step_count = 0;
        // The emissions a sub-emitter takes and those its particles make, in a chain; the
        // emissions an Emit job numbers are its target.
        SDL_GPUBuffer* source = nullptr;
        SDL_GPUBuffer* target = nullptr;
        std::uint32_t chain = 0; // kChainTakes and kChainEmits
        EmitUniforms emit{};
        DrawUniforms look{};   // for a draw; its first vertex is in the particle quads
        SortUniforms3D sort{}; // a sort's step, writing the keys in target
        MeshUniforms3D mesh{}; // a mesh, written to target from the keys in source
    };

    static std::size_t shader_index(ShaderKind kind, SDL_GPUShaderStage stage) {
        return (static_cast<std::size_t>(kind) * 2) + (stage == SDL_GPU_SHADERSTAGE_VERTEX ? 0 : 1);
    }

    static std::size_t sampler_index(Filter filter, bool repeat) {
        return (static_cast<std::size_t>(filter) * 2) + (repeat ? 1 : 0);
    }

    // A texture and its sampler; None is white. The texture must exist.
    // Whether a texture can be drawn with or sampled: known, and not a reflection atlas.
    [[nodiscard]] bool usable(TextureId id) const {
        const auto it = textures_.find(id);
        return it != textures_.end() && it->second.desc.cubes == 0;
    }

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
        SDL_GPUComputePipelineCreateInfo info{};
        info.entrypoint = "cs_main";
        info.num_uniform_buffers = 1;
        info.threadcount_x = kParticleThreads;
        info.threadcount_y = 1;
        info.threadcount_z = 1;
        std::span<const unsigned char> spirv;
        std::span<const char* const> pieces;
        std::string_view hlsl;
        switch (kind) {
        case ComputeKind::Process:
            // The steps' words and a sub-emitter's source; the particles and what they start.
            info.num_readonly_storage_buffers = 2;
            info.num_readwrite_storage_buffers = 2;
            spirv = shaders::kParticlesProcessComputeSpirv;
            pieces = shaders::kParticlesProcessComputeMsl;
            hlsl = shaders::kParticlesProcessHlsl;
            break;
        case ComputeKind::Draw:
            info.num_readonly_storage_buffers = 1;
            info.num_readwrite_storage_buffers = 1;
            spirv = shaders::kParticlesDrawComputeSpirv;
            pieces = shaders::kParticlesDrawComputeMsl;
            hlsl = shaders::kParticlesDrawHlsl;
            break;
        case ComputeKind::Emit:
            // The emissions, and the sub-emitter whose first particle keeps its count.
            info.num_readwrite_storage_buffers = 2;
            info.threadcount_x = kEmitThreads;
            spirv = shaders::kParticlesEmitComputeSpirv;
            pieces = shaders::kParticlesEmitComputeMsl;
            hlsl = shaders::kParticlesEmitHlsl;
            break;
        case ComputeKind::Process3D:
            // The steps' words; the particles.
            info.num_readonly_storage_buffers = 1;
            info.num_readwrite_storage_buffers = 1;
            spirv = shaders::kParticlesProcess3DComputeSpirv;
            pieces = shaders::kParticlesProcess3DComputeMsl;
            hlsl = shaders::kParticlesProcess3DHlsl;
            break;
        case ComputeKind::Sort3D:
            // The particles; the keys.
            info.num_readonly_storage_buffers = 1;
            info.num_readwrite_storage_buffers = 1;
            spirv = shaders::kParticlesSort3DComputeSpirv;
            pieces = shaders::kParticlesSort3DComputeMsl;
            hlsl = shaders::kParticlesSort3DHlsl;
            break;
        case ComputeKind::Mesh3D:
            // The source vertices' words, the particles and the keys; the mesh's vertices.
            info.num_readonly_storage_buffers = 3;
            info.num_readwrite_storage_buffers = 1;
            spirv = shaders::kParticlesMesh3DComputeSpirv;
            pieces = shaders::kParticlesMesh3DComputeMsl;
            hlsl = shaders::kParticlesMesh3DHlsl;
            break;
        }
        auto made = create_compute_pipeline(info, spirv, pieces, hlsl, "particle");
        if (!made) {
            return made.error();
        }
        slot = made.value();
        return slot;
    }

    // A compute pipeline with info's bindings and threads, from a shader's code in the format the
    // backend takes, for what.
    Result<SDL_GPUComputePipeline*> create_compute_pipeline(SDL_GPUComputePipelineCreateInfo info,
                                                            std::span<const unsigned char> spirv,
                                                            std::span<const char* const> pieces,
                                                            std::string_view hlsl,
                                                            std::string_view what) {
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
        std::vector<std::uint8_t> compiled;
        std::string msl;
        if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.code = spirv.data();
            info.code_size = spirv.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            msl = joined(pieces);
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.code = reinterpret_cast<const std::uint8_t*>(msl.data());
            info.code_size = msl.size();
        } else if ((formats & SDL_GPU_SHADERFORMAT_DXBC) != 0) {
            auto bytecode = compile_hlsl(hlsl, "cs_main", "cs_5_1");
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
        SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating the " + std::string(what) + " pipeline failed");
        }
        return pipeline;
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

    // Waits for the GPU, then copies the start of buffer into out, what naming it.
    Result<void> download(SDL_GPUBuffer* buffer, std::span<std::byte> out, std::string_view what) {
        const auto size = static_cast<std::uint32_t>(out.size());
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size);
        if (transfer.get() == nullptr) {
            return sdl_error("making room to read " + std::string(what) + " failed");
        }
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return sdl_error("reading " + std::string(what) + " failed");
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUBufferRegion source{.buffer = buffer, .offset = 0, .size = size};
        const SDL_GPUTransferBufferLocation destination{.transfer_buffer = transfer.get(),
                                                        .offset = 0};
        SDL_DownloadFromGPUBuffer(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (fence == nullptr) {
            return sdl_error("reading " + std::string(what) + " failed");
        }
        const bool finished = SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        if (!finished) {
            return sdl_error("waiting for the GPU failed");
        }
        const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer.get(), false);
        if (mapped == nullptr) {
            return sdl_error("reading " + std::string(what) + " failed");
        }
        std::memcpy(out.data(), mapped, size);
        SDL_UnmapGPUTransferBuffer(device_, transfer.get());
        return {};
    }

    // A mesh's vertex buffer of bytes that compute shaders can write too.
    SDL_GPUBuffer* make_mesh_vertices(std::uint32_t bytes) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        info.size = bytes;
        return SDL_CreateGPUBuffer(device_, &info);
    }

    static Error unknown_particles_3d() {
        return make_error(ErrorCode::NotFound, "no such 3D particle system");
    }

    // Where a 3D process material's words are, as material_words.
    std::uint32_t material_words_3d(const ParticleProcess3D& process) {
        if (last_material_3d_ && last_material_3d_->first == process) {
            return last_material_3d_->second;
        }
        const auto at = static_cast<std::uint32_t>(particle_words_.size());
        pack_particle_process_3d(process, particle_words_);
        last_material_3d_.emplace(process, at);
        return at;
    }

    // The buffers a process pass binds where it takes or writes no emissions, made once.
    Result<void> make_unbound_emissions() {
        for (SDL_GPUBuffer** unbound : {&no_source_, &no_target_}) {
            if (*unbound != nullptr) {
                continue;
            }
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
            if (unbound == &no_target_) {
                info.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            }
            info.size = static_cast<std::uint32_t>(sizeof(Word));
            *unbound = SDL_CreateGPUBuffer(device_, &info);
            if (*unbound == nullptr) {
                return sdl_error("creating the particle buffers failed");
            }
        }
        return {};
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
            std::uint32_t groups = (job.amount + kParticleThreads - 1) / kParticleThreads;
            std::array<SDL_GPUStorageBufferReadWriteBinding, 2> outputs{};
            std::uint32_t output_count = 1;
            switch (job.kind) {
            case ComputeKind::Draw:
                outputs[0].buffer = vertex_buffer_;
                outputs[0].cycle = cycle_vertices;
                cycle_vertices = false;
                break;
            case ComputeKind::Process:
                // Passes that write nothing for another system still bind a buffer there.
                outputs[0].buffer = job.particles;
                outputs[1].buffer = job.target != nullptr ? job.target : no_target_;
                output_count = 2;
                break;
            case ComputeKind::Emit:
                outputs[0].buffer = job.target;
                outputs[1].buffer = job.particles;
                output_count = 2;
                groups = 1;
                break;
            case ComputeKind::Process3D:
                outputs[0].buffer = job.particles;
                break;
            case ComputeKind::Sort3D:
                outputs[0].buffer = job.target;
                groups = (job.sort.size + kParticleThreads - 1) / kParticleThreads;
                break;
            case ComputeKind::Mesh3D:
                // Every vertex is written, so a frame sent before may keep reading the old ones.
                outputs[0].buffer = job.target;
                outputs[0].cycle = true;
                break;
            }
            if (job.kind == ComputeKind::Process3D || job.kind == ComputeKind::Sort3D ||
                job.kind == ComputeKind::Mesh3D) {
                dispatch_3d(commands, job, outputs[0], groups);
                continue;
            }
            SDL_GPUComputePass* pass =
                SDL_BeginGPUComputePass(commands, nullptr, 0, outputs.data(), output_count);
            SDL_BindGPUComputePipeline(pass,
                                       compute_pipelines_.at(static_cast<std::size_t>(job.kind)));
            if (job.kind == ComputeKind::Draw) {
                SDL_BindGPUComputeStorageBuffers(pass, 0, &job.particles, 1);
                DrawUniforms uniforms = job.look;
                uniforms.counts[3] += base;
                SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                              static_cast<std::uint32_t>(sizeof(uniforms)));
            } else if (job.kind == ComputeKind::Process) {
                const std::array<SDL_GPUBuffer*, 2> inputs{
                    particle_data_, job.source != nullptr ? job.source : no_source_};
                SDL_BindGPUComputeStorageBuffers(pass, 0, inputs.data(), 2);
                const ProcessUniforms uniforms{.amount = job.amount,
                                               .material = job.material,
                                               .steps = job.steps,
                                               .step_count = job.step_count,
                                               .reset = job.reset ? 1U : 0U,
                                               .chain = job.chain,
                                               .trail = job.trail,
                                               .history = job.history,
                                               .phase_step = particle_phase_step(job.amount)};
                SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                              static_cast<std::uint32_t>(sizeof(uniforms)));
            } else {
                SDL_PushGPUComputeUniformData(commands, 0, &job.emit,
                                              static_cast<std::uint32_t>(sizeof(job.emit)));
            }
            SDL_DispatchGPUCompute(pass, groups, 1, 1);
            SDL_EndGPUComputePass(pass);
        }
    }

    // Records a 3D particle job's compute pass, which writes output.
    void dispatch_3d(SDL_GPUCommandBuffer* commands, const ParticleJob& job,
                     const SDL_GPUStorageBufferReadWriteBinding& output, std::uint32_t groups) {
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &output, 1);
        SDL_BindGPUComputePipeline(pass, compute_pipelines_.at(static_cast<std::size_t>(job.kind)));
        std::array<std::uint32_t, 2> grid{groups, 1};
        if (job.kind == ComputeKind::Process3D) {
            SDL_BindGPUComputeStorageBuffers(pass, 0, &particle_data_, 1);
            const ProcessUniforms3D uniforms{.amount = job.amount,
                                             .material = job.material,
                                             .steps = job.steps,
                                             .step_count = job.step_count,
                                             .reset = job.reset ? 1U : 0U,
                                             .phase_step = particle_phase_step(job.amount)};
            SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                          static_cast<std::uint32_t>(sizeof(uniforms)));
        } else if (job.kind == ComputeKind::Sort3D) {
            SDL_BindGPUComputeStorageBuffers(pass, 0, &job.particles, 1);
            SDL_PushGPUComputeUniformData(commands, 0, &job.sort,
                                          static_cast<std::uint32_t>(sizeof(job.sort)));
        } else {
            const std::array<SDL_GPUBuffer*, 3> inputs{particle_data_, job.particles, job.source};
            SDL_BindGPUComputeStorageBuffers(pass, 0, inputs.data(), 3);
            SDL_PushGPUComputeUniformData(commands, 0, &job.mesh,
                                          static_cast<std::uint32_t>(sizeof(job.mesh)));
            grid = mesh_groups(job.mesh.counts[0] * job.mesh.counts[1]);
        }
        SDL_DispatchGPUCompute(pass, grid[0], grid[1], 1);
        SDL_EndGPUComputePass(pass);
    }

    void clear_particle_jobs() {
        particle_jobs_.clear();
        particle_words_.clear();
        last_material_.reset();
        last_material_3d_.reset();
    }

    void release_pending_buffers() {
        for (SDL_GPUBuffer* buffer : pending_buffers_) {
            SDL_ReleaseGPUBuffer(device_, buffer);
        }
        pending_buffers_.clear();
    }

    void release_mesh(const Mesh& mesh) {
        for (SDL_GPUBuffer* buffer : {mesh.vertices, mesh.indices, mesh.previous}) {
            if (buffer != nullptr) {
                SDL_ReleaseGPUBuffer(device_, buffer);
            }
        }
    }

    // The depth buffer the frame's meshes draw over, made again when the target's size changes.
    Result<SDL_GPUTexture*> depth_target(std::uint32_t width, std::uint32_t height) {
        if (depth_ != nullptr && depth_width_ == width && depth_height_ == height) {
            return depth_;
        }
        if (depth_ != nullptr) {
            pending_release_.push_back(std::exchange(depth_, nullptr));
        }
        // 32-bit floats suit reversed depth best; every GPU without them has 24-bit depth.
        // Depth of field samples it.
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (!SDL_GPUTextureSupportsFormat(device_, info.format, info.type, info.usage)) {
            info.format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        }
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth_ = SDL_CreateGPUTexture(device_, &info);
        if (depth_ == nullptr) {
            return sdl_error("creating the depth buffer failed");
        }
        depth_format_ = info.format;
        depth_width_ = width;
        depth_height_ = height;
        return depth_;
    }

    // The mesh shaders, unless they are made already.
    Result<void> make_mesh_shaders() {
        if (mesh_shaders_[0] != nullptr) {
            return {};
        }
        const std::string vertex_msl = joined(shaders::kMeshVertexMsl);
        const std::string fragment_msl = joined(shaders::kMeshFragmentMsl);
        const std::string hlsl = joined(shaders::kMeshHlsl);
        const ShaderCode code{.vertex_spirv = shaders::kMeshVertexSpirv,
                              .fragment_spirv = shaders::kMeshFragmentSpirv,
                              .vertex_msl = vertex_msl,
                              .fragment_msl = fragment_msl,
                              .hlsl = hlsl};
        auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 2);
        if (!vertex) {
            return vertex.error();
        }
        auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code,
                                        static_cast<std::uint32_t>(kMeshTextures) + 3, 3);
        if (!fragment) {
            SDL_ReleaseGPUShader(device_, vertex.value());
            return fragment.error();
        }
        mesh_shaders_ = {vertex.value(), fragment.value()};
        return {};
    }

    // Forward+'s fragment stage, which reads the clusters' storage buffers, unless it is made
    // already.
    Result<void> make_clustered_shader() {
        if (clustered_shader_ != nullptr) {
            return {};
        }
        const std::string fragment_msl = joined(shaders::kMeshClusteredFragmentMsl);
        const std::string hlsl = joined(shaders::kMeshClusteredHlsl);
        const ShaderCode code{.vertex_spirv = {},
                              .fragment_spirv = shaders::kMeshClusteredFragmentSpirv,
                              .vertex_msl = {},
                              .fragment_msl = fragment_msl,
                              .hlsl = hlsl};
        // The material's textures, the shadow atlases, the radiance, the screen's SSAO and SSR,
        // and the reflection probes' atlas; the lights, clusters, probes, decals, decal atlas and
        // volumetric fog.
        auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code,
                                        static_cast<std::uint32_t>(kMeshTextures) + 5, 3, 6);
        if (!fragment) {
            return fragment.error();
        }
        clustered_shader_ = fragment.value();
        // What the meshes bind in the volumetric fog's place without it.
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = static_cast<std::uint32_t>(sizeof(Float4));
        no_fog_ = SDL_CreateGPUBuffer(device_, &info);
        if (no_fog_ == nullptr) {
            return sdl_error("creating the volumetric fog's buffers failed");
        }
        return {};
    }

    // Vertex3D's fields as the mesh and shadow shaders' inputs.
    static std::array<SDL_GPUVertexAttribute, 6> mesh_attributes() {
        return {{
            {.location = 0,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
             .offset = offsetof(Vertex3D, position)},
            {.location = 1,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
             .offset = offsetof(Vertex3D, normal)},
            {.location = 2,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
             .offset = offsetof(Vertex3D, tangent)},
            {.location = 3,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
             .offset = offsetof(Vertex3D, uv)},
            {.location = 4,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
             .offset = offsetof(Vertex3D, rgba)},
            {.location = 5,
             .buffer_slot = 0,
             .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
             .offset = offsetof(Vertex3D, uv2)},
        }};
    }

    static SDL_GPUCullMode gpu_cull(CullMode cull) {
        switch (cull) {
        case CullMode::Back:
            return SDL_GPU_CULLMODE_BACK;
        case CullMode::Front:
            return SDL_GPU_CULLMODE_FRONT;
        case CullMode::Disabled:
            break;
        }
        return SDL_GPU_CULLMODE_NONE;
    }

    // The pipeline drawing a material's meshes into a target format, made the first time a frame
    // needs it: it culls as the material says, an Alpha material's blends and writes no depth, and
    // a clustered one lights with Forward+'s clusters.
    Result<SDL_GPUGraphicsPipeline*> mesh_pipeline_for(SDL_GPUTextureFormat format,
                                                       const Material3D& material, bool clustered) {
        const bool blended = material.transparency == Transparency::Alpha;
        const BlendMode blend = blended ? material.blend : BlendMode::Mix;
        for (const MeshPipeline& made : mesh_pipelines_) {
            if (made.format == format && made.cull == material.cull && made.blended == blended &&
                made.blend == blend && made.clustered == clustered) {
                return made.pipeline;
            }
        }
        if (auto made = make_mesh_shaders(); !made) {
            return made.error();
        }
        if (clustered) {
            if (auto made = make_clustered_shader(); !made) {
                return made.error();
            }
        }
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(Vertex3D);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        const std::array<SDL_GPUVertexAttribute, 6> attributes = mesh_attributes();
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        if (blended) {
            color.blend_state = blend_state(blend);
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = mesh_shaders_[0];
        info.fragment_shader = clustered ? clustered_shader_ : mesh_shaders_[1];
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = gpu_cull(material.cull);
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        // Reversed depth: nearer is greater.
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = !blended;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depth_format_;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating the 3D pipeline failed");
        }
        mesh_pipelines_.push_back({.format = format,
                                   .cull = material.cull,
                                   .blended = blended,
                                   .blend = blend,
                                   .clustered = clustered,
                                   .pipeline = pipeline});
        return pipeline;
    }

    // The pipeline drawing opaque meshes that cull as cull says into SSAO's normal-roughness
    // prepass, made the first time a frame needs it.
    Result<SDL_GPUGraphicsPipeline*> normal_pipeline_for(CullMode cull) {
        SDL_GPUGraphicsPipeline*& made = normal_pipelines_.at(static_cast<std::size_t>(cull));
        if (made != nullptr) {
            return made;
        }
        if (auto ready = make_mesh_shaders(); !ready) {
            return ready.error();
        }
        if (normal_shader_ == nullptr) {
            const std::string fragment_msl = joined(shaders::kMeshNormalFragmentMsl);
            const std::string hlsl = joined(shaders::kMeshNormalHlsl);
            const ShaderCode code{.vertex_spirv = {},
                                  .fragment_spirv = shaders::kMeshNormalFragmentSpirv,
                                  .vertex_msl = {},
                                  .fragment_msl = fragment_msl,
                                  .hlsl = hlsl};
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code,
                                            static_cast<std::uint32_t>(kMeshTextures) + 3, 3);
            if (!fragment) {
                return fragment.error();
            }
            normal_shader_ = fragment.value();
        }
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(Vertex3D);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        const std::array<SDL_GPUVertexAttribute, 6> attributes = mesh_attributes();
        SDL_GPUColorTargetDescription color{};
        color.format = kNormalFormat;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = mesh_shaders_[0];
        info.fragment_shader = normal_shader_;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = gpu_cull(cull);
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depth_format_;
        made = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (made == nullptr) {
            return sdl_error("creating the normal-roughness prepass's pipeline failed");
        }
        return made;
    }

    // The pipeline drawing opaque meshes that cull as cull says into TAA's prepass, which draws
    // their motion vectors too, from a second vertex buffer of where their vertices were, made
    // the first time a frame needs it.
    Result<SDL_GPUGraphicsPipeline*> motion_pipeline_for(CullMode cull) {
        SDL_GPUGraphicsPipeline*& made = motion_pipelines_.at(static_cast<std::size_t>(cull));
        if (made != nullptr) {
            return made;
        }
        if (motion_shaders_[0] == nullptr) {
            const std::string vertex_msl = joined(shaders::kMeshMotionVertexMsl);
            const std::string fragment_msl = joined(shaders::kMeshMotionFragmentMsl);
            const std::string hlsl = joined(shaders::kMeshMotionHlsl);
            const ShaderCode code{.vertex_spirv = shaders::kMeshMotionVertexSpirv,
                                  .fragment_spirv = shaders::kMeshMotionFragmentSpirv,
                                  .vertex_msl = vertex_msl,
                                  .fragment_msl = fragment_msl,
                                  .hlsl = hlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 2);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code,
                                            static_cast<std::uint32_t>(kMeshTextures) + 3, 3);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            motion_shaders_ = {vertex.value(), fragment.value()};
        }
        std::array<SDL_GPUVertexBufferDescription, 2> buffers{};
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            buffers.at(slot).slot = slot;
            buffers.at(slot).pitch = sizeof(Vertex3D);
            buffers.at(slot).input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        }
        const std::array<SDL_GPUVertexAttribute, 6> mesh = mesh_attributes();
        std::array<SDL_GPUVertexAttribute, 7> attributes{};
        std::ranges::copy(mesh, attributes.begin());
        attributes[6] = {.location = 6,
                         .buffer_slot = 1,
                         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                         .offset = offsetof(Vertex3D, position)};
        std::array<SDL_GPUColorTargetDescription, 2> colors{};
        colors[0].format = kNormalFormat;
        colors[1].format = kVelocityFormat;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = motion_shaders_[0];
        info.fragment_shader = motion_shaders_[1];
        info.vertex_input_state.vertex_buffer_descriptions = buffers.data();
        info.vertex_input_state.num_vertex_buffers = static_cast<std::uint32_t>(buffers.size());
        info.vertex_input_state.vertex_attributes = attributes.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = gpu_cull(cull);
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
        info.target_info.color_target_descriptions = colors.data();
        info.target_info.num_color_targets = static_cast<std::uint32_t>(colors.size());
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depth_format_;
        made = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (made == nullptr) {
            return sdl_error("creating TAA's prepass's pipeline failed");
        }
        return made;
    }

    // The frame's shadow plan, and the atlases, sampler and pipelines it draws with, made at the
    // frame's first mesh draw so a failure shows there.
    Result<void> prepare_shadows(const Scene3D& scene) {
        if (shadow_plan_) {
            return {};
        }
        if (shadow_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
            // 32-bit floats keep the depths the software device keeps; 16 bits are the fallback.
            shadow_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            if (!SDL_GPUTextureSupportsFormat(device_, shadow_format_, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                                  SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
                shadow_format_ = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
            }
        }
        if (shadow_sampler_ == nullptr) {
            // Each tap compares four texels and blends the results, as Godot's shadow sampler.
            SDL_GPUSamplerCreateInfo info{};
            info.min_filter = SDL_GPU_FILTER_LINEAR;
            info.mag_filter = SDL_GPU_FILTER_LINEAR;
            info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.enable_compare = true;
            info.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            shadow_sampler_ = SDL_CreateGPUSampler(device_, &info);
            if (shadow_sampler_ == nullptr) {
                return sdl_error("creating the shadow sampler failed");
            }
        }
        if (no_shadows_ == nullptr) {
            auto made = make_shadow_atlas(1);
            if (!made) {
                return made.error();
            }
            no_shadows_ = made.value();
            // Cleared once, so that it is never read uninitialized.
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
            if (commands == nullptr) {
                return sdl_error("acquiring a command buffer failed");
            }
            const SDL_GPUDepthStencilTargetInfo depth = shadow_target(no_shadows_);
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, nullptr, 0, &depth));
            SDL_SubmitGPUCommandBuffer(commands);
        }
        ShadowPlan plan = plan_shadows(scene, kMaxTextureSize);
        const std::array<std::uint32_t, 2> sizes{plan.directional_size, plan.positional_size};
        for (std::size_t i = 0; i < 2; ++i) {
            ShadowAtlasTexture& atlas = shadow_atlases_.at(i);
            if (sizes.at(i) == 0 || atlas.size == sizes.at(i)) {
                continue;
            }
            if (atlas.texture != nullptr) {
                pending_release_.push_back(std::exchange(atlas.texture, nullptr));
                atlas.size = 0;
            }
            auto made = make_shadow_atlas(sizes.at(i));
            if (!made) {
                return made.error();
            }
            atlas = {.texture = made.value(), .size = sizes.at(i)};
        }
        if (!plan.passes.empty()) {
            if (auto made = make_shadow_pipelines(); !made) {
                return made;
            }
        }
        shadow_plan_ = std::move(plan);
        return {};
    }

    Result<SDL_GPUTexture*> make_shadow_atlas(std::uint32_t size) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = shadow_format_;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = size;
        info.height = size;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
        if (texture == nullptr) {
            return sdl_error("creating a shadow atlas failed");
        }
        return texture;
    }

    // A shadow atlas as a pass's depth target, cleared to the far plane and kept.
    static SDL_GPUDepthStencilTargetInfo shadow_target(SDL_GPUTexture* texture) {
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = texture;
        depth.clear_depth = 0.0F;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.cycle = true;
        return depth;
    }

    // The shadow passes' pipelines, one a cull mode: shaders/shadow.hlsl, depth only.
    Result<void> make_shadow_pipelines() {
        if (shadow_pipelines_[0] != nullptr) {
            return {};
        }
        if (shadow_shaders_[0] == nullptr) {
            const ShaderCode code{.vertex_spirv = shaders::kShadowVertexSpirv,
                                  .fragment_spirv = shaders::kShadowFragmentSpirv,
                                  .vertex_msl = shaders::kShadowVertexMsl,
                                  .fragment_msl = shaders::kShadowFragmentMsl,
                                  .hlsl = shaders::kShadowHlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 2);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 1, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            shadow_shaders_ = {vertex.value(), fragment.value()};
        }
        const std::array<SDL_GPUVertexAttribute, 6> attributes = mesh_attributes();
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(Vertex3D);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        constexpr std::array kCullModes{CullMode::Back, CullMode::Front, CullMode::Disabled};
        for (std::size_t i = 0; i < shadow_pipelines_.size(); ++i) {
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = shadow_shaders_[0];
            info.fragment_shader = shadow_shaders_[1];
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes.data();
            info.vertex_input_state.num_vertex_attributes =
                static_cast<std::uint32_t>(attributes.size());
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = gpu_cull(kCullModes.at(i));
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = shadow_format_;
            shadow_pipelines_.at(i) = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (shadow_pipelines_.at(i) == nullptr) {
                return sdl_error("creating the shadow pipeline failed");
            }
        }
        return {};
    }

    // The frame's shadow passes: each atlas in a pass of its own, each light's view drawn into
    // its rect.
    void draw_shadows(SDL_GPUCommandBuffer* commands, const ShadowPlan& plan) {
        for (std::size_t atlas = 0; atlas < 2; ++atlas) {
            const bool positional = atlas == 1;
            if (std::ranges::none_of(plan.passes, [&](const ShadowPass& pass) {
                    return pass.positional == positional;
                })) {
                continue;
            }
            const SDL_GPUDepthStencilTargetInfo depth =
                shadow_target(shadow_atlases_.at(atlas).texture);
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
            SDL_GPUGraphicsPipeline* bound = nullptr;
            for (const ShadowPass& pass : plan.passes) {
                if (pass.positional != positional) {
                    continue;
                }
                const SDL_GPUViewport viewport{.x = static_cast<float>(pass.rect.x),
                                               .y = static_cast<float>(pass.rect.y),
                                               .w = static_cast<float>(pass.rect.width),
                                               .h = static_cast<float>(pass.rect.height),
                                               .min_depth = 0.0F,
                                               .max_depth = 1.0F};
                SDL_SetGPUViewport(render, &viewport);
                const SDL_Rect scissor{.x = static_cast<int>(pass.rect.x),
                                       .y = static_cast<int>(pass.rect.y),
                                       .w = static_cast<int>(pass.rect.width),
                                       .h = static_cast<int>(pass.rect.height)};
                SDL_SetGPUScissor(render, &scissor);
                SDL_PushGPUVertexUniformData(commands, 0, &pass.uniforms,
                                             static_cast<std::uint32_t>(sizeof(pass.uniforms)));
                for (std::size_t i = 0; i < mesh_draws_.size(); ++i) {
                    const MeshDraw& draw = mesh_draws_[i];
                    const MeshBatch& batch = mesh_batches_[i];
                    if (!shadow_pass_draws(pass, draw, batch.bounds)) {
                        continue;
                    }
                    SDL_GPUGraphicsPipeline* pipeline =
                        shadow_pipelines_.at(static_cast<std::size_t>(shadow_cull(pass, draw)));
                    if (pipeline != bound) {
                        SDL_BindGPUGraphicsPipeline(render, pipeline);
                        bound = pipeline;
                    }
                    const MeshInstanceUniforms instance =
                        mesh_instance_uniforms(draw.transform, draw.material);
                    SDL_PushGPUVertexUniformData(commands, 1, &instance,
                                                 static_cast<std::uint32_t>(sizeof(instance)));
                    const MeshSurfaceUniforms surface =
                        mesh_surface_uniforms(draw.material, draw.layers);
                    SDL_PushGPUFragmentUniformData(commands, 0, &surface,
                                                   static_cast<std::uint32_t>(sizeof(surface)));
                    const SDL_GPUBufferBinding vertices{.buffer = batch.vertices, .offset = 0};
                    SDL_BindGPUVertexBuffers(render, 0, &vertices, 1);
                    const SDL_GPUBufferBinding indices{.buffer = batch.indices, .offset = 0};
                    SDL_BindGPUIndexBuffer(render, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_BindGPUFragmentSamplers(render, 0, batch.samplers.data(), 1);
                    SDL_DrawGPUIndexedPrimitives(render, batch.range.count, 1, batch.range.first, 0,
                                                 0);
                }
            }
            SDL_EndGPURenderPass(render);
        }
    }

    // The frame's meshes: first into the shadow atlases, then in the order mesh_draw_order gives,
    // in a pass that clears the target and a depth buffer to the far plane.
    // The scene's sky, then its meshes with their shadows planned by plan (null with no meshes).
    void draw_meshes(SDL_GPUCommandBuffer* commands, const Scene3D& scene, const ShadowPlan* plan) {
        if (plan != nullptr) {
            draw_shadows(commands, *plan);
        }
        if (radiance_draws_) {
            draw_radiance(commands, scene);
        }
        // TAA's frame: the next jitter, from the camera the target's frame before was seen from
        // (this one's, for its first). FSR 2's: its clock's next frame, whose point of the jitter
        // phases moves the camera, even with no meshes, as the software device's.
        TaaHistory* taa =
            plan != nullptr && uses_taa(scene) ? find_taa_history(frame_.target) : nullptr;
        Fsr2Frames* fsr2 = uses_fsr2(scene) ? find_fsr2_frames(frame_.target) : nullptr;
        const Mat4 view_projection = multiply(scene.projection, scene.camera);
        std::array<float, 2> jitter{};
        MeshCameraUniforms camera = mesh_camera_uniforms(scene);
        if (taa != nullptr) {
            camera =
                taa_camera_uniforms(scene, taa_jitter(taa->frames, scene_width_, scene_height_),
                                    taa->primed ? taa->view_projection : view_projection);
        } else if (fsr2 != nullptr) {
            const std::uint32_t phases = fsr2_phase_count(scene_width_, frame_width_);
            fsr2->clock.advance(phases);
            jitter = camera_jitter(fsr2->clock.frames, phases, scene_width_, scene_height_);
            camera = taa_camera_uniforms(
                scene, jitter, fsr2->clock.frames == 0 ? view_projection : fsr2->view_projection);
        }
        // Whether the prepass draws motion vectors, and where.
        const bool motion = plan != nullptr && (taa != nullptr || fsr2 != nullptr);
        const auto lights = std::make_unique<MeshSceneUniforms>(mesh_scene_uniforms(scene));
        const bool clustered = scene.method == RenderMethod::ForwardPlus;
        if (clustered) {
            set_cluster_uniforms(*lights, clusters_, &decal_atlas_.atlas(), scene.decals.filter);
        }
        const bool ssao = plan != nullptr && uses_ssao(scene);
        const bool ssr = plan != nullptr && uses_ssr(scene);
        const bool traced = plan != nullptr && traced_;
        // What every draw samples besides its material's textures: the shadow atlases, the
        // radiance and, in Forward+, the SSAO and the SSR.
        std::array<SDL_GPUTextureSamplerBinding, kMeshTextures + 5> samplers{};
        std::vector<std::size_t> order;
        if (plan != nullptr) {
            for (std::size_t atlas = 0; atlas < 2; ++atlas) {
                const ShadowAtlasTexture& made = shadow_atlases_.at(atlas);
                const bool drawn =
                    (atlas == 0 ? plan->directional_size : plan->positional_size) != 0;
                samplers.at(kMeshTextures + atlas) = {.texture = drawn ? made.texture : no_shadows_,
                                                      .sampler = shadow_sampler_};
            }
            samplers.at(kMeshTextures + 2) = {.texture = radiance_used(scene) ? radiance_.filtered
                                                                              : no_radiance_,
                                              .sampler = radiance_sampler_};
            samplers.at(kMeshTextures +
                        3) = {.texture = ssao || ssr || traced ? screen_.texture : no_screen_,
                              .sampler = samplers_.at(sampler_index(Filter::Nearest, false))};
            samplers.at(kMeshTextures + 4) = {
                .texture = lights->reflections[0] > 0.0F ? textures_.at(scene.reflections.atlas).gpu
                                                         : no_probes_,
                .sampler = radiance_sampler_};
            order = mesh_draw_order(std::span<const MeshDraw>(mesh_draws_), scene,
                                    [&](std::size_t at) { return mesh_batches_[at].bounds; });
        }
        // Volumetric fog, which frames without meshes go without: its passes, before the meshes
        // and the sky read it.
        FogFrames* fog = clustered && plan != nullptr && uses_volumetric_fog(scene)
                             ? find_fog_frames(frame_.target)
                             : nullptr;
        if (fog != nullptr) {
            set_volumetric_fog(*lights, scene, fog->size);
            draw_volumetric_fog(commands, scene, *lights, *plan, *fog);
        }
        // Draws the meshes in pass, with their prepass pipelines (the opaque ones') for the
        // prepass.
        const auto draw_batches = [&](SDL_GPURenderPass* pass, bool prepass) {
            SDL_PushGPUVertexUniformData(commands, 0, &camera,
                                         static_cast<std::uint32_t>(sizeof(camera)));
            SDL_PushGPUFragmentUniformData(commands, 1, lights.get(),
                                           static_cast<std::uint32_t>(sizeof(MeshSceneUniforms)));
            SDL_PushGPUFragmentUniformData(commands, 2, plan->uniforms.get(),
                                           static_cast<std::uint32_t>(sizeof(MeshShadowUniforms)));
            const std::size_t bound_samplers =
                clustered && !prepass ? kMeshTextures + 5 : kMeshTextures + 3;
            SDL_GPUGraphicsPipeline* bound = nullptr;
            for (const std::size_t i : order) {
                const MeshDraw& draw = mesh_draws_[i];
                const MeshBatch& batch = mesh_batches_[i];
                SDL_GPUGraphicsPipeline* pipeline =
                    prepass ? batch.normal_pipeline : batch.pipeline;
                if (draw.shadows == ShadowCasting::ShadowsOnly || pipeline == nullptr) {
                    continue;
                }
                if (pipeline != bound) {
                    SDL_BindGPUGraphicsPipeline(pass, pipeline);
                    bound = pipeline;
                }
                const MeshInstanceUniforms instance =
                    mesh_instance_uniforms(draw.transform, draw.material, draw.previous);
                SDL_PushGPUVertexUniformData(commands, 1, &instance,
                                             static_cast<std::uint32_t>(sizeof(instance)));
                const MeshSurfaceUniforms surface =
                    mesh_surface_uniforms(draw.material, draw.layers);
                SDL_PushGPUFragmentUniformData(commands, 0, &surface,
                                               static_cast<std::uint32_t>(sizeof(surface)));
                // The motion vectors' prepass reads where the vertices were from a second buffer.
                const std::array vertices{
                    SDL_GPUBufferBinding{.buffer = batch.vertices, .offset = 0},
                    SDL_GPUBufferBinding{.buffer = batch.previous_vertices, .offset = 0}};
                SDL_BindGPUVertexBuffers(pass, 0, vertices.data(), prepass && motion ? 2 : 1);
                const SDL_GPUBufferBinding indices{.buffer = batch.indices, .offset = 0};
                SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                std::copy(batch.samplers.begin(), batch.samplers.end(), samplers.begin());
                SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(),
                                            static_cast<std::uint32_t>(bound_samplers));
                SDL_DrawGPUIndexedPrimitives(pass, batch.range.count, 1, batch.range.first, 0, 0);
            }
        };
        if (ssao || ssr || motion || traced) {
            // The prepass draws the opaque meshes' normals and depth, from which SSAO's passes
            // make the occlusion the meshes are then lit with, SSR's their reflections and the
            // rays their shadows, and with TAA their motion vectors, into the history's next
            // texture, or with FSR 2 into its own.
            std::array<SDL_GPUColorTargetInfo, 2> targets{};
            SDL_GPUColorTargetInfo& normals = targets[0];
            normals.texture = prepass_.normals;
            normals.clear_color = {0.0F, 0.0F, 0.0F, 0.0F};
            normals.load_op = SDL_GPU_LOADOP_CLEAR;
            normals.store_op = SDL_GPU_STOREOP_STORE;
            if (motion) {
                // FSR 2 works the sky's motion vectors out from the depth, as Godot's.
                targets[1] = normals;
                targets[1].texture =
                    taa != nullptr ? taa->velocity.at(1 - taa->last) : fsr2_textures_.velocity;
                if (taa == nullptr) {
                    targets[1].clear_color = {kInvalidMotion, kInvalidMotion, 0.0F, 0.0F};
                }
            }
            SDL_GPUDepthStencilTargetInfo prepass_depth{};
            prepass_depth.texture = prepass_.depth;
            prepass_depth.clear_depth = 0.0F;
            prepass_depth.load_op = SDL_GPU_LOADOP_CLEAR;
            prepass_depth.store_op = SDL_GPU_STOREOP_STORE;
            prepass_depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            prepass_depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            prepass_depth.cycle = true;
            SDL_GPURenderPass* pass =
                SDL_BeginGPURenderPass(commands, targets.data(), motion ? 2 : 1, &prepass_depth);
            draw_batches(pass, true);
            SDL_EndGPURenderPass(pass);
            if (ssao) {
                draw_ssao(commands, scene);
                copy_to_screen(commands, ssao_targets_.textures.at(kSsaoFinal), 0);
            }
            if (traced && trace_ray_shadows(commands, scene, camera.view_projection)) {
                lights->screen[2] = 1.0F;
            }
        } else if (fsr2 != nullptr) {
            // A frame with no meshes is all sky.
            clear_texture(commands, fsr2_textures_.velocity,
                          {kInvalidMotion, kInvalidMotion, 0.0F, 0.0F});
        }
        // The sky, then the meshes over it.
        const auto draw_color = [&] {
            SDL_GPUColorTargetInfo color{};
            color.texture = world_texture(scene);
            color.clear_color = world_clear(scene);
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = depth_;
            depth.clear_depth = 0.0F;
            depth.load_op = SDL_GPU_LOADOP_CLEAR;
            // Kept for depth of field and FSR 2, which read it after.
            depth.store_op = scene.method == RenderMethod::ForwardPlus &&
                                     (uses_dof(scene.post) || fsr2 != nullptr)
                                 ? SDL_GPU_STOREOP_STORE
                                 : SDL_GPU_STOREOP_DONT_CARE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth.cycle = true;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
            if (sky_draws(scene)) {
                const SDL_GPUTextureFormat format = world_format(scene, frame_format_);
                const SkyUniforms sky = sky_uniforms(
                    scene, scene_width_, scene_height_,
                    taa != nullptr ? taa_jitter(taa->frames, scene_width_, scene_height_) : jitter);
                SDL_PushGPUFragmentUniformData(commands, 0, &sky,
                                               static_cast<std::uint32_t>(sizeof(sky)));
                const SDL_GPUTextureSamplerBinding texture = binding(sky_texture(scene.sky), true);
                SDL_BindGPUGraphicsPipeline(pass, sky_pipeline(format));
                SDL_BindGPUFragmentSamplers(pass, 0, &texture, 1);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                if (fog != nullptr) {
                    // The sky seen through the fog.
                    const FogSkyUniforms seen = fog_sky_uniforms(*lights);
                    SDL_PushGPUFragmentUniformData(commands, 0, &seen,
                                                   static_cast<std::uint32_t>(sizeof(seen)));
                    SDL_BindGPUGraphicsPipeline(pass, fog_sky_pipeline(format));
                    SDL_BindGPUFragmentStorageBuffers(pass, 0, &fog->fog, 1);
                    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                }
            }
            if (plan == nullptr) {
                SDL_EndGPURenderPass(pass);
                return;
            }
            if (clustered) {
                const std::array storage{
                    cluster_lights_.buffer,     cluster_cells_.buffer,
                    probe_buffer_.buffer,       decal_buffer_.buffer,
                    decal_atlas_buffer_.buffer, fog != nullptr ? fog->fog : no_fog_};
                SDL_BindGPUFragmentStorageBuffers(pass, 0, storage.data(),
                                                  static_cast<std::uint32_t>(storage.size()));
            }
            draw_batches(pass, false);
            SDL_EndGPURenderPass(pass);
        };
        if (!ssr) {
            draw_color();
            if (taa != nullptr) {
                resolve_taa(commands, *taa, view_projection);
            }
            if (fsr2 != nullptr) {
                upscale_fsr2(commands, scene, *fsr2, jitter);
            }
            return;
        }
        // SSR reflects the frame before, which a target's first frame draws first without it.
        SsrHistory& history = ssr_history(frame_.target);
        const SsrCamera seen = ssr_camera(scene);
        if (!history.primed) {
            lights->ssao[3] = 0.0F;
            draw_color();
            keep_last_frame(commands, scene, history, seen);
            lights->ssao[3] = 1.0F;
        }
        draw_ssr(commands, scene, seen, history);
        copy_to_screen(commands, ssr_targets_.textures.at(kSsrFinal), 1);
        draw_color();
        if (taa != nullptr) {
            resolve_taa(commands, *taa, view_projection);
        }
        if (fsr2 != nullptr) {
            upscale_fsr2(commands, scene, *fsr2, jitter);
        }
        keep_last_frame(commands, scene, history, seen);
    }

    // A cube texture of format (the radiance's by default), size texels across with levels
    // levels, that can be drawn into; with cubes, an array of that many.
    Result<SDL_GPUTexture*> make_cube(std::uint32_t size, std::uint32_t levels,
                                      SDL_GPUTextureFormat format = kRadianceFormat,
                                      std::uint32_t cubes = 0) {
        SDL_GPUTextureCreateInfo info{};
        info.type = cubes > 0 ? SDL_GPU_TEXTURETYPE_CUBE_ARRAY : SDL_GPU_TEXTURETYPE_CUBE;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = size;
        info.height = size;
        info.layer_count_or_depth = kCubeFaces * std::max(cubes, 1U);
        info.num_levels = levels;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
        if (texture == nullptr) {
            return sdl_error("creating a radiance cube " + std::to_string(size) +
                             " texels across failed");
        }
        return texture;
    }

    // A pass drawing into a level of one face of a cube.
    static SDL_GPURenderPass* begin_cube_pass(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* cube,
                                              std::uint32_t face, std::uint32_t level) {
        SDL_GPUColorTargetInfo color{};
        color.texture = cube;
        color.mip_level = level;
        color.layer_or_depth_plane = face;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
        color.store_op = SDL_GPU_STOREOP_STORE;
        return SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    }

    // A pipeline drawing one triangle over a target of format (RGB10A2 by default) with no
    // depth, with shaders.
    Result<SDL_GPUGraphicsPipeline*> cube_pipeline(SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                                   SDL_GPUTextureFormat format = kRadianceFormat) {
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating a radiance pipeline failed");
        }
        return pipeline;
    }

    // The linear sampler with mipmaps that radiance and probe cubes are read with.
    Result<void> prepare_radiance_sampler() {
        if (radiance_sampler_ == nullptr) {
            SDL_GPUSamplerCreateInfo info{};
            info.min_filter = SDL_GPU_FILTER_LINEAR;
            info.mag_filter = SDL_GPU_FILTER_LINEAR;
            info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
            info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.max_lod = 1000.0F;
            radiance_sampler_ = SDL_CreateGPUSampler(device_, &info);
            if (radiance_sampler_ == nullptr) {
                return sdl_error("creating the radiance sampler failed");
            }
        }
        return {};
    }

    // radiance.hlsl's shaders, which filter the sky's radiance and probes' cubes.
    Result<void> make_radiance_shaders() {
        if (radiance_shaders_[0] != nullptr) {
            return {};
        }
        const ShaderCode code{.vertex_spirv = shaders::kRadianceVertexSpirv,
                              .fragment_spirv = shaders::kRadianceFragmentSpirv,
                              .vertex_msl = shaders::kRadianceVertexMsl,
                              .fragment_msl = shaders::kRadianceFragmentMsl,
                              .hlsl = shaders::kRadianceHlsl};
        auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
        if (!vertex) {
            return vertex.error();
        }
        auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 1, 1);
        if (!fragment) {
            SDL_ReleaseGPUShader(device_, vertex.value());
            return fragment.error();
        }
        radiance_shaders_ = {vertex.value(), fragment.value()};
        return {};
    }

    // What the frame's meshes take the radiance from, made at its first mesh draw: the sampler
    // and a black cube for meshes with none, and for a scene with radiance, its shaders,
    // pipelines and cubes, drawn again at end_frame when what they are drawn from changed.
    Result<void> prepare_radiance(const Scene3D& scene) {
        if (radiance_prepared_) {
            return {};
        }
        if (auto ready = prepare_radiance_sampler(); !ready) {
            return ready;
        }
        if (no_radiance_ == nullptr) {
            auto made = make_cube(1, 1);
            if (!made) {
                return made.error();
            }
            no_radiance_ = made.value();
            // Cleared once, so that it is never read uninitialized.
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
            if (commands == nullptr) {
                return sdl_error("acquiring a command buffer failed");
            }
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                SDL_EndGPURenderPass(begin_cube_pass(commands, no_radiance_, face, 0));
            }
            SDL_SubmitGPUCommandBuffer(commands);
        }
        if (no_probes_ == nullptr) {
            // A black atlas of one cube for frames with no probes, and white screen layers for
            // frames with no SSAO or SSR.
            auto probes = make_cube(1, 1, kHdrFormat, 1);
            if (!probes) {
                return probes.error();
            }
            no_probes_ = probes.value();
            auto screen =
                pass_texture(1, 1, kHdrFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, "screen", 2);
            if (!screen) {
                return screen.error();
            }
            no_screen_ = screen.value();
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
            if (commands == nullptr) {
                return sdl_error("acquiring a command buffer failed");
            }
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                SDL_EndGPURenderPass(begin_cube_pass(commands, no_probes_, face, 0));
            }
            for (std::uint32_t layer = 0; layer < 2; ++layer) {
                SDL_GPUColorTargetInfo color{};
                color.texture = no_screen_;
                color.layer_or_depth_plane = layer;
                color.load_op = SDL_GPU_LOADOP_CLEAR;
                color.clear_color = {1.0F, 1.0F, 1.0F, 1.0F};
                color.store_op = SDL_GPU_STOREOP_STORE;
                SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &color, 1, nullptr));
            }
            SDL_SubmitGPUCommandBuffer(commands);
        }
        if (radiance_used(scene)) {
            if (auto made = make_radiance_shaders(); !made) {
                return made;
            }
            // The sky draws into the raw cube with the sky pass's shaders.
            if (auto made = make_sky_shaders(); !made) {
                return made;
            }
            const std::array<std::array<SDL_GPUShader*, 2>, 2> stages{sky_shaders_,
                                                                      radiance_shaders_};
            for (std::size_t i = 0; i < 2; ++i) {
                if (radiance_pipelines_.at(i) == nullptr) {
                    auto pipeline = cube_pipeline(stages.at(i)[0], stages.at(i)[1]);
                    if (!pipeline) {
                        return pipeline.error();
                    }
                    radiance_pipelines_.at(i) = pipeline.value();
                }
            }
            const std::uint32_t size = scene.radiance.size;
            if (radiance_.size != size) {
                for (SDL_GPUTexture** cube : {&radiance_.raw, &radiance_.filtered}) {
                    if (*cube != nullptr) {
                        pending_release_.push_back(std::exchange(*cube, nullptr));
                    }
                }
                radiance_.size = 0;
                radiance_.key.reset();
                auto raw = make_cube(size, radiance_levels(size));
                if (!raw) {
                    return raw.error();
                }
                radiance_.raw = raw.value();
                auto filtered = make_cube(size, radiance_levels(size));
                if (!filtered) {
                    return filtered.error();
                }
                radiance_.filtered = filtered.value();
                radiance_.size = size;
            }
            const RadianceKey key = radiance_key(scene);
            radiance_draws_ = !radiance_.key || !(*radiance_.key == key);
            radiance_.key = key;
        }
        radiance_prepared_ = true;
        return {};
    }

    // The scene's radiance, as Godot's Compatibility renderer draws its radiance map: the sky into
    // each face of the raw cube, its mipmaps, then each level of each face filtered from it.
    void draw_radiance(SDL_GPUCommandBuffer* commands, const Scene3D& scene) {
        const SDL_GPUTextureSamplerBinding texture = binding(radiance_texture(scene), true);
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            const SkyUniforms sky = radiance_face_uniforms(scene, face);
            SDL_PushGPUFragmentUniformData(commands, 0, &sky,
                                           static_cast<std::uint32_t>(sizeof(sky)));
            SDL_GPURenderPass* pass = begin_cube_pass(commands, radiance_.raw, face, 0);
            SDL_BindGPUGraphicsPipeline(pass, radiance_pipelines_[0]);
            SDL_BindGPUFragmentSamplers(pass, 0, &texture, 1);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
        const std::uint32_t levels = radiance_levels(radiance_.size);
        const SDL_GPUTextureSamplerBinding source{.texture = radiance_.raw,
                                                  .sampler = radiance_sampler_};
        // The raw cube's mipmaps, each texel the mean of the four under it, as the WebGPU device
        // makes them: SDL_GenerateMipmapsForGPUTexture turns the up and down faces' mipmaps
        // upside down on Direct3D 12, whose blit shader takes +y and -y the other way round.
        // Each level is drawn into the filtered cube, which the filter passes draw over next, and
        // copied into the raw one, so no pass reads the texture it draws into.
        for (std::uint32_t level = 1; level < levels; ++level) {
            const std::uint32_t n = std::max(radiance_.size >> level, 1U);
            const auto mipmap = std::make_unique<RadianceUniforms>();
            mipmap->params = {0.0F, 1.0F / static_cast<float>(n), static_cast<float>(level - 1),
                              0.0F};
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                mipmap->params[0] = static_cast<float>(face);
                SDL_PushGPUFragmentUniformData(
                    commands, 0, mipmap.get(),
                    static_cast<std::uint32_t>(sizeof(RadianceUniforms)));
                SDL_GPURenderPass* pass =
                    begin_cube_pass(commands, radiance_.filtered, face, level);
                SDL_BindGPUGraphicsPipeline(pass, radiance_pipelines_[1]);
                SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(pass);
            }
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                const SDL_GPUTextureLocation from{.texture = radiance_.filtered,
                                                  .mip_level = level,
                                                  .layer = face,
                                                  .x = 0,
                                                  .y = 0,
                                                  .z = 0};
                SDL_GPUTextureLocation to = from;
                to.texture = radiance_.raw;
                SDL_CopyGPUTextureToTexture(copy, &from, &to, n, n, 1, false);
            }
            SDL_EndGPUCopyPass(copy);
        }
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto uniforms = std::make_unique<RadianceUniforms>(
                radiance_uniforms(radiance_.size, levels, level));
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                uniforms->params[0] = static_cast<float>(face);
                SDL_PushGPUFragmentUniformData(
                    commands, 0, uniforms.get(),
                    static_cast<std::uint32_t>(sizeof(RadianceUniforms)));
                SDL_GPURenderPass* pass =
                    begin_cube_pass(commands, radiance_.filtered, face, level);
                SDL_BindGPUGraphicsPipeline(pass, radiance_pipelines_[1]);
                SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(pass);
            }
        }
    }

    // The format the sky and meshes draw in, for a frame whose target has frame's: half floats for
    // Forward+, whose post passes draw them over the target; RGBA8 for a Compatibility scene the
    // post passes process; otherwise the target's own.
    static SDL_GPUTextureFormat world_format(const Scene3D& scene, SDL_GPUTextureFormat frame) {
        if (scene.method == RenderMethod::ForwardPlus) {
            return kHdrFormat;
        }
        return post_processes(scene.post) ? kTextureFormat : frame;
    }

    // The texture the sky and meshes draw into, and what it is cleared to: for Forward+ the
    // clear color made linear.
    // With FSR 2, its texture at the render size, which it upscales into Forward+'s world.
    [[nodiscard]] SDL_GPUTexture* world_texture(const Scene3D& scene) const {
        if (scene.method == RenderMethod::ForwardPlus) {
            return uses_fsr2(scene) ? fsr2_textures_.color : forward_targets_.world;
        }
        return post_processes(scene.post) ? post_targets_.world : frame_texture_;
    }
    [[nodiscard]] SDL_FColor world_clear(const Scene3D& scene) const {
        if (scene.method == RenderMethod::ForwardPlus) {
            const Float4 clear = forward_clear(frame_.clear);
            return {clear[0], clear[1], clear[2], clear[3]};
        }
        return {frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a};
    }

    // A pipeline drawing one triangle over a target of format with shaders (vertex, then
    // fragment), made once per format into made.
    Result<SDL_GPUGraphicsPipeline*> fullscreen_pipeline(
        std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>>& made,
        const std::array<SDL_GPUShader*, 2>& shaders, SDL_GPUTextureFormat format) {
        for (const auto& [made_format, pipeline] : made) {
            if (made_format == format) {
                return pipeline;
            }
        }
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = shaders[0];
        info.fragment_shader = shaders[1];
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating the post-processing pipeline failed");
        }
        made.emplace_back(format, pipeline);
        return pipeline;
    }

    // Forward+'s post passes: their shaders, their pipeline for each target format, the world's
    // texture of half floats at the frame's size and, with glow, glow's seven levels, with auto
    // exposure the luminance textures of floats, and with depth of field its textures. Made at
    // the start of a frame that needs them.
    Result<void> prepare_forward(SDL_GPUTextureFormat format, std::uint32_t width,
                                 std::uint32_t height, bool glow, bool auto_exposure, bool dof) {
        if (forward_shaders_[0] == nullptr) {
            const ShaderCode code{.vertex_spirv = shaders::kForwardPostVertexSpirv,
                                  .fragment_spirv = shaders::kForwardPostFragmentSpirv,
                                  .vertex_msl = shaders::kForwardPostVertexMsl,
                                  .fragment_msl = shaders::kForwardPostFragmentMsl,
                                  .hlsl = shaders::kForwardPostHlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment =
                make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, kForwardPostTextures, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            forward_shaders_ = {vertex.value(), fragment.value()};
        }
        for (const SDL_GPUTextureFormat needed : {kHdrFormat, format, kLuminanceFormat}) {
            if (auto pipeline = fullscreen_pipeline(forward_pipelines_, forward_shaders_, needed);
                !pipeline) {
                return pipeline.error();
            }
        }
        const auto made = [&](std::uint32_t w, std::uint32_t h,
                              SDL_GPUTextureFormat texture_format =
                                  kHdrFormat) -> Result<SDL_GPUTexture*> {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = texture_format;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = w;
            info.height = h;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
            if (texture == nullptr) {
                return sdl_error("creating a " + std::to_string(w) + "x" + std::to_string(h) +
                                 " texture for Forward+'s post passes failed");
            }
            return texture;
        };
        if (forward_targets_.world == nullptr || forward_targets_.width != width ||
            forward_targets_.height != height) {
            release_forward_targets();
            auto world = made(width, height);
            if (!world) {
                return world.error();
            }
            forward_targets_.world = world.value();
            forward_targets_.width = width;
            forward_targets_.height = height;
        }
        if (glow && forward_targets_.glow[0] == nullptr) {
            const GlowPlan plan = forward_glow_plan(PostProcess{}, width, height);
            for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
                auto level = made(plan.sizes.at(i).width, plan.sizes.at(i).height);
                if (!level) {
                    release_forward_targets();
                    return level.error();
                }
                forward_targets_.glow.at(i) = level.value();
            }
        }
        if (auto_exposure && forward_targets_.brightness == nullptr) {
            for (const PostSize size : luminance_plan(width, height)) {
                auto level = made(size.width, size.height, kLuminanceFormat);
                if (!level) {
                    release_forward_targets();
                    return level.error();
                }
                forward_targets_.luminance.push_back(level.value());
            }
            auto brightness = made(1, 1, kLuminanceFormat);
            if (!brightness) {
                release_forward_targets();
                return brightness.error();
            }
            forward_targets_.brightness = brightness.value();
            forward_targets_.measured = false;
        }
        if (dof && forward_targets_.dof[0] == nullptr) {
            // Sized, Secondary, Half0, Half1 and Out, as DofTexture after World
            const std::uint32_t half_width = std::max(width >> 1U, 1U);
            const std::uint32_t half_height = std::max(height >> 1U, 1U);
            for (std::size_t i = 0; i < forward_targets_.dof.size(); ++i) {
                const bool half = i == 2 || i == 3;
                auto texture = made(half ? half_width : width, half ? half_height : height);
                if (!texture) {
                    release_forward_targets();
                    return texture.error();
                }
                forward_targets_.dof.at(i) = texture.value();
            }
        }
        return {};
    }

    void release_forward_targets() {
        const auto release = [this](SDL_GPUTexture*& texture) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        };
        release(forward_targets_.world);
        for (SDL_GPUTexture*& texture : forward_targets_.glow) {
            release(texture);
        }
        for (SDL_GPUTexture*& texture : forward_targets_.luminance) {
            release(texture);
        }
        forward_targets_.luminance.clear();
        release(forward_targets_.brightness);
        for (SDL_GPUTexture*& texture : forward_targets_.dof) {
            release(texture);
        }
        forward_targets_.measured = false;
        forward_targets_.width = 0;
        forward_targets_.height = 0;
    }

    // A texture of width x height in format that passes draw into and sample, for what.
    Result<SDL_GPUTexture*> pass_texture(std::uint32_t width, std::uint32_t height,
                                         SDL_GPUTextureFormat format,
                                         SDL_GPUTextureUsageFlags usage, std::uint32_t levels,
                                         const char* what, std::uint32_t layers = 1) {
        SDL_GPUTextureCreateInfo info{};
        info.type = layers > 1 ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | usage;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = layers;
        info.num_levels = levels;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
        if (texture == nullptr) {
            return sdl_error("creating a " + std::to_string(width) + "x" + std::to_string(height) +
                             " texture for " + what + " failed");
        }
        return texture;
    }

    // The screen array the mesh pass reads the frame's SSAO and SSR from, at width x height.
    Result<void> prepare_screen(std::uint32_t width, std::uint32_t height) {
        if (screen_.texture != nullptr && screen_.width == width && screen_.height == height) {
            return {};
        }
        if (screen_.texture != nullptr) {
            pending_release_.push_back(std::exchange(screen_.texture, nullptr));
        }
        // Layers 2 and 3 hold the ray-traced shadows on a device with rays.
        auto texture = pass_texture(width, height, kHdrFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1,
                                    "SSAO", rays_ != nullptr ? 4 : 2);
        if (!texture) {
            return texture.error();
        }
        screen_ = {.texture = texture.value(), .width = width, .height = height};
        return {};
    }

    // Whether a frame traces its shadows' rays: on a device with rays, in Forward+ with
    // ray-traced shadows and a light that has a ray slot.
    [[nodiscard]] bool traces_shadows(const Scene3D& scene) const {
        return rays_ != nullptr && !ray_shadow_lights(scene).empty();
    }

    // The two textures of visibilities the rays write, for a frame of width x height.
    Result<void> prepare_ray_shadows(std::uint32_t width, std::uint32_t height) {
        if (ray_shadows_.textures[0] != nullptr && ray_shadows_.width == width &&
            ray_shadows_.height == height) {
            return {};
        }
        release_ray_shadows();
        for (SDL_GPUTexture*& texture : ray_shadows_.textures) {
            auto made =
                pass_texture(width, height, kHdrFormat, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                             1, "ray-traced shadows");
            if (!made) {
                release_ray_shadows();
                return made.error();
            }
            texture = made.value();
        }
        ray_shadows_.width = width;
        ray_shadows_.height = height;
        return {};
    }

    void release_ray_shadows() {
        for (SDL_GPUTexture*& texture : ray_shadows_.textures) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        }
        ray_shadows_.width = 0;
        ray_shadows_.height = 0;
    }

    // After the prepass: the frame's shadows traced through the draws that cast them, into the
    // screen array's layers 2 and 3. False, with the shadow maps left to light the frame, where
    // the rays failed.
    bool trace_ray_shadows(SDL_GPUCommandBuffer* commands, const Scene3D& scene,
                           const Mat4& view_projection) {
        std::vector<RayDraw> draws;
        draws.reserve(mesh_draws_.size());
        for (std::size_t i = 0; i < mesh_draws_.size(); ++i) {
            const MeshDraw& draw = mesh_draws_[i];
            const MeshBatch& batch = mesh_batches_[i];
            if (draw.shadows == ShadowCasting::Off) {
                continue;
            }
            draws.push_back({.mesh = static_cast<std::uint64_t>(draw.mesh),
                             .version = batch.version,
                             .vertices = batch.vertices,
                             .indices = batch.indices,
                             .vertex_count = batch.vertex_count,
                             .first_index = batch.range.first,
                             .index_count = batch.range.count,
                             .transform = draw.transform,
                             .layers = draw.layers});
        }
        const std::vector<RayShadowLight> lights = ray_shadow_lights(scene);
        if (auto traced = rays_->trace_shadows(
                commands, draws, lights,
                ray_shadow_frame(scene, view_projection, scene_width_, scene_height_),
                {.depth = prepass_.depth,
                 .normals = prepass_.normals,
                 .visibility = ray_shadows_.textures});
            !traced) {
            if (!ray_failure_logged_) {
                log::warn("ray-traced shadows failed; shadow maps light the frame",
                          {{"error", traced.error().message}});
                ray_failure_logged_ = true;
            }
            return false;
        }
        copy_to_screen(commands, ray_shadows_.textures[0], 2);
        copy_to_screen(commands, ray_shadows_.textures[1], 3);
        return true;
    }

    // The normal-roughness prepass's normals and depth for a frame of width x height, which
    // SSAO and SSR read. Made at the start of a frame that needs them.
    Result<void> prepare_prepass(std::uint32_t width, std::uint32_t height) {
        // The prepass's depth buffer is made as the color pass's is.
        if (auto depth = depth_target(width, height); !depth) {
            return depth.error();
        }
        if (prepass_.normals != nullptr && prepass_.width == width && prepass_.height == height) {
            return {};
        }
        release_prepass();
        auto normals = pass_texture(width, height, kNormalFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                    1, "the prepass");
        if (!normals) {
            return normals.error();
        }
        prepass_.normals = normals.value();
        auto depth = pass_texture(width, height, depth_format_,
                                  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, 1, "the prepass");
        if (!depth) {
            release_prepass();
            return depth.error();
        }
        prepass_.depth = depth.value();
        prepass_.width = width;
        prepass_.height = height;
        return {};
    }

    void release_prepass() {
        for (SDL_GPUTexture** texture : {&prepass_.normals, &prepass_.depth}) {
            if (*texture != nullptr) {
                pending_release_.push_back(std::exchange(*texture, nullptr));
            }
        }
        prepass_.width = 0;
        prepass_.height = 0;
    }

    // SSAO's shaders, their pipeline for each texture format they draw, and the textures a frame
    // of width x height with settings draws: those of ssao_plan. Made at the start of a frame
    // that needs them, after the prepass's.
    Result<void> prepare_ssao(std::uint32_t width, std::uint32_t height,
                              const SsaoSettings& settings) {
        if (ssao_shaders_[0] == nullptr) {
            const std::string vertex_msl = joined(shaders::kSsaoVertexMsl);
            const std::string fragment_msl = joined(shaders::kSsaoFragmentMsl);
            const std::string whole_hlsl = joined(shaders::kSsaoHlsl);
            ShaderCode code{.vertex_spirv = shaders::kSsaoVertexSpirv,
                            .fragment_spirv = shaders::kSsaoFragmentSpirv,
                            .vertex_msl = vertex_msl,
                            .fragment_msl = fragment_msl,
                            .hlsl = whole_hlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            ssao_split_ = (SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_DXBC) != 0 &&
                          (SDL_GetGPUShaderFormats(device_) &
                           (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL)) == 0;
            const std::size_t count = ssao_split_ ? kSsaoShaderGroups.size() : 1;
            for (std::size_t group = 0; group < count; ++group) {
                std::string hlsl;
                if (ssao_split_) {
                    hlsl = "#define SSAO_PASS_FIRST " +
                           std::to_string(kSsaoShaderGroups.at(group)[0]) +
                           "\n#define SSAO_PASS_LAST " +
                           std::to_string(kSsaoShaderGroups.at(group)[1]) + "\n" + whole_hlsl;
                    code.hlsl = hlsl;
                }
                auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, kSsaoSlots, 1);
                if (!fragment) {
                    SDL_ReleaseGPUShader(device_, vertex.value());
                    for (SDL_GPUShader*& made : ssao_shaders_) {
                        if (made != nullptr) {
                            SDL_ReleaseGPUShader(device_, std::exchange(made, nullptr));
                        }
                    }
                    return fragment.error();
                }
                ssao_shaders_.at(1 + group) = fragment.value();
            }
            ssao_shaders_[0] = vertex.value();
        }
        for (const SsaoStep& step : ssao_plan(settings, width, height).steps) {
            if (auto pipeline = ssao_pipeline(step); !pipeline) {
                return pipeline.error();
            }
        }
        if (ssao_targets_.textures.at(kSsaoFinal) != nullptr && ssao_targets_.width == width &&
            ssao_targets_.height == height && ssao_targets_.settings == settings) {
            return {};
        }
        release_ssao_targets();
        const SsaoPlan plan = ssao_plan(settings, width, height);
        for (std::uint32_t i = 0; i < kSsaoTextures; ++i) {
            const PostSize size = plan.sizes.at(i);
            if (size.width == 0) {
                continue;
            }
            auto texture = pass_texture(size.width, size.height, ssao_target_format(i),
                                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, "SSAO");
            if (!texture) {
                release_ssao_targets();
                return texture.error();
            }
            ssao_targets_.textures.at(i) = texture.value();
        }
        ssao_targets_.width = width;
        ssao_targets_.height = height;
        ssao_targets_.settings = settings;
        return {};
    }

    void release_ssao_targets() {
        const auto release = [this](SDL_GPUTexture*& texture) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        };
        for (SDL_GPUTexture*& texture : ssao_targets_.textures) {
            release(texture);
        }
        ssao_targets_.width = 0;
        ssao_targets_.height = 0;
    }

    // The pipeline drawing an SSAO pass into its target's format.
    Result<SDL_GPUGraphicsPipeline*> ssao_pipeline(const SsaoStep& step) {
        const std::size_t group = ssao_split_ ? ssao_shader_group(step.pass) : 0;
        return fullscreen_pipeline(ssao_pipelines_.at(group),
                                   {ssao_shaders_[0], ssao_shaders_.at(1 + group)},
                                   ssao_target_format(step.target));
    }

    // An SSAO texture's format: the final one's is half floats, so that it copies into the screen
    // array's first layer.
    static SDL_GPUTextureFormat ssao_target_format(std::uint32_t texture) {
        return texture == kSsaoFinal ? kHdrFormat : ssao_gpu_format(ssao_format(texture));
    }

    // Copies a texture of the sky and meshes' size into a layer of the screen array the mesh pass
    // reads.
    void copy_to_screen(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* texture,
                        std::uint32_t layer) const {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureLocation from{
            .texture = texture, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
        const SDL_GPUTextureLocation to{
            .texture = screen_.texture, .mip_level = 0, .layer = layer, .x = 0, .y = 0, .z = 0};
        SDL_CopyGPUTextureToTexture(copy, &from, &to, scene_width_, scene_height_, 1, false);
        SDL_EndGPUCopyPass(copy);
    }

    // ssao.hlsl's passes, from the prepass's depth and normals to the occlusion at the frame's
    // size, in the order ssao_plan gives.
    void draw_ssao(SDL_GPUCommandBuffer* commands, const Scene3D& scene) {
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        const SsaoPlan plan = ssao_plan(scene.ssao.settings, scene_width_, scene_height_);
        for (const SsaoStep& step : plan.steps) {
            const SsaoUniforms uniforms =
                ssao_uniforms(scene.ssao, step, scene.projection, scene_width_, scene_height_);
            // Made in prepare_ssao, at the start of the frame.
            auto pipeline = ssao_pipeline(step);
            if (!pipeline) {
                return;
            }
            SDL_GPUColorTargetInfo color{};
            color.texture = ssao_targets_.textures.at(step.target);
            color.load_op = SDL_GPU_LOADOP_DONT_CARE;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(render, pipeline.value());
            SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                           static_cast<std::uint32_t>(sizeof(uniforms)));
            std::array<SDL_GPUTextureSamplerBinding, kSsaoSlots> bindings{};
            for (std::size_t slot = 0; slot < kSsaoSlots; ++slot) {
                const std::uint32_t index = step.slots.at(slot);
                SDL_GPUTexture* texture = white_;
                if (index == kSsaoDepthBuffer) {
                    texture = prepass_.depth;
                } else if (index == kSsaoNormals) {
                    texture = prepass_.normals;
                } else if (index < kSsaoTextures && ssao_targets_.textures.at(index) != nullptr) {
                    texture = ssao_targets_.textures.at(index);
                }
                bindings.at(slot) = {.texture = texture, .sampler = nearest};
            }
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bindings.size()));
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        }
    }

    // SSR's shaders, their pipeline for each texture format they draw and the trace's, which draws
    // into two, the textures a frame of width x height with settings draws (those of ssr_plan, and
    // Hi-Z's and the reflections' levels copied into one texture each with mipmaps) and target's
    // frame before. Made at the start of a frame that needs them, after the prepass's.
    Result<void> prepare_ssr(TextureId target, std::uint32_t width, std::uint32_t height,
                             const SsrSettings& settings) {
        if (ssr_shaders_[0] == nullptr) {
            const std::string vertex_msl = joined(shaders::kSsrVertexMsl);
            const std::string fragment_msl = joined(shaders::kSsrFragmentMsl);
            const std::string hlsl = joined(shaders::kSsrHlsl);
            const ShaderCode code{.vertex_spirv = shaders::kSsrVertexSpirv,
                                  .fragment_spirv = shaders::kSsrFragmentSpirv,
                                  .vertex_msl = vertex_msl,
                                  .fragment_msl = fragment_msl,
                                  .hlsl = hlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, kSsrSlots, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            ssr_shaders_ = {vertex.value(), fragment.value()};
        }
        if (ssr_trace_shader_ == nullptr) {
            const std::string fragment_msl = joined(shaders::kSsrTraceFragmentMsl);
            const std::string hlsl = joined(shaders::kSsrTraceHlsl);
            const ShaderCode code{.vertex_spirv = {},
                                  .fragment_spirv = shaders::kSsrTraceFragmentSpirv,
                                  .vertex_msl = {},
                                  .fragment_msl = fragment_msl,
                                  .hlsl = hlsl};
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, kSsrSlots, 1);
            if (!fragment) {
                return fragment.error();
            }
            ssr_trace_shader_ = fragment.value();
        }
        for (const SsrFormat format :
             {SsrFormat::R32F, SsrFormat::RGBA8, SsrFormat::RGBA16F, SsrFormat::RGBA32F}) {
            if (auto pipeline =
                    fullscreen_pipeline(ssr_pipelines_, ssr_shaders_, ssr_gpu_format(format));
                !pipeline) {
                return pipeline.error();
            }
        }
        if (ssr_trace_pipeline_ == nullptr) {
            std::array<SDL_GPUColorTargetDescription, 2> colors{};
            colors[0].format = ssr_gpu_format(SsrFormat::RGBA16F);
            colors[1].format = ssr_gpu_format(SsrFormat::R8);
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = ssr_shaders_[0];
            info.fragment_shader = ssr_trace_shader_;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = colors.data();
            info.target_info.num_color_targets = static_cast<std::uint32_t>(colors.size());
            ssr_trace_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (ssr_trace_pipeline_ == nullptr) {
                return sdl_error("creating the SSR trace pipeline failed");
            }
        }
        const SsrPlan plan = ssr_plan(settings, width, height);
        if (ssr_targets_.textures.at(kSsrFinal) == nullptr || ssr_targets_.width != width ||
            ssr_targets_.height != height || !(ssr_targets_.settings == settings)) {
            release_ssr_targets();
            for (std::uint32_t i = 0; i < kSsrTextures; ++i) {
                const PostSize size = plan.sizes.at(i);
                if (size.width == 0) {
                    continue;
                }
                auto texture = pass_texture(size.width, size.height, ssr_gpu_format(ssr_format(i)),
                                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, "SSR");
                if (!texture) {
                    release_ssr_targets();
                    return texture.error();
                }
                ssr_targets_.textures.at(i) = texture.value();
            }
            // The passes' levels are copied in, never drawn into.
            for (const auto& [mips, index] : {std::pair{&ssr_targets_.hiz_mips, kSsrHiz},
                                              std::pair{&ssr_targets_.color_mips, kSsrColor}}) {
                auto texture =
                    pass_texture(plan.size.width, plan.size.height,
                                 ssr_gpu_format(ssr_format(index)), 0, plan.levels, "SSR");
                if (!texture) {
                    release_ssr_targets();
                    return texture.error();
                }
                *mips = texture.value();
            }
            ssr_targets_.width = width;
            ssr_targets_.height = height;
            ssr_targets_.settings = settings;
        }
        SsrHistory& history = ssr_history(target);
        if (history.texture == nullptr || history.width != plan.size.width ||
            history.height != plan.size.height) {
            if (history.texture != nullptr) {
                pending_release_.push_back(std::exchange(history.texture, nullptr));
            }
            auto texture =
                pass_texture(plan.size.width, plan.size.height, ssr_gpu_format(SsrFormat::RGBA16F),
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, "SSR's frame before");
            if (!texture) {
                return texture.error();
            }
            history = {.texture = texture.value(),
                       .width = plan.size.width,
                       .height = plan.size.height,
                       .camera = {},
                       .primed = false};
        }
        return {};
    }

    void release_ssr_targets() {
        const auto release = [this](SDL_GPUTexture*& texture) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        };
        for (SDL_GPUTexture*& texture : ssr_targets_.textures) {
            release(texture);
        }
        release(ssr_targets_.hiz_mips);
        release(ssr_targets_.color_mips);
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
        if (it->second.texture != nullptr) {
            pending_release_.push_back(it->second.texture);
        }
        ssr_histories_.erase(it);
    }

    // TAA's shaders and pipeline, and target's history at width x height, made empty when it has
    // none or it was drawn at another size. Made at the start of a frame that needs them.
    Result<void> prepare_taa(TextureId target, std::uint32_t width, std::uint32_t height) {
        if (taa_shaders_[0] == nullptr) {
            const ShaderCode code{.vertex_spirv = shaders::kTaaVertexSpirv,
                                  .fragment_spirv = shaders::kTaaFragmentSpirv,
                                  .vertex_msl = shaders::kTaaVertexMsl,
                                  .fragment_msl = shaders::kTaaFragmentMsl,
                                  .hlsl = shaders::kTaaHlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 5, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            taa_shaders_ = {vertex.value(), fragment.value()};
        }
        if (auto pipeline = fullscreen_pipeline(taa_pipelines_, taa_shaders_, kHdrFormat);
            !pipeline) {
            return pipeline.error();
        }
        TaaHistory* found = find_taa_history(target);
        if (found == nullptr) {
            found = &taa_histories_.emplace_back(target, TaaHistory{}).second;
        }
        TaaHistory& history = *found;
        if (history.color[0] != nullptr && history.width == width && history.height == height) {
            return {};
        }
        release_taa_textures(history);
        history = TaaHistory{};
        for (std::size_t i = 0; i < 2; ++i) {
            auto color = pass_texture(width, height, kHdrFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                      1, "TAA's frames before");
            if (!color) {
                release_taa_textures(history);
                return color.error();
            }
            history.color.at(i) = color.value();
            auto velocity = pass_texture(width, height, kVelocityFormat,
                                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, "TAA's motion");
            if (!velocity) {
                release_taa_textures(history);
                return velocity.error();
            }
            history.velocity.at(i) = velocity.value();
        }
        history.width = width;
        history.height = height;
        return {};
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
        for (SDL_GPUTexture*& texture : history.color) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        }
        for (SDL_GPUTexture*& texture : history.velocity) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
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

    // TAA's pass: the world, drawn with the frame's jitter, blended with the history by the
    // prepass's depth and motion vectors (the history's next texture) into the history's next
    // result, which is copied over the world; a target's first frame is kept as it is. The
    // history keeps the camera the frame was seen from without its jitter.
    void resolve_taa(SDL_GPUCommandBuffer* commands, TaaHistory& history,
                     const Mat4& view_projection) {
        const std::size_t next = 1 - history.last;
        SDL_GPUTexture* world = forward_targets_.world;
        SDL_GPUTexture* resolved = history.color.at(next);
        // Made in prepare_taa, at the start of the frame.
        auto pipeline = fullscreen_pipeline(taa_pipelines_, taa_shaders_, kHdrFormat);
        if (history.primed && pipeline) {
            SDL_GPUColorTargetInfo color{};
            color.texture = resolved;
            color.load_op = SDL_GPU_LOADOP_DONT_CARE;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(render, pipeline.value());
            const TaaUniforms uniforms = taa_uniforms(scene_width_, scene_height_);
            SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                           static_cast<std::uint32_t>(sizeof(uniforms)));
            SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
            const std::array<SDL_GPUTextureSamplerBinding, 5> bindings{{
                {.texture = world, .sampler = nearest},
                {.texture = prepass_.depth, .sampler = nearest},
                {.texture = history.velocity.at(next), .sampler = nearest},
                {.texture = history.velocity.at(history.last), .sampler = nearest},
                {.texture = history.color.at(history.last), .sampler = nearest},
            }};
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bindings.size()));
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureLocation from{};
        SDL_GPUTextureLocation to{};
        from.texture = history.primed ? resolved : world;
        to.texture = history.primed ? world : resolved;
        SDL_CopyGPUTextureToTexture(copy, &from, &to, scene_width_, scene_height_, 1, false);
        SDL_EndGPUCopyPass(copy);
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

    // FSR 2's shaders and pipelines, or FSR 3's (fsr3), the textures a frame's passes draw into at
    // sizes (the render size, then the display size), and target's frames before, made empty when
    // it has none or they were drawn at other sizes or by the other. Made at the start of a frame
    // that needs them.
    Result<void> prepare_fsr2(TextureId target, const std::array<std::uint32_t, 4>& sizes,
                              bool fsr3, bool metalfx, const std::string& added) {
        // A temporal upscaler outside the device's passes (MetalFX's or the game's) takes the
        // dilated motion vectors and writes the world at the display size.
        metalfx = metalfx && added.empty();
        const bool external = metalfx || !added.empty();
        fsr3 = fsr3 && !external;
        if (auto made = fsr3 ? make_fsr3_pipelines() : make_fsr2_pipelines(); !made) {
            return made;
        }
        const std::uint32_t render_width = sizes[0];
        const std::uint32_t render_height = sizes[1];
        const std::uint32_t width = sizes[2];
        const std::uint32_t height = sizes[3];
        const auto texture = [&](std::uint32_t w, std::uint32_t h, SDL_GPUTextureFormat format,
                                 SDL_GPUTexture*& made,
                                 SDL_GPUTextureUsageFlags usage =
                                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) -> Result<void> {
            auto created = pass_texture(w, h, format, usage, 1, "FSR 2");
            if (!created) {
                return created.error();
            }
            made = created.value();
            return {};
        };
        using Made =
            std::tuple<std::uint32_t, std::uint32_t, SDL_GPUTextureFormat, SDL_GPUTexture**>;
        Fsr2Textures& t = fsr2_textures_;
        if (t.color == nullptr || t.sizes != sizes || t.fsr3 != fsr3 || t.external != external) {
            release_fsr2_textures();
            std::vector<Made> made{
                {render_width, render_height, kHdrFormat, &t.color},
                {render_width, render_height, kVelocityFormat, &t.velocity},
            };
            if (fsr3) {
                const auto [half_width, half_height] = fsr3_half_size(render_width, render_height);
                made.insert(
                    made.end(),
                    {
                        {render_width, render_height, kFsr3PrepareTargets[0], &t.dilated_depth},
                        {render_width, render_height, kFsr3PrepareTargets[1], &t.dilated_motion},
                        {render_width, render_height, kFsr3PrepareTargets[2], &t.farthest_depth},
                        {half_width, half_height, kFsr3PyramidTargets[0], &t.farthest_half},
                        {half_width, half_height, kFsr3ShadingChangeTargets[0], &t.shading_change},
                        {render_width, render_height, kFsr3ReactivityTargets[0], &t.masks},
                        {render_width, render_height, kFsr3InstabilityTargets[1], &t.instability},
                        {width, height, kFsr3LockTargets[0], &t.new_locks},
                    });
                for (std::uint32_t level = 0; level < kFsr3ShadingLevels; ++level) {
                    const auto [level_width, level_height] =
                        fsr3_level_size(render_width, render_height, level);
                    made.emplace_back(level_width, level_height, kFsr3ReduceTargets[0],
                                      &t.levels.at(level));
                }
            } else {
                const auto [mip_width, mip_height] =
                    fsr2_luma_mip_size(render_width, render_height);
                made.insert(
                    made.end(),
                    {
                        {mip_width, mip_height, kFsr2LumaTargets[0], &t.luma_mip},
                        {render_width, render_height, kFsr2DilateTargets[0], &t.dilated_depth},
                        {render_width, render_height, kFsr2DilateTargets[2], &t.lock_luma},
                        {render_width, render_height, kFsr2DepthClipTargets[0], &t.prepared},
                        {render_width, render_height, kFsr2DepthClipTargets[1], &t.reactive},
                        {width, height, kFsr2LockTargets[0], &t.new_locks},
                    });
            }
            for (const auto& [w, h, format, slot] : made) {
                if (auto created = texture(w, h, format, *slot); !created) {
                    release_fsr2_textures();
                    return created;
                }
            }
            // MetalFX or the game's upscaler writes the world at the display size as a compute
            // shader does, then it is copied into the world the post passes read.
            if (external) {
                if (auto created =
                        texture(width, height, kHdrFormat, t.scaled,
                                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                    SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
                    !created) {
                    release_fsr2_textures();
                    return created;
                }
            }
            SDL_GPUBufferCreateInfo buffer{};
            buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                           SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            buffer.size = render_width * render_height * 4;
            t.reconstructed = SDL_CreateGPUBuffer(device_, &buffer);
            if (t.reconstructed == nullptr) {
                release_fsr2_textures();
                return sdl_error("creating FSR 2's depth buffer failed");
            }
            t.sizes = sizes;
            t.fsr3 = fsr3;
            t.external = external;
        }
        Fsr2Frames* found = find_fsr2_frames(target);
        if (found == nullptr) {
            found = &fsr2_frames_.emplace_back(target, Fsr2Frames{}).second;
        }
        Fsr2Frames& frames = *found;
        if (frames.dilated_motion[0] != nullptr || frames.color[0] != nullptr) {
            if (frames.sizes == sizes && frames.fsr3 == fsr3 && frames.metalfx == metalfx &&
                frames.added == added) {
                return {};
            }
        }
        release_fsr2_frames(frames);
        frames = Fsr2Frames{};
        for (std::size_t i = 0; i < 2; ++i) {
            std::vector<Made> made;
            if (external) {
                made = {{render_width, render_height, kFsr2DilateTargets[1],
                         &frames.dilated_motion.at(i)}};
            } else if (fsr3) {
                made = {
                    {width, height, kFsr3AccumulateTargets[0], &frames.color.at(i)},
                    {render_width, render_height, kFsr3PrepareTargets[3], &frames.luma.at(i)},
                    {render_width, render_height, kFsr3ReactivityTargets[1],
                     &frames.accumulation.at(i)},
                    {render_width, render_height, kFsr3InstabilityTargets[0], &frames.lumas.at(i)},
                };
            } else {
                made = {
                    {width, height, kFsr2AccumulateTargets[0], &frames.color.at(i)},
                    {width, height, kFsr2AccumulateTargets[1], &frames.lock_status.at(i)},
                    {width, height, kFsr2AccumulateTargets[2], &frames.luma_history.at(i)},
                    {render_width, render_height, kFsr2DilateTargets[1],
                     &frames.dilated_motion.at(i)},
                };
            }
            for (const auto& [w, h, format, slot] : made) {
                if (auto created = texture(w, h, format, *slot); !created) {
                    release_fsr2_frames(frames);
                    return created;
                }
            }
        }
        frames.sizes = sizes;
        frames.fsr3 = fsr3;
        frames.metalfx = metalfx;
        if (!added.empty()) {
            const std::optional<UpscalerInfo> info = find_upscaler(added);
            if (info && info->make_temporal) {
                frames.temporal = info->make_temporal();
            }
            if (frames.temporal == nullptr) {
                release_fsr2_frames(frames);
                return make_error(ErrorCode::InvalidArgument,
                                  "temporal upscaler " + added + " made nothing");
            }
            frames.added = added;
        }
        return {};
    }

    // FSR 2's shaders and pipelines, unless they are made already.
    Result<void> make_fsr2_pipelines() {
        for (std::size_t i = 0; i < kFsr2Passes; ++i) {
            if (fsr2_pipelines_.at(i) != nullptr) {
                continue;
            }
            const Fsr2PassCode pass = fsr2_pass_code(static_cast<Fsr2Pass>(i));
            std::array<SDL_GPUShader*, 2>& stages = fsr2_shaders_.at(i);
            if (stages[0] == nullptr) {
                auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, pass.code, 0, 0);
                if (!vertex) {
                    return vertex.error();
                }
                auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, pass.code,
                                                pass.samplers, 1, pass.storage_buffers);
                if (!fragment) {
                    SDL_ReleaseGPUShader(device_, vertex.value());
                    return fragment.error();
                }
                stages = {vertex.value(), fragment.value()};
            }
            std::array<SDL_GPUColorTargetDescription, 3> colors{};
            for (std::size_t target = 0; target < pass.targets.size(); ++target) {
                colors.at(target).format = pass.targets[target];
            }
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = stages[0];
            info.fragment_shader = stages[1];
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = colors.data();
            info.target_info.num_color_targets = static_cast<std::uint32_t>(pass.targets.size());
            fsr2_pipelines_.at(i) = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (fsr2_pipelines_.at(i) == nullptr) {
                return sdl_error("creating FSR 2's pipelines failed");
            }
        }
        if (fsr2_reconstruct_ == nullptr) {
            // The dilated depths and motion vectors; the depths as bits.
            SDL_GPUComputePipelineCreateInfo info{};
            info.entrypoint = "cs_main";
            info.num_samplers = 2;
            info.num_readwrite_storage_buffers = 1;
            info.num_uniform_buffers = 1;
            info.threadcount_x = kFsr2Threads;
            info.threadcount_y = kFsr2Threads;
            info.threadcount_z = 1;
            auto made = create_compute_pipeline(info, shaders::kFsr2ReconstructComputeSpirv,
                                                shaders::kFsr2ReconstructComputeMsl,
                                                shaders::kFsr2ReconstructHlsl, "FSR 2 reconstruct");
            if (!made) {
                return made.error();
            }
            fsr2_reconstruct_ = made.value();
        }
        return {};
    }

    // FSR 3's shaders and pipelines, and FSR 2's sharpening pass, unless they are made already.
    Result<void> make_fsr3_pipelines() {
        if (auto made = make_fsr2_pipelines(); !made) {
            return made;
        }
        for (std::size_t i = 0; i < kFsr3Passes; ++i) {
            if (fsr3_pipelines_.at(i) != nullptr) {
                continue;
            }
            const Fsr2PassCode pass = fsr3_pass_code(static_cast<Fsr3Pass>(i));
            std::array<SDL_GPUShader*, 2>& stages = fsr3_shaders_.at(i);
            if (stages[0] == nullptr) {
                auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, pass.code, 0, 0);
                if (!vertex) {
                    return vertex.error();
                }
                auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, pass.code,
                                                pass.samplers, 1, pass.storage_buffers);
                if (!fragment) {
                    SDL_ReleaseGPUShader(device_, vertex.value());
                    return fragment.error();
                }
                stages = {vertex.value(), fragment.value()};
            }
            std::array<SDL_GPUColorTargetDescription, 4> colors{};
            for (std::size_t target = 0; target < pass.targets.size(); ++target) {
                colors.at(target).format = pass.targets[target];
            }
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = stages[0];
            info.fragment_shader = stages[1];
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = colors.data();
            info.target_info.num_color_targets = static_cast<std::uint32_t>(pass.targets.size());
            fsr3_pipelines_.at(i) = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (fsr3_pipelines_.at(i) == nullptr) {
                return sdl_error("creating FSR 3's pipelines failed");
            }
        }
        if (fsr3_reconstruct_ == nullptr) {
            // The dilated depths and motion vectors; the depths as bits.
            SDL_GPUComputePipelineCreateInfo info{};
            info.entrypoint = "cs_main";
            info.num_samplers = 2;
            info.num_readwrite_storage_buffers = 1;
            info.num_uniform_buffers = 1;
            info.threadcount_x = kFsr2Threads;
            info.threadcount_y = kFsr2Threads;
            info.threadcount_z = 1;
            auto made = create_compute_pipeline(info, shaders::kFsr3ReconstructComputeSpirv,
                                                shaders::kFsr3ReconstructComputeMsl,
                                                shaders::kFsr3ReconstructHlsl, "FSR 3 reconstruct");
            if (!made) {
                return made.error();
            }
            fsr3_reconstruct_ = made.value();
        }
        return {};
    }

    // Volumetric fog's pipelines and target's froxel buffers for a volume of size, made at the
    // start of a frame with it.
    Result<void> prepare_volumetric_fog(TextureId target, SDL_GPUTextureFormat format,
                                        const std::array<std::uint32_t, 3>& size) {
        if (fog_pipeline_ == nullptr) {
            // The shadow atlases, the radiance and the VoxelGIs' atlas; the lights, the clusters,
            // the fog's items and the source; the target; the fog's and the shadows' uniforms.
            SDL_GPUComputePipelineCreateInfo info{};
            info.entrypoint = "cs_main";
            info.num_samplers = 4;
            info.num_readonly_storage_buffers = 4;
            info.num_readwrite_storage_buffers = 1;
            info.num_uniform_buffers = 2;
            info.threadcount_x = kFogThreads;
            info.threadcount_y = kFogThreads;
            info.threadcount_z = 1;
            auto made = create_compute_pipeline(info, shaders::kVolumetricFogComputeSpirv,
                                                shaders::kVolumetricFogComputeMsl,
                                                shaders::kVolumetricFogHlsl, "volumetric fog");
            if (!made) {
                return made.error();
            }
            fog_pipeline_ = made.value();
        }
        if (fog_sky_shaders_[0] == nullptr) {
            const ShaderCode code{.vertex_spirv = shaders::kVolumetricFogSkyVertexSpirv,
                                  .fragment_spirv = shaders::kVolumetricFogSkyFragmentSpirv,
                                  .vertex_msl = shaders::kVolumetricFogSkyVertexMsl,
                                  .fragment_msl = shaders::kVolumetricFogSkyFragmentMsl,
                                  .hlsl = shaders::kVolumetricFogSkyHlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 0, 1, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            fog_sky_shaders_ = {vertex.value(), fragment.value()};
        }
        if (fog_sky_pipeline(format) == nullptr) {
            // Over the sky, in the sky's pass: ONE, SRC_ALPHA, keeping the target's alpha.
            SDL_GPUColorTargetDescription color{};
            color.format = format;
            color.blend_state.enable_blend = true;
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = fog_sky_shaders_[0];
            info.fragment_shader = fog_sky_shaders_[1];
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = depth_format_;
            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (pipeline == nullptr) {
                return sdl_error("creating the volumetric fog's sky pipeline failed");
            }
            fog_sky_pipelines_.emplace_back(format, pipeline);
        }
        const auto buffer = [&](std::uint32_t bytes, SDL_GPUBuffer*& made) -> Result<void> {
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                         SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            info.size = bytes;
            made = SDL_CreateGPUBuffer(device_, &info);
            if (made == nullptr) {
                return sdl_error("creating the volumetric fog's buffers failed");
            }
            return {};
        };
        FogFrames* found = find_fog_frames(target);
        if (found == nullptr) {
            found = &fog_frames_.emplace_back(target, FogFrames{}).second;
        }
        FogFrames& frames = *found;
        if (frames.fog != nullptr && frames.size == size) {
            return {};
        }
        release_fog_frames(frames);
        frames = FogFrames{};
        const auto bytes =
            static_cast<std::uint32_t>(std::size_t{size[0]} * size[1] * size[2] * sizeof(Float4));
        for (SDL_GPUBuffer** slot :
             {frames.density.data(), &frames.density[1], &frames.scratch, &frames.fog}) {
            if (auto made = buffer(bytes, *slot); !made) {
                release_fog_frames(frames);
                return made;
            }
        }
        frames.size = size;
        return {};
    }

    // The volumetric fog's sky pipeline for a target format, or null before
    // prepare_volumetric_fog made it.
    SDL_GPUGraphicsPipeline* fog_sky_pipeline(SDL_GPUTextureFormat format) const {
        for (const auto& [made, pipeline] : fog_sky_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        return nullptr;
    }

    // target's volumetric fog, or null when it has none.
    FogFrames* find_fog_frames(TextureId target) {
        for (auto& [id, frames] : fog_frames_) {
            if (id == target) {
                return &frames;
            }
        }
        return nullptr;
    }

    void release_fog_frames(FogFrames& frames) {
        for (SDL_GPUBuffer** buffer :
             {frames.density.data(), &frames.density[1], &frames.scratch, &frames.fog}) {
            if (*buffer != nullptr) {
                pending_buffers_.push_back(std::exchange(*buffer, nullptr));
            }
        }
    }

    void forget_fog_frames(TextureId target) {
        const auto it = std::ranges::find_if(
            fog_frames_, [&](const auto& entry) { return entry.first == target; });
        if (it == fog_frames_.end()) {
            return;
        }
        release_fog_frames(it->second);
        fog_frames_.erase(it);
    }

    // Volumetric fog's passes for a frame whose meshes take lights and plan's shadows: the
    // density, written over the frame before's, which it reprojects; the blur across, then down,
    // into the other density buffer; the gathering into the fog.
    void draw_volumetric_fog(SDL_GPUCommandBuffer* commands, const Scene3D& scene,
                             const MeshSceneUniforms& lights, const ShadowPlan& plan,
                             FogFrames& frames) {
        FogUniforms uniforms =
            volumetric_fog_uniforms(scene, lights, scene_width_, scene_height_, frames.clock);
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        const bool voxel_gi = uniforms.items[1] > 0;
        const std::array<SDL_GPUTextureSamplerBinding, 4> textures{{
            {.texture = plan.directional_size != 0 ? shadow_atlases_[0].texture : no_shadows_,
             .sampler = nearest},
            {.texture = plan.positional_size != 0 ? shadow_atlases_[1].texture : no_shadows_,
             .sampler = nearest},
            {.texture = radiance_used(scene) ? radiance_.filtered : no_radiance_,
             .sampler = radiance_sampler_},
            binding(voxel_gi ? scene.volumetric_fog.voxel_gi_atlas : TextureId::None, false),
        }};
        const std::size_t last = frames.last;
        const std::size_t next = 1 - last;
        const auto pass = [&](FogPass which, SDL_GPUBuffer* source, SDL_GPUBuffer* target,
                              std::uint32_t depth) {
            uniforms.size[3] = static_cast<std::uint32_t>(which);
            const SDL_GPUStorageBufferReadWriteBinding output{
                .buffer = target, .cycle = false, .padding1 = 0, .padding2 = 0, .padding3 = 0};
            SDL_GPUComputePass* compute = SDL_BeginGPUComputePass(commands, nullptr, 0, &output, 1);
            SDL_BindGPUComputePipeline(compute, fog_pipeline_);
            SDL_BindGPUComputeSamplers(compute, 0, textures.data(),
                                       static_cast<std::uint32_t>(textures.size()));
            const std::array inputs{cluster_lights_.buffer, cluster_cells_.buffer,
                                    fog_items_.buffer, source};
            SDL_BindGPUComputeStorageBuffers(compute, 0, inputs.data(),
                                             static_cast<std::uint32_t>(inputs.size()));
            SDL_PushGPUComputeUniformData(commands, 0, &uniforms,
                                          static_cast<std::uint32_t>(sizeof(uniforms)));
            SDL_PushGPUComputeUniformData(commands, 1, plan.uniforms.get(),
                                          static_cast<std::uint32_t>(sizeof(MeshShadowUniforms)));
            SDL_DispatchGPUCompute(compute, (frames.size[0] + kFogThreads - 1) / kFogThreads,
                                   (frames.size[1] + kFogThreads - 1) / kFogThreads, depth);
            SDL_EndGPUComputePass(compute);
        };
        pass(FogPass::Density, frames.density.at(last), frames.density.at(next), frames.size[2]);
        SDL_GPUBuffer* gathered = frames.density.at(next);
        if (scene.volumetric_fog.settings.use_filter) {
            pass(FogPass::FilterX, frames.density.at(next), frames.scratch, frames.size[2]);
            pass(FogPass::FilterY, frames.scratch, frames.density.at(last), frames.size[2]);
            gathered = frames.density.at(last);
        }
        pass(FogPass::Integrate, gathered, frames.fog, 1);
        frames.last = next;
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
        for (std::array<SDL_GPUTexture*, 2>* pair :
             {&frames.color, &frames.lock_status, &frames.luma_history, &frames.dilated_motion,
              &frames.luma, &frames.accumulation, &frames.lumas}) {
            for (SDL_GPUTexture*& texture : *pair) {
                if (texture != nullptr) {
                    pending_release_.push_back(std::exchange(texture, nullptr));
                }
            }
        }
        frames.scaler.reset();
        frames.temporal.reset();
        frames.added.clear();
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
        for (SDL_GPUTexture** texture :
             {&t.color, &t.velocity, &t.luma_mip, &t.dilated_depth, &t.lock_luma, &t.prepared,
              &t.reactive, &t.new_locks, &t.dilated_motion, &t.farthest_depth, &t.farthest_half,
              &t.shading_change, &t.masks, &t.instability, &t.scaled}) {
            if (*texture != nullptr) {
                pending_release_.push_back(std::exchange(*texture, nullptr));
            }
        }
        for (SDL_GPUTexture*& level : t.levels) {
            if (level != nullptr) {
                pending_release_.push_back(std::exchange(level, nullptr));
            }
        }
        if (t.reconstructed != nullptr) {
            pending_buffers_.push_back(std::exchange(t.reconstructed, nullptr));
        }
        t.sizes = {};
    }

    // Clears a texture the frame draws into to transparent black.
    static void clear_texture(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* texture,
                              SDL_FColor clear = {0.0F, 0.0F, 0.0F, 0.0F}) {
        SDL_GPUColorTargetInfo color{};
        color.texture = texture;
        color.clear_color = clear;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &color, 1, nullptr));
    }

    // FSR 2's passes, as Godot's FSR 2 effect dispatches them: the world, drawn at the render size
    // with jitter, upscaled by the depth buffer, the prepass's motion vectors and the target's
    // frames before into Forward+'s world at the frame's size, which the post passes read. The
    // frames before keep the camera the frame was seen from without its jitter; a reset frame
    // starts them at 0, as the software device's.
    void upscale_fsr2(SDL_GPUCommandBuffer* commands, const Scene3D& scene, Fsr2Frames& frames,
                      const std::array<float, 2>& jitter) {
        if (frames.fsr3) {
            upscale_fsr3(commands, scene, frames, jitter);
            return;
        }
        if (frames.metalfx || frames.temporal != nullptr) {
            upscale_external(commands, scene, frames, jitter);
            return;
        }
        const std::size_t last = frames.last;
        const std::size_t next = 1 - last;
        const Mat4 view_projection = multiply(scene.projection, scene.camera);
        const Fsr2Uniforms uniforms = fsr2_uniforms(
            {.render_width = scene_width_,
             .render_height = scene_height_,
             .display_width = frame_width_,
             .display_height = frame_height_,
             .jitter = jitter,
             .projection = scene.projection,
             .view_projection = view_projection,
             .previous = frames.clock.frames == 0 ? view_projection : frames.view_projection,
             .sharpness = scene.fsr2.value_or(Fsr2Scaling{}).sharpness,
             .frame_index = frames.clock.frames,
             .phases = frames.clock.phases});
        if (frames.clock.frames == 0) {
            for (SDL_GPUTexture* texture :
                 {frames.color.at(last), frames.lock_status.at(last), frames.luma_history.at(last),
                  frames.dilated_motion.at(last)}) {
                clear_texture(commands, texture);
            }
        }
        const Fsr2Textures& t = fsr2_textures_;
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        const auto pass = [&](Fsr2Pass which, std::initializer_list<SDL_GPUTexture*> targets,
                              std::initializer_list<SDL_GPUTexture*> sources) {
            std::array<SDL_GPUColorTargetInfo, 3> colors{};
            std::size_t count = 0;
            for (SDL_GPUTexture* target : targets) {
                SDL_GPUColorTargetInfo& color = colors.at(count++);
                color.texture = target;
                color.load_op = SDL_GPU_LOADOP_DONT_CARE;
                color.store_op = SDL_GPU_STOREOP_STORE;
            }
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(
                commands, colors.data(), static_cast<std::uint32_t>(count), nullptr);
            SDL_BindGPUGraphicsPipeline(render,
                                        fsr2_pipelines_.at(static_cast<std::size_t>(which)));
            SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                           static_cast<std::uint32_t>(sizeof(uniforms)));
            std::array<SDL_GPUTextureSamplerBinding, 8> bindings{};
            std::size_t bound = 0;
            for (SDL_GPUTexture* source : sources) {
                bindings.at(bound++) = {.texture = source, .sampler = nearest};
            }
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bound));
            if (which == Fsr2Pass::DepthClip) {
                SDL_BindGPUFragmentStorageBuffers(render, 0, &t.reconstructed, 1);
            }
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        };
        pass(Fsr2Pass::Luma, {t.luma_mip}, {t.color});
        pass(Fsr2Pass::Dilate, {t.dilated_depth, frames.dilated_motion.at(next), t.lock_luma},
             {depth_, t.velocity, t.color});
        // The reconstruct pass clears the depths, then scatters them.
        const std::array<SDL_GPUTextureSamplerBinding, 2> dilated{{
            {.texture = t.dilated_depth, .sampler = nearest},
            {.texture = frames.dilated_motion.at(next), .sampler = nearest},
        }};
        for (const std::int32_t step : {0, 1}) {
            Fsr2Uniforms stepped = uniforms;
            stepped.options[2] = step;
            const SDL_GPUStorageBufferReadWriteBinding buffer{.buffer = t.reconstructed,
                                                              .cycle = false,
                                                              .padding1 = 0,
                                                              .padding2 = 0,
                                                              .padding3 = 0};
            SDL_GPUComputePass* compute = SDL_BeginGPUComputePass(commands, nullptr, 0, &buffer, 1);
            SDL_BindGPUComputePipeline(compute, fsr2_reconstruct_);
            SDL_BindGPUComputeSamplers(compute, 0, dilated.data(),
                                       static_cast<std::uint32_t>(dilated.size()));
            SDL_PushGPUComputeUniformData(commands, 0, &stepped,
                                          static_cast<std::uint32_t>(sizeof(stepped)));
            SDL_DispatchGPUCompute(compute, (scene_width_ + kFsr2Threads - 1) / kFsr2Threads,
                                   (scene_height_ + kFsr2Threads - 1) / kFsr2Threads, 1);
            SDL_EndGPUComputePass(compute);
        }
        pass(Fsr2Pass::DepthClip, {t.prepared, t.reactive},
             {frames.dilated_motion.at(next), t.dilated_depth, t.color,
              frames.dilated_motion.at(last), t.velocity, depth_});
        pass(Fsr2Pass::Lock, {t.new_locks}, {t.lock_luma});
        pass(Fsr2Pass::Accumulate,
             {frames.color.at(next), frames.lock_status.at(next), frames.luma_history.at(next)},
             {frames.dilated_motion.at(next), t.prepared, t.reactive, frames.color.at(last),
              frames.lock_status.at(last), t.new_locks, t.luma_mip, frames.luma_history.at(last)});
        pass(Fsr2Pass::Rcas, {forward_targets_.world}, {frames.color.at(next)});
        frames.last = next;
        frames.view_projection = view_projection;
    }

    // MetalFX's temporal scaler, as Godot runs it, or the game's temporal upscaler: FSR 2's dilate
    // pass first works the sky's motion vectors out from the depth (where the prepass left
    // kInvalidMotion), then the upscaler upscales the world with the depth and those motion
    // vectors and its own frames before into a texture at the frame's size, which is copied into
    // Forward+'s world for the post passes. A failure is logged, and the copy shows what the
    // texture held.
    void upscale_external(SDL_GPUCommandBuffer* commands, const Scene3D& scene, Fsr2Frames& frames,
                          const std::array<float, 2>& jitter) {
        const std::size_t next = 1 - frames.last;
        const Mat4 view_projection = multiply(scene.projection, scene.camera);
        const Fsr2Uniforms uniforms = fsr2_uniforms(
            {.render_width = scene_width_,
             .render_height = scene_height_,
             .display_width = frame_width_,
             .display_height = frame_height_,
             .jitter = jitter,
             .projection = scene.projection,
             .view_projection = view_projection,
             .previous = frames.clock.frames == 0 ? view_projection : frames.view_projection,
             .sharpness = scene.fsr2.value_or(Fsr2Scaling{}).sharpness,
             .frame_index = frames.clock.frames,
             .phases = frames.clock.phases});
        const Fsr2Textures& t = fsr2_textures_;
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        const std::array dilated{t.dilated_depth, frames.dilated_motion.at(next), t.lock_luma};
        std::array<SDL_GPUColorTargetInfo, 3> targets{};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            targets.at(i).texture = dilated.at(i);
            targets.at(i).load_op = SDL_GPU_LOADOP_DONT_CARE;
            targets.at(i).store_op = SDL_GPU_STOREOP_STORE;
        }
        SDL_GPURenderPass* dilate = SDL_BeginGPURenderPass(
            commands, targets.data(), static_cast<std::uint32_t>(targets.size()), nullptr);
        SDL_BindGPUGraphicsPipeline(dilate,
                                    fsr2_pipelines_.at(static_cast<std::size_t>(Fsr2Pass::Dilate)));
        SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                       static_cast<std::uint32_t>(sizeof(uniforms)));
        const std::array<SDL_GPUTextureSamplerBinding, 3> sources{{
            {.texture = depth_, .sampler = nearest},
            {.texture = t.velocity, .sampler = nearest},
            {.texture = t.color, .sampler = nearest},
        }};
        SDL_BindGPUFragmentSamplers(dilate, 0, sources.data(),
                                    static_cast<std::uint32_t>(sources.size()));
        SDL_DrawGPUPrimitives(dilate, 3, 1, 0, 0);
        SDL_EndGPURenderPass(dilate);
        const std::array<float, 2> pixels{uniforms.jitter_scale[0], uniforms.jitter_scale[1]};
        Result<void> scaled;
        if (frames.temporal != nullptr) {
            const TemporalFrame frame{
                .device = native_device(device_, backend_),
                .commands = SDL_SlashGPUNativeCommandBuffer(commands),
                .size = {.render_width = scene_width_,
                         .render_height = scene_height_,
                         .output_width = frame_width_,
                         .output_height = frame_height_},
                .color = native_texture(device_, t.color, scene_width_, scene_height_),
                .depth = native_texture(device_, depth_, scene_width_, scene_height_),
                .motion = native_texture(device_, frames.dilated_motion.at(next), scene_width_,
                                         scene_height_),
                .output = native_texture(device_, t.scaled, frame_width_, frame_height_),
                .jitter = pixels,
                .reset = frames.clock.frames == 0,
                .sharpness = scene.fsr2.value_or(Fsr2Scaling{}).sharpness,
                .projection = scene.projection,
                .camera = scene.camera};
            scaled = frames.temporal->upscale(frame);
            SDL_SlashGPURestoreState(commands);
        } else {
            scaled = encode_metalfx_temporal(device_, commands, frames.scaler,
                                             {.color = t.color,
                                              .depth = depth_,
                                              .motion = frames.dilated_motion.at(next),
                                              .output = t.scaled,
                                              .jitter = metalfx_jitter(pixels),
                                              .reset = frames.clock.frames == 0});
        }
        if (!scaled) {
            log::error("the temporal upscaler failed",
                       {{"upscaler",
                         frames.temporal != nullptr ? frames.added.c_str() : "metalfx_temporal"},
                        {"error", scaled.error().message}});
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureLocation from{
            .texture = t.scaled, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
        const SDL_GPUTextureLocation to{
            .texture = forward_targets_.world, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
        SDL_CopyGPUTextureToTexture(copy, &from, &to, frame_width_, frame_height_, 1, false);
        SDL_EndGPUCopyPass(copy);
        frames.last = next;
        frames.view_projection = view_projection;
    }

    // FSR 3's passes, in the order run_fsr3 runs them, as upscale_fsr2 runs FSR 2's: the frames
    // before keep the frame's jitter too, which the next reads.
    void upscale_fsr3(SDL_GPUCommandBuffer* commands, const Scene3D& scene, Fsr2Frames& frames,
                      const std::array<float, 2>& jitter) {
        const std::size_t last = frames.last;
        const std::size_t next = 1 - last;
        const Mat4 view_projection = multiply(scene.projection, scene.camera);
        const Fsr3Uniforms uniforms = fsr3_uniforms(
            {.render_width = scene_width_,
             .render_height = scene_height_,
             .display_width = frame_width_,
             .display_height = frame_height_,
             .jitter = jitter,
             .projection = scene.projection,
             .view_projection = view_projection,
             .previous = frames.clock.frames == 0 ? view_projection : frames.view_projection,
             .sharpness = scene.fsr2.value_or(Fsr2Scaling{}).sharpness,
             .frame_index = frames.clock.frames,
             .phases = frames.clock.phases},
            frames.jitter);
        if (frames.clock.frames == 0) {
            for (SDL_GPUTexture* texture : {frames.color.at(last), frames.luma.at(last),
                                            frames.accumulation.at(last), frames.lumas.at(last)}) {
                clear_texture(commands, texture);
            }
        }
        const Fsr2Textures& t = fsr2_textures_;
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        const auto draw = [&](SDL_GPUGraphicsPipeline* pipeline, const void* data,
                              std::uint32_t size, std::initializer_list<SDL_GPUTexture*> targets,
                              std::initializer_list<SDL_GPUTexture*> sources,
                              bool storage = false) {
            std::array<SDL_GPUColorTargetInfo, 4> colors{};
            std::size_t count = 0;
            for (SDL_GPUTexture* target : targets) {
                SDL_GPUColorTargetInfo& color = colors.at(count++);
                color.texture = target;
                color.load_op = SDL_GPU_LOADOP_DONT_CARE;
                color.store_op = SDL_GPU_STOREOP_STORE;
            }
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(
                commands, colors.data(), static_cast<std::uint32_t>(count), nullptr);
            SDL_BindGPUGraphicsPipeline(render, pipeline);
            SDL_PushGPUFragmentUniformData(commands, 0, data, size);
            std::array<SDL_GPUTextureSamplerBinding, 8> bindings{};
            std::size_t bound = 0;
            for (SDL_GPUTexture* source : sources) {
                bindings.at(bound++) = {.texture = source, .sampler = nearest};
            }
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bound));
            if (storage) {
                SDL_BindGPUFragmentStorageBuffers(render, 0, &t.reconstructed, 1);
            }
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        };
        const auto pass = [&](Fsr3Pass which, std::initializer_list<SDL_GPUTexture*> targets,
                              std::initializer_list<SDL_GPUTexture*> sources,
                              const Fsr3Uniforms& with) {
            draw(fsr3_pipelines_.at(static_cast<std::size_t>(which)), &with,
                 static_cast<std::uint32_t>(sizeof(with)), targets, sources,
                 which == Fsr3Pass::Reactivity);
        };
        pass(Fsr3Pass::Prepare,
             {t.dilated_depth, t.dilated_motion, t.farthest_depth, frames.luma.at(next)},
             {depth_, t.velocity, t.color}, uniforms);
        // The reconstruct pass clears the depths, then scatters them.
        const std::array<SDL_GPUTextureSamplerBinding, 2> dilated{{
            {.texture = t.dilated_depth, .sampler = nearest},
            {.texture = t.dilated_motion, .sampler = nearest},
        }};
        for (const std::int32_t step : {0, 1}) {
            Fsr3Uniforms stepped = uniforms;
            stepped.fsr2.options[2] = step;
            const SDL_GPUStorageBufferReadWriteBinding buffer{.buffer = t.reconstructed,
                                                              .cycle = false,
                                                              .padding1 = 0,
                                                              .padding2 = 0,
                                                              .padding3 = 0};
            SDL_GPUComputePass* compute = SDL_BeginGPUComputePass(commands, nullptr, 0, &buffer, 1);
            SDL_BindGPUComputePipeline(compute, fsr3_reconstruct_);
            SDL_BindGPUComputeSamplers(compute, 0, dilated.data(),
                                       static_cast<std::uint32_t>(dilated.size()));
            SDL_PushGPUComputeUniformData(commands, 0, &stepped,
                                          static_cast<std::uint32_t>(sizeof(stepped)));
            SDL_DispatchGPUCompute(compute, (scene_width_ + kFsr2Threads - 1) / kFsr2Threads,
                                   (scene_height_ + kFsr2Threads - 1) / kFsr2Threads, 1);
            SDL_EndGPUComputePass(compute);
        }
        pass(Fsr3Pass::Pyramid, {t.farthest_half, t.levels[0]},
             {t.farthest_depth, t.dilated_motion, frames.luma.at(next), frames.luma.at(last)},
             uniforms);
        for (std::uint32_t level = 1; level < kFsr3ShadingLevels; ++level) {
            Fsr3Uniforms leveled = uniforms;
            leveled.fsr2.options[0] = static_cast<std::int32_t>(level);
            pass(Fsr3Pass::Reduce, {t.levels.at(level)}, {t.levels.at(level - 1)}, leveled);
        }
        pass(Fsr3Pass::ShadingChange, {t.shading_change}, {t.levels[0], t.levels[1], t.levels[2]},
             uniforms);
        pass(Fsr3Pass::Reactivity, {t.masks, frames.accumulation.at(next)},
             {t.dilated_motion, t.dilated_depth, t.shading_change, frames.accumulation.at(last)},
             uniforms);
        pass(Fsr3Pass::Lock, {t.new_locks}, {frames.luma.at(next)}, uniforms);
        pass(Fsr3Pass::Instability, {frames.lumas.at(next), t.instability},
             {t.dilated_motion, t.masks, frames.luma.at(next), frames.lumas.at(last)}, uniforms);
        pass(Fsr3Pass::Accumulate, {frames.color.at(next)},
             {t.color, t.dilated_motion, t.instability, t.farthest_half, t.masks,
              frames.color.at(last), t.new_locks},
             uniforms);
        // FSR 2's sharpening pass reads FSR 2's uniforms, the first of FSR 3's.
        draw(fsr2_pipelines_.at(static_cast<std::size_t>(Fsr2Pass::Rcas)), &uniforms.fsr2,
             static_cast<std::uint32_t>(sizeof(uniforms.fsr2)), {forward_targets_.world},
             {frames.color.at(next)});
        frames.last = next;
        frames.view_projection = view_projection;
        frames.jitter = jitter;
    }

    // ssr.hlsl's passes, from the prepass's depth and normals and the frame before, seen from
    // history's camera, to the reflections at the frame's size, in the order ssr_plan gives.
    void draw_ssr(SDL_GPUCommandBuffer* commands, const Scene3D& scene, const SsrCamera& camera,
                  const SsrHistory& history) {
        const SsrPlan plan = ssr_plan(scene.ssr.settings, scene_width_, scene_height_);
        for (const SsrStep& step : plan.steps) {
            if (step.pass != SsrPass::CopyLevels) {
                draw_ssr_step(commands, step,
                              ssr_uniforms(scene.ssr, step, camera, history.camera, scene_width_,
                                           scene_height_),
                              history);
                continue;
            }
            // GPUs can't draw into one level of a texture while sampling another, so each level
            // is drawn into a texture of its own and copied into one with mipmaps for the passes
            // that read them all.
            const bool hiz = step.target == kSsrHizMips;
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            for (std::uint32_t level = 0; level < plan.levels; ++level) {
                const std::uint32_t index = (hiz ? kSsrHiz : kSsrColor) + level;
                const PostSize size = plan.sizes.at(index);
                SDL_GPUTextureLocation from{};
                from.texture = ssr_targets_.textures.at(index);
                SDL_GPUTextureLocation to{};
                to.texture = hiz ? ssr_targets_.hiz_mips : ssr_targets_.color_mips;
                to.mip_level = level;
                SDL_CopyGPUTextureToTexture(copy, &from, &to, size.width, size.height, 1, false);
            }
            SDL_EndGPUCopyPass(copy);
        }
    }

    // One of SSR's passes, other than a copy, drawn with uniforms over its target: the trace's
    // over the reflections' first level and the mip levels, the last frame's over history's
    // texture.
    void draw_ssr_step(SDL_GPUCommandBuffer* commands, const SsrStep& step,
                       const SsrUniforms& uniforms, const SsrHistory& history) {
        SDL_GPUGraphicsPipeline* pipeline = ssr_trace_pipeline_;
        if (step.pass != SsrPass::Trace) {
            // Made in prepare_ssr, at the start of the frame.
            auto made = fullscreen_pipeline(ssr_pipelines_, ssr_shaders_,
                                            ssr_gpu_format(ssr_format(step.target)));
            if (!made) {
                return;
            }
            pipeline = made.value();
        }
        std::array<SDL_GPUColorTargetInfo, 2> colors{};
        colors[0].texture =
            step.target == kSsrLastFrame ? history.texture : ssr_targets_.textures.at(step.target);
        colors[0].load_op = SDL_GPU_LOADOP_DONT_CARE;
        colors[0].store_op = SDL_GPU_STOREOP_STORE;
        colors[1] = colors[0];
        colors[1].texture = ssr_targets_.textures.at(kSsrMipLevel);
        const std::uint32_t count = step.pass == SsrPass::Trace ? 2 : 1;
        SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, colors.data(), count, nullptr);
        SDL_BindGPUGraphicsPipeline(render, pipeline);
        SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                       static_cast<std::uint32_t>(sizeof(uniforms)));
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        std::array<SDL_GPUTextureSamplerBinding, kSsrSlots> bindings{};
        for (std::size_t slot = 0; slot < kSsrSlots; ++slot) {
            const std::uint32_t index = step.slots.at(slot);
            SDL_GPUTexture* texture = white_;
            if (index == kSsrDepthBuffer) {
                texture = prepass_.depth;
            } else if (index == kSsrNormals) {
                texture = prepass_.normals;
            } else if (index == kSsrHizMips) {
                texture = ssr_targets_.hiz_mips;
            } else if (index == kSsrColorMips) {
                texture = ssr_targets_.color_mips;
            } else if (index == kSsrLastFrame) {
                texture = history.texture;
            } else if (index == kSsrWorld) {
                texture = frame_.scene ? world_texture(*frame_.scene) : forward_targets_.world;
            } else if (index < kSsrTextures && ssr_targets_.textures.at(index) != nullptr) {
                texture = ssr_targets_.textures.at(index);
            }
            bindings.at(slot) = {.texture = texture, .sampler = nearest};
        }
        SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                    static_cast<std::uint32_t>(bindings.size()));
        SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render);
    }

    // Keeps the world's colors, seen from camera, as history for the next frame's reflections.
    void keep_last_frame(SDL_GPUCommandBuffer* commands, const Scene3D& scene, SsrHistory& history,
                         const SsrCamera& camera) {
        const SsrStep step = ssr_last_frame_step();
        draw_ssr_step(commands, step,
                      ssr_uniforms(scene.ssr, step, camera, camera, scene_width_, scene_height_),
                      history);
        history.camera = camera;
        history.primed = true;
    }

    // forward_post.hlsl's passes: depth of field's, whose result the rest read for the world; with
    // auto exposure the luminance passes, from the world's texture down to one texel, then glow's
    // levels, each from the one before, then the tonemap pass from the world's texture over the
    // frame's target. A frame with no sky or mesh draws clears the world's texture first.
    void draw_forward_post(SDL_GPUCommandBuffer* commands, const Scene3D& scene, bool clear) {
        const PostProcess& post = scene.post;
        if (clear) {
            SDL_GPUColorTargetInfo color{};
            color.texture = forward_targets_.world;
            const Float4 linear = forward_clear(frame_.clear);
            color.clear_color = {linear[0], linear[1], linear[2], linear[3]};
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &color, 1, nullptr));
        }
        SDL_GPUSampler* sampler = samplers_.at(sampler_index(Filter::Linear, false));
        SDL_GPUSampler* nearest = samplers_.at(sampler_index(Filter::Nearest, false));
        // The world's texture, or after depth of field its result.
        SDL_GPUTexture* scene_world = forward_targets_.world;
        // A glow pass reads only its source; the tonemap pass reads glow's levels too. Passes
        // read the measured brightness given them, or white, and depth of field's the depth
        // buffer and the world.
        const auto pass = [&](SDL_GPUTexture* target, SDL_GPUTextureFormat format,
                              const ForwardPostUniforms& uniforms, SDL_GPUTexture* source,
                              bool levels, SDL_GPUTexture* lut, SDL_GPUTexture* measured = nullptr,
                              SDL_GPUTexture* depth = nullptr, SDL_GPUTexture* world = nullptr) {
            SDL_GPUColorTargetInfo color{};
            color.texture = target;
            color.load_op = SDL_GPU_LOADOP_DONT_CARE;
            color.store_op = SDL_GPU_STOREOP_STORE;
            // Made in prepare_forward, at the start of the frame.
            auto pipeline = fullscreen_pipeline(forward_pipelines_, forward_shaders_, format);
            if (!pipeline) {
                return;
            }
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(render, pipeline.value());
            SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                           static_cast<std::uint32_t>(sizeof(uniforms)));
            std::array<SDL_GPUTextureSamplerBinding, kForwardPostTextures> bindings{};
            // A luminance pass after the first loads the level before, a texture of floats, from
            // a slot of its own.
            const bool from_level = format == kLuminanceFormat && source != scene_world;
            bindings[0] = {.texture = from_level ? scene_world : source, .sampler = sampler};
            for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
                SDL_GPUTexture* level = forward_targets_.glow.at(i);
                bindings.at(1 + i) = {.texture = levels && level != nullptr ? level : white_,
                                      .sampler = sampler};
            }
            bindings.at(1 + kMaxGlowLevels) = {.texture = lut, .sampler = sampler};
            bindings.at(kBrightnessSlot) = {.texture = measured != nullptr ? measured : white_,
                                            .sampler = nearest};
            bindings.at(kLuminanceSourceSlot) = {.texture = from_level ? source : white_,
                                                 .sampler = nearest};
            bindings.at(kDepthSlot) = {.texture = depth != nullptr ? depth : white_,
                                       .sampler = nearest};
            bindings.at(kDofWorldSlot) = {.texture = world != nullptr ? world : white_,
                                          .sampler = nearest};
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bindings.size()));
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        };
        if (uses_dof(post)) {
            const auto texture = [&](DofTexture which) {
                return which == DofTexture::World
                           ? forward_targets_.world
                           : forward_targets_.dof.at(static_cast<std::size_t>(which) - 1);
            };
            // With FSR 2 the depth buffer is at the render size.
            const PostSize depth = uses_fsr2(scene)
                                       ? PostSize{.width = scene_width_, .height = scene_height_}
                                       : PostSize{};
            for (const DofStep& step : dof_plan(post, frame_width_, frame_height_)) {
                pass(texture(step.target), kHdrFormat,
                     dof_uniforms(post, step, scene.projection, depth), texture(step.source), false,
                     white_, nullptr, depth_, forward_targets_.world);
            }
            scene_world = texture(DofTexture::Out);
        }
        SDL_GPUTexture* brightness = nullptr;
        if (post.auto_exposure) {
            // As Godot's Luminance::luminance_reduction: the last pass moves from the last
            // frame's brightness, then the brightness texture and the last level swap.
            const bool set = !forward_targets_.measured ||
                             forward_targets_.version != post.auto_exposure_version;
            std::vector<SDL_GPUTexture*>& levels = forward_targets_.luminance;
            const std::vector<PostSize> plan = luminance_plan(frame_width_, frame_height_);
            PostSize source{.width = frame_width_, .height = frame_height_};
            for (std::size_t i = 0; i < levels.size(); ++i) {
                ForwardPass kind = i == 0 ? ForwardPass::LuminanceFirst : ForwardPass::Luminance;
                if (i > 0 && i + 1 == levels.size() && !set) {
                    kind = ForwardPass::LuminanceFinal;
                }
                pass(levels.at(i), kLuminanceFormat,
                     luminance_uniforms(post, kind, source, plan.at(i), i + 1 == levels.size()),
                     i == 0 ? scene_world : levels.at(i - 1), false, white_,
                     forward_targets_.brightness);
                source = plan.at(i);
            }
            std::swap(forward_targets_.brightness, levels.back());
            forward_targets_.measured = true;
            forward_targets_.version = post.auto_exposure_version;
            brightness = forward_targets_.brightness;
        }
        if (post.glow) {
            const GlowPlan plan = forward_glow_plan(post, frame_width_, frame_height_);
            for (std::size_t i = 0; i < plan.count; ++i) {
                const PostSize size = plan.sizes.at(i);
                pass(forward_targets_.glow.at(i), kHdrFormat,
                     forward_post_uniforms(post,
                                           i == 0 ? ForwardPass::GlowFirst : ForwardPass::GlowNext,
                                           size.width, size.height, frame_width_, frame_height_),
                     i == 0 ? scene_world : forward_targets_.glow.at(i - 1), false, white_,
                     brightness);
            }
        }
        SDL_GPUTexture* lut = white_;
        TextureDesc lut_desc{};
        if (post.adjustments && post.color_correction != TextureId::None) {
            const Texture& found = textures_.at(post.color_correction);
            lut = found.gpu;
            lut_desc = found.desc;
        }
        pass(frame_texture_, frame_format_,
             forward_post_uniforms(post, ForwardPass::Tonemap, frame_width_, frame_height_,
                                   frame_width_, frame_height_, lut_desc.width, lut_desc.height),
             scene_world, post.glow, lut, brightness);
    }

    // The post passes' shaders, their pipeline for a target format, and their textures at the
    // frame's size: the meshes' and glow's four. Made at the start of a frame that needs them.
    Result<void> prepare_post(SDL_GPUTextureFormat format, std::uint32_t width,
                              std::uint32_t height) {
        if (post_shaders_[0] == nullptr) {
            const ShaderCode code{.vertex_spirv = shaders::kPostVertexSpirv,
                                  .fragment_spirv = shaders::kPostFragmentSpirv,
                                  .vertex_msl = shaders::kPostVertexMsl,
                                  .fragment_msl = shaders::kPostFragmentMsl,
                                  .hlsl = shaders::kPostHlsl};
            auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
            if (!vertex) {
                return vertex.error();
            }
            auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 3, 1);
            if (!fragment) {
                SDL_ReleaseGPUShader(device_, vertex.value());
                return fragment.error();
            }
            post_shaders_ = {vertex.value(), fragment.value()};
        }
        for (const SDL_GPUTextureFormat needed : {kTextureFormat, format}) {
            if (auto pipeline = post_pipeline(needed); !pipeline) {
                return pipeline.error();
            }
        }
        if (post_targets_.world != nullptr && post_targets_.width == width &&
            post_targets_.height == height) {
            return {};
        }
        release_post_targets();
        const auto made = [&](std::uint32_t w, std::uint32_t h) {
            return create_gpu_texture(
                {.width = w, .height = h, .filter = Filter::Linear, .render_target = true});
        };
        auto world = made(width, height);
        if (!world) {
            return world.error();
        }
        post_targets_.world = world.value();
        const std::array<PostSize, kGlowLevels> sizes = glow_sizes(width, height);
        for (std::size_t i = 0; i < kGlowLevels; ++i) {
            auto level = made(sizes.at(i).width, sizes.at(i).height);
            if (!level) {
                release_post_targets();
                return level.error();
            }
            post_targets_.glow.at(i) = level.value();
        }
        post_targets_.width = width;
        post_targets_.height = height;
        return {};
    }

    // The sky's shaders, its pipeline for a target format and the depth buffer its pass has, made
    // at the start of a frame with a sky so a failure shows there.
    Result<void> prepare_sky(SDL_GPUTextureFormat format, std::uint32_t width,
                             std::uint32_t height) {
        if (auto depth = depth_target(width, height); !depth) {
            return depth.error();
        }
        if (auto made = make_sky_shaders(); !made) {
            return made;
        }
        if (sky_pipeline(format) != nullptr) {
            return {};
        }
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = sky_shaders_[0];
        info.fragment_shader = sky_shaders_[1];
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        // Behind everything: it neither tests nor writes depth.
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depth_format_;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (pipeline == nullptr) {
            return sdl_error("creating the sky pipeline failed");
        }
        sky_pipelines_.emplace_back(format, pipeline);
        return {};
    }

    // The sky's shaders, unless they are made already.
    Result<void> make_sky_shaders() {
        if (sky_shaders_[0] != nullptr) {
            return {};
        }
        const ShaderCode code{.vertex_spirv = shaders::kSkyVertexSpirv,
                              .fragment_spirv = shaders::kSkyFragmentSpirv,
                              .vertex_msl = shaders::kSkyVertexMsl,
                              .fragment_msl = shaders::kSkyFragmentMsl,
                              .hlsl = shaders::kSkyHlsl};
        auto vertex = make_gpu_shader(SDL_GPU_SHADERSTAGE_VERTEX, code, 0, 0);
        if (!vertex) {
            return vertex.error();
        }
        auto fragment = make_gpu_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, code, 1, 1);
        if (!fragment) {
            SDL_ReleaseGPUShader(device_, vertex.value());
            return fragment.error();
        }
        sky_shaders_ = {vertex.value(), fragment.value()};
        return {};
    }

    // The sky's pipeline for a target format, or null before prepare_sky made it.
    SDL_GPUGraphicsPipeline* sky_pipeline(SDL_GPUTextureFormat format) const {
        for (const auto& [made, pipeline] : sky_pipelines_) {
            if (made == format) {
                return pipeline;
            }
        }
        return nullptr;
    }

    Result<SDL_GPUGraphicsPipeline*> post_pipeline(SDL_GPUTextureFormat format) {
        return fullscreen_pipeline(post_pipelines_, post_shaders_, format);
    }

    void release_post_targets() {
        const auto release = [this](SDL_GPUTexture*& texture) {
            if (texture != nullptr) {
                pending_release_.push_back(std::exchange(texture, nullptr));
            }
        };
        release(post_targets_.world);
        for (SDL_GPUTexture*& texture : post_targets_.glow) {
            release(texture);
        }
        post_targets_.width = 0;
        post_targets_.height = 0;
    }

    // post.hlsl's passes: glow's filter, downsampling and upsampling through its four textures,
    // then the post pass from the meshes' texture over the frame's target. A frame with no mesh
    // draws clears the meshes' texture first.
    void draw_post(SDL_GPUCommandBuffer* commands, const PostProcess& post, bool clear) {
        if (clear) {
            SDL_GPUColorTargetInfo color{};
            color.texture = post_targets_.world;
            color.clear_color = {frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a};
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &color, 1, nullptr));
        }
        SDL_GPUSampler* sampler = samplers_.at(sampler_index(Filter::Linear, false));
        const auto pass = [&](SDL_GPUTexture* target, SDL_GPUTextureFormat format,
                              const PostUniforms& uniforms, SDL_GPUTexture* source,
                              SDL_GPUTexture* glow, SDL_GPUTexture* lut) {
            SDL_GPUColorTargetInfo color{};
            color.texture = target;
            color.load_op = SDL_GPU_LOADOP_DONT_CARE;
            color.store_op = SDL_GPU_STOREOP_STORE;
            // Made in prepare_post, at the start of the frame.
            auto pipeline = post_pipeline(format);
            if (!pipeline) {
                return;
            }
            SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(render, pipeline.value());
            SDL_PushGPUFragmentUniformData(commands, 0, &uniforms,
                                           static_cast<std::uint32_t>(sizeof(uniforms)));
            const std::array<SDL_GPUTextureSamplerBinding, 3> bindings{{
                {.texture = source, .sampler = sampler},
                {.texture = glow, .sampler = sampler},
                {.texture = lut, .sampler = sampler},
            }};
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(),
                                        static_cast<std::uint32_t>(bindings.size()));
            SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
            SDL_EndGPURenderPass(render);
        };
        const std::array<PostSize, kGlowLevels> sizes = glow_sizes(frame_width_, frame_height_);
        const auto glow_pass = [&](PostPass kind, std::size_t from, std::size_t to) {
            pass(post_targets_.glow.at(to), kTextureFormat,
                 post_uniforms(post, kind, sizes.at(to).width, sizes.at(to).height),
                 kind == PostPass::GlowFilter ? post_targets_.world : post_targets_.glow.at(from),
                 white_, white_);
        };
        if (post.glow) {
            glow_pass(PostPass::GlowFilter, 0, 0);
            for (std::size_t i = 1; i < kGlowLevels; ++i) {
                glow_pass(PostPass::GlowDownsample, i - 1, i);
            }
            for (std::size_t i = kGlowLevels - 1; i-- > 0;) {
                glow_pass(PostPass::GlowUpsample, i + 1, i);
            }
        }
        SDL_GPUTexture* lut = white_;
        TextureDesc lut_desc{};
        if (post.adjustments && post.color_correction != TextureId::None) {
            const Texture& found = textures_.at(post.color_correction);
            lut = found.gpu;
            lut_desc = found.desc;
        }
        pass(frame_texture_, frame_format_,
             post_uniforms(post, PostPass::Post, frame_width_, frame_height_, lut_desc.width,
                           lut_desc.height),
             post_targets_.world, post.glow ? post_targets_.glow[0] : white_, lut);
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
                                           std::uint32_t samplers, std::uint32_t uniforms,
                                           std::uint32_t storage_buffers = 0) {
        const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo info{};
        info.stage = stage;
        info.entrypoint = vertex ? "vs_main" : "fs_main";
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        info.num_storage_buffers = storage_buffers;
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
        info.format = gpu_format(desc.format);
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (desc.render_target) {
            info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        }
        // MetalFX's spatial scaler writes into it as a compute shader does.
        if (desc.scaled_into && metalfx_.spatial) {
            info.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
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

    // Uploads texels given in desc's format to a texture stored in stored's, decoding them
    // when the two differ.
    Result<void> upload_texels(SDL_GPUTexture* texture, const TextureDesc& desc,
                               const TextureDesc& stored, std::span<const std::uint8_t> texels) {
        if (stored.format == desc.format) {
            return upload(texture, stored, texels);
        }
        auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
        if (!pixels) {
            return pixels.error();
        }
        return upload(texture, stored, pixels.value());
    }

    // Replaces every texel, given in desc's format. It runs ahead of the frame being built,
    // whose draws are sent at end_frame, so they all see the new texels.
    Result<void> upload(SDL_GPUTexture* texture, const TextureDesc& desc,
                        std::span<const std::uint8_t> texels) {
        const TransferBuffer transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                      static_cast<std::uint32_t>(texels.size()));
        if (transfer.get() == nullptr) {
            return sdl_error("making room to upload the texture failed");
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer.get(), false);
        if (mapped == nullptr) {
            return sdl_error("uploading the texture failed");
        }
        std::memcpy(mapped, texels.data(), texels.size());
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
        const Scene3D* scene = frame_.scene ? &*frame_.scene : nullptr;
        const bool clustered =
            scene != nullptr && !mesh_draws_.empty() && scene->method == RenderMethod::ForwardPlus;
        if (total == 0 && particle_jobs_.empty() && !clustered) {
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
        if (uploaded && clustered) {
            uploaded = upload_clusters(copy, *scene);
        }
        SDL_EndGPUCopyPass(copy);
        return uploaded;
    }

    // Builds the frame's Forward+ clusters and copies them to the GPU, with the decals' atlas when
    // it changes.
    Result<void> upload_clusters(SDL_GPUCopyPass* copy, const Scene3D& scene) {
        decal_atlas_.update(scene.decals);
        const DecalAtlas& atlas = decal_atlas_.atlas();
        clusters_ = build_clusters(scene, scene_width_, scene_height_, &atlas);
        if (clusters_.lights.empty()) {
            clusters_.lights.emplace_back(); // a storage buffer is never empty
        }
        if (auto uploaded =
                upload_storage(copy, cluster_lights_, std::as_bytes(std::span(clusters_.lights)));
            !uploaded) {
            return uploaded;
        }
        if (auto uploaded =
                upload_storage(copy, cluster_cells_, std::as_bytes(std::span(clusters_.cells)));
            !uploaded) {
            return uploaded;
        }
        std::vector<MeshProbeUniforms> probes = probe_uniforms(scene.reflections);
        if (probes.empty()) {
            probes.emplace_back(); // a storage buffer is never empty
        }
        if (auto uploaded = upload_storage(copy, probe_buffer_, std::as_bytes(std::span(probes)));
            !uploaded) {
            return uploaded;
        }
        if (clusters_.decals.empty()) {
            clusters_.decals.emplace_back();
        }
        if (auto uploaded =
                upload_storage(copy, decal_buffer_, std::as_bytes(std::span(clusters_.decals)));
            !uploaded) {
            return uploaded;
        }
        if (uses_volumetric_fog(scene)) {
            const std::vector<Float4> items = fog_items(scene, scene_width_, scene_height_);
            if (auto uploaded = upload_storage(copy, fog_items_, std::as_bytes(std::span(items)));
                !uploaded) {
                return uploaded;
            }
        }
        if (atlas.generation == decal_atlas_uploaded_ && decal_atlas_buffer_.buffer != nullptr) {
            return {};
        }
        auto uploaded =
            upload_storage(copy, decal_atlas_buffer_, std::as_bytes(std::span(atlas.texels)));
        if (uploaded) {
            decal_atlas_uploaded_ = atlas.generation;
        }
        return uploaded;
    }

    // Copies bytes into a storage buffer, growing it first if they don't fit.
    Result<void> upload_storage(SDL_GPUCopyPass* copy, StorageBuffer& storage,
                                std::span<const std::byte> bytes) {
        const auto size = static_cast<std::uint32_t>(bytes.size());
        if (size > storage.capacity) {
            const std::uint32_t capacity =
                std::max({size, std::min(storage.capacity * 2, kMaxStorageBytes), kMinVertexBytes});
            if (storage.buffer != nullptr) {
                SDL_ReleaseGPUBuffer(device_, storage.buffer);
                SDL_ReleaseGPUTransferBuffer(device_, storage.transfer);
                storage = {};
            }
            SDL_GPUBufferCreateInfo buffer{};
            // Volumetric fog's passes read the lights and clusters too.
            buffer.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                           SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
            buffer.size = capacity;
            storage.buffer = SDL_CreateGPUBuffer(device_, &buffer);
            SDL_GPUTransferBufferCreateInfo transfer{};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer.size = capacity;
            storage.transfer = SDL_CreateGPUTransferBuffer(device_, &transfer);
            if (storage.buffer == nullptr || storage.transfer == nullptr) {
                return sdl_error("making room for the frame's light clusters failed");
            }
            storage.capacity = capacity;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, storage.transfer, true);
        if (mapped == nullptr) {
            return sdl_error("uploading the frame's light clusters failed");
        }
        std::memcpy(mapped, bytes.data(), size);
        SDL_UnmapGPUTransferBuffer(device_, storage.transfer);
        const SDL_GPUTransferBufferLocation source{.transfer_buffer = storage.transfer,
                                                   .offset = 0};
        const SDL_GPUBufferRegion destination{.buffer = storage.buffer, .offset = 0, .size = size};
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        return {};
    }

    void start_frame(const Frame& frame, SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                     std::uint32_t width, std::uint32_t height) {
        in_frame_ = true;
        frame_ = frame;
        frame_commands_ = commands;
        frame_texture_ = target;
        frame_width_ = width;
        frame_height_ = height;
        const auto [scene_width, scene_height] = scene_size(frame, width, height);
        scene_width_ = scene_width;
        scene_height_ = scene_height;
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
        frame_commands_ = nullptr;
        frame_texture_ = nullptr;
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
    // Which formats the GPU samples, by TextureFormat.
    std::array<bool, 4> sampled_{true, false, false, true};
    SDL_GPUBuffer* vertex_buffer_ = nullptr;
    SDL_GPUTransferBuffer* vertex_transfer_ = nullptr;
    std::uint32_t vertex_capacity_ = 0;
    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    std::vector<SDL_GPUTexture*> pending_release_;
    std::unordered_map<ShaderId, GameShader> game_shaders_;
    std::uint32_t next_shader_ = 1;
    std::vector<SDL_GPUGraphicsPipeline*> pending_pipelines_;
    std::array<SDL_GPUComputePipeline*, kComputeKinds> compute_pipelines_{}; // by ComputeKind
    std::unordered_map<ParticlesId, ParticleSystem> particle_systems_;
    std::uint32_t next_particles_ = 1;
    std::vector<SDL_GPUBuffer*> pending_buffers_;
    // Jobs waiting to run, and the words they read.
    std::vector<ParticleJob> particle_jobs_;
    std::vector<Word> particle_words_;
    std::optional<std::pair<ParticleProcess, std::uint32_t>> last_material_;
    std::optional<std::pair<ParticleProcess3D, std::uint32_t>> last_material_3d_;
    std::unordered_map<Particles3DId, ParticleSystem3D> particle_systems_3d_;
    std::uint32_t next_particles_3d_ = 1;
    SDL_GPUBuffer* particle_data_ = nullptr;
    SDL_GPUTransferBuffer* particle_transfer_ = nullptr;
    std::uint32_t particle_capacity_ = 0;
    // Bound where a process pass takes or writes no emissions.
    SDL_GPUBuffer* no_source_ = nullptr;
    SDL_GPUBuffer* no_target_ = nullptr;
    std::unordered_map<MeshId, Mesh> meshes_;
    std::uint32_t next_mesh_ = 1;
    std::array<SDL_GPUShader*, 2> mesh_shaders_{}; // vertex, then fragment
    SDL_GPUShader* clustered_shader_ = nullptr;    // Forward+'s fragment stage
    std::vector<MeshPipeline> mesh_pipelines_;
    // Forward+'s clusters for the frame, made as it is sent, and the buffers they go in.
    LightClusters clusters_;
    StorageBuffer cluster_lights_;
    StorageBuffer fog_items_; // volumetric fog's volumes and VoxelGIs (fog_items)
    StorageBuffer cluster_cells_;
    StorageBuffer probe_buffer_; // the frame's reflection probes
    // The decals' atlas, packed when their textures change, and uploaded then; and the frame's
    // decals.
    DecalAtlasCache decal_atlas_;
    std::uint64_t decal_atlas_uploaded_ = 0;
    StorageBuffer decal_buffer_;
    StorageBuffer decal_atlas_buffer_;
    // The frame's SSAO and SSR, as layers 0 and 1, for the mesh pass; and white layers for frames
    // with neither.
    struct Screen {
        SDL_GPUTexture* texture = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    Screen screen_;
    SDL_GPUTexture* no_screen_ = nullptr;
    // Ray queries on Vulkan where the GPU has them (src/vulkan_rays.hpp), the textures they
    // write a frame's shadows into, and whether this frame traces them.
    std::unique_ptr<VulkanRays> rays_;
    struct RayShadowTextures {
        std::array<SDL_GPUTexture*, 2> textures{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    RayShadowTextures ray_shadows_;
    bool traced_ = false;
    bool ray_failure_logged_ = false;
    std::uint64_t mesh_versions_ = 0;
    // A black atlas for frames with no probes, and the cube captures draw into, before their
    // levels are filtered into an atlas's cube.
    SDL_GPUTexture* no_probes_ = nullptr;
    SDL_GPUTexture* probe_raw_ = nullptr;
    std::uint32_t probe_raw_size_ = 0;
    SDL_GPUGraphicsPipeline* probe_pipeline_ = nullptr; // radiance.hlsl into half floats
    SDL_GPUTexture* depth_ = nullptr;
    SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    // The frame's shadow plan, made at its first mesh draw, and what its passes draw with.
    std::optional<ShadowPlan> shadow_plan_;
    std::array<ShadowAtlasTexture, 2> shadow_atlases_{}; // directional, then positional
    SDL_GPUTexture* no_shadows_ = nullptr; // bound for an atlas the frame doesn't draw
    SDL_GPUTextureFormat shadow_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUSampler* shadow_sampler_ = nullptr;
    std::array<SDL_GPUShader*, 2> shadow_shaders_{};             // vertex, then fragment
    std::array<SDL_GPUGraphicsPipeline*, 3> shadow_pipelines_{}; // by CullMode
    std::uint32_t depth_width_ = 0;
    std::uint32_t depth_height_ = 0;
    // What the post passes draw with: their shaders, a pipeline for each target format, and the
    // meshes' texture and glow's at the frame's size.
    struct PostTargets {
        SDL_GPUTexture* world = nullptr;
        std::array<SDL_GPUTexture*, kGlowLevels> glow{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    std::array<SDL_GPUShader*, 2> post_shaders_{}; // vertex, then fragment
    std::array<SDL_GPUShader*, 2> sky_shaders_{};  // vertex, then fragment
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> post_pipelines_;
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> sky_pipelines_;
    PostTargets post_targets_;
    // What Forward+'s post passes draw with: their shaders, a pipeline for each target format,
    // the world's texture of half floats at the frame's size and glow's levels, made with the
    // first frame that glows.
    struct ForwardTargets {
        SDL_GPUTexture* world = nullptr;
        std::array<SDL_GPUTexture*, kMaxGlowLevels> glow{};
        // Auto exposure's luminance textures, made with the first frame that uses it, and the
        // measured brightness: whether it holds one yet, and the version it was measured for.
        std::vector<SDL_GPUTexture*> luminance;
        SDL_GPUTexture* brightness = nullptr;
        bool measured = false;
        std::uint64_t version = 0;
        // Depth of field's textures, as DofTexture after World, made with the first frame that
        // uses it.
        std::array<SDL_GPUTexture*, 5> dof{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    std::array<SDL_GPUShader*, 2> forward_shaders_{}; // vertex, then fragment
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> forward_pipelines_;
    ForwardTargets forward_targets_;
    // The normal-roughness prepass's normals and depth at the frame's size, for SSAO and SSR.
    struct PrepassTargets {
        SDL_GPUTexture* normals = nullptr;
        SDL_GPUTexture* depth = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    PrepassTargets prepass_;
    // SSAO's textures for the frame's size and settings: those ssao_plan sizes (null for those it
    // doesn't draw).
    struct SsaoTargets {
        std::array<SDL_GPUTexture*, kSsaoTextures> textures{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SsaoSettings settings;
    };
    SDL_GPUShader* normal_shader_ = nullptr;                     // the prepass's fragment stage
    std::array<SDL_GPUGraphicsPipeline*, 3> normal_pipelines_{}; // by CullMode
    // The vertex shader, then the fragment shader, or on Direct3D 12 one for each group of passes
    // (kSsaoShaderGroups), and the pipelines for each.
    std::array<SDL_GPUShader*, 1 + kSsaoShaderGroups.size()> ssao_shaders_{};
    std::array<std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>>,
               kSsaoShaderGroups.size()>
        ssao_pipelines_;
    bool ssao_split_ = false;
    SsaoTargets ssao_targets_;
    // SSR's textures for the frame's size and settings: those ssr_plan sizes (null for those it
    // doesn't draw), and Hi-Z's and the reflections' levels copied into one texture each with
    // mipmaps.
    struct SsrTargets {
        std::array<SDL_GPUTexture*, kSsrTextures> textures{};
        SDL_GPUTexture* hiz_mips = nullptr;
        SDL_GPUTexture* color_mips = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        SsrSettings settings;
    };
    std::array<SDL_GPUShader*, 2> ssr_shaders_{}; // vertex, then fragment
    SDL_GPUShader* ssr_trace_shader_ = nullptr;   // the trace's fragment stage
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> ssr_pipelines_;
    SDL_GPUGraphicsPipeline* ssr_trace_pipeline_ = nullptr;
    SsrTargets ssr_targets_;
    std::vector<std::pair<TextureId, SsrHistory>> ssr_histories_;
    std::array<SDL_GPUShader*, 2> motion_shaders_{};             // TAA's prepass's stages
    std::array<SDL_GPUGraphicsPipeline*, 3> motion_pipelines_{}; // by CullMode
    std::array<SDL_GPUShader*, 2> taa_shaders_{};                // vertex, then fragment
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> taa_pipelines_;
    std::vector<std::pair<TextureId, TaaHistory>> taa_histories_;
    // FSR 2's shaders and pipelines by Fsr2Pass, its reconstruct pass's pipeline, the textures a
    // frame's passes draw into at sizes (the render size, then the display size) and targets'
    // frames before.
    struct Fsr2Textures {
        SDL_GPUTexture* color = nullptr;    // the world the sky and meshes draw into
        SDL_GPUTexture* velocity = nullptr; // the prepass's motion vectors
        SDL_GPUTexture* luma_mip = nullptr;
        SDL_GPUTexture* dilated_depth = nullptr;
        SDL_GPUTexture* lock_luma = nullptr;
        SDL_GPUTexture* prepared = nullptr;
        SDL_GPUTexture* reactive = nullptr;
        SDL_GPUTexture* new_locks = nullptr; // at the display size
        // FSR 3's, whose locks are new_locks.
        SDL_GPUTexture* dilated_motion = nullptr;
        SDL_GPUTexture* farthest_depth = nullptr;
        SDL_GPUTexture* farthest_half = nullptr; // at half the render size, as the next two
        std::array<SDL_GPUTexture*, kFsr3ShadingLevels> levels{};
        SDL_GPUTexture* shading_change = nullptr;
        SDL_GPUTexture* masks = nullptr;
        SDL_GPUTexture* instability = nullptr;
        SDL_GPUBuffer* reconstructed = nullptr;
        // MetalFX's or the game's temporal upscaler's output, at the display size.
        SDL_GPUTexture* scaled = nullptr;
        std::array<std::uint32_t, 4> sizes{};
        bool fsr3 = false;
        bool external = false;
    };
    std::array<std::array<SDL_GPUShader*, 2>, kFsr2Passes> fsr2_shaders_{};
    std::array<SDL_GPUGraphicsPipeline*, kFsr2Passes> fsr2_pipelines_{};
    SDL_GPUComputePipeline* fsr2_reconstruct_ = nullptr;
    // FSR 3's shaders and pipelines by Fsr3Pass and its reconstruct pass's pipeline.
    std::array<std::array<SDL_GPUShader*, 2>, kFsr3Passes> fsr3_shaders_{};
    std::array<SDL_GPUGraphicsPipeline*, kFsr3Passes> fsr3_pipelines_{};
    SDL_GPUComputePipeline* fsr3_reconstruct_ = nullptr;
    // The MetalFX scalers the device runs, and metalfx_spatial's scaler.
    MetalFxSupport metalfx_;
    MetalFxScalerPtr metalfx_spatial_scaler_;
    // Volumetric fog: each target's frames, the passes' pipeline, the sky's shaders and pipelines,
    // and the buffer the meshes bind in its place without it.
    std::vector<std::pair<TextureId, FogFrames>> fog_frames_;
    SDL_GPUComputePipeline* fog_pipeline_ = nullptr;
    std::array<SDL_GPUShader*, 2> fog_sky_shaders_{};
    std::vector<std::pair<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>> fog_sky_pipelines_;
    SDL_GPUBuffer* no_fog_ = nullptr;
    Fsr2Textures fsr2_textures_;
    std::vector<std::pair<TextureId, Fsr2Frames>> fsr2_frames_;
    // The radiance meshes are lit by: the raw cube the sky draws into and the one filtered from
    // it, and what they hold (nullopt to draw them again).
    struct RadianceCubes {
        SDL_GPUTexture* raw = nullptr;
        SDL_GPUTexture* filtered = nullptr;
        std::uint32_t size = 0;
        std::optional<RadianceKey> key;
    };
    RadianceCubes radiance_;
    std::array<SDL_GPUShader*, 2> radiance_shaders_{}; // vertex, then fragment
    // Drawing the sky into the raw cube, then filtering it.
    std::array<SDL_GPUGraphicsPipeline*, 2> radiance_pipelines_{};
    SDL_GPUSampler* radiance_sampler_ = nullptr;
    SDL_GPUTexture* no_radiance_ = nullptr; // bound when the frame's meshes have none
    bool radiance_prepared_ = false;        // at the frame's first mesh draw
    bool radiance_draws_ = false;           // whether end_frame draws the radiance again

    // The frame being built. A skipped frame has no command buffer.
    bool in_frame_ = false;
    Frame frame_;
    SDL_GPUCommandBuffer* frame_commands_ = nullptr;
    SDL_GPUTexture* frame_texture_ = nullptr;
    SDL_GPUTextureFormat frame_format_ = kTextureFormat;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    // The size its sky and meshes draw at: FSR 2's render size, or the frame's.
    std::uint32_t scene_width_ = 0;
    std::uint32_t scene_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    // The frame's mesh draws, and what each binds.
    std::vector<MeshDraw> mesh_draws_;
    std::vector<MeshBatch> mesh_batches_;
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

namespace {

// The shader formats the device has shaders in.
constexpr SDL_GPUShaderFormat kShaderFormats =
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | kHlslFormats;

// SDL's name for a driver's backend, or null for "auto".
const char* sdl_driver_name(const std::string& driver) {
    if (driver == "d3d12") {
        return "direct3d12";
    }
    return driver == "auto" ? nullptr : driver.c_str();
}

} // namespace

bool gpu_backend_works(const std::string& driver) {
    // SDL counts starts, so stopping after leaves video as it was.
    if (auto started = init_video(); !started) {
        return false;
    }
    const bool works = SDL_GPUSupportsShaderFormats(kShaderFormats, sdl_driver_name(driver));
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return works;
}

// SDL's device for options: on Vulkan, with ray queries where the GPU has them
// (VulkanRayOptions), else as SDL makes it.
SDL_GPUDevice* create_sdl_device(const DeviceOptions& options) {
    const VulkanRayOptions rays;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN,
                           (kShaderFormats & SDL_GPU_SHADERFORMAT_DXBC) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, options.debug);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                          sdl_driver_name(options.driver));
    SDL_GPUDevice* device = nullptr;
    if (rays.options() != nullptr) {
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
                               rays.options());
        if (SDL_GPUSupportsProperties(props)) {
            device = SDL_CreateGPUDeviceWithProperties(props);
        }
    }
    SDL_DestroyProperties(props);
    if (device == nullptr) {
        device =
            SDL_CreateGPUDevice(kShaderFormats, options.debug, sdl_driver_name(options.driver));
    }
    return device;
}

Result<std::unique_ptr<RenderDevice>> create_gpu_device(const DeviceOptions& options) {
    const bool owns_video = options.window == nullptr;
    if (owns_video) {
        if (auto started = init_video(); !started) {
            return started.error();
        }
    }
    SDL_GPUDevice* device = create_sdl_device(options);
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
