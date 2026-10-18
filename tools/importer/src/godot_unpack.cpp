#include "common.hpp"
#include "godot_binary.hpp"

#include "slash/core/text.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <src/webp/decode.h>
#include <string>
#include <utility>
#include <vector>

namespace slash::importer::godot {
namespace {

// --- The pack (core/io/file_access_pack.cpp) ---------------------------------------------------

constexpr std::string_view kPackMagic = "GDPC";
constexpr std::uint32_t kPackDirEncrypted = 1U << 0U;
constexpr std::uint32_t kPackRelativeBase = 1U << 1U;
constexpr std::uint32_t kPackSparseBundle = 1U << 2U;
constexpr std::uint32_t kFileEncrypted = 1U << 0U;
constexpr std::uint32_t kFileRemoval = 1U << 1U;
constexpr std::uint32_t kFileDelta = 1U << 2U;

struct PackFile {
    std::string path; // without "res://"
    std::string_view data;
    std::uint32_t flags = 0;
};

struct Pack {
    std::uint32_t format = 0;
    std::string version; // the Godot version that exported it, "4.5.0"
    int tab_size = 4;    // the columns its GDScript tokens count a tab as
    bool embedded = false;
    std::vector<PackFile> files;
};

std::string strip_res(std::string_view path) {
    if (path.starts_with("res://")) {
        path.remove_prefix(6);
    }
    return std::string(path);
}

// Whether a pack header starts at `at`: the magic, then a pack format and a Godot version that
// make sense, so the magic's bytes inside an executable's code aren't taken for one.
bool header_at(std::string_view bytes, std::size_t at) {
    if (at + 20 > bytes.size() || bytes.substr(at, 4) != kPackMagic) {
        return false;
    }
    ByteReader reader(bytes);
    reader.seek(at + 4);
    const std::uint32_t format = reader.u32();
    const std::uint32_t major = reader.u32();
    const std::uint32_t minor = reader.u32();
    const std::uint32_t patch = reader.u32();
    return format >= 1 && format <= 16 && major >= 2 && major <= 16 && minor < 100 && patch < 100;
}

// Where the pack starts, as Godot looks for it: at the start of a .pck file, at the end of an
// executable (its size and the magic are the last 12 bytes), or in the executable's "pck"
// section, found by its header.
std::optional<std::size_t> find_pack(std::string_view bytes, bool& embedded) {
    embedded = false;
    if (header_at(bytes, 0)) {
        return 0;
    }
    embedded = true;
    if (bytes.size() >= 16 && bytes.substr(bytes.size() - 4) == kPackMagic) {
        ByteReader reader(bytes);
        reader.seek(bytes.size() - 12);
        const std::uint64_t size = reader.u64();
        if (size <= bytes.size() - 12) {
            const std::size_t at = bytes.size() - 12 - static_cast<std::size_t>(size);
            if (header_at(bytes, at)) {
                return at;
            }
        }
    }
    for (std::size_t at = bytes.find(kPackMagic); at != std::string_view::npos;
         at = bytes.find(kPackMagic, at + 1)) {
        if (header_at(bytes, at)) {
            return at;
        }
    }
    return std::nullopt;
}

Result<Pack> read_pack(std::string_view bytes) {
    Pack pack;
    const auto start = find_pack(bytes, pack.embedded);
    if (!start) {
        return make_error(ErrorCode::InvalidArgument,
                          "no Godot pack in the file: a game exported with Godot has a .pck "
                          "file beside its executable, or the pack inside it");
    }
    ByteReader reader(bytes);
    reader.seek(*start + 4);
    pack.format = reader.u32();
    const std::uint32_t major = reader.u32();
    const std::uint32_t minor = reader.u32();
    const std::uint32_t patch = reader.u32();
    pack.version =
        std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (pack.format < 2 || major < 4) {
        return make_error(ErrorCode::InvalidArgument,
                          "a game made with Godot " + pack.version +
                              ": only games made with Godot 4 can be unpacked");
    }
    // Godot 4.7 stopped counting a tab as four columns.
    pack.tab_size = major == 4 && minor <= 6 ? 4 : 1;
    if (pack.format > 4) {
        return make_error(ErrorCode::InvalidArgument, "pack format " + std::to_string(pack.format) +
                                                          " (Godot " + pack.version +
                                                          ") is newer than this reads (2 to 4)");
    }
    const std::uint32_t flags = reader.u32();
    if ((flags & kPackDirEncrypted) != 0) {
        return make_error(ErrorCode::PermissionDenied,
                          "the pack's file list is encrypted with the game's own key, so only "
                          "its maker can read it: import the game's project folder instead");
    }
    if ((flags & kPackSparseBundle) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a sparse pack, whose files are in a folder beside it: only packs "
                          "holding their files can be unpacked");
    }
    std::uint64_t file_base = reader.u64();
    if (pack.format >= 3 || (flags & kPackRelativeBase) != 0) {
        file_base += *start;
    }
    if (pack.format >= 3) {
        reader.seek(static_cast<std::size_t>(reader.u64() + *start));
    } else {
        reader.skip(std::size_t{16} * 4); // reserved
    }
    const std::uint32_t count = reader.u32();
    for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
        std::string_view path = reader.bytes(reader.u32());
        if (const std::size_t end = path.find('\0'); end != std::string_view::npos) {
            path = path.substr(0, end); // paths are padded with zeros
        }
        const std::uint64_t offset = reader.u64() + file_base;
        const std::uint64_t size = reader.u64();
        reader.skip(16); // MD5
        const std::uint32_t file_flags = reader.u32();
        if ((file_flags & kFileRemoval) != 0) {
            continue;
        }
        if (offset > bytes.size() || size > bytes.size() - offset) {
            return make_error(ErrorCode::InvalidArgument,
                              "the pack is cut short: " + std::string(path) + " goes past its end");
        }
        pack.files.push_back(
            {.path = strip_res(path),
             .data = bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)),
             .flags = file_flags});
    }
    if (!reader.ok()) {
        return make_error(ErrorCode::InvalidArgument, "the pack's file list is cut short");
    }
    return pack;
}

// --- Textures (scene/resources/compressed_texture.cpp, core/io/image.cpp) ----------------------

enum class DataFormat : std::uint8_t { Image, Png, Webp, BasisUniversal };

// Image::Format values a texture is read from, which the file stores in 32 bits.
// NOLINTNEXTLINE(performance-enum-size)
enum ImageFormat : std::uint32_t {
    kL8 = 0,
    kLA8 = 1,
    kR8 = 2,
    kRG8 = 3,
    kRGB8 = 4,
    kRGBA8 = 5,
    kRGBA4444 = 6,
    kRGB565 = 7,
    kDXT1 = 17,
    kDXT3 = 18,
    kDXT5 = 19,
};

std::uint8_t expand(std::uint32_t value, std::uint32_t bits) {
    const std::uint32_t max = (1U << bits) - 1U;
    return static_cast<std::uint8_t>((value * 255U + max / 2U) / max);
}

std::array<std::uint8_t, 3> rgb565(std::uint16_t color) {
    return {expand((color >> 11U) & 0x1FU, 5), expand((color >> 5U) & 0x3FU, 6),
            expand(color & 0x1FU, 5)};
}

// One 4x4 block of BC1 colors, as RGBA into `out` (16 pixels). BC2 and BC3 blocks always use
// four colors.
void decode_bc1(const std::uint8_t* block, bool four_colors, std::uint8_t* out) {
    const auto c0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8U));
    const auto c1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8U));
    const bool opaque = four_colors || c0 > c1;
    std::array<std::array<std::uint8_t, 4>, 4> colors{};
    const auto a = rgb565(c0);
    const auto b = rgb565(c1);
    for (std::size_t i = 0; i < 3; ++i) {
        colors[0][i] = a[i];
        colors[1][i] = b[i];
        if (opaque) {
            colors[2][i] = static_cast<std::uint8_t>((2 * a[i] + b[i] + 1) / 3);
            colors[3][i] = static_cast<std::uint8_t>((a[i] + 2 * b[i] + 1) / 3);
        } else {
            colors[2][i] = static_cast<std::uint8_t>((a[i] + b[i] + 1) / 2);
            colors[3][i] = 0;
        }
    }
    colors[0][3] = 255;
    colors[1][3] = 255;
    colors[2][3] = 255;
    colors[3][3] = opaque ? 255 : 0;
    std::uint32_t indices = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        indices |= static_cast<std::uint32_t>(block[4 + i]) << (8U * i);
    }
    for (std::uint32_t pixel = 0; pixel < 16; ++pixel) {
        std::memcpy(out + static_cast<std::size_t>(pixel) * 4,
                    colors[(indices >> (2 * pixel)) & 3U].data(), 4);
    }
}

// A BC3 block's alpha, into `out`'s alpha bytes.
void decode_bc3_alpha(const std::uint8_t* block, std::uint8_t* out) {
    std::array<int, 8> alphas{};
    alphas[0] = block[0];
    alphas[1] = block[1];
    if (alphas[0] > alphas[1]) {
        for (int i = 1; i < 7; ++i) {
            alphas[static_cast<std::size_t>(i) + 1] = ((7 - i) * alphas[0] + i * alphas[1] + 3) / 7;
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            alphas[static_cast<std::size_t>(i) + 1] = ((5 - i) * alphas[0] + i * alphas[1] + 2) / 5;
        }
        alphas[6] = 0;
        alphas[7] = 255;
    }
    std::uint64_t indices = 0;
    for (std::uint32_t i = 0; i < 6; ++i) {
        indices |= static_cast<std::uint64_t>(block[2 + i]) << (8U * i);
    }
    for (std::uint32_t pixel = 0; pixel < 16; ++pixel) {
        out[static_cast<std::size_t>(pixel) * 4 + 3] =
            static_cast<std::uint8_t>(alphas[(indices >> (3U * pixel)) & 7U]);
    }
}

Result<render::Image> block_pixels(std::string_view data, std::uint32_t width, std::uint32_t height,
                                   std::uint32_t format) {
    const std::uint32_t blocks_x = (width + 3) / 4;
    const std::uint32_t blocks_y = (height + 3) / 4;
    const std::size_t block_size = format == kDXT1 ? 8 : 16;
    if (data.size() < static_cast<std::size_t>(blocks_x) * blocks_y * block_size) {
        return make_error(ErrorCode::InvalidArgument, "the texture's pixels are cut short");
    }
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
    std::array<std::uint8_t, 64> block{};
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const std::uint8_t* source =
                in + (static_cast<std::size_t>(by) * blocks_x + bx) * block_size;
            if (format == kDXT1) {
                decode_bc1(source, false, block.data());
            } else {
                decode_bc1(source + 8, true, block.data());
                if (format == kDXT3) {
                    for (std::uint32_t p = 0; p < 16; ++p) {
                        block[static_cast<std::size_t>(p) * 4 + 3] =
                            expand((source[p / 2] >> ((p % 2) * 4U)) & 0xFU, 4);
                    }
                } else {
                    decode_bc3_alpha(source, block.data());
                }
            }
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
                for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    std::memcpy(&image.pixels[((static_cast<std::size_t>(by) * 4 + y) * width +
                                               static_cast<std::size_t>(bx) * 4 + x) *
                                              4],
                                &block[(static_cast<std::size_t>(y) * 4 + x) * 4], 4);
                }
            }
        }
    }
    return image;
}

// Raw pixels of the first mipmap in one of Godot's formats, as RGBA.
Result<render::Image> raw_pixels(std::string_view data, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t format) {
    std::size_t pixel_size = 0;
    switch (format) {
    case kL8:
    case kR8:
        pixel_size = 1;
        break;
    case kLA8:
    case kRG8:
    case kRGBA4444:
    case kRGB565:
        pixel_size = 2;
        break;
    case kRGB8:
        pixel_size = 3;
        break;
    case kRGBA8:
        pixel_size = 4;
        break;
    case kDXT1:
    case kDXT3:
    case kDXT5:
        return block_pixels(data, width, height, format);
    default:
        return make_error(ErrorCode::InvalidArgument, "pixels in Godot's image format " +
                                                          std::to_string(format) +
                                                          ", which this doesn't decode");
    }
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (data.size() < count * pixel_size) {
        return make_error(ErrorCode::InvalidArgument, "the texture's pixels are cut short");
    }
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(count * 4);
    const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = in + i * pixel_size;
        std::uint8_t* o = &image.pixels[i * 4];
        std::array<std::uint8_t, 4> color{0, 0, 0, 255};
        switch (format) {
        case kL8:
            color = {p[0], p[0], p[0], 255};
            break;
        case kLA8:
            color = {p[0], p[0], p[0], p[1]};
            break;
        case kR8:
            color[0] = p[0];
            break;
        case kRG8:
            color[0] = p[0];
            color[1] = p[1];
            break;
        case kRGB8:
            color = {p[0], p[1], p[2], 255};
            break;
        case kRGBA4444: {
            const auto v = static_cast<std::uint32_t>(p[0] | (p[1] << 8U));
            color = {expand((v >> 12U) & 0xFU, 4), expand((v >> 8U) & 0xFU, 4),
                     expand((v >> 4U) & 0xFU, 4), expand(v & 0xFU, 4)};
            break;
        }
        case kRGB565: {
            const auto rgb = rgb565(static_cast<std::uint16_t>(p[0] | (p[1] << 8U)));
            color = {rgb[0], rgb[1], rgb[2], 255};
            break;
        }
        default:
            color = {p[0], p[1], p[2], p[3]};
            break;
        }
        std::memcpy(o, color.data(), 4);
    }
    return image;
}

Result<render::Image> decode_webp(std::string_view data) {
    int width = 0;
    int height = 0;
    std::uint8_t* pixels = WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(data.data()),
                                          data.size(), &width, &height);
    if (pixels == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "the texture's WebP image doesn't decode");
    }
    render::Image image{.width = static_cast<std::uint32_t>(width),
                        .height = static_cast<std::uint32_t>(height),
                        .pixels = {}};
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(image.width) * image.height * 4);
    WebPFree(pixels);
    return image;
}

// A CompressedTexture2D file (.ctex) as a PNG file.
Result<std::string> texture_png(std::string_view bytes) {
    ByteReader reader(bytes);
    if (reader.bytes(4) != "GST2") {
        return make_error(ErrorCode::InvalidArgument, "not a compressed texture (GST2)");
    }
    reader.skip(4); // version
    const std::uint32_t width = reader.u32();
    const std::uint32_t height = reader.u32();
    reader.skip(std::size_t{4} * 5); // data format bits, mipmap limit, reserved
    const std::uint32_t data_format = reader.u32();
    const std::uint32_t image_width = reader.u16();
    const std::uint32_t image_height = reader.u16();
    reader.skip(4); // mipmaps
    const std::uint32_t format = reader.u32();
    if (!reader.ok()) {
        return make_error(ErrorCode::InvalidArgument, "the texture is cut short");
    }
    Result<render::Image> image = render::Image{};
    if (data_format == static_cast<std::uint32_t>(DataFormat::Png) ||
        data_format == static_cast<std::uint32_t>(DataFormat::Webp)) {
        std::string_view data = reader.bytes(reader.u32());
        if (!reader.ok()) {
            return make_error(ErrorCode::InvalidArgument, "the texture is cut short");
        }
        if (data_format == static_cast<std::uint32_t>(DataFormat::Webp)) {
            image = decode_webp(data);
        } else {
            if (data.starts_with("PNG ")) {
                data.remove_prefix(4); // Godot's own prefix
            }
            image = render::decode_image(data);
        }
    } else if (data_format == static_cast<std::uint32_t>(DataFormat::Image)) {
        image = raw_pixels(bytes.substr(reader.position()), image_width, image_height, format);
    } else {
        return make_error(ErrorCode::InvalidArgument,
                          "a Basis Universal texture, which this doesn't decode");
    }
    if (!image) {
        return image.error();
    }
    // Block-compressed images are padded to whole blocks; the texture keeps its own size.
    render::Image& pixels = image.value();
    if (width > 0 && height > 0 && width <= pixels.width && height <= pixels.height &&
        (width < pixels.width || height < pixels.height)) {
        render::Image cropped{.width = width, .height = height, .pixels = {}};
        cropped.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(&cropped.pixels[static_cast<std::size_t>(y) * width * 4],
                        &pixels.pixels[static_cast<std::size_t>(y) * pixels.width * 4],
                        static_cast<std::size_t>(width) * 4);
        }
        pixels = std::move(cropped);
    }
    return render::encode_png(pixels);
}

// --- Sounds (scene/resources/audio/audio_stream_wav.cpp, modules/vorbis, modules/minimp3) ------

void put_u16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

// A loop, as AudioStreamWAV's loop_mode (1 forward, 2 ping-pong, 3 backward) and its frames.
struct WavLoop {
    int mode = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

// A PCM WAV file. A loop goes in a "smpl" chunk, which Godot's importer reads back.
std::string wav_file(std::uint32_t channels, std::uint32_t rate, std::uint32_t bits,
                     std::string_view samples, const WavLoop& loop) {
    std::string chunks = "fmt ";
    put_u32(chunks, 16);
    put_u16(chunks, 1); // PCM
    put_u16(chunks, channels);
    put_u32(chunks, rate);
    put_u32(chunks, rate * channels * bits / 8);
    put_u16(chunks, channels * bits / 8);
    put_u16(chunks, bits);
    chunks += "data";
    put_u32(chunks, static_cast<std::uint32_t>(samples.size()));
    chunks += samples;
    if (samples.size() % 2 != 0) {
        chunks += '\0';
    }
    if (loop.mode > 0) {
        chunks += "smpl";
        put_u32(chunks, 36 + 24);
        for (int i = 0; i < 7; ++i) {
            put_u32(chunks, 0); // manufacturer, product, period, note, pitch, SMPTE
        }
        put_u32(chunks, 1);                                         // loops
        put_u32(chunks, 0);                                         // sampler data
        put_u32(chunks, 0);                                         // cue point
        put_u32(chunks, static_cast<std::uint32_t>(loop.mode - 1)); // forward, ping-pong, back
        put_u32(chunks, loop.begin);
        put_u32(chunks, loop.end);
        put_u32(chunks, 0); // fraction
        put_u32(chunks, 0); // play count: forever
    }
    std::string out = "RIFF";
    put_u32(out, static_cast<std::uint32_t>(chunks.size() + 4));
    out += "WAVE";
    out += chunks;
    return out;
}

// IMA-ADPCM nibbles as AudioStreamWAV packs them (low nibble first, channels interleaved by
// byte), as 16-bit samples.
std::string decode_ima_adpcm(std::string_view data, std::uint32_t channels) {
    static constexpr std::array<int, 89> kSteps = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
        25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
        88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
        307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
        1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
        3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static constexpr std::array<int, 16> kIndices = {-1, -1, -1, -1, 2, 4, 6, 8,
                                                     -1, -1, -1, -1, 2, 4, 6, 8};
    const std::size_t frames = data.size() * 2 / channels;
    std::string out;
    out.reserve(frames * channels * 2);
    std::array<int, 2> predictor{};
    std::array<std::size_t, 2> index{};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto byte = static_cast<std::uint8_t>(data[(frame >> 1U) * channels + c]);
            const auto nibble =
                static_cast<std::uint32_t>((frame & 1U) != 0 ? byte >> 4U : byte & 0xFU);
            const int step = kSteps[index[c]];
            index[c] = static_cast<std::size_t>(
                std::clamp(static_cast<int>(index[c]) + kIndices[nibble], 0, 88));
            int diff = step >> 3;
            if ((nibble & 1U) != 0) {
                diff += step >> 2;
            }
            if ((nibble & 2U) != 0) {
                diff += step >> 1;
            }
            if ((nibble & 4U) != 0) {
                diff += step;
            }
            if ((nibble & 8U) != 0) {
                diff = -diff;
            }
            predictor[c] = std::clamp(predictor[c] + diff, -0x8000, 0x7FFF);
            put_u16(out, static_cast<std::uint16_t>(predictor[c]));
        }
    }
    return out;
}

struct Pcm {
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::string samples; // 16-bit
};

// A QOA ("Quite OK Audio") file, as AudioStreamWAV's QOA format holds it, as 16-bit samples.
// Each 64-bit slice holds a scale factor and 20 residuals of 3 bits; a sign-sign LMS filter
// predicts each sample from the four before it.
Result<Pcm> decode_qoa(std::string_view data) {
    ByteReader reader(data);
    reader.set_big_endian(true);
    if (reader.bytes(4) != "qoaf") {
        return make_error(ErrorCode::InvalidArgument, "not a QOA sound");
    }
    const std::uint32_t total = reader.u32();
    // Scale factor s is round((s + 1) ^ 2.75); residual q is the factor times one of these
    // quarters, rounded away from zero.
    static constexpr std::array<int, 16> kScales = {1,   7,   21,  45,  84,   138,  211,  304,
                                                    421, 562, 731, 928, 1157, 1419, 1715, 2048};
    static constexpr std::array<int, 8> kQuarters = {3, -3, 10, -10, 18, -18, 28, -28};
    Pcm pcm;
    std::uint32_t decoded = 0;
    while (decoded < total && reader.left() >= 8) {
        const std::uint32_t channels = reader.u8();
        const std::uint32_t high = reader.u8();
        const std::uint32_t rate = (high << 16U) | reader.u16();
        const std::uint32_t samples = reader.u16();
        reader.skip(2); // frame size
        if (channels == 0 || (pcm.channels != 0 && channels != pcm.channels)) {
            return make_error(ErrorCode::InvalidArgument, "a QOA sound whose channels change");
        }
        pcm.channels = channels;
        pcm.rate = rate;
        std::vector<std::array<int, 4>> history(channels);
        std::vector<std::array<int, 4>> weights(channels);
        for (std::uint32_t c = 0; c < channels; ++c) {
            for (int& value : history[c]) {
                value = static_cast<std::int16_t>(reader.u16());
            }
            for (int& value : weights[c]) {
                value = static_cast<std::int16_t>(reader.u16());
            }
        }
        std::vector<std::int16_t> frame(static_cast<std::size_t>(samples) * channels);
        for (std::uint32_t start = 0; start < samples; start += 20) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                std::uint64_t slice = reader.u64();
                const int scale = kScales[(slice >> 60U) & 0xFU];
                slice <<= 4U;
                for (std::uint32_t i = start; i < std::min(start + 20, samples); ++i) {
                    std::int64_t prediction = 0;
                    for (std::size_t k = 0; k < 4; ++k) {
                        prediction += static_cast<std::int64_t>(weights[c][k]) * history[c][k];
                    }
                    const int quarters = scale * kQuarters[(slice >> 61U) & 7U];
                    const int residual = quarters >= 0 ? (quarters + 2) / 4 : -((2 - quarters) / 4);
                    const int sample =
                        std::clamp(static_cast<int>(prediction >> 13) + residual, -32768, 32767);
                    slice <<= 3U;
                    frame[static_cast<std::size_t>(i) * channels + c] =
                        static_cast<std::int16_t>(sample);
                    const int delta = residual >> 4;
                    for (std::size_t k = 0; k < 4; ++k) {
                        weights[c][k] += history[c][k] < 0 ? -delta : delta;
                    }
                    history[c] = {history[c][1], history[c][2], history[c][3], sample};
                }
            }
        }
        if (!reader.ok()) {
            return make_error(ErrorCode::InvalidArgument, "the QOA sound is cut short");
        }
        for (const std::int16_t sample : frame) {
            put_u16(pcm.samples, static_cast<std::uint16_t>(sample));
        }
        decoded += samples;
    }
    return pcm;
}

std::int64_t integer(const Section& section, std::string_view name, std::int64_t fallback) {
    const Value* value = section.property(name);
    return value != nullptr && value->is_number() ? static_cast<std::int64_t>(value->number())
                                                  : fallback;
}

// A PackedByteArray value's bytes.
std::string bytes_of(const Value* value) {
    if (value == nullptr || !value->is_call("PackedByteArray") || value->items.empty()) {
        return {};
    }
    return base64_decode(value->items.front().text).value_or(std::string());
}

// An AudioStreamWAV resource (.sample) as a WAV file.
Result<std::string> sample_wav(std::string_view bytes) {
    auto properties = read_binary_properties(bytes);
    if (!properties) {
        return properties.error();
    }
    const Section& sample = properties.value();
    const std::int64_t format = integer(sample, "format", 0);
    const Value* stereo = sample.property("stereo");
    const std::uint32_t channels = stereo != nullptr && stereo->boolean ? 2 : 1;
    const auto rate = static_cast<std::uint32_t>(integer(sample, "mix_rate", 44100));
    std::string data = bytes_of(sample.property("data"));
    const WavLoop loop{.mode = static_cast<int>(integer(sample, "loop_mode", 0)),
                       .begin = static_cast<std::uint32_t>(integer(sample, "loop_begin", 0)),
                       .end = static_cast<std::uint32_t>(integer(sample, "loop_end", 0))};
    switch (format) {
    case 0: // 8-bit, signed in Godot and unsigned in WAV files
        for (char& byte : data) {
            byte = static_cast<char>(static_cast<std::uint8_t>(byte) ^ 0x80U);
        }
        return wav_file(channels, rate, 8, data, loop);
    case 1:
        return wav_file(channels, rate, 16, data, loop);
    case 2:
        return wav_file(channels, rate, 16, decode_ima_adpcm(data, channels), loop);
    case 3: {
        auto pcm = decode_qoa(data);
        if (!pcm) {
            return pcm.error();
        }
        return wav_file(pcm.value().channels, pcm.value().rate, 16, pcm.value().samples, loop);
    }
    default:
        return make_error(ErrorCode::InvalidArgument,
                          "a sound in AudioStreamWAV format " + std::to_string(format));
    }
}

// Ogg's CRC-32: polynomial 0x04C11DB7, not reflected, starting at 0.
std::uint32_t ogg_crc(std::string_view bytes) {
    static const std::array<std::uint32_t, 256> kTable = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i << 24U;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 0x80000000U) != 0 ? (value << 1U) ^ 0x04C11DB7U : value << 1U;
            }
            table[i] = value;
        }
        return table;
    }();
    std::uint32_t crc = 0;
    for (const char byte : bytes) {
        crc = (crc << 8U) ^ kTable[((crc >> 24U) ^ static_cast<std::uint8_t>(byte)) & 0xFFU];
    }
    return crc;
}

// An Ogg Vorbis file rebuilt from an OggPacketSequence: its pages' packets and each page's
// granule position, as Godot's importer took them from the file. A page whose packets need more
// than 255 lacing values is split, the packet that crosses continuing on the next.
std::string ogg_file(const std::vector<std::vector<std::string>>& pages,
                     const std::vector<std::int64_t>& granules) {
    constexpr std::uint32_t kSerial = 0x534C4153; // any number names the one stream
    std::string out;
    std::uint32_t sequence = 0;
    const auto write_page = [&](const std::string& segments, const std::string& body,
                                std::uint32_t type, std::int64_t granule) {
        std::string page = "OggS";
        page += '\0';
        page += static_cast<char>(type);
        for (std::uint32_t i = 0; i < 8; ++i) {
            page += static_cast<char>((static_cast<std::uint64_t>(granule) >> (8U * i)) & 0xFFU);
        }
        put_u32(page, kSerial);
        put_u32(page, sequence++);
        put_u32(page, 0); // the CRC, filled below
        page += static_cast<char>(segments.size());
        page += segments;
        page += body;
        const std::uint32_t crc = ogg_crc(page);
        for (std::uint32_t i = 0; i < 4; ++i) {
            page[22 + i] = static_cast<char>((crc >> (8U * i)) & 0xFFU);
        }
        out += page;
    };
    for (std::size_t p = 0; p < pages.size(); ++p) {
        std::string segments;
        std::string body;
        bool continued = false; // the page starts inside a packet
        bool first = p == 0;
        for (std::size_t k = 0; k < pages[p].size(); ++k) {
            std::string_view packet = pages[p][k];
            while (true) {
                const std::size_t take = std::min<std::size_t>(packet.size(), 255);
                segments += static_cast<char>(take);
                body += packet.substr(0, take);
                packet.remove_prefix(take);
                const bool done = take < 255;
                if (segments.size() == 255 && (!done || k + 1 != pages[p].size())) {
                    const std::uint32_t type = (continued ? 1U : 0U) | (first ? 2U : 0U);
                    write_page(segments, body, type, done ? granules[p] : -1);
                    segments.clear();
                    body.clear();
                    continued = !done;
                    first = false;
                }
                if (done) {
                    break;
                }
            }
        }
        if (!segments.empty() || first) {
            const bool last = p + 1 == pages.size();
            const std::uint32_t type = (continued ? 1U : 0U) | (first ? 2U : 0U) | (last ? 4U : 0U);
            write_page(segments, body, type, granules[p]);
        }
    }
    return out;
}

// An AudioStreamOggVorbis resource (.oggvorbisstr) as an Ogg Vorbis file.
Result<std::string> ogg_vorbis_file(std::string_view bytes) {
    auto resource = read_binary_resource(bytes);
    if (!resource) {
        return resource.error();
    }
    const Section* sequence = nullptr;
    for (const Section& section : resource.value().sections) {
        if (section.tag == "sub_resource" && section.field_string("type") == "OggPacketSequence") {
            sequence = &section;
        }
    }
    if (sequence == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "the Ogg sound has no packets");
    }
    std::vector<std::vector<std::string>> pages;
    if (const Value* data = sequence->property("packet_data"); data != nullptr) {
        for (const Value& page : data->items) {
            std::vector<std::string> packets;
            packets.reserve(page.items.size());
            for (const Value& packet : page.items) {
                packets.push_back(bytes_of(&packet));
            }
            pages.push_back(std::move(packets));
        }
    }
    std::vector<std::int64_t> granules;
    if (const Value* positions = sequence->property("granule_positions"); positions != nullptr) {
        for (const Value& position : positions->items) {
            granules.push_back(position.integer);
        }
    }
    if (pages.empty() || granules.size() != pages.size()) {
        return make_error(ErrorCode::InvalidArgument, "the Ogg sound's pages don't add up");
    }
    return ogg_file(pages, granules);
}

// A resource that holds its source file's bytes in `data`: AudioStreamMP3 (.mp3str) and
// FontFile (.fontdata).
Result<std::string> data_file(std::string_view bytes) {
    auto properties = read_binary_properties(bytes);
    if (!properties) {
        return properties.error();
    }
    std::string data = bytes_of(properties.value().property("data"));
    if (data.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the resource holds no file");
    }
    return data;
}

// --- The project -------------------------------------------------------------------------------

bool is_binary_resource(std::string_view bytes) {
    return bytes.starts_with("RSRC") || bytes.starts_with("RSCC");
}

// The [remap] section of a .remap or .import file.
std::optional<Section> remap_section(std::string_view text) {
    auto sections = parse_project_settings(text);
    if (!sections) {
        return std::nullopt;
    }
    for (Section& section : sections.value()) {
        if (section.tag == "remap") {
            return std::move(section);
        }
    }
    return std::nullopt;
}

constexpr std::string_view kOwnKey = "encrypted with the game's own key, which only its maker has";
constexpr std::string_view kLocalization =
    "localization tables come with the localization work on the roadmap";

class Unpacker {
public:
    explicit Unpacker(Pack pack) : pack_(std::move(pack)) {}

    UnpackedGame run() {
        for (const PackFile& file : pack_.files) {
            if ((file.flags & kFileEncrypted) != 0) {
                problems_.add("encrypted file", file.path, std::string(kOwnKey));
            } else if ((file.flags & kFileDelta) != 0) {
                problems_.add("patch file", file.path,
                              "a patch to a file in another pack; unpack that pack");
            } else {
                raw_[file.path] = file.data;
            }
        }
        for (const auto& [path, data] : raw_) {
            if (path.ends_with(".remap")) {
                used_.insert(path);
                remapped(path.substr(0, path.size() - 6), data);
            }
        }
        for (const auto& [path, data] : raw_) {
            if (path.ends_with(".import")) {
                used_.insert(path);
                imported(path.substr(0, path.size() - 7), data);
            }
        }
        if (const auto it = raw_.find("project.binary"); it != raw_.end()) {
            used_.insert(it->first);
            settings(it->second);
        }
        if (const auto it = raw_.find(".godot/uid_cache.bin"); it != raw_.end()) {
            read_uid_cache(it->second);
        }
        for (const auto& [path, data] : raw_) {
            if (!used_.contains(path) && !path.starts_with(".godot/")) {
                plain(path, data);
            }
        }
        write_uid_files();
        rewrite_references();
        return finish();
    }

private:
    // A file Godot replaced with another on export: a text scene or resource with its binary
    // form, a script with its tokens.
    void remapped(const std::string& original, std::string_view remap) {
        const auto section = remap_section(remap);
        const std::string target = section ? strip_res(section->field_string("path")) : "";
        const auto it = raw_.find(target);
        if (it == raw_.end()) {
            problems_.add("missing file", original, "its .remap names a file the pack lacks");
            return;
        }
        used_.insert(target);
        if (extension_of(original) == ".gd") {
            script(original, target, it->second);
        } else if (is_binary_resource(it->second)) {
            text_resource(original, it->second);
        } else {
            add(original, std::string(it->second));
        }
    }

    void script(const std::string& path, const std::string& stored, std::string_view data) {
        const std::string extension = extension_of(stored);
        if (extension == ".gde") {
            problems_.add("encrypted script", path, std::string(kOwnKey));
            return;
        }
        if (extension != ".gdc") {
            add(path, std::string(data));
            ++scripts_;
            return;
        }
        auto source = detokenize_gdscript(data, pack_.tab_size);
        if (!source) {
            problems_.add("unreadable script", path, source.error().message);
            return;
        }
        add(path, std::move(source.value()));
        ++scripts_;
        ++scripts_from_tokens_;
    }

    // A binary scene or resource as the text one it was, renaming a .scn or .res file.
    void text_resource(const std::string& original, std::string_view bytes) {
        auto resource = read_binary_resource(bytes);
        if (!resource) {
            problems_.add("unreadable resource", original, resource.error().message);
            return;
        }
        const bool scene = resource.value().type == "PackedScene";
        std::string path = original;
        const std::string extension = extension_of(original);
        if (extension != ".tscn" && extension != ".tres") {
            path = rename(original, without_extension(original) + (scene ? ".tscn" : ".tres"));
        }
        add(path, write_resource(resource.value().sections));
        ++(scene ? scenes_ : resources_);
    }

    // An asset Godot imported: its imported form turned back into a file.
    void imported(const std::string& source, std::string_view import_text) {
        const auto section = remap_section(import_text);
        if (!section) {
            problems_.add("unreadable file", source + ".import");
            return;
        }
        const std::string type = section->field_string("type");
        const std::string importer = section->field_string("importer");
        if (importer == "keep" || importer == "skip") {
            return; // the file is in the pack as it is, or not at all
        }
        const std::string what = type.empty() ? importer : type;
        // A VRAM-compressed texture has one file per format (path.s3tc, path.etc2...).
        std::string target;
        for (const char* key : {"path", "path.s3tc", "path.bptc", "path.etc2", "path.astc"}) {
            const std::string path = strip_res(section->field_string(key));
            if (!path.empty() && raw_.contains(path)) {
                target = path;
                break;
            }
        }
        if (target.empty()) {
            if (importer == "csv_translation" || type == "Translation") {
                problems_.add("translation", source, std::string(kLocalization));
            } else if (!what.empty()) {
                problems_.add("imported " + what, source, "its imported file isn't in the pack");
            }
            return;
        }
        used_.insert(target);
        const std::string_view data = raw_.at(target);
        Result<std::string> file = std::string();
        std::string extension;
        int* count = nullptr;
        if (type == "CompressedTexture2D") {
            file = texture_png(data);
            extension = ".png";
            count = &textures_;
        } else if (type == "AudioStreamWAV") {
            file = sample_wav(data);
            extension = ".wav";
            count = &sounds_;
        } else if (type == "AudioStreamOggVorbis") {
            file = ogg_vorbis_file(data);
            extension = ".ogg";
            count = &sounds_;
        } else if (type == "AudioStreamMP3") {
            file = data_file(data);
            extension = ".mp3";
            count = &sounds_;
        } else if (type == "FontFile") {
            file = data_file(data);
            extension = extension_of(source);
            count = &fonts_;
        } else if (type == "PackedScene") {
            problems_.add("3D scene", source, "3D models come with the 3D work on the roadmap");
            return;
        } else {
            problems_.add("imported " + what, source, "no SlashEngine equivalent yet");
            return;
        }
        if (!file) {
            problems_.add("unreadable " + what, source, file.error().message);
            return;
        }
        ++*count;
        std::string path = source;
        if (extension_of(source) != extension) {
            path = without_extension(source) + extension;
            if (raw_.contains(path) || files_.contains(path)) {
                path = source + extension;
            }
            path = rename(source, path);
        }
        add(path, std::move(file.value()));
        // The .import file keeps the UID scenes refer to the asset by. Export ends it with a
        // zero byte, which a text file shouldn't have.
        while (import_text.ends_with('\0')) {
            import_text.remove_suffix(1);
        }
        add(path + ".import", std::string(import_text));
    }

    void settings(std::string_view bytes) {
        if (raw_.contains("project.godot")) {
            return;
        }
        auto sections = read_project_binary(bytes);
        if (!sections) {
            problems_.add("unreadable file", "project.binary", sections.error().message);
            return;
        }
        add("project.godot", write_project_settings(sections.value()));
    }

    void read_uid_cache(std::string_view bytes) {
        ByteReader reader(bytes);
        const std::uint32_t count = reader.u32();
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            const auto id = static_cast<std::int64_t>(reader.u64());
            std::string path = strip_res(reader.bytes(reader.u32()));
            if (reader.ok()) {
                uids_.emplace_back(uid_text(id), std::move(path));
            }
        }
    }

    // Any other file: a binary scene or resource as text, a script's tokens as source, and the
    // rest as they are.
    void plain(const std::string& path, std::string_view data) {
        const std::string extension = extension_of(path);
        if ((extension == ".scn" || extension == ".res") && is_binary_resource(data)) {
            text_resource(path, data);
        } else if (extension == ".gde") {
            problems_.add("encrypted script", path, std::string(kOwnKey));
        } else if (extension == ".gdc") {
            script(rename(path, without_extension(path) + ".gd"), path, data);
        } else {
            if (extension == ".gd") {
                ++scripts_;
            } else if (extension == ".translation") {
                problems_.add("translation", path, std::string(kLocalization));
            }
            add(path, std::string(data));
        }
    }

    // Puts the cache's UIDs back where the project kept them: in a scene's or resource's header
    // (export saves those under a path with no UID), and in a .uid file for scripts and other
    // files that keep none of their own, as Godot writes.
    void write_uid_files() {
        for (const auto& [uid, original] : uids_) {
            const std::string path = renamed(original);
            const auto file = files_.find(path);
            if (file == files_.end()) {
                continue;
            }
            const std::string extension = extension_of(path);
            if (extension == ".tscn" || extension == ".tres") {
                std::string& text = file->second;
                const std::size_t end = text.find("]\n");
                if (end != std::string::npos && text.find(" uid=\"") > end) {
                    text.insert(end, " uid=\"" + uid + "\"");
                }
            } else if (!files_.contains(path + ".import")) {
                files_[path + ".uid"] = uid + "\n";
            }
        }
    }

    // Points text files at the files renamed on the way out.
    void rewrite_references() {
        for (auto& [path, contents] : files_) {
            const std::string extension = extension_of(path);
            if (extension != ".tscn" && extension != ".tres" && extension != ".godot" &&
                extension != ".gd") {
                continue;
            }
            for (const auto& [from, to] : renames_) {
                const std::string old_text = "\"res://" + from + "\"";
                const std::string new_text = "\"res://" + to + "\"";
                for (std::size_t at = contents.find(old_text); at != std::string::npos;
                     at = contents.find(old_text, at + new_text.size())) {
                    contents.replace(at, old_text.size(), new_text);
                }
            }
        }
    }

    UnpackedGame finish() {
        UnpackedGame game;
        json::Value& report = game.report;
        report["pack"]["format"] = pack_.format;
        report["pack"]["godot"] = pack_.version;
        report["pack"]["embedded"] = pack_.embedded;
        report["pack"]["files"] = pack_.files.size();
        report["files"] = files_.size();
        report["scenes"] = scenes_;
        report["resources"] = resources_;
        report["scripts"] = scripts_;
        if (scripts_from_tokens_ > 0) {
            report["scripts_from_tokens"] = scripts_from_tokens_;
        }
        report["textures"] = textures_;
        report["sounds"] = sounds_;
        report["fonts"] = fonts_;
        if (!renames_.empty()) {
            json::Value renamed = json::Object{};
            for (const auto& [from, to] : renames_) {
                renamed[from] = to;
            }
            report["renamed"] = std::move(renamed);
        }
        report["unsupported"] = problems_.to_json();
        game.files = std::move(files_);
        return game;
    }

    void add(const std::string& path, std::string contents) { files_[path] = std::move(contents); }

    std::string rename(const std::string& from, std::string to) {
        renames_.emplace_back(from, to);
        return to;
    }

    [[nodiscard]] std::string renamed(const std::string& path) const {
        for (const auto& [from, to] : renames_) {
            if (from == path) {
                return to;
            }
        }
        return path;
    }

    Pack pack_;
    std::map<std::string, std::string_view> raw_;
    std::set<std::string> used_;
    std::map<std::string, std::string> files_;
    std::vector<std::pair<std::string, std::string>> renames_;
    std::vector<std::pair<std::string, std::string>> uids_;
    Problems problems_;
    int scenes_ = 0;
    int resources_ = 0;
    int scripts_ = 0;
    int scripts_from_tokens_ = 0;
    int textures_ = 0;
    int sounds_ = 0;
    int fonts_ = 0;
};

} // namespace

Result<UnpackedGame> unpack_game(std::string_view bytes) {
    auto pack = read_pack(bytes);
    if (!pack) {
        return pack.error();
    }
    return Unpacker(std::move(pack.value())).run();
}

} // namespace slash::importer::godot
