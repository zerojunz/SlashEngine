#include "ray_records.hpp"

#include <algorithm>
#include <cstring>

namespace slash::render::detail {

namespace {

// Godot's rendering/lightmapping/bake_performance/region_size, squared: the texels a dispatch
// lights at most; max_rays_per_pass and max_rays_per_probe_pass, the rays.
constexpr std::uint32_t kBakeRegionTexels = 512U * 512U;
constexpr std::uint32_t kBakeRaysPerPass = 32;
constexpr std::uint32_t kBakeRaysPerProbePass = 64;

std::uint32_t probe_rays_of(const LightmapTrace& trace) {
    return std::clamp(trace.probe_rays, 16U, 8192U);
}

template <typename Uniforms>
void place_view(Uniforms& uniforms, const RayShadowFrame& frame) {
    for (std::size_t i = 0; i < 16; ++i) {
        uniforms.to_world.at(i) = static_cast<float>(frame.to_world.at(i));
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            uniforms.view_rows.at(row).at(column) = frame.view_rows.at(row).at(column);
        }
    }
}

} // namespace

std::uint8_t ray_mask(const RayDraw& draw, bool lit) {
    std::uint8_t mask = draw.shadows != ShadowCasting::Off ? kRayCastMask : 0;
    if (lit && draw.shadows != ShadowCasting::ShadowsOnly) {
        mask |= kRaySeeMask;
    }
    return mask;
}

RayShadowsUniforms ray_shadows_uniforms(const RayWork& work, const RayShadowFrame& frame,
                                        bool every_layer) {
    RayShadowsUniforms uniforms{};
    place_view(uniforms, frame);
    const auto light_count =
        static_cast<std::uint32_t>(std::min<std::size_t>(work.lights.size(), kMaxRayShadowLights));
    uniforms.size = {frame.width, frame.height, frame.samples, light_count};
    bool every_mask = true;
    for (std::uint32_t k = 0; k < light_count; ++k) {
        const RayShadowLight& light = work.lights[k];
        const Float3& place = light.directional ? light.toward : light.position;
        uniforms.lights.at(k) = {.place = {place[0], place[1], place[2], light.size},
                                 .params = {light.opacity, light.directional ? 1.0F : 0.0F,
                                            static_cast<float>(light.slot), 0.0F},
                                 .mask = {light.caster_mask, 0, 0, 0}};
        every_mask = every_mask && light.caster_mask == 0xFFFFFFFFU;
    }
    // Every draw casts for every light: rays need not ask which draw they meet.
    uniforms.options = {every_mask && every_layer ? 1U : 0U, 0, 0, 0};
    if (work.occlusion) {
        uniforms.options[1] = 1;
        uniforms.occlusion = {work.occlusion->radius, work.occlusion->power,
                              static_cast<float>(work.occlusion->samples),
                              work.occlusion->intensity};
    }
    return uniforms;
}

std::uint32_t RayTextureTable::index(const RayTexture& texture) {
    if (texture.texture == nullptr) {
        return kRayNoTexture;
    }
    const std::size_t sampler = (texture.nearest ? 1U : 0U) | (texture.repeat ? 2U : 0U);
    const std::pair<SDL_GPUTexture*, std::size_t> key{texture.texture, sampler};
    const auto found = std::find(used_.begin(), used_.end(), key);
    if (found != used_.end()) {
        return static_cast<std::uint32_t>(found - used_.begin());
    }
    if (used_.size() >= kRayTextures) {
        return kRayNoTexture;
    }
    used_.push_back(key);
    return static_cast<std::uint32_t>(used_.size() - 1);
}

RayDrawRecord ray_draw_record(const RayDraw& draw, std::uint64_t vertices, std::uint64_t indices,
                              RayTextureTable& textures) {
    const MeshSurfaceUniforms& surface = draw.surface;
    RayDrawRecord record;
    record.albedo = surface.albedo;
    record.emission = surface.emission;
    record.surface = surface.surface;
    record.metallic_channel = surface.metallic_channel;
    record.roughness_channel = surface.roughness_channel;
    record.options = {surface.options[0], surface.options[1], draw.layers, 0};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            record.normal_rows.at(row).at(column) = draw.normal_model.at((column * 4) + row);
        }
    }
    record.uv1 = draw.uv1;
    record.buffers = {
        static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>(vertices >> 32U),
        static_cast<std::uint32_t>(indices), static_cast<std::uint32_t>(indices >> 32U)};
    record.textures = {textures.index(draw.textures[0]), textures.index(draw.textures[1]),
                       textures.index(draw.textures[2]), textures.index(draw.textures[3])};
    std::uint32_t cull = 2;
    if (draw.cull == CullMode::Back) {
        cull = 0;
    } else if (draw.cull == CullMode::Front) {
        cull = 1;
    }
    record.more = {textures.index(draw.textures[4]), cull, 0, 0};
    return record;
}

std::vector<RayLightRecord> ray_light_records(const RayLighting& lighting) {
    std::vector<RayLightRecord> records;
    records.reserve(lighting.lights.size());
    for (const PathLight& light : lighting.lights) {
        records.push_back({.position = light.uniforms.position,
                           .direction = light.uniforms.direction,
                           .color = light.uniforms.color,
                           .params = light.uniforms.params,
                           // As direct_light in src/path_trace.cpp reads the layers it lights.
                           .masks = {static_cast<std::uint32_t>(light.uniforms.params[3]),
                                     light.caster_mask, light.shadow ? 1U : 0U, 0},
                           .shadow = {light.shadow_opacity, 0.0F, 0.0F, 0.0F}});
    }
    return records;
}

RayLightFrame ray_light_frame(const RayWork& work, const RayShadowFrame& frame,
                              std::size_t light_count) {
    const RayLighting& lighting = *work.lighting;
    RayLightFrame uniforms{};
    place_view(uniforms, frame);
    uniforms.size = {frame.width, frame.height, work.reflection ? work.reflection->samples : 0,
                     static_cast<std::uint32_t>(light_count)};
    uniforms.gi = {work.gi ? work.gi->samples : 0, 0, 0, 0};
    if (work.paths) {
        uniforms.paths = {work.paths->first, work.paths->samples, work.paths->bounces, 0};
    }
    uniforms.ambient = {lighting.ambient[0], lighting.ambient[1], lighting.ambient[2],
                        lighting.sky ? 1.0F : 0.0F};
    for (std::size_t row = 0; row < 3; ++row) {
        const Float3& r = lighting.to_sky.at(row);
        uniforms.sky_rows.at(row) = {r[0], r[1], r[2], 0.0F};
    }
    uniforms.sky_rows[0][3] = lighting.sky_energy;
    uniforms.sky_rows[1][3] = lighting.sky_share;
    return uniforms;
}

bool lightmap_trace_fits(const LightmapTrace& trace) {
    const std::size_t texels = std::size_t{trace.width} * trace.height * trace.slices;
    return !trace.indices.empty() && trace.indices.size() == trace.triangles.size() * 3 &&
           texels != 0 && trace.albedo.size() == texels && trace.emission.size() == texels &&
           trace.position.size() == texels && trace.normal.size() == texels &&
           trace.environment.size() ==
               std::size_t{trace.environment_width} * trace.environment_height &&
           !trace.environment.empty();
}

void lightmap_inputs(const LightmapTrace& trace, LightmapInputs& out) {
    out.probes.resize(trace.probe_points.size());
    for (std::size_t i = 0; i < out.probes.size(); ++i) {
        out.probes[i] = {trace.probe_points[i][0], trace.probe_points[i][1],
                         trace.probe_points[i][2], 0.0F};
    }
    out.triangles.resize(trace.triangles.size());
    for (std::size_t i = 0; i < out.triangles.size(); ++i) {
        const BakeTriangle& t = trace.triangles[i];
        // CullMode's order is the shader's: back, front, none.
        out.triangles[i] = {t.slice, static_cast<std::uint32_t>(t.cull)};
    }
    out.lights.resize(trace.lights.size());
    for (std::size_t i = 0; i < out.lights.size(); ++i) {
        const BakeLight& l = trace.lights[i];
        // LightType's order is the shader's: directional, omni, spot.
        const auto kind = static_cast<float>(l.type);
        out.lights[i] = {
            .position = {l.position[0], l.position[1], l.position[2], kind},
            .direction = {l.direction[0], l.direction[1], l.direction[2], l.energy},
            .color = {l.color[0], l.color[1], l.color[2], l.indirect_energy},
            .shape = {l.size, l.range, l.attenuation, l.cos_spot_angle},
            .more = {l.inv_spot_attenuation, l.shadow_blur, l.static_bake ? 1.0F : 0.0F, 0.0F}};
    }
    static_assert(sizeof(BakeVertex) == 32);
    out.inputs = {{
        {trace.vertices.data(), trace.vertices.size() * sizeof(BakeVertex)},
        {trace.indices.data(), trace.indices.size() * sizeof(std::uint32_t)},
        {out.triangles.data(), out.triangles.size() * sizeof(out.triangles[0])},
        {out.lights.data(), out.lights.size() * sizeof(LightmapLightRecord)},
        {trace.albedo.data(), trace.albedo.size_bytes()},
        {trace.emission.data(), trace.emission.size_bytes()},
        {trace.position.data(), trace.position.size_bytes()},
        {trace.normal.data(), trace.normal.size_bytes()},
        {trace.environment.data(), trace.environment.size_bytes()},
        {out.probes.data(), out.probes.size() * sizeof(out.probes[0])},
    }};
}

std::size_t lightmap_texel_bytes(const LightmapTrace& trace) {
    return std::size_t{trace.width} * trace.height * trace.slices * sizeof(std::array<float, 4>);
}

std::size_t lightmap_probe_bytes(const LightmapTrace& trace) {
    return trace.probe_points.size() * 9 * sizeof(std::array<float, 4>);
}

Result<void>
run_lightmap_passes(const LightmapTrace& trace, std::size_t light_count,
                    const std::function<Result<void>(std::size_t pass, std::uint32_t count,
                                                     const LightmapParams& params)>& dispatch) {
    LightmapParams params;
    params.size = {trace.width, trace.height, trace.slices, trace.environment_width};
    params.counts = {trace.environment_height, static_cast<std::uint32_t>(light_count), trace.rays,
                     trace.bounces};
    params.flags = {trace.use_texture_for_bounces ? 1U : 0U, 0, 0, 0};
    params.values = {trace.bias, trace.supersampling, trace.exposure, trace.world_size};
    params.more = {trace.bounce_indirect_energy, 0.0F, 0.0F, 0.0F};
    for (std::size_t c = 0; c < 3; ++c) {
        params.space.at(c) = {trace.probe_space.at(c)[0], trace.probe_space.at(c)[1],
                              trace.probe_space.at(c)[2], 0.0F};
    }
    // Each slice's texels a region at a time: the direct light, then each pass of bounces.
    const std::uint32_t slice_texels = trace.width * trace.height;
    const auto each_region = [&](std::size_t pass) -> Result<void> {
        for (std::uint32_t slice = 0; slice < trace.slices; ++slice) {
            for (std::uint32_t first = 0; first < slice_texels; first += kBakeRegionTexels) {
                const std::uint32_t end = std::min(first + kBakeRegionTexels, slice_texels);
                params.step[0] = slice;
                params.flags[1] = first;
                params.flags[2] = end;
                if (auto ran = dispatch(pass, end - first, params); !ran) {
                    return ran;
                }
            }
        }
        return {};
    };
    if (auto ran = each_region(0); !ran) {
        return ran;
    }
    if (trace.bounces > 0) {
        for (std::uint32_t from = 0; from < trace.rays; from += kBakeRaysPerPass) {
            params.step[1] = from;
            params.step[2] = std::min(from + kBakeRaysPerPass, trace.rays);
            if (auto ran = each_region(1); !ran) {
                return ran;
            }
        }
    }
    const auto probe_count = static_cast<std::uint32_t>(trace.probe_points.size());
    const std::uint32_t probe_rays = probe_rays_of(trace);
    if (probe_count > 0) {
        params.step[3] = probe_count;
        for (std::uint32_t from = 0; from < probe_rays; from += kBakeRaysPerProbePass) {
            params.step[1] = from;
            params.step[2] = std::min(from + kBakeRaysPerProbePass, probe_rays);
            if (auto ran = dispatch(2, probe_count, params); !ran) {
                return ran;
            }
        }
    }
    return {};
}

LightmapTraced lightmap_traced(const LightmapTrace& trace, const void* light_source,
                               const void* accum, const void* probe_sums) {
    const std::size_t texels = std::size_t{trace.width} * trace.height * trace.slices;
    const std::size_t texel_bytes = lightmap_texel_bytes(trace);
    LightmapTraced traced;
    traced.light_source.resize(texels);
    traced.accum.resize(texels);
    std::memcpy(traced.light_source.data(), light_source, texel_bytes);
    std::memcpy(traced.accum.data(), accum, texel_bytes);
    const std::size_t probe_count = trace.probe_points.size();
    traced.probe_sh.resize(probe_count);
    const float scale = 4.0F / static_cast<float>(probe_rays_of(trace));
    const auto* sums = static_cast<const std::array<float, 4>*>(probe_sums);
    for (std::size_t probe = 0; probe < probe_count; ++probe) {
        for (std::size_t j = 0; j < 9; ++j) {
            const std::array<float, 4>& sum = sums[(probe * 9) + j];
            traced.probe_sh[probe].at(j) = {sum[0] * scale, sum[1] * scale, sum[2] * scale};
        }
    }
    return traced;
}

} // namespace slash::render::detail
