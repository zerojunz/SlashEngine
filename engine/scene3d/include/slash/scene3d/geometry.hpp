#pragma once

#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"

#include <cstddef>
#include <optional>

// What a model's triangles say about how it was made, for reviewing generated and imported
// models: holes in surfaces that should be closed, triangles with no area, and how many texels
// of its albedo textures cover each meter.
namespace slash::scene3d {

struct GeometryReport {
    // Edges only one triangle has, once corners at the same place are merged, and the loops they
    // make: the rims of holes, or of a surface left open on purpose, such as a plane.
    std::size_t open_edges = 0;
    std::size_t holes = 0;
    // Triangles with no area, which draw nothing.
    std::size_t degenerate = 0;
    // Texels of the albedo textures per meter of the surfaces they cover, as texel density is
    // measured for a game's art: the square root of the texels over the area. None for a model
    // without albedo textures.
    std::optional<float> texel_density;
};

// The model's parts at rest, each where its node puts it. Images not loaded yet are read with
// read for their size; an error names an image that can't be.
[[nodiscard]] Result<GeometryReport> check_geometry(const Model& model, const ModelReader& read);

} // namespace slash::scene3d
