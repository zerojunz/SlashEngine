#pragma once

#include "slash/services/services.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Analytics sent to a server the developer runs, as project.json's
// services.analytics.endpoint asks: events batched as JSON and POSTed, kept on disk until the
// server takes them, with sessions as Firebase counts them. Exported games get it when the
// project names an endpoint; the HTTP client comes from engine/ai (main.cpp).
namespace slash::services {

// One POST of a batch.
struct HttpPost {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body; // JSON
};
// Sends a POST without waiting for it, then calls done once, from any thread, with the HTTP
// status, or 0 when the server couldn't be reached.
using HttpPoster = std::function<void(HttpPost post, std::function<void(int status)> done)>;

struct EndpointOptions {
    HttpPoster post;
    // A file that keeps unsent events and the install's ID from run to run; "" keeps nothing.
    std::string store_path;
    // The game, as events describe it.
    std::string app_name;
    std::string app_version;
    std::string platform; // platform::os_name()
    // Milliseconds since 1970, and new IDs (32 hex digits): the system's clock and random
    // numbers unless a test gives its own.
    std::function<std::int64_t()> now_ms;
    std::function<std::string()> new_id;
};

// The backend, named "endpoint". Each event carries the session it belongs to: one starts with
// the first event of a run, or after session_minutes without events, and logs session_start
// (with first_open before it on the install's first run). Batches go when batch_size events wait,
// flush_seconds after the oldest, and on flush(); one at a time, tried again after 1, 2, 4...
// minutes (at most 10) when the server can't be reached or answers 408, 429 or 5xx, and dropped
// when it refuses them otherwise. Revoked consent drops what waits. Its status() says
// {"device", "session", "queued", "in_flight", "sent", "dropped", "last_status"}.
[[nodiscard]] std::unique_ptr<AnalyticsBackend> make_endpoint_analytics(AnalyticsEndpoint endpoint,
                                                                        EndpointOptions options);

} // namespace slash::services
