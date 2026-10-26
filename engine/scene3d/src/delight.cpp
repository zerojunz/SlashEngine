// Taking the light baked into a model's albedo textures out of them (cleanup.hpp's
// delight_textures): each texel's place on the model and its ambient occlusion, a light of an
// ambient and a directional part fitted to the texels' luminance, and the texels divided by it.

#include "model_internal.hpp"

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/cleanup.hpp"
#include "slash/scene3d/occlusion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// The most cells a side of the grid the light is worked out on: baked light changes slowly
// across a texture, and the rays it takes grow with the cells.
constexpr std::uint32_t kGridSide = 256;
// Rays cast from each place to measure its ambient occlusion.
constexpr std::uint32_t kRays = 16;
// The most places on the model one cell keeps, for a texture the model repeats.
constexpr std::size_t kPlacesPerCell = 4;
// How far a texel may be brightened or darkened.
constexpr float kMinFactor = 0.25F;
constexpr float kMaxFactor = 4.0F;
// Passes spreading cells' factors into the cells no surface covers, so filtering across a
// chart's edge finds its own.
constexpr int kDilatePasses = 8;
constexpr float kTwoPi = 6.28318530718F;
// How much the surfaces' normals must vary along an axis, by variance, for a light along it to be
// told apart from the ambient part.
constexpr double kLeastTurn = 1e-3;

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 normalize(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return length > 0.0F ? v * (1.0F / length) : Vec3{};
}

// sRGB codes to linear light, made with the engine's exp and ln so every machine makes the same
// table.
const std::array<float, 256>& srgb_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0F;
            out[i] =
                c <= 0.04045F ? c / 12.92F : slash::exp(2.4F * slash::ln((c + 0.055F) / 1.055F));
        }
        return out;
    }();
    return table;
}

// The sRGB code whose linear light is nearest, found in the table, so encoding needs no pow.
std::uint8_t to_srgb(float linear) {
    const std::array<float, 256>& table = srgb_table();
    const auto* above = std::lower_bound(table.data(), table.data() + table.size(), linear);
    if (above == table.data()) {
        return 0;
    }
    if (above == table.data() + table.size()) {
        return 255;
    }
    const auto index = static_cast<std::size_t>(above - table.data());
    return static_cast<std::uint8_t>(linear - table[index - 1] <= *above - linear ? index - 1
                                                                                  : index);
}

float luminance(const std::array<float, 3>& rgb) {
    return (0.2126F * rgb[0]) + (0.7152F * rgb[1]) + (0.0722F * rgb[2]);
}

// A place on the model a cell of the grid covers.
struct Place {
    Vec3 position;
    Vec3 normal;
};

struct Cell {
    std::vector<Place> places;
    float occlusion = 1.0F; // the share of rays that get away, averaged over its places
    Vec3 normal;            // its places' normals, averaged
};

struct Grid {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Cell> cells;

    Cell& at(std::int64_t x, std::int64_t y) {
        const auto w = static_cast<std::int64_t>(width);
        const auto h = static_cast<std::int64_t>(height);
        const auto wx = static_cast<std::size_t>(((x % w) + w) % w);
        const auto wy = static_cast<std::size_t>(((y % h) + h) % h);
        return cells[(wy * width) + wx];
    }
};

void add_place(Cell& cell, const Place& place) {
    if (cell.places.size() < kPlacesPerCell) {
        cell.places.push_back(place);
    }
}

// A triangle's corners on the model and in the grid's cells.
struct Corner {
    Vec3 position;
    Vec3 normal;
    float x = 0.0F;
    float y = 0.0F;
};

// Puts the places a triangle covers into the cells whose centers it covers, or, for a triangle
// that covers no center, its middle into the cell it is in.
void draw(Grid& grid, const std::array<Corner, 3>& c) {
    const float area =
        ((c[1].x - c[0].x) * (c[2].y - c[0].y)) - ((c[2].x - c[0].x) * (c[1].y - c[0].y));
    bool covered = false;
    if (area != 0.0F) {
        const float min_x = std::min({c[0].x, c[1].x, c[2].x});
        const float max_x = std::max({c[0].x, c[1].x, c[2].x});
        const float min_y = std::min({c[0].y, c[1].y, c[2].y});
        const float max_y = std::max({c[0].y, c[1].y, c[2].y});
        // A triangle wider than the grid many times over repeats; it is drawn once round it.
        const auto x0 = static_cast<std::int64_t>(std::floor(min_x - 0.5F));
        const auto x1 = std::min(static_cast<std::int64_t>(std::ceil(max_x - 0.5F)),
                                 x0 + (4 * static_cast<std::int64_t>(grid.width)));
        const auto y0 = static_cast<std::int64_t>(std::floor(min_y - 0.5F));
        const auto y1 = std::min(static_cast<std::int64_t>(std::ceil(max_y - 0.5F)),
                                 y0 + (4 * static_cast<std::int64_t>(grid.height)));
        for (std::int64_t y = y0; y <= y1; ++y) {
            for (std::int64_t x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const float w0 =
                    (((c[1].x - px) * (c[2].y - py)) - ((c[2].x - px) * (c[1].y - py))) / area;
                const float w1 =
                    (((c[2].x - px) * (c[0].y - py)) - ((c[0].x - px) * (c[2].y - py))) / area;
                const float w2 = 1.0F - w0 - w1;
                if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) {
                    continue;
                }
                covered = true;
                add_place(
                    grid.at(x, y),
                    {.position = (c[0].position * w0) + (c[1].position * w1) + (c[2].position * w2),
                     .normal =
                         normalize((c[0].normal * w0) + (c[1].normal * w1) + (c[2].normal * w2))});
            }
        }
    }
    if (!covered) {
        const float third = 1.0F / 3.0F;
        add_place(
            grid.at(static_cast<std::int64_t>(std::floor((c[0].x + c[1].x + c[2].x) * third)),
                    static_cast<std::int64_t>(std::floor((c[0].y + c[1].y + c[2].y) * third))),
            {.position = (c[0].position + c[1].position + c[2].position) * third,
             .normal = normalize(c[0].normal + c[1].normal + c[2].normal)});
    }
}

// The share of kRays cosine-weighted rays from a place that leave the model within reach: a
// Hammersley set turned round the normal, the same rays at every place.
float occlusion_at(const OccluderScene& scene, const Place& place, float reach) {
    const Vec3 n = place.normal;
    if (dot(n, n) == 0.0F) {
        return 1.0F;
    }
    const Vec3 helper = std::abs(n.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 t = normalize(cross(helper, n));
    const Vec3 b = cross(n, t);
    const Vec3 origin = place.position + (n * (reach * 1e-3F));
    std::uint32_t free = 0;
    for (std::uint32_t i = 0; i < kRays; ++i) {
        // Van der Corput's radical inverse in base 2 for the second coordinate.
        std::uint32_t bits = i;
        bits = (bits << 16U) | (bits >> 16U);
        bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
        bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
        bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
        bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
        const float u = (static_cast<float>(i) + 0.5F) / static_cast<float>(kRays);
        const float v = static_cast<float>(bits) * 2.3283064365386963e-10F;
        const float radius = std::sqrt(u);
        const Vec2 cs = cos_sin(kTwoPi * v);
        const float up = std::sqrt(std::max(0.0F, 1.0F - u));
        const Vec3 dir = normalize((t * (radius * cs.x)) + (b * (radius * cs.y)) + (n * up));
        if (scene.cast(origin, dir, 0.0F, reach) >= reach) {
            ++free;
        }
    }
    return static_cast<float>(free) / static_cast<float>(kRays);
}

// Solves the 4 by 4 system a x = b by Gaussian elimination with partial pivoting; nullopt when
// it has no single answer.
std::optional<std::array<double, 4>> solve(std::array<std::array<double, 5>, 4> m) {
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < 4; ++row) {
            if (std::abs(m[row][col]) > std::abs(m[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(m[pivot][col]) < 1e-9) {
            return std::nullopt;
        }
        std::swap(m[col], m[pivot]);
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const double f = m[row][col] / m[col][col];
            for (std::size_t k = col; k < 5; ++k) {
                m[row][k] -= f * m[col][k];
            }
        }
    }
    std::array<double, 4> x{};
    for (std::size_t i = 0; i < 4; ++i) {
        x[i] = m[i][4] / m[i][i];
    }
    return x;
}

// A model's image's pixels: decoded already, or read from its file.
// An error about an image's file, naming it unless it does already.
Error image_error(const ModelImage& image, const Error& error) {
    if (error.message.find(image.path) != std::string::npos) {
        return error;
    }
    return make_error(error.code, quoted(image.path) + ": " + error.message);
}

Result<render::Image> pixels_of(const ModelImage& image, const ModelReader& read) {
    if (!image.image.pixels.empty() || image.path.empty()) {
        return image.image;
    }
    if (!read) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(image.path) + " can't be read to take its light out");
    }
    auto bytes = read(image.path);
    if (!bytes) {
        return image_error(image, bytes.error());
    }
    auto decoded = render::decode_image(bytes.value());
    if (!decoded) {
        return image_error(image, decoded.error());
    }
    return std::move(decoded).value();
}

struct Delit {
    render::Image image;
    std::size_t cells = 0; // cells the model covers
    std::optional<Vec3> light;
};

// One albedo image with the light taken out, or nullopt when no surface samples it.
std::optional<Delit> delight_image(const Model& model, int image_index, render::Image image,
                                   const OccluderScene& scene, float reach, float strength) {
    const float scale = std::min(1.0F, static_cast<float>(kGridSide) /
                                           static_cast<float>(std::max(image.width, image.height)));
    Grid grid;
    grid.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(
                                                (static_cast<float>(image.width) * scale) + 0.5F)));
    grid.height = std::max<std::uint32_t>(
        1,
        static_cast<std::uint32_t>(std::floor((static_cast<float>(image.height) * scale) + 0.5F)));
    grid.cells.resize(static_cast<std::size_t>(grid.width) * grid.height);

    bool sampled = false;
    for (const ModelPart& part : model.parts) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            if (surface.material < 0 ||
                static_cast<std::size_t>(surface.material) >= model.materials.size() ||
                model.materials[static_cast<std::size_t>(surface.material)].albedo_image !=
                    image_index) {
                continue;
            }
            sampled = true;
            for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
                std::array<Corner, 3> corners;
                for (std::size_t k = 0; k < 3; ++k) {
                    const render::Vertex3D& v = surface.vertices[surface.indices[i + k]];
                    corners[k] = {.position = part.transform.apply(position_of(v)),
                                  .normal = normalize(part.transform.basis(
                                      {v.normal[0], v.normal[1], v.normal[2]})),
                                  .x = v.uv[0] * static_cast<float>(grid.width),
                                  .y = v.uv[1] * static_cast<float>(grid.height)};
                }
                draw(grid, corners);
            }
        }
    }
    if (!sampled) {
        return std::nullopt;
    }

    // Each cell's occlusion and facing, and the texture's luminance over it.
    const std::array<float, 256>& table = srgb_table();
    std::vector<double> lum(grid.cells.size(), 0.0);
    std::vector<std::uint32_t> lum_count(grid.cells.size(), 0);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::uint32_t gy = std::min(grid.height - 1, (y * grid.height) / image.height);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t at = ((static_cast<std::size_t>(y) * image.width) + x) * 4;
            if (image.pixels[at + 3] == 0) {
                continue;
            }
            const std::uint32_t gx = std::min(grid.width - 1, (x * grid.width) / image.width);
            const std::size_t cell = (static_cast<std::size_t>(gy) * grid.width) + gx;
            lum[cell] += luminance({table[image.pixels[at]], table[image.pixels[at + 1]],
                                    table[image.pixels[at + 2]]});
            ++lum_count[cell];
        }
    }
    std::array<std::array<double, 5>, 4> normal_equations{};
    std::size_t covered = 0;
    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        Cell& cell = grid.cells[i];
        if (cell.places.empty()) {
            continue;
        }
        float occlusion = 0.0F;
        Vec3 normal;
        for (const Place& place : cell.places) {
            occlusion += occlusion_at(scene, place, reach);
            normal = normal + place.normal;
        }
        cell.occlusion = occlusion / static_cast<float>(cell.places.size());
        cell.normal = normalize(normal);
        if (lum_count[i] == 0) {
            continue;
        }
        ++covered;
        const double l = lum[i] / static_cast<double>(lum_count[i]);
        const auto a = static_cast<double>(cell.occlusion);
        const std::array<double, 4> row{a, a * cell.normal.x, a * cell.normal.y, a * cell.normal.z};
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                normal_equations[r][c] += row[r] * row[c];
            }
            normal_equations[r][4] += row[r] * l;
        }
    }
    if (covered == 0) {
        return std::nullopt;
    }
    // The light along an axis the surfaces hardly turn along (a card's up, a tube's length) can't
    // be told from the ambient part, so it is none.
    const double weight = normal_equations[0][0];
    if (weight <= 0.0) {
        return std::nullopt; // shut in on every side, so nothing lit it
    }
    for (std::size_t axis = 1; axis < 4; ++axis) {
        const double mean = normal_equations[0][axis] / weight;
        if ((normal_equations[axis][axis] / weight) - (mean * mean) >= kLeastTurn) {
            continue;
        }
        for (std::size_t k = 0; k < 5; ++k) {
            normal_equations[axis][k] = 0.0;
        }
        for (std::size_t r = 0; r < 4; ++r) {
            normal_equations[r][axis] = 0.0;
        }
        normal_equations[axis][axis] = 1.0;
    }
    std::array<double, 4> light{};
    if (auto fitted = solve(normal_equations)) {
        light = *fitted;
    } else {
        light[0] = normal_equations[0][4] / weight;
    }

    // What the light gave each cell, against its average.
    std::vector<float> shading(grid.cells.size(), 0.0F);
    std::vector<bool> known(grid.cells.size(), false);
    double total = 0.0;
    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        const Cell& cell = grid.cells[i];
        if (cell.places.empty()) {
            continue;
        }
        const double s = static_cast<double>(cell.occlusion) *
                         (light[0] + (light[1] * cell.normal.x) + (light[2] * cell.normal.y) +
                          (light[3] * cell.normal.z));
        shading[i] = static_cast<float>(std::max(s, 1e-4));
        known[i] = true;
        if (lum_count[i] > 0) {
            total += shading[i];
        }
    }
    const auto average = static_cast<float>(total / static_cast<double>(covered));
    std::vector<float> factor(grid.cells.size(), 1.0F);
    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        if (known[i]) {
            const float f = std::clamp(average / shading[i], kMinFactor, kMaxFactor);
            factor[i] = 1.0F + (strength * (f - 1.0F));
        }
    }
    // Into the cells around the charts, so filtering across an edge stays on the chart's side.
    for (int pass = 0; pass < kDilatePasses; ++pass) {
        std::vector<float> next = factor;
        std::vector<bool> next_known = known;
        for (std::uint32_t y = 0; y < grid.height; ++y) {
            for (std::uint32_t x = 0; x < grid.width; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * grid.width) + x;
                if (known[i]) {
                    continue;
                }
                float sum = 0.0F;
                int count = 0;
                const std::array<std::array<int, 2>, 4> around{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
                for (const auto& d : around) {
                    const auto nx = static_cast<std::int64_t>(x) + d[0];
                    const auto ny = static_cast<std::int64_t>(y) + d[1];
                    if (nx < 0 || ny < 0 || nx >= grid.width || ny >= grid.height) {
                        continue;
                    }
                    const std::size_t j =
                        (static_cast<std::size_t>(ny) * grid.width) + static_cast<std::size_t>(nx);
                    if (known[j]) {
                        sum += factor[j];
                        ++count;
                    }
                }
                if (count > 0) {
                    next[i] = sum / static_cast<float>(count);
                    next_known[i] = true;
                }
            }
        }
        factor = std::move(next);
        known = std::move(next_known);
    }

    // Each texel divided by the light, the factors filtered between the cells' centers.
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const float fy =
            std::clamp(((static_cast<float>(y) + 0.5F) * static_cast<float>(grid.height) /
                        static_cast<float>(image.height)) -
                           0.5F,
                       0.0F, static_cast<float>(grid.height - 1));
        const auto y0 = static_cast<std::uint32_t>(fy);
        const std::uint32_t y1 = std::min(y0 + 1, grid.height - 1);
        const float ty = fy - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float fx =
                std::clamp(((static_cast<float>(x) + 0.5F) * static_cast<float>(grid.width) /
                            static_cast<float>(image.width)) -
                               0.5F,
                           0.0F, static_cast<float>(grid.width - 1));
            const auto x0 = static_cast<std::uint32_t>(fx);
            const std::uint32_t x1 = std::min(x0 + 1, grid.width - 1);
            const float tx = fx - static_cast<float>(x0);
            const auto f_at = [&](std::uint32_t cx, std::uint32_t cy) {
                return factor[(static_cast<std::size_t>(cy) * grid.width) + cx];
            };
            const float top = f_at(x0, y0) + ((f_at(x1, y0) - f_at(x0, y0)) * tx);
            const float bottom = f_at(x0, y1) + ((f_at(x1, y1) - f_at(x0, y1)) * tx);
            const float f = top + ((bottom - top) * ty);
            const std::size_t at = ((static_cast<std::size_t>(y) * image.width) + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                image.pixels[at + c] = to_srgb(std::min(1.0F, table[image.pixels[at + c]] * f));
            }
        }
    }
    image.blocks.clear();
    image.block_format = render::TextureFormat::Rgba8;

    Delit out{.image = std::move(image), .cells = covered, .light = std::nullopt};
    const Vec3 direction{static_cast<float>(light[1]), static_cast<float>(light[2]),
                         static_cast<float>(light[3])};
    // A directional part under a twentieth of the ambient one is no light from a side.
    if (light[0] > 0.0 &&
        std::sqrt(dot(direction, direction)) > 0.05F * static_cast<float>(light[0])) {
        out.light = normalize(direction);
    }
    return out;
}

} // namespace

Result<DelightReport> delight_textures(Model& model, const ModelReader& read, float strength) {
    DelightReport report;
    strength = std::clamp(strength, 0.0F, 1.0F);
    if (strength == 0.0F) {
        return report;
    }
    // Every triangle the model draws, where rays may be blocked.
    std::vector<std::array<Vec3, 3>> triangles;
    Bounds bounds{.min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max()},
                  .max = Vec3{std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()}};
    for (const ModelPart& part : model.parts) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
                std::array<Vec3, 3> triangle;
                for (std::size_t k = 0; k < 3; ++k) {
                    triangle[k] =
                        part.transform.apply(position_of(surface.vertices[surface.indices[i + k]]));
                    bounds.min = {std::min(bounds.min.x, triangle[k].x),
                                  std::min(bounds.min.y, triangle[k].y),
                                  std::min(bounds.min.z, triangle[k].z)};
                    bounds.max = {std::max(bounds.max.x, triangle[k].x),
                                  std::max(bounds.max.y, triangle[k].y),
                                  std::max(bounds.max.z, triangle[k].z)};
                }
                triangles.push_back(triangle);
            }
        }
    }
    if (triangles.empty()) {
        return report;
    }
    const Vec3 size = bounds.max - bounds.min;
    // Rays reach half across the model, so its far side doesn't shade everything.
    const float reach = std::max(1e-4F, 0.5F * std::sqrt(dot(size, size)));
    OccluderScene scene;
    scene.build(std::move(triangles));

    std::size_t largest = 0;
    const std::size_t image_count = model.images.size();
    for (std::size_t i = 0; i < image_count; ++i) {
        const auto index = static_cast<int>(i);
        if (std::ranges::none_of(model.materials, [index](const ModelMaterial& material) {
                return material.albedo_image == index;
            })) {
            continue;
        }
        auto pixels = pixels_of(model.images[i], read);
        if (!pixels) {
            return pixels.error();
        }
        auto delit = delight_image(model, index, std::move(pixels).value(), scene, reach, strength);
        if (!delit) {
            continue;
        }
        // In place, unless a material samples the image for something else, which keeps it.
        const bool shared = std::ranges::any_of(model.materials, [index](const ModelMaterial& m) {
            return m.metallic_image == index || m.roughness_image == index ||
                   m.normal_image == index || m.emission_image == index;
        });
        if (!shared) {
            model.images[i] = {.path = "", .image = std::move(delit->image)};
        } else {
            const auto made = static_cast<int>(model.images.size());
            model.images.push_back({.path = "", .image = std::move(delit->image)});
            for (ModelMaterial& material : model.materials) {
                if (material.albedo_image == index) {
                    material.albedo_image = made;
                }
            }
        }
        ++report.textures;
        if (delit->cells > largest) {
            largest = delit->cells;
            report.light = delit->light;
        }
    }
    return report;
}

} // namespace slash::scene3d
