// SceneMultiplayer, SceneCacheInterface and SceneRPCInterface, ported from Godot 4.5's
// modules/multiplayer (MIT). Nodes become entities and node paths their IDs; an RPC's arguments
// are one JSON value, sent as compact JSON text where Godot encodes Variants.
#include "slash/net/multiplayer.hpp"

#include "slash/core/log.hpp"
#include "slash/core/sha256.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <entt/entity/registry.hpp>
#include <map>
#include <set>
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

    void poll() {
        forget_destroyed_entities();
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
            targets.push_back(to);
            has_all_peers = send_object_cache(entity, to, cache_id);
        } else {
            for (const int id : connected_peers) {
                if (to < 0 && id == -to) {
                    continue;
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

void Multiplayer::poll() {
    state_->poll();
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
        "multiplayer", [](world::World& w, double /*tick_seconds*/) { multiplayer(w).poll(); },
        {.doc = "Polls the world's multiplayer peer: peers joining and leaving, raw packets and "
                "remote procedure calls, as Godot's SceneMultiplayer.",
         .priority = -950});
}

Multiplayer& multiplayer(world::World& world) {
    return *world.ecs().ctx().get<Handle>().multiplayer;
}

} // namespace slash::net
