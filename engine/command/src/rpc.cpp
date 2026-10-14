#include "slash/command/rpc.hpp"

#include "slash/command/session.hpp"

#include <optional>
#include <utility>

namespace slash::command::rpc {
namespace {

json::Value error_response(const json::Value& id, int error_code, std::string message,
                           json::Value data = {}) {
    json::Value error;
    error["code"] = error_code;
    error["message"] = std::move(message);
    if (!data.is_null()) {
        error["data"] = std::move(data);
    }
    json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = std::move(error);
    return response;
}

// Handles one request object. Returns nothing for a notification.
std::optional<json::Value> handle_one(const json::Value& request, const Method& method) {
    const json::Value null_id;
    if (!request.is_object()) {
        return error_response(null_id, code::kInvalidRequest, "a request must be an object");
    }
    const json::Value* id = request.find("id");
    const bool valid_id =
        id == nullptr || id->is_null() || id->is_string() || id->is_int() || id->is_double();
    const json::Value& reply_id = id != nullptr && valid_id ? *id : null_id;
    const json::Value* version = request.find("jsonrpc");
    if (version == nullptr || !version->is_string() || version->as_string() != "2.0") {
        return error_response(reply_id, code::kInvalidRequest, R"("jsonrpc" must be "2.0")");
    }
    if (!valid_id) {
        return error_response(null_id, code::kInvalidRequest,
                              "\"id\" must be a string, a number or null");
    }
    const json::Value* name = request.find("method");
    if (name == nullptr || !name->is_string()) {
        return error_response(reply_id, code::kInvalidRequest, "\"method\" must be a string");
    }
    const json::Value* params = request.find("params");
    if (params != nullptr && !params->is_object()) {
        if (id == nullptr) {
            return std::nullopt;
        }
        return error_response(reply_id, code::kInvalidParams,
                              "params must be an object of named parameters");
    }

    Reply reply = method(name->as_string(), params != nullptr ? *params : json::Value());
    if (id == nullptr) {
        return std::nullopt;
    }
    if (!reply.ok) {
        return error_response(reply_id, reply.code, std::move(reply.message),
                              std::move(reply.data));
    }
    json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = reply_id;
    response["result"] = std::move(reply.value);
    return response;
}

} // namespace

std::string handle(std::string_view message, const Method& method) {
    auto parsed = json::parse(message);
    if (!parsed) {
        return json::write(
            error_response({}, code::kParseError, "parse error: " + parsed.error().message),
            json::Style::Compact);
    }
    const json::Value& request = parsed.value();
    if (!request.is_array()) {
        const auto response = handle_one(request, method);
        return response ? json::write(*response, json::Style::Compact) : std::string();
    }
    if (request.size() == 0) {
        return json::write(error_response({}, code::kInvalidRequest, "a batch cannot be empty"),
                           json::Style::Compact);
    }
    json::Array responses;
    for (const json::Value& item : request.as_array()) {
        if (auto response = handle_one(item, method)) {
            responses.push_back(std::move(*response));
        }
    }
    return responses.empty() ? std::string()
                             : json::write(json::Value(std::move(responses)), json::Style::Compact);
}

std::string handle(Session& session, std::string_view message) {
    return handle(message, [&session](std::string_view name, const json::Value& params) {
        return session.call(name, params);
    });
}

} // namespace slash::command::rpc
