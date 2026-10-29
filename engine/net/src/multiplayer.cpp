// SceneMultiplayer, SceneCacheInterface, SceneRPCInterface and SceneReplicationInterface, ported
// from Godot 4.5's modules/multiplayer (MIT). Nodes become entities and node paths their IDs; an
// RPC's arguments, a spawn's argument and state and a sync's properties are JSON, sent as compact
// JSON text where Godot encodes Variants.
#include "slash/net/multiplayer.hpp"

#include "slash/core/log.hpp"
#include "slash/core/sha256.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <entt/entity/registry.hpp>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace slash::net {

namespace {

// SceneMultiplayer::NetworkCommands, in the low three bits of a packet's first byte.
enum Command : std::uint8_t {
    kRemoteCall = 0,
    kSimplifyPath,
    kConfirmPath,
    kRaw,
    kSpawn,
    kDespawn,
    kSync,
    kSys,
};
constexpr std::uint8_t kCommandMask = 7;

// SceneMultiplayer::SysCommands.
enum SysCommand : std::uint8_t { kSysAuth, kSysAddPeer, kSysDelPeer, kSysRelay };
constexpr int kSysCommandSize = 6; // command, sys command, peer ID

// SceneRPCInterface's meta byte: the command, then the node ID's and name ID's sizes and whether
// there are no arguments.
constexpr int kNodeIdCompressionShift = 4;
constexpr int kNameIdCompressionShift = 6;
constexpr int kNoArgsShift = 7;
constexpr std::uint8_t kNodeIdCompressionFlag =
    (1U << kNodeIdCompressionShift) | (1U << (kNodeIdCompressionShift + 1));
constexpr std::uint8_t kNameIdCompressionFlag = 1U << kNameIdCompressionShift;
constexpr std::uint8_t kNoArgsFlag = 1U << kNoArgsShift;
enum NodeIdCompression : std::uint8_t { kNodeId8, kNodeId16, kNodeId32 };
enum NameIdCompression : std::uint8_t { kNameId8, kNameId16 };

constexpr int kMd5Length = 33; // Godot's 32 hex digits and the terminating zero

using Bytes = std::vector<std::uint8_t>;

void put_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void set_u32(Bytes& out, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[at + i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
    }
}

void put_cstring(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

std::uint16_t get_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8U));
}

std::uint32_t get_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

// A zero-terminated string, or the rest of the bytes when there is no zero.
std::string get_cstring(const std::uint8_t* data, int length) {
    const auto* end = std::find(data, data + length, std::uint8_t{0});
    return {reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(end)};
}

std::uint64_t ticks_msec() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

json::Value peer_data(int peer) {
    return json::Object{{"peer", peer}};
}

} // namespace

struct Multiplayer::State {
    struct PendingPeer {
        bool local = false;
        bool remote = false;
        std::uint64_t time = 0;
    };

    // SceneCacheInterface's NodeCache and PeerInfo, by entity ID.
    struct NodeCache {
        int cache_id = 0;
        std::map<int, int> recv_ids;         // peer, its cache ID
        std::map<int, bool> confirmed_peers; // peer, confirmed
    };
    struct PeerInfo {
        std::map<int, std::string> recv_nodes; // its cache ID, entity ID
        std::set<std::string> sent_nodes;
    };

    struct Rpc {
        RpcConfig config;
        RpcFunction function;
    };
    // An entity's remote procedures, sorted by name, as SceneRPCInterface's RPCConfigCache.
    struct RpcTable {
        std::vector<std::string_view> names;
    };

    explicit State(world::World& w) : world(w), peer(std::make_shared<OfflinePeer>()) {}

    world::World& world;
    std::shared_ptr<Peer> peer;
    ConnectionStatus last_status = ConnectionStatus::Disconnected;
    std::map<int, PendingPeer> pending_peers;
    AuthCallback auth_callback;
    std::uint64_t auth_timeout = 3000;
    std::set<int> connected_peers;
    int remote_sender_id = 0;
    int remote_sender_override = 0;
    bool server_relay = true;
    std::vector<RawPacket> packets;
    std::map<std::string, int, std::less<>> authority;
    std::map<std::string, Rpc, std::less<>> rpcs;

    std::map<std::string, NodeCache, std::less<>> nodes_cache;
    std::map<int, std::string> assigned_ids;
    std::map<int, PeerInfo> peers_info;
    int last_send_cache_id = 1;
    std::size_t events_seen = 0; // of world.events(), for entities destroyed

    void emit(std::string_view kind, json::Value data = {}) {
        world.emit(std::string(kind), {}, std::move(data));
    }

    [[nodiscard]] int unique_id() const { return peer->unique_id(); }

    Result<void> send(std::span<const std::uint8_t> packet) const {
        return peer->put_packet(packet);
    }

    // ------------------------------------------------------------------------------------------
    // SceneMultiplayer

    void update_status() {
        const ConnectionStatus status = peer->connection_status();
        if (last_status != status) {
            if (status == ConnectionStatus::Disconnected) {
                if (last_status == ConnectionStatus::Connecting) {
                    emit("connection_failed");
                } else {
                    emit("server_disconnected");
                }
                clear();
            }
            last_status = status;
        }
    }

    void clear() {
        last_status = ConnectionStatus::Disconnected;
        pending_peers.clear();
        connected_peers.clear();
        packets.clear();
        on_replication_reset();
        clear_cache();
    }

    Result<void> send_command(int to, std::span<const std::uint8_t> packet) {
        if (server_relay && unique_id() != 1 && to != 1 && peer->is_server_relay_supported()) {
            // Send a relay packet.
            Bytes relay{kSys, kSysRelay};
            put_u32(relay, static_cast<std::uint32_t>(to)); // the destination
            relay.insert(relay.end(), packet.begin(), packet.end());
            peer->set_target_peer(1);
            return send(relay);
        }
        if (to > 0) {
            if (!connected_peers.contains(to)) {
                return make_error(ErrorCode::NotFound,
                                  "peer " + std::to_string(to) + " isn't connected");
            }
            peer->set_target_peer(to);
            return send(packet);
        }
        for (const int id : connected_peers) {
            if (to != 0 && id == -to) {
                continue;
            }
            peer->set_target_peer(id);
            (void)send(packet);
        }
        return {};
    }

    void process_sys(int from, const std::uint8_t* packet, int length, TransferMode mode,
                     int channel) {
        if (length < kSysCommandSize) {
            log::error("invalid network packet: too small", {{"peer", from}});
            return;
        }
        const std::uint8_t command = packet[1];
        auto target = static_cast<std::int32_t>(get_u32(&packet[2]));
        const bool relay = server_relay && peer->is_server_relay_supported();
        switch (command) {
        case kSysAddPeer:
            if (!relay || unique_id() == 1 || from != 1) {
                return;
            }
            admit_peer(target); // relayed peers are accepted
            break;
        case kSysDelPeer:
            if (!relay || unique_id() == 1 || from != 1) {
                return;
            }
            del_peer(target);
            break;
        case kSysRelay: {
            if (!relay || length < kSysCommandSize + 1) {
                return;
            }
            const std::uint8_t* inner = packet + kSysCommandSize;
            const int inner_length = length - kSysCommandSize;
            bool should_process = false;
            if (unique_id() == 1) { // the server
                // The target might have left while the packet was on its way.
                if (target > 0 && !connected_peers.contains(target)) {
                    return;
                }
                Bytes out{kSys, kSysRelay};
                put_u32(out, static_cast<std::uint32_t>(from)); // the source
                out.insert(out.end(), inner, inner + inner_length);
                peer->set_transfer_mode(mode);
                peer->set_transfer_channel(channel);
                if (target > 0) {
                    peer->set_target_peer(target);
                    (void)send(out);
                } else {
                    for (const int id : connected_peers) {
                        // Not to the sender, nor the excluded.
                        if (id == from || id == -target) {
                            continue;
                        }
                        peer->set_target_peer(id);
                        (void)send(out);
                    }
                    if (target != -1) {
                        // The server is one of the targets.
                        should_process = true;
                        target = from;
                    }
                }
            } else {
                if (from != 1) {
                    return;
                }
                should_process = true;
            }
            if (should_process) {
                remote_sender_id = target;
                process_packet(target, inner, inner_length);
                remote_sender_id = 0;
            }
        } break;
        default:
            log::error("invalid network system command", {{"peer", from}});
            break;
        }
    }

    void process_packet(int from, const std::uint8_t* packet, int length) {
        if (length < 1) {
            log::error("invalid network packet: too small", {{"peer", from}});
            return;
        }
        switch (packet[0] & kCommandMask) {
        case kSimplifyPath:
            process_simplify_path(from, packet, length);
            break;
        case kConfirmPath:
            process_confirm_path(from, packet, length);
            break;
        case kRemoteCall:
            process_rpc(from, packet, length);
            break;
        case kRaw:
            process_raw(from, packet, length);
            break;
        case kSpawn:
            process_spawn(from, packet, length);
            break;
        case kDespawn:
            process_despawn(from, packet, length);
            break;
        case kSync:
            process_sync(from, packet, length);
            break;
        default:
            log::error("invalid network command", {{"peer", from}});
            break;
        }
    }

    void process_raw(int from, const std::uint8_t* packet, int length) {
        if (length < 2) {
            log::error("invalid network packet: too small", {{"peer", from}});
            return;
        }
        packets.push_back({from, Bytes(packet + 1, packet + length)});
        emit("peer_packet", json::Object{{"peer", from}, {"size", length - 1}});
    }

    void add_peer(int id) {
        if (auth_callback) {
            pending_peers[id] = PendingPeer{.time = ticks_msec()};
            emit("peer_authenticating", peer_data(id));
            return;
        }
        admit_peer(id);
    }

    void admit_peer(int id) {
        if (server_relay && unique_id() == 1 && peer->is_server_relay_supported()) {
            // Tell the others about the new peer, and the new peer about the others.
            peer->set_transfer_channel(0);
            peer->set_transfer_mode(TransferMode::Reliable);
            for (const int other : connected_peers) {
                Bytes message{kSys, kSysAddPeer};
                put_u32(message, static_cast<std::uint32_t>(id));
                peer->set_target_peer(other);
                (void)send(message);
                message.resize(2);
                put_u32(message, static_cast<std::uint32_t>(other));
                peer->set_target_peer(id);
                (void)send(message);
            }
        }
        connected_peers.insert(id);
        on_cache_peer_change(id, true);
        on_replication_peer_change(id, true);
        if (id == 1) {
            emit("connected_to_server");
        }
        emit("peer_connected", peer_data(id));
    }

    void del_peer(int id, bool report = true) {
        if (pending_peers.erase(id) != 0) {
            if (report) {
                emit("peer_authentication_failed", peer_data(id));
            }
            return;
        }
        if (!connected_peers.contains(id)) {
            return;
        }
        if (server_relay && unique_id() == 1 && peer->is_server_relay_supported()) {
            // Tell the others.
            peer->set_transfer_channel(0);
            peer->set_transfer_mode(TransferMode::Reliable);
            Bytes message{kSys, kSysDelPeer};
            put_u32(message, static_cast<std::uint32_t>(id));
            for (const int other : connected_peers) {
                if (other != id) {
                    peer->set_target_peer(other);
                    (void)send(message);
                }
            }
        }
        on_replication_peer_change(id, false);
        on_cache_peer_change(id, false);
        connected_peers.erase(id);
        if (report) {
            emit("peer_disconnected", peer_data(id));
        }
    }

    void take_peer_events() {
        for (const PeerEvent& event : peer->take_events()) {
            if (event.kind == PeerEvent::Kind::Connected) {
                add_peer(event.peer);
            } else {
                del_peer(event.peer);
            }
        }
    }

    void poll(double seconds) {
        clock_usec += static_cast<std::uint64_t>(std::max(seconds, 0.0) * 1'000'000.0);
        forget_destroyed_entities();
        replicate();
        update_status();
        if (last_status == ConnectionStatus::Disconnected) {
            return;
        }
        peer->poll();
        take_peer_events();
        update_status();
        if (last_status != ConnectionStatus::Connected) {
            // Still connecting, or the poll disconnected.
            return;
        }

        while (peer->available_packet_count() > 0) {
            const int sender = peer->packet_peer();
            const int channel = peer->packet_channel();
            const TransferMode mode = peer->packet_mode();
            auto got = peer->get_packet();
            if (!got) {
                log::error("couldn't get a network packet", {{"error", got.error().message}});
                return;
            }
            // Copied: handling it may send packets, which may reuse the peer's buffer.
            const Bytes packet(got.value().begin(), got.value().end());
            const auto length = static_cast<int>(packet.size());

            if (auto pending = pending_peers.find(sender); pending != pending_peers.end()) {
                if (length < 2 || (packet[0] & kCommandMask) != kSys || packet[1] != kSysAuth) {
                    continue;
                }
                if (length > 2) {
                    if (auth_callback) {
                        auth_callback(sender, std::span(packet).subspan(2));
                    }
                } else {
                    // The remote side finished.
                    pending->second.remote = true;
                    if (pending->second.local) {
                        pending_peers.erase(pending);
                        admit_peer(sender);
                    }
                }
                continue; // authentication in progress
            }

            if (!connected_peers.contains(sender)) {
                continue;
            }
            if (length > 0 && (packet[0] & kCommandMask) == kSys) {
                // System messages are handled apart, since they may handle packets themselves.
                if (length > 1 && packet[1] == kSysAuth) {
                    // The peer is admitted already; this confirms it.
                    continue;
                }
                process_sys(sender, packet.data(), length, mode, channel);
            } else {
                remote_sender_id = sender;
                process_packet(sender, packet.data(), length);
                remote_sender_id = 0;
            }

            take_peer_events();
            update_status();
            if (last_status != ConnectionStatus::Connected) {
                // Handling a packet may have disconnected.
                return;
            }
        }
        if (!pending_peers.empty() && auth_timeout != 0) {
            std::vector<int> to_drop;
            const std::uint64_t now = ticks_msec();
            for (const auto& [id, pending] : pending_peers) {
                if (pending.time + auth_timeout <= now) {
                    peer->disconnect_peer(id);
                    to_drop.push_back(id);
                }
            }
            for (const int id : to_drop) {
                pending_peers.erase(id);
                emit("peer_authentication_failed", peer_data(id));
            }
        }
        update_status();
        if (last_status != ConnectionStatus::Connected) {
            return; // signals may have disconnected
        }
        network_process();
    }

    // ------------------------------------------------------------------------------------------
    // SceneCacheInterface

    void clear_cache() {
        peers_info.clear();
        nodes_cache.clear();
        assigned_ids.clear();
        last_send_cache_id = 1;
    }

    // As Godot's tree_exited connection: an entity that is gone leaves the cache.
    void forget_destroyed_entities() {
        for (const world::Event& event : world.events()) {
            if (event.kind == world::kEntityDestroyed) {
                remove_node_cache(event.id);
                authority.erase(event.id);
            }
        }
    }

    void remove_node_cache(std::string_view entity) {
        const auto found = nodes_cache.find(entity);
        if (found == nodes_cache.end()) {
            return;
        }
        if (found->second.cache_id != 0) {
            assigned_ids.erase(found->second.cache_id);
        }
        for (const auto& [id, confirmed] : found->second.confirmed_peers) {
            if (auto info = peers_info.find(id); info != peers_info.end()) {
                info->second.sent_nodes.erase(found->first);
            }
        }
        nodes_cache.erase(found);
    }

    void on_cache_peer_change(int id, bool connected) {
        if (connected) {
            peers_info.emplace(id, PeerInfo{});
            return;
        }
        const auto info = peers_info.find(id);
        if (info == peers_info.end()) {
            return;
        }
        for (const auto& [cache_id, entity] : info->second.recv_nodes) {
            if (auto cache = nodes_cache.find(entity); cache != nodes_cache.end()) {
                cache->second.recv_ids.erase(id);
            }
        }
        for (const std::string& entity : info->second.sent_nodes) {
            if (auto cache = nodes_cache.find(entity); cache != nodes_cache.end()) {
                cache->second.confirmed_peers.erase(id);
            }
        }
        peers_info.erase(info);
    }

    void process_simplify_path(int from, const std::uint8_t* packet, int length) {
        const auto info = peers_info.find(from);
        if (info == peers_info.end() || length < 1 + kMd5Length + 4 + 1) {
            log::error("invalid network packet: too small", {{"peer", from}});
            return;
        }
        int offset = 1;
        const std::string methods_md5 = get_cstring(packet + offset, kMd5Length);
        offset += kMd5Length;
        const auto id = static_cast<int>(get_u32(packet + offset));
        offset += 4;
        if (info->second.recv_nodes.contains(id)) {
            log::error("duplicate remote cache ID", {{"peer", from}, {"id", id}});
            return;
        }
        const std::string entity = get_cstring(packet + offset, length - offset);
        if (world.find(entity) == entt::null) {
            log::error("a peer named an entity this world doesn't have",
                       {{"peer", from}, {"entity", entity}});
            return;
        }
        const bool valid_checksum = rpc_md5(entity) == methods_md5;
        if (!valid_checksum) {
            log::error("the entity's remote procedures differ between the peers; register the "
                       "same ones on each",
                       {{"peer", from}, {"entity", entity}});
        }
        info->second.recv_nodes[id] = entity;
        nodes_cache[entity].recv_ids[from] = id;

        // Acknowledge.
        Bytes ack{kConfirmPath, static_cast<std::uint8_t>(valid_checksum ? 1 : 0)};
        put_u32(ack, static_cast<std::uint32_t>(id));
        peer->set_transfer_channel(0);
        peer->set_transfer_mode(TransferMode::Reliable);
        (void)send_command(from, ack);
    }

    void process_confirm_path(int from, const std::uint8_t* packet, int length) {
        if (length != 6) {
            log::error("invalid network packet: wrong size", {{"peer", from}});
            return;
        }
        const bool valid_checksum = packet[1] != 0;
        const auto id = static_cast<int>(get_u32(&packet[2]));
        const auto assigned = assigned_ids.find(id);
        if (assigned == assigned_ids.end()) {
            return; // it may have been destroyed
        }
        if (!valid_checksum) {
            log::error("the entity's remote procedures differ between the peers; register the "
                       "same ones on each",
                       {{"peer", from}, {"entity", assigned->second}});
        }
        const auto cache = nodes_cache.find(assigned->second);
        if (cache == nodes_cache.end()) {
            return;
        }
        const auto confirmed = cache->second.confirmed_peers.find(from);
        if (confirmed == cache->second.confirmed_peers.end()) {
            log::error("a peer confirmed an entity that wasn't sent to it", {{"peer", from}});
            return;
        }
        confirmed->second = true;
    }

    Result<void> send_confirm_path(const std::string& entity, NodeCache& cache,
                                   const std::vector<int>& peers) {
        Bytes packet{kSimplifyPath};
        put_cstring(packet, rpc_md5(entity));
        put_u32(packet, static_cast<std::uint32_t>(cache.cache_id));
        put_cstring(packet, entity);
        for (const int id : peers) {
            peer->set_transfer_channel(0);
            peer->set_transfer_mode(TransferMode::Reliable);
            if (auto sent = send_command(id, packet); !sent) {
                return sent;
            }
            // Recorded as sent but not yet confirmed.
            cache.confirmed_peers[id] = false;
            if (auto info = peers_info.find(id); info != peers_info.end()) {
                info->second.sent_nodes.insert(entity);
            }
        }
        return {};
    }

    [[nodiscard]] bool is_cache_confirmed(std::string_view entity, int id) const {
        const auto cache = nodes_cache.find(entity);
        if (cache == nodes_cache.end()) {
            return false;
        }
        const auto confirmed = cache->second.confirmed_peers.find(id);
        return confirmed != cache->second.confirmed_peers.end() && confirmed->second;
    }

    // Whether every target peer has the entity's cache ID; sends it to those that lack it.
    bool send_object_cache(const std::string& entity, int to, int& cache_id) {
        NodeCache& cache = nodes_cache[entity];
        if (cache.cache_id == 0) {
            cache.cache_id = last_send_cache_id++;
            assigned_ids[cache.cache_id] = entity;
        }
        cache_id = cache.cache_id;

        bool has_all_peers = true;
        std::vector<int> peers_to_add;
        const auto check = [&](int id) {
            const auto confirmed = cache.confirmed_peers.find(id);
            if (confirmed == cache.confirmed_peers.end()) {
                peers_to_add.push_back(id);
                has_all_peers = false;
            } else if (!confirmed->second) {
                has_all_peers = false;
            }
        };
        if (to > 0) {
            if (!peers_info.contains(to)) {
                return false;
            }
            check(to);
        } else {
            for (const auto& [id, info] : peers_info) {
                if (to < 0 && id == -to) {
                    continue;
                }
                check(id);
            }
        }
        if (!peers_to_add.empty()) {
            (void)send_confirm_path(entity, cache, peers_to_add);
        }
        return has_all_peers;
    }

    // The entity a peer's cache ID names, or entt::null.
    [[nodiscard]] entt::entity cached_entity(int from, std::uint32_t cache_id) const {
        const auto info = peers_info.find(from);
        if (info == peers_info.end()) {
            return entt::null;
        }
        const auto found = info->second.recv_nodes.find(static_cast<int>(cache_id));
        if (found == info->second.recv_nodes.end()) {
            log::error("a peer named a cache ID it never sent",
                       {{"peer", from}, {"id", static_cast<std::int64_t>(cache_id)}});
            return entt::null;
        }
        return world.find(found->second);
    }

    // ------------------------------------------------------------------------------------------
    // SceneRPCInterface

    [[nodiscard]] bool has_component(entt::entity entity, std::string_view component) const {
        if (component.empty()) {
            return true;
        }
        const world::ComponentType* type = world.components().find(component);
        return type != nullptr && type->has(world, entity);
    }

    // The remote procedures an entity has, in name order; an RPC's ID is its index.
    [[nodiscard]] std::vector<std::string_view> rpc_names(std::string_view entity_id) const {
        std::vector<std::string_view> names;
        const entt::entity entity = world.find(entity_id);
        for (const auto& [name, rpc] : rpcs) {
            if (entity != entt::null && has_component(entity, rpc.config.component)) {
                names.push_back(name);
            }
        }
        return names;
    }

    // What Godot's get_rpc_md5 checks: a hash of the entity's procedures' names, 32 digits.
    [[nodiscard]] std::string rpc_md5(std::string_view entity) const {
        std::string list;
        for (const std::string_view name : rpc_names(entity)) {
            list += name;
        }
        return sha256_hex(list).substr(0, kMd5Length - 1);
    }

    void process_rpc(int from, const std::uint8_t* packet, int length) {
        int min_size = 1;
        int name_id_offset = 1;
        const int node_id_compression =
            (packet[0] & kNodeIdCompressionFlag) >> kNodeIdCompressionShift;
        const int name_id_compression =
            (packet[0] & kNameIdCompressionFlag) >> kNameIdCompressionShift;
        switch (node_id_compression) {
        case kNodeId8:
            min_size += 1;
            name_id_offset += 1;
            break;
        case kNodeId16:
            min_size += 2;
            name_id_offset += 2;
            break;
        case kNodeId32:
            min_size += 4;
            name_id_offset += 4;
            break;
        default:
            log::error("invalid remote call packet", {{"peer", from}});
            return;
        }
        min_size += name_id_compression == kNameId8 ? 1 : 2;
        if (length < min_size) {
            log::error("invalid network packet: too small", {{"peer", from}});
            return;
        }

        std::uint32_t node_target = 0;
        if (node_id_compression == kNodeId8) {
            node_target = packet[1];
        } else if (node_id_compression == kNodeId16) {
            node_target = get_u16(packet + 1);
        } else {
            node_target = get_u32(packet + 1);
        }

        entt::entity entity = entt::null;
        int packet_length = length;
        if ((node_target & 0x80000000U) != 0U) {
            // The full ID, not cached yet, after the arguments.
            const auto offset = static_cast<int>(node_target & 0x7FFFFFFFU);
            if (offset >= length) {
                log::error("invalid network packet: shorter than it says", {{"peer", from}});
                return;
            }
            const std::string id = get_cstring(packet + offset, length - offset);
            entity = world.find(id);
            if (entity == entt::null) {
                log::error("a remote call named an entity this world doesn't have",
                           {{"peer", from}, {"entity", id}});
                return;
            }
            packet_length = offset;
        } else {
            entity = cached_entity(from, node_target);
        }
        if (entity == entt::null) {
            log::error("a remote call's entity wasn't found", {{"peer", from}});
            return;
        }

        const std::uint16_t name_id = name_id_compression == kNameId8
                                          ? packet[name_id_offset]
                                          : get_u16(packet + name_id_offset);
        const std::string entity_id(world.id_of(entity));
        const std::vector<std::string_view> names = rpc_names(entity_id);
        if (name_id >= names.size()) {
            log::error("a remote call named a procedure the entity doesn't have",
                       {{"peer", from}, {"entity", entity_id}});
            return;
        }
        const Rpc& rpc = rpcs.find(names[name_id])->second;

        bool can_call = false;
        switch (rpc.config.mode) {
        case RpcMode::Disabled:
            break;
        case RpcMode::AnyPeer:
            can_call = true;
            break;
        case RpcMode::Authority:
            can_call = from == authority_of(entity_id);
            break;
        }
        if (!can_call) {
            log::error("a peer called a remote procedure it may not",
                       {{"peer", from},
                        {"entity", entity_id},
                        {"rpc", std::string(names[name_id])},
                        {"authority", authority_of(entity_id)}});
            return;
        }

        json::Value args;
        if ((packet[0] & kNoArgsFlag) == 0) {
            const std::string text(reinterpret_cast<const char*>(packet + min_size),
                                   static_cast<std::size_t>(packet_length - min_size));
            auto parsed = json::parse(text);
            if (!parsed) {
                log::error("a remote call's arguments aren't JSON", {{"peer", from}});
                return;
            }
            args = std::move(parsed).value();
        }
        rpc.function(world, entity, args);
    }

    Result<void> send_rpc(const std::string& entity, int to, std::uint16_t rpc_id,
                          const RpcConfig& config, const json::Value& args) {
        if (peer->connection_status() != ConnectionStatus::Connected) {
            return make_error(ErrorCode::InvalidArgument,
                              "can't call a remote procedure until the peer is connected");
        }
        if (to != 0 && !connected_peers.contains(std::abs(to))) {
            if (to == unique_id()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a remote call to this peer itself; set call_local");
            }
            return make_error(ErrorCode::NotFound,
                              "remote call to unknown peer " + std::to_string(to));
        }

        // See whether every peer has the entity's cache ID (then the call is short) while
        // listing the targets.
        std::vector<int> targets;
        int cache_id = -1;
        bool has_all_peers = true;
        if (to > 0) {
            if (!is_rpc_visible(entity, to)) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a remote call to peer " + std::to_string(to) +
                                      ", which can't see \"" + entity + "\"");
            }
            targets.push_back(to);
            has_all_peers = send_object_cache(entity, to, cache_id);
        } else {
            const bool restricted = !is_rpc_visible(entity, 0);
            for (const int id : connected_peers) {
                if (to < 0 && id == -to) {
                    continue; // the excluded peer
                }
                if (restricted && !is_rpc_visible(entity, id)) {
                    continue; // it can't see the entity
                }
                targets.push_back(id);
                has_all_peers = send_object_cache(entity, id, cache_id) && has_all_peers;
            }
        }
        if (targets.empty()) {
            return {};
        }

        Bytes packet{0}; // the meta, set at the end
        std::uint8_t node_id_compression = kNodeId32;
        if (has_all_peers && cache_id >= 0 && cache_id <= 255) {
            node_id_compression = kNodeId8;
            packet.push_back(static_cast<std::uint8_t>(cache_id));
        } else if (has_all_peers && cache_id >= 0 && cache_id <= 65535) {
            node_id_compression = kNodeId16;
            put_u16(packet, static_cast<std::uint16_t>(cache_id));
        } else {
            // Not every target knows it yet, so it takes 32 bits.
            put_u32(packet, static_cast<std::uint32_t>(cache_id));
        }
        std::uint8_t name_id_compression = kNameId8;
        if (rpc_id <= 255) {
            packet.push_back(static_cast<std::uint8_t>(rpc_id));
        } else {
            name_id_compression = kNameId16;
            put_u16(packet, rpc_id);
        }
        const bool no_args = args.is_null();
        if (!no_args) {
            const std::string text = json::write(args, json::Style::Compact);
            packet.insert(packet.end(), text.begin(), text.end());
        }
        packet[0] = static_cast<std::uint8_t>(
            static_cast<unsigned>(kRemoteCall) |
            (static_cast<unsigned>(node_id_compression) << kNodeIdCompressionShift) |
            (static_cast<unsigned>(name_id_compression) << kNameIdCompressionShift) |
            (no_args ? static_cast<unsigned>(kNoArgsFlag) : 0U));

        peer->set_transfer_channel(config.channel);
        peer->set_transfer_mode(config.transfer_mode);
        if (has_all_peers) {
            for (const int id : targets) {
                (void)send_command(id, packet);
            }
            return {};
        }
        // Not every target confirmed the ID: send the full ID after the arguments to those that
        // didn't.
        const std::size_t offset = packet.size();
        Bytes with_id = packet;
        put_cstring(with_id, entity);
        for (const int id : targets) {
            if (is_cache_confirmed(entity, id)) {
                set_u32(packet, 1, static_cast<std::uint32_t>(cache_id));
                (void)send_command(id, packet);
            } else {
                set_u32(with_id, 1, 0x80000000U | static_cast<std::uint32_t>(offset));
                (void)send_command(id, with_id);
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------------------------
    // SceneReplicationInterface, with MultiplayerSpawner's and MultiplayerSynchronizer's state.
    // Godot connects to tree signals; here each poll compares the world with what it tracked.

    static constexpr std::uint8_t kInvalidSceneId = 0xFF; // a custom spawn
    static constexpr unsigned kCmdFlag0Shift = 3;         // SceneMultiplayer::CMD_FLAG_0_SHIFT
    static constexpr std::uint32_t kPathNetIdFlag = 0x80000000U;

    // A tracked entity, as TrackedNode.
    struct TrackedNode {
        entt::entity handle{entt::null};
        std::uint32_t net_id = 0;
        int remote_peer = 0;
        std::string spawner;                 // the spawner that spawned it, or ""
        std::set<std::string> synchronizers; // started on it, by ID
    };

    // What a spawner tracks of what it spawned, as SpawnInfo.
    struct SpawnInfo {
        json::Value args;
        int scene_id = kInvalidSceneId;
    };
    struct SpawnerState {
        std::map<std::string, SpawnInfo, std::less<>> tracked;
        SpawnFunction spawn_function;
        EnterFunction enter_function;
    };

    struct Watcher {
        std::string prop;
        json::Value value;
        std::uint64_t last_change_usec = 0;
    };
    // A started synchronizer's state, as MultiplayerSynchronizer's.
    struct SyncState {
        entt::entity handle{entt::null};
        std::string root; // the entity it replicates
        int authority = 1;
        std::uint32_t net_id = 0;
        std::uint64_t last_sync_usec = 0;
        std::uint16_t last_inbound_sync = 0;
        std::uint64_t last_watch_usec = 0;
        bool sync_started = false;
        std::vector<Watcher> watchers;
        bool public_visibility = true;
    };
    // What code set on a synchronizer, kept while it stops and starts.
    struct SyncSettings {
        std::set<int> peer_visibility;
        std::vector<VisibilityFilter> filters;
    };

    // SceneReplicationInterface's PeerInfo.
    struct ReplicationPeer {
        std::set<std::string> sync_nodes;
        std::set<std::string> spawn_nodes;
        std::map<std::string, std::uint64_t> last_watch_usecs;
        std::map<std::uint32_t, std::string> recv_sync_ids;
        std::map<std::uint32_t, std::string> recv_nodes;
        std::uint16_t last_sent_sync = 0;
    };

    std::map<std::string, TrackedNode, std::less<>> tracked_nodes;
    std::map<std::string, SpawnerState, std::less<>> spawners;
    std::map<std::string, SyncState, std::less<>> sync_nodes;
    std::map<std::string, SyncSettings, std::less<>> sync_settings;
    std::set<std::string, std::less<>> spawned_nodes;
    std::map<int, ReplicationPeer> rep_peers;
    std::uint32_t last_net_id = 0;
    int sync_mtu = 1350;
    int delta_mtu = 65535;
    // World time in microseconds, which replication intervals count; from 1, so 0 is never.
    std::uint64_t clock_usec = 1;

    // The spawn being received, as pending_spawn and its buffer.
    std::string pending_spawn;
    int pending_spawn_remote = 0;
    json::Array pending_state;
    std::size_t pending_state_used = 0;
    std::vector<std::uint32_t> pending_sync_net_ids;

    [[nodiscard]] bool has_authority(std::string_view entity) const {
        return authority_of(entity) == unique_id();
    }

    TrackedNode& track(const std::string& id) {
        TrackedNode& node = tracked_nodes[id];
        node.handle = world.find(id);
        return node;
    }

    void untrack(const std::string& id) {
        const auto found = tracked_nodes.find(id);
        if (found == tracked_nodes.end()) {
            return;
        }
        const std::uint32_t net_id = found->second.net_id;
        const int remote = found->second.remote_peer;
        tracked_nodes.erase(found);
        // If a remote spawned it, it leaves the received entities.
        if (remote != 0) {
            if (auto info = rep_peers.find(remote); info != rep_peers.end()) {
                info->second.recv_nodes.erase(net_id);
            }
        }
        // If this peer spawned or synced it, it leaves every peer it was sent to.
        if (net_id != 0 || remote == 0) {
            for (auto& [peer_id, info] : rep_peers) {
                info.spawn_nodes.erase(id);
            }
        }
    }

    // Destroys the entities a peer spawned here, as _free_remotes.
    void free_remotes(const ReplicationPeer& info) {
        std::vector<std::string> ids;
        for (const auto& [net_id, id] : info.recv_nodes) {
            if (tracked_nodes.contains(id)) {
                ids.push_back(id);
            }
        }
        for (const std::string& id : ids) {
            destroy_spawned(id);
        }
    }

    // Takes an entity out of its parent's children and destroys it, as queue_free.
    void destroy_spawned(std::string_view id) {
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            return;
        }
        auto& registry = world.ecs();
        for (auto [parent, transform] : registry.view<world::Transform2D>().each()) {
            std::erase(transform.children, entity);
        }
        for (auto [parent, transform] : registry.view<world::Transform3D>().each()) {
            std::erase(transform.children, entity);
        }
        world.destroy(entity);
    }

    void on_replication_peer_change(int id, bool connected) {
        if (connected) {
            rep_peers[id] = ReplicationPeer{};
            const std::vector<std::string> spawned_list(spawned_nodes.begin(), spawned_nodes.end());
            for (const std::string& spawned : spawned_list) {
                update_spawn_visibility(id, spawned);
            }
            for (const auto& [sync_id, sync] : sync_nodes) {
                update_sync_visibility(id, sync_id);
            }
            return;
        }
        const auto info = rep_peers.find(id);
        if (info == rep_peers.end()) {
            return;
        }
        const ReplicationPeer gone = std::move(info->second);
        rep_peers.erase(info);
        free_remotes(gone);
    }

    void on_replication_reset() {
        for (const auto& [id, info] : rep_peers) {
            free_remotes(info);
        }
        rep_peers.clear();
        // Tracked entities stay; only their IDs are reset so they can be given again.
        for (auto& [id, node] : tracked_nodes) {
            node.net_id = 0;
            node.remote_peer = 0;
        }
        for (auto& [id, sync] : sync_nodes) {
            reset_sync(sync);
        }
        last_net_id = 0;
    }

    static void reset_sync(SyncState& sync) {
        sync.net_id = 0;
        sync.last_sync_usec = 0;
        sync.last_inbound_sync = 0;
        sync.last_watch_usec = 0;
        sync.sync_started = false;
        sync.watchers.clear();
    }

    // ------------------------------------------------------------------------------------------
    // Properties: "Component/field[/part...]" on an entity, read and written through reflection.

    [[nodiscard]] static std::vector<std::string_view> split_path(std::string_view path) {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t end = std::min(path.find('/', start), path.size());
            parts.push_back(path.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    [[nodiscard]] static std::optional<std::size_t> element_index(std::string_view token) {
        constexpr std::string_view kXyzw = "xyzw";
        constexpr std::string_view kRgba = "rgba";
        if (token.size() == 1) {
            if (const std::size_t at = kXyzw.find(token.front()); at != std::string_view::npos) {
                return at;
            }
            if (const std::size_t at = kRgba.find(token.front()); at != std::string_view::npos) {
                return at;
            }
        }
        std::size_t index = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
        if (error != std::errc{} || end != token.data() + token.size()) {
            return std::nullopt;
        }
        return index;
    }

    // The value a path's parts after the first two name inside a field's value.
    [[nodiscard]] static json::Value* inside(json::Value& value,
                                             std::span<const std::string_view> parts) {
        json::Value* at = &value;
        for (const std::string_view part : parts) {
            if (at->is_object()) {
                at = at->find(part);
            } else if (at->is_array()) {
                const auto index = element_index(part);
                if (!index || *index >= at->as_array().size()) {
                    return nullptr;
                }
                at = &at->as_array()[*index];
            } else {
                return nullptr;
            }
            if (at == nullptr) {
                return nullptr;
            }
        }
        return at;
    }

    Result<json::Value> get_property(entt::entity entity, std::string_view path) const {
        const std::vector<std::string_view> parts = split_path(path);
        const world::ComponentType* type = world.components().find(parts[0]);
        if (type == nullptr || !type->has(world, entity)) {
            return make_error(ErrorCode::NotFound,
                              "no component for property \"" + std::string(path) + "\"");
        }
        json::Value fields = type->get(world, entity);
        json::Value* value = inside(fields, std::span(parts).subspan(1));
        if (value == nullptr) {
            return make_error(ErrorCode::NotFound,
                              "property \"" + std::string(path) + "\" not found");
        }
        return *value;
    }

    Result<void> set_property(entt::entity entity, std::string_view path,
                              const json::Value& value) {
        const std::vector<std::string_view> parts = split_path(path);
        const world::ComponentType* type = world.components().find(parts[0]);
        if (type == nullptr) {
            return make_error(ErrorCode::NotFound,
                              "no component for property \"" + std::string(path) + "\"");
        }
        std::string at;
        if (parts.size() == 1) {
            return type->assign(world, entity, value, at);
        }
        json::Value fields = type->has(world, entity) ? type->get(world, entity) : type->defaults();
        json::Value* field = fields.find(parts[1]);
        if (field == nullptr) {
            return make_error(ErrorCode::NotFound,
                              "property \"" + std::string(path) + "\" not found");
        }
        json::Value* target = inside(*field, std::span(parts).subspan(2));
        if (target == nullptr) {
            return make_error(ErrorCode::NotFound,
                              "property \"" + std::string(path) + "\" not found");
        }
        *target = value;
        return type->assign(world, entity, json::Object{{std::string(parts[1]), *field}}, at);
    }

    // As MultiplayerSynchronizer::get_state and set_state.
    Result<json::Array> get_state(const std::vector<std::string>& props,
                                  std::string_view root) const {
        const entt::entity entity = world.find(root);
        json::Array values;
        for (const std::string& prop : props) {
            auto value = get_property(entity, prop);
            if (!value) {
                return value.error();
            }
            values.push_back(std::move(value).value());
        }
        return values;
    }

    Result<void> set_state(const std::vector<std::string>& props, std::string_view root,
                           const json::Array& values, std::size_t first = 0) {
        const entt::entity entity = world.find(root);
        for (std::size_t i = 0; i < props.size(); ++i) {
            if (auto set = set_property(entity, props[i], values[first + i]); !set) {
                return set;
            }
        }
        return {};
    }

    // SceneReplicationConfig's lists, read from the component each time.
    enum class PropertyList : std::uint8_t { Spawn, Sync, Watch };
    [[nodiscard]] std::vector<std::string> properties(std::string_view sync_id,
                                                      PropertyList list) const {
        std::vector<std::string> out;
        const auto* sync = world.ecs().try_get<world::MultiplayerSynchronizer>(world.find(sync_id));
        if (sync == nullptr) {
            return out;
        }
        for (const world::ReplicationProperty& prop : sync->properties) {
            bool wanted = prop.spawn;
            if (list == PropertyList::Sync) {
                wanted = prop.replication_mode == "always";
            } else if (list == PropertyList::Watch) {
                wanted = prop.replication_mode == "on_change";
            }
            if (wanted) {
                out.push_back(prop.path);
            }
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------
    // Visibility, as MultiplayerSynchronizer's.

    [[nodiscard]] bool is_visible_to(std::string_view sync_id, int peer_id) const {
        const auto settings = sync_settings.find(sync_id);
        if (settings != sync_settings.end()) {
            for (const VisibilityFilter& filter : settings->second.filters) {
                if (!filter(peer_id)) {
                    return false;
                }
            }
        }
        const auto sync = sync_nodes.find(sync_id);
        if (sync != sync_nodes.end() && sync->second.public_visibility) {
            return true;
        }
        return settings != sync_settings.end() &&
               (settings->second.peer_visibility.contains(0) ||
                settings->second.peer_visibility.contains(peer_id));
    }

    // As _visibility_changed.
    void visibility_changed(int peer_id, const std::string& sync_id) {
        const auto sync = sync_nodes.find(sync_id);
        if (sync == sync_nodes.end() || !has_authority(sync_id)) {
            return;
        }
        if (spawned_nodes.contains(sync->second.root) && peer_id != unique_id()) {
            update_spawn_visibility(peer_id, sync->second.root);
        }
        update_sync_visibility(peer_id, sync_id);
    }

    [[nodiscard]] bool is_rpc_visible(std::string_view entity, int peer_id) const {
        const auto tracked = tracked_nodes.find(entity);
        if (tracked == tracked_nodes.end()) {
            return true; // untracked entities are always visible to remote calls
        }
        const TrackedNode& node = tracked->second;
        if (node.synchronizers.empty()) {
            return true; // no synchronizers, no restrictions
        }
        if (node.remote_peer != 0 && peer_id == node.remote_peer) {
            return true; // calls on spawned entities are always visible to the spawner
        }
        if (spawned_nodes.contains(entity)) {
            if (peer_id != 0) {
                const auto info = rep_peers.find(peer_id);
                return info != rep_peers.end() &&
                       info->second.spawn_nodes.contains(std::string(entity));
            }
            return std::ranges::all_of(rep_peers, [&](const auto& entry) {
                return entry.second.spawn_nodes.contains(std::string(entity));
            });
        }
        // Visible when any of its synchronizers is.
        return std::ranges::any_of(node.synchronizers, [&](const std::string& sync_id) {
            return is_visible_to(sync_id, peer_id);
        });
    }

    void update_sync_visibility(int peer_id, const std::string& sync_id) {
        if (!has_authority(sync_id) || peer_id == unique_id()) {
            return;
        }
        const bool visible = is_visible_to(sync_id, peer_id);
        const auto apply = [&](ReplicationPeer& info, bool now) {
            if (now == info.sync_nodes.contains(sync_id)) {
                return;
            }
            if (now) {
                info.sync_nodes.insert(sync_id);
            } else {
                info.sync_nodes.erase(sync_id);
                info.last_watch_usecs.erase(sync_id);
            }
        };
        if (peer_id == 0) {
            for (auto& [id, info] : rep_peers) {
                // It might be visible to this peer in particular.
                apply(info, visible || is_visible_to(sync_id, id));
            }
            return;
        }
        if (auto info = rep_peers.find(peer_id); info != rep_peers.end()) {
            apply(info->second, visible);
        }
    }

    void update_spawn_visibility(int peer_id, const std::string& id) {
        const auto tracked = tracked_nodes.find(id);
        if (tracked == tracked_nodes.end() || !has_authority(tracked->second.spawner)) {
            return;
        }
        bool visible = true;
        for (const std::string& sync_id : tracked->second.synchronizers) {
            if (!has_authority(sync_id)) {
                continue;
            }
            // Spawn visibility is the OR of its synchronizers'.
            if (is_visible_to(sync_id, peer_id)) {
                visible = true;
                break;
            }
            visible = false;
        }
        std::vector<int> to_spawn;
        std::vector<int> to_despawn;
        if (peer_id != 0) {
            const auto info = rep_peers.find(peer_id);
            if (info == rep_peers.end() || visible == info->second.spawn_nodes.contains(id)) {
                return;
            }
            (visible ? to_spawn : to_despawn).push_back(peer_id);
        } else {
            std::vector<int> others;
            others.reserve(rep_peers.size());
            for (const auto& [other, info] : rep_peers) {
                others.push_back(other);
            }
            for (const int other : others) {
                if (visible) {
                    if (!rep_peers[other].spawn_nodes.contains(id)) {
                        to_spawn.push_back(other);
                    }
                } else {
                    update_spawn_visibility(other, id);
                }
            }
        }
        if (!to_spawn.empty()) {
            auto packet = make_spawn_packet(id);
            if (!packet) {
                log::error("couldn't make a spawn packet",
                           {{"entity", id}, {"error", packet.error().message}});
                return;
            }
            const std::string spawner = tracked_nodes[id].spawner;
            for (const int to : to_spawn) {
                int path_id = 0;
                (void)send_object_cache(spawner, to, path_id);
                (void)send_raw(packet.value(), to, true);
                rep_peers[to].spawn_nodes.insert(id);
            }
        }
        if (!to_despawn.empty()) {
            const Bytes packet = make_despawn_packet(id);
            for (const int to : to_despawn) {
                rep_peers[to].spawn_nodes.erase(id);
                (void)send_raw(packet, to, true);
            }
        }
    }

    Result<void> send_raw(std::span<const std::uint8_t> packet, int to, bool reliable) {
        peer->set_transfer_channel(0);
        peer->set_transfer_mode(reliable ? TransferMode::Reliable : TransferMode::Unreliable);
        return send_command(to, packet);
    }

    // ------------------------------------------------------------------------------------------
    // Spawning

    // The cache ID an entity has, made if it has none, without sending it.
    int make_object_cache(const std::string& entity) {
        NodeCache& cache = nodes_cache[entity];
        if (cache.cache_id == 0) {
            cache.cache_id = last_send_cache_id++;
            assigned_ids[cache.cache_id] = entity;
        }
        return cache.cache_id;
    }

    // [command, scene ID, spawner's cache ID, net ID, synchronizers, name length, synchronizers'
    // net IDs, ID, custom argument as size and JSON text, spawn state as one JSON array].
    Result<Bytes> make_spawn_packet(const std::string& id) {
        TrackedNode& node = tracked_nodes[id];
        if (node.net_id == 0) {
            node.net_id = ++last_net_id;
        }
        const SpawnerState& spawner = spawners[node.spawner];
        const auto info = spawner.tracked.find(id);
        const int scene_id =
            info != spawner.tracked.end() ? info->second.scene_id : kInvalidSceneId;
        const bool custom = scene_id == kInvalidSceneId;

        // The spawn state.
        json::Array state;
        std::vector<std::uint32_t> sync_ids;
        for (const std::string& sync_id : node.synchronizers) {
            if (!has_authority(sync_id)) {
                continue;
            }
            const auto sync = sync_nodes.find(sync_id);
            if (sync == sync_nodes.end()) {
                continue;
            }
            auto values = get_state(properties(sync_id, PropertyList::Spawn), sync->second.root);
            if (!values) {
                return values.error();
            }
            for (json::Value& value : values.value()) {
                state.push_back(std::move(value));
            }
            if (sync->second.net_id == 0) {
                sync->second.net_id = ++last_net_id;
            }
            sync_ids.push_back(sync->second.net_id);
        }

        const int path_id = make_object_cache(node.spawner);
        Bytes packet{kSpawn, static_cast<std::uint8_t>(scene_id)};
        put_u32(packet, static_cast<std::uint32_t>(path_id));
        put_u32(packet, node.net_id);
        put_u32(packet, static_cast<std::uint32_t>(sync_ids.size()));
        put_u32(packet, static_cast<std::uint32_t>(id.size() + 1));
        for (const std::uint32_t sync_id : sync_ids) {
            put_u32(packet, sync_id);
        }
        put_cstring(packet, id);
        if (custom) {
            const std::string arg =
                json::write(info != spawner.tracked.end() ? info->second.args : json::Value{},
                            json::Style::Compact);
            put_u32(packet, static_cast<std::uint32_t>(arg.size()));
            packet.insert(packet.end(), arg.begin(), arg.end());
        }
        if (!state.empty()) {
            const std::string text =
                json::write(json::Value(std::move(state)), json::Style::Compact);
            packet.insert(packet.end(), text.begin(), text.end());
        }
        return packet;
    }

    Bytes make_despawn_packet(const std::string& id) {
        Bytes packet{kDespawn};
        put_u32(packet, tracked_nodes[id].net_id);
        return packet;
    }

    // A spawner starts tracking an entity, as MultiplayerSpawner::_track then on_spawn.
    void on_spawn(const std::string& spawner_id, const std::string& id, json::Value args,
                  int scene_id) {
        SpawnerState& spawner = spawners[spawner_id];
        if (spawner.tracked.contains(id)) {
            return;
        }
        spawner.tracked[id] = SpawnInfo{std::move(args), scene_id};
        TrackedNode& node = track(id);
        if (!node.spawner.empty()) {
            log::error("an entity is spawned by two spawners", {{"entity", id}});
            return;
        }
        node.spawner = spawner_id;
        spawned_nodes.insert(id);
        if (has_authority(spawner_id)) {
            update_spawn_visibility(0, id);
        }
    }

    // A spawner stops tracking an entity that went, as _node_exit then on_despawn.
    void on_despawn(const std::string& spawner_id, const std::string& id) {
        if (auto spawner = spawners.find(spawner_id); spawner != spawners.end()) {
            spawner->second.tracked.erase(id);
        }
        const auto node = tracked_nodes.find(id);
        if (node == tracked_nodes.end() || node->second.spawner != spawner_id) {
            return;
        }
        // Despawn it on every peer that knows it.
        const Bytes packet = make_despawn_packet(id);
        for (const auto& [peer_id, info] : rep_peers) {
            if (info.spawn_nodes.contains(id)) {
                (void)send_raw(packet, peer_id, true);
            }
        }
        node->second.spawner.clear();
        spawned_nodes.erase(id);
        for (auto& [peer_id, info] : rep_peers) {
            info.spawn_nodes.erase(id);
        }
    }

    // The children of a spawner's spawn_path, or null when it has none.
    [[nodiscard]] const std::vector<entt::entity>* spawn_children(entt::entity parent) const {
        if (parent == entt::null || !world.valid(parent)) {
            return nullptr;
        }
        if (const auto* transform = world.ecs().try_get<world::Transform2D>(parent)) {
            return &transform->children;
        }
        if (const auto* transform = world.ecs().try_get<world::Transform3D>(parent)) {
            return &transform->children;
        }
        return nullptr;
    }

    void process_spawn(int from, const std::uint8_t* packet, int length) {
        if (length < 18) {
            log::error("invalid spawn packet", {{"peer", from}});
            return;
        }
        int offset = 1;
        const std::uint8_t scene_id = packet[offset];
        offset += 1;
        const std::uint32_t spawner_cache = get_u32(packet + offset);
        offset += 4;
        const entt::entity spawner_entity = cached_entity(from, spawner_cache);
        const auto* spawner = spawner_entity != entt::null
                                  ? world.ecs().try_get<world::MultiplayerSpawner>(spawner_entity)
                                  : nullptr;
        if (spawner == nullptr) {
            log::error("a spawn named a spawner this world doesn't have", {{"peer", from}});
            return;
        }
        const std::string spawner_id(world.id_of(spawner_entity));
        if (from != authority_of(spawner_id)) {
            log::error("a peer spawned through a spawner it isn't the authority of",
                       {{"peer", from}, {"spawner", spawner_id}});
            return;
        }
        const std::uint32_t net_id = get_u32(packet + offset);
        offset += 4;
        const std::uint32_t sync_count = get_u32(packet + offset);
        offset += 4;
        const std::uint32_t name_length = get_u32(packet + offset);
        offset += 4;
        if (static_cast<std::uint64_t>(name_length) + (std::uint64_t{sync_count} * 4) >
            static_cast<std::uint64_t>(length - offset)) {
            log::error("invalid spawn packet size", {{"peer", from}});
            return;
        }
        std::vector<std::uint32_t> sync_ids;
        for (std::uint32_t i = 0; i < sync_count; ++i) {
            sync_ids.push_back(get_u32(packet + offset));
            offset += 4;
        }
        if (name_length < 1) {
            log::error("a spawn has no ID", {{"peer", from}});
            return;
        }
        const std::string id = get_cstring(packet + offset, static_cast<int>(name_length));
        offset += static_cast<int>(name_length);
        if (!world::World::is_valid_id(id) || world.find(id) != entt::null) {
            log::error("a spawn's ID isn't valid or is taken", {{"peer", from}, {"entity", id}});
            return;
        }

        // Check that it can spawn.
        const auto* siblings = spawn_children(spawner->spawn_path);
        if (siblings == nullptr) {
            log::error("a spawner's spawn_path names no entity with a transform",
                       {{"spawner", spawner_id}});
            return;
        }
        SpawnerState& state = spawners[spawner_id];
        if (spawner->spawn_limit > 0 &&
            static_cast<std::size_t>(spawner->spawn_limit) <= state.tracked.size()) {
            log::error("the spawn limit was reached", {{"spawner", spawner_id}});
            return;
        }
        Result<entt::entity> made = make_error(ErrorCode::InvalidArgument, "");
        json::Value args;
        if (scene_id == kInvalidSceneId) {
            // A custom spawn.
            if (length - offset < 4) {
                log::error("invalid spawn packet", {{"peer", from}});
                return;
            }
            const std::uint32_t arg_size = get_u32(packet + offset);
            offset += 4;
            if (arg_size > static_cast<std::uint32_t>(length - offset)) {
                log::error("invalid spawn packet", {{"peer", from}});
                return;
            }
            auto parsed = json::parse(
                std::string_view(reinterpret_cast<const char*>(packet + offset), arg_size));
            if (!parsed) {
                log::error("a spawn's argument isn't JSON", {{"peer", from}});
                return;
            }
            offset += static_cast<int>(arg_size);
            args = std::move(parsed).value();
            if (!state.spawn_function) {
                log::error("a custom spawn needs the spawner's spawn function",
                           {{"spawner", spawner_id}});
                return;
            }
            made = state.spawn_function(world, id, args);
        } else {
            // A scene-based spawn.
            if (scene_id >= spawner->spawnable_scenes.size()) {
                log::error("a spawn named a scene the spawner doesn't have",
                           {{"spawner", spawner_id}, {"scene", int{scene_id}}});
                return;
            }
            const json::Value scene = json::Object{
                {"entities",
                 json::Object{
                     {id, json::Object{{"extends", spawner->spawnable_scenes[scene_id]}}}}},
                {"format", 1}};
            if (auto loaded = world::load_scene(world, scene); !loaded) {
                made = loaded.error();
            } else {
                made = world.find(id);
            }
        }
        if (!made || made.value() == entt::null) {
            log::error("couldn't make a spawned entity",
                       {{"spawner", spawner_id},
                        {"entity", id},
                        {"error", made ? std::string("none made") : made.error().message}});
            return;
        }
        const entt::entity entity = made.value();

        // Add and track it.
        auto info = rep_peers.find(from);
        if (info == rep_peers.end() || info->second.recv_nodes.contains(net_id)) {
            log::error("a spawn's network ID is in use", {{"peer", from}});
            world.destroy(entity);
            return;
        }
        state.tracked[id] = SpawnInfo{std::move(args), scene_id};
        TrackedNode& node = track(id);
        node.spawner = spawner_id;
        node.net_id = net_id;
        node.remote_peer = from;
        info->second.recv_nodes[net_id] = id;

        // The spawn state applies as its synchronizers start, before anything else sees it.
        pending_spawn = id;
        pending_spawn_remote = from;
        pending_state.clear();
        pending_state_used = 0;
        if (offset < length) {
            auto parsed =
                json::parse(std::string_view(reinterpret_cast<const char*>(packet + offset),
                                             static_cast<std::size_t>(length - offset)));
            if (parsed && parsed.value().is_array()) {
                pending_state = std::move(parsed.value().as_array());
            } else {
                log::error("a spawn's state isn't a JSON array", {{"peer", from}});
            }
        }
        pending_sync_net_ids = std::move(sync_ids);

        // Placed under the spawn path, as add_child.
        const entt::entity parent = spawner->spawn_path;
        if (auto* transform = world.ecs().try_get<world::Transform2D>(parent)) {
            transform->children.push_back(entity);
        } else if (auto* transform3 = world.ecs().try_get<world::Transform3D>(parent)) {
            transform3->children.push_back(entity);
        }
        if (state.enter_function) {
            state.enter_function(world, entity);
        }
        start_synchronizers();
        world.emit("spawned", id, json::Object{{"spawner", spawner_id}});

        pending_spawn.clear();
        pending_spawn_remote = 0;
        pending_state.clear();
        if (!pending_sync_net_ids.empty()) {
            pending_sync_net_ids.clear();
            log::error("a spawn named synchronizers the entity doesn't have",
                       {{"peer", from}, {"entity", id}});
        }
    }

    void process_despawn(int from, const std::uint8_t* packet, int length) {
        if (length < 5) {
            log::error("invalid despawn packet", {{"peer", from}});
            return;
        }
        const std::uint32_t net_id = get_u32(packet + 1);
        const auto info = rep_peers.find(from);
        if (info == rep_peers.end()) {
            return;
        }
        const auto received = info->second.recv_nodes.find(net_id);
        if (received == info->second.recv_nodes.end()) {
            log::error("a peer despawned an entity it didn't spawn", {{"peer", from}});
            return;
        }
        const std::string id = received->second;
        info->second.recv_nodes.erase(received);
        const auto node = tracked_nodes.find(id);
        if (node == tracked_nodes.end()) {
            return;
        }
        const std::string spawner = node->second.spawner;
        if (from != authority_of(spawner)) {
            log::error("a peer despawned through a spawner it isn't the authority of",
                       {{"peer", from}, {"spawner", spawner}});
            return;
        }
        destroy_spawned(id);
        forget_gone();
        world.emit("despawned", id, json::Object{{"spawner", spawner}});
    }

    // ------------------------------------------------------------------------------------------
    // Synchronizing

    // As on_replication_start: a synchronizer begins replicating its root.
    void on_replication_start(const std::string& sync_id, SyncState& sync) {
        TrackedNode& node = track(sync.root);
        node.synchronizers.insert(sync_id);
        update_sync_visibility(0, sync_id);

        if (pending_spawn == sync.root && authority_of(sync_id) == pending_spawn_remote) {
            // Take the synchronizer's network ID from the spawn.
            if (pending_sync_net_ids.empty()) {
                log::error("a spawned entity's synchronizer has no network ID; change "
                           "synchronizers' authority only in the spawner's enter function",
                           {{"synchronizer", sync_id}});
                return;
            }
            const std::uint32_t net_id = pending_sync_net_ids.front();
            pending_sync_net_ids.erase(pending_sync_net_ids.begin());
            rep_peers[pending_spawn_remote].recv_sync_ids[net_id] = sync_id;
            sync.net_id = net_id;

            // The spawn state, before anything else sees it.
            const std::vector<std::string> props = properties(sync_id, PropertyList::Spawn);
            if (!props.empty() && pending_state_used + props.size() <= pending_state.size()) {
                if (auto set = set_state(props, sync.root, pending_state, pending_state_used);
                    !set) {
                    log::error("couldn't apply a spawn's state",
                               {{"synchronizer", sync_id}, {"error", set.error().message}});
                }
                pending_state_used += props.size();
            }
        }
    }

    // As on_replication_stop.
    void on_replication_stop(const std::string& sync_id, const SyncState& sync) {
        if (auto node = tracked_nodes.find(sync.root); node != tracked_nodes.end()) {
            node->second.synchronizers.erase(sync_id);
        }
        for (auto& [peer_id, info] : rep_peers) {
            info.sync_nodes.erase(sync_id);
            info.last_watch_usecs.erase(sync_id);
            if (sync.net_id != 0) {
                info.recv_sync_ids.erase(sync.net_id);
            }
        }
    }

    // Starts and stops synchronizers as they come, go, change root or change authority, as
    // MultiplayerSynchronizer's _start and _stop.
    void start_synchronizers() {
        auto& registry = world.ecs();
        std::map<std::string, std::pair<entt::entity, std::string>> current;
        for (auto [entity, sync] : registry.view<const world::MultiplayerSynchronizer>().each()) {
            const std::string_view id = world.id_of(entity);
            const entt::entity root = sync.root_path == entt::null ? entity : sync.root_path;
            const std::string_view root_id = world.valid(root) ? world.id_of(root) : "";
            if (id.empty() || root_id.empty()) {
                continue;
            }
            current.emplace(std::string(id), std::pair{entity, std::string(root_id)});
        }
        // Stop those gone or changed.
        for (auto it = sync_nodes.begin(); it != sync_nodes.end();) {
            const auto now = current.find(it->first);
            if (now == current.end() || now->second.first != it->second.handle ||
                now->second.second != it->second.root ||
                authority_of(it->first) != it->second.authority) {
                on_replication_stop(it->first, it->second);
                it = sync_nodes.erase(it);
            } else {
                ++it;
            }
        }
        // Start the new ones, in ID order on every peer.
        for (const auto& [id, entry] : current) {
            if (sync_nodes.contains(id)) {
                continue;
            }
            SyncState& sync = sync_nodes[id];
            sync.handle = entry.first;
            sync.root = entry.second;
            sync.authority = authority_of(id);
            sync.public_visibility =
                registry.get<world::MultiplayerSynchronizer>(entry.first).public_visibility;
            on_replication_start(id, sync);
        }
    }

    // Spawners' authorities track the entities placed under their spawn paths, as
    // MultiplayerSpawner::_node_added, and despawn those gone.
    void track_spawns() {
        auto& registry = world.ecs();
        for (auto [entity, spawner] : registry.view<const world::MultiplayerSpawner>().each()) {
            const std::string_view id = world.id_of(entity);
            if (id.empty() || spawner.spawnable_scenes.empty() || !has_authority(id)) {
                continue;
            }
            const auto* children = spawn_children(spawner.spawn_path);
            if (children == nullptr) {
                continue;
            }
            const std::string spawner_id(id);
            for (const entt::entity child : *children) {
                const std::string_view child_id = world.valid(child) ? world.id_of(child) : "";
                if (child_id.empty()) {
                    continue;
                }
                if (auto state = spawners.find(spawner_id);
                    state != spawners.end() && state->second.tracked.contains(child_id)) {
                    continue;
                }
                const world::PrefabLink* link = world.prefab_of(child);
                if (link == nullptr) {
                    continue;
                }
                const auto scene = std::ranges::find(spawner.spawnable_scenes, link->path);
                if (scene == spawner.spawnable_scenes.end()) {
                    continue;
                }
                on_spawn(spawner_id, std::string(child_id), {},
                         static_cast<int>(scene - spawner.spawnable_scenes.begin()));
            }
        }
    }

    // Entities, spawners and synchronizers that went since the last poll leave the tracking,
    // despawning on the peers what a spawner spawned, as the tree signals Godot connects.
    void forget_gone() {
        for (auto& [spawner_id, state] : spawners) {
            const entt::entity spawner = world.find(spawner_id);
            std::vector<std::string> ids;
            ids.reserve(state.tracked.size());
            for (const auto& [id, info] : state.tracked) {
                ids.push_back(id);
            }
            for (const std::string& id : ids) {
                const auto node = tracked_nodes.find(id);
                const entt::entity entity = world.find(id);
                if (spawner == entt::null || entity == entt::null ||
                    (node != tracked_nodes.end() && node->second.handle != entity)) {
                    on_despawn(spawner_id, id);
                }
            }
        }
        for (auto it = sync_nodes.begin(); it != sync_nodes.end();) {
            if (world.find(it->first) != it->second.handle ||
                world.find(it->second.root) == entt::null) {
                on_replication_stop(it->first, it->second);
                it = sync_nodes.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<std::string> gone;
        for (const auto& [id, node] : tracked_nodes) {
            if (world.find(id) != node.handle) {
                gone.push_back(id);
            }
        }
        for (const std::string& id : gone) {
            untrack(id);
        }
    }

    // Each synchronizer's public_visibility as the component holds it now.
    void watch_public_visibility() {
        for (auto& [id, sync] : sync_nodes) {
            const auto* component =
                world.ecs().try_get<world::MultiplayerSynchronizer>(sync.handle);
            if (component != nullptr && component->public_visibility != sync.public_visibility) {
                sync.public_visibility = component->public_visibility;
                visibility_changed(0, id);
            }
        }
    }

    // Updates the visibility of synchronizers with filters whose visibility_update_mode is mode.
    void update_filtered_visibility(std::string_view mode) {
        for (const auto& [id, sync] : sync_nodes) {
            const auto settings = sync_settings.find(id);
            if (settings == sync_settings.end() || settings->second.filters.empty()) {
                continue;
            }
            const auto* component =
                world.ecs().try_get<world::MultiplayerSynchronizer>(sync.handle);
            if (component != nullptr && component->visibility_update_mode == mode) {
                visibility_changed(0, id);
            }
        }
    }

    void replicate() {
        forget_gone();
        start_synchronizers();
        watch_public_visibility();
        track_spawns();
        update_filtered_visibility("idle");
    }

    // As update_outbound_sync_time.
    [[nodiscard]] bool update_outbound_sync_time(SyncState& sync, std::uint64_t usec) const {
        if (sync.last_sync_usec == usec) {
            return true; // updated this poll already
        }
        const auto* component = world.ecs().try_get<world::MultiplayerSynchronizer>(sync.handle);
        const auto interval = static_cast<std::uint64_t>(
            (component != nullptr ? component->replication_interval : 0.0) * 1'000'000.0);
        if (usec < sync.last_sync_usec + interval) {
            return false; // too soon
        }
        sync.last_sync_usec = usec;
        return true;
    }

    // As update_inbound_sync_time.
    static bool update_inbound_sync_time(SyncState& sync, std::uint16_t network_time) {
        if (!sync.sync_started) {
            sync.sync_started = true;
        } else if (network_time <= sync.last_inbound_sync &&
                   sync.last_inbound_sync - network_time < 32767) {
            return false;
        }
        sync.last_inbound_sync = network_time;
        return true;
    }

    // As _verify_synchronizer: synchronizers not spawned by a spawner are named by cache ID.
    bool verify_synchronizer(int to, const std::string& sync_id, SyncState& sync) {
        if (sync.net_id == 0 || (sync.net_id & kPathNetIdFlag) != 0U) {
            int path_id = 0;
            const bool verified = send_object_cache(sync_id, to, path_id);
            if (sync.net_id == 0) {
                sync.net_id = static_cast<std::uint32_t>(path_id) | kPathNetIdFlag;
            }
            return verified;
        }
        return true;
    }

    // As _find_synchronizer.
    SyncState* find_synchronizer(int from, std::uint32_t net_id, std::string& id) {
        if ((net_id & kPathNetIdFlag) != 0U) {
            const entt::entity entity = cached_entity(from, net_id & ~kPathNetIdFlag);
            if (entity == entt::null) {
                return nullptr;
            }
            id = std::string(world.id_of(entity));
        } else {
            const auto info = rep_peers.find(from);
            if (info == rep_peers.end()) {
                return nullptr;
            }
            const auto found = info->second.recv_sync_ids.find(net_id);
            if (found == info->second.recv_sync_ids.end()) {
                return nullptr;
            }
            id = found->second;
        }
        const auto sync = sync_nodes.find(id);
        return sync != sync_nodes.end() ? &sync->second : nullptr;
    }

    // As _watch_changes.
    void watch_changes(const std::string& sync_id, SyncState& sync, std::uint64_t usec) const {
        const std::vector<std::string> props = properties(sync_id, PropertyList::Watch);
        sync.watchers.resize(props.size());
        const entt::entity root = world.find(sync.root);
        for (std::size_t i = 0; i < props.size(); ++i) {
            auto value = get_property(root, props[i]);
            if (!value) {
                log::error("a synchronizer's property wasn't found",
                           {{"synchronizer", sync_id}, {"property", props[i]}});
                continue;
            }
            Watcher& watcher = sync.watchers[i];
            if (watcher.prop != props[i] || !(watcher.value == value.value())) {
                watcher.prop = props[i];
                watcher.value = std::move(value).value();
                watcher.last_change_usec = usec;
            }
        }
    }

    // As get_delta_state.
    json::Array delta_state(const std::string& sync_id, SyncState& sync, std::uint64_t usec,
                            std::uint64_t last_usec, std::uint64_t& indexes) const {
        indexes = 0;
        json::Array out;
        if (sync.last_watch_usec != usec) {
            const auto* component =
                world.ecs().try_get<world::MultiplayerSynchronizer>(sync.handle);
            const auto interval = static_cast<std::uint64_t>(
                (component != nullptr ? component->delta_interval : 0.0) * 1'000'000.0);
            if (usec < last_usec + interval) {
                return out; // too soon
            }
            watch_changes(sync_id, sync, usec);
            sync.last_watch_usec = usec;
        }
        for (std::size_t i = 0; i < sync.watchers.size() && i < 64; ++i) {
            if (sync.watchers[i].last_change_usec <= last_usec) {
                continue;
            }
            out.push_back(sync.watchers[i].value);
            indexes |= std::uint64_t{1} << i;
        }
        return out;
    }

    void send_delta(int to, const std::set<std::string>& synchronizers, std::uint64_t usec) {
        const auto flag = static_cast<std::uint8_t>(1U << kCmdFlag0Shift);
        Bytes packet{static_cast<std::uint8_t>(kSync | flag)};
        for (const std::string& sync_id : synchronizers) {
            const auto found = sync_nodes.find(sync_id);
            if (found == sync_nodes.end() || !has_authority(sync_id)) {
                continue;
            }
            SyncState& sync = found->second;
            if (!verify_synchronizer(to, sync_id, sync)) {
                continue;
            }
            ReplicationPeer& info = rep_peers[to];
            const auto last = info.last_watch_usecs.find(sync_id);
            std::uint64_t indexes = 0;
            const json::Array delta =
                delta_state(sync_id, sync, usec,
                            last != info.last_watch_usecs.end() ? last->second : 0, indexes);
            if (delta.empty()) {
                continue;
            }
            const std::string text = json::write(json::Value(delta), json::Style::Compact);
            const auto size = static_cast<int>(text.size());
            if (size > delta_mtu) {
                log::error("a synchronizer's delta is bigger than the largest delta packet",
                           {{"synchronizer", sync_id}, {"size", size}});
                continue;
            }
            if (static_cast<int>(packet.size()) + 4 + 8 + 4 + size > delta_mtu) {
                (void)send_raw(packet, to, true);
                packet.resize(1);
            }
            put_u32(packet, sync.net_id);
            put_u32(packet, static_cast<std::uint32_t>(indexes & 0xFFFFFFFFU));
            put_u32(packet, static_cast<std::uint32_t>(indexes >> 32U));
            put_u32(packet, static_cast<std::uint32_t>(size));
            packet.insert(packet.end(), text.begin(), text.end());
            info.last_watch_usecs[sync_id] = usec;
        }
        if (packet.size() > 1) {
            (void)send_raw(packet, to, true);
        }
    }

    void process_delta(int from, const std::uint8_t* packet, int length) {
        int offset = 1;
        while (offset + 4 + 8 + 4 < length) {
            const std::uint32_t net_id = get_u32(packet + offset);
            offset += 4;
            const std::uint64_t indexes = std::uint64_t{get_u32(packet + offset)} |
                                          (std::uint64_t{get_u32(packet + offset + 4)} << 32U);
            offset += 8;
            const std::uint32_t size = get_u32(packet + offset);
            offset += 4;
            if (size > static_cast<std::uint32_t>(length - offset)) {
                log::error("invalid delta packet", {{"peer", from}});
                return;
            }
            std::string sync_id;
            SyncState* sync = find_synchronizer(from, net_id, sync_id);
            if (sync == nullptr || authority_of(sync_id) != from) {
                offset += static_cast<int>(size);
                log::error("a delta for a synchronizer the peer isn't the authority of",
                           {{"peer", from}});
                continue;
            }
            std::vector<std::string> props;
            const std::vector<std::string> watched = properties(sync_id, PropertyList::Watch);
            for (std::size_t i = 0; i < watched.size() && i < 64; ++i) {
                if ((indexes & (std::uint64_t{1} << i)) != 0U) {
                    props.push_back(watched[i]);
                }
            }
            auto values =
                json::parse(std::string_view(reinterpret_cast<const char*>(packet + offset), size));
            offset += static_cast<int>(size);
            if (props.empty() || !values || !values.value().is_array() ||
                values.value().as_array().size() != props.size()) {
                log::error("a delta doesn't match its synchronizer's properties",
                           {{"peer", from}, {"synchronizer", sync_id}});
                return;
            }
            if (auto set = set_state(props, sync->root, values.value().as_array()); !set) {
                log::error("couldn't apply a delta",
                           {{"synchronizer", sync_id}, {"error", set.error().message}});
                return;
            }
            world.emit("delta_synchronized", sync_id);
        }
    }

    void send_sync(int to, const std::set<std::string>& synchronizers, std::uint16_t sync_time,
                   std::uint64_t usec) {
        Bytes packet{kSync};
        put_u16(packet, sync_time);
        for (const std::string& sync_id : synchronizers) {
            const auto found = sync_nodes.find(sync_id);
            if (found == sync_nodes.end() || !has_authority(sync_id)) {
                continue;
            }
            SyncState& sync = found->second;
            if (!update_outbound_sync_time(sync, usec)) {
                continue; // nothing to sync yet
            }
            if (!verify_synchronizer(to, sync_id, sync)) {
                continue; // its cache ID isn't confirmed yet
            }
            const std::vector<std::string> props = properties(sync_id, PropertyList::Sync);
            if (props.empty()) {
                continue;
            }
            auto values = get_state(props, sync.root);
            if (!values) {
                log::error("couldn't read a synchronizer's state",
                           {{"synchronizer", sync_id}, {"error", values.error().message}});
                continue;
            }
            const std::string text =
                json::write(json::Value(std::move(values).value()), json::Style::Compact);
            const auto size = static_cast<int>(text.size());
            if (size > sync_mtu) {
                log::error("a synchronizer's state is bigger than the largest sync packet",
                           {{"synchronizer", sync_id}, {"size", size}});
                continue;
            }
            if (static_cast<int>(packet.size()) + 4 + 4 + size > sync_mtu) {
                (void)send_raw(packet, to, false);
                packet.resize(3);
            }
            put_u32(packet, sync.net_id);
            put_u32(packet, static_cast<std::uint32_t>(size));
            packet.insert(packet.end(), text.begin(), text.end());
        }
        if (packet.size() > 3) {
            (void)send_raw(packet, to, false);
        }
    }

    void process_sync(int from, const std::uint8_t* packet, int length) {
        if (length < 11) {
            log::error("invalid sync packet", {{"peer", from}});
            return;
        }
        if ((packet[0] & (1U << kCmdFlag0Shift)) != 0U) {
            process_delta(from, packet, length);
            return;
        }
        const std::uint16_t time = get_u16(packet + 1);
        int offset = 3;
        while (offset + 8 < length) {
            const std::uint32_t net_id = get_u32(packet + offset);
            offset += 4;
            const std::uint32_t size = get_u32(packet + offset);
            offset += 4;
            if (size > static_cast<std::uint32_t>(length - offset)) {
                log::error("invalid sync packet", {{"peer", from}});
                return;
            }
            std::string sync_id;
            SyncState* sync = find_synchronizer(from, net_id, sync_id);
            const std::string_view text(reinterpret_cast<const char*>(packet + offset), size);
            offset += static_cast<int>(size);
            if (sync == nullptr) {
                continue; // not received yet
            }
            if (authority_of(sync_id) != from) {
                log::error("sync data from a peer that isn't the synchronizer's authority",
                           {{"peer", from}, {"synchronizer", sync_id}});
                continue;
            }
            if (!update_inbound_sync_time(*sync, time)) {
                continue; // too old
            }
            const std::vector<std::string> props = properties(sync_id, PropertyList::Sync);
            auto values = json::parse(text);
            if (!values || !values.value().is_array() ||
                values.value().as_array().size() != props.size()) {
                log::error("a sync doesn't match its synchronizer's properties",
                           {{"peer", from}, {"synchronizer", sync_id}});
                return;
            }
            if (auto set = set_state(props, sync->root, values.value().as_array()); !set) {
                log::error("couldn't apply a sync",
                           {{"synchronizer", sync_id}, {"error", set.error().message}});
                return;
            }
            world.emit("synchronized", sync_id);
        }
    }

    // As on_network_process.
    void network_process() {
        for (auto& [peer_id, info] : rep_peers) {
            if (info.sync_nodes.empty()) {
                continue;
            }
            const std::set<std::string> to_sync = info.sync_nodes;
            const std::uint16_t sync_time = ++info.last_sent_sync;
            send_sync(peer_id, to_sync, sync_time, clock_usec);
            send_delta(peer_id, to_sync, clock_usec);
        }
    }

    [[nodiscard]] int authority_of(std::string_view entity) const {
        const auto found = authority.find(entity);
        return found != authority.end() ? found->second : 1;
    }
};

Multiplayer::Multiplayer(world::World& world) : state_(std::make_unique<State>(world)) {
    state_->update_status();
}

Multiplayer::~Multiplayer() = default;

Result<void> Multiplayer::set_peer(std::shared_ptr<Peer> peer) {
    State& s = *state_;
    if (!peer) {
        peer = std::make_shared<OfflinePeer>();
    }
    if (peer == s.peer) {
        return {};
    }
    if (peer->connection_status() == ConnectionStatus::Disconnected) {
        return make_error(ErrorCode::InvalidArgument,
                          "the peer must be connecting or connected; create a server or client "
                          "first");
    }
    s.clear();
    s.peer = std::move(peer);
    s.update_status();
    return {};
}

Peer& Multiplayer::peer() const {
    return *state_->peer;
}

void Multiplayer::poll(double seconds) {
    state_->poll(seconds);
}

int Multiplayer::unique_id() const {
    return state_->unique_id();
}

bool Multiplayer::is_server() const {
    return state_->peer->is_server();
}

std::vector<int> Multiplayer::peer_ids() const {
    return {state_->connected_peers.begin(), state_->connected_peers.end()};
}

int Multiplayer::remote_sender_id() const {
    return state_->remote_sender_override != 0 ? state_->remote_sender_override
                                               : state_->remote_sender_id;
}

void Multiplayer::set_server_relay(bool enabled) {
    state_->server_relay = enabled;
}

bool Multiplayer::server_relay() const {
    return state_->server_relay;
}

void Multiplayer::set_refuse_new_connections(bool refuse) {
    state_->peer->set_refuse_new_connections(refuse);
}

void Multiplayer::set_auth_callback(AuthCallback callback) {
    state_->auth_callback = std::move(callback);
}

void Multiplayer::set_auth_timeout(double seconds) {
    state_->auth_timeout = seconds > 0.0 ? static_cast<std::uint64_t>(seconds * 1000.0) : 0;
}

Result<void> Multiplayer::send_auth(int peer, std::span<const std::uint8_t> data) {
    State& s = *state_;
    if (s.peer->connection_status() != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument, "the peer isn't connected");
    }
    const auto pending = s.pending_peers.find(peer);
    if (pending == s.pending_peers.end()) {
        return make_error(ErrorCode::NotFound,
                          "peer " + std::to_string(peer) + " isn't authenticating");
    }
    if (data.empty()) {
        return make_error(ErrorCode::InvalidArgument, "authentication data can't be empty");
    }
    if (pending->second.local || pending->second.remote) {
        return make_error(ErrorCode::Conflict,
                          "the authentication was marked complete; no more data can be sent");
    }
    Bytes packet{kSys, kSysAuth};
    packet.insert(packet.end(), data.begin(), data.end());
    s.peer->set_target_peer(peer);
    s.peer->set_transfer_channel(0);
    s.peer->set_transfer_mode(TransferMode::Reliable);
    return s.send(packet);
}

Result<void> Multiplayer::complete_auth(int peer) {
    State& s = *state_;
    if (s.peer->connection_status() != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument, "the peer isn't connected");
    }
    const auto pending = s.pending_peers.find(peer);
    if (pending == s.pending_peers.end()) {
        return make_error(ErrorCode::NotFound,
                          "peer " + std::to_string(peer) + " isn't authenticating");
    }
    if (pending->second.local) {
        return make_error(ErrorCode::Conflict, "the authentication was already marked complete");
    }
    pending->second.local = true;
    // Tell the other side.
    const Bytes done{kSys, kSysAuth};
    s.peer->set_target_peer(peer);
    s.peer->set_transfer_channel(0);
    s.peer->set_transfer_mode(TransferMode::Reliable);
    auto sent = s.send(done);
    // The other side finished already, so admit it; after the confirmation, as admitting sends.
    if (pending->second.remote) {
        s.pending_peers.erase(pending);
        s.admit_peer(peer);
    }
    return sent;
}

std::vector<int> Multiplayer::authenticating_peer_ids() const {
    std::vector<int> ids;
    for (const auto& [id, pending] : state_->pending_peers) {
        ids.push_back(id);
    }
    return ids;
}

void Multiplayer::disconnect_peer(int peer) {
    State& s = *state_;
    if (s.peer->connection_status() != ConnectionStatus::Connected) {
        return;
    }
    s.del_peer(peer, false);
    s.peer->disconnect_peer(peer);
}

Result<void> Multiplayer::send_bytes(std::span<const std::uint8_t> bytes, int peer,
                                     TransferMode mode, int channel) {
    State& s = *state_;
    if (bytes.empty()) {
        return make_error(ErrorCode::InvalidArgument, "can't send an empty raw packet");
    }
    if (s.peer->connection_status() != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument, "the peer isn't connected");
    }
    Bytes packet{kRaw};
    packet.insert(packet.end(), bytes.begin(), bytes.end());
    s.peer->set_transfer_channel(channel);
    s.peer->set_transfer_mode(mode);
    return s.send_command(peer, packet);
}

std::vector<RawPacket> Multiplayer::take_packets() {
    return std::exchange(state_->packets, {});
}

void Multiplayer::set_authority(std::string_view entity, int peer, bool recursive) {
    State& s = *state_;
    s.authority[std::string(entity)] = peer;
    if (!recursive) {
        return;
    }
    // The entities its instanced scene brought.
    const std::string prefix = std::string(entity) + "/";
    for (const std::string_view id : s.world.ids()) {
        if (id.starts_with(prefix)) {
            s.authority[std::string(id)] = peer;
        }
    }
    // The entities placed under it.
    const entt::entity found = s.world.find(entity);
    if (found == entt::null) {
        return;
    }
    const auto children = [&](const std::vector<entt::entity>& list) {
        for (const entt::entity child : list) {
            const std::string_view id = s.world.id_of(child);
            if (!id.empty()) {
                set_authority(id, peer, true);
            }
        }
    };
    if (const auto* transform = s.world.ecs().try_get<world::Transform2D>(found)) {
        children(transform->children);
    }
    if (const auto* transform = s.world.ecs().try_get<world::Transform3D>(found)) {
        children(transform->children);
    }
}

int Multiplayer::authority(std::string_view entity) const {
    return state_->authority_of(entity);
}

bool Multiplayer::is_authority(std::string_view entity) const {
    return authority(entity) == unique_id();
}

void Multiplayer::add_rpc(std::string name, RpcConfig config, RpcFunction function) {
    state_->rpcs[std::move(name)] = {std::move(config), std::move(function)};
}

Result<void> Multiplayer::rpc(std::string_view entity_id, std::string_view method,
                              const json::Value& args, int peer) {
    State& s = *state_;
    if (s.peer->connection_status() != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument,
                          "can't call a remote procedure until the peer is connected");
    }
    const entt::entity entity = s.world.find(entity_id);
    if (entity == entt::null) {
        return make_error(ErrorCode::NotFound,
                          "no entity \"" + std::string(entity_id) + "\" for a remote call");
    }
    const std::string id(entity_id);
    const std::vector<std::string_view> names = s.rpc_names(id);
    const auto found = std::find(names.begin(), names.end(), method);
    if (found == names.end()) {
        return make_error(ErrorCode::NotFound,
                          "\"" + std::string(method) + "\" isn't a remote procedure of \"" + id +
                              "\"; register it with add_rpc (and give the entity its component)");
    }
    const auto rpc_id = static_cast<std::uint16_t>(found - names.begin());
    const State::Rpc& rpc = s.rpcs.find(method)->second;

    const int caller = s.unique_id();
    if (peer == caller && !rpc.config.call_local) {
        return make_error(ErrorCode::InvalidArgument, "\"" + std::string(method) +
                                                          "\" can't be called on this peer itself; "
                                                          "set call_local");
    }
    const bool call_local =
        rpc.config.call_local && (peer == 0 || peer == caller || (peer < 0 && peer != -caller));
    if (peer != caller) {
        if (auto sent = s.send_rpc(id, peer, rpc_id, rpc.config, args); !sent) {
            return sent;
        }
    }
    if (call_local) {
        s.remote_sender_override = caller;
        rpc.function(s.world, entity, args);
        s.remote_sender_override = 0;
    }
    return {};
}

Result<entt::entity> Multiplayer::spawn(std::string_view spawner_id, std::string_view id,
                                        const json::Value& data) {
    State& s = *state_;
    const entt::entity spawner_entity = s.world.find(spawner_id);
    const auto* spawner = spawner_entity != entt::null
                              ? s.world.ecs().try_get<world::MultiplayerSpawner>(spawner_entity)
                              : nullptr;
    if (spawner == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "no MultiplayerSpawner \"" + std::string(spawner_id) + "\"");
    }
    if (!s.has_authority(spawner_id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "only the spawner's multiplayer authority may spawn");
    }
    const State::SpawnerState& state = s.spawners[std::string(spawner_id)];
    if (spawner->spawn_limit > 0 &&
        static_cast<std::size_t>(spawner->spawn_limit) <= state.tracked.size()) {
        return make_error(ErrorCode::InvalidArgument, "the spawn limit was reached");
    }
    if (!state.spawn_function) {
        return make_error(ErrorCode::InvalidArgument,
                          "a custom spawn needs the spawner's spawn function");
    }
    if (!world::World::is_valid_id(id) || s.world.find(id) != entt::null) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(id) + "\" isn't a valid ID or is taken");
    }
    const auto* children = s.spawn_children(spawner->spawn_path);
    if (children == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "the spawner's spawn_path names no entity with a transform");
    }
    auto made = state.spawn_function(s.world, id, data);
    if (!made) {
        return made;
    }
    const entt::entity entity = made.value();
    // Placed under the spawn path, its synchronizers started, then spawned on the peers.
    const entt::entity parent = spawner->spawn_path;
    if (auto* transform = s.world.ecs().try_get<world::Transform2D>(parent)) {
        transform->children.push_back(entity);
    } else if (auto* transform3 = s.world.ecs().try_get<world::Transform3D>(parent)) {
        transform3->children.push_back(entity);
    }
    s.start_synchronizers();
    s.on_spawn(std::string(spawner_id), std::string(id), data, State::kInvalidSceneId);
    return entity;
}

void Multiplayer::set_spawn_function(std::string_view spawner, SpawnFunction function) {
    state_->spawners[std::string(spawner)].spawn_function = std::move(function);
}

void Multiplayer::set_enter_function(std::string_view spawner, EnterFunction function) {
    state_->spawners[std::string(spawner)].enter_function = std::move(function);
}

void Multiplayer::set_visibility_for(std::string_view synchronizer, int peer, bool visible) {
    State& s = *state_;
    if (peer == 0) {
        // Public visibility is the component's.
        const entt::entity entity = s.world.find(synchronizer);
        if (auto* sync = entity != entt::null
                             ? s.world.ecs().try_get<world::MultiplayerSynchronizer>(entity)
                             : nullptr) {
            sync->public_visibility = visible;
        }
        if (auto started = s.sync_nodes.find(synchronizer); started != s.sync_nodes.end()) {
            if (started->second.public_visibility == visible) {
                return;
            }
            started->second.public_visibility = visible;
        }
    } else {
        std::set<int>& peers = s.sync_settings[std::string(synchronizer)].peer_visibility;
        if (peers.contains(peer) == visible) {
            return;
        }
        if (visible) {
            peers.insert(peer);
        } else {
            peers.erase(peer);
        }
    }
    update_visibility(synchronizer, peer);
}

bool Multiplayer::visibility_for(std::string_view synchronizer, int peer) const {
    const State& s = *state_;
    if (peer == 0) {
        const entt::entity entity = s.world.find(synchronizer);
        const auto* sync = entity != entt::null
                               ? s.world.ecs().try_get<world::MultiplayerSynchronizer>(entity)
                               : nullptr;
        return sync != nullptr && sync->public_visibility;
    }
    const auto settings = s.sync_settings.find(synchronizer);
    return settings != s.sync_settings.end() && settings->second.peer_visibility.contains(peer);
}

void Multiplayer::add_visibility_filter(std::string_view synchronizer, VisibilityFilter filter) {
    state_->sync_settings[std::string(synchronizer)].filters.push_back(std::move(filter));
}

void Multiplayer::clear_visibility_filters(std::string_view synchronizer) {
    state_->sync_settings[std::string(synchronizer)].filters.clear();
}

void Multiplayer::update_visibility(std::string_view synchronizer, int peer) {
    State& s = *state_;
    if (s.sync_nodes.contains(synchronizer)) {
        s.visibility_changed(peer, std::string(synchronizer));
    }
}

void Multiplayer::set_max_sync_packet_size(int bytes) {
    if (bytes < 128) {
        log::error("the largest sync packet must be at least 128 bytes", {{"bytes", bytes}});
        return;
    }
    state_->sync_mtu = bytes;
}

void Multiplayer::set_max_delta_packet_size(int bytes) {
    if (bytes < 128) {
        log::error("the largest delta packet must be at least 128 bytes", {{"bytes", bytes}});
        return;
    }
    state_->delta_mtu = bytes;
}

namespace {

struct Handle {
    std::unique_ptr<Multiplayer> multiplayer;
};

} // namespace

void install(world::World& world) {
    if (world.ecs().ctx().contains<Handle>()) {
        return;
    }
    world.ecs().ctx().emplace<Handle>(Handle{std::make_unique<Multiplayer>(world)});
    world.add_system(
        "multiplayer",
        [](world::World& w, double tick_seconds) { multiplayer(w).poll(tick_seconds); },
        {.doc = "Polls the world's multiplayer peer: peers joining and leaving, raw packets, "
                "remote procedure calls, spawns and synchronizers, as Godot's SceneMultiplayer.",
         .priority = -950});
    world.add_system(
        "multiplayer_visibility",
        [](world::World& w, double /*tick_seconds*/) {
            multiplayer(w).state_->update_filtered_visibility("physics");
        },
        {.doc = "Checks the visibility filters of synchronizers whose visibility_update_mode is "
                "\"physics\", after physics, as Godot's MultiplayerSynchronizer.",
         .priority = 955});
}

Multiplayer& multiplayer(world::World& world) {
    return *world.ecs().ctx().get<Handle>().multiplayer;
}

} // namespace slash::net
