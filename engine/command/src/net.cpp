#include "slash/command/net.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/net/enet.hpp"
#include "slash/net/multiplayer.hpp"
#include "slash/net/websocket.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace slash::command {
namespace {

constexpr std::int64_t kMaxPort = 65535;
constexpr std::int64_t kMaxClients = 4095;

// The transport the world's multiplayer runs on, kept beside it for net.state.
struct Network {
    std::string transport = "offline";
    int port = 0; // a server's
    std::weak_ptr<net::ENetPeer> enet;
};

Network& network(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (!ctx.contains<Network>()) {
        ctx.emplace<Network>();
    }
    return ctx.get<Network>();
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value integer_property(std::string_view description, std::int64_t minimum,
                             std::int64_t maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value transport_property() {
    json::Value schema;
    schema["enum"] = json::Array{"enet", "websocket"};
    schema["description"] = "Default: enet.";
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

std::string string_param(const json::Value& params, std::string_view name,
                         std::string_view fallback) {
    const json::Value* value = params.find(name);
    return value != nullptr ? value->as_string() : std::string(fallback);
}

int int_param(const json::Value& params, std::string_view name, int fallback) {
    const json::Value* value = params.find(name);
    return value != nullptr ? static_cast<int>(value->as_int()) : fallback;
}

const char* status_name(net::ConnectionStatus status) {
    switch (status) {
    case net::ConnectionStatus::Connecting:
        return "connecting";
    case net::ConnectionStatus::Connected:
        return "connected";
    case net::ConnectionStatus::Disconnected:
        break;
    }
    return "disconnected";
}

// Puts the world's multiplayer on a peer, closing the one before.
Reply use_peer(world::World& world, std::shared_ptr<net::Peer> peer, Network info) {
    net::Multiplayer& multiplayer = net::multiplayer(world);
    multiplayer.peer().close();
    if (auto set = multiplayer.set_peer(std::move(peer)); !set) {
        return Reply::failure(set.error());
    }
    network(world) = std::move(info);
    json::Value out;
    out["transport"] = network(world).transport;
    out["id"] = multiplayer.unique_id();
    out["status"] = status_name(multiplayer.peer().connection_status());
    if (network(world).port != 0) {
        out["port"] = network(world).port;
    }
    return Reply::success(std::move(out));
}

Reply net_host(Session& session, const json::Value& params) {
    const std::string transport = string_param(params, "transport", "enet");
    const int port = int_param(params, "port", 0);
    const std::string bind = string_param(params, "bind_address", "*");
    world::World& world = session.world();
    if (transport == "websocket") {
        if (!net::websocket_server_supported()) {
            return param_error(code::kInvalidArgument, "/transport",
                               "this build can't host WebSocket servers");
        }
        auto server = std::make_shared<net::WebSocketMultiplayerPeer>();
        if (auto made = server->create_server(port, bind); !made) {
            return param_error(Reply::failure(made.error()).code, "/port", made.error().message);
        }
        const int local = server->local_port();
        return use_peer(world, std::move(server),
                        {.transport = transport, .port = local, .enet = {}});
    }
    if (!net::enet_supported()) {
        return param_error(code::kInvalidArgument, "/transport", "this build has no ENet");
    }
    auto server = std::make_shared<net::ENetPeer>();
    if (auto bound = server->set_bind_ip(bind); !bound) {
        return param_error(code::kInvalidArgument, "/bind_address", bound.error().message);
    }
    if (auto made = server->create_server(port, int_param(params, "max_clients", 32)); !made) {
        return param_error(Reply::failure(made.error()).code, "/port", made.error().message);
    }
    const int local = server->local_port();
    std::weak_ptr<net::ENetPeer> enet = server;
    return use_peer(world, std::move(server),
                    {.transport = transport, .port = local, .enet = std::move(enet)});
}

Reply net_join(Session& session, const json::Value& params) {
    const std::string transport = string_param(params, "transport", "enet");
    const std::string address = string_param(params, "address", "127.0.0.1");
    const int port = int_param(params, "port", 0);
    world::World& world = session.world();
    if (transport == "websocket") {
        auto client = std::make_shared<net::WebSocketMultiplayerPeer>();
        const std::string url =
            "ws://" + address + ":" + std::to_string(port) + string_param(params, "path", "/");
        if (auto made = client->create_client(url); !made) {
            return param_error(Reply::failure(made.error()).code, "/address", made.error().message);
        }
        return use_peer(world, std::move(client), {.transport = transport, .port = 0, .enet = {}});
    }
    if (!net::enet_supported()) {
        return param_error(code::kInvalidArgument, "/transport", "this build has no ENet");
    }
    auto client = std::make_shared<net::ENetPeer>();
    if (auto made = client->create_client(address, port); !made) {
        return param_error(Reply::failure(made.error()).code, "/address", made.error().message);
    }
    std::weak_ptr<net::ENetPeer> enet = client;
    return use_peer(world, std::move(client), {.transport = transport, .enet = std::move(enet)});
}

Reply net_close(Session& session, const json::Value& params) {
    world::World& world = session.world();
    net::Multiplayer& multiplayer = net::multiplayer(world);
    if (const json::Value* peer = params.find("peer"); peer != nullptr) {
        const int id = static_cast<int>(peer->as_int());
        if (!multiplayer.is_server()) {
            return param_error(code::kInvalidArgument, "/peer", "only the server drops peers");
        }
        const auto ids = multiplayer.peer_ids();
        if (std::ranges::find(ids, id) == ids.end()) {
            return param_error(code::kNotFound, "/peer",
                               "no peer " + std::to_string(id) + " is connected");
        }
        multiplayer.peer().disconnect_peer(id);
        json::Value out;
        out["dropped"] = id;
        return Reply::success(std::move(out));
    }
    multiplayer.peer().close();
    (void)multiplayer.set_peer(nullptr);
    network(world) = {};
    json::Value out;
    out["transport"] = "offline";
    return Reply::success(std::move(out));
}

Reply net_state(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const net::Multiplayer& multiplayer = net::multiplayer(world);
    const Network& info = network(world);
    json::Value out;
    out["transport"] = info.transport;
    out["status"] = status_name(multiplayer.peer().connection_status());
    out["id"] = multiplayer.unique_id();
    out["server"] = multiplayer.is_server();
    if (info.port != 0) {
        out["port"] = info.port;
    }
    json::Array peers;
    for (const int id : multiplayer.peer_ids()) {
        peers.emplace_back(id);
    }
    out["peers"] = std::move(peers);
    if (const auto enet = info.enet.lock();
        enet != nullptr &&
        multiplayer.peer().connection_status() == net::ConnectionStatus::Connected) {
        json::Value rtt = json::Object{};
        for (const int id : multiplayer.peer_ids()) {
            if (const int ms = enet->round_trip_time(id); ms >= 0) {
                rtt[std::to_string(id)] = ms;
            }
        }
        if (!rtt.as_object().empty()) {
            out["round_trip_ms"] = std::move(rtt);
        }
    }
    if (const auto waiting = multiplayer.authenticating_peer_ids(); !waiting.empty()) {
        json::Array ids;
        for (const int id : waiting) {
            ids.emplace_back(id);
        }
        out["authenticating"] = std::move(ids);
    }
    if (const auto spawned = multiplayer.spawned(); !spawned.empty()) {
        json::Value by_id = json::Object{};
        for (const auto& [id, spawner] : spawned) {
            by_id[id] = spawner;
        }
        out["spawned"] = std::move(by_id);
    }
    if (const auto syncs = multiplayer.synchronizers(); !syncs.empty()) {
        json::Array ids;
        for (const std::string& id : syncs) {
            ids.emplace_back(id);
        }
        out["synchronizers"] = std::move(ids);
    }
    if (const json::Value* entities = params.find("authority"); entities != nullptr) {
        json::Value by_id = json::Object{};
        const json::Array& ids = entities->as_array();
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string& id = ids[i].as_string();
            if (world.find(id) == entt::null) {
                return unknown_entity(world, id, "/authority/" + std::to_string(i));
            }
            by_id[id] = multiplayer.authority(id);
        }
        out["authority"] = std::move(by_id);
    }
    return Reply::success(std::move(out));
}

Reply net_rpc(Session& session, const json::Value& params) {
    world::World& world = session.world();
    net::Multiplayer& multiplayer = net::multiplayer(world);
    const std::string entity = params.find("entity")->as_string();
    const std::string method = params.find("method")->as_string();
    if (world.find(entity) == entt::null) {
        return unknown_entity(world, entity, "/entity");
    }
    if (!multiplayer.has_rpc(method)) {
        return param_error(code::kNotFound, "/method",
                           "no remote procedure " + quoted(method) +
                               " is registered; the game's code adds them with add_rpc");
    }
    if (session.dry_running()) {
        return Reply::success(json::Object{});
    }
    const json::Value* args = params.find("args");
    if (auto sent = multiplayer.rpc(entity, method, args != nullptr ? *args : json::Value{},
                                    int_param(params, "peer", net::kTargetPeerBroadcast));
        !sent) {
        return Reply::failure(sent.error());
    }
    return Reply::success(json::Object{});
}

} // namespace

void register_net_commands(Registry& registry) {
    using enum Capability;
    registry.add({
        .name = "net.host",
        .description = "Puts the world's multiplayer on the network as its server, peer 1, as "
                       "Godot's create_server. Returns the port.",
        .capabilities = {Run, Network},
        .effect = Effect::Read,
        .params = params_schema({
            {"transport", transport_property()},
            {"port", integer_property("0 picks a free one. Default: 0.", 0, kMaxPort)},
            {"max_clients", integer_property("ENet only. Default: 32.", 1, kMaxClients)},
            {"bind_address", property("string", R"(An IPv4 address or "*". Default: "*".)")},
        }),
        .handler = net_host,
    });
    registry.add({
        .name = "net.join",
        .description = "Connects the world's multiplayer to a server, as Godot's create_client; "
                       "connected once steps have polled the answer (net.state).",
        .capabilities = {Run, Network},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"transport", transport_property()},
                {"address", property("string", "Default: \"127.0.0.1\".")},
                {"port", integer_property("The server's port.", 1, kMaxPort)},
                {"path", property("string", "WebSocket only. Default: \"/\".")},
            },
            {"port"}),
        .handler = net_join,
    });
    registry.add({
        .name = "net.close",
        .description = "Closes the connection and goes back offline, or on a server drops one "
                       "peer.",
        .capabilities = {Run, Network},
        .effect = Effect::Read,
        .params =
            params_schema({{"peer", integer_property("A peer to drop.", 2,
                                                     std::numeric_limits<std::int32_t>::max())}}),
        .handler = net_close,
    });
    json::Value authority = property("array", "Entity IDs whose authority to return.");
    authority["items"]["type"] = "string";
    registry.add({
        .name = "net.state",
        .description = "Returns the world's multiplayer: transport, status, its ID, the peers, "
                       "and what spawners spawned and synchronizers replicate.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"authority", std::move(authority)}}),
        .handler = net_state,
    });
    json::Value args;
    args["description"] = "One JSON value of arguments. Default: null.";
    registry.add({
        .name = "net.rpc",
        .description = "Calls a remote procedure the game's code registered on an entity, as "
                       "Godot's rpc and rpc_id.",
        .capabilities = {Run, Network},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"entity", property("string", "The entity's ID.")},
                {"method", property("string", "The procedure's name.")},
                {"args", std::move(args)},
                {"peer", property("integer", "0 for all, minus an ID for all but it. Default: 0.")},
            },
            {"entity", "method"}),
        .handler = net_rpc,
    });
}

} // namespace slash::command
