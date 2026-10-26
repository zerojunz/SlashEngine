// Making a model game-ready (cleanup.hpp): turning, scaling and moving it by its root, then
// simplifying and unwrapping its surfaces, and making what it collides as.

#include "slash/scene3d/cleanup.hpp"

#include "slash/scene3d/collision.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <meshoptimizer.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// sin and cos of 45 degrees: a quarter turn's quaternion.
constexpr float kHalfTurn = 0.70710678F;
// How many times the simplifier is asked again, aiming lower, when the surfaces' shares don't
// add up to the budget.
constexpr int kSimplifyTries = 4;

// The turn that takes an axis to +y.
Result<Quat> up_turn(std::string_view up) {
    if (up.empty() || up == "y") {
        return Quat{};
    }
    if (up == "z") {
        return Quat{-kHalfTurn, 0.0F, 0.0F, kHalfTurn};
    }
    if (up == "-z") {
        return Quat{kHalfTurn, 0.0F, 0.0F, kHalfTurn};
    }
    if (up == "x") {
        return Quat{0.0F, 0.0F, kHalfTurn, kHalfTurn};
    }
    if (up == "-x") {
        return Quat{0.0F, 0.0F, -kHalfTurn, kHalfTurn};
    }
    if (up == "-y") {
        return Quat{1.0F, 0.0F, 0.0F, 0.0F};
    }
    return make_error(ErrorCode::InvalidArgument,
                      std::string(R"(up is "y", "-y", "x", "-x", "z" or "-z", not ")") +
                          std::string(up) + "\"");
}

// Puts the whole model through a turn, then a uniform scale, then a move, by its root: the nodes
// without a parent and the parts where they are.
void transform_root(Model& model, const Quat& turn, float scale, Vec3 move) {
    const world::Affine3D basis = world::rotation_basis(turn);
    world::Affine3D whole = basis;
    whole.x = whole.x * scale;
    whole.y = whole.y * scale;
    whole.z = whole.z * scale;
    whole.origin = move;
    for (ModelNode& node : model.nodes) {
        if (node.parent >= 0 && static_cast<std::size_t>(node.parent) < model.nodes.size()) {
            continue;
        }
        node.position = move + (basis.basis(node.position) * scale);
        node.rotation = multiply(turn, node.rotation);
        node.scale = node.scale * scale;
    }
    for (ModelPart& part : model.parts) {
        part.transform = whole * part.transform;
    }
    for (ModelCollision& collision : model.collision) {
        for (Vec3& point : collision.points) {
            point = whole.apply(point);
        }
    }
}

// The surface with only the vertices its triangles use, in the order they first use them.
void compact(ModelSurface& surface) {
    std::vector<std::uint32_t> remap(surface.vertices.size(), UINT32_MAX);
    std::vector<render::Vertex3D> kept;
    for (std::uint32_t& index : surface.indices) {
        if (remap[index] == UINT32_MAX) {
            remap[index] = static_cast<std::uint32_t>(kept.size());
            kept.push_back(surface.vertices[index]);
        }
        index = remap[index];
    }
    surface.vertices = std::move(kept);
}

// Each mesh's triangles times the parts that draw it.
std::size_t drawn_triangles(const Model& model) {
    std::size_t total = 0;
    for (const ModelPart& part : model.parts) {
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            total += surface.indices.size() / 3;
        }
    }
    return total;
}

// Simplifies every surface to its share of the budget, from the surfaces as they were.
Result<void> simplify(Model& model, std::size_t budget) {
    const std::size_t before = drawn_triangles(model);
    if (before <= budget) {
        return {};
    }
    const std::vector<ModelMesh> original = model.meshes;
    double share = static_cast<double>(budget) / static_cast<double>(before);
    std::size_t reached = before;
    for (int attempt = 0; attempt < kSimplifyTries && reached > budget; ++attempt) {
        for (std::size_t m = 0; m < original.size(); ++m) {
            for (std::size_t s = 0; s < original[m].surfaces.size(); ++s) {
                const ModelSurface& from = original[m].surfaces[s];
                ModelSurface& into = model.meshes[m].surfaces[s];
                const std::size_t triangles = from.indices.size() / 3;
                if (triangles == 0) {
                    continue;
                }
                const auto target = std::max<std::size_t>(
                    1, static_cast<std::size_t>(static_cast<double>(triangles) * share));
                std::vector<std::uint32_t> indices(from.indices.size());
                const std::size_t count =
                    meshopt_simplify(indices.data(), from.indices.data(), from.indices.size(),
                                     from.vertices.front().position.data(), from.vertices.size(),
                                     sizeof(render::Vertex3D), target * 3, 1.0F, 0, nullptr);
                indices.resize(count);
                into.vertices = from.vertices;
                into.indices = std::move(indices);
                compact(into);
            }
        }
        reached = drawn_triangles(model);
        if (reached > budget) {
            share *= static_cast<double>(budget) / static_cast<double>(reached);
        }
    }
    if (reached > budget) {
        model.meshes = original;
        return make_error(ErrorCode::InvalidArgument,
                          "the simplifier gets it down to " + std::to_string(reached) +
                              " triangles, not " + std::to_string(budget));
    }
    for (ModelMesh& mesh : model.meshes) {
        for (ModelSurface& surface : mesh.surfaces) {
            generate_lods(surface);
        }
    }
    return {};
}

bool has_uvs(const ModelSurface& surface) {
    return std::ranges::any_of(surface.vertices, [&](const render::Vertex3D& vertex) {
        return vertex.uv != surface.vertices.front().uv;
    });
}

} // namespace

Result<CleanupReport> cleanup_model(Model& model, const CleanupOptions& options) {
    if (!model.skins.empty() || !model.animations.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("cleanup takes models without skins and animations for "
                                      "now, and this one has ") +
                              (model.skins.empty() ? "animations" : "a skin"));
    }
    CleanupReport report;
    report.triangles_before = drawn_triangles(model);
    report.bounds_before = bounds(model);

    auto turn = up_turn(options.up);
    if (!turn) {
        return turn.error();
    }
    if (turn.value() != Quat{}) {
        transform_root(model, turn.value(), 1.0F, {});
    }
    // Simplified before it is measured, since simplifying moves its bounds a little.
    if (options.triangles > 0) {
        if (auto simplified = simplify(model, options.triangles); !simplified) {
            return simplified.error();
        }
    }
    if (options.height > 0.0F) {
        const Bounds now = bounds(model);
        const float tall = now.max.y - now.min.y;
        if (tall <= 0.0F) {
            return make_error(ErrorCode::InvalidArgument,
                              "the model is flat along y, so it has no height to scale");
        }
        transform_root(model, {}, options.height / tall, {});
    } else if (options.scale > 0.0F && options.scale != 1.0F) {
        transform_root(model, {}, options.scale, {});
    }
    if (options.pivot) {
        const Bounds now = bounds(model);
        const Vec3 size = now.max - now.min;
        const Vec3 origin = {now.min.x + (size.x * options.pivot->x),
                             now.min.y + (size.y * options.pivot->y),
                             now.min.z + (size.z * options.pivot->z)};
        transform_root(model, {}, 1.0F, Vec3{} - origin);
    }
    if (options.uvs) {
        for (ModelMesh& mesh : model.meshes) {
            for (ModelSurface& surface : mesh.surfaces) {
                if (surface.indices.size() < 3 || has_uvs(surface)) {
                    continue;
                }
                auto unwrap = unwrap_lightmap(surface.vertices, surface.indices);
                if (!unwrap) {
                    continue; // nothing of any area to map
                }
                std::vector<render::Vertex3D> vertices;
                vertices.reserve(unwrap.value().xref.size());
                for (std::size_t i = 0; i < unwrap.value().xref.size(); ++i) {
                    render::Vertex3D vertex = surface.vertices[unwrap.value().xref[i]];
                    vertex.uv = unwrap.value().uv2[i];
                    vertex.uv2 = unwrap.value().uv2[i];
                    vertices.push_back(vertex);
                }
                surface.vertices = std::move(vertices);
                surface.indices = std::move(unwrap.value().indices);
                generate_lods(surface);
                ++report.unwrapped;
            }
        }
    }
    if (!options.collision.empty()) {
        DecomposeOptions decompose;
        if (options.max_hulls > 0) {
            decompose.max_hulls = options.max_hulls;
        }
        auto made = generate_collision(model, options.collision, decompose);
        if (!made) {
            return made.error();
        }
        report.collision = made.value();
    }
    report.triangles = drawn_triangles(model);
    return report;
}

} // namespace slash::scene3d
