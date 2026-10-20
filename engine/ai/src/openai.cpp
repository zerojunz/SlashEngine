// OpenAI's chat completions API, which most local and hosted model servers also serve.

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

json::Value message_json(const Message& message) {
    json::Value out;
    out["role"] = to_string(message.role);
    switch (message.role) {
    case Role::System:
        out["content"] = message.text;
        break;
    case Role::User:
        if (message.images.empty()) {
            out["content"] = message.text;
            break;
        }
        for (const Image& image : message.images) {
            json::Value part;
            part["type"] = "image_url";
            part["image_url"]["url"] =
                "data:" + image.media_type + ";base64," + base64_encode(image.data);
            out["content"].push_back(std::move(part));
        }
        if (!message.text.empty()) {
            json::Value part;
            part["type"] = "text";
            part["text"] = message.text;
            out["content"].push_back(std::move(part));
        }
        break;
    case Role::Assistant:
        out["content"] = message.text.empty() && !message.tool_calls.empty()
                             ? json::Value()
                             : json::Value(message.text);
        for (const ToolCall& call : message.tool_calls) {
            json::Value item;
            item["id"] = call.id;
            item["type"] = "function";
            item["function"]["name"] = call.name;
            item["function"]["arguments"] = call.arguments.is_string()
                                                ? call.arguments.as_string()
                                                : json::write(call.arguments, json::Style::Compact);
            out["tool_calls"].push_back(std::move(item));
        }
        break;
    case Role::Tool:
        out["tool_call_id"] = message.tool_call_id;
        out["content"] = message.text;
        break;
    }
    return out;
}

StopReason stop_reason(std::string_view finish) {
    if (finish == "stop") {
        return StopReason::End;
    }
    if (finish == "length") {
        return StopReason::MaxTokens;
    }
    if (finish == "tool_calls" || finish == "function_call") {
        return StopReason::ToolCalls;
    }
    if (finish == "content_filter") {
        return StopReason::Refused;
    }
    return StopReason::Other;
}

class OpenAi final : public HttpProvider {
public:
    explicit OpenAi(AdapterOptions options) : HttpProvider("openai", std::move(options)) {}

    Result<Response> parse(const json::Value& body) const override {
        const json::Value* choices = body.find("choices");
        if (choices == nullptr || !choices->is_array() || choices->as_array().empty()) {
            return make_error(ErrorCode::PlatformFailure,
                              "the openai provider's reply has no choices");
        }
        const json::Value& choice = choices->as_array().front();
        Response response;
        response.model = string_or_empty(body, "model");
        if (const json::Value* usage = body.find("usage"); usage != nullptr) {
            response.usage.input_tokens = int_or_zero(*usage, "prompt_tokens");
            response.usage.output_tokens = int_or_zero(*usage, "completion_tokens");
        }
        response.stop = stop_reason(string_or_empty(choice, "finish_reason"));
        const json::Value* message = choice.find("message");
        if (message == nullptr) {
            return response;
        }
        response.message.text = string_or_empty(*message, "content");
        if (const std::string refusal = string_or_empty(*message, "refusal"); !refusal.empty()) {
            response.message.text = refusal;
            response.stop = StopReason::Refused;
        }
        if (const json::Value* calls = message->find("tool_calls");
            calls != nullptr && calls->is_array()) {
            for (const json::Value& item : calls->as_array()) {
                const json::Value* function = item.find("function");
                if (function == nullptr) {
                    continue;
                }
                ToolCall call{.id = string_or_empty(item, "id"),
                              .name = string_or_empty(*function, "name")};
                const std::string arguments = string_or_empty(*function, "arguments");
                // Arguments that aren't JSON stay as text, so the tool fails with a clear error
                // the model can see and correct.
                auto parsed = json::parse(arguments.empty() ? "{}" : arguments);
                call.arguments = parsed ? std::move(parsed).value() : json::Value(arguments);
                response.message.tool_calls.push_back(std::move(call));
            }
        }
        if (!response.message.tool_calls.empty() && response.stop == StopReason::End) {
            response.stop = StopReason::ToolCalls;
        }
        return response;
    }

protected:
    [[nodiscard]] HttpRequest build(const Request& request, bool stream) const override {
        json::Value body;
        body["model"] = options().model;
        body["messages"] = json::Array{};
        for (const Message& message : request.messages) {
            body["messages"].push_back(message_json(message));
        }
        for (const ToolSpec& tool : request.tools) {
            json::Value item;
            item["type"] = "function";
            item["function"]["name"] = tool.name;
            item["function"]["description"] = tool.description;
            item["function"]["parameters"] = tool.parameters;
            body["tools"].push_back(std::move(item));
        }
        if (const auto tokens = max_tokens(request)) {
            body[options().max_tokens_param] = *tokens;
        }
        if (request.params.temperature) {
            body["temperature"] = *request.params.temperature;
        }
        for (const std::string& stop : request.params.stop) {
            body["stop"].push_back(stop);
        }
        if (request.params.seed) {
            body["seed"] = *request.params.seed;
        }
        if (!request.output_schema.is_null() && options().capabilities.json_schema_output) {
            body["response_format"]["type"] = "json_schema";
            body["response_format"]["json_schema"]["name"] = "output";
            body["response_format"]["json_schema"]["schema"] = request.output_schema;
        }
        if (stream) {
            body["stream"] = true;
            body["stream_options"]["include_usage"] = true;
        }
        HttpRequest http;
        http.url = url("/chat/completions");
        if (!options().api_key.empty()) {
            http.headers.emplace_back("Authorization", "Bearer " + options().api_key);
        }
        http.body = json::write(body, json::Style::Compact);
        return http;
    }

    [[nodiscard]] std::unique_ptr<StreamState> start_stream() const override;
};

// Streamed chunks carry deltas: pieces of the text and of each tool call's arguments, by index.
class OpenAiStream final : public StreamState {
public:
    explicit OpenAiStream(const OpenAi& adapter) : adapter_(&adapter) {}

    Result<void> event(const SseEvent& event, const StreamSink& sink) override {
        if (event.data == "[DONE]") {
            return {};
        }
        auto chunk = json::parse(event.data);
        if (!chunk) {
            return make_error(ErrorCode::PlatformFailure,
                              "the openai provider streamed a chunk that is not JSON: " +
                                  chunk.error().message);
        }
        const json::Value& value = chunk.value();
        if (const json::Value* error = value.find("error"); error != nullptr) {
            return make_error(ErrorCode::PlatformFailure, "the openai provider's stream failed: " +
                                                              string_or_empty(*error, "message"));
        }
        if (const std::string model = string_or_empty(value, "model"); !model.empty()) {
            body_["model"] = model;
        }
        if (const json::Value* usage = value.find("usage");
            usage != nullptr && usage->is_object()) {
            body_["usage"] = *usage;
        }
        const json::Value* choices = value.find("choices");
        if (choices == nullptr || !choices->is_array() || choices->as_array().empty()) {
            return {};
        }
        const json::Value& choice = choices->as_array().front();
        if (const std::string finish = string_or_empty(choice, "finish_reason"); !finish.empty()) {
            finish_ = finish;
        }
        const json::Value* delta = choice.find("delta");
        if (delta == nullptr) {
            return {};
        }
        if (const std::string text = string_or_empty(*delta, "content"); !text.empty()) {
            text_ += text;
            sink(text);
        }
        refusal_ += string_or_empty(*delta, "refusal");
        if (const json::Value* calls = delta->find("tool_calls");
            calls != nullptr && calls->is_array()) {
            for (const json::Value& item : calls->as_array()) {
                Call& call = calls_[int_or_zero(item, "index")];
                if (const std::string id = string_or_empty(item, "id"); !id.empty()) {
                    call.id = id;
                }
                if (const json::Value* function = item.find("function"); function != nullptr) {
                    call.name += string_or_empty(*function, "name");
                    call.arguments += string_or_empty(*function, "arguments");
                }
            }
        }
        return {};
    }

    Result<Response> finish() override {
        body_["choices"] = json::Array(1);
        json::Value& choice = body_["choices"].as_array().front();
        choice["finish_reason"] = finish_;
        json::Value& message = choice["message"];
        message["content"] = text_;
        if (!refusal_.empty()) {
            message["refusal"] = refusal_;
        }
        for (const auto& [index, call] : calls_) {
            json::Value item;
            item["id"] = call.id;
            item["function"]["name"] = call.name;
            item["function"]["arguments"] = call.arguments;
            message["tool_calls"].push_back(std::move(item));
        }
        return adapter_->parse(body_);
    }

private:
    struct Call {
        std::string id;
        std::string name;
        std::string arguments;
    };

    const OpenAi* adapter_;
    json::Value body_ = json::Object{{"choices", json::Array{}}};
    std::string text_;
    std::string refusal_;
    std::string finish_;
    std::map<std::int64_t, Call> calls_;
};

std::unique_ptr<StreamState> OpenAi::start_stream() const {
    return std::make_unique<OpenAiStream>(*this);
}

} // namespace

std::shared_ptr<Provider> make_openai(AdapterOptions options) {
    return std::make_shared<OpenAi>(std::move(options));
}

} // namespace slash::ai::detail
