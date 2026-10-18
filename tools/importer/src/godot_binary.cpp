#include "godot_binary.hpp"

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <zstd.h>

namespace slash::importer::godot {

std::uint8_t ByteReader::u8() {
    const std::string_view got = bytes(1);
    return got.empty() ? 0 : static_cast<std::uint8_t>(got[0]);
}

std::uint16_t ByteReader::u16() {
    const std::string_view got = bytes(2);
    if (got.size() != 2) {
        return 0;
    }
    const auto b0 = static_cast<std::uint16_t>(static_cast<std::uint8_t>(got[0]));
    const auto b1 = static_cast<std::uint16_t>(static_cast<std::uint8_t>(got[1]));
    return big_endian_ ? static_cast<std::uint16_t>((b0 << 8U) | b1)
                       : static_cast<std::uint16_t>(b0 | (b1 << 8U));
}

std::uint32_t ByteReader::u32() {
    const std::string_view got = bytes(4);
    if (got.size() != 4) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const auto byte = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(got[static_cast<std::size_t>(big_endian_ ? i : 3 - i)]));
        value = (value << 8U) | byte;
    }
    return value;
}

std::uint64_t ByteReader::u64() {
    const std::uint64_t first = u32();
    const std::uint64_t second = u32();
    return big_endian_ ? (first << 32U) | second : first | (second << 32U);
}

float ByteReader::f32() {
    return std::bit_cast<float>(u32());
}

double ByteReader::f64() {
    return std::bit_cast<double>(u64());
}

std::string_view ByteReader::bytes(std::size_t size) {
    if (failed_ || size > bytes_.size() - at_) {
        failed_ = true;
        at_ = bytes_.size();
        return {};
    }
    const std::string_view out = bytes_.substr(at_, size);
    at_ += size;
    return out;
}

void ByteReader::seek(std::size_t at) {
    if (at > bytes_.size()) {
        failed_ = true;
        at_ = bytes_.size();
        return;
    }
    at_ = at;
}

void ByteWriter::u8(std::uint32_t value) {
    out_ += static_cast<char>(value & 0xFFU);
}

void ByteWriter::u16(std::uint32_t value) {
    u8(value);
    u8(value >> 8U);
}

void ByteWriter::u32(std::uint32_t value) {
    u16(value & 0xFFFFU);
    u16(value >> 16U);
}

void ByteWriter::u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    u32(static_cast<std::uint32_t>(value >> 32U));
}

void ByteWriter::f32(float value) {
    u32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::f64(double value) {
    u64(std::bit_cast<std::uint64_t>(value));
}

void ByteWriter::pad(std::size_t alignment) {
    while (out_.size() % alignment != 0) {
        out_ += '\0';
    }
}

void ByteWriter::patch_u64(std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out_[at + i] = static_cast<char>((value >> (8U * i)) & 0xFFU);
    }
}

namespace {
constexpr std::string_view kUidCharacters = "abcdefghijklmnopqrstuvwxy012345678";
} // namespace

std::string uid_text(std::int64_t id) {
    if (id < 0) {
        return "uid://<invalid>";
    }
    std::string digits;
    auto rest = static_cast<std::uint64_t>(id);
    do {
        digits += kUidCharacters[rest % kUidCharacters.size()];
        rest /= kUidCharacters.size();
    } while (rest != 0);
    std::ranges::reverse(digits);
    return "uid://" + digits;
}

std::int64_t uid_id(std::string_view text) {
    if (!text.starts_with("uid://") || text.size() == 6) {
        return -1;
    }
    std::uint64_t id = 0;
    for (const char c : text.substr(6)) {
        const std::size_t digit = kUidCharacters.find(c);
        if (digit == std::string_view::npos) {
            return -1;
        }
        id = (id * kUidCharacters.size()) + digit;
    }
    return static_cast<std::int64_t>(id & 0x7FFFFFFFFFFFFFFFU);
}

std::string zstd_store(std::string_view bytes) {
    // RFC 8878: a single-segment frame with a four-byte content size, then raw blocks of at most
    // 128 KiB, the last one flagged.
    constexpr std::size_t kMaxBlock = std::size_t{128} * 1024;
    ByteWriter out;
    out.u32(0xFD2FB528U);
    out.u8(0xA0U);
    out.u32(static_cast<std::uint32_t>(bytes.size()));
    std::size_t at = 0;
    do {
        const std::size_t size = std::min(kMaxBlock, bytes.size() - at);
        const bool last = at + size == bytes.size();
        const auto header = static_cast<std::uint32_t>((size << 3U) | (last ? 1U : 0U));
        out.u16(header & 0xFFFFU);
        out.u8(header >> 16U);
        out.bytes(bytes.substr(at, size));
        at += size;
    } while (at < bytes.size());
    return out.take();
}

std::array<std::uint8_t, 16> md5_digest(std::string_view bytes) {
    // RFC 1321.
    static constexpr std::array<std::uint32_t, 64> kSines = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static constexpr std::array<std::uint32_t, 16> kShifts = {7, 12, 17, 22, 5, 9,  14, 20,
                                                              4, 11, 16, 23, 6, 10, 15, 21};
    std::string message(bytes);
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) {
        message += '\0';
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        message += static_cast<char>((bits >> (8U * i)) & 0xFFU);
    }
    std::array<std::uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::array<std::uint32_t, 16> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t k = 0; k < 4; ++k) {
                words[i] |= static_cast<std::uint32_t>(
                                static_cast<std::uint8_t>(message[chunk + (i * 4) + k]))
                            << (8U * k);
            }
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = ((5 * i) + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = ((3 * i) + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f += a + kSines[i] + words[g];
            a = d;
            d = c;
            c = b;
            // By hand: std::rotl's count is an int in libstdc++ but unsigned in Apple's libc++.
            const std::uint32_t shift = kShifts[((i / 16) * 4) + (i % 4)];
            b += (f << shift) | (f >> (32U - shift));
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
    std::array<std::uint8_t, 16> digest{};
    for (std::size_t i = 0; i < 16; ++i) {
        digest[i] = static_cast<std::uint8_t>((state[i / 4] >> (8U * (i % 4))) & 0xFFU);
    }
    return digest;
}

std::string md5_hex(std::string_view bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : md5_digest(bytes)) {
        out += kHex[byte >> 4U];
        out += kHex[byte & 0xFU];
    }
    return out;
}

Result<std::string> zstd_decompress(std::string_view bytes, std::size_t size) {
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(),
                                                                       &ZSTD_freeDCtx);
    if (context == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "can't start a zstd decoder");
    }
    // Godot compresses with long-distance matching and a window of up to 2^27 bytes.
    ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 31);
    std::string out(size, '\0');
    const std::size_t got =
        ZSTD_decompressDCtx(context.get(), out.data(), out.size(), bytes.data(), bytes.size());
    if (ZSTD_isError(got) != 0U) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("bad zstd data: ") + ZSTD_getErrorName(got));
    }
    if (got != size) {
        return make_error(ErrorCode::InvalidArgument, "zstd data is shorter than its header says");
    }
    return out;
}

namespace {

Value make_kind(Value::Kind kind) {
    Value value;
    value.kind = kind;
    return value;
}

Value make_bool(bool flag) {
    Value value = make_kind(Value::Kind::Bool);
    value.boolean = flag;
    return value;
}

Value make_int(std::int64_t number) {
    Value value = make_kind(Value::Kind::Int);
    value.integer = number;
    return value;
}

Value make_real(double number) {
    Value value = make_kind(Value::Kind::Float);
    value.real = number;
    return value;
}

// A 32-bit float as the shortest decimal that reads back as it, so 0.1F comes back as 0.1.
Value make_float32(float number) {
    return make_real(json::Value(number).as_double());
}

Value make_string(std::string text, Value::Kind kind = Value::Kind::String) {
    Value value = make_kind(kind);
    value.text = std::move(text);
    return value;
}

Value make_call(std::string name, std::vector<Value> items = {}) {
    Value value = make_kind(Value::Kind::Call);
    value.text = std::move(name);
    value.items = std::move(items);
    return value;
}

// A PackedByteArray as Godot 4.3 and later write it, its bytes in base64.
Value byte_array(std::string_view bytes) {
    Value out = make_call("PackedByteArray");
    if (!bytes.empty()) {
        out.items.push_back(make_string(base64_encode(bytes)));
    }
    return out;
}

// A NodePath as Godot writes it: names joined by '/', then each subname after ':'.
std::string node_path_text(const std::vector<std::string>& names,
                           const std::vector<std::string>& subnames, bool absolute) {
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i > 0 ? "/" : "") + names[i];
    }
    for (const std::string& subname : subnames) {
        out += ":" + subname;
    }
    return out;
}

Result<std::string> decompress_rscc(ByteReader& reader) {
    const std::uint32_t mode = reader.u32();
    const std::uint32_t block_size = reader.u32();
    const std::uint32_t total = reader.u32();
    constexpr std::uint32_t kZstd = 2;
    if (!reader.ok() || block_size == 0) {
        return make_error(ErrorCode::InvalidArgument, "corrupt compressed resource header");
    }
    if (mode != kZstd) {
        return make_error(ErrorCode::InvalidArgument,
                          "compressed with Godot's compression mode " + std::to_string(mode) +
                              "; only zstd (2), Godot's default, is read");
    }
    const std::uint32_t blocks = (total / block_size) + 1;
    std::vector<std::uint32_t> sizes;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        sizes.push_back(reader.u32());
    }
    std::string out;
    out.reserve(total);
    for (std::uint32_t i = 0; i < blocks; ++i) {
        const std::string_view packed = reader.bytes(sizes[i]);
        const std::size_t size = i + 1 < blocks ? block_size : total - (block_size * i);
        if (!reader.ok()) {
            return make_error(ErrorCode::InvalidArgument, "compressed resource is cut short");
        }
        if (size == 0) {
            continue;
        }
        auto block = zstd_decompress(packed, size);
        if (!block) {
            return block.error();
        }
        out += block.value();
    }
    return out;
}

// --- Binary resources (ResourceLoaderBinary) ---

struct ExtResource {
    std::string type;
    std::string path;
    std::int64_t uid = -1;
};

struct IntResource {
    std::string path; // "local://<id>", or "res://file::<n>" in old files
    std::uint64_t offset = 0;
};

class BinaryReader {
public:
    explicit BinaryReader(std::string_view bytes) : reader_(bytes) {}

    Result<void> open() {
        const std::string_view magic = reader_.bytes(4);
        if (magic == "RSCC") {
            auto plain = decompress_rscc(reader_);
            if (!plain) {
                return plain.error();
            }
            decompressed_ = std::move(plain.value());
            reader_ = ByteReader(decompressed_);
        } else if (magic != "RSRC") {
            return make_error(ErrorCode::InvalidArgument, "not a Godot binary resource");
        }
        reader_.set_big_endian(reader_.u32() != 0);
        reader_.skip(4); // use_real64
        const std::uint32_t major = reader_.u32();
        reader_.skip(4); // minor
        format_ = reader_.u32();
        if (!reader_.ok()) {
            return make_error(ErrorCode::InvalidArgument, "binary resource is cut short");
        }
        if (major < 4) {
            return make_error(ErrorCode::InvalidArgument, "made with Godot " +
                                                              std::to_string(major) +
                                                              "; only Godot 4 resources are read");
        }
        if (format_ > kFormatVersion) {
            return make_error(ErrorCode::InvalidArgument, "binary resource format " +
                                                              std::to_string(format_) +
                                                              " is newer than this reads");
        }
        static_cast<void>(string()); // the main resource's type, read again below
        reader_.skip(8);             // import metadata offset
        const std::uint32_t flags = reader_.u32();
        named_ids_ = (flags & kFlagNamedSceneIds) != 0;
        real_is_double_ = (flags & kFlagRealIsDouble) != 0;
        const std::uint64_t uid = reader_.u64();
        if ((flags & kFlagUids) != 0) {
            uid_ = static_cast<std::int64_t>(uid);
        }
        if ((flags & kFlagHasScriptClass) != 0) {
            script_class_ = string();
        }
        reader_.skip(std::size_t{4} * static_cast<std::size_t>(kReservedFields));
        const std::uint32_t strings = reader_.u32();
        for (std::uint32_t i = 0; i < strings && reader_.ok(); ++i) {
            names_.push_back(string());
        }
        const std::uint32_t externals = reader_.u32();
        for (std::uint32_t i = 0; i < externals && reader_.ok(); ++i) {
            ExtResource ext{.type = string(), .path = string()};
            if ((flags & kFlagUids) != 0) {
                ext.uid = static_cast<std::int64_t>(reader_.u64());
            }
            externals_.push_back(std::move(ext));
        }
        const std::uint32_t internals = reader_.u32();
        for (std::uint32_t i = 0; i < internals && reader_.ok(); ++i) {
            IntResource internal{.path = string()};
            internal.offset = reader_.u64();
            internals_.push_back(std::move(internal));
        }
        if (!reader_.ok() || internals_.empty()) {
            return make_error(ErrorCode::InvalidArgument, "binary resource is cut short");
        }
        return {};
    }

    // Reads every resource into sections: ext resources, sub-resources, then the main one.
    Result<BinaryResource> read() {
        std::vector<Section> subs;
        for (std::size_t i = 0; i + 1 < internals_.size(); ++i) {
            auto section = read_internal(i);
            if (!section) {
                return section.error();
            }
            drop_nulls(section.value());
            section.value().tag = "sub_resource";
            section.value().fields.insert(section.value().fields.begin(),
                                          {"type", make_string(sub_types_[i])});
            section.value().fields.emplace_back("id", make_string(sub_id(i)));
            subs.push_back(std::move(section.value()));
        }
        auto main = read_internal(internals_.size() - 1);
        if (!main) {
            return main.error();
        }
        BinaryResource out;
        out.type = main_type_;
        std::vector<Section> nodes;
        if (main_type_ == "PackedScene") {
            const Value* bundled = main.value().property("_bundled");
            if (bundled == nullptr || bundled->kind != Value::Kind::Dictionary) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a PackedScene with no _bundled data");
            }
            auto scene = scene_sections(*bundled);
            if (!scene) {
                return scene.error();
            }
            nodes = std::move(scene.value());
        }

        Section header;
        const bool scene = main_type_ == "PackedScene";
        header.tag = scene ? "gd_scene" : "gd_resource";
        if (!scene) {
            header.fields.emplace_back("type", make_string(main_type_));
        }
        if (!script_class_.empty()) {
            header.fields.emplace_back("script_class", make_string(script_class_));
        }
        const std::size_t steps = externals_.size() + subs.size() + 1;
        if (steps > 1) {
            header.fields.emplace_back("load_steps", make_int(static_cast<std::int64_t>(steps)));
        }
        header.fields.emplace_back("format", make_int(3));
        if (uid_ >= 0) {
            header.fields.emplace_back("uid", make_string(uid_text(uid_)));
        }
        out.sections.push_back(std::move(header));
        for (std::size_t i = 0; i < externals_.size(); ++i) {
            Section ext;
            ext.tag = "ext_resource";
            ext.fields.emplace_back("type", make_string(externals_[i].type));
            if (externals_[i].uid >= 0) {
                ext.fields.emplace_back("uid", make_string(uid_text(externals_[i].uid)));
            }
            ext.fields.emplace_back("path", make_string(externals_[i].path));
            ext.fields.emplace_back("id", make_string(ext_id(i)));
            out.sections.push_back(std::move(ext));
        }
        for (Section& sub : subs) {
            out.sections.push_back(std::move(sub));
        }
        if (scene) {
            for (Section& node : nodes) {
                out.sections.push_back(std::move(node));
            }
        } else {
            drop_nulls(main.value());
            main.value().tag = "resource";
            out.sections.push_back(std::move(main.value()));
        }
        return out;
    }

    Result<Section> read_main() { return read_internal(internals_.size() - 1); }

private:
    // The binary saver keeps a resource's empty object properties, such as "script = null",
    // which the text saver leaves out.
    static void drop_nulls(Section& section) {
        std::erase_if(section.properties, [](const auto& property) {
            return property.second.kind == Value::Kind::Nil;
        });
    }

    std::string string() {
        const std::uint32_t size = reader_.u32();
        std::string text(reader_.bytes(size));
        // Godot stores the terminating zero.
        while (!text.empty() && text.back() == '\0') {
            text.pop_back();
        }
        return text;
    }

    // A string from the table, or inline when its top bit is set.
    std::string table_string() {
        const std::uint32_t id = reader_.u32();
        if ((id & 0x80000000U) != 0) {
            std::string text(reader_.bytes(id & 0x7FFFFFFFU));
            while (!text.empty() && text.back() == '\0') {
                text.pop_back();
            }
            return text;
        }
        return id < names_.size() ? names_[id] : std::string();
    }

    double real() {
        return real_is_double_ ? reader_.f64() : json::Value(reader_.f32()).as_double();
    }

    Value reals(std::string name, int count) {
        std::vector<Value> items;
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            items.push_back(make_real(real()));
        }
        return make_call(std::move(name), std::move(items));
    }

    Value ints(std::string name, int count) {
        std::vector<Value> items;
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            items.push_back(make_int(static_cast<std::int32_t>(reader_.u32())));
        }
        return make_call(std::move(name), std::move(items));
    }

    static std::string ext_id(std::size_t index) { return std::to_string(index + 1); }

    [[nodiscard]] std::string sub_id(std::size_t index) const {
        const std::string path = internals_[index].path;
        if (path.starts_with("local://")) {
            return path.substr(8);
        }
        const std::size_t at = path.rfind("::");
        return at != std::string::npos ? path.substr(at + 2) : path;
    }

    Result<Value> variant(int depth) {
        if (depth > kMaxDepth) {
            return make_error(ErrorCode::InvalidArgument, "values nest too deeply");
        }
        const std::uint32_t type = reader_.u32();
        switch (type) {
        case kVariantNil:
            return Value();
        case kVariantBool:
            return make_bool(reader_.u32() != 0);
        case kVariantInt:
            return make_int(static_cast<std::int32_t>(reader_.u32()));
        case kVariantInt64:
            return make_int(static_cast<std::int64_t>(reader_.u64()));
        case kVariantFloat:
            return make_real(real());
        case kVariantDouble:
            return make_real(reader_.f64());
        case kVariantString:
            return make_string(string());
        case kVariantStringName:
            return make_string(string(), Value::Kind::StringName);
        case kVariantVector2:
            return reals("Vector2", 2);
        case kVariantVector2i:
            return ints("Vector2i", 2);
        case kVariantRect2:
            return reals("Rect2", 4);
        case kVariantRect2i:
            return ints("Rect2i", 4);
        case kVariantVector3:
            return reals("Vector3", 3);
        case kVariantVector3i:
            return ints("Vector3i", 3);
        case kVariantVector4:
            return reals("Vector4", 4);
        case kVariantVector4i:
            return ints("Vector4i", 4);
        case kVariantPlane:
            return reals("Plane", 4);
        case kVariantQuaternion:
            return reals("Quaternion", 4);
        case kVariantAabb:
            return reals("AABB", 6);
        case kVariantBasis:
            return reals("Basis", 9);
        case kVariantTransform3d:
            return reals("Transform3D", 12);
        case kVariantTransform2d:
            return reals("Transform2D", 6);
        case kVariantProjection:
            return reals("Projection", 16);
        case kVariantColor: {
            std::vector<Value> items;
            items.reserve(4);
            for (int i = 0; i < 4; ++i) {
                items.push_back(make_float32(reader_.f32()));
            }
            return make_call("Color", std::move(items));
        }
        case kVariantNodePath: {
            const std::uint16_t name_count = reader_.u16();
            std::uint32_t subname_count = reader_.u16();
            const bool absolute = (subname_count & 0x8000U) != 0;
            subname_count &= 0x7FFFU;
            if (format_ < kFormatNoNodePathProperty) {
                ++subname_count;
            }
            std::vector<std::string> names;
            std::vector<std::string> subnames;
            for (std::uint16_t i = 0; i < name_count; ++i) {
                names.push_back(table_string());
            }
            for (std::uint32_t i = 0; i < subname_count; ++i) {
                if (std::string subname = table_string(); !subname.empty()) {
                    subnames.push_back(std::move(subname));
                }
            }
            return make_call("NodePath", {make_string(node_path_text(names, subnames, absolute))});
        }
        case kVariantRid:
            reader_.skip(4);
            return make_call("RID");
        case kVariantObject:
            return object();
        case kVariantInputEvent:
        case kVariantCallable:
        case kVariantSignal:
            return Value();
        case kVariantDictionary: {
            const std::uint32_t count = reader_.u32() & 0x7FFFFFFFU;
            Value out = make_kind(Value::Kind::Dictionary);
            for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
                auto key = variant(depth + 1);
                if (!key) {
                    return key.error();
                }
                auto value = variant(depth + 1);
                if (!value) {
                    return value.error();
                }
                out.entries.push_back({std::move(key.value()), std::move(value.value())});
            }
            return out;
        }
        case kVariantArray: {
            const std::uint32_t count = reader_.u32() & 0x7FFFFFFFU;
            Value out = make_kind(Value::Kind::Array);
            for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
                auto item = variant(depth + 1);
                if (!item) {
                    return item.error();
                }
                out.items.push_back(std::move(item.value()));
            }
            return out;
        }
        case kVariantPackedByteArray: {
            const std::uint32_t count = reader_.u32();
            const std::string_view data = reader_.bytes(count);
            reader_.skip((4 - (count % 4)) % 4);
            return byte_array(data);
        }
        case kVariantPackedInt32Array:
            return packed_ints("PackedInt32Array", 4);
        case kVariantPackedInt64Array:
            return packed_ints("PackedInt64Array", 8);
        case kVariantPackedFloat32Array: {
            const std::uint32_t count = reader_.u32();
            Value out = make_call("PackedFloat32Array");
            for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
                out.items.push_back(make_float32(reader_.f32()));
            }
            return out;
        }
        case kVariantPackedFloat64Array: {
            const std::uint32_t count = reader_.u32();
            Value out = make_call("PackedFloat64Array");
            for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
                out.items.push_back(make_real(reader_.f64()));
            }
            return out;
        }
        case kVariantPackedStringArray: {
            const std::uint32_t count = reader_.u32();
            Value out = make_call("PackedStringArray");
            for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
                out.items.push_back(make_string(string()));
            }
            return out;
        }
        case kVariantPackedVector2Array:
            return packed_reals("PackedVector2Array", 2);
        case kVariantPackedVector3Array:
            return packed_reals("PackedVector3Array", 3);
        case kVariantPackedVector4Array:
            return packed_reals("PackedVector4Array", 4);
        case kVariantPackedColorArray: {
            const std::uint32_t count = reader_.u32();
            Value out = make_call("PackedColorArray");
            for (std::uint32_t i = 0; i < count * 4 && reader_.ok(); ++i) {
                out.items.push_back(make_float32(reader_.f32()));
            }
            return out;
        }
        default:
            return make_error(ErrorCode::InvalidArgument,
                              "unknown value type " + std::to_string(type) + " at byte " +
                                  std::to_string(reader_.position() - 4));
        }
    }

    Value packed_ints(std::string name, int size) {
        const std::uint32_t count = reader_.u32();
        Value out = make_call(std::move(name));
        for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
            out.items.push_back(size == 8 ? make_int(static_cast<std::int64_t>(reader_.u64()))
                                          : make_int(static_cast<std::int32_t>(reader_.u32())));
        }
        return out;
    }

    Value packed_reals(std::string name, int components) {
        const std::uint32_t count = reader_.u32();
        Value out = make_call(std::move(name));
        for (std::uint32_t i = 0;
             i < count * static_cast<std::uint32_t>(components) && reader_.ok(); ++i) {
            out.items.push_back(make_real(real()));
        }
        return out;
    }

    Result<Value> object() {
        switch (reader_.u32()) {
        case kObjectEmpty:
            return Value();
        case kObjectInternalResource: {
            const std::uint32_t index = reader_.u32();
            if (named_ids_) {
                if (index >= internals_.size()) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "a sub-resource index out of range");
                }
                return make_call("SubResource", {make_string(sub_id(index))});
            }
            return make_call("SubResource", {make_string(std::to_string(index))});
        }
        case kObjectExternalResource: {
            // Old files name the resource inline; it joins the list.
            ExtResource ext{.type = string(), .path = string()};
            externals_.push_back(std::move(ext));
            return make_call("ExtResource", {make_string(ext_id(externals_.size() - 1))});
        }
        case kObjectExternalResourceIndex: {
            const std::uint32_t index = reader_.u32();
            if (index >= externals_.size()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "an external resource index out of range");
            }
            return make_call("ExtResource", {make_string(ext_id(index))});
        }
        default:
            return make_error(ErrorCode::InvalidArgument, "an object reference of unknown kind");
        }
    }

    Result<Section> read_internal(std::size_t index) {
        reader_.seek(internals_[index].offset);
        const std::string type = string();
        if (index + 1 == internals_.size()) {
            main_type_ = type;
        } else {
            if (sub_types_.size() <= index) {
                sub_types_.resize(index + 1);
            }
            sub_types_[index] = type;
        }
        Section section;
        const std::uint32_t count = reader_.u32();
        for (std::uint32_t i = 0; i < count && reader_.ok(); ++i) {
            std::string name = table_string();
            auto value = variant(0);
            if (!value) {
                return value.error();
            }
            section.properties.emplace_back(std::move(name), std::move(value.value()));
        }
        if (!reader_.ok()) {
            return make_error(ErrorCode::InvalidArgument, "binary resource is cut short");
        }
        return section;
    }

    // --- PackedScene (SceneState::set_bundled_scene and the text saver's _parse_nodes) ---

    struct NodeData {
        std::int32_t parent = -1;
        std::int32_t owner = -1;
        std::int32_t type = -1;
        std::int32_t name = 0;
        std::int32_t index = -1;
        std::int32_t instance = -1;
        std::vector<std::pair<std::int32_t, std::int32_t>> properties;
        std::vector<std::int32_t> groups;
    };

    Result<std::vector<Section>> scene_sections(const Value& bundled) {
        const auto ints_of = [&](std::string_view key) {
            std::vector<std::int32_t> out;
            if (const Value* value = bundled.get(key); value != nullptr) {
                for (const Value& item : value->items) {
                    out.push_back(static_cast<std::int32_t>(item.integer));
                }
            }
            return out;
        };
        const auto int_of = [&](std::string_view key, std::int64_t fallback) {
            const Value* value = bundled.get(key);
            return value != nullptr && value->kind == Value::Kind::Int ? value->integer : fallback;
        };
        std::vector<std::string> names;
        if (const Value* value = bundled.get("names"); value != nullptr) {
            for (const Value& item : value->items) {
                names.push_back(item.text);
            }
        }
        std::vector<Value> variants;
        if (const Value* value = bundled.get("variants"); value != nullptr) {
            variants = value->items;
        }
        std::vector<std::string> node_paths;
        if (const Value* value = bundled.get("node_paths"); value != nullptr) {
            for (const Value& item : value->items) {
                node_paths.push_back(item.string_argument().value_or(""));
            }
        }
        const std::int64_t version = int_of("version", 1);
        const auto node_count = static_cast<std::size_t>(int_of("node_count", 0));
        const auto conn_count = static_cast<std::size_t>(int_of("conn_count", 0));
        const std::int64_t base_scene = int_of("base_scene", -1);
        const std::vector<std::int32_t> raw_nodes = ints_of("nodes");
        const std::vector<std::int32_t> raw_conns = ints_of("conns");

        std::size_t at = 0;
        bool short_data = false;
        const auto next = [&](const std::vector<std::int32_t>& from) {
            if (at >= from.size()) {
                short_data = true;
                return 0;
            }
            return from[at++];
        };
        std::vector<NodeData> nodes(node_count);
        for (NodeData& node : nodes) {
            node.parent = next(raw_nodes);
            node.owner = next(raw_nodes);
            node.type = next(raw_nodes);
            const auto name_index = static_cast<std::uint32_t>(next(raw_nodes));
            node.name = static_cast<std::int32_t>(name_index & ((1U << kNameIndexBits) - 1));
            node.index = static_cast<std::int32_t>(name_index >> kNameIndexBits) - 1;
            node.instance = next(raw_nodes);
            const std::int32_t properties = next(raw_nodes);
            for (std::int32_t j = 0; j < properties && !short_data; ++j) {
                const std::int32_t name = next(raw_nodes);
                node.properties.emplace_back(name, next(raw_nodes));
            }
            const std::int32_t groups = next(raw_nodes);
            for (std::int32_t j = 0; j < groups && !short_data; ++j) {
                node.groups.push_back(next(raw_nodes));
            }
        }
        if (short_data) {
            return make_error(ErrorCode::InvalidArgument, "a scene's node data is cut short");
        }
        const auto name_at = [&](std::int32_t index) {
            return index >= 0 && static_cast<std::size_t>(index) < names.size()
                       ? names[static_cast<std::size_t>(index)]
                       : std::string();
        };
        const auto variant_at = [&](std::int32_t index) {
            return index >= 0 && static_cast<std::size_t>(index) < variants.size()
                       ? variants[static_cast<std::size_t>(index)]
                       : Value();
        };
        const auto path_at = [&](std::int32_t index) {
            return index >= 0 && static_cast<std::size_t>(index) < node_paths.size()
                       ? node_paths[static_cast<std::size_t>(index)]
                       : std::string();
        };
        const auto no_parent = [](std::int32_t parent) {
            return parent < 0 || parent == kNoParentSaved;
        };
        // SceneState::get_node_path, simplified as the text saver writes it.
        const std::function<std::string(std::size_t, bool)> node_path = [&](std::size_t index,
                                                                            bool for_parent) {
            if (no_parent(nodes[index].parent)) {
                return std::string(for_parent ? "" : ".");
            }
            std::vector<std::string> parts;
            std::string base;
            std::size_t walk = index;
            for (int guard = 0; guard <= static_cast<int>(nodes.size()); ++guard) {
                if (no_parent(nodes[walk].parent)) {
                    break;
                }
                if (!for_parent || walk != index) {
                    parts.insert(parts.begin(), name_at(nodes[walk].name));
                }
                if ((nodes[walk].parent & kFlagIdIsPath) != 0) {
                    base = path_at(nodes[walk].parent & kFlagMask);
                    break;
                }
                walk = static_cast<std::size_t>(nodes[walk].parent & kFlagMask);
                if (walk >= nodes.size()) {
                    break;
                }
            }
            std::string out = base;
            for (const std::string& part : parts) {
                out += (out.empty() ? "" : "/") + part;
            }
            return out.empty() ? std::string(".") : out;
        };
        const auto target_path = [&](std::int32_t target) {
            if ((target & kFlagIdIsPath) != 0) {
                return path_at(target & kFlagMask);
            }
            const auto index = static_cast<std::size_t>(target & kFlagMask);
            return index < nodes.size() ? node_path(index, false) : std::string(".");
        };

        std::vector<Section> out;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeData& node = nodes[i];
            Section section;
            section.tag = "node";
            section.fields.emplace_back("name", make_string(name_at(node.name)));
            if (node.type != kTypeInstantiated) {
                section.fields.emplace_back("type", make_string(name_at(node.type)));
            }
            if (const std::string parent = node_path(i, true); !parent.empty()) {
                section.fields.emplace_back("parent", make_string(parent));
            }
            if (!no_parent(node.owner)) {
                const std::string owner =
                    (node.owner & kFlagIdIsPath) != 0
                        ? path_at(node.owner & kFlagMask)
                        : node_path(static_cast<std::size_t>(node.owner & kFlagMask), false);
                if (!owner.empty() && owner != ".") {
                    section.fields.emplace_back("owner", make_string(owner));
                }
            }
            if (node.index >= 0) {
                section.fields.emplace_back("index", make_string(std::to_string(node.index)));
            }
            Value deferred = make_call("PackedStringArray");
            for (const auto& [name, value] : node.properties) {
                if ((name & kFlagPathPropertyIsNode) != 0) {
                    deferred.items.push_back(
                        make_string(name_at(name & (kFlagPathPropertyIsNode - 1))));
                }
            }
            if (!deferred.items.empty()) {
                section.fields.emplace_back("node_paths", std::move(deferred));
            }
            if (!node.groups.empty()) {
                Value groups = make_kind(Value::Kind::Array);
                for (const std::int32_t group : node.groups) {
                    groups.items.push_back(make_string(name_at(group)));
                }
                std::ranges::sort(groups.items, {}, &Value::text);
                section.fields.emplace_back("groups", std::move(groups));
            }
            if (node.instance >= 0) {
                if ((node.instance & kFlagInstanceIsPlaceholder) != 0) {
                    section.fields.emplace_back("instance_placeholder",
                                                variant_at(node.instance & kFlagMask));
                } else {
                    section.fields.emplace_back("instance", variant_at(node.instance & kFlagMask));
                }
            } else if (no_parent(node.parent) && base_scene >= 0) {
                section.fields.emplace_back("instance",
                                            variant_at(static_cast<std::int32_t>(base_scene)));
            }
            for (const auto& [name, value] : node.properties) {
                section.properties.emplace_back(name_at(name & (kFlagPathPropertyIsNode - 1)),
                                                variant_at(value));
            }
            out.push_back(std::move(section));
        }

        at = 0;
        for (std::size_t i = 0; i < conn_count; ++i) {
            const std::int32_t from = next(raw_conns);
            const std::int32_t to = next(raw_conns);
            const std::int32_t signal = next(raw_conns);
            const std::int32_t method = next(raw_conns);
            const std::int32_t flags = next(raw_conns);
            const std::int32_t bind_count = next(raw_conns);
            Value binds = make_kind(Value::Kind::Array);
            for (std::int32_t j = 0; j < bind_count && !short_data; ++j) {
                binds.items.push_back(variant_at(next(raw_conns)));
            }
            const std::int32_t unbinds = version >= 3 ? next(raw_conns) : 0;
            Section section;
            section.tag = "connection";
            section.fields.emplace_back("signal", make_string(name_at(signal)));
            section.fields.emplace_back("from", make_string(target_path(from)));
            section.fields.emplace_back("to", make_string(target_path(to)));
            section.fields.emplace_back("method", make_string(name_at(method)));
            if (flags != kConnectPersist) {
                section.fields.emplace_back("flags", make_int(flags));
            }
            if (unbinds > 0) {
                section.fields.emplace_back("unbinds", make_int(unbinds));
            }
            if (!binds.items.empty()) {
                section.fields.emplace_back("binds", std::move(binds));
            }
            out.push_back(std::move(section));
        }
        if (short_data) {
            return make_error(ErrorCode::InvalidArgument, "a scene's connection data is cut short");
        }
        if (const Value* editable = bundled.get("editable_instances"); editable != nullptr) {
            for (const Value& path : editable->items) {
                Section section;
                section.tag = "editable";
                section.fields.emplace_back("path",
                                            make_string(path.string_argument().value_or("")));
                out.push_back(std::move(section));
            }
        }
        return out;
    }

    ByteReader reader_;
    std::string decompressed_;
    std::uint32_t format_ = 0;
    std::string script_class_;
    std::string main_type_;
    std::int64_t uid_ = -1;
    bool named_ids_ = false;
    bool real_is_double_ = false;
    std::vector<std::string> names_;
    std::vector<ExtResource> externals_;
    std::vector<IntResource> internals_;
    std::vector<std::string> sub_types_;
};

// --- marshalls.cpp ---

std::string marshalled_string(ByteReader& reader) {
    const std::uint32_t size = reader.u32();
    std::string text(reader.bytes(size));
    reader.skip((4 - (size % 4)) % 4);
    // A PackedStringArray's strings keep their terminating zero.
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

// Skips a typed container's element type: none, a built-in type, a class name or a script path.
void skip_container_type(ByteReader& reader, std::uint32_t kind) {
    if (kind == 1) {
        reader.skip(4);
    } else if (kind == 2 || kind == 3) {
        static_cast<void>(marshalled_string(reader));
    }
}

Result<Value> marshalled(ByteReader& reader, int depth) {
    if (depth > kMaxDepth) {
        return make_error(ErrorCode::InvalidArgument, "values nest too deeply");
    }
    const std::uint32_t header = reader.u32();
    const bool wide = (header & kHeaderFlag64) != 0;
    const auto real = [&] { return wide ? reader.f64() : json::Value(reader.f32()).as_double(); };
    const auto reals = [&](std::string name, int count) {
        std::vector<Value> items;
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            items.push_back(make_real(real()));
        }
        return make_call(std::move(name), std::move(items));
    };
    const auto ints = [&](std::string name, int count) {
        std::vector<Value> items;
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            items.push_back(make_int(static_cast<std::int32_t>(reader.u32())));
        }
        return make_call(std::move(name), std::move(items));
    };
    const auto type = static_cast<VariantType>(header & kHeaderTypeMask);
    switch (type) {
    case VariantType::Nil:
        return Value();
    case VariantType::Bool:
        return make_bool(reader.u32() != 0);
    case VariantType::Int:
        return make_int(wide ? static_cast<std::int64_t>(reader.u64())
                             : static_cast<std::int32_t>(reader.u32()));
    case VariantType::Float:
        return make_real(real());
    case VariantType::String:
        return make_string(marshalled_string(reader));
    case VariantType::Vector2:
        return reals("Vector2", 2);
    case VariantType::Vector2i:
        return ints("Vector2i", 2);
    case VariantType::Rect2:
        return reals("Rect2", 4);
    case VariantType::Rect2i:
        return ints("Rect2i", 4);
    case VariantType::Vector3:
        return reals("Vector3", 3);
    case VariantType::Vector3i:
        return ints("Vector3i", 3);
    case VariantType::Transform2d:
        return reals("Transform2D", 6);
    case VariantType::Vector4:
        return reals("Vector4", 4);
    case VariantType::Vector4i:
        return ints("Vector4i", 4);
    case VariantType::Plane:
        return reals("Plane", 4);
    case VariantType::Quaternion:
        return reals("Quaternion", 4);
    case VariantType::Aabb:
        return reals("AABB", 6);
    case VariantType::Basis:
        return reals("Basis", 9);
    case VariantType::Transform3d:
        return reals("Transform3D", 12);
    case VariantType::Projection:
        return reals("Projection", 16);
    case VariantType::Color: {
        std::vector<Value> items;
        items.reserve(4);
        for (int i = 0; i < 4; ++i) {
            items.push_back(make_float32(reader.f32()));
        }
        return make_call("Color", std::move(items));
    }
    case VariantType::StringName:
        return make_string(marshalled_string(reader), Value::Kind::StringName);
    case VariantType::NodePath: {
        const std::uint32_t first = reader.u32();
        if ((first & 0x80000000U) == 0) {
            return make_error(ErrorCode::InvalidArgument, "a node path in Godot 3's encoding");
        }
        const std::uint32_t name_count = first & 0x7FFFFFFFU;
        std::uint32_t subname_count = reader.u32();
        const std::uint32_t flags = reader.u32();
        if ((flags & 2U) != 0) {
            ++subname_count;
        }
        std::vector<std::string> names;
        std::vector<std::string> subnames;
        for (std::uint32_t i = 0; i < name_count + subname_count && reader.ok(); ++i) {
            std::string part = marshalled_string(reader);
            if (i < name_count) {
                names.push_back(std::move(part));
            } else if (!part.empty()) {
                subnames.push_back(std::move(part));
            }
        }
        return make_call("NodePath",
                         {make_string(node_path_text(names, subnames, (flags & 1U) != 0))});
    }
    case VariantType::Rid:
        reader.skip(8);
        return make_call("RID");
    case VariantType::Object: {
        if ((header & kHeaderObjectAsId) != 0) {
            reader.skip(8);
            return Value();
        }
        std::string class_name = marshalled_string(reader);
        if (class_name.empty()) {
            return Value();
        }
        Value out = make_call("Object", {make_string(std::move(class_name))});
        const std::uint32_t count = reader.u32();
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            Value key = make_string(marshalled_string(reader));
            auto value = marshalled(reader, depth + 1);
            if (!value) {
                return value.error();
            }
            out.entries.push_back({std::move(key), std::move(value.value())});
        }
        return out;
    }
    case VariantType::Callable:
        return Value();
    case VariantType::Signal:
        static_cast<void>(marshalled_string(reader));
        reader.skip(8);
        return Value();
    case VariantType::Dictionary: {
        skip_container_type(reader, (header >> 16U) & 3U);
        skip_container_type(reader, (header >> 18U) & 3U);
        const std::uint32_t count = reader.u32() & 0x7FFFFFFFU;
        Value out = make_kind(Value::Kind::Dictionary);
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            auto key = marshalled(reader, depth + 1);
            if (!key) {
                return key.error();
            }
            auto value = marshalled(reader, depth + 1);
            if (!value) {
                return value.error();
            }
            out.entries.push_back({std::move(key.value()), std::move(value.value())});
        }
        return out;
    }
    case VariantType::Array: {
        skip_container_type(reader, (header >> 16U) & 3U);
        const std::uint32_t count = reader.u32() & 0x7FFFFFFFU;
        Value out = make_kind(Value::Kind::Array);
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            auto item = marshalled(reader, depth + 1);
            if (!item) {
                return item.error();
            }
            out.items.push_back(std::move(item.value()));
        }
        return out;
    }
    case VariantType::PackedByteArray: {
        const std::uint32_t count = reader.u32();
        const std::string_view data = reader.bytes(count);
        reader.skip((4 - (count % 4)) % 4);
        return byte_array(data);
    }
    case VariantType::PackedInt32Array:
    case VariantType::PackedInt64Array: {
        const bool long_ints = type == VariantType::PackedInt64Array;
        const std::uint32_t count = reader.u32();
        Value out = make_call(long_ints ? "PackedInt64Array" : "PackedInt32Array");
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            out.items.push_back(long_ints ? make_int(static_cast<std::int64_t>(reader.u64()))
                                          : make_int(static_cast<std::int32_t>(reader.u32())));
        }
        return out;
    }
    case VariantType::PackedFloat32Array:
    case VariantType::PackedFloat64Array: {
        const bool doubles = type == VariantType::PackedFloat64Array;
        const std::uint32_t count = reader.u32();
        Value out = make_call(doubles ? "PackedFloat64Array" : "PackedFloat32Array");
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            out.items.push_back(doubles ? make_real(reader.f64()) : make_float32(reader.f32()));
        }
        return out;
    }
    case VariantType::PackedStringArray: {
        const std::uint32_t count = reader.u32();
        Value out = make_call("PackedStringArray");
        for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
            out.items.push_back(make_string(marshalled_string(reader)));
        }
        return out;
    }
    case VariantType::PackedVector2Array:
    case VariantType::PackedVector3Array:
    case VariantType::PackedVector4Array:
    case VariantType::PackedColorArray: {
        const std::uint32_t count = reader.u32();
        std::uint32_t components = 2;
        std::string name = "PackedVector2Array";
        if (type == VariantType::PackedVector3Array) {
            components = 3;
            name = "PackedVector3Array";
        } else if (type == VariantType::PackedVector4Array) {
            components = 4;
            name = "PackedVector4Array";
        } else if (type == VariantType::PackedColorArray) {
            components = 4;
            name = "PackedColorArray";
        }
        Value out = make_call(std::move(name));
        for (std::uint32_t i = 0; i < count * components && reader.ok(); ++i) {
            out.items.push_back(type == VariantType::PackedColorArray ? make_float32(reader.f32())
                                                                      : make_real(real()));
        }
        return out;
    }
    case VariantType::Max:
        break;
    }
    return make_error(ErrorCode::InvalidArgument,
                      "unknown value type " + std::to_string(header & kHeaderTypeMask));
}

} // namespace

Result<BinaryResource> read_binary_resource(std::string_view bytes) {
    BinaryReader reader(bytes);
    if (auto opened = reader.open(); !opened) {
        return opened.error();
    }
    return reader.read();
}

Result<Section> read_binary_properties(std::string_view bytes) {
    BinaryReader reader(bytes);
    if (auto opened = reader.open(); !opened) {
        return opened.error();
    }
    return reader.read_main();
}

Result<Value> decode_variant(ByteReader& reader) {
    auto value = marshalled(reader, 0);
    if (value && !reader.ok()) {
        return make_error(ErrorCode::InvalidArgument, "a value is cut short");
    }
    return value;
}

Result<std::vector<Section>> read_project_binary(std::string_view bytes) {
    ByteReader reader(bytes);
    if (reader.bytes(4) != "ECFG") {
        return make_error(ErrorCode::InvalidArgument, "not a Godot project.binary file");
    }
    std::vector<Section> sections(1);
    std::map<std::string, std::size_t> by_tag{{"", 0}};
    const std::uint32_t count = reader.u32();
    for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
        const std::uint32_t key_size = reader.u32();
        std::string key(reader.bytes(key_size));
        while (!key.empty() && key.back() == '\0') {
            key.pop_back();
        }
        const std::uint32_t value_size = reader.u32();
        ByteReader value_reader(reader.bytes(value_size));
        auto value = decode_variant(value_reader);
        if (!value) {
            return make_error(ErrorCode::InvalidArgument,
                              "setting \"" + key + "\": " + value.error().message);
        }
        // ProjectSettings::_save_settings_text: the part before the first '/' is the section.
        std::string tag;
        std::string name = key;
        if (const std::size_t slash = key.find('/'); slash != std::string::npos) {
            tag = key.substr(0, slash);
            name = key.substr(slash + 1);
        }
        auto [it, added] = by_tag.try_emplace(tag, sections.size());
        if (added) {
            Section section;
            section.tag = tag;
            sections.push_back(std::move(section));
        }
        sections[it->second].properties.emplace_back(std::move(name), std::move(value.value()));
    }
    if (!reader.ok()) {
        return make_error(ErrorCode::InvalidArgument, "project.binary is cut short");
    }
    return sections;
}

} // namespace slash::importer::godot
