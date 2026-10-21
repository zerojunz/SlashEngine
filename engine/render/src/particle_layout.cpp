#include "particle_layout.hpp"

#include <cstring>

namespace slash::render::detail {
namespace {

std::uint32_t bits(float value) {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

Word floats(float x, float y, float z, float w) {
    return {bits(x), bits(y), bits(z), bits(w)};
}

Word floats(Vec4 v) {
    return floats(v.x, v.y, v.z, v.w);
}

// Sixteen values in four words, the ones past values zero.
void pack_sixteen(std::span<const float> values, std::vector<Word>& out) {
    for (std::size_t word = 0; word < 4; ++word) {
        Word packed{};
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::size_t at = (word * 4) + lane;
            packed.at(lane) = at < values.size() ? bits(values[at]) : 0U;
        }
        out.push_back(packed);
    }
}

} // namespace

void pack_particle_process(const ParticleProcess& process, std::vector<Word>& out) {
    out.reserve(out.size() + kPointsAt + (process.emission_points.size() * 2));
    out.push_back({static_cast<std::uint32_t>(process.emission_shape),
                   bits(process.emission_sphere_radius), bits(process.emission_ring_radius),
                   bits(process.emission_ring_inner_radius)});
    out.push_back(floats(process.emission_shape_offset.x, process.emission_shape_offset.y,
                         process.emission_shape_scale.x, process.emission_shape_scale.y));
    out.push_back({bits(process.emission_box_extents.x), bits(process.emission_box_extents.y),
                   static_cast<std::uint32_t>(process.emission_points.size()), 0U});
    out.push_back(floats(process.direction.x, process.direction.y, process.spread,
                         process.inherit_velocity_ratio));
    out.push_back(floats(process.velocity_pivot.x, process.velocity_pivot.y, process.gravity.x,
                         process.gravity.y));
    out.push_back(floats(process.color));
    out.push_back({bits(process.lifetime_randomness), process.align_y ? 1U : 0U,
                   process.damping_as_friction ? 1U : 0U, 0U});
    pack_sixteen(process.param_min, out);
    pack_sixteen(process.param_max, out);

    std::array<const ParticleCurve*, kCurves> curves{};
    for (std::size_t i = 0; i < kParticleParams; ++i) {
        curves.at(i) = &process.param_curve.at(i);
    }
    curves.at(kParticleParams) = &process.velocity_limit_curve;
    curves.at(kParticleParams + 1) = &process.alpha_curve;
    curves.at(kParticleParams + 2) = &process.emission_curve;
    for (std::size_t word = 0; word < kGradientCountsAt - kCurveCountsAt; ++word) {
        Word counts{};
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::size_t at = (word * 4) + lane;
            counts.at(lane) = at < kCurves ? static_cast<std::uint32_t>(curves.at(at)->size()) : 0U;
        }
        out.push_back(counts);
    }
    out.push_back({static_cast<std::uint32_t>(process.color_ramp.size()),
                   static_cast<std::uint32_t>(process.color_initial_ramp.size()), 0U, 0U});
    out.push_back({bits(process.turbulence_noise_strength),
                   bits(turbulence_frequency(process.turbulence_noise_scale)),
                   bits(process.turbulence_noise_speed_random),
                   process.turbulence_enabled ? 1U : 0U});
    out.push_back(floats(process.turbulence_noise_speed.x, process.turbulence_noise_speed.y,
                         process.turbulence_noise_speed.z, 0.0F));

    for (const ParticleCurve* curve : curves) {
        for (std::size_t word = 0; word < kCurveWords; ++word) {
            const std::size_t first = word * 2;
            const Vec2 a = first < curve->size() ? (*curve)[first] : Vec2{};
            const Vec2 b = first + 1 < curve->size() ? (*curve)[first + 1] : Vec2{};
            out.push_back(floats(a.x, a.y, b.x, b.y));
        }
    }
    for (const ParticleGradient* gradient : {&process.color_ramp, &process.color_initial_ramp}) {
        std::array<float, kMaxParticleCurvePoints> offsets{};
        for (std::size_t i = 0; i < gradient->size() && i < offsets.size(); ++i) {
            offsets.at(i) = (*gradient)[i].offset;
        }
        for (std::size_t word = 0; word < kMaxParticleCurvePoints / 4; ++word) {
            out.push_back(floats(offsets.at(word * 4), offsets.at((word * 4) + 1),
                                 offsets.at((word * 4) + 2), offsets.at((word * 4) + 3)));
        }
        for (std::size_t i = 0; i < kMaxParticleCurvePoints; ++i) {
            out.push_back(floats(i < gradient->size() ? (*gradient)[i].color : Vec4{}));
        }
    }
    // A point with no normal stays still and one with no color is white, as in the CPU twin.
    for (std::size_t i = 0; i < process.emission_points.size(); ++i) {
        const Vec2 point = process.emission_points[i];
        const Vec2 normal =
            i < process.emission_normals.size() ? process.emission_normals[i] : Vec2{};
        out.push_back(floats(point.x, point.y, normal.x, normal.y));
        out.push_back(floats(i < process.emission_colors.size() ? process.emission_colors[i]
                                                                : Vec4{1.0F, 1.0F, 1.0F, 1.0F}));
    }
}

void pack_particle_steps(std::span<const ParticleStep> steps, std::vector<Word>& out) {
    out.reserve(out.size() + (steps.size() * kStepWords));
    for (const ParticleStep& step : steps) {
        out.push_back(
            floats(step.emission[0], step.emission[1], step.emission[2], step.emission[3]));
        out.push_back(floats(step.emission[4], step.emission[5], step.emitter_velocity.x,
                             step.emitter_velocity.y));
        out.push_back(floats(step.system_phase, step.prev_system_phase, step.delta, step.lifetime));
        out.push_back(
            floats(step.explosiveness, step.randomness, step.amount_ratio, step.interp_to_end));
        const std::uint32_t flags = (step.emitting ? kStepEmitting : 0U) |
                                    (step.fractional_delta ? kStepFractionalDelta : 0U) |
                                    (step.clear ? kStepClear : 0U);
        out.push_back({step.cycle, step.seed, flags, bits(step.time)});
    }
}

DrawUniforms draw_uniforms(const ParticleDraw& look, std::uint32_t amount,
                           std::uint32_t first_vertex) {
    const std::array<float, 6>& t = look.transform;
    return {.transform_a = {t[0], t[1], t[2], t[3]},
            .transform_b = {t[4], t[5], look.size.x, look.size.y},
            .modulate = {look.modulate.x, look.modulate.y, look.modulate.z, look.modulate.w},
            .look = {look.remainder, static_cast<float>(look.hframes),
                     static_cast<float>(look.vframes), look.anim_loop ? 1.0F : 0.0F},
            .counts = {amount, static_cast<std::uint32_t>(look.order), look.lifetime_split,
                       first_vertex}};
}

} // namespace slash::render::detail
