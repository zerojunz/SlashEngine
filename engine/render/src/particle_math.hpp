#pragma once

#include "slash/core/math.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// What the CPU twins of the particle shaders share, in 2D (particles.cpp) and 3D
// (particles3d.cpp): GLSL's helpers, Godot's random numbers, hue turn and turbulence noise.
// shaders/particles_process.hlsl has the same; a change to one goes in the other.
namespace slash::render::particle_math {

// The value of pi Godot's generated shader uses.
inline constexpr float kPi = 3.14159F; // NOLINT(modernize-use-std-numbers): Godot's, not std's
inline constexpr float kDegreesToRadians = kPi / 180.0F;

// GLSL's mix: a * (1 - t) + b * t.
inline float mix(float a, float b, float t) {
    return (a * (1.0F - t)) + (b * t);
}

inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

inline Vec3 mix(Vec3 a, Vec3 b, float t) {
    return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
}

inline float sign(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

inline Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

inline Vec4 times(Vec4 a, float s) {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}

inline Vec4 plus(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

// GLSL's mod: x - y * floor(x / y).
inline float mod(float x, float y) {
    return x - (y * std::floor(x / y));
}

// Godot's hash, which improves a seed.
inline std::uint32_t hash(std::uint32_t x) {
    x = ((x >> 16U) ^ x) * 73244475U;
    x = ((x >> 16U) ^ x) * 73244475U;
    x = (x >> 16U) ^ x;
    return x;
}

// Godot's rand_from_seed: Park and Miller's generator, a number from 0 to 1.
inline float rand_from_seed(std::uint32_t& seed) {
    auto s = static_cast<std::int32_t>(seed);
    if (s == 0) {
        s = 305420679;
    }
    const std::int32_t k = s / 127773;
    s = (16807 * (s - (k * 127773))) - (2836 * k);
    if (s < 0) {
        s += 2147483647;
    }
    seed = static_cast<std::uint32_t>(s);
    return static_cast<float>(seed % 65536U) / 65535.0F;
}

inline float rand_m1_p1(std::uint32_t& seed) {
    return (rand_from_seed(seed) * 2.0F) - 1.0F;
}

// ---- Turbulence: the noise ParticleProcessMaterial generates for it.

using Coord = std::array<float, 4>;

inline float dot4(const Coord& a, const Coord& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

// 16 bits as a number from -1 to 1, exactly on every device.
inline float unit_of(std::uint32_t bits) {
    return (static_cast<float>(bits & 0xFFFFU) * (1.0F / 32768.0F)) - 1.0F;
}

// Godot's grad: a lattice point's gradient. Godot hashes the point with fractions of float
// products, which round differently on different GPUs; this hashes its integer coordinates, and
// channel tells apart the three noises curl_3d samples, which Godot moves far apart instead.
inline Coord grad(const std::array<std::int32_t, 4>& cell, std::uint32_t channel) {
    std::uint32_t h =
        hash(static_cast<std::uint32_t>(cell[0]) + (channel * 0x632BE59BU) + 0x9E3779B9U);
    h = hash(h + static_cast<std::uint32_t>(cell[1]));
    h = hash(h + static_cast<std::uint32_t>(cell[2]));
    h = hash(h + static_cast<std::uint32_t>(cell[3]));
    const std::uint32_t g = hash(h + 1U);
    return {unit_of(h), unit_of(h >> 16U), unit_of(g), unit_of(g >> 16U)};
}

// Godot's noise: 4D gradient noise, its domain turned to improve the look of slices.
inline float noise(Coord coord, std::uint32_t channel) {
    constexpr float kSixth = -0.1666667F;
    const float shift = dot4(coord, {kSixth, kSixth, kSixth, -0.5F});
    coord = {coord[0] + shift, coord[1] + shift, coord[2] + shift,
             dot4(coord, {0.5F, 0.5F, 0.5F, 0.5F})};
    std::array<std::int32_t, 4> base{};
    Coord delta{};
    Coord fade{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float floored = std::floor(coord.at(i));
        base.at(i) = static_cast<std::int32_t>(floored);
        const float d = coord.at(i) - floored;
        delta.at(i) = d;
        fade.at(i) = d * d * d * (10.0F + (d * (-15.0F + (d * 6.0F))));
    }
    // Each corner's dot with its gradient, weighted by the fade along each axis: Godot's mixes
    // over w, z, y and x as one sum, which the compute shader runs as a loop rather than as
    // sixteen copies of the hash.
    float result = 0.0F;
    for (std::uint32_t corner = 0; corner < 16; ++corner) {
        std::array<std::int32_t, 4> cell = base;
        Coord offset = delta;
        float weight = 1.0F;
        for (std::size_t axis = 0; axis < 4; ++axis) {
            if ((corner & (1U << axis)) != 0) {
                cell.at(axis) += 1;
                offset.at(axis) -= 1.0F;
                weight *= fade.at(axis);
            } else {
                weight *= 1.0F - fade.at(axis);
            }
        }
        result += weight * dot4(offset, grad(cell, channel));
    }
    return result;
}

// Godot's curl_3d: the curl of three noises, from differences epsilon either side. Godot samples
// each noise at all six places; the curl uses two at each, taken here in the order its sums take
// them: term i adds to component i / 2, plus for even i and minus for odd.
inline Vec3 curl_3d(const Coord& p, float c) {
    const float epsilon = 0.001F + c;
    std::array<float, 3> curl{};
    for (std::uint32_t term = 0; term < 6; ++term) {
        const std::uint32_t component = term / 2;
        const std::uint32_t odd = term % 2;
        const std::size_t axis = (component + 1 + odd) % 3;
        const std::uint32_t channel = (component + 2 - odd) % 3;
        Coord ahead = p;
        Coord behind = p;
        ahead.at(axis) += epsilon;
        behind.at(axis) -= epsilon;
        const float difference = noise(ahead, channel) - noise(behind, channel);
        curl.at(component) += odd != 0 ? -difference : difference;
    }
    return normalize(Vec3{curl[0], curl[1], curl[2]});
}

// Godot's rotate_hue: a turn of the color about the gray axis.
inline Vec4 rotate_hue(Vec4 color, float angle) {
    const Vec2 turn = cos_sin(angle);
    const float c = turn.x;
    const float s = turn.y;
    // Columns of the matrix (each [r, g, b] of a column; alpha passes through).
    constexpr std::array<std::array<float, 3>, 3> kBase{
        {{0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}}};
    constexpr std::array<std::array<float, 3>, 3> kCos{
        {{0.701F, -0.587F, -0.114F}, {-0.299F, 0.413F, -0.114F}, {-0.300F, -0.588F, 0.886F}}};
    constexpr std::array<std::array<float, 3>, 3> kSin{
        {{0.168F, 0.330F, -0.497F}, {-0.328F, 0.035F, 0.292F}, {1.250F, -1.050F, -0.203F}}};
    const std::array<float, 3> in{color.x, color.y, color.z};
    std::array<float, 3> out{};
    for (std::size_t row = 0; row < 3; ++row) {
        float sum = 0.0F;
        for (std::size_t column = 0; column < 3; ++column) {
            const float m = kBase[column][row] + (kCos[column][row] * c) + (kSin[column][row] * s);
            sum += m * in[column];
        }
        out[row] = sum;
    }
    return {out[0], out[1], out[2], color.w};
}

} // namespace slash::render::particle_math
