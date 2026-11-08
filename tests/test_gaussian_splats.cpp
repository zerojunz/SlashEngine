#include "../engine/render/src/mesh_shading.hpp"
#include "../engine/render/src/splats.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/splats.hpp"
#include "slash/scene3d/splats.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

constexpr float kSh0 = 0.28209479177387814F;

// A splat at place, round with the standard deviation size, of the sRGB color color (its
// degree-0 coefficients), with higher coefficients 0.
void add_splat(render::GaussianSplats& splats, std::array<float, 3> place, float size,
               std::array<float, 3> color, float opacity = 0.999F) {
    splats.positions.push_back(place);
    splats.scales.push_back({size, size, size});
    splats.rotations.push_back({1.0F, 0.0F, 0.0F, 0.0F});
    splats.opacities.push_back(opacity);
    for (std::uint32_t k = 0; k < splats.coefficients(); ++k) {
        for (std::size_t c = 0; c < 3; ++c) {
            splats.sh.push_back(k == 0 ? (color.at(c) - 0.5F) / kSh0 : 0.0F);
        }
    }
}

// A camera at the origin looking down -z, square, 60 degrees across.
render::detail::SplatFrame frame(std::uint32_t size) {
    render::detail::SplatFrame out;
    out.width = size;
    out.height = size;
    out.view_projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    out.camera = {0.0F, 0.0F, 0.0F};
    return out;
}

render::Splats3D cloud(const render::GaussianSplats& splats,
                       std::array<float, 16> transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,
                                                          0, 1}) {
    render::Splats3D out;
    out.clouds.push_back({.splats = std::make_shared<const render::GaussianSplats>(splats),
                          .transform = transform,
                          .sh_degree = 3,
                          .opacity = 1.0F});
    return out;
}

std::array<float, 4> pixel(const std::vector<float>& blend, std::uint32_t size, std::uint32_t x,
                           std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * size) + x) * 4;
    return {blend[at], blend[at + 1], blend[at + 2], blend[at + 3]};
}

} // namespace

TEST_CASE("Gaussian splat .ply files are read as 3D Gaussian Splatting writes them") {
    render::GaussianSplats splats;
    splats.sh_degree = 1;
    add_splat(splats, {1.0F, 2.0F, 3.0F}, 0.25F, {0.8F, 0.4F, 0.2F}, 0.75F);
    add_splat(splats, {-1.0F, 0.5F, -2.0F}, 0.5F, {0.1F, 0.9F, 0.3F}, 0.2F);
    splats.rotations[1] = {0.0F, 1.0F, 0.0F, 0.0F};
    splats.sh[3] = 0.25F; // splat 0's first degree-1 coefficient in red
    splats.sh[(4 * 3) + 11] = -0.5F;
    const std::string file = render::write_splat_ply(splats);
    CHECK(file.starts_with("ply\nformat binary_little_endian 1.0\nelement vertex 2\n"));
    CHECK(file.find("property float f_rest_8\nproperty float opacity\n") != std::string::npos);

    auto read = render::read_splat_ply(file);
    REQUIRE_MESSAGE(read.ok(), read.error().message);
    const render::GaussianSplats& back = read.value();
    REQUIRE(back.size() == 2);
    CHECK(back.sh_degree == 1);
    CHECK(back.positions == splats.positions);
    CHECK(back.rotations == splats.rotations);
    CHECK(back.sh == splats.sh);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(back.opacities[i] == doctest::Approx(splats.opacities[i]).epsilon(1e-5));
        CHECK(back.scales[i][0] == doctest::Approx(splats.scales[i][0]).epsilon(1e-5));
    }

    // ASCII files, with normals and an element before the vertices, read the same; rotations are
    // made unit length.
    const std::string ascii = "ply\r\nformat ascii 1.0\ncomment from a tool\nelement camera 1\n"
                              "property float fx\nelement vertex 1\nproperty float x\n"
                              "property float y\nproperty float z\nproperty float nx\n"
                              "property float ny\nproperty float nz\nproperty float f_dc_0\n"
                              "property float f_dc_1\nproperty float f_dc_2\n"
                              "property float opacity\nproperty float scale_0\n"
                              "property float scale_1\nproperty float scale_2\n"
                              "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
                              "property float rot_3\nend_header\n500\n"
                              "1 2 3 0 0 1 0.5 -0.5 0 0 0 0 0 2 0 0 0\n";
    auto text = render::read_splat_ply(ascii);
    REQUIRE_MESSAGE(text.ok(), text.error().message);
    CHECK(text.value().positions[0] == std::array<float, 3>{1.0F, 2.0F, 3.0F});
    CHECK(text.value().sh_degree == 0);
    CHECK(text.value().sh == std::vector<float>{0.5F, -0.5F, 0.0F});
    CHECK(text.value().opacities[0] == 0.5F);
    CHECK(text.value().scales[0] == std::array<float, 3>{1.0F, 1.0F, 1.0F});
    CHECK(text.value().rotations[0] == std::array<float, 4>{1.0F, 0.0F, 0.0F, 0.0F});

    // Big-endian files read too.
    std::string big = "ply\nformat binary_big_endian 1.0\nelement vertex 1\n";
    for (const char* name : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0",
                             "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}) {
        big += "property float " + std::string(name) + "\n";
    }
    big += "end_header\n";
    for (const float value :
         {4.0F, 5.0F, 6.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F}) {
        std::array<char, 4> raw{};
        std::memcpy(raw.data(), &value, 4);
        const std::uint16_t one = 1;
        std::uint8_t first = 0;
        std::memcpy(&first, &one, 1);
        if (first == 1) {
            std::swap(raw[0], raw[3]);
            std::swap(raw[1], raw[2]);
        }
        big.append(raw.data(), 4);
    }
    auto swapped = render::read_splat_ply(big);
    REQUIRE_MESSAGE(swapped.ok(), swapped.error().message);
    CHECK(swapped.value().positions[0] == std::array<float, 3>{4.0F, 5.0F, 6.0F});

    // Errors say what is wrong.
    CHECK(render::read_splat_ply("solid cube").error().message ==
          "not a .ply file: it doesn't start with ply");
    CHECK(render::read_splat_ply("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n"
                                 "end_header\n1\n")
              .error()
              .message == "the .ply file's vertices have no y, so they aren't Gaussian splats");
    CHECK(render::read_splat_ply(file.substr(0, file.size() - 10)).error().message ==
          "the .ply file ends before its 2 vertices do");
    std::string three = file;
    three.replace(three.find("property float f_rest_3\n"), 24, "");
    CHECK(render::read_splat_ply(three).error().message.starts_with("the .ply file's vertices"));
    CHECK(render::read_splat_ply("ply\nformat binary_little_endian 1.0\nelement face 1\n"
                                 "property list uchar int vertex_indices\nelement vertex 0\n"
                                 "end_header\n")
              .error()
              .message ==
          "the .ply file's face element has a list property, which splats don't have");
}

TEST_CASE("splats' colors come from their spherical harmonics toward the camera") {
    render::GaussianSplats splats;
    splats.sh_degree = 1;
    add_splat(splats, {0.0F, 0.0F, 0.0F}, 1.0F, {0.5F, 0.25F, 0.0F});
    // Red brightens toward -x (the trainer's sign: -C1 * x * coefficient 3).
    splats.sh[(3 * 3) + 0] = 1.0F;
    const auto flat = render::detail::splat_color(splats, 0, 0, {-1.0F, 0.0F, 0.0F});
    CHECK(flat[0] == doctest::Approx(0.5F));
    CHECK(flat[1] == doctest::Approx(0.25F));
    CHECK(flat[2] == doctest::Approx(0.0F)); // clamped at 0
    const auto left = render::detail::splat_color(splats, 0, 1, {-1.0F, 0.0F, 0.0F});
    const auto right = render::detail::splat_color(splats, 0, 1, {1.0F, 0.0F, 0.0F});
    CHECK(left[0] == doctest::Approx(0.5F + 0.4886025F));
    CHECK(right[0] == doctest::Approx(0.5F - 0.4886025F));
    CHECK(left[1] == doctest::Approx(0.25F));
    // The camera's place comes from its view matrix.
    const std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -3, 1};
    CHECK(render::detail::camera_place(view) == std::array<float, 3>{1.0F, 2.0F, 3.0F});
}

TEST_CASE("splats blend front to back over the frame, behind what the meshes drew") {
    constexpr std::uint32_t kSize = 32;
    render::GaussianSplats one;
    add_splat(one, {0.0F, 0.0F, -5.0F}, 0.5F, {1.0F, 0.5F, 0.25F});
    const std::vector<float> nothing(std::size_t{kSize} * kSize, 0.0F);
    const std::vector<float> blend =
        render::detail::run_splats(cloud(one), frame(kSize), nothing, nullptr);
    // Its center is between pixels 15 and 16 both ways: they're the same, nearly opaque, and of
    // its color made linear.
    const auto center = pixel(blend, kSize, 15, 15);
    CHECK(center == pixel(blend, kSize, 16, 16));
    CHECK(center == pixel(blend, kSize, 15, 16));
    CHECK(center[3] < 0.05F);
    CHECK(center[0] == doctest::Approx((1.0F - center[3]) * 1.0F).epsilon(1e-4));
    CHECK(center[1] ==
          doctest::Approx((1.0F - center[3]) * render::detail::srgb_to_linear_exact(0.5F))
              .epsilon(1e-4));
    // It fades out from there, and leaves the corners alone.
    CHECK(pixel(blend, kSize, 18, 15)[3] > center[3]);
    CHECK(pixel(blend, kSize, 0, 0) == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F});

    // A blue splat behind it shows only around it; in front, it hides the red one.
    render::GaussianSplats two = one;
    add_splat(two, {0.0F, 0.0F, -6.0F}, 0.4F, {0.0F, 0.0F, 1.0F});
    const auto behind = pixel(
        render::detail::run_splats(cloud(two), frame(kSize), nothing, nullptr), kSize, 15, 15);
    CHECK(behind[0] > 0.9F);
    CHECK(behind[2] < 0.1F);
    two.positions[1] = {0.0F, 0.0F, -4.0F};
    const auto front = pixel(render::detail::run_splats(cloud(two), frame(kSize), nothing, nullptr),
                             kSize, 15, 15);
    CHECK(front[0] < 0.05F);
    CHECK(front[2] > 0.9F);

    // A mesh nearer than the splat's center at a pixel keeps it out there.
    std::vector<float> wall(std::size_t{kSize} * kSize, 0.0F);
    wall[(std::size_t{15} * kSize) + 15] = 0.5F; // reversed depth: nearer than 5 m
    const std::vector<float> walled =
        render::detail::run_splats(cloud(one), frame(kSize), wall, nullptr);
    CHECK(pixel(walled, kSize, 15, 15) == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F});
    CHECK(pixel(walled, kSize, 16, 16) == center);

    // A cloud's transform places it: moved a meter right, it is drawn some 5.5 pixels right.
    const std::vector<float> moved =
        render::detail::run_splats(cloud(one, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.0F, 0, 0, 1}),
                                   frame(kSize), nothing, nullptr);
    CHECK(pixel(moved, kSize, 15, 15)[3] > 0.5F);
    CHECK(pixel(moved, kSize, 21, 15)[3] < 0.1F);

    // Behind the camera, or with no splats, nothing is drawn.
    render::GaussianSplats back;
    add_splat(back, {0.0F, 0.0F, 5.0F}, 0.2F, {1.0F, 1.0F, 1.0F});
    const std::vector<float> none =
        render::detail::run_splats(cloud(back), frame(kSize), nothing, nullptr);
    CHECK(pixel(none, kSize, 15, 15) == std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F});
}

TEST_CASE("a frame's splats are the same on any number of threads") {
    constexpr std::uint32_t kSize = 48;
    render::GaussianSplats many;
    many.sh_degree = 2;
    std::uint32_t seed = 7;
    const auto next = [&]() {
        seed = (seed * 1664525U) + 1013904223U;
        return static_cast<float>(seed >> 8) / static_cast<float>(1U << 24);
    };
    for (int i = 0; i < 300; ++i) {
        add_splat(many, {(next() * 4.0F) - 2.0F, (next() * 4.0F) - 2.0F, -3.0F - (next() * 4.0F)},
                  0.05F + (next() * 0.2F), {next(), next(), next()}, next());
        const std::size_t base = static_cast<std::size_t>(i) * 9 * 3;
        for (std::size_t k = 3; k < 27; ++k) {
            many.sh[base + k] = next() - 0.5F;
        }
        many.rotations.back() = {next(), next() - 0.5F, next() - 0.5F, next() - 0.5F};
        const auto& q = many.rotations.back();
        const float length =
            std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
        for (float& c : many.rotations.back()) {
            c /= length;
        }
        many.scales.back()[1] *= 3.0F;
    }
    const std::vector<float> depth(std::size_t{kSize} * kSize, 0.0F);
    const std::vector<float> alone =
        render::detail::run_splats(cloud(many), frame(kSize), depth, nullptr);
    slash::jobs::Pool pool(4);
    const std::vector<float> shared =
        render::detail::run_splats(cloud(many), frame(kSize), depth, &pool);
    CHECK(std::memcmp(alone.data(), shared.data(), alone.size() * sizeof(float)) == 0);
    std::size_t covered = 0;
    for (std::size_t at = 0; at < alone.size(); at += 4) {
        covered += alone[at + 3] < 0.5F ? 1U : 0U;
    }
    CHECK(covered > 100);
}

TEST_CASE("GaussianSplat3Ds name .ply files that read as splats") {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    world::World w(registry);
    render::GaussianSplats splats;
    add_splat(splats, {0.0F, 0.0F, 0.0F}, 0.1F, {1.0F, 1.0F, 1.0F});
    const std::string good = render::write_splat_ply(splats);
    w.set_file_reader([&](std::string_view path) -> slash::Result<std::string> {
        if (path == "rock.ply") {
            return good;
        }
        if (path == "bad.ply") {
            return std::string("ply\nformat ascii 1.0\nend_header\n");
        }
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    });
    const auto add = [&](const char* id, world::GaussianSplat3D cloud) {
        const entt::entity entity = w.create(id).value();
        w.ecs().emplace<world::GaussianSplat3D>(entity, std::move(cloud));
    };
    add("rock", {.splats = "rock.ply"});
    add("empty", {});
    add("broken", {.splats = "bad.ply"});
    add("missing", {.splats = "gone.ply"});
    add("mesh", {.splats = "rock.glb", .sh_degree = 4});
    std::vector<std::string> found;
    for (const scene3d::Problem& problem : scene3d::check_splats(w)) {
        found.push_back(std::string(w.id_of(problem.entity)) + " " + problem.path + ": " +
                        problem.message);
    }
    CHECK(found ==
          std::vector<std::string>{
              "broken /components/GaussianSplat3D/splats: \"bad.ply\": the .ply file has no "
              "vertex element",
              "missing /components/GaussianSplat3D/splats: \"gone.ply\" could not be read: no "
              "such file",
              "mesh /components/GaussianSplat3D/sh_degree: is 4, not 0 to 3",
              "mesh /components/GaussianSplat3D/splats: \"rock.glb\" is no .ply file of Gaussian "
              "splats"});
}
