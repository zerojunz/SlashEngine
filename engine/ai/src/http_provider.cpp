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

struct HttpProvider::Exchange {
    const HttpProvider* provider = nullptr;
    // Keeps the provider alive while start() waits on the transport.
    std::shared_ptr<const Provider> owner;
    Request request; // fitted to the model
    StreamSink sink;
    bool stream = false;
    HttpRequest http;
    int attempt = 0;

    // The attempt being made: its stream, and the body as it came until the first event, for
    // servers that answer a streamed request with the whole reply.
    std::unique_ptr<StreamState> state;
    SseParser parser;
    std::optional<Error> stream_error;
    bool streamed = false;
    bool evented = false;
    std::string whole;

    // What an attempt came to: the outcome, or how long to wait before the next one.
    struct Outcome {
        std::optional<Result<Response>> result;
        std::chrono::milliseconds retry_after{0};
    };

    void begin() {
        state = stream ? provider->start_stream() : nullptr;
        parser = SseParser();
        stream_error.reset();
        streamed = false;
        evented = false;
        whole.clear();
    }

    [[nodiscard]] BodySink body_sink() {
        if (!stream) {
            return nullptr;
        }
        return [this](std::string_view data) { return feed(data); };
    }

    bool take(const std::vector<SseEvent>& events) {
        return std::ranges::all_of(events, [this](const SseEvent& event) {
            auto taken = state->event(event, sink);
            if (!taken) {
                stream_error = taken.error();
            }
            return taken.ok();
        });
    }

    bool feed(std::string_view data) {
        streamed = true;
        const std::vector<SseEvent> events = parser.feed(data);
        if (!evented && events.empty()) {
            whole.append(data);
        } else if (!evented) {
            evented = true;
            whole.clear();
        }
        return take(events);
    }

    Outcome end(const Result<HttpResponse>& sent) {
        if (stream_error) {
            return {*stream_error};
        }
        if (!sent) {
            return {sent.error()};
        }
        const HttpResponse& reply = sent.value();
        if (reply.status != 200) {
            if (retryable(reply.status) && attempt < provider->options_.max_retries && !streamed) {
                const std::chrono::milliseconds delay = retry_delay(reply, attempt);
                log::info("retrying a model request",
                          {{"provider", provider->type_},
                           {"status", reply.status},
                           {"wait_ms", static_cast<std::int64_t>(delay.count())}});
                ++attempt;
                return {std::nullopt, delay};
            }
            return {http_error("the " + provider->type_ + " provider", reply)};
        }

        Result<Response> response = make_error(ErrorCode::Unknown, "");
        const std::vector<SseEvent> last = stream ? parser.finish() : std::vector<SseEvent>{};
        const bool unstreamed = !stream || (!evented && last.empty());
        if (!unstreamed) {
            if (!take(last) && stream_error) {
                return {*stream_error};
            }
            response = state->finish();
        } else {
            auto body = json::parse(stream ? whole : reply.body);
            if (!body) {
                return {make_error(ErrorCode::PlatformFailure,
                                   "the " + provider->type_ +
                                       " provider's reply is not JSON: " + body.error().message)};
            }
            response = provider->parse(body.value());
        }
        if (!response) {
            return {std::move(response)};
        }
        Response out = std::move(response).value();
        out.message.role = Role::Assistant;
        if (sink && unstreamed && !out.message.text.empty()) {
            sink(out.message.text);
        }
        if (!request.output_schema.is_null() && out.stop != StopReason::ToolCalls) {
            auto output = parse_output(out.message.text, request.output_schema);
            if (!output) {
                return {output.error()};
            }
            out.output = std::move(output).value();
        }
        return {std::move(out)};
    }
};

Result<HttpProvider::Exchange> HttpProvider::prepare(const Request& original,
                                                     StreamSink sink) const {
    auto fitted = fit_request(original, options_.capabilities);
    if (!fitted) {
        return fitted.error();
    }
    Exchange exchange;
    exchange.provider = this;
    exchange.request = std::move(fitted).value();
    exchange.stream = sink && options_.capabilities.streaming;
    exchange.sink = std::move(sink);
    exchange.http = build(exchange.request, exchange.stream);
    exchange.http.timeout_seconds = options_.timeout_seconds;
    for (const auto& header : options_.headers) {
        exchange.http.headers.push_back(header);
    }
    return exchange;
}

Result<Response> HttpProvider::complete(const Request& original, const StreamSink& sink,
                                        const CancelFlag* cancel) {
    auto prepared = prepare(original, sink);
    if (!prepared) {
        return prepared.error();
    }
    Exchange& exchange = prepared.value();
    for (;;) {
        exchange.begin();
        const auto sent = options_.transport->post(exchange.http, exchange.body_sink(), cancel);
        Exchange::Outcome outcome = exchange.end(sent);
        if (outcome.result) {
            return std::move(*outcome.result);
        }
        if (!wait_for(outcome.retry_after, cancel)) {
            return make_error(ErrorCode::PlatformFailure, "the request was cancelled");
        }
    }
}

void HttpProvider::start(const Request& original, StreamSink sink, ReplyCallback done,
                         const CancelFlag* cancel) {
    std::shared_ptr<const Provider> owner = weak_from_this().lock();
    if (owner == nullptr) {
        Provider::start(original, std::move(sink), std::move(done), cancel);
        return;
    }
    auto prepared = prepare(original, std::move(sink));
    if (!prepared) {
        done(prepared.error());
        return;
    }
    auto exchange = std::make_shared<Exchange>(std::move(prepared).value());
    exchange->owner = std::move(owner);
    send(exchange, std::move(done), cancel);
}

void HttpProvider::send(const std::shared_ptr<Exchange>& exchange, ReplyCallback done,
                        const CancelFlag* cancel) {
    exchange->begin();
    BodySink body_sink;
    if (exchange->stream) {
        body_sink = [exchange](std::string_view data) { return exchange->feed(data); };
    }
    const std::shared_ptr<Transport> transport = exchange->provider->options_.transport;
    transport->post_async(
        exchange->http, std::move(body_sink),
        [exchange, transport, done = std::move(done), cancel](const Result<HttpResponse>& sent) {
            Exchange::Outcome outcome = exchange->end(sent);
            if (outcome.result) {
                done(std::move(*outcome.result));
                return;
            }
            transport->later(outcome.retry_after, [exchange, done, cancel] {
                if (cancel != nullptr && cancel->load()) {
                    done(make_error(ErrorCode::PlatformFailure, "the request was cancelled"));
                    return;
                }
                send(exchange, done, cancel);
            });
        },
        cancel);
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
