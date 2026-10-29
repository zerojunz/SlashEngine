#pragma once

#include "slash/net/peer.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace slash::net {

// Whether this build has ENet: every platform but the web, whose browsers have no UDP sockets.
bool enet_supported();

// A Peer over ENet's reliable UDP, as Godot's ENetMultiplayerPeer: a server that clients join by
// address and port, each client sending to the server, which relays. Channels 0 and 1 of ENet
// carry reliable and unreliable packets of transfer channel 0, as Godot's; transfer channel n
// is ENet channel n + 1. IPv4 only.
class ENetPeer final : public Peer {
public:
    ENetPeer();
    ~ENetPeer() override;

    // Listens on a port (0 picks a free one; local_port() says which) for up to max_clients,
    // with max_channels transfer channels besides the default (0: as many as ENet allows) and
    // bandwidth limits in bytes a second (0: none).
    Result<void> create_server(int port, int max_clients = 32, int max_channels = 0,
                               int in_bandwidth = 0, int out_bandwidth = 0);
    // Connects to a server by IP address or host name; connected once poll() reports peer 1.
    Result<void> create_client(std::string_view address, int port, int channel_count = 0,
                               int in_bandwidth = 0, int out_bandwidth = 0, int local_port = 0);
    // The IPv4 address servers and clients bind to: "*" (any, the default) or an address.
    Result<void> set_bind_ip(std::string_view ip);

    // The port this peer's socket is bound to, 0 when inactive.
    [[nodiscard]] int local_port() const;
    // A connected peer's mean round trip time in milliseconds, -1 if there is no such peer.
    [[nodiscard]] int round_trip_time(int peer) const;

    void set_target_peer(int peer) override;
    [[nodiscard]] int packet_peer() const override;
    [[nodiscard]] TransferMode packet_mode() const override;
    [[nodiscard]] int packet_channel() const override;
    void disconnect_peer(int peer, bool force = false) override;
    [[nodiscard]] bool is_server() const override;
    [[nodiscard]] bool is_server_relay_supported() const override;
    void poll() override;
    void close() override;
    [[nodiscard]] int unique_id() const override;
    [[nodiscard]] ConnectionStatus connection_status() const override;
    [[nodiscard]] int available_packet_count() const override;
    Result<std::span<const std::uint8_t>> get_packet() override;
    Result<void> put_packet(std::span<const std::uint8_t> packet) override;
    [[nodiscard]] int max_packet_size() const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace slash::net
