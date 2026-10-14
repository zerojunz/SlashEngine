#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace json = slash::json;
namespace command = slash::command;
namespace mcp = slash::command::mcp;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct Client {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::unique_ptr<mcp::Server> server;
    int next_id = 1;

    explicit Client(command::SessionOptions options = {}) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        server = std::make_unique<mcp::Server>(*session);
    }

    // Sends a request and returns the whole response.
    json::Value request(std::string_view method, std::string_view params = "{}") {
        json::Value message;
        message["jsonrpc"] = "2.0";
        message["id"] = next_id++;
        message["method"] = method;
        message["params"] = parse(params);
        return parse(server->handle(json::write(message, json::Style::Compact)));
    }

    json::Value result(std::string_view method, std::string_view params = "{}") {
        json::Value response = request(method, params);
        const json::Value* value = response.find("result");
        REQUIRE_MESSAGE(value != nullptr, json::write(response, json::Style::Compact));
        return *value;
    }

    json::Value call_tool(std::string_view name, std::string_view arguments = "{}") {
        return result("tools/call", R"({"name": ")" + std::string(name) + R"(", "arguments": )" +
                                        std::string(arguments) + "}");
    }
};

bool valid_tool_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

} // namespace

TEST_CASE("tool names replace dots with underscores") {
    CHECK(mcp::tool_name("entity.create") == "entity_create");
    CHECK(mcp::tool_name("undo") == "undo");
}

TEST_CASE("initialize negotiates the protocol version and sends instructions") {
    Client client;
    const json::Value result =
        client.result("initialize", R"({"protocolVersion": "2025-06-18", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}})");
    CHECK(result.find("protocolVersion")->as_string() == "2025-06-18");
    CHECK(result.find("serverInfo")->find("name")->as_string() == "slashengine");
    CHECK(result.find("capabilities")->find("tools") != nullptr);
    CHECK(result.find("instructions")->as_string().find("batch") != std::string::npos);

    const json::Value newest = client.result("initialize", R"({"protocolVersion": "1999-01-01"})");
    CHECK(newest.find("protocolVersion")->as_string() == mcp::kProtocolVersions.front());

    CHECK(client.server->handle(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})")
              .empty());
    CHECK(client.result("ping") == parse("{}"));
    CHECK(client.request("sampling/createMessage").find("error")->find("code")->as_int() ==
          command::code::kMethodNotFound);
}

TEST_CASE("every command is listed as a tool") {
    Client client;
    const json::Value tools = *client.result("tools/list").find("tools");
    CHECK(tools.size() == client.commands.commands().size());
    bool saw_query = false;
    bool saw_create = false;
    for (const json::Value& tool : tools.as_array()) {
        const std::string& name = tool.find("name")->as_string();
        CHECK_MESSAGE(valid_tool_name(name), name);
        CHECK(tool.find("inputSchema")->find("type")->as_string() == "object");
        CHECK_FALSE(tool.find("description")->as_string().empty());
        const json::Value& annotations = *tool.find("annotations");
        if (name == "world_query") {
            saw_query = true;
            CHECK(annotations.find("readOnlyHint")->as_bool());
        }
        if (name == "entity_create") {
            saw_create = true;
            CHECK_FALSE(annotations.find("readOnlyHint")->as_bool());
            CHECK_FALSE(annotations.find("destructiveHint")->as_bool());
        }
        if (name == "scene_open") {
            CHECK(annotations.find("destructiveHint")->as_bool());
        }
    }
    CHECK(saw_query);
    CHECK(saw_create);
}

TEST_CASE("tools/call runs the command and returns text and structured content") {
    Client client;
    const json::Value created =
        client.call_tool("entity_create", R"({"id": "player", "components": {"Transform2D": {}}})");
    CHECK_FALSE(created.find("isError")->as_bool());
    CHECK(*created.find("structuredContent") == parse(R"({"id": "player"})"));
    const json::Value& content = created.find("content")->as_array().front();
    CHECK(content.find("type")->as_string() == "text");
    CHECK(parse(content.find("text")->as_string()) == parse(R"({"id": "player"})"));

    // Leaving out arguments counts as an empty object.
    const json::Value described = client.result("tools/call", R"({"name": "engine_describe"})");
    CHECK_FALSE(described.find("isError")->as_bool());
}

TEST_CASE("tool failures are results the model can read") {
    Client client;
    const json::Value failed = client.call_tool("entity_get", R"({"id": "ghost"})");
    CHECK(failed.find("isError")->as_bool());
    CHECK(failed.find("structuredContent") == nullptr);
    const json::Value error =
        *parse(failed.find("content")->as_array().front().find("text")->as_string()).find("error");
    CHECK(error.find("code")->as_int() == command::code::kNotFound);
    CHECK(error.find("data")->find("path")->as_string() == "/id");

    // Unknown tools are protocol errors, with a hint.
    const json::Value unknown = client.request("tools/call", R"({"name": "entity_creat"})");
    CHECK(unknown.find("error")->find("code")->as_int() == command::code::kInvalidParams);
    CHECK(unknown.find("error")->find("message")->as_string().find("entity_create") !=
          std::string::npos);
}

TEST_CASE("capabilities apply to tools") {
    command::SessionOptions options;
    options.capabilities = {command::Capability::Read};
    Client client(options);
    const json::Value denied = client.call_tool("entity_create");
    CHECK(denied.find("isError")->as_bool());
}

TEST_CASE("the scene, schema and logs are resources") {
    Client client;
    client.call_tool("entity_create", R"({"id": "player"})");
    const json::Value resources = *client.result("resources/list").find("resources");
    CHECK(resources.size() == 3);

    const json::Value scene = client.result("resources/read", R"({"uri": "slash://scene"})");
    const json::Value& content = scene.find("contents")->as_array().front();
    CHECK(content.find("uri")->as_string() == "slash://scene");
    CHECK(parse(content.find("text")->as_string()) ==
          parse(R"({"entities": {"player": {}}, "format": 1})"));

    const json::Value schema =
        client.result("resources/read", R"({"uri": "slash://schema/scene"})");
    CHECK(parse(schema.find("contents")->as_array().front().find("text")->as_string())
              .find("$defs") != nullptr);

    const json::Value missing = client.request("resources/read", R"({"uri": "slash://nope"})");
    CHECK(missing.find("error")->find("code")->as_int() == -32002);
    CHECK(client.result("resources/templates/list").find("resourceTemplates")->size() == 0);
}
