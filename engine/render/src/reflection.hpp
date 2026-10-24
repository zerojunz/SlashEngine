#pragma once

#include "mesh_shading.hpp"
#include "sky_shading.hpp"

#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to draw reflection probes (Reflections3D and ProbeCapture, in
// mesh.hpp): the probes as the mesh pass reads them, the filter that makes a captured cube's
// levels, and the CPU twin of Godot's reflection_process (scene_forward_lights_inc.glsl), which
// mesh.hlsl's Forward+ variant runs and the software device draws with.
namespace slash::render::detail {

// A frame's probes, in its order, and the scene uniform that goes with them: how many, the level
// the roughest surfaces read, and the exposure.
[[nodiscard]] std::vector<MeshProbeUniforms> probe_uniforms(const Reflections3D& reflections);
[[nodiscard]] Float4 reflections_uniform(const Reflections3D& reflections);

// The atlas cubes a texture made with TextureDesc::cubes has, and their size: how many levels each
// has, as the sky's radiance (radiance_levels).
[[nodiscard]] std::uint32_t probe_levels(std::uint32_t size);

// Checks a frame's probes and capture against the atlases a device has made: cube counts and
// sizes by texture. atlas_cubes returns 0 for a texture that is not an atlas.
template <typename AtlasCubes>
[[nodiscard]] Result<void> check_reflections(const Scene3D& scene, const AtlasCubes& atlas_cubes);

// The light a pixel takes from the probes, as Godot's reflection_process accumulates it: the
// reflection and the ambient light, each premultiplied by how much of the sky's it replaces.
struct ProbeLight {
    Float4 reflection{};
    Float4 ambient{};
};
// The CPU twin of mesh.hlsl's probe loop for a pixel at world, with the shading normal, view
// (toward the camera) and roughness, drawn on layers; cubes holds each atlas cube's image.
[[nodiscard]] ProbeLight reflect_probes(std::span<const MeshProbeUniforms> probes,
                                        std::span<const CubeImage> cubes, float exposure,
                                        const Float3& world, const Float3& normal,
                                        const Float3& view, float roughness, std::uint32_t layers);

template <typename AtlasCubes>
Result<void> check_reflections(const Scene3D& scene, const AtlasCubes& atlas_cubes) {
    const Reflections3D& reflections = scene.reflections;
    if (!reflections.probes.empty()) {
        const std::uint32_t cubes = atlas_cubes(reflections.atlas);
        if (cubes == 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "the reflections' atlas is not a texture made with cubes");
        }
        if (reflections.probes.size() > kMaxReflectionCubes) {
            return make_error(ErrorCode::InvalidArgument,
                              "a frame has at most 42 reflection probes");
        }
        for (const ReflectionProbe3D& probe : reflections.probes) {
            if (probe.cube >= cubes) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a reflection probe's cube is past its atlas's");
            }
        }
    }
    if (scene.capture) {
        const std::uint32_t cubes = atlas_cubes(scene.capture->atlas);
        if (cubes == 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "a capture's atlas is not a texture made with cubes");
        }
        if (scene.capture->cube >= cubes || scene.capture->face >= kCubeFaces) {
            return make_error(ErrorCode::InvalidArgument,
                              "a capture's cube or face is past its atlas's");
        }
    }
    return {};
}

} // namespace slash::render::detail
