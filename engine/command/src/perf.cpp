// perf.stats in every session: how long ticks and systems take. A running game's version
// (live.cpp) adds frame pacing.

#include "slash/command/perf.hpp"

#include "slash/command/session.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kDefaultTop = 10;

// Microseconds are as fine as steady clocks reliably go.
double round3(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

json::Value times(const world::Timing& timing) {
    json::Value out;
    out["avg"] = round3(timing.avg_ms);
    out["max"] = round3(timing.max_ms);
    return out;
}

} // namespace

json::Value world_perf_stats(const Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const json::Value* top_param = params.is_object() ? params.find("top") : nullptr;
    const auto top =
        static_cast<std::size_t>(top_param != nullptr ? top_param->as_int() : kDefaultTop);
    // The slowest first, so the first few say where a tick's time goes.
    std::vector<world::SystemInfo> systems = world.systems();
    std::ranges::stable_sort(systems, [](const world::SystemInfo& a, const world::SystemInfo& b) {
        return a.timing.avg_ms > b.timing.avg_ms;
    });
    json::Array listed;
    for (std::size_t i = 0; i < std::min(top, systems.size()); ++i) {
        json::Value system = times(systems[i].timing);
        system["name"] = systems[i].name;
        system["stage"] = static_cast<std::int64_t>(systems[i].stage);
        listed.push_back(std::move(system));
    }
    json::Value out;
    out["tick_ms"] = times(world.tick_timing());
    out["systems"] = std::move(listed);
    return out;
}

void register_perf_commands(Registry& registry) {
    json::Value top;
    top["type"] = "integer";
    top["description"] = "How many of the slowest systems to list; 0 for none.";
    top["minimum"] = 0;
    top["maximum"] = 1000;
    top["default"] = kDefaultTop;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"]["top"] = std::move(top);
    registry.add({
        .name = "perf.stats",
        .description = "Tick and system times in ms over the last 120 ticks (avg and max), the "
                       "slowest systems first.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler =
            [](Session& session, const json::Value& params) {
                return Reply::success(world_perf_stats(session, params));
            },
    });
}

} // namespace slash::command
