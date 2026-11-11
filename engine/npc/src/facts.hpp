#pragma once

// How brains, quests and dialogue read and test facts, shared by their files.

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/npc/npc.hpp"

#include <string>
#include <string_view>

namespace slash::npc::detail {

// Reads a whole number, all of text, with an optional "+".
bool read_int(std::string_view text, int& out);
// A condition's comparison and value from a file's value (true, 3, "<5", ">=1"), or an error.
std::string read_condition(const json::Value& value, Compare& compare, int& out);
// An effect from a file's value (true, 3, "+1", "-2"), or an error.
std::string read_effect(const json::Value& value, bool& add, int& out);
// Whether value compares with against as compare says.
bool meets(Compare compare, int value, int against);
// value + change, held to int's range.
int added(int value, int change);
// The assets the world's npc system loads brains, quests and dialogue from, or null.
asset::Assets* assets_of(const world::World& world);

} // namespace slash::npc::detail
