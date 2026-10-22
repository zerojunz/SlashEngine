#include "slash/world/tree3d.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace slash::world {

namespace {

Vec3 scaled(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// The entities with a Transform3D in the order they were made.
std::vector<entt::entity> made_order(const World& world) {
    std::vector<entt::entity> entities;
    for (const entt::entity entity : world.ecs().view<const Transform3D>()) {
        entities.push_back(entity);
    }
    std::ranges::sort(entities, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return entities;
}

// q times r: r's rotation, then q's.
Quat multiply(const Quat& q, const Quat& r) {
    return {.x = (q.w * r.x) + (q.x * r.w) + (q.y * r.z) - (q.z * r.y),
            .y = (q.w * r.y) + (q.y * r.w) + (q.z * r.x) - (q.x * r.z),
            .z = (q.w * r.z) + (q.z * r.w) + (q.x * r.y) - (q.y * r.x),
            .w = (q.w * r.w) - (q.x * r.x) - (q.y * r.y) - (q.z * r.z)};
}

} // namespace

Vec3 Affine3D::basis(Vec3 v) const {
    return {(x.x * v.x) + (y.x * v.y) + (z.x * v.z), (x.y * v.x) + (y.y * v.y) + (z.y * v.z),
            (x.z * v.x) + (y.z * v.y) + (z.z * v.z)};
}

Vec3 Affine3D::apply(Vec3 p) const {
    return basis(p) + origin;
}

Affine3D Affine3D::operator*(const Affine3D& o) const {
    return {.x = basis(o.x), .y = basis(o.y), .z = basis(o.z), .origin = apply(o.origin)};
}

Affine3D Affine3D::inverse() const {
    // The inverse of the basis is its adjugate over its determinant: the rows are the cross
    // products of pairs of its columns.
    const Vec3 r0 = cross(y, z);
    const Vec3 r1 = cross(z, x);
    const Vec3 r2 = cross(x, y);
    const float det = dot(x, r0);
    if (det == 0.0F || !std::isfinite(det)) {
        return *this;
    }
    const float inv = 1.0F / det;
    Affine3D out{.x = {r0.x * inv, r1.x * inv, r2.x * inv},
                 .y = {r0.y * inv, r1.y * inv, r2.y * inv},
                 .z = {r0.z * inv, r1.z * inv, r2.z * inv},
                 .origin = {}};
    const Vec3 moved = out.basis(origin);
    out.origin = {-moved.x, -moved.y, -moved.z};
    return out;
}

std::array<float, 16> Affine3D::matrix() const {
    return {x.x, x.y, x.z, 0.0F, y.x,      y.y,      y.z,      0.0F,
            z.x, z.y, z.z, 0.0F, origin.x, origin.y, origin.z, 1.0F};
}

Affine3D affine(const Transform3D& transform) {
    // Basis::from_euler for YXZ: ymat * xmat * zmat, written out by columns.
    const Vec2 cx = cos_sin(transform.rotation.x);
    const Vec2 cy = cos_sin(transform.rotation.y);
    const Vec2 cz = cos_sin(transform.rotation.z);
    const float c1 = cy.x;
    const float s1 = cy.y;
    const float c2 = cx.x;
    const float s2 = cx.y;
    const float c3 = cz.x;
    const float s3 = cz.y;
    const Vec3 col_x{(c1 * c3) + (s1 * s2 * s3), c2 * s3, (c1 * s2 * s3) - (s1 * c3)};
    const Vec3 col_y{(s1 * s2 * c3) - (c1 * s3), c2 * c3, (s1 * s3) + (c1 * s2 * c3)};
    const Vec3 col_z{s1 * c2, -s2, c1 * c2};
    return {.x = scaled(col_x, transform.scale.x),
            .y = scaled(col_y, transform.scale.y),
            .z = scaled(col_z, transform.scale.z),
            .origin = transform.position};
}

Quat quaternion(Vec3 rotation) {
    // The rotations about y, x and z of half each angle, multiplied in from_euler's order.
    const Vec2 hx = cos_sin(rotation.x * 0.5F);
    const Vec2 hy = cos_sin(rotation.y * 0.5F);
    const Vec2 hz = cos_sin(rotation.z * 0.5F);
    const Quat qx{.x = hx.y, .y = 0.0F, .z = 0.0F, .w = hx.x};
    const Quat qy{.x = 0.0F, .y = hy.y, .z = 0.0F, .w = hy.x};
    const Quat qz{.x = 0.0F, .y = 0.0F, .z = hz.y, .w = hz.x};
    return multiply(multiply(qy, qx), qz);
}

Affine3D rotation_basis(const Quat& q) {
    const float d = (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w);
    if (d == 0.0F) {
        return {};
    }
    const float s = 2.0F / d;
    const float xs = q.x * s;
    const float ys = q.y * s;
    const float zs = q.z * s;
    const float wx = q.w * xs;
    const float wy = q.w * ys;
    const float wz = q.w * zs;
    const float xx = q.x * xs;
    const float xy = q.x * ys;
    const float xz = q.x * zs;
    const float yy = q.y * ys;
    const float yz = q.y * zs;
    const float zz = q.z * zs;
    return {.x = {1.0F - (yy + zz), xy + wz, xz - wy},
            .y = {xy - wz, 1.0F - (xx + zz), yz + wx},
            .z = {xz + wy, yz - wx, 1.0F - (xx + yy)},
            .origin = {}};
}

Vec3 euler(const Affine3D& rotation) {
    // Godot's Basis::get_euler for EulerOrder::YXZ, where rows[r][c] is column c's row r. asin
    // goes through atan2, which is the same everywhere.
    constexpr float kEpsilon = 0.00001F; // Godot's CMP_EPSILON
    constexpr float kHalfPi = 1.57079632679489661923F;
    const auto asin = [](float v) { return slash::atan2(v, std::sqrt(std::max(0.0F, 1.0F - (v * v)))); };
    const float m12 = rotation.z.y;
    if (m12 >= 1.0F - kEpsilon) {
        return {-kHalfPi, -slash::atan2(rotation.y.x, rotation.x.x), 0.0F};
    }
    if (m12 <= -(1.0F - kEpsilon)) {
        return {kHalfPi, slash::atan2(rotation.y.x, rotation.x.x), 0.0F};
    }
    // A turn about x alone comes back in its simplest form.
    if (rotation.x.y == 0.0F && rotation.y.x == 0.0F && rotation.z.x == 0.0F &&
        rotation.x.z == 0.0F && rotation.x.x == 1.0F) {
        return {slash::atan2(-m12, rotation.y.y), 0.0F, 0.0F};
    }
    return {asin(-m12), slash::atan2(rotation.z.x, rotation.z.z),
            slash::atan2(rotation.x.y, rotation.y.y)};
}

Quat nlerp(const Quat& a, const Quat& b, float weight) {
    const float d = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    const float sign = d < 0.0F ? -1.0F : 1.0F;
    const float keep = 1.0F - weight;
    const float take = weight * sign;
    const Quat q{.x = (a.x * keep) + (b.x * take),
                 .y = (a.y * keep) + (b.y * take),
                 .z = (a.z * keep) + (b.z * take),
                 .w = (a.w * keep) + (b.w * take)};
    const float length = std::sqrt((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    if (length == 0.0F) {
        return b;
    }
    return {.x = q.x / length, .y = q.y / length, .z = q.z / length, .w = q.w / length};
}

Tree3D::Tree3D(const World& world) : world_(&world) {
    const auto& ecs = world.ecs();
    const std::vector<entt::entity> entities = made_order(world);
    for (const entt::entity entity : entities) {
        for (const entt::entity child : ecs.get<Transform3D>(entity).children) {
            if (child == entity || !world.valid(child) || !ecs.all_of<Transform3D>(child)) {
                continue;
            }
            parents_.try_emplace(child, entity);
        }
    }
    // A cycle is broken at its first entity in the order they were made, which becomes a root.
    for (const entt::entity entity : entities) {
        const auto link = parents_.find(entity);
        if (link == parents_.end()) {
            continue;
        }
        entt::entity up = link->second;
        for (std::size_t steps = 0; steps <= parents_.size() && up != entt::null; ++steps) {
            if (up == entity) {
                parents_.erase(link);
                break;
            }
            up = parent(up);
        }
    }
    order_.reserve(entities.size());
    std::vector<entt::entity> stack;
    for (const entt::entity root : entities) {
        if (parents_.contains(root)) {
            continue;
        }
        stack.push_back(root);
        while (!stack.empty()) {
            const entt::entity entity = stack.back();
            stack.pop_back();
            order_.push_back(entity);
            for (const entt::entity child :
                 std::views::reverse(ecs.get<Transform3D>(entity).children)) {
                if (const auto found = parents_.find(child);
                    found != parents_.end() && found->second == entity) {
                    stack.push_back(child);
                }
            }
        }
    }
}

entt::entity Tree3D::parent(entt::entity entity) const {
    const auto found = parents_.find(entity);
    return found != parents_.end() ? found->second : entt::entity{entt::null};
}

entt::entity Tree3D::transform_parent(entt::entity entity) const {
    const entt::entity up = parent(entity);
    if (up == entt::null || world_->ecs().get<Transform3D>(entity).top_level) {
        return entt::null;
    }
    return up;
}

Affine3D Tree3D::global(entt::entity entity) const {
    return global(entity, {});
}

Affine3D Tree3D::global(entt::entity entity, const Locals& locals) const {
    const auto* own = world_->valid(entity) ? world_->ecs().try_get<Transform3D>(entity) : nullptr;
    if (own == nullptr) {
        return {};
    }
    const auto replaced = locals.find(entity);
    const Affine3D local = replaced != locals.end() ? replaced->second : affine(*own);
    const entt::entity up = own->top_level ? entt::entity{entt::null} : parent(entity);
    if (up == entt::null) {
        return local;
    }
    return global(up, locals) * local;
}

bool Tree3D::visible(entt::entity entity) const {
    const auto& ecs = world_->ecs();
    for (std::size_t steps = 0; entity != entt::null && steps <= parents_.size(); ++steps) {
        const auto* own = world_->valid(entity) ? ecs.try_get<Transform3D>(entity) : nullptr;
        if (own == nullptr) {
            return true;
        }
        if (!own->visible) {
            return false;
        }
        entity = parent(entity);
    }
    return true;
}

} // namespace slash::world
