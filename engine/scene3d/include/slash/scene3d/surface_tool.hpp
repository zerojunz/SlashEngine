#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Meshes gameplay code builds while a game runs, as Godot's SurfaceTool and ArrayMesh: set a
// vertex's attributes, add it, add indices if you like, then commit the surface into a Model
// (Godot's ArrayMesh, one surface per commit) and make it an asset any MeshInstance3D can draw:
//
//   scene3d::SurfaceTool tool;
//   tool.set_uv({0, 0});
//   tool.add_vertex({0, 0, 0});
//   ... // triangles, front faces clockwise as Godot's
//   tool.generate_normals();
//   scene3d::Model mesh;
//   tool.commit(mesh);
//   auto path = scene3d::make_mesh(world, "ground", std::move(mesh)); // "made:ground"
//   world.ecs().get<world::MeshInstance3D>(entity).mesh = path.value();
//
// The results are the same bits on every machine.
namespace slash::scene3d {

// Godot's SurfaceTool for triangles.
class SurfaceTool {
public:
    // The attributes the next add_vertex takes, as Godot's set_* calls. Colors are sRGB 0 to 1.
    void set_color(Vec4 color) { color_ = color; }
    void set_normal(Vec3 normal) { normal_ = normal; }
    // xyz and the sign of the binormal in w, as Godot's Plane tangent.
    void set_tangent(Vec4 tangent) { tangent_ = tangent; }
    void set_uv(Vec2 uv) { uv_ = uv; }
    void set_uv2(Vec2 uv) { uv2_ = uv; }
    // generate_normals smooths across triangles of one group at shared positions; kFlat leaves a
    // vertex's triangle's own normal.
    static constexpr std::uint32_t kFlat = std::numeric_limits<std::uint32_t>::max();
    void set_smooth_group(std::uint32_t group) { smooth_group_ = group; }
    // The material the surface is drawn with; left unset, Godot's default.
    void set_material(const world::StandardMaterial3D& material) { material_ = material; }

    // A vertex at a position with the attributes set so far.
    void add_vertex(Vec3 position);
    // Indexes the vertices: three a triangle, front faces clockwise. Without any, every three
    // vertices are a triangle.
    void add_index(std::uint32_t index) { indices_.push_back(index); }

    // Godot's index: equal vertices become one, and the triangles index them.
    void index();
    // Godot's deindex: each triangle's corners become vertices of their own.
    void deindex();
    // Godot's generate_normals: each triangle's face normal, summed at positions its smooth
    // group shares and normalized. flip turns them over, for counter-clockwise triangles.
    void generate_normals(bool flip = false);
    // Godot's generate_tangents: MikkTSpace's tangents from the normals and texture
    // coordinates, as models' importers make them.
    void generate_tangents();

    [[nodiscard]] std::size_t vertex_count() const { return vertices_.size(); }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const { return indices_; }

    // Godot's commit: adds the surface to the model's one mesh (made if it has none, with a part
    // placing it at the origin), with its material, and clears the tool. Fails, changing
    // nothing, on a vertex count not a whole number of triangles, an index past the vertices or
    // more vertices than a mesh takes.
    Result<void> commit(Model& model);
    // Forgets the vertices, indices, attributes and material.
    void clear() { *this = SurfaceTool(); }

private:
    struct Vertex {
        render::Vertex3D vertex;
        std::uint32_t smooth_group = 0;
    };

    Vec4 color_{1.0F, 1.0F, 1.0F, 1.0F};
    Vec3 normal_{0.0F, 1.0F, 0.0F};
    Vec4 tangent_{1.0F, 0.0F, 0.0F, 1.0F};
    Vec2 uv_;
    Vec2 uv2_;
    std::uint32_t smooth_group_ = 0;
    std::optional<world::StandardMaterial3D> material_;
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
};

// Makes a mesh gameplay code built an asset at "made:<name>" in the assets the world's modules
// load from (canvas::install attaches them), as Godot's ArrayMesh: a MeshInstance3D's mesh, a
// particle emitter's or anything else that names a model can name the path. Making it again
// with the same name changes it in place, and renderers draw the new one. Returns the path, or
// fails when the world has no assets or name isn't letters, digits and "_-./".
Result<std::string> make_mesh(world::World& world, std::string_view name, Model model);

} // namespace slash::scene3d
