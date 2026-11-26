#include "slash/render/bake.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace slash::render {

Result<MeshMapTraced> trace_mesh_maps(RenderDevice& device, const MeshMapTrace& trace) {
    const std::size_t texels = std::size_t{trace.width} * trace.height;
    if (texels == 0 || trace.position.size() != texels || trace.normal.size() != texels ||
        trace.indices.empty() || trace.indices.size() % 3 != 0 ||
        trace.kind == BakeKind::Lightmap) {
        return Error{ErrorCode::InvalidArgument, "a mesh map trace's texels don't fit it"};
    }
    // The lightmap light shader's mesh map passes, over a lightmap trace of one slice whose
    // triangles cull nothing. It reads no albedo, emission or environment, so those are the
    // texels' points and one texel of black.
    static const std::array<std::array<float, 4>, 1> kNoEnvironment{};
    LightmapTrace lightmap;
    lightmap.kind = trace.kind;
    lightmap.vertices = trace.vertices;
    lightmap.indices = trace.indices;
    lightmap.triangles.assign(trace.indices.size() / 3, {.slice = 0, .cull = CullMode::Disabled});
    lightmap.width = trace.width;
    lightmap.height = trace.height;
    lightmap.slices = 1;
    lightmap.albedo = trace.position;
    lightmap.emission = trace.position;
    lightmap.position = trace.position;
    lightmap.normal = trace.normal;
    lightmap.environment_width = 1;
    lightmap.environment_height = 1;
    lightmap.environment = kNoEnvironment;
    lightmap.rays = trace.rays;
    lightmap.bounces = 0;
    lightmap.bias = trace.bias;
    lightmap.world_size = trace.sky_distance;
    lightmap.region_texels = trace.region_texels;
    lightmap.occlusion_distance = trace.occlusion_distance;
    lightmap.thickness_distance = trace.thickness_distance;
    lightmap.cage_distance = trace.cage_distance;
    auto traced = device.trace_lightmap(lightmap);
    if (!traced) {
        return traced.error();
    }
    MeshMapTraced out;
    out.texels = std::move(traced.value().light_source);
    return out;
}

} // namespace slash::render
