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
#include <cstdlib>
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

// How many of two images' pixels differ by more than tolerance in a channel.
int differing(const Image& a, const Image& b, int tolerance) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    int count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            if (std::abs(int{a.pixels[i + c]} - int{b.pixels[i + c]}) > tolerance) {
                ++count;
                break;
            }
        }
    }
    return count;
}

TEST_CASE("devices trace shadows' rays as the software device does, or draw them the raster way") {
    const auto software = render::create_software_device();
    for (const auto& device : slash::test::hardware_devices()) {
        CAPTURE(static_cast<int>(device->backend()));
        CAPTURE(device->ray_tracing());
        if (!device->ray_tracing()) {
            Light3D sun = shadowed(LightType::Directional);
            CHECK(draw_shadow(*device, above(sun)).pixels ==
                  draw_shadow(*device, above(sun, false)).pixels);
            continue;
        }
        std::vector<render::Scene3D> scenes;
        for (const LightType type : {LightType::Directional, LightType::Omni, LightType::Spot}) {
            scenes.push_back(above(shadowed(type)));
        }
        render::Scene3D soft = above(shadowed(LightType::Omni));
        soft.lights[0].size = 0.4F;
        soft.ray_tracing.shadow_samples = 8;
        scenes.push_back(soft);
        render::Scene3D masked = above(shadowed(LightType::Directional));
        masked.lights[0].shadow_caster_mask = 2;
        scenes.push_back(masked);
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            CAPTURE(i);
            const Image traced = draw_shadow(*device, scenes[i], {}, 48);
            const Image twin = draw_shadow(*software, scenes[i], {}, 48);
            // The GPU's float math and the CPU's part at a few pixels along shadows' edges.
            CHECK(differing(traced, twin, 3) <= 12);
            CHECK(differing(traced, twin, 40) <= 4);
        }
        const Image off = draw_shadow(*device, scenes[0], {.shadows = ShadowCasting::Off});
        CHECK(at(off, kShaded) == at(off, kLit));
        // Past where the sun's shadow maps end, its rays still shade the floor.
        Light3D sun = shadowed(LightType::Directional);
        sun.shadow_max_distance = 1.0F;
        const Image far = draw_shadow(*device, above(sun));
        CHECK(at(far, kShaded) < at(far, kLit) - 20);
    }
}

TEST_CASE("project.json's rendering.ray_tracing sets the ray-traced effects") {
    auto rendering =
        slash::json::parse(R"({"ray_tracing": {"shadows": true, "shadow_samples": 500}})");
    REQUIRE(rendering.ok());
    const render::RayTracing3D read = render::ray_tracing(rendering.value());
    CHECK(read.shadows);
    CHECK(read.shadow_samples == render::kMaxRayShadowSamples);
    CHECK_FALSE(read.ambient_occlusion);
    CHECK(read.ambient_occlusion_samples == 4);
    CHECK(render::ray_tracing(slash::json::Value{}) == render::RayTracing3D{});
    auto occluded = slash::json::parse(
        R"({"ray_tracing": {"ambient_occlusion": true, "ambient_occlusion_samples": 0}})");
    REQUIRE(occluded.ok());
    const render::RayTracing3D occlusion = render::ray_tracing(occluded.value());
    CHECK(occlusion.ambient_occlusion);
    CHECK(occlusion.ambient_occlusion_samples == 1);
}

namespace {

// The floor beside the cube, within its SSAO radius of it.
constexpr std::array<std::uint32_t, 2> kBeside{19, 16};

// The camera above the floor and cube, lit only by ambient light, with SSAO traced by rays.
render::Scene3D occluded(bool traced = true) {
    render::Scene3D scene = above(shadowed(LightType::Directional), false);
    scene.lights.clear();
    scene.ambient = {1.0F, 1.0F, 1.0F};
    scene.ssao.enabled = true;
    scene.ssao.radius = 1.0F;
    scene.ssao.power = 1.0F;
    scene.ssao.intensity = 1.0F;
    scene.ray_tracing.ambient_occlusion = traced;
    scene.ray_tracing.ambient_occlusion_samples = 16;
    return scene;
}

} // namespace

TEST_CASE("occlusion rays spread over the half of the sphere a surface faces") {
    const Float3 normal{0.0F, 0.6F, 0.8F};
    std::set<float> ways;
    float facing = 0.0F;
    constexpr std::uint32_t kSamples = 256;
    for (std::uint32_t s = 0; s < kSamples; ++s) {
        const Float3 way = render::detail::occlusion_ray(normal, 5, 7, 32, s);
        const float size = std::sqrt((way[0] * way[0]) + (way[1] * way[1]) + (way[2] * way[2]));
        CHECK(size == doctest::Approx(1.0F).epsilon(1e-4));
        const float cosine = (way[0] * normal[0]) + (way[1] * normal[1]) + (way[2] * normal[2]);
        CHECK(cosine >= 0.0F);
        facing += cosine;
        ways.insert(way[0]);
    }
    CHECK(ways.size() == kSamples);
    // Cosine weighted: the mean cosine is 2/3.
    CHECK(facing / kSamples == doctest::Approx(2.0F / 3.0F).epsilon(0.02));
}

TEST_CASE("ray-traced ambient occlusion darkens the floor beside the cube") {
    auto device = render::create_software_device();
    render::Scene3D scene = occluded();
    // Only with SSAO on, in Forward+.
    CHECK(render::detail::ray_occlusion(scene).has_value());
    scene.ssao.enabled = false;
    CHECK_FALSE(render::detail::ray_occlusion(scene).has_value());
    const Image traced = draw_shadow(*device, occluded());
    const Image plain = draw_shadow(*device, scene);
    CHECK(at(traced, kBeside) < at(plain, kBeside) - 10);
    // Open sky over the floor far away and the cube's top: nothing to darken.
    CHECK(at(traced, kLit) == at(plain, kLit));
    CHECK(at(traced, kTop) == at(plain, kTop));
    // More power, darker.
    render::Scene3D strong = occluded();
    strong.ssao.power = 3.0F;
    CHECK(at(draw_shadow(*device, strong), kBeside) < at(traced, kBeside));
    // Meshes that cast no shadow occlude nothing.
    const Image off = draw_shadow(*device, occluded(), {.shadows = ShadowCasting::Off});
    CHECK(at(off, kBeside) == at(plain, kBeside));
    // The same on any number of threads.
    slash::jobs::Pool pool(4);
    auto shared = render::create_software_device(&pool);
    CHECK(draw_shadow(*shared, occluded()).pixels == traced.pixels);
}

TEST_CASE("devices trace ambient occlusion as the software device does, or draw SSAO") {
    const auto software = render::create_software_device();
    for (const auto& device : slash::test::hardware_devices()) {
        CAPTURE(static_cast<int>(device->backend()));
        if (!device->ray_tracing()) {
            // SSAO's own passes.
            CHECK(draw_shadow(*device, occluded()).pixels ==
                  draw_shadow(*device, occluded(false)).pixels);
            continue;
        }
        render::Scene3D both = occluded();
        both.lights = {shadowed(LightType::Omni)};
        both.ray_tracing.shadows = true;
        for (const render::Scene3D& scene : {occluded(), both}) {
            const Image traced = draw_shadow(*device, scene, {}, 48);
            const Image twin = draw_shadow(*software, scene, {}, 48);
            CHECK(differing(traced, twin, 3) <= 12);
            CHECK(differing(traced, twin, 40) <= 4);
        }
    }
}

TEST_CASE("project.json's rendering.ray_tracing turns ray-traced reflections on") {
    auto rendering =
        slash::json::parse(R"({"ray_tracing": {"reflections": true, "reflection_samples": 100}})");
    REQUIRE(rendering.ok());
    const render::RayTracing3D read = render::ray_tracing(rendering.value());
    CHECK(read.reflections);
    CHECK(read.reflection_samples == render::kMaxRayShadowSamples);
    CHECK_FALSE(render::RayTracing3D{}.reflections);
    CHECK(render::RayTracing3D{}.reflection_samples == 1);
}

namespace {

// A camera looking down -Z at a mirror wall 2 meters off, with SSR, and a green cube 2 meters
// across behind the camera, off the screen, lit by a sun from behind the camera (its face toward
// the mirror only by ambient light); SSR traced by rays where traced.
render::Scene3D mirrored(bool traced = true) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 1.0F, 3.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    Light3D sun{.type = LightType::Directional, .shadow = true};
    sun.direction = {0.0F, -0.70710677F, -0.70710677F};
    sun.up = {0.0F, 0.70710677F, -0.70710677F};
    scene.lights = {sun};
    scene.shadows = {.directional_size = 2048, .positional_size = 2048};
    scene.ssr.enabled = true;
    scene.ray_tracing.reflections = traced;
    return scene;
}

// The mirror wall (white, metallic, as rough as roughness) and the cube behind the camera, size
// pixels across.
Image draw_mirror(RenderDevice& device, const render::Scene3D& scene, float roughness = 0.0F,
                  ShadowCasting cube_shadows = ShadowCasting::On, std::uint32_t size = 32) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    auto wall = device.create_mesh(quad({0, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 8.0F), {});
    REQUIRE(wall.ok());
    for (std::uint32_t frame = 0; frame < 2; ++frame) {
        // SSR reflects the frame before, so it is drawn twice for the two to compare.
        const auto begun =
            device.begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_MESSAGE(begun.ok(), error_of(begun));
        render::Material3D mirror{.albedo = {0.9F, 0.9F, 0.9F, 1.0F}};
        mirror.metallic = 1.0F;
        mirror.roughness = roughness;
        const auto wall_drawn = device.draw_mesh(wall.value(), moved(0, 0, 0), mirror);
        REQUIRE_MESSAGE(wall_drawn.ok(), error_of(wall_drawn));
        const auto box_drawn =
            device.draw_mesh(box.value(), {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 1.0F, 5.0F, 1},
                             {.albedo = {0.2F, 0.9F, 0.2F, 1.0F}}, 1, cube_shadows);
        REQUIRE_MESSAGE(box_drawn.ok(), error_of(box_drawn));
        const auto ended = device.end_frame();
        REQUIRE_MESSAGE(ended.ok(), error_of(ended));
    }
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    device.destroy_mesh(box.value());
    device.destroy_mesh(wall.value());
    device.destroy_texture(target.value());
    return image.value();
}

int channel(const Image& image, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return image.pixels.at(((std::size_t{y} * image.width + x) * 4) + c);
}

} // namespace

TEST_CASE("ray-traced reflections show what is off the screen") {
    auto device = render::create_software_device();
    render::Scene3D scene = mirrored();
    // Only with SSR on, in Forward+.
    CHECK(render::detail::ray_reflection(scene).has_value());
    scene.ssr.enabled = false;
    CHECK_FALSE(render::detail::ray_reflection(scene).has_value());
    scene.ssr.enabled = true;
    scene.method = render::RenderMethod::Compatibility;
    CHECK_FALSE(render::detail::ray_reflection(scene).has_value());

    // The cube behind the camera fills the middle of the mirror, green; SSR can't see it.
    const Image traced = draw_mirror(*device, mirrored());
    const Image screen = draw_mirror(*device, mirrored(false));
    CHECK(channel(traced, 16, 16, 1) > channel(traced, 16, 16, 0) + 40);
    CHECK(channel(screen, 16, 16, 1) <= channel(screen, 16, 16, 0) + 2);
    // Its edge: the mirror's corner reflects the sky in both.
    CHECK(channel(traced, 1, 1, 1) == channel(screen, 1, 1, 1));
    // From SSR's roughness cutoff up, nothing is traced.
    CHECK(draw_mirror(*device, mirrored(), 0.8F).pixels ==
          draw_mirror(*device, mirrored(false), 0.8F).pixels);
    // A rough mirror blurs the cube: with more rays, less noise along its edge.
    render::Scene3D many = mirrored();
    many.ray_tracing.reflection_samples = 16;
    const Image rough = draw_mirror(*device, many, 0.3F);
    CHECK(channel(rough, 16, 16, 1) > channel(rough, 16, 16, 0) + 10);
    CHECK(channel(rough, 4, 16, 1) - channel(rough, 4, 16, 0) <
          channel(rough, 16, 16, 1) - channel(rough, 16, 16, 0));
    // The same on any number of threads.
    slash::jobs::Pool pool(4);
    auto shared = render::create_software_device(&pool);
    CHECK(draw_mirror(*shared, many, 0.3F).pixels == rough.pixels);
}

TEST_CASE("reflected surfaces are lit by the frame's lights") {
    auto device = render::create_software_device();
    render::Scene3D scene = mirrored();
    const Image ambient = draw_mirror(*device, scene);
    // A steep sun from beyond the mirror lights the cube's face toward it, over the mirror's top.
    scene.lights[0].direction = {0.0F, -0.9397F, 0.3420F};
    scene.lights[0].up = {0.0F, 0.3420F, 0.9397F};
    const Image lit = draw_mirror(*device, scene);
    CHECK(channel(lit, 16, 16, 1) > channel(ambient, 16, 16, 1) + 20);
    // Its rays to the sun are traced: with the cube casting no shadow the face is no brighter,
    // and with the sun below the mirror's top, the mirror (which casts shadows from both sides)
    // shades it again.
    CHECK(draw_mirror(*device, scene, 0.0F, ShadowCasting::Off).pixels == lit.pixels);
    scene.lights[0].direction = {0.0F, -0.3420F, 0.9397F};
    scene.lights[0].up = {0.0F, 0.9397F, 0.3420F};
    CHECK(channel(draw_mirror(*device, scene), 16, 16, 1) < channel(lit, 16, 16, 1) - 20);
}

TEST_CASE("hardware devices draw SSR where reflections would be traced, for now") {
    for (const auto& device : slash::test::hardware_devices()) {
        CAPTURE(static_cast<int>(device->backend()));
        CHECK(draw_mirror(*device, mirrored()).pixels ==
              draw_mirror(*device, mirrored(false)).pixels);
    }
}
