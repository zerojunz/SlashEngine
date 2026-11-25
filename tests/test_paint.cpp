#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/paint.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/image.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace procedural = slash::procedural;
namespace render = slash::render;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(std::string root) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = std::move(root);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }

    json::Value ok(std::string_view name, std::string_view params) {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

// Two squares side by side facing +z, each its own part and half of the texture.
constexpr std::string_view kPanels = R"(# two panels
mtllib panels.mtl
o left
usemtl wood
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 0.5 0
vt 0.5 1
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1 4/4/1
o right
usemtl iron
v 1 0 0
v 2 0 0
v 2 1 0
v 1 1 0
vt 0.5 0
vt 1 0
vt 1 1
vt 0.5 1
f 5/5/1 6/6/1 7/7/1 8/8/1
)";

std::string make_project(const std::string& root) {
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/props").ok());
    REQUIRE(slash::platform::write_file(root + "/props/panels.obj", kPanels).ok());
    REQUIRE(
        slash::platform::write_file(root + "/props/panels.mtl",
                                    "newmtl wood\nKd 0.6 0.4 0.2\nnewmtl iron\nKd 0.3 0.3 0.3\n")
            .ok());
    return root;
}

// A surface of width x height texels, every one covered, the left half part 0 and the right
// part 1, places across a 2 x 1 rectangle facing +z.
procedural::PaintSurface flat_surface(std::uint32_t width, std::uint32_t height) {
    procedural::PaintSurface s;
    s.width = width;
    s.height = height;
    const std::size_t texels = std::size_t{width} * height;
    s.covered.assign(texels, 1);
    s.position.resize(texels * 3);
    s.normal.assign(texels * 3, 0.0F);
    s.part.resize(texels);
    s.material.assign(texels, 0);
    for (std::size_t i = 0; i < texels; ++i) {
        const auto x = static_cast<float>(i % width);
        const auto y = static_cast<float>(i / width);
        s.position[i * 3] = (x + 0.5F) / static_cast<float>(width) * 2.0F;
        s.position[(i * 3) + 1] = 1.0F - ((y + 0.5F) / static_cast<float>(height));
        s.normal[(i * 3) + 2] = 1.0F;
        s.part[i] = x < static_cast<float>(width) / 2.0F ? 0 : 1;
    }
    s.max = {2.0F, 1.0F, 0.0F};
    s.part_names = {"left", "right"};
    s.material_names = {"wood"};
    // Ambient occlusion falling off to the right.
    std::vector<float> ao(texels);
    for (std::size_t i = 0; i < texels; ++i) {
        ao[i] = 1.0F - (static_cast<float>(i % width) / static_cast<float>(width - 1));
    }
    s.maps.emplace("ao", std::move(ao));
    return s;
}

// Bakes a stack over a surface with the CPU twin: each output by name.
std::vector<std::pair<std::string, render::Image>> bake(const json::Value& stack,
                                                        const procedural::PaintSurface& surface,
                                                        const json::Object& overrides = {}) {
    auto resolved = procedural::resolve_paint(stack, {});
    REQUIRE_MESSAGE(resolved.ok(), (resolved.ok() ? std::string() : resolved.error().message));
    auto compiled = procedural::compile_paint(resolved.value(), surface, {}, overrides);
    REQUIRE_MESSAGE(compiled.ok(), (compiled.ok() ? std::string() : compiled.error().message));
    auto images = procedural::bake_texture_graph(compiled.value().program, nullptr);
    REQUIRE(images.ok());
    std::vector<std::pair<std::string, render::Image>> out;
    std::vector<std::string> names = compiled.value().outputs;
    for (const std::string& layer : compiled.value().mask_layers) {
        names.push_back("mask." + layer);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        out.emplace_back(names[i], std::move(images.value()[i]));
    }
    return out;
}

const render::Image& output(const std::vector<std::pair<std::string, render::Image>>& baked,
                            std::string_view name) {
    for (const auto& [n, image] : baked) {
        if (n == name) {
            return image;
        }
    }
    FAIL("no output ", name);
    return baked.front().second;
}

const std::uint8_t* texel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    return &image.pixels[((std::size_t{y} * image.width) + x) * 4];
}

} // namespace

TEST_CASE("paint stacks are checked before anything bakes") {
    const auto errors_of = [](std::string_view text) {
        return procedural::validate_paint(parse(text));
    };
    CHECK(errors_of(R"({"format": 1, "type": "paint", "model": "a.obj",
        "layers": [{"name": "base", "fill": {"roughness": 0.5}}]})")
              .empty());
    // A layer named twice.
    auto errors = errors_of(R"({"format": 1, "type": "paint", "layers": [
        {"name": "a", "fill": {"roughness": 0.5}}, {"name": "a", "fill": {"metallic": 1}}]})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/layers/1/name");
    // A model and a sprite; a sprite's size.
    CHECK(errors_of(R"({"format": 1, "type": "paint", "model": "a.obj", "sprite": "a.png"})")
              .front()
              .path == "/sprite");
    CHECK(errors_of(R"({"format": 1, "type": "paint", "sprite": "a.png", "size": 64})")
              .front()
              .path == "/size");
    // A model's points have three numbers, a sprite's two.
    errors = errors_of(R"({"format": 1, "type": "paint", "model": "a.obj", "layers": [
        {"name": "s", "fill": {"roughness": 1}, "strokes": [{"points": [[0, 0]], "size": 1}]}]})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/layers/0/strokes/0/points/0");
    // A base starts from a map that fits it.
    CHECK(errors_of(R"({"format": 1, "type": "paint", "base": {"roughness": "sprite"}})")
              .front()
              .path == "/base/roughness");
    // An unknown map is the schema's.
    CHECK_FALSE(errors_of(R"({"format": 1, "type": "paint", "layers": [
        {"name": "a", "fill": {"roughness": 1}, "mask": {"map": "curvy"}}]})")
                    .empty());
}

TEST_CASE("a paint stack extends another, layers merging by name") {
    const json::Value stack = parse(R"({"format": 1, "type": "paint",
        "extends": "builtin:paint/worn_painted_metal", "model": "a.obj",
        "parameters": {"wear": 0.9},
        "layers": [{"name": "dirt", "enabled": false},
                   {"name": "decal", "fill": {"albedo": [1, 1, 1]}}]})");
    auto resolved = procedural::resolve_paint(stack, {});
    REQUIRE(resolved.ok());
    const json::Value& out = resolved.value();
    CHECK(out.find("extends") == nullptr);
    CHECK(out.find("model")->as_string() == "a.obj");
    // The parameters merge; the paint color stays the smart material's.
    CHECK(out.find("parameters")->find("wear")->as_double() == 0.9);
    CHECK(out.find("parameters")->find("paint") != nullptr);
    const json::Array& layers = out.find("layers")->as_array();
    REQUIRE(layers.size() == 5);
    CHECK(layers[3].find("name")->as_string() == "dirt");
    CHECK_FALSE(layers[3].find("enabled")->as_bool());
    CHECK(layers[3].find("mask") != nullptr); // changed in place, the rest kept
    CHECK(layers[4].find("name")->as_string() == "decal");
    // The maps it reads: the curvature its wear masks with, and the model's own ambient
    // occlusion under everything; with no high model, no normal map.
    const std::vector<std::string> needs = procedural::paint_needs(out);
    CHECK(std::ranges::find(needs, "curvature") != needs.end());
    CHECK(std::ranges::find(needs, "ao") != needs.end());
    CHECK(std::ranges::find(needs, "normal") == needs.end());
    // Every smart material the engine ships is a valid stack.
    for (const std::string_view name : procedural::builtin_paint_names()) {
        const auto text = procedural::builtin_paint("builtin:paint/" + std::string(name));
        REQUIRE(text.has_value());
        CHECK_MESSAGE(procedural::validate_paint(parse(*text)).empty(), name);
    }
    // An unknown one names the nearest.
    auto missing = procedural::resolve_paint(
        parse(R"({"format": 1, "type": "paint", "extends": "builtin:paint/rusty_irn"})"), {});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("rusty_iron") != std::string::npos);
}

TEST_CASE("paint layers blend through masks of parts, maps and strokes") {
    const procedural::PaintSurface surface = flat_surface(16, 8);
    const json::Value stack = parse(R"({"format": 1, "type": "paint",
        "parameters": {"edge": 0.5},
        "base": {"roughness": 0.25},
        "layers": [
          {"name": "wood", "fill": {"albedo": [0.2, 0.2, 0.2], "roughness": 0.8}},
          {"name": "iron", "fill": {"albedo": [1, 0, 0], "metallic": 1}, "mask": {"part": "right"}},
          {"name": "dark", "fill": {"albedo": [0, 0, 1]}, "channels": ["albedo"],
           "mask": {"map": "ao", "below": "$edge", "softness": 0.01},
           "blend": "mix", "opacity": 0.5},
          {"name": "dab", "fill": {"emission": [0, 1, 0]},
           "strokes": [{"points": [[0.5, 0.5, 0]], "size": 0.3, "hardness": 1}]}
        ]})");
    const auto baked = bake(stack, surface);
    const render::Image& albedo = output(baked, "albedo");
    const render::Image& orm = output(baked, "orm");
    // Left: wood. Right: iron, then half blue where the occlusion is under 0.5.
    CHECK(texel(albedo, 1, 4)[0] == 51);
    CHECK(texel(albedo, 1, 4)[2] == 51);
    CHECK(std::abs(texel(albedo, 9, 4)[0] - 128) <= 1); // red, half covered by blue
    CHECK(std::abs(texel(albedo, 9, 4)[2] - 128) <= 1);
    CHECK(texel(albedo, 1, 4)[3] == 255);
    // orm: the model's own occlusion (1 - 1/15 at x = 1), the wood's roughness everywhere,
    // metal on the right.
    CHECK(texel(orm, 1, 4)[0] == 238);
    CHECK(texel(orm, 1, 4)[1] == 204);
    CHECK(texel(orm, 1, 4)[2] == 0);
    CHECK(texel(orm, 12, 4)[2] == 255);
    // The masks' coverage: half the panels, then where ao < 0.5.
    const json::Value iron = procedural::measure_paint(output(baked, "mask.iron"), surface);
    CHECK(iron.find("r")->as_double() == doctest::Approx(0.5));
    const json::Value dark = procedural::measure_paint(output(baked, "mask.dark"), surface);
    CHECK(dark.find("r")->as_double() == doctest::Approx(0.5).epsilon(0.1));
    // The stroke's dab: a disc of radius 0.3 round (0.5, 0.5) on the 2 x 1 panels.
    const render::Image& emission = output(baked, "emission");
    CHECK(texel(emission, 4, 4)[1] == 255);
    CHECK(texel(emission, 12, 4)[1] == 0);
    const json::Value dab = procedural::measure_paint(output(baked, "mask.dab"), surface);
    CHECK(dab.find("r")->as_double() == doctest::Approx(3.14159 * 0.09 / 2).epsilon(0.35));
    // A parameter moved: the ao threshold at 1 covers the whole right half.
    json::Object moved;
    moved.emplace("edge", json::Value(1.01));
    const auto again = bake(stack, surface, moved);
    CHECK(procedural::measure_paint(output(again, "mask.dark"), surface).find("r")->as_double() >
          0.95);
    // An unknown part names the nearest, pointing into the stack.
    auto resolved = procedural::resolve_paint(parse(R"({"format": 1, "type": "paint", "layers": [
            {"name": "a", "fill": {"roughness": 1}, "mask": {"part": "rigth"}}]})"),
                                              {});
    REQUIRE(resolved.ok());
    auto refused = procedural::compile_paint(resolved.value(), surface, {});
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.starts_with("/layers/0/mask/part"));
    CHECK(refused.error().message.find("\"right\"") != std::string::npos);
}

TEST_CASE("a sprite's paint stack wears its outline and keeps its alpha") {
    // A disc 12 pixels across on a 16 x 16 sprite.
    render::Image disc;
    disc.width = 16;
    disc.height = 16;
    disc.pixels.assign(16 * 16 * 4, 0);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - 8.0F;
            const float dy = static_cast<float>(y) + 0.5F - 8.0F;
            std::uint8_t* px = &disc.pixels[((std::size_t{y} * 16) + x) * 4];
            px[0] = 200;
            px[1] = 100;
            px[2] = 50;
            px[3] = (dx * dx) + (dy * dy) < 36.0F ? 255 : 0;
        }
    }
    auto surface = procedural::paint_sprite_surface(disc, nullptr, 2.0F);
    REQUIRE(surface.ok());
    const json::Value stack = parse(R"({"format": 1, "type": "paint", "sprite": "disc.png",
        "layers": [{"name": "rim", "fill": {"albedo": [1, 1, 1]},
                    "mask": {"map": "curvature", "above": 0.8, "softness": 0.01}}]})");
    const auto baked = bake(stack, surface.value());
    const render::Image& albedo = output(baked, "albedo");
    // The center keeps the sprite's color, the edge turns white, outside stays clear.
    CHECK(texel(albedo, 8, 8)[0] == 200);
    CHECK(texel(albedo, 8, 2)[0] == 255);
    CHECK(texel(albedo, 8, 2)[3] == 255);
    CHECK(texel(albedo, 0, 0)[3] == 0);
    const double rim = procedural::measure_paint(output(baked, "mask.rim"), surface.value())
                           .find("r")
                           ->as_double();
    CHECK(rim > 0.2);
    CHECK(rim < 0.8);
}

TEST_CASE("paint.edit, paint.bake and paint.preview paint a model") {
    const std::string root = make_project("command_paint_project");
    Engine engine(root);

    // A new stack, its layers added by name.
    const json::Value made = engine.ok("paint.edit", R"({"path": "props/panels.paint.json",
        "patch": {"model": "props/panels.obj", "size": 32, "maps": {"rays": 2}},
        "layers": {"base": {"fill": {"albedo": [0.2, 0.2, 0.2]}},
                   "iron": {"fill": {"albedo": [1, 0, 0], "metallic": 1},
                            "mask": {"part": "right"}}}})");
    CHECK(*made.find("layers") == parse(R"(["base", "iron"])"));
    CHECK(made.find("masks")->find("iron")->as_double() == doctest::Approx(0.5).epsilon(0.05));
    CHECK(made.find("outputs")->find("albedo")->find("r")->as_double() ==
          doctest::Approx(0.6).epsilon(0.05));
    CHECK(std::filesystem::exists(root + "/props/panels.paint.json"));

    // A stroke appended to a new layer, placed below iron.
    const json::Value stroked = engine.ok("paint.edit", R"({"path": "props/panels.paint.json",
        "layers": {"dab": {"fill": {"albedo": [0, 1, 0]}, "below": "iron",
                           "add_strokes": [{"points": [[0.5, 0.5, 0]], "size": 0.25,
                                            "hardness": 1}]}}})");
    CHECK(*stroked.find("layers") == parse(R"(["base", "dab", "iron"])"));
    CHECK(*stroked.find("changed") == parse(R"(["/layers/dab"])"));
    CHECK(stroked.find("masks")->find("dab")->as_double() ==
          doctest::Approx(3.14159 * 0.0625 / 2).epsilon(0.3));
    // A part that isn't there is refused, nothing written.
    const command::Reply refused = engine.call("paint.edit", R"({"path": "props/panels.paint.json",
        "layers": {"iron": {"mask": {"part": "rihgt"}}}})");
    CHECK_FALSE(refused.ok);
    CHECK(refused.message.find("\"right\"") != std::string::npos);

    // The bake writes textures beside the stack and a material that names them.
    const json::Value baked = engine.ok("paint.bake", R"({"path": "props/panels.paint.json"})");
    CHECK(baked.find("material")->as_string() == "props/panels.standard_material.json");
    CHECK(baked.find("textures")->find("albedo")->as_string() == "props/panels_albedo.png");
    CHECK(baked.find("texel_density")->as_double() == doctest::Approx(22.6).epsilon(0.05));
    CHECK(std::filesystem::exists(root + "/props/panels_orm.png"));
    auto material = slash::platform::read_file(root + "/props/panels.standard_material.json");
    REQUIRE(material.ok());
    const json::Value doc = parse(material.value());
    CHECK(doc.find("metallic_texture_channel")->as_string() == "blue");
    CHECK(doc.find("normal_texture")->as_string() == "props/panels_normal.png");
    // The same bake again changes nothing; undo takes the files away.
    CHECK(engine.ok("paint.bake", R"({"path": "props/panels.paint.json"})")
              .find("unchanged")
              ->as_bool());
    engine.ok("undo", "{}");
    CHECK_FALSE(std::filesystem::exists(root + "/props/panels_orm.png"));

    // texture.check holds it to its expect.
    engine.ok("paint.edit", R"({"path": "props/panels.paint.json", "measure": false,
        "patch": {"expect": {"mask.iron": [0.45, 0.55], "albedo.r": [0.9, 1]}}})");
    const json::Value checked =
        engine.ok("texture.check", R"({"paths": ["props/panels.paint.json"]})");
    CHECK(checked.find("checked")->as_int() == 1);
    REQUIRE(checked.find("errors")->size() == 1);
    CHECK(checked.find("errors")->as_array()[0].find("path")->as_string() == "/expect/albedo.r");

    // A preview draws it from two sides.
    const json::Value preview = engine.ok("paint.preview", R"({"path": "props/panels.paint.json",
        "views": 2, "size": [32, 32], "out": "previews/panels.png"})");
    CHECK(preview.find("views")->size() == 2);
    CHECK(*preview.find("image_size") == parse("[64, 32]"));
    CHECK(std::filesystem::exists(root + "/previews/panels.png"));
    std::filesystem::remove_all(root);
}

TEST_CASE("a model's stack starts from a smart material and reads its mesh maps") {
    const std::string root = make_project("command_paint_smart_project");
    Engine engine(root);
    REQUIRE(slash::platform::write_file(root + "/props/worn.paint.json", R"({"format": 1,
        "type": "paint", "extends": "builtin:paint/worn_painted_metal",
        "model": "props/panels.obj", "size": 32, "maps": {"rays": 2}})")
                .ok());
    const json::Value baked = engine.ok("paint.bake", R"({"path": "props/worn.paint.json"})");
    CHECK(baked.find("masks")->find("worn") != nullptr);
    CHECK(baked.find("masks")->find("dirt") != nullptr);
    // The mesh maps baked beside the model, as model.bake_maps would.
    CHECK(std::filesystem::exists(root + "/props/panels.maps/maps.json"));
    CHECK(engine.ok("model.bake_maps", R"({"path": "props/panels.obj", "size": 32, "rays": 2})")
              .find("cached")
              ->as_bool());
    std::filesystem::remove_all(root);
}

namespace {

// The flat surface, its left part turned to face up.
procedural::PaintSurface split_surface(std::uint32_t width, std::uint32_t height) {
    procedural::PaintSurface s = flat_surface(width, height);
    for (std::size_t i = 0; i < s.covered.size(); ++i) {
        if (s.part[i] == 0) {
            s.normal[(i * 3) + 1] = 1.0F;
            s.normal[(i * 3) + 2] = 0.0F;
        }
    }
    // Open sky everywhere.
    s.maps.emplace("sky", std::vector<float>(s.covered.size(), 1.0F));
    // Edges along the top and bottom rows.
    std::vector<float> curvature(s.covered.size(), 0.5F);
    for (std::size_t i = 0; i < curvature.size(); ++i) {
        const std::size_t y = i / width;
        if (y == 0 || y + 1 == height) {
            curvature[i] = 1.0F;
        }
    }
    s.maps.emplace("curvature", std::move(curvature));
    return s;
}

// The mean red of the columns [x0, x1), 0 to 1.
double share(const render::Image& image, std::uint32_t x0, std::uint32_t x1) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            sum += texel(image, x, y)[0];
        }
    }
    return sum / (255.0 * image.height * (x1 - x0));
}

} // namespace

TEST_CASE("paint masks read which way faces turn, streaks, points and groups") {
    procedural::PaintSurface surface = split_surface(32, 16);
    surface.points.emplace("hinge_l", std::array<float, 3>{1.5F, 0.5F, 0.0F});
    const json::Value stack = parse(R"({"format": 1, "type": "paint", "layers": [
        {"name": "up", "fill": {"roughness": 1}, "mask": {"map": "facing", "above": 0.75}},
        {"name": "side", "fill": {"roughness": 1},
         "mask": {"map": "facing", "direction": [0, 0, 1], "above": 0.75}},
        {"name": "runs", "fill": {"roughness": 1},
         "mask": {"map": "streaks", "above": 0.4, "softness": 0.01}},
        {"name": "dot", "fill": {"roughness": 1}, "mask": {"point": [0.5, 0.5, 0], "radius": 0.3}},
        {"name": "hinge", "fill": {"roughness": 1}, "mask": {"point": "hinge_l", "radius": 0.3}},
        {"name": "either", "fill": {"roughness": 1},
         "mask": [{"part": "left"},
                  {"group": [{"map": "ao", "below": 0.5, "softness": 0.01}], "op": "add"}]}
        ]})");
    const auto baked = bake(stack, surface);
    // facing: the left part faces up, the right faces +z.
    CHECK(share(output(baked, "mask.up"), 0, 16) == doctest::Approx(1.0));
    CHECK(share(output(baked, "mask.up"), 16, 32) == doctest::Approx(0.0));
    CHECK(share(output(baked, "mask.side"), 16, 32) == doctest::Approx(1.0));
    // streaks run down the faces that stand, never across the ones that face up.
    CHECK(share(output(baked, "mask.runs"), 0, 16) == doctest::Approx(0.0));
    const double runs = share(output(baked, "mask.runs"), 16, 32);
    CHECK(runs > 0.05);
    CHECK(runs < 0.95);
    // A point: 1 at the center, 0 past its radius; by name, at the model's node.
    CHECK(texel(output(baked, "mask.dot"), 8, 8)[0] > 200);
    CHECK(texel(output(baked, "mask.dot"), 24, 8)[0] == 0);
    CHECK(texel(output(baked, "mask.hinge"), 24, 8)[0] > 200);
    CHECK(texel(output(baked, "mask.hinge"), 8, 8)[0] == 0);
    // A group joins as one term: the left part, plus where the occlusion is low.
    CHECK(share(output(baked, "mask.either"), 0, 32) > 0.95);
    // A point that isn't there names the nearest.
    auto resolved = procedural::resolve_paint(parse(R"({"format": 1, "type": "paint", "layers": [
            {"name": "a", "fill": {"roughness": 1}, "mask": {"point": "hinge_r"}}]})"),
                                              {});
    REQUIRE(resolved.ok());
    auto refused = procedural::compile_paint(resolved.value(), surface, {});
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.starts_with("/layers/0/mask/point"));
    CHECK(refused.error().message.find("\"hinge_l\"") != std::string::npos);
}

TEST_CASE("a weather layer ages a surface by one number") {
    const procedural::PaintSurface surface = split_surface(32, 16);
    const auto stack_aged = [](double age) {
        return parse(R"({"format": 1, "type": "paint", "layers": [
            {"name": "base", "fill": {"albedo": [0.8, 0.2, 0.2], "roughness": 0.4}},
            {"name": "weather", "weather": {"age": )" +
                     std::to_string(age) + R"(, "rain": 1, "dust": 1, "rust": 1}}]})");
    };
    REQUIRE(procedural::validate_paint(stack_aged(0.5)).empty());
    const auto old = bake(stack_aged(1.0), surface);
    // Dust settles on the faces turned up, rain streaks down the faces that stand.
    CHECK(share(output(old, "mask.weather.dust"), 0, 16) >
          share(output(old, "mask.weather.dust"), 16, 32) + 0.3);
    CHECK(share(output(old, "mask.weather.rain"), 16, 32) >
          share(output(old, "mask.weather.rain"), 0, 16));
    // New, nothing shows; older, more.
    const auto young = bake(stack_aged(0.0), surface);
    const render::Image& fresh = output(young, "albedo");
    CHECK(texel(fresh, 4, 4)[0] == 204);
    CHECK(texel(fresh, 20, 4)[0] == 204);
    CHECK(share(output(old, "albedo"), 0, 32) < share(fresh, 0, 32) - 0.05);
    // Its own mask keeps it to the right part.
    const json::Value masked = parse(R"({"format": 1, "type": "paint", "layers": [
        {"name": "base", "fill": {"albedo": [0.8, 0.2, 0.2]}},
        {"name": "w", "weather": {"age": 1, "dust": 1, "rain": 0},
         "mask": {"part": "right"}}]})");
    const auto right = bake(masked, surface);
    CHECK(share(output(right, "mask.w.dust"), 0, 16) == doctest::Approx(0.0));
    // A weather layer paints by its own rules: no fill.
    const auto errors = procedural::validate_paint(parse(R"({"format": 1, "type": "paint",
        "layers": [{"name": "w", "weather": {}, "fill": {"roughness": 1}}]})"));
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().path == "/layers/0/fill");
}

TEST_CASE("a stack's seed and variants draw the same on every run") {
    const procedural::PaintSurface surface = flat_surface(16, 8);
    const auto stack_seeded = [](int seed) {
        return parse(R"({"format": 1, "type": "paint", "seed": )" + std::to_string(seed) +
                     R"(, "layers": [{"name": "spots", "fill": {"roughness": 1},
                         "mask": {"map": "noise", "scale": 8, "above": 0.5}}]})");
    };
    const auto a = bake(stack_seeded(0), surface);
    const auto b = bake(stack_seeded(0), surface);
    const auto c = bake(stack_seeded(5), surface);
    CHECK(output(a, "mask.spots").pixels == output(b, "mask.spots").pixels);
    CHECK(output(a, "mask.spots").pixels != output(c, "mask.spots").pixels);

    const json::Value stack = parse(R"({"format": 1, "type": "paint",
        "parameters": {"wear": 0.5, "paint": [1, 0, 0, 1]},
        "variants": {"count": 3, "seed": 9,
                     "ranges": {"wear": [0.2, 0.8], "paint": [[1, 0, 0], [0, 0, 1]]}},
        "layers": [{"name": "base", "fill": {"albedo": "$paint", "roughness": "$wear"}}]})");
    REQUIRE(procedural::validate_paint(stack).empty());
    auto resolved = procedural::resolve_paint(stack, {});
    REQUIRE(resolved.ok());
    CHECK(procedural::paint_variant_count(resolved.value()) == 3);
    const json::Value one = procedural::paint_variant(resolved.value(), 1);
    const json::Value two = procedural::paint_variant(resolved.value(), 2);
    CHECK(one == procedural::paint_variant(resolved.value(), 1));
    CHECK(one.find("variants") == nullptr);
    CHECK(*one.find("seed") != *two.find("seed"));
    const double wear = one.find("parameters")->find("wear")->as_double();
    CHECK(wear >= 0.2);
    CHECK(wear <= 0.8);
    CHECK(one.find("parameters")->find("wear")->as_double() !=
          two.find("parameters")->find("wear")->as_double());
    const json::Array& paint = one.find("parameters")->find("paint")->as_array();
    CHECK(paint.size() == 3); // as long as its ends
    CHECK(paint[0].as_double() + paint[2].as_double() == doctest::Approx(1.0).epsilon(0.01));
    // A range names a parameter the stack has.
    const auto errors = procedural::validate_paint(parse(R"({"format": 1, "type": "paint",
        "parameters": {"wear": 0.5}, "variants": {"count": 2, "ranges": {"waer": [0, 1]}}})"));
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/variants/ranges/waer");
}

TEST_CASE("paint.bake writes variants, and text stamps paint words") {
    const std::string root = make_project("command_paint_flavours_project");
    Engine engine(root);
    engine.ok("paint.edit", R"({"path": "props/panels.paint.json",
        "patch": {"model": "props/panels.obj", "size": 32, "maps": {"rays": 2},
                  "parameters": {"tone": 0.5},
                  "variants": {"count": 2, "ranges": {"tone": [0.1, 0.9]}}},
        "layers": {"base": {"fill": {"albedo": ["$tone", "$tone", "$tone"]}},
                   "sign": {"fill": {"roughness": 0.2},
                            "decals": [{"text": {"text": "OPEN", "size": 24,
                                                 "color": [1, 0, 0]},
                                        "at": [0.5, 0.5, 0.1], "size": 0.8}]}}})");
    // The words paint the left panel red, and only there.
    const json::Value baked = engine.ok("paint.bake", R"({"path": "props/panels.paint.json"})");
    CHECK(baked.find("masks")->find("sign")->as_double() > 0.01);
    CHECK(baked.find("masks")->find("sign")->as_double() < 0.4);
    // Every variant, each its own textures and material.
    const json::Value all =
        engine.ok("paint.bake", R"({"path": "props/panels.paint.json", "variant": "all"})");
    REQUIRE(all.find("variants")->size() == 2);
    const json::Value& second = all.find("variants")->as_array()[1];
    CHECK(second.find("variant")->as_int() == 2);
    CHECK(second.find("textures")->find("albedo")->as_string() == "props/panels_v2_albedo.png");
    CHECK(second.find("material")->as_string() == "props/panels_v2.standard_material.json");
    CHECK(std::filesystem::exists(root + "/props/panels_v1_albedo.png"));
    CHECK(all.find("variants")->as_array()[0].find("parameters")->find("tone")->as_double() !=
          second.find("parameters")->find("tone")->as_double());
    // One by number; one past the last is refused.
    CHECK(engine.ok("paint.bake", R"({"path": "props/panels.paint.json", "variant": 1})")
              .find("variants")
              ->size() == 1);
    CHECK_FALSE(
        engine.call("paint.bake", R"({"path": "props/panels.paint.json", "variant": 3})").ok);
    // A decal is an image or text, not both.
    const command::Reply both = engine.call("paint.edit", R"({"path": "props/panels.paint.json",
        "layers": {"sign": {"decals": [{"image": "a.png", "text": {"text": "A"},
                                        "at": [0, 0, 0]}]}}})");
    CHECK_FALSE(both.ok);

    // A texture graph's image node draws text too.
    const json::Value drawn = engine.ok("texture.generate", R"({"texture": {"format": 1,
        "type": "graph", "width": 64, "height": 32,
        "nodes": {"words": {"op": "image", "text": {"text": "HI", "size": 24}}},
        "outputs": [{"name": "albedo", "node": "words"}]}, "pixels": [[0, 0]]})");
    // White words (the default color) on clear.
    CHECK(*drawn.find("pixels") == parse("[[255, 255, 255, 0]]"));
    CHECK(drawn.find("channels")->find("a")->as_array()[2].as_double() == 255);
    std::filesystem::remove_all(root);
}
