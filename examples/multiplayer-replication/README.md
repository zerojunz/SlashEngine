# Multiplayer replication

Three worlds in one program share an arena over ENet, replicated as Godot's
`MultiplayerSpawner` and `MultiplayerSynchronizer` replicate a scene tree
([docs/reference/networking.md](../../docs/reference/networking.md#replication)). Every world
loads the same arena: an `arena` entity things are placed under, and a `spawner` whose
`MultiplayerSpawner` spawns bullets and treasures placed there, and custom spawns.

What happens:

- The server spawns a player for each client with `spawn`. Every peer makes it with the same
  spawn function, which gives it to its client with `set_authority`.
- Alice moves her player, and her `MultiplayerSynchronizer` sends its position to the others.
  When bob moves it on his own world, the next sync puts it back.
- The server places a bullet under the arena; the spawner spawns it everywhere with its spawn
  state. Its position is synced each tick (`"always"`), and its color reliably when it changes
  (`"on_change"`).
- A treasure without `public_visibility` is spawned only on alice's world, the peer
  `set_visibility_for` names.
- The bullet, then bob's player after bob leaves, are destroyed on the server and despawn on
  the clients.

IDs change each run, so the output names peers by role.
