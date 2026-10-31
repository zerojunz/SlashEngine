# Networking

Games talk over the network as Godot's high-level multiplayer does. A peer is one machine's end
of the game's network: a server is peer 1, and each client joins it with an ID of its own.
Transports are kinds of peer, so code written against `net::Peer` runs over any of them.

| Peer | Godot's | Where |
| --- | --- | --- |
| `net::ENetPeer` | `ENetMultiplayerPeer` | Reliable UDP over [ENet](https://github.com/lsalzman/enet): macOS, Windows, Linux, iOS and Android. Not on the web, whose browsers have no UDP sockets. IPv4 only |
| `net::OfflinePeer` | `OfflineMultiplayerPeer` | A server with no one else, always connected: a game played alone runs the same code it runs online |

Replication, remote procedure calls and WebSocket are next on the [roadmap](../roadmap.md).

## Peers

```cpp
#include "slash/net/enet.hpp"
namespace net = slash::net;

net::ENetPeer server;
(void)server.create_server(7000, /*max_clients=*/8);

net::ENetPeer client;
(void)client.create_client("127.0.0.1", 7000);

// Once a frame or tick, on each machine:
client.poll();
for (const net::PeerEvent& event : client.take_events()) {
    // event.kind is Connected or Disconnected; event.peer is the peer's ID (1 for the server).
}
while (client.available_packet_count() > 0) {
    const int from = client.packet_peer(); // read these before get_packet()
    auto packet = client.get_packet();     // valid until the next get_packet() or poll()
}
```

`poll()` takes in what arrived and sends what waits; nothing happens between calls. Packets wait
in the order they arrived, each with the peer that sent it (`packet_peer`), its transfer channel
(`packet_channel`) and how it travelled (`packet_mode`), which describe the next packet
`get_packet()` returns. Peers joining and leaving wait in `take_events()`, as Godot's
`peer_connected` and `peer_disconnected` signals.

| Call | Does |
| --- | --- |
| `set_target_peer(id)` | Where later packets go: a peer's ID, `kTargetPeerBroadcast` (0) for everyone, or minus an ID for everyone but that peer. A client's packets always go to the server, which relays |
| `set_transfer_mode(mode)` | `Reliable` (the default: arrives, in order), `UnreliableOrdered` (may be lost; older packets arriving after newer ones are dropped) or `Unreliable` (may be lost or arrive out of order) |
| `set_transfer_channel(n)` | Channel 0 is the default; each other channel keeps its own order, so a stream of positions doesn't wait behind chat messages |
| `put_packet(bytes)` | Sends to the target with the mode and channel set; fails while connecting, for a target that isn't connected, or for a channel past what the server allows |
| `disconnect_peer(id, force)` | Disconnects a peer (a client passes 1 to leave); with `force`, at once and without telling it |
| `set_refuse_new_connections(true)` | Turns away peers that try to join from now on. Over ENet the client hears the server connect, then disconnect, since ENet completes its handshake first; the server reports neither |
| `close()` | Disconnects everyone and stops; `unique_id()` is 0 again |
| `unique_id()` | 1 on a server, the client's ID on a client, 0 when inactive |
| `connection_status()` | `Disconnected`, `Connecting` (a client until the server answers) or `Connected` |

## ENet

`create_server(port, max_clients = 32, max_channels = 0, in_bandwidth = 0, out_bandwidth = 0)`
listens on a port for up to `max_clients` (1 to 4095), with `max_channels` transfer channels
besides channel 0 (0: as many as ENet allows, 254) and bandwidth limits in bytes a second (0:
none). Port 0 picks a free port, which `local_port()` reports. A full server leaves new clients
unanswered until ENet's timeout ends their attempt.

`create_client(address, port, channel_count = 0, in_bandwidth = 0, out_bandwidth = 0,
local_port = 0)` connects to a server by IPv4 address or host name. As in Godot, the client picks
its ID at random (`net::generate_unique_id()`: above 1, below 2^31) and sends it with the
connection; the server turns away an ID below 2 or one already taken. The client is connected,
and hears peer 1 connect, once the server answers.

`set_bind_ip(ip)` picks the address servers and clients bind to: `"*"` (any, the default) or an
IPv4 address such as `"127.0.0.1"` to listen on this machine only. `round_trip_time(id)` gives a
connected peer's mean round trip in milliseconds.

ENet's channels 0 and 1 carry transfer channel 0's reliable and unreliable packets, and transfer
channel n is ENet channel n + 1, as Godot lays them out. Unreliable packets above ENet's MTU
(1,392 bytes) are split and lost whole if a piece is lost. `net::enet_supported()` is false on
the web.

[`examples/enet-peers`](../../examples/enet-peers/) runs a server and three clients on one
machine.
