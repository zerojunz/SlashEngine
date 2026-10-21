#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/arrange.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/soundtrack/render.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace soundtrack = slash::soundtrack;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// Stereo silence but for a quiet square wave, seconds long.
std::string tone(double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * soundtrack::kRenderRate);
    std::vector<float> samples(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        samples[2 * i] = samples[(2 * i) + 1] = (i / 50) % 2 == 0 ? 0.1F : -0.1F;
    }
    return soundtrack::encode_wav(samples, soundtrack::kRenderRate);
}

} // namespace

TEST_CASE("arrange turns cues into clips, layers and stings of interactive music") {
    const json::Object entries = parse(R"({
        "cue/explore": {"name": "Explore", "layers": ["calm", "Tense!"],
                        "music": ["music/explore/base.audio.json", "music/explore/drums.ogg"]},
        "cue/combat": {"name": "Combat", "music": ["music/combat.score.json"]},
        "cue/victory": {"name": "Victory", "plays": "once", "after": ["cue/combat"],
                        "music": ["music/victory.wav"]},
        "cue/pickup": {"name": "Pickup", "plays": "once", "music": ["music/pickup.wav"]},
        "cue/menu": {"name": "Menu", "music": ["music/menu.mid"]},
        "cue/credits": {"name": "Credits", "layers": ["a", "b"], "music": ["music/end.ogg"]},
        "cue/silence": {"name": "Silence"}
    })")
                                     .as_object();
    const auto exists = [](std::string_view path) { return path != "music/combat.audio.json"; };
    const soundtrack::Arranged arranged = soundtrack::arrange(entries, {}, exists);

    CHECK(compact(arranged.music) ==
          R"({"clips":[)"
          R"({"name":"combat","stream":"music/combat.audio.json"},)"
          R"({"name":"credits","stream":"music/end.ogg"},)"
          R"({"name":"explore_calm","stream":"music/explore/base.audio.json"},)"
          R"({"name":"explore_tense_","stream":"music/soundtrack/explore_tense_.audio.json"},)"
          R"({"auto_advance":"return_to_hold","name":"pickup","stream":"music/pickup.wav"},)"
          R"({"name":"victory","stream":"music/victory.wav"}],)"
          R"("format":1,"transitions":[)"
          R"({"from_time":"next_bar"},)"
          R"({"from":"explore_calm","from_time":"next_beat","to":"explore_tense_",)"
          R"("to_time":"same_position"},)"
          R"({"from":"explore_tense_","from_time":"next_beat","to":"explore_calm",)"
          R"("to_time":"same_position"},)"
          R"({"from_time":"next_beat","hold_previous":true,"to":"pickup"},)"
          R"({"from":"pickup","from_time":"next_bar","to_time":"previous_position"},)"
          R"({"filler_clip":"victory","from":"combat","from_time":"next_bar"}]})");
    REQUIRE(arranged.files.size() == 1);
    CHECK(arranged.files[0].first == "music/soundtrack/explore_tense_.audio.json");
    CHECK(compact(arranged.files[0].second) ==
          R"({"format":1,"synchronized":[{"stream":"music/explore/base.audio.json"},)"
          R"({"stream":"music/explore/drums.ogg"}]})");
    CHECK(compact(arranged.clips[2]) == R"({"cue":"cue/explore","layer":"calm",)"
                                        R"("name":"explore_calm",)"
                                        R"("stream":"music/explore/base.audio.json"})");
    CHECK(arranged.problems ==
          std::vector<std::string>{
              "cue/combat's score music/combat.score.json isn't rendered yet; soundtrack.render "
              "writes music/combat.audio.json",
              "cue/credits has 2 layers and 1 music files; give one file per layer, lowest first",
              "cue/menu's music music/menu.mid can't play: name .audio.json, .ogg, .wav, .mp3 or "
              ".score.json files",
              "cue/menu has no music yet; name its files in music",
              "cue/silence has no music yet; name its files in music",
          });

    // Only some cues, starting with the one asked for.
    const soundtrack::Arranged some = soundtrack::arrange(entries,
                                                          {.out = "music/level.audio.json",
                                                           .cues = {"cue/combat", "cue/explore"},
                                                           .initial = "cue/explore"},
                                                          exists);
    CHECK(compact(*some.music.find("clips")).find("victory") == std::string::npos);
    CHECK(some.music.find("initial_clip")->as_string() == "explore_calm");
    CHECK(some.files[0].first == "music/level/explore_tense_.audio.json");

    // A stem a score renders says which score.
    const soundtrack::Arranged stems = soundtrack::arrange(
        parse(R"({"cue/a": {"music": ["music/a/lead.audio.json", "music/b/lead.audio.json"]}})")
            .as_object(),
        {}, [](std::string_view path) { return path == "music/a.score.json"; });
    CHECK(stems.problems == std::vector<std::string>{
                                "cue/a's music music/a/lead.audio.json isn't rendered yet; "
                                "soundtrack.render writes it from music/a.score.json",
                                "cue/a's music music/b/lead.audio.json doesn't exist yet",
                            });
}

TEST_CASE("soundtrack.arrange writes music a player switches through on the bar") {
    const std::string root = "command_soundtrack_arrange_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/music").ok());
    REQUIRE(slash::platform::create_directories(root + "/storyboard").ok());
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    };
    // Eight beats at 120: four seconds a loop.
    for (const std::string name : {"explore_base", "explore_drums"}) {
        write("music/" + name + ".wav", tone(4.0));
        write("music/" + name + ".audio.json",
              R"({"format": 1, "source": "music/)" + name +
                  R"(.wav", "loop_mode": "forward", "bpm": 120, "beat_count": 8})");
    }
    write("music/fanfare.wav", tone(0.5));
    write("music/boss.score.json", R"({"format": 1, "bpm": 120, "tracks": [
        {"name": "low", "instrument": "cello", "bars": ["C2:4", "G1:4"]}]})");
    write("storyboard/music.json", R"({"format": 1, "entries": {
        "cue/explore": {"name": "Explore", "layers": ["calm", "tense"],
                        "music": ["music/explore_base.audio.json",
                                  "music/explore_drums.audio.json"]},
        "cue/boss": {"name": "Boss", "music": ["music/boss.score.json"]},
        "cue/win": {"name": "Win", "plays": "once", "after": ["cue/boss"],
                    "music": ["music/fanfare.wav"]}}})");

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    slash::command::Registry commands;
    slash::command::register_builtin_commands(commands);
    soundtrack::register_commands(commands);
    slash::command::Session session(
        components, commands,
        slash::command::SessionOptions{.project_root = root, .capture_logs = false});
    const auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };

    slash::command::Reply reply = call("soundtrack.arrange", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    REQUIRE(reply.value.find("problems") != nullptr);
    CHECK(compact(reply.value["problems"]) ==
          R"(["cue/boss's score music/boss.score.json isn't rendered yet; soundtrack.render )"
          R"(writes music/boss.audio.json"])");
    reply = call("soundtrack.render", R"({"path": "music/boss.score.json", "loop": true})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    reply = call("soundtrack.arrange", R"({"initial": "cue/explore"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("problems") == nullptr);
    CHECK(reply.value["path"].as_string() == "music/soundtrack.audio.json");
    CHECK(compact(reply.value["layers"]) == R"(["music/soundtrack/explore_tense.audio.json"])");
    REQUIRE(reply.value["clips"].as_array().size() == 4);

    // A game plays it and switches clips by name.
    reply = call("entity.create", R"({"id": "music", "components": {"AudioStreamPlayer":
        {"stream": "music/soundtrack.audio.json", "autoplay": true}}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const auto clip = [&] {
        const slash::command::Reply state = call("audio.state", R"({"ids": ["music"]})");
        REQUIRE_MESSAGE(state.ok, state.message);
        const json::Value& player = state.value.find("players")->as_array()[0];
        const json::Value* name = player.find("clip");
        return name != nullptr ? name->as_string() : std::string();
    };
    const auto position = [&] {
        const slash::command::Reply state = call("audio.state", R"({"ids": ["music"]})");
        return state.value.find("players")->as_array()[0].find("position")->as_double();
    };
    const auto step = [&](int ticks) {
        const slash::command::Reply stepped =
            call("step", R"({"ticks": )" + std::to_string(ticks) + "}");
        REQUIRE_MESSAGE(stepped.ok, stepped.message);
    };
    const auto switch_to = [&](std::string_view name) {
        const slash::command::Reply set =
            call("component.set", R"({"id": "music", "components": {"AudioStreamPlayer":
            {"parameters": {"switch_to_clip": ")" +
                                      std::string(name) + R"("}}}})");
        REQUIRE_MESSAGE(set.ok, set.message);
    };
    step(30);
    CHECK(clip() == "explore_calm");

    // A layer change keeps the music's place, on the next beat.
    step(30); // a second in
    switch_to("explore_tense");
    step(30);
    CHECK(clip() == "explore_tense");
    CHECK(position() == doctest::Approx(1.5).epsilon(0.1));

    // Another cue waits for the bar: two seconds in, at four beats to a bar.
    switch_to("boss");
    step(6);
    CHECK(clip() == "explore_tense");
    step(60);
    CHECK(clip() == "boss");

    // Leaving the boss plays the win sting first.
    switch_to("explore_calm");
    std::vector<std::string> heard;
    for (int i = 0; i < 40 && (heard.empty() || heard.back() != "explore_calm"); ++i) {
        step(10);
        if (heard.empty() || heard.back() != clip()) {
            heard.push_back(clip());
        }
    }
    CHECK(heard == std::vector<std::string>{"boss", "win", "explore_calm"});

    // Errors: an unknown cue, and no music at all.
    reply = call("soundtrack.arrange", R"({"cues": ["cue/explor"]})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message.find("did you mean \"cue/explore\"?") != std::string::npos);
    write("storyboard/music.json", R"({"format": 1, "entries": {"cue/quiet": {"name": "Quiet"}}})");
    reply = call("soundtrack.arrange", "{}");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message == "no cue has music yet; name each cue's files in its music field");
}
