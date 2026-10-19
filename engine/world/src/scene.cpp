#include "slash/world/scene.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
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

bool is_scene_file(std::string_view path) {
    return path.ends_with(kSceneSuffix);
}

// Whether an ID names an entity an instanced scene brought, as "hero/sprite" does.
bool is_part_id(std::string_view id) {
    return id.find('/') != std::string_view::npos;
}

// The ID of the entity local of an instanced scene brings to instance.
std::string part_id(std::string_view instance, std::string_view local) {
    std::string id(instance);
    id += '/';
    id += local;
    return id;
}

// An instanced scene's IDs as the instance's: its root is the instance, and its other entities
// are the instance's ID, '/' and theirs. Other IDs name entities outside it and stay as they
// are. tmpl is a resolved scene (see PrefabLink) and must outlive the result.
EntityRename instance_rename(const json::Value& tmpl, const std::string& instance) {
    const std::string& root = tmpl.find("root")->as_string();
    const json::Value& entities = *tmpl.find("entities");
    return [&root, &entities, instance](const std::string& id) -> std::string {
        if (id == root) {
            return instance;
        }
        return entities.find(id) != nullptr ? part_id(instance, id) : id;
    };
}

// Components with their entity references renamed.
json::Value renamed(const ComponentRegistry& registry, json::Value components,
                    const EntityRename& rename) {
    if (!components.is_object()) {
        return components;
    }
    for (auto& [name, fields] : components.as_object()) {
        if (const ComponentType* type = registry.find(name); type != nullptr) {
            type->rename_entities(fields, rename);
        }
    }
    return components;
}

// What an entry extends: the resolved prefab or scene at a path, for the entry with an ID.
using Resolver = std::function<Result<std::shared_ptr<const json::Value>>(const std::string& id,
                                                                          const std::string& path)>;

// One entity a scene's entries make: a plain entity, an instance of a prefab or a scene, or an
// entity an instanced scene brings (a part).
struct Made {
    std::string id;
    json::Value components;                  // with prefabs applied, in the scene's IDs
    std::string extends;                     // the file the entry extends, or ""
    std::shared_ptr<const json::Value> base; // what that file resolved to
    std::string scene;                       // for a part, the scene its instance extends
};

// The entities a scene's entries make, sorted by ID: each entry's entity, and for an entry
// that extends a scene, the scene's other entities under the entry's ID, changed or left out by
// the entries for them. On failure path names the value at fault. A lenient expansion, for
// comparing saved scenes, skips what doesn't fit instead of failing.
Result<std::vector<Made>> expand(const ComponentRegistry& registry, const json::Object& entries,
                                 const Resolver& resolver, std::string& path,
                                 bool lenient = false) {
    std::vector<Made> made;
    std::map<std::string, Made, std::less<>> parts;
    for (const auto& [id, entry] : entries) {
        if (is_part_id(id)) {
            continue;
        }
        const std::string at = entity_path(id);
        if (!entry.is_object()) {
            if (lenient) {
                continue;
            }
            path = at;
            return make_error(ErrorCode::InvalidArgument,
                              "null leaves out an entity an instanced scene brings, but \"" + id +
                                  "\" is not one");
        }
        const json::Value* components = entry.find("components");
        const std::string extends = extends_of(entry);
        std::shared_ptr<const json::Value> base;
        if (!extends.empty()) {
            path = at + "/extends";
            auto resolved = resolver(id, extends);
            if (!resolved) {
                return resolved.error();
            }
            base = std::move(resolved).value();
        }
        const json::Value* start = base.get();
        json::Value root_components;
        if (base != nullptr && is_scene_file(extends)) {
            const std::string& root = base->find("root")->as_string();
            const EntityRename rename = instance_rename(*base, id);
            for (const auto& [local, fields] : base->find("entities")->as_object()) {
                json::Value own = renamed(registry, fields, rename);
                if (local == root) {
                    root_components = std::move(own);
                } else {
                    std::string part = part_id(id, local);
                    parts.emplace(part, Made{part, std::move(own), "", nullptr, extends});
                }
            }
            start = &root_components;
        }
        if (const std::string stray = stray_null(start, components); !stray.empty() && !lenient) {
            path = at + "/components/" + json::pointer_escape(stray);
            return make_error(ErrorCode::InvalidArgument, stray_null_message(stray, extends));
        }
        made.push_back({id, effective_components(start, components), extends, std::move(base), ""});
    }
    // Entries for the entities instances bring change them or leave them out.
    for (const auto& [id, entry] : entries) {
        if (!is_part_id(id)) {
            continue;
        }
        const std::string at = entity_path(id);
        const auto part = parts.find(id);
        if (part == parts.end()) {
            if (lenient) {
                if (entry.is_object()) {
                    made.push_back({id, effective_components(nullptr, entry.find("components")), "",
                                    nullptr, ""});
                }
                continue;
            }
            path = at;
            std::string message = "no instanced scene brings an entity \"" + id + "\"";
            const std::string owner = id.substr(0, id.find('/'));
            const auto instance = entries.find(owner);
            if (instance == entries.end()) {
                message += ": there is no entity \"" + owner + "\"";
            } else if (const std::string extends = extends_of(instance->second);
                       !is_scene_file(extends)) {
                message += ": \"" + owner + "\" extends no scene";
            } else {
                message +=
                    ": \"" + extends + "\" has no entity \"" + id.substr(owner.size() + 1) + "\"";
            }
            return make_error(ErrorCode::NotFound, message);
        }
        if (entry.is_null()) {
            parts.erase(part);
            continue;
        }
        if (entry.find("extends") != nullptr && !lenient) {
            path = at + "/extends";
            return make_error(ErrorCode::InvalidArgument, "\"" + id + "\" comes from \"" +
                                                              part->second.scene +
                                                              "\", which decides what it extends");
        }
        const json::Value* components = entry.find("components");
        if (const std::string stray = stray_null(&part->second.components, components);
            !stray.empty() && !lenient) {
            path = at + "/components/" + json::pointer_escape(stray);
            return make_error(ErrorCode::InvalidArgument,
                              "null removes a component the entity has in \"" + part->second.scene +
                                  "\", but it has no \"" + stray + "\"");
        }
        part->second.components = effective_components(&part->second.components, components);
    }
    for (auto& [id, part] : parts) {
        made.push_back(std::move(part));
    }
    std::ranges::sort(made, std::less<>(), &Made::id);
    return made;
}

Result<std::shared_ptr<const json::Value>> resolve(World& world, std::string_view path,
                                                   std::vector<std::string>& chain);

// A scene other scenes instance, as PrefabLink describes it.
Result<std::shared_ptr<const json::Value>> resolve_scene(World& world, const std::string& name,
                                                         const json::Value& scene,
                                                         std::vector<std::string>& chain) {
    const auto invalid = [&](const std::string& message) {
        return make_error(ErrorCode::InvalidArgument,
                          "scene \"" + name + "\" is not valid: " + message);
    };
    const json::Value* root = scene.find("root");
    if (root == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "scene \"" + name +
                              "\" has no \"root\": name the entity an instance of it becomes "
                              "(scene.save takes \"root\")");
    }
    const json::Object& entries = scene.find("entities")->as_object();
    if (const auto it = entries.find(root->as_string());
        it == entries.end() || !it->second.is_object() || is_part_id(root->as_string())) {
        return invalid("/root: no entity has the id \"" + root->as_string() + "\"");
    }
    chain.emplace_back(name);
    std::string at;
    auto made = expand(
        world.components(), entries,
        [&](const std::string& /*id*/, const std::string& extends) {
            return resolve(world, extends, chain);
        },
        at);
    chain.pop_back();
    if (!made) {
        return invalid(at + ": " + made.error().message);
    }
    json::Value entities = json::Object{};
    for (Made& entity : made.value()) {
        entities[entity.id] = std::move(entity.components);
    }
    json::Value resolved;
    resolved["entities"] = std::move(entities);
    resolved["root"] = *root;
    auto shared = std::make_shared<const json::Value>(std::move(resolved));
    world.cache_prefab(name, shared);
    return shared;
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
    const bool scene = is_scene_file(path);
    if (!scene && !path.ends_with(kPrefabSuffix)) {
        return make_error(ErrorCode::InvalidArgument,
                          "expected a .prefab.json or .scene.json file, got \"" + name + "\"");
    }
    if (std::ranges::find(chain, path) != chain.end()) {
        std::string loop;
        for (const std::string& link : chain) {
            loop += "\"" + link + (scene ? "\" instances " : "\" extends ");
        }
        return make_error(ErrorCode::InvalidArgument,
                          std::string(scene ? "scenes instance" : "prefabs extend") +
                              " each other in a loop: " + loop + "\"" + name + "\"");
    }
    const std::string kind = scene ? "scene" : "prefab";
    if (!world.file_reader()) {
        return make_error(ErrorCode::NotFound,
                          kind + " \"" + name + "\" cannot be read: there is no project folder");
    }
    auto text = world.file_reader()(path);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          kind + " \"" + name + "\" could not be read: " + text.error().message);
    }
    const auto invalid = [&](const std::string& message) {
        return make_error(ErrorCode::InvalidArgument,
                          kind + " \"" + name + "\" is not valid: " + message);
    };
    auto prefab = json::parse(text.value());
    if (!prefab) {
        return invalid(prefab.error().message);
    }
    const auto errors = json::validate(prefab.value(), scene ? scene_schema(world.components())
                                                             : prefab_schema(world.components()));
    if (!errors.empty()) {
        return invalid((errors.front().path.empty() ? std::string("(root)") : errors.front().path) +
                       ": " + errors.front().message);
    }
    if (scene) {
        return resolve_scene(world, name, prefab.value(), chain);
    }
    std::shared_ptr<const json::Value> parent;
    if (const std::string extends = extends_of(prefab.value()); !extends.empty()) {
        if (is_scene_file(extends)) {
            return invalid("/extends: a prefab extends another .prefab.json file; an entity in a "
                           "scene can extend a scene");
        }
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

// Assigns what still fits of a component saved with an older version of its type, as a game's
// reloaded code may leave one: each field the assignment fails on is left at its default. False
// if nothing could be assigned.
bool assign_what_fits(World& world, entt::entity entity, const ComponentType& type,
                      json::Value fields, const std::string& prefix, std::string& path) {
    while (fields.is_object()) {
        const std::string field =
            json::pointer_first_key(std::string_view(path).substr(prefix.size()));
        if (field.empty() || fields.as_object().erase(field) == 0) {
            return false;
        }
        path = prefix;
        if (type.assign(world, entity, fields, path)) {
            return true;
        }
    }
    return false;
}

// Loads an already schema-valid scene. On failure path names the offending value and every
// entity created so far is destroyed again. A lenient load, for copies saved from a world whose
// component types have changed since, leaves out components and fields the types no longer take.
Result<std::size_t> load_valid(World& world, const json::Value& scene, const PrefabLinks& links,
                               std::string& path, bool lenient = false) {
    const json::Object& entries = scene.find("entities")->as_object();
    const Resolver resolver =
        [&](const std::string& id,
            const std::string& extends) -> Result<std::shared_ptr<const json::Value>> {
        if (const auto link = links.find(id); link != links.end() && link->second.path == extends) {
            return link->second.base;
        }
        std::vector<std::string> chain;
        return resolve(world, extends, chain);
    };
    auto expanded = expand(world.components(), entries, resolver, path);
    if (!expanded) {
        return expanded.error();
    }
    const std::vector<Made>& made = expanded.value();
    std::size_t dropped = 0; // components a lenient load left out in part or whole
    std::vector<entt::entity> created;
    created.reserve(made.size());
    const auto fail = [&](const Error& error) {
        for (const entt::entity entity : created) {
            world.destroy(entity);
        }
        return error;
    };

    // Create every entity first so components can refer to any of them.
    for (const Made& entity : made) {
        path = entity_path(entity.id);
        auto added = is_part_id(entity.id) ? world.create_part(entity.id) : world.create(entity.id);
        if (!added) {
            return fail(added.error());
        }
        created.push_back(added.value());
    }

    for (std::size_t i = 0; i < made.size(); ++i) {
        const Made& entity = made[i];
        if (!entity.extends.empty()) {
            world.ecs().emplace<PrefabLink>(created[i], entity.extends, entity.base);
        }
        for (const auto& [name, fields] : entity.components.as_object()) {
            const std::string prefix =
                entity_path(entity.id) + "/components/" + json::pointer_escape(name);
            path = prefix;
            const ComponentType* type = world.components().find(name);
            if (type == nullptr) {
                if (lenient) {
                    ++dropped;
                    continue;
                }
                return fail(make_error(ErrorCode::NotFound, "unknown component \"" + name + "\""));
            }
            if (auto status = type->assign(world, created[i], fields, path); !status) {
                if (!lenient) {
                    return fail(status.error());
                }
                ++dropped;
                (void)assign_what_fits(world, created[i], *type, fields, prefix, path);
            }
        }
    }
    if (const json::Value* root = scene.find("root"); root != nullptr) {
        world.set_scene_root(root->as_string());
    }
    if (dropped > 0) {
        log::warn("a saved copy of the world no longer fits the component types; what doesn't "
                  "fit was left out",
                  {{"components", static_cast<std::int64_t>(dropped)}});
    }
    path.clear();
    return created.size();
}

// What an entity starts from before its own fields, in the world's IDs: its prefab's components,
// its scene's root, or for a part, what its scene gives it. Null for an entity that starts from
// nothing.
json::Value starting_components(const World& world, entt::entity entity) {
    const std::string id(world.id_of(entity));
    if (is_part_id(id)) {
        const std::string owner = id.substr(0, id.find('/'));
        const PrefabLink* link = world.prefab_of(world.find(owner));
        if (link == nullptr || link->base == nullptr || !is_scene_file(link->path)) {
            return {};
        }
        const json::Value* fields = link->base->find("entities")->find(id.substr(owner.size() + 1));
        return fields != nullptr
                   ? renamed(world.components(), *fields, instance_rename(*link->base, owner))
                   : json::Value();
    }
    const PrefabLink* link = world.prefab_of(entity);
    if (link == nullptr || link->base == nullptr) {
        return {};
    }
    if (!is_scene_file(link->path)) {
        return *link->base;
    }
    const json::Value& root =
        *link->base->find("entities")->find(link->base->find("root")->as_string());
    return renamed(world.components(), root, instance_rename(*link->base, id));
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
    const json::Value start = starting_components(world, entity);
    json::Value components = saved_components(world, entity, start.is_null() ? nullptr : &start);
    if (components.size() > 0) {
        entry["components"] = std::move(components);
    }
    return entry;
}

// Updates an entity's components to target (with prefabs applied), removing those it leaves out.
// Returns whether anything changed; a component that can't be set is reported under path.
bool update_components(World& world, entt::entity entity, const json::Value& target,
                       const std::string& path, std::vector<json::ValidationError>& errors) {
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
            errors.push_back({path, at + ": " + assigned.error().message});
            continue;
        }
        changed = true;
    }
    return changed;
}

// Brings an instance of a scene up to date with the scene's new contents, keeping what the
// instance changed: its own fields, and the parts it left out. Parts the scene no longer has go,
// and new ones come.
void reload_instance(World& world, entt::entity entity, const json::Value& before,
                     const json::Value& after, const std::string& path, PrefabReload& reload) {
    const std::string id(world.id_of(entity));
    const ComponentRegistry& registry = world.components();
    const EntityRename old_rename = instance_rename(before, id);
    const EntityRename new_rename = instance_rename(after, id);
    const std::string& old_root = before.find("root")->as_string();
    const std::string& new_root = after.find("root")->as_string();
    const json::Value& old_entities = *before.find("entities");
    const json::Value& new_entities = *after.find("entities");
    struct Target {
        std::string id;
        entt::entity entity;
        json::Value components;
        bool created = false;
    };
    std::vector<Target> targets;
    const auto keep = [&](const std::string& part, entt::entity which,
                          const json::Value& old_fields, const json::Value& new_fields) {
        const json::Value old_base = renamed(registry, old_fields, old_rename);
        const json::Value overrides = saved_components(world, which, &old_base);
        const json::Value new_base = renamed(registry, new_fields, new_rename);
        targets.push_back({part, which, effective_components(&new_base, &overrides), false});
    };
    keep(id, entity, *old_entities.find(old_root), *new_entities.find(new_root));
    for (const auto& [local, fields] : old_entities.as_object()) {
        if (local == old_root) {
            continue;
        }
        const std::string part = part_id(id, local);
        const entt::entity found = world.find(part);
        const json::Value* now = new_entities.find(local);
        if (now != nullptr && local != new_root) {
            if (found != entt::null) {
                keep(part, found, fields, *now);
            }
        } else if (found != entt::null) {
            world.destroy(found);
            reload.changed.push_back(part);
        }
    }
    for (const auto& [local, fields] : new_entities.as_object()) {
        if (local == new_root || (local != old_root && old_entities.find(local) != nullptr)) {
            continue;
        }
        const std::string part = part_id(id, local);
        auto made = world.create_part(part);
        if (!made) {
            reload.errors.push_back({path, made.error().message});
            continue;
        }
        targets.push_back({part, made.value(), renamed(registry, fields, new_rename), true});
    }
    // Once every part exists, so references to new ones resolve.
    for (const Target& target : targets) {
        if (update_components(world, target.entity, target.components, path, reload.errors) ||
            target.created) {
            reload.changed.push_back(target.id);
        }
    }
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

json::Value extends_schema(bool scenes) {
    json::Value extends;
    extends["type"] = "string";
    extends["description"] =
        scenes ? "A .prefab.json file, or a .scene.json file with a root, relative to the project "
                 "folder, e.g. \"prefabs/enemy.prefab.json\". Its components come first and the "
                 "fields given here override them. A scene's other entities come too, as "
                 "\"<this id>/<their id>\"."
               : "Another .prefab.json file relative to the project folder, e.g. "
                 "\"prefabs/enemy.prefab.json\". Its components come first and the fields given "
                 "here override them.";
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
    entity["properties"]["extends"] = extends_schema(true);

    json::Value ids;
    ids["minLength"] = 1;
    ids["maxLength"] = 255;
    ids["pattern"] = "^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*$";

    json::Value left_out;
    left_out["type"] = "null";
    left_out["description"] = "Leaves out this entity, which an instanced scene brings.";

    json::Value entities;
    entities["type"] = "object";
    entities["description"] =
        "Entities keyed by stable ID: 1 to 64 letters, digits, '_' or '-', e.g. \"player\". The "
        "entities an instanced scene brings are \"<instance id>/<their id>\", and their entries "
        "hold only what differs from the scene.";
    entities["propertyNames"] = std::move(ids);
    entities["additionalProperties"]["anyOf"] =
        json::Array{SchemaBuilder::reference(kEntityDefinition), std::move(left_out)};

    json::Value root;
    root["type"] = "string";
    root["description"] = "The entity an instance of this scene becomes when an entity in another "
                          "scene extends it; the scene's other entities come with it.";

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
    schema["properties"]["root"] = std::move(root);
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
    schema["properties"]["extends"] = extends_schema(false);
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
    const json::Object& entries = scene.find("entities")->as_object();
    for (const auto& [id, unused] : entries) {
        if (is_part_id(id) ? !World::is_valid_part_id(id) : !World::is_valid_id(id)) {
            errors.push_back({entity_path(id), "invalid entity id \"" + id +
                                                   "\": use 1 to 64 letters, digits, '_' or '-'"});
        }
    }
    if (const json::Value* root = scene.find("root"); root != nullptr) {
        if (const auto it = entries.find(root->as_string());
            it == entries.end() || !it->second.is_object() || is_part_id(root->as_string())) {
            errors.push_back({"/root", "no entity has the id \"" + root->as_string() + "\""});
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
    auto loaded = load_valid(world, saved, links, path, /*lenient=*/true);
    if (!loaded) {
        return located(path, loaded.error());
    }
    return loaded;
}

json::Value save_scene(const World& world) {
    json::Value entities = json::Object{};
    for (const std::string_view id : world.ids()) {
        const entt::entity entity = world.find(id);
        json::Value entry = saved_entry(world, entity);
        // A part its scene gives everything it has needs no entry.
        if (is_part_id(id) && entry.size() == 0 && !starting_components(world, entity).is_null()) {
            continue;
        }
        entities[id] = std::move(entry);
    }
    // Parts an instance left out.
    for (const auto [entity, link] : world.ecs().view<const PrefabLink>().each()) {
        const std::string_view id = world.id_of(entity);
        if (id.empty() || link.base == nullptr || !is_scene_file(link.path)) {
            continue;
        }
        const std::string& root = link.base->find("root")->as_string();
        for (const auto& [local, unused] : link.base->find("entities")->as_object()) {
            if (const std::string part = part_id(id, local);
                local != root && world.find(part) == entt::null) {
                entities[part] = json::Value();
            }
        }
    }
    json::Value scene;
    scene["entities"] = std::move(entities);
    scene["format"] = kSceneFormat;
    if (const std::string& root = world.scene_root();
        !root.empty() && world.find(root) != entt::null && !is_part_id(root)) {
        scene["root"] = root;
    }
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
    if (!is_scene_file(path)) {
        std::string at;
        if (auto assigned =
                assign_components(world, entity, base.value().get(), path, nullptr, "", at);
            !assigned) {
            return assigned.error();
        }
        world.ecs().emplace_or_replace<PrefabLink>(entity, std::string(path), base.value());
        return {};
    }
    // A scene: the entity becomes its root, and its other entities come too.
    const std::string id(world.id_of(entity));
    if (id.empty() || is_part_id(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "only an entity with an ID of its own can instance a scene");
    }
    const json::Value& tmpl = *base.value();
    const std::string& root = tmpl.find("root")->as_string();
    const EntityRename rename = instance_rename(tmpl, id);
    std::vector<entt::entity> parts;
    const auto fail = [&](const Error& error) {
        for (const entt::entity part : parts) {
            world.destroy(part);
        }
        return error;
    };
    std::vector<std::pair<entt::entity, json::Value>> targets;
    json::Value root_components;
    for (const auto& [local, fields] : tmpl.find("entities")->as_object()) {
        if (local == root) {
            root_components = renamed(world.components(), fields, rename);
            continue;
        }
        auto part = world.create_part(part_id(id, local));
        if (!part) {
            return fail(part.error());
        }
        parts.push_back(part.value());
        targets.emplace_back(part.value(), renamed(world.components(), fields, rename));
    }
    targets.emplace_back(entity, std::move(root_components));
    for (const auto& [which, components] : targets) {
        for (const auto& [name, fields] : components.as_object()) {
            const ComponentType* type = world.components().find(name);
            if (type == nullptr) {
                return fail(make_error(ErrorCode::NotFound, "unknown component \"" + name + "\""));
            }
            std::string at = "/components/" + json::pointer_escape(name);
            if (auto status = type->assign(world, which, fields, at); !status) {
                return fail(status.error());
            }
        }
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
            if (!world.valid(entity)) {
                continue;
            }
            const std::shared_ptr<const json::Value> before =
                world.ecs().get<PrefabLink>(entity).base;
            if (*before == *base) {
                world.ecs().get<PrefabLink>(entity).base = base;
                continue;
            }
            if (is_scene_file(path)) {
                reload_instance(world, entity, *before, *base, path, reload);
            } else {
                const json::Value overrides = saved_components(world, entity, before.get());
                const json::Value target = effective_components(base.get(), &overrides);
                if (const std::string_view id = world.id_of(entity);
                    update_components(world, entity, target, path, reload.errors) && !id.empty()) {
                    reload.changed.emplace_back(id);
                }
            }
            world.ecs().get<PrefabLink>(entity).base = base;
        }
    }
    std::ranges::sort(reload.changed);
    const auto [first, last] = std::ranges::unique(reload.changed);
    reload.changed.erase(first, last);
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
    // Each entity a scene makes, with what it extends applied, taking prefabs and scenes from the
    // links of the world it was saved from.
    const auto made = [&](const json::Value& scene, const PrefabLinks& links) {
        std::map<std::string, json::Value, std::less<>> entities;
        const json::Value* entries = object_at(&scene, "entities");
        if (entries == nullptr) {
            return entities;
        }
        const Resolver linked =
            [&](const std::string& id,
                const std::string& extends) -> Result<std::shared_ptr<const json::Value>> {
            const auto link = links.find(id);
            return link != links.end() && link->second.path == extends
                       ? link->second.base
                       : std::shared_ptr<const json::Value>();
        };
        std::string unused;
        if (auto expanded = expand(components, entries->as_object(), linked, unused, true)) {
            for (Made& entity : expanded.value()) {
                entities.emplace(entity.id, std::move(entity.components));
            }
        }
        return entities;
    };
    const auto old_entities = made(before, before_links);
    const auto new_entities = made(after, after_links);
    json::Array added;
    json::Array removed;
    json::Value changed = json::Object{};
    for (const auto& [id, unused] : old_entities) {
        if (!new_entities.contains(id)) {
            removed.emplace_back(id);
        }
    }
    for (const auto& [id, entity] : new_entities) {
        const auto old_entity = old_entities.find(id);
        const bool existed = old_entity != old_entities.end();
        if (!existed) {
            added.emplace_back(id);
        }
        json::Value changes =
            changed_components(components, existed ? &old_entity->second : nullptr, entity);
        if (changes.size() > 0) {
            changed[id] = std::move(changes);
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
