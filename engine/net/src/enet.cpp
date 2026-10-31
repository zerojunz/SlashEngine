// ENetMultiplayerPeer and the parts of ENetConnection and ENetPacketPeer it uses, ported from
// Godot 4.5's modules/enet (MIT). Godot's own ENet fork adds IPv6; this one is upstream ENet,
// which Godot also builds against, so it is IPv4 as Godot is then.
#include "slash/net/enet.hpp"

#if SLASH_NET_ENET
#include <enet/enet.h>
#endif

#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>

namespace slash::net {

#if SLASH_NET_ENET

bool enet_supported() {
    return true;
}

namespace {

// The first ENet channels, which carry transfer channel 0 (ENetMultiplayerPeer's SYSCH_*).
constexpr int kChannelReliable = 0;
constexpr int kChannelUnreliable = 1;
constexpr int kSystemChannels = 2;

bool initialize_enet() {
    static const bool initialized = enet_initialize() == 0;
    return initialized;
}

Error not_active() {
    return make_error(ErrorCode::InvalidArgument, "the peer isn't active");
}

void destroy_unused(ENetPacket* packet) {
    if (packet->referenceCount == 0) {
        enet_packet_destroy(packet);
    }
}

} // namespace

struct ENetPeer::State {
    enum class Mode : std::uint8_t { None, Server, Client };

    struct Packet {
        ENetPacket* packet = nullptr;
        int from = 0;
        int channel = 0;
        TransferMode transfer_mode = TransferMode::Reliable;
    };

    Mode mode = Mode::None;
    std::uint32_t unique_id = 0;
    int target_peer = 0;
    ConnectionStatus status = ConnectionStatus::Disconnected;
    ENetHost* host = nullptr;
    ENetAddress bind{ENET_HOST_ANY, 0};
    std::map<int, ::ENetPeer*> peers;         // by ID
    std::unordered_map<::ENetPeer*, int> ids; // the accepted peers' IDs
    std::deque<Packet> incoming;
    Packet current;

    [[nodiscard]] bool active() const { return mode != Mode::None; }

    void store_packet(int source, const ENetEvent& event) {
        Packet packet;
        packet.packet = event.packet;
        packet.channel = event.channelID;
        packet.from = source;
        if ((event.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0U) {
            packet.transfer_mode = TransferMode::Reliable;
        } else if ((event.packet->flags & ENET_PACKET_FLAG_UNSEQUENCED) != 0U) {
            packet.transfer_mode = TransferMode::Unreliable;
        } else {
            packet.transfer_mode = TransferMode::UnreliableOrdered;
        }
        packet.packet->referenceCount++;
        incoming.push_back(packet);
    }

    void pop_current_packet() {
        if (current.packet != nullptr) {
            current.packet->referenceCount--;
            destroy_unused(current.packet);
            current = {};
            current.channel = -1;
        }
    }

    Result<void> create_host(const ENetAddress* address, int max_peers, int max_channels,
                             int in_bandwidth, int out_bandwidth) {
        if (!initialize_enet()) {
            return make_error(ErrorCode::PlatformFailure, "ENet couldn't start");
        }
        host = enet_host_create(
            address, static_cast<std::size_t>(max_peers), static_cast<std::size_t>(max_channels),
            static_cast<enet_uint32>(in_bandwidth), static_cast<enet_uint32>(out_bandwidth));
        if (host == nullptr) {
            return make_error(ErrorCode::PlatformFailure,
                              address != nullptr ? "couldn't create the ENet host; is port " +
                                                       std::to_string(address->port) + " in use?"
                                                 : "couldn't create the ENet host");
        }
        return {};
    }

    void flush() const {
        if (host != nullptr) {
            enet_host_flush(host);
        }
    }

    static void send(::ENetPeer* peer, int channel, ENetPacket* packet) {
        if (enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) < 0) {
            destroy_unused(packet);
        }
    }
};

ENetPeer::ENetPeer() : state_(std::make_unique<State>()) {}

ENetPeer::~ENetPeer() {
    close();
}

Result<void> ENetPeer::create_server(int port, int max_clients, int max_channels, int in_bandwidth,
                                     int out_bandwidth) {
    State& s = *state_;
    if (s.active()) {
        return make_error(ErrorCode::Conflict, "the peer is already active");
    }
    if (port < 0 || port > 65535) {
        return make_error(ErrorCode::InvalidArgument,
                          "the port must be between 0 and 65535 (inclusive)");
    }
    if (max_clients < 1 || max_clients > 4095) {
        return make_error(ErrorCode::InvalidArgument,
                          "max_clients must be between 1 and 4095 (inclusive)");
    }
    set_refuse_new_connections(false);
    ENetAddress address = s.bind;
    address.port = static_cast<enet_uint16>(port);
    if (auto created = s.create_host(&address, max_clients,
                                     max_channels > 0 ? max_channels + kSystemChannels : 0,
                                     in_bandwidth, out_bandwidth);
        !created) {
        return created;
    }
    s.mode = State::Mode::Server;
    s.unique_id = 1;
    s.status = ConnectionStatus::Connected;
    return {};
}

Result<void> ENetPeer::create_client(std::string_view address, int port, int channel_count,
                                     int in_bandwidth, int out_bandwidth, int local_port) {
    State& s = *state_;
    if (s.active()) {
        return make_error(ErrorCode::Conflict, "the peer is already active");
    }
    if (port < 1 || port > 65535) {
        return make_error(ErrorCode::InvalidArgument,
                          "the remote port must be between 1 and 65535 (inclusive)");
    }
    if (local_port < 0 || local_port > 65535) {
        return make_error(ErrorCode::InvalidArgument,
                          "the local port must be between 0 and 65535 (inclusive)");
    }
    set_refuse_new_connections(false);
    if (local_port != 0) {
        ENetAddress bound = s.bind;
        bound.port = static_cast<enet_uint16>(local_port);
        if (auto created = s.create_host(&bound, 1, 0, in_bandwidth, out_bandwidth); !created) {
            return created;
        }
    } else if (auto created = s.create_host(nullptr, 1, 0, in_bandwidth, out_bandwidth); !created) {
        return created;
    }

    ENetAddress remote{};
    const std::string name(address);
    if (enet_address_set_host(&remote, name.c_str()) != 0) {
        enet_host_destroy(s.host);
        s.host = nullptr;
        return make_error(ErrorCode::NotFound,
                          "couldn't resolve the server's address \"" + name + "\"");
    }
    remote.port = static_cast<enet_uint16>(port);

    s.unique_id = generate_unique_id();
    // The client picks its ID and sends it with the connection, as Godot's does.
    ::ENetPeer* server = enet_host_connect(
        s.host, &remote,
        channel_count > 0 ? static_cast<std::size_t>(channel_count + kSystemChannels)
                          : std::size_t{ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT},
        s.unique_id);
    if (server == nullptr) {
        enet_host_destroy(s.host);
        s.host = nullptr;
        s.unique_id = 0;
        return make_error(ErrorCode::PlatformFailure, "couldn't connect to the ENet server");
    }

    // Connected at the connect event.
    s.status = ConnectionStatus::Connecting;
    s.mode = State::Mode::Client;
    s.peers[kTargetPeerServer] = server;
    s.ids[server] = kTargetPeerServer;
    return {};
}

Result<void> ENetPeer::set_bind_ip(std::string_view ip) {
    State& s = *state_;
    if (ip == "*") {
        s.bind.host = ENET_HOST_ANY;
        return {};
    }
    ENetAddress address{};
    const std::string text(ip);
    if (enet_address_set_host_ip(&address, text.c_str()) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid bind IP address \"" + text + "\"; want an IPv4 address or *");
    }
    s.bind.host = address.host;
    return {};
}

int ENetPeer::local_port() const {
    const State& s = *state_;
    if (s.host == nullptr) {
        return 0;
    }
    ENetAddress address{};
    if (enet_socket_get_address(s.host->socket, &address) != 0) {
        return 0;
    }
    return address.port;
}

int ENetPeer::round_trip_time(int peer) const {
    const auto found = state_->peers.find(peer);
    if (found == state_->peers.end() || found->second == nullptr) {
        return -1;
    }
    return static_cast<int>(found->second->roundTripTime);
}

void ENetPeer::set_target_peer(int peer) {
    state_->target_peer = peer;
}

int ENetPeer::packet_peer() const {
    const State& s = *state_;
    if (!s.active() || s.incoming.empty()) {
        return 1;
    }
    return s.incoming.front().from;
}

TransferMode ENetPeer::packet_mode() const {
    const State& s = *state_;
    if (!s.active() || s.incoming.empty()) {
        return TransferMode::Reliable;
    }
    return s.incoming.front().transfer_mode;
}

int ENetPeer::packet_channel() const {
    const State& s = *state_;
    if (!s.active() || s.incoming.empty()) {
        return 1;
    }
    const int channel = s.incoming.front().channel;
    if (channel >= kSystemChannels) { // the first two are channel 0's
        return channel - kSystemChannels + 1;
    }
    return 0;
}

void ENetPeer::disconnect_peer(int peer, bool force) {
    State& s = *state_;
    const auto found = s.peers.find(peer);
    if (!s.active() || found == s.peers.end()) {
        return;
    }
    if (found->second != nullptr) {
        enet_peer_disconnect(found->second, 0); // removed at its disconnect event
    }
    s.flush();
    if (force) {
        s.ids.erase(found->second);
        s.peers.erase(found);
        if (s.mode == State::Mode::Client) {
            close();
        }
    }
}

bool ENetPeer::is_server() const {
    return state_->mode == State::Mode::Server;
}

bool ENetPeer::is_server_relay_supported() const {
    return state_->active();
}

void ENetPeer::poll() {
    State& s = *state_;
    if (!s.active()) {
        return;
    }
    s.pop_current_packet();

    if (s.mode == State::Mode::Client && !s.peers.contains(kTargetPeerServer)) {
        close();
        return;
    }

    ENetEvent event{};
    int ret = enet_host_service(s.host, &event, 0);
    while (s.host != nullptr) {
        if (ret < 0) {
            close(); // an error
            return;
        }
        if (ret > 0) {
            if (s.mode == State::Mode::Client) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    s.status = ConnectionStatus::Connected;
                    peer_connected(kTargetPeerServer);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    if (s.status == ConnectionStatus::Connected) {
                        // The client just left the server.
                        peer_disconnected(kTargetPeerServer);
                    }
                    close();
                    return;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    s.store_packet(kTargetPeerServer, event);
                }
            } else if (event.type == ENET_EVENT_TYPE_CONNECT) {
                const auto id = static_cast<int>(event.data);
                if (is_refusing_new_connections() ||
                    // A client joined with an invalid ID, probably trying to exploit us.
                    event.data < 2 || event.data > 0x7FFFFFFFU || s.peers.contains(id)) {
                    // Godot resets the peer, which upstream ENet doesn't tell the client
                    // about; disconnecting it does, so it stops waiting at once.
                    enet_peer_disconnect_now(event.peer, 0);
                } else {
                    s.peers[id] = event.peer;
                    s.ids[event.peer] = id;
                    peer_connected(id);
                }
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                const auto found = s.ids.find(event.peer);
                if (found != s.ids.end()) { // else it never fully connected
                    const int id = found->second;
                    s.ids.erase(found);
                    const auto entry = s.peers.find(id);
                    if (entry != s.peers.end() && entry->second == event.peer) {
                        s.peers.erase(entry);
                        peer_disconnected(id);
                    }
                }
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                const auto found = s.ids.find(event.peer);
                if (found != s.ids.end()) {
                    s.store_packet(found->second, event);
                } else {
                    enet_packet_destroy(event.packet);
                }
            }
        }
        event = {};
        ret = enet_host_check_events(s.host, &event);
        if (ret == 0) {
            break;
        }
    }
}

void ENetPeer::close() {
    State& s = *state_;
    if (!s.active()) {
        return;
    }
    s.pop_current_packet();
    for (const auto& [id, peer] : s.peers) {
        if (peer != nullptr && peer->state == ENET_PEER_STATE_CONNECTED) {
            enet_peer_disconnect_now(peer, 0);
        }
    }
    if (s.host != nullptr) {
        enet_host_flush(s.host);
        enet_host_destroy(s.host);
        s.host = nullptr;
    }
    for (const State::Packet& packet : s.incoming) {
        packet.packet->referenceCount--;
        destroy_unused(packet.packet);
    }
    s.mode = State::Mode::None;
    s.incoming.clear();
    s.peers.clear();
    s.ids.clear();
    s.unique_id = 0;
    s.status = ConnectionStatus::Disconnected;
    Peer::set_refuse_new_connections(false);
}

int ENetPeer::unique_id() const {
    return state_->active() ? static_cast<int>(state_->unique_id) : 0;
}

ConnectionStatus ENetPeer::connection_status() const {
    return state_->status;
}

int ENetPeer::available_packet_count() const {
    return static_cast<int>(state_->incoming.size());
}

Result<std::span<const std::uint8_t>> ENetPeer::get_packet() {
    State& s = *state_;
    if (s.incoming.empty()) {
        return make_error(ErrorCode::NotFound, "no incoming packets available");
    }
    s.pop_current_packet();
    s.current = s.incoming.front();
    s.incoming.pop_front();
    return std::span<const std::uint8_t>(s.current.packet->data, s.current.packet->dataLength);
}

Result<void> ENetPeer::put_packet(std::span<const std::uint8_t> packet) {
    State& s = *state_;
    if (!s.active()) {
        return not_active();
    }
    if (s.status != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument,
                          "the peer isn't connected to any server or client");
    }
    if (s.target_peer != 0 && !s.peers.contains(std::abs(s.target_peer))) {
        return make_error(ErrorCode::NotFound,
                          "invalid target peer: " + std::to_string(s.target_peer));
    }

    enet_uint32 flags = 0;
    int channel = kChannelReliable;
    switch (transfer_mode()) {
    case TransferMode::Unreliable:
        flags = ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        channel = kChannelUnreliable;
        break;
    case TransferMode::UnreliableOrdered:
        flags = ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        channel = kChannelUnreliable;
        break;
    case TransferMode::Reliable:
        flags = ENET_PACKET_FLAG_RELIABLE;
        channel = kChannelReliable;
        break;
    }
    if (transfer_channel() > 0) {
        channel = kSystemChannels + transfer_channel() - 1;
    }
    // The channels the server allows: a host's limit, or what a peer agreed to.
    std::size_t channels = s.host->channelLimit;
    if (!is_server()) {
        channels = s.peers[kTargetPeerServer]->channelCount;
    } else if (s.target_peer > 0) {
        channels = s.peers[s.target_peer]->channelCount;
    }
    if (channel >= static_cast<int>(channels)) {
        return make_error(ErrorCode::InvalidArgument,
                          "transfer channel " + std::to_string(transfer_channel()) +
                              " is past the last one the server allows, " +
                              std::to_string(channels - kSystemChannels));
    }

    ENetPacket* message = enet_packet_create(packet.data(), packet.size(), flags);
    if (message == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "couldn't make the ENet packet");
    }
    if (is_server()) {
        if (s.target_peer == 0) {
            enet_host_broadcast(s.host, static_cast<enet_uint8>(channel), message);
        } else if (s.target_peer < 0) {
            // Everyone but one.
            const int exclude = -s.target_peer;
            for (const auto& [id, peer] : s.peers) {
                if (id != exclude && peer != nullptr) {
                    enet_peer_send(peer, static_cast<enet_uint8>(channel), message);
                }
            }
            destroy_unused(message);
        } else {
            State::send(s.peers[s.target_peer], channel, message);
        }
    } else {
        State::send(s.peers[kTargetPeerServer], channel, message); // the server relays
    }
    s.flush();
    return {};
}

int ENetPeer::max_packet_size() const {
    return 1 << 24; // anything is good
}

#else

bool enet_supported() {
    return false;
}

struct ENetPeer::State {};

ENetPeer::ENetPeer() = default;
ENetPeer::~ENetPeer() = default;

namespace {
Error unsupported() {
    return make_error(ErrorCode::PlatformFailure, "ENet isn't available on this platform");
}
} // namespace

Result<void> ENetPeer::create_server(int, int, int, int, int) {
    return unsupported();
}
Result<void> ENetPeer::create_client(std::string_view, int, int, int, int, int) {
    return unsupported();
}
Result<void> ENetPeer::set_bind_ip(std::string_view) {
    return unsupported();
}
int ENetPeer::local_port() const {
    return 0;
}
int ENetPeer::round_trip_time(int) const {
    return -1;
}
void ENetPeer::set_target_peer(int) {}
int ENetPeer::packet_peer() const {
    return 1;
}
TransferMode ENetPeer::packet_mode() const {
    return TransferMode::Reliable;
}
int ENetPeer::packet_channel() const {
    return 1;
}
void ENetPeer::disconnect_peer(int, bool) {}
bool ENetPeer::is_server() const {
    return false;
}
bool ENetPeer::is_server_relay_supported() const {
    return false;
}
void ENetPeer::poll() {}
void ENetPeer::close() {}
int ENetPeer::unique_id() const {
    return 0;
}
ConnectionStatus ENetPeer::connection_status() const {
    return ConnectionStatus::Disconnected;
}
int ENetPeer::available_packet_count() const {
    return 0;
}
Result<std::span<const std::uint8_t>> ENetPeer::get_packet() {
    return unsupported();
}
Result<void> ENetPeer::put_packet(std::span<const std::uint8_t>) {
    return unsupported();
}
int ENetPeer::max_packet_size() const {
    return 0;
}

#endif

} // namespace slash::net
