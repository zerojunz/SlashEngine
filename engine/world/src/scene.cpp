#include "slash/world/scene.hpp"

#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace slash::world {
namespace {

// Definition names that cannot clash with a C++ type name.
constexpr std::string_view kEntityDefinition = "scene.entity";

std::string entity_path(std::string_view id) {
    return "/entities/" + json::pointer_escape(id);
}

Error located(std::string_view path, const Error& error) {
    return make_error(error.code, (path.empty() ? std::string("(root)") : std::string(path)) +
                                      ": " + error.message);
}

// Loads an already schema-valid scene. On failure path names the offending value and every
// entity created so far is destroyed again.
Result<std::size_t> load_valid(World& world, const json::Value& scene, std::string& path) {
    const json::Object& entities = scene.find("entities")->as_object();
    std::vector<entt::entity> created;
    created.reserve(entities.size());
    const auto fail = [&](const Error& error) {
        for (const entt::entity entity : created) {
            world.destroy(entity);
        }
        return error;
    };

    // Create every entity first so components can refer to any of them.
    for (const auto& [id, unused] : entities) {
        path = entity_path(id);
        auto entity = world.create(id);
        if (!entity) {
            return fail(entity.error());
        }
        created.push_back(entity.value());
    }

    std::size_t index = 0;
    for (const auto& [id, data] : entities) {
        const entt::entity entity = created[index++];
        const json::Value* components = data.find("components");
        if (components == nullptr) {
            continue;
        }
        for (const auto& [name, fields] : components->as_object()) {
            path = entity_path(id) + "/components/" + json::pointer_escape(name);
            const ComponentType* type = world.components().find(name);
            if (type == nullptr) {
                return fail(make_error(ErrorCode::NotFound, "unknown component \"" + name + "\""));
            }
            if (auto status = type->assign(world, entity, fields, path); !status) {
                return fail(status.error());
            }
        }
    }
    path.clear();
    return created.size();
}

// The object under key, or nullptr if value is not an object or has no such object.
const json::Value* object_at(const json::Value* value, std::string_view key) {
    if (value == nullptr || !value->is_object()) {
        return nullptr;
    }
    const json::Value* member = value->find(key);
    return member != nullptr && member->is_object() ? member : nullptr;
}

// A field's value in saved fields, which leave out fields equal to their defaults.
json::Value field_value(const json::Value* fields, const json::Value& defaults,
                        const std::string& field) {
    if (const json::Value* value = fields != nullptr ? fields->find(field) : nullptr) {
        return *value;
    }
    if (const json::Value* value = defaults.is_object() ? defaults.find(field) : nullptr) {
        return *value;
    }
    return {};
}

// The fields of after whose values differ from before, with their new values.
json::Value changed_fields(const json::Value& defaults, const json::Value* before,
                           const json::Value& after) {
    std::vector<std::string> fields;
    for (const json::Value* source : {&defaults, before, &after}) {
        if (source != nullptr && source->is_object()) {
            for (const auto& [field, unused] : source->as_object()) {
                fields.push_back(field);
            }
        }
    }
    std::ranges::sort(fields);
    const auto [first, last] = std::ranges::unique(fields);
    fields.erase(first, last);

    json::Value changed = json::Object{};
    for (const std::string& field : fields) {
        json::Value value = field_value(&after, defaults, field);
        if (!(value == field_value(before, defaults, field))) {
            changed[field] = std::move(value);
        }
    }
    return changed;
}

// The components an entity gained, lost or changed, as diff_scenes describes. before is
// nullptr for an added entity.
json::Value changed_components(const ComponentRegistry& components, const json::Value* before,
                               const json::Value& after) {
    const json::Value* old_components = object_at(before, "components");
    const json::Value* new_components = object_at(&after, "components");
    json::Value changes = json::Object{};
    if (new_components != nullptr) {
        for (const auto& [name, fields] : new_components->as_object()) {
            const ComponentType* type = components.find(name);
            const json::Value defaults = type != nullptr ? type->defaults() : json::Value();
            const json::Value* old_fields = object_at(old_components, name);
            json::Value changed = changed_fields(defaults, old_fields, fields);
            if (old_fields == nullptr || changed.size() > 0) {
                changes[name] = std::move(changed);
            }
        }
    }
    if (old_components != nullptr) {
        for (const auto& [name, unused] : old_components->as_object()) {
            if (new_components == nullptr || new_components->find(name) == nullptr) {
                changes[name] = json::Value();
            }
        }
    }
    return changes;
}

} // namespace

json::Value scene_schema(const ComponentRegistry& components) {
    SchemaBuilder builder;
    json::Value component_properties = json::Object{};
    for (const auto& type : components.types()) {
        component_properties[type->name()] = type->schema(builder);
    }

    json::Value component_map;
    component_map["type"] = "object";
    component_map["description"] =
        "Components keyed by type name. Fields left out keep their default values.";
    component_map["additionalProperties"] = false;
    component_map["properties"] = std::move(component_properties);

    json::Value entity;
    entity["type"] = "object";
    entity["additionalProperties"] = false;
    entity["properties"]["components"] = std::move(component_map);

    json::Value ids;
    ids["minLength"] = 1;
    ids["maxLength"] = 64;
    ids["pattern"] = "^[A-Za-z0-9_-]+$";

    json::Value entities;
    entities["type"] = "object";
    entities["description"] =
        "Entities keyed by stable ID: 1 to 64 letters, digits, '_' or '-', e.g. \"player\".";
    entities["propertyNames"] = std::move(ids);
    entities["additionalProperties"] = SchemaBuilder::reference(kEntityDefinition);

    json::Value format;
    format["const"] = kSceneFormat;
    format["description"] = "Scene file format version.";

    json::Value definitions = builder.definitions();
    definitions[kEntityDefinition] = std::move(entity);

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine scene";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"entities", "format"};
    schema["properties"]["entities"] = std::move(entities);
    schema["properties"]["format"] = std::move(format);
    schema["$defs"] = std::move(definitions);
    return schema;
}

std::vector<json::ValidationError> validate_scene(const ComponentRegistry& components,
                                                  const json::Value& scene) {
    std::vector<json::ValidationError> errors = json::validate(scene, scene_schema(components));
    if (!errors.empty()) {
        return errors;
    }
    // The validator skips "pattern", so entity IDs are checked here.
    for (const auto& [id, unused] : scene.find("entities")->as_object()) {
        if (!World::is_valid_id(id)) {
            errors.push_back({entity_path(id), "invalid entity id \"" + id +
                                                   "\": use 1 to 64 letters, digits, '_' or '-'"});
        }
    }
    if (!errors.empty()) {
        return errors;
    }
    World scratch(components);
    std::string path;
    if (auto loaded = load_valid(scratch, scene, path); !loaded) {
        errors.push_back({path, loaded.error().message});
    }
    return errors;
}

Result<std::size_t> load_scene(World& world, const json::Value& scene) {
    const std::vector<json::ValidationError> errors =
        json::validate(scene, scene_schema(world.components()));
    if (!errors.empty()) {
        std::string message = errors.front().message;
        if (errors.size() > 1) {
            message += " (and " + std::to_string(errors.size() - 1) + " more errors)";
        }
        return located(errors.front().path, make_error(ErrorCode::InvalidArgument, message));
    }
    std::string path;
    auto loaded = load_valid(world, scene, path);
    if (!loaded) {
        return located(path, loaded.error());
    }
    return loaded;
}

json::Value save_scene(const World& world) {
    json::Value entities = json::Object{};
    for (const std::string_view id : world.ids()) {
        const entt::entity entity = world.find(id);
        json::Value components = json::Object{};
        for (const auto& type : world.components().types()) {
            if (type->has(world, entity)) {
                components[type->name()] = type->save(world, entity);
            }
        }
        json::Value& entry = entities[id];
        entry = json::Object{};
        if (components.size() > 0) {
            entry["components"] = std::move(components);
        }
    }
    json::Value scene;
    scene["entities"] = std::move(entities);
    scene["format"] = kSceneFormat;
    return scene;
}

json::Value diff_scenes(const ComponentRegistry& components, const json::Value& before,
                        const json::Value& after) {
    const json::Value* old_entities = object_at(&before, "entities");
    const json::Value* new_entities = object_at(&after, "entities");
    json::Array added;
    json::Array removed;
    json::Value changed = json::Object{};
    if (old_entities != nullptr) {
        for (const auto& [id, unused] : old_entities->as_object()) {
            if (new_entities == nullptr || new_entities->find(id) == nullptr) {
                removed.emplace_back(id);
            }
        }
    }
    if (new_entities != nullptr) {
        for (const auto& [id, entity] : new_entities->as_object()) {
            const json::Value* old_entity =
                old_entities != nullptr ? old_entities->find(id) : nullptr;
            if (old_entity == nullptr) {
                added.emplace_back(id);
            }
            json::Value changes = changed_components(components, old_entity, entity);
            if (changes.size() > 0) {
                changed[id] = std::move(changes);
            }
        }
    }
    json::Value diff = json::Object{};
    if (!added.empty()) {
        diff["added"] = std::move(added);
    }
    if (!removed.empty()) {
        diff["removed"] = std::move(removed);
    }
    if (changed.size() > 0) {
        diff["changed"] = std::move(changed);
    }
    return diff;
}

Result<std::string> format_scene(const ComponentRegistry& components, std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return parsed.error();
    }
    World world(components);
    if (auto loaded = load_scene(world, parsed.value()); !loaded) {
        return loaded.error();
    }
    return json::write(save_scene(world)) + "\n";
}

} // namespace slash::world
