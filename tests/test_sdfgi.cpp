#include "../engine/render/src/sdfgi.hpp"
#include "../engine/render/src/sdfgi_gpu.hpp"
#include "devices.hpp"

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

// The largest difference between a and b's channels.
int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// How many of the channels of a and b differ by more than by.
std::size_t differing(const Image& a, const Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
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
    const render::detail::SdfgiCascade& first = state.cascades[0];
    CHECK(std::none_of(first.cells.begin(), first.cells.end(),
                       [](std::uint32_t w) { return (w & render::detail::kSdfgiSolid) != 0; }));
    CHECK(render::detail::sdfgi_sdf(first, 0) == 255);
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
        // The cells (albedo, facing, emission and the neighbor empty cells take light from)
        // and the distance field come out the same, to the bit.
        CHECK(a.cells == b.cells);
        REQUIRE(a.field.size() == b.field.size());
        for (std::size_t i = 0; i < a.field.size() / 2; ++i) {
            if (render::detail::sdfgi_sdf(a, i) != render::detail::sdfgi_sdf(b, i)) {
                FAIL_CHECK("cell " << i << "'s distance differs");
                break;
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

TEST_CASE("SDFGI's probe occlusion floods out from each probe and stops at walls") {
    // A cascade of 64 cells (a probe every 4) at the origin, with a wall one cell thick across x
    // at cell 34, facing both ways.
    constexpr std::uint32_t kSize = 64;
    render::detail::SdfgiCascade cascade;
    cascade.cells.resize(std::size_t{kSize} * kSize * kSize * 2);
    const auto index = [](std::size_t x, std::size_t y, std::size_t z) {
        return (((z * kSize) + y) * kSize) + x;
    };
    for (std::size_t z = 0; z < kSize; ++z) {
        for (std::size_t y = 0; y < kSize; ++y) {
            cascade.cells[index(34, y, z) * 2] =
                render::detail::kSdfgiSolid | ((1U | 8U) << render::detail::kSdfgiFacingShift);
        }
    }
    render::detail::make_sdfgi_occlusion(cascade, kSize, nullptr);
    REQUIRE(cascade.occlusion.size() == cascade.cells.size() / 2);
    // Probe 8 (between cells 31 and 32 on each axis) is of parity 0; probe 9 (between 35 and
    // 36 in x) of parity 1.
    const auto seen = [&](std::size_t x, std::uint32_t parity) {
        return render::detail::sdfgi_occlusion(cascade, index(x, 32, 32), parity);
    };
    // Probe 8 sees the cells on its side of the wall and the wall's face toward it, and not
    // the cell behind the wall.
    CHECK(seen(32, 0) == 1.0F);
    CHECK(seen(33, 0) == 1.0F);
    CHECK(seen(34, 0) == 1.0F);
    CHECK(seen(35, 0) == 0.0F);
    // Probe 9, on the other side, sees that cell and not those on probe 8's side.
    CHECK(seen(35, 1) == 1.0F);
    CHECK(seen(33, 1) == 0.0F);
    // The same on any number of threads.
    slash::jobs::Pool pool(4);
    render::detail::SdfgiCascade shared = cascade;
    render::detail::make_sdfgi_occlusion(shared, kSize, &pool);
    CHECK(shared.occlusion == cascade.occlusion);
    // With no wall, every probe sees every cell within its spacing.
    render::detail::SdfgiCascade open;
    open.cells.resize(cascade.cells.size());
    render::detail::make_sdfgi_occlusion(open, kSize, nullptr);
    CHECK(std::all_of(open.occlusion.begin(), open.occlusion.end(),
                      [](std::uint32_t packed) { return packed == 0xFFFFFFFFU; }));
}

TEST_CASE("SDFGI with occlusion keeps a wall's bounce from leaking behind it") {
    auto device = render::create_software_device();
    // Looking at the floor behind the wall, in its shadow, from beyond it.
    render::Scene3D leaky = lit_cube();
    leaky.camera = render::look_at({4.5F, 4.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    render::Scene3D occluded = leaky;
    occluded.sdfgi.use_occlusion = true;
    const Image without = draw_frames(*device, leaky, 6, Stand::Wall);
    const Image with = draw_frames(*device, occluded, 6, Stand::Wall);
    // Without occlusion, the probes on the sunlit side light the floor behind the wall through
    // it; with it, they don't.
    constexpr std::array<std::uint32_t, 2> kBehind{12, 15};
    CHECK(channel(without, kBehind[0], kBehind[1], 0) >= 20);
    CHECK(channel(with, kBehind[0], kBehind[1], 0) <= 4);
    CHECK(greens(with) < greens(without));
    // The floor in front of the wall's lit face still turns red.
    render::Scene3D front = lit_cube();
    front.sdfgi.use_occlusion = true;
    const Image lit = draw_frames(*device, front, 6, Stand::Wall);
    CHECK(channel(lit, kBounce[0], kBounce[1], 0) - channel(lit, kBounce[0], kBounce[1], 1) > 4);
    // The same on any number of threads.
    slash::jobs::Pool pool(4);
    auto shared = render::create_software_device(&pool);
    CHECK(draw_frames(*shared, occluded, 6, Stand::Wall).pixels == with.pixels);
}

TEST_CASE("SDFGI's GPU buffers hold each cascade's words, and its steps follow update_sdfgi") {
    namespace detail = render::detail;
    const detail::SdfgiLayout layout = detail::sdfgi_layout(2, 64, 4);
    const std::uint32_t cells = 64U * 64U * 64U;
    CHECK(layout.cell_count() == cells);
    CHECK(layout.cells == 3 * cells);
    CHECK(layout.fields == 3 * cells);
    CHECK(layout.scratch >= 8 * cells);
    // Groups run across first, then down, so no dispatch's x passes 65535.
    CHECK(detail::sdfgi_groups(64) == std::array<std::uint32_t, 2>{1, 1});
    CHECK(detail::sdfgi_groups(65) == std::array<std::uint32_t, 2>{2, 1});
    CHECK(detail::sdfgi_groups(cells) ==
          std::array<std::uint32_t, 2>{detail::kSdfgiGroupsAcross, cells / 65536});
    // A fresh state's first frame voxelizes every cascade and clears its probes.
    detail::SdfgiState state;
    const render::Scene3D scene = lit_cube();
    const detail::SdfgiFramePlan plan = detail::plan_sdfgi_frame(state, scene, {0, 0, 0}, 1);
    REQUIRE(state.cascades.size() == 2);
    const auto steps = detail::sdfgi_steps(state, plan, layout, 1, {});
    const auto count = [&](detail::SdfgiStep::Kind kind) {
        return std::count_if(steps.begin(), steps.end(),
                             [&](const detail::SdfgiStep& step) { return step.kind == kind; });
    };
    CHECK(count(detail::SdfgiStep::Kind::Voxelize) == 2);
    CHECK(count(detail::SdfgiStep::Kind::CopyCells) == 2);
    // Each cascade's cells come before any light, and every probe is stored last.
    const auto pass = [](const detail::SdfgiStep& step) {
        return step.kind == detail::SdfgiStep::Kind::Dispatch
                   ? static_cast<detail::SdfgiPass>(step.uniforms.step[0])
                   : detail::SdfgiPass::Clear;
    };
    const auto first_light = std::find_if(steps.begin(), steps.end(), [&](const auto& step) {
        return pass(step) == detail::SdfgiPass::Light;
    });
    REQUIRE(first_light != steps.end());
    CHECK(std::none_of(first_light, steps.end(), [](const detail::SdfgiStep& step) {
        return step.kind == detail::SdfgiStep::Kind::Voxelize;
    }));
    CHECK(pass(steps.back()) == detail::SdfgiPass::ProbeStore);
}

TEST_CASE("GPUs draw SDFGI as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    render::Scene3D occluded = lit_cube();
    occluded.sdfgi.use_occlusion = true;
    render::Scene3D behind = occluded;
    behind.camera = render::look_at({4.5F, 4.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    struct Case {
        render::Scene3D scene;
        std::uint32_t frames;
        Stand stand;
    };
    const std::vector<Case> cases{
        {lit_cube(), 1, Stand::Cube}, {lit_cube(), 6, Stand::Cube}, {lit_cube(), 6, Stand::Wall},
        {occluded, 6, Stand::Wall},   {behind, 6, Stand::Wall},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        CAPTURE(i);
        const Image expected =
            draw_frames(*software, cases[i].scene, cases[i].frames, cases[i].stand);
        const Image drawn = draw_frames(*gpu, cases[i].scene, cases[i].frames, cases[i].stand);
        // Floats round differently on each GPU, and shadow edges move a texel now and then.
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 50);
        CHECK(difference(drawn, expected) <= 24);
    }
    // The GPU's own light reaches the cube's shadow.
    CHECK(channel(draw_frames(*gpu, lit_cube(), 6), kShaded[0], kShaded[1], 1) > 0);
}

namespace {

// TEMPORARY (SDFGI diagnostic): the SDFGI words a device holds after frames of the wall.
std::vector<std::vector<std::uint32_t>>
sdfgi_words_after(RenderDevice& device, const render::Scene3D& scene, std::uint32_t frames) {
    auto target = device.create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {});
    REQUIRE(floor.ok());
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        REQUIRE(device.begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
        REQUIRE(
            device.draw_mesh(floor.value(), moved(0, 0, 0), {.albedo = {0.8F, 0.8F, 0.8F, 1.0F}})
                .ok());
        REQUIRE(device
                    .draw_mesh(box.value(), placed({0.5F, 3.0F, 6.0F}, {1.25F, 1.5F, 0.0F}),
                               {.albedo = {0.9F, 0.2F, 0.2F, 1.0F}})
                    .ok());
        REQUIRE(device.end_frame().ok());
    }
    std::vector<std::vector<std::uint32_t>> words = device.sdfgi_words(target.value());
    device.destroy_mesh(box.value());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return words;
}

float rgbe_max(std::uint32_t w) {
    const float unit = std::ldexp(1.0F, static_cast<int>((w >> 27U) & 0x1FU) - 24);
    return static_cast<float>(std::max({w & 0x1FFU, (w >> 9U) & 0x1FFU, (w >> 18U) & 0x1FFU})) *
           unit;
}

// What differs in one section of words: how many, the first, and sums of each side's values
// read by how.
template <typename Read>
std::string compare_section(std::string_view name, const std::vector<std::uint32_t>& gpu,
                            const std::vector<std::uint32_t>& cpu, std::size_t at,
                            std::size_t count, const Read& read) {
    std::size_t differing = 0;
    std::size_t first = count;
    double gpu_sum = 0.0;
    double cpu_sum = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t g = at + i < gpu.size() ? gpu[at + i] : 0U;
        const std::uint32_t c = at + i < cpu.size() ? cpu[at + i] : 0U;
        const double gv = read(g);
        const double cv = read(c);
        gpu_sum += gv;
        cpu_sum += cv;
        if (g != c) {
            ++differing;
            first = std::min(first, i);
            worst = std::max(worst, std::abs(gv - cv));
        }
    }
    std::string first_text = "-";
    if (first < count) {
        const std::uint32_t g = at + first < gpu.size() ? gpu[at + first] : 0U;
        const std::uint32_t c = at + first < cpu.size() ? cpu[at + first] : 0U;
        first_text =
            std::to_string(first) + " gpu " + std::to_string(g) + " cpu " + std::to_string(c);
    }
    return std::string(name) + ": differing " + std::to_string(differing) + "/" +
           std::to_string(count) + " first " + first_text + " worst " + std::to_string(worst) +
           " sums gpu " + std::to_string(gpu_sum) + " cpu " + std::to_string(cpu_sum) + "\n";
}

} // namespace

TEST_CASE("SDFGI diagnostic: the words where a GPU and the software device part") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    const std::uint32_t sh = render::detail::kSdfgiProbeCount * render::detail::kSdfgiShSize;
    const auto plain = [](std::uint32_t w) { return static_cast<double>(w); };
    const auto signed_value = [](std::uint32_t w) {
        return static_cast<double>(static_cast<std::int32_t>(w));
    };
    const auto light = [](std::uint32_t w) { return static_cast<double>(rgbe_max(w)); };
    const auto solid = [](std::uint32_t w) { return (w & 0x80000000U) != 0U ? 1.0 : 0.0; };
    const auto sdf = [](std::uint32_t w) { return static_cast<double>((w >> 16U) & 0xFFU); };
    const auto history = [](std::uint32_t w) {
        return static_cast<double>(static_cast<std::int16_t>(w & 0xFFFFU)) +
               static_cast<double>(static_cast<std::int16_t>(w >> 16U));
    };
    struct Case {
        std::string name;
        render::Scene3D scene;
        std::uint32_t frames;
    };
    render::Scene3D no_feedback = lit_cube();
    no_feedback.sdfgi.bounce_feedback = 0.0F;
    const std::vector<Case> cases{{"wall 1", lit_cube(), 1},
                                  {"wall 2", lit_cube(), 2},
                                  {"wall no feedback 2", no_feedback, 2}};
    for (const Case& c : cases) {
        const auto g = sdfgi_words_after(*gpu, c.scene, c.frames);
        const auto s = sdfgi_words_after(*software, c.scene, c.frames);
        if (g.size() != 5 || s.size() != 5) {
            MESSAGE(c.name << ": no words, gpu " << g.size() << " software " << s.size());
            continue;
        }
        const std::uint32_t cascades = c.scene.sdfgi.cascades;
        const std::size_t n = s[0].size() / 3 / cascades;
        std::string report = c.name + "\n";
        for (std::uint32_t k = 0; k < cascades; ++k) {
            const std::size_t cells = (n * 3) * k;
            const std::size_t probes = s[2].size() / cascades * k;
            const std::size_t hist = s[4].size() / cascades;
            report += "cascade " + std::to_string(k) + "\n";
            report += compare_section("  cells solid", g[0], s[0], cells, n * 2, solid);
            report += compare_section("  cells words", g[0], s[0], cells, n * 2, plain);
            report += compare_section("  occlusion", g[0], s[0], cells + (n * 2), n, plain);
            report += compare_section("  field sdf", g[1], s[1], cells, n * 2, sdf);
            report += compare_section("  field words", g[1], s[1], cells, n * 2, plain);
            report += compare_section("  light", g[1], s[1], cells + (n * 2), n, light);
            report += compare_section("  averages", g[2], s[2], probes, std::size_t{sh} * 4,
                                      signed_value);
            report += compare_section("  irradiance", g[2], s[2], probes + (std::size_t{sh} * 4),
                                      render::detail::kSdfgiProbeImageTexels, light);
            report += compare_section("  history", g[4], s[4], hist * k, hist, history);
        }
        MESSAGE(report);
    }
}
