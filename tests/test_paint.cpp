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
