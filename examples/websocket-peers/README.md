# WebSocket peers

WebSocket on one machine, as Godot's `WebSocketPeer` and `WebSocketMultiplayerPeer`
([docs/reference/networking.md](../../docs/reference/networking.md)). [main.cpp](main.cpp)
polls each peer itself, as a game's multiplayer does once a tick:

- The server listens on a free port (`create_server(0)`, then `local_port()`) and speaks the
  `chat` subprotocol, so a client that speaks only `voice` is refused.
- A `net::WebSocketPeer` that offers `voice` and `chat` connects, agrees on `chat` and is given
  its ID in a first binary message of four bytes; the server hears it connect and reads the
  path it asked for.
- Text and bytes each arrive as what they were sent as (`was_string_packet()`).
- The client closes with code 4000 and a reason, which it reads back once the server answers;
  the server hears it go.
- Three worlds then play over `WebSocketMultiplayerPeer`: the clients hear of each other through
  the server, and alice's remote call reaches bob through the server's relay.

Web games join servers like this one as clients, through the browser's WebSocket. IDs and ports
change each run, so the output names peers by role.
