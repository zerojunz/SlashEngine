// The functions of slash_plugin.h, which plugins get by name through get_proc. Each checks what
// it is given, since a plugin's mistakes must not crash the engine where a check can catch them,
// and none throws or lets a C++ type cross the interface.

#include "internal.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/input/input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::plugin::detail {
namespace {

world::World* world_of(SlashWorld* world) {
    return reinterpret_cast<world::World*>(world);
}

entt::entity entity_of(SlashEntity entity) {
    return static_cast<entt::entity>(entity);
}

SlashEntity handle_of(entt::entity entity) {
    return entity == entt::null ? SLASH_NO_ENTITY
                                : static_cast<SlashEntity>(entt::to_integral(entity));
}

std::string_view text(const char* chars) {
    return chars != nullptr ? std::string_view(chars) : std::string_view();
}

// Writes text into a caller's buffer as snprintf does; returns its length.
std::int64_t put(std::string_view value, char* buffer, std::size_t capacity) {
    if (buffer != nullptr && capacity > 0) {
        const std::size_t count = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
    }
    return static_cast<std::int64_t>(value.size());
}

log::Level level_of(int level) {
    switch (level) {
    case SLASH_LOG_DEBUG:
        return log::Level::Debug;
    case SLASH_LOG_WARN:
        return log::Level::Warn;
    case SLASH_LOG_ERROR:
        return log::Level::Error;
    default:
        return log::Level::Info;
    }
}

bool valid_component_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'A' || name.front() > 'Z') {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

bool valid_field_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// A field type's bytes in the struct and the alignment it needs, or 0 for a type that isn't one.
std::pair<std::uint32_t, std::uint32_t> field_bytes(std::uint32_t type, std::uint32_t size) {
    switch (type) {
    case SLASH_FIELD_BOOL:
        return {1, 1};
    case SLASH_FIELD_INT32:
    case SLASH_FIELD_FLOAT:
    case SLASH_FIELD_ENTITY:
        return {4, 4};
    case SLASH_FIELD_INT64:
    case SLASH_FIELD_DOUBLE:
        return {8, 8};
    case SLASH_FIELD_VEC2:
        return {8, 4};
    case SLASH_FIELD_VEC3:
        return {12, 4};
    case SLASH_FIELD_VEC4:
        return {16, 4};
    case SLASH_FIELD_STRING:
        return {size, 1};
    default:
        return {0, 0};
    }
}

// What is wrong with a component's info, or "".
std::string check_component(const SlashComponentInfo& info) {
    const std::string_view name = text(info.name);
    if (!valid_component_name(name)) {
        return "a component name must be PascalCase letters and digits, at most 64";
    }
    if (info.size == 0 || info.size > detail::kBlobSizes.back()) {
        return "a component's size must be 1 to 4096 bytes";
    }
    if (info.align == 0 || info.align > detail::kMaxAlign || (info.align & (info.align - 1)) != 0) {
        return "a component's alignment must be a power of two up to 16";
    }
    if (info.fields == nullptr && info.field_count > 0) {
        return "a component with fields must give them";
    }
    std::vector<std::string_view> names;
    for (std::uint32_t i = 0; i < info.field_count; ++i) {
        const SlashField& field = info.fields[i];
        const std::string_view field_name = text(field.name);
        const std::string where =
            "field " + std::to_string(i) + " (" + std::string(field_name) + ")";
        if (!valid_field_name(field_name)) {
            return where + ": a field name must be snake_case, at most 64 characters";
        }
        if (std::ranges::find(names, field_name) != names.end()) {
            return where + ": the name is used twice";
        }
        names.push_back(field_name);
        if (field.type == SLASH_FIELD_STRING && field.size < 2) {
            return where + ": a string field needs a size of at least 2 bytes";
        }
        const auto [bytes, align] = field_bytes(field.type, field.size);
        if (bytes == 0) {
            return where + ": unknown field type " + std::to_string(field.type);
        }
        if (field.type != SLASH_FIELD_STRING && field.size != 0 && field.size != bytes) {
            return where + ": its size must be 0 or " + std::to_string(bytes);
        }
        if (field.offset % align != 0 || std::uint64_t{field.offset} + bytes > info.size) {
            return where + ": its offset doesn't fit the struct";
        }
        if ((field.has_min != 0 && !std::isfinite(field.min)) ||
            (field.has_max != 0 && !std::isfinite(field.max)) ||
            (field.has_min != 0 && field.has_max != 0 && field.min > field.max)) {
            return where + ": its range is not valid";
        }
    }
    return {};
}

SlashComponent register_component(SlashPlugin* plugin, const SlashComponentInfo* info) {
    if (plugin == nullptr || plugin->registration == nullptr) {
        log::error("a plugin registered a component outside its entry function");
        return 0;
    }
    Registration& registration = *plugin->registration;
    if (info == nullptr) {
        registration.errors.emplace_back("slash_register_component was given no info");
        return 0;
    }
    if (const std::string problem = check_component(*info); !problem.empty()) {
        registration.errors.push_back(std::string(text(info->name)) + ": " + problem);
        return 0;
    }
    if (registration.components->find(info->name) != nullptr) {
        registration.errors.push_back(std::string(info->name) +
                                      ": a component of that name exists");
        return 0;
    }
    Layout layout;
    layout.name = info->name;
    layout.plugin = registration.plugin;
    layout.size = info->size;
    layout.defaults.assign(info->size, std::byte{0});
    if (info->defaults != nullptr) {
        std::memcpy(layout.defaults.data(), info->defaults, info->size);
    }
    for (std::uint32_t i = 0; i < info->field_count; ++i) {
        const SlashField& field = info->fields[i];
        layout.fields.push_back(
            {.name = field.name,
             .type = static_cast<SlashFieldType>(field.type),
             .offset = field.offset,
             .size = field_bytes(field.type, field.size).first,
             .doc = std::string(text(field.doc)),
             .min = field.has_min != 0 ? std::optional(field.min) : std::nullopt,
             .max = field.has_max != 0 ? std::optional(field.max) : std::nullopt});
    }
    // A string field's default must end within it.
    for (const Field& field : layout.fields) {
        if (field.type == SLASH_FIELD_STRING) {
            layout.defaults[field.offset + field.size - 1] = std::byte{0};
        }
    }
    const Layout* added = add_layout(std::move(layout));
    if (added == nullptr) {
        registration.errors.push_back(std::string(info->name) +
                                      ": another plugin registered a component of that name");
        return 0;
    }
    world::ComponentType& type = registration.components->adopt(component_type(*added));
    if (info->doc != nullptr) {
        type.set_doc(info->doc);
    }
    registration.layouts.push_back(added);
    return added->id;
}

int register_system(SlashPlugin* plugin, const SlashSystemInfo* info) {
    if (plugin == nullptr || plugin->registration == nullptr) {
        log::error("a plugin registered a system outside its entry function");
        return 0;
    }
    Registration& registration = *plugin->registration;
    if (info == nullptr || info->run == nullptr || text(info->name).empty()) {
        registration.errors.emplace_back("a system needs a name and a run function");
        return 0;
    }
    if (std::ranges::find(registration.systems, text(info->name), &PluginSystem::name) !=
        registration.systems.end()) {
        registration.errors.push_back(std::string(info->name) + ": the system is registered twice");
        return 0;
    }
    registration.systems.push_back({.name = info->name,
                                    .doc = std::string(text(info->doc)),
                                    .priority = info->priority,
                                    .run = info->run,
                                    .userdata = info->userdata});
    return 1;
}

const char* engine_version() {
    static const std::string kVersion(slash::version());
    return kVersion.c_str();
}

void log_line(int level, const char* message, const char* fields_json) {
    const log::Level at = level_of(level);
    if (at < log::min_level()) {
        return;
    }
    json::Value line = json::Object{};
    line["level"] = log::to_string(at);
    line["msg"] = text(message);
    if (fields_json != nullptr) {
        if (auto fields = json::parse(fields_json); fields && fields.value().is_object()) {
            for (auto& [key, value] : fields.value().as_object()) {
                if (key != "level" && key != "msg") {
                    line[key] = std::move(value);
                }
            }
        }
    }
    if (const log::Sink sink = log::sink()) {
        sink(at, json::write(line, json::Style::Compact));
    }
}

SlashComponent component_find(SlashWorld* /*world*/, const char* name) {
    const Layout* layout = name != nullptr ? layout_by_name(name) : nullptr;
    return layout != nullptr ? layout->id : 0;
}

void* component_get(SlashWorld* world, SlashEntity entity, SlashComponent component) {
    const Layout* layout = layout_by_id(component);
    if (world == nullptr || layout == nullptr) {
        return nullptr;
    }
    return get(*world_of(world), *layout, entity_of(entity));
}

void* component_add(SlashWorld* world, SlashEntity entity, SlashComponent component) {
    const Layout* layout = layout_by_id(component);
    if (world == nullptr || layout == nullptr ||
        world_of(world)->components().find(layout->name) == nullptr) {
        return nullptr;
    }
    return add(*world_of(world), *layout, entity_of(entity));
}

int component_remove(SlashWorld* world, SlashEntity entity, SlashComponent component) {
    const Layout* layout = layout_by_id(component);
    if (world == nullptr || layout == nullptr) {
        return 0;
    }
    return remove(*world_of(world), *layout, entity_of(entity)) ? 1 : 0;
}

std::int64_t component_get_json(SlashWorld* world, SlashEntity entity, const char* name,
                                char* buffer, std::size_t capacity) {
    if (world == nullptr || name == nullptr) {
        return -1;
    }
    const world::World& into = *world_of(world);
    const world::ComponentType* type = into.components().find(name);
    if (type == nullptr || !type->has(into, entity_of(entity))) {
        return -1;
    }
    return put(json::write(type->get(into, entity_of(entity)), json::Style::Compact), buffer,
               capacity);
}

int component_set_json(SlashWorld* world, SlashEntity entity, const char* name, const char* json,
                       char* error, std::size_t error_capacity) {
    const auto fail = [&](std::string_view message) {
        (void)put(message, error, error_capacity);
        return 0;
    };
    if (world == nullptr || name == nullptr || json == nullptr) {
        return fail("a world, a component name and JSON are needed");
    }
    world::World& into = *world_of(world);
    const world::ComponentType* type = into.components().find(name);
    if (type == nullptr) {
        return fail("no component is named \"" + std::string(name) + "\"");
    }
    auto values = json::parse(json);
    if (!values) {
        return fail(values.error().message);
    }
    std::string path;
    if (auto assigned = type->assign(into, entity_of(entity), values.value(), path); !assigned) {
        return fail((path.empty() ? std::string() : path + ": ") + assigned.error().message);
    }
    return 1;
}

SlashEntity entity_create(SlashWorld* world, const char* id) {
    if (world == nullptr) {
        return SLASH_NO_ENTITY;
    }
    world::World& into = *world_of(world);
    if (id == nullptr) {
        return handle_of(into.create());
    }
    auto created = into.create(id);
    return created ? handle_of(created.value()) : SLASH_NO_ENTITY;
}

void entity_destroy(SlashWorld* world, SlashEntity entity) {
    if (world != nullptr && world_of(world)->valid(entity_of(entity))) {
        world_of(world)->destroy(entity_of(entity));
    }
}

SlashEntity entity_find(SlashWorld* world, const char* id) {
    if (world == nullptr || id == nullptr) {
        return SLASH_NO_ENTITY;
    }
    return handle_of(world_of(world)->find(id));
}

int entity_valid(SlashWorld* world, SlashEntity entity) {
    return world != nullptr && entity != SLASH_NO_ENTITY &&
                   world_of(world)->valid(entity_of(entity))
               ? 1
               : 0;
}

std::int64_t entity_id(SlashWorld* world, SlashEntity entity, char* buffer, std::size_t capacity) {
    if (entity_valid(world, entity) == 0) {
        return -1;
    }
    return put(world_of(world)->id_of(entity_of(entity)), buffer, capacity);
}

void world_each(SlashWorld* world, const SlashComponent* components, std::uint32_t count,
                void (*visit)(void* userdata, SlashEntity entity, void** components),
                void* userdata) {
    if (world == nullptr || visit == nullptr || count == 0 || components == nullptr || count > 32) {
        return;
    }
    world::World& into = *world_of(world);
    std::vector<const Layout*> layouts;
    layouts.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Layout* layout = layout_by_id(components[i]);
        if (layout == nullptr) {
            return;
        }
        layouts.push_back(layout);
    }
    // The smallest storage leads, as EnTT's views iterate.
    std::vector<entt::sparse_set*> sets;
    sets.reserve(layouts.size());
    for (const Layout* layout : layouts) {
        sets.push_back(&storage(into, *layout));
    }
    const entt::sparse_set* lead = *std::ranges::min_element(
        sets, {}, [](const entt::sparse_set* set) { return set->size(); });
    // A copy of the entities, so a visit that changes other storages can't break the walk.
    const std::vector<entt::entity> entities(lead->begin(), lead->end());
    std::array<void*, 32> found{};
    for (const entt::entity entity : entities) {
        bool all = true;
        for (std::size_t i = 0; i < layouts.size() && all; ++i) {
            found.at(i) = get(into, *layouts[i], entity);
            all = found.at(i) != nullptr;
        }
        if (all) {
            visit(userdata, handle_of(entity), found.data());
        }
    }
}

void world_emit(SlashWorld* world, const char* kind, SlashEntity about, const char* data_json) {
    if (world == nullptr || text(kind).empty()) {
        return;
    }
    world::World& into = *world_of(world);
    json::Value data;
    if (data_json != nullptr) {
        auto parsed = json::parse(data_json);
        if (!parsed) {
            log::warn("a plugin emitted an event with data that isn't JSON", {{"kind", kind}});
            return;
        }
        data = std::move(parsed).value();
    }
    const std::string_view id = about != SLASH_NO_ENTITY && into.valid(entity_of(about))
                                    ? into.id_of(entity_of(about))
                                    : std::string_view();
    into.emit(kind, id, std::move(data));
}

void world_events(SlashWorld* world,
                  void (*visit)(void* userdata, const char* kind, SlashEntity about,
                                const char* data_json),
                  void* userdata) {
    if (world == nullptr || visit == nullptr) {
        return;
    }
    const world::World& into = *world_of(world);
    // A copy, so events the visits emit wait for the next tick.
    const std::vector<world::Event> events(into.events().begin(), into.events().end());
    for (const world::Event& event : events) {
        const std::string data =
            event.data.is_null() ? std::string() : json::write(event.data, json::Style::Compact);
        visit(userdata, event.kind.c_str(), handle_of(into.find(event.id)),
              data.empty() ? nullptr : data.c_str());
    }
}

std::uint64_t world_tick(SlashWorld* world) {
    return world != nullptr ? world_of(world)->tick() : 0;
}

int input_pressed(SlashWorld* world, const char* action) {
    return world != nullptr && action != nullptr &&
                   input::is_action_pressed(*world_of(world), action)
               ? 1
               : 0;
}

int input_just_pressed(SlashWorld* world, const char* action) {
    return world != nullptr && action != nullptr &&
                   input::is_action_just_pressed(*world_of(world), action)
               ? 1
               : 0;
}

float input_strength(SlashWorld* world, const char* action) {
    return world != nullptr && action != nullptr
               ? input::get_action_strength(*world_of(world), action)
               : 0.0F;
}

template <class F>
SlashProc as_proc(F function) {
    return reinterpret_cast<SlashProc>(function);
}

} // namespace

SlashProc proc(const char* name) {
    static const std::array<std::pair<std::string_view, SlashProc>, 22> kProcs{{
        {"slash_engine_version", as_proc(&engine_version)},
        {"slash_log", as_proc(&log_line)},
        {"slash_register_component", as_proc(&register_component)},
        {"slash_register_system", as_proc(&register_system)},
        {"slash_component_find", as_proc(&component_find)},
        {"slash_component_get", as_proc(&component_get)},
        {"slash_component_add", as_proc(&component_add)},
        {"slash_component_remove", as_proc(&component_remove)},
        {"slash_component_get_json", as_proc(&component_get_json)},
        {"slash_component_set_json", as_proc(&component_set_json)},
        {"slash_entity_create", as_proc(&entity_create)},
        {"slash_entity_destroy", as_proc(&entity_destroy)},
        {"slash_entity_find", as_proc(&entity_find)},
        {"slash_entity_valid", as_proc(&entity_valid)},
        {"slash_entity_id", as_proc(&entity_id)},
        {"slash_world_each", as_proc(&world_each)},
        {"slash_world_emit", as_proc(&world_emit)},
        {"slash_world_events", as_proc(&world_events)},
        {"slash_world_tick", as_proc(&world_tick)},
        {"slash_input_pressed", as_proc(&input_pressed)},
        {"slash_input_just_pressed", as_proc(&input_just_pressed)},
        {"slash_input_strength", as_proc(&input_strength)},
    }};
    const std::string_view wanted = text(name);
    for (const auto& [known, function] : kProcs) {
        if (known == wanted) {
            return function;
        }
    }
    return nullptr;
}

} // namespace slash::plugin::detail
