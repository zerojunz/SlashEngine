// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include "slash/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace slash::jobs {
class Pool;
}

// Texture graph programs: what a texture graph (a .texture.json file of "type": "graph",
// slash/procedural/texture_graph.hpp) compiles to, and what devices bake. A program is a list
// of passes, each one op over whole images: it reads up to four slots and writes one, every
// pixel at once, so a device runs a pass as one compute dispatch (shaders/texture_graph.hlsl)
// and run_texture_graph, its CPU twin, as a loop over rows. Slots hold RGBA as floats, 0 to 1
// for colors though passes may go past. Coordinates are a pixel's center in UV: ((x + 0.5) /
// width, (y + 0.5) / height), rows from the top. A program that tiles reads its neighbours
// across the edges (wrapped); one that doesn't clamps at them.
//
// The numbers of ops and the layout of their params are part of shaders/texture_graph.hlsl, so
// they never change; new ops go at the end.
namespace slash::render {

// Floats a pass carries for its op.
inline constexpr std::size_t kGraphParams = 48;
// Inputs a pass reads.
inline constexpr std::size_t kGraphInputs = 4;
// An input a pass leaves out.
inline constexpr std::uint32_t kNoGraphSlot = 0xFFFFFFFFU;
// The most slots, passes and images a program holds.
inline constexpr std::uint32_t kMaxGraphSlots = 32;
inline constexpr std::uint32_t kMaxGraphPasses = 512;
inline constexpr std::uint32_t kMaxGraphImages = 8;
// The largest side a graph bakes at, and the most bytes its slots and images may take.
inline constexpr std::uint32_t kMaxGraphSize = 4096;
inline constexpr std::uint64_t kMaxGraphBytes = std::uint64_t{1} << 30U;
// Where a ramp starts in a pass's params (gradient and gradient_map): the number of points,
// then each point's offset and RGBA.
inline constexpr std::size_t kGraphRampAt = 16;
inline constexpr std::size_t kMaxGraphRampPoints = 6;

enum class GraphOp : std::uint8_t {
    // Generators: no inputs.
    Constant = 0, // p0-3: the color
    // FastNoiseLite as a FastNoiseLite generator sets it up: p0 noise type, p1 seed, p2 frequency,
    // p3 fractal type, p4 octaves, p5 lacunarity, p6 gain, p7 weighted strength, p8 ping-pong
    // strength, p9 cellular distance, p10 cellular return, p11 cellular jitter, p12 1 to warp the
    // domain, p13 warp type, p14 warp amplitude, p15 warp frequency, p16 warp fractal, p17 warp
    // octaves, p18 warp lacunarity, p19 warp gain (the generator's enum numbers), p20-22 the
    // offset, p23-24 the noise units across and down (the pixel size at 1x), p25 1 to make it
    // seamless, p26 its blend skirt. Gray: value * 0.5 + 0.5.
    Noise = 1,
    // A 2D gradient texture: p0 fill (0 linear, 1 radial, 2 square), p1-2 fill_from, p3-4 fill_to,
    // p5 repeat (0 none, 1 repeat, 2 mirror), the ramp at kGraphRampAt.
    Gradient = 2,
    // A shape in each of p1 x p2 cells: p0 kind (0 disc, 1 box, 2 polygon, 3 ring), p3 size
    // (share of the cell), p4 the polygon's sides, p5 softness (share of the size its edge
    // fades over), p6 rotation (turns), p7 the ring's thickness (share of the size). Gray.
    Shape = 3,
    // Bricks: p0 across, p1 down, p2 mortar (share of a brick), p3 each row's shift (share of a
    // brick), p4 bevel (share of a brick), p5 seed. r: the brick's height (0 in the mortar,
    // rising over the bevel to 1), g: a random value per brick, b: the brick's column and row
    // hashed to 0-1, a: 1.
    Bricks = 4,
    Checker = 5, // p0 across, p1 down. Gray.
    // Bands: p0 how many, p1 angle (turns), p2 shape (0 sine, 1 triangle, 2 square), p3 phase
    // (turns). Gray.
    Waves = 6,
    Image = 7, // p0 the image (TextureGraphProgram::images), sampled bilinearly

    // Filters: in0 the image.
    Levels = 8,       // p0-1 in low and high, p2 gamma, p3-4 out low and high; RGB
    Invert = 9,       // RGB = 1 - RGB
    Clamp = 10,       // every channel to 0-1
    GradientMap = 11, // in0's luminance through the ramp at kGraphRampAt
    Hsv = 12,         // p0 hue shift (turns), p1 saturation times, p2 value times
    // One axis of a blur (graphs blur across, then down): p0 radius in pixels, p1 axis (0 x,
    // 1 y). Each tap weighs (1 - (d / (radius + 1))^2)^2.
    Blur = 13,
    Sharpen = 14, // p0 amount: the color plus amount times its difference from its 4 neighbours
    // in0 moved by in1: p0 strength (UV), p1 mode (0 down in1's slope, 1 along p2's angle in
    // turns times in1's red).
    Warp = 15,
    Transform = 16,  // p0-1 repeats, p2 rotation (turns) about the center, p3-4 offset (UV)
    EdgeDetect = 17, // the Sobel magnitude of in0's luminance times p0
    Grayscale = 18,  // RGB = luminance
    Threshold = 19,  // smoothstep(p0 - p1, p0 + p1, luminance)
    Posterize = 20,  // RGB in p0 steps
    // Channels from four inputs: out channel c is input c's channel p<c> (0-3), or p<4+c> where
    // input c is left out.
    Merge = 21,
    Channels = 22, // out channel c is in0's channel p<c>: 0-3, 4 for 0, 5 for 1

    // Height (in0's red).
    NormalFromHeight = 23, // p0 strength: tangent-space normals as 0.5 + 0.5 n, green up (OpenGL)
    AoFromHeight = 24,     // p0 radius in pixels, p1 strength
    CurvatureFromHeight = 25, // 0.5 plus p0 times the Laplacian

    // in0 the base, in1 the layer, in2 a mask's red (left out: 1). p0 mode (0 mix, 1 add, 2
    // subtract, 3 multiply, 4 screen, 5 overlay, 6 darken, 7 lighten, 8 difference, 9 soft
    // light, 10 divide), p1 opacity. Alpha: the base's.
    Blend = 26,
    // in0 and in1 (left out: p2-5): p0 op (0 add, 1 subtract, 2 multiply, 3 divide, 4 min, 5
    // max, 6 pow, 7 abs, 8 fract, 9 one minus).
    Math = 27,
    Remap = 28, // p0-1 from, p2-3 to, p4 1 to clamp
};

// The op's name in a texture graph file ("noise", "gradient_map").
[[nodiscard]] std::string_view graph_op_name(GraphOp op);

struct GraphPass {
    GraphOp op = GraphOp::Constant;
    std::uint32_t out = 0;
    std::array<std::uint32_t, kGraphInputs> in{kNoGraphSlot, kNoGraphSlot, kNoGraphSlot,
                                               kNoGraphSlot};
    std::array<float, kGraphParams> params{};
};

// An image a program samples: RGBA floats, rows from the top.
struct GraphImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> texels;
};

struct TextureGraphProgram {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool tiling = false;
    std::uint32_t slots = 0;
    std::vector<GraphPass> passes;
    std::vector<GraphImage> images;
};

// A baked slot: width x height RGBA floats.
using GraphTexels = std::vector<float>;

// Checks a program: its size, slots, passes, inputs and images within bounds, every input
// written by an earlier pass, every op known.
[[nodiscard]] Result<void> check_texture_graph(const TextureGraphProgram& program);

// Bytes a program's slots and images take while it bakes.
[[nodiscard]] std::uint64_t texture_graph_bytes(const TextureGraphProgram& program);

// The CPU twin of shaders/texture_graph.hlsl: runs every pass, rows shared out over pool (null:
// on this thread), and returns the slots asked for, in order. The same on every machine.
[[nodiscard]] Result<std::vector<GraphTexels>>
run_texture_graph(const TextureGraphProgram& program, std::span<const std::uint32_t> outputs,
                  jobs::Pool* pool = nullptr);

} // namespace slash::render
