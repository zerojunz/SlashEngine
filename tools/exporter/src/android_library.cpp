#include "slash/exporter/android_library.hpp"

#include "steps.hpp"

#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <system_error>
#include <utility>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::display;

constexpr std::string_view kToolsPrefix = "tools:";
constexpr std::string_view kApplicationId = "${applicationId}";

// What tells an element apart from its siblings: its tag and android:name (or a feature's GL
// version). "" for elements matched by their whole text instead, such as <intent-filter>.
std::string element_key(const XmlElement& element) {
    if (const std::string* name = element.attribute("android:name")) {
        return element.name + "|" + *name;
    }
    if (const std::string* gl = element.attribute("android:glEsVersion")) {
        return element.name + "|" + *gl;
    }
    return {};
}

bool same_element(const XmlElement& a, const XmlElement& b) {
    const std::string key = element_key(a);
    if (!key.empty() || !element_key(b).empty()) {
        return key == element_key(b);
    }
    return a.name == b.name && write_xml(a) == write_xml(b);
}

std::string tools_node(const XmlElement& element) {
    const std::string* node = element.attribute("tools:node");
    return node == nullptr ? std::string() : *node;
}

// The element without tools: attributes, here and below.
XmlElement without_tools(XmlElement element) {
    std::erase_if(element.attributes, [](const std::pair<std::string, std::string>& attribute) {
        return attribute.first.starts_with(kToolsPrefix) || attribute.first == "xmlns:tools";
    });
    std::erase_if(element.children,
                  [](const XmlElement& child) { return tools_node(child) == "remove"; });
    for (XmlElement& child : element.children) {
        child = without_tools(std::move(child));
    }
    return element;
}

// Replaces ${applicationId} here and below; any other placeholder is Gradle's to fill and an
// error here.
Result<void> fill_placeholders(XmlElement& element, std::string_view application_id,
                               const std::string& library) {
    const auto fill = [&](std::string& text, std::string_view where) -> Result<void> {
        std::size_t at = 0;
        while ((at = text.find(kApplicationId, at)) != std::string::npos) {
            text.replace(at, kApplicationId.size(), application_id);
            at += application_id.size();
        }
        if (const std::size_t start = text.find("${"); start != std::string::npos) {
            const std::size_t end = text.find('}', start);
            return make_error(ErrorCode::InvalidArgument,
                              library + ": " + std::string(where) + " in <" + element.name +
                                  "> uses " + text.substr(start, end - start + 1) +
                                  ", a Gradle manifest placeholder; exports fill in only "
                                  "${applicationId}");
        }
        return {};
    };
    for (auto& [name, value] : element.attributes) {
        if (auto filled = fill(value, name); !filled) {
            return filled;
        }
    }
    if (auto filled = fill(element.text, "the text"); !filled) {
        return filled;
    }
    for (XmlElement& child : element.children) {
        if (auto filled = fill_placeholders(child, application_id, library); !filled) {
            return filled;
        }
    }
    return {};
}

void merge_children(XmlElement& into, const XmlElement& from,
                    std::optional<std::size_t> insert_before = std::nullopt);

// Merges from into into, an element with the same key: into keeps its attributes, gains the
// ones it lacks, and its children merge.
void merge_element(XmlElement& into, const XmlElement& from) {
    for (const auto& [name, value] : from.attributes) {
        if (!name.starts_with(kToolsPrefix) && name != "xmlns:tools" &&
            into.attribute(name) == nullptr) {
            into.attributes.emplace_back(name, value);
        }
    }
    if (into.text.empty()) {
        into.text = from.text;
    }
    merge_children(into, from);
}

void merge_children(XmlElement& into, const XmlElement& from,
                    std::optional<std::size_t> insert_before) {
    for (const XmlElement& child : from.children) {
        const std::string node = tools_node(child);
        if (node == "removeAll") {
            std::erase_if(into.children,
                          [&](const XmlElement& existing) { return existing.name == child.name; });
            continue;
        }
        const auto existing = std::ranges::find_if(into.children, [&](const XmlElement& candidate) {
            return same_element(candidate, child);
        });
        if (node == "remove") {
            if (existing != into.children.end()) {
                into.children.erase(existing);
            }
            continue;
        }
        if (existing != into.children.end()) {
            if (node == "replace") {
                *existing = without_tools(child);
            } else {
                merge_element(*existing, child);
            }
            continue;
        }
        if (insert_before && *insert_before <= into.children.size()) {
            into.children.insert(into.children.begin() +
                                     static_cast<std::ptrdiff_t>(*insert_before),
                                 without_tools(child));
            ++*insert_before;
        } else {
            into.children.push_back(without_tools(child));
        }
    }
}

// A file name made of the library's name: "androidx.core:core:1.13.1" -> androidx.core-core-1.13.1.
std::string file_stem(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out += safe ? c : '-';
    }
    return out;
}

// Copies from to to unless to exists; folders are copied file by file under the same rule.
// error is set only by a failed copy, never cleared: probes use their own, since asking about a
// file that isn't there reports an error too.
void copy_new(const fs::path& from, const fs::path& to, std::error_code& error) {
    std::error_code probe;
    if (fs::is_directory(from, probe)) {
        for (auto it = fs::recursive_directory_iterator(from, probe);
             !probe && it != fs::recursive_directory_iterator(); it.increment(probe)) {
            if (std::error_code kind; it->is_regular_file(kind)) {
                copy_new(it->path(), to / it->path().lexically_relative(from), error);
            }
        }
        if (probe) {
            error = probe;
        }
        return;
    }
    if (fs::exists(to, probe)) {
        return;
    }
    std::error_code copy;
    fs::create_directories(to.parent_path(), copy);
    if (!copy) {
        fs::copy_file(from, to, copy);
    }
    if (copy) {
        error = copy;
    }
}

bool is_file(const fs::path& path) {
    std::error_code probe;
    return fs::is_regular_file(path, probe);
}

bool is_folder(const fs::path& path) {
    std::error_code probe;
    return fs::is_directory(path, probe);
}

bool has_files(const fs::path& folder) {
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error)) {
            return true;
        }
    }
    return false;
}

std::vector<fs::path> files_with_extension(const fs::path& folder, std::string_view extension) {
    std::vector<fs::path> out;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == extension) {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

} // namespace

Result<XmlElement> merge_android_manifests(XmlElement app,
                                           const std::vector<LibraryManifest>& libraries,
                                           std::string_view application_id, int min_sdk) {
    if (app.name != "manifest") {
        return make_error(ErrorCode::InvalidArgument, "the game's AndroidManifest.xml has no "
                                                      "<manifest>");
    }
    const auto application_index = [&]() -> std::size_t {
        const auto found = std::ranges::find(app.children, "application", &XmlElement::name);
        if (found == app.children.end()) {
            app.children.push_back(XmlElement{"application", {}, {}, {}});
            return app.children.size() - 1;
        }
        return static_cast<std::size_t>(found - app.children.begin());
    };
    (void)application_index();
    for (const LibraryManifest& library : libraries) {
        XmlElement manifest = library.manifest;
        if (manifest.name != "manifest") {
            return make_error(ErrorCode::InvalidArgument,
                              library.library + ": AndroidManifest.xml has no <manifest>");
        }
        if (auto filled = fill_placeholders(manifest, application_id, library.library); !filled) {
            return filled.error();
        }
        if (const XmlElement* uses_sdk = manifest.child("uses-sdk")) {
            if (const std::string* level = uses_sdk->attribute("android:minSdkVersion")) {
                int value = 0;
                const auto [end, error] =
                    std::from_chars(level->data(), level->data() + level->size(), value);
                if (error != std::errc() || end != level->data() + level->size() ||
                    value > min_sdk) {
                    return make_error(ErrorCode::InvalidArgument,
                                      library.library + " needs Android API level " + *level +
                                          " (its minSdkVersion); games run on API level " +
                                          std::to_string(min_sdk) + " and up");
                }
            }
        }
        for (const auto& [name, value] : manifest.attributes) {
            if (name.starts_with("xmlns:") && name != "xmlns:tools" &&
                app.attribute(name) == nullptr) {
                app.attributes.emplace_back(name, value);
            }
        }
        XmlElement top;
        top.name = "manifest";
        for (const XmlElement& child : manifest.children) {
            if (child.name == "uses-sdk") {
                continue;
            }
            if (child.name == "application") {
                merge_element(app.children[application_index()], child);
            } else {
                top.children.push_back(child);
            }
        }
        // Permissions, features and queries go before <application>, as Android Studio writes
        // them.
        merge_children(app, top, application_index());
    }
    return app;
}

Result<AndroidResources> add_android_libraries(const std::vector<AndroidLibrary>& libraries,
                                               const fs::path& tree, const fs::path& work,
                                               const std::vector<std::string>& abis,
                                               std::string_view application_id, int min_sdk,
                                               const std::string& log_path) {
    AndroidResources out;
    if (libraries.empty()) {
        return out;
    }
    std::error_code error;
    std::vector<LibraryManifest> manifests;
    for (std::size_t i = 0; i < libraries.size(); ++i) {
        const AndroidLibrary& library = libraries[i];
        const std::string stem = file_stem(library.name);
        const fs::path source = fs::absolute(library.path, error);
        fs::path root;
        if (is_folder(source)) {
            root = source;
        } else if (source.extension() == ".jar") {
            copy_new(source, tree / "jars" / (stem + ".jar"), error);
            if (error) {
                return make_error(ErrorCode::PlatformFailure, library.name + ": could not copy " +
                                                                  display(source) + ": " +
                                                                  error.message());
            }
            continue;
        } else if (source.extension() == ".aar") {
            root = fs::absolute(work / (std::to_string(i) + "-" + stem), error);
            fs::remove_all(root, error);
            fs::create_directories(root, error);
            platform::ProcessOptions process;
            process.working_directory = root.string();
            process.log_path = log_path;
            const auto unpacked =
                platform::run_process({"cmake", "-E", "tar", "xf", source.string()}, process);
            if (!unpacked || unpacked.value().exit_code != 0) {
                return make_error(ErrorCode::PlatformFailure,
                                  library.name + ": could not unpack " + display(source));
            }
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              library.name +
                                  ": expected an .aar or .jar file or an Android "
                                  "library folder, not " +
                                  display(source));
        }

        error.clear();
        auto text = platform::read_file((root / "AndroidManifest.xml").string());
        if (!text) {
            return make_error(ErrorCode::NotFound,
                              library.name + " has no AndroidManifest.xml at its root");
        }
        auto manifest = parse_xml(text.value());
        if (!manifest) {
            return make_error(ErrorCode::InvalidArgument,
                              library.name + ": AndroidManifest.xml: " + manifest.error().message);
        }
        const std::string* package = manifest.value().attribute("package");

        if (is_file(root / "classes.jar")) {
            copy_new(root / "classes.jar", tree / "jars" / (stem + ".jar"), error);
        }
        for (const fs::path& jar : files_with_extension(root / "libs", ".jar")) {
            copy_new(jar, tree / "jars" / (stem + "-" + jar.stem().string() + ".jar"), error);
        }
        for (const std::string& abi : abis) {
            for (const char* folder : {"jni", "libs"}) {
                for (const fs::path& library_file :
                     files_with_extension(root / folder / abi, ".so")) {
                    copy_new(library_file, tree / "lib" / abi / library_file.filename(), error);
                }
            }
        }
        if (is_folder(root / "assets")) {
            copy_new(root / "assets", tree / "assets", error);
        }
        if (error) {
            return make_error(ErrorCode::PlatformFailure,
                              library.name + ": could not copy its files: " + error.message());
        }
        // Older libraries' R.txt lists the resources of what they depend on, with no res folder.
        const bool resources = is_folder(root / "res") && has_files(root / "res");
        const bool symbols = is_file(root / "R.txt") && fs::file_size(root / "R.txt", error) > 0;
        error.clear();
        if (resources || symbols) {
            if (package == nullptr || package->empty()) {
                return make_error(ErrorCode::InvalidArgument,
                                  library.name + " has resources but its AndroidManifest.xml "
                                                 "names no package");
            }
            if (*package != application_id &&
                std::ranges::find(out.packages, *package) == out.packages.end()) {
                out.packages.push_back(*package);
            }
        }
        if (resources) {
            out.folders.push_back(root / "res");
        }
        manifests.push_back({library.name, std::move(manifest).value()});
    }

    const fs::path manifest_path = tree / "AndroidManifest.xml";
    auto app_text = platform::read_file(manifest_path.string());
    if (!app_text) {
        return app_text.error();
    }
    auto app = parse_xml(app_text.value());
    if (!app) {
        return make_error(ErrorCode::InvalidArgument,
                          "the game's AndroidManifest.xml: " + app.error().message);
    }
    auto merged =
        merge_android_manifests(std::move(app).value(), manifests, application_id, min_sdk);
    if (!merged) {
        return merged.error();
    }
    if (auto written = platform::write_file(manifest_path.string(), write_xml(merged.value()));
        !written) {
        return written.error();
    }
    return out;
}

} // namespace slash::exporter
