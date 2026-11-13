#pragma once

// What character and creature models share (src/character.cpp, src/creature.cpp): placing
// Godot's primitive meshes by transforms that turn and stretch them, and a builder that gathers
// them into one skinned mesh with a surface per material, each corner moved by one joint or by
// several, blended.

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d::rig {

using world::Affine3D;

inline real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

inline real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l > 0.0F ? v * (1.0F / l) : v;
}

inline Vec3 lerp(Vec3 a, Vec3 b, real_t t) {
    return a + ((b - a) * t);
}

// A turn that takes +y to d, keeping x level where it can.
inline Affine3D facing(Vec3 d) {
    const Vec3 up = normalized(d);
    const Vec3 reference = std::abs(up.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    Affine3D out;
    out.x = normalized(cross(up, reference));
    out.y = up;
    out.z = cross(out.x, up);
    return out;
}

// A turn, then a stretch along its axes, then a move.
inline Affine3D placed(const Affine3D& turn, Vec3 stretch, Vec3 at) {
    Affine3D out;
    out.x = turn.x * stretch.x;
    out.y = turn.y * stretch.y;
    out.z = turn.z * stretch.z;
    out.origin = at;
    return out;
}

inline Affine3D placed(Vec3 stretch, Vec3 at) {
    return placed(Affine3D{}, stretch, at);
}

inline Affine3D moved(Vec3 at) {
    Affine3D out;
    out.origin = at;
    return out;
}

inline MeshData unit_sphere(bool hemisphere = false) {
    world::SphereMesh sphere;
    sphere.radius = 1.0F;
    // Godot's hemisphere is height tall, its sphere height across.
    sphere.height = hemisphere ? 1.0F : 2.0F;
    sphere.radial_segments = 16;
    sphere.rings = 8;
    sphere.is_hemisphere = hemisphere;
    return sphere_mesh(sphere);
}

// A tube that is radius wide with a round end each side, length long between their centers.
inline MeshData capsule(float radius, float length) {
    world::CapsuleMesh capsule;
    capsule.radius = radius;
    capsule.height = length + (2.0F * radius);
    capsule.radial_segments = 12;
    capsule.rings = 2;
    return capsule_mesh(capsule);
}

// A tube 1 high and 1 across at its top, bottom times as wide at its bottom, with no ends.
inline MeshData open_tube(float bottom = 1.0F) {
    world::CylinderMesh cylinder;
    cylinder.top_radius = 1.0F;
    cylinder.bottom_radius = bottom;
    cylinder.height = 1.0F;
    cylinder.radial_segments = 16;
    cylinder.rings = 1;
    cylinder.cap_top = false;
    cylinder.cap_bottom = false;
    return cylinder_mesh(cylinder);
}

inline MeshData disc_cylinder(float bottom = 1.0F) {
    world::CylinderMesh cylinder;
    cylinder.top_radius = 1.0F;
    cylinder.bottom_radius = bottom;
    cylinder.height = 1.0F;
    cylinder.radial_segments = 16;
    cylinder.rings = 1;
    return cylinder_mesh(cylinder);
}

// A bone of the skeleton: Godot's humanoid profile's name, its parent (before it), where its
// head is in the model's space at rest, how it is turned there (its global rest's basis;
// creatures' bones aren't turned) and where it points (its head, for one that points nowhere).
struct Bone {
    std::string name;
    int parent = -1;
    Vec3 head;
    Affine3D turn{};
    Vec3 tail{};
};

// Which joints move a corner, and how much: up to eight, their weights summing to 1.
using Weigh = std::function<VertexWeights(Vec3)>;
// The same for a mesh's corner by its index, for corners that share a place but not joints.
using WeighEach = std::function<VertexWeights(std::size_t, Vec3)>;

// The weights of one joint, or of two blended weight of the way from a to b.
inline VertexWeights one_joint(std::size_t joint) {
    VertexWeights out;
    out.joints[0] = static_cast<std::uint16_t>(joint);
    out.weights[0] = 1.0F;
    return out;
}

inline VertexWeights blend_joints(std::size_t a, std::size_t b, float weight) {
    if (weight <= 0.0F || a == b) {
        return one_joint(a);
    }
    if (weight >= 1.0F) {
        return one_joint(b);
    }
    VertexWeights out;
    out.joints[0] = static_cast<std::uint16_t>(a);
    out.weights[0] = 1.0F - weight;
    out.joints[1] = static_cast<std::uint16_t>(b);
    out.weights[1] = weight;
    return out;
}

// Weights mixed: a's scaled by 1 - weight and b's by weight, a joint in both summed, the eight
// heaviest kept and made to sum to 1.
inline VertexWeights mix_weights(const VertexWeights& a, const VertexWeights& b, float weight) {
    if (weight <= 0.0F) {
        return a;
    }
    if (weight >= 1.0F) {
        return b;
    }
    std::array<std::uint16_t, 16> joints{};
    std::array<float, 16> amounts{};
    std::size_t count = 0;
    const auto put = [&](std::uint16_t joint, float amount) {
        if (amount <= 0.0F) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (joints[i] == joint) {
                amounts[i] += amount;
                return;
            }
        }
        joints[count] = joint;
        amounts[count] = amount;
        ++count;
    };
    for (std::size_t i = 0; i < a.joints.size(); ++i) {
        put(a.joints[i], a.weights[i] * (1.0F - weight));
    }
    for (std::size_t i = 0; i < b.joints.size(); ++i) {
        put(b.joints[i], b.weights[i] * weight);
    }
    // Heaviest first, ties to the lower joint, so the order is the same everywhere.
    for (std::size_t i = 1; i < count; ++i) {
        for (std::size_t j = i;
             j > 0 && (amounts[j] > amounts[j - 1] ||
                       (amounts[j] == amounts[j - 1] && joints[j] < joints[j - 1]));
             --j) {
            std::swap(amounts[j], amounts[j - 1]);
            std::swap(joints[j], joints[j - 1]);
        }
    }
    VertexWeights out;
    float total = 0.0F;
    const std::size_t kept = std::min(count, out.joints.size());
    for (std::size_t i = 0; i < kept; ++i) {
        total += amounts[i];
    }
    for (std::size_t i = 0; i < kept; ++i) {
        out.joints[i] = joints[i];
        out.weights[i] = amounts[i] / total;
    }
    return out;
}

struct Builder {
    Model model;
    ModelMesh body;
    std::vector<int> surface_of; // a material's surface in body, or -1

    int material(const std::string& name, Vec4 color, float roughness, float metallic,
                 bool two_sided = false) {
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            if (model.materials[i].name == name) {
                return static_cast<int>(i);
            }
        }
        ModelMaterial made;
        made.name = name;
        made.standard.albedo_color = color;
        made.standard.roughness = roughness;
        made.standard.metallic = metallic;
        if (two_sided) {
            made.standard.cull_mode = "disabled";
        }
        model.materials.push_back(std::move(made));
        surface_of.push_back(-1);
        return static_cast<int>(model.materials.size()) - 1;
    }

    // Names the body's blend shapes, which add_shaped's offsets follow; each starts at 0.
    void set_blend_shapes(const std::vector<std::string_view>& names) {
        body.blend_shapes.assign(names.begin(), names.end());
        body.blend_weights.assign(names.size(), 0.0F);
    }

    // As add, with each blend shape's offset for each of the mesh's corners (offset(shape,
    // corner)), in the model's space.
    template <typename Offset>
    void add_shaped(const MeshData& mesh, const Affine3D& place, int material, const Weigh& weigh,
                    Offset offset) {
        add_shaped(mesh, place, material,
                   WeighEach([&weigh](std::size_t, Vec3 p) { return weigh(p); }), offset);
    }
    template <typename Offset>
    void add_shaped(const MeshData& mesh, const Affine3D& place, int material,
                    const WeighEach& weigh, Offset offset) {
        if (mesh.vertices.empty()) {
            return;
        }
        add(mesh, place, material, weigh);
        ModelSurface& surface = body.surfaces.at(
            static_cast<std::size_t>(surface_of.at(static_cast<std::size_t>(material))));
        const std::size_t first = surface.vertices.size() - mesh.vertices.size();
        surface.blend_shapes.resize(body.blend_shapes.size());
        for (std::size_t s = 0; s < body.blend_shapes.size(); ++s) {
            auto& positions = surface.blend_shapes[s].positions;
            positions.resize(first, {0.0F, 0.0F, 0.0F});
            for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
                const Vec3 o = offset(s, v);
                positions.push_back({to_float(o.x), to_float(o.y), to_float(o.z)});
            }
        }
    }

    // Each shaped surface's offsets padded with zeros to all its corners, once the body is built.
    void finish_blend_shapes() {
        for (ModelSurface& surface : body.surfaces) {
            for (SurfaceBlendShape& shape : surface.blend_shapes) {
                shape.positions.resize(surface.vertices.size(), {0.0F, 0.0F, 0.0F});
            }
        }
    }

    // The mesh's corners placed by place, drawn with material and moved by one joint.
    void add(const MeshData& mesh, const Affine3D& place, int material, std::size_t joint) {
        const VertexWeights weights = one_joint(joint);
        add(mesh, place, material, [&](Vec3) { return weights; });
    }

    // The same, each corner moved by the joints weigh gives for its place at rest.
    void add(const MeshData& mesh, const Affine3D& place, int material, const Weigh& weigh) {
        add(mesh, place, material, WeighEach([&weigh](std::size_t, Vec3 p) { return weigh(p); }));
    }

    // The same, each corner moved by the joints weigh gives for its index and place at rest.
    void add(const MeshData& mesh, const Affine3D& place, int material, const WeighEach& weigh) {
        if (mesh.vertices.empty()) {
            return; // nothing to draw, such as a low poly head too coarse for lips
        }
        if (surface_of.size() < model.materials.size()) {
            surface_of.resize(model.materials.size(), -1);
        }
        int& index = surface_of.at(static_cast<std::size_t>(material));
        if (index < 0) {
            index = static_cast<int>(body.surfaces.size());
            ModelSurface surface;
            surface.material = material;
            body.surfaces.push_back(std::move(surface));
        }
        ModelSurface& surface = body.surfaces.at(static_cast<std::size_t>(index));
        const Affine3D inverse = place.inverse();
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (render::Vertex3D vertex : mesh.vertices) {
            const Vec3 p =
                place.apply({vertex.position[0], vertex.position[1], vertex.position[2]});
            const Vec3 n{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
            // Normals turn by the inverse transpose, so stretched shapes keep them square.
            const Vec3 normal =
                normalized({dot(inverse.x, n), dot(inverse.y, n), dot(inverse.z, n)});
            const Vec3 tangent =
                normalized(place.basis({vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}));
            vertex.position = {static_cast<float>(p.x), static_cast<float>(p.y),
                               static_cast<float>(p.z)};
            vertex.normal = {static_cast<float>(normal.x), static_cast<float>(normal.y),
                             static_cast<float>(normal.z)};
            vertex.tangent = {static_cast<float>(tangent.x), static_cast<float>(tangent.y),
                              static_cast<float>(tangent.z), vertex.tangent[3]};
            surface.weights.push_back(weigh(surface.vertices.size() - first, p));
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t i : mesh.indices) {
            surface.indices.push_back(first + i);
        }
    }
};

} // namespace slash::scene3d::rig
