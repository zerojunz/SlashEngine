#pragma once

#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Meshes made from other meshes, as CSG roots and GridMap blocks are: their surfaces and what
// each is drawn with.
namespace slash::scene3d {

// What a surface is drawn with: a StandardMaterial3D, such as a CSG shape's or a mesh library
// item's, or a model's own material; neither for Godot's default material.
struct SurfaceMaterial {
    std::optional<world::StandardMaterial3D> standard;
    std::string model_path;             // the model whose material it is, or ""
    std::shared_ptr<const Model> model; // that model
    int model_material = -1;            // into its materials
    entt::entity entity = entt::null;   // the entity that names the model

    friend bool operator==(const SurfaceMaterial& a, const SurfaceMaterial& b) {
        return a.standard == b.standard && a.model == b.model &&
               a.model_material == b.model_material;
    }
};

// One surface of a made mesh: triangles with Godot's clockwise front faces, all drawn with one
// material.
struct MeshSurface {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    SurfaceMaterial material;
};

} // namespace slash::scene3d
