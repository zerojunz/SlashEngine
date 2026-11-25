// The mesh map baker, on the CPU: the twin of the lightmap light shader's mesh map passes
// (engine/render/shaders/lightmap_light.comp, kPass 3 and 4), which trace the same rays with the
// same random numbers on the GPU. The low mesh is rastered into its texture coordinates; each
// covered texel takes its point, normal and tangent frame, and with a high mesh, the high
// surface where a ray from the cage (the low surface pushed out along its normal) inward meets
// it. Rays from there find the ambient occlusion, sky and thickness; curvature comes from the
// angles of the edges near each point; the rest from the surface itself. Every texel's rays come
// from its own random numbers, so threads can't change what it gets.

#include "slash/scene3d/mesh_maps.hpp"

#include "bake_rays.hpp"

#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/render/bake.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

using bake::Bvh;
using bake::cross;
using bake::dot;
using bake::fill_triangle;
using bake::for_each;
using bake::Hit;
using bake::length;
using bake::normalize;
using bake::Point2;
using bake::random_seed;
using bake::ray_dir_from_normal;

constexpr std::uint32_t kMaxSize = 8192;
constexpr std::size_t kMaxTriangles = std::size_t{1} << 24U; // a hit's triangle is a float

constexpr std::array<std::string_view, 10> kNames{
    "ao",       "curvature", "thickness",   "sky",    "world_normal",
    "position", "part_id",   "material_id", "height", "normal"};

Vec3 vec(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}

Vec3 lerp3(Vec3 a, Vec3 b, Vec3 c, Vec3 w) {
    return (a * w.x) + (b * w.y) + (c * w.z);
}

// What a mesh's triangles are to rays, every one met from either side.
struct Scene {
    std::vector<bake::Vertex> vertices;
    std::vector<bake::Triangle> triangles;

    explicit Scene(const MeshMapMesh& mesh) {
        vertices.reserve(mesh.vertices.size());
        for (const render::Vertex3D& v : mesh.vertices) {
            vertices.push_back({.position = vec(v.position), .normal = vec(v.normal), .uv = v.uv});
        }
        triangles.reserve(mesh.indices.size() / 3);
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            bake::Triangle t;
            t.indices = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
            t.cull = render::CullMode::Disabled;
            const Vec3 a = vertices[t.indices[0]].position;
            const Vec3 b = vertices[t.indices[1]].position;
            const Vec3 c = vertices[t.indices[2]].position;
            t.min = {std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}),
                     std::min({a.z, b.z, c.z})};
            t.max = {std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}),
                     std::max({a.z, b.z, c.z})};
            triangles.push_back(t);
        }
    }
};

// The edges where a mesh bends, for curvature: each edge two triangles share (corners welded
// where they lie together), with the angle between their faces, plus where they bend outward
// and minus inward, found through a grid of cells as wide as the radius.
class Edges {
public:
    Edges(const MeshMapMesh& mesh, float weld, float radius) : radius_(radius) {
        const float inv_weld = 1.0F / weld;
        std::unordered_map<std::uint64_t, std::uint32_t> welded;
        std::vector<std::uint32_t> ids(mesh.vertices.size());
        std::vector<Vec3> points;
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vec3 p = vec(mesh.vertices[i].position);
            const std::uint64_t key = cell_key(p, inv_weld);
            const auto [at, added] =
                welded.try_emplace(key, static_cast<std::uint32_t>(points.size()));
            if (added) {
                points.push_back(p);
            }
            ids[i] = at->second;
        }
        struct Side {
            std::uint32_t face = 0;
            std::uint32_t other = 0; // the corner across from the edge
        };
        std::vector<Vec3> normals;
        std::unordered_map<std::uint64_t, std::vector<Side>> sides;
        std::vector<std::uint64_t> order;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const std::array<std::uint32_t, 3> c{ids[mesh.indices[i]], ids[mesh.indices[i + 1]],
                                                 ids[mesh.indices[i + 2]]};
            if (c[0] == c[1] || c[1] == c[2] || c[2] == c[0]) {
                continue;
            }
            Vec3 n = normalize(cross(points[c[1]] - points[c[0]], points[c[2]] - points[c[0]]));
            if (length(n) == 0.0F) {
                continue;
            }
            // Facing the way its corners' normals do, whichever way it winds.
            const Vec3 facing = vec(mesh.vertices[mesh.indices[i]].normal) +
                                vec(mesh.vertices[mesh.indices[i + 1]].normal) +
                                vec(mesh.vertices[mesh.indices[i + 2]].normal);
            if (dot(n, facing) < 0.0F) {
                n = n * -1.0F;
            }
            const auto face = static_cast<std::uint32_t>(normals.size());
            normals.push_back(n);
            for (std::size_t e = 0; e < 3; ++e) {
                const std::uint32_t a = std::min(c[e], c[(e + 1) % 3]);
                const std::uint32_t b = std::max(c[e], c[(e + 1) % 3]);
                const std::uint64_t key = (std::uint64_t{a} << 32U) | b;
                auto& list = sides[key];
                if (list.empty()) {
                    order.push_back(key);
                }
                list.push_back({face, c[(e + 2) % 3]});
            }
        }
        const float inv_cell = 1.0F / radius_;
        for (const std::uint64_t key : order) {
            const std::vector<Side>& list = sides[key];
            if (list.size() != 2) {
                continue; // an open or non-manifold edge
            }
            const Vec3 a = points[key >> 32U];
            const Vec3 b = points[key & 0xFFFFFFFFU];
            const Vec3 n0 = normals[list[0].face];
            const Vec3 n1 = normals[list[1].face];
            const float angle = slash::atan2(length(cross(n0, n1)), dot(n0, n1));
            if (!(angle > 1e-4F)) {
                continue;
            }
            // Convex where the second face falls away below the first's plane.
            const Vec3 middle = (a + b) * 0.5F;
            const bool convex = dot(n0, points[list[1].other] - middle) < 0.0F;
            const auto index = static_cast<std::uint32_t>(edges_.size());
            edges_.push_back({a, b, convex ? angle : -angle});
            // Into each cell the edge's box, grown by the radius, touches.
            const Vec3 lo{std::min(a.x, b.x) - radius_, std::min(a.y, b.y) - radius_,
                          std::min(a.z, b.z) - radius_};
            const Vec3 hi{std::max(a.x, b.x) + radius_, std::max(a.y, b.y) + radius_,
                          std::max(a.z, b.z) + radius_};
            const auto l = cell(lo, inv_cell);
            const auto h = cell(hi, inv_cell);
            for (std::int64_t x = l[0]; x <= h[0]; ++x) {
                for (std::int64_t y = l[1]; y <= h[1]; ++y) {
                    for (std::int64_t z = l[2]; z <= h[2]; ++z) {
                        cells_[pack({x, y, z})].push_back(index);
                    }
                }
            }
        }
    }

    // From -1 (a sharp concave edge through the point) to 1 (a convex one): the angles of the
    // edges within the radius, each by its length inside the radius's sphere and by how near it
    // comes (1 through the point, 0 at the radius), over pi times the radius.
    [[nodiscard]] float at(Vec3 p) const {
        const auto found = cells_.find(pack(cell(p, 1.0F / radius_)));
        if (found == cells_.end()) {
            return 0.0F;
        }
        float sum = 0.0F;
        const float r2 = radius_ * radius_;
        for (const std::uint32_t index : found->second) {
            const Edge& e = edges_[index];
            const Vec3 d = e.b - e.a;
            const float dd = dot(d, d);
            const Vec3 f = e.a - p;
            const float t = std::clamp(-dot(f, d) / dd, 0.0F, 1.0F);
            const float nearest = length(f + (d * t));
            if (!(nearest < radius_)) {
                continue;
            }
            // Where the edge's line enters and leaves the sphere, kept to the edge.
            const float half_b = dot(f, d);
            const float disc = (half_b * half_b) - (dd * (dot(f, f) - r2));
            if (!(disc > 0.0F)) {
                continue;
            }
            const float root = std::sqrt(disc);
            const float t0 = std::max(0.0F, (-half_b - root) / dd);
            const float t1 = std::min(1.0F, (-half_b + root) / dd);
            if (!(t1 > t0)) {
                continue;
            }
            sum += e.angle * ((t1 - t0) * std::sqrt(dd)) * (1.0F - (nearest / radius_));
        }
        return std::clamp(sum / (bake::kPi * radius_), -1.0F, 1.0F);
    }

private:
    struct Edge {
        Vec3 a;
        Vec3 b;
        float angle = 0.0F;
    };

    static std::array<std::int64_t, 3> cell(Vec3 p, float inv) {
        return {static_cast<std::int64_t>(std::floor(p.x * inv)),
                static_cast<std::int64_t>(std::floor(p.y * inv)),
                static_cast<std::int64_t>(std::floor(p.z * inv))};
    }

    static std::uint64_t pack(const std::array<std::int64_t, 3>& c) {
        const auto bits = [](std::int64_t v) {
            return static_cast<std::uint64_t>(v + (std::int64_t{1} << 20U)) & 0x1FFFFFU;
        };
        return (bits(c[0]) << 42U) | (bits(c[1]) << 21U) | bits(c[2]);
    }

    static std::uint64_t cell_key(Vec3 p, float inv) { return pack(cell(p, inv)); }

    float radius_;
    std::vector<Edge> edges_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

// A texel's place on a surface.
struct Sample {
    bool covered = false;
    Vec3 position;
    Vec3 normal;
    std::array<float, 4> tangent{};
    std::uint32_t triangle = 0; // in the mesh the surface is
    bool high = false;          // on the high mesh
};

// The kind of rays a texel traces, as the shader's mesh map passes: kPass 3's occlusion, sky and
// thickness, into x, y and z.
std::array<float, 4> trace_occlusion(const Bvh& bvh, const render::MeshMapTrace& t, int x, int y,
                                     Vec3 position, Vec3 normal) {
    const std::uint32_t rays = std::max(t.rays, 1U);
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    std::uint32_t noise = random_seed(ux, uy, 1U);
    float open = 0.0F;
    for (std::uint32_t i = 0; i < rays; ++i) {
        const Vec3 dir = ray_dir_from_normal(normal, noise);
        if (bvh.trace(position, position + (dir * t.occlusion_distance), t.bias).hit == Hit::Miss) {
            open += 1.0F;
        }
    }
    noise = random_seed(ux, uy, 2U);
    float sky = 0.0F;
    for (std::uint32_t i = 0; i < rays; ++i) {
        const Vec3 dir = ray_dir_from_normal(normal, noise);
        if (dir.y > 0.0F &&
            bvh.trace(position, position + (dir * t.sky_distance), t.bias).hit == Hit::Miss) {
            sky += 1.0F;
        }
    }
    noise = random_seed(ux, uy, 3U);
    float through = 0.0F;
    for (std::uint32_t i = 0; i < rays; ++i) {
        const Vec3 dir = ray_dir_from_normal(normal * -1.0F, noise);
        const Bvh::Result hit =
            bvh.trace(position, position + (dir * t.thickness_distance), t.bias);
        through += hit.hit == Hit::Miss ? t.thickness_distance
                                        : std::min(hit.distance, t.thickness_distance);
    }
    const auto count = static_cast<float>(rays);
    return {open / count, sky / count, through / (count * t.thickness_distance), 1.0F};
}

// kPass 4's: the nearest triangle the ray from the cage meets, plus 1, its barycentrics y and z,
// and how far from the cage.
std::array<float, 4> trace_hit(const Bvh& bvh, const render::MeshMapTrace& t, Vec3 position,
                               Vec3 normal) {
    const Vec3 outside = position + (normal * t.cage_distance);
    const Bvh::Result hit = bvh.trace(outside, position - (normal * t.cage_distance), t.bias);
    if (hit.hit == Hit::Miss) {
        return {};
    }
    return {static_cast<float>(hit.triangle + 1), hit.barycentric.y, hit.barycentric.z,
            hit.distance};
}

// Traces on the device where it can, else on the CPU.
std::vector<std::array<float, 4>> trace(const render::MeshMapTrace& t, const Scene& scene,
                                        render::RenderDevice* device, jobs::Pool* pool,
                                        bool& on_gpu) {
    if (device != nullptr) {
        auto traced = render::trace_mesh_maps(*device, t);
        if (traced) {
            on_gpu = true;
            return std::move(traced.value().texels);
        }
        if (traced.error().code != ErrorCode::NotFound) {
            log::warn("tracing mesh maps on the GPU failed; tracing them on the CPU",
                      {{"error", traced.error().message}});
        }
    }
    on_gpu = false;
    const Bvh bvh(scene.vertices, scene.triangles);
    std::vector<std::array<float, 4>> out(std::size_t{t.width} * t.height);
    for_each(pool, t.height, [&](std::size_t row) {
        const auto y = static_cast<int>(row);
        for (std::uint32_t col = 0; col < t.width; ++col) {
            const std::size_t at = (row * t.width) + col;
            const Vec3 normal{t.normal[at][0], t.normal[at][1], t.normal[at][2]};
            if (length(normal) < 0.5F) {
                continue;
            }
            const Vec3 position{t.position[at][0], t.position[at][1], t.position[at][2]};
            out[at] = t.kind == render::BakeKind::MeshOcclusion
                          ? trace_occlusion(bvh, t, static_cast<int>(col), y, position, normal)
                          : trace_hit(bvh, t, position, normal);
        }
    });
    return out;
}

std::vector<render::BakeVertex> bake_vertices(const MeshMapMesh& mesh) {
    std::vector<render::BakeVertex> out;
    out.reserve(mesh.vertices.size());
    for (const render::Vertex3D& v : mesh.vertices) {
        out.push_back({.position = v.position, .u = 0.0F, .normal = v.normal, .v = 0.0F});
    }
    return out;
}

std::uint8_t unorm8(float v) {
    if (!(v > 0.0F)) {
        return 0;
    }
    if (!(v < 1.0F)) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::lround(v * 255.0F));
}

// Grows the covered texels (alpha 255) into the empty ones around them, a texel a round: each
// empty texel next to covered ones takes the first of them, left, right, up, down, then the
// corners, as the round before left them.
void dilate(render::Image& image, std::uint32_t rounds) {
    const auto w = static_cast<int>(image.width);
    const auto h = static_cast<int>(image.height);
    constexpr std::array<std::array<int, 2>, 8> kAround{
        {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
    std::vector<std::uint8_t> before;
    for (std::uint32_t round = 0; round < rounds; ++round) {
        before = image.pixels;
        bool grew = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t at =
                    ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
                if (before[at + 3] != 0) {
                    continue;
                }
                for (const auto& d : kAround) {
                    const int nx = x + d[0];
                    const int ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
                    const std::size_t from = ((static_cast<std::size_t>(ny) * image.width) +
                                              static_cast<std::size_t>(nx)) *
                                             4;
                    if (before[from + 3] != 0) {
                        std::memcpy(&image.pixels[at], &before[from], 4);
                        grew = true;
                        break;
                    }
                }
            }
        }
        if (!grew) {
            return;
        }
    }
}

struct Box {
    Vec3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
             std::numeric_limits<float>::lowest()};

    void add(const MeshMapMesh& mesh) {
        for (const render::Vertex3D& v : mesh.vertices) {
            const Vec3 p = vec(v.position);
            min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
            max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
        }
    }
};

Result<void> check_mesh(const MeshMapMesh& mesh, std::string_view which) {
    const std::size_t triangles = mesh.indices.size() / 3;
    if (triangles == 0 || mesh.indices.size() % 3 != 0) {
        return Error{ErrorCode::InvalidArgument,
                     std::string("the ") + std::string(which) + " mesh has no triangles to bake"};
    }
    if (triangles > kMaxTriangles) {
        return Error{ErrorCode::InvalidArgument, std::string("the ") + std::string(which) +
                                                     " mesh has more than 2^24 triangles"};
    }
    for (const std::uint32_t i : mesh.indices) {
        if (i >= mesh.vertices.size()) {
            return Error{ErrorCode::InvalidArgument, std::string("the ") + std::string(which) +
                                                         " mesh's indices pass its vertices"};
        }
    }
    if (mesh.parts.size() != triangles || mesh.materials.size() != triangles) {
        return Error{ErrorCode::InvalidArgument,
                     std::string("the ") + std::string(which) +
                         " mesh needs a part and a material for each triangle"};
    }
    return {};
}

} // namespace

std::string_view mesh_map_name(MeshMap map) {
    return kNames.at(static_cast<std::size_t>(map));
}

std::optional<MeshMap> mesh_map_named(std::string_view name) {
    for (const MeshMap map : kMeshMaps) {
        if (mesh_map_name(map) == name) {
            return map;
        }
    }
    return std::nullopt;
}

bool mesh_map_needs_high(MeshMap map) {
    return map == MeshMap::Height || map == MeshMap::Normal;
}

std::array<std::uint8_t, 3> mesh_map_id_color(std::uint32_t id) {
    // Hue in sixths of a turn as fixed point, so every machine rounds alike: hue = id times the
    // golden ratio's fraction, saturation 0.65, value 0.95.
    const std::uint32_t hue = (id * 2654435769U) >> 8U; // 24 bits of a turn
    const std::uint32_t sixths = hue * 6U;              // the sector in its top 8 bits
    const std::uint32_t sector = sixths >> 24U;
    const float f = static_cast<float>(sixths & 0xFFFFFFU) / 16777216.0F;
    const float v = 0.95F;
    const float p = v * (1.0F - 0.65F);
    const float q = v * (1.0F - (0.65F * f));
    const float t = v * (1.0F - (0.65F * (1.0F - f)));
    std::array<float, 3> rgb{};
    switch (sector) {
    case 0:
        rgb = {v, t, p};
        break;
    case 1:
        rgb = {q, v, p};
        break;
    case 2:
        rgb = {p, v, t};
        break;
    case 3:
        rgb = {p, q, v};
        break;
    case 4:
        rgb = {t, p, v};
        break;
    default:
        rgb = {v, p, q};
        break;
    }
    return {unorm8(rgb[0]), unorm8(rgb[1]), unorm8(rgb[2])};
}

MeshMapMesh mesh_map_mesh(const Model& model) {
    MeshMapMesh out;
    for (const ModelMaterial& material : model.materials) {
        out.material_names.push_back(material.name);
    }
    const auto default_material = static_cast<std::uint32_t>(out.material_names.size());
    bool uses_default = false;
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const ModelPart& part = model.parts[p];
        out.part_names.push_back(part.node);
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        const world::Affine3D& to_model = part.transform;
        const world::Affine3D inverse = to_model.inverse();
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            const auto first = static_cast<std::uint32_t>(out.vertices.size());
            for (render::Vertex3D v : surface.vertices) {
                const Vec3 position = to_model.apply(vec(v.position));
                // Normals through the inverse's transpose, so scaling keeps them square.
                const Vec3 n = vec(v.normal);
                const Vec3 normal =
                    normalize(Vec3{dot(inverse.x, n), dot(inverse.y, n), dot(inverse.z, n)});
                const Vec3 tangent =
                    normalize(to_model.basis({v.tangent[0], v.tangent[1], v.tangent[2]}));
                v.position = {position.x, position.y, position.z};
                v.normal = {normal.x, normal.y, normal.z};
                v.tangent = {tangent.x, tangent.y, tangent.z, v.tangent[3]};
                out.vertices.push_back(v);
            }
            std::uint32_t material = default_material;
            if (surface.material >= 0 &&
                static_cast<std::size_t>(surface.material) < model.materials.size()) {
                material = static_cast<std::uint32_t>(surface.material);
            } else {
                uses_default = true;
            }
            for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
                for (std::size_t k = 0; k < 3; ++k) {
                    out.indices.push_back(first + surface.indices[i + k]);
                }
                out.parts.push_back(static_cast<std::uint32_t>(p));
                out.materials.push_back(material);
            }
        }
    }
    if (uses_default) {
        out.material_names.emplace_back("default");
    }
    return out;
}

Result<MeshMapBake> bake_mesh_maps(const MeshMapMesh& low, const MeshMapMesh* high,
                                   const MeshMapSettings& settings, jobs::Pool* pool) {
    if (settings.width == 0 || settings.height == 0 || settings.width > kMaxSize ||
        settings.height > kMaxSize) {
        return Error{ErrorCode::InvalidArgument, "mesh maps are 1 to 8192 texels wide and high"};
    }
    if (auto checked = check_mesh(low, "low"); !checked) {
        return checked.error();
    }
    if (high != nullptr) {
        if (auto checked = check_mesh(*high, "high"); !checked) {
            return checked.error();
        }
    }
    std::array<bool, kMeshMaps.size()> wanted{};
    for (const MeshMap map : settings.maps) {
        if (mesh_map_needs_high(map) && high == nullptr) {
            return Error{ErrorCode::InvalidArgument, std::string("the ") +
                                                         std::string(mesh_map_name(map)) +
                                                         " map bakes from a high mesh; give one"};
        }
        wanted.at(static_cast<std::size_t>(map)) = true;
    }
    if (settings.maps.empty()) {
        for (const MeshMap map : kMeshMaps) {
            wanted.at(static_cast<std::size_t>(map)) = high != nullptr || !mesh_map_needs_high(map);
        }
    }
    const auto wants = [&](MeshMap map) { return wanted.at(static_cast<std::size_t>(map)); };

    Box box;
    box.add(low);
    if (high != nullptr) {
        box.add(*high);
    }
    const float size = std::max(length(box.max - box.min), 1e-6F);
    const auto distance = [&](float given, float share) {
        return given > 0.0F ? given : size * share;
    };
    const float occlusion_distance = distance(settings.occlusion_distance, 0.25F);
    const float thickness_distance = distance(settings.thickness_distance, 0.5F);
    const float curvature_radius = distance(settings.curvature_radius, 0.02F);
    const float cage_distance = distance(settings.cage_distance, 0.05F);
    const float bias = std::max(size * 1e-5F, 1e-7F);

    const std::uint32_t width = settings.width;
    const std::uint32_t height = settings.height;
    const std::size_t texels = std::size_t{width} * height;

    // The low mesh rastered into its texture coordinates: each texel's triangle and barycentrics.
    std::vector<std::int64_t> owner(texels, -1);
    std::vector<Vec3> weights(texels);
    std::vector<std::uint8_t> overlapped(texels, 0);
    const std::size_t triangles = low.indices.size() / 3;
    for (std::size_t tri = 0; tri < triangles; ++tri) {
        std::array<Point2, 3> s{};
        for (std::size_t k = 0; k < 3; ++k) {
            const render::Vertex3D& v = low.vertices[low.indices[(tri * 3) + k]];
            const std::array<float, 2>& uv = settings.uv2 ? v.uv2 : v.uv;
            s.at(k) = {uv[0] * static_cast<float>(width), uv[1] * static_cast<float>(height)};
        }
        fill_triangle(s, static_cast<int>(width), static_cast<int>(height),
                      [&](int x, int y, Vec3 w) {
                          const std::size_t at =
                              (static_cast<std::size_t>(y) * width) + static_cast<std::size_t>(x);
                          if (owner[at] >= 0) {
                              // Overlap, unless the texel lies on an edge both triangles share,
                              // which rounding can give to both.
                              constexpr float kInside = 1e-3F;
                              if (w.x > kInside && w.y > kInside && w.z > kInside) {
                                  overlapped[at] = 1;
                              }
                              return;
                          }
                          owner[at] = static_cast<std::int64_t>(tri);
                          weights[at] = w;
                      });
    }

    // Each covered texel's point, normal and tangent frame on the low mesh.
    std::vector<Sample> samples(texels);
    std::vector<std::array<float, 4>> positions(texels);
    std::vector<std::array<float, 4>> normals(texels);
    std::size_t covered = 0;
    for (std::size_t at = 0; at < texels; ++at) {
        if (owner[at] < 0) {
            continue;
        }
        const auto tri = static_cast<std::size_t>(owner[at]);
        const render::Vertex3D& a = low.vertices[low.indices[tri * 3]];
        const render::Vertex3D& b = low.vertices[low.indices[(tri * 3) + 1]];
        const render::Vertex3D& c = low.vertices[low.indices[(tri * 3) + 2]];
        const Vec3 w = weights[at];
        Sample& s = samples[at];
        s.position = lerp3(vec(a.position), vec(b.position), vec(c.position), w);
        s.normal = normalize(lerp3(vec(a.normal), vec(b.normal), vec(c.normal), w));
        if (length(s.normal) == 0.0F) {
            continue; // normals that cancel out: nothing to face
        }
        const Vec3 tangent = lerp3({a.tangent[0], a.tangent[1], a.tangent[2]},
                                   {b.tangent[0], b.tangent[1], b.tangent[2]},
                                   {c.tangent[0], c.tangent[1], c.tangent[2]}, w);
        s.tangent = {tangent.x, tangent.y, tangent.z, a.tangent[3] < 0.0F ? -1.0F : 1.0F};
        s.triangle = static_cast<std::uint32_t>(tri);
        s.covered = true;
        ++covered;
        positions[at] = {s.position.x, s.position.y, s.position.z, 1.0F};
        normals[at] = {s.normal.x, s.normal.y, s.normal.z, 0.0F};
    }
    if (covered == 0) {
        return Error{ErrorCode::InvalidArgument,
                     std::string("no triangle of the low mesh covers a texel of its ") +
                         (settings.uv2 ? "second " : "") +
                         "texture coordinates; bake into the other set or unwrap the mesh"};
    }

    MeshMapBake out;
    out.width = width;
    out.height = height;
    out.coverage = static_cast<float>(covered) / static_cast<float>(texels);
    const auto overlapping =
        static_cast<std::size_t>(std::count(overlapped.begin(), overlapped.end(), std::uint8_t{1}));
    out.overlap = static_cast<float>(overlapping) / static_cast<float>(covered);
    out.min = box.min;
    out.max = box.max;

    render::MeshMapTrace base;
    base.width = width;
    base.height = height;
    base.rays = settings.rays;
    base.bias = bias;
    base.occlusion_distance = occlusion_distance;
    base.thickness_distance = thickness_distance;
    base.sky_distance = size * 2.0F;
    base.cage_distance = cage_distance;

    // With a high mesh, each texel moves to where its ray from the cage meets it.
    const MeshMapMesh& surface_mesh = high != nullptr ? *high : low;
    const std::optional<Scene> high_scene =
        high != nullptr ? std::optional<Scene>(std::in_place, *high) : std::nullopt;
    const Scene low_scene(low);
    const Scene& scene = high_scene ? *high_scene : low_scene;
    std::vector<float> heights(texels, 0.0F);
    bool traced_any = false;
    bool all_gpu = true;
    if (high != nullptr) {
        render::MeshMapTrace hits = base;
        hits.kind = render::BakeKind::MeshHits;
        hits.vertices = bake_vertices(*high);
        hits.indices = high->indices;
        hits.position = positions;
        hits.normal = normals;
        bool on_gpu = false;
        const auto traced = trace(hits, scene, settings.device, pool, on_gpu);
        traced_any = true;
        all_gpu = all_gpu && on_gpu;
        std::size_t missed = 0;
        for (std::size_t at = 0; at < texels; ++at) {
            Sample& s = samples[at];
            if (!s.covered) {
                continue;
            }
            const std::array<float, 4>& hit = traced[at];
            const auto tri = static_cast<std::int64_t>(hit[0]) - 1;
            if (tri < 0 || static_cast<std::size_t>(tri) >= high->indices.size() / 3) {
                ++missed;
                continue;
            }
            const auto t = static_cast<std::size_t>(tri);
            const Vec3 w{1.0F - (hit[1] + hit[2]), hit[1], hit[2]};
            const render::Vertex3D& a = high->vertices[high->indices[t * 3]];
            const render::Vertex3D& b = high->vertices[high->indices[(t * 3) + 1]];
            const render::Vertex3D& c = high->vertices[high->indices[(t * 3) + 2]];
            const Vec3 normal = normalize(lerp3(vec(a.normal), vec(b.normal), vec(c.normal), w));
            heights[at] = cage_distance - hit[3];
            s.position = lerp3(vec(a.position), vec(b.position), vec(c.position), w);
            if (length(normal) > 0.0F) {
                s.normal = normal;
            }
            s.triangle = static_cast<std::uint32_t>(t);
            s.high = true;
            positions[at] = {s.position.x, s.position.y, s.position.z, 1.0F};
            normals[at] = {s.normal.x, s.normal.y, s.normal.z, 0.0F};
        }
        out.missed = static_cast<float>(missed) / static_cast<float>(covered);
        out.height_range = cage_distance;
    }

    // Ambient occlusion, sky and thickness from each texel's surface point.
    std::vector<std::array<float, 4>> occlusion;
    if (wants(MeshMap::Ao) || wants(MeshMap::Sky) || wants(MeshMap::Thickness)) {
        render::MeshMapTrace rays = base;
        rays.kind = render::BakeKind::MeshOcclusion;
        rays.vertices = bake_vertices(surface_mesh);
        rays.indices = surface_mesh.indices;
        rays.position = positions;
        rays.normal = normals;
        bool on_gpu = false;
        occlusion = trace(rays, scene, settings.device, pool, on_gpu);
        traced_any = true;
        all_gpu = all_gpu && on_gpu;
    }
    out.traced_on_gpu = traced_any && all_gpu;

    std::vector<float> curvature;
    if (wants(MeshMap::Curvature)) {
        const Edges edges(surface_mesh, size * 1e-5F, curvature_radius);
        curvature.assign(texels, 0.0F);
        for_each(pool, height, [&](std::size_t row) {
            for (std::size_t at = row * width; at < (row + 1) * width; ++at) {
                if (samples[at].covered) {
                    curvature[at] = edges.at(samples[at].position);
                }
            }
        });
    }

    const Vec3 extent = box.max - box.min;
    const auto fraction = [](float v, float lo, float range) {
        return range > 0.0F ? (v - lo) / range : 0.5F;
    };
    for (const MeshMap map : kMeshMaps) {
        if (!wants(map)) {
            continue;
        }
        MeshMapImage image;
        image.map = map;
        image.image.width = width;
        image.image.height = height;
        image.image.pixels.assign(texels * 4, 0);
        for (std::size_t at = 0; at < texels; ++at) {
            const Sample& s = samples[at];
            if (!s.covered) {
                continue;
            }
            std::array<float, 3> rgb{};
            std::array<std::uint8_t, 3> id{};
            bool is_id = false;
            switch (map) {
            case MeshMap::Ao:
                rgb.fill(occlusion[at][0]);
                break;
            case MeshMap::Sky:
                rgb.fill(occlusion[at][1]);
                break;
            case MeshMap::Thickness:
                rgb.fill(occlusion[at][2]);
                break;
            case MeshMap::Curvature:
                rgb.fill(0.5F + (0.5F * curvature[at]));
                break;
            case MeshMap::WorldNormal:
                rgb = {(s.normal.x * 0.5F) + 0.5F, (s.normal.y * 0.5F) + 0.5F,
                       (s.normal.z * 0.5F) + 0.5F};
                break;
            case MeshMap::Position:
                rgb = {fraction(s.position.x, box.min.x, extent.x),
                       fraction(s.position.y, box.min.y, extent.y),
                       fraction(s.position.z, box.min.z, extent.z)};
                break;
            case MeshMap::PartId:
            case MeshMap::MaterialId: {
                const MeshMapMesh& mesh = s.high && high != nullptr ? *high : low;
                const std::vector<std::uint32_t>& ids =
                    map == MeshMap::PartId ? mesh.parts : mesh.materials;
                id = mesh_map_id_color(ids[s.triangle]);
                is_id = true;
                break;
            }
            case MeshMap::Height:
                rgb.fill(0.5F + (0.5F * std::clamp(heights[at] / cage_distance, -1.0F, 1.0F)));
                break;
            case MeshMap::Normal: {
                // The surface's normal in the low mesh's tangent frame, as the mesh shader
                // reads a normal map: tangent, binormal (normal x tangent, by its sign), normal.
                const Sample& low_sample = samples[at];
                const auto tri = static_cast<std::size_t>(owner[at]);
                const render::Vertex3D& a = low.vertices[low.indices[tri * 3]];
                const render::Vertex3D& b = low.vertices[low.indices[(tri * 3) + 1]];
                const render::Vertex3D& c = low.vertices[low.indices[(tri * 3) + 2]];
                const Vec3 n =
                    normalize(lerp3(vec(a.normal), vec(b.normal), vec(c.normal), weights[at]));
                Vec3 t{low_sample.tangent[0], low_sample.tangent[1], low_sample.tangent[2]};
                t = normalize(t - (n * dot(n, t)));
                const Vec3 bn = cross(n, t) * low_sample.tangent[3];
                const Vec3 m = s.normal;
                rgb = {(dot(m, t) * 0.5F) + 0.5F, (dot(m, bn) * 0.5F) + 0.5F,
                       (dot(m, n) * 0.5F) + 0.5F};
                break;
            }
            }
            std::uint8_t* px = &image.image.pixels[at * 4];
            if (is_id) {
                px[0] = id[0];
                px[1] = id[1];
                px[2] = id[2];
            } else {
                px[0] = unorm8(rgb[0]);
                px[1] = unorm8(rgb[1]);
                px[2] = unorm8(rgb[2]);
            }
            px[3] = 255;
        }
        float lo = 1.0F;
        float hi = 0.0F;
        double sum = 0.0;
        for (std::size_t at = 0; at < texels; ++at) {
            if (samples[at].covered) {
                const float v = static_cast<float>(image.image.pixels[at * 4]) / 255.0F;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
            }
        }
        image.stats = {lo, static_cast<float>(sum / static_cast<double>(covered)), hi};
        dilate(image.image, settings.padding);
        out.maps.push_back(std::move(image));
    }
    return out;
}

} // namespace slash::scene3d
