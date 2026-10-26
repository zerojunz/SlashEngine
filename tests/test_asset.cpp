#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace asset = slash::asset;
namespace json = slash::json;

namespace {

// Files in memory; editing one gives it a new stamp.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string text) {
        files[path] = std::move(text);
        ++edits[path];
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

} // namespace

TEST_CASE("assets load once and are shared by every handle") {
    MemoryFiles files;
    files.write("data/tuning.json", R"({"speed": 3})");
    files.write("data/names.csv", "id,name\n1,Ada\n");
    asset::Assets assets(files.source());

    auto tuning = assets.load<json::Value>("data/tuning.json");
    REQUIRE(tuning.ok());
    CHECK(tuning.value()->find("speed")->as_int() == 3);
    CHECK(tuning.value().version() == 1);
    auto again = assets.load<json::Value>("data/tuning.json");
    REQUIRE(again.ok());
    CHECK(again.value().get() == tuning.value().get());
    auto names = assets.load<std::string>("data/names.csv");
    REQUIRE(names.ok());
    CHECK(*names.value() == "id,name\n1,Ada\n");

    const auto list = assets.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].path == "data/names.csv");
    CHECK(list[0].type == "text");
    CHECK(list[0].refs == 1);
    CHECK(list[1].path == "data/tuning.json");
    CHECK(list[1].refs == 2);

    // An asset with no handles left is unloaded.
    names = asset::Handle<std::string>();
    CHECK(assets.poll().empty());
    CHECK(assets.list().size() == 1);
}

TEST_CASE("asset errors say what is wrong") {
    MemoryFiles files;
    files.write("bad.json", "{");
    files.write("ok.json", "{}");
    asset::Assets assets(files.source());
    const auto message = [](const auto& result) { return result.error().message; };

    CHECK(message(assets.load<json::Value>("missing.json")).find("could not be read") !=
          std::string::npos);
    CHECK(message(assets.load<json::Value>("bad.json")).find("did not load") != std::string::npos);
    CHECK(message(assets.load<json::Value>("art/hero.png")).find("no asset type loads") !=
          std::string::npos);
    CHECK(message(assets.load<std::string>("ok.json")) ==
          "\"ok.json\" is a json asset, not the type asked for");
    CHECK(message(assets.load<json::Value>("../ok.json")).find("relative to the project") !=
          std::string::npos);
    CHECK_FALSE(assets.load<json::Value>("/ok.json").ok());
}

TEST_CASE("poll reloads changed assets and keeps the last good version on failure") {
    MemoryFiles files;
    files.write("data/tuning.json", R"({"speed": 3})");
    files.write("prefabs/enemy.prefab.json", "{}");
    asset::Assets assets(files.source());
    auto tuning = assets.load<json::Value>("data/tuning.json").value();
    const auto old_version = tuning.share();
    REQUIRE(assets.read("prefabs/enemy.prefab.json", "prefab").ok());
    CHECK(assets.poll().empty());

    files.write("data/tuning.json", R"({"speed": 5})");
    auto changes = assets.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].path == "data/tuning.json");
    CHECK(changes[0].version == 2);
    CHECK(changes[0].error.empty());
    CHECK(tuning->find("speed")->as_int() == 5);
    CHECK(tuning.version() == 2);
    CHECK(old_version->find("speed")->as_int() == 3); // share() keeps a version alive

    files.write("data/tuning.json", "{oops");
    files.write("prefabs/enemy.prefab.json", R"({"format": 1})");
    changes = assets.poll();
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].path == "data/tuning.json");
    CHECK(changes[0].error.find("expected") != std::string::npos);
    CHECK(tuning->find("speed")->as_int() == 5);
    CHECK(changes[1].path == "prefabs/enemy.prefab.json");
    CHECK(changes[1].type == "prefab");
    CHECK(changes[1].version == 2);

    // Reading a watched file again does not hide an edit from the next poll.
    files.write("prefabs/enemy.prefab.json", R"({"format": 1, "components": {}})");
    REQUIRE(assets.read("prefabs/enemy.prefab.json", "prefab").ok());
    REQUIRE(assets.poll().size() == 1);

    files.files.erase("data/tuning.json");
    changes = assets.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].error == "the file is missing");
    CHECK(tuning->find("speed")->as_int() == 5);
}

TEST_CASE("mark_changed reloads a file whose stamp stayed the same") {
    MemoryFiles files;
    files.write("data/tuning.json", R"({"speed": 3})");
    files.write("prefabs/enemy.prefab.json", "{}");
    asset::Assets assets(files.source());
    auto tuning = assets.load<json::Value>("data/tuning.json").value();
    REQUIRE(assets.read("prefabs/enemy.prefab.json", "prefab").ok());

    // Written again at the same size within one tick of the file system's clock, a file keeps
    // its stamp.
    files.files["data/tuning.json"] = R"({"speed": 5})";
    files.files["prefabs/enemy.prefab.json"] = "[]";
    CHECK(assets.poll().empty());
    assets.mark_changed("data/tuning.json");
    assets.mark_changed("prefabs/enemy.prefab.json");
    assets.mark_changed("data/unloaded.json");
    const auto changes = assets.poll();
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].path == "data/tuning.json");
    CHECK(changes[0].version == 2);
    CHECK(tuning->find("speed")->as_int() == 5);
    CHECK(changes[1].path == "prefabs/enemy.prefab.json");
    CHECK(changes[1].version == 2);
    CHECK(assets.poll().empty());
}

TEST_CASE("games add their own asset types") {
    MemoryFiles files;
    files.write("levels/one.level", "3");
    asset::Assets assets(files.source());
    assets.add_type<int>("level", {".level"}, [](std::string_view text, std::string_view /*path*/) {
        return slash::Result<int>(static_cast<int>(text.size()) + 2);
    });
    auto level = assets.load<int>("levels/one.level");
    REQUIRE(level.ok());
    CHECK(*level.value() == 3);
}

TEST_CASE("registering an asset type again replaces it") {
    MemoryFiles files;
    files.write("levels/one.level", "3");
    asset::Assets assets(files.source());
    assets.add_type<int>(
        "level", {".level"},
        [](std::string_view /*text*/, std::string_view /*path*/) { return slash::Result<int>(1); });
    assets.add_type<int>(
        "level", {".level"},
        [](std::string_view /*text*/, std::string_view /*path*/) { return slash::Result<int>(2); });
    auto level = assets.load<int>("levels/one.level");
    REQUIRE(level.ok());
    CHECK(*level.value() == 2);
}

TEST_CASE("an asset reloads when a file it read as it loaded changes") {
    MemoryFiles files;
    files.write("levels/one.level", "parts/a.txt parts/b.txt");
    files.write("parts/a.txt", "aa");
    files.write("parts/b.txt", "bbb");
    asset::Assets assets(files.source());
    // A level is the sizes of the files it names.
    assets.add_type<int>(
        "level", {".level"}, [&assets](std::string_view text, std::string_view /*path*/) {
            int total = 0;
            for (const std::string_view part : {text.substr(0, 11), text.substr(12)}) {
                auto read = assets.read(part, "part");
                if (!read) {
                    return slash::Result<int>(read.error());
                }
                total += static_cast<int>(read.value().size());
            }
            return slash::Result<int>(total);
        });
    auto level = assets.load<int>("levels/one.level").value();
    CHECK(*level == 5);
    CHECK(assets.poll().empty());

    files.write("parts/b.txt", "bbbbbb");
    const auto changes = assets.poll();
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].path == "levels/one.level");
    CHECK(changes[0].version == 2);
    CHECK(changes[1].path == "parts/b.txt");
    CHECK(*level == 8);
    // Read again as it reloaded, the parts still belong to it.
    files.write("parts/a.txt", "a");
    CHECK(assets.poll().size() == 2);
    CHECK(*level == 7);
    // A file read outside any load belongs to no asset.
    REQUIRE(assets.read("parts/c.txt", "part").error().code == slash::ErrorCode::NotFound);
    files.write("notes.txt", "x");
    REQUIRE(assets.read("notes.txt", "note").ok());
    files.write("notes.txt", "xy");
    const auto notes = assets.poll();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].path == "notes.txt");
}
