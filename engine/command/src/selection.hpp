#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What world.query, run.until and replay.seek select, shared inside the command library.
namespace slash::command::detail {

inline constexpr std::int64_t kDefaultQueryLimit = 100;

struct Condition {
    const world::ComponentType* type = nullptr;
    std::vector<std::string> segments; // field name, then array indices or nested field names
    std::string op;
    json::Value value;
};

// Entities with all of `with`, none of `without` and meeting every `where` condition, returned
// as IDs or with the components `include` names.
struct Selection {
    std::vector<const world::ComponentType*> with;
    std::vector<const world::ComponentType*> without;
    std::vector<Condition> conditions;
    std::vector<const world::ComponentType*> included;
    bool with_components = false;
    bool omit_defaults = false;
    std::size_t limit = kDefaultQueryLimit;
};

// The params a selection reads, as schema properties: with, without, where, include,
// omit_defaults and limit.
[[nodiscard]] json::Object selection_properties();

// Reads a selection from params, or returns the error for a bad one.
[[nodiscard]] std::optional<Reply> parse_selection(const world::ComponentRegistry& components,
                                                   const json::Value& params, Selection& out);

[[nodiscard]] bool selects(const world::World& world, entt::entity entity,
                           const Selection& selection);
[[nodiscard]] bool any_selected(const world::World& world, const Selection& selection);

// {"count", "entities", "truncated"} for the entities a selection picks now.
[[nodiscard]] json::Value select(const world::World& world, const Selection& selection);

} // namespace slash::command::detail
