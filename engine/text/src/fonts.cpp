#include "slash/text/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

#include <hb.h>
#include <msdfgen.h>

namespace slash::text {
namespace {

// Opens a scalable font face from bytes, which must outlive it.
bool open_face(FT_Library library, const std::string& bytes, FT_Face& face) {
    if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(bytes.data()),
                           static_cast<FT_Long>(bytes.size()), 0, &face) != 0) {
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

// A run of a paragraph's text in one script.
struct Run {
    std::size_t start = 0;
    std::size_t end = 0;
};

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
    hb_language_t language = nullptr;
    std::vector<std::unique_ptr<Face>> faces;
    std::map<std::string, Source, std::less<>> sources;
    std::vector<AtlasPage> pages;
    std::vector<std::vector<Shelf>> shelves;
    std::vector<std::uint32_t> page_bottoms; // where the next shelf of each page goes
    std::map<std::tuple<std::uint32_t, std::uint32_t, int, int>, std::optional<GlyphImage>> glyphs;
    std::size_t glyph_images = 0;
    std::map<std::tuple<std::uint32_t, int, std::string>, std::vector<Glyph>, std::less<>> shaped;

    Impl() {
        FT_Init_FreeType(&library);
        FT_Stroker_New(library, &stroker);
        buffer = hb_buffer_create();
        // A fixed language, so shaping is the same whatever the machine's locale.
        language = hb_language_from_string("und", -1);
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
                                    std::string_view path) {
        auto face = std::make_unique<Face>();
        face->data = std::move(data);
        face->settings = std::move(settings);
        const std::string& bytes = face->data->bytes;
        if (!open_face(library, bytes, face->ft)) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(path) + " is not a TrueType or OpenType font");
        }
        face->blob = hb_blob_create(bytes.data(), static_cast<unsigned int>(bytes.size()),
                                    HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        face->hb_face = hb_face_create(face->blob, 0);
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
            auto face = make_face(std::move(data), {}, "the default font");
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
        auto face = make_face(source.data.share(), std::move(settings), data_path);
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

    // Splits a paragraph where its script changes; spaces and punctuation join the run they are
    // in.
    static std::vector<Run> script_runs(std::string_view text) {
        std::vector<Run> runs;
        hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
        hb_script_t current = HB_SCRIPT_INVALID;
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size();
             i += utf8_length(static_cast<unsigned char>(text[i]))) {
            const hb_script_t script = hb_unicode_script(unicode, char_at(text, i));
            if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
                script == HB_SCRIPT_UNKNOWN) {
                continue;
            }
            if (current != HB_SCRIPT_INVALID && script != current) {
                runs.push_back({start, i});
                start = i;
            }
            current = script;
        }
        runs.push_back({start, text.size()});
        return runs;
    }

    // A paragraph's glyphs in the order of its text, clusters from its start.
    std::vector<Glyph> shape(std::uint32_t face_id, int size, std::string_view text) {
        const auto key = std::make_tuple(face_id, size, std::string(text));
        if (const auto found = shaped.find(key); found != shaped.end()) {
            return found->second;
        }
        const Face& face = *faces[face_id];
        const bool round = !face.msdf();
        hb_font_set_scale(face.hb_font, size * 64, size * 64);
        std::vector<Glyph> out;
        for (const Run& run : script_runs(text)) {
            hb_buffer_clear_contents(buffer);
            hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                               static_cast<unsigned int>(run.start),
                               static_cast<int>(run.end - run.start));
            hb_buffer_set_language(buffer, language);
            hb_buffer_guess_segment_properties(buffer);
            hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
            hb_shape(face.hb_font, buffer, nullptr, 0);
            unsigned int count = 0;
            const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
            const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
            const std::size_t first = out.size();
            const bool rtl = HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(buffer));
            for (unsigned int i = 0; i < count; ++i) {
                // Glyph images draw at whole pixels, so their advances are whole too, as
                // Godot's are without subpixel positioning.
                const auto pixels = [round](hb_position_t value) {
                    const double exact = static_cast<double>(value) / kUnitsPerPixel;
                    return static_cast<float>(round ? std::round(exact) : exact);
                };
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
                std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
            }
        }
        if (shaped.size() >= kShapeCacheSize) {
            shaped.clear();
        }
        shaped.emplace(key, out);
        return out;
    }

    // Breaks one paragraph's glyphs into lines and appends them to the layout.
    static void break_lines(TextLayout& layout, std::string_view text, std::size_t offset,
                            std::vector<Glyph> glyphs, const LayoutOptions& options) {
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

    // Sets where each glyph of a line draws: in text order, with right-to-left runs reversed.
    static void place(TextLayout& layout, const Line& line) {
        const auto begin = layout.glyphs.begin() + line.first;
        const auto end = begin + line.count;
        for (auto run = begin; run != end;) {
            auto run_end = run;
            const bool rtl = run->rtl;
            while (run_end != end && run_end->rtl == rtl) {
                ++run_end;
            }
            if (rtl) {
                std::reverse(run, run_end);
            }
            run = run_end;
        }
        float pen = 0.0F;
        for (auto glyph = begin; glyph != end; ++glyph) {
            const float offset = glyph->x;
            glyph->x = pen + offset;
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
    auto face = impl_->face_for(options.font);
    if (!face) {
        return face.error();
    }
    TextLayout layout;
    layout.font = options.font;
    layout.font_size = options.font_size;
    layout.metrics = impl_->metrics(*impl_->faces[face.value()], options.font_size);
    std::size_t start = 0;
    std::size_t i = 0;
    while (true) {
        const std::size_t length = i < text.size() ? line_break_at(text, i) : 0;
        if (i < text.size() && length == 0) {
            i += utf8_length(static_cast<unsigned char>(text[i]));
            continue;
        }
        const std::string_view paragraph = text.substr(start, i - start);
        std::vector<Glyph> glyphs = impl_->shape(face.value(), options.font_size, paragraph);
        const std::size_t first_line = layout.lines.size();
        Impl::break_lines(layout, paragraph, start, std::move(glyphs), options);
        for (std::size_t l = first_line; l < layout.lines.size(); ++l) {
            Impl::place(layout, layout.lines[l]);
        }
        if (i >= text.size()) {
            break;
        }
        i += length;
        start = i;
    }
    for (const Glyph& glyph : layout.glyphs) {
        if (glyph.index == 0) {
            ++layout.missing;
        }
    }
    for (const Line& line : layout.lines) {
        layout.width = std::max(layout.width, line.width);
    }
    return layout;
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
    std::vector<Glyph> dots;
    float dots_width = 0.0F;
    if (add_dots && line.count > 0) {
        const std::uint32_t face = layout.glyphs[line.first].face;
        dots = impl_->shape(face, layout.font_size, ellipsis);
        if (std::ranges::any_of(dots, [](const Glyph& g) { return g.index == 0; })) {
            dots = impl_->shape(face, layout.font_size, "...");
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
        const Glyph& glyph = layout.glyphs[line.first + g];
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
    while (keep > 0 && layout.glyphs[line.first + keep - 1].space) {
        --keep;
    }
    const auto begin = layout.glyphs.begin() + line.first;
    const std::uint32_t end_byte =
        keep < line.count ? layout.glyphs[line.first + keep].cluster : line.end;
    layout.glyphs.erase(begin + keep, begin + line.count);
    float x = 0.0F;
    for (std::uint32_t g = 0; g < keep; ++g) {
        x += layout.glyphs[line.first + g].advance;
    }
    const float kept_width = x;
    for (Glyph dot : dots) {
        dot.x += x;
        dot.cluster = end_byte;
        x += dot.advance;
        layout.glyphs.insert(layout.glyphs.begin() + line.first + keep, dot);
        ++keep;
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
            .antialiasing = face.settings.antialiasing};
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
