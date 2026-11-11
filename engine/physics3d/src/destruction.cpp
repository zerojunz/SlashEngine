// Destruction (Godot has none; Unreal's geometry collections and Chaos's connection graph are
// the model): a Destructible3D is replaced by a rigid body per piece of its mesh's fracture
// (scene3d::fracture_of), each moving as that part of the body moved, when its health runs out
// (impacts, damage_body, damage_area) or break_body says so. With a profile
// (slash/world/damage.hpp) it is cut by its pattern where it was hit, its pieces break again
// while under its levels, and a connected one's pieces stand frozen and linked, settling by
// world::settle_stress whenever a link or piece goes, until what no link holds to an anchored
// piece falls. Pieces with a lifetime go when it runs out.

#include "space.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/fracture.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace slash::physics3d {

namespace {

using world::Destructible3D;
using world::FracturePiece3D;

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// The body's place in the world as a Transform3D: its own at the root, else its global
// transform taken apart.
world::Transform3D placed(const world::World& world, const world::Tree3D& tree,
                          entt::entity entity) {
    world::Transform3D out;
    const auto* own = world.ecs().try_get<const world::Transform3D>(entity);
    if (own != nullptr && tree.transform_parent(entity) == entt::null) {
        out = *own;
        out.children.clear();
        return out;
    }
    const world::Affine3D global = tree.global(entity);
    out.position = global.origin;
    out.scale = {length(global.x), length(global.y), length(global.z)};
    world::Affine3D turn = global;
    turn.origin = {};
    const auto unit = [](Vec3 v, float size) {
        return size > 0.0F ? Vec3{v.x / size, v.y / size, v.z / size} : v;
    };
    turn.x = unit(turn.x, out.scale.x);
    turn.y = unit(turn.y, out.scale.y);
    turn.z = unit(turn.z, out.scale.z);
    out.rotation = world::euler(turn);
    if (own != nullptr) {
        out.visible = own->visible;
    }
    return out;
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

// Gives a piece the body's MeshInstance3D, primitive mesh and material.
void copy_looks(entt::registry& ecs, entt::entity out, const world::MeshInstance3D& instance,
                const std::optional<scene3d::Primitive>& shape,
                const std::optional<world::StandardMaterial3D>& material) {
    ecs.emplace<world::MeshInstance3D>(out, instance);
    if (shape.has_value()) {
        std::visit(
            [&ecs, out](const auto& mesh) { ecs.emplace<std::decay_t<decltype(mesh)>>(out, mesh); },
            *shape);
    }
    if (material.has_value()) {
        ecs.emplace<world::StandardMaterial3D>(out, *material);
    }
}

std::uint32_t hash_id(std::string_view id) {
    std::uint32_t h = 2166136261U;
    for (const char c : id) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return h;
}

Vec3 scaled(Vec3 v, Vec3 by) {
    return {v.x * by.x, v.y * by.y, v.z * by.z};
}

float volume_scale(Vec3 scale) {
    return std::abs(scale.x * scale.y * scale.z);
}

// What the groups of standing pieces need: settling again after a link or piece went.
struct DestructionState {
    std::set<std::string> unsettled;
    bool started = false;
};

DestructionState& state_of(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<DestructionState>() == nullptr) {
        ctx.emplace<DestructionState>();
        world.add_reset([](world::World& w) {
            if (auto* state = w.ecs().ctx().find<DestructionState>(); state != nullptr) {
                *state = {};
            }
        });
    }
    return ctx.get<DestructionState>();
}

// The profile a Destructible3D names, if any.
const world::DamageProfile* profile_of(const world::World& world,
                                       const Destructible3D& destructible,
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

// Whether a piece is held in place by its profile's anchors, by its bounds in the mesh's space
// against the whole mesh's.
bool anchored_by(std::string_view anchors, const scene3d::FracturePiece& piece, Vec3 low,
                 Vec3 high) {
    if (anchors == "all") {
        return true;
    }
    if (anchors == "none") {
        return false;
    }
    const Vec3 size = high - low;
    const auto eps = static_cast<float>(0.02F * std::max<real_t>({size.x, size.y, size.z, 1e-3F}));
    if (anchors == "ends") {
        const bool along_x = size.x >= size.z;
        const float lo = along_x ? low.x : low.z;
        const float hi = along_x ? high.x : high.z;
        const float plo = along_x ? piece.low.x : piece.low.z;
        const float phi = along_x ? piece.high.x : piece.high.z;
        return plo <= lo + eps || phi >= hi - eps;
    }
    return piece.low.y <= low.y + eps;
}

// The bounds of the whole mesh, from its first cut's pieces.
std::pair<Vec3, Vec3> whole_bounds(const world::World& world, entt::entity entity,
                                   const world::FractureCut& first) {
    Vec3 low{1e30F, 1e30F, 1e30F};
    Vec3 high{-1e30F, -1e30F, -1e30F};
    world::FractureCut cut = first;
    cut.index = 0;
    auto fracture =
        scene3d::fracture_of(world, entity, std::span<const world::FractureCut>(&cut, 1));
    if (fracture) {
        for (const scene3d::FracturePiece& piece : fracture.value()->pieces) {
            low = {std::min(low.x, piece.low.x), std::min(low.y, piece.low.y),
                   std::min(low.z, piece.low.z)};
            high = {std::max(high.x, piece.high.x), std::max(high.y, piece.high.y),
                    std::max(high.z, piece.high.z)};
        }
    }
    return {low, high};
}

// A piece's bounds in the mesh's space.
std::optional<std::pair<Vec3, Vec3>> piece_bounds(const world::World& world, entt::entity entity) {
    const auto* piece = world.ecs().try_get<const FracturePiece3D>(entity);
    if (piece == nullptr) {
        return std::nullopt;
    }
    const std::vector<world::FractureCut> cuts = scene3d::piece_cuts(*piece);
    auto fracture = scene3d::fracture_of(world, entity, cuts);
    const auto index = static_cast<std::size_t>(std::max(cuts.back().index, 0));
    if (!fracture || index >= fracture.value()->pieces.size()) {
        return std::nullopt;
    }
    const scene3d::FracturePiece& made = fracture.value()->pieces[index];
    return std::pair{made.low, made.high};
}

bool touching(const std::pair<Vec3, Vec3>& a, const std::pair<Vec3, Vec3>& b, float eps) {
    return a.first.x <= b.second.x + eps && b.first.x <= a.second.x + eps &&
           a.first.y <= b.second.y + eps && b.first.y <= a.second.y + eps &&
           a.first.z <= b.second.z + eps && b.first.z <= a.second.z + eps;
}

// Sets a standing piece loose: it falls, links to it go and its group settles again.
void set_loose(world::World& world, entt::entity entity, float lifetime) {
    auto& ecs = world.ecs();
    auto* piece = ecs.try_get<FracturePiece3D>(entity);
    if (piece == nullptr) {
        return;
    }
    if (auto* rigid = ecs.try_get<world::RigidBody3D>(entity); rigid != nullptr && rigid->freeze) {
        rigid->freeze = false;
        rigid->sleeping = false;
        if (piece->lifetime <= 0.0F) {
            piece->lifetime = lifetime;
        }
    }
    for (const world::FractureLink& link : piece->links) {
        const entt::entity other = world.find(link.piece);
        if (other == entt::null) {
            continue;
        }
        if (auto* there = ecs.try_get<FracturePiece3D>(other); there != nullptr) {
            const std::string id(world.id_of(entity));
            std::erase_if(there->links,
                          [&](const world::FractureLink& l) { return l.piece == id; });
        }
    }
    piece->links.clear();
    piece->anchored = false;
    if (!piece->group.empty()) {
        state_of(world).unsettled.insert(piece->group);
    }
}

// Applies a damage state's looks: its tint over the material (taking the last state's off),
// its material and its mesh.
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
    if (auto* material = ecs.try_get<world::StandardMaterial3D>(entity); material != nullptr) {
        Vec4 color = material->albedo_color;
        if (before != nullptr && before->tinted) {
            const auto undo = [](float c, float t) { return t > 0.0F ? c / t : c; };
            color = {undo(color.x, before->tint.x), undo(color.y, before->tint.y),
                     undo(color.z, before->tint.z), undo(color.w, before->tint.w)};
        }
        if (after != nullptr && after->tinted) {
            color = {color.x * after->tint.x, color.y * after->tint.y, color.z * after->tint.z,
                     color.w * after->tint.w};
        }
        material->albedo_color = color;
    }
    if (auto* instance = ecs.try_get<world::MeshInstance3D>(entity);
        instance != nullptr && after != nullptr) {
        if (!after->material.empty()) {
            instance->material_override = after->material;
        }
        if (!after->mesh.empty() && !ecs.all_of<FracturePiece3D>(entity)) {
            instance->mesh = after->mesh;
        }
    }
}

} // namespace

Result<std::vector<entt::entity>> break_body(world::World& world, entt::entity entity, float push,
                                             std::optional<Vec3> from) {
    auto& ecs = world.ecs();
    const auto* destructible =
        world.valid(entity) ? ecs.try_get<const Destructible3D>(entity) : nullptr;
    if (destructible == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "has no Destructible3D"};
    }
    const auto* rigid = ecs.try_get<const world::RigidBody3D>(entity);
    const auto* fixed = ecs.try_get<const world::StaticBody3D>(entity);
    if (rigid == nullptr && fixed == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "is no StaticBody3D or RigidBody3D, so it has no body to break"};
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
    const world::Tree3D tree(world);
    const world::Transform3D place = placed(world, tree, entity);
    const world::Affine3D global = world::affine(place);
    const std::string id(world.id_of(entity));
    const auto* own_piece = ecs.try_get<const FracturePiece3D>(entity);

    // The cuts: the piece's own, then the new one where it was hit.
    std::vector<world::FractureCut> cuts;
    if (own_piece != nullptr) {
        cuts = scene3d::piece_cuts(*own_piece);
    }
    const Vec3 hit_local = from.has_value() ? global.inverse().apply(*from) : Vec3{};
    if (profile != nullptr) {
        world::FractureCut cut =
            world::damage_cut(*profile, hit_local, destructible->level,
                              hash_id(id) ^ static_cast<std::uint32_t>(world.tick()));
        if (!from.has_value() && own_piece == nullptr) {
            cut.at = {};
        }
        cuts.push_back(cut);
    } else {
        cuts.push_back({.pattern = "voronoi",
                        .cells = destructible->pieces,
                        .seed = destructible->seed,
                        .at = {},
                        .fine = 0.0F,
                        .index = 0});
    }
    auto fracture = scene3d::fracture_of(world, entity, cuts);
    if (!fracture) {
        return Error{.code = fracture.error().code,
                     .message = "can't break: " + fracture.error().message};
    }
    const std::vector<scene3d::FracturePiece>& pieces = fracture.value()->pieces;
    if (pieces.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: no cell holds any of its mesh"};
    }

    double volume = 0.0;
    Vec3 middle;
    for (const scene3d::FracturePiece& piece : pieces) {
        volume += piece.volume;
        const auto share = static_cast<float>(piece.volume);
        middle =
            middle + Vec3{piece.center.x * share, piece.center.y * share, piece.center.z * share};
    }
    if (volume > 0.0) {
        const auto whole = static_cast<float>(volume);
        middle = {middle.x / whole, middle.y / whole, middle.z / whole};
    }
    const Vec3 center = global.apply(middle);
    const Vec3 away_from = from.value_or(center);

    // What the pieces take from the body.
    world::RigidBody3D base;
    float mass = destructible->mass;
    Vec3 linear;
    Vec3 angular;
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
        // A profile's density weighs what has no mass of its own, and every piece of a piece.
        mass = static_cast<float>(volume) * volume_scale(place.scale) * profile->density;
    }
    const bool standing = rigid == nullptr || (own_piece != nullptr && rigid->freeze);
    const bool connected = profile != nullptr && profile->connected && standing;
    base.freeze = false;
    base.freeze_mode = "static";
    base.sleeping = false;
    base.inertia = {};
    base.lock_rotation = false;
    base.constant_force = {};
    base.constant_torque = {};
    const world::MeshInstance3D instance = ecs.get<const world::MeshInstance3D>(entity);
    std::optional<world::StandardMaterial3D> material;
    if (const auto* standard = ecs.try_get<const world::StandardMaterial3D>(entity);
        standard != nullptr) {
        material = *standard;
    }
    auto primitive = scene3d::primitive_of(world, entity);
    const auto shape = primitive ? primitive.value() : std::nullopt;
    const Destructible3D settings = *destructible;
    const float lifetime = profile != nullptr ? profile->debris.lifetime : settings.debris_lifetime;
    if (profile != nullptr && push == 0.0F) {
        push = profile->debris.push;
    }
    const std::string group =
        own_piece != nullptr && !own_piece->group.empty() ? own_piece->group : id;
    const std::vector<world::FractureLink> parent_links =
        own_piece != nullptr ? own_piece->links : std::vector<world::FractureLink>{};
    const bool parent_anchored = own_piece != nullptr && own_piece->anchored;
    // Which pieces are held, which are loose near the hit, and their health.
    std::pair<Vec3, Vec3> whole{};
    float fine = 0.0F;
    if (profile != nullptr) {
        whole = own_piece != nullptr ? whole_bounds(world, entity, cuts.front())
                                     : std::pair<Vec3, Vec3>{};
        if (own_piece == nullptr) {
            Vec3 low{1e30F, 1e30F, 1e30F};
            Vec3 high{-1e30F, -1e30F, -1e30F};
            for (const scene3d::FracturePiece& piece : pieces) {
                low = {std::min(low.x, piece.low.x), std::min(low.y, piece.low.y),
                       std::min(low.z, piece.low.z)};
                high = {std::max(high.x, piece.high.x), std::max(high.y, piece.high.y),
                        std::max(high.z, piece.high.z)};
            }
            whole = {low, high};
        }
        const Vec3 size = whole.second - whole.first;
        fine = profile->fracture.fine > 0.0F ? profile->fracture.fine
                                             : 0.25F * std::max({size.x, size.y, size.z});
        for (int l = 0; l < settings.level; ++l) {
            fine *= 0.5F;
        }
    }
    const auto mean_volume = static_cast<float>(volume / static_cast<double>(pieces.size()));
    const float area_scale = std::cbrt(volume_scale(place.scale) * volume_scale(place.scale));

    std::vector<entt::entity> made;
    made.reserve(pieces.size());
    json::Array ids;
    std::vector<bool> loose(pieces.size(), !connected);
    if (connected && from.has_value()) {
        // The pieces near the hit come loose, and the one hit at least.
        std::size_t nearest = 0;
        float nearest_distance = 1e30F;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const Vec3 d = pieces[i].center - hit_local;
            const float distance = std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
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
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const scene3d::FracturePiece& piece = pieces[i];
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
        ecs.emplace<world::Transform3D>(out, place);
        copy_looks(ecs, out, instance, shape, material);
        const Vec3 at = global.apply(piece.center);
        const Vec3 arm = at - center;
        world::RigidBody3D body = base;
        body.mass = volume > 0.0 ? static_cast<float>(mass * (piece.volume / volume))
                                 : mass / static_cast<float>(pieces.size());
        body.mass = std::max(body.mass, 0.001F);
        body.linear_velocity = linear + cross(angular, arm);
        if (push != 0.0F && loose[i]) {
            const Vec3 out_dir = at - away_from;
            const float size = length(out_dir);
            if (size > 0.0F) {
                const float scale = push / size;
                body.linear_velocity =
                    body.linear_velocity +
                    Vec3{out_dir.x * scale, out_dir.y * scale, out_dir.z * scale};
            }
        }
        body.angular_velocity = angular;
        body.freeze = !loose[i];
        if (body.freeze) {
            body.linear_velocity = {};
            body.angular_velocity = {};
        }
        ecs.emplace<world::RigidBody3D>(out, body);
        world::CollisionShape3D hull;
        hull.shape = "convex_polygon";
        hull.points = piece.hull;
        ecs.emplace<world::CollisionShape3D>(out, std::move(hull));
        std::vector<world::FractureCut> own_cuts = cuts;
        own_cuts.back().index = static_cast<int>(i);
        FracturePiece3D made_piece{.pieces = settings.pieces,
                                   .seed = settings.seed,
                                   .index = static_cast<int>(i),
                                   .lifetime = loose[i] ? lifetime : 0.0F,
                                   .cuts = {},
                                   .group = {},
                                   .links = {},
                                   .anchored = false};
        if (profile != nullptr) {
            made_piece.cuts = std::move(own_cuts);
            if (connected) {
                made_piece.group = group;
                if (!loose[i]) {
                    made_piece.anchored =
                        own_piece == nullptr
                            ? anchored_by(profile->anchors, piece, whole.first, whole.second)
                            : parent_anchored &&
                                  anchored_by(profile->anchors, piece, whole.first, whole.second);
                }
            }
        }
        ecs.emplace<FracturePiece3D>(out, std::move(made_piece));
        if (profile != nullptr) {
            // Pieces take damage too, and break again while under the profile's levels.
            Destructible3D again = settings;
            again.level = settings.level + 1;
            again.state = 0;
            float health =
                profile->health *
                (mean_volume > 0.0F ? static_cast<float>(piece.volume) / mean_volume : 1.0F);
            for (int l = 0; l <= settings.level; ++l) {
                health *= 0.5F;
            }
            again.health = std::max(health, 0.001F);
            ecs.emplace<Destructible3D>(out, std::move(again));
        }
    }
    // Links between the standing pieces, by the faces they share, and to what the piece that
    // broke was linked to, where they touch it.
    if (connected) {
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (loose[i]) {
                continue;
            }
            auto& links = ecs.get<FracturePiece3D>(made[i]).links;
            for (const auto& [other, area] : pieces[i].neighbors) {
                const auto o = static_cast<std::size_t>(other);
                if (o < made.size() && !loose[o]) {
                    links.push_back(
                        {.piece = std::string(world.id_of(made[o])), .area = area * area_scale});
                }
            }
        }
        const Vec3 size = whole.second - whole.first;
        const auto eps =
            static_cast<float>(0.002F * std::max<real_t>({size.x, size.y, size.z, 1e-3F}));
        for (const world::FractureLink& link : parent_links) {
            const entt::entity other = world.find(link.piece);
            const auto bounds = other != entt::null ? piece_bounds(world, other) : std::nullopt;
            if (!bounds.has_value()) {
                continue;
            }
            std::vector<std::size_t> touch;
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                if (!loose[i] && touching({pieces[i].low, pieces[i].high}, *bounds, eps)) {
                    touch.push_back(i);
                }
            }
            auto& theirs = ecs.get<FracturePiece3D>(other).links;
            std::erase_if(theirs, [&](const world::FractureLink& l) { return l.piece == id; });
            for (const std::size_t i : touch) {
                const float area = link.area / static_cast<float>(touch.size());
                const std::string child(world.id_of(made[i]));
                ecs.get<FracturePiece3D>(made[i]).links.push_back(
                    {.piece = link.piece, .area = area});
                theirs.push_back({.piece = child, .area = area});
            }
        }
        state_of(world).unsettled.insert(group);
    } else if (own_piece != nullptr && !own_piece->links.empty()) {
        // A loose piece that broke takes nothing with it; what it was linked to lets go of it.
        for (const world::FractureLink& link : own_piece->links) {
            if (const entt::entity other = world.find(link.piece); other != entt::null) {
                if (auto* there = ecs.try_get<FracturePiece3D>(other); there != nullptr) {
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

Result<DamageReport> damage_body(world::World& world, entt::entity entity, float amount,
                                 std::string_view kind, std::optional<Vec3> at, float push) {
    auto& ecs = world.ecs();
    auto* destructible = world.valid(entity) ? ecs.try_get<Destructible3D>(entity) : nullptr;
    if (destructible == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "has no Destructible3D"};
    }
    DamageReport report;
    report.id = std::string(world.id_of(entity));
    std::shared_ptr<const world::DamageProfile> keep;
    const world::DamageProfile* profile = profile_of(world, *destructible, keep);
    if (profile == nullptr) {
        if (!destructible->profile.empty()) {
            auto loaded = world::load_damage(world, destructible->profile, destructible->overrides);
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = loaded ? "its profile can't be read" : loaded.error().message};
        }
        // With no profile, damage at its threshold breaks it.
        report.dealt = std::max(amount, 0.0F);
        if (report.dealt < destructible->damage_threshold || report.dealt <= 0.0F) {
            return report;
        }
        auto broken = break_body(world, entity, push, at);
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
    const bool piece = ecs.all_of<FracturePiece3D>(entity);
    const int state = piece ? before : world::damage_state(*profile, report.health);
    report.state = state;
    if (state != before) {
        destructible->state = state;
        show_state(world, entity, *profile, before, state);
    }
    if (report.dealt > 0.0F) {
        json::Value data;
        data["dealt"] = report.dealt;
        data["health"] = report.health;
        data["kind"] = std::string(kind);
        if (state != before) {
            data["state"] = state;
        }
        world.emit("damaged", report.id, std::move(data));
    }
    if (report.health > 0.0F || report.dealt <= 0.0F) {
        return report;
    }
    // Out of health: it breaks, or, a piece that breaks no more, it comes loose.
    bool breaks =
        profile->fracture.pattern != "none" && destructible->level < profile->fracture.levels;
    if (breaks && piece && profile->fracture.min_size > 0.0F) {
        if (const auto bounds = piece_bounds(world, entity); bounds.has_value()) {
            const Vec3 size = scaled(bounds->second - bounds->first,
                                     ecs.all_of<world::Transform3D>(entity)
                                         ? ecs.get<const world::Transform3D>(entity).scale
                                         : Vec3{1.0F, 1.0F, 1.0F});
            breaks = std::max({std::abs(size.x), std::abs(size.y), std::abs(size.z)}) >=
                     profile->fracture.min_size;
        }
    }
    if (breaks) {
        auto broken = break_body(world, entity, push, at);
        if (!broken) {
            return broken.error();
        }
        report.broke = true;
        for (const entt::entity made : broken.value()) {
            report.pieces.emplace_back(world.id_of(made));
        }
    } else if (piece) {
        const bool was_standing = ecs.all_of<world::RigidBody3D>(entity) &&
                                  ecs.get<const world::RigidBody3D>(entity).freeze;
        set_loose(world, entity, profile->debris.lifetime);
        report.fell = was_standing;
    }
    return report;
}

std::vector<DamageReport> damage_area(world::World& world, Vec3 center, float radius, float amount,
                                      std::string_view kind, float push) {
    std::vector<DamageReport> out;
    const world::Tree3D tree(world);
    std::vector<std::pair<entt::entity, float>> hit;
    for (const entt::entity entity : world.ecs().view<const Destructible3D>()) {
        const Vec3 at = tree.global(entity).origin;
        Vec3 near = at;
        if (const auto bounds = piece_bounds(world, entity); bounds.has_value()) {
            near = tree.global(entity).apply((bounds->first + bounds->second) * 0.5F);
        }
        const Vec3 d = near - center;
        const float distance = std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
        if (distance <= radius) {
            hit.emplace_back(entity, radius > 0.0F ? 1.0F - (distance / radius) : 1.0F);
        }
    }
    std::ranges::sort(hit, {}, [](const auto& h) { return entt::to_entity(h.first); });
    for (const auto& [entity, falloff] : hit) {
        if (!world.valid(entity)) {
            continue;
        }
        auto report = damage_body(world, entity, amount * falloff, kind, center, push * falloff);
        if (report) {
            out.push_back(std::move(report.value()));
        }
    }
    return out;
}

std::vector<std::string> settle_group(world::World& world, std::string_view group) {
    auto& ecs = world.ecs();
    std::vector<entt::entity> members;
    for (const auto [entity, piece] : ecs.view<FracturePiece3D>().each()) {
        if (piece.group == group) {
            members.push_back(entity);
        }
    }
    std::ranges::sort(members, {}, [](entt::entity e) { return entt::to_entity(e); });
    std::unordered_map<std::string, std::size_t> index;
    std::vector<entt::entity> standing;
    for (const entt::entity entity : members) {
        const auto* rigid = ecs.try_get<const world::RigidBody3D>(entity);
        if (rigid != nullptr && rigid->freeze) {
            index.emplace(std::string(world.id_of(entity)), standing.size());
            standing.push_back(entity);
        }
    }
    const world::Tree3D tree(world);
    float gravity = 9.8F;
    if (const detail::Space* space = detail::find_space(world); space != nullptr) {
        gravity = space->settings().gravity;
    }
    std::vector<world::StressPiece> stress(standing.size());
    std::shared_ptr<const world::DamageProfile> keep;
    const world::DamageProfile* profile = nullptr;
    for (std::size_t i = 0; i < standing.size(); ++i) {
        const entt::entity entity = standing[i];
        auto& piece = ecs.get<FracturePiece3D>(entity);
        // Links to pieces gone or loose go.
        std::erase_if(piece.links,
                      [&](const world::FractureLink& l) { return !index.contains(l.piece); });
        if (profile == nullptr) {
            if (const auto* d = ecs.try_get<const Destructible3D>(entity); d != nullptr) {
                profile = profile_of(world, *d, keep);
            }
        }
        Vec3 middle = tree.global(entity).origin;
        if (const auto bounds = piece_bounds(world, entity); bounds.has_value()) {
            middle = tree.global(entity).apply((bounds->first + bounds->second) * 0.5F);
        }
        stress[i].center = middle;
        stress[i].weight = ecs.get<const world::RigidBody3D>(entity).mass * gravity;
        stress[i].anchored = piece.anchored;
        for (const world::FractureLink& link : piece.links) {
            stress[i].links.emplace_back(index.at(link.piece), link.area);
        }
    }
    std::vector<std::string> fell;
    if (profile == nullptr) {
        return fell;
    }
    const world::StressResult result =
        world::settle_stress(stress, profile->strength, profile->compression, {0.0F, 1.0F, 0.0F});
    for (const auto& [a, b] : result.broken) {
        auto& first = ecs.get<FracturePiece3D>(standing[a]).links;
        auto& second = ecs.get<FracturePiece3D>(standing[b]).links;
        const std::string a_id(world.id_of(standing[a]));
        const std::string b_id(world.id_of(standing[b]));
        std::erase_if(first, [&](const world::FractureLink& l) { return l.piece == b_id; });
        std::erase_if(second, [&](const world::FractureLink& l) { return l.piece == a_id; });
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

void break_and_age(world::World& world, Space& space, float seconds) {
    auto& ecs = world.ecs();
    DestructionState& state = state_of(world);
    if (!state.started) {
        // Groups standing when the world was made settle once.
        state.started = true;
        for (const auto [entity, piece] : ecs.view<const FracturePiece3D>().each()) {
            if (!piece.group.empty() && !piece.links.empty()) {
                state.unsettled.insert(piece.group);
            }
        }
    }
    // Bodies hit hard enough take damage, or break, in the order the world keeps them.
    std::unordered_map<entt::entity, Space::Impact> impacts = space.take_impacts();
    if (!impacts.empty()) {
        std::vector<std::pair<entt::entity, Space::Impact>> hit;
        for (const entt::entity entity : ecs.view<const Destructible3D>()) {
            if (const auto found = impacts.find(entity); found != impacts.end()) {
                hit.emplace_back(entity, found->second);
            }
        }
        for (const auto& [entity, impact] : hit) {
            if (!world.valid(entity)) {
                continue;
            }
            const auto& destructible = ecs.get<const Destructible3D>(entity);
            if (destructible.profile.empty()) {
                if (auto broken = break_body(world, entity); !broken) {
                    log::warn("a Destructible3D can't break",
                              {{"id", std::string(world.id_of(entity))},
                               {"error", broken.error().message}});
                }
                continue;
            }
            std::shared_ptr<const world::DamageProfile> keep;
            const world::DamageProfile* profile = profile_of(world, destructible, keep);
            if (profile == nullptr) {
                continue;
            }
            const float past = impact.impulse - profile->impact_threshold;
            if (auto damaged = damage_body(world, entity, past * profile->impact_damage, "impact",
                                           impact.point, 0.0F);
                !damaged) {
                log::warn("a Destructible3D can't break", {{"id", std::string(world.id_of(entity))},
                                                           {"error", damaged.error().message}});
            }
        }
    }
    // Links to pieces that went let go, and groups that changed settle.
    for (const auto [entity, piece] : ecs.view<FracturePiece3D>().each()) {
        for (const world::FractureLink& link : piece.links) {
            const entt::entity other = world.find(link.piece);
            const auto* rigid =
                other != entt::null ? ecs.try_get<const world::RigidBody3D>(other) : nullptr;
            if (rigid == nullptr || !rigid->freeze) {
                state.unsettled.insert(piece.group);
                break;
            }
        }
    }
    while (!state.unsettled.empty()) {
        const std::string group = *state.unsettled.begin();
        state.unsettled.erase(state.unsettled.begin());
        (void)settle_group(world, group);
    }
    // Debris goes when its lifetime runs out.
    std::vector<entt::entity> gone;
    for (const auto [entity, piece] : ecs.view<FracturePiece3D>().each()) {
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

} // namespace slash::physics3d
