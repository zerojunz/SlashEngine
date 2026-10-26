#include "slash/command/place.hpp"

#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/path.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Scene assembly: entity.place puts entities where an agent asks, then drops each onto what is
// below it as Godot's editor snaps a node to the floor (node_3d_editor_plugin.cpp's
// _snap_selected_nodes_to_floor: a ray from a meter above the bottom of its bounds, down at most
// 500 meters) and moves it aside to the nearest spot where its bounds overlap nothing. What is
// below and what is in the way are the meshes and static bodies navigation bakes from
// (navigation::world_geometry), so a floor needs no collision shape to be stood on.
namespace slash::command {
namespace {

using world::Affine3D;

constexpr float kSnapHeight = 500.0F; // Godot's max_snap_height
constexpr float kSnapMargin = 1.0F;   // Godot casts from this far above the bottom
constexpr float kTouch = 1.0e-3F;     // boxes sharing less than this don't overlap
constexpr int kRings = 16;            // rings of spots tried around one that is taken
constexpr float kMinStep = 0.05F;
constexpr std::int64_t kMaxEntities = 256;
constexpr int kMaxCells = 256; // per side of the grid finding what is below a point

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value point_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = 3;
    schema["maxItems"] = 3;
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

std::string name_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? std::string("an entity with no id") : quoted(id);
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

// A number rounded to four decimals, with no ".0" on whole ones, so replies stay short.
json::Value short_number(float value) {
    constexpr double kScale = 10000.0;
    const double out = std::round(static_cast<double>(value) * kScale) / kScale;
    if (out == std::floor(out) && std::abs(out) < 1.0e15) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

json::Value point_value(Vec3 point) {
    return json::Array{short_number(point.x), short_number(point.y), short_number(point.z)};
}

Vec3 vec3_of(const json::Value& value) {
    const json::Array& xyz = value.as_array();
    return {static_cast<float>(xyz[0].as_double()), static_cast<float>(xyz[1].as_double()),
            static_cast<float>(xyz[2].as_double())};
}

bool flag(const json::Value& params, std::string_view key, bool fallback) {
    const json::Value* value = params.find(key);
    return value == nullptr ? fallback : value->as_bool();
}

struct Box {
    Vec3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
             std::numeric_limits<float>::lowest()};

    [[nodiscard]] bool empty() const { return min.x > max.x; }
    void add(Vec3 point) {
        min = {std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)};
        max = {std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)};
    }
};

// Whether a overlaps b grown by margin across the ground, sharing more than kTouch on each axis,
// so boxes that only touch, such as a crate on a floor, don't overlap.
bool overlaps(const Box& a, const Box& b, float margin) {
    return a.min.x < b.max.x + margin - kTouch && b.min.x - margin < a.max.x - kTouch &&
           a.min.y < b.max.y - kTouch && b.min.y < a.max.y - kTouch &&
           a.min.z < b.max.z + margin - kTouch && b.min.z - margin < a.max.z - kTouch;
}

// The triangles things stand on, in cells across the ground, so a point finds what is below it
// by looking at a few.
class Ground {
public:
    struct Triangle {
        Vec3 a;
        Vec3 b;
        Vec3 c;
        entt::entity owner = entt::null;
    };
    struct Hit {
        float y = 0.0F;
        entt::entity owner = entt::null;
    };

    explicit Ground(std::vector<Triangle> triangles) : triangles_(std::move(triangles)) {
        if (triangles_.empty()) {
            return;
        }
        Box bounds;
        for (const Triangle& triangle : triangles_) {
            bounds.add(triangle.a);
            bounds.add(triangle.b);
            bounds.add(triangle.c);
        }
        min_x_ = bounds.min.x;
        min_z_ = bounds.min.z;
        const double per_side = std::ceil(std::sqrt(static_cast<double>(triangles_.size()) / 2.0));
        cells_ = std::clamp(static_cast<int>(per_side), 1, kMaxCells);
        size_x_ = std::max((bounds.max.x - bounds.min.x) / static_cast<float>(cells_), 1.0e-3F);
        size_z_ = std::max((bounds.max.z - bounds.min.z) / static_cast<float>(cells_), 1.0e-3F);
        grid_.resize(static_cast<std::size_t>(cells_) * static_cast<std::size_t>(cells_));
        for (std::size_t i = 0; i < triangles_.size(); ++i) {
            const Triangle& t = triangles_[i];
            const int x0 = cell(std::min({t.a.x, t.b.x, t.c.x}), min_x_, size_x_);
            const int x1 = cell(std::max({t.a.x, t.b.x, t.c.x}), min_x_, size_x_);
            const int z0 = cell(std::min({t.a.z, t.b.z, t.c.z}), min_z_, size_z_);
            const int z1 = cell(std::max({t.a.z, t.b.z, t.c.z}), min_z_, size_z_);
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) {
                    grid_[index(x, z)].push_back(i);
                }
            }
        }
    }

    // The highest point of a triangle straight below from, at most depth down; the first
    // triangle of those as high.
    [[nodiscard]] std::optional<Hit> below(Vec3 from, float depth) const {
        if (triangles_.empty()) {
            return std::nullopt;
        }
        const float fx = (from.x - min_x_) / size_x_;
        const float fz = (from.z - min_z_) / size_z_;
        if (fx < 0.0F || fz < 0.0F || fx > static_cast<float>(cells_) ||
            fz > static_cast<float>(cells_)) {
            return std::nullopt;
        }
        std::optional<Hit> best;
        for (const std::size_t i :
             grid_[index(cell(from.x, min_x_, size_x_), cell(from.z, min_z_, size_z_))]) {
            const std::optional<float> y = height(triangles_[i], from.x, from.z);
            if (!y || *y > from.y || *y < from.y - depth || (best && *y <= best->y)) {
                continue;
            }
            best = Hit{.y = *y, .owner = triangles_[i].owner};
        }
        return best;
    }

private:
    [[nodiscard]] int cell(float value, float start, float size) const {
        const auto at = static_cast<int>(std::floor((value - start) / size));
        return std::clamp(at, 0, cells_ - 1);
    }
    [[nodiscard]] std::size_t index(int x, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(cells_)) +
               static_cast<std::size_t>(x);
    }

    // The triangle's height at x and z, if they are over it; none for a wall seen edge on.
    static std::optional<float> height(const Triangle& t, float x, float z) {
        const double ax = t.a.x;
        const double az = t.a.z;
        const double bx = t.b.x;
        const double bz = t.b.z;
        const double cx = t.c.x;
        const double cz = t.c.z;
        const double d = ((bz - cz) * (ax - cx)) + ((cx - bx) * (az - cz));
        constexpr double kFlat = 1.0e-12;
        if (std::abs(d) < kFlat) {
            return std::nullopt;
        }
        const double u = (((bz - cz) * (x - cx)) + ((cx - bx) * (z - cz))) / d;
        const double v = (((cz - az) * (x - cx)) + ((ax - cx) * (z - cz))) / d;
        const double w = 1.0 - u - v;
        constexpr double kEdge = -1.0e-9;
        if (u < kEdge || v < kEdge || w < kEdge) {
            return std::nullopt;
        }
        return static_cast<float>((u * t.a.y) + (v * t.b.y) + (w * t.c.y));
    }

    std::vector<Triangle> triangles_;
    std::vector<std::vector<std::size_t>> grid_;
    float min_x_ = 0.0F;
    float min_z_ = 0.0F;
    float size_x_ = 1.0F;
    float size_z_ = 1.0F;
    int cells_ = 1;
};

struct Target {
    entt::entity entity = entt::null;
    std::string id;
    std::optional<Vec3> at;
    std::vector<Vec3> points; // its and its children's geometry, in its own space
};

// Something in the way: an entity's bounds where it is.
struct Obstacle {
    Box box;
    entt::entity owner = entt::null;
};

// The target under transform_parent links that moves an entity: the entity itself, or the
// nearest above it; -1 for none.
int target_moving(const world::Tree3D& tree, const std::unordered_map<entt::entity, int>& targets,
                  entt::entity entity) {
    for (entt::entity at = entity; at != entt::null; at = tree.transform_parent(at)) {
        if (const auto it = targets.find(at); it != targets.end()) {
            return it->second;
        }
    }
    return -1;
}

Box box_of(const std::vector<Vec3>& points, const Affine3D& place) {
    Box box;
    for (const Vec3 point : points) {
        box.add(place.apply(point));
    }
    if (box.empty()) {
        box.add(place.origin);
    }
    return box;
}

// The rotation basis without the transform's origin and scale.
Affine3D rotation_of(const Affine3D& transform) {
    Affine3D out = world::orthonormalized(transform);
    out.origin = {};
    return out;
}

Reply entity_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::registry& registry = world.ecs();
    const world::Tree3D tree(world);

    std::vector<Target> targets;
    std::unordered_map<entt::entity, int> index_of;
    const json::Array& entries = params.find("entities")->as_array();
    const json::Value* along = params.find("along");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const json::Value& entry = entries[i];
        const std::string path = "/entities/" + std::to_string(i);
        Target target;
        if (entry.is_string()) {
            target.id = entry.as_string();
        } else {
            target.id = entry.find("id")->as_string();
            if (const json::Value* at = entry.find("at"); at != nullptr) {
                if (along != nullptr) {
                    return param_error(code::kInvalidParams, path + "/at",
                                       "along places it on the path; leave at out");
                }
                target.at = vec3_of(*at);
            }
        }
        const std::string id_path = entry.is_string() ? path : path + "/id";
        target.entity = world.find(target.id);
        if (target.entity == entt::null) {
            return unknown_entity(world, target.id, id_path);
        }
        if (!registry.all_of<world::Transform3D>(target.entity)) {
            return param_error(code::kInvalidParams, id_path,
                               quoted(target.id) + " has no Transform3D to place it by");
        }
        if (!index_of.emplace(target.entity, static_cast<int>(i)).second) {
            return param_error(code::kInvalidParams, id_path,
                               quoted(target.id) + " is listed twice");
        }
        targets.push_back(std::move(target));
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const entt::entity parent = tree.transform_parent(targets[i].entity);
        const int above = parent == entt::null ? -1 : target_moving(tree, index_of, parent);
        if (above >= 0) {
            return param_error(code::kInvalidParams, "/entities/" + std::to_string(i),
                               quoted(targets[i].id) + " is under " +
                                   quoted(targets[static_cast<std::size_t>(above)].id) +
                                   ", which moves it; place one of them");
        }
    }

    std::shared_ptr<const world::Curve3D> curve;
    Affine3D path_place;
    float start = 0.0F;
    std::optional<float> spacing;
    bool closed = false;
    const bool rotate = along != nullptr && flag(*along, "rotate", false);
    if (along != nullptr) {
        const std::string& path_id = along->find("path")->as_string();
        const entt::entity path = world.find(path_id);
        if (path == entt::null) {
            return unknown_entity(world, path_id, "/along/path");
        }
        curve = world::curve_3d(world, path);
        if (!curve) {
            return param_error(code::kInvalidParams, "/along/path",
                               quoted(path_id) + " has no Path3D");
        }
        if (curve->length() <= 0.0F) {
            return param_error(code::kInvalidArgument, "/along/path",
                               quoted(path_id) +
                                   "'s curve has no length: give its Path3D two points or more");
        }
        closed = registry.get<world::Path3D>(path).closed;
        path_place = tree.global(path);
        if (const json::Value* from = along->find("from"); from != nullptr) {
            start = static_cast<float>(from->as_double());
        }
        if (const json::Value* each = along->find("spacing"); each != nullptr) {
            spacing = static_cast<float>(each->as_double());
        }
    }
    // What may be stood on: the entities on names and those under them, else anything.
    std::unordered_map<entt::entity, int> stands;
    if (const json::Value* on = params.find("on"); on != nullptr) {
        const json::Array& ids = on->as_array();
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string& id = ids[i].as_string();
            const entt::entity entity = world.find(id);
            if (entity == entt::null) {
                return unknown_entity(world, id, "/on/" + std::to_string(i));
            }
            stands.emplace(entity, static_cast<int>(i));
        }
    }
    const bool ground_on = flag(params, "ground", true);
    const bool apart = flag(params, "apart", true);
    const json::Value* margin_value = params.find("margin");
    const float margin =
        margin_value == nullptr ? 0.0F : static_cast<float>(margin_value->as_double());

    // What is there: each entity's own geometry, as the targets' points where it moves with one,
    // else as ground and obstacles.
    std::vector<std::string> warnings;
    const navigation::NavigationMesh settings;
    std::vector<Ground::Triangle> triangles;
    std::vector<Obstacle> obstacles;
    for (const navigation::EntityGeometry& each :
         navigation::world_geometry(world, session.assets(), settings, &warnings)) {
        const std::vector<float>& vertices = each.geometry.vertices;
        const auto vertex = [&](int i) {
            const auto at = static_cast<std::size_t>(i) * 3;
            return Vec3{vertices[at], vertices[at + 1], vertices[at + 2]};
        };
        const int moving = target_moving(tree, index_of, each.entity);
        if (moving >= 0) {
            Target& target = targets[static_cast<std::size_t>(moving)];
            const Affine3D to_local = tree.global(target.entity).inverse();
            for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
                target.points.push_back(
                    to_local.apply({vertices[i], vertices[i + 1], vertices[i + 2]}));
            }
            continue;
        }
        const bool standable = stands.empty() || target_moving(tree, stands, each.entity) >= 0;
        Obstacle obstacle{.box = {}, .owner = each.entity};
        const std::vector<int>& indices = each.geometry.indices;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const Ground::Triangle triangle{.a = vertex(indices[i]),
                                            .b = vertex(indices[i + 1]),
                                            .c = vertex(indices[i + 2]),
                                            .owner = each.entity};
            obstacle.box.add(triangle.a);
            obstacle.box.add(triangle.b);
            obstacle.box.add(triangle.c);
            if (standable) {
                triangles.push_back(triangle);
            }
        }
        obstacles.push_back(obstacle);
    }
    const Ground ground(std::move(triangles));

    json::Array placed_out;
    json::Array problems;
    const auto problem = [&](const Target& target, std::string message) {
        json::Value each;
        each["id"] = target.id;
        each["message"] = std::move(message);
        problems.emplace_back(std::move(each));
    };
    std::vector<Obstacle> placed;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const Target& target = targets[i];
        const entt::entity parent_entity = tree.transform_parent(target.entity);
        const Affine3D parent =
            parent_entity == entt::null ? Affine3D{} : tree.global(parent_entity);
        const Affine3D to_parent = parent.inverse();
        world::Transform3D local = registry.get<world::Transform3D>(target.entity);
        Vec3 origin = (parent * world::affine(local)).origin;
        if (curve) {
            float offset = start;
            if (spacing) {
                offset += *spacing * static_cast<float>(i);
            } else if (targets.size() > 1) {
                const auto parts = static_cast<float>(targets.size() - (closed ? 0 : 1));
                offset += curve->length() * static_cast<float>(i) / parts;
            }
            if (closed) {
                offset = std::fmod(offset, curve->length());
            } else if (offset > curve->length() + kTouch) {
                problem(target, "it would be past the end of the path, so it was left where it "
                                "was; give along a smaller spacing or a closed path");
                continue;
            }
            origin = path_place.apply(curve->sample(offset));
            if (rotate) {
                const Affine3D posture = curve->sample_with_rotation(offset);
                const Vec3 ahead = path_place.basis(posture.z) * -1.0F;
                const float yaw = slash::atan2(-ahead.x, -ahead.z);
                const Affine3D turned = world::rotation_basis(
                    world::quaternion({local.rotation.x, yaw, local.rotation.z}));
                local.rotation = world::euler(rotation_of(rotation_of(parent).inverse() * turned));
            }
        } else if (target.at) {
            origin = *target.at;
        }

        // Where it goes with its origin at a point: dropped onto what is below, and what it
        // stands on.
        struct Spot {
            Vec3 origin;
            Box box;
            entt::entity on = entt::null;
            bool grounded = false;
        };
        const auto spot_at = [&](Vec3 at) {
            Spot spot;
            local.position = to_parent.apply(at);
            spot.origin = at;
            spot.box = box_of(target.points, parent * world::affine(local));
            if (!ground_on) {
                return spot;
            }
            const Vec3 from{(spot.box.min.x + spot.box.max.x) * 0.5F, spot.box.min.y + kSnapMargin,
                            (spot.box.min.z + spot.box.max.z) * 0.5F};
            if (const auto hit = ground.below(from, kSnapHeight); hit) {
                const float rise = hit->y - spot.box.min.y;
                spot.origin.y += rise;
                spot.box.min.y += rise;
                spot.box.max.y += rise;
                spot.on = hit->owner;
                spot.grounded = true;
            }
            return spot;
        };
        const auto in_way = [&](const Spot& spot) -> std::optional<entt::entity> {
            for (const Obstacle& obstacle : obstacles) {
                if (obstacle.owner != spot.on && overlaps(spot.box, obstacle.box, margin)) {
                    return obstacle.owner;
                }
            }
            for (const Obstacle& other : placed) {
                if (overlaps(spot.box, other.box, margin)) {
                    return other.owner;
                }
            }
            return std::nullopt;
        };

        Spot spot = spot_at(origin);
        float moved = 0.0F;
        if (apart) {
            if (const std::optional<entt::entity> blocker = in_way(spot); blocker) {
                const float step = std::max(
                    (std::max(spot.box.max.x - spot.box.min.x, spot.box.max.z - spot.box.min.z) *
                     0.5F) +
                        margin,
                    kMinStep);
                std::optional<Spot> free;
                for (int ring = 1; ring <= kRings && !free; ++ring) {
                    const int count = 8 * ring;
                    for (int k = 0; k < count && !free; ++k) {
                        const Vec2 way = cos_sin(2.0F * std::numbers::pi_v<float> *
                                                 static_cast<float>(k) / static_cast<float>(count));
                        const float reach = step * static_cast<float>(ring);
                        const Spot tried = spot_at(
                            {origin.x + (way.x * reach), origin.y, origin.z + (way.y * reach)});
                        if ((!ground_on || tried.grounded) && !in_way(tried)) {
                            free = tried;
                            moved = reach;
                        }
                    }
                }
                if (free) {
                    spot = *free;
                } else {
                    problem(target, "it overlaps " + name_of(world, *blocker) +
                                        " and no spot within " +
                                        json::write(short_number(step * kRings)) +
                                        " m of where it was asked to go is free");
                }
            }
        }
        if (ground_on && !spot.grounded) {
            problem(target, "nothing is below it to stand on within 500 m");
        }
        local.position = to_parent.apply(spot.origin);
        registry.replace<world::Transform3D>(target.entity, local);
        placed.push_back({.box = spot.box, .owner = target.entity});

        json::Value out;
        out["id"] = target.id;
        out["position"] = point_value(spot.origin);
        if (rotate) {
            out["rotation"] = point_value(local.rotation);
        }
        if (spot.on != entt::null) {
            const std::string_view on = world.id_of(spot.on);
            out["on"] = on.empty() ? json::Value() : json::Value(on);
        }
        if (moved > 0.0F) {
            out["moved"] = short_number(moved);
        }
        placed_out.emplace_back(std::move(out));
    }

    json::Value result;
    result["placed"] = std::move(placed_out);
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    if (!warnings.empty()) {
        json::Array list;
        for (std::string& warning : warnings) {
            list.emplace_back(std::move(warning));
        }
        result["warnings"] = std::move(list);
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_place_commands(Registry& registry) {
    using enum Capability;
    json::Value id = property("string", "An entity with a Transform3D.");
    id["minLength"] = 1;
    json::Value entry = property("object", "An entity to place, at a point in the world.");
    entry["additionalProperties"] = false;
    entry["required"] = json::Array{"id"};
    entry["properties"]["id"] = id;
    entry["properties"]["at"] =
        point_property("Where its origin goes, [x, y, z] in the world, before it is dropped. "
                       "Default where it is.");
    json::Value item;
    item["oneOf"] = json::Array{id, std::move(entry)};
    json::Value entities = property("array", "Entities to place, in order: each keeps clear of "
                                             "those before it. An ID, or {id, at}.");
    entities["items"] = std::move(item);
    entities["minItems"] = 1;
    entities["maxItems"] = kMaxEntities;

    json::Value path = property("string", "An entity with a Path3D.");
    path["minLength"] = 1;
    json::Value from = property("number", "Meters along the curve to the first. Default 0.");
    from["minimum"] = 0;
    json::Value spacing = property("number", "Meters along the curve between each. Default "
                                             "spread evenly from end to end.");
    spacing["exclusiveMinimum"] = 0;
    json::Value along = property("object", "Spread the entities along a path's curve instead.");
    along["additionalProperties"] = false;
    along["required"] = json::Array{"path"};
    along["properties"]["path"] = std::move(path);
    along["properties"]["from"] = std::move(from);
    along["properties"]["spacing"] = std::move(spacing);
    along["properties"]["rotate"] =
        property("boolean", "Turn each about y to face along the path (-z ahead). Default false.");
    json::Value margin =
        property("number", "Meters kept clear across the ground between bounds. Default 0.");
    margin["minimum"] = 0;

    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"entities"};
    schema["properties"]["entities"] = std::move(entities);
    schema["properties"]["along"] = std::move(along);
    schema["properties"]["ground"] =
        property("boolean", "Drop each onto the mesh or static body below it, as Godot's Snap "
                            "Object to Floor. Default true.");
    json::Value on = property("array", "Entities it may stand on, with those under them. "
                                       "Default anything.");
    on["items"] = property("string", "An entity ID.");
    on["items"]["minLength"] = 1;
    on["minItems"] = 1;
    schema["properties"]["on"] = std::move(on);
    schema["properties"]["apart"] =
        property("boolean", "Move each aside to the nearest spot where its bounds overlap "
                            "nothing but what it stands on. Default true.");
    schema["properties"]["margin"] = std::move(margin);
    registry.add({
        .name = "entity.place",
        .description = "Places 3D entities with constraints: at a point or along a path, on the "
                       "ground and not overlapping. Returns each one's position, what it stands "
                       "on and how far it was moved aside, and problems.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = std::move(schema),
        .handler = entity_place,
    });
}

} // namespace slash::command
