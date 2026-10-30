#pragma once

#include "slash/plugin/plugins.hpp"

#include <map>
#include <string>
#include <string_view>

namespace slash::runtime {

// Loads the plugins a project's project.json lists, for this platform (slash/plugin/plugins.hpp),
// and returns each one's error by file; nothing for a folder with no project or no plugins.
std::map<std::string, std::string, std::less<>> load_plugins(plugin::Plugins& plugins,
                                                             std::string_view folder);

} // namespace slash::runtime
