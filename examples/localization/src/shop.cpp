// The shop's sign and till, in the player's language.
#include "slash/l10n/l10n.hpp"
#include "slash/world/game_module.hpp"

#include <string>

namespace {

// What the shop's sign and till show: whether it is open, and the player's coins.
struct ShopSign {
    bool open = true;
    int coins = 0;
    std::string sign;
    std::string till;
};
SLASH_COMPONENT(ShopSign, open, coins, sign, till)

// Shows each sign in the game's locale; "Open" is a shop's here, not a door's.
void show_signs(slash::world::World& world, double /*tick_seconds*/) {
    world.ecs().view<ShopSign>().each([&world](ShopSign& shop) {
        shop.sign = shop.open ? slash::l10n::tr(world, "Open", "shop")
                              : slash::l10n::tr(world, "Thanks for shopping!");
        shop.till = slash::l10n::tr_n(world, "%d coin", "%d coins", shop.coins);
    });
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<ShopSign>().set_doc("A shop's sign and till.");
    game.add_system("show_signs", show_signs,
                    {.doc = "Shows each shop sign in the game's locale."});
}
