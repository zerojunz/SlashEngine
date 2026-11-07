#include "../engine/render/src/sdfgi.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::Light3D;
using render::LightType;
using render::RenderDevice;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Float3 = std::array<float, 3>;

namespace {

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

Float3 scaled(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}
Float3 sum(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// A square of side size around center, facing normal, its corners clockwise from the front.
std::vector<Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                           const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        return Vertex3D{
            .position = sum(center, sum(scaled(u, s * size * 0.5F), scaled(up, t * size * 0.5F))),
            .normal = normal,
            .tangent = {u[0], u[1], u[2], 1.0F},
            .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
    };
    const Vertex3D top_left = corner(-1.0F, 1.0F);
    const Vertex3D top_right = corner(1.0F, 1.0F);
    const Vertex3D bottom_right = corner(1.0F, -1.0F);
    const Vertex3D bottom_left = corner(-1.0F, -1.0F);
    return {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left};
}

// A cube of side 1 around the origin.
std::vector<Vertex3D> cube() {
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
    std::vector<Vertex3D> out;
    for (const Face& face : faces) {
        const std::vector<Vertex3D> side =
            quad(scaled(face.normal, 0.5F), face.u, face.up, face.normal, 1.0F);
        out.insert(out.end(), side.begin(), side.end());
    }
    return out;
}

Mat4 moved(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

// Scaled by size, then moved to at.
Mat4 placed(const Float3& size, const Float3& at) {
    return {size[0], 0, 0, 0, 0, size[1], 0, 0, 0, 0, size[2], 0, at[0], at[1], at[2], 1};
}

int channel(const Image& image, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return image.pixels.at(((std::size_t{y} * image.width + x) * 4) + c);
}

// A camera above a gray floor and a red cube standing on it, lit by the sun from -X with no
// ambient light, with SDFGI where on: small cascades that settle in a few frames.
render::Scene3D lit_cube(bool on = true) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    Light3D sun{.type = LightType::Directional, .shadow = true};
    sun.direction = {0.70710677F, -0.70710677F, 0.0F};
    sun.up = {0.70710677F, 0.70710677F, 0.0F};
    sun.shadow_normal_bias = 3.0F;
    scene.lights = {sun};
    scene.shadows = {.directional_size = 2048, .positional_size = 2048};
    scene.sdfgi.enabled = on;
    scene.sdfgi.cascades = 2;
    scene.sdfgi.cascade_size = 64;
    scene.sdfgi.min_cell_size = 0.25F;
    scene.sdfgi.y_scale = 1.0F;
    scene.sdfgi.frames_to_converge = 4;
    scene.sdfgi.frames_to_update_light = 1;
    scene.sdfgi.read_sky_light = false;
    return scene;
}

// What stands on the floor: a red cube in the middle, or a tall red wall to its right whose
// left face the sun lights.
enum class Stand : std::uint8_t { Cube, Wall };

// Draws the floor and what stands on it frames times into one target, size pixels across, and
// returns the last frame.
Image draw_frames(RenderDevice& device, const render::Scene3D& scene, std::uint32_t frames,
                  Stand stand = Stand::Cube, std::uint32_t size = 32) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {});
    REQUIRE(floor.ok());
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto begun =
            device.begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_MESSAGE(begun.ok(), error_of(begun));
        REQUIRE(
            device.draw_mesh(floor.value(), moved(0, 0, 0), {.albedo = {0.8F, 0.8F, 0.8F, 1.0F}})
                .ok());
        const Mat4 at = stand == Stand::Cube ? moved(0, 0.5F, 0)
                                             : placed({0.5F, 3.0F, 6.0F}, {1.25F, 1.5F, 0.0F});
        REQUIRE(device.draw_mesh(box.value(), at, {.albedo = {0.9F, 0.2F, 0.2F, 1.0F}}).ok());
        const auto ended = device.end_frame();
        REQUIRE_MESSAGE(ended.ok(), error_of(ended));
    }
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    device.destroy_mesh(box.value());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

// Textures that are white everywhere.
class White final : public render::detail::PathTextures {
public:
    [[nodiscard]] render::detail::Float4 sample(std::size_t /*draw*/, std::size_t /*slot*/,
                                                std::array<float, 2> /*uv*/) const override {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    [[nodiscard]] render::detail::Float4 sky(bool /*radiance*/,
                                             std::array<float, 2> /*coords*/) const override {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
};

// Where the 32-pixel image shows the floor in front of the wall's lit face, and the floor in the
// cube's shadow.
constexpr std::array<std::uint32_t, 2> kBounce{18, 16};
constexpr std::array<std::uint32_t, 2> kShaded{22, 16};

// The image's green, summed.
long greens(const Image& image) {
    long sum = 0;
    for (std::size_t i = 1; i < image.pixels.size(); i += 4) {
        sum += image.pixels[i];
    }
    return sum;
}

} // namespace

TEST_CASE("SDFGI's light round-trips through Godot's RGB9E5") {
    for (const Float3 color : {Float3{0.0F, 0.0F, 0.0F}, Float3{1.0F, 0.5F, 0.25F},
                               Float3{12.0F, 0.01F, 3.0F}, Float3{0.002F, 0.003F, 0.001F}}) {
        CAPTURE(color[0]);
        const Float3 back =
            render::detail::sdfgi_rgbe_decode(render::detail::sdfgi_rgbe_encode(color));
        const float largest = std::max({color[0], color[1], color[2]});
        for (std::size_t c = 0; c < 3; ++c) {
            // 9 bits under the largest channel's exponent.
            CHECK(std::abs(back.at(c) - color.at(c)) <= (largest / 256.0F) + 1e-6F);
        }
    }
}

TEST_CASE("SDFGI's cascades follow the camera, each twice as coarse as the one before") {
    render::detail::SdfgiState state;
    render::Scene3D scene = lit_cube();
    scene.lights.clear();
    const render::detail::PathScene empty{};
    const White textures;
    render::detail::update_sdfgi(state, scene, {0.0F, 0.0F, 0.0F}, empty, textures, nullptr);
    REQUIRE(state.cascades.size() == 2);
    CHECK(state.cascades[0].cell_size == doctest::Approx(0.25F));
    CHECK(state.cascades[1].cell_size == doctest::Approx(0.5F));
    // With nothing in them, no cell is solid and every distance is as far as it goes.
    CHECK(state.cascades[0].cells.empty());
    CHECK(state.cascades[0].sdf.at(0) == 255);
    // A step of less than the drag margin leaves a cascade where it is; past it, the cascade
    // moves by whole probes (4 cells each here).
    const std::array<std::int32_t, 3> start = state.cascades[0].position;
    render::detail::update_sdfgi(state, scene, {0.1F, 0.0F, 0.0F}, empty, textures, nullptr);
    CHECK(state.cascades[0].position == start);
    render::detail::update_sdfgi(state, scene, {3.0F, 0.0F, 0.0F}, empty, textures, nullptr);
    CHECK(state.cascades[0].position[0] > start[0]);
    CHECK(state.cascades[0].position[0] % 4 == 0);
    CHECK(state.cascades[0].position[1] == start[1]);
}

TEST_CASE("SDFGI bounces the sun off the floor and a red wall, the same on any number of threads") {
    auto device = render::create_software_device();
    const Image plain = draw_frames(*device, lit_cube(false), 1);
    const Image gi = draw_frames(*device, lit_cube(), 6);
    // Without it, the cube's shadow is black; with it, the sunlit floor's light reaches it.
    CHECK(channel(plain, kShaded[0], kShaded[1], 1) == 0);
    CHECK(channel(gi, kShaded[0], kShaded[1], 1) > 0);
    // The floor in front of the wall's sunlit red face turns red.
    const Image flat = draw_frames(*device, lit_cube(false), 1, Stand::Wall);
    const Image wall = draw_frames(*device, lit_cube(), 6, Stand::Wall);
    const auto redness = [](const Image& image) {
        return channel(image, kBounce[0], kBounce[1], 0) -
               channel(image, kBounce[0], kBounce[1], 1);
    };
    CHECK(redness(flat) == 0);
    CHECK(redness(wall) > 4);
    // The same on any number of threads.
    slash::jobs::Pool pool(4);
    auto shared = render::create_software_device(&pool);
    CHECK(draw_frames(*shared, lit_cube(), 6).pixels == gi.pixels);
}

TEST_CASE("SDFGI's probes settle over frames_to_converge frames") {
    auto device = render::create_software_device();
    const long plain = greens(draw_frames(*device, lit_cube(false), 1));
    const long first = greens(draw_frames(*device, lit_cube(), 1)) - plain;
    const long second = greens(draw_frames(*device, lit_cube(), 2)) - plain;
    const long settled = greens(draw_frames(*device, lit_cube(), 4)) - plain;
    CHECK(first > 0);
    CHECK(first < second);
    CHECK(second < settled);
    // Past frames_to_converge, it holds.
    CHECK(std::abs(greens(draw_frames(*device, lit_cube(), 8)) - plain - settled) < settled / 20);
}

TEST_CASE("SDFGI's cascades scroll: a moved cascade keeps its cells and makes those that came in") {
    // A floor of four cubes along x, lit by nothing, so only cells are compared.
    render::Scene3D scene = lit_cube();
    scene.lights.clear();
    const std::vector<Vertex3D> box = cube();
    std::vector<render::detail::MeshDraw> draws;
    for (int i = 0; i < 4; ++i) {
        render::detail::MeshDraw draw;
        draw.transform =
            placed({1.5F, 0.5F, 1.5F}, {(static_cast<float>(i) * 3.0F) - 4.0F, 0.0F, 0.0F});
        draw.material.albedo = {0.2F * static_cast<float>(i + 1), 0.5F, 0.5F, 1.0F};
        draws.push_back(draw);
    }
    std::vector<std::uint32_t> triangles(box.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        triangles[i] = static_cast<std::uint32_t>(i);
    }
    std::vector<render::detail::PathMesh> meshes;
    for (const render::detail::MeshDraw& draw : draws) {
        meshes.push_back({.draw = &draw, .vertices = box, .triangles = triangles});
    }
    const render::detail::PathScene paths =
        render::detail::build_path_scene(scene, meshes, 16, 16, {});
    const White textures;
    // One state follows the camera along x; the other is made where it ends up.
    render::detail::SdfgiState moving;
    render::detail::update_sdfgi(moving, scene, {0.0F, 0.0F, 0.0F}, paths, textures, nullptr);
    const std::array<std::int32_t, 3> start = moving.cascades[0].position;
    render::detail::update_sdfgi(moving, scene, {2.0F, 0.0F, 0.0F}, paths, textures, nullptr);
    REQUIRE(moving.cascades[0].position != start);
    render::detail::SdfgiState fresh;
    render::detail::update_sdfgi(fresh, scene, {2.0F, 0.0F, 0.0F}, paths, textures, nullptr);
    REQUIRE(fresh.cascades[0].position == moving.cascades[0].position);
    for (std::size_t c = 0; c < fresh.cascades.size(); ++c) {
        CAPTURE(c);
        const render::detail::SdfgiCascade& a = moving.cascades[c];
        const render::detail::SdfgiCascade& b = fresh.cascades[c];
        CHECK(a.sdf == b.sdf);
        REQUIRE(a.cells.size() == b.cells.size());
        for (std::size_t i = 0; i < a.cells.size(); ++i) {
            CHECK(a.cells[i].index == b.cells[i].index);
            CHECK(a.cells[i].facing == b.cells[i].facing);
            CHECK(a.cells[i].neighbors == b.cells[i].neighbors);
            for (std::size_t k = 0; k < 3; ++k) {
                CHECK(a.cells[i].albedo.at(k) == doctest::Approx(b.cells[i].albedo.at(k)));
            }
        }
    }
}

TEST_CASE("SDFGI's settings read project.json's as Godot's") {
    const render::SdfgiSettings defaults;
    CHECK(defaults.probe_ray_count == render::Sdfgi3D{}.ray_count);
    CHECK(defaults.frames_to_converge == render::Sdfgi3D{}.frames_to_converge);
    CHECK(defaults.frames_to_update_lights == render::Sdfgi3D{}.frames_to_update_light);
    const auto read = [](std::string_view text) {
        auto parsed = slash::json::parse(text);
        REQUIRE(parsed.ok());
        return render::sdfgi_settings_from_json(parsed.value());
    };
    const render::SdfgiSettings set = read(R"({"global_illumination": {"sdfgi": {
        "probe_ray_count": 96, "frames_to_converge": 10, "frames_to_update_lights": 16}}})");
    CHECK(set.probe_ray_count == 96);
    CHECK(set.frames_to_converge == 10);
    CHECK(set.frames_to_update_lights == 16);
    // Values Godot doesn't offer, or of the wrong kind, keep their defaults.
    CHECK(read(R"({"global_illumination": {"sdfgi": {"probe_ray_count": 12,
        "frames_to_converge": "fast", "frames_to_update_lights": 3}}})") == defaults);
    CHECK(read(R"({"global_illumination": {"voxel_gi": {"quality": "high"}}})") == defaults);
    CHECK(read("{}") == defaults);
}
