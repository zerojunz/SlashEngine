// OpenAI's Responses API, which OpenAI's newer reasoning models need for tools.

#include "http_provider.hpp"

#include "slash/core/text.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::ai::detail {
namespace {

constexpr std::string_view kType = "openai-responses";

// The output items a reply sent, kept so its reasoning goes back with it.
const json::Value* native_output(const Message& message) {
    const json::Value* type = message.native.find("type");
    if (type == nullptr || !type->is_string() || type->as_string() != kType) {
        return nullptr;
    }
    const json::Value* output = message.native.find("output");
    return output != nullptr && output->is_array() ? output : nullptr;
}

json::Value text_part(std::string_view type, const std::string& text) {
    json::Value part;
    part["type"] = type;
    part["text"] = text;
    return part;
}

// Adds a message to the input as the API's items: the role's content, or an assistant's output
// items as they came, or its text and function calls.
void add_items(const Message& message, json::Array& input) {
    switch (message.role) {
    case Role::System: {
        json::Value item;
        item["role"] = "developer";
        item["content"] = message.text;
        input.push_back(std::move(item));
        return;
    }
    case Role::User: {
        json::Value item;
        item["role"] = "user";
        item["content"] = json::Array{};
        for (const Image& image : message.images) {
            json::Value part;
            part["type"] = "input_image";
            part["image_url"] = "data:" + image.media_type + ";base64," + base64_encode(image.data);
            item["content"].push_back(std::move(part));
        }
        if (!message.text.empty()) {
            item["content"].push_back(text_part("input_text", message.text));
        }
        input.push_back(std::move(item));
        return;
    }
    case Role::Assistant: {
        if (const json::Value* output = native_output(message); output != nullptr) {
            for (const json::Value& item : output->as_array()) {
                input.push_back(item);
            }
            return;
        }
        if (!message.text.empty()) {
            json::Value item;
            item["role"] = "assistant";
            item["content"] = json::Array{text_part("output_text", message.text)};
            input.push_back(std::move(item));
        }
        for (const ToolCall& call : message.tool_calls) {
            json::Value item;
            item["type"] = "function_call";
            item["call_id"] = call.id;
            item["name"] = call.name;
            item["arguments"] = call.arguments.is_string()
                                    ? call.arguments.as_string()
                                    : json::write(call.arguments, json::Style::Compact);
            input.push_back(std::move(item));
        }
        return;
    }
    case Role::Tool: {
        json::Value item;
        item["type"] = "function_call_output";
        item["call_id"] = message.tool_call_id;
        item["output"] = message.text;
        input.push_back(std::move(item));
        return;
    }
    }
}

class OpenAiResponses final : public HttpProvider {
public:
    explicit OpenAiResponses(AdapterOptions options) : HttpProvider(kType, std::move(options)) {}

    Result<Response> parse(const json::Value& body) const override {
        const json::Value* output = body.find("output");
        if (output == nullptr || !output->is_array()) {
            return make_error(ErrorCode::PlatformFailure,
                              "the openai-responses provider's reply has no output");
        }
        Response response;
        response.model = string_or_empty(body, "model");
        if (const json::Value* usage = body.find("usage"); usage != nullptr) {
            response.usage.input_tokens = int_or_zero(*usage, "input_tokens");
            response.usage.output_tokens = int_or_zero(*usage, "output_tokens");
        }
        response.stop = StopReason::End;
        if (string_or_empty(body, "status") == "incomplete") {
            const json::Value* details = body.find("incomplete_details");
            const std::string reason =
                details != nullptr ? string_or_empty(*details, "reason") : std::string();
            response.stop = StopReason::Other;
            if (reason == "max_output_tokens") {
                response.stop = StopReason::MaxTokens;
            } else if (reason == "content_filter") {
                response.stop = StopReason::Refused;
            }
        }
        bool refused = false;
        for (const json::Value& item : output->as_array()) {
            const std::string type = string_or_empty(item, "type");
            if (type == "message") {
                const json::Value* content = item.find("content");
                if (content == nullptr || !content->is_array()) {
                    continue;
                }
                for (const json::Value& part : content->as_array()) {
                    const std::string part_type = string_or_empty(part, "type");
                    if (part_type == "output_text") {
                        response.message.text += string_or_empty(part, "text");
                    } else if (part_type == "refusal") {
                        response.message.text += string_or_empty(part, "refusal");
                        refused = true;
                    }
                }
            } else if (type == "function_call") {
                ToolCall call{.id = string_or_empty(item, "call_id"),
                              .name = string_or_empty(item, "name")};
                const std::string arguments = string_or_empty(item, "arguments");
                // Arguments that aren't JSON stay as text, so the tool fails with a clear error
                // the model can see and correct.
                auto parsed = json::parse(arguments.empty() ? "{}" : arguments);
                call.arguments = parsed ? std::move(parsed).value() : json::Value(arguments);
                response.message.tool_calls.push_back(std::move(call));
            }
        }
        if (refused) {
            response.stop = StopReason::Refused;
        } else if (!response.message.tool_calls.empty() && response.stop == StopReason::End) {
            response.stop = StopReason::ToolCalls;
        }
        response.message.native["type"] = kType;
        response.message.native["output"] = *output;
        return response;
    }

protected:
    [[nodiscard]] HttpRequest build(const Request& request, bool stream) const override {
        json::Value body;
        body["model"] = options().model;
        json::Array input;
        for (const Message& message : request.messages) {
            add_items(message, input);
        }
        body["input"] = std::move(input);
        for (const ToolSpec& tool : request.tools) {
            json::Value item;
            item["type"] = "function";
            item["name"] = tool.name;
            item["description"] = tool.description;
            item["parameters"] = tool.parameters;
            item["strict"] = false;
            body["tools"].push_back(std::move(item));
        }
        if (const auto tokens = max_tokens(request)) {
            body["max_output_tokens"] = *tokens;
        }
        if (request.params.temperature) {
            body["temperature"] = *request.params.temperature;
        }
        if (!request.output_schema.is_null() && options().capabilities.json_schema_output) {
            json::Value& format = body["text"]["format"];
            format["type"] = "json_schema";
            format["name"] = "output";
            format["schema"] = request.output_schema;
            format["strict"] = false;
        }
        // Nothing is kept on OpenAI's side: each reply's reasoning comes back encrypted and goes
        // back in the next request, as the conversation is sent whole each time.
        body["store"] = false;
        body["include"] = json::Array{"reasoning.encrypted_content"};
        if (stream) {
            body["stream"] = true;
        }
        HttpRequest http;
        http.url = url("/responses");
        if (!options().api_key.empty()) {
            http.headers.emplace_back("Authorization", "Bearer " + options().api_key);
        }
        http.body = json::write(body, json::Style::Compact);
        return http;
    }

    [[nodiscard]] std::unique_ptr<StreamState> start_stream() const override;
};

// Streamed events carry the text as it comes, then the whole response when it ends.
class ResponsesStream final : public StreamState {
public:
    explicit ResponsesStream(const OpenAiResponses& adapter) : adapter_(&adapter) {}

    Result<void> event(const SseEvent& event, const StreamSink& sink) override {
        auto parsed = json::parse(event.data);
        if (!parsed) {
            return make_error(ErrorCode::PlatformFailure,
                              "the openai-responses provider streamed an event that is not JSON: " +
                                  parsed.error().message);
        }
        const json::Value& value = parsed.value();
        const std::string type = string_or_empty(value, "type");
        if (type == "response.output_text.delta" || type == "response.refusal.delta") {
            if (const std::string delta = string_or_empty(value, "delta"); !delta.empty()) {
                sink(delta);
            }
        } else if (type == "response.completed" || type == "response.incomplete") {
            if (const json::Value* response = value.find("response"); response != nullptr) {
                response_ = *response;
            }
        } else if (type == "response.failed" || type == "error") {
            const json::Value* response = value.find("response");
            const json::Value* error = response != nullptr ? response->find("error") : nullptr;
            if (error == nullptr) {
                error = value.find("error");
            }
            const std::string message = error != nullptr ? string_or_empty(*error, "message")
                                                         : string_or_empty(value, "message");
            return make_error(ErrorCode::PlatformFailure,
                              "the openai-responses provider's stream failed: " + message);
        }
        return {};
    }

    Result<Response> finish() override {
        if (!response_) {
            return make_error(ErrorCode::PlatformFailure,
                              "the openai-responses provider's stream ended before the response");
        }
        return adapter_->parse(*response_);
    }

private:
    const OpenAiResponses* adapter_;
    std::optional<json::Value> response_;
};

std::unique_ptr<StreamState> OpenAiResponses::start_stream() const {
    return std::make_unique<ResponsesStream>(*this);
}

} // namespace

std::shared_ptr<Provider> make_openai_responses(AdapterOptions options) {
    return std::make_shared<OpenAiResponses>(std::move(options));
}

} // namespace slash::ai::detail
