#pragma once

#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A project's own C++ code (project.json's "gameplay"), as the tools see it when they check the
// project's files.
namespace slash::runtime {

// A component the gameplay sources declare with SLASH_COMPONENT(Name, field, ...).
struct DeclaredComponent {
    std::string name;
    std::vector<std::string> fields;
};

// The components a project's gameplay sources declare, read from their text without compiling
// it: every .cpp, .hpp and .h file in the source folders, and the files listed. Sorted by name.
[[nodiscard]] std::vector<DeclaredComponent> declared_components(std::string_view folder,
                                                                 const Project& project);

// How add_gameplay_types found the project's own component types.
enum class GameplayTypes : std::uint8_t {
    None,     // the project has no gameplay code
    Loaded,   // from its gameplay library: every field is checked
    Declared, // stand-ins for what its sources declare: names and fields are checked
};

// Registers the component types of a project's own code, so its scenes and prefabs can be
// checked: those of its gameplay library when load_library is set and it loads, otherwise
// stand-ins (ComponentRegistry::add_stand_in) for the components its sources declare. A loaded
// library stays loaded until the program exits.
GameplayTypes add_gameplay_types(world::ComponentRegistry& registry, std::string_view folder,
                                 const Project& project, bool load_library);

} // namespace slash::runtime
