#pragma once

#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the model readers (model.cpp for glTF, obj.cpp for OBJ) share.
namespace slash::scene3d {

std::string quoted(std::string_view text);

// path with its "." and ".." parts worked out; nullopt if it climbs out of the project.
std::optional<std::string> normalized(std::string_view path);

// A surface's corners and triangles, before Godot's winding.
struct Triangles {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices; // counter-clockwise, as glTF's and OBJ's
};

// Each triangle's corners as vertices of their own.
Triangles deindexed(const Triangles& in);

// Equal vertices merged into one, in the order they first appear, as Godot's SurfaceTool::index.
void reindex(Triangles& triangles);

Vec3 position_of(const render::Vertex3D& v);

// Flat normals, as the glTF specification asks of a mesh without them: each triangle's corners
// take its face's normal.
void flat_normals(Triangles& triangles);

// MikkTSpace's tangents, as Godot's SurfaceTool::generate_tangents makes them.
void generate_tangents(Triangles& triangles);

// Godot's dummy tangent for a mesh with normals but no texture coordinates.
render::Vertex3D dummy_tangent(render::Vertex3D vertex);
void dummy_tangents(Triangles& triangles);

// Reads a Wavefront OBJ model (obj.cpp). Problems that leave a model drawing, such as a material
// library or texture it can't find, go in problems when it isn't null.
Result<Model> load_obj(std::string_view bytes, std::string_view path, const ModelReader& read,
                       std::vector<json::ValidationError>* problems);

} // namespace slash::scene3d
