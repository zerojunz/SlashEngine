// Soft bodies on Jolt, as Godot's SoftBody3D: main.scene.json drops a tablecloth onto a table and
// a ball full of air onto the floor, and hangs a flag from a pole by two pinned points.
//
// The program steps the world as an agent would, reads each soft body's points with
// physics.soft_body, then moves the pole with component.set: the flag's pinned points follow it.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

void step(command::Session& session, int ticks) {
    std::printf("step %d ticks\n", ticks);
    session.call("step", json::parse(R"({"ticks": )" + std::to_string(ticks) + "}").value());
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

    // Before the first step each soft body's points are where its mesh puts them: one point for
    // each vertex of the mesh, placed by the entity's transform.
    call(session, "physics.soft_body", R"({"id": "tablecloth"})");
    call(session, "physics.soft_body", R"({"id": "flag", "at": [6, 34, 0]})");

    // Four seconds: the tablecloth drapes over the table, the ball lands and stays round, and the
    // flag hangs still from its pins, as cloth does with no wind.
    step(session, 240);
    call(session, "physics.soft_body", R"({"id": "tablecloth", "at": [60]})");
    call(session, "physics.soft_body", R"({"id": "ball"})");
    call(session, "physics.soft_body", R"({"id": "flag", "at": [6, 34, 0]})");

    // The pinned points follow the pole's transform; the rest of the flag swings after them.
    call(session, "component.set",
         R"({"id": "pole", "components": {"Transform3D": {"position": [2.4, 2.0, 1.0]}}})");
    step(session, 60);
    call(session, "physics.soft_body", R"({"id": "flag", "at": [6, 34, 0]})");
    return 0;
}
