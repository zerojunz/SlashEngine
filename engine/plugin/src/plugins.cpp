#include "slash/plugin/plugins.hpp"

#include "internal.hpp"

#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/library.hpp"
#include "slash/platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <ranges>
#include <utility>

namespace slash::plugin {
namespace {

constexpr std::array<std::string_view, 6> kSystems{"android", "ios", "linux",
                                                   "macos",   "web", "windows"};
constexpr std::array<std::string_view, 3> kCpus{"arm64", "wasm32", "x86_64"};

// What a library key names: "<os>" or "<os>.<arch>".
bool known_key(std::string_view key) {
    const std::size_t dot = key.find('.');
    const std::string_view system = key.substr(0, dot);
    if (std::ranges::find(kSystems, system) == kSystems.end()) {
        return false;
    }
    return dot == std::string_view::npos ||
           std::ranges::find(kCpus, key.substr(dot + 1)) != kCpus.end();
}

SlashWorld* world_handle(world::World& world) {
    return reinterpret_cast<SlashWorld*>(&world);
}

} // namespace

json::Value plugin_file_schema() {
    auto schema = json::parse(R"({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "SlashEngine plugin",
        "description": "A plugin library built against slash/plugin/slash_plugin.h, as Godot's .gdextension file.",
        "type": "object",
        "additionalProperties": false,
        "required": ["entry"],
        "properties": {
            "entry": {"type": "string", "minLength": 1,
                      "description": "The C name of the plugin's entry function, unique among the plugins a game links."},
            "abi": {"type": "integer", "minimum": 1, "default": 1,
                    "description": "The SLASH_PLUGIN_ABI_VERSION the plugin was built against."},
            "libraries": {"type": "object", "minProperties": 1,
                          "additionalProperties": {"type": "string", "minLength": 1},
                          "description": "The library for each platform, by \"<os>.<arch>\" or \"<os>\" (android, ios, linux, macos, web, windows; arm64, wasm32, x86_64), relative to this file: a shared library, or a static one on iOS and the web."},
            "sources": {"type": "array", "minItems": 1, "items": {"type": "string", "minLength": 1},
                        "description": "C, C++ and Objective-C files and folders, relative to this file, that exports compile into the game for every platform in place of its libraries. The tools still load the library for the machine they run on."}
        }
    })");
    return std::move(schema).value();
}

std::vector<json::ValidationError> validate_plugin_file(const json::Value& file) {
    std::vector<json::ValidationError> errors = json::validate(file, plugin_file_schema());
    if (!errors.empty()) {
        return errors;
    }
    const std::string& entry = file.find("entry")->as_string();
    const bool c_name = std::isdigit(static_cast<unsigned char>(entry[0])) == 0 &&
                        std::ranges::all_of(entry, [](char c) {
                            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
                        });
    if (!c_name) {
        errors.push_back({"/entry", "expected a C function's name"});
    }
    const json::Value* libraries = file.find("libraries");
    if (libraries == nullptr && file.find("sources") == nullptr) {
        errors.push_back({"", R"(expected "libraries", "sources" or both)"});
    }
    const json::Object none;
    for (const auto& [key, path] : libraries != nullptr ? libraries->as_object() : none) {
        if (!known_key(key)) {
            errors.push_back({"/libraries/" + json::pointer_escape(key),
                              "expected \"<os>\" or \"<os>.<arch>\": os one of android, ios, "
                              "linux, macos, web or windows, arch arm64, wasm32 or x86_64"});
        }
    }
    if (const json::Value* abi = file.find("abi");
        abi != nullptr && abi->as_int() > SLASH_PLUGIN_ABI_VERSION) {
        errors.push_back({"/abi", "the plugin needs interface version " +
                                      std::to_string(abi->as_int()) + "; this engine has " +
                                      std::to_string(SLASH_PLUGIN_ABI_VERSION)});
    }
    return errors;
}

Result<PluginFile> parse_plugin_file(const json::Value& file, std::string path) {
    if (const auto errors = validate_plugin_file(file); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          path + errors.front().path + ": " + errors.front().message);
    }
    PluginFile out;
    out.path = std::move(path);
    out.entry = file.find("entry")->as_string();
    if (const json::Value* abi = file.find("abi"); abi != nullptr) {
        out.abi = static_cast<std::uint32_t>(abi->as_int());
    }
    if (const json::Value* libraries = file.find("libraries"); libraries != nullptr) {
        for (const auto& [key, library] : libraries->as_object()) {
            out.libraries.emplace(key, library.as_string());
        }
    }
    if (const json::Value* sources = file.find("sources"); sources != nullptr) {
        for (const json::Value& source : sources->as_array()) {
            out.sources.push_back(source.as_string());
        }
    }
    return out;
}

std::string library_for(const PluginFile& file, std::string_view os, std::string_view arch) {
    auto found = file.libraries.find(std::string(os) + "." + std::string(arch));
    if (found == file.libraries.end()) {
        found = file.libraries.find(os);
    }
    if (found == file.libraries.end()) {
        return {};
    }
    const std::filesystem::path folder = std::filesystem::path(file.path).parent_path();
    return (folder / found->second).lexically_normal().generic_string();
}

std::string_view cpu_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__EMSCRIPTEN__)
    return "wasm32";
#else
    return "x86_64";
#endif
}

SlashProc get_proc(const char* name) {
    return detail::proc(name);
}

Plugins::Plugins(world::ComponentRegistry& components) : components_(&components) {}

Plugins::~Plugins() {
    for (world::World* world : std::vector(worlds_)) {
        detach(*world);
    }
    for (const Loaded& plugin : std::views::reverse(loaded_)) {
        if (plugin.init.deinitialize != nullptr) {
            plugin.init.deinitialize(plugin.init.userdata);
        }
    }
}

Result<void> Plugins::load(const std::string& library, const std::string& entry, std::string name) {
    auto opened = platform::SharedLibrary::open(library);
    if (!opened) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not load " + library + ": " + opened.error().message);
    }
    // Plugins stay loaded: their code backs the components and systems they added.
    const platform::SharedLibrary kept = std::move(opened).value();
    const auto function = reinterpret_cast<SlashPluginEntry>(kept.symbol(entry.c_str()));
    if (function == nullptr) {
        return make_error(ErrorCode::NotFound, library + " has no function " + entry);
    }
    return enter(function, std::move(name));
}

Result<void> Plugins::load_linked(SlashPluginEntry entry, std::string name) {
    if (entry == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "no entry function");
    }
    return enter(entry, std::move(name));
}

Result<void> Plugins::enter(SlashPluginEntry entry, std::string name) {
    if (std::ranges::find(loaded_, name, &Loaded::name) != loaded_.end()) {
        return make_error(ErrorCode::Conflict, "the plugin " + name + " is loaded already");
    }
    detail::Registration registration;
    registration.plugin = name;
    registration.components = components_;
    SlashPlugin handle{&registration};
    SlashPluginInit init{
        .abi_version = SLASH_PLUGIN_ABI_VERSION, .userdata = nullptr, .deinitialize = nullptr};
    const int entered = entry(&get_proc, &handle, &init);
    // What it adds must not clash with the systems of the worlds it will run in.
    for (const world::World* world : worlds_) {
        const std::vector<std::string_view> names = world->system_names();
        for (const PluginSystem& system : registration.systems) {
            if (std::ranges::find(names, std::string_view(system.name)) != names.end()) {
                registration.errors.push_back(system.name + ": a system of that name exists");
            }
        }
    }
    if (entered == 0 || !registration.errors.empty()) {
        for (const detail::Layout* layout : registration.layouts) {
            (void)components_->take(layout->name);
        }
        if (entered != 0 && init.deinitialize != nullptr) {
            init.deinitialize(init.userdata);
        }
        std::string message =
            entered == 0 ? "its entry function refused to load" : registration.errors.front();
        if (registration.errors.size() > 1) {
            message += " (and " + std::to_string(registration.errors.size() - 1) + " more)";
        }
        return make_error(ErrorCode::InvalidArgument, "the plugin " + name + ": " + message);
    }
    Loaded loaded;
    loaded.name = std::move(name);
    for (const detail::Layout* layout : registration.layouts) {
        loaded.components.push_back(layout->name);
    }
    loaded.systems = std::move(registration.systems);
    loaded.init = init;
    for (world::World* world : worlds_) {
        add_systems(*world, loaded);
    }
    log::info("plugin loaded", {{"plugin", loaded.name},
                                {"components", loaded.components.size()},
                                {"systems", loaded.systems.size()}});
    loaded_.push_back(std::move(loaded));
    return {};
}

std::map<std::string, std::string, std::less<>>
Plugins::load_project(const std::string& root, std::span<const std::string> files,
                      const LoadOptions& options) {
    std::map<std::string, std::string, std::less<>> errors;
    for (const std::string& path : files) {
        const auto fail = [&](std::string message) {
            log::error("a plugin did not load", {{"file", path}, {"error", message}});
            errors.emplace(path, std::move(message));
        };
        auto text = platform::read_file_in(root, path);
        if (!text) {
            fail(text.error().message);
            continue;
        }
        auto parsed = json::parse(text.value());
        if (!parsed) {
            fail(parsed.error().message);
            continue;
        }
        auto file = parse_plugin_file(parsed.value(), path);
        if (!file) {
            fail(file.error().message);
            continue;
        }
        const std::string name = std::filesystem::path(path).filename().string();
        const std::string plugin = name.substr(0, name.find('.'));
        if (const auto linked = options.linked.find(file.value().entry);
            linked != options.linked.end()) {
            if (auto loaded = load_linked(linked->second, plugin); !loaded) {
                fail(loaded.error().message);
            }
            continue;
        }
        const std::string library = library_for(file.value(), platform::os_name(), cpu_name());
        if (library.empty()) {
            fail("it names no library for " + std::string(platform::os_name()) + "." +
                 std::string(cpu_name()));
            continue;
        }
        std::string full = (std::filesystem::path(root) / library).string();
        if (!platform::exists(full)) {
            // Where an export installed it: by its file name in the folders given.
            const std::string file_name = std::filesystem::path(library).filename().string();
            for (const std::string& folder : options.library_folders) {
                const std::string candidate =
                    folder.empty() ? file_name
                                   : (std::filesystem::path(folder) / file_name).string();
                if (folder.empty() || platform::exists(candidate)) {
                    full = candidate;
                    break;
                }
            }
        }
        if (auto loaded = load(full, file.value().entry, plugin); !loaded) {
            fail(loaded.error().message);
        }
    }
    return errors;
}

void Plugins::add_systems(world::World& world, const Loaded& plugin) const {
    const std::vector<std::string_view> names = world.system_names();
    for (const PluginSystem& system : plugin.systems) {
        if (std::ranges::find(names, std::string_view(system.name)) != names.end()) {
            log::error("a plugin's system has the name of one the world has",
                       {{"plugin", plugin.name}, {"system", system.name}});
            continue;
        }
        world.add_system(
            system.name,
            [run = system.run, userdata = system.userdata](world::World& into, double seconds) {
                run(userdata, world_handle(into), seconds);
            },
            {.doc = system.doc, .priority = system.priority});
    }
}

void Plugins::attach(world::World& world) {
    if (std::ranges::find(worlds_, &world) != worlds_.end()) {
        return;
    }
    worlds_.push_back(&world);
    for (const Loaded& plugin : loaded_) {
        add_systems(world, plugin);
    }
}

void Plugins::detach(world::World& world) {
    const auto it = std::ranges::find(worlds_, &world);
    if (it == worlds_.end()) {
        return;
    }
    for (const Loaded& plugin : loaded_) {
        for (const PluginSystem& system : plugin.systems) {
            (void)world.remove_system(system.name);
        }
    }
    worlds_.erase(it);
}

json::Value Plugins::status() const {
    json::Array out;
    for (const Loaded& plugin : loaded_) {
        json::Value item;
        item["name"] = plugin.name;
        json::Array components;
        for (const std::string& component : plugin.components) {
            components.emplace_back(component);
        }
        json::Array systems;
        for (const PluginSystem& system : plugin.systems) {
            systems.emplace_back(system.name);
        }
        item["components"] = std::move(components);
        item["systems"] = std::move(systems);
        out.push_back(std::move(item));
    }
    return {std::move(out)};
}

} // namespace slash::plugin
