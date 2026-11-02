// `rc`: Unreal Remote Control's calls (property, call and describe), as the Godot addon's
// lib/rc.gd (Autopilot, Apache 2.0) serves them over Godot's reflection, here over SlashEngine's
// entities, components and commands. An objectPath is an entity ID ("/root/" and a leading '/'
// are dropped, so Godot-style paths to a top-level entity work), or "Session" for the engine's
// commands. A propertyName is a component ("Health") or a field in one ("Health/current"; ':'
// works as '/' too, as Godot's indexed paths). A functionName is a command: on an entity, the
// entity's ID goes in as "id" unless the parameters name one.

#include "internal.hpp"

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace slash::autopilot::detail {

namespace {

bool is_session(std::string_view path) {
    return path == "Session" || path == "session" || path == "Engine" || path == "SlashEngine";
}

std::string strip_path(std::string_view path) {
    if (path.starts_with("/root/")) {
        path.remove_prefix(6);
    } else if (path.starts_with("/")) {
        path.remove_prefix(1);
    }
    return std::string(path);
}

std::string missing(std::string_view path) {
    return "Object " + std::string(path) + " does not exist";
}

// "Health:current" as "Health/current", split at '/'.
std::vector<std::string> split_name(std::string_view name) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : name) {
        if (c == '/' || c == ':') {
            parts.push_back(std::move(part));
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(std::move(part));
    return parts;
}

// The value at parts[1..] in a component's JSON, or nullptr.
json::Value* walk(json::Value& value, const std::vector<std::string>& parts) {
    json::Value* at = &value;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (at->is_object()) {
            at = at->find(parts[i]);
        } else if (at->is_array()) {
            // Vectors are arrays, so x, y, z and w name their items, as Godot's indexed paths.
            std::string key = parts[i];
            static constexpr std::string_view kAxes = "xyzw";
            if (key.size() == 1 && kAxes.find(key[0]) != std::string_view::npos) {
                key = std::to_string(kAxes.find(key[0]));
            }
            std::size_t index = 0;
            const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
            if (error != std::errc() || end != key.data() + key.size() ||
                index >= at->as_array().size()) {
                return nullptr;
            }
            at = &at->as_array()[index];
        } else {
            return nullptr;
        }
        if (at == nullptr) {
            return nullptr;
        }
    }
    return at;
}

std::string reply_error(const command::Reply& reply) {
    return reply.message.empty() ? std::string("the command failed") : reply.message;
}

Answer property(command::Session& session, const json::Value& body) {
    const json::Value* path_value = body.find("objectPath");
    const std::string raw =
        path_value != nullptr && path_value->is_string() ? path_value->as_string() : std::string();
    const std::string path = strip_path(raw);
    world::World const& world = session.world();
    const entt::entity entity = path.empty() ? entt::null : world.find(path);
    if (entity == entt::null) {
        return Answer::failure(missing(raw));
    }
    const json::Value* name_value = body.find("propertyName");
    const std::string name =
        name_value != nullptr && name_value->is_string() ? name_value->as_string() : "";
    const json::Value* new_value = body.find("propertyValue");
    const json::Value* access = body.find("access");
    const bool write =
        (new_value != nullptr && new_value->is_object()) ||
        (access != nullptr && access->is_string() && access->as_string().starts_with("WRITE"));
    if (name.empty()) {
        if (write) {
            return Answer::failure("A write needs propertyName");
        }
        json::Value all = json::Object{};
        for (const auto& type : world.components().types()) {
            if (type->has(world, entity)) {
                all[type->name()] = type->get(world, entity);
            }
        }
        return Answer::success(std::move(all));
    }
    const std::vector<std::string> parts = split_name(name);
    const world::ComponentType* type = world.components().find(parts[0]);
    if (type == nullptr || !type->has(world, entity)) {
        return Answer::failure("Property " + name + " does not exist on object " + raw);
    }
    json::Value current = type->get(world, entity);
    json::Value* at = walk(current, parts);
    if (at == nullptr) {
        return Answer::failure("Property " + name + " does not exist on object " + raw);
    }
    if (!write) {
        json::Value out;
        out[name] = *at;
        return Answer::success(std::move(out));
    }
    if (new_value == nullptr || !new_value->is_object()) {
        return Answer::failure("No value given for " + name);
    }
    const json::Value* given = new_value->find(name);
    if (given == nullptr && new_value->size() == 1) {
        given = &new_value->as_object().begin()->second;
    }
    if (given == nullptr) {
        return Answer::failure("No value given for " + name);
    }
    *at = *given;
    // component.set takes whole fields: the field the name is in, with the part changed.
    json::Value fields = json::Object{};
    if (parts.size() == 1) {
        fields = current;
    } else {
        fields[parts[1]] = *current.find(parts[1]);
    }
    json::Value components;
    components[type->name()] = std::move(fields);
    json::Value params;
    params["id"] = path;
    params["components"] = std::move(components);
    if (command::Reply const reply = session.call("component.set", params); !reply.ok) {
        return Answer::failure(reply_error(reply));
    }
    return Answer::success(json::Object{});
}

Answer call(command::Session& session, const json::Value& body) {
    const json::Value* path_value = body.find("objectPath");
    const std::string raw =
        path_value != nullptr && path_value->is_string() ? path_value->as_string() : std::string();
    const std::string path = strip_path(raw);
    entt::entity entity = entt::null;
    if (!path.empty() && !is_session(path)) {
        entity = session.world().find(path);
        if (entity == entt::null) {
            return Answer::failure(missing(raw));
        }
    }
    const json::Value* function = body.find("functionName");
    const std::string name =
        function != nullptr && function->is_string() ? function->as_string() : "";
    const command::Command* command = session.commands().find(name);
    if (command == nullptr) {
        return Answer::failure("Function: " + name + " does not exist on object: " + raw);
    }
    json::Value params = json::Object{};
    if (const json::Value* given = body.find("parameters");
        given != nullptr && given->is_object()) {
        params = *given;
    }
    if (entity != entt::null && params.find("id") == nullptr) {
        params["id"] = path;
    }
    command::Reply reply = session.call(name, params);
    if (!reply.ok) {
        return Answer::failure(name + " on " + raw + ": " + reply_error(reply));
    }
    json::Value out;
    out["ReturnValue"] = std::move(reply.value);
    return Answer::success(std::move(out));
}

json::Value function_of(const command::Command& command) {
    json::Value out;
    out["Name"] = command.name;
    out["Description"] = command.description;
    json::Array arguments;
    if (const json::Value* properties = command.params.find("properties");
        properties != nullptr && properties->is_object()) {
        for (const auto& [key, schema] : properties->as_object()) {
            if (key == "dry_run") {
                continue;
            }
            json::Value argument;
            argument["Name"] = key;
            const json::Value* type = schema.find("type");
            argument["Type"] = type != nullptr && type->is_string() ? type->as_string() : "any";
            arguments.push_back(std::move(argument));
        }
    }
    out["Arguments"] = std::move(arguments);
    return out;
}

Answer describe(command::Session& session, const json::Value& body) {
    const json::Value* path_value = body.find("objectPath");
    const std::string raw =
        path_value != nullptr && path_value->is_string() ? path_value->as_string() : std::string();
    const std::string path = strip_path(raw);
    const world::World& world = session.world();
    json::Array functions;
    if (path.empty() || is_session(path)) {
        for (const command::Command& command : session.commands().commands()) {
            functions.push_back(function_of(command));
        }
        json::Value out;
        out["Name"] = "Session";
        out["Class"] = "SlashEngine session";
        out["Properties"] = json::Array{};
        out["Functions"] = std::move(functions);
        return Answer::success(std::move(out));
    }
    const entt::entity entity = world.find(path);
    if (entity == entt::null) {
        return Answer::failure(missing(raw));
    }
    json::Array properties;
    std::string first;
    for (const auto& type : world.components().types()) {
        if (!type->has(world, entity)) {
            continue;
        }
        if (first.empty()) {
            first = type->name();
        }
        for (const std::string& field : type->fields()) {
            json::Value property;
            property["Name"] = type->name() + "/" + field;
            const world::FieldMeta* meta = type->field_meta(field);
            property["Description"] = meta != nullptr ? meta->doc : std::string();
            const json::Value* fallback = type->defaults().find(field);
            static constexpr std::array<std::string_view, 7> kTypes{
                "null", "bool", "int", "float", "String", "Array", "Dictionary"};
            property["Type"] = std::string(
                fallback != nullptr ? kTypes[static_cast<std::size_t>(fallback->type())] : "any");
            properties.push_back(std::move(property));
        }
    }
    // Commands that act on one entity: those with an "id" parameter.
    for (const command::Command& command : session.commands().commands()) {
        if (const json::Value* props = command.params.find("properties");
            props != nullptr && props->find("id") != nullptr) {
            functions.push_back(function_of(command));
        }
    }
    json::Value out;
    const std::size_t slash = path.rfind('/');
    out["Name"] = slash == std::string::npos ? path : path.substr(slash + 1);
    out["Class"] = first.empty() ? std::string("Entity") : first;
    out["Properties"] = std::move(properties);
    out["Functions"] = std::move(functions);
    return Answer::success(std::move(out));
}

} // namespace

Answer rc(command::Session& session, std::string_view verb, const json::Value* body) {
    if (body == nullptr || !body->is_object()) {
        return Answer::failure("rc needs a body");
    }
    if (verb == "call") {
        return call(session, *body);
    }
    if (verb == "property") {
        return property(session, *body);
    }
    if (verb == "describe") {
        return describe(session, *body);
    }
    return Answer::failure("unknown rc verb " + std::string(verb));
}

} // namespace slash::autopilot::detail
