#pragma once

#include "slash/importer/import.hpp"

#include <map>
#include <optional>
#include <string>

namespace slash::importer {

// A Godot project's files held in memory, by res:// path without "res://": a game unpacked by
// godot::unpack_game.
using GodotFiles = std::map<std::string, std::string>;

// Plans converting the Godot 4 project in the source folder (a path ending in '/'), or the files
// of an unpacked game when unpacked isn't null (source then only names it). Its res:// paths land
// under prefix in the destination ("" or a folder ending in '/'); copy_assets copies textures,
// sounds, fonts and scripts there, else they are used where they are. The destination's
// project.json and storyboard/work.json, when it has them, are merged with what the import writes.
[[nodiscard]] Result<ImportPlan> plan_godot(const std::string& source, const GodotFiles* unpacked,
                                            const std::string& prefix, bool copy_assets,
                                            const std::optional<std::string>& existing_project,
                                            const std::optional<std::string>& existing_work);

} // namespace slash::importer
