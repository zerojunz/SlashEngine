#include "slash/ai/transport.hpp"

#include "slash/platform/process.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include "slash/platform/file.hpp"

#include <httplib.h>
#include <mbedtls/x509_crt.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
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

class HttpTransport final : public Transport {
public:
    explicit HttpTransport(HttpOptions /*options*/) {}

    Result<HttpResponse> post(const HttpRequest& request, const BodySink& /*sink*/,
                              const CancelFlag* /*cancel*/) override {
        return make_error(ErrorCode::PlatformFailure,
                          "can't reach " + request.url + ": web builds have no HTTP client yet");
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
        http.set_header("Content-Type", "application/json");
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
