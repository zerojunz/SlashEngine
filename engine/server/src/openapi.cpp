#include "slash/server/openapi.hpp"

#include "slash/command/mcp.hpp"
#include "slash/core/version.hpp"

#include <string>
#include <utility>

namespace slash::server {
namespace {

json::Value json_content(json::Value schema) {
    json::Value content;
    content["application/json"]["schema"] = std::move(schema);
    return content;
}

json::Value error_schema() {
    json::Value error;
    error["type"] = "object";
    error["required"] = json::Array{"code", "message"};
    error["properties"]["code"]["type"] = "integer";
    error["properties"]["code"]["description"] = "JSON-RPC style error code.";
    error["properties"]["message"]["type"] = "string";
    error["properties"]["data"]["description"] =
        "Details such as the JSON Pointer of the bad value, or every schema error.";
    json::Value schema;
    schema["type"] = "object";
    schema["required"] = json::Array{"error"};
    schema["properties"]["error"] = std::move(error);
    return schema;
}

} // namespace

json::Value openapi(const command::Registry& commands, std::string_view server_url) {
    json::Value paths = json::Object{};
    for (const command::Command& command : commands.commands()) {
        json::Value operation;
        operation["operationId"] = command::mcp::tool_name(command.name);
        operation["summary"] = command.description;
        json::Array capabilities;
        for (const command::Capability capability : command.capabilities) {
            capabilities.emplace_back(command::to_string(capability));
        }
        operation["x-capabilities"] = std::move(capabilities);
        operation["requestBody"]["required"] = false;
        operation["requestBody"]["content"] = json_content(command.params);

        json::Value ok;
        ok["description"] = "The command's result.";
        json::Value result;
        result["type"] = "object";
        ok["content"] = json_content(std::move(result));
        json::Value failed;
        failed["description"] = "The command failed and changed nothing.";
        json::Value reference;
        reference["$ref"] = "#/components/schemas/Error";
        failed["content"] = json_content(std::move(reference));
        operation["responses"]["200"] = std::move(ok);
        operation["responses"]["default"] = std::move(failed);

        paths["/v1/commands/" + command.name]["post"] = std::move(operation);
    }

    json::Value document;
    document["openapi"] = "3.1.0";
    document["info"]["title"] = "SlashEngine";
    document["info"]["version"] = version();
    document["info"]["description"] =
        "Engine commands for agents. Each command takes its params as a JSON object and returns "
        "a JSON object; a failed command changes nothing. Send the token from "
        ".slash/server.json as a bearer token.";
    json::Value server;
    server["url"] = server_url;
    document["servers"] = json::Array{std::move(server)};
    json::Value bearer;
    bearer["bearer"] = json::Array{};
    document["security"] = json::Array{std::move(bearer)};
    document["components"]["securitySchemes"]["bearer"]["type"] = "http";
    document["components"]["securitySchemes"]["bearer"]["scheme"] = "bearer";
    document["components"]["schemas"]["Error"] = error_schema();
    document["paths"] = std::move(paths);
    return document;
}

int http_status(int error_code) noexcept {
    switch (error_code) {
    case command::code::kParseError:
    case command::code::kInvalidRequest:
    case command::code::kInvalidParams:
    case command::code::kInvalidArgument:
        return 400;
    case command::code::kPermissionDenied:
        return 403;
    case command::code::kMethodNotFound:
    case command::code::kNotFound:
        return 404;
    default:
        return 500;
    }
}

} // namespace slash::server
