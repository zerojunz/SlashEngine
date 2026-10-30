# Analytics endpoint

Analytics sent to a server the developer runs, with no SDK
([docs/reference/services.md](../../docs/reference/services.md#sending-to-your-own-server)).
project.json's `services.analytics.endpoint` names the server, and `funnels` the steps a player
goes through. [main.cpp](main.cpp) runs a collector on this machine in its place, and gives the
endpoint backend the same HTTP client an exported game's `main()` does, with a clock and IDs of
its own so the output repeats:

- The player goes through the tutorial: each of its steps is a `funnel_step` world event, and
  `services.state` says how far each funnel got.
- The first event of the install starts its first session, so `first_open` and
  `session_start` come before it; five events make a batch.
- The server is down (503), so the batch waits and goes again a minute later, when it is
  taken (204); `level_start`, by then older than `flush_seconds`, follows in a batch of its own.
- The server receives the engine's own JSON: the app, the install's device ID, and each event
  with its params, time, session, sequence number and user properties.
