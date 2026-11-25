// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include "slash/scene3d/model.hpp"

#include <cstddef>
#include <string>
#include <vector>

// The Skeleton3D nodes made of a model's skins: which nodes are a skeleton's bones, so
// importers convert a scene's overrides of them and retargeting (humanoid.hpp) works on the
// same bones.
namespace slash::scene3d {

struct ModelSkeleton {
    // Its node path in the model's scene, from the scene's root: "Armature/Skeleton3D".
    std::string path;
    // Its bones in order, so a property bones/N names bones[N]: into Model::nodes.
    std::vector<std::size_t> bones;
};

// The skeletons made of a model, in the order it makes them; none for a model without
// skins. Their bones are the skins' joints and the nodes between them, depth first from the
// roots, with roots and children in the order of their index in the file
// (ModelNode::file_index).
[[nodiscard]] std::vector<ModelSkeleton> model_skeletons(const Model& model);

} // namespace slash::scene3d
