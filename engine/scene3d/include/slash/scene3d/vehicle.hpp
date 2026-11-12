#pragma once

// A .vehicle.json recipe (slash/world/vehicle.hpp) built into a model, after Unreal's Chaos
// Vehicles' meshes: the body (a box, its cabin, a hull, wings or tracks, or the model file the
// body names), the recipe's own parts (shapes or model files) with its collision, and an attachment
// point for each seat ("seat_<name>") and door ("door_<name>", where one gets in). Wheels aren't in
// it: the vehicles system gives each wheel an entity of its own that turns. Models load
// .vehicle.json files through it, so a MeshInstance3D draws one.

#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/vehicle.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace slash::scene3d {

// The model a recipe makes, facing +z with y up and the ground at 0, in meters, its body drawn
// with materials "paint", "trim" and "glass". body is the model the recipe's body names, drawn
// in place of the box, or null; part_models holds, for each of the recipe's parts, the model
// file it names, or null for a part that is a shape.
[[nodiscard]] Model build_vehicle(const world::VehicleRecipe& recipe, const Model* body = nullptr,
                                  std::span<const Model* const> part_models = {});

// A .vehicle.json file's model (load_model calls it), and its problems (check_model's).
[[nodiscard]] Result<Model> load_vehicle_model(std::string_view bytes, const ModelReader& read);
[[nodiscard]] std::vector<json::ValidationError> check_vehicle_model(std::string_view bytes,
                                                                     const ModelReader& read);

} // namespace slash::scene3d
