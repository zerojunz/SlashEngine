#include "slash/world/component_registry.hpp"

#include <algorithm>
#include <entt/core/hashed_string.hpp>

namespace slash::world {
namespace {

// Applies a field's range to its number schema, or to the items of an array or values of a map
// of numbers.
void apply_range(json::Value& schema, const FieldMeta& meta) {
    if (json::Value* items = schema.find("items"); items != nullptr && items->is_object()) {
        apply_range(*items, meta);
        return;
    }
    if (json::Value* values = schema.find("additionalProperties");
        values != nullptr && values->is_object()) {
        apply_range(*values, meta);
        return;
    }
    const json::Value* type = schema.find("type");
    if (type == nullptr || !type->is_string() ||
        (type->as_string() != "number" && type->as_string() != "integer")) {
        return;
    }
    if (meta.min) {
        schema["minimum"] = *meta.min;
    }
    if (meta.max) {
        schema["maximum"] = *meta.max;
    }
}

} // namespace

ComponentType::ComponentType(std::string type_name, entt::id_type hash,
                             std::vector<std::string> field_names, json::Value default_values,
                             Ops ops)
    : name_(std::move(type_name)), type_hash_(hash), fields_(std::move(field_names)),
      defaults_(std::move(default_values)), ops_(ops) {}

const FieldMeta* ComponentType::field_meta(std::string_view field) const {
    const auto it = field_meta_.find(field);
    return it == field_meta_.end() ? nullptr : &it->second;
}

ComponentType& ComponentType::set_doc(std::string text) {
    doc_ = std::move(text);
    return *this;
}

ComponentType& ComponentType::describe(std::string_view field, FieldMeta meta) {
    SLASH_ASSERT(std::ranges::find(fields_, field) != fields_.end(),
                 "describe() names a field the component does not have");
    field_meta_.insert_or_assign(std::string(field), std::move(meta));
    return *this;
}

bool ComponentType::has(const World& world, entt::entity entity) const {
    return world.valid(entity) && ops_.has(world.ecs(), entity);
}

void ComponentType::remove(World& world, entt::entity entity) const {
    if (world.valid(entity)) {
        ops_.remove(world.ecs(), entity);
    }
}

json::Value ComponentType::get(const World& world, entt::entity entity) const {
    SLASH_ASSERT(has(world, entity), "get() called for an entity without this component");
    return ops_.save(world, entity);
}

json::Value ComponentType::save(const World& world, entt::entity entity) const {
    json::Value values = get(world, entity);
    std::erase_if(values.as_object(), [&](const auto& member) {
        const json::Value* fallback = defaults_.find(member.first);
        return fallback != nullptr && *fallback == member.second;
    });
    return values;
}

Result<void> ComponentType::assign(World& world, entt::entity entity, const json::Value& values,
                                   std::string& path) const {
    if (!world.valid(entity)) {
        return make_error(ErrorCode::NotFound, "the entity does not exist");
    }
    return ops_.assign(world, entity, values, path);
}

json::Value ComponentType::schema(SchemaBuilder& builder) const {
    if (ops_.schema == nullptr) {
        // A stand-in (ComponentRegistry::add_stand_in): its fields, holding anything.
        json::Value definition;
        definition["type"] = "object";
        definition["additionalProperties"] = false;
        json::Object properties;
        for (const std::string& field : fields_) {
            properties[field] = json::Object{};
        }
        definition["properties"] = std::move(properties);
        if (!doc_.empty()) {
            definition["description"] = doc_;
        }
        return builder.define(name_, std::move(definition));
    }
    json::Value reference = ops_.schema(builder);
    json::Value& definition = builder.definition(name_);
    if (!doc_.empty()) {
        definition["description"] = doc_;
    }
    json::Value& properties = definition["properties"];
    for (auto& [field, property] : properties.as_object()) {
        if (const json::Value* fallback = defaults_.find(field); fallback != nullptr) {
            property["default"] = *fallback;
        }
        if (const FieldMeta* meta = field_meta(field); meta != nullptr) {
            if (!meta->doc.empty()) {
                property["description"] = meta->doc;
            }
            apply_range(property, *meta);
            if (!meta->choices.empty()) {
                json::Array choices;
                for (const std::string& choice : meta->choices) {
                    choices.emplace_back(choice);
                }
                // For a list of strings, each item is one of them.
                const json::Value* type = property.find("type");
                if (type != nullptr && type->is_string() && type->as_string() == "array") {
                    property["items"]["enum"] = std::move(choices);
                } else {
                    property["enum"] = std::move(choices);
                }
            }
        }
    }
    return reference;
}

const ComponentType* ComponentRegistry::find(std::string_view name) const {
    const auto it = std::ranges::find_if(
        types_, [&](const std::unique_ptr<ComponentType>& type) { return type->name() == name; });
    return it == types_.end() ? nullptr : it->get();
}

ComponentType* ComponentRegistry::find_mutable(std::string_view name) {
    const auto it = std::ranges::find_if(
        types_, [&](const std::unique_ptr<ComponentType>& type) { return type->name() == name; });
    return it == types_.end() ? nullptr : it->get();
}

ComponentType& ComponentRegistry::insert(std::unique_ptr<ComponentType> type) {
    for (const auto& existing : types_) {
        SLASH_ASSERT(existing->type_hash() != type->type_hash(), "component type registered twice");
        SLASH_ASSERT(existing->name() != type->name(), "two component types have the same name");
    }
    types_.push_back(std::move(type));
    ++revision_;
    return *types_.back();
}

ComponentType& ComponentRegistry::add_stand_in(std::string name, std::vector<std::string> fields) {
    const ComponentType::Ops ops{
        .has = [](const entt::registry& /*registry*/, entt::entity /*entity*/) { return false; },
        .remove = [](entt::registry& /*registry*/, entt::entity /*entity*/) {},
        .save = [](const World& /*world*/, entt::entity /*entity*/) -> json::Value {
            return json::Object{};
        },
        // Loading a scene to check it leaves the component out.
        .assign = [](World& /*world*/, entt::entity /*entity*/, const json::Value& /*fields*/,
                     std::string& /*path*/) -> Result<void> { return {}; },
        .schema = nullptr,
        .rename = [](json::Value& /*fields*/, const EntityRename& /*rename*/) {},
    };
    const entt::id_type hash = entt::hashed_string::value(name.data(), name.size());
    return insert(std::make_unique<ComponentType>(std::move(name), hash, std::move(fields),
                                                  json::Object{}, ops));
}

ComponentType& ComponentRegistry::adopt(std::unique_ptr<ComponentType> type) {
    SLASH_ASSERT(type != nullptr, "adopt needs a component type");
    return insert(std::move(type));
}

std::unique_ptr<ComponentType> ComponentRegistry::take(std::string_view name) {
    const auto it = std::ranges::find_if(
        types_, [&](const std::unique_ptr<ComponentType>& type) { return type->name() == name; });
    if (it == types_.end()) {
        return nullptr;
    }
    std::unique_ptr<ComponentType> type = std::move(*it);
    types_.erase(it);
    ++revision_;
    return type;
}

json::Value& SchemaBuilder::definition(std::string_view name) {
    const auto it = definitions_.find(name);
    SLASH_ASSERT(it != definitions_.end(), "schema definition does not exist");
    return it->second;
}

json::Value SchemaBuilder::define(std::string_view name, json::Value schema) {
    definitions_.insert_or_assign(std::string(name), std::move(schema));
    return reference(name);
}

json::Value SchemaBuilder::reference(std::string_view name) {
    json::Value out;
    out["$ref"] = "#/$defs/" + json::pointer_escape(name);
    return out;
}

} // namespace slash::world
