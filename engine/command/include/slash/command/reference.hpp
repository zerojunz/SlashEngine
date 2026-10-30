#pragma once

#include "slash/core/json.hpp"

#include <string>
#include <vector>

namespace slash::command {

// A page of the reference: its file name and its Markdown.
struct ReferencePage {
    std::string path;
    std::string text;
};

// The engine's reference, written from engine.describe's full reply ({"full": true}) as Godot's
// --doctool writes its class reference from the running engine: README.md (what is there),
// components.md (each component's fields with their types, defaults, limits and docs, and the
// structs inside them), commands.md (each command's description, capabilities and params) and
// systems.md (the systems in the order they run). Names are sorted, so the same engine makes the
// same pages.
[[nodiscard]] std::vector<ReferencePage> reference_pages(const json::Value& describe);

// What the reply leaves without a description, as "component X", "field X.y", "command x.y" or
// "param x.y.z" (nested params and fields by their paths, an array's items as []), sorted.
[[nodiscard]] std::vector<std::string> undocumented(const json::Value& describe);

} // namespace slash::command
