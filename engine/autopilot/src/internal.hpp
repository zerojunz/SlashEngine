#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <cmath>
#include <entt/entity/entity.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slash::command {
class Session;
} // namespace slash::command

namespace slash::autopilot::detail {

// ---- The kit frame (the Godot addon's lib/frame.gd) ----
//
// 3D: Z up, left-handed, centimeters. SlashEngine's (x right, y up, -z forward, meters), Godot's,
// becomes kit (X, Y, Z) = 100 * (-z, x, y). Yaw is degrees about Z from +X toward +Y (turning
// right is positive), -deg(rotation.y) of the global basis; pitch is deg(rotation.x), positive
// up; roll -deg(rotation.z). 2D: (x right, y down) pixels become (x, 0, -y), 1 pixel a unit; a 2D
// rotation (clockwise on screen) is a pitch of -deg(rotation).

inline constexpr float kScale = 100.0F;
inline constexpr double kDegrees = 57.29577951308232;

// Rounded to hundredths, as the addon's snappedf(x, 0.01); infinities and NaN as they are.
[[nodiscard]] double r2(double x);
[[nodiscard]] json::Value kit(Vec3 v);
[[nodiscard]] json::Value kit_dir(Vec3 v);
[[nodiscard]] json::Value kit2d(Vec2 v);
// Half a size, as Unreal's extent.
[[nodiscard]] json::Value kit_extent(Vec3 size);
// A kit vector ({"X","Y","Z"}) as a direction in SlashEngine's axes, not scaled.
[[nodiscard]] Vec3 from_kit_dir(const json::Value* value);
[[nodiscard]] float yaw_of(const world::Affine3D& basis);
[[nodiscard]] float pitch_of(const world::Affine3D& basis);
[[nodiscard]] json::Value kit_rot(const world::Affine3D& basis);
[[nodiscard]] json::Value kit_rot2d(float rotation);
// The kit yaw of a direction, -180 to 180.
[[nodiscard]] float yaw_of_dir(Vec3 d);
[[nodiscard]] float wrap180(float degrees);

[[nodiscard]] inline float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}
[[nodiscard]] inline float length(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}
[[nodiscard]] inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
[[nodiscard]] inline Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l > 1e-6F ? v * (1.0F / l) : v;
}

// A box around points, as Godot's AABB.
struct Box3 {
    Vec3 low;
    Vec3 high;
    bool empty = true;

    void add(Vec3 p);
    void merge(const Box3& other);
    [[nodiscard]] Vec3 center() const { return (low + high) * 0.5F; }
    [[nodiscard]] Vec3 size() const { return high - low; }
    // The box around this one's corners placed by a transform.
    [[nodiscard]] Box3 placed(const world::Affine3D& transform) const;
};

struct Rect2 {
    Vec2 low;
    Vec2 high;
    bool empty = true;

    void add(Vec2 p);
    void merge(const Rect2& other);
    [[nodiscard]] Vec2 center() const { return (low + high) * 0.5F; }
    [[nodiscard]] Vec2 size() const { return high - low; }
};

// One look at the world, for one request: the trees, the player and the cameras as they are now.
class Look {
public:
    explicit Look(command::Session& session);

    command::Session& session;
    world::World& world;
    world::Tree3D tree3;
    world::Tree2D tree2;

    // The player, as the addon's world.gd finds it: the body the current camera hangs from
    // (CharacterBody, RigidBody, AnimatableBody or StaticBody), else an entity named "player"
    // (any case), else the first CharacterBody3D, else the first CharacterBody2D; null in a menu.
    [[nodiscard]] entt::entity player() const { return player_; }
    [[nodiscard]] entt::entity camera3d() const { return camera3d_; }
    [[nodiscard]] entt::entity camera2d() const { return camera2d_; }
    // "3d", "2d" or "ui".
    [[nodiscard]] std::string dimension() const;

    [[nodiscard]] bool is3d(entt::entity entity) const;
    [[nodiscard]] bool is2d(entt::entity entity) const;
    // Whether the entity is a physics body or area.
    [[nodiscard]] bool is_body(entt::entity entity) const;
    // The entity's parent in whichever tree places it.
    [[nodiscard]] entt::entity parent(entt::entity entity) const;
    // Whether ancestor is the entity or above it.
    [[nodiscard]] bool is_ancestor(entt::entity ancestor, entt::entity entity) const;
    // The thing an entity is part of, as the addon's thing_of: its nearest body or area (itself
    // included), else the nearest root of a scene instance above it, else itself.
    [[nodiscard]] entt::entity thing_of(entt::entity entity) const;
    // The entity's ID, or "" for one without.
    [[nodiscard]] std::string id(entt::entity entity) const;
    // Its last part, after the last '/': an instanced scene's entities are "<instance>/<id>".
    [[nodiscard]] std::string name(entt::entity entity) const;
    // The class an agent reads: its body, camera or drawing component, else its first one.
    [[nodiscard]] std::string class_of(entt::entity entity) const;
    // The entity an ID names, or null.
    [[nodiscard]] entt::entity resolve(std::string_view id) const;

    [[nodiscard]] world::Affine3D global3(entt::entity entity) const;
    [[nodiscard]] world::Affine2D global2(entt::entity entity) const;
    [[nodiscard]] Vec3 position3(entt::entity entity) const { return global3(entity).origin; }
    [[nodiscard]] Vec2 position2(entt::entity entity) const { return global2(entity).origin; }
    [[nodiscard]] Vec3 velocity3(entt::entity entity) const;
    [[nodiscard]] Vec2 velocity2(entt::entity entity) const;

    // The box around what an entity and those under it draw (meshes), else their collision
    // shapes, else its origin, in world space.
    [[nodiscard]] Box3 bounds3(entt::entity entity) const;
    // The box around one drawing entity's own mesh, in world space; empty for none.
    [[nodiscard]] Box3 mesh_bounds(entt::entity entity) const;
    // The rect around an entity's 2D collision shapes and sprites, else its origin.
    [[nodiscard]] Rect2 bounds2(entt::entity entity) const;

    // The middle of the player's body (its collision shape's center) and its half height.
    struct Body {
        Vec3 center;
        float half_height = 0.0F;
        float radius = 0.0F;
        bool has_shape = false;
        std::string shape;
    };
    [[nodiscard]] Body body3(entt::entity entity) const;
    [[nodiscard]] Body body2(entt::entity entity) const;

    // Entities in the order the trees list them, then the rest by ID.
    [[nodiscard]] const std::vector<entt::entity>& entities() const { return entities_; }
    // Entities under one, in tree order, it first.
    [[nodiscard]] std::vector<entt::entity> subtree(entt::entity root) const;

private:
    entt::entity player_ = entt::null;
    entt::entity camera3d_ = entt::null;
    entt::entity camera2d_ = entt::null;
    std::vector<entt::entity> entities_;
    std::unordered_map<entt::entity, std::vector<entt::entity>> children_;
    mutable std::unordered_map<entt::entity, Box3> mesh_boxes_;
};

// ---- Ops on the world (world.cpp) ----

// An op's answer: its result, or an error with an HTTP-style status.
struct Answer {
    bool ok = true;
    json::Value value;
    std::string error;
    int status = 400;

    [[nodiscard]] static Answer success(json::Value value) {
        Answer a;
        a.value = std::move(value);
        return a;
    }
    [[nodiscard]] static Answer failure(std::string error, int status = 400) {
        Answer a;
        a.ok = false;
        a.error = std::move(error);
        a.status = status;
        return a;
    }
};

[[nodiscard]] json::Value find(const Look& look, const json::Value& args);
[[nodiscard]] json::Value transforms(const Look& look, const json::Value& args);
[[nodiscard]] json::Value transform_of(const Look& look, entt::entity entity);
[[nodiscard]] Answer pose(const Look& look);
[[nodiscard]] Answer movement(const Look& look);
[[nodiscard]] Answer view(Look& look, const json::Value& args);

// A watch's state: each object's transform when it started and what changed since.
struct Watch {
    struct Entry {
        entt::entity entity = entt::null;
        entt::entity thing = entt::null; // for a drawn part: the thing it is placed in
        std::string path;
        bool sampled = false;
        Quat rotation{};
        Vec3 position{};
        Vec3 scale{};
        std::vector<std::pair<std::string, double>> found;
    };
    struct LookEntry {
        entt::entity entity = entt::null;
        std::string path;
        bool sampled = false;
        std::vector<std::string> parts;
        int lights = 0;
        std::vector<std::string> animations;
        json::Array found;
    };
    std::vector<Entry> entries;
    std::vector<LookEntry> looks;
    bool drawn = false;
    double turned = 1.0;
    double moved = 2.0;
    double resized = 0.02;
};
[[nodiscard]] Watch watch_start(const Look& look, const json::Value& args);
void watch_tick(const Look& look, Watch& watch, double t);
[[nodiscard]] json::Value watch_finish(const Watch& watch);

// ---- UI (ui.cpp) ----

// The visible texts of labels, buttons and text fields, keyed "<layer>.<id>" ("Screen" for
// controls in no CanvasLayer).
[[nodiscard]] json::Value screen(command::Session& session, const json::Value& args);
// The interactive controls on screen: buttons, check boxes, option buttons, sliders and text
// fields.
[[nodiscard]] json::Array ui_list(command::Session& session);
// The window pixel at a control's middle, or nullopt when it isn't laid out on screen.
[[nodiscard]] std::optional<Vec2> control_center(command::Session& session, entt::entity control);

// ---- rc (rc.cpp): components and commands, as Unreal's Remote Control ----

[[nodiscard]] Answer rc(command::Session& session, std::string_view verb, const json::Value* body);

// ---- The codex (codex.cpp) ----

// Where the codex's files are served from, the Godot addon's export folder: Autopilot's Godot
// adapter keeps what is under it at the codex's top, and only Godot's file types elsewhere.
inline constexpr std::string_view kCodexPrefix = ".godot/autopilot_codex/";
[[nodiscard]] json::Value codex_manifest(command::Session& session, std::string_view project);
[[nodiscard]] json::Value codex_read(command::Session& session, const json::Value& args);
// The project's summary for the codex: what SlashEngine is, the main scene, the input actions,
// the gameplay sources and each scene's entities.
[[nodiscard]] std::string codex_summary(command::Session& session);

} // namespace slash::autopilot::detail
