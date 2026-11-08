#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Face tracking, as Godot's XRFaceTracker and XRFaceModifier3D: a face tracker holds a weight for
// each of the 143 face shapes of the Unified Expressions standard (Tracker::blend_shapes), and a
// mesh's blend shapes named in that standard, Apple ARKit's, SRanipal's or Meta's follow them.
//
//   std::map<std::string, std::size_t> drives = xr::face_blend_mapping(model_shape_names);
//   for (const auto& [name, shape] : drives) instance.blend_shapes[name] = weights[shape];
namespace slash::xr {

// Godot's XRFaceTracker::FT_MAX: the face shapes, 102 base shapes then 41 blended of them.
inline constexpr std::size_t kFaceShapeCount = 143;
// The tracker Godot's XRFaceModifier3D follows by default.
inline constexpr std::string_view kFaceTracker = "/user/face_tracker";

// A face shape's Unified Expressions name, as "JawOpen" or "MouthSmileLeft".
[[nodiscard]] std::string_view face_shape_name(std::size_t shape);

// The face shape a blend shape's name stands for, as Godot's find_face_blend_shape: the name in
// any case, with underscores ignored, in Unified Expressions, ARKit ("jawOpen", "eyeBlinkLeft"),
// SRanipal or Meta's standard.
[[nodiscard]] std::optional<std::size_t> face_shape_named(std::string_view name);

// The shapes a blended face shape is made of, as Godot's table of unified blend shapes; empty for
// a base shape.
[[nodiscard]] std::span<const std::size_t> face_shape_parts(std::size_t shape);

// Which face shape drives each of a mesh's blend shapes, as Godot's XRFaceModifier3D works it
// out: each name face_shape_named knows, less a blended shape whose parts the mesh has (so a
// face isn't moved twice).
[[nodiscard]] std::map<std::string, std::size_t>
face_blend_mapping(std::span<const std::string> names);

// Fills in the shapes a recording leaves out (given false) that the shapes it gives imply, as
// ARKit records some blended shapes and none of their parts: each part of a given blended shape
// takes its value (the mean, for a part of several), then each blended shape whose parts are all
// known takes their mean. Marks each shape it fills as given. SlashEngine's own, so a mesh made
// with other shapes than the recording's follows it.
void fill_blended_shapes(std::span<float, kFaceShapeCount> weights,
                         std::span<bool, kFaceShapeCount> given);

} // namespace slash::xr
