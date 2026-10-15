#include "slash/command/builtin.hpp"

#include "slash/command/session.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/core/version.hpp"
#include "slash/platform/file.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

using world::ComponentType;

constexpr std::int64_t kDefaultQueryLimit = 100;
constexpr std::int64_t kDefaultLogLines = 50;
constexpr double kDefaultTickSeconds = 1.0 / 60.0;
constexpr std::string_view kDefaultSnapshot = "default";
constexpr std::int64_t kDefaultEventLimit = 100;

// ---- Params schemas ----

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value any_property(std::string_view description) {
    json::Value schema;
    schema["description"] = description;
    return schema;
}

json::Value id_property(std::string_view description) {
    json::Value schema = property("string", description);
    schema["minLength"] = 1;
    schema["maxLength"] = 64;
    return schema;
}

json::Value names_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    return schema;
}

json::Value integer_property(std::string_view description, std::int64_t minimum,
                             std::int64_t maximum, std::int64_t fallback) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    schema["default"] = fallback;
    return schema;
}

json::Value components_property(std::string_view description) {
    json::Value schema = property("object", description);
    schema["additionalProperties"]["type"] = "object";
    return schema;
}

json::Value params_schema(json::Object properties = {}, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    if (!properties.empty()) {
        schema["properties"] = std::move(properties);
    }
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

// ---- Params access ----

bool flag(const json::Value& params, std::string_view key) {
    const json::Value* value = params.find(key);
    return value != nullptr && value->is_bool() && value->as_bool();
}

std::int64_t integer(const json::Value& params, std::string_view key, std::int64_t fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr && value->is_number() ? static_cast<std::int64_t>(value->as_double())
                                                  : fallback;
}

std::string text(const json::Value& params, std::string_view key) {
    const json::Value* value = params.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

// ---- Errors ----

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

template <typename Range>
void add_hint(std::string& message, std::string_view name, const Range& candidates) {
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
}

// An error about one value in the params, located by its JSON Pointer.
Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

Reply param_error(const Error& error, std::string_view path) {
    return param_error(Reply::failure(error).code, path, error.message);
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    add_hint(message, id, world.ids());
    return param_error(code::kNotFound, path, message);
}

Reply unknown_component(const world::ComponentRegistry& components, std::string_view name,
                        std::string_view path) {
    std::vector<std::string_view> names;
    for (const auto& type : components.types()) {
        names.emplace_back(type->name());
    }
    std::string message = "unknown component " + quoted(name);
    add_hint(message, name, names);
    return param_error(code::kNotFound, path, message);
}

std::string item_path(std::string_view base, std::size_t index) {
    return std::string(base) + "/" + std::to_string(index);
}

std::string member_path(std::string_view base, std::string_view key) {
    return std::string(base) + "/" + json::pointer_escape(key);
}

// ---- Components ----

// A standalone JSON Schema for one component's fields.
json::Value component_schema(const ComponentType& type) {
    world::SchemaBuilder builder;
    json::Value schema = type.schema(builder);
    schema["$defs"] = builder.definitions();
    return schema;
}

// Validates fields against the component's schema, then assigns them, adding the component if
// the entity lacks it. path is the JSON Pointer of fields inside the params.
std::optional<Reply> apply_component(world::World& world, entt::entity entity,
                                     const ComponentType& type, const json::Value& fields,
                                     const std::string& path) {
    std::vector<json::ValidationError> errors = json::validate(fields, component_schema(type));
    if (!errors.empty()) {
        for (json::ValidationError& error : errors) {
            error.path = path + error.path;
        }
        return Reply::invalid_params(errors);
    }
    std::string field_path = path;
    if (auto status = type.assign(world, entity, fields, field_path); !status) {
        return param_error(status.error(), field_path);
    }
    return std::nullopt;
}

// Looks up the component names in a params array; reports the first unknown one.
std::optional<Reply> lookup_components(const world::ComponentRegistry& components,
                                       const json::Value* names, std::string_view path,
                                       std::vector<const ComponentType*>& out) {
    if (names == nullptr) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < names->size(); ++i) {
        const std::string& name = names->as_array()[i].as_string();
        const ComponentType* type = components.find(name);
        if (type == nullptr) {
            return unknown_component(components, name, item_path(path, i));
        }
        out.push_back(type);
    }
    return std::nullopt;
}

// The entity's components as {"Type": {fields}}: only the listed types if any are given, every
// registered type otherwise.
json::Value components_json(const world::World& world, entt::entity entity,
                            const std::vector<const ComponentType*>& only, bool omit_defaults) {
    json::Value out = json::Object{};
    const auto add = [&](const ComponentType& type) {
        if (type.has(world, entity)) {
            out[type.name()] = omit_defaults ? type.save(world, entity) : type.get(world, entity);
        }
    };
    if (only.empty()) {
        for (const auto& type : world.components().types()) {
            add(*type);
        }
    } else {
        for (const ComponentType* type : only) {
            add(*type);
        }
    }
    return out;
}

// ---- Scene files ----

// Checks that a params path names a scene file inside the project and returns the full path.
Result<std::string> scene_file(const Session& session, std::string_view path) {
    if (!path.ends_with(world::kSceneSuffix) || path.size() == world::kSceneSuffix.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          R"(scene files end in ".scene.json", e.g. "scenes/main.scene.json")");
    }
    return session.resolve_path(path);
}

json::Value errors_json(const std::vector<json::ValidationError>& errors) {
    json::Array items;
    items.reserve(errors.size());
    for (const json::ValidationError& error : errors) {
        json::Value item;
        item["path"] = error.path;
        item["message"] = error.message;
        items.push_back(std::move(item));
    }
    return items;
}

// ---- Describe ----

Reply engine_describe(Session& session, const json::Value& params) {
    const bool full = flag(params, "full");
    json::Value result;
    result["engine"]["name"] = "SlashEngine";
    result["engine"]["version"] = version();

    json::Array granted;
    for (const Capability capability : session.options().capabilities) {
        granted.emplace_back(to_string(capability));
    }
    result["capabilities"] = std::move(granted);

    // Names only by default: MCP clients already have the descriptions as tool definitions.
    json::Array commands;
    for (const Command& command : session.commands().commands()) {
        if (!full) {
            commands.emplace_back(command.name);
            continue;
        }
        json::Value item;
        item["name"] = command.name;
        item["description"] = command.description;
        json::Array needs;
        for (const Capability capability : command.capabilities) {
            needs.emplace_back(to_string(capability));
        }
        item["capabilities"] = std::move(needs);
        item["params"] = command.params;
        commands.push_back(std::move(item));
    }
    result["commands"] = std::move(commands);

    json::Array components;
    for (const auto& type : session.components().types()) {
        json::Value item;
        item["name"] = type->name();
        if (!type->doc().empty()) {
            item["doc"] = type->doc();
        }
        item["defaults"] = type->defaults();
        if (full) {
            item["schema"] = component_schema(*type);
        }
        components.push_back(std::move(item));
    }
    result["components"] = std::move(components);

    // In the order they run.
    json::Array systems;
    for (const world::SystemInfo& system : session.world().systems()) {
        if (!full) {
            systems.emplace_back(system.name);
            continue;
        }
        json::Value item;
        item["name"] = system.name;
        if (!system.doc.empty()) {
            item["doc"] = system.doc;
        }
        item["priority"] = system.priority;
        systems.push_back(std::move(item));
    }
    result["systems"] = std::move(systems);

    result["scene"]["path"] = session.scene_path();
    result["scene"]["entities"] = session.world().size();
    result["scene"]["tick"] = session.world().tick();
    // Only hosts that tick the world on their own clock can play.
    if (session.commands().find("play") != nullptr) {
        result["scene"]["playing"] = session.playing();
    }
    return Reply::success(std::move(result));
}

Reply schema_get(Session& session, const json::Value& params) {
    const std::string kind = text(params, "kind");
    const std::string name = text(params, "name");
    if (kind == "scene") {
        return Reply::success(world::scene_schema(session.components()));
    }
    if (kind == "prefab") {
        return Reply::success(world::prefab_schema(session.components()));
    }
    if (kind == "storyboard") {
        return Reply::success(storyboard::file_schema());
    }
    if (name.empty()) {
        return param_error(code::kInvalidParams, "/name",
                           "name is required when kind is " + quoted(kind));
    }
    if (kind == "component") {
        const ComponentType* type = session.components().find(name);
        if (type == nullptr) {
            return unknown_component(session.components(), name, "/name");
        }
        json::Value schema = component_schema(*type);
        schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
        schema["title"] = name;
        return Reply::success(std::move(schema));
    }
    const Command* command = session.commands().find(name);
    if (command == nullptr) {
        std::vector<std::string_view> names;
        for (const Command& candidate : session.commands().commands()) {
            names.emplace_back(candidate.name);
        }
        std::string message = "unknown command " + quoted(name);
        add_hint(message, name, names);
        return param_error(code::kNotFound, "/name", message);
    }
    return Reply::success(command->params);
}

// ---- Scenes ----

Reply scene_get(Session& session, const json::Value& /*params*/) {
    json::Value result;
    result["path"] = session.scene_path();
    result["scene"] = world::save_scene(session.world());
    return Reply::success(std::move(result));
}

Reply scene_new(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    if (!path.empty()) {
        if (auto file = scene_file(session, path); !file) {
            return param_error(file.error(), "/path");
        }
    }
    session.world().clear();
    session.set_scene_path(path);
    json::Value result;
    result["path"] = path;
    return Reply::success(std::move(result));
}

Reply scene_open(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    auto file = scene_file(session, path);
    if (!file) {
        return param_error(file.error(), "/path");
    }
    auto contents = platform::read_file(file.value());
    if (!contents) {
        return param_error(contents.error(), "/path");
    }
    auto scene = json::parse(contents.value());
    if (!scene) {
        return param_error(code::kInvalidArgument, "/path",
                           path + " is not valid JSON: " + scene.error().message);
    }
    const auto errors =
        world::validate_scene(session.components(), scene.value(), session.world().file_reader());
    if (!errors.empty()) {
        std::string message =
            path + " is not a valid scene: " +
            (errors.front().path.empty() ? std::string("(root)") : errors.front().path) + ": " +
            errors.front().message;
        if (errors.size() > 1) {
            message += " (and " + std::to_string(errors.size() - 1) + " more errors)";
        }
        json::Value data;
        data["file"] = path;
        data["file_errors"] = errors_json(errors);
        return Reply::failure(code::kInvalidArgument, std::move(message), std::move(data));
    }
    session.world().clear();
    auto loaded = world::load_scene(session.world(), scene.value());
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    session.set_scene_path(path);
    json::Value result;
    result["path"] = path;
    result["entities"] = loaded.value();
    return Reply::success(std::move(result));
}

// Writes a file, creating its folder if needed.
Result<void> write_file(const std::string& full, const std::string& contents) {
    if (const std::size_t slash = full.find_last_of('/');
        slash != std::string::npos && slash > 0 && !platform::exists(full.substr(0, slash))) {
        if (auto made = platform::create_directories(full.substr(0, slash)); !made) {
            return made.error();
        }
    }
    return platform::write_file(full, contents);
}

Reply scene_save(Session& session, const json::Value& params) {
    std::string path = text(params, "path");
    if (path.empty()) {
        path = session.scene_path();
    }
    if (path.empty()) {
        return param_error(code::kInvalidArgument, "/path",
                           "the scene has no file yet; pass a path such as "
                           "\"scenes/main.scene.json\"");
    }
    auto file = scene_file(session, path);
    if (!file) {
        return param_error(file.error(), "/path");
    }
    const std::string contents = json::write(world::save_scene(session.world())) + "\n";
    if (!session.dry_running()) {
        if (auto written = write_file(file.value(), contents); !written) {
            return Reply::failure(written.error());
        }
    }
    session.set_scene_path(path);
    json::Value result;
    result["path"] = path;
    result["entities"] = session.world().size();
    return Reply::success(std::move(result));
}

// ---- Entities ----

Reply entity_create(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::entity entity = entt::null;
    if (const json::Value* id = params.find("id"); id != nullptr) {
        auto created = world.create(id->as_string());
        if (!created) {
            return param_error(created.error(), "/id");
        }
        entity = created.value();
    } else {
        entity = world.create();
    }
    if (const std::string extends = text(params, "extends"); !extends.empty()) {
        if (auto made = world::instantiate_prefab(world, entity, extends); !made) {
            return param_error(made.error(), "/extends");
        }
    }
    if (const json::Value* components = params.find("components"); components != nullptr) {
        for (const auto& [name, fields] : components->as_object()) {
            const std::string path = member_path("/components", name);
            const ComponentType* type = world.components().find(name);
            if (type == nullptr) {
                return unknown_component(world.components(), name, path);
            }
            if (auto failed = apply_component(world, entity, *type, fields, path)) {
                return std::move(*failed);
            }
        }
    }
    json::Value result;
    result["id"] = world.id_of(entity);
    return Reply::success(std::move(result));
}

Reply entity_destroy(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    const entt::entity entity = session.world().find(id);
    if (entity == entt::null) {
        return unknown_entity(session.world(), id, "/id");
    }
    session.world().destroy(entity);
    json::Value result;
    result["destroyed"] = id;
    return Reply::success(std::move(result));
}

Reply entity_get(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    std::vector<const ComponentType*> only;
    if (auto failed =
            lookup_components(world.components(), params.find("components"), "/components", only)) {
        return std::move(*failed);
    }
    json::Value result;
    result["id"] = id;
    if (const world::PrefabLink* link = world.prefab_of(entity); link != nullptr) {
        result["extends"] = link->path;
    }
    result["components"] = components_json(world, entity, only, flag(params, "omit_defaults"));
    return Reply::success(std::move(result));
}

// ---- Prefabs ----

Reply prefab_save(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    const entt::entity entity = session.world().find(id);
    if (entity == entt::null) {
        return unknown_entity(session.world(), id, "/id");
    }
    const std::string path = text(params, "path");
    if (!path.ends_with(world::kPrefabSuffix) || path.size() == world::kPrefabSuffix.size()) {
        return param_error(
            code::kInvalidArgument, "/path",
            R"(prefab files end in ".prefab.json", e.g. "prefabs/enemy.prefab.json")");
    }
    auto file = session.resolve_path(path);
    if (!file) {
        return param_error(file.error(), "/path");
    }
    const json::Value prefab = world::save_prefab(session.world(), entity);
    const std::string contents = json::write(prefab) + "\n";
    // Load it as it would be read back, which catches a prefab that would extend itself.
    const world::FileReader& files = session.world().file_reader();
    const world::FileReader read_back = [&](std::string_view name) -> Result<std::string> {
        return name == path ? Result<std::string>(contents) : files(name);
    };
    if (const auto errors = world::validate_prefab(session.components(), prefab, read_back);
        !errors.empty()) {
        return param_error(code::kInvalidArgument, "/path",
                           path + " would not load: " + errors.front().message);
    }
    world::World scratch(session.components());
    scratch.set_file_reader(read_back);
    auto base = world::resolve_prefab(scratch, path);
    SLASH_ASSERT(base.ok(), "a validated prefab failed to resolve");
    if (!session.dry_running()) {
        if (auto written = write_file(file.value(), contents); !written) {
            return Reply::failure(written.error());
        }
    }
    // The rest of this command, such as a batch making instances, sees the new prefab.
    session.world().forget_prefabs();
    session.world().cache_prefab(path, std::move(base).value());
    json::Value result;
    result["path"] = path;
    if (const json::Value* extends = prefab.find("extends"); extends != nullptr) {
        result["extends"] = *extends;
    }
    json::Array names;
    for (const auto& type : session.components().types()) {
        if (type->has(session.world(), entity)) {
            names.emplace_back(type->name());
        }
    }
    result["components"] = std::move(names);
    return Reply::success(std::move(result));
}

// ---- Assets ----

Reply asset_list(Session& session, const json::Value& params) {
    const std::string type = text(params, "type");
    // A prefab's references are the entities that extend it.
    std::map<std::string, std::size_t, std::less<>> instances;
    for (const auto [entity, link] : session.world().ecs().view<const world::PrefabLink>().each()) {
        ++instances[link.path];
    }
    json::Array assets;
    for (const asset::Assets::Info& info : session.assets().list()) {
        if (!type.empty() && info.type != type) {
            continue;
        }
        json::Value item;
        item["path"] = info.path;
        item["type"] = info.type;
        item["version"] = info.version;
        std::size_t refs = info.refs;
        if (info.type == world::kPrefabType) {
            const auto it = instances.find(info.path);
            refs = it != instances.end() ? it->second : 0;
        }
        item["refs"] = refs;
        assets.push_back(std::move(item));
    }
    json::Value result;
    result["assets"] = std::move(assets);
    return Reply::success(std::move(result));
}

// ---- Storyboard ----

// The storyboard's entries, or a failure listing every problem in its files.
std::optional<Reply> load_storyboard(Session& session, json::Object& entries) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    const std::vector<storyboard::FileError>& errors = loaded.value().errors;
    if (!errors.empty()) {
        json::Array items;
        for (const storyboard::FileError& error : errors) {
            json::Value item;
            item["file"] = error.file;
            item["path"] = error.path;
            item["message"] = error.message;
            items.push_back(std::move(item));
        }
        json::Value data;
        data["errors"] = std::move(items);
        const storyboard::FileError& first = errors.front();
        std::string message =
            first.file + (first.path.empty() ? "" : " at " + first.path) + ": " + first.message;
        if (errors.size() > 1) {
            message += " (and " + std::to_string(errors.size() - 1) + " more errors)";
        }
        return Reply::failure(code::kInvalidArgument,
                              message + "; fix the storyboard file, then retry", std::move(data));
    }
    entries = std::move(loaded.value().entries);
    return std::nullopt;
}

// How many lines the journal has: a later brief can ask what changed since.
Result<std::size_t> journal_seq(Session& session) {
    auto journal = session.read_project_file(storyboard::kJournalPath);
    if (!journal) {
        return journal.error();
    }
    return storyboard::journal_length(journal.value().value_or(std::string()));
}

Reply storyboard_query(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }
    storyboard::Query query;
    std::vector<json::ValidationError> unknown;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        for (std::size_t i = 0; i < ids->size(); ++i) {
            const std::string& id = ids->as_array()[i].as_string();
            if (!entries.contains(id)) {
                std::string message = storyboard::id_error(id);
                unknown.push_back({"/ids/" + std::to_string(i),
                                   message.empty() ? storyboard::unknown_entry(entries, id)
                                                   : std::move(message)});
            }
            query.ids.push_back(id);
        }
    }
    if (!unknown.empty()) {
        Reply reply = Reply::invalid_params(unknown);
        reply.code = code::kNotFound;
        return reply;
    }
    query.kind = text(params, "kind");
    query.text = text(params, "text");
    query.linked = text(params, "linked");
    query.full = flag(params, "full");
    auto seq = journal_seq(session);
    if (!seq) {
        return Reply::failure(seq.error());
    }
    json::Value result;
    result["entries"] = storyboard::query(entries, query);
    result["seq"] = seq.value();
    return Reply::success(std::move(result));
}

Reply storyboard_edit(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }
    storyboard::EditResult edited = storyboard::apply_edits(entries, *params.find("edits"));
    if (!edited.errors.empty()) {
        Reply reply = Reply::invalid_params(edited.errors);
        if (!edited.current.is_null()) {
            reply.code = code::kConflict;
            reply.data["current"] = std::move(edited.current);
        }
        return reply;
    }

    // Rewrite each file an edit touched, and record the new versions in the journal.
    std::set<std::string_view> files;
    json::Object changes;
    for (const auto& [id, unused] : edited.previous) {
        files.insert(storyboard::kind_of(id)->file);
        const auto it = entries.find(id);
        changes[id] =
            it != entries.end() ? json::Value(storyboard::version(it->second)) : json::Value();
    }
    for (const std::string_view file : files) {
        json::Object in_file;
        for (const auto& [id, entry] : entries) {
            if (storyboard::kind_of(id)->file == file) {
                in_file.emplace(id, entry);
            }
        }
        if (auto staged =
                session.stage_file(storyboard::file_path(file), storyboard::format_file(in_file));
            !staged) {
            return Reply::failure(staged.error());
        }
    }
    const std::string why = text(params, "why");
    std::string by = text(params, "by");
    if (by.empty()) {
        by = session.client().empty() ? "unknown" : session.client();
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (auto logged = session.stage_append(storyboard::kJournalPath,
                                           storyboard::journal_line(by, why, changes, now) + "\n");
        !logged) {
        return Reply::failure(logged.error());
    }

    // Undo puts the entries back as they were, and redo makes this edit again; both are
    // refused if an entry changed in between. Whoever undoes or redoes is the journal's "by".
    json::Value undo;
    undo["edits"] = storyboard::reverse_edits(entries, edited.previous);
    undo["why"] = why.empty() ? std::string("undo") : "undo: " + why;
    json::Value redo = params;
    redo.as_object().erase("by");
    redo["why"] = why.empty() ? std::string("redo") : "redo: " + why;
    session.add_undo_call("storyboard.edit", std::move(undo), std::move(redo));

    auto seq = journal_seq(session);
    if (!seq) {
        return Reply::failure(seq.error());
    }
    json::Value result;
    result["changes"] = std::move(changes);
    result["seq"] = seq.value();
    return Reply::success(std::move(result));
}

// ---- Components ----

Reply component_set(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    json::Value changed = json::Object{};
    json::Array added;
    for (const auto& [name, fields] : params.find("components")->as_object()) {
        const std::string path = member_path("/components", name);
        const ComponentType* type = world.components().find(name);
        if (type == nullptr) {
            return unknown_component(world.components(), name, path);
        }
        const bool had = type->has(world, entity);
        const json::Value before = had ? type->get(world, entity) : type->defaults();
        if (auto failed = apply_component(world, entity, *type, fields, path)) {
            return std::move(*failed);
        }
        if (!had) {
            added.emplace_back(name);
        }
        const json::Value after = type->get(world, entity);
        json::Value differences = json::Object{};
        for (const auto& [field, value] : after.as_object()) {
            const json::Value* old = before.find(field);
            if (old == nullptr || !(*old == value)) {
                differences[field] = value;
            }
        }
        if (differences.size() > 0) {
            changed[name] = std::move(differences);
        }
    }
    json::Value result;
    result["id"] = id;
    result["changed"] = std::move(changed);
    if (!added.empty()) {
        result["added"] = std::move(added);
    }
    return Reply::success(std::move(result));
}

Reply component_remove(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    std::vector<const ComponentType*> types;
    if (auto failed = lookup_components(world.components(), params.find("components"),
                                        "/components", types)) {
        return std::move(*failed);
    }
    json::Array removed;
    for (const ComponentType* type : types) {
        if (type->has(world, entity)) {
            type->remove(world, entity);
            removed.emplace_back(type->name());
        }
    }
    json::Value result;
    result["id"] = id;
    result["removed"] = std::move(removed);
    return Reply::success(std::move(result));
}

// ---- Queries ----

struct Condition {
    const ComponentType* type = nullptr;
    std::vector<std::string> segments; // field name, then array indices or nested field names
    std::string op;
    json::Value value;
};

std::vector<std::string> split_field(std::string_view field) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= field.size()) {
        const std::size_t end = std::min(field.find('.', start), field.size());
        segments.emplace_back(field.substr(start, end - start));
        start = end + 1;
    }
    return segments;
}

// Steps into an array element ("0", "1", ... or "x", "y", "z", "w") or a nested field.
const json::Value* step_into(const json::Value& value, const std::string& segment) {
    if (value.is_object()) {
        return value.find(segment);
    }
    if (!value.is_array() || segment.empty()) {
        return nullptr;
    }
    constexpr std::array<std::string_view, 4> kAxes{"x", "y", "z", "w"};
    auto index = static_cast<std::size_t>(
        std::ranges::distance(kAxes.begin(), std::ranges::find(kAxes, segment)));
    if (index == kAxes.size()) {
        const char* end = segment.data() + segment.size();
        const auto [last, error] = std::from_chars(segment.data(), end, index);
        if (error != std::errc() || last != end) {
            return nullptr;
        }
    }
    return index < value.size() ? &value.as_array()[index] : nullptr;
}

bool compare(const json::Value& actual, const std::string& op, const json::Value& expected) {
    if (op == "==") {
        return actual == expected;
    }
    if (op == "!=") {
        return !(actual == expected);
    }
    int order = 0;
    if (actual.is_number() && expected.is_number()) {
        const double a = actual.as_double();
        const double b = expected.as_double();
        order = static_cast<int>(a > b) - static_cast<int>(a < b);
    } else if (actual.is_string() && expected.is_string()) {
        const int c = actual.as_string().compare(expected.as_string());
        order = static_cast<int>(c > 0) - static_cast<int>(c < 0);
    } else {
        return false;
    }
    if (op == "<") {
        return order < 0;
    }
    if (op == "<=") {
        return order <= 0;
    }
    if (op == ">") {
        return order > 0;
    }
    return order >= 0;
}

bool matches(const world::World& world, entt::entity entity, const Condition& condition) {
    if (!condition.type->has(world, entity)) {
        return false;
    }
    const json::Value fields = condition.type->get(world, entity);
    const json::Value* value = &fields;
    for (const std::string& segment : condition.segments) {
        value = step_into(*value, segment);
        if (value == nullptr) {
            return false;
        }
    }
    return compare(*value, condition.op, condition.value);
}

Reply world_query(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const world::ComponentRegistry& components = world.components();

    std::vector<const ComponentType*> with;
    std::vector<const ComponentType*> without;
    if (auto failed = lookup_components(components, params.find("with"), "/with", with)) {
        return std::move(*failed);
    }
    if (auto failed = lookup_components(components, params.find("without"), "/without", without)) {
        return std::move(*failed);
    }

    std::vector<Condition> conditions;
    if (const json::Value* where = params.find("where"); where != nullptr) {
        for (std::size_t i = 0; i < where->size(); ++i) {
            const json::Value& item = where->as_array()[i];
            const std::string base = item_path("/where", i);
            Condition condition;
            const std::string name = text(item, "component");
            condition.type = components.find(name);
            if (condition.type == nullptr) {
                return unknown_component(components, name, base + "/component");
            }
            condition.segments = split_field(text(item, "field"));
            const std::string& field = condition.segments.front();
            const auto names = condition.type->fields();
            if (std::ranges::find(names, field) == names.end()) {
                std::string message = name + " has no field " + quoted(field);
                add_hint(message, field, names);
                return param_error(code::kInvalidParams, base + "/field", message);
            }
            condition.op = item.find("op") != nullptr ? text(item, "op") : "==";
            condition.value = *item.find("value");
            if (condition.op != "==" && condition.op != "!=" && !condition.value.is_number() &&
                !condition.value.is_string()) {
                return param_error(code::kInvalidParams, base + "/value",
                                   "\"" + condition.op + "\" compares numbers or strings");
            }
            conditions.push_back(std::move(condition));
        }
    }

    // include: true returns every component, a list returns those, absent returns only IDs.
    const json::Value* include = params.find("include");
    std::vector<const ComponentType*> included;
    bool with_components = false;
    if (include != nullptr && include->is_array()) {
        if (auto failed = lookup_components(components, include, "/include", included)) {
            return std::move(*failed);
        }
        with_components = true;
    } else if (include != nullptr && include->is_bool()) {
        with_components = include->as_bool();
    }
    const bool omit_defaults = flag(params, "omit_defaults");
    const auto limit = static_cast<std::size_t>(integer(params, "limit", kDefaultQueryLimit));

    std::size_t count = 0;
    json::Array entities;
    for (const std::string_view id : world.ids()) {
        const entt::entity entity = world.find(id);
        const bool match =
            std::ranges::all_of(with, [&](const auto* t) { return t->has(world, entity); }) &&
            std::ranges::none_of(without, [&](const auto* t) { return t->has(world, entity); }) &&
            std::ranges::all_of(conditions,
                                [&](const Condition& c) { return matches(world, entity, c); });
        if (!match) {
            continue;
        }
        ++count;
        if (entities.size() >= limit) {
            continue;
        }
        if (!with_components) {
            entities.emplace_back(id);
            continue;
        }
        json::Value item;
        item["id"] = id;
        item["components"] = components_json(world, entity, included, omit_defaults);
        entities.push_back(std::move(item));
    }

    json::Value result;
    result["count"] = count;
    result["truncated"] = count > entities.size();
    result["entities"] = std::move(entities);
    return Reply::success(std::move(result));
}

// ---- Simulation and history ----

Reply step(Session& session, const json::Value& params) {
    const std::int64_t ticks = integer(params, "ticks", 1);
    const json::Value* seconds = params.find("tick_seconds");
    const double tick_seconds = seconds != nullptr ? seconds->as_double() : kDefaultTickSeconds;
    for (std::int64_t i = 0; i < ticks; ++i) {
        session.world().step(tick_seconds);
    }
    json::Value result;
    result["ticks"] = ticks;
    result["tick"] = session.world().tick();
    return Reply::success(std::move(result));
}

Reply run_state(Session& session, bool playing) {
    session.set_playing(playing);
    json::Value result;
    result["playing"] = playing;
    result["tick"] = session.world().tick();
    return Reply::success(std::move(result));
}

Reply history_get(Session& session, const json::Value& /*params*/) {
    json::Array undo;
    for (std::string& label : session.undo_labels()) {
        undo.emplace_back(std::move(label));
    }
    json::Array redo;
    for (std::string& label : session.redo_labels()) {
        redo.emplace_back(std::move(label));
    }
    json::Value result;
    result["undo"] = std::move(undo);
    result["redo"] = std::move(redo);
    return Reply::success(std::move(result));
}

Reply batch(Session& session, const json::Value& params) {
    const json::Array& commands = params.find("commands")->as_array();
    json::Array results;
    results.reserve(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const json::Value& item = commands[i];
        const std::string name = text(item, "command");
        const std::string base = item_path("/commands", i);
        const Command* command = session.commands().find(name);
        if (command != nullptr &&
            (command->effect == Effect::History || command->effect == Effect::Compound)) {
            return param_error(code::kInvalidParams, base + "/command",
                               quoted(name) + " cannot run inside a batch");
        }
        const json::Value* inner = item.find("params");
        Reply reply = session.call(name, inner != nullptr ? *inner : json::Value());
        if (!reply.ok) {
            // Point paths in the details at the batch's own params.
            json::Value data = reply.data.is_object() ? std::move(reply.data) : json::Object{};
            if (json::Value* path = data.find("path"); path != nullptr && path->is_string()) {
                *path = base + "/params" + path->as_string();
            }
            if (json::Value* errors = data.find("errors");
                errors != nullptr && errors->is_array()) {
                for (json::Value& error : errors->as_array()) {
                    if (json::Value* path = error.find("path");
                        path != nullptr && path->is_string()) {
                        *path = base + "/params" + path->as_string();
                    }
                }
            }
            data["index"] = i;
            data["command"] = name;
            // The batch command exists, so an unknown inner command is a params error.
            const int error_code =
                reply.code == code::kMethodNotFound ? code::kInvalidParams : reply.code;
            std::string message = base;
            message += " (" + name + ") failed, so nothing was changed. ";
            message += reply.message;
            return Reply::failure(error_code, std::move(message), std::move(data));
        }
        results.push_back(std::move(reply.value));
    }
    json::Value result;
    result["results"] = std::move(results);
    return Reply::success(std::move(result));
}

// ---- Snapshots ----

std::string snapshot_name(const json::Value& params, std::string_view key) {
    const std::string name = text(params, key);
    return name.empty() ? std::string(kDefaultSnapshot) : name;
}

// The snapshot named by params[key], or an error naming the closest snapshot.
const Session::SavedWorld* find_snapshot(const Session& session, const json::Value& params,
                                         std::string_view key, Reply& error) {
    const std::string name = snapshot_name(params, key);
    if (const Session::SavedWorld* snapshot = session.find_snapshot(name); snapshot != nullptr) {
        return snapshot;
    }
    const std::vector<std::string_view> names = session.snapshot_names();
    std::string message = "no snapshot is named " + quoted(name);
    if (names.empty()) {
        message += "; take one with world.snapshot first";
    } else {
        add_hint(message, name, names);
    }
    error = param_error(code::kNotFound, "/" + std::string(key), message);
    return nullptr;
}

Reply world_snapshot(Session& session, const json::Value& params) {
    std::string name = snapshot_name(params, "name");
    if (auto taken = session.take_snapshot(name); !taken) {
        return param_error(taken.error(), "/name");
    }
    json::Value result;
    result["name"] = std::move(name);
    result["entities"] = session.world().size();
    return Reply::success(std::move(result));
}

Reply world_diff(Session& session, const json::Value& params) {
    Reply error;
    const Session::SavedWorld* from = find_snapshot(session, params, "from", error);
    if (from == nullptr) {
        return error;
    }
    Session::SavedWorld now;
    const Session::SavedWorld* to = &now;
    if (params.find("to") == nullptr) {
        now = session.save();
    } else if (to = find_snapshot(session, params, "to", error); to == nullptr) {
        return error;
    }
    return Reply::success(
        world::diff_scenes(session.components(), from->scene, to->scene, from->links, to->links));
}

Reply world_restore(Session& session, const json::Value& params) {
    Reply error;
    const Session::SavedWorld* snapshot = find_snapshot(session, params, "name", error);
    if (snapshot == nullptr) {
        return error;
    }
    session.restore(*snapshot);
    json::Value result;
    result["restored"] = snapshot_name(params, "name");
    result["entities"] = session.world().size();
    return Reply::success(std::move(result));
}

// ---- Events ----

std::vector<std::string> strings(const json::Value& params, std::string_view key) {
    std::vector<std::string> out;
    if (const json::Value* list = params.find(key); list != nullptr) {
        for (const json::Value& item : list->as_array()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

Reply unknown_subscription(const Session& session, std::string_view name) {
    const std::vector<std::string_view> names = session.subscription_names();
    std::string message = "no subscription is named " + quoted(name);
    if (names.empty()) {
        message += "; start one with events.subscribe";
    } else {
        add_hint(message, name, names);
    }
    return param_error(code::kNotFound, "/subscription", message);
}

Reply events_subscribe(Session& session, const json::Value& params) {
    std::vector<std::string> kinds = strings(params, "kinds");
    // The engine's own kinds are checked, so a misspelled one doesn't silently match nothing.
    constexpr std::array<std::string_view, 3> kEngineKinds{world::kEntityCreated,
                                                           world::kEntityDestroyed, "command"};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const std::string& kind = kinds[i];
        if (kind.starts_with("entity.") &&
            std::ranges::find(kEngineKinds, kind) == kEngineKinds.end()) {
            std::string message = "unknown event kind " + quoted(kind);
            add_hint(message, kind, kEngineKinds);
            return param_error(code::kInvalidParams, item_path("/kinds", i), message);
        }
    }
    auto name = session.subscribe(std::move(kinds), strings(params, "ids"));
    if (!name) {
        return Reply::failure(name.error());
    }
    json::Value result;
    result["subscription"] = std::move(name).value();
    return Reply::success(std::move(result));
}

Reply events_read(Session& session, const json::Value& params) {
    const std::string name = text(params, "subscription");
    auto batch = session.read_events(
        name, static_cast<std::size_t>(integer(params, "limit", kDefaultEventLimit)));
    if (!batch) {
        return unknown_subscription(session, name);
    }
    json::Value result;
    result["events"] = std::move(batch->events);
    result["dropped"] = batch->dropped;
    result["more"] = batch->more;
    return Reply::success(std::move(result));
}

Reply events_unsubscribe(Session& session, const json::Value& params) {
    const std::string name = text(params, "subscription");
    if (!session.unsubscribe(name)) {
        return unknown_subscription(session, name);
    }
    json::Value result;
    result["unsubscribed"] = name;
    return Reply::success(std::move(result));
}

// ---- Logs ----

int level_rank(std::string_view level) {
    constexpr std::array<std::string_view, 4> kLevels{"debug", "info", "warn", "error"};
    const auto index = std::ranges::distance(kLevels.begin(), std::ranges::find(kLevels, level));
    return index == std::ssize(kLevels) ? 0 : static_cast<int>(index);
}

Reply log_tail(Session& session, const json::Value& params) {
    const auto wanted = static_cast<std::size_t>(integer(params, "lines", kDefaultLogLines));
    const int minimum = level_rank(text(params, "level"));
    json::Array lines;
    // Filter from the newest line back, so the count applies after filtering.
    const std::vector<std::string> recent = session.recent_logs(SIZE_MAX);
    for (auto it = recent.rbegin(); it != recent.rend() && lines.size() < wanted; ++it) {
        auto parsed = json::parse(*it);
        if (!parsed || !parsed.value().is_object()) {
            if (minimum == 0) {
                lines.emplace_back(*it);
            }
            continue;
        }
        if (level_rank(text(parsed.value(), "level")) >= minimum) {
            lines.push_back(std::move(parsed).value());
        }
    }
    std::ranges::reverse(lines);
    json::Value result;
    result["lines"] = std::move(lines);
    return Reply::success(std::move(result));
}

// ---- Registration ----

json::Value where_schema() {
    json::Value op = property("string", "Comparison; \"==\" if left out. Ordering operators "
                                        "compare numbers or strings.");
    op["enum"] = json::Array{"==", "!=", "<", "<=", ">", ">="};
    json::Value item = params_schema(
        {
            {"component", property("string", "Component type, e.g. \"Transform2D\".")},
            {"field", property("string", "Field name. Add \".x\", \".y\", \".z\", \".w\" or "
                                         "\".<index>\" for one element of a vector, e.g. "
                                         "\"position.y\".")},
            {"op", std::move(op)},
            {"value", any_property("Value to compare against.")},
        },
        {"component", "field", "value"});
    json::Value schema = property("array", "Field conditions; an entity must meet all of them.");
    schema["items"] = std::move(item);
    return schema;
}

json::Value include_schema() {
    json::Value names;
    names["type"] = "array";
    names["items"]["type"] = "string";
    json::Value boolean;
    boolean["type"] = "boolean";
    json::Value schema;
    schema["description"] = "Components to return with each entity: true for all, or a list "
                            "of names. Left out, only IDs are returned, which is cheapest.";
    schema["anyOf"] = json::Array{std::move(boolean), std::move(names)};
    return schema;
}

json::Value batch_schema() {
    json::Value item = params_schema(
        {
            {"command", property("string", "Command name, e.g. \"entity.create\".")},
            {"params", property("object", "The command's params.")},
        },
        {"command"});
    json::Value commands = property("array", "Commands to run in order.");
    commands["items"] = std::move(item);
    commands["minItems"] = 1;
    commands["maxItems"] = 1000;
    return params_schema({{"commands", std::move(commands)}}, {"commands"});
}

} // namespace

void register_builtin_commands(Registry& registry) {
    using enum Capability;

    json::Value full = property("boolean", "Also return each command's description, capabilities "
                                           "and params schema, and each component's field "
                                           "schema with docs and ranges.");
    registry.add({
        .name = "engine.describe",
        .description = "Lists the components with their default field values, the command "
                       "names, the granted capabilities and the open scene. Call this first.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"full", std::move(full)}}),
        .handler = engine_describe,
    });

    json::Value kind = property("string", "\"scene\", \"prefab\" or \"storyboard\" for "
                                          "those files, \"component\" for one component's "
                                          "fields, \"command\" for a command's params.");
    kind["enum"] = json::Array{"scene", "prefab", "storyboard", "component", "command"};
    registry.add({
        .name = "schema.get",
        .description = "Returns a JSON Schema: for scene, prefab or storyboard files, one "
                       "component or one command.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"kind", std::move(kind)},
                {"name", property("string", "Component or command name, e.g. \"Transform2D\" "
                                            "or \"entity.create\".")},
            },
            {"kind"}),
        .handler = schema_get,
    });

    registry.add({
        .name = "scene.get",
        .description = "Returns the whole open scene in scene-file form, with default fields "
                       "left out. Prefer world.query or entity.get for parts of it.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = scene_get,
    });

    registry.add({
        .name = "scene.new",
        .description = "Replaces the open scene with an empty one and clears undo history.",
        .capabilities = {Edit},
        .effect = Effect::Replace,
        .params = params_schema({{"path", property("string", "Where scene.save will write it, "
                                                             "relative to the project folder, "
                                                             "e.g. \"scenes/level2.scene.json\"."
                                                             " Nothing is written yet.")}}),
        .handler = scene_new,
    });

    registry.add({
        .name = "scene.open",
        .description = "Loads a .scene.json file in place of the open scene and clears undo "
                       "history. An invalid file changes nothing and every error is listed.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Replace,
        .params = params_schema({{"path", property("string", "Scene file relative to the project "
                                                             "folder, e.g. "
                                                             "\"scenes/main.scene.json\".")}},
                                {"path"}),
        .handler = scene_open,
    });

    registry.add({
        .name = "scene.save",
        .description = "Writes the open scene to a .scene.json file in canonical form, creating "
                       "folders as needed.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = params_schema({{"path", property("string", "File relative to the project "
                                                             "folder. Defaults to the scene's "
                                                             "current file.")}}),
        .handler = scene_save,
    });

    registry.add({
        .name = "entity.create",
        .description = "Creates an entity, optionally from a prefab and with components. Returns "
                       "its ID.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema({
            {"id", id_property("Stable ID: 1 to 64 letters, digits, '_' or '-'. Generated if "
                               "left out.")},
            {"extends", property("string", "A .prefab.json file to start from, e.g. "
                                           "\"prefabs/enemy.prefab.json\".")},
            {"components",
             components_property("Components keyed by type name, e.g. {\"Transform2D\": "
                                 "{\"position\": [64, 32]}}. Fields left out keep their "
                                 "defaults, or the prefab's.")},
        }),
        .handler = entity_create,
    });

    registry.add({
        .name = "entity.destroy",
        .description = "Destroys an entity and its components.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema({{"id", id_property("Entity ID.")}}, {"id"}),
        .handler = entity_destroy,
    });

    json::Value omit_defaults = property("boolean", "Leave out fields equal to their defaults.");
    registry.add({
        .name = "entity.get",
        .description = "Returns an entity's components.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property("Entity ID.")},
                {"components", names_property("Only these components. All if left out.")},
                {"omit_defaults", omit_defaults},
            },
            {"id"}),
        .handler = entity_get,
    });

    registry.add({
        .name = "prefab.save",
        .description = "Writes an entity to a .prefab.json file that entities can extend, "
                       "creating folders as needed.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property("Entity ID.")},
                {"path", property("string", "File relative to the project folder, e.g. "
                                            "\"prefabs/enemy.prefab.json\".")},
            },
            {"id", "path"}),
        .handler = prefab_save,
    });

    registry.add({
        .name = "asset.list",
        .description = "Lists loaded assets and the prefab files entities extend, with each one's "
                       "type, version (one more per reload) and refs (handles, or instances).",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"type", property("string", "Only assets of this type, e.g. "
                                                             "\"prefab\" or \"json\".")}}),
        .handler = asset_list,
    });

    json::Value set_components =
        components_property("Fields to set, keyed by component type, e.g. {\"Transform2D\": "
                            "{\"rotation\": 1.57}}. Fields left out are unchanged; missing "
                            "components are added.");
    set_components["minProperties"] = 1;
    json::Array kind_names;
    for (const storyboard::Kind& each : storyboard::kinds()) {
        kind_names.emplace_back(each.name);
    }
    json::Value story_kind = property("string", "Only entries of this kind.");
    story_kind["enum"] = std::move(kind_names);
    registry.add({
        .name = "storyboard.query",
        .description = "Finds storyboard entries: the game's style, world, story, design, names, "
                       "decisions and open work. Returns IDs, versions, names and summaries; "
                       "full: true returns every field.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"ids", names_property("Only these entries, e.g. [\"character/mira\"].")},
            {"kind", std::move(story_kind)},
            {"text", property("string", "Only entries with this text in a field, ignoring "
                                        "case.")},
            {"linked", property("string", "Only entries whose links name this project path, "
                                          "or whose refs name this entry ID.")},
            {"full", property("boolean", "Return every field, not only name and summary.")},
        }),
        .handler = storyboard_query,
    });

    json::Value story_version;
    story_version["description"] = "The version storyboard.query returned, or null to create "
                                   "the entry. A stale version is refused.";
    story_version["type"] = json::Array{"string", "null"};
    json::Value story_edit = params_schema(
        {
            {"id", property("string", "Entry ID: a kind and a lowercase name, e.g. "
                                      "\"character/mira\".")},
            {"version", std::move(story_version)},
            {"set", property("object", "Fields to set; null removes a field.")},
            {"remove", property("boolean", "Remove the entry.")},
        },
        {"id", "version"});
    json::Value story_edits = property("array", "Edits, applied together or not at all.");
    story_edits["items"] = std::move(story_edit);
    story_edits["minItems"] = 1;
    story_edits["maxItems"] = 1000;
    registry.add({
        .name = "storyboard.edit",
        .description = "Creates, changes or removes storyboard entries in one undoable step, and "
                       "records it in the journal. Each edit names the version it read.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"edits", std::move(story_edits)},
                {"why", property("string", "Why, for the journal.")},
                {"by", property("string", "Who, for the journal; defaults to the MCP client's "
                                          "name.")},
            },
            {"edits"}),
        .handler = storyboard_edit,
    });

    registry.add({
        .name = "component.set",
        .description = "Sets component fields on an entity, adding components it lacks. "
                       "Returns only the fields whose values changed.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"id", id_property("Entity ID.")},
                {"components", std::move(set_components)},
            },
            {"id", "components"}),
        .handler = component_set,
    });

    json::Value remove_names = names_property("Component types to remove.");
    remove_names["minItems"] = 1;
    registry.add({
        .name = "component.remove",
        .description = "Removes components from an entity. Returns the ones it had.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"id", id_property("Entity ID.")},
                {"components", std::move(remove_names)},
            },
            {"id", "components"}),
        .handler = component_remove,
    });

    registry.add({
        .name = "world.query",
        .description = "Finds entities by the components they have and their field values, "
                       "e.g. every Transform2D with position.y > 100. Returns IDs unless "
                       "include asks for components, plus the total count.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"with", names_property("Only entities with all of these components.")},
            {"without", names_property("Only entities with none of these components.")},
            {"where", where_schema()},
            {"include", include_schema()},
            {"omit_defaults", omit_defaults},
            {"limit", integer_property("Most entities to return; count still covers all.", 1, 10000,
                                       kDefaultQueryLimit)},
        }),
        .handler = world_query,
    });

    json::Value tick_seconds = property("number", "Seconds per tick.");
    tick_seconds["exclusiveMinimum"] = 0;
    tick_seconds["maximum"] = 1;
    tick_seconds["default"] = kDefaultTickSeconds;
    registry.add({
        .name = "step",
        .description = "Advances the simulation by running every system once per tick. Undo "
                       "reverts the entities to how they were before.",
        .capabilities = {Run},
        .effect = Effect::Edit,
        .params = params_schema({
            {"ticks", integer_property("Ticks to run.", 1, 100000, 1)},
            {"tick_seconds", std::move(tick_seconds)},
        }),
        .handler = step,
    });

    registry.add({
        .name = "undo",
        .description = "Reverts the last change to the scene.",
        .capabilities = {Edit},
        .effect = Effect::History,
        .params = params_schema(),
        .handler = [](Session& session, const json::Value& /*params*/) { return session.undo(); },
    });

    registry.add({
        .name = "redo",
        .description = "Reapplies the last change that undo reverted.",
        .capabilities = {Edit},
        .effect = Effect::History,
        .params = params_schema(),
        .handler = [](Session& session, const json::Value& /*params*/) { return session.redo(); },
    });

    registry.add({
        .name = "history.get",
        .description = "Lists the changes undo and redo would revert or reapply, by command "
                       "name. The last undo entry and the first redo entry are next.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = history_get,
    });

    registry.add({
        .name = "world.snapshot",
        .description = "Saves a copy of every entity under a name, to compare against with "
                       "world.diff or go back to with world.restore. A name in use is replaced.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"name", id_property("Snapshot name; \"default\" if left out.")}}),
        .handler = world_snapshot,
    });

    registry.add({
        .name = "world.diff",
        .description = "Lists the entities added, removed or changed since a snapshot, by ID, with "
                       "the new values of changed fields. A removed component is null.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"from", id_property("Snapshot to compare from; \"default\" if left out.")},
            {"to", id_property("Snapshot to compare to. The world as it is now if left out.")},
        }),
        .handler = world_diff,
    });

    registry.add({
        .name = "world.restore",
        .description = "Puts every entity back as it was in a snapshot. Undo reverts it.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema({{"name", id_property("Snapshot name; \"default\" if left out.")}}),
        .handler = world_restore,
    });

    registry.add({
        .name = "batch",
        .description = "Runs several commands in one call. If any fails, none of their changes "
                       "to the scene are kept and the error names the failing command. One "
                       "undo reverts the whole batch.",
        .capabilities = {},
        .effect = Effect::Compound,
        .params = batch_schema(),
        .handler = batch,
    });

    registry.add({
        .name = "events.subscribe",
        .description = "Starts queueing world events for events.read: entities created or "
                       "destroyed, each change to the world with what changed, and events the "
                       "game emits.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"kinds", names_property("Only these kinds: \"entity.created\", "
                                     "\"entity.destroyed\", \"command\" (a change to the "
                                     "world) or the game's own. All if left out.")},
            {"ids", names_property("Only events about these entity IDs.")},
        }),
        .handler = events_subscribe,
    });

    registry.add({
        .name = "events.read",
        .description = "Returns and removes a subscription's queued events, oldest first.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"subscription", property("string", "Name from events.subscribe.")},
                {"limit", integer_property("Most events to return.", 1, 1000, kDefaultEventLimit)},
            },
            {"subscription"}),
        .handler = events_read,
    });

    registry.add({
        .name = "events.unsubscribe",
        .description = "Stops a subscription and drops its queued events.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params =
            params_schema({{"subscription", property("string", "Name from events.subscribe.")}},
                          {"subscription"}),
        .handler = events_unsubscribe,
    });

    json::Value level = property("string", "Only lines at this level or above.");
    level["enum"] = json::Array{"debug", "info", "warn", "error"};
    registry.add({
        .name = "log.tail",
        .description = "Returns the most recent engine log lines as JSON objects, oldest first.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"lines", integer_property("How many lines.", 1, 1000, kDefaultLogLines)},
            {"level", std::move(level)},
        }),
        .handler = log_tail,
    });
}

void register_run_commands(Registry& registry) {
    using enum Capability;
    registry.add({
        .name = "play",
        .description = "Starts the game ticking in real time, running every system each tick.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = [](Session& session,
                      const json::Value& /*params*/) { return run_state(session, true); },
    });
    registry.add({
        .name = "pause",
        .description = "Stops the game ticking, so the world holds still while you inspect or "
                       "edit it. step still advances it.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = [](Session& session,
                      const json::Value& /*params*/) { return run_state(session, false); },
    });
}

Reply open_or_new_scene(Session& session, std::string_view path) {
    json::Value params;
    params["path"] = path;
    const auto file = session.resolve_path(path);
    const bool found = file && platform::exists(file.value());
    return session.call(found ? "scene.open" : "scene.new", params);
}

} // namespace slash::command
