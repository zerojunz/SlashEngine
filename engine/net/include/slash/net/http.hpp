#pragma once

#include "slash/core/json.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::world {
class World;
} // namespace slash::world

// HTTP from game code: a leaderboard's REST API, a news feed, a download. A request goes out
// without blocking the tick, and its reply comes back to game code at the start of a later tick,
// on the thread that steps the world, so callbacks touch the world without locks and a replayed
// run gets its replies at the same ticks. Games send through the platform (net::set_http_sender:
// the engine's HTTP client natively, the browser's fetch() on the web); tools' sessions answer
// from fakes an agent sets (net.http_fake) so playtests need no network.
namespace slash::net {

struct HttpRequest {
    std::string url;            // http:// or https://, with any query string
    std::string method = "GET"; // GET, POST, PUT, PATCH, DELETE or HEAD
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string content_type = "application/json"; // for a body
    int timeout_seconds = 30;
};

struct HttpReply {
    // The HTTP status, or 0 when the server couldn't be reached (error says why) or the request
    // was cancelled.
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string error;

    // A 2xx status.
    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    // The first header of that name, ignoring case, or "".
    [[nodiscard]] std::string_view header(std::string_view name) const;
};

// Sends a request and calls done once with the reply, from any thread.
using HttpSender = std::function<void(HttpRequest request, std::function<void(HttpReply)> done)>;

// Called with the reply at the start of the tick after it arrived, on the world's thread.
using HttpCallback = std::function<void(world::World& world, const HttpReply& reply)>;

// Adds the "http" system, which hands replies to their callbacks first in each tick, and sets
// how requests are sent. Without a sender every request fails with "no HTTP here". Installing
// again replaces the sender and keeps requests in flight. runtime::Game installs it with
// GameOptions::http_sender, which the game's main() gives the engine's HTTP client (requests
// go one after another, natively on a thread of their own and on the web through fetch());
// sessions answer from their fakes, then the network if they have the network capability.
void install_http(world::World& world, HttpSender sender);
// Replaces the sender, for a world that has HTTP.
void set_http_sender(world::World& world, HttpSender sender);

// Starts a request and returns its ID (from 1), or 0 when the world has no HTTP (install_http),
// when done is never called. A URL that isn't http:// or https://, an unknown method or a world
// with no sender gets an error reply (status 0) at the next tick, as an unreachable server
// does.
std::uint64_t http_request(world::World& world, HttpRequest request, HttpCallback done);
// GETs a URL; POSTs JSON.
std::uint64_t http_get(world::World& world, std::string url, HttpCallback done);
std::uint64_t http_post_json(world::World& world, std::string url, const json::Value& body,
                             HttpCallback done);
// Forgets a request: its callback isn't called. The request itself may still reach the
// server.
void http_cancel(world::World& world, std::uint64_t id);

// A request as net.http lists it.
struct HttpRecord {
    std::uint64_t id = 0;
    std::string method;
    std::string url;
    bool done = false;
    int status = 0;
    std::string error;
    std::uint64_t started_tick = 0;
    std::uint64_t finished_tick = 0;
    std::size_t bytes = 0; // the reply's body
};
// The requests in flight and the last 32 finished, oldest first.
[[nodiscard]] std::vector<HttpRecord> http_requests(const world::World& world);

// A canned reply for requests whose URL starts with url_prefix (and, if method isn't empty,
// that method), for tests and agents' playtests.
struct HttpFake {
    std::string url_prefix;
    std::string method;
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};
// A sender that answers from fakes, the latest first, and fails anything else with "no fake for
// <method> <url>"; or passes it on to fallback when there is one.
class HttpFakes {
public:
    void add(HttpFake fake);
    void clear();
    [[nodiscard]] const std::vector<HttpFake>& fakes() const noexcept { return fakes_; }
    // The sender, which reads this object's fakes as they are when each request is sent; keep
    // the object alive while the sender is in use.
    [[nodiscard]] HttpSender sender(HttpSender fallback = nullptr) const;

private:
    std::vector<HttpFake> fakes_;
};

} // namespace slash::net
