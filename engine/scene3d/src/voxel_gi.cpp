// VoxelGI, as Godot 4.6's: the voxelizer (scene/3d/voxelizer.cpp), its distance field, the light
// injection Godot's voxel_gi.glsl runs on the GPU (here on the CPU, on the job pool) and baking a
// VoxelGI entity (scene/3d/voxel_gi.cpp). Godot's code is MIT; its triangle and box test is Tomas
// Akenine-Möller's, its distance transform Pedro Felzenszwalb and Daniel Huttenlocher's (after
// Philip Rideout's code), and its RGBE9995 packing Microsoft's MiniEngine (MIT).

#include "slash/scene3d/voxel_gi.hpp"

#include "geometry.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/bits.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace slash::scene3d {

namespace {

using namespace detail;

static_assert(std::endian::native == std::endian::little,
              ".voxelgi.json files hold the machine's bytes, little-endian");
static_assert(sizeof(VoxelGICell) == 16, "cells are kept as their bytes");

// The deepest octree a file may hold: Godot's "512".
constexpr std::size_t kMaxLevels = 10;
// Godot's Voxelizer::color_scan_cell_width.
constexpr int kScan = 4;
constexpr int kMaterialSize = static_cast<int>(kVoxelGIMaterialSize);

Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return Error{ErrorCode::InvalidArgument,
                 (first.path.empty() ? std::string("root") : first.path) + ": " + first.message};
}

template <typename T>
std::string encode_values(const std::vector<T>& values) {
    std::string bytes(values.size() * sizeof(T), '\0');
    if (!values.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return base64_encode(bytes);
}

template <typename T>
std::optional<std::vector<T>> decode_values(const std::string& text) {
    const std::optional<std::string> bytes = base64_decode(text);
    if (!bytes || bytes->size() % sizeof(T) != 0) {
        return std::nullopt;
    }
    std::vector<T> out(bytes->size() / sizeof(T));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes->data(), bytes->size());
    }
    return out;
}

Vec3 vec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

// A cell's place in its level, as voxel_gi.glsl reads it.
std::array<std::uint32_t, 3> cell_position(const VoxelGICell& cell) {
    return {cell[0] & 0x7FFU, (cell[0] >> 11U) & 0x3FFU, cell[0] >> 21U};
}

std::uint64_t cell_key(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (std::uint64_t{x} << 32U) | (std::uint64_t{y} << 16U) | std::uint64_t{z};
}

std::uint64_t cell_key(const VoxelGICell& cell) {
    const auto p = cell_position(cell);
    return cell_key(p[0], p[1], p[2]);
}

// ---- The file

json::Value make_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine VoxelGI data";
    schema["description"] =
        "The cells a VoxelGI voxelized, as Godot's VoxelGIData: voxel_gi.bake writes it.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "bounds", "octree_size", "level_counts", "cells"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["bounds"]["type"] = "array";
    properties["bounds"]["items"]["type"] = "number";
    properties["bounds"]["minItems"] = 6;
    properties["bounds"]["maxItems"] = 6;
    properties["bounds"]["description"] =
        "The box it voxelized, in its VoxelGI's space: position x, y, z, then size x, y, z.";
    properties["octree_size"]["type"] = "array";
    properties["octree_size"]["items"]["type"] = "integer";
    properties["octree_size"]["items"]["minimum"] = 1;
    properties["octree_size"]["items"]["maximum"] = 512;
    properties["octree_size"]["minItems"] = 3;
    properties["octree_size"]["maxItems"] = 3;
    properties["octree_size"]["description"] =
        "Cells along x, y and z at the deepest level, each a power of two.";
    properties["baked_exposure"]["type"] = "number";
    properties["baked_exposure"]["exclusiveMinimum"] = 0;
    properties["baked_exposure"]["default"] = 1;
    properties["baked_exposure"]["description"] =
        "The exposure multiplier emission was baked with; a camera's over it scales the light.";
    properties["level_counts"]["type"] = "array";
    properties["level_counts"]["items"]["type"] = "integer";
    properties["level_counts"]["items"]["minimum"] = 0;
    properties["level_counts"]["minItems"] = 1;
    properties["level_counts"]["maxItems"] = static_cast<std::int64_t>(kMaxLevels);
    properties["level_counts"]["description"] =
        "Cells in each level of the octree, from the root, which is alone in the first.";
    properties["cells"]["type"] = "string";
    properties["cells"]["description"] =
        "Every cell, level by level, each sorted by x, then y, then z, as Godot's VoxelGIData's "
        "data cells: four little-endian 32-bit values, base64. The first is its place (x, y << 11, "
        "z << 21), the second its linear albedo (red in the low byte) and how much of it is solid "
        "(the high byte), the third its emission as RGBE9995 and the fourth its normal, three "
        "signed bytes.";
    return schema;
}

const json::Value& file_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

// The data of a document the schema passed: into data where given, each error into errors.
void read_voxel_gi(const json::Value& doc, VoxelGIData* data,
                   std::vector<json::ValidationError>& errors) {
    VoxelGIData out;
    const json::Array& bounds = doc.find("bounds")->as_array();
    for (std::size_t i = 0; i < 3; ++i) {
        out.bounds_position.at(i) = static_cast<float>(bounds[i].as_double());
        out.bounds_size.at(i) = static_cast<float>(bounds[i + 3].as_double());
        if (!(out.bounds_size.at(i) > 0.0F) || !std::isfinite(out.bounds_size.at(i))) {
            errors.push_back({"/bounds/" + std::to_string(i + 3), "isn't positive"});
        }
    }
    const json::Array& octree_size = doc.find("octree_size")->as_array();
    for (std::size_t i = 0; i < 3; ++i) {
        out.octree_size.at(i) = static_cast<std::uint32_t>(octree_size[i].as_int());
        const std::uint32_t size = out.octree_size.at(i);
        if (size == 0 || (size & (size - 1)) != 0) {
            errors.push_back({"/octree_size/" + std::to_string(i), "isn't a power of two"});
        }
    }
    if (const json::Value* exposure = doc.find("baked_exposure")) {
        out.baked_exposure = static_cast<float>(exposure->as_double());
    }
    for (const json::Value& count : doc.find("level_counts")->as_array()) {
        out.level_counts.push_back(static_cast<std::uint32_t>(count.as_int()));
    }
    if (!errors.empty()) {
        return;
    }
    const std::size_t levels = out.level_counts.size();
    const std::uint32_t longest = std::ranges::max(out.octree_size);
    if (longest != (1U << (levels - 1))) {
        errors.push_back({"/level_counts", "has " + std::to_string(levels) +
                                               " levels, so the longest side of octree_size must "
                                               "be " +
                                               std::to_string(1U << (levels - 1)) + " cells"});
        return;
    }
    if (out.level_counts[0] != 1) {
        errors.push_back({"/level_counts/0", "must be 1: the root"});
        return;
    }
    auto cells = decode_values<VoxelGICell>(doc.find("cells")->as_string());
    if (!cells) {
        errors.push_back({"/cells", "isn't base64 of 16-byte cells"});
        return;
    }
    std::size_t total = 0;
    for (const std::uint32_t count : out.level_counts) {
        total += count;
    }
    if (cells->size() != total) {
        errors.push_back({"/cells", "has " + std::to_string(cells->size()) +
                                        " cells, but level_counts add up to " +
                                        std::to_string(total)});
        return;
    }
    // Each level's cells in order, inside their level, each below a cell of the level above.
    std::size_t first = 0;
    for (std::size_t level = 0; level < levels; ++level) {
        const auto shift = static_cast<std::uint32_t>(levels - 1 - level);
        const std::size_t end = first + out.level_counts[level];
        const std::size_t parents = level == 0 ? 0 : first - out.level_counts[level - 1];
        for (std::size_t i = first; i < end; ++i) {
            const VoxelGICell& cell = (*cells)[i];
            const auto p = cell_position(cell);
            bool inside = true;
            for (std::size_t a = 0; a < 3; ++a) {
                inside = inside && p.at(a) < std::max(out.octree_size.at(a) >> shift, 1U);
            }
            if (!inside) {
                errors.push_back({"/cells", "cell " + std::to_string(i) + " (level " +
                                                std::to_string(level) + ") is outside its level"});
                return;
            }
            if (i > first && cell_key((*cells)[i - 1]) >= cell_key(cell)) {
                errors.push_back({"/cells", "cell " + std::to_string(i) +
                                                " isn't after the one before it in x, y, z order"});
                return;
            }
            if (level > 0) {
                const std::uint64_t parent = cell_key(p[0] >> 1U, p[1] >> 1U, p[2] >> 1U);
                const auto begin = cells->begin() + static_cast<std::ptrdiff_t>(parents);
                const auto stop = cells->begin() + static_cast<std::ptrdiff_t>(first);
                const auto found = std::lower_bound(
                    begin, stop, parent,
                    [](const VoxelGICell& c, std::uint64_t key) { return cell_key(c) < key; });
                if (found == stop || cell_key(*found) != parent) {
                    errors.push_back({"/cells", "cell " + std::to_string(i) +
                                                    " has no parent in the level above"});
                    return;
                }
            }
        }
        first = end;
    }
    out.cells = std::move(*cells);
    if (data != nullptr) {
        *data = std::move(out);
    }
}

std::size_t voxel_gi_bytes(const VoxelGIData& data) {
    return sizeof(VoxelGIData) + (data.cells.capacity() * sizeof(VoxelGICell)) +
           (data.level_counts.capacity() * sizeof(std::uint32_t));
}

// ---- Materials

// sRGB to linear, as Godot's Color::srgb_to_linear, its pow the same on every platform.
float srgb_to_linear(float c) {
    if (c < 0.04045F) {
        return c * (1.0F / 12.92F);
    }
    const auto base = static_cast<float>((static_cast<double>(c) + 0.055) * (1.0 / (1.0 + 0.055)));
    return slash::exp(2.4F * slash::ln(base));
}

// Image::_bicubic_interp_kernel.
double cubic_kernel(double x) {
    x = std::abs(x);
    if (x <= 1.0) {
        return (((1.5 * x) - 2.5) * x * x) + 1.0;
    }
    if (x < 2.0) {
        return ((((-0.5 * x) + 2.5) * x - 4.0) * x) + 2.0;
    }
    return 0.0;
}

// Image::resize to kMaterialSize square with INTERPOLATE_CUBIC (_scale_cubic over RGBA8), or the
// image as it is when it is that size already.
std::vector<std::uint8_t> material_texels(const render::Image& image) {
    const auto width = static_cast<int>(image.width);
    const auto height = static_cast<int>(image.height);
    if (width == kMaterialSize && height == kMaterialSize) {
        return image.pixels;
    }
    std::vector<std::uint8_t> out(std::size_t{kVoxelGIMaterialSize} * kVoxelGIMaterialSize * 4);
    const double xfac = static_cast<double>(width) / kMaterialSize;
    const double yfac = static_cast<double>(height) / kMaterialSize;
    for (int y = 0; y < kMaterialSize; ++y) {
        const double oy = ((static_cast<double>(y) + 0.5) * yfac) - 0.5;
        const auto oy1 = static_cast<int>(oy);
        const double dy = oy - static_cast<double>(oy1);
        for (int x = 0; x < kMaterialSize; ++x) {
            const double ox = ((static_cast<double>(x) + 0.5) * xfac) - 0.5;
            const auto ox1 = static_cast<int>(ox);
            const double dx = ox - static_cast<double>(ox1);
            std::array<double, 4> color{};
            for (int n = -1; n < 3; ++n) {
                const double k1 = cubic_kernel(dy - static_cast<double>(n));
                const int oy2 = std::clamp(oy1 + n, 0, height - 1);
                for (int m = -1; m < 3; ++m) {
                    const double k2 = k1 * cubic_kernel(static_cast<double>(m) - dx);
                    const int ox2 = std::clamp(ox1 + m, 0, width - 1);
                    const std::size_t at = ((static_cast<std::size_t>(oy2) * image.width) +
                                            static_cast<std::size_t>(ox2)) *
                                           4;
                    for (std::size_t c = 0; c < 4; ++c) {
                        color.at(c) += image.pixels[at + c] * k2;
                    }
                }
            }
            const std::size_t to = ((static_cast<std::size_t>(y) * kVoxelGIMaterialSize) +
                                    static_cast<std::size_t>(x)) *
                                   4;
            for (std::size_t c = 0; c < 4; ++c) {
                // Math::fast_ftoi: rounded to nearest, ties to even.
                const float rounded = std::nearbyint(static_cast<float>(color.at(c)));
                out[to + c] = static_cast<std::uint8_t>(std::clamp(rounded, 0.0F, 255.0F));
            }
        }
    }
    return out;
}

// Voxelizer::_get_bake_texture: each texel linear, times mul, plus add; add alone without an
// image.
std::vector<std::array<float, 3>> bake_texture(const render::Image* image,
                                               const std::array<float, 3>& mul,
                                               const std::array<float, 3>& add) {
    const std::size_t count = std::size_t{kVoxelGIMaterialSize} * kVoxelGIMaterialSize;
    if (image == nullptr || image->width == 0 || image->height == 0 ||
        image->pixels.size() < std::size_t{image->width} * image->height * 4) {
        return std::vector<std::array<float, 3>>(count, add);
    }
    const std::vector<std::uint8_t> texels = material_texels(*image);
    std::vector<std::array<float, 3>> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const auto src = static_cast<float>(texels[(i * 4) + c] / 255.0);
            out[i].at(c) = (srgb_to_linear(src) * mul.at(c)) + add.at(c);
        }
    }
    return out;
}

// ---- The voxelizer

constexpr std::uint32_t kChildEmpty = kVoxelGIEmpty;

// Voxelizer::Cell.
struct Cell {
    std::array<std::uint32_t, 8> children{kChildEmpty, kChildEmpty, kChildEmpty, kChildEmpty,
                                          kChildEmpty, kChildEmpty, kChildEmpty, kChildEmpty};
    std::array<float, 3> albedo{};
    std::array<float, 3> emission{};
    std::array<float, 3> normal{};
    float alpha = 0.0F;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t z = 0;
    std::uint16_t level = 0;
};

// Godot's get_uv_and_normal: a point's texture coordinate and normal from its triangle's.
void uv_and_normal(Vec3 pos, const std::array<Vec3, 3>& vtx, const std::array<Vec2, 3>& uv,
                   const std::array<Vec3, 3>& normal, Vec2& r_uv, Vec3& r_normal) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (equal_approx(pos, vtx.at(i))) {
            r_uv = uv.at(i);
            r_normal = normal.at(i);
            return;
        }
    }
    const Vec3 v0 = vtx[1] - vtx[0];
    const Vec3 v1 = vtx[2] - vtx[0];
    const Vec3 v2 = pos - vtx[0];
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = (d00 * d11) - (d01 * d01);
    if (denom == 0.0F) {
        r_uv = uv[0];
        r_normal = normal[0];
        return;
    }
    const float v = ((d11 * d20) - (d01 * d21)) / denom;
    const float w = ((d00 * d21) - (d01 * d20)) / denom;
    const float u = 1.0F - v - w;
    r_uv = (uv[0] * u) + (uv[1] * v) + (uv[2] * w);
    r_normal = normalized((normal[0] * u) + (normal[1] * v) + (normal[2] * w));
}

// Math::fposmod(x, 1).
float wrap(float x) {
    float value = std::fmod(x, 1.0F);
    if (value < 0.0F) {
        value += 1.0F;
    }
    return value + 0.0F;
}

int texel_of(float coordinate) {
    return std::clamp(static_cast<int>(wrap(coordinate) * static_cast<float>(kMaterialSize)), 0,
                      kMaterialSize - 1);
}

// The material of surfaces without one: Godot's default, white.
const VoxelGIMaterial& default_material() {
    static const VoxelGIMaterial material{
        .albedo = std::vector<std::array<float, 3>>(
            std::size_t{kVoxelGIMaterialSize} * kVoxelGIMaterialSize, {1.0F, 1.0F, 1.0F}),
        .emission = std::vector<std::array<float, 3>>(
            std::size_t{kVoxelGIMaterialSize} * kVoxelGIMaterialSize, {0.0F, 0.0F, 0.0F})};
    return material;
}

class Voxelizer {
public:
    // Voxelizer::begin_bake.
    Voxelizer(int subdiv, const Box& bounds) : subdiv_(subdiv), original_bounds_(bounds) {
        cells_.resize(1);
        po2_bounds_ = bounds;
        const int longest = po2_bounds_.longest_axis();
        axis_cell_size_.at(static_cast<std::size_t>(longest)) = 1 << subdiv;
        for (int i = 0; i < 3; ++i) {
            if (i == longest) {
                continue;
            }
            int cells = 1 << subdiv;
            float axis_size = component(po2_bounds_.size, longest);
            // Shrink until it fits; stopped at one cell where Godot would run on (a size of 0).
            while (cells > 1 && static_cast<double>(axis_size) / 2.0 >=
                                    static_cast<double>(component(po2_bounds_.size, i))) {
                axis_size /= 2.0F;
                cells >>= 1;
            }
            axis_cell_size_.at(static_cast<std::size_t>(i)) = cells;
            set_component(po2_bounds_.size, i, component(po2_bounds_.size, longest));
        }
    }

    [[nodiscard]] std::array<std::uint32_t, 3> octree_size() const {
        return {static_cast<std::uint32_t>(axis_cell_size_[0]),
                static_cast<std::uint32_t>(axis_cell_size_[1]),
                static_cast<std::uint32_t>(axis_cell_size_[2])};
    }

    // Voxelizer::plot_mesh.
    void plot_mesh(const VoxelGIMesh& mesh) {
        // The normals' transform: the basis' inverse, transposed.
        const world::Affine3D inv = mesh.transform.inverse();
        const auto normal_xform = [&inv](Vec3 n) {
            return Vec3{dot(inv.x, n), dot(inv.y, n), dot(inv.z, n)};
        };
        for (std::size_t s = 0; s < mesh.surfaces.size(); ++s) {
            const VoxelGIMesh::Surface& surface = mesh.surfaces[s];
            const VoxelGIMaterial& material =
                surface.material ? *surface.material : default_material();
            const std::size_t first = surface.first_index;
            const std::size_t end = s + 1 < mesh.surfaces.size() ? mesh.surfaces[s + 1].first_index
                                                                 : mesh.indices.size();
            for (std::size_t j = first; j + 3 <= std::min(end, mesh.indices.size()); j += 3) {
                std::array<Vec3, 3> vtxs;
                std::array<Vec2, 3> uvs;
                std::array<Vec3, 3> normals;
                bool valid = true;
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::uint32_t index = mesh.indices[j + k];
                    if (index >= mesh.vertices.size()) {
                        valid = false;
                        break;
                    }
                    const render::Vertex3D& v = mesh.vertices[index];
                    vtxs.at(k) = mesh.transform.apply(vec(v.position));
                    uvs.at(k) = {v.uv[0], v.uv[1]};
                    normals.at(k) = normalized(normal_xform(vec(v.normal)));
                }
                if (!valid || !triangle_box_overlap(original_bounds_.center(),
                                                    original_bounds_.size * 0.5F, vtxs)) {
                    continue;
                }
                plot_face(0, 0, 0, 0, 0, vtxs, normals, uvs, material, po2_bounds_);
            }
        }
    }

    // Voxelizer::end_bake, then the data Godot hands the rendering server.
    VoxelGIData finish(float exposure) {
        sort();
        fixup(0, 0);
        VoxelGIData out;
        const Vec3 position = original_bounds_.position;
        out.bounds_position = {position.x, position.y, position.z};
        out.bounds_size = {original_bounds_.size.x, original_bounds_.size.y,
                           original_bounds_.size.z};
        out.octree_size = octree_size();
        out.baked_exposure = exposure;
        out.level_counts.assign(static_cast<std::size_t>(subdiv_) + 1, 0);
        out.cells.reserve(cells_.size());
        for (const Cell& cell : cells_) {
            ++out.level_counts[cell.level];
            out.cells.push_back(data_cell(cell));
        }
        return out;
    }

private:
    // Voxelizer::_plot_face.
    void plot_face(std::uint32_t idx, int level, int x, int y, int z,
                   const std::array<Vec3, 3>& vtx, const std::array<Vec3, 3>& normals,
                   const std::array<Vec2, 3>& uvs, const VoxelGIMaterial& material,
                   const Box& aabb) {
        if (level == subdiv_) {
            plot_leaf(idx, vtx, normals, uvs, material, aabb);
            return;
        }
        const int half = (1 << subdiv_) >> (level + 1);
        for (int i = 0; i < 8; ++i) {
            Box child = aabb;
            child.size = child.size * 0.5F;
            int nx = x;
            int ny = y;
            int nz = z;
            if ((i & 1) != 0) {
                child.position.x += child.size.x;
                nx += half;
            }
            if ((i & 2) != 0) {
                child.position.y += child.size.y;
                ny += half;
            }
            if ((i & 4) != 0) {
                child.position.z += child.size.z;
                nz += half;
            }
            if (nx < 0 || nx >= axis_cell_size_[0] || ny < 0 || ny >= axis_cell_size_[1] ||
                nz < 0 || nz >= axis_cell_size_[2]) {
                continue;
            }
            const Vec3 qsize = child.size * 0.5F;
            if (!triangle_box_overlap(child.position + qsize, qsize, vtx)) {
                continue;
            }
            const auto slot = static_cast<std::size_t>(i);
            if (cells_[idx].children.at(slot) == kChildEmpty) {
                const auto child_idx = static_cast<std::uint32_t>(cells_.size());
                cells_[idx].children.at(slot) = child_idx;
                Cell made;
                made.level = static_cast<std::uint16_t>(level + 1);
                made.x = static_cast<std::uint16_t>(nx / half);
                made.y = static_cast<std::uint16_t>(ny / half);
                made.z = static_cast<std::uint16_t>(nz / half);
                cells_.push_back(made);
            }
            plot_face(cells_[idx].children.at(slot), level + 1, nx, ny, nz, vtx, normals, uvs,
                      material, child);
        }
    }

    // _plot_face at the deepest level: the triangle's albedo, emission and normal over a 4x4 scan
    // of the cell along the axis it faces most.
    void plot_leaf(std::uint32_t idx, const std::array<Vec3, 3>& vtx,
                   const std::array<Vec3, 3>& normals, const std::array<Vec2, 3>& uvs,
                   const VoxelGIMaterial& material, const Box& aabb) {
        const Plane plane = Plane::through(vtx[0], vtx[1], vtx[2]);
        Vec3 normal = plane.normal;
        int closest_axis = 0;
        float closest_dot = 0.0F;
        for (int i = 0; i < 3; ++i) {
            const float d = std::abs(component(normal, i));
            if (i == 0 || d > closest_dot) {
                closest_axis = i;
                closest_dot = d;
            }
        }
        Vec3 axis;
        set_component(axis, closest_axis, 1.0F);
        Vec3 t1;
        set_component(t1, (closest_axis + 1) % 3, 1.0F);
        Vec3 t2;
        set_component(t2, (closest_axis + 2) % 3, 1.0F);
        t1 = t1 * (component(aabb.size, (closest_axis + 1) % 3) / static_cast<float>(kScan));
        t2 = t2 * (component(aabb.size, (closest_axis + 2) % 3) / static_cast<float>(kScan));
        const float axis_size = component(aabb.size, closest_axis);

        std::array<float, 3> albedo_accum{};
        std::array<float, 3> emission_accum{};
        Vec3 normal_accum;
        float alpha = 0.0F;
        const std::array<Vec3, 3> face = vtx;
        for (int i = 0; i < kScan; ++i) {
            const Vec3 ofs_i = t1 * static_cast<float>(i);
            for (int j = 0; j < kScan; ++j) {
                const Vec3 ofs_j = t2 * static_cast<float>(j);
                const Vec3 from = aabb.position + ofs_i + ofs_j;
                const Vec3 to = from + t1 + t2 + (axis * axis_size);
                const Vec3 half = (to - from) * 0.5F;
                if (!triangle_box_overlap(from + half, half, vtx)) {
                    continue;
                }
                // From -size to +size * 2, to not skip collisions.
                Vec3 ray_from = from + ((t1 + t2) * 0.5F) - (axis * axis_size);
                Vec3 ray_to = ray_from + ((axis * axis_size) * 2.0F);
                if (dot(normal, ray_from - ray_to) < 0.0F) {
                    std::swap(ray_from, ray_to);
                }
                Vec3 intersection;
                if (!plane.intersects_segment(ray_from, ray_to, intersection)) {
                    if (std::abs(plane.distance_to(ray_from)) <
                        std::abs(plane.distance_to(ray_to))) {
                        intersection = plane.project(ray_from);
                    } else {
                        intersection = plane.project(ray_to);
                    }
                }
                intersection = closest_point_on_triangle(face, intersection);
                Vec2 uv;
                Vec3 lnormal;
                uv_and_normal(intersection, vtx, uvs, normals, uv, lnormal);
                if (lnormal == Vec3{}) {
                    lnormal = normal; // in case normals aren't given
                }
                const std::size_t ofs =
                    (static_cast<std::size_t>(texel_of(uv.y)) * kVoxelGIMaterialSize) +
                    static_cast<std::size_t>(texel_of(uv.x));
                for (std::size_t c = 0; c < 3; ++c) {
                    albedo_accum.at(c) += material.albedo[ofs].at(c);
                    emission_accum.at(c) += material.emission[ofs].at(c);
                }
                normal_accum = normal_accum + lnormal;
                alpha += 1.0F;
            }
        }
        if (alpha == 0.0F) {
            // No texture information at all: the point nearest the cell's middle. As Godot's, its
            // normal is the triangle's there (Godot's code writes it over the plane's normal and
            // then takes that).
            const Vec3 inters = closest_point_on_triangle(face, aabb.center());
            Vec2 uv;
            uv_and_normal(inters, vtx, uvs, normals, uv, normal);
            const Vec3 lnormal = normal;
            const std::size_t ofs =
                (static_cast<std::size_t>(texel_of(uv.y)) * kVoxelGIMaterialSize) +
                static_cast<std::size_t>(texel_of(uv.x));
            alpha = 1.0F / static_cast<float>(kScan * kScan);
            for (std::size_t c = 0; c < 3; ++c) {
                albedo_accum.at(c) = material.albedo[ofs].at(c) * alpha;
                emission_accum.at(c) = material.emission[ofs].at(c) * alpha;
            }
            normal_accum = lnormal * alpha;
        } else {
            const float accdiv = 1.0F / static_cast<float>(kScan * kScan);
            alpha *= accdiv;
            for (std::size_t c = 0; c < 3; ++c) {
                albedo_accum.at(c) *= accdiv;
                emission_accum.at(c) *= accdiv;
            }
            normal_accum = normal_accum * accdiv;
        }
        Cell& cell = cells_[idx];
        for (std::size_t c = 0; c < 3; ++c) {
            cell.albedo.at(c) += albedo_accum.at(c);
            cell.emission.at(c) += emission_accum.at(c);
        }
        cell.normal[0] += normal_accum.x;
        cell.normal[1] += normal_accum.y;
        cell.normal[2] += normal_accum.z;
        cell.alpha += alpha;
    }

    // Voxelizer::_sort: by level, then x, y and z, children renumbered.
    void sort() {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> order(cells_.size());
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            const Cell& c = cells_[i];
            const std::uint64_t key = (std::uint64_t{c.level} << 48U) |
                                      (std::uint64_t{c.x} << 32U) | (std::uint64_t{c.y} << 16U) |
                                      std::uint64_t{c.z};
            order[i] = {key, static_cast<std::uint32_t>(i)};
        }
        std::ranges::sort(order);
        std::vector<std::uint32_t> reverse(cells_.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            reverse[order[i].second] = static_cast<std::uint32_t>(i);
        }
        std::vector<Cell> sorted(cells_.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            sorted[i] = cells_[order[i].second];
            for (std::uint32_t& child : sorted[i].children) {
                if (child != kChildEmpty) {
                    child = reverse[child];
                }
            }
        }
        cells_ = std::move(sorted);
    }

    // Voxelizer::_fixup_plot.
    void fixup(std::uint32_t idx, int level) {
        if (level == subdiv_) {
            Cell& cell = cells_[idx];
            const float alpha = cell.alpha;
            for (std::size_t c = 0; c < 3; ++c) {
                cell.albedo.at(c) /= alpha;
                cell.emission.at(c) /= alpha;
                cell.normal.at(c) /= alpha;
            }
            const Vec3 n = vec(cell.normal);
            if (static_cast<double>(length(n)) < 0.01) {
                cell.normal = {0.0F, 0.0F, 0.0F}; // too much fight over the normal
            } else {
                const Vec3 unit = normalized(n);
                cell.normal = {unit.x, unit.y, unit.z};
            }
            cell.alpha = 1.0F;
            return;
        }
        cells_[idx].albedo = {0.0F, 0.0F, 0.0F};
        cells_[idx].emission = {0.0F, 0.0F, 0.0F};
        cells_[idx].normal = {0.0F, 0.0F, 0.0F};
        float alpha_average = 0.0F;
        for (std::size_t i = 0; i < 8; ++i) {
            const std::uint32_t child = cells_[idx].children.at(i);
            if (child == kChildEmpty) {
                continue;
            }
            fixup(child, level + 1);
            alpha_average += cells_[child].alpha;
        }
        cells_[idx].alpha = static_cast<float>(static_cast<double>(alpha_average) / 8.0);
    }

    // A cell as Voxelizer::get_voxel_gi_data_cells writes it.
    static VoxelGICell data_cell(const Cell& cell) {
        VoxelGICell out{};
        out[0] =
            std::uint32_t{cell.x} | (std::uint32_t{cell.y} << 11U) | (std::uint32_t{cell.z} << 21U);
        const auto byte = [](float v) {
            return static_cast<std::uint32_t>(
                std::clamp(static_cast<double>(v) * 255.0, 0.0, 255.0));
        };
        out[1] = (byte(cell.alpha) << 24U) | (byte(cell.albedo[2]) << 16U) |
                 (byte(cell.albedo[1]) << 8U) | byte(cell.albedo[0]);
        out[2] = rgbe9995(cell.emission);
        const Vec3 n = normalized(vec(cell.normal));
        const auto snorm = [](float v) {
            const double scaled = std::clamp(static_cast<double>(v) * 127.0, -128.0, 127.0);
            return std::uint32_t{static_cast<std::uint8_t>(static_cast<std::int8_t>(scaled))};
        };
        out[3] = snorm(n.x) | (snorm(n.y) << 8U) | (snorm(n.z) << 16U);
        return out;
    }

    // Color::to_rgbe9995, Microsoft's MiniEngine code.
    static std::uint32_t rgbe9995(const std::array<float, 3>& color) {
        const auto max_value = static_cast<float>(0x1FF << 7);
        const float min_value = 1.0F / static_cast<float>(1 << 16);
        const float r = std::clamp(color[0], 0.0F, max_value);
        const float g = std::clamp(color[1], 0.0F, max_value);
        const float b = std::clamp(color[2], 0.0F, max_value);
        const float max_channel = std::max(std::max(r, g), std::max(b, min_value));
        auto e = slash::bit_cast<std::uint32_t>(max_channel);
        e += 0x07804000U; // 15 more in the exponent, 0x4000 more in the mantissa
        e &= 0x7F800000U; // the mantissa zeroed
        const auto e_f = slash::bit_cast<float>(e);
        const auto ri = slash::bit_cast<std::uint32_t>(r + e_f);
        const auto gi = slash::bit_cast<std::uint32_t>(g + e_f);
        const auto bi = slash::bit_cast<std::uint32_t>(b + e_f);
        e <<= 4U;
        e += 0x10000000U;
        return e | (bi << 18U) | (gi << 9U) | (ri & 511U);
    }

    int subdiv_;
    Box original_bounds_;
    Box po2_bounds_;
    std::array<int, 3> axis_cell_size_{};
    std::vector<Cell> cells_;
};

// ---- The distance field

constexpr float kBig = 1e20F;

// edt: the distance transform of n squared distances along a row, Pedro Felzenszwalb and Daniel
// Huttenlocher's, in floats as Godot's.
void edt(float* f, std::size_t stride, int n, std::vector<float>& d, std::vector<int>& v,
         std::vector<float>& z) {
    d.resize(static_cast<std::size_t>(n));
    v.resize(static_cast<std::size_t>(n));
    z.resize(static_cast<std::size_t>(n) + 1);
    const auto at = [&](int q) -> float& { return f[static_cast<std::size_t>(q) * stride]; };
    const auto square = [](int s) { return static_cast<float>(s * s); };
    int k = 0;
    v[0] = 0;
    z[0] = -kBig;
    z[1] = kBig;
    for (int q = 1; q <= n - 1; ++q) {
        const auto vk = [&] { return v[static_cast<std::size_t>(k)]; };
        float s = ((at(q) + square(q)) - (at(vk()) + square(vk()))) /
                  static_cast<float>((2 * q) - (2 * vk()));
        while (s <= z[static_cast<std::size_t>(k)]) {
            --k;
            s = ((at(q) + square(q)) - (at(vk()) + square(vk()))) /
                static_cast<float>((2 * q) - (2 * vk()));
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k) + 1] = kBig;
    }
    k = 0;
    for (int q = 0; q <= n - 1; ++q) {
        while (z[static_cast<std::size_t>(k) + 1] < static_cast<float>(q)) {
            ++k;
        }
        const int vk = v[static_cast<std::size_t>(k)];
        d[static_cast<std::size_t>(q)] = square(q - vk) + at(vk);
    }
    for (int i = 0; i < n; ++i) {
        at(i) = d[static_cast<std::size_t>(i)];
    }
}

void run(jobs::Pool* pool, std::size_t count, const std::function<void(std::size_t)>& task) {
    if (pool != nullptr) {
        pool->parallel_for(count, task);
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
    }
}

// ---- Light injection: voxel_gi.glsl

// A cell as the shaders read it.
struct LitCell {
    Vec3 pos;    // its middle, in cells
    Vec3 albedo; // unpackUnorm4x8
    float solid = 0.0F;
    std::uint8_t solid_byte = 0;
    Vec3 emission; // RGBE9995 decoded
    Vec3 normal;   // unpackSnorm4x8
};

float snorm8(std::uint32_t bits) {
    const auto value = static_cast<std::int8_t>(static_cast<std::uint8_t>(bits & 0xFFU));
    return std::max(static_cast<float>(value) / 127.0F, -1.0F);
}

LitCell lit_cell(const VoxelGICell& cell) {
    LitCell out;
    const auto p = cell_position(cell);
    out.pos = {static_cast<float>(p[0]) + 0.5F, static_cast<float>(p[1]) + 0.5F,
               static_cast<float>(p[2]) + 0.5F};
    out.albedo = {static_cast<float>(cell[1] & 0xFFU) / 255.0F,
                  static_cast<float>((cell[1] >> 8U) & 0xFFU) / 255.0F,
                  static_cast<float>((cell[1] >> 16U) & 0xFFU) / 255.0F};
    out.solid_byte = static_cast<std::uint8_t>(cell[1] >> 24U);
    out.solid = static_cast<float>(out.solid_byte) / 255.0F;
    const int exponent = static_cast<int>(cell[2] >> 27U) - 15 - 9;
    out.emission =
        Vec3{static_cast<float>(cell[2] & 0x1FFU), static_cast<float>((cell[2] >> 9U) & 0x1FFU),
             static_cast<float>((cell[2] >> 18U) & 0x1FFU)} *
        std::ldexp(1.0F, exponent);
    out.normal = {snorm8(cell[3]), snorm8(cell[3] >> 8U), snorm8(cell[3] >> 16U)};
    return out;
}

// GLSL's pow for x > 0, the same on every platform.
float power(float x, float y) {
    return slash::exp(y * slash::ln(x));
}

// GLSL's mod.
float glsl_mod(float x, float y) {
    return x - (y * std::floor(x / y));
}

float sign_of(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

// Float to an 8-bit unorm, as a shader's image store rounds it.
std::uint8_t unorm8(float v) {
    if (!(v > 0.0F)) {
        return 0;
    }
    if (v >= 1.0F) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::lround(v * 255.0F));
}

// One level of a 3D texture of bytes, sampled with linear filtering and clamped edges.
struct Grid3D {
    std::array<std::uint32_t, 3> size{};
    const std::uint8_t* texels = nullptr;
    std::size_t components = 1;

    // The filtered value of a component at a texel coordinate (texel middles at .5), in bytes.
    template <std::size_t N>
    [[nodiscard]] std::array<float, N> sample(Vec3 at) const {
        std::array<int, 3> i0{};
        std::array<int, 3> i1{};
        std::array<float, 3> f{};
        const std::array<float, 3> coords{at.x - 0.5F, at.y - 0.5F, at.z - 0.5F};
        for (std::size_t a = 0; a < 3; ++a) {
            const float fl = std::floor(coords.at(a));
            const int last = static_cast<int>(size.at(a)) - 1;
            const int base = std::isfinite(fl) ? static_cast<int>(std::clamp(fl, -2.0F, 1e6F)) : 0;
            f.at(a) = coords.at(a) - fl;
            if (!std::isfinite(f.at(a))) {
                f.at(a) = 0.0F;
            }
            i0.at(a) = std::clamp(base, 0, last);
            i1.at(a) = std::clamp(base + 1, 0, last);
        }
        std::array<float, N> out{};
        for (int corner = 0; corner < 8; ++corner) {
            const int x = (corner & 1) != 0 ? i1[0] : i0[0];
            const int y = (corner & 2) != 0 ? i1[1] : i0[1];
            const int z = (corner & 4) != 0 ? i1[2] : i0[2];
            const float w = ((corner & 1) != 0 ? f[0] : 1.0F - f[0]) *
                            ((corner & 2) != 0 ? f[1] : 1.0F - f[1]) *
                            ((corner & 4) != 0 ? f[2] : 1.0F - f[2]);
            const std::size_t index =
                ((((static_cast<std::size_t>(z) * size[1]) + static_cast<std::size_t>(y)) *
                  size[0]) +
                 static_cast<std::size_t>(x)) *
                components;
            for (std::size_t c = 0; c < N; ++c) {
                out.at(c) += static_cast<float>(texels[index + c]) * w;
            }
        }
        return out;
    }
};

// The light lights bring places in a VoxelGI's cells, as voxel_gi.glsl's compute_light_at_pos:
// shadows are marched through the cells' distance field.
class LightField {
public:
    LightField(const VoxelGIData& data, const VoxelGIOctree& octree,
               std::span<const VoxelGILight> lights)
        : lights_(lights) {
        limits_ = {static_cast<float>(data.octree_size[0]), static_cast<float>(data.octree_size[1]),
                   static_cast<float>(data.octree_size[2])};
        const Vec3 ps = divide(limits_, vec(data.bounds_size));
        cell_size_ =
            static_cast<float>(1.0 / static_cast<double>(std::max(std::max(ps.x, ps.y), ps.z)));
        sdf_.size = data.octree_size;
        sdf_.texels = octree.distance_field.data();
        sdf_.components = 1;
    }

    // The cells along each axis.
    [[nodiscard]] Vec3 limits() const { return limits_; }
    [[nodiscard]] std::span<const VoxelGILight> lights() const { return lights_; }

private:
    // compute_light_vector.
    bool light_vector(const VoxelGILight& light, Vec3 pos, float& attenuation,
                      Vec3& light_pos) const {
        if (light.type == render::LightType::Directional) {
            light_pos = pos - (vec(light.direction) * length(limits_));
            attenuation = 1.0F;
            return true;
        }
        light_pos = vec(light.position);
        const float distance = length(pos - light_pos);
        if (distance >= light.radius) {
            return false;
        }
        // get_omni_attenuation.
        const float d = distance * cell_size_;
        const float inv_range = 1.0F / (light.radius * cell_size_);
        float nd = d * inv_range;
        nd *= nd;
        nd *= nd;
        nd = std::max(1.0F - nd, 0.0F);
        nd *= nd;
        attenuation = nd * power(std::max(d, 0.0001F), -light.attenuation);
        if (light.type == render::LightType::Spot) {
            const Vec3 rel = normalized(pos - light_pos);
            const float cos_angle = dot(rel, vec(light.direction));
            if (cos_angle < light.cos_spot_angle) {
                return false;
            }
            const float scos = std::max(cos_angle, light.cos_spot_angle);
            const float spot_rim = std::max(0.0001F, (1.0F - scos) / (1.0F - light.cos_spot_angle));
            attenuation *= 1.0F - power(spot_rim, light.inv_spot_attenuation);
        }
        return true;
    }

    // get_normal_advance.
    static float normal_advance(Vec3 normal) {
        const Vec3 unorm{std::abs(normal.x), std::abs(normal.y), std::abs(normal.z)};
        Vec3 axis{1.0F, 0.0F, 0.0F};
        if (unorm.x >= unorm.y && unorm.x >= unorm.z) {
            axis = {normal.x > 0.0F ? 1.0F : -1.0F, 0.0F, 0.0F};
        } else if (unorm.y > unorm.x && unorm.y >= unorm.z) {
            axis = {0.0F, normal.y > 0.0F ? 1.0F : -1.0F, 0.0F};
        } else if (unorm.z > unorm.x && unorm.z > unorm.y) {
            axis = {0.0F, 0.0F, normal.z > 0.0F ? 1.0F : -1.0F};
        }
        return 1.0F / dot(normal, axis);
    }

    // clip_segment. As Godot's, it never clips: the segment always runs away from the face's
    // plane, so its den is never positive.
    static void clip_segment(Vec3 plane_normal, float plane_d, Vec3 begin, Vec3& end) {
        const Vec3 segment = begin - end;
        const float den = dot(plane_normal, segment);
        if (den < 0.0001F) {
            return;
        }
        const float dist = (dot(plane_normal, begin) - plane_d) / den;
        if (dist < 0.0001F || dist > 1.0001F) {
            return;
        }
        end = begin + (segment * -dist);
    }

    // raymarch: how much of the light gets through, marched through the distance field.
    [[nodiscard]] float raymarch(float distance, float distance_adv, Vec3 from,
                                 Vec3 direction) const {
        float occlusion = 1.0F;
        while (distance > 0.5F) {
            float advance = sdf_.sample<1>(from)[0] - 1.0F;
            if (advance < 0.0F) {
                occlusion = 0.0F;
                break;
            }
            occlusion = std::min(advance, occlusion);
            advance = std::max(distance_adv, advance - glsl_mod(advance, distance_adv));
            from = from + (direction * advance);
            distance -= advance;
        }
        return occlusion;
    }

public:
    // compute_light_at_pos: the light from light at pos (in cells) on a surface facing normal, and
    // the way it shines; false where it brings none.
    bool light_at(const VoxelGILight& light, Vec3 pos, Vec3 normal, Vec3& out,
                  Vec3& light_dir) const {
        float attenuation = 0.0F;
        Vec3 light_pos;
        if (!light_vector(light, pos, attenuation, light_pos)) {
            return false;
        }
        light_dir = normalized(pos - light_pos);
        const bool has_normal = length(normal) > 0.2F;
        if (attenuation < 0.01F || (has_normal && dot(normal, light_dir) >= 0.0F)) {
            return false; // facing away, or too faint
        }
        if (light.has_shadow) {
            const float distance_adv = normal_advance(light_dir);
            Vec3 to = pos;
            if (has_normal) {
                to = to + (normal * distance_adv * 0.51F);
            } else {
                // Near the edge towards the light, against shadowing itself.
                to = to - (Vec3{sign_of(light_dir.x), sign_of(light_dir.y), sign_of(light_dir.z)} *
                           0.45F);
            }
            for (int a = 0; a < 3; ++a) {
                Vec3 plane_normal;
                float plane_d = 0.0F;
                if (component(light_dir, a) < 0.0F) {
                    set_component(plane_normal, a, 1.0F);
                    plane_d = component(limits_, a) - 1.0F;
                } else {
                    set_component(plane_normal, a, -1.0F);
                }
                clip_segment(plane_normal, plane_d, to, light_pos);
            }
            float distance = length(to - light_pos);
            if (distance < 0.1F) {
                return false;
            }
            // Always to the middle of the box.
            distance += distance_adv - glsl_mod(distance, distance_adv);
            light_pos = to - (light_dir * distance);
            const float occlusion = raymarch(distance, distance_adv, light_pos, light_dir);
            if (occlusion == 0.0F) {
                return false;
            }
            attenuation *= occlusion;
        }
        out = vec(light.color) * attenuation * light.energy;
        return true;
    }

private:
    std::span<const VoxelGILight> lights_;
    Vec3 limits_;
    float cell_size_ = 1.0F;
    Grid3D sdf_;
};

class Injector {
public:
    Injector(const VoxelGIData& data, const VoxelGIOctree& octree,
             std::span<const VoxelGILight> lights, const VoxelGISettings& settings,
             jobs::Pool* pool)
        : data_(data), octree_(octree), field_(data, octree, lights), settings_(settings),
          pool_(pool) {
        cells_.reserve(data.cells.size());
        for (const VoxelGICell& cell : data.cells) {
            cells_.push_back(lit_cell(cell));
        }
        outputs_.assign(data.cells.size(), Vec3{});
    }

    VoxelGITexture run_passes() {
        const std::size_t levels = data_.level_counts.size();
        VoxelGITexture texture;
        for (std::size_t m = 0; m < levels; ++m) {
            std::array<std::uint32_t, 3> size{};
            for (std::size_t a = 0; a < 3; ++a) {
                size.at(a) = std::max(data_.octree_size.at(a) >> m, 1U);
            }
            texture.sizes.push_back(size);
            texture.levels.emplace_back(std::size_t{size[0]} * size[1] * size[2] * 4, 0);
        }
        const std::size_t leaves = octree_.level_offsets[levels - 1];
        const std::size_t leaf_count = octree_.level_offsets[levels] - leaves;
        run(pool_, leaf_count, [&](std::size_t i) { compute_light(leaves + i); });
        mipmaps();
        write(texture);
        if (settings_.use_two_bounces) {
            run(pool_, leaf_count, [&](std::size_t i) { second_bounce(leaves + i, texture); });
            mipmaps();
            write(texture);
        }
        return texture;
    }

private:
    // MODE_COMPUTE_LIGHT.
    void compute_light(std::size_t index) {
        const LitCell& cell = cells_[index];
        Vec3 accum;
        for (const VoxelGILight& light : field_.lights()) {
            Vec3 light_color;
            Vec3 light_dir;
            if (!field_.light_at(light, cell.pos, cell.normal, light_color, light_dir)) {
                continue;
            }
            light_color = multiply(light_color, cell.albedo);
            if (length(cell.normal) > 0.2F) {
                accum = accum + (light_color * std::max(0.0F, dot(cell.normal, light_dir * -1.0F)));
            } else {
                accum = accum + light_color; // every way
            }
        }
        outputs_[index] = accum + cell.emission;
    }

    // textureLod on the light texture with trilinear filtering, clamped, as RGBA.
    [[nodiscard]] std::array<float, 4> sample_lod(const VoxelGITexture& texture, Vec3 uvw,
                                                  float lod) const {
        const auto last = static_cast<float>(texture.levels.size() - 1);
        lod = std::clamp(lod, 0.0F, last);
        const float floor_lod = std::floor(lod);
        const auto m0 = static_cast<std::size_t>(floor_lod);
        const std::size_t m1 = std::min(m0 + 1, texture.levels.size() - 1);
        const float blend = lod - floor_lod;
        const auto level = [&](std::size_t m) {
            const Grid3D grid{texture.sizes[m], texture.levels[m].data(), 4};
            const auto& s = texture.sizes[m];
            return grid.sample<4>(
                multiply(uvw, Vec3{static_cast<float>(s[0]), static_cast<float>(s[1]),
                                   static_cast<float>(s[2])}));
        };
        std::array<float, 4> out = level(m0);
        if (blend > 0.0F && m1 != m0) {
            const std::array<float, 4> next = level(m1);
            for (std::size_t c = 0; c < 4; ++c) {
                out.at(c) += (next.at(c) - out.at(c)) * blend;
            }
        }
        for (float& c : out) {
            c /= 255.0F;
        }
        return out;
    }

    // MODE_SECOND_BOUNCE: six cones traced through the first bounce's texture.
    void second_bounce(std::size_t index, const VoxelGITexture& texture) {
        const LitCell& cell = cells_[index];
        Vec3 accum = outputs_[index];
        const Vec3 normal = cell.normal;
        if (length(normal) > 0.2F) {
            const Vec3 v0 =
                std::abs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 tangent = normalized(cross(v0, normal));
            const Vec3 bitangent = normalized(cross(tangent, normal));
            static constexpr std::array<Vec3, 6> kConeDirs{Vec3{0.0F, 0.0F, 1.0F},
                                                           Vec3{0.866025F, 0.0F, 0.5F},
                                                           Vec3{0.267617F, 0.823639F, 0.5F},
                                                           Vec3{-0.700629F, 0.509037F, 0.5F},
                                                           Vec3{-0.700629F, -0.509037F, 0.5F},
                                                           Vec3{0.267617F, -0.823639F, 0.5F}};
            static constexpr std::array<float, 6> kConeWeights{0.25F, 0.15F, 0.15F,
                                                               0.15F, 0.15F, 0.15F};
            // NOLINTNEXTLINE(modernize-use-std-numbers): Godot's tan(30 degrees).
            constexpr float kTanHalfAngle = 0.577F;
            const float max_distance = length(field_.limits());
            const Vec3 cell_size = divide(Vec3{1.0F, 1.0F, 1.0F}, field_.limits());
            for (std::size_t i = 0; i < kConeDirs.size(); ++i) {
                const Vec3 d = kConeDirs.at(i);
                const Vec3 direction = (tangent * d.x) + (bitangent * d.y) + (normal * d.z);
                std::array<float, 4> color{};
                float dist = 1.5F;
                while (dist < max_distance && color[3] < 0.95F) {
                    const float diameter = std::max(1.0F, 2.0F * kTanHalfAngle * dist);
                    const Vec3 uvw = multiply(cell.pos + (direction * dist), cell_size);
                    const float half_diameter = diameter * 0.5F;
                    const float lod = slash::ln(diameter) / std::numbers::ln2_v<float>;
                    const std::array<float, 4> scolor = sample_lod(texture, uvw, lod);
                    const float a = 1.0F - color[3];
                    for (std::size_t c = 0; c < 4; ++c) {
                        color.at(c) += a * scolor.at(c);
                    }
                    dist += half_diameter;
                }
                // color *= weight * vec4(albedo, 1) * dynamic_range, restoring the range.
                const Vec3 factor = (cell.albedo * kConeWeights.at(i)) * settings_.dynamic_range;
                accum = accum + multiply(Vec3{color[0], color[1], color[2]}, factor);
            }
        }
        outputs_[index] = accum;
    }

    // MODE_UPDATE_MIPMAPS, from the level above the deepest up to the root.
    void mipmaps() {
        const std::size_t levels = data_.level_counts.size();
        for (std::size_t level = levels - 1; level-- > 0;) {
            const std::size_t first = octree_.level_offsets[level];
            const std::size_t count = octree_.level_offsets[level + 1] - first;
            run(pool_, count, [&](std::size_t i) {
                const std::size_t index = first + i;
                Vec3 light_accum;
                float children = 0.0F;
                for (const std::uint32_t child : octree_.children[index]) {
                    if (child == kChildEmpty) {
                        continue;
                    }
                    light_accum = light_accum + outputs_[child];
                    children += 1.0F;
                }
                const float divisor =
                    (8.0F * (1.0F - settings_.propagation)) + (children * settings_.propagation);
                outputs_[index] = {light_accum.x / divisor, light_accum.y / divisor,
                                   light_accum.z / divisor};
            });
        }
    }

    // MODE_WRITE_TEXTURE.
    void write(VoxelGITexture& texture) const {
        const std::size_t levels = data_.level_counts.size();
        for (std::size_t level = 0; level < levels; ++level) {
            const std::size_t m = levels - 1 - level;
            const auto& size = texture.sizes[m];
            std::vector<std::uint8_t>& texels = texture.levels[m];
            for (std::size_t index = octree_.level_offsets[level];
                 index < octree_.level_offsets[level + 1]; ++index) {
                const auto p = cell_position(data_.cells[index]);
                const std::size_t at =
                    ((((std::size_t{p[2]} * size[1]) + p[1]) * size[0]) + p[0]) * 4;
                const Vec3& out = outputs_[index];
                const float range = settings_.dynamic_range;
                const Vec3 light{out.x / range, out.y / range, out.z / range};
                texels[at] = unorm8(light.x);
                texels[at + 1] = unorm8(light.y);
                texels[at + 2] = unorm8(light.z);
                texels[at + 3] = cells_[index].solid_byte;
            }
        }
    }

    const VoxelGIData& data_;
    const VoxelGIOctree& octree_;
    LightField field_;
    const VoxelGISettings& settings_;
    jobs::Pool* pool_;
    std::vector<LitCell> cells_;
    std::vector<Vec3> outputs_;
};

// ---- Baking a world

// Whether entity is in the subtree under scope, or is scope (every entity for a null scope), as
// VoxelGI::_find_meshes walks from its parent.
bool at_or_under(const world::Tree3D& tree, entt::entity entity, entt::entity scope) {
    if (scope == entt::null) {
        return true;
    }
    for (entt::entity at = entity; at != entt::null; at = tree.parent(at)) {
        if (at == scope) {
            return true;
        }
    }
    return false;
}

template <typename T>
std::vector<entt::entity> made(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const T>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

// The box around a mesh's vertices placed by transform, as Transform3D::xform(AABB).
Box placed_box(std::span<const render::Vertex3D> vertices, const world::Affine3D& transform) {
    Box local;
    bool first = true;
    for (const render::Vertex3D& v : vertices) {
        if (first) {
            local.position = vec(v.position);
            first = false;
        } else {
            local.expand_to(vec(v.position));
        }
    }
    Box out;
    first = true;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner = transform.apply(local.endpoint(i));
        if (first) {
            out.position = corner;
            first = false;
        } else {
            out.expand_to(corner);
        }
    }
    return out;
}

// Reads entities' meshes as a VoxelGI takes them, their materials' images through sources and
// their emission at exposure; what can't be read is a problem.
class MeshReader {
public:
    MeshReader(const world::World& world, const LightmapSources& sources, float exposure,
               std::vector<Problem>& problems)
        : world_(world), sources_(sources), exposure_(exposure), problems_(problems) {}

    // The meshes of the entity's MeshInstance3D in its own space (voxel_gi_meshes).
    void read(entt::entity entity, std::vector<VoxelGIMesh>& out) {
        const auto& ecs = world_.ecs();
        const auto& instance = ecs.get<const world::MeshInstance3D>(entity);
        const auto* standard = ecs.try_get<const world::StandardMaterial3D>(entity);
        std::shared_ptr<const VoxelGIMaterial> override_material;
        if (standard != nullptr) {
            override_material = material(*standard, entity, nullptr);
        }
        if (!instance.mesh.empty()) {
            add_model(entity, instance, override_material, out);
            return;
        }
        auto primitive = primitive_of(world_, entity);
        if (!primitive) {
            return; // nothing drawn, which the renderer reports
        }
        const auto& shape = primitive.value();
        if (!shape) {
            return;
        }
        auto data = make_mesh(*shape);
        if (!data) {
            return;
        }
        VoxelGIMesh mesh;
        mesh.vertices = std::move(data.value().vertices);
        mesh.indices = std::move(data.value().indices);
        mesh.surfaces.push_back({.first_index = 0, .material = override_material});
        out.push_back(std::move(mesh));
    }

private:
    std::shared_ptr<const render::Image> image(const std::string& path, entt::entity entity,
                                               const std::string& field) {
        if (path.empty()) {
            return nullptr;
        }
        if (!sources_.images) {
            problems_.push_back({entity, field, "images can't load here"});
            return nullptr;
        }
        auto loaded = sources_.images(path);
        if (!loaded) {
            problems_.push_back({entity, field, loaded.error().message});
            return nullptr;
        }
        return loaded.value();
    }

    // A StandardMaterial3D as baked; its textures are those of a model where given, else its
    // own project paths.
    std::shared_ptr<const VoxelGIMaterial>
    material(const world::StandardMaterial3D& standard, entt::entity entity,
             const std::function<std::shared_ptr<const render::Image>(bool emission)>& model) {
        const std::string field = "/components/StandardMaterial3D/";
        std::shared_ptr<const render::Image> albedo;
        std::shared_ptr<const render::Image> emission;
        if (model) {
            albedo = model(false);
            emission = standard.emission_enabled ? model(true) : nullptr;
        } else {
            albedo = image(standard.albedo_texture, entity, field + "albedo_texture");
            emission = standard.emission_enabled
                           ? image(standard.emission_texture, entity, field + "emission_texture")
                           : nullptr;
        }
        return std::make_shared<const VoxelGIMaterial>(
            voxel_gi_material(standard, albedo.get(), emission.get(), exposure_));
    }

    void add_model(entt::entity entity, const world::MeshInstance3D& instance,
                   const std::shared_ptr<const VoxelGIMaterial>& override_material,
                   std::vector<VoxelGIMesh>& out) {
        const std::string field = "/components/MeshInstance3D/mesh";
        if (!sources_.models) {
            problems_.push_back({entity, field, "models can't load here"});
            return;
        }
        auto loaded = sources_.models(instance.mesh);
        if (!loaded) {
            problems_.push_back({entity, field, loaded.error().message});
            return;
        }
        const std::shared_ptr<const Model> model = loaded.value();
        std::vector<std::shared_ptr<const VoxelGIMaterial>> materials(model->materials.size());
        const auto model_image = [&](int index) -> std::shared_ptr<const render::Image> {
            if (index < 0 || static_cast<std::size_t>(index) >= model->images.size()) {
                return nullptr;
            }
            const ModelImage& found = model->images[static_cast<std::size_t>(index)];
            if (!found.path.empty()) {
                return image(found.path, entity, field);
            }
            return {model, &found.image}; // kept alive with the model
        };
        for (const ModelPart& part : model->parts) {
            // A skinned part is taken at rest, as Godot plots a mesh's own vertices.
            const ModelMesh& from = model->meshes[part.mesh];
            VoxelGIMesh mesh;
            mesh.transform = part.transform;
            for (const ModelSurface& surface : from.surfaces) {
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                VoxelGIMesh::Surface entry{.first_index =
                                               static_cast<std::uint32_t>(mesh.indices.size()),
                                           .material = override_material};
                if (!override_material && surface.material >= 0 &&
                    static_cast<std::size_t>(surface.material) < materials.size()) {
                    auto& kept = materials[static_cast<std::size_t>(surface.material)];
                    if (!kept) {
                        const ModelMaterial& m =
                            model->materials[static_cast<std::size_t>(surface.material)];
                        kept = material(m.standard, entity, [&](bool emission) {
                            return model_image(emission ? m.emission_image : m.albedo_image);
                        });
                    }
                    entry.material = kept;
                }
                mesh.surfaces.push_back(std::move(entry));
                mesh.vertices.insert(mesh.vertices.end(), surface.vertices.begin(),
                                     surface.vertices.end());
                for (const std::uint32_t index : surface.indices) {
                    mesh.indices.push_back(base + index);
                }
            }
            if (!mesh.indices.empty()) {
                out.push_back(std::move(mesh));
            }
        }
    }

    const world::World& world_;
    const LightmapSources& sources_;
    float exposure_;
    std::vector<Problem>& problems_;
};

// ---- Moving meshes: VoxelGIInstance::update's dynamic objects and voxel_gi.glsl's MODE_DYNAMIC

// The finest dynamic map is 2^4 times finer than the cells, as Godot's oversample.
constexpr int kOversample = 4;
constexpr int kMultiplier = 1 << kOversample;

// A side a mesh is drawn from, as set_look_at from a face of its box toward its middle with
// Godot's render_z and render_up: the cells' axes across and up the map, the axis it looks along,
// whether it looks down that axis from the far face, and the way it looks.
struct Side {
    int x_axis = 0;
    int y_axis = 0;
    int z_axis = 0;
    bool z_flip = false;
    Vec3 dir;
};

int axis_of(Vec3 v) {
    if (std::abs(v.x) > 0.5F) {
        return 0;
    }
    return std::abs(v.y) > 0.5F ? 1 : 2;
}

const std::array<Side, 6>& sides() {
    static const std::array<Side, 6> made = [] {
        static constexpr std::array<Vec3, 6> kRenderZ{
            Vec3{1.0F, 0.0F, 0.0F},  Vec3{0.0F, 1.0F, 0.0F},  Vec3{0.0F, 0.0F, 1.0F},
            Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}};
        static constexpr std::array<Vec3, 6> kRenderUp{
            Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 1.0F, 0.0F},
            Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 1.0F, 0.0F}};
        std::array<Side, 6> out{};
        for (std::size_t j = 0; j < out.size(); ++j) {
            // Basis::looking_at: z back toward the eye, x the up cross z, y z cross x.
            const Vec3 z = kRenderZ.at(j) * -1.0F;
            const Vec3 x = cross(kRenderUp.at(j), z);
            const Vec3 y = cross(z, x);
            out.at(j) = {.x_axis = axis_of(x),
                         .y_axis = axis_of(y),
                         .z_axis = axis_of(z),
                         .z_flip = z.x + z.y + z.z > 0.0F,
                         .dir = kRenderZ.at(j)};
        }
        return out;
    }();
    return made;
}

// A pixel of the material pass (MODE_RENDER_MATERIAL): the nearest surface's albedo, how much of
// it is there (0 where none), its normal in the cells' space, emission and distance from the side.
struct MaterialTexel {
    Vec3 albedo;
    float alpha = 0.0F;
    Vec3 normal;
    Vec3 emission;
    float depth = 0.0F;
    float nearest = std::numeric_limits<float>::infinity();
};

// A texel of a dynamic map: light and how much of it is covered, and its place along the side's
// axis in the map's units.
struct DynamicTexel {
    std::array<float, 4> light{};
    float z = 0.0F;
};

class ObjectPlotter {
public:
    ObjectPlotter(VoxelGITexture& texture, const VoxelGIData& data, const VoxelGIOctree& octree,
                  std::span<const VoxelGILight> lights, const VoxelGISettings& settings,
                  jobs::Pool* pool)
        : texture_(texture), field_(data, octree, lights), settings_(settings), pool_(pool),
          octree_size_(data.octree_size) {
        world::Affine3D oversample;
        oversample.x = {static_cast<float>(kMultiplier), 0.0F, 0.0F};
        oversample.y = {0.0F, static_cast<float>(kMultiplier), 0.0F};
        oversample.z = {0.0F, 0.0F, static_cast<float>(kMultiplier)};
        to_map_ = oversample * voxel_gi_to_cell(data);
    }

    void plot(const VoxelGIMesh& mesh) {
        const world::Affine3D place = to_map_ * mesh.transform;
        const Box box = placed_box(mesh.vertices, place);
        // Wrapped to the map's texels against jitter, a texel more each way, and even across for
        // halving.
        std::array<int, 3> begin{};
        std::array<int, 3> end{};
        for (int a = 0; a < 3; ++a) {
            const auto i = static_cast<std::size_t>(a);
            const float low = component(box.position, a);
            begin.at(i) = static_cast<int>(low - 1.0F);
            end.at(i) = static_cast<int>(low + component(box.size, a) + 1.0F);
            if (((end.at(i) - begin.at(i)) & 1) != 0) {
                ++end.at(i);
            }
            begin.at(i) = std::max(begin.at(i), 0);
            end.at(i) = std::min(end.at(i), static_cast<int>(octree_size_.at(i)) * kMultiplier);
            if (end.at(i) <= begin.at(i)) {
                return; // outside the cells
            }
        }
        for (const Side& side : sides()) {
            plot_side(mesh, place, begin, end, side);
        }
    }

private:
    void plot_side(const VoxelGIMesh& mesh, const world::Affine3D& place,
                   const std::array<int, 3>& begin, const std::array<int, 3>& end,
                   const Side& side) {
        const auto xa = static_cast<std::size_t>(side.x_axis);
        const auto ya = static_cast<std::size_t>(side.y_axis);
        const auto za = static_cast<std::size_t>(side.z_axis);
        std::array<int, 2> rect_pos{begin.at(xa), begin.at(ya)};
        std::array<int, 2> rect_size{end.at(xa) - begin.at(xa), end.at(ya) - begin.at(ya)};
        const auto z_base = static_cast<float>(side.z_flip ? end.at(za) : begin.at(za));
        const float z_sign = side.z_flip ? -1.0F : 1.0F;
        const auto far = static_cast<float>(end.at(za) - begin.at(za));

        std::vector<MaterialTexel> material(static_cast<std::size_t>(rect_size[0]) *
                                            static_cast<std::size_t>(rect_size[1]));
        draw_material(mesh, place, side, rect_pos, rect_size, z_base, z_sign, far, material);

        // MODE_DYNAMIC_LIGHTING: each covered pixel lit at its place in the cells.
        std::vector<DynamicTexel> map(material.size());
        run(pool_, static_cast<std::size_t>(rect_size[1]), [&](std::size_t row) {
            const auto py = static_cast<int>(row);
            for (int px = 0; px < rect_size[0]; ++px) {
                const std::size_t at =
                    (row * static_cast<std::size_t>(rect_size[0])) + static_cast<std::size_t>(px);
                const MaterialTexel& m = material[at];
                DynamicTexel& out = map[at];
                const float z = z_base + (m.depth * z_sign);
                out.z = z;
                if (m.alpha == 0.0F) {
                    continue; // nothing lit, as Godot's cleared albedo and emission
                }
                Vec3 pos;
                set_component(pos, side.x_axis, static_cast<float>(rect_pos[0] + px));
                set_component(pos, side.y_axis, static_cast<float>(rect_pos[1] + py));
                set_component(pos, side.z_axis, static_cast<float>(static_cast<int>(z)));
                pos = pos * (1.0F / static_cast<float>(kMultiplier));
                Vec3 accum;
                for (const VoxelGILight& light : field_.lights()) {
                    Vec3 color;
                    Vec3 light_dir;
                    if (!field_.light_at(light, pos, m.normal, color, light_dir)) {
                        continue;
                    }
                    color = multiply(color, m.albedo);
                    accum = accum + (color * std::max(0.0F, dot(m.normal, light_dir * -1.0F)));
                }
                accum = accum + m.emission;
                out.light = {accum.x, accum.y, accum.z, m.alpha};
            }
        });

        // MODE_DYNAMIC_SHRINK: halved down to the cells, then down the texture's levels, plotted
        // into each level from level 0.
        const std::size_t levels = texture_.levels.size();
        const std::size_t maps = kOversample + levels;
        for (std::size_t k = 1; k < maps; ++k) {
            const int mipmap = k < kOversample ? -1 : static_cast<int>(k - kOversample);
            std::array<int, 2> prev_ofs{0, 0};
            const std::array<int, 2> prev_size = rect_size;
            for (std::size_t a = 0; a < 2; ++a) {
                if ((rect_pos.at(a) & 1) != 0) {
                    ++rect_size.at(a);
                    prev_ofs.at(a) = 1;
                }
                if ((rect_size.at(a) & 1) != 0) {
                    ++rect_size.at(a);
                }
                rect_pos.at(a) >>= 1;
                rect_size.at(a) = std::max(1, rect_size.at(a) >> 1);
            }
            const bool on_mipmap = mipmap > 0;
            const bool write = mipmap < 0 || k + 1 < maps;
            std::vector<DynamicTexel> next(write ? static_cast<std::size_t>(rect_size[0]) *
                                                       static_cast<std::size_t>(rect_size[1])
                                                 : 0);
            for (int py = 0; py < rect_size[1]; ++py) {
                for (int px = 0; px < rect_size[0]; ++px) {
                    std::array<float, 4> accum{};
                    float accum_z = 0.0F;
                    float count = 0.0F;
                    for (int i = 0; i < 4; ++i) {
                        const int ox = (px * 2) + (i & 1) - prev_ofs[0];
                        const int oy = (py * 2) + (i >> 1) - prev_ofs[1];
                        if (ox < 0 || oy < 0 || ox >= prev_size[0] || oy >= prev_size[1]) {
                            continue;
                        }
                        const DynamicTexel& from = map[(static_cast<std::size_t>(oy) *
                                                        static_cast<std::size_t>(prev_size[0])) +
                                                       static_cast<std::size_t>(ox)];
                        if (from.light[3] == 0.0F) {
                            continue; // empty
                        }
                        for (std::size_t c = 0; c < 4; ++c) {
                            accum.at(c) += from.light.at(c);
                        }
                        accum_z += from.z * 0.5F; // halved too
                        count += 1.0F;
                    }
                    if (on_mipmap) {
                        // mix(8, count, propagation).
                        const float divisor = (8.0F * (1.0F - settings_.propagation)) +
                                              (count * settings_.propagation);
                        for (std::size_t c = 0; c < 3; ++c) {
                            accum.at(c) /= divisor;
                        }
                        accum[3] /= 8.0F;
                    } else {
                        for (float& c : accum) {
                            c /= 4.0F;
                        }
                    }
                    accum_z = count == 0.0F ? 0.0F : accum_z / count;
                    if (write) {
                        next[(static_cast<std::size_t>(py) *
                              static_cast<std::size_t>(rect_size[0])) +
                             static_cast<std::size_t>(px)] = {.light = accum, .z = accum_z};
                    }
                    if (mipmap >= 0) {
                        blit(static_cast<std::size_t>(mipmap), side,
                             {rect_pos[0] + px, rect_pos[1] + py}, accum, accum_z);
                    }
                }
            }
            map = std::move(next);
        }
    }

    // The mesh drawn from the side into the map, nearest surface first, as Godot's material pass
    // draws it orthographically from the box's face: faces its material culls are left out, and
    // where it culls none, a face seen from behind has its normal turned round.
    void draw_material(const VoxelGIMesh& mesh, const world::Affine3D& place, const Side& side,
                       const std::array<int, 2>& rect_pos, const std::array<int, 2>& rect_size,
                       float z_base, float z_sign, float far,
                       std::vector<MaterialTexel>& material) const {
        const world::Affine3D inv = place.inverse();
        const auto normal_xform = [&inv](Vec3 n) {
            return Vec3{dot(inv.x, n), dot(inv.y, n), dot(inv.z, n)};
        };
        // A mirrored mesh's faces turn the other way, as Godot's mirrored instances.
        const bool mirrored = dot(place.x, cross(place.y, place.z)) < 0.0F;
        for (std::size_t s = 0; s < mesh.surfaces.size(); ++s) {
            const VoxelGIMesh::Surface& surface = mesh.surfaces[s];
            const VoxelGIMaterial& from = surface.material ? *surface.material : default_material();
            const std::size_t first = surface.first_index;
            const std::size_t last = s + 1 < mesh.surfaces.size() ? mesh.surfaces[s + 1].first_index
                                                                  : mesh.indices.size();
            for (std::size_t j = first; j + 3 <= std::min(last, mesh.indices.size()); j += 3) {
                std::array<Vec3, 3> p;
                std::array<Vec2, 3> uv;
                std::array<Vec3, 3> n;
                bool valid = true;
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::uint32_t index = mesh.indices[j + k];
                    if (index >= mesh.vertices.size()) {
                        valid = false;
                        break;
                    }
                    const render::Vertex3D& v = mesh.vertices[index];
                    p.at(k) = place.apply(vec(v.position));
                    uv.at(k) = {v.uv[0], v.uv[1]};
                    n.at(k) = normal_xform(vec(v.normal));
                }
                if (!valid) {
                    continue;
                }
                // Clockwise as the side sees it, Godot's front, is a normal by the right hand
                // pointing the way it looks.
                const bool front =
                    (dot(cross(p[1] - p[0], p[2] - p[0]), side.dir) > 0.0F) != mirrored;
                if ((from.cull_mode == render::CullMode::Back && !front) ||
                    (from.cull_mode == render::CullMode::Front && front)) {
                    continue;
                }
                if (!front && from.cull_mode == render::CullMode::Disabled) {
                    for (Vec3& v : n) {
                        v = v * -1.0F;
                    }
                }
                raster(p, uv, n, from, side, rect_pos, rect_size, z_base, z_sign, far, material);
            }
        }
    }

    // One triangle's pixels, sampled at their middles, with a top-left rule on shared edges.
    static void raster(const std::array<Vec3, 3>& p, const std::array<Vec2, 3>& uv,
                       const std::array<Vec3, 3>& n, const VoxelGIMaterial& from, const Side& side,
                       const std::array<int, 2>& rect_pos, const std::array<int, 2>& rect_size,
                       float z_base, float z_sign, float far,
                       std::vector<MaterialTexel>& material) {
        std::array<double, 3> u{};
        std::array<double, 3> v{};
        std::array<float, 3> d{};
        std::array<std::size_t, 3> order{0, 1, 2};
        for (std::size_t k = 0; k < 3; ++k) {
            u.at(k) = static_cast<double>(component(p.at(k), side.x_axis)) - rect_pos[0];
            v.at(k) = static_cast<double>(component(p.at(k), side.y_axis)) - rect_pos[1];
            d.at(k) = (component(p.at(k), side.z_axis) - z_base) * z_sign;
        }
        const auto edge = [](double ax, double ay, double bx, double by, double cx, double cy) {
            return ((bx - ax) * (cy - ay)) - ((by - ay) * (cx - ax));
        };
        double area = edge(u[0], v[0], u[1], v[1], u[2], v[2]);
        if (area == 0.0 || !std::isfinite(area)) {
            return;
        }
        if (area < 0.0) {
            std::swap(order[1], order[2]);
            area = -area;
        }
        const std::array<double, 3> x{u.at(order[0]), u.at(order[1]), u.at(order[2])};
        const std::array<double, 3> y{v.at(order[0]), v.at(order[1]), v.at(order[2])};
        const double low_x = std::min({x[0], x[1], x[2]});
        const double high_x = std::max({x[0], x[1], x[2]});
        const double low_y = std::min({y[0], y[1], y[2]});
        const double high_y = std::max({y[0], y[1], y[2]});
        const int x0 = static_cast<int>(std::max(0.0, std::floor(low_x)));
        const int x1 =
            static_cast<int>(std::min(static_cast<double>(rect_size[0] - 1), std::ceil(high_x)));
        const int y0 = static_cast<int>(std::max(0.0, std::floor(low_y)));
        const int y1 =
            static_cast<int>(std::min(static_cast<double>(rect_size[1] - 1), std::ceil(high_y)));
        // Each edge from a corner to the next: a pixel on it counts for the triangle it bounds
        // on its top or left.
        const auto counts_on = [&](std::size_t a) {
            const std::size_t b = (a + 1) % 3;
            const double dy = y.at(b) - y.at(a);
            return dy < 0.0 || (dy == 0.0 && x.at(b) - x.at(a) > 0.0);
        };
        const std::array<bool, 3> on{counts_on(1), counts_on(2), counts_on(0)};
        for (int py = y0; py <= y1; ++py) {
            const double cy = py + 0.5;
            for (int px = x0; px <= x1; ++px) {
                const double cx = px + 0.5;
                // Each weight is that of the corner across from the edge.
                const std::array<double, 3> w{edge(x[1], y[1], x[2], y[2], cx, cy),
                                              edge(x[2], y[2], x[0], y[0], cx, cy),
                                              edge(x[0], y[0], x[1], y[1], cx, cy)};
                bool inside = true;
                for (std::size_t k = 0; k < 3; ++k) {
                    if (w.at(k) < 0.0 || (w.at(k) == 0.0 && !on.at(k))) {
                        inside = false;
                    }
                }
                if (!inside) {
                    continue;
                }
                std::array<float, 3> b{};
                for (std::size_t k = 0; k < 3; ++k) {
                    b.at(k) = static_cast<float>(w.at(k) / area);
                }
                const std::size_t i0 = order[0];
                const std::size_t i1 = order[1];
                const std::size_t i2 = order[2];
                const float depth = (d.at(i0) * b[0]) + (d.at(i1) * b[1]) + (d.at(i2) * b[2]);
                // The orthographic view's near and far planes.
                if (!(depth >= 0.0001F) || depth > far) {
                    continue;
                }
                MaterialTexel& out = material[(static_cast<std::size_t>(py) *
                                               static_cast<std::size_t>(rect_size[0])) +
                                              static_cast<std::size_t>(px)];
                if (depth > out.nearest) {
                    continue;
                }
                const Vec2 at = (uv.at(i0) * b[0]) + (uv.at(i1) * b[1]) + (uv.at(i2) * b[2]);
                const std::size_t texel =
                    (static_cast<std::size_t>(texel_of(at.y)) * kVoxelGIMaterialSize) +
                    static_cast<std::size_t>(texel_of(at.x));
                const auto& albedo = from.albedo[texel];
                const auto& emission = from.emission[texel];
                out.nearest = depth;
                out.depth = depth;
                out.alpha = 1.0F;
                out.albedo = {albedo[0], albedo[1], albedo[2]};
                out.emission = {emission[0], emission[1], emission[2]};
                out.normal = normalized((n.at(i0) * b[0]) + (n.at(i1) * b[1]) + (n.at(i2) * b[2]));
            }
        }
    }

    // MODE_DYNAMIC_SHRINK_PLOT: a texel of light blended over a level of the texture, split
    // between the two texels along the side's axis its place falls between.
    void blit(std::size_t level, const Side& side, std::array<int, 2> at,
              const std::array<float, 4>& accum, float accum_z) {
        if (accum[3] < 0.001F) {
            return; // too faint to blit
        }
        const auto& size = texture_.sizes[level];
        std::vector<std::uint8_t>& texels = texture_.levels[level];
        std::array<int, 3> pos{};
        pos.at(static_cast<std::size_t>(side.x_axis)) = at[0];
        pos.at(static_cast<std::size_t>(side.y_axis)) = at[1];
        pos.at(static_cast<std::size_t>(side.z_axis)) = static_cast<int>(accum_z);
        const float z_frac = accum_z - std::floor(accum_z);
        const float range = settings_.dynamic_range;
        for (int i = 0; i < 2; ++i) {
            std::array<int, 3> p = pos;
            p.at(static_cast<std::size_t>(side.z_axis)) += i;
            bool outside = false;
            for (std::size_t a = 0; a < 3; ++a) {
                outside = outside || p.at(a) < 0 || p.at(a) >= static_cast<int>(size.at(a));
            }
            if (outside) {
                continue;
            }
            const float weight = i == 0 ? 1.0F - z_frac : z_frac;
            std::array<float, 4> blit{};
            for (std::size_t c = 0; c < 4; ++c) {
                blit.at(c) = accum.at(c) * weight;
            }
            const std::size_t index =
                ((((static_cast<std::size_t>(p[2]) * size[1]) + static_cast<std::size_t>(p[1])) *
                  size[0]) +
                 static_cast<std::size_t>(p[0])) *
                4;
            std::array<float, 4> color{};
            for (std::size_t c = 0; c < 4; ++c) {
                color.at(c) = static_cast<float>(texels[index + c]) / 255.0F;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                color.at(c) *= range;
            }
            const float sa = 1.0F - blit[3];
            const float alpha = (color[3] * sa) + blit[3];
            // Where nothing shows, Godot leaves the texel's color as it was.
            if (alpha != 0.0F) {
                for (std::size_t c = 0; c < 3; ++c) {
                    color.at(c) = ((color.at(c) * color[3] * sa) + (blit.at(c) * blit[3])) / alpha;
                }
                color[3] = alpha;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                color.at(c) /= range;
            }
            for (std::size_t c = 0; c < 4; ++c) {
                texels[index + c] = unorm8(color.at(c));
            }
        }
    }

    VoxelGITexture& texture_;
    LightField field_;
    const VoxelGISettings& settings_;
    jobs::Pool* pool_;
    std::array<std::uint32_t, 3> octree_size_;
    world::Affine3D to_map_; // the VoxelGI's space to the finest map's texels
};

} // namespace

json::Value voxel_gi_schema() {
    return file_schema();
}

std::vector<json::ValidationError> validate_voxel_gi(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, file_schema());
    if (errors.empty()) {
        read_voxel_gi(doc, nullptr, errors);
    }
    return errors;
}

Result<VoxelGIData> parse_voxel_gi(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, file_schema());
    if (!errors.empty()) {
        return first_error(errors);
    }
    VoxelGIData data;
    read_voxel_gi(doc, &data, errors);
    if (!errors.empty()) {
        return first_error(errors);
    }
    return data;
}

json::Value voxel_gi_json(const VoxelGIData& data) {
    json::Value doc;
    doc["format"] = 1;
    json::Array bounds;
    for (const float v : data.bounds_position) {
        bounds.emplace_back(v);
    }
    for (const float v : data.bounds_size) {
        bounds.emplace_back(v);
    }
    doc["bounds"] = std::move(bounds);
    doc["octree_size"] = json::Array{static_cast<std::int64_t>(data.octree_size[0]),
                                     static_cast<std::int64_t>(data.octree_size[1]),
                                     static_cast<std::int64_t>(data.octree_size[2])};
    if (data.baked_exposure != 1.0F) {
        doc["baked_exposure"] = data.baked_exposure;
    }
    json::Array counts;
    for (const std::uint32_t count : data.level_counts) {
        counts.emplace_back(static_cast<std::int64_t>(count));
    }
    doc["level_counts"] = std::move(counts);
    doc["cells"] = encode_values(data.cells);
    return doc;
}

void add_voxel_gi_types(asset::Assets& assets) {
    assets.add_type<VoxelGIData>(
        "voxel_gi", {std::string(kVoxelGISuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<VoxelGIData> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_voxel_gi(doc.value());
        },
        voxel_gi_bytes);
}

std::array<std::uint32_t, 3> voxel_gi_octree_size(std::array<float, 3> size, int subdiv) {
    return Voxelizer(subdiv, Box{vec(size) * -0.5F, vec(size)}).octree_size();
}

int voxel_gi_subdiv(std::string_view name) {
    if (name == "64") {
        return 6;
    }
    if (name == "128") {
        return 7;
    }
    if (name == "256") {
        return 8;
    }
    return name == "512" ? 9 : 0;
}

world::Affine3D voxel_gi_to_cell(const VoxelGIData& data) {
    const Box bounds{vec(data.bounds_position), vec(data.bounds_size)};
    const int longest = bounds.longest_axis();
    const float scale = static_cast<float>(data.octree_size.at(static_cast<std::size_t>(longest))) /
                        component(bounds.size, longest);
    world::Affine3D out;
    out.x = {scale, 0.0F, 0.0F};
    out.y = {0.0F, scale, 0.0F};
    out.z = {0.0F, 0.0F, scale};
    out.origin = bounds.position * -scale;
    return out;
}

VoxelGIMaterial voxel_gi_material(const world::StandardMaterial3D& standard,
                                  const render::Image* albedo, const render::Image* emission,
                                  float exposure) {
    VoxelGIMaterial out;
    const std::array<float, 3> albedo_color{srgb_to_linear(standard.albedo_color.x),
                                            srgb_to_linear(standard.albedo_color.y),
                                            srgb_to_linear(standard.albedo_color.z)};
    if (albedo != nullptr) {
        out.albedo = bake_texture(albedo, albedo_color, {0.0F, 0.0F, 0.0F});
    } else {
        out.albedo = bake_texture(nullptr, {1.0F, 1.0F, 1.0F}, albedo_color);
    }
    if (standard.cull_mode == "front") {
        out.cull_mode = render::CullMode::Front;
    } else if (standard.cull_mode == "disabled") {
        out.cull_mode = render::CullMode::Disabled;
    }
    if (standard.emission_enabled) {
        const float energy = standard.emission_energy_multiplier * exposure;
        const std::array<float, 3> color{srgb_to_linear(standard.emission.x) * energy,
                                         srgb_to_linear(standard.emission.y) * energy,
                                         srgb_to_linear(standard.emission.z) * energy};
        out.emission = bake_texture(emission, {energy, energy, energy}, color);
    } else {
        out.emission = bake_texture(nullptr, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F});
    }
    return out;
}

VoxelGIData voxelize(std::span<const VoxelGIMesh> meshes, std::array<float, 3> size, int subdiv,
                     float exposure) {
    Voxelizer voxelizer(subdiv, Box{vec(size) * -0.5F, vec(size)});
    for (const VoxelGIMesh& mesh : meshes) {
        voxelizer.plot_mesh(mesh);
    }
    return voxelizer.finish(exposure);
}

VoxelGIOctree voxel_gi_octree(const VoxelGIData& data, jobs::Pool* pool) {
    VoxelGIOctree out;
    const std::size_t levels = data.level_counts.size();
    out.level_offsets.push_back(0);
    for (const std::uint32_t count : data.level_counts) {
        out.level_offsets.push_back(out.level_offsets.back() + count);
    }
    const std::array<std::uint32_t, 8> none{kChildEmpty, kChildEmpty, kChildEmpty, kChildEmpty,
                                            kChildEmpty, kChildEmpty, kChildEmpty, kChildEmpty};
    out.children.assign(data.cells.size(), none);
    for (std::size_t level = 1; level < levels; ++level) {
        const auto begin = data.cells.begin() + out.level_offsets[level - 1];
        const auto end = data.cells.begin() + out.level_offsets[level];
        for (std::size_t i = out.level_offsets[level]; i < out.level_offsets[level + 1]; ++i) {
            const auto p = cell_position(data.cells[i]);
            const std::uint64_t key = cell_key(p[0] >> 1U, p[1] >> 1U, p[2] >> 1U);
            const auto found =
                std::lower_bound(begin, end, key, [](const VoxelGICell& c, std::uint64_t k) {
                    return cell_key(c) < k;
                });
            if (found == end || cell_key(*found) != key) {
                continue; // not valid data
            }
            const auto parent = static_cast<std::size_t>(found - data.cells.begin());
            const std::size_t slot = (p[0] & 1U) | ((p[1] & 1U) << 1U) | ((p[2] & 1U) << 2U);
            out.children[parent].at(slot) = static_cast<std::uint32_t>(i);
        }
    }

    // Voxelizer::get_sdf_3d_image.
    const auto sx = static_cast<std::size_t>(data.octree_size[0]);
    const auto sy = static_cast<std::size_t>(data.octree_size[1]);
    const auto sz = static_cast<std::size_t>(data.octree_size[2]);
    std::vector<float> work(sx * sy * sz, kBig);
    const std::size_t y_mult = sx;
    const std::size_t z_mult = sx * sy;
    if (levels > 0) {
        for (std::size_t i = out.level_offsets[levels - 1]; i < out.level_offsets[levels]; ++i) {
            const auto p = cell_position(data.cells[i]);
            if (p[0] < sx && p[1] < sy && p[2] < sz) {
                work[p[0] + (p[1] * y_mult) + (p[2] * z_mult)] = 0.0F;
            }
        }
    }
    struct Scratch {
        std::vector<float> d;
        std::vector<int> v;
        std::vector<float> z;
    };
    const auto threads = static_cast<std::size_t>(pool != nullptr ? pool->thread_count() : 0) + 1;
    std::vector<Scratch> scratch(threads);
    const auto pass = [&](std::size_t rows, const std::function<std::size_t(std::size_t)>& start,
                          std::size_t stride, std::size_t n) {
        run(pool, rows, [&](std::size_t row) {
            Scratch& s =
                scratch[pool != nullptr ? static_cast<std::size_t>(pool->thread_index()) : 0];
            edt(&work[start(row)], stride, static_cast<int>(n), s.d, s.v, s.z);
        });
    };
    // xy -> z, xz -> y, yz -> x, as Godot's.
    pass(sx * sy, [&](std::size_t r) { return (r / sy) + ((r % sy) * y_mult); }, z_mult, sz);
    pass(sx * sz, [&](std::size_t r) { return (r / sz) + ((r % sz) * z_mult); }, y_mult, sy);
    pass(sy * sz, [&](std::size_t r) { return ((r / sz) * y_mult) + ((r % sz) * z_mult); }, 1, sx);
    out.distance_field.resize(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) {
        const float root = std::sqrt(work[i]);
        // Godot's uint32_t(sqrt): no place is farther than 255 that matters.
        const std::uint32_t d = root >= 255.0F ? 255U : static_cast<std::uint32_t>(root);
        out.distance_field[i] = d == 0 ? 0 : static_cast<std::uint8_t>(std::min(d, 254U) + 1);
    }
    return out;
}

VoxelGITexture light_voxel_gi(const VoxelGIData& data, const VoxelGIOctree& octree,
                              std::span<const VoxelGILight> lights, const VoxelGISettings& settings,
                              jobs::Pool* pool) {
    if (data.level_counts.empty() || data.cells.empty()) {
        return {};
    }
    Injector injector(data, octree, lights, settings, pool);
    return injector.run_passes();
}

std::vector<VoxelGIMesh> voxel_gi_meshes(const world::World& world, entt::entity entity,
                                         const LightmapSources& sources, float exposure,
                                         std::vector<Problem>& problems) {
    std::vector<VoxelGIMesh> out;
    if (!world.valid(entity) || !world.ecs().all_of<world::MeshInstance3D>(entity)) {
        return out;
    }
    MeshReader reader(world, sources, exposure, problems);
    reader.read(entity, out);
    return out;
}

void plot_voxel_gi_objects(VoxelGITexture& texture, const VoxelGIData& data,
                           const VoxelGIOctree& octree, std::span<const VoxelGIMesh> objects,
                           std::span<const VoxelGILight> lights, const VoxelGISettings& settings,
                           jobs::Pool* pool) {
    if (texture.levels.empty() || texture.levels.size() != texture.sizes.size() ||
        texture.sizes[0] != data.octree_size || octree.distance_field.empty()) {
        return;
    }
    ObjectPlotter plotter(texture, data, octree, lights, settings, pool);
    for (const VoxelGIMesh& object : objects) {
        plotter.plot(object);
    }
}

Result<VoxelGIAtlas> voxel_gi_atlas(const VoxelGITexture& texture) {
    const std::size_t levels = texture.sizes.size();
    if (levels == 0 || levels > render::kMaxVoxelGILevels || texture.levels.size() != levels) {
        return make_error(ErrorCode::InvalidArgument,
                          "a VoxelGI's texture has 1 to " +
                              std::to_string(render::kMaxVoxelGILevels) + " levels");
    }
    for (std::size_t i = 0; i < levels; ++i) {
        const std::array<std::uint32_t, 3>& size = texture.sizes[i];
        if (size[0] == 0 || size[1] == 0 || size[2] == 0 || size[0] > render::kMaxVoxelGICells ||
            size[1] > render::kMaxVoxelGICells || size[2] > render::kMaxVoxelGICells ||
            texture.levels[i].size() != std::size_t{size[0]} * size[1] * size[2] * 4) {
            return make_error(ErrorCode::InvalidArgument,
                              "a VoxelGI's texture has 1 to " +
                                  std::to_string(render::kMaxVoxelGICells) +
                                  " texels along each axis and 4 bytes a texel");
        }
    }
    const std::array<std::uint32_t, 3>& base = texture.sizes[0];
    const std::uint64_t one_row = std::uint64_t{base[0]} * base[2];
    VoxelGIAtlas out;
    out.levels = static_cast<std::uint32_t>(levels);
    std::uint64_t height = 0;
    std::uint64_t width = 0;
    for (const std::uint64_t limit :
         {std::uint64_t{4096}, std::uint64_t{8192}, std::uint64_t{render::kMaxTextureSize}}) {
        const std::uint64_t most = std::max<std::uint64_t>(std::min(one_row, limit), base[0]);
        height = 0;
        width = 0;
        for (std::size_t i = 0; i < levels; ++i) {
            const std::array<std::uint32_t, 3>& size = texture.sizes[i];
            const std::uint64_t columns = std::clamp<std::uint64_t>(most / size[0], 1, size[2]);
            const std::uint64_t rows = (size[2] + columns - 1) / columns;
            out.placement.at(i) = {0, static_cast<std::uint32_t>(height),
                                   static_cast<std::uint32_t>(columns)};
            width = std::max(width, columns * size[0]);
            height += rows * size[1];
        }
        if (height <= render::kMaxTextureSize) {
            break;
        }
    }
    if (height > render::kMaxTextureSize || width > render::kMaxTextureSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "a VoxelGI's texture is too big for an atlas of " +
                              std::to_string(render::kMaxTextureSize) + " texels a side");
    }
    out.image.width = static_cast<std::uint32_t>(width);
    out.image.height = static_cast<std::uint32_t>(height);
    out.image.pixels.assign(static_cast<std::size_t>(width * height * 4), 0);
    for (std::size_t i = 0; i < levels; ++i) {
        const std::array<std::uint32_t, 3>& size = texture.sizes[i];
        const std::array<std::uint32_t, 3>& place = out.placement.at(i);
        const std::size_t row_bytes = std::size_t{size[0]} * 4;
        for (std::uint32_t z = 0; z < size[2]; ++z) {
            const std::size_t left = place[0] + (std::size_t{z % place[2]} * size[0]);
            const std::size_t top = place[1] + (std::size_t{z / place[2]} * size[1]);
            for (std::uint32_t y = 0; y < size[1]; ++y) {
                const std::size_t from = ((std::size_t{z} * size[1]) + y) * row_bytes;
                const std::size_t to = (((top + y) * out.image.width) + left) * 4;
                std::copy_n(texture.levels[i].begin() + static_cast<std::ptrdiff_t>(from),
                            row_bytes, out.image.pixels.begin() + static_cast<std::ptrdiff_t>(to));
            }
        }
    }
    return out;
}

render::VoxelGIProbe voxel_gi_probe(const world::VoxelGI& voxel_gi, const VoxelGIData& data,
                                    const VoxelGIAtlas& atlas, const world::Affine3D& transform,
                                    float exposure_normalization) {
    const world::Affine3D to_cell = voxel_gi_to_cell(data) * transform.inverse();
    render::VoxelGIProbe probe;
    for (std::size_t row = 0; row < 3; ++row) {
        const auto at = static_cast<int>(row);
        probe.to_cell.at(row * 4) = detail::component(to_cell.x, at);
        probe.to_cell.at((row * 4) + 1) = detail::component(to_cell.y, at);
        probe.to_cell.at((row * 4) + 2) = detail::component(to_cell.z, at);
        probe.to_cell.at((row * 4) + 3) = detail::component(to_cell.origin, at);
    }
    probe.size = data.octree_size;
    probe.levels = atlas.levels;
    probe.placement = atlas.placement;
    probe.energy = voxel_gi.dynamic_range * voxel_gi.energy * exposure_normalization;
    probe.bias = voxel_gi.bias;
    probe.normal_bias = voxel_gi.normal_bias;
    probe.blend_ambient = !voxel_gi.interior;
    return probe;
}

VoxelGILight voxel_gi_light(const render::Light3D& light, float indirect_energy,
                            const VoxelGIData& data, const world::Affine3D& transform) {
    VoxelGILight out;
    const world::Affine3D to_cell = voxel_gi_to_cell(data);
    const world::Affine3D to_probe = to_cell * transform.inverse();
    out.type = light.type;
    out.attenuation = light.attenuation;
    out.energy = light.energy * indirect_energy;
    out.radius = length(to_cell.basis(Vec3{light.range, 0.0F, 0.0F}));
    out.color = {srgb_to_linear(light.color[0]), srgb_to_linear(light.color[1]),
                 srgb_to_linear(light.color[2])};
    out.cos_spot_angle =
        slash::cos_sin(light.spot_angle * static_cast<float>(std::numbers::pi / 180.0)).x;
    out.inv_spot_attenuation = 1.0F / light.spot_attenuation;
    const Vec3 pos = to_probe.apply(vec(light.position));
    const Vec3 dir = normalized(to_probe.basis(vec(light.direction)));
    out.position = {pos.x, pos.y, pos.z};
    out.direction = {dir.x, dir.y, dir.z};
    out.has_shadow = light.shadow;
    return out;
}

Result<VoxelGIBake> bake_voxel_gi(const world::World& world, entt::entity entity,
                                  const LightmapSources& sources) {
    if (!world.valid(entity) || !world.ecs().all_of<world::VoxelGI>(entity)) {
        return make_error(ErrorCode::InvalidArgument, "the entity has no VoxelGI");
    }
    const auto& gi = world.ecs().get<const world::VoxelGI>(entity);
    const int subdiv = voxel_gi_subdiv(gi.subdiv);
    if (subdiv == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "subdiv \"" + gi.subdiv + R"(" isn't "64", "128", "256" or "512")");
    }
    if (!(gi.size.x > 0.0F) || !(gi.size.y > 0.0F) || !(gi.size.z > 0.0F) ||
        !std::isfinite(gi.size.x + gi.size.y + gi.size.z)) {
        return make_error(ErrorCode::InvalidArgument, "size must be positive");
    }
    float exposure = 1.0F;
    if (!gi.camera_attributes.empty()) {
        const entt::entity attributes = world.find(gi.camera_attributes);
        if (attributes == entt::null ||
            !world.ecs().all_of<world::CameraAttributesPractical>(attributes)) {
            return make_error(ErrorCode::InvalidArgument,
                              "camera_attributes \"" + gi.camera_attributes +
                                  "\" isn't an entity with CameraAttributesPractical");
        }
        exposure =
            world.ecs().get<const world::CameraAttributesPractical>(attributes).exposure_multiplier;
    }
    VoxelGIBake out;
    const world::Tree3D tree(world);
    const world::Affine3D to_space = tree.global(entity).inverse();
    const entt::entity scope = tree.parent(entity);
    const std::array<float, 3> size{gi.size.x, gi.size.y, gi.size.z};
    const Box box{gi.size * -0.5F, gi.size};
    // The static MeshInstance3Ds at or under its parent touching its box, as
    // VoxelGI::_find_meshes finds them.
    MeshReader reader(world, sources, exposure, out.problems);
    std::vector<VoxelGIMesh> meshes;
    for (const entt::entity found : made<world::MeshInstance3D>(world)) {
        const auto& instance = world.ecs().get<const world::MeshInstance3D>(found);
        if (instance.gi_mode != "static" || !tree.visible(found) ||
            !at_or_under(tree, found, scope)) {
            continue;
        }
        std::vector<VoxelGIMesh> read;
        reader.read(found, read);
        const world::Affine3D place = to_space * tree.global(found);
        for (VoxelGIMesh& mesh : read) {
            mesh.transform = place * mesh.transform;
            if (placed_box(mesh.vertices, mesh.transform).intersects(box)) {
                meshes.push_back(std::move(mesh));
            }
        }
    }
    out.meshes = meshes.size();
    out.data = voxelize(meshes, size, subdiv, exposure);
    return out;
}

std::vector<Problem> check_voxel_gis(const world::World& world) {
    std::vector<Problem> problems;
    for (const entt::entity entity : made<world::VoxelGI>(world)) {
        const auto& gi = world.ecs().get<const world::VoxelGI>(entity);
        const std::string base = "/components/VoxelGI/";
        if (voxel_gi_subdiv(gi.subdiv) == 0) {
            problems.push_back({entity, base + "subdiv",
                                "\"" + gi.subdiv + R"(" isn't "64", "128", "256" or "512")"});
        }
        if (!(gi.size.x > 0.0F) || !(gi.size.y > 0.0F) || !(gi.size.z > 0.0F)) {
            problems.push_back({entity, base + "size", "must be positive"});
        }
        if (!gi.camera_attributes.empty()) {
            const entt::entity attributes = world.find(gi.camera_attributes);
            if (attributes == entt::null ||
                !world.ecs().all_of<world::CameraAttributesPractical>(attributes)) {
                problems.push_back({entity, base + "camera_attributes",
                                    "\"" + gi.camera_attributes +
                                        "\" isn't an entity with CameraAttributesPractical"});
            }
        }
        const std::string& path = gi.data;
        if (path.empty()) {
            continue;
        }
        const std::string field = base + "data";
        if (!path.ends_with(kVoxelGISuffix)) {
            problems.push_back(
                {entity, field,
                 "\"" + path + "\" isn't a " + std::string(kVoxelGISuffix) + " file"});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto text = reader(path);
        if (!text) {
            problems.push_back(
                {entity, field, "\"" + path + "\" could not be read: " + text.error().message});
            continue;
        }
        auto doc = json::parse(text.value());
        if (!doc) {
            problems.push_back({entity, field, "\"" + path + "\": " + doc.error().message});
            continue;
        }
        const std::vector<json::ValidationError> errors = validate_voxel_gi(doc.value());
        if (!errors.empty()) {
            problems.push_back(
                {entity, field,
                 "\"" + path + "\" at " + errors.front().path + ": " + errors.front().message});
        }
    }
    return problems;
}

} // namespace slash::scene3d
