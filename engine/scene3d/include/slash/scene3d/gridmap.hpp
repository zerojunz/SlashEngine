#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/surface.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace slash::world {
class World;
}

// Grid maps, as Godot 4.6's GridMap (modules/gridmap/grid_map.cpp) and MeshLibrary
// (scene/resources/3d/mesh_library.cpp): items of a .meshlib.json file placed in cells, each
// turned one of Godot's 24 ways. Cells are grouped into octants as Godot groups them; each
// octant's items draw as one mesh per material, their shapes collide as one static body of the
// GridMap's in physics3d, and with bake_navigation each cell with a navigation mesh is a
// navigation region.
namespace slash::scene3d {

inline constexpr std::string_view kMeshLibrarySuffix = ".meshlib.json";

// One item of a mesh library, as Godot's MeshLibrary::Item: what a cell holding it draws,
// collides as and walks on, each in the cell's space.
struct MeshLibraryItem {
    std::string mesh;                   // a model file's meshes, as they are at rest; or ""
    std::optional<Primitive> primitive; // or a primitive mesh
    // Takes the place of the model's materials, or colors the primitive; Godot's default
    // material without one.
    std::optional<world::StandardMaterial3D> material;
    world::Affine3D mesh_transform;
    std::string mesh_cast_shadow = "on"; // as MeshInstance3D's cast_shadow
    // Each placed by its position and rotation in the cell's space; disabled ones are left out.
    std::vector<world::CollisionShape3D> shapes;
    std::string navigation_mesh; // a .navmesh.json file, or ""
    world::Affine3D navigation_mesh_transform;
    std::uint32_t navigation_layers = 1;
};

// A .meshlib.json file: items by name.
struct MeshLibrary {
    std::map<std::string, MeshLibraryItem, std::less<>> items;
};

// The JSON Schema for .meshlib.json files.
[[nodiscard]] json::Value mesh_library_schema();

// Every error in a document, each with its JSON Pointer: those of the schema, item names with a
// ':' and navigation meshes that aren't .navmesh.json files.
[[nodiscard]] std::vector<json::ValidationError> validate_mesh_library(const json::Value& doc);

// The library in a document, or its first error.
[[nodiscard]] Result<MeshLibrary> parse_mesh_library(const json::Value& doc);

// Registers .meshlib.json files with assets as MeshLibrary. Called by add_asset_types.
void add_mesh_library_type(asset::Assets& assets);

// ---- Cells

// The ways a cell's item can be turned: Godot's 24 orthogonal bases.
inline constexpr int kOrientations = 24;

// Godot's GridMap::get_basis_with_orthogonal_index; no turn for an index out of range. 0 is
// none, and 16, 10 and 22 turn an item 90, 180 and 270 degrees about y.
[[nodiscard]] world::Affine3D orientation_basis(int orientation);

// Godot's get_orthogonal_index_from_basis: the orientation whose basis is the given one's
// entries rounded to -1, 0 or 1, or 0 when none is.
[[nodiscard]] int orientation_index(const world::Affine3D& basis);

// What a cell holds: an item of the library and how it is turned.
struct GridItem {
    std::string item;
    int orientation = 0;

    friend bool operator==(const GridItem&, const GridItem&) = default;
};

// Cells by floor (y), then row (z), then column (x).
struct GridCellOrder {
    bool operator()(Vec3i a, Vec3i b) const {
        return std::tie(a.y, a.z, a.x) < std::tie(b.y, b.z, b.x);
    }
};
using GridCells = std::map<Vec3i, GridItem, GridCellOrder>;

// Where cells can be, as in Godot, whose cell keys are 16-bit: from -32768 to 32767 each way.
inline constexpr int kMinGridCell = -32768;
inline constexpr int kMaxGridCell = 32767;

// A legend value: the item's name, then ":" and the orientation when it isn't 0.
[[nodiscard]] std::string grid_item_text(const GridItem& item);
[[nodiscard]] Result<GridItem> parse_grid_item(std::string_view text);

// A GridMap's cells. Legend keys are as a TileMapLayer's (world/legend.hpp), floors are each at
// a different y, and each row of a floor is a string of keys for the cells from its origin
// toward +x, the rows going toward +z. The error message starts with the JSON Pointer of the
// bad value within the component, as "/floors/1/rows/2: ...". Names aren't looked up in the
// library: check_gridmaps reports those that aren't there.
[[nodiscard]] Result<GridCells> decode_grid_cells(const world::GridMap& map);

// Every error decode_grid_cells would find, each with its JSON Pointer within the component.
[[nodiscard]] std::vector<json::ValidationError> validate_grid_cells(const world::GridMap& map);

// Writes cells into a GridMap's legend and floors in canonical form: a floor for each y in use,
// lowest first, its origin the corner of its cells nearest -x and -z, rows ending at their last
// item; keys keep standing for what they stood for, and new keys go to the most used items
// first, each the first letter of the item's name where that is free.
void encode_grid_cells(const GridCells& cells, world::GridMap& map);

// Godot's map_to_local: the center of a cell (or its corner along axes not centered) in the
// GridMap's space; and local_to_map, the cell a point is in.
[[nodiscard]] Vec3 map_to_local(const world::GridMap& map, Vec3i cell);
[[nodiscard]] Vec3i local_to_map(const world::GridMap& map, Vec3 local);

// Where a cell's item goes in the GridMap's space, as _octant_update places it: turned by its
// orientation, scaled by cell_scale, at map_to_local.
[[nodiscard]] world::Affine3D cell_transform(const world::GridMap& map, Vec3i cell,
                                             int orientation);

// The octant a cell is in, as Godot's get_octant_key_from_cell_coords.
[[nodiscard]] Vec3i octant_of(const world::GridMap& map, Vec3i cell);

// ---- What a GridMap makes

// A surface of an octant and which shadows it casts, its items' mesh_cast_shadow.
struct GridSurface {
    MeshSurface mesh;
    std::string cast_shadow = "on";
};

// One octant's items, placed in the GridMap's space and merged into one surface per material
// and shadow setting, as Godot draws an octant's MultiMesh of each item.
struct GridOctant {
    Vec3i key;
    // A number no other octant made in this process has, for keeping its meshes on a device.
    std::uint64_t version = 0;
    std::vector<GridSurface> surfaces;
    std::size_t triangles = 0;
};

// An item's shape in a cell.
struct GridShape {
    Vec3i cell;
    world::CollisionShape3D shape;
    world::Affine3D local; // in the GridMap's space: the cell's transform times the shape's
};

// An item's navigation mesh in a cell, as a NavigationCell of Godot's octants.
struct GridRegion {
    Vec3i cell;
    std::string navigation_mesh;
    world::Affine3D local; // the cell's transform times the item's navigation_mesh_transform
    std::uint32_t navigation_layers = 1;
};

// What a GridMap's cells make.
struct GridMesh {
    std::vector<std::shared_ptr<const GridOctant>> octants; // in octant order
    // Every cell's shapes: a change in any is a new pointer.
    std::shared_ptr<const std::vector<GridShape>> shapes;
    std::vector<GridRegion> regions;
    std::size_t cells = 0;
    std::size_t triangles = 0;
    // Why cells can't be made: a library that doesn't load, cells that don't decode, items the
    // library doesn't have, and item meshes that can't be made.
    std::vector<Problem> problems;
};

// What a GridMap's cells make. With install_gridmap it is kept until the GridMap, its library
// or a model an item names changes, and octants whose cells are the same are kept; without, it
// is made on each call, the library and models read with the world's file reader.
[[nodiscard]] std::shared_ptr<const GridMesh> grid_mesh(const world::World& world,
                                                        entt::entity entity);

// Reads mesh libraries by project path.
using MeshLibraryLoader =
    std::function<Result<std::shared_ptr<const MeshLibrary>>(const std::string& path)>;

// Keeps each GridMap's meshes in the world until they change, reading item models with models
// and libraries with libraries. Sessions and games call it through canvas::install.
void install_gridmap(world::World& world, ModelLoader models, MeshLibraryLoader libraries);

// A GridMap with shapes, as the static bodies of its octants: its collision layer, mask and
// physics material, and every cell's shapes.
struct GridBody {
    entt::entity entity = entt::null;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    std::shared_ptr<const std::vector<GridShape>> shapes;
};
[[nodiscard]] std::vector<GridBody> grid_bodies(const world::World& world);

// Every GridMap whose cells can't be made, in the order entities were made: a cell_size Godot
// can't divide by, cells but no library, and grid_mesh's problems.
[[nodiscard]] std::vector<Problem> check_gridmaps(const world::World& world);

} // namespace slash::scene3d
