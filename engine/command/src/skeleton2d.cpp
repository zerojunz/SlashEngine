#include "skeleton2d.hpp"

#include "slash/core/text.hpp"
#include "slash/world/components.hpp"
#include "slash/world/skeleton2d.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// skeleton.bind, and skeleton.get of a Skeleton2D, over slash/world/skeleton2d.hpp.
namespace slash::command::detail {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

json::Value vec(Vec2 v) {
    return json::Array{rounded(v.x), rounded(v.y)};
}

// The entity an id names, or a reply saying there is none.
Result<entt::entity> named(const world::World& world, const std::string& id) {
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        std::string message = "no entity has the id " + quoted(id);
        if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    return entity;
}

// The polygons a skeleton bends: each Polygon2D whose skeleton it is, in the order made.
std::vector<entt::entity> skinned_by(const world::World& world, entt::entity skeleton) {
    std::vector<entt::entity> out;
    const auto view = world.ecs().view<const world::Polygon2D>();
    for (const entt::entity entity : view) {
        if (view.get<const world::Polygon2D>(entity).skeleton == skeleton) {
            out.push_back(entity);
        }
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

json::Value pose_json(const world::Affine2D& matrix) {
    const world::Transform2D parts = world::parts(matrix);
    json::Value out;
    out["position"] = vec(parts.position);
    out["rotation"] = rounded(parts.rotation);
    return out;
}

Reply skeleton_bind(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto found = named(world, params.find("id")->as_string());
    if (!found) {
        return param_error(code::kNotFound, "/id", found.error().message);
    }
    const entt::entity skeleton = found.value();
    const std::string& id = params.find("id")->as_string();
    if (!world.ecs().all_of<world::Skeleton2D>(skeleton) ||
        !world.ecs().all_of<world::Transform2D>(skeleton)) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " has no Skeleton2D and Transform2D");
    }
    std::vector<entt::entity> polygons;
    if (const json::Value* list = params.find("polygons"); list != nullptr) {
        const json::Array& ids = list->as_array();
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string path = "/polygons/" + std::to_string(i);
            auto polygon = named(world, ids[i].as_string());
            if (!polygon) {
                return param_error(code::kNotFound, path, polygon.error().message);
            }
            if (!world.ecs().all_of<world::Polygon2D>(polygon.value())) {
                return param_error(code::kInvalidParams, path,
                                   quoted(ids[i].as_string()) + " has no Polygon2D");
            }
            polygons.push_back(polygon.value());
        }
    } else {
        polygons = skinned_by(world, skeleton);
    }
    const bool rest = params.find("rest") == nullptr || params.find("rest")->as_bool();
    const bool weigh = params.find("weights") == nullptr || params.find("weights")->as_bool();
    const int per_point = params.find("bones_per_point") != nullptr
                              ? static_cast<int>(params.find("bones_per_point")->as_int())
                              : 2;
    const world::Tree2D before(world);
    if (world::skeleton_bones_2d(world, before, skeleton).empty()) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " has no Bone2D in its Transform2D tree");
    }
    const std::size_t bones = rest ? world::set_rest_2d(world, skeleton) : 0;
    const world::Tree2D tree(world);
    json::Array bound;
    for (const entt::entity polygon : polygons) {
        std::vector<world::BoneWeights2D> weights;
        if (weigh) {
            weights = world::auto_weights_2d(world, tree, skeleton, polygon, per_point);
        }
        world.ecs().patch<world::Polygon2D>(polygon, [&](world::Polygon2D& shape) {
            shape.skeleton = skeleton;
            if (weigh) {
                shape.bone_weights = std::move(weights);
            }
        });
        const world::Polygon2D& shape = world.ecs().get<const world::Polygon2D>(polygon);
        json::Value entry;
        entry["id"] = world.id_of(polygon);
        entry["points"] = static_cast<std::int64_t>(shape.polygon.size());
        json::Array names;
        for (const world::BoneWeights2D& bone : shape.bone_weights) {
            names.emplace_back(std::string(world.id_of(bone.bone)));
        }
        entry["bones"] = std::move(names);
        bound.push_back(std::move(entry));
    }
    json::Value result;
    if (rest) {
        result["rested"] = static_cast<std::int64_t>(bones);
    }
    result["polygons"] = std::move(bound);
    return Reply::success(std::move(result));
}

json::Value id_property(std::string_view description) {
    json::Value id = property("string", description);
    id["minLength"] = 1;
    return id;
}

} // namespace

Reply skeleton_get_2d(Session& session, entt::entity skeleton, const json::Value& params) {
    const world::World& world = session.world();
    const world::Tree2D tree(world);
    std::vector<std::string> wanted;
    if (const json::Value* list = params.find("bones"); list != nullptr) {
        for (const json::Value& name : list->as_array()) {
            wanted.push_back(name.as_string());
        }
    }
    json::Array bones;
    for (const world::SkeletonBone2D& bone : world::skeleton_bones_2d(world, tree, skeleton)) {
        const std::string id(world.id_of(bone.entity));
        json::Value entry;
        entry["id"] = id;
        if (bone.parent != entt::null) {
            entry["parent"] = world.id_of(bone.parent);
        }
        if (std::ranges::find(wanted, id) != wanted.end()) {
            entry["rest"] = pose_json(bone.rest);
            entry["pose"] = pose_json(bone.pose);
            entry["tip"] = vec(bone.tip);
        }
        bones.push_back(std::move(entry));
    }
    json::Array polygons;
    for (const entt::entity polygon : skinned_by(world, skeleton)) {
        polygons.emplace_back(std::string(world.id_of(polygon)));
    }
    json::Value result;
    result["bones"] = std::move(bones);
    result["polygons"] = std::move(polygons);
    return Reply::success(std::move(result));
}

void register_skeleton2d_commands(Registry& registry) {
    using enum Capability;
    json::Value polygons = property("array", "IDs of Polygon2Ds to bind to it. Default: those "
                                             "whose skeleton it already is.");
    polygons["items"]["type"] = "string";
    polygons["maxItems"] = 256;
    json::Value per_point = property("integer", "Bones each point follows, nearest first. "
                                                "Default 2.");
    per_point["minimum"] = 1;
    per_point["maximum"] = 4;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"id", id_property("An entity with a Skeleton2D.")},
        {"polygons", std::move(polygons)},
        {"rest", property("boolean", "Sets each Bone2D's rest to its pose first. Default true.")},
        {"weights", property("boolean", "Weights each polygon's points to the bones nearest "
                                        "them at rest. Default true; false only sets their "
                                        "skeleton.")},
        {"bones_per_point", std::move(per_point)},
    };
    schema["required"] = json::Array{"id"};
    registry.add({
        .name = "skeleton.bind",
        .description = "Binds Polygon2Ds to a Skeleton2D so its Bone2Ds bend them: sets the "
                       "bones' rest to their pose, then weights each point to the bones nearest "
                       "it. Returns how many bones rested and each polygon's bones.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = std::move(schema),
        .handler = skeleton_bind,
    });
}

} // namespace slash::command::detail
