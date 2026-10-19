#include "slash/exporter/sdk.hpp"

#include "steps.hpp"

#include "slash/core/text.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/exporter/xml.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::display;
using detail::environment;

constexpr std::size_t kMaxName = 64;
// Written last into a fetched download's folder, so a half-unpacked one never counts.
constexpr std::string_view kMarker = ".slash-sdk-download.json";
// Characters of the SHA-256 that name a download's folder.
constexpr std::size_t kFolderHash = 16;

bool lower_letter(char c) {
    return c >= 'a' && c <= 'z';
}

bool upper_letter(char c) {
    return c >= 'A' && c <= 'Z';
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxName || !(lower_letter(name[0]) || digit(name[0]))) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return lower_letter(c) || digit(c) || c == '-' || c == '_' || c == '.';
    });
}

// Placeholders a target's paths can't use as download names.
constexpr std::array<std::string_view, 3> kReserved{"sdk", "build", "arch"};

bool valid_download_id(std::string_view id) {
    if (id.empty() || !lower_letter(id[0]) || std::ranges::find(kReserved, id) != kReserved.end()) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) { return lower_letter(c) || digit(c) || c == '_'; });
}

bool valid_key_name(std::string_view name) {
    if (name.empty() || digit(name[0])) {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c) { return upper_letter(c) || digit(c) || c == '_'; });
}

bool valid_identifier(std::string_view name) {
    if (name.empty() || digit(name[0])) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return upper_letter(c) || lower_letter(c) || digit(c) || c == '_'; });
}

bool valid_sha256(std::string_view text) {
    constexpr std::size_t kLength = 64;
    return text.size() == kLength &&
           std::ranges::all_of(text, [](char c) { return digit(c) || (c >= 'a' && c <= 'f'); });
}

bool is_apple(std::string_view platform) {
    return platform == "macos" || platform == "ios";
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value string_list(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    schema["items"]["minLength"] = 1;
    return schema;
}

json::Value object_schema(json::Object properties, std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = description;
    schema["properties"] = std::move(properties);
    return schema;
}

// The fields of a platform's section, as SdkTarget has them.
json::Value target_schema() {
    json::Object properties;
    properties["include"] = string_list("Header folders the game's code is compiled with.");
    properties["defines"] =
        string_list("Preprocessor definitions for the game's code, NAME or NAME=value; values "
                    "may use ${KEY}.");
    properties["libraries"] =
        string_list("Static libraries, shared libraries and Windows import libraries to link. "
                    "Shared libraries are shipped where the platform's loader finds them.");
    properties["frameworks"] =
        string_list("Apple .framework or .xcframework folders to link (macOS and iOS); dynamic "
                    "ones are embedded and signed.");
    properties["system_libraries"] =
        string_list("Libraries of the system or compiler to link by name, e.g. \"ws2_32\".");
    properties["system_frameworks"] =
        string_list("Apple frameworks to link by name, e.g. \"StoreKit\" (macOS and iOS).");
    properties["link_options"] = string_list("Options for the linker, e.g. \"-ObjC\".");
    properties["files"] =
        string_list("Files shipped beside the game: next to the executable on Windows and "
                    "Linux, in Resources on macOS, in the app on iOS, in assets on Android and "
                    "next to index.html on the web. Windows DLLs go here.");
    json::Value plist = property("object", "Info.plist entries (macOS and iOS); strings may use "
                                           "${KEY}.");
    properties["plist"] = std::move(plist);
    properties["entitlements"] =
        property("object", "Code signing entitlements (macOS and iOS); strings may use ${KEY}.");
    properties["permissions"] =
        string_list("Android permissions, e.g. \"android.permission.INTERNET\".");
    properties["manifest"] =
        string_list("XML elements added to AndroidManifest.xml's <manifest>, e.g. <queries>; "
                    "may use ${KEY}.");
    properties["application"] =
        string_list("XML elements added to AndroidManifest.xml's <application>, e.g. "
                    "<meta-data>; may use ${KEY}.");
    properties["jars"] = string_list("Java libraries the game's Java is compiled against and "
                                     "packaged with (Android).");
    properties["aars"] = string_list(
        "Android libraries: .aar files, or folders laid out like an unpacked one, added to the "
        "APK with their manifests, resources, assets and native libraries (Android).");
    properties["maven"] = string_list(
        "Maven coordinates of Android libraries, group:artifact:version, resolved with what they "
        "need into sdk.lock.json by slash-cli sdk lock (Android).");
    properties["maven_repositories"] =
        string_list("https:// or file:// Maven repositories searched in order; default Google's "
                    "and Maven Central (Android).");
    return object_schema(std::move(properties),
                         "What the package adds to this platform's builds. Paths are relative to "
                         "the package and may start with {sdk}, {build} or a download's {name}, "
                         "and contain {arch}.");
}

// Every "${NAME}" in text.
std::vector<std::string> keys_in(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = text.find("${", at)) != std::string_view::npos) {
        const std::size_t end = text.find('}', at + 2);
        if (end == std::string_view::npos) {
            out.emplace_back(text.substr(at + 2));
            break;
        }
        out.emplace_back(text.substr(at + 2, end - at - 2));
        at = end + 1;
    }
    return out;
}

// Every "{name}" in a path.
std::vector<std::string> placeholders_in(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = text.find('{', at)) != std::string_view::npos) {
        const std::size_t end = text.find('}', at + 1);
        if (end == std::string_view::npos) {
            out.emplace_back(text.substr(at + 1));
            break;
        }
        out.emplace_back(text.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return out;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// text with each ${KEY} replaced by its value, XML-escaped if escape.
std::string with_keys(std::string_view text, const std::map<std::string, std::string>& keys,
                      bool escape) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t start = text.find("${", at);
        if (start == std::string_view::npos) {
            out += text.substr(at);
            break;
        }
        const std::size_t end = text.find('}', start + 2);
        if (end == std::string_view::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, start - at);
        const auto value = keys.find(std::string(text.substr(start + 2, end - start - 2)));
        if (value != keys.end()) {
            out += escape ? xml_escaped(value->second) : value->second;
        }
        at = end + 1;
    }
    return out;
}

json::Value value_with_keys(const json::Value& value,
                            const std::map<std::string, std::string>& keys) {
    if (value.is_string()) {
        return with_keys(value.as_string(), keys, false);
    }
    if (value.is_array()) {
        json::Array out;
        for (const json::Value& item : value.as_array()) {
            out.push_back(value_with_keys(item, keys));
        }
        return out;
    }
    if (value.is_object()) {
        json::Object out;
        for (const auto& [key, item] : value.as_object()) {
            out[key] = value_with_keys(item, keys);
        }
        return out;
    }
    return value;
}

// Every string in a plist or entitlements value, with its JSON Pointer.
void strings_in(const json::Value& value, const std::string& path,
                std::vector<std::pair<std::string, std::string>>& out) {
    if (value.is_string()) {
        out.emplace_back(path, value.as_string());
    } else if (value.is_array()) {
        const json::Array& items = value.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            strings_in(items[i], path + "/" + std::to_string(i), out);
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.as_object()) {
            strings_in(item, path + "/" + json::pointer_escape(key), out);
        }
    }
}

bool has_null(const json::Value& value) {
    if (value.is_null()) {
        return true;
    }
    if (value.is_array()) {
        return std::ranges::any_of(value.as_array(), has_null);
    }
    if (value.is_object()) {
        return std::ranges::any_of(value.as_object(),
                                   [](const auto& member) { return has_null(member.second); });
    }
    return false;
}

std::vector<std::string> strings(const json::Value* list) {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const json::Value& item : list->as_array()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

const json::Value& object_or_empty(const json::Value* value) {
    static const json::Value kEmpty = json::Object{};
    return value != nullptr ? *value : kEmpty;
}

// Which fields only some platforms have.
constexpr std::array<std::string_view, 4> kAppleFields{"frameworks", "system_frameworks", "plist",
                                                       "entitlements"};
constexpr std::array<std::string_view, 7> kAndroidFields{
    "permissions", "manifest", "application", "jars", "aars", "maven", "maven_repositories"};
constexpr std::array<std::string_view, 6> kPathFields{"include", "libraries", "frameworks",
                                                      "files",   "jars",      "aars"};

void check_target(const std::string& platform, const json::Value& target,
                  const std::set<std::string, std::less<>>& placeholders,
                  const std::set<std::string, std::less<>>& keys,
                  std::vector<json::ValidationError>& errors) {
    const std::string base = "/platforms/" + platform;
    for (const auto& [field, value] : target.as_object()) {
        std::string path = base;
        path += "/";
        path += field;
        if (!is_apple(platform) && std::ranges::find(kAppleFields, field) != kAppleFields.end()) {
            errors.push_back({path, "only macOS and iOS have " + field});
        }
        if (platform != "android" &&
            std::ranges::find(kAndroidFields, field) != kAndroidFields.end()) {
            errors.push_back({path, "only Android has " + field});
        }
    }
    const auto check_keys = [&](const std::string& path, std::string_view text) {
        for (const std::string& key : keys_in(text)) {
            if (!keys.contains(key)) {
                std::string message = "${" + key + "} is not one of the package's keys";
                if (const std::string_view hint = closest_match(key, keys); !hint.empty()) {
                    message += "; did you mean ${" + std::string(hint) + "}?";
                }
                errors.push_back({path, message + " (add it to \"keys\")"});
            }
        }
    };
    for (const std::string_view field : kPathFields) {
        const std::vector<std::string> paths = strings(target.find(field));
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const std::string path = base + "/" + std::string(field) + "/" + std::to_string(i);
            if (paths[i].find("${") != std::string::npos) {
                errors.push_back({path, "paths can't use environment variables; give a download "
                                        "a \"path\" and use its {name}"});
                continue;
            }
            for (const std::string& name : placeholders_in(paths[i])) {
                if (!placeholders.contains(name)) {
                    std::string message = "{" + name + "} is not a placeholder";
                    if (const std::string_view hint = closest_match(name, placeholders);
                        !hint.empty()) {
                        message += "; did you mean {" + std::string(hint) + "}?";
                    }
                    errors.push_back({path, message});
                }
            }
            if (paths[i].find("..") != std::string::npos) {
                errors.push_back({path, "paths can't use \"..\""});
            }
        }
    }
    const std::vector<std::string> defines = strings(target.find("defines"));
    for (std::size_t i = 0; i < defines.size(); ++i) {
        const std::string path = base + "/defines/" + std::to_string(i);
        const std::string_view name = std::string_view(defines[i]).substr(0, defines[i].find('='));
        if (!valid_identifier(name)) {
            errors.push_back({path, "expected NAME or NAME=value, NAME a C identifier"});
        }
        check_keys(path, defines[i]);
    }
    for (const std::string_view field :
         {std::string_view("manifest"), std::string_view("application")}) {
        const std::vector<std::string> elements = strings(target.find(field));
        for (std::size_t i = 0; i < elements.size(); ++i) {
            const std::string path = base + "/" + std::string(field) + "/" + std::to_string(i);
            if (!elements[i].starts_with('<') || !elements[i].ends_with('>')) {
                errors.push_back({path, "expected an XML element, e.g. <meta-data ... />"});
            }
            check_keys(path, elements[i]);
        }
    }
    const std::vector<std::string> aars = strings(target.find("aars"));
    for (std::size_t i = 0; i < aars.size(); ++i) {
        if (aars[i].find("{arch}") != std::string::npos ||
            aars[i].find("{build}") != std::string::npos) {
            errors.push_back({base + "/aars/" + std::to_string(i),
                              "Android libraries serve every ABI; their paths can't use {arch} "
                              "or {build}"});
        }
    }
    const std::vector<std::string> coordinates = strings(target.find("maven"));
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        if (auto parsed = parse_maven_coordinate(coordinates[i]); !parsed) {
            errors.push_back({base + "/maven/" + std::to_string(i), parsed.error().message});
        }
    }
    const std::vector<std::string> repositories = strings(target.find("maven_repositories"));
    for (std::size_t i = 0; i < repositories.size(); ++i) {
        if (!repositories[i].starts_with("https://") && !repositories[i].starts_with("file://")) {
            errors.push_back({base + "/maven_repositories/" + std::to_string(i),
                              "expected an https:// or file:// address"});
        }
    }
    const std::vector<std::string> permissions = strings(target.find("permissions"));
    for (std::size_t i = 0; i < permissions.size(); ++i) {
        if (permissions[i].find_first_of("<>\"' ") != std::string::npos) {
            errors.push_back({base + "/permissions/" + std::to_string(i),
                              "expected a permission name, e.g. android.permission.INTERNET"});
        }
    }
    for (const std::string_view field :
         {std::string_view("plist"), std::string_view("entitlements")}) {
        const json::Value* entries = target.find(field);
        if (entries == nullptr) {
            continue;
        }
        const std::string path = base + "/" + std::string(field);
        if (has_null(*entries)) {
            errors.push_back({path, "property lists have no null; use a string, number, "
                                    "boolean, array or object"});
        }
        std::vector<std::pair<std::string, std::string>> texts;
        strings_in(*entries, path, texts);
        for (const auto& [where, text] : texts) {
            check_keys(where, text);
        }
    }
}

// A download's local path with each ${VARIABLE} from the environment; missing names the first
// variable that isn't set.
std::string expand_environment(std::string_view text, std::string& missing) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t start = text.find("${", at);
        const std::size_t end = start == std::string_view::npos ? start : text.find('}', start);
        if (end == std::string_view::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, start - at);
        const std::string name(text.substr(start + 2, end - start - 2));
        const std::string value = environment(name);
        if (value.empty() && missing.empty()) {
            missing = name;
        }
        out += value;
        at = end + 1;
    }
    return out;
}

// The file name at the end of a URL.
std::string url_file_name(std::string_view url) {
    url = url.substr(0, url.find_first_of("?#"));
    const std::size_t slash = url.rfind('/');
    std::string name(slash == std::string_view::npos ? url : url.substr(slash + 1));
    if (name.empty() || name == "." || name == "..") {
        return "download";
    }
    return name;
}

// Whether the URL names an archive to unpack: zip and the tar formats CMake reads.
bool archive_url(std::string_view url) {
    url = url.substr(0, url.find_first_of("?#"));
    constexpr std::array<std::string_view, 9> kSuffixes = {
        ".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".7z"};
    return std::ranges::any_of(kSuffixes,
                               [&](std::string_view suffix) { return url.ends_with(suffix); });
}

std::string tail_of(const Result<platform::ProcessResult>& result) {
    if (!result) {
        return result.error().message;
    }
    std::string tail = result.value().tail;
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) {
        tail.pop_back();
    }
    return tail;
}

SdkTarget parse_target(const json::Value& value) {
    SdkTarget target;
    target.include = strings(value.find("include"));
    target.defines = strings(value.find("defines"));
    target.libraries = strings(value.find("libraries"));
    target.frameworks = strings(value.find("frameworks"));
    target.system_libraries = strings(value.find("system_libraries"));
    target.system_frameworks = strings(value.find("system_frameworks"));
    target.link_options = strings(value.find("link_options"));
    target.files = strings(value.find("files"));
    target.plist = object_or_empty(value.find("plist"));
    target.entitlements = object_or_empty(value.find("entitlements"));
    target.permissions = strings(value.find("permissions"));
    target.manifest = strings(value.find("manifest"));
    target.application = strings(value.find("application"));
    target.jars = strings(value.find("jars"));
    target.aars = strings(value.find("aars"));
    target.maven = strings(value.find("maven"));
    target.maven_repositories = strings(value.find("maven_repositories"));
    return target;
}

Sdk parse_sdk(const json::Value& value, const fs::path& folder) {
    Sdk sdk;
    sdk.folder = folder;
    sdk.name = value.find("name")->as_string();
    sdk.version = value.find("version")->as_string();
    sdk.license = value.find("license")->as_string();
    const auto text = [&](std::string_view key) {
        const json::Value* found = value.find(key);
        return found != nullptr ? found->as_string() : std::string();
    };
    sdk.vendor = text("vendor");
    sdk.description = text("description");
    sdk.license_file = text("license_file");
    if (const json::Value* downloads = value.find("downloads"); downloads != nullptr) {
        for (const auto& [id, item] : downloads->as_object()) {
            SdkDownload download;
            download.id = id;
            const auto field = [&](std::string_view key) {
                const json::Value* found = item.find(key);
                return found != nullptr ? found->as_string() : std::string();
            };
            download.url = field("url");
            download.sha256 = field("sha256");
            download.folder = field("folder");
            download.path = field("path");
            const json::Value* unpack = item.find("unpack");
            download.unpack = unpack != nullptr ? unpack->as_bool() : archive_url(download.url);
            sdk.downloads.push_back(std::move(download));
        }
    }
    if (const json::Value* build = value.find("build"); build != nullptr) {
        sdk.build = build->find("cmake")->as_string();
        if (const json::Value* options = build->find("options"); options != nullptr) {
            for (const auto& [name, option] : options->as_object()) {
                sdk.build_options[name] = option.as_string();
            }
        }
    }
    if (const json::Value* keys = value.find("keys"); keys != nullptr) {
        for (const auto& [name, item] : keys->as_object()) {
            SdkKey key;
            key.name = name;
            if (const json::Value* description = item.find("description"); description != nullptr) {
                key.description = description->as_string();
            }
            if (const json::Value* optional = item.find("optional"); optional != nullptr) {
                key.optional = optional->as_bool();
            }
            sdk.keys.push_back(std::move(key));
        }
    }
    if (const json::Value* platforms = value.find("platforms"); platforms != nullptr) {
        for (const auto& [platform, target] : platforms->as_object()) {
            sdk.platforms[platform] = parse_target(target);
        }
    }
    return sdk;
}

// Reads and checks <folder>/sdk.json, adding a problem for each error.
std::optional<Sdk> read_sdk(const fs::path& folder, const std::string& location,
                            std::vector<SdkProblem>& problems) {
    const fs::path file = folder / kSdkFile;
    auto text = platform::read_file(file.string());
    if (!text) {
        std::error_code error;
        problems.push_back({location, "",
                            fs::is_directory(folder, error)
                                ? "the folder has no " + std::string(kSdkFile)
                                : display(folder) +
                                      " is not a folder; project.json's \"sdks\" lists the "
                                      "folders of SDK packages"});
        return std::nullopt;
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        problems.push_back({location, "", parsed.error().message});
        return std::nullopt;
    }
    const std::vector<json::ValidationError> errors = validate_sdk(parsed.value());
    if (!errors.empty()) {
        for (const json::ValidationError& error : errors) {
            problems.push_back({location, error.path, error.message});
        }
        return std::nullopt;
    }
    Sdk sdk = parse_sdk(parsed.value(), folder);
    sdk.location = location;
    return sdk;
}

// The slice of an .xcframework for platform and arch, read from its Info.plist with plutil.
struct Slice {
    fs::path path;    // a .framework, or a library
    fs::path headers; // the library's headers; empty for a framework
};

std::optional<Slice> xcframework_slice(const fs::path& xcframework, std::string_view platform,
                                       std::string_view arch, std::string& why) {
    platform::ProcessOptions process;
    process.tail_lines = 100000;
    auto converted = platform::run_process(
        {"plutil", "-convert", "json", "-o", "-", (xcframework / "Info.plist").string()}, process);
    if (!converted || converted.value().exit_code != 0) {
        why = "could not read its Info.plist with plutil: " + tail_of(converted);
        return std::nullopt;
    }
    auto info = json::parse(converted.value().tail);
    const json::Value* libraries = info ? info.value().find("AvailableLibraries") : nullptr;
    if (libraries == nullptr || !libraries->is_array()) {
        why = "its Info.plist lists no AvailableLibraries";
        return std::nullopt;
    }
    const std::string want_platform = platform == "macos" ? "macos" : "ios";
    const std::string want_variant = platform == "ios" && arch == kIosSimulator ? "simulator" : "";
    for (const json::Value& library : libraries->as_array()) {
        const json::Value* supported = library.find("SupportedPlatform");
        const json::Value* variant = library.find("SupportedPlatformVariant");
        const json::Value* identifier = library.find("LibraryIdentifier");
        const json::Value* path = library.find("LibraryPath");
        if (supported == nullptr || !supported->is_string() || identifier == nullptr ||
            !identifier->is_string() || path == nullptr || !path->is_string() ||
            supported->as_string() != want_platform) {
            continue;
        }
        const std::string found_variant =
            variant != nullptr && variant->is_string() ? variant->as_string() : "";
        if (found_variant != want_variant) {
            continue;
        }
        Slice slice;
        const fs::path folder = xcframework / identifier->as_string();
        slice.path = folder / path->as_string();
        if (const json::Value* headers = library.find("HeadersPath");
            headers != nullptr && headers->is_string()) {
            slice.headers = folder / headers->as_string();
        }
        return slice;
    }
    why = "it has no library for " +
          (want_variant.empty() ? want_platform : want_platform + " " + want_variant);
    return std::nullopt;
}

// Bytes at the start of a file.
std::string file_start(const fs::path& path, std::size_t offset, std::size_t count) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(static_cast<std::streamoff>(offset));
    std::string bytes(count, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(count));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

std::uint32_t read_u32(std::string_view bytes, std::size_t at, bool big_endian) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto byte = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i]));
        value |= big_endian ? byte << ((3 - i) * 8) : byte << (i * 8);
    }
    return value;
}

// Whether the bytes at offset in a file start a shared library.
bool shared_at(const fs::path& path, std::size_t offset, int depth) {
    constexpr std::size_t kHeader = 32;
    const std::string bytes = file_start(path, offset, kHeader);
    if (bytes.size() < 8 || bytes.starts_with("!<arch>\n")) {
        return false;
    }
    const auto byte = [&](std::size_t i) {
        return static_cast<unsigned>(static_cast<unsigned char>(bytes[i]));
    };
    if (bytes.starts_with("\x7f"
                          "ELF") &&
        bytes.size() >= 18) {
        constexpr unsigned kSharedObject = 3; // ET_DYN
        const bool big = byte(5) == 2;
        const unsigned type = big ? (byte(16) << 8U) | byte(17) : byte(16) | (byte(17) << 8U);
        return type == kSharedObject;
    }
    if (bytes.starts_with("MZ")) {
        return true; // a PE file: DLLs; import libraries are archives
    }
    const std::uint32_t magic_le = read_u32(bytes, 0, false);
    constexpr std::uint32_t kMachO32 = 0xfeedface;
    constexpr std::uint32_t kMachO64 = 0xfeedfacf;
    constexpr std::uint32_t kDylib = 6; // MH_DYLIB
    if ((magic_le == kMachO32 || magic_le == kMachO64) && bytes.size() >= 16) {
        return read_u32(bytes, 12, false) == kDylib;
    }
    constexpr std::uint32_t kFat = 0xcafebabe;
    constexpr std::uint32_t kFat64 = 0xcafebabf;
    const std::uint32_t magic_be = read_u32(bytes, 0, true);
    if ((magic_be == kFat || magic_be == kFat64) && depth == 0 && bytes.size() >= 32) {
        // The first architecture's offset: fat_arch is cputype, cpusubtype, offset, ...;
        // fat_arch_64 has 64-bit offsets.
        const std::size_t first =
            magic_be == kFat ? read_u32(bytes, 16, true) : read_u32(bytes, 20, true);
        return shared_at(path, first, depth + 1);
    }
    return false;
}

void add_problem(std::vector<SdkProblem>& problems, const Sdk& sdk, std::string path,
                 std::string message) {
    problems.push_back({sdk.location, std::move(path), std::move(message)});
}

// A CMake bracket argument holding text, with enough '='s that nothing in it closes it early.
std::string bracket(std::string_view text) {
    const std::string closed = std::string(text) + "]";
    std::string equals;
    while (closed.find("]" + equals + "]") != std::string::npos) {
        equals += '=';
    }
    return "[" + equals + "[" + std::string(text) + "]" + equals + "]";
}

void plist_value(const json::Value& value, int indent, std::string& out) {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    if (value.is_string()) {
        out += pad + "<string>" + xml_escaped(value.as_string()) + "</string>\n";
    } else if (value.is_bool()) {
        out += pad + (value.as_bool() ? "<true/>\n" : "<false/>\n");
    } else if (value.is_int()) {
        out += pad + "<integer>" + std::to_string(value.as_int()) + "</integer>\n";
    } else if (value.is_double()) {
        out += pad + "<real>" + json::write(value) + "</real>\n";
    } else if (value.is_array()) {
        if (value.as_array().empty()) {
            out += pad + "<array/>\n";
            return;
        }
        out += pad + "<array>\n";
        for (const json::Value& item : value.as_array()) {
            plist_value(item, indent + 1, out);
        }
        out += pad + "</array>\n";
    } else if (value.is_object()) {
        if (value.as_object().empty()) {
            out += pad + "<dict/>\n";
            return;
        }
        out += pad + "<dict>\n";
        for (const auto& [key, item] : value.as_object()) {
            out += pad + "    <key>" + xml_escaped(key) + "</key>\n";
            plist_value(item, indent + 1, out);
        }
        out += pad + "</dict>\n";
    }
}

} // namespace

std::string Sdk::define() const {
    std::string out = "SLASH_SDK_";
    for (const char c : name) {
        if (lower_letter(c)) {
            out += static_cast<char>(c - 'a' + 'A');
        } else {
            out += digit(c) ? c : '_';
        }
    }
    return out;
}

json::Value sdk_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine SDK package";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "name", "version", "license"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = kSdkFormat;
    properties["format"]["description"] = "The sdk.json format version: 1.";
    properties["name"] = property("string", "The package's name: lowercase letters, digits, "
                                            "'-', '_' and '.'. The game is compiled with "
                                            "SLASH_SDK_<NAME> defined.");
    properties["version"] = property("string", "The vendor SDK's version.");
    properties["version"]["minLength"] = 1;
    properties["vendor"] = property("string", "Who makes the SDK.");
    properties["description"] = property("string", "What the SDK does, in a sentence.");
    properties["license"] =
        property("string", "The SDK's license: an SPDX identifier, or \"Proprietary\". Exports "
                           "list it.");
    properties["license"]["minLength"] = 1;
    properties["license_file"] =
        property("string", "The license text, shipped in the game's data under licenses/.");

    json::Object download;
    download["url"] = property("string", "Where to download it: an https:// (or file://) URL.");
    download["sha256"] = property("string", "The download's SHA-256, 64 lowercase hex digits.");
    download["folder"] = property("string", "The folder inside the unpacked archive to use.");
    download["unpack"] = property("boolean", "Unpack the archive. Default: true for .zip and "
                                             ".tar files, false for anything else.");
    download["path"] = property("string", "A file or folder on this machine instead of a URL, "
                                          "e.g. \"${STEAMWORKS_SDK}\"; ${NAME}s come from the "
                                          "environment.");
    json::Value downloads = property("object", "The vendor's files by name, each downloaded "
                                               "once and checked, or found on this machine. "
                                               "Paths use a download as {name}.");
    downloads["additionalProperties"] = object_schema(std::move(download), "One download.");
    properties["downloads"] = std::move(downloads);

    json::Object build;
    build["cmake"] = property("string", "A folder in the package with a CMakeLists.txt.");
    json::Value options = property("object", "-D options for it, by name.");
    options["additionalProperties"]["type"] = "string";
    build["options"] = std::move(options);
    json::Value build_schema =
        object_schema(std::move(build), "A CMake project in the package that each export builds "
                                        "with the game's compilers first and installs; paths "
                                        "use it as {build}.");
    build_schema["required"] = json::Array{"cmake"};
    properties["build"] = std::move(build_schema);

    json::Object key;
    key["description"] = property("string", "What the value is and where to get it.");
    key["optional"] = property("boolean", "An export without it uses \"\" instead of failing.");
    json::Value keys = property("object", "Environment variables the package reads at export "
                                          "time, such as API keys, used as ${NAME}. Secrets "
                                          "never go in the file.");
    keys["additionalProperties"] = object_schema(std::move(key), "One key.");
    properties["keys"] = std::move(keys);

    json::Object platforms;
    for (const std::string_view platform : kPlatforms) {
        platforms[std::string(platform)] = target_schema();
    }
    properties["platforms"] =
        object_schema(std::move(platforms), "What the package adds to each platform's builds; "
                                            "exports for other platforms leave it out.");
    return schema;
}

std::vector<json::ValidationError> validate_sdk(const json::Value& sdk) {
    std::vector<json::ValidationError> errors = json::validate(sdk, sdk_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (!valid_name(sdk.find("name")->as_string())) {
        errors.push_back({"/name", "SDK names are 1 to 64 lowercase letters, digits, '-', '_' and "
                                   "'.', starting with a letter or digit"});
    }
    std::set<std::string, std::less<>> placeholders{"sdk", "arch"};
    if (const json::Value* downloads = sdk.find("downloads"); downloads != nullptr) {
        for (const auto& [id, download] : downloads->as_object()) {
            const std::string path = "/downloads/" + json::pointer_escape(id);
            placeholders.insert(id);
            if (!valid_download_id(id)) {
                errors.push_back({path, "download names are lowercase letters, digits and '_', "
                                        "starting with a letter, and not sdk, build or arch"});
            }
            const json::Value* url = download.find("url");
            const json::Value* sha256 = download.find("sha256");
            const json::Value* local = download.find("path");
            if ((url == nullptr) == (local == nullptr)) {
                errors.push_back({path, "give a download either a \"url\" and \"sha256\", or a "
                                        "\"path\""});
                continue;
            }
            if (url != nullptr) {
                const std::string& address = url->as_string();
                if (!address.starts_with("https://") && !address.starts_with("file://")) {
                    errors.push_back({path + "/url", "expected an https:// URL"});
                }
                if (sha256 == nullptr) {
                    errors.push_back({path, "a download from a URL needs its \"sha256\", so a "
                                            "changed file is caught"});
                } else if (!valid_sha256(sha256->as_string())) {
                    errors.push_back({path + "/sha256", "expected 64 lowercase hex digits"});
                }
                if (const json::Value* folder = download.find("folder");
                    folder != nullptr && !folder->as_string().empty() &&
                    !is_project_path(folder->as_string())) {
                    errors.push_back({path + "/folder", "expected a relative path separated by "
                                                        "'/', with no \".\" or \"..\" parts"});
                }
            } else {
                for (const std::string_view field : {"sha256", "folder", "unpack"}) {
                    if (download.find(field) != nullptr) {
                        errors.push_back({path + "/" + std::string(field),
                                          "only downloads from a URL have " + std::string(field)});
                    }
                }
            }
        }
    }
    if (const json::Value* build = sdk.find("build"); build != nullptr) {
        placeholders.insert("build");
        if (!is_project_path(build->find("cmake")->as_string())) {
            errors.push_back({"/build/cmake", "expected a folder in the package, separated by "
                                              "'/', with no \".\" or \"..\" parts"});
        }
    }
    std::set<std::string, std::less<>> keys;
    if (const json::Value* found = sdk.find("keys"); found != nullptr) {
        for (const auto& [name, key] : found->as_object()) {
            keys.insert(name);
            if (!valid_key_name(name)) {
                errors.push_back({"/keys/" + json::pointer_escape(name),
                                  "key names are environment variable names: capital letters, "
                                  "digits and '_'"});
            }
        }
    }
    if (const json::Value* license_file = sdk.find("license_file"); license_file != nullptr) {
        json::Value target;
        target["files"] = json::Array{license_file->as_string()};
        std::vector<json::ValidationError> found;
        check_target("license", target, placeholders, keys, found);
        for (const json::ValidationError& error : found) {
            errors.push_back({"/license_file", error.message});
        }
    }
    if (const json::Value* platforms = sdk.find("platforms"); platforms != nullptr) {
        for (const auto& [platform, target] : platforms->as_object()) {
            check_target(platform, target, placeholders, keys, errors);
        }
    }
    return errors;
}

Result<Sdk> load_sdk(const fs::path& folder) {
    std::vector<SdkProblem> problems;
    std::optional<Sdk> sdk = read_sdk(folder, display(folder), problems);
    if (!sdk) {
        const SdkProblem& problem = problems.front();
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kSdkFile) + (problem.path.empty() ? "" : " " + problem.path) +
                              ": " + problem.message);
    }
    return std::move(*sdk);
}

std::vector<Sdk> load_project_sdks(const fs::path& project_dir,
                                   const std::vector<std::string>& locations,
                                   std::vector<SdkProblem>& problems) {
    std::vector<Sdk> out;
    std::error_code error;
    for (const std::string& location : locations) {
        const fs::path folder = fs::absolute(project_dir / location, error).lexically_normal();
        std::optional<Sdk> sdk = read_sdk(folder, location, problems);
        if (!sdk) {
            continue;
        }
        const auto same = std::ranges::find(out, sdk->name, &Sdk::name);
        if (same != out.end()) {
            problems.push_back(
                {location, "/name", "\"" + sdk->name + "\" is also the name of " + same->location});
            continue;
        }
        out.push_back(std::move(*sdk));
    }
    return out;
}

std::vector<const Sdk*> sdks_for_preset(const std::vector<Sdk>& sdks, const Preset& preset,
                                        std::vector<SdkProblem>& problems) {
    std::vector<const Sdk*> out;
    if (!preset.sdks) {
        for (const Sdk& sdk : sdks) {
            if (sdk.platforms.contains(preset.platform)) {
                out.push_back(&sdk);
            }
        }
        return out;
    }
    std::vector<std::string_view> names;
    names.reserve(sdks.size());
    for (const Sdk& sdk : sdks) {
        names.emplace_back(sdk.name);
    }
    for (const std::string& name : *preset.sdks) {
        const auto found = std::ranges::find(sdks, name, &Sdk::name);
        if (found == sdks.end()) {
            std::string message = "preset \"" + preset.name + "\" links the SDK \"" + name +
                                  R"(", which no package in project.json's "sdks" is called)";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            problems.push_back(
                {std::string(kPresetsFile), "/presets/" + preset.name + "/sdks", message});
        } else if (!found->platforms.contains(preset.platform)) {
            problems.push_back({found->location, "/platforms",
                                "preset \"" + preset.name +
                                    "\" links it, but it has no section "
                                    "for " +
                                    preset.platform});
        } else {
            out.push_back(&*found);
        }
    }
    return out;
}

std::vector<std::string> sdk_archs(const Preset& preset) {
    const std::string& platform = preset.platform;
    if (platform == "linux") {
        return {preset.linux_options.arch};
    }
    if (platform == "android") {
        return preset.android.abis;
    }
    if (platform == "windows") {
        return {"x86_64"};
    }
    if (platform == "macos") {
        return {"universal"};
    }
    if (platform == "ios") {
        return {preset.ios.destination};
    }
    return {"wasm32"};
}

fs::path sdks_dir() {
    if (const std::string dir = environment(kSdksDirVariable); !dir.empty()) {
        return dir;
    }
#ifdef _WIN32
    const std::string home = environment("USERPROFILE");
#else
    const std::string home = environment("HOME");
#endif
    return fs::path(home) / ".slash" / "sdks";
}

fs::path download_location(const SdkDownload& download, const fs::path& sdk_folder,
                           const fs::path& dir) {
    if (download.url.empty()) {
        std::string missing;
        const fs::path path = expand_environment(download.path, missing);
        return path.is_absolute() ? path : (sdk_folder / path).lexically_normal();
    }
    const fs::path folder = dir / download.sha256.substr(0, kFolderHash);
    if (!download.unpack) {
        return folder / url_file_name(download.url);
    }
    return download.folder.empty() ? folder : folder / download.folder;
}

Result<fs::path> fetch_download(const SdkDownload& download, const fs::path& sdk_folder,
                                const fs::path& dir, const std::string& log_path) {
    std::error_code error;
    const fs::path location = download_location(download, sdk_folder, dir);
    if (download.url.empty()) {
        std::string missing;
        (void)expand_environment(download.path, missing);
        if (!missing.empty()) {
            return make_error(ErrorCode::NotFound, "the download \"" + download.id + "\" is at ${" +
                                                       missing + "}; set " + missing);
        }
        if (!fs::exists(location, error)) {
            return make_error(ErrorCode::NotFound, "the download \"" + download.id +
                                                       "\" is not at " + display(location));
        }
        return location;
    }
    const fs::path target = fs::absolute(dir / download.sha256.substr(0, kFolderHash), error);
    if (fs::is_regular_file(target / kMarker, error)) {
        if (!fs::exists(location, error)) {
            return make_error(ErrorCode::NotFound, "the download \"" + download.id + "\" has no " +
                                                       download.folder + " folder");
        }
        return location;
    }
    // A unique staging folder next to the target, so the final rename stays on one disk and two
    // fetches at once don't collide.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path staging =
        fs::absolute(dir / (".staging-" + download.id + "-" + std::to_string(stamp)), error);
    const fs::path unpacked = staging / "unpacked";
    fs::create_directories(unpacked, error);
    if (error) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not create " + display(unpacked) + ": " + error.message());
    }
    const auto clean_up = [&] {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };
    const fs::path file = download.unpack ? staging / url_file_name(download.url)
                                          : unpacked / url_file_name(download.url);
    platform::ProcessOptions process;
    process.log_path = log_path;
    process.tail_lines = 5;
    auto fetched =
        platform::run_process({"curl", "--fail", "--location", "--silent", "--show-error",
                               "--retry", "3", "--output", file.string(), download.url},
                              process);
    if (!fetched || fetched.value().exit_code != 0) {
        clean_up();
        return make_error(ErrorCode::PlatformFailure, "could not download \"" + download.id +
                                                          "\" from " + download.url + ": " +
                                                          tail_of(fetched));
    }
    auto sha256 = sha256_file(file);
    if (!sha256) {
        clean_up();
        return sha256.error();
    }
    if (sha256.value() != download.sha256) {
        clean_up();
        return make_error(ErrorCode::InvalidArgument,
                          "the download \"" + download.id + "\" from " + download.url +
                              " has SHA-256 " + sha256.value() + ", not the " + download.sha256 +
                              " sdk.json pins; it was deleted");
    }
    if (download.unpack) {
        process.working_directory = unpacked.string();
        auto unpack = platform::run_process({"cmake", "-E", "tar", "xf", file.string()}, process);
        if (!unpack || unpack.value().exit_code != 0) {
            clean_up();
            return make_error(ErrorCode::PlatformFailure,
                              "could not unpack \"" + download.id + "\": " + tail_of(unpack));
        }
    }
    json::Value marker;
    marker["url"] = download.url;
    marker["sha256"] = download.sha256;
    if (auto written = platform::write_file((unpacked / kMarker).string(), json::write(marker));
        !written) {
        clean_up();
        return written.error();
    }
    fs::remove_all(target, error);
    fs::rename(unpacked, target, error);
    clean_up();
    if (error && !fs::is_regular_file(target / kMarker, error)) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not move \"" + download.id + "\" into " + display(target));
    }
    if (!fs::exists(location, error)) {
        return make_error(ErrorCode::NotFound, "the download \"" + download.id + "\" has no " +
                                                   download.folder + " folder");
    }
    return location;
}

std::map<std::string, std::string> sdk_keys(const Sdk& sdk, std::vector<SdkProblem>& problems) {
    std::map<std::string, std::string> out;
    for (const SdkKey& key : sdk.keys) {
        std::string value = environment(key.name);
        if (value.empty() && !key.optional) {
            std::string message = "set the environment variable " + key.name;
            if (!key.description.empty()) {
                message += ": " + key.description;
            }
            add_problem(problems, sdk, "/keys/" + key.name, std::move(message));
        }
        out[key.name] = std::move(value);
    }
    return out;
}

ResolvedSdk resolve_sdk(const Sdk& sdk, std::string_view platform, std::string_view arch,
                        const SdkPlaces& places, bool check_paths,
                        std::vector<SdkProblem>& problems) {
    ResolvedSdk out;
    out.sdk = &sdk;
    out.platform = std::string(platform);
    out.arch = std::string(arch);
    std::error_code error;
    // A path with its placeholders filled in; nullopt when it can't be known yet.
    const auto expand = [&](const std::string& text) -> std::optional<fs::path> {
        std::string path = text;
        if (path.find("{build}") != std::string::npos) {
            if (places.build.empty()) {
                return std::nullopt;
            }
            path = replace_all(path, "{build}", places.build.generic_string());
        }
        path = replace_all(path, "{sdk}", sdk.folder.generic_string());
        path = replace_all(path, "{arch}", arch);
        for (const SdkDownload& download : sdk.downloads) {
            const std::string placeholder = "{" + download.id + "}";
            if (path.find(placeholder) == std::string::npos) {
                continue;
            }
            const auto found = places.downloads.find(download.id);
            if (found == places.downloads.end()) {
                return std::nullopt;
            }
            path = replace_all(path, placeholder, found->second.generic_string());
        }
        const fs::path result = path;
        return result.is_absolute() ? result.lexically_normal()
                                    : (sdk.folder / result).lexically_normal();
    };
    // The paths of one field that exist (or can't be checked yet), with a problem for the rest.
    const auto paths = [&](const std::string& pointer, const std::vector<std::string>& in) {
        std::vector<fs::path> found;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const std::optional<fs::path> path = expand(in[i]);
            if (!path) {
                continue;
            }
            if (check_paths && !fs::exists(*path, error)) {
                add_problem(problems, sdk, pointer + "/" + std::to_string(i),
                            display(*path) + " does not exist");
                continue;
            }
            found.push_back(*path);
        }
        return found;
    };
    if (!sdk.license_file.empty()) {
        if (const std::optional<fs::path> license = expand(sdk.license_file); license) {
            if (check_paths && !fs::is_regular_file(*license, error)) {
                add_problem(problems, sdk, "/license_file", display(*license) + " does not exist");
            } else {
                out.license_file = license->generic_string();
            }
        }
    }
    const auto target = sdk.platforms.find(platform);
    if (target == sdk.platforms.end()) {
        return out;
    }
    const SdkTarget& in = target->second;
    const std::string base = "/platforms/" + std::string(platform);
    const std::map<std::string, std::string>& keys = places.keys;
    for (const fs::path& path : paths(base + "/include", in.include)) {
        out.include.push_back(path.generic_string());
    }
    for (const std::string& define : in.defines) {
        out.defines.push_back(with_keys(define, keys, false));
    }
    const std::vector<fs::path> libraries = paths(base + "/libraries", in.libraries);
    for (const fs::path& library : libraries) {
        const std::string extension = library.extension().string();
        if (platform == "windows" && (extension == ".dll" || extension == ".DLL")) {
            add_problem(problems, sdk, base + "/libraries",
                        display(library.filename()) +
                            " is a DLL: link its import library (.lib) and list the DLL in "
                            "\"files\"");
            continue;
        }
        if (platform == "web" && check_paths && is_shared_library(library)) {
            add_problem(problems, sdk, base + "/libraries",
                        display(library.filename()) +
                            " is a shared library; web games link static libraries built with "
                            "Emscripten");
            continue;
        }
        out.libraries.push_back(library.generic_string());
    }
    for (const fs::path& framework : paths(base + "/frameworks", in.frameworks)) {
        if (framework.extension() != ".xcframework" || !check_paths) {
            out.frameworks.push_back(framework.generic_string());
            continue;
        }
        std::string why;
        const std::optional<Slice> slice = xcframework_slice(framework, platform, arch, why);
        if (!slice) {
            add_problem(problems, sdk, base + "/frameworks",
                        display(framework.filename()) + ": " + why);
            continue;
        }
        if (slice->path.extension() == ".framework") {
            out.frameworks.push_back(slice->path.generic_string());
        } else {
            out.libraries.push_back(slice->path.generic_string());
            if (!slice->headers.empty()) {
                out.include.push_back(slice->headers.generic_string());
            }
        }
    }
    out.system_libraries = in.system_libraries;
    out.system_frameworks = in.system_frameworks;
    out.link_options = in.link_options;
    for (const fs::path& path : paths(base + "/files", in.files)) {
        out.files.push_back(path.generic_string());
    }
    out.plist = value_with_keys(in.plist, keys);
    out.entitlements = value_with_keys(in.entitlements, keys);
    out.permissions = in.permissions;
    for (const std::string& element : in.manifest) {
        out.manifest.push_back(with_keys(element, keys, true));
    }
    for (const std::string& element : in.application) {
        out.application.push_back(with_keys(element, keys, true));
    }
    for (const fs::path& path : paths(base + "/jars", in.jars)) {
        out.jars.push_back(path.generic_string());
    }
    for (const fs::path& path : paths(base + "/aars", in.aars)) {
        out.aars.push_back(path.generic_string());
    }
    return out;
}

json::Value merge_sdk_entries(const std::vector<ResolvedSdk>& sdks, std::string_view field,
                              std::vector<SdkProblem>& problems) {
    json::Value merged = json::Object{};
    std::map<std::string, const Sdk*> owners;
    for (const ResolvedSdk& sdk : sdks) {
        const json::Value& entries = field == "plist" ? sdk.plist : sdk.entitlements;
        if (!entries.is_object()) {
            continue;
        }
        for (const auto& [key, value] : entries.as_object()) {
            if (const json::Value* existing = merged.find(key); existing != nullptr) {
                if (!(*existing == value)) {
                    add_problem(problems, *sdk.sdk,
                                "/platforms/" + sdk.platform + "/" + std::string(field) + "/" +
                                    json::pointer_escape(key),
                                "the SDK " + owners[key]->name + " sets " + key +
                                    " to something else");
                }
                continue;
            }
            merged[key] = value;
            owners[key] = sdk.sdk;
        }
    }
    return merged;
}

std::string sdk_cmake(const std::vector<ResolvedSdk>& sdks, const json::Value& plist) {
    std::string out = "# Written by slash-cli export (tools/exporter/src/sdk.cpp): the SDK "
                      "packages linked\n# into the game, for export/CMakeLists.txt.\n";
    std::string manifest;
    std::string application;
    std::set<std::string, std::less<>> permissions;
    for (const ResolvedSdk& sdk : sdks) {
        out += "slash_game_sdk(" + bracket(sdk.sdk->name) + "\n";
        const auto list = [&](std::string_view keyword, const std::vector<std::string>& items) {
            if (items.empty()) {
                return;
            }
            out += "    " + std::string(keyword);
            for (const std::string& item : items) {
                out += "\n        " + bracket(item);
            }
            out += "\n";
        };
        std::vector<std::string> defines{sdk.sdk->define()};
        defines.insert(defines.end(), sdk.defines.begin(), sdk.defines.end());
        list("DEFINES", defines);
        list("INCLUDE", sdk.include);
        list("LIBRARIES", sdk.libraries);
        std::vector<std::string> shared;
        for (const std::string& library : sdk.libraries) {
            if (is_shared_library(library)) {
                shared.push_back(library);
            }
        }
        list("SHARED", shared);
        list("FRAMEWORKS", sdk.frameworks);
        std::vector<std::string> embed;
        for (const std::string& framework : sdk.frameworks) {
            if (is_shared_library(framework)) {
                embed.push_back(framework);
            }
        }
        list("EMBED", embed);
        list("SYSTEM_LIBRARIES", sdk.system_libraries);
        list("SYSTEM_FRAMEWORKS", sdk.system_frameworks);
        list("LINK_OPTIONS", sdk.link_options);
        list("FILES", sdk.files);
        list("JARS", sdk.jars);
        out += ")\n";
        for (const std::string& permission : sdk.permissions) {
            if (permissions.insert(permission).second) {
                manifest +=
                    "<uses-permission android:name=\"" + xml_escaped(permission) + "\" />\n    ";
            }
        }
        for (const std::string& element : sdk.manifest) {
            manifest += element + "\n    ";
        }
        for (const std::string& element : sdk.application) {
            application += element + "\n        ";
        }
    }
    if (plist.is_object() && !plist.as_object().empty()) {
        const std::string entries = plist_entries(plist);
        out += "string(APPEND SLASH_SDK_PLIST " + bracket(entries) + ")\n";
    }
    if (!manifest.empty()) {
        out += "string(APPEND SLASH_SDK_ANDROID_MANIFEST " + bracket(manifest) + ")\n";
    }
    if (!application.empty()) {
        out += "string(APPEND SLASH_SDK_ANDROID_APPLICATION " + bracket(application) + ")\n";
    }
    return out;
}

std::string plist_entries(const json::Value& entries) {
    std::string out;
    if (!entries.is_object()) {
        return out;
    }
    for (const auto& [key, value] : entries.as_object()) {
        out += "<key>" + xml_escaped(key) + "</key>\n";
        plist_value(value, 0, out);
    }
    return out;
}

Result<json::Value> read_plist(const fs::path& path) {
    platform::ProcessOptions process;
    process.tail_lines = 100000;
    auto converted =
        platform::run_process({"plutil", "-convert", "json", "-o", "-", path.string()}, process);
    if (!converted || converted.value().exit_code != 0) {
        return make_error(ErrorCode::PlatformFailure,
                          "plutil could not read " + display(path) + ": " + tail_of(converted));
    }
    auto parsed = json::parse(converted.value().tail);
    if (!parsed || !parsed.value().is_object()) {
        return make_error(ErrorCode::InvalidArgument, display(path) + " is not a dictionary plist");
    }
    return std::move(parsed).value();
}

std::string plist_document(const json::Value& entries) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n";
    const json::Value dict = entries.is_object() ? entries : json::Value(json::Object{});
    plist_value(dict, 0, out);
    out += "</plist>\n";
    return out;
}

namespace {

// The {arch}es exports for platform build: those of the project's presets for it, or else the
// platform's defaults.
std::vector<std::string> archs_for(std::string_view platform, const Presets& presets) {
    std::vector<std::string> out;
    for (const auto& [name, preset] : presets) {
        if (preset.platform != platform) {
            continue;
        }
        for (std::string& arch : sdk_archs(preset)) {
            if (std::ranges::find(out, arch) == out.end()) {
                out.push_back(std::move(arch));
            }
        }
    }
    if (out.empty()) {
        if (auto defaults = find_preset({}, platform)) {
            out = sdk_archs(defaults.value());
        }
    }
    return out;
}

json::Value problems_json(const std::vector<SdkProblem>& problems) {
    json::Array out;
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    for (const SdkProblem& problem : problems) {
        if (!seen.emplace(problem.sdk, problem.path, problem.message).second) {
            continue;
        }
        json::Value item;
        item["sdk"] = problem.sdk;
        if (!problem.path.empty()) {
            item["path"] = problem.path;
        }
        item["message"] = problem.message;
        out.push_back(std::move(item));
    }
    return out;
}

json::Array platform_names(const Sdk& sdk) {
    json::Array out;
    for (const auto& [platform, target] : sdk.platforms) {
        out.emplace_back(platform);
    }
    return out;
}

bool is_url(std::string_view source) {
    return source.starts_with("https://") || source.starts_with("http://") ||
           source.starts_with("file://") || source.starts_with("git@") ||
           source.starts_with("ssh://");
}

bool is_git(std::string_view source) {
    return source.starts_with("git@") || source.starts_with("ssh://") ||
           source.substr(0, source.find_first_of("?#")).ends_with(".git");
}

// The folder of an unpacked archive with sdk.json: its root, or its only folder.
std::optional<fs::path> package_root(const fs::path& unpacked) {
    std::error_code error;
    if (fs::is_regular_file(unpacked / kSdkFile, error)) {
        return unpacked;
    }
    std::optional<fs::path> only;
    for (const fs::directory_entry& entry : fs::directory_iterator(unpacked, error)) {
        if (entry.is_directory(error)) {
            if (only) {
                return std::nullopt;
            }
            only = entry.path();
        }
    }
    if (only && fs::is_regular_file(*only / kSdkFile, error)) {
        return only;
    }
    return std::nullopt;
}

} // namespace

json::Value check_sdks(const fs::path& project, std::string_view platform, bool fetch, bool keys,
                       const std::string& log_path) {
    std::vector<SdkProblem> problems;
    json::Value out;
    out["sdks"] = json::Array{};
    auto loaded = runtime::load_project(project.string());
    if (!loaded) {
        problems.push_back({std::string(runtime::kProjectFile), "", loaded.error().message});
        out["problems"] = problems_json(problems);
        return out;
    }
    const std::vector<Sdk> sdks = load_project_sdks(project, loaded.value().sdks, problems);
    Presets presets;
    if (auto found = load_presets(project.string())) {
        presets = std::move(found).value();
        for (const auto& [name, preset] : presets) {
            if (platform.empty() || preset.platform == platform) {
                (void)sdks_for_preset(sdks, preset, problems);
            }
        }
    } else {
        problems.push_back({std::string(kPresetsFile), "", found.error().message});
    }
    const fs::path dir = sdks_dir();
    for (const Sdk& sdk : sdks) {
        json::Value item;
        item["name"] = sdk.name;
        item["version"] = sdk.version;
        item["license"] = sdk.license;
        item["folder"] = sdk.location;
        item["platforms"] = platform_names(sdk);
        SdkPlaces places;
        std::vector<SdkProblem> key_problems;
        places.keys = sdk_keys(sdk, key_problems);
        if (keys) {
            problems.insert(problems.end(), key_problems.begin(), key_problems.end());
        }
        json::Array unchecked;
        std::error_code error;
        for (const SdkDownload& download : sdk.downloads) {
            if (!download.url.empty() && !fetch &&
                !fs::is_regular_file(dir / download.sha256.substr(0, kFolderHash) / kMarker,
                                     error)) {
                unchecked.emplace_back(download.id);
                continue;
            }
            auto location = fetch_download(download, sdk.folder, dir, log_path);
            if (!location) {
                add_problem(problems, sdk, "/downloads/" + download.id, location.error().message);
                continue;
            }
            places.downloads[download.id] = location.value();
        }
        if (const auto android = sdk.platforms.find("android");
            android != sdk.platforms.end() && !android->second.maven.empty() &&
            (platform.empty() || platform == "android")) {
            auto lock = load_maven_lock(sdk);
            if (!lock) {
                add_problem(problems, sdk, "/platforms/android/maven", lock.error().message);
            } else {
                item["maven"] = lock.value().artifacts.size();
                bool missing = false;
                for (const MavenArtifact& artifact : lock.value().artifacts) {
                    if (fetch) {
                        if (auto fetched = fetch_maven_artifact(artifact, maven_cache(), log_path);
                            !fetched) {
                            add_problem(problems, sdk, "/platforms/android/maven",
                                        fetched.error().message);
                        }
                    } else if (!fs::is_regular_file(maven_artifact_path(artifact, maven_cache()),
                                                    error)) {
                        missing = true;
                    }
                }
                if (missing) {
                    unchecked.emplace_back("maven");
                }
            }
        }
        if (!unchecked.empty()) {
            item["unchecked"] = std::move(unchecked);
        }
        for (const auto& [name, target] : sdk.platforms) {
            if (!platform.empty() && name != platform) {
                continue;
            }
            for (const std::string& arch : archs_for(name, presets)) {
                (void)resolve_sdk(sdk, name, arch, places, true, problems);
            }
        }
        out["sdks"].push_back(std::move(item));
    }
    out["problems"] = problems_json(problems);
    return out;
}

Result<json::Value> add_sdk(const AddSdkOptions& options) {
    std::error_code error;
    const fs::path project = fs::absolute(options.project, error).lexically_normal();
    const fs::path project_file = project / runtime::kProjectFile;
    auto text = platform::read_file(project_file.string());
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          display(project) + " has no project.json; give the game's folder");
    }
    auto project_json = json::parse(text.value());
    if (!project_json || !project_json.value().is_object()) {
        return make_error(ErrorCode::InvalidArgument,
                          "project.json: " + (project_json ? std::string("expected an object")
                                                           : project_json.error().message));
    }
    if (!options.name.empty() && !valid_name(options.name)) {
        return make_error(ErrorCode::InvalidArgument,
                          "folder names are 1 to 64 lowercase letters, digits, '-', '_' and '.'");
    }
    if (options.source.starts_with("http://")) {
        return make_error(ErrorCode::InvalidArgument,
                          "SDKs are fetched over https://, never http://");
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path staging = project / ".slash" / "sdk-add" / std::to_string(stamp);
    const auto clean_up = [&] {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };
    platform::ProcessOptions process;
    process.tail_lines = 5;
    fs::path root;
    std::string location;
    bool copied = true;
    if (!is_url(options.source)) {
        root = fs::absolute(options.source, error).lexically_normal();
        if (!fs::is_directory(root, error)) {
            return make_error(ErrorCode::NotFound, display(root) + " is not a folder");
        }
        const fs::path relative = root.lexically_relative(project);
        if (!relative.empty() && relative.is_relative() && *relative.begin() != ".." &&
            relative != ".") {
            location = relative.generic_string();
            copied = false;
        }
    } else if (is_git(options.source)) {
        fs::create_directories(staging, error);
        root = staging / "repository";
        std::vector<std::string> clone{"git", "clone", "--depth", "1"};
        if (!options.ref.empty()) {
            clone.insert(clone.end(), {"--branch", options.ref});
        }
        clone.insert(clone.end(), {options.source, root.string()});
        auto cloned = platform::run_process(clone, process);
        if (!cloned || cloned.value().exit_code != 0) {
            clean_up();
            return make_error(ErrorCode::PlatformFailure,
                              "git could not clone " + options.source + ": " + tail_of(cloned));
        }
        fs::remove_all(root / ".git", error);
    } else {
        const fs::path unpacked = staging / "unpacked";
        fs::create_directories(unpacked, error);
        const fs::path archive = staging / url_file_name(options.source);
        auto fetched =
            platform::run_process({"curl", "--fail", "--location", "--silent", "--show-error",
                                   "--retry", "3", "--output", archive.string(), options.source},
                                  process);
        if (!fetched || fetched.value().exit_code != 0) {
            clean_up();
            return make_error(ErrorCode::PlatformFailure,
                              "could not download " + options.source + ": " + tail_of(fetched));
        }
        if (!options.sha256.empty()) {
            auto sha256 = sha256_file(archive);
            if (!sha256 || sha256.value() != options.sha256) {
                clean_up();
                return make_error(ErrorCode::InvalidArgument,
                                  options.source + " has SHA-256 " +
                                      (sha256 ? sha256.value() : std::string("unknown")) +
                                      ", not " + options.sha256);
            }
        }
        process.working_directory = unpacked.string();
        auto unpack =
            platform::run_process({"cmake", "-E", "tar", "xf", archive.string()}, process);
        if (!unpack || unpack.value().exit_code != 0) {
            clean_up();
            return make_error(ErrorCode::PlatformFailure,
                              "could not unpack " + options.source + ": " + tail_of(unpack));
        }
        const std::optional<fs::path> found = package_root(unpacked);
        if (!found) {
            clean_up();
            return make_error(ErrorCode::InvalidArgument,
                              options.source + " has no sdk.json at its root or in its one folder");
        }
        root = *found;
    }

    auto sdk = load_sdk(root);
    if (!sdk) {
        clean_up();
        return sdk.error();
    }
    fs::path destination;
    if (copied) {
        const std::string folder = options.name.empty() ? sdk.value().name : options.name;
        location = "sdks/" + folder;
        destination = project / "sdks" / folder;
        if (fs::exists(destination, error)) {
            clean_up();
            return make_error(ErrorCode::Conflict,
                              location + " already exists; remove it, or add the package under "
                                         "another name");
        }
    }
    json::Value& list = project_json.value()["sdks"];
    if (list.is_null()) {
        list = json::Array{};
    }
    const bool listed = std::ranges::any_of(list.as_array(), [&](const json::Value& item) {
        return item.is_string() && item.as_string() == location;
    });
    if (!options.dry_run) {
        if (copied) {
            fs::create_directories(destination.parent_path(), error);
            fs::copy(root, destination,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
            fs::remove_all(destination / ".git", error);
            if (error) {
                clean_up();
                return make_error(ErrorCode::PlatformFailure, "could not copy the package into " +
                                                                  location + ": " +
                                                                  error.message());
            }
        }
        if (!listed) {
            list.push_back(location);
            if (auto written = platform::write_file(project_file.string(),
                                                    json::write(project_json.value()) + "\n");
                !written) {
                clean_up();
                return written.error();
            }
        }
    }
    clean_up();
    json::Value out;
    out["name"] = sdk.value().name;
    out["version"] = sdk.value().version;
    out["license"] = sdk.value().license;
    out["folder"] = location;
    out["copied"] = copied;
    out["platforms"] = platform_names(sdk.value());
    json::Array keys;
    for (const SdkKey& key : sdk.value().keys) {
        keys.emplace_back(key.name);
    }
    out["keys"] = std::move(keys);
    if (const auto android = sdk.value().platforms.find("android");
        android != sdk.value().platforms.end() && !android->second.maven.empty() &&
        !load_maven_lock(sdk.value())) {
        out["needs_lock"] = true;
    }
    if (options.dry_run) {
        out["dry_run"] = true;
    }
    return out;
}

fs::path maven_cache() {
    return sdks_dir() / "maven";
}

Result<MavenLock> load_maven_lock(const Sdk& sdk) {
    const auto android = sdk.platforms.find("android");
    if (android == sdk.platforms.end() || android->second.maven.empty()) {
        return MavenLock{};
    }
    const SdkTarget& target = android->second;
    const fs::path file = sdk.folder / kSdkLockFile;
    const std::string run = "; run slash-cli sdk lock";
    auto text = platform::read_file(file.string());
    if (!text) {
        return make_error(ErrorCode::NotFound, sdk.location + " has no " +
                                                   std::string(kSdkLockFile) +
                                                   " pinning its Maven dependencies" + run);
    }
    auto parsed = json::parse(text.value());
    const json::Value* maven =
        parsed && parsed.value().is_object() ? parsed.value().find("maven") : nullptr;
    if (maven == nullptr) {
        return make_error(ErrorCode::InvalidArgument, std::string(kSdkLockFile) + " of " +
                                                          sdk.location + " is not a lock file" +
                                                          run);
    }
    auto lock = parse_maven_lock(*maven);
    if (!lock) {
        return make_error(ErrorCode::InvalidArgument, std::string(kSdkLockFile) + " of " +
                                                          sdk.location + ": " +
                                                          lock.error().message + run);
    }
    if (lock.value().dependencies != target.maven ||
        lock.value().repositories != maven_repositories(target.maven_repositories)) {
        return make_error(ErrorCode::Conflict,
                          std::string(kSdkLockFile) + " of " + sdk.location +
                              " was made for other Maven dependencies or repositories" + run);
    }
    return lock;
}

Result<json::Value> lock_sdks(const LockSdksOptions& options) {
    std::error_code error;
    const fs::path project = fs::absolute(options.project, error).lexically_normal();
    auto loaded = runtime::load_project(project.string());
    if (!loaded) {
        return loaded.error();
    }
    std::vector<SdkProblem> problems;
    const std::vector<Sdk> sdks = load_project_sdks(project, loaded.value().sdks, problems);
    if (!problems.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          problems.front().sdk + ": " + problems.front().message);
    }
    if (!options.name.empty() && std::ranges::find(sdks, options.name, &Sdk::name) == sdks.end()) {
        std::vector<std::string_view> names;
        names.reserve(sdks.size());
        for (const Sdk& sdk : sdks) {
            names.emplace_back(sdk.name);
        }
        std::string message =
            "no package in project.json's \"sdks\" is called \"" + options.name + "\"";
        if (const std::string_view hint = closest_match(options.name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, message);
    }
    json::Value out;
    out["sdks"] = json::Array{};
    for (const Sdk& sdk : sdks) {
        const auto android = sdk.platforms.find("android");
        if ((!options.name.empty() && sdk.name != options.name) || android == sdk.platforms.end() ||
            android->second.maven.empty()) {
            continue;
        }
        MavenOptions maven;
        maven.repositories = android->second.maven_repositories;
        maven.cache = maven_cache();
        maven.log_path = options.log_path;
        auto lock = resolve_maven(android->second.maven, maven);
        if (!lock) {
            return make_error(lock.error().code, sdk.location + ": " + lock.error().message);
        }
        json::Value document;
        document["format"] = 1;
        document["maven"] = maven_lock_json(lock.value());
        const std::string text = json::write(document) + "\n";
        const fs::path file = sdk.folder / kSdkLockFile;
        auto existing = platform::read_file(file.string());
        const bool changed = !existing || existing.value() != text;
        if (changed && !options.dry_run) {
            if (auto written = platform::write_file(file.string(), text); !written) {
                return written.error();
            }
        }
        json::Value item;
        item["name"] = sdk.name;
        item["lock"] = (fs::path(sdk.location) / kSdkLockFile).generic_string();
        item["artifacts"] = lock.value().artifacts.size();
        item["changed"] = changed;
        out["sdks"].push_back(std::move(item));
    }
    if (options.dry_run) {
        out["dry_run"] = true;
    }
    return out;
}

bool is_shared_library(const fs::path& path) {
    std::error_code error;
    if (fs::is_directory(path, error)) {
        if (path.extension() != ".framework") {
            return false;
        }
        // macOS frameworks keep their binary under Versions/ with a link at the top; iOS ones are
        // flat.
        const fs::path binary = path / path.stem();
        return fs::exists(binary, error) && shared_at(binary, 0, 0);
    }
    if (fs::is_regular_file(path, error)) {
        return shared_at(path, 0, 0);
    }
    const std::string name = path.filename().string();
    return name.ends_with(".so") || name.find(".so.") != std::string::npos ||
           name.ends_with(".dylib") || name.ends_with(".dll");
}

} // namespace slash::exporter
