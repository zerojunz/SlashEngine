#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using slash::world::ComponentRegistry;
using slash::world::World;

namespace stage_test {

struct Heat {
    float value = 0.0F;
};
SLASH_COMPONENT(Heat, value)

struct Charge {
    float value = 0.0F;
};
SLASH_COMPONENT(Charge, value)

} // namespace stage_test

namespace memory_test {

struct Bag {
    std::vector<std::string> items;
    std::map<std::string, int> counts;
    std::string label;
};
SLASH_COMPONENT(Bag, items, counts, label)

} // namespace memory_test

TEST_CASE("entities are created and found by stable ID") {
    const ComponentRegistry registry;
    World world(registry);
    auto player = world.create("player");
    REQUIRE(player.ok());
    CHECK(world.find("player") == player.value());
    CHECK(world.id_of(player.value()) == "player");
    CHECK(world.size() == 1);
    CHECK(world.find("nobody") == entt::entity{entt::null});
}

TEST_CASE("IDs must be well formed and unique") {
    const ComponentRegistry registry;
    World world(registry);
    REQUIRE(world.create("enemy_03").ok());
    auto duplicate = world.create("enemy_03");
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().message == R"(entity id "enemy_03" is already in use)");
    CHECK_FALSE(world.create("").ok());
    CHECK_FALSE(world.create("has space").ok());
    CHECK_FALSE(world.create(std::string(65, 'a')).ok());
    CHECK(World::is_valid_id("Boss-2_final"));
}

TEST_CASE("an instance's entities have IDs under its own and go with it") {
    const ComponentRegistry registry;
    World world(registry);
    CHECK(World::is_valid_part_id("hero/sprite"));
    CHECK(World::is_valid_part_id("hero/gun/muzzle"));
    CHECK_FALSE(World::is_valid_part_id("hero"));
    CHECK_FALSE(World::is_valid_part_id("hero/"));
    CHECK_FALSE(World::is_valid_part_id("/sprite"));
    CHECK_FALSE(World::is_valid_part_id("hero//sprite"));
    CHECK_FALSE(World::is_valid_part_id("hero/" + std::string(251, 'a')));
    CHECK_FALSE(world.create("hero/sprite").ok());
    CHECK_FALSE(world.create_part("hero").ok());

    const entt::entity hero = world.create("hero").value();
    REQUIRE(world.create_part("hero/sprite").ok());
    REQUIRE(world.create_part("hero/gun/muzzle").ok());
    REQUIRE(world.create("hero-2").ok());
    REQUIRE(world.create("heroine").ok());
    world.destroy(hero);
    CHECK(world.ids() == std::vector<std::string_view>{"hero-2", "heroine"});
}

TEST_CASE("generated IDs skip ones already taken") {
    const ComponentRegistry registry;
    World world(registry);
    REQUIRE(world.create("entity_1").ok());
    const entt::entity generated = world.create();
    CHECK(world.id_of(generated) == "entity_2");
}

TEST_CASE("destroying an entity frees its ID, even through the raw registry") {
    const ComponentRegistry registry;
    World world(registry);
    const entt::entity a = world.create("a").value();
    const entt::entity b = world.create("b").value();
    world.destroy(a);
    world.ecs().destroy(b);
    CHECK(world.size() == 0);
    CHECK(world.find("a") == entt::entity{entt::null});
    CHECK(world.create("b").ok());
    CHECK(world.id_of(a).empty());
}

TEST_CASE("systems run in the order they were added, once per step") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    world.add_system("input", [&](World& w, double dt) {
        calls.push_back("input " + std::to_string(w.tick()) + " " + std::to_string(dt));
    });
    world.add_system("physics",
                     [&](World& /*w*/, double /*dt*/) { calls.emplace_back("physics"); });
    world.step(0.5);
    world.step(0.5);
    CHECK(world.tick() == 2);
    CHECK(calls ==
          std::vector<std::string>{"input 0 0.500000", "physics", "input 1 0.500000", "physics"});
    CHECK(world.system_names() == std::vector<std::string_view>{"input", "physics"});
}

TEST_CASE("systems run in order of priority, then in the order they were added") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    const auto record = [&](std::string name) {
        return [&calls, name](World& /*w*/, double /*dt*/) { calls.push_back(name); };
    };
    world.add_system("render", record("render"), {.doc = "Draws.", .priority = 100});
    world.add_system("movement", record("movement"));
    world.add_system("input", record("input"), {.priority = -10});
    world.add_system("collisions", record("collisions"));
    world.step(0.5);
    CHECK(calls == std::vector<std::string>{"input", "movement", "collisions", "render"});

    const std::vector<slash::world::SystemInfo> systems = world.systems();
    REQUIRE(systems.size() == 4);
    CHECK(systems.back().name == "render");
    CHECK(systems.back().doc == "Draws.");
    CHECK(systems.back().priority == 100);
    CHECK(systems.front().priority == -10);
}

TEST_CASE("systems see each event once, at the tick after it came, whatever their order") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> early;
    std::vector<std::string> late;
    const auto record = [](std::vector<std::string>& seen) {
        return [&seen](World& w, double /*dt*/) {
            for (const slash::world::Event& event : w.events()) {
                seen.push_back(std::to_string(w.tick()) + " " + event.kind + " " + event.id + " " +
                               std::to_string(event.tick));
            }
        };
    };
    world.add_system("early", record(early), {.priority = -10});
    world.add_system("emitter", [](World& w, double /*dt*/) {
        if (w.tick() == 1) {
            w.emit("hit", "coin", slash::json::Value(slash::json::Object{{"damage", 2}}));
        }
    });
    world.add_system("late", record(late), {.priority = 10});
    // Events between ticks, such as a loaded scene's entities, come at the next tick.
    REQUIRE(world.create("coin").ok());
    world.step(0.5);
    world.step(0.5);
    CHECK(world.events().empty());
    world.step(0.5);
    REQUIRE(world.events().size() == 1);
    CHECK(world.events().front().data == slash::json::Value(slash::json::Object{{"damage", 2}}));
    world.step(0.5);
    const std::vector<std::string> expected{"0 entity.created coin 0", "2 hit coin 1"};
    CHECK(early == expected);
    CHECK(late == expected);

    // A rolled back command's events don't come; reset_state forgets them all.
    World::EventQueues before = world.event_queues();
    world.destroy(world.find("coin"));
    world.set_event_queues(std::move(before));
    world.step(0.5);
    CHECK(world.events().empty());
    world.emit("hit", "coin");
    world.reset_state();
    world.step(0.5);
    CHECK(world.events().empty());
}

TEST_CASE("reset_state forgets what systems keep, and the step listener hears each tick first") {
    const ComponentRegistry registry;
    World world(registry);
    int counted = 0;
    world.add_system("count", [&](World& w, double /*dt*/) { counted += w.stepping() ? 1 : 100; });
    world.add_reset([&](World& /*w*/) { counted = 0; });
    std::vector<std::string> heard;
    world.set_step_listener([&](World& w, double dt) {
        heard.push_back(std::to_string(w.tick()) + " " + std::to_string(dt) +
                        (w.stepping() ? " stepping" : ""));
    });
    REQUIRE(world.create("player").ok());
    world.step(0.5);
    world.step(0.25);
    CHECK(counted == 2);
    CHECK(heard == std::vector<std::string>{"0 0.500000", "1 0.250000"});

    // The entities and the tick stay.
    world.reset_state();
    CHECK(counted == 0);
    CHECK(world.size() == 1);
    CHECK(world.tick() == 2);

    world.set_step_listener({});
    world.step(0.5);
    CHECK(heard.size() == 2);
}

TEST_CASE("deferred calls run after the system that made them, and at once outside systems") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    world.add_system("first", [&](World& w, double /*dt*/) {
        w.defer([&](World& later) {
            calls.push_back("deferred " + std::to_string(later.size()));
            later.defer([&](World& /*again*/) { calls.emplace_back("deferred again"); });
            REQUIRE(later.create("made").ok());
        });
        calls.emplace_back("first");
    });
    world.add_system("second", [&](World& w, double /*dt*/) {
        calls.push_back("second " + std::to_string(w.size()));
    });
    world.step(0.5);
    CHECK(calls == std::vector<std::string>{"first", "deferred 0", "deferred again", "second 1"});
    world.defer([&](World& /*w*/) { calls.emplace_back("now"); });
    CHECK(calls.back() == "now");
}

TEST_CASE("threaded systems next to each other share a stage unless they clash") {
    const ComponentRegistry registry;
    World world(registry);
    const auto none = [](World& /*w*/, double /*dt*/) {};
    world.add_system("a", none,
                     {.priority = 1, .threaded = true, .reads = {"Heat"}, .writes = {"Charge"}});
    // Reading what another reads is no clash.
    world.add_system("b", none, {.priority = 2, .threaded = true, .reads = {"Heat"}});
    // a writes what c reads.
    world.add_system("c", none, {.priority = 3, .threaded = true, .reads = {"Charge"}});
    world.add_system("d", none, {.priority = 4});
    // A system that isn't threaded runs alone, so e starts a stage; f writes other state.
    world.add_system("e", none, {.priority = 5, .threaded = true, .writes = {"Heat"}});
    world.add_system("f", none, {.priority = 6, .threaded = true, .writes = {"assets"}});
    std::vector<std::string> stages;
    for (const slash::world::SystemInfo& system : world.systems()) {
        stages.push_back(std::string(system.name) + " " + std::to_string(system.stage) +
                         (system.threaded ? " threaded" : ""));
    }
    CHECK(stages == std::vector<std::string>{"a 0 threaded", "b 0 threaded", "c 1 threaded", "d 2",
                                             "e 3 threaded", "f 3 threaded"});
    REQUIRE(world.remove_system("c"));
    CHECK(world.systems()[2].name == "d");
    CHECK(world.systems()[2].stage == 1);
}

namespace {

// Ticks of threaded work that writes components, emits events and makes an entity, as one
// line per event and the components after.
std::vector<std::string> run_threaded(int threads) {
    ComponentRegistry registry;
    registry.add<stage_test::Heat>();
    registry.add<stage_test::Charge>();
    slash::jobs::Pool pool(threads);
    World world(registry);
    world.set_pool(pool);
    std::vector<std::string> lines;
    world.set_event_listener([&](const slash::world::Event& event) {
        lines.push_back(std::to_string(event.tick) + " " + event.kind +
                        (event.id.empty() ? "" : " " + event.id));
    });
    for (int i = 0; i < 20; ++i) {
        const entt::entity entity = world.create("e" + std::to_string(i)).value();
        world.ecs().emplace<stage_test::Heat>(entity, static_cast<float>(i));
        world.ecs().emplace<stage_test::Charge>(entity);
    }
    world.add_system("warm",
                     [](World& w, double /*dt*/) {
                         for (auto [entity, heat] : w.ecs().view<stage_test::Heat>().each()) {
                             heat.value += 1.5F;
                             if (heat.value > 25.0F && heat.value <= 26.5F) {
                                 w.emit("hot", w.id_of(entity));
                             }
                         }
                         if (w.tick() == 4) {
                             w.defer([](World& later) { REQUIRE(later.create("spark").ok()); });
                         }
                     },
                     {.threaded = true, .writes = {"Heat"}});
    world.add_system("charge",
                     [](World& w, double /*dt*/) {
                         auto view = w.ecs().view<stage_test::Charge>();
                         const std::vector<entt::entity> entities(view.begin(), view.end());
                         w.pool().parallel_for(entities.size(), [&](std::size_t i) {
                             w.ecs().get<stage_test::Charge>(entities[i]).value +=
                                 static_cast<float>(i);
                         });
                         w.emit("charged");
                     },
                     {.threaded = true, .writes = {"Charge"}});
    world.add_system("count", [&lines](World& w, double /*dt*/) {
        lines.push_back(std::to_string(w.tick()) + " count " + std::to_string(w.size()));
    });
    for (int i = 0; i < 5; ++i) {
        world.step(0.5);
    }
    for (auto [entity, heat, charge] :
         world.ecs().view<stage_test::Heat, stage_test::Charge>().each()) {
        lines.push_back(std::string(world.id_of(entity)) + " " + std::to_string(heat.value) + " " +
                        std::to_string(charge.value));
    }
    return lines;
}

} // namespace

TEST_CASE("threaded systems run the same ticks with any number of threads") {
    const std::vector<std::string> alone = run_threaded(0);
    CHECK(run_threaded(1) == alone);
    CHECK(run_threaded(3) == alone);
    // A stage's events and deferred calls go out in system order once it is done: warm's, then
    // the entity it made, then charge's.
    const auto spark = std::ranges::find(alone, "4 entity.created spark");
    REQUIRE(spark - alone.begin() >= 2);
    REQUIRE(alone.end() - spark >= 3);
    const std::vector<std::string> tick_four(spark - 2, spark + 3);
    CHECK(tick_four == std::vector<std::string>{"4 hot e19", "4 hot e18", "4 entity.created spark",
                                                "4 charged", "4 count 21"});
}

namespace {

// Keeps the thread busy for at least this long.
void spin(std::chrono::microseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
    }
}

const slash::world::SystemInfo& system_named(const std::vector<slash::world::SystemInfo>& systems,
                                             std::string_view name) {
    const auto found = std::ranges::find(systems, name, &slash::world::SystemInfo::name);
    REQUIRE(found != systems.end());
    return *found;
}

} // namespace

TEST_CASE("the world times each tick and system over the last 120 ticks") {
    const ComponentRegistry registry;
    slash::jobs::Pool pool(2);
    World world(registry);
    world.set_pool(pool);
    CHECK(world.tick_timing().avg_ms == 0.0);
    // Slow for the first ten ticks, then quick: the window keeps only the last 120.
    world.add_system("burst", [](World& w, double /*dt*/) {
        if (w.tick() < 10) {
            spin(std::chrono::milliseconds(20));
        }
    });
    world.add_system("busy",
                     [](World& /*w*/, double /*dt*/) { spin(std::chrono::microseconds(300)); },
                     {.priority = 1});
    // Threaded systems are timed on the threads that run them, with what they defer.
    world.add_system("left",
                     [](World& w, double /*dt*/) {
                         spin(std::chrono::microseconds(200));
                         w.defer([](World& /*later*/) { spin(std::chrono::microseconds(200)); });
                     },
                     {.priority = 2, .threaded = true, .writes = {"left"}});
    world.add_system("right", [](World& /*w*/, double /*dt*/) {},
                     {.priority = 2, .threaded = true, .writes = {"right"}});
    CHECK(system_named(world.systems(), "busy").timing.max_ms == 0.0);

    for (int tick = 0; tick < 10; ++tick) {
        world.step(1.0 / 60.0);
    }
    CHECK(system_named(world.systems(), "burst").timing.max_ms >= 20.0);
    for (int tick = 10; tick < 130; ++tick) {
        world.step(1.0 / 60.0);
    }
    const std::vector<slash::world::SystemInfo> systems = world.systems();
    const slash::world::Timing burst = system_named(systems, "burst").timing;
    CHECK(burst.max_ms < 10.0);
    const slash::world::Timing busy = system_named(systems, "busy").timing;
    CHECK(busy.last_ms >= 0.3);
    CHECK(busy.avg_ms >= 0.3);
    CHECK(busy.max_ms >= busy.avg_ms);
    const slash::world::Timing left = system_named(systems, "left").timing;
    CHECK(left.avg_ms >= 0.4);
    CHECK(system_named(systems, "right").timing.avg_ms < left.avg_ms);
    const slash::world::Timing tick = world.tick_timing();
    CHECK(tick.avg_ms >= busy.avg_ms + 0.4);
    CHECK(tick.max_ms >= tick.avg_ms);
}

namespace {

const slash::world::MemoryPart* part_named(const std::vector<slash::world::MemoryPart>& parts,
                                           std::string_view name) {
    const auto found = std::ranges::find(parts, name, &slash::world::MemoryPart::name);
    return found != parts.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("heap_bytes counts what values hold beyond their own size") {
    using slash::world::heap_bytes;
    const std::string long_text(100, 'x');
    CHECK(heap_bytes(std::string("hi")) == 0);
    CHECK(heap_bytes(long_text) > long_text.size());
    std::vector<int> numbers;
    numbers.reserve(50);
    CHECK(heap_bytes(numbers) == numbers.capacity() * sizeof(int));
    // Each long string's buffer, on top of the vector's.
    const std::vector<std::string> strings(3, long_text);
    CHECK(heap_bytes(strings) >= (strings.capacity() * sizeof(std::string)) + (3 * 100));
    const memory_test::Bag bag{.items = strings, .counts = {{"apples", 2}}, .label = long_text};
    CHECK(heap_bytes(bag) > heap_bytes(strings) + heap_bytes(long_text));
    CHECK(slash::json::heap_bytes(slash::json::parse(R"({"a": [1, 2, 3]})").value()) > 0);
    CHECK(slash::json::heap_bytes(slash::json::Value(3)) == 0);
}

TEST_CASE("the world reports what it holds in memory, the largest part first") {
    ComponentRegistry registry;
    registry.add<memory_test::Bag>();
    registry.add<stage_test::Heat>();
    registry.add_stand_in("Ghost", {"note"});
    World world(registry);

    std::vector<slash::world::MemoryPart> parts = world.memory();
    REQUIRE(part_named(parts, "world") != nullptr);
    REQUIRE(part_named(parts, "components") != nullptr);
    CHECK(part_named(parts, "components")->items.empty());

    const std::string long_text(200, 'y');
    for (int i = 0; i < 100; ++i) {
        const entt::entity entity = world.create("bag" + std::to_string(i)).value();
        world.ecs().emplace<memory_test::Bag>(entity).items.assign(4, long_text);
        if (i < 10) {
            world.ecs().emplace<stage_test::Heat>(entity);
        }
    }
    std::string path;
    REQUIRE(
        registry.find("Ghost")
            ->assign(world, world.find("bag0"), slash::json::parse(R"({"note": 1})").value(), path)
            .ok());
    parts = world.memory();
    const slash::world::MemoryPart* own = part_named(parts, "world");
    CHECK(own->counts.at("entities") == 100);
    CHECK(own->bytes > 0);
    const slash::world::MemoryPart* components = part_named(parts, "components");
    CHECK(components->counts.at("types") == 3);
    CHECK(components->counts.at("components") == 111);
    REQUIRE(components->items.size() == 3);
    // The largest first: each bag holds four strings of 200 characters.
    const slash::world::MemoryItem& bags = components->items.front();
    CHECK(bags.name == "Bag");
    CHECK(bags.count == 100);
    CHECK(bags.bytes >= 100 * ((4 * 200) + sizeof(memory_test::Bag)));
    CHECK(components->items[1].bytes >= components->items[2].bytes);
    std::uint64_t sum = 0;
    for (const slash::world::MemoryItem& item : components->items) {
        sum += item.bytes;
    }
    CHECK(components->bytes == sum);

    // A bag that grows shows in its type's bytes.
    const std::uint64_t before = bags.bytes;
    world.ecs().get<memory_test::Bag>(world.find("bag1")).items.assign(1000, long_text);
    parts = world.memory();
    CHECK(part_named(parts, "components")->items.front().bytes >= before + (996 * 200));

    // A source adds a part under its name, replaced by a source of the same name.
    world.add_memory_source("cache", [](const World& /*world*/) {
        return slash::world::MemoryPart{"ignored", std::uint64_t{1} << 40, {{"entries", 3}}, {}};
    });
    world.add_memory_source("small", [](const World& /*world*/) {
        return slash::world::MemoryPart{"small", 1, {}, {{"a", 1, 1}, {"b", 9, 1}}};
    });
    parts = world.memory();
    CHECK(parts.front().name == "cache");
    CHECK(parts.front().counts.at("entries") == 3);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        CHECK(parts[i].bytes <= parts[i - 1].bytes);
    }
    // Items come largest first too.
    CHECK(part_named(parts, "small")->items.front().name == "b");
    world.add_memory_source("cache", [](const World& /*world*/) {
        return slash::world::MemoryPart{"cache", 2, {}, {}};
    });
    parts = world.memory();
    CHECK(std::ranges::count(parts, std::string("cache"), &slash::world::MemoryPart::name) == 1);
    CHECK(part_named(parts, "cache")->bytes == 2);
}
