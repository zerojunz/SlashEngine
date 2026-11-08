#include "splats.hpp"

#include "mesh_shading.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::render {
namespace {

// A property's type in a .ply file, by its size in bytes and how it reads.
enum class PlyType : std::uint8_t { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64 };

std::optional<PlyType> ply_type(std::string_view name) {
    if (name == "char" || name == "int8") {
        return PlyType::Int8;
    }
    if (name == "uchar" || name == "uint8") {
        return PlyType::UInt8;
    }
    if (name == "short" || name == "int16") {
        return PlyType::Int16;
    }
    if (name == "ushort" || name == "uint16") {
        return PlyType::UInt16;
    }
    if (name == "int" || name == "int32") {
        return PlyType::Int32;
    }
    if (name == "uint" || name == "uint32") {
        return PlyType::UInt32;
    }
    if (name == "float" || name == "float32") {
        return PlyType::Float32;
    }
    if (name == "double" || name == "float64") {
        return PlyType::Float64;
    }
    return std::nullopt;
}

std::size_t type_size(PlyType type) {
    switch (type) {
    case PlyType::Int8:
    case PlyType::UInt8:
        return 1;
    case PlyType::Int16:
    case PlyType::UInt16:
        return 2;
    case PlyType::Int32:
    case PlyType::UInt32:
    case PlyType::Float32:
        return 4;
    case PlyType::Float64:
        return 8;
    }
    return 0;
}

struct PlyProperty {
    std::string name;
    PlyType type = PlyType::Float32;
};

struct PlyElement {
    std::string name;
    std::size_t count = 0;
    std::vector<PlyProperty> properties;
    bool lists = false; // has a list property, which only elements after the vertices may
};

enum class PlyFormat : std::uint8_t { Ascii, Little, Big };

// The next line of the header from at, without its end.
std::string_view next_line(std::string_view bytes, std::size_t& at) {
    const std::size_t end = bytes.find('\n', at);
    const std::size_t stop = end == std::string_view::npos ? bytes.size() : end;
    std::string_view line = bytes.substr(at, stop - at);
    at = end == std::string_view::npos ? bytes.size() : end + 1;
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t') {
            ++at;
        }
        if (at > start) {
            out.push_back(line.substr(start, at - start));
        }
    }
    return out;
}

double read_binary(const char* data, PlyType type, bool big) {
    std::array<unsigned char, 8> raw{};
    const std::size_t size = type_size(type);
    std::memcpy(raw.data(), data, size);
    if (big) {
        std::reverse(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(size));
    }
    switch (type) {
    case PlyType::Int8:
        return static_cast<double>(static_cast<std::int8_t>(raw[0]));
    case PlyType::UInt8:
        return static_cast<double>(raw[0]);
    case PlyType::Int16: {
        std::int16_t v = 0;
        std::memcpy(&v, raw.data(), 2);
        return v;
    }
    case PlyType::UInt16: {
        std::uint16_t v = 0;
        std::memcpy(&v, raw.data(), 2);
        return v;
    }
    case PlyType::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, raw.data(), 4);
        return v;
    }
    case PlyType::UInt32: {
        std::uint32_t v = 0;
        std::memcpy(&v, raw.data(), 4);
        return v;
    }
    case PlyType::Float32: {
        float v = 0.0F;
        std::memcpy(&v, raw.data(), 4);
        return v;
    }
    case PlyType::Float64: {
        double v = 0.0;
        std::memcpy(&v, raw.data(), 8);
        return v;
    }
    }
    return 0.0;
}

bool little_endian_host() {
    const std::uint16_t one = 1;
    std::uint8_t first = 0;
    std::memcpy(&first, &one, 1);
    return first == 1;
}

float sigmoid(float x) {
    return 1.0F / (1.0F + slash::exp(-x));
}

} // namespace

Result<GaussianSplats> read_splat_ply(std::string_view bytes) {
    std::size_t at = 0;
    if (next_line(bytes, at) != "ply") {
        return make_error(ErrorCode::InvalidArgument, "not a .ply file: it doesn't start with ply");
    }
    std::optional<PlyFormat> format;
    std::vector<PlyElement> elements;
    bool ended = false;
    while (at < bytes.size()) {
        const std::string_view line = next_line(bytes, at);
        const std::vector<std::string_view> w = words(line);
        if (w.empty() || w[0] == "comment" || w[0] == "obj_info") {
            continue;
        }
        if (w[0] == "end_header") {
            ended = true;
            break;
        }
        if (w[0] == "format" && w.size() >= 2) {
            if (w[1] == "ascii") {
                format = PlyFormat::Ascii;
            } else if (w[1] == "binary_little_endian") {
                format = PlyFormat::Little;
            } else if (w[1] == "binary_big_endian") {
                format = PlyFormat::Big;
            } else {
                return make_error(ErrorCode::InvalidArgument,
                                  "the .ply file's format \"" + std::string(w[1]) +
                                      "\" is none of ascii, binary_little_endian and "
                                      "binary_big_endian");
            }
            continue;
        }
        if (w[0] == "element" && w.size() == 3) {
            const std::optional<double> count = parse_decimal(w[2]);
            if (!count || *count < 0.0 || *count != std::floor(*count)) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the .ply file's element " + std::string(w[1]) + " has no count");
            }
            elements.push_back({.name = std::string(w[1]),
                                .count = static_cast<std::size_t>(*count),
                                .properties = {}});
            continue;
        }
        if (w[0] == "property" && !elements.empty()) {
            if (w.size() >= 2 && w[1] == "list") {
                elements.back().lists = true;
                continue;
            }
            const std::optional<PlyType> type = w.size() == 3 ? ply_type(w[1]) : std::nullopt;
            if (!type) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the .ply file has a property it can't read: " +
                                      std::string(line));
            }
            elements.back().properties.push_back({.name = std::string(w[2]), .type = *type});
            continue;
        }
        return make_error(ErrorCode::InvalidArgument,
                          "the .ply file's header has a line it can't read: " + std::string(line));
    }
    if (!ended || !format) {
        return make_error(ErrorCode::InvalidArgument,
                          "the .ply file's header has no format or no end_header");
    }
    const auto vertex = std::ranges::find(elements, std::string("vertex"), &PlyElement::name);
    if (vertex == elements.end()) {
        return make_error(ErrorCode::InvalidArgument, "the .ply file has no vertex element");
    }
    for (auto e = elements.begin(); e != vertex + 1; ++e) {
        if (e->lists) {
            return make_error(ErrorCode::InvalidArgument,
                              "the .ply file's " + e->name +
                                  " element has a list property, which splats don't have");
        }
    }

    // Where each value a splat needs is among the vertex's properties.
    const auto index_of = [&](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < vertex->properties.size(); ++i) {
            if (vertex->properties[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    };
    constexpr std::array<std::string_view, 14> kNeeded = {
        "x",       "y",       "z",       "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
        "scale_0", "scale_1", "scale_2", "rot_0",  "rot_1",  "rot_2",  "rot_3"};
    std::array<std::size_t, 14> needed{};
    for (std::size_t i = 0; i < kNeeded.size(); ++i) {
        const std::optional<std::size_t> found = index_of(kNeeded[i]);
        if (!found) {
            return make_error(ErrorCode::InvalidArgument, "the .ply file's vertices have no " +
                                                              std::string(kNeeded[i]) +
                                                              ", so they aren't Gaussian splats");
        }
        needed[i] = *found;
    }
    std::size_t rest = 0;
    while (index_of("f_rest_" + std::to_string(rest))) {
        ++rest;
    }
    GaussianSplats out;
    // Degree d has 3 * ((d + 1)^2 - 1) rest coefficients.
    constexpr std::array<std::size_t, 4> kRests = {0, 9, 24, 45};
    const auto degree = static_cast<std::size_t>(std::ranges::find(kRests, rest) - kRests.begin());
    if (degree < kRests.size()) {
        out.sh_degree = static_cast<std::uint32_t>(degree);
    } else {
        return make_error(ErrorCode::InvalidArgument,
                          "the .ply file's vertices have " + std::to_string(rest) +
                              " f_rest_ properties, not 0, 9, 24 or 45");
    }
    std::vector<std::size_t> rests(rest);
    for (std::size_t i = 0; i < rest; ++i) {
        rests[i] = *index_of("f_rest_" + std::to_string(i));
    }

    // Each vertex's values, read in order; elements before the vertices are skipped.
    const std::size_t props = vertex->properties.size();
    std::vector<double> values(props);
    std::size_t row_size = 0;
    for (const PlyProperty& property : vertex->properties) {
        row_size += type_size(property.type);
    }
    std::size_t skipped_rows = 0;
    std::size_t skipped_bytes = 0;
    for (auto e = elements.begin(); e != vertex; ++e) {
        skipped_rows += e->count;
        for (const PlyProperty& property : e->properties) {
            skipped_bytes += type_size(property.type) * e->count;
        }
    }
    const bool binary = *format != PlyFormat::Ascii;
    if (binary) {
        at += skipped_bytes;
        if (at > bytes.size() ||
            (bytes.size() - at) / std::max<std::size_t>(row_size, 1) < vertex->count) {
            return make_error(ErrorCode::InvalidArgument, "the .ply file ends before its " +
                                                              std::to_string(vertex->count) +
                                                              " vertices do");
        }
    } else {
        for (std::size_t i = 0; i < skipped_rows; ++i) {
            next_line(bytes, at);
        }
    }
    const std::uint32_t k = out.coefficients();
    out.positions.reserve(vertex->count);
    out.scales.reserve(vertex->count);
    out.rotations.reserve(vertex->count);
    out.opacities.reserve(vertex->count);
    out.sh.reserve(vertex->count * k * 3);
    for (std::size_t v = 0; v < vertex->count; ++v) {
        if (binary) {
            std::size_t offset = 0;
            for (std::size_t p = 0; p < props; ++p) {
                values[p] = read_binary(bytes.data() + at + offset, vertex->properties[p].type,
                                        *format == PlyFormat::Big);
                offset += type_size(vertex->properties[p].type);
            }
            at += row_size;
        } else {
            if (at >= bytes.size()) {
                return make_error(ErrorCode::InvalidArgument, "the .ply file ends before its " +
                                                                  std::to_string(vertex->count) +
                                                                  " vertices do");
            }
            const std::vector<std::string_view> w = words(next_line(bytes, at));
            if (w.size() < props) {
                return make_error(ErrorCode::InvalidArgument,
                                  "vertex " + std::to_string(v) + " of the .ply file has " +
                                      std::to_string(w.size()) + " values, not " +
                                      std::to_string(props));
            }
            for (std::size_t p = 0; p < props; ++p) {
                const std::optional<double> value = parse_decimal(w[p]);
                if (!value) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "vertex " + std::to_string(v) +
                                          " of the .ply file has a value that isn't a number: " +
                                          std::string(w[p]));
                }
                values[p] = *value;
            }
        }
        const auto get = [&](std::size_t i) { return static_cast<float>(values[needed[i]]); };
        out.positions.push_back({get(0), get(1), get(2)});
        out.scales.push_back({slash::exp(get(7)), slash::exp(get(8)), slash::exp(get(9))});
        std::array<float, 4> q{get(10), get(11), get(12), get(13)};
        const float length =
            std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
        if (length > 0.0F && std::isfinite(length)) {
            for (float& c : q) {
                c /= length;
            }
        } else {
            q = {1.0F, 0.0F, 0.0F, 0.0F};
        }
        out.rotations.push_back(q);
        out.opacities.push_back(sigmoid(get(6)));
        // The trainer keeps the rest coefficients a channel at a time: f_rest_(c * (k - 1) + j).
        for (std::uint32_t c = 0; c < k; ++c) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                out.sh.push_back(
                    c == 0 ? get(3 + channel)
                           : static_cast<float>(values[rests[(channel * (k - 1)) + c - 1]]));
            }
        }
    }
    return out;
}

std::string write_splat_ply(const GaussianSplats& splats) {
    const std::uint32_t k = splats.coefficients();
    const std::size_t rest = std::size_t{3} * (k - 1);
    std::string out = "ply\nformat binary_little_endian 1.0\nelement vertex " +
                      std::to_string(splats.size()) + "\n";
    for (const char* name : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"}) {
        out += "property float " + std::string(name) + "\n";
    }
    for (std::size_t i = 0; i < rest; ++i) {
        out += "property float f_rest_" + std::to_string(i) + "\n";
    }
    for (const char* name :
         {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}) {
        out += "property float " + std::string(name) + "\n";
    }
    out += "end_header\n";
    const bool little = little_endian_host();
    const auto put = [&](float value) {
        std::array<char, 4> raw{};
        std::memcpy(raw.data(), &value, 4);
        if (!little) {
            std::reverse(raw.begin(), raw.end());
        }
        out.append(raw.data(), 4);
    };
    for (std::size_t i = 0; i < splats.size(); ++i) {
        for (const float c : splats.positions[i]) {
            put(c);
        }
        put(0.0F);
        put(0.0F);
        put(0.0F);
        const std::size_t base = i * k * 3;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            put(splats.sh[base + channel]);
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            for (std::uint32_t c = 1; c < k; ++c) {
                put(splats.sh[base + (std::size_t{c} * 3) + channel]);
            }
        }
        const float opacity = std::clamp(splats.opacities[i], 1e-6F, 1.0F - 1e-6F);
        put(slash::ln(opacity / (1.0F - opacity)));
        for (const float s : splats.scales[i]) {
            put(slash::ln(std::max(s, 1e-30F)));
        }
        for (const float q : splats.rotations[i]) {
            put(q);
        }
    }
    return out;
}

namespace detail {
namespace {

using Vec3f3 = std::array<float, 3>;
using Mat3 = std::array<float, 9>; // row-major: (row, column) at row * 3 + column

constexpr float kSh0 = 0.28209479177387814F;
constexpr float kSh1 = 0.4886025119029199F;
constexpr std::array<float, 5> kSh2 = {1.0925484305920792F, -1.0925484305920792F,
                                       0.31539156525252005F, -1.0925484305920792F,
                                       0.5462742152960396F};
constexpr std::array<float, 7> kSh3 = {
    -0.5900435899266435F, 2.890611442640554F, -0.4570457994644658F, 0.3731763325901154F,
    -0.4570457994644658F, 1.445305721320277F, -0.5900435899266435F};
// The trainer's limits: alpha at most 0.99, skipped under 1/255, and a pixel done once under
// 1/10000 of the light behind it shows.
constexpr float kMostAlpha = 0.99F;
constexpr float kLeastAlpha = 1.0F / 255.0F;
constexpr float kLeastShowing = 0.0001F;
constexpr float kLowPass = 0.3F;
constexpr float kFrustumMargin = 1.3F;

Mat3 mat3_of(const std::array<float, 16>& m) {
    return {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]};
}

Mat3 mul3(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            out[(r * 3) + c] =
                (a[r * 3] * b[c]) + (a[(r * 3) + 1] * b[3 + c]) + (a[(r * 3) + 2] * b[6 + c]);
        }
    }
    return out;
}

std::optional<Mat3> inverse3(const Mat3& m) {
    const float a = m[0];
    const float b = m[1];
    const float c = m[2];
    const float d = m[3];
    const float e = m[4];
    const float f = m[5];
    const float g = m[6];
    const float h = m[7];
    const float i = m[8];
    const float ei_fh = (e * i) - (f * h);
    const float fg_di = (f * g) - (d * i);
    const float dh_eg = (d * h) - (e * g);
    const float det = (a * ei_fh) + (b * fg_di) + (c * dh_eg);
    if (det == 0.0F || !std::isfinite(det)) {
        return std::nullopt;
    }
    const float s = 1.0F / det;
    return Mat3{ei_fh * s, ((c * h) - (b * i)) * s, ((b * f) - (c * e)) * s,
                fg_di * s, ((a * i) - (c * g)) * s, ((c * d) - (a * f)) * s,
                dh_eg * s, ((b * g) - (a * h)) * s, ((a * e) - (b * d)) * s};
}

Vec3f3 apply3(const Mat3& m, const Vec3f3& v) {
    return {(m[0] * v[0]) + (m[1] * v[1]) + (m[2] * v[2]),
            (m[3] * v[0]) + (m[4] * v[1]) + (m[5] * v[2]),
            (m[6] * v[0]) + (m[7] * v[1]) + (m[8] * v[2])};
}

Vec3f3 normalized(const Vec3f3& v) {
    const float length = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
    if (length == 0.0F) {
        return {0.0F, 0.0F, 1.0F};
    }
    return {v[0] / length, v[1] / length, v[2] / length};
}

// A splat projected to the frame: its center in pixels, the inverse of its covariance there
// (a, b, c of [[a, b], [b, c]]), its linear color, opacity, the depth the opaque meshes are
// tested with, its distance along the view, the tiles it covers and its place in the frame's
// clouds.
struct Projected {
    float u = 0.0F;
    float v = 0.0F;
    std::array<float, 3> conic{};
    std::array<float, 3> color{};
    float opacity = 0.0F;
    float depth = 0.0F;
    float distance = 0.0F;
    std::array<std::uint32_t, 4> tiles{}; // first x, first y, last x, last y
    std::size_t order = 0;
};

} // namespace

std::array<float, 3> splat_color(const GaussianSplats& splats, std::size_t index,
                                 std::uint32_t degree, const std::array<float, 3>& direction) {
    const std::uint32_t k = splats.coefficients();
    const float* sh = splats.sh.data() + (index * k * 3);
    const auto coefficient = [&](std::size_t c, std::size_t channel) {
        return sh[(c * 3) + channel];
    };
    degree = std::min(degree, splats.sh_degree);
    const float x = direction[0];
    const float y = direction[1];
    const float z = direction[2];
    std::array<float, 3> out{};
    for (std::size_t ch = 0; ch < 3; ++ch) {
        float r = kSh0 * coefficient(0, ch);
        if (degree > 0) {
            r = r - (kSh1 * y * coefficient(1, ch)) + (kSh1 * z * coefficient(2, ch)) -
                (kSh1 * x * coefficient(3, ch));
            if (degree > 1) {
                const float xx = x * x;
                const float yy = y * y;
                const float zz = z * z;
                r = r + (kSh2[0] * x * y * coefficient(4, ch)) +
                    (kSh2[1] * y * z * coefficient(5, ch)) +
                    (kSh2[2] * ((2.0F * zz) - xx - yy) * coefficient(6, ch)) +
                    (kSh2[3] * x * z * coefficient(7, ch)) +
                    (kSh2[4] * (xx - yy) * coefficient(8, ch));
                if (degree > 2) {
                    r = r + (kSh3[0] * y * ((3.0F * xx) - yy) * coefficient(9, ch)) +
                        (kSh3[1] * x * y * z * coefficient(10, ch)) +
                        (kSh3[2] * y * ((4.0F * zz) - xx - yy) * coefficient(11, ch)) +
                        (kSh3[3] * z * ((2.0F * zz) - (3.0F * xx) - (3.0F * yy)) *
                         coefficient(12, ch)) +
                        (kSh3[4] * x * ((4.0F * zz) - xx - yy) * coefficient(13, ch)) +
                        (kSh3[5] * z * (xx - yy) * coefficient(14, ch)) +
                        (kSh3[6] * x * (xx - (3.0F * yy)) * coefficient(15, ch));
                }
            }
        }
        out.at(ch) = std::max(r + 0.5F, 0.0F);
    }
    return out;
}

std::vector<float> run_splats(const Splats3D& splats, const SplatFrame& frame,
                              std::span<const float> depth, jobs::Pool* pool) {
    const std::size_t pixels = std::size_t{frame.width} * frame.height;
    std::vector<float> out(pixels * 4, 0.0F);
    for (std::size_t at = 0; at < pixels; ++at) {
        out[(at * 4) + 3] = 1.0F;
    }
    if (pixels == 0) {
        return out;
    }
    const Mat4& vp = frame.view_projection;
    const auto w_size = static_cast<float>(frame.width);
    const auto h_size = static_cast<float>(frame.height);
    const std::uint32_t tiles_x = (frame.width + kSplatTile - 1) / kSplatTile;
    const std::uint32_t tiles_y = (frame.height + kSplatTile - 1) / kSplatTile;

    std::vector<Projected> projected;
    std::size_t order = 0;
    for (const SplatCloud3D& cloud : splats.clouds) {
        if (cloud.splats == nullptr) {
            continue;
        }
        const GaussianSplats& cloud_splats = *cloud.splats;
        const Mat3 model = mat3_of(cloud.transform);
        const std::optional<Mat3> to_local = inverse3(model);
        const std::uint32_t degree = std::min(cloud.sh_degree, cloud_splats.sh_degree);
        for (std::size_t i = 0; i < cloud_splats.size(); ++i, ++order) {
            const Vec3f3& local = cloud_splats.positions[i];
            const std::array<float, 16>& m = cloud.transform;
            const Vec3f3 p{(m[0] * local[0]) + (m[4] * local[1]) + (m[8] * local[2]) + m[12],
                           (m[1] * local[0]) + (m[5] * local[1]) + (m[9] * local[2]) + m[13],
                           (m[2] * local[0]) + (m[6] * local[1]) + (m[10] * local[2]) + m[14]};
            std::array<float, 4> clip{};
            for (std::size_t r = 0; r < 4; ++r) {
                clip.at(r) = (vp.at(r) * p[0]) + (vp.at(4 + r) * p[1]) + (vp.at(8 + r) * p[2]) +
                             vp.at(12 + r);
            }
            const float w = clip[3];
            if (!(w > 1e-6F)) {
                continue; // behind the camera
            }
            const float nx = clip[0] / w;
            const float ny = clip[1] / w;
            const float nz = clip[2] / w;
            if (nz > 1.0F || nz < 0.0F || std::abs(nx) > kFrustumMargin ||
                std::abs(ny) > kFrustumMargin) {
                continue;
            }
            // Its covariance in the world: (M R S)(M R S)^T.
            const std::array<float, 4>& q = cloud_splats.rotations[i];
            const float qw = q[0];
            const float qx = q[1];
            const float qy = q[2];
            const float qz = q[3];
            const Mat3 rotation{
                1.0F - (2.0F * ((qy * qy) + (qz * qz))), 2.0F * ((qx * qy) - (qw * qz)),
                2.0F * ((qx * qz) + (qw * qy)),          2.0F * ((qx * qy) + (qw * qz)),
                1.0F - (2.0F * ((qx * qx) + (qz * qz))), 2.0F * ((qy * qz) - (qw * qx)),
                2.0F * ((qx * qz) - (qw * qy)),          2.0F * ((qy * qz) + (qw * qx)),
                1.0F - (2.0F * ((qx * qx) + (qy * qy)))};
            const Vec3f3& s = cloud_splats.scales[i];
            const Mat3 scale{s[0], 0.0F, 0.0F, 0.0F, s[1], 0.0F, 0.0F, 0.0F, s[2]};
            const Mat3 a = mul3(model, mul3(rotation, scale));
            // The Jacobian of its pixel position by its place in the world, from the whole view
            // projection: d(clip.x / clip.w) = (row 0 - ndc.x * row 3) / w.
            Vec3f3 jx{};
            Vec3f3 jy{};
            for (std::size_t c = 0; c < 3; ++c) {
                jx.at(c) = (vp.at((c * 4) + 0) - (nx * vp.at((c * 4) + 3))) / w * (w_size * 0.5F);
                jy.at(c) = -(vp.at((c * 4) + 1) - (ny * vp.at((c * 4) + 3))) / w * (h_size * 0.5F);
            }
            // J A, then (J A)(J A)^T.
            Vec3f3 ja{};
            Vec3f3 jb{};
            for (std::size_t c = 0; c < 3; ++c) {
                ja.at(c) = (jx[0] * a.at(c)) + (jx[1] * a.at(3 + c)) + (jx[2] * a.at(6 + c));
                jb.at(c) = (jy[0] * a.at(c)) + (jy[1] * a.at(3 + c)) + (jy[2] * a.at(6 + c));
            }
            const float cov_a = (ja[0] * ja[0]) + (ja[1] * ja[1]) + (ja[2] * ja[2]) + kLowPass;
            const float cov_b = (ja[0] * jb[0]) + (ja[1] * jb[1]) + (ja[2] * jb[2]);
            const float cov_c = (jb[0] * jb[0]) + (jb[1] * jb[1]) + (jb[2] * jb[2]) + kLowPass;
            const float det = (cov_a * cov_c) - (cov_b * cov_b);
            if (!(det > 0.0F) || !std::isfinite(det)) {
                continue;
            }
            const float mid = 0.5F * (cov_a + cov_c);
            const float most = mid + std::sqrt(std::max(0.1F, (mid * mid) - det));
            const float radius = std::ceil(3.0F * std::sqrt(most));
            const float u = ((nx + 1.0F) * 0.5F * w_size) - 0.5F;
            const float v = ((1.0F - ny) * 0.5F * h_size) - 0.5F;
            if (u + radius < 0.0F || v + radius < 0.0F || u - radius > w_size - 1.0F ||
                v - radius > h_size - 1.0F) {
                continue;
            }
            const auto tile = [](float value, std::uint32_t tiles) {
                const float t = std::floor(value / static_cast<float>(kSplatTile));
                return static_cast<std::uint32_t>(
                    std::clamp(t, 0.0F, static_cast<float>(tiles - 1)));
            };
            const float opacity = cloud_splats.opacities[i] * cloud.opacity;
            if (opacity < kLeastAlpha) {
                continue;
            }
            Vec3f3 toward{p[0] - frame.camera[0], p[1] - frame.camera[1], p[2] - frame.camera[2]};
            if (to_local) {
                toward = apply3(*to_local, toward);
            }
            const std::array<float, 3> srgb =
                splat_color(cloud_splats, i, degree, normalized(toward));
            Projected out_splat;
            out_splat.u = u;
            out_splat.v = v;
            out_splat.conic = {cov_c / det, -cov_b / det, cov_a / det};
            for (std::size_t c = 0; c < 3; ++c) {
                out_splat.color.at(c) = srgb_to_linear_exact(std::min(srgb.at(c), 1.0F));
            }
            out_splat.opacity = opacity;
            out_splat.depth = nz;
            out_splat.distance = w;
            out_splat.tiles = {tile(u - radius, tiles_x), tile(v - radius, tiles_y),
                               tile(u + radius, tiles_x), tile(v + radius, tiles_y)};
            out_splat.order = order;
            projected.push_back(out_splat);
        }
    }
    std::ranges::sort(projected, [](const Projected& a, const Projected& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.order < b.order;
    });
    std::vector<std::vector<std::uint32_t>> tiles(std::size_t{tiles_x} * tiles_y);
    for (std::size_t i = 0; i < projected.size(); ++i) {
        const Projected& s = projected[i];
        for (std::uint32_t ty = s.tiles[1]; ty <= s.tiles[3]; ++ty) {
            for (std::uint32_t tx = s.tiles[0]; tx <= s.tiles[2]; ++tx) {
                tiles[(std::size_t{ty} * tiles_x) + tx].push_back(static_cast<std::uint32_t>(i));
            }
        }
    }
    const auto blend_tile = [&](std::size_t index) {
        const std::vector<std::uint32_t>& list = tiles[index];
        if (list.empty()) {
            return;
        }
        const auto tx = static_cast<std::uint32_t>(index % tiles_x);
        const auto ty = static_cast<std::uint32_t>(index / tiles_x);
        const std::uint32_t x_end = std::min(frame.width, (tx + 1) * kSplatTile);
        const std::uint32_t y_end = std::min(frame.height, (ty + 1) * kSplatTile);
        for (std::uint32_t y = ty * kSplatTile; y < y_end; ++y) {
            for (std::uint32_t x = tx * kSplatTile; x < x_end; ++x) {
                const std::size_t at = (std::size_t{y} * frame.width) + x;
                const float opaque = at < depth.size() ? depth[at] : 0.0F;
                float showing = 1.0F;
                std::array<float, 3> light{};
                for (const std::uint32_t i : list) {
                    const Projected& s = projected[i];
                    if (s.depth < opaque) {
                        continue; // behind what the meshes drew here
                    }
                    const float dx = s.u - static_cast<float>(x);
                    const float dy = s.v - static_cast<float>(y);
                    const float power =
                        (-0.5F * ((s.conic[0] * dx * dx) + (s.conic[2] * dy * dy))) -
                        (s.conic[1] * dx * dy);
                    if (power > 0.0F) {
                        continue;
                    }
                    const float alpha = std::min(kMostAlpha, s.opacity * slash::exp(power));
                    if (alpha < kLeastAlpha) {
                        continue;
                    }
                    const float next = showing * (1.0F - alpha);
                    if (next < kLeastShowing) {
                        break;
                    }
                    for (std::size_t c = 0; c < 3; ++c) {
                        light.at(c) += s.color.at(c) * alpha * showing;
                    }
                    showing = next;
                }
                out[at * 4] = light[0];
                out[(at * 4) + 1] = light[1];
                out[(at * 4) + 2] = light[2];
                out[(at * 4) + 3] = showing;
            }
        }
    };
    if (pool != nullptr) {
        pool->parallel_for(tiles.size(), blend_tile);
    } else {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            blend_tile(i);
        }
    }
    return out;
}

} // namespace detail
} // namespace slash::render
