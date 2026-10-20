// perf.stats in every session: how long ticks and systems take, and what the world holds in
// memory. A running game's version (live.cpp) adds frame pacing and GPU memory.

#include "slash/command/perf.hpp"

#include "slash/asset/assets.hpp"
#include "slash/command/session.hpp"
#include "slash/platform/memory.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
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

std::int64_t signed_bytes(std::uint64_t bytes) {
    return static_cast<std::int64_t>(bytes);
}

// Loaded assets by type, each type an item.
world::MemoryPart asset_memory(const asset::Assets& assets) {
    world::MemoryPart part{"assets", 0, {}, {}};
    std::map<std::string, world::MemoryItem, std::less<>> types;
    std::uint64_t count = 0;
    for (const asset::Assets::Info& info : assets.list()) {
        if (info.refs == 0 && info.bytes == 0) {
            continue; // a watched file, which the code that read it keeps
        }
        world::MemoryItem& type = types[info.type];
        type.name = info.type;
        type.bytes += info.bytes;
        ++type.count;
        part.bytes += info.bytes;
        ++count;
    }
    part.counts["assets"] = count;
    for (auto& [name, type] : types) {
        part.items.push_back(std::move(type));
    }
    std::ranges::stable_sort(
        part.items,
        [](const world::MemoryItem& a, const world::MemoryItem& b) { return a.bytes > b.bytes; });
    return part;
}

json::Value part_json(const world::MemoryPart& part, std::size_t top) {
    json::Value out;
    out["name"] = part.name;
    out["bytes"] = signed_bytes(part.bytes);
    if (!part.counts.empty()) {
        json::Value counts = json::Object{};
        for (const auto& [name, count] : part.counts) {
            counts[name] = signed_bytes(count);
        }
        out["counts"] = std::move(counts);
    }
    if (!part.items.empty() && top > 0) {
        json::Array items;
        for (std::size_t i = 0; i < std::min(top, part.items.size()); ++i) {
            json::Value item;
            item["name"] = part.items[i].name;
            item["bytes"] = signed_bytes(part.items[i].bytes);
            item["count"] = signed_bytes(part.items[i].count);
            items.push_back(std::move(item));
        }
        out["items"] = std::move(items);
    }
    return out;
}

json::Value memory_report(const Session& session, std::size_t top) {
    std::vector<world::MemoryPart> parts = session.world().memory();
    parts.push_back(asset_memory(session.assets()));
    std::ranges::stable_sort(parts, [](const world::MemoryPart& a, const world::MemoryPart& b) {
        return a.bytes > b.bytes;
    });
    json::Array listed;
    for (const world::MemoryPart& part : parts) {
        listed.push_back(part_json(part, top));
    }
    const platform::ProcessMemory process = platform::process_memory();
    json::Value out;
    out["process"]["resident"] = signed_bytes(process.resident);
    out["process"]["peak"] = signed_bytes(process.peak);
    out["parts"] = std::move(listed);
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
    if (const json::Value* memory = params.is_object() ? params.find("memory") : nullptr;
        memory != nullptr && memory->as_bool()) {
        out["memory"] = memory_report(session, top);
    }
    return out;
}

void register_perf_commands(Registry& registry) {
    json::Value top;
    top["type"] = "integer";
    top["description"] = "How many systems to list, the slowest first, and items in each memory "
                         "part, the largest first; 0 for none.";
    top["minimum"] = 0;
    top["maximum"] = 1000;
    top["default"] = kDefaultTop;
    json::Value memory;
    memory["type"] = "boolean";
    memory["description"] = "Also report memory in bytes: the process's, and each part of the "
                            "engine's (world, components, assets, physics2d, audio...).";
    memory["default"] = false;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"]["top"] = std::move(top);
    schema["properties"]["memory"] = std::move(memory);
    registry.add({
        .name = "perf.stats",
        .description = "Tick and system times in ms over the last 120 ticks (avg and max), the "
                       "slowest systems first, and with memory, what each part of the engine "
                       "holds in memory.",
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
