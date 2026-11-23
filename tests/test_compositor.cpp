// Compositor effects and pipeline files: a game's own screen passes, as Godot 4.3's Compositor and
// CompositorEffect, drawn over the 3D world at points of a Forward+ frame.

#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/compositor.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/shader.hpp"
#include "slash/scene3d/pipeline.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace shader_compiler = slash::shader_compiler;
namespace world = slash::world;

namespace {

using Float3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// A test shader compiled for every backend, once per source.
render::CompiledShader compiled(const render::ShaderSource& source) {
    static std::vector<std::pair<render::ShaderSource, render::CompiledShader>> made;
    for (const auto& [from, shader] : made) {
        if (from == source) {
            return shader;
        }
    }
    auto result = shader_compiler::compile(source);
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
    made.emplace_back(source, std::move(result).value());
    return made.back().second;
}

// A square of side size around center, facing normal, its corners clockwise from the front.
std::vector<render::Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                                   const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        render::Vertex3D vertex{};
        for (std::size_t i = 0; i < 3; ++i) {
            vertex.position.at(i) =
                center.at(i) + (u.at(i) * s * size * 0.5F) + (up.at(i) * t * size * 0.5F);
        }
        vertex.normal = normal;
        vertex.tangent = {u[0], u[1], u[2], 1.0F};
        vertex.uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F};
        return vertex;
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// A cube of side 1 around the origin.
std::vector<render::Vertex3D> cube() {
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
    std::vector<render::Vertex3D> out;
    for (const Face& face : faces) {
        const Float3 center{face.normal[0] * 0.5F, face.normal[1] * 0.5F, face.normal[2] * 0.5F};
        const std::vector<render::Vertex3D> side = quad(center, face.u, face.up, face.normal, 1.0F);
        out.insert(out.end(), side.begin(), side.end());
    }
    return out;
}

// A turn of degrees about the Y axis, then a move to (x, y, z).
Mat4 placed(float degrees, float x, float y, float z) {
    const slash::Vec2 turn = slash::cos_sin(degrees * std::numbers::pi_v<float> / 180.0F);
    return {static_cast<float>(turn.x), 0, static_cast<float>(-turn.y), 0, 0, 1, 0, 0,
            static_cast<float>(turn.y), 0, static_cast<float>(turn.x),  0, x, y, z, 1};
}

render::Scene3D lit_scene(render::RenderMethod method) {
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({2.0F, 1.6F, 3.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.ambient = {0.3F, 0.3F, 0.35F};
    scene.ambient_energy = 0.6F;
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 0.95F, 0.9F},
                     .direction = {-0.4F, -0.8F, -0.45F}}};
    return scene;
}

// Tints what is drawn so far, the sky before the meshes at pre_opaque.
render::ShaderSource tint() {
    return {.path = "shaders/tint.hlsl",
            .code = R"(void fragment() {
    COLOR.rgb = COLOR.rgb * shade.rgb + float3(0.0, 0.0, 0.2) * SCREEN_UV.y;
}
)",
            .type = render::ShaderType::Compositor,
            .params = {{.name = "shade", .components = 4, .slot = 0}}};
}

// Darkens edges found in the depth, and leans each pixel toward its normal: smooth, so devices'
// rounding doesn't move them.
render::ShaderSource outline() {
    return {.path = "shaders/outline.hlsl",
            .code = R"(void fragment() {
    float d = depth_texture(SCREEN_UV);
    float right = depth_texture(SCREEN_UV + float2(SCREEN_PIXEL_SIZE.x, 0.0));
    float below = depth_texture(SCREEN_UV + float2(0.0, SCREEN_PIXEL_SIZE.y));
    float edge = saturate((abs(d - right) + abs(d - below)) * 20.0);
    float4 nr = normal_roughness_texture(SCREEN_UV);
    COLOR.rgb = lerp(COLOR.rgb, nr.xyz * 0.5 + 0.5, 0.25) * (1.0 - 0.8 * edge);
}
)",
            .type = render::ShaderType::Compositor,
            .params = {}};
}

// Blurs the screen a little, darkens toward the corners and multiplies by a texture param.
render::ShaderSource vignette() {
    return {.path = "shaders/vignette.hlsl",
            .code = R"(void fragment() {
    float2 step = SCREEN_PIXEL_SIZE;
    float3 blur = (screen_texture(SCREEN_UV + step * 0.5) +
                   screen_texture(SCREEN_UV - step * 0.5)).rgb * 0.5;
    float fade = 1.0 - strength * dot(SCREEN_UV - 0.5, SCREEN_UV - 0.5);
    COLOR.rgb = blur * fade * texture(paper, SCREEN_UV).rgb;
}
)",
            .type = render::ShaderType::Compositor,
            .params = {{.name = "strength", .components = 1, .slot = 0},
                       {.name = "paper", .components = 0, .slot = 0}}};
}

// The shaders made on a device, and a texture the vignette multiplies by.
struct DeviceEffects {
    render::RenderDevice* device;
    std::array<render::ShaderId, 3> shaders{};
    render::TextureId paper{};

    explicit DeviceEffects(render::RenderDevice& on) : device(&on) {
        const std::array<render::ShaderSource, 3> sources{tint(), outline(), vignette()};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            auto made = on.create_shader(compiled(sources.at(i)));
            REQUIRE_MESSAGE(made.ok(), (made.ok() ? "" : made.error().message));
            shaders.at(i) = made.value();
        }
        std::vector<std::uint8_t> texels;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                texels.insert(texels.end(), {static_cast<std::uint8_t>(200 + (x * 15)),
                                             static_cast<std::uint8_t>(240 - (y * 10)), 220, 255});
            }
        }
        auto texture = on.create_texture({.width = 4, .height = 4}, texels);
        REQUIRE(texture.ok());
        paper = texture.value();
    }
    ~DeviceEffects() {
        for (const render::ShaderId id : shaders) {
            device->destroy_shader(id);
        }
        device->destroy_texture(paper);
    }
    DeviceEffects(const DeviceEffects&) = delete;
    DeviceEffects& operator=(const DeviceEffects&) = delete;
    DeviceEffects(DeviceEffects&&) = delete;
    DeviceEffects& operator=(DeviceEffects&&) = delete;

    // The three effects at the points given (tint, outline, vignette).
    [[nodiscard]] std::vector<render::CompositorEffect3D>
    effects(std::array<render::EffectCallback, 3> points) const {
        std::vector<render::CompositorEffect3D> out(3);
        out[0] = {.shader = shaders[0], .callback = points[0]};
        out[0].constants[0] = {0.7F, 0.85F, 1.1F, 1.0F};
        out[1] = {.shader = shaders[1], .callback = points[1], .normal_roughness = true};
        out[2] = {.shader = shaders[2], .callback = points[2]};
        out[2].constants[0] = {1.6F, 0.0F, 0.0F, 0.0F};
        out[2].textures[0] = paper;
        return out;
    }
};

// A frame of a cube and a blended card over the clear color.
render::Image draw(render::RenderDevice& device, const render::Scene3D& scene, bool meshes = true) {
    auto target = device.create_texture({.width = 64, .height = 64, .render_target = true});
    auto box = device.create_mesh(cube(), {});
    auto card = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0F), {});
    REQUIRE(target.ok());
    REQUIRE(box.ok());
    REQUIRE(card.ok());
    render::Material3D solid;
    solid.albedo = {0.9F, 0.6F, 0.4F, 1.0F};
    render::Material3D sheet;
    sheet.albedo = {0.2F, 0.5F, 0.9F, 0.5F};
    sheet.cull = render::CullMode::Disabled;
    sheet.transparency = render::Transparency::Alpha;
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.2F, 0.3F, 0.5F, 1.0F}, .scene = scene})
                .ok());
    if (meshes) {
        CHECK(device.draw_mesh(box.value(), placed(30, -0.5F, 0, -0.6F), solid).ok());
        CHECK(device.draw_mesh(card.value(), placed(-20, 0.4F, 0.1F, 0.8F), sheet).ok());
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? "" : image.error().message));
    device.destroy_mesh(box.value());
    device.destroy_mesh(card.value());
    device.destroy_texture(target.value());
    return image.value();
}

// How many pixels differ by more than 2 in a channel, and the first of them.
std::pair<int, std::string> differences(const render::Image& got, const render::Image& want) {
    REQUIRE(got.width == want.width);
    REQUIRE(got.height == want.height);
    int count = 0;
    std::string first;
    for (std::uint32_t y = 0; y < got.height; ++y) {
        for (std::uint32_t x = 0; x < got.width; ++x) {
            const std::size_t at = ((std::size_t{y} * got.width) + x) * 4;
            for (std::size_t c = 0; c < 4; ++c) {
                if (std::abs(int{got.pixels[at + c]} - int{want.pixels[at + c]}) > 2) {
                    if (count == 0) {
                        first = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
                    }
                    ++count;
                    break;
                }
            }
        }
    }
    return {count, first};
}

std::array<int, 3> pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]};
}

using render::EffectCallback;

// The scenes every device draws: effects at each point, without meshes, and in Compatibility.
std::vector<std::pair<std::string, render::Scene3D>> scenes(const DeviceEffects& effects) {
    std::vector<std::pair<std::string, render::Scene3D>> out;
    const auto with = [&](std::string name, std::array<EffectCallback, 3> points,
                          render::RenderMethod method = render::RenderMethod::ForwardPlus) {
        render::Scene3D scene = lit_scene(method);
        scene.effects = effects.effects(points);
        out.emplace_back(std::move(name), std::move(scene));
    };
    with("pre_opaque, post_opaque, post_transparent",
         {EffectCallback::PreOpaque, EffectCallback::PostOpaque, EffectCallback::PostTransparent});
    with("post_sky, pre_transparent, pre_transparent",
         {EffectCallback::PostSky, EffectCallback::PreTransparent, EffectCallback::PreTransparent});
    with("compatibility",
         {EffectCallback::PreOpaque, EffectCallback::PostOpaque, EffectCallback::PostTransparent},
         render::RenderMethod::Compatibility);
    return out;
}

} // namespace

TEST_CASE("a compositor shader compiles into a screen pass and its CPU twin") {
    const render::CompiledShader shader = compiled(vignette());
    CHECK(shader.type == render::ShaderType::Compositor);
    CHECK_FALSE(shader.spirv_fragment.empty());
    CHECK_FALSE(shader.cpu_fragment.empty());
    CHECK_FALSE(shader.wgsl_fragment.empty());
    // A param named as a screen built-in is refused.
    render::ShaderSource taken = tint();
    taken.params[0].name = "COLOR";
    CHECK_FALSE(shader_compiler::compile(taken).ok());
}

TEST_CASE("compositor effects change the frame at their points on the software device") {
    const auto software = render::create_software_device();
    const DeviceEffects effects(*software);
    const render::Image plain = draw(*software, lit_scene(render::RenderMethod::ForwardPlus));
    render::Scene3D tinted = lit_scene(render::RenderMethod::ForwardPlus);
    tinted.effects = {effects.effects(
        {EffectCallback::PreOpaque, EffectCallback::PreOpaque, EffectCallback::PreOpaque})[0]};
    const render::Image sky_only = draw(*software, tinted);
    // pre_opaque tints the sky but not the cube drawn after it.
    CHECK(pixel(sky_only, 2, 2) != pixel(plain, 2, 2));
    CHECK(pixel(sky_only, 30, 28) == pixel(plain, 30, 28));
    // post_transparent tints the cube too.
    tinted.effects[0].callback = EffectCallback::PostTransparent;
    const render::Image over_all = draw(*software, tinted);
    CHECK(pixel(over_all, 30, 28) != pixel(plain, 30, 28));
    // A disabled frame: Compatibility draws no effects.
    render::Scene3D compatibility = lit_scene(render::RenderMethod::Compatibility);
    const render::Image compatible = draw(*software, compatibility);
    compatibility.effects = tinted.effects;
    CHECK(differences(draw(*software, compatibility), compatible).first == 0);
    // Without meshes, every point draws over the clear color.
    tinted.effects[0].callback = EffectCallback::PostOpaque;
    CHECK(pixel(draw(*software, tinted, false), 30, 30) !=
          pixel(draw(*software, lit_scene(render::RenderMethod::ForwardPlus), false), 30, 30));
}

TEST_CASE("a device refuses effects whose shader or textures it doesn't have") {
    const auto software = render::create_software_device();
    const DeviceEffects effects(*software);
    auto target = software->create_texture({.width = 8, .height = 8, .render_target = true});
    REQUIRE(target.ok());
    render::Scene3D scene = lit_scene(render::RenderMethod::ForwardPlus);
    scene.effects = effects.effects(
        {EffectCallback::PreOpaque, EffectCallback::PreOpaque, EffectCallback::PreOpaque});
    scene.effects[2].textures[1] = static_cast<render::TextureId>(9999);
    auto begun = software->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
    REQUIRE_FALSE(begun.ok());
    CHECK(begun.error().message == "compositor effect 2 names no texture 9999");
    scene.effects.assign(render::kMaxCompositorEffects + 1, scene.effects[0]);
    begun = software->begin_frame({.target = target.value(), .clear = {}, .scene = scene});
    REQUIRE_FALSE(begun.ok());
    CHECK(begun.error().message.starts_with("a scene takes up to 16 compositor effects"));
    software->destroy_texture(target.value());
}

TEST_CASE("every device draws compositor effects as the software device does") {
    const auto software = render::create_software_device();
    std::vector<render::Image> expected;
    {
        const DeviceEffects effects(*software);
        for (const auto& [name, scene] : scenes(effects)) {
            expected.push_back(draw(*software, scene));
        }
        // The effects show: the frame with them isn't the frame without.
        CHECK(
            differences(expected[0], draw(*software, lit_scene(render::RenderMethod::ForwardPlus)))
                .first > 1000);
    }
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const DeviceEffects effects(*device);
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        const auto list = scenes(effects);
        for (std::size_t i = 0; i < list.size(); ++i) {
            CAPTURE(list.at(i).first);
            // The OpenGL devices draw only as Compatibility, which draws no effects.
            const render::Image& want = opengl ? expected.back() : expected.at(i);
            const render::Image image = draw(*device, list.at(i).second);
            const auto [count, first] = differences(image, want);
            CAPTURE(first);
            CHECK(count <= 12);
            if (count > 12) {
                auto drawn = render::encode_png(image);
                auto wanted = render::encode_png(want);
                REQUIRE(drawn.ok());
                REQUIRE(wanted.ok());
                MESSAGE("drawn, as a base64 PNG: " << slash::base64_encode(drawn.value()));
                MESSAGE("expected, as a base64 PNG: " << slash::base64_encode(wanted.value()));
            }
        }
    }
}

TEST_CASE("pipeline files are checked and read") {
    const json::Value good = parse(R"({
        "passes": {"ssao": false, "glow": true},
        "effects": [
            {"name": "outline", "material": "fx/outline.material.json",
             "effect_callback_type": "post_opaque", "needs_normal_roughness": true,
             "material_params": {"width": 2}, "code": "pulse"},
            {"name": "grade", "material": "fx/grade.material.json", "enabled": false}
        ]})");
    CHECK(scene3d::validate_pipeline(good).empty());
    auto file = scene3d::parse_pipeline(good);
    REQUIRE(file.ok());
    CHECK(file.value().passes == std::map<std::string, bool>{{"glow", true}, {"ssao", false}});
    REQUIRE(file.value().effects.size() == 2);
    const scene3d::PipelineEffect& outline = file.value().effects[0];
    CHECK(outline.callback == EffectCallback::PostOpaque);
    CHECK(outline.needs_normal_roughness);
    CHECK(outline.code == "pulse");
    CHECK(outline.material_params.at("width").as_double() == 2.0);
    CHECK(file.value().effects[1].callback == EffectCallback::PostTransparent);
    CHECK_FALSE(file.value().effects[1].enabled);

    const auto first_error = [](std::string_view text) {
        const auto errors = scene3d::validate_pipeline(parse(text));
        REQUIRE_FALSE(errors.empty());
        return errors.front();
    };
    CHECK(first_error(R"({"passes": {"bloom": false}})").path == "/passes/bloom");
    CHECK(first_error(R"({"effects": [{"name": "a", "material": "a.png"}]})").path ==
          "/effects/0/material");
    CHECK(first_error(R"({"effects": [{"name": "a", "material": "a.material.json",
                          "effect_callback_type": "post_sky_x"}]})")
              .path == "/effects/0/effect_callback_type");
    const json::ValidationError twice = first_error(
        R"({"effects": [{"name": "a", "material": "a.material.json"},
                        {"name": "a", "material": "b.material.json"}]})");
    CHECK(twice.path == "/effects/1/name");
    CHECK(twice.message == "another effect is named \"a\"");
    CHECK(scene3d::effect_callback_name(EffectCallback::PreTransparent) == "pre_transparent");
    CHECK(scene3d::is_pipeline_path("render/look.pipeline.json"));
}

TEST_CASE("a pipeline file turns passes off over the environment's") {
    render::Scene3D scene = lit_scene(render::RenderMethod::ForwardPlus);
    scene.ssao.enabled = true;
    scene.ssr.enabled = true;
    scene.post.glow = true;
    scene.fog.enabled = true;
    scene.use_taa = true;
    scene3d::apply_pipeline_passes({{"ssao", false}, {"glow", true}, {"taa", false}}, scene);
    CHECK_FALSE(scene.ssao.enabled);
    CHECK(scene.ssr.enabled);
    CHECK(scene.post.glow);
    CHECK(scene.fog.enabled);
    CHECK_FALSE(scene.use_taa);
}

namespace {

// A world whose files are in a map, as a project folder's would be.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        world.set_file_reader([this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        });
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }
};

} // namespace

TEST_CASE("a Environment3D's or a Camera3D's compositor draws its pipeline file's effects") {
    Scene scene;
    scene.files["shaders/grade.hlsl"] = "void fragment() {\n    COLOR.rgb *= tint.rgb;\n}\n";
    scene.files["fx/grade.material.json"] = R"({"format": 1, "params": {
        "tint": {"type": "color", "value": [1, 1, 1]}},
        "shader": "shaders/grade.hlsl", "shader_type": "compositor"})";
    scene.files["fx/flat.material.json"] = R"({"format": 1, "color": [1, 0, 0, 1]})";
    scene.files["look.pipeline.json"] = R"({"effects": [
        {"name": "grade", "material": "fx/grade.material.json",
         "material_params": {"tint": [0.2, 1, 0.2]}}]})";
    scene.files["night.pipeline.json"] = R"({"effects": [
        {"name": "grade", "material": "fx/grade.material.json", "code": "night"}]})";
    const entt::entity camera =
        scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("sun", R"({"Transform3D": {"rotation": [-0.6, 0.3, 0]},
                            "SunLight3D": {}})");
    scene.create("box", R"({"Transform3D": {}, "Model3D": {}, "BoxGeometry": {}})");
    const entt::entity environment =
        scene.create("environment", R"({"Environment3D": {"background_mode": "color",
                           "background_color": [0.6, 0.6, 0.6, 1]}})");
    const auto center = [&] {
        auto captured = canvas::capture(scene.world, scene.assets, {.scale = 0.125F});
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        return pixel(captured.value().image, captured.value().image.width / 2,
                     captured.value().image.height / 2);
    };
    const std::array<int, 3> plain = center();
    CHECK(plain[0] == plain[1]);
    auto& env = scene.world.ecs().get<world::Environment3D>(environment);
    env.compositor = "look.pipeline.json";
    const std::array<int, 3> graded = center();
    CHECK(graded[1] > graded[0] + 40);
    // The camera's compositor takes the environment's place, and its code changes the effect.
    scene3d::add_compositor_code("night", [](const world::World&, scene3d::PipelineEffect& effect) {
        effect.material_params["tint"] = json::Value(json::Array{0.2, 0.2, 1.0});
    });
    auto& lens = scene.world.ecs().get<world::Camera3D>(camera);
    lens.compositor = "night.pipeline.json";
    const std::array<int, 3> night = center();
    CHECK(night[2] > night[1] + 40);
    scene3d::add_compositor_code("night", {});
    CHECK(center() == plain);

    // A canvas material, or code no one registered, can't draw, as a problem says.
    scene.files["bad.pipeline.json"] = R"({"effects": [
        {"name": "flat", "material": "fx/flat.material.json"}]})";
    lens.compositor = "bad.pipeline.json";
    auto captured = canvas::capture(scene.world, scene.assets, {.scale = 0.125F});
    REQUIRE(captured.ok());
    CHECK(pixel(captured.value().image, captured.value().image.width / 2,
                captured.value().image.height / 2) == plain);
}
