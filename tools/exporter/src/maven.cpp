#include "slash/exporter/maven.hpp"

#include "steps.hpp"

#include "slash/exporter/toolchains.hpp"
#include "slash/exporter/xml.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::display;

// POM parents and BOM imports nest at most this deep.
constexpr int kMaxDepth = 16;
// Rounds of picking the newest versions before giving up on them settling.
constexpr int kMaxRounds = 64;

bool digit(char c) {
    return c >= '0' && c <= '9';
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

// Parts become folder and file names, so none may start with '.' (no "..").
bool valid_coordinate_part(std::string_view part) {
    return !part.empty() && !part.starts_with('.') && std::ranges::all_of(part, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '.' ||
               c == '-' || c == '_';
    });
}

// A version that names one release, not a range: a coordinate part that may also hold '+'.
bool valid_version(std::string_view version) {
    return !version.empty() && !version.starts_with('.') &&
           std::ranges::all_of(version, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '.' ||
                      c == '-' || c == '_' || c == '+';
           });
}

std::string trimmed(std::string_view text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(start, end - start + 1));
}

// Gradle splits versions at '.', '-', '_' and '+', and between digits and other characters.
std::vector<std::string_view> version_parts(std::string_view version) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i < version.size(); ++i) {
        const char c = version[i];
        if (c == '.' || c == '-' || c == '_' || c == '+') {
            if (i > start) {
                parts.push_back(version.substr(start, i - start));
            }
            start = i + 1;
        } else if (i > start && digit(c) != digit(version[i - 1])) {
            parts.push_back(version.substr(start, i - start));
            start = i;
        }
    }
    if (start < version.size()) {
        parts.push_back(version.substr(start));
    }
    return parts;
}

bool numeric(std::string_view part) {
    return !part.empty() && std::ranges::all_of(part, digit);
}

// Where a word ranks among words: "dev" below all others, then ordinary words, then Gradle's
// release qualifiers in their order.
int word_rank(std::string_view part) {
    const std::string word = lowered(part);
    if (word == "dev") {
        return 0;
    }
    constexpr std::array<std::string_view, 6> kQualifiers{"rc", "snapshot", "final",
                                                          "ga", "release",  "sp"};
    // An index, not an iterator: std::array's iterators are pointers only in some libraries.
    for (std::size_t i = 0; i < kQualifiers.size(); ++i) {
        if (kQualifiers[i] == word) {
            return 2 + static_cast<int>(i);
        }
    }
    return 1;
}

int sign(int value) {
    if (value == 0) {
        return 0;
    }
    return value < 0 ? -1 : 1;
}

int compare_parts(std::string_view a, std::string_view b) {
    const bool a_number = numeric(a);
    const bool b_number = numeric(b);
    if (a_number && b_number) {
        a.remove_prefix(std::min(a.find_first_not_of('0'), a.size()));
        b.remove_prefix(std::min(b.find_first_not_of('0'), b.size()));
        if (a.size() != b.size()) {
            return a.size() < b.size() ? -1 : 1;
        }
        return sign(a.compare(b));
    }
    if (a_number != b_number) {
        return a_number ? 1 : -1;
    }
    const int a_rank = word_rank(a);
    const int b_rank = word_rank(b);
    if (a_rank != b_rank) {
        return a_rank < b_rank ? -1 : 1;
    }
    return sign(lowered(a).compare(lowered(b)));
}

struct Bound {
    std::string version; // "" for none
    bool inclusive = false;
};

struct Interval {
    Bound lower;
    Bound upper;
};

std::optional<std::vector<Interval>> parse_range(std::string_view text) {
    std::vector<Interval> out;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ',' || text[at] == ' ')) {
            ++at;
        }
        if (at == text.size()) {
            break;
        }
        const char open = text[at];
        if (open != '[' && open != '(') {
            return std::nullopt;
        }
        const std::size_t close_at = text.find_first_of("])", at);
        if (close_at == std::string_view::npos) {
            return std::nullopt;
        }
        const char close = text[close_at];
        const std::string_view inner = text.substr(at + 1, close_at - at - 1);
        Interval interval;
        if (const std::size_t comma = inner.find(','); comma == std::string_view::npos) {
            const std::string exact = trimmed(inner);
            if (open != '[' || close != ']' || exact.empty()) {
                return std::nullopt;
            }
            interval.lower = {exact, true};
            interval.upper = {exact, true};
        } else {
            interval.lower = {trimmed(inner.substr(0, comma)), open == '['};
            interval.upper = {trimmed(inner.substr(comma + 1)), close == ']'};
            if (inner.find(',', comma + 1) != std::string_view::npos) {
                return std::nullopt;
            }
        }
        out.push_back(std::move(interval));
        at = close_at + 1;
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

bool in_interval(std::string_view version, const Interval& interval) {
    if (!interval.lower.version.empty()) {
        const int order = compare_maven_versions(version, interval.lower.version);
        if (order < 0 || (order == 0 && !interval.lower.inclusive)) {
            return false;
        }
    }
    if (!interval.upper.version.empty()) {
        const int order = compare_maven_versions(version, interval.upper.version);
        if (order > 0 || (order == 0 && !interval.upper.inclusive)) {
            return false;
        }
    }
    return true;
}

std::string concat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (const std::string_view part : parts) {
        out += part;
    }
    return out;
}

// "com.example:lib:1.0" lives at com/example/lib/1.0/.
std::string maven_folder(std::string_view group, std::string_view artifact,
                         std::string_view version) {
    std::string out(group);
    std::ranges::replace(out, '.', '/');
    out += '/';
    out += artifact;
    out += '/';
    out += version;
    return out;
}

std::string file_name(std::string_view artifact, std::string_view version,
                      std::string_view classifier, std::string_view extension) {
    std::string out(artifact);
    out += '-';
    out += version;
    if (!classifier.empty()) {
        out += '-';
        out += classifier;
    }
    out += '.';
    out += extension;
    return out;
}

std::string tail_of(const Result<platform::ProcessResult>& result) {
    if (!result) {
        return result.error().message;
    }
    const std::string tail = trimmed(result.value().tail);
    return tail.empty() ? "curl exited with " + std::to_string(result.value().exit_code) : tail;
}

// Downloads url to file through a temporary file beside it, so a failed download leaves nothing.
Result<void> download(const std::string& url, const fs::path& file, const std::string& log_path) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path part = file.string() + ".part-" + std::to_string(stamp);
    platform::ProcessOptions process;
    process.log_path = log_path;
    process.tail_lines = 3;
    const auto fetched =
        platform::run_process({"curl", "--fail", "--location", "--silent", "--show-error",
                               "--retry", "2", "--output", part.string(), url},
                              process);
    if (!fetched || fetched.value().exit_code != 0) {
        fs::remove(part, error);
        return make_error(ErrorCode::NotFound, tail_of(fetched));
    }
    fs::rename(part, file, error);
    if (error) {
        fs::remove(part, error);
        return make_error(ErrorCode::PlatformFailure, "could not write " + display(file));
    }
    return {};
}

struct PomDependency {
    std::string group;
    std::string artifact;
    std::string version;
    std::string scope;
    std::string type = "jar";
    std::string classifier;
    bool optional = false;
    std::vector<std::string> exclusions; // "group:artifact"; either may be "*"

    [[nodiscard]] std::string module() const { return group + ":" + artifact; }
    // Dependency management matches on these.
    [[nodiscard]] std::string key() const {
        return group + ":" + artifact + ":" + type + ":" + classifier;
    }
};

struct Pom {
    std::string group;
    std::string artifact;
    std::string version;
    std::string packaging = "jar";
    std::map<std::string, std::string> properties;
    std::vector<PomDependency> managed;
    std::vector<PomDependency> dependencies;
};

// text with each ${name} that properties define replaced, until none are left.
std::string interpolate(std::string text, const std::map<std::string, std::string>& properties) {
    for (int round = 0; round < 10 && text.find("${") != std::string::npos; ++round) {
        std::string out;
        std::size_t at = 0;
        bool changed = false;
        std::size_t start = 0;
        while ((start = text.find("${", at)) != std::string::npos) {
            const std::size_t end = text.find('}', start);
            if (end == std::string::npos) {
                break;
            }
            out.append(text, at, start - at);
            const auto found = properties.find(text.substr(start + 2, end - start - 2));
            if (found != properties.end()) {
                out += found->second;
                changed = true;
            } else {
                out.append(text, start, end - start + 1);
            }
            at = end + 1;
        }
        out.append(text, at);
        text = std::move(out);
        if (!changed) {
            break;
        }
    }
    return text;
}

PomDependency read_dependency(const XmlElement& element,
                              const std::map<std::string, std::string>& properties) {
    PomDependency out;
    out.group = interpolate(element.child_text("groupId"), properties);
    out.artifact = interpolate(element.child_text("artifactId"), properties);
    out.version = interpolate(element.child_text("version"), properties);
    out.scope = interpolate(element.child_text("scope"), properties);
    if (const std::string type = interpolate(element.child_text("type"), properties);
        !type.empty()) {
        out.type = type;
    }
    out.classifier = interpolate(element.child_text("classifier"), properties);
    out.optional = interpolate(element.child_text("optional"), properties) == "true";
    if (const XmlElement* exclusions = element.child("exclusions")) {
        for (const XmlElement& exclusion : exclusions->children) {
            if (exclusion.name != "exclusion") {
                continue;
            }
            const std::string group = interpolate(exclusion.child_text("groupId"), properties);
            const std::string artifact =
                interpolate(exclusion.child_text("artifactId"), properties);
            out.exclusions.push_back((group.empty() ? "*" : group) + ":" +
                                     (artifact.empty() ? "*" : artifact));
        }
    }
    return out;
}

void put_dependency(std::vector<PomDependency>& list, PomDependency dependency) {
    const auto found = std::ranges::find(list, dependency.key(), &PomDependency::key);
    if (found != list.end()) {
        *found = std::move(dependency);
    } else {
        list.push_back(std::move(dependency));
    }
}

bool excluded(const std::set<std::string>& exclusions, const PomDependency& dependency) {
    return std::ranges::any_of(exclusions, [&](const std::string& exclusion) {
        const std::size_t colon = exclusion.find(':');
        const std::string_view group = std::string_view(exclusion).substr(0, colon);
        const std::string_view artifact = std::string_view(exclusion).substr(colon + 1);
        return (group == "*" || group == dependency.group) &&
               (artifact == "*" || artifact == dependency.artifact);
    });
}

class Resolver {
public:
    explicit Resolver(const MavenOptions& options)
        : repositories_(maven_repositories(options.repositories)), cache_(options.cache),
          log_path_(options.log_path) {}

    Result<MavenLock> resolve(const std::vector<std::string>& dependencies);

private:
    // The local copy of a repository file: fetched from the first repository that has it, unless
    // an earlier run did, with the address it came from in "<file>.url".
    Result<fs::path> fetch(const std::string& relative) {
        std::error_code error;
        const fs::path file = cache_ / relative;
        if (fs::is_regular_file(file, error) &&
            fs::is_regular_file(fs::path(file.string() + ".url"), error)) {
            return file;
        }
        std::string last;
        for (const std::string& repository : repositories_) {
            std::string url = repository;
            url += '/';
            url += relative;
            if (auto fetched = download(url, file, log_path_); !fetched) {
                last = fetched.error().message;
                continue;
            }
            if (auto written = platform::write_file(file.string() + ".url", url); !written) {
                return written.error();
            }
            return file;
        }
        return make_error(ErrorCode::NotFound,
                          relative + " is in none of the repositories (" + last + ")");
    }

    Result<const Pom*> pom(const std::string& group, const std::string& artifact,
                           const std::string& version, int depth);

    // The newest version of a module in range that any repository lists.
    Result<std::string> concrete_version(const std::string& group, const std::string& artifact,
                                         const std::string& version);

    std::vector<std::string> repositories_;
    fs::path cache_;
    std::string log_path_;
    std::map<std::string, Pom> poms_;
    std::map<std::string, std::vector<std::string>> listed_; // by module, from maven-metadata.xml
};

Result<const Pom*> Resolver::pom(const std::string& group, const std::string& artifact,
                                 const std::string& version, int depth) {
    const std::string id = group + ":" + artifact + ":" + version;
    if (const auto found = poms_.find(id); found != poms_.end()) {
        return &found->second;
    }
    if (depth > kMaxDepth) {
        return make_error(ErrorCode::InvalidArgument,
                          id + ": POM parents and imports nest more than " +
                              std::to_string(kMaxDepth) + " deep");
    }
    if (version.empty() || version.find("${") != std::string::npos) {
        return make_error(ErrorCode::InvalidArgument, group + ":" + artifact +
                                                          " has no version its POMs define (\"" +
                                                          version + "\")");
    }
    if (!valid_coordinate_part(group) || !valid_coordinate_part(artifact) ||
        !valid_version(version)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + id + "\" is not a Maven coordinate a repository could hold");
    }
    auto file = fetch(maven_folder(group, artifact, version) + "/" +
                      file_name(artifact, version, "", "pom"));
    if (!file) {
        return make_error(file.error().code, id + ": " + file.error().message);
    }
    auto text = platform::read_file(file.value().string());
    if (!text) {
        return text.error();
    }
    auto parsed = parse_xml(text.value());
    if (!parsed || parsed.value().name != "project") {
        return make_error(ErrorCode::InvalidArgument,
                          id + ": the POM is not a Maven project" +
                              (parsed ? std::string() : ": " + parsed.error().message));
    }
    const XmlElement& project = parsed.value();
    Pom out;
    const Pom* parent = nullptr;
    if (const XmlElement* element = project.child("parent")) {
        auto loaded = pom(element->child_text("groupId"), element->child_text("artifactId"),
                          element->child_text("version"), depth + 1);
        if (!loaded) {
            return make_error(loaded.error().code, id + ": its parent " + loaded.error().message);
        }
        parent = loaded.value();
        out.properties = parent->properties;
        out.managed = parent->managed;
        out.dependencies = parent->dependencies;
    }
    if (const XmlElement* properties = project.child("properties")) {
        for (const XmlElement& property : properties->children) {
            out.properties[property.name] = property.text;
        }
    }
    out.group = project.child_text("groupId");
    out.version = project.child_text("version");
    if (parent != nullptr) {
        out.group = out.group.empty() ? parent->group : out.group;
        out.version = out.version.empty() ? parent->version : out.version;
        out.properties["project.parent.groupId"] = parent->group;
        out.properties["project.parent.version"] = parent->version;
    }
    out.artifact = project.child_text("artifactId");
    for (const char* prefix : {"project.", "pom.", ""}) {
        out.properties[std::string(prefix) + "groupId"] = out.group;
        out.properties[std::string(prefix) + "artifactId"] = out.artifact;
        out.properties[std::string(prefix) + "version"] = out.version;
    }
    out.group = interpolate(out.group, out.properties);
    out.version = interpolate(out.version, out.properties);
    for (const char* prefix : {"project.", "pom.", ""}) {
        out.properties[std::string(prefix) + "groupId"] = out.group;
        out.properties[std::string(prefix) + "version"] = out.version;
    }
    if (const std::string packaging = interpolate(project.child_text("packaging"), out.properties);
        !packaging.empty()) {
        out.packaging = packaging;
    }
    if (const XmlElement* management = project.child("dependencyManagement")) {
        if (const XmlElement* list = management->child("dependencies")) {
            for (const XmlElement& element : list->children) {
                if (element.name != "dependency") {
                    continue;
                }
                PomDependency dependency = read_dependency(element, out.properties);
                if (dependency.scope != "import") {
                    put_dependency(out.managed, std::move(dependency));
                    continue;
                }
                // A BOM: its managed versions, under those this POM sets itself.
                auto bom =
                    pom(dependency.group, dependency.artifact, dependency.version, depth + 1);
                if (!bom) {
                    return make_error(bom.error().code,
                                      id + ": the BOM it imports, " + bom.error().message);
                }
                for (const PomDependency& managed : bom.value()->managed) {
                    if (std::ranges::find(out.managed, managed.key(), &PomDependency::key) ==
                        out.managed.end()) {
                        out.managed.push_back(managed);
                    }
                }
            }
        }
    }
    if (const XmlElement* list = project.child("dependencies")) {
        for (const XmlElement& element : list->children) {
            if (element.name == "dependency") {
                put_dependency(out.dependencies, read_dependency(element, out.properties));
            }
        }
    }
    for (PomDependency& dependency : out.dependencies) {
        if (!dependency.version.empty() && !dependency.scope.empty()) {
            continue;
        }
        const auto managed = std::ranges::find(out.managed, dependency.key(), &PomDependency::key);
        if (managed != out.managed.end()) {
            dependency.version = dependency.version.empty() ? managed->version : dependency.version;
            dependency.scope = dependency.scope.empty() ? managed->scope : dependency.scope;
        }
    }
    return &poms_.emplace(id, std::move(out)).first->second;
}

Result<std::string> Resolver::concrete_version(const std::string& group,
                                               const std::string& artifact,
                                               const std::string& version) {
    if (!version.starts_with('[') && !version.starts_with('(')) {
        return version;
    }
    const std::optional<std::vector<Interval>> intervals = parse_range(version);
    if (!intervals) {
        return make_error(ErrorCode::InvalidArgument, group + ":" + artifact + " asks for \"" +
                                                          version + "\", which is not a range");
    }
    if (intervals->size() == 1 && intervals->front().lower.inclusive &&
        intervals->front().lower.version == intervals->front().upper.version) {
        return intervals->front().lower.version;
    }
    const std::string module = group + ":" + artifact;
    auto listed = listed_.find(module);
    if (listed == listed_.end()) {
        std::vector<std::string> versions;
        const std::string relative = maven_folder(group, artifact, "") + "maven-metadata.xml";
        for (std::size_t i = 0; i < repositories_.size(); ++i) {
            const fs::path file = cache_ / ".metadata" / std::to_string(i) / relative;
            if (!download(repositories_[i] + "/" + relative, file, log_path_)) {
                continue;
            }
            auto text = platform::read_file(file.string());
            auto parsed = text ? parse_xml(text.value()) : Result<XmlElement>(text.error());
            if (!parsed) {
                continue;
            }
            if (const XmlElement* versioning = parsed.value().child("versioning")) {
                if (const XmlElement* list = versioning->child("versions")) {
                    for (const XmlElement& element : list->children) {
                        if (element.name == "version" &&
                            std::ranges::find(versions, element.text) == versions.end()) {
                            versions.push_back(element.text);
                        }
                    }
                }
            }
        }
        listed = listed_.emplace(module, std::move(versions)).first;
    }
    std::string best;
    for (const std::string& candidate : listed->second) {
        if (lowered(candidate).find("snapshot") != std::string::npos || !valid_version(candidate)) {
            continue;
        }
        const bool inside = std::ranges::any_of(
            *intervals, [&](const Interval& interval) { return in_interval(candidate, interval); });
        if (inside && (best.empty() || compare_maven_versions(candidate, best) > 0)) {
            best = candidate;
        }
    }
    if (best.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no version of " + module + " in " + version + " is in the repositories");
    }
    return best;
}

Result<MavenLock> Resolver::resolve(const std::vector<std::string>& dependencies) {
    std::vector<MavenCoordinate> roots;
    for (const std::string& text : dependencies) {
        auto coordinate = parse_maven_coordinate(text);
        if (!coordinate) {
            return coordinate.error();
        }
        roots.push_back(std::move(coordinate).value());
    }

    struct Node {
        std::string group;
        std::string artifact;
        std::set<std::string> exclusions;
    };
    std::map<std::string, std::string> selected; // version by module
    std::vector<std::string> order;              // modules as the last round reached them
    std::map<std::string, std::string> types;    // "aar" or "pom" where a request says so
    std::map<std::string, std::string> classifiers;
    bool settled = false;
    for (int round = 0; round < kMaxRounds && !settled; ++round) {
        std::map<std::string, std::vector<std::string>> requested;
        std::deque<Node> queue;
        std::set<std::string> visited;
        order.clear();
        types.clear();
        classifiers.clear();
        for (const MavenCoordinate& root : roots) {
            auto version = concrete_version(root.group, root.artifact, root.version);
            if (!version) {
                return version.error();
            }
            requested[root.module()].push_back(std::move(version).value());
            if (!root.type.empty()) {
                types.emplace(root.module(), root.type);
            }
            classifiers.emplace(root.module(), root.classifier);
            queue.push_back({root.group, root.artifact, {}});
        }
        while (!queue.empty()) {
            const Node node = std::move(queue.front());
            queue.pop_front();
            const std::string module = node.group + ":" + node.artifact;
            std::string key = module;
            for (const std::string& exclusion : node.exclusions) {
                key += "|" + exclusion;
            }
            if (!visited.insert(key).second) {
                continue;
            }
            if (std::ranges::find(order, module) == order.end()) {
                order.push_back(module);
            }
            std::string version;
            if (const auto found = selected.find(module); found != selected.end()) {
                version = found->second;
            } else {
                const std::vector<std::string>& versions = requested[module];
                version = *std::ranges::max_element(versions, [](const auto& a, const auto& b) {
                    return compare_maven_versions(a, b) < 0;
                });
            }
            auto loaded = pom(node.group, node.artifact, version, 0);
            if (!loaded) {
                return loaded.error();
            }
            for (const PomDependency& dependency : loaded.value()->dependencies) {
                const std::string scope = dependency.scope.empty() ? "compile" : dependency.scope;
                if (dependency.optional || (scope != "compile" && scope != "runtime") ||
                    excluded(node.exclusions, dependency)) {
                    continue;
                }
                auto wanted =
                    concrete_version(dependency.group, dependency.artifact, dependency.version);
                if (!wanted) {
                    return make_error(wanted.error().code,
                                      concat({module, ":", version, ": ", wanted.error().message}));
                }
                if (wanted.value().empty() || wanted.value().find("${") != std::string::npos) {
                    return make_error(ErrorCode::InvalidArgument,
                                      concat({module, ":", version, " gives ", dependency.module(),
                                              " no version"}));
                }
                requested[dependency.module()].push_back(std::move(wanted).value());
                if (dependency.type == "aar" || dependency.type == "pom") {
                    types.emplace(dependency.module(), dependency.type);
                }
                classifiers.emplace(dependency.module(), dependency.classifier);
                Node child{dependency.group, dependency.artifact, node.exclusions};
                child.exclusions.insert(dependency.exclusions.begin(), dependency.exclusions.end());
                queue.push_back(std::move(child));
            }
        }
        std::map<std::string, std::string> newest;
        for (const auto& [module, versions] : requested) {
            newest[module] = *std::ranges::max_element(versions, [](const auto& a, const auto& b) {
                return compare_maven_versions(a, b) < 0;
            });
        }
        settled = newest == selected;
        selected = std::move(newest);
    }
    if (!settled) {
        return make_error(ErrorCode::InvalidArgument,
                          "the dependencies' versions did not settle after " +
                              std::to_string(kMaxRounds) + " rounds");
    }

    MavenLock lock;
    lock.dependencies = dependencies;
    lock.repositories = repositories_;
    for (const std::string& module : order) {
        const std::string& version = selected[module];
        const std::size_t colon = module.find(':');
        const std::string group = module.substr(0, colon);
        const std::string artifact = module.substr(colon + 1);
        auto loaded = pom(group, artifact, version, 0);
        if (!loaded) {
            return loaded.error();
        }
        std::string type;
        if (const auto found = types.find(module); found != types.end()) {
            type = found->second;
        } else if (loaded.value()->packaging == "aar" || loaded.value()->packaging == "pom") {
            type = loaded.value()->packaging;
        } else {
            type = "jar";
        }
        if (type == "pom") {
            continue; // a BOM or an aggregate: its dependencies, but no file of its own
        }
        const std::string classifier = classifiers[module];
        const std::string folder = maven_folder(group, artifact, version) + "/";
        auto file = fetch(folder + file_name(artifact, version, classifier, type));
        if (!file && !types.contains(module)) {
            // Some POMs say "jar" or nothing for an Android library.
            const std::string other = type == "jar" ? "aar" : "jar";
            if (auto again = fetch(folder + file_name(artifact, version, classifier, other))) {
                type = other;
                file = std::move(again);
            }
        }
        if (!file) {
            return make_error(file.error().code,
                              concat({module, ":", version, ": ", file.error().message}));
        }
        auto sha256 = sha256_file(file.value());
        if (!sha256) {
            return sha256.error();
        }
        auto url = platform::read_file(file.value().string() + ".url");
        if (!url) {
            return url.error();
        }
        MavenArtifact item;
        item.id = concat({module, ":", version});
        if (!classifier.empty()) {
            item.id += ':';
            item.id += classifier;
        }
        item.type = type;
        item.url = trimmed(url.value());
        item.sha256 = std::move(sha256).value();
        lock.artifacts.push_back(std::move(item));
    }
    std::ranges::sort(lock.artifacts, {}, &MavenArtifact::id);
    return lock;
}

std::vector<std::string> string_list(const json::Value* value) {
    std::vector<std::string> out;
    if (value == nullptr || !value->is_array()) {
        return out;
    }
    for (const json::Value& item : value->as_array()) {
        if (item.is_string()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

} // namespace

std::string MavenCoordinate::module() const {
    return group + ":" + artifact;
}

std::string MavenCoordinate::id() const {
    return module() + ":" + version + (classifier.empty() ? "" : ":" + classifier);
}

Result<MavenCoordinate> parse_maven_coordinate(std::string_view text) {
    const auto invalid = [&](std::string_view why) {
        return make_error(
            ErrorCode::InvalidArgument,
            "\"" + std::string(text) + "\" is not a Maven coordinate: " + std::string(why) +
                " (expected group:artifact:version, e.g. " + "androidx.core:core:1.13.1)");
    };
    MavenCoordinate out;
    std::string_view rest = text;
    if (const std::size_t at = rest.find('@'); at != std::string_view::npos) {
        out.type = std::string(rest.substr(at + 1));
        rest = rest.substr(0, at);
        if (out.type != "aar" && out.type != "jar") {
            return invalid("the type after '@' must be aar or jar");
        }
    }
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t colon = rest.find(':');
        parts.push_back(rest.substr(0, colon));
        if (colon == std::string_view::npos) {
            break;
        }
        rest = rest.substr(colon + 1);
    }
    if (parts.size() < 3 || parts.size() > 4) {
        return invalid("it needs a group, an artifact and a version");
    }
    if (!valid_coordinate_part(parts[0]) || !valid_coordinate_part(parts[1])) {
        return invalid("groups and artifacts are letters, digits, '.', '-' and '_'");
    }
    out.group = std::string(parts[0]);
    out.artifact = std::string(parts[1]);
    out.version = std::string(parts[2]);
    if (out.version.starts_with('[') || out.version.starts_with('(')) {
        if (!is_maven_range(out.version)) {
            return invalid("the version range is malformed");
        }
    } else if (!valid_version(out.version)) {
        return invalid("versions are letters, digits, '.', '-', '_' and '+'");
    }
    if (parts.size() == 4) {
        if (!valid_coordinate_part(parts[3])) {
            return invalid("classifiers are letters, digits, '.', '-' and '_'");
        }
        out.classifier = std::string(parts[3]);
    }
    return out;
}

int compare_maven_versions(std::string_view a, std::string_view b) {
    const std::vector<std::string_view> a_parts = version_parts(a);
    const std::vector<std::string_view> b_parts = version_parts(b);
    const std::size_t common = std::min(a_parts.size(), b_parts.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (const int order = compare_parts(a_parts[i], b_parts[i]); order != 0) {
            return order;
        }
    }
    if (a_parts.size() == b_parts.size()) {
        return 0;
    }
    // An extra number makes a version newer ("1.0.1" > "1.0"), an extra word older
    // ("1.0-beta" < "1.0").
    const bool a_longer = a_parts.size() > b_parts.size();
    const bool extra_number = numeric(a_longer ? a_parts[common] : b_parts[common]);
    return (a_longer == extra_number) ? 1 : -1;
}

bool is_maven_range(std::string_view version) {
    return (version.starts_with('[') || version.starts_with('(')) &&
           parse_range(version).has_value();
}

bool in_maven_range(std::string_view version, std::string_view range) {
    const std::optional<std::vector<Interval>> intervals = parse_range(range);
    return intervals && std::ranges::any_of(*intervals, [&](const Interval& interval) {
               return in_interval(version, interval);
           });
}

std::vector<std::string> maven_repositories(const std::vector<std::string>& listed) {
    std::vector<std::string> out;
    if (listed.empty()) {
        out.assign(kDefaultMavenRepositories.begin(), kDefaultMavenRepositories.end());
    } else {
        out = listed;
    }
    for (std::string& repository : out) {
        while (repository.ends_with('/')) {
            repository.pop_back();
        }
    }
    return out;
}

Result<MavenLock> resolve_maven(const std::vector<std::string>& dependencies,
                                const MavenOptions& options) {
    return Resolver(options).resolve(dependencies);
}

std::vector<MavenArtifact>
merge_maven_artifacts(const std::vector<std::vector<MavenArtifact>>& graphs) {
    std::vector<MavenArtifact> out;
    std::map<std::string, std::size_t> by_module; // "group:artifact:classifier" to its index
    std::map<std::string, std::string> versions;  // and its version
    for (const std::vector<MavenArtifact>& graph : graphs) {
        for (const MavenArtifact& artifact : graph) {
            auto coordinate = parse_maven_coordinate(artifact.id);
            if (!coordinate) {
                continue; // parse_maven_lock checked every id
            }
            const std::string key =
                coordinate.value().module() + ":" + coordinate.value().classifier;
            const auto [found, added] = by_module.emplace(key, out.size());
            if (added) {
                versions[key] = coordinate.value().version;
                out.push_back(artifact);
            } else if (compare_maven_versions(coordinate.value().version, versions[key]) > 0) {
                versions[key] = coordinate.value().version;
                out[found->second] = artifact;
            }
        }
    }
    return out;
}

fs::path maven_artifact_path(const MavenArtifact& artifact, const fs::path& cache) {
    std::vector<std::string_view> parts;
    std::string_view rest = artifact.id;
    while (true) {
        const std::size_t colon = rest.find(':');
        parts.push_back(rest.substr(0, colon));
        if (colon == std::string_view::npos) {
            break;
        }
        rest = rest.substr(colon + 1);
    }
    parts.resize(4);
    return cache / maven_folder(parts[0], parts[1], parts[2]) /
           file_name(parts[1], parts[2], parts[3], artifact.type);
}

Result<fs::path> fetch_maven_artifact(const MavenArtifact& artifact, const fs::path& cache,
                                      const std::string& log_path) {
    std::error_code error;
    const fs::path file = maven_artifact_path(artifact, cache);
    if (fs::is_regular_file(file, error)) {
        if (auto sha256 = sha256_file(file); sha256 && sha256.value() == artifact.sha256) {
            return file;
        }
    }
    if (auto fetched = download(artifact.url, file, log_path); !fetched) {
        return make_error(fetched.error().code, "could not download " + artifact.id + " from " +
                                                    artifact.url + ": " + fetched.error().message);
    }
    auto sha256 = sha256_file(file);
    if (!sha256) {
        return sha256.error();
    }
    if (sha256.value() != artifact.sha256) {
        fs::remove(file, error);
        return make_error(ErrorCode::InvalidArgument, artifact.id + " from " + artifact.url +
                                                          " has SHA-256 " + sha256.value() +
                                                          ", not the " + artifact.sha256 +
                                                          " sdk.lock.json pins; it was deleted");
    }
    if (auto written = platform::write_file(file.string() + ".url", artifact.url); !written) {
        return written.error();
    }
    return file;
}

json::Value maven_lock_json(const MavenLock& lock) {
    json::Value out;
    json::Array dependencies;
    for (const std::string& dependency : lock.dependencies) {
        dependencies.emplace_back(dependency);
    }
    out["dependencies"] = std::move(dependencies);
    json::Array repositories;
    for (const std::string& repository : lock.repositories) {
        repositories.emplace_back(repository);
    }
    out["repositories"] = std::move(repositories);
    json::Array artifacts;
    for (const MavenArtifact& artifact : lock.artifacts) {
        json::Value item;
        item["id"] = artifact.id;
        item["type"] = artifact.type;
        item["url"] = artifact.url;
        item["sha256"] = artifact.sha256;
        artifacts.push_back(std::move(item));
    }
    out["artifacts"] = std::move(artifacts);
    return out;
}

Result<MavenLock> parse_maven_lock(const json::Value& value) {
    if (!value.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "/maven: expected an object");
    }
    MavenLock out;
    out.dependencies = string_list(value.find("dependencies"));
    out.repositories = string_list(value.find("repositories"));
    const json::Value* artifacts = value.find("artifacts");
    if (artifacts == nullptr || !artifacts->is_array()) {
        return make_error(ErrorCode::InvalidArgument, "/maven/artifacts: expected an array");
    }
    for (std::size_t i = 0; i < artifacts->as_array().size(); ++i) {
        const json::Value& item = artifacts->as_array()[i];
        const std::string path = "/maven/artifacts/" + std::to_string(i);
        MavenArtifact artifact;
        for (const auto& [field, target] :
             {std::pair<const char*, std::string*>{"id", &artifact.id},
              std::pair<const char*, std::string*>{"type", &artifact.type},
              std::pair<const char*, std::string*>{"url", &artifact.url},
              std::pair<const char*, std::string*>{"sha256", &artifact.sha256}}) {
            const json::Value* text = item.is_object() ? item.find(field) : nullptr;
            if (text == nullptr || !text->is_string()) {
                return make_error(ErrorCode::InvalidArgument,
                                  path + ": expected a string \"" + field + "\"");
            }
            *target = text->as_string();
        }
        if (!parse_maven_coordinate(artifact.id) ||
            (artifact.type != "aar" && artifact.type != "jar")) {
            return make_error(ErrorCode::InvalidArgument,
                              path + ": expected a group:artifact:version id and a type of aar "
                                     "or jar");
        }
        if (artifact.sha256.size() != 64 ||
            artifact.sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return make_error(ErrorCode::InvalidArgument,
                              path + "/sha256: expected 64 lowercase hex digits");
        }
        out.artifacts.push_back(std::move(artifact));
    }
    return out;
}

} // namespace slash::exporter
