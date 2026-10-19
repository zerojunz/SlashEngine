#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <functional>
#include <map>
#include <memory>
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

// Reads a file by its path relative to the project folder. Worlds use one to load prefabs.
using FileReader = std::function<Result<std::string>(std::string_view path)>;

// Links an entity to the .prefab.json or .scene.json file it extends, shared by all its instances.
// For a prefab, base holds its components, resolved through every "extends"; saving writes only
// the fields that differ from them. For a scene, base holds what an instance brings, in the
// scene's own IDs: {"root": "<id>", "entities": {"<id>": {<components>}}}, with the scenes its
// entities extend in turn flattened in under their IDs (see resolve_prefab).
struct PrefabLink {
    std::string path;
    std::shared_ptr<const json::Value> base;
};

// The prefab each entity extends, by entity ID.
using PrefabLinks = std::map<std::string, PrefabLink, std::less<>>;

// A system runs once per fixed tick. Systems run in order of priority, then in the order they
// were added.
using System = std::function<void(World& world, double tick_seconds)>;

struct SystemOptions {
    // What the system does, one sentence. engine.describe returns it.
    std::string doc{}; // NOLINT(readability-redundant-member-init)
    // Lower priorities run first, as with Godot's process_priority.
    int priority = 0;
};

// Something that happened in the world, for events.subscribe: an entity created or destroyed, or
// an event gameplay code emitted with World::emit.
struct Event {
    std::string kind;
    std::string id;   // the entity it is about, or ""
    json::Value data; // details, or null
    std::uint64_t tick = 0;
};
using EventListener = std::function<void(const Event& event)>;

// The kinds of event the world emits itself. Gameplay events may use any other kind.
inline constexpr std::string_view kEntityCreated = "entity.created";
inline constexpr std::string_view kEntityDestroyed = "entity.destroyed";

// A system as engine.describe lists it.
struct SystemInfo {
    std::string_view name;
    std::string_view doc;
    int priority = 0;
};

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

    // Creates an entity an instanced scene brings. Its ID is the instance's ID, '/' and its ID in
    // the scene, such as "hero/sprite" (or "hero/gun/muzzle" through a scene the scene
    // instances), and it is destroyed with its instance.
    Result<entt::entity> create_part(std::string_view id);

    // Creates an entity with the next free generated ID: "entity_1", "entity_2", ...
    entt::entity create();

    // Destroys an entity and all its components, and the entities its instanced scene brought
    // (those whose IDs start with its ID and '/'). Does nothing for an invalid entity.
    void destroy(entt::entity entity);

    // Destroys every entity and forgets the scene's root. Systems and the tick counter are kept.
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

    // Adds a system. Names must be unique; IDs such as "movement" or "physics2d" read best.
    void add_system(std::string name, System system, SystemOptions options = {});
    // Removes a system, as the engine does when a game's code is reloaded. False if no system has
    // this name.
    bool remove_system(std::string_view name);
    // Every system, in the order they run.
    [[nodiscard]] std::vector<SystemInfo> systems() const;
    [[nodiscard]] std::vector<std::string_view> system_names() const;

    // Runs every system once and advances the tick counter.
    void step(double tick_seconds);
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    // True while step runs the systems, so code can tell their work from the platform's and
    // commands'.
    [[nodiscard]] bool stepping() const noexcept { return stepping_; }
    // Called at the start of each step, before the systems, with the tick's length; for
    // recording replays. An empty listener stops.
    using StepListener = std::function<void(World& world, double tick_seconds)>;
    void set_step_listener(StepListener listener) { step_listener_ = std::move(listener); }

    // What systems keep between ticks outside components (held input, focus, particles, physics
    // contacts, playing sounds) is forgotten by reset_state, so the world plays on as a new world
    // would with the same entities. Replays start so. Modules that keep such state add a reset
    // when they install.
    void add_reset(std::function<void(World&)> reset);
    void reset_state();

    // The counters a saved scene leaves out. Sessions put them back when they roll back a failed
    // command or a dry run, so the next generated ID and tick are the same as if it never ran.
    struct Counters {
        std::uint64_t tick = 0;
        std::uint64_t next_generated_id = 1;
    };
    [[nodiscard]] Counters counters() const noexcept { return {tick_, next_generated_id_}; }
    void set_counters(const Counters& counters) noexcept {
        tick_ = counters.tick;
        next_generated_id_ = counters.next_generated_id;
    }

    // Emits a gameplay event, such as emit("player_died", "player", {{"cause", "lava"}}). Kinds
    // starting with "entity." and the kind "command" are the engine's own.
    void emit(std::string kind, std::string_view id = {}, json::Value data = {});
    // The one function that receives every event: entity.created and entity.destroyed as entities
    // with IDs come and go, and emitted events. Sessions set it to serve events.subscribe.
    void set_event_listener(EventListener listener) { listener_ = std::move(listener); }

    // Where prefab files come from. Without a reader, entities cannot extend prefabs.
    void set_file_reader(FileReader reader) { reader_ = std::move(reader); }
    [[nodiscard]] const FileReader& file_reader() const noexcept { return reader_; }
    // The prefab an entity extends, or nullptr.
    [[nodiscard]] const PrefabLink* prefab_of(entt::entity entity) const;
    // The prefab each entity with an ID extends.
    [[nodiscard]] PrefabLinks prefab_links() const;
    // Prefabs resolved so far, by path, so every instance shares one copy (see resolve_prefab).
    // Sessions forget them before each command, so edited prefab files are read again.
    [[nodiscard]] std::shared_ptr<const json::Value> cached_prefab(std::string_view path) const;
    void cache_prefab(std::string path, std::shared_ptr<const json::Value> components);
    void forget_prefabs() { prefabs_.clear(); }

    // The entity an instance of this world's scene becomes when another scene extends it, as
    // scene files name it in "root"; "" for none. Loading a scene with a root sets it.
    [[nodiscard]] const std::string& scene_root() const noexcept { return scene_root_; }
    void set_scene_root(std::string id) { scene_root_ = std::move(id); }

    // IDs are 1 to 64 characters, each a letter, digit, '_' or '-'.
    [[nodiscard]] static bool is_valid_id(std::string_view id);
    // The ID of an entity an instanced scene brings: two or more valid IDs joined by '/', at most
    // 255 characters in all.
    [[nodiscard]] static bool is_valid_part_id(std::string_view id);

private:
    struct NamedSystem {
        std::string name;
        System run;
        SystemOptions options;
    };

    Result<entt::entity> add(std::string_view id);
    void on_id_destroyed(entt::registry& registry, entt::entity entity);
    void notify(std::string kind, std::string_view id, json::Value data);

    const ComponentRegistry* components_;
    // Declared before registry_ so it outlives the registry's destruction signals.
    std::map<std::string, entt::entity, std::less<>> by_id_;
    entt::registry registry_;
    std::vector<NamedSystem> systems_;
    std::vector<std::function<void(World&)>> resets_;
    StepListener step_listener_;
    EventListener listener_;
    FileReader reader_;
    std::map<std::string, std::shared_ptr<const json::Value>, std::less<>> prefabs_;
    std::string scene_root_;
    std::uint64_t tick_ = 0;
    std::uint64_t next_generated_id_ = 1;
    bool stepping_ = false;
};

} // namespace slash::world
