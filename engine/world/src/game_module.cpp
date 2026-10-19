#include "slash/world/game_module.hpp"

#include <algorithm>
#include <utility>

namespace slash::world {

void GameModule::add_system(std::string name, System system, SystemOptions options) {
    if (system == nullptr) {
        errors_.push_back("the system " + name + " has nothing to run");
        return;
    }
    if (std::ranges::any_of(systems_,
                            [&](const SystemEntry& added) { return added.name == name; })) {
        errors_.push_back("the system " + name + " is added twice");
        return;
    }
    systems_.push_back({std::move(name), std::move(system), std::move(options)});
}

} // namespace slash::world
