// The game's own code: wallets that earn their income and the bank's interest every tick.
// main.cpp makes mistakes in this file and checks it with build.check.

#include "bank.hpp"

#include "slash/world/game_module.hpp"

namespace {

struct Wallet {
    int coins = 0;
    int income = 1; // coins a tick
};
SLASH_COMPONENT(Wallet, coins, income)

void earn(slash::world::World& world, double /*tick_seconds*/) {
    world.ecs().view<Wallet>().each(
        [](Wallet& wallet) { wallet.coins += wallet.income + bank::interest(wallet.coins); });
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<Wallet>()
        .set_doc("Coins an entity holds.")
        .describe("income", {.doc = "Coins earned each tick."});
    game.add_system("earn", earn, {.doc = "Pays each wallet its income and interest."});
}
