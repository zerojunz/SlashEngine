#include "http_provider.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace slash::ai {
namespace detail {
namespace {

// Statuses worth trying again: timeouts, conflicts, rate limits and overloaded servers.
bool retryable(int status) {
    return status == 408 || status == 409 || status == 429 || status >= 500;
}

// How long to wait before attempt + 1: what Retry-After says, at most a minute, or 1, 2, 4...
// seconds.
std::chrono::milliseconds retry_delay(const HttpResponse& reply, int attempt) {
    int seconds = 0;
    const std::string& after = reply.retry_after;
    const auto [end, error] = std::from_chars(after.data(), after.data() + after.size(), seconds);
    if (!after.empty() && error == std::errc() && end == after.data() + after.size()) {
        return std::chrono::seconds(std::clamp(seconds, 0, 60));
    }
    return std::chrono::seconds(1LL << std::min(attempt, 5));
}

// Waits, a slice at a time so a cancel stops it. False if cancelled.
bool wait(std::chrono::milliseconds delay, const CancelFlag* cancel) {
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

// An error reply as an engine error, with the message the provider gave.
Error http_error(std::string_view type, const HttpResponse& reply) {
    std::string detail;
    if (auto body = json::parse(reply.body); body) {
        if (const json::Value* error = body.value().find("error"); error != nullptr) {
            detail = error->is_string() ? error->as_string() : string_or_empty(*error, "message");
        }
    }
    if (detail.empty()) {
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
    std::string message =
        "the " + std::string(type) + " provider answered HTTP " + std::to_string(reply.status);
    if (!detail.empty()) {
        message += ": " + detail;
    }
    return make_error(code, std::move(message));
}

} // namespace

HttpProvider::HttpProvider(std::string_view type, AdapterOptions options)
    : type_(type), options_(std::move(options)) {
    if (options_.transport == nullptr) {
        options_.transport = http_transport();
    }
}

std::string HttpProvider::url(std::string_view path) const {
    std::string base = options_.url.empty() ? std::string(default_url(type_)) : options_.url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + std::string(path);
}

std::optional<std::int64_t> HttpProvider::max_tokens(const Request& request) const {
    return request.params.max_tokens ? request.params.max_tokens : options_.max_tokens;
}

Result<Response> HttpProvider::complete(const Request& original, const StreamSink& sink,
                                        const CancelFlag* cancel) {
    auto fitted = fit_request(original, options_.capabilities);
    if (!fitted) {
        return fitted.error();
    }
    const Request& request = fitted.value();
    const bool stream = sink && options_.capabilities.streaming;
    HttpRequest http = build(request, stream);
    http.timeout_seconds = options_.timeout_seconds;
    for (const auto& header : options_.headers) {
        http.headers.push_back(header);
    }

    for (int attempt = 0;; ++attempt) {
        std::unique_ptr<StreamState> state;
        SseParser parser;
        std::optional<Error> stream_error;
        bool streamed = false;
        // The body as it came until the first event, for servers that answer a streamed request
        // with the whole reply.
        std::string whole;
        bool evented = false;
        BodySink body_sink;
        const auto take = [&](const std::vector<SseEvent>& events) {
            for (const SseEvent& event : events) {
                if (auto taken = state->event(event, sink); !taken) {
                    stream_error = taken.error();
                    return false;
                }
            }
            return true;
        };
        if (stream) {
            state = start_stream();
            body_sink = [&](std::string_view data) {
                streamed = true;
                const std::vector<SseEvent> events = parser.feed(data);
                if (!evented && events.empty()) {
                    whole.append(data);
                } else if (!evented) {
                    evented = true;
                    whole.clear();
                }
                return take(events);
            };
        }

        auto sent = options_.transport->post(http, body_sink, cancel);
        if (stream_error) {
            return *stream_error;
        }
        if (!sent) {
            return sent.error();
        }
        const HttpResponse& reply = sent.value();
        if (reply.status != 200) {
            if (retryable(reply.status) && attempt < options_.max_retries && !streamed) {
                const std::chrono::milliseconds delay = retry_delay(reply, attempt);
                log::info("retrying a model request",
                          {{"provider", type_},
                           {"status", reply.status},
                           {"wait_ms", static_cast<std::int64_t>(delay.count())}});
                if (!wait(delay, cancel)) {
                    return make_error(ErrorCode::PlatformFailure, "the request was cancelled");
                }
                continue;
            }
            return http_error(type_, reply);
        }

        Result<Response> response = make_error(ErrorCode::Unknown, "");
        const std::vector<SseEvent> last = stream ? parser.finish() : std::vector<SseEvent>{};
        const bool unstreamed = !stream || (!evented && last.empty());
        if (!unstreamed) {
            if (!take(last) && stream_error) {
                return *stream_error;
            }
            response = state->finish();
        } else {
            auto body = json::parse(stream ? whole : reply.body);
            if (!body) {
                return make_error(ErrorCode::PlatformFailure,
                                  "the " + type_ +
                                      " provider's reply is not JSON: " + body.error().message);
            }
            response = parse(body.value());
        }
        if (!response) {
            return response;
        }
        Response out = std::move(response).value();
        out.message.role = Role::Assistant;
        if (sink && unstreamed && !out.message.text.empty()) {
            sink(out.message.text);
        }
        if (!request.output_schema.is_null() && out.stop != StopReason::ToolCalls) {
            auto output = parse_output(out.message.text, request.output_schema);
            if (!output) {
                return output.error();
            }
            out.output = std::move(output).value();
        }
        return out;
    }
}

json::Value argument_object(const json::Value& arguments) {
    return arguments.is_object() ? arguments : json::Value(json::Object{});
}

std::string string_or_empty(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    return member != nullptr && member->is_string() ? member->as_string() : std::string();
}

std::int64_t int_or_zero(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    return member != nullptr && member->is_integral() ? member->as_int() : 0;
}

} // namespace detail

std::string_view default_url(std::string_view type) noexcept {
    if (type == "anthropic") {
        return "https://api.anthropic.com";
    }
    if (type == "gemini") {
        return "https://generativelanguage.googleapis.com/v1beta";
    }
    return "https://api.openai.com/v1";
}

Result<std::shared_ptr<Provider>> make_adapter(std::string_view type, AdapterOptions options) {
    if (type == "openai") {
        return detail::make_openai(std::move(options));
    }
    if (type == "anthropic") {
        return detail::make_anthropic(std::move(options));
    }
    if (type == "gemini") {
        return detail::make_gemini(std::move(options));
    }
    std::string message = "unknown provider type \"" + std::string(type) + "\"";
    if (const std::string_view hint = closest_match(type, kAdapterTypes); !hint.empty()) {
        message += "; did you mean \"" + std::string(hint) + "\"?";
    }
    return make_error(ErrorCode::InvalidArgument, message + " (anthropic, gemini or openai)");
}

} // namespace slash::ai
