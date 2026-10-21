#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
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

// Windowed paths use SDL's dummy video driver, so the suite runs on CI machines with no display.

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

TEST_CASE("a frame cap must be from 0 to kMaxFps, and can change while the loop runs") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());
    for (const int bad : {-1, slash::platform::kMaxFps + 1}) {
        auto loop = slash::platform::Loop::create(
            platform.value(), RunOptions{.tick_hz = 60.0, .max_ticks = 1, .max_fps = bad});
        REQUIRE_FALSE(loop.ok());
        CHECK(loop.error().message.find("max_fps") != std::string::npos);
    }
    auto loop = slash::platform::Loop::create(
        platform.value(), RunOptions{.tick_hz = 60.0, .max_ticks = 1, .max_fps = 60});
    REQUIRE(loop.ok());
    CHECK(loop.value().max_fps() == 60);
    CHECK_FALSE(loop.value().set_max_fps(slash::platform::kMaxFps + 1));
    CHECK(loop.value().max_fps() == 60);
    CHECK(loop.value().set_max_fps(0));
    CHECK(loop.value().max_fps() == 0);
}

TEST_CASE("headless frames run one tick each and never wait for the frame cap") {
    auto platform = Platform::create(Config{.headless = true});
    REQUIRE(platform.ok());
    CHECK(platform.value().displays().empty());
    auto loop = slash::platform::Loop::create(
        platform.value(), RunOptions{.tick_hz = 60.0, .max_ticks = 50, .max_fps = 1});
    REQUIRE(loop.ok());
    CHECK(loop.value().frame_stats().frames == 0);
    const double start = platform.value().now_seconds();
    const slash::platform::RunStats stats = slash::platform::run(loop.value(), nullptr);
    // At one frame per second, 50 frames would take 49 seconds if headless runs waited.
    CHECK(platform.value().now_seconds() - start < 10.0);
    CHECK(stats.ticks == 50);
    const slash::platform::FrameStats frames = loop.value().frame_stats();
    CHECK(frames.frames == 50);
    CHECK(frames.catch_up_frames == 0);
    CHECK(frames.frame_ms_min <= frames.frame_ms_avg);
    CHECK(frames.frame_ms_avg <= frames.frame_ms_max);
    CHECK(loop.value().interpolation() == 0.0);
}

// SDL's dummy video driver opens a window with no display on every platform.
TEST_CASE("a frame cap spaces windowed frames out") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    auto platform = Platform::create(Config{.headless = false});
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    REQUIRE_MESSAGE(platform.ok(), (platform.ok() ? std::string() : platform.error().message));
    const std::vector<slash::platform::Display> displays = platform.value().displays();
    REQUIRE(displays.size() == 1);
    CHECK(displays.front().primary);
    CHECK(displays.front().has_window);
    CHECK(displays.front().width > 0);

    constexpr int kFps = 200;
    constexpr int kFrames = 20;
    const double start = platform.value().now_seconds();
    auto loop = slash::platform::Loop::create(platform.value(),
                                              RunOptions{.tick_hz = 60.0, .max_fps = kFps});
    REQUIRE(loop.ok());
    for (int i = 0; i < kFrames; ++i) {
        CHECK(loop.value().frame(nullptr));
    }
    // Each frame starts at least one period after the one before was aimed, so the frames
    // can't arrive faster than the cap, even after a late one.
    CHECK(platform.value().now_seconds() - start >= kFrames * (1.0 / kFps));
    const slash::platform::FrameStats frames = loop.value().frame_stats();
    CHECK(frames.frames == kFrames);
    CHECK(frames.frame_ms_avg >= 4.5);
    CHECK(loop.value().interpolation() >= 0.0);
    CHECK(loop.value().interpolation() < 1.0);
}

TEST_CASE("a platform starts the video drivers it is given unless SDL_VIDEO_DRIVER names others") {
    // The environment variable, which CI may set, wins over the drivers.
    if (SDL_getenv("SDL_VIDEO_DRIVER") == nullptr) {
        auto platform = Platform::create(Config{.headless = false, .video_drivers = "dummy"});
        REQUIRE_MESSAGE(platform.ok(), (platform.ok() ? std::string() : platform.error().message));
        CHECK(platform.value().video_driver() == "dummy");
        // Only while SDL started.
        CHECK(SDL_GetHint(SDL_HINT_VIDEO_DRIVER) == nullptr);
    }
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    auto platform = Platform::create(Config{.headless = false, .video_drivers = "no-such-driver"});
    CHECK(std::string_view(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) == "dummy");
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    REQUIRE_MESSAGE(platform.ok(), (platform.ok() ? std::string() : platform.error().message));
    CHECK(platform.value().video_driver() == "dummy");
}

TEST_CASE("the running program's executable is found") {
    const std::string path = slash::platform::executable_path();
    REQUIRE_FALSE(path.empty());
    CHECK(std::filesystem::path(path).is_absolute());
    CHECK(std::filesystem::path(path).stem() == "slash_tests");
    CHECK(std::filesystem::is_regular_file(path));
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
         {"/prefabs/coin.prefab.json", "/prefabs/coins.json", "/build/coin.prefab.json"}) {
        REQUIRE(slash::platform::write_file(root + file, "{}").ok());
    }
    CHECK(slash::platform::read_file_in(root, "prefabs/coin.prefab.json").value() == "{}");
    CHECK(slash::platform::read_file_in(root + "/", "prefabs/coins.json").value() == "{}");

    const auto message = [&](std::string_view path) {
        auto text = slash::platform::read_file_in(root, path);
        REQUIRE_FALSE(text.ok());
        CHECK(text.error().code == slash::ErrorCode::NotFound);
        return text.error().message;
    };
    CHECK(message("prefabs/coins.prefab.json") ==
          "no such file; did you mean \"prefabs/coin.prefab.json\"?");
    CHECK(message("prefab/coin.prefab.json") ==
          "no such file; did you mean \"prefabs/coin.prefab.json\"?");
    CHECK(message("prefabs/nothing-like-it.prefab.json") == "no such file");
    std::filesystem::remove_all(root);
}

TEST_CASE("paths with a name Windows keeps for a device are refused") {
    using slash::platform::reserved_device_name;
    CHECK(reserved_device_name("prefabs/con.prefab.json") == "con");
    CHECK(reserved_device_name("AUX") == "AUX");
    CHECK(reserved_device_name("levels\\Nul.scene.json") == "Nul");
    CHECK(reserved_device_name("lpt1/x.json") == "lpt1");
    CHECK(reserved_device_name("com9.txt") == "com9");
    for (const char* fine : {"prefabs/coin.prefab.json", "console.json", "com0.json", "com10",
                             "lpt.json", "icon/con_x.png", ""}) {
        CHECK(reserved_device_name(fine).empty());
    }
    // Reading one on Windows would wait for console input, so it is refused unread everywhere.
    auto refused = slash::platform::read_file_in(".", "prefabs/con.prefab.json");
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == slash::ErrorCode::InvalidArgument);
    CHECK(refused.error().message ==
          "path \"prefabs/con.prefab.json\" uses \"con\", a name Windows keeps for a device; "
          "choose another name");
}
