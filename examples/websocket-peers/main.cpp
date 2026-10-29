// WebSocket on one machine, as Godot's WebSocketPeer and WebSocketMultiplayerPeer: a server
// that speaks the "chat" subprotocol, a plain connection that is given its ID, trades text and
// bytes and closes with a code, one refused for speaking no protocol the server does, then three
// worlds whose multiplayer runs over WebSocket and relays a remote call between two clients.
// IDs and ports change every run, so the output names peers by role.
#include "slash/net/multiplayer.hpp"
#include "slash/net/websocket.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

// Polls until done says so, for up to five seconds.
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

const char* state_name(net::WebSocketState state) {
    switch (state) {
    case net::WebSocketState::Connecting:
        return "connecting";
    case net::WebSocketState::Open:
        return "open";
    case net::WebSocketState::Closing:
        return "closing";
    case net::WebSocketState::Closed:
        break;
    }
    return "closed";
}

std::string text_of(std::span<const std::uint8_t> bytes) {
    return {bytes.begin(), bytes.end()};
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

int fail(const char* what) {
    std::printf("failed: %s\n", what);
    return 1;
}

} // namespace

int main() {
    net::WebSocketMultiplayerPeer server;
    server.set_supported_protocols({"chat"});
    if (!server.create_server(0, "127.0.0.1")) {
        return fail("the server didn't listen");
    }
    const std::string url = "ws://127.0.0.1:" + std::to_string(server.local_port()) + "/lobby";
    std::printf("The server listens on a free port and speaks \"chat\".\n\n");

    std::printf("A client that speaks only \"voice\" is refused:\n");
    net::WebSocketPeer stranger;
    stranger.set_supported_protocols({"voice"});
    (void)stranger.connect_to_url(url);
    poll_until(
        [&] {
            server.poll();
            stranger.poll();
        },
        [&] { return stranger.ready_state() == net::WebSocketState::Closed; });
    std::printf("  stranger: %s, close code %d\n\n", state_name(stranger.ready_state()),
                stranger.close_code());

    std::printf("A client that speaks \"chat\" joins and is given its ID:\n");
    net::WebSocketPeer guest;
    guest.set_supported_protocols({"voice", "chat"});
    (void)guest.connect_to_url(url);
    std::printf("  guest: %s\n", state_name(guest.ready_state()));
    const auto poll_both = [&] {
        server.poll();
        guest.poll();
    };
    if (!poll_until(poll_both, [&] { return guest.available_packet_count() == 1; })) {
        return fail("the guest got no ID");
    }
    const auto id_bytes = guest.get_packet().value();
    int id = 0;
    std::memcpy(&id, id_bytes.data(), sizeof(id));
    std::printf("  guest: %s, subprotocol \"%s\", a %zu-byte binary message: its ID, above 1: %s\n",
                state_name(guest.ready_state()), guest.selected_protocol().c_str(), id_bytes.size(),
                id > 1 ? "yes" : "no");
    for (const net::PeerEvent& event : server.take_events()) {
        std::printf("  server: %s the guest\n",
                    event.kind == net::PeerEvent::Kind::Connected && event.peer == id
                        ? "peer_connected"
                        : "something else from");
    }
    const std::string& asked = server.peer(id)->requested_url();
    std::printf("  server: the guest asked for the path %s\n\n",
                asked.substr(asked.find('/', std::strlen("ws://"))).c_str());

    std::printf("Text and bytes, each kept as what it was:\n");
    (void)guest.send_text("hello");
    poll_until(poll_both, [&] { return server.available_packet_count() == 1; });
    const int hello_from = server.packet_peer();
    const std::string hello = text_of(server.get_packet().value());
    std::printf("  server: \"%s\" from %s\n", hello.c_str(),
                hello_from == id ? "the guest" : "someone else");
    (void)server.peer(id)->send_text("welcome");
    (void)server.peer(id)->put_packet(bytes_of("\x01\x02\x03"));
    poll_until(poll_both, [&] { return guest.available_packet_count() == 2; });
    while (guest.available_packet_count() > 0) {
        const auto packet = guest.get_packet().value();
        const bool text = guest.was_string_packet();
        std::printf("  guest: %zu bytes, as %s\n", packet.size(), text ? "text" : "binary");
    }
    std::printf("\n");

    std::printf("The guest closes with a code and reason:\n");
    guest.close(4000, "see you");
    std::printf("  guest: %s\n", state_name(guest.ready_state()));
    poll_until(poll_both, [&] { return guest.ready_state() == net::WebSocketState::Closed; });
    poll_until(poll_both, [&] { return server.peer(id) == nullptr; });
    const char* closed = state_name(guest.ready_state());
    std::printf("  guest: %s, code %d, reason \"%s\"\n", closed, guest.close_code(),
                guest.close_reason().c_str());
    for (const net::PeerEvent& event : server.take_events()) {
        std::printf("  server: %s the guest\n",
                    event.kind == net::PeerEvent::Kind::Disconnected && event.peer == id
                        ? "peer_disconnected"
                        : "something else from");
    }
    server.close();

    std::printf("\nThree worlds play over WebSocket; alice's call reaches bob through the "
                "server:\n");
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    const std::vector<std::string> names = {"the server", "alice", "bob"};
    std::vector<std::unique_ptr<world::World>> worlds;
    std::vector<std::string> heard;
    for (const std::string& name : names) {
        auto& w = *worlds.emplace_back(std::make_unique<world::World>(components));
        (void)w.create("table");
        net::install(w);
        net::multiplayer(w).add_rpc(
            "say", {.mode = net::RpcMode::AnyPeer},
            [&heard, &name](world::World& from, entt::entity, const json::Value& args) {
                const int sender = net::multiplayer(from).remote_sender_id();
                heard.push_back("  " + name + " heard \"" + args.as_string() + "\" from " +
                                (sender == 1 ? "the server" : "a client"));
            });
    }
    auto host = std::make_shared<net::WebSocketMultiplayerPeer>();
    if (!host->create_server(0, "127.0.0.1") || !net::multiplayer(*worlds[0]).set_peer(host)) {
        return fail("the game's server didn't start");
    }
    const std::string game_url = "ws://127.0.0.1:" + std::to_string(host->local_port());
    for (std::size_t i = 1; i < worlds.size(); ++i) {
        auto client = std::make_shared<net::WebSocketMultiplayerPeer>();
        if (!client->create_client(game_url) || !net::multiplayer(*worlds[i]).set_peer(client)) {
            return fail("a client didn't start");
        }
    }
    const auto step = [&] {
        for (auto& w : worlds) {
            w->step(1.0 / 60.0);
        }
    };
    if (!poll_until(step, [&] {
            return net::multiplayer(*worlds[1]).peer_ids().size() == 2 &&
                   net::multiplayer(*worlds[2]).peer_ids().size() == 2;
        })) {
        return fail("the clients didn't hear of each other");
    }
    std::printf("  alice and bob each know 2 peers: the server and the other client\n");
    (void)net::multiplayer(*worlds[1]).rpc("table", "say", "hi, bob");
    poll_until(step, [&] { return heard.size() == 2; });
    std::ranges::sort(heard);
    for (const std::string& line : heard) {
        std::printf("%s\n", line.c_str());
    }
    return 0;
}
