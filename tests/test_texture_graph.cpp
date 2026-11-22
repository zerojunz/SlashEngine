#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/texture_graph.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace slash;

namespace {

json::Value parse(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE_MESSAGE(doc.ok(), doc.error().message);
    return doc.value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> stamps;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return make_error(ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                const auto stamp = stamps.find(path);
                return asset::FileStamp{.modified = stamp != stamps.end() ? stamp->second : 1,
                                        .size = it->second.size()};
            },
        };
    }
};

procedural::GraphReader reader_of(const MemoryFiles& files) {
    procedural::GraphReader reader;
    reader.text = [&files](std::string_view path) -> Result<std::string> {
        const auto it = files.files.find(path);
        if (it == files.files.end()) {
            return make_error(ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    reader.image = [&files](std::string_view path) -> Result<render::Image> {
        const auto it = files.files.find(path);
        if (it == files.files.end()) {
            return make_error(ErrorCode::NotFound, "no such file");
        }
        return render::decode_image(it->second);
    };
    return reader;
}

std::string first_error(const json::Value& doc) {
    const auto errors = procedural::validate_texture_graph(doc);
    return errors.empty() ? std::string() : errors.front().path + ": " + errors.front().message;
}

// A texel of slot texels: channel k of pixel (x, y).
float texel(const render::GraphTexels& texels, std::uint32_t width, std::uint32_t x,
            std::uint32_t y, std::size_t k) {
    return texels.at((static_cast<std::size_t>(y) * width + x) * 4 + k);
}

const char* const kWall = R"({
  "format": 1, "type": "graph", "width": 64, "height": 32,
  "parameters": {"mortar": 0.08, "rows": 4, "across": "=rows / 2",
                 "tint": [0.6, 0.3, 0.2, 1]},
  "nodes": {
    "bricks": {"op": "bricks", "across": "$across", "down": "$rows", "mortar": "$mortar",
               "bevel": 0.2},
    "grain": {"op": "noise", "noise": {"noise_type": "cellular", "frequency": 0.1}},
    "height": {"op": "blend", "base": "bricks", "layer": "grain", "mode": "multiply",
               "opacity": 0.3},
    "soft": {"op": "blur", "input": "height", "radius": 1},
    "color": {"op": "gradient_map", "input": "soft",
              "gradient": {"points": [{"offset": 0, "color": [0.2, 0.2, 0.2, 1]},
                                      {"offset": 1, "color": [0.6, 0.3, 0.2, 1]}]}},
    "normal": {"op": "normal_from_height", "height": "soft", "strength": 4},
    "unused": {"op": "checker"}
  },
  "outputs": [{"name": "albedo", "node": "color"}, {"name": "normal", "node": "normal"},
              {"name": "height", "node": "soft", "as": "gray"}]
})";

} // namespace

TEST_CASE("texture graphs check their nodes, settings, links and parameters") {
    const json::Value wall = parse(kWall);
    CHECK(procedural::is_texture_graph(wall));
    CHECK(procedural::validate_texture_graph(wall).empty());
    CHECK(procedural::validate_texture(wall).empty());

    // Each error has the JSON Pointer of the bad value and, for a misspelled name, the closest.
    json::Value doc = wall;
    doc["nodes"]["grain"]["op"] = "noize";
    CHECK(first_error(doc) == R"(/nodes/grain/op: unknown op "noize" (did you mean "noise"?))");
    doc = wall;
    doc["nodes"]["height"]["layer"] = "grian";
    CHECK(first_error(doc) ==
          R"(/nodes/height/layer: "grian" isn't a node (did you mean "grain"?))");
    doc = wall;
    doc["nodes"]["bricks"]["mortar"] = "$morter";
    CHECK(first_error(doc) ==
          R"(/nodes/bricks/mortar: "$morter" names no parameter (did you mean "$mortar"?))");
    doc = wall;
    doc["nodes"]["bricks"]["mortar"] = 2;
    CHECK(first_error(doc).starts_with("/nodes/bricks/mortar: "));
    doc = wall;
    doc["parameters"]["mortar"] = 3;
    CHECK(first_error(doc).starts_with("/nodes/bricks/mortar: "));
    doc = wall;
    doc["nodes"]["bricks"]["colour"] = 1;
    CHECK(first_error(doc).starts_with("/nodes/bricks/colour: unknown property"));
    doc = wall;
    doc["outputs"].as_array()[0]["node"] = "colour";
    CHECK(first_error(doc) == R"(/outputs/0/node: "colour" isn't a node (did you mean "color"?))");
    doc = wall;
    doc["nodes"]["bricks"] = parse(R"({"op": "invert", "input": "soft"})");
    CHECK(first_error(doc).starts_with("/nodes/"));
    CHECK(first_error(doc).find("reads itself through") != std::string::npos);
    doc = wall;
    doc["nodes"]["color"]["gradient"]["interpolation_mode"] = "cubic";
    CHECK(first_error(doc).starts_with("/nodes/color/gradient: a graph's gradient blends"));
    doc = wall;
    doc.as_object().erase("outputs");
    CHECK(first_error(doc).starts_with("/outputs: missing"));
    // A graph that extends another may leave its nodes and outputs to it.
    CHECK(procedural::validate_texture_graph(
              parse(R"({"format": 1, "type": "graph", "extends": "wall.texture.json",
                        "parameters": {"mortar": 0.1}})"))
              .empty());
}

TEST_CASE("texture graphs compile only what their outputs need, sharing slots") {
    MemoryFiles files;
    auto compiled = procedural::compile_texture_graph(parse(kWall), reader_of(files));
    REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
    const procedural::CompiledGraph& graph = compiled.value();
    CHECK(graph.program.width == 64);
    CHECK(graph.program.tiling);
    // bricks, grain, height, blur across and down, color, normal; the checker isn't needed.
    REQUIRE(graph.program.passes.size() == 7);
    CHECK(graph.program.passes.front().op == render::GraphOp::Bricks);
    CHECK(graph.program.passes[3].op == render::GraphOp::Blur);
    CHECK(graph.program.passes[3].params[1] == 0.0F);
    CHECK(graph.program.passes[4].params[1] == 1.0F);
    // Parameters and expressions are worked out: across = rows / 2.
    CHECK(graph.program.passes.front().params[0] == 2.0F);
    CHECK(graph.program.passes.front().params[1] == 4.0F);
    // Slots of values no longer needed are used again.
    CHECK(graph.program.slots < 7);
    REQUIRE(graph.outputs.size() == 3);
    CHECK(graph.outputs[2].name == "height");
    CHECK(graph.outputs[2].gray);
    CHECK(render::check_texture_graph(graph.program).ok());

    // Overrides replace parameters by name, keeping their kind.
    json::Object overrides{{"rows", json::Value(6)}};
    auto more = procedural::compile_texture_graph(parse(kWall), reader_of(files), overrides);
    REQUIRE(more.ok());
    CHECK(more.value().program.passes.front().params[0] == 3.0F);
    overrides = {{"rowz", json::Value(6)}};
    auto misspelled = procedural::compile_texture_graph(parse(kWall), reader_of(files), overrides);
    REQUIRE_FALSE(misspelled.ok());
    CHECK(misspelled.error().message.find("did you mean \"rows\"") != std::string::npos);
    overrides = {{"tint", json::Value(1)}};
    CHECK_FALSE(procedural::compile_texture_graph(parse(kWall), reader_of(files), overrides).ok());
}

TEST_CASE("texture graph passes run on the CPU as their ops say, the same on any thread count") {
    MemoryFiles files;
    const auto run = [&](std::string_view text, jobs::Pool* pool = nullptr) {
        auto compiled = procedural::compile_texture_graph(parse(text), reader_of(files));
        REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
        std::vector<std::uint32_t> slots;
        for (const procedural::GraphOutput& output : compiled.value().outputs) {
            slots.push_back(output.slot);
        }
        auto texels = render::run_texture_graph(compiled.value().program, slots, pool);
        REQUIRE_MESSAGE(texels.ok(), texels.error().message);
        return texels.value();
    };

    // A constant, a checker and math on them.
    const auto checker = run(R"({"format": 1, "type": "graph", "width": 4, "height": 4,
        "nodes": {"c": {"op": "checker", "cells": [2, 2]},
                  "k": {"op": "constant", "color": [0.25, 0.5, 0.75, 1]},
                  "m": {"op": "math", "a": "c", "b": "k", "operation": "multiply"}},
        "outputs": [{"name": "m", "node": "m"}]})");
    CHECK(texel(checker.at(0), 4, 0, 0, 1) == 0.0F);
    CHECK(texel(checker.at(0), 4, 2, 0, 1) == 0.5F);
    CHECK(texel(checker.at(0), 4, 2, 2, 1) == 0.0F);

    // Noise as a NoiseTexture2D samples it: one noise unit a pixel, gray = n * 0.5 + 0.5.
    const auto noise = run(R"({"format": 1, "type": "graph", "width": 16, "height": 16,
        "nodes": {"n": {"op": "noise", "noise": {"seed": 7}}},
        "outputs": [{"name": "n", "node": "n"}]})");
    procedural::NoiseSettings settings;
    settings.seed = 7;
    const procedural::Noise reference(settings);
    CHECK(texel(noise.at(0), 16, 10, 3, 0) == reference.get_noise_2d(10.0F, 3.0F) * 0.5F + 0.5F);

    // Merge packs channels, as an ORM texture.
    const auto orm = run(R"({"format": 1, "type": "graph", "width": 2, "height": 2,
        "nodes": {"k": {"op": "constant", "color": [0.1, 0.2, 0.3, 0.4]},
                  "orm": {"op": "merge", "red": 1, "green": {"node": "k", "channel": "b"},
                          "blue": "k"}},
        "outputs": [{"name": "orm", "node": "orm"}]})");
    CHECK(texel(orm.at(0), 2, 1, 1, 0) == 1.0F);
    CHECK(texel(orm.at(0), 2, 1, 1, 1) == 0.3F);
    CHECK(texel(orm.at(0), 2, 1, 1, 2) == 0.1F);
    CHECK(texel(orm.at(0), 2, 1, 1, 3) == 1.0F);

    // A flat height makes normals straight up; bricks are 0 in the mortar.
    const auto wall = run(kWall);
    CHECK(texel(wall.at(1), 64, 5, 5, 2) > 0.5F);
    jobs::Pool pool(3);
    CHECK(run(kWall, &pool) == wall);
}

TEST_CASE("texture graphs extend others and read images") {
    MemoryFiles files;
    files.files["wall.texture.json"] = kWall;
    const json::Value variant = parse(R"({"format": 1, "type": "graph",
        "extends": "wall.texture.json", "width": 16, "height": 16,
        "nodes": {"grain": {"noise": {"seed": 3}}}})");
    auto compiled = procedural::compile_texture_graph(variant, reader_of(files));
    REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
    CHECK(compiled.value().program.width == 16);
    CHECK(compiled.value().program.passes[1].params[1] == 3.0F);
    CHECK(compiled.value().files == std::vector<std::string>{"wall.texture.json"});

    const json::Value looped =
        parse(R"({"format": 1, "type": "graph", "extends": "loop.texture.json"})");
    files.files["loop.texture.json"] = json::write(looped);
    CHECK_FALSE(procedural::compile_texture_graph(looped, reader_of(files)).ok());

    render::Image red;
    red.width = 2;
    red.height = 2;
    red.pixels = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    files.files["red.png"] = render::encode_png(red).value();
    auto image = procedural::texture_graph_image(
        parse(R"({"format": 1, "type": "graph", "width": 4, "height": 4,
                  "nodes": {"i": {"op": "image", "path": "red.png"},
                            "g": {"op": "invert", "input": "i"}},
                  "outputs": [{"name": "inverted", "node": "g"}]})"),
        "", reader_of(files));
    REQUIRE_MESSAGE(image.ok(), image.error().message);
    CHECK(image.value().pixels[0] == 0);
    CHECK(image.value().pixels[1] == 255);
}

TEST_CASE("texture graphs load as textures, an output by its name") {
    MemoryFiles files;
    files.files["wall.texture.json"] = kWall;
    asset::Assets assets{files.source()};
    canvas::add_asset_types(assets);
    auto albedo = assets.load<render::Image>("wall.texture.json");
    REQUIRE_MESSAGE(albedo.ok(), albedo.error().message);
    CHECK(albedo.value()->width == 64);
    auto height = assets.load<render::Image>("wall.texture.json::height");
    REQUIRE_MESSAGE(height.ok(), height.error().message);
    // Gray outputs put the red in every color channel.
    CHECK(height.value()->pixels[0] == height.value()->pixels[1]);
    CHECK(height.value()->pixels[3] == 255);
    const auto missing = assets.load<render::Image>("wall.texture.json::hieght");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("did you mean \"height\"") != std::string::npos);
    CHECK(procedural::is_texture_path("wall.texture.json::normal"));

    // A graph reloads when a file it extends changes.
    files.files["small.texture.json"] =
        R"({"format": 1, "type": "graph", "extends": "wall.texture.json", "width": 8})";
    auto small = assets.load<render::Image>("small.texture.json");
    REQUIRE_MESSAGE(small.ok(), small.error().message);
    CHECK(small.value()->height == 32);
    json::Value wall = parse(kWall);
    wall["height"] = 16;
    files.files["wall.texture.json"] = json::write(wall);
    files.stamps["wall.texture.json"] = 2;
    (void)assets.poll();
    CHECK(small.value()->height == 16);
}

TEST_CASE("every device bakes texture graphs as the CPU twin does") {
    MemoryFiles files;
    auto compiled = procedural::compile_texture_graph(parse(kWall), reader_of(files));
    REQUIRE(compiled.ok());
    auto expected = procedural::bake_texture_graph(compiled.value(), nullptr);
    REQUIRE(expected.ok());
    for (const auto& device : test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto baked = procedural::bake_texture_graph(compiled.value(), device.get());
        REQUIRE_MESSAGE(baked.ok(), baked.error().message);
        REQUIRE(baked.value().size() == expected.value().size());
        for (std::size_t i = 0; i < baked.value().size(); ++i) {
            const auto& a = baked.value()[i].pixels;
            const auto& b = expected.value()[i].pixels;
            REQUIRE(a.size() == b.size());
            // GPUs are held to the twin within a step of 8 bits a channel, but for a few
            // channels on a hard edge, where float rounding can tip a pixel either way.
            std::size_t off = 0;
            for (std::size_t k = 0; k < a.size(); ++k) {
                off += std::abs(static_cast<int>(a[k]) - static_cast<int>(b[k])) > 1 ? 1U : 0U;
            }
            CHECK(off * 200 <= a.size());
        }
    }
}
