// A game's own denoiser for the ray-traced passes, as DLSS Ray Reconstruction, NVIDIA's NRD or
// AMD's FidelityFX denoisers come in from a vendor's SDK, made with render::add_denoiser and named
// in project.json's rendering.ray_tracing.denoiser (Scene3D::ray_tracing.denoiser here).
//
// Each frame that traces rays, the device hands it the passes' noisy results (shadows, ambient
// occlusion, reflections, ambient light) with the prepass's depth and normals, and it denoises
// them in place before the frame is lit from them. On a GPU those are the native API's objects
// (a VkImage, an ID3D12Resource, an MTLTexture) with the command buffer, which a vendor's library
// takes; on the software device, used here, they are float pixels, so the output below is the
// same on every machine.
//
// This one, "smooth", averages each pixel's shadows and occlusion with its neighbors' within two
// pixels on the same surface (a depth close to its own), as denoisers do over space: a light with
// a size casts soft shadows that one ray a pixel makes grainy, and four occlusion rays a pixel
// leave the floor around the cube speckled.

#include "slash/render/denoiser.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace render = slash::render;
using Float3 = std::array<float, 3>;

constexpr std::uint32_t kSize = 32;

template <typename T>
T value(slash::Result<T> result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
    return std::move(result.value());
}

void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// How grainy a pass's first channel is where the floor shows: the mean step between neighbors
// across a row, in thousandths.
long grain(std::span<const float> pixels, std::span<const float> depth, std::uint32_t width,
           std::uint32_t height) {
    double total = 0.0;
    std::size_t steps = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x + 1 < width; ++x) {
            const std::size_t at = (std::size_t{y} * width) + x;
            if (depth[at] == 0.0F || std::abs(depth[at] - depth[at + 1]) > 0.002F) {
                continue;
            }
            total += std::abs(pixels[at * 4] - pixels[(at + 1) * 4]);
            ++steps;
        }
    }
    return steps == 0 ? 0 : std::lround(1000.0 * total / static_cast<double>(steps));
}

// Averages each pixel of a pass with its neighbors on the same surface.
void smooth(std::span<float> pixels, std::span<const float> depth, std::uint32_t width,
            std::uint32_t height) {
    const std::vector<float> noisy(pixels.begin(), pixels.end());
    constexpr int kReach = 2;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (std::size_t{y} * width) + x;
            if (depth[at] == 0.0F) {
                continue;
            }
            std::array<float, 4> sum{};
            float count = 0.0F;
            for (int dy = -kReach; dy <= kReach; ++dy) {
                for (int dx = -kReach; dx <= kReach; ++dx) {
                    const long nx = static_cast<long>(x) + dx;
                    const long ny = static_cast<long>(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<long>(width) ||
                        ny >= static_cast<long>(height)) {
                        continue;
                    }
                    const std::size_t near =
                        (static_cast<std::size_t>(ny) * width) + static_cast<std::size_t>(nx);
                    // Only the same surface: a depth within a share of the pixel's own.
                    if (std::abs(depth[near] - depth[at]) > depth[at] * 0.05F) {
                        continue;
                    }
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum.at(c) += noisy[(near * 4) + c];
                    }
                    count += 1.0F;
                }
            }
            for (std::size_t c = 0; c < 4; ++c) {
                pixels[(at * 4) + c] = sum.at(c) / count;
            }
        }
    }
}

class Smooth final : public render::Denoiser {
public:
    slash::Result<void> denoise(const render::DenoiseFrame& frame) override {
        std::printf("  smooth: %ux%u%s\n", frame.width, frame.height, frame.reset ? ", reset" : "");
        const auto pass = [&](const char* name, const render::NativeTexture& texture) {
            if (texture.pixels.empty()) {
                return;
            }
            const long before =
                grain(texture.pixels, frame.depth.pixels, frame.width, frame.height);
            smooth(texture.pixels, frame.depth.pixels, frame.width, frame.height);
            std::printf("  %-9s grain %3ld -> %3ld\n", name, before,
                        grain(texture.pixels, frame.depth.pixels, frame.width, frame.height));
        };
        pass("shadows", frame.shadows[0]);
        pass("occlusion", frame.occlusion);
        return {};
    }
};

Float3 scaled(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}
Float3 sum(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// A square of side size around center, facing normal.
std::vector<render::Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                                   const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        return render::Vertex3D{
            .position = sum(center, sum(scaled(u, s * size * 0.5F), scaled(up, t * size * 0.5F))),
            .normal = normal,
            .tangent = {u[0], u[1], u[2], 1.0F},
            .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// A cube of side 1 around the origin.
std::vector<render::Vertex3D> cube() {
    struct Face {
        Float3 normal;
        Float3 u;
        Float3 up;
    };
    const std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    std::vector<render::Vertex3D> out;
    for (const Face& face : faces) {
        const std::vector<render::Vertex3D> side =
            quad(scaled(face.normal, 0.5F), face.u, face.up, face.normal, 1.0F);
        out.insert(out.end(), side.begin(), side.end());
    }
    return out;
}

render::Mat4 moved(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

// Draws the floor and the cube from above, lit by a light with a size from the -X side, with
// ray-traced shadows (one ray a pixel) and ambient occlusion (four), denoised by denoiser.
render::Image draw(render::RenderDevice& device, const std::string& denoiser, int frames) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.5F, 0.5F, 0.5F};
    render::Light3D light{.type = render::LightType::Directional, .shadow = true};
    light.direction = {0.70710677F, -0.70710677F, 0.0F};
    light.up = {0.70710677F, 0.70710677F, 0.0F};
    light.size = 16.0F;
    scene.lights = {light};
    scene.shadows = {.directional_size = 1024, .positional_size = 1024};
    scene.ssao.enabled = true;
    scene.ssao.radius = 1.0F;
    scene.ray_tracing.shadows = true;
    scene.ray_tracing.ambient_occlusion = true;
    scene.ray_tracing.denoiser = denoiser;

    const render::TextureId target =
        value(device.create_texture({.width = kSize, .height = kSize, .render_target = true}));
    const render::MeshId box = value(device.create_mesh(cube(), {}));
    const render::MeshId floor =
        value(device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {}));
    for (int i = 0; i < frames; ++i) {
        if (!value(device.begin_frame({.target = target, .clear = {}, .scene = scene}))) {
            std::exit(1);
        }
        check(device.draw_mesh(floor, moved(0, 0, 0), {.albedo = {0.8F, 0.8F, 0.8F, 1.0F}}));
        check(device.draw_mesh(box, moved(0, 1, 0), {.albedo = {0.9F, 0.2F, 0.2F, 1.0F}}));
        check(device.end_frame());
    }
    render::Image image = value(device.read_texture(target));
    device.destroy_mesh(box);
    device.destroy_mesh(floor);
    device.destroy_texture(target);
    return image;
}

// A row of the picture through the cube's shadow: its green channel in tenths, 0 to 9.
std::string row(const render::Image& image, std::uint32_t y) {
    std::string out;
    for (std::uint32_t x = 0; x < image.width; ++x) {
        const int green = image.pixels[((std::size_t{y} * image.width + x) * 4) + 1];
        out += static_cast<char>('0' + std::min(9, green * 10 / 256));
    }
    return out;
}

} // namespace

int main() {
    // A game adds its denoisers before it draws, for rendering.ray_tracing.denoiser to name; an
    // SDK package's register function would add the vendor's, with a supports function that
    // checks the GPU and driver.
    check(render::add_denoiser({.name = "smooth", .supports = {}, .make = [] {
                                    return std::unique_ptr<render::Denoiser>(
                                        std::make_unique<Smooth>());
                                }}));
    const auto device = render::create_software_device();

    const render::Image noisy = draw(*device, "", 1);
    std::printf("traced     %s\n", row(noisy, kSize / 2).c_str());
    std::printf("denoising two frames:\n");
    const render::Image denoised = draw(*device, "smooth", 2);
    std::printf("denoised   %s\n", row(denoised, kSize / 2).c_str());
    // A name no package added keeps the engine's own: the passes as traced.
    const render::Image missing = draw(*device, "missing", 1);
    std::printf("missing    %s\n", missing.pixels == noisy.pixels ? "as traced" : "changed");
    return 0;
}
