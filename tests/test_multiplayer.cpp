#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

// One machine's game: a world with a player and a door, its multiplayer and what it heard.
struct Game {
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    std::vector<world::Event> events;
    std::vector<std::string> said; // "<entity>:<text> from <sender>"

    Game() {
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        (void)world->create("player");
        (void)world->create("door");
        world->ecs().emplace<world::Transform2D>(world->find("player"));
        world->set_event_listener([this](const world::Event& event) { events.push_back(event); });
        net::install(*world);
        net::Multiplayer& mp = multiplayer();
        mp.add_rpc("say", {.mode = net::RpcMode::AnyPeer},
                   [this](world::World& w, entt::entity entity, const json::Value& args) {
                       said.push_back(std::string(w.id_of(entity)) + ":" +
                                      args.find("text")->as_string() + " from " +
                                      std::to_string(multiplayer().remote_sender_id()));
                   });
        mp.add_rpc("open", {.call_local = true},
                   [this](world::World& w, entt::entity entity, const json::Value& /*args*/) {
                       said.push_back(std::string(w.id_of(entity)) + ":open from " +
                                      std::to_string(multiplayer().remote_sender_id()));
                   });
        mp.add_rpc("jump", {.mode = net::RpcMode::AnyPeer, .component = "Transform2D"},
                   [this](world::World& w, entt::entity entity, const json::Value& args) {
                       said.push_back(std::string(w.id_of(entity)) + ":jump " +
                                      json::write(args, json::Style::Compact));
                   });
    }

    [[nodiscard]] net::Multiplayer& multiplayer() const { return net::multiplayer(*world); }

    // The kinds of the events heard since the last call, with their peers.
    std::vector<std::string> take(std::string_view prefix = "") {
        std::vector<std::string> kinds;
        for (const world::Event& event : events) {
            if (!event.kind.starts_with(prefix)) {
                continue;
            }
            std::string kind = event.kind;
            if (const json::Value* peer = event.data.find("peer")) {
                kind += " " + std::to_string(peer->as_int());
            }
            kinds.push_back(kind);
        }
        events.clear();
        return kinds;
    }
};

// Steps every world until done says so, for up to five seconds of wall time.
bool step_until(std::initializer_list<Game*> games, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Game* game : games) {
            game->world->step(1.0 / 60.0);
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Steps for a little while, for things that must not happen.
void step_for(std::initializer_list<Game*> games, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    step_until(games, [&] { return std::chrono::steady_clock::now() >= until; });
}

std::shared_ptr<net::ENetPeer> serve(int& port) {
    auto peer = std::make_shared<net::ENetPeer>();
    REQUIRE(peer->create_server(0).ok());
    port = peer->local_port();
    return peer;
}

std::shared_ptr<net::ENetPeer> join(int port) {
    auto peer = std::make_shared<net::ENetPeer>();
    REQUIRE(peer->create_client("127.0.0.1", port).ok());
    return peer;
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

} // namespace

TEST_CASE("a world's multiplayer starts offline") {
    const Game game;
    net::Multiplayer& mp = game.multiplayer();
    CHECK(mp.unique_id() == 1);
    CHECK(mp.is_server());
    CHECK(mp.peer_ids().empty());
    CHECK(mp.authority("door") == 1);
    CHECK(mp.is_authority("door"));

    // Alone, a call runs only where it is local.
    REQUIRE(mp.rpc("door", "open").ok());
    REQUIRE(mp.rpc("door", "say", json::Object{{"text", "anyone?"}}).ok());
    CHECK(game.said == std::vector<std::string>{"door:open from 1"});
    CHECK(mp.rpc("door", "say", {}, 1).error().code == ErrorCode::InvalidArgument);
    CHECK(mp.rpc("door", "fly").error().code == ErrorCode::NotFound);
    CHECK(mp.rpc("nobody", "open").error().code == ErrorCode::NotFound);
    // jump belongs to entities with a Transform2D.
    CHECK(mp.rpc("door", "jump").error().code == ErrorCode::NotFound);
    CHECK(mp.set_peer(std::make_shared<net::ENetPeer>()).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("peers join through the server and call each other") {
    if (!net::enet_supported()) {
        return;
    }
    Game server;
    Game alice;
    Game bob;
    int port = 0;
    REQUIRE(server.multiplayer().set_peer(serve(port)).ok());
    REQUIRE(alice.multiplayer().set_peer(join(port)).ok());
    REQUIRE(bob.multiplayer().set_peer(join(port)).ok());
    const int alice_id = alice.multiplayer().unique_id();
    const int bob_id = bob.multiplayer().unique_id();

    // Each client hears the server and, through it, the other client.
    REQUIRE(step_until({&server, &alice, &bob}, [&] {
        return server.multiplayer().peer_ids().size() == 2 &&
               alice.multiplayer().peer_ids().size() == 2 &&
               bob.multiplayer().peer_ids().size() == 2;
    }));
    const std::vector<std::string> heard = alice.take();
    CHECK(contains(heard, "connected_to_server"));
    CHECK(contains(heard, "peer_connected 1"));
    CHECK(contains(heard, "peer_connected " + std::to_string(bob_id)));
    CHECK(contains(server.take(), "peer_connected " + std::to_string(alice_id)));
    CHECK_FALSE(alice.multiplayer().is_server());

    SUBCASE("a call reaches every peer, relayed by the server, with its sender") {
        REQUIRE(alice.multiplayer().rpc("player", "say", json::Object{{"text", "hi"}}).ok());
        REQUIRE(step_until({&server, &alice, &bob},
                           [&] { return !server.said.empty() && !bob.said.empty(); }));
        const std::string expected = "player:hi from " + std::to_string(alice_id);
        CHECK(server.said == std::vector<std::string>{expected});
        CHECK(bob.said == std::vector<std::string>{expected});
        CHECK(alice.said.empty()); // not call_local

        // Once the peers confirmed the entity, calls name it by a short ID; they still arrive.
        step_for({&server, &alice, &bob}, 50);
        REQUIRE(
            alice.multiplayer().rpc("player", "jump", json::Object{{"height", 2}}, bob_id).ok());
        REQUIRE(step_until({&server, &alice, &bob}, [&] { return bob.said.size() == 2; }));
        CHECK(bob.said[1] == "player:jump {\"height\":2}");
        CHECK(server.said.size() == 1); // sent to bob alone
    }

    SUBCASE("only an entity's authority may call its authority calls") {
        // The door answers to the server, so alice's call is refused where it arrives.
        REQUIRE(alice.multiplayer().rpc("door", "open").ok());
        CHECK(alice.said == std::vector<std::string>{"door:open from " + std::to_string(alice_id)});
        step_for({&server, &alice, &bob}, 100);
        CHECK(server.said.empty());
        CHECK(bob.said.empty());

        // Once every peer gives the door to alice, her calls go through.
        for (Game* game : {&server, &alice, &bob}) {
            game->multiplayer().set_authority("door", alice_id);
        }
        CHECK(alice.multiplayer().is_authority("door"));
        REQUIRE(alice.multiplayer().rpc("door", "open").ok());
        REQUIRE(step_until({&server, &alice, &bob},
                           [&] { return !server.said.empty() && !bob.said.empty(); }));
        CHECK(server.said[0] == "door:open from " + std::to_string(alice_id));
    }

    SUBCASE("raw packets go to one peer or all") {
        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        REQUIRE(bob.multiplayer().send_bytes(ping, 1).ok());
        REQUIRE(server.multiplayer().send_bytes(ping).ok());
        std::vector<net::RawPacket> at_server;
        std::vector<net::RawPacket> at_alice;
        REQUIRE(step_until({&server, &alice, &bob}, [&] {
            for (net::RawPacket& packet : server.multiplayer().take_packets()) {
                at_server.push_back(std::move(packet));
            }
            for (net::RawPacket& packet : alice.multiplayer().take_packets()) {
                at_alice.push_back(std::move(packet));
            }
            return !at_server.empty() && !at_alice.empty();
        }));
        CHECK(at_server[0].peer == bob_id);
        CHECK(at_server[0].bytes == ping);
        CHECK(at_alice[0].peer == 1);
        CHECK(contains(server.take("peer_packet"), "peer_packet " + std::to_string(bob_id)));
        CHECK(server.multiplayer().send_bytes({}).error().code == ErrorCode::InvalidArgument);
    }

    SUBCASE("peers that leave are reported to the others") {
        bob.multiplayer().peer().close();
        REQUIRE(step_until({&server, &alice, &bob}, [&] {
            return server.multiplayer().peer_ids().size() == 1 &&
                   alice.multiplayer().peer_ids().size() == 1;
        }));
        CHECK(contains(server.take(), "peer_disconnected " + std::to_string(bob_id)));
        CHECK(contains(alice.take(), "peer_disconnected " + std::to_string(bob_id)));
        CHECK(contains(bob.take(), "server_disconnected"));

        server.multiplayer().peer().close();
        REQUIRE(
            step_until({&server, &alice}, [&] { return alice.multiplayer().peer_ids().empty(); }));
        CHECK(contains(alice.take(), "server_disconnected"));
    }
}

TEST_CASE("peers authenticate before they are admitted") {
    if (!net::enet_supported()) {
        return;
    }
    Game server;
    Game client;
    std::vector<std::string> server_got;
    server.multiplayer().set_auth_callback([&](int peer, std::span<const std::uint8_t> data) {
        server_got.emplace_back(data.begin(), data.end());
        if (std::string(data.begin(), data.end()) == "secret") {
            (void)server.multiplayer().complete_auth(peer);
        }
    });
    client.multiplayer().set_auth_callback([&](int /*peer*/, std::span<const std::uint8_t>) {});
    int port = 0;
    REQUIRE(server.multiplayer().set_peer(serve(port)).ok());
    REQUIRE(client.multiplayer().set_peer(join(port)).ok());
    const int client_id = client.multiplayer().unique_id();

    REQUIRE(step_until({&server, &client}, [&] {
        return !client.multiplayer().authenticating_peer_ids().empty() &&
               !server.multiplayer().authenticating_peer_ids().empty();
    }));
    CHECK(contains(server.take(), "peer_authenticating " + std::to_string(client_id)));
    CHECK(server.multiplayer().peer_ids().empty());

    const std::vector<std::uint8_t> secret{'s', 'e', 'c', 'r', 'e', 't'};
    REQUIRE(client.multiplayer().send_auth(1, secret).ok());
    REQUIRE(client.multiplayer().complete_auth(1).ok());
    CHECK(client.multiplayer().complete_auth(1).error().code == ErrorCode::Conflict);
    REQUIRE(step_until({&server, &client}, [&] {
        return server.multiplayer().peer_ids().size() == 1 &&
               client.multiplayer().peer_ids().size() == 1;
    }));
    CHECK(server_got == std::vector<std::string>{"secret"});
    CHECK(contains(client.take(), "connected_to_server"));

    SUBCASE("a peer that doesn't finish in time is dropped") {
        Game late;
        late.multiplayer().set_auth_callback([](int, std::span<const std::uint8_t>) {});
        server.multiplayer().set_auth_timeout(0.1);
        REQUIRE(late.multiplayer().set_peer(join(port)).ok());
        const int late_id = late.multiplayer().unique_id();
        REQUIRE(step_until({&server, &client, &late}, [&] {
            return contains(server.take("peer_authentication"),
                            "peer_authentication_failed " + std::to_string(late_id));
        }));
        CHECK(server.multiplayer().peer_ids().size() == 1);
    }
}
