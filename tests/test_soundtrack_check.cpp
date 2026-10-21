#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/soundtrack/render.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cmath>
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

constexpr int kRate = soundtrack::kRenderRate;

// Stereo frames, seconds long, each sample from sample(i, frames).
template <typename Sample>
std::string wav(double seconds, Sample sample) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> samples(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        samples[2 * i] = samples[(2 * i) + 1] = sample(i, frames);
    }
    return soundtrack::encode_wav(samples, kRate);
}

// A triangle wave at 441 Hz, which loops cleanly at a whole number of its periods.
std::string tone(double seconds, float level = 0.1F) {
    return wav(seconds, [level](std::size_t i, std::size_t) {
        const auto into = static_cast<float>(i % 100);
        return level * ((std::abs(into - 50.0F) / 25.0F) - 1.0F);
    });
}

// One rise from low to high across the sound, so its loop jumps back down.
std::string ramp(double seconds) {
    return wav(seconds, [](std::size_t i, std::size_t frames) {
        return -0.5F + (static_cast<float>(i) / static_cast<float>(frames));
    });
}

// A short click on each beat at bpm.
std::string pulse(double seconds, double bpm) {
    const auto beat = static_cast<std::size_t>(kRate * 60.0 / bpm);
    return wav(seconds, [beat](std::size_t i, std::size_t) {
        const std::size_t into = i % beat;
        return into < 400 ? ((into / 20) % 2 == 0 ? 0.8F : -0.8F) : 0.0F;
    });
}

} // namespace

TEST_CASE("soundtrack.check finds what keeps the music from playing as planned") {
    const std::string root = "command_soundtrack_check_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/music").ok());
    REQUIRE(slash::platform::create_directories(root + "/storyboard").ok());
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    };
    // calm loops eight beats cleanly; short says four beats but lasts one; ramp clicks where it
    // loops; pulse beats at 100 but says 120; fast is in a far key from calm's.
    write("music/calm.wav", tone(4.0));
    write("music/calm.audio.json", R"({"format": 1, "source": "music/calm.wav",
        "loop_mode": "forward", "bpm": 120, "beat_count": 8})");
    write("music/calm.score.json", R"({"format": 1, "bpm": 120, "key": "C major", "tracks": []})");
    write("music/short.wav", tone(1.0));
    write("music/short.audio.json", R"({"format": 1, "source": "music/short.wav", "bpm": 120,
        "beat_count": 4})");
    write("music/ramp.wav", ramp(2.0));
    write("music/ramp.audio.json", R"({"format": 1, "source": "music/ramp.wav",
        "loop_mode": "forward", "bpm": 120, "beat_count": 4})");
    write("music/pulse.wav", pulse(9.6, 100.0));
    write("music/pulse.audio.json", R"({"format": 1, "source": "music/pulse.wav", "bpm": 120,
        "beat_count": 16})");
    write("music/fast.wav", tone(4.0));
    write("music/fast.audio.json", R"({"format": 1, "source": "music/fast.wav",
        "loop_mode": "forward", "bpm": 150, "beat_count": 10})");
    write("music/fast.score.json", R"({"format": 1, "bpm": 150, "key": "F# major", "tracks": []})");
    write("music/raw.wav", pulse(2.0, 90.0));
    write("music/score.audio.json", R"({"format": 1, "clips": [
            {"name": "calm", "stream": "music/calm.audio.json"},
            {"name": "fast", "stream": "music/fast.audio.json"},
            {"name": "raw", "stream": "music/raw.wav"}],
        "transitions": [{"from_time": "next_bar"},
            {"from": "calm", "to": "fast", "from_time": "next_bar", "to_time": "same_position"},
            {"from": "fast", "to": "calm", "from_time": "immediate"}]})");
    write("storyboard/music.json", R"({"format": 1, "entries": {
        "cue/boss": {"name": "Boss", "music": ["music/boss.score.json"]},
        "cue/quiet": {"name": "Quiet"},
        "loudness/calm": {"name": "Calm", "lufs": -16, "links": ["music/calm.audio.json"]}}})");

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

    const auto problems = [](const json::Value& list) {
        std::vector<std::string> out;
        for (const json::Value& each : list.as_array()) {
            out.push_back(compact(each));
        }
        return out;
    };

    slash::command::Reply reply = call("soundtrack.check", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["checked"].as_int() == 6);
    std::vector<std::string> errors = problems(reply.value["errors"]);
    // Loudness is measured, so only its start and its advice are checked.
    REQUIRE(errors.size() == 5);
    CHECK(errors.back().starts_with(R"({"file":"music/calm.audio.json","message":"measures -2)"));
    CHECK(errors.back().find(R"( LUFS; loudness/calm asks for -16 ± 2, so raise it )") !=
          std::string::npos);
    errors.pop_back();
    CHECK(errors ==
          std::vector<std::string>{
              R"({"file":"storyboard/music.json","message":"cue/boss's score )"
              R"(music/boss.score.json isn't rendered yet; soundtrack.render writes )"
              R"(music/boss.audio.json","path":"/entries/cue~1boss"})",
              R"({"file":"music/ramp.audio.json","message":"the loop clicks where its end joins )"
              R"(its start","path":""})",
              R"({"file":"music/short.audio.json","message":"4 beats at 120 bpm last 2 s, longer )"
              R"(than the sound's 1 s, so the music's beats run past its end; the sound holds 2 )"
              R"(beats","path":"/beat_count"})",
              R"({"file":"music/score.audio.json","message":"raw's stream has no bpm, so )"
              R"(switches from it that wait for the beat or bar happen at once; give its )"
              R"(.audio.json file bpm and beat_count","path":"/clips/2"})",
          });
    CHECK(problems(reply.value["warnings"]) ==
          std::vector<std::string>{
              R"({"file":"storyboard/music.json","message":"cue/quiet has no music yet; name its )"
              R"(files in music","path":"/entries/cue~1quiet"})",
              R"({"file":"music/fast.audio.json","message":"10 beats isn't a whole number of )"
              R"(4-beat bars, so each time round the loop starts mid-bar","path":"/beat_count"})",
              R"({"file":"music/pulse.audio.json","message":"sounds like 100 bpm, but bpm is )"
              R"(120","path":"/bpm"})",
              R"({"file":"music/score.audio.json","message":"switches from fast to calm come at )"
              R"(once, off the bar; wait for next_bar, or keep the place with )"
              R"(same_position","path":"/transitions/2"})",
              R"({"file":"music/score.audio.json","message":"switches from calm to fast (120 to )"
              R"(150 bpm) keep the same position though the beats differ, so the next clip )"
              R"(comes in off its beat","path":"/transitions/1"})",
              R"({"file":"music/score.audio.json","message":"calm (C major) and fast (F# major) )"
              R"(are 6 steps apart round the circle of fifths, and the music goes straight from )"
              R"(one to the other; a filler clip, or a nearer key, joins them","path":"/clips/1"})",
          });

    // Only some music: a file and what it plays, and the cues that name it.
    reply = call("soundtrack.check", R"({"paths": ["music/short.audio.json"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["checked"].as_int() == 1);
    REQUIRE(reply.value.find("errors") != nullptr);
    CHECK(reply.value["errors"].as_array().size() == 1);
    CHECK(reply.value.find("warnings") == nullptr);

    // A clean file is quiet: the loop fixed, the beats fitted and the target met.
    write("music/ramp.audio.json", R"({"format": 1, "source": "music/ramp.wav", "bpm": 120,
        "beat_count": 4})");
    write("music/short.audio.json", R"({"format": 1, "source": "music/short.wav", "bpm": 120,
        "beat_count": 2})");
    reply = call("soundtrack.check", R"({"paths": ["music/ramp.audio.json",
        "music/short.audio.json"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(compact(reply.value) == R"({"checked":2})");

    // The whole project's test run fails on the music's errors, not its warnings.
    reply = call("test.run", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const std::vector<std::string> failed = problems(reply.value["failed"]);
    REQUIRE(failed.size() == 3);
    CHECK(failed[0].find("cue/boss's score") != std::string::npos);
    CHECK(failed[1].find("raw's stream has no bpm") != std::string::npos);
    CHECK(failed[2].find("loudness/calm") != std::string::npos);
}
