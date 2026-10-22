#include "space.hpp"

#include <Jolt/Geometry/ConvexHullBuilder.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace slash::physics3d::detail {

namespace {

// Godot's physics/jolt_physics_3d/collisions/collision_margin_fraction: no more of a box,
// cylinder or hull than this fraction of its shortest half extent is rounded off.
constexpr float kMarginFraction = 0.08F;
// Godot's active_edge_threshold, 50 degrees, as its cosine: mesh edges sharper than it collide.
constexpr float kActiveEdgeCos = 0.64278761F;
// Godot's world_boundary_shape_size: how far a world boundary reaches.
constexpr float kWorldBoundarySize = 2000.0F;

Vec3 column_scale(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

BuiltShape made(const JPH::ShapeSettings& settings, const char* what) {
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        const JPH::String& error = result.GetError();
        return {nullptr, std::string(what) + ": " + std::string(error.data(), error.size())};
    }
    return {result.Get(), {}};
}

} // namespace

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const float squared = dot(v, v);
    if (squared == 0.0F) {
        return {};
    }
    const float l = std::sqrt(squared);
    return {v.x / l, v.y / l, v.z / l};
}

Vec3 slide(Vec3 v, Vec3 n) {
    return v - (n * dot(n, v));
}

bool zero_approx(Vec3 v) {
    return std::abs(v.x) < kEpsilon && std::abs(v.y) < kEpsilon && std::abs(v.z) < kEpsilon;
}

float acos(float x) {
    if (x <= -1.0F) {
        return std::numbers::pi_v<float>;
    }
    if (x >= 1.0F) {
        return 0.0F;
    }
    return slash::atan2(std::sqrt((1.0F - x) * (1.0F + x)), x);
}

Unscaled unscale(const world::Affine3D& transform) {
    Vec3 x = transform.x;
    Vec3 y = transform.y;
    Vec3 z = transform.z;
    Unscaled out;
    out.scale = {length(x), length(y), length(z)};
    // A mirrored basis: its scale is negative and its rotation the rest, as Godot's get_scale.
    if (dot(x, cross(y, z)) < 0.0F) {
        out.scale = out.scale * -1.0F;
        x = x * -1.0F;
        y = y * -1.0F;
        z = z * -1.0F;
    }
    // Godot's Basis::orthonormalize, by Gram-Schmidt.
    x = normalized(x);
    y = normalized(y - (x * dot(x, y)));
    z = normalized(z - (x * dot(x, z)) - (y * dot(y, z)));
    out.transform = {.x = x, .y = y, .z = z, .origin = transform.origin};
    return out;
}

JPH::Quat rotation_of(const world::Affine3D& basis) {
    const JPH::Mat44 matrix(JPH::Vec4(basis.x.x, basis.x.y, basis.x.z, 0.0F),
                            JPH::Vec4(basis.y.x, basis.y.y, basis.y.z, 0.0F),
                            JPH::Vec4(basis.z.x, basis.z.y, basis.z.z, 0.0F),
                            JPH::Vec4(0.0F, 0.0F, 0.0F, 1.0F));
    return matrix.GetQuaternion().Normalized();
}

world::Affine3D affine_of(JPH::RVec3Arg position, JPH::QuatArg rotation) {
    const JPH::Mat44 matrix = JPH::Mat44::sRotation(rotation);
    return {.x = from_jolt(matrix.GetAxisX()),
            .y = from_jolt(matrix.GetAxisY()),
            .z = from_jolt(matrix.GetAxisZ()),
            .origin = from_jolt(JPH::Vec3(position))};
}

JPH::Mat44 matrix_of(const world::Affine3D& transform) {
    return {JPH::Vec4(transform.x.x, transform.x.y, transform.x.z, 0.0F),
            JPH::Vec4(transform.y.x, transform.y.y, transform.y.z, 0.0F),
            JPH::Vec4(transform.z.x, transform.z.y, transform.z.z, 0.0F),
            JPH::Vec4(transform.origin.x, transform.origin.y, transform.origin.z, 1.0F)};
}

BuiltShape build_shape(const world::CollisionShape3D& shape) {
    if (shape.shape == "box") {
        // Godot's JoltBoxShape3D.
        const Vec3 half = column_scale(shape.size, 0.5F);
        const float margin =
            std::min(shape.margin, std::min({half.x, half.y, half.z}) * kMarginFraction);
        return made(JPH::BoxShapeSettings(to_jolt(half), std::max(margin, 0.0F)), "box");
    }
    if (shape.shape == "sphere") {
        if (shape.radius <= 0.0F) {
            return {nullptr, "a sphere's radius must be above 0"};
        }
        return made(JPH::SphereShapeSettings(shape.radius), "sphere");
    }
    if (shape.shape == "capsule") {
        if (shape.radius <= 0.0F || shape.height <= 0.0F) {
            return {nullptr, "a capsule's radius and height must be above 0"};
        }
        if (shape.height < shape.radius * 2.0F) {
            return {nullptr, "a capsule's height must be at least twice its radius"};
        }
        const float cylinder = (shape.height / 2.0F) - shape.radius;
        if (cylinder <= 0.0F) {
            return made(JPH::SphereShapeSettings(shape.radius), "capsule");
        }
        return made(JPH::CapsuleShapeSettings(cylinder, shape.radius), "capsule");
    }
    if (shape.shape == "cylinder") {
        const float half_height = shape.height / 2.0F;
        const float margin =
            std::min(shape.margin, std::min(half_height, shape.radius) * kMarginFraction);
        return made(JPH::CylinderShapeSettings(half_height, shape.radius, std::max(margin, 0.0F)),
                    "cylinder");
    }
    if (shape.shape == "convex_polygon") {
        if (shape.points.empty()) {
            return {};
        }
        if (shape.points.size() < 3) {
            return {nullptr, "a convex polygon needs at least 3 points"};
        }
        JPH::Array<JPH::Vec3> points;
        points.reserve(shape.points.size());
        Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
        Vec3 high = low * -1.0F;
        for (const Vec3 point : shape.points) {
            points.push_back(to_jolt(point));
            low = {std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
            high = {std::max(high.x, point.x), std::max(high.y, point.y),
                    std::max(high.z, point.z)};
        }
        const Vec3 size = high - low;
        const float margin =
            std::min(shape.margin, std::min({size.x, size.y, size.z}) * 0.5F * kMarginFraction);
        return made(JPH::ConvexHullShapeSettings(points, std::max(margin, 0.0F)), "convex polygon");
    }
    if (shape.shape == "concave_polygon") {
        if (shape.faces.empty()) {
            return {};
        }
        if (shape.faces.size() % 3 != 0) {
            return {nullptr, "a concave polygon's faces need three points each"};
        }
        JPH::TriangleList triangles;
        const std::size_t count = shape.faces.size() / 3;
        triangles.reserve(shape.backface_collision ? 2 * count : count);
        const auto float3 = [](Vec3 v) { return JPH::Float3(v.x, v.y, v.z); };
        for (std::size_t i = 0; i < shape.faces.size(); i += 3) {
            // Jolt winds the other way round, as Godot's JoltConcavePolygonShape3D swaps them.
            const Vec3 a = shape.faces[i];
            const Vec3 b = shape.faces[i + 1];
            const Vec3 c = shape.faces[i + 2];
            triangles.emplace_back(float3(c), float3(b), float3(a), 0);
            if (shape.backface_collision) {
                // Solid from behind too: each face again, facing the other way.
                triangles.emplace_back(float3(a), float3(b), float3(c), 0);
            }
        }
        JPH::MeshShapeSettings settings(triangles);
        settings.mActiveEdgeCosThresholdAngle = kActiveEdgeCos;
        return made(settings, "concave polygon");
    }
    if (shape.shape == "world_boundary") {
        const Vec3 normal{shape.plane.x, shape.plane.y, shape.plane.z};
        const float l = length(normal);
        if (l == 0.0F) {
            return {nullptr, "a world boundary's plane normal must not be zero"};
        }
        // Godot's plane holds the points p where normal . p = d; Jolt's, normal . p + c = 0.
        const JPH::Plane plane(to_jolt(normal * (1.0F / l)), -shape.plane.w / l);
        return made(JPH::PlaneShapeSettings(plane, nullptr, kWorldBoundarySize / 2.0F),
                    "world boundary");
    }
    return {nullptr, "unknown shape \"" + shape.shape + "\""};
}

JPH::RefConst<JPH::Shape> scaled(const JPH::Shape* shape, Vec3 scale) {
    if (scale == Vec3{1.0F, 1.0F, 1.0F}) {
        return shape;
    }
    const JPH::Vec3 valid = shape->MakeScaleValid(to_jolt(scale));
    const JPH::ShapeSettings::ShapeResult result = JPH::ScaledShapeSettings(shape, valid).Create();
    return result.HasError() ? JPH::RefConst<JPH::Shape>(shape) : result.Get();
}

JPH::MassProperties mass_properties(const JPH::Shape& shape, float mass, Vec3 inertia) {
    const bool calculate_mass = mass <= 0.0F;
    const bool calculate_inertia = inertia.x <= 0.0F || inertia.y <= 0.0F || inertia.z <= 0.0F;
    JPH::MassProperties out = shape.GetMassProperties();
    if (calculate_mass && calculate_inertia) {
        // The shape's own.
    } else if (calculate_inertia) {
        out.ScaleToMass(mass);
    } else {
        out.mMass = mass;
    }
    // Shapes without volume, such as meshes, have no mass of their own; Jolt needs one.
    if (out.mMass <= 0.0F) {
        out.mMass = 1.0F;
    }
    JPH::Mat44& tensor = out.mInertia;
    if (inertia.x > 0.0F) {
        tensor(0, 0) = inertia.x;
        tensor(0, 1) = 0.0F;
        tensor(0, 2) = 0.0F;
        tensor(1, 0) = 0.0F;
        tensor(2, 0) = 0.0F;
    }
    if (inertia.y > 0.0F) {
        tensor(1, 1) = inertia.y;
        tensor(1, 0) = 0.0F;
        tensor(1, 2) = 0.0F;
        tensor(0, 1) = 0.0F;
        tensor(2, 1) = 0.0F;
    }
    if (inertia.z > 0.0F) {
        tensor(2, 2) = inertia.z;
        tensor(2, 0) = 0.0F;
        tensor(2, 1) = 0.0F;
        tensor(0, 2) = 0.0F;
        tensor(1, 2) = 0.0F;
    }
    tensor(3, 3) = 1.0F;
    return out;
}

} // namespace slash::physics3d::detail

namespace slash::physics3d {

std::vector<Vec3> convex_hull_triangles(std::span<const Vec3> points) {
    detail::set_up_jolt();
    JPH::Array<JPH::Vec3> positions;
    positions.reserve(points.size());
    for (const Vec3 point : points) {
        positions.emplace_back(point.x, point.y, point.z);
    }
    // As ConvexHullShapeSettings builds its hull, with no cap on its corners.
    JPH::ConvexHullBuilder builder(positions);
    const char* error = nullptr;
    const JPH::ConvexHullBuilder::EResult result =
        builder.Initialize(std::numeric_limits<int>::max(), 1.0e-3F, error);
    if (result != JPH::ConvexHullBuilder::EResult::Success) {
        return {};
    }
    std::vector<Vec3> out;
    for (const JPH::ConvexHullBuilder::Face* face : builder.GetFaces()) {
        if (face->mRemoved) {
            continue;
        }
        // Jolt winds faces counterclockwise from outside; Godot's go the other way.
        std::vector<int> corners;
        const JPH::ConvexHullBuilder::Edge* edge = face->mFirstEdge;
        do {
            corners.push_back(edge->mStartIdx);
            edge = edge->mNextEdge;
        } while (edge != face->mFirstEdge);
        std::reverse(corners.begin(), corners.end());
        const auto at = [&](int index) {
            const JPH::Vec3 p = positions[static_cast<std::size_t>(index)];
            return Vec3{p.GetX(), p.GetY(), p.GetZ()};
        };
        for (std::size_t k = 2; k < corners.size(); ++k) {
            out.push_back(at(corners[0]));
            out.push_back(at(corners[k - 1]));
            out.push_back(at(corners[k]));
        }
    }
    return out;
}

} // namespace slash::physics3d
