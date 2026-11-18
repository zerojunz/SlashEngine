#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// A frame's compositor effects, as Godot 4.3's Compositor and CompositorEffect: a game's own
// screen passes, each a compositor shader (ShaderType::Compositor) drawn over the 3D world at one
// point of a Forward+ frame. Godot runs a CompositorEffect's callback with the render buffers to
// dispatch compute shaders of its own; here each effect is a fragment() over the screen, which
// reads the world as it was drawn so far (screen_texture), its depth (depth_texture) and its
// normals and roughness (normal_roughness_texture), and writes COLOR, the world's new color. The
// effects come from a project's .pipeline.json files (scene3d), whose C++ callbacks may change
// them each frame. Compatibility and the OpenGL devices draw none, as Godot's Compatibility
// renderer has no compositor.
namespace slash::render {

// NOLINTNEXTLINE(performance-enum-size): matches shader.hpp's declaration
enum class ShaderId : std::uint32_t;
// NOLINTNEXTLINE(performance-enum-size): matches device.hpp's declaration
enum class TextureId : std::uint32_t;

// Where in the frame an effect runs, as Godot's EffectCallbackType. The sky is drawn before the
// meshes, so pre_opaque runs over the sky; post_opaque, post_sky and pre_transparent run in that
// order after the opaque meshes (and subsurface scattering), and post_transparent after the
// blended meshes, before TAA, FSR and post-processing.
enum class EffectCallback : std::uint8_t {
    PreOpaque,
    PostOpaque,
    PostSky,
    PreTransparent,
    PostTransparent
};
inline constexpr std::size_t kEffectCallbacks = 5;

// How many texture params a compositor shader takes, bound after the screen's three textures.
inline constexpr std::uint32_t kCompositorTextures = 4;
// How many effects a frame draws.
inline constexpr std::size_t kMaxCompositorEffects = 16;

// One effect of a frame: its compiled shader, where it runs, its material's 32 constants (params,
// the time in kShaderTimeConstant's x and WIND's slot) and its texture params. normal_roughness
// asks for the prepass's normals and roughness, as Godot's needs_normal_roughness: a frame with
// such an effect draws the prepass.
struct CompositorEffect3D {
    ShaderId shader{};
    EffectCallback callback = EffectCallback::PostTransparent;
    std::array<std::array<float, 4>, 32> constants{};
    std::array<TextureId, kCompositorTextures> textures{};
    bool normal_roughness = false;

    bool operator==(const CompositorEffect3D&) const = default;
};

// What a compositor shader's uniform buffer holds (b0 in space3 of its fragment stage, binding 0
// of its CPU module): its constants, then Godot's PROJECTION_MATRIX, INV_PROJECTION_MATRIX,
// VIEW_MATRIX and INV_VIEW_MATRIX (column-major), the screen (1 over its width and height, then
// its width and height in pixels) and options (x: 1 when normal_roughness_texture reads the
// prepass's; 0 reads a normal facing the camera and a roughness of 1).
struct CompositorUniforms {
    std::array<std::array<float, 4>, 32> constants{};
    std::array<float, 16> projection{};
    std::array<float, 16> inv_projection{};
    std::array<float, 16> view{};
    std::array<float, 16> inv_view{};
    std::array<float, 4> screen{};
    std::array<std::uint32_t, 4> options{};
};
static_assert(sizeof(CompositorUniforms) == 800);

// Where a compositor shader's CPU module (CompiledShader::cpu_fragment) reads and writes, and its
// textures' bindings, which are the GPU stage's too: the screen at 0, the depth at 1, the normals
// and roughness at 2, then the texture params.
namespace cpu_compositor {
inline constexpr std::uint32_t kFragCoord = 0; // the pixel's center, xy
inline constexpr std::uint32_t kOutColor = 0;
inline constexpr std::uint32_t kScreenBinding = 0;
inline constexpr std::uint32_t kDepthBinding = 1;
inline constexpr std::uint32_t kNormalBinding = 2;
inline constexpr std::uint32_t kFirstParamBinding = 3;
} // namespace cpu_compositor

} // namespace slash::render
