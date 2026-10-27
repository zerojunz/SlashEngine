#include "../engine/render/src/mesh_shading.hpp"
#include "../engine/render/src/ray_shadows.hpp"
#include "devices.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::Light3D;
using render::LightType;
using render::RenderDevice;
using render::ShadowCasting;
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

// The green channel of a pixel.
int green(const Image& image, std::uint32_t x, std::uint32_t y) {
    return image.pixels.at(((std::size_t{y} * image.width + x) * 4) + 1);
}

// A light of each kind, shining from the -X side so a cube's shadow falls toward +X.
Light3D shadowed(LightType type) {
    Light3D light{.type = type, .shadow = true};
    if (type == LightType::Directional) {
        light.direction = {0.70710677F, -0.70710677F, 0.0F};
        light.up = {0.70710677F, 0.70710677F, 0.0F};
        light.shadow_normal_bias = 3.0F;
    } else {
        light.position = {-1.5F, 3.0F, 0.0F};
        light.energy = 4.0F;
        light.range = 10.0F;
        light.attenuation = 0.5F;
        light.direction = {0.6401844F, -0.7682213F, 0.0F};
        light.up = {0.7682213F, 0.6401844F, 0.0F};
        light.spot_angle = 50.0F;
    }
    return light;
}

// A camera straight above the origin looking down, -Z up the image, with ray-traced shadows.
render::Scene3D above(const Light3D& light, bool traced = true) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    scene.lights = {light};
    scene.shadows = {.directional_size = 2048, .positional_size = 2048};
    scene.ray_tracing.shadows = traced;
    return scene;
}

struct Caster {
    ShadowCasting shadows = ShadowCasting::On;
    std::uint32_t layers = 1;
};

// A gray floor through the origin, and a cube standing above it, size pixels across.
Image draw_shadow(RenderDevice& device, const render::Scene3D& scene, const Caster& caster = {},
                  std::uint32_t size = 32) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {});
    REQUIRE(floor.ok());
    const auto begun = device.begin_frame({.target = target.value(), .clear = {}, .scene = scene});
    REQUIRE_MESSAGE(begun.ok(), error_of(begun));
    const auto floor_drawn =
        device.draw_mesh(floor.value(), moved(0, 0, 0), {.albedo = {0.8F, 0.8F, 0.8F, 1.0F}});
    REQUIRE_MESSAGE(floor_drawn.ok(), error_of(floor_drawn));
    const auto box_drawn =
        device.draw_mesh(box.value(), moved(0, 1.0F, 0), {.albedo = {0.9F, 0.2F, 0.2F, 1.0F}},
                         caster.layers, caster.shadows);
    REQUIRE_MESSAGE(box_drawn.ok(), error_of(box_drawn));
    const auto ended = device.end_frame();
    REQUIRE_MESSAGE(ended.ok(), error_of(ended));
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    device.destroy_mesh(box.value());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

// Where the 32-pixel image shows the floor in the cube's shadow, the floor well away from it, and
// the cube's top.
constexpr std::array<std::uint32_t, 2> kShaded{23, 16};
constexpr std::array<std::uint32_t, 2> kLit{8, 16};
constexpr std::array<std::uint32_t, 2> kTop{16, 16};

int at(const Image& image, const std::array<std::uint32_t, 2>& where) {
    return green(image, where[0], where[1]);
}

} // namespace

TEST_CASE("lights with shadows take ray-traced shadow slots in Forward+") {
    render::Scene3D scene = above(shadowed(LightType::Omni));
    scene.lights.push_back({.type = LightType::Directional});
    scene.lights.push_back(shadowed(LightType::Directional));
    scene.lights.push_back(shadowed(LightType::Spot));
    scene.lights[3].shadow_opacity = 0.0F;
    CHECK(render::detail::ray_shadow_slots(scene) == std::vector<int>{0, -1, 1, -1});
    // A light's uniforms carry its slot plus 1: in Forward+ the scene's are the directional
    // lights', the second of which has slot 1.
    const render::detail::MeshSceneUniforms uniforms = render::detail::mesh_scene_uniforms(scene);
    CHECK(uniforms.lights[0].shadow[3] == 0.0F);
    CHECK(uniforms.lights[1].shadow[3] == 2.0F);
    // Past the slots there are, a light takes none.
    scene.lights.assign(render::kMaxRayShadowLights + 1, shadowed(LightType::Omni));
    CHECK(render::detail::ray_shadow_slots(scene)[render::kMaxRayShadowLights] == -1);
    // Without ray-traced shadows, or in Compatibility, no light takes one.
    scene.ray_tracing.shadows = false;
    CHECK(render::detail::ray_shadow_slots(scene)[0] == -1);
    scene.ray_tracing.shadows = true;
    scene.method = render::RenderMethod::Compatibility;
    CHECK(render::detail::ray_shadow_slots(scene)[0] == -1);
}

TEST_CASE("ray-traced shadow lights spread rays across their size") {
    render::Scene3D scene = above(shadowed(LightType::Directional));
    scene.lights[0].size = 0.53F; // the sun's angular diameter, in degrees
    scene.lights.push_back(shadowed(LightType::Omni));
    scene.lights[1].size = 0.25F;
    const std::vector<render::detail::RayShadowLight> lights =
        render::detail::ray_shadow_lights(scene);
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].directional);
    CHECK(lights[0].toward[0] == doctest::Approx(-0.70710677F));
    CHECK(lights[0].toward[1] == doctest::Approx(0.70710677F));
    // The tangent of half the angle.
    CHECK(lights[0].size == doctest::Approx(std::tan(0.265 * 3.14159265358979 / 180.0)));
    CHECK_FALSE(lights[1].directional);
    CHECK(lights[1].slot == 1);
    CHECK(lights[1].size == 0.25F);

    // A pixel's rays toward the omni light reach points of a disk of its radius facing the pixel,
    // and stop just short of it.
    const render::detail::RayShadowFrame frame = render::detail::ray_shadow_frame(
        scene, render::detail::mesh_camera_uniforms(scene).view_projection, 32, 32);
    const Float3 origin{1.0F, 0.0F, 0.0F};
    std::set<float> ways;
    for (std::uint32_t s = 0; s < 16; ++s) {
        const auto ray = render::detail::ray_shadow_ray(frame, lights[1], origin, 3, 4, s);
        REQUIRE(ray.has_value());
        const Float3 end = sum(origin, scaled(ray->direction, ray->distance));
        const Float3 off{end[0] + 1.5F, end[1] - 3.0F, end[2]};
        CHECK(std::sqrt((off[0] * off[0]) + (off[1] * off[1]) + (off[2] * off[2])) <=
              doctest::Approx(0.25F).epsilon(0.01));
        ways.insert(ray->direction[2]);
    }
    CHECK(ways.size() == 16);
    // A point light's rays all go to its center; a sun without a size's all one way.
    render::detail::RayShadowLight point = lights[1];
    point.size = 0.0F;
    const auto first = render::detail::ray_shadow_ray(frame, point, origin, 3, 4, 0);
    const auto second = render::detail::ray_shadow_ray(frame, point, origin, 3, 4, 7);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->direction == second->direction);
    CHECK(first->distance == doctest::Approx(std::sqrt(15.25F) * (1.0F - 1e-4F)));
}

TEST_CASE("a pixel's rays start from its place in the world, off its surface") {
    const render::Scene3D scene = above(shadowed(LightType::Directional));
    const Mat4 view_projection = render::detail::mesh_camera_uniforms(scene).view_projection;
    const render::detail::RayShadowFrame frame =
        render::detail::ray_shadow_frame(scene, view_projection, 32, 32);
    // The floor's point under pixel (23, 16)'s center, and its depth as the prepass draws it.
    const auto project = [&](const Float3& p) {
        std::array<float, 4> clip{};
        for (std::size_t row = 0; row < 4; ++row) {
            clip.at(row) = (view_projection.at(row) * p[0]) + (view_projection.at(4 + row) * p[1]) +
                           (view_projection.at(8 + row) * p[2]) + view_projection.at(12 + row);
        }
        return clip;
    };
    const double half = std::tan(30.0 * 3.14159265358979 / 180.0) * 6.0;
    const Float3 floor{static_cast<float>(((23.5 / 16.0) - 1.0) * half), 0.0F,
                       static_cast<float>(((16.5 / 16.0) - 1.0) * half)};
    const std::array<float, 4> clip = project(floor);
    // The floor faces up: toward the camera, which looks down its -Z, so its normal in the
    // camera's space is +Z.
    const auto origin =
        render::detail::ray_shadow_origin(frame, 23, 16, clip[2] / clip[3], {0.5F, 0.5F, 1.0F});
    REQUIRE(origin.has_value());
    CHECK((*origin)[0] == doctest::Approx(floor[0]).epsilon(1e-4));
    CHECK((*origin)[2] == doctest::Approx(floor[2]).epsilon(1e-4));
    // Lifted off the floor by a share of the 6 units to the camera.
    CHECK((*origin)[1] == doctest::Approx((0.003F * 6.0F) + 0.002F).epsilon(0.01));
    // A normal facing away from the camera is turned toward it.
    const auto turned =
        render::detail::ray_shadow_origin(frame, 23, 16, clip[2] / clip[3], {0.5F, 0.5F, 0.0F});
    REQUIRE(turned.has_value());
    CHECK((*turned)[1] == doctest::Approx((*origin)[1]));
    // Nothing drawn: no rays.
    CHECK_FALSE(render::detail::ray_shadow_origin(frame, 23, 16, 0.0F, {0.5F, 0.5F, 1.0F}));
}

TEST_CASE("ray-traced shadows shade the floor where the cube stands in the light's way") {
    auto device = render::create_software_device();
    CHECK(device->ray_tracing());
    for (const LightType type : {LightType::Directional, LightType::Omni, LightType::Spot}) {
        CAPTURE(static_cast<int>(type));
        const Image traced = draw_shadow(*device, above(shadowed(type)));
        const Image mapped = draw_shadow(*device, above(shadowed(type), false));
        CHECK(at(traced, kShaded) < at(traced, kLit) - 20);
        CHECK(at(traced, kShaded) == doctest::Approx(at(mapped, kShaded)).epsilon(0.05));
        CHECK(at(traced, kLit) == at(mapped, kLit));
        CHECK(at(traced, kTop) == at(mapped, kTop));
    }
}

TEST_CASE("ray-traced shadows reach past where shadow maps end") {
    // A sun's shadow maps cover a meter from the camera, six above the floor: they leave the
    // floor lit, and its rays don't.
    Light3D sun = shadowed(LightType::Directional);
    sun.shadow_max_distance = 1.0F;
    auto device = render::create_software_device();
    const Image mapped = draw_shadow(*device, above(sun, false));
    const Image traced = draw_shadow(*device, above(sun));
    CHECK(at(mapped, kShaded) == at(mapped, kLit));
    CHECK(at(traced, kShaded) < at(traced, kLit) - 20);
}

TEST_CASE("meshes that cast no shadow for a light let its rays through") {
    auto device = render::create_software_device();
    const render::Scene3D scene = above(shadowed(LightType::Directional));
    const Image off = draw_shadow(*device, scene, {.shadows = ShadowCasting::Off});
    CHECK(at(off, kShaded) == at(off, kLit));
    // The light's caster mask misses the cube's layers.
    render::Scene3D masked = scene;
    masked.lights[0].shadow_caster_mask = 2;
    const Image missed = draw_shadow(*device, masked);
    CHECK(at(missed, kShaded) == at(missed, kLit));
    // Half the shadow's opacity lets half the light through.
    render::Scene3D faint = scene;
    faint.lights[0].shadow_opacity = 0.5F;
    const Image half = draw_shadow(*device, faint);
    const Image full = draw_shadow(*device, scene);
    CHECK(at(half, kShaded) > at(full, kShaded));
    CHECK(at(half, kShaded) < at(half, kLit));
}

TEST_CASE("a light with a size casts soft ray-traced shadows") {
    auto device = render::create_software_device();
    // The floor's pixels along the shadow's row, right of the cube: two values for a hard shadow,
    // more across a soft one's edge.
    const auto values = [](const Image& image) {
        std::set<int> found;
        for (std::uint32_t x = 20; x < 32; ++x) {
            found.insert(green(image, x, 16));
        }
        return found;
    };
    render::Scene3D scene = above(shadowed(LightType::Directional));
    CHECK(values(draw_shadow(*device, scene)).size() == 2);
    scene.lights[0].size = 30.0F;
    scene.ray_tracing.shadow_samples = 32;
    const Image soft = draw_shadow(*device, scene);
    CHECK(values(soft).size() > 3);
    // Still dark under the cube.
    CHECK(at(soft, kShaded) < at(soft, kLit) - 20);
}

TEST_CASE("ray-traced shadows are the same on any number of threads") {
    slash::jobs::Pool pool(4);
    auto alone = render::create_software_device();
    auto shared = render::create_software_device(&pool);
    render::Scene3D scene = above(shadowed(LightType::Omni));
    scene.lights[0].size = 0.4F;
    scene.ray_tracing.shadow_samples = 8;
    CHECK(draw_shadow(*alone, scene, {}, 48).pixels == draw_shadow(*shared, scene, {}, 48).pixels);
}

TEST_CASE("devices that don't trace rays draw ray-traced shadows the raster way") {
    for (const auto& device : slash::test::hardware_devices()) {
        CAPTURE(static_cast<int>(device->backend()));
        CHECK_FALSE(device->ray_tracing());
        Light3D sun = shadowed(LightType::Directional);
        CHECK(draw_shadow(*device, above(sun)).pixels ==
              draw_shadow(*device, above(sun, false)).pixels);
    }
}

TEST_CASE("project.json's rendering.ray_tracing sets the ray-traced effects") {
    auto rendering =
        slash::json::parse(R"({"ray_tracing": {"shadows": true, "shadow_samples": 500}})");
    REQUIRE(rendering.ok());
    const render::RayTracing3D read = render::ray_tracing(rendering.value());
    CHECK(read.shadows);
    CHECK(read.shadow_samples == render::kMaxRayShadowSamples);
    CHECK(render::ray_tracing(slash::json::Value{}) == render::RayTracing3D{});
}
