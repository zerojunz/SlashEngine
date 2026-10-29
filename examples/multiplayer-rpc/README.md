# Multiplayer and remote procedure calls

Three worlds in one program play one game over ENet, each with its world's multiplayer, as
Godot's `SceneMultiplayer` ([docs/reference/networking.md](../../docs/reference/networking.md)).
[main.cpp](main.cpp) gives each world a player entity per client and a door, calls
`net::install` and registers the same remote procedures on every peer:

- `chat`: any peer may call it, and it runs where it is called too (`call_local`).
- `move`: only an entity's authority may call it, unreliably on channel 1, as positions are sent
  many times a second, and only on entities with a `Transform2D`.
- `open`: the door's authority opens it everywhere.

What happens:

- Alice and bob join the server, and hear about each other through it (`peer_connected` events
  on each world).
- Every peer makes each player's entity answer to its client with `set_authority`.
- Alice chats: the server relays her call to bob, and each world logs her as the caller
  (`remote_sender_id`).
- Bob moves his player, which every other world applies to its `Transform2D`, then tries to move
  alice's: the server and alice refuse it and log an error, since bob isn't its authority.
- The server opens the door everywhere, sends raw bytes with `send_bytes`, and bob leaves.

IDs change each run, so the output names peers by role and sorts each world's log.
