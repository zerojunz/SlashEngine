#include "devices.hpp"

#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace render = slash::render;
using render::Frame;
using render::Image;
using render::Material3D;
using render::MeshId;
using render::RenderDevice;
using render::TextureId;
using render::Vertex3D;
using Mat4 = std::array<float, 16>;
using Float3 = std::array<float, 3>;

namespace {

// The error a call failed with, or "(succeeded)".
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

// A square of side size around center, facing normal, with u along the texture's x and up
// along its -y: cross(u, up) is normal. Its corners wind clockwise seen from the front.
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

// A turn of degrees about the Y axis, then a move to (x, y, z).
Mat4 placed(float degrees, float x, float y, float z) {
    const slash::Vec2 turn = slash::cos_sin(degrees * 3.14159265F / 180.0F);
    return {turn.x, 0, -turn.y, 0, 0, 1, 0, 0, turn.y, 0, turn.x, 0, x, y, z, 1};
}

render::Scene3D lit_scene() {
    render::Scene3D scene;
    scene.camera = render::look_at({2.5F, 2.0F, 3.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.ambient = {0.2F, 0.2F, 0.3F};
    scene.ambient_energy = 0.5F;
    scene.lights = {
        {.type = render::LightType::Directional,
         .color = {1.0F, 0.95F, 0.9F},
         .direction = {-0.4F, -0.8F, -0.45F}},
        {.type = render::LightType::Omni,
         .color = {1.0F, 0.6F, 0.2F},
         .energy = 2.0F,
         .position = {1.4F, 0.6F, 1.2F},
         .range = 3.0F},
        {.type = render::LightType::Spot,
         .color = {0.3F, 0.6F, 1.0F},
         .energy = 3.0F,
         .position = {-1.5F, 2.0F, 0.5F},
         .direction = {0.3F, -1.0F, 0.0F},
         .range = 5.0F,
         .spot_angle = 30.0F},
    };
    return scene;
}

render::TextureDesc target_desc(std::uint32_t size) {
    return {.width = size, .height = size, .render_target = true};
}

// An 8x8 texture of two colors in a checker of 2x2 squares, the left half's alpha 0.
std::vector<std::uint8_t> checker() {
    std::vector<std::uint8_t> texels;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
            const std::uint8_t alpha = x < 4 ? 0 : 255;
            texels.insert(texels.end(), {static_cast<std::uint8_t>(dark ? 60 : 230),
                                         static_cast<std::uint8_t>(dark ? 90 : 220),
                                         static_cast<std::uint8_t>(dark ? 50 : 200), alpha});
        }
    }
    return texels;
}

// A frame of every kind of mesh draw: lit, textured, emissive, blended, scissored and culled.
Image draw_everything(RenderDevice& device, std::uint32_t size) {
    auto target = device.create_texture(target_desc(size));
    REQUIRE(target.ok());
    const std::vector<std::uint8_t> texels = checker();
    auto texture =
        device.create_texture({.width = 8, .height = 8, .filter = render::Filter::Nearest}, texels);
    REQUIRE(texture.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    const std::vector<Vertex3D> square = quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0F);
    // The same square through indices.
    const std::vector<Vertex3D> corners{square[0], square[1], square[2], square[5]};
    const std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 3};
    auto card = device.create_mesh(corners, indices);
    REQUIRE(card.ok());
    const std::vector<Vertex3D> ground =
        quad({0, -0.5F, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 6.0F);
    auto floor = device.create_mesh(ground, {});
    REQUIRE(floor.ok());

    const render::Scene3D scene = lit_scene();
    auto began = device.begin_frame(
        {.target = target.value(), .clear = {0.05F, 0.05F, 0.1F, 1.0F}, .scene = scene});
    REQUIRE(began.ok());
    const Mat4 identity = placed(0.0F, 0.0F, 0.0F, 0.0F);
    CHECK(device
              .draw_mesh(floor.value(), identity,
                         {.albedo_texture = {.texture = texture.value(), .repeat = true},
                          .roughness = 0.8F,
                          .uv1_scale = {3.0F, 3.0F}})
              .ok());
    CHECK(
        device
            .draw_mesh(box.value(), placed(30.0F, 0.0F, 0.0F, 0.0F),
                       {.albedo = {0.8F, 0.25F, 0.2F, 1.0F}, .metallic = 0.3F, .roughness = 0.45F})
            .ok());
    // A small glowing cube, unshaded and emissive ones beside it.
    CHECK(device
              .draw_mesh(box.value(), placed(10.0F, -1.2F, -0.25F, 1.0F),
                         {.albedo = {0.1F, 0.1F, 0.1F, 1.0F},
                          .emission = {0.2F, 0.8F, 0.3F},
                          .emission_energy = 1.5F})
              .ok());
    CHECK(device
              .draw_mesh(box.value(), placed(-20.0F, 1.3F, -0.25F, -1.2F),
                         {.albedo = {0.9F, 0.9F, 0.2F, 1.0F}, .unshaded = true})
              .ok());
    // A blended card in front of the cube, and a scissored one beside it, drawn before what it
    // stands in front of, so only depth puts it there.
    CHECK(device
              .draw_mesh(
                  card.value(), placed(0.0F, 0.6F, 0.2F, 1.2F),
                  {.albedo = {0.2F, 0.4F, 1.0F, 0.5F}, .transparency = render::Transparency::Alpha})
              .ok());
    CHECK(device
              .draw_mesh(card.value(), placed(35.0F, -0.4F, 0.6F, 1.3F),
                         {.albedo_texture = {.texture = texture.value()},
                          .transparency = render::Transparency::AlphaScissor,
                          .cull = render::CullMode::Disabled})
              .ok());
    // A card facing away from the camera: culled as Godot's default culls it.
    CHECK(device.draw_mesh(card.value(), placed(180.0F, 0.0F, 1.2F, 0.0F), {}).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(box.value());
    device.destroy_mesh(card.value());
    device.destroy_mesh(floor.value());
    device.destroy_texture(texture.value());
    device.destroy_texture(target.value());
    return image.value();
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

} // namespace

TEST_CASE("perspective and orthogonal projections are Godot's, with reversed depth") {
    const Mat4 p = render::perspective(75.0F, 1.5F, 0.05F, 4000.0F);
    const float f = 1.0F / std::tan(37.5F * 3.14159265F / 180.0F);
    CHECK(p[0] == doctest::Approx(f / 1.5F));
    CHECK(p[5] == doctest::Approx(f));
    CHECK(p[11] == -1.0F);
    CHECK(p[15] == 0.0F);
    // The near plane goes to depth 1 and the far plane to 0.
    const auto depth_at = [](const Mat4& m, float distance) {
        const float z = (m[10] * -distance) + m[14];
        const float w = (m[11] * -distance) + m[15];
        return z / w;
    };
    CHECK(depth_at(p, 0.05F) == doctest::Approx(1.0F));
    CHECK(depth_at(p, 4000.0F) == doctest::Approx(0.0F).epsilon(1e-6));
    CHECK(render::Scene3D::perspective_default() ==
          render::perspective(75.0F, 1.0F, 0.05F, 4000.0F));

    const Mat4 o = render::orthogonal(10.0F, 2.0F, 1.0F, 101.0F);
    CHECK(o[0] == doctest::Approx(0.1F));
    CHECK(o[5] == doctest::Approx(0.2F));
    CHECK(o[15] == 1.0F);
    CHECK(depth_at(o, 1.0F) == doctest::Approx(1.0F));
    CHECK(depth_at(o, 101.0F) == doctest::Approx(0.0F));
}

TEST_CASE("look_at sees the target straight ahead, down -Z") {
    const Mat4 camera = render::look_at({0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    // The origin is 5 in front; a point above it is above in the camera's space.
    CHECK(camera[14] == doctest::Approx(-5.0F));
    CHECK(camera[12] == doctest::Approx(0.0F));
    CHECK(camera[5] == doctest::Approx(1.0F));
    const Mat4 side = render::look_at({3.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    // From +X, the world's -Z is to the camera's left... and +Z to its right.
    const float right_of_plus_z = side[8]; // camera x of world (0, 0, 1)
    CHECK(right_of_plus_z == doctest::Approx(-1.0F));
    CHECK(side[14] == doctest::Approx(-3.0F));
}

TEST_CASE("every device checks meshes, scenes and mesh draws") {
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::drawing_devices();
    devices.push_back(render::create_null_device());
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        const std::vector<Vertex3D> triangle{
            {.position = {0, 0, 0}}, {.position = {1, 0, 0}}, {.position = {0, 1, 0}}};
        CHECK(error_of(device->create_mesh(std::span(triangle).first(2), {})) ==
              "a mesh without indices has a multiple of 3 vertices, not 2");
        CHECK(error_of(device->create_mesh(triangle, std::vector<std::uint32_t>{0, 1, 3})) ==
              "index 2 is 3, past the mesh's 3 vertices");
        std::vector<Vertex3D> broken = triangle;
        broken[1].normal[2] = std::numeric_limits<float>::quiet_NaN();
        CHECK(error_of(device->create_mesh(broken, {})) == "vertex 1's numbers are finite");
        auto mesh = device->create_mesh(triangle, {});
        REQUIRE(mesh.ok());
        auto target = device->create_texture(target_desc(8));
        REQUIRE(target.ok());

        render::Scene3D crowded;
        crowded.lights.resize(render::kMaxLights3D + 1);
        CHECK(error_of(
                  device->begin_frame({.target = target.value(), .clear = {}, .scene = crowded})) ==
              "a scene takes up to 64 lights, not 65");
        const Mat4 identity = placed(0.0F, 0.0F, 0.0F, 0.0F);
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
        CHECK(error_of(device->draw_mesh(mesh.value(), identity, {})) ==
              "draw_mesh needs a frame with a scene (Frame::scene)");
        REQUIRE(device->end_frame().ok());

        REQUIRE(
            device->begin_frame({.target = target.value(), .clear = {}, .scene = render::Scene3D{}})
                .ok());
        CHECK(error_of(device->draw_mesh(static_cast<MeshId>(99), identity, {})) == "no mesh 99");
        Mat4 infinite = identity;
        infinite[12] = std::numeric_limits<float>::infinity();
        CHECK(error_of(device->draw_mesh(mesh.value(), infinite, {})) ==
              "a mesh's transform is finite");
        CHECK(error_of(device->draw_mesh(
                  mesh.value(), identity,
                  {.albedo_texture = {.texture = static_cast<TextureId>(77)}})) == "no texture 77");
        CHECK(error_of(device->draw_mesh(mesh.value(), identity,
                                         {.normal_texture = {.texture = target.value()}})) ==
              "texture " + std::to_string(static_cast<std::uint32_t>(target.value())) +
                  " is this frame's target, so it can't be drawn with until the frame ends");
        CHECK(device->draw_mesh(mesh.value(), identity, {}).ok());
        CHECK(error_of(device->update_mesh(mesh.value(), triangle)).starts_with("update_mesh"));
        REQUIRE(device->end_frame().ok());
        CHECK(error_of(device->draw_mesh(mesh.value(), identity, {})) ==
              "draw_mesh needs a frame; call begin_frame first");
        // A mesh's vertices are replaced by as many, as a skinned mesh's are.
        CHECK(error_of(device->update_mesh(mesh.value(), std::span(triangle).first(2))) ==
              "a mesh's new vertices are as many as it has, 3, not 2");
        CHECK(error_of(device->update_mesh(mesh.value(), broken)) ==
              "vertex 1's numbers are finite");
        CHECK(error_of(device->update_mesh(static_cast<MeshId>(99), triangle)) == "no mesh 99");
        CHECK(device->update_mesh(mesh.value(), triangle).ok());
        // A destroyed mesh is gone.
        device->destroy_mesh(mesh.value());
        REQUIRE(
            device->begin_frame({.target = target.value(), .clear = {}, .scene = render::Scene3D{}})
                .ok());
        CHECK(error_of(device->draw_mesh(mesh.value(), identity, {})) ==
              "no mesh " + std::to_string(static_cast<std::uint32_t>(mesh.value())));
        REQUIRE(device->end_frame().ok());
        device->destroy_texture(target.value());
    }
}

TEST_CASE("each mesh draw is a draw of its own, counting its triangles") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto box = device->create_mesh(cube(), {});
        REQUIRE(box.ok());
        auto target = device->create_texture(target_desc(16));
        REQUIRE(target.ok());
        const render::Stats before = device->stats();
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .scene = lit_scene()})
                    .ok());
        REQUIRE(device->draw_mesh(box.value(), placed(0, 0, 0, 0), {}).ok());
        REQUIRE(device->draw_mesh(box.value(), placed(45, 0, 0, 0), {}).ok());
        // Destroyed during the frame, it still draws in it.
        device->destroy_mesh(box.value());
        REQUIRE(device->end_frame().ok());
        CHECK(device->stats().draws - before.draws == 2);
        CHECK(device->stats().triangles - before.triangles == 24);
        device->destroy_texture(target.value());
    }
}

TEST_CASE("every device lights a face by Godot's formulas") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const std::vector<Vertex3D> square = quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 2.0F);
        auto card = device->create_mesh(square, {});
        REQUIRE(card.ok());
        auto target = device->create_texture(target_desc(8));
        REQUIRE(target.ok());
        const auto shade = [&](const render::Scene3D& scene) {
            REQUIRE(
                device->begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
            REQUIRE(device
                        ->draw_mesh(card.value(), placed(0, 0, 0, 0),
                                    {.albedo = {0.5F, 0.5F, 0.5F, 1.0F}})
                        .ok());
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            return pixel(image.value(), 4, 4);
        };
        render::Scene3D scene;
        scene.camera = render::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
        // A white light straight onto a gray face seen head on: Burley diffuse gives the albedo
        // in linear, 0.21497, and Schlick-GGX specular adds 0.005 at roughness 1, so sRGB 129.
        scene.lights = {{.direction = {0.0F, 0.0F, -1.0F}}};
        const std::array<int, 4> lit = shade(scene);
        CAPTURE(lit);
        CHECK(std::abs(lit[0] - 129) <= 1);
        CHECK(lit[0] == lit[1]);
        CHECK(lit[1] == lit[2]);
        CHECK(lit[3] == 255);
        // From behind, only the ambient light is left: 0.21404 times the albedo, sRGB 61.
        scene.lights = {{.direction = {0.0F, 0.0F, 1.0F}}};
        scene.ambient = {0.5F, 0.5F, 0.5F};
        const std::array<int, 4> ambient = shade(scene);
        CAPTURE(ambient);
        CHECK(std::abs(ambient[0] - 61) <= 1);
        // A light whose cull mask misses the draw's layers lights nothing.
        scene.lights = {{.direction = {0.0F, 0.0F, -1.0F}, .cull_mask = 2}};
        CHECK(shade(scene) == ambient);
        device->destroy_mesh(card.value());
        device->destroy_texture(target.value());
    }
}

TEST_CASE("nearer meshes hide farther ones whatever order they are drawn in") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const std::vector<Vertex3D> square = quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 2.0F);
        auto card = device->create_mesh(square, {});
        REQUIRE(card.ok());
        auto target = device->create_texture(target_desc(16));
        REQUIRE(target.ok());
        render::Scene3D scene;
        scene.camera = render::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
        scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
        for (const bool near_first : {true, false}) {
            CAPTURE(near_first);
            REQUIRE(
                device->begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
            const Material3D red{.albedo = {1, 0, 0, 1}, .unshaded = true};
            const Material3D blue{.albedo = {0, 0, 1, 1}, .unshaded = true};
            const Mat4 near = placed(0, 0, 0, 0.5F);
            const Mat4 far = placed(0, 0, 0, -0.5F);
            if (near_first) {
                REQUIRE(device->draw_mesh(card.value(), near, red).ok());
                REQUIRE(device->draw_mesh(card.value(), far, blue).ok());
            } else {
                REQUIRE(device->draw_mesh(card.value(), far, blue).ok());
                REQUIRE(device->draw_mesh(card.value(), near, red).ok());
            }
            REQUIRE(device->end_frame().ok());
            auto image = device->read_texture(target.value());
            REQUIRE(image.ok());
            CHECK(pixel(image.value(), 8, 8) == std::array<int, 4>{255, 0, 0, 255});
        }
        device->destroy_mesh(card.value());
        device->destroy_texture(target.value());
    }
}

TEST_CASE("cull modes hide back faces, front faces or neither") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const std::vector<Vertex3D> square = quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 2.0F);
        auto card = device->create_mesh(square, {});
        REQUIRE(card.ok());
        auto target = device->create_texture(target_desc(8));
        REQUIRE(target.ok());
        render::Scene3D scene;
        scene.camera = render::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
        for (const auto cull :
             {render::CullMode::Back, render::CullMode::Front, render::CullMode::Disabled}) {
            for (const float turn : {0.0F, 180.0F}) {
                CAPTURE(static_cast<int>(cull));
                CAPTURE(turn);
                REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .scene = scene})
                            .ok());
                REQUIRE(device
                            ->draw_mesh(card.value(), placed(turn, 0, 0, 0),
                                        {.albedo = {1, 1, 1, 1}, .unshaded = true, .cull = cull})
                            .ok());
                REQUIRE(device->end_frame().ok());
                auto image = device->read_texture(target.value());
                REQUIRE(image.ok());
                const bool facing = turn == 0.0F;
                const bool shown = cull == render::CullMode::Disabled ||
                                   (cull == render::CullMode::Back) == facing;
                CHECK(pixel(image.value(), 4, 4)[0] == (shown ? 255 : 0));
            }
        }
        device->destroy_mesh(card.value());
        device->destroy_texture(target.value());
    }
}

TEST_CASE("2D draws go over a frame's meshes") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto box = device->create_mesh(cube(), {});
        REQUIRE(box.ok());
        auto target = device->create_texture(target_desc(16));
        REQUIRE(target.ok());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .scene = lit_scene()})
                    .ok());
        REQUIRE(device
                    ->draw_mesh(box.value(), placed(0, 0, 0, 0),
                                {.albedo = {0, 1, 0, 1}, .unshaded = true})
                    .ok());
        const std::vector<render::Vertex2D> corner{
            {.x = 0, .y = 0, .rgba = {255, 0, 255, 255}},
            {.x = 8, .y = 0, .rgba = {255, 0, 255, 255}},
            {.x = 0, .y = 8, .rgba = {255, 0, 255, 255}},
            {.x = 8, .y = 0, .rgba = {255, 0, 255, 255}},
            {.x = 8, .y = 8, .rgba = {255, 0, 255, 255}},
            {.x = 0, .y = 8, .rgba = {255, 0, 255, 255}},
        };
        REQUIRE(device->draw(TextureId::None, corner).ok());
        REQUIRE(device->end_frame().ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel(image.value(), 2, 2) == std::array<int, 4>{255, 0, 255, 255});
        CHECK(pixel(image.value(), 8, 8) == std::array<int, 4>{0, 255, 0, 255});
        device->destroy_mesh(box.value());
        device->destroy_texture(target.value());
    }
}

TEST_CASE("GPUs draw meshes as the software device does") {
    const Image expected = draw_everything(*render::create_software_device(), 96);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image image = draw_everything(*device, 96);
        // Edges may land a pixel apart, and the GPU's sin and pow differ in their last bits.
        int differing = 0;
        int worst = 0;
        std::string first;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = pixel(image, x, y);
                const std::array<int, 4> b = pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                if (most > 2) {
                    if (differing == 0) {
                        first = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
                    }
                    ++differing;
                }
                worst = std::max(worst, most);
            }
        }
        CAPTURE(first);
        CAPTURE(worst);
        CHECK(differing <= 20);
        if (differing > 20) {
            // Both pictures, to see what differs where only the test's output reaches (CI).
            auto drawn = render::encode_png(image);
            auto wanted = render::encode_png(expected);
            REQUIRE(drawn.ok());
            REQUIRE(wanted.ok());
            MESSAGE("drawn, as a base64 PNG: " << slash::base64_encode(drawn.value()));
            MESSAGE("expected, as a base64 PNG: " << slash::base64_encode(wanted.value()));
        }
    }
}
