#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

// 2D skeletons: a Skeleton2D's Bone2Ds, and Polygon2Ds whose points bend with them. Each point
// weighted to bones moves by the way each bone has moved from its rest to its pose, blended by
// weight, in the skeleton's space.
namespace slash::world {

class World;

// One bone of a skeleton, with its rest and pose in the skeleton's space.
struct SkeletonBone2D {
    entt::entity entity{entt::null};
    entt::entity parent{entt::null}; // the nearest bone above it, or null
    Affine2D rest;
    Affine2D pose;
    Vec2 tip; // where its length reaches at rest
};

// A Skeleton2D's bones, parents before children: the Bone2D entities in its Transform2D tree.
[[nodiscard]] std::vector<SkeletonBone2D> skeleton_bones_2d(const World& world, const Tree2D& tree,
                                                            entt::entity skeleton);

// A skinned polygon's points, and the first thing wrong with its skin (path is a JSON Pointer
// under the entity, such as "/components/Polygon2D/bone_weights/0/bone"); a bad bone is left out.
struct Skin2D {
    std::vector<Vec2> points;
    std::string path;
    std::string problem;
};

// Points of an entity's Polygon2D (given in its space, offset included) as its skeleton's bones
// move them, or nullopt when it is not skinned: no skeleton, no bone_weights, or a skeleton
// showing its rest.
[[nodiscard]] std::optional<Skin2D> skin_2d(const World& world, const Tree2D& tree,
                                            entt::entity polygon, std::span<const Vec2> points);

// Sets each bone's rest to its Transform2D now, returning how many bones it has.
std::size_t set_rest_2d(World& world, entt::entity skeleton);

// Weights an entity's Polygon2D points to a skeleton's bones at rest: each point to the
// bones_per_point bones nearest it, by the inverse fourth power of its distance to each bone's
// segment (from its origin to its tip), adding to 1. Weights are kept to 3 places; bones that weigh
// no point are left out.
[[nodiscard]] std::vector<BoneWeights2D> auto_weights_2d(const World& world, const Tree2D& tree,
                                                         entt::entity skeleton,
                                                         entt::entity polygon,
                                                         int bones_per_point = 2);

} // namespace slash::world
