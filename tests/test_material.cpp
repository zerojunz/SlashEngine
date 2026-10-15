#include "slash/canvas/canvas.hpp"
#include "slash/canvas/material.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/image.hpp"
#include "slash/render/material.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;

namespace {

using Color = std::array<float, 4>;
using Uv = std::array<float, 2>;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// Each error in a material document, as "path: message".
std::vector<std::string> errors(std::string_view text) {
    std::vector<std::string> out;
    for (const json::ValidationError& error : canvas::validate_material(parse(text))) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

std::string first_error(std::string_view text) {
    const std::vector<std::string> all = errors(text);
    return all.empty() ? "(valid)" : all.front();
}

canvas::CanvasMaterial compile(std::string_view text) {
    auto material = canvas::parse_material(parse(text));
    REQUIRE_MESSAGE(material.ok(), (material.ok() ? "" : material.error().message));
    return std::move(material).value();
}

// Images by path, for evaluate.
struct Images {
    std::map<std::string, render::Image, std::less<>> images;

    canvas::ImageSource source() const {
        return [this](const std::string& path) -> slash::Result<const render::Image*> {
            const auto it = images.find(path);
            if (it == images.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return &it->second;
        };
    }
};

// An image one pixel high, one pixel per color.
render::Image row(const std::vector<std::array<std::uint8_t, 4>>& colors) {
    render::Image image;
    image.width = static_cast<std::uint32_t>(colors.size());
    image.height = 1;
    for (const auto& color : colors) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    return image;
}

// The colors a material draws at each uv.
std::vector<Color> colors(const canvas::CanvasMaterial& material, std::span<const Uv> uvs,
                          const std::map<std::string, json::Value>& params = {},
                          const canvas::PixelInputs& inputs = {}, double time = 0.0,
                          const std::string& texture = "", const Images& images = {}) {
    auto instance = canvas::instantiate(material, params, time);
    REQUIRE_MESSAGE(instance.ok(), (instance.ok() ? "" : instance.error().message));
    auto out = canvas::evaluate(instance.value(), texture, render::Filter::Nearest, inputs, uvs,
                                images.source());
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    return std::move(out).value();
}

Color color_at(const canvas::CanvasMaterial& material, Uv uv,
               const std::map<std::string, json::Value>& params = {},
               const canvas::PixelInputs& inputs = {}, double time = 0.0) {
    const std::array<Uv, 1> uvs{uv};
    return colors(material, uvs, params, inputs, time).front();
}

bool near(const Color& a, const Color& b) {
    constexpr float kTolerance = 0.002F;
    for (std::size_t i = 0; i < 4; ++i) {
        if (a.at(i) < b.at(i) - kTolerance || a.at(i) > b.at(i) + kTolerance) {
            return false;
        }
    }
    return true;
}

// The flash material from the materials docs: the texture times the vertex color, mixed toward
// a flash color.
constexpr std::string_view kFlash = R"({
  "color": {"op": "vec4", "args": [
    {"op": "mix", "args": ["base.rgb", "flash_color.rgb", "flash"]}, "base.a"]},
  "format": 1,
  "nodes": {
    "base": {"op": "mul", "args": [{"op": "texture", "args": ["texture", "uv"]}, "color"]}
  },
  "params": {
    "flash": {"type": "float", "value": 0, "min": 0, "max": 1},
    "flash_color": {"type": "color", "value": [1, 1, 1, 1]}
  }
})";

constexpr canvas::PixelInputs kRed{.color = {1.0F, 0.0F, 0.0F, 1.0F}, .frag_coord = {}};

// Ops of a given kind in a program.
std::size_t count(const render::MaterialProgram& program, render::Op op) {
    return static_cast<std::size_t>(
        std::ranges::count_if(program.code, [op](const render::Instruction& instruction) {
            return (instruction.head & 0xFFU) == static_cast<std::uint32_t>(op);
        }));
}

std::uint32_t destination(const render::Instruction& instruction) {
    return (instruction.head >> 8U) & 0xFFU;
}

} // namespace

TEST_CASE("a material file compiles its graph to a program the GPU runs") {
    const canvas::CanvasMaterial material = compile(kFlash);
    CHECK(material.blend == render::BlendMode::Mix);
    CHECK(material.program.code.size() == 5);
    CHECK(render::check_program(material.program).ok());
    CHECK_FALSE(material.time_constant.has_value());
    REQUIRE(material.params.size() == 2);
    CHECK(material.params.at("flash").max == 1.0);
    CHECK(material.params.at("flash").slot.has_value());
    CHECK(material.params.at("flash_color").value == Color{1.0F, 1.0F, 1.0F, 1.0F});

    CHECK(near(color_at(material, {0.5F, 0.5F}, {}, kRed), {1.0F, 0.0F, 0.0F, 1.0F}));
    CHECK(near(color_at(material, {0.5F, 0.5F}, {{"flash", 1}}, kRed), {1.0F, 1.0F, 1.0F, 1.0F}));
    CHECK(near(color_at(material, {0.5F, 0.5F}, {{"flash", 0.5}}, kRed), {1.0F, 0.5F, 0.5F, 1.0F}));
    CHECK(near(
        color_at(material, {0.5F, 0.5F}, {{"flash", 1}, {"flash_color", parse("[0, 0, 1]")}}, kRed),
        {0.0F, 0.0F, 1.0F, 1.0F}));
}

TEST_CASE("a material with no color draws the texture times the vertex color") {
    const canvas::CanvasMaterial material = compile(R"({"blend": "add", "format": 1})");
    CHECK(material.blend == render::BlendMode::Add);
    CHECK(material.program.code.empty());
    Images images;
    images.images.emplace("two.png", row({{255, 0, 0, 255}, {0, 0, 255, 255}}));
    const std::array<Uv, 2> uvs{{{0.25F, 0.5F}, {0.75F, 0.5F}}};
    const canvas::PixelInputs half{.color = {1.0F, 1.0F, 1.0F, 0.5F}, .frag_coord = {}};
    const auto out = colors(material, uvs, {}, half, 0.0, "two.png", images);
    CHECK(near(out[0], {1.0F, 0.0F, 0.0F, 0.5F}));
    CHECK(near(out[1], {0.0F, 0.0F, 1.0F, 0.5F}));
    // Every blend mode has Godot's name.
    for (const char* blend : {"mix", "add", "sub", "mul", "premul_alpha"}) {
        CHECK(first_error(R"({"format": 1, "blend": ")" + std::string(blend) + "\"}") == "(valid)");
    }
}

TEST_CASE("a material says how 2D lights shade it, as Godot's light_mode") {
    CHECK(compile(R"({"format": 1})").light_mode == render::LightMode::Normal);
    CHECK(compile(R"({"format": 1, "light_mode": "unshaded"})").light_mode ==
          render::LightMode::Unshaded);
    const canvas::CanvasMaterial glow = compile(R"({"format": 1, "light_mode": "light_only"})");
    CHECK(glow.light_mode == render::LightMode::LightOnly);
    auto instance = canvas::instantiate(glow, {}, 0.0);
    REQUIRE(instance.ok());
    CHECK(instance.value().light_mode == render::LightMode::LightOnly);
    CHECK(first_error(R"({"format": 1, "light_mode": "lit"})").starts_with("/light_mode: "));
}

TEST_CASE("material errors point at the bad value and suggest the closest name") {
    CHECK(errors(R"({
      "color": "tint.rgb",
      "format": 1,
      "nodes": {
        "a": {"op": "mull", "args": ["tint", 2]},
        "b": {"op": "add", "args": ["tnit", "uv.z"]},
        "c": {"op": "add", "args": ["d", 1]},
        "d": {"op": "mul", "args": ["c", 2]},
        "e": {"op": "add", "args": ["uv", "tint.rgb"]},
        "f": {"op": "mix", "args": ["uv", "uv"]}
      },
      "params": {"tint": {"type": "color", "value": [1, 0.5, 0.5]}, "uv": {"type": "float"}}
    })") == std::vector<std::string>{
                "/params/uv: \"uv\" is an input; name the param something else",
                "/nodes/a/op: no op \"mull\"; did you mean \"mul\"?",
                "/nodes/b/args/0: no node, param or input \"tnit\"; did you mean \"tint\"?",
                "/nodes/b/args/1: \"uv\" is a vec2, so it has no z",
                "/nodes/d/args/0: \"c\" depends on itself: c -> d -> c",
                "/nodes/e: add takes args of one size, or floats with them: got vec2, vec3",
                "/nodes/f/args: mix takes 3 args (a, b, t), not 2",
                "/color: color is a vec3; make a vec4, as {\"op\": \"vec4\", \"args\": [rgb, 1]}",
            });

    // The schema's own errors come first, and stop the graph from compiling.
    CHECK(first_error(R"({"format": 1, "colour": "color"})") ==
          "/colour: unknown property \"colour\"; did you mean \"color\"?");
    CHECK(first_error(R"({"color": "color"})") == ": missing required property \"format\"");
    CHECK(first_error(R"({"format": 1, "blend": "multiply"})").starts_with("/blend: "));

    // Names.
    CHECK(first_error(R"({"format": 1, "params": {"Tint": {"type": "float"}}})") ==
          "/params/Tint: names use lowercase letters, digits and _, and don't start with a digit");
    CHECK(first_error(R"({"format": 1, "nodes": {"2x": {"op": "abs", "args": [1]}}})") ==
          "/nodes/2x: names use lowercase letters, digits and _, and don't start with a digit");
    CHECK(first_error(R"({"format": 1, "nodes": {"time": {"op": "abs", "args": [1]}}})") ==
          "/nodes/time: \"time\" is an input; name the node something else");
    CHECK(first_error(R"({"format": 1, "params": {"k": {"type": "float"}},
                          "nodes": {"k": {"op": "abs", "args": [1]}}})") ==
          "/nodes/k: \"k\" is also a param; names must differ");

    // Components.
    CHECK(first_error(R"({"format": 1, "color": "time.x"})") ==
          "/color: \"time\" is a float; it has no components to pick");
    CHECK(first_error(R"({"format": 1, "color": "color.xr"})") ==
          "/color: pick 1 to 4 components, all from xyzw or all from rgba, as \"color.xy\"");
    CHECK(first_error(R"({"format": 1, "color": "texture.r"})") ==
          "/color: \"texture\" is a texture; it has no components to pick");

    // Args.
    CHECK(first_error(R"({"format": 1, "color": {"op": "mul", "args": ["texture", 2]}})") ==
          "/color: mul can't take a texture; sample it with the texture op first");
    CHECK(first_error(R"({"format": 1, "color": {"op": "texture", "args": ["uv", "uv"]}})") ==
          "/color/args/0: the texture op's first arg is a texture: \"texture\" or a texture "
          "param, not a vec2");
    CHECK(first_error(R"({"format": 1, "color": {"op": "texture", "args": ["texture", 1]}})") ==
          "/color/args/1: the texture op's second arg is a vec2 uv, not a float");
    CHECK(first_error(R"({"format": 1, "color": {"op": "vec4", "args": ["uv", 1]}})") ==
          "/color/args: vec4 takes floats and vectors with 4 components in all, or one float; "
          "these have 3");
    CHECK(first_error(R"({"format": 1, "color": {"op": "vec4", "args": [
                          {"op": "dot", "args": [1, "uv"]}, 0, 0, 1]}})") ==
          "/color/args/0: dot takes two args of one size: got a float and a vec2");
    CHECK(first_error(R"({"format": 1, "color": {"op": "vec4", "args": [
                          {"op": "rgb_to_hsv", "args": ["color"]}, 1]}})") ==
          "/color/args/0/args/0: rgb_to_hsv takes a vec3, not a vec4; pick .rgb or .xyz");
    CHECK(first_error(R"({"format": 1, "color": {"op": "atan", "args": [1, 2, 3]}})") ==
          "/color/args: atan takes 1 or 2 args (y_over_x, or y, x), not 3");
    CHECK(first_error(R"({"format": 1, "color": {"op": "abs"}})") ==
          "/color/args: abs takes 1 arg (a), not 0");

    // Params.
    CHECK(first_error(R"({"format": 1, "params": {"k": {"type": "float", "repeat": true}}})") ==
          "/params/k/repeat: only texture params have repeat");
    CHECK(first_error(R"({"format": 1, "params": {"t": {"type": "texture", "min": 0}}})") ==
          "/params/t/min: texture params have no min");
    CHECK(first_error(R"({"format": 1, "params": {"k": {"type": "float", "min": 2,
                                                         "max": 1}}})") ==
          "/params/k/max: max is below min");
    CHECK(first_error(R"({"format": 1, "params": {"k": {"type": "float", "value": 2, "min": 0,
                                                         "max": 1}}})") ==
          "/params/k/value: 2 is outside the param's range, from 0 to 1");
    CHECK(first_error(R"({"format": 1, "params": {"k": {"type": "vec2", "value": [1, 2, 3]}}})") ==
          "/params/k/value: expected an array of 2 numbers");
    CHECK(first_error(R"({"format": 1, "params": {"t": {"type": "texture",
                                                         "value": "../up.png"}}})") ==
          "/params/t/value: expected a path inside the project folder, separated by '/', with no "
          "\".\" or \"..\" parts");
    CHECK(first_error(R"({"format": 1, "params": {
                          "a": {"type": "texture"}, "b": {"type": "texture"},
                          "c": {"type": "texture"}, "d": {"type": "texture"}}})") ==
          "/params/d: a material samples at most 3 textures of its own, besides the sprite's");

    // parse_material reports the first error.
    auto parsed = canvas::parse_material(parse(R"({"format": 1, "color": "colr"})"));
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().message ==
          "/color: no node, param or input \"colr\"; did you mean \"color\"?");
}

namespace {

// A material whose nodes are named n0, n1 and so on, each made by node(i) from the one before,
// with the last as the color.
std::string chain(int count, const std::function<std::string(int)>& node,
                  std::string_view color = "") {
    std::string nodes;
    for (int i = 0; i < count; ++i) {
        nodes += (i == 0 ? "" : ", ") + ("\"n" + std::to_string(i) + "\": ") + node(i);
    }
    const std::string last = "\"n" + std::to_string(count - 1) + "\"";
    return R"({"format": 1, "nodes": {)" + nodes +
           "}, \"color\": " + (color.empty() ? last : std::string(color)) + "}";
}

std::string previous(int i) {
    return "\"n" + std::to_string(i - 1) + "\"";
}

} // namespace

TEST_CASE("a material's graph fits the shader's limits or says what to change") {
    // 17 vec4s of four instructions each.
    CHECK(first_error(chain(17,
                            [](int i) {
                                const std::string from = i == 0 ? "\"uv\"" : previous(i);
                                return R"({"op": "vec4", "args": [)" +
                                       from.substr(0, from.size() - 1) + R"(.x", )" +
                                       from.substr(0, from.size() - 1) + R"(.y", 0, 1]})";
                            })) ==
          "/color: the graph makes 68 instructions; a material runs at most 64. Name values "
          "used more than once, so they are computed once");

    // 33 different numbers, and a 0.5.
    CHECK(first_error(chain(
              33,
              [](int i) {
                  return R"({"op": "add", "args": [)" + (i == 0 ? "0.5" : previous(i)) + ", " +
                         std::to_string(i + 1) + "]}";
              },
              R"({"op": "vec4", "args": ["n32"]})")) ==
          "/color: the graph uses 34 constants and params; a material holds at most 32");

    // 29 values alive at once: each add's sine waits for the adds after it.
    CHECK(first_error(chain(
              28,
              [](int i) {
                  const std::string next = i == 27 ? R"({"op": "sin", "args": ["uv.x"]})"
                                                   : "\"n" + std::to_string(i + 1) + "\"";
                  return R"({"op": "add", "args": [{"op": "sin", "args": ["uv.x"]}, )" + next +
                         "]}";
              },
              R"({"op": "vec4", "args": ["n0"]})")) ==
          "/color: the graph needs more than 28 values at once; compute fewer things side by "
          "side");

    // Too many nodes to run, or nodes nested too deep to read, fail before anything else. Nodes
    // are counted in name order, so n9 comes last.
    CHECK(first_error(chain(65,
                            [](int i) {
                                return R"({"op": "sin", "args": [)" +
                                       (i == 0 ? "\"uv.x\"" : previous(i)) + "]}";
                            })) ==
          "/nodes/n9: a material has at most 64 nodes, since each runs at least one "
          "instruction");
    std::string nested = R"("uv.x")";
    for (int i = 0; i < 17; ++i) {
        nested = R"({"op": "sin", "args": [)" + nested + "]}";
    }
    CHECK(first_error(R"({"format": 1, "color": {"op": "vec4", "args": [)" + nested + "]}}") ==
          "/color/args/0/args/0/args/0/args/0/args/0/args/0/args/0/args/0/args/0/args/0/args/0/"
          "args/0/args/0/args/0/args/0/args/0: nodes nest at most 16 deep; name some in \"nodes\" "
          "and use their names as args");
}

TEST_CASE("a material's graph reuses registers, constants and named nodes") {
    // A long chain keeps one value at a time, so it reuses one register.
    const canvas::CanvasMaterial long_chain = compile(chain(
        40,
        [](int i) {
            return R"({"op": "sin", "args": [)" + (i == 0 ? "\"uv.x\"" : previous(i)) + "]}";
        },
        R"({"op": "vec4", "args": ["n39"]})"));
    CHECK(long_chain.program.code.size() == 41);
    for (const render::Instruction& instruction : long_chain.program.code) {
        CHECK((destination(instruction) == 0 || destination(instruction) == 4));
    }

    // Equal numbers share a constant; a param keeps its own, since sprites change it.
    const canvas::CanvasMaterial shared = compile(R"({
      "color": {"op": "vec4", "args": [
        {"op": "add", "args": [{"op": "mul", "args": ["uv.x", 2]}, 2]}, "k", 2, 2]},
      "format": 1,
      "params": {"k": {"type": "float", "value": 2}}
    })");
    CHECK(shared.program.constants.size() == 2);
    CHECK(near(color_at(shared, {0.25F, 0.0F}, {{"k", 0.5}}), {1.0F, 0.5F, 1.0F, 1.0F}));

    // A named node is computed once, however many args read it.
    const canvas::CanvasMaterial named = compile(R"({
      "color": {"op": "vec4", "args": ["s", "s", {"op": "one_minus", "args": ["s"]}, 1]},
      "format": 1,
      "nodes": {"s": {"op": "sin", "args": ["uv.x"]}}
    })");
    CHECK(count(named.program, render::Op::Sin) == 1);
    CHECK(near(color_at(named, {0.5F, 0.0F}), {0.479F, 0.479F, 0.521F, 1.0F}));

    // The color can read the texture coordinate after writing its first components.
    const canvas::CanvasMaterial swapped =
        compile(R"({"format": 1, "color": {"op": "vec4", "args": ["uv.y", "uv.x", 0, 1]}})");
    CHECK(near(color_at(swapped, {0.25F, 0.75F}), {0.75F, 0.25F, 0.0F, 1.0F}));
    const canvas::CanvasMaterial picked = compile(R"({"format": 1, "color": "color.bgra"})");
    CHECK(near(color_at(picked, {0.0F, 0.0F}, {}, kRed), {0.0F, 0.0F, 1.0F, 1.0F}));
}

TEST_CASE("instantiate fills in a sprite's params and the time") {
    const canvas::CanvasMaterial flash = compile(kFlash);
    const std::uint32_t slot = *flash.params.at("flash").slot;
    auto instance = canvas::instantiate(flash, {{"flash", 0.25}}, 0.0);
    REQUIRE(instance.ok());
    CHECK(instance.value().program.constants.at(slot) == Color{0.25F, 0.25F, 0.25F, 0.25F});

    std::string failed;
    auto unknown = canvas::instantiate(flash, {{"flsh", 1}}, 0.0, &failed);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().message == "the material has no param \"flsh\"; did you mean \"flash\"?");
    CHECK(failed == "flsh");
    auto outside = canvas::instantiate(flash, {{"flash", 2}}, 0.0, &failed);
    REQUIRE_FALSE(outside.ok());
    CHECK(outside.error().message == "2 is outside the param's range, from 0 to 1");
    CHECK(failed == "flash");
    auto wrong = canvas::instantiate(flash, {{"flash_color", "red"}}, 0.0);
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message == "expected [r, g, b] or [r, g, b, a]");
    CHECK(canvas::instantiate(flash, {{"flash", 1}}, 0.0, &failed).ok());
    CHECK(failed.empty());

    // Time wraps every hour, as Godot's TIME, and never runs backward past 0.
    const canvas::CanvasMaterial clock = compile(
        R"({"format": 1, "color": {"op": "vec4", "args": [
                {"op": "vec3", "args": [{"op": "fract", "args": ["time"]}]}, 1]}})");
    REQUIRE(clock.time_constant.has_value());
    CHECK(near(color_at(clock, {}, {}, {}, 2.25), {0.25F, 0.25F, 0.25F, 1.0F}));
    CHECK(near(color_at(clock, {}, {}, {}, canvas::kMaterialTimeWrap + 0.5),
               {0.5F, 0.5F, 0.5F, 1.0F}));
    CHECK(near(color_at(clock, {}, {}, {}, -3.0), {0.0F, 0.0F, 0.0F, 1.0F}));

    // Texture params name their files; a sprite can swap them.
    const canvas::CanvasMaterial overlay = compile(R"({
      "color": {"op": "texture", "args": ["noise", "uv"]},
      "format": 1,
      "params": {"noise": {"type": "texture", "value": "noise.png", "repeat": true,
                           "filter": "linear"}}
    })");
    instance = canvas::instantiate(overlay, {}, 0.0);
    REQUIRE(instance.ok());
    CHECK(instance.value().textures[0].path == "noise.png");
    CHECK(instance.value().textures[0].repeat);
    CHECK(instance.value().textures[0].filter == "linear");
    instance = canvas::instantiate(overlay, {{"noise", "other/noise.png"}}, 0.0);
    REQUIRE(instance.ok());
    CHECK(instance.value().textures[0].path == "other/noise.png");
    auto escaping = canvas::instantiate(overlay, {{"noise", "/etc/passwd"}}, 0.0);
    REQUIRE_FALSE(escaping.ok());
    CHECK(escaping.error().message.starts_with("expected a path inside"));
    auto number = canvas::instantiate(overlay, {{"noise", 1}}, 0.0);
    REQUIRE_FALSE(number.ok());
    CHECK(number.error().message == "expected a texture path, or \"\" for white");
}

TEST_CASE("read_param_value reads each param type") {
    canvas::MaterialParam param;
    param.type = "vec3";
    REQUIRE(canvas::read_param_value(parse("[1, 2, 3]"), param).ok());
    CHECK(param.value == Color{1.0F, 2.0F, 3.0F, 0.0F});
    CHECK(canvas::read_param_value(parse("2"), param).error().message ==
          "expected an array of 3 numbers");
    param.type = "color";
    REQUIRE(canvas::read_param_value(parse("[0, 0.5, 1]"), param).ok());
    CHECK(param.value == Color{0.0F, 0.5F, 1.0F, 1.0F});
    param.type = "float";
    CHECK(canvas::read_param_value(parse("[1]"), param).error().message ==
          "expected a number, got array");
    param.min = 0.0;
    CHECK(canvas::read_param_value(parse("-0.5"), param).error().message ==
          "-0.5 is outside the param's range, at least 0");
    param.type = "vec2";
    param.min.reset();
    param.max = 10.0;
    CHECK(canvas::read_param_value(parse("[1, 11]"), param).error().message ==
          "11 is outside the param's range, at most 10");
}

TEST_CASE("evaluate samples textures and reads inputs as the GPU does") {
    Images images;
    images.images.emplace("stripes.png", row({{255, 0, 0, 255}, {0, 0, 255, 255}}));
    images.images.emplace("four.png",
                          row({{0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}}));
    const std::array<Uv, 2> uvs{{{0.6F, 0.5F}, {0.9F, 0.5F}}};

    // Twice across: a repeating texture tiles, a clamped one stretches its edge.
    constexpr std::string_view kTiled = R"({
      "color": {"op": "texture", "args": ["stripes", {"op": "mul", "args": ["uv", 2]}]},
      "format": 1,
      "params": {"stripes": {"type": "texture", "value": "stripes.png", "repeat": REPEAT}}
    })";
    std::string tiled(kTiled);
    tiled.replace(tiled.find("REPEAT"), 6, "true");
    auto out = colors(compile(tiled), uvs, {}, {}, 0.0, "", images);
    CHECK(near(out[0], {1.0F, 0.0F, 0.0F, 1.0F}));
    CHECK(near(out[1], {0.0F, 0.0F, 1.0F, 1.0F}));
    std::string clamped(kTiled);
    clamped.replace(clamped.find("REPEAT"), 6, "false");
    out = colors(compile(clamped), uvs, {}, {}, 0.0, "", images);
    CHECK(near(out[0], {0.0F, 0.0F, 1.0F, 1.0F}));
    CHECK(near(out[1], {0.0F, 0.0F, 1.0F, 1.0F}));

    // A texture param left at "" samples white, as Godot's hint_default_white.
    out = colors(compile(tiled), uvs, {{"stripes", ""}}, {}, 0.0, "", images);
    CHECK(near(out[0], {1.0F, 1.0F, 1.0F, 1.0F}));

    // A missing file fails the whole evaluation.
    auto instance = canvas::instantiate(compile(tiled), {{"stripes", "gone.png"}}, 0.0);
    REQUIRE(instance.ok());
    CHECK_FALSE(
        canvas::evaluate(instance.value(), "", render::Filter::Nearest, {}, uvs, images.source())
            .ok());

    // texture_pixel_size is one over the sprite texture's size; fragcoord is the pixel's
    // position in the target.
    const canvas::CanvasMaterial inputs = compile(R"({
      "color": {"op": "vec4", "args": ["texture_pixel_size", {"op": "div",
                                                               "args": ["fragcoord", 100]}]},
      "format": 1
    })");
    const canvas::PixelInputs at{.color = {1.0F, 1.0F, 1.0F, 1.0F}, .frag_coord = {50.0F, 25.0F}};
    out = colors(inputs, uvs, {}, at, 0.0, "four.png", images);
    CHECK(near(out[0], {0.25F, 1.0F, 0.5F, 0.25F}));

    // Hue turned a third of the way round: red becomes green.
    const canvas::CanvasMaterial hue = compile(R"({
      "color": {"op": "vec4", "args": [{"op": "hsv_to_rgb", "args": [
        {"op": "add", "args": [{"op": "rgb_to_hsv", "args": ["color.rgb"]},
                               [0.3333333, 0, 0]]}]}, 1]},
      "format": 1
    })");
    CHECK(near(color_at(hue, {}, {}, kRed), {0.0F, 1.0F, 0.0F, 1.0F}));
}

TEST_CASE("material time follows the frame between ticks") {
    CHECK(canvas::material_time(0, 0.5, 1.0) == 0.0);
    CHECK(canvas::material_time(1, 0.5, 0.0) == 0.0);
    CHECK(canvas::material_time(1, 0.5, 1.0) == 0.5);
    CHECK(canvas::material_time(10, 0.5, 0.5) == 4.75);
    CHECK(canvas::material_time(10, 0.5, 7.0) == 5.0);
}

TEST_CASE("the published material schema lists the ops") {
    const json::Value schema = canvas::material_schema();
    const json::Value* ops = schema.find("$defs")->find("node")->find("properties")->find("op");
    REQUIRE(ops->find("enum") != nullptr);
    CHECK(ops->find("enum")->size() == 40);
}
