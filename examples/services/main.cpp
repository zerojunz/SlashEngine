// Ads, analytics and in-app purchases, as Unity's services: project.json names the ad
// placements and products, gameplay code asks for them by name, and what happens arrives as
// events at a later tick. A shop sells coins for a watched ad or a purchase, and ads go away for
// good with remove_ads.
//
// This program adds the shop's system, then plays it as an agent does over MCP or JSON-RPC.
// Sessions run the services on the test backend, so ads load and purchases go through with no
// store account: ui.press presses the shop's buttons, services.test says how the next purchase
// ends and buys as the game would, and services.state shows what the services saw.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/services/services.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace game {

struct Wallet {
    int coins = 0;
    bool ads_removed = false;
};
SLASH_COMPONENT(Wallet, coins, ads_removed)

} // namespace game

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace services = slash::services;
namespace ui = slash::ui;
namespace world = slash::world;

constexpr int kCoinsPerPurchase = 100;

void earn(world::World& game_world, game::Wallet& wallet, int coins, std::string_view source) {
    wallet.coins += coins;
    json::Value params;
    params["source"] = source;
    params["coins"] = coins;
    // Kept for services.state either way; sent only once the player agrees to analytics.
    (void)services::log_event(game_world, "coins_earned", params);
}

// The shop: buttons ask the services, and their events pay out.
void shop(world::World& game_world, double /*tick_seconds*/) {
    const entt::entity wallet_entity = game_world.find("coins");
    auto& wallet = game_world.ecs().get<game::Wallet>(wallet_entity);
    if (ui::was_pressed(game_world, game_world.find("watch_ad"))) {
        // Fails if no ad is loaded; a real game would grey the button out until ad_loaded.
        (void)services::show_ad(game_world, "bonus");
    }
    if (ui::was_pressed(game_world, game_world.find("buy_coins"))) {
        (void)services::buy(game_world, "coins_100");
    }
    for (const services::ServiceEvent& event : services::events(game_world)) {
        if (event.kind == "ad_rewarded") {
            earn(game_world, wallet, static_cast<int>(std::lround(event.reward.amount)), "ad");
        } else if (event.kind == "ad_loaded" && event.id == "footer" && !wallet.ads_removed) {
            (void)services::show_ad(game_world, "footer");
        } else if (event.kind == "purchase_completed") {
            // Grant it, then finish it: until then the store delivers it again at each start.
            if (event.id == "coins_100") {
                earn(game_world, wallet, kCoinsPerPurchase, "purchase");
            } else if (event.id == "remove_ads" && !wallet.ads_removed) {
                wallet.ads_removed = true;
                (void)services::hide_ad(game_world, "footer");
            }
            (void)services::finish(game_world, event.purchase.transaction);
        }
    }
    game_world.ecs().get<world::Label>(wallet_entity).text =
        std::to_string(wallet.coins) + " coins";
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Wallet>()
        .set_doc("The player's coins, and whether they bought remove_ads.")
        .describe("coins", {.doc = "Coins to spend."})
        .describe("ads_removed", {.doc = "No banner shows."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    // The session reads project.json's placements and products and runs them on the test
    // backend.
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("shop", shop);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    w.ecs().emplace<game::Wallet>(w.find("coins"));
    // At the first tick the products' prices arrive and both ads load; the shop shows the
    // banner, which shows at the next.
    call(session, "step", R"({"ticks": 2})");
    call(session, "services.state", R"({"events": 0})");

    // A watched ad: shown at the next tick, closed with its reward the tick after.
    call(session, "ui.press", R"({"id": "watch_ad"})");
    call(session, "step", R"({"ticks": 3})");
    call(session, "entity.get",
         R"({"id": "coins", "components": ["Wallet", "Label"], "omit_defaults": true})");
    call(session, "services.state", R"({"events": 1})");

    // With the player's consent, events are sent. Buying coins: the shop grants them and
    // finishes the purchase, so nothing is left unfinished.
    call(session, "services.test", R"({"consent": {"analytics": "granted"}})");
    call(session, "ui.press", R"({"id": "buy_coins"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get", R"({"id": "coins", "components": ["Wallet"]})");
    call(session, "services.state", R"({"events": 1})");

    // A purchase a parent must approve waits as pending until complete_pending ends it. Then
    // remove_ads is owned and the banner is gone.
    call(session, "services.test", R"({"outcome": "pending", "buy": "remove_ads"})");
    call(session, "step", "{}");
    call(session, "services.state", R"({"events": 0})");
    call(session, "services.test", R"({"complete_pending": "purchased"})");
    call(session, "step", "{}");
    call(session, "entity.get", R"({"id": "coins", "components": ["Wallet"]})");
    call(session, "services.state", R"({"events": 0})");

    // A declined card fails the purchase, and the wallet stays as it was.
    call(session, "services.test", R"({"outcome": "failed", "error": "card declined"})");
    call(session, "ui.press", R"({"id": "buy_coins"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get", R"({"id": "coins", "components": ["Wallet"]})");

    // Mistakes are caught with the closest name, and a failed call changes nothing.
    call(session, "services.test", R"({"show": "bnous"})");
    call(session, "services.test", R"({"buy": "remove_ads"})");
    return 0;
}
