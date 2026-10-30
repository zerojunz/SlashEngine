#pragma once

#include "slash/core/json.hpp"

#include <string_view>

// Reading what Unreal writes as text: values (ExportText) and whole assets (T3D).
namespace slash::importer::unreal_text {

// text without the white space around it.
[[nodiscard]] std::string_view trimmed(std::string_view text);

// A value as Unreal writes it as text (ExportText: (Key=Value,...), (Item,Item), "quoted" or a
// bare word) as JSON: structs as objects, lists as arrays, the rest as strings. Keys written with
// an index, Inputs(0)=..., gather into arrays.
[[nodiscard]] json::Value text_tree(std::string_view text);

// A Niagara system as Unreal writes it as text (T3D), read into what the export script reads of
// one through NiagaraExternalEditUtilities: "user_variables", and "emitters", each with its
// name, whether it is enabled, its sim_target, data (bLocalSpace), its scripts' modules in stack
// order with their inputs as Unreal writes them as text, and its enabled renderers with the
// meshes they draw. An input's value comes from the override node's pin feeding it (a linked
// parameter, a dynamic input with its own inputs as "chain", or a value), else from its script's
// rapid iteration parameters, else from the module's static switch, an enum's by its display
// name from enums (an enum's path, as the switch's pin names it, to its enumerators' display
// names by name).
[[nodiscard]] json::Value niagara_from_text(std::string_view text, const json::Value& enums);

} // namespace slash::importer::unreal_text
