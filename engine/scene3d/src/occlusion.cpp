#include "slash/scene3d/occlusion.hpp"

#include "geometry.hpp"

#include "slash/core/math.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <meshoptimizer.h>
#include <numbers>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

using detail::cross;
using detail::dot;
using detail::normalized;

constexpr float kPi = std::numbers::pi_v<float>;
// SphereOccluder3D's RINGS and RADIAL_SEGMENTS.
constexpr int kRings = 7;
constexpr int kRadialSegments = 7;
// HZBuffer::_is_occluded's max_samples.
constexpr int kMaxSamples = 512;
// Frames a mesh seen stays shown with jitter, as HZBuffer::is_occluded's timeout.
constexpr std::uint64_t kOcclusionTimeout = 9;

static_assert(sizeof(Vec3) == 3 * sizeof(float), "meshoptimizer reads points as three floats");

// The entities with a component in the order they were made.
template <typename T>
std::vector<entt::entity> made(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const T>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

// Godot's CLAMP macro, which compares as written.
template <typename T>
T clamp(T value, T low, T high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

// Godot's MIN and MAX macros, which keep the second of a NaN pair.
float min_of(float a, float b) {
    return a < b ? a : b;
}
float max_of(float a, float b) {
    return a > b ? a : b;
}

// Where texel x, y of a level width texels across is.
std::size_t texel(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
           static_cast<std::size_t>(x);
}

// Triangulate::get_area.
float area(const std::vector<Vec2>& contour) {
    const auto n = static_cast<int>(contour.size());
    float a = 0.0F;
    for (int p = n - 1, q = 0; q < n; p = q++) {
        const Vec2 cp = contour[static_cast<std::size_t>(p)];
        const Vec2 cq = contour[static_cast<std::size_t>(q)];
        a += (cp.x * cq.y) - (cp.y * cq.x);
    }
    return a * 0.5F;
}

// Math::ceil(Math::log2(size)) for the level a box's rectangle starts its search at, worked out
// exactly: the smallest power of two at least size, as an exponent; 0 or below for sizes up to 1.
int ceil_log2(float size) {
    if (!(size > 1.0F)) {
        return 0;
    }
    if (std::isinf(size)) {
        return 1 << 30;
    }
    int exponent = 0;
    const float mantissa = std::frexp(size, &exponent);
    return mantissa == 0.5F ? exponent - 1 : exponent;
}

// A point through a column-major projection, divided by its w, as Projection::xform.
Vec3 project(const std::array<float, 16>& m, Vec3 v) {
    const float x = (m[0] * v.x) + (m[4] * v.y) + (m[8] * v.z) + m[12];
    const float y = (m[1] * v.x) + (m[5] * v.y) + (m[9] * v.z) + m[13];
    const float z = (m[2] * v.x) + (m[6] * v.y) + (m[10] * v.z) + m[14];
    const float w = (m[3] * v.x) + (m[7] * v.y) + (m[11] * v.z) + m[15];
    return {x / w, y / w, z / w};
}

Bounds empty_bounds() {
    return {.min = {FLT_MAX, FLT_MAX, FLT_MAX}, .max = {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
}

void grow(Bounds& box, Vec3 p) {
    box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
    box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
}

// Where a ray enters and leaves a box, from near to far: false when it misses. A ray along a
// box's face counts as inside it.
bool crosses(const Bounds& box, Vec3 origin, Vec3 dir, float near, float far) {
    float enter = near;
    float leave = far;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = detail::component(origin, axis);
        const float d = detail::component(dir, axis);
        const float low = detail::component(box.min, axis);
        const float high = detail::component(box.max, axis);
        if (d == 0.0F) {
            if (o < low || o > high) {
                return false;
            }
            continue;
        }
        float t0 = (low - o) / d;
        float t1 = (high - o) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        // A little slack, so a ray grazing an edge still visits the triangles there.
        const float slack = 1.0e-5F * std::max(1.0F, std::abs(t1));
        enter = std::max(enter, t0 - slack);
        leave = std::min(leave, t1 + slack);
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

// How far along the ray a triangle is (Möller and Trumbore's test, from either side), or a
// negative distance when it misses.
float hit(const std::array<Vec3, 3>& triangle, Vec3 origin, Vec3 dir) {
    const Vec3 e1 = triangle[1] - triangle[0];
    const Vec3 e2 = triangle[2] - triangle[0];
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (det == 0.0F) {
        return -1.0F;
    }
    const float inverse = 1.0F / det;
    const Vec3 s = origin - triangle[0];
    const float u = dot(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return -1.0F;
    }
    const Vec3 q = cross(s, e1);
    const float v = dot(dir, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return -1.0F;
    }
    return dot(e2, q) * inverse;
}

// Godot's StandardMaterial3D check in OccluderInstance3D::_bake_material_check.
bool bakes(const world::StandardMaterial3D* material) {
    return material == nullptr || material->transparency == "disabled";
}

// SurfaceTool::TriangleHasher.
std::uint32_t triangle_hash(const int* triangle) {
    std::array<int, 3> t{triangle[0], triangle[1], triangle[2]};
    std::ranges::sort(t);
    return (static_cast<std::uint32_t>(t[0]) * 73856093U) ^
           (static_cast<std::uint32_t>(t[1]) * 19349663U) ^
           (static_cast<std::uint32_t>(t[2]) * 83492791U);
}
bool same_triangle(const int* a, const int* b) {
    std::array<int, 3> l{a[0], a[1], a[2]};
    std::array<int, 3> r{b[0], b[1], b[2]};
    std::ranges::sort(l);
    std::ranges::sort(r);
    return l == r;
}

// SurfaceTool::strip_mesh_arrays: points repeated exactly merge, then triangles with a repeated
// corner and triangles seen before go. As Godot's, the triangles seen are kept where they were
// first found, which later ones may have moved over, and compared as they are when looked up.
void strip(std::vector<Vec3>& vertices, std::vector<int>& indices) {
    std::vector<unsigned int> remap(vertices.size());
    std::vector<unsigned int> unsigned_indices(indices.begin(), indices.end());
    const std::size_t count =
        meshopt_generateVertexRemap(remap.data(), unsigned_indices.data(), unsigned_indices.size(),
                                    vertices.data(), vertices.size(), sizeof(Vec3));
    meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(Vec3),
                              remap.data());
    vertices.resize(count);
    meshopt_remapIndexBuffer(unsigned_indices.data(), unsigned_indices.data(),
                             unsigned_indices.size(), remap.data());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = static_cast<int>(unsigned_indices[i]);
    }
    std::unordered_multimap<std::uint32_t, std::size_t> found;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < indices.size() / 3; ++i) {
        const int* triangle = &indices[i * 3];
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
            triangle[2] == triangle[0]) {
            continue;
        }
        const std::uint32_t hash = triangle_hash(triangle);
        const auto [first, last] = found.equal_range(hash);
        const bool seen = std::any_of(first, last, [&](const auto& entry) {
            return same_triangle(&indices[entry.second * 3], triangle);
        });
        if (seen) {
            continue;
        }
        if (i != kept) {
            std::copy_n(triangle, 3, &indices[kept * 3]);
        }
        found.emplace(hash, i);
        ++kept;
    }
    indices.resize(kept * 3);
}

// OccluderInstance3D::_bake_surface: a surface's points placed by transform, simplified, stripped
// and added to out.
void bake_surface(const world::Affine3D& transform, const std::vector<render::Vertex3D>& source,
                  const std::vector<std::uint32_t>& source_indices, float simplification,
                  OccluderMesh& out) {
    if (source.empty() || source_indices.empty()) {
        return;
    }
    std::vector<Vec3> vertices;
    vertices.reserve(source.size());
    for (const render::Vertex3D& vertex : source) {
        vertices.push_back(
            transform.apply({vertex.position[0], vertex.position[1], vertex.position[2]}));
    }
    std::vector<unsigned int> indices(source_indices.begin(), source_indices.end());
    if (std::abs(simplification) >= detail::kEpsilon) {
        const float scale = meshopt_simplifyScale(&vertices[0].x, vertices.size(), sizeof(Vec3));
        const float target_error = simplification / scale;
        float error = -1.0F;
        const std::size_t target = std::min<std::size_t>(indices.size(), 36);
        const std::size_t count = meshopt_simplify(
            indices.data(), indices.data(), indices.size(), &vertices[0].x, vertices.size(),
            sizeof(Vec3), target, target_error, meshopt_SimplifyLockBorder, &error);
        indices.resize(count);
    }
    std::vector<int> signed_indices(indices.begin(), indices.end());
    strip(vertices, signed_indices);
    const auto offset = static_cast<int>(out.vertices.size());
    out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
    for (const int index : signed_indices) {
        out.indices.push_back(offset + index);
    }
}

} // namespace

Result<OccluderMesh> occluder_mesh(const world::OccluderInstance3D& occluder) {
    OccluderMesh out;
    if (occluder.shape == "quad") {
        // QuadOccluder3D::_update_arrays.
        const float x = occluder.size.x / 2.0F;
        const float y = occluder.size.y / 2.0F;
        out.vertices = {{-x, -y, 0.0F}, {-x, y, 0.0F}, {x, y, 0.0F}, {x, -y, 0.0F}};
        out.indices = {0, 1, 2, 0, 2, 3};
    } else if (occluder.shape == "box") {
        // BoxOccluder3D::_update_arrays.
        const float x = occluder.size.x / 2.0F;
        const float y = occluder.size.y / 2.0F;
        const float z = occluder.size.z / 2.0F;
        out.vertices = {{-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z},
                        {-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}};
        out.indices = {0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1, 7, 6, 5, 5, 4, 7,
                       4, 0, 3, 3, 7, 4, 4, 5, 1, 1, 0, 4, 3, 2, 6, 6, 7, 3};
    } else if (occluder.shape == "sphere") {
        // SphereOccluder3D::_update_arrays.
        int current_row = 0;
        int previous_row = 0;
        int point = 0;
        for (int j = 0; j <= kRings + 1; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(kRings + 1);
            const Vec2 ring = cos_sin(kPi * v);
            for (int i = 0; i <= kRadialSegments; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(kRadialSegments);
                const Vec2 around = cos_sin(u * 2.0F * kPi);
                out.vertices.push_back(Vec3{around.x * ring.y, ring.x, around.y * ring.y} *
                                       occluder.radius);
                if (i > 0 && j > 0) {
                    out.indices.insert(out.indices.end(),
                                       {previous_row + i - 1, previous_row + i, current_row + i - 1,
                                        previous_row + i, current_row + i, current_row + i - 1});
                }
                ++point;
            }
            previous_row = current_row;
            current_row = point;
        }
    } else if (occluder.shape == "polygon") {
        // PolygonOccluder3D::_update_arrays: wound clockwise, then cut into triangles.
        if (occluder.polygon.size() < 3) {
            return out;
        }
        std::vector<Vec2> polygon = occluder.polygon;
        if (area(polygon) > 0.0F) {
            std::ranges::reverse(polygon);
        }
        out.indices = triangulate(polygon);
        if (out.indices.size() < 3) {
            return make_error(ErrorCode::InvalidArgument,
                              "the polygon can't be cut into triangles: make sure its edges "
                              "don't cross");
        }
        for (const Vec2 p : polygon) {
            out.vertices.push_back({p.x, p.y, 0.0F});
        }
    } else if (occluder.shape == "array") {
        out.vertices = occluder.vertices;
        out.indices = occluder.indices;
        for (const int index : out.indices) {
            if (index < 0 || static_cast<std::size_t>(index) >= out.vertices.size()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "index " + std::to_string(index) +
                                      " is out of range: there are " +
                                      std::to_string(out.vertices.size()) + " vertices");
            }
        }
    } else {
        return make_error(ErrorCode::InvalidArgument,
                          "unknown occluder shape \"" + occluder.shape + "\"");
    }
    return out;
}

std::vector<Problem> check_occluders(const world::World& world) {
    std::vector<Problem> out;
    const std::string path = "/components/OccluderInstance3D";
    for (const entt::entity entity : made<world::OccluderInstance3D>(world)) {
        const auto& occluder = world.ecs().get<const world::OccluderInstance3D>(entity);
        if (occluder.shape == "polygon" && occluder.polygon.size() < 3) {
            out.push_back({entity, path + "/polygon",
                           "a polygon occluder needs at least 3 points to hide anything"});
            continue;
        }
        if (occluder.shape == "array" && occluder.indices.size() < 3) {
            out.push_back({entity, path + "/indices",
                           "an array occluder needs at least 3 indices to hide anything: bake it "
                           "with occluder.bake"});
            continue;
        }
        if (auto mesh = occluder_mesh(occluder); !mesh) {
            out.push_back({entity, path + (occluder.shape == "polygon" ? "/polygon" : "/indices"),
                           mesh.error().message});
        }
    }
    return out;
}

std::vector<std::array<Vec3, 3>> occluder_triangles(const world::World& world,
                                                    const world::Tree3D& tree) {
    std::vector<std::array<Vec3, 3>> out;
    for (const entt::entity entity : made<world::OccluderInstance3D>(world)) {
        if (!tree.visible(entity)) {
            continue;
        }
        auto mesh = occluder_mesh(world.ecs().get<const world::OccluderInstance3D>(entity));
        if (!mesh) {
            continue;
        }
        const world::Affine3D place = tree.global(entity);
        const OccluderMesh& m = mesh.value();
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            out.push_back({place.apply(m.vertices[static_cast<std::size_t>(m.indices[i])]),
                           place.apply(m.vertices[static_cast<std::size_t>(m.indices[i + 1])]),
                           place.apply(m.vertices[static_cast<std::size_t>(m.indices[i + 2])])});
        }
    }
    return out;
}

void OccluderScene::build(std::vector<std::array<Vec3, 3>> triangles) {
    if (triangles == triangles_) {
        return;
    }
    triangles_ = std::move(triangles);
    nodes_.clear();
    if (triangles_.empty()) {
        return;
    }
    // Triangles sorted by their middles, each node a run of them: the order is by value and then
    // by place, so the hierarchy is the same everywhere.
    std::vector<std::pair<Vec3, std::uint32_t>> middles;
    middles.reserve(triangles_.size());
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        const auto& t = triangles_[i];
        middles.emplace_back((t[0] + t[1] + t[2]) * (1.0F / 3.0F), static_cast<std::uint32_t>(i));
    }
    std::vector<std::array<Vec3, 3>> ordered;
    ordered.reserve(triangles_.size());
    // Built depth first: a node's first child follows it, its second is at its first.
    struct Pending {
        std::uint32_t node;
        std::uint32_t first;
        std::uint32_t count;
    };
    nodes_.push_back({});
    std::vector<Pending> stack{{0, 0, static_cast<std::uint32_t>(triangles_.size())}};
    while (!stack.empty()) {
        const Pending pending = stack.back();
        stack.pop_back();
        Bounds box = empty_bounds();
        Bounds middle_box = empty_bounds();
        for (std::uint32_t i = pending.first; i < pending.first + pending.count; ++i) {
            for (const Vec3& p : triangles_[middles[i].second]) {
                grow(box, p);
            }
            grow(middle_box, middles[i].first);
        }
        nodes_[pending.node].box = box;
        if (pending.count <= 4) {
            nodes_[pending.node].first = static_cast<std::uint32_t>(ordered.size());
            nodes_[pending.node].count = pending.count;
            for (std::uint32_t i = pending.first; i < pending.first + pending.count; ++i) {
                ordered.push_back(triangles_[middles[i].second]);
            }
            continue;
        }
        const Vec3 extent = middle_box.max - middle_box.min;
        int axis = 0;
        if (extent.y > extent.x) {
            axis = 1;
        }
        if (extent.z > detail::component(extent, axis)) {
            axis = 2;
        }
        const auto begin = middles.begin() + pending.first;
        std::sort(begin, begin + pending.count, [axis](const auto& a, const auto& b) {
            const float ka = detail::component(a.first, axis);
            const float kb = detail::component(b.first, axis);
            return ka < kb || (ka == kb && a.second < b.second);
        });
        const std::uint32_t half = pending.count / 2;
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        const auto right = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        nodes_[pending.node].first = left;
        nodes_[pending.node].count = 0;
        stack.push_back({right, pending.first + half, pending.count - half});
        stack.push_back({left, pending.first, half});
    }
    order_ = std::move(ordered);
}

float OccluderScene::cast(Vec3 origin, Vec3 dir, float near, float far) const {
    if (nodes_.empty()) {
        return far;
    }
    float best = far;
    std::array<std::uint32_t, 64> stack{};
    std::size_t top = 0;
    stack.at(top++) = 0;
    while (top > 0) {
        const Node& node = nodes_[stack.at(--top)];
        if (!crosses(node.box, origin, dir, near, best)) {
            continue;
        }
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const float t = hit(order_[i], origin, dir);
                if (t >= near && t <= best) {
                    best = t;
                }
            }
            continue;
        }
        stack.at(top++) = node.first + 1;
        stack.at(top++) = node.first;
    }
    return best;
}

std::array<int, 2> OcclusionBuffer::size_for(int width, int height, int rays_per_thread) {
    if (width <= 0 || height <= 0) {
        return {0, 0};
    }
    // RendererViewport::_draw_3d.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    int max_size = rays_per_thread * kOcclusionThreads;
    const int viewport_size = width * height;
    max_size = clamp(max_size, viewport_size / (32 * 32), viewport_size / (2 * 2));
    const float h = std::sqrt(static_cast<float>(max_size) / aspect);
    return {static_cast<int>(h * aspect), static_cast<int>(h)};
}

void OcclusionBuffer::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        sizes_.clear();
        levels_.clear();
        return;
    }
    if (!sizes_.empty() && sizes_[0] == std::array<int, 2>{width, height}) {
        return;
    }
    // HZBuffer::resize: levels down to one texel, and one more for a buffer of one texel.
    std::size_t count = 0;
    int w = width;
    int h = height;
    while (true) {
        w = std::max(1, w >> 1);
        h = std::max(1, h >> 1);
        ++count;
        if (w == 1 && h == 1) {
            ++count;
            break;
        }
    }
    sizes_.clear();
    levels_.clear();
    w = width;
    h = height;
    for (std::size_t i = 0; i < count; ++i) {
        sizes_.push_back({w, h});
        levels_.emplace_back(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), FLT_MAX);
        w = std::max(1, w >> 1);
        h = std::max(1, h >> 1);
    }
}

float OcclusionBuffer::depth(std::size_t level, int x, int y) const {
    const std::array<int, 2>& size = sizes_.at(level);
    return levels_.at(level).at(texel(x, y, size[0]));
}

void OcclusionBuffer::update(const OccluderScene& scene, const OcclusionCamera& camera,
                             std::uint64_t frame, bool jitter) {
    camera_ = camera;
    inverse_ = camera.transform.inverse();
    frame_ = frame;
    jitter_ = jitter;
    if (sizes_.empty()) {
        return;
    }
    const int width = sizes_[0][0];
    const int height = sizes_[0][1];
    // _get_viewport_rect: the near plane's rectangle, symmetric about the view's middle.
    const std::array<float, 16>& m = camera.projection;
    const float scale = camera.orthogonal ? 1.0F : camera.near;
    const Vec2 half{scale / m[0], scale / m[5]};
    Vec2 bottom_left{-half.x, -half.y};
    if (jitter) {
        // RaycastOcclusionCull::_get_jitter.
        static constexpr std::array<Vec2, 9> kJitter{
            Vec2{0.0F, 0.0F},  Vec2{-1.0F, -1.0F}, Vec2{1.0F, -1.0F},
            Vec2{-1.0F, 1.0F}, Vec2{1.0F, 1.0F},   Vec2{-0.5F, -0.5F},
            Vec2{0.5F, -0.5F}, Vec2{-0.5F, 0.5F},  Vec2{0.5F, 0.5F}};
        const Vec2 j = kJitter.at(static_cast<std::size_t>(frame % 9));
        bottom_left.x += j.x * (half.x / static_cast<float>(width)) * 0.66F;
        bottom_left.y += j.y * (half.y / static_cast<float>(height)) * 0.66F;
    }
    // RaycastHZBuffer::update_camera_rays and _generate_camera_rays.
    const float z_near = camera.near;
    const float z_far = camera.far * 1.05F;
    const Vec3 camera_position = camera.transform.origin;
    const Vec3 camera_dir = camera.transform.z * -1.0F;
    const Vec3 near_bottom_left{bottom_left.x, bottom_left.y, -z_near};
    const Vec3 corner = camera.transform.apply(near_bottom_left);
    const Vec3 top = camera.transform.apply(near_bottom_left + Vec3{0.0F, half.y * 2.0F, 0.0F});
    const Vec3 right = camera.transform.apply(near_bottom_left + Vec3{half.x * 2.0F, 0.0F, 0.0F});
    const Vec3 u_step = right - corner;
    const Vec3 v_step = top - corner;
    std::vector<float>& base = levels_[0];
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
            const Vec3 pixel = corner + (u_step * u) + (v_step * v);
            Vec3 origin;
            Vec3 dir;
            float near = z_near;
            if (camera.orthogonal) {
                dir = camera_dir;
                origin = pixel - (dir * z_near);
            } else {
                dir = normalized(pixel - camera_position);
                origin = camera_position;
                near /= dot(dir, camera_dir);
            }
            base[texel(x, y, width)] = scene.cast(origin, dir, near, z_far);
        }
    }
    // HZBuffer::update_mips: each texel the farthest of the two to three by two to three under it.
    for (std::size_t level = 1; level < levels_.size(); ++level) {
        const int pw = sizes_[level - 1][0];
        const int ph = sizes_[level - 1][1];
        const std::vector<float>& previous = levels_[level - 1];
        const bool odd_w = (pw % 2) != 0;
        const bool odd_h = (ph % 2) != 0;
        for (int y = 0; y < sizes_[level][1]; ++y) {
            for (int x = 0; x < sizes_[level][0]; ++x) {
                const int px = x * 2;
                const int py = y * 2;
                float most = previous[texel(px, py, pw)];
                const auto check = [&](int dx, int dy) {
                    const int sx = std::min(pw - 1, px + dx);
                    const int sy = std::min(ph - 1, py + dy);
                    most = max_of(most, previous[texel(sx, sy, pw)]);
                };
                check(0, 1);
                check(1, 0);
                check(1, 1);
                if (odd_w) {
                    check(2, 0);
                    check(2, 1);
                }
                if (odd_h) {
                    check(0, 2);
                    check(1, 2);
                }
                if (odd_w && odd_h) {
                    check(2, 2);
                }
                levels_[level][texel(x, y, sizes_[level][0])] = most;
            }
        }
    }
}

bool OcclusionBuffer::hidden(const Bounds& box) const {
    if (sizes_.empty()) {
        return false;
    }
    // HZBuffer::_is_occluded.
    const Vec3 camera_position = camera_.transform.origin;
    const Vec3 closest{clamp(camera_position.x, box.min.x, box.max.x),
                       clamp(camera_position.y, box.min.y, box.max.y),
                       clamp(camera_position.z, box.min.z, box.max.z)};
    if (closest == camera_position) {
        return false;
    }
    if (inverse_.apply(closest).z > -camera_.near) {
        return false;
    }
    const Vec3 d = closest - camera_position;
    auto min_depth = static_cast<float>(std::sqrt((static_cast<double>(d.x) * d.x) +
                                                  (static_cast<double>(d.y) * d.y) +
                                                  (static_cast<double>(d.z) * d.z)));
    Vec2 rect_min{FLT_MAX, FLT_MAX};
    Vec2 rect_max{FLT_MIN, FLT_MIN};
    for (int j = 0; j < 8; ++j) {
        const Vec3 corner{(j & 4) != 0 ? box.min.x : box.max.x,
                          (j & 2) != 0 ? box.min.y : box.max.y,
                          (j & 1) != 0 ? box.min.z : box.max.z};
        const Vec3 view = inverse_.apply(corner);
        if (camera_.orthogonal) {
            min_depth = min_of(min_depth, -view.z);
        }
        if (-view.z < 0.0F) {
            rect_min = {0.0F, 0.0F};
            rect_max = {1.0F, 1.0F};
            break;
        }
        const Vec3 projected = project(camera_.projection, view);
        const Vec2 normal{(projected.x * 0.5F) + 0.5F, (projected.y * 0.5F) + 0.5F};
        rect_min = {min_of(rect_min.x, normal.x), min_of(rect_min.y, normal.y)};
        rect_max = {max_of(rect_max.x, normal.x), max_of(rect_max.y, normal.y)};
    }
    rect_max = {min_of(rect_max.x, 1.0F), min_of(rect_max.y, 1.0F)};
    rect_min = {max_of(rect_min.x, 0.0F), max_of(rect_min.y, 0.0F)};
    const auto mip_count = static_cast<int>(sizes_.size());
    const float size = max_of((rect_max.x - rect_min.x) * static_cast<float>(sizes_[0][0]),
                              (rect_max.y - rect_min.y) * static_cast<float>(sizes_[0][1]));
    int lod = clamp(ceil_log2(size), 0, mip_count - 1);
    int samples = 0;
    bool visible = true;
    for (; lod >= 0; --lod) {
        const int w = sizes_[static_cast<std::size_t>(lod)][0];
        const int h = sizes_[static_cast<std::size_t>(lod)][1];
        const auto fw = static_cast<float>(w);
        const auto fh = static_cast<float>(h);
        const auto min_x = static_cast<int>(clamp((rect_min.x * fw) - 1.0F, 0.0F, fw - 1.0F));
        const auto max_x = static_cast<int>(clamp((rect_max.x * fw) + 1.0F, 0.0F, fw - 1.0F));
        const auto min_y = static_cast<int>(clamp((rect_min.y * fh) - 1.0F, 0.0F, fh - 1.0F));
        const auto max_y = static_cast<int>(clamp((rect_max.y * fh) + 1.0F, 0.0F, fh - 1.0F));
        samples += (max_x - min_x + 1) * (max_y - min_y + 1);
        if (samples > kMaxSamples) {
            return false;
        }
        visible = false;
        const std::vector<float>& level = levels_[static_cast<std::size_t>(lod)];
        for (int y = min_y; y <= max_y && !visible; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                if (level[texel(x, y, w)] > min_depth) {
                    visible = true;
                    break;
                }
            }
        }
        if (!visible) {
            return true;
        }
    }
    return !visible;
}

bool OcclusionBuffer::occluded(const Bounds& box, std::uint64_t& timeout) const {
    const bool occluded = hidden(box);
    if (!jitter_) {
        return occluded;
    }
    if (!occluded) {
        timeout = frame_ + kOcclusionTimeout;
    } else if (timeout != 0 && frame_ >= timeout) {
        timeout = 0;
    }
    return occluded && timeout == 0;
}

OccluderMesh bake_occluder(const world::World& world, entt::entity occluder,
                           const ModelLoader& models, std::vector<Problem>& problems) {
    OccluderMesh out;
    const auto& ecs = world.ecs();
    const auto* component = ecs.try_get<const world::OccluderInstance3D>(occluder);
    if (component == nullptr) {
        return out;
    }
    const world::Tree3D tree(world);
    const world::Affine3D to_local = tree.global(occluder).inverse();
    const float simplification = component->bake_simplification_distance;
    const std::string field = "/components/MeshInstance3D/mesh";
    // OccluderInstance3D::_bake_node, over every MeshInstance3D in the world.
    for (const entt::entity entity : made<world::MeshInstance3D>(world)) {
        const auto& instance = ecs.get<const world::MeshInstance3D>(entity);
        if (!tree.visible(entity) || (instance.layers & component->bake_mask) == 0) {
            continue;
        }
        const auto* override_material = ecs.try_get<const world::StandardMaterial3D>(entity);
        if (!bakes(override_material)) {
            continue;
        }
        const world::Affine3D place = to_local * tree.global(entity);
        if (instance.mesh.empty()) {
            auto primitive = primitive_of(world, entity);
            if (!primitive) {
                continue;
            }
            const auto& shape = primitive.value();
            if (!shape) {
                continue;
            }
            auto data = make_mesh(*shape);
            if (!data) {
                continue;
            }
            bake_surface(place, data.value().vertices, data.value().indices, simplification, out);
            continue;
        }
        if (!models) {
            problems.push_back({entity, field, "models can't load here"});
            continue;
        }
        auto loaded = models(instance.mesh);
        if (!loaded) {
            problems.push_back({entity, field, loaded.error().message});
            continue;
        }
        const Model& model = *loaded.value();
        for (const ModelPart& part : model.parts) {
            const world::Affine3D part_place = place * part.transform;
            for (const ModelSurface& surface : model.meshes.at(part.mesh).surfaces) {
                // Its active material: the entity's override, else the surface's.
                const world::StandardMaterial3D* material = override_material;
                if (material == nullptr && surface.material >= 0 &&
                    static_cast<std::size_t>(surface.material) < model.materials.size()) {
                    material =
                        &model.materials[static_cast<std::size_t>(surface.material)].standard;
                }
                if (bakes(material)) {
                    bake_surface(part_place, surface.vertices, surface.indices, simplification,
                                 out);
                }
            }
        }
    }
    return out;
}

} // namespace slash::scene3d
