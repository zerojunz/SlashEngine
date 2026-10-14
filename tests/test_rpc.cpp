#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/rpc.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace json = slash::json;
namespace command = slash::command;
namespace rpc = slash::command::rpc;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// Echoes its params, fails on "fail", and counts every call.
struct Echo {
    int calls = 0;

    std::string handle(std::string_view message) {
        return rpc::handle(message, [this](std::string_view method, const json::Value& params) {
            ++calls;
            if (method == "fail") {
                return command::Reply::failure(command::code::kNotFound, "nothing here",
                                               parse(R"({"why": "test"})"));
            }
            return command::Reply::success(params);
        });
    }
};

} // namespace

TEST_CASE("requests get a result with the same id") {
    Echo echo;
    CHECK(parse(echo.handle(R"({"jsonrpc": "2.0", "id": 7, "method": "echo",
                                "params": {"a": 1}})")) ==
          parse(R"({"jsonrpc": "2.0", "id": 7, "result": {"a": 1}})"));
    CHECK(parse(echo.handle(R"({"jsonrpc": "2.0", "id": "x", "method": "echo"})")) ==
          parse(R"({"jsonrpc": "2.0", "id": "x", "result": null})"));
}

TEST_CASE("failed calls become error responses") {
    Echo echo;
    CHECK(parse(echo.handle(R"({"jsonrpc": "2.0", "id": 1, "method": "fail"})")) ==
          parse(R"({"jsonrpc": "2.0", "id": 1, "error": {"code": -32001,
                    "message": "nothing here", "data": {"why": "test"}}})"));
}

TEST_CASE("notifications run but get no response") {
    Echo echo;
    CHECK(echo.handle(R"({"jsonrpc": "2.0", "method": "echo"})").empty());
    CHECK(echo.handle(R"({"jsonrpc": "2.0", "method": "fail"})").empty());
    CHECK(echo.calls == 2);
}

TEST_CASE("malformed messages get protocol errors") {
    Echo echo;
    const auto error_code = [&](std::string_view message) {
        const json::Value response = parse(echo.handle(message));
        return response.find("error")->find("code")->as_int();
    };
    CHECK(error_code("{not json") == command::code::kParseError);
    CHECK(error_code(R"({"id": 1, "method": "echo"})") == command::code::kInvalidRequest);
    CHECK(error_code(R"({"jsonrpc": "2.0", "id": 1})") == command::code::kInvalidRequest);
    CHECK(error_code(R"({"jsonrpc": "2.0", "id": [1], "method": "echo"})") ==
          command::code::kInvalidRequest);
    CHECK(error_code(R"({"jsonrpc": "2.0", "id": 1, "method": "echo", "params": [1, 2]})") ==
          command::code::kInvalidParams);
    CHECK(error_code("[]") == command::code::kInvalidRequest);
    CHECK(error_code("42") == command::code::kInvalidRequest);
    CHECK(echo.calls == 0);
    CHECK(parse(echo.handle("{not json")).find("id")->is_null());
}

TEST_CASE("batches answer every request and skip notifications") {
    Echo echo;
    const json::Value responses = parse(echo.handle(R"([
        {"jsonrpc": "2.0", "id": 1, "method": "echo", "params": {"n": 1}},
        {"jsonrpc": "2.0", "method": "echo"},
        {"jsonrpc": "2.0", "id": 2, "method": "fail"},
        5
    ])"));
    REQUIRE(responses.size() == 3);
    CHECK(*responses.as_array()[0].find("result") == parse(R"({"n": 1})"));
    CHECK(responses.as_array()[1].find("error")->find("code")->as_int() ==
          command::code::kNotFound);
    CHECK(responses.as_array()[2].find("error")->find("code")->as_int() ==
          command::code::kInvalidRequest);
    CHECK(echo.handle(R"([{"jsonrpc": "2.0", "method": "echo"}])").empty());
}

TEST_CASE("a session serves its commands as methods") {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    CHECK(parse(rpc::handle(session, R"({"jsonrpc": "2.0", "id": 1, "method": "entity.create",
                                        "params": {"id": "player"}})")) ==
          parse(R"({"jsonrpc": "2.0", "id": 1, "result": {"id": "player"}})"));
    const json::Value missing =
        parse(rpc::handle(session, R"({"jsonrpc": "2.0", "id": 2, "method": "entity.creat"})"));
    CHECK(missing.find("error")->find("code")->as_int() == command::code::kMethodNotFound);
    CHECK(session.world().size() == 1);
}
