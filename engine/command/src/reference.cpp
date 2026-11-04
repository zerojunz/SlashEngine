// The engine's reference pages, written from engine.describe as Godot's --doctool writes its
// class reference from the running engine.

#include "slash/command/reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace slash::command {
namespace {

constexpr std::string_view kGenerated =
    "<!-- Written by `slash-cli docs` from engine.describe. Don't edit: run it again. -->\n\n";
// How deep nested params and fields are listed.
constexpr int kMaxDepth = 4;

const json::Array& array_of(const json::Value& value, std::string_view key) {
    static const json::Array kEmpty;
    const json::Value* found = value.find(key);
    return found != nullptr && found->is_array() ? found->as_array() : kEmpty;
}

std::string string_of(const json::Value& value, std::string_view key) {
    const json::Value* found = value.find(key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

// Text for a table cell: one line, its pipes escaped.
std::string cell(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// GitHub's anchor for a heading: lowercase, spaces as dashes, other punctuation left out.
std::string anchor(std::string_view heading) {
    std::string out;
    for (const char c : heading) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out += c;
        } else if (c == ' ') {
            out += '-';
        }
    }
    return out;
}

std::string code(std::string_view text) {
    // A backtick inside is shown with doubled ones around it, as Markdown allows.
    return text.find('`') == std::string_view::npos ? "`" + std::string(text) + "`"
                                                    : "`` " + std::string(text) + " ``";
}

std::string number_text(const json::Value& value) {
    if (value.is_int()) {
        return std::to_string(value.as_int());
    }
    const double number = value.as_double();
    if (std::floor(number) == number && std::abs(number) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(number));
    }
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%g", number);
    return text.data();
}

// A schema's type in words: its type or enum, its items for arrays, its def for a $ref (a link
// to the section for_ref names), and its limits.
std::string type_text(const json::Value& schema, std::string_view owner) {
    if (const json::Value* ref = schema.find("$ref"); ref != nullptr && ref->is_string()) {
        const std::string& target = ref->as_string();
        const std::string name = target.substr(target.rfind('/') + 1);
        if (name == owner) {
            return code(name);
        }
        return "[" + name + "](#" + anchor(std::string(owner) + " " + name) + ")";
    }
    if (const json::Value* choices = schema.find("enum");
        choices != nullptr && choices->is_array()) {
        std::string out;
        const json::Array& values = choices->as_array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out += i + 1 == values.size() ? " or " : ", ";
            }
            out += code(json::write(values[i], json::Style::Compact));
        }
        return out;
    }
    std::string out;
    const json::Value* type = schema.find("type");
    if (type != nullptr && type->is_array()) {
        for (const json::Value& part : type->as_array()) {
            out += (out.empty() ? "" : " or ") + (part.is_string() ? part.as_string() : "");
        }
    } else if (type != nullptr && type->is_string()) {
        out = type->as_string();
    } else {
        out = "any";
    }
    if (out == "array") {
        const json::Value* items = schema.find("items");
        const std::string item =
            items != nullptr && items->is_object() ? type_text(*items, owner) : "any";
        const json::Value* low = schema.find("minItems");
        const json::Value* high = schema.find("maxItems");
        if (low != nullptr && high != nullptr && low->is_number() && high->is_number() &&
            low->as_double() == high->as_double()) {
            out = "array of " + number_text(*low) + " " + item;
        } else {
            out = "array of " + item;
            if (low != nullptr && low->is_number() && low->as_double() > 0.0) {
                out += ", at least " + number_text(*low);
            }
            if (high != nullptr && high->is_number()) {
                out += ", at most " + number_text(*high);
            }
        }
        return out;
    }
    if (out == "object") {
        const json::Value* values = schema.find("additionalProperties");
        if (values != nullptr && values->is_object() && values->size() > 0 &&
            schema.find("properties") == nullptr) {
            return "object of " + type_text(*values, owner) + " by name";
        }
        return out;
    }
    const json::Value* low = schema.find("minimum");
    const json::Value* above = schema.find("exclusiveMinimum");
    const json::Value* high = schema.find("maximum");
    if (low != nullptr && high != nullptr) {
        out += ", " + number_text(*low) + " to " + number_text(*high);
    } else if (low != nullptr) {
        out += ", at least " + number_text(*low);
    } else if (above != nullptr) {
        out += ", above " + number_text(*above);
    } else if (high != nullptr) {
        out += ", at most " + number_text(*high);
    }
    return out;
}

bool is_required(const json::Value& schema, std::string_view name) {
    return std::ranges::any_of(array_of(schema, "required"), [&](const json::Value& value) {
        return value.is_string() && value.as_string() == name;
    });
}

// A table row for each property of an object schema, then for the properties of objects in it
// (an array's items as name[]), down to kMaxDepth.
void property_rows(const json::Value& schema, const std::string& prefix, std::string_view owner,
                   bool with_required, int depth, std::string& out) {
    const json::Value* properties = schema.find("properties");
    if (properties == nullptr || !properties->is_object() || depth > kMaxDepth) {
        return;
    }
    for (const auto& [name, property] : properties->as_object()) {
        const std::string path = prefix + name;
        std::string default_text;
        if (with_required && is_required(schema, name)) {
            default_text = "required";
        } else if (const json::Value* value = property.find("default")) {
            default_text = code(json::write(*value, json::Style::Compact));
        }
        out += "| " + code(path) + " | " + cell(type_text(property, owner)) + " | " +
               cell(default_text) + " | " + cell(string_of(property, "description")) + " |\n";
        property_rows(property, path + ".", owner, with_required, depth + 1, out);
        const json::Value* items = property.find("items");
        if (items != nullptr && items->is_object()) {
            property_rows(*items, path + "[].", owner, with_required, depth + 1, out);
        }
    }
}

// Each item sorted by its name.
std::vector<const json::Value*> by_name(const json::Value& describe, std::string_view key) {
    std::vector<const json::Value*> out;
    for (const json::Value& item : array_of(describe, key)) {
        if (item.is_object()) {
            out.push_back(&item);
        }
    }
    std::ranges::sort(out, [](const json::Value* a, const json::Value* b) {
        return string_of(*a, "name") < string_of(*b, "name");
    });
    return out;
}

std::string version_line(const json::Value& describe) {
    const json::Value* engine = describe.find("engine");
    const std::string version = engine != nullptr ? string_of(*engine, "version") : std::string();
    return version.empty() ? std::string() : " (SlashEngine " + version + ")";
}

// Links to each item's section, as one paragraph.
std::string index_of(const std::vector<const json::Value*>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string name = string_of(*items[i], "name");
        out += (i > 0 ? " · " : "") + std::string("[") + name + "](#" + anchor(name) + ")";
    }
    return out + "\n";
}

std::string components_page(const json::Value& describe) {
    const std::vector<const json::Value*> components = by_name(describe, "components");
    std::string out(kGenerated);
    out += "# Components\n\nThe components scenes and prefabs hold" + version_line(describe) +
           ", each with its fields as scene files write them: their types, limits, defaults "
           "(fields equal to them are left out of files) and what they do. "
           "[scene-format.md](../reference/scene-format.md) tells how they work together.\n\n";
    out += index_of(components);
    for (const json::Value* component : components) {
        const std::string name = string_of(*component, "name");
        out += "\n## " + name + "\n\n" + string_of(*component, "doc") + "\n";
        const json::Value* schema = component->find("schema");
        const json::Value* defs = schema != nullptr ? schema->find("$defs") : nullptr;
        if (defs == nullptr || !defs->is_object()) {
            continue;
        }
        const json::Value* own = defs->find(name);
        if (own != nullptr && own->find("properties") != nullptr &&
            own->find("properties")->size() > 0) {
            out += "\n| Field | Type | Default | Description |\n| --- | --- | --- | --- |\n";
            property_rows(*own, "", name, false, 0, out);
        }
        // The structs its fields hold, each in a section of its own.
        for (const auto& [def, def_schema] : defs->as_object()) {
            if (def == name) {
                continue;
            }
            out += "\n### ";
            out += name;
            out += " ";
            out += def;
            out += "\n\n";
            if (const std::string doc = string_of(def_schema, "description"); !doc.empty()) {
                out += doc + "\n\n";
            }
            if (def_schema.find("properties") != nullptr) {
                out += "| Field | Type | Default | Description |\n| --- | --- | --- | --- |\n";
                property_rows(def_schema, "", name, false, 0, out);
            } else {
                out += "A " + cell(type_text(def_schema, name)) + ".\n";
            }
        }
    }
    return out;
}

std::string commands_page(const json::Value& describe) {
    const std::vector<const json::Value*> commands = by_name(describe, "commands");
    std::string out(kGenerated);
    out += "# Commands\n\nThe commands every front end serves" + version_line(describe) +
           ": slash-cli's `call` and `rpc`, MCP (with `_` for `.` in tool names), JSON-RPC and "
           "REST (`POST /v1/commands/<name>`). Each lists the capabilities a session must grant "
           "and its params; [commands.md](../reference/commands.md) has their replies and "
           "examples.\n\n";
    out += index_of(commands);
    for (const json::Value* command : commands) {
        const std::string name = string_of(*command, "name");
        out += "\n## " + name + "\n\n" + string_of(*command, "description") + "\n";
        std::string needs;
        for (const json::Value& capability : array_of(*command, "capabilities")) {
            if (capability.is_string()) {
                needs += (needs.empty() ? "" : ", ") + code(capability.as_string());
            }
        }
        if (!needs.empty()) {
            out += "\nNeeds " + needs + ".\n";
        }
        const json::Value* params = command->find("params");
        const json::Value* properties = params != nullptr ? params->find("properties") : nullptr;
        if (properties == nullptr || properties->size() == 0) {
            out += "\nNo params.\n";
            continue;
        }
        out += "\n| Param | Type | Default | Description |\n| --- | --- | --- | --- |\n";
        property_rows(*params, "", name, true, 0, out);
    }
    return out;
}

std::string systems_page(const json::Value& describe) {
    std::string out(kGenerated);
    out += "# Systems\n\nThe systems a session's world runs each tick" + version_line(describe) +
           ", in order: lowest priority first. A game's own systems join them by priority.\n\n"
           "| Priority | System | What it does |\n| ---: | --- | --- |\n";
    std::vector<const json::Value*> systems;
    for (const json::Value& system : array_of(describe, "systems")) {
        if (system.is_object()) {
            systems.push_back(&system);
        }
    }
    std::ranges::stable_sort(systems, [](const json::Value* a, const json::Value* b) {
        const json::Value* x = a->find("priority");
        const json::Value* y = b->find("priority");
        return (x != nullptr && x->is_number() ? x->as_double() : 0.0) <
               (y != nullptr && y->is_number() ? y->as_double() : 0.0);
    });
    for (const json::Value* system : systems) {
        const json::Value* priority = system->find("priority");
        out += "| " + (priority != nullptr && priority->is_number() ? number_text(*priority) : "") +
               " | " + code(string_of(*system, "name")) + " | " + cell(string_of(*system, "doc")) +
               " |\n";
    }
    return out;
}

std::string readme_page(const json::Value& describe) {
    std::string out(kGenerated);
    out += "# Engine reference\n\nWhat the engine" + version_line(describe) +
           " says of itself through `engine.describe`, written as Markdown by `slash-cli docs`, "
           "as Godot's class reference is written by its `--doctool`. Run in a game's folder "
           "(`--project`), it takes in the game's own components and systems too.\n\n";
    out += "- [Components](components.md): " +
           std::to_string(array_of(describe, "components").size()) +
           ", with their fields\n- [Commands](commands.md): " +
           std::to_string(array_of(describe, "commands").size()) +
           ", with their params\n- [Systems](systems.md): " +
           std::to_string(array_of(describe, "systems").size()) + ", in the order they run\n";
    std::string capabilities;
    for (const json::Value& capability : array_of(describe, "capabilities")) {
        if (capability.is_string()) {
            capabilities += (capabilities.empty() ? "" : ", ") + code(capability.as_string());
        }
    }
    if (!capabilities.empty()) {
        out += "\nCapabilities a session can grant: " + capabilities + ".\n";
    }
    return out;
}

// The paths of an object schema's properties with no description, as prefix + name.
void undocumented_properties(const json::Value& schema, const std::string& prefix,
                             std::string_view kind, int depth, std::vector<std::string>& out) {
    const json::Value* properties = schema.find("properties");
    if (properties == nullptr || !properties->is_object() || depth > kMaxDepth) {
        return;
    }
    for (const auto& [name, property] : properties->as_object()) {
        const std::string path = prefix + name;
        if (string_of(property, "description").empty()) {
            out.push_back(std::string(kind) + " " + path);
        }
        undocumented_properties(property, path + ".", kind, depth + 1, out);
        const json::Value* items = property.find("items");
        if (items != nullptr && items->is_object()) {
            undocumented_properties(*items, path + "[].", kind, depth + 1, out);
        }
    }
}

} // namespace

std::vector<ReferencePage> reference_pages(const json::Value& describe) {
    return {
        {.path = "README.md", .text = readme_page(describe)},
        {.path = "components.md", .text = components_page(describe)},
        {.path = "commands.md", .text = commands_page(describe)},
        {.path = "systems.md", .text = systems_page(describe)},
    };
}

std::vector<std::string> undocumented(const json::Value& describe) {
    std::vector<std::string> out;
    for (const json::Value* component : by_name(describe, "components")) {
        const std::string name = string_of(*component, "name");
        if (string_of(*component, "doc").empty()) {
            out.push_back("component " + name);
        }
        const json::Value* schema = component->find("schema");
        const json::Value* defs = schema != nullptr ? schema->find("$defs") : nullptr;
        if (defs == nullptr || !defs->is_object()) {
            continue;
        }
        for (const auto& [def, def_schema] : defs->as_object()) {
            undocumented_properties(def_schema, def + ".", "field", 0, out);
        }
    }
    for (const json::Value* command : by_name(describe, "commands")) {
        const std::string name = string_of(*command, "name");
        if (string_of(*command, "description").empty()) {
            out.push_back("command " + name);
        }
        if (const json::Value* params = command->find("params")) {
            undocumented_properties(*params, name + ".", "param", 0, out);
        }
    }
    // A struct that several components hold is listed once for each; keep one.
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace slash::command
