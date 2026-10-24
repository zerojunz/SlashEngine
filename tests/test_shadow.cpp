#include "../engine/render/src/mesh_shading.hpp"
#include "devices.hpp"

#include "slash/core/json.hpp"
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

// The error a call failed with, or "(succeeded)", so a failed check says why.
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

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// A light of each kind, shining from the -X side so a cube's shadow falls toward +X.
Light3D shadowed(LightType type) {
    Light3D light{.type = type, .shadow = true};
    if (type == LightType::Directional) {
        light.direction = {0.70710677F, -0.70710677F, 0.0F};
        light.up = {0.70710677F, 0.70710677F, 0.0F};
        // At the default normal bias of 2, a light at 45 degrees sits right on the edge of acne
        // with the 5-tap filter in a 2048-texel atlas, as in Godot; a little more keeps the lit
        // floor clean so the checks can be exact.
        light.shadow_normal_bias = 3.0F;
    } else {
        light.position = {-1.5F, 3.0F, 0.0F};
        light.energy = 4.0F;
        light.range = 10.0F;
        light.attenuation = 0.5F;
        light.direction = {0.6401844F, -0.7682213F, 0.0F};
        light.up = {0.7682213F, 0.6401844F, 0.0F};
        light.spot_angle = 50.0F;
        light.shadow_bias = type == LightType::Spot ? 0.03F : 0.1F;
    }
    return light;
}

// A camera straight above the origin looking down, -Z up the image.
render::Scene3D above(const Light3D& light) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.3F, 0.3F, 0.3F};
    scene.lights = {light};
    scene.shadows = {.directional_size = 2048, .positional_size = 2048};
    return scene;
}

struct Caster {
    ShadowCasting shadows = ShadowCasting::On;
    std::uint32_t layers = 1;
};

// A gray floor through the origin, and a cube standing above it, size pixels across.
Image draw_shadow(RenderDevice& device, const render::Scene3D& scene, const Caster& caster,
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
    return pixel(image, where[0], where[1])[1];
}

} // namespace

TEST_CASE("lights with shadows take slots, directional and positional apart") {
    render::Scene3D scene;
    scene.lights = {shadowed(LightType::Omni),
                    {.type = LightType::Directional},
                    shadowed(LightType::Directional),
                    shadowed(LightType::Spot)};
    const std::vector<int> slots = render::detail::shadow_slots(scene);
    CHECK(slots == std::vector<int>{0, -1, 0, 1});
    // Past the slots there are, a light casts no shadow.
    scene.lights.assign(render::kMaxDirectionalShadows + 1, shadowed(LightType::Directional));
    CHECK(render::detail::shadow_slots(scene)[render::kMaxDirectionalShadows] == -1);
}

TEST_CASE("shadow plans split the atlases as Godot's do") {
    render::Scene3D scene = above(shadowed(LightType::Directional));
    scene.lights.push_back(shadowed(LightType::Omni));
    scene.lights.push_back(shadowed(LightType::Spot));
    scene.shadows = {};
    const render::detail::ShadowPlan plan = render::detail::plan_shadows(scene, 1024);
    // The project's 4096 texels, cut to what the device can make.
    CHECK(plan.directional_size == 1024);
    CHECK(plan.positional_size == 1024);
    // Four cascades in the quarters of the directional atlas, then the omni light's six faces and
    // the spot light's one in tiles of a 4x4 grid of the positional atlas.
    REQUIRE(plan.passes.size() == 11);
    const std::array<std::array<std::uint32_t, 2>, 4> cascades{
        {{0, 0}, {512, 0}, {0, 512}, {512, 512}}};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const render::detail::ShadowPass& pass = plan.passes[i];
        CHECK_FALSE(pass.positional);
        CHECK(pass.light == 0);
        CHECK(pass.rect.x == cascades.at(i)[0]);
        CHECK(pass.rect.y == cascades.at(i)[1]);
        CHECK(pass.rect.width == 512);
    }
    for (std::size_t i = 0; i < 7; ++i) {
        CAPTURE(i);
        const render::detail::ShadowPass& pass = plan.passes[4 + i];
        CHECK(pass.positional);
        CHECK(pass.light == (i < 6 ? 1 : 2));
        CHECK(pass.rect.x == (i % 4) * 256);
        CHECK(pass.rect.y == (i / 4) * 256);
        CHECK(pass.rect.width == 256);
    }
    // Cascades reach the splits' share of max_distance or the far plane, whichever is nearer.
    const render::detail::MeshDirectionalShadowUniforms& directional =
        plan.uniforms->directional[0];
    CHECK(directional.splits[0] == doctest::Approx(0.1F + (0.1F * 19.9F)));
    CHECK(directional.splits[3] == doctest::Approx(20.0F));
    CHECK(directional.params[3] == 4.0F);
    CHECK(plan.uniforms->positional[0].params[3] == 6.0F);
    CHECK(plan.uniforms->positional[1].params[3] == 1.0F);
    // Hard shadows take one sample, soft_low 5 and soft_ultra 13.
    CHECK(plan.uniforms->filters[0] == 5.0F);
    scene.shadows.directional_quality = render::ShadowQuality::Hard;
    scene.shadows.positional_quality = render::ShadowQuality::SoftUltra;
    const render::detail::ShadowPlan sharper = render::detail::plan_shadows(scene, 1024);
    CHECK(sharper.uniforms->filters[0] == 1.0F);
    CHECK(sharper.uniforms->filters[1] == 13.0F);

    // Two splits halve the light's share; orthogonal takes it whole.
    scene.lights = {shadowed(LightType::Directional), shadowed(LightType::Directional)};
    scene.lights[0].shadow_mode = render::DirectionalShadowMode::Parallel2Splits;
    scene.lights[1].shadow_mode = render::DirectionalShadowMode::Orthogonal;
    const render::detail::ShadowPlan two = render::detail::plan_shadows(scene, 1024);
    REQUIRE(two.passes.size() == 3);
    CHECK(two.positional_size == 0);
    CHECK(two.passes[0].rect.width == 512);
    CHECK(two.passes[0].rect.height == 512);
    CHECK(two.passes[1].rect.y == 512);
    CHECK(two.passes[2].rect.x == 512);
    CHECK(two.passes[2].rect.height == 1024);

    // No light casts: nothing to draw.
    scene.lights[0].shadow = false;
    scene.lights[1].shadow = false;
    const render::detail::ShadowPlan none = render::detail::plan_shadows(scene, 1024);
    CHECK(none.passes.empty());
    CHECK(none.directional_size == 0);
}

TEST_CASE("shadow passes skip meshes that don't cast for their light") {
    // One shadow map over all the view.
    render::Scene3D scene = above(shadowed(LightType::Directional));
    scene.lights[0].shadow_mode = render::DirectionalShadowMode::Orthogonal;
    const render::detail::ShadowPlan plan = render::detail::plan_shadows(scene, 512);
    REQUIRE_FALSE(plan.passes.empty());
    const render::detail::ShadowPass& pass = plan.passes[0];
    const render::detail::Bounds unit{.low = {-0.5F, -0.5F, -0.5F}, .high = {0.5F, 0.5F, 0.5F}};
    render::detail::MeshDraw draw;
    draw.transform = moved(0, 1, 0);
    CHECK(render::detail::shadow_pass_draws(pass, draw, unit));
    draw.shadows = ShadowCasting::Off;
    CHECK_FALSE(render::detail::shadow_pass_draws(pass, draw, unit));
    draw.shadows = ShadowCasting::ShadowsOnly;
    CHECK(render::detail::shadow_pass_draws(pass, draw, unit));
    draw.material.transparency = render::Transparency::Alpha;
    CHECK_FALSE(render::detail::shadow_pass_draws(pass, draw, unit));
    draw.material.transparency = render::Transparency::AlphaScissor;
    CHECK(render::detail::shadow_pass_draws(pass, draw, unit));
    draw.layers = 2;
    scene.lights[0].shadow_caster_mask = 1;
    const render::detail::ShadowPlan masked = render::detail::plan_shadows(scene, 512);
    CHECK_FALSE(render::detail::shadow_pass_draws(masked.passes[0], draw, unit));
    // Far off to the side of every cascade.
    draw.layers = 1;
    draw.transform = moved(500, 0, 0);
    CHECK_FALSE(render::detail::shadow_pass_draws(pass, draw, unit));

    // Shadow maps cull as the material does, the other way round with reverse cull face, and
    // not at all for double-sided casters.
    render::detail::MeshDraw culled;
    CHECK(render::detail::shadow_cull(pass, culled) == render::CullMode::Back);
    culled.shadows = ShadowCasting::DoubleSided;
    CHECK(render::detail::shadow_cull(pass, culled) == render::CullMode::Disabled);
    culled.shadows = ShadowCasting::On;
    render::detail::ShadowPass reversed = pass;
    reversed.reverse_cull = true;
    CHECK(render::detail::shadow_cull(reversed, culled) == render::CullMode::Front);
}

TEST_CASE("every device darkens what a cube shades from each kind of light") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        for (const LightType type : {LightType::Directional, LightType::Omni, LightType::Spot}) {
            CAPTURE(static_cast<int>(type));
            const Light3D light = shadowed(type);
            const render::Scene3D scene = above(light);
            render::Scene3D unshadowed = scene;
            unshadowed.lights[0].shadow = false;
            const Image with = draw_shadow(*device, scene, {});
            const Image without = draw_shadow(*device, unshadowed, {});
            // In the shadow, only the ambient light is left; away from it, nothing changes.
            CAPTURE(at(with, kShaded));
            CAPTURE(at(without, kShaded));
            CHECK(at(with, kShaded) + 30 < at(without, kShaded));
            CHECK(std::abs(at(with, kLit) - at(without, kLit)) <= 1);
            CHECK(std::abs(at(with, kTop) - at(without, kTop)) <= 1);

            // A shadow of half opacity is halfway between.
            render::Scene3D half = scene;
            half.lights[0].shadow_opacity = 0.5F;
            const int between = at(draw_shadow(*device, half, {}), kShaded);
            CHECK(between > at(with, kShaded) + 5);
            CHECK(between + 5 < at(without, kShaded));
            // Opacity 0, a mesh that doesn't cast, and one off the light's caster mask leave no
            // shadow.
            half.lights[0].shadow_opacity = 0.0F;
            CHECK(std::abs(at(draw_shadow(*device, half, {}), kShaded) - at(without, kShaded)) <=
                  1);
            CHECK(
                std::abs(at(draw_shadow(*device, scene, {.shadows = ShadowCasting::Off}), kShaded) -
                         at(without, kShaded)) <= 1);
            render::Scene3D masked = scene;
            masked.lights[0].shadow_caster_mask = 2;
            CHECK(std::abs(at(draw_shadow(*device, masked, {.layers = 1}), kShaded) -
                           at(without, kShaded)) <= 1);
            // A shadows-only mesh casts but isn't seen: the floor shows where its top was.
            const Image only = draw_shadow(*device, scene, {.shadows = ShadowCasting::ShadowsOnly});
            CHECK(at(only, kShaded) == at(with, kShaded));
            CHECK(pixel(only, kTop[0], kTop[1])[0] == pixel(only, kTop[0], kTop[1])[1]);
            CHECK(pixel(with, kTop[0], kTop[1])[0] > pixel(with, kTop[0], kTop[1])[1] + 50);
        }
    }
}

TEST_CASE("GPUs draw shadows as the software device does") {
    render::Scene3D scene = above(shadowed(LightType::Directional));
    scene.lights.push_back(shadowed(LightType::Omni));
    scene.lights.push_back(shadowed(LightType::Spot));
    scene.lights[2].position = {1.5F, 3.0F, 1.0F};
    scene.lights[2].direction = {-0.4472136F, -0.8944272F, 0.0F};
    scene.lights[2].up = {-0.8944272F, 0.4472136F, 0.0F};
    scene.shadows.directional_quality = render::ShadowQuality::SoftHigh;
    scene.shadows.positional_quality = render::ShadowQuality::Hard;
    const Image expected = draw_shadow(*render::create_software_device(), scene, {}, 64);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image image = draw_shadow(*device, scene, {}, 64);
        // Edges, of meshes and of shadows, may land a pixel apart.
        int differing = 0;
        int worst = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = pixel(image, x, y);
                const std::array<int, 4> b = pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 3 ? 1 : 0;
                worst = std::max(worst, most);
            }
        }
        CAPTURE(worst);
        CHECK(differing <= 40);
    }
}

TEST_CASE("devices check shadow settings and light numbers") {
    for (auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto target = device->create_texture({.width = 4, .height = 4, .render_target = true});
        REQUIRE(target.ok());
        render::Scene3D scene = above(shadowed(LightType::Directional));
        scene.shadows.directional_size = 300;
        auto began = device->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_FALSE(began.ok());
        CHECK(began.error().message.find("powers of two") != std::string::npos);
        scene.shadows.directional_size = 512;
        scene.lights[0].shadow_bias = std::nanf("");
        began = device->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_FALSE(began.ok());
        CHECK(began.error().message.find("shadow numbers are finite") != std::string::npos);
        device->destroy_texture(target.value());
    }
}

TEST_CASE("project settings name the shadow atlases' sizes and filters") {
    auto parsed = slash::json::parse(R"({
        "directional_shadow": {"size": 8192, "soft_shadow_filter_quality": "hard"},
        "positional_shadow": {"atlas_size": 1024, "soft_shadow_filter_quality": "soft_ultra"}
    })");
    REQUIRE(parsed.ok());
    const render::ShadowSettings settings = render::shadow_settings_from_json(parsed.value());
    CHECK(settings.directional_size == 8192);
    CHECK(settings.directional_quality == render::ShadowQuality::Hard);
    CHECK(settings.positional_size == 1024);
    CHECK(settings.positional_quality == render::ShadowQuality::SoftUltra);
    // What isn't valid keeps its default.
    auto odd = slash::json::parse(R"({"directional_shadow": {"size": 1000,
        "soft_shadow_filter_quality": "blurry"}})");
    REQUIRE(odd.ok());
    CHECK(render::shadow_settings_from_json(odd.value()) == render::ShadowSettings{});
    CHECK(render::valid_shadow_atlas_size(256));
    CHECK(render::valid_shadow_atlas_size(16384));
    CHECK_FALSE(render::valid_shadow_atlas_size(128));
    CHECK_FALSE(render::valid_shadow_atlas_size(32768));
    CHECK_FALSE(render::valid_shadow_atlas_size(3000));
}

// The shadow uniforms are over 10 KiB; SDL's Vulkan device let shaders see only their first 4 KiB
// until cmake/patches/sdl-vulkan-uniform-range.cmake, which left the slots past the fifth unlit.
TEST_CASE("GPUs draw the last positional shadow slots as the software device does") {
    render::Scene3D scene = above(shadowed(LightType::Directional));
    for (int i = 0; i < 6; ++i) {
        Light3D far = shadowed(LightType::Omni);
        far.position = {40.0F + (static_cast<float>(i) * 10.0F), 3.0F, 40.0F};
        far.energy = 0.01F;
        scene.lights.push_back(far);
    }
    // Six dim lights far away take the first slots; the seventh casts the cube's shadow.
    scene.lights.push_back(shadowed(LightType::Omni));
    const Image expected = draw_shadow(*render::create_software_device(), scene, {}, 64);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image image = draw_shadow(*device, scene, {}, 64);
        int differing = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = pixel(image, x, y);
                const std::array<int, 4> b = pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 3 ? 1 : 0;
            }
        }
        CHECK(differing <= 40);
    }
}
