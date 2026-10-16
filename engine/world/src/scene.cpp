#include "slash/world/scene.hpp"

#include "slash/core/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <map>
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

// The object under key, or nullptr if value is not an object or has no such object.
const json::Value* object_at(const json::Value* value, std::string_view key) {
    if (value == nullptr || !value->is_object()) {
        return nullptr;
    }
    const json::Value* member = value->find(key);
    return member != nullptr && member->is_object() ? member : nullptr;
}

std::string extends_of(const json::Value& entry) {
    const json::Value* extends = entry.is_object() ? entry.find("extends") : nullptr;
    return extends != nullptr && extends->is_string() ? extends->as_string() : std::string();
}

// An entry's components over its prefab's: the prefab's components, with the entry's fields over
// them and its null components removing them. base may be nullptr.
json::Value effective_components(const json::Value* base, const json::Value* components) {
    json::Value out = base != nullptr && base->is_object() ? *base : json::Value(json::Object{});
    if (components == nullptr || !components->is_object()) {
        return out;
    }
    for (const auto& [name, fields] : components->as_object()) {
        if (fields.is_null()) {
            out.as_object().erase(name);
            continue;
        }
        json::Value& target = out[name];
        if (!target.is_object()) {
            target = json::Object{};
        }
        for (const auto& [field, value] : fields.as_object()) {
            target[field] = value;
        }
    }
    return out;
}

// Null components remove one the prefab has, so each must name one. Returns the first that
// doesn't, or "".
std::string stray_null(const json::Value* base, const json::Value* components) {
    if (components == nullptr || !components->is_object()) {
        return {};
    }
    for (const auto& [name, fields] : components->as_object()) {
        if (fields.is_null() && object_at(base, name) == nullptr) {
            return name;
        }
    }
    return {};
}

std::string stray_null_message(std::string_view name, std::string_view prefab) {
    return "null removes a component the prefab has, but " +
           (prefab.empty()
                ? std::string("this entity extends no prefab")
                : "\"" + std::string(prefab) + "\" has no \"" + std::string(name) + "\"");
}

Result<std::shared_ptr<const json::Value>> resolve(World& world, std::string_view path,
                                                   std::vector<std::string>& chain) {
    if (auto cached = world.cached_prefab(path); cached != nullptr) {
        return cached;
    }
    const std::string name(path);
    if (!is_project_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "expected a path inside the project folder, separated by '/', with no "
                          "\".\" or \"..\" parts, got \"" +
                              name + "\"");
    }
    if (!path.ends_with(kPrefabSuffix)) {
        return make_error(ErrorCode::InvalidArgument,
                          "expected a .prefab.json file, got \"" + name + "\"");
    }
    if (std::ranges::find(chain, path) != chain.end()) {
        std::string loop;
        for (const std::string& link : chain) {
            loop += "\"" + link + "\" extends ";
        }
        return make_error(ErrorCode::InvalidArgument,
                          "prefabs extend each other in a loop: " + loop + "\"" + name + "\"");
    }
    if (!world.file_reader()) {
        return make_error(ErrorCode::NotFound,
                          "prefab \"" + name + "\" cannot be read: there is no project folder");
    }
    auto text = world.file_reader()(path);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          "prefab \"" + name + "\" could not be read: " + text.error().message);
    }
    const auto invalid = [&](const std::string& message) {
        return make_error(ErrorCode::InvalidArgument,
                          "prefab \"" + name + "\" is not valid: " + message);
    };
    auto prefab = json::parse(text.value());
    if (!prefab) {
        return invalid(prefab.error().message);
    }
    const auto errors = json::validate(prefab.value(), prefab_schema(world.components()));
    if (!errors.empty()) {
        return invalid((errors.front().path.empty() ? std::string("(root)") : errors.front().path) +
                       ": " + errors.front().message);
    }
    std::shared_ptr<const json::Value> parent;
    if (const std::string extends = extends_of(prefab.value()); !extends.empty()) {
        chain.push_back(name);
        auto resolved = resolve(world, extends, chain);
        chain.pop_back();
        if (!resolved) {
            return resolved.error();
        }
        parent = std::move(resolved).value();
    }
    const json::Value* components = prefab.value().find("components");
    if (const std::string stray = stray_null(parent.get(), components); !stray.empty()) {
        return invalid("/components/" + json::pointer_escape(stray) + ": " +
                       stray_null_message(stray, extends_of(prefab.value())));
    }
    auto base = std::make_shared<const json::Value>(effective_components(parent.get(), components));
    world.cache_prefab(name, base);
    return base;
}

// Assigns an entity's components: its prefab's, with its own over them. path names the value
// at fault on failure.
Result<void> assign_components(World& world, entt::entity entity, const json::Value* base,
                               std::string_view prefab, const json::Value* components,
                               const std::string& prefix, std::string& path) {
    if (const std::string stray = stray_null(base, components); !stray.empty()) {
        path = prefix + "/components/" + json::pointer_escape(stray);
        return make_error(ErrorCode::InvalidArgument, stray_null_message(stray, prefab));
    }
    const json::Value effective = effective_components(base, components);
    for (const auto& [name, fields] : effective.as_object()) {
        path = prefix + "/components/" + json::pointer_escape(name);
        const ComponentType* type = world.components().find(name);
        if (type == nullptr) {
            return make_error(ErrorCode::NotFound, "unknown component \"" + name + "\"");
        }
        if (auto status = type->assign(world, entity, fields, path); !status) {
            return status.error();
        }
    }
    return {};
}

// The prefab components an entry's entity links to, or nullptr.
const json::Value* linked_base(const PrefabLinks& links, std::string_view id,
                               const json::Value& entry) {
    const auto it = links.find(id);
    return it != links.end() && it->second.path == extends_of(entry) ? it->second.base.get()
                                                                     : nullptr;
}

// Loads an already schema-valid scene. On failure path names the offending value and every
// entity created so far is destroyed again.
Result<std::size_t> load_valid(World& world, const json::Value& scene, const PrefabLinks& links,
                               std::string& path) {
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
        std::shared_ptr<const json::Value> base;
        const std::string extends = extends_of(data);
        if (!extends.empty()) {
            path = entity_path(id) + "/extends";
            if (const auto link = links.find(id);
                link != links.end() && link->second.path == extends) {
                base = link->second.base;
            } else {
                std::vector<std::string> chain;
                auto resolved = resolve(world, extends, chain);
                if (!resolved) {
                    return fail(resolved.error());
                }
                base = std::move(resolved).value();
            }
            world.ecs().emplace<PrefabLink>(entity, extends, base);
        }
        if (auto assigned = assign_components(world, entity, base.get(), extends,
                                              data.find("components"), entity_path(id), path);
            !assigned) {
            return fail(assigned.error());
        }
    }
    path.clear();
    return created.size();
}

// An entity's components as a scene saves them. For an entity that extends a prefab, only the
// fields that differ from the prefab's, plus null for a prefab component it lacks.
json::Value saved_components(const World& world, entt::entity entity, const json::Value* base) {
    json::Value components = json::Object{};
    for (const auto& type : world.components().types()) {
        const json::Value* base_fields = object_at(base, type->name());
        if (!type->has(world, entity)) {
            if (base_fields != nullptr) {
                components[type->name()] = json::Value();
            }
            continue;
        }
        if (base_fields == nullptr) {
            components[type->name()] = type->save(world, entity);
            continue;
        }
        json::Value differences = json::Object{};
        const json::Value fields = type->get(world, entity);
        for (const auto& [field, value] : fields.as_object()) {
            const json::Value* inherited = base_fields->find(field);
            if (inherited == nullptr) {
                inherited = type->defaults().find(field);
            }
            if (inherited == nullptr || !(*inherited == value)) {
                differences[field] = value;
            }
        }
        if (differences.size() > 0) {
            components[type->name()] = std::move(differences);
        }
    }
    return components;
}

// One entity's entry in a scene or prefab file.
json::Value saved_entry(const World& world, entt::entity entity) {
    json::Value entry = json::Object{};
    const PrefabLink* link = world.prefab_of(entity);
    if (link != nullptr) {
        entry["extends"] = link->path;
    }
    json::Value components =
        saved_components(world, entity, link != nullptr ? link->base.get() : nullptr);
    if (components.size() > 0) {
        entry["components"] = std::move(components);
    }
    return entry;
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

// The components an entity gained, lost or changed, as diff_scenes describes, from its
// components before and after (prefabs applied). before is nullptr for an added entity.
json::Value changed_components(const ComponentRegistry& components, const json::Value* before,
                               const json::Value& after) {
    const json::Value* old_components = before;
    const json::Value* new_components = &after;
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

namespace {

json::Value component_map_schema(const ComponentRegistry& components, SchemaBuilder& builder) {
    json::Value removed;
    removed["type"] = "null";
    removed["description"] = "Removes this component, which the prefab has.";
    json::Value component_properties = json::Object{};
    for (const auto& type : components.types()) {
        json::Value property;
        property["anyOf"] = json::Array{type->schema(builder), removed};
        component_properties[type->name()] = std::move(property);
    }

    json::Value component_map;
    component_map["type"] = "object";
    component_map["description"] =
        "Components keyed by type name. Fields left out keep their default values, or the "
        "prefab's.";
    component_map["additionalProperties"] = false;
    component_map["properties"] = std::move(component_properties);
    return component_map;
}

json::Value extends_schema() {
    json::Value extends;
    extends["type"] = "string";
    extends["description"] = "A .prefab.json file relative to the project folder, e.g. "
                             "\"prefabs/enemy.prefab.json\". Its components come first and the "
                             "fields given here override them.";
    return extends;
}

json::Value format_schema(std::string_view what) {
    json::Value format;
    format["const"] = kSceneFormat;
    format["description"] = std::string(what) + " file format version.";
    return format;
}

// Maps errors from loading a prefab as the one entity of a scene back to the prefab file.
std::vector<json::ValidationError> prefab_errors(std::vector<json::ValidationError> errors) {
    constexpr std::string_view kPrefix = "/entities/prefab";
    for (json::ValidationError& error : errors) {
        if (error.path.starts_with(kPrefix)) {
            error.path.erase(0, kPrefix.size());
        }
    }
    return errors;
}

// A prefab file as a scene with one entity named "prefab", so scene loading checks it.
json::Value prefab_as_scene(const json::Value& prefab) {
    json::Value entry = json::Object{};
    for (const std::string_view key : {"components", "extends"}) {
        if (const json::Value* value = prefab.find(key); value != nullptr) {
            entry[key] = *value;
        }
    }
    json::Value scene;
    scene["entities"]["prefab"] = std::move(entry);
    scene["format"] = kSceneFormat;
    return scene;
}

} // namespace

json::Value scene_schema(const ComponentRegistry& components) {
    SchemaBuilder builder;
    json::Value entity;
    entity["type"] = "object";
    entity["additionalProperties"] = false;
    entity["properties"]["components"] = component_map_schema(components, builder);
    entity["properties"]["extends"] = extends_schema();

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

    json::Value format = format_schema("Scene");

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

json::Value prefab_schema(const ComponentRegistry& components) {
    SchemaBuilder builder;
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine prefab";
    schema["description"] = "One entity's components, which scene entities extend.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    schema["properties"]["components"] = component_map_schema(components, builder);
    schema["properties"]["extends"] = extends_schema();
    schema["properties"]["format"] = format_schema("Prefab");
    schema["$defs"] = builder.definitions();
    return schema;
}

std::vector<json::ValidationError> validate_scene(const ComponentRegistry& components,
                                                  const json::Value& scene,
                                                  const FileReader& reader) {
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
    scratch.set_file_reader(reader);
    std::string path;
    if (auto loaded = load_valid(scratch, scene, {}, path); !loaded) {
        errors.push_back({path, loaded.error().message});
        return errors;
    }
    // Each child has one parent, a Transform2D, and isn't its own ancestor.
    for (TreeProblem& problem : check_tree(scratch)) {
        errors.push_back({std::move(problem.path), std::move(problem.message)});
    }
    return errors;
}

std::vector<json::ValidationError> validate_prefab(const ComponentRegistry& components,
                                                   const json::Value& prefab,
                                                   const FileReader& reader) {
    std::vector<json::ValidationError> errors = json::validate(prefab, prefab_schema(components));
    if (!errors.empty()) {
        return errors;
    }
    return prefab_errors(validate_scene(components, prefab_as_scene(prefab), reader));
}

Result<std::size_t> load_scene(World& world, const json::Value& scene, const PrefabLinks& links) {
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
    auto loaded = load_valid(world, scene, links, path);
    if (!loaded) {
        return located(path, loaded.error());
    }
    return loaded;
}

Result<std::size_t> restore_scene(World& world, const json::Value& saved,
                                  const PrefabLinks& links) {
    std::string path;
    auto loaded = load_valid(world, saved, links, path);
    if (!loaded) {
        return located(path, loaded.error());
    }
    return loaded;
}

json::Value save_scene(const World& world) {
    json::Value entities = json::Object{};
    for (const std::string_view id : world.ids()) {
        entities[id] = saved_entry(world, world.find(id));
    }
    json::Value scene;
    scene["entities"] = std::move(entities);
    scene["format"] = kSceneFormat;
    return scene;
}

Result<std::shared_ptr<const json::Value>> resolve_prefab(World& world, std::string_view path) {
    std::vector<std::string> chain;
    return resolve(world, path, chain);
}

Result<void> instantiate_prefab(World& world, entt::entity entity, std::string_view path) {
    auto base = resolve_prefab(world, path);
    if (!base) {
        return base.error();
    }
    std::string at;
    if (auto assigned = assign_components(world, entity, base.value().get(), path, nullptr, "", at);
        !assigned) {
        return assigned.error();
    }
    world.ecs().emplace_or_replace<PrefabLink>(entity, std::string(path), base.value());
    return {};
}

PrefabReload reload_prefabs(World& world) {
    PrefabReload reload;
    world.forget_prefabs();
    std::map<std::string, std::vector<entt::entity>, std::less<>> instances;
    for (const auto [entity, link] : world.ecs().view<const PrefabLink>().each()) {
        instances[link.path].push_back(entity);
    }
    for (const auto& [path, entities] : instances) {
        auto resolved = resolve_prefab(world, path);
        if (!resolved) {
            reload.errors.push_back({path, resolved.error().message});
            continue;
        }
        const std::shared_ptr<const json::Value>& base = resolved.value();
        for (const entt::entity entity : entities) {
            auto& link = world.ecs().get<PrefabLink>(entity);
            if (*link.base == *base) {
                link.base = base;
                continue;
            }
            const json::Value overrides = saved_components(world, entity, link.base.get());
            const json::Value target = effective_components(base.get(), &overrides);
            bool changed = false;
            for (const auto& type : world.components().types()) {
                const json::Value* fields = target.find(type->name());
                if (fields == nullptr) {
                    if (type->has(world, entity)) {
                        type->remove(world, entity);
                        changed = true;
                    }
                    continue;
                }
                // Every field, so ones the prefab no longer sets go back to their defaults.
                json::Value values = type->defaults();
                for (const auto& [field, value] : fields->as_object()) {
                    values[field] = value;
                }
                if (type->has(world, entity) && type->get(world, entity) == values) {
                    continue;
                }
                std::string at = "/components/" + type->name();
                if (auto assigned = type->assign(world, entity, values, at); !assigned) {
                    reload.errors.push_back({path, at + ": " + assigned.error().message});
                    continue;
                }
                changed = true;
            }
            link.base = base;
            if (const std::string_view id = world.id_of(entity); changed && !id.empty()) {
                reload.changed.emplace_back(id);
            }
        }
    }
    std::ranges::sort(reload.changed);
    return reload;
}

json::Value save_prefab(const World& world, entt::entity entity) {
    json::Value prefab = saved_entry(world, entity);
    prefab["format"] = kSceneFormat;
    return prefab;
}

json::Value diff_scenes(const ComponentRegistry& components, const json::Value& before,
                        const json::Value& after, const PrefabLinks& before_links,
                        const PrefabLinks& after_links) {
    // Each entity's components with its prefab's under them.
    const auto effective = [](const PrefabLinks& links, std::string_view id,
                              const json::Value& entry) {
        return effective_components(linked_base(links, id, entry), object_at(&entry, "components"));
    };
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
            const json::Value old_components =
                old_entity != nullptr ? effective(before_links, id, *old_entity) : json::Value();
            json::Value changes =
                changed_components(components, old_entity != nullptr ? &old_components : nullptr,
                                   effective(after_links, id, entity));
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

Result<std::string> format_scene(const ComponentRegistry& components, std::string_view text,
                                 const FileReader& reader) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return parsed.error();
    }
    World world(components);
    world.set_file_reader(reader);
    if (auto loaded = load_scene(world, parsed.value()); !loaded) {
        return loaded.error();
    }
    return json::write(save_scene(world)) + "\n";
}

Result<std::string> format_prefab(const ComponentRegistry& components, std::string_view text,
                                  const FileReader& reader) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return parsed.error();
    }
    if (const auto errors = validate_prefab(components, parsed.value(), reader); !errors.empty()) {
        return make_error(
            ErrorCode::InvalidArgument,
            (errors.front().path.empty() ? std::string("(root)") : errors.front().path) + ": " +
                errors.front().message);
    }
    World world(components);
    world.set_file_reader(reader);
    if (auto loaded = load_scene(world, prefab_as_scene(parsed.value())); !loaded) {
        return loaded.error();
    }
    return json::write(save_prefab(world, world.find("prefab"))) + "\n";
}

} // namespace slash::world
