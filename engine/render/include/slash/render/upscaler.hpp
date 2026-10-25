#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Drawing the world smaller than the screen and scaling it up, as Godot's scaling_3d settings:
// the canvas renderer draws the world into an upscaler's input at the render size, the upscaler
// scales it to the screen's size, and the controls draw over it at full size. "bilinear" (a
// filtered stretch) and "fsr" (AMD's FidelityFX Super Resolution 1: shaders/fsr_easu.hlsl and
// fsr_rcas.hlsl) are built in, drawn with the device's own calls, so they run on every backend
// and the software device; so is "metalfx_spatial" (Apple's MetalFX spatial scaler), on a device
// that has it. A game adds others with add_upscaler, from an SDK package's glue code for one that
// ships as a vendor's closed library. "fsr2" (FidelityFX Super Resolution 2), "fsr3" (FSR 3.1's
// upscaler, which Godot doesn't have) and "metalfx_temporal" (MetalFX's temporal scaler) are the
// 3D renderer's own instead: a Forward+ frame draws its 3D smaller and upscales it in the device
// with the depth buffer and motion vectors, and the 2D draws over it at full size, as Godot's.
// Where MetalFX isn't, its modes fall back as Godot's do (scaling_mode_on). A game adds temporal
// upscalers of its own (DLSS, XeSS, FSR 4 from a vendor's SDK) with add_upscaler too: the device
// runs one in place of FSR 2, handing it the frame's textures as the native API's objects
// (TemporalFrame), as Godot hands its RenderingDevice's to a CompositorEffect.
namespace slash::render {

// How a frame's world is scaled, as Godot's rendering/scaling_3d settings (project.json's
// rendering.scaling).
struct Scaling {
    // "bilinear", "fsr", "fsr2", "fsr3", "metalfx_spatial" or "metalfx_temporal", as Godot's
    // scaling_3d/mode.
    std::string mode = "bilinear";
    // The render size over the screen's, from 0.25 to 2: below 1 draws fewer pixels and scales
    // up, above 1 draws more and scales down (always bilinear, as FSR only scales up).
    float scale = 1.0F;
    // FSR's, FSR 2's and FSR 3's sharpening in stops: 0 is the sharpest, 2 the softest.
    float fsr_sharpness = 0.2F;
    // An upscaler the game added with add_upscaler, used instead of mode where it runs; mode
    // otherwise. Empty for none.
    std::string upscaler;

    bool operator==(const Scaling&) const = default;
};

// Settings from JSON ({"mode", "scale", "fsr_sharpness", "upscaler"}), each left at its default
// when missing or of the wrong type, and the numbers clamped to their ranges.
[[nodiscard]] Scaling scaling_from_json(const json::Value& value);
// The settings as JSON, every field but an empty upscaler.
[[nodiscard]] json::Value scaling_json(const Scaling& scaling);

inline constexpr float kMinRenderScale = 0.25F;
inline constexpr float kMaxRenderScale = 2.0F;
inline constexpr float kMaxFsrSharpness = 2.0F;

// The built-in modes.
inline constexpr std::string_view kBilinear = "bilinear";
inline constexpr std::string_view kFsr = "fsr";
inline constexpr std::string_view kFsr2 = "fsr2";
inline constexpr std::string_view kFsr3 = "fsr3";
inline constexpr std::string_view kMetalFxSpatial = "metalfx_spatial";
inline constexpr std::string_view kMetalFxTemporal = "metalfx_temporal";
[[nodiscard]] std::vector<std::string_view> scaling_modes();
// Why a mode isn't one, for project validation: unknown, with the closest name. Empty for a
// built-in mode.
[[nodiscard]] std::string scaling_mode_problem(std::string_view mode);
// The mode a device runs for these settings, as Godot's RendererViewport falls back:
// metalfx_temporal where MetalFX's temporal scaler isn't becomes metalfx_spatial, or fsr2 where
// that isn't either, and fsr2 at a scale outside the temporal scaler's (MetalFxSupport);
// metalfx_spatial where it isn't becomes fsr; an unknown mode is bilinear. Every other mode is
// itself.
[[nodiscard]] std::string_view scaling_mode_on(const Scaling& scaling, const RenderDevice& device);

// Whether a frame with these settings upscales its 3D with FSR 2, FSR 3, MetalFX's temporal
// scaler (scaling_mode_on is fsr2, fsr3 or metalfx_temporal) or the game's own temporal upscaler
// (temporal_upscaler_on) on this device: in Forward+ (method), at a scale under 1, on a device
// that draws Forward+ (every one but OpenGL's), as Godot runs them. The frame's Scene3D then
// takes the scale, sharpness and which upscaler (render::Fsr2Scaling) and no upscaler scales the
// world; choose_upscaler's bilinear scaling stands in otherwise, as Godot falls back to it.
[[nodiscard]] bool uses_fsr2(const Scaling& scaling, RenderMethod method,
                             const RenderDevice& device);

// The temporal upscaler the game added that a frame with these settings runs on this device in
// place of FSR 2 (render::Fsr2Scaling::upscaler): Scaling::upscaler, if add_upscaler added it as
// temporal and it runs here. Empty otherwise, or where uses_fsr2 is false.
[[nodiscard]] std::string temporal_upscaler_on(const Scaling& scaling, RenderMethod method,
                                               const RenderDevice& device);

// The render size for a screen of output pixels, as Godot sizes its render buffers: output times
// scale, rounded down, from 1 to kMaxTextureSize.
[[nodiscard]] std::uint32_t render_size(std::uint32_t output, float scale);

// A frame's sizes in pixels: the world's render and the screen it is scaled to.
struct UpscaleSize {
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;

    bool operator==(const UpscaleSize&) const = default;
};

// What the screen's frame draws with to show the upscaled world: a texture over the whole
// viewport, its texture coordinates from (0, 0) to (1, 1), with a material.
struct UpscaleDraw {
    TextureId texture = TextureId::None;
    Material material;
};

// Scales a frame's world up to the screen. One belongs to one device, which it makes its
// textures and shaders on; the renderer calls input, draws the world into it, then calls upscale
// outside any frame and draws what it returns in the screen's frame.
class Upscaler {
public:
    Upscaler() = default;
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;
    Upscaler(Upscaler&&) = delete;
    Upscaler& operator=(Upscaler&&) = delete;
    virtual ~Upscaler() = default;

    // The render target the world draws into, render_width x render_height: made again when
    // the size changes.
    [[nodiscard]] virtual Result<TextureId> input(RenderDevice& device,
                                                  const UpscaleSize& size) = 0;
    // Scales what was drawn into the input, running any passes of its own in frames of its own,
    // and returns how to draw the result over the screen.
    [[nodiscard]] virtual Result<UpscaleDraw> upscale(RenderDevice& device, const UpscaleSize& size,
                                                      const Scaling& scaling) = 0;
    // Destroys what it made on the device, before the device goes or another upscaler takes over.
    virtual void release(RenderDevice& device) = 0;
};

// The objects of the native API a GPU device draws with, each unretained, as Godot's
// RenderingDevice::get_driver_resource hands them over: on Vulkan, instance, physical_device,
// device and queue are the VkInstance, VkPhysicalDevice, VkDevice and VkQueue, and queue_family
// is the queue's family; on Direct3D 12, device and queue are the ID3D12Device and
// ID3D12CommandQueue; on Metal, the id<MTLDevice> and id<MTLCommandQueue>. The software device
// has none.
struct NativeDevice {
    Backend backend = Backend::Null;
    void* instance = nullptr;
    void* physical_device = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    std::uint32_t queue_family = 0;
};

// One of a temporal upscaler's textures. On a GPU device, image is the VkImage (and view its
// VkImageView of the whole image), the ID3D12Resource or the id<MTLTexture>, and format its
// VkFormat, DXGI_FORMAT or MTLPixelFormat; pixels is empty. On the software device, pixels holds
// its values, rows top to bottom and each pixel's channels together, and the rest is empty.
struct NativeTexture {
    void* image = nullptr;
    void* view = nullptr;
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<float> pixels{}; // NOLINT(readability-redundant-member-init)
};

// What a temporal upscaler takes for a frame, as FSR 2 takes it in the device. On a GPU device the
// textures are in the state shaders sample them in (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
// D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE), the output included, and the upscaler leaves them so,
// moving the output to be written and back. It records its work into commands between the
// device's passes; on Direct3D 12 it may set descriptor heaps of its own, as the device sets its
// own again after it.
struct TemporalFrame {
    NativeDevice device;
    // The VkCommandBuffer, ID3D12GraphicsCommandList or id<MTLCommandBuffer>, recording; null on
    // the software device.
    void* commands = nullptr;
    UpscaleSize size;
    // At the render size: the 3D world drawn with the jittered camera (RGBA half floats, linear
    // and unclamped), its depth (one 32-bit float, or 24 bits where a GPU lacks them: reversed,
    // 1 at the near plane and 0 at the far one) and its motion vectors (two half floats, in
    // texture coordinates with y down: a pixel was at its own plus its motion a frame before,
    // the sky's worked out from the depth).
    NativeTexture color;
    NativeTexture depth;
    NativeTexture motion;
    // At the display size: what the upscaler writes, as a compute shader writes (RGBA half
    // floats, linear), which the post passes then tonemap.
    NativeTexture output;
    // The camera's jitter in render pixels with y down, as AMD's FSR 2 and NVIDIA's DLSS take it:
    // the projection moves by 2 * jitter[0] / render_width and -2 * jitter[1] / render_height in
    // clip space.
    std::array<float, 2> jitter{};
    // Whether the frames before belong to another view: a first frame or a change of size.
    bool reset = false;
    // Scaling::fsr_sharpness: 0 is the sharpest, 2 the softest.
    float sharpness = 0.2F;
    // The camera's projection without the jitter (reversed depth) and the world to the camera's
    // space, column-major.
    Mat4 projection{};
    Mat4 camera{};
};

// Upscales a 3D frame with its depth, motion vectors and jittered camera. One belongs to one
// device's target; the device makes it when a frame first needs it and drops it with the target.
class TemporalUpscaler {
public:
    TemporalUpscaler() = default;
    TemporalUpscaler(const TemporalUpscaler&) = delete;
    TemporalUpscaler& operator=(const TemporalUpscaler&) = delete;
    TemporalUpscaler(TemporalUpscaler&&) = delete;
    TemporalUpscaler& operator=(TemporalUpscaler&&) = delete;
    virtual ~TemporalUpscaler() = default;

    // Writes frame's output. A failure is logged, and the frame shows what the output held.
    [[nodiscard]] virtual Result<void> upscale(const TemporalFrame& frame) = 0;
};

// An upscaler a game can choose by name.
struct UpscalerInfo {
    std::string name;
    // Needs the depth, motion vectors and jittered camera of a 3D frame (DLSS, XeSS, FSR 4), and
    // so runs in place of FSR 2 where it would (uses_fsr2), made by make_temporal; make is left
    // empty. Elsewhere Scaling::mode scales the world, as for an upscaler that doesn't run.
    bool temporal = false;
    // Whether it runs on a device, such as a vendor's on its own GPUs; always, when empty. A
    // temporal one runs only on the GPU device on Vulkan, Direct3D 12 or Metal and on the
    // software device whatever this says.
    std::function<bool(const RenderDevice& device)> supports;
    std::function<std::unique_ptr<Upscaler>()> make;
    // NOLINTNEXTLINE(readability-redundant-member-init): spatial ones leave it out
    std::function<std::unique_ptr<TemporalUpscaler>()> make_temporal{};
};

// Adds an upscaler for Scaling::upscaler to name, for the whole process, as an SDK package's
// register function can. Fails for one without make (or make_temporal, for a temporal one), a
// name already taken (the built-ins' too) or one that isn't lowercase letters, digits and
// underscores.
[[nodiscard]] Result<void> add_upscaler(UpscalerInfo info);
// The upscaler of that name, built in or added, or nullopt.
[[nodiscard]] std::optional<UpscalerInfo> find_upscaler(std::string_view name);
// Every upscaler's name: the built-in ones, then those added, in order.
[[nodiscard]] std::vector<std::string> upscaler_names();
// Forgets the upscalers add_upscaler added, for tests.
void clear_added_upscalers();

// The upscaler a frame uses for these settings on this device, by name: the game's added one if
// it runs here and isn't temporal, else scaling_mode_on's (fsr or metalfx_spatial), bilinear when
// the scale is above 1 (FSR and MetalFX don't scale down), the mode is unknown or it is a temporal
// one (in a frame uses_fsr2 turns down). Empty when nothing scales: any but fsr and metalfx_spatial
// at a scale of 1.
[[nodiscard]] std::string choose_upscaler(const Scaling& scaling, const RenderDevice& device);

} // namespace slash::render
