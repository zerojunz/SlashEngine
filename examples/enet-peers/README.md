# ENet peers

A server and three clients on one machine over ENet, as Godot's `ENetMultiplayerPeer`
([docs/reference/networking.md](../../docs/reference/networking.md)). [main.cpp](main.cpp)
polls each `net::ENetPeer` itself, as a game's multiplayer does once a tick:

- The server listens on a free port (`create_server(0)`, then `local_port()`), for two clients
  with one transfer channel besides channel 0.
- Each client picks a random ID above 1 and is `connecting` until the server answers; then both
  sides hear the other connect through `take_events()`.
- The server sends to one client, to everyone (`kTargetPeerBroadcast`) and to all but one
  (minus an ID). Each client's packets arrive in order, from peer 1.
- A client sends a reliable packet on channel 0 and a position unreliably on channel 1; the
  server reads each one's channel and mode. Channel 2 is past what the server allows, so
  `put_packet` refuses it with the reason.
- A third client gets no answer from the full server, as ENet ignores it until its timeout.
- A client leaves with `disconnect_peer(1)`, and the server closes; the peers that remain hear
  each one go.

IDs and ports change each run, so the output names peers by role.
