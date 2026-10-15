#include "polygon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace slash::physics2d::detail {

namespace {

// Points closer than this, in pixels, are the same point.
constexpr float kWeld = 1.0e-3F;

float cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

// Positive when a, b, c turn counter-clockwise (in y-up terms).
float turn(Vec2 a, Vec2 b, Vec2 c) {
    return cross(b - a, c - b);
}

float signed_area(std::span<const Vec2> points) {
    float area = 0.0F;
    for (std::size_t i = 0; i < points.size(); ++i) {
        area += cross(points[i], points[(i + 1) % points.size()]);
    }
    return area * 0.5F;
}

bool same_point(Vec2 a, Vec2 b) {
    return std::abs(a.x - b.x) <= kWeld && std::abs(a.y - b.y) <= kWeld;
}

// The polygon without repeated and collinear points, counter-clockwise.
Polygon clean(std::span<const Vec2> points) {
    Polygon out;
    for (const Vec2 point : points) {
        if (out.empty() || !same_point(out.back(), point)) {
            out.push_back(point);
        }
    }
    while (out.size() > 1 && same_point(out.front(), out.back())) {
        out.pop_back();
    }
    bool removed = true;
    while (removed && out.size() >= 3) {
        removed = false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const Vec2 prev = out[(i + out.size() - 1) % out.size()];
            const Vec2 next = out[(i + 1) % out.size()];
            const Vec2 edge = next - prev;
            const float length = std::sqrt((edge.x * edge.x) + (edge.y * edge.y));
            if (std::abs(turn(prev, out[i], next)) <= kWeld * length) {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
                removed = true;
                break;
            }
        }
    }
    if (out.size() >= 3 && signed_area(out) < 0.0F) {
        std::reverse(out.begin(), out.end());
    }
    return out;
}

// Whether segments ab and cd cross or touch.
bool segments_meet(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const float d1 = turn(c, d, a);
    const float d2 = turn(c, d, b);
    const float d3 = turn(a, b, c);
    const float d4 = turn(a, b, d);
    if (((d1 > 0.0F && d2 < 0.0F) || (d1 < 0.0F && d2 > 0.0F)) &&
        ((d3 > 0.0F && d4 < 0.0F) || (d3 < 0.0F && d4 > 0.0F))) {
        return true;
    }
    const auto on_segment = [](Vec2 p, Vec2 q, Vec2 r) {
        return std::min(p.x, q.x) - kWeld <= r.x && r.x <= std::max(p.x, q.x) + kWeld &&
               std::min(p.y, q.y) - kWeld <= r.y && r.y <= std::max(p.y, q.y) + kWeld;
    };
    return (d1 == 0.0F && on_segment(c, d, a)) || (d2 == 0.0F && on_segment(c, d, b)) ||
           (d3 == 0.0F && on_segment(a, b, c)) || (d4 == 0.0F && on_segment(a, b, d));
}

bool is_simple(std::span<const Vec2> points) {
    const std::size_t count = points.size();
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            // Neighboring edges share a point.
            if (j == i + 1 || (i == 0 && j == count - 1)) {
                continue;
            }
            if (segments_meet(points[i], points[(i + 1) % count], points[j],
                              points[(j + 1) % count])) {
                return false;
            }
        }
    }
    return true;
}

// Whether p is inside or on triangle abc, wound counter-clockwise.
bool in_triangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    return turn(a, b, p) >= 0.0F && turn(b, c, p) >= 0.0F && turn(c, a, p) >= 0.0F;
}

using Indices = std::vector<std::uint32_t>;

// Ear clipping: triangles of a simple counter-clockwise polygon, as indices into it.
Result<std::vector<Indices>> triangulate(const Polygon& polygon) {
    Indices left(polygon.size());
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        left[i] = i;
    }
    std::vector<Indices> triangles;
    while (left.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < left.size(); ++i) {
            const std::uint32_t prev = left[(i + left.size() - 1) % left.size()];
            const std::uint32_t at = left[i];
            const std::uint32_t next = left[(i + 1) % left.size()];
            const Vec2 a = polygon[prev];
            const Vec2 b = polygon[at];
            const Vec2 c = polygon[next];
            if (turn(a, b, c) <= 0.0F) {
                continue; // reflex
            }
            const bool ear = std::none_of(left.begin(), left.end(), [&](std::uint32_t other) {
                return other != prev && other != at && other != next &&
                       !same_point(polygon[other], a) && !same_point(polygon[other], b) &&
                       !same_point(polygon[other], c) && in_triangle(polygon[other], a, b, c);
            });
            if (ear) {
                triangles.push_back({prev, at, next});
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
                break;
            }
        }
        if (!clipped) {
            return make_error(ErrorCode::InvalidArgument, "the polygon could not be split");
        }
    }
    triangles.push_back(left);
    return triangles;
}

bool is_convex(const Indices& indices, const Polygon& polygon) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const Vec2 a = polygon[indices[i]];
        const Vec2 b = polygon[indices[(i + 1) % indices.size()]];
        const Vec2 c = polygon[indices[(i + 2) % indices.size()]];
        if (turn(a, b, c) < 0.0F) {
            return false;
        }
    }
    return true;
}

// p and q merged across an edge they share, or empty if they share none.
Indices merge(const Indices& p, const Indices& q) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const std::uint32_t u = p[i];
        const std::uint32_t v = p[(i + 1) % p.size()];
        for (std::size_t j = 0; j < q.size(); ++j) {
            if (q[j] != v || q[(j + 1) % q.size()] != u) {
                continue;
            }
            Indices merged;
            merged.reserve(p.size() + q.size() - 2);
            for (std::size_t k = 0; k < p.size(); ++k) {
                merged.push_back(p[(i + 1 + k) % p.size()]); // v round to u
            }
            for (std::size_t k = 2; k < q.size(); ++k) {
                merged.push_back(q[(j + k) % q.size()]); // after u, round to before v
            }
            return merged;
        }
    }
    return {};
}

} // namespace

Polygon convex_hull(std::span<const Vec2> points) {
    Polygon sorted(points.begin(), points.end());
    std::sort(sorted.begin(), sorted.end(),
              [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    sorted.erase(std::unique(sorted.begin(), sorted.end(), same_point), sorted.end());
    if (sorted.size() < 3) {
        return sorted;
    }
    // Andrew's monotone chain.
    Polygon hull(sorted.size() * 2);
    std::size_t count = 0;
    for (const Vec2 point : sorted) {
        while (count >= 2 && turn(hull[count - 2], hull[count - 1], point) <= 0.0F) {
            --count;
        }
        hull[count++] = point;
    }
    const std::size_t lower = count + 1;
    for (std::size_t i = sorted.size() - 1; i-- > 0;) {
        while (count >= lower && turn(hull[count - 2], hull[count - 1], sorted[i]) <= 0.0F) {
            --count;
        }
        hull[count++] = sorted[i];
    }
    hull.resize(count - 1);
    return clean(hull);
}

std::vector<Polygon> split_convex(std::span<const Vec2> hull, std::size_t max_points) {
    if (hull.size() <= max_points) {
        return {Polygon(hull.begin(), hull.end())};
    }
    std::vector<Polygon> pieces;
    std::size_t start = 1;
    while (start + 1 < hull.size()) {
        const std::size_t end = std::min(start + max_points - 2, hull.size() - 1);
        Polygon piece{hull[0]};
        piece.insert(piece.end(), hull.begin() + static_cast<std::ptrdiff_t>(start),
                     hull.begin() + static_cast<std::ptrdiff_t>(end) + 1);
        pieces.push_back(std::move(piece));
        start = end;
    }
    return pieces;
}

Result<std::vector<Polygon>> decompose(std::span<const Vec2> points, std::size_t max_points) {
    const Polygon polygon = clean(points);
    if (polygon.size() < 3) {
        return make_error(ErrorCode::InvalidArgument, "the polygon has no area");
    }
    if (!is_simple(polygon)) {
        return make_error(ErrorCode::InvalidArgument, "the polygon crosses itself");
    }
    auto triangles = triangulate(polygon);
    if (!triangles) {
        return triangles.error();
    }
    // Merge neighbors while the result stays convex and small enough, first pairs first.
    std::vector<Indices> parts = std::move(triangles).value();
    bool merged_any = true;
    while (merged_any) {
        merged_any = false;
        for (std::size_t i = 0; i < parts.size() && !merged_any; ++i) {
            for (std::size_t j = i + 1; j < parts.size() && !merged_any; ++j) {
                if (parts[i].size() + parts[j].size() - 2 > max_points) {
                    continue;
                }
                Indices merged = merge(parts[i], parts[j]);
                if (!merged.empty() && is_convex(merged, polygon)) {
                    parts[i] = std::move(merged);
                    parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(j));
                    merged_any = true;
                }
            }
        }
    }
    std::vector<Polygon> out;
    out.reserve(parts.size());
    for (const Indices& part : parts) {
        Polygon piece;
        piece.reserve(part.size());
        for (const std::uint32_t index : part) {
            piece.push_back(polygon[index]);
        }
        out.push_back(std::move(piece));
    }
    return out;
}

} // namespace slash::physics2d::detail
