#pragma once

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// glTF files written in tests, so tests make the models they read rather than commit them.
namespace slash::test {

inline json::Value gltf_json(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// A glTF file a test writes: its JSON and the one buffer its accessors read.
struct Gltf {
    json::Value document = gltf_json(R"({"asset": {"version": "2.0"}})");
    std::string bin;

    // Appends bytes to the buffer, at a multiple of four, as a buffer view; returns its index.
    std::int64_t view(std::string_view bytes) {
        while (bin.size() % 4 != 0) {
            bin.push_back('\0');
        }
        json::Value out;
        out["buffer"] = 0;
        out["byteOffset"] = bin.size();
        out["byteLength"] = bytes.size();
        bin.append(bytes);
        document["bufferViews"].push_back(std::move(out));
        return static_cast<std::int64_t>(document["bufferViews"].size()) - 1;
    }

    std::int64_t accessor(json::Value out) {
        document["accessors"].push_back(std::move(out));
        return static_cast<std::int64_t>(document["accessors"].size()) - 1;
    }

    // An accessor of floats, as many to an element as type says ("VEC2", "VEC3" or "VEC4").
    std::int64_t floats(const std::vector<float>& values, std::string_view type) {
        std::string bytes(values.size() * sizeof(float), '\0');
        std::memcpy(bytes.data(), values.data(), bytes.size());
        const std::size_t size = type == "VEC2" ? 2 : type == "VEC3" ? 3 : 4;
        json::Value out;
        out["bufferView"] = view(bytes);
        out["componentType"] = 5126;
        out["count"] = values.size() / size;
        out["type"] = type;
        if (type == "VEC3") {
            // POSITION accessors give their bounds.
            json::Array low(3, json::Value(1e9));
            json::Array high(3, json::Value(-1e9));
            for (std::size_t i = 0; i < values.size(); ++i) {
                const double value = values[i];
                low[i % 3] = std::min(low[i % 3].as_double(), value);
                high[i % 3] = std::max(high[i % 3].as_double(), value);
            }
            out["min"] = std::move(low);
            out["max"] = std::move(high);
        }
        return accessor(std::move(out));
    }

    std::int64_t indices(const std::vector<std::uint16_t>& values) {
        std::string bytes(values.size() * sizeof(std::uint16_t), '\0');
        std::memcpy(bytes.data(), values.data(), bytes.size());
        json::Value out;
        out["bufferView"] = view(bytes);
        out["componentType"] = 5123;
        out["count"] = values.size();
        out["type"] = "SCALAR";
        return accessor(std::move(out));
    }

    // An image inside the buffer.
    std::int64_t image(std::string_view bytes, std::string_view mime) {
        json::Value out;
        out["bufferView"] = view(bytes);
        out["mimeType"] = mime;
        document["images"].push_back(std::move(out));
        return static_cast<std::int64_t>(document["images"].size()) - 1;
    }

    // As a .gltf file: its buffer named by uri, or in a base64 data URI.
    [[nodiscard]] std::string gltf(std::string_view uri = "") const {
        json::Value out = document;
        json::Value buffer;
        buffer["byteLength"] = bin.size();
        buffer["uri"] = uri.empty() ? "data:application/octet-stream;base64," + base64_encode(bin)
                                    : std::string(uri);
        out["buffers"].push_back(std::move(buffer));
        return json::write(out);
    }

    // As a .glb file: a JSON chunk, then the buffer in a binary chunk.
    [[nodiscard]] std::string glb() const {
        json::Value out = document;
        json::Value buffer;
        buffer["byteLength"] = bin.size();
        out["buffers"].push_back(std::move(buffer));
        std::string text = json::write(out, json::Style::Compact);
        text.append((4 - (text.size() % 4)) % 4, ' ');
        std::string data = bin;
        data.append((4 - (data.size() % 4)) % 4, '\0');
        std::string file;
        const auto word = [&](std::uint32_t value) {
            for (int i = 0; i < 4; ++i) {
                file.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
            }
        };
        file += "glTF";
        word(2);
        word(static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + data.size()));
        word(static_cast<std::uint32_t>(text.size()));
        file += "JSON";
        file += text;
        word(static_cast<std::uint32_t>(data.size()));
        file.append("BIN\0", 4);
        file += data;
        return file;
    }
};

// A quad from -1 to 1 in x and y, facing +z with its texture's top at +y, as glTF writes one:
// counter-clockwise front faces. Its one primitive has the material given, or none for -1.
inline Gltf quad(std::int64_t material = -1, bool normals = true, bool uvs = true) {
    Gltf out;
    json::Value attributes;
    attributes["POSITION"] = out.floats(
        {-1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F, 0.0F}, "VEC3");
    if (normals) {
        attributes["NORMAL"] = out.floats(
            {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F}, "VEC3");
    }
    if (uvs) {
        attributes["TEXCOORD_0"] =
            out.floats({0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F}, "VEC2");
    }
    json::Value primitive;
    primitive["attributes"] = std::move(attributes);
    primitive["indices"] = out.indices({0, 1, 2, 0, 2, 3});
    if (material >= 0) {
        primitive["material"] = material;
    }
    json::Value mesh;
    mesh["name"] = "Quad";
    mesh["primitives"].push_back(std::move(primitive));
    out.document["meshes"].push_back(std::move(mesh));
    out.document["nodes"] = gltf_json(R"([{"name": "Quad", "mesh": 0}])");
    out.document["scenes"] = gltf_json(R"([{"nodes": [0]}])");
    out.document["scene"] = 0;
    return out;
}

} // namespace slash::test
