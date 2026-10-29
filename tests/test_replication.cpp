#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

constexpr std::string_view kBullet = R"({
  "components": {
    "Transform2D": {"position": [10.0, 20.0]},
    "MultiplayerSynchronizer": {
      "properties": [
        {"path": "Transform2D/position"},
        {"path": "Transform2D/rotation", "spawn": false, "replication_mode": "on_change"},
        {"path": "Transform2D/scale/y", "spawn": false, "replication_mode": "never"}
      ]
    }
  },
  "format": 1
})";

// One machine's game: a level, a spawner that spawns bullets under it, and what it heard.
struct Game {
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    std::vector<world::Event> events;

    Game() {
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        world->set_file_reader([](std::string_view path) -> Result<std::string> {
            if (path == "bullet.prefab.json") {
                return std::string(kBullet);
            }
            return make_error(ErrorCode::NotFound, std::string(path));
        });
        const json::Value scene = json::parse(R"({
          "entities": {
            "level": {"components": {"Transform2D": {}}},
            "spawner": {"components": {"MultiplayerSpawner": {
              "spawnable_scenes": ["bullet.prefab.json"], "spawn_path": "level"}}}
          },
          "format": 1
        })")
                                      .value();
        REQUIRE(world::load_scene(*world, scene));
        world->set_event_listener([this](const world::Event& event) { events.push_back(event); });
        net::install(*world);
    }

    [[nodiscard]] net::Multiplayer& multiplayer() const { return net::multiplayer(*world); }

    // The replication events heard since the last call, as "<kind> <id>".
    std::vector<std::string> take() {
        std::vector<std::string> kinds;
        for (const world::Event& event : events) {
            if (event.kind == "spawned" || event.kind == "despawned") {
                kinds.push_back(event.kind + " " + event.id);
            }
        }
        events.clear();
        return kinds;
    }

    // Makes a bullet under the level, as gameplay code would.
    void fire(std::string_view id) const {
        const json::Value scene = json::Object{
            {"entities",
             json::Object{{std::string(id), json::Object{{"extends", "bullet.prefab.json"}}}}},
            {"format", 1}};
        REQUIRE(world::load_scene(*world, scene));
        world->ecs()
            .get<world::Transform2D>(world->find("level"))
            .children.push_back(world->find(id));
    }

    [[nodiscard]] world::Transform2D* transform(std::string_view id) const {
        const entt::entity entity = world->find(id);
        return entity == entt::null ? nullptr : world->ecs().try_get<world::Transform2D>(entity);
    }
};

// Steps every world until done says so, for up to five seconds of wall time.
bool step_until(std::initializer_list<Game*> games, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Game* game : games) {
            game->world->step(1.0 / 60.0);
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Steps every world a number of times.
void steps(std::initializer_list<Game*> games, int count) {
    for (int i = 0; i < count; ++i) {
        for (Game* game : games) {
            game->world->step(1.0 / 60.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Connects two clients to a server on a port the OS picks, and returns the port.
int connect(Game& server, Game& a, Game& b) {
    auto host = std::make_shared<net::ENetPeer>();
    REQUIRE(host->set_bind_ip("127.0.0.1"));
    REQUIRE(host->create_server(0, 4));
    const int port = host->local_port();
    REQUIRE(server.multiplayer().set_peer(host));
    for (Game* client : {&a, &b}) {
        auto peer = std::make_shared<net::ENetPeer>();
        REQUIRE(peer->create_client("127.0.0.1", port));
        REQUIRE(client->multiplayer().set_peer(peer));
    }
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return server.multiplayer().peer_ids().size() == 2 &&
               a.multiplayer().peer_ids().size() == 2 && b.multiplayer().peer_ids().size() == 2;
    }));
    return port;
}

} // namespace

TEST_CASE("replication: spawners spawn, synchronizers sync and despawns follow") {
    Game server;
    Game a;
    Game b;
    const int port = connect(server, a, b);

    // A bullet placed under the level spawns on both clients with its spawn state.
    server.fire("bullet1");
    server.transform("bullet1")->position = {30.0F, 40.0F};
    server.transform("bullet1")->scale = {1.0F, 3.0F};
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return a.transform("bullet1") != nullptr && b.transform("bullet1") != nullptr;
    }));
    CHECK(a.take() == std::vector<std::string>{"spawned bullet1"});
    CHECK(a.transform("bullet1")->position.x == 30.0F);
    CHECK(a.transform("bullet1")->scale.y == 1.0F); // neither spawned nor synced
    const auto& children = a.world->ecs().get<world::Transform2D>(a.world->find("level")).children;
    CHECK(children == std::vector<entt::entity>{a.world->find("bullet1")});

    // "always" properties follow every tick, "on_change" ones when they change.
    server.transform("bullet1")->position = {50.0F, 60.0F};
    server.transform("bullet1")->rotation = 1.5F;
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return b.transform("bullet1")->position.x == 50.0F &&
               b.transform("bullet1")->rotation == 1.5F;
    }));
    bool synced = false;
    bool delta = false;
    for (const world::Event& event : b.events) {
        synced = synced || (event.kind == "synchronized" && event.id == "bullet1");
        delta = delta || (event.kind == "delta_synchronized" && event.id == "bullet1");
    }
    CHECK(synced);
    CHECK(delta);

    // A client can't move what the server is the authority of: the server puts it back.
    a.transform("bullet1")->position = {0.0F, 0.0F};
    REQUIRE(
        step_until({&server, &a, &b}, [&] { return a.transform("bullet1")->position.x == 50.0F; }));

    // Destroying it on the server despawns it everywhere.
    server.world->destroy(server.world->find("bullet1"));
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return a.transform("bullet1") == nullptr && b.transform("bullet1") == nullptr;
    }));
    CHECK(b.take() == std::vector<std::string>{"spawned bullet1", "despawned bullet1"});
    CHECK(a.world->ecs().get<world::Transform2D>(a.world->find("level")).children.empty());

    // A peer that joins later is sent what was spawned before.
    server.fire("bullet2");
    REQUIRE(step_until({&server, &a, &b}, [&] { return a.transform("bullet2") != nullptr; }));
    Game late;
    auto peer = std::make_shared<net::ENetPeer>();
    REQUIRE(peer->create_client("127.0.0.1", port));
    REQUIRE(late.multiplayer().set_peer(peer));
    REQUIRE(
        step_until({&server, &a, &b, &late}, [&] { return late.transform("bullet2") != nullptr; }));

    // A peer that leaves the game loses what the server spawned there.
    REQUIRE(late.multiplayer().set_peer(nullptr));
    CHECK(late.transform("bullet2") == nullptr);
    CHECK(late.world->ecs().get<world::Transform2D>(late.world->find("level")).children.empty());
}

TEST_CASE("replication: custom spawns, client authority and visibility") {
    Game server;
    Game a;
    Game b;

    // Each peer makes a custom spawn the same way, and gives the player to the peer it names.
    for (Game* game : {&server, &a, &b}) {
        net::Multiplayer& mp = game->multiplayer();
        mp.set_spawn_function("spawner", [](world::World& w, std::string_view id,
                                            const json::Value& data) {
            const json::Value scene = json::Object{
                {"entities",
                 json::Object{
                     {std::string(id),
                      json::Object{
                          {"components",
                           json::Object{{"Transform2D", json::Object{{"position", data}}},
                                        {"MultiplayerSynchronizer", json::parse(R"({"properties": [
                                                        {"path": "Transform2D/position/x"}]})")
                                                                        .value()}}}}}}},
                {"format", 1}};
            auto loaded = world::load_scene(w, scene);
            if (!loaded) {
                return Result<entt::entity>(loaded.error());
            }
            return Result<entt::entity>(w.find(id));
        });
        mp.set_enter_function("spawner", [game](world::World& w, entt::entity entity) {
            const std::string id(w.id_of(entity));
            if (id.starts_with("p")) {
                game->multiplayer().set_authority(id, std::stoi(id.substr(1)));
            }
        });
    }
    connect(server, a, b);

    // The server spawns a's player, which a then moves.
    const int a_id = a.multiplayer().unique_id();
    const std::string player = "p" + std::to_string(a_id);
    server.multiplayer().set_authority(player, a_id);
    REQUIRE(server.multiplayer().spawn("spawner", player, json::Array{1.0, 2.0}));
    CHECK_FALSE(a.multiplayer().spawn("spawner", "p9", json::Array{0.0, 0.0}));
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return a.transform(player) != nullptr && b.transform(player) != nullptr;
    }));
    CHECK(b.transform(player)->position.y == 2.0F);
    CHECK(a.multiplayer().is_authority(player));
    a.transform(player)->position.x = 7.0F;
    REQUIRE(step_until({&server, &a, &b}, [&] {
        return server.transform(player)->position.x == 7.0F &&
               b.transform(player)->position.x == 7.0F;
    }));

    // A bullet hidden from everyone but b spawns only on b.
    server.fire("secret");
    server.multiplayer().set_visibility_for("secret", 0, false);
    server.multiplayer().set_visibility_for("secret", b.multiplayer().unique_id(), true);
    REQUIRE(step_until({&server, &a, &b}, [&] { return b.transform("secret") != nullptr; }));
    steps({&server, &a, &b}, 20);
    CHECK(a.transform("secret") == nullptr);

    // Remote calls go only to the peers that can see the entity.
    int calls = 0;
    for (Game* game : {&server, &a, &b}) {
        game->multiplayer().add_rpc(
            "ping", {}, [&calls](world::World&, entt::entity, const json::Value&) { ++calls; });
    }
    CHECK_FALSE(server.multiplayer().rpc("secret", "ping", {}, a.multiplayer().unique_id()));
    REQUIRE(server.multiplayer().rpc("secret", "ping"));
    REQUIRE(step_until({&server, &a, &b}, [&] { return calls == 1; }));

    // Hidden from b too, it despawns there; a filter can do the same.
    server.multiplayer().set_visibility_for("secret", b.multiplayer().unique_id(), false);
    REQUIRE(step_until({&server, &a, &b}, [&] { return b.transform("secret") == nullptr; }));
    server.multiplayer().add_visibility_filter(
        "secret", [&](int peer) { return peer == a.multiplayer().unique_id(); });
    server.multiplayer().set_visibility_for("secret", 0, true);
    REQUIRE(step_until({&server, &a, &b}, [&] { return a.transform("secret") != nullptr; }));
    steps({&server, &a, &b}, 20);
    CHECK(b.transform("secret") == nullptr);
}
