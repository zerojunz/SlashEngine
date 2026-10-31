// A server and two clients on this machine over ENet, as Godot's ENetMultiplayerPeer: the
// clients join, the server sends to one, to all and to all but one, a client sends on its own
// channel unreliably, a third client gets no answer from the full server and a client leaves.
// IDs and ports change every run, so the output names peers by role.
#include "slash/net/enet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

std::map<int, std::string> names; // peer ID to role

std::string name_of(int peer) {
    const auto found = names.find(peer);
    return found != names.end() ? found->second : "an unknown peer";
}

// Polls the peers until done says so, for up to five seconds.
bool poll_until(std::initializer_list<net::Peer*> peers, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (net::Peer* peer : peers) {
            peer->poll();
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void send(net::Peer& from, const std::string& text) {
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    if (auto sent = from.put_packet(bytes); !sent) {
        std::printf("  refused: %s\n", sent.error().message.c_str());
    }
}

const char* mode_name(net::TransferMode mode) {
    switch (mode) {
    case net::TransferMode::Unreliable:
        return "unreliable";
    case net::TransferMode::UnreliableOrdered:
        return "unreliable_ordered";
    case net::TransferMode::Reliable:
        break;
    }
    return "reliable";
}

// Takes every waiting packet into lines; returns how many there were.
int receive(net::Peer& peer, const std::string& who, std::vector<std::string>& lines) {
    int count = 0;
    while (peer.available_packet_count() > 0) {
        const int from = peer.packet_peer();
        const int channel = peer.packet_channel();
        const net::TransferMode mode = peer.packet_mode();
        auto packet = peer.get_packet();
        const std::string text(packet.value().begin(), packet.value().end());
        std::string line = "  ";
        line += who;
        line += " got \"";
        line += text;
        line += "\" from ";
        line += name_of(from);
        line += " on channel ";
        line += std::to_string(channel);
        line += ", ";
        line += mode_name(mode);
        lines.push_back(line);
        ++count;
    }
    return count;
}

void print_lines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        std::printf("%s\n", line.c_str());
    }
}

void print_events(net::Peer& peer, const std::string& who) {
    for (const net::PeerEvent& event : peer.take_events()) {
        std::printf("  %s: %s %s\n", who.c_str(), name_of(event.peer).c_str(),
                    event.kind == net::PeerEvent::Kind::Connected ? "connected" : "disconnected");
    }
}

} // namespace

int main() {
    if (!net::enet_supported()) {
        std::printf("ENet isn't available here\n");
        return 0;
    }

    std::printf("A server on a free port, for up to 2 clients with 1 extra channel:\n");
    net::ENetPeer server;
    if (auto made = server.create_server(0, 2, 1); !made) {
        std::printf("  %s\n", made.error().message.c_str());
        return 1;
    }
    std::printf("  is_server %d, unique_id %d, listening: %d\n", server.is_server() ? 1 : 0,
                server.unique_id(), server.local_port() > 0 ? 1 : 0);
    const int port = server.local_port();
    names[1] = "the server";

    std::printf("\nTwo clients connect to 127.0.0.1; each picks a random ID above 1:\n");
    net::ENetPeer alice;
    net::ENetPeer bob;
    (void)alice.create_client("127.0.0.1", port, 1);
    (void)bob.create_client("127.0.0.1", port, 1);
    names[alice.unique_id()] = "alice";
    names[bob.unique_id()] = "bob";
    std::printf("  alice is %s until the server answers\n",
                alice.connection_status() == net::ConnectionStatus::Connecting ? "connecting"
                                                                               : "?");
    int joined = 0;
    poll_until({&server, &alice, &bob}, [&] {
        for (const net::PeerEvent& event : server.take_events()) {
            joined += event.kind == net::PeerEvent::Kind::Connected ? 1 : 0;
        }
        return joined == 2 && alice.connection_status() == net::ConnectionStatus::Connected &&
               bob.connection_status() == net::ConnectionStatus::Connected;
    });
    std::printf("  the server saw %d clients connect\n", joined);
    print_events(alice, "alice hears");
    print_events(bob, "bob hears");

    std::printf("\nThe server sends to alice, to everyone, then to all but alice:\n");
    server.set_target_peer(alice.unique_id());
    send(server, "welcome, alice");
    server.set_target_peer(net::kTargetPeerBroadcast);
    send(server, "round one");
    server.set_target_peer(-alice.unique_id());
    send(server, "bob, you're up");
    // Each peer's packets arrive in order; the two peers' may interleave, so print them apart.
    std::vector<std::string> to_alice;
    std::vector<std::string> to_bob;
    int got = 0;
    poll_until({&server, &alice, &bob}, [&] {
        got += receive(alice, "alice", to_alice);
        got += receive(bob, "bob", to_bob);
        return got == 4;
    });
    print_lines(to_alice);
    print_lines(to_bob);

    std::printf("\nAlice sends a reliable packet, then a position on channel 1 that may be "
                "lost:\n");
    send(alice, "ready");
    alice.set_transfer_channel(1);
    alice.set_transfer_mode(net::TransferMode::Unreliable);
    send(alice, "x=12 y=40");
    // Channels keep their own order, so the two may arrive either way round.
    std::vector<std::string> to_server;
    got = 0;
    poll_until({&server, &alice, &bob}, [&] {
        got += receive(server, "the server", to_server);
        return got == 2;
    });
    std::sort(to_server.begin(), to_server.end());
    print_lines(to_server);
    std::printf("  channel 2 is past what the server allows:\n");
    alice.set_transfer_channel(2);
    send(alice, "lost");

    std::printf("\nThe server is full, so a third client gets no answer:\n");
    net::ENetPeer carol;
    (void)carol.create_client("127.0.0.1", port);
    names[carol.unique_id()] = "carol";
    const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    poll_until({&server, &alice, &bob, &carol},
               [&] { return std::chrono::steady_clock::now() >= wait_until; });
    std::printf("  carol is %s; ENet gives up on her after its timeout\n",
                carol.connection_status() == net::ConnectionStatus::Connecting ? "still connecting"
                                                                               : "not connecting");
    print_events(server, "the server hears");
    carol.close();

    std::printf("\nBob leaves:\n");
    bob.disconnect_peer(net::kTargetPeerServer);
    bool left = false;
    poll_until({&server, &alice, &bob}, [&] {
        for (const net::PeerEvent& event : server.take_events()) {
            std::printf("  the server hears: %s %s\n", name_of(event.peer).c_str(),
                        event.kind == net::PeerEvent::Kind::Connected ? "connected"
                                                                      : "disconnected");
            left = left || event.kind == net::PeerEvent::Kind::Disconnected;
        }
        return left && bob.connection_status() == net::ConnectionStatus::Disconnected;
    });
    print_events(bob, "bob hears");
    std::printf("  bob's unique_id is now %d\n", bob.unique_id());

    std::printf("\nThe server closes, and alice is disconnected:\n");
    server.close();
    poll_until({&alice},
               [&] { return alice.connection_status() == net::ConnectionStatus::Disconnected; });
    print_events(alice, "alice hears");
    return 0;
}
