// hair.get: a HairInstance3D's or Hair2D's groom as it grew and where its guides are now
// (slash/world/hair.hpp, slash/scene3d/hair.hpp).
#include "slash/scene3d/hair.hpp"

#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/world/hair.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {

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

double rounded(real_t v) {
    return std::round(static_cast<double>(v) * 1000.0) / 1000.0;
}

json::Value point_of(Vec3 at, bool flat) {
    json::Array point{rounded(at.x), rounded(at.y)};
    if (!flat) {
        point.emplace_back(rounded(at.z));
    }
    return point;
}

real_t distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// What hair.get says of guides (points each) against where the groom puts them (targets).
void describe_guides(json::Value& out, const std::vector<Vec3>& guides,
                     const std::vector<Vec3>& targets, std::size_t points, bool flat,
                     bool every_point) {
    if (points == 0 || guides.empty()) {
        return;
    }
    Vec3 low = guides.front();
    Vec3 high = guides.front();
    for (const Vec3 p : guides) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    out["bounds"]["min"] = point_of(low, flat);
    out["bounds"]["max"] = point_of(high, flat);
    // How far each tip is from its groomed place: the wind, gravity and motion in it.
    real_t moved = 0.0F;
    real_t sum = 0.0F;
    json::Array tips;
    json::Array all;
    const std::size_t count = guides.size() / points;
    for (std::size_t g = 0; g < count; ++g) {
        const std::size_t tip = (g * points) + points - 1;
        tips.push_back(point_of(guides[tip], flat));
        if (tip < targets.size()) {
            const real_t d = distance(guides[tip], targets[tip]);
            moved = std::max(moved, d);
            sum += d;
        }
        if (every_point) {
            json::Array strand;
            for (std::size_t k = 0; k < points; ++k) {
                strand.emplace_back(point_of(guides[(g * points) + k], flat));
            }
            all.emplace_back(std::move(strand));
        }
    }
    out["tip_moved"]["max"] = rounded(moved);
    out["tip_moved"]["mean"] = rounded(count > 0 ? sum / static_cast<real_t>(count) : 0.0F);
    out["tips"] = std::move(tips);
    if (every_point) {
        out["points"] = std::move(all);
    }
}

void describe_groom(json::Value& out, const std::string& path, const world::Groom& groom) {
    out["groom"] = path;
    out["render"] = groom.render;
}

Reply hair_get_3d(world::World& world, entt::entity entity, bool every_point) {
    const auto& hair = world.ecs().get<const world::HairInstance3D>(entity);
    auto grown = scene3d::hair_growth(world, entity);
    if (!grown) {
        return param_error(code::kInvalidArgument, "/entity",
                           quoted(world.id_of(entity)) +
                               "'s hair can't grow: " + grown.error().message);
    }
    const scene3d::HairGrowth3D& growth = *grown.value();
    json::Value out;
    describe_groom(out, hair.groom, *growth.groom);
    out["guides"] = static_cast<std::int64_t>(growth.guides.size());
    out["strands"] = static_cast<std::int64_t>(growth.strands.size());
    out["points"] = static_cast<std::int64_t>(growth.points);
    out["area"] = rounded(growth.area);
    const world::Tree3D tree(world);
    std::vector<world::Affine3D> frames;
    std::vector<Vec3> guides;
    const world::HairStrands* simulated = world::hair_strands_3d(world, entity);
    const bool stepped = simulated != nullptr && simulated->current.size() == growth.rest.size() &&
                         simulated->frames.size() == growth.guides.size();
    if (stepped) {
        frames = simulated->frames;
        guides = simulated->current;
    } else {
        frames = scene3d::hair_frames(world, tree, entity, growth);
        guides = scene3d::hair_targets(growth, frames);
    }
    out["simulated"] = stepped;
    json::Value where;
    describe_guides(where, guides, scene3d::hair_targets(growth, frames),
                    static_cast<std::size_t>(growth.points), false, every_point);
    for (auto& [key, value] : where.as_object()) {
        if (key == "points") {
            out["guide_points"] = std::move(value);
        } else {
            out[key] = std::move(value);
        }
    }
    return Reply::success(std::move(out));
}

Reply hair_get_2d(world::World& world, entt::entity entity, bool every_point) {
    const auto& hair = world.ecs().get<const world::Hair2D>(entity);
    auto grown = world::hair_growth_2d(world, entity);
    if (!grown) {
        return param_error(code::kInvalidArgument, "/entity",
                           quoted(world.id_of(entity)) +
                               "'s hair can't grow: " + grown.error().message);
    }
    const world::Groom& groom = *grown.value().groom;
    const world::HairGrowth2D& growth = *grown.value().growth;
    const auto points = static_cast<std::size_t>(std::max(groom.segments, 1) + 1);
    json::Value out;
    describe_groom(out, hair.groom, groom);
    out["guides"] = static_cast<std::int64_t>(growth.guides.size());
    out["strands"] = static_cast<std::int64_t>(growth.strands.size());
    out["points"] = static_cast<std::int64_t>(points);
    const world::Tree2D tree(world);
    const world::Affine2D place = tree.global_affine(entity);
    std::vector<Vec3> targets;
    for (const world::HairGuide2D& guide : growth.guides) {
        for (const Vec2 point : guide.rest) {
            const Vec2 at = place.apply(point);
            targets.push_back({at.x, at.y, 0.0F});
        }
    }
    const world::HairStrands* simulated = world::hair_strands_2d(world, entity);
    const bool stepped = simulated != nullptr && simulated->current.size() == targets.size();
    out["simulated"] = stepped;
    json::Value where;
    describe_guides(where, stepped ? simulated->current : targets, targets, points, true,
                    every_point);
    for (auto& [key, value] : where.as_object()) {
        if (key == "points") {
            out["guide_points"] = std::move(value);
        } else {
            out[key] = std::move(value);
        }
    }
    return Reply::success(std::move(out));
}

Reply hair_get(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = params.find("entity")->as_string();
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        std::string message = "no entity has the id " + quoted(id);
        if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        return param_error(code::kNotFound, "/entity", message);
    }
    const json::Value* every = params.find("points");
    const bool every_point = every != nullptr && every->as_bool();
    if (world.ecs().all_of<world::HairInstance3D>(entity)) {
        return hair_get_3d(world, entity, every_point);
    }
    if (world.ecs().all_of<world::Hair2D>(entity)) {
        return hair_get_2d(world, entity, every_point);
    }
    return param_error(code::kInvalidArgument, "/entity",
                       quoted(id) + " has no HairInstance3D or Hair2D");
}

} // namespace

void register_hair_commands(Registry& registry) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"entity"};
    json::Value& p = schema["properties"];
    p["entity"] = property("string", "ID of the entity with the HairInstance3D or Hair2D.");
    p["points"] = property("boolean", "Also every guide's points, root first.");
    registry.add({
        .name = "hair.get",
        .description = "A HairInstance3D's or Hair2D's groom as grown (guides, strands, points "
                       "each) and its guides now: bounds, tips and how far the tips moved from "
                       "the groomed shape. $ slash-cli schema groom.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = hair_get,
    });
}

} // namespace slash::command
