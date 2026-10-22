#pragma once

#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::asset {
class Assets;
}

// glTF 2.0 models, read as Godot's glTF importer reads them (modules/gltf/gltf_document.cpp):
// .gltf files, their buffers and images beside them or in data URIs, and .glb files with
// everything in one. MeshInstance3D's mesh draws one. Meshes keep Godot's clockwise front faces,
// and a model with no tangents gets MikkTSpace's, as Godot's importer makes them.
namespace slash::scene3d {

// Reads a file a model names, by project path.
using ModelReader = std::function<Result<std::string>(std::string_view path)>;

// A texture a model's material samples: a file beside the model, read as any texture is, or an
// image inside it (a .glb's buffer or a data URI), decoded.
struct ModelImage {
    std::string path; // its project path, or "" for an image inside the model
    render::Image image;
};

// A glTF material as Godot's importer makes it a StandardMaterial3D. Its texture fields are left
// empty: the *_image fields say which of the model's images each one samples, or -1 for none.
struct ModelMaterial {
    std::string name;
    world::StandardMaterial3D standard;
    int albedo_image = -1;
    int metallic_roughness_image = -1; // metallic from blue, roughness from green
    int normal_image = -1;
    int emission_image = -1;
};

// One part of a mesh drawn with one material, as a glTF primitive becomes a surface of Godot's
// ArrayMesh: triangles only, with Godot's clockwise front faces.
struct ModelSurface {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    int material = -1; // into Model::materials, or -1 for Godot's default material
};

struct ModelMesh {
    std::string name;
    std::vector<ModelSurface> surfaces;
};

// A mesh where a node of the model's scene puts it.
struct ModelPart {
    std::string node;     // the node's name
    std::size_t mesh = 0; // into Model::meshes
    world::Affine3D transform;
};

struct Model {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelImage> images;
    // The meshes the model draws: those of its scene's nodes, in tree order.
    std::vector<ModelPart> parts;
};

// Whether a path names a model file: ".gltf" or ".glb".
[[nodiscard]] bool is_model_path(std::string_view path);

// Reads a model from its file's bytes; path is its project path, which the files it names are
// relative to, read with read. Points and lines are left out, since only triangles draw. Files
// that need extensions it doesn't read, such as Draco compression, are refused, naming them.
[[nodiscard]] Result<Model> load_model(std::string_view bytes, std::string_view path,
                                       const ModelReader& read);

// Every problem in a model file: why load_model refuses it, at the file's top level ("") since
// glTF files are not always JSON, or else each image file beside it that can't be read or
// decoded, at its "/images/<index>/uri". None for a model that draws as written.
[[nodiscard]] std::vector<json::ValidationError>
check_model(std::string_view bytes, std::string_view path, const ModelReader& read);

// Where a model's vertices reach, in its own space: the parts' corners as their nodes place
// them. Both are zero for a model with no vertices.
struct Bounds {
    Vec3 min;
    Vec3 max;
};
[[nodiscard]] Bounds bounds(const Model& model);

// What a model holds in memory, for asset.list and perf.stats.
[[nodiscard]] std::size_t model_bytes(const Model& model);

// Adds the "model" asset type, .gltf and .glb files as Model, their buffers and images read
// through assets, so asset.list shows them. A model reloads when its own file changes.
void add_asset_types(asset::Assets& assets);

} // namespace slash::scene3d
