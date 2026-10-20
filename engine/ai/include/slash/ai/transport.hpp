#pragma once

#include "slash/ai/provider.hpp"
#include "slash/core/result.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// How adapters reach a model: one POST of a JSON body, whose reply arrives whole or as a stream
// of server-sent events. Tests swap in their own Transport or point an adapter at a local server.
namespace slash::ai {

struct HttpRequest {
    std::string url; // scheme, host, optional port and path
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body; // JSON
    int timeout_seconds = 600;
};

struct HttpResponse {
    int status = 0;
    std::string body;        // the whole body, unless a 200's went to the sink
    std::string retry_after; // the Retry-After header, for a 429 or 503
};

// Receives a 200 response's body as it arrives; returning false stops the request.
using BodySink = std::function<bool(std::string_view data)>;

class Transport {
public:
    virtual ~Transport() = default;

    // POSTs the body as JSON. A 200 response's body goes to the sink, if there is one; any other
    // status keeps its body in the response for the error it carries. Fails only if the server
    // could not be reached, the request was cancelled or the sink stopped it.
    [[nodiscard]] virtual Result<HttpResponse>
    post(const HttpRequest& request, const BodySink& sink, const CancelFlag* cancel) = 0;
};

// HTTP over the network, shared by every adapter that is given no transport. It speaks plain
// http://, which local model servers use; an https:// URL fails with an error saying so.
[[nodiscard]] std::shared_ptr<Transport> http_transport();

// A URL split up for a client: "http", "localhost", 11434, "/v1".
struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path; // "" or starting with '/'
};

[[nodiscard]] Result<Url> parse_url(std::string_view url);

// A server-sent event: its event name ("" when it has none) and its data lines joined.
struct SseEvent {
    std::string event;
    std::string data;
};

// Splits a stream of server-sent events, fed in pieces as they arrive.
class SseParser {
public:
    // The events the data completes.
    [[nodiscard]] std::vector<SseEvent> feed(std::string_view data);
    // The last event, if the stream ended without a blank line after it.
    [[nodiscard]] std::vector<SseEvent> finish();

private:
    void line(std::string_view text, std::vector<SseEvent>& events);

    std::string buffer_;
    SseEvent current_;
    bool has_data_ = false;
};

} // namespace slash::ai
