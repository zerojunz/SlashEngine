#pragma once

#include "common.hpp"

#include "slash/importer/import.hpp"

#include <filesystem>
#include <string>

namespace slash::importer {

// Plans converting the Unreal project whose .uproject file is project, exported with the Unreal
// Editor (editor, or one find_editor finds) into work. Its paths ("Content/Maps/Main.umap",
// "/Game/Maps/Main" in Unreal) land under prefix in the destination ("" or a folder ending in
// '/'); copy_assets copies the Blueprints and C++ sources the port tasks link to there. The
// meshes and sounds the editor exported are written beside the assets they came from. The
// destination's project.json and storyboard/work.json, when it has them, are merged with what the
// import writes.
[[nodiscard]] Result<ImportPlan>
plan_unreal(const std::filesystem::path& project, const std::string& prefix, bool copy_assets,
            const Existing& existing, const std::filesystem::path& work, const std::string& editor);

} // namespace slash::importer
