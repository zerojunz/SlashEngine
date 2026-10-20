#pragma once

#include "slash/ai/adapters.hpp"
#include "slash/ai/provider.hpp"
#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// What the three adapters share: fitting a request to the model, sending it with retries,
// reading a streamed reply event by event, turning error replies into errors and checking the
// reply against an output schema. Each adapter says how to build its request and read its reply.
namespace slash::ai::detail {

// Adds up a streamed reply, event by event.
class StreamState {
public:
    virtual ~StreamState() = default;
    // Takes one event; the text it adds goes to the sink.
    [[nodiscard]] virtual Result<void> event(const SseEvent& event, const StreamSink& sink) = 0;
    // The reply, once the stream has ended.
    [[nodiscard]] virtual Result<Response> finish() = 0;
};

class HttpProvider : public Provider {
public:
    HttpProvider(std::string_view type, AdapterOptions options);

    [[nodiscard]] const Capabilities& capabilities() const final { return options_.capabilities; }
    [[nodiscard]] Result<Response> complete(const Request& original, const StreamSink& sink,
                                            const CancelFlag* cancel) final;

    // Reads a whole reply's body.
    [[nodiscard]] virtual Result<Response> parse(const json::Value& body) const = 0;

protected:
    // The HTTP request for a request already fitted to the model's capabilities.
    [[nodiscard]] virtual HttpRequest build(const Request& request, bool stream) const = 0;
    [[nodiscard]] virtual std::unique_ptr<StreamState> start_stream() const = 0;

    [[nodiscard]] const AdapterOptions& options() const noexcept { return options_; }
    // The base URL, without a trailing '/', followed by path.
    [[nodiscard]] std::string url(std::string_view path) const;
    // The request's max_tokens, or the provider's.
    [[nodiscard]] std::optional<std::int64_t> max_tokens(const Request& request) const;

private:
    std::string type_;
    AdapterOptions options_;
};

// A tool call's arguments as an object, for APIs that take nothing else.
[[nodiscard]] json::Value argument_object(const json::Value& arguments);

// The members a reply JSON has, read leniently: missing or mistyped members read as empty.
[[nodiscard]] std::string string_or_empty(const json::Value& value, std::string_view key);
[[nodiscard]] std::int64_t int_or_zero(const json::Value& value, std::string_view key);

[[nodiscard]] std::shared_ptr<Provider> make_openai(AdapterOptions options);
[[nodiscard]] std::shared_ptr<Provider> make_anthropic(AdapterOptions options);
[[nodiscard]] std::shared_ptr<Provider> make_gemini(AdapterOptions options);

} // namespace slash::ai::detail
