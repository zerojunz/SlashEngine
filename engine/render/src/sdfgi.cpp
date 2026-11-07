// Godot 4.6's SDFGI on the CPU (sdfgi.hpp): GI::SDFGI's create, update, render_region,
// update_light, update_probes and store_probes, and the shaders they run, step by step.

#include "sdfgi.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace slash::render::detail {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
Float3 mul(const Float3& a, const Float3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}
Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}
float length(const Float3& a) {
    return std::sqrt(dot(a, a));
}
Float3 normalize(const Float3& a) {
    const float size = length(a);
    return size > 0.0F ? scale(a, 1.0F / size) : a;
}
Float3 lerp(const Float3& a, const Float3& b, float t) {
    return add(a, scale(sub(b, a), t));
}
float max3(const Float3& a) {
    return std::max(a[0], std::max(a[1], a[2]));
}
float smoothstep(float from, float to, float x) {
    const float t = std::clamp((x - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

void for_each(std::size_t count, jobs::Pool* pool, const std::function<void(std::size_t)>& task) {
    if (pool != nullptr) {
        pool->parallel_for(count, task);
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
    }
}

// The six directions light is split over (aniso_dir in Godot's shaders).
constexpr std::array<Float3, 6> kAnisoDir{{{1.0F, 0.0F, 0.0F},
                                           {0.0F, 1.0F, 0.0F},
                                           {0.0F, 0.0F, 1.0F},
                                           {-1.0F, 0.0F, 0.0F},
                                           {0.0F, -1.0F, 0.0F},
                                           {0.0F, 0.0F, -1.0F}}};

// A cell's 26 neighbors in the order sdfgi_preprocess.glsl's MODE_STORE numbers them.
constexpr std::array<std::array<int, 3>, 26> neighbor_offsets() {
    std::array<std::array<int, 3>, 26> out{};
    std::size_t n = 0;
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            for (int k = -1; k <= 1; ++k) {
                if (i == 0 && j == 0 && k == 0) {
                    continue;
                }
                out.at(n++) = {i, j, k};
            }
        }
    }
    return out;
}
constexpr std::array<std::array<int, 3>, 26> kNeighbors = neighbor_offsets();

struct Grid {
    std::uint32_t size = 0;
    [[nodiscard]] std::size_t cells() const { return std::size_t{size} * size * size; }
    [[nodiscard]] std::size_t index(std::int64_t x, std::int64_t y, std::int64_t z) const {
        return static_cast<std::size_t>((((z * size) + y) * size) + x);
    }
    [[nodiscard]] bool inside(std::int64_t x, std::int64_t y, std::int64_t z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < size && y < size && z < size;
    }
};

// A texture of a cascade's cells sampled linearly at a point in cells (texel centers at half
// cells), clamped to its edges, as Godot's linear_sampler reads its 3D textures.
template <typename Texel, typename Read>
auto sample3(const Grid& grid, const Float3& p, const Read& read) {
    const auto max = static_cast<float>(grid.size - 1);
    const Float3 t{std::clamp(p[0] - 0.5F, 0.0F, max), std::clamp(p[1] - 0.5F, 0.0F, max),
                   std::clamp(p[2] - 0.5F, 0.0F, max)};
    const std::array<std::int64_t, 3> base{static_cast<std::int64_t>(std::floor(t[0])),
                                           static_cast<std::int64_t>(std::floor(t[1])),
                                           static_cast<std::int64_t>(std::floor(t[2]))};
    const Float3 f{t[0] - static_cast<float>(base[0]), t[1] - static_cast<float>(base[1]),
                   t[2] - static_cast<float>(base[2])};
    const auto at = [&](std::size_t axis, int step) {
        return std::min<std::int64_t>(base.at(axis) + step, grid.size - 1);
    };
    Texel sum{};
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1;
        const int dy = (c >> 1) & 1;
        const int dz = (c >> 2) & 1;
        const float w = (dx != 0 ? f[0] : 1.0F - f[0]) * (dy != 0 ? f[1] : 1.0F - f[1]) *
                        (dz != 0 ? f[2] : 1.0F - f[2]);
        if (w == 0.0F) {
            continue;
        }
        const Texel value = read(grid.index(at(0, dx), at(1, dy), at(2, dz)));
        for (std::size_t i = 0; i < sum.size(); ++i) {
            sum.at(i) += value.at(i) * w;
        }
    }
    return sum;
}

// The distance field's value at a point in cells: Godot's texture(...).r * 255.0.
float sdf_at(const Grid& grid, const SdfgiCascade& cascade, const Float3& p) {
    return sample3<std::array<float, 1>>(grid, p, [&](std::size_t i) {
        return std::array<float, 1>{static_cast<float>(cascade.sdf[i])};
    })[0];
}

// A 2D image (x fastest) sampled linearly at a point in texels (centers at half texels),
// clamped to its edges.
Float3 sample2(const std::vector<Float3>& image, std::uint32_t width, std::uint32_t height, float x,
               float y) {
    const float tx = std::clamp(x - 0.5F, 0.0F, static_cast<float>(width - 1));
    const float ty = std::clamp(y - 0.5F, 0.0F, static_cast<float>(height - 1));
    const auto x0 = static_cast<std::uint32_t>(std::floor(tx));
    const auto y0 = static_cast<std::uint32_t>(std::floor(ty));
    const std::uint32_t x1 = std::min(x0 + 1, width - 1);
    const std::uint32_t y1 = std::min(y0 + 1, height - 1);
    const float fx = tx - static_cast<float>(x0);
    const float fy = ty - static_cast<float>(y0);
    const auto texel = [&](std::uint32_t px, std::uint32_t py) {
        return image[(std::size_t{py} * width) + px];
    };
    return lerp(lerp(texel(x0, y0), texel(x1, y0), fx), lerp(texel(x0, y1), texel(x1, y1), fx), fy);
}

constexpr std::uint32_t kOctBorder = kSdfgiOctSize + 2;
constexpr std::uint32_t kProbeImageWidth = kOctBorder * kSdfgiProbeAxis * kSdfgiProbeAxis;
constexpr std::uint32_t kProbeImageHeight = kOctBorder * kSdfgiProbeAxis;
constexpr std::uint32_t kProbes = kSdfgiProbeAxis * kSdfgiProbeAxis * kSdfgiProbeAxis;

// Godot's octahedron_encode (to 0 to 1) and its inverse in sdfgi_integrate.glsl.
std::array<float, 2> octahedron_encode(Float3 n) {
    n = scale(n, 1.0F / (std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2])));
    std::array<float, 2> xy{n[0], n[1]};
    if (n[2] < 0.0F) {
        xy = {(1.0F - std::abs(n[1])) * (n[0] >= 0.0F ? 1.0F : -1.0F),
              (1.0F - std::abs(n[0])) * (n[1] >= 0.0F ? 1.0F : -1.0F)};
    }
    return {(xy[0] * 0.5F) + 0.5F, (xy[1] * 0.5F) + 0.5F};
}
Float3 octahedron_decode(std::array<float, 2> f) {
    f = {(f[0] * 2.0F) - 1.0F, (f[1] * 2.0F) - 1.0F};
    Float3 n{f[0], f[1], 1.0F - std::abs(f[0]) - std::abs(f[1])};
    const float t = std::clamp(-n[2], 0.0F, 1.0F);
    n[0] += n[0] >= 0.0F ? -t : t;
    n[1] += n[1] >= 0.0F ? -t : t;
    return normalize(n);
}

// The 16 spherical harmonics' basis along a direction, as Godot's SH_ACCUM.
std::array<float, kSdfgiShSize> sh_basis(const Float3& d) {
    const Float3 d2 = mul(d, d);
    return {0.282095F,
            0.488603F * d[1],
            0.488603F * d[2],
            0.488603F * d[0],
            1.092548F * d[0] * d[1],
            1.092548F * d[1] * d[2],
            0.315392F * ((3.0F * d2[2]) - 1.0F),
            1.092548F * d[0] * d[2],
            0.546274F * (d2[0] - d2[1]),
            0.590043F * d[1] * ((3.0F * d2[0]) - d2[1]),
            2.890611F * d[1] * d[0] * d[2],
            0.646360F * d[1] * (-1.0F + (5.0F * d2[2])),
            0.373176F * ((5.0F * d2[2] * d[2]) - (3.0F * d[2])),
            0.457045F * d[0] * (-1.0F + (5.0F * d2[2])),
            1.445305F * (d2[0] - d2[1]) * d[2],
            0.590043F * d[0] * (d2[0] - (3.0F * d2[1]))};
}

// Godot's hash3 and hashf3 (sdfgi_integrate.glsl), with the engine's sine.
std::array<std::uint32_t, 3> hash3(std::array<std::uint32_t, 3> x) {
    for (std::uint32_t& v : x) {
        v = ((v >> 16U) ^ v) * 0x45d9f3bU;
        v = ((v >> 16U) ^ v) * 0x45d9f3bU;
        v = (v >> 16U) ^ v;
    }
    return x;
}
float hashf3(const Float3& co) {
    const float s = slash::cos_sin(dot(co, {12.9898F, 78.233F, 137.13451F})).y * 43758.5453F;
    return s - std::floor(s);
}

// Godot's vogel_hemisphere.
Float3 vogel_hemisphere(std::uint32_t index, std::uint32_t count, float offset) {
    const float golden = kPi * (3.0F - std::sqrt(5.0F));
    const float r =
        std::sqrt(static_cast<float>(index) + 0.5F) / std::sqrt(static_cast<float>(count));
    const float theta = (static_cast<float>(index) * golden) + offset;
    const Vec2f yl = slash::cos_sin(r * kPi * 0.5F);
    const Vec2f cs = slash::cos_sin(theta);
    return {yl.y * cs.x, yl.y * cs.y, yl.x * ((index & 1U) != 0U ? 1.0F : -1.0F)};
}

// The quantization Godot's textures put light through: 8 bits for the aniso shares.
float unorm8(float v) {
    return std::round(std::clamp(v, 0.0F, 1.0F) * 255.0F) / 255.0F;
}

std::uint32_t probe_cells(std::uint32_t size) {
    return size / kSdfgiProbeDivisor;
}

// --- Voxelizing (render_region, _render_sdfgi) -------------------------------------------------

// How far a cell's box stops short of the next cell, so a triangle on the face between two cells
// (a floor at a whole number of cells) fills only the cell above it, as the GPU's rasterizer
// gives a pixel's edge to one side.
constexpr float kCellEdge = 1.0F / 1024.0F;

// Whether a triangle (in cells, relative to a cell's box's center) overlaps the cell (a box of
// half size 0.5 less half of kCellEdge), by the separating axis test (Akenine-Möller).
bool triangle_overlaps_cell(const std::array<Float3, 3>& v) {
    constexpr float h = 0.5F - (kCellEdge * 0.5F);
    const std::array<Float3, 3> e{sub(v[1], v[0]), sub(v[2], v[1]), sub(v[0], v[2])};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float lo = std::min({v[0].at(axis), v[1].at(axis), v[2].at(axis)});
        const float hi = std::max({v[0].at(axis), v[1].at(axis), v[2].at(axis)});
        if (lo > h || hi < -h) {
            return false;
        }
    }
    const Float3 n = cross(e[0], e[1]);
    const float r = h * (std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]));
    if (std::abs(dot(n, v[0])) > r) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            Float3 unit{};
            unit.at(axis) = 1.0F;
            const Float3 a = cross(unit, e.at(i));
            if (dot(a, a) == 0.0F) {
                continue;
            }
            const float p0 = dot(a, v[0]);
            const float p1 = dot(a, v[1]);
            const float p2 = dot(a, v[2]);
            const float radius = h * (std::abs(a[0]) + std::abs(a[1]) + std::abs(a[2]));
            if (std::min({p0, p1, p2}) > radius || std::max({p0, p1, p2}) < -radius) {
                return false;
            }
        }
    }
    return true;
}

// The point of a triangle nearest p, as the second and third corners' shares (Ericson's
// ClosestPtPointTriangle).
std::array<float, 2> nearest_on_triangle(const Float3& p, const Float3& a, const Float3& b,
                                         const Float3& c) {
    const Float3 ab = sub(b, a);
    const Float3 ac = sub(c, a);
    const Float3 ap = sub(p, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return {0.0F, 0.0F};
    }
    const Float3 bp = sub(p, b);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return {1.0F, 0.0F};
    }
    const float vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return {d1 / (d1 - d3), 0.0F};
    }
    const Float3 cp = sub(p, c);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return {0.0F, 1.0F};
    }
    const float vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return {0.0F, d2 / (d2 - d6)};
    }
    const float va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {1.0F - w, w};
    }
    const float denom = 1.0F / (va + vb + vc);
    return {vb * denom, vc * denom};
}

// Godot's 5-bit albedo channels (the R16 texture _render_sdfgi writes).
float albedo5(float v) {
    return std::round(std::clamp(v, 0.0F, 1.0F) * 31.0F) / 31.0F;
}

// One 1D pass of Felzenszwalb and Huttenlocher's squared distance transform over n values.
void distance_pass(std::span<double> f, std::vector<double>& out, std::vector<int>& v,
                   std::vector<double>& z) {
    const auto n = static_cast<int>(f.size());
    constexpr double kInf = std::numeric_limits<double>::infinity();
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[static_cast<std::size_t>(q)] == kInf) {
            continue;
        }
        while (k >= 0) {
            const int p = v[static_cast<std::size_t>(k)];
            const double s = ((f[static_cast<std::size_t>(q)] + (double(q) * q)) -
                              (f[static_cast<std::size_t>(p)] + (double(p) * p))) /
                             (2.0 * (q - p));
            if (s <= z[static_cast<std::size_t>(k)]) {
                --k;
                continue;
            }
            break;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] =
            k == 0 ? -kInf
                   : ((f[static_cast<std::size_t>(q)] + (double(q) * q)) -
                      (f[static_cast<std::size_t>(v[static_cast<std::size_t>(k - 1)])] +
                       (double(v[static_cast<std::size_t>(k - 1)]) *
                        v[static_cast<std::size_t>(k - 1)]))) /
                         (2.0 * (q - v[static_cast<std::size_t>(k - 1)]));
    }
    if (k < 0) {
        std::fill(out.begin(), out.begin() + n, kInf);
        return;
    }
    int j = 0;
    for (int q = 0; q < n; ++q) {
        while (j < k && z[static_cast<std::size_t>(j) + 1] < q) {
            ++j;
        }
        const int p = v[static_cast<std::size_t>(j)];
        out[static_cast<std::size_t>(q)] =
            (double(q - p) * (q - p)) + f[static_cast<std::size_t>(p)];
    }
}

// The squared distance from each cell to the nearest solid one: Godot's jump flood
// (sdfgi_preprocess.glsl's MODE_JUMPFLOOD passes) finds it nearly, this exactly.
std::vector<double> squared_distances(const Grid& grid, const std::vector<std::uint8_t>& solid,
                                      jobs::Pool* pool) {
    constexpr double kInf = std::numeric_limits<double>::infinity();
    const std::uint32_t n = grid.size;
    std::vector<double> d(grid.cells());
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = solid[i] != 0 ? 0.0 : kInf;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for_each(std::size_t{n} * n, pool, [&](std::size_t line) {
            std::vector<double> f(n);
            std::vector<double> out(n);
            std::vector<int> v(n);
            std::vector<double> z(n + 1);
            const auto a = static_cast<std::uint32_t>(line % n);
            const auto b = static_cast<std::uint32_t>(line / n);
            const auto at = [&](std::uint32_t t) {
                if (axis == 0) {
                    return grid.index(t, a, b);
                }
                return axis == 1 ? grid.index(a, t, b) : grid.index(a, b, t);
            };
            for (std::uint32_t t = 0; t < n; ++t) {
                f[t] = d[at(t)];
            }
            distance_pass(f, out, v, z);
            for (std::uint32_t t = 0; t < n; ++t) {
                d[at(t)] = out[t];
            }
        });
    }
    return d;
}

// Makes a cascade's cells and distance field again from the frame's triangles, as render_region
// renders the cascade's geometry into albedo, emission and facing textures and the preprocess
// passes make its distance field and solid cell list from them. Godot rasterizes the geometry
// from three sides and keeps what was drawn last in a cell; this averages what each triangle
// touching a cell has at the point nearest the cell's center, sampled as a ray meeting it would.
// With scrolled, the cascade moved by that many cells since its sums were made: the cells it
// still holds keep theirs, moved, and only those that came in are made, as Godot's scroll renders
// only the region that came in.
void voxelize(SdfgiCascade& cascade, std::uint32_t size, float y_mult, const PathScene& paths,
              const PathTextures& textures, jobs::Pool* pool,
              const std::optional<std::array<std::int32_t, 3>>& scrolled = std::nullopt) {
    const Grid grid{size};
    const auto whole = static_cast<std::int64_t>(size);
    // Whether the cell at (x, y, z) was in the cascade before it moved, and so keeps its sum.
    const bool scrolling = scrolled.has_value() && cascade.sums.size() == grid.cells();
    const auto kept = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
        if (!scrolling) {
            return false;
        }
        const std::int64_t ox = x + (*scrolled)[0];
        const std::int64_t oy = y + (*scrolled)[1];
        const std::int64_t oz = z + (*scrolled)[2];
        return ox >= 0 && oy >= 0 && oz >= 0 && ox < whole && oy < whole && oz < whole;
    };
    const Float3 offset = sdfgi_offset(cascade, size);
    const float to_cell = 1.0F / cascade.cell_size;
    // Each triangle's corners in cells, and the z slices it reaches.
    struct Placed {
        std::array<Float3, 3> corners{};
        std::int32_t z0 = 0;
        std::int32_t z1 = -1;
    };
    std::vector<Placed> placed(paths.triangles.size());
    for_each(paths.triangles.size(), pool, [&](std::size_t t) {
        const PathTriangle& triangle = paths.triangles[t];
        if (paths.draws[triangle.draw].shadows == ShadowCasting::ShadowsOnly) {
            return;
        }
        Placed& p = placed[t];
        for (std::size_t i = 0; i < 3; ++i) {
            Float3 world = paths.corners[triangle.corners.at(i)].world;
            world[1] *= y_mult;
            p.corners.at(i) = scale(sub(world, offset), to_cell);
        }
        const float lo = std::min({p.corners[0][2], p.corners[1][2], p.corners[2][2]});
        const float hi = std::max({p.corners[0][2], p.corners[1][2], p.corners[2][2]});
        p.z0 = std::max(0, static_cast<std::int32_t>(std::floor(lo - 0.5F)));
        p.z1 = std::min(static_cast<std::int32_t>(size) - 1,
                        static_cast<std::int32_t>(std::floor(hi + 0.5F)));
    });
    std::vector<std::vector<std::uint32_t>> slices(size);
    for (std::size_t t = 0; t < placed.size(); ++t) {
        for (std::int32_t z = placed[t].z0; z <= placed[t].z1; ++z) {
            slices[static_cast<std::size_t>(z)].push_back(static_cast<std::uint32_t>(t));
        }
    }
    std::vector<SdfgiCellSum> sums(grid.cells());
    if (scrolling) {
        for_each(size, pool, [&](std::size_t z) {
            const auto iz = static_cast<std::int64_t>(z);
            for (std::int64_t y = 0; y < whole; ++y) {
                for (std::int64_t x = 0; x < whole; ++x) {
                    if (kept(x, y, iz)) {
                        sums[grid.index(x, y, iz)] = cascade.sums[grid.index(
                            x + (*scrolled)[0], y + (*scrolled)[1], iz + (*scrolled)[2])];
                    }
                }
            }
        });
    }
    for_each(size, pool, [&](std::size_t z) {
        for (const std::uint32_t t : slices[z]) {
            const Placed& p = placed[t];
            // The sides its corners' normals face, whichever way its corners wind.
            const PathTriangle& triangle = paths.triangles[t];
            Float3 normal{};
            for (const std::uint32_t corner : triangle.corners) {
                normal = add(normal, paths.corners[corner].normal);
            }
            std::uint32_t facing = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                facing |= normal.at(k) > 0.0F ? 1U << k : 0U;
                facing |= normal.at(k) < 0.0F ? 1U << (k + 3) : 0U;
            }
            std::array<std::int32_t, 2> lo{};
            std::array<std::int32_t, 2> hi{};
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const float a =
                    std::min({p.corners[0].at(axis), p.corners[1].at(axis), p.corners[2].at(axis)});
                const float b =
                    std::max({p.corners[0].at(axis), p.corners[1].at(axis), p.corners[2].at(axis)});
                lo.at(axis) = std::max(0, static_cast<std::int32_t>(std::floor(a - 0.5F)));
                hi.at(axis) = std::min(static_cast<std::int32_t>(size) - 1,
                                       static_cast<std::int32_t>(std::floor(b + 0.5F)));
            }
            for (std::int32_t y = lo[1]; y <= hi[1]; ++y) {
                for (std::int32_t x = lo[0]; x <= hi[0]; ++x) {
                    if (kept(x, y, static_cast<std::int64_t>(z))) {
                        continue;
                    }
                    const Float3 center{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                                        static_cast<float>(z) + 0.5F};
                    // The cell's box: from its low corner to kCellEdge short of the next cell.
                    const Float3 box =
                        sub(center, {kCellEdge * 0.5F, kCellEdge * 0.5F, kCellEdge * 0.5F});
                    const std::array<Float3, 3> v{sub(p.corners[0], box), sub(p.corners[1], box),
                                                  sub(p.corners[2], box)};
                    if (!triangle_overlaps_cell(v)) {
                        continue;
                    }
                    const std::array<float, 2> uv =
                        nearest_on_triangle(center, p.corners[0], p.corners[1], p.corners[2]);
                    const std::optional<PathSurface> surface =
                        path_surface_at(paths, textures, t, uv[0], uv[1], true);
                    if (!surface) {
                        continue;
                    }
                    SdfgiCellSum& sum = sums[grid.index(x, y, static_cast<std::int64_t>(z))];
                    sum.albedo = add(sum.albedo, surface->unshaded ? Float3{} : surface->base);
                    sum.emission =
                        add(sum.emission, surface->unshaded ? surface->base : surface->emitted);
                    ++sum.count;
                    sum.facing |= facing;
                }
            }
        }
    });
    std::vector<std::uint8_t> solid(grid.cells(), 0);
    for (std::size_t i = 0; i < sums.size(); ++i) {
        solid[i] = sums[i].count > 0 ? 1 : 0;
    }
    // The distance field (MODE_STORE): 0 in a solid cell, else 1 + the distance, as 8 bits.
    const std::vector<double> d2 = squared_distances(grid, solid, pool);
    cascade.sdf.assign(grid.cells(), 0);
    for_each(grid.cells(), pool, [&](std::size_t i) {
        if (solid[i] != 0) {
            cascade.sdf[i] = 0;
            return;
        }
        const double d = 1.0 + std::sqrt(d2[i]);
        cascade.sdf[i] = static_cast<std::uint8_t>(std::min(255.0, std::round(d)));
    });
    // The solid cells, in order.
    cascade.cells.clear();
    std::vector<std::uint32_t> cell_of(grid.cells(), std::numeric_limits<std::uint32_t>::max());
    for (std::size_t i = 0; i < sums.size(); ++i) {
        if (sums[i].count == 0) {
            continue;
        }
        const float inv = 1.0F / static_cast<float>(sums[i].count);
        const Float3 albedo = scale(sums[i].albedo, inv);
        cell_of[i] = static_cast<std::uint32_t>(cascade.cells.size());
        cascade.cells.push_back(
            {.index = static_cast<std::uint32_t>(i),
             .albedo = {albedo5(albedo[0]), albedo5(albedo[1]), albedo5(albedo[2])},
             .emission = scale(sums[i].emission, inv),
             .facing = sums[i].facing,
             .neighbors = 0});
    }
    // The empty cells next to solid ones take the light of the nearest (the jump flood's
    // positions pointing at a solid cell, which MODE_STORE turns into neighbor bits): the
    // first nearest in MODE_STORE's order.
    for (std::size_t i = 0; i < grid.cells(); ++i) {
        if (solid[i] != 0 || d2[i] > 3.0) {
            continue;
        }
        const auto x = static_cast<std::int64_t>(i % size);
        const auto y = static_cast<std::int64_t>((i / size) % size);
        const auto z = static_cast<std::int64_t>(i / (std::size_t{size} * size));
        int best = -1;
        int best_length = 4;
        for (std::size_t n = 0; n < kNeighbors.size(); ++n) {
            const std::array<int, 3>& o = kNeighbors.at(n);
            const std::int64_t nx = x + o[0];
            const std::int64_t ny = y + o[1];
            const std::int64_t nz = z + o[2];
            if (!grid.inside(nx, ny, nz) || solid[grid.index(nx, ny, nz)] == 0) {
                continue;
            }
            const int l = (o[0] * o[0]) + (o[1] * o[1]) + (o[2] * o[2]);
            if (l < best_length) {
                best_length = l;
                best = static_cast<int>(n);
            }
        }
        if (best < 0) {
            continue;
        }
        const std::array<int, 3>& o = kNeighbors.at(static_cast<std::size_t>(best));
        const std::uint32_t c = cell_of[grid.index(x + o[0], y + o[1], z + o[2])];
        // The solid cell's bit for the way back to this one.
        for (std::size_t n = 0; n < kNeighbors.size(); ++n) {
            const std::array<int, 3>& back = kNeighbors.at(n);
            if (back[0] == -o[0] && back[1] == -o[1] && back[2] == -o[2]) {
                cascade.cells[c].neighbors |= 1U << n;
            }
        }
    }
    cascade.sums = std::move(sums);
    cascade.light.assign(grid.cells(), Float3{});
    cascade.aniso.assign(grid.cells(), std::array<float, 6>{});
    cascade.all_lights_dirty = true;
}

// --- Light (update_light, sdfgi_direct_light.glsl's MODE_PROCESS_DYNAMIC) -----------------------

struct SdfgiLight {
    LightType type = LightType::Directional;
    Float3 color{};
    Float3 direction{}; // the way it shines (y scaled for directional lights)
    Float3 position{};  // y scaled
    float attenuation = 1.0F;
    float radius = 1.0F;
    float cos_spot_angle = 0.0F;
    float inv_spot_attenuation = 1.0F;
};

std::vector<SdfgiLight> sdfgi_lights(const Scene3D& scene, float y_mult) {
    std::vector<SdfgiLight> out;
    for (const Light3D& light : scene.lights) {
        if (light.indirect_energy == 0.0F) {
            continue;
        }
        const float energy = light.energy * light.indirect_energy;
        SdfgiLight l;
        l.type = light.type;
        l.color = {srgb_to_linear_exact(light.color[0]) * energy,
                   srgb_to_linear_exact(light.color[1]) * energy,
                   srgb_to_linear_exact(light.color[2]) * energy};
        l.direction = normalize({light.direction[0], light.direction[1], light.direction[2]});
        if (light.type == LightType::Directional) {
            l.direction[1] *= y_mult;
            l.direction = normalize(l.direction);
        }
        l.position = {light.position[0], light.position[1] * y_mult, light.position[2]};
        l.attenuation = light.attenuation;
        l.radius = light.range;
        l.cos_spot_angle = slash::cos_sin(light.spot_angle * (kPi / 180.0F)).x;
        l.inv_spot_attenuation = 1.0F / light.spot_attenuation;
        out.push_back(l);
    }
    return out;
}

float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = std::max(1.0F - nd, 0.0F);
    nd *= nd;
    return nd * std::pow(std::max(distance, 0.0001F), -decay);
}

struct Cascades {
    const SdfgiState& state;
    Grid grid;
    std::vector<Float3> offsets;
    std::vector<float> to_cell;

    Cascades(const SdfgiState& s, std::uint32_t size) : state(s), grid{size} {
        for (const SdfgiCascade& c : s.cascades) {
            offsets.push_back(sdfgi_offset(c, size));
            to_cell.push_back(1.0F / c.cell_size);
        }
    }
};

// A ray through the cascades' distance fields from cascade first on, as the direct light and
// integrate passes march them: where it met something (in cells of that cascade) and in which
// cascade, or nothing. With limit, it gives up past that distance in the world (lights); least
// is how near a distance counts as meeting; occlusion gets the least distance it passed.
struct March {
    bool hit = false;
    std::size_t cascade = 0;
    Float3 at{};
};
March march(const Cascades& cs, std::size_t first, Float3 ray_pos, const Float3& ray_dir,
            float limit, float least) {
    const auto size = static_cast<float>(cs.grid.size);
    const Float3 inv{1.0F / ray_dir[0], 1.0F / ray_dir[1], 1.0F / ray_dir[2]};
    float light_distance = limit;
    for (std::size_t j = first; j < cs.state.cascades.size(); ++j) {
        Float3 pos = scale(sub(ray_pos, cs.offsets[j]), cs.to_cell[j]);
        const float local_distance = light_distance * cs.to_cell[j];
        if (pos[0] < 0.0F || pos[1] < 0.0F || pos[2] < 0.0F || pos[0] >= size || pos[1] >= size ||
            pos[2] >= size) {
            continue;
        }
        float max_advance = std::numeric_limits<float>::max();
        for (std::size_t a = 0; a < 3; ++a) {
            const float t0 = -pos.at(a) * inv.at(a);
            const float t1 = (size - pos.at(a)) * inv.at(a);
            max_advance = std::min(max_advance, std::max(t0, t1));
        }
        max_advance = std::min(local_distance, max_advance);
        float advance = 0.0F;
        while (advance < max_advance) {
            const Float3 p = add(pos, scale(ray_dir, advance));
            const float distance = sdf_at(cs.grid, cs.state.cascades[j], p) - 1.0F;
            if (distance < least) {
                return {.hit = true, .cascade = j, .at = p};
            }
            advance += distance;
        }
        if (advance >= local_distance) {
            return {};
        }
        pos = add(pos, scale(ray_dir, max_advance));
        ray_pos = add(scale(pos, 1.0F / cs.to_cell[j]), cs.offsets[j]);
        light_distance -= max_advance / cs.to_cell[j];
    }
    return {};
}

// The probes' irradiance at a point of a cascade's cells along a direction, as the bounce
// feedback reads lightprobe_texture: probe_base the probe below it, offset which of the 8 around.
Float3 probe_texel(const SdfgiCascade& cascade, const std::vector<Float3>& image,
                   const std::array<std::int32_t, 3>& probe, const Float3& n) {
    const std::array<float, 2> oct = octahedron_encode(n);
    const float x =
        static_cast<float>((probe[0] + (probe[2] * static_cast<std::int32_t>(kSdfgiProbeAxis))) *
                               static_cast<std::int32_t>(kOctBorder) +
                           1) +
        (oct[0] * static_cast<float>(kSdfgiOctSize));
    const float y = static_cast<float>((probe[1] * static_cast<std::int32_t>(kOctBorder)) + 1) +
                    (oct[1] * static_cast<float>(kSdfgiOctSize));
    (void)cascade;
    return sample2(image, kProbeImageWidth, kProbeImageHeight, x, y);
}

void light_cells(SdfgiState& state, std::size_t index, const std::vector<SdfgiLight>& lights,
                 jobs::Pool* pool) {
    SdfgiCascade& cascade = state.cascades[index];
    const std::uint32_t size = state.settings.cascade_size;
    const Cascades cs(state, size);
    const float feedback_amount = state.settings.bounce_feedback;
    const std::uint32_t increment =
        cascade.all_lights_dirty ? 1U : std::max(1U, state.settings.frames_to_update_light);
    const std::uint32_t process_offset = state.frame % increment;
    cascade.all_lights_dirty = false;
    const Float3 offset = cs.offsets[index];
    const float cell_size = cascade.cell_size;
    const float probe_scale = static_cast<float>(kSdfgiProbeAxis - 1) / static_cast<float>(size);
    struct Lit {
        Float3 total{};
        std::array<float, 6> aniso{};
    };
    std::vector<Lit> lit(cascade.cells.size());
    for_each(cascade.cells.size(), pool, [&](std::size_t c) {
        if ((c % increment) != process_offset) {
            return;
        }
        const SdfgiCell& cell = cascade.cells[c];
        const std::array<std::int64_t, 3> cpos{
            static_cast<std::int64_t>(cell.index % size),
            static_cast<std::int64_t>((cell.index / size) % size),
            static_cast<std::int64_t>(cell.index / (std::size_t{size} * size))};
        const Float3 cell_center{static_cast<float>(cpos[0]) + 0.5F,
                                 static_cast<float>(cpos[1]) + 0.5F,
                                 static_cast<float>(cpos[2]) + 0.5F};
        const Float3 position = add(scale(cell_center, cell_size), offset);
        std::array<Float3, 6> accum{};
        // Bounce feedback: the probes' light around the cell, toward each side it faces.
        if (feedback_amount > 0.001F) {
            const Float3 feedback = feedback_amount < 1.0F ? scale(cell.albedo, feedback_amount)
                                                           : lerp(cell.albedo, {1.0F, 1.0F, 1.0F},
                                                                  feedback_amount - 1.0F);
            const Float3 pos = scale(cell_center, probe_scale);
            const std::array<std::int32_t, 3> base{static_cast<std::int32_t>(pos[0]),
                                                   static_cast<std::int32_t>(pos[1]),
                                                   static_cast<std::int32_t>(pos[2])};
            std::array<float, 6> weights{};
            for (std::uint32_t j = 0; j < 8; ++j) {
                const std::array<std::int32_t, 3> probe{
                    base[0] + static_cast<std::int32_t>(j & 1U),
                    base[1] + static_cast<std::int32_t>((j >> 1U) & 1U),
                    base[2] + static_cast<std::int32_t>((j >> 2U) & 1U)};
                const Float3 probe_to_pos =
                    sub(pos, {static_cast<float>(probe[0]), static_cast<float>(probe[1]),
                              static_cast<float>(probe[2])});
                const Float3 probe_dir = normalize(scale(probe_to_pos, -1.0F));
                const Float3 trilinear{1.0F - std::abs(probe_to_pos[0]),
                                       1.0F - std::abs(probe_to_pos[1]),
                                       1.0F - std::abs(probe_to_pos[2])};
                for (std::size_t k = 0; k < 6; ++k) {
                    if ((cell.facing & (1U << k)) == 0) {
                        continue;
                    }
                    const float weight = trilinear[0] * trilinear[1] * trilinear[2] *
                                         std::max(0.0F, dot(kAnisoDir.at(k), probe_dir));
                    if (weight > 0.0F) {
                        accum.at(k) = add(
                            accum.at(k),
                            scale(probe_texel(cascade, cascade.irradiance, probe, kAnisoDir.at(k)),
                                  weight));
                        weights.at(k) += weight;
                    }
                }
            }
            for (std::size_t k = 0; k < 6; ++k) {
                if (weights.at(k) > 0.0F) {
                    accum.at(k) = mul(scale(accum.at(k), 1.0F / weights.at(k)), feedback);
                }
            }
        }
        // What the cell gives off (Godot's static light, made by MODE_PROCESS_STATIC from
        // emission), shared evenly over the sides it faces.
        {
            std::uint32_t sides = 0;
            for (std::size_t k = 0; k < 6; ++k) {
                sides += (cell.facing >> k) & 1U;
            }
            if (sides > 0) {
                const Float3 share = scale(cell.emission, 1.0F / static_cast<float>(sides));
                for (std::size_t k = 0; k < 6; ++k) {
                    if ((cell.facing & (1U << k)) != 0) {
                        accum.at(k) = add(accum.at(k), share);
                    }
                }
            }
        }
        // Each light, traced through the distance fields.
        for (const SdfgiLight& light : lights) {
            float attenuation = 1.0F;
            Float3 direction{};
            float light_distance = 1e20F;
            if (light.type == LightType::Directional) {
                direction = scale(light.direction, -1.0F);
            } else {
                const Float3 rel = sub(light.position, position);
                direction = normalize(rel);
                light_distance = length(rel);
                attenuation =
                    omni_attenuation(light_distance, 1.0F / light.radius, light.attenuation);
                if (light.type == LightType::Spot) {
                    const float cos_angle = dot(scale(direction, -1.0F), light.direction);
                    if (cos_angle < light.cos_spot_angle) {
                        continue;
                    }
                    const float scos = std::max(cos_angle, light.cos_spot_angle);
                    const float rim =
                        std::max(0.0001F, (1.0F - scos) / (1.0F - light.cos_spot_angle));
                    attenuation *= 1.0F - std::pow(rim, light.inv_spot_attenuation);
                }
            }
            if (attenuation < 0.001F) {
                continue;
            }
            Float3 ray_pos = position;
            for (std::size_t a = 0; a < 3; ++a) {
                float sign = 0.0F;
                if (direction.at(a) > 0.0F) {
                    sign = 1.0F;
                } else if (direction.at(a) < 0.0F) {
                    sign = -1.0F;
                }
                ray_pos.at(a) += sign * cell_size * 0.48F;
            }
            ray_pos = add(ray_pos, scale(direction, 0.4F * cell_size));
            if (march(cs, index, ray_pos, direction, light_distance, 0.001F).hit) {
                continue;
            }
            const Float3 l = scale(mul(cell.albedo, light.color), attenuation);
            for (std::size_t k = 0; k < 6; ++k) {
                if ((cell.facing & (1U << k)) != 0) {
                    accum.at(k) =
                        add(accum.at(k), scale(l, std::max(0.0F, dot(kAnisoDir.at(k), direction))));
                }
            }
        }
        Float3 total{};
        std::array<float, 6> lumas{};
        for (std::size_t k = 0; k < 6; ++k) {
            total = add(total, accum.at(k));
            lumas.at(k) = max3(accum.at(k));
        }
        const float luma_total = max3(total);
        Lit& out = lit[c];
        out.total = sdfgi_rgbe_decode(sdfgi_rgbe_encode(total));
        for (std::size_t k = 0; k < 6; ++k) {
            out.aniso.at(k) = luma_total > 0.0F ? unorm8(lumas.at(k) / luma_total) : 0.0F;
        }
    });
    // Stored, with the empty neighbors that take each cell's light.
    for (std::size_t c = 0; c < cascade.cells.size(); ++c) {
        if ((c % increment) != process_offset) {
            continue;
        }
        const SdfgiCell& cell = cascade.cells[c];
        cascade.light[cell.index] = lit[c].total;
        cascade.aniso[cell.index] = lit[c].aniso;
        const auto x = static_cast<std::int64_t>(cell.index % size);
        const auto y = static_cast<std::int64_t>((cell.index / size) % size);
        const auto z = static_cast<std::int64_t>(cell.index / (std::size_t{size} * size));
        for (std::size_t n = 0; n < kNeighbors.size(); ++n) {
            if ((cell.neighbors & (1U << n)) == 0) {
                continue;
            }
            const std::array<int, 3>& o = kNeighbors.at(n);
            const std::size_t at = cs.grid.index(x + o[0], y + o[1], z + o[2]);
            cascade.light[at] = lit[c].total;
            cascade.aniso[at] = lit[c].aniso;
        }
    }
}

// --- Probes (update_probes and store_probes, sdfgi_integrate.glsl) ------------------------------

std::size_t probe_index(const std::array<std::int32_t, 3>& p) {
    const auto axis = std::size_t{kSdfgiProbeAxis};
    return (((static_cast<std::size_t>(p[2]) * axis) + static_cast<std::size_t>(p[1])) * axis) +
           static_cast<std::size_t>(p[0]);
}

void integrate(SdfgiState& state, const Scene3D& scene, const PathScene& paths,
               const PathTextures& textures, jobs::Pool* pool) {
    const Sdfgi3D& settings = state.settings;
    const std::uint32_t size = settings.cascade_size;
    const float y_mult = sdfgi_y_mult(settings);
    const Cascades cs(state, size);
    const std::uint32_t history_size = std::max(1U, settings.frames_to_converge);
    const std::uint32_t history_index = state.render_pass % history_size;
    const std::uint32_t ray_count = std::max(1U, settings.ray_count);
    const std::uint32_t ray_total = history_size * ray_count;
    const std::size_t sh_count = std::size_t{kProbes} * kSdfgiShSize;
    (void)scene;
    for (std::size_t c = 0; c < state.cascades.size(); ++c) {
        SdfgiCascade& cascade = state.cascades[c];
        const float probe_cell_size = static_cast<float>(probe_cells(size)) * cascade.cell_size;
        const auto divisor = static_cast<std::int32_t>(probe_cells(size));
        const std::array<std::int32_t, 3> world_offset{cascade.position[0] / divisor,
                                                       cascade.position[1] / divisor,
                                                       cascade.position[2] / divisor};
        for_each(kProbes, pool, [&](std::size_t p) {
            const std::array<std::int32_t, 3> probe{
                static_cast<std::int32_t>(p % kSdfgiProbeAxis),
                static_cast<std::int32_t>((p / kSdfgiProbeAxis) % kSdfgiProbeAxis),
                static_cast<std::int32_t>(p / (std::size_t{kSdfgiProbeAxis} * kSdfgiProbeAxis))};
            const Float3 probe_pos = add(
                cs.offsets[c], scale({static_cast<float>(probe[0]), static_cast<float>(probe[1]),
                                      static_cast<float>(probe[2])},
                                     probe_cell_size));
            const std::array<std::uint32_t, 3> h3 =
                hash3({static_cast<std::uint32_t>(world_offset[0] + probe[0]),
                       static_cast<std::uint32_t>(world_offset[1] + probe[1]),
                       static_cast<std::uint32_t>(world_offset[2] + probe[2])});
            const float offset =
                hashf3({static_cast<float>(h3[0] & 0xFFFFFU), static_cast<float>(h3[1] & 0xFFFFFU),
                        static_cast<float>(h3[2] & 0xFFFFFU)});
            std::array<Float3, kSdfgiShSize> sh{};
            for (std::uint32_t i = 0; i < ray_count; ++i) {
                Float3 ray_dir =
                    vogel_hemisphere(history_index + (i * history_size), ray_total, offset);
                ray_dir[1] *= y_mult;
                ray_dir = normalize(ray_dir);
                const Float3 abs_dir{std::abs(ray_dir[0]), std::abs(ray_dir[1]),
                                     std::abs(ray_dir[2])};
                const Float3 ray_pos =
                    add(probe_pos, scale(ray_dir, (1.0F / max3(abs_dir)) * settings.probe_bias /
                                                      cs.to_cell[c]));
                const March met =
                    march(cs, c, ray_pos, ray_dir, std::numeric_limits<float>::max(), 0.05F);
                Float3 light{};
                if (met.hit) {
                    const SdfgiCascade& hit = state.cascades[met.cascade];
                    const float eps = 0.001F * static_cast<float>(size);
                    const auto gradient = [&](std::size_t axis) {
                        Float3 a = met.at;
                        Float3 b = met.at;
                        a.at(axis) += eps;
                        b.at(axis) -= eps;
                        return sdf_at(cs.grid, hit, a) - sdf_at(cs.grid, hit, b);
                    };
                    const Float3 n = normalize({gradient(0), gradient(1), gradient(2)});
                    const Float3 hit_light = sample3<Float3>(
                        cs.grid, met.at, [&](std::size_t at) { return hit.light[at]; });
                    const std::array<float, 6> aniso = sample3<std::array<float, 6>>(
                        cs.grid, met.at, [&](std::size_t at) { return hit.aniso[at]; });
                    float share = 0.0F;
                    for (std::size_t k = 0; k < 3; ++k) {
                        share += std::max(0.0F, n.at(k) * aniso.at(k));
                        share += std::max(0.0F, -n.at(k) * aniso.at(k + 3));
                    }
                    light = scale(hit_light, share);
                } else if (settings.read_sky_light) {
                    // The sky's light, as meshes' ambient light takes it, where Godot reads
                    // the background's color or the sky's irradiance.
                    light = path_environment(paths, textures, ray_dir);
                }
                const std::array<float, kSdfgiShSize> basis = sh_basis(ray_dir);
                for (std::size_t s = 0; s < kSdfgiShSize; ++s) {
                    sh.at(s) = add(sh.at(s), scale(light, basis.at(s)));
                }
            }
            for (std::size_t s = 0; s < kSdfgiShSize; ++s) {
                const std::size_t at = (p * kSdfgiShSize) + s;
                std::array<std::int32_t, 4> value{};
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    const float v =
                        (ch < 3 ? sh.at(s).at(ch) : 1.0F) * 4.0F / static_cast<float>(ray_count);
                    const float scaled = v * static_cast<float>(1 << kSdfgiHistoryBits);
                    value.at(ch) = std::clamp(static_cast<std::int32_t>(scaled), -32768, 32767);
                }
                std::array<std::int32_t, 4>& prev =
                    cascade.history[(std::size_t{history_index} * sh_count) + at];
                std::array<std::int32_t, 4>& average = cascade.average[at];
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    average.at(ch) += value.at(ch) - prev.at(ch);
                }
                prev = value;
            }
        });
        // Store (MODE_STORE): each probe's octahedral irradiance and radiance, with borders.
        for_each(kProbes, pool, [&](std::size_t p) {
            const std::array<std::int32_t, 3> probe{
                static_cast<std::int32_t>(p % kSdfgiProbeAxis),
                static_cast<std::int32_t>((p / kSdfgiProbeAxis) % kSdfgiProbeAxis),
                static_cast<std::int32_t>(p / (std::size_t{kSdfgiProbeAxis} * kSdfgiProbeAxis))};
            const std::int32_t ox =
                ((probe[0] + (probe[2] * static_cast<std::int32_t>(kSdfgiProbeAxis))) *
                 static_cast<std::int32_t>(kOctBorder)) +
                1;
            const std::int32_t oy = (probe[1] * static_cast<std::int32_t>(kOctBorder)) + 1;
            constexpr std::array<float, kSdfgiShSize> l_mult{
                1.0F,  2.0F / 3.0F, 2.0F / 3.0F, 2.0F / 3.0F, 0.25F, 0.25F, 0.25F, 0.25F,
                0.25F, 0.0F,        0.0F,        0.0F,        0.0F,  0.0F,  0.0F,  0.0F};
            const auto put = [&](std::int32_t x, std::int32_t y, const Float3& irr,
                                 const Float3& rad) {
                const std::size_t at =
                    (static_cast<std::size_t>(y) * kProbeImageWidth) + static_cast<std::size_t>(x);
                cascade.irradiance[at] = irr;
                cascade.radiance[at] = rad;
            };
            constexpr auto kO = static_cast<std::int32_t>(kSdfgiOctSize);
            for (std::int32_t ly = 0; ly < kO; ++ly) {
                for (std::int32_t lx = 0; lx < kO; ++lx) {
                    const Float3 normal =
                        octahedron_decode({static_cast<float>(lx) / static_cast<float>(kO),
                                           static_cast<float>(ly) / static_cast<float>(kO)});
                    const std::array<float, kSdfgiShSize> c_basis = sh_basis(normal);
                    Float3 irradiance{};
                    Float3 radiance{};
                    for (std::size_t s = 0; s < kSdfgiShSize; ++s) {
                        const std::array<std::int32_t, 4>& average =
                            cascade.average[(p * kSdfgiShSize) + s];
                        const float k = 1.0F / static_cast<float>(history_size) /
                                        static_cast<float>(1 << kSdfgiHistoryBits);
                        const Float3 m = scale({static_cast<float>(average[0]) * k,
                                                static_cast<float>(average[1]) * k,
                                                static_cast<float>(average[2]) * k},
                                               c_basis.at(s) * 4.0F);
                        irradiance = add(irradiance, scale(m, l_mult.at(s)));
                        radiance = add(radiance, m);
                    }
                    irradiance = sdfgi_rgbe_decode(sdfgi_rgbe_encode(irradiance));
                    radiance = sdfgi_rgbe_decode(sdfgi_rgbe_encode(radiance));
                    // The texel, and the border texels Godot copies it to.
                    std::array<std::array<std::int32_t, 2>, 4> to{};
                    std::size_t count = 1;
                    to[0] = {lx, ly};
                    if (lx == 0 && ly == 0) {
                        to[1] = {kO - 1, -1};
                        to[2] = {-1, kO - 1};
                        to[3] = {kO, kO};
                        count = 4;
                    } else if (lx == kO - 1 && ly == 0) {
                        to[1] = {0, -1};
                        to[2] = {kO, kO - 1};
                        to[3] = {-1, kO};
                        count = 4;
                    } else if (lx == 0 && ly == kO - 1) {
                        to[1] = {-1, 0};
                        to[2] = {kO - 1, kO};
                        to[3] = {kO, -1};
                        count = 4;
                    } else if (lx == kO - 1 && ly == kO - 1) {
                        to[1] = {0, kO};
                        to[2] = {kO, 0};
                        to[3] = {-1, -1};
                        count = 4;
                    } else if (ly == 0) {
                        to[1] = {kO - lx - 1, ly - 1};
                        count = 2;
                    } else if (lx == 0) {
                        to[1] = {lx - 1, kO - ly - 1};
                        count = 2;
                    } else if (ly == kO - 1) {
                        to[1] = {kO - lx - 1, ly + 1};
                        count = 2;
                    } else if (lx == kO - 1) {
                        to[1] = {lx + 1, kO - ly - 1};
                        count = 2;
                    }
                    for (std::size_t i = 0; i < count; ++i) {
                        put(ox + to.at(i)[0], oy + to.at(i)[1], irradiance, radiance);
                    }
                }
            }
        });
    }
    ++state.render_pass;
}

// Makes a cascade's probe data again: cleared, as Godot's create clears its textures.
void clear_probes(SdfgiCascade& cascade, std::uint32_t history_size) {
    const std::size_t sh_count = std::size_t{kProbes} * kSdfgiShSize;
    cascade.history.assign(sh_count * history_size, {});
    cascade.average.assign(sh_count, {});
    cascade.irradiance.assign(std::size_t{kProbeImageWidth} * kProbeImageHeight, Float3{});
    cascade.radiance.assign(std::size_t{kProbeImageWidth} * kProbeImageHeight, Float3{});
}

// Moves a cascade's probes' history and averages by a whole number of probes, as
// sdfgi_integrate.glsl's MODE_SCROLL does when the cascade moves: probes that come in start
// cleared.
void scroll_probes(SdfgiCascade& cascade, const std::array<std::int32_t, 3>& probes,
                   std::uint32_t history_size) {
    const std::size_t sh_count = std::size_t{kProbes} * kSdfgiShSize;
    std::vector<std::array<std::int32_t, 4>> history(cascade.history.size());
    std::vector<std::array<std::int32_t, 4>> average(cascade.average.size());
    constexpr auto kA = static_cast<std::int32_t>(kSdfgiProbeAxis);
    for (std::int32_t z = 0; z < kA; ++z) {
        for (std::int32_t y = 0; y < kA; ++y) {
            for (std::int32_t x = 0; x < kA; ++x) {
                const std::array<std::int32_t, 3> from{x + probes[0], y + probes[1], z + probes[2]};
                if (from[0] < 0 || from[1] < 0 || from[2] < 0 || from[0] >= kA || from[1] >= kA ||
                    from[2] >= kA) {
                    continue;
                }
                const std::size_t to = probe_index({x, y, z});
                const std::size_t src = probe_index(from);
                for (std::size_t s = 0; s < kSdfgiShSize; ++s) {
                    average[(to * kSdfgiShSize) + s] = cascade.average[(src * kSdfgiShSize) + s];
                    for (std::size_t h = 0; h < history_size; ++h) {
                        history[(h * sh_count) + (to * kSdfgiShSize) + s] =
                            cascade.history[(h * sh_count) + (src * kSdfgiShSize) + s];
                    }
                }
            }
        }
    }
    cascade.history = std::move(history);
    cascade.average = std::move(average);
}

} // namespace

float sdfgi_y_mult(const Sdfgi3D& settings) {
    return settings.y_scale > 0.0F ? settings.y_scale : 1.0F;
}

Float3 sdfgi_offset(const SdfgiCascade& cascade, std::uint32_t size) {
    const auto half = static_cast<std::int32_t>(size >> 1U);
    return {static_cast<float>(cascade.position[0] - half) * cascade.cell_size,
            static_cast<float>(cascade.position[1] - half) * cascade.cell_size,
            static_cast<float>(cascade.position[2] - half) * cascade.cell_size};
}

std::uint32_t sdfgi_rgbe_encode(const Float3& color) {
    constexpr float kPow2to9 = 512.0F;
    constexpr float kB = 15.0F;
    constexpr float kN = 9.0F;
    const float r = std::clamp(color[0], 0.0F, 65408.0F);
    const float g = std::clamp(color[1], 0.0F, 65408.0F);
    const float b = std::clamp(color[2], 0.0F, 65408.0F);
    const float c_max = std::max(r, std::max(g, b));
    // floor(log2(c_max)), exactly.
    float log2_floor = -kB - 1.0F;
    if (c_max > 0.0F) {
        int e = 0;
        (void)std::frexp(c_max, &e);
        log2_floor = std::max(-kB - 1.0F, static_cast<float>(e - 1));
    }
    const float expp = log2_floor + 1.0F + kB;
    const float s_max =
        std::floor((c_max / std::ldexp(1.0F, static_cast<int>(expp - kB - kN))) + 0.5F);
    float exps = expp + 1.0F;
    if (0.0F <= s_max && s_max < kPow2to9) {
        exps = expp;
    }
    const float unit = std::ldexp(1.0F, static_cast<int>(exps - kB - kN));
    const auto q = [&](float v) {
        return static_cast<std::uint32_t>(std::floor((v / unit) + 0.5F)) & 0x1FFU;
    };
    return q(r) | (q(g) << 9U) | (q(b) << 18U) |
           ((static_cast<std::uint32_t>(exps) & 0x1FU) << 27U);
}

Float3 sdfgi_rgbe_decode(std::uint32_t rgbe) {
    const int e = static_cast<int>((rgbe >> 27U) & 0x1FU);
    const float unit = std::ldexp(1.0F, e - 15 - 9);
    return {static_cast<float>(rgbe & 0x1FFU) * unit,
            static_cast<float>((rgbe >> 9U) & 0x1FFU) * unit,
            static_cast<float>((rgbe >> 18U) & 0x1FFU) * unit};
}

namespace {

// A hash of what the cascades' cells are made of: the path scene's triangles, their corners and
// their draws' surfaces, but not the camera or the lights, which path_scene_key holds too.
std::uint64_t geometry_key(const PathScene& paths) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    const auto add_bytes = [&](const auto& value) {
        std::array<unsigned char, sizeof(value)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        for (const unsigned char byte : bytes) {
            hash = (hash ^ byte) * 0x100000001B3ULL;
        }
    };
    for (const MeshCorner& corner : paths.corners) {
        add_bytes(corner.world);
        add_bytes(corner.normal);
        add_bytes(corner.uv);
        add_bytes(corner.color);
    }
    for (const PathTriangle& triangle : paths.triangles) {
        add_bytes(triangle.corners);
        add_bytes(triangle.draw);
    }
    for (const PathDraw& draw : paths.draws) {
        add_bytes(draw.surface);
        add_bytes(draw.cull);
    }
    return hash;
}

} // namespace

void update_sdfgi(SdfgiState& state, const Scene3D& scene, const Float3& camera,
                  const PathScene& paths, const PathTextures& textures, jobs::Pool* pool) {
    Sdfgi3D settings = scene.sdfgi;
    settings.cascades = std::clamp<std::uint32_t>(settings.cascades, 1, kMaxSdfgiCascades);
    settings.cascade_size = std::max<std::uint32_t>(
        kSdfgiProbeDivisor, settings.cascade_size / kSdfgiProbeDivisor * kSdfgiProbeDivisor);
    const std::uint32_t size = settings.cascade_size;
    const float y_mult = sdfgi_y_mult(settings);
    const std::uint32_t history_size = std::max(1U, settings.frames_to_converge);
    const auto drag_margin = static_cast<std::int32_t>(probe_cells(size) / 2);
    Float3 world = camera;
    world[1] *= y_mult;
    // A change to what the cascades are made of starts them over (Godot makes its SDFGI again).
    const bool remake = state.cascades.size() != settings.cascades ||
                        state.settings.min_cell_size != settings.min_cell_size ||
                        state.settings.y_scale != settings.y_scale ||
                        state.settings.cascade_size != settings.cascade_size ||
                        state.settings.frames_to_converge != settings.frames_to_converge ||
                        state.settings.use_occlusion != settings.use_occlusion;
    state.settings = settings;
    std::vector<bool> dirty(settings.cascades, false);
    // How far each cascade that moved moved, in cells, when it can scroll.
    std::vector<std::optional<std::array<std::int32_t, 3>>> scrolls(settings.cascades);
    if (remake) {
        state.cascades.assign(settings.cascades, {});
        float cell_size = settings.min_cell_size;
        for (SdfgiCascade& cascade : state.cascades) {
            cascade.cell_size = cell_size;
            const float probe_size = cell_size * static_cast<float>(probe_cells(size));
            for (std::size_t a = 0; a < 3; ++a) {
                cascade.position.at(a) =
                    static_cast<std::int32_t>(std::floor((world.at(a) / probe_size) + 0.5F)) *
                    static_cast<std::int32_t>(probe_cells(size));
            }
            clear_probes(cascade, history_size);
            cell_size *= 2.0F;
        }
        dirty.assign(settings.cascades, true);
        state.render_pass = 0;
    } else {
        // Godot's SDFGI::update: each cascade follows the camera by whole steps.
        for (std::size_t c = 0; c < state.cascades.size(); ++c) {
            SdfgiCascade& cascade = state.cascades[c];
            std::array<std::int32_t, 3> moved{};
            for (std::size_t a = 0; a < 3; ++a) {
                const auto pos =
                    static_cast<std::int32_t>(std::floor(world.at(a) / cascade.cell_size));
                std::int32_t& at = cascade.position.at(a);
                while (pos < at - drag_margin) {
                    at -= drag_margin * 2;
                    moved.at(a) -= drag_margin * 2;
                }
                while (pos > at + drag_margin) {
                    at += drag_margin * 2;
                    moved.at(a) += drag_margin * 2;
                }
            }
            if (moved != std::array<std::int32_t, 3>{}) {
                dirty[c] = true;
                const auto step = static_cast<std::int32_t>(probe_cells(size));
                bool far = false;
                for (std::size_t a = 0; a < 3; ++a) {
                    far = far || std::abs(moved.at(a)) >= static_cast<std::int32_t>(size);
                }
                if (far) {
                    clear_probes(cascade, history_size);
                } else {
                    scroll_probes(cascade, {moved[0] / step, moved[1] / step, moved[2] / step},
                                  history_size);
                    scrolls[c] = moved;
                }
            }
        }
    }
    const std::uint64_t geometry = geometry_key(paths);
    if (geometry != state.geometry) {
        dirty.assign(settings.cascades, true);
        scrolls.assign(settings.cascades, std::nullopt);
        state.geometry = geometry;
    }
    for (std::size_t c = 0; c < state.cascades.size(); ++c) {
        if (dirty[c]) {
            voxelize(state.cascades[c], size, y_mult, paths, textures, pool, scrolls[c]);
        }
    }
    const std::vector<SdfgiLight> lights = sdfgi_lights(scene, y_mult);
    for (std::size_t c = 0; c < state.cascades.size(); ++c) {
        light_cells(state, c, lights, pool);
    }
    integrate(state, scene, paths, textures, pool);
    ++state.frame;
}

namespace {

// gi.glsl's sdfvoxel_gi_process: a cascade's probes around a point (in probes) seen along a
// normal and a reflection.
void probe_light(const SdfgiState& state, std::size_t cascade, Float3 cascade_pos,
                 const Float3& normal, const Float3& reflection, float roughness, Float3& diffuse,
                 Float3& specular) {
    const SdfgiCascade& c = state.cascades[cascade];
    const auto csize = static_cast<float>(state.settings.cascade_size);
    const float normal_bias =
        (state.settings.normal_bias / csize) * static_cast<float>(kSdfgiProbeAxis - 1);
    cascade_pos = add(cascade_pos, scale(normal, normal_bias));
    const std::array<std::int32_t, 3> base{static_cast<std::int32_t>(std::floor(cascade_pos[0])),
                                           static_cast<std::int32_t>(std::floor(cascade_pos[1])),
                                           static_cast<std::int32_t>(std::floor(cascade_pos[2]))};
    Float3 diffuse_accum{};
    float weight_accum = 0.0F;
    Float3 specular_accum{};
    for (std::uint32_t j = 0; j < 8; ++j) {
        const std::array<std::int32_t, 3> probe{base[0] + static_cast<std::int32_t>(j & 1U),
                                                base[1] + static_cast<std::int32_t>((j >> 1U) & 1U),
                                                base[2] +
                                                    static_cast<std::int32_t>((j >> 2U) & 1U)};
        const Float3 probe_to_pos =
            sub(cascade_pos, {static_cast<float>(probe[0]), static_cast<float>(probe[1]),
                              static_cast<float>(probe[2])});
        const Float3 probe_dir = normalize(scale(probe_to_pos, -1.0F));
        const Float3 trilinear{1.0F - std::abs(probe_to_pos[0]), 1.0F - std::abs(probe_to_pos[1]),
                               1.0F - std::abs(probe_to_pos[2])};
        const float weight =
            trilinear[0] * trilinear[1] * trilinear[2] * std::max(0.005F, dot(normal, probe_dir));
        const Float3 d = probe_texel(c, c.irradiance, probe, normal);
        diffuse_accum = add(diffuse_accum, scale(d, weight));
        weight_accum += weight;
        Float3 s{};
        if (roughness < 0.99F) {
            s = probe_texel(c, c.radiance, probe, reflection);
        }
        if (roughness > 0.2F) {
            s = lerp(s, probe_texel(c, c.irradiance, probe, reflection),
                     (roughness - 0.2F) * 1.25F);
        }
        specular_accum = add(specular_accum, scale(s, weight));
    }
    if (weight_accum > 0.0F) {
        diffuse_accum = scale(diffuse_accum, 1.0F / weight_accum);
        specular_accum = scale(specular_accum, 1.0F / weight_accum);
    }
    diffuse = diffuse_accum;
    specular = specular_accum;
}

struct GiLight {
    std::array<float, 4> ambient{};
    std::array<float, 4> reflection{};
};

// gi.glsl's sdfgi_process, with vertex the pixel's point relative to the camera (y scaled) as
// Godot's GI pass has it.
GiLight sdfgi_process(const SdfgiState& state, const Cascades& cs, const Float3& camera_scaled,
                      Float3 vertex, Float3 normal, Float3 reflection, float roughness) {
    const Sdfgi3D& settings = state.settings;
    const float y_mult = sdfgi_y_mult(settings);
    vertex[1] *= y_mult;
    normal[1] *= y_mult;
    reflection[1] *= y_mult;
    normal = normalize(normal);
    reflection = normalize(reflection);
    const auto size = static_cast<float>(settings.cascade_size);
    const auto probe_size = static_cast<float>(kSdfgiProbeAxis - 1);
    // Cascades' places relative to the camera, as Godot's UBO holds them.
    const auto position = [&](std::size_t i) { return sub(cs.offsets[i], camera_scaled); };
    const auto to_probe = [&](std::size_t i) {
        return 1.0F / (size * state.cascades[i].cell_size / probe_size);
    };
    GiLight out;
    std::size_t cascade = state.cascades.size();
    Float3 cascade_pos{};
    for (std::size_t i = 0; i < state.cascades.size(); ++i) {
        cascade_pos = scale(sub(vertex, position(i)), to_probe(i));
        if (cascade_pos[0] < 0.0F || cascade_pos[1] < 0.0F || cascade_pos[2] < 0.0F ||
            cascade_pos[0] >= probe_size || cascade_pos[1] >= probe_size ||
            cascade_pos[2] >= probe_size) {
            continue;
        }
        cascade = i;
        break;
    }
    if (cascade == state.cascades.size()) {
        return out;
    }
    out.ambient = {0.0F, 0.0F, 0.0F, 1.0F};
    out.reflection = {0.0F, 0.0F, 0.0F, 1.0F};
    Float3 diffuse{};
    Float3 specular{};
    probe_light(state, cascade, cascade_pos, normal, reflection, roughness, diffuse, specular);
    {
        const float blend_from = (probe_size / 2.0F) - 2.5F;
        const float blend_to = blend_from + 2.0F;
        Float3 inner = scale(vertex, to_probe(cascade));
        float len = length(inner);
        inner = normalize(inner);
        len *= std::max(std::abs(inner[0]), std::max(std::abs(inner[1]), std::abs(inner[2])));
        const float blend = len >= blend_from ? smoothstep(blend_from, blend_to, len) : 0.0F;
        if (blend > 0.0F) {
            if (cascade == state.cascades.size() - 1) {
                out.ambient[3] = 1.0F - blend;
                out.reflection[3] = 1.0F - blend;
            } else {
                Float3 diffuse2{};
                Float3 specular2{};
                const Float3 pos2 =
                    scale(sub(vertex, position(cascade + 1)), to_probe(cascade + 1));
                probe_light(state, cascade + 1, pos2, normal, reflection, roughness, diffuse2,
                            specular2);
                diffuse = lerp(diffuse, diffuse2, blend);
                specular = lerp(specular, specular2, blend);
            }
        }
    }
    // Smooth surfaces march their reflection through the distance fields.
    if (roughness < 0.2F) {
        const std::size_t count = state.cascades.size();
        const float blend_size = (size / probe_size) * 0.5F;
        std::array<float, kMaxSdfgiCascades> radius{};
        std::size_t first = 0xFFFF;
        const float base_distance = length(vertex);
        for (std::size_t i = 0; i < count; ++i) {
            radius.at(i) = state.cascades[i].cell_size * ((size * 0.5F) - blend_size);
            if (first == 0xFFFF && base_distance < radius.at(i)) {
                first = i;
            }
        }
        std::size_t at = std::min(first, count - 1);
        const float max_distance = radius.at(count - 1);
        Float3 ray_pos = vertex;
        {
            const float prev = at > 0 ? radius.at(at - 1) : 0.0F;
            const float base_blend = (base_distance - prev) / (radius.at(at) - prev);
            const float bias = (1.0F + base_blend) * 1.1F;
            const float most = std::max(std::abs(reflection[0]),
                                        std::max(std::abs(reflection[1]), std::abs(reflection[2])));
            ray_pos = add(ray_pos, scale(add(scale(reflection, 1.0F / most), scale(normal, 1.4F)),
                                         bias * state.cascades[at].cell_size));
        }
        const float softness = 0.2F + (std::min(1.0F, roughness * 5.0F) * 4.0F);
        std::array<float, 4> accum{};
        std::size_t i = 0;
        bool found = false;
        // Godot's loop, with a bound on its steps.
        for (int steps = 0; steps < 4096; ++steps) {
            if (length(ray_pos) >= max_distance || accum[3] > 0.99F) {
                break;
            }
            if (!found && i >= at && length(ray_pos) < radius.at(i)) {
                const std::size_t next = std::min(i + 1, count - 1);
                at = std::max(i, at);
                const auto hit_at = [&](std::size_t k, float& fdistance) {
                    const Float3 p =
                        scale(sub(ray_pos, position(k)), 1.0F / state.cascades[k].cell_size);
                    fdistance = sdf_at(cs.grid, state.cascades[k], p) - 1.1F;
                    std::array<float, 4> light{};
                    if (fdistance < softness) {
                        const Float3 l = sample3<Float3>(
                            cs.grid, p, [&](std::size_t t) { return state.cascades[k].light[t]; });
                        light[3] = std::clamp(1.0F - (fdistance / softness), 0.0F, 1.0F);
                        for (std::size_t ch = 0; ch < 3; ++ch) {
                            light.at(ch) = l.at(ch) * 0.5F * light[3];
                        }
                    }
                    fdistance *= state.cascades[k].cell_size;
                    return light;
                };
                float fdistance = 0.0F;
                std::array<float, 4> light = hit_at(i, fdistance);
                if (i < count - 1) {
                    float fdistance2 = 0.0F;
                    const std::array<float, 4> light2 = hit_at(next, fdistance2);
                    const float prev = i == 0 ? 0.0F : radius.at(i - 1);
                    const float blend =
                        std::clamp((length(ray_pos) - prev) / (radius.at(i) - prev), 0.0F, 1.0F);
                    for (std::size_t ch = 0; ch < 4; ++ch) {
                        light.at(ch) += (light2.at(ch) - light.at(ch)) * blend;
                    }
                    fdistance += (fdistance2 - fdistance) * blend;
                }
                for (std::size_t ch = 0; ch < 4; ++ch) {
                    accum.at(ch) += light.at(ch);
                }
                ray_pos = add(ray_pos, scale(reflection, fdistance));
                found = true;
            }
            ++i;
            if (i == count) {
                i = 0;
                found = false;
            }
        }
        const Float3 light =
            scale({accum[0], accum[1], accum[2]}, 1.0F / std::max(accum[3], 1e-5F));
        const float alpha = std::min(1.0F, accum[3]);
        const float b = std::min(1.0F, roughness * 5.0F);
        const float sa = 1.0F - b;
        out.reflection[3] = (alpha * sa) + b;
        if (out.reflection[3] == 0.0F) {
            specular = {};
        } else {
            specular =
                scale(add(scale(light, alpha * sa), scale(specular, b)), 1.0F / out.reflection[3]);
        }
    }
    for (std::size_t ch = 0; ch < 3; ++ch) {
        out.ambient.at(ch) = diffuse.at(ch) * settings.energy;
        out.reflection.at(ch) = specular.at(ch) * settings.energy;
    }
    return out;
}

} // namespace

std::vector<float> run_sdfgi(const SdfgiState& state, const Float3& camera,
                             const RayShadowFrame& frame, std::span<const float> depth,
                             std::span<const float> normals, jobs::Pool* pool) {
    const std::size_t pixels = std::size_t{frame.width} * frame.height;
    std::vector<float> out(pixels * 8, 0.0F);
    if (state.cascades.empty()) {
        return out;
    }
    const Cascades cs(state, state.settings.cascade_size);
    Float3 camera_scaled = camera;
    camera_scaled[1] *= sdfgi_y_mult(state.settings);
    for_each(frame.height, pool, [&](std::size_t y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t at = (y * frame.width) + x;
            const std::optional<RaySurface> surface =
                ray_surface(frame, x, static_cast<std::uint32_t>(y), depth[at],
                            {normals[at * 4], normals[(at * 4) + 1], normals[(at * 4) + 2]});
            if (!surface || dot(surface->normal, surface->normal) == 0.0F) {
                continue;
            }
            const float roughness = std::clamp(normals[(at * 4) + 3] * 255.0F / 127.0F, 0.0F, 1.0F);
            const Float3 view = normalize(sub(surface->origin, camera));
            const Float3 reflection =
                sub(view, scale(surface->normal, 2.0F * dot(view, surface->normal)));
            const GiLight gi = sdfgi_process(state, cs, camera_scaled, sub(surface->origin, camera),
                                             surface->normal, reflection, roughness);
            for (std::size_t ch = 0; ch < 4; ++ch) {
                out[(at * 8) + ch] = gi.ambient.at(ch);
                out[(at * 8) + 4 + ch] = gi.reflection.at(ch);
            }
        }
    });
    return out;
}

} // namespace slash::render::detail
