#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Networking, as Godot's multiplayer: a Peer moves packets between the machines in a game, and
// each transport (ENet, WebSocket) is one kind of Peer.
namespace slash::net {

// How a packet travels, as Godot's MultiplayerPeer::TransferMode.
enum class TransferMode : std::uint8_t {
    Unreliable,        // may be lost or arrive out of order
    UnreliableOrdered, // may be lost; older packets arriving after newer ones are dropped
    Reliable,          // arrives, in order
};

enum class ConnectionStatus : std::uint8_t { Disconnected, Connecting, Connected };

// Targets for Peer::set_target_peer: every peer, or the server. A negative ID sends to every
// peer but that one.
inline constexpr int kTargetPeerBroadcast = 0;
inline constexpr int kTargetPeerServer = 1;

// A peer joining or leaving, as Godot's peer_connected and peer_disconnected signals.
struct PeerEvent {
    enum class Kind : std::uint8_t { Connected, Disconnected };
    Kind kind = Kind::Connected;
    int peer = 0;

    friend bool operator==(const PeerEvent&, const PeerEvent&) = default;
};

// One machine's end of a game's network, as Godot's MultiplayerPeer: a server is peer 1 and each
// client has an ID of its own. poll() takes in what arrived; packets then wait in order, each
// with the peer that sent it, its channel and how it travelled, and peers joining and leaving
// wait in take_events().
class Peer {
public:
    Peer() = default;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    virtual ~Peer() = default;

    // The channel (0 is the default; others keep their own order) and mode later packets use.
    virtual void set_transfer_channel(int channel) { transfer_channel_ = channel; }
    [[nodiscard]] int transfer_channel() const { return transfer_channel_; }
    virtual void set_transfer_mode(TransferMode mode) { transfer_mode_ = mode; }
    [[nodiscard]] TransferMode transfer_mode() const { return transfer_mode_; }

    // Turns away peers that try to join from now on.
    virtual void set_refuse_new_connections(bool refuse) { refuse_connections_ = refuse; }
    [[nodiscard]] bool is_refusing_new_connections() const { return refuse_connections_; }
    // Whether the server can relay packets between clients, as Godot's.
    [[nodiscard]] virtual bool is_server_relay_supported() const { return false; }

    // Where later packets go: a peer's ID, kTargetPeerBroadcast, or minus an ID for all but it.
    virtual void set_target_peer(int peer) = 0;

    // The sender, mode and channel of the next packet get_packet() returns.
    [[nodiscard]] virtual int packet_peer() const = 0;
    [[nodiscard]] virtual TransferMode packet_mode() const = 0;
    [[nodiscard]] virtual int packet_channel() const = 0;

    // Disconnects a peer; with force, at once and without telling it.
    virtual void disconnect_peer(int peer, bool force = false) = 0;

    [[nodiscard]] virtual bool is_server() const = 0;

    // Takes in what arrived and sends what is waiting; call it once a frame or tick.
    virtual void poll() = 0;
    // Disconnects everyone and stops.
    virtual void close() = 0;

    // This peer's ID: 1 for a server, 0 when inactive.
    [[nodiscard]] virtual int unique_id() const = 0;
    [[nodiscard]] virtual ConnectionStatus connection_status() const = 0;

    [[nodiscard]] virtual int available_packet_count() const = 0;
    // The next packet, which stays valid until the next get_packet(), poll() or close().
    virtual Result<std::span<const std::uint8_t>> get_packet() = 0;
    // Sends a packet to the target peer with the transfer mode and channel set.
    virtual Result<void> put_packet(std::span<const std::uint8_t> packet) = 0;
    [[nodiscard]] virtual int max_packet_size() const = 0;

    // Peers that joined or left since the last call, in order.
    std::vector<PeerEvent> take_events();

protected:
    void peer_connected(int peer);
    void peer_disconnected(int peer);

private:
    int transfer_channel_ = 0;
    TransferMode transfer_mode_ = TransferMode::Reliable;
    bool refuse_connections_ = false;
    std::vector<PeerEvent> events_;
};

// A random ID for a client, above 1 and below 2^31, as Godot's generate_unique_id.
std::uint32_t generate_unique_id();

// A peer that is always a connected server with no one else, as Godot's
// OfflineMultiplayerPeer: games run alone with the same code they run online with.
class OfflinePeer final : public Peer {
public:
    void set_target_peer(int /*peer*/) override {}
    [[nodiscard]] int packet_peer() const override { return 0; }
    [[nodiscard]] TransferMode packet_mode() const override { return TransferMode::Reliable; }
    [[nodiscard]] int packet_channel() const override { return 0; }
    void disconnect_peer(int /*peer*/, bool /*force*/) override {}
    [[nodiscard]] bool is_server() const override { return true; }
    void poll() override {}
    void close() override {}
    [[nodiscard]] int unique_id() const override { return kTargetPeerServer; }
    [[nodiscard]] ConnectionStatus connection_status() const override {
        return ConnectionStatus::Connected;
    }
    [[nodiscard]] int available_packet_count() const override { return 0; }
    Result<std::span<const std::uint8_t>> get_packet() override;
    Result<void> put_packet(std::span<const std::uint8_t> packet) override;
    [[nodiscard]] int max_packet_size() const override { return 0; }
};

} // namespace slash::net
