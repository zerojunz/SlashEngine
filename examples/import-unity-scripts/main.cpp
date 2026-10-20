// Importing a Unity game's scripts as C++ skeletons, then porting one.
//
// unity/ holds a Unity 2D project whose Frog.cs drifts the frog to its lily pad as it falls,
// picks up the coin it passes through and reports its landing. project.import turns the script
// into src/imported/frog.cpp: its serialized fields become the Frog component (the scene's
// values on the frog entity, its pad a reference to the pad entity), its UnityEvent a helper that
// emits a world event, and its methods stubs that keep the C# in comments, called by the frog
// system when Unity would call them: Start on the entities made since the last tick, Update once
// a tick, OnTriggerEnter2D as a body enters an Area2D and OnCollisionEnter2D as the frog's
// RigidBody2D, which the import set to report contacts, touches another body. The agent fills in
// the stubs, builds with gameplay.build and runs the scene.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

constexpr const char* kSkeleton = "src/imported/frog.cpp";

// Prints the call as an agent would make it, then the result or the error.
command::Reply call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    command::Reply reply = session.call(name, value);
    if (reply.ok) {
        std::puts(json::write(reply.value, json::Style::Pretty).c_str());
    } else {
        std::printf("error: %s\n", reply.message.c_str());
    }
    return reply;
}

// Edits the skeleton as an agent would, failing loudly if it isn't as expected.
void rewrite(std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(kSkeleton).value();
    const std::size_t at = text.find(from);
    if (at == std::string::npos) {
        std::fprintf(stderr, "%s has no %.*s\n", kSkeleton, static_cast<int>(from.size()),
                     from.data());
        std::exit(1);
    }
    text.replace(at, from.size(), to);
    (void)slash::platform::write_file(kSkeleton, text);
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::importer::register_import_commands(commands); // project.import
    slash::exporter::register_export_commands(commands); // gameplay.build
    slash::gameplay::Host code(components, slash::gameplay::library_path("."));
    command::SessionOptions options;
    options.gameplay = &code;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    // The import writes the skeleton, the file registering it and a game module, and points
    // project.json's gameplay sources at src/.
    call(session, "project.import", R"({"source": "unity"})");
    std::printf("--- %s\n%s", kSkeleton, slash::platform::read_file(kSkeleton).value().c_str());

    // Porting: each stub's body, from the C# in its comments. Unity measures in units, which the
    // import made pixels_per_unit pixels (its report says 100), so a speed in units a second
    // scales by that.
    rewrite("#include <utility>\n", "#include <algorithm>\n#include <utility>\n");
    rewrite("using slash::world::World;\n",
            "using slash::world::World;\n\ninline constexpr float kPixelsPerUnit = 100.0F;\n");
    rewrite("void start(World& /*world*/, entt::entity /*entity*/, Frog& /*self*/) {\n",
            "void start(World& world, entt::entity entity, Frog& self) {\n"
            "    world.emit(\"ready\", world.id_of(entity), slash::json::Value(self.coins));\n");
    rewrite("void update(World& /*world*/, entt::entity /*entity*/, Frog& /*self*/, double "
            "/*delta*/) {\n",
            "void update(World& world, entt::entity entity, Frog& self, double delta) {\n"
            "    auto& position = world.ecs().get<slash::world::Transform2D>(entity).position;\n"
            "    const auto* pad = world.ecs().try_get<slash::world::Transform2D>(self.pad);\n"
            "    if (pad != nullptr) {\n"
            "        const float step = self.speed * kPixelsPerUnit * static_cast<float>(delta);\n"
            "        position.x = std::clamp(pad->position.x, position.x - step, position.x + "
            "step);\n"
            "    }\n");
    rewrite("void on_trigger_enter2d(World& /*world*/, entt::entity /*entity*/, Frog& /*self*/,\n"
            "                        entt::entity /*other*/) {\n",
            "void on_trigger_enter2d(World& world, entt::entity /*entity*/, Frog& self,\n"
            "                        entt::entity other) {\n"
            "    self.coins += 1;\n"
            "    world.destroy(other);\n");
    rewrite("void on_collision_enter2d(World& /*world*/, entt::entity /*entity*/, Frog& /*self*/,\n"
            "                          entt::entity /*collision*/) {\n",
            "void on_collision_enter2d(World& world, entt::entity entity, Frog& self,\n"
            "                          entt::entity /*collision*/) {\n"
            "    self.landings += 1;\n"
            "    emit_on_landed(world, entity, self.coins);\n");

    call(session, "gameplay.build", "{}");
    call(session, "scene.open", R"({"path": "unity/Assets/Scenes/Pond.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["ready", "entity.destroyed", "on_landed"]})");
    // The frog falls from 300 pixels above the pad, drifting toward it at 150 pixels a second.
    // Start runs at the first tick, on the entities the scene made; the frog falls through the
    // coin's Area2D, which its OnTriggerEnter2D takes, then lands on the pad, which its
    // OnCollisionEnter2D reports.
    call(session, "step", R"({"ticks": 60})");
    call(session, "world.query", R"({"with": ["Frog"], "include": ["Frog", "Transform2D"]})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    return 0;
}
