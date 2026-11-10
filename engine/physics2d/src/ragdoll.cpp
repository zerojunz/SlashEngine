#include "slash/physics2d/ragdoll.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Ragdoll2D: Ragdoll3D's profiles and states over a cut-out character's Transform2D tree, made of
// RigidBody2D bodies and PinJoint2D joints. Godot's 2D physical bones need a Skeleton2D, which
// cut-out characters made of sprites don't have, so the bones here are the tree's own entities.
// The muscles are the same implicit springs physics3d's drives are, set as the bodies'
// velocities before the step.
namespace slash::physics2d {

namespace {

using world::AnimationPlayer;
using world::CollisionShape2D;
using world::PinJoint2D;
using world::RigidBody2D;
using world::Sprite2D;
using world::Transform2D;

constexpr float kPi = std::numbers::pi_v<float>;

// An angle in -pi to pi.
float wrapped(float angle) {
    while (angle > kPi) {
        angle -= 2.0F * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0F * kPi;
    }
    return angle;
}

float length(Vec2 v) {
    return std::sqrt(static_cast<float>((v.x * v.x) + (v.y * v.y)));
}

// Rounded to a thousandth, so generated profiles read well.
double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

// The rotation of a matrix's x axis, clockwise from +x as Transform2D's.
float angle_of(const world::Affine2D& matrix) {
    return slash::atan2(static_cast<float>(matrix.x.y), static_cast<float>(matrix.x.x));
}

bool same_place(const Transform2D& a, const Transform2D& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale &&
           a.skew == b.skew;
}

// Where physics left a bone, and the pose the animation last gave it.
struct Bone {
    Transform2D left; // its own transform after the physics step
    bool has_left = false;
    Transform2D pose; // its own transform as the animation last set it
    bool has_pose = false;
    Vec2 lying; // where it lay when getting up began, in the world
    float lying_rotation = 0.0F;
};

struct Ragdoll {
    std::map<entt::entity, Bone> bones;
    std::string warned;
};

struct Runtime {
    std::map<entt::entity, Ragdoll> ragdolls;
};

// The name an entity under a ragdoll goes by in its profile: its ID, without the ragdoll's own
// "<id>/" when it is part of an instance.
std::string bone_name(const world::World& world, entt::entity ragdoll, entt::entity bone) {
    const std::string_view id = world.id_of(bone);
    const std::string prefix = std::string(world.id_of(ragdoll)) + "/";
    if (id.size() > prefix.size() && id.starts_with(prefix)) {
        return std::string(id.substr(prefix.size()));
    }
    return std::string(id);
}

// The entities under root in its Transform2D tree, parents first, in their children's order.
std::vector<entt::entity> descendants(const world::World& world, entt::entity root) {
    std::vector<entt::entity> out;
    const auto& ecs = world.ecs();
    const world::Tree2D tree(world);
    std::vector<entt::entity> open{root};
    std::set<entt::entity> seen{root};
    for (std::size_t i = 0; i < open.size(); ++i) {
        const auto* transform = ecs.try_get<const Transform2D>(open[i]);
        if (transform == nullptr) {
            continue;
        }
        for (const entt::entity child : transform->children) {
            if (child == entt::null || !world.valid(child) || tree.parent(child) != open[i] ||
                !seen.insert(child).second) {
                continue;
            }
            open.push_back(child);
            out.push_back(child);
        }
    }
    return out;
}

// The nearest bone above an entity, or null.
entt::entity bone_above(const world::Tree2D& tree, entt::entity bone, entt::entity ragdoll,
                        const std::set<entt::entity>& bones) {
    for (entt::entity up = tree.parent(bone); up != entt::null && up != ragdoll;
         up = tree.parent(up)) {
        if (bones.contains(up)) {
            return up;
        }
    }
    return entt::null;
}

// An entity ID made of a bone's: letters, digits, '_' and '-' only, at most 64, with a suffix.
std::string made_id(std::string_view bone, std::string_view suffix) {
    std::string id;
    for (const char c : bone) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        id += plain ? c : '_';
    }
    id.resize(std::min(id.size(), 64 - suffix.size()));
    return id + std::string(suffix);
}

// The bone's body as the profile says, keeping what physics writes.
RigidBody2D body_of(const RigidBody2D& old, const world::RagdollBone& bone,
                    const world::RagdollProfile& profile, bool frozen) {
    RigidBody2D out = old;
    out.collision_layer = profile.collision_layer;
    // Bones keep out of each other by leaving their own layer out of their masks; what is on
    // their layer with them in its mask still meets them, as either mask is enough.
    out.collision_mask = profile.self_collision ? profile.collision_mask
                                                : profile.collision_mask & ~profile.collision_layer;
    out.mass = bone.mass;
    out.physics_material_override = bone.physics_material;
    out.linear_damp = bone.linear_damp;
    out.angular_damp = bone.angular_damp;
    out.freeze = frozen;
    out.freeze_mode = "kinematic";
    out.can_sleep = !frozen;
    return out;
}

CollisionShape2D shape_of(const CollisionShape2D& old, const world::RagdollBone& bone) {
    CollisionShape2D out = old;
    out.shape = bone.shape;
    if (bone.shape == "sphere") {
        out.shape = "circle";
    } else if (bone.shape == "box") {
        out.shape = "rectangle";
    }
    out.radius = bone.radius;
    out.height = std::max(bone.height, (2.0F * bone.radius) + 0.01F);
    out.size = {bone.size.x, bone.size.y};
    out.position = {bone.offset.x, bone.offset.y};
    out.rotation = static_cast<float>(bone.rotation.z);
    return out;
}

// How much of a bone's muscle a ragdoll has left.
float strength_of(const world::Ragdoll2D& ragdoll, const std::string& name) {
    const auto weak = ragdoll.weakness.find(name);
    const float left =
        std::clamp(1.0F - (weak != ragdoll.weakness.end() ? weak->second : 0.0F), 0.0F, 1.0F);
    return ragdoll.strength * left;
}

// One step of an implicit spring toward x away, from velocity v: what physics3d's drives do.
float spring(float v, float x, float stiffness, float damping, float limit, float seconds) {
    const float next = (v + (seconds * stiffness * x)) /
                       (1.0F + (seconds * damping) + (seconds * seconds * stiffness));
    if (limit <= 0.0F) {
        return next;
    }
    const float most = limit * seconds;
    return v + std::clamp(next - v, -most, most);
}

// Turns a limp ragdoll toward getting up: moves its entity under where it lies, starts the
// get-up animation for how it lies on its AnimationPlayer, and reports which.
std::string start_getting_up(world::World& world, entt::entity entity,
                             const world::RagdollProfile& profile,
                             const std::map<std::string, entt::entity>& bones) {
    auto& ecs = world.ecs();
    const world::Tree2D tree(world);
    entt::entity balance{entt::null};
    if (const auto it = bones.find(profile.balance.bone); it != bones.end()) {
        balance = it->second;
    } else if (!bones.empty()) {
        balance = bones.begin()->second;
    }
    if (balance == entt::null) {
        return "up";
    }
    const world::Affine2D placed = tree.global_affine(balance);
    // Forward in the bone's space; up is -y on the screen.
    const Vec2 forward{profile.get_up.forward.x, profile.get_up.forward.y};
    const Vec2 turned{(placed.x.x * forward.x) + (placed.y.x * forward.y),
                      (placed.x.y * forward.x) + (placed.y.y * forward.y)};
    const bool face_up = turned.y <= 0.0F;
    // The entity moves across to under the bone, keeping its height: a cut-out's origin is at
    // its feet, on the ground it stood on.
    const Transform2D root = tree.global(entity);
    tree.set_global_position(world, entity, {placed.origin.x, root.position.y});
    const std::string& animation = face_up ? profile.get_up.face_up : profile.get_up.face_down;
    if (auto* player = ecs.try_get<AnimationPlayer>(entity);
        player != nullptr && !animation.empty()) {
        player->animation = animation;
        player->time = 0.0;
        player->playing = true;
        player->backwards = false;
    }
    return face_up ? "up" : "down";
}

// Each bone's place in the world as the animation's pose has it: the bones' poses and the other
// entities' own transforms, from the ragdoll's entity down.
std::map<entt::entity, world::Affine2D>
animated_places(const world::World& world, const world::Tree2D& tree, entt::entity root,
                const std::map<entt::entity, Bone>& state, const std::vector<entt::entity>& order) {
    std::map<entt::entity, world::Affine2D> out;
    out.emplace(root, tree.global_affine(root));
    const auto& ecs = world.ecs();
    for (const entt::entity entity : order) {
        const entt::entity parent = tree.parent(entity);
        const auto above = out.find(parent);
        if (above == out.end()) {
            continue;
        }
        const auto bone = state.find(entity);
        const Transform2D own = bone != state.end() && bone->second.has_pose
                                    ? bone->second.pose
                                    : ecs.get<const Transform2D>(entity);
        out.emplace(entity, above->second * world::affine(own));
    }
    return out;
}

void run_ragdoll(world::World& world, entt::entity entity, Ragdoll& state, float seconds) {
    auto profile = ragdoll_profile(world, entity);
    if (!profile) {
        if (state.warned != profile.error().message) {
            state.warned = profile.error().message;
            log::warn("ragdoll profile can't be used",
                      {{"id", std::string(world.id_of(entity))}, {"problem", state.warned}});
        }
        return;
    }
    state.warned.clear();
    const world::RagdollProfile& p = profile.value();
    auto& ecs = world.ecs();
    if (!ecs.all_of<Transform2D>(entity)) {
        ecs.emplace<Transform2D>(entity);
    }
    const std::string owner(world.id_of(entity));
    const std::vector<entt::entity> order = descendants(world, entity);
    const std::map<std::string, entt::entity> bones = ragdoll_bones(world, entity);
    std::set<entt::entity> bone_set;
    std::map<entt::entity, std::string> names;
    for (const auto& [name, part] : bones) {
        bone_set.insert(part);
        names.emplace(part, name);
    }
    std::erase_if(state.bones, [&](const auto& each) { return !bone_set.contains(each.first); });

    world::Ragdoll2D ragdoll = ecs.get<const world::Ragdoll2D>(entity);

    // The animation's pose: what it set since the last tick left a bone. A frozen bone follows
    // it; a simulating one is put back where physics left it.
    const bool simulating = ragdoll.state == "active" || ragdoll.state == "limp";
    {
        const world::Tree2D tree(world);
        for (const entt::entity part : order) {
            if (!bone_set.contains(part)) {
                continue;
            }
            Bone& bone = state.bones[part];
            const Transform2D own = ecs.get<const Transform2D>(part);
            if (!bone.has_left || !same_place(own, bone.left)) {
                bone.pose = own;
                bone.has_pose = true;
            }
            if (simulating && bone.has_left && !same_place(own, bone.left)) {
                auto& transform = ecs.get<Transform2D>(part);
                transform.position = bone.left.position;
                transform.rotation = bone.left.rotation;
                transform.scale = bone.left.scale;
                transform.skew = bone.left.skew;
            }
        }
    }

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

    const world::Tree2D tree(world);
    const std::map<entt::entity, world::Affine2D> animated =
        animated_places(world, tree, entity, state.bones, order);

    // An active ragdoll falls when its balance bone leans too far from the pose.
    if (ragdoll.state == "active" && p.balance.max_tilt > 0.0F) {
        if (const auto it = bones.find(p.balance.bone); it != bones.end()) {
            if (const auto pose = animated.find(it->second); pose != animated.end()) {
                const float tilt =
                    wrapped(angle_of(tree.global_affine(it->second)) - angle_of(pose->second));
                if (std::abs(tilt) > p.balance.max_tilt) {
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
            if (const auto* body = ecs.try_get<const RigidBody2D>(part); body != nullptr) {
                fastest = std::max(fastest, length(body->linear_velocity));
            }
        }
        ragdoll.still = fastest < p.get_up.settle_speed ? ragdoll.still + seconds : 0.0F;
        if (ragdoll.still >= p.get_up.settle_time) {
            for (const auto& [name, part] : bones) {
                const Transform2D placed = tree.global(part);
                state.bones[part].lying = placed.position;
                state.bones[part].lying_rotation = placed.rotation;
            }
            ecs.replace<world::Ragdoll2D>(entity, ragdoll);
            const std::string face = start_getting_up(world, entity, p, bones);
            ragdoll = ecs.get<const world::Ragdoll2D>(entity);
            ragdoll.state = "getting_up";
            ragdoll.rising = 0.0F;
            ragdoll.still = 0.0F;
            world.emit("ragdoll_getting_up", owner, json::Object{{"face", face}});
        }
    } else if (ragdoll.state != "limp") {
        ragdoll.still = 0.0F;
    }
    float blend = 1.0F; // how far getting up has gone from where the bodies lay to the pose
    const bool blending = ragdoll.state == "getting_up";
    if (blending) {
        ragdoll.rising += seconds;
        blend = std::min(1.0F, ragdoll.rising / std::max(p.get_up.blend, 1.0e-3F));
        if (ragdoll.rising >= p.get_up.blend) {
            ragdoll.state = ragdoll.rest_state;
            ragdoll.rising = 0.0F;
            blend = 1.0F;
            world.emit("ragdoll_got_up", owner);
        }
    } else {
        ragdoll.rising = 0.0F;
    }
    if (!(ecs.get<const world::Ragdoll2D>(entity) == ragdoll)) {
        ecs.replace<world::Ragdoll2D>(entity, ragdoll);
    }

    // Each bone's body, shape and joint, as the profile says; changed only when they differ.
    const bool frozen = ragdoll.state == "animated" || ragdoll.state == "getting_up";
    for (const entt::entity part : order) {
        const auto name = names.find(part);
        if (name == names.end()) {
            continue;
        }
        const world::RagdollBone& bone = p.bones.at(name->second);
        const auto* old_body = ecs.try_get<const RigidBody2D>(part);
        const RigidBody2D body =
            body_of(old_body != nullptr ? *old_body : RigidBody2D{}, bone, p, frozen);
        if (old_body == nullptr || body.collision_layer != old_body->collision_layer ||
            body.collision_mask != old_body->collision_mask || body.mass != old_body->mass ||
            body.physics_material_override != old_body->physics_material_override ||
            body.linear_damp != old_body->linear_damp ||
            body.angular_damp != old_body->angular_damp || body.freeze != old_body->freeze ||
            body.freeze_mode != old_body->freeze_mode || body.can_sleep != old_body->can_sleep) {
            ecs.emplace_or_replace<RigidBody2D>(part, body);
        }
        const auto* old_shape = ecs.try_get<const CollisionShape2D>(part);
        const CollisionShape2D shape =
            shape_of(old_shape != nullptr ? *old_shape : CollisionShape2D{}, bone);
        if (old_shape == nullptr || !(*old_shape == shape)) {
            ecs.emplace_or_replace<CollisionShape2D>(part, shape);
        }

        // The joint to the bone above, made where the bone is when it is first made, so its
        // limits count from that pose, as a PinJoint2D's do.
        const entt::entity above = bone_above(tree, part, entity, bone_set);
        const std::string joint_id = made_id(std::string(world.id_of(part)), "_joint");
        entt::entity joint = world.find(joint_id);
        if (above == entt::null || bone.joint == "none") {
            if (joint != entt::null && ecs.all_of<PinJoint2D>(joint)) {
                world.destroy(joint);
            }
            continue;
        }
        PinJoint2D wanted;
        wanted.node_a = above;
        wanted.node_b = part;
        wanted.angular_limit_enabled = bone.joint == "hinge";
        wanted.angular_limit_lower = bone.lower;
        wanted.angular_limit_upper = bone.upper;
        if (joint == entt::null) {
            auto made = world.create(joint_id);
            if (!made) {
                continue;
            }
            joint = made.value();
            Transform2D at;
            at.position = tree.global(part).position;
            ecs.emplace<Transform2D>(joint, at);
        }
        const auto* old_joint = ecs.try_get<const PinJoint2D>(joint);
        if (old_joint == nullptr || old_joint->node_a != wanted.node_a ||
            old_joint->node_b != wanted.node_b ||
            old_joint->angular_limit_enabled != wanted.angular_limit_enabled ||
            old_joint->angular_limit_lower != wanted.angular_limit_lower ||
            old_joint->angular_limit_upper != wanted.angular_limit_upper) {
            ecs.emplace_or_replace<PinJoint2D>(joint, wanted);
        }
    }

    // Getting up: the frozen bodies go from where they lay to the pose.
    if (blending) {
        for (const entt::entity part : order) {
            const auto bone = state.bones.find(part);
            const auto pose = animated.find(part);
            if (bone == state.bones.end() || pose == animated.end()) {
                continue;
            }
            const Vec2 to = pose->second.origin;
            const Vec2 from = bone->second.lying;
            tree.set_global_position(world, part,
                                     from + ((to - from) * static_cast<real_t>(blend)));
            const float turn = wrapped(angle_of(pose->second) - bone->second.lying_rotation);
            tree.set_global_rotation(world, part, bone->second.lying_rotation + (turn * blend));
        }
        return;
    }
    if (ragdoll.state == "animated") {
        // Frozen bodies stand where the pose has them, wherever they were left.
        for (const entt::entity part : order) {
            const auto bone = state.bones.find(part);
            if (bone == state.bones.end() || !bone->second.has_pose) {
                continue;
            }
            auto& own = ecs.get<Transform2D>(part);
            if (!same_place(own, bone->second.pose)) {
                own.position = bone->second.pose.position;
                own.rotation = bone->second.pose.rotation;
                own.scale = bone->second.pose.scale;
                own.skew = bone->second.pose.skew;
            }
        }
        return;
    }
    if (ragdoll.state != "active") {
        return;
    }

    // The muscles: each body's velocity takes a step toward the pose, as an implicit spring.
    for (const entt::entity part : order) {
        const auto name = names.find(part);
        const auto pose = animated.find(part);
        if (name == names.end() || pose == animated.end()) {
            continue;
        }
        const world::RagdollBone& bone = p.bones.at(name->second);
        const float scale = strength_of(ragdoll, name->second);
        const bool balancing = name->second == p.balance.bone;
        std::string space = bone.strength > 0.0F ? bone.drive : "off";
        float stiffness = bone.stiffness * bone.strength * scale;
        float damping = bone.damping * bone.strength * scale;
        float pin = bone.pin * bone.strength * scale;
        float pin_damping = bone.pin_damping * bone.strength * scale;
        if (balancing) {
            space = "world";
            stiffness = p.balance.stiffness * scale;
            damping = p.balance.damping * scale;
            pin = p.balance.position_stiffness * scale;
            pin_damping = p.balance.position_damping * scale;
        }
        auto& body = ecs.get<RigidBody2D>(part);
        const world::Affine2D now = tree.global_affine(part);
        float target = angle_of(pose->second);
        if (space == "local") {
            // The pose's turn from the bone above, from where that bone is now.
            const entt::entity above = bone_above(tree, part, entity, bone_set);
            if (const auto up = animated.find(above); above != entt::null && up != animated.end()) {
                target = angle_of(tree.global_affine(above)) +
                         wrapped(angle_of(pose->second) - angle_of(up->second));
            }
        }
        if (space != "off") {
            body.angular_velocity = spring(body.angular_velocity, wrapped(target - angle_of(now)),
                                           stiffness, damping, bone.max_acceleration, seconds);
        }
        if (pin > 0.0F) {
            const Vec2 miss = pose->second.origin - now.origin;
            body.linear_velocity = {
                spring(static_cast<float>(body.linear_velocity.x), static_cast<float>(miss.x), pin,
                       pin_damping, 0.0F, seconds),
                spring(static_cast<float>(body.linear_velocity.y), static_cast<float>(miss.y), pin,
                       pin_damping, 0.0F, seconds)};
        }
        body.sleeping = false;
    }
}

} // namespace

Result<world::RagdollProfile> ragdoll_profile(const world::World& world, entt::entity entity) {
    const auto* ragdoll = world.ecs().try_get<const world::Ragdoll2D>(entity);
    if (ragdoll == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "has no Ragdoll2D");
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
    auto profile = ragdoll_profile(world, entity);
    if (!profile) {
        return out;
    }
    for (const entt::entity part : descendants(world, entity)) {
        std::string name = bone_name(world, entity, part);
        if (profile.value().bones.contains(name)) {
            out.emplace(std::move(name), part);
        }
    }
    return out;
}

void add_ragdoll_systems(world::World& world) {
    if (world.ecs().ctx().contains<std::shared_ptr<Runtime>>()) {
        return;
    }
    auto runtime = std::make_shared<Runtime>();
    world.ecs().ctx().emplace<std::shared_ptr<Runtime>>(runtime);
    world.add_reset([runtime](world::World& /*world*/) { runtime->ragdolls.clear(); });
    world.add_system(
        "ragdoll_2d",
        [runtime](world::World& w, double tick_seconds) {
            std::vector<entt::entity> ragdolls;
            for (const entt::entity entity : w.ecs().view<const world::Ragdoll2D>()) {
                ragdolls.push_back(entity);
            }
            // In the order they were made, the same in every session.
            std::ranges::sort(ragdolls, {}, [](entt::entity e) { return entt::to_entity(e); });
            std::erase_if(runtime->ragdolls, [&](const auto& each) {
                return !w.valid(each.first) || !w.ecs().all_of<world::Ragdoll2D>(each.first);
            });
            for (const entt::entity entity : ragdolls) {
                if (w.valid(entity) && w.ecs().all_of<world::Ragdoll2D>(entity)) {
                    run_ragdoll(w, entity, runtime->ragdolls[entity],
                                static_cast<float>(tick_seconds));
                }
            }
        },
        {.doc = "Keeps each Ragdoll2D's bodies, shapes, joints and muscles as its profile says "
                "and runs its states: hit reactions, falls and getting back up.",
         .priority = 940});
    world.add_system(
        "ragdoll_2d_pose",
        [runtime](world::World& w, double /*tick_seconds*/) {
            for (auto& [entity, state] : runtime->ragdolls) {
                if (!w.valid(entity)) {
                    continue;
                }
                for (auto& [part, bone] : state.bones) {
                    if (const auto* transform = w.ecs().try_get<const Transform2D>(part);
                        transform != nullptr) {
                        bone.left = *transform;
                        bone.has_left = true;
                    }
                }
            }
        },
        {.doc = "Keeps where the physics step left each Ragdoll2D bone, so the next tick tells "
                "the animation's pose from it.",
         .priority = 955});
}

Result<RagdollHitReport> hit_ragdoll(world::World& world, entt::entity entity,
                                     std::string_view bone, Vec2 impulse) {
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
        std::string message = "the ragdoll has no bone \"" + std::string(bone) + "\"";
        if (const std::string_view hint = closest_match(bone, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    auto& ecs = world.ecs();
    const float size = length(impulse);
    if (auto* body = ecs.try_get<RigidBody2D>(part->second); body != nullptr) {
        body->linear_velocity =
            body->linear_velocity +
            (impulse * static_cast<real_t>(1.0F / std::max(body->mass, 1.0e-3F)));
        body->sleeping = false;
    }

    auto ragdoll = ecs.get<const world::Ragdoll2D>(entity);
    const auto weaken = [&](const std::string& name, float amount) {
        float& weak = ragdoll.weakness[name];
        weak = std::min(1.0F, weak + amount);
    };
    weaken(std::string(bone), size * p.hit.weaken);
    if (p.hit.spread > 0.0F) {
        // The bones under it, a share less for each bone between them.
        std::set<entt::entity> bone_set;
        for (const auto& [name, each] : bones) {
            bone_set.insert(each);
        }
        const world::Tree2D tree(world);
        for (const auto& [name, each] : bones) {
            if (each == part->second) {
                continue;
            }
            float share = 1.0F;
            bool under = false;
            for (entt::entity up = tree.parent(each); up != entt::null && up != entity;
                 up = tree.parent(up)) {
                if (bone_set.contains(up)) {
                    share *= p.hit.spread;
                }
                if (up == part->second) {
                    under = true;
                    break;
                }
            }
            if (under) {
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
        ragdoll.state = "active";
        ragdoll.reaction = p.hit.recovery;
    }
    report.state = ragdoll.state;
    report.weakness = ragdoll.weakness;
    ecs.replace<world::Ragdoll2D>(entity, std::move(ragdoll));
    world.emit("ragdoll_hit", owner,
               json::Object{{"bone", std::string(bone)}, {"impulse", rounded(size)}});
    if (report.knocked_down) {
        world.emit("ragdoll_fell", owner, json::Object{{"reason", "hit"}});
    }
    return report;
}

Result<json::Value> generate_ragdoll(const world::World& world, entt::entity entity,
                                     const RagdollOptions& options) {
    const auto& ecs = world.ecs();
    if (!ecs.all_of<Transform2D>(entity)) {
        return make_error(ErrorCode::InvalidArgument, "has no Transform2D: a 2D ragdoll is made "
                                                      "of the Transform2D tree under it");
    }
    const std::vector<entt::entity> order = descendants(world, entity);
    const world::Tree2D tree(world);
    // The tree's height, for the shortest bone that gets a body.
    float top = std::numeric_limits<float>::max();
    float bottom = std::numeric_limits<float>::lowest();
    for (const entt::entity part : order) {
        const float y = static_cast<float>(tree.global(part).position.y);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    const float min_length =
        options.min_length > 0.0F ? options.min_length : 0.04F * std::max(bottom - top, 1.0F);

    // The bones: entities with children or a sprite.
    std::vector<entt::entity> chosen;
    std::set<entt::entity> chosen_set;
    for (const entt::entity part : order) {
        const auto& transform = ecs.get<const Transform2D>(part);
        if (!transform.children.empty() || ecs.all_of<Sprite2D>(part)) {
            chosen.push_back(part);
            chosen_set.insert(part);
        }
    }
    struct Made {
        std::string name;
        json::Value bone;
        float area = 0.0F;
        entt::entity entity{entt::null};
    };
    std::vector<Made> made;
    std::map<entt::entity, float> lengths;
    for (const entt::entity part : chosen) {
        // The child bones' places in the bone's space.
        std::vector<Vec2> ends;
        for (const entt::entity child : ecs.get<const Transform2D>(part).children) {
            if (chosen_set.contains(child) && tree.parent(child) == part) {
                ends.push_back(ecs.get<const Transform2D>(child).position);
            }
        }
        json::Value bone;
        float area = 0.0F;
        float span = 0.0F;
        if (ends.size() == 1) {
            span = length(ends.front());
            if (span < min_length) {
                continue;
            }
            const float radius = std::max(0.18F * span, 1.0F);
            const Vec2 d = ends.front();
            bone["shape"] = "capsule";
            bone["radius"] = rounded(radius);
            bone["height"] = rounded(span);
            bone["offset"] = json::Array{rounded(static_cast<double>(d.x) / 2.0),
                                         rounded(static_cast<double>(d.y) / 2.0)};
            // The capsule's y axis along the bone.
            bone["rotation"] =
                rounded(slash::atan2(-static_cast<float>(d.x), static_cast<float>(d.y)));
            area =
                (2.0F * radius * std::max(span - (2.0F * radius), 0.0F)) + (kPi * radius * radius);
        } else if (ends.size() > 1) {
            Vec2 low{0.0F, 0.0F};
            Vec2 high{0.0F, 0.0F};
            for (const Vec2 end : ends) {
                low = {std::min(low.x, end.x), std::min(low.y, end.y)};
                high = {std::max(high.x, end.x), std::max(high.y, end.y)};
            }
            span = std::max(length(high - low), min_length);
            const float pad = 0.15F * span;
            const Vec2 size{(high.x - low.x) + pad, (high.y - low.y) + pad};
            bone["shape"] = "rectangle";
            bone["size"] = json::Array{rounded(static_cast<double>(size.x)),
                                       rounded(static_cast<double>(size.y))};
            bone["offset"] = json::Array{rounded(static_cast<double>(low.x + high.x) / 2.0),
                                         rounded(static_cast<double>(low.y + high.y) / 2.0)};
            area = static_cast<float>(size.x * size.y);
        } else {
            // A tip (a hand, a head): a circle half its parent bone's length across.
            const auto parent = lengths.find(bone_above(tree, part, entity, chosen_set));
            span = parent != lengths.end() ? 0.5F * parent->second : 2.0F * min_length;
            if (span < min_length) {
                continue;
            }
            const float radius = 0.5F * span;
            bone["shape"] = "circle";
            bone["radius"] = rounded(radius);
            area = kPi * radius * radius;
        }
        lengths.emplace(part, span);
        made.push_back({.name = bone_name(world, entity, part),
                        .bone = std::move(bone),
                        .area = area,
                        .entity = part});
    }
    if (made.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "no entity under it is a bone long enough: a 2D ragdoll needs a tree "
                          "of entities with children or sprites");
    }
    std::set<entt::entity> made_set;
    for (const Made& each : made) {
        made_set.insert(each.entity);
    }
    float total = 0.0F;
    for (const Made& each : made) {
        total += each.area;
    }
    // Hinges between bones; the topmost bone, with the most under it, keeps the balance.
    std::string balance;
    std::size_t most = 0;
    json::Value bones = json::Object{};
    for (Made& each : made) {
        const entt::entity above = bone_above(tree, each.entity, entity, made_set);
        if (above == entt::null) {
            each.bone["joint"] = "none";
            std::size_t under = 0;
            for (const Made& other : made) {
                for (entt::entity up = tree.parent(other.entity); up != entt::null && up != entity;
                     up = tree.parent(up)) {
                    if (up == each.entity) {
                        ++under;
                        break;
                    }
                }
            }
            if (balance.empty() || under > most) {
                balance = each.name;
                most = under;
            }
        } else {
            each.bone["joint"] = "hinge";
            each.bone["lower"] = -1.2;
            each.bone["upper"] = 1.2;
        }
        each.bone["mass"] =
            rounded(std::max(options.mass * (total > 0.0F ? each.area / total : 0.0F), 0.01F));
        bones[each.name] = std::move(each.bone);
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
    file["bones"] = std::move(bones);
    json::Value balance_settings;
    balance_settings["bone"] = balance;
    balance_settings["stiffness"] = rounded(options.stiffness);
    balance_settings["damping"] = rounded(options.damping);
    balance_settings["position_stiffness"] = rounded(options.pin);
    balance_settings["position_damping"] = rounded(options.pin_damping);
    balance_settings["max_tilt"] = 0.6;
    file["balance"] = std::move(balance_settings);
    json::Value get_up;
    get_up["forward"] = json::Array{1.0, 0.0, 0.0};
    // Pixels a second: bodies settle on Box2D's contacts slower than 3D's meters.
    get_up["settle_speed"] = rounded(std::max(0.1F * (bottom - top), 5.0F));
    file["get_up"] = std::move(get_up);
    return file;
}

std::vector<RagdollProblem> check_ragdoll(const world::World& world, entt::entity entity) {
    std::vector<RagdollProblem> out;
    const auto* ragdoll = world.ecs().try_get<const world::Ragdoll2D>(entity);
    if (ragdoll == nullptr) {
        return out;
    }
    constexpr std::string_view kProfile = "/components/Ragdoll2D/profile";
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
                                                    : "/components/Ragdoll2D/overrides",
                       "\"" + ragdoll->profile + "\": " + profile.error().message});
        return out;
    }
    std::vector<std::string> names;
    for (const entt::entity part : descendants(world, entity)) {
        names.push_back(bone_name(world, entity, part));
    }
    const std::string own = bone_name(world, entity, entity);
    for (const auto& [name, bone] : profile.value().bones) {
        const std::string where = "\"" + ragdoll->profile + "\" bone \"" + name + "\"";
        if (name == own) {
            out.push_back({std::string(kProfile),
                           where + " is the Ragdoll2D's own entity: bones are the entities "
                                   "under it"});
            continue;
        }
        if (std::ranges::find(names, name) == names.end()) {
            std::string message = where + " is no entity under the Ragdoll2D";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            out.push_back({std::string(kProfile), std::move(message)});
        }
        if (bone.joint == "cone") {
            out.push_back({std::string(kProfile),
                           where + R"( has a cone joint, which 2D lacks: use "hinge" or "pin")"});
        }
        if (bone.shape == "box" || bone.shape == "sphere") {
            out.push_back({std::string(kProfile),
                           where + " has shape \"" + bone.shape +
                               R"(", a 3D one: use "capsule", "circle" or "rectangle")"});
        }
    }
    return out;
}

std::vector<json::ValidationError> check_ragdolls(const world::ComponentRegistry& components,
                                                  const json::Value& scene,
                                                  const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    world::World world(components);
    world.set_file_reader(reader);
    if (!world::load_scene(world, scene)) {
        return errors;
    }
    std::vector<entt::entity> ragdolls;
    for (const entt::entity entity : world.ecs().view<const world::Ragdoll2D>()) {
        ragdolls.push_back(entity);
    }
    std::ranges::sort(ragdolls, {}, [](entt::entity e) { return entt::to_entity(e); });
    for (const entt::entity entity : ragdolls) {
        for (RagdollProblem& problem : check_ragdoll(world, entity)) {
            errors.push_back(
                {"/entities/" + json::pointer_escape(world.id_of(entity)) + problem.path,
                 std::move(problem.message)});
        }
    }
    return errors;
}

} // namespace slash::physics2d
