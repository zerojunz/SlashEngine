#include "slash/ai/provider.hpp"

#include "slash/core/json_schema.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai {
namespace {

constexpr std::array kRoles{Role::System, Role::User, Role::Assistant, Role::Tool};
constexpr std::array kStopReasons{StopReason::End,       StopReason::ToolCalls,
                                  StopReason::MaxTokens, StopReason::StopSequence,
                                  StopReason::Refused,   StopReason::Other};

Error invalid(std::string_view path, std::string message) {
    return make_error(ErrorCode::InvalidArgument,
                      (path.empty() ? std::string() : std::string(path) + ": ") +
                          std::move(message));
}

const std::string* string_member(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    return member != nullptr && member->is_string() ? &member->as_string() : nullptr;
}

std::string_view trim(std::string_view text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

} // namespace

std::string_view to_string(Role role) noexcept {
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    case Role::Tool:
        return "tool";
    }
    return "user";
}

std::optional<Role> role_from_string(std::string_view name) noexcept {
    for (const Role role : kRoles) {
        if (to_string(role) == name) {
            return role;
        }
    }
    return std::nullopt;
}

std::string_view to_string(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::End:
        return "end";
    case StopReason::ToolCalls:
        return "tool_calls";
    case StopReason::MaxTokens:
        return "max_tokens";
    case StopReason::StopSequence:
        return "stop_sequence";
    case StopReason::Refused:
        return "refused";
    case StopReason::Other:
        return "other";
    }
    return "other";
}

std::optional<StopReason> stop_reason_from_string(std::string_view name) noexcept {
    for (const StopReason reason : kStopReasons) {
        if (to_string(reason) == name) {
            return reason;
        }
    }
    return std::nullopt;
}

Result<Request> fit_request(const Request& request, const Capabilities& capabilities) {
    if (!capabilities.tools && !request.tools.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the request offers tools, but this provider's model can't call them "
                          "(its capabilities.tools is false)");
    }
    if (!capabilities.vision) {
        for (const Message& message : request.messages) {
            if (!message.images.empty()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the request has images, but this provider's model can't see "
                                  "them (its capabilities.vision is false)");
            }
        }
    }
    Request fitted = request;
    if (!request.output_schema.is_null() && !capabilities.json_schema_output) {
        const std::string instruction =
            "Reply with only a JSON value that matches this JSON Schema, with no other text:\n" +
            json::write(request.output_schema, json::Style::Compact);
        if (!fitted.messages.empty() && fitted.messages.front().role == Role::System) {
            fitted.messages.front().text += "\n\n" + instruction;
        } else {
            fitted.messages.insert(fitted.messages.begin(),
                                   Message{.role = Role::System, .text = instruction});
        }
    }
    return fitted;
}

Result<json::Value> parse_output(std::string_view text, const json::Value& schema) {
    std::string_view body = trim(text);
    // Models without native schema output often wrap JSON in a Markdown fence.
    if (body.starts_with("```")) {
        const std::size_t line_end = body.find('\n');
        body = line_end == std::string_view::npos ? std::string_view() : body.substr(line_end + 1);
        body = trim(body);
        if (body.ends_with("```")) {
            body.remove_suffix(3);
        }
        body = trim(body);
    }
    auto parsed = json::parse(body);
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          "the model's reply is not JSON: " + parsed.error().message);
    }
    const std::vector<json::ValidationError> errors = json::validate(parsed.value(), schema);
    if (!errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          "the model's reply does not match output_schema: " + first.message +
                              (first.path.empty() ? std::string() : " at " + first.path));
    }
    return std::move(parsed).value();
}

json::Value to_json(const Message& message) {
    json::Value out;
    out["role"] = to_string(message.role);
    if (!message.text.empty()) {
        out["text"] = message.text;
    }
    for (const Image& image : message.images) {
        json::Value item;
        item["media_type"] = image.media_type;
        item["data"] = base64_encode(image.data);
        out["images"].push_back(std::move(item));
    }
    for (const ToolCall& call : message.tool_calls) {
        json::Value item;
        item["id"] = call.id;
        item["name"] = call.name;
        item["arguments"] = call.arguments;
        out["tool_calls"].push_back(std::move(item));
    }
    if (!message.tool_call_id.empty()) {
        out["tool_call_id"] = message.tool_call_id;
    }
    if (message.is_error) {
        out["is_error"] = true;
    }
    if (!message.native.is_null()) {
        out["native"] = message.native;
    }
    return out;
}

json::Value to_json(const Request& request) {
    json::Value out;
    out["messages"] = json::Array{};
    for (const Message& message : request.messages) {
        out["messages"].push_back(to_json(message));
    }
    for (const ToolSpec& tool : request.tools) {
        json::Value item;
        item["name"] = tool.name;
        item["description"] = tool.description;
        item["parameters"] = tool.parameters;
        out["tools"].push_back(std::move(item));
    }
    if (!request.output_schema.is_null()) {
        out["output_schema"] = request.output_schema;
    }
    if (request.params.max_tokens) {
        out["max_tokens"] = *request.params.max_tokens;
    }
    if (request.params.temperature) {
        out["temperature"] = *request.params.temperature;
    }
    for (const std::string& stop : request.params.stop) {
        out["stop"].push_back(stop);
    }
    if (request.params.seed) {
        out["seed"] = *request.params.seed;
    }
    return out;
}

json::Value to_json(const Response& response) {
    json::Value out;
    out["message"] = to_json(response.message);
    if (!response.output.is_null()) {
        out["output"] = response.output;
    }
    out["stop"] = to_string(response.stop);
    out["usage"]["input_tokens"] = response.usage.input_tokens;
    out["usage"]["output_tokens"] = response.usage.output_tokens;
    if (!response.model.empty()) {
        out["model"] = response.model;
    }
    return out;
}

Result<Message> message_from_json(const json::Value& value) {
    if (!value.is_object()) {
        return invalid("", "a message is an object");
    }
    Message message;
    const std::string* role = string_member(value, "role");
    if (role == nullptr) {
        return invalid("/role", "a message needs a role");
    }
    const auto parsed_role = role_from_string(*role);
    if (!parsed_role) {
        return invalid("/role",
                       "unknown role \"" + *role + "\"; use system, user, assistant or tool");
    }
    message.role = *parsed_role;
    if (const std::string* text = string_member(value, "text"); text != nullptr) {
        message.text = *text;
    }
    if (const json::Value* images = value.find("images"); images != nullptr && images->is_array()) {
        for (std::size_t i = 0; i < images->size(); ++i) {
            const json::Value& item = images->as_array()[i];
            const std::string path = "/images/" + std::to_string(i);
            const std::string* media_type = string_member(item, "media_type");
            const std::string* data = string_member(item, "data");
            if (media_type == nullptr || data == nullptr) {
                return invalid(path, "an image needs media_type and data");
            }
            auto bytes = base64_decode(*data);
            if (!bytes) {
                return invalid(path + "/data", "not base64");
            }
            message.images.push_back({*media_type, std::move(*bytes)});
        }
    }
    if (const json::Value* calls = value.find("tool_calls");
        calls != nullptr && calls->is_array()) {
        for (std::size_t i = 0; i < calls->size(); ++i) {
            const json::Value& item = calls->as_array()[i];
            const std::string* id = string_member(item, "id");
            const std::string* name = string_member(item, "name");
            if (id == nullptr || name == nullptr) {
                return invalid("/tool_calls/" + std::to_string(i),
                               "a tool call needs an id and a name");
            }
            ToolCall call{.id = *id, .name = *name};
            if (const json::Value* arguments = item.find("arguments"); arguments != nullptr) {
                call.arguments = *arguments;
            }
            message.tool_calls.push_back(std::move(call));
        }
    }
    if (const std::string* id = string_member(value, "tool_call_id"); id != nullptr) {
        message.tool_call_id = *id;
    }
    if (message.role == Role::Tool && message.tool_call_id.empty()) {
        return invalid("/tool_call_id", "a tool message needs the tool_call_id it answers");
    }
    if (const json::Value* is_error = value.find("is_error"); is_error != nullptr) {
        message.is_error = is_error->is_bool() && is_error->as_bool();
    }
    if (const json::Value* native = value.find("native"); native != nullptr) {
        message.native = *native;
    }
    return message;
}

Result<Request> request_from_json(const json::Value& value) {
    if (!value.is_object()) {
        return invalid("", "a request is an object");
    }
    Request request;
    if (const json::Value* messages = value.find("messages");
        messages != nullptr && messages->is_array()) {
        for (std::size_t i = 0; i < messages->size(); ++i) {
            auto message = message_from_json(messages->as_array()[i]);
            if (!message) {
                return invalid("/messages/" + std::to_string(i), message.error().message);
            }
            request.messages.push_back(std::move(message).value());
        }
    }
    if (const json::Value* tools = value.find("tools"); tools != nullptr && tools->is_array()) {
        for (std::size_t i = 0; i < tools->size(); ++i) {
            const json::Value& item = tools->as_array()[i];
            const std::string* name = string_member(item, "name");
            if (name == nullptr) {
                return invalid("/tools/" + std::to_string(i), "a tool needs a name");
            }
            ToolSpec tool{.name = *name};
            if (const std::string* description = string_member(item, "description");
                description != nullptr) {
                tool.description = *description;
            }
            if (const json::Value* parameters = item.find("parameters"); parameters != nullptr) {
                tool.parameters = *parameters;
            }
            request.tools.push_back(std::move(tool));
        }
    }
    if (const json::Value* schema = value.find("output_schema"); schema != nullptr) {
        request.output_schema = *schema;
    }
    if (const json::Value* max_tokens = value.find("max_tokens");
        max_tokens != nullptr && max_tokens->is_integral()) {
        request.params.max_tokens = max_tokens->as_int();
    }
    if (const json::Value* temperature = value.find("temperature");
        temperature != nullptr && temperature->is_number()) {
        request.params.temperature = temperature->as_double();
    }
    if (const json::Value* stop = value.find("stop"); stop != nullptr && stop->is_array()) {
        for (const json::Value& item : stop->as_array()) {
            if (item.is_string()) {
                request.params.stop.push_back(item.as_string());
            }
        }
    }
    if (const json::Value* seed = value.find("seed"); seed != nullptr && seed->is_integral()) {
        request.params.seed = seed->as_int();
    }
    return request;
}

Result<Response> response_from_json(const json::Value& value) {
    if (!value.is_object()) {
        return invalid("", "a response is an object");
    }
    Response response;
    if (const json::Value* message = value.find("message"); message != nullptr) {
        auto parsed = message_from_json(*message);
        if (!parsed) {
            return invalid("/message", parsed.error().message);
        }
        response.message = std::move(parsed).value();
    }
    if (const json::Value* output = value.find("output"); output != nullptr) {
        response.output = *output;
    }
    if (const std::string* stop = string_member(value, "stop"); stop != nullptr) {
        response.stop = stop_reason_from_string(*stop).value_or(StopReason::Other);
    }
    if (const json::Value* usage = value.find("usage"); usage != nullptr) {
        if (const json::Value* input = usage->find("input_tokens");
            input != nullptr && input->is_integral()) {
            response.usage.input_tokens = input->as_int();
        }
        if (const json::Value* output = usage->find("output_tokens");
            output != nullptr && output->is_integral()) {
            response.usage.output_tokens = output->as_int();
        }
    }
    if (const std::string* model = string_member(value, "model"); model != nullptr) {
        response.model = *model;
    }
    return response;
}

Call::Call(std::shared_ptr<Provider> provider, Request request, bool stream)
    : provider_(std::move(provider)) {
    auto run = [this, request = std::move(request), stream] {
        StreamSink sink;
        if (stream) {
            sink = [this](std::string_view text) {
                const std::scoped_lock lock(mutex_);
                text_.append(text);
            };
        }
        Result<Response> result = provider_->complete(request, sink, &cancel_);
        const std::scoped_lock lock(mutex_);
        if (result) {
            text_ = result.value().message.text;
        }
        result_.emplace(std::move(result));
        done_.store(true, std::memory_order_release);
    };
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    run();
#else
    thread_ = std::thread(std::move(run));
#endif
}

Call::~Call() {
    cancel_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Call::done() const noexcept {
    return done_.load(std::memory_order_acquire);
}

std::string Call::text() const {
    const std::scoped_lock lock(mutex_);
    return text_;
}

const Result<Response>* Call::result() const noexcept {
    // result_ is set before done_ and never changes after.
    return done() && result_.has_value() ? &*result_ : nullptr;
}

} // namespace slash::ai
