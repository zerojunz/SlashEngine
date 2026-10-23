#include "slash/world/path.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace slash::world {
namespace {

constexpr float kCmpEpsilon = 0.00001F; // Godot's CMP_EPSILON
constexpr float kUnitEpsilon = 0.001F;  // Godot's UNIT_EPSILON
constexpr int kBakeStages = 10;         // the most times _bake halves a segment

// ---- Godot's float helpers (core/math/math_funcs.h) ----

bool is_zero_approx(float v) {
    return std::abs(v) < kCmpEpsilon;
}

bool is_equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const float tolerance = std::max(kCmpEpsilon * std::abs(a), kCmpEpsilon);
    return std::abs(a - b) < tolerance;
}

float lerp(float a, float b, float weight) {
    return a + ((b - a) * weight);
}

float bezier(float start, float control_1, float control_2, float end, float t) {
    const float omt = 1.0F - t;
    const float omt2 = omt * omt;
    const float omt3 = omt2 * omt;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (start * omt3) + (control_1 * omt2 * t * 3.0F) + (control_2 * omt * t2 * 3.0F) +
           (end * t3);
}

float bezier_derivative(float start, float control_1, float control_2, float end, float t) {
    const float omt = 1.0F - t;
    const float omt2 = omt * omt;
    const float t2 = t * t;
    return ((control_1 - start) * 3.0F * omt2) + ((control_2 - control_1) * 6.0F * omt * t) +
           ((end - control_2) * 3.0F * t2);
}

float cubic(float from, float to, float pre, float post, float weight) {
    return 0.5F * ((from * 2.0F) + ((-pre + to) * weight) +
                   (((2.0F * pre) - (5.0F * from) + (4.0F * to) - post) * (weight * weight)) +
                   ((-pre + (3.0F * from) - (3.0F * to) + post) * (weight * weight * weight)));
}

// acos through atan2, which is the same everywhere.
float acos(float v) {
    return slash::atan2(std::sqrt(std::max(0.0F, 1.0F - (v * v))), v);
}

// ---- Vector2 ----

float dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}
float cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}
float length(Vec2 v) {
    return std::sqrt(dot(v, v));
}

Vec2 normalized(Vec2 v) {
    const float l = dot(v, v);
    if (l == 0.0F) {
        return {};
    }
    const float s = std::sqrt(l);
    return {v.x / s, v.y / s};
}

bool is_equal_approx(Vec2 a, Vec2 b) {
    return is_equal_approx(a.x, b.x) && is_equal_approx(a.y, b.y);
}

Vec2 lerp(Vec2 a, Vec2 b, float weight) {
    return {lerp(a.x, b.x, weight), lerp(a.y, b.y, weight)};
}

Vec2 bezier(Vec2 start, Vec2 control_1, Vec2 control_2, Vec2 end, float t) {
    return {bezier(start.x, control_1.x, control_2.x, end.x, t),
            bezier(start.y, control_1.y, control_2.y, end.y, t)};
}

Vec2 bezier_derivative(Vec2 start, Vec2 control_1, Vec2 control_2, Vec2 end, float t) {
    return {bezier_derivative(start.x, control_1.x, control_2.x, end.x, t),
            bezier_derivative(start.y, control_1.y, control_2.y, end.y, t)};
}

Vec2 cubic(Vec2 from, Vec2 to, Vec2 pre, Vec2 post, float weight) {
    return {cubic(from.x, to.x, pre.x, post.x, weight), cubic(from.y, to.y, pre.y, post.y, weight)};
}

Vec2 rotated(Vec2 v, float angle) {
    const Vec2 cs = cos_sin(angle);
    return {(v.x * cs.x) - (v.y * cs.y), (v.x * cs.y) + (v.y * cs.x)};
}

// Vector2::slerp.
Vec2 slerp(Vec2 from, Vec2 to, float weight) {
    const float from_sq = dot(from, from);
    const float to_sq = dot(to, to);
    if (from_sq == 0.0F || to_sq == 0.0F) {
        return lerp(from, to, weight);
    }
    const float from_length = std::sqrt(from_sq);
    const float result_length = lerp(from_length, std::sqrt(to_sq), weight);
    const float angle = slash::atan2(cross(from, to), dot(from, to));
    return rotated(from, angle * weight) * (result_length / from_length);
}

// ---- Vector3 ----

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
    const float l = dot(v, v);
    if (l == 0.0F) {
        return {};
    }
    const float s = std::sqrt(l);
    return {v.x / s, v.y / s, v.z / s};
}

bool is_equal_approx(Vec3 a, Vec3 b) {
    return is_equal_approx(a.x, b.x) && is_equal_approx(a.y, b.y) && is_equal_approx(a.z, b.z);
}

Vec3 lerp(Vec3 a, Vec3 b, float weight) {
    return {lerp(a.x, b.x, weight), lerp(a.y, b.y, weight), lerp(a.z, b.z, weight)};
}

Vec3 bezier(Vec3 start, Vec3 control_1, Vec3 control_2, Vec3 end, float t) {
    return {bezier(start.x, control_1.x, control_2.x, end.x, t),
            bezier(start.y, control_1.y, control_2.y, end.y, t),
            bezier(start.z, control_1.z, control_2.z, end.z, t)};
}

Vec3 bezier_derivative(Vec3 start, Vec3 control_1, Vec3 control_2, Vec3 end, float t) {
    return {bezier_derivative(start.x, control_1.x, control_2.x, end.x, t),
            bezier_derivative(start.y, control_1.y, control_2.y, end.y, t),
            bezier_derivative(start.z, control_1.z, control_2.z, end.z, t)};
}

Vec3 cubic(Vec3 from, Vec3 to, Vec3 pre, Vec3 post, float weight) {
    return {cubic(from.x, to.x, pre.x, post.x, weight), cubic(from.y, to.y, pre.y, post.y, weight),
            cubic(from.z, to.z, pre.z, post.z, weight)};
}

// ---- Basis, as an Affine3D whose origin is left at zero ----

// Basis(axis, angle): Basis::set_axis_angle.
Affine3D axis_angle(Vec3 axis, float angle) {
    const Vec3 sq{axis.x * axis.x, axis.y * axis.y, axis.z * axis.z};
    const Vec2 cs = cos_sin(angle);
    const float cosine = cs.x;
    const float sine = cs.y;
    const float t = 1.0F - cosine;
    // rows[r][c] is column c's row r.
    Affine3D b;
    b.x.x = sq.x + (cosine * (1.0F - sq.x));
    b.y.y = sq.y + (cosine * (1.0F - sq.y));
    b.z.z = sq.z + (cosine * (1.0F - sq.z));
    float xyzt = axis.x * axis.y * t;
    float zyxs = axis.z * sine;
    b.y.x = xyzt - zyxs; // rows[0][1]
    b.x.y = xyzt + zyxs; // rows[1][0]
    xyzt = axis.x * axis.z * t;
    zyxs = axis.y * sine;
    b.z.x = xyzt + zyxs; // rows[0][2]
    b.x.z = xyzt - zyxs; // rows[2][0]
    xyzt = axis.y * axis.z * t;
    zyxs = axis.x * sine;
    b.z.y = xyzt - zyxs; // rows[1][2]
    b.y.z = xyzt + zyxs; // rows[2][1]
    return b;
}

// Basis::get_quaternion.
Quat quaternion_of(const Affine3D& b) {
    // m[r][c] is column c's row r.
    const std::array<std::array<float, 3>, 3> m{
        {{b.x.x, b.y.x, b.z.x}, {b.x.y, b.y.y, b.z.y}, {b.x.z, b.y.z, b.z.z}}};
    const float trace = m[0][0] + m[1][1] + m[2][2];
    std::array<float, 4> temp{};
    if (trace > 0.0F) {
        float s = std::sqrt(trace + 1.0F);
        temp[3] = s * 0.5F;
        s = 0.5F / s;
        temp[0] = (m[2][1] - m[1][2]) * s;
        temp[1] = (m[0][2] - m[2][0]) * s;
        temp[2] = (m[1][0] - m[0][1]) * s;
    } else {
        // The largest diagonal entry, the first of equals as Godot picks it.
        std::size_t i = 0;
        if (m[0][0] < m[1][1]) {
            i = m[1][1] < m[2][2] ? 2 : 1;
        } else if (m[0][0] < m[2][2]) {
            i = 2;
        }
        const std::size_t j = (i + 1) % 3;
        const std::size_t k = (i + 2) % 3;
        float s = std::sqrt(m[i][i] - m[j][j] - m[k][k] + 1.0F);
        temp[i] = s * 0.5F;
        s = 0.5F / s;
        temp[3] = (m[k][j] - m[j][k]) * s;
        temp[j] = (m[j][i] + m[i][j]) * s;
        temp[k] = (m[k][i] + m[i][k]) * s;
    }
    return {temp[0], temp[1], temp[2], temp[3]};
}

// Quaternion::slerp, with acos and sin that are the same everywhere.
Quat slerp(const Quat& from, const Quat& to, float weight) {
    float cosom = (from.x * to.x) + (from.y * to.y) + (from.z * to.z) + (from.w * to.w);
    Quat target = to;
    if (cosom < 0.0F) {
        cosom = -cosom;
        target = {-to.x, -to.y, -to.z, -to.w};
    }
    float scale0 = 1.0F - weight;
    float scale1 = weight;
    if (1.0F - cosom > kCmpEpsilon) {
        const float omega = acos(cosom);
        const float sinom = cos_sin(omega).y;
        scale0 = cos_sin((1.0F - weight) * omega).y / sinom;
        scale1 = cos_sin(weight * omega).y / sinom;
    }
    return {(scale0 * from.x) + (scale1 * target.x), (scale0 * from.y) + (scale1 * target.y),
            (scale0 * from.z) + (scale1 * target.z), (scale0 * from.w) + (scale1 * target.w)};
}

Vec3 row(const Affine3D& b, int r) {
    switch (r) {
    case 0:
        return {b.x.x, b.y.x, b.z.x};
    case 1:
        return {b.x.y, b.y.y, b.z.y};
    default:
        return {b.x.z, b.y.z, b.z.z};
    }
}

// Basis::slerp: the rotations slerped, each row scaled by its lengths blended.
Affine3D slerp(const Affine3D& from, const Affine3D& to, float weight) {
    Affine3D b = rotation_basis(slerp(quaternion_of(from), quaternion_of(to), weight));
    const float s0 = lerp(length(row(from, 0)), length(row(to, 0)), weight);
    const float s1 = lerp(length(row(from, 1)), length(row(to, 1)), weight);
    const float s2 = lerp(length(row(from, 2)), length(row(to, 2)), weight);
    for (Vec3* column : {&b.x, &b.y, &b.z}) {
        column->x *= s0;
        column->y *= s1;
        column->z *= s2;
    }
    return b;
}

// The angle of Quaternion(from, to), the shortest arc between two directions:
// Quaternion::get_angle.
float arc_angle(Vec3 from, Vec3 to) {
    constexpr float kAlmostOne = 0.99999975F;
    const Vec3 n0 = normalized(from);
    const Vec3 n1 = normalized(to);
    const float d = dot(n0, n1);
    float w = 1.0F;
    if (std::abs(d) > kAlmostOne) {
        w = d >= 0.0F ? 1.0F : 0.0F;
    } else {
        w = std::sqrt((1.0F + d) * 2.0F) * 0.5F;
    }
    return 2.0F * acos(w);
}

float sign(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

template <typename V>
V tangent(V begin, V control_1, V control_2, V end, float t) {
    // Curve2D::_calculate_tangent and Curve3D's, corner cases first.
    if (is_zero_approx(t - 0.0F)) {
        if (is_equal_approx(control_1, begin)) {
            return is_equal_approx(control_1, control_2) ? normalized(end - begin)
                                                         : normalized(control_2 - begin);
        }
    } else if (is_zero_approx(t - 1.0F)) {
        if (is_equal_approx(control_2, end)) {
            return is_equal_approx(control_2, control_1) ? normalized(end - begin)
                                                         : normalized(end - control_1);
        }
    }
    if (is_equal_approx(control_1, end) && is_equal_approx(control_2, begin)) {
        return normalized(end - begin);
    }
    return normalized(bezier_derivative(begin, control_1, control_2, end, t));
}

// _bake_segment2d_even_length and _bake_segment3d_even_length: the segment from a to b halved
// until its pieces are no longer than length.
template <typename V>
void bake_segment(std::map<float, V>& bake, float begin, float end, V a, V out, V b, V in,
                  int depth, float length) {
    const V from = bezier(a, a + out, b + in, b, begin);
    const V to = bezier(a, a + out, b + in, b, end);
    if (world::length(to - from) > length && depth < kBakeStages) {
        const float middle = (begin + end) * 0.5F;
        bake[middle] = bezier(a, a + out, b + in, b, middle);
        bake_segment(bake, begin, middle, a, out, b, in, depth + 1, length);
        bake_segment(bake, middle, end, a, out, b, in, depth + 1, length);
    }
}

template <typename V>
float distance(V a, V b) {
    return world::length(b - a);
}

// _find_interval: the baked points an offset falls between, and how far between.
template <typename Interval>
Interval find_interval(const std::vector<float>& distances, float offset) {
    const int count = static_cast<int>(distances.size());
    int start = 0;
    int end = count;
    int index = (end + start) / 2;
    while (start < index) {
        if (offset <= distances[static_cast<std::size_t>(index)]) {
            end = index;
        } else {
            start = index;
        }
        index = (end + start) / 2;
    }
    const float begin = distances[static_cast<std::size_t>(index)];
    const float span = distances[static_cast<std::size_t>(index) + 1] - begin;
    Interval interval{.index = index, .fraction = 0.5F};
    if (span >= FLT_EPSILON) {
        interval.fraction = (offset - begin) / span;
    }
    return interval;
}

template <typename V>
V sample_points(const std::vector<V>& points, int index, float fraction, bool cubic_interp) {
    const auto i = static_cast<std::size_t>(index);
    if (cubic_interp) {
        const V pre = i > 0 ? points[i - 1] : points[i];
        const V post = i + 2 < points.size() ? points[i + 2] : points[i + 1];
        return cubic(points[i], points[i + 1], pre, post, fraction);
    }
    return lerp(points[i], points[i + 1], fraction);
}

// get_closest_point and get_closest_offset: the nearest point of each piece, the first of the
// nearest.
template <typename V>
std::pair<V, float> closest(const std::vector<V>& points, const std::vector<float>& distances,
                            V to) {
    if (points.empty()) {
        return {V{}, 0.0F};
    }
    if (points.size() == 1) {
        return {points[0], 0.0F};
    }
    V nearest{};
    float nearest_offset = 0.0F;
    float nearest_distance = -1.0F;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const float span = distances[i + 1] - distances[i];
        const V origin = points[i];
        const V direction = (points[i + 1] - origin) * (1.0F / span);
        const float d = std::clamp(dot(to - origin, direction), 0.0F, span);
        const V projected = origin + (direction * d);
        const V gap = projected - to;
        const float squared = dot(gap, gap);
        if (nearest_distance < 0.0F || squared < nearest_distance) {
            nearest = projected;
            nearest_offset = distances[i] + d;
            nearest_distance = squared;
        }
    }
    return {nearest, nearest_offset};
}

} // namespace

// ---- Curve2D ----

Curve2D::Curve2D(const std::vector<CurvePoint2D>& points, float bake_interval) {
    if (points.empty()) {
        return;
    }
    if (points.size() == 1) {
        points_ = {points[0].position};
        distances_ = {0.0F};
        forward_ = {{0.0F, 0.1F}};
        return;
    }
    const auto segment_tangent = [&](std::size_t i, float t) {
        return tangent(points[i].position, points[i].position + points[i].out,
                       points[i + 1].position + points[i + 1].in, points[i + 1].position, t);
    };
    points_.push_back(points[0].position);
    forward_.push_back(segment_tangent(0, 0.0F));
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        std::map<float, Vec2> middles;
        bake_segment(middles, 0.0F, 1.0F, points[i].position, points[i].out, points[i + 1].position,
                     points[i + 1].in, 0, bake_interval);
        for (const auto& [t, point] : middles) {
            points_.push_back(point);
            forward_.push_back(segment_tangent(i, t));
        }
        points_.push_back(points[i + 1].position);
        forward_.push_back(segment_tangent(i, 1.0F));
    }
    distances_.assign(points_.size(), 0.0F);
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        distances_[i + 1] = distances_[i] + distance(points_[i], points_[i + 1]);
    }
    length_ = distances_.back();
}

Curve2D::Interval Curve2D::find(float offset) const {
    return find_interval<Interval>(distances_, std::clamp(offset, 0.0F, length_));
}

Vec2 Curve2D::sample(float offset, bool cubic) const {
    if (points_.empty() || !std::isfinite(offset)) {
        return {};
    }
    if (points_.size() == 1) {
        return points_[0];
    }
    const Interval interval = find(offset);
    return sample_points(points_, interval.index, interval.fraction, cubic);
}

Affine2D Curve2D::sample_with_rotation(float offset, bool cubic) const {
    if (points_.empty() || !std::isfinite(offset)) {
        return {};
    }
    if (points_.size() == 1) {
        return {.x = {1.0F, 0.0F}, .y = {0.0F, 1.0F}, .origin = points_[0]};
    }
    const Interval interval = find(offset);
    const auto i = static_cast<std::size_t>(interval.index);
    // _sample_posture: the directions at both ends of the interval, slerped.
    const Vec2 forward = normalized(slerp(forward_[i], forward_[i + 1], interval.fraction));
    return {.x = forward,
            .y = {-forward.y, forward.x},
            .origin = sample_points(points_, interval.index, interval.fraction, cubic)};
}

Vec2 Curve2D::closest_point(Vec2 to) const {
    return closest(points_, distances_, to).first;
}

float Curve2D::closest_offset(Vec2 to) const {
    return closest(points_, distances_, to).second;
}

// ---- Curve3D ----

Curve3D::Curve3D(const std::vector<CurvePoint3D>& points, bool closed, float bake_interval,
                 bool up_vector_enabled)
    : up_vector_enabled_(up_vector_enabled) {
    if (points.empty()) {
        return;
    }
    if (points.size() == 1) {
        points_ = {points[0].position};
        tilts_ = {points[0].tilt};
        distances_ = {0.0F};
        forward_ = {{0.0F, 0.0F, 1.0F}};
        if (up_vector_enabled) {
            up_ = {{0.0F, 1.0F, 0.0F}};
        }
        return;
    }
    // Step 1: the curve cut into pieces of about even length, with tilts and tangents.
    const std::size_t intervals = closed ? points.size() : points.size() - 1;
    const auto next = [&](std::size_t i) { return i + 1 < points.size() ? i + 1 : 0; };
    const auto segment_tangent = [&](std::size_t i, float t) {
        const CurvePoint3D& a = points[i];
        const CurvePoint3D& b = points[next(i)];
        return tangent(a.position, a.position + a.out, b.position + b.in, b.position, t);
    };
    points_.push_back(points[0].position);
    forward_.push_back(tangent(points[0].position, points[0].position + points[0].out,
                               points[1].position + points[1].in, points[1].position, 0.0F));
    tilts_.push_back(points[0].tilt);
    for (std::size_t i = 0; i < intervals; ++i) {
        const CurvePoint3D& a = points[i];
        const CurvePoint3D& b = points[next(i)];
        std::map<float, Vec3> middles;
        bake_segment(middles, 0.0F, 1.0F, a.position, a.out, b.position, b.in, 0, bake_interval);
        for (const auto& [t, point] : middles) {
            points_.push_back(point);
            forward_.push_back(segment_tangent(i, t));
            tilts_.push_back(lerp(a.tilt, b.tilt, t));
        }
        points_.push_back(b.position);
        forward_.push_back(segment_tangent(i, 1.0F));
        tilts_.push_back(b.tilt);
    }
    distances_.assign(points_.size(), 0.0F);
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        distances_[i + 1] = distances_[i] + distance(points_[i], points_[i + 1]);
    }
    length_ = distances_.back();
    if (!up_vector_enabled) {
        return;
    }

    // Step 2: up vectors by the parallel transport frame (Dougan, "The parallel transport
    // frame", Game Programming Gems 2), starting from y up.
    const std::size_t count = points_.size();
    up_.resize(count);
    Affine3D previous;
    if (std::abs(dot(forward_[0], {0.0F, 1.0F, 0.0F})) > 1.0F - kUnitEpsilon) {
        previous = looking_at(forward_[0], {1.0F, 0.0F, 0.0F});
    } else {
        previous = looking_at(forward_[0]);
    }
    up_[0] = previous.y;
    for (std::size_t i = 1; i < count; ++i) {
        // Basis::rotate_to_align from the last frame's forward to this one's.
        const Vec3 from = previous.z * -1.0F;
        const Vec3 to = forward_[i];
        Affine3D frame = previous;
        const Vec3 axis = normalized(cross(from, to));
        if (dot(axis, axis) != 0.0F) {
            const float angle = acos(std::clamp(dot(from, to), -1.0F, 1.0F));
            frame = axis_angle(axis, angle) * previous;
        }
        frame = orthonormalized(frame);
        up_[i] = frame.y;
        previous = frame;
    }

    // A loop, whose ends meet going the same way, has its up vectors twisted to meet too.
    const bool loop = is_equal_approx(points_[0], points_[count - 1]) &&
                      dot(forward_[0], forward_[count - 1]) >= 1.0F - kUnitEpsilon;
    if (!loop) {
        return;
    }
    const Vec3 up_start = up_[0];
    const Vec3 up_end = up_[count - 1];
    const float twist_sign = sign(dot(cross(up_end, up_start), forward_[0]));
    const float full_angle = arc_angle(up_end, up_start);
    if (std::abs(full_angle) < kCmpEpsilon) {
        return;
    }
    for (std::size_t i = 1; i < count; ++i) {
        const float angle = lerp(0.0F, full_angle, distances_[i] / length_);
        up_[i] = axis_angle(forward_[i] * twist_sign, angle).basis(up_[i]);
    }
}

Curve3D::Interval Curve3D::find(float offset) const {
    return find_interval<Interval>(distances_, std::clamp(offset, 0.0F, length_));
}

Vec3 Curve3D::sample(float offset, bool cubic) const {
    if (points_.empty() || !std::isfinite(offset)) {
        return {};
    }
    if (points_.size() == 1) {
        return points_[0];
    }
    const Interval interval = find(offset);
    return sample_points(points_, interval.index, interval.fraction, cubic);
}

Affine3D Curve3D::posture(Interval interval, bool apply_tilt) const {
    const auto i = static_cast<std::size_t>(interval.index);
    // _compose_posture at both ends, slerped.
    const auto compose = [&](std::size_t at) {
        return looking_at(forward_[at], up_vector_enabled_ ? up_[at] : Vec3{0.0F, 1.0F, 0.0F});
    };
    const Affine3D frame = orthonormalized(slerp(compose(i), compose(i + 1), interval.fraction));
    if (!apply_tilt) {
        return frame;
    }
    const float tilt = lerp(tilts_[i], tilts_[i + 1], interval.fraction);
    return axis_angle(frame.z * -1.0F, tilt) * frame;
}

Affine3D Curve3D::sample_with_rotation(float offset, bool cubic, bool apply_tilt) const {
    if (points_.empty() || !std::isfinite(offset)) {
        return {};
    }
    if (points_.size() == 1) {
        Affine3D only;
        only.origin = points_[0];
        return only;
    }
    const Interval interval = find(offset);
    Affine3D t = posture(interval, apply_tilt);
    t.origin = sample_points(points_, interval.index, interval.fraction, cubic);
    return t;
}

float Curve3D::sample_tilt(float offset) const {
    if (tilts_.empty() || !std::isfinite(offset)) {
        return 0.0F;
    }
    if (tilts_.size() == 1) {
        return tilts_[0];
    }
    const Interval interval = find(offset);
    const auto i = static_cast<std::size_t>(interval.index);
    return lerp(tilts_[i], tilts_[i + 1], interval.fraction);
}

Vec3 Curve3D::sample_up_vector(float offset, bool apply_tilt) const {
    if (!up_vector_enabled_ || up_.empty() || !std::isfinite(offset)) {
        return {0.0F, 1.0F, 0.0F};
    }
    if (up_.size() == 1) {
        return up_[0];
    }
    return posture(find(offset), apply_tilt).y;
}

Vec3 Curve3D::closest_point(Vec3 to) const {
    return closest(points_, distances_, to).first;
}

float Curve3D::closest_offset(Vec3 to) const {
    return closest(points_, distances_, to).second;
}

// ---- Followers ----

float wrap_progress(float progress, float length, bool loop) {
    if (loop && length != 0.0F) {
        float wrapped = std::fmod(progress, length);
        if ((wrapped < 0.0F && length > 0.0F) || (wrapped > 0.0F && length < 0.0F)) {
            wrapped += length;
        }
        wrapped += 0.0F;
        if (!is_zero_approx(progress) && is_zero_approx(wrapped)) {
            return length;
        }
        return wrapped;
    }
    return std::clamp(progress, 0.0F, length);
}

Transform2D follow(const PathFollow2D& follower, const Curve2D& curve,
                   const Transform2D& transform) {
    Transform2D out = transform;
    if (curve.length() == 0.0F) {
        return out;
    }
    if (follower.rotates) {
        const Affine2D t = curve.sample_with_rotation(follower.progress, follower.cubic_interp);
        // Transform2D::translate_local, then the x axis's angle.
        out.position = t.origin + (t.x * follower.h_offset) + (t.y * follower.v_offset);
        out.rotation = slash::atan2(t.x.y, t.x.x);
    } else {
        const Vec2 position = curve.sample(follower.progress, follower.cubic_interp);
        out.position = {position.x + follower.h_offset, position.y + follower.v_offset};
    }
    return out;
}

Transform3D follow(const PathFollow3D& follower, const Curve3D& curve,
                   const Transform3D& transform) {
    Transform3D out = transform;
    if (curve.length() == 0.0F) {
        return out;
    }
    Affine3D t;
    if (follower.rotation_mode == "none") {
        t.origin = curve.sample(follower.progress, follower.cubic_interp);
    } else {
        t = curve.sample_with_rotation(follower.progress, follower.cubic_interp, false);
        const Vec3 tangent_at = t.z * -1.0F; // kept for the tilt
        // PathFollow3D::correct_posture.
        const Vec3 origin = t.origin;
        if (follower.rotation_mode == "oriented") {
            t = looking_at(tangent_at);
        } else if (follower.rotation_mode == "y" || follower.rotation_mode == "xy") {
            Vec3 angles = euler(orthonormalized(t));
            angles.z = 0.0F;
            if (follower.rotation_mode == "y") {
                angles.x = 0.0F;
            }
            Transform3D locked;
            locked.rotation = angles;
            t = affine(locked);
        }
        t.origin = origin;
        if (follower.use_model_front) {
            // Basis::from_scale(-1, 1, -1): +z faces forward.
            t.x = t.x * -1.0F;
            t.z = t.z * -1.0F;
        }
        if (follower.tilt_enabled) {
            const Affine3D twist = axis_angle(tangent_at, curve.sample_tilt(follower.progress));
            const Affine3D turned = twist * Affine3D{.x = t.x, .y = t.y, .z = t.z, .origin = {}};
            t.x = turned.x;
            t.y = turned.y;
            t.z = turned.z;
        }
    }
    // Transform3D::translate_local; the scale stays the entity's own.
    t.origin = t.origin + t.basis({follower.h_offset, follower.v_offset, 0.0F});
    out.position = t.origin;
    out.rotation = follower.rotation_mode == "none" ? Vec3{} : euler(orthonormalized(t));
    return out;
}

// ---- Cache and system ----

namespace {

struct PathCache {
    struct Entry2D {
        Path2D path;
        std::shared_ptr<const Curve2D> curve;
    };
    struct Entry3D {
        Path3D path;
        std::shared_ptr<const Curve3D> curve;
    };
    std::mutex mutex;
    std::unordered_map<entt::entity, Entry2D> curves_2d;
    std::unordered_map<entt::entity, Entry3D> curves_3d;
};

PathCache* find_cache(const World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<PathCache>>();
    return found != nullptr ? found->get() : nullptr;
}

std::shared_ptr<const Curve2D> bake(const Path2D& path) {
    return std::make_shared<const Curve2D>(path.points, path.bake_interval);
}

std::shared_ptr<const Curve3D> bake(const Path3D& path) {
    return std::make_shared<const Curve3D>(path.points, path.closed, path.bake_interval,
                                           path.up_vector_enabled);
}

template <typename Path, typename Map>
auto cached(const World& world, entt::entity entity, Map PathCache::*map) {
    using Curve = decltype(bake(std::declval<const Path&>()));
    if (!world.valid(entity)) {
        return Curve{};
    }
    const auto* path = world.ecs().try_get<const Path>(entity);
    if (path == nullptr) {
        return Curve{};
    }
    PathCache* cache = find_cache(world);
    if (cache == nullptr) {
        return bake(*path);
    }
    const std::scoped_lock lock(cache->mutex);
    auto& entries = cache->*map;
    if (const auto found = entries.find(entity);
        found != entries.end() && found->second.path == *path) {
        return found->second.curve;
    }
    Curve curve = bake(*path);
    std::erase_if(entries, [&](const auto& entry) {
        return !world.valid(entry.first) || !world.ecs().all_of<Path>(entry.first);
    });
    entries[entity] = {.path = *path, .curve = curve};
    return curve;
}

} // namespace

std::shared_ptr<const Curve2D> curve_2d(const World& world, entt::entity entity) {
    return cached<Path2D>(world, entity, &PathCache::curves_2d);
}

std::shared_ptr<const Curve3D> curve_3d(const World& world, entt::entity entity) {
    return cached<Path3D>(world, entity, &PathCache::curves_3d);
}

void add_path_systems(World& world) {
    if (find_cache(world) == nullptr) {
        auto cache = std::make_shared<PathCache>();
        world.ecs().ctx().emplace<std::shared_ptr<PathCache>>(cache);
        world.add_reset([cache](World& /*world*/) {
            const std::scoped_lock lock(cache->mutex);
            cache->curves_2d.clear();
            cache->curves_3d.clear();
        });
    }
    world.add_system(
        "path_follow",
        [](World& w, double /*tick_seconds*/) {
            auto& ecs = w.ecs();
            if (const auto followers = ecs.view<PathFollow2D, Transform2D>();
                followers.begin() != followers.end()) {
                const Tree2D tree(w);
                for (const auto [entity, follower, transform] : followers.each()) {
                    const std::shared_ptr<const Curve2D> curve = curve_2d(w, tree.parent(entity));
                    if (curve) {
                        follower.progress =
                            wrap_progress(follower.progress, curve->length(), follower.loop);
                        transform = follow(follower, *curve, transform);
                    }
                }
            }
            if (const auto followers = ecs.view<PathFollow3D, Transform3D>();
                followers.begin() != followers.end()) {
                const Tree3D tree(w);
                for (const auto [entity, follower, transform] : followers.each()) {
                    const std::shared_ptr<const Curve3D> curve = curve_3d(w, tree.parent(entity));
                    if (curve) {
                        follower.progress =
                            wrap_progress(follower.progress, curve->length(), follower.loop);
                        transform = follow(follower, *curve, transform);
                    }
                }
            }
        },
        {.doc = "Wraps each PathFollow2D's and PathFollow3D's progress and places it on its "
                "parent's curve.",
         .priority = 930});
}

std::vector<PathProblem> check_paths(const World& world) {
    std::vector<PathProblem> problems;
    const auto& ecs = world.ecs();
    std::vector<entt::entity> followers;
    for (const entt::entity entity : ecs.view<const PathFollow2D>()) {
        followers.push_back(entity);
    }
    for (const entt::entity entity : ecs.view<const PathFollow3D>()) {
        if (std::ranges::find(followers, entity) == followers.end()) {
            followers.push_back(entity);
        }
    }
    std::ranges::sort(followers, {}, [](entt::entity e) { return entt::to_entity(e); });
    std::optional<Tree2D> tree_2d;
    std::optional<Tree3D> tree_3d;
    for (const entt::entity entity : followers) {
        if (ecs.all_of<PathFollow2D>(entity)) {
            if (!tree_2d) {
                tree_2d.emplace(world);
            }
            const entt::entity parent = tree_2d->parent(entity);
            if (parent == entt::null || !ecs.all_of<Path2D>(parent)) {
                problems.push_back({entity, "/components/PathFollow2D",
                                    "only follows a path as a Transform2D child of an entity "
                                    "with a Path2D"});
            }
        }
        if (const auto* follower = ecs.try_get<const PathFollow3D>(entity); follower != nullptr) {
            if (!tree_3d) {
                tree_3d.emplace(world);
            }
            const entt::entity parent = tree_3d->parent(entity);
            const auto* path = parent != entt::null ? ecs.try_get<const Path3D>(parent) : nullptr;
            if (path == nullptr) {
                problems.push_back({entity, "/components/PathFollow3D",
                                    "only follows a path as a Transform3D child of an entity "
                                    "with a Path3D"});
            } else if (follower->rotation_mode == "oriented" && !path->up_vector_enabled) {
                problems.push_back({entity, "/components/PathFollow3D/rotation_mode",
                                    "\"oriented\" needs up vectors: set up_vector_enabled on the "
                                    "parent's Path3D"});
            }
        }
    }
    return problems;
}

} // namespace slash::world
