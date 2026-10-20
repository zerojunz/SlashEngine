// project.map: where things are in a project, made from its files on every call, so it can't go
// stale as notes do. Scenes and prefabs list their entities' components, the prefabs they extend,
// the files they use and the storyboard entries they stand for; the game's C++ sources (read with
// outline_cpp, not compiled) list their types, functions, components, systems, events and input
// actions; and across both, each component, system, event and action says where it is declared
// and used.
#include "project_map.hpp"

#include "slash/command/cpp_outline.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

using Names = std::set<std::string, std::less<>>;

constexpr std::array<std::string_view, 7> kSections = {
    "scenes", "prefabs", "sources", "components", "systems", "events", "actions"};

// Functions of slash/input/input.hpp whose string arguments are action names.
constexpr std::array<std::string_view, 9> kActionCalls = {"is_action_pressed",
                                                          "is_action_just_pressed",
                                                          "is_action_just_released",
                                                          "get_action_strength",
                                                          "get_action_raw_strength",
                                                          "get_axis",
                                                          "get_vector",
                                                          "action_press",
                                                          "action_release"};

bool source_file(std::string_view path) {
    return path.ends_with(".cpp") || path.ends_with(".cc") || path.ends_with(".hpp") ||
           path.ends_with(".h") || path.ends_with(".inl");
}

// Whether path is the file or folder within, or inside that folder.
bool inside(std::string_view path, std::string_view within) {
    if (path == within) {
        return true;
    }
    std::string_view folder = within;
    if (folder.ends_with('/')) {
        folder.remove_suffix(1);
    }
    return path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/';
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string folder_of(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

// a/b/../c as a/c; nullopt if it leaves the project.
std::optional<std::string> normalized_path(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if (part == "..") {
            if (parts.empty()) {
                return std::nullopt;
            }
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        path.remove_prefix(slash + 1);
    }
    std::string out;
    for (const std::string_view part : parts) {
        if (!out.empty()) {
            out += '/';
        }
        out += part;
    }
    return out;
}

json::Array to_array(const Names& names) {
    json::Array out;
    out.reserve(names.size());
    for (const std::string& name : names) {
        out.emplace_back(name);
    }
    return out;
}

std::string at_line(std::string_view path, int line) {
    return std::string(path) + ":" + std::to_string(line);
}

// What one scene, prefab or source file holds and touches.
struct FileMap {
    json::Value out;
    Names names;   // components, systems, events, actions, types and functions it names
    Names touches; // project files it extends, uses or includes
};

// A system the sources add with add_system("name", function, options): where its function is
// defined, or else where it is added.
struct SourceSystem {
    std::string location;
    std::string function;
    std::optional<std::int64_t> priority;
    Names components; // that its function's body names
};

// A function a source defines.
struct Definition {
    std::string file;
    const CppSymbol* symbol = nullptr;
};

class Mapper {
public:
    explicit Mapper(Session& session) : session_(session) {}

    std::optional<Reply> load() {
        auto files =
            platform::list_files(session_.options().project_root,
                                 [](std::string_view folder) { return folder != "build/"; });
        if (!files) {
            return Reply::failure(files.error());
        }
        files_.insert(files.value().begin(), files.value().end());
        for (const std::string& file : files_) {
            for (std::string folder = folder_of(file); !folder.empty();
                 folder = folder_of(folder)) {
                folders_.insert(folder);
            }
        }
        if (auto text = session_.read_project_file("project.json")) {
            if (const std::optional<std::string>& contents = text.value(); contents) {
                if (auto parsed = json::parse(*contents); parsed && parsed.value().is_object()) {
                    project_ = std::move(parsed).value();
                }
            }
        }
        // The map works with a broken storyboard too; storyboard.check reports what is wrong.
        if (auto loaded = storyboard::load(
                [this](std::string_view path) { return session_.read_project_file(path); })) {
            entries_ = std::move(loaded.value().entries);
        }
        for (const auto& type : session_.components().types()) {
            components_.emplace(type->name());
        }
        return std::nullopt;
    }

    [[nodiscard]] bool exists(std::string_view path) const {
        return files_.contains(path) || folders_.contains(path);
    }

    [[nodiscard]] std::string_view closest(std::string_view path) const {
        std::vector<std::string_view> candidates(files_.begin(), files_.end());
        candidates.insert(candidates.end(), folders_.begin(), folders_.end());
        return closest_match(path, candidates);
    }

    void read_sources() {
        const json::Value* gameplay = project_.find("gameplay");
        const json::Value* sources =
            gameplay != nullptr && gameplay->is_object() ? gameplay->find("sources") : nullptr;
        if (sources == nullptr || !sources->is_array()) {
            return;
        }
        for (const json::Value& source : sources->as_array()) {
            if (!source.is_string()) {
                continue;
            }
            const std::string& root = source.as_string();
            source_roots_.push_back(root);
            for (const std::string& file : files_) {
                if (inside(file, root) && source_file(file)) {
                    source_paths_.insert(file);
                }
            }
        }
        for (const std::string& path : source_paths_) {
            auto text = session_.read_project_file(path);
            if (!text) {
                continue;
            }
            if (const std::optional<std::string>& contents = text.value(); contents) {
                outlines_.emplace(path, outline_cpp(*contents));
            }
        }
        // Components the sources declare count as components even before the code is built.
        for (const auto& [path, outline] : outlines_) {
            for (const CppMacro& macro : outline.macros) {
                if (macro.name == "SLASH_COMPONENT" && !macro.args.empty()) {
                    components_.insert(macro.args.front());
                    declared_.try_emplace(macro.args.front(), at_line(path, macro.line));
                }
            }
        }
    }

    void map_documents() {
        const std::string& open = session_.scene_path();
        std::map<std::string, json::Value, std::less<>> scenes;
        for (const std::string& file : files_) {
            const bool scene = file.ends_with(world::kSceneSuffix);
            if (!scene && !file.ends_with(world::kPrefabSuffix)) {
                continue;
            }
            json::Value document;
            if (file == open) {
                document = session_.save().scene; // as it is now, unsaved edits included
            } else {
                auto text = session_.read_project_file(file);
                if (!text) {
                    continue;
                }
                const std::optional<std::string>& contents = text.value();
                if (!contents) {
                    continue;
                }
                auto parsed = json::parse(*contents);
                if (!parsed || !parsed.value().is_object()) {
                    continue; // scene validate reports it
                }
                document = std::move(parsed).value();
            }
            (scene ? scenes : prefab_documents_).emplace(file, std::move(document));
        }
        for (const auto& [file, document] : prefab_documents_) {
            prefabs_.emplace(file, map_document(file, document, false));
        }
        for (const auto& [file, document] : scenes) {
            scenes_.emplace(file, map_document(file, document, true));
        }
    }

    void map_sources() {
        for (const auto& [path, outline] : outlines_) {
            for (const CppSymbol& symbol : outline.symbols) {
                if (symbol.kind == "function" && symbol.definition) {
                    const std::size_t colon = symbol.name.rfind("::");
                    const std::string bare =
                        colon == std::string::npos ? symbol.name : symbol.name.substr(colon + 2);
                    definitions_.try_emplace(bare, Definition{path, &symbol});
                }
            }
        }
        for (const auto& [path, outline] : outlines_) {
            sources_.emplace(path, map_source(path, outline));
        }
    }

    // Narrows the map to files in or touching paths, and to what they name.
    void keep_paths(const std::vector<std::string>& paths) {
        const auto selected = [&](const std::string& file, const FileMap& map) {
            return std::ranges::any_of(paths, [&](const std::string& path) {
                return inside(file, path) ||
                       std::ranges::any_of(map.touches, [&](const std::string& touched) {
                           return inside(touched, path);
                       });
            });
        };
        for (auto* files : {&scenes_, &prefabs_, &sources_}) {
            std::erase_if(*files,
                          [&](const auto& item) { return !selected(item.first, item.second); });
        }
        kept_names_ = Names();
        for (const auto* files : {&scenes_, &prefabs_, &sources_}) {
            for (const auto& [file, map] : *files) {
                kept_names_->insert(map.names.begin(), map.names.end());
            }
        }
    }

    // Narrows the map to names containing find, and the files that name them.
    void keep_matching(std::string_view find) {
        find_ = lowered(find);
        for (auto* files : {&scenes_, &prefabs_, &sources_}) {
            std::erase_if(*files, [&](auto& item) {
                if (lowered(item.first).find(find_) != std::string::npos) {
                    return false;
                }
                if (!std::ranges::any_of(item.second.names,
                                         [&](const std::string& name) { return matches(name); })) {
                    return true;
                }
                filter_lists(item.second.out);
                return false;
            });
        }
    }

    json::Value result(const Names& sections) {
        json::Value out;
        json::Value& project = out["project"];
        project = json::Object{};
        for (const std::string_view key : {"name", "main_scene"}) {
            if (const json::Value* value = project_.find(key); value != nullptr) {
                project[key] = *value;
            }
        }
        if (!source_roots_.empty()) {
            json::Array roots;
            for (const std::string& root : source_roots_) {
                roots.emplace_back(root);
            }
            project["sources"] = std::move(roots);
        }
        const auto want = [&](std::string_view section) {
            return sections.empty() || sections.contains(section);
        };
        const auto files_section = [&](std::string_view key,
                                       const std::map<std::string, FileMap, std::less<>>& files) {
            if (!want(key) || files.empty()) {
                return;
            }
            json::Value& section = out[key];
            for (const auto& [file, map] : files) {
                section[file] = map.out;
            }
        };
        files_section("scenes", scenes_);
        files_section("prefabs", prefabs_);
        files_section("sources", sources_);
        if (want("components")) {
            add_components(out);
        }
        if (want("systems")) {
            add_systems(out);
        }
        if (want("events")) {
            add_events(out);
        }
        if (want("actions")) {
            add_actions(out);
        }
        return out;
    }

private:
    [[nodiscard]] bool matches(std::string_view name) const {
        return find_.empty() || lowered(name).find(find_) != std::string::npos;
    }

    // Whether a name survives the filters: named by a kept file, and containing find.
    [[nodiscard]] bool kept(std::string_view name) const {
        return (!kept_names_ || kept_names_->contains(name)) && matches(name);
    }

    // Keeps only matching names in a file's lists, and drops its lists of files and entries.
    void filter_lists(json::Value& out) const {
        for (auto& [key, value] : out.as_object()) {
            if (key == "prefabs" || key == "uses" || key == "includes" || key == "story") {
                value = json::Array{};
            } else if (value.is_array()) {
                std::erase_if(value.as_array(), [&](const json::Value& item) {
                    return item.is_string() && !matches(item.as_string());
                });
            } else if (value.is_object()) {
                std::erase_if(value.as_object(),
                              [&](const auto& item) { return !matches(item.first); });
            }
        }
        std::erase_if(out.as_object(), [](const auto& item) {
            return (item.second.is_array() && item.second.as_array().empty()) ||
                   (item.second.is_object() && item.second.as_object().empty());
        });
    }

    // Storyboard entries whose links name a file or a folder it is in.
    [[nodiscard]] Names linked_entries(std::string_view file) const {
        Names ids;
        for (const json::Value& entry :
             storyboard::query(entries_, {.linked = std::string(file)})) {
            ids.insert(entry.find("id")->as_string());
        }
        return ids;
    }

    // Every string in a document that names another project file.
    void collect_uses(const json::Value& value, std::string_view self, Names& uses) const {
        if (value.is_string()) {
            const std::string& text = value.as_string();
            if (text != self && files_.contains(text)) {
                uses.insert(text);
            }
        } else if (value.is_array()) {
            for (const json::Value& item : value.as_array()) {
                collect_uses(item, self, uses);
            }
        } else if (value.is_object()) {
            for (const auto& [key, member] : value.as_object()) {
                if (key != "extends") {
                    collect_uses(member, self, uses);
                }
            }
        }
    }

    // The components an entity or prefab has: those of the prefab it extends, then its own,
    // where null removes one.
    [[nodiscard]] Names components_of(const json::Value& entity, int depth = 0) const {
        Names names;
        if (!entity.is_object()) {
            return names;
        }
        if (const json::Value* extends = entity.find("extends");
            extends != nullptr && extends->is_string() && depth < 8) {
            if (const auto prefab = prefab_documents_.find(extends->as_string());
                prefab != prefab_documents_.end()) {
                names = components_of(prefab->second, depth + 1);
            }
        }
        if (const json::Value* components = entity.find("components");
            components != nullptr && components->is_object()) {
            for (const auto& [name, value] : components->as_object()) {
                if (value.is_null()) {
                    names.erase(name);
                } else {
                    names.insert(name);
                }
            }
        }
        return names;
    }

    void count_components(const json::Value& entity, std::map<std::string, std::int64_t>& counts,
                          Names& prefabs) const {
        if (!entity.is_object()) {
            return;
        }
        if (const json::Value* extends = entity.find("extends");
            extends != nullptr && extends->is_string()) {
            prefabs.insert(extends->as_string());
        }
        for (const std::string& name : components_of(entity)) {
            ++counts[name];
        }
    }

    FileMap map_document(const std::string& file, const json::Value& document, bool scene) {
        FileMap map;
        std::map<std::string, std::int64_t> counts;
        Names prefabs;
        if (scene) {
            std::int64_t entities = 0;
            if (const json::Value* items = document.find("entities");
                items != nullptr && items->is_object()) {
                for (const auto& [id, entity] : items->as_object()) {
                    ++entities;
                    count_components(entity, counts, prefabs);
                }
            }
            map.out["entities"] = entities;
        } else {
            count_components(document, counts, prefabs);
        }
        if (!counts.empty()) {
            json::Value& components = map.out["components"];
            for (const auto& [name, count] : counts) {
                components[name] = count;
                map.names.insert(name);
                (scene ? scene_uses_ : prefab_uses_)[name].insert(file);
            }
        }
        if (!prefabs.empty()) {
            map.out["prefabs"] = to_array(prefabs);
            map.touches.insert(prefabs.begin(), prefabs.end());
        }
        Names uses;
        collect_uses(document, file, uses);
        if (!uses.empty()) {
            map.out["uses"] = to_array(uses);
            map.touches.insert(uses.begin(), uses.end());
        }
        Names story = linked_entries(file);
        for (const storyboard::StoryRefId& ref : storyboard::story_refs(document)) {
            story.insert(ref.id);
        }
        if (!story.empty()) {
            map.out["story"] = to_array(story);
        }
        return map;
    }

    // An include as the compiler finds it: beside the file, or in a source folder.
    [[nodiscard]] std::optional<std::string> resolve_include(std::string_view file,
                                                             const std::string& include) const {
        const std::string folder = folder_of(file);
        std::vector<std::string> candidates = {folder.empty() ? include : folder + "/" + include};
        for (const std::string& root : source_roots_) {
            std::string candidate = root;
            candidate += '/';
            candidate += include;
            candidates.push_back(std::move(candidate));
        }
        for (const std::string& candidate : candidates) {
            if (auto path = normalized_path(candidate); path && files_.contains(*path)) {
                return path;
            }
        }
        return std::nullopt;
    }

    FileMap map_source(const std::string& path, const CppOutline& outline) {
        FileMap map;
        map.out["lines"] = outline.lines;
        json::Object types;
        json::Object functions;
        for (const CppSymbol& symbol : outline.symbols) {
            const std::string name =
                symbol.member_of.empty() ? symbol.name : symbol.member_of + "::" + symbol.name;
            if (symbol.kind == "function") {
                functions.try_emplace(name, symbol.line);
            } else {
                types.try_emplace(name, symbol.line);
            }
            map.names.insert(name);
        }
        if (!types.empty()) {
            map.out["types"] = std::move(types);
        }
        if (!functions.empty()) {
            map.out["functions"] = std::move(functions);
        }

        Names components;
        for (const std::string& identifier : outline.identifiers) {
            if (components_.contains(identifier)) {
                components.insert(identifier);
                source_uses_[identifier].insert(path);
            }
        }
        Names systems;
        Names events;
        Names actions;
        Names uses;
        for (const CppString& literal : outline.strings) {
            if (files_.contains(literal.value) && literal.value != path) {
                uses.insert(literal.value);
            }
            if (literal.callee == "add_system" && literal.arg == 0) {
                systems.insert(literal.value);
                source_systems_.try_emplace(literal.value, source_system(path, literal));
            } else if (literal.callee == "emit" && literal.arg == 0) {
                events.insert(literal.value);
                emitted_[literal.value].insert(at_line(path, literal.line));
            } else if (std::ranges::find(kActionCalls, literal.callee) != kActionCalls.end()) {
                actions.insert(literal.value);
                action_uses_[literal.value].insert(at_line(path, literal.line));
            } else if (literal.compared == "kind" && !literal.function.empty()) {
                events.insert(literal.value);
                read_[literal.value].insert(at_line(path, literal.line));
            }
        }
        const auto add_list = [&map](std::string_view key, const Names& names) {
            if (!names.empty()) {
                map.out[key] = to_array(names);
                map.names.insert(names.begin(), names.end());
            }
        };
        add_list("components", components);
        add_list("systems", systems);
        add_list("events", events);
        add_list("actions", actions);
        if (!uses.empty()) {
            map.out["uses"] = to_array(uses);
            map.touches.insert(uses.begin(), uses.end());
        }
        Names includes;
        for (const std::string& include : outline.includes) {
            if (auto resolved = resolve_include(path, include)) {
                includes.insert(*resolved);
            }
        }
        if (!includes.empty()) {
            map.out["includes"] = to_array(includes);
            map.touches.insert(includes.begin(), includes.end());
        }
        if (const Names story = linked_entries(path); !story.empty()) {
            map.out["story"] = to_array(story);
        }
        return map;
    }

    [[nodiscard]] SourceSystem source_system(const std::string& path,
                                             const CppString& literal) const {
        SourceSystem system;
        system.location = at_line(path, literal.line);
        if (literal.args.size() > 1) {
            std::string function = literal.args[1];
            if (const std::size_t colon = function.rfind("::"); colon != std::string::npos) {
                function.erase(0, colon + 2);
            }
            if (const auto found = definitions_.find(function); found != definitions_.end()) {
                system.location = at_line(found->second.file, found->second.symbol->line);
                for (const std::string& identifier : found->second.symbol->body_identifiers) {
                    if (components_.contains(identifier)) {
                        system.components.insert(identifier);
                    }
                }
            }
            system.function = std::move(function);
        }
        if (literal.args.size() > 2) {
            system.priority = priority_in(literal.args[2]);
        }
        return system;
    }

    // The priority in a system's options, such as {.doc = "...", .priority = 50}.
    static std::optional<std::int64_t> priority_in(std::string_view options) {
        const std::size_t at = options.find(".priority");
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        std::size_t k = options.find('=', at);
        if (k == std::string_view::npos) {
            return std::nullopt;
        }
        ++k;
        while (k < options.size() && options[k] == ' ') {
            ++k;
        }
        const bool negative = k < options.size() && options[k] == '-';
        k += negative ? 1 : 0;
        std::int64_t value = 0;
        bool digits = false;
        while (k < options.size() && std::isdigit(static_cast<unsigned char>(options[k])) != 0) {
            value = value * 10 + (options[k] - '0');
            digits = true;
            ++k;
        }
        if (!digits) {
            return std::nullopt;
        }
        return negative ? -value : value;
    }

    void add_components(json::Value& out) const {
        Names names;
        for (const auto* uses : {&scene_uses_, &prefab_uses_, &source_uses_}) {
            for (const auto& [name, files] : *uses) {
                names.insert(name);
            }
        }
        for (const auto& [name, where] : declared_) {
            names.insert(name);
        }
        json::Object components;
        for (const std::string& name : names) {
            if (!kept(name)) {
                continue;
            }
            json::Value item = json::Object{};
            if (const auto found = declared_.find(name); found != declared_.end()) {
                item["declared"] = found->second;
                if (const auto* type = session_.components().find(name);
                    type != nullptr && !type->doc().empty()) {
                    item["doc"] = type->doc();
                }
            }
            const auto add = [&](std::string_view key, const auto& uses,
                                 const std::map<std::string, FileMap, std::less<>>& files) {
                const auto found = uses.find(name);
                if (found == uses.end()) {
                    return;
                }
                json::Array list;
                for (const std::string& file : found->second) {
                    if (files.contains(file)) {
                        list.emplace_back(file);
                    }
                }
                if (!list.empty()) {
                    item[key] = std::move(list);
                }
            };
            add("scenes", scene_uses_, scenes_);
            add("prefabs", prefab_uses_, prefabs_);
            add("sources", source_uses_, sources_);
            components.emplace(name, std::move(item));
        }
        if (!components.empty()) {
            out["components"] = std::move(components);
        }
    }

    void add_systems(json::Value& out) const {
        json::Array systems;
        Names listed;
        const auto describe = [&](json::Value& item, const std::string& name) {
            const auto found = source_systems_.find(name);
            if (found == source_systems_.end()) {
                return;
            }
            item["source"] = found->second.location;
            if (!found->second.components.empty()) {
                item["components"] = to_array(found->second.components);
            }
        };
        for (const world::SystemInfo& system : session_.world().systems()) {
            const std::string name(system.name);
            listed.insert(name);
            const bool ours = source_systems_.contains(name);
            // With a filter, the engine's systems go: no file of the project names them.
            if ((kept_names_ || !find_.empty()) && !(ours && kept(name))) {
                continue;
            }
            json::Value item;
            item["name"] = name;
            item["priority"] = system.priority;
            if (ours) {
                if (!system.doc.empty()) {
                    item["doc"] = system.doc;
                }
                describe(item, name);
            }
            systems.push_back(std::move(item));
        }
        // Systems the sources add that aren't running: the code isn't built, or failed to load.
        for (const auto& [name, info] : source_systems_) {
            if (listed.contains(name) || !kept(name)) {
                continue;
            }
            json::Value item;
            item["name"] = name;
            if (info.priority) {
                item["priority"] = *info.priority;
            }
            item["loaded"] = false;
            describe(item, name);
            systems.push_back(std::move(item));
        }
        if (!systems.empty()) {
            out["systems"] = std::move(systems);
        }
    }

    void add_events(json::Value& out) const {
        Names names;
        for (const auto* uses : {&emitted_, &read_}) {
            for (const auto& [name, where] : *uses) {
                names.insert(name);
            }
        }
        json::Object events;
        for (const std::string& name : names) {
            if (!kept(name)) {
                continue;
            }
            json::Value item = json::Object{};
            if (const auto found = emitted_.find(name); found != emitted_.end()) {
                item["emitted"] = kept_locations(found->second);
            }
            if (const auto found = read_.find(name); found != read_.end()) {
                item["read"] = kept_locations(found->second);
            }
            events.emplace(name, std::move(item));
        }
        if (!events.empty()) {
            out["events"] = std::move(events);
        }
    }

    void add_actions(json::Value& out) const {
        std::map<std::string, Names, std::less<>> actions = action_uses_;
        // Actions project.json defines that no code reads still belong on the map.
        if (const json::Value* input = project_.find("input");
            input != nullptr && input->is_object()) {
            for (const auto& [name, action] : input->as_object()) {
                actions.try_emplace(name);
            }
        }
        json::Object listed;
        for (const auto& [name, where] : actions) {
            if (kept(name)) {
                listed.emplace(name, kept_locations(where));
            }
        }
        if (!listed.empty()) {
            out["actions"] = std::move(listed);
        }
    }

    // file:line locations in sources still on the map.
    [[nodiscard]] json::Array kept_locations(const Names& locations) const {
        json::Array out;
        for (const std::string& location : locations) {
            const std::string file = location.substr(0, location.rfind(':'));
            if (sources_.contains(file)) {
                out.emplace_back(location);
            }
        }
        return out;
    }

    Session& session_;
    Names files_;
    Names folders_;
    json::Value project_;
    json::Object entries_;
    Names components_; // registered or declared in the sources
    std::map<std::string, std::string, std::less<>> declared_; // component: file:line
    std::vector<std::string> source_roots_;
    Names source_paths_;
    std::map<std::string, CppOutline, std::less<>> outlines_;
    std::map<std::string, json::Value, std::less<>> prefab_documents_;

    std::map<std::string, FileMap, std::less<>> scenes_;
    std::map<std::string, FileMap, std::less<>> prefabs_;
    std::map<std::string, FileMap, std::less<>> sources_;
    std::map<std::string, Names, std::less<>> scene_uses_;       // component: scenes
    std::map<std::string, Names, std::less<>> prefab_uses_;      // component: prefabs
    std::map<std::string, Names, std::less<>> source_uses_;      // component: sources
    std::map<std::string, Definition, std::less<>> definitions_; // by name without classes
    std::map<std::string, SourceSystem, std::less<>> source_systems_;
    std::map<std::string, Names, std::less<>> emitted_;     // event: file:line
    std::map<std::string, Names, std::less<>> read_;        // event: file:line
    std::map<std::string, Names, std::less<>> action_uses_; // action: file:line

    std::optional<Names> kept_names_;
    std::string find_;
};

Reply failure_at(const Error& error, std::string path) {
    Reply reply = Reply::failure(error);
    reply.data["path"] = std::move(path);
    return reply;
}

Reply project_map(Session& session, const json::Value& params) {
    Mapper mapper(session);
    if (auto failed = mapper.load()) {
        return std::move(*failed);
    }
    std::vector<std::string> paths;
    if (const json::Value* items = params.find("paths"); items != nullptr) {
        for (std::size_t i = 0; i < items->size(); ++i) {
            std::string path = items->as_array()[i].as_string();
            if (path.ends_with('/')) {
                path.pop_back();
            }
            const std::string at = "/paths/" + std::to_string(i);
            if (!is_project_path(path)) {
                return failure_at(make_error(ErrorCode::InvalidArgument,
                                             "paths are relative to the project folder, "
                                             "separated by '/', with no \".\" or \"..\" parts; "
                                             "got \"" +
                                                 path + "\""),
                                  at);
            }
            if (!mapper.exists(path)) {
                const std::string_view hint = mapper.closest(path);
                return failure_at(
                    make_error(
                        ErrorCode::NotFound,
                        "no file or folder \"" + path + "\"" +
                            (hint.empty() ? "" : "; did you mean \"" + std::string(hint) + "\"?")),
                    at);
            }
            paths.push_back(std::move(path));
        }
    }
    Names sections;
    if (const json::Value* items = params.find("sections"); items != nullptr) {
        for (const json::Value& item : items->as_array()) {
            sections.insert(item.as_string());
        }
    }
    mapper.read_sources();
    mapper.map_documents();
    mapper.map_sources();
    if (!paths.empty()) {
        mapper.keep_paths(paths);
    }
    if (const json::Value* find = params.find("find"); find != nullptr) {
        mapper.keep_matching(find->as_string());
    }
    return Reply::success(mapper.result(sections));
}

json::Value string_array(std::string_view description) {
    json::Value schema;
    schema["type"] = "array";
    schema["items"]["type"] = "string";
    schema["description"] = description;
    return schema;
}

} // namespace

void register_project_map(Registry& registry) {
    json::Value sections = string_array("Only these parts of the map. Default: all.");
    json::Array names;
    for (const std::string_view section : kSections) {
        names.emplace_back(section);
    }
    sections["items"]["enum"] = std::move(names);
    json::Value find;
    find["type"] = "string";
    find["minLength"] = 1;
    find["description"] = "Only names containing this, ignoring case, and the files that name "
                          "them or whose paths contain it: where is it defined and used?";
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"paths", string_array("Only files in these files or folders, or that use, extend or "
                               "include something in them.")},
        {"find", std::move(find)},
        {"sections", std::move(sections)},
    };
    registry.add({
        .name = "project.map",
        .description =
            "Where things are in the project, made from its files: scenes and prefabs with their "
            "components, prefabs, used files and storyboard entries; the game's C++ sources with "
            "their types, functions (with lines), components, systems, events and input actions; "
            "and where each component, system, event and action is declared and used.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = project_map,
    });
}

} // namespace slash::command
