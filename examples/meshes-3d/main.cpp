// Meshes lit by a frame's lights, as Godot's Compatibility renderer lights them: a floor, a red
// cube, a small gold cube and a pane of blue glass, seen from a camera and lit by the sun, a lamp
// and the ambient light, then drawn into a texture.
//
// Every render device draws meshes: the GPU devices (Metal, Direct3D 12, Vulkan), OpenGL 3.3,
// OpenGL ES 3.0 and WebGPU on the web. This draws on the software device, which draws the same
// pixels as they do (tests/test_mesh.cpp holds them to it) on every machine, so the output below
// is the same everywhere. A game would make its device with create_device_or_null.

#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

namespace render = slash::render;

using Float3 = std::array<float, 3>;
using Mat4 = render::Mat4;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

Float3 scaled(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}
Float3 sum(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// A square of side size around center, facing normal, with u along the texture's x and up along
// its -y. Front faces wind clockwise as the camera sees them, as Godot's do.
std::vector<render::Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                                   const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        return render::Vertex3D{
            .position = sum(center, sum(scaled(u, s * size * 0.5F), scaled(up, t * size * 0.5F))),
            .normal = normal,
            .tangent = {u[0], u[1], u[2], 1.0F},
            .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
    };
    const render::Vertex3D top_left = corner(-1.0F, 1.0F);
    const render::Vertex3D top_right = corner(1.0F, 1.0F);
    const render::Vertex3D bottom_right = corner(1.0F, -1.0F);
    const render::Vertex3D bottom_left = corner(-1.0F, -1.0F);
    return {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left};
}

// A cube of side 1 around the origin: six squares, two triangles each.
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

// A mesh's transform: scaled by scale, turned degrees about the Y axis, then moved to (x, y, z).
// Matrices are column-major, as Godot's Transform3D written out.
Mat4 placed(float scale, float degrees, float x, float y, float z) {
    const slash::Vec2 turn = slash::cos_sin(degrees * std::numbers::pi_v<float> / 180.0F);
    const float c = turn.x * scale;
    const float s = turn.y * scale;
    return {c, 0, -s, 0, 0, scale, 0, 0, s, 0, c, 0, x, y, z, 1};
}

// What a pixel shows, by its color: the sky (the clear color), the red cube or the yellow cube
// (upper case where the sun lights a face, lower case where it is turned away), the glass over
// whatever is behind it, or the floor (+ where the lamp warms it).
char what(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    const int r = image.pixels[at];
    const int g = image.pixels[at + 1];
    const int b = image.pixels[at + 2];
    if (r == 26 && g == 31 && b == 46) {
        return '.';
    }
    const bool lit = r + g + b > 300;
    if (r > 2 * g && r > 2 * b) {
        return r > 100 ? 'R' : 'r';
    }
    if (2 * g > r && 2 * b < g) {
        return lit ? 'Y' : 'y';
    }
    if (b > g + 20) {
        return 'G';
    }
    return r - b > 40 ? '+' : '#';
}

} // namespace

int main() {
    const std::unique_ptr<render::RenderDevice> device = render::create_software_device();
    const render::MeshId floor =
        device->create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 6.0F), {}).value();
    const render::MeshId box = device->create_mesh(cube(), {}).value();
    const render::MeshId pane =
        device->create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0F), {}).value();

    // A target 60 pixels wide and 20 high, which prints as twice as tall as it is wide: the
    // projection's aspect is the printed one, 1.5.
    constexpr std::uint32_t kWidth = 60;
    constexpr std::uint32_t kHeight = 20;
    const render::TextureId target =
        device->create_texture({.width = kWidth, .height = kHeight, .render_target = true}).value();

    // The camera, the sun, a warm lamp beside the red cube and a blue-gray ambient light, each
    // with Godot's parameters. Colors are sRGB, as Godot's.
    render::Scene3D scene;
    scene.camera = render::look_at({2.6F, 2.2F, 3.6F}, {0.0F, 0.15F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(34.0F, 1.5F, 0.1F, 100.0F);
    scene.ambient = {0.4F, 0.45F, 0.6F};
    scene.ambient_energy = 0.3F;
    scene.lights = {
        {.type = render::LightType::Directional,
         .color = {1.0F, 0.95F, 0.85F},
         .direction = {0.6F, -0.7F, -0.4F}},
        {.type = render::LightType::Omni,
         .color = {1.0F, 0.7F, 0.4F},
         .energy = 1.5F,
         .position = {-1.6F, 0.8F, 1.2F},
         .range = 3.0F},
    };

    // Drawing a mesh needs a frame with a scene.
    const render::Color sky{.r = 0.1F, .g = 0.12F, .b = 0.18F, .a = 1.0F};
    if (!device->begin_frame({.target = target, .clear = sky}).value()) {
        return 1;
    }
    std::printf("draw_mesh with no scene: %s\n",
                device->draw_mesh(box, placed(1, 0, 0, 0, 0), {}).error().message.c_str());
    check(device->end_frame());

    // The frame. Opaque meshes draw nearest first and the glass, blended, after them, farthest
    // first, whatever order they are drawn in; the depth buffer hides what is behind.
    if (!device->begin_frame({.target = target, .clear = sky, .scene = scene}).value()) {
        return 1;
    }
    check(device->draw_mesh(box, placed(1.0F, 25.0F, -0.6F, 0.5F, -0.2F),
                            {.albedo = {0.8F, 0.1F, 0.1F, 1.0F}, .roughness = 0.6F}));
    check(device->draw_mesh(pane, placed(1.2F, 0.0F, 0.2F, 0.6F, 1.1F),
                            {.albedo = {0.2F, 0.4F, 1.0F, 0.5F},
                             .transparency = render::Transparency::Alpha,
                             .cull = render::CullMode::Disabled}));
    check(device->draw_mesh(box, placed(0.5F, -20.0F, 1.0F, 0.25F, 0.4F),
                            {.albedo = {1.0F, 0.75F, 0.2F, 1.0F}, .roughness = 0.3F}));
    check(device->draw_mesh(floor, placed(1, 0, 0, 0, 0), {.albedo = {0.55F, 0.55F, 0.55F, 1.0F}}));
    check(device->end_frame());

    const render::Image image = device->read_texture(target).value();
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::string row;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            row += what(image, x, y);
        }
        std::printf("%s\n", row.c_str());
    }
    // Each mesh draw is one GPU draw; the two frames are both counted.
    const render::Stats stats = device->stats();
    std::printf("frames: %llu, draws: %llu, triangles: %llu\n",
                static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles));
    device->destroy_mesh(floor);
    device->destroy_mesh(box);
    device->destroy_mesh(pane);
    return 0;
}
