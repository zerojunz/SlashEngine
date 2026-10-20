#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/exporter/sdk.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace exporter = slash::exporter;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The message of the error at path in an sdk.json, or "" if there is none.
std::string error_at(std::string_view sdk, std::string_view path) {
    for (const auto& error : exporter::validate_sdk(parse(sdk))) {
        if (error.path == path) {
            return error.message;
        }
    }
    return {};
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

// Problems as "<sdk> <path>: <message>", sorted.
std::vector<std::string> describe(const std::vector<exporter::SdkProblem>& problems) {
    std::vector<std::string> out;
    for (const exporter::SdkProblem& problem : problems) {
        out.push_back(problem.sdk + " " + problem.path + ": " + problem.message);
    }
    std::ranges::sort(out);
    return out;
}

// An environment variable set for one test.
struct ScopedVariable {
    std::string name;
    ScopedVariable(std::string variable, const std::string& value) : name(std::move(variable)) {
        REQUIRE(SDL_setenv_unsafe(name.c_str(), value.c_str(), 1) == 0);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;
    ~ScopedVariable() { SDL_unsetenv_unsafe(name.c_str()); }
};

// The first bytes of an x86-64 ELF shared object, enough for is_shared_library.
std::string elf_shared_object() {
    std::string bytes(64, '\0');
    bytes.replace(0, 4,
                  "\x7f"
                  "ELF");
    bytes[4] = 2;  // 64-bit
    bytes[5] = 1;  // little-endian
    bytes[16] = 3; // ET_DYN
    return bytes;
}

constexpr std::string_view kHello = R"({
  "format": 1,
  "name": "hello",
  "version": "2.1.0",
  "license": "Proprietary",
  "license_file": "{vendor}/LICENSE.txt",
  "downloads": {"vendor": {"path": "vendor"}},
  "keys": {"HELLO_APP_ID": {"description": "The game's ID on the Hello dashboard."},
           "HELLO_REGION": {"optional": true}},
  "platforms": {
    "linux": {"include": ["{vendor}/include"],
              "libraries": ["{vendor}/lib/linux-{arch}/libhello.so"],
              "defines": ["HELLO_APP_ID=\"${HELLO_APP_ID}\""]},
    "windows": {"libraries": ["{vendor}/lib/hello.lib"], "files": ["{vendor}/bin/hello.dll"]},
    "android": {"libraries": ["{vendor}/lib/{arch}/libhello.so"],
                "permissions": ["android.permission.INTERNET"],
                "application": ["<meta-data android:name=\"hello.ID\" android:value=\"${HELLO_APP_ID}\" />"]},
    "ios": {"plist": {"HelloAppID": "${HELLO_APP_ID}", "LSApplicationQueriesSchemes": ["hello"]},
            "system_frameworks": ["StoreKit"]}
  }
})";

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

TEST_CASE("sdk.json files are checked against the schema and the rules it can't express") {
    CHECK(exporter::validate_sdk(parse(kHello)).empty());

    CHECK(contains(
        error_at(R"({"format": 1, "name": "Hello", "version": "1", "license": "MIT"})", "/name"),
        "lowercase letters"));
    // Placeholders and keys must exist, with the closest one suggested.
    constexpr std::string_view kTypos = R"({"format": 1, "name": "hello", "version": "1",
        "license": "MIT", "downloads": {"vendor": {"path": "v"}}, "keys": {"APP_ID": {}},
        "platforms": {"linux": {"include": ["{vendr}/include"], "defines": ["ID=${APP_IDS}"],
                                "libraries": ["{build}/libhello.a", "../libhello.a"]}}})";
    CHECK(error_at(kTypos, "/platforms/linux/include/0") ==
          "{vendr} is not a placeholder; did you mean {vendor}?");
    CHECK(error_at(kTypos, "/platforms/linux/defines/0") ==
          "${APP_IDS} is not one of the package's keys; did you mean ${APP_ID}? (add it to "
          "\"keys\")");
    // {build} only exists when the package builds a CMake project.
    CHECK(
        contains(error_at(kTypos, "/platforms/linux/libraries/0"), "{build} is not a placeholder"));
    CHECK(error_at(kTypos, "/platforms/linux/libraries/1") == "paths can't use \"..\"");

    // Fields belong to the platforms that have them.
    constexpr std::string_view kWrongPlatform = R"({"format": 1, "name": "hello", "version": "1",
        "license": "MIT", "platforms": {"linux": {"plist": {"A": "b"}},
                                        "ios": {"permissions": ["android.permission.INTERNET"]},
                                        "android": {"manifest": ["queries"], "defines": ["1X"]}}})";
    CHECK(error_at(kWrongPlatform, "/platforms/linux/plist") == "only macOS and iOS have plist");
    CHECK(error_at(kWrongPlatform, "/platforms/ios/permissions") == "only Android has permissions");
    CHECK(contains(error_at(kWrongPlatform, "/platforms/android/manifest/0"),
                   "expected an XML element"));
    CHECK(contains(error_at(kWrongPlatform, "/platforms/android/defines/0"), "NAME or NAME=value"));

    // Downloads are pinned by SHA-256 and fetched over https.
    constexpr std::string_view kDownloads = R"({"format": 1, "name": "hello", "version": "1",
        "license": "MIT", "downloads": {
          "plain": {"url": "http://example.com/a.zip", "sha256": "ABC"},
          "unpinned": {"url": "https://example.com/a.zip"},
          "both": {"url": "https://example.com/a.zip", "sha256": "0000000000000000000000000000000000000000000000000000000000000000", "path": "x"},
          "build": {"path": "x", "sha256": "0"}}})";
    CHECK(error_at(kDownloads, "/downloads/plain/url") == "expected an https:// URL");
    CHECK(error_at(kDownloads, "/downloads/plain/sha256") == "expected 64 lowercase hex digits");
    CHECK(contains(error_at(kDownloads, "/downloads/unpinned"), "needs its \"sha256\""));
    CHECK(contains(error_at(kDownloads, "/downloads/both"), "either a \"url\""));
    CHECK(contains(error_at(kDownloads, "/downloads/build"), "not sdk, build or arch"));

    // The schema itself: unknown fields and platforms are refused.
    CHECK_FALSE(error_at(R"({"format": 1, "name": "hello", "version": "1", "license": "MIT",
                             "platforms": {"switch": {}}})",
                         "/platforms/switch")
                    .empty());
    CHECK(exporter::sdk_schema().find("properties")->find("platforms") != nullptr);
}

TEST_CASE("projects list SDK packages and presets pick which ones they link") {
    const fs::path folder = fs::absolute("sdk_project_list");
    fs::remove_all(folder);
    write(folder / "sdks" / "hello" / "sdk.json", kHello);
    write(folder / "sdks" / "copy" / "sdk.json", kHello); // the same name again
    write(folder / "sdks" / "broken" / "sdk.json", R"({"format": 1, "name": "broken"})");
    write(folder / "sdks" / "ads" / "sdk.json", R"({"format": 1, "name": "ads", "version": "1",
        "license": "MIT", "platforms": {"android": {}, "ios": {}}})");

    std::vector<exporter::SdkProblem> problems;
    const std::vector<exporter::Sdk> sdks = exporter::load_project_sdks(
        folder, {"sdks/hello", "sdks/copy", "sdks/broken", "sdks/ads", "sdks/missing"}, problems);
    REQUIRE(sdks.size() == 2);
    CHECK(sdks[0].name == "hello");
    CHECK(sdks[0].location == "sdks/hello");
    CHECK(sdks[0].define() == "SLASH_SDK_HELLO");
    CHECK(sdks[1].name == "ads");
    const std::vector<std::string> found = describe(problems);
    INFO(found.size());
    REQUIRE(found.size() == 4);
    // The broken package lacks its version and license.
    CHECK(contains(found[0], "sdks/broken "));
    CHECK(contains(found[1], "sdks/broken "));
    CHECK(found[2] == "sdks/copy /name: \"hello\" is also the name of sdks/hello");
    CHECK(contains(found[3], "sdks/missing : "));
    CHECK(contains(found[3], "is not a folder"));

    // A preset links every package with its platform, or the ones it names.
    exporter::Preset linux_preset = exporter::find_preset({}, "linux").value();
    problems.clear();
    std::vector<const exporter::Sdk*> linked =
        exporter::sdks_for_preset(sdks, linux_preset, problems);
    REQUIRE(linked.size() == 1);
    CHECK(linked[0]->name == "hello");
    exporter::Preset android = exporter::find_preset({}, "android").value();
    CHECK(exporter::sdks_for_preset(sdks, android, problems).size() == 2);
    android.sdks = std::vector<std::string>{"ads", "helo"};
    linked = exporter::sdks_for_preset(sdks, android, problems);
    REQUIRE(linked.size() == 1);
    CHECK(linked[0]->name == "ads");
    linux_preset.sdks = std::vector<std::string>{"ads"};
    CHECK(exporter::sdks_for_preset(sdks, linux_preset, problems).empty());
    const std::vector<std::string> preset_problems = describe(problems);
    REQUIRE(preset_problems.size() == 2);
    CHECK(preset_problems[0] ==
          "export_presets.json /presets/android/sdks: preset \"android\" links the SDK \"helo\", "
          "which no package in project.json's \"sdks\" is called; did you mean \"hello\"?");
    CHECK(preset_problems[1] ==
          "sdks/ads /platforms: preset \"linux\" links it, but it has no section for linux");

    // {arch} follows each platform's builds.
    CHECK(exporter::sdk_archs(exporter::find_preset({}, "linux").value()) ==
          std::vector<std::string>{exporter::find_preset({}, "linux").value().linux_options.arch});
    CHECK(exporter::sdk_archs(exporter::find_preset({}, "macos").value()) ==
          std::vector<std::string>{"universal"});
    CHECK(exporter::sdk_archs(exporter::find_preset({}, "ios").value()) ==
          std::vector<std::string>{"simulator"});
    CHECK(exporter::sdk_archs(exporter::find_preset({}, "web").value()) ==
          std::vector<std::string>{"wasm32"});
    fs::remove_all(folder);
}

TEST_CASE("project.json lists SDK folders and presets name the SDKs they link") {
    CHECK(slash::runtime::validate_project(parse(R"({"format": 1, "name": "Game",
        "main_scene": "main.scene.json", "sdks": ["sdks/hello"]})"))
              .empty());
    const auto errors = slash::runtime::validate_project(parse(R"({"format": 1, "name": "Game",
        "main_scene": "main.scene.json", "sdks": ["../hello"]})"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/sdks/0");
    CHECK(exporter::validate_presets(parse(R"({"format": 1, "presets": {"store": {
        "platform": "android", "sdks": ["ads"]}}})"))
              .empty());
    auto presets = exporter::parse_presets(parse(R"({"format": 1, "presets": {"store": {
        "platform": "android", "sdks": ["ads"]}, "plain": {"platform": "linux"}}})"));
    REQUIRE(presets.ok());
    REQUIRE(presets.value().at("store").sdks.has_value());
    CHECK(*presets.value().at("store").sdks == std::vector<std::string>{"ads"});
    CHECK_FALSE(presets.value().at("plain").sdks.has_value());
}

TEST_CASE("SDK targets resolve their placeholders, keys and files for each build") {
    const fs::path folder = fs::absolute("sdk_resolve");
    fs::remove_all(folder);
    const fs::path vendor = folder / "vendor";
    write(vendor / "include" / "hello.h", "");
    write(vendor / "lib" / "linux-x86_64" / "libhello.so", elf_shared_object());
    write(vendor / "lib" / "hello.lib", "!<arch>\n");
    write(vendor / "bin" / "hello.dll", "MZ");
    write(vendor / "LICENSE.txt", "terms");
    write(folder / "sdk.json", kHello);
    auto sdk = exporter::load_sdk(folder);
    REQUIRE_MESSAGE(sdk.ok(), (sdk.ok() ? std::string() : sdk.error().message));

    exporter::SdkPlaces places;
    places.downloads["vendor"] = vendor;
    places.keys["HELLO_APP_ID"] = "id<&>\"";
    std::vector<exporter::SdkProblem> problems;
    const exporter::ResolvedSdk linux_sdk =
        exporter::resolve_sdk(sdk.value(), "linux", "x86_64", places, true, problems);
    CHECK(problems.empty());
    REQUIRE(linux_sdk.include.size() == 1);
    CHECK(linux_sdk.include[0] == (vendor / "include").generic_string());
    REQUIRE(linux_sdk.libraries.size() == 1);
    CHECK(linux_sdk.libraries[0] == (vendor / "lib/linux-x86_64/libhello.so").generic_string());
    CHECK(linux_sdk.defines == std::vector<std::string>{"HELLO_APP_ID=\"id<&>\"\""});
    CHECK(linux_sdk.license_file == (vendor / "LICENSE.txt").generic_string());

    // A file that isn't there is a problem at its pointer, for that {arch}.
    (void)exporter::resolve_sdk(sdk.value(), "linux", "arm64", places, true, problems);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/platforms/linux/libraries/0");
    CHECK(contains(problems[0].message, "lib/linux-arm64/libhello.so does not exist"));
    problems.clear();
    // Unless paths aren't checked, as when a package's files come from a build not run yet.
    (void)exporter::resolve_sdk(sdk.value(), "linux", "arm64", places, false, problems);
    CHECK(problems.empty());

    // Android's XML gets the keys escaped; the permission is listed once however many ask.
    const exporter::ResolvedSdk android =
        exporter::resolve_sdk(sdk.value(), "android", "arm64-v8a", places, false, problems);
    REQUIRE(android.application.size() == 1);
    CHECK(android.application[0] ==
          "<meta-data android:name=\"hello.ID\" android:value=\"id&lt;&amp;&gt;&quot;\" />");
    const std::string cmake = exporter::sdk_cmake({android, android}, json::Object{});
    CHECK(cmake.find("<uses-permission android:name=\"android.permission.INTERNET\" />") ==
          cmake.rfind("<uses-permission android:name=\"android.permission.INTERNET\" />"));
    CHECK(contains(cmake, "string(APPEND SLASH_SDK_ANDROID_APPLICATION"));

    // Windows links the import library and ships the DLL; a DLL in "libraries" is a mistake.
    const exporter::ResolvedSdk windows =
        exporter::resolve_sdk(sdk.value(), "windows", "x86_64", places, true, problems);
    CHECK(problems.empty());
    CHECK(windows.files == std::vector<std::string>{(vendor / "bin/hello.dll").generic_string()});
    exporter::Sdk dll = sdk.value();
    dll.platforms["windows"].libraries = {"{vendor}/bin/hello.dll"};
    (void)exporter::resolve_sdk(dll, "windows", "x86_64", places, true, problems);
    REQUIRE(problems.size() == 1);
    CHECK(contains(problems[0].message, "hello.dll is a DLL: link its import library"));
    problems.clear();

    // A {build} path waits for the package's CMake project to be built.
    exporter::Sdk built = sdk.value();
    built.platforms["linux"].libraries = {"{build}/lib/libhello.a"};
    CHECK(
        exporter::resolve_sdk(built, "linux", "x86_64", places, true, problems).libraries.empty());
    CHECK(problems.empty());

    // Info.plist entries take the keys and merge across packages; a clash is a problem.
    const exporter::ResolvedSdk ios =
        exporter::resolve_sdk(sdk.value(), "ios", "simulator", places, true, problems);
    CHECK(ios.plist.find("HelloAppID")->as_string() == "id<&>\"");
    exporter::ResolvedSdk other = ios;
    exporter::Sdk other_sdk = sdk.value();
    other_sdk.name = "other";
    other.sdk = &other_sdk;
    other.plist["HelloAppID"] = "something else";
    other.plist["OtherKey"] = true;
    const json::Value merged = exporter::merge_sdk_entries({ios, other}, "plist", problems);
    CHECK(merged.find("HelloAppID")->as_string() == "id<&>\"");
    CHECK(merged.find("OtherKey")->as_bool());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/platforms/ios/plist/HelloAppID");
    CHECK(problems[0].message == "the SDK hello sets HelloAppID to something else");

    const std::string entries = exporter::plist_entries(merged);
    CHECK(contains(entries, "<key>HelloAppID</key>"));
    CHECK(contains(entries, "<string>id&lt;&amp;&gt;&quot;</string>"));
    CHECK(contains(entries, "<true/>"));
    CHECK(contains(exporter::plist_document(merged), "<plist version=\"1.0\">"));
    fs::remove_all(folder);
}

TEST_CASE("the CMake that links SDK packages names each library, framework and file") {
    const fs::path folder = fs::absolute("sdk_cmake");
    fs::remove_all(folder);
    write(folder / "libshared.so", elf_shared_object());
    write(folder / "libstatic.a", "!<arch>\n");
    write(folder / "app.dll", "MZ" + std::string(62, '\0'));
    CHECK(exporter::is_shared_library(folder / "libshared.so"));
    CHECK_FALSE(exporter::is_shared_library(folder / "libstatic.a"));
    CHECK(exporter::is_shared_library(folder / "app.dll"));
    // Before a build, the file name decides.
    CHECK(exporter::is_shared_library(folder / "missing.dylib"));
    CHECK_FALSE(exporter::is_shared_library(folder / "missing.a"));

    exporter::Sdk sdk;
    sdk.name = "hello-kit";
    exporter::ResolvedSdk resolved;
    resolved.sdk = &sdk;
    resolved.platform = "linux";
    resolved.defines = {"HELLO_ID=\"]]\"", "TRAILING=]"};
    resolved.include = {"/opt/hello/include"};
    resolved.libraries = {(folder / "libshared.so").generic_string(),
                          (folder / "libstatic.a").generic_string()};
    resolved.system_libraries = {"dl"};
    resolved.files = {"/opt/hello/data.txt"};
    json::Value plist;
    plist["HelloAppID"] = "abc";
    const std::string cmake = exporter::sdk_cmake({resolved}, plist);
    INFO(cmake);
    // Bracket arguments, with '='s enough that a value can't close its own.
    CHECK(contains(cmake, "slash_game_sdk([[hello-kit]]\n"));
    CHECK(contains(cmake, "    DEFINES\n        [[SLASH_SDK_HELLO_KIT]]\n"
                          "        [=[HELLO_ID=\"]]\"]=]\n        [=[TRAILING=]]=]\n"));
    CHECK(contains(cmake, "    INCLUDE\n        [[/opt/hello/include]]\n"));
    CHECK(contains(cmake, "    SHARED\n        [[" + (folder / "libshared.so").generic_string() +
                              "]]\n    SYSTEM_LIBRARIES"));
    CHECK(contains(cmake, "    SYSTEM_LIBRARIES\n        [[dl]]\n"));
    CHECK(contains(cmake, "    FILES\n        [[/opt/hello/data.txt]]\n)\n"));
    CHECK(contains(cmake, "string(APPEND SLASH_SDK_PLIST [[<key>HelloAppID</key>\n"));
    CHECK_FALSE(contains(cmake, "FRAMEWORKS"));
    fs::remove_all(folder);
}

TEST_CASE("SDK glue code: sources, Java, backends by class name and a register function") {
    constexpr std::string_view kGlue = R"({"format": 1, "name": "hello", "version": "1",
        "license": "MIT", "register": "hello::register_backends",
        "platforms": {
          "linux": {"sources": ["glue/hello.cpp", "glue"]},
          "android": {"sources": ["glue/hello.cpp"], "java": ["glue/android", "Extra.java"],
                      "backends": {"ads": "com.example.hello.HelloAds",
                                   "analytics": ["com.example.hello.Hello$Analytics"]}},
          "ios": {"sources": ["glue/HelloAds.mm"], "backends": {"purchases": "HelloStore"}}}})";
    CHECK(exporter::validate_sdk(parse(kGlue)).empty());

    constexpr std::string_view kWrong = R"({"format": 1, "name": "hello", "version": "1",
        "license": "MIT", "register": "hello:register",
        "platforms": {
          "linux": {"sources": ["glue/hello.mm", "glue/hello.py"], "java": ["A.java"],
                    "backends": {"ads": "HelloAds"}},
          "android": {"java": ["glue/hello.kt"],
                      "backends": {"ads": "com.example.2d.Ads", "analytics": ["Hello Analytics"]}},
          "macos": {"backends": {"analytics": ["com.example.Hello"]}}}})";
    CHECK(error_at(kWrong, "/platforms/linux/sources/0") ==
          "only macOS and iOS compile Objective-C");
    CHECK(contains(error_at(kWrong, "/platforms/linux/sources/1"), "expected a C or C++ file"));
    CHECK(error_at(kWrong, "/platforms/linux/java") == "only Android has java");
    CHECK(contains(error_at(kWrong, "/platforms/linux/backends"),
                   "only Android, macOS and iOS have backends by class name"));
    CHECK(error_at(kWrong, "/platforms/android/java/0") == "expected a .java file or a folder");
    CHECK(contains(error_at(kWrong, "/platforms/android/backends/ads"), "Java class's full name"));
    CHECK(contains(error_at(kWrong, "/platforms/android/backends/analytics/0"),
                   "Java class's full name"));
    CHECK(contains(error_at(kWrong, "/platforms/macos/backends/analytics/0"),
                   "Objective-C class's name"));
    CHECK(contains(error_at(kWrong, "/register"), "expected a C++ function's name"));
    // The function must be defined somewhere.
    CHECK(error_at(R"({"format": 1, "name": "hello", "version": "1", "license": "MIT",
                       "register": "register_hello", "platforms": {"linux": {}}})",
                   "/register") == "no platform has \"sources\" to define it in");

    // Folders of sources resolve to their files for the platform, in order.
    const fs::path folder = fs::absolute("sdk_glue");
    fs::remove_all(folder);
    write(folder / "sdk.json", kGlue);
    write(folder / "glue" / "hello.cpp", "");
    write(folder / "glue" / "more" / "store.c", "");
    write(folder / "glue" / "HelloAds.mm", "");
    write(folder / "glue" / "README.md", "");
    write(folder / "glue" / "android" / "com" / "example" / "hello" / "HelloAds.java", "");
    write(folder / "Extra.java", "");
    auto sdk = exporter::load_sdk(folder);
    REQUIRE_MESSAGE(sdk.ok(), (sdk.ok() ? std::string() : sdk.error().message));
    CHECK(sdk.value().register_function == "hello::register_backends");
    std::vector<exporter::SdkProblem> problems;
    const exporter::ResolvedSdk linux_sdk =
        exporter::resolve_sdk(sdk.value(), "linux", "x86_64", {}, true, problems);
    CHECK(problems.empty());
    const std::string glue = (folder / "glue").generic_string();
    CHECK(linux_sdk.sources == std::vector<std::string>{glue + "/hello.cpp", glue + "/hello.cpp",
                                                        glue + "/more/store.c"});
    CHECK(linux_sdk.register_function == "hello::register_backends");
    const exporter::ResolvedSdk android =
        exporter::resolve_sdk(sdk.value(), "android", "x86_64", {}, true, problems);
    CHECK(problems.empty());
    CHECK(android.java ==
          std::vector<std::string>{glue + "/android", (folder / "Extra.java").generic_string()});
    CHECK(android.backends.ads == "com.example.hello.HelloAds");
    // Exports only call the function where the package's code is compiled in.
    exporter::Sdk no_sources = sdk.value();
    no_sources.platforms["linux"].sources.clear();
    CHECK(exporter::resolve_sdk(no_sources, "linux", "x86_64", {}, true, problems)
              .register_function.empty());
    // A folder with no sources for the platform is a mistake.
    fs::create_directories(folder / "empty");
    no_sources.platforms["linux"].sources = {"empty"};
    (void)exporter::resolve_sdk(no_sources, "linux", "x86_64", {}, true, problems);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/platforms/linux/sources");
    CHECK(contains(problems[0].message, "has no source files for linux"));
    problems.clear();

    const std::string cmake =
        exporter::sdk_cmake({android}, json::Object{}, "/build/sdk_register.cpp");
    INFO(cmake);
    CHECK(contains(cmake, "    SOURCES\n        [[" + glue + "/hello.cpp]]\n"));
    CHECK(contains(cmake, "    JAVA\n        [[" + glue + "/android]]\n"));
    CHECK(contains(cmake, "slash_game_sdk_register([[/build/sdk_register.cpp]])\n"));
    CHECK_FALSE(contains(exporter::sdk_cmake({android}, json::Object{}), "register"));

    // The register code calls each package's function and makes its classes' backends.
    const std::string source = exporter::sdk_register_source({linux_sdk}, problems);
    INFO(source);
    CHECK(contains(source, "namespace hello {\n"
                           "void register_backends(slash::services::Backends& backends);\n"
                           "} // namespace\n"));
    CHECK(contains(source, "        ::hello::register_backends(added);\n"
                           "        slash::services::merge_backends(backends, std::move(added), "
                           "\"hello\");\n"));
    CHECK_FALSE(contains(source, "bridges.hpp"));
    const std::string android_source = exporter::sdk_register_source({android}, problems);
    INFO(android_source);
    CHECK(contains(android_source, "#include \"slash/services/bridges.hpp\""));
    CHECK(contains(android_source,
                   "        added.ads = "
                   "slash::services::java_ads_backend(\"com.example.hello.HelloAds\");\n"));
    CHECK(contains(android_source,
                   "        added.analytics.push_back(slash::services::"
                   "java_analytics_backend(\"com.example.hello.Hello$Analytics\"));\n"));
    const exporter::ResolvedSdk ios =
        exporter::resolve_sdk(sdk.value(), "ios", "simulator", {}, true, problems);
    CHECK(contains(
        exporter::sdk_register_source({ios}, problems),
        "        added.purchases = slash::services::objc_purchases_backend(\"HelloStore\");\n"));
    CHECK(problems.empty());
    // Nothing to register, no file.
    CHECK(exporter::sdk_register_source({}, problems).empty());

    // Two packages can't both give the game's ads.
    exporter::Sdk other_sdk = sdk.value();
    other_sdk.name = "other";
    other_sdk.location = "sdks/other";
    exporter::ResolvedSdk other = android;
    other.sdk = &other_sdk;
    (void)exporter::sdk_register_source({android, other}, problems);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].sdk == "sdks/other");
    CHECK(problems[0].path == "/platforms/android/backends/ads");
    CHECK(problems[0].message ==
          "the SDK hello gives the ads backend too, and a game's ads run on one");
    fs::remove_all(folder);
}

TEST_CASE("sdk.check reports every problem with the project's packages") {
    const fs::path folder = fs::absolute("sdk_check_project");
    fs::remove_all(folder);
    write(folder / "project.json", R"({"format": 1, "name": "Game",
        "main_scene": "main.scene.json", "sdks": ["sdks/hello", "sdks/pinned"]})");
    write(folder / "main.scene.json", R"({"format": 1, "entities": {}})");
    write(folder / "export_presets.json", R"({"format": 1, "presets": {
        "store": {"platform": "linux", "sdks": ["hello", "pined"]}}})");
    write(folder / "sdks" / "hello" / "sdk.json", kHello);
    write(folder / "sdks" / "hello" / "vendor" / "include" / "hello.h", "");
    write(folder / "sdks" / "hello" / "vendor" / "LICENSE.txt", "terms");
    write(folder / "sdks" / "pinned" / "sdk.json", R"({"format": 1, "name": "pinned",
        "version": "1", "license": "MIT", "downloads": {"kit": {
          "url": "https://example.invalid/kit.zip",
          "sha256": "0000000000000000000000000000000000000000000000000000000000000000"}},
        "platforms": {"linux": {"include": ["{kit}/include"]}}})");
    const ScopedVariable sdks_dir(std::string(exporter::kSdksDirVariable),
                                  (folder / "cache").string());

    const json::Value checked = exporter::check_sdks(folder, "linux", false);
    INFO(json::write(checked));
    REQUIRE(checked.find("sdks")->as_array().size() == 2);
    const json::Value& pinned = checked.find("sdks")->as_array()[1];
    CHECK(pinned.find("name")->as_string() == "pinned");
    // Downloads not fetched yet aren't fetched to be checked unless asked.
    CHECK(*pinned.find("unchecked") == json::Value(json::Array{"kit"}));
    std::vector<std::string> problems;
    for (const json::Value& problem : checked.find("problems")->as_array()) {
        const json::Value* path = problem.find("path");
        problems.push_back(problem.find("sdk")->as_string() + " " +
                           (path != nullptr ? path->as_string() : std::string()) + ": " +
                           problem.find("message")->as_string());
    }
    std::ranges::sort(problems);
    REQUIRE(problems.size() == 3);
    CHECK(contains(problems[0], "export_presets.json /presets/store/sdks: preset \"store\" links "
                                "the SDK \"pined\""));
    CHECK(contains(problems[0], "did you mean \"pinned\"?"));
    CHECK(problems[1] == "sdks/hello /keys/HELLO_APP_ID: set the environment variable "
                         "HELLO_APP_ID: The game's ID on the Hello dashboard.");
    CHECK(contains(problems[2], "sdks/hello /platforms/linux/libraries/0: "));
    CHECK(contains(problems[2], "libhello.so does not exist"));

    // Without keys, as project validate checks: only what is in the project.
    const json::Value files_only = exporter::check_sdks(folder, "linux", false, false);
    CHECK(files_only.find("problems")->as_array().size() == 2);

    // The command reports the same, and fetching needs the build capability.
    command::SessionOptions options;
    options.project_root = folder.string();
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    Engine engine(options);
    command::Reply reply = engine.session->call("sdk.check", parse(R"({"platform": "linux"})"));
    REQUIRE(reply.ok);
    CHECK_FALSE(reply.value.find("ok")->as_bool());
    CHECK(reply.value.find("problems")->as_array().size() == 3);
    reply = engine.session->call("sdk.check", parse(R"({"fetch": true})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kPermissionDenied);
    CHECK(reply.data.find("path")->as_string() == "/fetch");
    reply = engine.session->call("sdk.check", parse(R"({"platform": "switch"})"));
    CHECK_FALSE(reply.ok);
    fs::remove_all(folder);
}

TEST_CASE("sdk.add lists a package in project.json, copying it in from elsewhere") {
    const fs::path folder = fs::absolute("sdk_add_project");
    const fs::path elsewhere = fs::absolute("sdk_add_elsewhere");
    fs::remove_all(folder);
    fs::remove_all(elsewhere);
    write(folder / "project.json",
          R"({"format": 1, "name": "Game", "main_scene": "main.scene.json"})");
    write(folder / "main.scene.json", R"({"format": 1, "entities": {}})");
    write(folder / "vendor" / "greeter" / "sdk.json", R"({"format": 1, "name": "greeter",
        "version": "0.3.0", "license": "MIT", "keys": {"GREETER_KEY": {}},
        "platforms": {"linux": {"files": ["greetings.txt"]}}})");
    write(folder / "vendor" / "greeter" / "greetings.txt", "hi");
    write(elsewhere / "kit" / "sdk.json", kHello);
    write(elsewhere / "kit" / "vendor" / "LICENSE.txt", "terms");
    const auto listed = [&] {
        auto text = slash::platform::read_file((folder / "project.json").string());
        REQUIRE(text.ok());
        const json::Value project = parse(text.value());
        const json::Value* sdks = project.find("sdks");
        return sdks != nullptr ? *sdks : json::Value(json::Array{});
    };

    // A folder in the project is listed where it is.
    exporter::AddSdkOptions options;
    options.project = folder;
    options.source = (folder / "vendor" / "greeter").string();
    options.dry_run = true;
    auto added = exporter::add_sdk(options);
    REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
    CHECK(added.value().find("dry_run")->as_bool());
    CHECK(listed().as_array().empty());
    options.dry_run = false;
    added = exporter::add_sdk(options);
    REQUIRE(added.ok());
    CHECK(added.value().find("folder")->as_string() == "vendor/greeter");
    CHECK_FALSE(added.value().find("copied")->as_bool());
    CHECK(*added.value().find("keys") == json::Value(json::Array{"GREETER_KEY"}));
    CHECK(listed() == json::Value(json::Array{"vendor/greeter"}));
    // Adding it again lists it once.
    REQUIRE(exporter::add_sdk(options).ok());
    CHECK(listed().as_array().size() == 1);

    // A folder elsewhere is copied into sdks/<name>.
    options.source = (elsewhere / "kit").string();
    added = exporter::add_sdk(options);
    REQUIRE(added.ok());
    CHECK(added.value().find("folder")->as_string() == "sdks/hello");
    CHECK(added.value().find("copied")->as_bool());
    CHECK(fs::is_regular_file(folder / "sdks" / "hello" / "vendor" / "LICENSE.txt"));
    CHECK(listed() == json::Value(json::Array{"vendor/greeter", "sdks/hello"}));
    auto again = exporter::add_sdk(options);
    REQUIRE_FALSE(again.ok());
    CHECK(again.error().code == slash::ErrorCode::Conflict);
    options.name = "Hello Kit";
    CHECK_FALSE(exporter::add_sdk(options).ok());
    options.name.clear();

    // http:// is never used, and a folder without sdk.json is refused.
    options.source = "http://example.com/kit.zip";
    CHECK(contains(exporter::add_sdk(options).error().message, "never http://"));
    options.source = elsewhere.string();
    CHECK(contains(exporter::add_sdk(options).error().message, "sdk.json"));

    // The command adds folders of the project; paths can't leave it, and URLs need "build".
    command::SessionOptions session_options;
    session_options.project_root = folder.string();
    session_options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    Engine engine(session_options);
    command::Reply reply =
        engine.session->call("sdk.add", parse(R"({"source": "../sdk_add_elsewhere/kit"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.data.find("path")->as_string() == "/source");
    reply = engine.session->call("sdk.add", parse(R"({"source": "https://example.com/kit.zip"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kPermissionDenied);
    reply =
        engine.session->call("sdk.add", parse(R"({"source": "vendor/greeter", "dry_run": true})"));
    REQUIRE(reply.ok);
    CHECK(reply.value.find("name")->as_string() == "greeter");
    fs::remove_all(folder);
    fs::remove_all(elsewhere);
}

#ifndef _WIN32
TEST_CASE("SDK downloads are fetched once, checked against their SHA-256 and unpacked") {
    const fs::path folder = fs::absolute("sdk_downloads");
    fs::remove_all(folder);
    write(folder / "src" / "kit-1.0" / "include" / "kit.h", "int kit();\n");
    write(folder / "src" / "kit-1.0" / "sdk.json", R"({"format": 1, "name": "kit",
        "version": "1.0", "license": "MIT", "platforms": {"linux": {"include": ["include"]}}})");
    const fs::path archive = folder / "kit-1.0.tar.gz";
    slash::platform::ProcessOptions process;
    process.working_directory = (folder / "src").string();
    auto packed = slash::platform::run_process(
        {"cmake", "-E", "tar", "czf", archive.string(), "kit-1.0"}, process);
    REQUIRE(packed.ok());
    REQUIRE(packed.value().exit_code == 0);
    auto bytes = slash::platform::read_file(archive.string());
    REQUIRE(bytes.ok());
    const std::string sha256 = exporter::sha256_hex(bytes.value());

    exporter::SdkDownload download;
    download.id = "kit";
    download.url = "file://" + archive.generic_string();
    download.sha256 = exporter::sha256_hex("something else");
    download.folder = "kit-1.0";
    const fs::path dir = folder / "cache";
    auto rejected = exporter::fetch_download(download, folder, dir);
    REQUIRE_FALSE(rejected.ok());
    CHECK(contains(rejected.error().message, "has SHA-256 " + sha256));
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 0);

    download.sha256 = sha256;
    auto fetched = exporter::fetch_download(download, folder, dir);
    REQUIRE_MESSAGE(fetched.ok(), (fetched.ok() ? std::string() : fetched.error().message));
    CHECK(fetched.value() == exporter::download_location(download, folder, dir));
    CHECK(fs::is_regular_file(fetched.value() / "include" / "kit.h"));
    // Once fetched, the URL isn't asked again.
    exporter::SdkDownload moved = download;
    moved.url = "file://" + (folder / "missing.tar.gz").generic_string();
    auto again = exporter::fetch_download(moved, folder, dir);
    REQUIRE(again.ok());
    CHECK(again.value() == fetched.value());

    // A local path, with an environment variable.
    exporter::SdkDownload local;
    local.id = "local";
    local.path = "${SLASH_TEST_SDK_ROOT}/kit-1.0";
    CHECK(contains(exporter::fetch_download(local, folder, dir).error().message,
                   "set SLASH_TEST_SDK_ROOT"));
    {
        const ScopedVariable root("SLASH_TEST_SDK_ROOT", (folder / "src").string());
        auto found = exporter::fetch_download(local, folder, dir);
        REQUIRE(found.ok());
        CHECK(found.value() == folder / "src" / "kit-1.0");
    }

    // sdk add takes the archive too, checked against the hash given.
    write(folder / "game" / "project.json",
          R"({"format": 1, "name": "Game", "main_scene": "main.scene.json"})");
    exporter::AddSdkOptions options;
    options.project = folder / "game";
    options.source = download.url;
    options.sha256 = exporter::sha256_hex("something else");
    CHECK(contains(exporter::add_sdk(options).error().message, "has SHA-256"));
    options.sha256 = sha256;
    auto added = exporter::add_sdk(options);
    REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
    CHECK(added.value().find("folder")->as_string() == "sdks/kit");
    CHECK(fs::is_regular_file(folder / "game" / "sdks" / "kit" / "include" / "kit.h"));
    // Nothing is left in the project's work folder.
    CHECK(std::distance(fs::directory_iterator(folder / "game" / ".slash" / "sdk-add"),
                        fs::directory_iterator()) == 0);
    fs::remove_all(folder);
}
#endif
