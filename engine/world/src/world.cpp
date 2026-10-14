#include "slash/world/world.hpp"

#include <algorithm>
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
    if (by_id_.contains(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "entity id \"" + std::string(id) + "\" is already in use");
    }
    const entt::entity entity = registry_.create();
    registry_.emplace<EntityId>(entity, std::string(id));
    by_id_.emplace(std::string(id), entity);
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
    if (registry_.valid(entity)) {
        registry_.destroy(entity);
    }
}

void World::clear() {
    registry_.clear();
    by_id_.clear();
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

void World::add_system(std::string name, System system) {
    SLASH_ASSERT(system != nullptr, "add_system needs a callable system");
    SLASH_ASSERT(!stepping_, "systems cannot be added while the world is stepping");
    systems_.push_back({std::move(name), std::move(system)});
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
    stepping_ = true;
    for (const NamedSystem& system : systems_) {
        system.run(*this, tick_seconds);
    }
    stepping_ = false;
    ++tick_;
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

void World::on_id_destroyed(entt::registry& registry, entt::entity entity) {
    const auto it = by_id_.find(registry.get<EntityId>(entity).value);
    if (it != by_id_.end() && it->second == entity) {
        by_id_.erase(it);
    }
}

} // namespace slash::world
