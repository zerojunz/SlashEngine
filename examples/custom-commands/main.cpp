// A game's own command, served like the built-in ones.
//
// Every change to a world goes through a command, so agents can make it over MCP, JSON-RPC, REST
// or the CLI, and undo it. A command has a name, a short description (agents read it as the tool
// description), the capabilities it needs, its effect, a JSON Schema for its params and a handler.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/rpc.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/core/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace game {

struct Health {
    float current = 100.0F;
    float max = 100.0F;
};
SLASH_COMPONENT(Health, current, max)

} // namespace game

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

// Params are checked against the schema before the handler runs, so the handler can rely on
// "id" being a string and "amount" a positive number.
constexpr const char* kDamageParams = R"({
    "type": "object",
    "properties": {
        "id": {"type": "string", "description": "Entity ID."},
        "amount": {"type": "number", "exclusiveMinimum": 0, "description": "Hit points to take."}
    },
    "required": ["id", "amount"],
    "additionalProperties": false
})";

command::Reply damage(command::Session& session, const json::Value& params) {
    world::World& w = session.world();
    const std::string& id = params.find("id")->as_string();
    const entt::entity entity = w.find(id);
    auto* health = entity == entt::null ? nullptr : w.ecs().try_get<game::Health>(entity);
    if (health == nullptr) {
        // Errors name the bad value's JSON Pointer and, for a misspelled name, the closest one.
        std::string message = "/id: no entity with Health has the id \"" + id + "\"";
        std::vector<std::string_view> candidates;
        for (const entt::entity other : w.ecs().view<const game::Health>()) {
            candidates.push_back(w.id_of(other));
        }
        if (const std::string_view hint = slash::closest_match(id, candidates); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        json::Value data;
        data["path"] = "/id";
        return command::Reply::failure(command::code::kNotFound, std::move(message),
                                       std::move(data));
    }
    const auto amount = static_cast<float>(params.find("amount")->as_double());
    health->current = std::max(0.0F, health->current - amount);
    // Return only what the caller needs.
    json::Value result;
    result["id"] = id;
    result["current"] = health->current;
    return command::Reply::success(std::move(result));
}

void print(const command::Reply& reply) {
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Health>().set_doc("Hit points for anything that can take damage.");

    command::Registry commands;
    command::register_builtin_commands(commands);
    commands.add({
        .name = "health.damage",
        .description = "Takes hit points from an entity, down to 0.",
        .capabilities = {command::Capability::Edit},
        // Edit: the session records an undo step, and the command takes "dry_run".
        .effect = command::Effect::Edit,
        .params = json::parse(kDamageParams).value(),
        .handler = damage,
    });

    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    print(session.call("entity.create",
                       json::parse(R"({"id": "knight", "components": {"Health": {}}})").value()));
    print(session.call("health.damage", json::parse(R"({"id": "knight", "amount": 30})").value()));
    // Over JSON-RPC it is the same command, as it is over MCP (tool "health_damage") and REST.
    std::puts(command::rpc::handle(session, R"({"jsonrpc": "2.0", "id": 1,
        "method": "health.damage", "params": {"id": "knight", "amount": 90}})")
                  .c_str());
    print(session.call("health.damage", json::parse(R"({"id": "knigt", "amount": 1})").value()));
    print(session.call("health.damage", json::parse(R"({"id": "knight", "amount": -5})").value()));
    print(session.call("undo", json::Value()));
    print(session.call("entity.get", json::parse(R"({"id": "knight"})").value()));
    return 0;
}
