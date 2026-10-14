#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

class ComponentRegistry;
class World;

// The stable string ID of an entity, e.g. "player" or "enemy_03". Scenes key entities by it, and
// entity references in components are saved as it. Every entity created through World has one.
struct EntityId {
    std::string value;
};

// A system runs once per fixed tick, in the order systems were added.
using System = std::function<void(World& world, double tick_seconds)>;

// The simulation state: an EnTT registry plus stable entity IDs and an ordered list of systems.
// Game code reads and writes components through ecs(); entities should be created through
// create() so they get an ID and are saved with the scene.
class World {
public:
    explicit World(const ComponentRegistry& registry);
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] const ComponentRegistry& components() const noexcept { return *components_; }
    [[nodiscard]] entt::registry& ecs() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& ecs() const noexcept { return registry_; }

    // Creates an entity with the given ID. Fails if the ID is malformed or already in use.
    Result<entt::entity> create(std::string_view id);

    // Creates an entity with the next free generated ID: "entity_1", "entity_2", ...
    entt::entity create();

    // Destroys an entity and all its components. Does nothing for an invalid entity.
    void destroy(entt::entity entity);

    // Destroys every entity. Systems and the tick counter are kept.
    void clear();

    [[nodiscard]] bool valid(entt::entity entity) const;

    // The entity with this ID, or entt::null.
    [[nodiscard]] entt::entity find(std::string_view id) const;

    // The ID of an entity, or "" if it is invalid or was created without one.
    [[nodiscard]] std::string_view id_of(entt::entity entity) const;

    // The number of entities with IDs.
    [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }

    // Every entity ID, sorted.
    [[nodiscard]] std::vector<std::string_view> ids() const;

    void add_system(std::string name, System system);
    [[nodiscard]] std::vector<std::string_view> system_names() const;

    // Runs every system once and advances the tick counter.
    void step(double tick_seconds);
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }

    // IDs are 1 to 64 characters, each a letter, digit, '_' or '-'.
    [[nodiscard]] static bool is_valid_id(std::string_view id);

private:
    struct NamedSystem {
        std::string name;
        System run;
    };

    void on_id_destroyed(entt::registry& registry, entt::entity entity);

    const ComponentRegistry* components_;
    // Declared before registry_ so it outlives the registry's destruction signals.
    std::map<std::string, entt::entity, std::less<>> by_id_;
    entt::registry registry_;
    std::vector<NamedSystem> systems_;
    std::uint64_t tick_ = 0;
    std::uint64_t next_generated_id_ = 1;
    bool stepping_ = false;
};

} // namespace slash::world
