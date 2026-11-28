// Save data for game code (slash/platform/save_data.hpp): named slots of JSON in the game's save
// folder, written whole and read back with the game version that wrote them.
//
// A running game sets the folder when it starts (runtime::Game::start: the player's folder for
// the game, or the project's .slash/saves/ for a headless run), so game code only names slots.
// This program starts the game headless from this folder, saves, lists, loads and deletes slots
// as a game's menus would, then reads the slots as an agent does, with the save.* commands.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/save_data.hpp"
#include "slash/runtime/game.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace json = slash::json;
namespace platform = slash::platform;

json::Value parse(std::string_view text) {
    return json::parse(text).value();
}

void print_slots() {
    std::printf("slots:");
    for (const platform::SaveSlot& slot : platform::save_slots()) {
        std::printf(" %s (version %s)", slot.name.c_str(), slot.game_version.c_str());
    }
    std::printf("\n");
}

} // namespace

int main() {
    auto game = slash::runtime::Game::start({.data_dir = ".", .headless = true, .max_ticks = 1});
    if (!game) {
        std::fprintf(stderr, "%s\n", game.error().message.c_str());
        return 1;
    }
    std::printf("folder: %s\n", platform::save_store().folder == platform::project_save_folder(".")
                                    ? ".slash/saves/ (headless)"
                                    : "the player's");

    // The player saves on the first slot, and the game keeps an autosave.
    const json::Value progress =
        parse(R"({"level": 3, "health": 72, "inventory": ["key", "lantern"]})");
    if (auto saved = platform::save_game("slot-1", progress); !saved) {
        std::fprintf(stderr, "%s\n", saved.error().message.c_str());
        return 1;
    }
    (void)platform::save_game("autosave", parse(R"({"level": 3, "health": 80})"));
    print_slots();

    // Continue: load the slot back.
    auto loaded = platform::load_game("slot-1");
    std::printf("slot-1: %s, written by %s\n",
                json::write(loaded.value().data, json::Style::Compact).c_str(),
                loaded.value().game_version.c_str());
    // Names are lowercase letters, digits, '_' and '-'; an empty slot is NotFound.
    std::printf("Slot 2: %s\n", platform::load_game("Slot 2").error().message.c_str());
    std::printf("slot-2: %s\n", platform::load_game("slot-2").error().message.c_str());

    (void)platform::delete_save("autosave");
    print_slots();

    // An agent reads and seeds the same slots through the commands.
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    slash::command::Registry commands;
    slash::command::register_builtin_commands(commands);
    slash::command::Session session(components, commands, {.project_root = "."});
    const auto read = session.call("save.read", parse(R"({"slot": "slot-1"})"));
    std::printf("save.read slot-1: %s\n",
                json::write(*read.value.find("data"), json::Style::Compact).c_str());
    (void)session.call("save.write",
                       parse(R"({"slot": "boss-fight", "data": {"level": 9, "health": 100}})"));
    print_slots();
    return 0;
}
