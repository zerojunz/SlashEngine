#include "slash/scene3d/surface_tool.hpp"

#include "model_internal.hpp"

#include "slash/asset/assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace slash::scene3d {
namespace {

std::uint8_t byte_of(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return length > 0.0F ? v * (1.0F / length) : Vec3{};
}

// Godot's Plane(p1, p2, p3).normal, for clockwise front faces.
Vec3 face_normal(Vec3 p1, Vec3 p2, Vec3 p3) {
    const Vec3 a = p1 - p3;
    const Vec3 b = p1 - p2;
    return normalized(
        {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)});
}

// A position and smooth group, keyed by the position's bits as Godot's SmoothGroupVertex
// compares them.
using SmoothKey = std::pair<std::array<std::uint32_t, 3>, std::uint32_t>;

SmoothKey smooth_key(const render::Vertex3D& vertex, std::uint32_t group) {
    SmoothKey key{{}, group};
    std::memcpy(key.first.data(), vertex.position.data(), sizeof(key.first));
    return key;
}

} // namespace

void SurfaceTool::add_vertex(Vec3 position) {
    Vertex v;
    v.vertex.position = {static_cast<float>(position.x), static_cast<float>(position.y),
                         static_cast<float>(position.z)};
    v.vertex.normal = {static_cast<float>(normal_.x), static_cast<float>(normal_.y),
                       static_cast<float>(normal_.z)};
    v.vertex.tangent = {tangent_.x, tangent_.y, tangent_.z, tangent_.w};
    v.vertex.uv = {static_cast<float>(uv_.x), static_cast<float>(uv_.y)};
    v.vertex.uv2 = {static_cast<float>(uv2_.x), static_cast<float>(uv2_.y)};
    v.vertex.rgba = {byte_of(color_.x), byte_of(color_.y), byte_of(color_.z), byte_of(color_.w)};
    v.smooth_group = smooth_group_;
    vertices_.push_back(v);
}

void SurfaceTool::deindex() {
    if (indices_.empty()) {
        return;
    }
    std::vector<Vertex> out;
    out.reserve(indices_.size());
    for (const std::uint32_t index : indices_) {
        if (index < vertices_.size()) {
            out.push_back(vertices_[index]);
        }
    }
    vertices_ = std::move(out);
    indices_.clear();
}

void SurfaceTool::index() {
    if (!indices_.empty()) {
        return;
    }
    // Equal vertices, smooth group included, merge in the order they first appear.
    std::map<std::string, std::uint32_t> seen;
    std::vector<Vertex> out;
    for (const Vertex& vertex : vertices_) {
        std::string key(sizeof(render::Vertex3D) + sizeof(std::uint32_t), '\0');
        std::memcpy(key.data(), &vertex.vertex, sizeof(render::Vertex3D));
        std::memcpy(key.data() + sizeof(render::Vertex3D), &vertex.smooth_group,
                    sizeof(std::uint32_t));
        const auto [it, added] =
            seen.emplace(std::move(key), static_cast<std::uint32_t>(out.size()));
        if (added) {
            out.push_back(vertex);
        }
        indices_.push_back(it->second);
    }
    vertices_ = std::move(out);
}

void SurfaceTool::generate_normals(bool flip) {
    const bool was_indexed = !indices_.empty();
    deindex();
    if (vertices_.size() % 3 != 0) {
        return;
    }
    std::map<SmoothKey, Vec3> smooth;
    const auto position = [](const Vertex& v) {
        return Vec3{v.vertex.position[0], v.vertex.position[1], v.vertex.position[2]};
    };
    for (std::size_t i = 0; i < vertices_.size(); i += 3) {
        Vertex* v = &vertices_[i];
        const Vec3 normal = flip ? face_normal(position(v[2]), position(v[1]), position(v[0]))
                                 : face_normal(position(v[0]), position(v[1]), position(v[2]));
        for (int k = 0; k < 3; ++k) {
            if (v[k].smooth_group != kFlat) {
                const auto [it, added] =
                    smooth.emplace(smooth_key(v[k].vertex, v[k].smooth_group), normal);
                if (!added) {
                    it->second = it->second + normal;
                }
            } else {
                v[k].vertex.normal = {static_cast<float>(normal.x), static_cast<float>(normal.y),
                                      static_cast<float>(normal.z)};
            }
        }
    }
    for (Vertex& vertex : vertices_) {
        if (vertex.smooth_group != kFlat) {
            const Vec3 n = normalized(smooth.at(smooth_key(vertex.vertex, vertex.smooth_group)));
            vertex.vertex.normal = {static_cast<float>(n.x), static_cast<float>(n.y),
                                    static_cast<float>(n.z)};
        }
    }
    if (was_indexed) {
        index();
    }
}

void SurfaceTool::generate_tangents() {
    const bool was_indexed = !indices_.empty();
    deindex();
    if (vertices_.size() % 3 != 0) {
        return;
    }
    // MikkTSpace as the importers run it, on counter-clockwise triangles.
    Triangles triangles;
    for (const Vertex& vertex : vertices_) {
        triangles.vertices.push_back(vertex.vertex);
    }
    for (std::uint32_t i = 0; i < vertices_.size(); i += 3) {
        triangles.indices.insert(triangles.indices.end(), {i, i + 2, i + 1});
    }
    scene3d::generate_tangents(triangles);
    // Each corner's tangent goes back to the vertex it came from, matched by everything else.
    std::vector<Vertex> out;
    out.reserve(vertices_.size());
    for (std::size_t i = 0; i + 2 < triangles.indices.size(); i += 3) {
        for (const std::size_t k : {std::size_t{0}, std::size_t{2}, std::size_t{1}}) {
            Vertex vertex = vertices_[out.size()];
            vertex.vertex.tangent = triangles.vertices[triangles.indices[i + k]].tangent;
            out.push_back(vertex);
        }
    }
    vertices_ = std::move(out);
    if (was_indexed) {
        index();
    }
}

Result<void> SurfaceTool::commit(Model& model) {
    const std::size_t count = indices_.empty() ? vertices_.size() : indices_.size();
    if (count % 3 != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a surface is triangles, so its " +
                              std::string(indices_.empty() ? "vertices" : "indices") +
                              " come in threes; it has " + std::to_string(count));
    }
    if (vertices_.size() > render::kMaxMeshVertices || count > render::kMaxMeshIndices) {
        return make_error(ErrorCode::InvalidArgument,
                          "a surface has at most " + std::to_string(render::kMaxMeshVertices) +
                              " vertices and " + std::to_string(render::kMaxMeshIndices) +
                              " indices");
    }
    for (std::size_t i = 0; i < indices_.size(); ++i) {
        if (indices_[i] >= vertices_.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "index " + std::to_string(i) + " is " + std::to_string(indices_[i]) +
                                  ", past the " + std::to_string(vertices_.size()) + " vertices");
        }
    }
    ModelSurface surface;
    surface.vertices.reserve(vertices_.size());
    for (const Vertex& vertex : vertices_) {
        surface.vertices.push_back(vertex.vertex);
    }
    if (indices_.empty()) {
        surface.indices.resize(vertices_.size());
        for (std::uint32_t i = 0; i < surface.indices.size(); ++i) {
            surface.indices[i] = i;
        }
    } else {
        surface.indices = indices_;
    }
    if (material_) {
        ModelMaterial material;
        material.name = "surface_" + std::to_string(model.materials.size());
        material.standard = *material_;
        surface.material = static_cast<int>(model.materials.size());
        model.materials.push_back(std::move(material));
    }
    if (model.meshes.empty()) {
        model.meshes.push_back({.name = "mesh", .surfaces = {}});
        model.parts.push_back(
            {.node = "mesh", .mesh = 0, .transform = {}, .node_index = -1, .skin = -1});
    }
    model.meshes.front().surfaces.push_back(std::move(surface));
    clear();
    return {};
}

Result<std::string> make_mesh(world::World& world, std::string_view name, Model model) {
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "the world has no assets to make a mesh in (canvas::install attaches "
                          "them)");
    }
    std::string path = std::string(asset::kMadePrefix) + std::string(name);
    const std::size_t bytes = model_bytes(model);
    if (auto put = assets->put<Model>(path, std::move(model), bytes); !put) {
        return put.error();
    }
    return path;
}

} // namespace slash::scene3d
