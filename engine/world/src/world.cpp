#include "slash/world/world.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace slash::world {

World::World(const ComponentRegistry& registry) : components_(&registry) {
    registry_.on_destroy<EntityId>().connect<&World::on_id_destroyed>(*this);
}

World::~World() {
    registry_.on_destroy<EntityId>().disconnect(this);
}

Result<entt::entity> World::create(std::string_view id) {
    if (!is_valid_id(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid entity id \"" + std::string(id) +
                              "\": use 1 to 64 letters, digits, '_' or '-'");
    }
    return add(id);
}

Result<entt::entity> World::create_part(std::string_view id) {
    if (!is_valid_part_id(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid id \"" + std::string(id) +
                              "\" for an entity an instanced scene brings: use the instance's ID, "
                              "'/' and its ID in the scene, such as \"hero/sprite\"");
    }
    return add(id);
}

Result<entt::entity> World::add(std::string_view id) {
    if (by_id_.contains(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "entity id \"" + std::string(id) + "\" is already in use");
    }
    const entt::entity entity = registry_.create();
    registry_.emplace<EntityId>(entity, std::string(id));
    by_id_.emplace(std::string(id), entity);
    notify(std::string(kEntityCreated), id, {});
    return entity;
}

entt::entity World::create() {
    std::string id;
    do {
        id = "entity_" + std::to_string(next_generated_id_++);
    } while (by_id_.contains(id));
    return create(id).value();
}

void World::destroy(entt::entity entity) {
    if (!registry_.valid(entity)) {
        return;
    }
    // The entities its instanced scene brought go with it.
    std::vector<entt::entity> parts;
    if (const auto* id = registry_.try_get<EntityId>(entity); id != nullptr) {
        const std::string prefix = id->value + "/";
        for (auto it = by_id_.lower_bound(prefix);
             it != by_id_.end() && it->first.starts_with(prefix); ++it) {
            parts.push_back(it->second);
        }
    }
    registry_.destroy(entity);
    for (const entt::entity part : parts) {
        if (registry_.valid(part)) {
            registry_.destroy(part);
        }
    }
}

void World::clear() {
    registry_.clear();
    by_id_.clear();
    scene_root_.clear();
}

bool World::valid(entt::entity entity) const {
    return registry_.valid(entity);
}

entt::entity World::find(std::string_view id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? entt::entity{entt::null} : it->second;
}

std::string_view World::id_of(entt::entity entity) const {
    if (!registry_.valid(entity)) {
        return {};
    }
    const auto* id = registry_.try_get<EntityId>(entity);
    return id == nullptr ? std::string_view() : std::string_view(id->value);
}

std::vector<std::string_view> World::ids() const {
    std::vector<std::string_view> out;
    out.reserve(by_id_.size());
    for (const auto& [id, entity] : by_id_) {
        out.emplace_back(id);
    }
    return out;
}

void World::add_system(std::string name, System system, SystemOptions options) {
    SLASH_ASSERT(system != nullptr, "add_system needs a callable system");
    SLASH_ASSERT(!stepping_, "systems cannot be added while the world is stepping");
    SLASH_ASSERT(std::ranges::none_of(systems_,
                                      [&](const NamedSystem& added) { return added.name == name; }),
                 "two systems have the same name");
    // After every system with the same or a lower priority, so equal priorities keep their order.
    const auto position =
        std::ranges::upper_bound(systems_, options.priority, std::less<>(),
                                 [](const NamedSystem& added) { return added.options.priority; });
    systems_.insert(position, {std::move(name), std::move(system), std::move(options)});
}

std::vector<SystemInfo> World::systems() const {
    std::vector<SystemInfo> out;
    out.reserve(systems_.size());
    for (const NamedSystem& system : systems_) {
        out.push_back({system.name, system.options.doc, system.options.priority});
    }
    return out;
}

std::vector<std::string_view> World::system_names() const {
    std::vector<std::string_view> out;
    out.reserve(systems_.size());
    for (const NamedSystem& system : systems_) {
        out.emplace_back(system.name);
    }
    return out;
}

void World::step(double tick_seconds) {
    if (step_listener_) {
        step_listener_(*this, tick_seconds);
    }
    stepping_ = true;
    for (const NamedSystem& system : systems_) {
        system.run(*this, tick_seconds);
    }
    stepping_ = false;
    ++tick_;
}

void World::add_reset(std::function<void(World&)> reset) {
    resets_.push_back(std::move(reset));
}

void World::reset_state() {
    SLASH_ASSERT(!stepping_, "the world cannot be reset while it is stepping");
    for (const auto& reset : resets_) {
        reset(*this);
    }
}

bool World::is_valid_id(std::string_view id) {
    constexpr std::size_t kMaxLength = 64;
    if (id.empty() || id.size() > kMaxLength) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool World::is_valid_part_id(std::string_view id) {
    constexpr std::size_t kMaxLength = 255;
    if (id.size() > kMaxLength || id.find('/') == std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = id.find('/', start);
        if (!is_valid_id(id.substr(start, end == std::string_view::npos ? end : end - start))) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

void World::on_id_destroyed(entt::registry& registry, entt::entity entity) {
    const std::string& id = registry.get<EntityId>(entity).value;
    const auto it = by_id_.find(id);
    if (it != by_id_.end() && it->second == entity) {
        by_id_.erase(it);
        notify(std::string(kEntityDestroyed), id, {});
    }
}

const PrefabLink* World::prefab_of(entt::entity entity) const {
    return registry_.valid(entity) ? registry_.try_get<PrefabLink>(entity) : nullptr;
}

PrefabLinks World::prefab_links() const {
    PrefabLinks links;
    for (const auto [entity, link] : registry_.view<const PrefabLink>().each()) {
        if (const std::string_view id = id_of(entity); !id.empty()) {
            links.emplace(id, link);
        }
    }
    return links;
}

std::shared_ptr<const json::Value> World::cached_prefab(std::string_view path) const {
    const auto it = prefabs_.find(path);
    return it != prefabs_.end() ? it->second : nullptr;
}

void World::cache_prefab(std::string path, std::shared_ptr<const json::Value> components) {
    prefabs_[std::move(path)] = std::move(components);
}

void World::emit(std::string kind, std::string_view id, json::Value data) {
    SLASH_ASSERT(!kind.empty(), "events need a kind");
    SLASH_ASSERT(!kind.starts_with("entity.") && kind != "command",
                 "event kinds starting with \"entity.\" and \"command\" are the engine's own");
    notify(std::move(kind), id, std::move(data));
}

void World::notify(std::string kind, std::string_view id, json::Value data) {
    if (listener_) {
        listener_(Event{std::move(kind), std::string(id), std::move(data), tick_});
    }
}

} // namespace slash::world
