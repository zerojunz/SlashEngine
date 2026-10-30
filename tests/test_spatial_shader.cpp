// Spatial shaders: a game's own 3D shaders, as Godot's shader_type spatial, whose fragment() gives
// a mesh's surface.

#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/material.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/shader.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#if defined(_WIN32)
// Windows' HLSL compiler, which the Direct3D 12 device compiles a game's shaders with.
#include "../engine/render/src/hlsl_compiler.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
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

render::CompiledShader compiled(const render::ShaderSource& source) {
    auto result = shader_compiler::compile(source);
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
    return std::move(result).value();
}

std::string error_of(const render::ShaderSource& source) {
    auto result = shader_compiler::compile(source);
    return result.ok() ? "(compiled)" : result.error().message;
}

// A glowing, bumpy surface: a number param, a color param and two textures, one of sRGB colors.
render::ShaderSource glow() {
    return {.path = "shaders/glow.hlsl",
            .code = R"(// Pulses.
void fragment() {
    float pulse = 0.5 + 0.5 * sin(TIME * speed);
    ALBEDO = tint.rgb * texture(pattern, UV).rgb;
    ROUGHNESS = 0.4;
    EMISSION = tint.rgb * pulse;
    NORMAL_MAP = texture(bumps, UV * 2.0).rgb;
}
)",
            .type = render::ShaderType::Spatial,
            .params = {{.name = "bumps", .components = 0, .slot = 0},
                       {.name = "pattern", .components = 0, .slot = 1, .source_color = true},
                       {.name = "speed", .components = 1, .slot = 0},
                       {.name = "tint", .components = 4, .slot = 1}}};
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// The first error in a material document, as "path: message", or "(valid)".
std::string first_error(std::string_view text) {
    const std::vector<json::ValidationError> all = canvas::validate_material(parse(text));
    return all.empty() ? "(valid)" : all.front().path + ": " + all.front().message;
}

// The lowercase names a shader file declares at its top level or in a uniform buffer: its globals,
// functions and uniform buffers' fields.
std::vector<std::string> globals_of(const std::string& file) {
    auto text = slash::platform::read_file(std::string(SLASH_TEST_DATA_DIR) +
                                           "/../../engine/render/shaders/" + file);
    REQUIRE(text.ok());
    const std::regex declaration(
        R"(^\s*(?:static const |inline )?[A-Za-z_][A-Za-z0-9_<>]*\s+([a-z_][a-z0-9_]*)\s*[\[;:=(])");
    std::vector<std::string> names;
    std::vector<char> blocks; // 'c' a uniform buffer, 's' a struct, 'f' anything else
    std::istringstream lines(text.value());
    std::string line;
    while (std::getline(lines, line)) {
        line = line.substr(0, line.find("//"));
        if (line.starts_with("#")) {
            continue;
        }
        std::smatch match;
        if ((blocks.empty() || blocks.back() == 'c') &&
            std::regex_search(line, match, declaration)) {
            names.push_back(match[1]);
        }
        for (const char c : line) {
            if (c == '{') {
                const auto start = line.find_first_not_of(' ');
                const std::string_view head =
                    start == std::string::npos ? "" : std::string_view(line).substr(start);
                char block = 'f';
                if (head.starts_with("cbuffer")) {
                    block = 'c';
                } else if (head.starts_with("struct")) {
                    block = 's';
                }
                blocks.push_back(block);
            } else if (c == '}') {
                REQUIRE_FALSE(blocks.empty());
                blocks.pop_back();
            }
        }
    }
    return names;
}

using Float3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;

// A square of side size around center, facing normal, with u along the texture's x and up along
// its -y. Its corners wind clockwise seen from the front.
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
    return {turn.x, 0, -turn.y, 0, 0, 1, 0, 0, turn.y, 0, turn.x, 0, x, y, z, 1};
}

// A shader that reads the built-ins Godot's surfaces are made of, in view space as Godot's.
render::ShaderSource surface() {
    return {.path = "shaders/surface.hlsl",
            .code = R"(float stripe(float x) {
    return step(0.5, frac(x * 4.0));
}

void fragment() {
    float4 texel = texture(pattern, UV);
    ALBEDO = lerp(texel.rgb * tint.rgb, float3(0.9, 0.9, 0.9), stripe(UV.y) * 0.3);
    METALLIC = metal;
    ROUGHNESS = 0.3 + 0.5 * UV.x;
    SPECULAR = 0.4;
    NORMAL_MAP = texture(bumps, UV * 2.0).rgb;
    float rim = pow(1.0 - saturate(dot(NORMAL, VIEW)), 3.0);
    EMISSION = float3(0.2, 0.5, 1.0) * rim + float3(0.1, 0.0, 0.0) * TIME;
    if (cut > 0.0) {
        ALPHA = texel.a;
        ALPHA_SCISSOR_THRESHOLD = cut;
    }
}
)",
            .type = render::ShaderType::Spatial,
            .params = {{.name = "bumps", .components = 0, .slot = 0},
                       {.name = "pattern", .components = 0, .slot = 1, .source_color = true},
                       {.name = "cut", .components = 1, .slot = 0},
                       {.name = "metal", .components = 1, .slot = 1},
                       {.name = "tint", .components = 4, .slot = 2}}};
}

// A glass of half the light through it.
render::ShaderSource glass() {
    return {.path = "shaders/glass.hlsl",
            .code = "void fragment() {\n    ALBEDO = float3(0.3, 0.6, 1.0);\n"
                    "    ALPHA = 0.25 + 0.5 * UV.y;\n}\n",
            .type = render::ShaderType::Spatial,
            .params = {}};
}

// Hills: vertex() lifts a sheet by a height texture and a wave in time and colors it by its UV,
// and fragment() cuts a strip out of it, in its shadows too.
render::ShaderSource hills() {
    return {.path = "shaders/hills.hlsl",
            .code = R"(void vertex() {
    float rise = texture(heights, UV).r * lift;
    VERTEX.y += rise + 0.1 * sin(TIME * 2.0 + VERTEX.x * 3.0);
    NORMAL = normalize(NORMAL + float3(-0.3 * cos(VERTEX.x * 3.0), 0.0, 0.0));
    COLOR = float4(UV, 0.4, 1.0);
}

void fragment() {
    ALBEDO = COLOR.rgb;
    ROUGHNESS = 0.6;
    ALPHA = UV.x > 0.4 && UV.x < 0.55 ? 0.0 : 1.0;
    ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)",
            .type = render::ShaderType::Spatial,
            .params = {{.name = "heights", .components = 0, .slot = 0},
                       {.name = "lift", .components = 1, .slot = 0}}};
}

// A swell: vertex() pushes the corners out along their normals, in the world.
render::ShaderSource swell() {
    return {.path = "shaders/swell.hlsl",
            .code = "void vertex() {\n    VERTEX += normalize(NORMAL) * 0.15;\n}\n",
            .type = render::ShaderType::Spatial,
            .params = {},
            .world_vertex_coords = true};
}

// A card moved along x by vertex(), its right half cut away by fragment().
render::ShaderSource shifted() {
    return {.path = "shaders/shifted.hlsl",
            .code = "void vertex() {\n    VERTEX.x += shift;\n}\n\n"
                    "void fragment() {\n    ALPHA = UV.x < 0.5 ? 1.0 : 0.0;\n"
                    "    ALPHA_SCISSOR_THRESHOLD = 0.5;\n}\n",
            .type = render::ShaderType::Spatial,
            .params = {{.name = "shift", .components = 1, .slot = 0}}};
}

// A sheet of cells by cells squares over -1 to 1 in x and z, facing up.
std::vector<render::Vertex3D> grid(int cells) {
    std::vector<render::Vertex3D> out;
    const float size = 2.0F / static_cast<float>(cells);
    for (int row = 0; row < cells; ++row) {
        for (int column = 0; column < cells; ++column) {
            const Float3 center{-1.0F + ((static_cast<float>(column) + 0.5F) * size), 0.0F,
                                -1.0F + ((static_cast<float>(row) + 0.5F) * size)};
            std::vector<render::Vertex3D> cell =
                quad(center, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, size);
            for (render::Vertex3D& vertex : cell) {
                vertex.uv = {(vertex.position[0] + 1.0F) * 0.5F,
                             (vertex.position[2] + 1.0F) * 0.5F};
            }
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    return out;
}

// Where a point in the world lands on a square target size pixels across.
std::array<std::uint32_t, 2> pixel_of(const render::Scene3D& scene, const Float3& point,
                                      std::uint32_t size) {
    const auto apply = [](const Mat4& m, const std::array<float, 4>& v) {
        std::array<float, 4> out{};
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                out.at(row) += m.at((column * 4) + row) * v.at(column);
            }
        }
        return out;
    };
    const std::array<float, 4> clip =
        apply(scene.projection, apply(scene.camera, {point[0], point[1], point[2], 1.0F}));
    const float x = ((clip[0] / clip[3]) * 0.5F) + 0.5F;
    const float y = 0.5F - ((clip[1] / clip[3]) * 0.5F);
    return {static_cast<std::uint32_t>(x * static_cast<float>(size)),
            static_cast<std::uint32_t>(y * static_cast<float>(size))};
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
                     .direction = {-0.4F, -0.8F, -0.45F}},
                    {.type = render::LightType::Omni,
                     .color = {1.0F, 0.6F, 0.2F},
                     .energy = 2.0F,
                     .position = {1.2F, 0.8F, 1.0F},
                     .range = 3.0F}};
    return scene;
}

// A frame of a lit cube, a cut card and a glass pane drawn with spatial shaders over a plain
// floor, as the scene's renderer draws them.
render::Image draw_surfaces(render::RenderDevice& device, const render::Scene3D& scene) {
    auto lit = device.create_shader(compiled(surface()));
    REQUIRE_MESSAGE(lit.ok(), (lit.ok() ? "" : lit.error().message));
    auto see_through = device.create_shader(compiled(glass()));
    REQUIRE(see_through.ok());
    auto target = device.create_texture({.width = 64, .height = 64, .render_target = true});
    REQUIRE(target.ok());
    // A checker of two colors, the left half see-through, and bumps in rows.
    std::vector<std::uint8_t> checker;
    std::vector<std::uint8_t> bumps;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
            checker.insert(checker.end(), {static_cast<std::uint8_t>(dark ? 60 : 230),
                                           static_cast<std::uint8_t>(dark ? 90 : 210),
                                           static_cast<std::uint8_t>(dark ? 50 : 190),
                                           static_cast<std::uint8_t>(x < 4 ? 40 : 255)});
            const std::uint8_t tilt = y % 2 == 0 ? 90 : 165;
            bumps.insert(bumps.end(), {tilt, 128, 255, 255});
        }
    }
    auto pattern = device.create_texture({.width = 8, .height = 8}, checker);
    auto bumpy = device.create_texture({.width = 8, .height = 8}, bumps);
    REQUIRE(pattern.ok());
    REQUIRE(bumpy.ok());
    auto box = device.create_mesh(cube(), {});
    auto card = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0F), {});
    auto floor =
        device.create_mesh(quad({0, -0.5F, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 6.0F), {});
    REQUIRE(box.ok());
    REQUIRE(card.ok());
    REQUIRE(floor.ok());

    render::Material3D material;
    material.shader = lit.value();
    material.shader_constants[0] = {0.0F, 0.0F, 0.0F, 0.0F}; // cut
    material.shader_constants[1] = {0.6F, 0.6F, 0.6F, 0.6F}; // metal
    material.shader_constants[2] = {1.0F, 0.7F, 0.6F, 1.0F}; // tint
    material.shader_constants[render::kShaderTimeConstant] = {2.0F, 0.0F, 0.0F, 0.0F};
    material.shader_textures[0] = {.texture = bumpy.value(), .repeat = true};
    material.shader_textures[1] = {.texture = pattern.value()};
    render::Material3D cutout = material;
    cutout.shader_constants[0] = {0.5F, 0.5F, 0.5F, 0.5F};
    cutout.transparency = render::Transparency::AlphaScissor;
    cutout.cull = render::CullMode::Disabled;
    render::Material3D pane;
    pane.shader = see_through.value();
    pane.transparency = render::Transparency::Alpha;

    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.05F, 0.05F, 0.1F, 1.0F}, .scene = scene})
                .ok());
    CHECK(device.draw_mesh(floor.value(), placed(0, 0, 0, 0), {.roughness = 0.8F}).ok());
    CHECK(device.draw_mesh(box.value(), placed(30, 0, 0, 0), material).ok());
    CHECK(device.draw_mesh(card.value(), placed(35, -0.6F, 0.4F, 1.2F), cutout).ok());
    CHECK(device.draw_mesh(card.value(), placed(-10, 0.7F, 0.1F, 1.3F), pane).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? "" : image.error().message));
    for (const render::MeshId mesh : {box.value(), card.value(), floor.value()}) {
        device.destroy_mesh(mesh);
    }
    for (const render::TextureId texture : {pattern.value(), bumpy.value(), target.value()}) {
        device.destroy_texture(texture);
    }
    device.destroy_shader(lit.value());
    device.destroy_shader(see_through.value());
    return image.value();
}

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

} // namespace

TEST_CASE("a spatial shader compiles into the mesh shader's fragment stages and the CPU") {
    const render::CompiledShader shader = compiled(glow());
    CHECK(shader.type == render::ShaderType::Spatial);
    // With no vertex(), the mesh shader's own vertex stage draws it, and with no
    // ALPHA_SCISSOR_THRESHOLD, the mesh shader's shadow stages draw its shadows.
    CHECK_FALSE(shader.vertex_used);
    CHECK_FALSE(shader.shadows_used());
    CHECK(shader.spirv_vertex.empty());
    CHECK(shader.msl_vertex.empty());
    CHECK(shader.glsl_vertex.empty());
    CHECK(shader.wgsl_vertex.empty());
    CHECK(shader.hlsl_vertex.empty());
    CHECK(shader.cpu_vertex.empty());
    CHECK(shader.motion_vertex == render::ShaderStage{});
    CHECK(shader.shadow_vertex == render::ShaderStage{});
    CHECK(shader.shadow == render::ShaderStage{});
    CHECK(shader.spirv_fragment.front() == 0x07230203U);
    CHECK(shader.msl_fragment.find("fs_main") != std::string::npos);
    // The GL device binds the mesh shader's textures and varyings by name.
    CHECK(shader.glsl_fragment.find("albedo_texture") != std::string::npos);
    CHECK(shader.glsl_fragment.find("metallic_texture") != std::string::npos);
    CHECK(shader.glsl_fragment.find("v_world") != std::string::npos);
    CHECK(shader.glsl_fragment.find("GameMaterial") != std::string::npos);
    // WGSL samplers sit 16 bindings after the textures, as the mesh shader's do, and its uniforms
    // are the fourth of group 3.
    CHECK(shader.wgsl_fragment.find("@group(2u) @binding(16u)") != std::string::npos);
    CHECK(shader.wgsl_fragment.find("@group(3u) @binding(3u)") != std::string::npos);
    CHECK(shader.hlsl.find("#line 1 \"shaders/glow.hlsl\"") != std::string::npos);
    CHECK(shader.hlsl.find("#define GAME_SHADER") != std::string::npos);
    for (const render::ShaderStage* stage :
         {&shader.clustered, &shader.normal_roughness, &shader.motion}) {
        CHECK_FALSE(stage->spirv.empty());
        CHECK_FALSE(stage->msl.empty());
        CHECK_FALSE(stage->wgsl.empty());
        CHECK_FALSE(stage->hlsl.empty());
    }
    CHECK(shader.clustered.hlsl.find("#define CLUSTERED") != std::string::npos);
    CHECK(shader.motion.hlsl.find("#define MOTION_VECTORS") != std::string::npos);
    CHECK(render::ShaderVm::load(shader.cpu_fragment).ok());
    CHECK(shader.normal_map_used);
    CHECK_FALSE(shader.alpha_used);
    CHECK_FALSE(shader.alpha_scissor_used);

    const render::CompiledShader cut = compiled(
        {.path = "cut.hlsl",
         .code = "void fragment() {\n    ALPHA = UV.x;\n    ALPHA_SCISSOR_THRESHOLD = 0.5;\n}\n",
         .type = render::ShaderType::Spatial,
         .params = {}});
    CHECK(cut.alpha_used);
    CHECK(cut.alpha_scissor_used);
    CHECK_FALSE(cut.normal_map_used);
    // It cuts its holes in its shadows too, so its shadow passes run it.
    CHECK(cut.shadows_used());
    CHECK_FALSE(cut.vertex_used);
    for (const render::ShaderStage* stage : {&cut.shadow_vertex, &cut.shadow}) {
        CHECK_FALSE(stage->spirv.empty());
        CHECK_FALSE(stage->msl.empty());
        CHECK_FALSE(stage->wgsl.empty());
        CHECK_FALSE(stage->hlsl.empty());
        CHECK_FALSE(stage->glsl.empty());
    }
    CHECK(cut.spirv_vertex.empty());
}

#if defined(_WIN32)
TEST_CASE("a spatial shader's stages compile with Windows' HLSL compiler") {
    for (const render::CompiledShader& shader : {compiled(surface()), compiled(hills())}) {
        for (const std::string* hlsl :
             {&shader.hlsl, &shader.clustered.hlsl, &shader.normal_roughness.hlsl,
              &shader.motion.hlsl, &shader.shadow.hlsl}) {
            const auto result = render::detail::compile_hlsl(*hlsl, "fs_main", "ps_5_1");
            CHECK_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
        }
    }
    const render::CompiledShader shader = compiled(hills());
    for (const std::string* hlsl :
         {&shader.hlsl_vertex, &shader.motion_vertex.hlsl, &shader.shadow_vertex.hlsl}) {
        const auto result = render::detail::compile_hlsl(*hlsl, "vs_main", "vs_5_1");
        CHECK_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
    }
}
#endif

TEST_CASE("spatial shader errors name the game's file and line") {
    render::ShaderSource source = glow();
    source.code += "\nvoid more() {\n    ALBEDO = missing;\n}\n";
    const std::string error = error_of(source);
    CAPTURE(error);
    CHECK(error.starts_with("shaders/glow.hlsl:11: "));
}

TEST_CASE("param names the code around a spatial shader takes are reserved, and the rest compile") {
    for (const char* file : {"mesh.hlsl", "tonemap.hlsl", "shadow.hlsl"}) {
        const std::vector<std::string> globals = globals_of(file);
        CHECK(globals.size() > 5);
        for (const std::string& name : globals) {
            CAPTURE(file);
            CAPTURE(name);
            CHECK(render::shader_name_reserved(name, render::ShaderType::Spatial));
        }
    }
    for (const char* name : {"world", "normal", "uv", "uv2", "flip", "game_surface", "texture",
                             "albedo_texture", "float4x4", "lerp"}) {
        CAPTURE(name);
        CHECK(render::shader_name_reserved(name, render::ShaderType::Spatial));
    }
    // Canvas shaders don't see the mesh shader.
    CHECK_FALSE(render::shader_name_reserved("albedo_texture"));
    for (const char* name : {"tint", "speed", "noise_map", "metal"}) {
        CAPTURE(name);
        CHECK_FALSE(render::shader_name_reserved(name, render::ShaderType::Spatial));
    }

    // Every other lowercase name in the code around a spatial shader compiles as a param.
    const shader_compiler::Wrapped wrapped = shader_compiler::wrap(
        {.path = "probe.hlsl", .code = "", .type = render::ShaderType::Spatial, .params = {}});
    std::set<std::string> free;
    for (const std::string* code : {&wrapped.gpu, &wrapped.cpu_fragment}) {
        std::string word;
        for (const char c : *code + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
                word += c;
                continue;
            }
            const bool lowercase = !word.empty() && std::ranges::all_of(word, [](char k) {
                return (k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '_';
            });
            if (lowercase && (word[0] < '0' || word[0] > '9') &&
                !render::shader_name_reserved(word, render::ShaderType::Spatial)) {
                free.insert(word);
            }
            word.clear();
        }
    }
    std::vector<std::string> names(free.begin(), free.end());
    for (std::size_t first = 0; first < names.size(); first += render::kShaderTimeConstant) {
        render::ShaderSource source{
            .path = "probe.hlsl", .code = {}, .type = render::ShaderType::Spatial, .params = {}};
        std::string sum = "0.0";
        for (std::size_t i = first; i < std::min(names.size(), first + render::kShaderTimeConstant);
             ++i) {
            source.params.push_back(
                {.name = names[i], .components = 1, .slot = static_cast<std::uint32_t>(i - first)});
            sum += " + " + names[i];
        }
        source.code = "void fragment() {\n    ALBEDO.r += (" + sum + ") * 0.0;\n}\n";
        CAPTURE(source.code);
        CHECK(error_of(source) == "(compiled)");
    }
}

TEST_CASE("devices draw spatial shaders only on meshes, and canvas shaders only in 2D") {
    const render::CompiledShader spatial = compiled(glass());
    const render::CompiledShader flat =
        compiled({.path = "flat.hlsl", .code = "void fragment() {\n}\n", .params = {}});
    for (const auto& device : slash::test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto mesh_shader = device->create_shader(spatial);
        auto sprite_shader = device->create_shader(flat);
        REQUIRE(mesh_shader.ok());
        REQUIRE(sprite_shader.ok());
        auto target = device->create_texture({.width = 8, .height = 8, .render_target = true});
        auto card = device->create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0F), {});
        REQUIRE(target.ok());
        REQUIRE(card.ok());
        REQUIRE(device
                    ->begin_frame({.target = target.value(),
                                   .clear = {},
                                   .scene = lit_scene(render::RenderMethod::ForwardPlus)})
                    .ok());
        render::Material3D material;
        material.shader = sprite_shader.value();
        auto drawn = device->draw_mesh(card.value(), placed(0, 0, 0, 0), material);
        REQUIRE_FALSE(drawn.ok());
        CHECK(drawn.error().message.find("isn't a spatial shader") != std::string::npos);
        render::Material sprite;
        sprite.shader = mesh_shader.value();
        sprite.program.constants.assign(render::cpu_shader::kConstants, {});
        const std::vector<render::Vertex2D> triangle{
            {.x = 0, .y = 0}, {.x = 8, .y = 0}, {.x = 0, .y = 8}};
        auto flat_drawn = device->draw(render::TextureId::None, triangle, sprite);
        REQUIRE_FALSE(flat_drawn.ok());
        CHECK(flat_drawn.error().message.find("no canvas shader") != std::string::npos);
        CHECK(device->end_frame().ok());
        device->destroy_mesh(card.value());
        device->destroy_texture(target.value());
        device->destroy_shader(mesh_shader.value());
        device->destroy_shader(sprite_shader.value());
    }
    // A spatial shader without a backend's Forward+ stages can't be made there.
    render::CompiledShader missing = spatial;
    missing.clustered = {};
    missing.cpu_fragment.clear();
    const auto software = render::create_software_device();
    CHECK_FALSE(software->create_shader(missing).ok());
}

TEST_CASE("every device draws spatial shaders' surfaces as the software device does") {
    render::Scene3D occluded = lit_scene(render::RenderMethod::ForwardPlus);
    occluded.ssao.enabled = true;
    occluded.use_taa = true;
    const std::array<std::pair<const char*, render::Scene3D>, 3> scenes{
        {{"forward+", lit_scene(render::RenderMethod::ForwardPlus)},
         {"forward+ with SSAO and TAA", occluded},
         {"compatibility", lit_scene(render::RenderMethod::Compatibility)}}};
    const auto software = render::create_software_device();
    std::vector<render::Image> expected;
    expected.reserve(scenes.size());
    for (const auto& [name, scene] : scenes) {
        expected.push_back(draw_surfaces(*software, scene));
    }
    // The shader's own surface shows: the cube isn't the plain white a material without one is.
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = ((std::size_t{y} * 64) + x) * 4;
        return std::array<int, 3>{expected[0].pixels[i], expected[0].pixels[i + 1],
                                  expected[0].pixels[i + 2]};
    };
    CHECK(at(32, 30) != at(4, 60));
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            CAPTURE(scenes.at(i).first);
            // The OpenGL devices draw only as Compatibility.
            const render::Image& want = opengl ? expected.back() : expected.at(i);
            const render::Image image = draw_surfaces(*device, scenes.at(i).second);
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

TEST_CASE("a spatial shader's vertex() compiles into the mesh shader's vertex stages and the CPU") {
    const render::CompiledShader shader = compiled(hills());
    CHECK(shader.vertex_used);
    CHECK(shader.shadows_used());
    CHECK(shader.spirv_vertex.front() == 0x07230203U);
    CHECK(shader.msl_vertex.find("vs_main") != std::string::npos);
    CHECK(shader.hlsl_vertex.find("#define GAME_VERTEX") != std::string::npos);
    CHECK(shader.hlsl_vertex.find("#line 1 \"shaders/hills.hlsl\"") != std::string::npos);
    // Its uniforms follow the camera's and the instance's, and the GL device binds its textures
    // by the names the fragment stage reads them by.
    CHECK(shader.glsl_vertex.find("GameMaterial") != std::string::npos);
    CHECK(shader.glsl_vertex.find("albedo_texture") != std::string::npos);
    CHECK(shader.glsl_vertex.find("v_world") != std::string::npos);
    // WGSL reads its uniforms third in group 1 and its textures in the fragment stage's group.
    CHECK(shader.wgsl_vertex.find("@group(1u) @binding(2u)") != std::string::npos);
    CHECK(shader.wgsl_vertex.find("@group(2u) @binding(0u)") != std::string::npos);
    CHECK(shader.motion_vertex.hlsl.find("#define MOTION_VECTORS") != std::string::npos);
    for (const render::ShaderStage* stage :
         {&shader.motion_vertex, &shader.shadow_vertex, &shader.shadow}) {
        CHECK_FALSE(stage->spirv.empty());
        CHECK_FALSE(stage->msl.empty());
        CHECK_FALSE(stage->wgsl.empty());
        CHECK_FALSE(stage->hlsl.empty());
    }
    CHECK_FALSE(shader.shadow_vertex.glsl.empty());
    CHECK_FALSE(shader.shadow.glsl.empty());
    CHECK(render::ShaderVm::load(shader.cpu_vertex).ok());
    // world_vertex_coords moves the corners in the world, so its code differs.
    render::ShaderSource local = swell();
    local.world_vertex_coords = false;
    CHECK(compiled(swell()).hlsl_vertex != compiled(local).hlsl_vertex);

    // Material files keep every stage.
    json::Value doc = parse(
        R"({"format": 1, "params": {"heights": {"type": "texture"}, "lift": {"type": "float"}},
            "shader": "shaders/hills.hlsl", "shader_type": "spatial"})");
    doc["compiled"] = canvas::compiled_shader_json(shader);
    CHECK(canvas::validate_material(doc).empty());
    auto read = canvas::parse_material(doc);
    REQUIRE_MESSAGE(read.ok(), (read.ok() ? "" : read.error().message));
    REQUIRE(read.value().compiled != nullptr);
    CHECK(*read.value().compiled == shader);
}

TEST_CASE("vertex() moves a mesh's corners, in its shadows too, which fragment() cuts") {
    const auto device = render::create_software_device();
    auto shader = device->create_shader(compiled(shifted()));
    REQUIRE_MESSAGE(shader.ok(), (shader.ok() ? "" : shader.error().message));
    constexpr std::uint32_t kSize = 64;
    auto target = device->create_texture({.width = kSize, .height = kSize, .render_target = true});
    auto ground = device->create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {});
    auto card = device->create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0F), {});
    REQUIRE(target.ok());
    REQUIRE(ground.ok());
    REQUIRE(card.ok());
    // Seen from above, lit from nearly above: the card casts without being drawn.
    render::Scene3D scene;
    scene.method = render::RenderMethod::Compatibility;
    scene.camera = render::look_at({0.8F, 6.0F, 0.01F}, {0.8F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.ambient = {0.2F, 0.2F, 0.2F};
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 1.0F, 1.0F},
                     .direction = {0.1F, -1.0F, 0.05F},
                     .shadow = true}};
    const auto frame = [&](float shift) {
        render::Material3D material;
        material.shader = shader.value();
        material.shader_constants[0] = {shift, shift, shift, shift};
        material.transparency = render::Transparency::AlphaScissor;
        material.cull = render::CullMode::Disabled;
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
        CHECK(device->draw_mesh(ground.value(), placed(0, 0, 0, 0), {.roughness = 1.0F}).ok());
        CHECK(device
                  ->draw_mesh(card.value(), placed(0, 0, 1, 0), material, 1,
                              render::ShadowCasting::ShadowsOnly)
                  .ok());
        REQUIRE(device->end_frame().ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        return std::move(image).value();
    };
    const auto light = [&](const render::Image& image, float x) {
        const auto [px, py] = pixel_of(scene, {x, 0.0F, 0.0F}, kSize);
        const std::size_t i = ((std::size_t{py} * kSize) + px) * 4;
        return int{image.pixels.at(i)} + image.pixels.at(i + 1) + image.pixels.at(i + 2);
    };
    // The card's left half shadows the ground under it; its right half is cut away.
    const render::Image still = frame(0.0F);
    CHECK(light(still, -0.25F) < light(still, 0.35F) - 100);
    CHECK(light(still, 1.8F) == light(still, 0.35F));
    // Moved 2 along x by vertex(), its shadow goes with it.
    const render::Image moved = frame(2.0F);
    CHECK(light(moved, 1.8F) < light(moved, -0.25F) - 100);
    CHECK(light(moved, -0.25F) == light(still, 0.35F));
    device->destroy_mesh(ground.value());
    device->destroy_mesh(card.value());
    device->destroy_texture(target.value());
    device->destroy_shader(shader.value());
}

// A frame of a sheet lifted into hills by vertex(), cut in a strip, and a box swelled in the
// world, over a plain floor and lit by a shadowed sun, as the scene's renderer draws them.
render::Image draw_hills(render::RenderDevice& device, const render::Scene3D& scene) {
    auto lifted = device.create_shader(compiled(hills()));
    REQUIRE_MESSAGE(lifted.ok(), (lifted.ok() ? "" : lifted.error().message));
    auto swelled = device.create_shader(compiled(swell()));
    REQUIRE_MESSAGE(swelled.ok(), (swelled.ok() ? "" : swelled.error().message));
    auto target = device.create_texture({.width = 64, .height = 64, .render_target = true});
    REQUIRE(target.ok());
    std::vector<std::uint8_t> rises;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto rise = static_cast<std::uint8_t>((x * 60) + (y * 20));
            rises.insert(rises.end(), {rise, rise, rise, 255});
        }
    }
    auto heights = device.create_texture({.width = 4, .height = 4}, rises);
    auto sheet = device.create_mesh(grid(8), {});
    auto box = device.create_mesh(cube(), {});
    auto floor =
        device.create_mesh(quad({0, -0.5F, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 6.0F), {});
    REQUIRE(heights.ok());
    REQUIRE(sheet.ok());
    REQUIRE(box.ok());
    REQUIRE(floor.ok());
    render::Material3D hilly;
    hilly.shader = lifted.value();
    hilly.shader_constants[0] = {0.6F, 0.6F, 0.6F, 0.6F}; // lift
    hilly.shader_constants[render::kShaderTimeConstant] = {2.0F, 1.9F, 0.0F, 0.0F};
    hilly.shader_textures[0] = {.texture = heights.value()};
    hilly.transparency = render::Transparency::AlphaScissor;
    hilly.cull = render::CullMode::Disabled;
    render::Material3D swollen;
    swollen.shader = swelled.value();
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.05F, 0.05F, 0.1F, 1.0F}, .scene = scene})
                .ok());
    CHECK(device.draw_mesh(floor.value(), placed(0, 0, 0, 0), {.roughness = 0.8F}).ok());
    CHECK(device.draw_mesh(sheet.value(), placed(20, -0.3F, 0.0F, 0.2F), hilly).ok());
    CHECK(device.draw_mesh(box.value(), placed(30, 0.9F, 0.2F, -0.6F), swollen).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), (image.ok() ? "" : image.error().message));
    for (const render::MeshId mesh : {sheet.value(), box.value(), floor.value()}) {
        device.destroy_mesh(mesh);
    }
    device.destroy_texture(heights.value());
    device.destroy_texture(target.value());
    device.destroy_shader(lifted.value());
    device.destroy_shader(swelled.value());
    return image.value();
}

TEST_CASE("every device draws spatial shaders' vertex() and shadows as the software device does") {
    render::Scene3D sunny = lit_scene(render::RenderMethod::ForwardPlus);
    sunny.lights.at(0).shadow = true;
    render::Scene3D smoothed = sunny;
    smoothed.ssao.enabled = true;
    smoothed.use_taa = true;
    render::Scene3D compatibility = sunny;
    compatibility.method = render::RenderMethod::Compatibility;
    const std::array<std::pair<const char*, render::Scene3D>, 3> scenes{
        {{"forward+", sunny},
         {"forward+ with SSAO and TAA", smoothed},
         {"compatibility", compatibility}}};
    const auto software = render::create_software_device();
    std::vector<render::Image> expected;
    expected.reserve(scenes.size());
    for (const auto& [name, scene] : scenes) {
        expected.push_back(draw_hills(*software, scene));
    }
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            CAPTURE(scenes.at(i).first);
            // The OpenGL devices draw only as Compatibility.
            const render::Image& want = opengl ? expected.back() : expected.at(i);
            const render::Image image = draw_hills(*device, scenes.at(i).second);
            const auto [count, first] = differences(image, want);
            CAPTURE(first);
            // SSAO over the shadows' edges rounds a few scattered pixels a level or three apart.
            CHECK(count <= 24);
            if (count > 24) {
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

TEST_CASE("a spatial material reads Godot's render modes, and its textures take slots from 0") {
    auto parsed = canvas::parse_material(parse(R"({
      "format": 1,
      "params": {
        "bumps": {"type": "texture", "repeat": true},
        "metal": {"type": "float", "value": 0.5},
        "pattern": {"type": "texture", "value": "pattern.png", "source_color": true},
        "tint": {"type": "color", "value": [1, 0.5, 0]}
      },
      "render_mode": ["blend_add", "cull_disabled", "unshaded", "fog_disabled",
                      "shadows_disabled", "world_vertex_coords"],
      "shader": "shaders/surface.hlsl",
      "shader_type": "spatial"
    })"));
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    const canvas::CanvasMaterial& material = parsed.value();
    CHECK(material.shader_type == render::ShaderType::Spatial);
    CHECK(material.modes.blend == render::BlendMode::Add);
    CHECK(material.modes.cull == render::CullMode::Disabled);
    CHECK(material.modes.unshaded);
    CHECK(material.modes.fog_disabled);
    CHECK(material.modes.shadows_disabled);
    CHECK(material.modes.world_vertex_coords);
    CHECK(material.params.at("bumps").slot == 0U);
    CHECK(material.params.at("pattern").slot == 1U);

    const render::ShaderSource source = canvas::shader_source(material, "code");
    CHECK(source.type == render::ShaderType::Spatial);
    CHECK(source.world_vertex_coords);
    REQUIRE(source.params.size() == 4);
    CHECK(source.params[2] ==
          render::ShaderSource::Param{
              .name = "pattern", .components = 0, .slot = 1, .source_color = true});

    // Colors are linear in the shader, as Godot's source_color makes them.
    auto instance = canvas::instantiate(material, {{"metal", json::Value(0.25)}}, 3.0);
    REQUIRE(instance.ok());
    CHECK(instance.value().shader_type == render::ShaderType::Spatial);
    CHECK(instance.value().modes.blend == render::BlendMode::Add);
    CHECK(instance.value().spatial_textures[0].repeat);
    CHECK(instance.value().spatial_textures[1].path == "pattern.png");
    const auto& constants = instance.value().program.constants;
    CHECK(constants[0][0] == 0.25F);
    CHECK(constants[1][0] == doctest::Approx(1.0F));
    CHECK(constants[1][1] == doctest::Approx(0.214041F).epsilon(1e-5));
    CHECK(constants[1][2] == 0.0F);
    CHECK(constants[render::kShaderTimeConstant][0] == 3.0F);
    // material.eval colors sprites; meshes take the 3D lights.
    const std::array<std::array<float, 2>, 1> uv{{{0.5F, 0.5F}}};
    auto evaluated = canvas::evaluate(
        instance.value(), "", render::Filter::Linear, {}, uv,
        [](const std::string&) -> slash::Result<const render::Image*> { return nullptr; });
    REQUIRE_FALSE(evaluated.ok());
    CHECK(evaluated.error().message.starts_with("a spatial material colors meshes"));
}

TEST_CASE("spatial material mistakes point at the bad value") {
    const std::string spatial = R"("shader": "shaders/a.hlsl", "shader_type": "spatial")";
    CHECK(first_error(R"({"format": 1, "shader_type": "spatial"})") ==
          "/shader_type: a spatial material needs a shader: an .hlsl file with a fragment() that "
          "gives the surface");
    CHECK(
        first_error(R"({"format": 1, "render_mode": ["unshaded"], "shader": "shaders/a.hlsl"})") ==
        "/render_mode: only spatial materials have render modes");
    CHECK(first_error(R"({"format": 1, "blend": "add", )" + spatial + "}") ==
          "/blend: spatial materials have no blend; use a blend_ render mode");
    CHECK(first_error(R"({"format": 1, "render_mode": ["blend_add", "blend_mul"], )" + spatial +
                      "}") == "/render_mode/1: a material has one blend_ mode");
    CHECK(first_error(R"({"format": 1, "render_mode": ["shiny"], )" + spatial + "}")
              .starts_with("/render_mode/0: "));
    CHECK(first_error(R"({"format": 1, "params": {"ramp": {"type": "texture",
          "source_color": true}}, "shader": "shaders/a.hlsl"})") ==
          "/params/ramp/source_color: only a spatial material's texture params have "
          "source_color; its color params are linear always");
    // The mesh shader's names are taken, as canvas.hlsl's are in canvas materials.
    CHECK(first_error(R"({"format": 1, "params": {"albedo_texture": {"type": "float"}}, )" +
                      spatial + "}")
              .starts_with("/params/albedo_texture: \"albedo_texture\" is taken"));
    std::string many = R"({"format": 1, "params": {)";
    for (int i = 0; i < 6; ++i) {
        many += (i == 0 ? "" : ", ") + std::string("\"t") + std::to_string(i) +
                R"(": {"type": "texture"})";
    }
    CHECK(first_error(many + "}, " + spatial + "}") ==
          "/params/t5: a spatial material samples at most 5 textures");
}

TEST_CASE("MeshInstance3D's material_override draws a spatial material or a StandardMaterial3D") {
    Scene scene;
    scene.files["shaders/paint.hlsl"] =
        "void fragment() {\n    ALBEDO = tint.rgb;\n    EMISSION = tint.rgb * glow;\n}\n";
    scene.files["materials/paint.material.json"] = R"({"format": 1, "params": {
        "glow": {"type": "float", "value": 0},
        "tint": {"type": "color", "value": [0.1, 0.9, 0.2]}},
        "shader": "shaders/paint.hlsl", "shader_type": "spatial"})";
    scene.files["materials/red.standard_material.json"] =
        R"({"format": 1, "albedo_color": [1, 0, 0, 1], "shading_mode": "unshaded"})";
    scene.files["materials/flat.material.json"] = R"({"format": 1, "color": [1, 0, 0, 1]})";
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("sun", R"({"Transform3D": {"rotation": [-0.6, 0.3, 0]},
                            "DirectionalLight3D": {}})");
    const entt::entity box = scene.create("box", R"({"Transform3D": {}, "MeshInstance3D": {},
        "BoxMesh": {}})");
    const auto center = [&](const canvas::Capture& captured) {
        const render::Image& image = captured.image;
        const std::size_t at =
            ((std::size_t{image.height / 2} * image.width) + (image.width / 2)) * 4;
        return std::array<int, 3>{image.pixels[at], image.pixels[at + 1], image.pixels[at + 2]};
    };
    const auto draw = [&] {
        auto captured = canvas::capture(scene.world, scene.assets, {.scale = 0.125F});
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        CHECK(captured.value().meshes == 1);
        return center(captured.value());
    };
    const std::array<int, 3> plain = draw();
    auto& instance = scene.world.ecs().get<world::MeshInstance3D>(box);
    instance.material_override = "materials/paint.material.json";
    const std::array<int, 3> painted = draw();
    CHECK(painted[1] > painted[0] + 60);
    CHECK(painted[1] > painted[2] + 60);
    CHECK(plain[0] == plain[1]);
    // The entity's params override the material's.
    instance.material_params = {{"tint", json::Value(json::Array{0.9, 0.1, 0.1})},
                                {"glow", json::Value(0.5)}};
    const std::array<int, 3> repainted = draw();
    CHECK(repainted[0] > repainted[1] + 60);
    CHECK(repainted[0] > painted[0]);
    instance.material_params.clear();
    instance.material_override = "materials/red.standard_material.json";
    CHECK(draw() == std::array<int, 3>{255, 0, 0});
    CHECK(scene3d::check_meshes(scene.world).empty());

    // A canvas material and a missing file are problems scene validate reports.
    instance.material_override = "materials/flat.material.json";
    std::vector<scene3d::Problem> problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/components/MeshInstance3D/material_override");
    CHECK(problems[0].message == "\"materials/flat.material.json\" is a canvas material; a mesh "
                                 "takes one with \"shader_type\": \"spatial\"");
    instance.material_override = "materials/gone.material.json";
    problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.starts_with("\"materials/gone.material.json\" could not be read"));
    instance.material_override = "materials/paint.png";
    problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.starts_with("expected a .material.json"));
}
