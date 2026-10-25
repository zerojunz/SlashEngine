#include "slash/scene3d/ik.hpp"

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/bone_attachment.hpp"
#include "slash/scene3d/physical_bones.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

// Godot 4.6's skeleton modifiers (scene/3d/skeleton_modifier_3d.cpp, ik_modifier_3d.cpp,
// two_bone_ik_3d.cpp, chain_ik_3d.cpp, iterate_ik_3d.cpp, fabr_ik_3d.cpp, ccd_ik_3d.cpp,
// jacobian_ik_3d.cpp and scene/resources/3d/joint_limitation_cone_3d.cpp), line for line where
// it can. Godot keeps some values in float and some in double; so does this. Angles go through
// slash::atan2 and slash::cos_sin, not libm, so poses are the same on every platform.
namespace slash::scene3d {
namespace {

constexpr float kCmpEpsilon = 0.00001F; // Godot's CMP_EPSILON
constexpr double kPi = std::numbers::pi;

// ---- Godot's Vector3 ----

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length_squared(Vec3 v) {
    return dot(v, v);
}

float length(Vec3 v) {
    return std::sqrt(length_squared(v));
}

Vec3 normalized(Vec3 v) {
    const float lengthsq = length_squared(v);
    if (lengthsq == 0.0F) {
        return {};
    }
    const float l = std::sqrt(lengthsq);
    return {v.x / l, v.y / l, v.z / l};
}
using scene3d::normalized;

bool is_zero_approx(float v) {
    return std::abs(v) < kCmpEpsilon;
}

bool is_zero_approx(double v) {
    return std::abs(v) < static_cast<double>(kCmpEpsilon);
}

bool is_zero_approx(Vec3 v) {
    return is_zero_approx(v.x) && is_zero_approx(v.y) && is_zero_approx(v.z);
}

bool is_equal_approx(double a, double b) {
    if (a == b) {
        return true;
    }
    const double tolerance =
        std::max(static_cast<double>(kCmpEpsilon) * std::abs(a), static_cast<double>(kCmpEpsilon));
    return std::abs(a - b) < tolerance;
}

float distance_squared(Vec3 a, Vec3 b) {
    return length_squared(b - a);
}

Vec3 slide(Vec3 v, Vec3 normal) {
    return v - (normal * dot(v, normal));
}

float angle_to(Vec3 a, Vec3 b) {
    return slash::atan2(length(cross(a, b)), dot(a, b));
}

Vec3 any_perpendicular(Vec3 v) {
    const bool x_least = std::abs(v.x) <= std::abs(v.y) && std::abs(v.x) <= std::abs(v.z);
    return normalized(cross(v, x_least ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F}));
}

// Godot's Math::acos, which clamps.
float acos_of(float x) {
    const float c = std::clamp(x, -1.0F, 1.0F);
    return slash::atan2(std::sqrt((1.0F - c) * (1.0F + c)), c);
}

// ---- Godot's Quaternion ----

float length_squared(const Quat& q) {
    return (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w);
}

bool is_normalized(const Quat& q) {
    return std::abs(length_squared(q) - 1.0F) < 0.001F; // Godot's UNIT_EPSILON
}

Quat operator*(const Quat& a, const Quat& b) {
    return multiply(a, b);
}

Vec3 xform(const Quat& q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 uv = cross(u, v);
    return v + (((uv * q.w) + cross(u, uv)) * 2.0F);
}

// Quaternion(axis, angle).
Quat axis_angle(Vec3 axis, float angle) {
    const float d = length(axis);
    if (d == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    const Vec2 cs = cos_sin(angle * 0.5F);
    const float s = cs.y / d;
    return {axis.x * s, axis.y * s, axis.z * s, cs.x};
}

// Quaternion(v0, v1), the shortest arc from one direction to another.
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

float angle_to(const Quat& a, const Quat& b) {
    const float d = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    return acos_of((d * d * 2.0F) - 1.0F);
}

// Basis(x, y, z).get_rotation_quaternion(), for orthonormal columns.
Quat basis_rotation(Vec3 x, Vec3 y, Vec3 z) {
    return decompose(world::Affine3D{.x = x, .y = y, .z = z, .origin = {}}).rotation;
}

// Basis::xform_inv: the basis's transpose times v.
Vec3 basis_xform_inv(const world::Affine3D& basis, Vec3 v) {
    return {dot(basis.x, v), dot(basis.y, v), dot(basis.z, v)};
}

// ---- SkeletonModifier3D's helpers ----

Vec3 limit_length(Vec3 origin, Vec3 destination, float length) {
    return origin + (normalized(destination - origin) * length);
}

Quat from_to_rotation_by_axis(Vec3 from, Vec3 to, Vec3 axis) {
    const double almost_one = 1.0 - static_cast<double>(kCmpEpsilon);
    const double d = dot(from, to);
    if (d > almost_one) {
        return {};
    }
    if (d < -almost_one) {
        return axis_angle(axis, static_cast<float>(kPi));
    }
    float angle = angle_to(from, to);
    if (std::signbit(dot(cross(from, to), axis))) {
        angle = -angle;
    }
    return axis_angle(axis, angle);
}

Quat swing(const Quat& rotation, Vec3 axis) {
    if (is_zero_approx(axis)) {
        return rotation;
    }
    Quat rot = rotation;
    if (!is_normalized(rot)) {
        rot = normalized(rot);
    }
    const Vec3 a = normalized(axis);
    const Vec3 v{rot.x, rot.y, rot.z};
    const Vec3 twist_vec = a * dot(v, a);
    Quat twist{twist_vec.x, twist_vec.y, twist_vec.z, rot.w};
    if (!is_normalized(twist)) {
        if (is_zero_approx(length_squared(twist))) {
            return rot;
        }
        twist = normalized(twist);
    }
    return normalized(rot * inverse(twist));
}

Vec3 snap_vector_to_plane(Vec3 plane_normal, Vec3 v) {
    if (is_zero_approx(length_squared(plane_normal))) {
        return v;
    }
    const auto l = static_cast<double>(length(v));
    return slide(normalized(v), normalized(plane_normal)) * static_cast<float>(l);
}

Vec3 projected_normal(Vec3 a, Vec3 b, Vec3 point) {
    const Vec3 dir = b - a;
    const float denom = length_squared(dir);
    if (is_zero_approx(denom)) {
        return {};
    }
    const Vec3 w = point - a;
    const float t = dot(w, dir) / denom;
    const Vec3 h = a + (dir * t);
    return normalized(point - h);
}

// A BoneDirection or SecondaryDirection's vector: zero for "none" and "from_parent".
Vec3 direction_vector(std::string_view name, Vec3 custom) {
    if (name == "plus_x") {
        return {1.0F, 0.0F, 0.0F};
    }
    if (name == "minus_x") {
        return {-1.0F, 0.0F, 0.0F};
    }
    if (name == "plus_y") {
        return {0.0F, 1.0F, 0.0F};
    }
    if (name == "minus_y") {
        return {0.0F, -1.0F, 0.0F};
    }
    if (name == "plus_z") {
        return {0.0F, 0.0F, 1.0F};
    }
    if (name == "minus_z") {
        return {0.0F, 0.0F, -1.0F};
    }
    if (name == "custom") {
        return custom;
    }
    return {};
}

// A RotationAxis's vector: zero for "all".
Vec3 rotation_axis_vector(const world::ChainIKJoint& joint) {
    if (joint.rotation_axis == "x") {
        return {1.0F, 0.0F, 0.0F};
    }
    if (joint.rotation_axis == "y") {
        return {0.0F, 1.0F, 0.0F};
    }
    if (joint.rotation_axis == "z") {
        return {0.0F, 0.0F, 1.0F};
    }
    if (joint.rotation_axis == "custom") {
        return joint.rotation_axis_vector;
    }
    return {};
}

constexpr std::array<std::string_view, 7> kBoneDirections{
    "from_parent", "plus_x", "minus_x", "plus_y", "minus_y", "plus_z", "minus_z"};
constexpr std::array<std::string_view, 8> kSecondaryDirections{
    "none", "plus_x", "minus_x", "plus_y", "minus_y", "plus_z", "minus_z", "custom"};
constexpr std::array<std::string_view, 5> kRotationAxes{"all", "x", "y", "z", "custom"};
constexpr std::array<std::string_view, 2> kLimitations{"", "cone"};

// ---- A model's bones, as a Skeleton3D sees them ----

// The bones of a model posed as pose, with their poses, rests and global poses in the model's
// space, as Skeleton3D's get_bone_pose, get_bone_rest and get_bone_global_pose. Global poses are
// worked out again after a bone is turned.
class Bones {
public:
    Bones(const Model& model, std::vector<NodePose>& pose) : model_(&model), pose_(&pose) {}

    [[nodiscard]] int parent(int bone) const {
        return model_->nodes[static_cast<std::size_t>(bone)].parent;
    }
    [[nodiscard]] const NodePose& pose(int bone) const {
        return (*pose_)[static_cast<std::size_t>(bone)];
    }
    [[nodiscard]] Quat pose_rotation(int bone) const { return normalized(pose(bone).rotation); }
    [[nodiscard]] const ModelNode& rest(int bone) const {
        return model_->nodes[static_cast<std::size_t>(bone)];
    }
    [[nodiscard]] world::Affine3D rest_basis(int bone) const {
        const ModelNode& node = rest(bone);
        return node_transform({.position = {}, .rotation = node.rotation, .scale = node.scale});
    }
    [[nodiscard]] const world::Affine3D& global_pose(int bone) {
        if (dirty_) {
            globals_ = node_globals(*model_, *pose_);
            dirty_ = false;
        }
        return globals_[static_cast<std::size_t>(bone)];
    }
    [[nodiscard]] Quat global_pose_rotation(int bone) {
        return decompose(global_pose(bone)).rotation;
    }
    // The rotation of the global pose of a bone's parent; none for a root.
    [[nodiscard]] Quat parent_global_rotation(int bone) {
        const int p = parent(bone);
        return p >= 0 ? global_pose_rotation(p) : Quat{};
    }
    [[nodiscard]] const world::Affine3D& global_rest(int bone) {
        if (rests_.empty()) {
            rests_ = node_globals(*model_, rest_pose(*model_));
        }
        return rests_[static_cast<std::size_t>(bone)];
    }
    void set_rotation(int bone, const Quat& rotation) {
        (*pose_)[static_cast<std::size_t>(bone)].rotation = rotation;
        dirty_ = true;
    }
    // Skeleton3D::set_bone_global_pose: the pose that puts a bone there under its parent.
    void set_global_pose(int bone, const world::Affine3D& global) {
        const int p = parent(bone);
        const world::Affine3D local = p >= 0 ? global_pose(p).inverse() * global : global;
        (*pose_)[static_cast<std::size_t>(bone)] = decompose(local);
        dirty_ = true;
    }

private:
    const Model* model_;
    std::vector<NodePose>* pose_;
    std::vector<world::Affine3D> globals_;
    std::vector<world::Affine3D> rests_;
    bool dirty_ = true;
};

// IKModifier3D::get_bone_axis: the way a bone points, in its rest's space.
Vec3 bone_axis(Bones& bones, int end_bone, std::string_view direction, bool mutable_bone_axes) {
    if (direction == "from_parent" || direction.empty()) {
        const Vec3 origin =
            mutable_bone_axes ? bones.pose(end_bone).position : bones.rest(end_bone).position;
        return normalized(basis_xform_inv(bones.rest_basis(end_bone), origin));
    }
    return direction_vector(direction, {});
}

// IKModifier3D::get_local_pose_rotation.
Quat local_pose_rotation(Bones& bones, int bone, const Quat& global_rotation) {
    const int p = bones.parent(bone);
    if (p < 0) {
        return global_rotation;
    }
    return inverse(bones.global_pose_rotation(p)) * global_rotation;
}

// IKModifier3DSolverInfo.
struct SolverInfo {
    Quat current_lpose;
    Quat current_lrest;
    Quat current_gpose;
    Quat current_grest;
    Vec3 current_vector; // in the model's space
    Vec3 forward_vector; // in the bone's
    float length = 0.0F;
    bool made = false; // false where Godot's is null, for a bone with no length
};

// ---- TwoBoneIK3D ----

struct TwoBone {
    const world::TwoBoneIKSetting* setting = nullptr;
    int root = -1;
    int middle = -1;
    int end = -1;
    SolverInfo root_info;
    SolverInfo mid_info;
    Vec3 root_pos;
    Vec3 mid_pos;
    Vec3 end_pos;
    double cached_length_sq = 0.0;

    [[nodiscard]] bool extends() const {
        return setting->use_virtual_end || setting->extend_end_bone;
    }
    [[nodiscard]] int valid_end_bone() const { return setting->use_virtual_end ? middle : end; }
    [[nodiscard]] Vec3 pole_direction_vector() const {
        return direction_vector(setting->pole_direction, setting->pole_direction_vector);
    }

    void cache_current_vectors() {
        root_info.current_vector = normalized(mid_pos - root_pos);
        mid_info.current_vector = normalized(end_pos - mid_pos);
    }

    void init_current_joint_rotations(Bones& bones) {
        Quat parent_gpose = bones.parent_global_rotation(root);
        root_info.current_lrest = bones.pose_rotation(root);
        root_info.current_grest = normalized(parent_gpose * root_info.current_lrest);
        root_info.current_lpose = bones.pose_rotation(root);
        root_info.current_gpose = normalized(parent_gpose * root_info.current_lpose);
        parent_gpose = root_info.current_gpose;
        // The middle joint's pose is relative to the root joint's.
        const Quat relative =
            inverse(bones.global_pose_rotation(root)) * bones.global_pose_rotation(middle);
        mid_info.current_lrest = relative;
        mid_info.current_grest = normalized(parent_gpose * relative);
        mid_info.current_lpose = relative;
        mid_info.current_gpose = normalized(parent_gpose * relative);
        cache_current_vectors();
    }

    // Makes the bones' rotations from the chain's positions, rolled so the middle bone's
    // pole_direction faces the pole.
    void cache_current_joint_rotations(Bones& bones, Vec3 pole_destination) {
        const Quat parent_gpose = bones.parent_global_rotation(root);
        root_info.current_lrest = bones.pose_rotation(root);
        root_info.current_grest = normalized(parent_gpose * root_info.current_lrest);
        Vec3 from = root_info.forward_vector;
        Vec3 to = normalized(xform_inv(root_info.current_grest, root_info.current_vector));
        root_info.current_lpose = root_info.current_lrest * swing(arc(from, to), from);
        root_info.current_gpose = normalized(parent_gpose * root_info.current_lpose);
        Quat root_gpose = root_info.current_gpose;
        mid_info.current_lrest =
            inverse(bones.global_pose_rotation(root)) * bones.global_pose_rotation(middle);
        mid_info.current_grest = normalized(root_gpose * mid_info.current_lrest);
        from = mid_info.forward_vector;
        to = normalized(xform_inv(mid_info.current_grest, mid_info.current_vector));
        mid_info.current_lpose = mid_info.current_lrest * swing(arc(from, to), from);
        mid_info.current_gpose = normalized(root_gpose * mid_info.current_lpose);
        const bool pole_defined =
            setting->pole_direction != "none" && (setting->pole_direction != "custom" ||
                                                  !is_zero_approx(setting->pole_direction_vector));
        if (!pole_defined) {
            return;
        }
        // The roll that puts the pole vector on the plane, on the side nearer the pole.
        const Vec3 pole_dir = projected_normal(root_pos, end_pos, pole_destination);
        if (is_zero_approx(pole_dir)) {
            return;
        }
        const Vec3 a = normalized(mid_info.current_vector);
        const Vec3 k = normalized(xform(mid_info.current_gpose, pole_direction_vector()));
        const Vec3 n = normalized(cross(pole_dir, normalized(mid_pos - root_pos)));
        if (is_zero_approx(a) || is_zero_approx(k) || is_zero_approx(n) ||
            is_zero_approx(dot(n, k))) {
            return;
        }
        // c0 cos(t) + c1 sin(t) + c2 = 0
        const double c0 = dot(n, k - (a * dot(k, a)));
        const double c1 = dot(n, cross(a, k));
        const double c2 = static_cast<double>(dot(n, a)) * static_cast<double>(dot(k, a));
        const double r = std::sqrt((c0 * c0) + (c1 * c1));
        const double cos_arg = std::clamp(-c2 / r, -1.0, 1.0);
        const double phi = slash::atan2(static_cast<float>(c1), static_cast<float>(c0));
        const double acosv = acos_of(static_cast<float>(cos_arg));
        const double t1 = phi + acosv;
        const double t2 = phi - acosv;
        const Quat q1 = axis_angle(a, static_cast<float>(t1));
        const Quat q2 = axis_angle(a, static_cast<float>(t2));
        const Vec3 pole_proj = normalized(snap_vector_to_plane(n, pole_dir));
        const Vec3 k1p = normalized(snap_vector_to_plane(n, xform(q1, k)));
        const Vec3 k2p = normalized(snap_vector_to_plane(n, xform(q2, k)));
        const double s1 = is_zero_approx(pole_proj) ? std::abs(t1) : dot(k1p, pole_proj);
        const double s2 = is_zero_approx(pole_proj) ? std::abs(t2) : dot(k2p, pole_proj);
        const auto t = static_cast<float>(s1 >= s2 ? t1 : t2);
        const Quat root_roll = axis_angle(root_info.forward_vector, t);
        const Quat mid_roll = axis_angle(mid_info.forward_vector, t);
        root_info.current_lpose = root_info.current_lpose * root_roll;
        root_info.current_gpose = normalized(parent_gpose * root_info.current_lpose);
        root_gpose = root_info.current_gpose;
        mid_info.current_lpose = inverse(root_roll) * mid_info.current_lpose * mid_roll;
        mid_info.current_gpose = normalized(root_gpose * mid_info.current_lpose);
    }
};

// TwoBoneIK3D::_get_bone_global_rest: with mutable bone axes, the rest rotations down from the
// root with the posed positions.
world::Affine3D two_bone_global_rest(Bones& bones, int bone, int root, bool mutable_bone_axes) {
    if (!mutable_bone_axes) {
        return bones.global_rest(bone);
    }
    world::Affine3D tr = bones.global_rest(root);
    tr.origin = tr.origin - bones.rest(root).position + bones.pose(root).position;
    std::vector<int> path;
    for (int b = bone; b != root && b != -1; b = bones.parent(b)) {
        path.push_back(b);
    }
    std::ranges::reverse(path);
    for (const int b : path) {
        world::Affine3D step = bones.rest_basis(b);
        step.origin = bones.pose(b).position;
        tr = tr * step;
    }
    return tr;
}

// TwoBoneIK3D::_update_bone_axis: each bone's length and the way it points at rest. False when
// a bone has no length, as Godot clears the joints.
bool update_bone_axis(TwoBone& tb, Bones& bones, bool mutable_bone_axes) {
    const world::TwoBoneIKSetting& s = *tb.setting;
    tb.root_info = {};
    tb.mid_info = {};
    const int end = tb.valid_end_bone();
    Vec3 axis = bone_axis(bones, end, s.end_bone_direction, mutable_bone_axes);
    Vec3 global_rest_origin;
    if (tb.extends() && s.end_bone_length > 0.0F && !is_zero_approx(axis)) {
        tb.end_pos = bones.global_pose(end).apply(axis * s.end_bone_length);
        global_rest_origin = two_bone_global_rest(bones, end, tb.root, mutable_bone_axes)
                                 .apply(axis * s.end_bone_length);
    } else {
        tb.end_pos = bones.global_pose(end).origin;
        global_rest_origin = two_bone_global_rest(bones, end, tb.root, mutable_bone_axes).origin;
    }
    Vec3 origin = two_bone_global_rest(bones, tb.middle, tb.root, mutable_bone_axes).origin;
    axis = global_rest_origin - origin;
    global_rest_origin = origin;
    if (is_zero_approx(axis)) {
        return false;
    }
    tb.mid_pos = bones.global_pose(tb.middle).origin;
    tb.mid_info.forward_vector =
        normalized(xform_inv(decompose(bones.global_rest(tb.middle)).rotation, axis));
    tb.mid_info.length = length(axis);
    origin = two_bone_global_rest(bones, tb.root, tb.root, mutable_bone_axes).origin;
    axis = global_rest_origin - origin;
    if (is_zero_approx(axis)) {
        return false;
    }
    tb.root_pos = bones.global_pose(tb.root).origin;
    tb.root_info.forward_vector =
        normalized(xform_inv(decompose(bones.global_rest(tb.root)).rotation, axis));
    tb.root_info.length = length(axis);
    tb.init_current_joint_rotations(bones);
    const double total = static_cast<double>(tb.root_info.length) + tb.mid_info.length;
    tb.cached_length_sq = total * total;
    return true;
}

// TwoBoneIK3D::_process_joints.
void process_two_bone(TwoBone& tb, Bones& bones, Vec3 target, Vec3 pole_destination) {
    Vec3 destination = target;
    tb.root_pos = bones.global_pose(tb.root).origin;
    Vec3 root_to_destination = destination - tb.root_pos;
    if (is_zero_approx(root_to_destination)) {
        return;
    }
    const double rd_len_sq = length_squared(root_to_destination);
    if (rd_len_sq >= tb.cached_length_sq) {
        // Out of reach: straight.
        const Vec3 rd_nrm = normalized(root_to_destination);
        tb.mid_pos = tb.root_pos + (rd_nrm * tb.root_info.length);
        tb.end_pos = tb.mid_pos + (rd_nrm * tb.mid_info.length);
    } else {
        // Too near to reach: pushed out to the nearest point it can.
        const double sub = static_cast<double>(tb.root_info.length) - tb.mid_info.length;
        if (rd_len_sq < sub * sub) {
            const Vec3 push_nrm = normalized(destination - tb.root_pos);
            destination = tb.root_pos + (push_nrm * static_cast<float>(std::abs(sub)));
            root_to_destination = destination - tb.root_pos;
        }
        tb.end_pos = destination;
        // Bent: the middle where the circles around the root and the end meet, nearer the pole.
        const double l_chain = length(root_to_destination);
        const Vec3 u = normalized(root_to_destination);
        const Vec3 pole_vec = projected_normal(tb.root_pos, tb.end_pos, pole_destination);
        const double r_root = tb.root_info.length;
        const double r_mid = tb.mid_info.length;
        const double a =
            ((l_chain * l_chain) + (r_root * r_root) - (r_mid * r_mid)) / (2.0 * l_chain);
        const double h = std::sqrt(std::max(0.0, (r_root * r_root) - (a * a)));
        const Vec3 center = tb.root_pos + (u * static_cast<float>(a));
        const Vec3 det_plus = center + (pole_vec * static_cast<float>(h));
        const Vec3 det_minus = center - (pole_vec * static_cast<float>(h));
        tb.mid_pos = distance_squared(pole_destination, det_plus) <
                             distance_squared(pole_destination, det_minus)
                         ? det_plus
                         : det_minus;
    }
    tb.cache_current_vectors();
    tb.cache_current_joint_rotations(bones, pole_destination);
    bones.set_rotation(tb.root, tb.root_info.current_lpose);
    bones.set_rotation(tb.middle, local_pose_rotation(bones, tb.middle, tb.mid_info.current_gpose));
}

// Whether a bone is below another (or is it).
bool descends(const Bones& bones, int bone, int ancestor) {
    for (int b = bone; b >= 0; b = bones.parent(b)) {
        if (b == ancestor) {
            return true;
        }
    }
    return false;
}

// ---- ChainIK3D and IterateIK3D ----

enum class Solver : std::uint8_t { Fabrik, Ccd, Jacobian };

// One chain of an iterating solver and what it keeps from tick to tick: Godot's
// IterateIK3DSetting.
struct Chain {
    world::ChainIKSetting setting; // as it was when made; a change makes it again
    bool mutable_bone_axes = true;
    std::size_t model_nodes = 0;
    int root = -1;
    int end = -1;
    std::vector<int> joints;
    std::vector<world::ChainIKJoint> joint_settings;
    std::vector<SolverInfo> infos;
    std::vector<Vec3> chain;
    bool simulated = false;

    [[nodiscard]] bool extends_end() const {
        return setting.extend_end_bone && setting.end_bone_length > 0.0F;
    }

    void cache_current_vector(int index) {
        const auto chain_size = static_cast<int>(chain.size());
        if (index - 1 >= 0) {
            auto& info = infos[static_cast<std::size_t>(index - 1)];
            if (info.made) {
                info.current_vector = normalized(chain[static_cast<std::size_t>(index)] -
                                                 chain[static_cast<std::size_t>(index - 1)]);
            }
        }
        if (index + 1 < chain_size) {
            auto& info = infos[static_cast<std::size_t>(index)];
            if (info.made) {
                info.current_vector = normalized(chain[static_cast<std::size_t>(index) + 1] -
                                                 chain[static_cast<std::size_t>(index)]);
            }
        }
    }

    void cache_current_vectors() {
        for (std::size_t i = 0; i < joints.size(); ++i) {
            if (infos[i].made && i + 1 < chain.size()) {
                infos[i].current_vector = normalized(chain[i + 1] - chain[i]);
            }
        }
    }

    // update_chain_coordinate_bw: moved unless that flips the bone before it.
    void update_bw(int index, Vec3 position) {
        const auto i = static_cast<std::size_t>(index);
        if (is_zero_approx(length(position - chain[i]))) {
            return;
        }
        const int head = index - 1;
        if (head >= 0 && head < static_cast<int>(infos.size())) {
            const auto& info = infos[static_cast<std::size_t>(head)];
            if (info.made) {
                const Vec3 old_head_to_tail = info.current_vector;
                const Vec3 new_head_to_tail =
                    normalized(position - chain[static_cast<std::size_t>(head)]);
                if (is_equal_approx(dot(old_head_to_tail, new_head_to_tail), -1.0)) {
                    chain[i] =
                        chain[static_cast<std::size_t>(head)] + (old_head_to_tail * info.length);
                    return;
                }
            }
        }
        chain[i] = position;
        cache_current_vector(index);
    }

    // update_chain_coordinate_fw: moved unless that flips its own bone.
    void update_fw(int index, Vec3 position) {
        const auto i = static_cast<std::size_t>(index);
        if (is_zero_approx(length(position - chain[i]))) {
            return;
        }
        const int tail = index + 1;
        if (tail < static_cast<int>(infos.size())) {
            const auto& info = infos[i];
            if (info.made) {
                const Vec3 old_head_to_tail = info.current_vector;
                const Vec3 new_head_to_tail =
                    normalized(chain[static_cast<std::size_t>(tail)] - position);
                if (is_equal_approx(dot(old_head_to_tail, new_head_to_tail), -1.0)) {
                    chain[i] =
                        chain[static_cast<std::size_t>(tail)] - (old_head_to_tail * info.length);
                    return;
                }
            }
        }
        chain[i] = position;
        cache_current_vector(index);
    }

    void init_current_joint_rotations(Bones& bones) {
        Quat parent_gpose = bones.parent_global_rotation(root);
        for (std::size_t i = 0; i < joints.size(); ++i) {
            auto& info = infos[i];
            if (!info.made) {
                continue;
            }
            info.current_lrest = bones.pose_rotation(joints[i]);
            info.current_grest = normalized(parent_gpose * info.current_lrest);
            info.current_lpose = bones.pose_rotation(joints[i]);
            info.current_gpose = normalized(parent_gpose * info.current_lpose);
            parent_gpose = info.current_gpose;
        }
        cache_current_vectors();
    }

    // IterateIK3DSetting::init_joints.
    void init_joints(Bones& bones, bool mutable_axes) {
        chain.clear();
        for (std::size_t i = 0; i < joints.size(); ++i) {
            chain.push_back(bones.global_pose(joints[i]).origin);
            const bool last = i + 1 == joints.size();
            if (last && extends_end()) {
                const Vec3 axis = bone_axis(bones, end, setting.end_bone_direction, mutable_axes);
                if (is_zero_approx(axis)) {
                    continue;
                }
                infos[i].made = true;
                infos[i].forward_vector =
                    snap_vector_to_plane(rotation_axis_vector(joint_settings[i]), normalized(axis));
                infos[i].length = setting.end_bone_length;
                chain.push_back(bones.global_pose(joints[i]).apply(axis * setting.end_bone_length));
            } else if (!last) {
                const Vec3 axis = bones.rest(joints[i + 1]).position;
                if (is_zero_approx(axis)) {
                    continue;
                }
                infos[i].made = true;
                infos[i].forward_vector =
                    snap_vector_to_plane(rotation_axis_vector(joint_settings[i]), normalized(axis));
                infos[i].length = length(axis);
            }
        }
        init_current_joint_rotations(bones);
    }

    // IterateIK3D::_update_bone_axis, with mutable bone axes: lengths from the posed positions.
    void update_bone_axis(Bones& bones, bool mutable_axes) {
        const int len = static_cast<int>(infos.size()) - 1;
        for (int j = 0; j < len; ++j) {
            auto& info = infos[static_cast<std::size_t>(j)];
            if (!info.made) {
                continue;
            }
            const Vec3 axis = bones.pose(joints[static_cast<std::size_t>(j) + 1]).position;
            if (is_zero_approx(axis)) {
                continue;
            }
            info.forward_vector = snap_vector_to_plane(
                rotation_axis_vector(joint_settings[static_cast<std::size_t>(j)]),
                normalized(axis));
            info.length = length(axis);
        }
        if (setting.extend_end_bone && len >= 0) {
            auto& info = infos[static_cast<std::size_t>(len)];
            if (info.made) {
                const Vec3 axis = bone_axis(bones, end, setting.end_bone_direction, mutable_axes);
                if (!is_zero_approx(axis)) {
                    info.forward_vector = snap_vector_to_plane(
                        rotation_axis_vector(joint_settings[static_cast<std::size_t>(len)]),
                        normalized(axis));
                    info.length = setting.end_bone_length;
                }
            }
        }
    }

    // IterateIK3DSetting::cache_current_joint_rotations: the bones' rotations from the chain,
    // each at most angular_delta_limit from what it was, then the chain from the rotations.
    void cache_current_joint_rotations(Bones& bones, double angular_delta_limit = kPi) {
        Quat parent_gpose = bones.parent_global_rotation(root);
        for (std::size_t i = 0; i < joints.size(); ++i) {
            auto& info = infos[i];
            if (!info.made) {
                continue;
            }
            info.current_lrest = bones.pose_rotation(joints[i]);
            info.current_grest = normalized(parent_gpose * info.current_lrest);
            const Vec3 from = info.forward_vector;
            const Vec3 to = normalized(xform_inv(info.current_grest, info.current_vector));
            const Quat prev = info.current_lpose;
            if (joint_settings[i].rotation_axis == "all") {
                info.current_lpose = info.current_lrest * swing(arc(from, to), from);
            } else {
                // Steadier near half a turn.
                info.current_lpose =
                    info.current_lrest *
                    from_to_rotation_by_axis(from, to,
                                             normalized(rotation_axis_vector(joint_settings[i])));
            }
            const double diff = angle_to(prev, info.current_lpose);
            if (!is_zero_approx(diff)) {
                info.current_lpose =
                    slerp(prev, info.current_lpose,
                          static_cast<float>(std::min(1.0, angular_delta_limit / diff)));
            }
            info.current_gpose = normalized(parent_gpose * info.current_lpose);
            parent_gpose = info.current_gpose;
        }
        if (chain.empty()) {
            return;
        }
        chain[0] = bones.global_pose(root).origin;
        for (std::size_t i = 0; i < infos.size(); ++i) {
            const auto& info = infos[i];
            if (!info.made || i + 1 >= chain.size()) {
                continue;
            }
            chain[i + 1] =
                chain[i] + (xform(info.current_gpose, info.forward_vector) * info.length);
        }
        cache_current_vectors();
    }
};

// IterateIK3DJointSetting::get_projected_rotation: a vector turned only about the joint's axis.
Vec3 projected_rotation(const world::ChainIKJoint& joint, const Quat& offset, Vec3 vector) {
    const double almost_one = 1.0 - static_cast<double>(kCmpEpsilon);
    const Vec3 axis = normalized(rotation_axis_vector(joint));
    const Vec3 local_vector = xform_inv(offset, vector);
    const double l = length(local_vector);
    Vec3 projected = snap_vector_to_plane(axis, normalized(local_vector));
    if (!is_zero_approx(l)) {
        projected = normalized(projected) * static_cast<float>(l);
    }
    if (std::abs(dot(normalized(local_vector), axis)) > almost_one) {
        return vector;
    }
    return xform(offset, projected);
}

// JointLimitation3D::make_space: the cone's space, its axis along forward.
Quat limitation_space(const world::ChainIKJoint& joint, Vec3 forward) {
    const double almost_one = 1.0 - static_cast<double>(kCmpEpsilon);
    const Quat offset = normalized(world::quaternion(joint.limitation_rotation_offset));
    const Vec3 axis_y = normalized(forward);
    Vec3 axis_x = normalized(
        direction_vector(joint.limitation_right_axis, joint.limitation_right_axis_vector));
    if (is_zero_approx(axis_x) || std::abs(dot(axis_x, axis_y)) > almost_one) {
        return normalized(arc({0.0F, 1.0F, 0.0F}, axis_y) * offset);
    }
    const Vec3 axis_z = normalized(cross(axis_x, axis_y));
    axis_x = normalized(cross(axis_y, axis_z));
    return normalized(basis_rotation(axis_x, axis_y, axis_z) * offset);
}

// JointLimitationCone3D::_solve: a direction kept within the cone around +y.
Vec3 cone_solve(float angle, Vec3 direction) {
    const Vec3 center_axis{0.0F, 1.0F, 0.0F};
    const float current_angle = angle_to(direction, center_axis);
    const float max_angle = angle * 0.5F;
    if (current_angle <= max_angle) {
        return direction;
    }
    const Vec3 plane_normal = is_equal_approx(current_angle, kPi)
                                  ? any_perpendicular(center_axis)
                                  : normalized(cross(center_axis, direction));
    Vec3 limited_dir = xform(axis_angle(plane_normal, max_angle), center_axis);
    const Vec3 projection = direction - (center_axis * dot(direction, center_axis));
    if (length_squared(projection) > kCmpEpsilon) {
        const Vec3 side_dir = normalized(projection);
        limited_dir =
            xform(axis_angle(normalized(cross(center_axis, side_dir)), max_angle), center_axis);
    }
    return normalized(limited_dir);
}

// IterateIK3DJointSetting::get_limited_rotation: a vector kept within the joint's cone.
Vec3 limited_rotation(const world::ChainIKJoint& joint, const Quat& offset, Vec3 vector,
                      Vec3 forward) {
    const Vec3 local_vector = xform_inv(offset, vector);
    const float l = length(local_vector);
    if (is_zero_approx(l)) {
        return vector;
    }
    const Quat space = limitation_space(joint, forward);
    const Vec3 dir = normalized(local_vector);
    const Vec3 limited =
        xform(space, cone_solve(joint.limitation_angle, xform_inv(space, dir))) * l;
    return xform(offset, limited);
}

// The joint limits a solver applies after moving a bone's tail.
template <typename Update>
void constrain(Chain& c, int head, int tail, const Update& update) {
    const auto h = static_cast<std::size_t>(head);
    const auto t = static_cast<std::size_t>(tail);
    const world::ChainIKJoint& joint = c.joint_settings[h];
    const SolverInfo& info = c.infos[h];
    if (joint.rotation_axis != "all") {
        update(c.chain[h] + projected_rotation(joint, info.current_grest, c.chain[t] - c.chain[h]));
    }
    if (joint.limitation == "cone") {
        update(c.chain[h] + limited_rotation(joint, info.current_grest, c.chain[t] - c.chain[h],
                                             info.forward_vector));
    }
}

bool has_length(const SolverInfo& info) {
    return info.made && !is_zero_approx(info.length);
}

// FABRIK3D::_solve_iteration.
void solve_fabrik(Chain& c, Bones& bones, Vec3 destination) {
    const auto joint_size = static_cast<int>(c.joints.size());
    bool first = true;
    for (int i = joint_size - 1; i >= 0; --i) {
        const auto& info = c.infos[static_cast<std::size_t>(i)];
        if (!has_length(info)) {
            continue;
        }
        const int head = i;
        const int tail = i + 1;
        if (first) {
            c.update_bw(tail, destination);
            first = false;
        }
        c.update_bw(head, limit_length(c.chain[static_cast<std::size_t>(tail)],
                                       c.chain[static_cast<std::size_t>(head)], info.length));
        const auto h = static_cast<std::size_t>(head);
        const auto t = static_cast<std::size_t>(tail);
        const world::ChainIKJoint& joint = c.joint_settings[h];
        if (joint.rotation_axis != "all") {
            c.update_bw(head, c.chain[t] + projected_rotation(joint, info.current_grest,
                                                              c.chain[h] - c.chain[t]));
        }
        if (joint.limitation == "cone") {
            c.update_bw(head, c.chain[t] + limited_rotation(joint, info.current_grest,
                                                            c.chain[h] - c.chain[t],
                                                            info.forward_vector));
        }
    }
    first = true;
    for (int i = 0; i < joint_size; ++i) {
        const auto& info = c.infos[static_cast<std::size_t>(i)];
        if (!has_length(info)) {
            continue;
        }
        const int head = i;
        const int tail = i + 1;
        if (first) {
            c.update_fw(head, bones.global_pose(c.joints[static_cast<std::size_t>(head)]).origin);
            first = false;
        }
        c.update_fw(tail, limit_length(c.chain[static_cast<std::size_t>(head)],
                                       c.chain[static_cast<std::size_t>(tail)], info.length));
        constrain(c, head, tail, [&](Vec3 p) { c.update_fw(tail, p); });
    }
}

// CCDIK3D::_solve_iteration.
void solve_ccd(Chain& c, Vec3 destination) {
    const auto joint_size = static_cast<int>(c.joints.size());
    const auto chain_size = static_cast<int>(c.chain.size());
    for (int ancestor = joint_size - 1; ancestor >= 0; --ancestor) {
        for (int i = ancestor; i < joint_size; ++i) {
            const auto& info = c.infos[static_cast<std::size_t>(i)];
            if (!has_length(info)) {
                continue;
            }
            const int head = i;
            const int tail = i + 1;
            const Vec3 current_head = c.chain[static_cast<std::size_t>(head)];
            const Vec3 current_effector = c.chain[static_cast<std::size_t>(chain_size - 1)];
            const Vec3 head_to_effector = current_effector - current_head;
            const Vec3 head_to_destination = destination - current_head;
            if (is_zero_approx(length_squared(head_to_destination) *
                               length_squared(head_to_effector))) {
                continue;
            }
            const Quat to_rot = arc(normalized(head_to_effector), normalized(head_to_destination));
            const Vec3 to_tail = c.chain[static_cast<std::size_t>(tail)] - current_head;
            c.update_fw(tail, current_head + xform(to_rot, to_tail));
            constrain(c, head, tail, [&](Vec3 p) { c.update_fw(tail, p); });
        }
    }
}

// JacobianIK3D::_solve_iteration.
void solve_jacobian(Chain& c, Vec3 destination, double angular_delta_limit) {
    const auto joint_size = static_cast<int>(c.joints.size());
    const auto chain_size = static_cast<int>(c.chain.size());
    for (int i = 0; i < joint_size; ++i) {
        const auto& info = c.infos[static_cast<std::size_t>(i)];
        if (!has_length(info)) {
            continue;
        }
        const Vec3 current_head = c.chain[static_cast<std::size_t>(i)];
        const Vec3 current_effector = c.chain[static_cast<std::size_t>(chain_size - 1)];
        const Vec3 head_to_effector = current_effector - current_head;
        const Vec3 effector_to_destination = destination - current_effector;
        const Vec3 axis = cross(head_to_effector, effector_to_destination);
        if (is_zero_approx(length_squared(axis))) {
            continue;
        }
        // Clipped by angular_delta_limit, to stay steady.
        const double angle =
            std::min(static_cast<double>(length(axis)) /
                         std::max(static_cast<double>(kCmpEpsilon),
                                  static_cast<double>(length_squared(head_to_effector))),
                     angular_delta_limit);
        const Quat to_rot = axis_angle(normalized(axis), static_cast<float>(angle));
        for (int j = i + 1; j < chain_size; ++j) {
            const Vec3 to_tail = c.chain[static_cast<std::size_t>(j)] - current_head;
            c.update_fw(j, current_head + xform(to_rot, to_tail));
            const int k = j - 1;
            const auto kk = static_cast<std::size_t>(k);
            const world::ChainIKJoint& joint = c.joint_settings[kk];
            const auto jj = static_cast<std::size_t>(j);
            if (joint.rotation_axis != "all") {
                c.update_fw(j, c.chain[kk] + projected_rotation(joint, info.current_grest,
                                                                c.chain[jj] - c.chain[kk]));
            }
            if (joint.limitation == "cone") {
                c.update_fw(j, c.chain[kk] + limited_rotation(joint, info.current_grest,
                                                              c.chain[jj] - c.chain[kk],
                                                              info.forward_vector));
            }
        }
    }
}

// The bones from root down to end, as ChainIK3D::_update_joints finds them; empty when end isn't
// under root.
std::vector<int> chain_joints(const Bones& bones, int root, int end) {
    if (root < 0 || end < 0 || !descends(bones, end, root)) {
        return {};
    }
    std::vector<int> out;
    for (int b = end; b != root; b = bones.parent(b)) {
        out.push_back(b);
    }
    out.push_back(root);
    std::ranges::reverse(out);
    return out;
}

// ---- Modifiers in order ----

enum class Kind : std::uint8_t { TwoBone, Fabrik, Ccd, Jacobian, Simulator };
constexpr std::array<std::string_view, 5> kKindNames{"TwoBoneIK3D", "FABRIK3D", "CCDIK3D",
                                                     "JacobianIK3D", "PhysicalBoneSimulator3D"};

struct Modifier {
    entt::entity entity{entt::null};
    Kind kind = Kind::TwoBone;
};

// The fields every iterating solver shares.
struct Iterating {
    const std::vector<world::ChainIKSetting>* settings = nullptr;
    double min_distance = 0.0;
    double angular_delta_limit = 0.0;
    int max_iterations = 0;
    float influence = 1.0F;
    bool active = true;
    bool mutable_bone_axes = true;
    bool deterministic = false;
};

template <typename T>
Iterating iterating_of(const T& c) {
    return {.settings = &c.settings,
            .min_distance = c.min_distance,
            .angular_delta_limit = c.angular_delta_limit,
            .max_iterations = c.max_iterations,
            .influence = c.influence,
            .active = c.active,
            .mutable_bone_axes = c.mutable_bone_axes,
            .deterministic = c.deterministic};
}

std::optional<Iterating> iterating(const world::World& world, const Modifier& m) {
    const auto& ecs = world.ecs();
    switch (m.kind) {
    case Kind::Fabrik:
        return iterating_of(ecs.get<const world::FABRIK3D>(m.entity));
    case Kind::Ccd:
        return iterating_of(ecs.get<const world::CCDIK3D>(m.entity));
    case Kind::Jacobian:
        return iterating_of(ecs.get<const world::JacobianIK3D>(m.entity));
    case Kind::TwoBone:
    case Kind::Simulator:
        break;
    }
    return std::nullopt;
}

void add_modifiers_of(const world::World& world, entt::entity entity, std::vector<Modifier>& out) {
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::TwoBoneIK3D>(entity)) {
        out.push_back({entity, Kind::TwoBone});
    }
    if (ecs.all_of<world::FABRIK3D>(entity)) {
        out.push_back({entity, Kind::Fabrik});
    }
    if (ecs.all_of<world::CCDIK3D>(entity)) {
        out.push_back({entity, Kind::Ccd});
    }
    if (ecs.all_of<world::JacobianIK3D>(entity)) {
        out.push_back({entity, Kind::Jacobian});
    }
    if (ecs.all_of<world::PhysicalBoneSimulator3D>(entity)) {
        out.push_back({entity, Kind::Simulator});
    }
}

std::vector<Modifier> modifiers_of(const world::World& world, entt::entity entity) {
    std::vector<Modifier> out;
    add_modifiers_of(world, entity, out);
    if (const auto* transform = world.ecs().try_get<const world::Transform3D>(entity);
        transform != nullptr) {
        for (const entt::entity child : transform->children) {
            if (child != entt::null && world.valid(child)) {
                add_modifiers_of(world, child, out);
            }
        }
    }
    return out;
}

// What an entity is kept under: its ID, which outlives the handles restoring a copy of the world
// hands out anew.
std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

// Every chain that carries on from tick to tick, by drawing entity, modifier entity, kind and
// setting.
struct ModifierState {
    std::map<std::string, Chain, std::less<>> chains;
};

std::string chain_key(const world::World& world, entt::entity drawn, const Modifier& m,
                      std::size_t index) {
    return key_of(world, drawn) + "/" + key_of(world, m.entity) + "/" +
           std::string(kKindNames[static_cast<std::size_t>(m.kind)]) + "/" + std::to_string(index);
}

// How a pass runs the iterating solvers that carry on from tick to tick: stepping them (the
// system) or showing where they were left.
enum class Pass : std::uint8_t { Step, Show };

struct Run {
    const world::World* world = nullptr;
    entt::entity entity{entt::null};
    const Model* model = nullptr;
    world::Affine3D place;
    world::Affine3D inverse_place;
    const Placer* global = nullptr;
    Pass pass = Pass::Show;
    ModifierState* state = nullptr;      // stepping: where chains are kept
    const ModifierState* kept = nullptr; // showing: where they were left
    std::set<std::string>* seen = nullptr;
    // Stepping: where the physical bones that follow the pose go.
    std::vector<std::pair<entt::entity, world::Affine3D>>* follows = nullptr;

    [[nodiscard]] std::optional<Vec3> where(entt::entity target) const {
        if (target == entt::null || !world->valid(target)) {
            return std::nullopt;
        }
        return inverse_place.apply((*global)(target).origin);
    }
};

void run_two_bone(const Run& run, const world::TwoBoneIK3D& modifier, Bones& bones) {
    for (const world::TwoBoneIKSetting& s : modifier.settings) {
        TwoBone tb;
        tb.setting = &s;
        tb.root = find_node(*run.model, s.root_bone);
        tb.middle = find_node(*run.model, s.middle_bone);
        tb.end = s.use_virtual_end ? -1 : find_node(*run.model, s.end_bone);
        const bool end_valid = s.use_virtual_end ? !is_zero_approx(s.end_bone_length) : tb.end >= 0;
        if (tb.root < 0 || tb.middle < 0 || !end_valid || !descends(bones, tb.middle, tb.root) ||
            (!s.use_virtual_end && !descends(bones, tb.end, tb.middle))) {
            continue;
        }
        const std::optional<Vec3> target = run.where(s.target);
        const std::optional<Vec3> pole = run.where(s.pole);
        if (!target || !pole || !update_bone_axis(tb, bones, modifier.mutable_bone_axes)) {
            continue;
        }
        // First, to catch poses of bones above the chain.
        tb.cache_current_joint_rotations(bones, *pole);
        process_two_bone(tb, bones, *target, *pole);
    }
}

// IterateIK3D::_init_joints and _process_ik for one chain: false when the chain can't run.
bool prepare_chain(Chain& c, const world::ChainIKSetting& s, const Iterating& it, Bones& bones,
                   const Model& model) {
    const bool dirty = c.joints.empty() || !(c.setting == s) ||
                       c.mutable_bone_axes != it.mutable_bone_axes ||
                       c.model_nodes != model.nodes.size();
    if (dirty) {
        c = Chain{};
        c.setting = s;
        c.mutable_bone_axes = it.mutable_bone_axes;
        c.model_nodes = model.nodes.size();
        c.root = find_node(model, s.root_bone);
        c.end = find_node(model, s.end_bone);
        c.joints = chain_joints(bones, c.root, c.end);
        if (c.joints.empty()) {
            return false;
        }
        for (const int joint : c.joints) {
            const auto found = s.joints.find(model.nodes[static_cast<std::size_t>(joint)].name);
            c.joint_settings.push_back(found != s.joints.end() ? found->second
                                                               : world::ChainIKJoint{});
        }
        c.infos.resize(c.joints.size());
        c.init_joints(bones, it.mutable_bone_axes);
    } else if (it.deterministic) {
        c.init_joints(bones, it.mutable_bone_axes);
    }
    if (it.mutable_bone_axes) {
        c.update_bone_axis(bones, it.mutable_bone_axes);
    }
    c.simulated = false;
    return !c.chain.empty();
}

// IterateIK3D::_process_joints.
void process_chain(Chain& c, const Iterating& it, Solver solver, Bones& bones, Vec3 destination) {
    const double min_distance_squared = it.min_distance * it.min_distance;
    double distance_to_target_sq = std::numeric_limits<double>::infinity();
    if (c.simulated) {
        distance_to_target_sq = distance_squared(c.chain.back(), destination);
    }
    for (int count = 0; distance_to_target_sq > min_distance_squared && count < it.max_iterations;
         ++count) {
        switch (solver) {
        case Solver::Fabrik:
            solve_fabrik(c, bones, destination);
            break;
        case Solver::Ccd:
            solve_ccd(c, destination);
            break;
        case Solver::Jacobian:
            solve_jacobian(c, destination, it.angular_delta_limit);
            break;
        }
        c.cache_current_joint_rotations(bones, it.angular_delta_limit);
        distance_to_target_sq = distance_squared(c.chain.back(), destination);
    }
    c.simulated = true;
}

void apply_chain(const Chain& c, Bones& bones) {
    for (std::size_t i = 0; i < c.infos.size(); ++i) {
        if (has_length(c.infos[i])) {
            bones.set_rotation(c.joints[i], c.infos[i].current_lpose);
        }
    }
}

Solver solver_of(Kind kind) {
    switch (kind) {
    case Kind::Fabrik:
        return Solver::Fabrik;
    case Kind::Ccd:
        return Solver::Ccd;
    default:
        return Solver::Jacobian;
    }
}

void run_iterating(const Run& run, const Modifier& m, const Iterating& it, Bones& bones) {
    const Solver solver = solver_of(m.kind);
    for (std::size_t i = 0; i < it.settings->size(); ++i) {
        const world::ChainIKSetting& s = (*it.settings)[i];
        const std::optional<Vec3> target = run.where(s.target);
        if (it.deterministic) {
            Chain c;
            if (!prepare_chain(c, s, it, bones, *run.model) || !target) {
                continue;
            }
            c.cache_current_joint_rotations(bones);
            process_chain(c, it, solver, bones, *target);
            apply_chain(c, bones);
            continue;
        }
        const std::string key = chain_key(*run.world, run.entity, m, i);
        if (run.pass == Pass::Step) {
            run.seen->insert(key);
            Chain& c = run.state->chains[key];
            if (!prepare_chain(c, s, it, bones, *run.model) || !target) {
                continue;
            }
            c.cache_current_joint_rotations(bones);
            process_chain(c, it, solver, bones, *target);
            apply_chain(c, bones);
            continue;
        }
        // Where the last tick left it, fitted to the pose under it now.
        if (run.kept == nullptr) {
            continue;
        }
        const auto found = run.kept->chains.find(key);
        if (found == run.kept->chains.end() || !(found->second.setting == s) ||
            found->second.joints.empty() || found->second.chain.empty()) {
            continue;
        }
        Chain c = found->second;
        if (c.model_nodes != run.model->nodes.size() ||
            c.mutable_bone_axes != it.mutable_bone_axes) {
            continue;
        }
        if (it.mutable_bone_axes) {
            c.update_bone_axis(bones, it.mutable_bone_axes);
        }
        c.cache_current_joint_rotations(bones);
        apply_chain(c, bones);
    }
}

// PhysicalBoneSimulator3D::_process_modification: the bones that simulate go where their bodies
// are, parents first, and the bodies of the others follow their bones (PhysicalBone3D::
// reset_to_rest_position) when stepping.
void run_simulator(const Run& run, const Modifier& m, Bones& bones) {
    const auto& simulator = run.world->ecs().get<const world::PhysicalBoneSimulator3D>(m.entity);
    std::vector<PhysicalBone> physical = physical_bones(*run.world, m.entity, run.model);
    const auto depth = [&](const PhysicalBone& b) {
        std::size_t count = 0;
        for (int up = b.bone >= 0 ? bones.parent(b.bone) : -1; up >= 0; up = bones.parent(up)) {
            ++count;
        }
        return count;
    };
    std::ranges::stable_sort(physical, {}, depth);
    for (const PhysicalBone& b : physical) {
        if (b.bone < 0) {
            continue;
        }
        const world::Affine3D offset =
            body_offset(run.world->ecs().get<const world::PhysicalBone3D>(b.entity));
        if (!b.simulating) {
            if (run.follows != nullptr) {
                const world::Affine3D follow = run.place * bones.global_pose(b.bone) * offset;
                // Orthonormalized, as Godot's.
                world::Affine3D unscaled =
                    node_transform({.position = {}, .rotation = decompose(follow).rotation});
                unscaled.origin = follow.origin;
                run.follows->emplace_back(b.entity, unscaled);
            }
        } else if (simulator.simulating) {
            bones.set_global_pose(b.bone,
                                  run.inverse_place * (*run.global)(b.entity) * offset.inverse());
        }
    }
}

// Skeleton3D::_process_modifiers: each active modifier in order, influence of the way.
void run_modifiers(const Run& run, std::vector<NodePose>& pose) {
    const auto& ecs = run.world->ecs();
    for (const Modifier& m : modifiers_of(*run.world, run.entity)) {
        float influence = 1.0F;
        bool active = true;
        if (m.kind == Kind::TwoBone) {
            const auto& c = ecs.get<const world::TwoBoneIK3D>(m.entity);
            influence = c.influence;
            active = c.active;
        } else if (m.kind == Kind::Simulator) {
            const auto& c = ecs.get<const world::PhysicalBoneSimulator3D>(m.entity);
            influence = c.influence;
            active = c.active;
        } else {
            const Iterating it = *iterating(*run.world, m);
            influence = it.influence;
            active = it.active;
        }
        if (!active) {
            continue;
        }
        const std::vector<NodePose> old = influence < 1.0F ? pose : std::vector<NodePose>{};
        Bones bones(*run.model, pose);
        if (m.kind == Kind::TwoBone) {
            run_two_bone(run, ecs.get<const world::TwoBoneIK3D>(m.entity), bones);
        } else if (m.kind == Kind::Simulator) {
            run_simulator(run, m, bones);
        } else {
            run_iterating(run, m, *iterating(*run.world, m), bones);
        }
        if (influence < 1.0F) {
            // Transform3D::interpolate_with on each bone it turned.
            for (std::size_t i = 0; i < pose.size(); ++i) {
                if (old[i] == pose[i]) {
                    continue;
                }
                pose[i].position =
                    old[i].position + ((pose[i].position - old[i].position) * influence);
                pose[i].rotation = normalized(
                    slerp(normalized(old[i].rotation), normalized(pose[i].rotation), influence));
                pose[i].scale = old[i].scale + ((pose[i].scale - old[i].scale) * influence);
            }
        }
    }
}

} // namespace

bool has_modifiers(const world::World& world, entt::entity entity) {
    return !modifiers_of(world, entity).empty();
}

void apply_modifiers(const world::World& world, entt::entity entity, const Model& model,
                     const world::Affine3D& place, const Placer& global,
                     std::vector<NodePose>& pose) {
    if (model.nodes.empty() || pose.size() != model.nodes.size()) {
        return;
    }
    const auto* state = world.ecs().ctx().find<std::shared_ptr<ModifierState>>();
    const Run run{.world = &world,
                  .entity = entity,
                  .model = &model,
                  .place = place,
                  .inverse_place = place.inverse(),
                  .global = &global,
                  .pass = Pass::Show,
                  .kept = state != nullptr ? state->get() : nullptr};
    run_modifiers(run, pose);
}

void add_modifier_system(world::World& world, ModelLoader load) {
    if (world.ecs().ctx().contains<std::shared_ptr<ModifierState>>()) {
        return;
    }
    auto state = std::make_shared<ModifierState>();
    world.ecs().ctx().emplace<std::shared_ptr<ModifierState>>(state);
    world.add_reset([state](world::World& /*world*/) { state->chains.clear(); });
    world::set_physical_bone_linker(
        world, [load](const world::World& w) { return physical_bone_links(w, load); });
    world.add_system(
        "skeleton_modifiers",
        [state, load = std::move(load)](world::World& w, double /*tick_seconds*/) {
            std::set<std::string> seen;
            const world::Tree3D tree(w);
            const Placer global = [&tree](entt::entity e) { return tree.global(e); };
            for (const entt::entity entity : w.ecs().view<const world::MeshInstance3D>()) {
                const auto& instance = w.ecs().get<const world::MeshInstance3D>(entity);
                if (instance.mesh.empty() || !load) {
                    continue;
                }
                bool steps = false;
                const std::vector<Modifier> modifiers = modifiers_of(w, entity);
                for (const Modifier& m : modifiers) {
                    if (const auto it = iterating(w, m);
                        (it && it->active && !it->deterministic) || m.kind == Kind::Simulator) {
                        steps = true;
                    }
                }
                if (!steps) {
                    continue;
                }
                auto model = load(instance.mesh);
                if (!model || model.value()->nodes.empty()) {
                    continue;
                }
                // PhysicalBone3D::_start_physics_simulation and _stop_physics_simulation: a
                // body that simulates is top_level, out of its parent's space.
                for (const Modifier& m : modifiers) {
                    if (m.kind != Kind::Simulator) {
                        continue;
                    }
                    for (const PhysicalBone& b : physical_bones(w, m.entity, model.value().get())) {
                        set_top_level(w, tree, b.entity, b.simulating);
                    }
                }
                const auto* skeleton = w.ecs().try_get<const world::Skeleton3D>(entity);
                std::vector<NodePose> pose =
                    pose_of(*model.value(), clips(w, entity), skeleton, load);
                std::vector<std::pair<entt::entity, world::Affine3D>> follows;
                const world::Affine3D place = tree.global(entity);
                const Run run{.world = &w,
                              .entity = entity,
                              .model = model.value().get(),
                              .place = place,
                              .inverse_place = place.inverse(),
                              .global = &global,
                              .pass = Pass::Step,
                              .state = state.get(),
                              .seen = &seen,
                              .follows = &follows};
                run_modifiers(run, pose);
                for (const auto& [bone, where] : follows) {
                    set_global_transform(w, tree, bone, where);
                }
            }
            std::erase_if(state->chains,
                          [&](const auto& entry) { return !seen.contains(entry.first); });
            // Then the bone attachments, where the bones ended up.
            place_attachments(w, load);
        },
        {.doc = "Steps the chains of FABRIK3D, CCDIK3D and JacobianIK3D modifiers that aren't "
                "deterministic toward their targets, moves the physical bones that follow the "
                "pose and puts bone attachments where their bones are.",
         .priority = 920});
}

constexpr const char* kNoTarget = "names no target: name the entity the end reaches for";

std::vector<ModifierProblem> check_modifiers(const world::World& world, entt::entity entity,
                                             const Model* model) {
    std::vector<ModifierProblem> out;
    const std::vector<Modifier> modifiers = modifiers_of(world, entity);
    if (modifiers.empty()) {
        return out;
    }
    std::vector<std::string_view> names;
    if (model != nullptr) {
        names.reserve(model->nodes.size());
        for (const ModelNode& node : model->nodes) {
            names.emplace_back(node.name);
        }
    }
    for (const Modifier& m : modifiers) {
        const std::string prefix =
            "/components/" + std::string(kKindNames[static_cast<std::size_t>(m.kind)]);
        const auto problem = [&](const std::string& path, std::string message) {
            out.push_back({m.entity, prefix + path, std::move(message)});
        };
        if (model == nullptr) {
            problem("", "turns bones, but the entity's MeshInstance3D draws no model: set its mesh "
                        "to a model file with a skeleton");
            continue;
        }
        // A bone name, checked against the model; its index, or -1.
        const auto bone = [&](const std::string& path, const std::string& name) {
            if (name.empty()) {
                problem(path, "names no bone");
                return -1;
            }
            const int found = find_node(*model, name);
            if (found < 0) {
                std::string message = "the model has no bone or node named \"" + name + "\"";
                if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                problem(path, std::move(message));
            }
            return found;
        };
        const auto choice = [&](const std::string& path, const std::string& value,
                                const auto& choices) {
            if (std::ranges::find(choices, value) != choices.end()) {
                return;
            }
            std::string message = "\"" + value + "\" isn't one of";
            for (const std::string_view each : choices) {
                message += std::string(each == choices.front() ? " " : ", ") + "\"" +
                           std::string(each) + "\"";
            }
            if (const std::string_view hint = closest_match(value, choices); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            problem(path, std::move(message));
        };
        const auto entity_field = [&](const std::string& path, entt::entity target,
                                      const char* message) {
            if (target == entt::null || !world.valid(target)) {
                problem(path, message);
            }
        };
        std::vector<NodePose> rest = rest_pose(*model);
        const Bones bones(*model, rest);
        if (m.kind == Kind::TwoBone) {
            const auto& c = world.ecs().get<const world::TwoBoneIK3D>(m.entity);
            for (std::size_t i = 0; i < c.settings.size(); ++i) {
                const world::TwoBoneIKSetting& s = c.settings[i];
                const std::string at = "/settings/" + std::to_string(i);
                const int root = bone(at + "/root_bone", s.root_bone);
                const int middle = bone(at + "/middle_bone", s.middle_bone);
                if (root >= 0 && middle >= 0 && !descends(bones, middle, root)) {
                    problem(at + "/middle_bone",
                            "\"" + s.middle_bone + "\" isn't under \"" + s.root_bone + "\"");
                }
                if (s.use_virtual_end) {
                    if (is_zero_approx(s.end_bone_length)) {
                        problem(at + "/end_bone_length",
                                "a virtual end needs a length past the middle bone");
                    }
                } else {
                    const int end = bone(at + "/end_bone", s.end_bone);
                    if (middle >= 0 && end >= 0 && !descends(bones, end, middle)) {
                        problem(at + "/end_bone",
                                "\"" + s.end_bone + "\" isn't under \"" + s.middle_bone + "\"");
                    }
                }
                choice(at + "/end_bone_direction", s.end_bone_direction, kBoneDirections);
                choice(at + "/pole_direction", s.pole_direction, kSecondaryDirections);
                entity_field(at + "/target", s.target, kNoTarget);
                entity_field(at + "/pole", s.pole,
                             "names no pole: name the entity the middle bone bends toward");
            }
            continue;
        }
        if (m.kind == Kind::Simulator) {
            const auto& c = world.ecs().get<const world::PhysicalBoneSimulator3D>(m.entity);
            for (std::size_t i = 0; i < c.simulated_bones.size(); ++i) {
                (void)bone("/simulated_bones/" + std::to_string(i), c.simulated_bones[i]);
            }
            std::vector<entt::entity> owners(model->nodes.size(), entt::entity{entt::null});
            for (const PhysicalBone& b : physical_bones(world, m.entity, model)) {
                const std::string& name =
                    world.ecs().get<const world::PhysicalBone3D>(b.entity).bone_name;
                const auto bone_problem = [&](std::string message) {
                    out.push_back(
                        {b.entity, "/components/PhysicalBone3D/bone_name", std::move(message)});
                };
                const int found = name.empty() ? -1 : find_node(*model, name);
                if (name.empty()) {
                    bone_problem("names no bone: name the model's bone it moves");
                } else if (found < 0) {
                    std::string message = "the model has no bone or node named \"" + name + "\"";
                    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                        message += "; did you mean \"" + std::string(hint) + "\"?";
                    }
                    bone_problem(std::move(message));
                } else if (b.bone < 0) {
                    bone_problem("\"" + name + "\" already has a physical bone: \"" +
                                 std::string(world.id_of(owners[static_cast<std::size_t>(found)])) +
                                 "\"");
                } else {
                    owners[static_cast<std::size_t>(found)] = b.entity;
                }
            }
            continue;
        }
        const Iterating it = *iterating(world, m);
        for (std::size_t i = 0; i < it.settings->size(); ++i) {
            const world::ChainIKSetting& s = (*it.settings)[i];
            const std::string at = "/settings/" + std::to_string(i);
            const int root = bone(at + "/root_bone", s.root_bone);
            const int end = bone(at + "/end_bone", s.end_bone);
            std::vector<int> joints;
            if (root >= 0 && end >= 0) {
                joints = chain_joints(bones, root, end);
                if (joints.empty()) {
                    problem(at + "/end_bone",
                            "\"" + s.end_bone + "\" isn't \"" + s.root_bone + "\" or under it");
                }
            }
            choice(at + "/end_bone_direction", s.end_bone_direction, kBoneDirections);
            entity_field(at + "/target", s.target, kNoTarget);
            std::vector<std::string_view> chain_names;
            chain_names.reserve(joints.size());
            for (const int joint : joints) {
                chain_names.emplace_back(model->nodes[static_cast<std::size_t>(joint)].name);
            }
            for (const auto& [name, joint] : s.joints) {
                const std::string jat = at + "/joints/" + json::pointer_escape(name);
                if (!joints.empty() && std::ranges::find(chain_names, name) == chain_names.end()) {
                    std::string message = "\"" + name + "\" isn't a bone of the chain from \"" +
                                          s.root_bone + "\" to \"" + s.end_bone + "\"";
                    if (const std::string_view hint = closest_match(name, chain_names);
                        !hint.empty()) {
                        message += "; did you mean \"" + std::string(hint) + "\"?";
                    }
                    problem(jat, std::move(message));
                }
                choice(jat + "/rotation_axis", joint.rotation_axis, kRotationAxes);
                choice(jat + "/limitation", joint.limitation, kLimitations);
                choice(jat + "/limitation_right_axis", joint.limitation_right_axis,
                       kSecondaryDirections);
            }
        }
    }
    return out;
}

std::vector<ModifierProblem> check_stray_modifiers(const world::World& world) {
    const auto& ecs = world.ecs();
    std::set<entt::entity> covered;
    for (const entt::entity entity : ecs.view<const world::MeshInstance3D>()) {
        for (const Modifier& m : modifiers_of(world, entity)) {
            covered.insert(m.entity);
        }
    }
    std::vector<entt::entity> strays;
    const auto add = [&](entt::entity entity) {
        if (!covered.contains(entity) && std::ranges::find(strays, entity) == strays.end()) {
            strays.push_back(entity);
        }
    };
    for (const entt::entity entity : ecs.view<const world::TwoBoneIK3D>()) {
        add(entity);
    }
    for (const entt::entity entity : ecs.view<const world::FABRIK3D>()) {
        add(entity);
    }
    for (const entt::entity entity : ecs.view<const world::CCDIK3D>()) {
        add(entity);
    }
    for (const entt::entity entity : ecs.view<const world::JacobianIK3D>()) {
        add(entity);
    }
    for (const entt::entity entity : ecs.view<const world::PhysicalBoneSimulator3D>()) {
        add(entity);
    }
    // In the order entities were made, so reports read the same each time.
    std::ranges::sort(strays, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    std::vector<ModifierProblem> out;
    for (const entt::entity entity : strays) {
        std::vector<Modifier> own;
        add_modifiers_of(world, entity, own);
        for (const Modifier& m : own) {
            out.push_back(
                {entity, "/components/" + std::string(kKindNames[static_cast<std::size_t>(m.kind)]),
                 "turns bones, but neither its entity nor a parent's draws a model: put "
                 "it on the MeshInstance3D's entity or in its Transform3D children"});
        }
    }
    // Physical bones no simulator holds, which are static bodies that never move.
    std::set<entt::entity> held;
    for (const entt::entity entity : ecs.view<const world::PhysicalBoneSimulator3D>()) {
        for (const PhysicalBone& b : physical_bones(world, entity, nullptr)) {
            held.insert(b.entity);
        }
    }
    std::vector<entt::entity> loose;
    for (const entt::entity entity : ecs.view<const world::PhysicalBone3D>()) {
        if (!held.contains(entity)) {
            loose.push_back(entity);
        }
    }
    std::ranges::sort(loose, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : loose) {
        out.push_back({entity, "/components/PhysicalBone3D",
                       "isn't in the Transform3D children of a PhysicalBoneSimulator3D's entity, "
                       "so it is a static body that never moves: put it there"});
    }
    return out;
}

} // namespace slash::scene3d
