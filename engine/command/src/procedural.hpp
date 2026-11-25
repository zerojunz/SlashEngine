// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include "slash/command/command.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/image.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::command {

// Adds texture.generate, over slash/procedural/texture.hpp.
void register_procedural_commands(Registry& registry);

// Adds texture.inspect, texture.edit and texture.check, over
// slash/procedural/texture_graph.hpp (texture_tools.cpp).
void register_texture_tools(Registry& registry);

// Adds model.bake_maps, over slash/scene3d/mesh_maps.hpp (mesh_maps.cpp).
void register_mesh_map_commands(Registry& registry);

// Reads what a graph names from the session's project: text files, and images decoded as
// texture paths are (a graph's images may be other graphs' outputs).
[[nodiscard]] procedural::GraphReader graph_reader(Session& session);

// Each channel's [min, mean, max] over an image, 0 to 255.
[[nodiscard]] json::Value channel_stats(const render::Image& image);

// How far apart, on average per channel, neighboring pixels are inside an image ("inside") and
// across its edges when it repeats ("x", "y"), 0 to 255.
[[nodiscard]] json::Value wrap_differences(const render::Image& image);

// Adds scatter.run, over slash/procedural/scatter.hpp (scatter.cpp).
void register_scatter_commands(Registry& registry);

// Adds pcg.run, pcg.preview and pcg.check, over slash/procedural/pcg.hpp (pcg.cpp).
void register_pcg_commands(Registry& registry);

// Adds hair.get, over slash/world/hair.hpp and slash/scene3d/hair.hpp (hair.cpp).
void register_hair_commands(Registry& registry);
void register_damage_commands(Registry& registry);

// Adds vfx.play, vfx.state, vfx.check and vfx.preview, over slash/world/vfx.hpp (vfx.cpp).
void register_vfx_commands(Registry& registry);

// Adds fluid.place, fluid.pour, fluid.state, fluid.check and fluid.preview, over
// slash/world/fluid.hpp (fluid.cpp).
void register_fluid_commands(Registry& registry);

// Adds water.place and water.sample, over slash/world/water.hpp (water.cpp).
void register_water_commands(Registry& registry);

// The noise a command samples: a .texture.json noise texture's (its noise, normalize and
// invert), or inline FastNoiseLite settings, normalized.
struct NoiseSource {
    procedural::NoiseSettings settings;
    bool normalize = true;
    bool invert = false;
};

// The schema of a param naming one: a .texture.json path or a noise object.
[[nodiscard]] json::Value noise_source_property(std::string_view description);

// Reads the noise a param at `at` names into out, or returns the reply to fail with.
[[nodiscard]] std::optional<Reply> read_noise_source(Session& session, const json::Value& value,
                                                     const std::string& at, NoiseSource& out);

// The noise at width by height whole points from (x, y), rows first, from 0 to 1: normalized over
// these points when normalize is on, else value / 2 + 1 / 2 kept within 0 and 1, then inverted when
// invert is on.
[[nodiscard]] std::vector<float> sample_noise_source(const NoiseSource& source, int x, int y,
                                                     int width, int height);

} // namespace slash::command
