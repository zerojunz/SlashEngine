# HTTP requests

HTTP from game code: [main.cpp](main.cpp) is a game that reads its leaderboard from a web API
with `net::http_get` and posts the player's score with `net::http_post_json`, played in a
session with `net.http_fake` standing in for the server and `net.http` listing the requests.

```text
step {"ticks":3}
{"tick":3,"ticks":3}
shown: Ada 9100; Grace 5600; Linus 3900; posted: HTTP 201
...
shown: leaderboard unavailable: HTTP 503
```

- A request goes out without blocking the tick, and its callback gets the reply at the start of
  a later tick (the `http` system), on the world's thread: `status` (0 when the server couldn't
  be reached, with `error`), `headers` (`reply.header("Content-Type")`) and `body`.
- `net::http_request` takes any method (GET, POST, PUT, PATCH, DELETE, HEAD), headers, a body and
  a timeout; `http_cancel` forgets one.
- Exported games send through the engine's HTTP client (the browser's `fetch()` on the web);
  sessions answer from `net.http_fake`'s fakes first, then the network if they may use it.
- `net.http` lists the requests in flight and the last 32 finished, with their status and ticks.

See [networking.md](../../docs/reference/networking.md#http-requests) and
[commands.md](../../docs/reference/commands.md#nethttp).
