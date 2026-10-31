#include "../engine/render/src/path_trace.hpp"
#include "devices.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace render = slash::render;
namespace detail = slash::render::detail;
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Float3 = std::array<float, 3>;

namespace {

Float3 scaled(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}
Float3 sum(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// A square of side size around center, facing normal, its corners clockwise seen from the front.
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

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// How many pixels of a and b differ by more than tolerance in a color channel.
int differing(const Image& a, const Image& b, int tolerance) {
    int count = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::array<int, 4> p = pixel(a, x, y);
            const std::array<int, 4> q = pixel(b, x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                if (std::abs(p.at(c) - q.at(c)) > tolerance) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

// A white floor two units across at y 0, a red wall standing at its left edge, a gray box on it
// and a camera above in front, lit by a white sun from above, its shadows on.
struct Room {
    RenderDevice& device;
    render::MeshId floor{};
    render::MeshId wall{};
    render::MeshId box{};
    render::TextureId target{};
    render::Scene3D scene;

    explicit Room(RenderDevice& on, std::uint32_t size) : device(on) {
        floor =
            device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 2.0F), {}).value();
        wall = device.create_mesh(quad({-1, 0.5F, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}, 2.0F), {})
                   .value();
        box = device.create_mesh(cube(), {}).value();
        target =
            device.create_texture({.width = size, .height = size, .render_target = true}).value();
        scene.camera = render::look_at({0.2F, 1.6F, 2.0F}, {-0.1F, 0.1F, -0.2F}, {0, 1, 0});
        scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
        scene.lights = {{.direction = {-0.4F, -1.0F, 0.5F}, .shadow = true}};
    }
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
    Room(Room&&) = delete;
    Room& operator=(Room&&) = delete;
    ~Room() {
        device.destroy_mesh(floor);
        device.destroy_mesh(wall);
        device.destroy_mesh(box);
        device.destroy_texture(target);
    }

    Image draw(std::optional<render::PathTrace3D> paths) {
        render::Scene3D frame = scene;
        frame.path_trace = paths;
        REQUIRE(device.begin_frame({.target = target, .clear = {}, .scene = frame}).ok());
        REQUIRE(device.draw_mesh(floor, moved(0, 0, 0), {.albedo = {0.9F, 0.9F, 0.9F, 1.0F}}).ok());
        REQUIRE(device.draw_mesh(wall, moved(0, 0, 0), {.albedo = {0.9F, 0.1F, 0.1F, 1.0F}}).ok());
        REQUIRE(device
                    .draw_mesh(box, moved(0.4F, 0.3F, -0.2F),
                               {.albedo = {0.5F, 0.5F, 0.5F, 1.0F}, .roughness = 0.6F})
                    .ok());
        REQUIRE(device.end_frame().ok());
        return device.read_texture(target).value();
    }
};

// Where a ray meets a triangle, in doubles, or a negative distance.
double crossing(const Float3& origin, const Float3& direction, const Float3& a, const Float3& b,
                const Float3& c) {
    const std::array<double, 3> e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array<double, 3> e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array<double, 3> d{direction[0], direction[1], direction[2]};
    const auto cross = [](const std::array<double, 3>& x, const std::array<double, 3>& y) {
        return std::array<double, 3>{(x[1] * y[2]) - (x[2] * y[1]), (x[2] * y[0]) - (x[0] * y[2]),
                                     (x[0] * y[1]) - (x[1] * y[0])};
    };
    const auto dot = [](const std::array<double, 3>& x, const std::array<double, 3>& y) {
        return (x[0] * y[0]) + (x[1] * y[1]) + (x[2] * y[2]);
    };
    const std::array<double, 3> p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::abs(det) < 1e-12) {
        return -1.0;
    }
    const std::array<double, 3> s{origin[0] - a[0], origin[1] - a[1], origin[2] - a[2]};
    const double u = dot(s, p) / det;
    const std::array<double, 3> q = cross(s, e1);
    const double v = dot(d, q) / det;
    if (u < 0.0 || v < 0.0 || u + v > 1.0) {
        return -1.0;
    }
    return dot(e2, q) / det;
}

} // namespace

TEST_CASE("the path tracer's hierarchy finds the triangle each ray meets first") {
    // 300 small triangles strewn through a box, met from both sides.
    std::uint32_t state = 12345;
    const auto random = [&] {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<float>(state >> 8U) / 16777216.0F;
    };
    std::vector<Vertex3D> vertices;
    for (int i = 0; i < 300; ++i) {
        const Float3 center{(random() * 4.0F) - 2.0F, (random() * 4.0F) - 2.0F,
                            (random() * 4.0F) - 2.0F};
        for (int corner = 0; corner < 3; ++corner) {
            vertices.push_back(
                {.position = sum(center, {(random() - 0.5F) * 0.8F, (random() - 0.5F) * 0.8F,
                                          (random() - 0.5F) * 0.8F}),
                 .normal = {0, 1, 0}});
        }
    }
    std::vector<std::uint32_t> triangles(vertices.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        triangles[i] = static_cast<std::uint32_t>(i);
    }
    const detail::MeshDraw draw{.transform = moved(0, 0, 0),
                                .material = {.cull = render::CullMode::Disabled}};
    const std::array<detail::PathMesh, 1> meshes{
        detail::PathMesh{.draw = &draw, .vertices = vertices, .triangles = triangles}};
    render::Scene3D scene;
    scene.path_trace = render::PathTrace3D{};
    const detail::PathScene paths = detail::build_path_scene(scene, meshes, 4, 4, {});
    REQUIRE(paths.triangles.size() == 300);
    CHECK(paths.nodes.size() > 1);
    int met = 0;
    for (int ray = 0; ray < 500; ++ray) {
        const Float3 origin{(random() * 8.0F) - 4.0F, (random() * 8.0F) - 4.0F,
                            (random() * 8.0F) - 4.0F};
        Float3 direction{(random() * 2.0F) - 1.0F, (random() * 2.0F) - 1.0F,
                         (random() * 2.0F) - 1.0F};
        const float length =
            std::sqrt((direction[0] * direction[0]) + (direction[1] * direction[1]) +
                      (direction[2] * direction[2]));
        direction = scaled(direction, 1.0F / length);
        double nearest = -1.0;
        for (const detail::PathTriangle& triangle : paths.triangles) {
            const double t = crossing(origin, direction, paths.corners[triangle.corners[0]].world,
                                      paths.corners[triangle.corners[1]].world,
                                      paths.corners[triangle.corners[2]].world);
            if (t > 0.0 && (nearest < 0.0 || t < nearest)) {
                nearest = t;
            }
        }
        const std::optional<detail::PathHit> hit =
            detail::path_hit(paths, origin, direction, 100.0F);
        CAPTURE(ray);
        REQUIRE(hit.has_value() == (nearest > 0.0));
        if (hit) {
            ++met;
            CHECK(std::abs(hit->distance - nearest) < 1e-4);
        }
    }
    CHECK(met > 20);
}

TEST_CASE("direct light alone path traces as the rasterizer lights it") {
    const auto device = render::create_software_device();
    const std::vector<Vertex3D> square = quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 2.0F);
    auto card = device->create_mesh(square, {});
    REQUIRE(card.ok());
    auto target = device->create_texture({.width = 8, .height = 8, .render_target = true});
    REQUIRE(target.ok());
    render::Scene3D scene;
    scene.camera = render::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    scene.lights = {{.direction = {0.3F, -0.2F, -1.0F}},
                    {.type = render::LightType::Omni,
                     .color = {1.0F, 0.5F, 0.2F},
                     .energy = 2.0F,
                     .position = {0.5F, 0.5F, 1.0F},
                     .range = 4.0F}};
    const auto shade = [&](std::optional<render::PathTrace3D> paths) {
        render::Scene3D frame = scene;
        frame.path_trace = paths;
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .scene = frame}).ok());
        REQUIRE(device
                    ->draw_mesh(card.value(), moved(0, 0, 0),
                                {.albedo = {0.5F, 0.6F, 0.7F, 1.0F}, .roughness = 0.7F})
                    .ok());
        REQUIRE(device->end_frame().ok());
        return device->read_texture(target.value()).value();
    };
    const Image drawn = shade(std::nullopt);
    const Image traced = shade(render::PathTrace3D{.samples = 16, .bounces = 0});
    CHECK(device->path_samples(target.value()) == 16);
    // Inside the card, away from its edges, which the paths blur across their pixels; paths spread
    // over each pixel where the rasterizer shades its middle, a step or two apart.
    for (std::uint32_t y = 3; y <= 4; ++y) {
        for (std::uint32_t x = 3; x <= 4; ++x) {
            const std::array<int, 4> a = pixel(drawn, x, y);
            const std::array<int, 4> b = pixel(traced, x, y);
            CAPTURE(a);
            CAPTURE(b);
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(std::abs(a.at(c) - b.at(c)) <= 4);
            }
        }
    }
    device->destroy_mesh(card.value());
    device->destroy_texture(target.value());
}

TEST_CASE("a path-traced still adds up samples over frames, the same on any number of threads") {
    const auto alone = render::create_software_device();
    slash::jobs::Pool pool(3);
    const auto shared = render::create_software_device(&pool);
    Room one(*alone, 24);
    Room other(*shared, 24);
    const render::PathTrace3D four{.samples = 4, .bounces = 2};
    static_cast<void>(one.draw(four));
    const Image summed = one.draw(four);
    CHECK(alone->path_samples(one.target) == 8);
    const Image at_once = other.draw(render::PathTrace3D{.samples = 8, .bounces = 2});
    CHECK(shared->path_samples(other.target) == 8);
    CHECK(summed.pixels == at_once.pixels);
    // Moving the sun starts the sums over.
    one.scene.lights[0].direction = {0.0F, -1.0F, -0.3F};
    static_cast<void>(one.draw(four));
    CHECK(alone->path_samples(one.target) == 4);
    static_cast<void>(one.draw(four));
    CHECK(alone->path_samples(one.target) == 8);
    // A rasterized frame leaves them be.
    static_cast<void>(one.draw(std::nullopt));
    static_cast<void>(one.draw(four));
    CHECK(alone->path_samples(one.target) == 12);
}

TEST_CASE("paths carry light from surface to surface and are shadowed by ray") {
    const auto device = render::create_software_device();
    Room room(*device, 32);
    // The floor in the left third of the frame, beside the red wall: as red as it is green with
    // direct light alone, and reddened by the wall's light when paths bounce.
    const Image direct = room.draw(render::PathTrace3D{.samples = 32, .bounces = 0});
    const Image bounced = room.draw(render::PathTrace3D{.samples = 32, .bounces = 3});
    int floor = 0;
    int direct_red = 0;
    int bounced_red = 0;
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 12; ++x) {
            const std::array<int, 4> a = pixel(direct, x, y);
            if (a[0] > 120 && std::abs(a[0] - a[1]) < 8) {
                const std::array<int, 4> b = pixel(bounced, x, y);
                ++floor;
                direct_red += a[0] - a[1];
                bounced_red += b[0] - b[1];
            }
        }
    }
    REQUIRE(floor > 20);
    CAPTURE(floor);
    CHECK(bounced_red > direct_red + (floor * 8));

    // The box's shadow: floor the sun reaches only with shadows off.
    const Image shadowed = room.draw(render::PathTrace3D{.samples = 8, .bounces = 0});
    room.scene.lights[0].shadow = false;
    const Image unshadowed = room.draw(render::PathTrace3D{.samples = 8, .bounces = 0});
    int in_shadow = 0;
    int lighter = 0;
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const int a = pixel(shadowed, x, y)[1];
            const int b = pixel(unshadowed, x, y)[1];
            in_shadow += b > a + 100 ? 1 : 0;
            lighter += a > b ? 1 : 0;
        }
    }
    CHECK(in_shadow > 20);
    CHECK(lighter == 0);
}

TEST_CASE("emitting surfaces light what is around them when paths bounce") {
    const auto device = render::create_software_device();
    Room room(*device, 16);
    room.scene.lights.clear();
    const auto draw = [&](std::uint32_t bounces) {
        render::Scene3D frame = room.scene;
        frame.path_trace = render::PathTrace3D{.samples = 16, .bounces = bounces};
        REQUIRE(device->begin_frame({.target = room.target, .clear = {}, .scene = frame}).ok());
        REQUIRE(device->draw_mesh(room.floor, moved(0, 0, 0), {}).ok());
        REQUIRE(device
                    ->draw_mesh(room.box, moved(0, 0.5F, 0),
                                {.albedo = {0, 0, 0, 1},
                                 .emission = {1.0F, 0.8F, 0.2F},
                                 .emission_energy = 2.0F})
                    .ok());
        REQUIRE(device->end_frame().ok());
        return device->read_texture(room.target).value();
    };
    // With no lights, the floor is black but for the glowing box's light that bounces off it.
    const Image dark = draw(0);
    const Image lit = draw(1);
    int glowing = 0;
    int red = 0;
    int blue = 0;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const std::array<int, 4> before = pixel(dark, x, y);
            const std::array<int, 4> after = pixel(lit, x, y);
            if (before[0] == 0 && after[0] > 10) {
                ++glowing;
                red += after[0];
                blue += after[2];
            }
        }
    }
    CHECK(glowing > 10);
    CHECK(red > blue * 2);
}

TEST_CASE("devices refuse path-traced frames of no samples or too many bounces") {
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 4, .height = 4, .render_target = true});
    REQUIRE(target.ok());
    for (const render::PathTrace3D paths :
         {render::PathTrace3D{.samples = 0}, render::PathTrace3D{.bounces = 33}}) {
        render::Scene3D scene;
        scene.path_trace = paths;
        const auto began =
            device->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_FALSE(began.ok());
        CHECK(began.error().message.find("1 to 4096 samples") != std::string::npos);
    }
    device->destroy_texture(target.value());
}

TEST_CASE("devices that trace rays path trace as the software device does") {
    const auto software = render::create_software_device();
    for (const auto& device : slash::test::hardware_devices()) {
        CAPTURE(static_cast<int>(device->backend()));
        CAPTURE(device->ray_tracing());
        Room room(*device, 32);
        const render::PathTrace3D four{.samples = 4, .bounces = 2};
        if (!device->ray_tracing()) {
            // Elsewhere a path-traced frame is drawn the raster way.
            CHECK(room.draw(four).pixels == room.draw(std::nullopt).pixels);
            CHECK(device->path_samples(room.target) == 0);
            continue;
        }
        Room twin(*software, 32);
        static_cast<void>(room.draw(four));
        const Image summed = room.draw(four);
        CHECK(device->path_samples(room.target) == 8);
        const Image at_once = twin.draw(render::PathTrace3D{.samples = 8, .bounces = 2});
        // The same paths, but for those a float's rounding sends another way.
        CHECK(differing(summed, at_once, 3) <= 8);
        CHECK(differing(summed, at_once, 40) <= 2);
        // Moving the sun starts the sums over; a rasterized frame leaves them be.
        room.scene.lights[0].direction = {0.0F, -1.0F, -0.3F};
        twin.scene.lights[0].direction = room.scene.lights[0].direction;
        static_cast<void>(room.draw(four));
        CHECK(device->path_samples(room.target) == 4);
        static_cast<void>(room.draw(std::nullopt));
        const Image moved_sun = room.draw(four);
        CHECK(device->path_samples(room.target) == 8);
        const Image twin_moved = twin.draw(render::PathTrace3D{.samples = 8, .bounces = 2});
        CHECK(differing(moved_sun, twin_moved, 3) <= 8);
        CHECK(differing(moved_sun, twin_moved, 40) <= 2);
    }
}
