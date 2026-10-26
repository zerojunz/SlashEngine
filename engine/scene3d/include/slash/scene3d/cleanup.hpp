#pragma once

#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"

#include <cstddef>
#include <optional>
#include <string>

// Making a generated or imported model game-ready, as an artist's last pass before it goes into
// a game: up along y, real-world size, its origin where scenes expect it, under its triangle
// budget and with texture coordinates on every surface. model.cleanup runs it on a model file.
namespace slash::scene3d {

// What cleanup_model does; each field at its default does nothing. Every member has an
// initializer so designated initializers may skip any of them.
struct CleanupOptions {
    // The axis the model's up points along in its file ("z" for Blender's and most CAD tools'
    // without glTF's conversion, "-y", "x", ...), turned to y; "y" or "" for none.
    std::string up{}; // NOLINT(readability-redundant-member-init)
    // Its size times this, or, with height, made height meters tall (after up); 0 for none.
    float scale = 0.0F;
    float height = 0.0F;
    // Where its origin goes in its box, 0 at the low side and 1 at the high one on each axis, as
    // ModelSummary::pivot: [0.5, 0, 0.5] stands it on the ground, centered.
    std::optional<Vec3> pivot{}; // NOLINT(readability-redundant-member-init)
    // The most triangles it may have: each surface is simplified with meshoptimizer's
    // simplifier, as Godot's importer simplifies LODs, to its share; 0 for none.
    std::size_t triangles = 0;
    // Gives each surface whose vertices all have one texture coordinate (none, in effect) its
    // own, unwrapped with xatlas as lightmap UVs are, so textures and painted detail can map.
    bool uvs = false;
    // What it collides as, made last from its drawn triangles as generate_collision makes it:
    // "trimesh", "convex" or "decompose" (into at most max_hulls convex hulls, 0 for
    // DecomposeOptions' default); "" keeps the collision it has.
    std::string collision{}; // NOLINT(readability-redundant-member-init)
    std::size_t max_hulls = 0;
    // How much of the light baked into its albedo textures to take out, 0 to 1, as
    // delight_textures does; 0 for none. Done first, on the model as it came.
    float delight = 0.0F;
    // Reads the texture files its materials name, for delight; may be empty without it.
    ModelReader read{}; // NOLINT(readability-redundant-member-init)
};

struct CleanupReport {
    std::size_t triangles_before = 0;
    std::size_t triangles = 0;
    Bounds bounds_before;
    std::size_t unwrapped = 0; // surfaces given texture coordinates
    std::size_t collision = 0; // collision shapes made
    std::size_t delit = 0;     // albedo textures the light was taken out of
    // Where the light baked into the largest of them came from, in the model's space after the
    // cleanup's turn; none when it was lit evenly.
    std::optional<Vec3> light{}; // NOLINT(readability-redundant-member-init)
};

// What delight_textures did: how many albedo textures it changed, and where the light baked into
// the one covering the most of the model came from (a unit vector in the model's space), when
// it was lit from a side.
struct DelightReport {
    std::size_t textures = 0;
    std::optional<Vec3> light{}; // NOLINT(readability-redundant-member-init)
};

// Takes the light baked into a model's albedo textures out of them, as Unity's De-Lighting tool
// sets out to for scanned and generated assets, so lights in the game shade it instead. For each
// texture, the surfaces sampling it are drawn into its texels (at most 256 a side), each texel's
// ambient occlusion is cast against the whole model, and a light of an ambient and a directional
// part (the first two bands of spherical harmonics) is fitted to the texels' luminance times
// that occlusion by least squares; each texel is divided by what that light gave it, by
// strength (0 to 1), and kept within a quarter and four times what it was. Shading finer than
// the 256 texels, and color that truly follows the surfaces' facing, stay. The changed images
// replace the albedo ones inside the model; files are read with read. The same texels on every
// machine.
[[nodiscard]] Result<DelightReport> delight_textures(Model& model, const ModelReader& read,
                                                     float strength = 1.0F);

// Cleans a model up in place: turned, delit, simplified, sized, moved, unwrapped and given
// collision, in that order; collision it already has is turned, sized and moved with it. A model
// with skins or animations is refused for now, since write_glb leaves them out; so is a triangle
// budget the simplifier can't get under, a height for a model flat along y, an up it doesn't
// know, a texture it can't read to delight, and collision it can't make.
[[nodiscard]] Result<CleanupReport> cleanup_model(Model& model, const CleanupOptions& options);

} // namespace slash::scene3d
