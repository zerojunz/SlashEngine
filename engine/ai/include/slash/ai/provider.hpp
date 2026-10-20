#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Calling language models through one interface, so no engine code names a vendor. A feature
// builds a Request and sends it to a Provider; adapters translate it for OpenAI-compatible
// servers, Anthropic's Messages API and Google's Gemini API (slash/ai/adapters.hpp), providers.json
// says which model each role uses (slash/ai/config.hpp), and ReplayProvider records replies once
// and replays them so tests run offline (slash/ai/replay.hpp).
namespace slash::ai {

enum class Role : std::uint8_t {
    System,    // instructions for the model
    User,      // the person or game asking
    Assistant, // the model's replies
    Tool,      // the result of a tool the model called
};

[[nodiscard]] std::string_view to_string(Role role) noexcept;
[[nodiscard]] std::optional<Role> role_from_string(std::string_view name) noexcept;

// An image for a model that takes them (Capabilities::vision).
struct Image {
    std::string media_type; // "image/png", "image/jpeg", "image/gif" or "image/webp"
    std::string data;       // the file's bytes
};

// A model asking for a tool to be called.
struct ToolCall {
    std::string id; // a Tool message with this tool_call_id answers it
    std::string name;
    // As the tool's parameters schema describes; an empty object, not null.
    json::Value arguments = json::Object{}; // NOLINT(readability-redundant-member-init)
};

// Members left out of a designated initializer, such as {.role = Role::User, .text = "Hi"}, are
// empty.
struct Message {
    Role role = Role::User;
    std::string text{};                 // NOLINT(readability-redundant-member-init)
    std::vector<Image> images{};        // NOLINT(readability-redundant-member-init): User messages
    std::vector<ToolCall> tool_calls{}; // NOLINT(readability-redundant-member-init): Assistant
    std::string tool_call_id{};         // NOLINT(readability-redundant-member-init): Tool messages
    bool is_error = false;              // Tool messages: the call failed
    // The reply as the provider sent it, for providers that need parts of it back unchanged in
    // later requests (a model's reasoning and its signatures): {"type": <adapter type>, ...}.
    // Only an adapter of that type reads it, so a conversation can move to another provider.
    json::Value native{}; // NOLINT(readability-redundant-member-init)
};

// A tool the model may call: a name of letters, digits, '_' and '-', what it does, and a JSON
// Schema for its arguments.
struct ToolSpec {
    std::string name;
    std::string description{}; // NOLINT(readability-redundant-member-init)
    json::Value parameters{};  // NOLINT(readability-redundant-member-init)
};

struct Params {
    std::optional<std::int64_t> max_tokens;
    std::optional<double> temperature;
    std::vector<std::string> stop;
    std::optional<std::int64_t> seed; // sent to providers that take one
};

struct Request {
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    // A JSON Schema the reply must match; the response's output is the reply parsed. Null for
    // a reply in plain text.
    json::Value output_schema;
    Params params;
};

enum class StopReason : std::uint8_t {
    End,          // the model finished
    ToolCalls,    // the model wants tools called
    MaxTokens,    // cut off at max_tokens
    StopSequence, // reached one of the stop strings
    Refused,      // the provider or the model declined
    Other,
};

[[nodiscard]] std::string_view to_string(StopReason reason) noexcept;
[[nodiscard]] std::optional<StopReason> stop_reason_from_string(std::string_view name) noexcept;

struct Usage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
};

struct Response {
    Message message;    // the reply: Role::Assistant with its text, tool calls and native form
    json::Value output; // the reply parsed, when the request had an output_schema
    StopReason stop = StopReason::End;
    Usage usage;
    std::string model;     // the model that answered, as the provider names it
    bool replayed = false; // read from a replay file; no model was called
};

// What a model can do. Features ask for capabilities, not vendors: without json_schema_output
// the schema goes in the prompt instead, and a request needing anything else the model lacks
// fails with a clear error.
struct Capabilities {
    bool tools = true;
    bool json_schema_output = true;
    bool vision = true;
    bool streaming = true;
    std::int64_t context_tokens = 0; // 0 if unknown
};

// Receives a reply's text as it arrives, on the thread running Provider::complete.
using StreamSink = std::function<void(std::string_view text)>;

// Set from another thread to stop a request; complete() then fails soon after.
using CancelFlag = std::atomic<bool>;

class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual const Capabilities& capabilities() const = 0;

    // Sends the request and waits for the whole reply. With a sink, the reply streams where the
    // model can, and the sink gets each piece of text as it comes. Safe to call from several
    // threads at once.
    [[nodiscard]] virtual Result<Response> complete(const Request& request,
                                                    const StreamSink& sink = nullptr,
                                                    const CancelFlag* cancel = nullptr) = 0;
};

// What a request's output_schema asks of the reply: an error if the request needs something the
// model lacks (tools, images), and for a model without native schema output, the request with
// the schema written into its system message.
[[nodiscard]] Result<Request> fit_request(const Request& request, const Capabilities& capabilities);

// Parses a reply's text as the request's output_schema asks, allowing a ```json fence around
// it, and checks it against the schema. Errors name the first mismatch's JSON Pointer.
[[nodiscard]] Result<json::Value> parse_output(std::string_view text, const json::Value& schema);

// Requests, responses and messages as JSON, as ai.complete takes and returns them and replay
// files store them. Images are base64; "native" is kept as is.
[[nodiscard]] json::Value to_json(const Message& message);
[[nodiscard]] json::Value to_json(const Request& request);
[[nodiscard]] json::Value to_json(const Response& response);
[[nodiscard]] Result<Message> message_from_json(const json::Value& value);
[[nodiscard]] Result<Request> request_from_json(const json::Value& value);
[[nodiscard]] Result<Response> response_from_json(const json::Value& value);

// A request running on its own thread, so a game keeps ticking while a model answers: start it,
// then check done() each tick. Destroying a Call cancels the request without waiting: its thread
// stops when the request notices, on its own.
class Call {
public:
    // With stream, text() grows as the reply arrives.
    Call(std::shared_ptr<Provider> provider, Request request, bool stream = false);
    ~Call();

    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call(Call&&) = delete;
    Call& operator=(Call&&) = delete;

    [[nodiscard]] bool done() const noexcept;
    // The reply's text so far.
    [[nodiscard]] std::string text() const;
    // The outcome once done(), otherwise nullptr.
    [[nodiscard]] const Result<Response>* result() const noexcept;
    // Blocks until the reply ends, for headless runs that must repeat exactly.
    void wait();

private:
    // What the thread fills in, kept alive by the thread too, so a Call can go first.
    struct Shared {
        mutable std::mutex mutex;
        std::string text;
        std::optional<Result<Response>> result;
        std::atomic<bool> done = false;
        CancelFlag cancel = false;
    };

    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::thread thread_;
};

} // namespace slash::ai
