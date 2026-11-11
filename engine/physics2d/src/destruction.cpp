// 2D destruction, as physics3d's (Godot has none; Chaos's geometry collections and connection
// graph are the model): a Destructible2D's outline is cut into cells (world::fracture_sites)
// with Clipper2, and each piece becomes a RigidBody2D with a CollisionPolygon2D and a Polygon2D
// drawing its part of the body's look (FracturePiece2D). Health, damage states, levels of
// pieces, linked standing pieces and their stress follow the body's .damage.json profile as in
// 3D, with y down.

#include "space.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/cloth2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::physics2d {

namespace {

using world::Destructible2D;
using world::FracturePiece2D;
using world::Transform2D;
using Polygon = std::vector<Vec2>;

constexpr int kClipperPrecision = 3; // decimals of a pixel Clipper2 keeps
constexpr float kPixelsPerMeter = 64.0F;

std::uint32_t hash_id(std::string_view id) {
    std::uint32_t h = 2166136261U;
    for (const char c : id) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return h;
}

real_t area_of(const Polygon& polygon) {
    real_t twice = 0.0F;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[(i + 1) % polygon.size()];
        twice += (a.x * b.y) - (b.x * a.y);
    }
    return twice * 0.5F;
}

Vec2 centroid_of(const Polygon& polygon) {
    real_t twice = 0.0F;
    Vec2 sum;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[(i + 1) % polygon.size()];
        const real_t cross = (a.x * b.y) - (b.x * a.y);
        twice += cross;
        sum = sum + Vec2{(a.x + b.x) * cross, (a.y + b.y) * cross};
    }
    if (std::abs(twice) < 1e-9F) {
        Vec2 mean;
        for (const Vec2 p : polygon) {
            mean = mean + p;
        }
        return polygon.empty() ? mean : mean * (1.0F / static_cast<real_t>(polygon.size()));
    }
    return sum * (1.0F / (3.0F * twice));
}

std::pair<Vec2, Vec2> bounds_of(const Polygon& polygon) {
    Vec2 low{1e30F, 1e30F};
    Vec2 high{-1e30F, -1e30F};
    for (const Vec2 p : polygon) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
    }
    return {low, high};
}

// The outline a body breaks along, in its own space: its CollisionPolygon2D's, its
// CollisionShape2D's or its Sprite2D's region, counterclockwise as Clipper2 likes it.
std::optional<Polygon> outline_of(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    Polygon out;
    if (const auto* polygon = ecs.try_get<const world::CollisionPolygon2D>(entity);
        polygon != nullptr && polygon->polygon.size() >= 3) {
        out = polygon->polygon;
    } else if (const auto* shape = ecs.try_get<const world::CollisionShape2D>(entity);
               shape != nullptr) {
        const Vec2f turn = cos_sin(shape->rotation);
        const auto place = [&](Vec2 p) {
            return Vec2{shape->position.x + (p.x * turn.x) - (p.y * turn.y),
                        shape->position.y + (p.x * turn.y) + (p.y * turn.x)};
        };
        if (shape->shape == "rectangle") {
            const Vec2 h = shape->size * 0.5F;
            out = {place({-h.x, -h.y}), place({h.x, -h.y}), place({h.x, h.y}), place({-h.x, h.y})};
        } else if (shape->shape == "circle" || shape->shape == "capsule") {
            constexpr int kAround = 24;
            const real_t half = shape->shape == "capsule"
                                    ? std::max<real_t>((shape->height * 0.5F) - shape->radius, 0.0F)
                                    : 0.0F;
            for (int i = 0; i < kAround; ++i) {
                const Vec2f cs = cos_sin(static_cast<float>(i) * 2.0F * std::numbers::pi_v<float> /
                                         static_cast<float>(kAround));
                const real_t shift = cs.y >= 0.0F ? half : -half;
                out.push_back(place({cs.x * shape->radius, (cs.y * shape->radius) + shift}));
            }
        } else if (shape->shape == "polygon" && shape->points.size() >= 3) {
            for (const Vec2 p : shape->points) {
                out.push_back(place(p));
            }
        }
    } else if (const auto* sprite = ecs.try_get<const world::Sprite2D>(entity);
               sprite != nullptr && sprite->region.z > 0.0F && sprite->region.w > 0.0F) {
        const Vec2 size{sprite->region.z, sprite->region.w};
        const Vec2 low = (sprite->centered ? size * -0.5F : Vec2{}) + sprite->offset;
        out = {low, {low.x + size.x, low.y}, low + size, {low.x, low.y + size.y}};
    }
    if (out.size() < 3) {
        return std::nullopt;
    }
    if (area_of(out) < 0.0F) {
        std::ranges::reverse(out);
    }
    return out;
}

Clipper2Lib::PathD to_path(const Polygon& polygon) {
    Clipper2Lib::PathD path;
    path.reserve(polygon.size());
    for (const Vec2 p : polygon) {
        path.emplace_back(static_cast<double>(p.x), static_cast<double>(p.y));
    }
    return path;
}

Polygon to_polygon(const Clipper2Lib::PathD& path) {
    Polygon out;
    out.reserve(path.size());
    for (const auto& p : path) {
        out.push_back({static_cast<real_t>(p.x), static_cast<real_t>(p.y)});
    }
    if (area_of(out) < 0.0F) {
        std::ranges::reverse(out);
    }
    return out;
}

// One piece of a cut outline.
struct Piece2D {
    Polygon polygon;
    std::vector<std::size_t> sites;
    std::vector<std::pair<std::size_t, float>> neighbors; // other piece, pixels of shared edge
};

std::vector<Piece2D> cut_outline(const Polygon& outline, const world::FractureCut& cut) {
    const std::pair<Vec2, Vec2> bounds = bounds_of(outline);
    const Vec2 low = bounds.first;
    const Vec2 high = bounds.second;
    const world::FractureSites sites =
        world::fracture_sites(cut, {low.x, low.y, 0.0F}, {high.x, high.y, 0.0F}, true);
    const std::size_t count = sites.points.size();
    const auto site = [&](std::size_t i) {
        return Vec2{static_cast<real_t>(sites.points[i][0]),
                    static_cast<real_t>(sites.points[i][1])};
    };
    const Vec2 size = high - low;
    const real_t pad = 1.0F + (0.1F * std::max(size.x, size.y));
    // Each point's cell: a box around the outline cut by the bisectors with every other point.
    const auto cell_of = [&](std::size_t i) {
        Polygon cell{{low.x - pad, low.y - pad},
                     {high.x + pad, low.y - pad},
                     {high.x + pad, high.y + pad},
                     {low.x - pad, high.y + pad}};
        const Vec2 p = site(i);
        for (std::size_t j = 0; j < count && cell.size() >= 3; ++j) {
            const Vec2 q = site(j);
            const Vec2 n = p - q;
            if (j == i || (n.x == 0.0F && n.y == 0.0F)) {
                continue;
            }
            const Vec2 mid = (p + q) * 0.5F;
            const auto side = [&](Vec2 v) { return ((v.x - mid.x) * n.x) + ((v.y - mid.y) * n.y); };
            Polygon kept;
            for (std::size_t k = 0; k < cell.size(); ++k) {
                const Vec2 a = cell[k];
                const Vec2 b = cell[(k + 1) % cell.size()];
                const real_t sa = side(a);
                const real_t sb = side(b);
                if (sa >= 0.0F) {
                    kept.push_back(a);
                }
                if ((sa >= 0.0F) != (sb >= 0.0F)) {
                    const real_t t = sa / (sa - sb);
                    kept.push_back(a + ((b - a) * t));
                }
            }
            cell = std::move(kept);
        }
        return cell;
    };
    std::vector<std::vector<std::size_t>> groups;
    if (sites.groups.empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            groups.push_back({i});
        }
    } else {
        groups.resize(static_cast<std::size_t>(std::max(cut.cells, 0)));
        for (std::size_t i = 0; i < count; ++i) {
            groups[static_cast<std::size_t>(sites.groups[i])].push_back(i);
        }
    }
    const Clipper2Lib::PathsD subject{to_path(outline)};
    std::vector<Piece2D> out;
    std::vector<int> owner(count, -1);
    for (const std::vector<std::size_t>& group : groups) {
        if (group.empty()) {
            continue;
        }
        Clipper2Lib::PathsD cells;
        for (const std::size_t i : group) {
            if (const Polygon cell = cell_of(i); cell.size() >= 3) {
                cells.push_back(to_path(cell));
            }
        }
        if (cells.size() > 1) {
            cells = Clipper2Lib::Union(cells, Clipper2Lib::FillRule::NonZero, kClipperPrecision);
        }
        const Clipper2Lib::PathsD shares = Clipper2Lib::Intersect(
            subject, cells, Clipper2Lib::FillRule::NonZero, kClipperPrecision);
        for (const Clipper2Lib::PathD& share : shares) {
            Polygon polygon = to_polygon(share);
            if (polygon.size() < 3 || std::abs(area_of(polygon)) < 0.5F) {
                continue;
            }
            for (const std::size_t i : group) {
                owner[i] = static_cast<int>(out.size());
            }
            out.push_back({.polygon = std::move(polygon), .sites = group, .neighbors = {}});
        }
    }
    // Edges on a bisector between two pieces' points are shared.
    const real_t eps = 0.01F + (1e-4F * std::max(size.x, size.y));
    for (std::size_t piece = 0; piece < out.size(); ++piece) {
        std::vector<float> lengths(out.size(), 0.0F);
        const Polygon& polygon = out[piece].polygon;
        for (std::size_t k = 0; k < polygon.size(); ++k) {
            const Vec2 a = polygon[k];
            const Vec2 b = polygon[(k + 1) % polygon.size()];
            const Vec2 d = b - a;
            const real_t edge = std::sqrt((d.x * d.x) + (d.y * d.y));
            bool found = false;
            for (const std::size_t s : out[piece].sites) {
                for (std::size_t t = 0; t < count && !found; ++t) {
                    const int other = owner[t];
                    if (other < 0 || static_cast<std::size_t>(other) == piece) {
                        continue;
                    }
                    const Vec2 n = site(s) - site(t);
                    const real_t len = std::sqrt((n.x * n.x) + (n.y * n.y));
                    if (len == 0.0F) {
                        continue;
                    }
                    const Vec2 unit = n * (1.0F / len);
                    const Vec2 mid = (site(s) + site(t)) * 0.5F;
                    const auto off = [&](Vec2 v) {
                        return std::abs(((v.x - mid.x) * unit.x) + ((v.y - mid.y) * unit.y));
                    };
                    if (off(a) < eps && off(b) < eps) {
                        lengths[static_cast<std::size_t>(other)] += edge;
                        found = true;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
        for (std::size_t other = 0; other < lengths.size(); ++other) {
            if (lengths[other] > 0.0F) {
                out[piece].neighbors.emplace_back(other, lengths[other]);
            }
        }
    }
    return out;
}

// What the groups of standing pieces need: settling again after a link or piece went.
struct DestructionState2D {
    std::set<std::string> unsettled;
    bool started = false;
};

DestructionState2D& state_of(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<DestructionState2D>() == nullptr) {
        ctx.emplace<DestructionState2D>();
        world.add_reset([](world::World& w) {
            if (auto* state = w.ecs().ctx().find<DestructionState2D>(); state != nullptr) {
                *state = {};
            }
        });
    }
    return ctx.get<DestructionState2D>();
}

const world::DamageProfile* profile_of(const world::World& world,
                                       const Destructible2D& destructible,
                                       std::shared_ptr<const world::DamageProfile>& keep) {
    if (destructible.profile.empty()) {
        return nullptr;
    }
    auto loaded = world::load_damage(world, destructible.profile, destructible.overrides);
    if (!loaded) {
        return nullptr;
    }
    keep = loaded.value();
    return keep.get();
}

// A PNG file's size from its header, for a sprite's texture whose region isn't set.
std::optional<Vec2> png_size(const world::World& world, const std::string& path) {
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr || !path.ends_with(".png")) {
        return std::nullopt;
    }
    auto bytes = assets->read(path, "texture");
    if (!bytes || bytes.value().size() < 24) {
        return std::nullopt;
    }
    const std::string& b = bytes.value();
    const auto word = [&](std::size_t at) {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24U) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16U) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8U) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
    };
    return Vec2{static_cast<real_t>(word(16)), static_cast<real_t>(word(20))};
}

// The look a piece draws: the body's Polygon2D with its outline (its texture coordinates mapped
// as the body's were), or its Sprite2D's texture over it.
std::optional<world::Polygon2D> look_of(const world::World& world, entt::entity entity,
                                        const Polygon& piece) {
    const auto& ecs = world.ecs();
    if (const auto* polygon = ecs.try_get<const world::Polygon2D>(entity); polygon != nullptr) {
        world::Polygon2D out = *polygon;
        out.polygon = piece;
        out.polygons.clear();
        out.vertex_colors.clear();
        out.uv.clear();
        if (polygon->uv.size() >= 3 && polygon->uv.size() == polygon->polygon.size()) {
            // The body's mapping from places to texture coordinates, as an affine fit (exact for
            // the rectangles and turned shapes most are), from three of its corners.
            // Three corners as far apart as can be: the first, the one farthest from it and
            // the one farthest off the line through both, so a dented outline still fits.
            const std::vector<Vec2>& points = polygon->polygon;
            std::size_t i1 = 1;
            std::size_t i2 = points.size() - 1;
            real_t far = -1.0F;
            for (std::size_t i = 1; i < points.size(); ++i) {
                const Vec2 d = points[i] - points[0];
                if (const real_t d2 = (d.x * d.x) + (d.y * d.y); d2 > far) {
                    far = d2;
                    i1 = i;
                }
            }
            far = -1.0F;
            for (std::size_t i = 1; i < points.size(); ++i) {
                const Vec2 a = points[i1] - points[0];
                const Vec2 b = points[i] - points[0];
                if (const real_t off = std::abs((a.x * b.y) - (a.y * b.x)); off > far) {
                    far = off;
                    i2 = i;
                }
            }
            const Vec2 p0 = points[0];
            const Vec2 e1 = points[i1] - p0;
            const Vec2 e2 = points[i2] - p0;
            const real_t det = (e1.x * e2.y) - (e1.y * e2.x);
            if (std::abs(det) > 1e-9F) {
                const Vec2 u0 = polygon->uv[0];
                const Vec2 f1 = polygon->uv[i1] - u0;
                const Vec2 f2 = polygon->uv[i2] - u0;
                for (const Vec2 p : piece) {
                    const Vec2 d = p - p0;
                    const real_t a = ((d.x * e2.y) - (d.y * e2.x)) / det;
                    const real_t c = ((e1.x * d.y) - (e1.y * d.x)) / det;
                    out.uv.push_back(u0 + (f1 * a) + (f2 * c));
                }
            }
        }
        return out;
    }
    if (const auto* sprite = ecs.try_get<const world::Sprite2D>(entity); sprite != nullptr) {
        world::Polygon2D out;
        out.polygon = piece;
        out.texture = sprite->texture;
        out.modulate = sprite->modulate;
        out.z_index = sprite->z_index;
        out.z_as_relative = sprite->z_as_relative;
        out.visible = sprite->visible;
        out.light_mask = sprite->light_mask;
        out.texture_filter = sprite->texture_filter;
        Vec2 size{sprite->region.z, sprite->region.w};
        Vec2 origin{sprite->region.x, sprite->region.y};
        if (size.x <= 0.0F || size.y <= 0.0F) {
            size = png_size(world, sprite->texture).value_or(Vec2{});
            origin = {};
        }
        const Vec2 low = (sprite->centered ? size * -0.5F : Vec2{}) + sprite->offset;
        for (const Vec2 p : piece) {
            Vec2 uv = p - low;
            if (sprite->flip_h) {
                uv.x = size.x - uv.x;
            }
            if (sprite->flip_v) {
                uv.y = size.y - uv.y;
            }
            out.uv.push_back(origin + uv);
        }
        return out;
    }
    return std::nullopt;
}

// The ID a piece takes: the body's, then "_" and its number, or the next free one after it.
Result<entt::entity> create_piece(world::World& world, std::string_view body, std::size_t number) {
    if (body.empty()) {
        return world.create();
    }
    for (std::size_t extra = 0;; ++extra) {
        std::string id = std::string(body) + "_" + std::to_string(number);
        if (extra > 0) {
            id += "_" + std::to_string(extra);
        }
        if (world.find(id) == entt::null) {
            return world.create(id);
        }
    }
}

void set_loose(world::World& world, entt::entity entity, float lifetime) {
    auto& ecs = world.ecs();
    auto* piece = ecs.try_get<FracturePiece2D>(entity);
    if (piece == nullptr) {
        return;
    }
    if (auto* rigid = ecs.try_get<world::RigidBody2D>(entity); rigid != nullptr && rigid->freeze) {
        rigid->freeze = false;
        rigid->sleeping = false;
        if (piece->lifetime <= 0.0F) {
            piece->lifetime = lifetime;
        }
    }
    const std::string id(world.id_of(entity));
    for (const world::FractureLink& link : piece->links) {
        if (const entt::entity other = world.find(link.piece); other != entt::null) {
            if (auto* there = ecs.try_get<FracturePiece2D>(other); there != nullptr) {
                std::erase_if(there->links,
                              [&](const world::FractureLink& l) { return l.piece == id; });
            }
        }
    }
    piece->links.clear();
    piece->anchored = false;
    if (!piece->group.empty()) {
        state_of(world).unsettled.insert(piece->group);
    }
}

void show_state(world::World& world, entt::entity entity, const world::DamageProfile& profile,
                int from, int to) {
    auto& ecs = world.ecs();
    const auto state_at = [&](int s) -> const world::DamageState* {
        return s > 0 && static_cast<std::size_t>(s) <= profile.states.size()
                   ? &profile.states[static_cast<std::size_t>(s - 1)]
                   : nullptr;
    };
    const world::DamageState* before = state_at(from);
    const world::DamageState* after = state_at(to);
    const auto retint = [&](Vec4& color) {
        if (before != nullptr && before->tinted) {
            const auto undo = [](float c, float t) { return t > 0.0F ? c / t : c; };
            color = {undo(color.x, before->tint.x), undo(color.y, before->tint.y),
                     undo(color.z, before->tint.z), undo(color.w, before->tint.w)};
        }
        if (after != nullptr && after->tinted) {
            color = {color.x * after->tint.x, color.y * after->tint.y, color.z * after->tint.z,
                     color.w * after->tint.w};
        }
    };
    if (auto* sprite = ecs.try_get<world::Sprite2D>(entity); sprite != nullptr) {
        retint(sprite->modulate);
        if (after != nullptr && !after->mesh.empty()) {
            sprite->texture = after->mesh;
        }
    }
    if (auto* polygon = ecs.try_get<world::Polygon2D>(entity); polygon != nullptr) {
        retint(polygon->modulate);
        if (after != nullptr && !after->mesh.empty() && !ecs.all_of<FracturePiece2D>(entity)) {
            polygon->texture = after->mesh;
        }
    }
}

// How far a point is from an outline: 0 inside it, else to its nearest edge.
real_t outside_by(const Polygon& outline, Vec2 p) {
    bool inside = false;
    real_t nearest = 1e30F;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const Vec2 a = outline[i];
        const Vec2 b = outline[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < ((b.x - a.x) * (p.y - a.y) / (b.y - a.y)) + a.x) {
            inside = !inside;
        }
        const Vec2 ab = b - a;
        const real_t len2 = (ab.x * ab.x) + (ab.y * ab.y);
        const real_t t =
            len2 > 0.0F
                ? std::clamp<real_t>((((p - a).x * ab.x) + ((p - a).y * ab.y)) / len2, 0.0F, 1.0F)
                : 0.0F;
        const Vec2 d = p - (a + (ab * t));
        nearest = std::min<real_t>(nearest, std::sqrt((d.x * d.x) + (d.y * d.y)));
    }
    return inside ? 0.0F : nearest;
}

// The nearest point of an outline's edges to a point.
Vec2 nearest_on(const Polygon& outline, Vec2 p) {
    Vec2 best = outline.front();
    real_t best_d2 = 1e30F;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const Vec2 a = outline[j];
        const Vec2 ab = outline[i] - a;
        const real_t len2 = (ab.x * ab.x) + (ab.y * ab.y);
        const real_t t =
            len2 > 0.0F
                ? std::clamp<real_t>((((p - a).x * ab.x) + ((p - a).y * ab.y)) / len2, 0.0F, 1.0F)
                : 0.0F;
        const Vec2 q = a + (ab * t);
        const Vec2 d = p - q;
        if (const real_t d2 = (d.x * d.x) + (d.y * d.y); d2 < best_d2) {
            best_d2 = d2;
            best = q;
        }
    }
    return best;
}

// A circle as a polygon, made with the engine's sines.
Polygon circle(Vec2 center, real_t radius) {
    constexpr int kAround = 24;
    Polygon out;
    out.reserve(kAround);
    for (int i = 0; i < kAround; ++i) {
        const Vec2f cs = cos_sin(static_cast<float>(i) * 2.0F * std::numbers::pi_v<float> /
                                 static_cast<float>(kAround));
        out.push_back({center.x + (cs.x * radius), center.y + (cs.y * radius)});
    }
    return out;
}

// What is left of outlines with holes cut into them, as simple outlines: each hole opened to the
// outline it is in by a slot a pixel wide, leftward from its leftmost corner, so a body keeps one
// outline Box2D and drawing take; largest first.
std::vector<Polygon> open_holes(Clipper2Lib::PathsD paths) {
    constexpr int kTries = 16;
    for (int attempt = 0; attempt < kTries; ++attempt) {
        const auto hole = std::ranges::find_if(
            paths, [](const Clipper2Lib::PathD& path) { return Clipper2Lib::Area(path) < 0.0; });
        if (hole == paths.end()) {
            break;
        }
        const auto left =
            std::ranges::min_element(*hole, {}, [](const Clipper2Lib::PointD& p) { return p.x; });
        double reach = left->x;
        for (const Clipper2Lib::PathD& path : paths) {
            for (const auto& p : path) {
                reach = std::min(reach, p.x);
            }
        }
        const double half = 0.5;
        const Clipper2Lib::PathsD slot{{{reach - 1.0, left->y - half},
                                        {left->x + half, left->y - half},
                                        {left->x + half, left->y + half},
                                        {reach - 1.0, left->y + half}}};
        // The slot runs out of the outline only through what lies left of the hole, which it
        // opens to the outside.
        paths =
            Clipper2Lib::Difference(paths, slot, Clipper2Lib::FillRule::NonZero, kClipperPrecision);
    }
    std::vector<Polygon> out;
    for (const Clipper2Lib::PathD& path : paths) {
        if (Clipper2Lib::Area(path) > 0.0) {
            out.push_back(to_polygon(path));
        }
    }
    std::ranges::stable_sort(out, std::greater<>{}, [](const Polygon& p) { return area_of(p); });
    return out;
}

// An outline with a dent pushed into it: its edges near the hit split to a third of the dent's
// radius, then each corner within it moved along direction by depth (1 - (d/r)^2)^2.
Polygon dented(const Polygon& outline, Vec2 at, Vec2 direction, real_t radius, real_t depth) {
    Polygon fine;
    const real_t step = std::max<real_t>(radius / 3.0F, 0.5F);
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const Vec2 a = outline[i];
        const Vec2 b = outline[(i + 1) % outline.size()];
        fine.push_back(a);
        const Vec2 ab = b - a;
        const real_t length = std::sqrt((ab.x * ab.x) + (ab.y * ab.y));
        // Only edges that pass near the dent are split.
        const real_t len2 = length * length;
        const real_t t =
            len2 > 0.0F
                ? std::clamp<real_t>((((at - a).x * ab.x) + ((at - a).y * ab.y)) / len2, 0.0F, 1.0F)
                : 0.0F;
        const Vec2 d = at - (a + (ab * t));
        if ((d.x * d.x) + (d.y * d.y) >= radius * radius || length <= step) {
            continue;
        }
        const int parts = std::min(static_cast<int>(length / step), 256);
        for (int k = 1; k < parts; ++k) {
            fine.push_back(a + (ab * (static_cast<real_t>(k) / static_cast<real_t>(parts))));
        }
    }
    const real_t r2 = radius * radius;
    for (Vec2& p : fine) {
        const Vec2 d = p - at;
        const real_t d2 = (d.x * d.x) + (d.y * d.y);
        if (d2 < r2) {
            const real_t t = 1.0F - (d2 / r2);
            p = p + (direction * (depth * t * t));
        }
    }
    return fine;
}

} // namespace

void set_tile_carver(world::World& world, TileCarver carver) {
    world.ecs().ctx().insert_or_assign<TileCarver>(std::move(carver));
}

std::string deform_body_2d(world::World& world, entt::entity entity,
                           const world::DamageProfile& profile, float dealt, std::string_view kind,
                           std::optional<Vec2> at, std::optional<Vec2> direction) {
    auto& ecs = world.ecs();
    const auto* destructible = world.valid(entity) ? ecs.try_get<Destructible2D>(entity) : nullptr;
    const world::DeformSettings& deform = profile.deform;
    if (destructible == nullptr || dealt <= 0.0F || !at.has_value()) {
        return {};
    }
    const bool explosive = kind == "explosive";
    // A tile map layer loses the cells a blast reaches.
    if (ecs.all_of<world::TileMapLayer>(entity)) {
        const auto* carver = ecs.ctx().find<TileCarver>();
        if (!explosive || deform.carve <= 0.0F || carver == nullptr || !*carver) {
            return {};
        }
        const float radius = std::min(dealt * deform.carve, deform.max_carve) * kPixelsPerMeter;
        return (*carver)(world, entity, *at, radius, true) > 0 ? "carve" : "";
    }
    const auto outline = outline_of(world, entity);
    if (!outline.has_value()) {
        return {};
    }
    const world::Tree2D tree(world);
    const Transform2D place = tree.global(entity);
    const world::Affine2D global = world::affine(place);
    const world::Affine2D inverse = global.inverse();
    const Vec2 local = inverse.apply(*at);
    const real_t scale =
        std::max<real_t>(std::sqrt(std::abs(place.scale.x * place.scale.y)), 1e-6F);
    const Vec2 surface = outside_by(*outline, local) > 0.0F ? nearest_on(*outline, local) : local;
    const Vec2 middle = centroid_of(*outline);
    Vec2 along = middle - surface;
    if (direction.has_value()) {
        along = inverse.apply(*at + *direction) - local;
    }
    if (const real_t size = std::sqrt((along.x * along.x) + (along.y * along.y)); size > 1e-6F) {
        along = along * (1.0F / size);
    } else {
        along = {0.0F, 1.0F};
    }
    std::string done;
    Clipper2Lib::PathsD left;
    if (explosive && deform.carve > 0.0F) {
        const real_t radius =
            std::min(dealt * deform.carve, deform.max_carve) * kPixelsPerMeter / scale;
        if (outside_by(*outline, local) >= radius) {
            return {};
        }
        left = Clipper2Lib::Difference(Clipper2Lib::PathsD{to_path(*outline)},
                                       Clipper2Lib::PathsD{to_path(circle(local, radius))},
                                       Clipper2Lib::FillRule::NonZero, kClipperPrecision);
        done = "carve";
    } else if (kind == "pierce" && deform.hole > 0.0F) {
        // A hole the bullet's size, just inside where it went in.
        const real_t radius =
            std::min(dealt * deform.hole, deform.max_hole) * kPixelsPerMeter / scale;
        left = Clipper2Lib::Difference(
            Clipper2Lib::PathsD{to_path(*outline)},
            Clipper2Lib::PathsD{to_path(circle(surface + (along * (radius * 1.5F)), radius))},
            Clipper2Lib::FillRule::NonZero, kClipperPrecision);
        done = "hole";
    } else if ((kind == "blunt" || kind == "impact" || explosive) && deform.dent > 0.0F) {
        const auto [low, high] = bounds_of(*outline);
        const real_t thin = std::min(high.x - low.x, high.y - low.y);
        const real_t depth = std::min<real_t>(
            std::min(dealt * deform.dent, deform.max_dent) * kPixelsPerMeter / scale, 0.45F * thin);
        const Polygon made =
            dented(*outline, surface, along, deform.dent_radius * kPixelsPerMeter / scale, depth);
        // A dent that folds the outline over itself is cleaned up as what it covers.
        left = Clipper2Lib::Union(Clipper2Lib::PathsD{to_path(made)},
                                  Clipper2Lib::FillRule::NonZero, kClipperPrecision);
        done = "dent";
    } else {
        return {};
    }
    std::vector<Polygon> parts = open_holes(std::move(left));
    if (parts.empty()) {
        return {};
    }
    // The largest part stays the body; the rest fall away as debris.
    const real_t total = area_of(*outline);
    auto look = look_of(world, entity, parts.front());
    for (std::size_t i = 1; i < parts.size(); ++i) {
        auto created = create_piece(world, world.id_of(entity), i);
        if (!created) {
            continue;
        }
        const entt::entity out = created.value();
        ecs.emplace<Transform2D>(out, place);
        if (auto part_look = look_of(world, entity, parts[i]); part_look.has_value()) {
            ecs.emplace<world::Polygon2D>(out, std::move(*part_look));
        }
        ecs.emplace<world::CollisionPolygon2D>(out, world::CollisionPolygon2D{.polygon = parts[i]});
        world::RigidBody2D body;
        const real_t area = std::abs(area_of(parts[i])) * scale * scale;
        body.mass = std::max(
            static_cast<float>(area / (kPixelsPerMeter * kPixelsPerMeter) * profile.density),
            0.001F);
        if (const auto* fixed = ecs.try_get<const world::StaticBody2D>(entity); fixed != nullptr) {
            body.collision_layer = fixed->collision_layer;
            body.collision_mask = fixed->collision_mask;
        } else if (const auto* rigid = ecs.try_get<const world::RigidBody2D>(entity);
                   rigid != nullptr) {
            body.collision_layer = rigid->collision_layer;
            body.collision_mask = rigid->collision_mask;
            body.linear_velocity = rigid->linear_velocity;
        }
        ecs.emplace<world::RigidBody2D>(out, body);
        ecs.emplace<FracturePiece2D>(out, FracturePiece2D{.lifetime = profile.debris.lifetime,
                                                          .group = {},
                                                          .links = {},
                                                          .anchored = false});
    }
    // The body's own outline is what is left, drawn as before.
    if (auto* rigid = ecs.try_get<world::RigidBody2D>(entity);
        rigid != nullptr && total > 0.0F && rigid->center_of_mass_mode == "auto") {
        rigid->mass =
            std::max(static_cast<float>(rigid->mass * area_of(parts.front()) / total), 0.001F);
    }
    ecs.remove<world::CollisionShape2D>(entity);
    if (auto* shape = ecs.try_get<world::CollisionPolygon2D>(entity); shape != nullptr) {
        shape->polygon = parts.front();
    } else {
        ecs.emplace<world::CollisionPolygon2D>(entity,
                                               world::CollisionPolygon2D{.polygon = parts.front()});
    }
    if (look.has_value()) {
        ecs.remove<world::Sprite2D>(entity);
        ecs.emplace_or_replace<world::Polygon2D>(entity, std::move(*look));
    }
    return done;
}

std::vector<int> tear_cloth_2d(world::World& world, entt::entity entity, Vec2 center,
                               float radius) {
    auto* cloth = world.valid(entity) ? world.ecs().try_get<world::Cloth2D>(entity) : nullptr;
    if (cloth == nullptr || radius <= 0.0F) {
        return {};
    }
    const world::ClothGrid2D grid = world::cloth_grid_2d(*cloth);
    std::vector<Vec2> points;
    if (const world::ClothPoints2D* simulated = world::cloth_points_2d(world, entity);
        simulated != nullptr && simulated->current.size() == grid.down.size()) {
        points = simulated->current;
    } else {
        points = world::cloth_rest_2d(*cloth, world::Tree2D(world).global_affine(entity));
    }
    std::vector<int> tore;
    const real_t reach = static_cast<real_t>(radius) * radius;
    for (std::size_t e = 0; e < grid.edges.size(); ++e) {
        const Vec2 middle = (points[static_cast<std::size_t>(grid.edges[e][0])] +
                             points[static_cast<std::size_t>(grid.edges[e][1])]) *
                            0.5F;
        const Vec2 d = middle - center;
        if (!grid.torn[e] && (d.x * d.x) + (d.y * d.y) <= reach) {
            tore.push_back(static_cast<int>(e));
        }
    }
    if (tore.empty()) {
        return tore;
    }
    cloth->torn.insert(cloth->torn.end(), tore.begin(), tore.end());
    std::ranges::sort(cloth->torn);
    const auto [first, last] = std::ranges::unique(cloth->torn);
    cloth->torn.erase(first, last);
    json::Array edges;
    for (const int edge : tore) {
        edges.emplace_back(edge);
    }
    json::Value data;
    data["edges"] = std::move(edges);
    world.emit("torn", world.id_of(entity), std::move(data));
    return tore;
}

void detail::Space::collect_hits() {
    if (B2_IS_NULL(world_)) {
        return;
    }
    const b2ContactEvents events = b2World_GetContactEvents(world_);
    for (int i = 0; i < events.hitCount; ++i) {
        const b2ContactHitEvent& hit = events.hitEvents[i];
        const ShapeInfo* a = info_of(hit.shapeIdA);
        const ShapeInfo* b = info_of(hit.shapeIdB);
        if (a == nullptr || b == nullptr) {
            continue;
        }
        const auto mass_of = [](b2ShapeId shape) {
            const b2BodyId body = b2Shape_GetBody(shape);
            return b2Body_GetType(body) == b2_dynamicBody ? b2Body_GetMass(body) : 0.0F;
        };
        const float ma = mass_of(hit.shapeIdA);
        const float mb = mass_of(hit.shapeIdB);
        float inverse = 0.0F;
        inverse += ma > 0.0F ? 1.0F / ma : 0.0F;
        inverse += mb > 0.0F ? 1.0F / mb : 0.0F;
        if (inverse <= 0.0F) {
            continue;
        }
        const float impulse = hit.approachSpeed / inverse;
        for (const ShapeInfo* info : {a, b}) {
            Impact& impact = impacts_[info->entity];
            if (impulse > impact.impulse) {
                impact = {.impulse = impulse, .point = {hit.point.x, hit.point.y}};
            }
        }
    }
}

Result<std::vector<entt::entity>> break_body_2d(world::World& world, entt::entity entity,
                                                float push, std::optional<Vec2> from) {
    auto& ecs = world.ecs();
    const auto* destructible =
        world.valid(entity) ? ecs.try_get<const Destructible2D>(entity) : nullptr;
    if (destructible == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "has no Destructible2D"};
    }
    const auto* rigid = ecs.try_get<const world::RigidBody2D>(entity);
    const auto* fixed = ecs.try_get<const world::StaticBody2D>(entity);
    if (rigid == nullptr && fixed == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "is no StaticBody2D or RigidBody2D, so it has no body to break"};
    }
    std::shared_ptr<const world::DamageProfile> keep;
    const world::DamageProfile* profile = profile_of(world, *destructible, keep);
    if (!destructible->profile.empty() && profile == nullptr) {
        auto loaded = world::load_damage(world, destructible->profile, destructible->overrides);
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: " +
                                (loaded ? std::string("its profile") : loaded.error().message)};
    }
    if (profile != nullptr && profile->fracture.pattern == "none") {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: its profile's fracture pattern is none"};
    }
    const std::optional<Polygon> outline = outline_of(world, entity);
    if (!outline.has_value()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: it has no outline; give it a CollisionPolygon2D, a "
                                "CollisionShape2D or a Sprite2D with a region"};
    }
    const world::Tree2D tree(world);
    Transform2D place = tree.global(entity);
    place.children.clear();
    if (const auto* own = ecs.try_get<const Transform2D>(entity); own != nullptr) {
        place.top_level = own->top_level;
    }
    const world::Affine2D global = world::affine(place);
    const std::string id(world.id_of(entity));
    const auto* own_piece = ecs.try_get<const FracturePiece2D>(entity);
    const Vec2 hit_local = from.has_value() ? global.inverse().apply(*from) : centroid_of(*outline);
    world::FractureCut cut{.pattern = "voronoi",
                           .cells = destructible->pieces,
                           .seed = destructible->seed,
                           .at = {hit_local.x, hit_local.y, 0.0F},
                           .fine = 0.0F,
                           .index = 0,
                           .dents = {},
                           .carves = {}};
    if (profile != nullptr) {
        cut = world::damage_cut(*profile, {hit_local.x, hit_local.y, 0.0F}, destructible->level,
                                hash_id(id) ^ static_cast<std::uint32_t>(world.tick()));
    }
    const std::vector<Piece2D> pieces = cut_outline(*outline, cut);
    if (pieces.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: no cell holds any of its outline"};
    }
    real_t total = 0.0F;
    for (const Piece2D& piece : pieces) {
        total += area_of(piece.polygon);
    }
    const real_t area_scale = std::abs(place.scale.x * place.scale.y);
    world::RigidBody2D base;
    float mass = destructible->mass;
    Vec2 linear;
    float angular = 0.0F;
    if (rigid != nullptr) {
        base = *rigid;
        mass = rigid->mass;
        linear = rigid->linear_velocity;
        angular = rigid->angular_velocity;
    } else {
        base.collision_layer = fixed->collision_layer;
        base.collision_mask = fixed->collision_mask;
        base.friction = fixed->friction;
        base.bounce = fixed->bounce;
        base.physics_material_override = fixed->physics_material_override;
    }
    if (profile != nullptr && (rigid == nullptr || own_piece != nullptr)) {
        mass = (total * area_scale / (kPixelsPerMeter * kPixelsPerMeter)) * profile->density;
    }
    const bool standing = rigid == nullptr || (own_piece != nullptr && rigid->freeze);
    const bool connected = profile != nullptr && profile->connected && standing;
    base.freeze = false;
    base.freeze_mode = "static";
    base.sleeping = false;
    base.inertia = 0.0F;
    base.lock_rotation = false;
    base.constant_force = {};
    base.constant_torque = 0.0F;
    base.center_of_mass_mode = "auto";
    const Destructible2D settings = *destructible;
    const float lifetime = profile != nullptr ? profile->debris.lifetime : settings.debris_lifetime;
    if (profile != nullptr && push == 0.0F) {
        push = profile->debris.push;
    }
    const std::string group =
        own_piece != nullptr && !own_piece->group.empty() ? own_piece->group : id;
    const std::vector<world::FractureLink> parent_links =
        own_piece != nullptr ? own_piece->links : std::vector<world::FractureLink>{};
    const bool parent_anchored = own_piece != nullptr && own_piece->anchored;
    const auto [low, high] = bounds_of(*outline);
    const Vec2 size = high - low;
    float fine = 0.0F;
    if (profile != nullptr) {
        fine = profile->fracture.fine > 0.0F ? profile->fracture.fine
                                             : 0.25F * std::max(size.x, size.y);
    }
    const auto anchored = [&](const Piece2D& piece) {
        if (profile == nullptr || (own_piece != nullptr && !parent_anchored)) {
            return false;
        }
        if (profile->anchors == "all") {
            return true;
        }
        if (profile->anchors == "none") {
            return false;
        }
        const auto [plo, phi] = bounds_of(piece.polygon);
        const auto eps = static_cast<float>(0.02F * std::max<real_t>({size.x, size.y, 1e-3F}));
        if (profile->anchors == "ends") {
            return plo.x <= low.x + eps || phi.x >= high.x - eps;
        }
        return phi.y >= high.y - eps; // y points down
    };
    const real_t mean = total / static_cast<real_t>(pieces.size());
    std::vector<bool> loose(pieces.size(), !connected);
    if (connected && from.has_value()) {
        // The pieces near the hit come loose, and the one hit at least.
        std::size_t nearest = 0;
        real_t nearest_distance = 1e30F;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const Vec2 d = centroid_of(pieces[i].polygon) - hit_local;
            const real_t distance = std::sqrt((d.x * d.x) + (d.y * d.y));
            loose[i] = distance <= fine;
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest = i;
            }
        }
        if (!pieces.empty()) {
            loose[nearest] = true;
        }
    }
    std::vector<entt::entity> made;
    json::Array ids;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Piece2D& piece = pieces[i];
        auto created = create_piece(world, id, i + 1);
        if (!created) {
            for (const entt::entity done : made) {
                world.destroy(done);
            }
            return created.error();
        }
        const entt::entity out = created.value();
        made.push_back(out);
        ids.emplace_back(std::string(world.id_of(out)));
        const Vec2 middle = centroid_of(piece.polygon);
        ecs.emplace<Transform2D>(out, place);
        if (auto look = look_of(world, entity, piece.polygon); look.has_value()) {
            ecs.emplace<world::Polygon2D>(out, std::move(*look));
        }
        world::CollisionPolygon2D shape;
        shape.polygon = piece.polygon;
        ecs.emplace<world::CollisionPolygon2D>(out, std::move(shape));
        world::RigidBody2D body = base;
        const real_t share = total > 0.0F ? area_of(piece.polygon) / total
                                          : 1.0F / static_cast<real_t>(pieces.size());
        body.mass = std::max(static_cast<float>(mass * share), 0.001F);
        const Vec2 at = global.apply(middle);
        const Vec2 arm = at - global.origin;
        // A point turning clockwise (y down) at angular radians a second moves along (-y, x).
        body.linear_velocity = linear + Vec2{-arm.y * angular, arm.x * angular};
        body.angular_velocity = angular;
        if (push != 0.0F && loose[i]) {
            const Vec2 away = at - from.value_or(global.apply(centroid_of(*outline)));
            const real_t length = std::sqrt((away.x * away.x) + (away.y * away.y));
            if (length > 0.0F) {
                body.linear_velocity = body.linear_velocity + (away * (push / length));
            }
        }
        body.freeze = !loose[i];
        if (body.freeze) {
            body.linear_velocity = {};
            body.angular_velocity = 0.0F;
        }
        ecs.emplace<world::RigidBody2D>(out, body);
        FracturePiece2D made_piece{.lifetime = loose[i] ? lifetime : 0.0F,
                                   .group = connected ? group : std::string{},
                                   .links = {},
                                   .anchored = !loose[i] && connected && anchored(piece)};
        ecs.emplace<FracturePiece2D>(out, std::move(made_piece));
        if (profile != nullptr) {
            Destructible2D again = settings;
            again.level = settings.level + 1;
            again.state = 0;
            real_t health = profile->health * (mean > 0.0F ? area_of(piece.polygon) / mean : 1.0F);
            for (int l = 0; l <= settings.level; ++l) {
                health *= 0.5F;
            }
            again.health = std::max<real_t>(health, 0.001F);
            ecs.emplace<Destructible2D>(out, std::move(again));
        }
    }
    if (connected) {
        const real_t length_scale = std::sqrt(area_scale);
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (loose[i]) {
                continue;
            }
            auto& links = ecs.get<FracturePiece2D>(made[i]).links;
            for (const auto& [other, length] : pieces[i].neighbors) {
                if (!loose[other]) {
                    links.push_back({.piece = std::string(world.id_of(made[other])),
                                     .area = static_cast<float>(length * length_scale)});
                }
            }
        }
        // What the broken piece was linked to holds the new pieces that touch it.
        const auto eps = static_cast<float>(0.01F * std::max<real_t>({size.x, size.y, 1.0F}));
        for (const world::FractureLink& link : parent_links) {
            const entt::entity other = world.find(link.piece);
            const auto* outline_other =
                other != entt::null ? ecs.try_get<const world::CollisionPolygon2D>(other) : nullptr;
            if (outline_other == nullptr) {
                continue;
            }
            const auto [olo, ohi] = bounds_of(outline_other->polygon);
            std::vector<std::size_t> touch;
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                const auto [plo, phi] = bounds_of(pieces[i].polygon);
                if (!loose[i] && plo.x <= ohi.x + eps && olo.x <= phi.x + eps &&
                    plo.y <= ohi.y + eps && olo.y <= phi.y + eps) {
                    touch.push_back(i);
                }
            }
            auto& theirs = ecs.get<FracturePiece2D>(other).links;
            std::erase_if(theirs, [&](const world::FractureLink& l) { return l.piece == id; });
            for (const std::size_t i : touch) {
                const float length = link.area / static_cast<float>(touch.size());
                ecs.get<FracturePiece2D>(made[i]).links.push_back(
                    {.piece = link.piece, .area = length});
                theirs.push_back({.piece = std::string(world.id_of(made[i])), .area = length});
            }
        }
        state_of(world).unsettled.insert(group);
    } else if (own_piece != nullptr && !own_piece->links.empty()) {
        for (const world::FractureLink& link : own_piece->links) {
            if (const entt::entity other = world.find(link.piece); other != entt::null) {
                if (auto* there = ecs.try_get<FracturePiece2D>(other); there != nullptr) {
                    std::erase_if(there->links,
                                  [&](const world::FractureLink& l) { return l.piece == id; });
                }
            }
        }
        if (!own_piece->group.empty()) {
            state_of(world).unsettled.insert(own_piece->group);
        }
    }
    world.destroy(entity);
    json::Value data;
    data["pieces"] = std::move(ids);
    world.emit("broken", id, std::move(data));
    return made;
}

Result<DamageReport2D> damage_body_2d(world::World& world, entt::entity entity, float amount,
                                      std::string_view kind, std::optional<Vec2> at, float push,
                                      std::optional<Vec2> direction) {
    auto& ecs = world.ecs();
    auto* destructible = world.valid(entity) ? ecs.try_get<Destructible2D>(entity) : nullptr;
    if (destructible == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "has no Destructible2D"};
    }
    DamageReport2D report;
    report.id = std::string(world.id_of(entity));
    std::shared_ptr<const world::DamageProfile> keep;
    const world::DamageProfile* profile = profile_of(world, *destructible, keep);
    if (profile == nullptr) {
        if (!destructible->profile.empty()) {
            auto loaded = world::load_damage(world, destructible->profile, destructible->overrides);
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = loaded ? "its profile can't be read" : loaded.error().message};
        }
        report.dealt = std::max(amount, 0.0F);
        if (report.dealt < destructible->damage_threshold || report.dealt <= 0.0F) {
            return report;
        }
        auto broken = break_body_2d(world, entity, push, at);
        if (!broken) {
            return broken.error();
        }
        report.broke = true;
        for (const entt::entity piece : broken.value()) {
            report.pieces.emplace_back(world.id_of(piece));
        }
        return report;
    }
    const float full = destructible->health < 0.0F ? profile->health : destructible->health;
    report.dealt = world::damage_taken(*profile, amount, kind);
    report.health = std::max(full - report.dealt, 0.0F);
    destructible->health = report.health;
    const int before = destructible->state;
    const bool piece = ecs.all_of<FracturePiece2D>(entity);
    const int state = piece ? before : world::damage_state(*profile, report.health);
    report.state = state;
    if (state != before) {
        destructible->state = state;
        show_state(world, entity, *profile, before, state);
    }
    if (report.health > 0.0F && report.dealt > 0.0F) {
        report.deformed =
            deform_body_2d(world, entity, *profile, report.dealt, kind, at, direction);
        destructible = ecs.try_get<Destructible2D>(entity);
    }
    if (report.dealt > 0.0F) {
        json::Value data;
        data["dealt"] = report.dealt;
        data["health"] = report.health;
        data["kind"] = std::string(kind);
        if (state != before) {
            data["state"] = state;
        }
        if (!report.deformed.empty()) {
            data["deformed"] = report.deformed;
        }
        world.emit("damaged", report.id, std::move(data));
    }
    if (report.health > 0.0F || report.dealt <= 0.0F) {
        return report;
    }
    bool breaks =
        profile->fracture.pattern != "none" && destructible->level < profile->fracture.levels;
    if (breaks && piece && profile->fracture.min_size > 0.0F) {
        if (const auto outline = outline_of(world, entity); outline.has_value()) {
            const auto [lo, hi] = bounds_of(*outline);
            breaks = std::max(hi.x - lo.x, hi.y - lo.y) >= profile->fracture.min_size;
        }
    }
    if (breaks) {
        auto broken = break_body_2d(world, entity, push, at);
        if (!broken) {
            return broken.error();
        }
        report.broke = true;
        for (const entt::entity made : broken.value()) {
            report.pieces.emplace_back(world.id_of(made));
        }
    } else if (piece) {
        const bool was_standing = ecs.all_of<world::RigidBody2D>(entity) &&
                                  ecs.get<const world::RigidBody2D>(entity).freeze;
        set_loose(world, entity, profile->debris.lifetime);
        report.fell = was_standing;
    }
    return report;
}

std::vector<DamageReport2D> damage_area_2d(world::World& world, Vec2 center, float radius,
                                           float amount, std::string_view kind, float push) {
    std::vector<DamageReport2D> out;
    const world::Tree2D tree(world);
    std::vector<std::pair<entt::entity, float>> hit;
    for (const entt::entity entity : world.ecs().view<const Destructible2D>()) {
        const Transform2D place = tree.global(entity);
        const Vec2 d = place.position - center;
        float distance = std::sqrt((d.x * d.x) + (d.y * d.y));
        if (world.ecs().all_of<world::TileMapLayer>(entity)) {
            // A blast reaches a layer where it would take cells out, all of it at once.
            distance = radius + 1.0F;
            std::shared_ptr<const world::DamageProfile> keep;
            const world::DamageProfile* profile =
                profile_of(world, world.ecs().get<const Destructible2D>(entity), keep);
            const auto* carver = world.ecs().ctx().find<TileCarver>();
            if (profile != nullptr && carver != nullptr && *carver && kind == "explosive") {
                const float dealt = world::damage_taken(*profile, amount, kind);
                const float reach =
                    std::min(dealt * profile->deform.carve, profile->deform.max_carve) *
                    kPixelsPerMeter;
                if ((*carver)(world, entity, center, reach, false) > 0) {
                    distance = 0.0F;
                }
            }
        } else if (const auto outline = outline_of(world, entity); outline.has_value()) {
            const world::Affine2D global = world::affine(place);
            const real_t scale =
                std::max<real_t>(std::sqrt(std::abs(place.scale.x * place.scale.y)), 1e-6F);
            distance =
                static_cast<float>(outside_by(*outline, global.inverse().apply(center)) * scale);
        }
        if (distance <= radius) {
            hit.emplace_back(entity, radius > 0.0F ? 1.0F - (distance / radius) : 1.0F);
        }
    }
    std::ranges::sort(hit, {}, [](const auto& h) { return entt::to_entity(h.first); });
    for (const auto& [entity, falloff] : hit) {
        if (!world.valid(entity)) {
            continue;
        }
        if (auto report =
                damage_body_2d(world, entity, amount * falloff, kind, center, push * falloff);
            report) {
            out.push_back(std::move(report.value()));
        }
    }
    return out;
}

std::vector<std::string> settle_group_2d(world::World& world, std::string_view group) {
    auto& ecs = world.ecs();
    std::vector<entt::entity> members;
    for (const auto [entity, piece] : ecs.view<FracturePiece2D>().each()) {
        if (piece.group == group) {
            members.push_back(entity);
        }
    }
    std::ranges::sort(members, {}, [](entt::entity e) { return entt::to_entity(e); });
    std::unordered_map<std::string, std::size_t> index;
    std::vector<entt::entity> standing;
    for (const entt::entity entity : members) {
        const auto* rigid = ecs.try_get<const world::RigidBody2D>(entity);
        if (rigid != nullptr && rigid->freeze) {
            index.emplace(std::string(world.id_of(entity)), standing.size());
            standing.push_back(entity);
        }
    }
    const world::Tree2D tree(world);
    float gravity = 980.0F;
    if (const detail::Space* space = detail::find_space(world); space != nullptr) {
        gravity = space->settings().gravity;
    }
    std::vector<world::StressPiece> stress(standing.size());
    std::shared_ptr<const world::DamageProfile> keep;
    const world::DamageProfile* profile = nullptr;
    for (std::size_t i = 0; i < standing.size(); ++i) {
        const entt::entity entity = standing[i];
        auto& piece = ecs.get<FracturePiece2D>(entity);
        std::erase_if(piece.links,
                      [&](const world::FractureLink& l) { return !index.contains(l.piece); });
        if (profile == nullptr) {
            if (const auto* d = ecs.try_get<const Destructible2D>(entity); d != nullptr) {
                profile = profile_of(world, *d, keep);
            }
        }
        const Transform2D place = tree.global(entity);
        Vec2 middle = place.position;
        if (const auto* polygon = ecs.try_get<const world::CollisionPolygon2D>(entity);
            polygon != nullptr && !polygon->polygon.empty()) {
            middle = world::affine(place).apply(centroid_of(polygon->polygon));
        }
        stress[i].center = {middle.x, middle.y, 0.0F};
        // Newtons, from kilograms and the gravity in pixels a second squared.
        stress[i].weight =
            ecs.get<const world::RigidBody2D>(entity).mass * gravity / kPixelsPerMeter;
        stress[i].anchored = piece.anchored;
        for (const world::FractureLink& link : piece.links) {
            // A pixel of edge holds a 64th of what a square meter does.
            stress[i].links.emplace_back(index.at(link.piece), link.area / kPixelsPerMeter);
        }
    }
    std::vector<std::string> fell;
    if (profile == nullptr) {
        return fell;
    }
    const world::StressResult result =
        world::settle_stress(stress, profile->strength, profile->compression, {0.0F, -1.0F, 0.0F});
    for (const auto& [a, b] : result.broken) {
        const std::string a_id(world.id_of(standing[a]));
        const std::string b_id(world.id_of(standing[b]));
        std::erase_if(ecs.get<FracturePiece2D>(standing[a]).links,
                      [&](const world::FractureLink& l) { return l.piece == b_id; });
        std::erase_if(ecs.get<FracturePiece2D>(standing[b]).links,
                      [&](const world::FractureLink& l) { return l.piece == a_id; });
    }
    for (std::size_t i = 0; i < standing.size(); ++i) {
        if (result.falls[i]) {
            fell.emplace_back(world.id_of(standing[i]));
            set_loose(world, standing[i], profile->debris.lifetime);
        }
    }
    if (!fell.empty()) {
        json::Value data;
        json::Array ids;
        for (const std::string& f : fell) {
            ids.emplace_back(f);
        }
        data["pieces"] = std::move(ids);
        data["links_broken"] = static_cast<std::int64_t>(result.broken.size());
        world.emit("collapsed", group, std::move(data));
    }
    return fell;
}

namespace detail {

void break_and_age_2d(world::World& world, Space& space, float seconds) {
    auto& ecs = world.ecs();
    DestructionState2D& state = state_of(world);
    if (!state.started) {
        state.started = true;
        for (const auto [entity, piece] : ecs.view<const FracturePiece2D>().each()) {
            if (!piece.group.empty() && !piece.links.empty()) {
                state.unsettled.insert(piece.group);
            }
        }
    }
    std::unordered_map<entt::entity, Space::Impact> impacts = space.take_impacts();
    if (!impacts.empty()) {
        std::vector<std::pair<entt::entity, Space::Impact>> hit;
        for (const entt::entity entity : ecs.view<const Destructible2D>()) {
            if (const auto found = impacts.find(entity); found != impacts.end()) {
                hit.emplace_back(entity, found->second);
            }
        }
        for (const auto& [entity, impact] : hit) {
            if (!world.valid(entity)) {
                continue;
            }
            const auto& destructible = ecs.get<const Destructible2D>(entity);
            float threshold = destructible.damage_threshold;
            float per = 1.0F;
            std::shared_ptr<const world::DamageProfile> keep;
            const world::DamageProfile* profile = profile_of(world, destructible, keep);
            if (profile != nullptr) {
                threshold = profile->impact_threshold;
                per = profile->impact_damage;
            } else if (!destructible.profile.empty() || threshold <= 0.0F) {
                continue;
            }
            if (impact.impulse < threshold) {
                continue;
            }
            const float amount =
                profile != nullptr ? (impact.impulse - threshold) * per : impact.impulse;
            if (auto damaged = damage_body_2d(world, entity, amount, "impact", impact.point, 0.0F);
                !damaged) {
                log::warn("a Destructible2D can't break", {{"id", std::string(world.id_of(entity))},
                                                           {"error", damaged.error().message}});
            }
        }
    }
    for (const auto [entity, piece] : ecs.view<FracturePiece2D>().each()) {
        for (const world::FractureLink& link : piece.links) {
            const entt::entity other = world.find(link.piece);
            const auto* rigid =
                other != entt::null ? ecs.try_get<const world::RigidBody2D>(other) : nullptr;
            if (rigid == nullptr || !rigid->freeze) {
                state.unsettled.insert(piece.group);
                break;
            }
        }
    }
    while (!state.unsettled.empty()) {
        const std::string group = *state.unsettled.begin();
        state.unsettled.erase(state.unsettled.begin());
        (void)settle_group_2d(world, group);
    }
    std::vector<entt::entity> gone;
    for (const auto [entity, piece] : ecs.view<FracturePiece2D>().each()) {
        if (piece.lifetime <= 0.0F) {
            continue;
        }
        piece.lifetime -= seconds;
        if (piece.lifetime <= 0.0F) {
            gone.push_back(entity);
        }
    }
    for (const entt::entity entity : gone) {
        world.destroy(entity);
    }
}

} // namespace detail

} // namespace slash::physics2d
