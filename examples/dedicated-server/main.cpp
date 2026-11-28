// Dedicated server: the same game run as a player's copy and as a server.
//
// The game's code (the arena function, as SLASH_GAME_MODULE would register it) asks
// world::is_dedicated_server once the world starts ticking: a server hosts the match on a
// network peer, and a player's copy would join one. export_presets.json has a "linux-server"
// preset with dedicated_server, which builds the game to always run as a server, with no window,
// GPU or sound, and leaves hero.png out of its data; any game runs that way with --server.
//
// This program starts the project with runtime::Game, as an export's main() does: first as a
// server for ten ticks, which take half a second at the project's 20 ticks a second, then as a
// headless player for one, and prints what each did and the summary each prints when it stops.

#include "slash/core/json.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/runtime/game.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/server.hpp"
#include "slash/world/world.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace {

namespace exporter = slash::exporter;
namespace json = slash::json;
namespace net = slash::net;
namespace runtime = slash::runtime;
namespace world = slash::world;

std::string role;

// The game's code: a server hosts on a free port the first tick; a player would join one.
void arena(world::GameModule& game) {
    game.add_system("lobby", [](world::World& game_world, double /*tick_seconds*/) {
        if (!role.empty()) {
            return;
        }
        if (!world::is_dedicated_server(game_world)) {
            role = "player: would join a server";
            return;
        }
        auto peer = std::make_shared<net::ENetPeer>();
        if (!peer->create_server(0, 8)) {
            role = "server: could not host";
            return;
        }
        (void)net::multiplayer(game_world).set_peer(peer);
        role = net::multiplayer(game_world).is_server() ? "server: hosting the match"
                                                        : "server: not hosting";
    });
}

void play(runtime::GameOptions options) {
    role.clear();
    options.data_dir = ".";
    options.gameplay = arena;
    auto game = runtime::Game::start(options);
    if (!game) {
        std::printf("did not start: %s\n", game.error().message.c_str());
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    while (game.value()->frame()) {
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%s\n", role.c_str());
    // A server keeps to the clock; a headless test runs as fast as it can.
    std::printf("kept to real time: %s\n", seconds >= 0.4 ? "yes" : "no");
    json::Value summary = game.value()->summary();
    json::Value shown;
    for (const char* key : {"headless", "renderer", "server", "sprites", "ticks"}) {
        if (const json::Value* value = summary.find(key); value != nullptr) {
            shown[key] = *value;
        }
    }
    std::printf("%s\n", json::write(shown, json::Style::Compact).c_str());
}

} // namespace

int main() {
    auto presets = exporter::load_presets(".");
    if (presets) {
        for (const auto& [name, preset] : presets.value()) {
            std::printf("preset %s: %s\n", name.c_str(),
                        preset.dedicated_server ? "dedicated server" : "game");
        }
    }
    std::printf("-- as a server\n");
    play({.data_dir = {}, .headless = false, .server = true, .max_ticks = 10});
    std::printf("-- as a player, headless\n");
    play({.data_dir = {}, .headless = true, .server = false, .max_ticks = 1});
    return 0;
}
