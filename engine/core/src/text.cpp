#include "slash/core/text.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <yyjson.h>

namespace slash {

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const std::size_t substitute = diagonal + (a[i - 1] == b[j - 1] ? 0 : 1);
            row[j] = std::min({above + 1, row[j - 1] + 1, substitute});
            diagonal = above;
        }
    }
    return row[b.size()];
}

char32_t next_utf8(std::string_view text, std::size_t& i) {
    const auto first = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    if (first >= 0xF0U) {
        length = 4;
    } else if (first >= 0xE0U) {
        length = 3;
    } else if (first >= 0xC0U) {
        length = 2;
    }
    if (length == 1 || i + length > text.size()) {
        ++i;
        return first < 0x80U ? char32_t{first} : char32_t{0xFFFD};
    }
    char32_t code = first & (0xFFU >> (length + 1));
    for (std::size_t k = 1; k < length; ++k) {
        const auto byte = static_cast<unsigned char>(text[i + k]);
        if ((byte & 0xC0U) != 0x80U) {
            ++i;
            return 0xFFFD;
        }
        code = (code << 6U) | (byte & 0x3FU);
    }
    i += length;
    return code;
}

void append_utf8(std::string& out, char32_t character) {
    const auto code = static_cast<std::uint32_t>(character);
    if (code < 0x80U) {
        out += static_cast<char>(code);
    } else if (code < 0x800U) {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code < 0x10000U) {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (code >> 18U));
        out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

std::u32string to_utf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        out += next_utf8(text, i);
    }
    return out;
}

std::string to_utf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char32_t c : text) {
        append_utf8(out, c);
    }
    return out;
}

bool is_project_path(std::string_view path) {
    if (path.empty() || path.front() == '/' ||
        path.find_first_of("\\:") != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::optional<double> parse_decimal(std::string_view text) {
    // Rewritten in JSON's grammar, which yyjson reads correctly rounded whatever the locale.
    std::string json;
    json.reserve(text.size() + 1);
    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        return text.substr(start, i - start);
    };
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        if (text[i] == '-') {
            json.push_back('-');
        }
        ++i;
    }
    std::string_view whole = digits();
    std::string_view fraction;
    if (i < text.size() && text[i] == '.') {
        ++i;
        fraction = digits();
    }
    if (whole.empty() && fraction.empty()) {
        return std::nullopt;
    }
    while (whole.size() > 1 && whole.front() == '0') {
        whole.remove_prefix(1);
    }
    json.append(whole.empty() ? "0" : whole);
    if (!fraction.empty()) {
        json.append(".").append(fraction);
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        json.push_back('e');
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            json.push_back(text[i]);
            ++i;
        }
        const std::string_view exponent = digits();
        if (exponent.empty()) {
            return std::nullopt;
        }
        json.append(exponent);
    }
    if (i != text.size()) {
        return std::nullopt;
    }
    yyjson_val value{};
    if (yyjson_read_number(json.c_str(), &value, YYJSON_READ_NOFLAG, nullptr, nullptr) == nullptr) {
        return std::nullopt;
    }
    return yyjson_get_num(&value);
}

std::string base64_encode(std::string_view bytes) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        std::uint32_t group = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
                              << 16U;
        if (left > 1) {
            group |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8U;
        }
        if (left > 2) {
            group |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        }
        out += kAlphabet[(group >> 18U) & 63U];
        out += kAlphabet[(group >> 12U) & 63U];
        out += left > 1 ? kAlphabet[(group >> 6U) & 63U] : '=';
        out += left > 2 ? kAlphabet[group & 63U] : '=';
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    const auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        return c == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t group = 0;
    std::size_t bits = 0;
    for (const char c : text) {
        const int value = sextet(c);
        if (value < 0) {
            return std::nullopt;
        }
        group = (group << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((group >> bits) & 0xFFU);
        }
    }
    return out;
}

} // namespace slash
