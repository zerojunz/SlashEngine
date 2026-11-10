// The visual debugger's record of a 2D step, as Chaos's Visual Debugger keeps one: the contacts
// the step solved, how hard each joint pulled and how far it stretched, and how many bodies were
// awake. The fields' share is recorded where they act (fields.cpp).

#include "space.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace slash::physics2d::detail {

namespace {

std::string id_of_body(const world::World& world, b2BodyId body) {
    if (B2_IS_NULL(body) || !b2Body_IsValid(body)) {
        return {};
    }
    const auto* made = static_cast<const Body*>(b2Body_GetUserData(body));
    return made != nullptr ? std::string(world.id_of(made->entity)) : std::string();
}

std::string id_of_shape(const world::World& world, b2ShapeId shape) {
    const ShapeInfo* info = info_of(shape);
    return info != nullptr ? std::string(world.id_of(info->entity)) : std::string();
}

std::string joint_kind(const JointKey& key) {
    return std::visit(
        [](const auto& joint) {
            using Joint = std::decay_t<decltype(joint)>;
            if constexpr (std::is_same_v<Joint, world::PinJoint2D>) {
                return std::string("PinJoint2D");
            } else if constexpr (std::is_same_v<Joint, world::DampedSpringJoint2D>) {
                return std::string("DampedSpringJoint2D");
            } else {
                return std::string("GrooveJoint2D");
            }
        },
        key);
}

} // namespace

void Space::finish_debug(const world::World& world, world::PhysicsDebugStep& step) const {
    std::map<std::pair<std::string, std::string>, world::DebugContact> contacts;
    std::vector<b2ContactData> data;
    const auto gather = [&](const Body& body) {
        if (B2_IS_NULL(body.id) || !b2Body_IsValid(body.id)) {
            return;
        }
        const int capacity = b2Body_GetContactCapacity(body.id);
        if (capacity == 0) {
            return;
        }
        data.resize(static_cast<std::size_t>(capacity));
        const int count = b2Body_GetContactData(body.id, data.data(), capacity);
        for (int i = 0; i < count; ++i) {
            const b2ContactData& contact = data[static_cast<std::size_t>(i)];
            const b2Manifold& manifold = contact.manifold;
            if (manifold.pointCount == 0) {
                continue;
            }
            float separation = manifold.points[0].separation;
            float closing = 0.0F;
            float impulse = 0.0F;
            b2Vec2 middle = b2Vec2_zero;
            for (int at = 0; at < manifold.pointCount; ++at) {
                const b2ManifoldPoint& point = manifold.points[at];
                separation = std::min(separation, point.separation);
                closing = std::max(closing, -point.normalVelocity);
                impulse += point.totalNormalImpulse;
                middle = b2Add(middle, point.point);
            }
            if (separation > kLinearSlop) {
                continue; // only near: Box2D keeps contacts a little ahead
            }
            middle = b2MulSV(1.0F / static_cast<float>(manifold.pointCount), middle);
            std::string a = id_of_shape(world, contact.shapeIdA);
            std::string b = id_of_shape(world, contact.shapeIdB);
            if (a.empty() || b.empty()) {
                continue;
            }
            // Box2D's normal points from shape A toward shape B; the record names them in order.
            Vec3 normal{manifold.normal.x, manifold.normal.y, 0.0F};
            if (b < a) {
                std::swap(a, b);
                normal = normal * -1.0F;
            }
            std::pair<std::string, std::string> key{a, b};
            if (contacts.contains(key)) {
                continue;
            }
            contacts.emplace(std::move(key), world::DebugContact{
                                                 .a = std::move(a),
                                                 .b = std::move(b),
                                                 .point = {middle.x, middle.y, 0.0F},
                                                 .normal = normal,
                                                 .depth = std::max(-separation, 0.0F),
                                                 .speed = closing,
                                                 .impulse = impulse,
                                             });
        }
    };
    for (const auto& [entity, body] : bodies_) {
        gather(*body);
        if (body->kind == Kind::Rigid) {
            ++(body->sleeping ? step.sleeping : step.awake);
        }
    }
    for (const auto& [entity, tiles] : tiles_) {
        gather(*tiles);
    }
    for (auto& [key, contact] : contacts) {
        step.contacts.push_back(std::move(contact));
    }
    for (const auto& [entity, joint] : joints_) {
        if (!b2Joint_IsValid(joint.id)) {
            continue;
        }
        const b2Vec2 force = b2Joint_GetConstraintForce(joint.id);
        step.joints.push_back({
            .id = std::string(world.id_of(entity)),
            .kind = joint_kind(joint.key),
            .a = id_of_body(world, joint.body_a),
            .b = id_of_body(world, joint.body_b),
            .force = b2Length(force),
            .stretch = b2Joint_GetLinearSeparation(joint.id),
        });
    }
    std::ranges::sort(step.joints, [](const world::DebugJoint& x, const world::DebugJoint& y) {
        return x.id < y.id;
    });
}

} // namespace slash::physics2d::detail
