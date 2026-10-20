// Google's Gemini API (generateContent).

#include "http_provider.hpp"

#include "slash/core/text.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai::detail {
namespace {

bool is_native(const Message& message) {
    const json::Value* type = message.native.find("type");
    return type != nullptr && type->is_string() && type->as_string() == "gemini" &&
           message.native.find("parts") != nullptr;
}

StopReason stop_reason(std::string_view finish) {
    if (finish == "STOP") {
        return StopReason::End;
    }
    if (finish == "MAX_TOKENS") {
        return StopReason::MaxTokens;
    }
    if (finish == "SAFETY" || finish == "RECITATION" || finish == "BLOCKLIST" ||
        finish == "PROHIBITED_CONTENT" || finish == "SPII" || finish == "IMAGE_SAFETY") {
        return StopReason::Refused;
    }
    return StopReason::Other;
}

// The function name and, if the model gave one, the ID of the call a tool result answers, found
// in the assistant message that made the call.
struct CallInfo {
    std::string name;
    std::string id{}; // NOLINT(readability-redundant-member-init): none for made-up IDs
};

CallInfo find_call(const std::vector<Message>& messages, std::size_t before,
                   const std::string& call_id) {
    for (std::size_t i = before; i-- > 0;) {
        const Message& message = messages[i];
        if (message.role != Role::Assistant) {
            continue;
        }
        for (const ToolCall& call : message.tool_calls) {
            if (call.id != call_id) {
                continue;
            }
            CallInfo info{.name = call.name};
            // IDs the adapter made up for calls without one aren't sent back.
            if (is_native(message)) {
                for (const json::Value& part : message.native.find("parts")->as_array()) {
                    const json::Value* function = part.find("functionCall");
                    if (function != nullptr && string_or_empty(*function, "id") == call_id) {
                        info.id = call_id;
                    }
                }
            }
            return info;
        }
    }
    return {};
}

json::Value function_response(const Message& message, const CallInfo& call) {
    json::Value part;
    json::Value& response = part["functionResponse"];
    response["name"] = call.name;
    if (!call.id.empty()) {
        response["id"] = call.id;
    }
    auto parsed = json::parse(message.text);
    json::Value result = parsed ? std::move(parsed).value() : json::Value(message.text);
    if (message.is_error) {
        response["response"]["error"] = std::move(result);
    } else if (result.is_object()) {
        response["response"] = std::move(result);
    } else {
        response["response"]["output"] = std::move(result);
    }
    return part;
}

class Gemini final : public HttpProvider {
public:
    explicit Gemini(AdapterOptions options) : HttpProvider("gemini", std::move(options)) {}

    Result<Response> parse(const json::Value& body) const override {
        Response response;
        response.model = string_or_empty(body, "modelVersion");
        if (const json::Value* usage = body.find("usageMetadata"); usage != nullptr) {
            response.usage.input_tokens = int_or_zero(*usage, "promptTokenCount");
            response.usage.output_tokens = int_or_zero(*usage, "candidatesTokenCount") +
                                           int_or_zero(*usage, "thoughtsTokenCount");
        }
        const json::Value* candidates = body.find("candidates");
        if (candidates == nullptr || !candidates->is_array() || candidates->as_array().empty()) {
            const json::Value* feedback = body.find("promptFeedback");
            if (feedback != nullptr && !string_or_empty(*feedback, "blockReason").empty()) {
                response.stop = StopReason::Refused;
                return response;
            }
            return make_error(ErrorCode::PlatformFailure,
                              "the gemini provider's reply has no candidates");
        }
        const json::Value& candidate = candidates->as_array().front();
        response.stop = stop_reason(string_or_empty(candidate, "finishReason"));
        const json::Value* content = candidate.find("content");
        const json::Value* parts = content != nullptr ? content->find("parts") : nullptr;
        if (parts == nullptr || !parts->is_array()) {
            return response;
        }
        for (const json::Value& part : parts->as_array()) {
            const json::Value* thought = part.find("thought");
            const bool is_thought = thought != nullptr && thought->is_bool() && thought->as_bool();
            if (!is_thought) {
                response.message.text += string_or_empty(part, "text");
            }
            if (const json::Value* function = part.find("functionCall"); function != nullptr) {
                ToolCall call{.id = string_or_empty(*function, "id"),
                              .name = string_or_empty(*function, "name")};
                if (call.id.empty()) {
                    call.id = "call_" + std::to_string(response.message.tool_calls.size());
                }
                if (const json::Value* args = function->find("args"); args != nullptr) {
                    call.arguments = *args;
                }
                response.message.tool_calls.push_back(std::move(call));
            }
        }
        if (!response.message.tool_calls.empty() && response.stop == StopReason::End) {
            response.stop = StopReason::ToolCalls;
        }
        response.message.native["type"] = "gemini";
        response.message.native["parts"] = *parts;
        return response;
    }

protected:
    [[nodiscard]] HttpRequest build(const Request& request, bool stream) const override {
        json::Value body;
        std::string system;
        json::Array contents;
        const auto add_user_parts = [&contents](json::Array parts) {
            if (!contents.empty() && contents.back()["role"].as_string() == "user") {
                for (json::Value& part : parts) {
                    contents.back()["parts"].push_back(std::move(part));
                }
                return;
            }
            json::Value item;
            item["role"] = "user";
            item["parts"] = std::move(parts);
            contents.push_back(std::move(item));
        };
        for (std::size_t i = 0; i < request.messages.size(); ++i) {
            const Message& message = request.messages[i];
            switch (message.role) {
            case Role::System:
                system += (system.empty() ? "" : "\n\n") + message.text;
                break;
            case Role::User: {
                json::Array parts;
                for (const Image& image : message.images) {
                    json::Value part;
                    part["inlineData"]["mimeType"] = image.media_type;
                    part["inlineData"]["data"] = base64_encode(image.data);
                    parts.push_back(std::move(part));
                }
                if (!message.text.empty()) {
                    json::Value part;
                    part["text"] = message.text;
                    parts.push_back(std::move(part));
                }
                add_user_parts(std::move(parts));
                break;
            }
            case Role::Tool:
                add_user_parts(json::Array{function_response(
                    message, find_call(request.messages, i, message.tool_call_id))});
                break;
            case Role::Assistant: {
                json::Value item;
                item["role"] = "model";
                // The parts as the API sent them keep their thought signatures, which the API
                // wants back.
                if (is_native(message)) {
                    item["parts"] = *message.native.find("parts");
                } else {
                    item["parts"] = json::Array{};
                    if (!message.text.empty()) {
                        json::Value part;
                        part["text"] = message.text;
                        item["parts"].push_back(std::move(part));
                    }
                    for (const ToolCall& call : message.tool_calls) {
                        json::Value part;
                        part["functionCall"]["name"] = call.name;
                        part["functionCall"]["args"] = argument_object(call.arguments);
                        item["parts"].push_back(std::move(part));
                    }
                }
                if (item["parts"].size() > 0) {
                    contents.push_back(std::move(item));
                }
                break;
            }
            }
        }
        body["contents"] = std::move(contents);
        if (!system.empty()) {
            json::Value part;
            part["text"] = system;
            body["systemInstruction"]["parts"].push_back(std::move(part));
        }
        if (!request.tools.empty()) {
            json::Value declarations;
            for (const ToolSpec& tool : request.tools) {
                json::Value item;
                item["name"] = tool.name;
                item["description"] = tool.description;
                item["parametersJsonSchema"] = tool.parameters;
                declarations["functionDeclarations"].push_back(std::move(item));
            }
            body["tools"].push_back(std::move(declarations));
        }
        json::Value config = json::Object{};
        if (const auto tokens = max_tokens(request)) {
            config["maxOutputTokens"] = *tokens;
        }
        if (request.params.temperature) {
            config["temperature"] = *request.params.temperature;
        }
        for (const std::string& stop : request.params.stop) {
            config["stopSequences"].push_back(stop);
        }
        if (request.params.seed) {
            config["seed"] = *request.params.seed;
        }
        if (!request.output_schema.is_null() && options().capabilities.json_schema_output) {
            config["responseMimeType"] = "application/json";
            config["responseJsonSchema"] = request.output_schema;
        }
        if (config.size() > 0) {
            body["generationConfig"] = std::move(config);
        }
        HttpRequest http;
        http.url = url("/models/" + options().model +
                       (stream ? ":streamGenerateContent?alt=sse" : ":generateContent"));
        if (!options().api_key.empty()) {
            http.headers.emplace_back("x-goog-api-key", options().api_key);
        }
        http.body = json::write(body, json::Style::Compact);
        return http;
    }

    [[nodiscard]] std::unique_ptr<StreamState> start_stream() const override;
};

// Each streamed event is a whole reply holding the next parts; text parts are joined.
class GeminiStream final : public StreamState {
public:
    explicit GeminiStream(const Gemini& adapter) : adapter_(&adapter) {}

    Result<void> event(const SseEvent& event, const StreamSink& sink) override {
        auto parsed = json::parse(event.data);
        if (!parsed) {
            return make_error(ErrorCode::PlatformFailure,
                              "the gemini provider streamed an event that is not JSON: " +
                                  parsed.error().message);
        }
        const json::Value& value = parsed.value();
        if (const json::Value* error = value.find("error"); error != nullptr) {
            return make_error(ErrorCode::PlatformFailure, "the gemini provider's stream failed: " +
                                                              string_or_empty(*error, "message"));
        }
        for (const char* key : {"usageMetadata", "modelVersion", "promptFeedback"}) {
            if (const json::Value* member = value.find(key); member != nullptr) {
                body_[key] = *member;
            }
        }
        const json::Value* candidates = value.find("candidates");
        if (candidates == nullptr || !candidates->is_array() || candidates->as_array().empty()) {
            return {};
        }
        const json::Value& candidate = candidates->as_array().front();
        if (const std::string finish = string_or_empty(candidate, "finishReason");
            !finish.empty()) {
            finish_ = finish;
        }
        const json::Value* content = candidate.find("content");
        const json::Value* parts = content != nullptr ? content->find("parts") : nullptr;
        if (parts == nullptr || !parts->is_array()) {
            return {};
        }
        for (const json::Value& part : parts->as_array()) {
            const bool plain_text = part.size() == 1 && part.find("text") != nullptr;
            if (plain_text) {
                sink(string_or_empty(part, "text"));
                if (!parts_.empty() && parts_.back().find("text") != nullptr &&
                    parts_.back().find("functionCall") == nullptr &&
                    parts_.back().find("thought") == nullptr) {
                    json::Value& last = parts_.back();
                    last["text"] = string_or_empty(last, "text") + string_or_empty(part, "text");
                    continue;
                }
            } else if (const json::Value* thought = part.find("thought");
                       (thought == nullptr || !thought->is_bool() || !thought->as_bool()) &&
                       part.find("text") != nullptr) {
                sink(string_or_empty(part, "text"));
            }
            parts_.push_back(part);
        }
        return {};
    }

    Result<Response> finish() override {
        json::Value candidate;
        candidate["content"]["role"] = "model";
        candidate["content"]["parts"] = std::move(parts_);
        candidate["finishReason"] = finish_;
        body_["candidates"].push_back(std::move(candidate));
        return adapter_->parse(body_);
    }

private:
    const Gemini* adapter_;
    json::Value body_;
    json::Array parts_;
    std::string finish_;
};

std::unique_ptr<StreamState> Gemini::start_stream() const {
    return std::make_unique<GeminiStream>(*this);
}

} // namespace

std::shared_ptr<Provider> make_gemini(AdapterOptions options) {
    return std::make_shared<Gemini>(std::move(options));
}

} // namespace slash::ai::detail
