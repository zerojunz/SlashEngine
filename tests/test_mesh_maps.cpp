#include "devices.hpp"

#include "slash/core/jobs.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/mesh_maps.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/tree3d.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
namespace jobs = slash::jobs;
using slash::Vec3;

namespace {

// A face of a box: four corners counterclockwise from outside, its normal and its tangent.
struct Face {
    std::array<Vec3, 4> corners;
    Vec3 normal;
    Vec3 tangent;
};

// The faces of a box from lo to hi: +x, -x, +y, -y, +z, -z.
std::array<Face, 6> box_faces(Vec3 lo, Vec3 hi) {
    const float x0 = lo.x;
    const float y0 = lo.y;
    const float z0 = lo.z;
    const float x1 = hi.x;
    const float y1 = hi.y;
    const float z1 = hi.z;
    return {{
        {{{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}}, {1, 0, 0}, {0, 0, -1}},
        {{{{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}}, {-1, 0, 0}, {0, 0, 1}},
        {{{{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}}}, {0, 1, 0}, {1, 0, 0}},
        {{{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}}, {0, -1, 0}, {1, 0, 0}},
        {{{{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}}, {0, 0, 1}, {1, 0, 0}},
        {{{{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}}}, {0, 0, -1}, {-1, 0, 0}},
    }};
}

// Adds a quad over cell of a grid by grid layout of texture space, a margin inside it.
void add_quad(scene3d::MeshMapMesh& mesh, const Face& face, int cell, int grid, std::uint32_t part,
              std::uint32_t material) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    const float size = 1.0F / static_cast<float>(grid);
    const float margin = size * 0.05F;
    const float u0 = (static_cast<float>(cell % grid) * size) + margin;
    const float v0 = (static_cast<float>(cell / grid) * size) + margin;
    const float u1 = u0 + size - (2.0F * margin);
    const float v1 = v0 + size - (2.0F * margin);
    const std::array<std::array<float, 2>, 4> uvs{{{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}}};
    for (std::size_t k = 0; k < 4; ++k) {
        render::Vertex3D v;
        v.position = {face.corners.at(k).x, face.corners.at(k).y, face.corners.at(k).z};
        v.normal = {face.normal.x, face.normal.y, face.normal.z};
        v.tangent = {face.tangent.x, face.tangent.y, face.tangent.z, 1.0F};
        v.uv = uvs.at(k);
        v.uv2 = uvs.at(k);
        mesh.vertices.push_back(v);
    }
    for (const std::uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) {
        mesh.indices.push_back(first + i);
    }
    for (int t = 0; t < 2; ++t) {
        mesh.parts.push_back(part);
        mesh.materials.push_back(material);
    }
}

// A unit cube on the ground, its faces in a 3x3 grid of texture space.
scene3d::MeshMapMesh cube() {
    scene3d::MeshMapMesh mesh;
    mesh.part_names = {"cube"};
    mesh.material_names = {"default"};
    const auto faces = box_faces({-0.5F, 0.0F, -0.5F}, {0.5F, 1.0F, 0.5F});
    for (int f = 0; f < 6; ++f) {
        add_quad(mesh, faces.at(static_cast<std::size_t>(f)), f, 3, 0, 0);
    }
    return mesh;
}

// A flat square on y = height, facing up, over all of texture space.
scene3d::MeshMapMesh plane(float height, Vec3 normal = {0, 1, 0}, float slope = 0.0F) {
    scene3d::MeshMapMesh mesh;
    mesh.part_names = {"plane"};
    mesh.material_names = {"default"};
    const auto y = [&](float x) { return height + (slope * x); };
    Face face{{{{-0.5F, y(-0.5F), 0.5F},
                {0.5F, y(0.5F), 0.5F},
                {0.5F, y(0.5F), -0.5F},
                {-0.5F, y(-0.5F), -0.5F}}},
              normal,
              {1, 0, 0}};
    add_quad(mesh, face, 0, 1, 0, 0);
    return mesh;
}

const scene3d::MeshMapImage& map_of(const scene3d::MeshMapBake& bake, scene3d::MeshMap map) {
    for (const scene3d::MeshMapImage& image : bake.maps) {
        if (image.map == map) {
            return image;
        }
    }
    FAIL("no such map");
    return bake.maps.front();
}

// The texel at the center of a cell of the 3x3 grid, or near one of its edges.
std::array<std::uint8_t, 4> texel(const scene3d::MeshMapImage& map, float u, float v) {
    const auto x = static_cast<std::uint32_t>(u * static_cast<float>(map.image.width));
    const auto y = static_cast<std::uint32_t>(v * static_cast<float>(map.image.height));
    const std::size_t at = ((std::size_t{y} * map.image.width) + x) * 4;
    return {map.image.pixels[at], map.image.pixels[at + 1], map.image.pixels[at + 2],
            map.image.pixels[at + 3]};
}

std::array<std::uint8_t, 4> cell_center(const scene3d::MeshMapImage& map, int cell) {
    return texel(map, (static_cast<float>(cell % 3) + 0.5F) / 3.0F,
                 (static_cast<float>(cell / 3) + 0.5F) / 3.0F);
}

int near(std::uint8_t a, int b) {
    return std::abs(static_cast<int>(a) - b);
}

} // namespace

TEST_CASE("mesh maps are named, and the height and normal maps need a high mesh") {
    for (const scene3d::MeshMap map : scene3d::kMeshMaps) {
        CHECK(scene3d::mesh_map_named(scene3d::mesh_map_name(map)) == map);
    }
    CHECK_FALSE(scene3d::mesh_map_named("albedo"));
    CHECK(scene3d::mesh_map_needs_high(scene3d::MeshMap::Height));
    CHECK(scene3d::mesh_map_needs_high(scene3d::MeshMap::Normal));
    CHECK_FALSE(scene3d::mesh_map_needs_high(scene3d::MeshMap::Ao));
    // Id colors differ from one id to the next, the same on every machine.
    CHECK(scene3d::mesh_map_id_color(0) != scene3d::mesh_map_id_color(1));
    CHECK(scene3d::mesh_map_id_color(1) != scene3d::mesh_map_id_color(2));
}

TEST_CASE("a cube's mesh maps say where it faces, what the sky reaches and where it bends") {
    scene3d::MeshMapSettings settings;
    settings.width = 96;
    settings.height = 96;
    settings.rays = 16;
    settings.curvature_radius = 0.1F;
    const scene3d::MeshMapBake bake = scene3d::bake_mesh_maps(cube(), nullptr, settings).value();
    // Every map a low mesh alone allows, in order.
    REQUIRE(bake.maps.size() == 8);
    CHECK(bake.maps.front().map == scene3d::MeshMap::Ao);
    CHECK(bake.maps.back().map == scene3d::MeshMap::MaterialId);
    CHECK_FALSE(bake.traced_on_gpu);
    // Six of nine cells, less their margins.
    CHECK(bake.coverage == doctest::Approx(6.0 / 9.0 * 0.81).epsilon(0.05));
    CHECK(bake.overlap == 0.0F);
    CHECK(bake.min == Vec3{-0.5F, 0.0F, -0.5F});
    CHECK(bake.max == Vec3{0.5F, 1.0F, 0.5F});

    // Nothing occludes a lone convex box.
    const auto& ao = map_of(bake, scene3d::MeshMap::Ao);
    CHECK(ao.stats[0] == 1.0F);
    // The top sees all the sky, the sides half and the bottom none.
    const auto& sky = map_of(bake, scene3d::MeshMap::Sky);
    CHECK(cell_center(sky, 2)[0] == 255);
    CHECK(near(cell_center(sky, 0)[0], 128) <= 40);
    CHECK(near(cell_center(sky, 4)[0], 128) <= 40);
    CHECK(cell_center(sky, 3)[0] == 0);
    // A metre through, more than the most measured (half the diagonal) straight in, less toward
    // the sides.
    const auto& thickness = map_of(bake, scene3d::MeshMap::Thickness);
    CHECK(thickness.stats[1] > 0.5F);
    CHECK(thickness.stats[2] > 0.8F);
    // Flat in the middle of a face, convex at its edges.
    const auto& curvature = map_of(bake, scene3d::MeshMap::Curvature);
    CHECK(near(cell_center(curvature, 2)[0], 128) <= 1);
    CHECK(texel(curvature, (1.0F / 96.0F) + (1.0F / 3.0F * 0.05F) + (2.0F / 3.0F), 1.0F / 6.0F)[0] >
          190);
    CHECK(curvature.stats[0] >= 0.49F);
    // The top faces up, and its middle is the box's top middle.
    const auto& normal = map_of(bake, scene3d::MeshMap::WorldNormal);
    const auto up = cell_center(normal, 2);
    CHECK(near(up[0], 128) <= 1);
    CHECK(up[1] == 255);
    CHECK(near(up[2], 128) <= 1);
    const auto& position = map_of(bake, scene3d::MeshMap::Position);
    const auto top = cell_center(position, 2);
    CHECK(near(top[0], 128) <= 6);
    CHECK(top[1] == 255);
    CHECK(near(top[2], 128) <= 6);
    // One part, one material.
    const auto id = scene3d::mesh_map_id_color(0);
    const auto part = cell_center(map_of(bake, scene3d::MeshMap::PartId), 5);
    CHECK(part[0] == id[0]);
    CHECK(part[1] == id[1]);
    CHECK(part[2] == id[2]);
    // Padding grows the islands into their margins, and the unused cells stay empty.
    CHECK(texel(ao, 1.0F / 96.0F, 1.0F / 96.0F)[3] == 255);
    CHECK(cell_center(ao, 8)[3] == 0);
    settings.padding = 0;
    const auto bare = scene3d::bake_mesh_maps(cube(), nullptr, settings).value();
    CHECK(texel(map_of(bare, scene3d::MeshMap::Ao), 1.0F / 96.0F, 1.0F / 96.0F)[3] == 0);
}

TEST_CASE("a box under a shelf is occluded where the shelf covers it") {
    scene3d::MeshMapMesh mesh = cube();
    // A slab half a metre above the cube's top, wider than it.
    const auto slab = box_faces({-1.0F, 1.5F, -1.0F}, {1.0F, 1.6F, 1.0F});
    mesh.part_names.emplace_back("slab");
    for (int f = 0; f < 6; ++f) {
        // In the same cells: what the cube's texels see doesn't depend on the slab's own.
        scene3d::MeshMapMesh one;
        add_quad(one, slab.at(static_cast<std::size_t>(f)), 8, 3, 1, 0);
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), one.vertices.begin(), one.vertices.end());
        for (const std::uint32_t i : one.indices) {
            mesh.indices.push_back(first + i);
        }
        mesh.parts.insert(mesh.parts.end(), one.parts.begin(), one.parts.end());
        mesh.materials.insert(mesh.materials.end(), one.materials.begin(), one.materials.end());
    }
    scene3d::MeshMapSettings settings;
    settings.width = 48;
    settings.height = 48;
    settings.rays = 32;
    settings.occlusion_distance = 2.0F;
    settings.maps = {scene3d::MeshMap::Ao, scene3d::MeshMap::Sky, scene3d::MeshMap::PartId};
    const auto bake = scene3d::bake_mesh_maps(mesh, nullptr, settings).value();
    REQUIRE(bake.maps.size() == 3);
    // The slab's six faces share a cell: they overlap there.
    CHECK(bake.overlap > 0.0F);
    // The top is now in the slab's shade, and sees only the sky low past its rims.
    CHECK(cell_center(map_of(bake, scene3d::MeshMap::Ao), 2)[0] < 100);
    CHECK(cell_center(map_of(bake, scene3d::MeshMap::Sky), 2)[0] < 64);
    // Each part its own color.
    const auto slab_color = scene3d::mesh_map_id_color(1);
    CHECK(cell_center(map_of(bake, scene3d::MeshMap::PartId), 8)[0] == slab_color[0]);
}

TEST_CASE("height and normal maps carry a high mesh over onto a low one") {
    scene3d::MeshMapSettings settings;
    settings.width = 32;
    settings.height = 32;
    settings.maps = {scene3d::MeshMap::Height, scene3d::MeshMap::Normal,
                     scene3d::MeshMap::WorldNormal};
    const scene3d::MeshMapMesh low = plane(0.0F);
    // A high surface 2 cm above, tilted along x: its normal leans toward -x.
    const float slope = 0.1F;
    const Vec3 leaning{-slope / std::sqrt(1.0F + (slope * slope)),
                       1.0F / std::sqrt(1.0F + (slope * slope)), 0.0F};
    const scene3d::MeshMapMesh high = plane(0.02F, leaning, slope);
    settings.cage_distance = 0.1F;
    const auto bake = scene3d::bake_mesh_maps(low, &high, settings).value();
    REQUIRE(bake.maps.size() == 3);
    CHECK(bake.missed == 0.0F);
    CHECK(bake.height_range == 0.1F);
    // In the middle, 2 cm above the low surface: 0.5 + 0.5 * 0.02 / 0.1.
    const auto& height = map_of(bake, scene3d::MeshMap::Height);
    CHECK(near(texel(height, 0.5F, 0.5F)[0], 153) <= 2);
    // Higher toward +x, as the surface tilts.
    CHECK(texel(height, 0.9F, 0.5F)[0] > texel(height, 0.1F, 0.5F)[0]);
    // The leaning normal in the low surface's frame: tangent +x, binormal +y x +x = -z.
    const auto mapped = texel(map_of(bake, scene3d::MeshMap::Normal), 0.5F, 0.5F);
    CHECK(near(mapped[0], static_cast<int>(std::lround((leaning.x * 0.5F + 0.5F) * 255.0F))) <= 1);
    CHECK(near(mapped[1], 128) <= 1);
    CHECK(near(mapped[2], static_cast<int>(std::lround((leaning.y * 0.5F + 0.5F) * 255.0F))) <= 1);
    // The world normal is the high one's.
    const auto world = texel(map_of(bake, scene3d::MeshMap::WorldNormal), 0.5F, 0.5F);
    CHECK(near(world[0], static_cast<int>(std::lround((leaning.x * 0.5F + 0.5F) * 255.0F))) <= 1);

    // A high surface beyond the cage is missed: the texels keep the low surface.
    const scene3d::MeshMapMesh far_above = plane(0.5F);
    const auto missed = scene3d::bake_mesh_maps(low, &far_above, settings).value();
    CHECK(missed.missed == 1.0F);
    CHECK(texel(map_of(missed, scene3d::MeshMap::Height), 0.5F, 0.5F)[0] == 128);
}

TEST_CASE("mesh map bakes refuse what they can't bake") {
    scene3d::MeshMapSettings settings;
    settings.maps = {scene3d::MeshMap::Normal};
    auto no_high = scene3d::bake_mesh_maps(cube(), nullptr, settings);
    REQUIRE_FALSE(no_high);
    CHECK(no_high.error().message.find("high mesh") != std::string::npos);
    settings.maps.clear();
    settings.width = 0;
    CHECK_FALSE(scene3d::bake_mesh_maps(cube(), nullptr, settings));
    settings.width = 16;
    CHECK_FALSE(scene3d::bake_mesh_maps(scene3d::MeshMapMesh{}, nullptr, settings));
    // Every texture coordinate at one point: no texel is covered.
    scene3d::MeshMapMesh flat = cube();
    for (render::Vertex3D& v : flat.vertices) {
        v.uv = {0.25F, 0.25F};
    }
    auto uncovered = scene3d::bake_mesh_maps(flat, nullptr, settings);
    REQUIRE_FALSE(uncovered);
    CHECK(uncovered.error().message.find("unwrap") != std::string::npos);
    // In the second set they are laid out.
    settings.uv2 = true;
    CHECK(scene3d::bake_mesh_maps(flat, nullptr, settings));
}

TEST_CASE("mesh map bakes are the same for any number of threads") {
    scene3d::MeshMapSettings settings;
    settings.width = 48;
    settings.height = 48;
    settings.rays = 8;
    const auto alone = scene3d::bake_mesh_maps(cube(), nullptr, settings).value();
    jobs::Pool pool(4);
    const auto threaded = scene3d::bake_mesh_maps(cube(), nullptr, settings, &pool).value();
    REQUIRE(alone.maps.size() == threaded.maps.size());
    for (std::size_t i = 0; i < alone.maps.size(); ++i) {
        CHECK(alone.maps[i].image.pixels == threaded.maps[i].image.pixels);
    }
}

TEST_CASE("devices that trace rays bake mesh maps as the CPU does") {
    // A cube under a slab, from a high plane: rays that hit and miss, and hits to carry over.
    scene3d::MeshMapSettings settings;
    settings.width = 48;
    settings.height = 48;
    settings.rays = 16;
    const scene3d::MeshMapMesh low = cube();
    scene3d::MeshMapMesh high = cube();
    for (render::Vertex3D& v : high.vertices) {
        v.position[1] += v.position[1] > 0.5F ? 0.01F : 0.0F;
    }
    const auto cpu = scene3d::bake_mesh_maps(low, &high, settings).value();
    // The software device leaves the rays to the CPU.
    const auto software = render::create_software_device();
    settings.device = software.get();
    const auto on_software = scene3d::bake_mesh_maps(low, &high, settings).value();
    CHECK_FALSE(on_software.traced_on_gpu);
    REQUIRE(on_software.maps.size() == cpu.maps.size());
    for (std::size_t i = 0; i < cpu.maps.size(); ++i) {
        CHECK(on_software.maps[i].image.pixels == cpu.maps[i].image.pixels);
    }

    const auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        return;
    }
    settings.device = gpu.get();
    const auto baked = scene3d::bake_mesh_maps(low, &high, settings).value();
    CHECK(baked.traced_on_gpu == gpu->ray_tracing());
    REQUIRE(baked.maps.size() == cpu.maps.size());
    CHECK(baked.missed == cpu.missed);
    // The same but for rounding, which can turn a grazing ray from a hit to a miss.
    for (std::size_t i = 0; i < cpu.maps.size(); ++i) {
        CAPTURE(scene3d::mesh_map_name(cpu.maps[i].map));
        const std::vector<std::uint8_t>& a = cpu.maps[i].image.pixels;
        const std::vector<std::uint8_t>& b = baked.maps[i].image.pixels;
        REQUIRE(a.size() == b.size());
        double off = 0.0;
        std::size_t far = 0;
        for (std::size_t k = 0; k < a.size(); ++k) {
            const int d = std::abs(static_cast<int>(a[k]) - static_cast<int>(b[k]));
            off += d;
            far += d > 24 ? 1 : 0;
        }
        CHECK(off / static_cast<double>(a.size()) < 1.0);
        CHECK(far <= a.size() / 100);
    }
}

TEST_CASE("a model's mesh maps mesh puts its parts where its nodes do") {
    scene3d::Model model;
    scene3d::ModelMaterial oak;
    oak.name = "oak";
    model.materials.push_back(oak);
    scene3d::ModelMesh mesh;
    mesh.name = "quad";
    scene3d::ModelSurface surface;
    for (const auto& p : {std::array<float, 3>{0, 0, 0}, {1, 0, 0}, {1, 0, 1}}) {
        render::Vertex3D v;
        v.position = p;
        v.normal = {0, 1, 0};
        surface.vertices.push_back(v);
    }
    surface.indices = {0, 1, 2};
    surface.material = 0;
    mesh.surfaces.push_back(surface);
    surface.material = -1;
    mesh.surfaces.push_back(surface);
    model.meshes.push_back(mesh);
    world::Affine3D moved;
    moved.origin = {0, 2, 0};
    moved.x = {2, 0, 0}; // stretched along x
    model.parts.push_back({.node = "left", .mesh = 0, .transform = {}});
    model.parts.push_back({.node = "right", .mesh = 0, .transform = moved});
    const scene3d::MeshMapMesh flat = scene3d::mesh_map_mesh(model);
    CHECK(flat.part_names == std::vector<std::string>{"left", "right"});
    CHECK(flat.material_names == std::vector<std::string>{"oak", "default"});
    CHECK(flat.indices.size() == 12);
    CHECK(flat.parts == std::vector<std::uint32_t>{0, 0, 1, 1});
    CHECK(flat.materials == std::vector<std::uint32_t>{0, 1, 0, 1});
    CHECK(flat.vertices[7].position == std::array<float, 3>{2, 2, 0});
    // Normals stay unit length and square to the stretched surface.
    CHECK(flat.vertices[7].normal == std::array<float, 3>{0, 1, 0});
}
