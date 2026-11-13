#include "bone_constraints.hpp"

#include "slash/core/easing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>

// Ports of Godot 4.6's LookAtModifier3D and CopyTransformModifier3D. Each function names the one
// it follows. Godot works in the skeleton's global space; these work in the model's, which only
// differs by the skeleton's own transform, so directions come out the same.
namespace slash::scene3d::detail {
namespace {

using scene3d::normalized;
using world::Affine3D;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTau = 6.28318530717958647692F;
constexpr float kCmpEpsilon = 0.00001F;

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l == 0.0F ? Vec3{} : v * static_cast<real_t>(1.0F / static_cast<float>(l));
}

real_t component(Vec3 v, int axis) {
    if (axis == 0) {
        return v.x;
    }
    return axis == 1 ? v.y : v.z;
}

Vec3 abs_of(Vec3 v) {
    return {std::abs(v.x), std::abs(v.y), std::abs(v.z)};
}

// Math::is_equal_approx and is_zero_approx.
bool is_equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const float tolerance = std::max(kCmpEpsilon * std::abs(a), kCmpEpsilon);
    return std::abs(a - b) < tolerance;
}

bool is_equal_approx(Vec3 a, Vec3 b) {
    return is_equal_approx(static_cast<float>(a.x), static_cast<float>(b.x)) &&
           is_equal_approx(static_cast<float>(a.y), static_cast<float>(b.y)) &&
           is_equal_approx(static_cast<float>(a.z), static_cast<float>(b.z));
}

bool is_zero_approx(float v) {
    return std::abs(v) < kCmpEpsilon;
}

// Math::acos, which clamps.
float acos_of(float x) {
    const float c = std::clamp(x, -1.0F, 1.0F);
    return slash::atan2(std::sqrt((1.0F - c) * (1.0F + c)), c);
}

// Math::fposmod.
float fposmod(float x, float y) {
    float value = std::fmod(x, y);
    if ((value < 0.0F && y > 0.0F) || (value > 0.0F && y < 0.0F)) {
        value += y;
    }
    return value + 0.0F;
}

// Quaternion(axis, angle).
Quat axis_angle(Vec3 axis, float angle) {
    const auto d = static_cast<float>(length(axis));
    if (d == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    const Vec2f cs = cos_sin(angle * 0.5F);
    const float s = cs.y / d;
    return {axis.x * s, axis.y * s, axis.z * s, cs.x};
}

Quat operator*(const Quat& a, const Quat& b) {
    return multiply(a, b);
}

// The vector of an axis: 0 x, 1 y, 2 z.
Vec3 axis_vector(int axis) {
    return {axis == 0 ? 1.0F : 0.0F, axis == 1 ? 1.0F : 0.0F, axis == 2 ? 1.0F : 0.0F};
}

// SkeletonModifier3D::BoneAxis by name: 0 plus_x, 1 minus_x, ... 5 minus_z; -1 for none.
int bone_axis_of(std::string_view name) {
    constexpr std::array<std::string_view, 6> kNames{"plus_x",  "minus_x", "plus_y",
                                                     "minus_y", "plus_z",  "minus_z"};
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int axis_of(std::string_view name) {
    if (name == "x") {
        return 0;
    }
    if (name == "y") {
        return 1;
    }
    return name == "z" ? 2 : -1;
}

// SkeletonModifier3D::get_vector_from_bone_axis.
Vec3 bone_axis_vector(int bone_axis) {
    const Vec3 v = axis_vector(bone_axis / 2);
    return bone_axis % 2 == 0 ? v : v * -1.0F;
}

// SkeletonModifier3D::get_axis_from_bone_axis.
int axis_from_bone_axis(int bone_axis) {
    return bone_axis / 2;
}

// LookAtModifier3D::get_basis_vector_from_bone_axis.
Vec3 basis_vector(const Affine3D& basis, int bone_axis) {
    const int axis = bone_axis / 2;
    Vec3 column = basis.z;
    if (axis == 0) {
        column = basis.x;
    } else if (axis == 1) {
        column = basis.y;
    }
    return bone_axis % 2 == 0 ? column : column * -1.0F;
}

// LookAtModifier3D::get_secondary_rotation_axis.
int secondary_rotation_axis_of(int forward_axis, int primary_rotation_axis) {
    const Vec3 plane = bone_axis_vector(forward_axis) + axis_vector(primary_rotation_axis);
    if (is_zero_approx(static_cast<float>(plane.x))) {
        return 0;
    }
    return is_zero_approx(static_cast<float>(plane.y)) ? 1 : 2;
}

// LookAtModifier3D::get_projection_vector: axes swap between 2D and 3D.
Vec2 projection(Vec3 v, int axis) {
    switch (axis) {
    case 0:
        return {v.z, v.y};
    case 1:
        return {v.x, v.z};
    default:
        return {v.y, v.x};
    }
}

Vec2 normalized(Vec2 v) {
    const real_t l = std::sqrt((v.x * v.x) + (v.y * v.y));
    return l == 0.0F ? Vec2{} : Vec2{v.x / l, v.y / l};
}

// Vector2::angle_to.
float angle_to(Vec2 a, Vec2 b) {
    return slash::atan2(static_cast<float>((a.x * b.y) - (a.y * b.x)),
                        static_cast<float>((a.x * b.x) + (a.y * b.y)));
}

// Basis::xform_inv: the transpose times v.
Vec3 xform_inv(const Affine3D& basis, Vec3 v) {
    return {dot(basis.x, v), dot(basis.y, v), dot(basis.z, v)};
}

// Transform3D::rotated_local: turned about an axis in its own space.
Affine3D rotated_local(const Affine3D& t, Vec3 axis, float angle) {
    const Affine3D turn = world::rotation_basis(axis_angle(axis, angle));
    Affine3D out = t * turn;
    out.origin = t.origin;
    return out;
}

// Transform3D::translated_local.
Vec3 translated_local(const Affine3D& t, Vec3 offset) {
    return t.origin + t.basis(offset);
}

NodePose rest_of(const Model& model, int bone) {
    const ModelNode& node = model.nodes[static_cast<std::size_t>(bone)];
    return {.position = node.position, .rotation = node.rotation, .scale = node.scale};
}

// ---- LookAtModifier3D ----

struct LookAt {
    const world::LookAtModifier3D* m = nullptr;
    int forward_axis = 4;
    int primary_rotation_axis = 1;
    int secondary_rotation_axis = 0;
    Vec3 forward_vector_nrm;

    // LookAtModifier3D::remap_damped and get_bspline_y.
    static double bspline_y(Vec2 from, Vec2 control, Vec2 to, double x) {
        const double a = from.x - (2.0 * control.x) + to.x;
        const double b = (-2.0 * from.x) + (2.0 * control.x);
        const double c = from.x - x;
        double t = 0.0;
        if (std::abs(a) < static_cast<double>(kCmpEpsilon)) {
            t = -c / b; // Almost linear.
        } else {
            const double discriminant = (b * b) - (4.0 * a * c);
            const double root = std::sqrt(discriminant);
            const double e = 1.0 / (2.0 * a);
            const double t1 = (-b + root) * e;
            t = (0.0 <= t1 && t1 <= 1.0) ? t1 : (-b - root) * e;
        }
        const double u = 1.0 - t;
        return (u * u * from.y) + (2.0 * u * t * control.y) + (t * t * to.y);
    }

    static float remap_damped(float from, float to, float damp_threshold, float value) {
        const float sign = std::signbit(value) ? -1.0F : 1.0F;
        const float abs_value = std::abs(value);
        if (is_equal_approx(damp_threshold, 1.0F) || is_zero_approx(to)) {
            return sign * std::clamp(abs_value, from, to); // Avoid division by zero.
        }
        float ratio = (abs_value - from) / (to - from);
        if (ratio <= damp_threshold) {
            return sign * std::clamp(abs_value, from, to);
        }
        const double limit = kPi;
        const double inv_to = 1.0 / to;
        const double end_x = limit * inv_to;
        const double position = abs_value * inv_to;
        ratio = static_cast<float>(bspline_y({damp_threshold, damp_threshold}, {1.0F, 1.0F},
                                             {static_cast<real_t>(end_x), 1.0F}, position));
        return sign * (from + ((to - from) * ratio));
    }

    // The limit and damp threshold of a turn about the primary or the secondary axis.
    [[nodiscard]] std::pair<float, float> limits(bool primary, float angle) const {
        if (m->symmetry_limitation) {
            return primary
                       ? std::pair{m->primary_limit_angle * 0.5F, m->primary_damp_threshold}
                       : std::pair{m->secondary_limit_angle * 0.5F, m->secondary_damp_threshold};
        }
        if (std::signbit(angle)) {
            return primary ? std::pair{m->primary_negative_limit_angle,
                                       m->primary_negative_damp_threshold}
                           : std::pair{m->secondary_negative_limit_angle,
                                       m->secondary_negative_damp_threshold};
        }
        return primary
                   ? std::pair{m->primary_positive_limit_angle, m->primary_positive_damp_threshold}
                   : std::pair{m->secondary_positive_limit_angle,
                               m->secondary_positive_damp_threshold};
    }

    // LookAtModifier3D::look_at_with_axes.
    Affine3D look_at_with_axes(const Affine3D& rest) {
        // Primary rotation by projection to 2D plane by xform_inv and picking elements.
        Vec3 current_vector = normalized(basis_vector(rest, forward_axis));
        Vec2 src_vec2 =
            normalized(projection(xform_inv(rest, forward_vector_nrm), primary_rotation_axis));
        Vec2 dst_vec2 =
            normalized(projection(xform_inv(rest, current_vector), primary_rotation_axis));
        float calculated_angle = angle_to(src_vec2, dst_vec2);
        const Affine3D primary_result =
            rotated_local(rest, axis_vector(primary_rotation_axis), calculated_angle);
        Affine3D current_result = primary_result;
        if (m->use_angle_limitation) {
            const auto [limit_angle, damp_threshold] = limits(true, calculated_angle);
            calculated_angle = remap_damped(0.0F, limit_angle, damp_threshold, calculated_angle);
            current_result =
                rotated_local(rest, axis_vector(primary_rotation_axis), calculated_angle);
        }
        // Needs for detecting flipping even if use_secondary_rotation is false.
        secondary_rotation_axis = secondary_rotation_axis_of(forward_axis, primary_rotation_axis);
        if (!m->use_secondary_rotation) {
            return current_result;
        }
        // Secondary rotation by projection to 2D plane by xform_inv and picking elements.
        current_vector = normalized(basis_vector(primary_result, forward_axis));
        src_vec2 = normalized(
            projection(xform_inv(primary_result, forward_vector_nrm), secondary_rotation_axis));
        dst_vec2 = normalized(
            projection(xform_inv(primary_result, current_vector), secondary_rotation_axis));
        calculated_angle = angle_to(src_vec2, dst_vec2);
        if (m->use_angle_limitation) {
            const auto [limit_angle, damp_threshold] = limits(false, calculated_angle);
            calculated_angle = remap_damped(0.0F, limit_angle, damp_threshold, calculated_angle);
        }
        return rotated_local(current_result, axis_vector(secondary_rotation_axis),
                             calculated_angle);
    }

    // LookAtModifier3D::is_intersecting_axis.
    [[nodiscard]] bool is_intersecting_axis(Vec3 prev, Vec3 current, int flipping_axis,
                                            int check_axis, bool check_plane = false) const {
        // Prevent that the angular velocity does not become too large.
        const auto margin = static_cast<real_t>(m->origin_safe_margin);
        if (check_plane) {
            const Vec2 a = projection(prev, check_axis);
            const Vec2 b = projection(current, check_axis);
            if (std::sqrt((a.x * a.x) + (a.y * a.y)) > margin &&
                std::sqrt((b.x * b.x) + (b.y * b.y)) > margin) {
                return false;
            }
        } else if (std::abs(component(prev, check_axis)) > margin &&
                   std::abs(component(current, check_axis)) > margin) {
            return false;
        }
        return std::signbit(component(prev, flipping_axis)) !=
               std::signbit(component(current, flipping_axis));
    }
};

// Animation::interpolate_via_rest for rotations.
Quat interpolate_via_rest(const Quat& from, const Quat& to, float weight, const Quat& rest) {
    return normalized(from * slerp(Quat{}, inverse(rest) * to, weight));
}

// ---- CopyTransformModifier3D ----

// SkeletonModifier3D::symmetrize_angle and get_roll_angle.
float symmetrize_angle(float angle) {
    const float a = fposmod(angle, kTau);
    return a > kPi ? a - kTau : a;
}

float roll_angle(const Quat& rotation, Vec3 axis) {
    const Vec3 roll_axis = normalized(axis);
    // Project the quaternion rotation onto the roll axis.
    const real_t d =
        (rotation.x * roll_axis.x) + (rotation.y * roll_axis.y) + (rotation.z * roll_axis.z);
    Quat roll{roll_axis.x * d, roll_axis.y * d, roll_axis.z * d, rotation.w};
    const real_t l =
        std::sqrt((roll.x * roll.x) + (roll.y * roll.y) + (roll.z * roll.z) + (roll.w * roll.w));
    if (l <= kCmpEpsilon) {
        return 0.0F;
    }
    roll = {roll.x / l, roll.y / l, roll.z / l, roll.w / l};
    const float angle = 2.0F * acos_of(static_cast<float>(roll.w));
    const float direction =
        (roll.x * roll_axis.x) + (roll.y * roll_axis.y) + (roll.z * roll_axis.z) > 0.0F ? 1.0F
                                                                                        : -1.0F;
    return symmetrize_angle(angle * direction);
}

real_t& at(Vec3& v, int axis) {
    if (axis == 0) {
        return v.x;
    }
    return axis == 1 ? v.y : v.z;
}

real_t& at(Quat& q, int axis) {
    if (axis == 0) {
        return q.x;
    }
    return axis == 1 ? q.y : q.z;
}

// Transform3D::interpolate_with, on poses.
NodePose interpolate(const NodePose& from, const NodePose& to, float weight) {
    const auto w = static_cast<real_t>(weight);
    return {.position = from.position + ((to.position - from.position) * w),
            .rotation =
                normalized(slerp(normalized(from.rotation), normalized(to.rotation), weight)),
            .scale = from.scale + ((to.scale - from.scale) * w)};
}

Vec3 divided(Vec3 a, Vec3 b) {
    return {a.x / b.x, a.y / b.y, a.z / b.z};
}

Vec3 times(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// CopyTransformModifier3D::_process_copy.
void process_copy(const world::CopyTransformSetting& s, const Model& model,
                  std::vector<NodePose>& pose, int apply_bone, const NodePose& destination,
                  float amount, bool relative) {
    Vec3 dest_pos = destination.position;
    Quat dest_rot = normalized(destination.rotation);
    Vec3 dest_scl = destination.scale;
    const std::array<bool, 3> axes{s.axis_x_enabled, s.axis_y_enabled, s.axis_z_enabled};
    const std::array<bool, 3> inverts{s.axis_x_inverted, s.axis_y_inverted, s.axis_z_inverted};

    // Mask pos and scale.
    for (int i = 0; i < 3; ++i) {
        if (!axes[static_cast<std::size_t>(i)]) {
            at(dest_pos, i) = 0.0F;
            at(dest_scl, i) = 1.0F;
        }
    }

    // Mask rot.
    const int flags = (axes[0] ? 1 : 0) | (axes[1] ? 2 : 0) | (axes[2] ? 4 : 0);
    const auto roll_only = [&](int axis) {
        const Vec3 v = axis_vector(axis);
        dest_rot = axis_angle(v, roll_angle(dest_rot, v));
    };
    const auto without_roll = [&](int axis) {
        const Vec3 v = axis_vector(axis);
        dest_rot = dest_rot * inverse(axis_angle(v, roll_angle(dest_rot, v)));
    };
    switch (flags) {
    case 0:
        dest_rot = Quat{};
        break;
    case 1:
        roll_only(0);
        break;
    case 2:
        roll_only(1);
        break;
    case 4:
        roll_only(2);
        break;
    case 1 | 2:
        without_roll(2);
        break;
    case 2 | 4:
        without_roll(0);
        break;
    case 4 | 1:
        without_roll(1);
        break;
    default:
        break;
    }

    // Process inversion.
    for (int i = 0; i < 3; ++i) {
        if (axes[static_cast<std::size_t>(i)] && inverts[static_cast<std::size_t>(i)]) {
            at(dest_pos, i) *= -1.0F;
            at(dest_rot, i) *= -1.0F;
            at(dest_scl, i) = 1.0F / at(dest_scl, i);
        }
    }
    dest_rot = normalized(dest_rot);

    const auto index = static_cast<std::size_t>(apply_bone);
    NodePose result;
    if (s.additive) {
        result = {.position = pose[index].position + dest_pos,
                  .rotation = pose[index].rotation * dest_rot,
                  .scale = times(pose[index].scale, dest_scl)};
    } else if (relative) {
        const NodePose rest = rest_of(model, apply_bone);
        result = {.position = rest.position + dest_pos,
                  .rotation = normalized(rest.rotation) * dest_rot,
                  .scale = times(rest.scale, dest_scl)};
    } else {
        result = {.position = dest_pos, .rotation = dest_rot, .scale = dest_scl};
    }

    // Process interpolation depends on the amount.
    result = interpolate(pose[index], result, amount);

    // Apply transform depends on the element mask.
    if (s.copy_position) {
        pose[index].position = result.position;
    }
    if (s.copy_rotation) {
        pose[index].rotation = result.rotation;
    }
    if (s.copy_scale) {
        pose[index].scale = result.scale;
    }
}

} // namespace

void run_look_at(const world::LookAtModifier3D& modifier, const Model& model,
                 std::vector<NodePose>& pose, const LookAtPlaces& places, LookAtState& state,
                 double delta) {
    const int bone = find_node(model, modifier.bone);
    const int forward_axis = bone_axis_of(modifier.forward_axis);
    const int primary_axis = axis_of(modifier.primary_rotation_axis);
    if (bone < 0 || forward_axis < 0 || primary_axis < 0) {
        return;
    }
    const auto index = static_cast<std::size_t>(bone);
    LookAt look{.m = &modifier,
                .forward_axis = forward_axis,
                .primary_rotation_axis = primary_axis,
                .secondary_rotation_axis = secondary_rotation_axis_of(forward_axis, primary_axis),
                .forward_vector_nrm = {}};

    // Calculate bone rest space in the world.
    const std::vector<Affine3D> globals = node_globals(model, pose);
    const NodePose rest_pose = modifier.relative ? pose[index] : rest_of(model, bone);
    const Affine3D bone_rest = node_transform(rest_pose);
    const int parent_bone = model.nodes[index].parent;
    Affine3D bone_rest_space =
        parent_bone < 0 ? Affine3D{} : globals[static_cast<std::size_t>(parent_bone)];
    bone_rest_space.origin = translated_local(bone_rest_space, bone_rest.origin);

    // Calculate forward_vector and destination.
    const Vec3 prev_forward_vector = state.forward_vector;
    Quat destination;
    if (!places.target) {
        destination = pose[index].rotation;
    } else {
        Affine3D origin_tr = bone_rest_space;
        const int origin_bone = find_node(model, modifier.origin_bone);
        if (modifier.origin_from == "specific_bone" && origin_bone >= 0) {
            origin_tr = globals[static_cast<std::size_t>(origin_bone)];
        } else if (modifier.origin_from == "external_node" && places.origin_external) {
            origin_tr.origin = *places.origin_external;
        }
        state.forward_vector =
            xform_inv(world::orthonormalized(bone_rest_space),
                      *places.target - translated_local(origin_tr, modifier.origin_offset));
        state.forward_vector_nrm = normalized(state.forward_vector);
        look.forward_vector_nrm = state.forward_vector_nrm;
        if (is_equal_approx(abs_of(look.forward_vector_nrm), axis_vector(primary_axis))) {
            destination = pose[index].rotation;
            // The zero-vector to be used for checking in the line immediately below to avoid
            // animation glitch.
            state.forward_vector = {};
        } else {
            destination = decompose(look.look_at_with_axes(bone_rest)).rotation;
        }
    }
    const Vec3 forward_vector = state.forward_vector;

    // Detect flipping.
    const bool is_not_max_influence = modifier.influence < 1.0F;
    const bool is_flippable = modifier.use_angle_limitation || is_not_max_influence;
    const int current_forward_axis = axis_from_bone_axis(forward_axis);
    const int secondary = look.secondary_rotation_axis;
    const Vec3 zero{};
    bool transition = false;
    if (look.is_intersecting_axis(prev_forward_vector, forward_vector, current_forward_axis,
                                  secondary) ||
        look.is_intersecting_axis(prev_forward_vector, forward_vector, primary_axis, primary_axis,
                                  true) ||
        look.is_intersecting_axis(prev_forward_vector, forward_vector, secondary,
                                  current_forward_axis) ||
        (prev_forward_vector != zero && forward_vector == zero) ||
        (prev_forward_vector == zero && forward_vector != zero)) {
        transition = true;
    } else if (is_flippable && std::signbit(component(prev_forward_vector, secondary)) !=
                                   std::signbit(component(forward_vector, secondary))) {
        // Flipping by angle_limitation can be detected by sign of secondary rotation axes during
        // forward_vector is rotated more than 90 degree from forward_axis.
        const Vec3 prev_forward_vector_nrm = normalized(forward_vector);
        const Vec3 rest_forward_vector = bone_axis_vector(forward_axis);
        const bool full = modifier.symmetry_limitation
                              ? is_equal_approx(modifier.primary_limit_angle, kTau)
                              : is_equal_approx(modifier.primary_positive_limit_angle +
                                                    modifier.primary_negative_limit_angle,
                                                kTau);
        if ((is_not_max_influence || !full) &&
            dot(prev_forward_vector_nrm, rest_forward_vector) < 0 &&
            dot(state.forward_vector_nrm, rest_forward_vector) < 0) {
            transition = true;
        }
    }
    // LookAtModifier3D::init_transition.
    if (transition && !is_zero_approx(modifier.duration)) {
        state.from_q = state.prev_q;
        state.remaining = 1.0F;
    }

    // Do time-based interpolation.
    if (state.remaining > 0.0F) {
        const float time_step = modifier.duration > 0.0F ? 1.0F / modifier.duration : 1.0F;
        state.remaining = std::max(0.0F, state.remaining - (time_step * static_cast<float>(delta)));
        Transition kind = Transition::Linear;
        Ease ease = Ease::In;
        (void)parse_transition(modifier.transition_type, kind);
        (void)parse_ease(modifier.ease_type, ease);
        const float weight = run_equation(kind, ease, 1.0F - state.remaining, 0.0F, 1.0F, 1.0F);
        if (is_flippable) {
            // Interpolate through the rest same as AnimationTree blending for preventing to
            // penetrate the bone into the body.
            const Quat rest = normalized(model.nodes[index].rotation);
            destination =
                interpolate_via_rest(interpolate_via_rest(rest, state.from_q, 1.0F - weight, rest),
                                     destination, weight, rest);
        } else {
            destination = slerp(state.from_q, destination, weight);
        }
    }

    pose[index].rotation = destination;
    state.prev_q = destination;
}

void run_copy_transform(const world::CopyTransformModifier3D& modifier, const Model& model,
                        std::vector<NodePose>& pose, const NodePlace& node_place) {
    for (const world::CopyTransformSetting& s : modifier.settings) {
        const float amount = s.amount;
        if (amount <= 0.0F) {
            continue;
        }
        const int apply_bone = find_node(model, s.apply_bone);
        if (apply_bone < 0) {
            continue;
        }
        if (s.reference_type != "node") {
            // CopyTransformModifier3D::_process_constraint_by_bone.
            const int reference_bone = find_node(model, s.reference_bone);
            if (reference_bone < 0) {
                continue;
            }
            NodePose destination = pose[static_cast<std::size_t>(reference_bone)];
            if (s.relative) {
                const NodePose rest = rest_of(model, reference_bone);
                destination.scale = divided(destination.scale, rest.scale);
                destination.rotation =
                    inverse(normalized(rest.rotation)) * normalized(destination.rotation);
                destination.position = destination.position - rest.position;
            }
            process_copy(s, model, pose, apply_bone, destination, amount, s.relative);
            continue;
        }
        // CopyTransformModifier3D::_process_constraint_by_node.
        if (s.reference_node == entt::null) {
            continue;
        }
        const std::optional<Affine3D> node = node_place(s.reference_node);
        if (!node) {
            continue;
        }
        Affine3D skel_tr;
        if (const int parent = model.nodes[static_cast<std::size_t>(apply_bone)].parent;
            parent >= 0) {
            skel_tr = node_globals(model, pose)[static_cast<std::size_t>(parent)];
        }
        process_copy(s, model, pose, apply_bone, decompose(skel_tr.inverse() * *node), amount,
                     false);
    }
}

} // namespace slash::scene3d::detail
