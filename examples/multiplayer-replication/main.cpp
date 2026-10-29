// Three worlds in one program share an arena over ENet, replicated as Godot's
// MultiplayerSpawner and MultiplayerSynchronizer replicate a scene tree: the server spawns a
// player for each client, which the client then moves, bullets that fly on every peer and change
// color when they hit, and a treasure only alice can see; bullets and a player that leaves are
// despawned everywhere. IDs change every run, so the output names peers by role.
#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slash;

namespace {

// A bullet: its position is synced every tick, its color when it changes.
constexpr std::string_view kBullet = R"({
  "components": {
    "Transform2D": {},
    "Sprite2D": {"texture": "bullet.png"},
    "MultiplayerSynchronizer": {"properties": [
      {"path": "Transform2D/position"},
      {"path": "Sprite2D/modulate", "replication_mode": "on_change"}
    ]}
  },
  "format": 1
})";

// The arena every peer loads: where things are spawned, and the spawner.
constexpr std::string_view kArena = R"({
  "entities": {
    "arena": {"components": {"Transform2D": {}}},
    "spawner": {"components": {"MultiplayerSpawner": {
      "spawnable_scenes": ["bullet.prefab.json", "treasure.prefab.json"],
      "spawn_path": "arena"}}}
  },
  "format": 1
})";

std::map<int, std::string> names; // peer ID to role

std::string name_of(int peer) {
    const auto found = names.find(peer);
    return found != names.end() ? found->second : "?";
}

std::string position_of(const world::World& world, std::string_view id) {
    const auto& transform = world.ecs().get<world::Transform2D>(world.find(id));
    char text[64];
    std::snprintf(text, sizeof(text), "%g, %g", static_cast<double>(transform.position.x),
                  static_cast<double>(transform.position.y));
    return text;
}

// A player, made the same way on every peer from the spawn's data, answering to its client.
Result<entt::entity> make_player(world::World& w, std::string_view id, const json::Value& data) {
    json::Value components = json::Object{
        {"Transform2D", json::Object{{"position", *data.find("at")}}},
        {"MultiplayerSynchronizer",
         json::parse(R"({"properties": [{"path": "Transform2D/position"}]})").value()}};
    json::Value entities =
        json::Object{{std::string(id), json::Object{{"components", std::move(components)}}}};
    const json::Value scene = json::Object{{"entities", std::move(entities)}, {"format", 1}};
    if (auto loaded = world::load_scene(w, scene); !loaded) {
        return loaded.error();
    }
    net::multiplayer(w).set_authority(id, static_cast<int>(data.find("peer")->as_int()));
    return w.find(id);
}

// One machine: its world with the arena, and what happened in it.
struct Machine {
    std::string name;
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    std::vector<std::string> log;

    explicit Machine(std::string role) : name(std::move(role)) {
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        world->set_file_reader([](std::string_view path) -> Result<std::string> {
            if (path == "bullet.prefab.json") {
                return std::string(kBullet);
            }
            if (path == "treasure.prefab.json") {
                return std::string(R"({"components": {"Transform2D": {},
                    "MultiplayerSynchronizer": {"public_visibility": false}}, "format": 1})");
            }
            return make_error(ErrorCode::NotFound, std::string(path));
        });
        (void)world::load_scene(*world, json::parse(kArena).value());
        world->set_event_listener([this](const world::Event& event) {
            if (event.kind == "spawned" || event.kind == "despawned") {
                log.push_back(event.kind + " " + event.id);
            }
        });
        net::install(*world);
        multiplayer().set_spawn_function("spawner", make_player);
    }

    [[nodiscard]] net::Multiplayer& multiplayer() const { return net::multiplayer(*world); }

    // Places an entity under the arena, as gameplay code does; the spawner sees it there.
    void place(std::string_view id, std::string_view prefab, Vec2 at) const {
        const json::Value scene = json::Object{
            {"entities",
             json::Object{{std::string(id), json::Object{{"extends", std::string(prefab)}}}}},
            {"format", 1}};
        (void)world::load_scene(*world, scene);
        const entt::entity entity = world->find(id);
        world->ecs().get<world::Transform2D>(entity).position = at;
        world->ecs().get<world::Transform2D>(world->find("arena")).children.push_back(entity);
    }

    [[nodiscard]] bool has(std::string_view id) const { return world->find(id) != entt::null; }

    void print() {
        std::sort(log.begin(), log.end());
        for (const std::string& line : log) {
            std::printf("  %s: %s\n", name.c_str(), line.c_str());
        }
        log.clear();
    }
};

// Steps every world, a tick each, until done says so (up to five seconds).
void step_until(std::initializer_list<Machine*> machines, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        for (Machine* machine : machines) {
            machine->world->step(1.0 / 60.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void step_for(std::initializer_list<Machine*> machines, int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    step_until(machines, [&] { return std::chrono::steady_clock::now() >= until; });
}

} // namespace

int main() {
    if (!net::enet_supported()) {
        std::printf("ENet isn't available here\n");
        return 0;
    }
    Machine server("server");
    Machine alice("alice");
    Machine bob("bob");
    names[1] = "the server";

    auto hosting = std::make_shared<net::ENetPeer>();
    (void)hosting->create_server(0);
    (void)server.multiplayer().set_peer(hosting);
    for (Machine* client : {&alice, &bob}) {
        auto joining = std::make_shared<net::ENetPeer>();
        (void)joining->create_client("127.0.0.1", hosting->local_port());
        (void)client->multiplayer().set_peer(joining);
        names[client->multiplayer().unique_id()] = client->name;
    }
    step_until({&server, &alice, &bob}, [&] {
        return alice.multiplayer().peer_ids().size() == 2 &&
               bob.multiplayer().peer_ids().size() == 2;
    });
    std::printf("Alice and bob joined the server.\n");

    std::printf("\nThe server spawns a player for each, made by the spawn function everywhere:\n");
    (void)server.multiplayer().spawn(
        "spawner", "alice_player",
        json::Object{{"at", json::Array{100, 100}}, {"peer", alice.multiplayer().unique_id()}});
    (void)server.multiplayer().spawn(
        "spawner", "bob_player",
        json::Object{{"at", json::Array{300, 100}}, {"peer", bob.multiplayer().unique_id()}});
    step_until({&server, &alice, &bob},
               [&] { return alice.log.size() == 2 && bob.log.size() == 2; });
    alice.print();
    bob.print();
    std::printf("  on bob's world, alice_player answers to %s and is at %s\n",
                name_of(bob.multiplayer().authority("alice_player")).c_str(),
                position_of(*bob.world, "alice_player").c_str());

    std::printf("\nAlice moves her player; her synchronizer sends it to the others:\n");
    alice.world->ecs().get<world::Transform2D>(alice.world->find("alice_player")).position = {
        140.0F, 90.0F};
    step_until({&server, &alice, &bob},
               [&] { return position_of(*bob.world, "alice_player") == "140, 90"; });
    std::printf("  the server sees her at %s, bob at %s\n",
                position_of(*server.world, "alice_player").c_str(),
                position_of(*bob.world, "alice_player").c_str());
    std::printf("  bob moves alice's player on his own world, and the next sync puts it back:\n");
    bob.world->ecs().get<world::Transform2D>(bob.world->find("alice_player")).position = {0.0F,
                                                                                          0.0F};
    step_until({&server, &alice, &bob},
               [&] { return position_of(*bob.world, "alice_player") == "140, 90"; });
    std::printf("  bob sees her at %s\n", position_of(*bob.world, "alice_player").c_str());

    std::printf("\nThe server fires a bullet: placed under the arena, it spawns everywhere:\n");
    server.place("bullet1", "bullet.prefab.json", {100.0F, 200.0F});
    step_until({&server, &alice, &bob}, [&] { return alice.has("bullet1") && bob.has("bullet1"); });
    alice.print();
    bob.print();
    std::printf("  it starts at %s on alice's world\n",
                position_of(*alice.world, "bullet1").c_str());
    for (int tick = 0; tick < 10; ++tick) {
        server.world->ecs().get<world::Transform2D>(server.world->find("bullet1")).position.x +=
            10.0F;
        server.world->step(1.0 / 60.0);
    }
    step_until({&server, &alice, &bob},
               [&] { return position_of(*alice.world, "bullet1") == "200, 200"; });
    std::printf("  it flies on the server, and alice sees it at %s\n",
                position_of(*alice.world, "bullet1").c_str());
    server.world->ecs().get<world::Sprite2D>(server.world->find("bullet1")).modulate = {1.0F, 0.0F,
                                                                                        0.0F, 1.0F};
    step_until({&server, &alice, &bob}, [&] {
        return bob.world->ecs().get<world::Sprite2D>(bob.world->find("bullet1")).modulate.y == 0.0F;
    });
    std::printf("  it hits and turns red; the change reaches bob reliably\n");

    std::printf("\nA treasure only alice can see:\n");
    server.place("treasure", "treasure.prefab.json", {500.0F, 500.0F});
    server.multiplayer().set_visibility_for("treasure", alice.multiplayer().unique_id(), true);
    step_until({&server, &alice, &bob}, [&] { return alice.has("treasure"); });
    step_for({&server, &alice, &bob}, 50);
    alice.print();
    bob.print();
    std::printf("  bob has it: %d\n", bob.has("treasure") ? 1 : 0);

    std::printf("\nThe bullet is gone on the server, so it despawns everywhere:\n");
    server.world->destroy(server.world->find("bullet1"));
    step_until({&server, &alice, &bob},
               [&] { return !alice.has("bullet1") && !bob.has("bullet1"); });
    alice.print();
    bob.print();

    std::printf("\nBob leaves; the server destroys his player, which despawns on alice's:\n");
    bob.multiplayer().peer().close();
    step_until({&server, &alice}, [&] { return server.multiplayer().peer_ids().size() == 1; });
    server.world->destroy(server.world->find("bob_player"));
    step_until({&server, &alice}, [&] { return !alice.has("bob_player"); });
    alice.print();
    return 0;
}
