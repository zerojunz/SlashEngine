#pragma once

#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"

#include <map>
#include <span>
#include <string>
#include <vector>

// Blend shapes (morph targets), as Godot's ArrayMesh blend shapes, MeshInstance3D's
// blend_shapes/<name> properties and its AnimationMixer's blend shape tracks:
//
//   std::map<std::string, float> weights =
//       scene3d::blend_shape_weights(model, clips, instance.blend_shapes, load);
//   scene3d::blend_vertices(surface, scene3d::mesh_weights(mesh, weights), vertices);
//
// An entity's model draws every mesh it has, so a name moves the shape of that name in each of
// them: a face, its teeth and its tongue can share "jawOpen".
namespace slash::scene3d {

// A blend shape track's weight at a time, from 0 to the animation's length, as sample_track
// takes a node track's value.
[[nodiscard]] float sample_blend_track(const BlendShapeTrack& track, double time, double length,
                                       bool loop);

// The weight of each of a model's blend shapes, by name, as an entity draws it: the weight its
// mesh gives the shape, or the one set (MeshInstance3D's blend_shapes) gives that name, then
// moved by the clips' blend shape tracks as Godot's AnimationMixer blends them, each clip moving
// its weight of the way from that value to its track's (a clip's node_weights, by the name of
// the node a track's shape is on, take the place of its weight). A clip plays from its source or
// its model loaded with load, as pose_of plays them. Names that no mesh of the model has are
// left out.
[[nodiscard]] std::map<std::string, float>
blend_shape_weights(const Model& model, std::span<const ClipSample> clips,
                    const std::map<std::string, float>& set, const ModelLoader& load);

// A mesh's blend shapes' weights, in its order, from weights by name; a name weights hasn't is
// the mesh's own weight.
[[nodiscard]] std::vector<float> mesh_weights(const ModelMesh& mesh,
                                              const std::map<std::string, float>& weights);

// Whether any weight moves a vertex: Godot's skeleton shader skips weights within 0.0001 of 0.
[[nodiscard]] bool any_blend(std::span<const float> weights);

// A surface's vertices moved by its blend shapes, as Godot's skeleton shader moves them: each
// position plus each shape's offset times its weight, and each normal and tangent the same way,
// then normalized (a tangent keeps its sign). weights are mesh_weights' for its mesh.
void blend_vertices(const ModelSurface& surface, std::span<const float> weights,
                    std::vector<render::Vertex3D>& out);

} // namespace slash::scene3d
