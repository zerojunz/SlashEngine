#include "slash/storyboard/storyboard.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace slash::storyboard {
namespace {

using enum FieldType;

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::vector<Kind> make_kinds() {
    return {
        {.name = "palette",
         .file = "style",
         .doc = "Named colors the art uses; textures in its links may use only these.",
         .fields = {{.name = "colors",
                     .type = Colors,
                     .doc = R"(Color names mapped to "#rrggbb", e.g. {"sky": "#87ceeb"}.)"},
                    {.name = "tolerance",
                     .type = Integer,
                     .doc = "How far, 0 to 255 per channel, a pixel may be from a color; 0 if "
                            "unset."}}},
        {.name = "tone", .file = "style", .doc = "The mood and voice of the game, or of a part."},
        {.name = "reference",
         .file = "style",
         .doc = "Art or writing to follow; put the file in links."},
        {.name = "budget",
         .file = "style",
         .doc = "Limits for the assets in its links, or for all.",
         .fields = {{.name = "triangles", .type = Integer, .doc = "Most triangles per model."},
                    {.name = "texture_size",
                     .type = Integer,
                     .doc = "Largest texture side, in pixels."}}},
        {.name = "character",
         .file = "world",
         .doc = "A person or creature in the game.",
         .fields = {{.name = "faction",
                     .type = Ref,
                     .doc = "The faction they belong to.",
                     .ref_kind = "faction"},
                    {.name = "home", .type = Ref, .doc = "Where they live.", .ref_kind = "place"},
                    {.name = "traits", .type = Texts, .doc = "Short traits, e.g. \"reckless\"."},
                    {.name = "voice", .type = Text, .doc = "How they speak."}}},
        {.name = "place",
         .file = "world",
         .doc = "A location.",
         .fields = {{.name = "region",
                     .type = Ref,
                     .doc = "The larger place this one is in.",
                     .ref_kind = "place"}}},
        {.name = "faction",
         .file = "world",
         .doc = "A group, side or organization.",
         .fields =
             {{.name = "leader", .type = Ref, .doc = "Who leads it.", .ref_kind = "character"}}},
        {.name = "item",
         .file = "world",
         .doc = "An object that matters to the world or the story.",
         .fields = {{.name = "owner", .type = Ref, .doc = "Who has it.", .ref_kind = "character"}}},
        {.name = "lore", .file = "world", .doc = "A fact about the world's history or nature."},
        {.name = "act",
         .file = "story",
         .doc = "A major part of the story.",
         .fields = {{.name = "order", .type = Integer, .doc = "Position in the story, from 1."}}},
        {.name = "beat",
         .file = "story",
         .doc = "One event in the story.",
         .fields = {{.name = "act", .type = Ref, .doc = "The act it is in.", .ref_kind = "act"},
                    {.name = "order", .type = Integer, .doc = "Position in its act, from 1."},
                    {.name = "place", .type = Ref, .doc = "Where it happens.", .ref_kind = "place"},
                    {.name = "characters",
                     .type = Refs,
                     .doc = "Who takes part.",
                     .ref_kind = "character"}}},
        {.name = "quest",
         .file = "story",
         .doc = "A goal the player can take on.",
         .fields = {{.name = "giver", .type = Ref, .doc = "Who gives it.", .ref_kind = "character"},
                    {.name = "steps", .type = Texts, .doc = "What the player does, in order."}}},
        {.name = "mechanic",
         .file = "design",
         .doc = "A system the player interacts with, e.g. wall running.",
         .fields = {{.name = "controls",
                     .type = Refs,
                     .doc = "The controls it uses.",
                     .ref_kind = "control"}}},
        {.name = "rule",
         .file = "design",
         .doc = "A design rule every change must keep, e.g. \"no fall damage\"."},
        {.name = "tuning",
         .file = "design",
         .doc = "A gameplay number and its limits.",
         .fields = {{.name = "value", .type = Number, .doc = "The current value."},
                    {.name = "unit", .type = Text, .doc = "e.g. \"px/s\"."},
                    {.name = "min", .type = Number, .doc = "Lowest allowed value."},
                    {.name = "max", .type = Number, .doc = "Highest allowed value."}}},
        {.name = "control",
         .file = "design",
         .doc = "An input action and its bindings.",
         .fields = {{.name = "action", .type = Text, .doc = "The action name, e.g. \"jump\"."},
                    {.name = "keys", .type = Texts, .doc = "Keyboard and mouse, e.g. [\"Space\"]."},
                    {.name = "gamepad", .type = Texts, .doc = "Gamepad, e.g. [\"A\"]."}}},
        {.name = "term",
         .file = "glossary",
         .doc = "A canonical name; name is the spelling to use.",
         .fields = {{.name = "aliases", .type = Texts, .doc = "Other accepted names."},
                    {.name = "avoid", .type = Texts, .doc = "Words never to use for it."}}},
        {.name = "decision",
         .file = "decisions",
         .doc = "Something decided, by whom and why.",
         .fields = {{.name = "status",
                     .type = Choice,
                     .doc = "proposed, accepted or superseded.",
                     .choices = {"proposed", "accepted", "superseded"}},
                    {.name = "by", .type = Text, .doc = "Who decided."},
                    {.name = "date", .type = Text, .doc = "When, as YYYY-MM-DD."},
                    {.name = "why", .type = Text, .doc = "The reason."},
                    {.name = "supersedes",
                     .type = Ref,
                     .doc = "The decision this replaces.",
                     .ref_kind = "decision"}}},
        {.name = "task",
         .file = "work",
         .doc = "Open work: refs and links say what it touches.",
         .fields = {{.name = "status",
                     .type = Choice,
                     .doc = "open, in_progress, blocked or done.",
                     .choices = {"open", "in_progress", "blocked", "done"}},
                    {.name = "owner", .type = Text, .doc = "The agent or person on it."},
                    {.name = "questions", .type = Texts, .doc = "Open questions."}}},
    };
}

json::Value field_schema(const Field& field) {
    json::Value schema;
    switch (field.type) {
    case Text:
    case Ref:
        schema["type"] = "string";
        break;
    case Number:
        schema["type"] = "number";
        break;
    case Integer:
        schema["type"] = "integer";
        break;
    case Refs:
    case Texts:
    case Paths:
        schema["type"] = "array";
        schema["items"]["type"] = "string";
        break;
    case Colors:
        schema["type"] = "object";
        schema["additionalProperties"]["type"] = "string";
        break;
    case Choice: {
        schema["type"] = "string";
        json::Array choices;
        for (const std::string_view choice : field.choices) {
            choices.emplace_back(choice);
        }
        schema["enum"] = std::move(choices);
        break;
    }
    }
    schema["description"] = field.doc;
    return schema;
}

// Each field of a kind, the common ones first.
template <typename Visit>
void for_each_field(const Kind& kind, Visit&& visit) {
    for (const Field& field : common_fields()) {
        visit(field);
    }
    for (const Field& field : kind.fields) {
        visit(field);
    }
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool is_color(std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdefABCDEF";
    return text.size() == 7 && text.front() == '#' &&
           text.find_first_not_of(kHex, 1) == std::string_view::npos;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return out;
}

bool contains_text(const json::Value& value, std::string_view needle) {
    switch (value.type()) {
    case json::Type::String:
        return lowered(value.as_string()).find(needle) != std::string::npos;
    case json::Type::Array:
        return std::ranges::any_of(
            value.as_array(), [&](const json::Value& item) { return contains_text(item, needle); });
    case json::Type::Object:
        return std::ranges::any_of(value.as_object(), [&](const auto& member) {
            return contains_text(member.second, needle);
        });
    default:
        return false;
    }
}

// Whether a link names the path, or a folder the path is in.
bool covers(std::string_view link, std::string_view path) {
    return path.starts_with(link) && (path.size() == link.size() || path[link.size()] == '/');
}

bool links_to(const Kind& kind, const json::Value& entry, std::string_view target) {
    bool found = false;
    for_each_field(kind, [&](const Field& field) {
        const json::Value* value = entry.find(field.name);
        if (found || value == nullptr || field.type != Paths || !value->is_array()) {
            return;
        }
        found = std::ranges::any_of(value->as_array(), [&](const json::Value& path) {
            return path.is_string() && covers(path.as_string(), target);
        });
    });
    if (found) {
        return true;
    }
    return std::ranges::any_of(references(kind, entry), [&](const Reference& reference) {
        return reference.target == target;
    });
}

// A query or brief result: the ID, the version and every field, or only name and summary.
json::Value entry_item(const std::string& id, const json::Value& entry, bool full) {
    json::Value item;
    item["id"] = id;
    item["version"] = version(entry);
    if (full) {
        item["fields"] = entry;
        return item;
    }
    json::Value& fields = item["fields"];
    fields = json::Object{};
    for (const std::string_view name : {"name", "summary"}) {
        if (const json::Value* value = entry.find(name); value != nullptr) {
            fields[name] = *value;
        }
    }
    return item;
}

// Every string in an entry's Text and Texts fields, with its path inside the entry.
template <typename Visit>
void for_each_text(const Kind& kind, const json::Value& entry, Visit&& visit) {
    for_each_field(kind, [&](const Field& field) {
        const json::Value* value = entry.find(field.name);
        if (value == nullptr) {
            return;
        }
        const std::string path = "/" + std::string(field.name);
        if (field.type == Text && value->is_string()) {
            visit(value->as_string(), path);
        } else if (field.type == Texts && value->is_array()) {
            for (std::size_t i = 0; i < value->size(); ++i) {
                if (const json::Value& item = value->as_array()[i]; item.is_string()) {
                    visit(item.as_string(), path + "/" + std::to_string(i));
                }
            }
        }
    });
}

// Each value of an entry's Paths fields, with its path inside the entry.
template <typename Visit>
void for_each_link(const Kind& kind, const json::Value& entry, Visit&& visit) {
    for_each_field(kind, [&](const Field& field) {
        const json::Value* value = entry.find(field.name);
        if (field.type != Paths || value == nullptr || !value->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            if (const json::Value& item = value->as_array()[i]; item.is_string()) {
                visit(item.as_string(), "/" + std::string(field.name) + "/" + std::to_string(i));
            }
        }
    });
}

bool is_word_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || byte >= 0x80;
}

// Whether lowered text has word as a whole word.
bool has_word(std::string_view text, std::string_view word) {
    if (word.empty()) {
        return false;
    }
    for (std::size_t at = text.find(word); at != std::string_view::npos;
         at = text.find(word, at + 1)) {
        const std::size_t end = at + word.size();
        if ((at == 0 || !is_word_char(text[at - 1])) &&
            (end == text.size() || !is_word_char(text[end]))) {
            return true;
        }
    }
    return false;
}

std::string text_of(const json::Value& entry, std::string_view field) {
    const json::Value* value = entry.find(field);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

std::string pad(std::int64_t value, std::size_t width) {
    std::string text = std::to_string(value);
    if (text.size() < width) {
        text.insert(0, width - text.size(), '0');
    }
    return text;
}

} // namespace

const std::vector<std::string_view>& files() {
    static const std::vector<std::string_view> kFiles = {"style",    "world",     "story", "design",
                                                         "glossary", "decisions", "work"};
    return kFiles;
}

std::string file_path(std::string_view file) {
    return std::string(kFolder) + std::string(file) + ".json";
}

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> kKinds = make_kinds();
    return kKinds;
}

const std::vector<Field>& common_fields() {
    static const std::vector<Field> kFields = {
        {.name = "name", .type = Text, .doc = "What it is called, or a short title."},
        {.name = "summary", .type = Text, .doc = "One line on what it is."},
        {.name = "text", .type = Text, .doc = "Anything longer, in full."},
        {.name = "refs", .type = Refs, .doc = "Other entries it relates to, by ID."},
        {.name = "links",
         .type = Paths,
         .doc = "Scenes, assets and source files it is about, relative to the project folder."},
        {.name = "tags", .type = Texts, .doc = "Labels for filtering."},
    };
    return kFields;
}

const Kind* find_kind(std::string_view name) {
    const auto it = std::ranges::find(kinds(), name, &Kind::name);
    return it != kinds().end() ? &*it : nullptr;
}

const Kind* kind_of(std::string_view id) {
    const std::size_t slash = id.find('/');
    return slash == std::string_view::npos ? nullptr : find_kind(id.substr(0, slash));
}

std::string id_error(std::string_view id) {
    const std::size_t slash = id.find('/');
    if (slash == std::string_view::npos) {
        return "storyboard IDs are a kind and a lowercase name, such as \"character/mira\"; got " +
               quoted(id);
    }
    const std::string_view kind = id.substr(0, slash);
    if (find_kind(kind) == nullptr) {
        std::vector<std::string_view> names;
        for (const Kind& candidate : kinds()) {
            names.push_back(candidate.name);
        }
        std::string message = "unknown kind " + quoted(kind) + " in " + quoted(id);
        if (const std::string_view hint = closest_match(kind, names); !hint.empty()) {
            return message + "; did you mean " + quoted(hint) + "?";
        }
        message += "; kinds are ";
        for (std::size_t i = 0; i < names.size(); ++i) {
            message += (i == 0 ? "" : ", ") + std::string(names[i]);
        }
        return message;
    }
    const std::string_view name = id.substr(slash + 1);
    if (name.empty() || name.size() > kMaxNameLength || !std::ranges::all_of(name, is_name_char)) {
        return "the name after the kind is 1 to 64 lowercase letters, digits, '_' or '-'; got " +
               quoted(id);
    }
    return {};
}

json::Value kind_schema(const Kind& kind) {
    json::Value schema;
    schema["type"] = "object";
    schema["description"] = kind.doc;
    for_each_field(
        kind, [&](const Field& field) { schema["properties"][field.name] = field_schema(field); });
    schema["required"] = json::Array{"name"};
    schema["additionalProperties"] = false;
    return schema;
}

json::Value file_schema() {
    std::string kinds_pattern;
    json::Value by_kind;
    for (const Kind& kind : kinds()) {
        kinds_pattern += (kinds_pattern.empty() ? "" : "|") + std::string(kind.name);
        by_kind["^" + std::string(kind.name) + "/"] = kind_schema(kind);
    }
    json::Value entries;
    entries["type"] = "object";
    entries["description"] = "Entries by ID: a kind and a lowercase name, such as "
                             "\"character/mira\". Each kind's entries live in one file.";
    entries["propertyNames"]["pattern"] = "^(" + kinds_pattern + ")/[a-z0-9_-]{1,64}$";
    entries["patternProperties"] = std::move(by_kind);
    entries["additionalProperties"] = false;

    json::Value format;
    format["const"] = kFormat;
    format["description"] = "Format version.";

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine storyboard file";
    schema["type"] = "object";
    schema["properties"]["entries"] = std::move(entries);
    schema["properties"]["format"] = std::move(format);
    schema["required"] = json::Array{"entries", "format"};
    schema["additionalProperties"] = false;
    return schema;
}

void validate_entry(const Kind& kind, const json::Value& entry, std::string_view pointer,
                    std::vector<json::ValidationError>& errors) {
    static const std::map<std::string_view, json::Value> kSchemas = [] {
        std::map<std::string_view, json::Value> schemas;
        for (const Kind& each : kinds()) {
            schemas.emplace(each.name, kind_schema(each));
        }
        return schemas;
    }();
    for (json::ValidationError& error : json::validate(entry, kSchemas.at(kind.name))) {
        errors.push_back({std::string(pointer) + error.path, std::move(error.message)});
    }
    if (!entry.is_object()) {
        return;
    }
    for_each_field(kind, [&](const Field& field) {
        const json::Value* value = entry.find(field.name);
        if (value == nullptr) {
            return;
        }
        const std::string path = std::string(pointer) + "/" + std::string(field.name);
        const auto check_id = [&](const json::Value& item, const std::string& at) {
            if (item.is_string()) {
                if (std::string message = id_error(item.as_string()); !message.empty()) {
                    errors.push_back({at, std::move(message)});
                }
            }
        };
        if (field.type == Ref) {
            check_id(*value, path);
        } else if ((field.type == Refs || field.type == Paths) && value->is_array()) {
            for (std::size_t i = 0; i < value->size(); ++i) {
                const json::Value& item = value->as_array()[i];
                const std::string at = path + "/" + std::to_string(i);
                if (field.type == Refs) {
                    check_id(item, at);
                } else if (item.is_string() && !is_project_path(item.as_string())) {
                    errors.push_back({at, "links are paths relative to the project folder, "
                                          "separated by '/', with no \".\" or \"..\" parts; got " +
                                              quoted(item.as_string())});
                }
            }
        } else if (field.type == Colors && value->is_object()) {
            for (const auto& [name, color] : value->as_object()) {
                if (color.is_string() && !is_color(color.as_string())) {
                    errors.push_back({path + "/" + json::pointer_escape(name),
                                      R"(colors are written "#rrggbb", e.g. "#87ceeb"; got )" +
                                          quoted(color.as_string())});
                }
            }
        }
    });
}

std::vector<json::ValidationError> validate_file(std::string_view file,
                                                 const json::Value& document) {
    static const json::Value kShape = [] {
        json::Value shape;
        shape["type"] = "object";
        shape["properties"]["entries"]["type"] = "object";
        shape["properties"]["format"]["const"] = kFormat;
        shape["required"] = json::Array{"entries", "format"};
        shape["additionalProperties"] = false;
        return shape;
    }();
    std::vector<json::ValidationError> errors = json::validate(document, kShape);
    const json::Value* entries = document.is_object() ? document.find("entries") : nullptr;
    if (entries == nullptr || !entries->is_object()) {
        return errors;
    }
    for (const auto& [id, entry] : entries->as_object()) {
        const std::string pointer = "/entries/" + json::pointer_escape(id);
        if (std::string message = id_error(id); !message.empty()) {
            errors.push_back({pointer, std::move(message)});
            continue;
        }
        const Kind& kind = *kind_of(id);
        if (kind.file != file) {
            errors.push_back(
                {pointer, std::string(kind.name) + " entries belong in " + file_path(kind.file)});
            continue;
        }
        validate_entry(kind, entry, pointer, errors);
    }
    return errors;
}

std::string format_file(const json::Object& entries) {
    json::Value document;
    document["entries"] = entries;
    document["format"] = kFormat;
    return json::write(document) + "\n";
}

std::string version(const json::Value& entry) {
    // FNV-1a over the compact text, folded to 32 bits.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : json::write(entry, json::Style::Compact)) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32U));
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text(8, '0');
    for (std::size_t i = text.size(); i-- > 0;) {
        text[i] = kDigits[folded & 0xFU];
        folded >>= 4U;
    }
    return text;
}

std::vector<Reference> references(const Kind& kind, const json::Value& entry) {
    std::vector<Reference> out;
    if (!entry.is_object()) {
        return out;
    }
    for_each_field(kind, [&](const Field& field) {
        const json::Value* value = entry.find(field.name);
        if (value == nullptr) {
            return;
        }
        const std::string path = "/" + std::string(field.name);
        if (field.type == Ref && value->is_string()) {
            out.push_back({value->as_string(), path, field.ref_kind});
        } else if (field.type == Refs && value->is_array()) {
            for (std::size_t i = 0; i < value->size(); ++i) {
                const json::Value& item = value->as_array()[i];
                if (item.is_string()) {
                    out.push_back(
                        {item.as_string(), path + "/" + std::to_string(i), field.ref_kind});
                }
            }
        }
    });
    return out;
}

std::string reference_error(const json::Object& entries, const Reference& reference) {
    if (!id_error(reference.target).empty()) {
        return {};
    }
    if (!reference.kind.empty() && kind_of(reference.target)->name != reference.kind) {
        return "expected a " + std::string(reference.kind) + " entry, got " +
               quoted(reference.target);
    }
    if (!entries.contains(reference.target)) {
        return unknown_entry(entries, reference.target);
    }
    return {};
}

std::string unknown_entry(const json::Object& entries, std::string_view id) {
    std::vector<std::string_view> ids;
    ids.reserve(entries.size());
    for (const auto& [existing, unused] : entries) {
        ids.emplace_back(existing);
    }
    std::string message = "unknown storyboard entry " + quoted(id);
    if (const std::string_view hint = closest_match(id, ids); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return message;
}

Result<Loaded> load(const Reader& read) {
    Loaded loaded;
    for (const std::string_view file : files()) {
        const std::string path = file_path(file);
        auto text = read(path);
        if (!text) {
            return text.error();
        }
        if (!text.value()) {
            continue;
        }
        auto parsed = json::parse(*text.value());
        if (!parsed) {
            loaded.errors.push_back({path, "", parsed.error().message});
            continue;
        }
        const json::Value document = std::move(parsed).value();
        for (json::ValidationError& error : validate_file(file, document)) {
            loaded.errors.push_back({path, std::move(error.path), std::move(error.message)});
        }
        const json::Value* entries = document.is_object() ? document.find("entries") : nullptr;
        if (entries == nullptr || !entries->is_object()) {
            continue;
        }
        for (const auto& [id, entry] : entries->as_object()) {
            if (const Kind* kind = kind_of(id);
                kind != nullptr && kind->file == file && id_error(id).empty()) {
                loaded.entries.emplace(id, entry);
            }
        }
    }
    return loaded;
}

EditResult apply_edits(json::Object& entries, const json::Value& edits) {
    EditResult result;
    const auto fail = [&](std::string path, std::string message) {
        result.errors.push_back({std::move(path), std::move(message)});
    };
    json::Object next = entries;
    std::set<std::string, std::less<>> edited;
    struct Done {
        std::string base;
        std::string id;
        const json::Value* set = nullptr; // nullptr for a removal
    };
    std::vector<Done> done;

    const json::Array& list = edits.as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const json::Value& edit = list[i];
        const std::string base = "/edits/" + std::to_string(i);
        const std::string& id = edit.find("id")->as_string();
        const json::Value* read = edit.find("version");
        const json::Value* set = edit.find("set");
        const json::Value* remove_flag = edit.find("remove");
        const bool remove = remove_flag != nullptr && remove_flag->as_bool();
        if (std::string message = id_error(id); !message.empty()) {
            fail(base + "/id", std::move(message));
            continue;
        }
        if (!edited.insert(id).second) {
            fail(base + "/id", quoted(id) + " is edited twice; edit each entry once per call");
            continue;
        }
        if (remove == (set != nullptr)) {
            fail(base, remove ? "give set or remove, not both" : "give set, or \"remove\": true");
            continue;
        }
        const auto existing = next.find(id);
        const bool exists = existing != next.end();
        if (read == nullptr || read->is_null()) {
            if (exists) {
                fail(base + "/version", quoted(id) + " already exists, at version " +
                                            version(existing->second) +
                                            "; pass that version to change it");
                continue;
            }
            if (remove) {
                fail(base + "/id", unknown_entry(next, id));
                continue;
            }
        } else {
            if (!exists) {
                fail(base + "/id",
                     unknown_entry(next, id) + "; pass \"version\": null to create it");
                continue;
            }
            const std::string now = version(existing->second);
            if (now != read->as_string()) {
                fail(base + "/version", quoted(id) + " changed since version " + read->as_string() +
                                            " was read; it is now " + now +
                                            " (see current), so edit that version");
                if (result.current.is_null()) {
                    result.current["id"] = id;
                    result.current["version"] = now;
                    result.current["fields"] = existing->second;
                }
                continue;
            }
        }
        result.previous[id] = exists ? existing->second : json::Value();
        if (remove) {
            next.erase(existing);
            done.push_back({base, id, nullptr});
            continue;
        }
        json::Value entry = exists ? existing->second : json::Value(json::Object{});
        for (const auto& [field, value] : set->as_object()) {
            if (value.is_null()) {
                entry.as_object().erase(field);
            } else {
                entry[field] = value;
            }
        }
        validate_entry(*kind_of(id), entry, base + "/set", result.errors);
        next[id] = std::move(entry);
        done.push_back({base, id, set});
    }

    for (const Done& item : done) {
        if (item.set != nullptr) {
            // References in the fields this edit sets; removals below catch the rest.
            for (const Reference& reference : references(*kind_of(item.id), next.at(item.id))) {
                const std::string field = reference.path.substr(1, reference.path.find('/', 1) - 1);
                if (item.set->find(field) == nullptr) {
                    continue;
                }
                if (std::string message = reference_error(next, reference); !message.empty()) {
                    fail(item.base + "/set" + reference.path, std::move(message));
                }
            }
            continue;
        }
        for (const auto& [other, entry] : next) {
            const Kind* kind = kind_of(other);
            const std::vector<Reference> named =
                kind != nullptr ? references(*kind, entry) : std::vector<Reference>{};
            const auto it = std::ranges::find(named, item.id, &Reference::target);
            if (it != named.end()) {
                fail(item.base + "/remove", quoted(item.id) + " is still named by " +
                                                quoted(other) + " at " + it->path +
                                                "; change that entry in the same call");
                break;
            }
        }
    }

    if (result.errors.empty()) {
        entries = std::move(next);
    }
    return result;
}

json::Array reverse_edits(const json::Object& entries, const json::Object& previous) {
    json::Array out;
    for (const auto& [id, before] : previous) {
        const auto it = entries.find(id);
        json::Value edit;
        edit["id"] = id;
        if (before.is_null()) {
            edit["version"] = version(it->second);
            edit["remove"] = true;
        } else if (it == entries.end()) {
            edit["version"] = nullptr;
            edit["set"] = before;
        } else {
            edit["version"] = version(it->second);
            json::Value set = json::Object{};
            for (const auto& [field, value] : it->second.as_object()) {
                if (before.find(field) == nullptr) {
                    set[field] = nullptr;
                }
            }
            for (const auto& [field, value] : before.as_object()) {
                const json::Value* now = it->second.find(field);
                if (now == nullptr || !(*now == value)) {
                    set[field] = value;
                }
            }
            edit["set"] = std::move(set);
        }
        out.push_back(std::move(edit));
    }
    return out;
}

json::Array query(const json::Object& entries, const Query& query) {
    const std::string needle = lowered(query.text);
    json::Array out;
    for (const auto& [id, entry] : entries) {
        const Kind* kind = kind_of(id);
        if (kind == nullptr ||
            (!query.ids.empty() && std::ranges::find(query.ids, id) == query.ids.end())) {
            continue;
        }
        if ((!query.kind.empty() && kind->name != query.kind) ||
            (!needle.empty() && !contains_text(entry, needle)) ||
            (!query.linked.empty() && !links_to(*kind, entry, query.linked))) {
            continue;
        }
        out.push_back(entry_item(id, entry, query.full));
    }
    return out;
}

CheckResult check(const json::Object& entries,
                  const std::function<bool(std::string_view path)>& exists) {
    struct Avoided {
        std::string word;    // lowered
        std::string written; // as the term gives it
        std::string term;
        std::string name;
    };
    std::vector<Avoided> avoided;
    for (const auto& [id, entry] : entries) {
        if (const Kind* kind = kind_of(id); kind == nullptr || kind->name != "term") {
            continue;
        }
        if (const json::Value* words = entry.find("avoid"); words != nullptr && words->is_array()) {
            for (const json::Value& word : words->as_array()) {
                if (word.is_string()) {
                    avoided.push_back(
                        {lowered(word.as_string()), word.as_string(), id, text_of(entry, "name")});
                }
            }
        }
    }

    CheckResult result;
    for (const auto& [id, entry] : entries) {
        const Kind* found = kind_of(id);
        if (found == nullptr) {
            continue;
        }
        const Kind& kind = *found;
        const std::string file = file_path(kind.file);
        const std::string pointer = "/entries/" + json::pointer_escape(id);
        for (const Reference& reference : references(kind, entry)) {
            if (std::string message = reference_error(entries, reference); !message.empty()) {
                result.errors.push_back({file, pointer + reference.path, std::move(message)});
            }
        }
        for_each_link(kind, entry, [&](const std::string& link, const std::string& path) {
            if (is_project_path(link) && !exists(link)) {
                result.warnings.push_back(
                    {file, pointer + path, "links to " + quoted(link) + ", which does not exist"});
            }
        });
        // A term may mention the words it warns about.
        if (kind.name == "term") {
            continue;
        }
        for_each_text(kind, entry, [&](const std::string& text, const std::string& path) {
            const std::string lower = lowered(text);
            for (const Avoided& avoid : avoided) {
                if (has_word(lower, avoid.word)) {
                    result.errors.push_back({file, pointer + path,
                                             "uses " + quoted(avoid.written) + ", which " +
                                                 avoid.term + " says to avoid; write " +
                                                 quoted(avoid.name)});
                }
            }
        });
    }
    return result;
}

std::vector<StoryRefId> story_refs(const json::Value& document) {
    std::vector<StoryRefId> out;
    const auto collect = [&](const json::Value& components, const std::string& pointer) {
        const json::Value* story = components.is_object() ? components.find("StoryRef") : nullptr;
        const json::Value* ids =
            story != nullptr && story->is_object() ? story->find("ids") : nullptr;
        if (ids == nullptr || !ids->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < ids->size(); ++i) {
            if (const json::Value& id = ids->as_array()[i]; id.is_string()) {
                out.push_back(
                    {id.as_string(), pointer + "/components/StoryRef/ids/" + std::to_string(i)});
            }
        }
    };
    if (!document.is_object()) {
        return out;
    }
    if (const json::Value* components = document.find("components"); components != nullptr) {
        collect(*components, "");
    }
    if (const json::Value* entities = document.find("entities");
        entities != nullptr && entities->is_object()) {
        for (const auto& [id, entity] : entities->as_object()) {
            if (const json::Value* components =
                    entity.is_object() ? entity.find("components") : nullptr;
                components != nullptr) {
                collect(*components, "/entities/" + json::pointer_escape(id));
            }
        }
    }
    return out;
}

void check_story_refs(const json::Object& entries, std::string_view file,
                      const json::Value& document, std::vector<FileError>& errors) {
    for (const StoryRefId& ref : story_refs(document)) {
        if (entries.contains(ref.id)) {
            continue;
        }
        std::string message = id_error(ref.id);
        errors.push_back({std::string(file), ref.path,
                          message.empty() ? unknown_entry(entries, ref.id) : std::move(message)});
    }
}

void check_textures(const json::Object& entries, const std::vector<std::string>& textures,
                    const std::function<std::optional<Texture>(std::string_view path)>& load,
                    std::vector<FileError>& errors) {
    struct Color {
        std::string name;
        std::array<int, 3> rgb{};
    };
    struct Palette {
        std::string id;
        std::vector<Color> colors;
        int tolerance = 0;
    };
    struct Style {
        std::string id;
        const json::Value* entry;
        bool everywhere;
    };
    std::vector<Style> palettes;
    std::vector<Style> budgets;
    for (const auto& [id, entry] : entries) {
        const Kind* kind = kind_of(id);
        if (kind == nullptr || (kind->name != "palette" && kind->name != "budget")) {
            continue;
        }
        // A palette checks only what it links: painted art, normal maps and light textures rarely
        // keep to one.
        const bool palette = kind->name == "palette";
        const bool everywhere =
            !palette && entry.find("links") == nullptr && entry.find("refs") == nullptr;
        (palette ? palettes : budgets).push_back({id, &entry, everywhere});
    }
    const auto applies = [](const Style& style, const std::string& path) {
        if (style.everywhere) {
            return true;
        }
        const json::Value* links = style.entry->find("links");
        return links != nullptr && links->is_array() &&
               std::ranges::any_of(links->as_array(), [&](const json::Value& link) {
                   return link.is_string() && covers(link.as_string(), path);
               });
    };
    const auto hex = [](const std::string& text, std::size_t at) {
        const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        return (digit(text[at]) * 16) + digit(text[at + 1]);
    };
    const auto integer = [](const json::Value& entry, std::string_view field) {
        const json::Value* value = entry.find(field);
        return value != nullptr && value->is_int() ? value->as_int() : std::int64_t{0};
    };

    for (const std::string& path : textures) {
        std::vector<const Style*> limits;
        std::vector<Palette> covering;
        for (const Style& budget : budgets) {
            if (integer(*budget.entry, "texture_size") > 0 && applies(budget, path)) {
                limits.push_back(&budget);
            }
        }
        for (const Style& palette : palettes) {
            const json::Value* colors = palette.entry->find("colors");
            if (colors == nullptr || !colors->is_object() || !applies(palette, path)) {
                continue;
            }
            Palette& used = covering.emplace_back();
            used.id = palette.id;
            used.tolerance = static_cast<int>(
                std::clamp<std::int64_t>(integer(*palette.entry, "tolerance"), 0, 255));
            for (const auto& [name, color] : colors->as_object()) {
                if (color.is_string() && is_color(color.as_string())) {
                    const std::string& text = color.as_string();
                    used.colors.push_back({name, {hex(text, 1), hex(text, 3), hex(text, 5)}});
                }
            }
        }
        if (limits.empty() && covering.empty()) {
            continue;
        }
        const std::optional<Texture> texture = load(path);
        if (!texture) {
            continue;
        }
        for (const Style* budget : limits) {
            const std::int64_t most = integer(*budget->entry, "texture_size");
            if (texture->width > most || texture->height > most) {
                errors.push_back({path, "",
                                  "is " + std::to_string(texture->width) + "x" +
                                      std::to_string(texture->height) + " pixels; " + budget->id +
                                      " allows " + std::to_string(most) + " on a side"});
            }
        }
        if (covering.empty()) {
            continue;
        }
        // Pixels off every covering palette, by color, with where each color is first seen.
        struct Off {
            std::uint32_t x = 0;
            std::uint32_t y = 0;
        };
        std::unordered_map<std::uint32_t, Off> off;
        std::size_t off_pixels = 0;
        std::unordered_map<std::uint32_t, bool> seen; // whether each color seen is in a palette
        const std::size_t count = std::size_t{texture->width} * texture->height;
        for (std::size_t i = 0; i < count && (i * 4) + 3 < texture->pixels.size(); ++i) {
            const std::uint8_t* pixel = &texture->pixels[i * 4];
            if (pixel[3] == 0) {
                continue;
            }
            const std::uint32_t key =
                (std::uint32_t{pixel[0]} << 16U) | (std::uint32_t{pixel[1]} << 8U) | pixel[2];
            auto known = seen.find(key);
            if (known == seen.end()) {
                const std::array<int, 3> rgb = {pixel[0], pixel[1], pixel[2]};
                const bool matches = std::ranges::any_of(covering, [&](const Palette& palette) {
                    return std::ranges::any_of(palette.colors, [&](const Color& color) {
                        return std::abs(color.rgb[0] - rgb[0]) <= palette.tolerance &&
                               std::abs(color.rgb[1] - rgb[1]) <= palette.tolerance &&
                               std::abs(color.rgb[2] - rgb[2]) <= palette.tolerance;
                    });
                });
                known = seen.emplace(key, matches).first;
            }
            if (known->second) {
                continue;
            }
            ++off_pixels;
            off.try_emplace(key, Off{static_cast<std::uint32_t>(i % texture->width),
                                     static_cast<std::uint32_t>(i / texture->width)});
        }
        if (off.empty()) {
            continue;
        }
        std::string names;
        for (std::size_t i = 0; i < covering.size(); ++i) {
            if (i > 0) {
                names += i + 1 == covering.size() ? " or " : ", ";
            }
            names += covering[i].id;
        }
        // The first few colors in the order they appear, each with the nearest palette color.
        std::vector<std::pair<std::uint32_t, Off>> first(off.begin(), off.end());
        std::ranges::sort(first, [](const auto& a, const auto& b) {
            return std::pair(a.second.y, a.second.x) < std::pair(b.second.y, b.second.x);
        });
        static constexpr std::string_view kHex = "0123456789abcdef";
        const auto written = [](const std::array<int, 3>& rgb) {
            std::string text = "#";
            for (const int channel : rgb) {
                text += kHex[static_cast<std::size_t>(channel) >> 4U];
                text += kHex[static_cast<std::size_t>(channel) & 0xFU];
            }
            return text;
        };
        std::string message = "has " + std::to_string(off_pixels) +
                              (off_pixels == 1 ? " pixel" : " pixels") + " in " +
                              std::to_string(off.size()) +
                              (off.size() == 1 ? " color" : " colors") + " not in " + names + ": ";
        constexpr std::size_t kShown = 3;
        for (std::size_t i = 0; i < first.size() && i < kShown; ++i) {
            const auto [key, at] = first[i];
            const std::array<int, 3> rgb = {static_cast<int>(key >> 16U),
                                            static_cast<int>((key >> 8U) & 0xFFU),
                                            static_cast<int>(key & 0xFFU)};
            const Color* nearest = nullptr;
            int best = 0;
            for (const Palette& palette : covering) {
                for (const Color& color : palette.colors) {
                    int distance = 0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        distance += (color.rgb[c] - rgb[c]) * (color.rgb[c] - rgb[c]);
                    }
                    if (nearest == nullptr || distance < best) {
                        nearest = &color;
                        best = distance;
                    }
                }
            }
            message += (i == 0 ? "" : "; ") + written(rgb) + " at (" + std::to_string(at.x) + ", " +
                       std::to_string(at.y) + ")";
            if (nearest != nullptr) {
                message += ", nearest " + quoted(nearest->name) + " " + written(nearest->rgb);
            }
        }
        if (first.size() > kShown) {
            message += "; and " + std::to_string(first.size() - kShown) + " more";
        }
        errors.push_back({path, "", std::move(message)});
    }
}

Brief brief(const json::Object& entries, const std::vector<std::string>& ids,
            const std::vector<std::string>& paths) {
    const auto links_any = [&](const Kind& kind, const json::Value& entry) {
        bool found = false;
        for_each_link(kind, entry, [&](const std::string& link, const std::string& /*path*/) {
            found = found || std::ranges::any_of(paths, [&](const std::string& path) {
                        return covers(link, path);
                    });
        });
        return found;
    };
    std::set<std::string, std::less<>> chosen;
    for (const std::string& id : ids) {
        if (entries.contains(id) && kind_of(id) != nullptr) {
            chosen.insert(id);
        }
    }
    constexpr std::array<std::string_view, 4> kEverywhere = {"rule", "tone", "palette", "budget"};
    const auto is_work = [](const Kind& kind) {
        return kind.name == "task" || kind.name == "decision";
    };
    for (const auto& [id, entry] : entries) {
        const Kind* kind = kind_of(id);
        if (kind == nullptr || is_work(*kind)) {
            continue;
        }
        const bool global = std::ranges::find(kEverywhere, kind->name) != kEverywhere.end() &&
                            entry.find("links") == nullptr && entry.find("refs") == nullptr;
        if (global || links_any(*kind, entry)) {
            chosen.insert(id);
        }
    }
    // Open work and live decisions about the paths or any of those; finished ones are history.
    const std::set<std::string, std::less<>> direct = chosen;
    for (const auto& [id, entry] : entries) {
        const Kind* kind = kind_of(id);
        if (kind == nullptr || !is_work(*kind) || chosen.contains(id)) {
            continue;
        }
        const std::string status = text_of(entry, "status");
        if (status == (kind->name == "task" ? "done" : "superseded")) {
            continue;
        }
        const std::vector<Reference> named = references(*kind, entry);
        if (links_any(*kind, entry) || std::ranges::any_of(named, [&](const Reference& reference) {
                return direct.contains(reference.target);
            })) {
            chosen.insert(id);
        }
    }
    std::set<std::string, std::less<>> related;
    for (const std::string& id : chosen) {
        for (const Reference& reference : references(*kind_of(id), entries.at(id))) {
            if (!chosen.contains(reference.target) && entries.contains(reference.target)) {
                related.insert(reference.target);
            }
        }
    }
    Brief out;
    for (const std::string& id : chosen) {
        out.entries.push_back(entry_item(id, entries.at(id), true));
    }
    for (const std::string& id : related) {
        out.related.push_back(entry_item(id, entries.at(id), false));
    }
    return out;
}

json::Object changes_since(std::string_view journal, std::size_t seq) {
    json::Object changes;
    std::size_t line_number = 0;
    std::size_t start = 0;
    while (start < journal.size()) {
        const std::size_t end = std::min(journal.find('\n', start), journal.size());
        const std::string_view line = journal.substr(start, end - start);
        start = end + 1;
        if (++line_number <= seq) {
            continue;
        }
        auto parsed = json::parse(line);
        if (!parsed) {
            continue;
        }
        const json::Value entry = std::move(parsed).value();
        if (const json::Value* changed = entry.find("changes");
            changed != nullptr && changed->is_object()) {
            for (const auto& [id, version] : changed->as_object()) {
                changes[id] = version;
            }
        }
    }
    return changes;
}

std::string journal_line(std::string_view by, std::string_view why, const json::Object& changes,
                         std::int64_t unix_seconds) {
    json::Value line;
    line["by"] = by;
    line["changes"] = changes;
    line["time"] = format_time(unix_seconds);
    if (!why.empty()) {
        line["why"] = why;
    }
    return json::write(line, json::Style::Compact);
}

std::size_t journal_length(std::string_view journal) {
    std::size_t lines = static_cast<std::size_t>(std::ranges::count(journal, '\n'));
    if (!journal.empty() && journal.back() != '\n') {
        ++lines;
    }
    return lines;
}

std::string format_time(std::int64_t unix_seconds) {
    constexpr std::int64_t kDay = 86400;
    std::int64_t days = unix_seconds / kDay;
    std::int64_t seconds = unix_seconds % kDay;
    if (seconds < 0) {
        seconds += kDay;
        --days;
    }
    // Civil date from days since 1970-01-01, after Howard Hinnant's algorithm (public domain).
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - (era * 146097);
    const std::int64_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    const std::int64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    const std::int64_t mp = ((5 * doy) + 2) / 153;
    const std::int64_t day = doy - (((153 * mp) + 2) / 5) + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + (era * 400) + (month <= 2 ? 1 : 0);
    return pad(year, 4) + "-" + pad(month, 2) + "-" + pad(day, 2) + "T" + pad(seconds / 3600, 2) +
           ":" + pad((seconds / 60) % 60, 2) + ":" + pad(seconds % 60, 2) + "Z";
}

} // namespace slash::storyboard
