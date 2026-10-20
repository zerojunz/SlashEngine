#include "enemy.hpp"

#include "slash/world/game_module.hpp"

SLASH_GAME_MODULE(game) {
    game.add_component<arena::Enemy>().set_doc("An enemy that chases the player.");
    game.add_system("chase", arena::chase, {.doc = "Enemies fly at the player."});
}
