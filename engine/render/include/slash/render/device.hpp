#pragma once

#include "slash/core/result.hpp"
#include "slash/render/light.hpp"
#include "slash/render/material.hpp"
#include "slash/render/particles.hpp"
#include "slash/render/shader.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

// The renderer's device layer: the few calls every drawing pass is built on, over SDL3's GPU API
// (Vulkan, Metal or Direct3D 12), over OpenGL ES 3.0 (WebGL 2 on the web) or, for headless runs,
// a null device that draws nothing.
//
//   auto device = render::create_device({.window = window});
//   device->begin_frame({.clear = {0.3F, 0.3F, 0.3F, 1.0F}});
//   device->draw(render::TextureId::None, vertices); // every three vertices make a triangle
//   device->draw(texture, vertices, material);       // with a blend mode and material program
//   device->end_frame();                               // submits and presents
//
// A frame can carry lights (light.hpp); draws whose material's shading isn't unshaded are lit by
// them. Particle systems (particles.hpp) run and draw on the device itself.
namespace slash::render {

enum class Backend : std::uint8_t { Null, Vulkan, Metal, D3D12, Software, OpenGl };

// "null", "vulkan", "metal", "d3d12", "software" or "opengl".
[[nodiscard]] std::string_view backend_name(Backend backend);

// Straight (not premultiplied) RGBA, each from 0 to 1.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// How frames wait for the display, as in Godot.
enum class VsyncMode : std::uint8_t {
    Disabled, // present at once: lowest latency, may tear; Enabled where unsupported
    Enabled,  // wait for the display's refresh: no tearing, the default
    Adaptive, // like Enabled, but a late frame presents at once; Enabled where unsupported
    Mailbox,  // present the newest finished frame each refresh: no tearing, low latency; Enabled
              // where unsupported
};

// "disabled", "enabled", "adaptive" or "mailbox".
[[nodiscard]] std::string_view vsync_mode_name(VsyncMode mode);
// The mode with this name, or nullopt.
[[nodiscard]] std::optional<VsyncMode> parse_vsync_mode(std::string_view name);

// What a window shows behind everything: Godot's default clear color.
inline constexpr Color kDefaultClearColor{.r = 0.3F, .g = 0.3F, .b = 0.3F, .a = 1.0F};

// The widest and tallest texture every GPU takes.
inline constexpr std::uint32_t kMaxTextureSize = 16384;

// A texture. None draws as plain white, and as a frame's target it means the window.
// NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class TextureId : std::uint32_t { None = 0 };

enum class Filter : std::uint8_t {
    Linear,  // smooth when scaled, Godot's default
    Nearest, // sharp texels, for pixel art
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Filter filter = Filter::Linear;
    // Frames can draw into it and read_texture can read it back.
    bool render_target = false;
};

// The bytes a texture's texels take: textures are RGBA8, with no mipmaps.
[[nodiscard]] constexpr std::uint64_t texture_bytes(const TextureDesc& desc) noexcept {
    return std::uint64_t{desc.width} * desc.height * 4;
}

// A texture a material samples in slot 1, 2 or 3.
struct MaterialTexture {
    TextureId texture = TextureId::None; // None samples white
    bool repeat = false;                 // tile past the edges instead of stretching them

    bool operator==(const MaterialTexture&) const = default;
};

// How the frame's lights shade a draw (see light.hpp).
struct Shading {
    LightMode mode = LightMode::Unshaded;
    // Lit by lights whose item_cull_mask shares a bit with it, whose z range holds z_index and
    // whose layer range holds layer, the canvas layer it is drawn in (0 for the world).
    std::uint32_t light_mask = 1;
    std::int32_t z_index = 0;
    std::int32_t layer = 0;
    // Multiplies a Normal draw, as the CanvasModulate of its canvas.
    std::array<float, 4> ambient{1.0F, 1.0F, 1.0F, 1.0F};
    // Where the draw's positions are in the coordinates lights use, for a draw in another canvas
    // than theirs: x' = [0] x + [1] y + [2] and y' = [3] x + [4] y + [5]. None takes them as they
    // are.
    std::optional<std::array<float, 6>> to_lights;
    // RGB normals, OpenGL style (green up), laid out as the draw's texture; None faces the
    // viewer.
    TextureId normal_map = TextureId::None;
    // Specular color and shininess, laid out likewise, times specular; None has no highlights.
    TextureId specular_map = TextureId::None;
    std::array<float, 4> specular{1.0F, 1.0F, 1.0F, 1.0F}; // rgb color, a shininess from 0 to 1

    bool operator==(const Shading&) const = default;
};

// A draw's texture read as a multichannel signed distance field, as Godot draws MSDF text: the
// median of its red, green and blue is the distance to the edge, 0.5 on it, and alpha is the
// true distance (msdfgen's MTSDF). Each pixel is the vertex color with coverage as alpha.
struct DistanceField {
    // How far the field reaches across its 0 to 1, in texels; 0 draws the texture as colors.
    float pixel_range = 0.0F;
    // Widens the shape by this many texels past its edge, for outlines; at most half the range,
    // less one.
    float outline = 0.0F;

    bool operator==(const DistanceField&) const = default;
};

// The coverage a distance field gives a pixel, from 0 to 1: texel is what the texture holds
// there, and scale is pixels of the target per texel. The material shader does the same, with
// scale from how the texture coordinates change across the screen.
[[nodiscard]] float distance_field_coverage(const std::array<float, 4>& texel,
                                            const DistanceField& field, float scale);

// A column-major 4x4 matrix: element (row, column) is at [column * 4 + row].
using Mat4 = std::array<float, 16>;

// How a draw colors its pixels: a blend mode, a program the material shader runs for each pixel
// (see material.hpp) and how lights shade it. The default draws the texture times the vertex
// color, mixed and unlit.
struct Material {
    BlendMode blend = BlendMode::Mix;
    // With no code, each pixel is the texture times the vertex color.
    MaterialProgram program;
    std::array<MaterialTexture, kMaterialTextureSlots - 1> textures{};
    Shading shading;
    // Draws the texture as a distance field instead; only with no program.
    DistanceField field;
    // Moves the draw's positions before the frame's view maps them to clip space, as Godot's
    // canvas transform moves the world under a Camera2D: the view times this. Lights shade the
    // positions as given.
    std::optional<Mat4> transform;
    // A game's own shader (create_shader), drawn instead of a program: program has no code, and
    // its constants hold the shader's params and, at kShaderTimeConstant, the time.
    ShaderId shader = ShaderId::None;

    bool operator==(const Material&) const = default;
};

// A frame's lights: what lit draws are shaded by, each by those its Shading takes.
struct Lighting {
    std::vector<Light> lights; // up to kMaxLights
    // Point lights' textures, at their atlas_rect; None is white.
    TextureId atlas = TextureId::None;
    // Rows of kShadowMapWidth texels made with encode_shadow_depth, one per shadowed light, in a
    // texture created with Filter::Nearest.
    TextureId shadow_map = TextureId::None;
};

// One corner of a triangle: a position, a texture coordinate from (0, 0) at the texture's top
// left to (1, 1), and an RGBA color that multiplies the texture.
struct Vertex2D {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
};

// Maps pixels of a width x height target, origin at the top left and y down, to clip space.
[[nodiscard]] Mat4 pixel_projection(float width, float height);
// The matrix that applies b, then a. The same bits on every platform.
[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b);

struct Frame {
    TextureId target = TextureId::None; // None draws to the window
    Color clear;
    // Maps vertex positions to clip space. Left out, positions are pixels of the target.
    std::optional<Mat4> view;
    Lighting lighting{}; // NOLINT(readability-redundant-member-init): may be left out
};

// RGBA pixels, 4 bytes each, in rows from the top.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct Stats {
    std::uint64_t frames = 0;        // frames drawn, not counting skipped ones
    std::uint64_t draws = 0;         // draw calls sent to the GPU, after batching
    std::uint64_t triangles = 0;     // triangles drawn
    std::uint32_t textures = 0;      // textures alive
    std::uint64_t texture_bytes = 0; // what they take on the GPU (texture_bytes)
};

class RenderDevice {
public:
    RenderDevice() = default;
    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;
    RenderDevice(RenderDevice&&) = delete;
    RenderDevice& operator=(RenderDevice&&) = delete;
    virtual ~RenderDevice() = default;

    [[nodiscard]] virtual Backend backend() const = 0;

    // A texture of desc's size, filled with rgba (width * height * 4 bytes) or transparent black.
    [[nodiscard]] virtual Result<TextureId>
    create_texture(const TextureDesc& desc, std::span<const std::uint8_t> rgba = {}) = 0;
    // Replaces every pixel. Draws are sent at end_frame, so an update during a frame shows in
    // all of that frame's draws, as in Godot.
    [[nodiscard]] virtual Result<void> update_texture(TextureId texture,
                                                      std::span<const std::uint8_t> rgba) = 0;
    virtual void destroy_texture(TextureId texture) = 0;

    // A game's shader for materials to draw with. Fails when the shader has no code for this
    // backend, or the backend refuses it. The null device takes any.
    [[nodiscard]] virtual Result<ShaderId> create_shader(const CompiledShader& shader) = 0;
    // A shader in use by the frame being built goes once the frame is sent.
    virtual void destroy_shader(ShaderId shader) = 0;

    // A particle system of amount particles, from 1 to kMaxParticles, none of them live.
    [[nodiscard]] virtual Result<ParticlesId> create_particles(std::uint32_t amount) = 0;
    // A system in use by the frame being built goes once the frame is sent.
    virtual void destroy_particles(ParticlesId particles) = 0;
    // Runs steps on a system with a process material, in order after the steps before. The GPU
    // device runs them on the GPU: during a frame, when the frame is sent, ahead of its draws;
    // outside one, at once. The null device keeps no particles and runs nothing.
    [[nodiscard]] virtual Result<void> process_particles(ParticlesId particles,
                                                         const ParticleProcess& process,
                                                         std::span<const ParticleStep> steps) = 0;
    // Draws a system's particles as they are after the steps run so far, as quads (ParticleDraw)
    // with a texture and a material, as draw does: two triangles a particle.
    [[nodiscard]] virtual Result<void> draw_particles(ParticlesId particles, TextureId texture,
                                                      const ParticleDraw& look,
                                                      const Material& material) = 0;
    // Waits for the GPU, then returns a system's particles; not during a frame. The null
    // device's are none live.
    [[nodiscard]] virtual Result<std::vector<Particle>> read_particles(ParticlesId particles) = 0;

    // Starts a frame and clears its target. Returns false when there is nothing to draw to,
    // such as a minimized window: draws are still checked but dropped, and end_frame ends it.
    [[nodiscard]] virtual Result<bool> begin_frame(const Frame& frame) = 0;
    // Draws triangles, every three vertices one, with a texture or None for plain color, and a
    // material. Draws in a row with the same texture and an equal material are batched into one
    // GPU draw.
    [[nodiscard]] virtual Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices,
                                            const Material& material) = 0;
    // Draws with the default material: the texture times the vertex color.
    [[nodiscard]] Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices) {
        return draw(texture, vertices, Material{});
    }
    // Sends the frame to the GPU, and to the window when it is the target.
    [[nodiscard]] virtual Result<void> end_frame() = 0;

    // Waits for the GPU, then returns a render target's pixels.
    [[nodiscard]] virtual Result<Image> read_texture(TextureId texture) = 0;

    // Changes how frames presented to the window wait for the display, from the next frame.
    // Returns the mode in effect, which falls back to Enabled where the requested one isn't
    // supported. Without a window there is nothing to present, so the mode is only recorded.
    virtual VsyncMode set_vsync(VsyncMode mode) = 0;
    [[nodiscard]] virtual VsyncMode vsync() const = 0;

    [[nodiscard]] virtual Stats stats() const = 0;
};

struct DeviceOptions {
    // The window to draw to. Without one, only render-target textures can be drawn to.
    SDL_Window* window = nullptr;
    // "auto" picks the GPU backend SDL finds best, or OpenGL ES on the web; "null", "vulkan",
    // "metal", "d3d12" or "opengl" asks for one. "opengl" is OpenGL ES 3.0, as Godot's
    // compatibility renderer: WebGL 2 on the web, and wherever else the system has OpenGL ES
    // (Android, Linux), drawing to a window made with SDL_WINDOW_OPENGL.
    std::string driver = "auto";
    // Turns on the GPU API's validation, which is slow.
    bool debug = false;
};

// Creates a device. Fails if the driver is unknown, or no GPU backend works for a GPU driver.
[[nodiscard]] Result<std::unique_ptr<RenderDevice>> create_device(const DeviceOptions& options);

// A frame of only the clear color, drawn to the window: what a window shows until there is
// something to draw. Does nothing on the null and software devices, which have no window.
[[nodiscard]] Result<void> draw_empty_frame(RenderDevice& device, const Color& clear);

// create_device, or the null device with a logged warning if that fails, so a game still runs
// where no GPU backend works, such as a server.
[[nodiscard]] std::unique_ptr<RenderDevice> create_device_or_null(const DeviceOptions& options);

// The null device: it keeps textures and counts frames and triangles but draws nothing. Reading a
// render target returns the color its last frame cleared it to.
[[nodiscard]] std::unique_ptr<RenderDevice> create_null_device();

// The software device: it draws on the CPU, into render-target textures only, as the GPU device's
// shaders would, with their CPU twins (run_program, shade and distance_field_coverage), and
// covers the same pixels a GPU does. Much slower than a GPU, but it needs none and draws the same
// on every machine, so frame captures use it.
[[nodiscard]] std::unique_ptr<RenderDevice> create_software_device();

} // namespace slash::render
