#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace slash::command {
class Session;
}

// A localhost HTTP server over a command session, so tools that don't speak MCP over stdio, and
// agents connecting to a running game, can drive the engine:
//
//   GET  /v1/commands          every command with its description, capabilities and params
//   POST /v1/commands/<name>   runs a command; the body is its params as a JSON object
//   GET  /v1/openapi.json      the same commands as an OpenAPI 3.1 document
//   POST /rpc                  JSON-RPC 2.0, as slash-cli rpc speaks it over stdio
//   POST /mcp                  MCP over Streamable HTTP (JSON responses, no event stream)
//
// Every request needs "Authorization: Bearer <token>". The server binds to the loopback
// interface only and rejects requests whose Host or Origin header names another site, which
// blocks DNS rebinding from web pages.
namespace slash::server {

struct ServerOptions {
    // Loopback addresses only: "127.0.0.1", "::1" or "localhost".
    std::string host = "127.0.0.1";
    // 0 picks a free port.
    int port = 0;
    // The bearer token clients must send. Generated at random if empty.
    std::string token;
    // How long a request waits for the owner thread to run it before failing with 503.
    std::chrono::milliseconds request_timeout{30000};
};

// Where a running server listens, as written to .slash/server.json for other tools to find.
struct Endpoint {
    std::string host;
    int port = 0;
    std::string token;

    [[nodiscard]] std::string url() const;
    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static Result<Endpoint> from_json(const json::Value& value);
};

// The discovery file a server's owner writes in the project folder.
inline constexpr std::string_view kEndpointFile = ".slash/server.json";

// Reads the discovery file in a project folder.
[[nodiscard]] Result<Endpoint> read_endpoint(std::string_view project_root);

class Server {
public:
    // Starts listening on a background thread. Commands never run there: each request waits in
    // a queue until the session's owner calls poll(), so they never race the game loop.
    [[nodiscard]] static Result<std::unique_ptr<Server>> start(command::Session& session,
                                                               ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] const Endpoint& endpoint() const noexcept;

    // Runs every queued request on the calling thread, which must own the session. Waits up to
    // `wait` for the first request if none is queued. Returns how many requests ran.
    std::size_t poll(std::chrono::milliseconds wait = std::chrono::milliseconds(0));

    // Stops listening. Requests still waiting fail with 503. The destructor calls it.
    void stop();

private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace slash::server
