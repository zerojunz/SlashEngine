#pragma once

#include "taa.hpp"

#include "slash/render/device.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to generate a frame between two drawn ones with AMD's FidelityFX SDK
// 1.1.4's frame generation, the frame interpolation and optical flow of FSR 3.1, which Godot
// doesn't have: the passes' sizes and the CPU twin of their shaders, which the software device
// runs. A frame that asks for it (Scene3D::frame_generation) keeps its prepass's depth and motion
// vectors, dilated and pushed back to where they were in the frame before (ffxFrameInterpolation-
// Prepare: prepare_frame_generation). Generating a frame (RenderDevice::generate_frame) then finds
// how the picture moved since the frame before by blocks of 8 x 8 pixels (the optical flow), lays
// the game's vectors and the optical flow's over the frame halfway between as vector fields,
// works out what each of the two frames can't see, blends the two frames moved halfway along the
// vectors, and fills what neither shows from a pyramid of what was blended.
//
// It follows AMD's passes line for line, with a few changes so that every device makes the same
// pixels: the game's vector field writes every primary vector before any secondary one (AMD's
// pass writes both at once, so a secondary vector stops on whichever primaries got there first),
// the scene change pass reads its histograms before any group clears them, the pyramids are made
// a level a pass where AMD's downsampler makes them in one, and textures AMD samples with a
// sampler are filtered in code.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;

// The camera of a frame with no scene.
inline constexpr Mat4 kNoCamera{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

// The optical flow's levels and its blocks' size in pixels.
inline constexpr std::uint32_t kOpticalFlowLevels = 7;
inline constexpr std::uint32_t kOpticalFlowBlock = 8;

// The optical flow's vectors at a level: a block of 8 x 8 pixels each, rounded up, then each
// coarser level half the last, rounded up.
[[nodiscard]] std::array<std::uint32_t, 2>
optical_flow_size(std::uint32_t width, std::uint32_t height, std::uint32_t level);

// What the optical flow keeps between frames: the luma pyramid of the frame before (8 bits a
// pixel, the frame's size then each level half the last, rounded down), the scene change pass's
// filtered histograms of it (9 regions of 256 bins) and its history of scene changes (a bit a
// frame, the last frame's lowest), and frames since the last reset.
struct OpticalFlowHistory {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::vector<std::uint8_t>, kOpticalFlowLevels> luma;
    std::vector<float> scene_histograms;
    std::uint32_t scene_history = 0;
    std::uint32_t frame_index = 0;
};

// A frame's optical flow: for each block of 8 x 8 pixels (optical_flow_size at level 0), where
// it was in the frame before in whole pixels, x then y; and the scene change history after it.
struct OpticalFlow {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::int32_t> vectors;
    std::uint32_t scene_history = 0;
};

// The CPU twin of the optical flow's passes over a frame (RGBA, 8 bits a channel, rows top to
// bottom): its luma pyramid, the scene change detection and the block search from the coarsest
// level to the finest, each level's vectors filtered and scaled to the next. A reset starts the
// history again.
[[nodiscard]] OpticalFlow run_optical_flow(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                           std::uint32_t height, bool reset,
                                           OpticalFlowHistory& history);

// What a frame's camera tells frame generation: AMD's deviceToViewDepth from its projection (as
// FSR 2's), whether it is orthogonal, and the tangent of half its horizontal field of view.
struct FrameGenerationCamera {
    Float4 view_depth{};
    bool ortho = false;
    float tan_half_fov = 1.0F;
};
// From a projection, as Fsr2Uniforms::view_depth is made.
[[nodiscard]] FrameGenerationCamera frame_generation_camera(const Mat4& projection);

// A frame's prepass as frame generation takes it, at the size it was drawn (the render size):
// the depth (reversed: 1 at the near plane, 0 where nothing drew), its motion vectors (two a
// pixel in texture coordinates, to where each was in the frame before; a pixel where nothing drew
// is worked out from the depth with reprojection, as derive_motion_vector), and the camera.
struct FrameGenerationInputs {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const float> depth;
    std::span<const float> motion;
    std::array<Float4, 4> reprojection{};
    FrameGenerationCamera camera;
};

// What ffxFrameInterpolationPrepare leaves for the frame's generation, at the render size: each
// pixel's nearest depth among its neighbors, the motion vector there (two half floats a pixel),
// and the depth pushed back to where it was in the frame before (a float's bits, the nearest
// kept), with the camera.
struct FramePrepared {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> dilated_depth;
    std::vector<float> dilated_motion;
    std::vector<std::uint32_t> previous_depth;
    FrameGenerationCamera camera;
};

// The CPU twin of the prepare pass (ReconstructAndDilate) for a frame shown at display_width x
// display_height.
[[nodiscard]] FramePrepared prepare_frame_generation(const FrameGenerationInputs& inputs,
                                                     std::uint32_t display_width,
                                                     std::uint32_t display_height);

// What frame generation keeps between a target's frames: the frame before as it was shown (RGBA,
// 8 bits a channel), the frames since the last reset or scene change, and the optical flow's.
struct FrameGenerationHistory {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> previous;
    std::uint32_t frames = 0;
    OpticalFlowHistory flow;
};

// The CPU twin of the passes: into out (RGBA, 8 bits a channel, display size), the frame halfway
// between the history's frame before and current (the same size), with current's prepare pass
// (render size). A reset, or a first frame, gives current as it is. The history then holds
// current.
void run_frame_generation(std::span<const std::uint8_t> current, std::uint32_t width,
                          std::uint32_t height, const FramePrepared& prepared, bool reset,
                          FrameGenerationHistory& history, std::span<std::uint8_t> out);

// --- The GPU devices' passes (shaders/frame_generation.hlsl) ---------------------------------

// The passes, as the shader numbers them.
enum class FrameGenerationPass : std::uint8_t {
    ClearWords = 0,
    ClearAtoms,
    CopyWords,
    Prepare,
    Pack,
    Luma,
    Histogram,
    SceneChange,
    Search,
    Filter,
    Scale,
    Setup,
    Reconstruct,
    GamePrimary,
    GameSecondary,
    GamePyramid,
    FlowField,
    Disocclusion,
    Interpolate,
    InpaintPyramid,
    Inpaint,
};

// The parts of the two buffers a target's frame generation works in, as the shader numbers them:
// words, then atoms (written by atomic max or add). The frame before's colors and lumas, the scene
// change histograms and the state are kept between frames; the dilated depth and motion and the
// previous depth are the last frame's prepare pass.
enum class FrameGenerationSection : std::uint8_t {
    PrevRgba = 0,
    CurRgba,
    PrevLuma,
    CurLuma,
    SceneKept,
    State,
    Searched,
    Vectors,
    Prediction,
    GamePyramid,
    Disocclusion,
    OutRgba,
    Mask,
    InpaintPyramid,
    DilatedDepth,
    DilatedMotion,
    PreviousDepth, // atoms from here
    InterpolatedDepth,
    GameField,
    FlowField,
    Histograms,
    Count,
};

// Where each section starts, in uints, and the two buffers' sizes in uints, for frames drawn at
// the render size and shown at the display size.
struct FrameGenerationLayout {
    std::array<std::uint32_t, 2> render{};
    std::array<std::uint32_t, 2> display{};
    std::array<std::uint32_t, static_cast<std::size_t>(FrameGenerationSection::Count)> sections{};
    std::uint32_t words = 0;
    std::uint32_t atoms = 0;
};
[[nodiscard]] FrameGenerationLayout frame_generation_layout(std::uint32_t render_width,
                                                            std::uint32_t render_height,
                                                            std::uint32_t display_width,
                                                            std::uint32_t display_height);

// The shader's uniforms (272 bytes).
struct FrameGenerationUniforms {
    std::array<std::uint32_t, 4> step{}; // the pass, its level, and two numbers it takes
    std::array<std::int32_t, 4> sizes{}; // the render size, then the display size
    std::array<std::int32_t, 4> flow_sizes{};
    Float4 view_depth{};
    Float4 camera{}; // 1 when orthogonal, unused, 1 over the display size
    std::array<Float4, 4> reprojection{};
    std::array<std::uint32_t, 32> sections{};
};
static_assert(sizeof(FrameGenerationUniforms) == 272);

// A dispatch of the shader: its uniforms and thread groups of 8 x 8.
struct FrameGenerationDispatch {
    FrameGenerationUniforms uniforms;
    std::array<std::uint32_t, 3> groups{};
};

// The prepare pass's dispatches for a frame, from its prepass (the depth and motion textures), or
// with none (still: nothing near, moved as its camera turned), with the reprojection to the frame
// before and the camera.
[[nodiscard]] std::vector<FrameGenerationDispatch>
frame_generation_prepare_plan(const FrameGenerationLayout& layout,
                              const std::array<Float4, 4>& reprojection,
                              const FrameGenerationCamera& camera, bool still);
// Generating a frame's dispatches, after the pack pass reads the frame. A reset starts the
// history again.
[[nodiscard]] std::vector<FrameGenerationDispatch>
frame_generation_plan(const FrameGenerationLayout& layout, const FrameGenerationCamera& camera,
                      bool reset);

} // namespace slash::render::detail
