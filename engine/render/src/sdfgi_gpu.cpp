#include "sdfgi_gpu.hpp"

#include "slash/core/bits.hpp"

#include <algorithm>

namespace slash::render::detail {

namespace {

std::uint32_t word_of(float value) {
    return slash::bit_cast<std::uint32_t>(value);
}
std::uint32_t word_of(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

// A matrix's row (column-major, as Mat4).
Float4 row_of(const Mat4& m, std::size_t row) {
    return {m.at(row), m.at(4 + row), m.at(8 + row), m.at(12 + row)};
}

SdfgiUniforms dispatch(const SdfgiFramePlan& plan, SdfgiPass pass, std::uint32_t cascade,
                       std::uint32_t threads, std::uint32_t number = 0) {
    SdfgiUniforms u;
    u.step = {static_cast<std::uint32_t>(pass), cascade, number, threads};
    u.grid = {plan.size, static_cast<std::uint32_t>(plan.cascades.size()), 1, 0};
    u.probes = {plan.history_size, plan.history_index, 0, 0};
    u.params = {plan.y_mult, 0.0F, 0.0F, 0.0F};
    return u;
}

SdfgiStep clear(const SdfgiFramePlan& plan, SdfgiBuffer buffer, std::uint32_t start,
                std::uint32_t count) {
    SdfgiStep step;
    step.uniforms = dispatch(plan, SdfgiPass::Clear, 0, count, static_cast<std::uint32_t>(buffer));
    step.uniforms.range = {start, count, 0, 0};
    return step;
}

SdfgiStep run(const SdfgiFramePlan& plan, SdfgiPass pass, std::uint32_t cascade,
              std::uint32_t threads, std::uint32_t number = 0) {
    SdfgiStep step;
    step.cascade = cascade;
    step.uniforms = dispatch(plan, pass, cascade, threads, number);
    return step;
}

} // namespace

SdfgiLayout sdfgi_layout(std::uint32_t cascades, std::uint32_t size, std::uint32_t history_size) {
    SdfgiLayout layout;
    layout.cascades = cascades;
    layout.size = size;
    layout.history_size = history_size;
    const std::uint32_t n = layout.cell_count();
    const std::uint32_t sh = kSdfgiProbeCount * kSdfgiShSize;
    layout.cells = n * 3;
    layout.fields = n * 3;
    layout.probes = (sh * 4) + (kSdfgiProbeImageTexels * 2);
    layout.history = sh * history_size * 2;
    // The voxelized sums (8 words a cell), the distance transform's lines (5 a cell) and the
    // probes' scroll (their averages and history).
    layout.scratch = std::max({n * 8, n * 5, (sh * 4) + layout.history});
    return layout;
}

std::vector<std::uint32_t> sdfgi_items(const SdfgiState& state, const SdfgiLayout& layout,
                                       std::span<const SdfgiLight> lights) {
    const auto light_words = static_cast<std::uint32_t>(lights.size()) * kSdfgiItemLight;
    const std::uint32_t offsets = kSdfgiItemLights + light_words;
    std::vector<std::uint32_t> items(offsets + (layout.cascades * kSdfgiProbeCount), 0U);
    for (std::uint32_t c = 0; c < layout.cascades && c < state.cascades.size(); ++c) {
        const SdfgiCascade& cascade = state.cascades[c];
        const Float3 offset = sdfgi_offset(cascade, layout.size);
        const std::uint32_t at = c * kSdfgiItemCascade;
        items[at] = word_of(offset[0]);
        items[at + 1] = word_of(offset[1]);
        items[at + 2] = word_of(offset[2]);
        items[at + 3] = word_of(cascade.cell_size);
        items[at + 4] = word_of(cascade.position[0]);
        items[at + 5] = word_of(cascade.position[1]);
        items[at + 6] = word_of(cascade.position[2]);
        items[at + 7] = word_of(1.0F / cascade.cell_size);
        items[at + 8] = c * layout.cells;
        items[at + 9] = c * layout.fields;
        items[at + 10] = c * layout.probes;
        items[at + 11] = offsets + (c * kSdfgiProbeCount);
        items[at + 12] = c * layout.history;
        const std::vector<float> rays = sdfgi_probe_offsets(cascade, layout.size);
        for (std::size_t p = 0; p < rays.size() && p < kSdfgiProbeCount; ++p) {
            items[items[at + 11] + p] = word_of(rays[p]);
        }
    }
    for (std::size_t l = 0; l < lights.size(); ++l) {
        const SdfgiLight& light = lights[l];
        const auto at = kSdfgiItemLights + (static_cast<std::uint32_t>(l) * kSdfgiItemLight);
        // sdfgi_common.hlsl's light types: directional 0, omni 1, spot 2.
        std::uint32_t type = 2U;
        if (light.type == LightType::Directional) {
            type = 0U;
        } else if (light.type == LightType::Omni) {
            type = 1U;
        }
        items[at] = type;
        for (std::size_t a = 0; a < 3; ++a) {
            items[at + 1 + a] = word_of(light.color.at(a));
            items[at + 4 + a] = word_of(light.direction.at(a));
            items[at + 7 + a] = word_of(light.position.at(a));
        }
        items[at + 10] = word_of(light.attenuation);
        items[at + 11] = word_of(light.radius);
        items[at + 12] = word_of(light.cos_spot_angle);
        items[at + 13] = word_of(light.inv_spot_attenuation);
    }
    return items;
}

std::vector<SdfgiStep> sdfgi_steps(const SdfgiState& state, const SdfgiFramePlan& plan,
                                   const SdfgiLayout& layout, std::uint32_t light_count,
                                   const SdfgiEnvironment& environment) {
    std::vector<SdfgiStep> steps;
    const std::uint32_t n = layout.cell_count();
    const std::uint32_t size = layout.size;
    const std::uint32_t sh = kSdfgiProbeCount * kSdfgiShSize;
    const Sdfgi3D& settings = state.settings;
    const auto cascades = static_cast<std::uint32_t>(plan.cascades.size());
    for (std::uint32_t c = 0; c < cascades; ++c) {
        const SdfgiCascadePlan& p = plan.cascades[c];
        // The probes: cleared, or moved by whole probes (scroll_probes).
        if (p.probes == SdfgiCascadePlan::Probes::Clear) {
            steps.push_back(clear(plan, SdfgiBuffer::Probes, c * layout.probes, layout.probes));
            steps.push_back(clear(plan, SdfgiBuffer::History, c * layout.history, layout.history));
        } else if (p.probes == SdfgiCascadePlan::Probes::Scroll) {
            steps.push_back(run(plan, SdfgiPass::ScrollCopy, c, (sh * 4) + layout.history));
            SdfgiStep move = run(plan, SdfgiPass::ScrollMove, c, kSdfgiProbeCount);
            move.uniforms.shift = {p.probe_shift[0], p.probe_shift[1], p.probe_shift[2], 0};
            steps.push_back(move);
        }
        if (!p.voxelize) {
            continue;
        }
        // voxelize: the draws' sums, the cells resolved from them (those still held moved), and
        // the distance field (make_field) with MODE_STORE.
        const std::array<std::int32_t, 3> moved =
            p.scrolled.value_or(std::array<std::int32_t, 3>{});
        const std::int32_t scrolling = p.scrolled.has_value() ? 1 : 0;
        steps.push_back(clear(plan, SdfgiBuffer::Scratch, 0, n * 8));
        SdfgiStep voxelize;
        voxelize.kind = SdfgiStep::Kind::Voxelize;
        voxelize.cascade = c;
        const SdfgiCascade& cascade = state.cascades[c];
        const Float3 offset = sdfgi_offset(cascade, size);
        voxelize.voxelize.place = {offset[0], offset[1], offset[2], 1.0F / cascade.cell_size};
        voxelize.voxelize.work = {size, 0, 0, word_of(plan.y_mult)};
        voxelize.voxelize.shift = {moved[0], moved[1], moved[2], scrolling};
        steps.push_back(voxelize);
        SdfgiStep resolve = run(plan, SdfgiPass::Resolve, c, n);
        resolve.uniforms.shift = {moved[0], moved[1], moved[2], scrolling};
        steps.push_back(resolve);
        SdfgiStep copy;
        copy.kind = SdfgiStep::Kind::CopyCells;
        copy.cascade = c;
        steps.push_back(copy);
        steps.push_back(run(plan, SdfgiPass::EdtInit, c, n));
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            steps.push_back(run(plan, SdfgiPass::Edt, c, size * size, axis));
        }
        steps.push_back(run(plan, SdfgiPass::Store, c, n));
        if (settings.use_occlusion) {
            // make_sdfgi_occlusion: each world parity's regions, then their store.
            steps.push_back(clear(plan, SdfgiBuffer::Cells, (c * layout.cells) + (n * 2), n));
            steps.push_back(clear(plan, SdfgiBuffer::Scratch, 0, n));
            const auto occ = static_cast<std::int32_t>(size / kSdfgiProbeDivisor);
            for (std::uint32_t parity = 0; parity < 8; ++parity) {
                std::uint32_t regions = 1;
                for (std::uint32_t a = 0; a < 3; ++a) {
                    const std::int32_t probe_global = cascade.position.at(a) / occ;
                    const std::uint32_t off =
                        ((parity >> a) & 1U) ^ static_cast<std::uint32_t>(probe_global & 1);
                    regions *= ((kSdfgiProbeDivisor / 2) + 1) - off;
                }
                steps.push_back(run(plan, SdfgiPass::Occlusion, c, regions, parity));
                steps.push_back(run(plan, SdfgiPass::OcclusionStore, c, n, parity));
            }
        }
    }
    // light_cells: each cascade's solid cells, their aniso shares merged, then the empty cells
    // next to them.
    const std::uint32_t flags = (settings.use_occlusion ? kSdfgiFlagOcclusion : 0U) |
                                (settings.read_sky_light ? kSdfgiFlagSkyLight : 0U);
    for (std::uint32_t c = 0; c < cascades; ++c) {
        const SdfgiCascadePlan& p = plan.cascades[c];
        for (const SdfgiPass pass :
             {SdfgiPass::Light, SdfgiPass::LightMerge, SdfgiPass::LightGather}) {
            SdfgiStep step = run(plan, pass, c, n);
            step.uniforms.grid[2] = p.light_increment;
            step.uniforms.grid[3] = p.light_offset;
            step.uniforms.range[3] = light_count;
            step.uniforms.probes[3] = flags;
            step.uniforms.params[1] = settings.bounce_feedback;
            steps.push_back(step);
        }
    }
    // integrate: every cascade's probes trace their rays, then store their maps.
    for (const SdfgiPass pass : {SdfgiPass::Integrate, SdfgiPass::ProbeStore}) {
        for (std::uint32_t c = 0; c < cascades; ++c) {
            SdfgiStep step = run(plan, pass, c, kSdfgiProbeCount);
            step.uniforms.probes[2] = std::max(1U, settings.ray_count);
            step.uniforms.probes[3] = flags;
            step.uniforms.params[2] = settings.probe_bias;
            step.uniforms.ambient = {environment.ambient[0], environment.ambient[1],
                                     environment.ambient[2], environment.sky ? 1.0F : 0.0F};
            for (std::size_t r = 0; r < 3; ++r) {
                const Float3& row = environment.to_sky.at(r);
                step.uniforms.sky_rows.at(r) = {row[0], row[1], row[2], 0.0F};
            }
            step.uniforms.sky_rows[0][3] = environment.sky_energy;
            step.uniforms.sky_rows[1][3] = environment.sky_share;
            steps.push_back(step);
        }
    }
    return steps;
}

std::array<std::uint32_t, 2> sdfgi_groups(std::uint32_t threads) {
    const std::uint32_t groups = (threads + kSdfgiThreads - 1) / kSdfgiThreads;
    return {std::min(groups, kSdfgiGroupsAcross),
            (groups + kSdfgiGroupsAcross - 1) / kSdfgiGroupsAcross};
}

SdfgiVoxelizeUniforms sdfgi_voxelize_uniforms(const SdfgiStep& step, const Mat4& model,
                                              const Mat4& normal_model,
                                              const MeshSurfaceUniforms& surface, const Float4& uv1,
                                              std::uint32_t first_index,
                                              std::uint32_t index_count) {
    SdfgiVoxelizeUniforms u = step.voxelize;
    for (std::size_t r = 0; r < 3; ++r) {
        u.model_rows.at(r) = row_of(model, r);
        u.normal_rows.at(r) = row_of(normal_model, r);
    }
    u.albedo = surface.albedo;
    u.emission = surface.emission;
    u.options = surface.options;
    u.uv1 = uv1;
    u.work[1] = first_index;
    u.work[2] = index_count / 3;
    return u;
}

SdfgiGatherUniforms sdfgi_gather_uniforms(const SdfgiState& state, const Float3& camera,
                                          const RayShadowFrame& frame) {
    SdfgiGatherUniforms u;
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t col = 0; col < 4; ++col) {
            u.to_world.at(r).at(col) = static_cast<float>(frame.to_world.at((col * 4) + r));
        }
    }
    u.view_rows = frame.view_rows;
    u.camera = {camera[0], camera[1], camera[2], state.settings.energy};
    u.grid = {state.settings.cascade_size, static_cast<std::uint32_t>(state.cascades.size()),
              state.settings.use_occlusion ? kSdfgiFlagOcclusion : 0U, 0};
    u.params = {sdfgi_y_mult(state.settings), state.settings.normal_bias,
                static_cast<float>(frame.width), static_cast<float>(frame.height)};
    return u;
}

} // namespace slash::render::detail
