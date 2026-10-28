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
};

struct CleanupReport {
    std::size_t triangles_before = 0;
    std::size_t triangles = 0;
    Bounds bounds_before;
    std::size_t unwrapped = 0; // surfaces given texture coordinates
};

// Cleans a model up in place: turned, simplified, sized, moved and unwrapped, in that order. A
// model with skins or animations is refused for now, since write_glb leaves them out; so is a
// triangle budget the simplifier can't get under, a height for a model flat along y, and an up it
// doesn't know.
[[nodiscard]] Result<CleanupReport> cleanup_model(Model& model, const CleanupOptions& options);

} // namespace slash::scene3d
