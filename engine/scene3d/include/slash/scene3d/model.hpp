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

// 3D models, read as Godot's importers read them: glTF 2.0 as modules/gltf/gltf_document.cpp
// (.gltf files, their buffers and images beside them or in data URIs, and .glb files with
// everything in one), Wavefront OBJ as editor/import/3d/resource_importer_obj.cpp (.obj files
// and the .mtl material libraries and textures beside them), and FBX as modules/fbx/
// fbx_document.cpp reads it with ufbx (.fbx files, binary or ASCII, and their textures inside them
// or beside them). Blender's .blend files are read as the glTF Blender exports of them
// (blend.hpp). MeshInstance3D's mesh draws one.
// Meshes keep Godot's clockwise front faces, and a model with no tangents gets MikkTSpace's, as
// Godot's importers make them.
namespace slash::scene3d {

// Reads a file a model names, by project path.
using ModelReader = std::function<Result<std::string>(std::string_view path)>;

// A texture a model's material samples: a file beside the model, read as any texture is, or an
// image inside the model (a .glb's buffer, a data URI or an FBX file's video), decoded.
struct ModelImage {
    std::string path; // its project path, or "" for an image inside the model
    render::Image image;
};

// A model's material as Godot's importer makes it a StandardMaterial3D. Its texture fields are
// left empty: the *_image fields say which of the model's images each one samples, or -1 for
// none, read from the channels standard's *_texture_channel fields name (a glTF model's metallic
// and roughness share one image, read from its blue and green).
struct ModelMaterial {
    std::string name;
    world::StandardMaterial3D standard;
    int albedo_image = -1;
    int metallic_image = -1;
    int roughness_image = -1;
    int normal_image = -1;
    int emission_image = -1;
};

// How a vertex of a skinned surface follows its skin's joints, as Godot's bone indices and
// weights: up to eight joints (indices into the skin's joints) and how much each moves it, adding
// up to 1. Unused places have a weight of 0.
struct VertexWeights {
    std::array<std::uint16_t, 8> joints{};
    std::array<float, 8> weights{};

    friend bool operator==(const VertexWeights&, const VertexWeights&) = default;
};

// One part of a mesh drawn with one material, as a glTF primitive or an OBJ material's faces
// become a surface of Godot's ArrayMesh: triangles only, with Godot's clockwise front faces.
struct ModelSurface {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    int material = -1; // into Model::materials, or -1 for Godot's default material
    // One per vertex for a surface a skin deforms; empty for one that doesn't.
    std::vector<VertexWeights> weights;
};

struct ModelMesh {
    std::string name;
    std::vector<ModelSurface> surfaces;
};

// A mesh where a node of the model's scene puts it.
struct ModelPart {
    std::string node;          // the node's name
    std::size_t mesh = 0;      // into Model::meshes
    world::Affine3D transform; // where the node puts it at rest, in the model's space
    int node_index = -1;       // into Model::nodes; -1 for a model without them (OBJ)
    // Into Model::skins for a mesh a skin deforms, which its joints place instead of its node,
    // as the glTF specification asks; -1 for none.
    int skin = -1;
};

// A node of the model's scene, as Godot's importer makes it a Node3D or a skeleton's bone: its
// name, unique in the model as Godot's importer makes it, its parent (into Model::nodes, or -1),
// and where it is in its parent at rest.
struct ModelNode {
    std::string name;
    // Its name in the file, before Godot's naming rules made name of it ("mixamorig:Hips" for
    // "mixamorig_Hips"), by which other engines' scenes name the node; "" for none.
    std::string file_name;
    int parent = -1;
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};
    // Its index in the file (a glTF node's index, an FBX node's typed_id), by which Godot's
    // importer orders a skeleton's bones; -1 where the file has none.
    int file_index = -1;
};

// A skin, as glTF's and Godot's Skin: the nodes that are its joints, each with its inverse bind
// matrix, which takes a vertex from the skinned mesh's space into the joint's at rest.
struct ModelSkin {
    std::string name;
    std::vector<std::size_t> joints; // into Model::nodes
    std::vector<world::Affine3D> inverse_binds;
};

// Which part of a node a track moves, as Godot's position_3d, rotation_3d and scale_3d tracks.
enum class NodeProperty : std::uint8_t { Position, Rotation, Scale };

// A track of a model's animation: one node's position, rotation or scale over time, as Godot's
// importer makes a glTF channel a 3D track. Values are xyz, or a rotation's quaternion xyzw.
// Keys interpolate linearly (rotations the shorter way round, as Godot's slerp), or hold until
// the next with step set, as Godot's nearest interpolation.
struct NodeTrack {
    std::size_t node = 0; // into Model::nodes
    NodeProperty property = NodeProperty::Position;
    bool step = false;
    // In a looping animation, move from the last key to the first across the loop's end, as
    // Godot's loop wrap.
    bool loop_wrap = true;
    std::vector<float> times; // seconds, earliest first
    std::vector<std::array<float, 4>> values;
};

// An animation in a model file, as Godot's importer makes it an Animation: as long as its last
// key, and looping (linearly) when its name ends with Godot's "-loop" or "-cycle" import hint.
struct ModelAnimation {
    std::string name;
    double length = 0.0;
    bool loop = false;
    std::vector<NodeTrack> tracks;
    // The frames a second of the file's timeline, which tools that count in frames go by: an FBX
    // file's; 0 when the file counts in seconds only, as glTF files do.
    double frame_rate = 0.0;
};

struct Model {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelImage> images;
    // The meshes the model draws: those of its scene's nodes, in tree order.
    std::vector<ModelPart> parts;
    // The scene's nodes in tree order, parents before children; empty for an OBJ model.
    std::vector<ModelNode> nodes;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimation> animations;
};

// Whether a path names a model file: ".gltf", ".glb", ".obj", ".fbx" or ".blend".
[[nodiscard]] bool is_model_path(std::string_view path);

// Reads a model from its file's bytes; path is its project path, which the files it names are
// relative to, read with read. Points and lines are left out, since only triangles draw. glTF
// files that need extensions it doesn't read, such as Draco compression, are refused, naming
// them. An OBJ model whose material library can't be read loads with Godot's default material,
// and an OBJ or FBX model whose textures can't be found loads without them, as Godot's importers.
// A .blend file's bytes must be the glTF Blender made of it (blend.hpp).
[[nodiscard]] Result<Model> load_model(std::string_view bytes, std::string_view path,
                                       const ModelReader& read);

// A model as a glTF 2.0 file, as Godot's GLTFDocument exports one, which load_model reads back
// as written: its nodes (or for a model without them, as an OBJ model, a node for each part,
// where it puts it), the meshes they place, the materials and the images those sample. path is
// the project path it is written to. An image beside the model is named by its path relative to
// path, and one inside it goes in as a PNG; a material's texture paths, where it names no image
// of the model, are written as images too. Everything goes in one text file, its buffer in a data
// URI. A metallic or roughness texture goes in only when they share one image, blue for metallic
// and green for roughness, as glTF's do. Skins and animations are left out: a skinned mesh is
// placed by its node.
[[nodiscard]] Result<std::string> write_gltf(const Model& model, std::string_view path);

// Every problem in a model file: why load_model refuses it, at the file's top level ("") since
// model files are not always JSON, or else each image file beside it that can't be read or
// decoded, at a glTF model's "/images/<index>/uri". OBJ and FBX models' problems are all at "": an
// OBJ model's messages name the line, as do its material libraries' and the textures they name,
// and an FBX model's name its textures and give ufbx's warnings. None for a model that draws as
// written.
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

// Adds the "model" asset type, .gltf, .glb, .obj, .fbx and .blend files as Model, the files they
// name read through assets, so asset.list shows them. A model reloads when its own file changes.
// A .blend file loads only from files that read it as its glTF (scene3d::with_blender).
void add_asset_types(asset::Assets& assets);

} // namespace slash::scene3d
