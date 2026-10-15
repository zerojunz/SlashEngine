#pragma once

#include <cstddef>
#include <string_view>

namespace slash {

// The Levenshtein distance between two strings: the fewest single-character insertions,
// deletions or substitutions that turn one into the other.
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b);

// The candidate closest to a misspelled name, or "" if none is close enough to suggest.
// Used for "did you mean ...?" hints.
template <typename Range>
[[nodiscard]] std::string_view closest_match(std::string_view name, const Range& candidates) {
    std::string_view best;
    std::size_t best_distance = (name.size() / 3 > 2 ? name.size() / 3 : 2) + 1;
    for (const auto& candidate : candidates) {
        const std::string_view text(candidate);
        const std::size_t distance = edit_distance(name, text);
        if (distance < best_distance) {
            best = text;
            best_distance = distance;
        }
    }
    return best;
}

// Whether path names a file inside a project: relative, separated by '/', with no empty, "." or
// ".." segments, and no backslash or ':'. Such a path names the same file on every platform.
[[nodiscard]] bool is_project_path(std::string_view path);

} // namespace slash
