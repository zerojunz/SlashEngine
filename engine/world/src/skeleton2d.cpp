#include "slash/world/skeleton2d.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slash::world {
namespace {

constexpr real_t kDefaultLength = 16.0F;

real_t dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

// A point's distance to the segment from a to b.
real_t distance_to_segment(Vec2 point, Vec2 a, Vec2 b) {
    const Vec2 along = b - a;
    const real_t squared = dot(along, along);
    real_t t = 0.0F;
    if (squared > 0.0F) {
        t = std::clamp(dot(point - a, along) / squared, real_t{0.0F}, real_t{1.0F});
    }
    const Vec2 nearest = a + (along * t);
    const Vec2 gap = point - nearest;
    return std::sqrt(dot(gap, gap));
}

Affine2D rest_affine(const Bone2D& bone) {
    Transform2D rest;
    rest.position = bone.rest_position;
    rest.rotation = bone.rest_rotation;
    rest.scale = bone.rest_scale;
    return affine(rest);
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

} // namespace

std::vector<SkeletonBone2D> skeleton_bones_2d(const World& world, const Tree2D& tree,
                                              entt::entity skeleton) {
    std::vector<SkeletonBone2D> bones;
    const auto& ecs = world.ecs();
    if (!ecs.valid(skeleton) || !ecs.all_of<Transform2D>(skeleton)) {
        return bones;
    }
    const Affine2D to_skeleton = tree.global_affine(skeleton).inverse();
    // Depth first, so parents come before their children: each entity with the rest of the way
    // from the skeleton to it and the nearest bone above it.
    struct Visit {
        entt::entity entity;
        Affine2D rest;
        entt::entity parent_bone;
    };
    std::vector<Visit> stack;
    std::unordered_set<entt::entity> seen{skeleton};
    const auto push_children = [&](entt::entity entity, const Affine2D& rest, entt::entity bone) {
        const auto& children = ecs.get<const Transform2D>(entity).children;
        for (const entt::entity child : std::views::reverse(children)) {
            stack.push_back({.entity = child, .rest = rest, .parent_bone = bone});
        }
    };
    push_children(skeleton, Affine2D{}, entt::null);
    while (!stack.empty()) {
        const Visit visit = stack.back();
        stack.pop_back();
        const entt::entity entity = visit.entity;
        if (!ecs.valid(entity) || !ecs.all_of<Transform2D>(entity) || !seen.insert(entity).second) {
            continue;
        }
        const Transform2D& transform = ecs.get<const Transform2D>(entity);
        // Another skeleton's bones are its own, and a top_level entity leaves its parent's space.
        if (transform.top_level || ecs.all_of<Skeleton2D>(entity)) {
            continue;
        }
        const Bone2D* bone = ecs.try_get<const Bone2D>(entity);
        const Affine2D rest =
            visit.rest * (bone != nullptr ? rest_affine(*bone) : affine(transform));
        entt::entity below = visit.parent_bone;
        if (bone != nullptr) {
            bones.push_back({.entity = entity,
                             .parent = visit.parent_bone,
                             .rest = rest,
                             .pose = to_skeleton * tree.global_affine(entity),
                             .tip = rest.apply(Vec2{static_cast<real_t>(bone->length), 0.0F})});
            below = entity;
        }
        push_children(entity, rest, below);
    }
    // A bone of length 0 reaches to its first child bone, or kDefaultLength along its x axis.
    for (SkeletonBone2D& bone : bones) {
        if (ecs.get<const Bone2D>(bone.entity).length > 0.0F) {
            continue;
        }
        const auto child = std::ranges::find(bones, bone.entity, &SkeletonBone2D::parent);
        bone.tip =
            child != bones.end() ? child->rest.origin : bone.rest.apply(Vec2{kDefaultLength, 0.0F});
    }
    return bones;
}

std::optional<Skin2D> skin_2d(const World& world, const Tree2D& tree, entt::entity polygon,
                              std::span<const Vec2> points) {
    const auto& ecs = world.ecs();
    const Polygon2D* shape = ecs.try_get<const Polygon2D>(polygon);
    if (shape == nullptr || shape->skeleton == entt::null || shape->bone_weights.empty()) {
        return std::nullopt;
    }
    Skin2D skin;
    skin.points.assign(points.begin(), points.end());
    const entt::entity skeleton = shape->skeleton;
    const Skeleton2D* root =
        ecs.valid(skeleton) ? ecs.try_get<const Skeleton2D>(skeleton) : nullptr;
    if (root == nullptr) {
        skin.path = "/components/Polygon2D/skeleton";
        skin.problem = ecs.valid(skeleton) ? quoted(world.id_of(skeleton)) + " has no Skeleton2D"
                                           : "its skeleton is gone";
        return skin;
    }
    if (root->show_rest_only) {
        return std::nullopt;
    }
    std::unordered_map<entt::entity, Affine2D> moves; // each bone's rest to its pose
    for (const SkeletonBone2D& bone : skeleton_bones_2d(world, tree, skeleton)) {
        moves.emplace(bone.entity, bone.pose * bone.rest.inverse());
    }
    struct Weighted {
        const Affine2D* move;
        const std::vector<float>* weights;
    };
    std::vector<Weighted> weighted;
    for (std::size_t i = 0; i < shape->bone_weights.size(); ++i) {
        const BoneWeights2D& entry = shape->bone_weights[i];
        const auto found = moves.find(entry.bone);
        if (found == moves.end()) {
            if (skin.problem.empty()) {
                skin.path = "/components/Polygon2D/bone_weights/" + std::to_string(i) + "/bone";
                skin.problem = (ecs.valid(entry.bone)
                                    ? quoted(world.id_of(entry.bone)) + " is not a Bone2D of " +
                                          quoted(world.id_of(skeleton))
                                    : std::string("the bone is gone"));
            }
            continue;
        }
        weighted.push_back({.move = &found->second, .weights = &entry.weights});
    }
    const Affine2D to_skeleton =
        tree.global_affine(skeleton).inverse() * tree.global_affine(polygon);
    const Affine2D from_skeleton = to_skeleton.inverse();
    for (std::size_t index = 0; index < skin.points.size(); ++index) {
        const Vec2 at = to_skeleton.apply(skin.points[index]);
        Vec2 sum;
        real_t total = 0.0F;
        for (const Weighted& bone : weighted) {
            const real_t weight =
                index < bone.weights->size() ? static_cast<real_t>((*bone.weights)[index]) : 0.0F;
            if (weight <= 0.0F) {
                continue;
            }
            sum = sum + (bone.move->apply(at) * weight);
            total += weight;
        }
        if (total > 0.0F) {
            skin.points[index] = from_skeleton.apply(sum * (1.0F / total));
        }
    }
    return skin;
}

std::size_t set_rest_2d(World& world, entt::entity skeleton) {
    const Tree2D tree(world);
    const std::vector<SkeletonBone2D> bones = skeleton_bones_2d(world, tree, skeleton);
    for (const SkeletonBone2D& bone : bones) {
        const Transform2D transform = world.ecs().get<const Transform2D>(bone.entity);
        world.ecs().patch<Bone2D>(bone.entity, [&](Bone2D& rest) {
            rest.rest_position = transform.position;
            rest.rest_rotation = transform.rotation;
            rest.rest_scale = transform.scale;
        });
    }
    return bones.size();
}

std::vector<BoneWeights2D> auto_weights_2d(const World& world, const Tree2D& tree,
                                           entt::entity skeleton, entt::entity polygon,
                                           int bones_per_point) {
    const auto& ecs = world.ecs();
    const Polygon2D* shape = ecs.try_get<const Polygon2D>(polygon);
    const std::vector<SkeletonBone2D> bones = skeleton_bones_2d(world, tree, skeleton);
    if (shape == nullptr || bones.empty()) {
        return {};
    }
    const std::size_t count = shape->polygon.size();
    const std::size_t keep = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::max(bones_per_point, 1)), 1, bones.size());
    const Affine2D to_skeleton =
        tree.global_affine(skeleton).inverse() * tree.global_affine(polygon);
    std::vector<std::vector<float>> weights(bones.size(), std::vector<float>(count, 0.0F));
    std::vector<std::pair<real_t, std::size_t>> nearest(bones.size());
    for (std::size_t point = 0; point < count; ++point) {
        const Vec2 at = to_skeleton.apply(shape->polygon[point] + shape->offset);
        for (std::size_t b = 0; b < bones.size(); ++b) {
            nearest[b] = {distance_to_segment(at, bones[b].rest.origin, bones[b].tip), b};
        }
        std::ranges::partial_sort(nearest, nearest.begin() + static_cast<std::ptrdiff_t>(keep));
        // One pixel added keeps a point on a bone from dividing by zero.
        real_t total = 0.0F;
        std::vector<real_t> raw(keep);
        for (std::size_t k = 0; k < keep; ++k) {
            const real_t d = nearest[k].first + 1.0F;
            raw[k] = 1.0F / (d * d * d * d);
            total += raw[k];
        }
        for (std::size_t k = 0; k < keep; ++k) {
            const auto share = static_cast<double>(raw[k] / total);
            weights[nearest[k].second][point] =
                static_cast<float>(std::round(share * 1000.0) / 1000.0);
        }
    }
    std::vector<BoneWeights2D> out;
    for (std::size_t b = 0; b < bones.size(); ++b) {
        if (std::ranges::any_of(weights[b], [](float weight) { return weight > 0.0F; })) {
            out.push_back({.bone = bones[b].entity, .weights = std::move(weights[b])});
        }
    }
    return out;
}

} // namespace slash::world
