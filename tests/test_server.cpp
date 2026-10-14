#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/server/client.hpp"
#include "slash/server/openapi.hpp"
#include "slash/server/server.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace json = slash::json;
namespace command = slash::command;
namespace server = slash::server;
using namespace std::chrono_literals;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// A session served over HTTP. The test thread owns the session: it polls the server while a
// helper thread waits for each response.
struct Live {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::unique_ptr<server::Server> http;

    explicit Live(server::ServerOptions options = {}) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions session_options;
        session_options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, session_options);
        auto started = server::Server::start(*session, std::move(options));
        REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
        http = std::move(started).value();
    }

    // Runs work on a helper thread, polling the server until it finishes.
    template <typename Work>
    auto serve_while(Work work) {
        auto future = std::async(std::launch::async, std::move(work));
        while (future.wait_for(0ms) != std::future_status::ready) {
            http->poll(5ms);
        }
        return future.get();
    }

    server::HttpResponse send(server::HttpRequest request) {
        return send_to(http->endpoint(), std::move(request));
    }

    server::HttpResponse send_to(const server::Endpoint& endpoint, server::HttpRequest request) {
        auto result = serve_while([&] { return server::send(endpoint, request); });
        REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
        return result.value();
    }

    server::HttpResponse post(std::string_view path, std::string_view body = "") {
        server::HttpRequest request;
        request.path = path;
        request.body = body;
        return send(std::move(request));
    }

    server::HttpResponse get(std::string_view path) {
        server::HttpRequest request;
        request.method = "GET";
        request.path = path;
        return send(std::move(request));
    }
};

} // namespace

TEST_CASE("the REST mirror runs commands and maps errors to HTTP statuses") {
    Live live;
    auto response = live.post("/v1/commands/entity.create",
                              R"({"id": "player", "components": {"Transform2D": {}}})");
    CHECK(response.status == 200);
    CHECK(response.content_type == "application/json");
    CHECK(parse(response.body) == parse(R"({"id": "player"})"));
    CHECK(live.session->world().size() == 1);

    response = live.post("/v1/commands/world.query");
    CHECK(response.status == 200);
    CHECK(*parse(response.body).find("entities") == parse(R"(["player"])"));

    response = live.post("/v1/commands/entity.get", R"({"id": "playr"})");
    CHECK(response.status == 404);
    const json::Value error = *parse(response.body).find("error");
    CHECK(error.find("code")->as_int() == command::code::kNotFound);
    CHECK(error.find("data")->find("path")->as_string() == "/id");

    CHECK(live.post("/v1/commands/step", R"({"ticks": "ten"})").status == 400);
    CHECK(live.post("/v1/commands/step", "{not json").status == 400);
    response = live.post("/v1/commands/world.qurey");
    CHECK(response.status == 404);
    CHECK(response.body.find("did you mean") != std::string::npos);
    CHECK(live.post("/v1/nowhere").status == 404);
}

TEST_CASE("the command list and the OpenAPI document cover every command") {
    Live live;
    const json::Value listed = parse(live.get("/v1/commands").body);
    CHECK(listed.find("commands")->size() == live.commands.commands().size());

    const json::Value document = parse(live.get("/v1/openapi.json").body);
    CHECK(document.find("openapi")->as_string() == "3.1.0");
    CHECK(document.find("servers")->as_array().front().find("url")->as_string() ==
          live.http->endpoint().url());
    const json::Value& operation =
        *document.find("paths")->find("/v1/commands/entity.create")->find("post");
    CHECK(operation.find("operationId")->as_string() == "entity_create");
    CHECK(
        *operation.find("requestBody")->find("content")->find("application/json")->find("schema") ==
        live.commands.find("entity.create")->params);
    CHECK(document.find("paths")->size() == live.commands.commands().size());
}

TEST_CASE("requests need the token and a local Host and Origin") {
    Live live;
    server::Endpoint wrong = live.http->endpoint();
    wrong.token = "not-the-token";
    server::HttpRequest query;
    query.path = "/v1/commands/world.query";
    CHECK(live.send_to(wrong, query).status == 401);
    wrong.token.clear();
    CHECK(live.send_to(wrong, query).status == 401);

    server::HttpRequest rebinding = query;
    rebinding.headers = {{"Host", "evil.example"}};
    CHECK(live.send(rebinding).status == 403);

    server::HttpRequest cross_site = query;
    cross_site.headers = {{"Origin", "http://evil.example"}};
    CHECK(live.send(cross_site).status == 403);

    server::HttpRequest local = query;
    local.headers = {{"Origin", "http://localhost:" + std::to_string(live.http->endpoint().port)}};
    CHECK(live.send(local).status == 200);
    CHECK(live.session->world().size() == 0);
}

TEST_CASE("JSON-RPC is served at /rpc") {
    Live live;
    auto response = live.post("/rpc", R"({"jsonrpc": "2.0", "id": 4, "method": "entity.create",
                              "params": {"id": "a"}})");
    CHECK(response.status == 200);
    CHECK(parse(response.body) == parse(R"({"jsonrpc": "2.0", "id": 4, "result": {"id": "a"}})"));
    CHECK(live.post("/rpc", R"({"jsonrpc": "2.0", "method": "step"})").status == 204);
    CHECK(live.session->world().tick() == 1);
}

TEST_CASE("MCP is served over Streamable HTTP at /mcp") {
    Live live;
    auto response = live.post("/mcp", R"({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                          "params": {"protocolVersion": "2025-11-25"}})");
    CHECK(response.status == 200);
    CHECK(parse(response.body).find("result")->find("protocolVersion")->as_string() ==
          "2025-11-25");

    CHECK(
        live.post("/mcp", R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").status ==
        202);

    response = live.post("/mcp", R"({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                     "params": {"name": "entity_create",
                                                "arguments": {"id": "a"}}})");
    CHECK(*parse(response.body).find("result")->find("structuredContent") ==
          parse(R"({"id": "a"})"));

    server::HttpRequest old_version;
    old_version.path = "/mcp";
    old_version.body = R"({"jsonrpc": "2.0", "id": 3, "method": "ping"})";
    old_version.headers = {{"MCP-Protocol-Version", "2023-01-01"}};
    CHECK(live.send(old_version).status == 400);
    old_version.headers = {{"MCP-Protocol-Version", "2025-06-18"}};
    CHECK(live.send(old_version).status == 200);

    CHECK(live.get("/mcp").status == 405);
}

TEST_CASE("a request fails with 503 when the owner never runs it") {
    server::ServerOptions options;
    options.request_timeout = 50ms;
    Live live(options);
    server::HttpRequest request;
    request.path = "/v1/commands/entity.create";
    // Nothing polls, so the request times out and is dropped from the queue.
    const auto result = server::send(live.http->endpoint(), request);
    REQUIRE(result.ok());
    CHECK(result.value().status == 503);
    CHECK(live.http->poll() == 0);
    CHECK(live.session->world().size() == 0);
}

TEST_CASE("servers listen only on this machine") {
    Live live;
    for (const std::string_view host : {"0.0.0.0", "192.168.1.10", "example.com"}) {
        server::ServerOptions options;
        options.host = host;
        CHECK(server::Server::start(*live.session, options).error().code ==
              slash::ErrorCode::InvalidArgument);
    }
    server::ServerOptions bad_port;
    bad_port.port = 70000;
    CHECK_FALSE(server::Server::start(*live.session, bad_port).ok());

    server::ServerOptions taken;
    taken.port = live.http->endpoint().port;
    CHECK(server::Server::start(*live.session, taken).error().code ==
          slash::ErrorCode::PlatformFailure);
}

TEST_CASE("tokens are random unless one is given") {
    Live first;
    Live second;
    CHECK(first.http->endpoint().token.size() == 32);
    CHECK(first.http->endpoint().token != second.http->endpoint().token);

    server::ServerOptions options;
    options.token = "fixed-token";
    Live fixed(options);
    CHECK(fixed.http->endpoint().token == "fixed-token");
    CHECK(fixed.post("/v1/commands/world.query").status == 200);
}

TEST_CASE("endpoints round-trip through the discovery file") {
    const server::Endpoint endpoint{"127.0.0.1", 4321, "abc"};
    CHECK(endpoint.url() == "http://127.0.0.1:4321");
    CHECK(server::Endpoint{"::1", 80, ""}.url() == "http://[::1]:80");

    const std::string root = "server_test_project";
    REQUIRE(slash::platform::create_directories(root + "/.slash").ok());
    REQUIRE(slash::platform::write_file(root + "/" + std::string(server::kEndpointFile),
                                        json::write(endpoint.to_json()))
                .ok());
    const auto read = server::read_endpoint(root);
    REQUIRE(read.ok());
    CHECK(read.value().port == 4321);
    CHECK(read.value().token == "abc");

    CHECK(server::read_endpoint("no_such_project").error().code == slash::ErrorCode::NotFound);
    CHECK_FALSE(server::Endpoint::from_json(parse(R"({"host": "x", "port": "80"})")).ok());
}

TEST_CASE("forward_mcp relays messages and explains why delivery failed") {
    Live live;
    const auto forward = [&](const slash::Result<server::Endpoint>& endpoint,
                             std::string_view message) {
        return live.serve_while([&] { return server::forward_mcp(endpoint, message); });
    };
    const slash::Result<server::Endpoint> endpoint = live.http->endpoint();

    const json::Value pong =
        parse(forward(endpoint, R"({"jsonrpc": "2.0", "id": 7, "method": "ping"})"));
    CHECK(*pong.find("id") == json::Value(7));
    CHECK(pong.find("result") != nullptr);
    CHECK(
        forward(endpoint, R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").empty());

    const slash::Result<server::Endpoint> missing =
        slash::make_error(slash::ErrorCode::NotFound, "no running engine server found");
    const json::Value error =
        parse(forward(missing, R"({"jsonrpc": "2.0", "id": "x", "method": "ping"})"));
    CHECK(error.find("id")->as_string() == "x");
    CHECK(error.find("error")->find("message")->as_string() == "no running engine server found");
    CHECK(forward(missing, R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").empty());

    server::Endpoint wrong_token = live.http->endpoint();
    wrong_token.token = "nope";
    const json::Value refused =
        parse(forward(wrong_token, R"({"jsonrpc": "2.0", "id": 1, "method": "ping"})"));
    CHECK(refused.find("error")->find("message")->as_string().find("401") != std::string::npos);

    // A server that has stopped no longer answers.
    server::Endpoint stopped;
    {
        Live gone;
        stopped = gone.http->endpoint();
    }
    const json::Value unreachable =
        parse(server::forward_mcp(stopped, R"({"jsonrpc": "2.0", "id": 2, "method": "ping"})"));
    CHECK(unreachable.find("error")->find("message")->as_string().find("could not reach") !=
          std::string::npos);
}

TEST_CASE("http_status maps command errors") {
    CHECK(server::http_status(command::code::kInvalidParams) == 400);
    CHECK(server::http_status(command::code::kPermissionDenied) == 403);
    CHECK(server::http_status(command::code::kNotFound) == 404);
    CHECK(server::http_status(command::code::kInternalError) == 500);
}
