// Finding what keeps growing with perf.stats' memory report, then stopping it.
//
// Each of 200 comets keeps a trail of where it has been. The first trail system never drops old
// points, so the trails grow every tick: two memory reports ten seconds apart show which part of
// the engine grew, and that part's items name the component. A trail system that keeps the last
// 32 points stops the growth. Sizes vary by platform, so this prints what the reports say, not
// the bytes themselves.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace comets {

struct Trail {
    std::vector<slash::Vec2> points;
};
SLASH_COMPONENT(Trail, points)

} // namespace comets

namespace {

namespace command = slash::command;
namespace json = slash::json;
using slash::world::Transform2D;
using slash::world::World;

constexpr int kComets = 200;
constexpr std::size_t kKept = 32;

// Moves each comet on and remembers where it was, every point of it.
void trail_forever(World& world, double tick_seconds) {
    for (const auto [entity, transform, trail] :
         world.ecs().view<Transform2D, comets::Trail>().each()) {
        trail.points.push_back(transform.position);
        transform.position.x += static_cast<float>(40.0 * tick_seconds);
    }
}

// The same, keeping the last kKept points.
void trail_capped(World& world, double tick_seconds) {
    for (const auto [entity, transform, trail] :
         world.ecs().view<Transform2D, comets::Trail>().each()) {
        trail.points.push_back(transform.position);
        if (trail.points.size() > kKept) {
            trail.points.erase(trail.points.begin());
        }
        transform.position.x += static_cast<float>(40.0 * tick_seconds);
    }
}

json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    command::Reply reply = session.call(name, json::parse(params).value());
    if (!reply.ok) {
        std::printf("%.*s failed: %s\n", static_cast<int>(name.size()), name.data(),
                    reply.message.c_str());
    }
    return std::move(reply.value);
}

// Each part's bytes, and each item's under "part/item".
std::map<std::string, std::int64_t> sizes(const json::Value& stats) {
    std::map<std::string, std::int64_t> out;
    for (const json::Value& part : stats.find("memory")->find("parts")->as_array()) {
        const std::string& name = part.find("name")->as_string();
        out[name] = part.find("bytes")->as_int();
        if (const json::Value* items = part.find("items"); items != nullptr) {
            for (const json::Value& item : items->as_array()) {
                out[name + "/" + item.find("name")->as_string()] = item.find("bytes")->as_int();
            }
        }
    }
    return out;
}

// What grew the most between two reports, among the keys with no '/' or those under part.
std::string grew_most(const std::map<std::string, std::int64_t>& before,
                      const std::map<std::string, std::int64_t>& after, std::string_view part) {
    std::string best;
    std::int64_t most = 0;
    for (const auto& [key, bytes] : after) {
        const bool wanted = part.empty() ? key.find('/') == std::string::npos
                                         : key.starts_with(std::string(part) + "/");
        const auto was = before.find(key);
        const std::int64_t growth = bytes - (was != before.end() ? was->second : 0);
        if (wanted && growth > most) {
            most = growth;
            best = part.empty() ? key : key.substr(part.size() + 1);
        }
    }
    return best.empty() ? "nothing" : best;
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    components.add<comets::Trail>().set_doc("Where a comet has been.");
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    World& world = session.world();

    for (int i = 0; i < kComets; ++i) {
        const entt::entity comet = world.create("comet" + std::to_string(i)).value();
        world.ecs().emplace<Transform2D>(comet).position = {0.0F, static_cast<float>(i) * 4.0F};
        world.ecs().emplace<comets::Trail>(comet);
    }

    world.add_system("trail", trail_forever, {.doc = "Moves comets and records their trails."});
    call(session, "step", R"({"ticks": 60})");
    const json::Value first = call(session, "perf.stats", R"({"memory": true, "top": 3})");
    std::vector<std::string> names;
    for (const json::Value& part : first.find("memory")->find("parts")->as_array()) {
        names.push_back(part.find("name")->as_string());
    }
    std::ranges::sort(names);
    std::string listed;
    for (const std::string& name : names) {
        listed += (listed.empty() ? "" : ", ") + name;
    }
    std::printf("perf.stats {\"memory\": true} reports %zu parts: %s\n", names.size(),
                listed.c_str());
    const json::Value& process = *first.find("memory")->find("process");
    std::printf("  and the process's, whose peak is at least what it uses now: %s\n",
                process.find("peak")->as_int() >= process.find("resident")->as_int() ? "yes"
                                                                                     : "no");

    // Ten seconds later.
    call(session, "step", R"({"ticks": 600})");
    const json::Value second = call(session, "perf.stats", R"({"memory": true, "top": 3})");
    const auto before = sizes(first);
    const auto after = sizes(second);
    const std::string part = grew_most(before, after, "");
    std::printf("over 600 ticks, the part that grew the most: %s\n", part.c_str());
    std::printf("  and its item that grew the most: %s\n", grew_most(before, after, part).c_str());

    // Trails that keep their last 32 points.
    world.remove_system("trail");
    world.add_system("trail", trail_capped, {.doc = "Moves comets and records their trails."});
    call(session, "step", R"({"ticks": 60})");
    const auto capped = sizes(call(session, "perf.stats", R"({"memory": true})"));
    call(session, "step", R"({"ticks": 600})");
    const auto later = sizes(call(session, "perf.stats", R"({"memory": true})"));
    std::printf("with trails capped at %zu points, components stay the same size: %s\n", kKept,
                later.at("components") == capped.at("components") ? "yes" : "no");
    return 0;
}
