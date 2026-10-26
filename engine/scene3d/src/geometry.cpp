// Holes, degenerate triangles and texel density, for reviewing a model (geometry.hpp).

#include "slash/scene3d/geometry.hpp"

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// Corners within this share of the model's size of each other are one corner.
constexpr float kWeld = 1e-5F;

// The union-find of open edges' corners, so loops count once.
struct Loops {
    std::vector<std::size_t> parent;

    std::size_t find(std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void join(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

// An image's width and height, from its pixels or its file.
Result<std::pair<std::uint32_t, std::uint32_t>> size_of(const ModelImage& image,
                                                        const ModelReader& read) {
    if (!image.image.pixels.empty() || image.path.empty() || !read) {
        return std::pair{image.image.width, image.image.height};
    }
    auto bytes = read(image.path);
    if (!bytes) {
        return make_error(bytes.error().code, "\"" + image.path + "\": " + bytes.error().message);
    }
    auto decoded = render::decode_image(bytes.value());
    if (!decoded) {
        return make_error(decoded.error().code,
                          "\"" + image.path + "\": " + decoded.error().message);
    }
    return std::pair{decoded.value().width, decoded.value().height};
}

} // namespace

Result<GeometryReport> check_geometry(const Model& model, const ModelReader& read) {
    GeometryReport out;
    const Bounds box = bounds(model);
    const float size = std::max(length(box.max - box.min), 1e-6F);
    const float step = size * kWeld;

    std::vector<std::optional<std::pair<std::uint32_t, std::uint32_t>>> sizes(model.images.size());
    std::map<std::array<std::int64_t, 3>, std::size_t> corners;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edges;
    double texels = 0.0;
    double area = 0.0;
    for (const ModelPart& part : model.parts) {
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            std::optional<std::pair<std::uint32_t, std::uint32_t>> texture;
            if (surface.material >= 0) {
                const int image =
                    model.materials[static_cast<std::size_t>(surface.material)].albedo_image;
                if (image >= 0) {
                    auto& known = sizes[static_cast<std::size_t>(image)];
                    if (!known) {
                        auto found = size_of(model.images[static_cast<std::size_t>(image)], read);
                        if (!found) {
                            return found.error();
                        }
                        known = found.value();
                    }
                    if (known->first > 0 && known->second > 0) {
                        texture = known;
                    }
                }
            }
            std::vector<std::size_t> welded(surface.vertices.size());
            std::vector<Vec3> placed(surface.vertices.size());
            for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
                const auto& p = surface.vertices[i].position;
                placed[i] = part.transform.apply({p[0], p[1], p[2]});
                const std::array<std::int64_t, 3> key{std::llround(placed[i].x / step),
                                                      std::llround(placed[i].y / step),
                                                      std::llround(placed[i].z / step)};
                welded[i] = corners.try_emplace(key, corners.size()).first->second;
            }
            for (std::size_t t = 0; t + 2 < surface.indices.size(); t += 3) {
                const std::array<std::uint32_t, 3> index{surface.indices[t], surface.indices[t + 1],
                                                         surface.indices[t + 2]};
                const std::array<std::size_t, 3> c{welded[index[0]], welded[index[1]],
                                                   welded[index[2]]};
                const float triangle = 0.5F * length(cross(placed[index[1]] - placed[index[0]],
                                                           placed[index[2]] - placed[index[0]]));
                if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2] || triangle <= step * step) {
                    ++out.degenerate;
                    continue;
                }
                for (std::size_t e = 0; e < 3; ++e) {
                    const std::size_t a = c[e];
                    const std::size_t b = c[(e + 1) % 3];
                    ++edges[{std::min(a, b), std::max(a, b)}];
                }
                if (texture) {
                    const auto& uv0 = surface.vertices[index[0]].uv;
                    const auto& uv1 = surface.vertices[index[1]].uv;
                    const auto& uv2 = surface.vertices[index[2]].uv;
                    const float uv_area = 0.5F * std::abs(((uv1[0] - uv0[0]) * (uv2[1] - uv0[1])) -
                                                          ((uv2[0] - uv0[0]) * (uv1[1] - uv0[1])));
                    texels += static_cast<double>(uv_area) * static_cast<double>(texture->first) *
                              static_cast<double>(texture->second);
                    area += static_cast<double>(triangle);
                }
            }
        }
    }

    Loops loops;
    loops.parent.resize(corners.size());
    std::iota(loops.parent.begin(), loops.parent.end(), std::size_t{0});
    std::vector<bool> on_rim(corners.size(), false);
    for (const auto& [edge, count] : edges) {
        if (count == 1) {
            ++out.open_edges;
            loops.join(edge.first, edge.second);
            on_rim[edge.first] = true;
            on_rim[edge.second] = true;
        }
    }
    for (std::size_t i = 0; i < on_rim.size(); ++i) {
        if (on_rim[i] && loops.find(i) == i) {
            ++out.holes;
        }
    }
    if (area > 0.0) {
        out.texel_density = static_cast<float>(std::sqrt(texels / area));
    }
    return out;
}

} // namespace slash::scene3d
