#include "../engine/render/src/post_shading.hpp"
#include "../engine/render/src/sky_shading.hpp"
#include "devices.hpp"

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
#include <string>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Fog3D;
using render::Image;
using render::RenderDevice;
using render::Scene3D;
using render::Sky3D;
using render::SkyKind;
using render::Vertex3D;
using Float3 = std::array<float, 3>;

namespace {

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

int byte(float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

// The sky's color where it looks along eye, worked out on the CPU at the target's top left, in
// sRGB as Compatibility draws it (Forward+ leaves it linear for its post pass).
Float3 sky_color(Scene3D scene, const Float3& eye) {
    scene.method = render::RenderMethod::Compatibility;
    const detail::SkyUniforms uniforms = detail::sky_uniforms(scene, 8, 8);
    const detail::Float4 color =
        detail::shade_sky(uniforms, {.eye = eye, .coords = {0.0F, 0.0F}}, {}, 0.5F, 0.5F);
    return {color[0], color[1], color[2]};
}

// Whether a device draws every scene as Compatibility, as the OpenGL devices do.
bool draws_as_compatibility(const RenderDevice& device) {
    return device.backend() == render::Backend::OpenGl ||
           device.backend() == render::Backend::OpenGlEs;
}

// An sRGB color after the sky's trip to linear and back, which Godot's approximations of the two
// don't quite undo.
Float3 round_trip(const Float3& color) {
    return detail::linear_to_srgb(detail::srgb_to_linear(color));
}

// Checks the sky's color against an sRGB color it works with.
void check_color(const Float3& got, const Float3& expected) {
    const Float3 wanted = round_trip(expected);
    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(got.at(i) == doctest::Approx(wanted.at(i)).epsilon(2e-3));
    }
}

// A square facing the camera, half as wide as half, its corners clockwise from the front.
std::vector<Vertex3D> square(float half) {
    const auto corner = [&](float x, float y) {
        return Vertex3D{.position = {x * half, y * half, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(x + 1.0F) * 0.5F, (1.0F - y) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// A camera at the origin looking along forward, 90 degrees across a square target.
Scene3D looking(const Float3& forward, const Float3& up = {0.0F, 1.0F, 0.0F}) {
    Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 0.0F}, forward, up);
    scene.projection = render::perspective(90.0F, 1.0F, 0.05F, 100.0F);
    return scene;
}

// An orthogonal camera on +Z, 5 from the origin, seeing from -1 to 1 both ways.
Scene3D facing() {
    Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(2.0F, 1.0F, 0.1F, 10.0F);
    return scene;
}

// A frame of scene, size pixels square, with a square of material at the origin when there is
// one.
Image draw(RenderDevice& device, const Scene3D& scene, const render::Material3D* material = nullptr,
           std::uint32_t size = 16, render::Color clear = {0.0F, 0.0F, 0.0F, 1.0F}) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(0.5F), {});
    REQUIRE(mesh.ok());
    REQUIRE(device.begin_frame({.target = target.value(), .clear = clear, .scene = scene}).ok());
    if (material != nullptr) {
        REQUIRE(device.draw_mesh(mesh.value(), kIdentity, *material).ok());
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return image.value();
}

} // namespace

TEST_CASE("a procedural sky runs from its top through the horizon to the ground, sun on top") {
    Scene3D scene;
    scene.sky.kind = SkyKind::Procedural;
    scene.sky.procedural.use_debanding = false;
    const render::ProceduralSky& sky = scene.sky.procedural;
    check_color(sky_color(scene, {0.0F, 1.0F, 0.0F}), sky.sky_top_color);
    check_color(sky_color(scene, {1.0F, 0.0F, 0.0F}), sky.sky_horizon_color);
    check_color(sky_color(scene, {0.0F, -1.0F, 0.0F}), sky.ground_bottom_color);

    // A sun straight overhead, half a degree across, is its color times its energy where it is.
    scene.sky.lights = {{.direction = {0.0F, -1.0F, 0.0F},
                         .color = {1.0F, 0.5F, 0.25F},
                         .energy = 0.8F,
                         .angular_distance = 0.5F}};
    check_color(sky_color(scene, {0.0F, 1.0F, 0.0F}), {0.8F, 0.4F, 0.2F});
    // Its glow reaches sun_angle_max degrees past its edge, and no further.
    const Float3 near = sky_color(scene, {0.0F, 0.9998F, 0.02F}); // about 1.1 degrees away
    CHECK(near[0] > sky.sky_top_color[0] + 0.05F);
    check_color(sky_color(scene, {0.0F, 0.0F, -1.0F}), sky.sky_horizon_color);

    // The energy multiplies everything, and the background's energy too, before the sky's colors
    // go linear, as Godot's Compatibility renderer multiplies them.
    scene.sky.lights.clear();
    scene.sky.procedural.energy_multiplier = 0.5F;
    scene.sky.energy = 0.5F;
    check_color(
        sky_color(scene, {0.0F, 1.0F, 0.0F}),
        {sky.sky_top_color[0] * 0.25F, sky.sky_top_color[1] * 0.25F, sky.sky_top_color[2] * 0.25F});
}

TEST_CASE("a physical sky scatters its sun's light") {
    Scene3D scene;
    scene.sky.kind = SkyKind::Physical;
    scene.sky.physical.use_debanding = false;
    // With no sun, it is black.
    check_color(sky_color(scene, {0.0F, 1.0F, 0.0F}), {0.0F, 0.0F, 0.0F});
    // A sun 30 degrees up in the -Z: the sky is blue overhead, brightest at the sun, and the
    // ground darker than the sky.
    const float up = 0.5F;
    const float across = std::sqrt(1.0F - (up * up));
    scene.sky.lights = {
        {.direction = {0.0F, -up, across}, .energy = 1.0F, .angular_distance = 1.0F}};
    const Float3 zenith = sky_color(scene, {0.0F, 1.0F, 0.0F});
    CHECK(zenith[2] > zenith[0]);
    // Its disk, lit by 0.757 times the sine of its height (Godot's sun energy), is many times
    // brighter than the sky round it, and warmer, its blue scattered away.
    const Float3 sun = sky_color(scene, {0.0F, up, -across});
    CHECK(sun[0] > 0.45F);
    CHECK(sun[0] > zenith[0] * 5.0F);
    CHECK(sun[0] > sun[2]);
    const Float3 ground = sky_color(scene, {0.0F, -1.0F, 0.0F});
    CHECK(ground[2] < zenith[2]);
}

TEST_CASE("fog covers the sky by its sky affect, and a colored background with fog") {
    Scene3D scene;
    scene.sky.kind = SkyKind::Color;
    scene.sky.color = {1.0F, 0.0F, 0.0F};
    scene.fog = {.enabled = true, .light_color = {0.0F, 1.0F, 0.0F}};
    check_color(sky_color(scene, {0.0F, 0.0F, -1.0F}), {0.0F, 1.0F, 0.0F});
    scene.fog.sky_affect = 0.0F;
    check_color(sky_color(scene, {0.0F, 0.0F, -1.0F}), {1.0F, 0.0F, 0.0F});
    // Aerial perspective tints the fog with the sky behind it.
    scene.fog.sky_affect = 1.0F;
    scene.fog.aerial_perspective = 1.0F;
    check_color(sky_color(scene, {0.0F, 0.0F, -1.0F}), {1.0F, 0.0F, 0.0F});
}

TEST_CASE("every device draws a sky behind its meshes, the right way up") {
    Scene3D scene = looking({0.0F, 0.0F, -1.0F});
    scene.sky.kind = SkyKind::Procedural;
    scene.sky.procedural.use_debanding = false;
    scene.sky.procedural.sky_top_color = {0.0F, 0.0F, 1.0F};
    scene.sky.procedural.sky_horizon_color = {0.0F, 0.0F, 1.0F};
    scene.sky.procedural.ground_bottom_color = {1.0F, 0.0F, 0.0F};
    scene.sky.procedural.ground_horizon_color = {1.0F, 0.0F, 0.0F};
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image image = draw(*device, scene);
        CHECK(pixel(image, 8, 2) == std::array<int, 4>{0, 0, 255, 255});
        CHECK(pixel(image, 8, 13) == std::array<int, 4>{255, 0, 0, 255});
    }
}

TEST_CASE("every device wraps a panorama all round") {
    // Red on the left half, blue on the right: east (+X) is a quarter of the way across, and
    // west three quarters.
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto panorama =
            device->create_texture({.width = 2, .height = 1, .filter = render::Filter::Nearest},
                                   std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
        REQUIRE(panorama.ok());
        Scene3D east = looking({1.0F, 0.0F, 0.0F});
        east.sky.kind = SkyKind::Panorama;
        east.sky.panorama.panorama = panorama.value();
        CHECK(pixel(draw(*device, east), 8, 8) == std::array<int, 4>{255, 0, 0, 255});
        Scene3D west = looking({-1.0F, 0.0F, 0.0F});
        west.sky = east.sky;
        CHECK(pixel(draw(*device, west), 8, 8) == std::array<int, 4>{0, 0, 255, 255});
        // Turning the sky half round swaps them.
        west.sky.orientation = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        CHECK(pixel(draw(*device, west), 8, 8) == std::array<int, 4>{255, 0, 0, 255});
        // A panorama sky with no texture is black, as Godot's.
        west.sky.panorama.panorama = render::TextureId::None;
        CHECK(pixel(draw(*device, west), 8, 8) == std::array<int, 4>{0, 0, 0, 255});
        device->destroy_texture(panorama.value());
    }
}

TEST_CASE("every device fogs meshes as Godot's Compatibility renderer does") {
    const render::Material3D white{.albedo = {1, 1, 1, 1}, .unshaded = true};
    const auto expect = [](float amount, float fog = 0.0F) {
        return byte(detail::linear_to_srgb({(1.0F - amount) + (fog * amount), 0.0F, 0.0F})[0]);
    };
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const auto middle = [&](const Fog3D& fog, const render::Material3D& material) {
            Scene3D scene = facing();
            scene.fog = fog;
            return pixel(draw(*device, scene, &material), 8, 8)[0];
        };
        // Exponential: the square is 5 from the camera.
        const Fog3D black{.enabled = true, .light_color = {0.0F, 0.0F, 0.0F}, .density = 0.1F};
        CHECK(std::abs(middle(black, white) - expect(1.0F - std::exp(-0.5F))) <= 1);
        // Depth fog halfway from 2 to 8.
        Fog3D depth = black;
        depth.depth = true;
        depth.density = 1.0F;
        depth.depth_begin = 2.0F;
        depth.depth_end = 8.0F;
        CHECK(std::abs(middle(depth, white) - expect(0.5F)) <= 1);
        // Height fog below 1, with no other fog, where the middle pixel's center is, 1/16 below
        // the origin.
        Fog3D height = black;
        height.density = 0.0F;
        height.height = 1.0F;
        height.height_density = 0.5F;
        CHECK(std::abs(middle(height, white) - expect(1.0F - std::exp(-1.0625F * 0.5F))) <= 1);
        // A material that disables fog has none.
        render::Material3D clear_of_fog = white;
        clear_of_fog.disable_fog = true;
        CHECK(middle(black, clear_of_fog) == 255);
        // A directional light the camera looks toward scatters its light into the fog.
        Scene3D scene = facing();
        scene.fog = {.enabled = true,
                     .light_color = {0.0F, 0.0F, 0.0F},
                     .sun_scatter = 0.1F,
                     .density = 10.0F};
        scene.lights = {{.type = render::LightType::Directional, .direction = {0.0F, 0.0F, 1.0F}}};
        const render::Material3D black_square{.albedo = {0, 0, 0, 1}, .unshaded = true};
        const int scattered = pixel(draw(*device, scene, &black_square), 8, 8)[0];
        CHECK(std::abs(scattered -
                       byte(detail::linear_to_srgb({0.1F * 3.14159265F, 0.0F, 0.0F})[0])) <= 1);
    }
}

TEST_CASE("GPUs draw skies and fog as the software device does") {
    const render::Material3D lit{.albedo = {0.8F, 0.6F, 0.4F, 1.0F}, .roughness = 0.5F};
    const auto texels = [] {
        std::vector<std::uint8_t> out;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 16; ++x) {
                for (const int value : {x * 16, y * 32, 255 - (x * 16), 200}) {
                    out.push_back(static_cast<std::uint8_t>(value));
                }
            }
        }
        return out;
    }();
    for (const SkyKind kind :
         {SkyKind::Color, SkyKind::Procedural, SkyKind::Physical, SkyKind::Panorama}) {
        CAPTURE(static_cast<int>(kind));
        const auto scene_for = [&](render::TextureId texture) {
            Scene3D scene;
            scene.camera =
                render::look_at({0.0F, 0.3F, 2.0F}, {0.0F, 0.2F, 0.0F}, {0.0F, 1.0F, 0.0F});
            scene.projection = render::perspective(80.0F, 1.0F, 0.05F, 50.0F);
            scene.lights = {{.type = render::LightType::Directional,
                             .energy = 1.2F,
                             .direction = {0.3F, -0.5F, -0.8F}}};
            scene.sky = {
                .kind = kind,
                .color = {0.2F, 0.3F, 0.5F},
                .procedural = {.sky_cover = texture},
                .physical = {.night_sky = texture},
                .panorama = {.panorama = texture},
                .energy = 1.1F,
                .lights = {
                    {.direction = {0.3F, -0.5F, -0.8F}, .energy = 1.2F, .angular_distance = 5.0F}}};
            scene.fog = {.enabled = true,
                         .sun_scatter = 0.2F,
                         .density = 0.05F,
                         .aerial_perspective = 0.3F,
                         .sky_affect = 0.6F,
                         .height = 0.1F,
                         .height_density = 0.4F};
            return scene;
        };
        auto software = render::create_software_device();
        auto made = software->create_texture({.width = 16, .height = 8}, texels);
        REQUIRE(made.ok());
        const Image forward = draw(*software, scene_for(made.value()), &lit, 48);
        Scene3D compatibility = scene_for(made.value());
        compatibility.method = render::RenderMethod::Compatibility;
        const Image compatible = draw(*software, compatibility, &lit, 48);
        for (auto& device : slash::test::hardware_devices()) {
            CAPTURE(render::backend_name(device->backend()));
            const Image& expected = draws_as_compatibility(*device) ? compatible : forward;
            auto texture = device->create_texture({.width = 16, .height = 8}, texels);
            REQUIRE(texture.ok());
            const Image image = draw(*device, scene_for(texture.value()), &lit, 48);
            CHECK(difference(image, expected) <= 3);
            device->destroy_texture(texture.value());
        }
    }
}

TEST_CASE("a radiance cube has Godot's levels and every API's faces") {
    // Down to faces 4 texels across.
    CHECK(detail::radiance_levels(32) == 4);
    CHECK(detail::radiance_levels(256) == 7);
    CHECK(detail::radiance_levels(2048) == 10);
    // Each face's middle looks along its axis; +y's first row is toward -z and +z's first texel
    // up and to -x, as every API lays cube faces out.
    CHECK(detail::cube_direction(0, 0.0F, 0.0F) == Float3{1.0F, 0.0F, 0.0F});
    CHECK(detail::cube_direction(2, 0.0F, -1.0F) == Float3{0.0F, 1.0F, -1.0F});
    CHECK(detail::cube_direction(4, -1.0F, -1.0F) == Float3{-1.0F, 1.0F, 1.0F});
    CHECK(detail::cube_direction(5, -1.0F, -1.0F) == Float3{1.0F, 1.0F, -1.0F});
}

TEST_CASE("a radiance is the sky drawn into a cube, blurred more each level") {
    Scene3D scene;
    scene.sky.panorama.panorama = render::TextureId{1}; // its texels are the function's below
    scene.radiance = {.kind = SkyKind::Panorama, .size = 32, .reflections = true};
    // Red to the east (+X) and blue to the west, as the two-texel panorama above.
    const detail::CubeImage cube =
        detail::make_radiance(scene, [](const std::array<float, 2>& coords) {
            return coords[0] < 0.5F ? detail::Float4{1.0F, 0.0F, 0.0F, 1.0F}
                                    : detail::Float4{0.0F, 0.0F, 1.0F, 1.0F};
        });
    REQUIRE(cube.levels.size() == 4);
    // The first level is sharp.
    CHECK(cube.sample({1.0F, 0.0F, 0.0F}, 0.0F) == detail::Float4{1.0F, 0.0F, 0.0F, 1.0F});
    CHECK(cube.sample({-1.0F, 0.0F, 0.0F}, 0.0F) == detail::Float4{0.0F, 0.0F, 1.0F, 1.0F});
    // Each next one blurs the west further into the east, and overhead, at the last, both mix.
    float west = 0.0F;
    for (std::uint32_t level = 1; level < 4; ++level) {
        CAPTURE(level);
        const float blue = cube.sample({1.0F, 0.0F, 0.0F}, static_cast<float>(level))[2];
        CHECK(blue >= west);
        west = blue;
    }
    CHECK(west > 0.0F);
    const detail::Float4 up = cube.sample({0.0F, 1.0F, 0.0F}, 3.0F);
    CHECK(up[0] > 0.3F);
    CHECK(up[2] > 0.3F);
}

TEST_CASE("every device lights meshes with the sky's radiance") {
    const render::Material3D lit{.albedo = {0.8F, 0.6F, 0.4F, 1.0F}, .roughness = 1.0F};
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // A sky of one color lights a rough surface as an ambient light of that color does.
        Scene3D ambient = facing();
        ambient.ambient = {0.3F, 0.5F, 0.7F};
        const Image expected = draw(*device, ambient, &lit);
        Scene3D sky = facing();
        sky.sky.color = {0.3F, 0.5F, 0.7F};
        sky.radiance = {.kind = SkyKind::Color, .size = 32, .ambient = true};
        const Image got = draw(*device, sky, &lit);
        for (std::size_t i = 0; i < 3; ++i) {
            CAPTURE(i);
            CHECK(std::abs(pixel(got, 8, 8).at(i) - pixel(expected, 8, 8).at(i)) <= 2);
        }
        // Half of it, mixed with a black ambient light, is dimmer.
        sky.radiance.ambient_mix = 0.5F;
        const int half = pixel(draw(*device, sky, &lit), 8, 8)[2];
        CHECK(half > 0);
        CHECK(half < pixel(got, 8, 8)[2] - 20);
        // Neither ambient light nor reflections: the radiance lights nothing.
        sky.radiance.ambient = false;
        CHECK(pixel(draw(*device, sky, &lit), 8, 8) == std::array<int, 4>{0, 0, 0, 255});
    }
}

TEST_CASE("every device reflects the sky's radiance in smooth metal") {
    const render::Material3D mirror{.albedo = {1, 1, 1, 1}, .metallic = 1.0F, .roughness = 0.0F};
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto panorama =
            device->create_texture({.width = 2, .height = 1, .filter = render::Filter::Nearest},
                                   std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
        REQUIRE(panorama.ok());
        // The mirror faces the camera, so it reflects what is behind it, toward +Z: the sky turned
        // so that +Z is its east, red, then its west, blue. Its ambient light is the sky's too,
        // since Godot occludes reflections where there is no ambient light.
        Scene3D scene = facing();
        scene.sky.panorama.panorama = panorama.value();
        scene.sky.orientation = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        scene.radiance = {
            .kind = SkyKind::Panorama, .size = 32, .ambient = true, .reflections = true};
        const std::array<int, 4> east = pixel(draw(*device, scene, &mirror), 8, 8);
        CHECK(east[0] >= 250);
        CHECK(east[2] <= 5);
        scene.sky.orientation = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1};
        const std::array<int, 4> west = pixel(draw(*device, scene, &mirror), 8, 8);
        CHECK(west[0] <= 5);
        CHECK(west[2] >= 250);
        // The roughest metal reflects the blurriest level, so both colors.
        render::Material3D rough = mirror;
        rough.roughness = 1.0F;
        const std::array<int, 4> blurred = pixel(draw(*device, scene, &rough), 8, 8);
        CHECK(blurred[0] > 20);
        CHECK(blurred[2] > blurred[0]);
        device->destroy_texture(panorama.value());
    }
}

TEST_CASE("GPUs light meshes with radiance as the software device does") {
    // No channel is 0: a physical sky with its sun down raises the night sky to a power under 1,
    // which is steep near 0, so a texel's last bit of filtering would show.
    const auto texels = [] {
        std::vector<std::uint8_t> out;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 16; ++x) {
                for (const int value : {16 + (x * 14), 16 + (y * 28), 255 - (x * 14), 255}) {
                    out.push_back(static_cast<std::uint8_t>(value));
                }
            }
        }
        return out;
    }();
    for (const SkyKind kind : {SkyKind::Procedural, SkyKind::Physical, SkyKind::Panorama}) {
        CAPTURE(static_cast<int>(kind));
        const auto scene_for = [&](render::TextureId texture) {
            Scene3D scene;
            scene.camera =
                render::look_at({0.4F, 0.6F, 1.2F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
            scene.projection = render::perspective(70.0F, 1.0F, 0.05F, 50.0F);
            scene.ambient = {0.1F, 0.1F, 0.1F};
            scene.sky = {
                .kind = kind,
                .procedural = {.sky_cover = texture},
                .physical = {.energy_multiplier = 4.0F, .night_sky = texture},
                .panorama = {.panorama = texture},
                .energy = 1.2F,
                .lights = {
                    {.direction = {0.3F, 0.5F, 0.8F}, .energy = 1.0F, .angular_distance = 5.0F}}};
            scene.radiance = {.kind = kind,
                              .size = 64,
                              .ambient = true,
                              .ambient_mix = 0.8F,
                              .reflections = true};
            return scene;
        };
        auto software = render::create_software_device();
        auto made = software->create_texture({.width = 16, .height = 8}, texels);
        REQUIRE(made.ok());
        for (const float roughness : {0.1F, 0.5F, 0.9F}) {
            CAPTURE(roughness);
            const render::Material3D material{
                .albedo = {0.7F, 0.7F, 0.7F, 1.0F}, .metallic = 0.6F, .roughness = roughness};
            const Image forward = draw(*software, scene_for(made.value()), &material, 48);
            Scene3D compatibility = scene_for(made.value());
            compatibility.method = render::RenderMethod::Compatibility;
            const Image compatible = draw(*software, compatibility, &material, 48);
            for (auto& device : slash::test::hardware_devices()) {
                CAPTURE(render::backend_name(device->backend()));
                const Image& expected = draws_as_compatibility(*device) ? compatible : forward;
                auto texture = device->create_texture({.width = 16, .height = 8}, texels);
                REQUIRE(texture.ok());
                const Image image = draw(*device, scene_for(texture.value()), &material, 48);
                CHECK(difference(image, expected) <= 3);
                device->destroy_texture(texture.value());
            }
        }
    }
}

TEST_CASE("devices check sky and fog settings") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto target = device->create_texture({.width = 4, .height = 4, .render_target = true});
        REQUIRE(target.ok());
        const auto refused = [&](const Scene3D& scene) {
            auto began =
                device->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
            REQUIRE_FALSE(began.ok());
            return began.error().message;
        };
        Scene3D scene;
        scene.sky.kind = static_cast<SkyKind>(9);
        CHECK(refused(scene).find("one of five") != std::string::npos);
        scene.sky.kind = SkyKind::Procedural;
        scene.sky.lights.resize(5);
        CHECK(refused(scene).find("at most 4") != std::string::npos);
        scene.sky.lights.clear();
        scene.fog.density = std::nanf("");
        CHECK(refused(scene).find("finite") != std::string::npos);
        scene.fog.density = 0.01F;
        scene.sky.procedural.sky_cover = render::TextureId{77};
        CHECK(refused(scene).find("77") != std::string::npos);
        scene.sky.procedural.sky_cover = render::TextureId::None;
        scene.radiance.kind = static_cast<SkyKind>(9);
        CHECK(refused(scene).find("radiance kind") != std::string::npos);
        scene.radiance.kind = SkyKind::Physical;
        scene.radiance.size = 100;
        CHECK(refused(scene).find("power of two") != std::string::npos);
        scene.radiance.size = 16;
        CHECK(refused(scene).find("not 16") != std::string::npos);
        scene.radiance.size = 64;
        scene.sky.physical.night_sky = render::TextureId{78};
        CHECK(refused(scene).find("78") != std::string::npos);
        device->destroy_texture(target.value());
    }
}
