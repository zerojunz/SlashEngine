#include "slash/net/peer.hpp"

#include <random>
#include <utility>

namespace slash::net {

std::vector<PeerEvent> Peer::take_events() {
    return std::exchange(events_, {});
}

void Peer::peer_connected(int peer) {
    events_.push_back({PeerEvent::Kind::Connected, peer});
}

void Peer::peer_disconnected(int peer) {
    events_.push_back({PeerEvent::Kind::Disconnected, peer});
}

std::uint32_t generate_unique_id() {
    // Godot's MultiplayerPeer::generate_unique_id hashes the clock and addresses; the system's
    // random source does the same job.
    std::random_device device;
    std::uint32_t id = 0;
    while (id == 0 || id == 1) {
        id = device();
        // MurmurHash3's finalizer, as Godot's hash_fmix32.
        id ^= id >> 16U;
        id *= 0x85ebca6bU;
        id ^= id >> 13U;
        id *= 0xc2b2ae35U;
        id ^= id >> 16U;
        id &= 0x7FFFFFFFU; // negative IDs mean "all but"
    }
    return id;
}

// As Godot's, an offline peer has nothing to read and drops what it is given.
Result<std::span<const std::uint8_t>> OfflinePeer::get_packet() {
    return std::span<const std::uint8_t>();
}

Result<void> OfflinePeer::put_packet(std::span<const std::uint8_t> /*packet*/) {
    return {};
}

} // namespace slash::net
