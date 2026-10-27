#include "slash/scene3d/particle_collision.hpp"

#include "geometry.hpp"

#include "slash/asset/assets.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Godot's GPUParticlesCollisionSDF3D bake (scene/3d/gpu_particles_collision_3d.cpp) and the
// height field its renderer draws (RendererSceneRenderRD::render_particle_collider_heightfield),
// ported. Godot's code is MIT.
namespace slash::scene3d {
namespace {

using detail::Box;
using detail::Plane;
using world::Affine3D;
using Triangle = std::array<Vec3, 3>;

template <typename T>
std::vector<entt::entity> made(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const T>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

// Whether entity is in the subtree under scope, or is scope (every entity for a null scope).
bool at_or_under(const world::Tree3D& tree, entt::entity entity, entt::entity scope) {
    if (scope == entt::null) {
        return true;
    }
    for (entt::entity at = entity; at != entt::null; at = tree.parent(at)) {
        if (at == scope) {
            return true;
        }
    }
    return false;
}

Vec3 position_of(const render::Vertex3D& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

bool opaque(const world::StandardMaterial3D* material) {
    return material == nullptr || material->transparency == "disabled";
}

// Which of what draws gives its triangles: those on a layer of mask under scope, and whether
// CSG roots and GridMaps must be on one too (Godot's SDF bake takes every node with get_meshes),
// whether only opaque surfaces count (a depth pass draws no others) and whether HTerrains do.
struct Gather {
    std::uint32_t mask = 0xFFFFFFFFU;
    entt::entity scope = entt::null;
    bool built_by_layer = false;
    bool opaque_only = false;
    bool terrains = false;
};

// Adds a mesh's triangles, placed by place, and counts it.
void add_mesh(std::span<const render::Vertex3D> vertices, std::span<const std::uint32_t> indices,
              const Affine3D& place, std::vector<Triangle>& out) {
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size()) {
            continue;
        }
        out.push_back({place.apply(position_of(vertices[indices[i]])),
                       place.apply(position_of(vertices[indices[i + 1]])),
                       place.apply(position_of(vertices[indices[i + 2]]))});
    }
}

// The triangles of what draws in the world as gather picks it, placed by to_space, mesh by mesh:
// MeshInstance3Ds' primitive meshes or models' parts at rest, CSG roots, GridMaps' cells and
// HTerrains at full detail. keep says whether a mesh's triangles in to_space's space, placed,
// are kept.
std::size_t gather_triangles(const world::World& world, const world::Tree3D& tree,
                             const Affine3D& to_space, const Gather& gather,
                             const ModelLoader& models, std::vector<Triangle>& out,
                             std::vector<Problem>& problems) {
    const auto& ecs = world.ecs();
    std::size_t meshes = 0;
    for (const entt::entity entity : made<world::MeshInstance3D>(world)) {
        const auto& instance = ecs.get<const world::MeshInstance3D>(entity);
        if (!tree.visible(entity) || (instance.layers & gather.mask) == 0 ||
            !at_or_under(tree, entity, gather.scope)) {
            continue;
        }
        const auto* override_material = ecs.try_get<const world::StandardMaterial3D>(entity);
        const Affine3D place = to_space * tree.global(entity);
        const std::size_t before = out.size();
        if (instance.mesh.empty()) {
            auto primitive = primitive_of(world, entity);
            if (!primitive || !primitive.value()) {
                continue;
            }
            if (gather.opaque_only && !opaque(override_material)) {
                continue;
            }
            auto data = make_mesh(*primitive.value());
            if (!data) {
                continue;
            }
            add_mesh(data.value().vertices, data.value().indices, place, out);
        } else {
            if (!models) {
                problems.push_back(
                    {entity, "/components/MeshInstance3D/mesh", "models can't load here"});
                continue;
            }
            auto loaded = models(instance.mesh);
            if (!loaded) {
                problems.push_back(
                    {entity, "/components/MeshInstance3D/mesh", loaded.error().message});
                continue;
            }
            const Model& model = *loaded.value();
            for (const ModelPart& part : model.parts) {
                if (part.mesh >= model.meshes.size()) {
                    continue;
                }
                for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
                    const world::StandardMaterial3D* material = override_material;
                    if (material == nullptr && surface.material >= 0 &&
                        static_cast<std::size_t>(surface.material) < model.materials.size()) {
                        material =
                            &model.materials[static_cast<std::size_t>(surface.material)].standard;
                    }
                    if (gather.opaque_only && !opaque(material)) {
                        continue;
                    }
                    add_mesh(surface.vertices, surface.indices, place * part.transform, out);
                }
            }
        }
        if (out.size() > before) {
            ++meshes;
        }
    }
    for (const entt::entity root : csg_roots(world, tree)) {
        if (!tree.visible(root) || !at_or_under(tree, root, gather.scope)) {
            continue;
        }
        const std::optional<CsgShape> shape = csg_shape_of(world, root);
        if (gather.built_by_layer && shape && (csg_settings(*shape).layers & gather.mask) == 0) {
            continue;
        }
        const std::shared_ptr<const CsgMesh> mesh = csg_mesh(world, tree, root);
        if (mesh == nullptr) {
            continue;
        }
        const std::size_t before = out.size();
        const Affine3D place = to_space * tree.global(root);
        for (const MeshSurface& surface : mesh->surfaces) {
            if (gather.opaque_only && surface.material.standard &&
                !opaque(&*surface.material.standard)) {
                continue;
            }
            add_mesh(surface.vertices, surface.indices, place, out);
        }
        if (out.size() > before) {
            ++meshes;
        }
    }
    for (const entt::entity entity : made<world::GridMap>(world)) {
        // A GridMap's octants are on the first render layer.
        if (!tree.visible(entity) || !at_or_under(tree, entity, gather.scope) ||
            (gather.built_by_layer && (gather.mask & 1U) == 0)) {
            continue;
        }
        const std::shared_ptr<const GridMesh> grid = grid_mesh(world, entity);
        if (grid == nullptr) {
            continue;
        }
        const std::size_t before = out.size();
        const Affine3D place = to_space * tree.global(entity);
        for (const auto& octant : grid->octants) {
            for (const GridSurface& surface : octant->surfaces) {
                if (gather.opaque_only && surface.mesh.material.standard &&
                    !opaque(&*surface.mesh.material.standard)) {
                    continue;
                }
                add_mesh(surface.mesh.vertices, surface.mesh.indices, place, out);
            }
        }
        if (out.size() > before) {
            ++meshes;
        }
    }
    if (gather.terrains) {
        for (const entt::entity entity : made<world::HTerrain>(world)) {
            const auto& terrain = ecs.get<const world::HTerrain>(entity);
            if (!tree.visible(entity) || (terrain.render_layers & gather.mask) == 0 ||
                !at_or_under(tree, entity, gather.scope)) {
                continue;
            }
            const std::shared_ptr<const TerrainData> data = terrain_data(world, entity);
            if (data == nullptr || data->heights.empty()) {
                continue;
            }
            const Affine3D place =
                to_space * terrain_transform(terrain, data->resolution, tree.global(entity));
            const std::vector<Vec3> faces = terrain_faces(*data);
            const std::size_t before = out.size();
            for (std::size_t i = 0; i + 2 < faces.size(); i += 3) {
                out.push_back(
                    {place.apply(faces[i]), place.apply(faces[i + 1]), place.apply(faces[i + 2])});
            }
            if (out.size() > before) {
                ++meshes;
            }
        }
    }
    return meshes;
}

// ---- The SDF bake

// Godot's SIGN: 0 for 0.
float sign_of(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

// A node of the bake's bounding volume hierarchy: its bounds and its children, each a node or,
// with kLeaf set, a triangle.
struct BvhNode {
    Box bounds;
    std::array<std::uint32_t, 2> children{};
};
constexpr std::uint32_t kLeaf = 0x80000000U;
constexpr std::uint32_t kLeafMask = 0x7FFFFFFFU;

struct FacePos {
    Vec3 center;
    std::uint32_t index = 0;
};

class SdfBaker {
public:
    SdfBaker(std::vector<Triangle> triangles, float thickness)
        : triangles_(std::move(triangles)), thickness_(thickness) {
        planes_.reserve(triangles_.size());
        for (const Triangle& t : triangles_) {
            planes_.push_back(Plane::through(t[0], t[1], t[2]));
        }
        std::vector<FacePos> faces(triangles_.size());
        for (std::size_t i = 0; i < triangles_.size(); ++i) {
            const Triangle& t = triangles_[i];
            faces[i].index = static_cast<std::uint32_t>(i);
            // Godot halves the sum of the three corners.
            faces[i].center = (t[0] + t[1] + t[2]) * 0.5F;
            if (thickness_ > 0.0F) {
                faces[i].center = faces[i].center - (planes_[i].normal * (thickness_ * 0.5F));
            }
        }
        build(faces);
    }

    // _find_closest_distance from the root.
    [[nodiscard]] float distance(Vec3 p) const {
        float closest = 1e20F;
        find(p, 0, closest);
        return closest;
    }

private:
    // _create_bvh: the faces' bounds, grown thickness behind each, split at the middle along
    // their longest axis. Faces with the same center keep their order, so every platform builds
    // the same tree.
    std::uint32_t build(std::span<FacePos> faces) {
        if (faces.size() == 1) {
            return kLeaf | faces[0].index;
        }
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        BvhNode node;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const Triangle& t = triangles_[faces[i].index];
            Box box{t[0], Vec3{}};
            box.expand_to(t[1]);
            box.expand_to(t[2]);
            if (thickness_ > 0.0F) {
                const Vec3 back = planes_[faces[i].index].normal * thickness_;
                box.expand_to(t[0] - back);
                box.expand_to(t[1] - back);
                box.expand_to(t[2] - back);
            }
            if (i == 0) {
                node.bounds = box;
            } else {
                node.bounds.expand_to(box.position);
                node.bounds.expand_to(box.position + box.size);
            }
        }
        nodes_.push_back(node);
        const int axis = node.bounds.longest_axis();
        std::ranges::sort(faces, [axis](const FacePos& a, const FacePos& b) {
            const float ca = detail::component(a.center, axis);
            const float cb = detail::component(b.center, axis);
            return ca < cb || (ca == cb && a.index < b.index);
        });
        const std::size_t middle = faces.size() / 2;
        const std::uint32_t left = build(faces.first(middle));
        const std::uint32_t right = build(faces.subspan(middle));
        nodes_[index].children = {left, right};
        return index;
    }

    void find(Vec3 p, std::uint32_t cell, float& closest) const {
        if ((cell & kLeaf) != 0) {
            leaf(p, cell & kLeafMask, closest);
            return;
        }
        const BvhNode& node = nodes_[cell];
        if (!node.bounds.has_point(p)) {
            // Outside: skipped when already closer than the box.
            const Vec3 he = node.bounds.size * 0.5F;
            const Vec3 center = node.bounds.position + he;
            const Vec3 rel{std::abs(p.x - center.x), std::abs(p.y - center.y),
                           std::abs(p.z - center.z)};
            const Vec3 nearest{std::min(rel.x, he.x), std::min(rel.y, he.y), std::min(rel.z, he.z)};
            if (detail::length(rel - nearest) >= closest) {
                return;
            }
        }
        find(p, node.children[0], closest);
        find(p, node.children[1], closest);
    }

    void leaf(Vec3 point, std::uint32_t index, float& closest) const {
        const Triangle& t = triangles_[index];
        const Plane& plane = planes_[index];
        const float d = plane.distance_to(point);
        if (d < 0.0F && d > -thickness_) {
            // Behind the face, within thickness: in its plane, in 2D
            // (https://www.shadertoy.com/view/XsXSz4).
            const Vec3 x_axis = detail::normalized(t[0] - t[1]);
            const Vec3 y_axis = detail::normalized(detail::cross(plane.normal, x_axis));
            std::array<Vec2, 3> points;
            for (std::size_t i = 0; i < 3; ++i) {
                points.at(i) = {detail::dot(x_axis, t.at(i)), detail::dot(y_axis, t.at(i))};
            }
            const Vec2 p2{detail::dot(x_axis, point), detail::dot(y_axis, point)};
            const Vec2 e0 = points[1] - points[0];
            const Vec2 e1 = points[2] - points[1];
            const Vec2 e2 = points[0] - points[2];
            const Vec2 v0 = p2 - points[0];
            const Vec2 v1 = p2 - points[1];
            const Vec2 v2 = p2 - points[2];
            const auto dot2 = [](Vec2 a, Vec2 b) { return (a.x * b.x) + (a.y * b.y); };
            const auto towards = [&](Vec2 v, Vec2 e) {
                return v - (e * std::clamp(dot2(v, e) / dot2(e, e), 0.0F, 1.0F));
            };
            const Vec2 pq0 = towards(v0, e0);
            const Vec2 pq1 = towards(v1, e1);
            const Vec2 pq2 = towards(v2, e2);
            const float s = sign_of((e0.x * e2.y) - (e0.y * e2.x));
            const auto min2 = [](Vec2 a, Vec2 b) {
                return Vec2{std::min(a.x, b.x), std::min(a.y, b.y)};
            };
            const Vec2 d2 = min2(min2(Vec2{dot2(pq0, pq0), s * ((v0.x * e0.y) - (v0.y * e0.x))},
                                      Vec2{dot2(pq1, pq1), s * ((v1.x * e1.y) - (v1.y * e1.x))}),
                                 Vec2{dot2(pq2, pq2), s * ((v2.x * e2.y) - (v2.y * e2.x))});
            float inside = -std::sqrt(d2.x) * sign_of(d2.y);
            // The distance to the planes isn't shorter when inside.
            if (inside < 0.0F) {
                inside = std::max(inside, d);
                inside = std::max(inside, -(thickness_ + d));
            }
            closest = std::min(closest, inside);
            return;
        }
        if (d < 0.0F) {
            point = point - (plane.normal * thickness_); // flatten
        }
        // https://iquilezles.org/www/articles/distfunctions/distfunctions.htm
        const Vec3 a = t[0];
        const Vec3 b = t[1];
        const Vec3 c = t[2];
        const Vec3 ba = b - a;
        const Vec3 pa = point - a;
        const Vec3 cb = c - b;
        const Vec3 pb = point - b;
        const Vec3 ac = a - c;
        const Vec3 pc = point - c;
        const Vec3 nor = detail::cross(ba, ac);
        const auto dot2 = [](Vec3 v) { return detail::dot(v, v); };
        const auto edge = [&](Vec3 e, Vec3 p) {
            return dot2((e * std::clamp(detail::dot(e, p) / dot2(e), 0.0F, 1.0F)) - p);
        };
        const bool outside_edges = sign_of(detail::dot(detail::cross(ba, nor), pa)) +
                                       sign_of(detail::dot(detail::cross(cb, nor), pb)) +
                                       sign_of(detail::dot(detail::cross(ac, nor), pc)) <
                                   2.0F;
        const float squared = outside_edges
                                  ? std::min(std::min(edge(ba, pa), edge(cb, pb)), edge(ac, pc))
                                  : detail::dot(nor, pa) * detail::dot(nor, pa) / dot2(nor);
        closest = std::min(closest, std::sqrt(squared));
    }

    std::vector<Triangle> triangles_;
    std::vector<Plane> planes_;
    std::vector<BvhNode> nodes_;
    float thickness_ = 0.0F;
};

int sdf_subdiv(std::string_view resolution) {
    for (const int subdiv : {16, 32, 64, 128, 256, 512}) {
        if (resolution == std::to_string(subdiv)) {
            return subdiv;
        }
    }
    return 0;
}

int height_field_resolution(std::string_view resolution) {
    for (const int texels : {256, 512, 1024}) {
        if (resolution == std::to_string(texels)) {
            return texels;
        }
    }
    return 0;
}

float longest(Vec3 v) {
    return std::max(v.x, std::max(v.y, v.z));
}

} // namespace

std::array<std::uint32_t, 3> particle_sdf_cells(Vec3 size, std::string_view resolution) {
    const int subdiv = sdf_subdiv(resolution);
    if (subdiv == 0 || !(size.x > 0.0F) || !(size.y > 0.0F) || !(size.z > 0.0F) ||
        !std::isfinite(size.x + size.y + size.z)) {
        return {0, 0, 0};
    }
    const float cell_size = longest(size) / static_cast<float>(subdiv);
    const auto cells = [&](float side) {
        return static_cast<std::uint32_t>(std::max(static_cast<int>(side / cell_size), 1));
    };
    return {cells(size.x), cells(size.y), cells(size.z)};
}

std::array<std::uint32_t, 2> particle_height_field_size(Vec3 size, std::string_view resolution) {
    const int texels = height_field_resolution(resolution);
    if (texels == 0 || !(size.x > 0.0F) || !(size.z > 0.0F) || !std::isfinite(size.x + size.z)) {
        return {0, 0};
    }
    const auto most = static_cast<std::uint32_t>(texels);
    // ParticlesStorage::particles_collision_get_heightfield_framebuffer, from the half sizes.
    if (size.x > size.z) {
        return {most, std::max(static_cast<std::uint32_t>(size.z * 0.5F / (size.x * 0.5F) *
                                                          static_cast<float>(most)),
                               1U)};
    }
    return {std::max(static_cast<std::uint32_t>(size.x * 0.5F / (size.z * 0.5F) *
                                                static_cast<float>(most)),
                     1U),
            most};
}

Result<ParticleSdfBake> bake_particle_sdf(const world::World& world, entt::entity entity,
                                          const ModelLoader& models, jobs::Pool* pool) {
    if (!world.valid(entity) || !world.ecs().all_of<world::GPUParticlesCollisionSDF3D>(entity)) {
        return make_error(ErrorCode::InvalidArgument,
                          "the entity has no GPUParticlesCollisionSDF3D");
    }
    const auto& component = world.ecs().get<const world::GPUParticlesCollisionSDF3D>(entity);
    const std::array<std::uint32_t, 3> cells =
        particle_sdf_cells(component.size, component.resolution);
    if (cells[0] == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "resolution \"" + component.resolution +
                              R"(" isn't "16", "32", "64", "128", "256" or "512", or size isn't )"
                              "positive");
    }
    const std::size_t count = std::size_t{cells[0]} * cells[1] * cells[2];
    if (count > render::kMaxParticleSdfCells3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "the SDF's " + std::to_string(count) + " cells are more than " +
                              std::to_string(render::kMaxParticleSdfCells3D) +
                              ": lower its resolution");
    }
    const float cell_size =
        longest(component.size) / static_cast<float>(sdf_subdiv(component.resolution));
    // The box of whole cells, from the corner of the size's.
    const Box box{component.size * -0.5F,
                  Vec3{static_cast<float>(cells[0]), static_cast<float>(cells[1]),
                       static_cast<float>(cells[2])} *
                      cell_size};
    ParticleSdfBake out;
    const world::Tree3D tree(world);
    const Gather gather{.mask = component.bake_mask, .scope = tree.parent(entity)};
    std::vector<Triangle> found;
    out.meshes = gather_triangles(world, tree, tree.global(entity).inverse(), gather, models, found,
                                  out.problems);
    // Only the triangles that touch the box.
    std::vector<Triangle> triangles;
    for (const Triangle& t : found) {
        if (detail::triangle_box_overlap(box.center(), box.size * 0.5F, t)) {
            triangles.push_back(t);
        }
    }
    out.triangles = triangles.size();
    if (triangles.size() <= 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "no triangles to bake: no visible mesh on a layer of bake_mask "
                          "touches the box");
    }
    const SdfBaker baker(std::move(triangles), cell_size * component.thickness);
    out.sdf.width = cells[0];
    out.sdf.height = cells[1];
    out.sdf.depth = cells[2];
    out.sdf.distances.assign(count, 0);
    const Vec3 offset = box.position + Vec3{cell_size * 0.5F, cell_size * 0.5F, cell_size * 0.5F};
    const auto slice = [&](std::size_t z) {
        for (std::uint32_t y = 0; y < cells[1]; ++y) {
            for (std::uint32_t x = 0; x < cells[0]; ++x) {
                const Vec3 p = offset + (Vec3{static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(z)} *
                                         cell_size);
                const std::size_t at = (((z * cells[1]) + y) * cells[0]) + x;
                // Converted to half, as Godot's image; subnormals flushed to 0, which devices read.
                std::uint16_t half = render::float_to_half(baker.distance(p));
                if ((half & 0x7C00U) == 0) {
                    half = 0;
                }
                out.sdf.distances[at] = half;
            }
        }
    };
    if (pool != nullptr) {
        pool->parallel_for(cells[2], slice);
    } else {
        for (std::size_t z = 0; z < cells[2]; ++z) {
            slice(z);
        }
    }
    return out;
}

Result<std::string> encode_particle_sdf(const render::ParticleSdf3D& sdf) {
    if (sdf.width == 0 || sdf.height == 0 || sdf.depth == 0 ||
        sdf.distances.size() != std::size_t{sdf.width} * sdf.height * sdf.depth) {
        return make_error(ErrorCode::InvalidArgument,
                          "an SDF has width times height times depth distances");
    }
    render::Gray16 image;
    image.width = sdf.width;
    image.height = sdf.height * sdf.depth;
    image.values = sdf.distances;
    return render::encode_gray16_png(image);
}

Result<render::ParticleSdf3D> particle_sdf(const render::Gray16& texture, int slices_horizontal,
                                           int slices_vertical) {
    if (slices_horizontal < 1 || slices_vertical < 1) {
        return make_error(ErrorCode::InvalidArgument, "slices must be at least 1");
    }
    const auto across = static_cast<std::uint32_t>(slices_horizontal);
    const auto down = static_cast<std::uint32_t>(slices_vertical);
    render::ParticleSdf3D sdf;
    sdf.width = texture.width / across;
    sdf.height = texture.height / down;
    sdf.depth = across * down;
    if (sdf.width == 0 || sdf.height == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a " + std::to_string(texture.width) + "x" +
                              std::to_string(texture.height) + " texture can't be cut into " +
                              std::to_string(across) + "x" + std::to_string(down) + " slices");
    }
    const std::size_t count = std::size_t{sdf.width} * sdf.height * sdf.depth;
    if (count > render::kMaxParticleSdfCells3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "the SDF's " + std::to_string(count) + " cells are more than " +
                              std::to_string(render::kMaxParticleSdfCells3D));
    }
    if (texture.values.size() < std::size_t{texture.width} * texture.height) {
        return make_error(ErrorCode::InvalidArgument, "the texture has no samples");
    }
    sdf.distances.reserve(count);
    for (std::uint32_t row = 0; row < down; ++row) {
        for (std::uint32_t column = 0; column < across; ++column) {
            for (std::uint32_t y = 0; y < sdf.height; ++y) {
                for (std::uint32_t x = 0; x < sdf.width; ++x) {
                    const std::size_t across_at = (std::size_t{column} * sdf.width) + x;
                    const std::size_t down_at = (std::size_t{row} * sdf.height) + y;
                    sdf.distances.push_back(texture.values[(down_at * texture.width) + across_at]);
                }
            }
        }
    }
    return sdf;
}

render::ParticleHeightField3D particle_height_field(const world::World& world,
                                                    const world::Tree3D& tree, entt::entity entity,
                                                    const ModelLoader& models,
                                                    std::vector<Problem>& problems) {
    render::ParticleHeightField3D out;
    const auto* component =
        world.ecs().try_get<const world::GPUParticlesCollisionHeightField3D>(entity);
    if (component == nullptr) {
        return out;
    }
    const std::array<std::uint32_t, 2> size =
        particle_height_field_size(component->size, component->resolution);
    if (size[0] == 0 || size[1] == 0 || !(component->size.y > 0.0F)) {
        return out;
    }
    out.width = size[0];
    out.height = size[1];
    out.heights.assign(std::size_t{size[0]} * size[1], 0.0F);
    // RendererSceneRenderRD::render_particle_collider_heightfield: the box's half size scaled,
    // a camera half its height above its origin (in world y) looking down its -y with -z up.
    const Affine3D transform = tree.global(entity);
    const auto scale = [](Vec3 axis) { return detail::length(axis); };
    const Vec3 extents{component->size.x * 0.5F * scale(transform.x),
                       component->size.y * 0.5F * scale(transform.y),
                       component->size.z * 0.5F * scale(transform.z)};
    const Vec3 eye = transform.origin + Vec3{0.0F, extents.y, 0.0F};
    const Affine3D look =
        world::looking_at(transform.y * -1.0F, detail::normalized(transform.z * -1.0F));
    Affine3D camera = look;
    camera.origin = eye;
    const Affine3D to_camera = camera.inverse();
    const Gather gather{.mask = component->heightfield_mask,
                        .built_by_layer = true,
                        .opaque_only = true,
                        .terrains = true};
    std::vector<Triangle> triangles;
    gather_triangles(world, tree, to_camera, gather, models, triangles, problems);
    // Each texel's center: x across from -extents.x, y down from extents.z; depth 1 at the camera
    // (Godot's depth is reversed) to 0 twice the half height below it, where the texture clears.
    const auto width = static_cast<float>(out.width);
    const auto height = static_cast<float>(out.height);
    const float depth_range = extents.y * 2.0F;
    for (const Triangle& t : triangles) {
        // Texel space: x from 0 to width, y from 0 to height.
        std::array<Vec3, 3> p;
        for (std::size_t i = 0; i < 3; ++i) {
            p.at(i) = {((t.at(i).x / extents.x) + 1.0F) * 0.5F * width,
                       ((1.0F - (t.at(i).y / extents.z)) * 0.5F) * height,
                       1.0F + (t.at(i).z / depth_range)};
        }
        const float area =
            ((p[1].x - p[0].x) * (p[2].y - p[0].y)) - ((p[2].x - p[0].x) * (p[1].y - p[0].y));
        if (area == 0.0F || !std::isfinite(area)) {
            continue;
        }
        const float low_x = std::min({p[0].x, p[1].x, p[2].x});
        const float high_x = std::max({p[0].x, p[1].x, p[2].x});
        const float low_y = std::min({p[0].y, p[1].y, p[2].y});
        const float high_y = std::max({p[0].y, p[1].y, p[2].y});
        const int x0 = std::max(static_cast<int>(std::ceil(low_x - 0.5F)), 0);
        const int x1 =
            std::min(static_cast<int>(std::floor(high_x - 0.5F)), static_cast<int>(out.width) - 1);
        const int y0 = std::max(static_cast<int>(std::ceil(low_y - 0.5F)), 0);
        const int y1 =
            std::min(static_cast<int>(std::floor(high_y - 0.5F)), static_cast<int>(out.height) - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float cx = static_cast<float>(x) + 0.5F;
                const float cy = static_cast<float>(y) + 0.5F;
                const auto edge = [&](const Vec3& a, const Vec3& b) {
                    return ((b.x - a.x) * (cy - a.y)) - ((cx - a.x) * (b.y - a.y));
                };
                float w0 = edge(p[1], p[2]);
                float w1 = edge(p[2], p[0]);
                float w2 = edge(p[0], p[1]);
                if (area < 0.0F) {
                    w0 = -w0;
                    w1 = -w1;
                    w2 = -w2;
                }
                if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) {
                    continue;
                }
                const float sum = std::abs(area);
                const float d = ((w0 * p[0].z) + (w1 * p[1].z) + (w2 * p[2].z)) / sum;
                // Clipped to the box's top and bottom.
                if (!std::isfinite(d) || d < 0.0F || d > 1.0F) {
                    continue;
                }
                float& texel = out.heights[(static_cast<std::size_t>(y) * out.width) +
                                           static_cast<std::size_t>(x)];
                texel = std::max(texel, d);
            }
        }
    }
    return out;
}

void add_particle_sdf_type(asset::Assets& assets) {
    assets.add_type<render::Gray16>(
        "particle_sdf", {std::string(kParticleSdfSuffix)},
        [](std::string_view bytes, std::string_view /*path*/) {
            return render::decode_gray16(bytes);
        },
        [](const render::Gray16& map) {
            return sizeof(render::Gray16) + (map.values.capacity() * sizeof(std::uint16_t));
        });
}

std::vector<Problem> check_particle_colliders(const world::World& world) {
    std::vector<Problem> problems;
    const auto positive = [](Vec3 size) {
        return size.x > 0.0F && size.y > 0.0F && size.z > 0.0F &&
               std::isfinite(size.x + size.y + size.z);
    };
    for (const entt::entity entity : made<world::GPUParticlesCollisionSDF3D>(world)) {
        const auto& sdf = world.ecs().get<const world::GPUParticlesCollisionSDF3D>(entity);
        const std::string base = "/components/GPUParticlesCollisionSDF3D/";
        if (sdf_subdiv(sdf.resolution) == 0) {
            problems.push_back(
                {entity, base + "resolution",
                 "\"" + sdf.resolution + R"(" isn't "16", "32", "64", "128", "256" or "512")"});
        }
        if (!positive(sdf.size)) {
            problems.push_back({entity, base + "size", "must be positive"});
        }
        if (!(sdf.thickness >= 0.0F)) {
            problems.push_back({entity, base + "thickness", "must be 0 or more"});
        }
        if (sdf.texture.empty()) {
            continue;
        }
        const std::string field = base + "texture";
        if (!sdf.texture.ends_with(kParticleSdfSuffix)) {
            problems.push_back(
                {entity, field,
                 "\"" + sdf.texture + "\" isn't a " + std::string(kParticleSdfSuffix) + " file"});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto bytes = reader(sdf.texture);
        if (!bytes) {
            problems.push_back(
                {entity, field,
                 "\"" + sdf.texture + "\" could not be read: " + bytes.error().message});
            continue;
        }
        auto texture = render::decode_gray16(bytes.value());
        if (!texture) {
            problems.push_back(
                {entity, field, "\"" + sdf.texture + "\": " + texture.error().message});
            continue;
        }
        if (auto cut = particle_sdf(texture.value(), sdf.slices_horizontal, sdf.slices_vertical);
            !cut) {
            problems.push_back({entity, field, "\"" + sdf.texture + "\": " + cut.error().message});
        }
    }
    for (const entt::entity entity : made<world::GPUParticlesCollisionHeightField3D>(world)) {
        const auto& field =
            world.ecs().get<const world::GPUParticlesCollisionHeightField3D>(entity);
        const std::string base = "/components/GPUParticlesCollisionHeightField3D/";
        if (height_field_resolution(field.resolution) == 0) {
            problems.push_back({entity, base + "resolution",
                                "\"" + field.resolution + R"(" isn't "256", "512" or "1024")"});
        }
        if (field.update_mode != "when_moved" && field.update_mode != "always") {
            problems.push_back({entity, base + "update_mode",
                                "\"" + field.update_mode + R"(" isn't "when_moved" or "always")"});
        }
        if (!positive(field.size)) {
            problems.push_back({entity, base + "size", "must be positive"});
        }
    }
    return problems;
}

} // namespace slash::scene3d
