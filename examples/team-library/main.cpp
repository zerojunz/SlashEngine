// Team sharing for the asset library: what one teammate makes, uses and says about an asset, the
// others find, so nobody makes it again.
//
// Alice, Bob and Carol each have a project and a library on their own machine. The team's library
// is the folder team/, as a network share or a synced folder would be; each project's
// providers.json names it. A library server that `slash-cli library serve` runs works the same,
// with its URL in place of the folder.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/library/library.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

struct Teammate {
    std::string name;
    std::unique_ptr<command::Session> session;
};

// A teammate's project, with providers.json naming the team's library when shared is set.
Teammate join(const slash::world::ComponentRegistry& components, const command::Registry& commands,
              const std::string& name, bool shared) {
    (void)slash::platform::create_directories(name);
    (void)slash::platform::write_file(name + "/project.json",
                                      R"({"format": 1, "name": ")" + name + R"("})");
    if (shared) {
        (void)slash::platform::write_file(name + "/providers.json",
                                          R"({"library": {"team": "../team"}})");
    }
    command::SessionOptions options;
    options.capture_logs = false;
    options.project_root = name;
    options.library_dir = name + "-library"; // ~/.slash/library on each machine
    return {name, std::make_unique<command::Session>(components, commands, options)};
}

// Prints who made the call and the call as an agent would make it, then the reply.
json::Value call(const Teammate& who, std::string_view name, const std::string& params) {
    const json::Value value = json::parse(params).value();
    std::printf("%s: %.*s %s\n", who.name.c_str(), static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = who.session->call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

std::string id_of(const json::Value& reply) {
    return reply.find("id")->as_string();
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    (void)slash::platform::create_directories("team");
    const Teammate alice = join(components, commands, "alice", true);
    const Teammate bob = join(components, commands, "bob", true);

    // What Alice generates goes to the team's library with the request that made it.
    const std::string wall =
        id_of(call(alice, "asset.generate", R"({"prompt": "grey stone wall tile",
        "generator": "test", "width": 16, "tags": ["tile"], "path": "art/wall.png"})"));
    call(alice, "asset.generate", R"({"prompt": "torch flicker loop", "type": "sound",
        "generator": "test", "seconds": 0.25, "tags": ["fire"]})");

    // Bob's exact repeat is answered from the team's library, with no model call, and his
    // search finds Alice's sound before it is on his machine.
    call(bob, "asset.generate", R"({"prompt": "grey stone wall tile", "generator": "test",
        "width": 16, "tags": ["tile"], "path": "art/wall.png"})");
    const json::Value found = call(bob, "asset.search", R"({"text": "torch"})");
    call(bob, "asset.use",
         R"({"id": ")" + id_of(found.find("assets")->as_array()[0]) +
             R"(", "path": "sfx/torch.wav"})");

    // Bob's verdict reaches the team: Alice's search ranks by it, and her refined version is
    // shared with the one it came from.
    call(bob, "asset.feedback", R"({"id": ")" + wall + R"(", "verdict": "rejected",
        "reason": "too dark beside the floor tiles"})");
    call(alice, "asset.search", R"({"text": "stone wall"})");
    call(alice, "asset.refine", R"({"id": ")" + wall + R"(", "prompt": "make it lighter",
        "path": "art/wall.png"})");
    call(alice, "asset.info", R"({"id": ")" + wall + R"("})");

    // Carol made a sprite before she joined; asset.sync shares what her project uses.
    const Teammate carol = join(components, commands, "carol", false);
    call(carol, "asset.generate", R"({"prompt": "golden key pickup", "generator": "test",
        "width": 8, "path": "art/key.png"})");
    (void)slash::platform::write_file("carol/providers.json",
                                      R"({"library": {"team": "../team"}})");
    call(carol, "asset.sync", "{}");
    call(bob, "asset.search", R"({"text": "key"})");

    const auto records = slash::library::Library("team").all();
    std::printf("team library: %zu assets\n", records ? records.value().size() : 0);
    return 0;
}
