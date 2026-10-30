#include "particle_layout_3d.hpp"

#include "particle_math.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

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

Word floats(Vec3 v, float w) {
    return floats(v.x, v.y, v.z, w);
}

Word floats(Vec4 v) {
    return floats(v.x, v.y, v.z, v.w);
}

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

void pack_particle_process_3d(const ParticleProcess3D& process, std::vector<Word>& out) {
    using particle_math::kDegreesToRadians;
    out.reserve(out.size() + kPointsAt3D + (process.emission_points.size() * kPointWords3D));
    // The ring's top radius and skew, as the CPU twin works them out for each particle.
    const float radius = std::max(0.001F, process.emission_ring_radius);
    const Vec2 cone = cos_sin((90.0F - process.emission_ring_cone_angle) * kDegreesToRadians);
    const float top = std::max(radius - ((cone.y / cone.x) * process.emission_ring_height), 0.0F);
    const float skew = std::max(std::min(radius, top) / std::max(radius, top), 0.5F);
    const Vec3 axis = process.emission_ring_axis == Vec3{}
                          ? Vec3{0.0F, 0.0F, 1.0F}
                          : particle_math::normalize(process.emission_ring_axis);
    const std::uint32_t flags =
        (process.align_y ? kProcessAlignY : 0U) | (process.rotate_y ? kProcessRotateY : 0U) |
        (process.disable_z ? kProcessDisableZ : 0U) |
        (process.damping_as_friction ? kProcessDampingAsFriction : 0U) |
        (process.turbulence_enabled ? kProcessTurbulence : 0U) |
        (process.directional_velocity_global ? kProcessDirectionalGlobal : 0U) |
        (process.attractor_interaction ? kProcessAttractors : 0U);
    out.push_back({static_cast<std::uint32_t>(process.emission_shape),
                   bits(process.emission_sphere_radius), bits(process.emission_ring_radius),
                   bits(process.emission_ring_inner_radius)});
    out.push_back(floats(process.emission_shape_offset, process.emission_ring_height));
    out.push_back(floats(process.emission_shape_scale, top));
    out.push_back({bits(process.emission_box_extents.x), bits(process.emission_box_extents.y),
                   bits(process.emission_box_extents.z),
                   static_cast<std::uint32_t>(process.emission_points.size())});
    out.push_back({bits(axis.x), bits(axis.y), bits(axis.z), flags});
    out.push_back(floats(process.direction, process.spread));
    out.push_back(floats(process.velocity_pivot, process.flatness));
    out.push_back(floats(process.gravity, process.inherit_velocity_ratio));
    out.push_back(floats(process.color));
    out.push_back(floats(process.lifetime_randomness, process.directional_velocity_min,
                         process.directional_velocity_max, skew));
    out.push_back(floats(process.scale_over_velocity_max - process.scale_over_velocity_min,
                         process.turbulence_noise_strength,
                         turbulence_frequency(process.turbulence_noise_scale),
                         process.turbulence_noise_speed_random));
    out.push_back(floats(process.turbulence_noise_speed, 0.0F));
    pack_sixteen(process.param_min, out);
    pack_sixteen(process.param_max, out);

    std::array<const ParticleCurve*, kCurves3D> curves{};
    for (std::size_t i = 0; i < kParticleParams; ++i) {
        curves.at(i) = &process.param_curve.at(i);
    }
    curves.at(kParticleParams) = &process.velocity_limit_curve;
    curves.at(kParticleParams + 1) = &process.alpha_curve;
    curves.at(kParticleParams + 2) = &process.emission_curve;
    std::size_t next = kParticleParams + 3;
    for (const ParticleCurve3* curve :
         {&process.scale_curve, &process.orbit_velocity_curve, &process.directional_velocity_curve,
          &process.scale_over_velocity_curve}) {
        for (const ParticleCurve& axis_curve : curve->axes) {
            curves.at(next++) = &axis_curve;
        }
    }
    for (std::size_t word = 0; word < kGradientCountsAt3D - kCurveCountsAt3D; ++word) {
        Word counts{};
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::size_t at = (word * 4) + lane;
            counts.at(lane) =
                at < kCurves3D ? static_cast<std::uint32_t>(curves.at(at)->size()) : 0U;
        }
        out.push_back(counts);
    }
    out.push_back({static_cast<std::uint32_t>(process.color_ramp.size()),
                   static_cast<std::uint32_t>(process.color_initial_ramp.size()), 0U, 0U});
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
        out.push_back(floats(process.emission_points[i], 0.0F));
        out.push_back(floats(
            i < process.emission_normals.size() ? process.emission_normals[i] : Vec3{}, 0.0F));
        out.push_back(floats(i < process.emission_colors.size() ? process.emission_colors[i]
                                                                : Vec4{1.0F, 1.0F, 1.0F, 1.0F}));
    }
}

void pack_particle_steps_3d(std::span<const ParticleStep3D> steps, std::vector<Word>& out) {
    out.reserve(out.size() + (steps.size() * kStepWords3D));
    const std::size_t first = out.size();
    for (const ParticleStep3D& step : steps) {
        const ParticleTransform& t = step.emission;
        out.push_back(floats(t[0], t[1], t[2], t[3]));
        out.push_back(floats(t[4], t[5], t[6], t[7]));
        out.push_back(floats(t[8], t[9], t[10], t[11]));
        out.push_back(floats(step.emitter_velocity, step.system_phase));
        out.push_back(
            floats(step.prev_system_phase, step.delta, step.lifetime, step.explosiveness));
        out.push_back(floats(step.randomness, step.amount_ratio, step.interp_to_end, step.time));
        const std::uint32_t flags =
            (step.emitting ? kStepEmitting : 0U) |
            (step.fractional_delta ? kStepFractionalDelta : 0U) | (step.clear ? kStepClear : 0U) |
            (static_cast<std::uint32_t>(step.attractors.size()) << kStepAttractorShift);
        out.push_back({step.cycle, step.seed, flags, 0U});
    }
    // Then each step's attractors, and the fields they sample once each.
    std::vector<std::pair<const ParticleField3D*, std::uint32_t>> fields;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ParticleStep3D& step = steps[i];
        if (step.attractors.empty()) {
            continue;
        }
        out[first + (i * kStepWords3D) + 6].at(3) = static_cast<std::uint32_t>(out.size());
        const std::size_t block = out.size();
        out.resize(block + (step.attractors.size() * kAttractorWords3D));
        for (std::size_t a = 0; a < step.attractors.size(); ++a) {
            const ParticleAttractor3D& attractor = step.attractors[a];
            std::uint32_t field_at = 0;
            if (attractor.shape == AttractorShape3D::VectorField && attractor.field != nullptr) {
                const ParticleField3D* field = attractor.field.get();
                const auto found = std::find_if(fields.begin(), fields.end(),
                                                [&](const auto& f) { return f.first == field; });
                if (found != fields.end()) {
                    field_at = found->second;
                } else {
                    field_at = static_cast<std::uint32_t>(out.size());
                    fields.emplace_back(field, field_at);
                    out.push_back({field->width, field->height, field->depth, 0U});
                    for (const Vec4& texel : field->texels) {
                        out.push_back(floats(texel));
                    }
                }
            }
            const ParticleTransform& t = attractor.transform;
            const std::size_t at = block + (a * kAttractorWords3D);
            out[at] = floats(t[0], t[1], t[2], t[3]);
            out[at + 1] = floats(t[4], t[5], t[6], t[7]);
            out[at + 2] = floats(t[8], t[9], t[10], t[11]);
            out[at + 3] = floats(attractor.extents, attractor.strength);
            out[at + 4] = {static_cast<std::uint32_t>(attractor.shape), field_at,
                           bits(attractor.attenuation), bits(attractor.directionality)};
        }
    }
}

void pack_particle_vertices_3d(std::span<const Vertex3D> vertices, std::vector<Word>& out) {
    const std::size_t count = vertices.size() * kVertexWords3D;
    std::vector<std::uint32_t> flat(count);
    std::memcpy(flat.data(), vertices.data(), count * sizeof(std::uint32_t));
    for (std::size_t at = 0; at < count; at += 4) {
        Word word{};
        for (std::size_t lane = 0; lane < 4 && at + lane < count; ++lane) {
            word.at(lane) = flat[at + lane];
        }
        out.push_back(word);
    }
}

std::uint32_t sort_size(std::uint32_t amount) {
    std::uint32_t size = 1;
    while (size < amount) {
        size *= 2;
    }
    return size;
}

std::vector<std::array<std::uint32_t, 2>> bitonic_steps(std::uint32_t size) {
    std::vector<std::array<std::uint32_t, 2>> steps;
    for (std::uint32_t k = 2; k <= size; k *= 2) {
        for (std::uint32_t j = k / 2; j > 0; j /= 2) {
            steps.push_back({j, k});
        }
    }
    return steps;
}

std::array<std::uint32_t, 2> mesh_groups(std::uint32_t threads) {
    constexpr std::uint32_t kRow = 65535;
    const std::uint32_t groups = (threads + kParticleThreads3D - 1) / kParticleThreads3D;
    const std::uint32_t x = std::max(std::min(groups, kRow), 1U);
    return {x, std::max((groups + x - 1) / x, 1U)};
}

MeshUniforms3D mesh_uniforms_3d(const ParticleMeshDraw3D& draw, std::uint32_t amount,
                                std::uint32_t corners, std::uint32_t source_at) {
    MeshUniforms3D out;
    out.counts = {amount, corners, source_at,
                  mesh_groups(amount * corners)[0] * kParticleThreads3D};
    const std::uint32_t flags =
        (draw.keep_scale ? kMeshKeepScale : 0U) | (draw.anim_loop ? kMeshAnimLoop : 0U) |
        (draw.order == ParticleOrder3D::ViewDepth ? kMeshSorted : 0U) |
        (draw.order == ParticleOrder3D::Lifetime ? kMeshLifetime : 0U) |
        (draw.order == ParticleOrder3D::ReverseLifetime ? kMeshReverseLifetime : 0U);
    out.look = {draw.lifetime_split, static_cast<std::uint32_t>(draw.align),
                static_cast<std::uint32_t>(draw.billboard), flags};
    out.frames = {static_cast<float>(std::max(draw.h_frames, 1U)),
                  static_cast<float>(std::max(draw.v_frames, 1U)), draw.remainder, 0.0F};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            out.transform.at(row).at(lane) = draw.transform.at((row * 4) + lane);
            out.camera.at(row).at(lane) = draw.camera.at((row * 4) + lane);
        }
    }
    out.sort_direction = {draw.sort_direction.x, draw.sort_direction.y, draw.sort_direction.z,
                          0.0F};
    out.align_up = {draw.align_up.x, draw.align_up.y, draw.align_up.z, 0.0F};
    out.uv = {draw.uv_scale[0], draw.uv_scale[1], draw.uv_offset[0], draw.uv_offset[1]};
    return out;
}

} // namespace slash::render::detail
