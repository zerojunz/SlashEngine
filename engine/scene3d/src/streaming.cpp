#include "slash/scene3d/streaming.hpp"

#include "slash/core/json.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace slash::scene3d {

namespace {

using world::StreamedScene3D;
using world::StreamingSource3D;

constexpr std::string_view kSceneSuffix = ".scene.json";
constexpr std::string_view kRootSuffix = "_scene";

// What the system keeps between ticks: the scene each StreamedScene3D failed to load, by entity,
// so it says so once rather than every tick.
struct StreamingState {
    std::map<entt::entity, std::string> failed;
};

std::vector<entt::entity> made_order(const world::World& world) {
    std::vector<entt::entity> cells;
    for (const entt::entity entity : world.ecs().view<const StreamedScene3D>()) {
        cells.push_back(entity);
    }
    std::ranges::sort(cells, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return cells;
}

// The enabled sources' global positions, in the order their entities were made.
std::vector<Vec3> source_points(const world::World& world, const world::Tree3D& tree) {
    std::vector<entt::entity> sources;
    for (const auto [entity, source] : world.ecs().view<const StreamingSource3D>().each()) {
        if (source.enabled) {
            sources.push_back(entity);
        }
    }
    std::ranges::sort(sources, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    std::vector<Vec3> points;
    points.reserve(sources.size());
    for (const entt::entity entity : sources) {
        points.push_back(tree.global(entity).origin);
    }
    return points;
}

// Meters from a point to a box of size centered on center, along the world's axes.
float box_distance(Vec3 point, Vec3 center, Vec3 size) {
    const auto axis = [](float p, float c, float s) {
        return std::max(std::abs(p - c) - (std::abs(s) * 0.5F), 0.0F);
    };
    const float dx = axis(point.x, center.x, size.x);
    const float dy = axis(point.y, center.y, size.y);
    const float dz = axis(point.z, center.z, size.z);
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

std::optional<float> nearest(const std::vector<Vec3>& sources, Vec3 center, Vec3 size) {
    std::optional<float> best;
    for (const Vec3 source : sources) {
        const float distance = box_distance(source, center, size);
        if (!best || distance < *best) {
            best = distance;
        }
    }
    return best;
}

void remove_child(world::World& world, entt::entity parent, entt::entity child) {
    if (auto* transform = world.ecs().try_get<world::Transform3D>(parent); transform != nullptr) {
        std::erase(transform->children, child);
    }
}

Result<entt::entity> load(world::World& world, entt::entity cell, const std::string& root_id,
                          const std::string& scene) {
    auto created = world.create(root_id);
    if (!created) {
        return created.error();
    }
    const entt::entity root = created.value();
    if (auto made = world::instantiate_prefab(world, root, scene); !made) {
        world.destroy(root);
        return made.error();
    }
    world.ecs().get_or_emplace<world::Transform3D>(cell).children.push_back(root);
    return root;
}

void run(world::World& world, StreamingState& state) {
    const world::Tree3D tree(world);
    const std::vector<Vec3> sources = source_points(world, tree);
    std::erase_if(state.failed, [&](const auto& entry) {
        const auto* cell = world.ecs().try_get<const StreamedScene3D>(entry.first);
        return cell == nullptr || cell->scene != entry.second;
    });
    for (const entt::entity entity : made_order(world)) {
        // Copied: loading a scene adds entities, which can move the storage.
        const StreamedScene3D cell = world.ecs().get<const StreamedScene3D>(entity);
        const std::string id(world.id_of(entity));
        if (id.empty()) {
            continue;
        }
        const std::string root_id = streamed_root_id(id);
        const entt::entity root = world.find(root_id);
        const std::optional<float> distance =
            nearest(sources, tree.global(entity).origin, cell.size);
        const bool near = distance && *distance <= cell.load_radius;
        const bool gone = !distance || *distance > cell.load_radius + cell.unload_margin;
        if (root == entt::null && near && !state.failed.contains(entity)) {
            auto loaded = load(world, entity, root_id, cell.scene);
            if (!loaded) {
                state.failed[entity] = cell.scene;
                world.emit("stream_failed", id,
                           json::Object{{"scene", cell.scene}, {"error", loaded.error().message}});
                continue;
            }
            world.emit("streamed_in", id, json::Object{{"scene", cell.scene}, {"root", root_id}});
        } else if (root != entt::null && gone) {
            remove_child(world, entity, root);
            world.destroy(root);
            world.emit("streamed_out", id, json::Object{{"scene", cell.scene}});
        }
    }
}

} // namespace

std::string streamed_root_id(std::string_view id) {
    return std::string(id) + std::string(kRootSuffix);
}

void add_streaming_system(world::World& world) {
    auto state = std::make_shared<StreamingState>();
    world.add_reset([state](world::World& /*world*/) { state->failed.clear(); });
    world.add_system(
        "world_streaming", [state](world::World& w, double /*tick_seconds*/) { run(w, *state); },
        {.doc = "Loads each StreamedScene3D's scene while a StreamingSource3D is near and unloads "
                "it when they have all gone, as Unreal's World Partition streams cells.",
         .priority = -700});
}

std::vector<StreamedCell> streamed_cells(const world::World& world) {
    const world::Tree3D tree(world);
    const std::vector<Vec3> sources = source_points(world, tree);
    std::vector<StreamedCell> cells;
    for (const entt::entity entity : made_order(world)) {
        const auto& cell = world.ecs().get<const StreamedScene3D>(entity);
        const std::string_view id = world.id_of(entity);
        cells.push_back({.entity = entity,
                         .loaded = !id.empty() && world.find(streamed_root_id(id)) != entt::null,
                         .distance = nearest(sources, tree.global(entity).origin, cell.size)});
    }
    return cells;
}

std::vector<Problem> check_streaming(const world::World& world) {
    std::vector<Problem> problems;
    for (const entt::entity entity : made_order(world)) {
        const auto& cell = world.ecs().get<const StreamedScene3D>(entity);
        const std::string_view id = world.id_of(entity);
        if (!world.ecs().all_of<world::Transform3D>(entity)) {
            problems.push_back({entity, "/components/StreamedScene3D",
                                "needs a Transform3D to place its scene under"});
        }
        if (!id.empty() && !world::World::is_valid_id(streamed_root_id(id))) {
            problems.push_back({entity, "/components/StreamedScene3D",
                                "its scene's root would be \"" + streamed_root_id(id) +
                                    "\", which is too long: give it an ID of at most " +
                                    std::to_string(64 - kRootSuffix.size()) + " characters"});
        }
        if (!cell.scene.ends_with(kSceneSuffix)) {
            problems.push_back({entity, "/components/StreamedScene3D/scene",
                                "\"" + cell.scene + "\" is no .scene.json file"});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto text = reader(cell.scene);
        if (!text) {
            problems.push_back({entity, "/components/StreamedScene3D/scene",
                                "\"" + cell.scene + "\" can't be read: " + text.error().message});
            continue;
        }
        auto parsed = json::parse(text.value());
        const json::Value* root = parsed ? parsed.value().find("root") : nullptr;
        if (root == nullptr || !root->is_string()) {
            problems.push_back({entity, "/components/StreamedScene3D/scene",
                                "\"" + cell.scene +
                                    "\" names no root: save it with scene.save's "
                                    "root"});
        }
    }
    return problems;
}

} // namespace slash::scene3d
