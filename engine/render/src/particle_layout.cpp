#include "particle_layout.hpp"

#include <algorithm>
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
    const ParticleProgram& start = process.start_program;
    const ParticleProgram& step = process.process_program;
    const auto graph_words = [](const ParticleProgram& graph) {
        return graph.outputs == 0 ? std::size_t{0}
                                  : graph.program.code.size() + graph.program.constants.size();
    };
    const bool graphs = start.outputs != 0 || step.outputs != 0;
    const auto points_end =
        static_cast<std::uint32_t>(kPointsAt + (process.emission_points.size() * 2));
    out.reserve(out.size() + points_end + (graphs ? kGraphHeaderWords : 0) + graph_words(start) +
                graph_words(step));
    out.push_back({static_cast<std::uint32_t>(process.emission_shape),
                   bits(process.emission_sphere_radius), bits(process.emission_ring_radius),
                   bits(process.emission_ring_inner_radius)});
    out.push_back(floats(process.emission_shape_offset.x, process.emission_shape_offset.y,
                         process.emission_shape_scale.x, process.emission_shape_scale.y));
    out.push_back({bits(process.emission_box_extents.x), bits(process.emission_box_extents.y),
                   static_cast<std::uint32_t>(process.emission_points.size()),
                   graphs ? points_end : 0U});
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
    out.push_back({static_cast<std::uint32_t>(process.collision_mode),
                   bits(process.collision_friction), bits(process.collision_bounce),
                   process.collision_use_scale ? 1U : 0U});
    // The mode, the seconds between Constant emissions as Godot passes them, the mode's amount and
    // whether particles started keep the velocity.
    std::uint32_t amount = 1;
    if (process.sub_emitter_mode == SubEmitterMode::AtEnd) {
        amount = process.sub_emitter_amount_at_end;
    } else if (process.sub_emitter_mode == SubEmitterMode::AtCollision) {
        amount = process.sub_emitter_amount_at_collision;
    } else if (process.sub_emitter_mode == SubEmitterMode::AtStart) {
        amount = process.sub_emitter_amount_at_start;
    }
    out.push_back(
        {static_cast<std::uint32_t>(process.sub_emitter_mode),
         bits(static_cast<float>(1.0 / static_cast<double>(process.sub_emitter_frequency))), amount,
         process.sub_emitter_keep_velocity ? 1U : 0U});

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
    if (!graphs) {
        return;
    }
    // A program without outputs runs nothing.
    const auto count = [](const ParticleProgram& graph, std::size_t size) {
        return graph.outputs == 0 ? 0U : static_cast<std::uint32_t>(size);
    };
    out.push_back(
        {count(start, start.program.code.size()), count(start, start.program.constants.size()),
         count(step, step.program.code.size()), count(step, step.program.constants.size())});
    out.push_back({start.outputs, step.outputs, 0U, 0U});
    for (const ParticleProgram* graph : {&start, &step}) {
        if (graph->outputs == 0) {
            continue;
        }
        for (const Instruction& instruction : graph->program.code) {
            out.push_back({instruction.head, instruction.a, instruction.b, instruction.c});
        }
        for (const std::array<float, 4>& constant : graph->program.constants) {
            out.push_back(floats(constant[0], constant[1], constant[2], constant[3]));
        }
    }
}

std::vector<std::uint32_t> pack_particle_shapes(std::span<const ParticleStep> steps,
                                                std::span<const ParticleShape> shapes,
                                                std::vector<Word>& out) {
    // Only the shapes the steps name, which are together when the steps name them in order.
    std::size_t first = shapes.size();
    std::size_t end = 0;
    for (const ParticleStep& step : steps) {
        if (step.shape_count > 0) {
            first = std::min<std::size_t>(first, step.shape_first);
            end = std::max<std::size_t>(end, step.shape_first + step.shape_count);
        }
    }
    std::vector<std::uint32_t> at(shapes.size(), 0U);
    for (std::size_t i = first; i < end && i < shapes.size(); ++i) {
        const ParticleShape& shape = shapes[i];
        at[i] = static_cast<std::uint32_t>(out.size());
        out.push_back(
            {static_cast<std::uint32_t>(shape.points.size()), shape.closed ? 1U : 0U, 0U, 0U});
        for (std::size_t point = 0; point < shape.points.size(); point += 2) {
            const Vec2 a = shape.points[point];
            const Vec2 b = point + 1 < shape.points.size() ? shape.points[point + 1] : Vec2{};
            out.push_back(floats(a.x, a.y, b.x, b.y));
        }
    }
    return at;
}

void pack_particle_steps(std::span<const ParticleStep> steps,
                         std::span<const std::uint32_t> shape_at, std::vector<Word>& out) {
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
        const bool shaped = step.shape_count > 0 && step.shape_first < shape_at.size();
        out.push_back({shaped ? shape_at[step.shape_first] : 0U, shaped ? step.shape_count : 0U,
                       bits(step.collision_size), step.trail_steps});
        out.push_back(
            floats(step.to_shapes[0], step.to_shapes[1], step.to_shapes[2], step.to_shapes[3]));
        out.push_back(floats(step.to_shapes[4], step.to_shapes[5], 0.0F, 0.0F));
        out.push_back(floats(step.from_shapes[0], step.from_shapes[1], step.from_shapes[2],
                             step.from_shapes[3]));
    }
}

DrawUniforms draw_uniforms(const ParticleDraw& look, std::uint32_t amount, std::uint32_t trail,
                           std::uint32_t first_vertex) {
    const std::array<float, 6>& t = look.transform;
    return {.transform_a = {t[0], t[1], t[2], t[3]},
            .transform_b = {t[4], t[5], look.size.x, look.size.y},
            .modulate = {look.modulate.x, look.modulate.y, look.modulate.z, look.modulate.w},
            .look = {look.remainder, static_cast<float>(look.hframes),
                     static_cast<float>(look.vframes), look.anim_loop ? 1.0F : 0.0F},
            .counts = {amount, static_cast<std::uint32_t>(look.order), look.lifetime_split,
                       first_vertex},
            .trail = {trail, look.trail_subdivisions, look.trail_steps, bits(look.step_delta)}};
}

} // namespace slash::render::detail
