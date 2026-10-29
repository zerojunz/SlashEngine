#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// A small game's files: an arena whose spawner spawns bullets under it.
void write_project(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "bullet.prefab.json") << R"({
  "components": {
    "Transform2D": {"position": [10.0, 20.0]},
    "MultiplayerSynchronizer": {"properties": [{"path": "Transform2D/position"}]}
  },
  "format": 1
})";
    std::ofstream(root / "arena.scene.json") << R"({
  "entities": {
    "arena": {"components": {"Transform2D": {}}},
    "spawner": {"components": {"MultiplayerSpawner": {
      "spawnable_scenes": ["bullet.prefab.json"], "spawn_path": "arena"}}},
    "table": {}
  },
  "format": 1
})";
}

// One machine's session over the game, with a "say" procedure that records what it heard.
struct Player {
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::vector<std::string> heard;

    Player(world::ComponentRegistry& components, const std::filesystem::path& root) {
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.project_root = root.string();
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "arena.scene.json"})");
        net::multiplayer(session->world())
            .add_rpc("say", {.mode = net::RpcMode::AnyPeer, .call_local = true},
                     [this](world::World& /*world*/, entt::entity, const json::Value& args) {
                         heard.push_back(args.as_string());
                     });
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") const {
        return session->call(name, parse(params));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

// Steps every session a tick at a time until done says so, for up to five seconds.
bool step_until(const std::vector<Player*>& players, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Player* player : players) {
            player->ok("step");
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST_CASE("net commands host, join, call and inspect a game on several sessions") {
    const std::filesystem::path root = "net_commands_project";
    write_project(root);
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    {
        Player server(components, root);
        Player alice(components, root);
        Player bob(components, root);

        // Offline at first: a server alone.
        json::Value state = server.ok("net.state");
        CHECK(state == parse(R"({"id":1,"peers":[],"server":true,"status":"connected",
                                 "transport":"offline"})"));

        for (const char* transport : {"enet", "websocket"}) {
            CAPTURE(transport);
            const json::Value hosted =
                server.ok("net.host", R"({"transport": ")" + std::string(transport) +
                                          R"(", "bind_address": "127.0.0.1"})");
            CHECK(hosted.find("id")->as_int() == 1);
            const std::int64_t port = hosted.find("port")->as_int();
            REQUIRE(port > 0);
            const std::string join = R"({"transport": ")" + std::string(transport) +
                                     R"(", "port": )" + std::to_string(port) + "}";
            CHECK(alice.ok("net.join", join).find("status")->as_string() == "connecting");
            (void)bob.ok("net.join", join);
            REQUIRE(step_until({&server, &alice, &bob}, [&] {
                return alice.ok("net.state").find("peers")->as_array().size() == 2 &&
                       bob.ok("net.state").find("peers")->as_array().size() == 2 &&
                       server.ok("net.state").find("peers")->as_array().size() == 2;
            }));
            state = alice.ok("net.state", R"({"authority": ["table"]})");
            CHECK(state.find("transport")->as_string() == transport);
            CHECK(state.find("status")->as_string() == "connected");
            CHECK_FALSE(state.find("server")->as_bool());
            CHECK(state.find("authority")->find("table")->as_int() == 1);

            // A call any peer may make reaches the others through the server, and runs locally.
            alice.heard.clear();
            bob.heard.clear();
            server.heard.clear();
            (void)alice.ok("net.rpc", R"({"entity": "table", "method": "say", "args": "hi"})");
            CHECK(alice.heard == std::vector<std::string>{"hi"});
            REQUIRE(step_until({&server, &alice, &bob},
                               [&] { return bob.heard.size() == 1 && server.heard.size() == 1; }));

            // The server drops bob, then closes; alice goes offline when told.
            const std::int64_t bob_id = bob.ok("net.state").find("id")->as_int();
            CHECK(server.ok("net.close", R"({"peer": )" + std::to_string(bob_id) + "}")
                      .find("dropped")
                      ->as_int() == bob_id);
            REQUIRE(step_until({&server, &alice, &bob}, [&] {
                return server.ok("net.state").find("peers")->as_array().size() == 1;
            }));
            CHECK(server.ok("net.close").find("transport")->as_string() == "offline");
            (void)alice.ok("net.close");
            (void)bob.ok("net.close");
        }

        // Mistakes say where they are.
        command::Reply reply = alice.call("net.rpc", R"({"entity": "tabel", "method": "say"})");
        CHECK(reply.code == command::code::kNotFound);
        CHECK(reply.message.find("did you mean \"table\"") != std::string::npos);
        reply = alice.call("net.rpc", R"({"entity": "table", "method": "shout"})");
        CHECK(reply.code == command::code::kNotFound);
        reply = alice.call("net.close", R"({"peer": 5})");
        CHECK_FALSE(reply.ok);
        reply = alice.call("net.join", R"({"transport": "carrier pigeon", "port": 1})");
        CHECK(reply.code == command::code::kInvalidParams);
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("net commands: spawned entities survive undo on the server") {
    const std::filesystem::path root = "net_commands_undo_project";
    write_project(root);
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    {
        Player server(components, root);
        Player client(components, root);
        const std::int64_t port = server.ok("net.host").find("port")->as_int();
        (void)client.ok("net.join", R"({"port": )" + std::to_string(port) + "}");
        REQUIRE(step_until({&server, &client}, [&] {
            return client.ok("net.state").find("status")->as_string() == "connected" &&
                   server.ok("net.state").find("peers")->as_array().size() == 1;
        }));

        // A bullet placed under the arena is spawned on the client.
        (void)server.ok("entity.create", R"({"id": "bullet", "extends": "bullet.prefab.json"})");
        (void)server.ok(
            "component.set",
            R"({"id": "arena", "components": {"Transform2D": {"children": ["bullet"]}}})");
        REQUIRE(step_until({&server, &client},
                           [&] { return client.ok("net.state").find("spawned") != nullptr; }));
        CHECK(client.ok("net.state").find("spawned")->find("bullet")->as_string() == "spawner");

        // Moving it and undoing the move puts the server's entities back from a copy; the bullet
        // stays spawned rather than being despawned and spawned again.
        (void)server.ok(
            "component.set",
            R"({"id": "bullet", "components": {"Transform2D": {"position": [50, 60]}}})");
        (void)server.ok("undo");
        const std::string server_events =
            server.ok("events.subscribe", R"({"kinds": ["spawned", "despawned"]})")
                .find("subscription")
                ->as_string();
        const std::string client_events =
            client.ok("events.subscribe", R"({"kinds": ["spawned", "despawned"]})")
                .find("subscription")
                ->as_string();
        for (int i = 0; i < 5; ++i) {
            (void)server.ok("step");
            (void)client.ok("step");
        }
        CHECK(server.ok("net.state").find("spawned")->find("bullet") != nullptr);
        CHECK(client.ok("net.state").find("spawned")->find("bullet") != nullptr);
        CHECK(server.ok("events.read", R"({"subscription": ")" + server_events + "\"}")
                  .find("events")
                  ->as_array()
                  .empty());
        CHECK(client.ok("events.read", R"({"subscription": ")" + client_events + "\"}")
                  .find("events")
                  ->as_array()
                  .empty());
    }
    std::filesystem::remove_all(root);
}
