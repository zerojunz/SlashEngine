// The game's files for an agent service that can't read this machine's disk: codex_manifest and
// codex_read, after the Godot addon's lib/codex.gd (Autopilot, Apache 2.0). Autopilot's Godot
// adapter keeps only Godot's file types, plus whatever the addon serves under
// .godot/autopilot_codex/, which it puts at the codex's top; so every file here is served under
// that folder: project.json, scenes, prefabs and the other JSON resources, the storyboard, the
// gameplay sources and shaders, translations and notes, and summary.txt, the project in brief.
// Limits are the addon's: 20000 files and 200 MB in a manifest, reads of up to 4 MB.

#include "internal.hpp"

#include "slash/command/session.hpp"
#include "slash/core/sha256.hpp"
#include "slash/core/version.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>

namespace slash::autopilot::detail {

namespace {

constexpr std::size_t kMaxFiles = 20000;
constexpr std::uint64_t kMaxTotal = 200ULL * 1024 * 1024;
constexpr std::int64_t kDefaultRead = 1000000;
constexpr std::int64_t kMaxRead = 4LL * 1024 * 1024;
constexpr std::string_view kSummary = "summary.txt";

// File types served, by extension.
constexpr std::array<std::string_view, 14> kExtensions{".json", ".jsonl", ".cpp",  ".cc", ".c",
                                                       ".hpp",  ".h",     ".hlsl", ".md", ".txt",
                                                       ".csv",  ".po",    ".pot",  ".py"};

// Names Autopilot keeps for its own files at the codex's top, served one folder down instead.
constexpr std::array<std::string_view, 3> kCodexOwn{"README.md", "level.txt", "controls.txt"};

bool served_type(std::string_view path) {
    return std::any_of(kExtensions.begin(), kExtensions.end(),
                       [&](std::string_view ext) { return path.ends_with(ext); });
}

// Folders left out: build output, exports, dependencies and SlashEngine's own work folders (dot
// folders are left out by list_files).
bool skipped_folder(std::string_view folder) {
    std::string_view name = folder;
    if (name.ends_with('/')) {
        name.remove_suffix(1);
    }
    if (const std::size_t slash = name.rfind('/'); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    return name == "build" || name == "node_modules" || name == "export" || name.starts_with('.');
}

// The codex path a project file is served as, and back.
std::string served(std::string_view rel) {
    std::string out(kCodexPrefix);
    if (std::find(kCodexOwn.begin(), kCodexOwn.end(), rel) != kCodexOwn.end()) {
        out += "project/";
    }
    out += rel;
    return out;
}

// The project path a codex path names, "" for one the manifest couldn't list, or "summary.txt".
std::string project_path(std::string_view asked) {
    if (!asked.starts_with(kCodexPrefix)) {
        return "";
    }
    std::string_view rel = asked.substr(kCodexPrefix.size());
    if (rel == kSummary) {
        return std::string(kSummary);
    }
    bool own = false;
    if (rel.starts_with("project/") &&
        std::find(kCodexOwn.begin(), kCodexOwn.end(), rel.substr(8)) != kCodexOwn.end()) {
        rel.remove_prefix(8);
        own = true;
    }
    if (rel.empty() || rel.find('\\') != std::string_view::npos ||
        rel.find(':') != std::string_view::npos || rel.starts_with('/')) {
        return "";
    }
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t end = std::min(rel.find('/', start), rel.size());
        const std::string_view part = rel.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." ||
            (end < rel.size() && skipped_folder(part))) {
            return "";
        }
        start = end + 1;
    }
    if (!served_type(rel)) {
        return "";
    }
    if (!own && std::find(kCodexOwn.begin(), kCodexOwn.end(), rel) != kCodexOwn.end()) {
        return "";
    }
    return std::string(rel);
}

std::vector<std::string> project_files(const std::string& root) {
    Result<std::vector<std::string>> files =
        platform::list_files(root, [](std::string_view folder) { return !skipped_folder(folder); });
    if (!files.ok()) {
        return {};
    }
    std::vector<std::string> out;
    for (std::string& file : files.value()) {
        if (served_type(file)) {
            out.push_back(std::move(file));
        }
    }
    return out;
}

std::string join(const std::string& root, std::string_view rel) {
    if (root.empty() || root == ".") {
        return std::string(rel);
    }
    return root + (root.ends_with('/') ? "" : "/") + std::string(rel);
}

// A binding as the summary writes it: its Unreal name, else the engine's.
std::string binding_text(const input::Event& binding) {
    switch (binding.type) {
    case input::EventType::Key: {
        const input::Key key = binding.key != input::Key::None ? binding.key : binding.physical_key;
        return std::string(input::key_name(key));
    }
    case input::EventType::MouseButton:
        return "mouse " + std::string(input::mouse_button_name(binding.button));
    case input::EventType::JoypadButton:
        return "joypad " + std::string(input::joy_button_name(binding.joy_button));
    case input::EventType::JoypadMotion:
        return "joypad " + std::string(input::joy_axis_name(binding.axis)) +
               (binding.axis_value < 0.0F ? " -" : " +");
    default:
        return std::string(input::type_name(binding.type));
    }
}

void outline_scene(const std::string& root, const std::string& rel, std::string& out) {
    Result<std::string> text = platform::read_file(join(root, rel));
    if (!text.ok()) {
        return;
    }
    Result<json::Value> doc = json::parse(text.value());
    if (!doc.ok()) {
        out += "    (doesn't parse: " + doc.error().message + ")\n";
        return;
    }
    const json::Value* entities = doc.value().find("entities");
    if (entities == nullptr || !entities->is_object()) {
        return;
    }
    if (const json::Value* scene_root = doc.value().find("root");
        scene_root != nullptr && scene_root->is_string()) {
        out += "    root: " + scene_root->as_string() + "\n";
    }
    int count = 0;
    for (const auto& [id, entity] : entities->as_object()) {
        if (++count > 400) {
            out += "    ... (more entities)\n";
            break;
        }
        std::string line = "    " + id;
        if (const json::Value* extends = entity.find("extends");
            extends != nullptr && extends->is_string()) {
            line += " extends " + extends->as_string();
        }
        if (const json::Value* components = entity.find("components");
            components != nullptr && components->is_object() && components->size() > 0) {
            line += " (";
            bool first = true;
            for (const auto& [name, value] : components->as_object()) {
                line += (first ? "" : ", ") + name + (value.is_null() ? " removed" : "");
                first = false;
            }
            line += ")";
        }
        out += line + "\n";
    }
}

} // namespace

std::string codex_summary(command::Session& session) {
    const std::string& root = session.options().project_root;
    std::string out;
    Result<runtime::Project> project = runtime::load_project(root);
    const std::string name = project.ok() ? project.value().name : std::string("(no project.json)");
    out +=
        "SlashEngine project: " + name + " (SlashEngine " + std::string(slash::version()) + ")\n";
    out +=
        "SlashEngine follows Godot's scene model: scenes are .scene.json files of entities by ID, "
        "each with components named and laid out as Godot's nodes (Transform3D, "
        "CharacterBody3D, Camera3D, Button...). Gameplay code is plain C++ in the folders "
        "project.json's gameplay.sources names, registered in SLASH_GAME_MODULE. In this "
        "codex every project file is under its project path; rc's objectPath is an entity "
        "ID and its propertyName \"Component/field\".\n";
    if (project.ok()) {
        out += "Main scene: " + project.value().main_scene + "\n";
        if (!project.value().gameplay_sources.empty()) {
            out += "Gameplay sources:";
            for (const std::string& source : project.value().gameplay_sources) {
                out += " " + source;
            }
            out += "\n";
        }
    }
    out += "Open scene: " + session.scene_path() + "\n\n";
    out += "Input actions (project.json's input):\n";
    if (const input::Map* map = input::map(session.world()); map != nullptr) {
        const input::Map defaults = input::Map::with_defaults();
        for (const auto& [action, entry] : map->actions()) {
            if (const input::Action* fallback = defaults.find(action);
                fallback != nullptr && fallback->events == entry.events) {
                continue;
            }
            std::string line = "  " + action + ":";
            bool first = true;
            for (const input::Event& binding : entry.events) {
                line += (first ? " " : ", ") + binding_text(binding);
                first = false;
            }
            out += line + "\n";
        }
    }
    out += "\nScenes and prefabs (entities: ID, what it extends, its components):\n";
    for (const std::string& file : project_files(root)) {
        if (file.ends_with(".scene.json") || file.ends_with(".prefab.json")) {
            out += "  " + file + "\n";
            outline_scene(root, file, out);
        }
    }
    return out;
}

json::Value codex_manifest(command::Session& session, std::string_view project) {
    const auto started = std::chrono::steady_clock::now();
    const std::string& root = session.options().project_root;
    json::Array files;
    std::uint64_t total = 0;
    bool truncated = false;
    for (const std::string& rel : project_files(root)) {
        if (files.size() >= kMaxFiles) {
            truncated = true;
            break;
        }
        Result<std::string> text = platform::read_file(join(root, rel));
        if (!text.ok()) {
            continue;
        }
        const std::uint64_t size = text.value().size();
        if (total + size > kMaxTotal) {
            truncated = true;
            continue;
        }
        total += size;
        json::Value entry;
        entry["path"] = served(rel);
        entry["size"] = static_cast<std::int64_t>(size);
        entry["hash"] = sha256_hex(text.value());
        files.push_back(std::move(entry));
    }
    const std::string summary = codex_summary(session);
    json::Value entry;
    entry["path"] = std::string(kCodexPrefix) + std::string(kSummary);
    entry["size"] = static_cast<std::int64_t>(summary.size());
    entry["hash"] = sha256_hex(summary);
    files.push_back(std::move(entry));
    json::Value out;
    out["project"] = project;
    out["files"] = std::move(files);
    out["truncated"] = truncated;
    json::Value exported;
    exported["ran"] = true;
    exported["seconds"] =
        r2(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    exported["exit_code"] = 0;
    out["export"] = std::move(exported);
    return out;
}

json::Value codex_read(command::Session& session, const json::Value& args) {
    const std::string& root = session.options().project_root;
    std::int64_t max_bytes = kDefaultRead;
    if (const json::Value* given = args.find("max_bytes"); given != nullptr && given->is_number()) {
        max_bytes =
            std::clamp<std::int64_t>(static_cast<std::int64_t>(given->as_double()), 1, kMaxRead);
    }
    json::Value files = json::Object{};
    json::Array missing;
    json::Array too_big;
    std::int64_t used = 0;
    if (const json::Value* paths = args.find("paths"); paths != nullptr && paths->is_array()) {
        for (const json::Value& path : paths->as_array()) {
            if (!path.is_string()) {
                continue;
            }
            const std::string& asked = path.as_string();
            const std::string rel = project_path(asked);
            std::string text;
            if (rel.empty()) {
                missing.emplace_back(asked);
                continue;
            }
            if (rel == kSummary) {
                text = codex_summary(session);
            } else {
                Result<std::string> read = platform::read_file(join(root, rel));
                if (!read.ok()) {
                    missing.emplace_back(asked);
                    continue;
                }
                text = std::move(read.value());
            }
            const auto size = static_cast<std::int64_t>(text.size());
            if (size > max_bytes) {
                too_big.emplace_back(asked);
                continue;
            }
            if (used + size > max_bytes) {
                break; // this and the rest go unanswered: the agent service asks again
            }
            files[asked] = std::move(text);
            used += size;
        }
    }
    json::Value out;
    out["files"] = std::move(files);
    out["missing"] = std::move(missing);
    out["too_big"] = std::move(too_big);
    return out;
}

} // namespace slash::autopilot::detail
