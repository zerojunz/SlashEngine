// l10n.check: what a game's translations get wrong. Godot leaves this to the eye (its editor's
// pseudolocalization and a play in each language); this reads the translation files against the
// messages l10n.extract finds, and lays out every scene in each language to see what doesn't fit.
#include "l10n_check.hpp"

#include "slash/command/l10n.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/l10n/l10n.hpp"
#include "slash/l10n/locale.hpp"
#include "slash/l10n/translation.hpp"
#include "slash/platform/file.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

using Key = std::pair<std::string, std::string>; // context, message

constexpr std::int64_t kDefaultMax = 50;
constexpr std::int64_t kMaxMax = 500;

// What one check found.
struct Issue {
    std::string check; // "file", "placeholders", "overflow", "glyphs", "missing" or "stale"
    std::string file;
    std::string id{};     // NOLINT(readability-redundant-member-init): the control, in a scene
    std::string locale{}; // NOLINT(readability-redundant-member-init): "" for every locale
    std::string message;
};

// What gets in the way of a message being shown, sorted out as the checks find it.
class Findings {
public:
    void error(Issue issue) { add(errors_, std::move(issue)); }
    void warning(Issue issue) { add(warnings_, std::move(issue)); }

    [[nodiscard]] json::Value json(std::int64_t max) const {
        json::Value out;
        json::Object counts;
        for (const auto& [check, count] : counts_) {
            counts.emplace(check, count);
        }
        if (!counts.empty()) {
            out["counts"] = std::move(counts);
        }
        list(out, "errors", errors_, max);
        list(out, "warnings", warnings_, max);
        return out;
    }

private:
    void add(std::vector<Issue>& to, Issue issue) {
        ++counts_[issue.check];
        to.push_back(std::move(issue));
    }

    static void list(json::Value& out, std::string_view name, const std::vector<Issue>& issues,
                     std::int64_t max) {
        if (issues.empty()) {
            return;
        }
        json::Array items;
        for (const Issue& issue : issues) {
            if (std::cmp_greater_equal(items.size(), max)) {
                break;
            }
            json::Value item;
            item["check"] = issue.check;
            item["file"] = issue.file;
            if (!issue.id.empty()) {
                item["id"] = issue.id;
                item["path"] = "/entities/" + json::pointer_escape(issue.id);
            } else {
                item["path"] = "";
            }
            if (!issue.locale.empty()) {
                item["locale"] = issue.locale;
            }
            item["message"] = issue.message;
            items.push_back(std::move(item));
        }
        out[name] = std::move(items);
        if (std::cmp_greater(issues.size(), max)) {
            out[std::string(name) + "_more"] = static_cast<std::int64_t>(issues.size()) - max;
        }
    }

    std::vector<Issue> errors_;
    std::vector<Issue> warnings_;
    std::map<std::string, std::int64_t> counts_;
};

std::string quoted(std::string_view text) {
    std::string out = "\"";
    out += text;
    out += '"';
    return out;
}

// A message as a check names it: "Open" (door).
std::string named(const Key& key) {
    std::string out = quoted(key.second);
    if (!key.first.empty()) {
        out += " (" + key.first + ")";
    }
    return out;
}

// A translation file the project lists, read again.
struct ListedFile {
    std::string path;
    l10n::TranslationFile file;
};

// The source of each message to hold translations to: the fallback locale's own translation
// where it has one (projects that key messages by ID, as MENU_PLAY), else the message itself.
struct Sources {
    const l10n::Translation* fallback = nullptr;
    std::map<Key, std::string> plurals; // msgid_plural of plural messages, from l10n.extract

    [[nodiscard]] std::string singular(const Key& key) const {
        if (fallback != nullptr) {
            if (const auto found = fallback->messages().find(key);
                found != fallback->messages().end() && !found->second.empty() &&
                !found->second.front().empty()) {
                return found->second.front();
            }
        }
        return key.second;
    }

    [[nodiscard]] std::optional<std::string> plural(const Key& key) const {
        if (fallback != nullptr) {
            if (const auto found = fallback->messages().find(key);
                found != fallback->messages().end() && found->second.size() > 1 &&
                !found->second.back().empty()) {
                return found->second.back();
            }
        }
        if (const auto found = plurals.find(key); found != plurals.end()) {
            return found->second;
        }
        return std::nullopt;
    }
};

void check_placeholders(const std::vector<ListedFile>& files, const Sources& sources,
                        const std::string& fallback, const std::vector<std::string>& locales,
                        Findings& out) {
    for (const ListedFile& listed : files) {
        for (const l10n::Translation& translation : listed.file.translations) {
            if (std::ranges::find(locales, translation.locale()) == locales.end()) {
                continue;
            }
            const bool is_fallback = translation.locale() == fallback;
            for (const auto& [key, forms] : translation.messages()) {
                // The fallback's own translations are held to the messages they translate.
                std::string singular = key.second;
                std::optional<std::string> plural;
                if (!is_fallback) {
                    singular = sources.singular(key);
                    plural = sources.plural(key);
                } else if (const auto found = sources.plurals.find(key);
                           found != sources.plurals.end()) {
                    plural = found->second;
                }
                for (std::size_t i = 0; i < forms.size(); ++i) {
                    if (forms[i].empty()) {
                        continue;
                    }
                    // A plural message's forms all take the count, as its msgid_plural does.
                    const std::string& source = plural ? *plural : singular;
                    if (const std::string problem = l10n::placeholder_problem(source, forms[i]);
                        !problem.empty()) {
                        std::string message = named(key);
                        if (forms.size() > 1) {
                            message += " form " + std::to_string(i);
                        }
                        message += " " + problem;
                        out.error({.check = "placeholders",
                                   .file = listed.path,
                                   .locale = translation.locale(),
                                   .message = std::move(message)});
                    }
                }
            }
        }
    }
}

// The messages each locale lacks (warnings: they show in the fallback or as written), and those
// translated that the game no longer shows (warnings: tr may build them at run time).
void check_coverage(const std::vector<ListedFile>& files,
                    const std::vector<ExtractedMessage>& messages,
                    const std::vector<std::string>& locales, Findings& out) {
    struct Coverage {
        std::string file; // the first file with the locale
        std::set<Key> translated;
    };
    std::map<std::string, Coverage> by_locale;
    for (const ListedFile& listed : files) {
        for (const l10n::Translation& translation : listed.file.translations) {
            Coverage& coverage = by_locale[translation.locale()];
            if (coverage.file.empty()) {
                coverage.file = listed.path;
            }
            for (const auto& [key, forms] : translation.messages()) {
                if (std::ranges::any_of(forms, [](const std::string& f) { return !f.empty(); })) {
                    coverage.translated.insert(key);
                }
            }
        }
    }
    std::set<Key> shown;
    for (const ExtractedMessage& message : messages) {
        shown.insert({message.context, message.message});
    }
    for (const std::string& locale : locales) {
        const auto own = by_locale.find(locale);
        if (own == by_locale.end()) {
            continue;
        }
        // A regional locale takes what its language's translation has, as lookups do.
        const std::string language = l10n::Locale::parse(locale, false).language;
        const auto broader = language != locale ? by_locale.find(language) : by_locale.end();
        for (const ExtractedMessage& message : messages) {
            const Key key{message.context, message.message};
            if (own->second.translated.contains(key) ||
                (broader != by_locale.end() && broader->second.translated.contains(key))) {
                continue;
            }
            out.warning({.check = "missing",
                         .file = own->second.file,
                         .locale = locale,
                         .message = named(key) + " has no translation"});
        }
        if (messages.empty()) {
            continue; // nothing to hold the files to
        }
        for (const Key& key : own->second.translated) {
            if (!shown.contains(key)) {
                out.warning({.check = "stale",
                             .file = own->second.file,
                             .locale = locale,
                             .message = named(key) + " is translated but no scene or tr call "
                                                     "shows it"});
            }
        }
    }
}

// What each control shows: whether its text doesn't fit, and the glyphs its font lacks.
struct Shown {
    std::string text;
    bool cut = false; // wider or taller than its rect, trimmed, or lines hidden
    std::size_t missing = 0;
};

std::map<std::string, Shown> shown_controls(const world::World& world) {
    std::map<std::string, Shown> out;
    text::Fonts* fonts = ui::fonts(world);
    if (fonts == nullptr) {
        return out;
    }
    const ui::Layout layout = ui::layout(world, *fonts, ui::settings(world).viewport);
    for (const ui::Placed& placed : layout.controls) {
        const ui::LabelText* label = layout.label(placed.entity);
        const std::string_view id = world.id_of(placed.entity);
        if (label == nullptr || id.empty() || label->source.empty()) {
            continue;
        }
        Shown& shown = out[std::string(id)];
        shown.text = label->source;
        shown.cut = label->overflow || !label->trimmed.empty() ||
                    std::min(label->last, label->gutters_from) < label->text.lines.size();
        shown.missing = label->text.missing;
    }
    return out;
}

// Lays out a scene in each locale (and pseudolocalized), reporting text that fits as written but
// not translated, and glyphs the translated text needs that its font lacks.
void check_scene(Session& host, const std::string& path, const std::vector<std::string>& locales,
                 bool pseudo, Findings& out) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    Session session(host.components(), host.commands(), options);
    json::Value params;
    params["path"] = path;
    if (!session.call("scene.open", params).ok) {
        return; // scene validate reports it
    }
    if (path == host.scene_path()) {
        session.restore(host.save()); // as it is now, unsaved edits included
    }
    world::World& world = session.world();
    const std::map<std::string, Shown> written = shown_controls(world);
    if (written.empty()) {
        return;
    }
    const auto compare = [&](const std::string& locale, const std::string& as) {
        for (const auto& [id, shown] : shown_controls(world)) {
            const auto before = written.find(id);
            if (before == written.end() || before->second.text == shown.text) {
                continue; // not translated here
            }
            if (shown.cut && !before->second.cut) {
                out.error({.check = "overflow",
                           .file = path,
                           .id = id,
                           .locale = locale,
                           .message = quoted(shown.text) + " doesn't fit its control " + as + "; " +
                                      quoted(before->second.text) + " does"});
            }
            if (shown.missing > before->second.missing) {
                out.error({.check = "glyphs",
                           .file = path,
                           .id = id,
                           .locale = locale,
                           .message = quoted(shown.text) + " needs " +
                                      std::to_string(shown.missing) + " glyphs its font lacks " +
                                      as + "; give the control a font with them"});
            }
        }
    };
    const std::string start = l10n::locale(world);
    for (const std::string& locale : locales) {
        l10n::set_locale(world, locale);
        compare(locale, "in " + locale);
    }
    if (pseudo) {
        l10n::set_locale(world, start);
        l10n::Settings settings = l10n::settings(world);
        settings.pseudolocalization.use_pseudolocalization = true;
        l10n::install(world, session.assets(), settings);
        compare({}, "pseudolocalized");
    }
}

Reply l10n_check(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const l10n::Settings settings = l10n::settings(world);
    Findings findings;
    const auto max_of = [&]() {
        const json::Value* max = params.find("max");
        return max != nullptr ? max->as_int() : kDefaultMax;
    };

    // The files, as the game reads them.
    for (const l10n::FileProblem& problem : l10n::problems(world)) {
        std::string message;
        if (problem.line > 0) {
            message = "line " + std::to_string(problem.line) + ": ";
        }
        message += problem.message;
        findings.error({.check = "file", .file = problem.file, .message = std::move(message)});
    }
    std::vector<ListedFile> files;
    std::vector<std::string> all_locales;
    for (const std::string& path : settings.translations) {
        auto text = session.read_project_file(path);
        if (!text || !text.value()) {
            continue; // a problem above
        }
        auto read = l10n::read_translation_file(path, *text.value());
        if (!read) {
            continue;
        }
        for (const l10n::Translation& translation : read.value().translations) {
            if (std::ranges::find(all_locales, translation.locale()) == all_locales.end()) {
                all_locales.push_back(translation.locale());
            }
        }
        files.push_back({path, std::move(read).value()});
    }

    // The locales to check: those asked for, else every one translated.
    std::vector<std::string> locales = all_locales;
    if (const json::Value* asked = params.find("locales"); asked != nullptr) {
        locales.clear();
        const json::Array& list = asked->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string locale = l10n::standardize_locale(list[i].as_string());
            if (std::ranges::find(all_locales, locale) == all_locales.end()) {
                json::Value data;
                data["path"] = "/locales/" + std::to_string(i);
                std::string message = data["path"].as_string() + ": no translation file has " +
                                      quoted(list[i].as_string());
                if (const std::string_view near = closest_match(locale, all_locales);
                    !near.empty()) {
                    message += "; did you mean " + quoted(near) + "?";
                }
                return Reply::failure(code::kInvalidParams, std::move(message), std::move(data));
            }
            locales.push_back(locale);
        }
    }

    const std::vector<ExtractedMessage> messages = extract_messages(session);
    Sources sources;
    for (const ListedFile& listed : files) {
        for (const l10n::Translation& translation : listed.file.translations) {
            if (sources.fallback == nullptr && translation.locale() == settings.fallback) {
                sources.fallback = &translation;
            }
        }
    }
    for (const ExtractedMessage& message : messages) {
        if (!message.plural.empty()) {
            sources.plurals.emplace(Key{message.context, message.message}, message.plural);
        }
    }
    check_placeholders(files, sources, settings.fallback, locales, findings);
    check_coverage(files, messages, locales, findings);

    // Every scene, laid out in each locale.
    std::int64_t scenes = 0;
    const bool pseudo = [&] {
        const json::Value* value = params.find("pseudolocalization");
        return value != nullptr && value->as_bool();
    }();
    if (const json::Value* layout = params.find("layout"); layout == nullptr || layout->as_bool()) {
        auto listed =
            platform::list_files(session.options().project_root,
                                 [](std::string_view folder) { return folder != "build/"; });
        if (listed) {
            for (const std::string& path : listed.value()) {
                if (!path.ends_with(world::kSceneSuffix)) {
                    continue;
                }
                ++scenes;
                check_scene(session, path, locales, pseudo, findings);
            }
        }
    }

    json::Value out = findings.json(max_of());
    json::Value checked;
    json::Array checked_locales;
    for (const std::string& locale : locales) {
        checked_locales.emplace_back(locale);
    }
    checked["locales"] = std::move(checked_locales);
    checked["messages"] = static_cast<std::int64_t>(messages.size());
    checked["scenes"] = scenes;
    out["checked"] = std::move(checked);
    return Reply::success(std::move(out));
}

} // namespace

void register_l10n_check(Registry& registry) {
    using enum Capability;
    json::Value locales;
    locales["type"] = "array";
    locales["description"] = "Locales to check. Default: every one the translation files have.";
    locales["items"]["type"] = "string";
    locales["items"]["minLength"] = 1;
    locales["maxItems"] = 64;
    json::Value max;
    max["type"] = "integer";
    max["minimum"] = 1;
    max["maximum"] = kMaxMax;
    max["description"] = "Errors and warnings to return, each. Default 50.";
    json::Value layout;
    layout["type"] = "boolean";
    layout["description"] = "Lay out every scene in each locale. Default true.";
    json::Value pseudo;
    pseudo["type"] = "boolean";
    pseudo["description"] = "Lay them out pseudolocalized too, as the project's settings make it. "
                            "Default false.";
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{{"locales", std::move(locales)},
                                        {"max", std::move(max)},
                                        {"layout", std::move(layout)},
                                        {"pseudolocalization", std::move(pseudo)}};
    registry.add({
        .name = "l10n.check",
        .description = "Checks the translations: files that don't load, placeholders that don't "
                       "match (errors), messages missing or no longer shown (warnings), and "
                       "text that doesn't fit its control or lacks glyphs in a locale (errors).",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = l10n_check,
    });
}

} // namespace slash::command
