#include "slash/command/builtin.hpp"

#include "selection.hpp"

#include "slash/animation/animation.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/light.hpp"
#include "slash/canvas/material.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/command/replay.hpp"
#include "slash/command/session.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/core/version.hpp"
#include "slash/input/input.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/render/light.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
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

using detail::Condition;
using detail::Selection;
using world::ComponentType;

// run.until gives up after this many ticks unless told otherwise: ten seconds at 60 ticks a second.
constexpr std::int64_t kDefaultUntilTicks = 600;
constexpr std::int64_t kDefaultLogLines = 50;
constexpr double kDefaultTickSeconds = 1.0 / 60.0;
constexpr std::string_view kDefaultSnapshot = "default";
constexpr std::int64_t kDefaultEventLimit = 100;
// Light masks have 20 bits, and z indexes run from -4096 to 4096, as Godot's.
constexpr std::int64_t kMaxLightMask = (std::int64_t{1} << 20) - 1;
constexpr std::int64_t kMinZIndex = -4096;
constexpr std::int64_t kMaxZIndex = 4096;

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
    if (kind == "frames") {
        return Reply::success(canvas::sprite_frames_schema());
    }
    if (kind == "material") {
        return Reply::success(canvas::material_schema());
    }
    if (kind == "animation") {
        return Reply::success(animation::animation_schema());
    }
    if (kind == "tileset") {
        return Reply::success(canvas::tileset_schema());
    }
    if (kind == "font") {
        return Reply::success(text::font_schema());
    }
    if (kind == "theme") {
        return Reply::success(ui::theme_schema());
    }
    if (kind == "audio") {
        return Reply::success(audio::stream_file_schema());
    }
    if (kind == "bus_layout") {
        return Reply::success(audio::bus_layout_schema());
    }
    if (kind == "replay") {
        return Reply::success(replay_schema());
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

// ---- Materials ----

template <std::size_t N>
std::array<float, N> floats(const json::Value& array) {
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out.at(i) = static_cast<float>(array.as_array()[i].as_double());
    }
    return out;
}

Reply material_eval(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    std::string path = text(params, "path");
    if (id.empty() == path.empty()) {
        return param_error(code::kInvalidParams, "",
                           "give id (an entity with a Sprite2D) or path (a .material.json file)");
    }
    std::string texture;
    std::map<std::string, json::Value> overrides;
    canvas::PixelInputs inputs;
    render::Filter filter = render::Filter::Linear;
    if (!id.empty()) {
        const world::World& world = session.world();
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            return unknown_entity(world, id, "/id");
        }
        const auto* sprite = world.ecs().try_get<world::Sprite2D>(entity);
        if (sprite == nullptr) {
            return param_error(code::kInvalidParams, "/id", quoted(id) + " has no Sprite2D");
        }
        path = sprite->material;
        texture = sprite->texture;
        overrides = sprite->material_params;
        inputs.color = {sprite->modulate.x, sprite->modulate.y, sprite->modulate.z,
                        sprite->modulate.w};
        if (sprite->texture_filter == "nearest") {
            filter = render::Filter::Nearest;
        }
    }
    if (const json::Value* value = params.find("texture"); value != nullptr) {
        texture = value->as_string();
    }
    if (const json::Value* value = params.find("params"); value != nullptr) {
        for (const auto& [name, param] : value->as_object()) {
            overrides.insert_or_assign(name, param);
        }
    }
    if (const json::Value* value = params.find("color"); value != nullptr) {
        inputs.color = floats<4>(*value);
    }
    if (const json::Value* value = params.find("frag_coord"); value != nullptr) {
        inputs.frag_coord = floats<2>(*value);
    }
    const json::Value* time = params.find("time");

    const canvas::CanvasMaterial plain; // no graph: the texture times the color
    asset::Handle<canvas::CanvasMaterial> loaded;
    if (!path.empty()) {
        auto handle = session.assets().load<canvas::CanvasMaterial>(path);
        if (!handle) {
            return param_error(handle.error(), id.empty() ? "/path" : "/id");
        }
        loaded = std::move(handle).value();
    }
    std::string failed;
    auto instance = canvas::instantiate(loaded ? *loaded : plain, overrides,
                                        time != nullptr ? time->as_double() : 0.0, &failed);
    if (!instance) {
        // A param given here, or one of the entity's material_params.
        const json::Value* given = params.find("params");
        if (given != nullptr && given->find(failed) != nullptr) {
            return param_error(instance.error(), "/params/" + json::pointer_escape(failed));
        }
        return param_error(make_error(instance.error().code, "material_params/" + failed + ": " +
                                                                 instance.error().message),
                           "/id");
    }
    std::vector<std::array<float, 2>> uvs;
    for (const json::Value& uv : params.find("uv")->as_array()) {
        uvs.push_back(floats<2>(uv));
    }
    std::vector<asset::Handle<render::Image>> images;
    auto colors = canvas::evaluate(
        instance.value(), texture, filter, inputs, uvs,
        [&](const std::string& image_path) -> Result<const render::Image*> {
            auto image = session.assets().load<render::Image>(image_path);
            if (!image) {
                return make_error(image.error().code, image_path + ": " + image.error().message);
            }
            images.push_back(std::move(image).value());
            return images.back().get();
        });
    if (!colors) {
        return Reply::failure(colors.error());
    }
    json::Array out;
    for (const std::array<float, 4>& color : colors.value()) {
        json::Array channels;
        for (const float channel : color) {
            // Three decimals are finer than 8-bit color.
            channels.emplace_back(std::round(static_cast<double>(channel) * 1000.0) / 1000.0);
        }
        out.emplace_back(std::move(channels));
    }
    json::Value result;
    result["colors"] = std::move(out);
    return Reply::success(std::move(result));
}

// ---- Animations ----

// A number as JSON, with no ".0" on whole ones.
json::Value short_number(double value) {
    constexpr double kWhole = 1.0e15;
    if (value == std::floor(value) && std::abs(value) < kWhole) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

// Numbers rounded to four decimals, finer than a pixel or a degree, so replies stay short.
json::Value rounded(const json::Value& value) {
    constexpr double kScale = 10000.0;
    if (value.is_number() && !value.is_int()) {
        return short_number(std::round(value.as_double() * kScale) / kScale);
    }
    if (value.is_array()) {
        json::Array out;
        out.reserve(value.size());
        for (const json::Value& item : value.as_array()) {
            out.push_back(rounded(item));
        }
        return out;
    }
    if (value.is_object()) {
        json::Value out = json::Object{};
        for (const auto& [name, item] : value.as_object()) {
            out[name] = rounded(item);
        }
        return out;
    }
    return value;
}

Reply animation_eval(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    std::string path = text(params, "path");
    std::string name = text(params, "animation");
    if (id.empty() == path.empty()) {
        return param_error(code::kInvalidParams, "",
                           "give id (an entity with an AnimationPlayer) or path (a .anim.json "
                           "file)");
    }
    if (!id.empty()) {
        const world::World& world = session.world();
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            return unknown_entity(world, id, "/id");
        }
        const auto* player = world.ecs().try_get<world::AnimationPlayer>(entity);
        if (player == nullptr || player->library.empty()) {
            return param_error(code::kInvalidParams, "/id",
                               quoted(id) + (player == nullptr
                                                 ? " has no AnimationPlayer"
                                                 : "'s AnimationPlayer has no library"));
        }
        path = player->library;
        if (name.empty()) {
            name = player->animation;
        }
    }
    auto library = session.assets().load<animation::AnimationLibrary>(path);
    if (!library) {
        return param_error(library.error(), id.empty() ? "/path" : "/id");
    }
    const auto& animations = library.value()->animations;
    const auto found = animations.find(name);
    if (found == animations.end()) {
        std::vector<std::string_view> names;
        names.reserve(animations.size());
        for (const auto& [each, animation] : animations) {
            names.emplace_back(each);
        }
        if (name.empty()) {
            std::string message = "name one of " + path + "'s animations:";
            for (const std::string_view each : names) {
                message += (each == names.front() ? " " : ", ") + quoted(each);
            }
            return param_error(code::kInvalidParams, "/animation", message);
        }
        std::string message = path + " has no animation " + quoted(name);
        add_hint(message, name, names);
        return param_error(code::kNotFound, "/animation", message);
    }
    const animation::Animation& animation = found->second;

    std::vector<double> times;
    const json::Array& given_times = params.find("times")->as_array();
    for (std::size_t i = 0; i < given_times.size(); ++i) {
        const double time = given_times[i].as_double();
        if (time > animation.length) {
            return param_error(
                code::kInvalidParams, "/times/" + std::to_string(i),
                json::write(short_number(time), json::Style::Compact) +
                    " is past the animation's length, " +
                    json::write(short_number(animation.length), json::Style::Compact));
        }
        times.push_back(time);
    }
    std::vector<std::string_view> paths;
    for (const animation::Track& track : animation.tracks) {
        if (!track.path.empty()) {
            paths.emplace_back(track.path);
        }
    }
    std::set<std::string, std::less<>> wanted;
    if (const json::Value* only = params.find("tracks"); only != nullptr) {
        const json::Array& items = only->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string& track = items[i].as_string();
            if (std::ranges::find(paths, track) == paths.end()) {
                std::string message = name + " has no value track " + quoted(track);
                add_hint(message, track, paths);
                return param_error(code::kNotFound, "/tracks/" + std::to_string(i), message);
            }
            wanted.insert(track);
        }
    }

    json::Value tracks = json::Object{};
    for (const animation::Track& track : animation.tracks) {
        if (track.path.empty() || (!wanted.empty() && !wanted.contains(track.path))) {
            continue;
        }
        auto target = animation::resolve_target(track.path, session.components());
        if (!target) {
            return Reply::failure(code::kInvalidArgument, path + ": " + track.path + ": " +
                                                              target.error().message +
                                                              "; fix the file, then retry");
        }
        const bool discrete =
            track.update == animation::UpdateMode::Discrete ||
            (track.update == animation::UpdateMode::Auto && target.value().discrete);
        json::Array values;
        values.reserve(times.size());
        for (const double time : times) {
            values.push_back(rounded(animation::sample(animation, track, time, discrete)));
        }
        tracks[track.path] = std::move(values);
    }
    json::Value result;
    result["length"] = short_number(animation.length);
    result["loop"] = animation::loop_mode_name(animation.loop);
    result["tracks"] = std::move(tracks);
    return Reply::success(std::move(result));
}

// ---- Tile maps ----

// The most cells one tilemap command takes.
constexpr std::int64_t kMaxTileCells = 65536;

// An entity's TileMapLayer, the tile set it draws from and its cells.
struct LayerTarget {
    entt::entity entity = entt::null;
    asset::Handle<canvas::TileSet> set;
    canvas::Cells cells;
};

std::optional<Reply> find_layer(Session& session, const std::string& id, LayerTarget& out) {
    world::World& world = session.world();
    out.entity = world.find(id);
    if (out.entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    const auto* layer = world.ecs().try_get<world::TileMapLayer>(out.entity);
    if (layer == nullptr || layer->tile_set.empty()) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + (layer == nullptr ? " has no TileMapLayer"
                                                          : "'s TileMapLayer has no tile_set"));
    }
    auto set = session.assets().load<canvas::TileSet>(layer->tile_set);
    if (!set) {
        return param_error(set.error(), "/id");
    }
    auto cells = canvas::decode_cells(*layer, *set.value());
    if (!cells) {
        return param_error(code::kInvalidArgument, "/id",
                           quoted(id) + "'s TileMapLayer" + cells.error().message);
    }
    out.set = std::move(set).value();
    out.cells = std::move(cells).value();
    return std::nullopt;
}

// The cells a tilemap command names, as "cells" or "rect".
std::optional<Reply> target_cells(const json::Value& params, std::vector<Vec2i>& out) {
    const json::Value* cells = params.find("cells");
    const json::Value* rect = params.find("rect");
    if ((cells == nullptr) == (rect == nullptr)) {
        return param_error(code::kInvalidParams, "",
                           "give cells, a list of [column, row], or rect, [column, row, width, "
                           "height]");
    }
    if (cells != nullptr) {
        for (const json::Value& cell : cells->as_array()) {
            out.push_back({static_cast<int>(cell.as_array()[0].as_int()),
                           static_cast<int>(cell.as_array()[1].as_int())});
        }
        return std::nullopt;
    }
    const json::Array& box = rect->as_array();
    const std::int64_t x = box[0].as_int();
    const std::int64_t y = box[1].as_int();
    const std::int64_t width = box[2].as_int();
    const std::int64_t height = box[3].as_int();
    if (width * height > kMaxTileCells) {
        return param_error(code::kInvalidParams, "/rect",
                           "covers " + std::to_string(width * height) + " cells; the most is " +
                               std::to_string(kMaxTileCells));
    }
    if (x + width - 1 > canvas::kMaxCell || y + height - 1 > canvas::kMaxCell) {
        return param_error(code::kInvalidParams, "/rect",
                           "reaches past 32767, the last column or row cells can be in");
    }
    for (std::int64_t row = y; row < y + height; ++row) {
        for (std::int64_t column = x; column < x + width; ++column) {
            out.push_back({static_cast<int>(column), static_cast<int>(row)});
        }
    }
    return std::nullopt;
}

// Writes cells back into the entity's layer after a change.
void store_cells(Session& session, const LayerTarget& target) {
    auto& layer = session.world().ecs().get<world::TileMapLayer>(target.entity);
    canvas::encode_cells(target.cells, *target.set, layer);
}

json::Value changed_json(const std::string& id, std::size_t changed) {
    json::Value result;
    result["id"] = id;
    result["changed"] = changed;
    return result;
}

Reply tilemap_set(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    LayerTarget target;
    if (auto failed = find_layer(session, id, target)) {
        return std::move(*failed);
    }
    std::vector<Vec2i> cells;
    if (auto failed = target_cells(params, cells)) {
        return std::move(*failed);
    }
    const json::Value& tile = *params.find("tile");
    std::optional<canvas::TileRef> ref;
    if (!tile.is_null()) {
        auto found = canvas::find_tile(*target.set, tile.as_string());
        if (!found) {
            return param_error(found.error(), "/tile");
        }
        ref = std::move(found).value();
    }
    std::size_t changed = 0;
    for (const Vec2i cell : cells) {
        if (!ref) {
            changed += target.cells.erase(cell);
            continue;
        }
        const auto it = target.cells.find(cell);
        if (it == target.cells.end() || !(it->second == *ref)) {
            target.cells.insert_or_assign(cell, *ref);
            ++changed;
        }
    }
    if (changed > 0) {
        store_cells(session, target);
    }
    return Reply::success(changed_json(id, changed));
}

Reply tilemap_paint(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    LayerTarget target;
    if (auto failed = find_layer(session, id, target)) {
        return std::move(*failed);
    }
    std::vector<Vec2i> cells;
    if (auto failed = target_cells(params, cells)) {
        return std::move(*failed);
    }
    const canvas::TileSet& set = *target.set;
    const json::Value& name = *params.find("terrain");
    int terrain = -1;
    if (!name.is_null()) {
        terrain = set.terrain(name.as_string());
        if (terrain < 0) {
            if (set.terrains.empty()) {
                return param_error(code::kNotFound, "/terrain",
                                   "the tile set has no terrains; its terrain_sets name them");
            }
            std::vector<std::string_view> names;
            names.reserve(set.terrains.size());
            for (const canvas::Terrain& each : set.terrains) {
                names.emplace_back(each.name);
            }
            std::string message = "the tile set has no terrain " + quoted(name.as_string());
            add_hint(message, name.as_string(), names);
            return param_error(code::kNotFound, "/terrain", message);
        }
    }
    auto changed = canvas::paint_terrain(target.cells, set, cells, terrain);
    if (!changed) {
        return param_error(changed.error(), "/terrain");
    }
    if (changed.value() > 0) {
        store_cells(session, target);
    }
    return Reply::success(changed_json(id, changed.value()));
}

Reply tilemap_get(Session& session, const json::Value& params) {
    const std::string id = text(params, "id");
    LayerTarget target;
    if (auto failed = find_layer(session, id, target)) {
        return std::move(*failed);
    }
    const auto& layer = session.world().ecs().get<world::TileMapLayer>(target.entity);
    json::Value result;
    const json::Value* rect = params.find("rect");
    if (rect == nullptr) {
        result["origin"] = json::Array{layer.origin.x, layer.origin.y};
        result["legend"] = json::Object{};
        for (const auto& [key, tile] : layer.legend) {
            result["legend"][key] = tile;
        }
        json::Array rows;
        for (const std::string& row : layer.rows) {
            rows.emplace_back(row);
        }
        result["rows"] = std::move(rows);
        return Reply::success(std::move(result));
    }
    std::vector<Vec2i> cells;
    if (auto failed = target_cells(params, cells)) {
        return std::move(*failed);
    }
    // The layer's own keys, the first for each tile.
    const canvas::TileSet& set = *target.set;
    std::map<std::string, std::string, std::less<>> key_of;
    std::size_t length = 1;
    for (const auto& [key, tile] : layer.legend) {
        length = key.size();
        if (auto found = canvas::find_tile(set, tile); found) {
            key_of.try_emplace(canvas::tile_text(set, found.value()), key);
        }
    }
    const json::Array& box = rect->as_array();
    const auto x = static_cast<int>(box[0].as_int());
    const auto y = static_cast<int>(box[1].as_int());
    const auto width = static_cast<int>(box[2].as_int());
    const auto height = static_cast<int>(box[3].as_int());
    json::Value legend = json::Object{};
    json::Array rows;
    for (int row = y; row < y + height; ++row) {
        std::string line;
        for (int column = x; column < x + width; ++column) {
            const auto it = target.cells.find(Vec2i{column, row});
            if (it == target.cells.end()) {
                line.append(length, '.');
                continue;
            }
            const std::string text = canvas::tile_text(set, it->second);
            const std::string& key = key_of.at(text);
            legend[key] = text;
            line += key;
        }
        line.erase(line.find_last_not_of('.') + 1);
        rows.emplace_back(std::move(line));
    }
    result["origin"] = json::Array{x, y};
    result["legend"] = std::move(legend);
    result["rows"] = std::move(rows);
    return Reply::success(std::move(result));
}

// ---- Physics ----

json::Value point_value(Vec2 point) {
    return rounded(json::Array{point.x, point.y});
}

json::Value id_array(const world::World& world, const std::vector<entt::entity>& entities) {
    json::Array ids;
    for (const entt::entity entity : entities) {
        ids.emplace_back(world.id_of(entity));
    }
    return ids;
}

// The filter a physics query's params describe, or the error for an unknown excluded ID.
std::optional<Reply> query_filter(const world::World& world, const json::Value& params,
                                  physics2d::QueryFilter& filter) {
    if (const json::Value* mask = params.find("collision_mask"); mask != nullptr) {
        filter.collision_mask = static_cast<std::uint32_t>(mask->as_int());
    }
    if (const json::Value* bodies = params.find("collide_with_bodies"); bodies != nullptr) {
        filter.collide_with_bodies = bodies->as_bool();
    }
    if (const json::Value* areas = params.find("collide_with_areas"); areas != nullptr) {
        filter.collide_with_areas = areas->as_bool();
    }
    if (const json::Value* exclude = params.find("exclude"); exclude != nullptr) {
        for (std::size_t i = 0; i < exclude->size(); ++i) {
            const std::string& id = exclude->as_array()[i].as_string();
            const entt::entity entity = world.find(id);
            if (entity == entt::null) {
                return unknown_entity(world, id, item_path("/exclude", i));
            }
            filter.exclude.push_back(entity);
        }
    }
    return std::nullopt;
}

Reply physics_raycast(Session& session, const json::Value& params) {
    world::World& world = session.world();
    physics2d::QueryFilter filter;
    if (auto failed = query_filter(world, params, filter)) {
        return std::move(*failed);
    }
    const auto from = floats<2>(*params.find("from"));
    const auto to = floats<2>(*params.find("to"));
    const auto hit = physics2d::raycast(world, {from[0], from[1]}, {to[0], to[1]}, filter);
    json::Value result;
    if (!hit) {
        result["hit"] = nullptr;
        return Reply::success(std::move(result));
    }
    json::Value& found = result["hit"];
    found["id"] = world.id_of(hit->entity);
    found["point"] = point_value(hit->point);
    found["normal"] = point_value(hit->normal);
    found["fraction"] = rounded(hit->fraction);
    return Reply::success(std::move(result));
}

Reply physics_overlap(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value* point = params.find("point");
    const json::Value* shape_fields = params.find("shape");
    if ((point == nullptr) == (shape_fields == nullptr)) {
        return param_error(code::kInvalidParams, "",
                           "give point, or shape (a CollisionShape2D) with position and rotation");
    }
    physics2d::QueryFilter filter;
    if (auto failed = query_filter(world, params, filter)) {
        return std::move(*failed);
    }
    const auto max_results = static_cast<std::size_t>(integer(params, "max_results", 32));
    json::Value result;
    if (point != nullptr) {
        const auto at = floats<2>(*point);
        result["ids"] =
            id_array(world, physics2d::overlap_point(world, {at[0], at[1]}, filter, max_results));
        return Reply::success(std::move(result));
    }
    world::CollisionShape2D shape;
    std::string path = "/shape";
    if (auto read = world::from_json(world, *shape_fields, shape, path); !read) {
        return param_error(code::kInvalidParams, path, read.error().message);
    }
    constexpr std::array<std::string_view, 5> kShapes{"rectangle", "circle", "capsule", "segment",
                                                      "polygon"};
    if (std::ranges::find(kShapes, shape.shape) == kShapes.end()) {
        std::string message = "unknown shape " + quoted(shape.shape);
        add_hint(message, shape.shape, kShapes);
        return param_error(code::kInvalidParams, "/shape/shape", message);
    }
    std::array<float, 2> position{};
    if (const json::Value* at = params.find("position"); at != nullptr) {
        position = floats<2>(*at);
    }
    const json::Value* rotation = params.find("rotation");
    result["ids"] =
        id_array(world, physics2d::overlap_shape(
                            world, shape, {position[0], position[1]},
                            rotation != nullptr ? static_cast<float>(rotation->as_double()) : 0.0F,
                            filter, max_results));
    return Reply::success(std::move(result));
}

Reply physics_contacts(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    const physics2d::Contacts contacts = physics2d::contacts(world, entity);
    json::Value result;
    result["bodies"] = id_array(world, contacts.bodies);
    result["areas"] = id_array(world, contacts.areas);
    return Reply::success(std::move(result));
}

// ---- Particles ----

Reply particles_eval(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    const auto* emitter = world.ecs().try_get<world::CPUParticles2D>(entity);
    if (emitter == nullptr) {
        return param_error(code::kInvalidParams, "/id", quoted(id) + " has no CPUParticles2D");
    }
    world::Transform2D transform;
    if (const auto* current = world.ecs().try_get<world::Transform2D>(entity); current != nullptr) {
        transform = *current;
    }
    std::vector<canvas::Particle> live;
    if (const json::Value* time = params.find("time"); time != nullptr) {
        const json::Value* seconds = params.find("tick_seconds");
        live = canvas::simulate(*emitter, transform, canvas::particle_seed(id, emitter->seed),
                                time->as_double(),
                                seconds != nullptr ? seconds->as_double() : kDefaultTickSeconds);
    } else {
        live = canvas::particles(world, entity);
    }
    const auto place = [&](Vec2 point) {
        return canvas::world_position(*emitter, transform, point);
    };
    json::Value result;
    result["alive"] = static_cast<std::int64_t>(live.size());
    if (live.empty()) {
        result["bounds"] = nullptr;
    } else {
        Vec2 low = place(live.front().position);
        Vec2 high = low;
        for (const canvas::Particle& particle : live) {
            const Vec2 at = place(particle.position);
            low = {std::min(low.x, at.x), std::min(low.y, at.y)};
            high = {std::max(high.x, at.x), std::max(high.y, at.y)};
        }
        result["bounds"] = rounded(json::Array{low.x, low.y, high.x - low.x, high.y - low.y});
    }
    if (flag(params, "list")) {
        const auto max_results = static_cast<std::size_t>(integer(params, "max_results", 32));
        json::Array items;
        for (std::size_t i = 0; i < live.size() && i < max_results; ++i) {
            const canvas::Particle& particle = live[i];
            json::Value item;
            item["position"] = point_value(place(particle.position));
            item["rotation"] =
                rounded(particle.rotation + (emitter->local_coords ? transform.rotation : 0.0F));
            item["scale"] = rounded(particle.scale);
            item["color"] = rounded(json::Array{particle.color.x, particle.color.y,
                                                particle.color.z, particle.color.w});
            item["age"] = rounded(particle.age);
            items.push_back(std::move(item));
        }
        result["particles"] = std::move(items);
    }
    return Reply::success(std::move(result));
}

// ---- UI and text ----

// The lines of laid-out text, without trailing spaces, those cut ending with the ellipsis.
json::Array line_texts(const text::TextLayout& layout, std::string_view source, std::size_t first,
                       std::size_t last, const std::vector<std::size_t>& trimmed = {},
                       std::string_view ellipsis = {}) {
    json::Array lines;
    for (std::size_t l = first; l < last && l < layout.lines.size(); ++l) {
        const text::Line& line = layout.lines[l];
        std::string_view shown = source.substr(line.start, line.end - line.start);
        while (!shown.empty() && (shown.back() == ' ' || shown.back() == '\t')) {
            shown.remove_suffix(1);
        }
        std::string item(shown);
        if (std::ranges::find(trimmed, l) != trimmed.end()) {
            item += ellipsis;
        }
        lines.emplace_back(std::move(item));
    }
    return lines;
}

// Which of lines [first, last) go right to left: true when all do, a bool per line when some
// do, null when none do.
json::Value rtl_lines(const text::TextLayout& layout, std::size_t first, std::size_t last) {
    last = std::min(last, layout.lines.size());
    json::Array lines;
    std::size_t count = 0;
    for (std::size_t l = first; l < last; ++l) {
        lines.emplace_back(layout.lines[l].rtl);
        if (layout.lines[l].rtl) {
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return count == lines.size() ? json::Value(true) : json::Value(std::move(lines));
}

// A LineEdit as ui.layout reports it: whether it is being edited, its caret and selection in
// characters, what an input method is composing, whether it shows its placeholder, and the
// characters drawn (of the text with the composition at the caret) when they aren't all of them.
json::Value line_edit_json(const world::World& world, entt::entity entity,
                           const ui::LineEditView& view) {
    const auto& edit = world.ecs().get<world::LineEdit>(entity);
    const auto length = static_cast<std::uint32_t>(to_utf32(edit.text).size());
    json::Value out;
    if (ui::is_editing(world, entity)) {
        out["editing"] = true;
    }
    out["caret_column"] = std::clamp(edit.caret_column, 0, static_cast<int>(length));
    if (const auto selection = ui::selection(world, entity); selection) {
        out["selection"] = json::Array{selection->first, selection->second};
    }
    const std::string ime = ui::ime_text(world, entity);
    if (!ime.empty()) {
        out["ime"] = ime;
    }
    const auto shown = length + static_cast<std::uint32_t>(to_utf32(ime).size());
    if (shown == 0) {
        out["placeholder"] = !edit.placeholder_text.empty();
    } else if (view.visible_start != 0 || view.visible_end != shown) {
        out["visible"] = json::Array{static_cast<std::int64_t>(view.visible_start),
                                     static_cast<std::int64_t>(view.visible_end)};
    }
    if (!view.error.empty()) {
        out["font_error"] = view.error;
    }
    return out;
}

// A TextEdit as ui.layout reports it: its caret and selection as [line, column], what an input
// method is composing, whether it shows its placeholder, the first and last lines in view when
// they aren't all of them, its rows when wrapping makes more of them than lines, and which
// scroll bars show.
json::Value text_edit_json(const world::World& world, entt::entity entity,
                           const ui::TextEditView& view) {
    const auto& edit = world.ecs().get<world::TextEdit>(entity);
    json::Value out;
    out["caret"] = json::Array{view.caret_line, view.caret_column};
    if (view.selection) {
        const std::array<int, 4>& range = *view.selection;
        out["selection"] =
            json::Array{json::Array{range[0], range[1]}, json::Array{range[2], range[3]}};
    }
    if (!view.ime.empty()) {
        out["ime"] = view.ime;
    }
    if (view.placeholder) {
        out["placeholder"] = true;
    }
    const auto lines = static_cast<int>(std::ranges::count(edit.text, '\n')) + 1;
    if (view.first_visible_line != 0 || view.last_visible_line != lines - 1) {
        out["visible_lines"] = json::Array{view.first_visible_line, view.last_visible_line};
    }
    if (view.total_rows != lines) {
        out["rows"] = view.total_rows;
    }
    if (view.v_scroll_visible) {
        out["v_scroll_bar"] = true;
    }
    if (view.h_scroll_visible) {
        out["h_scroll_bar"] = true;
    }
    if (!view.error.empty()) {
        out["font_error"] = view.error;
    }
    return out;
}

Reply ui_layout(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    text::Fonts* fonts = ui::fonts(world);
    SLASH_ASSERT(fonts != nullptr, "sessions install ui");
    Vec2 viewport = ui::settings(world).viewport;
    if (const json::Value* size = params.find("viewport"); size != nullptr) {
        viewport = {static_cast<float>(size->as_array()[0].as_double()),
                    static_cast<float>(size->as_array()[1].as_double())};
    }
    std::set<entt::entity> wanted;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        const json::Array& list = ids->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& id = list[i].as_string();
            const std::string path = "/ids/" + std::to_string(i);
            const entt::entity entity = world.find(id);
            if (entity == entt::null) {
                return unknown_entity(world, id, path);
            }
            if (!ui::is_control(world, entity)) {
                return param_error(code::kInvalidParams, path,
                                   quoted(id) + " isn't a control: it has no Control or UI "
                                                "component");
            }
            wanted.insert(entity);
        }
    }
    const ui::Layout layout = ui::layout(world, *fonts, viewport);
    const entt::entity focused = ui::focus_owner(world);
    const entt::entity hovered = ui::hovered(world);
    json::Array controls;
    for (const ui::Placed& placed : layout.controls) {
        const std::string_view id = world.id_of(placed.entity);
        if (id.empty() || (!wanted.empty() && !wanted.contains(placed.entity))) {
            continue;
        }
        // The rect's bounds on screen, turned and scaled as it draws.
        const std::array<Vec2, 4> corners{
            placed.transform.apply({0.0F, 0.0F}),
            placed.transform.apply({placed.size.x, 0.0F}),
            placed.transform.apply({0.0F, placed.size.y}),
            placed.transform.apply({placed.size.x, placed.size.y}),
        };
        Vec2 low = corners[0];
        Vec2 high = corners[0];
        for (const Vec2 corner : corners) {
            low = {std::min(low.x, corner.x), std::min(low.y, corner.y)};
            high = {std::max(high.x, corner.x), std::max(high.y, corner.y)};
        }
        json::Value item;
        item["id"] = id;
        item["rect"] = rounded(json::Array{low.x, low.y, high.x - low.x, high.y - low.y});
        if (!placed.visible) {
            item["visible"] = false;
        }
        if (const auto button = layout.buttons.find(placed.entity);
            button != layout.buttons.end()) {
            item["draw_mode"] = ui::draw_mode_name(button->second.mode);
        }
        if (placed.entity == focused) {
            item["focused"] = true;
        }
        if (placed.entity == hovered) {
            item["hovered"] = true;
        }
        if (const auto edit = layout.line_edits.find(placed.entity);
            edit != layout.line_edits.end()) {
            item["line_edit"] = line_edit_json(world, placed.entity, edit->second);
        }
        if (const auto edit = layout.text_edits.find(placed.entity);
            edit != layout.text_edits.end()) {
            item["text_edit"] = text_edit_json(world, placed.entity, edit->second);
        }
        if (const ui::LabelText* label = layout.label(placed.entity); label != nullptr) {
            item["lines"] = line_texts(label->text, label->source, label->first, label->last,
                                       label->trimmed, label->ellipsis);
            if (json::Value rtl = rtl_lines(label->text, label->first, label->last);
                !rtl.is_null()) {
                item["rtl"] = std::move(rtl);
            }
            if (label->overflow) {
                item["overflow"] = true;
            }
            if (label->text.missing > 0) {
                item["missing_glyphs"] = static_cast<std::int64_t>(label->text.missing);
            }
            if (label->text.system > 0) {
                item["system_glyphs"] = static_cast<std::int64_t>(label->text.system);
            }
            if (!label->error.empty()) {
                item["font_error"] = label->error;
            }
        } else if (const auto percent = layout.percentages.find(placed.entity);
                   percent != layout.percentages.end()) {
            // A ProgressBar's percentage.
            item["lines"] = json::Array{percent->second.source};
        }
        controls.push_back(std::move(item));
    }
    json::Value result;
    result["viewport"] = point_value(viewport);
    result["controls"] = std::move(controls);
    // Children that can't be laid out, theme overrides with names their type doesn't have and
    // fonts that don't load, for the controls asked about.
    json::Array problems;
    for (const ui::Problem& problem : ui::check(world, *fonts)) {
        if (!wanted.empty() && !wanted.contains(world.find(problem.id))) {
            continue;
        }
        json::Value item;
        item["id"] = problem.id;
        item["path"] = problem.path;
        item["message"] = problem.message;
        problems.push_back(std::move(item));
    }
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

Reply ui_theme(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    text::Fonts* fonts = ui::fonts(world);
    SLASH_ASSERT(fonts != nullptr, "sessions install ui");
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    if (!ui::is_control(world, entity)) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " isn't a control: it has no Control or UI component");
    }
    const std::vector<ui::ThemeItem> items = ui::theme_items(world, *fonts, entity);
    // What it has, as "kind" and "kind/name", for the filter and its suggestions.
    std::vector<std::string> names;
    for (const ui::ThemeItem& item : items) {
        if (std::ranges::find(names, item.kind) == names.end()) {
            names.push_back(item.kind);
        }
        names.push_back(item.kind + "/" + item.name);
    }
    std::set<std::string, std::less<>> wanted;
    if (const json::Value* filter = params.find("items"); filter != nullptr) {
        const json::Array& list = filter->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& name = list[i].as_string();
            if (std::ranges::find(names, name) == names.end()) {
                std::string message = quoted(id) + " uses no theme item " + quoted(name);
                add_hint(message, name, names);
                return param_error(code::kInvalidParams, "/items/" + std::to_string(i), message);
            }
            wanted.insert(name);
        }
    }
    json::Array out;
    for (const ui::ThemeItem& item : items) {
        const std::string name = item.kind + "/" + item.name;
        if (!wanted.empty() && !wanted.contains(item.kind) && !wanted.contains(name)) {
            continue;
        }
        json::Value entry;
        entry["item"] = name;
        entry["value"] = item.value;
        entry["from"] = item.from;
        out.push_back(std::move(entry));
    }
    json::Value result;
    result["items"] = std::move(out);
    return Reply::success(std::move(result));
}

Reply text_measure(Session& session, const json::Value& params) {
    text::Fonts* fonts = ui::fonts(session.world());
    SLASH_ASSERT(fonts != nullptr, "sessions install ui");
    const std::string source = text(params, "text");
    const json::Value* width = params.find("width");
    const text::Autowrap autowrap =
        text::parse_autowrap(text(params, "autowrap_mode")).value_or(text::Autowrap::Off);
    if (autowrap != text::Autowrap::Off && width == nullptr) {
        return param_error(code::kInvalidParams, "/width",
                           "autowrap_mode needs a width to wrap to");
    }
    const std::string language = text(params, "language");
    if (!language.empty() && !text::is_language_code(language)) {
        return param_error(code::kInvalidParams, "/language",
                           quoted(language) +
                               R"( isn't a BCP 47 language tag, such as "ar" or "zh-Hant")");
    }
    auto measured = fonts->layout(
        source,
        {.font = text(params, "font"),
         .font_size = static_cast<int>(integer(params, "font_size", text::kDefaultFontSize)),
         .autowrap = autowrap,
         .width = width != nullptr ? static_cast<float>(width->as_double()) : 0.0F,
         .direction =
             text::parse_direction(text(params, "direction")).value_or(text::Direction::Auto),
         .language = language});
    if (!measured) {
        return param_error(code::kInvalidParams, "/font", measured.error().message);
    }
    const text::TextLayout& layout = measured.value();
    const float line_height = layout.metrics.height();
    json::Value result;
    result["size"] =
        rounded(json::Array{layout.width, line_height * static_cast<float>(layout.lines.size())});
    result["line_height"] = rounded(line_height);
    result["ascent"] = rounded(layout.metrics.ascent);
    result["lines"] = line_texts(layout, source, 0, layout.lines.size());
    if (json::Value rtl = rtl_lines(layout, 0, layout.lines.size()); !rtl.is_null()) {
        result["rtl"] = std::move(rtl);
    }
    if (layout.missing > 0) {
        result["missing_glyphs"] = static_cast<std::int64_t>(layout.missing);
    }
    if (layout.system > 0) {
        result["system_glyphs"] = static_cast<std::int64_t>(layout.system);
    }
    return Reply::success(std::move(result));
}

Reply ui_press(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = text(params, "id");
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    const auto* button = world.ecs().try_get<world::Button>(entity);
    const auto* texture = world.ecs().try_get<world::TextureButton>(entity);
    if (button == nullptr && texture == nullptr) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " isn't a button: it has no Button or TextureButton");
    }
    if ((button != nullptr && button->disabled) || (texture != nullptr && texture->disabled)) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " is disabled, so it can't be pressed");
    }
    ui::press(world, entity);
    json::Value result;
    result["tick"] = world.tick();
    return Reply::success(std::move(result));
}

Reply ui_focus(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value* id = params.find("id");
    if (id->is_null()) {
        ui::release_focus(world);
    } else {
        const entt::entity entity = world.find(id->as_string());
        if (entity == entt::null) {
            return unknown_entity(world, id->as_string(), "/id");
        }
        if (!ui::is_control(world, entity)) {
            return param_error(code::kInvalidParams, "/id",
                               quoted(id->as_string()) +
                                   " isn't a control: it has no Control or UI component");
        }
        if (!ui::grab_focus(world, entity)) {
            return param_error(code::kInvalidParams, "/id",
                               quoted(id->as_string()) +
                                   " can't take focus: it is hidden, or its focus_mode is "
                                   "\"none\" (the default for controls that aren't buttons or "
                                   "LineEdits)");
        }
    }
    json::Value result;
    const entt::entity owner = ui::focus_owner(world);
    result["focused"] = owner != entt::null ? json::Value(world.id_of(owner)) : json::Value();
    return Reply::success(std::move(result));
}

Reply ui_clipboard(Session& session, const json::Value& params) {
    world::World& world = session.world();
    if (const json::Value* text = params.find("text"); text != nullptr) {
        ui::set_clipboard(world, text->as_string());
    }
    json::Value result;
    result["text"] = ui::clipboard(world);
    return Reply::success(std::move(result));
}

// ---- Input ----

const input::Map& input_map(const Session& session) {
    const input::Map* map = input::map(session.world());
    SLASH_ASSERT(map != nullptr, "sessions install input");
    return *map;
}

// An error for an action the map doesn't have, with the closest name.
json::ValidationError unknown_action(const input::Map& map, std::string_view name,
                                     std::string path) {
    std::string message = "no action " + quoted(name);
    add_hint(message, name, map.names());
    return {std::move(path), std::move(message)};
}

Reply input_inject(Session& session, const json::Value& params) {
    const input::Map& map = input_map(session);
    const json::Array& items = params.find("events")->as_array();
    std::vector<json::ValidationError> errors;
    std::vector<input::Event> events;
    events.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string path = item_path("/events", i);
        auto found = input::validate_event(items[i], input::Use::Event, path);
        if (!found.empty()) {
            errors.insert(errors.end(), found.begin(), found.end());
            continue;
        }
        auto parsed = input::parse_events(items[i]);
        if (!parsed) {
            errors.push_back({path, parsed.error().message});
            continue;
        }
        for (input::Event& event : parsed.value()) {
            if (event.type == input::EventType::Action && !map.has_action(event.action)) {
                errors.push_back(unknown_action(map, event.action, path + "/action"));
                continue;
            }
            events.push_back(std::move(event));
        }
    }
    if (!errors.empty()) {
        return Reply::invalid_params(errors);
    }
    world::World& world = session.world();
    for (input::Event& event : events) {
        input::push(world, std::move(event));
    }
    json::Value result;
    result["queued"] = input::queued(world);
    result["tick"] = world.tick();
    return Reply::success(std::move(result));
}

Reply input_state(Session& session, const json::Value& params) {
    const input::Map& map = input_map(session);
    std::vector<std::string> actions;
    if (const json::Value* names = params.find("actions"); names != nullptr) {
        std::vector<json::ValidationError> errors;
        const json::Array& items = names->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!map.has_action(items[i].as_string())) {
                errors.push_back(
                    unknown_action(map, items[i].as_string(), item_path("/actions", i)));
            }
            actions.push_back(items[i].as_string());
        }
        if (!errors.empty()) {
            return Reply::invalid_params(errors);
        }
    }
    return Reply::success(input::state_json(session.world(), actions));
}

Reply input_actions(Session& session, const json::Value& params) {
    const input::Map& map = input_map(session);
    json::Value actions = json::Object{};
    if (const json::Value* names = params.find("names"); names != nullptr) {
        std::vector<json::ValidationError> errors;
        const json::Array& items = names->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const input::Action* action = map.find(items[i].as_string());
            if (action == nullptr) {
                errors.push_back(unknown_action(map, items[i].as_string(), item_path("/names", i)));
                continue;
            }
            actions[items[i].as_string()] = input::action_json(*action);
        }
        if (!errors.empty()) {
            return Reply::invalid_params(errors);
        }
    } else {
        for (const auto& [name, action] : map.actions()) {
            if (!input::is_text_action(name)) {
                actions[name] = input::action_json(action);
            }
        }
    }
    json::Value result;
    result["actions"] = std::move(actions);
    return Reply::success(std::move(result));
}

// ---- Audio ----

constexpr double kMaxCaptureSeconds = 10.0;
constexpr std::int64_t kDefaultCaptureSlices = 10;

audio::Server& audio_server(Session& session) {
    audio::Server* server = audio::server(session.world());
    SLASH_ASSERT(server != nullptr, "sessions install audio");
    return *server;
}

// Decibels to a tenth, which is finer than an ear can tell and the same on every machine.
json::Value decibels(double db) {
    return short_number(std::round(db * 10.0) / 10.0);
}

// A level in dB, or null for silence: Godot's floor of -200 dB, give or take the tiny offset
// it adds to a peak.
json::Value level(float db) {
    constexpr float kSilent = -199.0F;
    return db > kSilent ? decibels(db) : json::Value(nullptr);
}

json::Value bus_json(const audio::Server& server, int index) {
    const audio::BusSettings& bus = server.layout().buses[static_cast<std::size_t>(index)];
    json::Value item;
    item["name"] = bus.name;
    item["volume_db"] = decibels(bus.volume_db);
    const audio::Frame peak = server.bus_peak_db(index);
    item["peak_db"] = level(std::max(peak.left, peak.right));
    if (bus.solo) {
        item["solo"] = true;
    }
    if (bus.mute) {
        item["mute"] = true;
    }
    if (bus.bypass_effects) {
        item["bypass_effects"] = true;
    }
    if (index > 0 && bus.send != "Master") {
        item["send"] = bus.send;
    }
    if (!bus.effects.empty()) {
        json::Array effects;
        for (const audio::EffectSlot& slot : bus.effects) {
            json::Value effect;
            effect["type"] = slot.type;
            if (!slot.enabled) {
                effect["enabled"] = false;
            }
            effects.push_back(std::move(effect));
        }
        item["effects"] = std::move(effects);
    }
    return item;
}

// The index of a bus, or an error naming the closest one.
Result<int> find_bus(const audio::Server& server, std::string_view name) {
    const int index = server.bus_index(name);
    if (index >= 0) {
        return index;
    }
    std::vector<std::string_view> names;
    for (const audio::BusSettings& bus : server.layout().buses) {
        names.emplace_back(bus.name);
    }
    std::string message = "no bus is named " + quoted(name);
    add_hint(message, name, names);
    return make_error(ErrorCode::NotFound, std::move(message));
}

Reply audio_state(Session& session, const json::Value& params) {
    const audio::Server& server = audio_server(session);
    json::Value result;
    result["mix_rate"] = server.mix_rate();
    json::Array buses;
    for (std::size_t i = 0; i < server.layout().buses.size(); ++i) {
        buses.push_back(bus_json(server, static_cast<int>(i)));
    }
    result["buses"] = std::move(buses);
    std::set<std::string, std::less<>> wanted;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        for (const json::Value& id : ids->as_array()) {
            wanted.insert(id.as_string());
        }
    }
    json::Array players;
    for (const audio::PlayerInfo& info : audio::players(session.world())) {
        if (!wanted.empty() && !wanted.contains(info.id)) {
            continue;
        }
        json::Value item;
        item["id"] = info.id;
        item["kind"] = info.kind;
        item["stream"] = info.stream;
        item["bus"] = info.bus;
        item["voices"] = info.voices;
        if (info.voices > 0) {
            item["position"] = rounded(info.position);
        }
        if (info.length > 0.0) {
            item["length"] = rounded(info.length);
        }
        if (info.paused) {
            item["paused"] = true;
        }
        item["volume"] = rounded(json::Array{info.volume.left, info.volume.right});
        if (!info.error.empty()) {
            item["error"] = info.error;
        }
        players.push_back(std::move(item));
    }
    result["players"] = std::move(players);
    return Reply::success(std::move(result));
}

Reply audio_capture(Session& session, const json::Value& params) {
    const std::string bus = params.find("bus") != nullptr ? text(params, "bus") : "Master";
    const json::Value* seconds = params.find("seconds");
    const std::string path = text(params, "path");
    if (!path.empty()) {
        if (!session.allowed(Capability::Filesystem)) {
            json::Value data;
            data["capability"] = "filesystem";
            data["path"] = "/path";
            return Reply::failure(code::kPermissionDenied,
                                  "/path: writing a file needs the \"filesystem\" capability, "
                                  "which this session does not grant",
                                  std::move(data));
        }
        if (!path.ends_with(".wav") || path.size() == 4) {
            return param_error(code::kInvalidArgument, "/path",
                               R"(captures are .wav files, e.g. "build/capture.wav")");
        }
    }
    auto captured = audio::capture(
        session.world(), bus, seconds != nullptr ? seconds->as_double() : 1.0,
        static_cast<int>(integer(params, "slices", kDefaultCaptureSlices)), !path.empty());
    if (!captured) {
        return param_error(captured.error(), "/bus");
    }
    const audio::Capture& capture = captured.value();
    json::Value result;
    result["seconds"] = rounded(capture.seconds);
    result["peak_db"] = level(capture.peak_db);
    result["rms_db"] = level(capture.rms_db);
    json::Array slices;
    for (const float peak : capture.slice_peaks_db) {
        slices.push_back(level(peak));
    }
    result["slices"] = std::move(slices);
    if (!path.empty()) {
        auto file = session.resolve_path(path);
        if (!file) {
            return param_error(file.error(), "/path");
        }
        if (!session.dry_running()) {
            const std::string wav =
                audio::encode_wav(capture.frames, audio_server(session).mix_rate());
            if (auto written = write_file(file.value(), wav); !written) {
                return Reply::failure(written.error());
            }
        }
        result["path"] = path;
    }
    return Reply::success(std::move(result));
}

// The entity a player command names, or why it can't play.
Result<entt::entity> audio_player(const world::World& world, std::string_view id) {
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return make_error(ErrorCode::NotFound, "");
    }
    if (!world.ecs().any_of<world::AudioStreamPlayer, world::AudioStreamPlayer2D>(entity)) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(id) + " has no AudioStreamPlayer or AudioStreamPlayer2D");
    }
    return entity;
}

Reply audio_play_or_stop(Session& session, const json::Value& params, bool play) {
    world::World& world = session.world();
    const std::string id = text(params, "id");
    auto entity = audio_player(world, id);
    if (!entity) {
        return entity.error().code == ErrorCode::NotFound
                   ? unknown_entity(world, id, "/id")
                   : param_error(code::kInvalidParams, "/id", entity.error().message);
    }
    if (play) {
        const json::Value* from = params.find("from");
        audio::play(world, entity.value(), from != nullptr ? from->as_double() : 0.0);
    } else {
        audio::stop(world, entity.value());
    }
    json::Value result;
    result["tick"] = world.tick();
    return Reply::success(std::move(result));
}

Reply audio_set_bus(Session& session, const json::Value& params) {
    audio::Server& server = audio_server(session);
    auto found = find_bus(server, text(params, "bus"));
    if (!found) {
        return param_error(found.error(), "/bus");
    }
    const int bus = found.value();
    const json::Value* effect = params.find("effect");
    const json::Value* enabled = params.find("enabled");
    if (effect != nullptr) {
        const auto count = static_cast<std::int64_t>(
            server.layout().buses[static_cast<std::size_t>(bus)].effects.size());
        if (effect->as_int() >= count) {
            return param_error(code::kInvalidArgument, "/effect",
                               quoted(text(params, "bus")) + " has " + std::to_string(count) +
                                   (count == 1 ? " effect" : " effects"));
        }
        if (enabled == nullptr) {
            return param_error(code::kInvalidParams, "/enabled",
                               R"(missing required property; "effect" needs "enabled")");
        }
    } else if (enabled != nullptr) {
        return param_error(code::kInvalidParams, "/effect",
                           R"(missing required property; "enabled" needs "effect")");
    }
    if (const json::Value* volume = params.find("volume_db"); volume != nullptr) {
        server.set_bus_volume_db(bus, static_cast<float>(volume->as_double()));
    }
    if (const json::Value* mute = params.find("mute"); mute != nullptr) {
        server.set_bus_mute(bus, mute->as_bool());
    }
    if (const json::Value* solo = params.find("solo"); solo != nullptr) {
        server.set_bus_solo(bus, solo->as_bool());
    }
    if (const json::Value* bypass = params.find("bypass_effects"); bypass != nullptr) {
        server.set_bus_bypass_effects(bus, bypass->as_bool());
    }
    if (effect != nullptr) {
        server.set_bus_effect_enabled(bus, static_cast<int>(effect->as_int()), enabled->as_bool());
    }
    return Reply::success(bus_json(server, bus));
}

// ---- Lights ----

// Three decimals are finer than 8-bit color.
json::Value color_value(double channel) {
    constexpr double kScale = 1000.0;
    return short_number(std::round(channel * kScale) / kScale);
}

Reply light_sample(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const std::string id = text(params, "id");
    const json::Value* points = params.find("points");
    if (id.empty() == (points == nullptr)) {
        return param_error(code::kInvalidParams, "",
                           "give points, or id (an entity whose place, light mask and z index "
                           "are used)");
    }
    render::ShadeItem item;
    std::vector<std::array<float, 2>> positions;
    if (points != nullptr) {
        for (const json::Value& point : points->as_array()) {
            positions.push_back(floats<2>(point));
        }
    } else {
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            return unknown_entity(world, id, "/id");
        }
        const auto& ecs = world.ecs();
        const auto* transform = ecs.try_get<world::Transform2D>(entity);
        if (transform == nullptr) {
            return param_error(code::kInvalidParams, "/id", quoted(id) + " has no Transform2D");
        }
        positions.push_back({transform->position.x, transform->position.y});
        if (const auto* sprite = ecs.try_get<world::Sprite2D>(entity); sprite != nullptr) {
            item.light_mask = static_cast<std::uint32_t>(sprite->light_mask);
            item.z_index = sprite->z_index;
        } else if (const auto* layer = ecs.try_get<world::TileMapLayer>(entity); layer != nullptr) {
            item.light_mask = static_cast<std::uint32_t>(layer->light_mask);
            item.z_index = layer->z_index;
        }
    }
    if (const json::Value* mask = params.find("light_mask"); mask != nullptr) {
        item.light_mask = static_cast<std::uint32_t>(mask->as_int());
    }
    if (const json::Value* z_index = params.find("z_index"); z_index != nullptr) {
        item.z_index = static_cast<std::int32_t>(z_index->as_int());
    }

    std::vector<asset::Handle<render::Image>> images;
    const canvas::LightImage load = [&](const std::string& path) -> Result<const render::Image*> {
        auto image = session.assets().load<render::Image>(path);
        if (!image) {
            return image.error();
        }
        images.push_back(std::move(image).value());
        return images.back().get();
    };
    const auto name_of = [&](entt::entity entity) {
        const std::string_view name = world.id_of(entity);
        return name.empty() ? "entity " + std::to_string(entt::to_entity(entity))
                            : std::string(name);
    };
    std::vector<canvas::LightProblem> problems;
    canvas::SceneLighting scene = canvas::collect_lighting(world, load, {}, problems);
    render::Image atlas;
    if (std::ranges::any_of(scene.lights, [](const canvas::SceneLight& light) {
            return light.light.kind == render::LightKind::Point;
        })) {
        auto packed = canvas::pack_light_atlas(scene.lights, load);
        if (!packed) {
            return Reply::failure(packed.error());
        }
        atlas = std::move(packed).value();
    }
    std::vector<render::Light> lights;
    lights.reserve(scene.lights.size());
    for (const canvas::SceneLight& light : scene.lights) {
        lights.push_back(light.light);
    }
    const render::AtlasSampler sample_atlas = [&](float u, float v) {
        return render::sample_image(atlas, render::Filter::Linear, false, u, v);
    };
    const render::ShadowTexel shadow_texel = [&](std::uint32_t column, std::uint32_t row) {
        std::array<std::uint8_t, 4> texel{};
        const std::size_t at = ((std::size_t{row} * render::kShadowMapWidth) + column) * 4;
        std::copy_n(scene.shadow_pixels.data() + at, 4, texel.data());
        return texel;
    };
    if (!scene.lit) {
        item.mode = render::LightMode::Unshaded;
    }

    json::Array out;
    for (const std::array<float, 2>& position : positions) {
        std::vector<render::LightHit> hits;
        render::ShadePixel pixel;
        pixel.position = position;
        const std::array<float, 4> color =
            render::shade(lights, scene.ambient, item, pixel, sample_atlas, shadow_texel, &hits);
        json::Value point;
        point["color"] =
            json::Array{color_value(color[0]), color_value(color[1]), color_value(color[2])};
        json::Array reached;
        for (const render::LightHit& hit : hits) {
            const render::Light& light = lights.at(hit.light);
            json::Array light_color;
            bool dark = true;
            for (std::size_t i = 0; i < 3; ++i) {
                light_color.push_back(color_value(static_cast<double>(hit.color.at(i)) *
                                                  static_cast<double>(hit.color[3])));
                dark = dark && light_color.back().as_double() == 0.0;
            }
            // A light whose texture is clear here only touches the point.
            if (dark && !(hit.shadow > 0.0F)) {
                continue;
            }
            json::Value each;
            each["id"] = name_of(scene.lights.at(hit.light).entity);
            each["color"] = std::move(light_color);
            if (light.shadow_row >= 0) {
                each["shadow"] = color_value(hit.shadow);
            }
            reached.push_back(std::move(each));
        }
        point["lights"] = std::move(reached);
        out.push_back(std::move(point));
    }
    json::Value result;
    result["lit"] = scene.lit;
    result["ambient"] = json::Array{color_value(scene.ambient[0]), color_value(scene.ambient[1]),
                                    color_value(scene.ambient[2])};
    result["points"] = std::move(out);
    if (!problems.empty()) {
        json::Array warnings;
        for (const canvas::LightProblem& problem : problems) {
            json::Value warning;
            warning["id"] = name_of(problem.entity);
            warning["message"] = problem.message;
            warnings.push_back(std::move(warning));
        }
        result["warnings"] = std::move(warnings);
    }
    return Reply::success(std::move(result));
}

// ---- Frames ----

constexpr double kMinCaptureScale = 0.05;
constexpr double kMaxCaptureScale = 4.0;

// Part of an image: x, y, width and height in its pixels, inside it.
render::Image crop(const render::Image& image, std::uint32_t x, std::uint32_t y,
                   std::uint32_t width, std::uint32_t height) {
    render::Image out{.width = width, .height = height, .pixels = {}};
    out.pixels.reserve(std::size_t{width} * height * 4);
    for (std::uint32_t row = y; row < y + height; ++row) {
        const auto begin = image.pixels.begin() +
                           static_cast<std::ptrdiff_t>(((std::size_t{row} * image.width) + x) * 4);
        out.pixels.insert(out.pixels.end(), begin,
                          begin + static_cast<std::ptrdiff_t>(std::size_t{width} * 4));
    }
    return out;
}

Reply frame_capture(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    if (!path.empty()) {
        if (!session.allowed(Capability::Filesystem)) {
            json::Value data;
            data["capability"] = "filesystem";
            data["path"] = "/path";
            return Reply::failure(code::kPermissionDenied,
                                  "/path: writing a file needs the \"filesystem\" capability, "
                                  "which this session does not grant",
                                  std::move(data));
        }
        if (!path.ends_with(".png") || path.size() == 4) {
            return param_error(code::kInvalidArgument, "/path",
                               R"(captures are .png files, e.g. "build/frame.png")");
        }
    }
    const json::Value* scale_param = params.find("scale");
    const double scale = scale_param != nullptr ? scale_param->as_double() : 1.0;
    const json::Value* time = params.find("time");
    world::World& world = session.world();
    const Vec2 viewport = ui::settings(world).viewport;
    const std::string size_text = std::to_string(static_cast<int>(viewport.x)) + "x" +
                                  std::to_string(static_cast<int>(viewport.y));

    // Points and the region are window pixels, checked before drawing.
    std::vector<std::array<float, 2>> points;
    if (const json::Value* given = params.find("points"); given != nullptr) {
        for (std::size_t i = 0; i < given->as_array().size(); ++i) {
            const auto point = floats<2>(given->as_array()[i]);
            if (!(point[0] >= 0.0F) || !(point[1] >= 0.0F) || point[0] >= viewport.x ||
                point[1] >= viewport.y) {
                return param_error(code::kInvalidArgument, "/points/" + std::to_string(i),
                                   "outside the " + size_text + " window");
            }
            points.push_back(point);
        }
    }
    std::optional<std::array<float, 4>> region;
    if (const json::Value* given = params.find("region"); given != nullptr) {
        const auto rect = floats<4>(*given);
        if (!(rect[0] >= 0.0F) || !(rect[1] >= 0.0F) || !(rect[2] > 0.0F) || !(rect[3] > 0.0F) ||
            rect[0] + rect[2] > viewport.x || rect[1] + rect[3] > viewport.y) {
            return param_error(code::kInvalidArgument, "/region",
                               "[x, y, width, height] inside the " + size_text + " window");
        }
        region = rect;
    }

    const json::Value* collisions = params.find("collisions");
    auto captured = canvas::capture(world, session.assets(),
                                    {.scale = static_cast<float>(scale),
                                     .time = time != nullptr ? time->as_double() : -1.0,
                                     .collisions = collisions != nullptr && collisions->as_bool()});
    if (!captured) {
        return param_error(captured.error(), scale_param != nullptr ? "/scale" : "");
    }
    const canvas::Capture& capture = captured.value();
    const render::Image& full = capture.image;
    // A window pixel's place in the image, kept inside it.
    const auto pixel = [&](float at, std::uint32_t size) {
        const auto scaled = static_cast<std::int64_t>(std::floor(static_cast<double>(at) * scale));
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(scaled, 0, size - 1));
    };

    json::Value result;
    render::Image image = full;
    if (region) {
        const std::uint32_t x = pixel((*region)[0], full.width);
        const std::uint32_t y = pixel((*region)[1], full.height);
        const auto right = static_cast<std::uint32_t>(
            std::clamp<double>(std::ceil(static_cast<double>((*region)[0] + (*region)[2]) * scale),
                               x + 1.0, full.width));
        const auto bottom = static_cast<std::uint32_t>(
            std::clamp<double>(std::ceil(static_cast<double>((*region)[1] + (*region)[3]) * scale),
                               y + 1.0, full.height));
        image = crop(full, x, y, right - x, bottom - y);
    }
    result["width"] = image.width;
    result["height"] = image.height;
    json::Value drawn = json::Object{};
    for (const auto& [name, count] :
         {std::pair{"sprites", capture.sprites}, std::pair{"tiles", capture.tiles},
          std::pair{"particles", capture.particles}, std::pair{"lights", capture.lights},
          std::pair{"controls", capture.controls}, std::pair{"shapes", capture.shapes}}) {
        if (count > 0) {
            drawn[name] = count;
        }
    }
    result["drawn"] = std::move(drawn);
    if (!points.empty()) {
        json::Array colors;
        for (const auto& point : points) {
            const std::size_t at = ((std::size_t{pixel(point[1], full.height)} * full.width) +
                                    pixel(point[0], full.width)) *
                                   4;
            json::Array color;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                color.push_back(color_value(full.pixels[at + channel] / 255.0));
            }
            colors.emplace_back(std::move(color));
        }
        result["colors"] = std::move(colors);
    }
    const bool want_image = params.find("image") != nullptr && params.find("image")->as_bool();
    if (!path.empty() || want_image) {
        auto png = render::encode_png(image);
        if (!png) {
            return Reply::failure(png.error());
        }
        if (!path.empty()) {
            auto file = session.resolve_path(path);
            if (!file) {
                return param_error(file.error(), "/path");
            }
            if (!session.dry_running()) {
                if (auto written = write_file(file.value(), png.value()); !written) {
                    return Reply::failure(written.error());
                }
            }
            result["path"] = path;
        }
        if (want_image) {
            result["png"] = base64_encode(png.value());
        }
    }
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

json::Array file_errors(const std::vector<storyboard::FileError>& errors) {
    json::Array items;
    for (const storyboard::FileError& error : errors) {
        json::Value item;
        item["file"] = error.file;
        item["path"] = error.path;
        item["message"] = error.message;
        items.push_back(std::move(item));
    }
    return items;
}

Reply storyboard_check(Session& session, const json::Value& /*params*/) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    const json::Object& entries = loaded.value().entries;
    std::vector<storyboard::FileError> errors = std::move(loaded.value().errors);
    storyboard::CheckResult checked = storyboard::check(entries, [&](std::string_view path) {
        auto file = session.resolve_path(path);
        return file && platform::exists(file.value());
    });
    errors.insert(errors.end(), checked.errors.begin(), checked.errors.end());

    // StoryRef IDs in every scene and prefab in the project, except exports in build/, and in the
    // open scene as it is now rather than as last saved.
    const std::string& open = session.scene_path();
    auto files = platform::list_files(session.options().project_root,
                                      [](std::string_view folder) { return folder != "build/"; });
    if (!files) {
        return Reply::failure(files.error());
    }
    for (const std::string& path : files.value()) {
        if (path == open ||
            (!path.ends_with(world::kSceneSuffix) && !path.ends_with(world::kPrefabSuffix))) {
            continue;
        }
        auto text = session.read_project_file(path);
        const std::optional<std::string> contents = text ? text.value() : std::nullopt;
        if (!contents) {
            continue;
        }
        // A file that does not parse is scene validate's to report.
        if (auto document = json::parse(*contents); document) {
            storyboard::check_story_refs(entries, path, document.value(), errors);
        }
    }
    storyboard::check_story_refs(entries, open.empty() ? "(open scene)" : open,
                                 session.save().scene, errors);

    json::Value result;
    result["valid"] = errors.empty();
    if (!errors.empty()) {
        result["errors"] = file_errors(errors);
    }
    if (!checked.warnings.empty()) {
        result["warnings"] = file_errors(checked.warnings);
    }
    return Reply::success(std::move(result));
}

Reply storyboard_brief(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }
    std::vector<std::string> ids;
    std::vector<std::string> paths;
    std::vector<json::ValidationError> errors;
    const auto each = [&](std::string_view key, const auto& visit) {
        if (const json::Value* list = params.find(key); list != nullptr) {
            for (std::size_t i = 0; i < list->size(); ++i) {
                visit(list->as_array()[i].as_string(),
                      "/" + std::string(key) + "/" + std::to_string(i));
            }
        }
    };
    each("ids", [&](const std::string& id, const std::string& path) {
        if (!entries.contains(id)) {
            const std::string message = storyboard::id_error(id);
            errors.push_back(
                {path, message.empty() ? storyboard::unknown_entry(entries, id) : message});
        }
        ids.push_back(id);
    });
    const auto project_path = [&](const std::string& file, const std::string& path) {
        if (!is_project_path(file)) {
            errors.push_back({path, "paths are relative to the project folder, separated by "
                                    "'/', with no \".\" or \"..\" parts; got " +
                                        quoted(file)});
            return false;
        }
        paths.push_back(file);
        return true;
    };
    each("files", project_path);
    // A scene brings the entries its entities stand for and the prefabs they extend.
    each("scenes", [&](const std::string& file, const std::string& path) {
        if (!project_path(file, path)) {
            return;
        }
        json::Value document;
        if (file == session.scene_path()) {
            document = session.save().scene;
        } else {
            auto text = session.read_project_file(file);
            if (!text || !text.value()) {
                errors.push_back({path, "no scene at " + quoted(file)});
                return;
            }
            auto parsed = json::parse(*text.value());
            if (!parsed) {
                errors.push_back({path, quoted(file) + " is not valid: " + parsed.error().message});
                return;
            }
            document = std::move(parsed).value();
        }
        for (const storyboard::StoryRefId& ref : storyboard::story_refs(document)) {
            ids.push_back(ref.id);
        }
        if (const json::Value* scene_entities = document.find("entities");
            scene_entities != nullptr && scene_entities->is_object()) {
            for (const auto& [id, entity] : scene_entities->as_object()) {
                if (const json::Value* base = entity.find("extends");
                    base != nullptr && base->is_string()) {
                    paths.push_back(base->as_string());
                }
            }
        }
    });
    const world::World& world = session.world();
    const world::PrefabLinks links = world.prefab_links();
    const ComponentType* story = world.components().find("StoryRef");
    each("entities", [&](const std::string& id, const std::string& path) {
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            std::string message = "no entity has the id " + quoted(id);
            add_hint(message, id, world.ids());
            errors.push_back({path, std::move(message)});
            return;
        }
        if (story != nullptr && story->has(world, entity)) {
            const json::Value fields = story->get(world, entity);
            for (const json::Value& ref : fields.find("ids")->as_array()) {
                ids.push_back(ref.as_string());
            }
        }
        if (const auto link = links.find(id); link != links.end()) {
            paths.push_back(link->second.path);
        }
    });
    if (!errors.empty()) {
        Reply reply = Reply::invalid_params(errors);
        reply.code = code::kNotFound;
        return reply;
    }

    auto journal = session.read_project_file(storyboard::kJournalPath);
    if (!journal) {
        return Reply::failure(journal.error());
    }
    const std::string journal_text = journal.value().value_or(std::string());
    storyboard::Brief found = storyboard::brief(entries, ids, paths);
    json::Value result;
    result["entries"] = std::move(found.entries);
    result["related"] = std::move(found.related);
    if (const json::Value* since = params.find("since"); since != nullptr) {
        result["changed"] =
            storyboard::changes_since(journal_text, static_cast<std::size_t>(since->as_int()));
    }
    result["seq"] = storyboard::journal_length(journal_text);
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

} // namespace

namespace detail {

std::optional<Reply> parse_selection(const world::ComponentRegistry& components,
                                     const json::Value& params, Selection& out) {
    if (auto failed = lookup_components(components, params.find("with"), "/with", out.with)) {
        return failed;
    }
    if (auto failed =
            lookup_components(components, params.find("without"), "/without", out.without)) {
        return failed;
    }
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
            out.conditions.push_back(std::move(condition));
        }
    }
    // include: true returns every component, a list returns those, absent returns only IDs.
    const json::Value* include = params.find("include");
    if (include != nullptr && include->is_array()) {
        if (auto failed = lookup_components(components, include, "/include", out.included)) {
            return failed;
        }
        out.with_components = true;
    } else if (include != nullptr && include->is_bool()) {
        out.with_components = include->as_bool();
    }
    out.omit_defaults = flag(params, "omit_defaults");
    out.limit = static_cast<std::size_t>(integer(params, "limit", kDefaultQueryLimit));
    return std::nullopt;
}

bool selects(const world::World& world, entt::entity entity, const Selection& selection) {
    return std::ranges::all_of(selection.with,
                               [&](const auto* type) { return type->has(world, entity); }) &&
           std::ranges::none_of(selection.without,
                                [&](const auto* type) { return type->has(world, entity); }) &&
           std::ranges::all_of(selection.conditions,
                               [&](const Condition& c) { return matches(world, entity, c); });
}

bool any_selected(const world::World& world, const Selection& selection) {
    const auto ids = world.ids();
    return std::ranges::any_of(
        ids, [&](std::string_view id) { return selects(world, world.find(id), selection); });
}

json::Value select(const world::World& world, const Selection& selection) {
    std::size_t count = 0;
    json::Array entities;
    for (const std::string_view id : world.ids()) {
        const entt::entity entity = world.find(id);
        if (!selects(world, entity, selection)) {
            continue;
        }
        ++count;
        if (entities.size() >= selection.limit) {
            continue;
        }
        if (!selection.with_components) {
            entities.emplace_back(id);
            continue;
        }
        json::Value item;
        item["id"] = id;
        item["components"] =
            components_json(world, entity, selection.included, selection.omit_defaults);
        entities.push_back(std::move(item));
    }
    json::Value result;
    result["count"] = count;
    result["truncated"] = count > entities.size();
    result["entities"] = std::move(entities);
    return result;
}

} // namespace detail

namespace {

using detail::parse_selection;
using detail::select;

Reply world_query(Session& session, const json::Value& params) {
    Selection selection;
    if (auto failed = parse_selection(session.world().components(), params, selection)) {
        return std::move(*failed);
    }
    return Reply::success(select(session.world(), selection));
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

// Steps until any entity matches the selection (or, with "until": "none", until none does), or
// the tick limit passes. The condition is checked before each tick, so one that already holds
// runs no ticks.
Reply run_until(Session& session, const json::Value& params) {
    if (params.find("with") == nullptr && params.find("without") == nullptr &&
        params.find("where") == nullptr) {
        return param_error(code::kInvalidParams, "",
                           "give with, without or where: the entities to wait for");
    }
    Selection selection;
    if (auto failed = parse_selection(session.world().components(), params, selection)) {
        return std::move(*failed);
    }
    const bool until_none = text(params, "until") == "none";
    const std::int64_t max_ticks = integer(params, "ticks", kDefaultUntilTicks);
    const json::Value* seconds = params.find("tick_seconds");
    const double tick_seconds = seconds != nullptr ? seconds->as_double() : kDefaultTickSeconds;
    world::World& world = session.world();
    const auto holds = [&] { return detail::any_selected(world, selection) != until_none; };
    std::int64_t ticks = 0;
    bool matched = holds();
    while (!matched && ticks < max_ticks) {
        world.step(tick_seconds);
        ++ticks;
        matched = holds();
    }
    json::Value result = select(world, selection);
    result["matched"] = matched;
    result["ticks"] = ticks;
    result["tick"] = world.tick();
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

} // namespace

json::Object detail::selection_properties() {
    return {
        {"with", names_property("Only entities with all of these components.")},
        {"without", names_property("Only entities with none of these components.")},
        {"where", where_schema()},
        {"include", include_schema()},
        {"omit_defaults", property("boolean", "Leave out fields equal to their defaults.")},
        {"limit", integer_property("Most entities to return; count still covers all.", 1, 10000,
                                   kDefaultQueryLimit)},
    };
}

namespace {

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

    json::Value kind = property("string", "\"scene\", \"prefab\", \"frames\" (.frames.json), "
                                          "\"material\" (.material.json), \"animation\" "
                                          "(.anim.json), \"tileset\" (.tileset.json), \"font\" "
                                          "(.font.json), \"theme\" (.theme.json), \"audio\" "
                                          "(.audio.json), \"bus_layout\" (.bus_layout.json), "
                                          "\"replay\" (.replay.json) or \"storyboard\" for "
                                          "those files, \"component\" for "
                                          "one component's fields, \"command\" for a command's "
                                          "params.");
    kind["enum"] = json::Array{"scene",   "prefab",     "frames",    "material", "animation",
                               "tileset", "font",       "theme",     "audio",    "bus_layout",
                               "replay",  "storyboard", "component", "command"};
    registry.add({
        .name = "schema.get",
        .description = "Returns a JSON Schema: for scene, prefab, .frames.json, .material.json, "
                       ".anim.json, .tileset.json, .font.json, .theme.json, .audio.json, "
                       ".bus_layout.json, .replay.json or storyboard files, one component or "
                       "one command.",
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

    json::Value uv_pair = property("array", "[u, v]");
    uv_pair["items"]["type"] = "number";
    uv_pair["minItems"] = 2;
    uv_pair["maxItems"] = 2;
    json::Value uvs = property("array", "Texture coordinates, from [0, 0] at the texture's top "
                                        "left to [1, 1].");
    uvs["items"] = uv_pair;
    uvs["minItems"] = 1;
    uvs["maxItems"] = 64;
    json::Value eval_color = property("array", "The vertex color, [r, g, b, a]. Default: the "
                                               "entity's modulate, or white.");
    eval_color["items"]["type"] = "number";
    eval_color["minItems"] = 4;
    eval_color["maxItems"] = 4;
    json::Value frag_coord = property("array", "What fragcoord reads, in pixels. Default: [0, 0].");
    frag_coord["items"]["type"] = "number";
    frag_coord["minItems"] = 2;
    frag_coord["maxItems"] = 2;
    json::Value eval_time = property("number", "What time reads, in seconds. Default: 0.");
    eval_time["minimum"] = 0;
    registry.add({
        .name = "material.eval",
        .description = "Returns the colors a material draws at texture coordinates, before "
                       "blending, computed as the GPU does: a check without a screenshot.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property("An entity with a Sprite2D: its material, texture, params and "
                                   "modulate. Or give path.")},
                {"path", property("string", "A .material.json file. Or give id.")},
                {"uv", std::move(uvs)},
                {"texture",
                 property("string", "The sprite's texture path. Default: the entity's, or "
                                    "none (white).")},
                {"params", property("object", "Param values, over the entity's material_params.")},
                {"color", std::move(eval_color)},
                {"frag_coord", std::move(frag_coord)},
                {"time", std::move(eval_time)},
            },
            {"uv"}),
        .handler = material_eval,
    });

    json::Value times = property("array", "Seconds into the animation, each from 0 to its length.");
    times["items"]["type"] = "number";
    times["items"]["minimum"] = 0;
    times["minItems"] = 1;
    times["maxItems"] = 100;
    json::Value only_tracks = property("array", "Value track paths to return. Default: all.");
    only_tracks["items"]["type"] = "string";
    only_tracks["minItems"] = 1;
    registry.add({
        .name = "animation.eval",
        .description = "Returns the values an animation's tracks set at times, as AnimationPlayer "
                       "sets them: a check without playing it.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property("An entity with an AnimationPlayer: its library and, by "
                                   "default, its animation. Or give path.")},
                {"path", property("string", "A .anim.json file. Or give id.")},
                {"animation", property("string", "The animation's name.")},
                {"times", std::move(times)},
                {"tracks", std::move(only_tracks)},
            },
            {"times"}),
        .handler = animation_eval,
    });

    // A cell is [column, row]; a rect is [column, row, width, height].
    json::Value coordinate;
    coordinate["type"] = "integer";
    coordinate["minimum"] = canvas::kMinCell;
    coordinate["maximum"] = canvas::kMaxCell;
    json::Value extent;
    extent["type"] = "integer";
    extent["minimum"] = 1;
    extent["maximum"] = kMaxTileCells;
    json::Value cell = property("array", "[column, row]");
    cell["items"] = coordinate;
    cell["minItems"] = 2;
    cell["maxItems"] = 2;
    json::Value tile_cells = property("array", "Cells as [column, row]. Or give rect.");
    tile_cells["items"] = std::move(cell);
    tile_cells["minItems"] = 1;
    tile_cells["maxItems"] = 4096;
    json::Value tile_rect =
        property("array", "[column, row, width, height]: every cell in it. Or give cells.");
    tile_rect["prefixItems"] = json::Array{coordinate, coordinate, extent, extent};
    tile_rect["minItems"] = 4;
    tile_rect["maxItems"] = 4;
    json::Value tile = any_property("A tile by name or as \"source:column,row\"; null erases.");
    tile["type"] = json::Array{"string", "null"};
    json::Value terrain = any_property("A terrain name; null erases.");
    terrain["type"] = json::Array{"string", "null"};
    const json::Value layer_id = id_property("An entity with a TileMapLayer.");
    registry.add({
        .name = "tilemap.set",
        .description = "Sets cells of an entity's TileMapLayer to one tile, or erases them. "
                       "Returns how many changed.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema({{"id", layer_id},
                                 {"cells", tile_cells},
                                 {"rect", tile_rect},
                                 {"tile", std::move(tile)}},
                                {"id", "tile"}),
        .handler = tilemap_set,
    });
    registry.add({
        .name = "tilemap.paint",
        .description = "Paints a terrain on cells of a TileMapLayer, picking tiles for them and "
                       "their neighbors so edges connect, as Godot's terrain painting. Returns "
                       "how many cells changed.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = params_schema({{"id", layer_id},
                                 {"cells", tile_cells},
                                 {"rect", tile_rect},
                                 {"terrain", std::move(terrain)}},
                                {"id", "terrain"}),
        .handler = tilemap_paint,
    });
    tile_rect["description"] = "[column, row, width, height]: only these cells. Default: all.";
    registry.add({
        .name = "tilemap.get",
        .description = "Returns a TileMapLayer's cells as rows of legend keys from an origin, "
                       "with \".\" for an empty cell.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"id", layer_id}, {"rect", std::move(tile_rect)}}, {"id"}),
        .handler = tilemap_get,
    });

    json::Value sample_point = property("array", "[x, y]");
    sample_point["items"]["type"] = "number";
    sample_point["minItems"] = 2;
    sample_point["maxItems"] = 2;
    json::Value sample_points = property("array", "Points as [x, y]. Or give id.");
    sample_points["items"] = std::move(sample_point);
    sample_points["minItems"] = 1;
    sample_points["maxItems"] = 256;
    registry.add({
        .name = "light.sample",
        .description = "Returns how lit points are, as the GPU shades a white pixel there: the "
                       "color and each light reaching it, with its shadow. A check without a "
                       "screenshot.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"points", std::move(sample_points)},
            {"id", id_property("An entity with a Transform2D: sample at its position, with its "
                               "Sprite2D or TileMapLayer's light mask and z index. Or give "
                               "points.")},
            {"light_mask", integer_property("The draw's light mask: lights whose "
                                            "range_item_cull_mask shares a bit reach it.",
                                            0, kMaxLightMask, 1)},
            {"z_index", integer_property("The draw's z index, for the lights' z ranges.",
                                         kMinZIndex, kMaxZIndex, 0)},
        }),
        .handler = light_sample,
    });

    json::Value capture_point = property("array", "[x, y]");
    capture_point["items"]["type"] = "number";
    capture_point["minItems"] = 2;
    capture_point["maxItems"] = 2;
    json::Value capture_points =
        property("array", "Window pixels as [x, y] to return the colors of.");
    capture_points["items"] = std::move(capture_point);
    capture_points["minItems"] = 1;
    capture_points["maxItems"] = 256;
    json::Value capture_region =
        property("array", "Only this part of the window: [x, y, width, height] in pixels.");
    capture_region["items"]["type"] = "number";
    capture_region["minItems"] = 4;
    capture_region["maxItems"] = 4;
    json::Value capture_scale =
        property("number", "Image pixels per window pixel, e.g. 0.5 for half size. Default: 1.");
    capture_scale["minimum"] = kMinCaptureScale;
    capture_scale["maximum"] = kMaxCaptureScale;
    json::Value capture_time =
        property("number", "Seconds of game time materials read. Default: the tick's.");
    capture_time["minimum"] = 0;
    registry.add({
        .name = "frame.capture",
        .description = "Draws the frame the window shows now, on the CPU, the same on every "
                       "machine. Returns its size and what it drew; \"image\" returns it as a "
                       "PNG to look at, \"path\" saves one (needs the filesystem capability) and "
                       "\"points\" returns the colors of pixels.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"collisions",
             property("boolean", "Draw collision shapes over the world, as Godot's Visible "
                                 "Collision Shapes.")},
            {"image", property("boolean", "Return the PNG, base64, as \"png\".")},
            {"path", property("string", "A .png file to write.")},
            {"points", std::move(capture_points)},
            {"region", std::move(capture_region)},
            {"scale", std::move(capture_scale)},
            {"time", std::move(capture_time)},
        }),
        .handler = frame_capture,
    });

    json::Value physics_point = property("array", "[x, y]");
    physics_point["items"]["type"] = "number";
    physics_point["minItems"] = 2;
    physics_point["maxItems"] = 2;
    const auto with_filter = [&](json::Object properties) {
        properties["collision_mask"] =
            integer_property("Layers to look in, as bits; default all.", 0, 0xFFFFFFFF, 0xFFFFFFFF);
        properties["exclude"] = names_property("Entity IDs to leave out.");
        json::Value bodies = property("boolean", "Find bodies. Default true.");
        properties["collide_with_bodies"] = std::move(bodies);
        json::Value areas = property("boolean", "Find areas. Default false.");
        properties["collide_with_areas"] = std::move(areas);
        return properties;
    };
    json::Value ray_from = physics_point;
    ray_from["description"] = "Where the ray starts, [x, y].";
    json::Value ray_to = physics_point;
    ray_to["description"] = "Where it ends, [x, y].";
    registry.add({
        .name = "physics.raycast",
        .description = "Returns the first body (or area) a ray from from to to hits: its id, the "
                       "point, the surface normal and how far along. hit is null if none.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params =
            params_schema(with_filter({{"from", std::move(ray_from)}, {"to", std::move(ray_to)}}),
                          {"from", "to"}),
        .handler = physics_raycast,
    });
    json::Value overlap_point = physics_point;
    overlap_point["description"] = "Find shapes containing this point, [x, y]. Or give shape.";
    json::Value overlap_position = physics_point;
    overlap_position["description"] = "Where the shape goes, [x, y].";
    registry.add({
        .name = "physics.overlap",
        .description = "Returns the IDs of bodies (or areas) whose shapes contain a point or "
                       "overlap a shape placed somewhere, sorted.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(with_filter({
            {"point", std::move(overlap_point)},
            {"shape", property("object", "CollisionShape2D fields, e.g. {\"shape\": "
                                         "\"circle\", \"radius\": 8}. Or give point.")},
            {"position", std::move(overlap_position)},
            {"rotation", property("number", "The shape's rotation, radians.")},
            {"max_results", integer_property("At most this many IDs.", 1, 256, 32)},
        })),
        .handler = physics_overlap,
    });
    registry.add({
        .name = "physics.contacts",
        .description = "Returns what touches an entity after the last step: bodies touching it "
                       "(for a character, what its last move hit and its floor) and the areas "
                       "it is in; for an area, what is in it.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"id", id_property("A body or area.")}}, {"id"}),
        .handler = physics_contacts,
    });

    json::Value particles_time = property("number", "Run its settings from a fresh start for this "
                                                    "many seconds instead, apart from the world.");
    particles_time["minimum"] = 0;
    particles_time["maximum"] = 3600;
    json::Value particles_tick = property("number", "Seconds per tick for time. Default 1/60.");
    particles_tick["exclusiveMinimum"] = 0;
    particles_tick["maximum"] = 1;
    registry.add({
        .name = "particles.eval",
        .description = "Returns a CPUParticles2D's live particles: how many, their bounds [x, y, "
                       "w, h] in the world and, with list, each one. A check without a "
                       "screenshot.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property("An entity with a CPUParticles2D.")},
                {"time", std::move(particles_time)},
                {"tick_seconds", std::move(particles_tick)},
                {"list", property("boolean", "Also return each particle. Default false.")},
                {"max_results", integer_property("Particles to list at most.", 1, 256, 32)},
            },
            {"id"}),
        .handler = particles_eval,
    });

    json::Value viewport = property("array", "[width, height] to lay out in. Default: the "
                                             "project's window size.");
    viewport["items"]["type"] = "number";
    viewport["items"]["exclusiveMinimum"] = 0;
    viewport["minItems"] = 2;
    viewport["maxItems"] = 2;
    registry.add({
        .name = "ui.layout",
        .description = "Returns where controls are on screen, in drawing order: each one's rect "
                       "[x, y, w, h], the lines labels, buttons, progress bars and text fields "
                       "show (rtl when they go right to left) and whether text overflows, each "
                       "button's draw_mode, each LineEdit's and TextEdit's caret and selection, "
                       "the control with focus and the one hovered, and mistakes such as "
                       "misspelled theme overrides. A check without a screenshot.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"ids", names_property("Only these controls. Default: all.")},
            {"viewport", std::move(viewport)},
        }),
        .handler = ui_layout,
    });
    registry.add({
        .name = "ui.theme",
        .description = "Returns the theme items a control uses (colors, constants, fonts, font "
                       "sizes, styles) as \"kind/name\", each with its value and where it "
                       "comes from: \"override\", a .theme.json path or \"default\".",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", property("string", "The control.")},
                {"items", names_property("Only these: kinds such as \"styles\" or items such "
                                         "as \"constants/separation\". Default: all.")},
            },
            {"id"}),
        .handler = ui_theme,
    });
    registry.add({
        .name = "ui.press",
        .description = "Presses a Button or TextureButton at the next tick, as a click would: "
                       "it emits pressed, and toggles first if it is a toggle button. Returns "
                       "the tick.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({{"id", property("string", "The button.")}}, {"id"}),
        .handler = ui_press,
    });
    json::Value focus_id = property("string", "The control, or null to take focus away.");
    focus_id["type"] = json::Array{"string", "null"};
    registry.add({
        .name = "ui.focus",
        .description = "Gives a control focus, so ui_accept presses it and ui_up, ui_down, "
                       "ui_left, ui_right and Tab move on from it; null takes focus away. "
                       "Returns the control with focus.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({{"id", std::move(focus_id)}}, {"id"}),
        .handler = ui_focus,
    });
    json::Value clipboard_text = property("string", "Text to put on the clipboard first.");
    clipboard_text["maxLength"] = 65536;
    registry.add({
        .name = "ui.clipboard",
        .description = "Returns the clipboard LineEdits cut, copy and paste with; text sets it "
                       "first. A game running in a window shares the system's.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({{"text", std::move(clipboard_text)}}),
        .handler = ui_clipboard,
    });
    json::Value measure_text = property("string", R"(The text; lines break at "\n".)");
    measure_text["maxLength"] = 65536;
    json::Value measure_width = property("number", "The width to wrap to, with autowrap_mode.");
    measure_width["exclusiveMinimum"] = 0;
    json::Value measure_wrap =
        property("string", "Where lines wrap, as Label's autowrap_mode. Default off.");
    measure_wrap["enum"] = json::Array{"off", "arbitrary", "word", "word_smart"};
    json::Value measure_direction =
        property("string", "The base direction; auto takes the first letter's. Default auto.");
    measure_direction["enum"] = json::Array{"auto", "ltr", "rtl"};
    registry.add({
        .name = "text.measure",
        .description = "Returns text's size in a font: [width, height], the line height and "
                       "each line, wrapped to a width if asked; rtl when lines go right to "
                       "left, and how many glyphs the font lacks (missing_glyphs) or came from "
                       "fonts installed on the machine (system_glyphs). Height has no line "
                       "spacing; a Label adds line_spacing (3) between lines.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"text", std::move(measure_text)},
                {"font", property("string", "A .ttf, .otf or .font.json file. Default: the "
                                            "built-in font.")},
                {"font_size",
                 integer_property("Pixels.", 1, text::kMaxFontSize, text::kDefaultFontSize)},
                {"autowrap_mode", std::move(measure_wrap)},
                {"width", std::move(measure_width)},
                {"direction", std::move(measure_direction)},
                {"language", property("string", "BCP 47, such as \"ar\" or \"ja\"; picks "
                                                "fallback fonts and glyphs.")},
            },
            {"text"}),
        .handler = text_measure,
    });

    json::Value inject_events = property("array", "Events in order. Mouse and finger positions "
                                                  "are window pixels from the top left.");
    inject_events["minItems"] = 1;
    inject_events["maxItems"] = 256;
    inject_events["items"] = input::event_schema(input::Use::Event);
    registry.add({
        .name = "input.inject",
        .description = "Queues input events (keys, typed text, IME composition, mouse, touches, "
                       "gamepads or actions by name) for the next tick, as if a player sent "
                       "them. A press and release before one step is a tap. Returns how many are "
                       "queued and the tick they apply at.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({{"events", std::move(inject_events)}}, {"events"}),
        .handler = input_inject,
    });
    registry.add({
        .name = "input.state",
        .description = "Returns what the game sees: actions pressed or just pressed or released "
                       "at the last tick (strength if partial), keys and mouse buttons held, the "
                       "mouse position, fingers down, gamepad buttons and axes, and events "
                       "queued.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {{"actions", names_property("Only these actions, pressed or not. Default: the ones "
                                        "pressed or just released, except text editing "
                                        "actions.")}}),
        .handler = input_state,
    });
    registry.add({
        .name = "input.actions",
        .description = "Returns input actions with their deadzone and the events bound to each: "
                       "Godot's ui_* actions and project.json's \"input\". Text editing actions "
                       "(ui_text_*, ui_cut, ui_copy and so on) only when named.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"names", names_property("Only these actions. Default: all.")}}),
        .handler = input_actions,
    });

    registry.add({
        .name = "audio.state",
        .description = "Returns the buses (volume, peak dB over the last mix step, null if silent, "
                       "flags, send, effects) and each player with an ID: stream, bus, voices, "
                       "position and length in seconds, volume after 2D distance and panning.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"ids", names_property("Only these players. Default: all.")}}),
        .handler = audio_state,
    });
    json::Value capture_seconds = property("number", "How far back, in seconds. Default: 1.");
    capture_seconds["minimum"] = 0;
    capture_seconds["maximum"] = kMaxCaptureSeconds;
    registry.add({
        .name = "audio.capture",
        .description = "Measures what a bus played over the last seconds (up to 10): peak and RMS "
                       "dB and the peak of each slice of that time, null if silent. Hear it with "
                       "\"path\", a .wav file to write (needs the filesystem capability).",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params =
            params_schema({{"bus", id_property("The bus. Default: \"Master\".")},
                           {"seconds", std::move(capture_seconds)},
                           {"slices", integer_property("Equal slices to report the peak of.", 0,
                                                       100, kDefaultCaptureSlices)},
                           {"path", property("string", "A .wav file to write the sound to.")}}),
        .handler = audio_capture,
    });
    json::Value play_from = property("number", "Seconds into the stream. Default: 0.");
    play_from["minimum"] = 0;
    registry.add({
        .name = "audio.play",
        .description = "Plays an AudioStreamPlayer or AudioStreamPlayer2D at the next tick, as "
                       "Godot's play(): it restarts, or adds a voice up to max_polyphony. "
                       "Returns the tick it plays at.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema(
            {{"id", id_property("The player's entity.")}, {"from", std::move(play_from)}}, {"id"}),
        .handler =
            [](Session& session, const json::Value& params) {
                return audio_play_or_stop(session, params, true);
            },
    });
    registry.add({
        .name = "audio.stop",
        .description = "Stops an AudioStreamPlayer or AudioStreamPlayer2D at the next tick, as "
                       "Godot's stop(). Returns the tick it stops at.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({{"id", id_property("The player's entity.")}}, {"id"}),
        .handler =
            [](Session& session, const json::Value& params) {
                return audio_play_or_stop(session, params, false);
            },
    });
    json::Value bus_volume = property("number", "Gain in dB.");
    bus_volume["minimum"] = -80;
    bus_volume["maximum"] = 24;
    registry.add({
        .name = "audio.set_bus",
        .description = "Changes a bus while the game runs, as Godot's AudioServer setters: volume, "
                       "mute, solo, bypass_effects, or one effect's enabled. The bus layout file "
                       "is unchanged. Returns the bus.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema(
            {{"bus", id_property("The bus, e.g. \"Master\".")},
             {"volume_db", std::move(bus_volume)},
             {"mute", property("boolean", "Silences it.")},
             {"solo", property("boolean", "Plays only solo buses and what they send to.")},
             {"bypass_effects", property("boolean", "Skips its effects.")},
             {"effect", integer_property("An effect's index on the bus, with enabled.", 0, 15, 0)},
             {"enabled", property("boolean", "Turns that effect on or off.")}},
            {"bus"}),
        .handler = audio_set_bus,
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

    registry.add({
        .name = "storyboard.brief",
        .description = "Call before a task: the storyboard facts it needs. Give the scenes, "
                       "entities, files or entry IDs it touches; returns linked entries, global "
                       "rules, open tasks and decisions about them, and with since, what changed.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"scenes", names_property("Scene files, e.g. [\"scenes/main.scene.json\"]; their "
                                      "entities' StoryRef entries count too.")},
            {"entities", names_property("Entity IDs in the open scene.")},
            {"files", names_property("Other project files: assets, prefabs, source.")},
            {"ids", names_property("Storyboard entry IDs.")},
            {"since", integer_property("Also list entries changed after this journal seq "
                                       "(from an earlier result).",
                                       0, std::numeric_limits<std::int32_t>::max(), 0)},
        }),
        .handler = storyboard_brief,
    });

    registry.add({
        .name = "storyboard.check",
        .description = "Call after a task: checks storyboard files, references, glossary words "
                       "to avoid and StoryRef IDs in scenes and prefabs; missing link targets are "
                       "warnings.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = storyboard_check,
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
        .params = params_schema(detail::selection_properties()),
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

    json::Value until_seconds = property("number", "Seconds per tick.");
    until_seconds["exclusiveMinimum"] = 0;
    until_seconds["maximum"] = 1;
    until_seconds["default"] = kDefaultTickSeconds;
    json::Value until = property("string", "Stop when any entity matches, or when none does.");
    until["enum"] = json::Array{"any", "none"};
    until["default"] = "any";
    json::Object until_params = detail::selection_properties();
    until_params.emplace("until", std::move(until));
    until_params.emplace("ticks",
                         integer_property("Most ticks to run.", 1, 100000, kDefaultUntilTicks));
    until_params.emplace("tick_seconds", std::move(until_seconds));
    registry.add({
        .name = "run.until",
        .description = "Steps the simulation until an entity matches with/without/where (e.g. "
                       "the player's position.y > 600), or none does, or ticks run out. Returns "
                       "whether it matched, the tick and the matching entities.",
        .capabilities = {Run},
        .effect = Effect::Edit,
        .params = params_schema(std::move(until_params)),
        .handler = run_until,
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

    register_replay_commands(registry);
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
