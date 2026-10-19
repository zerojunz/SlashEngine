#pragma once

#include "slash/core/assert.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/core/result.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace slash::world {

// Optional documentation for one field, shown in the generated schema.
struct FieldMeta {
    // Every member has an initializer so designated initializers may skip any of them without
    // tripping GCC's -Wmissing-field-initializers.
    std::string doc{};                        // NOLINT(readability-redundant-member-init)
    std::optional<double> min = std::nullopt; // inclusive; for arrays and maps, to each number
    std::optional<double> max = std::nullopt;
    // For a string field, the only values allowed; for a list of strings, those of each item.
    std::vector<std::string> choices{}; // NOLINT(readability-redundant-member-init)
};

// A registered component type: its name, fields and docs, plus type-erased operations so scenes
// (and later the command API) can handle components they only know by name.
class ComponentType {
public:
    struct Ops {
        bool (*has)(const entt::registry& registry, entt::entity entity);
        void (*remove)(entt::registry& registry, entt::entity entity);
        json::Value (*save)(const World& world, entt::entity entity);
        Result<void> (*assign)(World& world, entt::entity entity, const json::Value& fields,
                               std::string& path);
        json::Value (*schema)(SchemaBuilder& builder);
        void (*rename)(json::Value& fields, const EntityRename& rename);
    };

    ComponentType(std::string type_name, entt::id_type hash, std::vector<std::string> field_names,
                  json::Value default_values, Ops ops);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] entt::id_type type_hash() const noexcept { return type_hash_; }
    [[nodiscard]] std::span<const std::string> fields() const noexcept { return fields_; }
    [[nodiscard]] const std::string& doc() const noexcept { return doc_; }
    [[nodiscard]] const FieldMeta* field_meta(std::string_view field) const;

    // Every field at its default value, as JSON.
    [[nodiscard]] const json::Value& defaults() const noexcept { return defaults_; }

    // Documents the component; returns *this so calls chain after ComponentRegistry::add.
    ComponentType& set_doc(std::string text);

    // Documents one field. The field must exist.
    ComponentType& describe(std::string_view field, FieldMeta meta);

    [[nodiscard]] bool has(const World& world, entt::entity entity) const;
    void remove(World& world, entt::entity entity) const;

    // Every field of the entity's component as a JSON object.
    [[nodiscard]] json::Value get(const World& world, entt::entity entity) const;

    // The component's fields as a JSON object, leaving out those equal to their defaults.
    [[nodiscard]] json::Value save(const World& world, entt::entity entity) const;

    // Adds the component if the entity lacks it, then sets the fields present in the object.
    // Either the whole change applies or nothing does. On failure path is extended to the
    // offending field (see from_json).
    [[nodiscard]] Result<void> assign(World& world, entt::entity entity, const json::Value& values,
                                      std::string& path) const;

    // Adds this component's definition, with docs, defaults and ranges, and returns a $ref to it.
    [[nodiscard]] json::Value schema(SchemaBuilder& builder) const;

    // Renames the entity references in some of this component's fields, as JSON.
    void rename_entities(json::Value& fields, const EntityRename& rename) const {
        ops_.rename(fields, rename);
    }

private:
    std::string name_;
    entt::id_type type_hash_;
    std::vector<std::string> fields_;
    json::Value defaults_;
    Ops ops_;
    std::string doc_;
    std::map<std::string, FieldMeta, std::less<>> field_meta_;
};

// The component types a world can save, load and describe. Register every component that should
// appear in scene files:
//
//   registry.add<Health>().set_doc("Hit points.").describe("current", {.doc = "...", .min = 0});
class ComponentRegistry {
public:
    template <reflect::Component T>
    ComponentType& add();

    // The type with this name, or nullptr.
    [[nodiscard]] const ComponentType* find(std::string_view name) const;
    [[nodiscard]] ComponentType* find_mutable(std::string_view name);

    // Registered types in the order they were added.
    [[nodiscard]] std::span<const std::unique_ptr<ComponentType>> types() const noexcept {
        return types_;
    }

    // A stand-in for a component of a game's own code when the code isn't loaded, so files that
    // use it can still be checked: scene checks take any value for its fields, and loading a
    // scene into a world leaves it out. Its name must be new.
    ComponentType& add_stand_in(std::string name, std::vector<std::string> fields);

    // Moves types between registries, as the engine does with a game's own types when it loads
    // or reloads its code (slash/world/game_module.hpp). adopt adds a type another registry made;
    // its name and type must be new here. take removes a type and returns it, or nullptr.
    ComponentType& adopt(std::unique_ptr<ComponentType> type);
    std::unique_ptr<ComponentType> take(std::string_view name);

    // Counts the types added and taken, so code that keeps ComponentType pointers between calls
    // can tell when to look them up again.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    ComponentType& insert(std::unique_ptr<ComponentType> type);

    std::vector<std::unique_ptr<ComponentType>> types_;
    std::uint64_t revision_ = 0;
};

template <reflect::Component T>
ComponentType& ComponentRegistry::add() {
    static_assert(std::is_default_constructible_v<T> && std::is_copy_constructible_v<T>,
                  "components must be default-constructible and copyable");

    const ComponentType::Ops ops{
        .has = [](const entt::registry& registry,
                  entt::entity entity) { return registry.all_of<T>(entity); },
        .remove = [](entt::registry& registry, entt::entity entity) { registry.remove<T>(entity); },
        .save = [](const World& world [[maybe_unused]],
                   entt::entity entity [[maybe_unused]]) -> json::Value {
            if constexpr (std::is_empty_v<T>) {
                return json::Object{};
            } else {
                return to_json(world, world.ecs().get<T>(entity));
            }
        },
        .assign = [](World& world, entt::entity entity, const json::Value& fields,
                     std::string& path) -> Result<void> {
            entt::registry& registry = world.ecs();
            T value{};
            if constexpr (!std::is_empty_v<T>) {
                if (registry.all_of<T>(entity)) {
                    value = registry.get<T>(entity);
                }
            }
            if (auto status = from_json(world, fields, value, path); !status) {
                return status;
            }
            if constexpr (std::is_empty_v<T>) {
                if (!registry.all_of<T>(entity)) {
                    registry.emplace<T>(entity);
                }
            } else {
                registry.emplace_or_replace<T>(entity, std::move(value));
            }
            return {};
        },
        .schema = [](SchemaBuilder& builder) { return builder.ref<T>(); },
        .rename = [](json::Value& fields,
                     const EntityRename& rename) { world::rename_entities<T>(fields, rename); },
    };

    std::vector<std::string> fields;
    reflect::for_each_field<T>([&](const auto& field) { fields.emplace_back(field.name); });

    // Defaults hold no entity references, so a scratch world is enough to serialize them.
    const World scratch(*this);
    json::Value defaults = to_json(scratch, T{});

    return insert(std::make_unique<ComponentType>(std::string(reflect::type_name<T>()),
                                                  entt::type_hash<T>::value(), std::move(fields),
                                                  std::move(defaults), ops));
}

} // namespace slash::world
