// A game played on three sessions at once, each driven only by commands as three agents would
// drive three copies of a game: the server hosts with net.host, two players join with net.join,
// and every session steps. A chat call travels with net.rpc, a bullet the server places under
// its spawner appears on both players and follows the server's moves, restoring a snapshot on
// the server keeps it spawned, and destroying it despawns it everywhere. IDs and ports change every
// run, so the output names peers by role.
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

std::map<std::int64_t, std::string> names; // peer ID to role
int heard = 0;                             // calls of "say" that ran

// One machine's session over the game, with the game's "say" procedure.
struct Player {
    std::string name;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Player(world::ComponentRegistry& components, std::string role) : name(std::move(role)) {
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        (void)call("scene.open", R"({"path": "arena.scene.json"})");
        // The game's code registers its procedures on every peer.
        net::multiplayer(session->world())
            .add_rpc("say", {.mode = net::RpcMode::AnyPeer, .call_local = true},
                     [this](world::World& world, entt::entity, const json::Value& args) {
                         const int from = net::multiplayer(world).remote_sender_id();
                         ++heard;
                         std::printf("  %s hears %s say \"%s\"\n", name.c_str(),
                                     names[from].c_str(), args.as_string().c_str());
                     });
    }

    [[nodiscard]] command::Reply call(std::string_view command, std::string_view params) const {
        return session->call(command, json::parse(params).value());
    }

    // Calls a command and returns its result, printing the error if it failed.
    [[nodiscard]] json::Value ok(std::string_view command, std::string_view params = "{}") const {
        command::Reply reply = call(command, params);
        if (!reply.ok) {
            std::printf("  %s: %.*s failed: %s\n", name.c_str(), static_cast<int>(command.size()),
                        command.data(), reply.message.c_str());
            return {};
        }
        return std::move(reply.value);
    }

    // net.state, with peers named by role and the port left out.
    void print_state() const {
        json::Value state = ok("net.state");
        if (state.find("transport")->as_string() == "offline") {
            std::printf("  %s: offline, a server alone\n", name.c_str());
            return;
        }
        std::vector<std::string> roles;
        for (const json::Value& id : state.find("peers")->as_array()) {
            roles.push_back(names[id.as_int()]);
        }
        std::ranges::sort(roles); // IDs are random, so not by ID
        std::string peers;
        for (const std::string& role : roles) {
            peers += (peers.empty() ? "" : ", ") + role;
        }
        std::printf(
            "  %s: %s over %s as %s, peers: %s\n", name.c_str(),
            state.find("status")->as_string().c_str(), state.find("transport")->as_string().c_str(),
            names[state.find("id")->as_int()].c_str(), peers.empty() ? "none" : peers.c_str());
    }

    // Where the bullet is in this session, or that it isn't.
    void print_bullet() const {
        const command::Reply reply =
            call("entity.get", R"({"id": "bullet", "components": ["Transform2D"]})");
        if (!reply.ok) {
            std::printf("  %s: no bullet\n", name.c_str());
            return;
        }
        const json::Value* position =
            reply.value.find("components")->find("Transform2D")->find("position");
        std::printf("  %s: the bullet is at %s\n", name.c_str(),
                    json::write(*position, json::Style::Compact).c_str());
    }
};

// Steps every session a tick at a time until done says so, for up to five seconds.
bool step_until(const std::vector<Player*>& players, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const Player* player : players) {
            (void)player->ok("step");
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::string bullet_position(const Player& player) {
    const command::Reply reply =
        player.call("entity.get", R"({"id": "bullet", "components": ["Transform2D"]})");
    if (!reply.ok) {
        return "";
    }
    return json::write(*reply.value.find("components")->find("Transform2D")->find("position"),
                       json::Style::Compact);
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    Player server(components, "the server");
    Player alice(components, "alice");
    Player bob(components, "bob");
    const std::vector<Player*> everyone = {&server, &alice, &bob};
    names[1] = "the server";

    std::printf("The server hosts on a free port; alice and bob join it:\n");
    const std::int64_t port = server.ok("net.host").find("port")->as_int();
    const std::string join = R"({"port": )" + std::to_string(port) + "}";
    const json::Value alice_joined = alice.ok("net.join", join);
    const json::Value bob_joined = bob.ok("net.join", join);
    names[alice_joined.find("id")->as_int()] = "alice";
    names[bob_joined.find("id")->as_int()] = "bob";
    std::printf("  alice: %s, bob: %s\n", alice_joined.find("status")->as_string().c_str(),
                bob_joined.find("status")->as_string().c_str());
    if (!step_until(everyone, [&] {
            return alice.ok("net.state").find("peers")->as_array().size() == 2 &&
                   bob.ok("net.state").find("peers")->as_array().size() == 2;
        })) {
        std::printf("  the players didn't connect\n");
        return 1;
    }
    for (const Player* player : everyone) {
        player->print_state();
    }

    std::printf("\nAlice says hello with net.rpc; it runs on her session and reaches the others "
                "through the server:\n");
    (void)alice.ok("net.rpc", R"({"entity": "table", "method": "say", "args": "hello"})");
    step_until(everyone, [] { return heard == 3; });

    std::printf("\nThe server places a bullet under the spawner's arena; both players get it:\n");
    (void)server.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "bullet", "extends": "bullet.prefab.json"}},
        {"command": "component.set",
         "params": {"id": "arena", "components": {"Transform2D": {"children": ["bullet"]}}}}]})");
    step_until(everyone,
               [&] { return !bullet_position(alice).empty() && !bullet_position(bob).empty(); });
    for (const Player* player : everyone) {
        player->print_bullet();
    }

    std::printf("\nThe server saves a snapshot, then moves it; its synchronizer sends the position "
                "every tick:\n");
    (void)server.ok("world.snapshot");
    (void)server.ok("component.set",
                    R"({"id": "bullet", "components": {"Transform2D": {"position": [50, 60]}}})");
    step_until(everyone, [&] { return bullet_position(bob) == "[50.0,60.0]"; });
    bob.print_bullet();

    std::printf("\nworld.restore on the server puts its entities back from the snapshot; the "
                "bullet goes back and stays spawned:\n");
    (void)server.ok("world.restore");
    step_until(everyone, [&] { return bullet_position(bob) == "[10.0,20.0]"; });
    bob.print_bullet();
    const json::Value state = bob.ok("net.state");
    std::printf("  bob's net.state: spawned %s\n",
                json::write(*state.find("spawned"), json::Style::Compact).c_str());

    std::printf("\nThe server destroys it; it is despawned everywhere:\n");
    (void)server.ok("entity.destroy", R"({"id": "bullet"})");
    step_until(everyone,
               [&] { return bullet_position(alice).empty() && bullet_position(bob).empty(); });
    for (const Player* player : everyone) {
        player->print_bullet();
    }

    std::printf("\nBob leaves with net.close; the others hear him go:\n");
    (void)bob.ok("net.close");
    step_until(everyone,
               [&] { return alice.ok("net.state").find("peers")->as_array().size() == 1; });
    for (const Player* player : everyone) {
        player->print_state();
    }
    return 0;
}
