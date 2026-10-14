#include "slash/server/client.hpp"

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

#include <chrono>
#include <httplib.h>

namespace slash::server {
namespace {

// A JSON-RPC error for a request that could not be delivered, or "" for a notification.
std::string undeliverable(std::string_view message, const std::string& reason) {
    auto parsed = json::parse(message);
    const json::Value* id = parsed ? parsed.value().find("id") : nullptr;
    if (id == nullptr) {
        return {};
    }
    json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = *id;
    response["error"]["code"] = command::code::kInternalError;
    response["error"]["message"] = reason;
    return json::write(response, json::Style::Compact);
}

} // namespace

Result<HttpResponse> send(const Endpoint& endpoint, const HttpRequest& request) {
    httplib::Client client(endpoint.host, endpoint.port);
    client.set_connection_timeout(std::chrono::seconds(2));
    client.set_read_timeout(std::chrono::seconds(120));
    httplib::Headers headers;
    if (!endpoint.token.empty()) {
        headers.emplace("Authorization", "Bearer " + endpoint.token);
    }
    for (const auto& [name, value] : request.headers) {
        headers.emplace(name, value);
    }
    const httplib::Result result =
        request.method == "GET"
            ? client.Get(request.path, headers)
            : client.Post(request.path, headers, request.body, "application/json");
    if (!result) {
        return make_error(ErrorCode::PlatformFailure, "could not reach the engine at " +
                                                          endpoint.url() + ": " +
                                                          httplib::to_string(result.error()));
    }
    return HttpResponse{result->status, result->body, result->get_header_value("Content-Type")};
}

std::string forward_mcp(const Result<Endpoint>& endpoint, std::string_view message) {
    if (!endpoint) {
        return undeliverable(message, endpoint.error().message);
    }
    HttpRequest request;
    request.path = "/mcp";
    request.body = std::string(message);
    request.headers = {{"Accept", "application/json, text/event-stream"}};
    auto response = send(endpoint.value(), request);
    if (!response) {
        return undeliverable(message, response.error().message +
                                          "; is the game still running? Start it with "
                                          "\"slash-cli run --serve\" or \"slash-cli serve\"");
    }
    const HttpResponse& reply = response.value();
    if (reply.status == 200) {
        std::string body = reply.body;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
            body.pop_back();
        }
        return body;
    }
    if (reply.status == 202 || reply.status == 204) {
        return {};
    }
    std::string reason = "the engine answered HTTP " + std::to_string(reply.status);
    if (auto parsed = json::parse(reply.body); parsed) {
        if (const json::Value* error = parsed.value().find("error"); error != nullptr) {
            if (const json::Value* text = error->find("message");
                text != nullptr && text->is_string()) {
                reason += ": " + text->as_string();
            }
        }
    }
    return undeliverable(message, reason);
}

} // namespace slash::server
