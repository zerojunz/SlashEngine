// Importing a Godot game's scripts as C++ skeletons, then porting one.
//
// godot/ holds a Godot 4 project whose player.gd walks the player to its goal and counts the coins
// a button gives it. project.import turns the script into src/imported/player.cpp: its variables
// become the Player component (the scene's values on the player entity, its goal a reference to
// the goal entity), its signal a helper that emits a world event, and its functions stubs that
// keep the GDScript in comments, called by the player system when Godot would call them: _ready
// on the entities made since the last tick, _physics_process once a tick, and _on_coin_pressed
// when the coin button emits pressed, as the scene connected it. The agent fills in the stubs,
// builds with gameplay.build and runs the scene.

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

constexpr const char* kSkeleton = "src/imported/player.cpp";

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
    call(session, "project.import", R"({"source": "godot"})");
    std::printf("--- %s\n%s", kSkeleton, slash::platform::read_file(kSkeleton).value().c_str());

    // Porting: each stub's body, from the GDScript in its comments.
    rewrite("#include \"slash/world/game_module.hpp\"\n",
            "#include \"slash/world/components.hpp\"\n#include \"slash/world/game_module.hpp\"\n"
            "\n#include <algorithm>\n");
    rewrite("void ready(World& /*world*/, entt::entity /*entity*/, Player& /*self*/) {\n",
            "void ready(World& world, entt::entity entity, Player& self) {\n"
            "    world.emit(\"ready\", world.id_of(entity), slash::json::Value(self.coins));\n");
    rewrite("void physics_process(World& /*world*/, entt::entity /*entity*/, Player& /*self*/,\n"
            "                     double /*delta*/) {\n",
            "void physics_process(World& world, entt::entity entity, Player& self, double delta) "
            "{\n"
            "    auto& position = world.ecs().get<slash::world::Transform2D>(entity).position;\n"
            "    const auto* goal = world.ecs().try_get<slash::world::Transform2D>(self.goal);\n"
            "    if (goal != nullptr) {\n"
            "        const float step = std::min(self.speed * static_cast<float>(delta), "
            "kStepLimit);\n"
            "        position.x = std::clamp(goal->position.x, position.x - step, position.x + "
            "step);\n"
            "    }\n");
    rewrite("void on_coin_pressed(World& /*world*/, entt::entity /*entity*/, Player& /*self*/,\n"
            "                     const slash::world::Event& /*event*/) {\n",
            "void on_coin_pressed(World& world, entt::entity entity, Player& self,\n"
            "                     const slash::world::Event& /*event*/) {\n"
            "    self.coins += 1;\n"
            "    emit_coin_collected(world, entity, self.coins);\n");

    call(session, "gameplay.build", "{}");
    call(session, "scene.open", R"({"path": "godot/main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["ready", "coin_collected"]})");
    // The player walks toward the goal, 20 pixels away, at its speed: 120 pixels a second is 2 a
    // tick at 60 ticks a second. Its _ready ran at the first tick, on the entities the scene made.
    call(session, "step", R"({"ticks": 6})");
    // The coin button emits pressed at the next tick, and the player system sees it at the one
    // after, as the scene's connection says.
    call(session, "ui.press", R"({"id": "coin"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "world.query", R"({"with": ["Player"], "include": ["Player", "Transform2D"]})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    return 0;
}
