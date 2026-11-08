#pragma once

#include "slash/render/splats.hpp"
#include "slash/scene3d/scene3d.hpp"

#include <string_view>
#include <vector>

namespace slash::asset {
class Assets;
} // namespace slash::asset

namespace slash::world {
class World;
} // namespace slash::world

// GaussianSplat3D, a research spike of SlashEngine's own (Godot has none): clouds of 3D Gaussians
// from 3D Gaussian Splatting .ply files, which scene3d::Renderer hands the frame as
// render::Scene3D::splats (see docs/reference/gaussian-splats.md).
namespace slash::scene3d {

inline constexpr std::string_view kSplatSuffix = ".ply";

// Adds .ply files as render::GaussianSplats assets, read with render::read_splat_ply, which
// GaussianSplat3D's splats names.
void add_splat_type(asset::Assets& assets);

// The GaussianSplat3Ds whose splats isn't a .ply file, whose sh_degree isn't 0 to 3, or, with the
// world's file reader set, whose file can't be read as splats, in the order they were made.
[[nodiscard]] std::vector<Problem> check_splats(const world::World& world);

} // namespace slash::scene3d
