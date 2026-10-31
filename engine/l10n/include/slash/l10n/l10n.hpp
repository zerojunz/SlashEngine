#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/l10n/translation.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slash::asset {
class Assets;
} // namespace slash::asset

namespace slash::world {
class World;
} // namespace slash::world

// Localization, as Godot's TranslationServer: a world's locale, the project's translation files
// and the lookups gameplay code and controls make through them.
//
//   l10n::install(world, assets, project_settings, l10n::preferred_locale());
//   label.text = l10n::tr(world, "Start game");
//   l10n::tr_n(world, "%d apple", "%d apples", count);
//   l10n::set_locale(world, "fr");   // from an options menu; controls follow at once
//
// A lookup asks the translations of the locale best matching the world's (compare_locales; "es"
// answers for "es_MX"), then those of the fallback locale, and else returns the source text, so
// a game is written in any language and translated into others. Controls translate their text as
// Godot's do, as their auto_translate_mode says (ui::layout).
namespace slash::l10n {

// A fake translation that shows untranslated, clipped or right-to-left-unready text before any
// real translation exists, as Godot's internationalization/pseudolocalization settings.
struct Pseudolocalization {
    bool use_pseudolocalization = false; // pseudolocalize every lookup
    bool replace_with_accents = true;    // "Start" -> "Ŝŧáŕŧ"
    bool double_vowels = false;          // "Start" -> "Staart", a rough look at longer text
    bool fake_bidi = false;              // as if written right to left
    bool override = false;               // every character "*", so nothing reads as source
    // How much longer text gets, as underscores around it: 0.3 adds 30% of its length.
    double expansion_ratio = 0.0;
    std::string prefix = "["; // around every text, so cut-off ends show
    std::string suffix = "]";
    bool skip_placeholders = true; // %s, %d and the rest are left as they are
};

// What a control at the root takes as its layout direction when it inherits, as Godot's
// internationalization/rendering/root_node_layout_direction.
enum class RootDirection : std::uint8_t { ApplicationLocale, Ltr, Rtl, SystemLocale };

// project.json's "internationalization", as Godot's internationalization settings.
struct Settings {
    // Translation files (.csv, .po and .mo) by project path, as Godot's
    // internationalization/locale/translations.
    std::vector<std::string> translations;
    // The locale to look in when the world's has no translation of a message.
    std::string fallback = "en";
    // A locale to run in instead of the player's, to check a translation; "" for the player's.
    std::string test;
    Pseudolocalization pseudolocalization;
    RootDirection root_node_layout_direction = RootDirection::ApplicationLocale;
    // Every control lays out right to left, to check a layout before a right-to-left
    // translation exists.
    bool force_right_to_left_layout_direction = false;
};

[[nodiscard]] json::Value settings_schema();
// Every error in project.json's "internationalization", each with its JSON Pointer under at.
[[nodiscard]] std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                                   std::string_view at);
// The settings in a valid "internationalization" object.
[[nodiscard]] Settings parse_settings(const json::Value& settings);
[[nodiscard]] std::string_view direction_name(RootDirection direction);

// The player's language, as Godot's OS::get_locale: the first of the operating system's
// preferred locales, such as "en_US"; "en" where it says none.
[[nodiscard]] std::string preferred_locale();

// Asset type "translation": .po and .mo files as TranslationFile. CSV files load as text and
// are read here.
void add_asset_types(asset::Assets& assets);

// Gives the world a translation server reading the settings' files through assets, which must
// outlive the world, in the settings' test locale, else system_locale (preferred_locale() in
// games), else the fallback. Again, it takes the new settings and keeps the locale unless test
// changed. Translation files reload as they change.
void install(world::World& world, asset::Assets& assets, const Settings& settings,
             std::string_view system_locale = {});
// The settings of a world, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);

// The world's locale, standardized; "en" where nothing was installed.
[[nodiscard]] std::string locale(const world::World& world);
// The locale the world was started in, before set_locale.
[[nodiscard]] std::string system_locale(const world::World& world);
// Changes the world's locale (standardized, as Godot's set_locale), and emits
// "translation_changed" (data {"locale"}) when it changed. Controls show the new language at
// once.
void set_locale(world::World& world, std::string_view locale);

// The language controls shape text in when they name none, as Godot's text server takes the
// locale: the world's locale in BCP 47 ("pt-BR"), which picks fonts' fallbacks by their
// language_support; "" until the project localizes (lists translations or a test locale).
[[nodiscard]] std::string text_language(const world::World& world);

// A message in the world's language, as Godot's tr: from the translations of the locale that
// matches best, then the fallback's, else the message itself; pseudolocalized when the settings
// say so. context tells apart messages written the same.
[[nodiscard]] std::string tr(const world::World& world, std::string_view message,
                             std::string_view context = {});
// The form of a message for n, as Godot's tr_n: plural for any n but 1 where nothing translates
// it.
[[nodiscard]] std::string tr_n(const world::World& world, std::string_view message,
                               std::string_view plural, int n, std::string_view context = {});

// The locales the world's translations are in, each once, in the order they load.
[[nodiscard]] std::vector<std::string> loaded_locales(const world::World& world);
// The translations loaded, in the order the settings list their files.
[[nodiscard]] std::vector<Translation> translations(const world::World& world);

// A problem with a translation file the settings list.
struct FileProblem {
    std::string file;
    int line = 0; // from 1; 0 for the whole file
    std::string message;
};
// What didn't load of the world's translation files.
[[nodiscard]] std::vector<FileProblem> problems(const world::World& world);

// Whether a control lays out right to left, as Godot's Control::is_layout_rtl: direction is its
// layout_direction ("inherited", "application_locale", "ltr", "rtl" or "system_locale"), parent
// its parent's, or nullptr at the root, where inheriting goes by root_node_layout_direction.
// With force_right_to_left_layout_direction, all but "ltr" are right to left.
[[nodiscard]] bool is_layout_rtl(const world::World& world, std::string_view direction,
                                 const bool* parent);

// Text as pseudo-localization makes it, as Godot's TranslationDomain::pseudolocalize.
[[nodiscard]] std::string pseudolocalize(const Pseudolocalization& settings,
                                         std::string_view message);

} // namespace slash::l10n
