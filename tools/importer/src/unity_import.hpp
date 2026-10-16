#pragma once

#include "slash/importer/import.hpp"

#include <optional>
#include <string>

namespace slash::importer {

// Plans converting the Unity project in the source folder (a path ending in '/', holding Assets/
// and ProjectSettings/). Its paths, such as "Assets/Sprites/hero.png", land under prefix in the
// destination ("" or a folder ending in '/'); copy_assets copies textures, sounds, fonts and
// scripts there, else they are used where they are. The destination's project.json and
// storyboard/work.json, when it has them, are merged with what the import writes.
[[nodiscard]] Result<ImportPlan> plan_unity(const std::string& source, const std::string& prefix,
                                            bool copy_assets,
                                            const std::optional<std::string>& existing_project,
                                            const std::optional<std::string>& existing_work);

} // namespace slash::importer
