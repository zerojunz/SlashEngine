# Networking

Games talk over the network as Godot's high-level multiplayer does. A peer is one machine's end
of the game's network: a server is peer 1, and each client joins it with an ID of its own.
Transports are kinds of peer, so code written against `net::Peer` runs over any of them.

| Peer | Godot's | Where |
| --- | --- | --- |
| `net::ENetPeer` | `ENetMultiplayerPeer` | Reliable UDP over [ENet](https://github.com/lsalzman/enet): macOS, Windows, Linux, iOS and Android. Not on the web, whose browsers have no UDP sockets. IPv4 only |
| `net::OfflinePeer` | `OfflineMultiplayerPeer` | A server with no one else, always connected: a game played alone runs the same code it runs online |

A world's multiplayer runs on a peer: it admits the peers that join, tells clients about each
other through the server, relays their packets and calls remote procedures on entities, as
Godot's `SceneMultiplayer`. Replication, WebSocket and commands are next on the
[roadmap](../roadmap.md).

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

## A world's multiplayer

```cpp
#include "slash/net/multiplayer.hpp"

net::install(world); // the world's multiplayer, offline, and the system that polls it
auto peer = std::make_shared<net::ENetPeer>();
(void)peer->create_client("game.example.com", 7000);
(void)net::multiplayer(world).set_peer(peer);
```

`net::install` gives a world its `net::Multiplayer` and the `multiplayer` system (priority -950,
right after `input`), which polls it at the start of each tick, as Godot polls a scene tree's
multiplayer before processing nodes. It starts on an `OfflinePeer`; `set_peer` takes a peer that
is connecting or connected (null goes back offline) and forgets the peers and caches of the one
before.

Its events are world events (`World::events()`, `events.read`), each with data `{"peer": id}`:

| Event | When |
| --- | --- |
| `peer_connected`, `peer_disconnected` | A peer joined or left. A client hears the server (peer 1) and, through it, every other client |
| `connected_to_server` | A client joined its server |
| `connection_failed` | A client's connection failed before it joined |
| `server_disconnected` | The connection ended, by either side |
| `peer_authenticating`, `peer_authentication_failed` | A joining peer waits for authentication, or left or timed out during it |
| `peer_packet` | Raw bytes arrived (`{"peer", "size"}`); `take_packets()` returns them |

`peer_ids()` lists the connected peers, `unique_id()` and `is_server()` describe this one, and
`disconnect_peer(id)` drops a peer without reporting it. With `set_server_relay(false)` the
server neither tells clients about each other nor relays their packets.

**Authentication.** With `set_auth_callback(fn)`, a joining peer waits in
`authenticating_peer_ids()` while the two sides exchange `send_auth(id, bytes)`, which reach the
other side's callback, until each calls `complete_auth(id)`; one that doesn't finish within
`set_auth_timeout(seconds)` (3 by default, 0 for none) is dropped. Both sides need a callback.

**Raw packets.** `send_bytes(bytes, id = 0, mode = Reliable, channel = 0)` sends bytes to a peer,
everyone (0) or all but one (minus an ID), relayed by the server between clients.

## Remote procedure calls

Every peer registers the same procedures, by name, with Godot's `@rpc` settings, and calls them
on an entity by its ID, as Godot calls a method on a node by its path:

```cpp
net::Multiplayer& mp = net::multiplayer(world);
mp.add_rpc("move",
           {.mode = net::RpcMode::Authority, // only the entity's authority may call it
            .transfer_mode = net::TransferMode::UnreliableOrdered,
            .channel = 1,
            .component = "Transform2D"}, // a procedure of entities with a Transform2D
           [](world::World& world, entt::entity entity, const json::Value& args) {
               // net::multiplayer(world).remote_sender_id() is the caller.
           });
(void)mp.rpc("player", "move", json::Array{120, 64});         // every peer
(void)mp.rpc("player", "move", json::Array{120, 64}, 1);      // the server alone
```

| Setting | Means |
| --- | --- |
| `mode` | `Authority` (the default): only the peer the entity answers to may call it. `AnyPeer`: every peer. `Disabled`: no one. A call that isn't allowed is refused where it arrives, with an error logged |
| `call_local` | The call also runs on the peer that makes it (`remote_sender_id()` is then its own ID). Without it, calling yourself is refused |
| `transfer_mode`, `channel` | How the call travels |
| `component` | The component an entity needs for the procedure to be its own, as a Godot method belongs to a node's script; `""` for every entity |

The arguments are one JSON value (`null` for none), sent as compact JSON text. An entity answers
to peer 1 unless `set_authority(id, peer)` says otherwise; by default that also sets the
entities placed under it (`Transform2D` and `Transform3D` children) and those its instanced scene
brought. Every peer sets the same authorities, as in Godot.

As in Godot, the first call on an entity to a peer carries the entity's ID, and the peer answers
with a short number used from then on, after checking that both sides have the same procedures
for it (an error is logged if not).

[`examples/multiplayer-rpc`](../../examples/multiplayer-rpc/) plays a game on three worlds in one
program.

## Replication

Two components replicate a world, as Godot's `MultiplayerSpawner` and `MultiplayerSynchronizer`
do a scene tree. Every peer loads the same scene with them; only entities with IDs take part.

```json
"spawner": {"components": {"MultiplayerSpawner": {
  "spawnable_scenes": ["prefabs/bullet.prefab.json"], "spawn_path": "level"}}},
```

```json
{
  "components": {
    "Transform2D": {},
    "MultiplayerSynchronizer": {"properties": [
      {"path": "Transform2D/position"},
      {"path": "Sprite2D/modulate/a", "spawn": false, "replication_mode": "on_change"}
    ]}
  },
  "format": 1
}
```

**Spawning.** When the spawner's authority places an entity that extends one of
`spawnable_scenes` among `spawn_path`'s `Transform2D` or `Transform3D` children, the entity is
spawned on every peer that can see it, with the same ID and under the same parent, and
despawned there when it is destroyed. Peers that join later are sent what was spawned before,
and a peer that leaves (or a server that goes) takes what it spawned with it. Each peer hears a
`spawned` or `despawned` world event, with the entity's ID and data `{"spawner": id}`.
`spawn_limit` caps how many a spawner has out at once.

A custom spawn is made by code on every peer, as Godot's `spawn_function`:

```cpp
net::Multiplayer& mp = net::multiplayer(world);
mp.set_spawn_function("spawner", [](world::World& w, std::string_view id, const json::Value& data)
                                     -> Result<entt::entity> { /* make the entity with this ID */ });
mp.set_enter_function("spawner", [&](world::World& w, entt::entity entity) {
    mp.set_authority(w.id_of(entity), /* the peer it answers to */ 2);
});
(void)mp.spawn("spawner", "p2", json::Array{100, 40}); // on the spawner's authority only
```

The enter function runs on a peer that receives a spawn, once the entity is placed and before
its synchronizers start, as a spawned scene's `_enter_tree`: the place to give it to the peer
that controls it.

**Synchronizing.** A `MultiplayerSynchronizer` sends the properties of its root (its own entity,
or `root_path`) from its authority to the peers that can see it. A property's `path` is
`"Component/field"`, or part of one (`"Transform2D/position/x"`, `"Sprite2D/modulate/a"`):

| Field | Means |
| --- | --- |
| `spawn` | Sent with the entity when a spawner spawns it, set before anything sees it (`true`) |
| `replication_mode` | `"always"`: in every sync, unreliably, the newest kept. `"on_change"`: reliably, when it changes. `"never"`: only at spawn |
| `replication_interval`, `delta_interval` | Seconds of world time between syncs and between looks for changes; 0 for every tick |

Each sync and change that arrives is a `synchronized` or `delta_synchronized` world event with
the synchronizer's ID. Data from a peer that isn't the synchronizer's authority is refused. The
intervals count world time (the ticks the multiplayer system polls in), so runs repeat.
`set_max_sync_packet_size` and `set_max_delta_packet_size` set the largest packets (1350 and
65535 bytes).

**Visibility.** A synchronizer with `public_visibility` is seen by every peer; without it, only
by those `set_visibility_for(id, peer, true)` names. `add_visibility_filter(id, fn)` adds a test
every peer must pass, checked again each tick before the game's systems (`visibility_update_mode`
`"idle"`), after physics (`"physics"`, by the `multiplayer_visibility` system) or when
`update_visibility(id)` is called (`"none"`). A spawned entity is spawned only on the peers one
of its synchronizers is visible to, and despawned from those that stop seeing it, and remote
calls on an entity with synchronizers go only to the peers that can see it.

As in Godot, spawns and their synchronizers carry network IDs, and a synchronizer on an entity
nothing spawned is named by the same short number remote calls use.
