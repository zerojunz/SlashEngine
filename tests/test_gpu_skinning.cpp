// GPU skinning (create_skin and skin_mesh, shaders/skin.hlsl): the words the shader reads, and
// every device that skins on the GPU drawing what scene3d's CPU twins (blend_vertices and
// skin_vertices) make of the same skin.

#include "../engine/render/src/skin_gpu.hpp"
#include "devices.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#ifndef __EMSCRIPTEN__
#include "slash/scene3d/blend_shapes.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#endif

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
using render::Image;
using render::MeshId;
using render::RenderDevice;
using render::SkinId;
using render::Vertex3D;

namespace {

std::uint32_t word(float value) {
    return slash::bit_cast<std::uint32_t>(value);
}

// A strip two vertices wide and five tall from y -1 to 1, facing +z, as a bending arm's skin.
std::vector<Vertex3D> strip() {
    std::vector<Vertex3D> out;
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float x = column == 0 ? -0.3F : 0.3F;
            const float y = -1.0F + (0.5F * static_cast<float>(row));
            out.push_back({.position = {x, y, 0.0F},
                           .normal = {0.0F, 0.0F, 1.0F},
                           .tangent = {1.0F, 0.0F, 0.0F, -1.0F},
                           .uv = {static_cast<float>(column), static_cast<float>(row) * 0.25F},
                           .rgba = {224, 160, 64, 255}});
        }
    }
    return out;
}

std::vector<std::uint32_t> strip_indices() {
    std::vector<std::uint32_t> out;
    for (std::uint32_t row = 0; row < 4; ++row) {
        const std::uint32_t a = row * 2;
        out.insert(out.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
    return out;
}

// Each vertex weighed between joint 0 at the bottom and joint 1 at the top, a third joint
// taking no part.
std::vector<render::SkinWeights> strip_weights(std::span<const Vertex3D> vertices) {
    std::vector<render::SkinWeights> out;
    for (const Vertex3D& v : vertices) {
        const float t = (v.position[1] + 1.0F) * 0.5F;
        render::SkinWeights w;
        w.joints = {0, 1, 2, 0, 0, 0, 0, 0};
        w.weights = {1.0F - t, t, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        out.push_back(w);
    }
    return out;
}

// A blend shape that leans the strip toward the camera and turns its normals sideways.
render::SkinShape lean(std::span<const Vertex3D> vertices) {
    render::SkinShape shape;
    for (const Vertex3D& v : vertices) {
        shape.positions.push_back({0.2F * v.position[1], 0.0F, 0.3F});
        shape.normals.push_back({0.4F, 0.0F, 0.0F});
        shape.tangents.push_back({0.0F, 0.2F, 0.0F});
    }
    return shape;
}

// Joint 0 where it rests, joint 1 turned degrees about z around (0, 0, 0) and lifted.
std::vector<render::SkinJoint> pose(float degrees) {
    const slash::Vec2 turn = slash::cos_sin(degrees * 3.14159265F / 180.0F);
    const auto c = static_cast<float>(turn.x);
    const auto s = static_cast<float>(turn.y);
    return {render::SkinJoint{},
            {.x = {c, s, 0.0F},
             .y = {-s, c, 0.0F},
             .z = {0.0F, 0.0F, 1.0F},
             .origin = {0.1F, 0.15F, 0.0F}}};
}

using Float3 = std::array<float, 3>;

Float3 unit(const Float3& v) {
    const float length = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
    return length == 0.0F ? v : Float3{v[0] / length, v[1] / length, v[2] / length};
}

// What scene3d's blend_vertices and skin_vertices make of a skin (the test after this one holds
// the two to each other), written over render's types so the web build, which has no scene3d,
// checks WebGPU against it too.
std::vector<Vertex3D> moved_on_cpu(std::span<const Vertex3D> vertices,
                                   std::span<const render::SkinWeights> weights,
                                   std::span<const render::SkinShape> shapes,
                                   std::span<const float> shape_weights,
                                   std::span<const render::SkinJoint> joints) {
    std::vector<Vertex3D> out(vertices.begin(), vertices.end());
    std::vector<Float3> normals(out.size(), Float3{});
    std::vector<Float3> tangents(out.size(), Float3{});
    bool turned = false;
    for (std::size_t k = 0; k < shapes.size() && k < shape_weights.size(); ++k) {
        const float w = shape_weights[k];
        if (std::abs(w) <= 0.0001F) {
            continue;
        }
        const render::SkinShape& shape = shapes[k];
        for (std::size_t i = 0; i < out.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                if (!shape.positions.empty()) {
                    out[i].position.at(c) += shape.positions[i].at(c) * w;
                }
                if (!shape.normals.empty()) {
                    normals[i].at(c) += shape.normals[i].at(c) * w;
                }
                if (!shape.tangents.empty()) {
                    tangents[i].at(c) += shape.tangents[i].at(c) * w;
                }
            }
        }
        turned = turned || !shape.normals.empty() || !shape.tangents.empty();
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        Vertex3D& v = out[i];
        if (turned) {
            v.normal = unit({v.normal[0] + normals[i][0], v.normal[1] + normals[i][1],
                             v.normal[2] + normals[i][2]});
            const Float3 t = unit({v.tangent[0] + tangents[i][0], v.tangent[1] + tangents[i][1],
                                   v.tangent[2] + tangents[i][2]});
            v.tangent = {t[0], t[1], t[2], v.tangent[3]};
        }
        if (i >= weights.size()) {
            continue;
        }
        render::SkinJoint m{.x = {}, .y = {}, .z = {}, .origin = {}};
        float total = 0.0F;
        for (std::size_t k = 0; k < 8; ++k) {
            const float weight = weights[i].weights.at(k);
            const std::size_t joint = weights[i].joints.at(k);
            if (weight == 0.0F || joint >= joints.size()) {
                continue;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                m.x.at(c) += joints[joint].x.at(c) * weight;
                m.y.at(c) += joints[joint].y.at(c) * weight;
                m.z.at(c) += joints[joint].z.at(c) * weight;
                m.origin.at(c) += joints[joint].origin.at(c) * weight;
            }
            total += weight;
        }
        if (total <= 0.0F) {
            continue;
        }
        const auto basis = [&m](const Float3& p) {
            return Float3{(m.x[0] * p[0]) + (m.y[0] * p[1]) + (m.z[0] * p[2]),
                          (m.x[1] * p[0]) + (m.y[1] * p[1]) + (m.z[1] * p[2]),
                          (m.x[2] * p[0]) + (m.y[2] * p[1]) + (m.z[2] * p[2])};
        };
        const Float3 p = basis(v.position);
        v.position = {p[0] + m.origin[0], p[1] + m.origin[1], p[2] + m.origin[2]};
        v.normal = unit(basis(v.normal));
        const Float3 t = unit(basis({v.tangent[0], v.tangent[1], v.tangent[2]}));
        v.tangent = {t[0], t[1], t[2], v.tangent[3]};
    }
    return out;
}

struct Box {
    std::array<float, 3> low{};
    std::array<float, 3> high{};
};

Box bounds_of(std::span<const Vertex3D> vertices) {
    Box box{.low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()},
            .high = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                     std::numeric_limits<float>::lowest()}};
    for (const Vertex3D& v : vertices) {
        for (std::size_t a = 0; a < 3; ++a) {
            box.low.at(a) = std::min(box.low.at(a), v.position.at(a));
            box.high.at(a) = std::max(box.high.at(a), v.position.at(a));
        }
    }
    return box;
}

// The mesh drawn lit from the front, both its sides.
Image draw(RenderDevice& device, MeshId mesh) {
    auto target = device.create_texture({.width = 64, .height = 64, .render_target = true});
    REQUIRE(target.ok());
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 3.2F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.ambient = {0.2F, 0.2F, 0.25F};
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 0.95F, 0.9F},
                     .direction = {-0.5F, -0.3F, -0.8F}}};
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}, .scene = scene}).ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK(device
              .draw_mesh(mesh, identity,
                         {.albedo = {0.9F, 0.6F, 0.3F, 1.0F},
                          .roughness = 0.5F,
                          .cull = render::CullMode::Disabled})
              .ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_texture(target.value());
    return image.value();
}

// How many pixels of a and b differ by more than 2 in a channel.
int differing(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    int count = 0;
    for (std::size_t at = 0; at < a.pixels.size(); at += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int{a.pixels.at(at + c)} - int{b.pixels.at(at + c)}) > 2) {
                ++count;
                break;
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("a skin's words are its rest vertices, weights two joints a word and shapes' offsets") {
    const std::vector<Vertex3D> vertices = strip();
    const std::vector<render::SkinWeights> device = strip_weights(vertices);
    // A shape that moves only positions, and one that turns normals.
    render::SkinShape still;
    still.positions.assign(vertices.size(), {1.0F, 2.0F, 3.0F});
    const render::SkinShape turning = lean(vertices);
    const std::vector<render::SkinShape> shapes{still, turning};
    auto words =
        render::detail::skin_words({.vertices = vertices, .weights = device, .shapes = shapes});
    REQUIRE(words.ok());
    const render::detail::SkinWords& w = words.value();
    CHECK(w.vertex_count == 10);
    CHECK(w.shape_count == 2);
    CHECK(w.rest.size() == 10 * 15);
    CHECK(w.rest.at(15 + 1) == word(-1.0F)); // vertex 1's y
    CHECK(w.rest.at(9) == word(-1.0F));      // the tangent's sign
    REQUIRE(w.weights.size() == 10 * render::detail::kSkinWeightWords);
    CHECK(w.weights.at(0) == (0U | (1U << 16U)));
    CHECK(w.weights.at(1) == 2U);
    CHECK(w.weights.at(4) == word(1.0F));
    CHECK(w.weights.at(5) == word(0.0F));
    REQUIRE(w.shapes.size() == 2 * 10 * render::detail::kSkinShapeWords);
    CHECK(w.shapes.at(2) == word(3.0F));
    CHECK(w.shapes.at(3) == word(0.0F)); // the still shape's normal offset
    CHECK(w.shapes.at((10 * 9) + 3) == word(0.4F));
    CHECK(w.turns == std::vector<bool>{false, true});

    // Weights for some vertices only, and a shape's offsets for some, are refused.
    const std::span<const render::SkinWeights> some(device.data(), 4);
    CHECK_FALSE(render::detail::skin_words({.vertices = vertices, .weights = some}).ok());
    render::SkinShape short_shape;
    short_shape.normals.assign(3, {0.0F, 0.0F, 0.0F});
    const std::vector<render::SkinShape> short_shapes{short_shape};
    CHECK_FALSE(render::detail::skin_words({.vertices = vertices, .shapes = short_shapes}).ok());
    CHECK_FALSE(render::detail::skin_words({}).ok());
}

TEST_CASE("a pose appends joints and shape weights, turning normals only for weighed turners") {
    const std::vector<Vertex3D> vertices = strip();
    render::SkinShape still;
    still.positions.assign(vertices.size(), {1.0F, 2.0F, 3.0F});
    const std::vector<render::SkinShape> shapes{still, lean(vertices)};
    auto words = render::detail::skin_words({.vertices = vertices, .shapes = shapes});
    REQUIRE(words.ok());

    std::vector<std::uint32_t> poses{7, 7, 7};
    const std::vector<render::SkinJoint> joints = pose(30.0F);
    const std::array<float, 2> moving_still{0.5F, 0.0F};
    const render::detail::SkinUniforms first =
        render::detail::skin_pose(words.value(), joints, moving_still, poses);
    CHECK(first.counts == std::array<std::uint32_t, 4>{10, 2, 2, 0});
    CHECK(first.at == std::array<std::uint32_t, 4>{3, 3 + 24, 0, 0});
    REQUIRE(poses.size() == 3 + 24 + 2);
    CHECK(poses.at(3 + 12 + 9) == word(0.1F)); // joint 1's origin
    CHECK(poses.at(3 + 24) == word(0.5F));

    // The turning shape at a weight, and fewer weights than shapes (the rest are 0).
    const std::array<float, 2> both{0.0F, 0.25F};
    CHECK(render::detail::skin_pose(words.value(), {}, both, poses).counts[3] == 1);
    const std::array<float, 1> one{0.25F};
    const render::detail::SkinUniforms fewer =
        render::detail::skin_pose(words.value(), {}, one, poses);
    CHECK(fewer.counts[3] == 0);
    CHECK(poses.back() == word(0.0F));

    // The shader reads a word even when there is nothing to read.
    auto plain = render::detail::skin_words({.vertices = vertices});
    REQUIRE(plain.ok());
    std::vector<std::uint32_t> empty;
    CHECK(render::detail::skin_pose(plain.value(), {}, {}, empty).at[2] == 0);
    CHECK(empty.size() == 1);
}

TEST_CASE("a skinning dispatch runs a thread a vertex, 65536 across before it wraps") {
    using render::detail::skin_groups;
    CHECK(skin_groups(1) == std::array<std::uint32_t, 2>{1, 1});
    CHECK(skin_groups(64) == std::array<std::uint32_t, 2>{1, 1});
    CHECK(skin_groups(65) == std::array<std::uint32_t, 2>{2, 1});
    CHECK(skin_groups(65536) == std::array<std::uint32_t, 2>{1024, 1});
    CHECK(skin_groups((65536 * 2) + 1) == std::array<std::uint32_t, 2>{1024, 3});
}

TEST_CASE("devices that skin on the GPU check skins and the meshes they move") {
    const std::vector<Vertex3D> vertices = strip();
    const std::vector<std::uint32_t> indices = strip_indices();
    const std::vector<render::SkinWeights> device_w = strip_weights(vertices);
    const std::vector<render::SkinJoint> joints = pose(30.0F);
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::drawing_devices();
    devices.push_back(render::create_null_device());
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        if (!device->gpu_skinning()) {
            // These skin on the CPU, and scene3d calls update_mesh.
            CHECK_FALSE(device->create_skin({.vertices = vertices, .weights = device_w}).ok());
            CHECK_FALSE(device->skin_mesh(MeshId{1}, SkinId{1}, joints, {}, {}, {}).ok());
            continue;
        }
        CHECK_FALSE(device->create_skin({}).ok());
        const std::span<const render::SkinWeights> some(device_w.data(), 3);
        CHECK_FALSE(device->create_skin({.vertices = vertices, .weights = some}).ok());
        auto skin = device->create_skin({.vertices = vertices, .weights = device_w});
        REQUIRE(skin.ok());
        auto mesh = device->create_mesh(vertices, indices);
        REQUIRE(mesh.ok());
        const std::vector<Vertex3D> fewer(vertices.begin(), vertices.begin() + 3);
        auto small = device->create_mesh(fewer, {});
        REQUIRE(small.ok());
        CHECK_FALSE(device->skin_mesh(mesh.value(), SkinId{999}, joints, {}, {}, {}).ok());
        CHECK_FALSE(device->skin_mesh(MeshId{999}, skin.value(), joints, {}, {}, {}).ok());
        CHECK_FALSE(device->skin_mesh(small.value(), skin.value(), joints, {}, {}, {}).ok());

        auto target = device->create_texture({.width = 8, .height = 8, .render_target = true});
        REQUIRE(target.ok());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}}).ok());
        CHECK_FALSE(device->skin_mesh(mesh.value(), skin.value(), joints, {}, {}, {}).ok());
        REQUIRE(device->end_frame().ok());

        CHECK(device->skin_mesh(mesh.value(), skin.value(), joints, {}, {}, {}).ok());
        // A mesh destroyed with its skinning waiting, and a skin destroyed after it.
        device->destroy_mesh(mesh.value());
        device->destroy_skin(skin.value());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}}).ok());
        REQUIRE(device->end_frame().ok());
        device->destroy_mesh(small.value());
        device->destroy_texture(target.value());
    }
}

#ifndef __EMSCRIPTEN__
TEST_CASE("the test's CPU twin moves vertices as scene3d's blend_vertices and skin_vertices") {
    const std::vector<Vertex3D> vertices = strip();
    const std::vector<render::SkinWeights> weights = strip_weights(vertices);
    const render::SkinShape shape = lean(vertices);
    slash::scene3d::ModelSurface surface;
    surface.vertices = vertices;
    for (const render::SkinWeights& w : weights) {
        surface.weights.push_back({.joints = w.joints, .weights = w.weights});
    }
    surface.blend_shapes.push_back(
        {.positions = shape.positions, .normals = shape.normals, .tangents = shape.tangents});
    const std::vector<render::SkinShape> shapes{shape};
    for (const float weight : {0.0F, 0.6F, -0.4F}) {
        const std::vector<render::SkinJoint> joints = pose(35.0F);
        std::vector<slash::world::Affine3D> affine;
        for (const render::SkinJoint& j : joints) {
            affine.push_back({.x = {j.x[0], j.x[1], j.x[2]},
                              .y = {j.y[0], j.y[1], j.y[2]},
                              .z = {j.z[0], j.z[1], j.z[2]},
                              .origin = {j.origin[0], j.origin[1], j.origin[2]}});
        }
        std::vector<Vertex3D> expected;
        const std::array<float, 1> weights_of{weight};
        slash::scene3d::blend_vertices(surface, weights_of, expected);
        slash::scene3d::skin_vertices(expected, surface.weights, affine, expected);
        const std::vector<Vertex3D> moved =
            moved_on_cpu(vertices, weights, shapes, weights_of, joints);
        REQUIRE(moved.size() == expected.size());
        for (std::size_t i = 0; i < moved.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(moved[i].position.at(c) == doctest::Approx(expected[i].position.at(c)));
                CHECK(moved[i].normal.at(c) == doctest::Approx(expected[i].normal.at(c)));
                CHECK(moved[i].tangent.at(c) == doctest::Approx(expected[i].tangent.at(c)));
            }
            CHECK(moved[i].tangent[3] == expected[i].tangent[3]);
        }
    }
}
#endif

TEST_CASE("GPU skinning draws what scene3d's CPU twins make of the skin") {
    const std::vector<Vertex3D> vertices = strip();
    const std::vector<std::uint32_t> indices = strip_indices();
    const std::vector<render::SkinWeights> device_w = strip_weights(vertices);
    const std::vector<render::SkinShape> shapes{lean(vertices)};
    const auto on_cpu = [&](float weight, std::span<const render::SkinJoint> joints,
                            bool skinned = true) {
        const std::array<float, 1> shape_weights{weight};
        return moved_on_cpu(vertices,
                            skinned ? std::span<const render::SkinWeights>(device_w)
                                    : std::span<const render::SkinWeights>(),
                            shapes, shape_weights, joints);
    };
    bool skinned_any = false;
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        if (!device->gpu_skinning()) {
            continue;
        }
        skinned_any = true;
        auto skin =
            device->create_skin({.vertices = vertices, .weights = device_w, .shapes = shapes});
        REQUIRE(skin.ok());
        auto gpu = device->create_mesh(vertices, indices);
        auto cpu = device->create_mesh(vertices, indices);
        REQUIRE(gpu.ok());
        REQUIRE(cpu.ok());
        const Image rest = draw(*device, cpu.value());

        // Three poses in turn, each through the vertices the one before wrote, the last with no
        // shape at its weight.
        for (const auto& [degrees, weight] :
             std::vector<std::pair<float, float>>{{40.0F, 0.6F}, {-25.0F, 1.0F}, {70.0F, 0.0F}}) {
            CAPTURE(degrees);
            const std::vector<render::SkinJoint> joints = pose(degrees);
            const std::vector<Vertex3D> moved = on_cpu(weight, joints);
            REQUIRE(device->update_mesh(cpu.value(), moved).ok());
            const Box box = bounds_of(moved);
            const std::array<float, 1> shape_weights{weight};
            REQUIRE(
                device
                    ->skin_mesh(gpu.value(), skin.value(), joints, shape_weights, box.low, box.high)
                    .ok());
            const Image expected = draw(*device, cpu.value());
            const Image image = draw(*device, gpu.value());
            CHECK(differing(rest, expected) > 100);
            CHECK(differing(image, expected) <= 6);
        }

        // update_mesh after skin_mesh wins, as the later call.
        const std::vector<Vertex3D> moved = on_cpu(0.3F, pose(15.0F));
        const std::array<float, 1> some{0.8F};
        REQUIRE(
            device
                ->skin_mesh(gpu.value(), skin.value(), pose(-60.0F), some, {-2, -2, -2}, {2, 2, 2})
                .ok());
        REQUIRE(device->update_mesh(gpu.value(), moved).ok());
        REQUIRE(device->update_mesh(cpu.value(), moved).ok());
        CHECK(differing(draw(*device, gpu.value()), draw(*device, cpu.value())) == 0);

        // A skin of blend shapes only.
        auto shaped = device->create_skin({.vertices = vertices, .shapes = shapes});
        REQUIRE(shaped.ok());
        const std::array<float, 1> full{1.0F};
        REQUIRE(
            device->skin_mesh(gpu.value(), shaped.value(), {}, full, {-2, -2, -2}, {2, 2, 2}).ok());
        REQUIRE(device->update_mesh(cpu.value(), on_cpu(1.0F, {}, false)).ok());
        CHECK(differing(draw(*device, gpu.value()), draw(*device, cpu.value())) <= 6);

        device->destroy_skin(shaped.value());
        device->destroy_skin(skin.value());
        device->destroy_mesh(gpu.value());
        device->destroy_mesh(cpu.value());
    }
    if (!skinned_any) {
        MESSAGE("no device here skins on the GPU, so GPU skinning's pixels aren't checked");
    }
}
