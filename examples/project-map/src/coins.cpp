// Coins: picked up by the wallets that touch them, and put back after.
#include "runner.hpp"

#include "slash/world/game_module.hpp"
#include "slash/world/scene.hpp"

#include <string>

namespace {

// A coin, picked up on contact.
struct Coin {
    int value = 1;
};
SLASH_COMPONENT(Coin, value)

// Coins an entity holds.
struct Wallet {
    int coins = 0;
};
SLASH_COMPONENT(Wallet, coins)

constexpr const char* kCoinPrefab = "prefabs/coin.prefab.json";

// Pays every wallet for each coin touched last tick, and puts a new coin in its place.
void collect(slash::world::World& world, double /*tick_seconds*/) {
    for (const slash::world::Event& event : world.events()) {
        if (event.kind != "area_entered") {
            continue;
        }
        const entt::entity coin = world.find(event.id);
        if (coin == entt::null || !world.ecs().all_of<Coin>(coin)) {
            continue;
        }
        const int value = world.ecs().get<Coin>(coin).value;
        world.ecs().view<Wallet>().each([value](Wallet& wallet) { wallet.coins += value; });
        world.emit("coin_collected", event.id);
        world.destroy(coin);
        if (const auto next = world.create(event.id + "_next")) {
            (void)slash::world::instantiate_prefab(world, next.value(), kCoinPrefab);
        }
    }
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<Coin>().set_doc("A coin, picked up on contact.");
    game.add_component<Wallet>().set_doc("Coins an entity holds.");
    game.add_system("collect", collect, {.doc = "Pays wallets for coins.", .priority = 960});
    game.add_system("move_runner", game::move_runner, {.doc = "Runs and jumps."});
}
