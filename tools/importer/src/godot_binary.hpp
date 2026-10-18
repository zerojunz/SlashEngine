#pragma once

#include "slash/core/result.hpp"
#include "slash/importer/godot_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Godot's binary formats, read into the values and sections of its text formats so an exported
// game turns back into the project it was made from, and written from them as Godot's export
// does (godot_pack.cpp): binary resources (.scn, .res and the imported .ctex, .sample and the
// like, resource_format_binary.cpp), values as marshalls.cpp encodes them (project.binary,
// GDScript constants), and UIDs (resource_uid.cpp).
namespace slash::importer::godot {

// --- Binary resources (core/io/resource_format_binary.cpp) ---

// The file's own numbers, which it stores in 32 bits.
// NOLINTNEXTLINE(performance-enum-size)
enum : std::uint32_t {
    kVariantNil = 1,
    kVariantBool = 2,
    kVariantInt = 3,
    kVariantFloat = 4,
    kVariantString = 5,
    kVariantVector2 = 10,
    kVariantRect2 = 11,
    kVariantVector3 = 12,
    kVariantPlane = 13,
    kVariantQuaternion = 14,
    kVariantAabb = 15,
    kVariantBasis = 16,
    kVariantTransform3d = 17,
    kVariantTransform2d = 18,
    kVariantColor = 20,
    kVariantNodePath = 22,
    kVariantRid = 23,
    kVariantObject = 24,
    kVariantInputEvent = 25,
    kVariantDictionary = 26,
    kVariantArray = 30,
    kVariantPackedByteArray = 31,
    kVariantPackedInt32Array = 32,
    kVariantPackedFloat32Array = 33,
    kVariantPackedStringArray = 34,
    kVariantPackedVector3Array = 35,
    kVariantPackedColorArray = 36,
    kVariantPackedVector2Array = 37,
    kVariantInt64 = 40,
    kVariantDouble = 41,
    kVariantCallable = 42,
    kVariantSignal = 43,
    kVariantStringName = 44,
    kVariantVector2i = 45,
    kVariantRect2i = 46,
    kVariantVector3i = 47,
    kVariantPackedInt64Array = 48,
    kVariantPackedFloat64Array = 49,
    kVariantVector4 = 50,
    kVariantVector4i = 51,
    kVariantProjection = 52,
    kVariantPackedVector4Array = 53,
};

// NOLINTNEXTLINE(performance-enum-size)
enum : std::uint32_t {
    kObjectEmpty = 0,
    kObjectExternalResource = 1,
    kObjectInternalResource = 2,
    kObjectExternalResourceIndex = 3,
};

// NOLINTNEXTLINE(performance-enum-size)
enum : std::uint32_t {
    kFlagNamedSceneIds = 1,
    kFlagUids = 2,
    kFlagRealIsDouble = 4,
    kFlagHasScriptClass = 8,
};

constexpr std::uint32_t kFormatVersion = 6;
constexpr std::uint32_t kFormatNoNodePathProperty = 3;
constexpr int kReservedFields = 11;

// Container depth past which values are refused.
constexpr int kMaxDepth = 512;

// PackedScene's bundled data (scene/resources/packed_scene.h).
constexpr std::int32_t kNoParentSaved = 0x7FFFFFFF;
constexpr std::uint32_t kNameIndexBits = 18;
constexpr std::int32_t kFlagIdIsPath = 1 << 30;
constexpr std::int32_t kTypeInstantiated = 0x7FFFFFFF;
constexpr std::int32_t kFlagInstanceIsPlaceholder = 1 << 30;
constexpr std::int32_t kFlagPathPropertyIsNode = 1 << 30;
constexpr std::int32_t kFlagMask = (1 << 24) - 1;
constexpr std::int32_t kPackedSceneVersion = 3;
constexpr std::int32_t kConnectPersist = 2;

// --- Values as marshalls.cpp encodes them: Variant::Type's order ---

// NOLINTNEXTLINE(performance-enum-size): a value's header stores it in 32 bits
enum class VariantType : std::uint32_t {
    Nil,
    Bool,
    Int,
    Float,
    String,
    Vector2,
    Vector2i,
    Rect2,
    Rect2i,
    Vector3,
    Vector3i,
    Transform2d,
    Vector4,
    Vector4i,
    Plane,
    Quaternion,
    Aabb,
    Basis,
    Transform3d,
    Projection,
    Color,
    StringName,
    NodePath,
    Rid,
    Object,
    Callable,
    Signal,
    Dictionary,
    Array,
    PackedByteArray,
    PackedInt32Array,
    PackedInt64Array,
    PackedFloat32Array,
    PackedFloat64Array,
    PackedStringArray,
    PackedVector2Array,
    PackedVector3Array,
    PackedColorArray,
    PackedVector4Array,
    Max
};

constexpr std::uint32_t kHeaderTypeMask = 0xFF;
constexpr std::uint32_t kHeaderFlag64 = 1U << 16U;
constexpr std::uint32_t kHeaderObjectAsId = 1U << 16U;

// Reads little- or big-endian numbers and strings from bytes. A read past the end returns zeros
// and marks the reader failed, so callers check ok() once at the end.
class ByteReader {
public:
    explicit ByteReader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] float f32();
    [[nodiscard]] double f64();
    // `size` bytes, or "" past the end.
    [[nodiscard]] std::string_view bytes(std::size_t size);
    void skip(std::size_t size) { static_cast<void>(bytes(size)); }
    void seek(std::size_t at);

    void set_big_endian(bool big) noexcept { big_endian_ = big; }
    [[nodiscard]] std::size_t position() const noexcept { return at_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t left() const noexcept { return bytes_.size() - at_; }
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

private:
    std::string_view bytes_;
    std::size_t at_ = 0;
    bool big_endian_ = false;
    bool failed_ = false;
};

// Writes little-endian numbers and bytes, as ByteReader reads them.
class ByteWriter {
public:
    void u8(std::uint32_t value);
    void u16(std::uint32_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f32(float value);
    void f64(double value);
    void bytes(std::string_view data) { out_ += data; }
    // Zeros up to a multiple of `alignment` bytes.
    void pad(std::size_t alignment);
    // Overwrites the eight bytes at `at` with `value`.
    void patch_u64(std::size_t at, std::uint64_t value);

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    [[nodiscard]] std::string take() { return std::move(out_); }

private:
    std::string out_;
};

// A UID as Godot writes it, "uid://" and the number in base 34, as ResourceUID::id_to_text.
[[nodiscard]] std::string uid_text(std::int64_t id);

// The number of a UID's text, as ResourceUID::text_to_id; -1 for anything else.
[[nodiscard]] std::int64_t uid_id(std::string_view text);

// A zstd frame holding `bytes` in stored blocks, which zstd_decompress reads back.
[[nodiscard]] std::string zstd_store(std::string_view bytes);

// The MD5 digest of `bytes`, and as 32 lowercase hex digits as String::md5_text writes it.
[[nodiscard]] std::array<std::uint8_t, 16> md5_digest(std::string_view bytes);
[[nodiscard]] std::string md5_hex(std::string_view bytes);

// A binary resource read back into the sections of the text resource Godot saves for it: a
// PackedScene's nodes, connections and editable instances as a .tscn has them, anything else as
// a .tres with its sub-resources and [resource] section.
struct BinaryResource {
    std::string type; // the main resource's class, "PackedScene" for a scene
    std::vector<Section> sections;
};

// Reads a binary resource file (RSRC, or RSCC compressed with zstd). External resources keep the
// res:// paths and UIDs the file names.
[[nodiscard]] Result<BinaryResource> read_binary_resource(std::string_view bytes);

// The main resource's properties of a binary resource, without its sub-resources: what the
// imported formats (.ctex aside) hold, such as an AudioStreamWAV's data.
[[nodiscard]] Result<Section> read_binary_properties(std::string_view bytes);

// Decodes one value as marshalls.cpp's encode_variant writes it, from `reader`'s position.
[[nodiscard]] Result<Value> decode_variant(ByteReader& reader);

// Encodes one value as marshalls.cpp's encode_variant does, objects in full (an Object call's
// class and properties). Callables, signals and RIDs can't be encoded.
[[nodiscard]] Result<std::string> encode_variant(const Value& value);

// Reads project.binary (ECFG) into the sections parse_project_settings returns for project.godot.
[[nodiscard]] Result<std::vector<Section>> read_project_binary(std::string_view bytes);

// Decompresses a zstd frame of known size, as Godot's Compression::decompress with MODE_ZSTD.
[[nodiscard]] Result<std::string> zstd_decompress(std::string_view bytes, std::size_t size);

} // namespace slash::importer::godot
