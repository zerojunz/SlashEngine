#include "slash/net/multiplayer.hpp"
#include "slash/net/websocket.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

// Polls until done says so, for up to five seconds of wall time.
bool poll_until(const std::function<void()>& poll, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        poll();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::string text_of(std::span<const std::uint8_t> bytes) {
    return {bytes.begin(), bytes.end()};
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string url_of(const net::WebSocketMultiplayerPeer& server) {
    return "ws://127.0.0.1:" + std::to_string(server.local_port()) + "/game";
}

} // namespace

TEST_CASE("websocket: a multiplayer peer's server and clients") {
    if (!net::websocket_server_supported()) {
        return;
    }
    net::WebSocketMultiplayerPeer server;
    REQUIRE(server.create_server(0, "127.0.0.1"));
    CHECK(server.is_server());
    CHECK(server.unique_id() == 1);
    CHECK(server.local_port() > 0);
    CHECK_FALSE(server.create_server(0)); // already in use

    net::WebSocketMultiplayerPeer a;
    net::WebSocketMultiplayerPeer b;
    REQUIRE(a.create_client(url_of(server)));
    REQUIRE(b.create_client(url_of(server)));
    CHECK(a.connection_status() == net::ConnectionStatus::Connecting);
    const auto poll_all = [&] {
        server.poll();
        a.poll();
        b.poll();
    };
    REQUIRE(poll_until(poll_all, [&] {
        return a.connection_status() == net::ConnectionStatus::Connected &&
               b.connection_status() == net::ConnectionStatus::Connected &&
               server.peer(a.unique_id()) != nullptr && server.peer(b.unique_id()) != nullptr;
    }));
    CHECK(a.unique_id() > 1);
    CHECK(a.unique_id() != b.unique_id());
    CHECK(a.take_events() == std::vector<net::PeerEvent>{{net::PeerEvent::Kind::Connected, 1}});
    CHECK(server.peer(a.unique_id())->requested_url().ends_with("/game"));

    // Packets to one client, to all and to all but one.
    server.set_target_peer(a.unique_id());
    REQUIRE(server.put_packet(bytes_of("to a")));
    server.set_target_peer(net::kTargetPeerBroadcast);
    REQUIRE(server.put_packet(bytes_of("to all")));
    server.set_target_peer(-a.unique_id());
    REQUIRE(server.put_packet(bytes_of("not to a")));
    std::vector<std::string> got_a;
    std::vector<std::string> got_b;
    REQUIRE(poll_until(poll_all, [&] {
        while (a.available_packet_count() > 0) {
            CHECK(a.packet_peer() == 1);
            got_a.push_back(text_of(a.get_packet().value()));
        }
        while (b.available_packet_count() > 0) {
            got_b.push_back(text_of(b.get_packet().value()));
        }
        return got_a.size() == 2 && got_b.size() == 2;
    }));
    CHECK(got_a == std::vector<std::string>{"to a", "to all"});
    CHECK(got_b == std::vector<std::string>{"to all", "not to a"});

    // A client's packet reaches the server with its sender.
    REQUIRE(b.put_packet(bytes_of("from b")));
    REQUIRE(poll_until(poll_all, [&] { return server.available_packet_count() == 1; }));
    CHECK(server.packet_peer() == b.unique_id());
    CHECK(text_of(server.get_packet().value()) == "from b");

    // A client leaving is a peer_disconnected on the server.
    CHECK(server.take_events().size() == 2); // both joined
    const int a_id = a.unique_id();
    a.close();
    CHECK(a.connection_status() == net::ConnectionStatus::Disconnected);
    std::vector<net::PeerEvent> events;
    REQUIRE(poll_until(poll_all, [&] {
        for (const net::PeerEvent& event : server.take_events()) {
            events.push_back(event);
        }
        return !events.empty();
    }));
    CHECK(events.front() == net::PeerEvent{net::PeerEvent::Kind::Disconnected, a_id});

    // When the server closes, the client is told.
    server.close();
    REQUIRE(
        poll_until([&] { b.poll(); },
                   [&] { return b.connection_status() == net::ConnectionStatus::Disconnected; }));
    CHECK(b.take_events().back() ==
          net::PeerEvent{net::PeerEvent::Kind::Disconnected, net::kTargetPeerServer});
}

TEST_CASE("websocket: a peer's text, subprotocols and close codes") {
    if (!net::websocket_server_supported()) {
        return;
    }
    net::WebSocketMultiplayerPeer server;
    server.set_supported_protocols({"chess", "checkers"});
    REQUIRE(server.create_server(0, "127.0.0.1"));

    // A client that speaks no protocol the server does is refused.
    net::WebSocketPeer stranger;
    stranger.set_supported_protocols({"go"});
    REQUIRE(stranger.connect_to_url(url_of(server)));
    REQUIRE(poll_until(
        [&] {
            server.poll();
            stranger.poll();
        },
        [&] { return stranger.ready_state() == net::WebSocketState::Closed; }));

    // One that does gets its ID in a first binary message.
    net::WebSocketPeer client;
    client.set_supported_protocols({"checkers"});
    REQUIRE(client.connect_to_url(url_of(server)));
    CHECK(client.ready_state() == net::WebSocketState::Connecting);
    const auto poll_both = [&] {
        server.poll();
        client.poll();
    };
    REQUIRE(poll_until(poll_both, [&] { return client.available_packet_count() == 1; }));
    CHECK(client.selected_protocol() == "checkers");
    const auto id_bytes = client.get_packet().value();
    REQUIRE(id_bytes.size() == 4);
    CHECK_FALSE(client.was_string_packet());
    int id = 0;
    std::memcpy(&id, id_bytes.data(), 4);
    CHECK(id > 1);
    REQUIRE(server.peer(id) != nullptr);

    // Text arrives as text.
    REQUIRE(client.send_text("king me"));
    REQUIRE(poll_until(poll_both, [&] { return server.available_packet_count() == 1; }));
    CHECK(text_of(server.get_packet().value()) == "king me");
    CHECK(server.peer(id)->was_string_packet());
    REQUIRE(server.peer(id)->send_text("ok"));
    REQUIRE(poll_until(poll_both, [&] { return client.available_packet_count() == 1; }));
    CHECK(text_of(client.get_packet().value()) == "ok");
    CHECK(client.was_string_packet());

    // A clean close carries its code and reason both ways.
    client.close(4000, "game over");
    CHECK(client.ready_state() == net::WebSocketState::Closing);
    REQUIRE(
        poll_until(poll_both, [&] { return client.ready_state() == net::WebSocketState::Closed; }));
    CHECK(client.close_code() == 4000);

    // Messages too big for the inbound buffer close the connection.
    net::WebSocketPeer small;
    small.set_supported_protocols({"chess"});
    small.set_inbound_buffer_size(16);
    REQUIRE(small.connect_to_url(url_of(server)));
    REQUIRE(poll_until(
        [&] {
            server.poll();
            small.poll();
        },
        [&] { return small.available_packet_count() == 1; }));
    int small_id = 0;
    std::memcpy(&small_id, small.get_packet().value().data(), 4);
    REQUIRE(server.peer(small_id)->put_packet(bytes_of(std::string(64, 'x'))));
    REQUIRE(poll_until(
        [&] {
            server.poll();
            small.poll();
        },
        [&] { return small.ready_state() == net::WebSocketState::Closed; }));
    CHECK(small.close_code() == 1009); // message too big
}

TEST_CASE("websocket: a world's multiplayer runs over it") {
    if (!net::websocket_server_supported()) {
        return;
    }
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    std::vector<std::unique_ptr<world::World>> worlds;
    std::vector<std::string> heard;
    for (int i = 0; i < 3; ++i) {
        auto& w = *worlds.emplace_back(std::make_unique<world::World>(components));
        (void)w.create("table");
        net::install(w);
        net::multiplayer(w).add_rpc(
            "say", {.mode = net::RpcMode::AnyPeer},
            [&heard, i](world::World& from, entt::entity, const json::Value& args) {
                heard.push_back(
                    std::to_string(i) + " heard " + args.as_string() + " from " +
                    (net::multiplayer(from).remote_sender_id() == 1 ? "the server" : "a client"));
            });
    }
    auto server = std::make_shared<net::WebSocketMultiplayerPeer>();
    REQUIRE(server->create_server(0, "127.0.0.1"));
    REQUIRE(net::multiplayer(*worlds[0]).set_peer(server));
    for (std::size_t i = 1; i < 3; ++i) {
        auto client = std::make_shared<net::WebSocketMultiplayerPeer>();
        REQUIRE(client->create_client(url_of(*server)));
        REQUIRE(net::multiplayer(*worlds[i]).set_peer(client));
    }
    const auto step = [&] {
        for (auto& w : worlds) {
            w->step(1.0 / 60.0);
        }
    };
    // The clients hear of each other through the server, and a call relays between them.
    REQUIRE(poll_until(step, [&] {
        return net::multiplayer(*worlds[1]).peer_ids().size() == 2 &&
               net::multiplayer(*worlds[2]).peer_ids().size() == 2;
    }));
    REQUIRE(net::multiplayer(*worlds[1]).rpc("table", "say", "hi"));
    REQUIRE(poll_until(step, [&] { return heard.size() == 2; }));
    std::ranges::sort(heard);
    CHECK(heard ==
          std::vector<std::string>{"0 heard hi from a client", "2 heard hi from a client"});
}
