#include "slash/text/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// A TrueType font written byte by byte (OpenType's cmap, glyf, head, hhea, hmtx, loca, maxp,
// name, OS/2 and post tables), whose glyphs are boxes.
namespace slash::text {

namespace {

constexpr int kUnitsPerEm = 1000;
constexpr int kAscender = 800;
constexpr int kDescender = -200;
constexpr int kAdvance = 600;
constexpr int kSpaceAdvance = 250;
constexpr int kBoxLeft = 50;
constexpr int kBoxRight = 550;
constexpr int kBoxTop = 700;
constexpr int kNotdefInset = 60;
constexpr std::uint32_t kChecksumMagic = 0xB1B0AFBA;

// Big-endian bytes.
struct Writer {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint32_t value) { bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU)); }
    void u16(std::uint32_t value) {
        u8(value >> 8U);
        u8(value);
    }
    void i16(int value) { u16(static_cast<std::uint32_t>(static_cast<std::uint16_t>(value))); }
    void u32(std::uint32_t value) {
        u16(value >> 16U);
        u16(value);
    }
    void tag(std::string_view name) {
        for (const char c : name) {
            u8(static_cast<std::uint8_t>(c));
        }
    }
    void zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
    void pad4() { zeros((4 - (bytes.size() % 4)) % 4); }
    void put(std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<std::uint8_t>((value >> (24U - (8U * i))) & 0xFFU);
        }
    }
};

std::uint32_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            word = (word << 8U) | (i + b < bytes.size() ? bytes[i + b] : 0U);
        }
        sum += word;
    }
    return sum;
}

bool is_blank(char32_t c) {
    return c == U' ' || c == U' ' || c == U'　' || c == U'\t';
}

int advance_of(char32_t c) {
    if (c == U'　') {
        return kUnitsPerEm;
    }
    return is_blank(c) ? kSpaceAdvance : kAdvance;
}

// One contour per box, clockwise for a filled one and counterclockwise for a hole.
void box_contour(Writer& points_x, Writer& points_y, int& x, int& y, std::array<int, 4> box,
                 bool clockwise) {
    const auto [left, bottom, right, top] = box;
    std::array<std::pair<int, int>, 4> corners{
        {{left, bottom}, {left, top}, {right, top}, {right, bottom}}};
    if (!clockwise) {
        std::swap(corners[1], corners[3]);
    }
    for (const auto& [cx, cy] : corners) {
        points_x.i16(cx - x);
        points_y.i16(cy - y);
        x = cx;
        y = cy;
    }
}

// A simple glyph of boxes: filled ones, or a hollow one for .notdef.
std::vector<std::uint8_t> glyph(bool hollow) {
    Writer out;
    const int contours = hollow ? 2 : 1;
    out.i16(contours);
    out.i16(kBoxLeft);
    out.i16(0);
    out.i16(kBoxRight);
    out.i16(kBoxTop);
    out.u16(3);
    if (hollow) {
        out.u16(7);
    }
    out.u16(0); // no instructions
    for (int i = 0; i < contours * 4; ++i) {
        out.u8(0x01); // on the curve; x and y as 16-bit deltas
    }
    Writer xs;
    Writer ys;
    int x = 0;
    int y = 0;
    box_contour(xs, ys, x, y, {kBoxLeft, 0, kBoxRight, kBoxTop}, true);
    if (hollow) {
        box_contour(xs, ys, x, y,
                    {kBoxLeft + kNotdefInset, kNotdefInset, kBoxRight - kNotdefInset,
                     kBoxTop - kNotdefInset},
                    false);
    }
    out.bytes.insert(out.bytes.end(), xs.bytes.begin(), xs.bytes.end());
    out.bytes.insert(out.bytes.end(), ys.bytes.begin(), ys.bytes.end());
    out.pad4();
    return out.bytes;
}

void name_table(Writer& out, std::string_view family) {
    const std::array<std::pair<std::uint32_t, std::string>, 4> names{{
        {1, std::string(family)},
        {2, "Regular"},
        {4, std::string(family) + " Regular"},
        {6, std::string(family) + "-Regular"},
    }};
    const auto count = static_cast<std::uint32_t>(names.size());
    out.u16(0);
    out.u16(count);
    out.u16(6 + (12 * count));
    std::uint32_t offset = 0;
    for (const auto& [id, value] : names) {
        out.u16(3);      // Windows
        out.u16(1);      // Unicode BMP
        out.u16(0x0409); // English
        out.u16(id);
        const auto length = static_cast<std::uint32_t>(value.size() * 2);
        out.u16(length);
        out.u16(offset);
        offset += length;
    }
    for (const auto& [id, value] : names) {
        for (const char c : value) {
            out.u16(static_cast<std::uint8_t>(c)); // the names are ASCII
        }
    }
}

} // namespace

std::vector<std::uint8_t> box_font(std::u32string_view characters, std::string_view family) {
    std::vector<char32_t> chars(characters.begin(), characters.end());
    std::ranges::sort(chars);
    const auto [last, end] = std::ranges::unique(chars);
    chars.erase(last, end);
    const auto glyph_count = static_cast<std::uint32_t>(chars.size() + 1);
    const std::vector<std::uint8_t> filled = glyph(false);
    const std::vector<std::uint8_t> hollow = glyph(true);

    std::vector<std::pair<std::string_view, std::vector<std::uint8_t>>> tables;

    // OS/2, version 4.
    {
        Writer t;
        t.u16(4);
        t.i16(kAdvance); // xAvgCharWidth
        t.u16(400);      // usWeightClass
        t.u16(5);        // usWidthClass
        t.u16(0);        // fsType: installable
        for (const int value : {650, 650, 0, 0}) {
            t.i16(value); // subscript size and offset
        }
        for (const int value : {650, 650, 0, 350}) {
            t.i16(value); // superscript size and offset
        }
        t.i16(50);   // yStrikeoutSize
        t.i16(250);  // yStrikeoutPosition
        t.i16(0);    // sFamilyClass
        t.zeros(10); // panose
        t.zeros(16); // ulUnicodeRange1-4
        t.tag("NONE");
        t.u16(0x40); // fsSelection: regular
        const std::uint32_t first =
            chars.empty() ? 0 : std::min<std::uint32_t>(chars.front(), 0xFFFF);
        const std::uint32_t last_char =
            chars.empty() ? 0 : std::min<std::uint32_t>(chars.back(), 0xFFFF);
        t.u16(first);
        t.u16(last_char);
        t.i16(kAscender);
        t.i16(kDescender);
        t.i16(0);
        t.u16(static_cast<std::uint32_t>(kAscender));
        t.u16(static_cast<std::uint32_t>(-kDescender));
        t.u32(1); // ulCodePageRange1: Latin 1
        t.u32(0);
        t.i16(kBoxTop / 2); // sxHeight
        t.i16(kBoxTop);     // sCapHeight
        t.u16(0);
        t.u16(0x20);
        t.u16(1);
        tables.emplace_back("OS/2", std::move(t.bytes));
    }

    // cmap: one format 12 subtable for Unicode, under both Unicode and Windows platform IDs.
    {
        Writer t;
        t.u16(0);
        t.u16(2);
        t.u16(0); // Unicode
        t.u16(4); // full repertoire
        t.u32(20);
        t.u16(3);  // Windows
        t.u16(10); // UCS-4
        t.u32(20);
        t.u16(12);
        t.u16(0);
        t.u32(16 + (12 * static_cast<std::uint32_t>(chars.size())));
        t.u32(0);
        t.u32(static_cast<std::uint32_t>(chars.size()));
        for (std::size_t i = 0; i < chars.size(); ++i) {
            t.u32(chars[i]);
            t.u32(chars[i]);
            t.u32(static_cast<std::uint32_t>(i + 1));
        }
        tables.emplace_back("cmap", std::move(t.bytes));
    }

    // glyf and loca (long offsets): .notdef, then a box per character, none for blanks.
    Writer glyf;
    Writer loca;
    loca.u32(0);
    glyf.bytes = hollow;
    loca.u32(static_cast<std::uint32_t>(glyf.bytes.size()));
    for (const char32_t c : chars) {
        if (!is_blank(c)) {
            glyf.bytes.insert(glyf.bytes.end(), filled.begin(), filled.end());
        }
        loca.u32(static_cast<std::uint32_t>(glyf.bytes.size()));
    }
    tables.emplace_back("glyf", std::move(glyf.bytes));

    // head; its checkSumAdjustment is filled in last.
    {
        Writer t;
        t.u32(0x00010000);
        t.u32(0x00010000);
        t.u32(0);          // checkSumAdjustment
        t.u32(0x5F0F3CF5); // magicNumber
        t.u16(0x0003);     // flags: baseline at y 0, left side bearing at x 0
        t.u16(kUnitsPerEm);
        t.zeros(16); // created and modified
        t.i16(kBoxLeft);
        t.i16(0);
        t.i16(kBoxRight);
        t.i16(kBoxTop);
        t.u16(0); // macStyle
        t.u16(8); // lowestRecPPEM
        t.i16(2); // fontDirectionHint
        t.i16(1); // indexToLocFormat: long
        t.i16(0);
        tables.emplace_back("head", std::move(t.bytes));
    }

    {
        Writer t;
        t.u32(0x00010000);
        t.i16(kAscender);
        t.i16(kDescender);
        t.i16(0);
        t.u16(kUnitsPerEm); // advanceWidthMax
        t.i16(0);           // minLeftSideBearing
        t.i16(0);           // minRightSideBearing
        t.i16(kBoxRight);   // xMaxExtent
        t.i16(1);           // caretSlopeRise
        t.i16(0);
        t.i16(0);
        t.zeros(8);
        t.i16(0);
        t.u16(glyph_count);
        tables.emplace_back("hhea", std::move(t.bytes));
    }

    {
        Writer t;
        t.u16(kAdvance);
        t.i16(kBoxLeft);
        for (const char32_t c : chars) {
            t.u16(static_cast<std::uint32_t>(advance_of(c)));
            t.i16(is_blank(c) ? 0 : kBoxLeft);
        }
        tables.emplace_back("hmtx", std::move(t.bytes));
    }

    tables.emplace_back("loca", std::move(loca.bytes));

    {
        Writer t;
        t.u32(0x00010000);
        t.u16(glyph_count);
        t.u16(8); // maxPoints
        t.u16(2); // maxContours
        t.u16(0);
        t.u16(0);
        t.u16(2); // maxZones
        t.zeros(16);
        tables.emplace_back("maxp", std::move(t.bytes));
    }

    {
        Writer t;
        name_table(t, family);
        tables.emplace_back("name", std::move(t.bytes));
    }

    {
        Writer t;
        t.u32(0x00030000); // no glyph names
        t.u32(0);          // italicAngle
        t.i16(-100);       // underlinePosition
        t.i16(50);         // underlineThickness
        t.zeros(20);
        tables.emplace_back("post", std::move(t.bytes));
    }

    std::ranges::sort(tables, {}, &std::pair<std::string_view, std::vector<std::uint8_t>>::first);
    Writer font;
    const auto count = static_cast<std::uint32_t>(tables.size());
    std::uint32_t power = 1;
    std::uint32_t log2 = 0;
    while (power * 2 <= count) {
        power *= 2;
        ++log2;
    }
    font.u32(0x00010000);
    font.u16(count);
    font.u16(power * 16);
    font.u16(log2);
    font.u16((count * 16) - (power * 16));
    std::uint32_t offset = 12 + (16 * count);
    std::size_t head_at = 0;
    for (const auto& [name, bytes] : tables) {
        font.tag(name);
        font.u32(checksum(bytes));
        font.u32(offset);
        font.u32(static_cast<std::uint32_t>(bytes.size()));
        if (name == "head") {
            head_at = offset;
        }
        offset += static_cast<std::uint32_t>((bytes.size() + 3) / 4 * 4);
    }
    for (const auto& [name, bytes] : tables) {
        font.bytes.insert(font.bytes.end(), bytes.begin(), bytes.end());
        font.pad4();
    }
    font.put(head_at + 8, kChecksumMagic - checksum(font.bytes));
    return std::move(font.bytes);
}

} // namespace slash::text
