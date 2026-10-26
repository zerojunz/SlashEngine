#include "slash/scene3d/gridmap.hpp"

#include "slash/core/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/legend.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>

namespace slash::scene3d {

namespace {

constexpr std::string_view kNavigationMeshSuffix = ".navmesh.json";
constexpr std::size_t kMaxItemName = 64;

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return Error{ErrorCode::InvalidArgument,
                 (first.path.empty() ? std::string("root") : first.path) + ": " + first.message};
}

// The engine's own components, whose schemas items' primitives, materials and shapes follow.
const world::ComponentRegistry& builtin_components() {
    static const std::unique_ptr<const world::ComponentRegistry> registry = [] {
        auto made = std::make_unique<world::ComponentRegistry>();
        world::register_builtin_components(*made);
        return std::unique_ptr<const world::ComponentRegistry>(std::move(made));
    }();
    return *registry;
}

json::Value vec3_schema(std::string_view description, Vec3 fallback) {
    json::Value out;
    out["type"] = "array";
    out["items"]["type"] = "number";
    out["minItems"] = 3;
    out["maxItems"] = 3;
    out["default"] = json::Array{fallback.x, fallback.y, fallback.z};
    out["description"] = description;
    return out;
}

json::Value transform_schema(std::string_view description) {
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = description;
    out["properties"]["position"] = vec3_schema("Meters, as [x, y, z].", {});
    out["properties"]["rotation"] =
        vec3_schema("Radians about each axis, as Transform3D's (YXZ order).", {});
    out["properties"]["scale"] = vec3_schema("As [x, y, z].", {1.0F, 1.0F, 1.0F});
    return out;
}

json::Value make_schema() {
    const world::ComponentRegistry& components = builtin_components();
    world::SchemaBuilder builder;
    const auto ref = [&](std::string_view name) { return components.find(name)->schema(builder); };

    json::Value primitive;
    primitive["type"] = "object";
    primitive["additionalProperties"] = false;
    primitive["minProperties"] = 1;
    primitive["maxProperties"] = 1;
    primitive["description"] =
        R"(One primitive mesh by its component's name, such as {"BoxMesh": {"size": [2, 2, 2]}}.)";
    for (const std::string_view name : kPrimitiveNames) {
        primitive["properties"][name] = ref(name);
    }

    json::Value item;
    item["type"] = "object";
    item["additionalProperties"] = false;
    item["description"] = "What a cell holding the item draws, collides as and walks on, in the "
                          "cell's space, as Godot's MeshLibrary item.";
    json::Value& properties = item["properties"];
    properties["mesh"]["type"] = "string";
    properties["mesh"]["description"] = "A .gltf, .glb, .obj, .fbx, .blend or .model.json file's "
                                        "meshes, at rest; or use primitive.";
    properties["primitive"] = std::move(primitive);
    properties["material"] = ref("StandardMaterial3D");
    properties["material"]["description"] =
        "Takes the place of the model's materials, or colors the primitive.";
    properties["mesh_transform"] = transform_schema("Places the mesh in the cell.");
    properties["mesh_cast_shadow"]["enum"] =
        json::Array{"off", "on", "double_sided", "shadows_only"};
    properties["mesh_cast_shadow"]["default"] = "on";
    properties["mesh_cast_shadow"]["description"] = "As MeshInstance3D's cast_shadow.";
    properties["shapes"]["type"] = "array";
    properties["shapes"]["items"] = ref("CollisionShape3D");
    properties["shapes"]["description"] =
        "What the cell collides as, each placed by its position and rotation.";
    properties["navigation_mesh"]["type"] = "string";
    properties["navigation_mesh"]["description"] =
        "A .navmesh.json file: a navigation region in each cell when the GridMap bakes "
        "navigation.";
    properties["navigation_mesh_transform"] =
        transform_schema("Places the navigation mesh in the cell.");
    properties["navigation_layers"]["type"] = "integer";
    properties["navigation_layers"]["minimum"] = 0;
    properties["navigation_layers"]["maximum"] = 4294967295LL;
    properties["navigation_layers"]["default"] = 1;
    properties["navigation_layers"]["description"] = "The navigation layers its regions are in.";

    json::Value names;
    names["minLength"] = 1;
    names["maxLength"] = static_cast<std::int64_t>(kMaxItemName);

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine mesh library";
    schema["description"] = "Items a GridMap places in its cells, by name, as Godot's MeshLibrary.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    schema["properties"]["format"]["const"] = 1;
    schema["properties"]["format"]["description"] = "Format version. Must be 1.";
    schema["properties"]["items"]["type"] = "object";
    schema["properties"]["items"]["description"] =
        "Items by name: 1 to 64 characters, no ':', such as \"wall\".";
    schema["properties"]["items"]["propertyNames"] = std::move(names);
    schema["properties"]["items"]["additionalProperties"] =
        world::SchemaBuilder::reference("MeshLibraryItem");
    json::Value definitions = builder.definitions();
    definitions["MeshLibraryItem"] = std::move(item);
    schema["$defs"] = std::move(definitions);
    return schema;
}

const json::Value& library_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

Vec3 vec3_of(const json::Value& value) {
    const json::Array& xyz = value.as_array();
    return {static_cast<float>(xyz.at(0).as_double()), static_cast<float>(xyz.at(1).as_double()),
            static_cast<float>(xyz.at(2).as_double())};
}

world::Affine3D transform_of(const json::Value* value) {
    if (value == nullptr) {
        return {};
    }
    world::Transform3D transform;
    if (const json::Value* position = value->find("position"); position != nullptr) {
        transform.position = vec3_of(*position);
    }
    if (const json::Value* rotation = value->find("rotation"); rotation != nullptr) {
        transform.rotation = vec3_of(*rotation);
    }
    if (const json::Value* scale = value->find("scale"); scale != nullptr) {
        transform.scale = vec3_of(*scale);
    }
    return world::affine(transform);
}

// The primitive a {"<name>": {fields}} object names.
template <std::size_t I = 0>
Result<Primitive> primitive_from(world::World& scratch, std::string_view name,
                                 const json::Value& fields, std::string& path) {
    if constexpr (I < std::variant_size_v<Primitive>) {
        if (name == kPrimitiveNames[I]) {
            std::variant_alternative_t<I, Primitive> out;
            if (auto read = world::from_json(scratch, fields, out, path); !read) {
                return read.error();
            }
            return Primitive{std::in_place_index<I>, out};
        }
        return primitive_from<I + 1>(scratch, name, fields, path);
    } else {
        return make_error(ErrorCode::InvalidArgument, quoted(name) + " is not a primitive mesh");
    }
}

// Reads a document the schema let through into out, if given, collecting what the schema can't
// catch.
void read_items(const json::Value& doc, MeshLibrary* out,
                std::vector<json::ValidationError>& errors) {
    const json::Value* items = doc.find("items");
    if (items == nullptr) {
        return;
    }
    world::World scratch(builtin_components());
    for (const auto& [name, value] : items->as_object()) {
        const std::string at = "/items/" + json::pointer_escape(name);
        if (name.find(':') != std::string::npos) {
            errors.push_back({at, "an item's name has no ':', which comes before an orientation "
                                  "in a GridMap's legend"});
        }
        MeshLibraryItem item;
        if (const json::Value* mesh = value.find("mesh"); mesh != nullptr) {
            item.mesh = mesh->as_string();
            if (!item.mesh.empty() && !is_model_path(item.mesh)) {
                errors.push_back(
                    {at + "/mesh", "is not a .gltf, .glb, .obj, .fbx, .blend or .model.json file"});
            }
        }
        if (const json::Value* primitive = value.find("primitive"); primitive != nullptr) {
            const auto& [type, fields] = *primitive->as_object().begin();
            std::string path;
            auto made = primitive_from(scratch, type, fields, path);
            const std::string primitive_at =
                std::string(at).append("/primitive/").append(json::pointer_escape(type));
            if (!made) {
                errors.push_back({primitive_at + path, made.error().message});
            } else if (auto checked = check_primitive(made.value()); !checked) {
                errors.push_back({primitive_at, checked.error().message});
            } else {
                item.primitive = std::move(made).value();
            }
            if (!item.mesh.empty()) {
                errors.push_back({at, "has both a mesh and a primitive: keep one"});
            }
        }
        if (const json::Value* material = value.find("material"); material != nullptr) {
            world::StandardMaterial3D standard;
            std::string path;
            if (auto read = world::from_json(scratch, *material, standard, path); !read) {
                errors.push_back(
                    {std::string(at).append("/material").append(path), read.error().message});
            } else {
                item.material = std::move(standard);
            }
        }
        item.mesh_transform = transform_of(value.find("mesh_transform"));
        if (const json::Value* cast = value.find("mesh_cast_shadow"); cast != nullptr) {
            item.mesh_cast_shadow = cast->as_string();
        }
        if (const json::Value* shapes = value.find("shapes"); shapes != nullptr) {
            const json::Array& list = shapes->as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                world::CollisionShape3D shape;
                std::string path;
                const std::string shape_at =
                    std::string(at).append("/shapes/").append(std::to_string(i));
                if (auto read = world::from_json(scratch, list[i], shape, path); !read) {
                    errors.push_back({shape_at + path, read.error().message});
                } else if (shape.shape == "world_boundary") {
                    errors.push_back({shape_at + "/shape",
                                      "a world boundary can't be in a cell: it is infinite"});
                } else {
                    item.shapes.push_back(std::move(shape));
                }
            }
        }
        if (const json::Value* navigation = value.find("navigation_mesh"); navigation != nullptr) {
            item.navigation_mesh = navigation->as_string();
            if (!item.navigation_mesh.empty() &&
                !item.navigation_mesh.ends_with(kNavigationMeshSuffix)) {
                errors.push_back({at + "/navigation_mesh", "is not a .navmesh.json file"});
            }
        }
        item.navigation_mesh_transform = transform_of(value.find("navigation_mesh_transform"));
        if (const json::Value* layers = value.find("navigation_layers"); layers != nullptr) {
            item.navigation_layers = static_cast<std::uint32_t>(layers->as_int());
        }
        if (out != nullptr) {
            out->items.emplace(name, std::move(item));
        }
    }
}

std::size_t library_bytes(const MeshLibrary& library) {
    std::size_t bytes = sizeof(MeshLibrary);
    for (const auto& [name, item] : library.items) {
        bytes += world::kMapNodeBytes<decltype(library.items)> + json::heap_bytes(name) +
                 json::heap_bytes(item.mesh) + json::heap_bytes(item.navigation_mesh) +
                 (item.shapes.size() * sizeof(world::CollisionShape3D));
        for (const world::CollisionShape3D& shape : item.shapes) {
            bytes += (shape.points.size() + shape.faces.size()) * sizeof(Vec3);
        }
    }
    return bytes;
}

// Godot's _ortho_bases, each as its rows.
constexpr std::array<std::array<float, 9>, kOrientations> kOrthoBases = {{
    {1, 0, 0, 0, 1, 0, 0, 0, 1},    {0, -1, 0, 1, 0, 0, 0, 0, 1},  {-1, 0, 0, 0, -1, 0, 0, 0, 1},
    {0, 1, 0, -1, 0, 0, 0, 0, 1},   {1, 0, 0, 0, 0, -1, 0, 1, 0},  {0, 0, 1, 1, 0, 0, 0, 1, 0},
    {-1, 0, 0, 0, 0, 1, 0, 1, 0},   {0, 0, -1, -1, 0, 0, 0, 1, 0}, {1, 0, 0, 0, -1, 0, 0, 0, -1},
    {0, 1, 0, 1, 0, 0, 0, 0, -1},   {-1, 0, 0, 0, 1, 0, 0, 0, -1}, {0, -1, 0, -1, 0, 0, 0, 0, -1},
    {1, 0, 0, 0, 0, 1, 0, -1, 0},   {0, 0, -1, 1, 0, 0, 0, -1, 0}, {-1, 0, 0, 0, 0, -1, 0, -1, 0},
    {0, 0, 1, -1, 0, 0, 0, -1, 0},  {0, 0, 1, 0, 1, 0, -1, 0, 0},  {0, -1, 0, 0, 0, 1, -1, 0, 0},
    {0, 0, -1, 0, -1, 0, -1, 0, 0}, {0, 1, 0, 0, 0, -1, -1, 0, 0}, {0, 0, 1, 0, -1, 0, 1, 0, 0},
    {0, 1, 0, 0, 0, 1, 1, 0, 0},    {0, 0, -1, 0, 1, 0, 1, 0, 0},  {0, -1, 0, 0, 0, -1, 1, 0, 0},
}};

// Decodes a GridMap's cells into out, if given, collecting errors; stops at the first if asked.
void decode(const world::GridMap& map, GridCells* out, std::vector<json::ValidationError>& errors,
            bool first_only) {
    const auto stop = [&] { return first_only && !errors.empty(); };
    std::string_view first_key;
    std::map<std::string, std::optional<GridItem>, std::less<>> keys;
    bool lengths_ok = true;
    for (const auto& [key, value] : map.legend) {
        const std::string at = "/legend/" + json::pointer_escape(key);
        if (!world::valid_legend_key(key)) {
            errors.push_back({at, "a key is one to three letters, digits or punctuation marks "
                                  "other than '.', '\"' and backslash"});
            lengths_ok = false;
        } else if (first_key.empty()) {
            first_key = key;
        } else if (key.size() != first_key.size()) {
            errors.push_back({at, "keys are all the same length: " + quoted(key) + " is " +
                                      std::to_string(key.size()) + " characters, " +
                                      quoted(first_key) + " " + std::to_string(first_key.size())});
            lengths_ok = false;
        }
        auto item = parse_grid_item(value);
        if (!item) {
            errors.push_back({at, item.error().message});
            keys.emplace(key, std::nullopt);
        } else {
            keys.emplace(key, std::move(item).value());
        }
        if (stop()) {
            return;
        }
    }
    if (!lengths_ok) {
        return; // the rows can't be split into keys
    }
    const std::size_t length = std::max<std::size_t>(first_key.size(), 1);
    std::set<int> heights;
    for (std::size_t f = 0; f < map.floors.size(); ++f) {
        const world::GridFloor& floor = map.floors[f];
        const std::string at = "/floors/" + std::to_string(f);
        if (floor.y < kMinGridCell || floor.y > kMaxGridCell) {
            errors.push_back({at + "/y", "cells are from -32768 to 32767 in each direction"});
        } else if (floor.origin.x < kMinGridCell || floor.origin.x > kMaxGridCell ||
                   floor.origin.y < kMinGridCell || floor.origin.y > kMaxGridCell) {
            errors.push_back({at + "/origin", "cells are from -32768 to 32767 in each direction"});
        } else if (!heights.insert(floor.y).second) {
            errors.push_back({at + "/y", "another floor is at y " + std::to_string(floor.y) +
                                             ": put their rows in one floor"});
        } else {
            for (std::size_t i = 0; i < floor.rows.size(); ++i) {
                const std::string& row = floor.rows[i];
                const std::string row_at = at + "/rows/" + std::to_string(i);
                if (row.size() % length != 0) {
                    errors.push_back({row_at, "is " + std::to_string(row.size()) +
                                                  " characters, not a whole number of " +
                                                  std::to_string(length) + "-character keys"});
                } else if (static_cast<std::int64_t>(floor.origin.y) +
                                   static_cast<std::int64_t>(i) >
                               kMaxGridCell ||
                           static_cast<std::int64_t>(floor.origin.x) +
                                   static_cast<std::int64_t>(row.size() / length) - 1 >
                               kMaxGridCell) {
                    errors.push_back({row_at, "reaches past 32767, the last cell along x or z"});
                } else {
                    for (std::size_t column = 0; column * length < row.size(); ++column) {
                        const std::string_view key =
                            std::string_view(row).substr(column * length, length);
                        if (world::empty_legend_key(key)) {
                            continue;
                        }
                        const auto it = keys.find(key);
                        if (it == keys.end()) {
                            errors.push_back({row_at, "column " + std::to_string(column) + ": " +
                                                          quoted(key) + " is not in the legend"});
                            break;
                        }
                        if (out != nullptr && it->second) {
                            out->insert_or_assign(Vec3i{floor.origin.x + static_cast<int>(column),
                                                        floor.y,
                                                        floor.origin.y + static_cast<int>(i)},
                                                  *it->second);
                        }
                    }
                }
                if (stop()) {
                    return;
                }
            }
        }
        if (stop()) {
            return;
        }
    }
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

std::array<float, 3> normalized(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    if (length == 0.0F) {
        return {v.x, v.y, v.z};
    }
    return {v.x / length, v.y / length, v.z / length};
}

// Appends triangles to a surface, placed by transform: normals by its inverse transpose,
// tangents by its basis, and, where it mirrors them, triangles turned the other way round so
// their front faces stay in front.
void append(MeshSurface& to, const std::vector<render::Vertex3D>& vertices,
            const std::vector<std::uint32_t>& indices, const world::Affine3D& transform) {
    const world::Affine3D inverse = transform.inverse();
    const bool mirrored = dot(transform.x, cross(transform.y, transform.z)) < 0.0F;
    const auto base = static_cast<std::uint32_t>(to.vertices.size());
    to.vertices.reserve(to.vertices.size() + vertices.size());
    for (const render::Vertex3D& vertex : vertices) {
        render::Vertex3D out = vertex;
        const Vec3 p =
            transform.apply({vertex.position[0], vertex.position[1], vertex.position[2]});
        out.position = {p.x, p.y, p.z};
        const Vec3 n{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
        out.normal = normalized({dot(inverse.x, n), dot(inverse.y, n), dot(inverse.z, n)});
        const std::array<float, 3> t =
            normalized(transform.basis({vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}));
        out.tangent = {t[0], t[1], t[2], mirrored ? -vertex.tangent[3] : vertex.tangent[3]};
        to.vertices.push_back(out);
    }
    to.indices.reserve(to.indices.size() + indices.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        to.indices.push_back(base + indices[i]);
        to.indices.push_back(base + indices[mirrored ? i + 2 : i + 1]);
        to.indices.push_back(base + indices[mirrored ? i + 1 : i + 2]);
    }
}

// An item's mesh in its cell's space, placed by its mesh_transform, one surface per material.
struct ItemMesh {
    std::vector<GridSurface> surfaces;
    std::string error; // why it can't be made
};

// A model loaded for an item, or why it didn't load.
struct LoadedModel {
    std::shared_ptr<const Model> model;
    std::string error;

    friend bool operator==(const LoadedModel&, const LoadedModel&) = default;
};
using LoadedModels = std::map<std::string, LoadedModel, std::less<>>;

ItemMesh item_mesh(const MeshLibraryItem& item, const LoadedModels& models) {
    ItemMesh out;
    const auto surface_for = [&](const SurfaceMaterial& material) -> MeshSurface& {
        for (GridSurface& surface : out.surfaces) {
            if (surface.mesh.material == material) {
                return surface.mesh;
            }
        }
        GridSurface& made = out.surfaces.emplace_back();
        made.mesh.material = material;
        made.cast_shadow = item.mesh_cast_shadow;
        return made.mesh;
    };
    if (item.primitive) {
        auto made = make_mesh(*item.primitive);
        if (!made) {
            out.error = made.error().message;
            return out;
        }
        SurfaceMaterial material;
        material.standard = item.material;
        append(surface_for(material), made.value().vertices, made.value().indices,
               item.mesh_transform);
        return out;
    }
    if (item.mesh.empty()) {
        return out;
    }
    const auto found = models.find(item.mesh);
    if (found == models.end() || found->second.model == nullptr) {
        out.error = found != models.end() ? found->second.error : "it didn't load";
        return out;
    }
    const std::shared_ptr<const Model>& model = found->second.model;
    for (const ModelPart& part : model->parts) {
        // A skin's surfaces are where its joints put them, which is where they are at rest.
        const world::Affine3D place =
            item.mesh_transform * (part.skin >= 0 ? world::Affine3D{} : part.transform);
        for (const ModelSurface& surface : model->meshes[part.mesh].surfaces) {
            SurfaceMaterial material;
            if (item.material) {
                material.standard = item.material;
            } else if (surface.material >= 0) {
                material.model_path = item.mesh;
                material.model = model;
                material.model_material = surface.material;
            }
            append(surface_for(material), surface.vertices, surface.indices, place);
        }
    }
    return out;
}

using ItemMeshes = std::map<std::string, ItemMesh, std::less<>>;

std::atomic<std::uint64_t> next_version{1};

std::size_t octant_bytes(const GridOctant& octant) {
    std::size_t bytes = sizeof(GridOctant);
    for (const GridSurface& surface : octant.surfaces) {
        bytes += (surface.mesh.vertices.size() * sizeof(render::Vertex3D)) +
                 (surface.mesh.indices.size() * sizeof(std::uint32_t));
    }
    return bytes;
}

// The cells of one octant, in cell order.
using OctantCells = std::vector<std::pair<Vec3i, GridItem>>;

std::shared_ptr<const GridOctant> make_octant(const world::GridMap& map, Vec3i key,
                                              const OctantCells& cells, const ItemMeshes& meshes,
                                              entt::entity entity) {
    auto octant = std::make_shared<GridOctant>();
    octant->key = key;
    octant->version = next_version.fetch_add(1);
    for (const auto& [cell, held] : cells) {
        const auto found = meshes.find(held.item);
        if (found == meshes.end() || found->second.surfaces.empty()) {
            continue;
        }
        const world::Affine3D transform = cell_transform(map, cell, held.orientation);
        for (const GridSurface& from : found->second.surfaces) {
            GridSurface* to = nullptr;
            for (GridSurface& surface : octant->surfaces) {
                if (surface.mesh.material == from.mesh.material &&
                    surface.cast_shadow == from.cast_shadow) {
                    to = &surface;
                    break;
                }
            }
            if (to == nullptr) {
                to = &octant->surfaces.emplace_back();
                to->mesh.material = from.mesh.material;
                to->mesh.material.entity = entity;
                to->cast_shadow = from.cast_shadow;
            }
            append(to->mesh, from.mesh.vertices, from.mesh.indices, transform);
        }
    }
    for (const GridSurface& surface : octant->surfaces) {
        octant->triangles += surface.mesh.indices.size() / 3;
    }
    return octant;
}

struct Vec3iOrder {
    bool operator()(Vec3i a, Vec3i b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

// What a GridMap's meshes are made from besides its component.
struct Sources {
    std::shared_ptr<const MeshLibrary> library;
    std::string library_error;
    LoadedModels models; // by path, those the library's items name

    friend bool operator==(const Sources&, const Sources&) = default;
};

Sources sources_of(const world::GridMap& map, const MeshLibraryLoader& libraries,
                   const ModelLoader& models) {
    Sources out;
    if (map.mesh_library.empty()) {
        return out;
    }
    if (!map.mesh_library.ends_with(kMeshLibrarySuffix)) {
        out.library_error = "is not a .meshlib.json file";
        return out;
    }
    auto loaded = libraries(map.mesh_library);
    if (!loaded) {
        out.library_error = loaded.error().message;
        return out;
    }
    out.library = std::move(loaded).value();
    for (const auto& [name, item] : out.library->items) {
        if (item.mesh.empty() || out.models.contains(item.mesh)) {
            continue;
        }
        auto model = models(item.mesh);
        out.models.emplace(item.mesh,
                           model ? LoadedModel{.model = model.value(), .error = {}}
                                 : LoadedModel{.model = nullptr, .error = model.error().message});
    }
    return out;
}

// What a GridMap made last, kept to tell what changed: the octants whose cells are the same and
// the item meshes, when its library and models are.
struct Made {
    world::GridMap map;
    Sources sources;
    std::shared_ptr<const ItemMeshes> items;
    std::map<Vec3i, std::pair<OctantCells, std::shared_ptr<const GridOctant>>, Vec3iOrder> octants;
    std::shared_ptr<const GridMesh> mesh;
};

// Whether two GridMaps place items in cells the same way, so octants with the same cells are
// the same.
bool same_placement(const world::GridMap& a, const world::GridMap& b) {
    return a.cell_size == b.cell_size && a.cell_octant_size == b.cell_octant_size &&
           a.cell_center_x == b.cell_center_x && a.cell_center_y == b.cell_center_y &&
           a.cell_center_z == b.cell_center_z && a.cell_scale == b.cell_scale;
}

constexpr std::string_view kField = "/components/GridMap";

// Makes a GridMap's meshes, reusing what the last make (if any) holds where it can.
Made make(const world::GridMap& map, entt::entity entity, Sources sources, const Made* last) {
    Made made;
    made.map = map;
    made.sources = std::move(sources);
    auto mesh = std::make_shared<GridMesh>();
    const std::string field(kField);
    if (map.cell_size.x < 0.001F || map.cell_size.y < 0.001F || map.cell_size.z < 0.001F) {
        mesh->problems.push_back(
            {entity, field + "/cell_size", "a cell is at least 0.001 meters along each axis"});
    }
    if (!made.sources.library_error.empty()) {
        mesh->problems.push_back({entity, field + "/mesh_library", made.sources.library_error});
    }
    GridCells cells;
    std::vector<json::ValidationError> errors;
    decode(map, &cells, errors, false);
    for (json::ValidationError& error : errors) {
        mesh->problems.push_back({entity, field + error.path, std::move(error.message)});
    }
    if (!cells.empty() && map.mesh_library.empty()) {
        mesh->problems.push_back({entity, field + "/mesh_library",
                                  "names no mesh library, so its cells hold nothing: name a "
                                  ".meshlib.json file"});
    }
    const MeshLibrary* library = made.sources.library.get();
    if (library != nullptr) {
        std::vector<std::string_view> names;
        names.reserve(library->items.size());
        for (const auto& [name, item] : library->items) {
            names.push_back(name);
        }
        for (const auto& [key, value] : map.legend) {
            auto item = parse_grid_item(value);
            if (item && !library->items.contains(item.value().item)) {
                std::string message =
                    quoted(item.value().item) + " is not an item of " + quoted(map.mesh_library);
                if (const std::string_view near = closest_match(item.value().item, names);
                    !near.empty()) {
                    message += "; did you mean " + quoted(near) + "?";
                }
                mesh->problems.push_back(
                    {entity, field + "/legend/" + json::pointer_escape(key), std::move(message)});
            }
        }
    }
    // Item meshes are made again only when the library or a model is new.
    if (last != nullptr && last->items != nullptr && last->sources == made.sources) {
        made.items = last->items;
    } else {
        auto items = std::make_shared<ItemMeshes>();
        if (library != nullptr) {
            for (const auto& [name, item] : library->items) {
                items->emplace(name, item_mesh(item, made.sources.models));
            }
        }
        made.items = std::move(items);
    }
    std::set<std::string, std::less<>> used;
    for (const auto& [cell, held] : cells) {
        used.insert(held.item);
    }
    for (const std::string& name : used) {
        const auto found = made.items->find(name);
        if (found != made.items->end() && !found->second.error.empty()) {
            mesh->problems.push_back({entity, field + "/mesh_library",
                                      "item " + quoted(name) + "'s mesh: " + found->second.error});
        }
    }

    std::map<Vec3i, OctantCells, Vec3iOrder> by_octant;
    auto shapes = std::make_shared<std::vector<GridShape>>();
    for (const auto& [cell, held] : cells) {
        by_octant[octant_of(map, cell)].emplace_back(cell, held);
        if (library == nullptr) {
            continue;
        }
        const auto item = library->items.find(held.item);
        if (item == library->items.end()) {
            continue;
        }
        const world::Affine3D transform = cell_transform(map, cell, held.orientation);
        for (const world::CollisionShape3D& shape : item->second.shapes) {
            if (shape.disabled) {
                continue;
            }
            world::Transform3D local;
            local.position = shape.position;
            local.rotation = shape.rotation;
            shapes->push_back(
                {.cell = cell, .shape = shape, .local = transform * world::affine(local)});
        }
        if (!item->second.navigation_mesh.empty()) {
            mesh->regions.push_back({.cell = cell,
                                     .navigation_mesh = item->second.navigation_mesh,
                                     .local = transform * item->second.navigation_mesh_transform,
                                     .navigation_layers = item->second.navigation_layers});
        }
    }
    const bool reuse =
        last != nullptr && last->items == made.items && same_placement(last->map, map);
    for (auto& [key, octant_cells] : by_octant) {
        std::shared_ptr<const GridOctant> octant;
        if (reuse) {
            if (const auto found = last->octants.find(key);
                found != last->octants.end() && found->second.first == octant_cells) {
                octant = found->second.second;
            }
        }
        if (octant == nullptr) {
            octant = make_octant(map, key, octant_cells, *made.items, entity);
        }
        mesh->triangles += octant->triangles;
        if (!octant->surfaces.empty()) {
            mesh->octants.push_back(octant);
        }
        made.octants.emplace(key, std::make_pair(std::move(octant_cells), std::move(octant)));
    }
    mesh->cells = cells.size();
    mesh->shapes = std::move(shapes);
    made.mesh = std::move(mesh);
    return made;
}

// The meshes of a world's GridMaps, kept until they change.
struct GridCache {
    std::mutex mutex;
    ModelLoader models;
    MeshLibraryLoader libraries;
    std::unordered_map<entt::entity, Made> entries;

    world::MemoryPart memory() {
        const std::scoped_lock lock(mutex);
        world::MemoryPart part{"gridmap", 0, {}, {}};
        std::uint64_t octants = 0;
        std::uint64_t cells = 0;
        std::uint64_t triangles = 0;
        for (const auto& [entity, entry] : entries) {
            for (const auto& octant : entry.mesh->octants) {
                part.bytes += octant_bytes(*octant);
            }
            part.bytes += entry.mesh->shapes->size() * sizeof(GridShape);
            octants += entry.mesh->octants.size();
            cells += entry.mesh->cells;
            triangles += entry.mesh->triangles;
        }
        part.counts = {{"gridmaps", entries.size()},
                       {"octants", octants},
                       {"cells", cells},
                       {"triangles", triangles}};
        return part;
    }
};

GridCache* find_cache(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<GridCache>>();
    return found != nullptr ? found->get() : nullptr;
}

Result<std::string> read_file(const world::World& world, const std::string& path) {
    const world::FileReader& reader = world.file_reader();
    if (!reader) {
        return Error{.code = ErrorCode::NotFound, .message = "files can't be read here"};
    }
    auto bytes = reader(path);
    if (!bytes) {
        return Error{.code = bytes.error().code,
                     .message = "\"" + path + "\" could not be read: " + bytes.error().message};
    }
    return bytes;
}

// Reads models and libraries through the world's file reader, for worlds without
// install_gridmap.
ModelLoader file_models(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const Model>> {
        auto bytes = read_file(world, path);
        if (!bytes) {
            return bytes.error();
        }
        auto model = load_model(bytes.value(), path, world.file_reader());
        if (!model) {
            return model.error();
        }
        return std::make_shared<const Model>(std::move(model).value());
    };
}

MeshLibraryLoader file_libraries(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const MeshLibrary>> {
        auto bytes = read_file(world, path);
        if (!bytes) {
            return bytes.error();
        }
        auto doc = json::parse(bytes.value());
        if (!doc) {
            return doc.error();
        }
        auto library = parse_mesh_library(doc.value());
        if (!library) {
            return library.error();
        }
        return std::make_shared<const MeshLibrary>(std::move(library).value());
    };
}

std::shared_ptr<const GridMesh> empty_mesh() {
    auto empty = std::make_shared<GridMesh>();
    empty->shapes = std::make_shared<const std::vector<GridShape>>();
    return empty;
}

std::vector<entt::entity> gridmaps(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const world::GridMap>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

} // namespace

json::Value mesh_library_schema() {
    return library_schema();
}

std::vector<json::ValidationError> validate_mesh_library(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, library_schema());
    if (!errors.empty()) {
        return errors;
    }
    read_items(doc, nullptr, errors);
    return errors;
}

Result<MeshLibrary> parse_mesh_library(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, library_schema());
    if (!errors.empty()) {
        return first_error(errors);
    }
    MeshLibrary library;
    read_items(doc, &library, errors);
    if (!errors.empty()) {
        return first_error(errors);
    }
    return library;
}

void add_mesh_library_type(asset::Assets& assets) {
    assets.add_type<MeshLibrary>(
        "mesh_library", {std::string(kMeshLibrarySuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<MeshLibrary> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_mesh_library(doc.value());
        },
        library_bytes);
}

world::Affine3D orientation_basis(int orientation) {
    if (orientation < 0 || orientation >= kOrientations) {
        return {};
    }
    const std::array<float, 9>& r = kOrthoBases[static_cast<std::size_t>(orientation)];
    return {
        .x = {r[0], r[3], r[6]}, .y = {r[1], r[4], r[7]}, .z = {r[2], r[5], r[8]}, .origin = {}};
}

int orientation_index(const world::Affine3D& basis) {
    const auto round = [](float v) {
        if (v > 0.5F) {
            return 1.0F;
        }
        return v < -0.5F ? -1.0F : 0.0F;
    };
    const std::array<float, 9> rows = {round(basis.x.x), round(basis.y.x), round(basis.z.x),
                                       round(basis.x.y), round(basis.y.y), round(basis.z.y),
                                       round(basis.x.z), round(basis.y.z), round(basis.z.z)};
    for (int i = 0; i < kOrientations; ++i) {
        if (kOrthoBases[static_cast<std::size_t>(i)] == rows) {
            return i;
        }
    }
    return 0;
}

std::string grid_item_text(const GridItem& item) {
    if (item.orientation == 0) {
        return item.item;
    }
    return item.item + ":" + std::to_string(item.orientation);
}

Result<GridItem> parse_grid_item(std::string_view text) {
    GridItem out;
    const std::size_t colon = text.rfind(':');
    out.item = std::string(text.substr(0, colon));
    if (out.item.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(text) + " names no item: write the item's name, then \":\" and "
                                         "an orientation if it is turned");
    }
    if (colon != std::string_view::npos) {
        const std::string_view digits = text.substr(colon + 1);
        if (digits.empty() || digits.size() > 2 ||
            !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) + ": after the ':' comes an orientation, 0 to 23");
        }
        out.orientation = std::stoi(std::string(digits));
        if (out.orientation >= kOrientations) {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) + ": " + std::string(digits) +
                                  " is not one of Godot's 24 orientations, 0 to 23");
        }
    }
    return out;
}

Result<GridCells> decode_grid_cells(const world::GridMap& map) {
    GridCells cells;
    std::vector<json::ValidationError> errors;
    decode(map, &cells, errors, true);
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    return cells;
}

std::vector<json::ValidationError> validate_grid_cells(const world::GridMap& map) {
    std::vector<json::ValidationError> errors;
    decode(map, nullptr, errors, false);
    return errors;
}

void encode_grid_cells(const GridCells& cells, world::GridMap& map) {
    if (cells.empty()) {
        map.legend.clear();
        map.floors.clear();
        return;
    }
    // The items in use, the most used first, each with its key.
    std::vector<std::string> order;
    std::map<std::string, std::string, std::less<>> key_of;
    std::map<std::string, std::size_t, std::less<>> uses;
    std::map<std::string, std::string, std::less<>> name_of;
    for (const auto& [cell, held] : cells) {
        std::string text = grid_item_text(held);
        ++uses[text];
        if (key_of.try_emplace(text).second) {
            name_of.emplace(text, held.item);
            order.push_back(std::move(text));
        }
    }
    std::ranges::stable_sort(order, [&](const std::string& a, const std::string& b) {
        return uses.find(a)->second > uses.find(b)->second;
    });
    const std::size_t length = world::legend_key_length(order.size());
    std::set<std::string, std::less<>> taken;
    for (const auto& [key, value] : map.legend) {
        if (key.size() != length || !world::valid_legend_key(key)) {
            continue;
        }
        auto item = parse_grid_item(value);
        if (!item) {
            continue;
        }
        const auto it = key_of.find(grid_item_text(item.value()));
        if (it != key_of.end() && it->second.empty()) {
            it->second = key;
            taken.insert(key);
        }
    }
    for (const std::string& text : order) {
        std::string& key = key_of.find(text)->second;
        if (key.empty()) {
            key = world::new_legend_key(name_of.find(text)->second, length, taken);
            taken.insert(key);
        }
    }
    map.legend.clear();
    for (const auto& [text, key] : key_of) {
        map.legend.emplace(key, text);
    }
    map.floors.clear();
    for (auto first = cells.begin(); first != cells.end();) {
        const int y = first->first.y;
        auto last = first;
        int min_x = INT_MAX;
        int max_z = first->first.z;
        for (; last != cells.end() && last->first.y == y; ++last) {
            min_x = std::min(min_x, last->first.x);
            max_z = last->first.z;
        }
        world::GridFloor& floor = map.floors.emplace_back();
        floor.y = y;
        floor.origin = {min_x, first->first.z};
        floor.rows.assign(static_cast<std::size_t>(max_z - first->first.z) + 1, std::string());
        for (auto it = first; it != last; ++it) {
            std::string& row = floor.rows[static_cast<std::size_t>(it->first.z - floor.origin.y)];
            const std::size_t at = static_cast<std::size_t>(it->first.x - min_x) * length;
            row.append(at - row.size(), '.');
            row += key_of.find(grid_item_text(it->second))->second;
        }
        first = last;
    }
}

Vec3 map_to_local(const world::GridMap& map, Vec3i cell) {
    const Vec3 size = map.cell_size;
    const Vec3 offset{map.cell_center_x ? size.x * 0.5F : 0.0F,
                      map.cell_center_y ? size.y * 0.5F : 0.0F,
                      map.cell_center_z ? size.z * 0.5F : 0.0F};
    return {(static_cast<float>(cell.x) * size.x) + offset.x,
            (static_cast<float>(cell.y) * size.y) + offset.y,
            (static_cast<float>(cell.z) * size.z) + offset.z};
}

Vec3i local_to_map(const world::GridMap& map, Vec3 local) {
    const auto axis = [](float p, float size) {
        const float cell = std::floor(p / size);
        // Cells past what an int holds, as from a cell size of 0, are clamped.
        return static_cast<int>(std::clamp(cell, -2147483648.0F, 2147483520.0F));
    };
    return {axis(local.x, map.cell_size.x), axis(local.y, map.cell_size.y),
            axis(local.z, map.cell_size.z)};
}

world::Affine3D cell_transform(const world::GridMap& map, Vec3i cell, int orientation) {
    world::Affine3D transform = orientation_basis(orientation);
    const float s = map.cell_scale;
    transform.x = transform.x * s;
    transform.y = transform.y * s;
    transform.z = transform.z * s;
    transform.origin = map_to_local(map, cell);
    return transform;
}

Vec3i octant_of(const world::GridMap& map, Vec3i cell) {
    const int size = std::max(map.cell_octant_size, 1);
    const auto axis = [size](int c) { return c > 0 ? c / size : (c - (size - 1)) / size; };
    return {axis(cell.x), axis(cell.y), axis(cell.z)};
}

std::shared_ptr<const GridMesh> grid_mesh(const world::World& world, entt::entity entity) {
    const auto* map =
        world.valid(entity) ? world.ecs().try_get<const world::GridMap>(entity) : nullptr;
    if (map == nullptr) {
        return empty_mesh();
    }
    GridCache* cache = find_cache(world);
    if (cache == nullptr) {
        return make(*map, entity, sources_of(*map, file_libraries(world), file_models(world)),
                    nullptr)
            .mesh;
    }
    const std::scoped_lock lock(cache->mutex);
    Sources sources = sources_of(*map, cache->libraries, cache->models);
    const auto found = cache->entries.find(entity);
    if (found != cache->entries.end() && found->second.map == *map &&
        found->second.sources == sources) {
        return found->second.mesh;
    }
    Made made = make(*map, entity, std::move(sources),
                     found != cache->entries.end() ? &found->second : nullptr);
    std::shared_ptr<const GridMesh> mesh = made.mesh;
    // GridMaps that are gone go with it.
    std::erase_if(cache->entries, [&](const auto& entry) {
        return !world.valid(entry.first) || !world.ecs().all_of<world::GridMap>(entry.first);
    });
    cache->entries.insert_or_assign(entity, std::move(made));
    return mesh;
}

void install_gridmap(world::World& world, ModelLoader models, MeshLibraryLoader libraries) {
    if (GridCache* cache = find_cache(world); cache != nullptr) {
        const std::scoped_lock lock(cache->mutex);
        cache->models = std::move(models);
        cache->libraries = std::move(libraries);
        cache->entries.clear();
        return;
    }
    auto cache = std::make_shared<GridCache>();
    cache->models = std::move(models);
    cache->libraries = std::move(libraries);
    world.ecs().ctx().emplace<std::shared_ptr<GridCache>>(cache);
    world.add_reset([cache](world::World& /*world*/) {
        const std::scoped_lock lock(cache->mutex);
        cache->entries.clear();
    });
    world.add_memory_source("gridmap",
                            [cache](const world::World& /*world*/) { return cache->memory(); });
}

std::vector<GridBody> grid_bodies(const world::World& world) {
    std::vector<GridBody> bodies;
    for (const entt::entity entity : gridmaps(world)) {
        const std::shared_ptr<const GridMesh> mesh = grid_mesh(world, entity);
        if (mesh->shapes->empty()) {
            continue;
        }
        const auto& map = world.ecs().get<const world::GridMap>(entity);
        bodies.push_back({.entity = entity,
                          .layer = map.collision_layer,
                          .mask = map.collision_mask,
                          .friction = map.friction,
                          .bounce = map.bounce,
                          .shapes = mesh->shapes});
    }
    return bodies;
}

std::vector<Problem> check_gridmaps(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : gridmaps(world)) {
        if (ecs.any_of<world::RigidBody3D, world::CharacterBody3D, world::AnimatableBody3D,
                       world::StaticBody3D, world::Area3D>(entity)) {
            problems.push_back({entity, std::string(kField),
                                "is on a physics body, so its cells don't collide: put the "
                                "GridMap on an entity of its own"});
        }
        const std::shared_ptr<const GridMesh> mesh = grid_mesh(world, entity);
        problems.insert(problems.end(), mesh->problems.begin(), mesh->problems.end());
    }
    return problems;
}

} // namespace slash::scene3d
