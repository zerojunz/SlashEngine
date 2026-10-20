#include "slash/library/library.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::library {
namespace {

constexpr std::array<std::string_view, 14> kStopWords = {
    "a", "an", "and", "as", "at", "for", "in", "is", "it", "of", "on", "the", "to", "with"};

// Lowercase words of letters and digits, with a plural "s" dropped so "coins" finds "coin".
// Bytes past ASCII count as letters, so words in any language match whole.
std::set<std::string, std::less<>> words(std::string_view text) {
    std::set<std::string, std::less<>> out;
    std::string word;
    const auto finish = [&out, &word] {
        if (word.size() > 3 && word.back() == 's' && word[word.size() - 2] != 's') {
            word.pop_back();
        }
        if (!word.empty() && std::ranges::find(kStopWords, word) == kStopWords.end()) {
            out.insert(word);
        }
        word.clear();
    };
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || byte >= 0x80) {
            word += c;
        } else if (c >= 'A' && c <= 'Z') {
            word += static_cast<char>(c - 'A' + 'a');
        } else {
            finish();
        }
    }
    finish();
    return out;
}

std::string prompt_of(const Record& record) {
    const json::Value* prompt = record.request.find("prompt");
    return prompt != nullptr && prompt->is_string() ? prompt->as_string() : std::string();
}

// The score, moved by the asset's feedback and rounded to two places.
double with_feedback(double share, const Record& record) {
    const double moved = share + kFeedbackStep * std::clamp(net_feedback(record), -4, 4);
    return std::round(std::clamp(moved, 0.0, 1.0) * 100.0) / 100.0;
}

} // namespace

std::vector<Match> search(std::span<const Record> records, const Query& query) {
    const std::set<std::string, std::less<>> wanted = words(query.text);
    std::vector<Match> matches;
    std::vector<std::size_t> tag_hits;
    for (const Record& record : records) {
        if (!query.type.empty() && record.type != query.type) {
            continue;
        }
        if (!std::ranges::all_of(query.tags, [&record](const std::string& tag) {
                return std::ranges::find(record.tags, tag) != record.tags.end();
            })) {
            continue;
        }
        if (wanted.empty()) {
            matches.push_back({&record, with_feedback(1.0, record)});
            tag_hits.push_back(0);
            continue;
        }
        std::string tags;
        for (const std::string& tag : record.tags) {
            tags += tag;
            tags += ' ';
        }
        const std::set<std::string, std::less<>> tag_words = words(tags);
        const std::set<std::string, std::less<>> text_words =
            words(record.description + " " + prompt_of(record) + " " + record.type);
        std::size_t found = 0;
        std::size_t in_tags = 0;
        for (const std::string& word : wanted) {
            const bool tagged = tag_words.contains(word);
            in_tags += tagged ? 1U : 0U;
            found += tagged || text_words.contains(word) ? 1U : 0U;
        }
        if (found == 0) {
            continue;
        }
        const double share = static_cast<double>(found) / static_cast<double>(wanted.size());
        matches.push_back({&record, with_feedback(share, record)});
        tag_hits.push_back(in_tags);
    }
    std::vector<std::size_t> order(matches.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    // Best first; among equals, the better received, those whose tags matched more of the
    // words, then by ID.
    std::ranges::sort(order, [&matches, &tag_hits](std::size_t a, std::size_t b) {
        if (matches[a].score != matches[b].score) {
            return matches[a].score > matches[b].score;
        }
        const int net_a = net_feedback(*matches[a].record);
        const int net_b = net_feedback(*matches[b].record);
        if (net_a != net_b) {
            return net_a > net_b;
        }
        if (tag_hits[a] != tag_hits[b]) {
            return tag_hits[a] > tag_hits[b];
        }
        return matches[a].record->id < matches[b].record->id;
    });
    std::vector<Match> sorted;
    for (std::size_t i = 0; i < order.size() && sorted.size() < query.limit; ++i) {
        sorted.push_back(matches[order[i]]);
    }
    return sorted;
}

} // namespace slash::library
