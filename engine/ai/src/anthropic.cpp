// Anthropic's Messages API.

#include "http_provider.hpp"

#include "slash/core/text.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace slash::ai::detail {
namespace {

constexpr std::int64_t kDefaultMaxTokens = 16000;

StopReason stop_reason(std::string_view stop) {
    if (stop == "end_turn") {
        return StopReason::End;
    }
    if (stop == "tool_use") {
        return StopReason::ToolCalls;
    }
    if (stop == "max_tokens" || stop == "model_context_window_exceeded") {
        return StopReason::MaxTokens;
    }
    if (stop == "stop_sequence") {
        return StopReason::StopSequence;
    }
    if (stop == "refusal") {
        return StopReason::Refused;
    }
    return StopReason::Other;
}

json::Value text_block(const std::string& text) {
    json::Value block;
    block["type"] = "text";
    block["text"] = text;
    return block;
}

// The content blocks of a message the user side sends: images, then text, or a tool's result.
json::Array user_blocks(const Message& message) {
    json::Array blocks;
    if (message.role == Role::Tool) {
        json::Value block;
        block["type"] = "tool_result";
        block["tool_use_id"] = message.tool_call_id;
        block["content"] = message.text;
        if (message.is_error) {
            block["is_error"] = true;
        }
        blocks.push_back(std::move(block));
        return blocks;
    }
    for (const Image& image : message.images) {
        json::Value block;
        block["type"] = "image";
        block["source"]["type"] = "base64";
        block["source"]["media_type"] = image.media_type;
        block["source"]["data"] = base64_encode(image.data);
        blocks.push_back(std::move(block));
    }
    if (!message.text.empty()) {
        blocks.push_back(text_block(message.text));
    }
    return blocks;
}

// An assistant message's content: the blocks the API sent, unchanged, so its thinking blocks
// and their signatures go back as the API requires; otherwise its text and tool calls.
json::Value assistant_content(const Message& message) {
    if (const json::Value* type = message.native.find("type");
        type != nullptr && type->is_string() && type->as_string() == "anthropic") {
        if (const json::Value* content = message.native.find("content"); content != nullptr) {
            return *content;
        }
    }
    json::Array blocks;
    if (!message.text.empty()) {
        blocks.push_back(text_block(message.text));
    }
    for (const ToolCall& call : message.tool_calls) {
        json::Value block;
        block["type"] = "tool_use";
        block["id"] = call.id;
        block["name"] = call.name;
        block["input"] = argument_object(call.arguments);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

class Anthropic final : public HttpProvider {
public:
    explicit Anthropic(AdapterOptions options) : HttpProvider("anthropic", std::move(options)) {}

    Result<Response> parse(const json::Value& body) const override {
        Response response;
        response.model = string_or_empty(body, "model");
        response.stop = stop_reason(string_or_empty(body, "stop_reason"));
        if (const json::Value* usage = body.find("usage"); usage != nullptr) {
            response.usage.input_tokens = int_or_zero(*usage, "input_tokens") +
                                          int_or_zero(*usage, "cache_creation_input_tokens") +
                                          int_or_zero(*usage, "cache_read_input_tokens");
            response.usage.output_tokens = int_or_zero(*usage, "output_tokens");
        }
        const json::Value* content = body.find("content");
        if (content == nullptr || !content->is_array()) {
            return make_error(ErrorCode::PlatformFailure,
                              "the anthropic provider's reply has no content");
        }
        for (const json::Value& block : content->as_array()) {
            const std::string type = string_or_empty(block, "type");
            if (type == "text") {
                response.message.text += string_or_empty(block, "text");
            } else if (type == "tool_use") {
                ToolCall call{.id = string_or_empty(block, "id"),
                              .name = string_or_empty(block, "name")};
                if (const json::Value* input = block.find("input"); input != nullptr) {
                    call.arguments = *input;
                }
                response.message.tool_calls.push_back(std::move(call));
            }
        }
        response.message.native["type"] = "anthropic";
        response.message.native["content"] = *content;
        return response;
    }

protected:
    [[nodiscard]] HttpRequest build(const Request& request, bool stream) const override {
        json::Value body;
        body["model"] = options().model;
        body["max_tokens"] = max_tokens(request).value_or(kDefaultMaxTokens);
        std::string system;
        json::Array messages;
        for (const Message& message : request.messages) {
            if (message.role == Role::System) {
                system += (system.empty() ? "" : "\n\n") + message.text;
                continue;
            }
            if (message.role == Role::Assistant) {
                json::Value content = assistant_content(message);
                if (content.size() == 0) {
                    continue; // the API refuses empty messages
                }
                json::Value item;
                item["role"] = "assistant";
                item["content"] = std::move(content);
                messages.push_back(std::move(item));
                continue;
            }
            // Tool results and what the user says next go in one user message, as the API
            // expects all of a turn's results together.
            json::Array blocks = user_blocks(message);
            if (!messages.empty() && messages.back()["role"].as_string() == "user") {
                for (json::Value& block : blocks) {
                    messages.back()["content"].push_back(std::move(block));
                }
                continue;
            }
            json::Value item;
            item["role"] = "user";
            item["content"] = std::move(blocks);
            messages.push_back(std::move(item));
        }
        body["messages"] = std::move(messages);
        if (!system.empty()) {
            body["system"] = system;
        }
        for (const ToolSpec& tool : request.tools) {
            json::Value item;
            item["name"] = tool.name;
            item["description"] = tool.description;
            item["input_schema"] = tool.parameters;
            body["tools"].push_back(std::move(item));
        }
        if (request.params.temperature) {
            body["temperature"] = *request.params.temperature;
        }
        for (const std::string& stop : request.params.stop) {
            body["stop_sequences"].push_back(stop);
        }
        if (!request.output_schema.is_null() && options().capabilities.json_schema_output) {
            body["output_config"]["format"]["type"] = "json_schema";
            body["output_config"]["format"]["schema"] = request.output_schema;
        }
        if (stream) {
            body["stream"] = true;
        }
        HttpRequest http;
        http.url = url("/v1/messages");
        if (!options().api_key.empty()) {
            http.headers.emplace_back("x-api-key", options().api_key);
        }
        http.headers.emplace_back("anthropic-version", "2023-06-01");
        http.body = json::write(body, json::Style::Compact);
        return http;
    }

    [[nodiscard]] std::unique_ptr<StreamState> start_stream() const override;
};

// Rebuilds the message the stream describes, block by block, then reads it as a whole reply.
class AnthropicStream final : public StreamState {
public:
    explicit AnthropicStream(const Anthropic& adapter) : adapter_(&adapter) {}

    Result<void> event(const SseEvent& event, const StreamSink& sink) override {
        auto parsed = json::parse(event.data);
        if (!parsed) {
            return make_error(ErrorCode::PlatformFailure,
                              "the anthropic provider streamed an event that is not JSON: " +
                                  parsed.error().message);
        }
        const json::Value& value = parsed.value();
        const std::string type = string_or_empty(value, "type");
        if (type == "message_start") {
            if (const json::Value* message = value.find("message"); message != nullptr) {
                message_["model"] = string_or_empty(*message, "model");
                if (const json::Value* usage = message->find("usage"); usage != nullptr) {
                    message_["usage"] = *usage;
                }
            }
        } else if (type == "content_block_start") {
            const json::Value* block = value.find("content_block");
            if (block != nullptr) {
                blocks_[int_or_zero(value, "index")] = *block;
            }
        } else if (type == "content_block_delta") {
            const std::int64_t index = int_or_zero(value, "index");
            const json::Value* delta = value.find("delta");
            if (delta == nullptr) {
                return {};
            }
            json::Value& block = blocks_[index];
            const std::string delta_type = string_or_empty(*delta, "type");
            if (delta_type == "text_delta") {
                const std::string text = string_or_empty(*delta, "text");
                block["text"] = string_or_empty(block, "text") + text;
                sink(text);
            } else if (delta_type == "input_json_delta") {
                inputs_[index] += string_or_empty(*delta, "partial_json");
            } else if (delta_type == "thinking_delta") {
                block["thinking"] =
                    string_or_empty(block, "thinking") + string_or_empty(*delta, "thinking");
            } else if (delta_type == "signature_delta") {
                block["signature"] =
                    string_or_empty(block, "signature") + string_or_empty(*delta, "signature");
            } else if (delta_type == "citations_delta") {
                if (const json::Value* citation = delta->find("citation"); citation != nullptr) {
                    block["citations"].push_back(*citation);
                }
            }
        } else if (type == "content_block_stop") {
            const std::int64_t index = int_or_zero(value, "index");
            if (const auto input = inputs_.find(index); input != inputs_.end()) {
                auto arguments = json::parse(input->second.empty() ? "{}" : input->second);
                if (!arguments) {
                    return make_error(ErrorCode::PlatformFailure,
                                      "the anthropic provider streamed tool input that is not "
                                      "JSON: " +
                                          arguments.error().message);
                }
                blocks_[index]["input"] = std::move(arguments).value();
            }
        } else if (type == "message_delta") {
            if (const json::Value* delta = value.find("delta"); delta != nullptr) {
                if (const std::string stop = string_or_empty(*delta, "stop_reason");
                    !stop.empty()) {
                    message_["stop_reason"] = stop;
                }
            }
            if (const json::Value* usage = value.find("usage"); usage != nullptr) {
                message_["usage"]["output_tokens"] = int_or_zero(*usage, "output_tokens");
            }
        } else if (type == "error") {
            const json::Value* error = value.find("error");
            return make_error(
                ErrorCode::PlatformFailure,
                "the anthropic provider's stream failed: " +
                    (error != nullptr ? string_or_empty(*error, "message") : event.data));
        }
        return {};
    }

    Result<Response> finish() override {
        message_["content"] = json::Array{};
        for (auto& [index, block] : blocks_) {
            message_["content"].push_back(std::move(block));
        }
        return adapter_->parse(message_);
    }

private:
    const Anthropic* adapter_;
    json::Value message_;
    std::map<std::int64_t, json::Value> blocks_;
    std::map<std::int64_t, std::string> inputs_; // tool input JSON, as it streams
};

std::unique_ptr<StreamState> Anthropic::start_stream() const {
    return std::make_unique<AnthropicStream>(*this);
}

} // namespace

std::shared_ptr<Provider> make_anthropic(AdapterOptions options) {
    return std::make_shared<Anthropic>(std::move(options));
}

} // namespace slash::ai::detail
