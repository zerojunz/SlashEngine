#include "slash/world/scene.hpp"

#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <utility>

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
