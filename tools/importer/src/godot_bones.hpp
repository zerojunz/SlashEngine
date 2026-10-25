#pragma once

#include "slash/scene3d/model.hpp"

#include <cstddef>
#include <string>
#include <vector>

// The Skeleton3D nodes Godot's glTF and FBX importers make of a model's skins, as its SkinTool
// (modules/gltf/skin_tool.cpp) and GLTFDocument::_generate_skeleton_bone_node make them, so a
// scene's overrides of their bones, and the modifiers it adds under them, convert.
namespace slash::importer::godot {

struct ModelSkeleton {
    // Its node path in the model's scene, from the scene's root: "Armature/Skeleton3D".
    std::string path;
    // Its bones in Godot's order, so a property bones/N names bones[N]: into Model::nodes.
    std::vector<std::size_t> bones;
};

// The skeletons Godot makes of a model, in the order it makes them; none for a model without
// skins. Their bones are the skins' joints and the nodes between them, depth first from the
// roots, with roots and children in the order of their index in the file
// (ModelNode::file_index), as SkinTool::_create_skeletons orders them.
[[nodiscard]] std::vector<ModelSkeleton> model_skeletons(const scene3d::Model& model);

} // namespace slash::importer::godot
