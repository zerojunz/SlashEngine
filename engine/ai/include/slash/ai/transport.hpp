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

// How an HTTP transport reaches servers.
struct HttpOptions {
    // A PEM file of the certificates to trust instead of the system's; "" for the system's.
    std::string ca_file{}; // NOLINT(readability-redundant-member-init)
    // A proxy, as http://[user:password@]host:port; "" for none.
    std::string proxy{}; // NOLINT(readability-redundant-member-init)
    // What is left empty comes from the environment, as curl takes it: SSL_CERT_FILE, then
    // https_proxy (or HTTPS_PROXY) for https:// URLs and http_proxy for http:// ones, except for
    // hosts no_proxy (or NO_PROXY) names. Reads a variable; null for the process's own.
    std::function<std::string(const std::string& name)>
        environment{}; // NOLINT(readability-redundant-member-init)
};

// HTTP and HTTPS over the network. https:// checks the server's certificate against the
// system's: the platform's trust settings on macOS and iOS, the system store on Windows, and
// the CA bundle on Linux and Android. On the web, requests fail until a fetch transport lands.
[[nodiscard]] std::shared_ptr<Transport> make_http_transport(HttpOptions options);

// The transport every adapter given none shares, with the default options.
[[nodiscard]] std::shared_ptr<Transport> http_transport();

// A transport that reaches nothing and fails each request with the message, for sessions that
// may not use the network: replay files still answer what they recorded.
[[nodiscard]] std::shared_ptr<Transport> refusing_transport(std::string message);

// A URL split up for a client: "http", "localhost", 11434, "/v1".
struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path; // "" or starting with '/'
};

[[nodiscard]] Result<Url> parse_url(std::string_view url);

// The proxy a request to the URL goes through with these options, or "" for none. This
// machine's own addresses (localhost, 127.x.x.x, ::1) never use one.
[[nodiscard]] std::string proxy_for(const Url& url, const HttpOptions& options);

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
