#pragma once

#include "slash/core/result.hpp"
#include "slash/server/server.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::server {

struct HttpRequest {
    std::string method = "POST"; // "GET" or "POST"
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string content_type;
};

// Sends one request to a server on this machine, adding the endpoint's bearer token. Fails only
// when no response arrives, e.g. because nothing is listening.
[[nodiscard]] Result<HttpResponse> send(const Endpoint& endpoint, const HttpRequest& request);

// Forwards one MCP message to a server's /mcp endpoint and returns the response line, or "" when
// none is due. If the endpoint is missing or doesn't answer, a request gets a JSON-RPC error
// saying why, so an MCP client shows the reason instead of hanging. slash-mcp --live uses it.
[[nodiscard]] std::string forward_mcp(const Result<Endpoint>& endpoint, std::string_view message);

} // namespace slash::server
