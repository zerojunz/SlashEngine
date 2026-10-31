#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/net/peer.hpp"

#include <cstdint>
#include <entt/entity/fwd.hpp>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
} // namespace slash::world

namespace slash::net {

// Who may call a remote procedure, as Godot's MultiplayerAPI::RPCMode.
enum class RpcMode : std::uint8_t {
    Disabled,  // no one
    AnyPeer,   // every peer
    Authority, // only the entity's multiplayer authority (the default)
};

// How a remote procedure is called, as Godot's @rpc annotation.
struct RpcConfig {
    RpcMode mode = RpcMode::Authority;
    bool call_local = false; // also runs on the peer that calls it
    TransferMode transfer_mode = TransferMode::Reliable;
    int channel = 0;
    // The component an entity needs for the call to be one of its own, as a Godot method
    // belongs to a node's script; "" for every entity.
    std::string component{}; // NOLINT(readability-redundant-member-init)
};

// Runs a remote procedure on an entity. Multiplayer::remote_sender_id says who called it.
using RpcFunction =
    std::function<void(world::World& world, entt::entity entity, const json::Value& args)>;

// A raw packet a peer sent with send_bytes, as Godot's peer_packet signal.
struct RawPacket {
    int peer = 0;
    std::vector<std::uint8_t> bytes;
};

// A world's multiplayer, as Godot's SceneMultiplayer: it runs on a Peer, admits the peers that
// join (through authentication when there is a callback), tells clients about each other
// through the server and relays their packets, sends raw packets and calls remote procedures on
// entities, which are named by ID as Godot names nodes by path. Its events are world events,
// with data {"peer": id}: peer_connected, peer_disconnected, connected_to_server,
// connection_failed, server_disconnected, peer_authenticating, peer_authentication_failed and
// peer_packet (data {"peer", "size"}). It starts offline: a server alone.
class Multiplayer {
public:
    explicit Multiplayer(world::World& world);
    ~Multiplayer();
    Multiplayer(const Multiplayer&) = delete;
    Multiplayer& operator=(const Multiplayer&) = delete;

    // The peer to run on, connecting or connected; null goes back offline. Changing it forgets
    // the peers and caches of the one before.
    Result<void> set_peer(std::shared_ptr<Peer> peer);
    [[nodiscard]] Peer& peer() const;

    // Polls the peer and handles what arrived. The multiplayer system calls it at the start of
    // each tick.
    void poll();

    [[nodiscard]] int unique_id() const;
    [[nodiscard]] bool is_server() const;
    // The connected peers' IDs, sorted.
    [[nodiscard]] std::vector<int> peer_ids() const;
    // The peer whose call or packet is being handled (this peer for a local call), else 0.
    [[nodiscard]] int remote_sender_id() const;

    // Whether the server tells clients about each other and relays their packets (true).
    void set_server_relay(bool enabled);
    [[nodiscard]] bool server_relay() const;
    void set_refuse_new_connections(bool refuse);

    // Authentication, as Godot's: with a callback, joining peers wait until both sides call
    // complete_auth, exchanging send_auth's bytes meanwhile, or are dropped after the timeout
    // (seconds; 0 for none).
    using AuthCallback = std::function<void(int peer, std::span<const std::uint8_t> data)>;
    void set_auth_callback(AuthCallback callback);
    void set_auth_timeout(double seconds);
    Result<void> send_auth(int peer, std::span<const std::uint8_t> data);
    Result<void> complete_auth(int peer);
    [[nodiscard]] std::vector<int> authenticating_peer_ids() const;

    // Disconnects a peer without reporting it.
    void disconnect_peer(int peer);

    // Sends bytes to a peer (0: everyone; minus an ID: all but it), as Godot's send_bytes.
    Result<void> send_bytes(std::span<const std::uint8_t> bytes, int peer = kTargetPeerBroadcast,
                            TransferMode mode = TransferMode::Reliable, int channel = 0);
    // The raw packets that arrived since the last call.
    std::vector<RawPacket> take_packets();

    // The peer an entity answers to (1 unless set), as Godot's multiplayer authority. recursive
    // sets the entities placed under it and those its instanced scene brought too.
    void set_authority(std::string_view entity, int peer, bool recursive = true);
    [[nodiscard]] int authority(std::string_view entity) const;
    [[nodiscard]] bool is_authority(std::string_view entity) const;

    // Registers a remote procedure; every peer registers the same ones.
    void add_rpc(std::string name, RpcConfig config, RpcFunction function);
    // Calls a remote procedure on an entity on a peer (0: every peer; minus an ID: all but it),
    // with one JSON value of arguments (null for none), as Godot's rpc and rpc_id.
    Result<void> rpc(std::string_view entity, std::string_view method, const json::Value& args = {},
                     int peer = kTargetPeerBroadcast);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Gives a world its multiplayer, offline, and the multiplayer system (priority -950, after
// input) that polls it each tick.
void install(world::World& world);
// The world's multiplayer; install first.
[[nodiscard]] Multiplayer& multiplayer(world::World& world);

} // namespace slash::net
