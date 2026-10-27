// l10n.translate: a model from providers.json drafts the translations a locale lacks, told the
// storyboard's glossary, characters and tone so names and voices stay the same. Drafts are
// checked (their forms, placeholders and the glossary's words to avoid) before they are written,
// and a .po file marks each one fuzzy, gettext's mark for a translation to review, which Godot
// and the game leave out until a person or agent clears it.
#include "commands_internal.hpp"

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/command/l10n.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/text.hpp"
#include "slash/l10n/l10n.hpp"
#include "slash/l10n/locale.hpp"
#include "slash/l10n/translation.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai::detail {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

using Key = std::pair<std::string, std::string>; // context, message

constexpr std::int64_t kDefaultMax = 40;
constexpr std::int64_t kMaxMax = 200;
constexpr std::size_t kMaxExamples = 20;

constexpr std::string_view kInstructions =
    "You translate a game's interface text and dialogue. Translate each numbered message into "
    "the target language as a native speaker writing for this game would: short where the "
    "source is short, in the tone and each character's voice the storyboard gives. Use the "
    "glossary's names exactly and never its words to avoid. Keep every placeholder exactly as "
    "written: printf conversions such as %d, %s and %.2f in the same order, and {name} fields. "
    "Keep line breaks. A message with a plural gets one translation per plural form, in the "
    "order the plural rule numbers them; otherwise give one.";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply failure_at(int code, const std::string& path, std::string message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code, path + ": " + std::move(message), std::move(data));
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool is_word_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || byte >= 0x80;
}

// Whether lowered text has word as a whole word, as storyboard.check matches words to avoid.
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

// A message to draft: its source and how many forms it takes.
struct Pending {
    Key key;
    std::string plural; // msgid_plural, or ""
};

// A checked draft.
struct Draft {
    Key key;
    std::vector<std::string> forms;
    bool plural = false;
};

// The text inside a .po line's quotes, unescaped as TranslationLoaderPO unescapes it.
std::string po_unquote(std::string_view line) {
    const std::size_t open = line.find('"');
    const std::size_t close = line.rfind('"');
    if (open == std::string_view::npos || close <= open) {
        return {};
    }
    std::string out;
    for (std::size_t i = open + 1; i < close; ++i) {
        if (line[i] != '\\' || i + 1 >= close) {
            out += line[i];
            continue;
        }
        const char next = line[++i];
        if (next == 'n') {
            out += '\n';
        } else if (next == 't') {
            out += '\t';
        } else {
            out += next;
        }
    }
    return out;
}

// One entry of a .po file: its lines, what it says and where its msgstr lines start.
struct PoEntry {
    std::vector<std::string_view> lines;
    Key key;
    bool fuzzy = false;
    bool translated = false; // a msgstr has text
    std::size_t first_msgstr = 0;
};

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        out.push_back(line);
        start = end + 1;
    }
    return out;
}

bool blank(std::string_view line) {
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

// A .po file's entries, split at blank lines as gettext writes them; the lines between are
// kept as they are.
std::vector<PoEntry> po_entries(std::string_view text) {
    std::vector<PoEntry> out;
    PoEntry entry;
    std::string* reading = nullptr;
    std::string context;
    std::string id;
    std::string str;
    const auto finish = [&] {
        if (!entry.lines.empty()) {
            entry.key = {context, id};
            entry.translated = !str.empty();
            if (entry.first_msgstr == 0) {
                entry.first_msgstr = entry.lines.size();
            }
            out.push_back(std::move(entry));
        }
        entry = PoEntry{};
        reading = nullptr;
        context.clear();
        id.clear();
        str.clear();
    };
    for (const std::string_view line : lines_of(text)) {
        if (blank(line)) {
            finish();
            continue;
        }
        if (line.starts_with("#,") && line.find("fuzzy") != std::string_view::npos) {
            entry.fuzzy = true;
        }
        if (line.starts_with("msgctxt")) {
            reading = &context;
        } else if (line.starts_with("msgid_plural")) {
            reading = nullptr; // the plural's text isn't the key
        } else if (line.starts_with("msgid")) {
            reading = &id;
        } else if (line.starts_with("msgstr")) {
            if (entry.first_msgstr == 0) {
                entry.first_msgstr = entry.lines.size();
            }
            reading = &str;
        }
        if (line.starts_with('"') || line.starts_with("msg")) {
            if (reading != nullptr) {
                *reading += po_unquote(line);
            }
        }
        entry.lines.push_back(line);
    }
    finish();
    return out;
}

std::string po_forms(const Draft& draft) {
    if (!draft.plural) {
        return l10n::po_string("msgstr", draft.forms.front());
    }
    std::string out;
    for (std::size_t i = 0; i < draft.forms.size(); ++i) {
        out += l10n::po_string("msgstr[" + std::to_string(i) + "]", draft.forms[i]);
    }
    return out;
}

// A .po file with the drafts in: an entry with nothing translated gets the draft and the fuzzy
// flag (added to a "#," line it has), and a message the file lacks is added at its end.
std::string po_with_drafts(std::string_view text, const std::vector<Draft>& drafts,
                           const std::map<Key, std::string>& plurals) {
    std::map<Key, const Draft*> by_key;
    for (const Draft& draft : drafts) {
        by_key.emplace(draft.key, &draft);
    }
    std::string out;
    std::set<Key> written;
    const std::vector<PoEntry> entries = po_entries(text);
    for (std::size_t e = 0; e < entries.size(); ++e) {
        const PoEntry& entry = entries[e];
        if (e > 0) {
            out += '\n';
        }
        const auto found = by_key.find(entry.key);
        if (found == by_key.end() || entry.translated || entry.key.second.empty() ||
            written.contains(entry.key)) {
            for (const std::string_view line : entry.lines) {
                out += line;
                out += '\n';
            }
            continue;
        }
        written.insert(entry.key);
        bool flagged = entry.fuzzy;
        for (std::size_t i = 0; i < entry.first_msgstr; ++i) {
            const std::string_view line = entry.lines[i];
            if (line.starts_with("#,") && !flagged) {
                out += "#, fuzzy,";
                out += line.substr(2);
                out += '\n';
                flagged = true;
                continue;
            }
            if (!flagged && !line.starts_with('#')) {
                out += "#, fuzzy\n";
                flagged = true;
            }
            out += line;
            out += '\n';
        }
        out += po_forms(*found->second);
    }
    for (const Draft& draft : drafts) {
        if (written.contains(draft.key)) {
            continue;
        }
        out += "\n#, fuzzy\n";
        if (!draft.key.first.empty()) {
            out += "msgctxt \"" + l10n::po_escape(draft.key.first) + "\"\n";
        }
        out += l10n::po_string("msgid", draft.key.second);
        if (draft.plural) {
            const auto plural = plurals.find(draft.key);
            out += l10n::po_string("msgid_plural",
                                   plural != plurals.end() ? plural->second : draft.key.second);
        }
        out += po_forms(draft);
    }
    return out;
}

// A CSV file's columns for a locale, its ?context and its ?plural.
struct CsvColumns {
    std::optional<std::size_t> locale;
    std::optional<std::size_t> context;
    std::optional<std::size_t> plural;
    std::size_t count = 1;
};

CsvColumns csv_columns(const std::vector<l10n::CsvRow>& rows, std::string_view locale) {
    CsvColumns out;
    if (rows.empty()) {
        return out;
    }
    const std::vector<std::string>& header = rows.front().fields;
    out.count = header.size();
    for (std::size_t i = 1; i < header.size(); ++i) {
        const std::string name = lower(header[i]);
        if (name == "?context") {
            out.context = out.context.value_or(i);
        } else if (name == "?plural") {
            out.plural = out.plural.value_or(i);
        } else if (!name.starts_with('_') && !name.starts_with('?') && !out.locale &&
                   l10n::standardize_locale(header[i]) == locale) {
            out.locale = i;
        }
    }
    return out;
}

std::string csv_line(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += l10n::csv_field(fields[i]);
    }
    return out;
}

// A CSV file with the drafts in the locale's column: a row whose cell is empty gets its draft,
// and a message with no row gets one at the end. CSV has no mark for a draft, so they go in as
// translations.
std::string csv_with_drafts(std::string_view text, const std::vector<Draft>& drafts,
                            std::string_view locale) {
    const std::vector<l10n::CsvRow> rows = l10n::csv_rows(text);
    const CsvColumns columns = csv_columns(rows, locale);
    std::map<Key, const Draft*> by_key;
    for (const Draft& draft : drafts) {
        by_key.emplace(draft.key, &draft);
    }
    std::string out;
    std::size_t copied = 0;
    std::set<Key> written;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const l10n::CsvRow& row = rows[r];
        if (row.fields.empty() || row.fields.front().empty()) {
            continue;
        }
        Key key{columns.context && *columns.context < row.fields.size()
                    ? row.fields[*columns.context]
                    : std::string(),
                row.fields.front()};
        const auto found = by_key.find(key);
        if (found == by_key.end() || written.contains(key)) {
            continue;
        }
        std::vector<std::string> fields = row.fields;
        fields.resize(std::max(fields.size(), *columns.locale + 1));
        if (!fields[*columns.locale].empty()) {
            continue;
        }
        fields[*columns.locale] = found->second->forms.front();
        out += text.substr(copied, row.begin - copied);
        out += csv_line(fields);
        copied = row.end;
        written.insert(std::move(key));
    }
    out += text.substr(copied);
    for (const Draft& draft : drafts) {
        if (written.contains(draft.key)) {
            continue;
        }
        if (!out.empty() && !out.ends_with('\n')) {
            out += '\n';
        }
        std::vector<std::string> fields(columns.count);
        fields[0] = draft.key.second;
        if (columns.context) {
            fields[*columns.context] = draft.key.first;
        }
        fields[*columns.locale] = draft.forms.front();
        out += csv_line(fields) + '\n';
    }
    return out;
}

json::Value output_schema() {
    json::Value forms = property("array", "The translation; for a plural message, one per form.");
    forms["items"]["type"] = "string";
    forms["minItems"] = 1;
    json::Value item;
    item["type"] = "object";
    item["additionalProperties"] = false;
    item["required"] = json::Array{"item", "forms"};
    item["properties"] = json::Object{
        {"item", property("integer", "The message's number.")},
        {"forms", std::move(forms)},
    };
    json::Value list = property("array", "A translation per message.");
    list["items"] = std::move(item);
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"translations"};
    schema["properties"] = json::Object{{"translations", std::move(list)}};
    return schema;
}

// The storyboard's facts a translator needs: the glossary, the characters and the tone.
json::Array translator_facts(const json::Object& entries, std::vector<std::string>& avoid) {
    json::Array out;
    for (const auto& [id, entry] : entries) {
        const bool term = id.starts_with("term/");
        if (!term && !id.starts_with("character/") && !id.starts_with("tone/")) {
            continue;
        }
        json::Value fact;
        fact["id"] = id;
        for (const char* field : {"name", "summary", "aliases", "avoid", "voice", "traits"}) {
            if (const json::Value* value = entry.find(field); value != nullptr) {
                fact[field] = *value;
            }
        }
        if (const json::Value* words = entry.find("avoid");
            term && words != nullptr && words->is_array()) {
            for (const json::Value& word : words->as_array()) {
                if (word.is_string() && !word.as_string().empty()) {
                    avoid.push_back(word.as_string());
                }
            }
        }
        out.push_back(std::move(fact));
    }
    return out;
}

// Why a draft can't be kept, or "".
std::string draft_problem(const Pending& pending, const std::vector<std::string>& forms,
                          int nplurals, const std::vector<std::string>& avoid) {
    const bool plural = !pending.plural.empty();
    const std::size_t want = plural ? static_cast<std::size_t>(nplurals) : 1;
    if (forms.size() != want) {
        return "gave " + std::to_string(forms.size()) + " forms where " + std::to_string(want) +
               (want == 1 ? " is" : " are") + " needed";
    }
    for (const std::string& form : forms) {
        if (form.empty()) {
            return "gave an empty form";
        }
        if (std::string problem =
                l10n::placeholder_problem(plural ? pending.plural : pending.key.second, form);
            !problem.empty()) {
            return problem;
        }
        const std::string text = lower(form);
        for (const std::string& word : avoid) {
            if (has_word(text, lower(word))) {
                return "uses \"" + word + "\", which the glossary says to avoid";
            }
        }
    }
    return {};
}

Reply l10n_translate(Session& session, const json::Value& params) {
    const l10n::Settings settings = l10n::settings(session.world());
    const std::string locale = l10n::standardize_locale(params.find("locale")->as_string());
    if (!l10n::is_locale(locale)) {
        return failure_at(command::code::kInvalidParams, "/locale",
                          R"(expected a locale such as "fr" or "pt_BR")");
    }

    // The file to write: the one asked for, else the first that has the locale.
    std::string path;
    std::optional<l10n::TranslationFile> file;
    std::string text;
    const auto read = [&](const std::string& each) -> bool {
        auto read_text = session.read_project_file(each);
        if (!read_text || !read_text.value()) {
            return false;
        }
        auto parsed = l10n::read_translation_file(each, *read_text.value());
        if (!parsed) {
            return false;
        }
        text = *read_text.value();
        file = std::move(parsed).value();
        return true;
    };
    const auto has_locale = [&]() {
        return std::ranges::any_of(
            file->translations, [&](const l10n::Translation& t) { return t.locale() == locale; });
    };
    if (const json::Value* asked = params.find("file"); asked != nullptr) {
        path = asked->as_string();
        if (std::ranges::find(settings.translations, path) == settings.translations.end()) {
            std::string message = "project.json's internationalization lists no \"" + path + "\"";
            if (const std::string_view near = closest_match(path, settings.translations);
                !near.empty()) {
                message += "; did you mean \"" + std::string(near) + "\"?";
            }
            return failure_at(command::code::kInvalidParams, "/file", std::move(message));
        }
        if (path.ends_with(".mo")) {
            return failure_at(command::code::kInvalidParams, "/file",
                              "a .mo file is compiled; draft into its .po file");
        }
        if (!read(path)) {
            return failure_at(command::code::kInvalidParams, "/file",
                              "it doesn't read; run l10n.check");
        }
        if (!has_locale()) {
            return failure_at(command::code::kInvalidParams, "/file",
                              path.ends_with(".po")
                                  ? "its Language is another locale's"
                                  : "it has no " + locale + " column; add one to its header");
        }
    } else {
        for (const std::string& each : settings.translations) {
            if (!each.ends_with(".mo") && read(each) && has_locale()) {
                path = each;
                break;
            }
        }
        if (path.empty()) {
            return failure_at(command::code::kInvalidParams, "/locale",
                              "no listed .po or CSV file has " + locale +
                                  "; add a column for it or a .po file with its Language, or "
                                  "pass file");
        }
    }
    const bool po = path.ends_with(".po");
    const l10n::Translation* translation = nullptr;
    for (const l10n::Translation& each : file->translations) {
        if (each.locale() == locale) {
            translation = &each;
        }
    }
    if (translation == nullptr) {
        return failure_at(command::code::kInvalidParams, "/locale", "no file has " + locale);
    }
    const int nplurals = translation->plural_rules().nplurals();

    // What the locale lacks: drafts waiting for review in a .po file count as there.
    std::set<Key> done;
    for (const auto& [key, forms] : translation->messages()) {
        if (std::ranges::any_of(forms, [](const std::string& f) { return !f.empty(); })) {
            done.insert(key);
        }
    }
    std::int64_t awaiting = 0;
    if (po) {
        for (const PoEntry& entry : po_entries(text)) {
            if (entry.fuzzy && entry.translated && !entry.key.second.empty()) {
                done.insert(entry.key);
                ++awaiting;
            }
        }
    }
    std::set<std::string> wanted;
    if (const json::Value* list = params.find("messages"); list != nullptr) {
        for (const json::Value& message : list->as_array()) {
            wanted.insert(message.as_string());
        }
    }
    const std::int64_t max =
        params.find("max") != nullptr ? params.find("max")->as_int() : kDefaultMax;
    std::vector<Pending> pending;
    std::map<Key, std::string> plurals;
    std::int64_t remaining = 0;
    std::int64_t skipped = 0;
    for (const command::ExtractedMessage& message : command::extract_messages(session)) {
        Key key{message.context, message.message};
        if (!message.plural.empty()) {
            plurals.emplace(key, message.plural);
        }
        if (done.contains(key) || (!wanted.empty() && !wanted.contains(message.message))) {
            continue;
        }
        if (!po && !message.plural.empty()) {
            ++skipped; // a CSV file's plural rows aren't drafted; a .po file's are
            continue;
        }
        if (std::cmp_greater_equal(pending.size(), max)) {
            ++remaining;
            continue;
        }
        pending.push_back({std::move(key), message.plural});
    }

    json::Value out;
    out["locale"] = locale;
    out["file"] = path;
    if (pending.empty()) {
        out["drafted"] = 0;
        if (awaiting > 0) {
            out["awaiting_review"] = awaiting;
        }
        if (skipped > 0) {
            out["skipped"] = skipped;
        }
        session.mark_unchanged();
        return Reply::success(std::move(out));
    }

    // What the model is told.
    auto loaded = storyboard::load(
        [&session](std::string_view each) { return session.read_project_file(each); });
    std::vector<std::string> avoid;
    json::Array facts;
    if (loaded && loaded.value().errors.empty()) {
        facts = translator_facts(loaded.value().entries, avoid);
    }
    std::string prompt = "Target language: " + locale + " (" + std::to_string(nplurals) +
                         " plural form" + (nplurals == 1 ? "" : "s");
    if (nplurals > 1) {
        prompt += ", numbered by the gettext rule " + translation->plural_rules().plural();
    }
    prompt += ").\n";
    if (!facts.empty()) {
        prompt += "\nThe storyboard's glossary, characters and tone:\n" +
                  json::write(json::Value(std::move(facts)), json::Style::Compact) + "\n";
    }
    std::size_t examples = 0;
    for (const auto& [key, forms] : translation->messages()) {
        if (examples == kMaxExamples || forms.size() != 1 || forms.front().empty()) {
            continue;
        }
        if (examples++ == 0) {
            prompt += "\nTranslations the game already has, to match:\n";
        }
        prompt += json::write(json::Value(key.second), json::Style::Compact);
        if (!key.first.empty()) {
            prompt += " (context: " + key.first + ")";
        }
        prompt += " -> " + json::write(json::Value(forms.front()), json::Style::Compact) + "\n";
    }
    prompt += "\nMessages to translate:\n";
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const Pending& each = pending[i];
        prompt += "[" + std::to_string(i + 1) + "] " +
                  json::write(json::Value(each.key.second), json::Style::Compact);
        if (!each.key.first.empty()) {
            prompt += " (context: " + each.key.first + ")";
        }
        if (!each.plural.empty()) {
            prompt += " plural: " + json::write(json::Value(each.plural), json::Style::Compact);
        }
        prompt += "\n";
    }
    Request request;
    request.messages.push_back({.role = Role::System, .text = std::string(kInstructions)});
    request.messages.push_back({.role = Role::User, .text = std::move(prompt)});
    request.output_schema = output_schema();
    if (const json::Value* tokens = params.find("max_tokens"); tokens != nullptr) {
        request.params.max_tokens = tokens->as_int();
    }
    const std::string& root = session.options().project_root;
    auto config = load_config(root);
    if (!config) {
        return Reply::failure(config.error());
    }
    const json::Value* role = params.find("role");
    auto opened = open(config.value(), role != nullptr ? role->as_string() : std::string(),
                       {.project_root = root, .write_replays = session.options().write_files});
    if (!opened) {
        if (role != nullptr) {
            Reply reply = Reply::failure(opened.error());
            reply.data["path"] = "/role";
            return reply;
        }
        return Reply::failure(opened.error());
    }
    auto response = opened.value().provider->complete(request);
    if (!response) {
        return Reply::failure(response.error());
    }
    const json::Value& output = response.value().output;
    if (const std::vector<json::ValidationError> errors =
            json::validate(output, request.output_schema);
        !errors.empty()) {
        return Reply::failure(make_error(ErrorCode::InvalidArgument,
                                         "the model's reply does not list translations as "
                                         "asked: " +
                                             errors.front().message));
    }

    // Checked before anything is written.
    std::vector<Draft> drafts;
    json::Array rejected;
    std::set<std::size_t> answered;
    for (const json::Value& item : output.find("translations")->as_array()) {
        const std::int64_t number = item.find("item")->as_int();
        if (number < 1 || std::cmp_greater(number, pending.size()) ||
            !answered.insert(static_cast<std::size_t>(number)).second) {
            continue;
        }
        const Pending& each = pending[static_cast<std::size_t>(number) - 1];
        std::vector<std::string> forms;
        for (const json::Value& form : item.find("forms")->as_array()) {
            forms.push_back(form.as_string());
        }
        if (std::string problem = draft_problem(each, forms, nplurals, avoid); !problem.empty()) {
            json::Value entry;
            entry["message"] = each.key.second;
            if (!each.key.first.empty()) {
                entry["context"] = each.key.first;
            }
            entry["problem"] = std::move(problem);
            rejected.push_back(std::move(entry));
            continue;
        }
        drafts.push_back({each.key, std::move(forms), !each.plural.empty()});
    }
    json::Array listed;
    for (const Draft& draft : drafts) {
        json::Value entry;
        entry["message"] = draft.key.second;
        if (!draft.key.first.empty()) {
            entry["context"] = draft.key.first;
        }
        json::Array forms;
        for (const std::string& form : draft.forms) {
            forms.emplace_back(form);
        }
        entry["forms"] = std::move(forms);
        listed.push_back(std::move(entry));
    }
    if (!drafts.empty()) {
        std::string after =
            po ? po_with_drafts(text, drafts, plurals) : csv_with_drafts(text, drafts, locale);
        if (auto staged = session.stage_file(path, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, text, std::move(after));
    } else {
        session.mark_unchanged();
    }
    out["drafted"] = static_cast<std::int64_t>(drafts.size());
    out["drafts"] = std::move(listed);
    out["review"] = po ? "fuzzy" : "none";
    if (!rejected.empty()) {
        out["rejected"] = std::move(rejected);
    }
    if (const std::int64_t unanswered =
            static_cast<std::int64_t>(pending.size()) - static_cast<std::int64_t>(answered.size());
        unanswered > 0) {
        out["unanswered"] = unanswered;
    }
    if (remaining > 0) {
        out["remaining"] = remaining;
    }
    if (skipped > 0) {
        out["skipped"] = skipped;
    }
    out["model"] = response.value().model.empty() ? opened.value().model : response.value().model;
    out["usage"]["input_tokens"] = response.value().usage.input_tokens;
    out["usage"]["output_tokens"] = response.value().usage.output_tokens;
    if (response.value().replayed) {
        out["replayed"] = true;
    }
    return Reply::success(std::move(out));
}

} // namespace

void register_translate(command::Registry& registry) {
    json::Value locale = property("string", R"(The locale to draft, such as "fr" or "pt_BR".)");
    locale["minLength"] = 1;
    locale["maxLength"] = 64;
    json::Value file = property("string", "A .po or CSV file project.json lists to write the "
                                          "drafts to. Default: the first that has the locale.");
    file["minLength"] = 1;
    json::Value messages = property("array", "Only these messages. Default: every one the "
                                             "locale lacks.");
    messages["items"]["type"] = "string";
    messages["maxItems"] = kMaxMax;
    json::Value max = property("integer", "Messages to draft in this call. Default 40.");
    max["minimum"] = 1;
    max["maximum"] = kMaxMax;
    json::Value tokens = property("integer", "The most tokens the model may reply with.");
    tokens["minimum"] = 1;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"locale"};
    schema["properties"] = json::Object{
        {"locale", std::move(locale)},
        {"file", std::move(file)},
        {"messages", std::move(messages)},
        {"max", std::move(max)},
        {"role", property("string", "A role or provider from providers.json. Default: the "
                                    "\"default\" role, or the only provider.")},
        {"max_tokens", std::move(tokens)},
    };
    registry.add({
        .name = "l10n.translate",
        .description = "Drafts the translations a locale lacks with a model from providers.json, "
                       "told the storyboard's glossary, characters and tone. Drafts that break "
                       "placeholders or the glossary are rejected; a .po file marks the rest "
                       "fuzzy, for review.",
        .capabilities = {Capability::Read, Capability::Edit, Capability::Filesystem,
                         Capability::Network},
        .effect = command::Effect::Edit,
        .params = std::move(schema),
        .handler = l10n_translate,
    });
}

} // namespace slash::ai::detail
