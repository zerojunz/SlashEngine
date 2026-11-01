# Network sessions

A game played on three sessions at once, each driven only by commands, as three agents would
drive three copies of a game ([docs/reference/networking.md](../../docs/reference/networking.md)).
[main.cpp](main.cpp) opens [arena.scene.json](arena.scene.json) in each session, whose spawner
spawns [bullet.prefab.json](bullet.prefab.json) entities under the arena:

- The server hosts with `net.host` on a free port; alice and bob join with `net.join` and are
  `connecting` until steps poll the answer. `net.state` then lists each session's peers.
- Alice calls the game's `say` procedure with `net.rpc`: it runs on her session at once and on
  the others when their steps take it in, relayed through the server.
- The server places a bullet under the arena with a `batch`; the players' steps spawn it, and
  its synchronizer sends its position each tick when the server moves it.
- The server saves a `world.snapshot` before the move and goes back to it with
  `world.restore`: its entities are put back from a copy, the bullet returns to where it was and
  stays spawned, as `net.state`'s `spawned` shows, rather than being despawned and spawned
  again.
- Destroying the bullet on the server despawns it everywhere, and bob's `net.close` takes him
  offline; the others hear him go.

The `say` procedure is registered in C++, as a game's code registers its own. IDs and ports
change each run, so the output names peers by role.
