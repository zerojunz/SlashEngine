#include "slash/scene3d/csg.hpp"

#include "model_internal.hpp"

#include "slash/core/text.hpp"
#include "slash/world/path.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <manifold/manifold.h>
#include <mutex>
#include <numbers>
#include <unordered_map>
#include <utility>

namespace slash::scene3d {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * std::numbers::pi;
constexpr float kCmpEpsilon = 0.00001F; // Godot's CMP_EPSILON

// ---- Godot's math, operation for operation, so faces match Godot's bit for bit where it can ----

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

// Vector3::normalize: divided by the length, or zero for a zero vector.
Vec3 normalized(Vec3 v) {
    const float length_squared = (v.x * v.x) + (v.y * v.y) + (v.z * v.z);
    if (length_squared == 0.0F) {
        return {};
    }
    const float length = std::sqrt(length_squared);
    return {v.x / length, v.y / length, v.z / length};
}

// Plane(a, b, c)'s normal: Godot's faces are clockwise, so it points out of them.
Vec3 plane_normal(Vec3 a, Vec3 b, Vec3 c) {
    return normalized(cross(a - c, a - b));
}

Vec3 times(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 negated(Vec3 v) {
    return {-v.x, -v.y, -v.z};
}

bool is_equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const float tolerance = std::max(kCmpEpsilon * std::abs(a), kCmpEpsilon);
    return std::abs(a - b) < tolerance;
}

bool is_equal_approx(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return is_equal_approx(a[0], b[0]) && is_equal_approx(a[1], b[1]) &&
           is_equal_approx(a[2], b[2]);
}

// An angle in radians as Godot's float math leaves it after a double multiply.
float angle(double radians) {
    return static_cast<float>(radians);
}

// A Transform3D with Godot's row-major basis, for CSGPolygon3D's spin.
struct Xform {
    std::array<std::array<float, 3>, 3> rows{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    Vec3 origin;

    [[nodiscard]] Vec3 xform(Vec3 v) const {
        return {((rows[0][0] * v.x) + (rows[0][1] * v.y) + (rows[0][2] * v.z)) + origin.x,
                ((rows[1][0] * v.x) + (rows[1][1] * v.y) + (rows[1][2] * v.z)) + origin.y,
                ((rows[2][0] * v.x) + (rows[2][1] * v.y) + (rows[2][2] * v.z)) + origin.z};
    }

    // Transform3D::rotate: turned about axis by radians, in the parent's space.
    void rotate(Vec3 axis, float radians) {
        const Vec2 cs = cos_sin(radians);
        const float cosine = cs.x;
        const float sine = cs.y;
        const Vec3 sq{axis.x * axis.x, axis.y * axis.y, axis.z * axis.z};
        std::array<std::array<float, 3>, 3> r{};
        r[0][0] = sq.x + (cosine * (1.0F - sq.x));
        r[1][1] = sq.y + (cosine * (1.0F - sq.y));
        r[2][2] = sq.z + (cosine * (1.0F - sq.z));
        const float t = 1.0F - cosine;
        float xyzt = axis.x * axis.y * t;
        float zyxs = axis.z * sine;
        r[0][1] = xyzt - zyxs;
        r[1][0] = xyzt + zyxs;
        xyzt = axis.x * axis.z * t;
        zyxs = axis.y * sine;
        r[0][2] = xyzt + zyxs;
        r[2][0] = xyzt - zyxs;
        xyzt = axis.y * axis.z * t;
        zyxs = axis.x * sine;
        r[1][2] = xyzt - zyxs;
        r[2][1] = xyzt + zyxs;
        Xform turned;
        turned.rows = r;
        const Vec3 moved = turned.xform(origin);
        std::array<std::array<float, 3>, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                out[i][j] =
                    (rows[0][j] * r[i][0]) + (rows[1][j] * r[i][1]) + (rows[2][j] * r[i][2]);
            }
        }
        rows = out;
        origin = moved;
    }
};

float area(const std::vector<Vec2>& contour) {
    const auto n = static_cast<int>(contour.size());
    float a = 0.0F;
    for (int p = n - 1, q = 0; q < n; p = q++) {
        const Vec2 c = contour[static_cast<std::size_t>(p)];
        const Vec2 d = contour[static_cast<std::size_t>(q)];
        a += (c.x * d.y) - (c.y * d.x);
    }
    return a * 0.5F;
}

// Triangulate::is_inside_triangle; include_edges is Godot's name, though it excludes them.
bool is_inside_triangle(Vec2 a, Vec2 b, Vec2 c, Vec2 p, bool include_edges) {
    const float ax = c.x - b.x;
    const float ay = c.y - b.y;
    const float bx = a.x - c.x;
    const float by = a.y - c.y;
    const float cx = b.x - a.x;
    const float cy = b.y - a.y;
    const float apx = p.x - a.x;
    const float apy = p.y - a.y;
    const float bpx = p.x - b.x;
    const float bpy = p.y - b.y;
    const float cpx = p.x - c.x;
    const float cpy = p.y - c.y;
    const float a_cross_bp = (ax * bpy) - (ay * bpx);
    const float c_cross_ap = (cx * apy) - (cy * apx);
    const float b_cross_cp = (bx * cpy) - (by * cpx);
    if (include_edges) {
        return a_cross_bp > 0.0F && b_cross_cp > 0.0F && c_cross_ap > 0.0F;
    }
    return a_cross_bp >= 0.0F && b_cross_cp >= 0.0F && c_cross_ap >= 0.0F;
}

bool snip(const std::vector<Vec2>& contour, int u, int v, int w, int n, const std::vector<int>& vs,
          bool relaxed) {
    const auto at = [&](int i) {
        return contour[static_cast<std::size_t>(vs[static_cast<std::size_t>(i)])];
    };
    const Vec2 a = at(u);
    const Vec2 b = at(v);
    const Vec2 c = at(w);
    // Zero-area triangles are allowed only once all else failed, so three points in a line can
    // still close the last triangle.
    const float threshold = relaxed ? -kCmpEpsilon : kCmpEpsilon;
    if (threshold > (((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x)))) {
        return false;
    }
    for (int p = 0; p < n; ++p) {
        if (p == u || p == v || p == w) {
            continue;
        }
        if (is_inside_triangle(a, b, c, at(p), relaxed)) {
            return false;
        }
    }
    return true;
}

// ---- Brushes, as Godot's CSGBrush ----

struct Face {
    std::array<Vec3, 3> vertices{};
    std::array<Vec2, 3> uvs{};
    bool smooth = false;
    bool invert = false;
    int material = -1; // into Brush::materials, or -1 for Godot's default
};

struct Brush {
    std::vector<Face> faces;
    std::vector<SurfaceMaterial> materials;
};

// The shapes under a root as read from the world, compared to tell when its mesh must be made
// again.
struct CsgNode {
    entt::entity entity = entt::null;
    CsgShape shape;
    world::Affine3D local; // in its parent shape; the root's isn't used
    std::optional<world::StandardMaterial3D> material;
    // A CSGMesh3D's mesh: its model, or its primitive mesh, or why it has neither.
    std::shared_ptr<const Model> model;
    std::optional<Primitive> primitive;
    std::string mesh_error;
    // A CSGPolygon3D's path in path mode: its Path3D, where the curve starts (the path's place in
    // the world, or nowhere with path_local), or why there is none.
    std::optional<world::Path3D> path;
    world::Affine3D path_place;
    std::string path_error;
    std::vector<CsgNode> children; // the visible ones, in order

    friend bool operator==(const CsgNode&, const CsgNode&) = default;
};

std::string component_path(const CsgShape& shape) {
    return "/components/" + std::string(kCsgNames[shape.index()]);
}

// Faces all drawn with the shape's material, as CSGPrimitive3D makes them.
Brush primitive_brush(std::vector<Face> faces, const CsgNode& node, bool flip) {
    Brush brush;
    const int index = node.material ? 0 : -1;
    if (node.material) {
        SurfaceMaterial material;
        material.standard = node.material;
        brush.materials.push_back(std::move(material));
    }
    for (Face& face : faces) {
        face.invert = flip;
        face.material = index;
    }
    brush.faces = std::move(faces);
    return brush;
}

Face face_of(std::array<Vec3, 3> vertices, std::array<Vec2, 3> uvs, bool smooth) {
    return {.vertices = vertices, .uvs = uvs, .smooth = smooth};
}

std::vector<Face> box_faces(const world::CSGBox3D& box) {
    std::vector<Face> faces;
    const Vec3 mul{box.size.x / 2.0F, box.size.y / 2.0F, box.size.z / 2.0F};
    constexpr std::array<float, 8> kUvPoints = {0, 0, 0, 1, 1, 1, 1, 0};
    for (int i = 0; i < 6; ++i) {
        std::array<std::array<float, 3>, 4> points{};
        for (int j = 0; j < 4; ++j) {
            std::array<float, 3> v{};
            v[0] = 1.0F;
            v[1] = static_cast<float>(1 - (2 * ((j >> 1) & 1)));
            v[2] = v[1] * static_cast<float>(1 - (2 * (j & 1)));
            for (int k = 0; k < 3; ++k) {
                const auto axis = static_cast<std::size_t>((i + k) % 3);
                if (i < 3) {
                    points[static_cast<std::size_t>(j)][axis] = v[static_cast<std::size_t>(k)];
                } else {
                    points[static_cast<std::size_t>(3 - j)][axis] = -v[static_cast<std::size_t>(k)];
                }
            }
        }
        std::array<Vec3, 4> p{};
        std::array<Vec2, 4> u{};
        for (std::size_t j = 0; j < 4; ++j) {
            p[j] = times({points[j][0], points[j][1], points[j][2]}, mul);
            u[j] = {kUvPoints[j * 2], kUvPoints[(j * 2) + 1]};
        }
        faces.push_back(face_of({p[0], p[1], p[2]}, {u[0], u[1], u[2]}, false));
        faces.push_back(face_of({p[2], p[3], p[0]}, {u[2], u[3], u[0]}, false));
    }
    return faces;
}

std::vector<Face> sphere_faces(const world::CSGSphere3D& sphere) {
    std::vector<Face> faces;
    const int rings = std::max(sphere.rings, 1);
    const int segments = std::max(sphere.radial_segments, 4);
    const float radius = sphere.radius;
    // From the top down, as in an image, for the texture coordinates.
    const double latitude_step = -kPi / rings;
    const double longitude_step = kTau / segments;
    for (int i = 0; i < rings; ++i) {
        double cos0 = 0.0;
        double sin0 = 1.0;
        if (i > 0) {
            const Vec2 cs = cos_sin(angle((latitude_step * i) + (kTau / 4.0)));
            cos0 = cs.x;
            sin0 = cs.y;
        }
        const double v0 = static_cast<double>(i) / rings;
        double cos1 = 0.0;
        double sin1 = -1.0;
        if (i < rings - 1) {
            const Vec2 cs = cos_sin(angle((latitude_step * (i + 1)) + (kTau / 4.0)));
            cos1 = cs.x;
            sin1 = cs.y;
        }
        const double v1 = static_cast<double>(i + 1) / rings;
        for (int j = 0; j < segments; ++j) {
            // Sine to x and cosine to z, so the texture runs counter-clockwise on +x.
            const Vec2 cs0 = cos_sin(angle(longitude_step * j));
            const double x0 = cs0.y;
            const double z0 = cs0.x;
            const double u0 = static_cast<double>(j) / segments;
            const Vec2 cs1 = cos_sin(j == segments - 1 ? 0.0F : angle(longitude_step * (j + 1)));
            const double x1 = cs1.y;
            const double z1 = cs1.x;
            const double u1 = static_cast<double>(j + 1) / segments;
            const auto point = [&](double x, double y, double z) {
                return Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)} *
                       radius;
            };
            const std::array<Vec3, 4> v = {
                point(x0 * cos0, sin0, z0 * cos0), point(x1 * cos0, sin0, z1 * cos0),
                point(x1 * cos1, sin1, z1 * cos1), point(x0 * cos1, sin1, z0 * cos1)};
            const auto uv = [](double x, double y) {
                return Vec2{static_cast<float>(x), static_cast<float>(y)};
            };
            const std::array<Vec2, 4> u = {uv(u0, v0), uv(u1, v0), uv(u1, v1), uv(u0, v1)};
            // The first face, except at the north pole.
            if (i > 0) {
                faces.push_back(
                    face_of({v[0], v[1], v[2]}, {u[0], u[1], u[2]}, sphere.smooth_faces));
            }
            // The second face, except at the south pole.
            if (i < rings - 1) {
                faces.push_back(
                    face_of({v[2], v[3], v[0]}, {u[2], u[3], u[0]}, sphere.smooth_faces));
            }
        }
    }
    return faces;
}

std::vector<Face> cylinder_faces(const world::CSGCylinder3D& cylinder) {
    std::vector<Face> faces;
    const int sides = std::max(cylinder.sides, 3);
    const Vec3 mul{cylinder.radius, cylinder.height * 0.5F, cylinder.radius};
    const float top_scale = cylinder.cone ? 0.0F : 1.0F;
    const auto cap_uv = [](Vec3 p) { return Vec2{p.x, p.y} * 0.5F + Vec2{0.5F, 0.5F}; };
    for (int i = 0; i < sides; ++i) {
        const float inc = static_cast<float>(i) / static_cast<float>(sides);
        const float inc_n =
            i == sides - 1 ? 0.0F : static_cast<float>(i + 1) / static_cast<float>(sides);
        const Vec2 cs = cos_sin(angle(static_cast<double>(inc) * kTau));
        const Vec2 cs_n = cos_sin(angle(static_cast<double>(inc_n) * kTau));
        const Vec3 base{cs.x, 0.0F, cs.y};
        const Vec3 base_n{cs_n.x, 0.0F, cs_n.y};
        const std::array<Vec3, 4> p = {base + Vec3{0.0F, -1.0F, 0.0F},
                                       base_n + Vec3{0.0F, -1.0F, 0.0F},
                                       (base_n * top_scale) + Vec3{0.0F, 1.0F, 0.0F},
                                       (base * top_scale) + Vec3{0.0F, 1.0F, 0.0F}};
        const std::array<Vec2, 4> u = {Vec2{inc, 0.0F}, Vec2{inc_n, 0.0F}, Vec2{inc_n, 1.0F},
                                       Vec2{inc, 1.0F}};
        faces.push_back(face_of({times(p[0], mul), times(p[1], mul), times(p[2], mul)},
                                {u[0], u[1], u[2]}, cylinder.smooth_faces));
        if (!cylinder.cone) {
            faces.push_back(face_of({times(p[2], mul), times(p[3], mul), times(p[0], mul)},
                                    {u[2], u[3], u[0]}, cylinder.smooth_faces));
        }
        faces.push_back(
            face_of({times(p[1], mul), times(p[0], mul), times(Vec3{0.0F, -1.0F, 0.0F}, mul)},
                    {cap_uv(p[1]), cap_uv(p[0]), Vec2{0.5F, 0.5F}}, false));
        if (!cylinder.cone) {
            // The top takes the bottom's texture coordinates, as Godot's does.
            faces.push_back(
                face_of({times(p[3], mul), times(p[2], mul), times(Vec3{0.0F, 1.0F, 0.0F}, mul)},
                        {cap_uv(p[1]), cap_uv(p[0]), Vec2{0.5F, 0.5F}}, false));
        }
    }
    return faces;
}

std::vector<Face> torus_faces(const world::CSGTorus3D& torus) {
    std::vector<Face> faces;
    float min_radius = torus.inner_radius;
    float max_radius = torus.outer_radius;
    if (min_radius == max_radius) {
        return faces;
    }
    if (min_radius > max_radius) {
        std::swap(min_radius, max_radius);
    }
    const float radius = (max_radius - min_radius) * 0.5F;
    const int sides = std::max(torus.sides, 3);
    const int ring_sides = std::max(torus.ring_sides, 3);
    for (int i = 0; i < sides; ++i) {
        const float inci = static_cast<float>(i) / static_cast<float>(sides);
        const float inci_n =
            i == sides - 1 ? 0.0F : static_cast<float>(i + 1) / static_cast<float>(sides);
        const Vec2 csi = cos_sin(angle(static_cast<double>(inci) * kTau));
        const Vec2 csi_n = cos_sin(angle(static_cast<double>(inci_n) * kTau));
        const Vec3 normali{csi.x, 0.0F, csi.y};
        const Vec3 normali_n{csi_n.x, 0.0F, csi_n.y};
        for (int j = 0; j < ring_sides; ++j) {
            const float incj = static_cast<float>(j) / static_cast<float>(ring_sides);
            const float incj_n = j == ring_sides - 1
                                     ? 0.0F
                                     : static_cast<float>(j + 1) / static_cast<float>(ring_sides);
            const Vec2 csj = cos_sin(angle(static_cast<double>(incj) * kTau));
            const Vec2 csj_n = cos_sin(angle(static_cast<double>(incj_n) * kTau));
            const Vec2 normalj = (csj * radius) + Vec2{min_radius + radius, 0.0F};
            const Vec2 normalj_n = (csj_n * radius) + Vec2{min_radius + radius, 0.0F};
            const std::array<Vec3, 4> p = {
                Vec3{normali.x * normalj.x, normalj.y, normali.z * normalj.x},
                Vec3{normali.x * normalj_n.x, normalj_n.y, normali.z * normalj_n.x},
                Vec3{normali_n.x * normalj_n.x, normalj_n.y, normali_n.z * normalj_n.x},
                Vec3{normali_n.x * normalj.x, normalj.y, normali_n.z * normalj.x}};
            const std::array<Vec2, 4> u = {Vec2{inci, incj}, Vec2{inci, incj_n},
                                           Vec2{inci_n, incj_n}, Vec2{inci_n, incj}};
            faces.push_back(face_of({p[0], p[2], p[1]}, {u[0], u[2], u[1]}, torus.smooth_faces));
            faces.push_back(face_of({p[3], p[2], p[0]}, {u[3], u[2], u[0]}, torus.smooth_faces));
        }
    }
    return faces;
}

std::vector<Face> polygon_faces(const world::CSGPolygon3D& polygon, const std::string& path,
                                const CsgNode& node, std::vector<Problem>& problems) {
    const entt::entity entity = node.entity;
    std::vector<Face> faces;
    const std::string field = path + "/polygon";
    if (polygon.polygon.size() < 3) {
        problems.push_back({entity, field,
                            "has " + std::to_string(polygon.polygon.size()) +
                                " points; a polygon needs at least 3"});
        return faces;
    }
    const bool spin = polygon.mode == "spin";
    const bool along_path = polygon.mode == "path";
    if (!spin && !along_path && polygon.mode != "depth") {
        problems.push_back({entity, path + "/mode",
                            R"(expected "depth", "spin" or "path", not ")" + polygon.mode + "\""});
        return faces;
    }
    std::vector<Vec2> shape = polygon.polygon;
    const float shape_area = area(shape);
    if (shape_area == 0.0F) {
        problems.push_back({entity, field, "has no area: its points are in a line"});
        return faces;
    }
    if (shape_area > 0.0F) {
        std::ranges::reverse(shape);
    }
    const auto sides = static_cast<int>(shape.size());
    const std::vector<int> triangles = triangulate(shape);
    if (triangles.size() < 3) {
        problems.push_back(
            {entity, field, "can't be split into triangles: make sure its edges don't cross"});
        return faces;
    }
    Vec2 rect_begin = shape[0];
    Vec2 rect_end = shape[0];
    for (const Vec2 p : shape) {
        rect_begin = {std::min(rect_begin.x, p.x), std::min(rect_begin.y, p.y)};
        rect_end = {std::max(rect_end.x, p.x), std::max(rect_end.y, p.y)};
    }
    const Vec2 rect_size = rect_end - rect_begin;
    const auto rect_uv = [&](Vec2 p) {
        const Vec2 d = p - rect_begin;
        return Vec2{d.x / rect_size.x, d.y / rect_size.y};
    };

    // In path mode, the curve it is pushed along.
    std::optional<world::Curve3D> curve;
    if (along_path) {
        if (!node.path) {
            problems.push_back({entity, path + "/path_node", node.path_error});
            return faces;
        }
        if (node.path->points.size() < 2) {
            problems.push_back({entity, path + "/path_node",
                                "names a Path3D with fewer than 2 points: give it more"});
            return faces;
        }
        curve.emplace(node.path->points, node.path->closed, node.path->bake_interval,
                      node.path->up_vector_enabled);
    }

    const int spin_sides = std::max(polygon.spin_sides, 3);
    const float spin_degrees = std::clamp(polygon.spin_degrees, 0.01F, 360.0F);
    const float path_interval = std::max(polygon.path_interval, 0.01F);
    int extrusions = 1;
    int end_count = 2;
    double curve_length = 1.0;
    if (spin) {
        extrusions = spin_sides;
        end_count = spin_degrees < 360.0F ? 2 : 0;
    } else if (along_path) {
        curve_length = curve->length();
        if (polygon.path_interval_type == "subdivide") {
            extrusions = static_cast<int>(std::ceil(static_cast<double>(node.path->points.size()) /
                                                    static_cast<double>(path_interval)));
        } else {
            extrusions = std::max(1, static_cast<int>(std::ceil(curve_length / path_interval))) + 1;
        }
        if (!polygon.path_joined) {
            extrusions -= 1;
        } else {
            end_count = 0;
        }
        extrusions = std::max(extrusions, 1);
    }
    double u_step = 1.0 / extrusions;
    if (polygon.path_u_distance > 0.0F) {
        u_step *= curve_length / polygon.path_u_distance;
    }
    const double v_step = 1.0 / sides;
    const auto spin_step =
        (spin_degrees / static_cast<float>(spin_sides)) * static_cast<float>(kPi / 180.0);
    const float depth = std::max(polygon.depth, 0.001F);
    double extrusion_step = 1.0 / extrusions;
    if (along_path) {
        if (polygon.path_joined) {
            extrusion_step = 1.0 / std::max(extrusions - 1, 1);
        }
        extrusion_step *= curve_length;
    }

    // Where each ring of the polygon is: Godot's spin turns an Xform; a path places an Affine3D,
    // its rings facing the way the curve goes.
    Xform current;
    world::Affine3D on_path;
    world::Affine3D previous_on_path;
    world::Affine3D before_previous_on_path;
    const world::Affine3D base = along_path ? node.path_place : world::Affine3D{};
    const bool rotate_with_path = polygon.path_rotation != "polygon";
    const auto ring_at = [&](Vec3 at, Vec3 direction, Vec3 up) {
        // base.translated_local(at) * Transform3D().looking_at(direction, up).
        world::Affine3D placed = base * world::looking_at(direction, up);
        placed.origin = base.apply(at);
        return placed;
    };
    if (along_path) {
        Vec3 at;
        Vec3 up{0.0F, 1.0F, 0.0F};
        Vec3 direction{0.0F, 0.0F, -1.0F};
        if (!rotate_with_path) {
            at = curve->sample(0.0F);
        } else {
            if (!polygon.path_rotation_accurate) {
                at = curve->sample(0.0F);
                const Vec3 next = curve->sample(static_cast<float>(extrusion_step));
                direction = next - at;
                if (polygon.path_joined) {
                    direction = next - curve->sample(curve->length());
                }
            } else {
                const world::Affine3D sampled = curve->sample_with_rotation(0.0F);
                at = sampled.origin;
                direction = sampled.basis({0.0F, 0.0F, -1.0F});
            }
            if (polygon.path_rotation == "path_follow") {
                up = curve->sample_up_vector(0.0F, true);
            }
        }
        on_path = ring_at(at, direction, up);
    }
    const auto point = [&](const Xform& t, Vec2 p) {
        return along_path ? on_path.apply({p.x, p.y, 0.0F}) : t.xform({p.x, p.y, 0.0F});
    };
    const auto triangle_at = [&](std::size_t face, std::size_t corner) {
        return static_cast<std::size_t>(triangles[(face * 3) + corner]);
    };
    const std::size_t shape_faces = triangles.size() / 3;
    if (end_count > 0) {
        // The front end, its corners turned the other way.
        for (std::size_t f = 0; f < shape_faces; ++f) {
            Face face;
            for (std::size_t k = 0; k < 3; ++k) {
                const Vec2 p = shape[triangle_at(f, 2 - k)];
                Vec2 uv = rect_uv(p);
                // The left of the bottom half of the texture, upside down.
                uv.x = uv.x / 2.0F;
                uv.y = 1.0F - (uv.y / 2.0F);
                face.vertices[k] = point(current, p);
                face.uvs[k] = uv;
            }
            faces.push_back(face);
        }
    }
    const float simplify_dot =
        cos_sin(static_cast<float>(static_cast<double>(polygon.path_simplify_angle) * kPi / 180.0))
            .x;
    Vec3 previous_simplify_direction;
    int faces_combined = 0;
    const std::size_t extrusion_faces = static_cast<std::size_t>(sides) * 2;
    for (int x0 = 0; x0 < extrusions; ++x0) {
        const Xform previous = current;
        before_previous_on_path = previous_on_path;
        previous_on_path = on_path;
        if (along_path) {
            // Godot's MODE_PATH step: the next ring along the curve.
            const double previous_offset = x0 * extrusion_step;
            double current_offset = (x0 + 1) * extrusion_step;
            if (polygon.path_joined && x0 == extrusions - 1) {
                current_offset = 0.0;
            }
            const Vec3 previous_point = curve->sample(static_cast<float>(previous_offset));
            const world::Affine3D sampled =
                curve->sample_with_rotation(static_cast<float>(current_offset));
            const Vec3 current_point = sampled.origin;
            const Vec3 step = current_point - previous_point;
            const float step_length =
                std::sqrt((step.x * step.x) + (step.y * step.y) + (step.z * step.z));
            const Vec3 extrusion_direction =
                step_length == 0.0F ? Vec3{} : step * (1.0F / step_length);
            // A ring turned less than path_simplify_angle from the last replaces its quads.
            const float turn = (previous_simplify_direction.x * extrusion_direction.x) +
                               (previous_simplify_direction.y * extrusion_direction.y) +
                               (previous_simplify_direction.z * extrusion_direction.z);
            if (polygon.path_simplify_angle > 0.0F && x0 > 0 && turn > simplify_dot) {
                faces_combined += 1;
                previous_on_path = before_previous_on_path;
                faces.resize(faces.size() - extrusion_faces);
            } else {
                faces_combined = 0;
                previous_simplify_direction = extrusion_direction;
            }
            Vec3 up{0.0F, 1.0F, 0.0F};
            Vec3 direction{0.0F, 0.0F, -1.0F};
            if (rotate_with_path) {
                if (!polygon.path_rotation_accurate) {
                    double next_offset = (x0 + 2) * extrusion_step;
                    if (x0 == extrusions - 1) {
                        next_offset = polygon.path_joined ? extrusion_step : current_offset;
                    }
                    direction = curve->sample(static_cast<float>(next_offset)) - previous_point;
                } else {
                    direction = sampled.basis({0.0F, 0.0F, -1.0F});
                }
                if (polygon.path_rotation == "path_follow") {
                    up = curve->sample_up_vector(static_cast<float>(current_offset), true);
                }
            }
            on_path = ring_at(current_point, direction, up);
        } else if (!spin) {
            current.origin = current.origin + Vec3{0.0F, 0.0F, -depth};
        } else if (end_count == 0 && x0 == extrusions - 1) {
            current = Xform{};
        } else {
            current.rotate({0.0F, 1.0F, 0.0F}, spin_step);
        }
        double u0 = (x0 - faces_combined) * u_step;
        double u1 = (x0 + 1) * u_step;
        if (along_path && !polygon.path_continuous_u) {
            u0 = 0.0;
            u1 = 1.0;
        }
        const auto previous_point_of = [&](Vec2 p) {
            return along_path ? previous_on_path.apply({p.x, p.y, 0.0F}) : point(previous, p);
        };
        for (int y0 = 0; y0 < sides; ++y0) {
            const int y1 = (y0 + 1) % sides;
            // The top half of the texture.
            const double v0 = (y0 * v_step) / 2.0;
            const double v1 = ((y0 + 1) * v_step) / 2.0;
            const Vec2 p0 = shape[static_cast<std::size_t>(y0)];
            const Vec2 p1 = shape[static_cast<std::size_t>(y1)];
            const std::array<Vec3, 4> v = {previous_point_of(p0), point(current, p0),
                                           point(current, p1), previous_point_of(p1)};
            const auto uv = [](double x, double y) {
                return Vec2{static_cast<float>(x), static_cast<float>(y)};
            };
            const std::array<Vec2, 4> u = {uv(u0, v0), uv(u1, v0), uv(u1, v1), uv(u0, v1)};
            faces.push_back(face_of({v[0], v[1], v[2]}, {u[0], u[1], u[2]}, polygon.smooth_faces));
            faces.push_back(face_of({v[2], v[3], v[0]}, {u[2], u[3], u[0]}, polygon.smooth_faces));
        }
    }
    if (end_count > 1) {
        // The back end.
        for (std::size_t f = 0; f < shape_faces; ++f) {
            Face face;
            for (std::size_t k = 0; k < 3; ++k) {
                const Vec2 p = shape[triangle_at(f, k)];
                Vec2 uv = rect_uv(p);
                // The right of the bottom half, mirrored and upside down.
                uv.x = 1.0F - (uv.x / 2.0F);
                uv.y = 1.0F - (uv.y / 2.0F);
                face.vertices[k] = point(current, p);
                face.uvs[k] = uv;
            }
            faces.push_back(face);
        }
    }
    return faces;
}

// CSGMesh3D's faces: smooth where a triangle's corners have different normals.
Brush mesh_brush(const CsgNode& node, bool flip) {
    Brush brush;
    const auto material_index = [&](const SurfaceMaterial& material) {
        if (!material.standard && !material.model) {
            return -1;
        }
        const auto found = std::ranges::find(brush.materials, material);
        if (found != brush.materials.end()) {
            return static_cast<int>(found - brush.materials.begin());
        }
        brush.materials.push_back(material);
        return static_cast<int>(brush.materials.size() - 1);
    };
    const auto add = [&](const std::vector<render::Vertex3D>& vertices,
                         const std::vector<std::uint32_t>& indices, const world::Affine3D& place,
                         int material) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            Face face;
            std::array<std::array<float, 3>, 3> normals{};
            for (std::size_t k = 0; k < 3; ++k) {
                const render::Vertex3D& v = vertices[indices[i + k]];
                face.vertices[k] = place.apply(position_of(v));
                face.uvs[k] = {v.uv[0], v.uv[1]};
                normals[k] = v.normal;
            }
            face.smooth = !(is_equal_approx(normals[0], normals[1]) &&
                            is_equal_approx(normals[0], normals[2]));
            face.invert = flip;
            face.material = material;
            brush.faces.push_back(face);
        }
    };
    const auto& shape = std::get<world::CSGMesh3D>(node.shape);
    if (node.primitive) {
        auto made = make_mesh(*node.primitive);
        if (made) {
            SurfaceMaterial material;
            material.standard = node.material;
            add(made.value().vertices, made.value().indices, {}, material_index(material));
        }
        return brush;
    }
    if (!node.model) {
        return brush;
    }
    const Model& model = *node.model;
    for (const ModelPart& part : model.parts) {
        // A skin's surfaces are where its joints put them, which is where they are at rest.
        const world::Affine3D place = part.skin >= 0 ? world::Affine3D{} : part.transform;
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            SurfaceMaterial material;
            if (node.material) {
                material.standard = node.material;
            } else if (surface.material >= 0) {
                material.model_path = shape.mesh;
                material.model = node.model;
                material.model_material = surface.material;
                material.entity = node.entity;
            }
            add(surface.vertices, surface.indices, place, material_index(material));
        }
    }
    return brush;
}

Brush own_brush(const CsgNode& node, std::vector<Problem>& problems) {
    const std::string path = component_path(node.shape);
    return std::visit(
        [&](const auto& shape) -> Brush {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, world::CSGCombiner3D>) {
                return {};
            } else if constexpr (std::is_same_v<T, world::CSGBox3D>) {
                return primitive_brush(box_faces(shape), node, shape.flip_faces);
            } else if constexpr (std::is_same_v<T, world::CSGSphere3D>) {
                return primitive_brush(sphere_faces(shape), node, shape.flip_faces);
            } else if constexpr (std::is_same_v<T, world::CSGCylinder3D>) {
                return primitive_brush(cylinder_faces(shape), node, shape.flip_faces);
            } else if constexpr (std::is_same_v<T, world::CSGTorus3D>) {
                if (shape.inner_radius == shape.outer_radius) {
                    problems.push_back({node.entity, path + "/inner_radius",
                                        "equals outer_radius, so the torus has no thickness"});
                }
                return primitive_brush(torus_faces(shape), node, shape.flip_faces);
            } else if constexpr (std::is_same_v<T, world::CSGPolygon3D>) {
                return primitive_brush(polygon_faces(shape, path, node, problems), node,
                                       shape.flip_faces);
            } else {
                if (!node.mesh_error.empty()) {
                    problems.push_back({node.entity, path + "/mesh", node.mesh_error});
                }
                return mesh_brush(node, shape.flip_faces);
            }
        },
        node.shape);
}

// ---- Manifold, as Godot's _pack_manifold and _unpack_manifold ----

enum Property : std::uint8_t { PositionX, PositionY, PositionZ, Invert, Smooth, UvX, UvY, Count };
constexpr std::array<std::size_t, 3> kOrder = {0, 2, 1};
using Materials = std::unordered_map<std::uint32_t, SurfaceMaterial>;

manifold::Manifold pack(const Brush& brush, Materials& materials) {
    // A run of triangles per material, in the order the materials first appear.
    std::vector<std::pair<std::uint32_t, std::vector<const Face*>>> runs;
    for (const Face& face : brush.faces) {
        const auto key = static_cast<std::uint32_t>(face.material);
        auto run = std::ranges::find(runs, key,
                                     &std::pair<std::uint32_t, std::vector<const Face*>>::first);
        if (run == runs.end()) {
            runs.push_back({key, {}});
            run = runs.end() - 1;
        }
        run->second.push_back(&face);
    }
    manifold::MeshGL64 mesh;
    mesh.numProp = Property::Count;
    mesh.runOriginalID.reserve(runs.size());
    mesh.runIndex.reserve(runs.size() + 1);
    mesh.vertProperties.reserve(brush.faces.size() * 3 * Property::Count);
    for (const auto& [key, faces] : runs) {
        mesh.runIndex.push_back(mesh.triVerts.size());
        const std::uint32_t id = manifold::Manifold::ReserveIDs(1);
        mesh.runOriginalID.push_back(id);
        materials[id] = key < brush.materials.size() ? brush.materials[key] : SurfaceMaterial{};
        for (const Face* face : faces) {
            for (const std::size_t i : kOrder) {
                mesh.triVerts.push_back(mesh.vertProperties.size() / Property::Count);
                mesh.vertProperties.insert(mesh.vertProperties.end(),
                                           {face->vertices[i].x, face->vertices[i].y,
                                            face->vertices[i].z, face->invert ? 1.0 : 0.0,
                                            face->smooth ? 1.0 : 0.0, face->uvs[i].x,
                                            face->uvs[i].y});
            }
        }
    }
    // The runs need an end.
    mesh.runIndex.push_back(mesh.triVerts.size());
    mesh.tolerance = 2.0 * FLT_EPSILON;
    mesh.Merge();
    return {mesh};
}

Brush unpack(const manifold::Manifold& result, const Materials& materials) {
    Brush brush;
    const manifold::MeshGL64 mesh = result.GetMeshGL64();
    if (mesh.runIndex.size() < 2) {
        return brush;
    }
    const auto property = [&](std::uint64_t vertex, Property p) {
        return mesh.vertProperties[(vertex * mesh.numProp) + p];
    };
    for (std::size_t run = 0; run + 1 < mesh.runIndex.size(); ++run) {
        SurfaceMaterial material;
        if (run < mesh.runOriginalID.size()) {
            if (const auto found = materials.find(mesh.runOriginalID[run]);
                found != materials.end()) {
                material = found->second;
            }
        }
        auto index = std::ranges::find(brush.materials, material) - brush.materials.begin();
        if (static_cast<std::size_t>(index) == brush.materials.size()) {
            brush.materials.push_back(material);
        }
        for (std::uint64_t corner = mesh.runIndex[run]; corner < mesh.runIndex[run + 1];
             corner += 3) {
            Face face;
            face.material = static_cast<int>(index);
            const std::uint64_t first = mesh.triVerts[corner + kOrder[0]];
            face.smooth = property(first, Property::Smooth) > 0.5;
            face.invert = property(first, Property::Invert) > 0.5;
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint64_t v = mesh.triVerts[corner + kOrder[k]];
                face.vertices[k] = {static_cast<float>(property(v, Property::PositionX)),
                                    static_cast<float>(property(v, Property::PositionY)),
                                    static_cast<float>(property(v, Property::PositionZ))};
                face.uvs[k] = {static_cast<float>(property(v, Property::UvX)),
                               static_cast<float>(property(v, Property::UvY))};
            }
            brush.faces.push_back(face);
        }
    }
    return brush;
}

manifold::OpType op_of(const std::string& operation) {
    if (operation == "subtraction") {
        return manifold::OpType::Subtract;
    }
    if (operation == "intersection") {
        return manifold::OpType::Intersect;
    }
    return manifold::OpType::Add;
}

std::string status_name(manifold::Manifold::Error error) {
    using E = manifold::Manifold::Error;
    switch (error) {
    case E::NonFiniteVertex:
        return "a corner isn't a finite number";
    case E::NotManifold:
        return "not manifold";
    case E::ResultTooLarge:
        return "too many triangles";
    default:
        return "invalid mesh";
    }
}

// Godot's _get_brush: the shape's own faces, then its visible children's in order, each added to,
// intersected with or cut from what came before.
Brush brush_of(const CsgNode& node, std::vector<Problem>& problems) {
    const Brush own = own_brush(node, problems);
    Materials materials;
    const manifold::Manifold root = pack(own, materials);
    if (!own.faces.empty() && root.Status() != manifold::Manifold::Error::NoError) {
        problems.push_back({node.entity, component_path(node.shape),
                            "its faces don't close (" + status_name(root.Status()) +
                                "), so it adds nothing: each edge must join exactly two faces"});
    }
    manifold::OpType current = op_of(csg_settings(node.shape).operation);
    std::vector<manifold::Manifold> manifolds{root};
    for (const CsgNode& child : node.children) {
        Brush brush = brush_of(child, problems);
        for (Face& face : brush.faces) {
            for (Vec3& v : face.vertices) {
                v = child.local.apply(v);
            }
        }
        manifold::Manifold packed = pack(brush, materials);
        const manifold::OpType op = op_of(csg_settings(child.shape).operation);
        if (op != current) {
            manifold::Manifold done = manifold::Manifold::BatchBoolean(manifolds, current);
            manifolds.clear();
            manifolds.push_back(std::move(done));
            current = op;
        }
        manifolds.push_back(std::move(packed));
    }
    return unpack(manifold::Manifold::BatchBoolean(manifolds, current), materials);
}

// A float's bits. std::bit_cast would do, but the glibc 2.31 sysroot Linux exports build
// against has libstdc++ 10, which lacks it.
std::uint32_t bits_of(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct Vec3Hash {
    std::size_t operator()(Vec3 v) const noexcept {
        // -0 and 0 are equal, so they hash the same.
        const auto bits = [](float f) {
            return static_cast<std::size_t>(bits_of(f == 0.0F ? 0.0F : f));
        };
        return (bits(v.x) * 73856093U) ^ (bits(v.y) * 19349663U) ^ (bits(v.z) * 83492791U);
    }
};

// Godot's update_shape: one surface per material, smooth faces' normals the sum of the faces
// meeting at each corner, inverted faces turned round, and tangents by MikkTSpace.
CsgMesh mesh_of(const Brush& brush, const CsgSettings& settings) {
    CsgMesh out;
    std::unordered_map<Vec3, Vec3, Vec3Hash> smooth_normals;
    const std::size_t count = brush.materials.size() + 1;
    const auto surface_of = [&](const Face& face) {
        return face.material < 0 ? count - 1 : static_cast<std::size_t>(face.material);
    };
    std::vector<std::size_t> face_counts(count, 0);
    for (const Face& face : brush.faces) {
        if (face.smooth) {
            const Vec3 n = plane_normal(face.vertices[0], face.vertices[1], face.vertices[2]);
            for (const Vec3 v : face.vertices) {
                auto [it, added] = smooth_normals.try_emplace(v, n);
                if (!added) {
                    it->second = it->second + n;
                }
            }
        }
        ++face_counts[surface_of(face)];
    }
    std::vector<Triangles> surfaces(count);
    for (std::size_t i = 0; i < count; ++i) {
        surfaces[i].vertices.reserve(face_counts[i] * 3);
    }
    for (const Face& face : brush.faces) {
        std::array<std::size_t, 3> order = {0, 1, 2};
        if (face.invert) {
            std::swap(order[1], order[2]);
        }
        Triangles& surface = surfaces[surface_of(face)];
        const std::size_t last = surface.vertices.size();
        surface.vertices.resize(last + 3);
        const Vec3 flat = plane_normal(face.vertices[0], face.vertices[1], face.vertices[2]);
        for (std::size_t j = 0; j < 3; ++j) {
            const Vec3 v = face.vertices[j];
            Vec3 normal = flat;
            if (face.smooth) {
                if (const auto found = smooth_normals.find(v); found != smooth_normals.end()) {
                    normal = normalized(found->second);
                }
            }
            if (face.invert) {
                normal = negated(normal);
            }
            render::Vertex3D& corner = surface.vertices[last + order[j]];
            corner.position = {v.x, v.y, v.z};
            corner.normal = {normal.x, normal.y, normal.z};
            corner.uv = {face.uvs[j].x, face.uvs[j].y};
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        Triangles& surface = surfaces[i];
        if (surface.vertices.empty()) {
            continue;
        }
        surface.indices.resize(surface.vertices.size());
        for (std::size_t k = 0; k < surface.indices.size(); ++k) {
            surface.indices[k] = static_cast<std::uint32_t>(k);
        }
        if (settings.calculate_tangents) {
            generate_tangents(surface);
        } else {
            dummy_tangents(surface);
            reindex(surface);
        }
        out.surfaces.push_back(
            {.vertices = std::move(surface.vertices),
             .indices = std::move(surface.indices),
             .material = i + 1 < count ? brush.materials[i] : SurfaceMaterial{}});
    }
    auto collision = std::make_shared<std::vector<Vec3>>();
    collision->reserve(brush.faces.size() * 3);
    for (const Face& face : brush.faces) {
        collision->push_back(face.vertices[0]);
        collision->push_back(face.vertices[face.invert ? 2 : 1]);
        collision->push_back(face.vertices[face.invert ? 1 : 2]);
    }
    out.collision = std::move(collision);
    out.triangles = brush.faces.size();
    if (!brush.faces.empty()) {
        out.min = brush.faces[0].vertices[0];
        out.max = out.min;
        for (const Face& face : brush.faces) {
            for (const Vec3 v : face.vertices) {
                out.min = {std::min(out.min.x, v.x), std::min(out.min.y, v.y),
                           std::min(out.min.z, v.z)};
                out.max = {std::max(out.max.x, v.x), std::max(out.max.y, v.y),
                           std::max(out.max.z, v.z)};
            }
        }
    }
    return out;
}

std::atomic<std::uint64_t> next_version{1};

std::size_t bytes_of(const CsgMesh& mesh) {
    std::size_t bytes = sizeof(CsgMesh);
    for (const MeshSurface& surface : mesh.surfaces) {
        bytes += (surface.vertices.size() * sizeof(render::Vertex3D)) +
                 (surface.indices.size() * sizeof(std::uint32_t));
    }
    if (mesh.collision) {
        bytes += mesh.collision->size() * sizeof(Vec3);
    }
    return bytes;
}

// The meshes of a world's roots, kept until a shape under them changes.
struct CsgCache {
    struct Entry {
        CsgNode input;
        std::shared_ptr<const CsgMesh> mesh;
    };
    std::mutex mutex;
    ModelLoader load;
    std::unordered_map<entt::entity, Entry> entries;

    world::MemoryPart memory() {
        const std::scoped_lock lock(mutex);
        world::MemoryPart part{"csg", 0, {}, {}};
        std::uint64_t triangles = 0;
        for (const auto& [entity, entry] : entries) {
            part.bytes += bytes_of(*entry.mesh);
            triangles += entry.mesh->triangles;
        }
        part.counts = {{"meshes", entries.size()}, {"triangles", triangles}};
        return part;
    }
};

CsgCache* find_cache(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<CsgCache>>();
    return found != nullptr ? found->get() : nullptr;
}

// Reads models through the world's file reader, for worlds without install_csg.
ModelLoader file_loader(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const Model>> {
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            return Error{.code = ErrorCode::NotFound, .message = "models can't be read here"};
        }
        auto bytes = reader(path);
        if (!bytes) {
            return Error{.code = bytes.error().code,
                         .message = "\"" + path + "\" could not be read: " + bytes.error().message};
        }
        auto model = load_model(bytes.value(), path, reader);
        if (!model) {
            return model.error();
        }
        return std::make_shared<const Model>(std::move(model).value());
    };
}

CsgNode node_of(const world::World& world, const world::Tree3D& tree, entt::entity entity,
                const CsgShape& shape, const ModelLoader& load) {
    CsgNode node;
    node.entity = entity;
    node.shape = shape;
    const auto& ecs = world.ecs();
    if (const auto* transform = ecs.try_get<const world::Transform3D>(entity);
        transform != nullptr) {
        node.local = world::affine(*transform);
        for (const entt::entity child : transform->children) {
            if (!world.valid(child) || tree.parent(child) != entity) {
                continue;
            }
            const auto* child_transform = ecs.try_get<const world::Transform3D>(child);
            if (child_transform != nullptr && !child_transform->visible) {
                continue;
            }
            if (std::optional<CsgShape> child_shape = csg_shape_of(world, child); child_shape) {
                node.children.push_back(node_of(world, tree, child, *child_shape, load));
            }
        }
    }
    if (const auto* standard = ecs.try_get<const world::StandardMaterial3D>(entity);
        standard != nullptr) {
        node.material = *standard;
    }
    if (const auto* polygon = std::get_if<world::CSGPolygon3D>(&shape);
        polygon != nullptr && polygon->mode == "path") {
        const auto* path = world.valid(polygon->path_node)
                               ? ecs.try_get<const world::Path3D>(polygon->path_node)
                               : nullptr;
        if (polygon->path_node == entt::null) {
            node.path_error = "names no entity: name the entity with the Path3D to push the "
                              "polygon along";
        } else if (path == nullptr) {
            node.path_error = "\"" + std::string(world.id_of(polygon->path_node)) +
                              "\" has no Path3D: give it one, or name another entity";
        } else {
            node.path = *path;
            if (!polygon->path_local) {
                node.path_place = tree.global(polygon->path_node);
            }
        }
    }
    if (const auto* mesh = std::get_if<world::CSGMesh3D>(&shape); mesh != nullptr) {
        auto found = primitive_of(world, entity);
        const std::optional<Primitive> primitive =
            found ? found.value() : std::optional<Primitive>{};
        if (!found) {
            node.mesh_error = found.error().message;
        } else if (!mesh->mesh.empty()) {
            if (!is_project_path(mesh->mesh)) {
                node.mesh_error = "expected a path inside the project folder, separated by '/', "
                                  "with no \".\" or \"..\" parts";
            } else if (!is_model_path(mesh->mesh)) {
                node.mesh_error = "expected a .gltf, .glb, .obj, .fbx or .blend file";
            } else if (primitive) {
                node.mesh_error = "names a model, so its " +
                                  std::string(kPrimitiveNames[primitive->index()]) +
                                  " is left out: remove one of them";
            } else if (auto model = load(mesh->mesh); !model) {
                node.mesh_error = model.error().message;
            } else {
                node.model = model.value();
            }
        } else if (!primitive) {
            node.mesh_error = "names no model and the entity has no primitive mesh: name a model "
                              "file, or give it a BoxMesh, SphereMesh or another closed mesh";
        } else if (std::holds_alternative<world::PlaneMesh>(*primitive) ||
                   std::holds_alternative<world::QuadMesh>(*primitive)) {
            node.mesh_error = "its " + std::string(kPrimitiveNames[primitive->index()]) +
                              " is flat, so it has no inside: use a closed mesh such as a BoxMesh";
        } else if (auto checked = check_primitive(*primitive); !checked) {
            node.mesh_error = checked.error().message;
        } else {
            node.primitive = primitive;
        }
    }
    return node;
}

std::shared_ptr<const CsgMesh> make(const CsgNode& root) {
    std::vector<Problem> problems;
    const Brush brush = brush_of(root, problems);
    auto mesh = std::make_shared<CsgMesh>(mesh_of(brush, csg_settings(root.shape)));
    mesh->version = next_version.fetch_add(1);
    // An empty result, unless a shape under the root already says why.
    if (brush.faces.empty() && problems.empty()) {
        problems.push_back({root.entity, component_path(root.shape),
                            "makes an empty shape: shapes are usually empty because a mesh isn't "
                            "closed (manifold), or a subtraction or intersection leaves nothing"});
    }
    mesh->problems = std::move(problems);
    return mesh;
}

} // namespace

CsgSettings csg_settings(const CsgShape& shape) {
    return std::visit(
        [](const auto& s) {
            return CsgSettings{.operation = s.operation,
                               .calculate_tangents = s.calculate_tangents,
                               .use_collision = s.use_collision,
                               .collision_layer = s.collision_layer,
                               .collision_mask = s.collision_mask,
                               .layers = s.layers,
                               .cast_shadow = s.cast_shadow};
        },
        shape);
}

std::optional<CsgShape> csg_shape_of(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (!world.valid(entity)) {
        return std::nullopt;
    }
    if (const auto* s = ecs.try_get<const world::CSGCombiner3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGBox3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGSphere3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGCylinder3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGTorus3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGPolygon3D>(entity); s != nullptr) {
        return *s;
    }
    if (const auto* s = ecs.try_get<const world::CSGMesh3D>(entity); s != nullptr) {
        return *s;
    }
    return std::nullopt;
}

bool is_csg_root(const world::World& world, const world::Tree3D& tree, entt::entity entity) {
    if (!csg_shape_of(world, entity)) {
        return false;
    }
    const entt::entity parent = tree.parent(entity);
    return parent == entt::null || !csg_shape_of(world, parent);
}

std::vector<entt::entity> csg_roots(const world::World& world, const world::Tree3D& tree) {
    std::vector<entt::entity> roots;
    for (const entt::entity entity : tree.order()) {
        if (is_csg_root(world, tree, entity)) {
            roots.push_back(entity);
        }
    }
    std::vector<entt::entity> loose;
    const auto& ecs = world.ecs();
    const auto add_loose = [&](auto view) {
        for (const entt::entity entity : view) {
            if (!ecs.all_of<world::Transform3D>(entity) &&
                std::ranges::find(loose, entity) == loose.end()) {
                loose.push_back(entity);
            }
        }
    };
    add_loose(ecs.view<const world::CSGCombiner3D>());
    add_loose(ecs.view<const world::CSGBox3D>());
    add_loose(ecs.view<const world::CSGSphere3D>());
    add_loose(ecs.view<const world::CSGCylinder3D>());
    add_loose(ecs.view<const world::CSGTorus3D>());
    add_loose(ecs.view<const world::CSGPolygon3D>());
    add_loose(ecs.view<const world::CSGMesh3D>());
    std::ranges::sort(loose, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    roots.insert(roots.end(), loose.begin(), loose.end());
    return roots;
}

std::shared_ptr<const CsgMesh> csg_mesh(const world::World& world, const world::Tree3D& tree,
                                        entt::entity root) {
    const std::optional<CsgShape> shape = csg_shape_of(world, root);
    if (!shape) {
        auto empty = std::make_shared<CsgMesh>();
        empty->collision = std::make_shared<const std::vector<Vec3>>();
        return empty;
    }
    CsgCache* cache = find_cache(world);
    if (cache == nullptr) {
        return make(node_of(world, tree, root, *shape, file_loader(world)));
    }
    const std::scoped_lock lock(cache->mutex);
    CsgNode input = node_of(world, tree, root, *shape, cache->load);
    // Where the root is plays no part in its mesh.
    input.local = {};
    if (const auto found = cache->entries.find(root);
        found != cache->entries.end() && found->second.input == input) {
        return found->second.mesh;
    }
    std::shared_ptr<const CsgMesh> mesh = make(input);
    // Roots that are gone go with it.
    std::erase_if(cache->entries, [&](const auto& entry) {
        return !world.valid(entry.first) || !csg_shape_of(world, entry.first);
    });
    cache->entries[root] = {.input = std::move(input), .mesh = mesh};
    return mesh;
}

void install_csg(world::World& world, ModelLoader load) {
    if (CsgCache* cache = find_cache(world); cache != nullptr) {
        const std::scoped_lock lock(cache->mutex);
        cache->load = std::move(load);
        cache->entries.clear();
        return;
    }
    auto cache = std::make_shared<CsgCache>();
    cache->load = std::move(load);
    world.ecs().ctx().emplace<std::shared_ptr<CsgCache>>(cache);
    world.add_reset([cache](world::World& /*world*/) {
        const std::scoped_lock lock(cache->mutex);
        cache->entries.clear();
    });
    world.add_memory_source("csg",
                            [cache](const world::World& /*world*/) { return cache->memory(); });
}

std::vector<CsgBody> csg_bodies(const world::World& world, const world::Tree3D& tree) {
    std::vector<CsgBody> bodies;
    for (const entt::entity root : csg_roots(world, tree)) {
        const std::optional<CsgShape> shape = csg_shape_of(world, root);
        if (!shape || !csg_settings(*shape).use_collision) {
            continue;
        }
        const CsgSettings settings = csg_settings(*shape);
        const std::shared_ptr<const CsgMesh> mesh = csg_mesh(world, tree, root);
        if (mesh->collision->empty()) {
            continue;
        }
        bodies.push_back({.entity = root,
                          .layer = settings.collision_layer,
                          .mask = settings.collision_mask,
                          .faces = mesh->collision});
    }
    return bodies;
}

std::vector<Problem> check_csg(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    std::vector<entt::entity> shapes;
    if (const auto* entities = ecs.storage<entt::entity>(); entities != nullptr) {
        for (const entt::entity entity : *entities) {
            if (world.valid(entity) && csg_shape_of(world, entity)) {
                shapes.push_back(entity);
            }
        }
    }
    std::ranges::sort(shapes, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : shapes) {
        std::vector<std::string_view> names;
        const auto has = [&](auto tag, std::string_view name) {
            using T = typename decltype(tag)::type;
            if (ecs.all_of<T>(entity)) {
                names.push_back(name);
            }
        };
        has(std::type_identity<world::CSGCombiner3D>{}, kCsgNames[0]);
        has(std::type_identity<world::CSGBox3D>{}, kCsgNames[1]);
        has(std::type_identity<world::CSGSphere3D>{}, kCsgNames[2]);
        has(std::type_identity<world::CSGCylinder3D>{}, kCsgNames[3]);
        has(std::type_identity<world::CSGTorus3D>{}, kCsgNames[4]);
        has(std::type_identity<world::CSGPolygon3D>{}, kCsgNames[5]);
        has(std::type_identity<world::CSGMesh3D>{}, kCsgNames[6]);
        if (names.size() > 1) {
            std::string list;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i > 0) {
                    list += i + 1 == names.size() ? " and " : ", ";
                }
                list += names[i];
            }
            problems.push_back(
                {entity, "/components", "has more than one CSG shape (" + list + "): keep one"});
        }
        if (ecs.all_of<world::MeshInstance3D>(entity)) {
            problems.push_back({entity, "/components/MeshInstance3D",
                                "is on a CSG shape, which draws itself: remove the "
                                "MeshInstance3D"});
        }
    }
    const world::Tree3D tree(world);
    std::vector<Problem> made;
    for (const entt::entity root : csg_roots(world, tree)) {
        const std::shared_ptr<const CsgMesh> mesh = csg_mesh(world, tree, root);
        made.insert(made.end(), mesh->problems.begin(), mesh->problems.end());
    }
    std::ranges::stable_sort(
        made, {}, [](const Problem& problem) { return entt::to_entity(problem.entity); });
    problems.insert(problems.end(), made.begin(), made.end());
    std::ranges::stable_sort(
        problems, {}, [](const Problem& problem) { return entt::to_entity(problem.entity); });
    return problems;
}

std::vector<int> triangulate(const std::vector<Vec2>& contour) {
    std::vector<int> result;
    const auto n = static_cast<int>(contour.size());
    if (n < 3) {
        return result;
    }
    // A counter-clockwise polygon in vs.
    std::vector<int> vs(static_cast<std::size_t>(n));
    const bool counter_clockwise = 0.0F < area(contour);
    for (int v = 0; v < n; ++v) {
        vs[static_cast<std::size_t>(v)] = counter_clockwise ? v : (n - 1) - v;
    }
    bool relaxed = false;
    int nv = n;
    // Removes nv - 2 corners, a triangle each.
    int count = 2 * nv;
    for (int v = nv - 1; nv > 2;) {
        // Looping means it is probably not a simple polygon.
        if (0 >= (count--)) {
            if (relaxed) {
                return {};
            }
            // Corners in a line can stop the strict checks; flat triangles beat failing.
            count = 2 * nv;
            relaxed = true;
        }
        int u = v;
        if (nv <= u) {
            u = 0;
        }
        v = u + 1;
        if (nv <= v) {
            v = 0;
        }
        int w = v + 1;
        if (nv <= w) {
            w = 0;
        }
        if (snip(contour, u, v, w, nv, vs, relaxed)) {
            result.push_back(vs[static_cast<std::size_t>(u)]);
            result.push_back(vs[static_cast<std::size_t>(v)]);
            result.push_back(vs[static_cast<std::size_t>(w)]);
            for (int s = v, t = v + 1; t < nv; ++s, ++t) {
                vs[static_cast<std::size_t>(s)] = vs[static_cast<std::size_t>(t)];
            }
            --nv;
            count = 2 * nv;
        }
    }
    return result;
}

} // namespace slash::scene3d
