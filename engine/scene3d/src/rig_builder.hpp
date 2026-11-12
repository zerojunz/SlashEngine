#pragma once

// What character and creature models share (src/character.cpp, src/creature.cpp): placing
// Godot's primitive meshes by transforms that turn and stretch them, and a builder that gathers
// them into one skinned mesh with a surface per material, each corner moved by one joint.

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
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

// A bone of the skeleton: Godot's humanoid profile's name, its parent (before it) and where its
// head is in the model's space at rest.
struct Bone {
    std::string name;
    int parent = -1;
    Vec3 head;
};

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

    // The mesh's corners placed by place, drawn with material and moved by one joint.
    void add(const MeshData& mesh, const Affine3D& place, int material, std::size_t joint) {
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
        VertexWeights weights;
        weights.joints[0] = static_cast<std::uint16_t>(joint);
        weights.weights[0] = 1.0F;
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
            surface.vertices.push_back(vertex);
            surface.weights.push_back(weights);
        }
        for (const std::uint32_t i : mesh.indices) {
            surface.indices.push_back(first + i);
        }
    }
};

} // namespace slash::scene3d::rig
