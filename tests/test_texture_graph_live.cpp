#include "devices.hpp"

#include "slash/canvas/material.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/shader.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace slash;
using test::drawing_devices;

namespace {

using Files = std::map<std::string, std::string, std::less<>>;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

world::FileReader read_from(const Files& files) {
    return [&files](std::string_view path) -> Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return make_error(ErrorCode::NotFound, "no file " + std::string(path));
        }
        return it->second;
    };
}

// Lava: noise that flows with time, warped, through a ramp, under a crust of bricks whose
// mortar and tint the material moves.
constexpr std::string_view kLava = R"({
  "format": 1,
  "type": "graph",
  "width": 32,
  "height": 24,
  "tiling": true,
  "parameters": {"time": 0, "mortar": 0.1, "glow": [1, 0.45, 0.1, 1]},
  "nodes": {
    "flow": {"op": "noise", "noise": {"frequency": 0.08, "fractal_type": "fbm",
                                      "fractal_octaves": 3, "offset": ["=time * 3", 0, 0]}},
    "swirl": {"op": "noise", "noise": {"seed": 4, "frequency": 0.1}},
    "warped": {"op": "warp", "input": "flow", "by": "swirl", "strength": 0.05},
    "hot": {"op": "gradient_map", "input": "warped", "gradient": {"points": [
      {"offset": 0, "color": [0.2, 0, 0, 1]}, {"offset": 1, "color": "$glow"}]}},
    "crust": {"op": "bricks", "across": 2, "down": 3, "mortar": "$mortar"},
    "lava": {"op": "blend", "base": "hot", "layer": "crust", "mode": "multiply", "opacity": 0.5}
  },
  "outputs": [{"name": "albedo", "node": "lava"}]
})";

// The graph drawn live with a device's shader over a target of its size, each pixel at a
// texel's center, with the material's constants as given.
render::Image draw_live(render::RenderDevice& device, render::ShaderId shader,
                        const std::vector<std::pair<std::size_t, std::array<float, 4>>>& constants,
                        float time) {
    auto target = device.create_texture(
        {.width = 32, .height = 24, .filter = render::Filter::Nearest, .render_target = true});
    const std::vector<std::uint8_t> white(4, 255);
    auto texture = device.create_texture(
        {.width = 1, .height = 1, .filter = render::Filter::Nearest, .render_target = false},
        white);
    REQUIRE(target.ok());
    REQUIRE(texture.ok());
    render::Material material;
    material.shader = shader;
    material.program.constants.assign(render::cpu_shader::kConstants, {});
    for (const auto& [slot, value] : constants) {
        material.program.constants.at(slot) = value;
    }
    material.program.constants[render::kShaderTimeConstant] = {time, 0.0F, 0.0F, 0.0F};
    const std::array<std::uint8_t, 4> rgba{255, 255, 255, 255};
    const std::vector<render::Vertex2D> quad{
        {.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F, .rgba = rgba},
        {.x = 32.0F, .y = 0.0F, .u = 1.0F, .v = 0.0F, .rgba = rgba},
        {.x = 0.0F, .y = 24.0F, .u = 0.0F, .v = 1.0F, .rgba = rgba},
        {.x = 32.0F, .y = 0.0F, .u = 1.0F, .v = 0.0F, .rgba = rgba},
        {.x = 32.0F, .y = 24.0F, .u = 1.0F, .v = 1.0F, .rgba = rgba},
        {.x = 0.0F, .y = 24.0F, .u = 0.0F, .v = 1.0F, .rgba = rgba}};
    REQUIRE(device
                .begin_frame({.target = target.value(),
                              .clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
                              .view = {}})
                .ok());
    REQUIRE(device.draw(texture.value(), quad, material).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_texture(target.value());
    device.destroy_texture(texture.value());
    return std::move(image).value();
}

// "" when every channel of every pixel is within 2 of the baked one, else the first that isn't.
std::string matches(const render::Image& got, const render::Image& want) {
    REQUIRE(got.width == want.width);
    REQUIRE(got.height == want.height);
    for (std::size_t i = 0; i < got.pixels.size(); ++i) {
        if (std::abs(int{got.pixels[i]} - int{want.pixels[i]}) > 2) {
            const std::size_t pixel = i / 4;
            return "(" + std::to_string(pixel % got.width) + ", " +
                   std::to_string(pixel / got.width) + ") channel " + std::to_string(i % 4) +
                   " is " + std::to_string(got.pixels[i]) + ", baked " +
                   std::to_string(want.pixels[i]);
        }
    }
    return "";
}

render::Image baked(const Files& files, std::string_view path, const json::Object& overrides) {
    const procedural::GraphReader reader{.text = read_from(files), .image = {}};
    auto compiled =
        procedural::compile_texture_graph(parse(files.at(std::string(path))), reader, overrides);
    REQUIRE_MESSAGE(compiled.ok(), (compiled.ok() ? "" : compiled.error().message));
    auto images = procedural::bake_texture_graph(compiled.value(), nullptr);
    REQUIRE(images.ok());
    return images.value().front();
}

std::string live_error(std::string_view graph, render::ShaderType type) {
    const procedural::GraphReader reader{.text = {}, .image = {}};
    auto made = procedural::texture_graph_shader(parse(graph), reader, type);
    return made.ok() ? "(made)" : made.error().message;
}

} // namespace

TEST_CASE("live texture graphs draw their baked texels, moved by the material's params") {
    const Files files{{"lava.texture.json", std::string(kLava)}};
    auto material = canvas::parse_material(parse(R"({
      "format": 1,
      "params": {
        "glow": {"type": "color", "value": [1, 0.45, 0.1, 1]},
        "mortar": {"type": "float", "value": 0.1}
      },
      "shader": "lava.texture.json"
    })"));
    REQUIRE_MESSAGE(material.ok(), (material.ok() ? "" : material.error().message));
    auto compiled = canvas::compile_material_shader(material.value(), read_from(files));
    REQUIRE_MESSAGE(compiled.ok(), (compiled.ok() ? "" : compiled.error().message));
    REQUIRE(compiled.value() != nullptr);

    // Params take slots in name order: glow, then mortar.
    const std::array<float, 4> glow{1.0F, 0.45F, 0.1F, 1.0F};
    const std::array<float, 4> red{0.9F, 0.1F, 0.05F, 1.0F};
    const render::Image at_rest = baked(files, "lava.texture.json", {});
    const render::Image moved = baked(files, "lava.texture.json",
                                      {{"time", json::Value(2.5)},
                                       {"mortar", json::Value(0.25)},
                                       {"glow", json::Value(json::Array{0.9, 0.1, 0.05, 1.0})}});
    CHECK(matches(moved, at_rest) != "");
    for (const auto& device : drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto shader = device->create_shader(*compiled.value());
        REQUIRE_MESSAGE(shader.ok(), (shader.ok() ? "" : shader.error().message));
        const render::Image still =
            draw_live(*device, shader.value(), {{0, glow}, {1, {0.1F, 0.0F, 0.0F, 0.0F}}}, 0.0F);
        CHECK_MESSAGE(matches(still, at_rest).empty(), matches(still, at_rest));
        const render::Image flowing =
            draw_live(*device, shader.value(), {{0, red}, {1, {0.25F, 0.0F, 0.0F, 0.0F}}}, 2.5F);
        CHECK_MESSAGE(matches(flowing, moved).empty(), matches(flowing, moved));
        device->destroy_shader(shader.value());
    }
}

TEST_CASE("live texture graphs give spatial shaders their outputs by name") {
    const std::string graph = R"({
      "format": 1, "type": "graph", "width": 16, "height": 16,
      "nodes": {
        "height": {"op": "bricks", "across": 2, "down": 4},
        "color": {"op": "gradient_map", "input": "height"},
        "normal": {"op": "normal_from_height", "height": "height", "strength": 2}
      },
      "outputs": [{"name": "albedo", "node": "color"}, {"name": "normal", "node": "normal"},
                  {"name": "height", "node": "height", "as": "gray"}]
    })";
    const procedural::GraphReader reader{.text = {}, .image = {}};
    auto code = procedural::texture_graph_shader(parse(graph), reader, render::ShaderType::Spatial);
    REQUIRE_MESSAGE(code.ok(), (code.ok() ? "" : code.error().message));
    CHECK(code.value().find("ALBEDO = tg_linear") != std::string::npos);
    CHECK(code.value().find("NORMAL_MAP = c.rgb") != std::string::npos);
    // Height has no use in a spatial shader, so only two outputs are drawn.
    CHECK(code.value().find("tg_output_2") == std::string::npos);
    auto compiled = render::compile_shader({.path = "wall.texture.json",
                                            .code = code.value(),
                                            .type = render::ShaderType::Spatial,
                                            .params = {},
                                            .world_vertex_coords = false});
    REQUIRE_MESSAGE(compiled.ok(), (compiled.ok() ? "" : compiled.error().message));
    CHECK(compiled.value()->normal_map_used);
}

TEST_CASE("live texture graphs refuse what can't work out as they draw") {
    // Image files.
    CHECK(live_error(R"({"format": 1, "type": "graph", "width": 8, "height": 8,
      "nodes": {"photo": {"op": "image", "path": "photo.png"}},
      "outputs": [{"name": "albedo", "node": "photo"}]})",
                     render::ShaderType::Canvas) ==
          "a live graph can't read image files; bake it, or make the image from nodes");
    // A parameter that moves a setting by its square.
    CHECK(live_error(R"({"format": 1, "type": "graph", "width": 8, "height": 8,
      "parameters": {"time": 1},
      "nodes": {"n": {"op": "noise", "noise": {"frequency": "=0.01 + time * time"}}},
      "outputs": [{"name": "albedo", "node": "n"}]})",
                     render::ShaderType::Canvas) ==
          "parameter \"time\" moves a setting of a \"noise\" node other than in a straight line, "
          "so it can't move as the material draws");
    // Too many node pixels a pixel: two wide blurs.
    CHECK(live_error(R"({"format": 1, "type": "graph", "width": 8, "height": 8,
      "nodes": {"n": {"op": "noise"}, "soft": {"op": "blur", "input": "n", "radius": 30}},
      "outputs": [{"name": "albedo", "node": "soft"}]})",
                     render::ShaderType::Canvas)
              .find("node pixels for each pixel drawn, more than 2048") != std::string::npos);
    // Spatial outputs it can't use, and compositor effects.
    CHECK(live_error(R"({"format": 1, "type": "graph", "width": 8, "height": 8,
      "nodes": {"n": {"op": "noise"}}, "outputs": [{"name": "mask", "node": "n"}]})",
                     render::ShaderType::Spatial)
              .starts_with("a spatial material's graph needs an output named albedo"));
    CHECK(live_error(R"({"format": 1, "type": "graph", "width": 8, "height": 8,
      "nodes": {"n": {"op": "noise"}}, "outputs": [{"name": "albedo", "node": "n"}]})",
                     render::ShaderType::Compositor)
              .starts_with("a texture graph draws canvas_item and spatial materials"));
}
