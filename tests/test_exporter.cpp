#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/android.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/ios.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
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
