#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A model's nodes posed by its animations and by a Skeleton3D, and the meshes its skins deform,
// as Godot's Skeleton3D, Skin and AnimationMixer do it:
//
//   std::vector<scene3d::NodePose> pose = scene3d::rest_pose(model);
//   scene3d::blend_animation(model, model, model.animations[0], 0.5, 1.0F, {}, pose);
//   const auto globals = scene3d::node_globals(model, pose);
//   scene3d::skin_vertices(surface, model.skins[0], globals, vertices);
//
// A pose is worked out from the components each time it is needed, never kept between ticks, so
// a world's snapshot, undo or dry run is never out of step with it.
namespace slash::scene3d {

// Where a node is in its parent, as a Godot bone's pose: the position, rotation and scale that
// make its transform, the rotation times the scale, then moved by the position.
struct NodePose {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};

    friend bool operator==(const NodePose&, const NodePose&) = default;
};

// An animation playing on a model: the model file it comes from, its name there, how far in it
// is and how much it counts. The file can be another than the one drawn, as a Godot animation
// library can be: its tracks move the nodes of the same names.
struct ClipSample {
    std::string model;
    std::string animation;
    double time = 0.0;
    float weight = 1.0F;
    // How much it counts for some nodes, by name, in place of weight, as an AnimationTree's
    // filters and root motion track leave them.
    std::map<std::string, float> node_weights{}; // NOLINT(readability-redundant-member-init)

    friend bool operator==(const ClipSample&, const ClipSample&) = default;
};

// Godot's Quaternion operations: a turned after b (a * b), the inverse of a unit quaternion,
// normalized, slerp and xform_inv, a vector turned back by a rotation.
[[nodiscard]] Quat multiply(const Quat& a, const Quat& b);
[[nodiscard]] Quat inverse(const Quat& q);
[[nodiscard]] Quat normalized(const Quat& q);
[[nodiscard]] Quat slerp(const Quat& from, const Quat& to, float weight);
[[nodiscard]] Vec3 xform_inv(const Quat& q, Vec3 v);

// Each node of a model at rest.
[[nodiscard]] std::vector<NodePose> rest_pose(const Model& model);

// The node of a model with a name, or -1.
[[nodiscard]] int find_node(const Model& model, std::string_view name);

// For each node of source, the node of target with its name, or -1: how an animation from
// source moves target's nodes.
[[nodiscard]] std::vector<int> node_map(const Model& target, const Model& source);

// A track's value at a time, from 0 to the animation's length: xyz, or a rotation's xyzw. Before
// its first key or after its last, a looping animation's track goes round from one to the other,
// and another holds its first or last value, as Godot's tracks with loop wrap do.
[[nodiscard]] std::array<float, 4> sample_track(const NodeTrack& track, double time, double length,
                                                bool loop);

// Adds an animation of source at a time to target's pose, as Godot's AnimationMixer blends a 3D
// track into a node: each position and scale moves weight times the way from its rest to the
// track's value, and each rotation turns weight of the way from its rest to the track's. map is
// node_map(target, source), or empty when source is target.
void blend_animation(const Model& target, const Model& source, const ModelAnimation& animation,
                     double time, float weight, std::span<const int> map,
                     std::vector<NodePose>& pose);
// The same with a weight for each of target's nodes; nodes whose weight is about 0 are left as
// they are, as Godot's mixer skips them.
void blend_animation(const Model& target, const Model& source, const ModelAnimation& animation,
                     double time, std::span<const float> weights, std::span<const int> map,
                     std::vector<NodePose>& pose);

// Puts the poses a Skeleton3D gives nodes by name into a pose; names the model hasn't are left
// out (check_skeleton names them).
void apply_skeleton(const Model& model, const world::Skeleton3D& skeleton,
                    std::vector<NodePose>& pose);

// A node's transform in its parent.
[[nodiscard]] world::Affine3D node_transform(const NodePose& pose);

// The position, rotation and scale of a transform, as Godot's Basis::get_rotation_quaternion
// and get_scale take them apart; a shear is lost.
[[nodiscard]] NodePose decompose(const world::Affine3D& transform);

// Each node's transform in the model's space, from a pose of all its nodes.
[[nodiscard]] std::vector<world::Affine3D> node_globals(const Model& model,
                                                        std::span<const NodePose> pose);

// Each joint's skinning transform: its node's global times its inverse bind matrix, which takes
// a vertex from the mesh's space at rest to the model's space as posed.
[[nodiscard]] std::vector<world::Affine3D>
joint_transforms(const ModelSkin& skin, std::span<const world::Affine3D> globals);

// A skinned surface's vertices moved by its skin's joints, as Godot's skeleton shader moves
// them: each vertex by its joints' transforms (joint_transforms) blended by weight, its normal and
// tangent turned the same way and normalized. A vertex with no weight stays where it is.
void skin_vertices(const ModelSurface& surface, std::span<const world::Affine3D> joints,
                   std::vector<render::Vertex3D>& out);

// Blends two poses of the same model weight of the way from a to b: positions and scales in a
// line, rotations by nlerp, as frames between ticks blend Transform3Ds.
[[nodiscard]] std::vector<NodePose> blend_poses(std::span<const NodePose> a,
                                                std::span<const NodePose> b, float weight);

// What an entity's model plays: its clips, which the animation module works out from the
// entity's AnimationTree or AnimationPlayer (animation::install sets it with set_clip_source).
// Without one set, models stay at rest.
using ClipSource =
    std::function<std::vector<ClipSample>(const world::World& world, entt::entity entity)>;
void set_clip_source(world::World& world, ClipSource source);
[[nodiscard]] std::vector<ClipSample> clips(const world::World& world, entt::entity entity);

// Loads a model by project path.
using ModelLoader = std::function<Result<std::shared_ptr<const Model>>(const std::string& path)>;

// A model posed as an entity's MeshInstance3D draws it: at rest, then moved by its clips
// (blend_animation, with a clip's node_weights for the nodes it names), then by its Skeleton3D if
// it has one (apply_skeleton), or at rest with show_rest_only. A clip whose model doesn't load,
// or that names an animation it hasn't, is left out.
[[nodiscard]] std::vector<NodePose> pose_of(const Model& model, std::span<const ClipSample> clips,
                                            const world::Skeleton3D* skeleton,
                                            const ModelLoader& load);

// The problems with a Skeleton3D posing model (the model of the entity's MeshInstance3D, or null
// for none): bone names the model hasn't, with "did you mean", each at its field's JSON Pointer
// in the component, and a Skeleton3D with no model to pose, at "".
struct SkeletonProblem {
    std::string path;
    std::string message;
};
[[nodiscard]] std::vector<SkeletonProblem> check_skeleton(const world::Skeleton3D& skeleton,
                                                          const Model* model);

} // namespace slash::scene3d
