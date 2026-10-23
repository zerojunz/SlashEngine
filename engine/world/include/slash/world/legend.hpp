#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>

// Legend keys: the short keys a TileMapLayer's and a GridMap's rows of cells are written in,
// each standing for what the component's legend says. Keys of one legend are all the same
// length, one to three characters, and "." (or "..", "...") is an empty cell.
namespace slash::world {

// The characters keys are made of: printable ASCII but space, '.', '"' and backslash.
inline constexpr std::string_view kLegendKeyCharacters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&'()*+,-/:;<=>?@[]^_`{|}~!";
inline constexpr std::size_t kMaxLegendKeyLength = 3;

// Whether key is one to three of kLegendKeyCharacters.
[[nodiscard]] bool valid_legend_key(std::string_view key);

// Whether key is all '.', an empty cell.
[[nodiscard]] bool empty_legend_key(std::string_view key);

// How long keys must be for a legend of count entries: one character up to 91, then two, then
// three.
[[nodiscard]] std::size_t legend_key_length(std::size_t count);

// The key a new entry gets: the start of its name if that is free (or, for one character, the
// same letter in upper case), else the first free key of that length in kLegendKeyCharacters
// order.
[[nodiscard]] std::string new_legend_key(std::string_view name, std::size_t length,
                                         const std::set<std::string, std::less<>>& taken);

} // namespace slash::world
