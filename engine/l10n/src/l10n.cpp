#include "slash/l10n/l10n.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/text.hpp"
#include "slash/l10n/locale.hpp"
#include "slash/platform/locale.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::l10n {
namespace {

constexpr std::array<std::string_view, 4> kDirections{"application_locale", "ltr", "rtl",
                                                      "system_locale"};

// A translation file the settings list, as last read.
struct Source {
    std::string path;
    asset::Handle<TranslationFile> file; // a .po or .mo file
    asset::Handle<std::string> text;     // a CSV file
    std::uint64_t version = 0;           // of the handle, when translations were read
    std::vector<Translation> translations;
    std::vector<FileProblem> problems;
};

struct State {
    Settings settings;
    std::string locale = "en";
    std::string system = "en";
    // Lookups come from any thread (threaded systems, the renderer's layout), so what they
    // read again after a reload is behind a lock.
    mutable std::mutex mutex;
    mutable std::vector<Source> sources;
    std::vector<FileProblem> load_problems; // files that didn't load at all
    // How well each translation matches a locale, by locale, for the translations as now read.
    mutable std::map<std::string, std::vector<int>, std::less<>> scores;
};

const State* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<State>>();
    return found != nullptr ? found->get() : nullptr;
}

State* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<std::shared_ptr<State>>();
    return found != nullptr ? found->get() : nullptr;
}

// Reads again the sources whose files reloaded. Call with the lock held.
void refresh(const State& state) {
    bool changed = false;
    for (Source& source : state.sources) {
        const std::uint64_t version = source.file ? source.file.version() : source.text.version();
        if (version == source.version) {
            continue;
        }
        changed = true;
        source.version = version;
        source.translations.clear();
        source.problems.clear();
        Result<TranslationFile> read =
            source.file ? Result<TranslationFile>(*source.file) : read_csv(*source.text);
        if (!read) {
            source.problems.push_back({source.path, 0, read.error().message});
            continue;
        }
        source.translations = std::move(read.value().translations);
        for (const TranslationFile::Problem& problem : read.value().problems) {
            source.problems.push_back({source.path, problem.line, problem.message});
        }
    }
    if (changed) {
        state.scores.clear();
    }
}

// The translations' scores against a locale. Call with the lock held.
const std::vector<int>& scores_for(const State& state, std::string_view locale) {
    auto found = state.scores.find(locale);
    if (found == state.scores.end()) {
        std::vector<int> scores;
        for (const Source& source : state.sources) {
            for (const Translation& translation : source.translations) {
                scores.push_back(compare_locales(locale, translation.locale()));
            }
        }
        found = state.scores.emplace(std::string(locale), std::move(scores)).first;
    }
    return found->second;
}

// The translated text of the translation that matches the locale best, as
// TranslationDomain::get_message_from_translations: the last of those that match equally well,
// unless one matches exactly. Call with the lock held.
template <typename Get>
const std::string* message_from(const State& state, std::string_view locale, const Get& get) {
    const std::vector<int>& scores = scores_for(state, locale);
    const std::string* best = nullptr;
    int best_score = 0;
    std::size_t index = 0;
    for (const Source& source : state.sources) {
        for (const Translation& translation : source.translations) {
            const int score = scores[index++];
            if (score <= 0 || score < best_score) {
                continue;
            }
            const std::string* found = get(translation);
            if (found == nullptr) {
                continue;
            }
            best = found;
            best_score = score;
            if (score == 10) {
                return best; // Exact match, skip the rest.
            }
        }
    }
    return best;
}

template <typename Get>
const std::string* lookup(const State& state, const Get& get) {
    const std::string* found = message_from(state, state.locale, get);
    if (found == nullptr && state.settings.fallback.size() >= 2) {
        found = message_from(state, state.settings.fallback, get);
    }
    return found;
}

// Pseudo-localization, as Godot's TranslationDomain.

constexpr std::array<std::string_view, 52> kAccented{
    "Å", "ß", "Ç", "Ð", "É", "F́", "Ĝ", "Ĥ", "Ĩ", "Ĵ", "ĸ", "Ł", "Ḿ", "й", "Ö", "Ṕ", "Q́", "Ř",
    "Ŝ", "Ŧ", "Ũ", "Ṽ", "Ŵ", "X́", "Ÿ", "Ž", "á", "ḅ", "ć", "d́", "é", "f́", "ǵ", "h̀", "í", "ǰ",
    "ḱ", "ł", "m̀", "ή", "ô", "ṕ", "q́", "ŕ", "š", "ŧ", "ü", "ṽ", "ŵ", "x́", "ý", "ź",
};

bool is_placeholder(const std::u32string& message, std::size_t i) {
    if (i + 1 >= message.size() || message[i] != U'%') {
        return false;
    }
    const char32_t c = message[i + 1];
    return c == U's' || c == U'c' || c == U'd' || c == U'o' || c == U'x' || c == U'X' || c == U'f';
}

// Copies a placeholder at i, if skipping them, and says whether it did.
bool keep_placeholder(const Pseudolocalization& settings, const std::u32string& message,
                      std::size_t& i, std::u32string& out) {
    if (!settings.skip_placeholders || !is_placeholder(message, i)) {
        return false;
    }
    out += message[i];
    out += message[i + 1];
    ++i;
    return true;
}

std::u32string override_string(const Pseudolocalization& settings, const std::u32string& message) {
    std::u32string out;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (!keep_placeholder(settings, message, i, out)) {
            out += U'*';
        }
    }
    return out;
}

std::u32string double_vowels(const Pseudolocalization& settings, const std::u32string& message) {
    std::u32string out;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (keep_placeholder(settings, message, i, out)) {
            continue;
        }
        const char32_t c = message[i];
        out += c;
        if (std::u32string_view(U"aeiouAEIOU").find(c) != std::u32string_view::npos) {
            out += c;
        }
    }
    return out;
}

std::u32string accented(const Pseudolocalization& settings, const std::u32string& message) {
    std::u32string out;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (keep_placeholder(settings, message, i, out)) {
            continue;
        }
        const char32_t c = message[i];
        if (c >= U'A' && c <= U'Z') {
            out += to_utf32(kAccented[c - U'A']);
        } else if (c >= U'a' && c <= U'z') {
            out += to_utf32(kAccented[26 + (c - U'a')]);
        } else {
            out += c;
        }
    }
    return out;
}

std::u32string fake_bidi(const Pseudolocalization& settings, const std::u32string& message) {
    constexpr char32_t kPrefix = 0x202E; // right-to-left override
    constexpr char32_t kSuffix = 0x202C; // pop directional formatting
    std::u32string out(1, kPrefix);
    // The override ends at every newline, so it starts again after each.
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] == U'\n') {
            out += kSuffix;
            out += message[i];
            out += kPrefix;
        } else if (settings.skip_placeholders && is_placeholder(message, i)) {
            out += kSuffix;
            out += message[i];
            out += message[i + 1];
            out += kPrefix;
            ++i;
        } else {
            out += message[i];
        }
    }
    out += kSuffix;
    return out;
}

json::Value boolean(bool value, std::string_view description) {
    json::Value schema;
    schema["type"] = "boolean";
    schema["default"] = value;
    schema["description"] = description;
    return schema;
}

json::Value string(std::string_view value, std::string_view description) {
    json::Value schema;
    schema["type"] = "string";
    schema["default"] = value;
    schema["description"] = description;
    return schema;
}

json::Value object(std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = description;
    return schema;
}

const json::Value* member(const json::Value* object, std::string_view key) {
    return object != nullptr && object->is_object() ? object->find(key) : nullptr;
}

void read_bool(const json::Value* object, std::string_view key, bool& out) {
    if (const json::Value* value = member(object, key); value != nullptr && value->is_bool()) {
        out = value->as_bool();
    }
}

void read_string(const json::Value* object, std::string_view key, std::string& out) {
    if (const json::Value* value = member(object, key); value != nullptr && value->is_string()) {
        out = value->as_string();
    }
}

bool translation_suffix(std::string_view path) {
    return path.ends_with(".csv") || path.ends_with(".po") || path.ends_with(".mo");
}

// Loads the settings' translation files.
void load_sources(State& state, asset::Assets& assets) {
    const std::scoped_lock lock(state.mutex);
    state.sources.clear();
    state.scores.clear();
    state.load_problems.clear();
    for (const std::string& path : state.settings.translations) {
        Source source;
        source.path = path;
        if (path.ends_with(".po") || path.ends_with(".mo")) {
            auto loaded = assets.load<TranslationFile>(path);
            if (!loaded) {
                state.load_problems.push_back({path, 0, loaded.error().message});
                continue;
            }
            source.file = std::move(loaded).value();
        } else {
            auto loaded = assets.load<std::string>(path);
            if (!loaded) {
                state.load_problems.push_back({path, 0, loaded.error().message});
                continue;
            }
            source.text = std::move(loaded).value();
        }
        state.sources.push_back(std::move(source));
    }
    refresh(state);
}

} // namespace

json::Value settings_schema() {
    json::Value translations;
    translations["type"] = "array";
    translations["items"]["type"] = "string";
    translations["default"] = json::Array{};
    translations["description"] =
        "Translation files by project path: CSV files (a keys column, then a column per locale) "
        "and gettext .po and .mo files.";
    json::Value locale = object("The project's translations and the locales it looks in, as "
                                "Godot's internationalization/locale.");
    locale["properties"]["translations"] = std::move(translations);
    locale["properties"]["fallback"] =
        string("en", "The locale to look in when the player's has no translation of a message; "
                     "\"\" for none, so the source text shows.");
    locale["properties"]["test"] =
        string("", "A locale to run in instead of the player's, to check a translation; \"\" "
                   "for the player's.");

    json::Value pseudo = object("A fake translation of every message that shows untranslated, "
                                "clipped or right-to-left-unready text, as Godot's.");
    pseudo["properties"]["use_pseudolocalization"] =
        boolean(false, "Pseudolocalize every message looked up.");
    pseudo["properties"]["replace_with_accents"] =
        boolean(true, "Replace letters with accented ones: \"Start\" is \"Ŝŧáŕŧ\".");
    pseudo["properties"]["double_vowels"] =
        boolean(false, "Write vowels twice, a rough look at longer text.");
    pseudo["properties"]["fake_bidi"] = boolean(false, "Show text as if written right to left.");
    pseudo["properties"]["override"] =
        boolean(false, "Replace every character with '*', so nothing reads as the source.");
    pseudo["properties"]["expansion_ratio"]["type"] = "number";
    pseudo["properties"]["expansion_ratio"]["minimum"] = 0;
    pseudo["properties"]["expansion_ratio"]["maximum"] = 1;
    pseudo["properties"]["expansion_ratio"]["default"] = 0;
    pseudo["properties"]["expansion_ratio"]["description"] =
        "How much longer text gets, as underscores around it: 0.3 adds 30% of its length.";
    pseudo["properties"]["prefix"] = string("[", "Put before every message.");
    pseudo["properties"]["suffix"] = string("]", "Put after every message.");
    pseudo["properties"]["skip_placeholders"] =
        boolean(true, "Leave %s, %d, %f and the other placeholders as they are.");

    json::Value direction;
    json::Array directions;
    for (const std::string_view each : kDirections) {
        directions.emplace_back(std::string(each));
    }
    direction["enum"] = std::move(directions);
    direction["default"] = "application_locale";
    direction["description"] =
        "Which way controls at the root lay out when they inherit: as the game's locale writes, "
        "left to right, right to left, or as the player's machine's locale writes.";
    json::Value rendering = object("How locales lay out controls, as Godot's "
                                   "internationalization/rendering.");
    rendering["properties"]["root_node_layout_direction"] = std::move(direction);
    rendering["properties"]["force_right_to_left_layout_direction"] =
        boolean(false, "Lay out every control right to left but those set to ltr, to check a "
                       "layout before a right-to-left translation exists.");

    json::Value schema = object("Translations and the player's language, as Godot's "
                                "internationalization settings.");
    schema["properties"]["locale"] = std::move(locale);
    schema["properties"]["pseudolocalization"] = std::move(pseudo);
    schema["properties"]["rendering"] = std::move(rendering);
    return schema;
}

std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                     std::string_view at) {
    std::vector<json::ValidationError> errors = json::validate(settings, settings_schema());
    for (json::ValidationError& error : errors) {
        error.path = std::string(at) + error.path;
    }
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* translations = member(member(&settings, "locale"), "translations");
    if (translations == nullptr) {
        return errors;
    }
    const json::Array& files = translations->as_array();
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string& path = files[i].as_string();
        const std::string pointer = std::string(at) + "/locale/translations/" + std::to_string(i);
        if (!is_project_path(path)) {
            errors.push_back({pointer, "expected a project path, relative and separated by '/'"});
        } else if (!translation_suffix(path)) {
            errors.push_back({pointer, "expected a .csv, .po or .mo file"});
        }
    }
    return errors;
}

Settings parse_settings(const json::Value& settings) {
    Settings out;
    const json::Value* locale = member(&settings, "locale");
    if (const json::Value* translations = member(locale, "translations");
        translations != nullptr && translations->is_array()) {
        for (const json::Value& path : translations->as_array()) {
            if (path.is_string()) {
                out.translations.push_back(path.as_string());
            }
        }
    }
    read_string(locale, "fallback", out.fallback);
    read_string(locale, "test", out.test);
    const json::Value* pseudo = member(&settings, "pseudolocalization");
    Pseudolocalization& p = out.pseudolocalization;
    read_bool(pseudo, "use_pseudolocalization", p.use_pseudolocalization);
    read_bool(pseudo, "replace_with_accents", p.replace_with_accents);
    read_bool(pseudo, "double_vowels", p.double_vowels);
    read_bool(pseudo, "fake_bidi", p.fake_bidi);
    read_bool(pseudo, "override", p.override);
    if (const json::Value* ratio = member(pseudo, "expansion_ratio");
        ratio != nullptr && ratio->is_number()) {
        p.expansion_ratio = ratio->as_double();
    }
    read_string(pseudo, "prefix", p.prefix);
    read_string(pseudo, "suffix", p.suffix);
    read_bool(pseudo, "skip_placeholders", p.skip_placeholders);
    const json::Value* rendering = member(&settings, "rendering");
    if (const json::Value* direction = member(rendering, "root_node_layout_direction");
        direction != nullptr && direction->is_string()) {
        for (std::size_t i = 0; i < kDirections.size(); ++i) {
            if (kDirections[i] == direction->as_string()) {
                out.root_node_layout_direction = static_cast<RootDirection>(i);
            }
        }
    }
    read_bool(rendering, "force_right_to_left_layout_direction",
              out.force_right_to_left_layout_direction);
    return out;
}

std::string_view direction_name(RootDirection direction) {
    return kDirections[static_cast<std::size_t>(direction)];
}

std::string preferred_locale() {
    const std::string locale = platform::preferred_locale();
    return locale.empty() ? std::string("en") : locale;
}

void add_asset_types(asset::Assets& assets) {
    assets.add_type<TranslationFile>("translation", {".po", ".mo"},
                                     [](std::string_view text, std::string_view path) {
                                         return read_translation_file(asset::file_of(path), text);
                                     });
}

void install(world::World& world, asset::Assets& assets, const Settings& settings,
             std::string_view system_locale) {
    add_asset_types(assets);
    State* state = state_of(world);
    const bool first = state == nullptr;
    if (first) {
        auto made = std::make_shared<State>();
        state = made.get();
        world.ecs().ctx().emplace<std::shared_ptr<State>>(std::move(made));
    }
    const bool test_changed = first || state->settings.test != settings.test;
    state->settings = settings;
    if (first) {
        std::string system = standardize_locale(system_locale);
        if (system.empty()) {
            system = settings.fallback.size() >= 2 ? standardize_locale(settings.fallback) : "en";
        }
        state->system = std::move(system);
    }
    load_sources(*state, assets);
    if (test_changed) {
        const std::string test = standardize_locale(settings.test);
        const std::scoped_lock lock(state->mutex);
        state->locale = !test.empty() ? test : state->system;
    }
}

Settings settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->settings : Settings{};
}

std::string locale(const world::World& world) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return "en";
    }
    const std::scoped_lock lock(state->mutex);
    return state->locale;
}

std::string system_locale(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->system : std::string("en");
}

void set_locale(world::World& world, std::string_view locale) {
    State* state = state_of(world);
    const std::string standard = standardize_locale(locale);
    if (state == nullptr || standard.empty()) {
        return;
    }
    {
        const std::scoped_lock lock(state->mutex);
        if (state->locale == standard) {
            return;
        }
        state->locale = standard;
    }
    json::Value data;
    data["locale"] = standard;
    world.emit("translation_changed", {}, std::move(data));
}

std::string text_language(const world::World& world) {
    const State* state = state_of(world);
    if (state == nullptr ||
        (state->settings.translations.empty() && state->settings.test.empty())) {
        return {};
    }
    std::string language = locale(world);
    std::replace(language.begin(), language.end(), '_', '-');
    return language;
}

std::string tr(const world::World& world, std::string_view message, std::string_view context) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return std::string(message);
    }
    std::string out;
    {
        const std::scoped_lock lock(state->mutex);
        refresh(*state);
        const std::string* found = lookup(*state, [&](const Translation& translation) {
            return translation.message(message, context);
        });
        out = found != nullptr ? *found : std::string(message);
    }
    if (state->settings.pseudolocalization.use_pseudolocalization) {
        out = pseudolocalize(state->settings.pseudolocalization, out);
    }
    return out;
}

std::string tr_n(const world::World& world, std::string_view message, std::string_view plural,
                 int n, std::string_view context) {
    const State* state = state_of(world);
    const std::string_view source = n == 1 ? message : plural;
    if (state == nullptr) {
        return std::string(source);
    }
    const std::scoped_lock lock(state->mutex);
    refresh(*state);
    const std::string* found = lookup(*state, [&](const Translation& translation) {
        return translation.plural_message(message, n, context);
    });
    return found != nullptr ? *found : std::string(source);
}

std::vector<std::string> loaded_locales(const world::World& world) {
    std::vector<std::string> out;
    for (const Translation& translation : translations(world)) {
        if (std::ranges::find(out, translation.locale()) == out.end()) {
            out.push_back(translation.locale());
        }
    }
    return out;
}

std::vector<Translation> translations(const world::World& world) {
    std::vector<Translation> out;
    const State* state = state_of(world);
    if (state == nullptr) {
        return out;
    }
    const std::scoped_lock lock(state->mutex);
    refresh(*state);
    for (const Source& source : state->sources) {
        out.insert(out.end(), source.translations.begin(), source.translations.end());
    }
    return out;
}

std::vector<FileProblem> problems(const world::World& world) {
    std::vector<FileProblem> out;
    const State* state = state_of(world);
    if (state == nullptr) {
        return out;
    }
    const std::scoped_lock lock(state->mutex);
    refresh(*state);
    out = state->load_problems;
    for (const Source& source : state->sources) {
        out.insert(out.end(), source.problems.begin(), source.problems.end());
    }
    // In the order the settings list the files.
    const std::vector<std::string>& files = state->settings.translations;
    std::ranges::stable_sort(out, {}, [&files](const FileProblem& problem) {
        return std::ranges::find(files, problem.file) - files.begin();
    });
    return out;
}

bool is_layout_rtl(const world::World& world, std::string_view direction, const bool* parent) {
    if (direction == "ltr") {
        return false;
    }
    if (direction == "rtl") {
        return true;
    }
    const State* state = state_of(world);
    if (state == nullptr) {
        return direction == "inherited" && parent != nullptr && *parent;
    }
    if (state->settings.force_right_to_left_layout_direction) {
        return true;
    }
    RootDirection mode = RootDirection::ApplicationLocale;
    if (direction == "system_locale") {
        mode = RootDirection::SystemLocale;
    } else if (direction == "inherited") {
        if (parent != nullptr) {
            return *parent;
        }
        mode = state->settings.root_node_layout_direction;
    }
    switch (mode) {
    case RootDirection::Ltr:
        return false;
    case RootDirection::Rtl:
        return true;
    case RootDirection::SystemLocale:
        return is_locale_right_to_left(state->system);
    case RootDirection::ApplicationLocale:
        break;
    }
    const std::scoped_lock lock(state->mutex);
    return is_locale_right_to_left(state->locale);
}

std::string pseudolocalize(const Pseudolocalization& settings, std::string_view message) {
    if (message.empty()) {
        return {};
    }
    std::u32string text = to_utf32(message);
    const std::size_t length = text.size();
    if (settings.override) {
        text = override_string(settings, text);
    }
    if (settings.double_vowels) {
        text = double_vowels(settings, text);
    }
    if (settings.replace_with_accents) {
        text = accented(settings, text);
    }
    if (settings.fake_bidi) {
        text = fake_bidi(settings, text);
    }
    // As Godot's _add_padding, in its float: length * ratio / 2 underscores each side, rounded
    // down.
    const auto count = static_cast<std::size_t>(
        static_cast<float>(length) * static_cast<float>(settings.expansion_ratio) / 2.0F);
    const std::string underscores(count, '_');
    return settings.prefix + underscores + to_utf8(text) + underscores + settings.suffix;
}

} // namespace slash::l10n
