#pragma once

#include "slash/core/expression.hpp"
#include "slash/core/result.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Translation tables, as Godot's Translation: the translated text of each message (its source
// text, and a context that tells apart messages written the same) in one locale, with gettext's
// plural forms. They come from gettext .po and .mo files and from CSV files, read as Godot's
// TranslationLoaderPO and ResourceImporterCSVTranslation read them.
namespace slash::l10n {

// A gettext plural rule, "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 ? 1 : 2);", which
// picks the form of a message for a number, as Godot's PluralRules: the expression is split at
// its "?" and ":" into tests that run as Godot's Expression.
class PluralRules {
public:
    // The rule, or why it isn't one, as Godot's PluralRules::parse.
    [[nodiscard]] static Result<PluralRules> parse(std::string_view rules);

    [[nodiscard]] int nplurals() const { return nplurals_; }
    [[nodiscard]] const std::string& plural() const { return plural_; }
    // The form for n, from 0; 0 where a test can't run.
    [[nodiscard]] int evaluate(int n) const;

    struct Test {
        Expression expression;
        std::shared_ptr<const Test> left;  // when it holds
        std::shared_ptr<const Test> right; // when it doesn't
    };

private:
    int nplurals_ = 1;
    std::string plural_;
    std::shared_ptr<const Test> tests_;
};

// One locale's messages, as Godot's Translation.
class Translation {
public:
    // The locale, standardized (standardize_locale).
    [[nodiscard]] const std::string& locale() const { return locale_; }
    void set_locale(std::string_view locale);
    // A rule that replaces the locale's own, as a .po file's Plural-Forms header or a CSV file's
    // ?pluralrule row; "" for the locale's.
    [[nodiscard]] const std::string& plural_rules_override() const { return plural_override_; }
    void set_plural_rules_override(std::string_view rules);
    // The rule that picks plural forms: the override, the locale's (plural_rules_for), else
    // English's.
    [[nodiscard]] const PluralRules& plural_rules() const;

    void add_message(std::string_view message, std::string_view translated,
                     std::string_view context = {});
    // A message's plural forms, in the order the rule numbers them.
    void add_plural_message(std::string_view message, std::vector<std::string> forms,
                            std::string_view context = {});
    // A message's translation, or nullptr.
    [[nodiscard]] const std::string* message(std::string_view message,
                                             std::string_view context = {}) const;
    // A message's form for n, or nullptr: when it has none, n is negative or the rule picks a
    // form it lacks.
    [[nodiscard]] const std::string* plural_message(std::string_view message, int n,
                                                    std::string_view context = {}) const;

    // Orders messages by context, then source text, and finds them by string views.
    struct KeyLess {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            const std::string_view a_context = a.first;
            const std::string_view b_context = b.first;
            if (a_context != b_context) {
                return a_context < b_context;
            }
            return std::string_view(a.second) < std::string_view(b.second);
        }
    };
    // Messages by context, then source text.
    using Messages =
        std::map<std::pair<std::string, std::string>, std::vector<std::string>, KeyLess>;
    [[nodiscard]] const Messages& messages() const { return messages_; }

private:
    std::string locale_ = "en";
    std::string plural_override_;
    Messages messages_;
    mutable std::shared_ptr<const PluralRules> rules_;
};

// What a translation file holds: one translation per locale it has (a .po or .mo file has one, a
// CSV file one per column), and what in it didn't read, each with the line it is on.
struct TranslationFile {
    std::vector<Translation> translations;
    struct Problem {
        int line = 0; // from 1; 0 for the whole file
        std::string message;
    };
    std::vector<Problem> problems;
};

// A gettext .po file, as TranslationLoaderPO reads it: msgctxt, msgid, msgid_plural, msgstr and
// msgstr[n], fuzzy entries left out, and the locale and Plural-Forms from the header. An error
// for what Godot refuses to load, naming the line.
[[nodiscard]] Result<TranslationFile> read_po(std::string_view text);
// A gettext .mo file, compiled from a .po file, as TranslationLoaderPO reads one.
[[nodiscard]] Result<TranslationFile> read_mo(std::string_view bytes);

// How a CSV translation file is read, as Godot's csv_translation importer's options.
struct CsvOptions {
    char delimiter = ',';              // ',', ';' or '\t'
    bool unescape_keys = false;        // read \n, \t, \" and \\ in the keys as what they stand for
    bool unescape_translations = true; // and in the translations
};
// A CSV file, as ResourceImporterCSVTranslation reads one: a header row of "keys" (any name)
// then a locale per column (columns starting with "_" are left out), with optional "?context"
// and "?plural" columns, then a row per message. A row whose ?plural is set starts a plural
// message, and the rows after it with an empty key give its other forms. A "?pluralrule" row
// sets columns' rules. Columns with no valid locale are problems.
[[nodiscard]] Result<TranslationFile> read_csv(std::string_view text,
                                               const CsvOptions& options = {});

// A translation file by its suffix: .po, .mo, or else CSV.
[[nodiscard]] Result<TranslationFile> read_translation_file(std::string_view path,
                                                            std::string_view text);

// The placeholders of a text: the conversions of Godot's String::sprintf ("%d", "%.2f", "%s"),
// which take their values in order, and the names of String::format ("{name}", "{0}").
struct Placeholders {
    std::vector<std::string> conversions; // "%d", "%s": each one's letter, in order
    std::set<std::string> names;          // "{name}"
};
[[nodiscard]] Placeholders placeholders_of(std::string_view text);
// Why a translation's placeholders don't fit its source's, or "": sprintf's conversions must
// come in the same order, since they take their values so; String::format's names must be the
// source's (an unknown one would show as written).
[[nodiscard]] std::string placeholder_problem(std::string_view source, std::string_view translated);

} // namespace slash::l10n
