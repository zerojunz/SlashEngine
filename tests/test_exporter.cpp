#include "gltf.hpp"
#include "jpeg.hpp"

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/android.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/ios.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/image.hpp"
#include "slash/text/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace exporter = slash::exporter;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// The message of the error at path, or "" if there is none.
std::string error_at(std::string_view presets, std::string_view path) {
    for (const auto& error : exporter::validate_presets(parse(presets))) {
        if (error.path == path) {
            return error.message;
        }
    }
    return {};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        exporter::register_export_commands(commands);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }
};

} // namespace

TEST_CASE("export presets parse with defaults") {
    auto presets = exporter::parse_presets(parse(R"({
        "format": 1,
        "presets": {
            "mac": {"platform": "macos"},
            "win": {"platform": "windows", "output": "dist/win", "debug": true,
                    "windows": {"sign": true, "timestamp_url": ""}}
        }
    })"));
    REQUIRE(presets.ok());
    const exporter::Preset& mac = presets.value().at("mac");
    CHECK(mac.platform == "macos");
    CHECK(mac.output.empty());
    CHECK_FALSE(mac.debug);
    CHECK(mac.macos.codesign_identity == "-");
    CHECK_FALSE(mac.macos.notarize);
    const exporter::Preset& win = presets.value().at("win");
    CHECK(win.output == "dist/win");
    CHECK(win.debug);
    CHECK(win.windows.sign);
    CHECK(win.windows.timestamp_url.empty());
}

TEST_CASE("export presets report every error with its path") {
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "macso"}}})",
                            "/presets/a/platform"),
                   "did you mean \"macos\""));
    // Secrets belong in environment variables, and the error says so.
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "windows": {"sign": true, "password": "hunter2"}}}})",
                            "/presets/w/windows/password"),
                   "environment variables"));
    CHECK(
        contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "web", "outptu": "x"}}})",
                          "/presets/a/outptu"),
                 "did you mean \"output\""));
    CHECK(contains(error_at(R"({"presets": {}})", ""), "\"format\""));

    // Rules beyond the schema.
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "linux",
                                "output": "../elsewhere"}}})",
                            "/presets/a/output"),
                   "inside the project"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "macos": {"notarize": false}}}})",
                            "/presets/w/macos"),
                   "only macos presets"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"m": {"platform": "macos",
                                "macos": {"notarize": true}}}})",
                            "/presets/m/macos/notarize"),
                   "Developer ID"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"m": {"platform": "macos",
                                "macos": {"entitlements": "/etc/app.plist"}}}})",
                            "/presets/m/macos/entitlements"),
                   "inside the project"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "windows": {"timestamp_url": "ftp://example.com"}}}})",
                            "/presets/w/windows/timestamp_url"),
                   "http"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"bad name": {"platform": "web"}}})",
                            "/presets/bad name"),
                   "letters, digits"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"linux": {"platform": "web"}}})",
                            "/presets/linux/platform"),
                   "named after a platform"));

    CHECK(exporter::validate_presets(parse(R"json({"format": 1, "presets": {
        "store": {"platform": "macos", "output": "dist/mac",
                  "macos": {"codesign_identity": "Developer ID Application: Example (ABCDE12345)",
                            "entitlements": "macos/game.entitlements", "notarize": true}}
    }})json"))
              .empty());
}

TEST_CASE("android presets choose ABIs, a package and a version code") {
    auto presets = exporter::parse_presets(parse(R"({
        "format": 1,
        "presets": {
            "android": {"platform": "android"},
            "phones": {"platform": "android", "android": {"abis": ["arm64-v8a"],
                       "package": "com.example.phones", "version_code": 42}}
        }
    })"));
    REQUIRE(presets.ok());
    const exporter::Preset& defaults = presets.value().at("android");
    CHECK(defaults.android.abis == exporter::kDefaultAndroidAbis);
    CHECK(defaults.android.package.empty());
    CHECK(defaults.android.version_code == 0);
    const exporter::Preset& phones = presets.value().at("phones");
    CHECK(phones.android.abis == std::vector<std::string>{"arm64-v8a"});
    CHECK(phones.android.package == "com.example.phones");
    CHECK(phones.android.version_code == 42);

    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "android",
                                "android": {"abis": ["arm64-v8a", "x86_64", "arm64-v8a"]}}}})",
                            "/presets/a/android/abis/2"),
                   "listed twice"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "android",
                                "android": {"abis": ["mips"]}}}})",
                            "/presets/a/android/abis/0"),
                   "arm64-v8a"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "android",
                                "android": {"package": "com.example.my-game"}}}})",
                            "/presets/a/android/package"),
                   "letters, digits and '_'"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "android",
                                "android": {"version_code": 0}}}})",
                            "/presets/a/android/version_code"),
                   "1"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "web",
                                "android": {"abis": ["x86_64"]}}}})",
                            "/presets/w/android"),
                   "only android presets"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "android",
                                "android": {"keystore_password": "x"}}}})",
                            "/presets/a/android/keystore_password"),
                   "environment variables"));
}

TEST_CASE("android package names follow Java's rules") {
    CHECK(exporter::android_package_problem("com.example.pong").empty());
    CHECK(exporter::android_package_problem("org.slash_engine.Game2").empty());
    CHECK(contains(exporter::android_package_problem("pong"), "two or more parts"));
    CHECK(contains(exporter::android_package_problem("com..pong"), "single dots"));
    CHECK(contains(exporter::android_package_problem("com.example.my-game"), "\"my-game\""));
    CHECK(contains(exporter::android_package_problem("com.example.2048"), "start with a letter"));
    CHECK(contains(exporter::android_package_problem("com.example.new"), "Java keyword"));
}

TEST_CASE("android version codes grow with the version") {
    CHECK(exporter::android_version_code("0.1.0").value() == 1000);
    CHECK(exporter::android_version_code("1.2.3").value() == 1002003);
    CHECK(exporter::android_version_code("2100.0.0").value() == 2100000000);
    CHECK(contains(exporter::android_version_code("1.1000.0").error().message, "version_code"));
    CHECK(contains(exporter::android_version_code("2101.0.0").error().message, "version_code"));
    CHECK_FALSE(exporter::android_version_code("0.0.0").ok());
    CHECK_FALSE(exporter::android_version_code("1.2").ok());
}

namespace {

void touch(const std::filesystem::path& path, std::string_view text = "") {
    std::filesystem::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string exe(std::string_view name) {
#ifdef _WIN32
    return std::string(name) + ".exe";
#else
    return std::string(name);
#endif
}

} // namespace

TEST_CASE("android exports find the newest stable SDK tools, an NDK and a JDK") {
    const std::filesystem::path root = "exporter_android_tools";
    std::filesystem::remove_all(root);
    const std::filesystem::path sdk = root / "sdk";
    for (const char* version : {"34.0.0", "35.0.1", "36.1.0", "37.0.0-rc1"}) {
        touch(sdk / "build-tools" / version / exe("aapt2"));
    }
    for (const char* level :
         {"android-35", "android-36", "android-36-ext19", "android-37.0", "android-37.2-beta3"}) {
        touch(sdk / "platforms" / level / "android.jar");
    }
    const std::filesystem::path ndk = root / "ndk";
    touch(ndk / "source.properties", "Pkg.Desc = Android NDK\nPkg.Revision = 27.3.13750724\n");
    touch(ndk / "build" / "cmake" / "android.toolchain.cmake");
    const std::filesystem::path jdk = root / "jdk";
    for (const char* tool : {"javac", "jar", "keytool"}) {
        touch(jdk / "bin" / exe(tool));
    }
    touch(jdk / "release", "IMPLEMENTOR=\"Eclipse Adoptium\"\nJAVA_VERSION=\"17.0.20\"\n");

    exporter::AndroidSearch search;
    search.sdk = sdk.string();
    search.ndk = ndk.string();
    search.java_home = jdk.string();
    auto tools = exporter::find_android_tools(search);
    REQUIRE_MESSAGE(tools.ok(), (tools.ok() ? std::string() : tools.error().message));
    CHECK(std::filesystem::path(tools.value().build_tools).filename() == "36.1.0");
    CHECK(std::filesystem::path(tools.value().platform).filename() == "android-37.0");
    CHECK(tools.value().ndk == ndk.string());
    CHECK(tools.value().java_home == jdk.string());

    touch(jdk / "release", "JAVA_VERSION=\"11.0.2\"\n");
    auto old_java = exporter::find_android_tools(search);
    REQUIRE_FALSE(old_java.ok());
    CHECK(contains(old_java.error().message, "found Java 11.0.2"));

    touch(ndk / "source.properties", "Pkg.Revision = 26.1.10909125\n");
    auto old_ndk = exporter::find_android_tools(search);
    REQUIRE_FALSE(old_ndk.ok());
    CHECK(contains(old_ndk.error().message, "NDK r27"));

    std::filesystem::remove_all(sdk / "platforms" / "android-37.0");
    std::filesystem::remove_all(sdk / "platforms" / "android-36");
    auto no_platform = exporter::find_android_tools(search);
    REQUIRE_FALSE(no_platform.ok());
    CHECK(contains(no_platform.error().message, "platforms;android-36"));

    search.sdk = (root / "missing").string();
    auto no_sdk = exporter::find_android_tools(search);
    REQUIRE_FALSE(no_sdk.ok());
    CHECK(contains(no_sdk.error().message, "ANDROID_HOME"));
    CHECK(contains(no_sdk.error().message,
                   "slash-cli toolchain install android --accept-android-sdk-license"));
    std::filesystem::remove_all(root);
}

TEST_CASE("ios presets build for the Simulator unless they name a team") {
    auto presets = exporter::parse_presets(parse(R"({
        "format": 1,
        "presets": {
            "ios": {"platform": "ios"},
            "store": {"platform": "ios", "ios": {"team_id": "ABCDE12345",
                      "bundle_id": "com.example.pong", "build_number": 7}},
            "team-sim": {"platform": "ios", "ios": {"team_id": "ABCDE12345",
                         "destination": "simulator"}}
        }
    })"));
    REQUIRE(presets.ok());
    const exporter::Preset& defaults = presets.value().at("ios");
    CHECK(defaults.ios.destination == exporter::kIosSimulator);
    CHECK(defaults.ios.team_id.empty());
    CHECK(defaults.ios.bundle_id.empty());
    CHECK(defaults.ios.build_number == 0);
    const exporter::Preset& store = presets.value().at("store");
    CHECK(store.ios.destination == exporter::kIosDevice);
    CHECK(store.ios.team_id == "ABCDE12345");
    CHECK(store.ios.bundle_id == "com.example.pong");
    CHECK(store.ios.build_number == 7);
    CHECK(store.ios.codesign_identity.empty());
    CHECK(presets.value().at("team-sim").ios.destination == exporter::kIosSimulator);
    // An ad hoc device build needs no team.
    CHECK(exporter::validate_presets(parse(R"({"format": 1, "presets": {"a": {"platform": "ios",
              "ios": {"destination": "device", "codesign_identity": "-"}}}})"))
              .empty());

    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "ios",
                                "ios": {"destination": "device"}}}})",
                            "/presets/a/ios/destination"),
                   "team_id"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "ios",
                                "ios": {"team_id": "abcde12345"}}}})",
                            "/presets/a/ios/team_id"),
                   "10 capital letters"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "ios",
                                "ios": {"bundle_id": "com.example.my_game"}}}})",
                            "/presets/a/ios/bundle_id"),
                   "\"my_game\""));
    CHECK_FALSE(error_at(R"({"format": 1, "presets": {"a": {"platform": "ios",
                             "ios": {"build_number": 0}}}})",
                         "/presets/a/ios/build_number")
                    .empty());
    CHECK(contains(error_at(R"({"format": 1, "presets": {"m": {"platform": "macos",
                                "ios": {"team_id": "ABCDE12345"}}}})",
                            "/presets/m/ios"),
                   "only ios presets"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "ios",
                                "ios": {"certificate_password": "x"}}}})",
                            "/presets/a/ios/certificate_password"),
                   "environment variables"));
}

TEST_CASE("ios bundle IDs use letters, digits and '-'") {
    CHECK(exporter::ios_bundle_id_problem("com.example.pong").empty());
    CHECK(exporter::ios_bundle_id_problem("com.example.my-game2").empty());
    CHECK(exporter::ios_bundle_id_problem("com.example.2048").empty());
    CHECK(contains(exporter::ios_bundle_id_problem("pong"), "two or more parts"));
    CHECK(contains(exporter::ios_bundle_id_problem("com..pong"), "single dots"));
    CHECK(contains(exporter::ios_bundle_id_problem("com.example.pong."), "single dots"));
    CHECK(contains(exporter::ios_bundle_id_problem("com.example.my_game"), "\"my_game\""));
}

TEST_CASE("ios exports test on a booted iPhone Simulator, else the newest one") {
    json::Value list = parse(R"({"devices": {
        "com.apple.CoreSimulator.SimRuntime.iOS-13-7": [
            {"udid": "OLD", "name": "iPhone 11", "state": "Shutdown", "isAvailable": true,
             "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPhone-11"}],
        "com.apple.CoreSimulator.SimRuntime.iOS-17-10": [
            {"udid": "A", "name": "iPhone 15", "state": "Shutdown", "isAvailable": true,
             "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPhone-15"},
            {"udid": "PAD", "name": "iPad Air", "state": "Booted", "isAvailable": true,
             "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPad-Air"}],
        "com.apple.CoreSimulator.SimRuntime.iOS-17-5": [
            {"udid": "B", "name": "iPhone 15 Pro", "state": "Shutdown", "isAvailable": true,
             "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPhone-15-Pro"}],
        "com.apple.CoreSimulator.SimRuntime.iOS-18-0": [
            {"udid": "GONE", "name": "iPhone 16", "state": "Shutdown", "isAvailable": false,
             "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPhone-16"}],
        "com.apple.CoreSimulator.SimRuntime.watchOS-11-0": [
            {"udid": "WATCH", "name": "Apple Watch Series 10", "state": "Booted",
             "isAvailable": true}]
    }})");
    auto newest = exporter::pick_simulator(list);
    REQUIRE(newest.has_value());
    CHECK(newest->udid == "A");
    CHECK(newest->name == "iPhone 15");
    CHECK_FALSE(newest->booted);

    list["devices"]["com.apple.CoreSimulator.SimRuntime.iOS-17-5"].as_array()[0]["state"] =
        "Booted";
    auto booted = exporter::pick_simulator(list);
    REQUIRE(booted.has_value());
    CHECK(booted->udid == "B");
    CHECK(booted->booted);

    CHECK_FALSE(exporter::pick_simulator(parse(R"({"devices": {
        "com.apple.CoreSimulator.SimRuntime.iOS-13-7": [
            {"udid": "OLD", "name": "iPhone 11", "state": "Booted", "isAvailable": true}]
    }})"))
                    .has_value());
    CHECK_FALSE(exporter::pick_simulator(parse(R"({"devices": {}})")).has_value());
    CHECK_FALSE(exporter::pick_simulator(parse("[]")).has_value());
}

TEST_CASE("ios device builds pick the team's codesign identity") {
    constexpr std::string_view kIdentities =
        R"ids(  1) 1111111111111111111111111111111111111111 "Apple Development: Jamie Doe (XYZ9876543)"
  2) 2222222222222222222222222222222222222222 "Apple Distribution: Example Inc (ABCDE12345)"
  3) 3333333333333333333333333333333333333333 "Apple Distribution: Other Studio (QWERT67890)"
  4) 2222222222222222222222222222222222222222 "Apple Distribution: Example Inc (ABCDE12345)"
     4 valid identities found
)ids";
    auto development =
        exporter::find_codesign_identity(kIdentities, "Apple Development", "ABCDE12345");
    REQUIRE(development.ok());
    CHECK(development.value().hash == "1111111111111111111111111111111111111111");
    auto distribution =
        exporter::find_codesign_identity(kIdentities, "Apple Distribution", "ABCDE12345");
    REQUIRE(distribution.ok());
    CHECK(distribution.value().name == "Apple Distribution: Example Inc (ABCDE12345)");
    auto by_hash = exporter::find_codesign_identity(
        kIdentities, "3333333333333333333333333333333333333333", "ABCDE12345");
    REQUIRE(by_hash.ok());
    CHECK(by_hash.value().name == "Apple Distribution: Other Studio (QWERT67890)");
    auto by_name = exporter::find_codesign_identity(
        kIdentities, "Apple Distribution: Other Studio (QWERT67890)", "ABCDE12345");
    REQUIRE(by_name.ok());
    CHECK(by_name.value().hash == "3333333333333333333333333333333333333333");

    auto ambiguous =
        exporter::find_codesign_identity(kIdentities, "Apple Distribution", "ZZZZZ00000");
    REQUIRE_FALSE(ambiguous.ok());
    CHECK(contains(ambiguous.error().message, "Other Studio"));
    CHECK(contains(ambiguous.error().message, "codesign_identity"));
    auto missing = exporter::find_codesign_identity(kIdentities, "iPhone Distribution", "");
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "security find-identity"));
    CHECK_FALSE(exporter::find_codesign_identity("", "Apple Development", "ABCDE12345").ok());
}

TEST_CASE("ios provisioning profiles cover exact and wildcard bundle IDs") {
    CHECK(
        exporter::profile_covers("ABCDE12345.com.example.pong", "ABCDE12345", "com.example.pong"));
    CHECK_FALSE(
        exporter::profile_covers("ABCDE12345.com.example.pong", "ABCDE12345", "com.example.pongs"));
    CHECK(exporter::profile_covers("ABCDE12345.*", "ABCDE12345", "com.example.pong"));
    CHECK(exporter::profile_covers("ABCDE12345.com.example.*", "ABCDE12345", "com.example.pong"));
    CHECK_FALSE(
        exporter::profile_covers("ABCDE12345.com.example.*", "ABCDE12345", "org.example.pong"));
    CHECK_FALSE(exporter::profile_covers("QWERT67890.*", "ABCDE12345", "com.example.pong"));
    CHECK_FALSE(exporter::profile_covers("ABCDE12345*", "ABCDE12345", "com.example.pong"));
    CHECK_FALSE(exporter::profile_covers("ABCDE12345.*", "", "com.example.pong"));
}

TEST_CASE("linux presets choose an architecture") {
    auto presets = exporter::parse_presets(parse(R"({
        "format": 1,
        "presets": {
            "linux": {"platform": "linux"},
            "pi": {"platform": "linux", "linux": {"arch": "arm64"}}
        }
    })"));
    REQUIRE(presets.ok());
    CHECK(presets.value().at("linux").linux_options.arch == exporter::kLinuxX86_64);
    CHECK(presets.value().at("pi").linux_options.arch == exporter::kLinuxArm64);
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "linux",
                                "linux": {"arch": "arm46"}}}})",
                            "/presets/a/linux/arch"),
                   "\"x86_64\", \"arm64\""));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "linux": {"arch": "arm64"}}}})",
                            "/presets/w/linux"),
                   "only linux presets"));
}

TEST_CASE("toolchains hash their downloads with SHA-256") {
    CHECK(exporter::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(exporter::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(exporter::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1'000'000, 'a');
    CHECK(exporter::sha256_hex(million) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::filesystem::path file = "exporter_sha256.bin";
    REQUIRE(slash::platform::write_file(file.string(), million).ok());
    auto hashed = exporter::sha256_file(file);
    REQUIRE(hashed.ok());
    CHECK(hashed.value() == exporter::sha256_hex(million));
    std::filesystem::remove(file);
    CHECK_FALSE(exporter::sha256_file(file).ok());
}

TEST_CASE("toolchains are pinned per host and chosen per platform") {
    const std::vector<exporter::Toolchain> toolchains = exporter::host_toolchains();
    const auto has = [&toolchains](std::string_view name) {
        return std::any_of(toolchains.begin(), toolchains.end(),
                           [name](const exporter::Toolchain& t) { return t.name == name; });
    };
    for (const exporter::Toolchain& toolchain : toolchains) {
        CAPTURE(toolchain.name);
        CHECK(toolchain.url.starts_with("https://"));
        CHECK(toolchain.sha256.size() == 64);
        CHECK(toolchain.sha256.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK_FALSE(toolchain.version.empty());
        CHECK_FALSE(toolchain.description.empty());
    }
#if defined(_WIN32)
    // Windows exports build with MSVC, and Linux exports aren't built on Windows.
    CHECK_FALSE(has("llvm-mingw"));
    CHECK(exporter::toolchains_for("windows").empty());
#elif defined(__APPLE__) || (defined(__linux__) && defined(__x86_64__))
    CHECK(has("llvm-mingw"));
    CHECK(exporter::toolchains_for("windows") == std::vector<std::string>{"llvm-mingw"});
    CHECK(exporter::toolchains_for("linux") ==
          std::vector<std::string>{"llvm-mingw", "linux-sysroot-x86_64"});
    CHECK(exporter::toolchains_for("linux", exporter::kLinuxArm64) ==
          std::vector<std::string>{"llvm-mingw", "linux-sysroot-arm64"});
    for (const std::string& name : exporter::toolchains_for("linux", exporter::kLinuxArm64)) {
        CHECK(has(name));
    }
#endif
    CHECK(exporter::toolchains_for("macos").empty());

    // Web exports with no emsdk download Emscripten, Node.js and, where the system has none to
    // count on, Python.
    const std::vector<std::string> web = exporter::toolchains_for("web");
#if defined(__APPLE__) || defined(_WIN32)
    CHECK(web == std::vector<std::string>{"emscripten", "node", "python"});
#else
    CHECK(web == std::vector<std::string>{"emscripten", "node"});
#endif
#if defined(__APPLE__) || defined(__linux__) || (defined(_WIN32) && defined(_M_X64))
    for (const std::string& name : web) {
        CHECK(has(name));
    }
    const auto emscripten =
        std::find_if(toolchains.begin(), toolchains.end(),
                     [](const exporter::Toolchain& t) { return t.name == "emscripten"; });
    REQUIRE(emscripten != toolchains.end());
    CHECK(emscripten->version == "6.0.10");
    CHECK(emscripten->folder == "install");
    CHECK(contains(emscripten->url, "storage.googleapis.com/webassembly/"));
#endif
}

TEST_CASE("the Android SDK installs from a pinned JDK and command-line tools") {
    const std::optional<exporter::AndroidInstall> install = exporter::android_install();
#if defined(__APPLE__) || (defined(__linux__) && defined(__x86_64__)) ||                           \
    (defined(_WIN32) && defined(_M_X64))
    REQUIRE(install.has_value());
    for (const exporter::Toolchain* toolchain : {&install->jdk, &install->command_line_tools}) {
        CAPTURE(toolchain->name);
        CHECK(toolchain->sha256.size() == 64);
        CHECK(toolchain->sha256.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK_FALSE(toolchain->description.empty());
    }
    CHECK(install->jdk.name == "jdk");
    CHECK(install->jdk.version == "21.0.12.1+1");
    CHECK(install->jdk.folder == "jdk-21.0.12.1+1");
    CHECK(install->jdk.url.starts_with(
        "https://github.com/adoptium/temurin21-binaries/releases/download/"));
    CHECK(install->command_line_tools.name == "android-cmdline-tools");
    CHECK(install->command_line_tools.folder == "cmdline-tools");
    CHECK(install->command_line_tools.url.starts_with("https://dl.google.com/android/repository/"));
    CHECK(install->command_line_tools.url.ends_with("15859902_latest.zip"));
    // What exports check for: build tools 35 or later, the android-36 platform and NDK r27.
    CHECK(install->packages == std::vector<std::string>{"platform-tools", "build-tools;36.0.0",
                                                        "platforms;android-36", "ndk;27.3.13750724",
                                                        "cmake;3.31.6"});
#else
    CHECK_FALSE(install.has_value()); // Google builds no SDK for this host
#endif

    // Nothing is installed until the license is accepted.
    if (install) {
        const std::filesystem::path dir = std::filesystem::absolute("exporter_android_refused");
        std::filesystem::remove_all(dir);
        auto refused = exporter::install_android(*install, dir, false);
        REQUIRE_FALSE(refused.ok());
        CHECK(contains(refused.error().message, "https://developer.android.com/studio/terms"));
        CHECK_FALSE(std::filesystem::exists(dir));
    }
}

TEST_CASE("downloaded Emscripten gets a config naming its tools") {
    const std::filesystem::path root = "/opt/toolchains";
    const exporter::WebTools tools = exporter::web_tools(
        {root / "emscripten-6.0.10", root / "node-22.16.0", root / "python-3.13.3"});
    CHECK(tools.toolchain_file == root / "emscripten-6.0.10" / "emscripten" / "cmake" / "Modules" /
                                      "Platform" / "Emscripten.cmake");
#ifdef _WIN32
    CHECK(tools.node == root / "node-22.16.0" / "bin" / "node.exe");
    CHECK(tools.python == root / "python-3.13.3" / "python.exe");
#else
    CHECK(tools.node == root / "node-22.16.0" / "bin" / "node");
    CHECK(tools.python == root / "python-3.13.3" / "bin" / "python3");
#endif
    CHECK(tools.config == "LLVM_ROOT = '" + (root / "emscripten-6.0.10" / "bin").generic_string() +
                              "'\nBINARYEN_ROOT = '" +
                              (root / "emscripten-6.0.10").generic_string() + "'\nNODE_JS = '" +
                              tools.node.generic_string() + "'\n");

    // Linux runs Emscripten with its own python3. Quotes in a path are escaped for Python.
    const exporter::WebTools linux_tools =
        exporter::web_tools({"/home/o'neil/emscripten", "/home/o'neil/node"});
    CHECK(linux_tools.python.empty());
    CHECK(contains(linux_tools.config, "BINARYEN_ROOT = '/home/o\\'neil/emscripten'\n"));
}

#ifndef _WIN32
TEST_CASE("toolchains install from a checked archive") {
    const std::filesystem::path folder = std::filesystem::absolute("exporter_toolchains");
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "src" / "kit-1.0" / "bin");
    REQUIRE(slash::platform::write_file((folder / "src" / "kit-1.0" / "bin" / "tool").string(),
                                        "#!/bin/sh\n")
                .ok());
    const std::filesystem::path archive = folder / "kit.tar.gz";
    auto packed = slash::platform::run_process(
        {"tar", "-czf", archive.string(), "-C", (folder / "src").string(), "kit-1.0"}, {});
    REQUIRE(packed.ok());
    REQUIRE(packed.value().exit_code == 0);
    auto bytes = slash::platform::read_file(archive.string());
    REQUIRE(bytes.ok());

    exporter::Toolchain kit{.name = "kit",
                            .version = "1.0",
                            .description = "A test toolchain",
                            .url = "file://" + archive.generic_string(),
                            .sha256 = exporter::sha256_hex(bytes.value()),
                            .folder = "kit-1.0"};
    const std::filesystem::path dir = folder / "toolchains";

    // A download that doesn't match its pinned hash is deleted, and nothing is installed.
    exporter::Toolchain tampered = kit;
    tampered.sha256 = exporter::sha256_hex("something else");
    auto rejected = exporter::install_toolchain(tampered, dir);
    REQUIRE_FALSE(rejected.ok());
    CHECK(contains(rejected.error().message, "has SHA-256 " + kit.sha256));
    CHECK_FALSE(exporter::installed_toolchain(kit, dir).has_value());
    // Not even its staging folder is left behind.
    CHECK(std::distance(std::filesystem::directory_iterator(dir),
                        std::filesystem::directory_iterator()) == 0);

    auto installed = exporter::install_toolchain(kit, dir);
    REQUIRE_MESSAGE(installed.ok(), (installed.ok() ? std::string() : installed.error().message));
    CHECK(installed.value() == dir / "kit-1.0");
    CHECK(std::filesystem::is_regular_file(dir / "kit-1.0" / "bin" / "tool"));
    REQUIRE(exporter::installed_toolchain(kit, dir).has_value());
    CHECK(*exporter::installed_toolchain(kit, dir) == installed.value());

    // An installed toolchain isn't downloaded again.
    exporter::Toolchain moved = kit;
    moved.url = "file://" + (folder / "missing.tar.gz").generic_string();
    auto again = exporter::install_toolchain(moved, dir);
    REQUIRE(again.ok());
    CHECK(again.value() == installed.value());

    exporter::Toolchain missing = moved;
    missing.version = "2.0";
    auto failed = exporter::install_toolchain(missing, dir);
    REQUIRE_FALSE(failed.ok());
    CHECK(contains(failed.error().message, "could not download kit"));
    std::filesystem::remove_all(folder);
}
#endif

#ifndef _WIN32
namespace {

// Sets an environment variable, or unsets it for an empty value, and puts it back on scope exit.
class ScopedVariable {
public:
    ScopedVariable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* old = SDL_getenv_unsafe(name_.c_str())) {
            old_ = old;
        }
        if (value.empty()) {
            SDL_unsetenv_unsafe(name_.c_str());
        } else {
            SDL_setenv_unsafe(name_.c_str(), value.c_str(), 1);
        }
    }
    ~ScopedVariable() {
        if (old_) {
            SDL_setenv_unsafe(name_.c_str(), old_->c_str(), 1);
        } else {
            SDL_unsetenv_unsafe(name_.c_str());
        }
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

// A .tar.gz (or, by extension, .zip) of folder/name, for a file:// toolchain.
exporter::Toolchain pack_toolchain(const std::filesystem::path& folder, const std::string& name,
                                   const std::string& archive_name) {
    const std::filesystem::path archive = folder / archive_name;
    slash::platform::ProcessOptions in_folder;
    in_folder.working_directory = folder.string();
    auto packed = archive_name.ends_with(".zip")
                      ? slash::platform::run_process({SLASH_TEST_CMAKE, "-E", "tar", "cf",
                                                      archive.string(), "--format=zip", name},
                                                     in_folder)
                      : slash::platform::run_process(
                            {"tar", "-czf", archive.string(), "-C", folder.string(), name}, {});
    REQUIRE(packed.ok());
    REQUIRE(packed.value().exit_code == 0);
    auto bytes = slash::platform::read_file(archive.string());
    REQUIRE(bytes.ok());
    return {.name = name,
            .version = "1",
            .description = "A test toolchain",
            .url = "file://" + archive.generic_string(),
            .sha256 = exporter::sha256_hex(bytes.value()),
            .folder = name};
}

} // namespace

TEST_CASE("the Android SDK installs with sdkmanager and exports find it") {
    const std::filesystem::path folder = std::filesystem::absolute("exporter_android_install");
    std::filesystem::remove_all(folder);
    const std::filesystem::path src = folder / "src";

    // A JDK, laid out as on this host.
#ifdef __APPLE__
    const std::filesystem::path jdk_home = src / "jdk-test" / "Contents" / "Home";
#else
    const std::filesystem::path jdk_home = src / "jdk-test";
#endif
    for (const char* tool : {"javac", "jar", "keytool"}) {
        touch(jdk_home / "bin" / tool);
    }
    touch(jdk_home / "release", "JAVA_VERSION=\"21.0.12\"\n");

    // Google's command-line tools, whose sdkmanager stands in here: it notes how it was run, and
    // installs the packages only if told yes.
    const std::filesystem::path sdkmanager = src / "cmdline-tools" / "bin" / "sdkmanager";
    touch(sdkmanager, R"(#!/bin/sh
tools="$(dirname "$0")/.."
echo "run with JAVA_HOME=$JAVA_HOME $*" >> "$tools/calls.txt"
read answer
[ "$answer" = y ] || { echo "The following packages can not be installed since their licenses were not accepted"; exit 1; }
for arg in "$@"; do
  case "$arg" in
    --sdk_root=*) root="${arg#--sdk_root=}" ;;
    --package_file=*) cp "${arg#--package_file=}" "$tools/packages.txt" ;;
  esac
done
mkdir -p "$root/build-tools/36.0.0" "$root/platforms/android-36" "$root/ndk/27.3.13750724/build/cmake"
touch "$root/build-tools/36.0.0/aapt2" "$root/platforms/android-36/android.jar" "$root/ndk/27.3.13750724/build/cmake/android.toolchain.cmake"
echo "Pkg.Revision = 27.3.13750724" > "$root/ndk/27.3.13750724/source.properties"
)");
    std::filesystem::permissions(sdkmanager, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);

    exporter::AndroidInstall install;
    install.jdk = pack_toolchain(src, "jdk-test", "jdk.tar.gz");
    install.jdk.name = "jdk";
    const std::optional<exporter::AndroidInstall> pinned = exporter::android_install();
    if (pinned) {
        install.jdk.version = pinned->jdk.version; // so exports look for it here
    }
    install.command_line_tools = pack_toolchain(src, "cmdline-tools", "tools.zip");
    install.command_line_tools.name = "android-cmdline-tools";
    install.packages = {"platform-tools", "build-tools;36.0.0", "platforms;android-36",
                        "ndk;27.3.13750724"};
    const std::filesystem::path dir = folder / "toolchains";

    exporter::AndroidFolders none = exporter::installed_android(install, dir);
    CHECK(none.sdk.empty());
    CHECK(none.java_home.empty());

    auto installed = exporter::install_android(install, dir, true);
    REQUIRE_MESSAGE(installed.ok(), (installed.ok() ? std::string() : installed.error().message));
    CHECK(installed.value().sdk == dir / "android-sdk");
#ifdef __APPLE__
    CHECK(installed.value().java_home ==
          dir / ("jdk-" + install.jdk.version) / "Contents" / "Home");
#else
    CHECK(installed.value().java_home == dir / ("jdk-" + install.jdk.version));
#endif
    const std::filesystem::path tools = dir / "android-cmdline-tools-1";
    auto calls = slash::platform::read_file((tools / "calls.txt").string());
    REQUIRE(calls.ok());
    CHECK(calls.value() ==
          "run with JAVA_HOME=" + installed.value().java_home.string() +
              " --sdk_root=" + installed.value().sdk.string() +
              " --package_file=" + (installed.value().sdk / ".slash-packages.txt").string() + "\n");
    auto packages = slash::platform::read_file((tools / "packages.txt").string());
    REQUIRE(packages.ok());
    CHECK(packages.value() ==
          "platform-tools\nbuild-tools;36.0.0\nplatforms;android-36\nndk;27.3.13750724\n");
    CHECK_FALSE(std::filesystem::exists(installed.value().sdk / ".slash-packages.txt"));

    exporter::AndroidFolders found = exporter::installed_android(install, dir);
    CHECK(found.sdk == installed.value().sdk);
    CHECK(found.java_home == installed.value().java_home);

    // Installed packages aren't installed again, but a package added since is.
    auto again = exporter::install_android(install, dir, true);
    REQUIRE(again.ok());
    calls = slash::platform::read_file((tools / "calls.txt").string());
    REQUIRE(calls.ok());
    CHECK(std::ranges::count(calls.value(), '\n') == 1);
    install.packages.emplace_back("cmake;3.31.6");
    CHECK(exporter::installed_android(install, dir).sdk.empty());
    REQUIRE(exporter::install_android(install, dir, true).ok());
    calls = slash::platform::read_file((tools / "calls.txt").string());
    REQUIRE(calls.ok());
    CHECK(std::ranges::count(calls.value(), '\n') == 2);

    // With no ANDROID_HOME or JAVA_HOME, exports use what slash-cli installed.
    if (pinned) {
        const ScopedVariable toolchains(std::string(exporter::kToolchainsDirVariable),
                                        dir.string());
        std::vector<std::unique_ptr<ScopedVariable>> unset;
        for (const char* name : {"ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_NDK_HOME",
                                 "ANDROID_NDK_ROOT", "ANDROID_NDK", "JAVA_HOME"}) {
            unset.push_back(std::make_unique<ScopedVariable>(name, ""));
        }
        auto tools_found = exporter::find_android_tools();
        REQUIRE_MESSAGE(tools_found.ok(),
                        (tools_found.ok() ? std::string() : tools_found.error().message));
        CHECK(tools_found.value().sdk == installed.value().sdk.string());
        CHECK(tools_found.value().java_home == installed.value().java_home.string());
        CHECK(tools_found.value().ndk ==
              (installed.value().sdk / "ndk" / "27.3.13750724").string());
    }

    // A refused license fails the install with what sdkmanager said.
    touch(dir / "android-sdk" / ".slash-android-sdk.json", "{}");
    const std::filesystem::path stubborn = tools / "bin" / "sdkmanager";
    touch(stubborn, "#!/bin/sh\necho 'Accept? (y/N): '\nexit 1\n");
    std::filesystem::permissions(stubborn, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    auto failed = exporter::install_android(install, dir, true);
    REQUIRE_FALSE(failed.ok());
    CHECK(contains(failed.error().message, "sdkmanager could not install the Android SDK"));
    CHECK(contains(failed.error().message, "Accept? (y/N)"));
    std::filesystem::remove_all(folder);
}
#endif

TEST_CASE("presets fall back to platform defaults and suggest names") {
    auto presets = exporter::parse_presets(
        parse(R"({"format": 1, "presets": {"mac-store": {"platform": "macos"}}})"));
    REQUIRE(presets.ok());
    auto named = exporter::find_preset(presets.value(), "mac-store");
    REQUIRE(named.ok());
    CHECK(named.value().platform == "macos");
    auto defaults = exporter::find_preset(presets.value(), "linux");
    REQUIRE(defaults.ok());
    CHECK(defaults.value().name == "linux");
    CHECK(defaults.value().platform == "linux");

    auto misspelled = exporter::find_preset(presets.value(), "mac-stor");
    REQUIRE_FALSE(misspelled.ok());
    CHECK(contains(misspelled.error().message, "did you mean \"mac-store\""));
    auto unknown = exporter::find_preset(presets.value(), "playstation");
    REQUIRE_FALSE(unknown.ok());
    CHECK(contains(unknown.error().message, "macos, windows, linux, web, android or ios"));
}

TEST_CASE("presets load from the project folder") {
    const std::filesystem::path folder = "exporter_presets_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    auto none = exporter::load_presets(folder.string());
    REQUIRE(none.ok());
    CHECK(none.value().empty());

    REQUIRE(slash::platform::write_file((folder / "export_presets.json").string(),
                                        R"({"format": 1, "presets": {"a": {"platform": "wbe"}}})")
                .ok());
    auto broken = exporter::load_presets(folder.string());
    REQUIRE_FALSE(broken.ok());
    CHECK(contains(broken.error().message, "export_presets.json /presets/a/platform"));

    // An export reports every preset error before it builds anything.
    REQUIRE(slash::platform::write_file(
                (folder / "project.json").string(),
                R"({"format": 1, "name": "Presets", "main_scene": "main.scene.json"})")
                .ok());
    exporter::ExportOptions options;
    options.target = "linux";
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    CHECK(error->find("errors")->as_array()[0].find("file")->as_string() == "export_presets.json");
    CHECK_FALSE(std::filesystem::exists(folder / ".slash"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship textures, frames, material, animation and tile set files, and check them "
          "first") {
    const std::filesystem::path folder = "exporter_sprites_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "sprites");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Sprites", "main_scene": "main.scene.json"})");
    // A tile map layer whose rows use a key its legend lacks.
    write("main.scene.json", R"({"format": 1, "entities": {"map": {"components": {
        "TileMapLayer": {"tile_set": "sprites/ground.tileset.json", "legend": {"r": "rock"},
                         "rows": ["rx"]}}}}})");
    const slash::render::Image image{
        .width = 2, .height = 1, .pixels = {255, 0, 0, 255, 0, 0, 255, 255}};
    write("sprites/hero.png", slash::render::encode_png(image).value());
    write(
        "sprites/hero.frames.json",
        R"({"format": 1, "animations": {"walk": {"texture": "sprites/hero.png", "hframes": 2}}})");
    write("sprites/notes.md", "not part of the game");
    write("sprites/broken.png", "not an image");
    write("sprites/bad.frames.json", R"({"format": 1, "animations": {"walk": {"frames": [0]}}})");
    write("sprites/glow.material.json",
          R"({"format": 1, "color": {"op": "mul", "args": ["color", 2]}, "blend": "add"})");
    write("sprites/bad.material.json", R"({"format": 1, "color": "colr"})");
    write("sprites/hero.anim.json", R"({"format": 1, "animations": {"spin": {"length": 1,
        "tracks": [{"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 0}]}]}}})");
    write("sprites/bad.anim.json", R"({"format": 1, "animations": {"spin": {"length": 1,
        "tracks": [{"path": "Transform2D/rotation", "keys": [{"time": 0, "value": "fast"}]}]}}})");
    write("sprites/ground.tileset.json", R"({"format": 1, "sources": {"ground": {
        "texture": "sprites/hero.png", "tiles": {"0,0": {"name": "rock"}}}}})");
    write("sprites/bad.tileset.json", R"({"format": 1, "sources": {"ground": {
        "texture": "sprites/hero.png", "tiles": {"0,0": {"terrain": "grass"}}}}})");

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    std::vector<std::string> problems;
    for (const json::Value& problem : error->find("errors")->as_array()) {
        problems.push_back(problem.find("file")->as_string() + " " +
                           problem.find("path")->as_string() + " " +
                           problem.find("message")->as_string());
    }
    std::ranges::sort(problems); // folders list in any order
    REQUIRE(problems.size() == 6);
    INFO(problems[0], "\n", problems[1], "\n", problems[2], "\n", problems[3], "\n", problems[4],
         "\n", problems[5]);
    CHECK(problems[0] == "main.scene.json /entities/map/components/TileMapLayer/rows/0 column 1: "
                         "\"x\" is not in the legend");
    CHECK(problems[1] == "sprites/bad.anim.json /animations/spin/tracks/0/keys/0/value expected "
                         "number, got string");
    CHECK(contains(problems[2], "sprites/bad.frames.json /animations/walk/frames/0 a frame number "
                                "needs the animation's \"texture\""));
    CHECK(problems[3] ==
          "sprites/bad.material.json /color no node, param or input \"colr\"; did you mean "
          "\"color\"?");
    CHECK(problems[4] == "sprites/bad.tileset.json /sources/ground/tiles/0,0/terrain no terrain "
                         "set has a terrain named \"grass\"");
    CHECK(contains(problems[5], "sprites/broken.png  not a PNG, BMP, JPEG or KTX2 image"));

    // The good files were staged with the scenes; files a game never loads were not.
    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK(std::filesystem::exists(data / "sprites" / "hero.png"));
    CHECK(std::filesystem::exists(data / "sprites" / "hero.frames.json"));
    CHECK(std::filesystem::exists(data / "sprites" / "glow.material.json"));
    CHECK(std::filesystem::exists(data / "sprites" / "hero.anim.json"));
    CHECK(std::filesystem::exists(data / "sprites" / "ground.tileset.json"));
    CHECK(std::filesystem::exists(data / "main.scene.json"));
    CHECK_FALSE(std::filesystem::exists(data / "sprites" / "notes.md"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports compile each material's shader into its file, and check the shaders first") {
    const std::filesystem::path folder = "exporter_shaders_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "shaders");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Shaders", "main_scene": "main.scene.json"})");
    write("main.scene.json", R"({"format": 1, "entities": {}})");
    write("shaders/tint.hlsl", "void fragment() {\n    COLOR *= tint;\n}\n");
    write("shaders/bad.hlsl", "void fragment() {\n    COLOR *= tnt;\n}\n");
    write("tint.material.json", R"({"format": 1, "params": {"tint": {"type": "color"}},
        "shader": "shaders/tint.hlsl"})");
    write("bad.material.json", R"({"format": 1, "params": {"tint": {"type": "color"}},
        "shader": "shaders/bad.hlsl"})");

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    REQUIRE(error->find("errors")->size() == 1);
    const json::Value& problem = error->find("errors")->as_array()[0];
    CHECK(problem.find("file")->as_string() == "bad.material.json");
    CHECK(problem.find("path")->as_string() == "/shader");
    CHECK(problem.find("message")->as_string() == "shaders/bad.hlsl:2: 'tnt' : unknown variable");

    // The good material was staged with its shader compiled for the platform; the shader files
    // stay behind, since games can't compile them.
    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK_FALSE(std::filesystem::exists(data / "shaders"));
    auto staged = slash::platform::read_file((data / "tint.material.json").string());
    REQUIRE(staged.ok());
    const json::Value doc = json::parse(staged.value()).value();
    CHECK(doc.find("shader")->as_string() == "shaders/tint.hlsl");
    const json::Value* compiled = doc.find("compiled");
    REQUIRE(compiled != nullptr);
    CHECK(compiled->find("cpu_fragment") != nullptr);
    CHECK((compiled->find("msl_fragment") != nullptr) == (host == "macos"));
    CHECK((compiled->find("spirv_fragment") != nullptr) == (host != "macos"));
    CHECK((compiled->find("hlsl") != nullptr) == (host == "windows"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship fonts and themes and check them, and the controls of scenes, first") {
    const std::filesystem::path folder = "exporter_fonts_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "fonts");
    std::filesystem::create_directories(folder / "ui");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Fonts", "main_scene": "main.scene.json"})");
    // A control listed as its own child.
    write("main.scene.json", R"({"format": 1, "entities": {"panel": {"components": {
        "Control": {"children": ["panel"],
                    "theme_override_fonts": {"font": "fonts/title.font.json"}},
        "Label": {"text": "Paused"}}}}})");
    const std::span<const std::uint8_t> roboto = slash::text::default_font();
    write("fonts/roboto.ttf",
          std::string_view(reinterpret_cast<const char*>(roboto.data()), roboto.size()));
    write(
        "fonts/title.font.json",
        R"({"format": 1, "font": "fonts/roboto.ttf", "multichannel_signed_distance_field": true})");
    write("fonts/broken.ttf", "not a font");
    write("fonts/bad.font.json", R"({"format": 1, "font": "fonts/title.woff"})");
    write("ui/game.theme.json", R"({"format": 1, "types": {"Label": {"font_sizes": {
        "font_size": 20}}}})");
    write("ui/bad.theme.json", R"({"format": 1, "types": {"Lable": {}}})");

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    std::vector<std::string> problems;
    for (const json::Value& problem : error->find("errors")->as_array()) {
        problems.push_back(problem.find("file")->as_string() + " " +
                           problem.find("path")->as_string() + " " +
                           problem.find("message")->as_string());
    }
    std::ranges::sort(problems); // folders list in any order
    REQUIRE(problems.size() == 4);
    INFO(problems[0], "\n", problems[1], "\n", problems[2], "\n", problems[3]);
    CHECK(problems[0] == "fonts/bad.font.json /font expected a .ttf or .otf file");
    CHECK(problems[1] == "fonts/broken.ttf  not a TrueType or OpenType font");
    CHECK(problems[2] ==
          "main.scene.json /entities/panel/components/Control/children/0 a control can't be "
          "its own child");
    CHECK(problems[3] ==
          "ui/bad.theme.json /types/Lable \"Lable\" isn't a control type; did you mean "
          "\"Label\"?");

    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK(std::filesystem::exists(data / "fonts" / "roboto.ttf"));
    CHECK(std::filesystem::exists(data / "fonts" / "title.font.json"));
    CHECK(std::filesystem::exists(data / "ui" / "game.theme.json"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship sounds, .audio.json and .bus_layout.json files and check them first") {
    const std::filesystem::path folder = "exporter_audio_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "audio");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Sounds", "main_scene": "main.scene.json",
        "audio": {"bus_layout": "audio/main.bus_layout.json"}})");
    write("main.scene.json", R"({"format": 1, "entities": {}})");
    const std::vector<slash::audio::Frame> beep(100, slash::audio::Frame{0.5F, 0.5F});
    write("audio/beep.wav", slash::audio::encode_wav(beep, 44100));
    write("audio/broken.ogg", "not a sound");
    write(
        "audio/steps.audio.json",
        R"({"format": 1, "streams": [{"stream": "audio/beep.wav"}], "playback_mode": "shuffle"})");
    write("audio/main.bus_layout.json",
          R"({"format": 1, "buses": [{"name": "Master"}, {"name": "Sfx", "send": "Sfxx"}]})");

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    std::vector<std::string> problems;
    for (const json::Value& problem : error->find("errors")->as_array()) {
        problems.push_back(problem.find("file")->as_string() + " " +
                           problem.find("path")->as_string());
    }
    std::ranges::sort(problems); // folders list in any order
    CHECK(problems == std::vector<std::string>{"audio/broken.ogg ",
                                               "audio/main.bus_layout.json /buses/1/send",
                                               "audio/steps.audio.json /playback_mode"});
    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK(std::filesystem::exists(data / "audio" / "beep.wav"));
    CHECK(std::filesystem::exists(data / "audio" / "main.bus_layout.json"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship glTF models with their buffers and images, and check them first") {
    const std::filesystem::path folder = "exporter_model_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "models");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Models", "main_scene": "main.scene.json"})");
    write("main.scene.json", R"({"format": 1, "entities": {"house": {"components": {
        "Transform3D": {}, "MeshInstance3D": {"mesh": "models/house.gltf"}}}}})");
    slash::test::Gltf house = slash::test::quad(0);
    house.document["images"] = parse(R"([{"uri": "wood.jpg"}])");
    house.document["textures"] = parse(R"([{"source": 0}])");
    house.document["materials"] =
        parse(R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])");
    write("models/house.gltf", house.gltf("house.bin"));
    write("models/house.bin", house.bin);
    slash::render::Image wood;
    wood.width = 2;
    wood.height = 2;
    wood.pixels.assign(16, 128);
    write("models/wood.jpg", slash::test::make_jpeg(wood));
    house.document["images"] = parse(R"([{"uri": "gone.png"}])");
    write("models/broken.gltf", house.gltf("house.bin"));

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    std::vector<std::string> problems;
    for (const json::Value& problem : error->find("errors")->as_array()) {
        problems.push_back(problem.find("file")->as_string() + " " +
                           problem.find("path")->as_string());
    }
    CHECK(problems == std::vector<std::string>{"models/broken.gltf /images/0/uri"});
    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    CHECK(std::filesystem::exists(data / "models" / "house.gltf"));
    CHECK(std::filesystem::exists(data / "models" / "house.bin"));
    CHECK(std::filesystem::exists(data / "models" / "wood.jpg"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship .blend models as the glTF Blender makes of them, and check them first") {
    const std::filesystem::path folder = "exporter_blend_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "models");
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    write("project.json", R"({"format": 1, "name": "Models", "main_scene": "main.scene.json"})");
    write("main.scene.json", R"({"format": 1, "entities": {"house": {"components": {
        "Transform3D": {}, "MeshInstance3D": {"mesh": "models/house.blend"}}}}})");
    // Stand-ins for .blend files, which Blender's stand-in exports as the glTF after the first
    // line; it can't read the broken one.
    write("models/house.blend", "BLENDER-fake\n" + slash::test::quad().gltf());
    write("models/broken.blend", "not a blend file");
    REQUIRE(SDL_setenv_unsafe("SLASH_BLENDER_PATH", SLASH_FAKE_BLENDER, 1) == 0);

#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    SDL_unsetenv_unsafe("SLASH_BLENDER_PATH");
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    const json::Array& problems = error->find("errors")->as_array();
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("file")->as_string() == "models/broken.blend");
    CHECK(problems[0].find("message")->as_string().starts_with(
        "Blender couldn't convert it (exit code 1): "));
    // The game gets the glTF in the .blend file's place, so it never needs Blender.
    const std::filesystem::path data = folder / ".slash" / "export" / host / "data";
    auto shipped = slash::platform::read_file((data / "models" / "house.blend").string());
    REQUIRE(shipped.ok());
    CHECK(shipped.value().starts_with("glTF"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("exports ship providers.json without its replay settings, and check it first") {
    const std::filesystem::path folder = "exporter_ai_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file((folder / path).string(), text).ok());
    };
    // No main scene, so the export stops after staging.
    write("project.json", R"({"format": 1, "name": "Talk", "main_scene": "main.scene.json"})");
    write("providers.json", R"({
        "providers": {"local": {"type": "openai", "model": "llama3.2", "url": "http://localhost:11434/v1"}},
        "roles": {"dialogue": "local"},
        "replay": {"file": "tests/ai.replay.jsonl", "mode": "replay"}})");
#if defined(__APPLE__)
    const std::string host = "macos";
#elif defined(_WIN32)
    const std::string host = "windows";
#else
    const std::string host = "linux";
#endif
    exporter::ExportOptions options;
    options.target = host;
    options.project = folder.string();
    const auto problems = [&options] {
        const exporter::ExportResult result = exporter::export_game(options);
        CHECK_FALSE(result.ok);
        std::vector<std::string> found;
        const json::Value* error = result.json.find("error");
        REQUIRE(error != nullptr);
        REQUIRE(error->find("errors") != nullptr);
        for (const json::Value& problem : error->find("errors")->as_array()) {
            found.push_back(problem.find("file")->as_string() + " " +
                            problem.find("path")->as_string());
        }
        return found;
    };
    CHECK(problems() == std::vector<std::string>{"project.json /main_scene"});
    const std::filesystem::path staged =
        folder / ".slash" / "export" / host / "data" / "providers.json";
    const json::Value shipped = parse(slash::platform::read_file(staged.string()).value());
    CHECK(shipped.find("replay") == nullptr);
    CHECK(shipped.find("roles")->find("dialogue")->as_string() == "local");

    write("providers.json", R"({"providers": {}, "roles": {"dialogue": "hosted"}})");
    CHECK(problems() ==
          std::vector<std::string>{"providers.json /roles/dialogue", "project.json /main_scene"});
    CHECK_FALSE(std::filesystem::exists(staged));
    std::filesystem::remove_all(folder);
}

TEST_CASE("export.run checks its request before building") {
    command::SessionOptions options;
    options.project_root = SLASH_TEST_DATA_DIR "/game";
    Engine engine(options);
    const command::Command* run = engine.commands.find("export.run");
    REQUIRE(run != nullptr);
    CHECK(run->capabilities == std::vector<command::Capability>{command::Capability::Filesystem,
                                                                command::Capability::Build});

    command::Reply reply = engine.session->call("export.run", parse(R"({"preset": "macso"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidArgument);
    CHECK(contains(reply.message, "did you mean \"macos\""));
    CHECK(reply.data.find("step")->as_string() == "options");

    reply = engine.session->call("export.run", parse(R"({"preset": "linux", "tset": true})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidParams);

    // Building runs compilers, so it needs its own capability.
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    Engine limited(options);
    reply = limited.session->call("export.run", parse(R"({"preset": "linux"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kPermissionDenied);
    CHECK(contains(reply.message, "build"));

#ifndef __APPLE__
    // iOS games need Xcode, so only a Mac builds them.
    reply = engine.session->call("export.run", parse(R"({"preset": "ios"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(contains(reply.message, "ios games can't be built on"));
#endif
}
