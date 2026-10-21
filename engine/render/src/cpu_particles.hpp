#pragma once

#include "checks.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/particles.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

// Particle systems kept and run on the CPU, with the GPU's twins (particles.hpp), for the devices
// with no compute shaders: the software device and OpenGL ES.
namespace slash::render::detail {

class CpuParticles {
public:
    [[nodiscard]] Result<ParticlesId> create(std::uint32_t amount, std::uint32_t trail) {
        if (auto checked = check_particle_amount(amount, trail); !checked) {
            return checked.error();
        }
        const auto id = static_cast<ParticlesId>(next_++);
        System& system = systems_[id];
        system.particles.resize(std::size_t{amount} * trail);
        system.trail = trail;
        return id;
    }

    void destroy(ParticlesId particles) { systems_.erase(particles); }

    [[nodiscard]] Result<void> process(std::span<const ParticleRun> chain) {
        if (auto checked = check_particle_chain(
                chain, [this](ParticlesId particles) { return systems_.contains(particles); });
            !checked) {
            return checked;
        }
        std::vector<ParticleChainLink> links;
        links.reserve(chain.size());
        for (const ParticleRun& run : chain) {
            System& system = systems_.at(run.particles);
            links.push_back({.particles = system.particles,
                             .process = run.process,
                             .steps = run.steps,
                             .shapes = run.shapes,
                             .trail = system.trail,
                             .history = run.history});
        }
        process_particle_chain(links);
        return {};
    }

    // Replaces out with a system's quads, after checking them against a frame that has drawn
    // in_frame vertices.
    [[nodiscard]] Result<void> vertices(ParticlesId particles, const ParticleDraw& look,
                                        std::size_t in_frame, std::vector<Vertex2D>& out) const {
        const auto it = systems_.find(particles);
        if (it == systems_.end()) {
            return unknown_particles(particles);
        }
        const System& system = it->second;
        const auto amount = static_cast<std::uint32_t>(system.particles.size() / system.trail);
        if (auto checked = check_particle_draw(look, amount, system.trail, in_frame); !checked) {
            return checked;
        }
        out.clear();
        particle_vertices(system.particles, look, out, system.trail);
        return {};
    }

    [[nodiscard]] Result<std::vector<Particle>> read(ParticlesId particles) const {
        const auto it = systems_.find(particles);
        if (it == systems_.end()) {
            return unknown_particles(particles);
        }
        return it->second.particles;
    }

private:
    struct System {
        std::vector<Particle> particles; // the trails' copies after them
        std::uint32_t trail = 1;
    };
    std::unordered_map<ParticlesId, System> systems_;
    std::uint32_t next_ = 1;
};

} // namespace slash::render::detail
