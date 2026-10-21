#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// 3D meshes and what a frame sees them from and lights them by, as Godot's Compatibility
// renderer: a forward pass over a depth buffer, before the frame's 2D draws, that lights each mesh
// with the frame's lights and a StandardMaterial3D (Material3D, in device.hpp).
//
//   auto mesh = device->create_mesh(vertices, indices);
//   render::Scene3D scene{.camera = render::look_at({0, 2, 5}, {0, 0, 0}, {0, 1, 0}),
//                         .projection = render::perspective(75.0F, aspect, 0.05F, 4000.0F),
//                         .lights = {{.type = render::LightType::Directional}}};
//   device->begin_frame({.clear = sky, .scene = scene});
//   device->draw_mesh(mesh, transform, {.albedo = {0.8F, 0.2F, 0.2F, 1.0F}});
//   device->end_frame();
//
// Space is Godot's: Y up, X right, and a camera looks down its -Z. Front faces wind clockwise as
// seen on the target, as Godot's do. Depth is reversed: projections map the near plane to 1 and
// the far plane to 0, and nearer pixels win.
namespace slash::render {

// A mesh on the device. NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class MeshId : std::uint32_t { None = 0 };

// One corner of a mesh, in the mesh's own space: a position, a normal, a tangent and the sign of
// its binormal (cross(normal, tangent) times w, as Godot's), a texture coordinate from (0, 0) at
// the texture's top left, and a color.
struct Vertex3D {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0F, 1.0F, 0.0F};
    std::array<float, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
};

// The most corners and indices a mesh may have, and the most lights a frame's scene may have.
inline constexpr std::uint32_t kMaxMeshVertices = 1U << 24U;
inline constexpr std::uint32_t kMaxMeshIndices = 1U << 26U;
inline constexpr std::size_t kMaxLights3D = 64;

// Which faces a mesh hides, as Godot's cull_mode.
enum class CullMode : std::uint8_t {
    Back,     // faces seen from behind, Godot's default
    Front,    // faces seen from the front
    Disabled, // none; a face seen from behind is lit as if it faced the camera
};

// How a mesh's alpha shows, as Godot's transparency.
enum class Transparency : std::uint8_t {
    Disabled,     // opaque, Godot's default
    Alpha,        // blended, after the opaque meshes, without writing depth
    AlphaScissor, // opaque where alpha reaches alpha_scissor_threshold, and not drawn elsewhere
};

// Which channel of a texture a value is read from, as Godot's TextureChannel.
enum class TextureChannel : std::uint8_t { Red, Green, Blue, Alpha, Gray };

enum class LightType : std::uint8_t { Directional, Omni, Spot };

// A light in a frame's scene, with Godot's Light3D parameters and defaults.
struct Light3D {
    LightType type = LightType::Directional;
    std::array<float, 3> color{1.0F, 1.0F, 1.0F}; // sRGB
    float energy = 1.0F;
    float specular = 0.5F;
    // Where an omni or spot light is, in the world.
    std::array<float, 3> position{};
    // The way a directional or spot light shines: its node's -Z.
    std::array<float, 3> direction{0.0F, 0.0F, -1.0F};
    float range = 5.0F;       // omni and spot
    float attenuation = 1.0F; // how light falls off with distance, omni and spot
    float spot_angle = 45.0F; // degrees from direction to the cone's edge
    float spot_attenuation = 1.0F;
    // Lights the meshes whose layers share a bit with it.
    std::uint32_t cull_mask = 0xFFFFFFFFU;

    bool operator==(const Light3D&) const = default;
};

// What a frame's meshes are seen from and lit by. Matrices are column-major, as Mat4.
struct Scene3D {
    // The world to the camera's space: the inverse of the camera's transform.
    std::array<float, 16> camera = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                    0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    // The camera's space to clip space, with reversed depth: perspective or orthogonal.
    std::array<float, 16> projection = perspective_default();
    // Light every lit pixel gets, times its albedo: sRGB, times ambient_energy.
    std::array<float, 3> ambient{0.0F, 0.0F, 0.0F};
    float ambient_energy = 1.0F;
    std::vector<Light3D> lights; // up to kMaxLights3D

    // Godot's default Camera3D: 75 degrees across a square target, from 0.05 to 4000.
    static std::array<float, 16> perspective_default();
};

// Godot's Projection::set_perspective, with reversed depth: fov_y degrees across the target's
// height, aspect its width over its height, from near to far.
[[nodiscard]] std::array<float, 16> perspective(float fov_y, float aspect, float near, float far);
// Godot's Projection::set_orthogonal, with reversed depth: size across the target's height.
[[nodiscard]] std::array<float, 16> orthogonal(float size, float aspect, float near, float far);
// The world to the space of a camera at eye looking at target, with up as near its up as it can
// be: the inverse of Godot's Transform3D::looking_at.
[[nodiscard]] std::array<float, 16> look_at(const std::array<float, 3>& eye,
                                            const std::array<float, 3>& target,
                                            const std::array<float, 3>& up);

} // namespace slash::render
