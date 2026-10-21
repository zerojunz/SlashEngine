// A music timeline: which clip played when, and every transition.
//
// The game's music director switches audio/score.audio.json's clips as the game goes: explore
// while the hero walks, boss once the boss arrives, a victory sting when it falls, which goes back
// to explore at its end. step and run.until return the clips that started while they ran, with
// the clip before, the transitions entry the switch followed and how long it waited for its beat
// or bar, so an agent checks the music follows the game without listening. playtest.run sums the
// music up over many runs.
//
// This program writes the sound files (square waves, made without libm so they are the same
// everywhere), then plays the game as an agent does over MCP or JSON-RPC.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// The fight: the boss comes at boss_tick and loses the hero's damage each tick.
struct Arena {
    int boss_tick = 90;
    int boss_hp = 240;
    int damage = 1;
    int ticks = 0;
    std::string phase = "explore"; // explore, boss, then won
    std::string music;             // the clip the music director last asked for
};
SLASH_COMPONENT(Arena, boss_tick, boss_hp, damage, ticks, phase, music)

struct Boss {
    int hp = 0;
};
SLASH_COMPONENT(Boss, hp)

} // namespace game

namespace {

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr int kRate = 44100;

// Brings the boss in, wears it down and says when the fight is won.
void fight(world::World& w, double /*tick_seconds*/) {
    for (const entt::entity entity : w.ecs().view<game::Arena>()) {
        auto& arena = w.ecs().get<game::Arena>(entity);
        ++arena.ticks;
        const entt::entity boss = w.find("boss");
        if (arena.phase == "explore" && arena.ticks == arena.boss_tick) {
            if (auto made = w.create("boss")) {
                w.ecs().emplace<game::Boss>(made.value(), arena.boss_hp);
                arena.phase = "boss";
            }
        } else if (boss != entt::null) {
            auto& hp = w.ecs().get<game::Boss>(boss).hp;
            hp -= arena.damage;
            if (hp <= 0) {
                w.destroy(boss);
                arena.phase = "won";
                w.emit("boss_defeated", "arena");
            }
        }
    }
}

// Asks the music for the clip the fight calls for when it changes: the music keeps it to the
// beat. The victory sting goes back to explore by itself.
void direct_music(world::World& w, double /*tick_seconds*/) {
    const entt::entity music = w.find("music");
    auto* player = music != entt::null ? w.ecs().try_get<world::AudioStreamPlayer>(music) : nullptr;
    for (const entt::entity entity : w.ecs().view<game::Arena>()) {
        auto& arena = w.ecs().get<game::Arena>(entity);
        const std::string clip = arena.phase == "won" ? "victory" : arena.phase;
        if (player != nullptr && clip != arena.music) {
            arena.music = clip;
            player->parameters["switch_to_clip"] = clip;
        }
    }
}

// The game's own code, as its SLASH_GAME_MODULE function would be. Every run gets its systems.
void module(world::GameModule& code) {
    code.add_component<game::Arena>().set_doc("Where the boss fight happens.");
    code.add_component<game::Boss>().set_doc("The boss and its hit points.");
    code.add_system("fight", fight, {.doc = "Brings the boss in and wears it down."});
    code.add_system("direct_music", direct_music,
                    {.doc = "Switches the music to the clip the fight calls for."});
}

// A square wave: +amplitude for half of each cycle, -amplitude for the other half.
std::vector<audio::Frame> square(int hz, float amplitude, int frames) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float value = (i * 2 * hz / kRate) % 2 == 0 ? amplitude : -amplitude;
        out[static_cast<std::size_t>(i)] = {value, value};
    }
    return out;
}

void write_sound(const std::string& path, const std::vector<audio::Frame>& frames) {
    if (!slash::platform::write_file(path, audio::encode_wav(frames, kRate))) {
        std::printf("could not write %s\n", path.c_str());
    }
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    write_sound("audio/explore.wav", square(220, 0.25F, 4 * kRate)); // eight beats at 120 BPM
    write_sound("audio/boss.wav", square(330, 0.5F, 4 * kRate));
    write_sound("audio/victory.wav", square(440, 0.25F, kRate));
    std::puts("wrote the sounds in audio/");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::gameplay::Host code(components, module);
    command::SessionOptions options;
    options.capture_logs = false;
    options.gameplay = &code;
    command::Session session(components, commands, options);

    // The arena, with the game's component added and not saved, kept to come back to.
    call(session, "scene.open", R"({"path": "arena.scene.json"})");
    call(session, "entity.create", R"({"id": "arena", "components": {"Arena": {}}})");
    call(session, "world.snapshot", R"({"name": "start"})");

    // Explore starts with the game. The boss comes at tick 90.
    call(session, "run.until", R"({"with": ["Boss"]})");

    // Boss music waits for explore's next bar, two seconds in. The hero takes four seconds to win;
    // the sting starts on the boss music's next beat.
    call(session, "run.until", R"({"with": ["Boss"], "until": "none", "ticks": 600})");

    // The sting goes back to explore at its end by itself: automatic.
    call(session, "step", R"({"ticks": 90})");

    // Back to the start, then four damages, a run each, from there: how many runs heard each
    // clip, how soon, and how often each transition came. With 0 the fight never ends, so the
    // sting never plays.
    call(session, "world.restore", R"({"name": "start"})");
    call(session, "playtest.run", R"({"vary": {"arena/Arena/damage": [0, 1, 2, 4]},
        "end": {"won": {"with": ["Arena"], "where": [
            {"component": "Arena", "field": "phase", "value": "won"}]}},
        "ticks": 480})");
    return 0;
}
