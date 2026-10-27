// l10n.extract: the messages a game shows, as Godot's POT generator collects them
// (editor/translations/pot_generator.cpp, with the scene parser of
// editor/translations/packed_scene_translation_parser_plugin.cpp), written as a POT file and added
// to the project's translation files so no text is missed.
#include "l10n_extract.hpp"

#include "slash/command/cpp_outline.hpp"
#include "slash/command/l10n.hpp"
#include "slash/core/text.hpp"
#include "slash/l10n/locale.hpp"
#include "slash/l10n/translation.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

using Key = std::pair<std::string, std::string>; // context, message

// Components whose "text" a control shows, as Godot's parser takes Label's and BaseButton's.
constexpr std::array<std::string_view, 5> kTextComponents = {"Label", "Button", "CheckBox",
                                                             "CheckButton", "MenuButton"};
// Components whose "items" are a list's options or a menu's items.
constexpr std::array<std::string_view, 3> kItemComponents = {"OptionButton", "MenuButton",
                                                             "PopupMenu"};

class Collector {
public:
    void add(std::string context, std::string message, std::string plural, std::string where) {
        if (message.empty()) {
            return;
        }
        const Key key{std::move(context), std::move(message)};
        auto [it, added] = index_.try_emplace(key, messages_.size());
        if (added) {
            messages_.push_back({key.first, key.second, {}, {}});
        }
        ExtractedMessage& found = messages_[it->second];
        if (found.plural.empty()) {
            found.plural = std::move(plural);
        }
        if (std::ranges::find(found.where, where) == found.where.end()) {
            found.where.push_back(std::move(where));
        }
    }

    std::vector<ExtractedMessage> take() { return std::move(messages_); }

private:
    std::vector<ExtractedMessage> messages_;
    std::map<Key, std::size_t> index_;
};

const json::Value* member(const json::Value* object, std::string_view key) {
    return object != nullptr && object->is_object() ? object->find(key) : nullptr;
}

std::string text_of(const json::Value* value) {
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

// A document's messages: each control's text, placeholders, tooltip and items, as their
// auto_translate_mode lets them translate (Godot's Node::can_auto_translate).
void collect_document(const json::Value& document, const std::string& file, Collector& out) {
    const json::Value* entities = member(&document, "entities");
    json::Value prefab; // a prefab file is one entity's components
    if (entities == nullptr && member(&document, "components") != nullptr) {
        prefab[""] = document;
        entities = &prefab;
    }
    if (entities == nullptr || !entities->is_object()) {
        return;
    }
    std::map<std::string, std::string, std::less<>> parent_of;
    for (const auto& [id, entity] : entities->as_object()) {
        const json::Value* children =
            member(member(member(&entity, "components"), "Control"), "children");
        if (children == nullptr || !children->is_array()) {
            continue;
        }
        for (const json::Value& child : children->as_array()) {
            if (child.is_string()) {
                parent_of.try_emplace(child.as_string(), id);
            }
        }
    }
    const auto mode_of = [&](std::string_view id) {
        const json::Value* entity = entities->find(id);
        const std::string mode =
            text_of(member(member(member(entity, "components"), "Control"), "auto_translate_mode"));
        return mode.empty() ? std::string("inherit") : mode;
    };
    const auto translates = [&](std::string_view id) {
        std::string at(id);
        // A parent cycle is an error scene validate reports; stop where it starts again.
        for (std::size_t steps = 0; steps <= parent_of.size(); ++steps) {
            const std::string mode = mode_of(at);
            if (mode != "inherit") {
                return mode == "always";
            }
            const auto parent = parent_of.find(at);
            if (parent == parent_of.end()) {
                return true;
            }
            at = parent->second;
        }
        return true;
    };
    for (const auto& [id, entity] : entities->as_object()) {
        const json::Value* components = member(&entity, "components");
        if (components == nullptr || !components->is_object()) {
            continue;
        }
        const bool translate = translates(id);
        const json::Value* control = member(components, "Control");
        if (translate) {
            for (const std::string_view type : kTextComponents) {
                out.add({}, text_of(member(member(components, type), "text")), {}, file);
            }
            for (const std::string_view type : {"LineEdit", "TextEdit"}) {
                out.add({}, text_of(member(member(components, type), "placeholder_text")), {},
                        file);
            }
        }
        if (control != nullptr) {
            const std::string tooltip_mode =
                text_of(member(control, "tooltip_auto_translate_mode"));
            const bool tooltip = tooltip_mode.empty() || tooltip_mode == "inherit"
                                     ? translate
                                     : tooltip_mode == "always";
            if (tooltip) {
                out.add({}, text_of(member(control, "tooltip_text")), {}, file);
            }
        }
        for (const std::string_view type : kItemComponents) {
            const json::Value* items = member(member(components, type), "items");
            if (items == nullptr || !items->is_array()) {
                continue;
            }
            // A PopupMenu is a window of its own, so it translates as a root does.
            const bool owner = type == "PopupMenu" && control == nullptr ? true : translate;
            for (const json::Value& item : items->as_array()) {
                const std::string mode = text_of(member(&item, "auto_translate_mode"));
                const bool shown = mode.empty() || mode == "inherit" ? owner : mode == "always";
                if (shown) {
                    out.add({}, text_of(member(&item, "text")), {}, file);
                    out.add({}, text_of(member(&item, "tooltip")), {}, file);
                }
            }
        }
    }
}

// A C++ string literal's value, as the outline gives an argument's tokens; nullopt for anything
// else, such as a variable.
std::optional<std::string> literal_of(std::string_view tokens) {
    std::string out;
    std::size_t i = 0;
    bool any = false;
    while (i < tokens.size()) {
        if (tokens[i] == ' ') {
            ++i;
            continue;
        }
        if (tokens[i] != '"') {
            return std::nullopt;
        }
        any = true;
        for (++i; i < tokens.size() && tokens[i] != '"'; ++i) {
            if (tokens[i] != '\\' || i + 1 == tokens.size()) {
                out += tokens[i];
                continue;
            }
            const char next = tokens[++i];
            if (next == 'n') {
                out += '\n';
            } else if (next == 't') {
                out += '\t';
            } else {
                out += next;
            }
        }
        ++i;
    }
    if (!any) {
        return std::nullopt;
    }
    return out;
}

// The tr and tr_n calls in a source file, as l10n::tr(world, message, context) and
// l10n::tr_n(world, message, plural, n, context).
void collect_source(const CppOutline& outline, const std::string& file, Collector& out) {
    for (const CppString& literal : outline.strings) {
        const bool plural = literal.callee == "tr_n";
        if ((literal.callee != "tr" && !plural) || literal.arg != 1) {
            continue;
        }
        const std::size_t context_arg = plural ? 4 : 2;
        std::string context;
        if (literal.args.size() > context_arg) {
            context = literal_of(literal.args[context_arg]).value_or("");
        }
        std::string plural_text;
        if (plural && literal.args.size() > 2) {
            plural_text = literal_of(literal.args[2]).value_or("");
        }
        out.add(std::move(context), literal.value, std::move(plural_text),
                file + ":" + std::to_string(literal.line));
    }
}

bool source_file(std::string_view path) {
    constexpr std::array<std::string_view, 7> kSuffixes = {".cpp", ".hpp", ".h",  ".cc",
                                                           ".cxx", ".hh",  ".inl"};
    return std::ranges::any_of(kSuffixes,
                               [&](std::string_view suffix) { return path.ends_with(suffix); });
}

// A project file's text, or nullopt when there is none or it can't be read.
std::optional<std::string> read_text(Session& session, std::string_view path) {
    auto text = session.read_project_file(path);
    if (!text) {
        return std::nullopt;
    }
    return std::move(text).value();
}

json::Value read_project(Session& session) {
    const std::optional<std::string> text = read_text(session, "project.json");
    if (!text) {
        return {};
    }
    auto parsed = json::parse(*text);
    return parsed ? std::move(parsed).value() : json::Value();
}

bool inside(std::string_view path, std::string_view folder) {
    if (folder.ends_with('/')) {
        folder.remove_suffix(1);
    }
    return path == folder ||
           (path.starts_with(folder) && path.size() > folder.size() && path[folder.size()] == '/');
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// What a translation file lacks of the messages, added to its end with nothing translated: a row
// per message in a CSV file (its ?context and ?plural columns filled), an entry per message in a
// .po file (as many msgstr[n] as its rule has forms). Messages a CSV file has no column for
// (a context, a plural) are left out and counted in skipped.
std::string with_messages(std::string_view path, std::string text,
                          const l10n::TranslationFile& file,
                          const std::vector<const ExtractedMessage*>& missing, int& skipped) {
    if (!text.empty() && !text.ends_with('\n')) {
        text += '\n';
    }
    if (path.ends_with(".po")) {
        int forms = 2;
        if (!file.translations.empty()) {
            forms = file.translations.front().plural_rules().nplurals();
        }
        for (const ExtractedMessage* message : missing) {
            text += '\n';
            if (!message->context.empty()) {
                text += "msgctxt \"" + l10n::po_escape(message->context) + "\"\n";
            }
            text += l10n::po_string("msgid", message->message);
            if (message->plural.empty()) {
                text += "msgstr \"\"\n";
                continue;
            }
            text += l10n::po_string("msgid_plural", message->plural);
            for (int i = 0; i < forms; ++i) {
                text += "msgstr[" + std::to_string(i) + "] \"\"\n";
            }
        }
        return text;
    }
    const char delimiter = ',';
    const std::vector<l10n::CsvRow> rows = l10n::csv_rows(text, delimiter);
    const std::vector<std::string> header =
        rows.empty() ? std::vector<std::string>{""} : rows.front().fields;
    std::optional<std::size_t> context_column;
    std::optional<std::size_t> plural_column;
    for (std::size_t i = 1; i < header.size(); ++i) {
        const std::string name = to_lower(header[i]);
        if (name == "?context" && !context_column) {
            context_column = i;
        } else if (name == "?plural" && !plural_column) {
            plural_column = i;
        }
    }
    for (const ExtractedMessage* message : missing) {
        if ((!message->context.empty() && !context_column) ||
            (!message->plural.empty() && !plural_column)) {
            ++skipped;
            continue;
        }
        std::vector<std::string> row(std::max<std::size_t>(header.size(), 1));
        row[0] = message->message;
        if (context_column) {
            row[*context_column] = message->context;
        }
        if (plural_column) {
            row[*plural_column] = message->plural;
        }
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) {
                line += delimiter;
            }
            line += l10n::csv_field(row[i], delimiter);
        }
        text += line + '\n';
    }
    return text;
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply l10n_extract(Session& session, const json::Value& params) {
    const std::vector<ExtractedMessage> messages = extract_messages(session);
    const json::Value project = read_project(session);
    std::vector<std::string> translations;
    if (const json::Value* list =
            member(member(member(&project, "internationalization"), "locale"), "translations");
        list != nullptr && list->is_array()) {
        for (const json::Value& file : list->as_array()) {
            if (file.is_string()) {
                translations.push_back(file.as_string());
            }
        }
    }

    json::Value out;
    out["messages"] = messages.size();
    if (const json::Value* list = params.find("list"); list != nullptr && list->as_bool()) {
        json::Array items;
        for (const ExtractedMessage& message : messages) {
            json::Value item;
            item["message"] = message.message;
            if (!message.context.empty()) {
                item["context"] = message.context;
            }
            if (!message.plural.empty()) {
                item["plural"] = message.plural;
            }
            json::Array where;
            for (const std::string& place : message.where) {
                where.emplace_back(place);
            }
            item["where"] = std::move(where);
            items.push_back(std::move(item));
        }
        out["list"] = std::move(items);
    }

    bool changed = false;
    // The POT file.
    if (const json::Value* pot = params.find("pot"); pot != nullptr) {
        const std::string& path = pot->as_string();
        if (!path.ends_with(".pot")) {
            json::Value data;
            data["path"] = "/pot";
            return Reply::failure(code::kInvalidParams, "/pot: expected a .pot file",
                                  std::move(data));
        }
        std::vector<std::string> files;
        for (const ExtractedMessage& message : messages) {
            for (const std::string& place : message.where) {
                const std::string file = place.substr(0, place.find(':'));
                if (std::ranges::find(files, file) == files.end()) {
                    files.push_back(file);
                }
            }
        }
        std::ranges::sort(files);
        const std::string name = text_of(project.find("name"));
        auto before = session.read_project_file(path);
        if (!before) {
            return Reply::failure(before.error());
        }
        std::string after = write_pot(name, files, messages);
        if (before.value() != after) {
            if (auto staged = session.stage_file(path, after); !staged) {
                return Reply::failure(staged.error());
            }
            session.add_undo_file(path, std::move(before.value()), std::move(after));
            changed = true;
        }
        out["pot"] = path;
    }

    // The messages each translation file lacks, added to it.
    if (const json::Value* add = params.find("add_to"); add != nullptr) {
        json::Object added;
        const json::Array& files = add->as_array();
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string& path = files[i].as_string();
            const std::string at = "/add_to/" + std::to_string(i);
            json::Value data;
            data["path"] = at;
            if (std::ranges::find(translations, path) == translations.end()) {
                std::string message = at;
                message += ": project.json's internationalization lists no \"";
                message += path;
                message += '"';
                if (const std::string_view near = closest_match(path, translations);
                    !near.empty()) {
                    message += "; did you mean \"" + std::string(near) + "\"?";
                }
                return Reply::failure(code::kInvalidParams, std::move(message), std::move(data));
            }
            if (path.ends_with(".mo")) {
                return Reply::failure(code::kInvalidParams,
                                      at + ": a .mo file is compiled; add to its .po file",
                                      std::move(data));
            }
            auto before = session.read_project_file(path);
            if (!before) {
                return Reply::failure(before.error());
            }
            const std::string text = before.value().value_or("");
            auto read = l10n::read_translation_file(path, text);
            if (!read) {
                return Reply::failure(code::kInvalidParams, at + ": " + read.error().message,
                                      std::move(data));
            }
            std::set<Key> known;
            for (const l10n::Translation& translation : read.value().translations) {
                for (const auto& [key, forms] : translation.messages()) {
                    known.insert(key);
                }
            }
            std::vector<const ExtractedMessage*> missing;
            for (const ExtractedMessage& message : messages) {
                if (!known.contains({message.context, message.message})) {
                    missing.push_back(&message);
                }
            }
            int skipped = 0;
            std::string after = with_messages(path, text, read.value(), missing, skipped);
            json::Value entry;
            entry["added"] = static_cast<std::int64_t>(missing.size()) - skipped;
            if (skipped > 0) {
                entry["skipped"] = skipped;
            }
            added.emplace(path, std::move(entry));
            if (after != text) {
                if (auto staged = session.stage_file(path, after); !staged) {
                    return Reply::failure(staged.error());
                }
                session.add_undo_file(path, std::move(before.value()), std::move(after));
                changed = true;
            }
        }
        out["added"] = std::move(added);
    }
    if (!changed) {
        session.mark_unchanged();
    }
    return Reply::success(std::move(out));
}

} // namespace

std::vector<ExtractedMessage> extract_messages(Session& session) {
    Collector out;
    auto files = platform::list_files(session.options().project_root,
                                      [](std::string_view folder) { return folder != "build/"; });
    if (!files) {
        return {};
    }
    const std::string& open = session.scene_path();
    for (const std::string& file : files.value()) {
        if (!file.ends_with(world::kSceneSuffix) && !file.ends_with(world::kPrefabSuffix)) {
            continue;
        }
        json::Value document;
        if (file == open) {
            document = session.save().scene; // as it is now, unsaved edits included
        } else {
            const std::optional<std::string> text = read_text(session, file);
            if (!text) {
                continue;
            }
            auto parsed = json::parse(*text);
            if (!parsed) {
                continue; // scene validate reports it
            }
            document = std::move(parsed).value();
        }
        collect_document(document, file, out);
    }
    // The game's C++ sources.
    const json::Value project = read_project(session);
    const json::Value* sources = member(member(&project, "gameplay"), "sources");
    if (sources != nullptr && sources->is_array()) {
        for (const std::string& file : files.value()) {
            const bool listed =
                std::ranges::any_of(sources->as_array(), [&](const json::Value& root) {
                    return root.is_string() && inside(file, root.as_string());
                });
            if (!listed || !source_file(file)) {
                continue;
            }
            const std::optional<std::string> text = read_text(session, file);
            if (!text) {
                continue;
            }
            collect_source(outline_cpp(*text), file, out);
        }
    }
    return out.take();
}

std::string write_pot(std::string_view project_name, const std::vector<std::string>& files,
                      const std::vector<ExtractedMessage>& messages) {
    std::string out =
        "# LANGUAGE translation for " + std::string(project_name) + " for the following files:\n";
    for (const std::string& file : files) {
        out += "# " + file + "\n";
    }
    out += "#\n"
           "# FIRST AUTHOR <EMAIL@ADDRESS>, YEAR.\n"
           "#\n"
           "#, fuzzy\n"
           "msgid \"\"\n"
           "msgstr \"\"\n"
           "\"Project-Id-Version: " +
           l10n::po_escape(project_name) +
           "\\n\"\n"
           "\"MIME-Version: 1.0\\n\"\n"
           "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
           "\"Content-Transfer-Encoding: 8-bit\\n\"\n";
    for (const ExtractedMessage& message : messages) {
        out += '\n';
        for (const std::string& place : message.where) {
            out += "#: " + place + "\n";
        }
        if (!message.context.empty()) {
            out += "msgctxt \"" + l10n::po_escape(message.context) + "\"\n";
        }
        out += l10n::po_string("msgid", message.message);
        if (message.plural.empty()) {
            out += "msgstr \"\"\n";
        } else {
            out += l10n::po_string("msgid_plural", message.plural);
            out += "msgstr[0] \"\"\nmsgstr[1] \"\"\n";
        }
    }
    return out;
}

void register_l10n_extract(Registry& registry) {
    using enum Capability;
    json::Value pot = property("string", "A .pot file to write every message to, as Godot's "
                                         "Generate POT.");
    pot["minLength"] = 1;
    json::Value add = property("array", "Translation files project.json lists (.csv or .po) to "
                                        "add the messages they lack to, untranslated.");
    add["items"]["type"] = "string";
    add["items"]["minLength"] = 1;
    add["maxItems"] = 64;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"list", property("boolean", "Return each message with where it is. Default false.")},
        {"pot", std::move(pot)},
        {"add_to", std::move(add)},
    };
    registry.add({
        .name = "l10n.extract",
        .description = "Collects the messages the game shows: controls' text, placeholders, "
                       "tooltips and items in scenes and prefabs (as auto_translate_mode lets "
                       "them), and tr and tr_n calls in its C++. Returns the count; writes a POT "
                       "file or adds the missing messages to translation files.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = std::move(schema),
        .handler = l10n_extract,
    });
}

} // namespace slash::command
