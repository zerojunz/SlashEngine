#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace jobs = slash::jobs;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// A world with the built-in components.
struct Scene {
    world::ComponentRegistry components;
    world::World world{components};

    Scene() { world::register_builtin_components(components); }

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

// A floor of 4 by 4 meters with a box of 1 meter on it, lit from above at a slant by a sun whose
// light is baked, under a grey sky color, and a LightmapGI baking them with few rays.
struct Room : Scene {
    entt::entity gi = entt::null;

    explicit Room(std::string_view quality = "low") {
        create("floor", R"({"Transform3D": {}, "MeshInstance3D": {},
            "PlaneMesh": {"size": [4, 4]}})");
        create("box", R"({"Transform3D": {"position": [0, 0.5, 0]}, "MeshInstance3D": {},
            "BoxMesh": {}, "StandardMaterial3D": {"albedo_color": [1, 0.2, 0.2, 1]}})");
        create("sun", R"({"Transform3D": {"rotation": [-1.0, 0.6, 0]},
            "DirectionalLight3D": {"light_bake_mode": "static"}})");
        gi = create("gi", std::string(R"({"LightmapGI": {"quality": ")") + std::string(quality) +
                              R"(", "bounces": 1, "environment_mode": "custom_color",
            "environment_custom_color": [0.2, 0.2, 0.2, 1]}})");
    }
};

// A texel's light, by where it is in a user's rectangle of the baked texture.
std::array<float, 3> light_at(const scene3d::LightmapBake& bake, std::string_view entity,
                              const std::array<float, 2>& uv2) {
    for (const scene3d::LightmapUser& user : bake.data.users) {
        if (user.entity != entity) {
            continue;
        }
        const float u = user.uv_rect[0] + (uv2[0] * user.uv_rect[2]);
        const float v = user.uv_rect[1] + (uv2[1] * user.uv_rect[3]);
        const auto x = std::min(static_cast<std::uint32_t>(u * static_cast<float>(bake.data.width)),
                                bake.data.width - 1);
        const auto y =
            std::min(static_cast<std::uint32_t>(v * static_cast<float>(bake.data.height)),
                     bake.data.height - 1) +
            (user.slice * bake.data.height);
        const std::size_t at = ((std::size_t{y} * bake.texels.width) + x) * 3;
        return {bake.texels.rgb[at], bake.texels.rgb[at + 1], bake.texels.rgb[at + 2]};
    }
    FAIL("no user " << entity);
    return {};
}

// Where on the floor's lightmap a point of the floor is: the UV2 of its unwrap's triangle there.
std::array<float, 2> floor_uv2(const scene3d::LightmapBake& bake, float x, float z) {
    world::PlaneMesh floor;
    floor.size = {4.0F, 4.0F};
    const scene3d::MeshData plane = scene3d::plane_mesh(floor);
    const scene3d::LightmapUnwrap& unwrap =
        bake.data.meshes.at(scene3d::lightmap_mesh_hash(plane.vertices, plane.indices));
    for (std::size_t i = 0; i + 2 < unwrap.indices.size(); i += 3) {
        std::array<std::array<float, 2>, 3> p{};
        std::array<std::array<float, 2>, 3> uv{};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t v = unwrap.indices[i + k];
            const render::Vertex3D& source = plane.vertices[unwrap.xref[v]];
            p.at(k) = {source.position[0], source.position[2]};
            uv.at(k) = unwrap.uv2[v];
        }
        const float d = ((p[1][1] - p[2][1]) * (p[0][0] - p[2][0])) +
                        ((p[2][0] - p[1][0]) * (p[0][1] - p[2][1]));
        const float a =
            (((p[1][1] - p[2][1]) * (x - p[2][0])) + ((p[2][0] - p[1][0]) * (z - p[2][1]))) / d;
        const float b =
            (((p[2][1] - p[0][1]) * (x - p[2][0])) + ((p[0][0] - p[2][0]) * (z - p[2][1]))) / d;
        const float c = 1.0F - a - b;
        if (a >= 0.0F && b >= 0.0F && c >= 0.0F) {
            return {(a * uv[0][0]) + (b * uv[1][0]) + (c * uv[2][0]),
                    (a * uv[0][1]) + (b * uv[1][1]) + (c * uv[2][1])};
        }
    }
    FAIL("no floor at " << x << ", " << z);
    return {};
}

float brightness(const std::array<float, 3>& rgb) {
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0F;
}

} // namespace

TEST_CASE("half floats and Radiance HDR files keep a lightmap's light") {
    CHECK(render::float_to_half(1.0F) == 0x3C00);
    CHECK(render::float_to_half(-2.0F) == 0xC000);
    CHECK(render::float_to_half(65504.0F) == 0x7BFF);
    CHECK(render::float_to_half(70000.0F) == 0x7C00);
    CHECK(render::float_to_half(5.9604645e-8F) == 0x0001);
    // Ties go to the even half.
    CHECK(render::float_to_half(1.0F + (1.0F / 2048.0F)) == 0x3C00);
    CHECK(render::float_to_half(1.0F + (3.0F / 2048.0F)) == 0x3C02);
    for (std::uint32_t bits = 0; bits < 0x7C00; ++bits) {
        CHECK(render::float_to_half(render::half_to_float(static_cast<std::uint16_t>(bits))) ==
              bits);
    }
    CHECK(std::isnan(render::half_to_float(render::float_to_half(std::nanf("")))));

    render::HdrImage image{.width = 3, .height = 2, .rgb = {}};
    image.rgb = {0.0F, 0.5F, 1.0F, 2.0F, 100.0F, 0.001F, -1.0F, 0.25F, 3.0F,
                 7.5F, 7.5F, 7.5F, 0.0F, 0.0F,   0.0F,   1e-3F, 1e3F,  1.0F};
    const std::string bytes = render::encode_hdr(image).value();
    CHECK(bytes.starts_with("#?RADIANCE\n"));
    const render::HdrImage read = render::decode_hdr(bytes).value();
    REQUIRE(read.width == 3);
    REQUIRE(read.height == 2);
    for (std::size_t i = 0; i < image.rgb.size(); ++i) {
        CAPTURE(i);
        const float want = std::max(image.rgb[i], 0.0F);
        // RGBE keeps 8 bits of each color under the brightest's exponent.
        const std::size_t first = i - (i % 3);
        const float largest =
            std::max({image.rgb[first], image.rgb[first + 1], image.rgb[first + 2]});
        CHECK(std::abs(read.rgb[i] - want) <= largest / 128.0F);
    }
    // Encoding what was decoded gives the same bytes.
    CHECK(render::encode_hdr(read).value() == bytes);
    CHECK_FALSE(render::decode_hdr("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n").ok());
}

TEST_CASE("meshes unwrap into lightmap UVs as Godot's lightmap_unwrap, the same every time") {
    const scene3d::MeshData box = scene3d::box_mesh({});
    const scene3d::LightmapUnwrap unwrap =
        scene3d::unwrap_lightmap(box.vertices, box.indices).value();
    // Six faces of 5 by 5 texels at 0.2 meters a texel, a texel apart.
    CHECK(unwrap.width > 10);
    CHECK(unwrap.height > 10);
    CHECK(unwrap.width < 40);
    CHECK(unwrap.indices.size() == box.indices.size());
    REQUIRE(unwrap.xref.size() == unwrap.uv2.size());
    for (std::size_t i = 0; i < unwrap.xref.size(); ++i) {
        CHECK(unwrap.xref[i] < box.vertices.size());
        CHECK(unwrap.uv2[i][0] >= 0.0F);
        CHECK(unwrap.uv2[i][0] <= 1.0F);
        CHECK(unwrap.uv2[i][1] >= 0.0F);
        CHECK(unwrap.uv2[i][1] <= 1.0F);
    }
    // Each triangle is the source's, by its corners' source vertices.
    for (std::size_t i = 0; i < box.indices.size(); ++i) {
        CHECK(unwrap.xref[unwrap.indices[i]] == box.indices[i]);
    }
    CHECK(scene3d::unwrap_lightmap(box.vertices, box.indices).value() == unwrap);
    // Smaller texels make more of them.
    const scene3d::LightmapUnwrap fine =
        scene3d::unwrap_lightmap(box.vertices, box.indices, 0.05F).value();
    CHECK(fine.width > unwrap.width * 3);

    const std::vector<render::Vertex3D> flat(3);
    const std::vector<std::uint32_t> triangle{0, 1, 2};
    CHECK(scene3d::unwrap_lightmap(flat, triangle).error().message ==
          "the mesh has no triangle of any area to unwrap");
    CHECK(scene3d::unwrap_lightmap(box.vertices, {}).error().message ==
          "the mesh has no triangles to unwrap");

    const std::string hash = scene3d::lightmap_mesh_hash(box.vertices, box.indices);
    CHECK(hash.size() == 16);
    CHECK(hash.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(scene3d::lightmap_mesh_hash(box.vertices, box.indices) == hash);
    const scene3d::MeshData bigger = scene3d::box_mesh({.size = {2.0F, 1.0F, 1.0F}});
    CHECK(scene3d::lightmap_mesh_hash(bigger.vertices, bigger.indices) != hash);
}

TEST_CASE(".lightmap.json files keep unwraps and users, and say what's wrong with them") {
    const scene3d::MeshData box = scene3d::box_mesh({});
    scene3d::LightmapData data;
    data.texture = "level.lightmap.hdr";
    data.width = 64;
    data.height = 32;
    data.slices = 2;
    data.baked_exposure = 0.5F;
    const std::string hash = scene3d::lightmap_mesh_hash(box.vertices, box.indices);
    data.meshes.emplace(hash, scene3d::unwrap_lightmap(box.vertices, box.indices).value());
    data.users.push_back({.entity = "box",
                          .part = 0,
                          .mesh = hash,
                          .slice = 1,
                          .uv_rect = {0.25F, 0.0F, 0.5F, 0.75F}});
    const json::Value doc = scene3d::lightmap_json(data);
    CHECK(scene3d::validate_lightmap(doc).empty());
    CHECK(scene3d::parse_lightmap(doc).value() == data);
    CHECK(doc.find("users")->as_array()[0].find("part") == nullptr);
    // What it writes reads back the same.
    CHECK(scene3d::parse_lightmap(parse(json::write(doc, json::Style::Compact))).value() == data);

    json::Value bad = doc;
    bad["users"].as_array()[0]["mesh"] = "0000000000000000";
    bad["users"].as_array()[0]["slice"] = 2;
    bad["users"].as_array()[0]["uv_rect"] = json::Array{0.75, 0.0, 0.5, 0.5};
    bad["meshes"][hash]["indices"] = "AAAAAAAAAAAAAAAAAAAAAA=="; // four zeros
    const auto errors = scene3d::validate_lightmap(bad);
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].path == "/meshes/" + hash + "/indices");
    CHECK(errors[0].message == "expected triangles: a multiple of 3 indices");
    CHECK(errors[1].path == "/users/0/mesh");
    CHECK(errors[2].path == "/users/0/slice");
    CHECK(errors[2].message == "is past the last of 2 slices");
    CHECK(errors[3].path == "/users/0/uv_rect");
    CHECK(scene3d::parse_lightmap(bad).error().message ==
          "/meshes/" + hash + "/indices: expected triangles: a multiple of 3 indices");
    CHECK(scene3d::validate_lightmap(parse(R"({"format": 1})")).size() == 4);

    CHECK(scene3d::lightmap_texture_path("levels/one.lightmap.json") == "levels/one.lightmap.hdr");
    CHECK(scene3d::lightmap_texture_path("one.json") == "one.json.hdr");
}

TEST_CASE("a LightmapGI bakes its static meshes' light, shadows and bounces, as Godot's") {
    Room room;
    const scene3d::LightmapBake bake = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    CHECK(bake.problems.empty());
    CHECK(bake.meshes == 2);
    CHECK(bake.lights == 1);
    REQUIRE(bake.data.users.size() == 2);
    CHECK(bake.data.users[0].entity == "floor");
    CHECK(bake.data.users[1].entity == "box");
    CHECK(bake.data.meshes.size() == 2);
    // Two slices of the smallest square hold the two as cheaply as one slice twice as wide, and
    // Godot keeps the first it tried.
    CHECK(bake.data.slices == 2);
    CHECK(bake.texels.width == bake.data.width);
    CHECK(bake.texels.height == bake.data.height * bake.data.slices);
    for (const float value : bake.texels.rgb) {
        REQUIRE(std::isfinite(value));
        REQUIRE(value >= 0.0F);
    }
    // The sun shines down at a slant, toward -x and -z: the floor beside the box is lit, the
    // floor in its shadow takes only the sky's light and the bounces.
    const float lit = brightness(light_at(bake, "floor", floor_uv2(bake, -1.5F, -1.5F)));
    const float shadowed = brightness(light_at(bake, "floor", floor_uv2(bake, -0.6F, -0.7F)));
    CAPTURE(lit);
    CAPTURE(shadowed);
    CHECK(lit > 0.5F);
    CHECK(shadowed < lit * 0.5F);
    CHECK(shadowed > 0.0F);
}

TEST_CASE("bakes are the same for any number of threads") {
    Room room;
    const scene3d::LightmapBake bake = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    for (const int threads : {0, 3}) {
        CAPTURE(threads);
        jobs::Pool pool(threads);
        Room again;
        again.world.set_pool(pool);
        const scene3d::LightmapBake other =
            scene3d::bake_lightmap(again.world, again.gi, {}).value();
        CHECK(other.data == bake.data);
        CHECK(other.texels == bake.texels);
    }
}

namespace {

using Point = std::array<float, 3>;

// A repeatable stream of numbers from 0 up to 1.
struct Numbers {
    std::uint32_t state = 12345;

    float next() {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<float>(state >> 8U) / 16777216.0F;
    }
};

// Six times a tetrahedron's volume, signed.
double volume6(std::span<const Point> points, const std::array<std::uint32_t, 4>& t) {
    std::array<std::array<double, 3>, 3> e{};
    for (std::size_t k = 0; k < 3; ++k) {
        for (std::size_t c = 0; c < 3; ++c) {
            e.at(k).at(c) = static_cast<double>(points[t.at(k + 1)].at(c)) -
                            static_cast<double>(points[t[0]].at(c));
        }
    }
    return (e[0][0] * ((e[1][1] * e[2][2]) - (e[1][2] * e[2][1]))) -
           (e[0][1] * ((e[1][0] * e[2][2]) - (e[1][2] * e[2][0]))) +
           (e[0][2] * ((e[1][0] * e[2][1]) - (e[1][1] * e[2][0])));
}

// The corners of a box, then its middle.
std::vector<Point> box_points(const Point& low, const Point& high) {
    std::vector<Point> points;
    for (int i = 0; i < 8; ++i) {
        points.push_back({(i & 1) != 0 ? high[0] : low[0], (i & 2) != 0 ? high[1] : low[1],
                          (i & 4) != 0 ? high[2] : low[2]});
    }
    points.push_back(
        {(low[0] + high[0]) * 0.5F, (low[1] + high[1]) * 0.5F, (low[2] + high[2]) * 0.5F});
    return points;
}

// Probes at points between low and high, each lit from every direction by light at its place.
scene3d::LightmapProbes probes_at(std::vector<Point> points, const Point& low, const Point& high,
                                  const std::function<std::array<float, 3>(const Point&)>& light) {
    scene3d::LightmapProbes probes;
    probes.bounds_position = low;
    probes.bounds_size = {high[0] - low[0], high[1] - low[1], high[2] - low[2]};
    for (const Point& point : points) {
        scene3d::LightmapSh sh{};
        sh[0] = light(point);
        probes.sh.push_back(sh);
    }
    probes.tetrahedra = scene3d::tetrahedralize(points);
    probes.bsp = scene3d::lightmap_bsp_tree(points, probes.tetrahedra);
    probes.points = std::move(points);
    return probes;
}

// Little-endian 32-bit words as base64, as .lightmap.json files keep them.
std::string words(std::initializer_list<std::uint32_t> values) {
    std::string bytes;
    for (const std::uint32_t value : values) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<char>((value >> (8U * i)) & 0xFFU));
        }
    }
    return slash::base64_encode(bytes);
}

// The light along a direction from spherical harmonics in Godot's order.
float sh_value(const scene3d::LightmapSh& sh, std::size_t c, const Point& n) {
    return (0.282095F * sh[0].at(c)) +
           (0.488603F * ((sh[1].at(c) * n[1]) + (sh[2].at(c) * n[2]) + (sh[3].at(c) * n[0]))) +
           (1.092548F * ((sh[4].at(c) * n[0] * n[1]) + (sh[5].at(c) * n[1] * n[2]) +
                         (sh[7].at(c) * n[0] * n[2]))) +
           (0.315392F * sh[6].at(c) * ((3.0F * n[2] * n[2]) - 1.0F)) +
           (0.546274F * sh[8].at(c) * ((n[0] * n[0]) - (n[1] * n[1])));
}

} // namespace

TEST_CASE("light probes go where Godot's LightmapGI places them") {
    // A floor of 4 by 4 meters, and a LightmapProbe in a doorway above it.
    const std::vector<Point> floor{{-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F},
                                   {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F},  {-2.0F, 0.0F, 2.0F}};
    const std::vector<Point> doorway{{0.5F, 1.0F, 0.5F}};
    const auto inside = [](const scene3d::LightmapProbes& probes, const Point& p) {
        for (std::size_t c = 0; c < 3; ++c) {
            if (p.at(c) < probes.bounds_position.at(c) - 1e-4F ||
                p.at(c) > probes.bounds_position.at(c) + probes.bounds_size.at(c) + 1e-4F) {
                return false;
            }
        }
        return true;
    };

    // With no octree, the LightmapProbe first and then the bounds' corners, the bounds grown a
    // little around the floor and the LightmapProbe.
    const scene3d::LightmapProbes corners = scene3d::place_lightmap_probes(floor, doorway, 0);
    REQUIRE(corners.points.size() == 9);
    CHECK(corners.points[0] == doorway[0]);
    CHECK(corners.bounds_position[0] < -2.0F);
    CHECK(corners.bounds_position[0] > -2.01F);
    CHECK(corners.bounds_position[1] < 0.0F);
    CHECK(corners.bounds_size[1] > 1.0F);
    CHECK(corners.points[1] == corners.bounds_position);
    for (const Point& p : corners.points) {
        CHECK(inside(corners, p));
    }

    // An octree of 4 cells along the floor puts probes on the corners of the cells the floor
    // touches, none of them within half a cell of the LightmapProbe or twice in one place.
    const scene3d::LightmapProbes octree = scene3d::place_lightmap_probes(floor, doorway, 4);
    const float cell = octree.bounds_size[0] / 4.0F;
    CHECK(octree.bounds_size[2] == doctest::Approx(cell * 4.0F));
    CHECK(octree.bounds_size[1] == doctest::Approx(cell * 2.0F)); // grown to whole cells
    REQUIRE(octree.points.size() > 9);
    CHECK(octree.points[0] == doorway[0]);
    std::set<Point> seen;
    for (std::size_t i = 0; i < octree.points.size(); ++i) {
        CAPTURE(i);
        const Point& p = octree.points[i];
        CHECK(inside(octree, p));
        CHECK(seen.insert(p).second);
        if (i >= 9) {
            const float dx = p[0] - doorway[0][0];
            const float dy = p[1] - doorway[0][1];
            const float dz = p[2] - doorway[0][2];
            CHECK(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) >= cell * 0.5F);
            // On a corner of the cells.
            const float row = (p[1] - octree.bounds_position[1]) / cell;
            CHECK(std::abs(row - std::round(row)) < 1e-3F);
        }
    }
    CHECK(scene3d::place_lightmap_probes(floor, doorway, 4) == octree);
    // More cells, more probes.
    CHECK(scene3d::place_lightmap_probes(floor, doorway, 8).points.size() > octree.points.size());
}

TEST_CASE("light probes are joined into Delaunay tetrahedra, the same every time") {
    // A cube's corners and middle fill it with tetrahedra.
    const std::vector<Point> cube = box_points({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F});
    const auto tetrahedra = scene3d::tetrahedralize(cube);
    REQUIRE_FALSE(tetrahedra.empty());
    double volume = 0.0;
    for (const auto& t : tetrahedra) {
        for (const std::uint32_t corner : t) {
            REQUIRE(corner < cube.size());
        }
        const double v = std::abs(volume6(cube, t)) / 6.0;
        CHECK(v > 1e-6);
        volume += v;
    }
    CHECK(volume == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(scene3d::tetrahedralize(cube) == tetrahedra);
    CHECK(scene3d::tetrahedralize(std::span(cube).first(3)).empty());

    // Points scattered in a box: no tetrahedron's sphere holds another point.
    std::vector<Point> points = box_points({-3.0F, 0.0F, -2.0F}, {3.0F, 2.0F, 2.0F});
    Numbers numbers;
    for (int i = 0; i < 60; ++i) {
        points.push_back({-3.0F + (6.0F * numbers.next()), 2.0F * numbers.next(),
                          -2.0F + (4.0F * numbers.next())});
    }
    const auto scattered = scene3d::tetrahedralize(points);
    REQUIRE(scattered.size() > 60);
    volume = 0.0;
    for (const auto& t : scattered) {
        volume += std::abs(volume6(points, t)) / 6.0;
        // The sphere through its corners, where |c - p|² is the same for all four.
        std::array<std::array<double, 4>, 3> rows{};
        const auto at = [&](std::uint32_t i, std::size_t c) {
            return static_cast<double>(points[i].at(c));
        };
        for (std::size_t k = 0; k < 3; ++k) {
            double right = 0.0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double d = at(t.at(k + 1), c) - at(t[0], c);
                rows.at(k).at(c) = 2.0 * d;
                right += (at(t.at(k + 1), c) * at(t.at(k + 1), c)) - (at(t[0], c) * at(t[0], c));
            }
            rows.at(k)[3] = right;
        }
        const auto det = [](const std::array<std::array<double, 3>, 3>& m) {
            return (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) -
                   (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
                   (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
        };
        std::array<std::array<double, 3>, 3> m{};
        for (std::size_t k = 0; k < 3; ++k) {
            m.at(k) = {rows.at(k)[0], rows.at(k)[1], rows.at(k)[2]};
        }
        const double d = det(m);
        std::array<double, 3> center{};
        for (std::size_t c = 0; c < 3; ++c) {
            std::array<std::array<double, 3>, 3> mc = m;
            for (std::size_t k = 0; k < 3; ++k) {
                mc.at(k).at(c) = rows.at(k)[3];
            }
            center.at(c) = det(mc) / d;
        }
        const auto distance2 = [&](std::uint32_t i) {
            double sum = 0.0;
            for (std::size_t c = 0; c < 3; ++c) {
                sum += (at(i, c) - center.at(c)) * (at(i, c) - center.at(c));
            }
            return sum;
        };
        const double radius2 = distance2(t[0]);
        for (std::uint32_t i = 0; i < points.size(); ++i) {
            if (std::ranges::find(t, i) == t.end()) {
                REQUIRE(distance2(i) > radius2 * (1.0 - 1e-3));
            }
        }
    }
    // Tetrahedra touching the starting tetrahedron's corners go, as in Godot, which can leave
    // slivers by the hull empty.
    CHECK(volume <= 48.001);
    CHECK(volume > 47.9);
}

TEST_CASE("light probes over a bare floor fill their bounds with tetrahedra that don't overlap") {
    // A floor's probes lie in two flat layers of a grid, where every cell's corners share a
    // sphere: Godot's tolerant circumsphere test lets tetrahedra overlap there (twice the bounds'
    // volume, and a BSP tree that ran for minutes); the exact test doesn't.
    const std::vector<Point> floor{{-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F},
                                   {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F},  {-2.0F, 0.0F, 2.0F}};
    for (const int subdiv : {8, 16}) {
        CAPTURE(subdiv);
        const scene3d::LightmapProbes placed = scene3d::place_lightmap_probes(floor, {}, subdiv);
        const Point low = placed.bounds_position;
        const Point high{low[0] + placed.bounds_size[0], low[1] + placed.bounds_size[1],
                         low[2] + placed.bounds_size[2]};
        const scene3d::LightmapProbes probes =
            probes_at(placed.points, low, high, [](const Point& p) { return p; });
        double volume = 0.0;
        for (const auto& t : probes.tetrahedra) {
            const double v = std::abs(volume6(probes.points, t)) / 6.0;
            CHECK(v > 1e-6);
            volume += v;
        }
        const double bounds = static_cast<double>(placed.bounds_size[0]) *
                              static_cast<double>(placed.bounds_size[1]) *
                              static_cast<double>(placed.bounds_size[2]);
        CHECK(volume == doctest::Approx(bounds).epsilon(1e-4));
        CHECK(probes.bsp.size() < 2 * probes.tetrahedra.size());
        Numbers numbers;
        for (int i = 0; i < 100; ++i) {
            const Point p{low[0] + (placed.bounds_size[0] * numbers.next()),
                          low[1] + (placed.bounds_size[1] * numbers.next()),
                          low[2] + (placed.bounds_size[2] * numbers.next())};
            const scene3d::LightmapSh sh = scene3d::tap_lightmap_probes(probes, p);
            for (std::size_t c = 0; c < 3; ++c) {
                CHECK(sh[0].at(c) == doctest::Approx(p.at(c)).epsilon(1e-3).scale(1.0));
            }
        }
    }
}

TEST_CASE("a BSP tree finds the tetrahedron of probes around a point, whose light blends") {
    // Probes whose light is their place: blending by the point's barycentric coordinates gives
    // back the place of any point among them.
    const Point low{-3.0F, 0.0F, -2.0F};
    const Point high{3.0F, 2.0F, 2.0F};
    std::vector<Point> points = box_points(low, high);
    Numbers numbers;
    for (int i = 0; i < 40; ++i) {
        points.push_back({-3.0F + (6.0F * numbers.next()), 2.0F * numbers.next(),
                          -2.0F + (4.0F * numbers.next())});
    }
    const scene3d::LightmapProbes probes =
        probes_at(points, low, high, [](const Point& p) { return p; });
    REQUIRE(probes.bsp.size() > 1);
    for (std::size_t i = 0; i < probes.bsp.size(); ++i) {
        for (const std::int32_t side : {probes.bsp[i].over, probes.bsp[i].under}) {
            CHECK((side < 0 || static_cast<std::size_t>(side) > i));
        }
    }
    for (int i = 0; i < 200; ++i) {
        const Point p{-2.9F + (5.8F * numbers.next()), 0.05F + (1.9F * numbers.next()),
                      -1.9F + (3.8F * numbers.next())};
        CAPTURE(p[0]);
        CAPTURE(p[1]);
        CAPTURE(p[2]);
        const scene3d::LightmapSh sh = scene3d::tap_lightmap_probes(probes, p);
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(sh[0].at(c) == doctest::Approx(p.at(c)).epsilon(1e-3).scale(1.0));
        }
        CHECK(sh[1] == std::array<float, 3>{0.0F, 0.0F, 0.0F});
    }
    // The same tree on any number of threads; none for one tetrahedron.
    jobs::Pool pool(3);
    CHECK(scene3d::lightmap_bsp_tree(probes.points, probes.tetrahedra, &pool) == probes.bsp);
    CHECK(scene3d::lightmap_bsp_tree(probes.points, std::span(probes.tetrahedra).first(1)).empty());
}

TEST_CASE("light probes' light turns with their LightmapGI, and blends where meshes are") {
    // Turned light, looked at along a turned direction, is the light along the direction.
    scene3d::LightmapSh sh{};
    Numbers numbers;
    for (auto& coefficient : sh) {
        for (float& c : coefficient) {
            c = numbers.next() - 0.5F;
        }
    }
    world::Transform3D placed;
    placed.rotation = {0.4F, -1.1F, 0.3F};
    placed.scale = {2.0F, 2.0F, 2.0F}; // only its axes count
    const world::Affine3D turn = world::affine(placed);
    scene3d::LightmapSh turned = sh;
    scene3d::rotate_sh(turned, turn);
    for (int i = 0; i < 20; ++i) {
        Point d{numbers.next() - 0.5F, numbers.next() - 0.5F, numbers.next() - 0.5F};
        const float length = std::sqrt((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
        for (float& c : d) {
            c /= length;
        }
        const world::Affine3D unit = world::orthonormalized(turn);
        const slash::Vec3 t = unit.basis({d[0], d[1], d[2]});
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(sh_value(turned, c, {t.x, t.y, t.z}) ==
                  doctest::Approx(sh_value(sh, c, d)).epsilon(1e-4).scale(1.0));
        }
    }

    // A LightmapGI turned half round y, 10 meters along x, whose probes are lit only from their
    // +x: a mesh in it is lit from the world's -x, times the exposure.
    const Point low{-2.0F, -2.0F, -2.0F};
    const Point high{2.0F, 2.0F, 2.0F};
    scene3d::LightmapProbes probes = probes_at(
        box_points(low, high), low, high, [](const Point&) { return Point{1.0F, 1.0F, 1.0F}; });
    for (scene3d::LightmapSh& light : probes.sh) {
        light[3] = {0.5F, 0.5F, 0.5F};
    }
    world::Transform3D gi;
    gi.position = {10.0F, 0.0F, 0.0F};
    gi.rotation = {0.0F, 3.14159265F, 0.0F};
    const std::array<scene3d::LightmapCaptureSource, 1> sources{
        {{.probes = &probes, .transform = world::affine(gi), .exposure = 2.0F}}};
    const std::optional<scene3d::LightmapSh> captured =
        scene3d::lightmap_capture(sources, {10.5F, 0.0F, 0.0F});
    REQUIRE(captured.has_value());
    CHECK((*captured)[0][0] == doctest::Approx(2.0F));
    CHECK((*captured)[3][0] == doctest::Approx(-1.0F));
    CHECK(std::abs((*captured)[2][0]) < 1e-5F);
    // Outside every lightmap's bounds, none.
    CHECK_FALSE(scene3d::lightmap_capture(sources, {0.0F, 0.0F, 0.0F}).has_value());

    // Two lightmaps blend by how far inside each the mesh is; an interior one stands in for the
    // others.
    scene3d::LightmapProbes dark = probes;
    for (scene3d::LightmapSh& light : dark.sh) {
        light = {};
    }
    const std::array<scene3d::LightmapCaptureSource, 2> both{
        {{.probes = &probes, .transform = {}, .exposure = 1.0F},
         {.probes = &dark, .transform = {}, .exposure = 1.0F}}};
    const float blended = scene3d::lightmap_capture(both, {0.5F, 0.0F, 0.0F})->at(0)[0];
    CHECK(blended == doctest::Approx(0.5F));
    dark.interior = true;
    CHECK(scene3d::lightmap_capture(both, {0.5F, 0.0F, 0.0F})->at(0)[0] == 0.0F);
}

TEST_CASE(".lightmap.json files keep light probes, and say what's wrong with them") {
    scene3d::LightmapData data;
    data.texture = "level.lightmap.hdr";
    data.width = 4;
    data.height = 4;
    data.slices = 1;
    CHECK(scene3d::lightmap_json(data).find("probes") == nullptr);
    const Point low{-1.0F, 0.0F, -1.0F};
    const Point high{1.0F, 2.0F, 1.0F};
    data.probes = probes_at(box_points(low, high), low, high, [](const Point& p) {
        return std::array<float, 3>{p[0] + 1.0F, p[1], 0.25F};
    });
    data.probes.sh[2][5] = {0.5F, -0.25F, 0.125F};
    data.probes.interior = true;
    REQUIRE(data.probes.bsp.size() > 1);
    const json::Value doc = scene3d::lightmap_json(data);
    CHECK(scene3d::validate_lightmap(doc).empty());
    CHECK(scene3d::parse_lightmap(doc).value() == data);
    CHECK(scene3d::parse_lightmap(parse(json::write(doc, json::Style::Compact))).value() == data);

    json::Value bad = doc;
    bad["probes"]["tetrahedra"] = words({0, 1, 2, 99});
    auto errors = scene3d::validate_lightmap(bad);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/probes/tetrahedra");
    CHECK(errors[0].message == "names probe 99 of 9");

    bad = doc;
    bad["probes"]["sh"] = words({0, 0, 0});
    errors = scene3d::validate_lightmap(bad);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/probes/sh");
    CHECK(errors[0].message == "expected base64 of 27 finite 32-bit floats for each of 9 probes");

    bad = doc;
    bad["probes"]["bounds"] = json::Array{0.0, 0.0, 0.0, 1.0, -1.0, 1.0};
    bad["probes"]["bsp"] = "AAAA";
    errors = scene3d::validate_lightmap(bad);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/probes/bounds/4");
    CHECK(errors[0].message == "is negative");
    CHECK(errors[1].path == "/probes/bsp");
    CHECK(errors[1].message == "isn't base64 of 24-byte nodes");
}

TEST_CASE("a LightmapGI bakes light probes over its static meshes, as Godot's") {
    Room room;
    room.create("doorway", R"({"Transform3D": {"position": [1, 1, 1]}, "LightmapProbe": {}})");
    const scene3d::LightmapBake bake = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    const scene3d::LightmapProbes& probes = bake.data.probes;
    REQUIRE(probes.points.size() > 9);
    CHECK(probes.points[0] == Point{1.0F, 1.0F, 1.0F});
    CHECK(probes.sh.size() == probes.points.size());
    CHECK(probes.tetrahedra.size() > probes.points.size());
    CHECK(probes.bsp.size() > 1);
    CHECK_FALSE(probes.interior);
    for (const scene3d::LightmapSh& sh : probes.sh) {
        for (const auto& coefficient : sh) {
            for (const float c : coefficient) {
                REQUIRE(std::isfinite(c));
            }
        }
        CHECK(sh[0][0] > 0.0F); // the sky's light at least
    }
    // Light comes up from the floor where the sun lights it more than from the floor in the box's
    // shadow.
    const Point down{0.0F, -1.0F, 0.0F};
    const float lit = sh_value(scene3d::tap_lightmap_probes(probes, {-1.5F, 0.2F, -1.5F}), 0, down);
    const float shadowed =
        sh_value(scene3d::tap_lightmap_probes(probes, {-0.6F, 0.2F, -0.7F}), 0, down);
    CAPTURE(lit);
    CAPTURE(shadowed);
    CHECK(lit > shadowed * 1.5F);

    // Without an octree, the bounds' corners only; an interior LightmapGI says so.
    auto& gi = room.world.ecs().get<world::LightmapGI>(room.gi);
    gi.generate_probes_subdiv = "disabled";
    gi.interior = true;
    const scene3d::LightmapBake corners = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    CHECK(corners.data.probes.points.size() == 9);
    CHECK(corners.data.probes.interior);
}

namespace {

// The room on a 160 by 90 screen, seen from 4 meters straight above, with its files in memory and
// the canvas installed as games do.
struct DrawnRoom {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
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
    }};
    Room room;

    DrawnRoom() {
        canvas::install(room.world, assets);
        slash::ui::set_viewport(room.world, {160.0F, 90.0F});
        room.create("camera", R"({"Transform3D": {"position": [0, 4, 0],
            "rotation": [-1.5707964, 0, 0]}, "Camera3D": {}})");
    }

    render::Image capture(double time = -1.0) {
        auto captured = canvas::capture(room.world, assets, {.time = time});
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        return std::move(captured).value().image;
    }
};

// How bright the floor's pixel is where a point of it shows: the camera looks down -y with +x to
// the right and -z up, about 14.7 pixels a meter at the floor.
float floor_pixel(const render::Image& image, float x, float z) {
    constexpr float kPixelsPerMeter = 45.0F / (4.0F * 0.76732698F); // tan(37.5 degrees)
    const auto px = static_cast<std::uint32_t>(80.0F + (x * kPixelsPerMeter));
    const auto py = static_cast<std::uint32_t>(45.0F + (z * kPixelsPerMeter));
    const std::size_t at = ((std::size_t{py} * image.width) + px) * 4;
    return static_cast<float>(image.pixels[at] + image.pixels[at + 1] + image.pixels[at + 2]) /
           (3.0F * 255.0F);
}

// The color of the pixel where a point y meters up shows.
std::array<int, 3> seen_pixel(const render::Image& image, float x, float y, float z) {
    const float pixels_per_meter = 45.0F / ((4.0F - y) * 0.76732698F);
    const auto px = static_cast<std::uint32_t>(80.0F + (x * pixels_per_meter));
    const auto py = static_cast<std::uint32_t>(45.0F + (z * pixels_per_meter));
    const std::size_t at = ((std::size_t{py} * image.width) + px) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2)};
}

} // namespace

TEST_CASE("lightmapped meshes draw their baked light in place of baked lights, as Godot's") {
    DrawnRoom drawn;
    // Unbaked, the sun lights the floor live and casts no shadow, since its shadow is off.
    const render::Image live = drawn.capture();
    const float live_lit = floor_pixel(live, -1.5F, -1.5F);
    const float live_shadowed = floor_pixel(live, -0.2F, -0.95F);
    CAPTURE(live_lit);
    CAPTURE(live_shadowed);
    CHECK(live_lit > 0.2F);
    CHECK(std::abs(live_shadowed - live_lit) < 0.05F);

    scene3d::LightmapBake bake =
        scene3d::bake_lightmap(drawn.room.world, drawn.room.gi, {}).value();
    bake.data.texture = "room.lightmap.hdr";
    drawn.files["room.lightmap.json"] = json::write(scene3d::lightmap_json(bake.data));
    drawn.files["room.lightmap.hdr"] = render::encode_hdr(bake.texels).value();
    drawn.room.world.ecs().get<world::LightmapGI>(drawn.room.gi).light_data = "room.lightmap.json";
    // Baked, the floor takes the sun from the lightmap, with the box's shadow in it, and the sun
    // no longer lights it live.
    const render::Image baked = drawn.capture();
    const float lit = floor_pixel(baked, -1.5F, -1.5F);
    const float shadowed = floor_pixel(baked, -0.2F, -0.95F);
    CAPTURE(lit);
    CAPTURE(shadowed);
    CHECK(lit > 0.2F);
    CHECK(shadowed < lit * 0.6F);
    CHECK(shadowed > 0.0F);

    // A mesh changed since the bake draws without its lightmap.
    drawn.room.world.ecs().get<world::PlaneMesh>(drawn.room.world.find("floor")).size = {4.0F,
                                                                                         4.5F};
    const render::Image changed = drawn.capture();
    CHECK(std::abs(floor_pixel(changed, -0.2F, -0.95F) - live_shadowed) < 0.05F);
}

TEST_CASE("moving meshes take their ambient light from a LightmapGI's probes, as Godot's") {
    DrawnRoom drawn;
    const entt::entity ball =
        drawn.room.create("ball", R"({"Transform3D": {"position": [1.2, 0.5, 1.2]},
        "MeshInstance3D": {"gi_mode": "dynamic"}, "SphereMesh": {"radius": 0.4, "height": 0.8}})");
    auto& place = drawn.room.world.ecs().get<world::Transform3D>(ball);
    const render::Image unbaked = drawn.capture();
    place.position = {1.2F, 2.0F, 1.2F};
    const render::Image unbaked_high = drawn.capture();

    // Baked, the ball isn't in the lightmap, but the probes' light lights it besides the sun.
    scene3d::LightmapBake bake =
        scene3d::bake_lightmap(drawn.room.world, drawn.room.gi, {}).value();
    CHECK(bake.meshes == 2);
    REQUIRE_FALSE(bake.data.probes.points.empty());
    bake.data.texture = "room.lightmap.hdr";
    drawn.files["room.lightmap.json"] = json::write(scene3d::lightmap_json(bake.data));
    drawn.files["room.lightmap.hdr"] = render::encode_hdr(bake.texels).value();
    auto& gi = drawn.room.world.ecs().get<world::LightmapGI>(drawn.room.gi);
    gi.light_data = "room.lightmap.json";
    place.position = {1.2F, 0.5F, 1.2F};
    const render::Image baked = drawn.capture();
    const std::array<int, 3> before = seen_pixel(unbaked, 1.2F, 0.9F, 1.2F);
    const std::array<int, 3> after = seen_pixel(baked, 1.2F, 0.9F, 1.2F);
    CAPTURE(before[0]);
    CAPTURE(after[0]);
    CHECK(after[0] > before[0] + 5);

    // Probes red on the left and blue on the right, with the sun off: the ball takes the color
    // where it is, eased toward it as it moves, 15 times the time a frame takes as Godot's update
    // speed.
    const Point low{-2.5F, -0.5F, -2.5F};
    const Point high{2.5F, 1.5F, 2.5F};
    bake.data.probes = probes_at(box_points(low, high), low, high, [&](const Point& p) {
        const float t = (p[0] - low[0]) / (high[0] - low[0]);
        return std::array<float, 3>{0.8F * (1.0F - t), 0.05F, 0.8F * t};
    });
    auto& sun = drawn.room.world.ecs().get<world::DirectionalLight3D>(drawn.room.world.find("sun"));
    sun.light_energy = 0.0F;
    drawn.files["colored.lightmap.json"] = json::write(scene3d::lightmap_json(bake.data));
    gi.light_data = "colored.lightmap.json";
    place.position = {-1.2F, 0.5F, -1.2F};
    const std::array<int, 3> left = seen_pixel(drawn.capture(5.0), -1.2F, 0.9F, -1.2F);
    place.position = {1.2F, 0.5F, 1.2F};
    const std::array<int, 3> moving =
        seen_pixel(drawn.capture(5.0 + (1.0 / 30.0)), 1.2F, 0.9F, 1.2F);
    const std::array<int, 3> right = seen_pixel(drawn.capture(6.0), 1.2F, 0.9F, 1.2F);
    CAPTURE(left[0] - left[2]);
    CAPTURE(moving[0] - moving[2]);
    CAPTURE(right[0] - right[2]);
    CHECK(left[0] > left[2] + 20);
    CHECK(right[2] > right[0] + 20);
    CHECK(std::abs(moving[0] - moving[2]) < 10);
    // Above the probes' bounds, the scene's ambient light again.
    sun.light_energy = 1.0F;
    place.position = {1.2F, 2.0F, 1.2F};
    CHECK(seen_pixel(drawn.capture(7.0), 1.2F, 2.4F, 1.2F) ==
          seen_pixel(unbaked_high, 1.2F, 2.4F, 1.2F));
}

namespace {

// A floor two meters across seen from above, lit by a 4 by 4 lightmap: warm in one corner, dim
// blue in the opposite one. The scene's ambient light and a baked sun leave it alone, and a
// live omni light by one edge still lights it. Its left half samples bicubically, its right half
// bilinearly, from the lower half of the lightmap.
render::Image draw_lightmapped(render::RenderDevice& device,
                               render::RenderMethod method = render::RenderMethod::ForwardPlus,
                               bool lights = true) {
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    std::vector<std::uint8_t> texels;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float t = static_cast<float>(x + y) / 6.0F;
            for (const float value :
                 {2.0F - (1.8F * t), 0.9F - (0.6F * t), 0.1F + (0.5F * t), 1.0F}) {
                const std::uint16_t half = render::float_to_half(value);
                texels.push_back(static_cast<std::uint8_t>(half & 0xFFU));
                texels.push_back(static_cast<std::uint8_t>(half >> 8U));
            }
        }
    }
    auto lightmap = device.create_texture(
        {.width = 4, .height = 4, .format = render::TextureFormat::Rgba16F}, texels);
    REQUIRE(lightmap.ok());
    // Half of the floor: its uv2 goes 0 to 1 across it.
    const auto half_floor = [](float left) {
        const auto corner = [&](float x, float z) {
            return render::Vertex3D{
                .position = {left + x, 0.0F, z - 1.0F}, .uv = {x, z * 0.5F}, .uv2 = {x, z * 0.5F}};
        };
        return std::vector<render::Vertex3D>{corner(0, 0), corner(1, 2), corner(0, 2),
                                             corner(0, 0), corner(1, 0), corner(1, 2)};
    };
    auto left = device.create_mesh(half_floor(-1.0F), {});
    auto right = device.create_mesh(half_floor(0.0F), {});
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({0.0F, 2.5F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
    scene.ambient = {0.6F, 0.6F, 0.6F};
    if (lights) {
        scene.lights = {
            {.type = render::LightType::Directional,
             .energy = 3.0F,
             .direction = {0.0F, -1.0F, 0.0F},
             .baked = true},
            {.type = render::LightType::Omni,
             .color = {0.2F, 1.0F, 0.3F},
             .energy = 2.0F,
             .position = {0.9F, 0.3F, 0.0F},
             .range = 1.0F},
        };
    }
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .scene = scene})
                .ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    render::Material3D material{.albedo = {0.8F, 0.8F, 0.8F, 1.0F}};
    material.lightmap = {.texture = lightmap.value(), .uv_rect = {0.0F, 0.0F, 1.0F, 1.0F}};
    const auto drawn = device.draw_mesh(left.value(), identity, material);
    CHECK_MESSAGE(drawn.ok(), (drawn.ok() ? "" : drawn.error().message));
    material.lightmap.uv_rect = {0.0F, 0.5F, 1.0F, 0.5F};
    material.lightmap.bicubic = false;
    CHECK(device.draw_mesh(right.value(), identity, material).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(left.value());
    device.destroy_mesh(right.value());
    device.destroy_texture(lightmap.value());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

std::array<int, 4> image_pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

} // namespace

TEST_CASE("every device draws lightmaps as the software device does") {
    const auto software = render::create_software_device();
    const render::Image forward = draw_lightmapped(*software);
    // The lightmap's warm corner and its blue one show, and the baked sun adds nothing: only the
    // omni light by the right edge changes the picture.
    const std::array<int, 4> warm = image_pixel(forward, 12, 10);
    const std::array<int, 4> cool = image_pixel(forward, 20, 40);
    CAPTURE(warm[0]);
    CAPTURE(cool[2]);
    CHECK(warm[0] > warm[2] + 60);
    CHECK(cool[2] > cool[0] - 20);
    const render::Image unlit =
        draw_lightmapped(*software, render::RenderMethod::ForwardPlus, false);
    CHECK(image_pixel(unlit, 12, 10) == warm);
    CHECK(image_pixel(unlit, 39, 24) != image_pixel(forward, 39, 24));
    const render::Image compatibility =
        draw_lightmapped(*software, render::RenderMethod::Compatibility);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        const render::Image& expected = opengl ? compatibility : forward;
        const render::Image image = draw_lightmapped(*device);
        int differing = 0;
        int worst = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = image_pixel(image, x, y);
                const std::array<int, 4> b = image_pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 2 ? 1 : 0;
                worst = std::max(worst, most);
            }
        }
        CAPTURE(worst);
        CHECK(differing <= 10);
    }
}

namespace {

// A ball a meter and a half across seen from above, its ambient light from light probes with
// light from above, warm from the right and a little of every band, or the scene's when capture
// is false. No lights shine on it.
render::Image draw_captured(render::RenderDevice& device, bool capture = true,
                            std::array<float, 3> ambient = {0.6F, 0.6F, 0.6F},
                            render::RenderMethod method = render::RenderMethod::ForwardPlus) {
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    const scene3d::MeshData sphere = scene3d::sphere_mesh({.radius = 0.75F, .height = 1.5F});
    auto mesh = device.create_mesh(sphere.vertices, sphere.indices);
    REQUIRE(mesh.ok());
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({0.0F, 2.5F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
    scene.ambient = ambient;
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .scene = scene})
                .ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    render::Material3D material{.albedo = {0.8F, 0.8F, 0.8F, 1.0F}};
    material.capture.enabled = capture;
    material.capture.sh = {{{0.5F, 0.4F, 0.3F},
                            {0.3F, 0.1F, -0.1F},
                            {0.0F, 0.05F, 0.1F},
                            {0.4F, 0.1F, -0.2F},
                            {0.05F, 0.0F, 0.0F},
                            {0.0F, 0.05F, 0.0F},
                            {0.0F, 0.0F, 0.1F},
                            {0.05F, 0.0F, 0.05F},
                            {0.05F, 0.05F, 0.0F}}};
    const auto drawn = device.draw_mesh(mesh.value(), identity, material);
    CHECK_MESSAGE(drawn.ok(), (drawn.ok() ? "" : drawn.error().message));
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

} // namespace

TEST_CASE("every device draws light probes' light as the software device does") {
    const auto software = render::create_software_device();
    const render::Image forward = draw_captured(*software);
    // The probes' light stands in for the scene's ambient light: warmer to the right, where the
    // probes' light is.
    CHECK(draw_captured(*software, true, {0.0F, 0.0F, 1.0F}).pixels == forward.pixels);
    CHECK(draw_captured(*software, false).pixels != forward.pixels);
    const std::array<int, 4> right = image_pixel(forward, 34, 24);
    const std::array<int, 4> left = image_pixel(forward, 14, 24);
    CAPTURE(right[0]);
    CAPTURE(left[0]);
    CHECK(right[0] > left[0] + 20);
    CHECK(right[0] > right[2]);
    const render::Image compatibility =
        draw_captured(*software, true, {0.6F, 0.6F, 0.6F}, render::RenderMethod::Compatibility);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        const render::Image& expected = opengl ? compatibility : forward;
        const render::Image image = draw_captured(*device);
        int differing = 0;
        int worst = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = image_pixel(image, x, y);
                const std::array<int, 4> b = image_pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 2 ? 1 : 0;
                worst = std::max(worst, most);
            }
        }
        CAPTURE(worst);
        CHECK(differing <= 10);
    }
}
