#pragma once

#include <string>
#include <string_view>

// Locale codes as Godot's TranslationServer reads them: language_Script_COUNTRY_VARIANT, such as
// "en", "pt_BR", "zh_Hant_TW" or "ca_ES_valencia". Each function ports the one of the same name
// in Godot 4.6's core/string/translation_server.cpp, with the tables of core/string/locales.h.
namespace slash::l10n {

// A locale's parts, as Godot's TranslationServer::Locale reads them: "-" read as "_" (macOS and
// BCP 47 style), Windows' old language codes renamed ("iw" is "he"), old country codes renamed
// ("UK" is "GB"), scripts Godot doesn't know dropped, and an "@cyrillic", "@latin" or
// "@devanagari" extra read as the script. With add_defaults, a language written in more than one
// script gets the one its country uses ("sr" is "sr_Cyrl", "zh_TW" is "zh_Hant_TW") and, with
// that script and no country, the country it is mostly used in ("zh_Hans" is "zh_Hans_CN").
struct Locale {
    std::string language;
    std::string script;
    std::string country;
    std::string variant;

    [[nodiscard]] static Locale parse(std::string_view text, bool add_defaults);
    // The parts joined with "_", as Godot's Locale::operator String.
    [[nodiscard]] std::string str() const;

    friend bool operator==(const Locale&, const Locale&) = default;
};

// The locale in Godot's form, as TranslationServer::standardize_locale: "en-us" is "en",
// "pt-BR" is "pt_BR", "zh-TW" with add_defaults is "zh_Hant_TW".
[[nodiscard]] std::string standardize_locale(std::string_view locale, bool add_defaults = false);

// Whether a locale names a language: two or three letters (ISO 639), as every language Godot
// knows, before its script, country and variant.
[[nodiscard]] bool is_locale(std::string_view locale);

// How well two locales match, as TranslationServer::compare_locales: 10 for the same locale
// (with defaults added), 0 for different languages, else 5, one more for each of script, country
// and variant both have and share and one less for each both have and don't.
[[nodiscard]] int compare_locales(std::string_view a, std::string_view b);

// The gettext plural rule of a locale, as TranslationServer::get_plural_rules: the locale's own,
// its language and country's, or its language's ("nplurals=2; plural=(n != 1);" for "en"); ""
// when there is none.
[[nodiscard]] std::string plural_rules_for(std::string_view locale);

// Whether a locale's language is written right to left, as TextServerAdvanced's
// is_locale_right_to_left: Arabic, Dhivehi, Hebrew, Persian, Fula, Kurdish and Urdu.
[[nodiscard]] bool is_locale_right_to_left(std::string_view locale);

} // namespace slash::l10n
