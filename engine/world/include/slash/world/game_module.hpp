#pragma once

#include "slash/core/api_hash.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <string>
#include <vector>

// A game's own C++ code, as Godot's scripts are: the components and systems it adds to the
// engine's. The game defines one function with SLASH_GAME_MODULE in its gameplay sources:
//
//   SLASH_GAME_MODULE(game) {
//       game.add_component<Health>().set_doc("Hit points.");
//       game.add_system("regeneration", regenerate, {.doc = "Heals 1 point a second."});
//   }
//
// Exports link the sources into the game. slash-cli and slash-mcp load them from the library
// gameplay.build makes, and load it again when it is rebuilt: the world keeps its entities, and
// each component of the game's types keeps its values field by field.
namespace slash::world {

class GameModule {
public:
    // A system the code adds.
    struct SystemEntry {
        std::string name;
        System run;
        SystemOptions options;
    };

    // Registers a component, as ComponentRegistry::add does; chain set_doc and describe on the
    // result.
    template <reflect::Component T>
    ComponentType& add_component();

    // Adds a system, as World::add_system does. Names must be unique among every system.
    void add_system(std::string name, System system, SystemOptions options = {});

    // What the code registered, for the engine to install.
    [[nodiscard]] ComponentRegistry& components() noexcept { return components_; }
    [[nodiscard]] std::vector<SystemEntry>& systems() noexcept { return systems_; }
    // Mistakes in the registration, such as a name used twice. The engine loads no code that has
    // any.
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    ComponentRegistry components_;
    std::vector<SystemEntry> systems_;
    std::vector<std::string> errors_;
};

template <reflect::Component T>
ComponentType& GameModule::add_component() {
    if (ComponentType* added = components_.find_mutable(reflect::type_name<T>());
        added != nullptr) {
        errors_.push_back("the component " + added->name() + " is registered twice");
        return *added;
    }
    return components_.add<T>();
}

// The function SLASH_GAME_MODULE defines.
using GameModuleFunction = void (*)(GameModule& game);

// What a gameplay library was built against. The engine refuses a library built against other
// headers or with another compiler, since their structs might not match its own.
struct GameModuleAbi {
    const char* api;      // SLASH_API_HASH
    const char* compiler; // SLASH_COMPILER_ID
};

} // namespace slash::world

#define SLASH_GAME_MODULE_STRINGIFY_(value) #value
#define SLASH_GAME_MODULE_STRINGIFY(value) SLASH_GAME_MODULE_STRINGIFY_(value)

// The compiler and C++ standard library, as text.
#if defined(__clang__)
#define SLASH_COMPILER_NAME "clang " __clang_version__
#elif defined(__GNUC__)
#define SLASH_COMPILER_NAME "gcc " __VERSION__
#elif defined(_MSC_VER)
#define SLASH_COMPILER_NAME "msvc " SLASH_GAME_MODULE_STRINGIFY(_MSC_FULL_VER)
#else
#define SLASH_COMPILER_NAME "unknown"
#endif
#if defined(_LIBCPP_VERSION)
#define SLASH_COMPILER_ID                                                                          \
    SLASH_COMPILER_NAME ", libc++ " SLASH_GAME_MODULE_STRINGIFY(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define SLASH_COMPILER_ID                                                                          \
    SLASH_COMPILER_NAME ", libstdc++ " SLASH_GAME_MODULE_STRINGIFY(__GLIBCXX__)
#else
#define SLASH_COMPILER_ID SLASH_COMPILER_NAME
#endif

#if defined(_MSC_VER)
#define SLASH_GAME_MODULE_EXPORT
#else
#define SLASH_GAME_MODULE_EXPORT __attribute__((visibility("default")))
#endif

// Defines the game's registration function, named slash_game_module, and slash_game_module_abi,
// which says what it was built against. The argument names the GameModule in the body.
// NOLINTBEGIN(bugprone-macro-parentheses): the argument is a parameter name
#define SLASH_GAME_MODULE(game)                                                                    \
    extern "C" SLASH_GAME_MODULE_EXPORT const ::slash::world::GameModuleAbi*                       \
    slash_game_module_abi() {                                                                      \
        static constexpr ::slash::world::GameModuleAbi kAbi{SLASH_API_HASH, SLASH_COMPILER_ID};    \
        return &kAbi;                                                                              \
    }                                                                                              \
    extern "C" SLASH_GAME_MODULE_EXPORT void slash_game_module(::slash::world::GameModule& game)
// NOLINTEND(bugprone-macro-parentheses)
