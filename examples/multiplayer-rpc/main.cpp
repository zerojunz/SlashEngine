// Three worlds in one program play one game over ENet, as Godot's SceneMultiplayer runs it: a
// server and two clients that hear about each other through the server, a chat call any peer
// may make, each player's position sent unreliably by the client it belongs to, a door only its
// authority may open, raw packets and a client leaving. IDs change every run, so the output names
// peers by role.
#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

std::map<int, std::string> names; // peer ID to role

std::string name_of(int peer) {
    const auto found = names.find(peer);
    return found != names.end() ? found->second : "?";
}

// One machine: its world, with a player for each client and a door, and what happened in it.
struct Machine {
    std::string name;
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    std::vector<std::string> log;

    explicit Machine(std::string role) : name(std::move(role)) {
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        for (const char* id : {"alice", "bob", "door"}) {
            const entt::entity entity = world->create(id).value();
            world->ecs().emplace<world::Transform2D>(entity);
        }
        world->set_event_listener([this](const world::Event& event) {
            if (event.kind.starts_with("peer_") || event.kind.find("server") != std::string::npos) {
                std::string line = event.kind;
                if (const json::Value* peer = event.data.find("peer")) {
                    line += " " + name_of(static_cast<int>(peer->as_int()));
                }
                log.push_back(line);
            }
        });
        net::install(*world);
        net::Multiplayer& mp = multiplayer();

        // Any peer may chat; the server and every client run it.
        mp.add_rpc("chat", {.mode = net::RpcMode::AnyPeer, .call_local = true},
                   [this](world::World&, entt::entity, const json::Value& args) {
                       log.push_back(name_of(multiplayer().remote_sender_id()) + " says \"" +
                                     args.find("text")->as_string() + "\"");
                   });
        // A player's position, from the client it belongs to (its authority), unreliably on
        // channel 1, as positions are sent many times a second.
        mp.add_rpc("move",
                   {.transfer_mode = net::TransferMode::UnreliableOrdered,
                    .channel = 1,
                    .component = "Transform2D"},
                   [this](world::World& w, entt::entity entity, const json::Value& args) {
                       auto& transform = w.ecs().get<world::Transform2D>(entity);
                       const json::Array& xy = args.as_array();
                       transform.position = Vec2{static_cast<float>(xy[0].as_double()),
                                                 static_cast<float>(xy[1].as_double())};
                       log.push_back(std::string(w.id_of(entity)) + " moved to " +
                                     json::write(args, json::Style::Compact));
                   });
        // The door opens for whoever is its authority.
        mp.add_rpc(
            "open", {.call_local = true}, [this](world::World&, entt::entity, const json::Value&) {
                log.push_back("the door opens for " + name_of(multiplayer().remote_sender_id()));
            });
    }

    [[nodiscard]] net::Multiplayer& multiplayer() const { return net::multiplayer(*world); }

    void print() {
        std::sort(log.begin(), log.end());
        for (const std::string& line : log) {
            std::printf("  %s: %s\n", name.c_str(), line.c_str());
        }
        log.clear();
    }
};

// Steps every world, a tick each, until done says so (up to five seconds).
void step_until(std::initializer_list<Machine*> machines, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        for (Machine* machine : machines) {
            machine->world->step(1.0 / 60.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void step_for(std::initializer_list<Machine*> machines, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    step_until(machines, [&] { return std::chrono::steady_clock::now() >= until; });
}

} // namespace

int main() {
    if (!net::enet_supported()) {
        std::printf("ENet isn't available here\n");
        return 0;
    }
    Machine server("server");
    Machine alice("alice");
    Machine bob("bob");
    names[1] = "the server";

    std::printf("The server listens; alice and bob join:\n");
    auto hosting = std::make_shared<net::ENetPeer>();
    (void)hosting->create_server(0);
    (void)server.multiplayer().set_peer(hosting);
    for (Machine* client : {&alice, &bob}) {
        auto joining = std::make_shared<net::ENetPeer>();
        (void)joining->create_client("127.0.0.1", hosting->local_port());
        (void)client->multiplayer().set_peer(joining);
        names[client->multiplayer().unique_id()] = client->name;
    }
    step_until({&server, &alice, &bob}, [&] {
        return alice.multiplayer().peer_ids().size() == 2 &&
               bob.multiplayer().peer_ids().size() == 2;
    });
    server.print();
    alice.print();
    bob.print();

    std::printf("\nEach player answers to its client:\n");
    for (Machine* machine : {&server, &alice, &bob}) {
        machine->multiplayer().set_authority("alice", alice.multiplayer().unique_id());
        machine->multiplayer().set_authority("bob", bob.multiplayer().unique_id());
    }
    std::printf("  alice is the authority of alice: %d, of bob: %d, of the door: %d\n",
                alice.multiplayer().is_authority("alice") ? 1 : 0,
                alice.multiplayer().is_authority("bob") ? 1 : 0,
                alice.multiplayer().is_authority("door") ? 1 : 0);

    std::printf("\nAlice chats; the server relays it to bob, and it runs on alice too:\n");
    (void)alice.multiplayer().rpc("alice", "chat", json::Object{{"text", "hello!"}});
    step_until({&server, &alice, &bob}, [&] { return !bob.log.empty() && !server.log.empty(); });
    server.print();
    alice.print();
    bob.print();

    std::printf("\nBob moves his player, then tries to move alice's:\n");
    (void)bob.multiplayer().rpc("bob", "move", json::Array{120, 64});
    (void)bob.multiplayer().rpc("alice", "move", json::Array{0, 0});
    step_until({&server, &alice, &bob}, [&] { return !alice.log.empty() && !server.log.empty(); });
    step_for({&server, &alice, &bob}, 50);
    server.print();
    alice.print();
    std::printf("  (the server and alice log an error for the second: bob isn't alice's "
                "authority)\n");
    const auto& moved = server.world->ecs().get<world::Transform2D>(server.world->find("bob"));
    std::printf("  bob's player is at %g, %g on the server\n",
                static_cast<double>(moved.position.x), static_cast<double>(moved.position.y));

    std::printf("\nThe door answers to the server, so only the server opens it everywhere:\n");
    (void)server.multiplayer().rpc("door", "open");
    step_until({&server, &alice, &bob}, [&] { return !alice.log.empty() && !bob.log.empty(); });
    server.print();
    alice.print();
    bob.print();

    std::printf("\nThe server sends raw bytes to everyone:\n");
    const std::string news = "round 2";
    (void)server.multiplayer().send_bytes(std::vector<std::uint8_t>(news.begin(), news.end()));
    std::vector<net::RawPacket> got;
    step_until({&server, &alice, &bob}, [&] {
        for (net::RawPacket& packet : alice.multiplayer().take_packets()) {
            got.push_back(std::move(packet));
        }
        return !got.empty();
    });
    std::printf("  alice got \"%s\" from %s\n",
                std::string(got[0].bytes.begin(), got[0].bytes.end()).c_str(),
                name_of(got[0].peer).c_str());
    server.log.clear();
    alice.log.clear();
    bob.log.clear();

    std::printf("\nBob leaves:\n");
    bob.multiplayer().peer().close();
    step_until({&server, &alice, &bob}, [&] { return alice.multiplayer().peer_ids().size() == 1; });
    server.print();
    alice.print();
    bob.print();
    return 0;
}
