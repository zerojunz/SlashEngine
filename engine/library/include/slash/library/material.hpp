#pragma once

#include "slash/render/device.hpp"

// Tileable PBR materials from one image, as the library makes them: the image made to tile, then
// a normal map and a roughness map worked out from its brightness, for a StandardMaterial3D's
// albedo_texture, normal_texture and roughness_texture. Integer math only, so the maps are the
// same bytes on every machine.
namespace slash::library {

// A ratio of how much the pixels on an image's opposite edges differ, where it wraps, against how
// much neighbouring pixels inside it differ on average: about 1 or less for an image that tiles
// without a seam, more the plainer its seams show; 0 for one flat color. Rounded to hundredths,
// at most 99.
[[nodiscard]] double seam_ratio(const render::Image& image);

// Images whose seam_ratio is above this get made to tile.
inline constexpr double kSeamless = 1.5;

// The image made to tile, opaque: four copies of it, moved half its width, half its height or
// both, blended by how far each pixel is from the edges where each copy breaks, so every copy
// counts for nothing at its own seams. The middle stays the image as it was.
[[nodiscard]] render::Image make_seamless(const render::Image& image);

// A normal map of the image's brightness taken as height, wrapping round its edges so it tiles
// as the image does, in Godot's convention (OpenGL's: green points up the image).
[[nodiscard]] render::Image normal_map(const render::Image& image);

// A roughness map of the image's brightness: darker places, such as cracks, rougher. Gray, from
// 0.55 at white to 0.94 at black, for the material's roughness to scale.
[[nodiscard]] render::Image roughness_map(const render::Image& image);

} // namespace slash::library
