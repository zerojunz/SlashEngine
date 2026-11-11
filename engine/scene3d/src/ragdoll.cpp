#include "slash/scene3d/ragdoll.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/physical_bones.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physical_bones.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>

// Ragdoll3D: SlashEngine's own profiles and states, after Unreal's physics assets (PhAT's bodies
// fitted to bones), its Physical Animation component (muscles as drives toward the animated pose)
// and the usual hit reaction, fall and get-up loop of games built on them, made of Godot 4.6's
// PhysicalBoneSimulator3D and PhysicalBone3D.
namespace slash::scene3d {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float dot(Vec3 a, Vec3 b) {
    return static_cast<float>((a.x * b.x) + (a.y * b.y) + (a.z * b.z));
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const float size = length(v);
    return size > 0.0F ? v * static_cast<real_t>(1.0F / size) : Vec3{};
}

// The angle between two directions, in radians from 0 to pi.
float angle_between(Vec3 a, Vec3 b) {
    return slash::atan2(length(cross(a, b)), dot(a, b));
}

// An orthonormal basis whose y axis is up, with its x axis as near hint as can be.
world::Affine3D basis_along(Vec3 up, Vec3 hint) {
    world::Affine3D out;
    out.y = normalized(up);
    Vec3 x = hint - (out.y * static_cast<real_t>(dot(hint, out.y)));
    if (length(x) < 1.0e-4F) {
        x = cross(out.y,
                  std::abs(out.y.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F});
    }
    out.x = normalized(x);
    out.z = cross(out.x, out.y);
    return out;
}

// A transform without its scale or shear: its rotation and origin.
world::Affine3D unscaled(const world::Affine3D& transform) {
    world::Affine3D out =
        node_transform({.position = {}, .rotation = decompose(transform).rotation});
    out.origin = transform.origin;
    return out;
}

// What the ragdoll system keeps between ticks: the problems it has warned about, so each is
// logged once.
struct RagdollRuntime {
    std::map<entt::entity, std::string> warned;
};

// An entity ID made of a ragdoll's and a bone's: letters, digits, '_' and '-' only, at most 64.
std::string bone_id(std::string_view owner, std::string_view bone) {
    std::string id(owner);
    id += '_';
    for (const char c : bone) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        id += plain ? c : '_';
    }
    if (id.size() > 64) {
        id.resize(64);
    }
    return id;
}

Result<entt::entity> create_bone(world::World& world, std::string_view owner,
                                 std::string_view bone) {
    if (owner.empty()) {
        return world.create();
    }
    const std::string base = bone_id(owner, bone);
    for (std::size_t extra = 0;; ++extra) {
        std::string id = base;
        if (extra > 0) {
            const std::string suffix = "_" + std::to_string(extra);
            id.resize(std::min(id.size(), 64 - suffix.size()));
            id += suffix;
        }
        if (world.find(id) == entt::null) {
            return world.create(id);
        }
    }
}

// The body a profile bone makes: its PhysicalBone3D (keeping what physics writes) and shape.
world::PhysicalBone3D physical_bone(const world::PhysicalBone3D& old, const std::string& name,
                                    const world::RagdollBone& bone,
                                    const world::RagdollProfile& profile) {
    world::PhysicalBone3D out = old;
    out.bone_name = name;
    out.body_offset_position = bone.offset;
    out.body_offset_rotation = bone.rotation;
    out.joint_type = bone.joint;
    out.joint_rotation = bone.joint_rotation;
    out.swing_span = bone.swing_span;
    out.twist_span = bone.twist_span;
    out.angular_limit_enabled = bone.joint == "hinge";
    out.angular_limit_lower = bone.lower;
    out.angular_limit_upper = bone.upper;
    out.collision_layer = profile.collision_layer;
    out.collision_mask = profile.collision_mask;
    out.mass = bone.mass;
    out.physics_material_override = bone.physics_material;
    out.linear_damp = bone.linear_damp;
    out.angular_damp = bone.angular_damp;
    return out;
}

world::CollisionShape3D shape_of(const world::CollisionShape3D& old,
                                 const world::RagdollBone& bone) {
    world::CollisionShape3D out = old;
    out.shape = bone.shape == "circle" ? "sphere" : bone.shape;
    out.radius = bone.radius;
    out.height = std::max(bone.height, (2.0F * bone.radius) + 0.001F);
    out.size = bone.size;
    return out;
}

// The muscle a bone has in a state.
world::PhysicalBoneDrive drive_of(const world::Ragdoll3D& ragdoll, const std::string& name,
                                  const world::RagdollBone& bone,
                                  const world::RagdollProfile& profile) {
    const auto weak = ragdoll.weakness.find(name);
    const float left =
        std::clamp(1.0F - (weak != ragdoll.weakness.end() ? weak->second : 0.0F), 0.0F, 1.0F);
    const float scale = ragdoll.strength * left;
    world::PhysicalBoneDrive drive;
    const bool active = ragdoll.state == "active";
    if (name == profile.balance.bone) {
        drive.space = active ? "world" : "off";
        drive.orientation_strength = profile.balance.stiffness * scale;
        drive.angular_velocity_strength = profile.balance.damping * scale;
        drive.position_strength = profile.balance.position_stiffness * scale;
        drive.velocity_strength = profile.balance.position_damping * scale;
    } else {
        drive.space = active && bone.strength > 0.0F ? bone.drive : "off";
        drive.orientation_strength = bone.stiffness * bone.strength * scale;
        drive.angular_velocity_strength = bone.damping * bone.strength * scale;
        drive.position_strength = bone.pin * bone.strength * scale;
        drive.velocity_strength = bone.pin_damping * bone.strength * scale;
    }
    drive.max_angular_acceleration = bone.max_acceleration;
    return drive;
}

std::shared_ptr<const Model> model_of(const world::World& world, entt::entity entity,
                                      const ModelLoader& load) {
    const auto* instance = world.ecs().try_get<const world::MeshInstance3D>(entity);
    if (instance == nullptr || instance->mesh.empty() || !load) {
        return nullptr;
    }
    auto loaded = load(instance->mesh);
    return loaded ? loaded.value() : nullptr;
}

// Whether above is an ancestor of below in the model.
bool is_under(const Model& model, int below, int above) {
    for (int up = model.nodes[static_cast<std::size_t>(below)].parent; up >= 0;
         up = model.nodes[static_cast<std::size_t>(up)].parent) {
        if (up == above) {
            return true;
        }
    }
    return false;
}

// Turns a limp ragdoll toward getting up: moves its entity under where it lies, facing as its
// get-up animations start, starts the one for how it lies, and reports which.
std::string start_getting_up(world::World& world, entt::entity entity,
                             const world::RagdollProfile& profile,
                             const std::map<std::string, entt::entity>& bones) {
    const world::Tree3D tree(world);
    auto& ecs = world.ecs();
    // The balance bone, else the first the profile names.
    entt::entity root{entt::null};
    std::string root_name;
    if (const auto it = bones.find(profile.balance.bone); it != bones.end()) {
        root = it->second;
        root_name = it->first;
    } else if (!bones.empty()) {
        root = bones.begin()->second;
        root_name = bones.begin()->first;
    }
    if (root == entt::null) {
        return "up";
    }
    const world::Affine3D body = tree.global(root);
    const world::Affine3D bone =
        body * body_offset(ecs.get<const world::PhysicalBone3D>(root)).inverse();
    const Vec3 forward = normalized(bone.basis(profile.get_up.forward));
    const bool face_up = forward.y >= 0.0F;
    Vec3 spine = bone.basis({0.0F, 1.0F, 0.0F});
    spine.y = 0.0F;
    if (length(spine) < 1.0e-4F) {
        spine = forward;
        spine.y = 0.0F;
    }
    spine = normalized(spine);
    // The front of a model faces +z. One lying on its front gets up facing where its head
    // points; one on its back, the other way.
    const Vec3 front = face_up ? spine * static_cast<real_t>(-1.0F) : spine;
    const float yaw = slash::atan2(static_cast<float>(front.x), static_cast<float>(front.z));
    // The ground: the lowest of the bodies' bottoms.
    float ground = std::numeric_limits<float>::max();
    for (const auto& [name, part] : bones) {
        const world::Affine3D placed = tree.global(part);
        float reach = 0.0F;
        if (const auto* shape = ecs.try_get<const world::CollisionShape3D>(part);
            shape != nullptr) {
            reach = shape->shape == "box"
                        ? 0.5F * static_cast<float>(
                                     std::max({shape->size.x, shape->size.y, shape->size.z}))
                        : shape->radius;
        }
        ground = std::min(ground, static_cast<float>(placed.origin.y) - reach);
    }
    world::Transform3D standing;
    standing.position = {body.origin.x, static_cast<real_t>(ground), body.origin.z};
    standing.rotation = {0.0F, static_cast<real_t>(yaw), 0.0F};
    set_global_transform(world, tree, entity, world::affine(standing));
    const std::string& animation = face_up ? profile.get_up.face_up : profile.get_up.face_down;
    if (auto* player = ecs.try_get<world::AnimationPlayer>(entity);
        player != nullptr && !animation.empty()) {
        player->animation = animation;
        player->time = 0.0;
        player->playing = true;
        player->backwards = false;
    }
    return face_up ? "up" : "down";
}

// One ragdoll's tick.
void run_ragdoll(world::World& world, entt::entity entity, const ModelLoader& load,
                 RagdollRuntime& runtime, float seconds) {
    auto profile = ragdoll_profile(world, entity);
    if (!profile) {
        std::string& warned = runtime.warned[entity];
        if (warned != profile.error().message) {
            warned = profile.error().message;
            log::warn("ragdoll profile can't be used",
                      {{"id", std::string(world.id_of(entity))}, {"problem", warned}});
        }
        return;
    }
    runtime.warned.erase(entity);
    const std::shared_ptr<const Model> model = model_of(world, entity, load);
    if (model == nullptr || model->nodes.empty()) {
        return;
    }
    auto& ecs = world.ecs();
    if (!ecs.all_of<world::Transform3D>(entity)) {
        ecs.emplace<world::Transform3D>(entity);
    }

    // A body for each bone the profile names and the model has, made where its bone is at rest.
    std::map<std::string, entt::entity> bones = ragdoll_bones(world, entity);
    const std::string owner(world.id_of(entity));
    std::vector<world::Affine3D> rest;
    for (const auto& [name, bone] : profile.value().bones) {
        const int node = find_node(*model, name);
        if (node < 0 || bones.contains(name)) {
            continue;
        }
        if (rest.empty()) {
            rest = node_globals(*model, rest_pose(*model));
        }
        auto made = create_bone(world, owner, name);
        if (!made) {
            continue;
        }
        const entt::entity child = made.value();
        world::PhysicalBone3D physical = physical_bone({}, name, bone, profile.value());
        const world::Affine3D placed =
            unscaled(rest[static_cast<std::size_t>(node)] * body_offset(physical));
        world::Transform3D transform;
        transform.position = placed.origin;
        transform.rotation = world::euler(placed);
        ecs.emplace<world::Transform3D>(child, transform);
        ecs.emplace<world::PhysicalBone3D>(child, std::move(physical));
        ecs.emplace<world::CollisionShape3D>(child, shape_of({}, bone));
        ecs.get<world::Transform3D>(entity).children.push_back(child);
        bones.emplace(name, child);
    }

    world::Ragdoll3D ragdoll = ecs.get<const world::Ragdoll3D>(entity);
    const world::RagdollProfile& p = profile.value();

    // Muscles get their strength back, and a hit reaction ends once its time is up.
    for (auto it = ragdoll.weakness.begin(); it != ragdoll.weakness.end();) {
        it->second -= seconds / std::max(p.hit.recovery, 1.0e-3F);
        it = it->second <= 0.0F ? ragdoll.weakness.erase(it) : std::next(it);
    }
    if (ragdoll.reaction > 0.0F) {
        ragdoll.reaction = std::max(ragdoll.reaction - seconds, 0.0F);
        if (ragdoll.reaction == 0.0F && ragdoll.state == "active") {
            ragdoll.state = ragdoll.rest_state;
        }
    }

    // An active ragdoll falls when its balance bone leans too far from the pose.
    const world::PhysicalBoneTargets* targets = world::physical_bone_targets(world);
    if (ragdoll.state == "active" && p.balance.max_tilt > 0.0F && targets != nullptr) {
        if (const auto it = bones.find(p.balance.bone); it != bones.end()) {
            if (const auto target = targets->find(it->second); target != targets->end()) {
                const world::Tree3D tree(world);
                const float tilt = angle_between(normalized(tree.global(it->second).y),
                                                 normalized(target->second.y));
                if (tilt > p.balance.max_tilt) {
                    ragdoll.state = "limp";
                    ragdoll.reaction = 0.0F;
                    ragdoll.still = 0.0F;
                    world.emit("ragdoll_fell", owner, json::Object{{"reason", "balance"}});
                }
            }
        }
    }

    // A limp ragdoll that has lain still long enough gets up.
    if (ragdoll.state == "limp" && p.get_up.enabled && !bones.empty()) {
        float fastest = 0.0F;
        for (const auto& [name, part] : bones) {
            fastest = std::max(fastest,
                               length(ecs.get<const world::PhysicalBone3D>(part).linear_velocity));
        }
        ragdoll.still = fastest < p.get_up.settle_speed ? ragdoll.still + seconds : 0.0F;
        if (ragdoll.still >= p.get_up.settle_time) {
            ecs.replace<world::Ragdoll3D>(entity, ragdoll);
            const std::string face = start_getting_up(world, entity, p, bones);
            ragdoll = ecs.get<const world::Ragdoll3D>(entity);
            ragdoll.state = "getting_up";
            ragdoll.rising = 0.0F;
            ragdoll.still = 0.0F;
            world.emit("ragdoll_getting_up", owner, json::Object{{"face", face}});
        }
    } else if (ragdoll.state != "limp") {
        ragdoll.still = 0.0F;
    }
    float influence = 1.0F;
    if (ragdoll.state == "getting_up") {
        ragdoll.rising += seconds;
        influence = std::max(0.0F, 1.0F - (ragdoll.rising / p.get_up.blend));
        if (ragdoll.rising >= p.get_up.blend) {
            ragdoll.state = ragdoll.rest_state;
            ragdoll.rising = 0.0F;
            influence = 1.0F;
            world.emit("ragdoll_got_up", owner);
        }
    } else {
        ragdoll.rising = 0.0F;
    }
    if (!(ecs.get<const world::Ragdoll3D>(entity) == ragdoll)) {
        ecs.replace<world::Ragdoll3D>(entity, ragdoll);
    }

    // The simulator and each body, as the state and profile say; changed only when they differ.
    const bool simulating = ragdoll.state != "animated";
    std::vector<entt::entity> exceptions;
    if (!p.self_collision) {
        for (const auto& [name, part] : bones) {
            exceptions.push_back(part);
        }
    }
    auto& simulator = ecs.get_or_emplace<world::PhysicalBoneSimulator3D>(entity);
    if (simulator.simulating != simulating || simulator.influence != influence ||
        !simulator.active || !simulator.simulated_bones.empty() ||
        simulator.collision_exceptions != exceptions) {
        simulator.simulating = simulating;
        simulator.influence = influence;
        simulator.active = true;
        simulator.simulated_bones.clear();
        simulator.collision_exceptions = std::move(exceptions);
    }
    for (const auto& [name, part] : bones) {
        const auto found = p.bones.find(name);
        if (found == p.bones.end()) {
            continue;
        }
        const auto& old = ecs.get<const world::PhysicalBone3D>(part);
        world::PhysicalBone3D next = physical_bone(old, name, found->second, p);
        next.drive = drive_of(ragdoll, name, found->second, p);
        if (!(next == old)) {
            ecs.replace<world::PhysicalBone3D>(part, std::move(next));
        }
        const auto* shape = ecs.try_get<const world::CollisionShape3D>(part);
        const world::CollisionShape3D wanted =
            shape_of(shape != nullptr ? *shape : world::CollisionShape3D{}, found->second);
        if (shape == nullptr || !(*shape == wanted)) {
            ecs.emplace_or_replace<world::CollisionShape3D>(part, wanted);
        }
    }
}

// The model space positions of the vertices each node moves most, from a skinned model at rest.
std::vector<std::vector<Vec3>> vertices_by_node(const Model& model,
                                                const std::vector<world::Affine3D>& globals) {
    std::vector<std::vector<Vec3>> out(model.nodes.size());
    for (const ModelPart& part : model.parts) {
        if (part.skin < 0 || static_cast<std::size_t>(part.skin) >= model.skins.size()) {
            continue;
        }
        const ModelSkin& skin = model.skins[static_cast<std::size_t>(part.skin)];
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            for (std::size_t v = 0; v < surface.weights.size() && v < surface.vertices.size();
                 ++v) {
                const VertexWeights& w = surface.weights[v];
                const auto most = std::ranges::max_element(w.weights) - w.weights.begin();
                const std::size_t joint = w.joints[static_cast<std::size_t>(most)];
                if (joint >= skin.joints.size() || joint >= skin.inverse_binds.size()) {
                    continue;
                }
                const std::size_t node = skin.joints[joint];
                const auto& at = surface.vertices[v].position;
                const Vec3 local{at[0], at[1], at[2]};
                out[node].push_back((globals[node] * skin.inverse_binds[joint]).apply(local));
            }
        }
    }
    return out;
}

// The distance from p to the segment from a to b.
float segment_distance(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float span = dot(ab, ab);
    const float t = span > 0.0F ? std::clamp(dot(p - a, ab) / span, 0.0F, 1.0F) : 0.0F;
    return length(p - (a + (ab * static_cast<real_t>(t))));
}

// A humanoid joint's limits, by the profile bone it is.
struct Limits {
    std::string_view joint = "cone";
    float swing = 0.5F;
    float twist = 0.3F;
    float upper = 0.0F; // hinge
    bool knee = false;
};

Limits humanoid_limits(std::string_view bone) {
    const auto ends = [&](std::string_view tail) { return bone.ends_with(tail); };
    if (bone == "Hips") {
        return {.joint = "none"};
    }
    if (bone == "Head") {
        return {.swing = 0.6F, .twist = 0.6F};
    }
    if (ends("UpperArm")) {
        return {.swing = 1.4F, .twist = 0.8F};
    }
    if (ends("LowerArm")) {
        return {.joint = "hinge", .upper = 2.5F};
    }
    if (ends("Hand")) {
        return {.swing = 0.7F, .twist = 0.3F};
    }
    if (ends("UpperLeg")) {
        return {.swing = 1.2F, .twist = 0.4F};
    }
    if (ends("LowerLeg")) {
        return {.joint = "hinge", .upper = 2.6F, .knee = true};
    }
    if (ends("Foot")) {
        return {.swing = 0.5F, .twist = 0.2F};
    }
    return {.swing = 0.35F, .twist = 0.3F}; // the spine
}

// Rounded to a millimeter (or a thousandth), so generated profiles read well.
double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

json::Value vec(Vec3 v) {
    return json::Array{rounded(v.x), rounded(v.y), rounded(v.z)};
}

} // namespace

Result<world::RagdollProfile> ragdoll_profile(const world::World& world, entt::entity entity) {
    const auto* ragdoll = world.ecs().try_get<const world::Ragdoll3D>(entity);
    if (ragdoll == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "has no Ragdoll3D");
    }
    if (ragdoll->profile.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "names no profile: set profile to a .ragdoll.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          "the world has no assets to load profiles with");
    }
    auto file = assets->load<json::Value>(ragdoll->profile);
    if (!file) {
        return make_error(file.error().code,
                          "\"" + ragdoll->profile + "\" can't be read: " + file.error().message);
    }
    auto parsed = world::parse_ragdoll(*file.value(), ragdoll->overrides);
    if (!parsed) {
        return make_error(parsed.error().code,
                          "\"" + ragdoll->profile + "\" with overrides: " + parsed.error().message);
    }
    return parsed;
}

std::map<std::string, entt::entity> ragdoll_bones(const world::World& world, entt::entity entity) {
    std::map<std::string, entt::entity> out;
    const auto& ecs = world.ecs();
    const auto* transform = ecs.try_get<const world::Transform3D>(entity);
    if (transform == nullptr) {
        return out;
    }
    for (const entt::entity child : transform->children) {
        if (child != entt::null && world.valid(child)) {
            if (const auto* bone = ecs.try_get<const world::PhysicalBone3D>(child);
                bone != nullptr) {
                out.emplace(bone->bone_name, child);
            }
        }
    }
    return out;
}

void add_ragdoll_system(world::World& world, ModelLoader load) {
    if (world.ecs().ctx().contains<std::shared_ptr<RagdollRuntime>>()) {
        return;
    }
    auto runtime = std::make_shared<RagdollRuntime>();
    world.ecs().ctx().emplace<std::shared_ptr<RagdollRuntime>>(runtime);
    world.add_system(
        "ragdoll",
        [runtime, load = std::move(load)](world::World& w, double tick_seconds) {
            std::vector<entt::entity> ragdolls;
            for (const entt::entity entity : w.ecs().view<const world::Ragdoll3D>()) {
                ragdolls.push_back(entity);
            }
            // In the order they were made, the same in every session.
            std::ranges::sort(ragdolls, {}, [](entt::entity e) { return entt::to_entity(e); });
            for (const entt::entity entity : ragdolls) {
                if (w.valid(entity) && w.ecs().all_of<world::Ragdoll3D>(entity)) {
                    run_ragdoll(w, entity, load, *runtime, static_cast<float>(tick_seconds));
                }
            }
            std::erase_if(runtime->warned, [&](const auto& e) { return !w.valid(e.first); });
        },
        {.doc = "Keeps each Ragdoll3D's bodies, joints and muscles as its profile says and runs "
                "its states: hit reactions, falls and getting back up.",
         .priority = 915});
}

Result<RagdollHitReport> hit_ragdoll(world::World& world, entt::entity entity,
                                     std::string_view bone, Vec3 impulse) {
    auto profile = ragdoll_profile(world, entity);
    if (!profile) {
        return profile.error();
    }
    const world::RagdollProfile& p = profile.value();
    const std::map<std::string, entt::entity> bones = ragdoll_bones(world, entity);
    const auto part = bones.find(std::string(bone));
    if (part == bones.end()) {
        std::vector<std::string_view> names;
        names.reserve(bones.size());
        for (const auto& [name, each] : bones) {
            names.emplace_back(name);
        }
        std::string message = "the ragdoll has no body for bone \"" + std::string(bone) + "\"";
        if (const std::string_view hint = closest_match(bone, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    auto& ecs = world.ecs();
    auto& physical = ecs.get<world::PhysicalBone3D>(part->second);
    physical.linear_velocity =
        physical.linear_velocity + (impulse * static_cast<real_t>(1.0F / physical.mass));
    const float size = length(impulse);

    auto ragdoll = ecs.get<const world::Ragdoll3D>(entity);
    // The hit bone weakens, and the bones under it a share less at each step down.
    const auto weaken = [&](const std::string& name, float amount) {
        float& weak = ragdoll.weakness[name];
        weak = std::min(1.0F, weak + amount);
    };
    weaken(std::string(bone), size * p.hit.weaken);
    asset::Assets* assets = asset::assets_of(ecs);
    const auto* instance = ecs.try_get<const world::MeshInstance3D>(entity);
    if (assets != nullptr && instance != nullptr && p.hit.spread > 0.0F) {
        if (auto loaded = assets->load<Model>(instance->mesh); loaded) {
            const Model& model = *loaded.value();
            const int hit = find_node(model, bone);
            for (const auto& [name, each] : bones) {
                const int node = find_node(model, name);
                if (hit < 0 || node < 0 || node == hit || !is_under(model, node, hit)) {
                    continue;
                }
                // A share less for each body between them.
                float share = 1.0F;
                for (int up = node; up >= 0 && up != hit;
                     up = model.nodes[static_cast<std::size_t>(up)].parent) {
                    if (bones.contains(model.nodes[static_cast<std::size_t>(up)].name)) {
                        share *= p.hit.spread;
                    }
                }
                weaken(name, size * p.hit.weaken * share);
            }
        }
    }
    const std::string owner(world.id_of(entity));
    RagdollHitReport report;
    if (p.hit.knockdown > 0.0F && size >= p.hit.knockdown) {
        report.knocked_down = ragdoll.state != "limp";
        ragdoll.state = "limp";
        ragdoll.reaction = 0.0F;
        ragdoll.still = 0.0F;
    } else if (ragdoll.state == "animated" || ragdoll.reaction > 0.0F) {
        // An animated ragdoll reacts with its muscles until they have their strength back.
        ragdoll.state = "active";
        ragdoll.reaction = p.hit.recovery;
    }
    report.state = ragdoll.state;
    report.weakness = ragdoll.weakness;
    ecs.replace<world::Ragdoll3D>(entity, std::move(ragdoll));
    world.emit("ragdoll_hit", owner,
               json::Object{{"bone", std::string(bone)}, {"impulse", rounded(size)}});
    if (report.knocked_down) {
        world.emit("ragdoll_fell", owner, json::Object{{"reason", "hit"}});
    }
    return report;
}

Result<json::Value> generate_ragdoll(const Model& model, const RagdollOptions& options) {
    if (model.nodes.empty() || model.skins.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the model has no skeleton: a ragdoll needs a skinned model");
    }
    const std::vector<world::Affine3D> globals = node_globals(model, rest_pose(model));
    const std::vector<std::vector<Vec3>> vertices = vertices_by_node(model, globals);

    // The bones that get bodies: a humanoid's main ones, by its bone map, or every joint.
    const BoneMap map = auto_map_bones(model);
    const bool humanoid = map.contains("Hips");
    std::vector<int> chosen;
    std::map<int, std::string, std::less<>> kinds; // humanoid: the profile bone each is
    if (humanoid) {
        static constexpr std::array kMain{
            "Hips",          "Spine",        "Chest",        "UpperChest",    "Head",
            "LeftUpperArm",  "LeftLowerArm", "LeftHand",     "RightUpperArm", "RightLowerArm",
            "RightHand",     "LeftUpperLeg", "LeftLowerLeg", "LeftFoot",      "RightUpperLeg",
            "RightLowerLeg", "RightFoot"};
        const bool upper_chest = map.contains("UpperChest");
        for (const char* name : kMain) {
            // The chest is one body: the upper chest's where a model has both.
            if (std::string_view(name) == "Chest" && upper_chest) {
                continue;
            }
            if (const auto it = map.find(name); it != map.end()) {
                if (const int node = find_node(model, it->second); node >= 0) {
                    chosen.push_back(node);
                    kinds.emplace(node, name);
                }
            }
        }
    } else {
        std::set<std::size_t> joints;
        for (const ModelSkin& skin : model.skins) {
            joints.insert(skin.joints.begin(), skin.joints.end());
        }
        for (const std::size_t joint : joints) {
            chosen.push_back(static_cast<int>(joint));
        }
    }
    const auto chosen_has = [&](int node) {
        return std::ranges::find(chosen, node) != chosen.end();
    };

    // The skeleton's height, for sizes of last resort and what counts as too short.
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const int node : chosen) {
        low = std::min(low, static_cast<float>(globals[static_cast<std::size_t>(node)].origin.y));
        high = std::max(high, static_cast<float>(globals[static_cast<std::size_t>(node)].origin.y));
    }
    const float height = std::max(high - low, 0.1F);
    const float shortest = (options.min_length > 0.0F ? options.min_length : 0.04F) * height;

    // The model's front, for elbows and knees: +z, as glTF's.
    const Vec3 front{0.0F, 0.0F, 1.0F};

    struct Made {
        std::string name;
        json::Value bone;
        float volume = 0.0F;
    };
    std::vector<Made> made;
    for (const int node : chosen) {
        const auto index = static_cast<std::size_t>(node);
        const world::Affine3D& global = globals[index];
        const world::Affine3D turn = unscaled(global);
        const Vec3 start = global.origin;
        // Where the body reaches: the bodies below it, else the bone's own children.
        std::vector<Vec3> below;
        for (const int other : chosen) {
            if (other == node || !is_under(model, other, node)) {
                continue;
            }
            bool nearest = true;
            for (int up = model.nodes[static_cast<std::size_t>(other)].parent;
                 up >= 0 && up != node; up = model.nodes[static_cast<std::size_t>(up)].parent) {
                nearest = nearest && !chosen_has(up);
            }
            if (nearest) {
                below.push_back(globals[static_cast<std::size_t>(other)].origin);
            }
        }
        bool sphere = below.size() > 1;
        if (below.empty()) {
            for (std::size_t child = 0; child < model.nodes.size(); ++child) {
                if (model.nodes[child].parent == node) {
                    below.push_back(globals[child].origin);
                }
            }
        }
        Vec3 end = start;
        for (const Vec3 each : below) {
            end = end + (each * static_cast<real_t>(1.0F / static_cast<float>(below.size())));
        }
        if (!below.empty()) {
            end = end - (start * static_cast<real_t>(1.0F / static_cast<float>(below.size())));
        }
        const float span = length(end - start);
        if (below.empty() || span < 1.0e-5F) {
            sphere = true;
        }
        if (!humanoid && !sphere && span < shortest) {
            continue;
        }
        // Fitted to the vertices it moves most, else sized by its span.
        const std::vector<Vec3>& own = vertices[index];
        Vec3 center = sphere && !below.empty() ? (start + end) * static_cast<real_t>(0.5F) : start;
        float radius = 0.0F;
        if (!own.empty()) {
            double sum = 0.0;
            for (const Vec3 p : own) {
                sum += sphere ? length(p - center) : segment_distance(p, start, end);
            }
            radius = static_cast<float>(sum / static_cast<double>(own.size()));
        }
        if (radius <= 0.0F) {
            radius = sphere ? std::max(0.25F * std::max(span, 0.2F * height), 0.02F * height)
                            : 0.15F * span;
        }
        radius = std::max(radius, 0.01F * height);
        if (!sphere) {
            radius = std::min(radius, 0.45F * span);
        }

        json::Value bone;
        const world::Affine3D to_bone = global.inverse();
        float volume = 0.0F;
        if (sphere) {
            if (below.empty()) {
                center = start;
            }
            bone["shape"] = "sphere";
            bone["radius"] = rounded(radius);
            bone["offset"] = vec(to_bone.apply(center));
            volume = 4.0F / 3.0F * kPi * radius * radius * radius;
        } else {
            const Vec3 middle = (start + end) * static_cast<real_t>(0.5F);
            const world::Affine3D along = basis_along(end - start, turn.x);
            // The body's turn in the bone's space: the bone's rotation undone.
            const world::Affine3D local = turn.inverse() * along;
            bone["shape"] = "capsule";
            bone["radius"] = rounded(radius);
            bone["height"] = rounded(span + (2.0F * radius));
            bone["offset"] = vec(to_bone.apply(middle));
            bone["rotation"] = vec(world::euler(
                world::Affine3D{.x = local.x, .y = local.y, .z = local.z, .origin = {}}));
            volume =
                (kPi * radius * radius * span) + (4.0F / 3.0F * kPi * radius * radius * radius);
        }
        const auto kind = kinds.find(node);
        const Limits limits =
            humanoid && kind != kinds.end() ? humanoid_limits(kind->second) : Limits{};
        if (model.nodes[index].parent < 0 || (humanoid && limits.joint == "none")) {
            bone["joint"] = "none";
        } else if (limits.joint == "hinge") {
            // Elbows bend toward the front and knees toward the back, about the axis across the
            // limb: the hinge turns about its joint's z axis, in the body's space.
            const Vec3 dir = normalized(end - start);
            const Vec3 axis =
                normalized(cross(dir, limits.knee ? front * static_cast<real_t>(-1.0F) : front));
            const world::Affine3D body_turn = sphere ? turn : basis_along(end - start, turn.x);
            const Vec3 axis_in_body = body_turn.inverse().basis(axis);
            world::Affine3D joint;
            joint.z = normalized(axis_in_body);
            joint.y = normalized(cross(joint.z, Vec3{1.0F, 0.0F, 0.0F}));
            if (length(joint.y) < 1.0e-3F) {
                joint.y = normalized(cross(joint.z, Vec3{0.0F, 1.0F, 0.0F}));
            }
            joint.x = cross(joint.y, joint.z);
            bone["joint"] = "hinge";
            bone["joint_rotation"] = vec(world::euler(joint));
            bone["lower"] = 0.0;
            bone["upper"] = rounded(limits.upper);
        } else {
            // A cone about the bone: the joint's x axis along the body's y.
            bone["joint"] = "cone";
            if (!sphere) {
                bone["joint_rotation"] = vec({0.0F, 0.0F, kPi / 2.0F});
            }
            bone["swing_span"] = rounded(limits.swing);
            bone["twist_span"] = rounded(limits.twist);
        }
        made.push_back(
            {.name = model.nodes[index].name, .bone = std::move(bone), .volume = volume});
    }
    if (made.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no bone of the model is long enough");
    }
    float total = 0.0F;
    for (const Made& each : made) {
        total += each.volume;
    }
    json::Value file;
    file["format"] = 1;
    json::Value defaults;
    defaults["drive"] = "local";
    defaults["stiffness"] = rounded(options.stiffness);
    defaults["damping"] = rounded(options.damping);
    defaults["pin"] = rounded(options.pin);
    defaults["pin_damping"] = rounded(options.pin_damping);
    file["defaults"] = std::move(defaults);
    json::Value bones = json::Object{};
    for (Made& each : made) {
        each.bone["mass"] =
            rounded(std::max(options.mass * (total > 0.0F ? each.volume / total : 0.0F), 0.01F));
        bones[each.name] = std::move(each.bone);
    }
    file["bones"] = std::move(bones);
    if (humanoid) {
        const std::string& hips = map.at("Hips");
        const int node = find_node(model, hips);
        json::Value balance;
        balance["bone"] = hips;
        balance["stiffness"] = rounded(options.stiffness);
        balance["damping"] = rounded(options.damping);
        balance["position_stiffness"] = rounded(options.pin);
        balance["position_damping"] = rounded(options.pin_damping);
        balance["max_tilt"] = 0.6;
        file["balance"] = std::move(balance);
        json::Value get_up;
        get_up["forward"] = vec(
            normalized(unscaled(globals[static_cast<std::size_t>(node)]).inverse().basis(front)));
        file["get_up"] = std::move(get_up);
    }
    return file;
}

std::vector<RagdollProblem> check_ragdoll(const world::World& world, entt::entity entity,
                                          const Model* model) {
    std::vector<RagdollProblem> out;
    const auto* ragdoll = world.ecs().try_get<const world::Ragdoll3D>(entity);
    if (ragdoll == nullptr) {
        return out;
    }
    constexpr std::string_view kProfile = "/components/Ragdoll3D/profile";
    if (ragdoll->profile.empty()) {
        out.push_back({std::string(kProfile), "names no profile: set it to a .ragdoll.json file"});
        return out;
    }
    if (!world::is_ragdoll_path(ragdoll->profile)) {
        out.push_back(
            {std::string(kProfile), "\"" + ragdoll->profile + "\" is not a .ragdoll.json file"});
        return out;
    }
    const world::FileReader& reader = world.file_reader();
    if (!reader) {
        return out;
    }
    auto bytes = reader(ragdoll->profile);
    if (!bytes) {
        out.push_back({std::string(kProfile),
                       "\"" + ragdoll->profile + "\" could not be read: " + bytes.error().message});
        return out;
    }
    auto file = json::parse(bytes.value());
    if (!file) {
        out.push_back({std::string(kProfile),
                       "\"" + ragdoll->profile + "\" is not JSON: " + file.error().message});
        return out;
    }
    auto profile = world::parse_ragdoll(file.value(), ragdoll->overrides);
    if (!profile) {
        out.push_back({ragdoll->overrides.is_null() ? std::string(kProfile)
                                                    : "/components/Ragdoll3D/overrides",
                       "\"" + ragdoll->profile + "\": " + profile.error().message});
        return out;
    }
    if (model == nullptr) {
        out.push_back({"/components/Ragdoll3D",
                       "the entity's MeshInstance3D draws no model: set its mesh to a model "
                       "file with a skeleton"});
        return out;
    }
    std::vector<std::string_view> names;
    names.reserve(model->nodes.size());
    for (const ModelNode& node : model->nodes) {
        names.emplace_back(node.name);
    }
    for (const auto& [name, bone] : profile.value().bones) {
        if (find_node(*model, name) >= 0) {
            continue;
        }
        std::string message = "\"" + ragdoll->profile + "\" names bone \"" + name +
                              "\", which the model doesn't have";
        if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        out.push_back({std::string(kProfile), std::move(message)});
    }
    return out;
}

} // namespace slash::scene3d
