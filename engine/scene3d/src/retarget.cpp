#include "slash/core/math.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/skin_tool.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Godot's PostImportPluginSkeletonTrackOrganizer, PostImportPluginSkeletonRenamer and
// PostImportPluginSkeletonRestFixer (editor/import/3d/), in the order its importer runs them,
// on a model: a Godot skeleton's bones are the nodes model_skeletons names, their rests the
// nodes' transforms and its animations' bone tracks the model's node tracks. Godot's importer
// also divides position tracks by the skeleton's motion scale, which a glTF file can't keep;
// blend_animation scales them by the ratio of two models' motion scales instead.
namespace slash::scene3d {
namespace {

using world::Affine3D;

constexpr float kCmpEpsilon = 0.00001F; // Godot's CMP_EPSILON

// ---- Godot's Vector3, Quaternion and Basis ----

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l == 0.0F ? Vec3{} : v * (1.0F / l);
}

Vec3 times(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 any_perpendicular(Vec3 v) {
    const bool x_least = std::abs(v.x) <= std::abs(v.y) && std::abs(v.x) <= std::abs(v.z);
    return normalized(cross(v, x_least ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F}));
}

// Vector3::angle_to.
float angle_to(Vec3 a, Vec3 b) {
    return slash::atan2(length(cross(a, b)), dot(a, b));
}

// Quaternion(v0, v1): the shortest arc from one direction to another.
Quat arc(Vec3 v0, Vec3 v1) {
    constexpr float kAlmostOne = 0.99999975F;
    const Vec3 n0 = normalized(v0);
    const Vec3 n1 = normalized(v1);
    const float d = dot(n0, n1);
    if (std::abs(d) > kAlmostOne) {
        if (d >= 0.0F) {
            return {};
        }
        const Vec3 axis = any_perpendicular(n0);
        return {axis.x, axis.y, axis.z, 0.0F};
    }
    const Vec3 c = cross(n0, n1);
    const float s = std::sqrt((1.0F + d) * 2.0F);
    const float rs = 1.0F / s;
    return {c.x * rs, c.y * rs, c.z * rs, s * 0.5F};
}

Quat operator*(const Quat& a, const Quat& b) {
    return multiply(a, b);
}

// A transform's basis alone, as Transform3D::basis.
Affine3D basis_of(Affine3D transform) {
    transform.origin = {};
    return transform;
}

Affine3D with_origin(Affine3D basis, Vec3 origin) {
    basis.origin = origin;
    return basis;
}

// Basis().scaled(scale).
Affine3D diagonal(Vec3 scale) {
    Affine3D out;
    out.x = {scale.x, 0.0F, 0.0F};
    out.y = {0.0F, scale.y, 0.0F};
    out.z = {0.0F, 0.0F, scale.z};
    return out;
}

float determinant(const Affine3D& m) {
    return dot(m.x, cross(m.y, m.z));
}

// Basis::get_rotation_quaternion.
Quat rotation_of(const Affine3D& transform) {
    return decompose(basis_of(transform)).rotation;
}

// Basis::get_scale.
Vec3 scale_of(const Affine3D& transform) {
    return decompose(basis_of(transform)).scale;
}

// Basis::get_scale_global: the lengths of the rows rather than the columns.
Vec3 scale_global(const Affine3D& m) {
    const float sign = determinant(m) < 0.0F ? -1.0F : 1.0F;
    return Vec3{length({m.x.x, m.y.x, m.z.x}), length({m.x.y, m.y.y, m.z.y}),
                length({m.x.z, m.y.z, m.z.z})} *
           sign;
}

// Basis::xform_inv, which multiplies by the transpose.
Vec3 basis_xform_inv(const Affine3D& m, Vec3 v) {
    return {dot(m.x, v), dot(m.y, v), dot(m.z, v)};
}

// Basis(Quaternion).
Affine3D basis_of(const Quat& q) {
    return world::rotation_basis(q);
}

// A profile bone's reference pose, its basis given row by row as Godot's Transform3D takes it.
Affine3D reference_pose(const ProfileBone& bone) {
    const std::array<float, 9>& b = bone.basis;
    Affine3D out;
    out.x = {b[0], b[3], b[6]};
    out.y = {b[1], b[4], b[7]};
    out.z = {b[2], b[5], b[8]};
    out.origin = bone.origin;
    return out;
}

// ---- The skeleton ----

struct Skeleton {
    std::vector<std::size_t> nodes; // a bone's node, parents first
    std::vector<int> parent;        // a bone's parent bone, or -1
    std::vector<int> bone_of;       // a node's bone, or -1

    Skeleton(const Model& model, const std::vector<std::size_t>& bones)
        : nodes(bones), parent(bones.size(), -1), bone_of(model.nodes.size(), -1) {
        for (std::size_t b = 0; b < nodes.size(); ++b) {
            bone_of[nodes[b]] = static_cast<int>(b);
        }
        for (std::size_t b = 0; b < nodes.size(); ++b) {
            const int p = model.nodes[nodes[b]].parent;
            parent[b] = p >= 0 ? bone_of[static_cast<std::size_t>(p)] : -1;
        }
    }

    [[nodiscard]] int find(const Model& model, std::string_view name) const {
        for (std::size_t b = 0; b < nodes.size(); ++b) {
            if (model.nodes[nodes[b]].name == name) {
                return static_cast<int>(b);
            }
        }
        return -1;
    }

    // Skeleton3D::get_bone_global_rest of every bone, from the rests of the model's nodes.
    [[nodiscard]] std::vector<Affine3D> globals(const std::vector<Affine3D>& rest) const {
        std::vector<Affine3D> out(nodes.size());
        for (std::size_t b = 0; b < nodes.size(); ++b) {
            out[b] = parent[b] >= 0 ? out[static_cast<std::size_t>(parent[b])] * rest[nodes[b]]
                                    : rest[nodes[b]];
        }
        return out;
    }

    [[nodiscard]] bool descends_from(std::size_t bone, std::size_t ancestor) const {
        for (int p = parent[bone]; p >= 0; p = parent[static_cast<std::size_t>(p)]) {
            if (static_cast<std::size_t>(p) == ancestor) {
                return true;
            }
        }
        return false;
    }
};

// The profile's bones' global rests, as Godot's importer builds a skeleton of them.
std::vector<Affine3D> profile_globals() {
    const std::span<const ProfileBone> profile = humanoid_profile();
    std::vector<Affine3D> out(profile.size());
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const int parent = find_profile_bone(profile[i].parent);
        out[i] = parent >= 0 ? out[static_cast<std::size_t>(parent)] * reference_pose(profile[i])
                             : reference_pose(profile[i]);
    }
    return out;
}

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

// Visits each track of the model's animations that moves a bone.
template <typename Visit>
void for_bone_tracks(Model& model, const Skeleton& skeleton, const Visit& visit) {
    for (ModelAnimation& animation : model.animations) {
        for (NodeTrack& track : animation.tracks) {
            if (track.node < skeleton.bone_of.size() && skeleton.bone_of[track.node] >= 0) {
                visit(track, static_cast<std::size_t>(skeleton.bone_of[track.node]));
            }
        }
    }
}

Vec3 vec_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2]};
}

std::array<float, 4> array_of(Vec3 v) {
    return {v.x, v.y, v.z, 0.0F};
}

Quat quat_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2], v[3]};
}

std::array<float, 4> array_of(const Quat& q) {
    return {q.x, q.y, q.z, q.w};
}

} // namespace

Result<RetargetReport> retarget_model(Model& model, const RetargetOptions& options) {
    const std::vector<ModelSkeleton> skeletons = model_skeletons(model);
    const ModelSkeleton* largest = nullptr;
    for (const ModelSkeleton& candidate : skeletons) {
        if (largest == nullptr || candidate.bones.size() > largest->bones.size()) {
            largest = &candidate;
        }
    }
    if (largest == nullptr) {
        return invalid("the model has no skin to retarget");
    }
    const Skeleton skeleton(model, largest->bones);
    const std::span<const ProfileBone> profile = humanoid_profile();

    // The map, as profile bone names by bone.
    std::vector<std::string> profile_of(skeleton.nodes.size());
    for (const auto& [profile_bone, bone_name] : options.bone_map) {
        if (find_profile_bone(profile_bone) < 0) {
            return invalid("the humanoid profile has no bone '" + profile_bone + "'");
        }
        if (bone_name.empty()) {
            continue;
        }
        const int bone = skeleton.find(model, bone_name);
        if (bone < 0) {
            return invalid("the model's skeleton has no bone '" + bone_name + "'");
        }
        std::string& mapped = profile_of[static_cast<std::size_t>(bone)];
        if (!mapped.empty()) {
            std::string message = "bone '" + bone_name;
            message += "' is mapped twice, as '";
            message += mapped;
            message += "' and '";
            message += profile_bone;
            message += "'";
            return invalid(std::move(message));
        }
        mapped = profile_bone;
    }

    RetargetReport report;
    for (const ProfileBone& bone : profile) {
        const auto it = options.bone_map.find(bone.name);
        if (bone.required && (it == options.bone_map.end() || it->second.empty())) {
            report.missing.emplace_back(bone.name);
        }
    }
    const std::vector<Affine3D> old_model_globals = node_globals(model, rest_pose(model));

    // PostImportPluginSkeletonTrackOrganizer, unimportant_positions: a humanoid's proportions
    // are in its rests, so only Root's and Hips' positions move it.
    if (options.remove_unimportant_positions) {
        for (ModelAnimation& animation : model.animations) {
            std::erase_if(animation.tracks, [&](const NodeTrack& track) {
                if (track.property != NodeProperty::Position ||
                    track.node >= skeleton.bone_of.size() || skeleton.bone_of[track.node] < 0) {
                    return false;
                }
                const std::string& mapped =
                    profile_of[static_cast<std::size_t>(skeleton.bone_of[track.node])];
                return !mapped.empty() && mapped != kProfileRoot && mapped != kScaleBaseBone;
            });
        }
    }

    // PostImportPluginSkeletonRenamer: first a node that has the name a bone is to take moves
    // aside ("Spine" becomes "Spine2"), then the mapped bones take the profile's names. Node
    // names are unique in a model, so any node moves aside, not only bones.
    std::vector<std::string> original_names(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        original_names[i] = model.nodes[i].name;
    }
    {
        std::vector<std::string> solved_names;
        std::map<std::string, std::string, std::less<>> pre_rename;
        for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
            const std::string& target = profile_of[b];
            if (target.empty() || model.nodes[skeleton.nodes[b]].name == target ||
                find_node(model, target) < 0) {
                continue;
            }
            std::string solved = target;
            for (int j = 2; find_node(model, solved) >= 0 || find_profile_bone(solved) >= 0 ||
                            std::ranges::find(solved_names, solved) != solved_names.end();
                 ++j) {
                solved = target + std::to_string(j);
            }
            solved_names.push_back(solved);
            pre_rename.emplace(target, solved);
        }
        for (ModelNode& node : model.nodes) {
            if (const auto it = pre_rename.find(node.name); it != pre_rename.end()) {
                node.name = it->second;
            }
        }
        for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
            if (!profile_of[b].empty()) {
                model.nodes[skeleton.nodes[b]].name = profile_of[b];
            }
        }
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const std::string& name = model.nodes[i].name;
            if (name != original_names[i] &&
                (skeleton.bone_of[i] < 0 ||
                 profile_of[static_cast<std::size_t>(skeleton.bone_of[i])].empty())) {
                report.moved_aside.emplace(name, original_names[i]);
            }
        }
        for (ModelPart& part : model.parts) {
            if (part.node_index >= 0 &&
                part.node == original_names[static_cast<std::size_t>(part.node_index)]) {
                part.node = model.nodes[static_cast<std::size_t>(part.node_index)].name;
            }
        }
        for (ModelCollision& shape : model.collision) {
            if (shape.node_index >= 0 &&
                shape.node == original_names[static_cast<std::size_t>(shape.node_index)]) {
                shape.node = model.nodes[static_cast<std::size_t>(shape.node_index)].name;
            }
        }
    }

    // PostImportPluginSkeletonRestFixer.
    std::vector<Affine3D> rest(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode& node = model.nodes[i];
        rest[i] = node_transform({node.position, node.rotation, node.scale});
    }
    std::vector<bool> changed(model.nodes.size(), false);

    // apply_node_transforms: the nodes above the skeleton fold into its roots, all but their
    // move, and their scale into every bone's position.
    Affine3D global_transform;
    {
        bool first = true;
        for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
            if (skeleton.parent[b] >= 0) {
                continue;
            }
            for (int p = model.nodes[skeleton.nodes[b]].parent; p >= 0;
                 p = model.nodes[static_cast<std::size_t>(p)].parent) {
                const auto node = static_cast<std::size_t>(p);
                if (first) {
                    global_transform = rest[node] * global_transform;
                }
                rest[node] = Affine3D{};
                changed[node] = true;
            }
            first = false;
        }
    }
    global_transform.origin = {};
    const Vec3 scl = scale_global(global_transform);
    const Affine3D ortho = world::orthonormalized(global_transform);
    for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
        const std::size_t node = skeleton.nodes[b];
        if (skeleton.parent[b] < 0) {
            rest[node] = ortho * rest[node];
        }
        rest[node].origin = times(rest[node].origin, scl);
        changed[node] = true;
    }
    const Quat global_rotation = rotation_of(global_transform);
    for_bone_tracks(model, skeleton, [&](NodeTrack& track, std::size_t bone) {
        const bool root = skeleton.parent[bone] < 0;
        for (std::array<float, 4>& value : track.values) {
            switch (track.property) {
            case NodeProperty::Position:
                value = array_of(root ? global_transform.basis(vec_of(value))
                                      : times(vec_of(value), scl));
                break;
            case NodeProperty::Rotation:
                if (root) {
                    value = array_of(global_rotation * quat_of(value));
                }
                break;
            case NodeProperty::Scale:
                if (root) {
                    value = array_of(scale_of(ortho * diagonal(vec_of(value))));
                }
                break;
            }
        }
    });

    // Complements rotation tracks, so an animation that leaves a mapped bone at rest keeps it at
    // this model's rest when it plays on another.
    for (ModelAnimation& animation : model.animations) {
        const bool found_skeleton = std::ranges::any_of(animation.tracks, [&](const NodeTrack& t) {
            return t.node < skeleton.bone_of.size() && skeleton.bone_of[t.node] >= 0;
        });
        if (!found_skeleton) {
            continue;
        }
        for (const ProfileBone& bone : profile) {
            const int src = skeleton.find(model, bone.name);
            if (src < 0) {
                continue;
            }
            const std::size_t node = skeleton.nodes[static_cast<std::size_t>(src)];
            const bool has_rotation =
                std::ranges::any_of(animation.tracks, [&](const NodeTrack& t) {
                    return t.node == node && t.property == NodeProperty::Rotation;
                });
            if (!has_rotation) {
                NodeTrack track;
                track.node = node;
                track.property = NodeProperty::Rotation;
                track.times.push_back(0.0F);
                track.values.push_back(array_of(rotation_of(rest[node])));
                animation.tracks.push_back(std::move(track));
            }
        }
    }

    // fix_silhouette: turns bones toward the profile's directions.
    const std::vector<Affine3D> pre_silhouette = skeleton.globals(rest);
    std::vector<Affine3D> silhouette_diff(skeleton.nodes.size());
    const std::vector<Affine3D> prof_globals = profile_globals();
    if (options.fix_silhouette) {
        for (std::size_t prof = 0; prof < profile.size(); ++prof) {
            const ProfileBone& bone = profile[prof];
            if (std::ranges::find(options.silhouette_filter, bone.name) !=
                options.silhouette_filter.end()) {
                continue;
            }
            const int src = skeleton.find(model, bone.name);
            if (src < 0 || bone.tail_direction == TailDirection::End) {
                continue;
            }
            const std::vector<Affine3D> globals = skeleton.globals(rest);
            Vec3 prof_tail;
            Vec3 src_tail;
            if (bone.tail_direction == TailDirection::AverageChildren) {
                int children = 0;
                bool all_children = true;
                for (std::size_t child = 0; child < profile.size(); ++child) {
                    if (profile[child].parent != bone.name) {
                        continue;
                    }
                    const int src_child = skeleton.find(model, profile[child].name);
                    if (src_child < 0) {
                        all_children = false;
                        break;
                    }
                    prof_tail = prof_tail + prof_globals[child].origin;
                    src_tail = src_tail + globals[static_cast<std::size_t>(src_child)].origin;
                    ++children;
                }
                if (children == 0 || !all_children) {
                    continue;
                }
                prof_tail = prof_tail * (1.0F / static_cast<float>(children));
                src_tail = src_tail * (1.0F / static_cast<float>(children));
            }
            if (bone.tail_direction == TailDirection::SpecificChild) {
                const int prof_tail_index = find_profile_bone(bone.tail);
                if (prof_tail_index < 0) {
                    continue;
                }
                const int src_tail_index = skeleton.find(model, bone.tail);
                if (src_tail_index < 0) {
                    continue;
                }
                prof_tail = prof_globals[static_cast<std::size_t>(prof_tail_index)].origin;
                src_tail = globals[static_cast<std::size_t>(src_tail_index)].origin;
            }
            const Vec3 prof_head = prof_globals[prof].origin;
            const Vec3 src_head = globals[static_cast<std::size_t>(src)].origin;
            Vec3 prof_dir = prof_tail - prof_head;
            Vec3 src_dir = src_tail - src_head;
            if (std::abs(dot(prof_dir, prof_dir)) < kCmpEpsilon ||
                std::abs(dot(src_dir, src_dir)) < kCmpEpsilon) {
                continue;
            }
            prof_dir = normalized(prof_dir);
            src_dir = normalized(src_dir);
            constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;
            if (std::abs(angle_to(src_dir, prof_dir) * kDegrees) > options.silhouette_threshold) {
                const Affine3D diff = basis_of(arc(src_dir, prof_dir));
                const int src_parent = skeleton.parent[static_cast<std::size_t>(src)];
                const Affine3D src_pg =
                    src_parent >= 0 ? basis_of(globals[static_cast<std::size_t>(src_parent)])
                                    : Affine3D{};
                const std::size_t node = skeleton.nodes[static_cast<std::size_t>(src)];
                rest[node] = with_origin(src_pg.inverse() * diff * src_pg * basis_of(rest[node]),
                                         rest[node].origin);
            }
        }

        // base_height_adjustment moves the hips up, and their position tracks with them.
        if (std::abs(options.base_height_adjustment) >= kCmpEpsilon) {
            const int src = skeleton.find(model, kScaleBaseBone);
            if (src >= 0) {
                Vec3 up{0.0F, options.base_height_adjustment, 0.0F};
                const int src_parent = skeleton.parent[static_cast<std::size_t>(src)];
                if (src_parent >= 0) {
                    const std::vector<Affine3D> globals = skeleton.globals(rest);
                    up = xform_inv(rotation_of(globals[static_cast<std::size_t>(src_parent)]), up);
                }
                const std::size_t node = skeleton.nodes[static_cast<std::size_t>(src)];
                rest[node].origin = rest[node].origin + up;
                for (ModelAnimation& animation : model.animations) {
                    for (NodeTrack& track : animation.tracks) {
                        if (track.node == node && track.property == NodeProperty::Position) {
                            for (std::array<float, 4>& value : track.values) {
                                value = array_of(vec_of(value) + up);
                            }
                        }
                    }
                }
            }
        }

        const std::vector<Affine3D> globals = skeleton.globals(rest);
        for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
            silhouette_diff[b] = pre_silhouette[b] * globals[b].inverse();
        }
    }

    // Overwrite Axis: each mapped bone's rest turns to the profile's in the world, unmapped bones
    // with no mapped bone under them keep their rest in the world (keep_global_rest_on_leftovers),
    // and the rest lie as their parents' did.
    const std::vector<Affine3D> old_rest = rest;
    const std::vector<Affine3D> old_globals = skeleton.globals(rest);
    std::vector<bool> keep_rest(skeleton.nodes.size(), false);
    if (options.keep_global_rest_on_leftovers) {
        for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
            if (find_profile_bone(model.nodes[skeleton.nodes[b]].name) >= 0) {
                continue;
            }
            bool found_mapped = false;
            for (std::size_t d = 0; d < skeleton.nodes.size() && !found_mapped; ++d) {
                found_mapped = skeleton.descends_from(d, b) &&
                               find_profile_bone(model.nodes[skeleton.nodes[d]].name) >= 0;
            }
            keep_rest[b] = !found_mapped;
        }
    }
    std::vector<Affine3D> diffs(skeleton.nodes.size());
    std::vector<Affine3D> new_globals(skeleton.nodes.size());
    for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
        const std::size_t node = skeleton.nodes[b];
        const int parent = skeleton.parent[b];
        Affine3D tgt_rot;
        const Affine3D src_pg =
            parent >= 0 ? basis_of(new_globals[static_cast<std::size_t>(parent)]) : Affine3D{};
        const int prof = find_profile_bone(model.nodes[node].name);
        if (prof >= 0) {
            tgt_rot = src_pg.inverse() * basis_of(prof_globals[static_cast<std::size_t>(prof)]);
        } else if (keep_rest[b]) {
            tgt_rot = src_pg.inverse() * basis_of(old_globals[b]);
        }
        const Affine3D parent_diff =
            parent >= 0 ? diffs[static_cast<std::size_t>(parent)] : Affine3D{};
        diffs[b] = tgt_rot.inverse() * parent_diff * basis_of(rest[node]);
        rest[node] = with_origin(tgt_rot, parent_diff.basis(rest[node].origin));
        new_globals[b] =
            parent >= 0 ? new_globals[static_cast<std::size_t>(parent)] * rest[node] : rest[node];
    }
    for_bone_tracks(model, skeleton, [&](NodeTrack& track, std::size_t bone) {
        const std::size_t node = skeleton.nodes[bone];
        const int parent = skeleton.parent[bone];
        const Affine3D old_pg =
            parent >= 0 ? basis_of(old_globals[static_cast<std::size_t>(parent)]) : Affine3D{};
        const Affine3D new_pg =
            parent >= 0 ? basis_of(new_globals[static_cast<std::size_t>(parent)]) : Affine3D{};
        switch (track.property) {
        case NodeProperty::Rotation: {
            const Quat old_rest_q = rotation_of(old_rest[node]);
            const Quat new_rest_q = rotation_of(rest[node]);
            const Quat old_pg_q = rotation_of(old_pg);
            const Quat new_pg_q = rotation_of(new_pg);
            for (std::array<float, 4>& value : track.values) {
                value = array_of(inverse(new_pg_q) * old_pg_q * quat_of(value) *
                                 inverse(old_rest_q) * inverse(old_pg_q) * new_pg_q * new_rest_q);
            }
            break;
        }
        case NodeProperty::Scale: {
            const Affine3D old_rest_inv = basis_of(old_rest[node]).inverse();
            const Affine3D new_rest_b = basis_of(rest[node]);
            for (std::array<float, 4>& value : track.values) {
                value = array_of(scale_of(new_pg.inverse() * old_pg * diagonal(vec_of(value)) *
                                          old_rest_inv * old_pg.inverse() * new_pg * new_rest_b));
            }
            break;
        }
        case NodeProperty::Position:
            for (std::array<float, 4>& value : track.values) {
                value = array_of(
                    basis_xform_inv(new_pg, old_pg.basis(vec_of(value) - old_rest[node].origin)) +
                    rest[node].origin);
            }
            break;
        }
    });

    // Fix skin: each joint's inverse bind matrix and each node a bone carries turn with the
    // bone, so the model draws as it did at rest.
    const Affine3D global_scale = diagonal(scl);
    std::vector<Affine3D> adjust(skeleton.nodes.size());
    for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
        adjust[b] = global_scale *
                    (new_globals[b].inverse() * silhouette_diff[b].inverse() * pre_silhouette[b]);
    }
    for (ModelSkin& skin : model.skins) {
        for (std::size_t i = 0; i < skin.joints.size() && i < skin.inverse_binds.size(); ++i) {
            const std::size_t joint = skin.joints[i];
            if (joint < skeleton.bone_of.size() && skeleton.bone_of[joint] >= 0) {
                skin.inverse_binds[i] = adjust[static_cast<std::size_t>(skeleton.bone_of[joint])] *
                                        skin.inverse_binds[i];
            }
        }
    }
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const int parent = model.nodes[i].parent;
        if (skeleton.bone_of[i] < 0 && parent >= 0 &&
            skeleton.bone_of[static_cast<std::size_t>(parent)] >= 0) {
            rest[i] = adjust[static_cast<std::size_t>(
                          skeleton.bone_of[static_cast<std::size_t>(parent)])] *
                      rest[i];
            changed[i] = true;
        }
    }

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (changed[i]) {
            const NodePose pose = decompose(rest[i]);
            model.nodes[i].position = pose.position;
            model.nodes[i].rotation = pose.rotation;
            model.nodes[i].scale = pose.scale;
        }
    }

    // Parts and collision shapes placed by a node move as it did.
    const std::vector<Affine3D> new_model_globals = node_globals(model, rest_pose(model));
    const auto moved = [&](int node) {
        const auto n = static_cast<std::size_t>(node);
        return new_model_globals[n] * old_model_globals[n].inverse();
    };
    for (ModelPart& part : model.parts) {
        if (part.node_index >= 0 &&
            static_cast<std::size_t>(part.node_index) < model.nodes.size()) {
            part.transform = moved(part.node_index) * part.transform;
        }
    }
    for (ModelCollision& shape : model.collision) {
        if (shape.node_index >= 0 &&
            static_cast<std::size_t>(shape.node_index) < model.nodes.size()) {
            const Affine3D delta = moved(shape.node_index);
            for (Vec3& point : shape.points) {
                point = delta.apply(point);
            }
        }
    }

    for (std::size_t b = 0; b < skeleton.nodes.size(); ++b) {
        if (profile_of[b].empty()) {
            report.unmapped.push_back(model.nodes[skeleton.nodes[b]].name);
        }
    }
    report.motion_scale = motion_scale(model);
    return report;
}

float motion_scale(const Model& model) {
    if (model.skins.empty()) {
        return 0.0F;
    }
    const int hips = find_node(model, kScaleBaseBone);
    if (hips < 0) {
        return 0.0F;
    }
    Affine3D global;
    for (int n = hips; n >= 0; n = model.nodes[static_cast<std::size_t>(n)].parent) {
        const ModelNode& node = model.nodes[static_cast<std::size_t>(n)];
        global = node_transform({node.position, node.rotation, node.scale}) * global;
    }
    return std::abs(global.origin.y);
}

} // namespace slash::scene3d
