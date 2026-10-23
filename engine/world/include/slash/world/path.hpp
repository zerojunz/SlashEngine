#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <entt/entity/entity.hpp>
#include <memory>
#include <string>
#include <vector>

// Paths, as Godot's Path2D and Path3D with their Curve2D and Curve3D (scene/resources/curve.cpp)
// and the PathFollow2D and PathFollow3D that move along them. A curve is baked as Godot bakes
// it: each Bézier segment halved until its pieces are no longer than bake_interval, up to ten
// times, with the direction the curve goes at each point and, in 3D, the tilt and an up vector
// carried along it as a parallel transport frame. The same bits on every platform.
namespace slash::world {

class World;

// A Path2D's curve, baked.
class Curve2D {
public:
    Curve2D() = default;
    Curve2D(const std::vector<CurvePoint2D>& points, float bake_interval);

    // How long it is along its baked points, as get_baked_length.
    [[nodiscard]] float length() const { return length_; }
    // The point offset along it, offset clamped to its length, as sample_baked: between two
    // baked points on a line or, with cubic, on a cubic curve through their neighbors too.
    [[nodiscard]] Vec2 sample(float offset, bool cubic = false) const;
    // The same with the way the curve goes as its x axis and its left as y, as
    // sample_baked_with_rotation.
    [[nodiscard]] Affine2D sample_with_rotation(float offset, bool cubic = false) const;
    // The point of the curve nearest to a point, and how far along the curve it is, as
    // get_closest_point and get_closest_offset.
    [[nodiscard]] Vec2 closest_point(Vec2 to) const;
    [[nodiscard]] float closest_offset(Vec2 to) const;
    // The baked points, as get_baked_points.
    [[nodiscard]] const std::vector<Vec2>& points() const { return points_; }

private:
    struct Interval {
        int index = 0;
        float fraction = 0.0F;
    };
    [[nodiscard]] Interval find(float offset) const;

    std::vector<Vec2> points_;
    std::vector<Vec2> forward_;
    std::vector<float> distances_;
    float length_ = 0.0F;
};

// A Path3D's curve, baked.
class Curve3D {
public:
    Curve3D() = default;
    Curve3D(const std::vector<CurvePoint3D>& points, bool closed, float bake_interval,
            bool up_vector_enabled);

    [[nodiscard]] float length() const { return length_; }
    [[nodiscard]] Vec3 sample(float offset, bool cubic = false) const;
    // The same with the way the curve goes as its -z axis and its up vector as y, the tilt turned
    // in with apply_tilt, as sample_baked_with_rotation. Only the basis with one point.
    [[nodiscard]] Affine3D sample_with_rotation(float offset, bool cubic = false,
                                                bool apply_tilt = false) const;
    // The tilt offset along it, as sample_baked_tilt.
    [[nodiscard]] float sample_tilt(float offset) const;
    // The up vector offset along it, as sample_baked_up_vector; [0, 1, 0] without up vectors.
    [[nodiscard]] Vec3 sample_up_vector(float offset, bool apply_tilt = false) const;
    [[nodiscard]] Vec3 closest_point(Vec3 to) const;
    [[nodiscard]] float closest_offset(Vec3 to) const;
    [[nodiscard]] const std::vector<Vec3>& points() const { return points_; }
    [[nodiscard]] bool up_vector_enabled() const { return up_vector_enabled_; }

private:
    struct Interval {
        int index = 0;
        float fraction = 0.0F;
    };
    [[nodiscard]] Interval find(float offset) const;
    [[nodiscard]] Affine3D posture(Interval interval, bool apply_tilt) const;

    std::vector<Vec3> points_;
    std::vector<float> tilts_;
    std::vector<Vec3> up_;
    std::vector<Vec3> forward_;
    std::vector<float> distances_;
    float length_ = 0.0F;
    bool up_vector_enabled_ = true;
};

// The entity's Path2D or Path3D curve, baked: kept until the path changes with
// add_path_systems, else baked on each call. Null for an entity without one.
[[nodiscard]] std::shared_ptr<const Curve2D> curve_2d(const World& world, entt::entity entity);
[[nodiscard]] std::shared_ptr<const Curve3D> curve_3d(const World& world, entt::entity entity);

// Progress as Godot's set_progress leaves it on a curve of a length: round and round with loop
// (the length itself rather than 0 for progress past the start), else kept on the curve.
[[nodiscard]] float wrap_progress(float progress, float length, bool loop);

// Where a PathFollow2D or PathFollow3D goes on a curve at its progress, its offsets applied, as
// Godot's _update_transform and update_transform: the follower's Transform2D or Transform3D
// with its position and, as its settings say, its rotation set, the rest kept. The transform as
// it was on a curve of no length.
[[nodiscard]] Transform2D follow(const PathFollow2D& follower, const Curve2D& curve,
                                 const Transform2D& transform);
[[nodiscard]] Transform3D follow(const PathFollow3D& follower, const Curve3D& curve,
                                 const Transform3D& transform);

// The path_follow system: each tick, after animations and before physics, it wraps each
// PathFollow2D's and PathFollow3D's progress and places it on its parent's curve. Sessions and
// games add it through canvas::install.
void add_path_systems(World& world);

// Followers that can't follow: a PathFollow2D whose parent has no Path2D, a PathFollow3D whose
// parent has no Path3D, an unknown rotation_mode and "oriented" on a curve without up vectors,
// as Godot's configuration warnings. In the order entities were made.
struct PathProblem {
    entt::entity entity = entt::null;
    std::string path; // JSON Pointer within the entity, such as "/components/PathFollow3D"
    std::string message;
};
[[nodiscard]] std::vector<PathProblem> check_paths(const World& world);

} // namespace slash::world
