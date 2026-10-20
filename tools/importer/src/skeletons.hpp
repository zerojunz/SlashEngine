#pragma once

#include "common.hpp"

#include "slash/importer/scripts.hpp"
#include "slash/world/component_registry.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>

// The C++ skeletons an import writes for a game's scripts, which the Godot and Unity importers
// share: each named apart from the engine's, the game's own code and each other's, one the
// destination has left as it is, and all registered by imported/scripts.cpp and a game module.
namespace slash::importer {

struct Skeleton {
    Script script;
    std::string code;  // its file
    bool kept = false; // the destination has it already, so it stays as it is
};

class Skeletons {
public:
    // Reads the names the destination's own code takes.
    explicit Skeletons(const Existing& existing);

    // Names a script's namespace, system and component and, for a node script, registers a
    // stand-in for its component, so scenes can give entities its values before the code is
    // built. source is the script's path in the project imported.
    Skeleton& add(const std::string& source, Script script, world::ComponentRegistry& components);

    [[nodiscard]] Skeleton* find(const std::string& source);
    [[nodiscard]] const Skeleton* find(const std::string& source) const;
    [[nodiscard]] bool empty() const { return skeletons_.empty(); }
    [[nodiscard]] const std::string& folder() const { return folder_; } // the gameplay folder
    [[nodiscard]] std::map<std::string, Skeleton>& all() { return skeletons_; }

    // Writes each skeleton but those kept, the file registering them all and, when the game's
    // code has no module, a game module calling it. Returns the module written, or "".
    std::string write(const std::function<void(std::string, std::string)>& add_file,
                      Problems& problems) const;

private:
    const Existing& existing_;
    std::string folder_;
    std::set<std::string> components_taken_;
    std::set<std::string> systems_taken_;
    std::set<std::string> namespaces_taken_{"scripts", "module"};
    std::map<std::string, Skeleton> skeletons_; // by source
};

} // namespace slash::importer
