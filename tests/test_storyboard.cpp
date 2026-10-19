#include "slash/core/json.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace storyboard = slash::storyboard;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The path and message of each error, as "path: message".
std::vector<std::string> describe(const std::vector<slash::json::ValidationError>& errors) {
    std::vector<std::string> out;
    out.reserve(errors.size());
    for (const auto& error : errors) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

// A small cast: Mira, her faction and a place.
json::Object cast() {
    return parse(R"({
    "character/mira": {"name": "Mira", "faction": "faction/couriers", "summary": "A courier.",
                       "links": ["scenes/lowtown.scene.json"]},
    "faction/couriers": {"name": "The Couriers", "leader": "character/mira"},
    "place/docks": {"name": "The Docks", "text": "Fog every morning."}
})")
        .as_object();
}

} // namespace

TEST_CASE("storyboard IDs are a known kind and a lowercase name") {
    CHECK(storyboard::id_error("character/mira").empty());
    CHECK(storyboard::id_error("task/fix-jump_2").empty());
    CHECK(contains(storyboard::id_error("mira"), "a kind and a lowercase name"));
    CHECK(contains(storyboard::id_error("charcter/mira"), "did you mean \"character\"?"));
    CHECK(contains(storyboard::id_error("zzzzzzzz/mira"), "kinds are palette, tone"));
    CHECK(contains(storyboard::id_error("character/Mira"), "lowercase letters"));
    CHECK(contains(storyboard::id_error("character/"), "1 to 64"));
    CHECK(contains(storyboard::id_error("character/a/b"), "lowercase letters"));
    CHECK(storyboard::kind_of("quest/escape")->file == "story");
    CHECK(storyboard::kind_of("quest") == nullptr);
    CHECK(storyboard::file_path("world") == "storyboard/world.json");
}

TEST_CASE("storyboard files check every entry against its kind") {
    CHECK(storyboard::validate_file("world", json::Value(json::Object{
                                                 {"entries", cast()},
                                                 {"format", 1},
                                             }))
              .empty());

    const json::Value document = parse(R"({"format": 2, "extra": 1, "entries": {
        "character/mira": {"name": "Mira", "fation": "faction/couriers", "faction": "couriers",
                           "links": ["../secret.txt"], "traits": "brave"},
        "place/docks": {"summary": "No name."},
        "task/fix": {"name": "Fix it"},
        "Character/bad": {"name": "Bad"}
    }})");
    CHECK(describe(storyboard::validate_file("world", document)) ==
          std::vector<std::string>{
              "/extra: unknown property \"extra\"",
              "/format: expected 1, got 2",
              "/entries/Character~1bad: unknown kind \"Character\" in \"Character/bad\"; did you "
              "mean \"character\"?",
              "/entries/character~1mira/fation: unknown property \"fation\"; did you mean "
              "\"faction\"?",
              "/entries/character~1mira/traits: expected array, got string",
              "/entries/character~1mira/links/0: links are paths relative to the project folder, "
              "separated by '/', with no \".\" or \"..\" parts; got \"../secret.txt\"",
              "/entries/character~1mira/faction: storyboard IDs are a kind and a lowercase name, "
              "such as \"character/mira\"; got \"couriers\"",
              "/entries/place~1docks: missing required property \"name\"",
              "/entries/task~1fix: task entries belong in storyboard/work.json",
          });

    const json::Value style = parse(R"({"format": 1, "entries": {
        "palette/main": {"name": "Main", "colors": {"sky": "#87ceeb", "bad": "red"}},
        "budget/props": {"name": "Props", "triangles": 1.5}
    }})");
    CHECK(describe(storyboard::validate_file("style", style)) ==
          std::vector<std::string>{
              "/entries/budget~1props/triangles: expected integer, got number",
              "/entries/palette~1main/colors/bad: colors are written \"#rrggbb\", e.g. "
              "\"#87ceeb\"; got \"red\"",
          });

    const json::Value work = parse(R"({"format": 1, "entries": {
        "task/a": {"name": "A", "status": "started"}}})");
    const auto errors = storyboard::validate_file("work", work);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entries/task~1a/status");
}

TEST_CASE("storyboard files have one canonical text, and the schema describes them") {
    CHECK(storyboard::format_file(
              parse(R"({"place/b": {"name": "B"}, "place/a": {"name": "A"}})").as_object()) == R"({
  "entries": {
    "place/a": {
      "name": "A"
    },
    "place/b": {
      "name": "B"
    }
  },
  "format": 1
}
)");
    const json::Value schema = storyboard::file_schema();
    CHECK(schema.find("title")->as_string() == "SlashEngine storyboard file");
    const json::Value& kinds =
        *schema.find("properties")->find("entries")->find("patternProperties");
    CHECK(kinds.size() == storyboard::kinds().size());
    CHECK(kinds.find("^character/")->find("properties")->find("faction") != nullptr);
}

TEST_CASE("an entry's version changes when its fields do") {
    const json::Value mira = parse(R"({"name": "Mira", "voice": "dry"})");
    const std::string version = storyboard::version(mira);
    CHECK(version.size() == 8);
    CHECK(version.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(storyboard::version(parse(R"({"voice": "dry", "name": "Mira"})")) == version);
    CHECK(storyboard::version(parse(R"({"name": "Mira", "voice": "warm"})")) != version);
}

TEST_CASE("edits create, change and remove entries, all or nothing") {
    json::Object entries = cast();
    const std::string mira = storyboard::version(entries.at("character/mira"));
    const std::string docks = storyboard::version(entries.at("place/docks"));

    storyboard::EditResult result = storyboard::apply_edits(
        entries, parse(R"({"edits": [
            {"id": "character/mira", "version": ")" +
                       mira + R"(", "set": {"home": "place/docks", "summary": null}},
            {"id": "item/key", "version": null, "set": {"name": "Brass key",
                                                        "owner": "character/mira"}}
        ]})")
                     .find("edits")
                     ->as_array());
    REQUIRE(result.errors.empty());
    CHECK(entries.at("character/mira") == parse(R"({"name": "Mira", "faction": "faction/couriers",
        "home": "place/docks", "links": ["scenes/lowtown.scene.json"]})"));
    CHECK(entries.at("item/key").find("owner")->as_string() == "character/mira");
    CHECK(result.previous.at("character/mira") == cast().at("character/mira"));
    CHECK(result.previous.at("item/key").is_null());

    // Undoing applies the reverse edits, which restore every entry exactly.
    json::Object undone = entries;
    const json::Value reverse(storyboard::reverse_edits(entries, result.previous));
    REQUIRE(storyboard::apply_edits(undone, reverse).errors.empty());
    CHECK(undone == cast());

    // Any error leaves every entry as it was, and every error is reported.
    const json::Object before = entries;
    result = storyboard::apply_edits(entries, parse(R"([
        {"id": "place/docks", "version": ")" + docks +
                                                    R"(", "set": {"region": "place/lowtwn"}},
        {"id": "character/mira", "version": ")" + mira +
                                                    R"(", "set": {"voice": "dry"}},
        {"id": "faction/couriers", "version": null, "set": {"name": "Again"}},
        {"id": "character/nobody", "version": "0000000a", "set": {"name": "Nobody"}},
        {"id": "item/key", "version": null, "remove": true},
        {"id": "place/docks", "version": null, "set": {}},
        {"id": "beat/one", "version": null, "set": {"name": "One", "place": "character/mira"}},
        {"id": "beat/two", "version": null}
    ])"));
    CHECK(entries == before);
    CHECK(describe(result.errors) ==
          std::vector<std::string>{
              "/edits/1/version: \"character/mira\" changed since version " + mira +
                  " was read; it is now " + storyboard::version(entries.at("character/mira")) +
                  " (see current), so edit that version",
              "/edits/2/version: \"faction/couriers\" already exists, at version " +
                  storyboard::version(entries.at("faction/couriers")) +
                  "; pass that version to change it",
              "/edits/3/id: unknown storyboard entry \"character/nobody\"; pass \"version\": "
              "null to create it",
              "/edits/4/version: \"item/key\" already exists, at version " +
                  storyboard::version(entries.at("item/key")) + "; pass that version to change it",
              "/edits/5/id: \"place/docks\" is edited twice; edit each entry once per call",
              "/edits/7: give set, or \"remove\": true",
              "/edits/0/set/region: unknown storyboard entry \"place/lowtwn\"",
              "/edits/6/set/place: expected a place entry, got \"character/mira\"",
          });
    CHECK(result.current == json::Value(json::Object{
                                {"fields", entries.at("character/mira")},
                                {"id", "character/mira"},
                                {"version", storyboard::version(entries.at("character/mira"))},
                            }));

    // An entry other entries still name cannot be removed on its own.
    const std::string couriers = storyboard::version(entries.at("faction/couriers"));
    result = storyboard::apply_edits(entries, parse(R"([{"id": "faction/couriers", "version": ")" +
                                                    couriers + R"(", "remove": true}])"));
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].path == "/edits/0/remove");
    CHECK(contains(result.errors[0].message, "still named by \"character/mira\" at /faction"));
}

TEST_CASE("queries filter entries by ID, kind, text and links") {
    const auto ids = [](const json::Array& found) {
        std::vector<std::string> out;
        for (const json::Value& item : found) {
            out.push_back(item.find("id")->as_string());
        }
        return out;
    };
    CHECK(ids(storyboard::query(cast(), {})) ==
          std::vector<std::string>{"character/mira", "faction/couriers", "place/docks"});
    CHECK(ids(storyboard::query(cast(), {.kind = "place"})) ==
          std::vector<std::string>{"place/docks"});
    CHECK(ids(storyboard::query(cast(), {.text = "FOG"})) ==
          std::vector<std::string>{"place/docks"});
    CHECK(ids(storyboard::query(cast(), {.linked = "scenes/lowtown.scene.json"})) ==
          std::vector<std::string>{"character/mira"});
    CHECK(ids(storyboard::query(cast(), {.linked = "character/mira"})) ==
          std::vector<std::string>{"faction/couriers"});

    const json::Array mira = storyboard::query(cast(), {.ids = {"character/mira"}});
    REQUIRE(mira.size() == 1);
    CHECK(mira[0] == json::Value(json::Object{
                         {"fields", parse(R"({"name": "Mira", "summary": "A courier."})")},
                         {"id", "character/mira"},
                         {"version", storyboard::version(cast().at("character/mira"))},
                     }));
    const json::Array full = storyboard::query(cast(), {.ids = {"character/mira"}, .full = true});
    CHECK(*full[0].find("fields") == cast().at("character/mira"));
}

TEST_CASE("load reads every storyboard file and reports the invalid ones") {
    const std::map<std::string, std::string, std::less<>> files = {
        {"storyboard/world.json", R"({"format": 1, "entries": {"place/docks": {"name": "D"}}})"},
        {"storyboard/story.json", R"({"format": 1, "entries": {"beat/x": {}}})"},
        {"storyboard/design.json", "{"},
    };
    const auto loaded =
        storyboard::load([&](std::string_view path) -> slash::Result<std::optional<std::string>> {
            const auto it = files.find(path);
            return it != files.end() ? std::optional<std::string>(it->second) : std::nullopt;
        });
    REQUIRE(loaded.ok());
    CHECK(loaded.value().entries.size() == 2);
    CHECK(loaded.value().entries.contains("place/docks"));
    REQUIRE(loaded.value().errors.size() == 2);
    CHECK(loaded.value().errors[0].file == "storyboard/story.json");
    CHECK(loaded.value().errors[0].path == "/entries/beat~1x");
    CHECK(loaded.value().errors[1].file == "storyboard/design.json");
}

TEST_CASE("journal lines record who changed what, when and why") {
    CHECK(storyboard::format_time(0) == "1970-01-01T00:00:00Z");
    CHECK(storyboard::format_time(951782400) == "2000-02-29T00:00:00Z");
    CHECK(storyboard::format_time(1760497916) == "2025-10-15T03:11:56Z");
    CHECK(storyboard::format_time(-1) == "1969-12-31T23:59:59Z");

    const std::string line = storyboard::journal_line(
        "codex", "Mira leaves",
        parse(R"({"character/mira": "0badf00d", "item/key": null})").as_object(), 0);
    CHECK(line == R"({"by":"codex","changes":{"character/mira":"0badf00d","item/key":null},)"
                  R"("time":"1970-01-01T00:00:00Z","why":"Mira leaves"})");
    CHECK(storyboard::journal_length("") == 0);
    CHECK(storyboard::journal_length(line + "\n" + line + "\n") == 2);
    CHECK(storyboard::journal_length(line) == 1);
}

TEST_CASE("check finds broken references, avoided words and missing links") {
    json::Object entries = cast();
    entries["term/couriers"] = parse(R"({"name": "Couriers", "avoid": ["Runners", "mail"],
                                         "summary": "Never call them runners."})");
    entries["beat/arrival"] = parse(R"({"name": "Arrival", "place": "place/dock",
                                        "summary": "The RUNNERS land; e-mail and mailboxes stay.",
                                        "links": ["scenes/lowtown.scene.json", "scenes/new.scene.json",
                                                  "https://example.com/art"]})");
    entries["quest/delivery"] = parse(R"({"name": "Delivery", "steps": ["Find the mail"]})");
    const auto exists = [](std::string_view path) { return path == "scenes/lowtown.scene.json"; };

    const storyboard::CheckResult result = storyboard::check(entries, exists);
    std::vector<std::string> errors;
    for (const auto& error : result.errors) {
        errors.push_back(error.file + " " + error.path + ": " + error.message);
    }
    REQUIRE(errors.size() == 4);
    CHECK(contains(errors[0], "storyboard/story.json /entries/beat~1arrival/place: "));
    CHECK(contains(errors[0], "did you mean \"place/docks\"?"));
    CHECK(errors[1] == "storyboard/story.json /entries/beat~1arrival/summary: uses \"Runners\", "
                       "which term/couriers says to avoid; write \"Couriers\"");
    // "e-mail" holds "mail" as a whole word; "mailboxes" does not.
    CHECK(errors[2] == "storyboard/story.json /entries/beat~1arrival/summary: uses \"mail\", "
                       "which term/couriers says to avoid; write \"Couriers\"");
    CHECK(errors[3] == "storyboard/story.json /entries/quest~1delivery/steps/0: uses \"mail\", "
                       "which term/couriers says to avoid; write \"Couriers\"");

    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].path == "/entries/beat~1arrival/links/1");
    CHECK(result.warnings[0].message == "links to \"scenes/new.scene.json\", which does not exist");

    CHECK(storyboard::check(cast(), exists).errors.empty());
}

TEST_CASE("StoryRef IDs in scenes and prefabs must name entries") {
    const json::Value scene = parse(R"({"entities": {
        "hero": {"components": {"StoryRef": {"ids": ["character/mira", "character/mia"]}}},
        "crate": {"components": {"Transform2D": {}}},
        "sign": {"components": {"StoryRef": {"ids": ["Place/docks"]}}}
    }, "format": 1})");
    const std::vector<storyboard::StoryRefId> refs = storyboard::story_refs(scene);
    REQUIRE(refs.size() == 3);
    CHECK(refs[0].id == "character/mira");
    CHECK(refs[0].path == "/entities/hero/components/StoryRef/ids/0");
    CHECK(refs[2].path == "/entities/sign/components/StoryRef/ids/0");

    std::vector<storyboard::FileError> errors;
    storyboard::check_story_refs(cast(), "scenes/lowtown.scene.json", scene, errors);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].file == "scenes/lowtown.scene.json");
    CHECK(errors[0].path == "/entities/hero/components/StoryRef/ids/1");
    CHECK(contains(errors[0].message, "did you mean \"character/mira\"?"));
    CHECK(errors[1].message ==
          "unknown kind \"Place\" in \"Place/docks\"; did you mean \"place\"?");

    const json::Value prefab =
        parse(R"({"components": {"StoryRef": {"ids": ["place/docks"]}}, "format": 1})");
    REQUIRE(storyboard::story_refs(prefab).size() == 1);
    CHECK(storyboard::story_refs(prefab)[0].path == "/components/StoryRef/ids/0");
    errors.clear();
    storyboard::check_story_refs(cast(), "prefabs/sign.prefab.json", prefab, errors);
    CHECK(errors.empty());
}

TEST_CASE("a brief holds the entries a task touches and names the rest") {
    json::Object entries = cast();
    entries["rule/one_hit"] =
        parse(R"({"name": "One hit", "summary": "Anything dies in one hit."})");
    entries["rule/boats"] = parse(R"({"name": "Boats", "links": ["scenes/harbor.scene.json"]})");
    entries["task/mira-voice"] =
        parse(R"({"name": "Record Mira", "refs": ["character/mira"], "status": "open"})");
    entries["task/old"] =
        parse(R"({"name": "Old work", "refs": ["character/mira"], "status": "done"})");
    entries["decision/fog"] =
        parse(R"({"name": "Fog", "links": ["scenes/lowtown.scene.json"], "status": "superseded"})");

    const auto ids = [](const json::Array& items) {
        std::vector<std::string> out;
        for (const json::Value& item : items) {
            out.push_back(item.find("id")->as_string());
        }
        return out;
    };

    const storyboard::Brief by_path = storyboard::brief(entries, {}, {"scenes/lowtown.scene.json"});
    CHECK(ids(by_path.entries) ==
          std::vector<std::string>{"character/mira", "rule/one_hit", "task/mira-voice"});
    CHECK(ids(by_path.related) == std::vector<std::string>{"faction/couriers"});
    CHECK(by_path.entries[0].find("fields")->find("links") != nullptr);
    const json::Value& couriers = *by_path.related[0].find("fields");
    CHECK(couriers.find("leader") == nullptr);
    CHECK(couriers.find("name")->as_string() == "The Couriers");

    const storyboard::Brief by_id =
        storyboard::brief(entries, {"place/docks", "place/nowhere"}, {});
    CHECK(ids(by_id.entries) == std::vector<std::string>{"place/docks", "rule/one_hit"});
    CHECK(by_id.related.empty());
}

TEST_CASE("a link to a folder covers what is in it") {
    json::Object entries = cast();
    entries["palette/dock"] = parse(R"({"name": "Dock", "colors": {"tar": "#202020"},
                                       "links": ["art/docks"]})");
    const auto ids = [](const json::Array& items) {
        std::vector<std::string> out;
        for (const json::Value& item : items) {
            out.push_back(item.find("id")->as_string());
        }
        return out;
    };
    CHECK(ids(storyboard::query(entries, {.linked = "art/docks/crate.png"})) ==
          std::vector<std::string>{"palette/dock"});
    CHECK(ids(storyboard::query(entries, {.linked = "art/docks"})) ==
          std::vector<std::string>{"palette/dock"});
    CHECK(ids(storyboard::query(entries, {.linked = "art/docks2/crate.png"})).empty());
    CHECK(ids(storyboard::brief(entries, {}, {"art/docks/rope.png"}).entries) ==
          std::vector<std::string>{"palette/dock"});
}

TEST_CASE("textures keep to the palettes and budgets that cover them") {
    // Textures of 2x2 pixels: row by row, RGBA.
    const auto texture = [](std::uint32_t width, std::uint32_t height,
                            std::vector<std::uint8_t> pixels) {
        return storyboard::Texture{.width = width, .height = height, .pixels = std::move(pixels)};
    };
    std::map<std::string, storyboard::Texture, std::less<>> textures = {
        // On the palette, with a transparent pixel of any color.
        {"art/sprites/hero.png",
         texture(2, 2, {0, 0, 0, 255, 255, 0, 77, 255, 9, 9, 9, 0, 255, 241, 232, 128})},
        // Two colors off it, one of them twice; one within the second palette's tolerance.
        {"art/sprites/crate.png",
         texture(2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255, 36, 30, 44, 255})},
        {"art/title.png", texture(512, 128, {})},
        {"ui/icon.png", texture(64, 64, {})},
    };
    json::Object entries = cast();
    entries["budget/textures"] = parse(R"({"name": "Textures", "texture_size": 256})");
    entries["budget/ui"] =
        parse(R"({"name": "UI", "texture_size": 32, "links": ["ui", "missing/folder"]})");
    entries["budget/models"] = parse(R"({"name": "Models", "triangles": 500})");
    entries["palette/pico"] = parse(R"({"name": "PICO-8", "links": ["art/sprites"],
        "colors": {"black": "#000000", "red": "#FF004D", "white": "#fff1e8"}})");
    entries["palette/stone"] = parse(R"({"name": "Stone", "links": ["art/sprites/crate.png"],
        "colors": {"stone": "#202428"}, "tolerance": 8})");
    // With no links, a palette guides the art but checks nothing.
    entries["palette/mood"] = parse(R"({"name": "Mood", "colors": {"teal": "#008080"}})");

    std::vector<std::string> loaded;
    const auto load = [&](std::string_view path) -> std::optional<storyboard::Texture> {
        loaded.emplace_back(path);
        const auto found = textures.find(path);
        if (found == textures.end()) {
            return std::nullopt;
        }
        return found->second;
    };
    std::vector<storyboard::FileError> errors;
    storyboard::check_textures(
        entries, {"art/sprites/hero.png", "art/sprites/crate.png", "art/title.png", "ui/icon.png"},
        load, errors);
    std::vector<std::string> found;
    for (const storyboard::FileError& error : errors) {
        CHECK(error.path.empty());
        found.push_back(error.file + " " + error.message);
    }
    CHECK(found ==
          std::vector<std::string>{
              "art/sprites/crate.png has 3 pixels in 2 colors not in palette/pico or "
              "palette/stone: #ff0000 at (0, 0), nearest \"red\" #ff004d; #00ff00 at (1, 0), "
              "nearest \"stone\" #202428",
              "art/title.png is 512x128 pixels; budget/textures allows 256 on a side",
              "ui/icon.png is 64x64 pixels; budget/ui allows 32 on a side",
          });

    // Only what something covers is read: with no budget for every texture, the title is left.
    entries.erase("budget/textures");
    loaded.clear();
    errors.clear();
    storyboard::check_textures(entries, {"art/sprites/hero.png", "art/title.png"}, load, errors);
    CHECK(loaded == std::vector<std::string>{"art/sprites/hero.png"});
    CHECK(errors.empty());

    // A texture that can't be read is left to whatever reads it, and at most three colors are
    // named.
    std::vector<std::uint8_t> rainbow;
    for (std::uint8_t i = 1; i <= 5; ++i) {
        rainbow.insert(rainbow.end(), {static_cast<std::uint8_t>(i * 40), 0, 0, 255});
    }
    textures["art/sprites/sky.png"] = texture(5, 1, rainbow);
    storyboard::check_textures(entries, {"art/sprites/gone.png", "art/sprites/sky.png"}, load,
                               errors);
    REQUIRE(errors.size() == 1);
    CHECK(contains(errors[0].message, "has 5 pixels in 5 colors not in palette/pico: #280000 at "
                                      "(0, 0), nearest \"black\" #000000; #500000 at (1, 0)"));
    CHECK(errors[0].message.ends_with("; and 2 more"));
}

TEST_CASE("changes_since reads the journal lines after a seq") {
    const std::string journal = R"({"changes":{"character/mira":"00000001"}}
{"changes":{"character/mira":"00000002","place/docks":"0000000a"}}
not json
{"changes":{"place/docks":null}}
)";
    CHECK(storyboard::changes_since(journal, 4).empty());
    const json::Object since_one = storyboard::changes_since(journal, 1);
    REQUIRE(since_one.size() == 2);
    CHECK(since_one.at("character/mira").as_string() == "00000002");
    CHECK(since_one.at("place/docks").is_null());
    CHECK(storyboard::changes_since(journal, 3).size() == 1);
    CHECK(storyboard::changes_since(journal, 0).size() == 2);
}
