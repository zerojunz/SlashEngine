#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"

#include <functional>
#include <string_view>
#include <vector>

// Model recipes: .model.json files that describe a model as text, as the design's parametric
// models: named parameters, materials, and parts made of Godot's CSG shapes (box, sphere,
// cylinder, torus, polygon pushed, spun or swept along a path, and closed primitive meshes),
// added to, cut from or intersected with each other in order, with bevels and arrays. Numbers
// can be expressions ("=width / 2") over the parameters, in Godot's Expression syntax. The engine
// builds the model when it loads the file, so a MeshInstance3D draws a recipe as it draws a glTF
// file, and refining a model means changing a number.
namespace slash::scene3d {

inline constexpr std::string_view kModelRecipeSuffix = ".model.json";

// The JSON Schema of recipe files, for `slash-cli schema model`.
[[nodiscard]] json::Value model_recipe_schema();

// Loads a model file a recipe's model part names, by project path.
using ModelPartLoader = std::function<Result<Model>(std::string_view path)>;

// Every error in a recipe, each at its JSON Pointer: the schema's, expressions that don't parse
// or run, unknown parameters and materials (with the closest name), fields its shape doesn't
// have, bevels that don't fit, shapes the CSG can't make, such as a polygon with no area or a
// part that comes out empty, and, with load, model parts whose files don't load. None for a
// recipe that builds as written.
[[nodiscard]] std::vector<json::ValidationError>
validate_model_recipe(const json::Value& doc, const ModelPartLoader& load = {});

// The model a recipe builds: a node named after each top-level part (and each copy its array
// makes, numbered from 2 as Godot names nodes) placing a mesh of one surface per material, as a
// CSG root's, or for a model part, the nodes of the model it names, loaded with load, under it,
// as Godot instances a scene: their names kept unless taken, then numbered, and the model's
// animations left out. Fails with the first error when the schema, an expression, a field or a
// material is wrong, or a model part's file doesn't load or there is no load; shapes the CSG
// can't make are left out, as validate_model_recipe reports them.
[[nodiscard]] Result<Model> build_model_recipe(const json::Value& doc,
                                               const ModelPartLoader& load = {});

} // namespace slash::scene3d
