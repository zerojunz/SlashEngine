// Particle attractors in 3D, as Godot's GPUParticlesAttractor3D nodes: in main.scene.json a sheet
// of dust hangs in the air over a 6 by 6 meter square. A sphere (the well) pulls the dust near it
// in, a box (the gust) pushes the dust inside it along its -z as a wind does, and a vector field
// (the swirl) turns the dust inside it round the y axis by a 3D texture of directions.
//
// This program makes the swirl's texture, then checks what the attractors did with
// particles.eval as an agent would, with no screenshot.

#include "slash/canvas/gpu_particles.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/gpu_particles.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// The swirl's field, 4 texels a side: each texel's color c pushes along c * 2 - 1, here round
// the y axis, counterclockwise seen from above. Its 4 slices front to back are cut 2 across and
// 2 down from an 8x8 image, as Godot's Texture3D importer cuts slices/horizontal and
// slices/vertical; games ship such a PNG made in a tool, this one is made here so the example
// has no binary file.
void write_swirl() {
    constexpr int kSide = 4;
    constexpr std::size_t kBytes = std::size_t{8} * 8 * 4;
    render::Image image{.width = 8, .height = 8, .pixels = std::vector<std::uint8_t>(kBytes)};
    for (int z = 0; z < kSide; ++z) {
        for (int y = 0; y < kSide; ++y) {
            for (int x = 0; x < kSide; ++x) {
                // The texel's center in the box, from -1 to 1 across it.
                const float px = ((static_cast<float>(x) + 0.5F) / 2.0F) - 1.0F;
                const float pz = ((static_cast<float>(z) + 0.5F) / 2.0F) - 1.0F;
                const float length = std::sqrt((px * px) + (pz * pz));
                const std::array<float, 3> push{-pz / length, 0.0F, px / length};
                // Slice z sits at column z % 2 and row z / 2 of the image.
                const int column = ((z % 2) * kSide) + x;
                const int row = ((z / 2) * kSide) + y;
                const auto at = static_cast<std::size_t>((row * 8) + column) * 4;
                for (std::size_t c = 0; c < 3; ++c) {
                    image.pixels[at + c] =
                        static_cast<std::uint8_t>(std::lround((push.at(c) + 1.0F) * 0.5F * 255.0F));
                }
                image.pixels[at + 3] = 255;
            }
        }
    }
    check(slash::platform::write_file("swirl.png", render::encode_png(image).value()));
    std::puts("wrote swirl.png: 8x8, 4 slices of 4x4");
}

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

// Prints what scene validation finds in the open world's 3D entities.
void validate(const command::Session& session) {
    const auto problems = slash::scene3d::check_meshes(session.world());
    std::printf("problems: %zu\n", problems.size());
    for (const auto& problem : problems) {
        std::printf("  %s%s: %s\n", std::string(session.world().id_of(problem.entity)).c_str(),
                    problem.path.c_str(), problem.message.c_str());
    }
}

// How many of the dust's particles are in the swirl's box, and how many of those it has set
// turning round its middle, which particles.eval's bounds can't tell: for a particle at r from
// the middle moving at v, r x v points down (-y) when it goes round as the swirl pushes.
void count_swirl(command::Session& session) {
    const world::World& world = session.world();
    const entt::entity dust = world.find("dust");
    const auto& emitter = world.ecs().get<const world::GPUParticles3D>(dust);
    const auto process =
        session.assets().load<slash::canvas::ProcessMaterial>(emitter.process_material);
    if (!process) {
        std::exit(1);
    }
    int inside = 0;
    int turning = 0;
    for (const render::Particle3D& particle :
         slash::scene3d::gpu_particles_3d(world, dust, process.value()->space)) {
        const float rx = particle.origin.x - 2.0F;
        const float rz = particle.origin.z;
        if (rx < -1.0F || rx > 1.0F || rz < -1.0F || rz > 1.0F || particle.origin.y < -0.5F ||
            particle.origin.y > 0.5F) {
            continue;
        }
        ++inside;
        turning += (rz * particle.velocity.x) - (rx * particle.velocity.z) < 0.0F ? 1 : 0;
    }
    std::printf("in the swirl: %d particles, %d of them turning round it\n", inside, turning);
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_swirl();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Where would the dust be a second in with nothing pulling it? A time runs the emitter's
    // settings from the start apart from the world, so no attractor acts: the dust stays in its
    // sheet, 6 meters square and 0.2 high.
    call(session, "particles.eval", R"({"id": "dust", "time": 1})");
    // A second of ticks in the world: each tick the gpu_particles_3d system pairs the dust with
    // the attractors whose bounds touch its visibility box, and the well has drawn dust up toward
    // its center a meter above, the gust has blown dust toward -z and the swirl has turned it.
    call(session, "step", R"({"ticks": 60})");
    call(session, "particles.eval", R"({"id": "dust"})");
    call(session, "particles.eval", R"({"id": "dust", "list": true, "max_results": 3})");
    count_swirl(session);

    // An attractor acts only on emitters whose layers share a bit with its cull_mask, as Godot's:
    // with the well's on layer 2 only, the dust it gathered drifts on, slowed by damping, and no
    // more is drawn up.
    call(session, "component.set",
         R"({"id": "well", "components": {"GPUParticlesAttractorSphere3D": {"cull_mask": 2}}})");
    call(session, "step", R"({"ticks": 60})");
    call(session, "particles.eval", R"({"id": "dust"})");

    // A vector field's texture must cut into its slices: 16 across an 8 pixel wide image leaves
    // none, which scene validation reports (a field that can't be read or cut pushes as white).
    call(session, "component.set",
         R"({"id": "swirl", "components": {"GPUParticlesAttractorVectorField3D":
             {"slices_horizontal": 16}}})");
    validate(session);
    return 0;
}
