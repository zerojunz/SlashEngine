#pragma once

#include "slash/command/command.hpp"
#include "slash/procedural/noise.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::command {

// Adds texture.generate, over slash/procedural/texture.hpp.
void register_procedural_commands(Registry& registry);

// Adds scatter.run, over slash/procedural/scatter.hpp (scatter.cpp).
void register_scatter_commands(Registry& registry);

// The noise a command samples: a .texture.json NoiseTexture2D's (its noise, normalize and
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

// The noise at width by height whole points from (x, y), rows first, from 0 to 1, as Godot's
// Noise.get_image makes them: normalized over these points when normalize is on, else
// value / 2 + 1 / 2 kept within 0 and 1, then inverted when invert is on.
[[nodiscard]] std::vector<float> sample_noise_source(const NoiseSource& source, int x, int y,
                                                     int width, int height);

} // namespace slash::command
