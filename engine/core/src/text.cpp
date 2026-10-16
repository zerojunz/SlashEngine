#include "slash/core/text.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

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

} // namespace slash
