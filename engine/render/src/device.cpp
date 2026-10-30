#include "slash/render/device.hpp"

#include "checks.hpp"
#include "gl_device.hpp"
#include "gpu_device.hpp"
#include "mesh_shading.hpp"
#include "post_shading.hpp"
#include "reflection.hpp"
#include "sky_shading.hpp"
#ifdef __EMSCRIPTEN__
#include "webgpu_device.hpp"
#endif

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slash::render {

namespace detail {

Result<void> check_texture_desc(const TextureDesc& desc) {
    if (desc.cubes > 0) {
        if (desc.cubes > kMaxReflectionCubes) {
            return make_error(ErrorCode::InvalidArgument, "a reflection atlas holds 1 to " +
                                                              std::to_string(kMaxReflectionCubes) +
                                                              " cubes, not " +
                                                              std::to_string(desc.cubes));
        }
        if (desc.width != desc.height || desc.width < 32 || desc.width > 2048 ||
            (desc.width & (desc.width - 1)) != 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "a reflection atlas's faces are square, a power of two from 32 to "
                              "2048 texels across, not " +
                                  std::to_string(desc.width) + "x" + std::to_string(desc.height));
        }
        if (desc.render_target || desc.format != TextureFormat::Rgba8) {
            return make_error(ErrorCode::InvalidArgument,
                              "a reflection atlas is neither a render target nor compressed");
        }
        return {};
    }
    if (desc.width == 0 || desc.height == 0 || desc.width > kMaxTextureSize ||
        desc.height > kMaxTextureSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "a texture is 1 to " + std::to_string(kMaxTextureSize) +
                              " pixels wide and high, not " + std::to_string(desc.width) + "x" +
                              std::to_string(desc.height));
    }
    if (is_compressed(desc.format)) {
        const std::string name(texture_format_name(desc.format));
        if (desc.width % 4 != 0 || desc.height % 4 != 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "a " + name + " texture is whole 4x4 blocks, so its sides are " +
                                  "multiples of 4, not " + std::to_string(desc.width) + "x" +
                                  std::to_string(desc.height));
        }
        if (desc.render_target) {
            return make_error(ErrorCode::InvalidArgument,
                              "only RGBA8 textures are render targets, not " + name + " ones");
        }
    }
    if (desc.scaled_into && !desc.render_target) {
        return make_error(ErrorCode::InvalidArgument,
                          "only render targets can be scaled into (scaled_into)");
    }
    if (desc.format == TextureFormat::Rgba16F && desc.render_target) {
        return make_error(ErrorCode::InvalidArgument,
                          "only RGBA8 textures are render targets, not rgba16f ones");
    }
    return {};
}

Result<void> check_pixels(const TextureDesc& desc, std::span<const std::uint8_t> texels,
                          bool allow_empty) {
    if (desc.cubes > 0) {
        if (allow_empty && texels.empty()) {
            return {};
        }
        return make_error(ErrorCode::InvalidArgument,
                          "a reflection atlas takes no texels: captures draw its cubes");
    }
    const std::uint64_t expected = texture_bytes(desc);
    if ((allow_empty && texels.empty() && !is_compressed(desc.format)) ||
        texels.size() == expected) {
        return {};
    }
    std::string what = "RGBA";
    if (is_compressed(desc.format)) {
        what = std::string(texture_format_name(desc.format)) + " blocks";
    } else if (desc.format == TextureFormat::Rgba16F) {
        what = "half-float RGBA";
    }
    return make_error(ErrorCode::InvalidArgument,
                      "a " + std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                          " texture takes " + std::to_string(expected) + " bytes of " + what +
                          ", not " + std::to_string(texels.size()));
}

Result<void> check_vertices(std::span<const Vertex2D> vertices, std::size_t in_frame) {
    if (vertices.size() % 3 != 0) {
        return make_error(ErrorCode::InvalidArgument, "every three vertices make a triangle, so " +
                                                          std::to_string(vertices.size()) +
                                                          " vertices is not a whole number "
                                                          "of triangles");
    }
    if (vertices.size() > kMaxFrameVertices - in_frame) {
        return make_error(ErrorCode::InvalidArgument,
                          "a frame draws up to " + std::to_string(kMaxFrameVertices) +
                              " vertices; this draw would make " +
                              std::to_string(in_frame + vertices.size()));
    }
    return {};
}

Error unknown_texture(TextureId texture) {
    return make_error(ErrorCode::NotFound,
                      "no texture " + std::to_string(static_cast<std::uint32_t>(texture)));
}

Error unknown_shader(ShaderId shader) {
    return make_error(ErrorCode::NotFound,
                      "no shader " + std::to_string(static_cast<std::uint32_t>(shader)));
}

Result<void> check_compiled_shader(const CompiledShader& shader, Backend backend) {
    bool present = true;
    switch (backend) {
    case Backend::Vulkan:
        present = !shader.spirv_vertex.empty() && !shader.spirv_fragment.empty();
        break;
    case Backend::Metal:
        present = !shader.msl_vertex.empty() && !shader.msl_fragment.empty();
        break;
    case Backend::D3D12:
        present = !shader.hlsl.empty();
        break;
    case Backend::OpenGl:
    case Backend::OpenGlEs:
        present = !shader.glsl_vertex.empty() && !shader.glsl_fragment.empty();
        break;
    case Backend::WebGpu:
        present = !shader.wgsl_vertex.empty() && !shader.wgsl_fragment.empty();
        break;
    case Backend::Software:
        present = !shader.cpu_fragment.empty();
        break;
    case Backend::Null:
        break;
    }
    if (!present) {
        return make_error(ErrorCode::InvalidArgument, "the shader has no code for the " +
                                                          std::string(backend_name(backend)) +
                                                          " backend");
    }
    return {};
}

Error unknown_particles(ParticlesId particles) {
    return make_error(ErrorCode::NotFound,
                      "no particle system " +
                          std::to_string(static_cast<std::uint32_t>(particles)));
}

Result<void> check_particle_amount(std::uint32_t amount, std::uint32_t trail) {
    if (amount < 1 || amount > kMaxParticles) {
        return make_error(ErrorCode::InvalidArgument,
                          "a particle system has from 1 to " + std::to_string(kMaxParticles) +
                              " particles, not " + std::to_string(amount));
    }
    if (trail < 1 || trail > kMaxParticleTrail) {
        return make_error(ErrorCode::InvalidArgument,
                          "a particle trail has from 2 to " + std::to_string(kMaxParticleTrail) +
                              " particles, or 1 for none, not " + std::to_string(trail));
    }
    if (std::uint64_t{amount} * trail > kMaxParticles) {
        return make_error(ErrorCode::InvalidArgument,
                          "a particle system with trails has at most " +
                              std::to_string(kMaxParticles) + " particles with its trails; " +
                              std::to_string(amount) + " with trails of " + std::to_string(trail) +
                              " would make " + std::to_string(std::uint64_t{amount} * trail));
    }
    return {};
}

namespace {

bool finite(float value) {
    return std::isfinite(value);
}

// That a curve or gradient has at most kMaxParticleCurvePoints points, their offsets in order.
template <typename OffsetAt>
Result<void> check_offsets(std::string_view what, std::size_t count, const OffsetAt& offset_at) {
    if (count > kMaxParticleCurvePoints) {
        return make_error(ErrorCode::InvalidArgument, std::string(what) + " has more than " +
                                                          std::to_string(kMaxParticleCurvePoints) +
                                                          " points");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float offset = offset_at(i);
        if (!finite(offset) || (i > 0 && offset < offset_at(i - 1))) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(what) + "'s offsets must be numbers in order");
        }
    }
    return {};
}

Result<void> check_curve(std::string_view what, const ParticleCurve& curve) {
    return check_offsets(what, curve.size(), [&](std::size_t i) { return curve[i].x; });
}

// The curves and gradients 2D and 3D process materials share.
template <typename Process>
Result<void> check_curves(const Process& process) {
    for (const ParticleCurve& curve : process.param_curve) {
        if (auto checked = check_curve("a parameter's curve", curve); !checked) {
            return checked;
        }
    }
    for (const auto& [what, curve] :
         {std::pair{"velocity_limit_curve", &process.velocity_limit_curve},
          std::pair{"alpha_curve", &process.alpha_curve},
          std::pair{"emission_curve", &process.emission_curve}}) {
        if (auto checked = check_curve(what, *curve); !checked) {
            return checked;
        }
    }
    for (const auto& [what, gradient] :
         {std::pair{"color_ramp", &process.color_ramp},
          std::pair{"color_initial_ramp", &process.color_initial_ramp}}) {
        const ParticleGradient& stops = *gradient;
        if (auto checked = check_offsets(what, stops.size(),
                                         [&stops](std::size_t i) { return stops[i].offset; });
            !checked) {
            return checked;
        }
    }
    return {};
}

// The most emission points a process material has.
constexpr std::size_t kMaxEmissionPoints = 65536;

} // namespace

Result<void> check_particle_process(const ParticleProcess& process,
                                    std::span<const ParticleStep> steps,
                                    std::span<const ParticleShape> shapes) {
    if (auto checked = check_curves(process); !checked) {
        return checked;
    }
    if (process.sub_emitter_mode == SubEmitterMode::Constant &&
        (!(process.sub_emitter_frequency > 0.0F) || !finite(process.sub_emitter_frequency))) {
        return make_error(ErrorCode::InvalidArgument, "sub_emitter_frequency is more than 0");
    }
    for (const auto& [what, graph, outputs, parts] :
         {std::tuple{"the start graph", &process.start_program, kParticleStartOutputs,
                     "position, velocity and active"},
          std::tuple{"the process graph", &process.process_program, kParticleProcessOutputs,
                     "position, velocity, color, angle, scale, frame and active"}}) {
        if (graph->outputs == 0) {
            continue;
        }
        if ((graph->outputs & ~outputs) != 0) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(what) + " changes only " + parts);
        }
        if (auto checked = check_program(graph->program, kParticleFirstFreeRegister, false);
            !checked) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(what) + ": " + checked.error().message);
        }
    }
    if (process.emission_points.size() > kMaxEmissionPoints) {
        return make_error(ErrorCode::InvalidArgument, "a process material has at most " +
                                                          std::to_string(kMaxEmissionPoints) +
                                                          " emission points");
    }
    for (const ParticleStep& step : steps) {
        if (!(step.lifetime > 0.0F) || !finite(step.lifetime) || !(step.delta >= 0.0F) ||
            !finite(step.delta)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's lifetime is more than 0 and its delta 0 or more");
        }
        if (!(step.collision_size >= 0.0F) || !finite(step.collision_size)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's collision size is 0 or more");
        }
        if (step.trail_steps < 1 || step.trail_steps > kMaxParticleTrailSteps) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's trail_steps is from 1 to " +
                                  std::to_string(kMaxParticleTrailSteps));
        }
        if (step.shape_count == 0) {
            continue;
        }
        if (!std::ranges::all_of(step.to_shapes, finite) ||
            !std::ranges::all_of(step.from_shapes, finite)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's shape transforms are numbers");
        }
        if (static_cast<std::size_t>(step.shape_first) + step.shape_count > shapes.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step names shapes " + std::to_string(step.shape_first) +
                                  " to " + std::to_string(step.shape_first + step.shape_count - 1) +
                                  " of " + std::to_string(shapes.size()));
        }
        std::size_t points = 0;
        for (const ParticleShape& shape : shapes.subspan(step.shape_first, step.shape_count)) {
            points += shape.points.size();
            for (const Vec2& point : shape.points) {
                if (!finite(point.x) || !finite(point.y)) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "a particle shape's points are numbers");
                }
            }
        }
        if (points > kMaxParticleShapePoints) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's shapes have at most " +
                                  std::to_string(kMaxParticleShapePoints) + " points");
        }
    }
    return {};
}

Result<void> check_particle_amount_3d(std::uint32_t amount) {
    if (amount == 0 || amount > kMaxParticles3D) {
        return make_error(ErrorCode::InvalidArgument, "a 3D particle system holds from 1 to " +
                                                          std::to_string(kMaxParticles3D) +
                                                          " particles, not " +
                                                          std::to_string(amount));
    }
    return {};
}

Result<void> check_particle_process_3d(const ParticleProcess3D& process,
                                       std::span<const ParticleStep3D> steps) {
    if (auto checked = check_curves(process); !checked) {
        return checked;
    }
    for (const auto& [what, curve] :
         {std::pair{"scale_curve", &process.scale_curve},
          std::pair{"orbit_velocity_curve", &process.orbit_velocity_curve},
          std::pair{"directional_velocity_curve", &process.directional_velocity_curve},
          std::pair{"scale_over_velocity_curve", &process.scale_over_velocity_curve}}) {
        for (const ParticleCurve& axis : curve->axes) {
            if (auto checked = check_curve(what, axis); !checked) {
                return checked;
            }
        }
    }
    if (process.emission_points.size() > kMaxEmissionPoints) {
        return make_error(ErrorCode::InvalidArgument, "a process material has at most " +
                                                          std::to_string(kMaxEmissionPoints) +
                                                          " emission points");
    }
    for (const ParticleStep3D& step : steps) {
        if (!(step.lifetime > 0.0F) || !finite(step.lifetime) || !(step.delta >= 0.0F) ||
            !finite(step.delta)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's lifetime is more than 0 and its delta 0 or more");
        }
        if (step.attractors.size() > kMaxParticleAttractors3D) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step has at most " +
                                  std::to_string(kMaxParticleAttractors3D) + " attractors");
        }
        if (step.colliders.size() > kMaxParticleColliders3D) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step has at most " +
                                  std::to_string(kMaxParticleColliders3D) + " colliders");
        }
        if (!(step.collision_size >= 0.0F) || !finite(step.collision_size)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle step's collision size is 0 or more");
        }
        std::size_t height_fields = 0;
        for (const ParticleCollider3D& collider : step.colliders) {
            if (collider.shape == ColliderShape3D::Sdf) {
                const ParticleSdf3D* sdf = collider.sdf.get();
                if (sdf == nullptr || sdf->width == 0 || sdf->height == 0 || sdf->depth == 0 ||
                    sdf->distances.size() != std::size_t{sdf->width} * sdf->height * sdf->depth ||
                    sdf->distances.size() > kMaxParticleSdfCells3D) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "an SDF collider needs width times height times depth "
                                      "distances, at most " +
                                          std::to_string(kMaxParticleSdfCells3D));
                }
                if (!finite(collider.scale)) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "an SDF collider's scale is a number");
                }
            } else if (collider.shape == ColliderShape3D::HeightField) {
                const ParticleHeightField3D* field = collider.heights.get();
                if (++height_fields > 1) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "a particle step has at most one height field");
                }
                if (field == nullptr || field->width == 0 || field->height == 0 ||
                    field->heights.size() != std::size_t{field->width} * field->height ||
                    field->heights.size() > kMaxParticleHeightTexels3D) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "a height field collider needs width times height heights, "
                                      "at most " +
                                          std::to_string(kMaxParticleHeightTexels3D));
                }
            }
        }
        for (const ParticleAttractor3D& attractor : step.attractors) {
            if (attractor.shape != AttractorShape3D::VectorField) {
                continue;
            }
            const ParticleField3D* field = attractor.field.get();
            if (field == nullptr || field->width == 0 || field->height == 0 || field->depth == 0 ||
                field->texels.size() != std::size_t{field->width} * field->height * field->depth ||
                field->texels.size() > kMaxParticleFieldTexels3D) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a vector field attractor needs a field of width times height "
                                  "times depth texels, at most " +
                                      std::to_string(kMaxParticleFieldTexels3D));
            }
        }
    }
    return {};
}

Result<void> check_particle_mesh(std::size_t amount, std::size_t corners,
                                 std::size_t mesh_vertices) {
    if (corners == 0 || mesh_vertices != amount * corners) {
        return make_error(ErrorCode::InvalidArgument,
                          "a particle mesh needs the system's amount times the draw pass's "
                          "vertices, and the pass some");
    }
    return {};
}

Result<void> check_particle_chain(std::span<const ParticleRun> chain,
                                  const std::function<bool(ParticlesId)>& known) {
    if (chain.empty() || chain.size() > kMaxParticleChain) {
        return make_error(ErrorCode::InvalidArgument,
                          "a particle chain has from 1 to " + std::to_string(kMaxParticleChain) +
                              " systems, not " + std::to_string(chain.size()));
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const ParticleRun& run = chain[i];
        if (!known(run.particles)) {
            return unknown_particles(run.particles);
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (chain[j].particles == run.particles) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a particle chain names system " +
                                      std::to_string(static_cast<std::uint32_t>(run.particles)) +
                                      " twice");
            }
        }
        if (run.history > run.steps.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle run's history is " + std::to_string(run.history) +
                                  " of its " + std::to_string(run.steps.size()) + " steps");
        }
        if (run.steps.size() - run.history !=
            chain.front().steps.size() -
                std::min(chain.front().history, chain.front().steps.size())) {
            return make_error(ErrorCode::InvalidArgument,
                              "a particle chain's systems run as many steps each");
        }
        if (run.process == nullptr) {
            return make_error(ErrorCode::InvalidArgument, "a particle run has a process material");
        }
        if (auto checked = check_particle_process(*run.process, run.steps, run.shapes); !checked) {
            return checked;
        }
    }
    return {};
}

Result<void> check_particle_draw(const ParticleDraw& look, std::uint32_t amount,
                                 std::uint32_t trail, std::size_t in_frame) {
    if (look.hframes < 1 || look.vframes < 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "particles' hframes and vframes are at least 1");
    }
    if (trail > 1 &&
        (look.trail_subdivisions < 1 || look.trail_subdivisions > kMaxParticleTrailSubdivisions ||
         look.trail_steps < 1 || look.trail_steps > kMaxParticleTrailSteps ||
         !(look.step_delta >= 0.0F) || !std::isfinite(look.step_delta))) {
        return make_error(
            ErrorCode::InvalidArgument,
            "particle trails have from 1 to " + std::to_string(kMaxParticleTrailSubdivisions) +
                " subdivisions, trail_steps from 1 to " + std::to_string(kMaxParticleTrailSteps) +
                " and a step_delta of 0 or more");
    }
    const std::uint32_t per = particle_vertex_count(trail, look);
    const std::size_t vertices = std::size_t{amount} * per;
    if (vertices > kMaxFrameVertices - std::min(in_frame, kMaxFrameVertices)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a frame draws up to " + std::to_string(kMaxFrameVertices) +
                              " vertices, " + std::to_string(per) + " a particle; these " +
                              std::to_string(amount) + " particles would make " +
                              std::to_string(in_frame + vertices));
    }
    return {};
}

Error not_a_render_target(TextureId texture) {
    return make_error(ErrorCode::InvalidArgument,
                      "texture " + std::to_string(static_cast<std::uint32_t>(texture)) +
                          " is not a render target; create it with render_target set");
}

Error target_in_use(TextureId texture) {
    return make_error(
        ErrorCode::InvalidArgument,
        "texture " + std::to_string(static_cast<std::uint32_t>(texture)) +
            " is this frame's target, so it can't be drawn with until the frame ends");
}

Error no_frame(std::string_view call) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(call) + " needs a frame; call begin_frame first");
}

Error frame_in_progress(std::string_view call) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(call) + " can't run during a frame; call end_frame first");
}

std::array<std::uint8_t, 4> to_rgba8(const Color& color) {
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return {byte(color.r), byte(color.g), byte(color.b), byte(color.a)};
}

} // namespace detail

std::uint16_t float_to_half(float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFFU) {
        // Infinity, or NaN with a mantissa bit kept.
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa != 0 ? 0x200U : 0U));
    }
    const int rebiased = static_cast<int>(exponent) - 127 + 15;
    if (rebiased >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    if (rebiased <= 0) {
        // A subnormal half, or zero: the mantissa with its leading 1, shifted into place.
        if (rebiased < -10) {
            return sign;
        }
        mantissa |= 0x800000U;
        const auto shift = static_cast<std::uint32_t>(14 - rebiased);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1U << shift) - 1U);
        const std::uint32_t halfway = 1U << (shift - 1U);
        if (rest > halfway || (rest == halfway && (half & 1U) != 0)) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(rebiased) << 10U) | (mantissa >> 13U);
    const std::uint32_t rest = mantissa & 0x1FFFU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0)) {
        ++half; // may carry into the exponent, up to infinity, as it should
    }
    return static_cast<std::uint16_t>(sign | half);
}

float half_to_float(std::uint16_t bits) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
    std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    std::uint32_t mantissa = bits & 0x3FFU;
    std::uint32_t out = sign;
    if (exponent == 0x1FU) {
        out |= 0x7F800000U | (mantissa << 13U);
    } else if (exponent != 0) {
        out |= ((exponent + 127U - 15U) << 23U) | (mantissa << 13U);
    } else if (mantissa != 0) {
        // A subnormal half is a normal float: shift its mantissa up to the leading 1.
        exponent = 127U - 15U + 1U;
        while ((mantissa & 0x400U) == 0) {
            mantissa <<= 1U;
            --exponent;
        }
        out |= (exponent << 23U) | ((mantissa & 0x3FFU) << 13U);
    }
    float value = 0.0F;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

namespace {

using detail::check_pixels;
using detail::check_texture_desc;
using detail::check_vertices;

class NullDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    [[nodiscard]] Backend backend() const override { return Backend::Null; }

    // The null device stands in for any GPU, so it takes every format.
    [[nodiscard]] bool supports(TextureFormat /*format*/) const override { return true; }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> texels) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, texels, true); !checked) {
            return checked.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.desc = desc, .cleared = {}};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> texels) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return detail::unknown_texture(texture);
        }
        return check_pixels(it->second.desc, texels, false);
    }

    void destroy_texture(TextureId texture) override { textures_.erase(texture); }

    Result<ShaderId> create_shader(const CompiledShader& /*shader*/) override {
        const auto id = static_cast<ShaderId>(next_shader_++);
        shaders_.insert(id);
        return id;
    }

    void destroy_shader(ShaderId shader) override { shaders_.erase(shader); }

    Result<ParticlesId> create_particles(std::uint32_t amount, std::uint32_t trail) override {
        if (auto checked = detail::check_particle_amount(amount, trail); !checked) {
            return checked.error();
        }
        const auto id = static_cast<ParticlesId>(next_particles_++);
        particles_[id] = {.amount = amount, .trail = trail};
        return id;
    }

    void destroy_particles(ParticlesId particles) override { particles_.erase(particles); }

    Result<void> process_particle_chain(std::span<const ParticleRun> chain) override {
        return detail::check_particle_chain(
            chain, [this](ParticlesId particles) { return particles_.contains(particles); });
    }

    Result<void> draw_particles(ParticlesId particles, TextureId texture, const ParticleDraw& look,
                                const Material& material) override {
        if (!in_frame_) {
            return detail::no_frame("draw_particles");
        }
        const auto it = particles_.find(particles);
        if (it == particles_.end()) {
            return detail::unknown_particles(particles);
        }
        const Shape shape = it->second;
        if (auto checked =
                detail::check_particle_draw(look, shape.amount, shape.trail, frame_vertices_);
            !checked) {
            return checked;
        }
        if (auto checked = check_draw(texture, {}, material); !checked) {
            return checked;
        }
        // As the GPU device, each draw of particles is a draw of its own.
        ++stats_.draws;
        last_texture_.reset();
        const std::size_t vertices =
            std::size_t{shape.amount} * particle_vertex_count(shape.trail, look);
        frame_vertices_ += vertices;
        stats_.triangles += vertices / 3;
        return {};
    }

    Result<std::vector<Particle>> read_particles(ParticlesId particles) override {
        if (in_frame_) {
            return detail::frame_in_progress("read_particles");
        }
        const auto it = particles_.find(particles);
        if (it == particles_.end()) {
            return detail::unknown_particles(particles);
        }
        return std::vector<Particle>(std::size_t{it->second.amount} * it->second.trail);
    }

    // 3D systems are checked as every device checks them, and run nothing.
    Result<Particles3DId> create_particles_3d(std::uint32_t amount) override {
        // The base device's store checks the amount as every device does.
        auto made = RenderDevice::create_particles_3d(amount);
        if (made) {
            particles_3d_[made.value()] = amount;
        }
        return made;
    }

    void destroy_particles_3d(Particles3DId particles) override {
        RenderDevice::destroy_particles_3d(particles);
        particles_3d_.erase(particles);
    }

    Result<void> process_particles_3d(Particles3DId particles, const ParticleProcess3D& process,
                                      std::span<const ParticleStep3D> steps) override {
        if (!particles_3d_.contains(particles)) {
            return make_error(ErrorCode::NotFound, "no such 3D particle system");
        }
        return detail::check_particle_process_3d(process, steps);
    }

    Result<void> fill_particle_mesh(Particles3DId particles, MeshId mesh,
                                    std::span<const Vertex3D> source,
                                    const ParticleMeshDraw3D& /*draw*/) override {
        const auto it = particles_3d_.find(particles);
        if (it == particles_3d_.end()) {
            return make_error(ErrorCode::NotFound, "no such 3D particle system");
        }
        if (in_frame_) {
            return detail::frame_in_progress("fill_particle_mesh");
        }
        const auto counts = meshes_.find(mesh);
        if (counts == meshes_.end()) {
            return detail::unknown_mesh(mesh);
        }
        return detail::check_particle_mesh(it->second, source.size(), counts->second.vertices);
    }

    Result<std::vector<Particle3D>> read_particles_3d(Particles3DId particles) override {
        if (in_frame_) {
            return detail::frame_in_progress("read_particles_3d");
        }
        const auto it = particles_3d_.find(particles);
        if (it == particles_3d_.end()) {
            return make_error(ErrorCode::NotFound, "no such 3D particle system");
        }
        return std::vector<Particle3D>(it->second);
    }

    Result<MeshId> create_mesh(std::span<const Vertex3D> vertices,
                               std::span<const std::uint32_t> indices,
                               std::span<const std::span<const std::uint32_t>> lods) override {
        if (auto checked = detail::check_mesh(vertices, indices, lods); !checked) {
            return checked.error();
        }
        const auto id = static_cast<MeshId>(next_mesh_++);
        Counts& counts = meshes_[id];
        counts.vertices = vertices.size();
        counts.triangles = {(indices.empty() ? vertices.size() : indices.size()) / 3};
        for (const std::span<const std::uint32_t> lod : lods) {
            counts.triangles.push_back(lod.size() / 3);
        }
        return id;
    }

    Result<void> update_mesh(MeshId mesh, std::span<const Vertex3D> vertices) override {
        if (in_frame_) {
            return detail::frame_in_progress("update_mesh");
        }
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return detail::unknown_mesh(mesh);
        }
        return detail::check_mesh_update(vertices, it->second.vertices);
    }

    void destroy_mesh(MeshId mesh) override { meshes_.erase(mesh); }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return detail::frame_in_progress("begin_frame");
        }
        const bool capture = frame.scene && frame.scene->capture;
        if (frame.target == TextureId::None && !capture) {
            return make_error(ErrorCode::InvalidArgument,
                              "the null device has no window; draw to a render-target texture");
        }
        if (frame.scene) {
            if (auto checked = detail::check_scene(*frame.scene); !checked) {
                return checked.error();
            }
            if (auto checked = detail::check_post(
                    frame.scene->post, [&](TextureId id) { return textures_.contains(id); },
                    [&](TextureId id) {
                        const TextureDesc& desc = textures_.at(id).desc;
                        return std::pair{desc.width, desc.height};
                    });
                !checked) {
                return checked.error();
            }
            if (auto checked = detail::check_sky(
                    *frame.scene, [&](TextureId id) { return textures_.contains(id); });
                !checked) {
                return checked.error();
            }
            if (auto checked = detail::check_reflections(*frame.scene,
                                                         [&](TextureId id) {
                                                             const auto found = textures_.find(id);
                                                             return found == textures_.end()
                                                                        ? 0U
                                                                        : found->second.desc.cubes;
                                                         });
                !checked) {
                return checked.error();
            }
        }
        if (capture) {
            // A capture checks its draws and draws nothing, like every frame here.
            if (frame.target != TextureId::None) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a capture frame draws into its atlas; its target is None");
            }
            lighting_ = {};
            has_scene_ = true;
            in_frame_ = true;
            target_ = TextureId::None;
            frame_vertices_ = 0;
            last_texture_.reset();
            return true;
        }
        const auto it = textures_.find(frame.target);
        if (it == textures_.end()) {
            return detail::unknown_texture(frame.target);
        }
        if (!it->second.desc.render_target) {
            return detail::not_a_render_target(frame.target);
        }
        if (auto checked = detail::check_lighting(
                frame.lighting, frame.target, [&](TextureId id) { return textures_.contains(id); },
                [&](TextureId id) {
                    const TextureDesc& desc = textures_.at(id).desc;
                    return std::pair{desc.width, desc.height};
                });
            !checked) {
            return checked.error();
        }
        it->second.cleared = detail::to_rgba8(frame.clear);
        lighting_ = frame.lighting;
        has_scene_ = frame.scene.has_value();
        in_frame_ = true;
        target_ = frame.target;
        frame_vertices_ = 0;
        last_texture_.reset();
        return true;
    }

    Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices,
                      const Material& material) override {
        if (!in_frame_) {
            return detail::no_frame("draw");
        }
        if (auto checked = check_draw(texture, vertices, material); !checked) {
            return checked;
        }
        if (vertices.empty()) {
            return {};
        }
        // Batched as the GPU device batches.
        if (last_texture_ != texture || last_material_ != material) {
            ++stats_.draws;
            last_texture_ = texture;
            last_material_ = material;
        }
        frame_vertices_ += vertices.size();
        stats_.triangles += vertices.size() / 3;
        return {};
    }

    Result<void> draw_mesh(MeshId mesh, const Mat4& transform, const Material3D& material,
                           std::uint32_t /*layers*/, ShadowCasting /*shadows*/,
                           const std::optional<Mat4>& previous, std::uint32_t lod) override {
        if (!in_frame_) {
            return detail::no_frame("draw_mesh");
        }
        const auto it = meshes_.find(mesh);
        if (auto checked = detail::check_mesh_draw(
                has_scene_, it != meshes_.end(), mesh, transform, material, target_,
                [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (auto checked = detail::check_previous_transform(previous); !checked) {
            return checked;
        }
        if (auto checked = detail::check_lod(lod, it->second.triangles.size() - 1); !checked) {
            return checked;
        }
        // Each mesh is a draw of its own, as on the GPU device.
        ++stats_.draws;
        stats_.triangles += it->second.triangles[lod];
        return {};
    }

    Result<void> end_frame() override {
        if (!in_frame_) {
            return detail::no_frame("end_frame");
        }
        in_frame_ = false;
        ++stats_.frames;
        return {};
    }

    Result<Image> read_texture(TextureId texture) override {
        if (in_frame_) {
            return detail::frame_in_progress("read_texture");
        }
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return detail::unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (!desc.render_target) {
            return detail::not_a_render_target(texture);
        }
        Image image{.width = desc.width, .height = desc.height, .pixels = {}};
        image.pixels.reserve(std::size_t{desc.width} * desc.height * 4);
        for (std::size_t i = 0; i < std::size_t{desc.width} * desc.height; ++i) {
            image.pixels.insert(image.pixels.end(), it->second.cleared.begin(),
                                it->second.cleared.end());
        }
        return image;
    }

    VsyncMode set_vsync(VsyncMode mode) override {
        vsync_ = mode;
        return vsync_;
    }
    [[nodiscard]] VsyncMode vsync() const override { return vsync_; }

    [[nodiscard]] Stats stats() const override {
        Stats stats = stats_;
        stats.textures = static_cast<std::uint32_t>(textures_.size());
        for (const auto& [id, texture] : textures_) {
            stats.texture_bytes += texture_bytes(texture.desc);
        }
        return stats;
    }

private:
    struct Texture {
        TextureDesc desc;
        std::array<std::uint8_t, 4> cleared{};
    };

    // What every draw checks: its texture, material, vertices and the frame's lights.
    Result<void> check_draw(TextureId texture, std::span<const Vertex2D> vertices,
                            const Material& material) {
        if (texture != TextureId::None && !textures_.contains(texture)) {
            return detail::unknown_texture(texture);
        }
        if (texture != TextureId::None && texture == target_) {
            return detail::target_in_use(texture);
        }
        if (auto checked = detail::check_material(
                material, target_, [&](TextureId id) { return textures_.contains(id); },
                [&](ShaderId id) { return shaders_.contains(id); });
            !checked) {
            return checked;
        }
        if (auto checked = check_vertices(vertices, frame_vertices_); !checked) {
            return checked;
        }
        return detail::check_lighting_textures(
            lighting_, [&](TextureId id) { return textures_.contains(id); });
    }

    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    std::unordered_set<ShaderId> shaders_;
    std::uint32_t next_shader_ = 1;
    // A particle system's amount and trail.
    struct Shape {
        std::uint32_t amount = 0;
        std::uint32_t trail = 1;
    };
    std::unordered_map<ParticlesId, Shape> particles_;
    std::uint32_t next_particles_ = 1;
    std::unordered_map<Particles3DId, std::uint32_t> particles_3d_; // amounts
    struct Counts {
        std::size_t vertices = 0;
        std::vector<std::size_t> triangles; // the mesh's, then each LOD's
    };
    std::unordered_map<MeshId, Counts> meshes_;
    std::uint32_t next_mesh_ = 1;
    bool in_frame_ = false;
    bool has_scene_ = false;
    TextureId target_ = TextureId::None;
    std::size_t frame_vertices_ = 0;
    std::optional<TextureId> last_texture_;
    Material last_material_;
    Lighting lighting_;
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

constexpr std::array<std::string_view, 4> kVsyncModes = {"disabled", "enabled", "adaptive",
                                                         "mailbox"};

} // namespace

std::string_view vsync_mode_name(VsyncMode mode) {
    return kVsyncModes.at(static_cast<std::size_t>(mode));
}

std::optional<VsyncMode> parse_vsync_mode(std::string_view name) {
    for (std::size_t i = 0; i < kVsyncModes.size(); ++i) {
        if (kVsyncModes.at(i) == name) {
            return static_cast<VsyncMode>(i);
        }
    }
    return std::nullopt;
}

std::string_view backend_name(Backend backend) {
    switch (backend) {
    case Backend::Null:
        return "null";
    case Backend::Vulkan:
        return "vulkan";
    case Backend::Metal:
        return "metal";
    case Backend::D3D12:
        return "d3d12";
    case Backend::Software:
        return "software";
    case Backend::OpenGl:
        return "opengl";
    case Backend::OpenGlEs:
        return "opengl_es";
    case Backend::WebGpu:
        return "webgpu";
    }
    return "null";
}

Mat4 pixel_projection(float width, float height) {
    Mat4 m{};
    m[0] = 2.0F / width;
    m[5] = -2.0F / height;
    m[10] = 1.0F;
    m[12] = -1.0F;
    m[13] = 1.0F;
    m[15] = 1.0F;
    return m;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 m{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a[(k * 4) + row] * b[(column * 4) + k];
            }
            m[(column * 4) + row] = sum;
        }
    }
    return m;
}

float distance_field_coverage(const std::array<float, 4>& texel, const DistanceField& field,
                              float scale) {
    // As field_coverage in shaders/material.hlsl, where the screen size of the texture over its
    // size in texels is the scale.
    const float px_size = std::max(field.pixel_range * scale, 1.0F);
    float d =
        std::max(std::min(texel[0], texel[1]), std::min(std::max(texel[0], texel[1]), texel[2]));
    if (field.outline > 0.0F) {
        const float cr =
            std::clamp(field.outline, 0.0F, (field.pixel_range / 2.0F) - 1.0F) / field.pixel_range;
        d = std::min(d, texel[3]);
        return std::clamp((d - 0.5F + cr) * px_size, 0.0F, 1.0F);
    }
    return std::clamp(((d - 0.5F) * px_size) + 0.5F, 0.0F, 1.0F);
}

std::unique_ptr<RenderDevice> create_null_device() {
    return std::make_unique<NullDevice>();
}

Result<std::unique_ptr<RenderDevice>> create_device(const DeviceOptions& options) {
    static constexpr std::array<std::string_view, 8> kDrivers = {
        "auto", "null", "vulkan", "metal", "d3d12", "opengl", "opengl_es", "webgpu"};
    if (std::ranges::find(kDrivers, options.driver) == kDrivers.end()) {
        std::string message =
            "no render driver \"" + options.driver +
            "\"; use auto, null, vulkan, metal, d3d12, opengl, opengl_es or webgpu";
        if (const std::string_view hint = closest_match(options.driver, kDrivers); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::InvalidArgument, std::move(message));
    }
    if (options.driver == "null") {
        return create_null_device();
    }
    if (uses_opengl(options.driver)) {
        return detail::create_gl_device(options);
    }
#ifdef __EMSCRIPTEN__
    // SDL's GPU API has no web backend, so browsers draw with WebGPU, where they have it.
    if (options.driver == "auto" || options.driver == "webgpu") {
        return detail::create_webgpu_device(options);
    }
#else
    if (options.driver == "webgpu") {
        return make_error(ErrorCode::InvalidArgument,
                          "the webgpu driver is the browser's, so only web builds have it");
    }
#endif
    return detail::create_gpu_device(options);
}

bool uses_opengl(std::string_view driver) {
#ifdef __EMSCRIPTEN__
    // WebGL 2 where the browser has no WebGPU.
    if (driver == "auto") {
        return !detail::webgpu_available();
    }
#endif
    return driver == "opengl" || driver == "opengl_es";
}

std::string choose_driver(const std::string& driver, bool fallback_to_opengl) {
    if (!fallback_to_opengl || uses_opengl(driver) || driver == "null") {
        return driver;
    }
#ifdef __EMSCRIPTEN__
    // auto is WebGPU here only where it works, and SDL's GPU backends never do.
    if (driver == "auto" || (driver == "webgpu" && detail::webgpu_available())) {
        return driver;
    }
#else
    // Where the video driver has no OpenGL either, the window stays as it was and the device
    // falls back to the null one.
    if ((driver != "webgpu" && detail::gpu_backend_works(driver)) || !detail::gl_library_loads()) {
        return driver;
    }
#endif
    log::warn("no GPU backend works here, so drawing with OpenGL instead",
              {{"driver", driver}, {"setting", "rendering.fallback_to_opengl"}});
    return "opengl";
}

Result<void> draw_empty_frame(RenderDevice& device, const Color& clear) {
    if (device.backend() == Backend::Null || device.backend() == Backend::Software) {
        return {};
    }
    auto began = device.begin_frame({.target = TextureId::None, .clear = clear, .view = {}});
    if (!began) {
        return began.error();
    }
    return device.end_frame(); // also ends a skipped frame, e.g. while minimized
}

std::unique_ptr<RenderDevice> create_device_or_null(const DeviceOptions& options) {
    auto device = create_device(options);
    if (device) {
        return std::move(device).value();
    }
    log::warn("drawing nothing: no render device works here",
              {{"driver", options.driver}, {"error", device.error().message}});
    return create_null_device();
}

} // namespace slash::render
