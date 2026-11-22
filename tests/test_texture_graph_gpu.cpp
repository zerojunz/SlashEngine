#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/texture_graph.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace slash;
using render::GraphOp;
using render::GraphPass;
using render::kNoGraphSlot;

namespace {

GraphPass pass(GraphOp op, std::uint32_t out, std::initializer_list<std::uint32_t> in,
               std::initializer_list<std::pair<std::size_t, float>> params) {
    GraphPass made;
    made.op = op;
    made.out = out;
    std::size_t k = 0;
    for (const std::uint32_t slot : in) {
        made.in.at(k++) = slot;
    }
    for (const auto& [at, value] : params) {
        made.params.at(at) = value;
    }
    return made;
}

// A ramp of two points at kGraphRampAt: black to orange.
void add_ramp(GraphPass& to) {
    const std::size_t at = render::kGraphRampAt;
    to.params.at(at) = 2.0F;
    const std::array<float, 10> points{0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.5F, 0.1F, 1.0F};
    std::copy(points.begin(), points.end(), to.params.begin() + at + 1);
}

// Every op once, each into a slot of its own, reading what came before.
render::TextureGraphProgram every_op(bool tiling) {
    render::TextureGraphProgram program;
    program.width = 48;
    program.height = 40;
    program.tiling = tiling;
    std::vector<GraphPass>& p = program.passes;
    // 0: fractal noise with a warp, seamless when tiling.
    p.push_back(pass(GraphOp::Noise, 0, {}, {{0, 0.0F},   {1, 7.0F},   {2, 0.03F},
                                             {3, 1.0F},   {4, 4.0F},   {5, 2.0F},
                                             {6, 0.5F},   {8, 2.0F},   {9, 1.0F},
                                             {11, 1.0F},  {12, 1.0F},  {13, 0.0F},
                                             {14, 10.0F}, {15, 0.05F}, {16, 1.0F},
                                             {17, 3.0F},  {18, 2.0F},  {19, 0.5F},
                                             {23, 48.0F}, {24, 40.0F}, {25, tiling ? 1.0F : 0.0F},
                                             {26, 0.1F}}));
    // 1: cellular noise.
    p.push_back(pass(GraphOp::Noise, 1, {},
                     {{0, 2.0F},
                      {1, 3.0F},
                      {2, 0.08F},
                      {4, 1.0F},
                      {5, 2.0F},
                      {6, 0.5F},
                      {9, 1.0F},
                      {10, 1.0F},
                      {11, 1.0F},
                      {23, 48.0F},
                      {24, 40.0F}}));
    GraphPass gradient = pass(GraphOp::Gradient, 2, {},
                              {{0, 1.0F}, {1, 0.5F}, {2, 0.5F}, {3, 1.0F}, {4, 0.5F}, {5, 2.0F}});
    add_ramp(gradient);
    p.push_back(gradient);
    p.push_back(
        pass(GraphOp::Shape, 3, {},
             {{0, 2.0F}, {1, 2.0F}, {2, 2.0F}, {3, 0.7F}, {4, 6.0F}, {5, 0.3F}, {6, 0.1F}}));
    p.push_back(pass(GraphOp::Bricks, 4, {},
                     {{0, 4.0F}, {1, 8.0F}, {2, 0.06F}, {3, 0.5F}, {4, 0.1F}, {5, 9.0F}}));
    p.push_back(pass(GraphOp::Constant, 5, {}, {{0, 0.2F}, {1, 0.4F}, {2, 0.6F}, {3, 1.0F}}));
    p.push_back(pass(GraphOp::Waves, 6, {}, {{0, 3.0F}, {1, 0.125F}, {2, 0.0F}, {3, 0.25F}}));
    p.push_back(pass(GraphOp::Image, 7, {}, {{0, 0.0F}}));
    p.push_back(
        pass(GraphOp::Levels, 8, {0}, {{0, 0.1F}, {1, 0.9F}, {2, 1.6F}, {3, 0.05F}, {4, 0.95F}}));
    p.push_back(pass(GraphOp::Invert, 9, {4}, {}));
    p.push_back(pass(GraphOp::Clamp, 10, {9}, {}));
    GraphPass map = pass(GraphOp::GradientMap, 11, {0}, {});
    add_ramp(map);
    p.push_back(map);
    p.push_back(pass(GraphOp::Hsv, 12, {11}, {{0, 0.3F}, {1, 1.2F}, {2, 0.9F}}));
    p.push_back(pass(GraphOp::Blur, 13, {1}, {{0, 3.0F}, {1, 0.0F}}));
    p.push_back(pass(GraphOp::Blur, 14, {13}, {{0, 3.0F}, {1, 1.0F}}));
    p.push_back(pass(GraphOp::Sharpen, 15, {0}, {{0, 0.8F}}));
    p.push_back(pass(GraphOp::Warp, 16, {4, 0}, {{0, 0.05F}, {1, 0.0F}}));
    p.push_back(pass(GraphOp::Warp, 17, {4, 1}, {{0, 0.04F}, {1, 1.0F}, {2, 0.2F}}));
    p.push_back(pass(GraphOp::Transform, 18, {7},
                     {{0, 2.0F}, {1, 1.5F}, {2, 0.1F}, {3, 0.2F}, {4, -0.1F}}));
    p.push_back(pass(GraphOp::EdgeDetect, 19, {4}, {{0, 1.0F}}));
    p.push_back(pass(GraphOp::Grayscale, 20, {12}, {}));
    p.push_back(pass(GraphOp::Threshold, 21, {0}, {{0, 0.5F}, {1, 0.1F}}));
    p.push_back(pass(GraphOp::Posterize, 22, {2}, {{0, 4.0F}}));
    p.push_back(pass(GraphOp::Merge, 23, {4, kNoGraphSlot, 0, kNoGraphSlot},
                     {{0, 0.0F}, {2, 1.0F}, {5, 0.5F}, {7, 1.0F}}));
    p.push_back(pass(GraphOp::Channels, 24, {4}, {{0, 2.0F}, {1, 1.0F}, {2, 4.0F}, {3, 5.0F}}));
    p.push_back(pass(GraphOp::NormalFromHeight, 25, {4}, {{0, 4.0F}}));
    p.push_back(pass(GraphOp::AoFromHeight, 26, {4}, {{0, 4.0F}, {1, 1.0F}}));
    p.push_back(pass(GraphOp::CurvatureFromHeight, 27, {4}, {{0, 2.0F}}));
    p.push_back(pass(GraphOp::Blend, 28, {11, 2, 0}, {{0, 5.0F}, {1, 0.8F}}));
    p.push_back(pass(GraphOp::Math, 29, {0, 1}, {{0, 6.0F}}));
    p.push_back(
        pass(GraphOp::Math, 30, {0}, {{0, 2.0F}, {2, 0.5F}, {3, 0.5F}, {4, 0.5F}, {5, 1.0F}}));
    p.push_back(
        pass(GraphOp::Remap, 31, {0}, {{0, 0.2F}, {1, 0.8F}, {2, 1.0F}, {3, 0.0F}, {4, 1.0F}}));
    program.slots = 32;
    render::GraphImage image;
    image.width = 5;
    image.height = 3;
    for (std::uint32_t i = 0; i < image.width * image.height; ++i) {
        image.texels.insert(image.texels.end(), {static_cast<float>(i % 5) / 4.0F,
                                                 static_cast<float>(i / 5) / 2.0F, 0.25F, 1.0F});
    }
    program.images.push_back(std::move(image));
    return program;
}

} // namespace

TEST_CASE("every device bakes every texture graph op as the CPU twin does") {
    for (const bool tiling : {false, true}) {
        CAPTURE(tiling);
        const render::TextureGraphProgram program = every_op(tiling);
        REQUIRE(render::check_texture_graph(program).ok());
        std::vector<std::uint32_t> outputs(program.slots);
        for (std::uint32_t i = 0; i < program.slots; ++i) {
            outputs[i] = i;
        }
        auto expected = render::run_texture_graph(program, outputs);
        REQUIRE_MESSAGE(expected.ok(), expected.error().message);
        for (const auto& device : test::drawing_devices()) {
            CAPTURE(render::backend_name(device->backend()));
            auto baked = device->bake_texture_graph(program, outputs);
            REQUIRE_MESSAGE(baked.ok(), baked.error().message);
            REQUIRE(baked.value().size() == outputs.size());
            for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
                CAPTURE(render::graph_op_name(program.passes.at(slot).op));
                const render::GraphTexels& a = baked.value()[slot];
                const render::GraphTexels& b = expected.value()[slot];
                REQUIRE(a.size() == b.size());
                // Within an 8-bit step, but for a few values where float rounding tips a hard
                // edge (a threshold, a posterize step, a cell's border) either way.
                std::size_t off = 0;
                float worst = 0.0F;
                for (std::size_t k = 0; k < a.size(); ++k) {
                    const float d = std::abs(a[k] - b[k]);
                    worst = std::max(worst, d);
                    off += d > 1.0F / 255.0F ? 1 : 0;
                }
                CAPTURE(worst);
                CHECK(off * 100 <= a.size());
            }
        }
    }
}

TEST_CASE("devices refuse texture graph programs the twin refuses") {
    render::TextureGraphProgram program = every_op(false);
    program.passes.at(9).in.at(0) = 31; // a slot no earlier pass wrote
    const std::vector<std::uint32_t> outputs{0};
    for (const auto& device : test::drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto baked = device->bake_texture_graph(program, outputs);
        REQUIRE_FALSE(baked.ok());
        CHECK(baked.error().message.find("no earlier pass wrote") != std::string::npos);
    }
}
