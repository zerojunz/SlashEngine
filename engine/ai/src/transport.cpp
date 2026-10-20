#include "slash/ai/transport.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <map>
#else
#include "slash/platform/file.hpp"

#include <httplib.h>
#include <mbedtls/x509_crt.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#if defined(__EMSCRIPTEN__)
// POSTs with the browser's fetch(), handing the status, the body as it streams and the end back
// to the slash_ai_fetch_* functions below, which say whether to go on reading. Bytes go through
// a buffer the engine owns (slash_ai_fetch_buffer), so the game needs no malloc exported.
// clang-format off
EM_JS_DEPS(slash_ai_fetch, "$UTF8ToString");
EM_JS(void, slash_ai_fetch,
      (int id, const char* url, const char* headers, const char* body, int timeout_ms), {
          const controller = new AbortController();
          let timedOut = false;
          const timer = setTimeout(() => {
              timedOut = true;
              controller.abort();
          }, timeout_ms);
          const put = (bytes) => {
              const at = _slash_ai_fetch_buffer(id, bytes.length);
              if (at) {
                  HEAPU8.set(bytes, at);
              }
              return bytes.length;
          };
          const text = (value) => put(new TextEncoder().encode(value));
          const end = (message) => {
              clearTimeout(timer);
              _slash_ai_fetch_end(id, message === null ? 0 : 1, message === null ? 0 : text(message));
          };
          fetch(UTF8ToString(url), {
              method : "POST",
              headers : JSON.parse(UTF8ToString(headers)),
              body : UTF8ToString(body),
              signal : controller.signal,
          })
              .then(async (response) => {
                  const after = text(response.headers.get("Retry-After") || "");
                  if (!_slash_ai_fetch_status(id, response.status, after)) {
                      controller.abort();
                      end(null);
                      return;
                  }
                  if (response.body && response.body.getReader) {
                      const reader = response.body.getReader();
                      for (;;) {
                          const part = await reader.read();
                          if (part.done) {
                              break;
                          }
                          if (!_slash_ai_fetch_data(id, put(part.value))) {
                              controller.abort();
                              break;
                          }
                      }
                  } else {
                      _slash_ai_fetch_data(id, put(new Uint8Array(await response.arrayBuffer())));
                  }
                  end(null);
              })
              .catch((error) => {
                  end(timedOut ? "timed out" : String((error && error.message) || error));
              });
      });
// clang-format on
#endif

namespace slash::ai {

Result<Url> parse_url(std::string_view url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(url) + "\" is not a URL; expected http://host/path");
    }
    Url out;
    out.scheme = std::string(url.substr(0, scheme_end));
    if (out.scheme != "http" && out.scheme != "https") {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(url) + "\" is not an http:// or https:// URL");
    }
    const std::string_view rest = url.substr(scheme_end + 3);
    const std::size_t path_start = rest.find('/');
    const std::string_view authority = rest.substr(0, path_start);
    if (path_start != std::string_view::npos) {
        out.path = std::string(rest.substr(path_start));
    }
    std::string_view port;
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(url) + "\" has an unclosed '[' in its host");
        }
        out.host = std::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            port = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        out.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
        }
    }
    if (out.host.empty()) {
        return make_error(ErrorCode::InvalidArgument, "\"" + std::string(url) + "\" has no host");
    }
    out.port = out.scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        int number = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
        if (error != std::errc() || end != port.data() + port.size() || number <= 0 ||
            number > 65535) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(url) + "\" has an invalid port");
        }
        out.port = number;
    }
    return out;
}

std::vector<SseEvent> SseParser::feed(std::string_view data) {
    buffer_.append(data);
    std::vector<SseEvent> events;
    std::size_t start = 0;
    while (start < buffer_.size()) {
        const std::size_t end = buffer_.find_first_of("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        // A '\r' at the end may be the first half of "\r\n"; wait for the next piece.
        if (buffer_[end] == '\r' && end + 1 == buffer_.size()) {
            break;
        }
        line(std::string_view(buffer_).substr(start, end - start), events);
        start = end + 1;
        if (buffer_[end] == '\r' && buffer_[start] == '\n') {
            ++start;
        }
    }
    buffer_.erase(0, start);
    return events;
}

std::vector<SseEvent> SseParser::finish() {
    std::vector<SseEvent> events;
    if (!buffer_.empty()) {
        std::string rest = std::move(buffer_);
        buffer_.clear();
        if (rest.back() == '\r') {
            rest.pop_back();
        }
        line(rest, events);
    }
    line({}, events);
    return events;
}

void SseParser::line(std::string_view text, std::vector<SseEvent>& events) {
    if (text.empty()) {
        if (has_data_) {
            events.push_back(std::move(current_));
        }
        current_ = {};
        has_data_ = false;
        return;
    }
    if (text.front() == ':') {
        return; // a comment, such as a keep-alive
    }
    const std::size_t colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value;
    if (colon != std::string_view::npos) {
        value = text.substr(colon + 1);
        if (value.starts_with(' ')) {
            value.remove_prefix(1);
        }
    }
    if (field == "data") {
        if (has_data_) {
            current_.data += '\n';
        }
        current_.data.append(value);
        has_data_ = true;
    } else if (field == "event") {
        current_.event = std::string(value);
    }
}

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return out;
}

// A variable from the options' environment, or else the other one.
std::string environment(const HttpOptions& options, const char* name, const char* other = nullptr) {
    const auto read = [&options](const char* key) {
        return options.environment ? options.environment(key) : platform::environment_variable(key);
    };
    std::string value = read(name);
    if (value.empty() && other != nullptr) {
        value = read(other);
    }
    return value;
}

bool is_loopback(const std::string& host) {
    return host == "localhost" || host == "::1" || host.starts_with("127.") ||
           lower(host).ends_with(".localhost");
}

// An IPv4 address as a number, if the text is one.
std::optional<std::uint32_t> ipv4(std::string_view text) {
    std::uint32_t value = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (!text.starts_with('.')) {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
        unsigned number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc() || number > 255) {
            return std::nullopt;
        }
        value = (value << 8U) | number;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    }
    return text.empty() ? std::optional<std::uint32_t>(value) : std::nullopt;
}

// Whether a no_proxy entry in CIDR notation ("10.0.0.0/8") covers the host, an IPv4 address.
bool in_network(std::string_view entry, const std::string& host) {
    const std::size_t slash = entry.find('/');
    const std::optional<std::uint32_t> network = ipv4(entry.substr(0, slash));
    const std::optional<std::uint32_t> address = ipv4(host);
    const std::string_view bits_text = entry.substr(slash + 1);
    unsigned bits = 0;
    const auto [end, error] =
        std::from_chars(bits_text.data(), bits_text.data() + bits_text.size(), bits);
    if (!network || !address || error != std::errc() ||
        end != bits_text.data() + bits_text.size() || bits > 32) {
        return false;
    }
    const std::uint32_t mask = bits == 0 ? 0U : ~std::uint32_t{0} << (32U - bits);
    return (*network & mask) == (*address & mask);
}

// Whether no_proxy's comma-separated entries cover the host, as curl and Go read them: "*",
// the host itself, a domain it is in ("example.com", ".example.com" and "*.example.com" all
// cover api.example.com) or a network it is in ("10.0.0.0/8").
bool bypasses_proxy(const std::string& host, std::string_view no_proxy) {
    const std::string name = lower(host);
    while (!no_proxy.empty()) {
        const std::size_t comma = no_proxy.find(',');
        std::string entry = lower(no_proxy.substr(0, comma));
        no_proxy =
            comma == std::string_view::npos ? std::string_view() : no_proxy.substr(comma + 1);
        while (!entry.empty() && entry.front() == ' ') {
            entry.erase(0, 1);
        }
        while (!entry.empty() && entry.back() == ' ') {
            entry.pop_back();
        }
        if (entry.find('/') != std::string::npos) {
            if (in_network(entry, name)) {
                return true;
            }
            continue;
        }
        if (const std::size_t colon = entry.rfind(':');
            colon != std::string::npos && entry.find(':') == colon) {
            entry.resize(colon); // a port: entries cover every port here
        }
        if (entry.starts_with("*.")) {
            entry.erase(0, 2);
        } else if (entry.starts_with('.')) {
            entry.erase(0, 1);
        }
        if (entry == "*" || (!entry.empty() && (name == entry || name.ends_with("." + entry)))) {
            return true;
        }
    }
    return false;
}

#if defined(__EMSCRIPTEN__)

// A fetch() under way, by the ID its JavaScript calls back with.
struct Fetch {
    std::string url;
    BodySink sink;
    ResponseCallback done;
    const CancelFlag* cancel = nullptr;
    HttpResponse response;
    bool stopped = false; // the sink stopped it
    std::string buffer;   // the bytes JavaScript last handed over
};

std::map<int, Fetch>& fetches() {
    static std::map<int, Fetch> all;
    return all;
}

bool cancelled(const Fetch& fetch) {
    return fetch.cancel != nullptr && fetch.cancel->load();
}

class HttpTransport final : public Transport {
public:
    explicit HttpTransport(HttpOptions /*options*/) {}

    Result<HttpResponse> post(const HttpRequest& request, const BodySink& /*sink*/,
                              const CancelFlag* /*cancel*/) override {
        return make_error(ErrorCode::PlatformFailure,
                          "can't reach " + request.url +
                              ": the web has no threads to wait on a request; use post_async");
    }

    void post_async(const HttpRequest& request, BodySink sink, ResponseCallback done,
                    const CancelFlag* cancel) override {
        static int next = 1;
        const int id = next++;
        json::Value headers = json::Object{};
        for (const auto& [name, value] : request.headers) {
            headers[name] = value;
        }
        headers["Content-Type"] = request.content_type;
        fetches()[id] = Fetch{.url = request.url,
                              .sink = std::move(sink),
                              .done = std::move(done),
                              .cancel = cancel,
                              .response = {},
                              .stopped = false,
                              .buffer = {}};
        slash_ai_fetch(id, request.url.c_str(), json::write(headers, json::Style::Compact).c_str(),
                       request.body.c_str(), request.timeout_seconds * 1000);
    }

    void later(std::chrono::milliseconds delay, std::function<void()> then) override {
        auto* call = new std::function<void()>(std::move(then));
        emscripten_set_timeout(
            [](void* data) {
                const std::unique_ptr<std::function<void()>> owned(
                    static_cast<std::function<void()>*>(data));
                (*owned)();
            },
            static_cast<double>(delay.count()), call);
    }
};

#else

struct Proxy {
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
};

Result<Proxy> parse_proxy(const std::string& text) {
    std::string url = text.find("://") == std::string::npos ? "http://" + text : text;
    Proxy proxy;
    const std::size_t authority = url.find("://") + 3;
    const std::size_t at = url.find('@', authority);
    if (at != std::string::npos && url.find('/', authority) > at) {
        const std::string userinfo = url.substr(authority, at - authority);
        const std::size_t colon = userinfo.find(':');
        proxy.user = userinfo.substr(0, colon);
        proxy.password = colon == std::string::npos ? "" : userinfo.substr(colon + 1);
        url.erase(authority, at + 1 - authority);
    }
    auto parsed = parse_url(url);
    if (!parsed || parsed.value().scheme != "http") {
        return make_error(ErrorCode::InvalidArgument,
                          "the proxy \"" + text + "\" is not an http://host:port URL");
    }
    proxy.host = parsed.value().host;
    proxy.port = parsed.value().port;
    return proxy;
}

#if defined(__APPLE__)
// Whether the platform trusts a certificate chain (leaf first) for the host, by its own trust
// settings, as Safari would.
bool apple_trusts(const mbedtls_x509_crt* leaf, const std::string& host) {
    CFMutableArrayRef certificates = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const mbedtls_x509_crt* item = leaf; item != nullptr; item = item->next) {
        CFDataRef data = CFDataCreate(nullptr, item->raw.p, static_cast<CFIndex>(item->raw.len));
        SecCertificateRef certificate = SecCertificateCreateWithData(nullptr, data);
        CFRelease(data);
        if (certificate != nullptr) {
            CFArrayAppendValue(certificates, certificate);
            CFRelease(certificate);
        }
    }
    CFStringRef name = CFStringCreateWithCString(nullptr, host.c_str(), kCFStringEncodingUTF8);
    SecPolicyRef policy = SecPolicyCreateSSL(true, name);
    SecTrustRef trust = nullptr;
    const bool trusted =
        CFArrayGetCount(certificates) > 0 &&
        SecTrustCreateWithCertificates(certificates, policy, &trust) == errSecSuccess &&
        SecTrustEvaluateWithError(trust, nullptr);
    if (trust != nullptr) {
        CFRelease(trust);
    }
    CFRelease(policy);
    CFRelease(name);
    CFRelease(certificates);
    return trusted;
}
#endif

// Where Android keeps its root certificates, one file each: the updatable module's since
// Android 14, then the system's.
std::string android_ca_dir() {
#if defined(__ANDROID__)
    for (const char* dir :
         {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"}) {
        if (platform::exists(dir)) {
            return dir;
        }
    }
#endif
    return {};
}

std::string host_for_url(const std::string& host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string reach_error(const std::string& url, httplib::Error error, bool untrusted) {
    if (untrusted) {
        error = httplib::Error::SSLServerVerification;
    }
    switch (error) {
    case httplib::Error::SSLServerVerification:
        return "can't reach " + url +
               ": its certificate is not trusted; if a proxy or private server signs it, set "
               "SSL_CERT_FILE to a PEM file of the certificates to trust";
    case httplib::Error::SSLServerHostnameVerification:
        return "can't reach " + url + ": its certificate is for another host";
    case httplib::Error::SSLLoadingCerts:
        return "can't reach " + url + ": the certificates to trust could not be loaded";
    default:
        return "can't reach " + url + ": " + httplib::to_string(error);
    }
}

class HttpTransport final : public Transport {
public:
    explicit HttpTransport(HttpOptions options) : options_(std::move(options)) {}

    Result<HttpResponse> post(const HttpRequest& request, const BodySink& sink,
                              const CancelFlag* cancel) override {
        auto url = parse_url(request.url);
        if (!url) {
            return url.error();
        }
        const Url& target = url.value();
        const bool secure = target.scheme == "https";
        httplib::Client client(target.scheme + "://" + host_for_url(target.host) + ":" +
                               std::to_string(target.port));
        client.set_connection_timeout(std::chrono::seconds(10));
        client.set_read_timeout(std::chrono::seconds(request.timeout_seconds));
        client.set_write_timeout(std::chrono::seconds(request.timeout_seconds));

        if (const std::string proxy_text = proxy_for(target, options_); !proxy_text.empty()) {
            auto proxy = parse_proxy(proxy_text);
            if (!proxy) {
                return proxy.error();
            }
            client.set_proxy(proxy.value().host, proxy.value().port);
            if (!proxy.value().user.empty()) {
                client.set_proxy_basic_auth(proxy.value().user, proxy.value().password);
            }
        }
        // Set when the platform refuses the server's certificate, which mbedTLS reports only as a
        // failed handshake.
        const auto untrusted = std::make_shared<bool>(false);
        if (secure) {
            std::string ca_file = options_.ca_file;
            if (ca_file.empty()) {
                ca_file = environment(options_, "SSL_CERT_FILE");
            }
            if (!ca_file.empty()) {
                client.set_ca_cert_path(ca_file);
            } else if (const std::string dir = android_ca_dir(); !dir.empty()) {
                client.set_ca_cert_path("", dir);
            } else {
#if defined(__APPLE__)
                // mbedTLS reads the chain; the platform decides whether to trust it. It is
                // asked once, for the server's own certificate, with the chain the server sent.
                client.set_server_certificate_verifier(
                    [host = target.host, untrusted](const httplib::tls::VerifyContext& context) {
                        if (context.depth > 0) {
                            return true;
                        }
                        *untrusted =
                            !apple_trusts(static_cast<const mbedtls_x509_crt*>(context.cert), host);
                        return !*untrusted;
                    });
#endif
            }
        }

        httplib::Request http;
        http.method = "POST";
        http.path = target.path.empty() ? "/" : target.path;
        for (const auto& [name, value] : request.headers) {
            http.set_header(name, value);
        }
        http.set_header("Content-Type", request.content_type);
        http.body = request.body;

        const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
        HttpResponse out;
        bool stopped = false;
        http.response_handler = [&](const httplib::Response& response) {
            out.status = response.status;
            out.retry_after = response.get_header_value("Retry-After");
            return !cancelled();
        };
        http.content_receiver = [&](const char* data, std::size_t size, std::uint64_t /*offset*/,
                                    std::uint64_t /*total*/) {
            if (cancelled()) {
                return false;
            }
            if (out.status == 200 && sink) {
                if (!sink(std::string_view(data, size))) {
                    stopped = true;
                    return false;
                }
                return true;
            }
            out.body.append(data, size);
            return true;
        };

        const httplib::Result result = client.send(http);
        if (cancelled()) {
            return make_error(ErrorCode::PlatformFailure, "the request was cancelled");
        }
        if (stopped) {
            return make_error(ErrorCode::PlatformFailure, "the reply was stopped");
        }
        if (result.error() == httplib::Error::ProxyConnection) {
            // The proxy refused the tunnel to an https:// server; its reply is the result.
            const int status = result ? result->status : 0;
            return make_error(ErrorCode::PlatformFailure,
                              "can't reach " + request.url + ": the proxy answered HTTP " +
                                  std::to_string(status) +
                                  (status == 407 ? "; give its user and password in the proxy "
                                                   "URL, as http://user:password@host:port"
                                                 : ""));
        }
        if (!result) {
            return make_error(ErrorCode::PlatformFailure,
                              reach_error(request.url, result.error(), *untrusted));
        }
        return out;
    }

private:
    HttpOptions options_;
};

#endif

} // namespace

bool retryable(int status) noexcept {
    return status == 408 || status == 409 || status == 429 || status >= 500;
}

std::chrono::milliseconds retry_delay(const HttpResponse& reply, int attempt) {
    int seconds = 0;
    const std::string& after = reply.retry_after;
    const auto [end, error] = std::from_chars(after.data(), after.data() + after.size(), seconds);
    if (!after.empty() && error == std::errc() && end == after.data() + after.size()) {
        return std::chrono::seconds(std::clamp(seconds, 0, 60));
    }
    return std::chrono::seconds(1LL << std::min(attempt, 5));
}

bool wait_for(std::chrono::milliseconds delay, const CancelFlag* cancel) {
    const auto until = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < until) {
        if (cancel != nullptr && cancel->load()) {
            return false;
        }
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(50), until - std::chrono::steady_clock::now()));
    }
    return cancel == nullptr || !cancel->load();
}

Error http_error(std::string_view who, const HttpResponse& reply) {
    // {"error": "..."}, {"error": {"message": "..."}} or {"detail": ...} in the same shapes.
    const auto message_in = [](const json::Value& value) -> std::string {
        if (value.is_string()) {
            return value.as_string();
        }
        if (value.is_object()) {
            if (const json::Value* message = value.find("message");
                message != nullptr && message->is_string()) {
                return message->as_string();
            }
        }
        return {};
    };
    std::string detail;
    if (auto body = json::parse(reply.body); body && body.value().is_object()) {
        for (const std::string_view key : {"error", "detail"}) {
            if (const json::Value* member = body.value().find(key);
                member != nullptr && detail.empty()) {
                detail = message_in(*member);
            }
        }
    }
    if (detail.empty() && reply.body.find('\0') == std::string::npos) {
        detail = reply.body.substr(0, 300);
    }
    ErrorCode code = ErrorCode::PlatformFailure;
    if (reply.status == 400 || reply.status == 413 || reply.status == 422) {
        code = ErrorCode::InvalidArgument;
    } else if (reply.status == 401 || reply.status == 403) {
        code = ErrorCode::PermissionDenied;
    } else if (reply.status == 404) {
        code = ErrorCode::NotFound;
    }
    std::string message = std::string(who) + " answered HTTP " + std::to_string(reply.status);
    if (!detail.empty()) {
        message += ": " + detail;
    }
    return make_error(code, std::move(message));
}

Result<HttpResponse> post_with_retries(Transport& transport, const HttpRequest& request,
                                       std::string_view who, int max_retries,
                                       const CancelFlag* cancel) {
    for (int attempt = 0;; ++attempt) {
        auto sent = transport.post(request, nullptr, cancel);
        if (!sent) {
            return sent.error();
        }
        HttpResponse& reply = sent.value();
        if (reply.status == 200) {
            return std::move(reply);
        }
        if (!retryable(reply.status) || attempt >= max_retries) {
            return http_error(who, reply);
        }
        const std::chrono::milliseconds delay = retry_delay(reply, attempt);
        log::info("retrying a request", {{"to", who},
                                         {"status", reply.status},
                                         {"wait_ms", static_cast<std::int64_t>(delay.count())}});
        if (!wait_for(delay, cancel)) {
            return make_error(ErrorCode::PlatformFailure, "the request was cancelled");
        }
    }
}

std::string proxy_for(const Url& url, const HttpOptions& options) {
    if (is_loopback(url.host)) {
        return {};
    }
    if (!options.proxy.empty()) {
        return options.proxy;
    }
    if (bypasses_proxy(url.host, environment(options, "no_proxy", "NO_PROXY"))) {
        return {};
    }
    return url.scheme == "https" ? environment(options, "https_proxy", "HTTPS_PROXY")
                                 : environment(options, "http_proxy");
}

std::shared_ptr<Transport> make_http_transport(HttpOptions options) {
    return std::make_shared<HttpTransport>(std::move(options));
}

std::shared_ptr<Transport> http_transport() {
    static const std::shared_ptr<Transport> transport = make_http_transport({});
    return transport;
}

std::shared_ptr<Transport> refusing_transport(std::string message) {
    class Refusing final : public Transport {
    public:
        explicit Refusing(std::string message) : message_(std::move(message)) {}

        Result<HttpResponse> post(const HttpRequest& /*request*/, const BodySink& /*sink*/,
                                  const CancelFlag* /*cancel*/) override {
            return make_error(ErrorCode::PermissionDenied, message_);
        }

    private:
        std::string message_;
    };
    return std::make_shared<Refusing>(std::move(message));
}

} // namespace slash::ai

#if defined(__EMSCRIPTEN__)
extern "C" {

// Room for size bytes that JavaScript is about to hand over, or null once the request is gone.
EMSCRIPTEN_KEEPALIVE char* slash_ai_fetch_buffer(int id, int size) {
    const auto found = slash::ai::fetches().find(id);
    if (found == slash::ai::fetches().end()) {
        return nullptr;
    }
    found->second.buffer.resize(static_cast<std::size_t>(size) + 1);
    return found->second.buffer.data();
}

// fetch() has the response's status, and its Retry-After header in the buffer: 1 to read its
// body, 0 to stop.
EMSCRIPTEN_KEEPALIVE int slash_ai_fetch_status(int id, int status, int after_size) {
    const auto found = slash::ai::fetches().find(id);
    if (found == slash::ai::fetches().end()) {
        return 0;
    }
    slash::ai::Fetch& fetch = found->second;
    fetch.response.status = status;
    fetch.response.retry_after = fetch.buffer.substr(0, static_cast<std::size_t>(after_size));
    return slash::ai::cancelled(fetch) ? 0 : 1;
}

// A piece of the body, in the buffer: 1 to go on, 0 to stop.
EMSCRIPTEN_KEEPALIVE int slash_ai_fetch_data(int id, int size) {
    const auto found = slash::ai::fetches().find(id);
    if (found == slash::ai::fetches().end()) {
        return 0;
    }
    slash::ai::Fetch& fetch = found->second;
    if (slash::ai::cancelled(fetch)) {
        return 0;
    }
    const std::string_view piece(fetch.buffer.data(), static_cast<std::size_t>(size));
    if (fetch.response.status == 200 && fetch.sink) {
        if (!fetch.sink(piece)) {
            fetch.stopped = true;
            return 0;
        }
        return 1;
    }
    fetch.response.body.append(piece);
    return 1;
}

// The request is over; if it failed, the buffer says why.
EMSCRIPTEN_KEEPALIVE void slash_ai_fetch_end(int id, int failed, int message_size) {
    auto node = slash::ai::fetches().extract(id);
    if (node.empty()) {
        return;
    }
    slash::ai::Fetch fetch = std::move(node.mapped());
    using slash::ErrorCode;
    if (slash::ai::cancelled(fetch)) {
        fetch.done(slash::make_error(ErrorCode::PlatformFailure, "the request was cancelled"));
    } else if (fetch.stopped) {
        fetch.done(slash::make_error(ErrorCode::PlatformFailure, "the reply was stopped"));
    } else if (failed != 0) {
        fetch.done(
            slash::make_error(ErrorCode::PlatformFailure,
                              "can't reach " + fetch.url + ": " +
                                  fetch.buffer.substr(0, static_cast<std::size_t>(message_size))));
    } else {
        fetch.done(std::move(fetch.response));
    }
}

} // extern "C"
#endif
