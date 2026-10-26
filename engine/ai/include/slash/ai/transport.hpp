#pragma once

#include "slash/ai/provider.hpp"
#include "slash/core/result.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// How adapters reach a model: one POST of a JSON body (or a GET, for a service's task status
// and files), whose reply arrives whole or as a stream
// of server-sent events. Tests swap in their own Transport or point an adapter at a local server.
namespace slash::ai {

struct HttpRequest {
    std::string url;             // scheme, host, optional port and path
    std::string method = "POST"; // or "GET", which sends no body
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body; // JSON, unless content_type says otherwise
    int timeout_seconds = 600;
    // Text only on the web, where the body goes to fetch() as a string.
    std::string content_type = "application/json";
};

struct HttpResponse {
    int status = 0;
    std::string body;        // the whole body, unless a 200's went to the sink
    std::string retry_after; // the Retry-After header, for a 429 or 503
};

// Receives a 200 response's body as it arrives; returning false stops the request.
using BodySink = std::function<bool(std::string_view data)>;
// Receives what Transport::post would have returned, for Transport::post_async.
using ResponseCallback = std::function<void(Result<HttpResponse> response)>;

class Transport {
public:
    virtual ~Transport() = default;

    // Sends the request: POSTs the body as JSON, or GETs. A 200 response's body goes to the sink,
    // if there is one; any other status keeps its body in the response for the error it carries.
    // Fails only if the server could not be reached, the request was cancelled or the sink stopped
    // it.
    [[nodiscard]] virtual Result<HttpResponse>
    post(const HttpRequest& request, const BodySink& sink, const CancelFlag* cancel) = 0;

    // Starts the POST and returns, for Provider::start: done gets what post() would return, and
    // the sink the body, later, from the platform's event loop. The web's transport does this
    // with the browser's fetch(); by default it calls post(), then done, before returning.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides keep them
    virtual void post_async(const HttpRequest& request, BodySink sink, ResponseCallback done,
                            const CancelFlag* cancel) {
        done(post(request, sink, cancel));
    }
    // Calls then once delay has passed, from the event loop post_async answers on, to try a
    // request again. By default it sleeps, then calls then, before returning.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides keep it
    virtual void later(std::chrono::milliseconds delay, std::function<void()> then) {
        std::this_thread::sleep_for(delay);
        then();
    }
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
// the CA bundle on Linux and Android. On the web the browser's fetch() makes requests, through
// post_async only, and the browser's certificates, proxies and CORS rules apply.
[[nodiscard]] std::shared_ptr<Transport> make_http_transport(HttpOptions options);

// The transport every adapter given none shares, with the default options.
[[nodiscard]] std::shared_ptr<Transport> http_transport();

// A transport that reaches nothing and fails each request with the message, for sessions that
// may not use the network: replay files still answer what they recorded.
[[nodiscard]] std::shared_ptr<Transport> refusing_transport(std::string message);

// Statuses worth trying again: timeouts, conflicts, rate limits and overloaded servers.
[[nodiscard]] bool retryable(int status) noexcept;

// How long to wait before attempt + 1: what Retry-After says, at most a minute, or 1, 2, 4...
// seconds.
[[nodiscard]] std::chrono::milliseconds retry_delay(const HttpResponse& reply, int attempt);

// Waits, a slice at a time so a cancel stops it. False if cancelled.
[[nodiscard]] bool wait_for(std::chrono::milliseconds delay, const CancelFlag* cancel);

// An error reply as an engine error, "<who> answered HTTP <status>: <the server's message>", its
// code following the status.
[[nodiscard]] Error http_error(std::string_view who, const HttpResponse& reply);

// Sends the request and returns the whole reply, trying again after a retryable status up to
// max_retries times. A status other than 2xx (a task accepted is 202) after that is an error
// naming who answered.
[[nodiscard]] Result<HttpResponse> post_with_retries(Transport& transport,
                                                     const HttpRequest& request,
                                                     std::string_view who, int max_retries,
                                                     const CancelFlag* cancel = nullptr);

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
