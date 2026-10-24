#include "../engine/render/src/reflection.hpp"
#include "devices.hpp"

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
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using render::Material3D;
using render::ReflectionProbe3D;
using render::RenderDevice;
using render::Vertex3D;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no probes yet.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

int channel(const Image& image, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return image.pixels.at(((std::size_t{y} * image.width + x) * 4) + c);
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

std::size_t differing(const Image& a, const Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
}

// A square facing +Z, one unit across.
std::vector<Vertex3D> square() {
    const auto corner = [](float x, float y) {
        return Vertex3D{.position = {x * 0.5F, y * 0.5F, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 1.0F) * 0.5F, (1.0F - y) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// A cube of size texels a side and every level, each face one color.
detail::CubeImage faces_cube(const std::array<detail::Float4, 6>& colors) {
    constexpr std::uint32_t kSize = 64;
    detail::CubeImage cube{.size = kSize, .levels = {}};
    for (std::uint32_t level = 0; level <= detail::probe_levels(kSize) - 1; ++level) {
        const std::uint32_t side = kSize >> level;
        std::vector<detail::Float4> texels;
        for (const detail::Float4& color : colors) {
            texels.insert(texels.end(), std::size_t{side} * side, color);
        }
        cube.levels.push_back(std::move(texels));
    }
    return cube;
}

// What reflect_probes returns at world for a perfect mirror whose reflection runs along
// direction, its normal along the same, so the view is the direction too.
detail::ProbeLight mirror_at(const std::vector<ReflectionProbe3D>& probes,
                             const std::vector<detail::CubeImage>& cubes, detail::Float3 world,
                             detail::Float3 direction, std::uint32_t layers = 1) {
    const float length = std::sqrt((direction[0] * direction[0]) + (direction[1] * direction[1]) +
                                   (direction[2] * direction[2]));
    for (float& value : direction) {
        value /= length;
    }
    const std::vector<detail::MeshProbeUniforms> uniforms =
        detail::probe_uniforms({.atlas = {}, .probes = probes, .exposure = 1.0F});
    return detail::reflect_probes(uniforms, cubes, 1.0F, world, direction, direction, 0.0F, layers);
}

// What draw_room draws.
struct Room {
    bool probe = true;
    float roughness = 0.1F; // the floor's
    render::ReflectionAmbient ambient = render::ReflectionAmbient::Disabled;
    std::uint32_t size = 64;
};

// A shiny metal floor before a red wall a meter and a half behind the middle, lit by grey ambient
// light and seen from above the floor, with a reflection probe half a meter up in the middle whose
// cube was drawn from the wall alone: the floor reflects the wall through the probe, since the
// scene has no sky.
Image draw_room(RenderDevice& device, const Room& room) {
    auto target =
        device.create_texture({.width = room.size, .height = room.size, .render_target = true});
    REQUIRE(target.ok());
    auto atlas = device.create_texture({.width = 32, .height = 32, .cubes = 1}, {});
    REQUIRE(atlas.ok());
    auto mesh = device.create_mesh(square(), {});
    REQUIRE(mesh.ok());
    constexpr std::array<float, 16> kFloor{4, 0, 0, 0, 0, 0, -4, 0, 0, 4, 0, 0, 0, 0, 0, 1};
    constexpr std::array<float, 16> kWall{4, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 1, -1.5F, 1};
    const Material3D wall{
        .albedo = {1.0F, 0.2F, 0.1F, 1.0F}, .roughness = 1.0F, .cull = render::CullMode::Disabled};
    const Material3D floor{.albedo = {1.0F, 1.0F, 1.0F, 1.0F},
                           .metallic = 1.0F,
                           .roughness = room.roughness,
                           .cull = render::CullMode::Disabled};
    ReflectionProbe3D probe{.size = {4.0F, 4.0F, 4.0F}, .ambient_mode = room.ambient};
    probe.transform.at(13) = 0.5F;
    const render::Color clear{0.1F, 0.2F, 0.3F, 1.0F};
    for (std::uint32_t face = 0; face < 6; ++face) {
        render::Scene3D scene;
        scene.camera = render::probe_face_view(probe.transform, probe.origin_offset, face);
        scene.projection =
            render::probe_face_projection(probe.size, probe.origin_offset, 0.0F, face);
        scene.ambient = {0.6F, 0.6F, 0.6F};
        scene.capture = render::ProbeCapture{.atlas = atlas.value(), .cube = 0, .face = face};
        REQUIRE(
            device.begin_frame({.target = render::TextureId::None, .clear = clear, .scene = scene})
                .ok());
        REQUIRE(device.draw_mesh(mesh.value(), kWall, wall).ok());
        REQUIRE(device.end_frame().ok());
    }
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 1.0F, 2.5F}, {0.0F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.6F, 0.6F, 0.6F};
    if (room.probe) {
        scene.reflections = {.atlas = atlas.value(), .probes = {probe}, .exposure = 1.0F};
    }
    REQUIRE(device.begin_frame({.target = target.value(), .clear = clear, .scene = scene}).ok());
    REQUIRE(device.draw_mesh(mesh.value(), kFloor, floor).ok());
    REQUIRE(device.draw_mesh(mesh.value(), kWall, wall).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(atlas.value());
    device.destroy_texture(target.value());
    return image.value();
}

} // namespace

TEST_CASE("reflection atlas settings come from project.json's rendering.reflections") {
    auto parsed = slash::json::parse(R"({"reflection_size": 512, "reflection_count": 8})");
    REQUIRE(parsed.ok());
    CHECK(render::reflection_atlas_settings_from_json(parsed.value()) ==
          render::ReflectionAtlasSettings{.reflection_size = 512, .reflection_count = 8});
    // What is missing or not valid keeps its default: sizes are powers of two from 32 to 2048.
    auto wrong = slash::json::parse(R"({"reflection_size": 300, "reflection_count": 0})");
    REQUIRE(wrong.ok());
    CHECK(render::reflection_atlas_settings_from_json(wrong.value()) ==
          render::ReflectionAtlasSettings{});
}

TEST_CASE("a probe's faces look along each axis from its origin, as far as its box reaches") {
    // A probe turned a quarter round y at (1, 2, 3), drawn from one up from its middle.
    const std::array<float, 16> transform{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 2, 3, 1};
    const std::array<float, 3> offset{0.0F, 1.0F, 0.0F};
    // Faces +x, -x, +y, -y, +z, -z hold what lies along the probe's +x, -x, +y, -y, -z and +z:
    // its cube is mirrored along z. Here the probe's +x is the world's -z.
    const std::array<std::array<float, 3>, 6> forward{
        {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}}};
    for (std::uint32_t face = 0; face < 6; ++face) {
        CAPTURE(face);
        const std::array<float, 16> view = render::probe_face_view(transform, offset, face);
        // The camera looks along its -z, the view's third row negated.
        CHECK(-view[2] == doctest::Approx(forward.at(face)[0]));
        CHECK(-view[6] == doctest::Approx(forward.at(face)[1]));
        CHECK(-view[10] == doctest::Approx(forward.at(face)[2]));
        // The eye, at (1, 3, 3), is at the view's origin.
        for (std::size_t row = 0; row < 3; ++row) {
            const float at = (view.at(row) * 1.0F) + (view.at(4 + row) * 3.0F) +
                             (view.at(8 + row) * 3.0F) + view.at(12 + row);
            CHECK(at == doctest::Approx(0.0F).epsilon(0.0001));
        }
        // A turn: its rows are a right-handed set of unit axes.
        const float determinant = (view[0] * ((view[5] * view[10]) - (view[9] * view[6]))) -
                                  (view[4] * ((view[1] * view[10]) - (view[9] * view[2]))) +
                                  (view[8] * ((view[1] * view[6]) - (view[5] * view[2])));
        CHECK(determinant == doctest::Approx(1.0F));
    }
    // Each face sees as far as the furthest wall of the box drawn so far, in Godot's order +x, -x,
    // +y, -y, -z, +z, and at least max_distance.
    const std::array<float, 3> size{2.0F, 8.0F, 2.0F};
    const std::array<float, 6> far{1.0F, 1.0F, 4.0F, 4.0F, 4.0F, 4.0F};
    for (std::uint32_t face = 0; face < 6; ++face) {
        CAPTURE(face);
        CHECK(render::probe_face_projection(size, {}, 0.0F, face) ==
              render::perspective(90.0F, 1.0F, 0.01F, far.at(face)));
        CHECK(render::probe_face_projection(size, {}, 10.0F, face) ==
              render::perspective(90.0F, 1.0F, 0.01F, 10.0F));
    }
    // From the top of a tall box, the walls below are further.
    CHECK(render::probe_face_projection(size, {0.0F, 3.0F, 0.0F}, 0.0F, 3) ==
          render::perspective(90.0F, 1.0F, 0.01F, 7.0F));
}

TEST_CASE("probes reflect as Godot's reflection_process: box projection, order, masks, blend") {
    const detail::Float4 red{1, 0, 0, 1};
    const detail::Float4 green{0, 1, 0, 1};
    const detail::Float4 blue{0, 0, 1, 1};
    const detail::Float4 grey{0.5F, 0.5F, 0.5F, 1};
    // Face -x green, face -z (what lies along the probe's +z) blue.
    const std::vector<detail::CubeImage> cubes{faces_cube({grey, green, grey, grey, grey, blue}),
                                               faces_cube({red, red, red, red, red, red})};
    ReflectionProbe3D box{.size = {2.0F, 2.0F, 2.0F},
                          .blend_distance = 0.0F,
                          .ambient_mode = render::ReflectionAmbient::Disabled};
    // Near the +z wall, looking mostly along -x: without box projection the reflection is what
    // lies along -x, but the ray meets the +z wall first, which box projection looks toward.
    const detail::Float3 near_wall{0.0F, 0.0F, 0.9F};
    const detail::Float3 along{-1.0F, 0.0F, 0.6F};
    detail::ProbeLight light = mirror_at({box}, cubes, near_wall, along);
    CHECK(light.reflection == detail::Float4{0, 1, 0, 1});
    CHECK(light.ambient == detail::Float4{0, 0, 0, 0});
    box.box_projection = true;
    light = mirror_at({box}, cubes, near_wall, along);
    CHECK(light.reflection == detail::Float4{0, 0, 1, 1});
    // Straight along an axis, box projection still finds the wall.
    light = mirror_at({box}, cubes, near_wall, {0.0F, 0.0F, 1.0F});
    CHECK(light.reflection == detail::Float4{0, 0, 1, 1});
    // Outside the box, nothing.
    light = mirror_at({box}, cubes, {0.0F, 0.0F, 1.5F}, along);
    CHECK(light.reflection == detail::Float4{0, 0, 0, 0});

    // Of two probes, largest first in the frame, the smaller is taken first, as Godot walks its
    // cluster's bits from the highest.
    ReflectionProbe3D large{.size = {8.0F, 8.0F, 8.0F},
                            .blend_distance = 0.0F,
                            .ambient_mode = render::ReflectionAmbient::Disabled,
                            .cube = 1};
    box.box_projection = false;
    light = mirror_at({large, box}, cubes, near_wall, along);
    CHECK(light.reflection == detail::Float4{0, 1, 0, 1});
    // A probe whose reflection_mask leaves out the mesh's layers is skipped.
    box.reflection_mask = 2;
    light = mirror_at({large, box}, cubes, near_wall, along);
    CHECK(light.reflection == detail::Float4{1, 0, 0, 1});
    box.reflection_mask = 0xFFFFF;
    // Within blend_distance of a wall a probe fades, by the square of the product along each
    // axis, and the next one makes up the rest: 0.2² of the small one here.
    box.blend_distance = 0.5F;
    light = mirror_at({large, box}, cubes, near_wall, along);
    CHECK(light.reflection[0] == doctest::Approx(0.96F));
    CHECK(light.reflection[1] == doctest::Approx(0.04F));
    CHECK(light.reflection[3] == doctest::Approx(1.0F));
    // Intensity scales the light but not how much of the next probe's it takes.
    box.blend_distance = 0.0F;
    box.intensity = 0.5F;
    light = mirror_at({large, box}, cubes, near_wall, along);
    CHECK(light.reflection == detail::Float4{0, 0.5F, 0, 1});

    // Ambient light: the blurriest level along the normal, or a color times its energy, sRGB.
    box.intensity = 1.0F;
    box.ambient_mode = render::ReflectionAmbient::Color;
    box.ambient_color = {1.0F, 0.5F, 0.0F};
    box.ambient_color_energy = 2.0F;
    light = mirror_at({box}, cubes, {}, along);
    CHECK(light.ambient[0] == doctest::Approx(2.0F));
    CHECK(light.ambient[1] == doctest::Approx(2.0F * 0.21404F).epsilon(0.001));
    CHECK(light.ambient[3] == 1.0F);
    large.ambient_mode = render::ReflectionAmbient::Environment;
    light = mirror_at({large}, cubes, {}, along);
    CHECK(light.ambient == detail::Float4{1, 0, 0, 1});
}

TEST_CASE("every Forward+ device reflects a room in its floor through a probe") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image plain = draw_room(*device, {.probe = false});
        const Image reflected = draw_room(*device, {});
        // The floor in front of the wall shows the wall's red, where it showed nothing before,
        // and the wall itself takes little from the probe: it is rough and not metal.
        CHECK(channel(reflected, 32, 50, 0) > channel(plain, 32, 50, 0) + 40);
        CHECK(channel(reflected, 32, 50, 0) > channel(reflected, 32, 50, 2) + 40);
        CHECK(std::abs(channel(reflected, 32, 12, 0) - channel(plain, 32, 12, 0)) <= 8);
        // With its ambient light, the probe lights the wall in place of the grey ambient light:
        // by its blurriest level behind the wall, mostly the dark blue it was cleared to.
        const Image ambient =
            draw_room(*device, {.ambient = render::ReflectionAmbient::Environment});
        CHECK(channel(ambient, 32, 12, 0) < channel(plain, 32, 12, 0) - 10);
        // A rough floor reflects it blurred, so darker.
        const Image rough = draw_room(*device, {.roughness = 0.6F});
        CHECK(channel(rough, 32, 50, 0) < channel(reflected, 32, 50, 0));
    }
}

TEST_CASE("GPUs draw probes and their captures as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        return;
    }
    const auto software = render::create_software_device();
    for (const float roughness : {0.1F, 0.6F}) {
        CAPTURE(roughness);
        const Image expected = draw_room(*software, {.roughness = roughness});
        const Image drawn = draw_room(*gpu, {.roughness = roughness});
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 100);
        CHECK(difference(drawn, expected) <= 24);
    }
}

TEST_CASE("devices check a frame's probes and captures") {
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 8, .height = 8, .render_target = true});
    auto plain = device->create_texture({.width = 8, .height = 8}, {});
    auto atlas = device->create_texture({.width = 32, .height = 32, .cubes = 2}, {});
    REQUIRE(target.ok());
    REQUIRE(plain.ok());
    REQUIRE(atlas.ok());
    // An atlas's cubes are square and a power of two across.
    CHECK_FALSE(device->create_texture({.width = 32, .height = 16, .cubes = 1}, {}).ok());
    CHECK_FALSE(device->create_texture({.width = 24, .height = 24, .cubes = 1}, {}).ok());
    const auto begin = [&](const render::Scene3D& scene, render::TextureId to) {
        auto began = device->begin_frame({.target = to, .clear = {}, .scene = scene});
        if (began.ok()) {
            REQUIRE(device->end_frame().ok());
        }
        return began.ok();
    };
    render::Scene3D scene;
    scene.reflections = {.atlas = atlas.value(), .probes = {{.cube = 1}}, .exposure = 1.0F};
    CHECK(begin(scene, target.value()));
    // A cube past the atlas's, or an atlas that is a plain texture, is refused.
    scene.reflections.probes.at(0).cube = 2;
    CHECK_FALSE(begin(scene, target.value()));
    scene.reflections = {.atlas = plain.value(), .probes = {{}}, .exposure = 1.0F};
    CHECK_FALSE(begin(scene, target.value()));
    // A capture draws into its atlas, with no target of its own.
    scene.reflections = {};
    scene.capture = render::ProbeCapture{.atlas = atlas.value(), .cube = 1, .face = 5};
    CHECK(begin(scene, render::TextureId::None));
    CHECK_FALSE(begin(scene, target.value()));
    scene.capture->face = 6;
    CHECK_FALSE(begin(scene, render::TextureId::None));
    // The null device checks the same, and draws nothing.
    const auto null = render::create_null_device();
    auto null_atlas = null->create_texture({.width = 32, .height = 32, .cubes = 1}, {});
    REQUIRE(null_atlas.ok());
    scene.capture = render::ProbeCapture{.atlas = null_atlas.value(), .cube = 0, .face = 0};
    CHECK(null->begin_frame({.target = render::TextureId::None, .clear = {}, .scene = scene}).ok());
    CHECK(null->end_frame().ok());
    scene.capture->cube = 1;
    CHECK_FALSE(
        null->begin_frame({.target = render::TextureId::None, .clear = {}, .scene = scene}).ok());
    // OpenGL draws as Compatibility, which has no probes yet: it makes no atlas.
    if (auto gl = slash::test::gl_device(); gl != nullptr) {
        CHECK_FALSE(gl->create_texture({.width = 32, .height = 32, .cubes = 1}, {}).ok());
    }
}
