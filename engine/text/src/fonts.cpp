#include "slash/platform/fonts.hpp"

#include "slash/platform/file.hpp"
#include "slash/text/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// FreeType's headers are included through macros, its documented way.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include FT_TRUETYPE_TABLES_H

#include <SheenBidi/SheenBidi.h>
#include <hb.h>
#include <msdfgen.h>

namespace slash::text {
namespace {

// Opens a scalable font face from bytes, which must outlive it: the face at index in a collection.
bool open_face(FT_Library library, const std::string& bytes, FT_Face& face, FT_Long index = 0) {
    if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(bytes.data()),
                           static_cast<FT_Long>(bytes.size()), index, &face) != 0) {
        face = nullptr;
        return false;
    }
    if ((face->face_flags & FT_FACE_FLAG_SCALABLE) == 0) {
        FT_Done_Face(face);
        face = nullptr;
        return false;
    }
    return true;
}

// Atlas pages start at this size; a glyph too big for one gets a page of its own.
constexpr std::uint32_t kPageSize = 1024;
// Shaped paragraphs kept for reuse; the cache starts again when it fills.
constexpr std::size_t kShapeCacheSize = 1024;
// FreeType's 26.6 fixed point.
constexpr double kUnitsPerPixel = 64.0;
// How deep fallbacks of fallbacks go, as Godot's limit on font fallback recursion.
constexpr int kMaxFallbackDepth = 64;
// Installed fonts tried, one after another, for the characters of a run no font of its chain has.
constexpr std::size_t kMaxSystemFaces = 4;

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::size_t utf8_length(unsigned char first) {
    if (first >= 0xF0U) {
        return 4;
    }
    if (first >= 0xE0U) {
        return 3;
    }
    if (first >= 0xC0U) {
        return 2;
    }
    return 1;
}

// The character at byte i of UTF-8 text; malformed bytes read as U+FFFD.
char32_t char_at(std::string_view text, std::size_t i) {
    const auto first = static_cast<unsigned char>(text[i]);
    const std::size_t length = utf8_length(first);
    if (length == 1 || i + length > text.size()) {
        return first < 0x80U ? char32_t{first} : char32_t{0xFFFD};
    }
    char32_t code = first & (0xFFU >> (length + 1));
    for (std::size_t k = 1; k < length; ++k) {
        const auto byte = static_cast<unsigned char>(text[i + k]);
        if ((byte & 0xC0U) != 0x80U) {
            return 0xFFFD;
        }
        code = (code << 6U) | (byte & 0x3FU);
    }
    return code;
}

void append_utf8(std::string& out, char32_t code) {
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

// The start of the character before byte i.
std::size_t previous_char(std::string_view text, std::size_t i) {
    std::size_t at = i;
    while (at > 0) {
        --at;
        if ((static_cast<unsigned char>(text[at]) & 0xC0U) != 0x80U) {
            break;
        }
    }
    return at;
}

// Whitespace that lines break after and justified lines stretch.
bool is_space(char32_t c) {
    return c == U' ' || c == U'\t' || c == 0x1680U || (c >= 0x2000U && c <= 0x2006U) ||
           (c >= 0x2008U && c <= 0x200AU) || c == 0x205FU || c == 0x3000U;
}

// Unicode's Default_Ignorable_Code_Point characters, which draw nothing in any font: soft
// hyphens, joiners, direction marks, variation selectors and the like.
bool is_default_ignorable(char32_t c) {
    return c == 0xADU || c == 0x34FU || c == 0x61CU || c == 0x115FU || c == 0x1160U ||
           c == 0x17B4U || c == 0x17B5U || (c >= 0x180BU && c <= 0x180FU) ||
           (c >= 0x200BU && c <= 0x200FU) || (c >= 0x202AU && c <= 0x202EU) ||
           (c >= 0x2060U && c <= 0x206FU) || c == 0x3164U || (c >= 0xFE00U && c <= 0xFE0FU) ||
           c == 0xFEFFU || c == 0xFFA0U || (c >= 0xFFF0U && c <= 0xFFF8U) ||
           (c >= 0x1BCA0U && c <= 0x1BCA3U) || (c >= 0x1D173U && c <= 0x1D17AU) ||
           (c >= 0xE0000U && c <= 0xE0FFFU);
}

// Characters a font must have a glyph for to draw: not whitespace, controls or ignorables.
bool is_graphic(char32_t c) {
    return !is_space(c) && c >= 0x20U && (c < 0x7FU || c >= 0xA0U) && !is_default_ignorable(c);
}

// The Unicode Bidirectional Algorithm's isolate controls, which rule L1 treats as whitespace.
bool is_isolate(char32_t c) {
    return c >= 0x2066U && c <= 0x2069U;
}

// Characters that keep their neighbors on one line: no-break spaces and the word joiner.
bool is_glue(char32_t c) {
    return c == 0xA0U || c == 0x202FU || c == 0x2007U || c == 0x2060U || c == 0xFEFFU;
}

// Ideographs, kana, Hangul and full-width forms, which lines may break between.
bool is_cjk(char32_t c) {
    return (c >= 0x2E80U && c <= 0x2FFFU) || (c >= 0x3001U && c <= 0x9FFFU) ||
           (c >= 0xAC00U && c <= 0xD7AFU) || (c >= 0xF900U && c <= 0xFAFFU) ||
           (c >= 0xFF01U && c <= 0xFFEFU) || (c >= 0x20000U && c <= 0x3FFFFU);
}

// Characters a line may not start with (Japanese kinsoku shori): closing brackets, small kana
// and punctuation.
bool no_line_start(char32_t c) {
    constexpr std::u32string_view kClosing = U"!%),.:;?]}¢°’”‰′″℃、。々"
                                             U"〉》」』】〕〗〙〛〟ぁぃぅ"
                                             U"ぇぉっゃゅょゎゕゖゝゞ゠ァ"
                                             U"ィゥェォッャュョヮヵヶ・ー"
                                             U"ヽヾ！％），．：；？］｝｡"
                                             U"｣､･ｧｨｩｪｫｬｭｮｯｰ";
    return kClosing.find(c) != std::u32string_view::npos;
}

// Characters a line may not end with: opening brackets.
bool no_line_end(char32_t c) {
    constexpr std::u32string_view kOpening = U"([{‘“〈《「『【"
                                             U"〔〖〘〚〝（［"
                                             U"｛｢";
    return kOpening.find(c) != std::u32string_view::npos;
}

bool is_letter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
           (c >= 0xC0U && c != 0xD7U && c != 0xF7U && !is_space(c) && !is_glue(c) && !is_cjk(c));
}

// Whether a line may break between two characters: after spaces, after a hyphen inside a word,
// and next to ideographs and kana unless a closing mark would start the line.
bool may_break(char32_t before, char32_t after) {
    if (is_glue(before) || is_glue(after) || is_space(after)) {
        return false;
    }
    if (is_space(before) || before == 0x200BU) {
        return true;
    }
    if ((before == U'-' || before == 0x2010U || before == 0x2013U) && is_letter(after)) {
        return true;
    }
    if (is_cjk(before) || is_cjk(after)) {
        return !no_line_start(after) && !no_line_end(before);
    }
    return false;
}

// The byte length of a line break at byte i, or 0: "\r\n", "\n", "\r", U+0085, U+2028, U+2029.
std::size_t line_break_at(std::string_view text, std::size_t i) {
    const char c = text[i];
    if (c == '\r') {
        return i + 1 < text.size() && text[i + 1] == '\n' ? 2 : 1;
    }
    if (c == '\n') {
        return 1;
    }
    if (text.substr(i, 2) == "\xC2\x85") {
        return 2;
    }
    if (text.substr(i, 3) == "\xE2\x80\xA8" || text.substr(i, 3) == "\xE2\x80\xA9") {
        return 3;
    }
    return 0;
}

std::uint32_t to_u32(std::size_t value) {
    return static_cast<std::uint32_t>(value);
}

struct Face {
    std::shared_ptr<const FontData> data; // keeps the bytes FreeType and HarfBuzz read
    FontSettings settings;
    std::string path;    // the font file: a project path, "" for the default font, or installed
    bool system = false; // installed on the machine
    FT_Face ft = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* hb_face = nullptr;
    hb_font_t* hb_font = nullptr;
    std::map<int, FontMetrics> metrics;

    Face() = default;
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;
    Face(Face&&) = delete;
    Face& operator=(Face&&) = delete;
    ~Face() {
        hb_font_destroy(hb_font);
        hb_face_destroy(hb_face);
        hb_blob_destroy(blob);
        if (ft != nullptr) {
            FT_Done_Face(ft);
        }
    }

    [[nodiscard]] bool msdf() const { return settings.multichannel_signed_distance_field; }
};

// A font path's files, as last loaded, and the face made from them.
struct Source {
    asset::Handle<FontSettings> settings;
    asset::Handle<FontData> data;
    std::uint64_t settings_version = 0;
    std::uint64_t data_version = 0;
    std::uint32_t face = 0;
};

struct Shelf {
    std::uint32_t y = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
};

// A run of a paragraph's text in one direction and script, shaped as one.
struct Run {
    std::size_t start = 0;
    std::size_t end = 0;
    std::uint8_t level = 0; // its bidi embedding level: odd goes right to left
    hb_script_t script = HB_SCRIPT_COMMON;
};

// A paragraph shaped: its glyphs in the order of its text, each with its x offset only, and the
// bidi embedding level of each of its bytes.
struct Shaped {
    std::vector<Glyph> glyphs;
    std::vector<std::uint8_t> levels;
    std::uint8_t base = 0; // the paragraph's level: 1 when it goes right to left
};

// A font and its fallbacks, depth first, each face once.
struct Chain {
    std::vector<std::uint32_t> faces;
    bool system = false; // installed fonts may stand in when none of them has a character
};

// What makes two shapings of a paragraph the same.
using ShapeKey =
    std::tuple<std::vector<std::uint32_t>, bool, int, Direction, std::string, std::string>;

// Whether a font is for a language or script, by its support overrides: the code, then a BCP 47
// code's language alone ("zh" for "zh-Hant"), then "*"; yes when none is listed.
bool supported(const std::map<std::string, bool, std::less<>>& overrides, std::string_view code) {
    if (overrides.empty() || code.empty()) {
        return true;
    }
    if (const auto found = overrides.find(code); found != overrides.end()) {
        return found->second;
    }
    if (const std::size_t dash = code.find('-'); dash != std::string_view::npos) {
        if (const auto found = overrides.find(code.substr(0, dash)); found != overrides.end()) {
            return found->second;
        }
    }
    if (const auto found = overrides.find("*"); found != overrides.end()) {
        return found->second;
    }
    return true;
}

// A script's ISO 15924 code, such as "Arab".
std::string script_code(hb_script_t script) {
    std::array<char, 5> tag{};
    hb_tag_to_string(hb_script_to_iso15924_tag(script), tag.data());
    return {tag.data(), 4};
}

// Glyphs sharing a cluster: what a line breaks between.
struct Cluster {
    std::size_t first = 0; // glyph
    std::size_t last = 0;  // one past
    std::size_t start = 0; // byte
    float advance = 0.0F;
    bool space = false;
    bool may_break_before = false;
};

// The outline of a glyph as msdfgen shapes it.
struct ShapeBuilder {
    msdfgen::Shape* shape = nullptr;
    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 position;

    static msdfgen::Point2 point(const FT_Vector* v) {
        return {static_cast<double>(v->x) / kUnitsPerPixel,
                static_cast<double>(v->y) / kUnitsPerPixel};
    }
    static ShapeBuilder& of(void* user) { return *static_cast<ShapeBuilder*>(user); }
    static int move_to(const FT_Vector* to, void* user) {
        ShapeBuilder& b = of(user);
        if (b.contour == nullptr || !b.contour->edges.empty()) {
            b.contour = &b.shape->addContour();
        }
        b.position = point(to);
        return 0;
    }
    static int line_to(const FT_Vector* to, void* user) {
        ShapeBuilder& b = of(user);
        const msdfgen::Point2 end = point(to);
        if (end != b.position) {
            b.contour->addEdge(msdfgen::EdgeHolder(b.position, end));
            b.position = end;
        }
        return 0;
    }
    static int conic_to(const FT_Vector* control, const FT_Vector* to, void* user) {
        ShapeBuilder& b = of(user);
        const msdfgen::Point2 end = point(to);
        b.contour->addEdge(msdfgen::EdgeHolder(b.position, point(control), end));
        b.position = end;
        return 0;
    }
    static int cubic_to(const FT_Vector* first, const FT_Vector* second, const FT_Vector* to,
                        void* user) {
        ShapeBuilder& b = of(user);
        const msdfgen::Point2 end = point(to);
        b.contour->addEdge(msdfgen::EdgeHolder(b.position, point(first), point(second), end));
        b.position = end;
        return 0;
    }
};

// Coverage, or a 1-bit mask, as 8-bit alpha in rows from the top.
std::vector<std::uint8_t> coverage(const FT_Bitmap& bitmap) {
    const std::uint32_t width = bitmap.width;
    const std::uint32_t rows = bitmap.rows;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * rows);
    const int pitch = bitmap.pitch;
    for (std::uint32_t y = 0; y < rows; ++y) {
        const unsigned char* row =
            bitmap.buffer + (static_cast<std::ptrdiff_t>(pitch) * static_cast<std::ptrdiff_t>(y));
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t value = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                value = (row[x / 8] & (0x80U >> (x % 8))) != 0 ? 255 : 0;
            } else {
                value = row[x];
            }
            out[(static_cast<std::size_t>(y) * width) + x] = value;
        }
    }
    return out;
}

} // namespace

struct Fonts::Impl {
    asset::Assets* assets = nullptr;
    FT_Library library = nullptr;
    FT_Stroker stroker = nullptr;
    hb_buffer_t* buffer = nullptr;
    hb_language_t default_language = nullptr; // "und", for text with no language
    std::vector<std::unique_ptr<Face>> faces;
    std::map<std::string, Source, std::less<>> sources;
    std::vector<AtlasPage> pages;
    std::vector<std::vector<Shelf>> shelves;
    std::vector<std::uint32_t> page_bottoms; // where the next shelf of each page goes
    std::map<std::tuple<std::uint32_t, std::uint32_t, int, int>, std::optional<GlyphImage>> glyphs;
    std::size_t glyph_images = 0;
    std::map<ShapeKey, Shaped> shaped;
    bool system_fallback = true;
    // Installed font files by path, read when first needed; nullptr for one that didn't read.
    std::map<std::string, std::shared_ptr<const FontData>, std::less<>> system_files;
    // Faces of installed fonts by file and index in it.
    std::map<std::pair<std::string, int>, std::uint32_t> system_faces;
    // The installed face found for a character in a language and script, or nullopt for none.
    std::map<std::tuple<char32_t, std::string, std::string>, std::optional<std::uint32_t>>
        system_lookups;

    Impl() {
        FT_Init_FreeType(&library);
        FT_Stroker_New(library, &stroker);
        buffer = hb_buffer_create();
        // A fixed language, so shaping is the same whatever the machine's locale.
        default_language = hb_language_from_string("und", -1);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        faces.clear();
        hb_buffer_destroy(buffer);
        FT_Stroker_Done(stroker);
        FT_Done_FreeType(library);
    }

    Result<std::uint32_t> make_face(std::shared_ptr<const FontData> data, FontSettings settings,
                                    std::string_view path, std::string_view name, int index = 0) {
        auto face = std::make_unique<Face>();
        face->data = std::move(data);
        face->settings = std::move(settings);
        face->path = std::string(path);
        const std::string& bytes = face->data->bytes;
        if (!open_face(library, bytes, face->ft, index)) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(name) + " is not a TrueType or OpenType font");
        }
        face->blob = hb_blob_create(bytes.data(), static_cast<unsigned int>(bytes.size()),
                                    HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        face->hb_face = hb_face_create(face->blob, static_cast<unsigned int>(index));
        face->hb_font = hb_font_create(face->hb_face);
        faces.push_back(std::move(face));
        return to_u32(faces.size() - 1);
    }

    // The face for a font path, made again when its files changed.
    Result<std::uint32_t> face_for(std::string_view path) {
        if (path.empty()) {
            auto found = sources.find(path);
            if (found != sources.end()) {
                return found->second.face;
            }
            const std::span<const std::uint8_t> bytes = default_font();
            auto data = std::make_shared<const FontData>(
                FontData{std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
            auto face = make_face(std::move(data), {}, "", "the default font");
            if (!face) {
                return face.error();
            }
            Source source;
            source.face = face.value();
            sources.emplace(std::string(), std::move(source));
            return face;
        }
        if (!is_font_path(path)) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(path) +
                                  "\" is not a .ttf, .otf or .font.json file");
        }
        Source source;
        FontSettings settings;
        std::string data_path(path);
        if (ends_with(path, kFontSuffix)) {
            auto loaded = assets->load<FontSettings>(path);
            if (!loaded) {
                return loaded.error();
            }
            source.settings = std::move(loaded).value();
            source.settings_version = source.settings.version();
            settings = *source.settings;
            data_path = settings.font;
        }
        auto data = assets->load<FontData>(data_path);
        if (!data) {
            return data.error();
        }
        source.data = std::move(data).value();
        source.data_version = source.data.version();
        auto found = sources.find(path);
        if (found != sources.end() && found->second.settings_version == source.settings_version &&
            found->second.data_version == source.data_version &&
            found->second.data.path() == source.data.path()) {
            return found->second.face;
        }
        auto face = make_face(source.data.share(), std::move(settings), data_path, data_path);
        if (!face) {
            return face.error();
        }
        source.face = face.value();
        sources.insert_or_assign(std::string(path), std::move(source));
        return face;
    }

    void set_size(Face& face, int size) const {
        FT_Set_Pixel_Sizes(face.ft, 0, static_cast<FT_UInt>(size));
    }

    FontMetrics metrics(Face& face, int size) const {
        const auto found = face.metrics.find(size);
        if (found != face.metrics.end()) {
            return found->second;
        }
        // Distance fields scale the metrics of msdf_size, as Godot's do.
        const int at = face.msdf() ? face.settings.msdf_size : size;
        set_size(face, at);
        const double scale = face.msdf() ? static_cast<double>(size) / at : 1.0;
        const FT_Size_Metrics& m = face.ft->size->metrics;
        const FontMetrics out{
            .ascent = static_cast<float>(static_cast<double>(m.ascender) / kUnitsPerPixel * scale),
            .descent =
                static_cast<float>(static_cast<double>(-m.descender) / kUnitsPerPixel * scale)};
        face.metrics.emplace(size, out);
        return out;
    }

    // A font path's face followed by its fallbacks', depth first, each face once. A fallback
    // that doesn't load fails the chain, naming the file that lists it.
    Result<void> add_to_chain(Chain& chain, std::string_view path, std::string_view listed_by,
                              std::vector<std::string>& visited, int depth) {
        if (std::ranges::find(visited, path) != visited.end()) {
            return {};
        }
        visited.emplace_back(path);
        auto face = face_for(path);
        if (!face) {
            if (listed_by.empty()) {
                return face.error();
            }
            return make_error(face.error().code, "fallback " + std::string(path) + " of " +
                                                     std::string(listed_by) + ": " +
                                                     face.error().message);
        }
        if (std::ranges::find(chain.faces, face.value()) == chain.faces.end()) {
            chain.faces.push_back(face.value());
        }
        if (depth >= kMaxFallbackDepth) {
            return {};
        }
        const std::vector<std::string> fallbacks = faces[face.value()]->settings.fallbacks;
        for (const std::string& fallback : fallbacks) {
            if (auto added = add_to_chain(chain, fallback, path, visited, depth + 1); !added) {
                return added.error();
            }
        }
        return {};
    }

    Result<Chain> chain_for(std::string_view path) {
        Chain chain;
        std::vector<std::string> visited;
        if (auto added = add_to_chain(chain, path, "", visited, 0); !added) {
            return added.error();
        }
        chain.system = system_fallback && faces[chain.faces[0]]->settings.allow_system_fallback;
        return chain;
    }

    // A chain in the order its fonts are tried for a run, as Godot's FontPriorityList: the first
    // font, then fallbacks for the script and language, those for the script only, then the rest.
    [[nodiscard]] std::vector<std::uint32_t> ordered(const Chain& chain, std::string_view script,
                                                     std::string_view language) const {
        std::vector<std::uint32_t> out{chain.faces[0]};
        const auto priority = [&](const Face& face) {
            if (!supported(face.settings.script_support, script)) {
                return 2;
            }
            return supported(face.settings.language_support, language) ? 0 : 1;
        };
        for (int level = 0; level <= 2; ++level) {
            for (std::size_t i = 1; i < chain.faces.size(); ++i) {
                if (priority(*faces[chain.faces[i]]) == level) {
                    out.push_back(chain.faces[i]);
                }
            }
        }
        return out;
    }

    // An installed font file's bytes, read once.
    std::shared_ptr<const FontData> system_file(const std::string& path) {
        if (const auto found = system_files.find(path); found != system_files.end()) {
            return found->second;
        }
        std::shared_ptr<const FontData> data;
        if (auto bytes = platform::read_file(path)) {
            data = std::make_shared<const FontData>(FontData{std::move(bytes).value()});
        }
        system_files.emplace(path, data);
        return data;
    }

    // The face of an installed font file, or of a collection, that has a character: the most
    // regular one (upright, weight nearest 400), as Godot picks a system font's face.
    std::optional<std::uint32_t> system_face_in(const std::string& path, char32_t character) {
        const std::shared_ptr<const FontData> data = system_file(path);
        if (!data) {
            return std::nullopt;
        }
        FT_Face probe = nullptr;
        if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(data->bytes.data()),
                               static_cast<FT_Long>(data->bytes.size()), -1, &probe) != 0) {
            return std::nullopt;
        }
        const FT_Long count = probe->num_faces;
        FT_Done_Face(probe);
        int best = -1;
        int best_score = -1;
        for (FT_Long index = 0; index < count; ++index) {
            FT_Face face = nullptr;
            if (!open_face(library, data->bytes, face, index)) {
                continue;
            }
            if (FT_Get_Char_Index(face, character) != 0) {
                const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
                const int weight = os2 != nullptr ? static_cast<int>(os2->usWeightClass) : 400;
                int score = 20 - (std::abs(weight - 400) / 50);
                if ((face->style_flags & FT_STYLE_FLAG_ITALIC) == 0) {
                    score += 30;
                }
                if (score > best_score) {
                    best_score = score;
                    best = static_cast<int>(index);
                }
            }
            FT_Done_Face(face);
        }
        if (best < 0) {
            return std::nullopt;
        }
        const auto key = std::make_pair(path, best);
        if (const auto found = system_faces.find(key); found != system_faces.end()) {
            return found->second;
        }
        auto face = make_face(data, {}, path, path, best);
        if (!face) {
            return std::nullopt;
        }
        faces[face.value()]->system = true;
        system_faces.emplace(key, face.value());
        return face.value();
    }

    // The installed face the platform picks for a character in a language and script.
    std::optional<std::uint32_t> system_face(char32_t character, const std::string& language,
                                             const std::string& script) {
        const auto key = std::make_tuple(character, language, script);
        if (const auto found = system_lookups.find(key); found != system_lookups.end()) {
            return found->second;
        }
        std::string utf8;
        append_utf8(utf8, character);
        std::optional<std::uint32_t> out;
        for (const std::string& path : platform::system_fonts_for(utf8, language, script)) {
            out = system_face_in(path, character);
            if (out) {
                break;
            }
        }
        system_lookups.emplace(key, out);
        return out;
    }

    // Glyphs of text[start, end) shaped with one face in one direction, in the order of the text,
    // each with its x offset; the rest of the text is context for joining.
    std::vector<Glyph> shape_with(std::uint32_t face_id, int size, std::string_view text,
                                  std::size_t start, std::size_t end, bool rtl, hb_script_t script,
                                  hb_language_t lang) {
        const Face& face = *faces[face_id];
        const bool round = !face.msdf();
        hb_font_set_scale(face.hb_font, size * 64, size * 64);
        hb_buffer_clear_contents(buffer);
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                           static_cast<unsigned int>(start), static_cast<int>(end - start));
        hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, script);
        hb_buffer_set_language(buffer, lang);
        unsigned int flags = HB_BUFFER_FLAG_DEFAULT;
        if (start == 0) {
            flags |= HB_BUFFER_FLAG_BOT;
        }
        if (end == text.size()) {
            flags |= HB_BUFFER_FLAG_EOT;
        }
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): HarfBuzz's flags combine.
        hb_buffer_set_flags(buffer, static_cast<hb_buffer_flags_t>(flags));
        hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_shape(face.hb_font, buffer, nullptr, 0);
        unsigned int count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
        // Glyph images draw at whole pixels, so their advances are whole too, as Godot's are
        // without subpixel positioning.
        const auto pixels = [round](hb_position_t value) {
            const double exact = static_cast<double>(value) / kUnitsPerPixel;
            return static_cast<float>(round ? std::round(exact) : exact);
        };
        std::vector<Glyph> out;
        out.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            Glyph glyph;
            glyph.face = face_id;
            glyph.index = infos[i].codepoint;
            glyph.cluster = infos[i].cluster;
            glyph.x = pixels(positions[i].x_offset);
            glyph.y = -pixels(positions[i].y_offset);
            glyph.advance = pixels(positions[i].x_advance);
            glyph.space = is_space(char_at(text, infos[i].cluster));
            glyph.rtl = rtl;
            out.push_back(glyph);
        }
        // Right-to-left runs come out in display order; lines break in text order.
        if (rtl) {
            std::ranges::reverse(out);
        }
        return out;
    }

    // What one run's shaping needs to know.
    struct RunContext {
        std::string_view text;
        int size = 0;
        bool rtl = false;
        hb_script_t script = HB_SCRIPT_COMMON;
        hb_language_t lang = nullptr;
        std::string language;
        std::string script_code;
        std::vector<std::uint32_t> fonts; // tried in order
        bool system = false;
        std::vector<std::uint32_t> tried; // faces shaping text[start, end) now, outermost first
    };

    // Shapes text[start, end) with the run's fonts from the one at index on, as Godot's
    // _shape_run: characters a font has no glyph for are shaped again with the next font, then
    // with installed fonts; what no font has is drawn with the first font's missing-glyph box.
    void shape_run(std::vector<Glyph>& out, RunContext& run, std::size_t start, std::size_t end,
                   std::size_t index) {
        std::optional<std::uint32_t> face;
        if (index < run.fonts.size()) {
            face = run.fonts[index];
        } else if (run.system && index < run.fonts.size() + kMaxSystemFaces) {
            face = system_face(char_at(run.text, start), run.language, run.script_code);
            if (face && std::ranges::find(run.tried, *face) != run.tried.end()) {
                face.reset();
            }
        }
        if (!face) {
            std::vector<Glyph> boxes = shape_with(run.fonts[0], run.size, run.text, start, end,
                                                  run.rtl, run.script, run.lang);
            out.insert(out.end(), boxes.begin(), boxes.end());
            return;
        }
        run.tried.push_back(*face);
        const std::vector<Glyph> shaped_glyphs =
            shape_with(*face, run.size, run.text, start, end, run.rtl, run.script, run.lang);
        std::optional<std::size_t> failed; // where the characters this face lacks start
        std::size_t g = 0;
        while (g < shaped_glyphs.size()) {
            std::size_t next = g;
            bool valid = true;
            while (next < shaped_glyphs.size() &&
                   shaped_glyphs[next].cluster == shaped_glyphs[g].cluster) {
                valid = valid && shaped_glyphs[next].index != 0;
                ++next;
            }
            valid = valid || !is_graphic(char_at(run.text, shaped_glyphs[g].cluster));
            if (!valid) {
                if (!failed) {
                    failed = shaped_glyphs[g].cluster;
                }
            } else {
                if (failed) {
                    shape_run(out, run, *failed, shaped_glyphs[g].cluster, index + 1);
                    failed.reset();
                }
                out.insert(out.end(), shaped_glyphs.begin() + static_cast<std::ptrdiff_t>(g),
                           shaped_glyphs.begin() + static_cast<std::ptrdiff_t>(next));
            }
            g = next;
        }
        if (failed) {
            shape_run(out, run, *failed, end, index + 1);
        }
        run.tried.pop_back();
    }

    // The bidi embedding level of each byte of a paragraph, and the paragraph's own level, by the
    // Unicode Bidirectional Algorithm (rules P2 to I2).
    static std::pair<std::vector<std::uint8_t>, std::uint8_t> bidi_levels(std::string_view text,
                                                                          Direction direction) {
        SBLevel request = SBLevelDefaultLTR;
        if (direction == Direction::Ltr) {
            request = 0;
        } else if (direction == Direction::Rtl) {
            request = 1;
        }
        const std::uint8_t fallback = direction == Direction::Rtl ? 1 : 0;
        std::vector<std::uint8_t> levels(text.size(), fallback);
        std::optional<std::uint8_t> base;
        const SBCodepointSequence sequence{SBStringEncodingUTF8, text.data(), text.size()};
        SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
        // Information separators end bidi paragraphs too, so there may be more than one.
        std::size_t offset = 0;
        while (algorithm != nullptr && offset < text.size()) {
            SBParagraphRef paragraph =
                SBAlgorithmCreateParagraph(algorithm, offset, text.size() - offset, request);
            if (paragraph == nullptr) {
                break;
            }
            const std::size_t length = SBParagraphGetLength(paragraph);
            const SBLevel* resolved = SBParagraphGetLevelsPtr(paragraph);
            std::copy_n(resolved, length, levels.begin() + static_cast<std::ptrdiff_t>(offset));
            if (!base) {
                base = SBParagraphGetBaseLevel(paragraph);
            }
            SBParagraphRelease(paragraph);
            offset += std::max<std::size_t>(length, 1);
        }
        if (algorithm != nullptr) {
            SBAlgorithmRelease(algorithm);
        }
        return {std::move(levels), base.value_or(fallback)};
    }

    // A paragraph cut into runs of one level and one script (UAX #24, with brackets and marks
    // joining the run they belong to).
    static std::vector<Run> runs_of(std::string_view text,
                                    const std::vector<std::uint8_t>& levels) {
        std::vector<Run> scripts;
        const SBCodepointSequence sequence{SBStringEncodingUTF8, text.data(), text.size()};
        SBScriptLocatorRef locator = SBScriptLocatorCreate();
        SBScriptLocatorLoadCodepoints(locator, &sequence);
        const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator);
        while (SBScriptLocatorMoveNext(locator) != 0) {
            const hb_script_t script =
                hb_script_from_iso15924_tag(SBScriptGetUnicodeTag(agent->script));
            scripts.push_back({.start = agent->offset,
                               .end = agent->offset + agent->length,
                               .level = 0,
                               .script = script == HB_SCRIPT_INVALID ||
                                                 script == HB_SCRIPT_UNKNOWN ||
                                                 script == HB_SCRIPT_INHERITED
                                             ? HB_SCRIPT_COMMON
                                             : script});
        }
        SBScriptLocatorRelease(locator);
        std::vector<Run> out;
        for (const Run& script : scripts) {
            std::size_t start = script.start;
            for (std::size_t i = script.start + 1; i <= script.end; ++i) {
                if (i == script.end || levels[i] != levels[start]) {
                    out.push_back({.start = start,
                                   .end = i,
                                   .level = levels[start],
                                   .script = script.script});
                    start = i;
                }
            }
        }
        return out;
    }

    // A paragraph's glyphs in the order of its text, clusters from its start, with its levels.
    const Shaped& shape(const Chain& chain, int size, std::string_view text,
                        const LayoutOptions& options) {
        ShapeKey key{chain.faces,       chain.system,     size,
                     options.direction, options.language, std::string(text)};
        if (const auto found = shaped.find(key); found != shaped.end()) {
            return found->second;
        }
        Shaped out;
        auto [levels, base] = bidi_levels(text, options.direction);
        out.levels = std::move(levels);
        out.base = base;
        const hb_language_t lang = options.language.empty()
                                       ? default_language
                                       : hb_language_from_string(options.language.c_str(), -1);
        for (const Run& each : runs_of(text, out.levels)) {
            RunContext run;
            run.text = text;
            run.size = size;
            run.rtl = (each.level % 2U) == 1U;
            run.script = each.script;
            run.lang = lang;
            run.language = options.language;
            run.script_code = script_code(each.script);
            run.fonts = ordered(chain, run.script_code, options.language);
            run.system = chain.system;
            shape_run(out.glyphs, run, each.start, each.end, 0);
        }
        if (shaped.size() >= kShapeCacheSize) {
            shaped.clear();
        }
        return shaped.emplace(std::move(key), std::move(out)).first->second;
    }

    // Text shaped left to right with one face, for an ellipsis.
    std::vector<Glyph> shape_plain(std::uint32_t face_id, int size, std::string_view text) {
        return shape_with(face_id, size, text, 0, text.size(), false, HB_SCRIPT_COMMON,
                          default_language);
    }
    // Breaks one paragraph's glyphs into lines and appends them to the layout.
    static void break_lines(TextLayout& layout, std::string_view text, std::size_t offset,
                            const std::vector<Glyph>& glyphs, bool rtl,
                            const LayoutOptions& options) {
        std::vector<Cluster> clusters;
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            if (clusters.empty() || glyphs[i].cluster != glyphs[clusters.back().first].cluster) {
                clusters.push_back({.first = i,
                                    .last = i,
                                    .start = glyphs[i].cluster,
                                    .advance = 0.0F,
                                    .space = glyphs[i].space,
                                    .may_break_before = false});
            }
            clusters.back().last = i + 1;
            clusters.back().advance += glyphs[i].advance;
        }
        for (std::size_t k = 1; k < clusters.size(); ++k) {
            const char32_t before = char_at(text, previous_char(text, clusters[k].start));
            const char32_t after = char_at(text, clusters[k].start);
            clusters[k].may_break_before = options.autowrap == Autowrap::Arbitrary
                                               ? !is_space(after)
                                               : may_break(before, after);
        }

        const auto emit = [&](std::size_t from, std::size_t to, bool paragraph_end) {
            Line line;
            line.first = to_u32(layout.glyphs.size());
            line.start = to_u32(offset + (from < clusters.size() ? clusters[from].start : 0));
            line.end = to_u32(offset + (to < clusters.size() ? clusters[to].start : text.size()));
            line.paragraph_end = paragraph_end;
            line.rtl = rtl;
            float width = 0.0F;
            float trailing = 0.0F;
            for (std::size_t k = from; k < to; ++k) {
                for (std::size_t g = clusters[k].first; g < clusters[k].last; ++g) {
                    Glyph glyph = glyphs[g];
                    glyph.cluster += to_u32(offset);
                    layout.glyphs.push_back(glyph);
                }
                width += clusters[k].advance;
                trailing = clusters[k].space ? trailing + clusters[k].advance : 0.0F;
            }
            line.count = to_u32(layout.glyphs.size()) - line.first;
            line.width = width - trailing;
            layout.lines.push_back(line);
        };

        const bool wrap = options.autowrap != Autowrap::Off && options.width > 0.0F;
        std::size_t start = 0;
        float width = 0.0F; // of the line so far, trailing spaces included
        std::size_t last_break = 0;
        std::size_t k = 0;
        while (k < clusters.size()) {
            const Cluster& cluster = clusters[k];
            if (k > start && cluster.may_break_before) {
                last_break = k;
            }
            if (wrap && !cluster.space && k > start && width + cluster.advance > options.width) {
                // Arbitrary breaks here; word and word_smart at the last break between words,
                // else word_smart breaks here too.
                std::size_t at = 0;
                if (options.autowrap != Autowrap::Arbitrary && last_break > start) {
                    at = last_break;
                } else if (options.autowrap != Autowrap::Word) {
                    at = k;
                }
                if (at > start) {
                    emit(start, at, false);
                    start = at;
                    last_break = 0;
                    width = 0.0F;
                    for (std::size_t j = start; j < k; ++j) {
                        width += clusters[j].advance;
                    }
                    // Look at this cluster again, on the new line.
                    continue;
                }
            }
            width += cluster.advance;
            ++k;
        }
        emit(start, clusters.size(), true);
    }

    // Puts a line's glyphs in display order and sets where each draws, by the Unicode
    // Bidirectional Algorithm's rules L1 and L2: whitespace at the line's end and before tabs
    // takes the paragraph's level, then from the highest level down to the lowest odd one, each
    // stretch of glyphs at that level or higher is reversed. A right-to-left line's trailing
    // spaces end up at its left, out of its width, so its text starts at 0 as a left-to-right
    // line's does.
    static void place(TextLayout& layout, const Line& line, std::string_view text,
                      std::size_t offset, const Shaped& shaped) {
        const auto begin = layout.glyphs.begin() + line.first;
        std::vector<std::uint8_t> levels(line.count);
        for (std::uint32_t g = 0; g < line.count; ++g) {
            levels[g] = shaped.levels[layout.glyphs[line.first + g].cluster - offset];
        }
        bool trailing = true;
        for (std::uint32_t g = line.count; g-- > 0;) {
            const char32_t c = char_at(text, layout.glyphs[line.first + g].cluster - offset);
            if (c == U'\t') {
                levels[g] = shaped.base;
                trailing = true;
            } else if (trailing && (is_space(c) || is_isolate(c))) {
                levels[g] = shaped.base;
            } else {
                trailing = false;
            }
        }
        // The spaces at the line's end, which its width leaves out.
        float hanging = 0.0F;
        for (std::uint32_t g = line.count; g > 0 && layout.glyphs[line.first + g - 1].space; --g) {
            hanging += layout.glyphs[line.first + g - 1].advance;
        }
        std::uint8_t highest = 0;
        std::uint8_t lowest_odd = 0xFF;
        for (const std::uint8_t level : levels) {
            highest = std::max(highest, level);
            if ((level % 2U) == 1U) {
                lowest_odd = std::min(lowest_odd, level);
            }
        }
        for (int level = highest; level >= lowest_odd && level > 0; --level) {
            std::uint32_t g = 0;
            while (g < line.count) {
                if (levels[g] < level) {
                    ++g;
                    continue;
                }
                std::uint32_t end = g;
                while (end < line.count && levels[end] >= level) {
                    ++end;
                }
                std::reverse(begin + g, begin + end);
                std::reverse(levels.begin() + g, levels.begin() + end);
                g = end;
            }
        }
        float pen = line.rtl ? -hanging : 0.0F;
        for (auto glyph = begin; glyph != begin + line.count; ++glyph) {
            const float x_offset = glyph->x;
            glyph->x = pen + x_offset;
            pen += glyph->advance;
        }
    }

    // Space for a w x h image on a page of the given kind.
    std::pair<std::uint32_t, Shelf> allocate(std::uint32_t w, std::uint32_t h, bool msdf) {
        for (std::uint32_t p = 0; p < pages.size(); ++p) {
            const AtlasPage& page = pages[p];
            if (page.msdf != msdf || w > page.width) {
                continue;
            }
            for (Shelf& shelf : shelves[p]) {
                if (shelf.height >= h && shelf.height <= h + (h / 2) + 2 &&
                    shelf.x + w <= page.width) {
                    const Shelf at{.y = shelf.y, .height = h, .x = shelf.x};
                    shelf.x += w;
                    return {p, at};
                }
            }
            if (page_bottoms[p] + h <= page.height) {
                const Shelf at{.y = page_bottoms[p], .height = h, .x = 0};
                shelves[p].push_back({.y = page_bottoms[p], .height = h, .x = w});
                page_bottoms[p] += h;
                return {p, at};
            }
        }
        AtlasPage page;
        page.width = std::max(kPageSize, ((w + 255U) / 256U) * 256U);
        page.height = std::max(kPageSize, ((h + 255U) / 256U) * 256U);
        page.rgba.assign(static_cast<std::size_t>(page.width) * page.height * 4U, 0);
        page.msdf = msdf;
        if (!msdf) {
            // White under coverage, so a draw's color tints it.
            for (std::size_t i = 0; i < page.rgba.size(); i += 4) {
                page.rgba[i] = 255;
                page.rgba[i + 1] = 255;
                page.rgba[i + 2] = 255;
            }
        }
        pages.push_back(std::move(page));
        shelves.push_back({{.y = 0, .height = h, .x = w}});
        page_bottoms.push_back(h);
        return {to_u32(pages.size() - 1), Shelf{.y = 0, .height = h, .x = 0}};
    }

    // Copies alpha in rows from the top into a glyph image page, inside a one-texel border.
    GlyphImage put_coverage(const std::vector<std::uint8_t>& alpha, std::uint32_t width,
                            std::uint32_t rows, int left, int top) {
        const std::uint32_t w = width + 2;
        const std::uint32_t h = rows + 2;
        const auto [p, at] = allocate(w, h, false);
        AtlasPage& page = pages[p];
        for (std::uint32_t y = 0; y < rows; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t texel =
                    ((static_cast<std::size_t>(at.y + 1 + y) * page.width) + at.x + 1 + x) * 4U;
                page.rgba[texel + 3] = alpha[(static_cast<std::size_t>(y) * width) + x];
            }
        }
        ++page.version;
        return {.page = p,
                .x = at.x,
                .y = at.y,
                .width = w,
                .height = h,
                .left = static_cast<float>(left - 1),
                .top = static_cast<float>(-top - 1),
                .scale = 1.0F};
    }

    std::optional<GlyphImage> rasterize(Face& face, std::uint32_t index, int size, int outline) {
        FT_Int32 flags = FT_LOAD_DEFAULT;
        if (face.settings.antialiasing == Antialiasing::None) {
            flags |=
                face.settings.hinting == Hinting::None ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_MONO;
        } else if (face.settings.hinting == Hinting::None) {
            flags |= FT_LOAD_NO_HINTING;
        } else if (face.settings.hinting == Hinting::Light) {
            flags |= FT_LOAD_TARGET_LIGHT;
        } else {
            flags |= FT_LOAD_TARGET_NORMAL;
        }
        if (outline > 0) {
            flags |= FT_LOAD_NO_BITMAP;
        }
        const FT_Render_Mode mode = face.settings.antialiasing == Antialiasing::None
                                        ? FT_RENDER_MODE_MONO
                                        : FT_RENDER_MODE_NORMAL;
        set_size(face, size);
        if (FT_Load_Glyph(face.ft, index, flags) != 0) {
            return std::nullopt;
        }
        if (outline == 0) {
            if (FT_Render_Glyph(face.ft->glyph, mode) != 0) {
                return std::nullopt;
            }
            const FT_Bitmap& bitmap = face.ft->glyph->bitmap;
            if (bitmap.width == 0 || bitmap.rows == 0) {
                return std::nullopt;
            }
            return put_coverage(coverage(bitmap), bitmap.width, bitmap.rows,
                                face.ft->glyph->bitmap_left, face.ft->glyph->bitmap_top);
        }
        // Godot's outline: a stroke a quarter of outline_size in pixels each side of the edge.
        FT_Stroker_Set(stroker, static_cast<FT_Fixed>(outline) * 16, FT_STROKER_LINECAP_BUTT,
                       FT_STROKER_LINEJOIN_ROUND, 0);
        FT_Glyph glyph = nullptr;
        if (FT_Get_Glyph(face.ft->glyph, &glyph) != 0) {
            return std::nullopt;
        }
        std::optional<GlyphImage> out;
        if (FT_Glyph_Stroke(&glyph, stroker, 1) == 0 &&
            FT_Glyph_To_Bitmap(&glyph, mode, nullptr, 1) == 0) {
            const auto* image = reinterpret_cast<FT_BitmapGlyph>(glyph);
            if (image->bitmap.width > 0 && image->bitmap.rows > 0) {
                out = put_coverage(coverage(image->bitmap), image->bitmap.width, image->bitmap.rows,
                                   image->left, image->top);
            }
        }
        FT_Done_Glyph(glyph);
        return out;
    }

    // A glyph's distance field, made from its outline at msdf_size, as Godot's MSDF fonts are.
    std::optional<GlyphImage> rasterize_msdf(Face& face, std::uint32_t index) {
        set_size(face, face.settings.msdf_size);
        if (FT_Load_Glyph(face.ft, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0 ||
            face.ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            return std::nullopt;
        }
        FT_Outline& outline = face.ft->glyph->outline;
        msdfgen::Shape shape;
        ShapeBuilder builder{.shape = &shape, .contour = nullptr, .position = {}};
        FT_Outline_Funcs funcs{};
        funcs.move_to = &ShapeBuilder::move_to;
        funcs.line_to = &ShapeBuilder::line_to;
        funcs.conic_to = &ShapeBuilder::conic_to;
        funcs.cubic_to = &ShapeBuilder::cubic_to;
        if (FT_Outline_Decompose(&outline, &funcs, &builder) != 0) {
            return std::nullopt;
        }
        if (!shape.contours.empty() && shape.contours.back().edges.empty()) {
            shape.contours.pop_back();
        }
        // PostScript outlines wind the other way.
        if (FT_Outline_Get_Orientation(&outline) == FT_ORIENTATION_POSTSCRIPT) {
            for (msdfgen::Contour& contour : shape.contours) {
                contour.reverse();
            }
        }
        shape.normalize();
        if (shape.contours.empty() || !shape.validate()) {
            return std::nullopt;
        }
        // The field reaches half the range each way, so that much room goes round the outline.
        const double range = face.settings.msdf_pixel_range;
        const msdfgen::Shape::Bounds bounds = shape.getBounds(range / 2.0);
        const int w = static_cast<int>(std::ceil(bounds.r - bounds.l));
        const int h = static_cast<int>(std::ceil(bounds.t - bounds.b));
        if (w <= 0 || h <= 0) {
            return std::nullopt;
        }
        msdfgen::edgeColoringSimple(shape, 3.0);
        msdfgen::Bitmap<float, 4> field(w, h);
        const msdfgen::Projection projection(msdfgen::Vector2(1.0, 1.0),
                                             msdfgen::Vector2(-bounds.l, -bounds.b));
        msdfgen::generateMTSDF(field, shape, projection, msdfgen::Range(range),
                               msdfgen::MSDFGeneratorConfig(true));

        const auto uw = static_cast<std::uint32_t>(w);
        const auto uh = static_cast<std::uint32_t>(h);
        const auto [p, at] = allocate(uw, uh, true);
        AtlasPage& page = pages[p];
        for (int y = 0; y < h; ++y) {
            // The field's rows go up from the bottom of the bounds.
            const float* row = field(0, h - 1 - y);
            for (int x = 0; x < w; ++x) {
                const std::size_t texel =
                    ((static_cast<std::size_t>(at.y) + static_cast<std::size_t>(y)) * page.width +
                     at.x + static_cast<std::size_t>(x)) *
                    4U;
                for (std::size_t c = 0; c < 4; ++c) {
                    const float value = row[(static_cast<std::size_t>(x) * 4U) + c] * 256.0F;
                    page.rgba[texel + c] =
                        static_cast<std::uint8_t>(std::clamp(value, 0.0F, 255.0F));
                }
            }
        }
        ++page.version;
        return GlyphImage{.page = p,
                          .x = at.x,
                          .y = at.y,
                          .width = uw,
                          .height = uh,
                          .left = static_cast<float>(bounds.l),
                          .top = static_cast<float>(-bounds.t),
                          .scale = 1.0F};
    }
};

Fonts::Fonts(asset::Assets& assets) : impl_(std::make_unique<Impl>()) {
    impl_->assets = &assets;
}

Fonts::~Fonts() = default;

Result<TextLayout> Fonts::layout(std::string_view text, const LayoutOptions& options) {
    if (options.font_size < 1 || options.font_size > kMaxFontSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "font sizes go from 1 to " + std::to_string(kMaxFontSize));
    }
    auto chain = impl_->chain_for(options.font);
    if (!chain) {
        return chain.error();
    }
    TextLayout layout;
    layout.font = options.font;
    layout.font_size = options.font_size;
    std::size_t start = 0;
    std::size_t i = 0;
    while (true) {
        const std::size_t length = i < text.size() ? line_break_at(text, i) : 0;
        if (i < text.size() && length == 0) {
            i += utf8_length(static_cast<unsigned char>(text[i]));
            continue;
        }
        const std::string_view paragraph = text.substr(start, i - start);
        const Shaped& shaped = impl_->shape(chain.value(), options.font_size, paragraph, options);
        const std::size_t first_line = layout.lines.size();
        Impl::break_lines(layout, paragraph, start, shaped.glyphs, (shaped.base % 2U) == 1U,
                          options);
        for (std::size_t l = first_line; l < layout.lines.size(); ++l) {
            Impl::place(layout, layout.lines[l], paragraph, start, shaped);
        }
        if (i >= text.size()) {
            break;
        }
        i += length;
        start = i;
    }
    // Lines are as tall as the tallest font drawn in them, as Godot's.
    layout.metrics = impl_->metrics(*impl_->faces[chain.value().faces[0]], options.font_size);
    std::vector<std::uint32_t> used;
    for (const Glyph& glyph : layout.glyphs) {
        if (glyph.index == 0) {
            ++layout.missing;
        }
        const Face& face = *impl_->faces[glyph.face];
        if (face.system) {
            ++layout.system;
        }
        if (std::ranges::find(used, glyph.face) == used.end()) {
            used.push_back(glyph.face);
            const FontMetrics metrics =
                impl_->metrics(*impl_->faces[glyph.face], options.font_size);
            layout.metrics.ascent = std::max(layout.metrics.ascent, metrics.ascent);
            layout.metrics.descent = std::max(layout.metrics.descent, metrics.descent);
        }
    }
    for (const Line& line : layout.lines) {
        layout.width = std::max(layout.width, line.width);
    }
    return layout;
}

void Fonts::set_system_fallback(bool on) {
    if (impl_->system_fallback != on) {
        impl_->system_fallback = on;
        impl_->shaped.clear();
    }
}

bool Fonts::system_fallback() const {
    return impl_->system_fallback;
}

Result<FontMetrics> Fonts::metrics(std::string_view font, int size) {
    if (size < 1 || size > kMaxFontSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "font sizes go from 1 to " + std::to_string(kMaxFontSize));
    }
    auto face = impl_->face_for(font);
    if (!face) {
        return face.error();
    }
    return impl_->metrics(*impl_->faces[face.value()], size);
}

bool Fonts::trim(TextLayout& layout, std::size_t line_index, float width, Overrun overrun,
                 std::string_view ellipsis, bool force_ellipsis) {
    if (overrun == Overrun::NoTrimming || line_index >= layout.lines.size()) {
        return false;
    }
    Line& line = layout.lines[line_index];
    const bool add_dots = overrun == Overrun::TrimEllipsis || overrun == Overrun::TrimWordEllipsis;
    if (line.width <= width && !(force_ellipsis && add_dots)) {
        return false;
    }
    // The line's glyphs from where its text starts: the left, or the right for a right-to-left
    // line, which is cut at its left.
    const auto at = [&](std::uint32_t g) -> Glyph& {
        return layout.glyphs[line.first + (line.rtl ? line.count - 1 - g : g)];
    };
    std::vector<Glyph> dots;
    float dots_width = 0.0F;
    if (add_dots && line.count > 0) {
        // The ellipsis from the line's first font that has it, then the layout's fonts in
        // order, as Godot looks for one; three dots where none has it.
        std::vector<std::uint32_t> faces{at(0).face};
        if (const auto chain = impl_->chain_for(layout.font); chain) {
            for (const std::uint32_t face : chain.value().faces) {
                if (std::ranges::find(faces, face) == faces.end()) {
                    faces.push_back(face);
                }
            }
        }
        const auto has_all = [](const std::vector<Glyph>& glyphs) {
            return std::ranges::none_of(glyphs, [](const Glyph& g) { return g.index == 0; });
        };
        for (const std::string_view mark : {ellipsis, std::string_view("...")}) {
            for (const std::uint32_t face : faces) {
                dots = impl_->shape_plain(face, layout.font_size, mark);
                if (has_all(dots)) {
                    break;
                }
            }
            if (has_all(dots)) {
                break;
            }
        }
        if (!has_all(dots)) {
            dots = impl_->shape_plain(faces.front(), layout.font_size, ellipsis);
        }
        for (const Glyph& dot : dots) {
            dots_width += dot.advance;
        }
    }
    const bool by_word = overrun == Overrun::TrimWord || overrun == Overrun::TrimWordEllipsis;
    const float room = width - dots_width;
    // How many glyphs fit, and how many end at a word.
    std::uint32_t fit = 0;
    std::uint32_t word_fit = 0;
    float pen = 0.0F;
    for (std::uint32_t g = 0; g < line.count; ++g) {
        const Glyph& glyph = at(g);
        if (!glyph.space && pen + glyph.advance > room) {
            break;
        }
        pen += glyph.advance;
        fit = g + 1;
        if (glyph.space) {
            word_fit = g;
        }
    }
    std::uint32_t keep = by_word && word_fit > 0 ? word_fit : fit;
    while (keep > 0 && at(keep - 1).space) {
        --keep;
    }
    // Where the text cut off starts.
    std::uint32_t end_byte = line.end;
    for (std::uint32_t g = keep; g < line.count; ++g) {
        end_byte = std::min(end_byte, at(g).cluster);
    }
    float kept_width = 0.0F;
    for (std::uint32_t g = 0; g < keep; ++g) {
        kept_width += at(g).advance;
    }
    const auto begin = layout.glyphs.begin() + line.first;
    if (line.rtl) {
        // The kept glyphs move left, after the ellipsis.
        const std::uint32_t cut = line.count - keep;
        float left = line.count > 0 ? begin->x : 0.0F;
        for (std::uint32_t g = 0; g < cut; ++g) {
            left += begin[g].advance;
        }
        layout.glyphs.erase(begin, begin + cut);
        for (std::uint32_t g = 0; g < keep; ++g) {
            layout.glyphs[line.first + g].x += dots_width - left;
        }
        float x = 0.0F;
        std::uint32_t inserted = 0;
        for (Glyph dot : dots) {
            dot.x += x;
            dot.cluster = end_byte;
            x += dot.advance;
            layout.glyphs.insert(layout.glyphs.begin() + line.first + inserted, dot);
            ++inserted;
        }
        keep += inserted;
    } else {
        layout.glyphs.erase(begin + keep, begin + line.count);
        float x = kept_width;
        for (Glyph dot : dots) {
            dot.x += x;
            dot.cluster = end_byte;
            x += dot.advance;
            layout.glyphs.insert(layout.glyphs.begin() + line.first + keep, dot);
            ++keep;
        }
    }
    const std::uint32_t removed = line.count - keep;
    line.count = keep;
    line.end = end_byte;
    line.width = kept_width + dots_width;
    for (std::size_t l = line_index + 1; l < layout.lines.size(); ++l) {
        layout.lines[l].first -= removed;
    }
    layout.width = 0.0F;
    for (const Line& each : layout.lines) {
        layout.width = std::max(layout.width, each.width);
    }
    return true;
}

std::optional<GlyphImage> Fonts::glyph(std::uint32_t face_id, std::uint32_t index, int size,
                                       int outline_size) {
    if (face_id >= impl_->faces.size() || size < 1 || size > kMaxFontSize) {
        return std::nullopt;
    }
    Face& face = *impl_->faces[face_id];
    const bool msdf = face.msdf();
    const auto key =
        std::make_tuple(face_id, index, msdf ? 0 : size, msdf ? 0 : std::max(outline_size, 0));
    auto found = impl_->glyphs.find(key);
    if (found == impl_->glyphs.end()) {
        std::optional<GlyphImage> image =
            msdf ? impl_->rasterize_msdf(face, index)
                 : impl_->rasterize(face, index, size, std::max(outline_size, 0));
        if (image) {
            ++impl_->glyph_images;
        }
        found = impl_->glyphs.emplace(key, image).first;
    }
    std::optional<GlyphImage> image = found->second;
    if (image && msdf) {
        image->scale = static_cast<float>(size) / static_cast<float>(face.settings.msdf_size);
    }
    return image;
}

FaceInfo Fonts::face_info(std::uint32_t face_id) const {
    if (face_id >= impl_->faces.size()) {
        return {};
    }
    const Face& face = *impl_->faces[face_id];
    return {.msdf = face.msdf(),
            .msdf_pixel_range = face.msdf() ? face.settings.msdf_pixel_range : 0,
            .antialiasing = face.settings.antialiasing,
            .path = face.path,
            .system = face.system};
}

const std::vector<AtlasPage>& Fonts::pages() const {
    return impl_->pages;
}

asset::Assets& Fonts::assets() const {
    return *impl_->assets;
}

std::size_t Fonts::glyph_count() const {
    return impl_->glyph_images;
}

Result<void> check_font_data(std::string_view bytes) {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        return make_error(ErrorCode::PlatformFailure, "FreeType didn't start");
    }
    const std::string copy(bytes);
    FT_Face face = nullptr;
    const bool opened = open_face(library, copy, face);
    if (opened) {
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
    if (!opened) {
        return make_error(ErrorCode::InvalidArgument, "not a TrueType or OpenType font");
    }
    return {};
}

} // namespace slash::text
