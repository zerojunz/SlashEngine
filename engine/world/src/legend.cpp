#include "slash/world/legend.hpp"

#include "slash/core/assert.hpp"

#include <algorithm>

namespace slash::world {

namespace {

constexpr std::size_t kKeyCount = kLegendKeyCharacters.size();

} // namespace

bool valid_legend_key(std::string_view key) {
    return !key.empty() && key.size() <= kMaxLegendKeyLength &&
           std::ranges::all_of(
               key, [](char c) { return kLegendKeyCharacters.find(c) != std::string_view::npos; });
}

bool empty_legend_key(std::string_view key) {
    return std::ranges::all_of(key, [](char c) { return c == '.'; });
}

std::size_t legend_key_length(std::size_t count) {
    std::size_t length = 1;
    for (std::size_t capacity = kKeyCount; capacity < count; capacity *= kKeyCount) {
        ++length;
    }
    return length;
}

std::string new_legend_key(std::string_view name, std::size_t length,
                           const std::set<std::string, std::less<>>& taken) {
    if (name.size() >= length) {
        std::string start(name.substr(0, length));
        if (valid_legend_key(start) && !taken.contains(start)) {
            return start;
        }
        if (length == 1 && start.front() >= 'a' && start.front() <= 'z') {
            start.front() = static_cast<char>(start.front() - 'a' + 'A');
            if (!taken.contains(start)) {
                return start;
            }
        }
    }
    // Every key of this length, in kLegendKeyCharacters order.
    std::size_t combinations = 1;
    for (std::size_t i = 0; i < length; ++i) {
        combinations *= kKeyCount;
    }
    std::string key(length, ' ');
    for (std::size_t n = 0; n < combinations; ++n) {
        std::size_t rest = n;
        for (std::size_t i = length; i-- > 0;) {
            key[i] = kLegendKeyCharacters[rest % kKeyCount];
            rest /= kKeyCount;
        }
        if (!taken.contains(key)) {
            return key;
        }
    }
    SLASH_ASSERT(false, "no legend key left");
    return key;
}

} // namespace slash::world
