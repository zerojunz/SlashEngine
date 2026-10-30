// Particles colliding with baked shapes and the ground in 3D, as Godot's
// GPUParticlesCollisionSDF3D and GPUParticlesCollisionHeightField3D: in main.scene.json snow falls
// onto a yard with a mound in it, which a height field draws from above, and sparks fall onto a
// round statue, whose signed distance field particles.bake_sdf bakes from its mesh.
//
// This program bakes the statue's field, then checks where the snow and the sparks came to rest
// by counting particles, as an agent would, with no screenshot.

#include "slash/canvas/gpu_particles.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/scene3d/gpu_particles.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
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

// The yard's surface under a point: the ground's top at y 0, or the mound's where it rises above.
float surface(float x, float z) {
    const float dx = x + 1.5F;
    const float across = 1.0F - (dx * dx) - (z * z);
    return across > 0.0F ? std::max(0.0F, -0.4F + std::sqrt(across)) : 0.0F;
}

// Where the snow is against the yard: none sinks into it, and the flakes that landed lie on it,
// some on the mound.
void count_snow(command::Session& session) {
    int below = 0;
    int lying = 0;
    int on_mound = 0;
    const std::vector<render::Particle3D> snow = live(session, "snow");
    for (const render::Particle3D& flake : snow) {
        const float ground = surface(flake.origin.x, flake.origin.z);
        below += flake.origin.y < ground - 0.15F ? 1 : 0;
        if (flake.origin.y < ground + 0.1F) {
            ++lying;
            on_mound += ground > 0.1F ? 1 : 0;
        }
    }
    std::printf("snow: %zu alive, %d below the yard, %d lying on it, %d of them on the mound\n",
                snow.size(), below, lying, on_mound);
}

// Where the sparks are against the statue, a ball of 75 centimeters' radius at (2, 1, 0).
void count_sparks(command::Session& session) {
    int inside = 0;
    int on_top = 0;
    int below = 0;
    const std::vector<render::Particle3D> sparks = live(session, "sparks");
    for (const render::Particle3D& spark : sparks) {
        const float dx = spark.origin.x - 2.0F;
        const float dy = spark.origin.y - 1.0F;
        const float dz = spark.origin.z;
        inside += std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) < 0.7F ? 1 : 0;
        on_top += dy > 0.6F && dy < 0.9F && std::abs(spark.velocity.y) < 0.5F ? 1 : 0;
        below += dy < -0.75F ? 1 : 0;
    }
    std::printf("sparks: %zu alive, %d inside the statue, %d resting on its top, %d below it\n",
                sparks.size(), inside, on_top, below);
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

    // The statue's distances, baked from the meshes on its bake_mask's layers in its box, as
    // Godot's Bake SDF: 16 cells along each side, each closer than its thickness behind a face
    // less than 0. The bake sets the collider's texture to the .sdf.png file it writes.
    call(session, "particles.bake_sdf", R"({"id": "statue_sdf"})");

    // Three seconds: the height field is drawn from above once, and the snow settles on what it
    // drew. The sparks land on the statue and come to rest on its top.
    call(session, "step", R"({"ticks": 180})");
    count_snow(session);
    count_sparks(session);

    // Without a texture an SDF collider holds nothing, as Godot's: the sparks fall through.
    call(session, "component.set",
         R"({"id": "statue_sdf", "components": {"GPUParticlesCollisionSDF3D": {"texture": ""}}})");
    call(session, "step", R"({"ticks": 60})");
    count_sparks(session);

    // A height field with follow_camera_enabled steps after the current camera a meter at a time
    // along its x and z axes, as Godot's.
    call(session, "batch", R"({"commands": [
        {"command": "component.set", "params": {"id": "snowfield", "components": {
            "GPUParticlesCollisionHeightField3D": {"follow_camera_enabled": true}}}},
        {"command": "component.set", "params": {"id": "camera", "components": {
            "Transform3D": {"position": [5.5, 3.0, 9.0]}}}}]})");
    call(session, "step", R"({"ticks": 1})");
    const world::World& world = session.world();
    const auto& field = world.ecs().get<const world::Transform3D>(world.find("snowfield"));
    std::printf("snowfield at %.1f %.1f %.1f\n", static_cast<double>(field.position.x),
                static_cast<double>(field.position.y), static_cast<double>(field.position.z));
    return 0;
}
