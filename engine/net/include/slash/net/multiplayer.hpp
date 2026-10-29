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
#include <utility>
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

// Makes the entity a custom spawn becomes, with the ID given, from the data passed to
// Multiplayer::spawn, on every peer, as a MultiplayerSpawner's spawn_function. The spawner then
// places it under its spawn_path.
using SpawnFunction = std::function<Result<entt::entity>(world::World& world, std::string_view id,
                                                         const json::Value& data)>;
// Runs on a peer that receives a spawn, once the entity is placed and before its synchronizers
// start, as a spawned scene's _enter_tree: the place to set its multiplayer authority.
using EnterFunction = std::function<void(world::World& world, entt::entity entity)>;
// Whether a synchronizer is visible to a peer, as Godot's visibility filters.
using VisibilityFilter = std::function<bool(int peer)>;

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
//
// It also replicates, as Godot's SceneReplicationInterface: each MultiplayerSpawner's authority
// spawns the entities placed under its spawn_path on the peers (spawned and despawned events,
// with the entity's ID and data {"spawner"}), and each MultiplayerSynchronizer's authority sends
// its properties to the peers it is visible to (synchronized and delta_synchronized events, with
// the synchronizer's ID). Only entities with IDs take part; a spawned entity keeps its ID.
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

    // Polls the peer, handles what arrived and replicates. seconds is the world time since the
    // last poll, which replication intervals count, so runs repeat. The multiplayer system calls
    // it at the start of each tick.
    void poll(double seconds = 0.0);

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

    // Spawns a custom entity under a spawner's spawn_path on every peer, made by the spawner's
    // spawn function from data, as MultiplayerSpawner.spawn. Only the spawner's authority may.
    Result<entt::entity> spawn(std::string_view spawner, std::string_view id,
                               const json::Value& data = {});
    void set_spawn_function(std::string_view spawner, SpawnFunction function);
    void set_enter_function(std::string_view spawner, EnterFunction function);

    // A synchronizer's visibility, as Godot's: public_visibility, else the peers set visible
    // here, and every filter must agree.
    void set_visibility_for(std::string_view synchronizer, int peer, bool visible);
    [[nodiscard]] bool visibility_for(std::string_view synchronizer, int peer) const;
    void add_visibility_filter(std::string_view synchronizer, VisibilityFilter filter);
    void clear_visibility_filters(std::string_view synchronizer);
    // Checks visibility again for a peer (0: every peer), as update_visibility.
    void update_visibility(std::string_view synchronizer, int peer = 0);

    // The largest sync packet (unreliable; 1350 bytes) and delta packet (reliable; 65535).
    void set_max_sync_packet_size(int bytes);
    void set_max_delta_packet_size(int bytes);

    // Whether a remote procedure of that name is registered.
    [[nodiscard]] bool has_rpc(std::string_view name) const;
    // The entities spawners spawned that this peer tracks, with each one's spawner, by ID.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> spawned() const;
    // The synchronizers that are replicating, by ID.
    [[nodiscard]] std::vector<std::string> synchronizers() const;

    // After the world's entities were put back from a copy (undo, or a session's failed
    // command), points what it tracks at the entities that now have the same IDs, so they are
    // not despawned and spawned again on the other peers.
    void rebind();

private:
    friend void install(world::World& world);
    struct State;
    std::unique_ptr<State> state_;
};

// Gives a world its multiplayer, offline, the multiplayer system (priority -950, after input)
// that polls it each tick, and the multiplayer_visibility system (priority 955, after physics)
// that updates the visibility of synchronizers whose visibility_update_mode is "physics".
void install(world::World& world);
// The world's multiplayer; install first.
[[nodiscard]] Multiplayer& multiplayer(world::World& world);

} // namespace slash::net
