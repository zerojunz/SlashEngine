#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Denoising from a vendor's SDK (DLSS Ray Reconstruction, NVIDIA's NRD, AMD's FidelityFX
// denoisers) for the clustered renderer's ray-traced passes (RayTracing3D): a game adds one with
// add_denoiser, from an SDK package's glue code, as it adds a temporal upscaler (upscaler.hpp), and
// names it in project.json's rendering.ray_tracing.denoiser. Each frame that traces rays, after the
// rays and before the mesh pass reads what they wrote, the device hands it the passes' noisy
// results with the prepass's depth, normals and roughness and the frame's motion vectors, as the
// native API's objects: on the GPU device on Vulkan, Direct3D 12 and Metal (where it traces rays),
// and on the software device as floats. It denoises them in place, and the mesh pass lights the
// frame from what it wrote. With a denoiser, the ambient light comes to it as traced, without the
// engine's own smoothing across neighbors. Nothing of the vendor's is in the engine: only these
// calls.
namespace slash::render {

// What a denoiser gets for a frame, as GPU objects, by TemporalFrame's rules: the GPU device's
// command buffer, recording, outside any pass; the textures in the state shaders sample them in,
// which the denoiser leaves them in, moving the results to be written and back; on Direct3D 12 the
// device sets its own descriptor heaps again after. On the GPU the passes' textures are RGBA16F. On
// the software device, commands is null and the textures' pixels hold four floats a pixel (one for
// depth, two for motion), rows top to bottom. Every texture is at the frame's render size.
struct DenoiseFrame {
    NativeDevice device;
    void* commands = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The prepass's reversed depth (0 where nothing drew), and its normals: view space, from 0 to
    // 1 in RGB, with the surface's roughness in alpha.
    NativeTexture depth;
    NativeTexture normals;
    // The frame's motion vectors, in texture coordinates with y down (a pixel was at its own plus
    // its motion a frame before), where the frame draws them (with TAA, FSR 2 or frame
    // generation); else an empty texture, and the denoiser works them out from the depth and the
    // cameras, as vendors' SDKs can.
    NativeTexture motion;
    // The passes' results to denoise in place; an empty texture for a pass the frame doesn't
    // trace. shadows: the visibilities of the lights in ray slots 0 to 3, then 4 to 7, a
    // channel each (1 lit, 0 in shadow). occlusion: the ambient occlusion in red (1 open, 0
    // closed). reflections: the reflected light in RGB and the share of rays that met something
    // in alpha. ambient_light: the light bouncing off what is near, in RGB.
    std::array<NativeTexture, 2> shadows{};
    NativeTexture occlusion;
    NativeTexture reflections;
    NativeTexture ambient_light;
    // Whether the denoiser's history belongs to another view: its target's first frame with it, or
    // a change of size.
    bool reset = false;
    // The camera's projection (reversed depth) and the world to the camera's space,
    // column-major.
    Mat4 projection{};
    Mat4 camera{};
};

// Denoises a frame's ray-traced passes. One belongs to one device's target; the device makes it
// when the target's first frame traces rays with it.
class Denoiser {
public:
    Denoiser() = default;
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;
    Denoiser(Denoiser&&) = delete;
    Denoiser& operator=(Denoiser&&) = delete;
    virtual ~Denoiser() = default;

    // Denoises frame's passes in place. A failure is logged once, and the frame is lit by what
    // the passes hold.
    [[nodiscard]] virtual Result<void> denoise(const DenoiseFrame& frame) = 0;
};

// A denoiser a game can choose by name.
struct DenoiserInfo {
    std::string name;
    // Whether it runs on a device, such as a vendor's on its own GPUs; always, when empty. It runs
    // only where the device traces rays: the GPU device on Vulkan, Direct3D 12 or Metal with ray
    // queries, and the software device.
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<bool(const RenderDevice& device)> supports{};
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<std::unique_ptr<Denoiser>()> make{};
};

// Adds a denoiser for rendering.ray_tracing.denoiser to name, for the whole process, as an SDK
// package's register function can. Fails for one without make, a name already taken or one that
// isn't lowercase letters, digits and underscores.
[[nodiscard]] Result<void> add_denoiser(DenoiserInfo info);
// The denoiser added with that name, or nullopt.
[[nodiscard]] std::optional<DenoiserInfo> find_denoiser(std::string_view name);
// Every denoiser's name, in the order they were added.
[[nodiscard]] std::vector<std::string> denoiser_names();
// Forgets the denoisers add_denoiser added, for tests.
void clear_added_denoisers();

// The added denoiser of that name, where it runs on device (a device that traces rays, and its
// supports says yes); else nullopt, and the passes keep the engine's own smoothing.
[[nodiscard]] std::optional<DenoiserInfo> choose_denoiser(std::string_view name,
                                                          const RenderDevice& device);

} // namespace slash::render
