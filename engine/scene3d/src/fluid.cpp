// Fluids drawn as discs facing the camera (slash/scene3d/fluid.hpp).
#include "slash/scene3d/fluid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace slash::scene3d {

namespace {

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::floor((std::clamp(value, 0.0F, 1.0F) * 255.0F) + 0.5F));
}

float dot3(Vec3f a, Vec3f b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3f cross3(Vec3f a, Vec3f b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3f unit3(Vec3f v, Vec3f fallback) {
    const float n = std::sqrt(dot3(v, v));
    return n > 1e-9F ? v * (1.0F / n) : fallback;
}

Vec4 mix4(Vec4 a, Vec4 b, float t) {
    return {a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t),
            a.w + ((b.w - a.w) * t)};
}

// How much particle i foams: none below 40 radii a second, all at twice it, as fluid_color.
float foam_of(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
              std::size_t i) {
    const Vec3f v = particles.velocity[i];
    const float calm = 40.0F * recipe.radius;
    return std::clamp((std::sqrt(dot3(v, v)) - calm) / calm, 0.0F, 1.0F);
}

std::array<std::uint8_t, 4> bytes_of(Vec4 c) {
    return {to_byte(c.x), to_byte(c.y), to_byte(c.z), to_byte(c.w)};
}

// The most texels a surface's image takes across or down; past it, texels grow.
constexpr int kMaxSurfaceTexels = 192;
constexpr float kEmpty = 3.0e38F;

} // namespace

render::Image fluid_disc_image() {
    constexpr std::uint32_t kSize = 64;
    render::Image image;
    image.width = kSize;
    image.height = kSize;
    image.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float u =
                ((static_cast<float>(x) + 0.5F) / static_cast<float>(kSize) * 2.0F) - 1.0F;
            const float v =
                ((static_cast<float>(y) + 0.5F) / static_cast<float>(kSize) * 2.0F) - 1.0F;
            const float r2 = (u * u) + (v * v);
            // Solid to half way, then falling smoothly to nothing at the rim.
            const float edge = std::clamp((1.0F - r2) / 0.75F, 0.0F, 1.0F);
            const std::size_t at = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
            image.pixels[at + 3] = to_byte(edge * edge * (3.0F - (2.0F * edge)));
        }
    }
    return image;
}

void fluid_quads(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                 Vec3f right, Vec3f up, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                 std::vector<std::uint32_t>& indices, const std::function<bool(std::size_t)>& keep,
                 std::span<const float> light) {
    vertices.clear();
    indices.clear();
    const float half = world::fluid_draw_size(recipe) * 0.5F;
    const Vec3f facing{(right.y * up.z) - (right.z * up.y), (right.z * up.x) - (right.x * up.z),
                       (right.x * up.y) - (right.y * up.x)};
    constexpr std::array<std::array<float, 2>, 4> kCorners{
        {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}}};
    for (std::size_t i = 0; i < particles.size(); ++i) {
        if (keep && !keep(i)) {
            continue;
        }
        const Vec4 color =
            world::fluid_color(recipe, particles, i, i < light.size() ? light[i] : 1.0F);
        if (color.w * 255.0F < 1.0F) {
            continue;
        }
        const std::array<std::uint8_t, 4> rgba{to_byte(color.x), to_byte(color.y), to_byte(color.z),
                                               to_byte(color.w)};
        const Vec3f middle = particles.position[i] + shift;
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (const auto& [cx, cy] : kCorners) {
            render::Vertex3D vertex;
            const Vec3f at = middle + (right * (cx * half)) + (up * (cy * half));
            vertex.position = {at.x, at.y, at.z};
            // A bulge: halfway between facing and out along the corner.
            Vec3f normal = facing + (((right * cx) + (up * cy)) * (recipe.gas ? 0.0F : 0.7F));
            const float n =
                std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
            normal = n > 1e-9F ? normal * (1.0F / n) : facing;
            vertex.normal = {normal.x, normal.y, normal.z};
            vertex.tangent = {right.x, right.y, right.z, 1.0F};
            vertex.uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F};
            vertex.rgba = rgba;
            vertices.push_back(vertex);
        }
        indices.insert(indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
    }
}

void fluid_surface(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                   const FluidEye& eye, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                   std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    const std::size_t n = particles.size();
    if (recipe.gas || n == 0) {
        return;
    }
    const std::array<float, 16>& m = eye.projection;
    if (std::abs(m[0]) < 1e-9F || std::abs(m[5]) < 1e-9F) {
        return;
    }
    const Vec3f origin = to_float(eye.eye.origin);
    const Vec3f ex = unit3(to_float(eye.eye.x), {1.0F, 0.0F, 0.0F});
    const Vec3f ey = unit3(to_float(eye.eye.y), {0.0F, 1.0F, 0.0F});
    const Vec3f ez = unit3(to_float(eye.eye.z), {0.0F, 0.0F, 1.0F});
    // Each particle splats a sphere a little past its radius, so neighbors two radii apart join.
    const float big = recipe.radius * 1.6F;

    // Where each particle is on the screen (normalized device coordinates) and how far.
    struct Seen {
        float sx = 0.0F;
        float sy = 0.0F;
        float rx = 0.0F;
        float ry = 0.0F;
        float distance = 0.0F;
        float foam = 0.0F;
    };
    std::vector<Seen> seen;
    seen.reserve(n);
    float low_x = kEmpty;
    float low_y = kEmpty;
    float high_x = -kEmpty;
    float high_y = -kEmpty;
    float least_rx = kEmpty;
    float least_ry = kEmpty;
    for (std::size_t i = 0; i < n; ++i) {
        if (world::fluid_is_spray(recipe, particles, i)) {
            continue;
        }
        const Vec3f d = (particles.position[i] + shift) - origin;
        const float vx = dot3(d, ex);
        const float vy = dot3(d, ey);
        const float vz = dot3(d, ez);
        const float w = (m[3] * vx) + (m[7] * vy) + (m[11] * vz) + m[15];
        if (w <= 1e-4F || -vz < big) {
            continue; // behind the camera, or too near it to splat
        }
        Seen s;
        s.sx = ((m[0] * vx) + (m[4] * vy) + (m[8] * vz) + m[12]) / w;
        s.sy = ((m[1] * vx) + (m[5] * vy) + (m[9] * vz) + m[13]) / w;
        s.rx = big * std::abs(m[0]) / w;
        s.ry = big * std::abs(m[5]) / w;
        s.distance = -vz;
        s.foam = foam_of(recipe, particles, i);
        if (s.sx + s.rx < -1.0F || s.sx - s.rx > 1.0F || s.sy + s.ry < -1.0F ||
            s.sy - s.ry > 1.0F) {
            continue; // off the screen
        }
        low_x = std::min(low_x, s.sx - s.rx);
        low_y = std::min(low_y, s.sy - s.ry);
        high_x = std::max(high_x, s.sx + s.rx);
        high_y = std::max(high_y, s.sy + s.ry);
        least_rx = std::min(least_rx, s.rx);
        least_ry = std::min(least_ry, s.ry);
        seen.push_back(s);
    }
    if (seen.empty()) {
        return;
    }
    low_x = std::max(low_x, -1.0F);
    low_y = std::max(low_y, -1.0F);
    high_x = std::min(high_x, 1.0F);
    high_y = std::min(high_y, 1.0F);
    // Texels a little smaller than the nearest sphere, fewer when it covers a large part.
    float cell_x = least_rx * 0.7F;
    float cell_y = least_ry * 0.7F;
    cell_x = std::max(cell_x, (high_x - low_x) / static_cast<float>(kMaxSurfaceTexels));
    cell_y = std::max(cell_y, (high_y - low_y) / static_cast<float>(kMaxSurfaceTexels));
    const int columns = std::clamp(static_cast<int>(std::ceil((high_x - low_x) / cell_x)) + 1, 2,
                                   kMaxSurfaceTexels + 1);
    const int rows = std::clamp(static_cast<int>(std::ceil((high_y - low_y) / cell_y)) + 1, 2,
                                kMaxSurfaceTexels + 1);
    const auto at = [columns](int x, int y) {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(columns)) +
               static_cast<std::size_t>(x);
    };
    const std::size_t texels = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    std::vector<float> depth(texels, kEmpty);
    std::vector<float> thickness(texels, 0.0F);
    std::vector<float> foam(texels, 0.0F);
    std::vector<float> weight(texels, 0.0F);
    // Splat: the nearest sphere's front at each texel, and how much liquid lies along it.
    for (const Seen& s : seen) {
        const int x0 = std::max(static_cast<int>(std::floor((s.sx - s.rx - low_x) / cell_x)), 0);
        const int x1 =
            std::min(static_cast<int>(std::ceil((s.sx + s.rx - low_x) / cell_x)), columns - 1);
        const int y0 = std::max(static_cast<int>(std::floor((s.sy - s.ry - low_y) / cell_y)), 0);
        const int y1 =
            std::min(static_cast<int>(std::ceil((s.sy + s.ry - low_y) / cell_y)), rows - 1);
        for (int y = y0; y <= y1; ++y) {
            const float dy = ((low_y + (static_cast<float>(y) * cell_y)) - s.sy) / s.ry;
            for (int x = x0; x <= x1; ++x) {
                const float dx = ((low_x + (static_cast<float>(x) * cell_x)) - s.sx) / s.rx;
                const float q = (dx * dx) + (dy * dy);
                if (q >= 1.0F) {
                    continue;
                }
                const float h = std::sqrt(1.0F - q);
                const std::size_t k = at(x, y);
                depth[k] = std::min(depth[k], s.distance - (big * h));
                thickness[k] += 2.0F * big * h;
                foam[k] += s.foam * h;
                weight[k] += h;
            }
        }
    }
    // Smooth the depths among neighbors near in depth, so the spheres melt into one surface
    // and its edges and the gaps between near and far liquid stay.
    const float edge = big * 2.0F;
    std::vector<float> next(texels);
    constexpr std::array<float, 3> kTaps{1.0F, 2.0F, 1.0F};
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                const float here = depth[at(x, y)];
                if (here >= kEmpty) {
                    next[at(x, y)] = kEmpty;
                    continue;
                }
                float sum = 0.0F;
                float total = 0.0F;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int nx = x + ox;
                        const int ny = y + oy;
                        if (nx < 0 || ny < 0 || nx >= columns || ny >= rows) {
                            continue;
                        }
                        const float there = depth[at(nx, ny)];
                        if (there >= kEmpty || std::abs(there - here) > edge) {
                            continue;
                        }
                        const float tap = kTaps.at(static_cast<std::size_t>(ox) + 1U) *
                                          kTaps.at(static_cast<std::size_t>(oy) + 1U);
                        sum += there * tap;
                        total += tap;
                    }
                }
                next[at(x, y)] = sum / total;
            }
        }
        depth.swap(next);
    }
    // A vertex back in the world at each covered texel.
    std::vector<std::uint32_t> index_of(texels, 0xFFFFFFFFU);
    std::vector<Vec3f> place(texels);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const std::size_t k = at(x, y);
            if (depth[k] >= kEmpty) {
                continue;
            }
            const float sx = low_x + (static_cast<float>(x) * cell_x);
            const float sy = low_y + (static_cast<float>(y) * cell_y);
            const float vz = -depth[k];
            const float w = (m[11] * vz) + m[15];
            const float vy = ((sy * w) - (m[9] * vz) - m[13]) / m[5];
            const float vx = ((sx * w) - (m[8] * vz) - m[12] - (m[4] * vy)) / m[0];
            place[k] = origin + (ex * vx) + (ey * vy) + (ez * vz);
        }
    }
    const auto covered = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < columns && y < rows && depth[at(x, y)] < kEmpty;
    };
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const std::size_t k = at(x, y);
            if (depth[k] >= kEmpty) {
                continue;
            }
            const Vec3f p = place[k];
            const auto near = [&](int nx, int ny) {
                return covered(nx, ny) && std::abs(depth[at(nx, ny)] - depth[k]) <= edge;
            };
            const Vec3f right = near(x + 1, y) ? place[at(x + 1, y)] : p;
            const Vec3f left = near(x - 1, y) ? place[at(x - 1, y)] : p;
            const Vec3f top = near(x, y + 1) ? place[at(x, y + 1)] : p;
            const Vec3f bottom = near(x, y - 1) ? place[at(x, y - 1)] : p;
            Vec3f normal = cross3(right - left, top - bottom);
            if (dot3(normal, origin - p) < 0.0F) {
                normal = normal * -1.0F;
            }
            normal = unit3(normal, unit3(origin - p, ez));
            const float frothy = weight[k] > 0.0F ? foam[k] / weight[k] : 0.0F;
            Vec4 color = mix4(recipe.color, recipe.foam, frothy * 0.8F);
            // Shallow liquid shows what is behind it; the rim, half as much again.
            const float deep = 1.0F - slash::exp(-thickness[k] / std::max(recipe.depth, 1e-6F));
            color.w *= std::clamp(deep, 0.15F, 1.0F);
            if (!near(x + 1, y) || !near(x - 1, y) || !near(x, y + 1) || !near(x, y - 1)) {
                color.w *= 0.6F;
            }
            render::Vertex3D vertex;
            vertex.position = {p.x, p.y, p.z};
            vertex.normal = {normal.x, normal.y, normal.z};
            vertex.tangent = {ex.x, ex.y, ex.z, 1.0F};
            vertex.uv = {(static_cast<float>(x) + 0.5F) / static_cast<float>(columns),
                         (static_cast<float>(y) + 0.5F) / static_cast<float>(rows)};
            vertex.rgba = bytes_of(color);
            index_of[k] = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back(vertex);
        }
    }
    // Triangles between neighbors near in depth.
    for (int y = 0; y + 1 < rows; ++y) {
        for (int x = 0; x + 1 < columns; ++x) {
            const std::array<std::size_t, 4> k{at(x, y), at(x + 1, y), at(x + 1, y + 1),
                                               at(x, y + 1)};
            float nearest = kEmpty;
            float farthest = -kEmpty;
            bool all = true;
            for (const std::size_t c : k) {
                if (depth[c] >= kEmpty) {
                    all = false;
                    break;
                }
                nearest = std::min(nearest, depth[c]);
                farthest = std::max(farthest, depth[c]);
            }
            if (!all || farthest - nearest > edge) {
                continue;
            }
            indices.insert(indices.end(), {index_of[k[0]], index_of[k[2]], index_of[k[1]],
                                           index_of[k[0]], index_of[k[3]], index_of[k[2]]});
        }
    }
    if (indices.empty()) {
        vertices.clear();
    }
}

void fluid_surface_2d(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                      std::vector<FluidCorner2D>& corners) {
    corners.clear();
    const std::size_t n = particles.size();
    if (recipe.gas || n == 0) {
        return;
    }
    const float reach = recipe.radius * 2.2F;
    float low_x = kEmpty;
    float low_y = kEmpty;
    float high_x = -kEmpty;
    float high_y = -kEmpty;
    std::vector<std::size_t> body;
    body.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (world::fluid_is_spray(recipe, particles, i)) {
            continue;
        }
        const Vec3f p = particles.position[i];
        low_x = std::min(low_x, p.x - reach);
        low_y = std::min(low_y, p.y - reach);
        high_x = std::max(high_x, p.x + reach);
        high_y = std::max(high_y, p.y + reach);
        body.push_back(i);
    }
    if (body.empty()) {
        return;
    }
    float cell = recipe.radius * 0.8F;
    cell = std::max(cell, std::max(high_x - low_x, high_y - low_y) /
                              static_cast<float>(2 * kMaxSurfaceTexels));
    const int columns = static_cast<int>(std::ceil((high_x - low_x) / cell)) + 1;
    const int rows = static_cast<int>(std::ceil((high_y - low_y) / cell)) + 1;
    const auto at = [columns](int x, int y) {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(columns)) +
               static_cast<std::size_t>(x);
    };
    const std::size_t nodes = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    std::vector<float> field(nodes, 0.0F);
    std::vector<float> foam(nodes, 0.0F);
    const float reach2 = reach * reach;
    for (const std::size_t i : body) {
        const Vec3f p = particles.position[i];
        const float frothy = foam_of(recipe, particles, i);
        const int x0 = std::max(static_cast<int>(std::floor((p.x - reach - low_x) / cell)), 0);
        const int x1 =
            std::min(static_cast<int>(std::ceil((p.x + reach - low_x) / cell)), columns - 1);
        const int y0 = std::max(static_cast<int>(std::floor((p.y - reach - low_y) / cell)), 0);
        const int y1 =
            std::min(static_cast<int>(std::ceil((p.y + reach - low_y) / cell)), rows - 1);
        for (int y = y0; y <= y1; ++y) {
            const float dy = (low_y + (static_cast<float>(y) * cell)) - p.y;
            for (int x = x0; x <= x1; ++x) {
                const float dx = (low_x + (static_cast<float>(x) * cell)) - p.x;
                const float q = ((dx * dx) + (dy * dy)) / reach2;
                if (q >= 1.0F) {
                    continue;
                }
                const float k = (1.0F - q) * (1.0F - q) * (1.0F - q);
                field[at(x, y)] += k;
                foam[at(x, y)] += frothy * k;
            }
        }
    }
    // The surface is where the field crosses this: about a particle's edge.
    constexpr float kIso = 0.4F;
    const auto node_color = [&](std::size_t k) {
        const float f = field[k];
        Vec4 color = mix4(recipe.color, recipe.foam, f > 0.0F ? (foam[k] / f) * 0.8F : 0.0F);
        // Deeper inside, as the field is fuller, the color's own alpha.
        color.w *= std::clamp(0.55F + (0.45F * f), 0.0F, 1.0F);
        return color;
    };
    // Where the surface crosses an edge, lighter, as light catches the surface.
    const Vec4 rim = mix4(recipe.color, recipe.foam, 0.45F);
    for (int y = 0; y + 1 < rows; ++y) {
        for (int x = 0; x + 1 < columns; ++x) {
            // The cell's corners round it, top left first (y is down the screen).
            const std::array<std::pair<int, int>, 4> c{
                {{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}}};
            std::array<std::size_t, 4> k{};
            std::array<bool, 4> inside{};
            int count = 0;
            for (std::size_t j = 0; j < 4; ++j) {
                k.at(j) = at(c.at(j).first, c.at(j).second);
                inside.at(j) = field[k.at(j)] >= kIso;
                count += inside.at(j) ? 1 : 0;
            }
            if (count == 0) {
                continue;
            }
            // The polygon of the cell's inside: its inside corners and, between a corner in and
            // one out, where the field crosses (marching squares, saddles joined).
            std::array<FluidCorner2D, 8> polygon{};
            std::size_t sides = 0;
            for (std::size_t j = 0; j < 4; ++j) {
                const std::size_t j2 = (j + 1) % 4;
                const Vec2f a{low_x + (static_cast<float>(c.at(j).first) * cell),
                              low_y + (static_cast<float>(c.at(j).second) * cell)};
                if (inside.at(j)) {
                    polygon.at(sides) = {.position = a, .color = node_color(k.at(j))};
                    ++sides;
                }
                if (inside.at(j) != inside.at(j2)) {
                    const Vec2f b{low_x + (static_cast<float>(c.at(j2).first) * cell),
                                  low_y + (static_cast<float>(c.at(j2).second) * cell)};
                    const float fa = field[k.at(j)];
                    const float fb = field[k.at(j2)];
                    const float t = std::clamp((kIso - fa) / (fb - fa), 0.0F, 1.0F);
                    polygon.at(sides) = {
                        .position = {a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t)},
                        .color = rim};
                    ++sides;
                }
            }
            for (std::size_t j = 1; j + 1 < sides; ++j) {
                corners.push_back(polygon.at(0));
                corners.push_back(polygon.at(j));
                corners.push_back(polygon.at(j + 1));
            }
        }
    }
}

} // namespace slash::scene3d
