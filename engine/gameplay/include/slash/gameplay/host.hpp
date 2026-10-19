#pragma once

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/library.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Puts a game's own C++ code (slash/world/game_module.hpp) into a world: linked into the program,
// as exports link it, or from the library a project's gameplay sources build to, loaded again
// whenever it is rebuilt.
namespace slash::gameplay {

// Where a project's gameplay library is built: <project>/.slash/gameplay/game.so, or game.dll on
// Windows.
[[nodiscard]] std::string library_path(std::string_view project_root);

// What putting the game's code into the world did.
struct Load {
    // Why the code did not go in, or "". The code that was in keeps running.
    std::string error;
    // The game's component types that came, went, or changed their fields.
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed;
    // Values that did not survive, such as "Health.armor: no longer in the code (3 entities)".
    std::vector<std::string> dropped;

    [[nodiscard]] json::Value to_json() const;
};

class Host {
public:
    // Code built into a library, read when the host is attached and again whenever the file
    // changes.
    Host(world::ComponentRegistry& components, std::string library);
    // Code linked into the program.
    Host(world::ComponentRegistry& components, world::GameModuleFunction linked);

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;
    ~Host() = default;

    // Puts the code into a world made with the host's component types, before it loads any
    // scene: the first world attached loads the code and registers its components, and every
    // world gets its systems. Worlds that share the component types, such as the sessions
    // test.run makes, share the host.
    Load attach(world::World& world);
    // Takes a world's systems out and forgets the world, before it goes.
    void detach(world::World& world);

    // Reads the library again if its file changed since it was last read, and swaps the new code
    // into every attached world. Each keeps its entities, and each of their components of the
    // game's types keeps the fields its new type still has. nullopt when the file is unchanged or
    // missing. Libraries stay loaded until the program exits, so nothing that still points into
    // old code can crash.
    std::optional<Load> poll();
    // Whether poll would read the library again: the file is there and changed since.
    [[nodiscard]] bool changed() const;

    // {"library", "loaded", "components", "systems"} and "error" if the last load failed.
    [[nodiscard]] json::Value status() const;
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    // The library's path; "" for linked code.
    [[nodiscard]] const std::string& library() const noexcept { return library_; }

private:
    Load load_library();
    // Swaps the old code's types and systems for the module's, keeping the worlds' values.
    // image is where the library holding the module's code starts, or nullptr for linked code.
    Load install(world::GameModule& module, const void* image);

    world::ComponentRegistry* components_;
    std::vector<world::World*> worlds_;
    std::string library_;
    world::GameModuleFunction linked_ = nullptr;
    std::vector<std::string> types_;                      // the code's component types
    std::vector<world::GameModule::SystemEntry> systems_; // the code's systems
    std::optional<platform::FileStamp> stamp_;            // the library's when last read
    std::vector<platform::SharedLibrary> libraries_;
    const void* image_ = nullptr; // where the loaded library starts
    std::string error_;
    std::uint64_t copies_ = 0;
    bool loaded_ = false;
};

} // namespace slash::gameplay
