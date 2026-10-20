#include "slash/ai/transport.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <httplib.h>
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

#if defined(__EMSCRIPTEN__)

class HttpTransport final : public Transport {
public:
    Result<HttpResponse> post(const HttpRequest& request, const BodySink& /*sink*/,
                              const CancelFlag* /*cancel*/) override {
        return make_error(ErrorCode::PlatformFailure,
                          "can't reach " + request.url + ": web builds have no HTTP client yet");
    }
};

#else

class HttpTransport final : public Transport {
public:
    Result<HttpResponse> post(const HttpRequest& request, const BodySink& sink,
                              const CancelFlag* cancel) override {
        auto url = parse_url(request.url);
        if (!url) {
            return url.error();
        }
        if (url.value().scheme == "https") {
            return make_error(ErrorCode::PlatformFailure,
                              "can't reach " + request.url +
                                  ": this build of the engine has no TLS, so it reaches only "
                                  "http:// servers, such as a model server on this machine");
        }
        httplib::Client client(url.value().host, url.value().port);
        client.set_connection_timeout(std::chrono::seconds(10));
        client.set_read_timeout(std::chrono::seconds(request.timeout_seconds));
        client.set_write_timeout(std::chrono::seconds(request.timeout_seconds));

        httplib::Request http;
        http.method = "POST";
        http.path = url.value().path.empty() ? "/" : url.value().path;
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
        if (!result) {
            return make_error(ErrorCode::PlatformFailure, "can't reach " + request.url + ": " +
                                                              httplib::to_string(result.error()));
        }
        return out;
    }
};

#endif

} // namespace

std::shared_ptr<Transport> http_transport() {
    static const std::shared_ptr<Transport> transport = std::make_shared<HttpTransport>();
    return transport;
}

} // namespace slash::ai
