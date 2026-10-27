#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/l10n/l10n.hpp"
#include "slash/l10n/locale.hpp"
#include "slash/l10n/translation.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Localization, as Godot's TranslationServer, Translation, TranslationLoaderPO and
// ResourceImporterCSVTranslation (core/string/translation*.cpp, core/string/plural_rules.cpp,
// editor/import/resource_importer_csv_translation.cpp) and the controls' auto translation
// (scene/main/node.cpp's can_auto_translate, scene/gui/control.cpp's is_layout_rtl).

namespace command = slash::command;
namespace json = slash::json;
namespace l10n = slash::l10n;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), std::string(text));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

void put32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFU);
    }
}

// A little-endian gettext .mo file of (source, translation) pairs, as msgfmt writes one.
std::string mo_file(const std::vector<std::pair<std::string, std::string>>& entries) {
    const auto count = static_cast<std::uint32_t>(entries.size());
    const std::uint32_t originals = 28;
    const std::uint32_t translations = originals + (count * 8);
    std::uint32_t data = translations + (count * 8);
    std::string out;
    put32(out, 0x950412DEU);
    put32(out, 0);
    put32(out, count);
    put32(out, originals);
    put32(out, translations);
    put32(out, 0);
    put32(out, data);
    std::string strings;
    std::string original_table;
    std::string translation_table;
    for (const auto& [source, translated] : entries) {
        put32(original_table, static_cast<std::uint32_t>(source.size()));
        put32(original_table, data + static_cast<std::uint32_t>(strings.size()));
        strings += source;
        strings += '\0';
    }
    for (const auto& [source, translated] : entries) {
        put32(translation_table, static_cast<std::uint32_t>(translated.size()));
        put32(translation_table, data + static_cast<std::uint32_t>(strings.size()));
        strings += translated;
        strings += '\0';
    }
    return out + original_table + translation_table + strings;
}

// A session on a project with these files, project.json's "internationalization" and a scene.
struct Project {
    std::filesystem::path root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string_view name, std::string_view settings,
            const std::vector<std::pair<std::string, std::string>>& files, std::string_view scene) {
        root = std::filesystem::path("l10n_test_") += std::string(name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("project.json", R"({"format": 1, "name": "Menu", "main_scene": "main.scene.json",
                                  "gui": {"allow_system_fallback": false},
                                  "window": {"width": 320, "height": 180},
                                  "internationalization": )" +
                                  std::string(settings) + "}");
        for (const auto& [file, text] : files) {
            write(file, text);
        }
        write("main.scene.json", R"({"format": 1, "entities": )" + std::string(scene) + "}");
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands, command::SessionOptions{.project_root = root.string()});
        REQUIRE(call("scene.open", R"({"path": "main.scene.json"})").ok);
    }

    void write(const std::string& file, const std::string& text) const {
        std::filesystem::create_directories((root / file).parent_path());
        REQUIRE(slash::platform::write_file((root / file).string(), text).ok());
    }

    command::Reply call(std::string_view name, std::string_view params) {
        command::Reply reply = session->call(name, parse(params));
        return reply;
    }

    // ui.layout's lines and rtl of each control, as "id: line|line (rtl)".
    std::string shown(std::string_view ids) {
        command::Reply reply = call("ui.layout", R"({"ids": )" + std::string(ids) + "}");
        REQUIRE_MESSAGE(reply.ok, reply.message);
        std::string out;
        for (const json::Value& control : reply.value.find("controls")->as_array()) {
            if (!out.empty()) {
                out += "; ";
            }
            out += control.find("id")->as_string() + ":";
            if (const json::Value* lines = control.find("lines"); lines != nullptr) {
                for (const json::Value& line : lines->as_array()) {
                    out += " " + line.as_string();
                }
            }
            if (const json::Value* rtl = control.find("rtl"); rtl != nullptr && rtl->as_bool()) {
                out += " (rtl)";
            }
        }
        return out;
    }

    world::World& world() { return session->world(); }
};

constexpr std::string_view kMenuCsv = "keys,fr,ar\n"
                                      "Play,Jouer,العب\n"
                                      "Quit,Quitter,خروج\n"
                                      "Hint,Astuce,تلميح\n"
                                      "Easy,Facile,سهل\n"
                                      "Hard,Difficile,صعب\n";

} // namespace

TEST_CASE("locales are standardized and compared as Godot's TranslationServer") {
    CHECK(l10n::standardize_locale("en-US") == "en_US");
    CHECK(l10n::standardize_locale("en-us") == "en"); // a country is upper case
    CHECK(l10n::standardize_locale("pt_BR") == "pt_BR");
    CHECK(l10n::standardize_locale("zh-TW", true) == "zh_Hant_TW");
    CHECK(l10n::standardize_locale("zh_Hans") == "zh_Hans");
    CHECK(l10n::standardize_locale("sr@latin") == "sr_Latn");
    // Old codes take their new names.
    CHECK(l10n::standardize_locale("iw") == "he");
    CHECK(l10n::standardize_locale("in_ID") == "id_ID");
    CHECK(l10n::standardize_locale("en_UK") == "en_GB");
    CHECK(l10n::standardize_locale("C") == "en");

    CHECK(l10n::compare_locales("fr", "fr") == 10);
    CHECK(l10n::compare_locales("es", "es_MX") == 5);
    CHECK(l10n::compare_locales("es_MX", "es_AR") == 4);
    CHECK(l10n::compare_locales("es_MX", "es_MX") == 10);
    CHECK(l10n::compare_locales("fr", "de") == 0);
    // Defaults are added: Taiwanese Chinese is Traditional, so zh_TW is zh_Hant_TW.
    CHECK(l10n::compare_locales("zh_TW", "zh_Hant_TW") == 10);
    CHECK(l10n::compare_locales("zh_TW", "zh_CN") < l10n::compare_locales("zh_TW", "zh_Hant"));

    CHECK(l10n::is_locale("fr"));
    CHECK(l10n::is_locale("fil_PH"));
    CHECK_FALSE(l10n::is_locale("french"));
    CHECK_FALSE(l10n::is_locale("f"));
    CHECK_FALSE(l10n::is_locale("zz!"));

    CHECK(l10n::is_locale_right_to_left("ar"));
    CHECK(l10n::is_locale_right_to_left("he_IL"));
    CHECK(l10n::is_locale_right_to_left("fa"));
    CHECK_FALSE(l10n::is_locale_right_to_left("en"));
    CHECK_FALSE(l10n::is_locale_right_to_left("ja"));
}

TEST_CASE("plural rules pick forms as gettext's and Godot's PluralRules") {
    auto polish = l10n::PluralRules::parse(l10n::plural_rules_for("pl"));
    REQUIRE_MESSAGE(polish.ok(), polish.error().message);
    CHECK(polish.value().nplurals() == 3);
    const std::vector<std::pair<int, int>> polish_forms{{1, 0},  {2, 1},  {4, 1},   {5, 2}, {12, 2},
                                                        {22, 1}, {25, 2}, {112, 2}, {0, 2}};
    for (const auto& [n, form] : polish_forms) {
        CHECK_MESSAGE(polish.value().evaluate(n) == form, n);
    }
    auto english = l10n::PluralRules::parse(l10n::plural_rules_for("en_US"));
    REQUIRE(english.ok());
    CHECK(english.value().nplurals() == 2);
    CHECK(english.value().evaluate(1) == 0);
    CHECK(english.value().evaluate(0) == 1);
    auto arabic = l10n::PluralRules::parse(l10n::plural_rules_for("ar"));
    REQUIRE(arabic.ok());
    CHECK(arabic.value().nplurals() == 6);
    CHECK(arabic.value().evaluate(0) == 0);
    CHECK(arabic.value().evaluate(1) == 1);
    CHECK(arabic.value().evaluate(2) == 2);
    CHECK(arabic.value().evaluate(5) == 3);
    CHECK(arabic.value().evaluate(11) == 4);
    CHECK(arabic.value().evaluate(100) == 5);
    // A locale with one form, such as Japanese.
    auto japanese = l10n::PluralRules::parse(l10n::plural_rules_for("ja"));
    REQUIRE(japanese.ok());
    CHECK(japanese.value().nplurals() == 1);
    CHECK(japanese.value().evaluate(7) == 0);

    CHECK_FALSE(l10n::PluralRules::parse("plural=n != 1;").ok());
    CHECK_FALSE(l10n::PluralRules::parse("nplurals=x; plural=n != 1;").ok());
}

TEST_CASE("translations find messages by context and pick plural forms") {
    l10n::Translation polish;
    polish.set_locale("pl-PL");
    CHECK(polish.locale() == "pl_PL");
    polish.add_message("Open", "Otwórz", "door");
    polish.add_message("Open", "Otwarte", "shop");
    polish.add_plural_message("%d coin", {"%d moneta", "%d monety", "%d monet"});
    REQUIRE(polish.message("Open", "door") != nullptr);
    CHECK(*polish.message("Open", "door") == "Otwórz");
    CHECK(*polish.message("Open", "shop") == "Otwarte");
    CHECK(polish.message("Open") == nullptr);
    CHECK(*polish.plural_message("%d coin", 1) == "%d moneta");
    CHECK(*polish.plural_message("%d coin", 3) == "%d monety");
    CHECK(*polish.plural_message("%d coin", 5) == "%d monet");
    CHECK(polish.plural_message("%d coin", -1) == nullptr);
    // A plural message read as a singular one gives its first form, as Godot's.
    CHECK(*polish.message("%d coin") == "%d moneta");

    // An override takes the place of the locale's own rule.
    polish.set_plural_rules_override("nplurals=2; plural=(n != 1);");
    CHECK(polish.plural_rules().nplurals() == 2);
    CHECK(*polish.plural_message("%d coin", 5) == "%d monety");
}

TEST_CASE("gettext .po and .mo files read as TranslationLoaderPO reads them") {
    const std::string po = R"(# A comment
msgid ""
msgstr ""
"Language: pl\n"
"Plural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\n"

msgid "Quit"
msgstr "Wyjdź"

#, fuzzy
msgid "Options"
msgstr "Opcje"

msgctxt "door"
msgid "Open"
msgstr ""
"Otw"
"órz"

msgid "Untranslated"
msgstr ""

msgid "Line\nbreak \"quoted\""
msgstr "Wiersz\nłamany \"cytat\""

msgid "%d coin"
msgid_plural "%d coins"
msgstr[0] "%d moneta"
msgstr[1] "%d monety"
msgstr[2] "%d monet"
)";
    auto read = l10n::read_po(po);
    REQUIRE_MESSAGE(read.ok(), read.error().message);
    REQUIRE(read.value().translations.size() == 1);
    const l10n::Translation& polish = read.value().translations[0];
    CHECK(polish.locale() == "pl");
    // The header line, as Godot keeps it; PluralRules reads from its "nplurals=".
    CHECK(polish.plural_rules_override().find("nplurals=3;") != std::string::npos);
    CHECK(*polish.message("Quit") == "Wyjdź");
    CHECK(polish.message("Options") == nullptr); // fuzzy entries are left out
    CHECK(*polish.message("Open", "door") == "Otwórz");
    // An empty msgstr is kept, and lookups take it as missing, as Godot's.
    REQUIRE(polish.message("Untranslated") != nullptr);
    CHECK(polish.message("Untranslated")->empty());
    CHECK(*polish.message("Line\nbreak \"quoted\"") == "Wiersz\nłamany \"cytat\"");
    CHECK(*polish.plural_message("%d coin", 22) == "%d monety");

    auto bad = l10n::read_po("msgid \"a\"\nmsgid \"b\"\nmsgstr \"c\"\n");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.starts_with("line 2:"));
    CHECK_FALSE(l10n::read_po("msgid \"a\nmsgstr \"b\"\n").ok());

    // The same as a compiled .mo file: the header, a context, then a plural message.
    const std::string mo = mo_file({
        {"", "Language: pl\nPlural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && "
             "(n%100<10 || n%100>=20) ? 1 : 2);\n"},
        {"%d coin" + std::string(1, '\0') + "%d coins",
         "%d moneta" + std::string(1, '\0') + "%d monety" + std::string(1, '\0') + "%d monet"},
        {"Quit", "Wyjdź"},
        {"door\x04Open", "Otwórz"},
    });
    auto compiled = l10n::read_mo(mo);
    REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
    REQUIRE(compiled.value().translations.size() == 1);
    const l10n::Translation& from_mo = compiled.value().translations[0];
    CHECK(from_mo.locale() == "pl");
    CHECK(*from_mo.message("Quit") == "Wyjdź");
    CHECK(*from_mo.message("Open", "door") == "Otwórz");
    CHECK(*from_mo.plural_message("%d coin", 12) == "%d monet");
    CHECK_FALSE(l10n::read_mo("not a mo file at all, though long enough").ok());
    CHECK_FALSE(l10n::read_mo(mo.substr(0, 40)).ok());

    auto by_name = l10n::read_translation_file("lang/pl.mo", mo);
    REQUIRE(by_name.ok());
    CHECK(by_name.value().translations[0].messages().size() == 3);
}

TEST_CASE("CSV files read as Godot's CSV translation importer") {
    const std::string csv = "keys,?context,fr,_notes,es_MX,?plural\n"
                            "Play,,Jouer,the title button,Jugar,\n"
                            "Open,door,Ouvrir,,Abrir,\n"
                            "Open,shop,Ouvert,,Abierto,\n"
                            "\"Say \"\"hi\"\"\",,\"Dis \"\"salut\"\"\",,Di\\thola,\n"
                            "Two\\nlines,,\"Deux\nlignes\",,Dos\\nlíneas,\n"
                            "%d apple,,%d pomme,,%d manzana,%d apples\n"
                            ",,%d pommes,,%d manzanas,\n"
                            "?pluralrule,,nplurals=2; plural=(n > 1);,,,\n";
    auto read = l10n::read_csv(csv);
    REQUIRE_MESSAGE(read.ok(), read.error().message);
    CHECK(read.value().problems.empty());
    REQUIRE(read.value().translations.size() == 2);
    const l10n::Translation& french = read.value().translations[0];
    const l10n::Translation& spanish = read.value().translations[1];
    CHECK(french.locale() == "fr");
    CHECK(spanish.locale() == "es_MX");
    CHECK(*french.message("Play") == "Jouer");
    CHECK(*french.message("Open", "door") == "Ouvrir");
    CHECK(*spanish.message("Open", "shop") == "Abierto");
    CHECK(*french.message("Say \"hi\"") == "Dis \"salut\"");
    // Translations are unescaped by default, keys not.
    CHECK(*spanish.message("Say \"hi\"") == "Di\thola");
    CHECK(*french.message("Two\\nlines") == "Deux\nlignes");
    CHECK(*spanish.message("Two\\nlines") == "Dos\nlíneas");
    CHECK(french.plural_rules_override() == "nplurals=2; plural=(n > 1);");
    CHECK(*french.plural_message("%d apple", 0) == "%d pomme"); // French's 0 is singular
    CHECK(*french.plural_message("%d apple", 2) == "%d pommes");
    CHECK(*spanish.plural_message("%d apple", 0) == "%d manzanas");

    auto keys =
        l10n::read_csv("keys;fr\nTwo\\nlines;Deux\\nlignes\n",
                       {.delimiter = ';', .unescape_keys = true, .unescape_translations = false});
    REQUIRE(keys.ok());
    CHECK(*keys.value().translations[0].message("Two\nlines") == "Deux\\nlignes");

    auto unnamed = l10n::read_csv("keys,fr,french\nA,B,C\n");
    REQUIRE(unnamed.ok());
    CHECK(unnamed.value().translations.size() == 1);
    REQUIRE(unnamed.value().problems.size() == 1);
    CHECK(unnamed.value().problems[0].line == 1);
    auto none = l10n::read_csv("keys\nA\n");
    REQUIRE(none.ok());
    CHECK(none.value().translations.empty());
    CHECK(none.value().problems.size() == 1);
}

TEST_CASE("pseudolocalization makes text as Godot's") {
    l10n::Pseudolocalization settings;
    CHECK(l10n::pseudolocalize(settings, "Start") == "[Ŝŧáŕŧ]");
    // Placeholders are kept.
    CHECK(l10n::pseudolocalize(settings, "%d left") == "[%d łéf́ŧ]");
    settings.skip_placeholders = false;
    CHECK(l10n::pseudolocalize(settings, "%d") == "[%d́]");
    settings = {};
    settings.replace_with_accents = false;
    settings.double_vowels = true;
    CHECK(l10n::pseudolocalize(settings, "Start game") == "[Staart gaamee]");
    settings.double_vowels = false;
    settings.expansion_ratio = 0.4;
    CHECK(l10n::pseudolocalize(settings, "Start game") == "[__Start game__]");
    settings.expansion_ratio = 0.0;
    settings.override = true;
    CHECK(l10n::pseudolocalize(settings, "Hi %s") == "[***%s]");
    settings.override = false;
    settings.fake_bidi = true;
    settings.prefix = "";
    settings.suffix = "";
    const std::string bidi = l10n::pseudolocalize(settings, "ab");
    CHECK(bidi.starts_with("\xE2\x80\xAE"));
    CHECK(bidi.ends_with("\xE2\x80\xAC"));
}

TEST_CASE("controls show their text translated in the game's locale") {
    Project project("controls", R"({"locale": {"translations": ["menu.csv"]}})",
                    {{"menu.csv", std::string(kMenuCsv)}},
                    R"({
        "menu": {"components": {"BoxContainer": {"vertical": true},
            "Control": {"anchor_right": 1.0, "anchor_bottom": 1.0,
                        "children": ["play", "quit", "box", "level", "hint"]}}},
        "play": {"components": {"Control": {}, "Button": {"text": "Play"}}},
        "quit": {"components": {"Control": {"auto_translate_mode": "disabled"},
                                "Button": {"text": "Quit"}}},
        "box": {"components": {"Control": {"auto_translate_mode": "disabled",
                                           "children": ["inner", "always"]},
                               "BoxContainer": {"vertical": true}}},
        "inner": {"components": {"Control": {}, "Label": {"text": "Play"}}},
        "always": {"components": {"Control": {"auto_translate_mode": "always"},
                                  "Label": {"text": "Play"}}},
        "level": {"components": {"Control": {}, "OptionButton": {"items": [
            {"text": "Easy"}, {"text": "Hard", "auto_translate_mode": "disabled"}],
            "selected": 0}}},
        "hint": {"components": {"Control": {"tooltip_text": "Hint"},
                                "LineEdit": {"placeholder_text": "Play"}}}
    })");
    const std::string ids = R"(["play", "quit", "inner", "always", "level"])";
    CHECK(project.shown(ids) == "play: Play; quit: Quit; inner: Play; always: Play; level: Easy");

    world::World& world = project.world();
    CHECK(l10n::locale(world) == "en");
    CHECK(l10n::tr(world, "Play") == "Play");
    // A game sets the locale from its options menu.
    l10n::set_locale(world, "fr_BE");
    CHECK(l10n::locale(world) == "fr_BE");
    CHECK(l10n::tr(world, "Play") == "Jouer");
    CHECK(l10n::tr(world, "Credits") == "Credits");
    // A disabled parent stops its children translating, unless one says always, as Godot's
    // can_auto_translate; an option's own mode overrides its button's.
    CHECK(project.shown(ids) ==
          "play: Jouer; quit: Quit; inner: Play; always: Jouer; level: Facile");
    REQUIRE(project.call("ui.popup", R"({"id": "level"})").ok);
    command::Reply layout = project.call("ui.layout", R"({"ids": ["level"]})");
    REQUIRE(layout.ok);
    const json::Value& items = *layout.value.find("popups")->as_array()[0].find("items");
    CHECK(items.as_array()[0].find("text")->as_string() == "Facile");
    CHECK(items.as_array()[1].find("text")->as_string() == "Hard");
    REQUIRE(project.call("ui.popup", R"({"id": "level", "open": false})").ok);

    // The tooltip and the placeholder translate too.
    REQUIRE(project.call("ui.tooltip", R"({"id": "hint"})").ok);
    layout = project.call("ui.layout", R"({"ids": ["hint"]})");
    REQUIRE(layout.ok);
    CHECK(layout.value.find("tooltip")->find("text")->as_string() == "Astuce");

    // Arabic lays out right to left, since the root follows the application's locale; text
    // keeps its own direction (text_direction "auto").
    l10n::set_locale(world, "ar");
    CHECK(project.shown(R"(["play", "quit"])") == "play: العب (rtl); quit: Quit");
    CHECK(l10n::text_language(world) == "ar");
}

TEST_CASE("the locale comes from the project, the player and the fallback") {
    Project tested("tested", R"({"locale": {"translations": ["menu.csv"], "test": "fr"}})",
                   {{"menu.csv", std::string(kMenuCsv)}},
                   R"({"play": {"components": {"Control": {}, "Label": {"text": "Play"}}}})");
    CHECK(l10n::locale(tested.world()) == "fr");
    CHECK(tested.shown(R"(["play"])") == "play: Jouer");

    // A game starts in the player's locale; sessions in the fallback, so they repeat.
    world::ComponentRegistry components;
    world::World world{components};
    slash::asset::Assets assets{{.read = [](std::string_view) -> slash::Result<std::string> {
                                     return slash::make_error(slash::ErrorCode::NotFound, "none");
                                 },
                                 .stamp = {}}};
    l10n::Settings settings;
    l10n::install(world, assets, settings, "de-AT");
    CHECK(l10n::locale(world) == "de_AT");
    CHECK(l10n::system_locale(world) == "de_AT");
    CHECK(l10n::text_language(world).empty()); // not localized: text shapes as before
    l10n::set_locale(world, "it");
    // Installing again with the same test locale keeps the one set.
    l10n::install(world, assets, settings, "en");
    CHECK(l10n::locale(world) == "it");
    settings.test = "ja";
    l10n::install(world, assets, settings, "en");
    CHECK(l10n::locale(world) == "ja");
    CHECK(l10n::text_language(world) == "ja");
    // A world with no translation server returns messages as written.
    world::World bare{components};
    CHECK(l10n::tr(bare, "Play") == "Play");
    CHECK(l10n::tr_n(bare, "%d coin", "%d coins", 2) == "%d coins");
    CHECK(l10n::locale(bare) == "en");
}

TEST_CASE("lookups take the best locale, then the fallback, then the source") {
    const std::string spanish = "keys,es,es_AR\nCar,Coche,Auto\nBus,Autobús,\n";
    const std::string english = "keys,en\nCar,Automobile\nTruck,Lorry\n";
    // Written in Japanese and translated into English, the fallback.
    const std::string japanese = "keys,en\nはじめる,Start\n";
    Project project("fallback",
                    R"({"locale": {"translations": ["es.csv", "en.csv", "ja.csv"],
                                   "fallback": "en"}})",
                    {{"es.csv", spanish}, {"en.csv", english}, {"ja.csv", japanese}}, "{}");
    world::World& world = project.world();
    CHECK(compact(json::Value(json::Array{"es", "es_AR", "en"})) ==
          compact(*project.call("l10n.state", "{}").value.find("locales")));
    l10n::set_locale(world, "es_MX");
    CHECK(l10n::tr(world, "Car") == "Coche"); // es scores 5, es_AR 4
    l10n::set_locale(world, "es_AR");
    CHECK(l10n::tr(world, "Car") == "Auto");
    // The best match lacks it, so the next best answers, as Godot's.
    CHECK(l10n::tr(world, "Bus") == "Autobús");
    CHECK(l10n::tr(world, "Truck") == "Lorry");
    l10n::set_locale(world, "ja");
    CHECK(l10n::tr(world, "はじめる") == "Start");
    l10n::set_locale(world, "en_GB");
    CHECK(l10n::tr(world, "Car") == "Automobile");
}

TEST_CASE("plural lookups and pseudolocalization in a session") {
    const std::string po = "msgid \"\"\nmsgstr \"Language: pl\\n\"\n"
                           "\"Plural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && "
                           "(n%100<10 || n%100>=20) ? 1 : 2);\\n\"\n\n"
                           "msgid \"%d coin\"\nmsgid_plural \"%d coins\"\n"
                           "msgstr[0] \"%d moneta\"\nmsgstr[1] \"%d monety\"\n"
                           "msgstr[2] \"%d monet\"\n";
    Project project("plurals",
                    R"({"locale": {"translations": ["pl.po"], "test": "pl"},
                        "pseudolocalization": {"use_pseudolocalization": true}})",
                    {{"pl.po", po}},
                    R"({"title": {"components": {"Control": {}, "Label": {"text": "Start"}}}})");
    world::World& world = project.world();
    CHECK(l10n::tr_n(world, "%d coin", "%d coins", 1) == "%d moneta");
    CHECK(l10n::tr_n(world, "%d coin", "%d coins", 14) == "%d monet");
    CHECK(l10n::tr_n(world, "%d cat", "%d cats", 1) == "%d cat");
    CHECK(l10n::tr_n(world, "%d cat", "%d cats", 2) == "%d cats");
    // tr pseudolocalizes, as Godot's; tr_n doesn't.
    CHECK(l10n::tr(world, "Start") == "[Ŝŧáŕŧ]");
    CHECK(project.shown(R"(["title"])") == "title: [Ŝŧáŕŧ]");
    command::Reply reply = project.call(
        "l10n.state", R"({"tr": [{"message": "%d coin", "plural": "%d coins", "n": 3}, "Start"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(compact(*reply.value.find("tr")) == R"(["%d monety","[Ŝŧáŕŧ]"])");
    CHECK(reply.value.find("pseudolocalization")->as_bool());
}

TEST_CASE("layout direction follows Godot's root and forced settings") {
    const std::string scene = R"({
        "root": {"components": {"Control": {"children": ["ltr", "inherits"]}, "BoxContainer": {}}},
        "ltr": {"components": {"Control": {"layout_direction": "ltr"}, "Label": {"text": "A"}}},
        "inherits": {"components": {"Control": {}, "Label": {"text": "B"}}}
    })";
    // A right-to-left box places its first child on the right.
    const auto first_on_right = [](Project& project) {
        command::Reply reply = project.call("ui.layout", R"({"ids": ["ltr", "inherits"]})");
        REQUIRE_MESSAGE(reply.ok, reply.message);
        const json::Array& controls = reply.value.find("controls")->as_array();
        return controls[0].find("rect")->as_array()[0].as_double() >
               controls[1].find("rect")->as_array()[0].as_double();
    };
    Project rtl("rtl", R"({"rendering": {"root_node_layout_direction": "rtl"}})", {}, scene);
    CHECK(first_on_right(rtl));
    CHECK(l10n::is_layout_rtl(rtl.world(), "inherited", nullptr));
    CHECK_FALSE(l10n::is_layout_rtl(rtl.world(), "ltr", nullptr));
    Project forced("forced", R"({"rendering": {"force_right_to_left_layout_direction": true}})", {},
                   scene);
    CHECK(first_on_right(forced));
    CHECK(l10n::is_layout_rtl(forced.world(), "application_locale", nullptr));
    CHECK_FALSE(l10n::is_layout_rtl(forced.world(), "ltr", nullptr));
    Project normal("normal", "{}", {}, scene);
    CHECK_FALSE(first_on_right(normal));
    // system_locale goes by the player's locale, which sessions take as the fallback.
    world::World& world = normal.world();
    CHECK_FALSE(l10n::is_layout_rtl(world, "system_locale", nullptr));
    l10n::set_locale(world, "he");
    CHECK(l10n::is_layout_rtl(world, "application_locale", nullptr));
    CHECK_FALSE(l10n::is_layout_rtl(world, "system_locale", nullptr));
    const bool parent = false;
    CHECK_FALSE(l10n::is_layout_rtl(world, "inherited", &parent));
    CHECK(l10n::is_layout_rtl(world, "rtl", &parent));
}

TEST_CASE("internationalization settings validate and translation files report problems") {
    auto errors = l10n::validate_settings(parse(R"({"locale": {"fallback": 3},
                  "rendering": {"root_node_layout_direction": "rtl_please"}})"),
                                          "/internationalization");
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/internationalization/locale/fallback");
    CHECK(errors[1].path == "/internationalization/rendering/root_node_layout_direction");
    errors = l10n::validate_settings(
        parse(R"({"locale": {"translations": ["menu.txt", "../out.csv", "ok.mo"]}})"), "/i");
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/i/locale/translations/0");
    CHECK(errors[1].path == "/i/locale/translations/1");
    const l10n::Settings parsed = l10n::parse_settings(
        parse(R"({"locale": {"translations": ["a.po"], "fallback": "de", "test": "fr"},
                  "pseudolocalization": {"use_pseudolocalization": true, "expansion_ratio": 0.3},
                  "rendering": {"root_node_layout_direction": "system_locale"}})"));
    CHECK(parsed.translations == std::vector<std::string>{"a.po"});
    CHECK(parsed.fallback == "de");
    CHECK(parsed.test == "fr");
    CHECK(parsed.pseudolocalization.use_pseudolocalization);
    CHECK(parsed.pseudolocalization.expansion_ratio == doctest::Approx(0.3));
    CHECK(parsed.root_node_layout_direction == l10n::RootDirection::SystemLocale);

    Project project("problems", R"({"locale": {"translations": ["bad.csv", "gone.po"]}})",
                    {{"bad.csv", "keys,fr,french\nA,B,C\n"}}, "{}");
    command::Reply reply = project.call("l10n.state", "{}");
    REQUIRE(reply.ok);
    const json::Value& problems = *reply.value.find("problems");
    REQUIRE(problems.as_array().size() == 2);
    CHECK(problems.as_array()[0].find("file")->as_string() == "bad.csv");
    CHECK(problems.as_array()[0].find("line")->as_int() == 1);
    CHECK(problems.as_array()[1].find("file")->as_string() == "gone.po");
}

TEST_CASE("l10n.set_locale switches the game's language") {
    Project project("set", R"({"locale": {"translations": ["menu.csv"]}})",
                    {{"menu.csv", std::string(kMenuCsv)}},
                    R"({"play": {"components": {"Control": {}, "Label": {"text": "Play"}}}})");
    REQUIRE(project.call("events.subscribe", R"({"kinds": ["translation_changed"]})").ok);
    command::Reply reply = project.call("l10n.set_locale", R"({"locale": "fr-CA"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(compact(reply.value) == R"({"locale":"fr_CA","was":"en"})");
    CHECK(project.shown(R"(["play"])") == "play: Jouer");
    reply = project.call("l10n.set_locale", R"({"locale": "fr_CA"})");
    CHECK(compact(reply.value) == R"({"locale":"fr_CA"})");
    reply = project.call("l10n.set_locale", R"({"locale": "de"})");
    REQUIRE(reply.ok);
    CHECK(reply.value.find("warning") != nullptr);
    reply = project.call("l10n.set_locale", R"({"locale": "german"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidParams);
    CHECK(reply.data.find("path")->as_string() == "/locale");
    REQUIRE(project.call("step", "{}").ok);
    reply = project.call("events.read", R"({"subscription": "events_1"})");
    REQUIRE(reply.ok);
    CHECK(reply.value.find("events")->as_array().size() == 2);
}
