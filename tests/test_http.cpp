#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/net/http.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <httplib.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace slash;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), text);
    return std::move(value).value();
}

struct Web {
    world::ComponentRegistry components;
    world::World world{components};
    net::HttpFakes fakes;
    std::vector<std::string> heard; // "<tick> <status> <body or error>", in callback order

    Web() { net::install_http(world, fakes.sender()); }

    net::HttpCallback note() {
        return [this](world::World& in_world, const net::HttpReply& reply) {
            heard.push_back(std::to_string(in_world.tick()) + " " + std::to_string(reply.status) +
                            " " + (reply.error.empty() ? reply.body : reply.error));
        };
    }
    void step() { world.step(kTick); }
};

} // namespace

TEST_CASE("HTTP replies reach game code at the next tick, in the order asked") {
    Web web;
    web.fakes.add({.url_prefix = "https://api.example.com/",
                   .method = {},
                   .status = 200,
                   .headers = {{"Content-Type", "application/json"}},
                   .body = R"({"news": "Hello"})"});
    web.fakes.add({.url_prefix = "https://api.example.com/scores",
                   .method = "POST",
                   .status = 201,
                   .headers = {},
                   .body = "saved"});
    std::optional<net::HttpReply> news;
    const std::uint64_t first =
        net::http_get(web.world, "https://api.example.com/news",
                      [&](world::World& /*world*/, const net::HttpReply& reply) { news = reply; });
    const std::uint64_t second = net::http_post_json(web.world, "https://api.example.com/scores",
                                                     parse(R"({"score": 12})"), web.note());
    CHECK(first == 1);
    CHECK(second == 2);
    // Nothing is handed back mid-tick, even when the reply is already in.
    CHECK_FALSE(news.has_value());
    CHECK(web.heard.empty());
    web.step();
    REQUIRE(news.has_value());
    const net::HttpReply& got = news.value();
    CHECK(got.ok());
    CHECK(got.body == R"({"news": "Hello"})");
    CHECK(got.header("content-type") == "application/json");
    CHECK(got.header("ETag").empty());
    REQUIRE(web.heard.size() == 1);
    CHECK(web.heard[0] == "0 201 saved");

    const std::vector<net::HttpRecord> records = net::http_requests(web.world);
    REQUIRE(records.size() == 2);
    CHECK(records[0].done);
    CHECK(records[0].status == 200);
    CHECK(records[0].bytes == 17);
    CHECK(records[1].method == "POST");
}

TEST_CASE("HTTP requests that can't go fail at the next tick, and cancelled ones say nothing") {
    Web web;
    net::http_get(web.world, "ftp://example.com/file", web.note());
    net::HttpRequest odd;
    odd.url = "https://example.com/";
    odd.method = "BREW";
    net::http_request(web.world, odd, web.note());
    // No fake answers this one.
    net::http_get(web.world, "https://example.com/missing", web.note());
    const std::uint64_t dropped = net::http_get(web.world, "https://example.com/x", web.note());
    net::http_cancel(web.world, dropped);
    web.step();
    REQUIRE(web.heard.size() == 3);
    CHECK(web.heard[0] == R"(0 0 "ftp://example.com/file" is not an http:// or https:// URL)");
    CHECK(web.heard[1].find(R"(no HTTP method "BREW")") != std::string::npos);
    CHECK(web.heard[2] ==
          "0 0 no fake for GET https://example.com/missing; add one with net.http_fake");
    CHECK(net::http_requests(web.world).size() == 3);

    // A world with no sender refuses; one without HTTP takes nothing.
    net::set_http_sender(web.world, nullptr);
    net::http_get(web.world, "https://example.com/", web.note());
    web.step();
    CHECK(web.heard.back() == "1 0 no HTTP here: this program sends no requests");
    const world::ComponentRegistry components;
    world::World offline(components);
    CHECK(net::http_get(offline, "https://example.com/", nullptr) == 0);
}

TEST_CASE("a session answers game code's HTTP from fakes, then the network") {
    httplib::Server server;
    server.Get("/motd", [](const httplib::Request& /*request*/, httplib::Response& response) {
        response.set_header("X-Server", "local");
        response.set_content("Welcome back", "text/plain");
    });
    server.Put("/profile", [](const httplib::Request& request, httplib::Response& response) {
        response.status = request.get_header_value("Authorization") == "Bearer token" ? 204 : 401;
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread serving([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    {
        command::Session session(components, commands, command::SessionOptions{});
        const auto call = [&](std::string_view name, std::string_view params) {
            return session.call(name, parse(params));
        };
        world::World& world = session.world();
        std::vector<net::HttpReply> replies;
        const auto keep = [&](world::World& /*world*/, const net::HttpReply& reply) {
            replies.push_back(reply);
        };
        const auto wait_for = [&](std::size_t count) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (replies.size() < count && std::chrono::steady_clock::now() < deadline) {
                world.step(kTick);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            REQUIRE(replies.size() == count);
        };

        // Over the network.
        net::http_get(world, base + "/motd", keep);
        net::HttpRequest put;
        put.url = base + "/profile";
        put.method = "PUT";
        put.headers = {{"Authorization", "Bearer token"}};
        put.body = R"({"name": "Player"})";
        net::http_request(world, put, keep);
        wait_for(2);
        CHECK(replies[0].status == 200);
        CHECK(replies[0].body == "Welcome back");
        CHECK(replies[0].header("x-server") == "local");
        CHECK(replies[1].status == 204);

        // A fake answers first.
        auto reply = call("net.http_fake", R"({"url_prefix": ")" + base +
                                               R"(/motd", "status": 503, "body": {"retry": 5}})");
        REQUIRE_MESSAGE(reply.ok, reply.message);
        CHECK(reply.value.find("fakes")->as_int() == 1);
        net::http_get(world, base + "/motd", keep);
        wait_for(3);
        CHECK(replies[2].status == 503);
        CHECK(replies[2].body == R"({"retry":5})");
        CHECK(replies[2].header("Content-Type") == "application/json");

        reply = call("net.http", "{}");
        REQUIRE(reply.ok);
        const json::Array& listed = reply.value.find("requests")->as_array();
        REQUIRE(listed.size() == 3);
        CHECK(listed[2].find("status")->as_int() == 503);
        CHECK(listed[1].find("method")->as_string() == "PUT");
        CHECK(reply.value.find("fakes")->as_array()[0].find("status")->as_int() == 503);

        CHECK(call("net.http_fake", R"({"clear": true})").value.find("fakes")->as_int() == 0);
        CHECK_FALSE(call("net.http_fake", "{}").ok);
        CHECK_FALSE(call("net.http_fake", R"({"url_prefix": "x", "method": "BREW"})").ok);
    }
    {
        // Without the network capability, only fakes answer.
        command::SessionOptions options;
        options.capabilities = {command::Capability::Read, command::Capability::Run};
        command::Session offline(components, commands, options);
        std::optional<net::HttpReply> got;
        net::http_get(offline.world(), base + "/motd",
                      [&](world::World& /*world*/, const net::HttpReply& reply) { got = reply; });
        offline.world().step(kTick);
        REQUIRE(got.has_value());
        CHECK(got.value().status == 0);
        CHECK(got.value().error.find("no fake for GET") != std::string::npos);
    }
    server.stop();
    serving.join();
}
