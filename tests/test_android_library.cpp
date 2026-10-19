#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/android.hpp"
#include "slash/exporter/android_library.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/maven.hpp"
#include "slash/exporter/sdk.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/exporter/xml.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
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
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE_MESSAGE(text.ok(), path.string());
    return std::move(text).value();
}

exporter::XmlElement xml(std::string_view text) {
    auto parsed = exporter::parse_xml(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string xml_error(std::string_view text) {
    auto parsed = exporter::parse_xml(text);
    REQUIRE_FALSE(parsed.ok());
    return parsed.error().message;
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// The android:name of each child of element called tag, in order.
std::vector<std::string> names(const exporter::XmlElement& element, std::string_view tag) {
    std::vector<std::string> out;
    for (const exporter::XmlElement& child : element.children) {
        if (child.name == tag) {
            const std::string* name = child.attribute("android:name");
            out.push_back(name != nullptr ? *name : std::string());
        }
    }
    return out;
}

// The child of element called tag whose android:name is name.
const exporter::XmlElement* named(const exporter::XmlElement& element, std::string_view tag,
                                  std::string_view name) {
    for (const exporter::XmlElement& child : element.children) {
        const std::string* value = child.attribute("android:name");
        if (child.name == tag && value != nullptr && *value == name) {
            return &child;
        }
    }
    return nullptr;
}

constexpr std::string_view kAppManifest = R"(<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.example.game">
    <uses-permission android:name="android.permission.VIBRATE" />
    <uses-feature android:glEsVersion="0x00030000" />
    <application android:label="Game" android:hasCode="true">
        <activity android:name="com.example.game.GameActivity" android:exported="true" />
    </application>
</manifest>
)";

// A Maven repository in a folder, reached with a file:// address.
struct Repository {
    fs::path root;

    [[nodiscard]] std::string url() const { return "file://" + root.generic_string(); }

    [[nodiscard]] fs::path folder(std::string_view group, std::string_view artifact,
                                  std::string_view version) const {
        std::string path(group);
        for (char& c : path) {
            c = c == '.' ? '/' : c;
        }
        return root / path / artifact / version;
    }

    // A POM with body inside <project>, and a file of packaging's type unless it is "pom".
    void add(std::string_view group, std::string_view artifact, std::string_view version,
             std::string_view body, std::string_view packaging = "jar") const {
        const fs::path dir = folder(group, artifact, version);
        const std::string stem = std::string(artifact) + "-" + std::string(version);
        write(dir / (stem + ".pom"),
              "<?xml version=\"1.0\"?>\n<project><modelVersion>4.0.0</modelVersion><groupId>" +
                  std::string(group) + "</groupId><artifactId>" + std::string(artifact) +
                  "</artifactId><version>" + std::string(version) + "</version><packaging>" +
                  std::string(packaging) + "</packaging>" + std::string(body) + "</project>\n");
        if (packaging != "pom") {
            write(dir / (stem + "." + std::string(packaging)),
                  std::string(artifact) + " " + std::string(version));
        }
    }

    void list(std::string_view group, std::string_view artifact,
              const std::vector<std::string>& versions) const {
        std::string body = "<metadata><versioning><versions>";
        for (const std::string& version : versions) {
            body += "<version>" + version + "</version>";
        }
        body += "</versions></versioning></metadata>";
        write(folder(group, artifact, "") / "maven-metadata.xml", body);
    }
};

std::string dependency(std::string_view group, std::string_view artifact, std::string_view version,
                       std::string_view extra = {}) {
    return "<dependency><groupId>" + std::string(group) + "</groupId><artifactId>" +
           std::string(artifact) + "</artifactId>" +
           (version.empty() ? std::string() : "<version>" + std::string(version) + "</version>") +
           std::string(extra) + "</dependency>";
}

// "id type" for each artifact.
std::vector<std::string> describe(const std::vector<exporter::MavenArtifact>& artifacts) {
    std::vector<std::string> out;
    for (const exporter::MavenArtifact& artifact : artifacts) {
        out.push_back(artifact.id + " " + artifact.type);
    }
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

TEST_CASE("XML is read and written as Android's tools write it") {
    const exporter::XmlElement manifest = xml(
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- a comment -->\n"
        "<!DOCTYPE manifest>\n"
        "<manifest xmlns:android='http://schemas.android.com/apk/res/android' package=\"a.b\">\n"
        "  <application>\n"
        "    <meta-data android:name=\"k\" android:value=\"a &amp; b &#x263A; &#65;\"/>\n"
        "  </application>\n"
        "  <note>  <![CDATA[<raw> & text]]>  </note>\n"
        "</manifest>\n");
    CHECK(manifest.name == "manifest");
    REQUIRE(manifest.attribute("package") != nullptr);
    CHECK(*manifest.attribute("package") == "a.b");
    CHECK(manifest.child_text("note") == "<raw> & text");
    const exporter::XmlElement* application = manifest.child("application");
    REQUIRE(application != nullptr);
    REQUIRE(application->children.size() == 1);
    CHECK(*application->children[0].attribute("android:value") == "a & b \xE2\x98\xBA A");

    // Written with a declaration, four-space indents and one attribute a line where there are
    // several, and read back the same.
    const std::string written = exporter::write_xml(manifest);
    CHECK(written.starts_with("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<manifest\n"
                              "    xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
                              "    package=\"a.b\">\n"));
    CHECK(contains(written, "android:value=\"a &amp; b \xE2\x98\xBA A\" />"));
    CHECK(contains(written, "    <note>&lt;raw&gt; &amp; text</note>\n"));
    CHECK(exporter::write_xml(xml(written)) == written);

    CHECK(xml_error("") == "line 1: expected the root element");
    CHECK(xml_error("<a>\n<b></a>") == "line 2: </a> closes <b>");
    CHECK(xml_error("<a x='1' x='2'/>") == "line 1: <a> has the attribute x twice");
    CHECK(xml_error("<a>\n\n&nbsp;</a>") == "line 3: unknown entity &nbsp;");
    CHECK(xml_error("<a>") == "line 1: <a> is never closed");
    CHECK(xml_error("<a/><b/>") == "line 1: unexpected content after the root element");
    CHECK(contains(xml_error("<!DOCTYPE a [<!ENTITY x 'y'>]><a/>"), "internal subset"));
}

TEST_CASE("Maven coordinates and versions are read and ordered as Gradle does") {
    auto core = exporter::parse_maven_coordinate("androidx.core:core:1.13.1");
    REQUIRE(core.ok());
    CHECK(core.value().group == "androidx.core");
    CHECK(core.value().artifact == "core");
    CHECK(core.value().version == "1.13.1");
    CHECK(core.value().module() == "androidx.core:core");
    CHECK(core.value().id() == "androidx.core:core:1.13.1");
    auto classified = exporter::parse_maven_coordinate("com.example:kit:2.0:natives@aar");
    REQUIRE(classified.ok());
    CHECK(classified.value().classifier == "natives");
    CHECK(classified.value().type == "aar");
    CHECK(classified.value().id() == "com.example:kit:2.0:natives");
    auto ranged = exporter::parse_maven_coordinate("com.example:kit:[1.0,2.0)");
    REQUIRE(ranged.ok());
    CHECK(ranged.value().version == "[1.0,2.0)");

    for (const char* bad : {"androidx.core:core", "a:b:c:d:e", "a:b:1.0@zip", "a b:c:1.0",
                            "a:..:1.0", "a:b:../1.0", "a:b:[1.0", ":b:1.0"}) {
        INFO(bad);
        auto parsed = exporter::parse_maven_coordinate(bad);
        REQUIRE_FALSE(parsed.ok());
        CHECK(contains(parsed.error().message, "is not a Maven coordinate"));
    }

    const auto older = [](std::string_view a, std::string_view b) {
        return exporter::compare_maven_versions(a, b) < 0 &&
               exporter::compare_maven_versions(b, a) > 0;
    };
    CHECK(older("1.9", "1.10"));
    CHECK(older("1.0", "1.0.1"));
    CHECK(older("1.0-rc1", "1.0"));
    CHECK(older("1.0-alpha01", "1.0-beta01"));
    CHECK(older("1.0-beta01", "1.0-rc01"));
    CHECK(older("1.0-dev", "1.0-alpha"));
    CHECK(older("1.0-rc", "1.0-release"));
    CHECK(older("1.0-a", "1.0.1"));
    CHECK(older("1.0a", "1.0.0"));
    CHECK(exporter::compare_maven_versions("1.01", "1.1") == 0);
    CHECK(exporter::compare_maven_versions("1.0-RC1", "1.0-rc1") == 0);

    CHECK(exporter::is_maven_range("[1.0,2.0)"));
    CHECK(exporter::is_maven_range("(,1.0],[1.2,)"));
    CHECK(exporter::is_maven_range("[1.5]"));
    CHECK_FALSE(exporter::is_maven_range("1.0"));
    CHECK_FALSE(exporter::is_maven_range("[1.0"));
    CHECK(exporter::in_maven_range("1.5", "[1.0,2.0)"));
    CHECK(exporter::in_maven_range("1.0", "[1.0,2.0)"));
    CHECK_FALSE(exporter::in_maven_range("2.0", "[1.0,2.0)"));
    CHECK_FALSE(exporter::in_maven_range("1.1", "(,1.0],[1.2,)"));
    CHECK(exporter::in_maven_range("1.3", "(,1.0],[1.2,)"));
    CHECK(exporter::in_maven_range("1.5", "[1.5]"));
}

TEST_CASE("sdk.lock.json files round trip and joined graphs keep the newest version") {
    exporter::MavenLock lock;
    lock.dependencies = {"com.example:kit:1.0"};
    lock.repositories = {"https://repo.example/maven2"};
    lock.artifacts = {{"com.example:kit:1.0", "aar", "https://repo.example/maven2/kit.aar",
                       std::string(64, 'a')}};
    const json::Value written = exporter::maven_lock_json(lock);
    auto read_back = exporter::parse_maven_lock(written);
    REQUIRE(read_back.ok());
    CHECK(read_back.value().dependencies == lock.dependencies);
    CHECK(read_back.value().repositories == lock.repositories);
    CHECK(describe(read_back.value().artifacts) ==
          std::vector<std::string>{"com.example:kit:1.0 aar"});
    CHECK(read_back.value().artifacts[0].url == lock.artifacts[0].url);
    CHECK(read_back.value().artifacts[0].sha256 == lock.artifacts[0].sha256);

    json::Value bad = written;
    bad["artifacts"].as_array()[0]["sha256"] = "ABC";
    CHECK(exporter::parse_maven_lock(bad).error().message ==
          "/maven/artifacts/0/sha256: expected 64 lowercase hex digits");
    bad = written;
    bad["artifacts"].as_array()[0]["id"] = "com.example:../../escape:1.0";
    CHECK(contains(exporter::parse_maven_lock(bad).error().message, "/maven/artifacts/0"));
    bad = written;
    bad["artifacts"].as_array()[0]["type"] = "zip";
    CHECK(contains(exporter::parse_maven_lock(bad).error().message, "aar or jar"));

    // Files live at their Maven paths in the cache.
    CHECK(exporter::maven_artifact_path(lock.artifacts[0], "cache") ==
          fs::path("cache") / "com/example/kit/1.0" / "kit-1.0.aar");
    CHECK(exporter::maven_artifact_path({"com.example:kit:1.0:natives", "jar", "", ""}, "c") ==
          fs::path("c") / "com/example/kit/1.0" / "kit-1.0-natives.jar");

    // Two packages pinning different versions of a module link the newer, in first-seen order.
    const std::vector<exporter::MavenArtifact> a{{"x:core:1.2", "aar", "", ""},
                                                 {"x:util:1.0", "jar", "", ""}};
    const std::vector<exporter::MavenArtifact> b{{"x:core:1.10", "aar", "", ""},
                                                 {"x:util:1.0", "jar", "", ""},
                                                 {"x:util:1.0:natives", "jar", "", ""},
                                                 {"y:extra:3.0", "jar", "", ""}};
    CHECK(describe(exporter::merge_maven_artifacts({a, b})) ==
          std::vector<std::string>{"x:core:1.10 aar", "x:util:1.0 jar", "x:util:1.0:natives jar",
                                   "y:extra:3.0 jar"});

    CHECK(exporter::maven_repositories({}) ==
          std::vector<std::string>(exporter::kDefaultMavenRepositories.begin(),
                                   exporter::kDefaultMavenRepositories.end()));
    CHECK(exporter::maven_repositories({"https://a.example/m2/"}) ==
          std::vector<std::string>{"https://a.example/m2"});
}

TEST_CASE("library manifests merge into the game's as Android's manifest merger does") {
    const exporter::XmlElement app = xml(kAppManifest);
    const exporter::XmlElement first = xml(R"(<manifest
        xmlns:android="http://schemas.android.com/apk/res/android"
        xmlns:tools="http://schemas.android.com/tools" package="com.vendor.ads">
    <uses-sdk android:minSdkVersion="21" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.VIBRATE" android:maxSdkVersion="25" />
    <permission android:name="${applicationId}.ADS_PERMISSION" android:protectionLevel="signature" />
    <queries><intent><action android:name="android.intent.action.VIEW" /></intent></queries>
    <application android:appComponentFactory="com.vendor.Factory" android:label="Ads">
        <activity android:name="com.vendor.ads.AdActivity" android:exported="false" />
        <provider android:name="com.vendor.ads.Init" android:authorities="${applicationId}.ads"
                  android:exported="false" tools:node="merge" />
        <meta-data android:name="com.vendor.ads.VERSION" android:value="1" />
    </application>
</manifest>)");
    const exporter::XmlElement second = xml(R"(<manifest
        xmlns:android="http://schemas.android.com/apk/res/android"
        xmlns:tools="http://schemas.android.com/tools" package="com.vendor.core">
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.READ_PHONE_STATE" tools:node="remove" />
    <queries><intent><action android:name="android.intent.action.VIEW" /></intent></queries>
    <application>
        <activity android:name="com.vendor.ads.AdActivity" android:theme="@style/Clear" />
        <meta-data android:name="com.vendor.ads.VERSION" android:value="2" tools:node="replace" />
        <service android:name="com.vendor.core.Sync" tools:ignore="ExportedService" />
    </application>
</manifest>)");

    auto merged = exporter::merge_android_manifests(
        app, {{"com.vendor:ads:1.0", first}, {"com.vendor:core:1.0", second}}, "com.example.game",
        30);
    REQUIRE_MESSAGE(merged.ok(), (merged.ok() ? std::string() : merged.error().message));
    const exporter::XmlElement& manifest = merged.value();
    // New top-level elements go before <application>, each once.
    std::vector<std::string> tags;
    for (const exporter::XmlElement& child : manifest.children) {
        tags.push_back(child.name);
    }
    CHECK(tags == std::vector<std::string>{"uses-permission", "uses-feature", "uses-permission",
                                           "permission", "queries", "application"});
    CHECK(names(manifest, "uses-permission") ==
          std::vector<std::string>{"android.permission.VIBRATE", "android.permission.INTERNET"});
    // The game's element wins where they disagree, and gains what it lacks.
    CHECK(*named(manifest, "uses-permission", "android.permission.VIBRATE")
               ->attribute("android:maxSdkVersion") == "25");
    CHECK(named(manifest, "permission", "com.example.game.ADS_PERMISSION") != nullptr);
    const exporter::XmlElement* application = manifest.child("application");
    REQUIRE(application != nullptr);
    CHECK(*application->attribute("android:label") == "Game");
    CHECK(*application->attribute("android:appComponentFactory") == "com.vendor.Factory");
    CHECK(names(*application, "activity") ==
          std::vector<std::string>{"com.example.game.GameActivity", "com.vendor.ads.AdActivity"});
    const exporter::XmlElement* activity =
        named(*application, "activity", "com.vendor.ads.AdActivity");
    CHECK(*activity->attribute("android:exported") == "false");
    CHECK(*activity->attribute("android:theme") == "@style/Clear");
    const exporter::XmlElement* provider = named(*application, "provider", "com.vendor.ads.Init");
    REQUIRE(provider != nullptr);
    CHECK(*provider->attribute("android:authorities") == "com.example.game.ads");
    CHECK(provider->attribute("tools:node") == nullptr);
    CHECK(*named(*application, "meta-data", "com.vendor.ads.VERSION")->attribute("android:value") ==
          "2");
    CHECK(named(*application, "service", "com.vendor.core.Sync")->attribute("tools:ignore") ==
          nullptr);
    CHECK(manifest.attribute("xmlns:tools") == nullptr);
    CHECK_FALSE(contains(exporter::write_xml(manifest), "tools:"));

    // A library may remove what the game declares.
    const exporter::XmlElement remover = xml(R"(<manifest
        xmlns:android="http://schemas.android.com/apk/res/android"
        xmlns:tools="http://schemas.android.com/tools">
    <uses-permission android:name="android.permission.VIBRATE" tools:node="remove" />
</manifest>)");
    auto removed = exporter::merge_android_manifests(app, {{"remover", remover}}, "g", 30);
    REQUIRE(removed.ok());
    CHECK(names(removed.value(), "uses-permission").empty());

    const exporter::XmlElement newer = xml(R"(<manifest><uses-sdk android:minSdkVersion="33" />
</manifest>)");
    CHECK(exporter::merge_android_manifests(app, {{"com.vendor:new:1.0", newer}}, "g", 30)
              .error()
              .message == "com.vendor:new:1.0 needs Android API level 33 (its minSdkVersion); "
                          "games run on API level 30 and up");
    const exporter::XmlElement placeholder = xml(R"(<manifest><application>
    <meta-data android:name="key" android:value="${adsKey}" /></application></manifest>)");
    CHECK(contains(
        exporter::merge_android_manifests(app, {{"ads", placeholder}}, "g", 30).error().message,
        "ads: android:value in <meta-data> uses ${adsKey}, a Gradle manifest "
        "placeholder"));
}

TEST_CASE("Android libraries add their code, native libraries, assets and resources") {
    const fs::path folder = fs::absolute("android_libraries");
    fs::remove_all(folder);
    const fs::path tree = folder / "tree";
    write(tree / "AndroidManifest.xml", kAppManifest);
    write(tree / "assets" / "data" / "main.scene.json", "game");
    write(tree / "assets" / "vendor.txt", "game's own");

    // A library folder laid out like an unpacked .aar, as Unity's .androidlib folders are.
    const fs::path library = folder / "src" / "ads.androidlib";
    write(library / "AndroidManifest.xml", R"(<manifest
        xmlns:android="http://schemas.android.com/apk/res/android" package="com.vendor.ads">
    <uses-permission android:name="android.permission.INTERNET" />
    <application><meta-data android:name="ads.VERSION" android:value="@string/ads_version" />
    </application>
</manifest>)");
    write(library / "classes.jar", "classes");
    write(library / "libs" / "helper.jar", "helper");
    write(library / "jni" / "arm64-v8a" / "libads.so", "arm64");
    write(library / "jni" / "x86" / "libads.so", "x86");
    write(library / "assets" / "vendor.txt", "the library's");
    write(library / "assets" / "ads.cfg", "cfg");
    write(library / "res" / "values" / "strings.xml",
          "<resources><string name=\"ads_version\">1.0</string></resources>");
    // A plain Java library.
    write(folder / "src" / "util.jar", "util");
    // A library with only a manifest.
    write(folder / "src" / "perms" / "AndroidManifest.xml",
          R"(<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" /></manifest>)");

    std::vector<exporter::AndroidLibrary> libraries{
        {"ads", library},
        {"com.example:util:1.0", folder / "src" / "util.jar"},
        {"perms", folder / "src" / "perms"}};
#ifndef _WIN32
    // An .aar file: a zip of the same layout.
    const fs::path aar_source = folder / "src" / "kit";
    write(aar_source / "AndroidManifest.xml",
          R"(<manifest package="com.vendor.kit"><application /></manifest>)");
    write(aar_source / "classes.jar", "kit classes");
    write(aar_source / "R.txt", "int string kit_name 0x7f010001\n");
    slash::platform::ProcessOptions process;
    process.working_directory = aar_source.string();
    auto zipped = slash::platform::run_process(
        {"cmake", "-E", "tar", "cf", (folder / "src" / "kit-2.0.aar").string(), "--format=zip",
         "AndroidManifest.xml", "classes.jar", "R.txt"},
        process);
    REQUIRE(zipped.ok());
    REQUIRE(zipped.value().exit_code == 0);
    libraries.push_back({"com.vendor:kit:2.0", folder / "src" / "kit-2.0.aar"});
#endif

    auto added =
        exporter::add_android_libraries(libraries, tree, folder / "work", {"arm64-v8a", "x86_64"},
                                        "com.example.game", exporter::kAndroidMinSdk);
    REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
    CHECK(read(tree / "jars" / "ads.jar") == "classes");
    CHECK(read(tree / "jars" / "ads-helper.jar") == "helper");
    CHECK(read(tree / "jars" / "com.example-util-1.0.jar") == "util");
    CHECK(read(tree / "lib" / "arm64-v8a" / "libads.so") == "arm64");
    CHECK_FALSE(fs::exists(tree / "lib" / "x86"));
    // The game's files win over a library's.
    CHECK(read(tree / "assets" / "vendor.txt") == "game's own");
    CHECK(read(tree / "assets" / "ads.cfg") == "cfg");
    REQUIRE(added.value().folders.size() == 1);
    CHECK(added.value().folders[0] == fs::absolute(library) / "res");
    const exporter::XmlElement manifest = xml(read(tree / "AndroidManifest.xml"));
    CHECK(names(manifest, "uses-permission") ==
          std::vector<std::string>{"android.permission.VIBRATE", "android.permission.INTERNET",
                                   "android.permission.ACCESS_NETWORK_STATE"});
    CHECK(named(*manifest.child("application"), "meta-data", "ads.VERSION") != nullptr);
#ifndef _WIN32
    CHECK(read(tree / "jars" / "com.vendor-kit-2.0.jar") == "kit classes");
    // Its R.txt names resources it uses, so its package gets an R class.
    CHECK(added.value().packages == std::vector<std::string>{"com.vendor.ads", "com.vendor.kit"});
#else
    CHECK(added.value().packages == std::vector<std::string>{"com.vendor.ads"});
#endif

    // Resources need a package to put their R class in.
    write(folder / "src" / "nameless" / "AndroidManifest.xml", "<manifest />");
    write(folder / "src" / "nameless" / "res" / "values" / "v.xml", "<resources />");
    CHECK(exporter::add_android_libraries({{"nameless", folder / "src" / "nameless"}}, tree,
                                          folder / "work", {"arm64-v8a"}, "com.example.game", 30)
              .error()
              .message == "nameless has resources but its AndroidManifest.xml names no package");
    CHECK(exporter::add_android_libraries({{"lib", folder / "src" / "missing"}}, tree,
                                          folder / "work", {"arm64-v8a"}, "g", 30)
              .error()
              .message == "lib: expected an .aar or .jar file or an Android library folder, not " +
                              (folder / "src" / "missing").generic_string());
    fs::remove_all(folder);
}

#ifndef _WIN32
TEST_CASE("Maven dependencies resolve as Gradle resolves them and are pinned by SHA-256") {
    const fs::path folder = fs::absolute("maven_resolve");
    fs::remove_all(folder);
    const Repository google{folder / "google"};
    const Repository central{folder / "central"};

    // A parent with properties and dependency management, and a BOM.
    central.add("com.vendor", "parent", "5",
                R"(<properties><kit.version>2.1</kit.version>
        </properties><dependencyManagement><dependencies>)" +
                    dependency("com.vendor", "util", "1.4") +
                    "</dependencies></dependencyManagement>",
                "pom");
    central.add("com.vendor", "bom", "1",
                "<dependencyManagement><dependencies>" + dependency("org.other", "log", "3.0") +
                    "</dependencies></dependencyManagement>",
                "pom");
    // The SDK: its parent gives util's version and a property, the BOM log's.
    google.add("com.vendor", "ads", "2.0",
               "<parent><groupId>com.vendor</groupId><artifactId>parent</artifactId>"
               "<version>5</version></parent><dependencyManagement><dependencies>" +
                   dependency("com.vendor", "bom", "1", "<type>pom</type><scope>import</scope>") +
                   "</dependencies></dependencyManagement><dependencies>" +
                   dependency("com.vendor", "kit", "${kit.version}", "<type>aar</type>") +
                   dependency("com.vendor", "util", "") + dependency("org.other", "log", "") +
                   dependency("junit", "junit", "4.13", "<scope>test</scope>") +
                   dependency("org.other", "optional", "1.0", "<optional>true</optional>") +
                   dependency("org.other", "heavy", "1.0",
                              "<exclusions><exclusion><groupId>org.other</groupId>"
                              "<artifactId>bloat</artifactId></exclusion></exclusions>") +
                   "</dependencies>",
               "aar");
    // kit's POM says jar, but it ships an .aar: the type the dependency names wins.
    google.add("com.vendor", "kit", "2.1",
               "<dependencies>" + dependency("com.vendor", "util", "[1.0,2.0)") +
                   "</dependencies>");
    fs::rename(google.folder("com.vendor", "kit", "2.1") / "kit-2.1.jar",
               google.folder("com.vendor", "kit", "2.1") / "kit-2.1.aar");
    central.add("com.vendor", "util", "1.4", "");
    central.add("com.vendor", "util", "1.9", "");
    central.add("com.vendor", "util", "2.0", "");
    central.list("com.vendor", "util", {"1.4", "1.9", "2.0", "1.10-SNAPSHOT"});
    central.add("org.other", "log", "3.0", "");
    central.add("org.other", "heavy", "1.0",
                "<dependencies>" + dependency("org.other", "bloat", "1.0") + "</dependencies>");
    // An Android library whose POM says nothing of its packaging.
    google.add("com.vendor", "ui", "1.0", "", "aar");
    write(google.folder("com.vendor", "ui", "1.0") / "ui-1.0.pom",
          "<project><groupId>com.vendor</groupId><artifactId>ui</artifactId>"
          "<version>1.0</version></project>");

    exporter::MavenOptions options;
    options.repositories = {google.url(), central.url() + "/"};
    options.cache = folder / "cache";
    auto lock = exporter::resolve_maven({"com.vendor:ads:2.0", "com.vendor:ui:1.0"}, options);
    REQUIRE_MESSAGE(lock.ok(), (lock.ok() ? std::string() : lock.error().message));
    // util: 1.4 from the parent, but kit's range picks the newest release under 2.0, and the
    // newest wins. No test, optional or excluded modules.
    CHECK(describe(lock.value().artifacts) ==
          std::vector<std::string>{"com.vendor:ads:2.0 aar", "com.vendor:kit:2.1 aar",
                                   "com.vendor:ui:1.0 aar", "com.vendor:util:1.9 jar",
                                   "org.other:heavy:1.0 jar", "org.other:log:3.0 jar"});
    CHECK(lock.value().dependencies ==
          std::vector<std::string>{"com.vendor:ads:2.0", "com.vendor:ui:1.0"});
    CHECK(lock.value().repositories == std::vector<std::string>{google.url(), central.url()});
    const exporter::MavenArtifact& util = lock.value().artifacts[3];
    CHECK(util.url == central.url() + "/com/vendor/util/1.9/util-1.9.jar");
    CHECK(util.sha256 == exporter::sha256_hex("util 1.9"));
    CHECK(lock.value().artifacts[0].url == google.url() + "/com/vendor/ads/2.0/ads-2.0.aar");

    // Fetching takes the cached file, or downloads it again and checks it.
    auto cached = exporter::fetch_maven_artifact(util, options.cache);
    REQUIRE(cached.ok());
    CHECK(cached.value() == exporter::maven_artifact_path(util, options.cache));
    fs::remove(cached.value());
    auto fetched = exporter::fetch_maven_artifact(util, options.cache);
    REQUIRE(fetched.ok());
    CHECK(read(fetched.value()) == "util 1.9");
    exporter::MavenArtifact tampered = util;
    tampered.sha256 = exporter::sha256_hex("something else");
    fs::remove(fetched.value());
    auto rejected = exporter::fetch_maven_artifact(tampered, options.cache);
    REQUIRE_FALSE(rejected.ok());
    CHECK(contains(rejected.error().message, "has SHA-256 " + util.sha256));
    CHECK_FALSE(fs::exists(fetched.value()));

    // What can't be found names the module.
    auto missing = exporter::resolve_maven({"com.vendor:gone:1.0"}, options);
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "com.vendor:gone:1.0: com/vendor/gone/1.0/gone-1.0.pom "
                                            "is in none of the repositories"));
    central.add("com.vendor", "broken", "1.0",
                "<dependencies>" + dependency("com.vendor", "util", "[3.0,)") + "</dependencies>");
    auto unmet = exporter::resolve_maven({"com.vendor:broken:1.0"}, options);
    REQUIRE_FALSE(unmet.ok());
    CHECK(contains(unmet.error().message, "no version of com.vendor:util in [3.0,) is in the "
                                          "repositories"));
    fs::remove_all(folder);
}

TEST_CASE("sdk lock pins a package's Maven dependencies, and exports and checks need it") {
    const fs::path folder = fs::absolute("sdk_lock_project");
    fs::remove_all(folder);
    const Repository repository{folder / "repository"};
    repository.add("com.vendor", "ads", "2.0",
                   "<dependencies>" + dependency("com.vendor", "util", "1.0") + "</dependencies>",
                   "aar");
    repository.add("com.vendor", "util", "1.0", "");
    const fs::path game = folder / "game";
    write(game / "project.json", R"({"format": 1, "name": "Game",
        "main_scene": "main.scene.json", "sdks": ["sdks/ads"]})");
    write(game / "main.scene.json", R"({"format": 1, "entities": {}})");
    const std::string sdk_json = R"({"format": 1, "name": "ads", "version": "2.0",
        "license": "MIT", "platforms": {"android": {
          "maven": ["com.vendor:ads:2.0"], "maven_repositories": [")" +
                                 repository.url() + R"("]}}})";
    write(game / "sdks" / "ads" / "sdk.json", sdk_json);
    const ScopedVariable sdks_dir(std::string(exporter::kSdksDirVariable),
                                  (folder / "cache").string());

    // Before locking, checks say how to fix it.
    json::Value checked = exporter::check_sdks(game, "android", false);
    REQUIRE(checked.find("problems")->as_array().size() == 1);
    const json::Value& problem = checked.find("problems")->as_array()[0];
    CHECK(problem.find("path")->as_string() == "/platforms/android/maven");
    CHECK(problem.find("message")->as_string() ==
          "sdks/ads has no sdk.lock.json pinning its Maven dependencies; run slash-cli sdk lock");
    // Other platforms don't need it.
    CHECK(exporter::check_sdks(game, "linux", false).find("problems")->as_array().empty());

    exporter::LockSdksOptions options;
    options.project = game;
    options.dry_run = true;
    auto dry = exporter::lock_sdks(options);
    REQUIRE_MESSAGE(dry.ok(), (dry.ok() ? std::string() : dry.error().message));
    CHECK(dry.value().find("dry_run")->as_bool());
    CHECK_FALSE(fs::exists(game / "sdks" / "ads" / "sdk.lock.json"));

    options.dry_run = false;
    options.name = "adz";
    CHECK(contains(exporter::lock_sdks(options).error().message, "did you mean \"ads\"?"));
    options.name = "ads";
    auto locked = exporter::lock_sdks(options);
    REQUIRE(locked.ok());
    const json::Value& item = locked.value().find("sdks")->as_array()[0];
    CHECK(item.find("lock")->as_string() == "sdks/ads/sdk.lock.json");
    CHECK(item.find("artifacts")->as_int() == 2);
    CHECK(item.find("changed")->as_bool());
    const json::Value file = parse(read(game / "sdks" / "ads" / "sdk.lock.json"));
    CHECK(file.find("format")->as_int() == 1);
    CHECK(*file.find("maven")->find("dependencies") ==
          json::Value(json::Array{"com.vendor:ads:2.0"}));
    // Locking again changes nothing.
    auto again = exporter::lock_sdks(options);
    REQUIRE(again.ok());
    CHECK_FALSE(again.value().find("sdks")->as_array()[0].find("changed")->as_bool());

    checked = exporter::check_sdks(game, "android", false);
    CHECK(checked.find("problems")->as_array().empty());
    CHECK(checked.find("sdks")->as_array()[0].find("maven")->as_int() == 2);

    // Changing the dependencies makes the lock stale.
    write(game / "sdks" / "ads" / "sdk.json",
          std::string(sdk_json).replace(sdk_json.find("ads:2.0"), 7, "ads:2.1"));
    checked = exporter::check_sdks(game, "android", false);
    REQUIRE(checked.find("problems")->as_array().size() == 1);
    CHECK(contains(checked.find("problems")->as_array()[0].find("message")->as_string(),
                   "was made for other Maven dependencies or repositories; run slash-cli sdk "
                   "lock"));
    write(game / "sdks" / "ads" / "sdk.json", sdk_json);

    // The command does the same, and needs the build capability: it downloads.
    command::SessionOptions session_options;
    session_options.project_root = game.string();
    session_options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    {
        Engine engine(session_options);
        const command::Reply denied = engine.session->call("sdk.lock", parse("{}"));
        REQUIRE_FALSE(denied.ok);
        CHECK(denied.code == command::code::kPermissionDenied);
    }
    session_options.capabilities.push_back(command::Capability::Build);
    Engine engine(session_options);
    command::Reply reply = engine.session->call("sdk.lock", parse(R"({"dry_run": true})"));
    REQUIRE(reply.ok);
    CHECK(reply.value.find("dry_run")->as_bool());
    CHECK_FALSE(reply.value.find("sdks")->as_array()[0].find("changed")->as_bool());
    reply = engine.session->call("sdk.lock", parse(R"({"sdk": "nope"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.data.find("path")->as_string() == "/sdk");

    // sdk add says when a package still needs locking.
    fs::remove(game / "sdks" / "ads" / "sdk.lock.json");
    write(game / "project.json",
          R"({"format": 1, "name": "Game", "main_scene": "main.scene.json"})");
    exporter::AddSdkOptions add;
    add.project = game;
    add.source = (game / "sdks" / "ads").string();
    auto added = exporter::add_sdk(add);
    REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
    CHECK(added.value().find("needs_lock")->as_bool());
    fs::remove_all(folder);
}
#endif

TEST_CASE("sdk.json checks Android libraries, Maven coordinates and repositories") {
    const auto errors = [](std::string_view android) {
        std::vector<std::string> out;
        for (const auto& error : exporter::validate_sdk(parse(
                 R"({"format": 1, "name": "kit", "version": "1", "license": "MIT",
                     "platforms": {"android": )" +
                 std::string(android) + "}}"))) {
            out.push_back(error.path + ": " + error.message);
        }
        return out;
    };
    CHECK(errors(R"({"aars": ["libs/kit.aar", "kit.androidlib"],
                     "maven": ["androidx.core:core:1.13.1"],
                     "maven_repositories": ["https://maven.example/m2", "file:///srv/m2"]})")
              .empty());
    const std::vector<std::string> found =
        errors(R"({"aars": ["libs/{arch}/kit.aar"], "maven": ["androidx.core:core"],
                   "maven_repositories": ["http://maven.example/m2"]})");
    REQUIRE(found.size() == 3);
    CHECK(found[0] == "/platforms/android/aars/0: Android libraries serve every ABI; their paths "
                      "can't use {arch} or {build}");
    CHECK(contains(found[1], "/platforms/android/maven/0: \"androidx.core:core\" is not a Maven "
                             "coordinate"));
    CHECK(found[2] ==
          "/platforms/android/maven_repositories/0: expected an https:// or file:// address");
    // Only Android has them.
    bool linux_maven = false;
    for (const auto& error : exporter::validate_sdk(parse(
             R"({"format": 1, "name": "kit", "version": "1", "license": "MIT",
                 "platforms": {"linux": {"maven": ["a:b:1"]}}})"))) {
        linux_maven = linux_maven || error.path == "/platforms/linux/maven";
    }
    CHECK(linux_maven);
}
