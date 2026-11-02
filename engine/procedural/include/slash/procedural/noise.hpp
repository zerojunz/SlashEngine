#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Noise, as Godot's FastNoiseLite resource (modules/noise/fastnoise_lite.cpp), on Auburn's
// FastNoiseLite: simplex, cellular, Perlin and value noise, with fractals and domain warping.
// Gameplay code samples it while a game runs:
//
//   procedural::NoiseSettings settings;
//   settings.noise_type = procedural::NoiseType::Cellular;
//   settings.frequency = 0.05F;
//   const procedural::Noise noise(settings);
//   const float height = noise.get_noise_2d(x, y); // about -1 to 1
//
// Values are the same bits on every machine: FastNoiseLite calls libm only for square roots,
// and the module builds with no fused multiply-adds.
namespace slash::procedural {

// Godot's FastNoiseLite.NoiseType.
enum class NoiseType : std::uint8_t { Simplex, SimplexSmooth, Cellular, Perlin, ValueCubic, Value };
// Godot's FastNoiseLite.FractalType.
enum class FractalType : std::uint8_t { None, Fbm, Ridged, PingPong };
// Godot's FastNoiseLite.CellularDistanceFunction.
enum class CellularDistance : std::uint8_t { Euclidean, EuclideanSquared, Manhattan, Hybrid };
// Godot's FastNoiseLite.CellularReturnType.
enum class CellularReturn : std::uint8_t {
    CellValue,
    Distance,
    Distance2,
    Distance2Add,
    Distance2Sub,
    Distance2Mul,
    Distance2Div,
};
// Godot's FastNoiseLite.DomainWarpType.
enum class DomainWarpType : std::uint8_t { Simplex, SimplexReduced, BasicGrid };
// Godot's FastNoiseLite.DomainWarpFractalType.
enum class DomainWarpFractal : std::uint8_t { None, Progressive, Independent };

// A FastNoiseLite resource's settings, with Godot's defaults.
struct NoiseSettings {
    NoiseType noise_type = NoiseType::SimplexSmooth;
    int seed = 0;
    float frequency = 0.01F;
    Vec3 offset; // added to every point before sampling
    FractalType fractal_type = FractalType::Fbm;
    int fractal_octaves = 5;
    float fractal_lacunarity = 2.0F;
    float fractal_gain = 0.5F;
    float fractal_weighted_strength = 0.0F;
    float fractal_ping_pong_strength = 2.0F;
    CellularDistance cellular_distance_function = CellularDistance::Euclidean;
    CellularReturn cellular_return_type = CellularReturn::Distance;
    float cellular_jitter = 1.0F;
    bool domain_warp_enabled = false;
    DomainWarpType domain_warp_type = DomainWarpType::Simplex;
    float domain_warp_amplitude = 30.0F;
    float domain_warp_frequency = 0.05F;
    DomainWarpFractal domain_warp_fractal_type = DomainWarpFractal::Progressive;
    int domain_warp_fractal_octaves = 5;
    float domain_warp_fractal_lacunarity = 6.0F;
    float domain_warp_fractal_gain = 0.5F;
};

// Noise of some settings, as Godot's FastNoiseLite samples it: the offset added, the point
// warped when domain warping is on, then the noise. Cheap to copy; safe to sample from many
// threads at once.
class Noise {
public:
    Noise();
    explicit Noise(const NoiseSettings& settings);
    Noise(const Noise& other);
    Noise& operator=(const Noise& other);
    Noise(Noise&&) noexcept;
    Noise& operator=(Noise&&) noexcept;
    ~Noise();

    [[nodiscard]] const NoiseSettings& settings() const { return settings_; }

    // The noise at a point, about -1 to 1 (cellular distances can pass 1).
    [[nodiscard]] float get_noise_1d(float x) const;
    [[nodiscard]] float get_noise_2d(float x, float y) const;
    [[nodiscard]] float get_noise_3d(float x, float y, float z) const;

    // Godot's Noise.get_image as values: width by height samples at whole coordinates (x, y), or
    // (x, y, z) in 3D space, rows from the top.
    [[nodiscard]] std::vector<float> get_values(int width, int height, bool in_3d_space = false,
                                                int z = 0) const;

private:
    struct Impl;
    NoiseSettings settings_;
    std::unique_ptr<Impl> impl_;
};

// JSON Schema for a FastNoiseLite's settings as files write them: Godot's property names, enum
// values in snake_case ("simplex_smooth", "ping_pong"), each optional with Godot's default.
[[nodiscard]] json::Value noise_schema();

// Every error in a noise object, each with the JSON Pointer of the bad value under at.
[[nodiscard]] std::vector<json::ValidationError> validate_noise(const json::Value& doc,
                                                                const std::string& at = "");

// The settings a noise object holds, or its first error.
[[nodiscard]] Result<NoiseSettings> parse_noise(const json::Value& doc);

// The settings as a noise object, leaving out those at Godot's defaults.
[[nodiscard]] json::Value noise_to_json(const NoiseSettings& settings);

} // namespace slash::procedural
