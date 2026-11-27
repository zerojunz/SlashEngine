#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Frame generation from a vendor's SDK (DLSS Frame Generation and Multi Frame Generation, XeSS
// Frame Generation, FSR's own from AMD's SDK), in place of the engine's FSR 3 frame generation
// (RenderDevice::generate_frame). A game adds one with add_frame_generator, from an SDK package's
// glue code, as it adds a temporal upscaler (upscaler.hpp), and names it in project.json's
// rendering.frame_generator. The device keeps a frame's depth and motion vectors for it and,
// when the renderer asks for frames between the last drawn one and the one before
// (RenderDevice::generate_frames), hands it the frame's textures as the native API's objects:
// on the GPU device on Vulkan, Direct3D 12 and Metal, and on the software device as floats. On
// the other devices, or where its supports says no, the engine's own generates one frame.
namespace slash::render {

// The name of the engine's own frame generation, AMD's FSR 3.
inline constexpr std::string_view kFsr3FrameGeneration = "fsr3";
// The most frames a generator makes between two drawn ones, as DLSS 4's Multi Frame Generation.
inline constexpr std::uint32_t kMaxGeneratedFrames = 3;

// What a frame generator gets for a drawn frame, as GPU objects, by TemporalFrame's rules: the
// GPU device's command buffer, recording, outside any pass; the textures in the state shaders
// sample them in, outputs included, which the generator leaves them in, moving the outputs to be
// written and back; on Direct3D 12 the device sets its own descriptor heaps again after. On the
// software device, commands is null and the textures' pixels hold four floats a pixel (one for
// depth, two for motion), rows top to bottom.
struct GeneratedFrames {
    NativeDevice device;
    void* commands = nullptr;
    // The size the 3D world was drawn at (depth and motion), then the size frames are shown at
    // (color and outputs).
    UpscaleSize size;
    // The frame just drawn, as it shows: RGBA, 8 bits a channel (floats from 0 to 1 on the
    // software device), the world with the canvases below it and none of the controls over it.
    NativeTexture color;
    // The 3D world's depth and motion vectors at the render size: reversed depth, 0 where
    // nothing drew; motion in texture coordinates with y down, a pixel was at its own plus its
    // motion a frame before. Where nothing drew the motion is no use: the sky's moved only as
    // the camera turned, which the generator works out from the depth and the cameras, as
    // vendors' SDKs do. Both are empty textures (no image, no pixels) for a frame that drew no
    // meshes.
    NativeTexture depth;
    NativeTexture motion;
    // The frames to write, as a render pass writes them, earliest first: outputs[i] is the frame
    // (i + 1) / (outputs.size() + 1) of the way from the frame before to color.
    std::span<const NativeTexture> outputs;
    // Whether the frame before belongs to another view: a first frame, a change of size, or a
    // frame the target drew without generating from.
    bool reset = false;
    // The camera's projection without the jitter (reversed depth) and the world to the camera's
    // space, column-major; identity for a frame with no 3D world.
    Mat4 projection{};
    Mat4 camera{};
};

// Makes frames between two drawn ones. One belongs to one device's target; the device makes it
// when a frame first asks for it and drops it with the target.
class FrameGenerator {
public:
    FrameGenerator() = default;
    FrameGenerator(const FrameGenerator&) = delete;
    FrameGenerator& operator=(const FrameGenerator&) = delete;
    FrameGenerator(FrameGenerator&&) = delete;
    FrameGenerator& operator=(FrameGenerator&&) = delete;
    virtual ~FrameGenerator() = default;

    // Writes frames' outputs. A failure is returned by RenderDevice::generate_frames, and the
    // renderer shows only the drawn frame.
    [[nodiscard]] virtual Result<void> generate(const GeneratedFrames& frames) = 0;
};

// A frame generator a game can choose by name.
struct FrameGeneratorInfo {
    std::string name;
    // The most frames it makes between two drawn ones, 1 to kMaxGeneratedFrames.
    std::uint32_t max_frames = 1;
    // Whether it runs on a device, such as a vendor's on its own GPUs; always, when empty. It runs
    // only on the GPU device on Vulkan, Direct3D 12 or Metal and on the software device whatever
    // this says.
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<bool(const RenderDevice& device)> supports{};
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<std::unique_ptr<FrameGenerator>()> make{};
};

// Adds a frame generator for rendering.frame_generator to name, for the whole process, as an SDK
// package's register function can. Fails for one without make, a max_frames outside 1 to
// kMaxGeneratedFrames, a name already taken ("fsr3" too) or one that isn't lowercase letters,
// digits and underscores.
[[nodiscard]] Result<void> add_frame_generator(FrameGeneratorInfo info);
// The frame generator added with that name, or nullopt ("fsr3", the engine's own, is not one).
[[nodiscard]] std::optional<FrameGeneratorInfo> find_frame_generator(std::string_view name);
// Every frame generator's name: "fsr3", then those added, in order.
[[nodiscard]] std::vector<std::string> frame_generator_names();
// Forgets the frame generators add_frame_generator added, for tests.
void clear_added_frame_generators();

// Whether a device can run a game's frame generators: the GPU device on Vulkan, Direct3D 12 or
// Metal, and the software device.
[[nodiscard]] bool runs_frame_generators(const RenderDevice& device);

// What a frame generates with on a device, for the settings' rendering.frame_generator and
// rendering.generated_frames: the added generator of that name and how many frames it makes
// (the count asked for, at most its max_frames), where it runs there; else the engine's own
// ("" for the name, "fsr3" or one that doesn't run) and one frame.
struct FrameGeneratorChoice {
    std::string name; // empty for the engine's own
    std::uint32_t frames = 1;
};
[[nodiscard]] FrameGeneratorChoice
choose_frame_generator(std::string_view name, std::uint32_t frames, const RenderDevice& device);

} // namespace slash::render
