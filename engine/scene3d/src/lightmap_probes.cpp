// A LightmapGI's light probes, as Godot 4.6's: where they go (LightmapGI::bake's octree), the
// Delaunay tetrahedra between them (core/math/delaunay_3d.h), the BSP tree that finds a point's
// tetrahedron (LightmapGI::_compute_bsp_tree), the light at a point (LightStorage::
// lightmap_tap_sh_light) and the light a moving mesh takes (RendererSceneCull::
// _update_instance_lightmap_captures). Godot's code is MIT; its triangle and box test is Tomas
// Akenine-Möller's and its spherical harmonics rotation John Hable's, and the exact in-sphere test
// uses Jonathan Shewchuk's expansion arithmetic, all public domain.

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// Godot's CMP_EPSILON.
constexpr float kEpsilon = 0.00001F;

using Point = std::array<float, 3>;

Vec3 vec(const Point& p) {
    return {p[0], p[1], p[2]};
}

float component(Vec3 v, int axis) {
    if (axis == 0) {
        return v.x;
    }
    return axis == 1 ? v.y : v.z;
}

void set_component(Vec3& v, int axis, float value) {
    if (axis == 0) {
        v.x = value;
    } else if (axis == 1) {
        v.y = value;
    } else {
        v.z = value;
    }
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// As Godot's Vector3::normalize: zero stays zero.
Vec3 normalized(Vec3 v) {
    const float squared = dot(v, v);
    if (squared == 0.0F) {
        return {};
    }
    const float l = std::sqrt(squared);
    return {v.x / l, v.y / l, v.z / l};
}

Vec3 divide(Vec3 a, Vec3 b) {
    return {a.x / b.x, a.y / b.y, a.z / b.z};
}

Vec3 multiply(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// As Godot's Math::is_equal_approx.
bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    float tolerance = kEpsilon * std::abs(a);
    if (tolerance < kEpsilon) {
        tolerance = kEpsilon;
    }
    return std::abs(a - b) < tolerance;
}

bool equal_approx(Vec3 a, Vec3 b) {
    return equal_approx(a.x, b.x) && equal_approx(a.y, b.y) && equal_approx(a.z, b.z);
}

// Godot's Vector3i(Vector3): each component truncated toward zero. Values no int holds, which
// Godot leaves undefined, are clamped first.
int truncate(float v) {
    if (!(std::abs(v) < 1.0e9F)) {
        if (std::isnan(v)) {
            return 0;
        }
        return v < 0.0F ? -1000000000 : 1000000000;
    }
    return static_cast<int>(v);
}

// ---- Planes, as Godot's

struct Plane {
    Vec3 normal;
    float d = 0.0F;

    // Plane(p1, p2, p3), clockwise, as Godot's.
    static Plane through(Vec3 a, Vec3 b, Vec3 c) {
        Plane out;
        out.normal = normalized(cross(a - c, a - b));
        out.d = dot(out.normal, a);
        return out;
    }

    [[nodiscard]] float distance_to(Vec3 p) const { return dot(normal, p) - d; }
    [[nodiscard]] bool is_point_over(Vec3 p) const { return dot(normal, p) > d; }
    [[nodiscard]] bool has_point(Vec3 p, float tolerance) const {
        return std::abs(dot(normal, p) - d) <= tolerance;
    }
    [[nodiscard]] bool equal_approx_any_side(const Plane& o) const {
        return (equal_approx(normal, o.normal) && equal_approx(d, o.d)) ||
               (equal_approx(normal, o.normal * -1.0F) && equal_approx(d, -o.d));
    }
    [[nodiscard]] bool empty() const { return normal == Vec3{} && d == 0.0F; }
};

// ---- Placing probes: LightmapGI::bake's octree

// An AABB, as Godot's.
struct Box {
    Vec3 position;
    Vec3 size;

    void expand_to(Vec3 p) {
        Vec3 begin = position;
        Vec3 end = position + size;
        begin = {std::min(begin.x, p.x), std::min(begin.y, p.y), std::min(begin.z, p.z)};
        end = {std::max(end.x, p.x), std::max(end.y, p.y), std::max(end.z, p.z)};
        position = begin;
        size = end - begin;
    }
    void grow_by(float amount) {
        position = position - Vec3{amount, amount, amount};
        size = size + Vec3{2.0F * amount, 2.0F * amount, 2.0F * amount};
    }
    [[nodiscard]] int longest_axis() const {
        int axis = 0;
        float most = size.x;
        if (size.y > most) {
            axis = 1;
            most = size.y;
        }
        if (size.z > most) {
            axis = 2;
        }
        return axis;
    }
    [[nodiscard]] Vec3 endpoint(int i) const {
        return {(i & 4) != 0 ? position.x + size.x : position.x,
                (i & 2) != 0 ? position.y + size.y : position.y,
                (i & 1) != 0 ? position.z + size.z : position.z};
    }
    [[nodiscard]] bool has_point(Vec3 p) const {
        return p.x >= position.x && p.y >= position.y && p.z >= position.z &&
               p.x <= position.x + size.x && p.y <= position.y + size.y &&
               p.z <= position.z + size.z;
    }
};

// Geometry3D::planeBoxOverlap.
bool plane_box_overlap(Vec3 normal, float d, Vec3 max_box) {
    Vec3 vmin;
    Vec3 vmax;
    for (int q = 0; q < 3; ++q) {
        const float m = component(max_box, q);
        if (component(normal, q) > 0.0F) {
            set_component(vmin, q, -m);
            set_component(vmax, q, m);
        } else {
            set_component(vmin, q, m);
            set_component(vmax, q, -m);
        }
    }
    if (dot(normal, vmin) + d > 0.0F) {
        return false;
    }
    return dot(normal, vmax) + d >= 0.0F;
}

// Geometry3D::triangle_box_overlap: Tomas Akenine-Möller's separating axis test.
bool triangle_box_overlap(Vec3 center, Vec3 half, const std::array<Vec3, 3>& triangle) {
    const Vec3 v0 = triangle[0] - center;
    const Vec3 v1 = triangle[1] - center;
    const Vec3 v2 = triangle[2] - center;
    const Vec3 e0 = v1 - v0;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = v0 - v2;
    // A test along one edge's cross product with an axis: pa and pb are the two vertices'
    // projections, r the box's.
    const auto separated = [](float pa, float pb, float r) {
        const float lo = std::min(pa, pb);
        const float hi = std::max(pa, pb);
        return lo > r || hi < -r;
    };
    const auto x_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((a * p.y) - (b * p.z), (a * q.y) - (b * q.z),
                         (fa * half.y) + (fb * half.z));
    };
    const auto y_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((-a * p.x) + (b * p.z), (-a * q.x) + (b * q.z),
                         (fa * half.x) + (fb * half.z));
    };
    const auto z_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((a * p.x) - (b * p.y), (a * q.x) - (b * q.y),
                         (fa * half.x) + (fb * half.y));
    };
    Vec3 f{std::abs(e0.x), std::abs(e0.y), std::abs(e0.z)};
    if (x_test(e0.z, e0.y, f.z, f.y, v0, v2) || y_test(e0.z, e0.x, f.z, f.x, v0, v2) ||
        z_test(e0.y, e0.x, f.y, f.x, v1, v2)) {
        return false;
    }
    f = {std::abs(e1.x), std::abs(e1.y), std::abs(e1.z)};
    if (x_test(e1.z, e1.y, f.z, f.y, v0, v2) || y_test(e1.z, e1.x, f.z, f.x, v0, v2) ||
        z_test(e1.y, e1.x, f.y, f.x, v0, v1)) {
        return false;
    }
    f = {std::abs(e2.x), std::abs(e2.y), std::abs(e2.z)};
    if (x_test(e2.z, e2.y, f.z, f.y, v0, v1) || y_test(e2.z, e2.x, f.z, f.x, v0, v1) ||
        z_test(e2.y, e2.x, f.y, f.x, v1, v2)) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        const float a = component(v0, axis);
        const float b = component(v1, axis);
        const float c = component(v2, axis);
        const float h = component(half, axis);
        if (std::min({a, b, c}) > h || std::max({a, b, c}) < -h) {
            return false;
        }
    }
    const Vec3 normal = cross(e0, e1);
    return plane_box_overlap(normal, -dot(normal, v0), half);
}

// LightmapGI::GenProbesOctree: a cube of size cells at offset, its touched children made.
struct Octree {
    Vec3i offset;
    int size = 0;
    std::array<std::unique_ptr<Octree>, 8> children{};
};

Vec3i child_offset(const Octree& cell, int i, int step) {
    return {cell.offset.x + ((i & 1) != 0 ? step : 0), cell.offset.y + ((i & 2) != 0 ? step : 0),
            cell.offset.z + ((i & 4) != 0 ? step : 0)};
}

Vec3 to_vec(Vec3i v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// LightmapGI::_plot_triangle_into_octree.
void plot_triangle(Octree& cell, float cell_size, const std::array<Vec3, 3>& triangle) {
    const int half_size = cell.size / 2;
    for (int i = 0; i < 8; ++i) {
        const Vec3i pos = child_offset(cell, i, half_size);
        const Vec3 sub_position = to_vec(pos) * cell_size;
        const auto h = static_cast<float>(half_size);
        const Vec3 sub_size = Vec3{h, h, h} * cell_size;
        if (!triangle_box_overlap(sub_position + (sub_size * 0.5F), sub_size * 0.5F, triangle)) {
            continue;
        }
        auto& child = cell.children.at(static_cast<std::size_t>(i));
        if (!child) {
            child = std::make_unique<Octree>();
            child->offset = pos;
            child->size = half_size;
        }
        if (half_size > 1) {
            plot_triangle(*child, cell_size, triangle);
        }
    }
}

struct Vec3iLess {
    bool operator()(const Vec3i& a, const Vec3i& b) const {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.z < b.z;
    }
};

// LightmapGI::_gen_new_positions_from_octree.
void new_positions(const Octree& cell, float cell_size, std::span<const Point> user,
                   std::vector<Point>& out, std::map<Vec3i, bool, Vec3iLess>& used,
                   const Box& bounds) {
    for (int i = 0; i < 8; ++i) {
        const Vec3i pos = child_offset(cell, i, cell.size);
        if (cell.size == 1 && !used.contains(pos)) {
            const Vec3 real = bounds.position + (to_vec(pos) * cell_size);
            const bool exists = std::ranges::any_of(
                user, [&](const Point& p) { return length(vec(p) - real) < cell_size * 0.5F; });
            if (!exists) {
                out.push_back({real.x, real.y, real.z});
            }
            used[pos] = true;
        }
        if (const auto& child = cell.children.at(static_cast<std::size_t>(i))) {
            new_positions(*child, cell_size, user, out, used, bounds);
        }
    }
}

// ---- Exact signs, with Jonathan Shewchuk's expansion arithmetic (public domain)

// a + b, exactly, as sum + error.
void two_sum(double a, double b, double& sum, double& error) {
    sum = a + b;
    const double b_virtual = sum - a;
    const double a_virtual = sum - b_virtual;
    error = (a - a_virtual) + (b - b_virtual);
}

// a + b, exactly, where |a| >= |b|.
void fast_two_sum(double a, double b, double& sum, double& error) {
    sum = a + b;
    error = b - (sum - a);
}

// a * b, exactly, as product + error (Dekker's split: nothing here may be fused).
void two_product(double a, double b, double& product, double& error) {
    const auto split = [](double v, double& high, double& low) {
        const double c = 134217729.0 * v; // 2^27 + 1
        high = c - (c - v);
        low = v - high;
    };
    product = a * b;
    double a_high = 0.0;
    double a_low = 0.0;
    double b_high = 0.0;
    double b_low = 0.0;
    split(a, a_high, a_low);
    split(b, b_high, b_low);
    error =
        (a_low * b_low) - (((product - (a_high * b_high)) - (a_low * b_high)) - (a_high * b_low));
}

// A number as a sum of doubles that don't overlap, smallest first, with no zeros: its sign is
// its largest part's.
class Expansion {
public:
    // Shewchuk's grow_expansion_zeroelim.
    void add(double b) {
        double q = b;
        std::size_t out = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            double error = 0.0;
            two_sum(q, parts_.at(i), q, error);
            if (error != 0.0) {
                parts_.at(out++) = error;
            }
        }
        if (q != 0.0 || out == 0) {
            parts_.at(out++) = q;
        }
        size_ = out;
    }

    void add(const Expansion& e) {
        for (std::size_t i = 0; i < e.size_; ++i) {
            add(e.parts_.at(i));
        }
    }

    void add_product(double a, double b) {
        double product = 0.0;
        double error = 0.0;
        two_product(a, b, product, error);
        add(error);
        add(product);
    }

    // Shewchuk's scale_expansion_zeroelim.
    [[nodiscard]] Expansion scaled(double b) const {
        Expansion out;
        if (size_ == 0) {
            return out;
        }
        double q = 0.0;
        double error = 0.0;
        two_product(parts_[0], b, q, error);
        out.push(error);
        for (std::size_t i = 1; i < size_; ++i) {
            double high = 0.0;
            double low = 0.0;
            two_product(parts_.at(i), b, high, low);
            double sum = 0.0;
            two_sum(q, low, sum, error);
            out.push(error);
            fast_two_sum(high, sum, q, error);
            out.push(error);
        }
        if (q != 0.0 || out.size_ == 0) {
            out.parts_.at(out.size_++) = q;
        }
        return out;
    }

    [[nodiscard]] int sign() const {
        const double top = size_ == 0 ? 0.0 : parts_.at(size_ - 1);
        if (top > 0.0) {
            return 1;
        }
        return top < 0.0 ? -1 : 0;
    }

private:
    void push(double v) {
        if (v != 0.0) {
            parts_.at(size_++) = v;
        }
    }

    // A 4x4 determinant has at most 48 parts.
    std::array<double, 48> parts_{};
    std::size_t size_ = 0;
};

// The tetrahedralization's coordinates are all multiples of 2^-16 smaller than 2^8 (the points
// quantized into the unit cube, and the root simplex's corners), so a difference of two is an
// exact double, as are products of two differences, sums of three such products and 2x2 minors.
using Row = std::array<double, 3>;

Row difference(Vec3 a, Vec3 b) {
    return {static_cast<double>(a.x) - static_cast<double>(b.x),
            static_cast<double>(a.y) - static_cast<double>(b.y),
            static_cast<double>(a.z) - static_cast<double>(b.z)};
}

// The minors of det[u; v; w] along u, and their sizes: what bounds a sum's rounding.
struct Minors {
    std::array<double, 3> value{};
    std::array<double, 3> size{};
};

Minors minors(const Row& v, const Row& w) {
    Minors out;
    out.value = {(v[1] * w[2]) - (v[2] * w[1]), (v[2] * w[0]) - (v[0] * w[2]),
                 (v[0] * w[1]) - (v[1] * w[0])};
    out.size = {std::abs(v[1] * w[2]) + std::abs(v[2] * w[1]),
                std::abs(v[2] * w[0]) + std::abs(v[0] * w[2]),
                std::abs(v[0] * w[1]) + std::abs(v[1] * w[0])};
    return out;
}

// A sum far bigger than this times the sum of its terms' sizes has its sign; the rounding of
// sums of a dozen products is a few hundred times smaller.
constexpr double kRelativeError = 1.0e-12;

Expansion det3_exact(const Row& u, const Minors& m) {
    Expansion out;
    for (std::size_t i = 0; i < 3; ++i) {
        out.add_product(u.at(i), m.value.at(i));
    }
    return out;
}

// The sign of det[u; v; w].
int det3_sign(const Row& u, const Row& v, const Row& w) {
    const Minors m = minors(v, w);
    double approx = 0.0;
    double size = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        approx += u.at(i) * m.value.at(i);
        size += std::abs(u.at(i)) * m.size.at(i);
    }
    if (std::abs(approx) > size * kRelativeError) {
        return approx > 0.0 ? 1 : -1;
    }
    return det3_exact(u, m).sign();
}

// Whether e is inside the sphere through a, b, c and d, never on it: where Shewchuk's insphere
// and orient3d have the same sign. Godot works it out from the circumsphere in 128-bit fixed point
// with a tolerance, which lets tetrahedra overlap where points lie in flat layers or on a grid,
// as a LightmapGI's probes over a floor do; here it is exact.
bool in_sphere(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 e) {
    const int orientation = det3_sign(difference(a, d), difference(b, d), difference(c, d));
    if (orientation == 0) {
        return false;
    }
    const std::array<Row, 4> rows{difference(a, e), difference(b, e), difference(c, e),
                                  difference(d, e)};
    // Along the lifted column: -|b c d| la + |a c d| lb - |a b d| lc + |a b c| ld.
    constexpr std::array<std::array<std::size_t, 3>, 4> kOthers{
        {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}}};
    std::array<double, 4> lifted{};
    std::array<Minors, 4> parts{};
    double approx = 0.0;
    double size = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const Row& r = rows.at(i);
        lifted.at(i) = (r[0] * r[0]) + (r[1] * r[1]) + (r[2] * r[2]);
        if ((i % 2) == 0) {
            lifted.at(i) = -lifted.at(i);
        }
        const auto& others = kOthers.at(i);
        parts.at(i) = minors(rows.at(others[1]), rows.at(others[2]));
        const Row& u = rows.at(others[0]);
        double minor = 0.0;
        double minor_size = 0.0;
        for (std::size_t j = 0; j < 3; ++j) {
            minor += u.at(j) * parts.at(i).value.at(j);
            minor_size += std::abs(u.at(j)) * parts.at(i).size.at(j);
        }
        approx += lifted.at(i) * minor;
        size += std::abs(lifted.at(i)) * minor_size;
    }
    int inside = 0;
    if (std::abs(approx) > size * kRelativeError) {
        inside = approx > 0.0 ? 1 : -1;
    } else {
        Expansion exact;
        for (std::size_t i = 0; i < 4; ++i) {
            exact.add(det3_exact(rows.at(kOthers.at(i)[0]), parts.at(i)).scaled(lifted.at(i)));
        }
        inside = exact.sign();
    }
    return inside == orientation;
}

// ---- Delaunay3D

constexpr int kGrid = 16;                // ACCEL_GRID_SIZE
constexpr float kQuantization = 65536.0; // QUANTIZATION_MAX

struct Simplex {
    std::array<std::uint32_t, 4> points{};
    bool alive = true;
    std::uint32_t cells = 0; // the grid cells it is listed in
};

struct CellRange {
    Vec3i from;
    Vec3i to;
};

// The grid cells a simplex's circumsphere touches, as Godot's: Godot works the sphere in 128-bit
// fixed point; here it is in doubles, grown by their rounding error, and a simplex too flat to
// bound has every cell.
CellRange sphere_cells(const std::vector<Vec3>& points, const Simplex& s, Vec3 proportions) {
    constexpr CellRange kEvery{{0, 0, 0}, {kGrid - 1, kGrid - 1, kGrid - 1}};
    const Vec3 origin = points[s.points[0]];
    const std::array<Row, 3> rows{difference(points[s.points[1]], origin),
                                  difference(points[s.points[2]], origin),
                                  difference(points[s.points[3]], origin)};
    // The center c solves 2 (rows) c = (the rows' squared lengths): by its cofactors, exact here.
    std::array<std::array<double, 3>, 3> cofactors{};
    for (std::size_t m = 0; m < 3; ++m) {
        const Minors minor = minors(rows.at((m + 1) % 3), rows.at((m + 2) % 3));
        cofactors.at(m) = minor.value;
    }
    double det = 0.0;
    double det_size = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
        det += rows[0].at(k) * cofactors[0].at(k);
        det_size += std::abs(rows[0].at(k) * cofactors[0].at(k));
    }
    const double det_error = det_size * kRelativeError;
    if (!(std::abs(det) > 2.0 * det_error)) {
        return kEvery;
    }
    std::array<double, 3> squared{};
    for (std::size_t m = 0; m < 3; ++m) {
        const Row& r = rows.at(m);
        squared.at(m) = (r[0] * r[0]) + (r[1] * r[1]) + (r[2] * r[2]);
    }
    std::array<double, 3> center{};
    double radius2 = 0.0;
    double error = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
        double numerator = 0.0;
        double numerator_error = 0.0;
        for (std::size_t m = 0; m < 3; ++m) {
            numerator += cofactors.at(m).at(k) * squared.at(m);
            numerator_error += std::abs(cofactors.at(m).at(k) * squared.at(m));
        }
        numerator_error *= kRelativeError;
        const double offset = numerator / (2.0 * det);
        const double at = static_cast<double>(component(origin, static_cast<int>(k))) + offset;
        center.at(k) = at;
        radius2 += offset * offset;
        // |n'/2d' - n/2d| <= dn / 2|d'| + (|n'| + dn) dd / |d'|^2, with dd <= |d'| / 2.
        error += (numerator_error / (2.0 * std::abs(det))) +
                 ((std::abs(numerator) + numerator_error) * det_error / (det * det)) +
                 ((std::abs(at) + std::abs(offset)) * kRelativeError);
    }
    const double extents = (std::sqrt(radius2) * (1.0 + kRelativeError)) + error + 0.0001;
    const auto index = [](double v) {
        const double clamped = std::clamp(v, -1.0, static_cast<double>(kGrid));
        return std::clamp(static_cast<int>(clamped), 0, kGrid - 1);
    };
    CellRange out;
    std::array<int, 3> from{};
    std::array<int, 3> to{};
    for (std::size_t k = 0; k < 3; ++k) {
        const double scale = static_cast<double>(component(proportions, static_cast<int>(k))) *
                             static_cast<double>(kGrid);
        from.at(k) = index((center.at(k) - extents) * scale);
        to.at(k) = index((center.at(k) + extents) * scale);
    }
    out.from = {from[0], from[1], from[2]};
    out.to = {to[0], to[1], to[2]};
    return out;
}

bool simplex_is_coplanar(std::span<const Point> points, const std::array<std::uint32_t, 4>& s) {
    for (std::size_t i = 0; i < 4; ++i) {
        const Plane p = Plane::through(vec(points[s.at(i)]), vec(points[s.at((i + 1) % 4)]),
                                       vec(points[s.at((i + 2) % 4)]));
        if (std::abs(p.distance_to(vec(points[s.at((i + 3) % 4)]))) < 0.001F) {
            return true;
        }
    }
    return false;
}

// ---- The BSP tree

struct BspSimplex {
    std::array<std::uint32_t, 4> vertices{};
    std::array<std::uint32_t, 4> planes{};
};

constexpr std::array<std::array<int, 3>, 4> kFaceOrder{
    {{0, 1, 2}, {0, 2, 3}, {0, 1, 3}, {1, 2, 3}}};

// Finds a plane already kept that equals one on either side, as Godot's linear search for
// is_equal_approx_any_side, by the normal's cell: every plane is kept under its normal's cell and
// its flipped normal's, and a normal near a cell's edge looks in the cells beside it too.
class PlaneIndex {
public:
    // The first kept plane equal to p, or the index p is kept at.
    std::uint32_t find_or_add(const Plane& p, std::vector<Plane>& planes) {
        std::uint32_t found = std::numeric_limits<std::uint32_t>::max();
        for_cells(p.normal, [&](std::uint64_t key) {
            const auto it = cells_.find(key);
            if (it == cells_.end()) {
                return;
            }
            for (const std::uint32_t index : it->second) {
                if (index < found && planes[index].equal_approx_any_side(p)) {
                    found = index;
                }
            }
        });
        if (found != std::numeric_limits<std::uint32_t>::max()) {
            return found;
        }
        const auto index = static_cast<std::uint32_t>(planes.size());
        planes.push_back(p);
        cells_[key_of(cell_of(p.normal))].push_back(index);
        const Vec3i flipped = cell_of(p.normal * -1.0F);
        if (key_of(flipped) != key_of(cell_of(p.normal))) {
            cells_[key_of(flipped)].push_back(index);
        }
        return index;
    }

private:
    static constexpr float kCell = 0.001F;

    static Vec3i cell_of(Vec3 n) {
        return {truncate(std::floor(n.x / kCell)), truncate(std::floor(n.y / kCell)),
                truncate(std::floor(n.z / kCell))};
    }
    static std::uint64_t key_of(Vec3i c) {
        const auto part = [](int v) { return static_cast<std::uint64_t>(v + 4096) & 0x1FFFFFU; };
        return (part(c.x) << 42U) | (part(c.y) << 21U) | part(c.z);
    }
    // Each cell a normal equal to n (within Godot's tolerance) may be kept under.
    template <typename F>
    static void for_cells(Vec3 n, F&& visit) {
        const Vec3i c = cell_of(n);
        std::array<std::array<int, 2>, 3> ranges{};
        for (int axis = 0; axis < 3; ++axis) {
            const float v = component(n, axis);
            const int at = std::array<int, 3>{c.x, c.y, c.z}.at(static_cast<std::size_t>(axis));
            const float low = static_cast<float>(at) * kCell;
            ranges.at(static_cast<std::size_t>(axis)) = {
                v - low < 2.0F * kEpsilon ? at - 1 : at,
                (low + kCell) - v < 2.0F * kEpsilon ? at + 1 : at};
        }
        for (int x = ranges[0][0]; x <= ranges[0][1]; ++x) {
            for (int y = ranges[1][0]; y <= ranges[1][1]; ++y) {
                for (int z = ranges[2][0]; z <= ranges[2][1]; ++z) {
                    visit(key_of({x, y, z}));
                }
            }
        }
    }

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

class BspBuilder {
public:
    BspBuilder(std::span<const Point> points, std::vector<BspSimplex> simplices,
               std::size_t plane_count, jobs::Pool* pool)
        : points_(points), simplices_(std::move(simplices)),
          planes_tested_(plane_count, std::numeric_limits<std::int32_t>::max()), pool_(pool) {}

    std::vector<LightmapBspNode> build() {
        std::vector<std::int32_t> indices(simplices_.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<std::int32_t>(i);
        }
        if (indices.size() >= 2) {
            compute(indices);
        }
        return std::move(nodes_);
    }

private:
    // LightmapGI::_bsp_get_simplex_side: 1 all over, -1 all under, 0 crossing, -2 flat.
    [[nodiscard]] int side(const Plane& plane, std::int32_t simplex) const {
        int over = 0;
        int under = 0;
        const BspSimplex& s = simplices_[static_cast<std::size_t>(simplex)];
        for (const std::uint32_t vertex : s.vertices) {
            const Vec3 v = vec(points_[vertex]);
            // Godot's tolerance, found on scenes up to 1000x1000x100 meters.
            if (plane.has_point(v, 1.0F / static_cast<float>(1 << 13))) {
                // Coplanar.
            } else if (plane.is_point_over(v)) {
                ++over;
            } else {
                ++under;
            }
        }
        if (under == 0 && over == 0) {
            return -2;
        }
        if (under == 0) {
            return 1;
        }
        return over == 0 ? -1 : 0;
    }

    [[nodiscard]] Plane face_plane(std::int32_t simplex, std::size_t face) const {
        const BspSimplex& s = simplices_[static_cast<std::size_t>(simplex)];
        const auto& order = kFaceOrder.at(face);
        return Plane::through(vec(points_[s.vertices.at(static_cast<std::size_t>(order[0]))]),
                              vec(points_[s.vertices.at(static_cast<std::size_t>(order[1]))]),
                              vec(points_[s.vertices.at(static_cast<std::size_t>(order[2]))]));
    }

    // A plane's score at a node: how evenly it splits the simplices and how few it crosses, or
    // -1 where it splits none.
    [[nodiscard]] float score(const Plane& plane, const std::vector<std::int32_t>& indices) const {
        int over_count = 0;
        int under_count = 0;
        for (const std::int32_t index : indices) {
            const int s = side(plane, index);
            if (s == -2) {
                continue;
            }
            if (s < 0) {
                ++under_count;
            } else if (s > 0) {
                ++over_count;
            }
        }
        if (under_count == 0 && over_count == 0) {
            return -1.0F;
        }
        if (under_count > over_count) {
            std::swap(under_count, over_count);
        }
        if (over_count == 0) {
            return 0.0F;
        }
        const float balance = static_cast<float>(under_count) / static_cast<float>(over_count);
        const float separation =
            static_cast<float>(over_count + under_count) / static_cast<float>(indices.size());
        return balance * separation * separation;
    }

    std::int32_t compute(const std::vector<std::int32_t>& indices) {
        const auto node_index = static_cast<std::int32_t>(nodes_.size());
        nodes_.emplace_back();

        // The simplices' faces not yet tried at this node, in Godot's order; their scores are
        // worked out together and the first best wins, as Godot's loop picks it.
        std::vector<Plane> candidates;
        for (const std::int32_t idx : indices) {
            const BspSimplex& s = simplices_[static_cast<std::size_t>(idx)];
            for (std::size_t j = 0; j < 4; ++j) {
                std::int32_t& tested = planes_tested_[s.planes.at(j)];
                if (tested == node_index) {
                    continue;
                }
                tested = node_index;
                candidates.push_back(face_plane(idx, j));
            }
        }
        std::vector<float> scores(candidates.size());
        const auto score_one = [&](std::size_t i) { scores[i] = score(candidates[i], indices); };
        if (pool_ != nullptr && candidates.size() > 64) {
            pool_->parallel_for(candidates.size(), score_one);
        } else {
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                score_one(i);
            }
        }
        Plane best_plane;
        float best_score = -1.0F;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            // A plane that splits nothing, most likely a flat simplex's, isn't tried.
            if (scores[i] >= 0.0F && scores[i] > best_score) {
                best_plane = candidates[i];
                best_score = scores[i];
            }
        }

        // Two (rarely three) simplices that are apart or share one vertex may have no separating
        // plane among their faces: try planes through one vertex of the first and two of the
        // second.
        if (best_score == 0.0F) {
            const BspSimplex& s0 = simplices_[static_cast<std::size_t>(indices[0])];
            const BspSimplex& s1 = simplices_[static_cast<std::size_t>(indices[1])];
            for (std::size_t i = 0; i < 4 && best_score == 0.0F; ++i) {
                const Vec3 v0 = vec(points_[s0.vertices.at(i)]);
                for (std::size_t j = 0; j < 3 && best_score == 0.0F; ++j) {
                    if (s0.vertices.at(i) == s1.vertices.at(j)) {
                        break;
                    }
                    const Vec3 v1 = vec(points_[s1.vertices.at(j)]);
                    for (std::size_t k = j + 1; k < 4; ++k) {
                        if (s0.vertices.at(i) == s1.vertices.at(k)) {
                            break;
                        }
                        const Plane plane = Plane::through(v0, v1, vec(points_[s1.vertices.at(k)]));
                        if (plane.empty()) {
                            continue; // collinear
                        }
                        const int side0 = side(plane, indices[0]);
                        const int side1 = side(plane, indices[1]);
                        if ((side0 == 1 && side1 == -1) || (side0 == -1 && side1 == 1)) {
                            best_plane = plane;
                            best_score = 1.0F;
                            break;
                        }
                    }
                }
            }
        }

        std::vector<std::int32_t> over;
        std::vector<std::int32_t> under;
        for (const std::int32_t index : indices) {
            const int s = side(best_plane, index);
            if (s == -2) {
                continue; // it lies on the plane
            }
            if (s <= 0) {
                under.push_back(index);
            }
            if (s >= 0) {
                over.push_back(index);
            }
        }

        if (best_score < 0.0F || over.size() == indices.size() || under.size() == indices.size()) {
            // No face separates them, so the triangulation broke somewhere: split off the simplex
            // lowest along the longest axis, as Godot does. (Godot keeps only the first center and
            // puts its position in the list rather than its index; this takes every center.)
            std::vector<Vec3> centers;
            Box all;
            for (std::size_t i = 0; i < indices.size(); ++i) {
                Box box;
                const BspSimplex& s = simplices_[static_cast<std::size_t>(indices[i])];
                for (std::size_t j = 0; j < 4; ++j) {
                    const Vec3 p = vec(points_[s.vertices.at(j)]);
                    if (j == 0) {
                        box.position = p;
                    } else {
                        box.expand_to(p);
                    }
                }
                centers.push_back(box.position + (box.size * 0.5F));
                if (i == 0) {
                    all = box;
                } else {
                    all.expand_to(box.position);
                    all.expand_to(box.position + box.size);
                }
            }
            const int axis = all.longest_axis();
            std::size_t min_index = 0;
            float min_distance = 1e20F;
            for (std::size_t i = 0; i < centers.size(); ++i) {
                if (component(centers[i], axis) < min_distance) {
                    min_index = i;
                    min_distance = component(centers[i], axis);
                }
            }
            best_plane = Plane{};
            set_component(best_plane.normal, axis, 1.0F);
            best_plane.d = min_distance;
            under = {indices[min_index]};
            over.clear();
            for (std::size_t i = 0; i < indices.size(); ++i) {
                if (i != min_index) {
                    over.push_back(indices[i]);
                }
            }
        }

        LightmapBspNode node;
        node.plane = {best_plane.normal.x, best_plane.normal.y, best_plane.normal.z, best_plane.d};
        node.under = side_of(under);
        node.over = side_of(over);
        nodes_[static_cast<std::size_t>(node_index)] = node;
        return node_index;
    }

    std::int32_t side_of(const std::vector<std::int32_t>& indices) {
        if (indices.empty()) {
            return kLightmapBspEmpty;
        }
        if (indices.size() == 1) {
            return -(indices[0] + 1);
        }
        return compute(indices);
    }

    std::span<const Point> points_;
    std::vector<BspSimplex> simplices_;
    std::vector<std::int32_t> planes_tested_;
    jobs::Pool* pool_;
    std::vector<LightmapBspNode> nodes_;
};

// Geometry3D::tetrahedron_get_barycentric_coords.
std::array<float, 4> barycentric(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 p) {
    const Vec3 vap = p - a;
    const Vec3 vbp = p - b;
    const Vec3 vab = b - a;
    const Vec3 vac = c - a;
    const Vec3 vad = d - a;
    const Vec3 vbc = c - b;
    const Vec3 vbd = d - b;
    const auto stp = [](Vec3 x, Vec3 y, Vec3 z) { return dot(x, cross(y, z)); };
    const float va6 = stp(vbp, vbd, vbc);
    const float vb6 = stp(vap, vac, vad);
    const float vc6 = stp(vap, vad, vab);
    const float vd6 = stp(vap, vab, vac);
    const float v6 = 1.0F / stp(vab, vac, vad);
    return {va6 * v6, vb6 * v6, vc6 * v6, vd6 * v6};
}

} // namespace

LightmapProbes place_lightmap_probes(std::span<const std::array<float, 3>> triangles,
                                     std::span<const std::array<float, 3>> user, int subdiv) {
    LightmapProbes out;
    Box bounds;
    bool first = true;
    for (const Point& p : triangles) {
        if (first) {
            bounds.position = vec(p);
            first = false;
        } else {
            bounds.expand_to(vec(p));
        }
    }
    // The bounds hold the LightmapProbes too.
    for (const Point& p : user) {
        if (first) {
            bounds.position = vec(p);
            first = false;
        } else {
            bounds.expand_to(vec(p));
        }
    }
    bounds.grow_by(length(bounds.size) * 0.001F);

    std::vector<Point> placed;
    if (subdiv <= 0) {
        for (int i = 0; i < 8; ++i) {
            const Vec3 p = bounds.endpoint(i);
            placed.push_back({p.x, p.y, p.z});
        }
    } else {
        const int longest = bounds.longest_axis();
        const float cell_size = component(bounds.size, longest) / static_cast<float>(subdiv);
        std::array<int, 3> limit{};
        limit.at(static_cast<std::size_t>(longest)) = subdiv;
        for (const int axis : {(longest + 1) % 3, (longest + 2) % 3}) {
            limit.at(static_cast<std::size_t>(axis)) =
                static_cast<int>(std::ceil(component(bounds.size, axis) / cell_size));
            // The bounds grow to whole cells.
            set_component(bounds.size, axis,
                          static_cast<float>(limit.at(static_cast<std::size_t>(axis))) * cell_size);
        }
        Octree octree;
        octree.size = subdiv;
        for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
            const std::array<Vec3, 3> triangle{vec(triangles[i]) - bounds.position,
                                               vec(triangles[i + 1]) - bounds.position,
                                               vec(triangles[i + 2]) - bounds.position};
            plot_triangle(octree, cell_size, triangle);
        }
        std::map<Vec3i, bool, Vec3iLess> used;
        // The bounds' corners first.
        for (int i = 0; i < 8; ++i) {
            const Vec3i pos{(i & 1) != 0 ? limit[0] : 0, (i & 2) != 0 ? limit[1] : 0,
                            (i & 4) != 0 ? limit[2] : 0};
            used[pos] = true;
            const Vec3 real = bounds.position + (to_vec(pos) * cell_size);
            placed.push_back({real.x, real.y, real.z});
        }
        // The first level is left out: its corners are the bounds', above.
        for (const auto& child : octree.children) {
            if (child) {
                new_positions(*child, cell_size, user, placed, used, bounds);
            }
        }
    }
    // Godot adds the LightmapProbes first and the generated ones after.
    out.points.assign(user.begin(), user.end());
    out.points.insert(out.points.end(), placed.begin(), placed.end());
    out.bounds_position = {bounds.position.x, bounds.position.y, bounds.position.z};
    out.bounds_size = {bounds.size.x, bounds.size.y, bounds.size.z};
    return out;
}

std::vector<std::array<std::uint32_t, 4>>
tetrahedralize(std::span<const std::array<float, 3>> points) {
    const auto point_count = static_cast<std::uint32_t>(points.size());
    if (point_count < 4) {
        return {};
    }
    std::vector<Vec3> quantized(point_count + 4);
    Box rect;
    for (std::uint32_t i = 0; i < point_count; ++i) {
        if (i == 0) {
            rect.position = vec(points[i]);
        } else {
            rect.expand_to(vec(points[i]));
        }
    }
    const float longest = component(rect.size, rect.longest_axis());
    const Vec3 proportions = divide(Vec3{longest, longest, longest}, rect.size);
    for (std::uint32_t i = 0; i < point_count; ++i) {
        // Scaled to the unit cube and quantized, so the triangulation holds steady over a wide
        // range of distances and the in-sphere test's arithmetic is exact.
        const Vec3 scaled = vec(points[i]) - rect.position;
        const Vec3 unit{scaled.x / longest, scaled.y / longest, scaled.z / longest};
        const Vec3 q = unit * kQuantization;
        quantized[i] = {static_cast<float>(truncate(q.x)) / kQuantization,
                        static_cast<float>(truncate(q.y)) / kQuantization,
                        static_cast<float>(truncate(q.z)) / kQuantization};
    }
    // The root simplex is huge, so that the triangulation comes out convex.
    const auto delta_max = static_cast<float>(std::numbers::sqrt2 * 100.0);
    const Vec3 center{0.5F, 0.5F, 0.5F};
    quantized[point_count + 0] = center + (Vec3{0.0F, 1.0F, 0.0F} * delta_max);
    quantized[point_count + 1] = center + (Vec3{0.0F, -1.0F, 1.0F} * delta_max);
    quantized[point_count + 2] = center + (Vec3{1.0F, -1.0F, -1.0F} * delta_max);
    quantized[point_count + 3] = center + (Vec3{-1.0F, -1.0F, -1.0F} * delta_max);

    // The acceleration grid lists each simplex in the cells its circumsphere touches, in the
    // order they were made, as Godot's lists do. A simplex that goes leaves the cell looked in at
    // once and the others as they are next looked in, or all at once when they hold too many.
    std::vector<Simplex> simplices;
    std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(kGrid * kGrid * kGrid));
    std::size_t listed = 0;      // grid entries
    std::size_t listed_live = 0; // grid entries of simplices still there
    const auto cell = [&](Vec3i p) -> std::vector<std::uint32_t>& {
        const int index = (((p.x * kGrid) + p.y) * kGrid) + p.z;
        return grid[static_cast<std::size_t>(index)];
    };
    const auto make = [&](std::array<std::uint32_t, 4> corners, bool everywhere) {
        const auto index = static_cast<std::uint32_t>(simplices.size());
        simplices.push_back({corners});
        const CellRange range = everywhere ? CellRange{{0, 0, 0}, {kGrid - 1, kGrid - 1, kGrid - 1}}
                                           : sphere_cells(quantized, simplices.back(), proportions);
        std::uint32_t count = 0;
        for (int x = range.from.x; x <= range.to.x; ++x) {
            for (int y = range.from.y; y <= range.to.y; ++y) {
                for (int z = range.from.z; z <= range.to.z; ++z) {
                    cell({x, y, z}).push_back(index);
                    ++count;
                }
            }
        }
        simplices.back().cells = count;
        listed += count;
        listed_live += count;
    };
    const auto clamp_cell = [](Vec3i p) {
        return Vec3i{std::clamp(p.x, 0, kGrid - 1), std::clamp(p.y, 0, kGrid - 1),
                     std::clamp(p.z, 0, kGrid - 1)};
    };
    const auto cell_of = [&](Vec3 p) {
        const Vec3 at = multiply(p, proportions) * static_cast<float>(kGrid);
        return Vec3i{truncate(at.x), truncate(at.y), truncate(at.z)};
    };
    make({point_count, point_count + 1, point_count + 2, point_count + 3}, true);

    // Each triangle of the simplices a point falls in, once; one shared by two of them is bad.
    struct Triangle {
        std::array<std::uint32_t, 3> corners{};
        bool bad = false;
    };
    std::vector<Triangle> triangles;
    std::map<std::array<std::uint32_t, 3>, std::size_t> inserted;
    constexpr std::array<std::array<std::size_t, 3>, 4> kTriangleOrder{
        {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}}};

    for (std::uint32_t i = 0; i < point_count; ++i) {
        bool unique = true;
        for (std::uint32_t j = i + 1; j < point_count; ++j) {
            if (quantized[i] == quantized[j]) {
                unique = false;
                break;
            }
        }
        if (!unique) {
            continue;
        }
        std::vector<std::uint32_t>& list = cell(clamp_cell(cell_of(quantized[i])));
        std::size_t kept = 0;
        for (const std::uint32_t index : list) {
            Simplex& simplex = simplices[index];
            if (simplex.alive &&
                !in_sphere(quantized[simplex.points[0]], quantized[simplex.points[1]],
                           quantized[simplex.points[2]], quantized[simplex.points[3]],
                           quantized[i])) {
                list[kept++] = index;
                continue;
            }
            --listed;
            if (!simplex.alive) {
                continue;
            }
            for (const auto& order : kTriangleOrder) {
                std::array<std::uint32_t, 3> t{simplex.points.at(order[0]),
                                               simplex.points.at(order[1]),
                                               simplex.points.at(order[2])};
                std::ranges::sort(t);
                const auto [found, added] = inserted.try_emplace(t, triangles.size());
                if (added) {
                    triangles.push_back({t, false});
                } else {
                    // Bowyer-Watson: a triangle the retriangulated simplices share is not kept.
                    triangles[found->second].bad = true;
                }
            }
            simplex.alive = false;
            listed_live -= simplex.cells;
        }
        list.resize(kept);
        for (const Triangle& triangle : triangles) {
            if (!triangle.bad) {
                make({triangle.corners[0], triangle.corners[1], triangle.corners[2], i}, false);
            }
        }
        triangles.clear();
        inserted.clear();
        if (listed > (2 * listed_live) + grid.size()) {
            for (std::vector<std::uint32_t>& entries : grid) {
                std::erase_if(entries,
                              [&](std::uint32_t index) { return !simplices[index].alive; });
            }
            listed = listed_live;
        }
    }

    std::vector<std::array<std::uint32_t, 4>> out;
    for (const Simplex& simplex : simplices) {
        const bool outer =
            std::ranges::any_of(simplex.points, [&](std::uint32_t p) { return p >= point_count; });
        if (!simplex.alive || outer || simplex_is_coplanar(points, simplex.points)) {
            continue;
        }
        out.push_back(simplex.points);
    }
    return out;
}

std::vector<LightmapBspNode>
lightmap_bsp_tree(std::span<const std::array<float, 3>> points,
                  std::span<const std::array<std::uint32_t, 4>> tetrahedra, jobs::Pool* pool) {
    std::vector<BspSimplex> simplices;
    std::vector<Plane> planes;
    PlaneIndex index;
    for (const auto& tetrahedron : tetrahedra) {
        BspSimplex s;
        s.vertices = tetrahedron;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto& order = kFaceOrder.at(j);
            const Plane p =
                Plane::through(vec(points[tetrahedron.at(static_cast<std::size_t>(order[0]))]),
                               vec(points[tetrahedron.at(static_cast<std::size_t>(order[1]))]),
                               vec(points[tetrahedron.at(static_cast<std::size_t>(order[2]))]));
            // Planes are kept once, to try each once per node.
            s.planes.at(j) = index.find_or_add(p, planes);
        }
        simplices.push_back(s);
    }
    BspBuilder builder(points, std::move(simplices), planes.size(), pool);
    return builder.build();
}

LightmapSh tap_lightmap_probes(const LightmapProbes& probes, std::array<float, 3> point) {
    LightmapSh out{};
    if (probes.points.empty() || probes.bsp.empty() || probes.tetrahedra.empty()) {
        return out;
    }
    const Vec3 p = vec(point);
    std::int32_t node = 0;
    while (node >= 0) {
        const LightmapBspNode& at = probes.bsp[static_cast<std::size_t>(node)];
        const Plane plane{{at.plane[0], at.plane[1], at.plane[2]}, at.plane[3]};
        const std::int32_t next = plane.is_point_over(p) ? at.over : at.under;
        // Children come later: a file whose don't would loop.
        if (next >= 0 && (next <= node || static_cast<std::size_t>(next) >= probes.bsp.size())) {
            return out;
        }
        node = next;
    }
    if (node == kLightmapBspEmpty) {
        return out;
    }
    const auto tetrahedron = static_cast<std::size_t>(-(static_cast<std::int64_t>(node) + 1));
    if (tetrahedron >= probes.tetrahedra.size()) {
        return out;
    }
    const std::array<std::uint32_t, 4>& corners = probes.tetrahedra[tetrahedron];
    for (const std::uint32_t corner : corners) {
        if (corner >= probes.points.size() || corner >= probes.sh.size()) {
            return out;
        }
    }
    const std::array<float, 4> weights =
        barycentric(vec(probes.points[corners[0]]), vec(probes.points[corners[1]]),
                    vec(probes.points[corners[2]]), vec(probes.points[corners[3]]), p);
    for (std::size_t i = 0; i < 4; ++i) {
        const float c = std::clamp(weights.at(i), 0.0F, 1.0F);
        const LightmapSh& sh = probes.sh[corners.at(i)];
        for (std::size_t j = 0; j < 9; ++j) {
            for (std::size_t k = 0; k < 3; ++k) {
                out.at(j).at(k) += sh.at(j).at(k) * c;
            }
        }
    }
    return out;
}

void rotate_sh(LightmapSh& sh, const world::Affine3D& basis) {
    const world::Affine3D rotation = world::orthonormalized(basis);
    // Rows of Godot's Basis: row r, column c is axis c's component r.
    const float m00 = rotation.x.x;
    const float m01 = rotation.y.x;
    const float m02 = rotation.z.x;
    const float m10 = rotation.x.y;
    const float m11 = rotation.y.y;
    const float m12 = rotation.z.y;
    const float m20 = rotation.x.z;
    const float m21 = rotation.y.z;
    const float m22 = rotation.z.z;

    constexpr float kC3 = 0.94617469575F;  // (3*sqrt(5))/(4*sqrt(pi))
    constexpr float kC4 = -0.31539156525F; // (-sqrt(5))/(4*sqrt(pi))
    constexpr float kC5 = 0.54627421529F;  // (sqrt(15))/(4*sqrt(pi))
    constexpr float kScale = 1.0F / 0.91529123286551084F;
    constexpr float kScaleInv = 0.91529123286551084F;
    constexpr float kRc2 = 1.5853309190550713F * kScale;
    constexpr float kC4DivC3 = kC4 / kC3;
    constexpr float kC4DivC3x2 = (kC4 / kC3) * 2.0F;
    constexpr float kScaleDst2 = kC3 * kScaleInv;
    constexpr float kScaleDst4 = kC5 * kScaleInv;

    for (std::size_t channel = 0; channel < 3; ++channel) {
        // Hable's code takes the coefficients with the Condon-Shortley phase, odd orders negated,
        // which Godot's lightmapper doesn't use: flipping them around it keeps the light's
        // directions turning with the rotation (Godot passes them as they are, which turns the
        // light mirrored in z).
        std::array<float, 9> src{};
        for (std::size_t j = 0; j < 9; ++j) {
            src.at(j) = (j % 2 == 1 ? -1.0F : 1.0F) * sh.at(j).at(channel);
        }
        std::array<float, 9> dst{};
        dst[0] = src[0];
        dst[1] = (m11 * src[1]) - (m12 * src[2]) + (m10 * src[3]);
        dst[2] = (-m21 * src[1]) + (m22 * src[2]) - (m20 * src[3]);
        dst[3] = (m01 * src[1]) - (m02 * src[2]) + (m00 * src[3]);

        const float sh0 = src[7] + src[8] + src[8] - src[5];
        const float sh1 = src[4] + (kRc2 * src[6]) + src[7] + src[8];
        const float sh2 = src[4];
        const float sh3 = -src[7];
        const float sh4 = -src[5];

        // R0 and R1 are the matrix's columns; R2 to R4 sums of two.
        const float r2x = m00 + m01;
        const float r2y = m10 + m11;
        const float r2z = m20 + m21;
        const float r3x = m00 + m02;
        const float r3y = m10 + m12;
        const float r3z = m20 + m22;
        const float r4x = m01 + m02;
        const float r4y = m11 + m12;
        const float r4z = m21 + m22;

        // Column 0.
        const float sh0_x = sh0 * m00;
        const float sh0_y = sh0 * m10;
        float d0 = sh0_x * m10;
        float d1 = sh0_y * m20;
        float d2 = sh0 * ((m20 * m20) + kC4DivC3);
        float d3 = sh0_x * m20;
        float d4 = (sh0_x * m00) - (sh0_y * m10);
        // Column 1.
        const float sh1_x = sh1 * m02;
        const float sh1_y = sh1 * m12;
        d0 += sh1_x * m12;
        d1 += sh1_y * m22;
        d2 += sh1 * ((m22 * m22) + kC4DivC3);
        d3 += sh1_x * m22;
        d4 += (sh1_x * m02) - (sh1_y * m12);
        // Column 2.
        const float sh2_x = sh2 * r2x;
        const float sh2_y = sh2 * r2y;
        d0 += sh2_x * r2y;
        d1 += sh2_y * r2z;
        d2 += sh2 * ((r2z * r2z) + kC4DivC3x2);
        d3 += sh2_x * r2z;
        d4 += (sh2_x * r2x) - (sh2_y * r2y);
        // Column 3.
        const float sh3_x = sh3 * r3x;
        const float sh3_y = sh3 * r3y;
        d0 += sh3_x * r3y;
        d1 += sh3_y * r3z;
        d2 += sh3 * ((r3z * r3z) + kC4DivC3x2);
        d3 += sh3_x * r3z;
        d4 += (sh3_x * r3x) - (sh3_y * r3y);
        // Column 4.
        const float sh4_x = sh4 * r4x;
        const float sh4_y = sh4 * r4y;
        d0 += sh4_x * r4y;
        d1 += sh4_y * r4z;
        d2 += sh4 * ((r4z * r4z) + kC4DivC3x2);
        d3 += sh4_x * r4z;
        d4 += (sh4_x * r4x) - (sh4_y * r4y);

        dst[4] = d0;
        dst[5] = -d1;
        dst[6] = d2 * kScaleDst2;
        dst[7] = -d3;
        dst[8] = d4 * kScaleDst4;
        for (std::size_t j = 0; j < 9; ++j) {
            sh.at(j).at(channel) = (j % 2 == 1 ? -1.0F : 1.0F) * dst.at(j);
        }
    }
}

std::optional<LightmapSh> lightmap_capture(std::span<const LightmapCaptureSource> lightmaps,
                                           std::array<float, 3> center) {
    bool inside = false;
    LightmapSh accum{};
    float accum_blend = 0.0F;
    for (const LightmapCaptureSource& lightmap : lightmaps) {
        if (lightmap.probes == nullptr || lightmap.probes->points.empty()) {
            continue;
        }
        const LightmapProbes& probes = *lightmap.probes;
        if (inside && !probes.interior) {
            continue; // inside an interior: exteriors are left out
        }
        const Vec3 at = lightmap.transform.inverse().apply(vec(center));
        const Box bounds{vec(probes.bounds_position), vec(probes.bounds_size)};
        if (!bounds.has_point(at)) {
            continue;
        }
        LightmapSh sh = tap_lightmap_probes(probes, {at.x, at.y, at.z});
        rotate_sh(sh, lightmap.transform);
        for (auto& coefficient : sh) {
            for (float& c : coefficient) {
                c *= lightmap.exposure;
            }
        }
        // How far inside the bounds it is, rounded off toward the corners.
        const Vec3 inner =
            (divide(at - bounds.position, bounds.size) * 2.0F) - Vec3{1.0F, 1.0F, 1.0F};
        float blend = std::max(std::abs(inner.x), std::max(std::abs(inner.y), std::abs(inner.z)));
        const float rounded = length(inner);
        blend = rounded + ((blend - rounded) * blend);
        blend *= blend;
        blend = std::max(0.0F, 1.0F - blend);
        if (probes.interior && !inside) {
            // Not blended: it stands in for the rest.
            for (std::size_t j = 0; j < 9; ++j) {
                for (std::size_t k = 0; k < 3; ++k) {
                    accum.at(j).at(k) = sh.at(j).at(k) * blend;
                }
            }
            accum_blend = blend;
            inside = true;
        } else {
            for (std::size_t j = 0; j < 9; ++j) {
                for (std::size_t k = 0; k < 3; ++k) {
                    accum.at(j).at(k) += sh.at(j).at(k) * blend;
                }
            }
            accum_blend += blend;
        }
    }
    if (!(accum_blend > 0.0F)) {
        return std::nullopt;
    }
    for (auto& coefficient : accum) {
        for (float& c : coefficient) {
            c /= accum_blend;
        }
    }
    return accum;
}

} // namespace slash::scene3d
