// The played world in Autopilot's kit frame: the player, things' transforms and bounds, what the
// player sees (view) and what changes over time (watch). After the Godot addon's lib/world.gd and
// lib/sight.gd (Autopilot, Apache 2.0), on SlashEngine's entities and components.

#include "slash/world/world.hpp"

#include "internal.hpp"

#include "slash/command/session.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <ranges>
#include <regex>
#include <set>
#include <utility>

namespace slash::autopilot::detail {

// ---- The kit frame ----

double r2(double x) {
    if (!std::isfinite(x)) {
        return x;
    }
    return std::round(x * 100.0) / 100.0;
}

json::Value kit(Vec3 v) {
    json::Value out;
    out["X"] = r2(-static_cast<double>(v.z) * kScale);
    out["Y"] = r2(static_cast<double>(v.x) * kScale);
    out["Z"] = r2(static_cast<double>(v.y) * kScale);
    return out;
}

json::Value kit_dir(Vec3 v) {
    json::Value out;
    out["X"] = r2(-static_cast<double>(v.z));
    out["Y"] = r2(static_cast<double>(v.x));
    out["Z"] = r2(static_cast<double>(v.y));
    return out;
}

json::Value kit2d(Vec2 v) {
    json::Value out;
    out["X"] = r2(v.x);
    out["Y"] = 0.0;
    out["Z"] = r2(-static_cast<double>(v.y));
    return out;
}

json::Value kit_extent(Vec3 size) {
    json::Value out;
    out["X"] = r2(std::abs(size.z) * kScale * 0.5);
    out["Y"] = r2(std::abs(size.x) * kScale * 0.5);
    out["Z"] = r2(std::abs(size.y) * kScale * 0.5);
    return out;
}

namespace {

float number(const json::Value* value, std::string_view key, float fallback = 0.0F) {
    if (value == nullptr || !value->is_object()) {
        return fallback;
    }
    const json::Value* found = value->find(key);
    return found != nullptr && found->is_number() ? static_cast<float>(found->as_double())
                                                  : fallback;
}

double arg_number(const json::Value& args, std::string_view key, double fallback) {
    const json::Value* found = args.find(key);
    return found != nullptr && found->is_number() ? found->as_double() : fallback;
}

bool arg_flag(const json::Value& args, std::string_view key) {
    const json::Value* found = args.find(key);
    return found != nullptr && found->is_bool() && found->as_bool();
}

world::Affine3D rotation_only(const world::Affine3D& transform) {
    world::Affine3D basis = world::orthonormalized(transform);
    basis.origin = {};
    return basis;
}

} // namespace

Vec3 from_kit_dir(const json::Value* value) {
    return {number(value, "Y"), number(value, "Z"), -number(value, "X")};
}

float yaw_of(const world::Affine3D& basis) {
    return static_cast<float>(-world::euler(rotation_only(basis)).y * kDegrees);
}

float pitch_of(const world::Affine3D& basis) {
    return static_cast<float>(world::euler(rotation_only(basis)).x * kDegrees);
}

json::Value kit_rot(const world::Affine3D& basis) {
    const Vec3 e = world::euler(rotation_only(basis));
    json::Value out;
    out["Pitch"] = r2(e.x * kDegrees);
    out["Yaw"] = r2(-e.y * kDegrees);
    out["Roll"] = r2(-e.z * kDegrees);
    return out;
}

json::Value kit_rot2d(float rotation) {
    json::Value out;
    out["Pitch"] = r2(-rotation * kDegrees);
    out["Yaw"] = 0.0;
    out["Roll"] = 0.0;
    return out;
}

float yaw_of_dir(Vec3 d) {
    return static_cast<float>(std::atan2(d.x, -d.z) * kDegrees);
}

float wrap180(float degrees) {
    float a = std::fmod(degrees + 180.0F, 360.0F);
    if (a < 0.0F) {
        a += 360.0F;
    }
    return a - 180.0F;
}

void Box3::add(Vec3 p) {
    if (empty) {
        low = p;
        high = p;
        empty = false;
        return;
    }
    low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
    high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
}

void Box3::merge(const Box3& other) {
    if (!other.empty) {
        add(other.low);
        add(other.high);
    }
}

Box3 Box3::placed(const world::Affine3D& transform) const {
    Box3 out;
    if (empty) {
        return out;
    }
    for (int i = 0; i < 8; ++i) {
        out.add(transform.apply({(i & 1) != 0 ? high.x : low.x, (i & 2) != 0 ? high.y : low.y,
                                 (i & 4) != 0 ? high.z : low.z}));
    }
    return out;
}

void Rect2::add(Vec2 p) {
    if (empty) {
        low = p;
        high = p;
        empty = false;
        return;
    }
    low = {std::min(low.x, p.x), std::min(low.y, p.y)};
    high = {std::max(high.x, p.x), std::max(high.y, p.y)};
}

void Rect2::merge(const Rect2& other) {
    if (!other.empty) {
        add(other.low);
        add(other.high);
    }
}

// ---- Look ----

namespace {

template <typename... T>
bool has_any(const world::World& world, entt::entity entity) {
    return world.ecs().valid(entity) && world.ecs().any_of<T...>(entity);
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

Look::Look(command::Session& session_)
    : session(session_), world(session_.world()), tree3(world), tree2(world) {
    std::set<entt::entity> seen;
    for (const entt::entity e : tree3.order()) {
        if (seen.insert(e).second) {
            entities_.push_back(e);
        }
    }
    std::vector<std::string_view> ids = world.ids();
    std::ranges::sort(ids);
    for (const std::string_view id : ids) {
        const entt::entity e = world.find(id);
        if (e != entt::null && seen.insert(e).second) {
            entities_.push_back(e);
        }
    }
    for (const entt::entity e : entities_) {
        const entt::entity p = parent(e);
        if (p != entt::null) {
            children_[p].push_back(e);
        }
    }
    camera3d_ = world::current_camera(world, tree3);
    camera2d_ = world::current_camera(world, tree2);
    // The body the current camera hangs from.
    for (const entt::entity camera : {camera3d_, camera2d_}) {
        for (entt::entity e = camera; e != entt::null && player_ == entt::null; e = parent(e)) {
            if (has_any<world::CharacterBody3D, world::RigidBody3D, world::AnimatableBody3D,
                        world::CharacterBody2D, world::RigidBody2D, world::AnimatableBody2D>(world,
                                                                                             e)) {
                player_ = e;
            }
        }
        if (player_ != entt::null) {
            return;
        }
    }
    for (const entt::entity e : entities_) {
        const std::string n = lower(name(e));
        if ((n == "player" || n == "players") && (is3d(e) || is2d(e))) {
            player_ = e;
            return;
        }
    }
    for (const entt::entity e : entities_) {
        if (world.ecs().all_of<world::CharacterBody3D>(e)) {
            player_ = e;
            return;
        }
    }
    for (const entt::entity e : entities_) {
        if (world.ecs().all_of<world::CharacterBody2D>(e)) {
            player_ = e;
            return;
        }
    }
}

std::string Look::dimension() const {
    if ((player_ != entt::null && is3d(player_)) || camera3d_ != entt::null) {
        return "3d";
    }
    if ((player_ != entt::null && is2d(player_)) || camera2d_ != entt::null) {
        return "2d";
    }
    if (!world.ecs().view<world::Transform3D>().empty()) {
        return "3d";
    }
    if (!world.ecs().view<world::Transform2D>().empty()) {
        return "2d";
    }
    return "ui";
}

bool Look::is3d(entt::entity entity) const {
    return has_any<world::Transform3D>(world, entity);
}

bool Look::is2d(entt::entity entity) const {
    return has_any<world::Transform2D>(world, entity);
}

bool Look::is_body(entt::entity entity) const {
    return has_any<world::StaticBody3D, world::AnimatableBody3D, world::RigidBody3D,
                   world::CharacterBody3D, world::Area3D, world::StaticBody2D,
                   world::AnimatableBody2D, world::RigidBody2D, world::CharacterBody2D,
                   world::Area2D>(world, entity);
}

entt::entity Look::parent(entt::entity entity) const {
    const entt::entity p = tree3.parent(entity);
    return p != entt::null ? p : tree2.parent(entity);
}

bool Look::is_ancestor(entt::entity ancestor, entt::entity entity) const {
    for (entt::entity e = entity; e != entt::null; e = parent(e)) {
        if (e == ancestor) {
            return true;
        }
    }
    return false;
}

entt::entity Look::thing_of(entt::entity entity) const {
    for (entt::entity e = entity; e != entt::null; e = parent(e)) {
        if (is_body(e)) {
            return e;
        }
    }
    // An instanced scene's entities are "<instance>/<id>": the instance is the thing.
    const std::string own = id(entity);
    if (const auto slash = own.find('/'); slash != std::string::npos) {
        const entt::entity root = world.find(std::string_view(own).substr(0, slash));
        if (root != entt::null) {
            return root;
        }
    }
    return entity;
}

std::string Look::id(entt::entity entity) const {
    return std::string(world.id_of(entity));
}

std::string Look::name(entt::entity entity) const {
    const std::string own = id(entity);
    const auto slash = own.rfind('/');
    return slash == std::string::npos ? own : own.substr(slash + 1);
}

std::string Look::class_of(entt::entity entity) const {
    static constexpr std::array<std::string_view, 24> kClasses{
        "CharacterBody3D",  "RigidBody3D",    "AnimatableBody3D", "StaticBody3D",
        "Area3D",           "VehicleBody3D",  "CharacterBody2D",  "RigidBody2D",
        "AnimatableBody2D", "StaticBody2D",   "Area2D",           "Camera3D",
        "Camera2D",         "MeshInstance3D", "Sprite2D",         "AnimatedSprite2D",
        "TileMapLayer",     "OmniLight3D",    "SpotLight3D",      "DirectionalLight3D",
        "PointLight2D",     "Control",        "Transform3D",      "Transform2D"};
    for (const std::string_view name : kClasses) {
        if (const world::ComponentType* type = world.components().find(name);
            type != nullptr && type->has(world, entity)) {
            return std::string(name);
        }
    }
    for (const auto& type : world.components().types()) {
        if (type->has(world, entity)) {
            return type->name();
        }
    }
    return "Entity";
}

entt::entity Look::resolve(std::string_view id) const {
    if (id.empty()) {
        return entt::null;
    }
    if (id.front() == '/') {
        id.remove_prefix(1);
    }
    return world.find(id);
}

world::Affine3D Look::global3(entt::entity entity) const {
    return tree3.global(entity);
}

world::Affine2D Look::global2(entt::entity entity) const {
    return tree2.global_affine(entity);
}

Vec3 Look::velocity3(entt::entity entity) const {
    if (const auto* body = world.ecs().try_get<world::CharacterBody3D>(entity)) {
        return body->velocity;
    }
    if (const auto* body = world.ecs().try_get<world::RigidBody3D>(entity)) {
        return body->linear_velocity;
    }
    return {};
}

Vec2 Look::velocity2(entt::entity entity) const {
    if (const auto* body = world.ecs().try_get<world::CharacterBody2D>(entity)) {
        return body->velocity;
    }
    if (const auto* body = world.ecs().try_get<world::RigidBody2D>(entity)) {
        return body->linear_velocity;
    }
    return {};
}

std::vector<entt::entity> Look::subtree(entt::entity root) const {
    std::vector<entt::entity> out;
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        if (const auto found = children_.find(e); found != children_.end()) {
            for (auto it : std::ranges::reverse_view(found->second)) {
                stack.push_back(it);
            }
        }
    }
    return out;
}

namespace {

// A collision shape's box in its entity's space.
Box3 shape_box(const world::CollisionShape3D& shape) {
    Box3 box;
    if (shape.disabled) {
        return box;
    }
    const auto add_half = [&](Vec3 half) {
        box.add(half * -1.0F);
        box.add(half);
    };
    if (shape.shape == "box") {
        add_half(shape.size * 0.5F);
    } else if (shape.shape == "sphere") {
        add_half({shape.radius, shape.radius, shape.radius});
    } else if (shape.shape == "capsule" || shape.shape == "cylinder") {
        add_half({shape.radius, shape.height * 0.5F, shape.radius});
    } else if (shape.shape == "convex_polygon") {
        for (const Vec3 p : shape.points) {
            box.add(p);
        }
    } else if (shape.shape == "concave_polygon") {
        for (const Vec3 p : shape.faces) {
            box.add(p);
        }
    }
    if (box.empty) {
        return box;
    }
    world::Transform3D local;
    local.position = shape.position;
    local.rotation = shape.rotation;
    return box.placed(world::affine(local));
}

Rect2 shape_rect(const world::CollisionShape2D& shape, const world::Affine2D& global) {
    Rect2 local;
    if (shape.disabled) {
        return local;
    }
    if (shape.shape == "rectangle") {
        local.add(shape.size * -0.5F);
        local.add(shape.size * 0.5F);
    } else if (shape.shape == "circle") {
        local.add({-shape.radius, -shape.radius});
        local.add({shape.radius, shape.radius});
    } else if (shape.shape == "capsule") {
        local.add({-shape.radius, -shape.height * 0.5F});
        local.add({shape.radius, shape.height * 0.5F});
    } else if (shape.shape == "segment") {
        local.add(shape.a);
        local.add(shape.b);
    } else {
        for (const Vec2 p : shape.points) {
            local.add(p);
        }
    }
    Rect2 out;
    if (local.empty) {
        return out;
    }
    world::Transform2D t;
    t.position = shape.position;
    t.rotation = shape.rotation;
    const world::Affine2D placed = global * world::affine(t);
    for (const Vec2 corner : {local.low, local.high, Vec2{local.low.x, local.high.y},
                              Vec2{local.high.x, local.low.y}}) {
        out.add(placed.apply(corner));
    }
    return out;
}

} // namespace

Box3 Look::mesh_bounds(entt::entity entity) const {
    if (const auto cached = mesh_boxes_.find(entity); cached != mesh_boxes_.end()) {
        return cached->second;
    }
    Box3 local;
    if (const auto* instance = world.ecs().try_get<world::MeshInstance3D>(entity)) {
        if (!instance->mesh.empty()) {
            if (auto model = session.assets().load<scene3d::Model>(instance->mesh); model) {
                const scene3d::Bounds b = scene3d::bounds(*model.value());
                local.add(b.min);
                local.add(b.max);
            }
        } else if (auto primitive = scene3d::primitive_of(world, entity); primitive) {
            if (const std::optional<scene3d::Primitive>& shape = primitive.value(); shape) {
                if (auto mesh = scene3d::make_mesh(*shape); mesh) {
                    for (const render::Vertex3D& v : mesh.value().vertices) {
                        local.add({v.position[0], v.position[1], v.position[2]});
                    }
                }
            }
        }
    }
    Box3 out = local.placed(global3(entity));
    mesh_boxes_[entity] = out;
    return out;
}

Box3 Look::bounds3(entt::entity entity) const {
    const std::vector<entt::entity> under = subtree(entity);
    Box3 box;
    for (const entt::entity e : under) {
        if (world.ecs().all_of<world::MeshInstance3D>(e) && tree3.visible(e)) {
            box.merge(mesh_bounds(e));
        }
    }
    if (!box.empty) {
        return box;
    }
    for (const entt::entity e : under) {
        if (const auto* shape = world.ecs().try_get<world::CollisionShape3D>(e)) {
            box.merge(shape_box(*shape).placed(global3(e)));
        }
    }
    if (box.empty) {
        box.add(position3(entity));
    }
    return box;
}

Rect2 Look::bounds2(entt::entity entity) const {
    Rect2 rect;
    for (const entt::entity e : subtree(entity)) {
        if (const auto* shape = world.ecs().try_get<world::CollisionShape2D>(e)) {
            rect.merge(shape_rect(*shape, global2(e)));
        }
    }
    if (rect.empty) {
        rect.add(position2(entity));
    }
    return rect;
}

Look::Body Look::body3(entt::entity entity) const {
    Body body;
    body.center = position3(entity);
    const auto from = [&](entt::entity e, const world::CollisionShape3D& shape) {
        body.has_shape = true;
        body.shape = shape.shape;
        body.center = global3(e).apply(shape.position);
        if (shape.shape == "capsule" || shape.shape == "cylinder") {
            body.half_height = shape.height * 0.5F;
            body.radius = shape.radius;
        } else if (shape.shape == "sphere") {
            body.half_height = shape.radius;
            body.radius = shape.radius;
        } else if (shape.shape == "box") {
            body.half_height = shape.size.y * 0.5F;
            body.radius = std::max(shape.size.x, shape.size.z) * 0.5F;
        }
    };
    if (const auto* own = world.ecs().try_get<world::CollisionShape3D>(entity);
        own != nullptr && !own->disabled) {
        from(entity, *own);
        return body;
    }
    if (const auto found = children_.find(entity); found != children_.end()) {
        for (const entt::entity child : found->second) {
            const auto* shape = world.ecs().try_get<world::CollisionShape3D>(child);
            if (shape != nullptr && !shape->disabled && !is_body(child)) {
                from(child, *shape);
                return body;
            }
        }
    }
    return body;
}

Look::Body Look::body2(entt::entity entity) const {
    Body body;
    const Vec2 at = position2(entity);
    body.center = {at.x, at.y, 0.0F};
    const auto from = [&](entt::entity e, const world::CollisionShape2D& shape) {
        body.has_shape = true;
        body.shape = shape.shape;
        const Vec2 c = global2(e).apply(shape.position);
        body.center = {c.x, c.y, 0.0F};
        if (shape.shape == "rectangle") {
            body.half_height = shape.size.y * 0.5F;
            body.radius = shape.size.x * 0.5F;
        } else if (shape.shape == "circle") {
            body.half_height = shape.radius;
            body.radius = shape.radius;
        } else if (shape.shape == "capsule") {
            body.half_height = shape.height * 0.5F;
            body.radius = shape.radius;
        }
    };
    if (const auto* own = world.ecs().try_get<world::CollisionShape2D>(entity);
        own != nullptr && !own->disabled) {
        from(entity, *own);
        return body;
    }
    if (const auto found = children_.find(entity); found != children_.end()) {
        for (const entt::entity child : found->second) {
            const auto* shape = world.ecs().try_get<world::CollisionShape2D>(child);
            if (shape != nullptr && !shape->disabled && !is_body(child)) {
                from(child, *shape);
                return body;
            }
        }
    }
    return body;
}

// ---- find and transforms ----

json::Value find(const Look& look, const json::Value& args) {
    const json::Value* cls_value = args.find("class");
    std::string cls = cls_value != nullptr && cls_value->is_string() ? cls_value->as_string() : "";
    const json::Value* name_value = args.find("name");
    const std::string name =
        lower(name_value != nullptr && name_value->is_string() ? name_value->as_string() : "");
    const auto limit = static_cast<std::size_t>(std::max(0.0, arg_number(args, "limit", 1000)));
    const bool things = arg_flag(args, "things");
    // SlashEngine has no groups; "group:<name>" finds entities with a component of that name.
    if (cls.starts_with("group:")) {
        cls = cls.substr(6);
    }
    const world::ComponentType* type = cls.empty() ? nullptr : look.world.components().find(cls);
    json::Array out;
    for (const entt::entity e : look.entities()) {
        if (out.size() >= limit) {
            break;
        }
        const std::string id = look.id(e);
        if (id.empty()) {
            continue;
        }
        if (things) {
            // The scene's things, as Unreal's actors: roots, bodies and areas, and instances'
            // roots, not every mesh and shape under them.
            const bool root = look.parent(e) == entt::null;
            if (!root && !look.is_body(e) && look.world.prefab_of(e) == nullptr) {
                continue;
            }
        }
        if (!cls.empty() && (type == nullptr || !type->has(look.world, e)) &&
            lower(look.class_of(e)) != lower(cls)) {
            continue;
        }
        if (!name.empty() && lower(look.name(e)).find(name) == std::string::npos) {
            continue;
        }
        out.emplace_back(id);
    }
    json::Value result;
    result["objects"] = std::move(out);
    return result;
}

json::Value transform_of(const Look& look, entt::entity entity) {
    if (look.is3d(entity)) {
        const Box3 b = look.bounds3(entity);
        json::Value out;
        out["location"] = kit(look.position3(entity));
        out["rotation"] = kit_rot(look.global3(entity));
        out["velocity"] = kit(look.velocity3(entity));
        out["center"] = kit(b.center());
        out["extent"] = kit_extent(b.size());
        return out;
    }
    if (look.is2d(entity)) {
        const Rect2 r = look.bounds2(entity);
        const world::Transform2D parts = world::parts(look.global2(entity));
        json::Value out;
        out["location"] = kit2d(parts.position);
        out["rotation"] = kit_rot2d(parts.rotation);
        out["velocity"] = kit2d(look.velocity2(entity));
        out["center"] = kit2d(r.center());
        json::Value extent;
        extent["X"] = r2(r.size().x / 2.0);
        extent["Y"] = 0.0;
        extent["Z"] = r2(r.size().y / 2.0);
        out["extent"] = std::move(extent);
        return out;
    }
    return nullptr;
}

json::Value transforms(const Look& look, const json::Value& args) {
    json::Value objects = json::Object{};
    if (const json::Value* list = args.find("objects"); list != nullptr && list->is_array()) {
        for (const json::Value& item : list->as_array()) {
            if (!item.is_string()) {
                continue;
            }
            const entt::entity e = look.resolve(item.as_string());
            objects[item.as_string()] = e == entt::null ? json::Value() : transform_of(look, e);
        }
    }
    json::Value result;
    result["objects"] = std::move(objects);
    return result;
}

// ---- pose and movement ----

Answer pose(const Look& look) {
    const entt::entity p = look.player();
    if (p == entt::null) {
        return Answer::failure("No player");
    }
    json::Value out;
    out["path"] = look.id(p);
    if (look.is3d(p)) {
        const Look::Body body = look.body3(p);
        const entt::entity cam = look.camera3d();
        const world::Affine3D basis = cam != entt::null ? look.global3(cam) : look.global3(p);
        out["at"] = kit(body.center);
        out["feet"] = kit(body.center - Vec3{0.0F, body.half_height, 0.0F});
        out["eye"] = kit(cam != entt::null ? basis.origin : body.center);
        out["yaw"] = r2(yaw_of(basis));
        out["pitch"] = r2(pitch_of(basis));
        out["velocity"] = kit(look.velocity3(p));
        const auto* character = look.world.ecs().try_get<world::CharacterBody3D>(p);
        out["on_ground"] = character != nullptr ? json::Value(character->on_floor) : json::Value();
        return Answer::success(std::move(out));
    }
    const Look::Body body = look.body2(p);
    const Vec2 center{body.center.x, body.center.y};
    out["at"] = kit2d(center);
    out["feet"] = kit2d(center + Vec2{0.0F, body.half_height});
    Vec2 eye = center;
    if (look.camera2d() != entt::null) {
        eye = look.position2(look.camera2d());
    }
    out["eye"] = kit2d(eye);
    out["yaw"] = 0.0;
    out["pitch"] = 0.0;
    out["velocity"] = kit2d(look.velocity2(p));
    const auto* character = look.world.ecs().try_get<world::CharacterBody2D>(p);
    out["on_ground"] = character != nullptr ? json::Value(character->on_floor) : json::Value();
    return Answer::success(std::move(out));
}

namespace {

// A number the player's own components hold under a field whose name matches, as the addon
// reads its script's variables: [value, "Component/field"].
std::optional<std::pair<double, std::string>> component_number(const Look& look, entt::entity p,
                                                               const std::regex& pattern) {
    for (const auto& type : look.world.components().types()) {
        if (!type->has(look.world, p)) {
            continue;
        }
        const json::Value values = type->get(look.world, p);
        if (!values.is_object()) {
            continue;
        }
        for (const auto& [field, value] : values.as_object()) {
            if (value.is_number() && std::regex_search(lower(field), pattern)) {
                return std::pair{value.as_double(), type->name() + "/" + field};
            }
        }
    }
    return std::nullopt;
}

} // namespace

Answer movement(const Look& look) {
    const entt::entity p = look.player();
    if (p == entt::null) {
        return Answer::failure("No player");
    }
    const bool three = look.is3d(p);
    const double scale = three ? kScale : 1.0;
    json::Value out;
    for (const char* key :
         {"walk_speed", "jump_z", "gravity", "half_height", "radius", "step", "max_slope"}) {
        out[key] = nullptr;
    }
    json::Value sources = json::Object{};
    static const std::regex kSpeed(
        "^(walk_?|move_?|movement_?|max_?|run_?|player_?|base_?)?speed$");
    static const std::regex kJump("^(jump_?)(velocity|speed|force|impulse|strength|power)$");
    static const std::regex kJumpHeight("^(jump_?)(height)$");
    static const std::regex kGravity("^gravity$");
    // Skip the engine's own components: a game's player component holds the tuning.
    if (const auto speed = component_number(look, p, kSpeed)) {
        out["walk_speed"] = r2(std::abs(speed->first) * scale);
        sources["walk_speed"] = "component " + speed->second;
    }
    double g = 0.0;
    if (const auto gravity = component_number(look, p, kGravity)) {
        g = std::abs(gravity->first);
        sources["gravity"] = "component " + gravity->second;
    } else if (three) {
        const auto found = physics3d::gravity(look.world, p);
        g = found ? length(found->gravity) : 9.8;
        sources["gravity"] = "physics_3d";
    } else {
        const auto found = physics2d::gravity(look.world, p);
        g = found ? length(found->gravity) : 980.0;
        sources["gravity"] = "physics_2d";
    }
    out["gravity"] = r2(g * scale);
    if (const auto jump = component_number(look, p, kJump)) {
        out["jump_z"] = r2(std::abs(jump->first) * scale);
        sources["jump_z"] = "component " + jump->second;
    } else if (const auto height = component_number(look, p, kJumpHeight)) {
        out["jump_z"] = r2(std::sqrt(2.0 * g * std::abs(height->first)) * scale);
        sources["jump_z"] = "component " + height->second + " (as sqrt(2 g h))";
    }
    const Look::Body body = three ? look.body3(p) : look.body2(p);
    if (body.has_shape) {
        out["half_height"] = r2(body.half_height * scale);
        out["radius"] = r2(body.radius * scale);
        sources["half_height"] = "CollisionShape " + body.shape;
    }
    if (const auto* character = look.world.ecs().try_get<world::CharacterBody3D>(p)) {
        out["max_slope"] = r2(character->floor_max_angle * kDegrees);
        sources["step"] = "CharacterBody3D has no step-up (step is null)";
    } else if (const auto* character2 = look.world.ecs().try_get<world::CharacterBody2D>(p)) {
        out["max_slope"] = r2(character2->floor_max_angle * kDegrees);
        sources["step"] = "CharacterBody2D has no step-up (step is null)";
    }
    out["sources"] = std::move(sources);
    return Answer::success(std::move(out));
}

// ---- view ----

namespace {

const json::Array kOmitted{"held",      "scene.sun",         "openings",      "ways_up",
                           "lane",      "top_slope_degrees", "low_end",       "high_end",
                           "uphill",    "body.under",        "body.overhead", "look.highlighted",
                           "look.limp", "look.effects",      "blocks_player", "sonar"};

std::set<std::string> only_set(const json::Value& args) {
    std::set<std::string> out;
    if (const json::Value* only = args.find("only"); only != nullptr && only->is_array()) {
        for (const json::Value& item : only->as_array()) {
            if (item.is_string()) {
                out.insert(item.as_string());
            }
        }
    }
    return out;
}

// What a thing looks like: its parts (meshes' entity names), lights and animation.
json::Value look_of(const Look& look, entt::entity thing) {
    json::Value out;
    json::Array parts;
    int lights = 0;
    float brightest = 0.0F;
    std::string animation;
    for (const entt::entity e : look.subtree(thing)) {
        const bool shown = look.tree3.visible(e);
        if (look.world.ecs().all_of<world::MeshInstance3D>(e) && shown && e != thing) {
            parts.emplace_back(look.name(e));
        }
        float energy = -1.0F;
        if (const auto* omni = look.world.ecs().try_get<world::OmniLight3D>(e)) {
            energy = omni->light_energy;
        } else if (const auto* spot = look.world.ecs().try_get<world::SpotLight3D>(e)) {
            energy = spot->light_energy;
        }
        if (energy > 0.0F && shown) {
            ++lights;
            brightest = std::max(brightest, energy);
        }
        if (const auto* player = look.world.ecs().try_get<world::AnimationPlayer>(e);
            player != nullptr && player->playing && !player->animation.empty() &&
            animation.empty()) {
            animation = player->animation;
        }
    }
    if (!parts.empty()) {
        out["parts"] = std::move(parts);
    }
    if (lights > 0) {
        json::Value l;
        l["on"] = lights;
        l["brightness"] = r2(brightest);
        out["lights"] = std::move(l);
    }
    if (!animation.empty()) {
        out["animation"] = animation;
    }
    return out;
}

struct Frustum {
    world::Affine3D eye;
    world::Affine3D to_camera;
    float tan_h = 1.0F;
    float tan_v = 1.0F;
    float near = 0.05F;
    bool orthogonal = false;
    float half_w = 1.0F;
    float half_h = 1.0F;

    // The point in -1..1 on the screen (right and up positive), with whether it is in front.
    [[nodiscard]] std::optional<Vec2> project(Vec3 p) const {
        const Vec3 c = to_camera.apply(p);
        if (orthogonal) {
            return Vec2{c.x / half_w, c.y / half_h};
        }
        if (-c.z <= near) {
            return std::nullopt;
        }
        return Vec2{(c.x / -c.z) / tan_h, (c.y / -c.z) / tan_v};
    }
    [[nodiscard]] bool inside(Vec3 p) const {
        const auto s = project(p);
        return s && std::abs(s->x) <= 1.0F && std::abs(s->y) <= 1.0F;
    }
};

Answer view3d(Look& look, const json::Value& args) {
    const entt::entity cam = look.camera3d();
    if (cam == entt::null) {
        return Answer::failure("No camera");
    }
    const entt::entity p = look.player();
    const float max_m = static_cast<float>(arg_number(args, "max_distance", 5000.0)) / kScale;
    const auto max_count =
        static_cast<std::size_t>(std::max(0.0, arg_number(args, "max_count", 24.0)));
    const std::set<std::string> only = only_set(args);
    const auto& camera = look.world.ecs().get<world::Camera3D>(cam);
    // The window's aspect: ui::settings' viewport, as frame.capture draws.
    float aspect = 16.0F / 9.0F;
    if (const Vec2 size = ui::settings(look.world).viewport; size.x > 0.0F && size.y > 0.0F) {
        aspect = size.x / size.y;
    }
    Frustum frustum;
    frustum.eye = world::orthonormalized(look.global3(cam));
    frustum.to_camera = frustum.eye.inverse();
    frustum.near = camera.near;
    const auto half = static_cast<float>(std::tan(camera.fov * 0.5 / kDegrees));
    float vfov = camera.fov;
    if (camera.keep_aspect == "width") {
        frustum.tan_h = half;
        frustum.tan_v = half / aspect;
        vfov = static_cast<float>(2.0 * std::atan(frustum.tan_v) * kDegrees);
    } else {
        frustum.tan_v = half;
        frustum.tan_h = half * aspect;
    }
    const auto hfov = static_cast<float>(2.0 * std::atan(frustum.tan_h) * kDegrees);
    if (camera.projection == "orthogonal") {
        frustum.orthogonal = true;
        const float span = camera.size * 0.5F;
        frustum.half_w = camera.keep_aspect == "width" ? span : span * aspect;
        frustum.half_h = camera.keep_aspect == "width" ? span / aspect : span;
    }
    const Vec3 cam_pos = frustum.eye.origin;
    const float view_yaw = yaw_of(frustum.eye);
    float feet_y = cam_pos.y;
    entt::entity standing_on = entt::null;
    json::Value body = json::Object{};
    physics3d::QueryFilter exclude;
    if (p != entt::null) {
        exclude.exclude.push_back(p);
    }
    if (p != entt::null && look.is3d(p)) {
        const Look::Body shape = look.body3(p);
        feet_y = shape.center.y - shape.half_height;
        body["velocity"] = kit(look.velocity3(p));
        body["feet_z"] = r2(feet_y * kScale);
        body["crouched"] = false;
        if (const auto* character = look.world.ecs().try_get<world::CharacterBody3D>(p)) {
            body["on_ground"] = character->on_floor;
            body["falling"] = !character->on_floor;
            if (character->on_floor) {
                const Vec3 n = normalized(character->floor_normal);
                body["floor_slope_degrees"] =
                    r2(std::acos(
                           std::clamp(dot(n, normalized(character->up_direction)), -1.0F, 1.0F)) *
                       kDegrees);
                const auto hit = physics3d::raycast(
                    look.world, shape.center,
                    shape.center - Vec3{0.0F, shape.half_height + 0.25F, 0.0F}, exclude);
                if (hit) {
                    standing_on = look.thing_of(hit->entity);
                    body["standing_on"] = look.name(standing_on);
                }
            }
            json::Array touching;
            std::set<entt::entity> seen;
            for (const entt::entity hit : physics3d::contacts(look.world, p).bodies) {
                const entt::entity t = look.thing_of(hit);
                if (t == standing_on || !seen.insert(t).second) {
                    continue;
                }
                const Vec3 way = look.bounds3(t).center() - shape.center;
                json::Value item;
                item["thing"] = look.name(t);
                item["path"] = look.id(t);
                const float yaw = yaw_of_dir(way);
                item["yaw"] = r2(yaw);
                item["turn"] = r2(wrap180(yaw - view_yaw));
                touching.push_back(std::move(item));
            }
            if (!touching.empty()) {
                body["touching"] = std::move(touching);
            }
        } else {
            body["on_ground"] = nullptr;
            body["falling"] = nullptr;
        }
    }
    // Things: drawn parts grouped by thing.
    std::vector<entt::entity> order;
    std::unordered_map<entt::entity, Box3> boxes;
    for (const entt::entity e : look.entities()) {
        if (!look.world.ecs().all_of<world::MeshInstance3D>(e) || !look.tree3.visible(e)) {
            continue;
        }
        if (p != entt::null && look.is_ancestor(p, e)) {
            continue;
        }
        const Box3 b = look.mesh_bounds(e);
        if (b.empty || length(b.size()) <= 0.0F) {
            continue;
        }
        const entt::entity t = look.thing_of(e);
        if (p != entt::null && look.is_ancestor(p, t)) {
            continue;
        }
        if (!only.empty() && !only.contains(look.name(t)) && !only.contains(look.id(t))) {
            continue;
        }
        if (!boxes.contains(t)) {
            order.push_back(t);
        }
        boxes[t].merge(b);
    }
    // What the crosshair is on.
    physics3d::QueryFilter with_areas = exclude;
    with_areas.collide_with_areas = true;
    const Vec3 forward = normalized(frustum.eye.basis({0.0F, 0.0F, -1.0F}));
    entt::entity crosshair = entt::null;
    if (const auto hit =
            physics3d::raycast(look.world, cam_pos, cam_pos + forward * max_m, with_areas)) {
        crosshair = look.thing_of(hit->entity);
    }
    json::Array out;
    for (const entt::entity t : order) {
        const Box3& box = boxes[t];
        const Vec3 size = box.size();
        if (t == standing_on && size.x > 10.0F && size.z > 10.0F) {
            continue; // the ground
        }
        const Vec3 center = box.center();
        const Vec3 nearest{std::clamp(cam_pos.x, box.low.x, box.high.x),
                           std::clamp(cam_pos.y, box.low.y, box.high.y),
                           std::clamp(cam_pos.z, box.low.z, box.high.z)};
        const float dist = length(nearest - cam_pos);
        if (dist > max_m) {
            continue;
        }
        const bool center_in = frustum.inside(center);
        bool any_in = center_in;
        for (int i = 0; i < 8 && !any_in; ++i) {
            any_in = frustum.inside({(i & 1) != 0 ? box.high.x : box.low.x,
                                     (i & 2) != 0 ? box.high.y : box.low.y,
                                     (i & 4) != 0 ? box.high.z : box.low.z});
        }
        if (!any_in) {
            continue;
        }
        bool seen = false;
        for (const Vec3 target : {center, Vec3{center.x, box.high.y - 0.01F, center.z}, nearest}) {
            const auto hit = physics3d::raycast(look.world, cam_pos, target, exclude);
            if (!hit || look.thing_of(hit->entity) == t || length(hit->point - target) < 0.05F) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            continue;
        }
        json::Value item;
        item["name"] = look.name(t);
        item["path"] = look.id(t);
        item["class"] = look.class_of(t);
        item["center"] = kit(center);
        json::Value kit_size;
        kit_size["X"] = r2(size.z * kScale);
        kit_size["Y"] = r2(size.x * kScale);
        kit_size["Z"] = r2(size.y * kScale);
        item["size"] = std::move(kit_size);
        item["yaw"] = look.is3d(t) ? json::Value(r2(yaw_of(look.global3(t)))) : json::Value(0.0);
        item["distance"] = r2(dist * kScale);
        item["top_above_feet"] = r2((box.high.y - feet_y) * kScale);
        item["bottom_above_feet"] = r2((box.low.y - feet_y) * kScale);
        item["crosshair_on"] = t == crosshair;
        if (const auto s = frustum.project(center)) {
            item["screen_x"] = r2(s->x);
            item["screen_y"] = r2(s->y);
        }
        if (!center_in) {
            item["partly_in_view"] = true;
        }
        if (const float speed = length(look.velocity3(t)); speed > 0.01F) {
            item["speed"] = r2(speed * kScale);
        }
        if (json::Value seen_look = look_of(look, t); seen_look.size() > 0) {
            item["look"] = std::move(seen_look);
        }
        out.push_back(std::move(item));
    }
    std::ranges::stable_sort(out, [](const json::Value& a, const json::Value& b) {
        return a.find("distance")->as_double() < b.find("distance")->as_double();
    });
    if (out.size() > max_count) {
        out.resize(max_count);
    }
    json::Value result;
    result["visible"] = std::move(out);
    json::Value cam_json;
    cam_json["location"] = kit(cam_pos);
    cam_json["yaw"] = r2(view_yaw);
    cam_json["pitch"] = r2(pitch_of(frustum.eye));
    cam_json["fov"] = r2(hfov);
    cam_json["vfov"] = r2(vfov);
    result["camera"] = std::move(cam_json);
    result["crosshair_tested"] = true;
    result["oriented"] = true;
    result["effects_play"] = false;
    result["body"] = std::move(body);
    result["omitted"] = kOmitted;
    if (const json::Value* only_arg = args.find("only"); only_arg != nullptr) {
        result["only"] = *only_arg;
    }
    return Answer::success(std::move(result));
}

Answer view2d(Look& look, const json::Value& args) {
    const entt::entity p = look.player();
    const auto max_count =
        static_cast<std::size_t>(std::max(0.0, arg_number(args, "max_count", 24.0)));
    const auto max_d = static_cast<float>(arg_number(args, "max_distance", 5000.0));
    const std::set<std::string> only = only_set(args);
    // What the screen shows of the world.
    Vec2 screen = ui::settings(look.world).viewport;
    if (screen.x <= 0.0F || screen.y <= 0.0F) {
        screen = {1280.0F, 720.0F};
    }
    const world::CameraView view = world::camera_view(look.world, look.tree2, screen);
    const world::Affine2D to_world = view.canvas.inverse();
    Rect2 world_rect;
    for (const Vec2 corner :
         {Vec2{0.0F, 0.0F}, Vec2{screen.x, 0.0F}, Vec2{0.0F, screen.y}, Vec2{screen.x, screen.y}}) {
        world_rect.add(to_world.apply(corner));
    }
    const Vec2 origin = p != entt::null && look.is2d(p)
                            ? Vec2{look.body2(p).center.x, look.body2(p).center.y}
                            : world_rect.center();
    std::vector<entt::entity> order;
    std::unordered_map<entt::entity, Rect2> rects;
    for (const entt::entity e : look.entities()) {
        if (!look.world.ecs().any_of<world::Sprite2D, world::AnimatedSprite2D>(e)) {
            continue;
        }
        const auto* sprite = look.world.ecs().try_get<world::Sprite2D>(e);
        if (sprite != nullptr && !sprite->visible) {
            continue;
        }
        if (p != entt::null && look.is_ancestor(p, e)) {
            continue;
        }
        const entt::entity t = look.thing_of(e);
        if (p != entt::null && look.is_ancestor(p, t)) {
            continue;
        }
        if (!only.empty() && !only.contains(look.name(t)) && !only.contains(look.id(t))) {
            continue;
        }
        if (!rects.contains(t)) {
            order.push_back(t);
            rects[t] = look.bounds2(t);
        }
        // A sprite with no shape is its own position at least.
        rects[t].add(look.position2(e));
    }
    json::Array out;
    for (const entt::entity t : order) {
        const Rect2& r = rects[t];
        const bool overlaps = r.high.x >= world_rect.low.x && r.low.x <= world_rect.high.x &&
                              r.high.y >= world_rect.low.y && r.low.y <= world_rect.high.y;
        if (!overlaps) {
            continue;
        }
        const Vec2 c = r.center();
        const float d = length(c - origin);
        if (d > max_d) {
            continue;
        }
        const Vec2 sp = view.canvas.apply(c);
        json::Value item;
        item["name"] = look.name(t);
        item["path"] = look.id(t);
        item["class"] = look.class_of(t);
        item["center"] = kit2d(c);
        json::Value size;
        size["X"] = r2(r.size().x);
        size["Y"] = 0.0;
        size["Z"] = r2(r.size().y);
        item["size"] = std::move(size);
        item["distance"] = r2(d);
        item["screen_x"] = r2((sp.x / screen.x * 2.0) - 1.0);
        item["screen_y"] = r2(1.0 - (sp.y / screen.y * 2.0));
        const bool enclosed = r.low.x >= world_rect.low.x && r.high.x <= world_rect.high.x &&
                              r.low.y >= world_rect.low.y && r.high.y <= world_rect.high.y;
        if (!enclosed) {
            item["partly_in_view"] = true;
        }
        if (const float speed = length(look.velocity2(t)); speed > 0.01F) {
            item["speed"] = r2(speed);
        }
        out.push_back(std::move(item));
    }
    std::ranges::stable_sort(out, [](const json::Value& a, const json::Value& b) {
        return a.find("distance")->as_double() < b.find("distance")->as_double();
    });
    if (out.size() > max_count) {
        out.resize(max_count);
    }
    json::Value body = json::Object{};
    if (p != entt::null && look.is2d(p)) {
        body["velocity"] = kit2d(look.velocity2(p));
        body["crouched"] = false;
        if (const auto* character = look.world.ecs().try_get<world::CharacterBody2D>(p)) {
            body["on_ground"] = character->on_floor;
            body["falling"] = !character->on_floor;
        }
    }
    json::Value result;
    result["visible"] = std::move(out);
    json::Value cam;
    cam["location"] = kit2d(world_rect.center());
    cam["yaw"] = 0.0;
    cam["pitch"] = 0.0;
    cam["fov"] = nullptr;
    result["camera"] = std::move(cam);
    result["effects_play"] = false;
    result["body"] = std::move(body);
    result["omitted"] = kOmitted;
    if (const json::Value* only_arg = args.find("only"); only_arg != nullptr) {
        result["only"] = *only_arg;
    }
    return Answer::success(std::move(result));
}

} // namespace

Answer view(Look& look, const json::Value& args) {
    const std::string dimension = look.dimension();
    if (dimension == "3d") {
        return view3d(look, args);
    }
    if (dimension == "2d") {
        return view2d(look, args);
    }
    json::Value result;
    result["visible"] = json::Array{};
    result["camera"] = nullptr;
    result["body"] = nullptr;
    result["effects_play"] = false;
    result["omitted"] = kOmitted;
    return Answer::success(std::move(result));
}

// ---- watch ----

namespace {

struct Sample {
    Quat rotation;
    Vec3 position;
    Vec3 scale;
};

Quat quat_of(const world::Affine3D& rotation) {
    return world::quaternion(world::euler(rotation));
}

Vec3 scale_of(const world::Affine3D& t) {
    return {length(t.x), length(t.y), length(t.z)};
}

std::optional<Sample> sample(const Look& look, const Watch::Entry& entry) {
    if (!look.world.valid(entry.entity)) {
        return std::nullopt;
    }
    if (const auto* t3 = look.world.ecs().try_get<world::Transform3D>(entry.entity)) {
        world::Affine3D local = world::affine(*t3);
        if (entry.thing != entt::null) {
            if (!look.world.valid(entry.thing)) {
                return std::nullopt;
            }
            local = look.global3(entry.thing).inverse() * look.global3(entry.entity);
        }
        return Sample{quat_of(rotation_only(local)), local.origin * kScale, scale_of(local)};
    }
    if (const auto* t2 = look.world.ecs().try_get<world::Transform2D>(entry.entity)) {
        return Sample{world::quaternion({0.0F, 0.0F, -t2->rotation}),
                      {t2->position.x, t2->position.y, 0.0F},
                      {t2->scale.x, t2->scale.y, 1.0F}};
    }
    if (const auto* control = look.world.ecs().try_get<world::Control>(entry.entity)) {
        return Sample{world::quaternion({0.0F, 0.0F, -control->rotation}),
                      {control->offset_left, control->offset_top, 0.0F},
                      {control->scale.x, control->scale.y, 1.0F}};
    }
    return std::nullopt;
}

double angle_between(const Quat& a, const Quat& b) {
    const double d = std::abs((a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w));
    return 2.0 * std::acos(std::clamp(d, 0.0, 1.0)) * kDegrees;
}

bool found_already(const Watch::Entry& entry, std::string_view what) {
    return std::ranges::any_of(entry.found, [&](const auto& f) { return f.first == what; });
}

} // namespace

Watch watch_start(const Look& look, const json::Value& args) {
    Watch watch;
    watch.drawn = arg_flag(args, "drawn");
    watch.turned = arg_number(args, "turned", 1.0);
    watch.moved = arg_number(args, "moved", 2.0);
    watch.resized = arg_number(args, "resized", 0.02);
    const auto parts =
        static_cast<std::size_t>(std::clamp(arg_number(args, "parts", 3.0), 0.0, 16.0));
    const json::Value* objects = args.find("objects");
    if (objects == nullptr || !objects->is_array()) {
        return watch;
    }
    for (const json::Value& item : objects->as_array()) {
        if (!item.is_string()) {
            continue;
        }
        const std::string& path = item.as_string();
        const entt::entity e = look.resolve(path);
        if (e == entt::null ||
            !look.world.ecs().any_of<world::Transform3D, world::Transform2D, world::Control>(e)) {
            continue;
        }
        watch.entries.emplace_back();
        watch.entries.back().entity = e;
        watch.entries.back().path = path;
        if (watch.drawn && look.is3d(e)) {
            std::vector<std::pair<float, entt::entity>> meshes;
            for (const entt::entity m : look.subtree(e)) {
                if (m != e && look.world.ecs().all_of<world::MeshInstance3D>(m) &&
                    look.tree3.visible(m)) {
                    meshes.emplace_back(length(look.mesh_bounds(m).size()), m);
                }
            }
            std::ranges::stable_sort(
                meshes, [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < meshes.size() && i < parts; ++i) {
                Watch::Entry& part = watch.entries.emplace_back();
                part.entity = meshes[i].second;
                part.thing = e;
                part.path = path + "/" + look.name(meshes[i].second);
            }
            watch.looks.emplace_back();
            watch.looks.back().entity = e;
            watch.looks.back().path = path;
        }
    }
    return watch;
}

void watch_tick(const Look& look, Watch& watch, double t) {
    for (Watch::Entry& entry : watch.entries) {
        const auto s = sample(look, entry);
        if (!s) {
            continue;
        }
        if (!entry.sampled) {
            entry.sampled = true;
            entry.rotation = s->rotation;
            entry.position = s->position;
            entry.scale = s->scale;
            continue;
        }
        if (!found_already(entry, "turned") &&
            angle_between(entry.rotation, s->rotation) > watch.turned) {
            entry.found.emplace_back("turned", t);
        }
        if (!found_already(entry, "moved") && length(entry.position - s->position) > watch.moved) {
            entry.found.emplace_back("moved", t);
        }
        if (!found_already(entry, "changed size")) {
            const Vec3 base{std::max<real_t>(std::abs(entry.scale.x), 0.001F),
                            std::max<real_t>(std::abs(entry.scale.y), 0.001F),
                            std::max<real_t>(std::abs(entry.scale.z), 0.001F)};
            const Vec3 d = s->scale - entry.scale;
            const double rel =
                std::max({std::abs(d.x) / base.x, std::abs(d.y) / base.y, std::abs(d.z) / base.z});
            if (rel > watch.resized) {
                entry.found.emplace_back("changed size", t);
            }
        }
    }
    for (Watch::LookEntry& entry : watch.looks) {
        if (!look.world.valid(entry.entity)) {
            continue;
        }
        const json::Value now = look_of(look, entry.entity);
        std::vector<std::string> parts;
        if (const json::Value* list = now.find("parts")) {
            for (const json::Value& part : list->as_array()) {
                parts.push_back(part.as_string());
            }
        }
        const int lights = [&] {
            const json::Value* l = now.find("lights");
            return l != nullptr ? static_cast<int>(l->find("on")->as_int()) : 0;
        }();
        std::vector<std::string> animations;
        if (const json::Value* a = now.find("animation")) {
            animations.push_back(a->as_string());
        }
        const auto change = [&](const char* what, const std::string& asset) {
            json::Value item;
            item["t"] = t;
            item["what"] = what;
            item["asset"] = asset;
            entry.found.push_back(std::move(item));
        };
        if (entry.sampled) {
            for (const std::string& x : parts) {
                if (std::ranges::find(entry.parts, x) == entry.parts.end()) {
                    change("part shown", x);
                }
            }
            for (const std::string& x : entry.parts) {
                if (std::ranges::find(parts, x) == parts.end()) {
                    change("part hidden", x);
                }
            }
            if (lights != entry.lights) {
                change(lights > entry.lights ? "light on" : "light off", "");
            }
            for (const std::string& x : animations) {
                if (std::ranges::find(entry.animations, x) == entry.animations.end()) {
                    change("animation started", x);
                }
            }
            for (const std::string& x : entry.animations) {
                if (std::ranges::find(animations, x) == animations.end()) {
                    change("animation ended", x);
                }
            }
        }
        entry.sampled = true;
        entry.parts = std::move(parts);
        entry.lights = lights;
        entry.animations = std::move(animations);
    }
}

json::Value watch_finish(const Watch& watch) {
    json::Array changes;
    std::set<std::string> seen;
    for (const Watch::Entry& entry : watch.entries) {
        for (const auto& [what, t] : entry.found) {
            if (!seen.insert(entry.path + "|" + what).second) {
                continue;
            }
            json::Value item;
            item["t"] = r2(t);
            item["object"] = entry.path;
            item["what"] = what;
            changes.push_back(std::move(item));
        }
    }
    for (const Watch::LookEntry& entry : watch.looks) {
        for (const json::Value& found : entry.found) {
            json::Value item;
            item["t"] = r2(found.find("t")->as_double());
            item["object"] = entry.path;
            item["what"] = *found.find("what");
            item["asset"] = *found.find("asset");
            changes.push_back(std::move(item));
        }
    }
    std::ranges::stable_sort(changes, [](const json::Value& a, const json::Value& b) {
        return a.find("t")->as_double() < b.find("t")->as_double();
    });
    json::Value out;
    out["changes"] = std::move(changes);
    if (watch.drawn) {
        out["drawn"] = true;
    }
    return out;
}

} // namespace slash::autopilot::detail
