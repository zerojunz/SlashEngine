#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Drawing the world smaller than the screen and scaling it up, as Godot's scaling_3d settings:
// the canvas renderer draws the world into an upscaler's input at the render size, the upscaler
// scales it to the screen's size, and the controls draw over it at full size. "bilinear" (a
// filtered stretch) and "fsr" (AMD's FidelityFX Super Resolution 1: shaders/fsr_easu.hlsl and
// fsr_rcas.hlsl) are built in, drawn with the device's own calls, so they run on every backend
// and the software device. A game adds others with add_upscaler, from an SDK package's glue code
// for one that ships as a vendor's closed library. "fsr2" (FidelityFX Super Resolution 2) is the
// 3D renderer's own instead: a Forward+ frame draws its 3D smaller and upscales it in the device
// with the depth buffer and motion vectors, and the 2D draws over it at full size, as Godot's.
namespace slash::render {

// How a frame's world is scaled, as Godot's rendering/scaling_3d settings (project.json's
// rendering.scaling).
struct Scaling {
    // "bilinear", "fsr" or "fsr2", as Godot's scaling_3d/mode.
    std::string mode = "bilinear";
    // The render size over the screen's, from 0.25 to 2: below 1 draws fewer pixels and scales
    // up, above 1 draws more and scales down (always bilinear, as FSR only scales up).
    float scale = 1.0F;
    // FSR's and FSR 2's sharpening in stops: 0 is the sharpest, 2 the softest.
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

// The built-in modes. Godot's MetalFX modes aren't built.
inline constexpr std::string_view kBilinear = "bilinear";
inline constexpr std::string_view kFsr = "fsr";
inline constexpr std::string_view kFsr2 = "fsr2";
[[nodiscard]] std::vector<std::string_view> scaling_modes();
// Why a mode isn't one, for project validation: unknown (with the closest name), or one of
// Godot's that isn't built. Empty for a built-in mode.
[[nodiscard]] std::string scaling_mode_problem(std::string_view mode);

// Whether a frame with these settings upscales its 3D with FSR 2 on this device: in Forward+
// (method), at a scale under 1, on a device that draws Forward+ (every one but OpenGL's), as
// Godot runs it. The frame's Scene3D then takes the scale and sharpness (render::Fsr2Scaling) and
// no upscaler scales the world; choose_upscaler's bilinear scaling stands in otherwise, as Godot
// falls back to it.
[[nodiscard]] bool uses_fsr2(const Scaling& scaling, RenderMethod method,
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

// An upscaler a game can choose by name.
struct UpscalerInfo {
    std::string name;
    // Needs the depth, motion vectors and jittered camera of a 3D frame (DLSS, XeSS, FSR 2 and
    // later), which add_upscaler refuses until the 3D renderer makes them.
    bool temporal = false;
    // Whether it runs on a device, such as a vendor's on its own GPUs; always, when empty.
    std::function<bool(const RenderDevice& device)> supports;
    std::function<std::unique_ptr<Upscaler>()> make;
};

// Adds an upscaler for Scaling::upscaler to name, for the whole process, as an SDK package's
// register function can. Fails for a temporal one, a name already taken (the built-ins' too)
// or one that isn't lowercase letters, digits and underscores.
[[nodiscard]] Result<void> add_upscaler(UpscalerInfo info);
// The upscaler of that name, built in or added, or nullopt.
[[nodiscard]] std::optional<UpscalerInfo> find_upscaler(std::string_view name);
// Every upscaler's name: the built-in ones, then those added, in order.
[[nodiscard]] std::vector<std::string> upscaler_names();
// Forgets the upscalers add_upscaler added, for tests.
void clear_added_upscalers();

// The upscaler a frame uses for these settings on this device, by name: the game's added one if
// it runs here, else the mode's, bilinear when the scale is above 1 (FSR doesn't scale down), the
// mode is unknown or it is fsr2 (in a frame uses_fsr2 turns down). Empty when nothing scales:
// bilinear or fsr2 at a scale of 1.
[[nodiscard]] std::string choose_upscaler(const Scaling& scaling, const RenderDevice& device);

} // namespace slash::render
