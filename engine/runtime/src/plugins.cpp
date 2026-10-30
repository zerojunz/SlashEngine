#include "slash/runtime/plugins.hpp"

#include "slash/runtime/project.hpp"

namespace slash::runtime {

std::map<std::string, std::string, std::less<>> load_plugins(plugin::Plugins& plugins,
                                                             std::string_view folder) {
    auto project = load_project(folder);
    if (!project || project.value().plugins.empty()) {
        return {};
    }
    return plugins.load_project(std::string(folder), project.value().plugins);
}

} // namespace slash::runtime
