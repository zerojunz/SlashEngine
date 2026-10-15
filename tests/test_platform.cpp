#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using slash::platform::Config;
using slash::platform::Platform;
using slash::platform::RunOptions;

// Only headless paths are tested here, so the suite runs on CI machines with no display.

TEST_CASE("headless platform starts without a window") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());
    CHECK(platform.value().headless());
    CHECK(platform.value().video_driver() == "none");
}

TEST_CASE("only one platform may exist at a time") {
    auto first = Platform::create(Config{.headless = true});
    REQUIRE(first.ok());
    auto second = Platform::create(Config{.headless = true});
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == slash::ErrorCode::InvalidArgument);
}

TEST_CASE("headless run executes exactly max_ticks ticks in order") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());

    std::uint64_t calls = 0;
    bool in_order = true;
    double seen_tick_seconds = 0.0;
    auto stats =
        slash::platform::run(platform.value(), RunOptions{.tick_hz = 50.0, .max_ticks = 120},
                             [&](std::uint64_t tick, double tick_seconds) {
                                 in_order = in_order && tick == calls;
                                 seen_tick_seconds = tick_seconds;
                                 ++calls;
                             });

    REQUIRE(stats.ok());
    CHECK(stats.value().ticks == 120);
    CHECK_FALSE(stats.value().quit_requested);
    CHECK(calls == 120);
    CHECK(in_order);
    CHECK(seen_tick_seconds == doctest::Approx(0.02));
}

TEST_CASE("headless run without a tick limit is rejected") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());
    auto stats = slash::platform::run(platform.value(), RunOptions{}, nullptr);
    REQUIRE_FALSE(stats.ok());
    CHECK(stats.error().code == slash::ErrorCode::InvalidArgument);
}

TEST_CASE("run rejects a non-positive tick rate") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());
    auto stats =
        slash::platform::run(platform.value(), RunOptions{.tick_hz = 0.0, .max_ticks = 1}, nullptr);
    REQUIRE_FALSE(stats.ok());
}

TEST_CASE("files round-trip through write_file and read_file") {
    const std::string path = "slash_test_file.txt";
    const std::string contents = std::string("line one\nline two\0end", 22);
    REQUIRE(slash::platform::write_file(path, contents).ok());
    auto read = slash::platform::read_file(path);
    REQUIRE(read.ok());
    CHECK(read.value() == contents);
    std::remove(path.c_str());

    auto missing = slash::platform::read_file("no/such/file.json");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == slash::ErrorCode::NotFound);
}

TEST_CASE("list_files walks subfolders, skipping hidden names and refused folders") {
    const std::string root = "slash_test_list";
    std::filesystem::remove_all(root);
    for (const char* folder : {"/scenes/levels", "/build", "/.slash"}) {
        REQUIRE(slash::platform::create_directories(root + folder).ok());
    }
    for (const char* file :
         {"/project.json", "/scenes/main.scene.json", "/scenes/levels/one.scene.json",
          "/build/game.scene.json", "/.slash/server.json", "/scenes/.draft.json"}) {
        REQUIRE(slash::platform::write_file(root + file, "{}").ok());
    }
    std::vector<std::string> seen;
    auto files = slash::platform::list_files(root, [&](std::string_view folder) {
        seen.emplace_back(folder);
        return folder != "build/";
    });
    REQUIRE(files.ok());
    CHECK(files.value() == std::vector<std::string>{"project.json", "scenes/levels/one.scene.json",
                                                    "scenes/main.scene.json"});
    std::ranges::sort(seen);
    CHECK(seen == std::vector<std::string>{"build/", "scenes/", "scenes/levels/"});
    CHECK(slash::platform::list_files(root + "/").value().size() == 4);
    std::filesystem::remove_all(root);

    CHECK_FALSE(slash::platform::list_files("no/such/folder").ok());
}

TEST_CASE("read_file_in names the closest file of the same type when a file is missing") {
    const std::string root = "slash_test_read_in";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/prefabs").ok());
    REQUIRE(slash::platform::create_directories(root + "/build").ok());
    for (const char* file :
         {"/prefabs/coin.prefab.json", "/prefabs/con.json", "/build/coin.prefab.json"}) {
        REQUIRE(slash::platform::write_file(root + file, "{}").ok());
    }
    CHECK(slash::platform::read_file_in(root, "prefabs/coin.prefab.json").value() == "{}");
    CHECK(slash::platform::read_file_in(root + "/", "prefabs/con.json").value() == "{}");

    const auto message = [&](std::string_view path) {
        auto text = slash::platform::read_file_in(root, path);
        REQUIRE_FALSE(text.ok());
        CHECK(text.error().code == slash::ErrorCode::NotFound);
        return text.error().message;
    };
    CHECK(message("prefabs/con.prefab.json") ==
          "no such file; did you mean \"prefabs/coin.prefab.json\"?");
    CHECK(message("prefab/coin.prefab.json") ==
          "no such file; did you mean \"prefabs/coin.prefab.json\"?");
    CHECK(message("prefabs/nothing-like-it.prefab.json") == "no such file");
    std::filesystem::remove_all(root);
}
