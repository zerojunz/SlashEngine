#pragma once

#include "slash/render/particles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// How the GPU device hands a process material and steps to shaders/particles_process.hlsl, and a
// draw to particles_draw.hlsl: as 16-byte words of 32-bit values, floats as their bits. The
// shaders read them at the same places; a change here goes there too.
namespace slash::render::detail {

using Word = std::array<std::uint32_t, 4>;

// A process material's words: a header, the curves, the gradients, then two words a point.
inline constexpr std::uint32_t kCurveWords = kMaxParticleCurvePoints / 2; // two points a word
// The parameters' curves, then velocity_limit_curve, alpha_curve and emission_curve.
inline constexpr std::uint32_t kCurves = kParticleParams + 3;
inline constexpr std::uint32_t kGradientWords =
    (kMaxParticleCurvePoints / 4) + kMaxParticleCurvePoints;
// Words 0 to 6 hold the settings, 7 to 14 the parameters' ranges, then a word for every four
// curves' point counts, one for the gradients' stop counts, two for turbulence and one for
// collision.
inline constexpr std::uint32_t kCurveCountsAt = 15;
inline constexpr std::uint32_t kGradientCountsAt = kCurveCountsAt + ((kCurves + 3) / 4);
inline constexpr std::uint32_t kTurbulenceAt = kGradientCountsAt + 1;
inline constexpr std::uint32_t kCollisionAt = kTurbulenceAt + 2;
inline constexpr std::uint32_t kCurvesAt = kCollisionAt + 1;
inline constexpr std::uint32_t kGradientsAt = kCurvesAt + (kCurves * kCurveWords);
inline constexpr std::uint32_t kPointsAt = kGradientsAt + (2 * kGradientWords);
// The places particles_process.hlsl reads them at.
static_assert(kCurveCountsAt == 15 && kGradientCountsAt == 20 && kTurbulenceAt == 21);
static_assert(kCollisionAt == 23 && kCurvesAt == 24 && kGradientsAt == 312 && kPointsAt == 392);
// A step's words.
inline constexpr std::uint32_t kStepWords = 9;
// A step's flags, in its last word.
inline constexpr std::uint32_t kStepEmitting = 1U;
inline constexpr std::uint32_t kStepFractionalDelta = 2U;
inline constexpr std::uint32_t kStepClear = 4U;

// Appends a process material's words.
void pack_particle_process(const ParticleProcess& process, std::vector<Word>& out);
// Appends the shapes the steps collide with, each a word {point count, closed, 0, 0} then its
// points two a word, and returns where each of shapes starts (0 for those no step names).
[[nodiscard]] std::vector<std::uint32_t> pack_particle_shapes(std::span<const ParticleStep> steps,
                                                              std::span<const ParticleShape> shapes,
                                                              std::vector<Word>& out);
// Appends steps' words, with shape_at from pack_particle_shapes.
void pack_particle_steps(std::span<const ParticleStep> steps,
                         std::span<const std::uint32_t> shape_at, std::vector<Word>& out);

// particles_process.hlsl's uniforms: offsets are in words.
struct ProcessUniforms {
    std::uint32_t amount = 0;
    std::uint32_t material = 0;
    std::uint32_t steps = 0;
    std::uint32_t step_count = 0;
    std::uint32_t reset = 0; // clears every particle first
    std::array<std::uint32_t, 3> padding{};
};

// particles_draw.hlsl's uniforms.
struct DrawUniforms {
    std::array<float, 4> transform_a{}; // ParticleDraw::transform[0..3]
    std::array<float, 4> transform_b{}; // [4], [5], then size
    std::array<float, 4> modulate{};
    std::array<float, 4> look{};           // remainder, hframes, vframes, anim_loop
    std::array<std::uint32_t, 4> counts{}; // amount, order, lifetime_split, first vertex
};

[[nodiscard]] DrawUniforms draw_uniforms(const ParticleDraw& look, std::uint32_t amount,
                                         std::uint32_t first_vertex);

} // namespace slash::render::detail
