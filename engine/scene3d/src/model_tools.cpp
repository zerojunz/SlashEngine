#include "model_internal.hpp"

#include <cmath>
#include <cstring>
#include <mikktspace.h>
#include <string>
#include <unordered_map>
#include <utility>

namespace slash::scene3d {
namespace {

// MikkTSpace's callbacks over deindexed triangles. Texture coordinates go in with v flipped,
// since MikkTSpace takes v up, as Blender bakes normal maps; that gives the tangents and signs
// Godot's SurfaceTool::generate_tangents makes.
int faces(const SMikkTSpaceContext* context) {
    return static_cast<int>(static_cast<Triangles*>(context->m_pUserData)->indices.size() / 3);
}
int corners(const SMikkTSpaceContext* /*context*/, int /*face*/) {
    return 3;
}
render::Vertex3D& corner(const SMikkTSpaceContext* context, int face, int vert) {
    auto* triangles = static_cast<Triangles*>(context->m_pUserData);
    return triangles
        ->vertices[(static_cast<std::size_t>(face) * 3) + static_cast<std::size_t>(vert)];
}
void get_position(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    std::memcpy(out, corner(context, face, vert).position.data(), sizeof(float) * 3);
}
void get_normal(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    std::memcpy(out, corner(context, face, vert).normal.data(), sizeof(float) * 3);
}
void get_uv(const SMikkTSpaceContext* context, float out[], int face, int vert) {
    const render::Vertex3D& v = corner(context, face, vert);
    out[0] = v.uv[0];
    out[1] = 1.0F - v.uv[1];
}
void set_tangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face,
                 int vert) {
    corner(context, face, vert).tangent = {tangent[0], tangent[1], tangent[2], sign};
}

} // namespace

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::optional<std::string> normalized(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, slash - start);
        if (part == "..") {
            if (parts.empty()) {
                return std::nullopt;
            }
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = slash + 1;
    }
    std::string out;
    for (const std::string_view part : parts) {
        out += (out.empty() ? "" : "/") + std::string(part);
    }
    return out;
}

Triangles deindexed(const Triangles& in) {
    Triangles out;
    out.vertices.reserve(in.indices.size());
    out.indices.reserve(in.indices.size());
    for (const std::uint32_t index : in.indices) {
        out.indices.push_back(static_cast<std::uint32_t>(out.vertices.size()));
        out.vertices.push_back(in.vertices[index]);
    }
    return out;
}

void reindex(Triangles& triangles) {
    std::unordered_map<std::string, std::uint32_t> seen;
    std::vector<render::Vertex3D> vertices;
    for (std::uint32_t& index : triangles.indices) {
        const render::Vertex3D& vertex = triangles.vertices[index];
        std::string key(sizeof(render::Vertex3D), '\0');
        std::memcpy(key.data(), &vertex, sizeof(vertex));
        const auto [it, added] =
            seen.emplace(std::move(key), static_cast<std::uint32_t>(vertices.size()));
        if (added) {
            vertices.push_back(vertex);
        }
        index = it->second;
    }
    triangles.vertices = std::move(vertices);
}

Vec3 position_of(const render::Vertex3D& v) {
    return {v.position[0], v.position[1], v.position[2]};
}

void flat_normals(Triangles& triangles) {
    triangles = deindexed(triangles);
    for (std::size_t i = 0; i + 2 < triangles.indices.size(); i += 3) {
        render::Vertex3D* corner = &triangles.vertices[i];
        const Vec3 a = position_of(corner[1]) - position_of(corner[0]);
        const Vec3 b = position_of(corner[2]) - position_of(corner[0]);
        Vec3 n{(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
        const float length = std::sqrt((n.x * n.x) + (n.y * n.y) + (n.z * n.z));
        n = length > 0.0F ? n * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
        for (int k = 0; k < 3; ++k) {
            corner[k].normal = {n.x, n.y, n.z};
        }
    }
}

void generate_tangents(Triangles& triangles) {
    triangles = deindexed(triangles);
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = faces;
    callbacks.m_getNumVerticesOfFace = corners;
    callbacks.m_getPosition = get_position;
    callbacks.m_getNormal = get_normal;
    callbacks.m_getTexCoord = get_uv;
    callbacks.m_setTSpaceBasic = set_tangent;
    const SMikkTSpaceContext context{.m_pInterface = &callbacks, .m_pUserData = &triangles};
    genTangSpaceDefault(&context);
    reindex(triangles);
}

render::Vertex3D dummy_tangent(render::Vertex3D vertex) {
    const Vec3 n{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
    const Vec3 a{n.z, -n.x, n.y};
    const float length = std::sqrt((n.x * n.x) + (n.y * n.y) + (n.z * n.z));
    const Vec3 u = length > 0.0F ? n * (1.0F / length) : n;
    Vec3 t{(a.y * u.z) - (a.z * u.y), (a.z * u.x) - (a.x * u.z), (a.x * u.y) - (a.y * u.x)};
    const float t_length = std::sqrt((t.x * t.x) + (t.y * t.y) + (t.z * t.z));
    t = t_length > 0.0F ? t * (1.0F / t_length) : Vec3{1.0F, 0.0F, 0.0F};
    vertex.tangent = {t.x, t.y, t.z, 1.0F};
    return vertex;
}

void dummy_tangents(Triangles& triangles) {
    for (render::Vertex3D& v : triangles.vertices) {
        v = dummy_tangent(v);
    }
}

} // namespace slash::scene3d
