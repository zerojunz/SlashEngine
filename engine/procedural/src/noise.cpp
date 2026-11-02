#include "slash/procedural/noise.hpp"

#include "json_fields.hpp"

#include <FastNoiseLite.h>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace slash::procedural {

// Two of FastNoiseLite's objects, as Godot's resource keeps them: one for the noise and one for
// the domain warp (modules/noise/fastnoise_lite.cpp).
struct Noise::Impl {
    FastNoiseLite noise;
    FastNoiseLite warp;
};

namespace {

using detail::EnumName;

constexpr std::array<EnumName<NoiseType>, 6> kNoiseTypes{{
    {NoiseType::Simplex, "simplex"},
    {NoiseType::SimplexSmooth, "simplex_smooth"},
    {NoiseType::Cellular, "cellular"},
    {NoiseType::Perlin, "perlin"},
    {NoiseType::ValueCubic, "value_cubic"},
    {NoiseType::Value, "value"},
}};
constexpr std::array<EnumName<FractalType>, 4> kFractalTypes{{
    {FractalType::None, "none"},
    {FractalType::Fbm, "fbm"},
    {FractalType::Ridged, "ridged"},
    {FractalType::PingPong, "ping_pong"},
}};
constexpr std::array<EnumName<CellularDistance>, 4> kCellularDistances{{
    {CellularDistance::Euclidean, "euclidean"},
    {CellularDistance::EuclideanSquared, "euclidean_squared"},
    {CellularDistance::Manhattan, "manhattan"},
    {CellularDistance::Hybrid, "hybrid"},
}};
constexpr std::array<EnumName<CellularReturn>, 7> kCellularReturns{{
    {CellularReturn::CellValue, "cell_value"},
    {CellularReturn::Distance, "distance"},
    {CellularReturn::Distance2, "distance2"},
    {CellularReturn::Distance2Add, "distance2_add"},
    {CellularReturn::Distance2Sub, "distance2_sub"},
    {CellularReturn::Distance2Mul, "distance2_mul"},
    {CellularReturn::Distance2Div, "distance2_div"},
}};
constexpr std::array<EnumName<DomainWarpType>, 3> kWarpTypes{{
    {DomainWarpType::Simplex, "simplex"},
    {DomainWarpType::SimplexReduced, "simplex_reduced"},
    {DomainWarpType::BasicGrid, "basic_grid"},
}};
constexpr std::array<EnumName<DomainWarpFractal>, 3> kWarpFractals{{
    {DomainWarpFractal::None, "none"},
    {DomainWarpFractal::Progressive, "progressive"},
    {DomainWarpFractal::Independent, "independent"},
}};

// Godot's _convert_domain_warp_fractal_type_enum.
FastNoiseLite::FractalType warp_fractal(DomainWarpFractal type) {
    switch (type) {
    case DomainWarpFractal::Progressive:
        return FastNoiseLite::FractalType_DomainWarpProgressive;
    case DomainWarpFractal::Independent:
        return FastNoiseLite::FractalType_DomainWarpIndependent;
    case DomainWarpFractal::None:
        break;
    }
    return FastNoiseLite::FractalType_None;
}

// FastNoiseLite's objects set up as Godot's FastNoiseLite constructor and setters leave them.
void configure(FastNoiseLite& noise, FastNoiseLite& warp, const NoiseSettings& s) {
    noise.SetNoiseType(static_cast<FastNoiseLite::NoiseType>(s.noise_type));
    noise.SetSeed(s.seed);
    noise.SetFrequency(s.frequency);
    noise.SetFractalType(static_cast<FastNoiseLite::FractalType>(s.fractal_type));
    noise.SetFractalOctaves(s.fractal_octaves);
    noise.SetFractalLacunarity(s.fractal_lacunarity);
    noise.SetFractalGain(s.fractal_gain);
    noise.SetFractalWeightedStrength(s.fractal_weighted_strength);
    noise.SetFractalPingPongStrength(s.fractal_ping_pong_strength);
    noise.SetCellularDistanceFunction(
        static_cast<FastNoiseLite::CellularDistanceFunction>(s.cellular_distance_function));
    noise.SetCellularReturnType(
        static_cast<FastNoiseLite::CellularReturnType>(s.cellular_return_type));
    noise.SetCellularJitter(s.cellular_jitter);

    warp.SetDomainWarpType(static_cast<FastNoiseLite::DomainWarpType>(s.domain_warp_type));
    warp.SetSeed(s.seed);
    warp.SetDomainWarpAmp(s.domain_warp_amplitude);
    warp.SetFrequency(s.domain_warp_frequency);
    warp.SetFractalType(warp_fractal(s.domain_warp_fractal_type));
    warp.SetFractalOctaves(s.domain_warp_fractal_octaves);
    warp.SetFractalLacunarity(s.domain_warp_fractal_lacunarity);
    warp.SetFractalGain(s.domain_warp_fractal_gain);
}

} // namespace

Noise::Noise() : Noise(NoiseSettings{}) {}

Noise::Noise(const NoiseSettings& settings) : settings_(settings), impl_(std::make_unique<Impl>()) {
    configure(impl_->noise, impl_->warp, settings_);
}

Noise::Noise(const Noise& other)
    : settings_(other.settings_), impl_(std::make_unique<Impl>(*other.impl_)) {}

Noise& Noise::operator=(const Noise& other) {
    if (this != &other) {
        settings_ = other.settings_;
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

Noise::Noise(Noise&&) noexcept = default;
Noise& Noise::operator=(Noise&&) noexcept = default;
Noise::~Noise() = default;

// Godot's FastNoiseLite::get_noise_1d: the offset added, warped in x alone, then 2D noise at y 0.
float Noise::get_noise_1d(float x) const {
    x += settings_.offset.x;
    if (settings_.domain_warp_enabled) {
        float y_dummy = 0.0F;
        impl_->warp.DomainWarp(x, y_dummy);
    }
    return get_noise_2d(x, 0.0F);
}

float Noise::get_noise_2d(float x, float y) const {
    x += settings_.offset.x;
    y += settings_.offset.y;
    if (settings_.domain_warp_enabled) {
        impl_->warp.DomainWarp(x, y);
    }
    return impl_->noise.GetNoise(x, y);
}

float Noise::get_noise_3d(float x, float y, float z) const {
    x += settings_.offset.x;
    y += settings_.offset.y;
    z += settings_.offset.z;
    if (settings_.domain_warp_enabled) {
        impl_->warp.DomainWarp(x, y, z);
    }
    return impl_->noise.GetNoise(x, y, z);
}

std::vector<float> Noise::get_values(int width, int height, bool in_3d_space, int z) const {
    std::vector<float> values;
    if (width <= 0 || height <= 0) {
        return values;
    }
    values.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            values.push_back(in_3d_space
                                 ? get_noise_3d(static_cast<float>(x), static_cast<float>(y),
                                                static_cast<float>(z))
                                 : get_noise_2d(static_cast<float>(x), static_cast<float>(y)));
        }
    }
    return values;
}

json::Value noise_schema() {
    using detail::boolean_field;
    using detail::enum_field;
    using detail::integer_field;
    using detail::number_field;
    const NoiseSettings d;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "Noise settings, as Godot's FastNoiseLite: each optional, with "
                            "Godot's defaults.";
    json::Value& p = schema["properties"];
    p["noise_type"] = enum_field(kNoiseTypes, d.noise_type, "The noise algorithm.");
    p["seed"] = integer_field(d.seed, "Seed of the random values.");
    p["frequency"] = number_field(d.frequency, "Scale of the noise: lower is smoother.");
    json::Value& offset = p["offset"];
    offset["type"] = "array";
    offset["items"]["type"] = "number";
    offset["minItems"] = 2;
    offset["maxItems"] = 3;
    offset["description"] = "Added to every point before sampling: [x, y] or [x, y, z].";
    p["fractal_type"] = enum_field(kFractalTypes, d.fractal_type,
                                   "How octaves combine: none, fBm, ridged or ping-pong.");
    p["fractal_octaves"] = integer_field(d.fractal_octaves, "Layers of noise.", 1, 16);
    p["fractal_lacunarity"] =
        number_field(d.fractal_lacunarity, "Frequency multiplier from one octave to the next.");
    p["fractal_gain"] =
        number_field(d.fractal_gain, "Strength multiplier from one octave to the next.");
    p["fractal_weighted_strength"] = number_field(
        d.fractal_weighted_strength, "Higher weights lower octaves by the ones above.", 0, 1);
    p["fractal_ping_pong_strength"] =
        number_field(d.fractal_ping_pong_strength, "Strength of the ping-pong fractal.");
    p["cellular_distance_function"] =
        enum_field(kCellularDistances, d.cellular_distance_function,
                   "How cellular noise measures distance to a cell's point.");
    p["cellular_return_type"] =
        enum_field(kCellularReturns, d.cellular_return_type, "What cellular noise returns.");
    p["cellular_jitter"] =
        number_field(d.cellular_jitter, "How far cell points stray from their grid.");
    p["domain_warp_enabled"] =
        boolean_field(d.domain_warp_enabled, "Warp each point before sampling.");
    p["domain_warp_type"] = enum_field(kWarpTypes, d.domain_warp_type, "The warp algorithm.");
    p["domain_warp_amplitude"] =
        number_field(d.domain_warp_amplitude, "Largest distance a point is warped.");
    p["domain_warp_frequency"] = number_field(d.domain_warp_frequency, "Scale of the warp.");
    p["domain_warp_fractal_type"] =
        enum_field(kWarpFractals, d.domain_warp_fractal_type, "How warp octaves combine.");
    p["domain_warp_fractal_octaves"] =
        integer_field(d.domain_warp_fractal_octaves, "Layers of warp.", 1, 16);
    p["domain_warp_fractal_lacunarity"] = number_field(
        d.domain_warp_fractal_lacunarity, "Warp frequency multiplier from one octave to the next.");
    p["domain_warp_fractal_gain"] = number_field(
        d.domain_warp_fractal_gain, "Warp strength multiplier from one octave to the next.");
    return schema;
}

std::vector<json::ValidationError> validate_noise(const json::Value& doc, const std::string& at) {
    std::vector<json::ValidationError> errors = json::validate(doc, noise_schema());
    for (json::ValidationError& error : errors) {
        error.path = at + error.path;
    }
    return errors;
}

Result<NoiseSettings> parse_noise(const json::Value& doc) {
    if (const auto errors = validate_noise(doc); !errors.empty()) {
        return detail::first_error(errors);
    }
    using detail::read;
    NoiseSettings s;
    read(doc, "noise_type", kNoiseTypes, s.noise_type);
    read(doc, "seed", s.seed);
    read(doc, "frequency", s.frequency);
    if (const json::Value* offset = doc.find("offset"); offset != nullptr) {
        const json::Array& items = offset->as_array();
        s.offset.x = static_cast<float>(items[0].as_double());
        s.offset.y = static_cast<float>(items[1].as_double());
        s.offset.z = items.size() > 2 ? static_cast<float>(items[2].as_double()) : 0.0F;
    }
    read(doc, "fractal_type", kFractalTypes, s.fractal_type);
    read(doc, "fractal_octaves", s.fractal_octaves);
    read(doc, "fractal_lacunarity", s.fractal_lacunarity);
    read(doc, "fractal_gain", s.fractal_gain);
    read(doc, "fractal_weighted_strength", s.fractal_weighted_strength);
    read(doc, "fractal_ping_pong_strength", s.fractal_ping_pong_strength);
    read(doc, "cellular_distance_function", kCellularDistances, s.cellular_distance_function);
    read(doc, "cellular_return_type", kCellularReturns, s.cellular_return_type);
    read(doc, "cellular_jitter", s.cellular_jitter);
    read(doc, "domain_warp_enabled", s.domain_warp_enabled);
    read(doc, "domain_warp_type", kWarpTypes, s.domain_warp_type);
    read(doc, "domain_warp_amplitude", s.domain_warp_amplitude);
    read(doc, "domain_warp_frequency", s.domain_warp_frequency);
    read(doc, "domain_warp_fractal_type", kWarpFractals, s.domain_warp_fractal_type);
    read(doc, "domain_warp_fractal_octaves", s.domain_warp_fractal_octaves);
    read(doc, "domain_warp_fractal_lacunarity", s.domain_warp_fractal_lacunarity);
    read(doc, "domain_warp_fractal_gain", s.domain_warp_fractal_gain);
    return s;
}

json::Value noise_to_json(const NoiseSettings& s) {
    using detail::write;
    const NoiseSettings d;
    json::Value out = json::Object{};
    write(out, "noise_type", kNoiseTypes, s.noise_type, d.noise_type);
    write(out, "seed", s.seed, d.seed);
    write(out, "frequency", s.frequency, d.frequency);
    if (s.offset != d.offset) {
        json::Array offset{s.offset.x, s.offset.y};
        if (s.offset.z != 0.0F) {
            offset.emplace_back(s.offset.z);
        }
        out["offset"] = std::move(offset);
    }
    write(out, "fractal_type", kFractalTypes, s.fractal_type, d.fractal_type);
    write(out, "fractal_octaves", s.fractal_octaves, d.fractal_octaves);
    write(out, "fractal_lacunarity", s.fractal_lacunarity, d.fractal_lacunarity);
    write(out, "fractal_gain", s.fractal_gain, d.fractal_gain);
    write(out, "fractal_weighted_strength", s.fractal_weighted_strength,
          d.fractal_weighted_strength);
    write(out, "fractal_ping_pong_strength", s.fractal_ping_pong_strength,
          d.fractal_ping_pong_strength);
    write(out, "cellular_distance_function", kCellularDistances, s.cellular_distance_function,
          d.cellular_distance_function);
    write(out, "cellular_return_type", kCellularReturns, s.cellular_return_type,
          d.cellular_return_type);
    write(out, "cellular_jitter", s.cellular_jitter, d.cellular_jitter);
    write(out, "domain_warp_enabled", s.domain_warp_enabled, d.domain_warp_enabled);
    write(out, "domain_warp_type", kWarpTypes, s.domain_warp_type, d.domain_warp_type);
    write(out, "domain_warp_amplitude", s.domain_warp_amplitude, d.domain_warp_amplitude);
    write(out, "domain_warp_frequency", s.domain_warp_frequency, d.domain_warp_frequency);
    write(out, "domain_warp_fractal_type", kWarpFractals, s.domain_warp_fractal_type,
          d.domain_warp_fractal_type);
    write(out, "domain_warp_fractal_octaves", s.domain_warp_fractal_octaves,
          d.domain_warp_fractal_octaves);
    write(out, "domain_warp_fractal_lacunarity", s.domain_warp_fractal_lacunarity,
          d.domain_warp_fractal_lacunarity);
    write(out, "domain_warp_fractal_gain", s.domain_warp_fractal_gain, d.domain_warp_fractal_gain);
    return out;
}

} // namespace slash::procedural
