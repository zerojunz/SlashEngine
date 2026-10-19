#include "slash/importer/unity_build.hpp"

#include "common.hpp"
#include "compression.hpp"

#include "slash/core/log.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <httplib.h>
#include <map>
#include <system_error>
#include <thread>
#include <utility>

namespace slash::importer::unity {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kWebDataMagic{"UnityWebData1.0\0", 16};
constexpr std::string_view kBrotliComment = "UnityWeb Compressed Content (brotli)";
constexpr std::string_view kListening = "Now listening on: http://";
// How long loading or exporting a build may take.
constexpr auto kRipTimeout = std::chrono::hours(4);

Error failure(std::string message) {
    return Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)};
}

// AssetRipper failed, rather than the build.
Error ripper_failure(std::string message) {
    return Error{.code = ErrorCode::PlatformFailure, .message = std::move(message)};
}

std::uint32_t big_u32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
    }
    return value;
}

std::uint64_t little(std::string_view bytes, std::size_t at, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = size; i-- > 0;) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
    }
    return value;
}

// The text up to a NUL at at, or nullopt if there is none within limit bytes.
std::optional<std::string_view> c_string(std::string_view bytes, std::size_t at,
                                         std::size_t limit = 64) {
    if (at >= bytes.size()) {
        return std::nullopt;
    }
    const std::size_t end = bytes.find('\0', at);
    if (end == std::string_view::npos || end - at > limit) {
        return std::nullopt;
    }
    return bytes.substr(at, end - at);
}

// Whether text looks like a Unity version, such as 2021.3.5f1 or 5.6.7f1.
bool unity_version(std::string_view text) {
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return false;
    }
    return std::ranges::count(text, '.') >= 2 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.';
           });
}

// The first size bytes of a file, or fewer if it is shorter.
std::string read_head(const fs::path& path, std::size_t size) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes(size, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return bytes;
}

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool is_folder(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

// The entries of a folder, sorted, or none if it can't be read.
std::vector<fs::path> entries(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        out.push_back(it->path());
    }
    std::ranges::sort(out);
    return out;
}

// The names in a zip file's central directory, or nullopt if it isn't a zip file.
std::optional<std::vector<std::string>> zip_names(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const auto size = static_cast<std::uint64_t>(in.tellg());
    const std::uint64_t tail_size = std::min<std::uint64_t>(size, 22U + 0xFFFFU);
    std::string tail(tail_size, '\0');
    in.seekg(static_cast<std::streamoff>(size - tail_size));
    in.read(tail.data(), static_cast<std::streamsize>(tail_size));
    const std::size_t end = tail.rfind(std::string_view("PK\x05\x06", 4));
    if (!in || end == std::string::npos || end + 22 > tail.size()) {
        return std::nullopt;
    }
    std::uint64_t directory_size = little(tail, end + 12, 4);
    std::uint64_t directory_at = little(tail, end + 16, 4);
    if ((directory_size == 0xFFFFFFFFU || directory_at == 0xFFFFFFFFU) && end >= 20 &&
        tail.compare(end - 20, 4, std::string_view("PK\x06\x07", 4)) == 0) {
        // ZIP64: the locator names the ZIP64 end of central directory record.
        std::string record(56, '\0');
        in.seekg(static_cast<std::streamoff>(little(tail, end - 20 + 8, 8)));
        in.read(record.data(), static_cast<std::streamsize>(record.size()));
        if (!in || record.compare(0, 4, std::string_view("PK\x06\x06", 4)) != 0) {
            return std::nullopt;
        }
        directory_size = little(record, 40, 8);
        directory_at = little(record, 48, 8);
    }
    if (directory_at > size || directory_size > size - directory_at) {
        return std::nullopt;
    }
    std::string directory(directory_size, '\0');
    in.seekg(static_cast<std::streamoff>(directory_at));
    in.read(directory.data(), static_cast<std::streamsize>(directory_size));
    if (!in) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    std::size_t at = 0;
    while (at + 46 <= directory.size() &&
           directory.compare(at, 4, std::string_view("PK\x01\x02", 4)) == 0) {
        const std::size_t name_size = little(directory, at + 28, 2);
        const std::size_t skip =
            name_size + little(directory, at + 30, 2) + little(directory, at + 32, 2);
        if (at + 46 + name_size > directory.size()) {
            break;
        }
        names.emplace_back(directory.substr(at + 46, name_size));
        at += 46 + skip;
    }
    return names;
}

bool is_data_folder(const fs::path& folder) {
    return is_file(folder / "globalgamemanagers") || is_file(folder / "data.unity3d") ||
           is_file(folder / "mainData");
}

// mono or il2cpp, by what a data folder and the build around it hold.
std::string scripting_backend(const fs::path& data, const fs::path& root) {
    if (is_folder(data / "il2cpp_data") ||
        is_file(data / "Managed" / "Metadata" / "global-metadata.dat") ||
        is_file(root / "GameAssembly.dll") || is_file(root / "GameAssembly.so") ||
        is_file(root / "Contents" / "Frameworks" / "GameAssembly.dylib")) {
        return "il2cpp";
    }
    if (is_folder(data / "Managed")) {
        return "mono";
    }
    return {};
}

Build from_data(const fs::path& data) {
    Build build;
    build.data = data;
    build.root = data;
    const std::string path = data.generic_string();
    const fs::path parent = data.parent_path();
    if (path.ends_with("Contents/Resources/Data")) {
        build.platform = "macos";
        build.root = parent.parent_path().parent_path();
    } else if (path.ends_with("assets/bin/Data")) {
        build.platform = "android";
        build.root = parent.parent_path().parent_path();
    } else if (data.filename() == "Data" && parent.extension() == ".app") {
        build.platform = "ios";
        build.root = parent;
    } else if (data.filename().string().ends_with("_Data")) {
        build.root = parent;
        build.platform = "linux";
        if (is_file(parent / "UnityPlayer.dll") ||
            std::ranges::any_of(entries(parent), [](const fs::path& entry) {
                return lowercase(entry.extension().string()) == ".exe";
            })) {
            build.platform = "windows";
        }
    }
    build.backend = build.platform == "ios" ? "il2cpp" : scripting_backend(data, build.root);
    return build;
}

// The length of a web build file's compression suffix (.gz, .br or .unityweb), or 0.
std::size_t compression_suffix(std::string_view name) {
    for (const std::string_view suffix : {".gz", ".br", ".unityweb"}) {
        if (name.ends_with(suffix)) {
            return suffix.size();
        }
    }
    return 0;
}

bool is_web_data_name(std::string_view name) {
    name.remove_suffix(compression_suffix(name));
    return name.ends_with(".data") || name.ends_with(".datagz");
}

// A web build whose files are in folder (Build/, or Release/ before Unity 5.6).
std::optional<Build> web_build(const fs::path& folder) {
    Build build;
    for (const fs::path& entry : entries(folder)) {
        if (!is_file(entry)) {
            continue;
        }
        if (build.web_data.empty() && is_web_data_name(entry.filename().string())) {
            build.web_data = entry;
        } else {
            build.web_files.push_back(entry);
        }
    }
    if (build.web_data.empty()) {
        return std::nullopt;
    }
    build.root = folder.parent_path();
    build.platform = "web";
    build.backend = "il2cpp";
    return build;
}

std::optional<Build> find_in_folder(const fs::path& folder, int depth) {
    if (is_data_folder(folder)) {
        return from_data(folder);
    }
    for (const fs::path& data :
         {folder / "Contents" / "Resources" / "Data", folder / "assets" / "bin" / "Data"}) {
        if (is_data_folder(data)) {
            return from_data(data);
        }
    }
    if (folder.extension() == ".app" && is_data_folder(folder / "Data")) {
        return from_data(folder / "Data");
    }
    const std::string name = folder.filename().string();
    if (name == "Build" || name == "Release") {
        if (auto build = web_build(folder)) {
            return build;
        }
    }
    for (const std::string_view sub : {"Build", "Release"}) {
        if (auto build = web_build(folder / sub)) {
            return build;
        }
    }
    const std::vector<fs::path> children = entries(folder);
    for (const fs::path& child : children) {
        if (child.filename().string().ends_with("_Data") && is_data_folder(child)) {
            return from_data(child);
        }
    }
    if (depth > 0) {
        // A folder holding the game's .app, or an unzipped .ipa's Payload/Game.app.
        for (const fs::path& child : children) {
            if (child.extension() == ".app" || child.filename() == "Payload") {
                if (auto build = find_in_folder(child, depth - 1)) {
                    return build;
                }
            }
        }
    }
    return std::nullopt;
}

// An .apk or .ipa file made with Unity, by the player data its zip holds.
std::optional<Build> package_build(const fs::path& file, std::string_view extension) {
    const auto names = zip_names(file);
    if (!names) {
        return std::nullopt;
    }
    const bool apk = extension == ".apk";
    const bool unity = std::ranges::any_of(*names, [&](const std::string& name) {
        if (apk) {
            return name.starts_with("assets/bin/Data/");
        }
        const std::size_t app = name.find(".app/Data/");
        return name.starts_with("Payload/") && app != std::string::npos;
    });
    if (!unity) {
        return std::nullopt;
    }
    Build build;
    build.root = file;
    build.platform = apk ? "android" : "ios";
    build.package = true;
    // iOS builds are always IL2CPP.
    if (!apk || std::ranges::any_of(*names, [](const std::string& name) {
            return name.ends_with("/libil2cpp.so") ||
                   name.ends_with("Metadata/global-metadata.dat");
        })) {
        build.backend = "il2cpp";
    } else if (std::ranges::any_of(*names, [](const std::string& name) {
                   return name.starts_with("assets/bin/Data/Managed/");
               })) {
        build.backend = "mono";
    }
    return build;
}

// Whether bytes start with gzip's or Unity's brotli's signature.
bool gzip_bytes(std::string_view bytes) {
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1F &&
           static_cast<unsigned char>(bytes[1]) == 0x8B;
}

bool unity_brotli(std::string_view bytes) {
    return bytes.substr(0, 64).find(kBrotliComment) != std::string_view::npos;
}

// A web build's file decompressed, by its contents, then its name's suffix.
Result<std::string> decompress_file(std::string_view name, std::string_view bytes) {
    if (gzip_bytes(bytes)) {
        return gunzip(bytes);
    }
    if (unity_brotli(bytes) || name.ends_with(".br")) {
        return unbrotli(bytes);
    }
    return std::string(bytes);
}

// The first file in a build that holds a UnityFS bundle marked encrypted, by a name to report.
std::optional<std::string> encrypted_file(const Build& build,
                                          const std::vector<WebFile>& web_files) {
    for (const WebFile& file : web_files) {
        if (encrypted_bundle(file.data.substr(0, 256))) {
            return file.name;
        }
    }
    if (build.data.empty()) {
        return std::nullopt;
    }
    std::vector<fs::path> candidates{build.data / "data.unity3d"};
    const fs::path streaming = build.data / "StreamingAssets";
    if (auto files = platform::list_files(streaming.string())) {
        for (const std::string& file : files.value()) {
            candidates.push_back(streaming / file);
        }
    }
    // Android keeps StreamingAssets in assets/ beside bin/Data.
    if (build.platform == "android") {
        const fs::path assets = build.data.parent_path().parent_path();
        if (auto files = platform::list_files(
                assets.string(), [](std::string_view folder) { return folder != "bin/"; })) {
            for (const std::string& file : files.value()) {
                candidates.push_back(assets / file);
            }
        }
    }
    for (const fs::path& candidate : candidates) {
        if (is_file(candidate) && encrypted_bundle(read_head(candidate, 256))) {
            return candidate.lexically_relative(build.root).generic_string();
        }
    }
    return std::nullopt;
}

Result<std::string> find_assetripper(const UnpackOptions& options) {
    if (!options.assetripper.empty()) {
        return options.assetripper;
    }
    if (std::string named = platform::environment_variable("SLASH_ASSETRIPPER"); !named.empty()) {
        return named;
    }
    const auto toolchain = exporter::assetripper();
    if (!toolchain) {
        return failure("AssetRipper has no build for this computer; set SLASH_ASSETRIPPER to its "
                       "program");
    }
    const fs::path dir = exporter::toolchains_dir();
    auto folder = exporter::installed_toolchain(*toolchain, dir);
    if (!folder) {
        log::info("installing AssetRipper", {{"version", toolchain->version}});
        auto installed =
            exporter::install_toolchain(*toolchain, dir, (options.work / "install.log").string());
        if (!installed) {
            return installed.error();
        }
        folder = std::move(installed.value());
    }
    return (*folder / exporter::assetripper_program()).string();
}

// The version AssetRipper's --version prints ("AssetRipper.GUI.Web 2.0.0+1ac666f"), or "".
std::string assetripper_version(const std::string& program) {
    auto run = platform::run_process({program, "--version"});
    if (!run || run.value().exit_code != 0) {
        return {};
    }
    const std::string& text = run.value().tail;
    const std::size_t name = text.find("AssetRipper");
    const std::size_t space = text.find(' ', name);
    if (name == std::string::npos || space == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_first_of("+ \r\n", space + 1);
    return text.substr(space + 1, end == std::string::npos ? end : end - space - 1);
}

// The last lines a program printed, for an error message.
std::string last_lines(const std::string& output, std::size_t count = 12) {
    std::size_t at = output.size();
    while (at > 0 && (output[at - 1] == '\n' || output[at - 1] == '\r')) {
        --at;
    }
    const std::size_t end = at;
    for (std::size_t lines = 0; at > 0 && lines < count;) {
        if (output[--at] == '\n') {
            ++lines;
        }
    }
    return output.substr(at == 0 ? 0 : at + 1, end - (at == 0 ? 0 : at + 1));
}

// AssetRipper running headless, serving its web interface on this machine.
class Ripper {
public:
    static Result<Ripper> start(const std::string& program, const fs::path& work,
                                int start_timeout) {
        platform::ProcessOptions options;
        options.working_directory = work.string();
        options.log_path = (work / "assetripper-output.log").string();
        auto process = platform::start_process(
            {program, "--headless", "--log-path", (work / "assetripper.log").string()}, options);
        if (!process) {
            return ripper_failure("could not start AssetRipper (" + program +
                                  "): " + process.error().message);
        }
        Ripper ripper(std::move(process.value()));
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(start_timeout);
        while (true) {
            const std::string output = ripper.process_->output();
            if (const std::size_t at = output.find(kListening); at != std::string::npos) {
                const std::size_t host = at + kListening.size();
                const std::size_t colon = output.find(':', host);
                const std::size_t end = output.find_first_not_of("0123456789", colon + 1);
                if (colon != std::string::npos && end != std::string::npos && end > colon + 1) {
                    ripper.host_ = output.substr(host, colon - host);
                    ripper.port_ = static_cast<int>(
                        parse_int(output.substr(colon + 1, end - colon - 1)).value_or(0));
                    return ripper;
                }
            }
            if (!ripper.process_->running()) {
                return ripper_failure("AssetRipper stopped before it started serving:\n" +
                                      last_lines(ripper.process_->output()));
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return ripper_failure("AssetRipper didn't start serving within " +
                                      std::to_string(start_timeout) + " seconds:\n" +
                                      last_lines(ripper.process_->output()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // POSTs a form with one Path field, as AssetRipper's pages do.
    Result<void> post(const std::string& path, const fs::path& value) {
        httplib::Client client(host_, port_);
        client.set_connection_timeout(std::chrono::seconds(10));
        client.set_read_timeout(kRipTimeout);
        client.set_write_timeout(std::chrono::seconds(60));
        const httplib::Params form{{"Path", value.string()}};
        const httplib::Result result = client.Post(path, form);
        if (!result) {
            return ripper_failure("AssetRipper didn't answer " + path + ": " +
                                  httplib::to_string(result.error()) + "\n" +
                                  last_lines(process_->output()));
        }
        if (result->status >= 400) {
            return ripper_failure("AssetRipper failed " + path + " (HTTP " +
                                  std::to_string(result->status) + "):\n" +
                                  last_lines(process_->output()));
        }
        return {};
    }

    [[nodiscard]] std::string output() const { return process_->output(); }

    void stop() { process_->stop(); }

private:
    explicit Ripper(std::unique_ptr<platform::ChildProcess> process)
        : process_(std::move(process)) {}

    std::unique_ptr<platform::ChildProcess> process_;
    std::string host_;
    int port_ = 0;
};

// Whether an assembly is one of Unity's own: its engine modules and packages.
bool unity_assembly(std::string_view name) {
    return name == "UnityEngine" || name.starts_with("UnityEngine.") || name.starts_with("Unity.");
}

// Files Unity writes as YAML that can name scripts.
bool yaml_file(std::string_view extension) {
    static constexpr std::array<std::string_view, 12> kExtensions{
        ".unity",  ".prefab", ".asset", ".controller", ".overridecontroller",
        ".anim",   ".mixer",  ".mat",   ".playable",   ".spriteatlas",
        ".signal", ".brush"};
    return contains(kExtensions, extension);
}

// The GUID in a .meta file's text, or "".
std::string meta_guid(std::string_view meta) {
    const std::size_t at = meta.find("\nguid: ");
    if (at == std::string_view::npos || at + 7 + 32 > meta.size()) {
        return {};
    }
    return std::string(meta.substr(at + 7, 32));
}

// What becomes of a script AssetRipper wrote: package scripts get their package's GUID, Unity's
// other scripts are named in the objects that use them, as Unity 6 names a package script.
struct ScriptFixes {
    std::map<std::string, std::string> guids;       // old GUID: package GUID
    std::map<std::string, std::string> identifiers; // old GUID: "Assembly::Namespace.Class"
};

// A YAML file's text with its script references fixed, or nullopt if nothing changed.
std::optional<std::string> fix_scripts(std::string_view text, const ScriptFixes& fixes) {
    std::string out;
    out.reserve(text.size());
    bool changed = false;
    std::string identifier; // for the current document's m_EditorClassIdentifier
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        end = end == std::string_view::npos ? text.size() : end + 1;
        std::string line(text.substr(at, end - at));
        at = end;
        if (line.starts_with("--- ")) {
            identifier.clear();
        } else if (!identifier.empty() && line.starts_with("  m_EditorClassIdentifier:")) {
            line = "  m_EditorClassIdentifier: " + identifier + "\n";
            identifier.clear();
            changed = true;
        } else {
            for (std::size_t guid = line.find("guid: "); guid != std::string::npos;
                 guid = line.find("guid: ", guid + 6)) {
                if (guid + 6 + 32 > line.size()) {
                    break;
                }
                const std::string old = line.substr(guid + 6, 32);
                if (const auto found = fixes.guids.find(old); found != fixes.guids.end()) {
                    line.replace(guid + 6, 32, found->second);
                    changed = true;
                } else if (const auto named = fixes.identifiers.find(old);
                           named != fixes.identifiers.end() &&
                           line.find("m_Script:") != std::string::npos) {
                    identifier = named->second;
                }
            }
        }
        out += line;
    }
    if (!changed) {
        return std::nullopt;
    }
    return out;
}

// Tidies AssetRipper's project so it converts as one made in Unity would, and counts what it
// holds into report.
Result<void> tidy(const fs::path& project, json::Value& report) {
    const fs::path assets = project / "Assets";
    auto files = platform::list_files(assets.string());
    if (!files) {
        return files.error();
    }
    Problems problems;
    ScriptFixes fixes;
    std::vector<std::string> drop; // Unity's assemblies' script folders
    int scripts = 0;
    int scenes = 0;
    int prefabs = 0;
    int package_scripts = 0;
    for (const std::string& file : files.value()) {
        const std::string extension = extension_of(file);
        scenes += extension == ".unity" ? 1 : 0;
        prefabs += extension == ".prefab" ? 1 : 0;
        if (!file.starts_with("Scripts/") || extension != ".cs") {
            continue;
        }
        std::vector<std::string_view> parts = split_path(file);
        const std::string assembly(parts.at(1));
        const std::string name = file_stem(file);
        parts.erase(parts.begin(), parts.begin() + 2);
        parts.pop_back();
        std::string space;
        for (const std::string_view part : parts) {
            space += (space.empty() ? "" : ".") + std::string(part);
        }
        const bool unity = unity_assembly(assembly);
        if (unity && !contains(drop, assembly)) {
            drop.push_back(assembly);
        }
        auto meta = platform::read_file((assets / (file + ".meta")).string());
        const std::string guid = meta ? meta_guid(meta.value()) : std::string();
        if (const std::string_view known = package_script_guid(space, name);
            !known.empty() && !guid.empty()) {
            fixes.guids[guid] = std::string(known);
            ++package_scripts;
        } else if (unity && !guid.empty()) {
            std::string identifier = assembly + "::";
            if (!space.empty()) {
                identifier += space;
                identifier += '.';
            }
            identifier += name;
            fixes.identifiers[guid] = std::move(identifier);
        } else if (!unity) {
            ++scripts;
            auto text = platform::read_file((assets / file).string());
            if (text && text.value().find("Dummy class.") != std::string::npos) {
                problems.add("script without its code", "Assets/" + file,
                             "the build's code couldn't be read back (IL2CPP builds keep it "
                             "compiled), so only the script's name and the objects using it "
                             "came back");
            }
        }
    }
    if (!fixes.guids.empty() || !fixes.identifiers.empty()) {
        for (const std::string& file : files.value()) {
            if (!yaml_file(extension_of(file))) {
                continue;
            }
            const std::string path = (assets / file).string();
            auto text = platform::read_file(path);
            if (!text) {
                return text.error();
            }
            if (auto fixed = fix_scripts(text.value(), fixes)) {
                if (auto written = platform::write_file(path, *fixed); !written) {
                    return written.error();
                }
            }
        }
    }
    for (const std::string& assembly : drop) {
        std::error_code error;
        fs::remove_all(assets / "Scripts" / assembly, error);
        fs::remove(assets / "Scripts" / (assembly + ".meta"), error);
    }
    report["scenes"] = scenes;
    report["prefabs"] = prefabs;
    report["scripts"] = scripts;
    report["package_scripts"] = package_scripts;
    // The files that came back, besides Unity's metadata and the scripts dropped above.
    report["files"] = static_cast<std::int64_t>(
        std::ranges::count_if(files.value(), [&drop](const std::string& file) {
            return !file.ends_with(".meta") &&
                   std::ranges::none_of(drop, [&file](const std::string& assembly) {
                       return file.starts_with("Scripts/" + assembly + "/");
                   });
        }));
    report["unsupported"] = problems.to_json();
    return {};
}

} // namespace

std::optional<Build> find_build(const fs::path& source) {
    if (is_folder(source)) {
        return find_in_folder(source, 2);
    }
    if (!is_file(source)) {
        return std::nullopt;
    }
    const std::string extension = lowercase(source.extension().string());
    if (extension == ".apk" || extension == ".ipa") {
        return package_build(source, extension);
    }
    // A file in the build: its executable, a file in its data folder, or its web data.
    const fs::path parent = source.parent_path();
    if (auto build = find_in_folder(parent, 0)) {
        return build;
    }
    if (is_data_folder(parent.parent_path())) {
        return from_data(parent.parent_path());
    }
    return std::nullopt;
}

std::string file_unity_version(std::string_view header) {
    for (const std::string_view signature :
         {std::string_view("UnityFS\0", 8), std::string_view("UnityWeb\0", 9),
          std::string_view("UnityRaw\0", 9)}) {
        if (header.starts_with(signature)) {
            const std::size_t at = signature.size() + 4;
            const auto version = c_string(header, at);
            const auto revision =
                version ? c_string(header, at + version->size() + 1) : std::nullopt;
            return revision && unity_version(*revision) ? std::string(*revision) : "";
        }
    }
    constexpr std::size_t kMinimum = 20;
    if (header.size() < kMinimum) {
        return {};
    }
    const std::uint32_t version = big_u32(header, 8);
    if (version < 7 || version > 50) {
        return {};
    }
    std::size_t at = version >= 9 ? 20 : 16;
    if (version >= 22) {
        at += 4 + 8 + 8 + 8;
    }
    const auto text = c_string(header, at);
    return text && unity_version(*text) ? std::string(*text) : "";
}

bool encrypted_bundle(std::string_view header) {
    constexpr std::string_view kSignature{"UnityFS\0", 8};
    if (!header.starts_with(kSignature)) {
        return false;
    }
    std::size_t at = kSignature.size() + 4;
    const auto version = c_string(header, at);
    if (!version) {
        return false;
    }
    at += version->size() + 1;
    const auto revision = c_string(header, at);
    if (!revision) {
        return false;
    }
    at += revision->size() + 1 + 8 + 4 + 4;
    if (at + 4 > header.size()) {
        return false;
    }
    const std::uint32_t flags = big_u32(header, at);
    // Unity's own flags took 0x200 in 2020.3.34, 2021.3.2 and 2022.1.1; UnityCN marks its
    // bundles with 0x200 before then and 0x1400 since.
    std::array<std::int64_t, 3> number{0, 0, 0};
    std::size_t part = 0;
    for (const char c : *revision) {
        if (c == '.') {
            if (++part == number.size()) {
                break;
            }
        } else if (c >= '0' && c <= '9') {
            number.at(part) = number.at(part) * 10 + (c - '0');
        } else {
            break;
        }
    }
    const auto before = [&](std::int64_t minor, std::int64_t patch) {
        return std::pair(number[1], number[2]) < std::pair(minor, patch);
    };
    const bool old = number[0] < 2020 || (number[0] == 2020 && before(3, 34)) ||
                     (number[0] == 2021 && before(3, 2)) || (number[0] == 2022 && before(1, 1));
    return old ? (flags & 0x200U) != 0 : (flags & 0x1400U) != 0;
}

Result<std::string> decompress_web_data(std::string_view bytes) {
    Result<std::string> data = std::string(bytes);
    if (gzip_bytes(bytes)) {
        data = gunzip(bytes);
    } else if (!bytes.starts_with(kWebDataMagic)) {
        data = unbrotli(bytes);
    }
    if (!data || !data.value().starts_with(kWebDataMagic)) {
        return failure("not Unity web data: once decompressed it doesn't start with "
                       "UnityWebData1.0");
    }
    return data;
}

Result<std::vector<WebFile>> read_web_data(std::string_view bytes) {
    if (!bytes.starts_with(kWebDataMagic) || bytes.size() < kWebDataMagic.size() + 4) {
        return failure("not Unity web data: it doesn't start with UnityWebData1.0");
    }
    const std::size_t header = little(bytes, kWebDataMagic.size(), 4);
    if (header > bytes.size()) {
        return failure("Unity web data cut short: its header is longer than the file");
    }
    std::vector<WebFile> files;
    std::size_t at = kWebDataMagic.size() + 4;
    while (at + 12 <= header) {
        const std::size_t offset = little(bytes, at, 4);
        const std::size_t size = little(bytes, at + 4, 4);
        const std::size_t name_size = little(bytes, at + 8, 4);
        at += 12;
        if (at + name_size > header || offset > bytes.size() || size > bytes.size() - offset) {
            return failure("Unity web data cut short: an entry runs past the end of the file");
        }
        files.push_back({std::string(bytes.substr(at, name_size)), bytes.substr(offset, size)});
        at += name_size;
    }
    return files;
}

Result<UnpackedBuild> unpack_build(const Build& build, const UnpackOptions& options) {
    fs::path load = build.root;
    std::string web_data;
    std::vector<WebFile> web_files;
    if (!build.web_data.empty()) {
        auto bytes = platform::read_file(build.web_data.string());
        if (!bytes) {
            return bytes.error();
        }
        auto data = decompress_web_data(bytes.value());
        if (!data) {
            return failure(build.web_data.filename().string() + ": " + data.error().message);
        }
        web_data = std::move(data.value());
        auto files = read_web_data(web_data);
        if (!files) {
            return files.error();
        }
        web_files = std::move(files.value());
    }
    if (const auto encrypted = encrypted_file(build, web_files)) {
        return Error{.code = ErrorCode::PermissionDenied,
                     .message = "the build's asset bundles are encrypted (" + *encrypted +
                                "), which nothing here decrypts"};
    }
    // AssetRipper reads web builds only uncompressed, so they are staged in the work folder.
    if (!build.web_data.empty()) {
        const bool compressed = compression_suffix(build.web_data.filename().string()) != 0 ||
                                std::ranges::any_of(build.web_files, [](const fs::path& file) {
                                    return compression_suffix(file.filename().string()) != 0;
                                });
        if (compressed || !build.web_data.string().ends_with(".data")) {
            load = options.work / "web";
            const fs::path staged = load / "Build";
            if (auto made = platform::create_directories(staged.string()); !made) {
                return made.error();
            }
            std::string name = build.web_data.filename().string();
            name.resize(name.size() - compression_suffix(name));
            if (name.ends_with(".datagz")) {
                name.resize(name.size() - 2);
            }
            if (auto written = platform::write_file((staged / name).string(), web_data); !written) {
                return written.error();
            }
            for (const fs::path& file : build.web_files) {
                std::string file_name = file.filename().string();
                auto bytes = platform::read_file(file.string());
                if (!bytes) {
                    return bytes.error();
                }
                auto contents = decompress_file(file_name, bytes.value());
                if (!contents) {
                    return failure(file_name + ": " + contents.error().message);
                }
                file_name.resize(file_name.size() - compression_suffix(file_name));
                if (auto written =
                        platform::write_file((staged / file_name).string(), contents.value());
                    !written) {
                    return written.error();
                }
            }
        }
    }
    web_files.clear();
    web_data.clear();

    auto program = find_assetripper(options);
    if (!program) {
        return program.error();
    }
    const std::string version = assetripper_version(program.value());
    auto ripper = Ripper::start(program.value(), options.work, options.start_timeout);
    if (!ripper) {
        return ripper.error();
    }
    const fs::path exported = options.work / "export";
    log::info("AssetRipper is loading the build", {{"path", load.generic_string()}});
    if (auto loaded = ripper.value().post(build.package ? "/LoadFile" : "/LoadFolder", load);
        !loaded) {
        return loaded.error();
    }
    log::info("AssetRipper is exporting its Unity project", {{"path", exported.generic_string()}});
    if (auto done = ripper.value().post("/Export/UnityProject", exported); !done) {
        return done.error();
    }
    ripper.value().stop();
    UnpackedBuild unpacked;
    unpacked.project = exported / "ExportedProject";
    if (!is_folder(unpacked.project / "Assets")) {
        return ripper_failure("AssetRipper exported no project from " +
                              build.root.filename().string() + ":\n" +
                              last_lines(ripper.value().output()));
    }
    json::Value& report = unpacked.report;
    report["build"]["platform"] = build.platform;
    report["build"]["backend"] = build.backend;
    auto project_version =
        platform::read_file((unpacked.project / "ProjectSettings" / "ProjectVersion.txt").string());
    if (project_version) {
        const std::string& text = project_version.value();
        constexpr std::string_view kKey = "m_EditorVersion: ";
        if (const std::size_t at = text.find(kKey); at != std::string::npos) {
            const std::size_t end = text.find_first_of("\r\n", at);
            report["build"]["unity"] =
                text.substr(at + kKey.size(),
                            end == std::string::npos ? std::string::npos : end - at - kKey.size());
        }
    }
    report["assetripper"] = version;
    if (auto tidied = tidy(unpacked.project, report); !tidied) {
        return tidied.error();
    }
    return unpacked;
}

} // namespace slash::importer::unity
