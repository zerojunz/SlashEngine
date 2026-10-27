// Particle collision in 3D, as Godot's GPUParticlesCollision3D nodes: in main.scene.json a fountain
// of sparks falls onto a floor (a collision box) and bounces, as its process material's rigid
// collision_mode says, and rain falls round an umbrella (a collision sphere) that hides each drop
// it touches, as hide_on_contact says, so the ground under it stays dry.
//
// This program checks what the colliders did with particles.eval and by counting particles, as
// an agent would, with no screenshot.

#include "slash/canvas/gpu_particles.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/scene3d/gpu_particles.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

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

// An emitter's live particles, as the devices that draw it hold them.
std::vector<render::Particle3D> live(command::Session& session, std::string_view id) {
    const world::World& world = session.world();
    const entt::entity entity = world.find(id);
    const auto& emitter = world.ecs().get<const world::GPUParticles3D>(entity);
    const auto process =
        session.assets().load<slash::canvas::ProcessMaterial>(emitter.process_material);
    if (!process) {
        std::exit(1);
    }
    return slash::scene3d::gpu_particles_3d(world, entity, process.value()->space);
}

// Where the sparks are against the floor, whose top is at y 0: none sinks through it, and
// those that have stopped bouncing rest a collision_base_size above it.
void count_sparks(command::Session& session) {
    int below = 0;
    int resting = 0;
    const std::vector<render::Particle3D> sparks = live(session, "sparks");
    for (const render::Particle3D& spark : sparks) {
        below += spark.origin.y < -0.05F ? 1 : 0;
        resting += spark.origin.y < 0.1F && std::abs(spark.velocity.y) < 0.5F ? 1 : 0;
    }
    std::printf("sparks: %zu alive, %d below the floor, %d resting on it\n", sparks.size(), below,
                resting);
}

// How many drops are under the umbrella (a meter round its middle, below it) and beside it.
void count_rain(command::Session& session) {
    int under = 0;
    int beside = 0;
    for (const render::Particle3D& drop : live(session, "rain")) {
        const float dx = drop.origin.x - 3.0F;
        const float dz = drop.origin.z;
        if (drop.origin.y > 1.0F) {
            continue;
        }
        if ((dx * dx) + (dz * dz) < 0.5F) {
            ++under;
        } else {
            ++beside;
        }
    }
    std::printf("rain below the umbrella's middle: %d under it, %d beside it\n", under, beside);
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

    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Two seconds of ticks: each tick the gpu_particles_3d system pairs each emitter with the
    // colliders whose bounds touch its visibility box. The sparks have risen, fallen onto the
    // floor and bounced, losing 60% of their speed into it each time and 10% along it.
    call(session, "step", R"({"ticks": 120})");
    call(session, "particles.eval", R"({"id": "sparks"})");
    count_sparks(session);
    count_rain(session);

    // A collider acts only on emitters whose layers share a bit with its cull_mask, as Godot's:
    // with the umbrella on layer 2 only, the rain falls through it, and only the floor hides it.
    call(
        session, "component.set",
        R"({"id": "umbrella", "components": {"GPUParticlesCollisionSphere3D": {"cull_mask": 2}}})");
    call(session, "step", R"({"ticks": 60})");
    count_rain(session);
    return 0;
}
