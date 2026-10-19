#include "slash/gameplay/host.hpp"

#include "slash/core/assert.hpp"
#include "slash/core/log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string_view>
#include <utility>

namespace slash::gameplay {
namespace {

#if defined(_WIN32)
constexpr std::string_view kLibraryFile = ".slash/gameplay/game.dll";
#else
constexpr std::string_view kLibraryFile = ".slash/gameplay/game.so";
#endif

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

json::Value names_json(const std::vector<std::string>& names) {
    json::Array out;
    out.reserve(names.size());
    for (const std::string& name : names) {
        out.emplace_back(name);
    }
    return out;
}

// Whether the code changed a type's fields, or what one holds.
bool same_fields(const std::vector<std::string>& before, const json::Value& defaults_before,
                 const world::ComponentType& after) {
    if (!std::ranges::equal(before, after.fields())) {
        return false;
    }
    return std::ranges::all_of(before, [&](const std::string& field) {
        const json::Value* old_value = defaults_before.find(field);
        const json::Value* new_value = after.defaults().find(field);
        if (old_value == nullptr || new_value == nullptr) {
            return old_value == new_value;
        }
        return old_value->is_number() ? new_value->is_number()
                                      : old_value->type() == new_value->type();
    });
}

std::string random_name() {
    std::random_device random;
    std::array<char, 17> text{};
    (void)std::snprintf(text.data(), text.size(), "%08x%08x", random(), random());
    return text.data();
}

} // namespace

std::string library_path(std::string_view project_root) {
    if (project_root.empty() || project_root == ".") {
        return std::string(kLibraryFile);
    }
    std::string path(project_root);
    if (path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    return path + std::string(kLibraryFile);
}

json::Value Load::to_json() const {
    json::Value out = json::Object{};
    if (!error.empty()) {
        out["error"] = error;
    }
    const std::pair<std::string_view, const std::vector<std::string>*> lists[] = {
        {"added", &added}, {"removed", &removed}, {"changed", &changed}, {"dropped", &dropped}};
    for (const auto& [key, names] : lists) {
        if (!names->empty()) {
            out[std::string(key)] = names_json(*names);
        }
    }
    return out;
}

Host::Host(world::ComponentRegistry& components, std::string library)
    : components_(&components), library_(std::move(library)) {}

Host::Host(world::ComponentRegistry& components, world::GameModuleFunction linked)
    : components_(&components), linked_(linked) {
    SLASH_ASSERT(linked != nullptr, "a gameplay host needs code to link");
}

Load Host::attach(world::World& world) {
    SLASH_ASSERT(std::ranges::find(worlds_, &world) == worlds_.end(),
                 "a world is attached to a gameplay host twice");
    SLASH_ASSERT(&world.components() == components_,
                 "a gameplay host's world must use the host's component types");
    worlds_.push_back(&world);
    if (worlds_.size() > 1) {
        // The code is in already: the world only needs its systems.
        for (const world::GameModule::SystemEntry& system : systems_) {
            world.add_system(system.name, system.run, system.options);
        }
        return {};
    }
    if (linked_ != nullptr) {
        world::GameModule module;
        linked_(module);
        Load load = install(module, nullptr);
        error_ = load.error;
        loaded_ = load.error.empty();
        return load;
    }
    stamp_ = platform::file_stamp(library_);
    if (!stamp_) {
        return {};
    }
    return load_library();
}

void Host::detach(world::World& world) {
    const auto it = std::ranges::find(worlds_, &world);
    SLASH_ASSERT(it != worlds_.end(), "the world is not attached to this gameplay host");
    for (const world::GameModule::SystemEntry& system : systems_) {
        world.remove_system(system.name);
    }
    worlds_.erase(it);
}

std::optional<Load> Host::poll() {
    if (worlds_.empty() || library_.empty()) {
        return std::nullopt;
    }
    for (const world::World* world : worlds_) {
        SLASH_ASSERT(!world->stepping(), "gameplay code cannot be reloaded while a world steps");
    }
    const std::optional<platform::FileStamp> stamp = platform::file_stamp(library_);
    if (!stamp || stamp == stamp_) {
        return std::nullopt;
    }
    stamp_ = stamp;
    return load_library();
}

bool Host::changed() const {
    if (worlds_.empty() || library_.empty()) {
        return false;
    }
    const std::optional<platform::FileStamp> stamp = platform::file_stamp(library_);
    return stamp && stamp != stamp_;
}

json::Value Host::status() const {
    json::Value out;
    if (!library_.empty()) {
        out["library"] = library_;
    }
    out["loaded"] = loaded_;
    out["components"] = names_json(types_);
    json::Array systems;
    for (const world::GameModule::SystemEntry& system : systems_) {
        systems.emplace_back(system.name);
    }
    out["systems"] = std::move(systems);
    if (!error_.empty()) {
        out["error"] = error_;
    }
    return out;
}

Load Host::load_library() {
    const auto fail = [&](std::string message) {
        log::warn("gameplay code did not load", {{"library", library_}, {"detail", message}});
        error_ = message;
        Load load;
        load.error = std::move(message);
        return load;
    };
    if (!platform::can_load_libraries()) {
        return fail("the tools on this platform can't load gameplay libraries yet; exports link "
                    "the code instead");
    }
    auto bytes = platform::read_file(library_);
    if (!bytes) {
        return fail(bytes.error().message);
    }
    // A copy under a name of its own: loaders hand back the library they already hold for a path,
    // and the next build may write the file while this one is loaded.
    const std::size_t slash = library_.find_last_of("/\\");
    const std::string folder =
        (slash == std::string::npos ? std::string() : library_.substr(0, slash + 1)) + "loaded";
    const std::size_t dot = library_.rfind('.');
    const std::string copy = folder + "/game-" + random_name() + "-" + std::to_string(++copies_) +
                             (dot == std::string::npos ? "" : library_.substr(dot));
    if (auto made = platform::create_directories(folder); !made) {
        return fail(made.error().message);
    }
    if (auto written = platform::write_file(copy, bytes.value()); !written) {
        return fail(written.error().message);
    }
    auto opened = platform::SharedLibrary::open(copy);
    (void)platform::remove_file(copy);
    if (!opened) {
        return fail("the gameplay library did not load: " + opened.error().message);
    }
    platform::SharedLibrary library = std::move(opened).value();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto abi = reinterpret_cast<const world::GameModuleAbi* (*)()>(
        library.symbol("slash_game_module_abi"));
    const auto entry =
        reinterpret_cast<world::GameModuleFunction>(library.symbol("slash_game_module"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (abi == nullptr || entry == nullptr) {
        library.unload();
        return fail("the gameplay library has no SLASH_GAME_MODULE function");
    }
    const world::GameModuleAbi* built = abi();
    if (std::string_view(built->api) != SLASH_API_HASH) {
        library.unload();
        return fail("the gameplay library was built against other engine headers; build it again");
    }
    if (std::string_view(built->compiler) != SLASH_COMPILER_ID) {
        const std::string compiler = built->compiler;
        library.unload();
        return fail("the gameplay library was built with " + compiler + ", but the engine with " +
                    SLASH_COMPILER_ID + "; build it again");
    }
    Load load;
    {
        // The module holds functions from the library, so it goes before the library might.
        world::GameModule module;
        entry(module);
        load = install(module, platform::image_of(reinterpret_cast<const void*>(entry)));
    }
    if (!load.error.empty()) {
        library.unload();
        log::warn("gameplay code did not load", {{"library", library_}, {"detail", load.error}});
        error_ = load.error;
        return load;
    }
    libraries_.push_back(library);
    error_.clear();
    loaded_ = true;
    log::info("gameplay code loaded", {{"library", library_},
                                       {"components", static_cast<std::int64_t>(types_.size())},
                                       {"systems", static_cast<std::int64_t>(systems_.size())}});
    return load;
}

Load Host::install(world::GameModule& module, const void* image) {
    Load load;
    world::ComponentRegistry& staged = module.components();
    const auto old_system = [&](std::string_view name) {
        return std::ranges::any_of(systems_, [&](const world::GameModule::SystemEntry& system) {
            return system.name == name;
        });
    };

    // Check everything first, so code that can't go in changes nothing.
    if (!module.errors().empty()) {
        load.error = module.errors().front();
        if (module.errors().size() > 1) {
            load.error += " (and " + std::to_string(module.errors().size() - 1) + " more)";
        }
        return load;
    }
    for (const auto& type : staged.types()) {
        for (const auto& existing : components_->types()) {
            if (!contains(types_, existing->name()) &&
                (existing->name() == type->name() || existing->type_hash() == type->type_hash())) {
                load.error = "the component " + type->name() + " is already the engine's";
                return load;
            }
        }
    }
    for (const world::World* world : worlds_) {
        const std::vector<std::string_view> engine_systems = world->system_names();
        for (const world::GameModule::SystemEntry& system : module.systems()) {
            if (!old_system(system.name) &&
                std::ranges::find(engine_systems, system.name) != engine_systems.end()) {
                load.error = "the system " + system.name + " is already the engine's";
                return load;
            }
        }
    }

    // What the old types hold, world by world and entity by entity.
    struct Kept {
        world::World* world;
        entt::entity entity;
        std::string type;
        json::Value fields;
    };
    struct OldType {
        std::vector<std::string> fields;
        json::Value defaults;
    };
    std::vector<Kept> kept;
    std::map<std::string, OldType, std::less<>> old_types;
    std::vector<entt::id_type> type_pools;
    for (const std::string& name : types_) {
        const world::ComponentType* type = components_->find(name);
        old_types[name] = {{type->fields().begin(), type->fields().end()}, type->defaults()};
        type_pools.push_back(type->type_hash());
    }
    std::map<std::string, std::size_t, std::less<>> drops;
    std::vector<std::pair<world::World*, std::vector<entt::id_type>>> pools;
    for (world::World* world : worlds_) {
        entt::registry& ecs = world->ecs();
        for (const std::string& name : types_) {
            const world::ComponentType* type = components_->find(name);
            if (const auto* pool = std::as_const(ecs).storage(type->type_hash()); pool != nullptr) {
                for (const entt::entity entity : *pool) {
                    kept.push_back({world, entity, name, type->get(*world, entity)});
                }
            }
        }
        std::vector<entt::id_type> ids = type_pools;
        // The old code may have kept components of types it never registered. Those can't be
        // kept, but their storage, which runs the old code, must go with it.
        if (image_ != nullptr) {
            for (const auto [id, pool] : ecs.storage()) {
                if (platform::image_of(&pool.info()) == image_ &&
                    std::ranges::find(ids, id) == ids.end()) {
                    ids.push_back(id);
                    if (!pool.empty()) {
                        drops[std::string(pool.info().name()) +
                              ": not registered, so reloading can't keep it"] += pool.size();
                    }
                }
            }
        }
        pools.emplace_back(world, std::move(ids));
    }

    // Swap the code.
    for (auto& [world, ids] : pools) {
        for (const world::GameModule::SystemEntry& system : systems_) {
            world->remove_system(system.name);
        }
        for (const entt::id_type id : ids) {
            world->ecs().reset(id);
        }
    }
    for (const std::string& name : types_) {
        (void)components_->take(name);
    }
    types_.clear();
    for (const auto& type : staged.types()) {
        types_.push_back(type->name());
    }
    for (const std::string& name : types_) {
        components_->adopt(staged.take(name));
    }
    systems_ = std::move(module.systems());
    for (world::World* world : worlds_) {
        for (const world::GameModule::SystemEntry& system : systems_) {
            world->add_system(system.name, system.run, system.options);
        }
    }
    image_ = image;

    // Put back each value the new types still take.
    for (Kept& entry : kept) {
        const world::ComponentType* type = components_->find(entry.type);
        if (type == nullptr) {
            ++drops[entry.type + ": no longer in the code"];
            continue;
        }
        json::Object& fields = entry.fields.as_object();
        for (auto it = fields.begin(); it != fields.end();) {
            if (std::ranges::find(type->fields(), it->first) == type->fields().end()) {
                ++drops[entry.type + "." + it->first + ": no longer in the code"];
                it = fields.erase(it);
            } else {
                ++it;
            }
        }
        while (true) {
            std::string path;
            auto assigned = type->assign(*entry.world, entry.entity, entry.fields, path);
            if (assigned) {
                break;
            }
            const std::string field = json::pointer_first_key(path);
            if (!fields.contains(field)) {
                ++drops[entry.type + ": " + assigned.error().message];
                break;
            }
            ++drops[entry.type + "." + field + ": " + assigned.error().message];
            fields.erase(field);
        }
    }
    for (const auto& [what, count] : drops) {
        load.dropped.push_back(what + " (" + std::to_string(count) +
                               (count == 1 ? " entity)" : " entities)"));
    }
    for (const std::string& name : types_) {
        const auto old_type = old_types.find(name);
        if (old_type == old_types.end()) {
            load.added.push_back(name);
        } else if (!same_fields(old_type->second.fields, old_type->second.defaults,
                                *components_->find(name))) {
            load.changed.push_back(name);
        }
    }
    for (const auto& [name, unused] : old_types) {
        if (!contains(types_, name)) {
            load.removed.push_back(name);
        }
    }
    return load;
}

} // namespace slash::gameplay
