#pragma once

#include "particle_layout.hpp"

#include "slash/render/particles3d.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// How the GPU devices hand a 3D process material and steps to shaders/particles_process_3d.hlsl,
// and a draw pass to particles_mesh_3d.hlsl: as 16-byte words of 32-bit values, floats as their
// bits, as particle_layout.hpp does in 2D. The shaders read them at the same places; a change here
// goes there too.
namespace slash::render::detail {

// The process material's curves: the parameters', the velocity limit, alpha and emission curves,
// then the x, y and z of the scale, orbit velocity, directional velocity and scale over velocity
// curves.
inline constexpr std::uint32_t kCurves3D = kParticleParams + 3 + 12;
// Words 0 to 11 hold the settings, 12 to 19 the parameters' ranges, then a word for every four
// curves' point counts and one for the gradients' stop counts, then the curves, the gradients
// and three words a point: {point, 0}, {normal, 0} and its color.
inline constexpr std::uint32_t kCurveCountsAt3D = 20;
inline constexpr std::uint32_t kGradientCountsAt3D = kCurveCountsAt3D + ((kCurves3D + 3) / 4);
inline constexpr std::uint32_t kCurvesAt3D = kGradientCountsAt3D + 1;
inline constexpr std::uint32_t kGradientsAt3D = kCurvesAt3D + (kCurves3D * kCurveWords);
inline constexpr std::uint32_t kPointsAt3D = kGradientsAt3D + (2 * kGradientWords);
inline constexpr std::uint32_t kPointWords3D = 3;
// The places particles_process_3d.hlsl reads them at.
static_assert(kCurves3D == 30 && kGradientCountsAt3D == 28 && kCurvesAt3D == 29);
static_assert(kGradientsAt3D == 509 && kPointsAt3D == 589);
// The flags in word 4's w.
inline constexpr std::uint32_t kProcessAlignY = 1U;
inline constexpr std::uint32_t kProcessRotateY = 2U;
inline constexpr std::uint32_t kProcessDisableZ = 4U;
inline constexpr std::uint32_t kProcessDampingAsFriction = 8U;
inline constexpr std::uint32_t kProcessTurbulence = 16U;
inline constexpr std::uint32_t kProcessDirectionalGlobal = 32U;
inline constexpr std::uint32_t kProcessAttractors = 64U;
// A step's words. Its last word holds the cycle, the seed, the flags (particle_layout.hpp's, with
// the attractor count from bit kStepAttractorShift) and where its attractors' words are. Each
// attractor is kAttractorWords3D words: its transform (3), {extents, strength} and {shape, where
// its field is, attenuation, directionality}. A field is {width, height, depth, 0}, then a word a
// texel.
inline constexpr std::uint32_t kStepWords3D = 7;
inline constexpr std::uint32_t kStepAttractorShift = 8;
inline constexpr std::uint32_t kAttractorWords3D = 5;

// Appends a 3D process material's words.
void pack_particle_process_3d(const ParticleProcess3D& process, std::vector<Word>& out);
// Appends 3D steps' words, then their attractors' and the fields those sample.
void pack_particle_steps_3d(std::span<const ParticleStep3D> steps, std::vector<Word>& out);
// Appends a draw pass's vertices, 15 words of 32 bits each as Vertex3D lays them out, four to a
// word.
void pack_particle_vertices_3d(std::span<const Vertex3D> vertices, std::vector<Word>& out);

// The words of a Vertex3D.
inline constexpr std::uint32_t kVertexWords3D = 15;
static_assert(sizeof(Vertex3D) == std::size_t{kVertexWords3D} * 4);

// particles_process_3d.hlsl's uniforms: offsets are in words.
struct ProcessUniforms3D {
    std::uint32_t amount = 0;
    std::uint32_t material = 0;
    std::uint32_t steps = 0;
    std::uint32_t step_count = 0;
    std::uint32_t reset = 0;
    float phase_step = 0.0F; // particle_phase_step(amount)
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

// particles_sort_3d.hlsl's uniforms: stage 0 makes each slot's key, from its particle's depth
// along sort_direction; the others are a bitonic sort's step, comparing slots j apart in runs of
// k. size is amount rounded up to a power of two.
struct SortUniforms3D {
    std::uint32_t amount = 0;
    std::uint32_t size = 0;
    std::uint32_t j = 0;
    std::uint32_t k = 0;
    std::uint32_t stage = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
    std::uint32_t pad2 = 0;
    std::array<float, 4> sort_direction{};
};

// The bitonic sort's steps for size slots, after the stage that makes the keys: {j, k} pairs.
[[nodiscard]] std::vector<std::array<std::uint32_t, 2>> bitonic_steps(std::uint32_t size);
// amount rounded up to a power of two.
[[nodiscard]] std::uint32_t sort_size(std::uint32_t amount);

// particles_mesh_3d.hlsl's uniforms, from a ParticleMeshDraw3D. The shader runs a thread a vertex
// it writes, in rows of counts[3] threads (mesh_groups), since one row of groups stops at 65535.
struct MeshUniforms3D {
    // amount, corners, where the vertices are in words, threads a row
    std::array<std::uint32_t, 4> counts{};
    // lifetime_split, align, billboard, flags (kMesh*)
    std::array<std::uint32_t, 4> look{};
    std::array<float, 4> frames{}; // h_frames, v_frames, remainder, 0
    std::array<std::array<float, 4>, 3> transform{};
    std::array<std::array<float, 4>, 3> camera{};
    std::array<float, 4> sort_direction{};
    std::array<float, 4> align_up{};
    std::array<float, 4> uv{}; // scale, offset
};
inline constexpr std::uint32_t kMeshKeepScale = 1U;
inline constexpr std::uint32_t kMeshAnimLoop = 2U;
inline constexpr std::uint32_t kMeshSorted = 4U;           // slots come from the sort's keys
inline constexpr std::uint32_t kMeshLifetime = 8U;         // ParticleOrder3D::Lifetime
inline constexpr std::uint32_t kMeshReverseLifetime = 16U; // ParticleOrder3D::ReverseLifetime
// The threads of a compute group in the 3D particle shaders.
inline constexpr std::uint32_t kParticleThreads3D = 64;

// The groups to dispatch for threads threads, kParticleThreads3D each, in rows of at most 65535.
[[nodiscard]] std::array<std::uint32_t, 2> mesh_groups(std::uint32_t threads);
[[nodiscard]] MeshUniforms3D mesh_uniforms_3d(const ParticleMeshDraw3D& draw, std::uint32_t amount,
                                              std::uint32_t corners, std::uint32_t source_at);

} // namespace slash::render::detail
