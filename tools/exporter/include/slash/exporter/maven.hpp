#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Maven dependencies for Android SDK packages, resolved without Gradle. A package's sdk.json
// lists coordinates ("com.google.android.gms:play-services-ads:23.6.0"); resolve_maven() walks
// their POMs as Gradle does and pins every file of the graph by SHA-256 in the package's
// sdk.lock.json, which exports then fetch and check.
namespace slash::exporter {

// Google's Maven repository, then Maven Central: where Android libraries are published.
inline constexpr std::array<std::string_view, 2> kDefaultMavenRepositories{
    "https://dl.google.com/dl/android/maven2", "https://repo.maven.apache.org/maven2"};

struct MavenCoordinate {
    std::string group;
    std::string artifact;
    std::string version;    // a version, or a range such as "[1.0,2.0)"
    std::string classifier; // "" for none
    std::string type;       // "aar", "jar", or "" for what the POM's packaging says

    // "group:artifact".
    [[nodiscard]] std::string module() const;
    // "group:artifact:version", with ":classifier" when there is one.
    [[nodiscard]] std::string id() const;
};

// "group:artifact:version", with an optional ":classifier" and "@type", as Gradle writes them.
[[nodiscard]] Result<MavenCoordinate> parse_maven_coordinate(std::string_view text);

// Orders versions as Gradle does: negative if a is older than b, 0 if they are the same,
// positive if a is newer. "1.0-rc1" < "1.0" < "1.0.1" < "1.1"; "1.10" > "1.9".
[[nodiscard]] int compare_maven_versions(std::string_view a, std::string_view b);

// Whether version is a Maven range: "[1.0]", "[1.0,2.0)", "(,1.0],[1.2,)".
[[nodiscard]] bool is_maven_range(std::string_view version);

// Whether version is inside range.
[[nodiscard]] bool in_maven_range(std::string_view version, std::string_view range);

// One file of a resolved graph.
struct MavenArtifact {
    std::string id;   // "group:artifact:version", with ":classifier" when there is one
    std::string type; // "aar" or "jar"
    std::string url;
    std::string sha256;
};

// A package's resolved Maven dependencies, as sdk.lock.json records them.
struct MavenLock {
    std::vector<std::string> dependencies; // as sdk.json lists them
    std::vector<std::string> repositories; // the repositories they were resolved from
    std::vector<MavenArtifact> artifacts;  // every file the graph needs, sorted by id
};

struct MavenOptions {
    std::vector<std::string> repositories; // https:// or file:// addresses, searched in order
    std::filesystem::path cache;           // where POMs and files are kept
    std::string log_path;                  // curl's output is appended here
};

// The repositories a package resolves from: its own, or else kDefaultMavenRepositories.
[[nodiscard]] std::vector<std::string> maven_repositories(const std::vector<std::string>& listed);

// Resolves the dependencies and everything they need, as Gradle resolves an app's runtime
// classpath: compile and runtime scopes, POM parents, properties, dependency management and BOM
// imports, exclusions and version ranges, with the newest version of each module winning.
// Downloads each file to pin its SHA-256. The error names the module that failed.
[[nodiscard]] Result<MavenLock> resolve_maven(const std::vector<std::string>& dependencies,
                                              const MavenOptions& options);

// Joins several packages' resolved files into the one graph an app links: where two pin
// different versions of a module, the newest wins, as Gradle resolves an app's dependencies.
// Keeps the order files were first seen in.
[[nodiscard]] std::vector<MavenArtifact>
merge_maven_artifacts(const std::vector<std::vector<MavenArtifact>>& graphs);

// Where artifact is kept in cache, by its Maven path.
[[nodiscard]] std::filesystem::path maven_artifact_path(const MavenArtifact& artifact,
                                                        const std::filesystem::path& cache);

// Downloads artifact into cache unless it is there already, and checks its SHA-256.
[[nodiscard]] Result<std::filesystem::path> fetch_maven_artifact(const MavenArtifact& artifact,
                                                                 const std::filesystem::path& cache,
                                                                 const std::string& log_path = {});

[[nodiscard]] json::Value maven_lock_json(const MavenLock& lock);
// Reads the "maven" object of sdk.lock.json.
[[nodiscard]] Result<MavenLock> parse_maven_lock(const json::Value& value);

} // namespace slash::exporter
