#include "godot_terrain.hpp"

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"

#include <bit>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace slash::importer::godot {

namespace {

// HTerrainData's map types' names, which name their files.
constexpr std::array<std::string_view, kTerrainMapTypes> kMapNames{
    "height", "normal", "splat", "color", "detail", "global_albedo", "splat_index", "splat_weight"};

// HTerrainData::_get_map_filename's sizes: a power of two plus one, from 65 to 4097. Larger
// images are refused before their bytes are read.
constexpr int kMaxResolution = 4097;

// The RGB8 height encoding's constants (_V2_UNIT_STEPS and _V2_MIN).
constexpr double kUnitSteps = 1024.0;
constexpr double kLowest = -8192.0;

Error invalid(std::string message) {
    return Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)};
}

// A PackedByteArray's bytes: base64, as Godot 4.3 and later write them, or a number each.
std::optional<std::string> packed_bytes(const Value& value) {
    if (!value.is_call("PackedByteArray")) {
        return std::nullopt;
    }
    if (value.items.empty()) {
        return std::string();
    }
    if (value.items.size() == 1 && value.items.front().is_string()) {
        return base64_decode(value.items.front().text);
    }
    std::string out;
    out.reserve(value.items.size());
    for (const Value& item : value.items) {
        if (item.kind != Value::Kind::Int || item.integer < 0 || item.integer > 255) {
            return std::nullopt;
        }
        out += static_cast<char>(item.integer);
    }
    return out;
}

} // namespace

Result<TerrainMetadata> parse_terrain_metadata(std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed || !parsed.value().is_object()) {
        return invalid("not the JSON HTerrainData saves");
    }
    const json::Value& root = parsed.value();
    TerrainMetadata out;
    if (const json::Value* version = root.find("version");
        version != nullptr && version->is_string()) {
        out.version = version->as_string();
    }
    const json::Value* maps = root.find("maps");
    if (maps == nullptr || !maps->is_array() || maps->as_array().size() > kTerrainMapTypes) {
        return invalid("no list of maps");
    }
    for (std::size_t type = 0; type < maps->as_array().size(); ++type) {
        const json::Value& list = maps->as_array()[type];
        if (!list.is_array()) {
            return invalid("maps/" + std::to_string(type) + " isn't a list");
        }
        for (const json::Value& map : list.as_array()) {
            const json::Value* id = map.is_object() ? map.find("id") : nullptr;
            if (id == nullptr || !id->is_number() || id->as_double() < 0.0) {
                return invalid("a map of maps/" + std::to_string(type) + " has no ID");
            }
            out.maps.at(type).push_back(static_cast<int>(id->as_double()));
        }
    }
    return out;
}

std::string terrain_map_file(TerrainMap type, int id) {
    std::string out(kMapNames.at(static_cast<std::size_t>(type)));
    if (id > 0) {
        out += std::to_string(id + 1);
    }
    out += type == TerrainMap::Height ? ".res" : ".png";
    return out;
}

Result<TerrainHeights> terrain_heights_of_image(const Value& data) {
    const Value* width = data.get("width");
    const Value* height = data.get("height");
    const Value* format = data.get("format");
    const Value* bytes_value = data.get("data");
    if (width == nullptr || height == nullptr || format == nullptr || bytes_value == nullptr ||
        width->kind != Value::Kind::Int || height->kind != Value::Kind::Int ||
        !format->is_string()) {
        return invalid("not an Image");
    }
    if (width->integer != height->integer || width->integer < 2 ||
        width->integer > kMaxResolution) {
        return invalid("a heightmap of " + std::to_string(width->integer) + " by " +
                       std::to_string(height->integer) + " pixels");
    }
    if (format->text != "RFloat") {
        return invalid("a heightmap of " + format->text + " pixels");
    }
    constexpr std::size_t size = 4;
    const auto bytes = packed_bytes(*bytes_value);
    TerrainHeights out;
    out.resolution = static_cast<int>(width->integer);
    const std::size_t count =
        static_cast<std::size_t>(out.resolution) * static_cast<std::size_t>(out.resolution);
    // A mipmapped image keeps its levels after the first.
    if (!bytes || bytes->size() < count * size) {
        return invalid("an Image with fewer bytes than pixels");
    }
    out.heights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto byte = [&](std::size_t at) {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>((*bytes)[(i * size) + at]));
        };
        const std::uint32_t bits = byte(0) | (byte(1) << 8U) | (byte(2) << 16U) | (byte(3) << 24U);
        out.heights[i] = std::bit_cast<float>(bits);
        if (!std::isfinite(out.heights[i])) {
            return invalid("a height that isn't a number");
        }
    }
    return out;
}

Result<TerrainHeights> terrain_heights_of_rgb8(int width, int height,
                                               const std::vector<std::uint8_t>& rgba) {
    if (width != height || width < 2 || width > kMaxResolution) {
        return invalid("a heightmap of " + std::to_string(width) + " by " + std::to_string(height) +
                       " pixels");
    }
    TerrainHeights out;
    out.resolution = width;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(width);
    if (rgba.size() < count * 4) {
        return invalid("fewer pixels than its size");
    }
    out.heights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // (r + g * 256 + b * 65536) / 1024 - 8192, as decode_height_from_rgb8_unorm works out.
        const double steps = static_cast<double>(rgba[i * 4]) +
                             (static_cast<double>(rgba[(i * 4) + 1]) * 256.0) +
                             (static_cast<double>(rgba[(i * 4) + 2]) * 65536.0);
        out.heights[i] = static_cast<float>((steps / kUnitSteps) + kLowest);
    }
    return out;
}

} // namespace slash::importer::godot
