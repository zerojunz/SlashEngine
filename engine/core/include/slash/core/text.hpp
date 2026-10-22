#pragma once

#include <cstddef>
#include <optional>
#include <string>
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

// The character of UTF-8 text at byte i, moving i past it; a malformed byte reads as U+FFFD
// and moves one byte.
[[nodiscard]] char32_t next_utf8(std::string_view text, std::size_t& i);
// Adds a character to UTF-8 text.
void append_utf8(std::string& out, char32_t character);
// UTF-8 text as characters, and back.
[[nodiscard]] std::u32string to_utf32(std::string_view text);
[[nodiscard]] std::string to_utf8(std::u32string_view text);

// Whether path names a file inside a project: relative, separated by '/', with no empty, "." or
// ".." segments, and no backslash or ':'. Such a path names the same file on every platform.
[[nodiscard]] bool is_project_path(std::string_view path);

// The number decimal text such as "-1.5e3" stands for, correctly rounded and read the same in
// every locale; nullopt if text isn't one. It takes JSON's numbers, and also a leading '+',
// leading zeros and a '.' with digits on one side only, as other text formats write them.
[[nodiscard]] std::optional<double> parse_decimal(std::string_view text);

// Bytes as base64 text (RFC 4648, with padding), as JSON carries images.
[[nodiscard]] std::string base64_encode(std::string_view bytes);
// The bytes base64 text stands for, padded or not; nullopt if it isn't base64.
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view text);

} // namespace slash
