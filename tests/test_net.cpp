#include "slash/net/enet.hpp"
#include "slash/net/peer.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

// Polls every peer until done says so, for up to five seconds of wall time (or the time given).
bool poll_until(std::initializer_list<net::Peer*> peers, const std::function<bool()>& done,
                std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
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

std::vector<std::uint8_t> bytes(const std::string& text) {
    return {text.begin(), text.end()};
}

struct Received {
    int from = 0;
    int channel = 0;
    net::TransferMode mode = net::TransferMode::Reliable;
    std::string text;
};

// Takes every waiting packet.
void drain(net::Peer& peer, std::vector<Received>& into) {
    while (peer.available_packet_count() > 0) {
        Received received{peer.packet_peer(), peer.packet_channel(), peer.packet_mode(), {}};
        auto packet = peer.get_packet();
        REQUIRE(packet.ok());
        received.text.assign(packet.value().begin(), packet.value().end());
        into.push_back(received);
    }
}

} // namespace

TEST_CASE("generate_unique_id gives client IDs") {
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t id = net::generate_unique_id();
        CHECK(id >= 2);
        CHECK(id <= 0x7FFFFFFFU);
    }
}

TEST_CASE("an offline peer is a connected server alone") {
    net::OfflinePeer peer;
    CHECK(peer.is_server());
    CHECK(peer.unique_id() == 1);
    CHECK(peer.connection_status() == net::ConnectionStatus::Connected);
    peer.poll();
    CHECK(peer.available_packet_count() == 0);
    CHECK(peer.get_packet().value().empty());
    CHECK(peer.put_packet(bytes("hi")).ok()); // dropped
    CHECK(peer.take_events().empty());
}

TEST_CASE("ENet peers connect, send and disconnect") {
    if (!net::enet_supported()) {
        return;
    }
    net::ENetPeer server;
    REQUIRE(server.create_server(0, 4, 2).ok());
    CHECK(server.is_server());
    CHECK(server.unique_id() == 1);
    CHECK(server.connection_status() == net::ConnectionStatus::Connected);
    const int port = server.local_port();
    REQUIRE(port > 0);

    net::ENetPeer first;
    net::ENetPeer second;
    REQUIRE(first.create_client("127.0.0.1", port, 2).ok());
    REQUIRE(second.create_client("localhost", port, 2).ok());
    CHECK(first.connection_status() == net::ConnectionStatus::Connecting);
    CHECK_FALSE(first.is_server());
    CHECK(first.unique_id() >= 2);
    CHECK(first.unique_id() != second.unique_id());

    std::vector<net::PeerEvent> server_events;
    std::vector<net::PeerEvent> first_events;
    REQUIRE(poll_until({&server, &first, &second}, [&] {
        for (const net::PeerEvent& event : server.take_events()) {
            server_events.push_back(event);
        }
        for (const net::PeerEvent& event : first.take_events()) {
            first_events.push_back(event);
        }
        return server_events.size() == 2 &&
               second.connection_status() == net::ConnectionStatus::Connected;
    }));
    CHECK(first.connection_status() == net::ConnectionStatus::Connected);
    CHECK(first_events == std::vector<net::PeerEvent>{{net::PeerEvent::Kind::Connected, 1}});
    for (const net::PeerEvent& event : server_events) {
        CHECK(event.kind == net::PeerEvent::Kind::Connected);
        CHECK((event.peer == first.unique_id() || event.peer == second.unique_id()));
    }
    CHECK(server.round_trip_time(first.unique_id()) >= 0);
    CHECK(server.round_trip_time(12345) == -1);

    SUBCASE("a client's packets reach the server with their channel and mode") {
        REQUIRE(first.put_packet(bytes("hello")).ok());
        first.set_transfer_mode(net::TransferMode::Unreliable);
        first.set_transfer_channel(2);
        REQUIRE(first.put_packet(bytes("fast")).ok());
        first.set_transfer_mode(net::TransferMode::UnreliableOrdered);
        first.set_transfer_channel(0);
        REQUIRE(first.put_packet(bytes("ordered")).ok());

        std::vector<Received> got;
        REQUIRE(poll_until({&server, &first, &second}, [&] {
            drain(server, got);
            return got.size() == 3;
        }));
        // Channels keep their own order, so look each packet up by its text.
        for (const Received& packet : got) {
            CHECK(packet.from == first.unique_id());
            if (packet.text == "hello") {
                CHECK(packet.channel == 0);
                CHECK(packet.mode == net::TransferMode::Reliable);
            } else if (packet.text == "fast") {
                CHECK(packet.channel == 2);
                CHECK(packet.mode == net::TransferMode::Unreliable);
            } else {
                CHECK(packet.text == "ordered");
                CHECK(packet.channel == 0);
                CHECK(packet.mode == net::TransferMode::UnreliableOrdered);
            }
        }

        // Past the channels the server allows.
        first.set_transfer_channel(5);
        CHECK_FALSE(first.put_packet(bytes("nowhere")).ok());
    }

    SUBCASE("the server sends to one, to all and to all but one") {
        server.set_target_peer(first.unique_id());
        REQUIRE(server.put_packet(bytes("one")).ok());
        server.set_target_peer(net::kTargetPeerBroadcast);
        REQUIRE(server.put_packet(bytes("all")).ok());
        server.set_target_peer(-first.unique_id());
        REQUIRE(server.put_packet(bytes("but")).ok());
        server.set_target_peer(999);
        CHECK(server.put_packet(bytes("nobody")).error().code == ErrorCode::NotFound);

        std::vector<Received> to_first;
        std::vector<Received> to_second;
        REQUIRE(poll_until({&server, &first, &second}, [&] {
            drain(first, to_first);
            drain(second, to_second);
            return to_first.size() == 2 && to_second.size() == 2;
        }));
        CHECK(to_first[0].text == "one");
        CHECK(to_first[1].text == "all");
        CHECK(to_first[0].from == 1);
        CHECK(to_second[0].text == "all");
        CHECK(to_second[1].text == "but");
    }

    SUBCASE("a client that leaves is reported to the server") {
        const int id = first.unique_id();
        first.disconnect_peer(net::kTargetPeerServer);
        std::vector<net::PeerEvent> left;
        REQUIRE(poll_until({&server, &first, &second}, [&] {
            for (const net::PeerEvent& event : server.take_events()) {
                left.push_back(event);
            }
            return !left.empty() &&
                   first.connection_status() == net::ConnectionStatus::Disconnected;
        }));
        CHECK(left == std::vector<net::PeerEvent>{{net::PeerEvent::Kind::Disconnected, id}});
        CHECK(first.take_events() ==
              std::vector<net::PeerEvent>{{net::PeerEvent::Kind::Disconnected, 1}});
        CHECK(first.unique_id() == 0);
    }

    SUBCASE("a server that refuses new connections turns clients away") {
        server.set_refuse_new_connections(true);
        net::ENetPeer late;
        REQUIRE(late.create_client("127.0.0.1", port).ok());
        // ENet completes the handshake before the server sees the client, so the client hears
        // the server connect, then disconnect.
        bool joined = false;
        REQUIRE(poll_until({&server, &first, &second, &late}, [&] {
            for (const net::PeerEvent& event : server.take_events()) {
                joined = joined || event.kind == net::PeerEvent::Kind::Connected;
            }
            return late.connection_status() == net::ConnectionStatus::Disconnected;
        }));
        CHECK_FALSE(joined);
        CHECK(late.unique_id() == 0);
    }

    SUBCASE("closing the server disconnects its clients") {
        server.close();
        CHECK(server.unique_id() == 0);
        CHECK(server.connection_status() == net::ConnectionStatus::Disconnected);
        REQUIRE(poll_until({&first, &second}, [&] {
            return first.connection_status() == net::ConnectionStatus::Disconnected &&
                   second.connection_status() == net::ConnectionStatus::Disconnected;
        }));
        CHECK_FALSE(first.put_packet(bytes("anyone?")).ok());
    }
}

TEST_CASE("ENet peers check their arguments") {
    if (!net::enet_supported()) {
        return;
    }
    net::ENetPeer peer;
    CHECK(peer.create_server(70000).error().code == ErrorCode::InvalidArgument);
    CHECK(peer.create_client("127.0.0.1", 0).error().code == ErrorCode::InvalidArgument);
    CHECK(peer.set_bind_ip("not an address").error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(peer.put_packet(bytes("x")).ok());
    CHECK(peer.unique_id() == 0);
    REQUIRE(peer.set_bind_ip("127.0.0.1").ok());
    REQUIRE(peer.create_server(0).ok());
    CHECK(peer.create_server(0).error().code == ErrorCode::Conflict);
    // A client that isn't connected yet can't send.
    net::ENetPeer client;
    REQUIRE(client.create_client("127.0.0.1", peer.local_port()).ok());
    CHECK_FALSE(client.put_packet(bytes("early")).ok());
}
