#pragma once

#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the model readers (model.cpp for glTF, obj.cpp for OBJ, fbx.cpp for FBX) share.
namespace slash::scene3d {

std::string quoted(std::string_view text);

// Problems that leave a model drawing, at the file's top level, when the caller wants them.
struct Problems {
    std::vector<json::ValidationError>* out = nullptr;

    void add(std::string message) const {
        if (out != nullptr) {
            out->push_back({"", std::move(message)});
        }
    }
};

using NameSet = std::set<std::string, std::less<>>;

// Puts a model's meshes, materials, images, nodes, parts, skins and collision into another under
// a node (parent, or -1 for the root), as Godot instances a scene under one: node names kept
// unless taken (then numbered), a model without nodes given one for each part, the parts placed
// by place in the model's space, and materials it has already shared. Animations stay with the
// file. Model recipes' model parts and vehicles' bodies go in this way.
void add_model(Model& into, const Model& from, int parent, const world::Affine3D& place,
               std::set<std::string, std::less<>>& taken);

// Godot's importers' _gen_unique_name: name with the characters a node's name (or an animation's,
// which also can't have ',' or '[') can't have made underscores, numbered from 2 when it is taken.
std::string unique_name(NameSet& taken, std::string_view name, bool animation = false);

// The names a model's nodes can't take: "Skeleton3D", which Godot's importers keep for the
// skeleton.
NameSet node_names();

// Godot's import hint: an animation whose name begins or ends with "loop" or "cycle", in any case,
// loops.
bool loops_by_name(std::string_view name);

// Godot's Color::linear_to_srgb, for one channel.
float linear_to_srgb(float c);

// path with its "." and ".." parts worked out; nullopt if it climbs out of the project.
std::optional<std::string> normalized(std::string_view path);

// A surface's corners and triangles, before Godot's winding.
struct Triangles {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices; // counter-clockwise, as glTF's and OBJ's
    std::vector<VertexWeights> weights; // one per vertex for a skinned surface, else empty
    // For each vertex, the one it came from in the file, so blend shapes' offsets follow it when
    // vertices split or merge; empty where nothing needs it.
    std::vector<std::uint32_t> origins;
};

// Each triangle's corners as vertices of their own.
Triangles deindexed(const Triangles& in);

// Blend shapes' offsets for the vertices of a surface made from the file's, each vertex taking
// those of the one origins says it came from.
std::vector<SurfaceBlendShape> follow_origins(const std::vector<SurfaceBlendShape>& shapes,
                                              const std::vector<std::uint32_t>& origins);

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

// Reads an FBX model, binary or ASCII (fbx.cpp). Problems that leave a model drawing, such as a
// texture it can't find or a warning ufbx gives, go in problems when it isn't null.
Result<Model> load_fbx(std::string_view bytes, std::string_view path, const ModelReader& read,
                       std::vector<json::ValidationError>* problems);

} // namespace slash::scene3d
