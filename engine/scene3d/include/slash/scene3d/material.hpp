#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// Material files: a StandardMaterial3D kept in a .standard_material.json file of its own, as Godot
// saves one as a resource, so many models share one material and its textures instead of each
// carrying its own. A model recipe's material can name one, alone or with fields over it.
namespace slash::scene3d {

inline constexpr std::string_view kStandardMaterialSuffix = ".standard_material.json";

// The StandardMaterial3D fields that name texture files, by project path.
inline constexpr std::array<std::string_view, 5> kMaterialTextureFields{
    "albedo_texture", "metallic_texture", "roughness_texture", "normal_texture",
    "emission_texture"};

// The JSON Schema of .standard_material.json files: "format": 1 and StandardMaterial3D's fields,
// for `slash-cli schema standard_material`.
[[nodiscard]] json::Value standard_material_schema();

// Every error in a material file, each at its JSON Pointer; none for one that reads as written.
[[nodiscard]] std::vector<json::ValidationError> validate_standard_material(const json::Value& doc);

// The material a file holds. Fails with the first error validate_standard_material finds.
[[nodiscard]] Result<world::StandardMaterial3D> parse_standard_material(const json::Value& doc);

// The file's StandardMaterial3D fields as JSON, without its format.
[[nodiscard]] json::Value standard_material_fields(const json::Value& doc);

// Each texture the material's fields name that read can't read or that doesn't decode, at "at"
// followed by the field's name.
[[nodiscard]] std::vector<json::ValidationError>
check_material_textures(const json::Value& fields, const ModelReader& read, std::string_view at);

// A material file's errors and those of the textures it names, read through read.
[[nodiscard]] std::vector<json::ValidationError> check_standard_material(std::string_view text,
                                                                         const ModelReader& read);

} // namespace slash::scene3d
