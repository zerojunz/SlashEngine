#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/render/particles.hpp"

#include <optional>
#include <vector>

// Particle graphs: node graphs in a process material, compiled to material programs as canvas
// materials' are (src/material.cpp), for process_material.cpp.
namespace slash::canvas::detail {

// Adds a process material's params, nodes, start and process to its schema, with the $defs they
// share; list_ops lists the ops' names.
void add_particle_graph_schema(json::Value& schema, bool list_ops);

// A graph nested too deep or with too many nodes for the schema check to walk, if any.
[[nodiscard]] std::optional<json::ValidationError>
check_particle_graph_size(const json::Value& doc);

// Every mistake in a schema-valid process material's graphs; with none, the compiled stages go in
// start and process, which may be null.
[[nodiscard]] std::vector<json::ValidationError>
compile_particle_graphs(const json::Value& doc, render::ParticleProgram* start,
                        render::ParticleProgram* process);

} // namespace slash::canvas::detail
