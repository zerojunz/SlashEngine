#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/soundtrack/render.hpp"
#include "slash/soundtrack/score.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace json = slash::json;
namespace soundtrack = slash::soundtrack;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return parsed.value();
}

soundtrack::Score score_of(std::string_view text) {
    auto score = soundtrack::parse_score(parse(text));
    REQUIRE_MESSAGE(score.ok(), text);
    return score.value();
}

std::vector<soundtrack::Stem> render(const soundtrack::Score& score, bool loop = false) {
    auto stems = soundtrack::render_score(score, soundtrack::builtin_sound_bank(), {.loop = loop});
    REQUIRE_MESSAGE(stems.ok(), stems.error().message);
    return std::move(stems).value();
}

// A score of one track playing one note for a beat at 120 beats a minute.
soundtrack::Score one_note(std::string_view instrument, std::string_view note) {
    return score_of(R"({"format": 1, "tracks": [{"name": "t", "instrument": ")" +
                    std::string(instrument) + R"(", "bars": [")" + std::string(note) +
                    R"( r:3"]}]})");
}

// The frequency of the left channel between two times, from where it rises through zero.
double frequency(std::span<const float> stereo, double from, double to) {
    const auto first = static_cast<std::size_t>(from * soundtrack::kRenderRate);
    const auto last = static_cast<std::size_t>(to * soundtrack::kRenderRate);
    double start = -1.0;
    double end = -1.0;
    int crossings = 0;
    for (std::size_t i = first; i + 1 < last; ++i) {
        const float a = stereo[2 * i];
        const float b = stereo[(2 * i) + 2];
        if (a < 0.0F && b >= 0.0F) {
            const double at = static_cast<double>(i) + (a / (a - b));
            if (start < 0.0) {
                start = at;
            } else {
                ++crossings;
            }
            end = at;
        }
    }
    REQUIRE(crossings > 2);
    return crossings * soundtrack::kRenderRate / (end - start);
}

std::string read(const std::string& path) {
    auto bytes = slash::platform::read_file(path);
    REQUIRE_MESSAGE(bytes.ok(), path);
    return bytes.value();
}

} // namespace

TEST_CASE("the built-in sound bank plays every instrument and drum, in tune") {
    for (const std::string_view instrument : soundtrack::instrument_names()) {
        const auto stems = render(one_note(instrument, "C4"));
        REQUIRE(stems.size() == 1);
        const double peak = soundtrack::peak_db(stems[0].samples);
        CHECK_MESSAGE(peak < -6.0, instrument);
        CHECK_MESSAGE(peak > -40.0, instrument);
    }
    for (const std::string_view drum : soundtrack::drum_names()) {
        const auto stems = render(one_note("drums", drum));
        const double peak = soundtrack::peak_db(stems[0].samples);
        CHECK_MESSAGE(peak < -3.0, drum);
        CHECK_MESSAGE(peak > -40.0, drum);
    }
    // A sine and a square wave, each at a pitch played from a different set of samples.
    for (const std::string_view instrument : {"ocarina", "lead_square"}) {
        CHECK(frequency(render(one_note(instrument, "A2"))[0].samples, 0.1, 0.4) ==
              doctest::Approx(110.0).epsilon(0.002));
        CHECK(frequency(render(one_note(instrument, "A4"))[0].samples, 0.1, 0.25) ==
              doctest::Approx(440.0).epsilon(0.002));
        CHECK(frequency(render(one_note(instrument, "A6"))[0].samples, 0.1, 0.2) ==
              doctest::Approx(1760.0).epsilon(0.002));
    }
    // The bank is made the same way every time.
    CHECK(soundtrack::builtin_sound_bank().starts_with("RIFF"));
    CHECK(soundtrack::builtin_sound_bank().substr(8, 4) == "sfbk");
}

TEST_CASE("stems ring on after the score, or loop with what rings past the end at the start") {
    const soundtrack::Score score = score_of(R"({
        "format": 1, "bpm": 120,
        "tracks": [
            {"name": "strings", "instrument": "string_ensemble_1", "bars": ["r:3 A3:1"]},
            {"name": "bass", "instrument": "acoustic_bass", "pan": -1, "bars": ["A1:4"]}
        ]
    })");
    // One bar of four beats at 120 is two seconds.
    constexpr std::size_t kLength = std::size_t{2} * soundtrack::kRenderRate;

    const auto ringing = render(score);
    REQUIRE(ringing.size() == 2);
    CHECK(ringing[0].track == "strings");
    CHECK(ringing[0].samples.size() > 2 * kLength);
    CHECK(ringing[0].samples.size() <=
          2 * (kLength + static_cast<std::size_t>(soundtrack::kMaxTail * soundtrack::kRenderRate)));
    // Before its note, the strings stem is silent.
    CHECK(soundtrack::peak_db(std::span(ringing[0].samples).first(2 * kLength / 2)) < -120.0);
    // The bass, panned hard left, has nothing on the right.
    float right = 0.0F;
    for (std::size_t i = 1; i < ringing[1].samples.size(); i += 2) {
        right = std::max(right, std::abs(ringing[1].samples[i]));
    }
    CHECK(right == 0.0F);

    const auto looping = render(score, true);
    REQUIRE(looping[0].samples.size() == 2 * kLength);
    // The strings' release, past the end, now sounds at the start, continuing from the end.
    CHECK(soundtrack::peak_db(std::span(looping[0].samples).first(200)) > -60.0);
    const std::vector<float>& wrapped = looping[0].samples;
    const float step = std::abs(wrapped[0] - wrapped[wrapped.size() - 2]);
    const float before = std::abs(wrapped[wrapped.size() - 2] - wrapped[wrapped.size() - 4]);
    CHECK(step < 4.0F * before + 1e-4F);

    // Only the tracks named.
    const std::vector<std::string> named = {"bass"};
    auto only = soundtrack::render_score(score, soundtrack::builtin_sound_bank(), {}, named);
    REQUIRE(only.ok());
    REQUIRE(only.value().size() == 1);
    CHECK(only.value()[0].track == "bass");

    auto broken = soundtrack::render_score(score, "not a sound font", {});
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message == "isn't a SoundFont 2 file");
}

TEST_CASE("stems encode as WAV and Ogg Vorbis files") {
    const std::vector<float> samples = {0.0F, 0.5F, -0.5F, 2.0F};
    const std::string wav = soundtrack::encode_wav(samples, 44100);
    CHECK(wav.size() == 44 + 8);
    CHECK(wav.substr(0, 4) == "RIFF");
    CHECK(wav.substr(8, 8) == "WAVEfmt ");
    // 0.5 is 16384, and 2 is clipped to 32767.
    CHECK(static_cast<std::uint8_t>(wav[46]) == 0x00);
    CHECK(static_cast<std::uint8_t>(wav[47]) == 0x40);
    CHECK(static_cast<std::uint8_t>(wav[50]) == 0xFF);
    CHECK(static_cast<std::uint8_t>(wav[51]) == 0x7F);

    const auto stems = render(one_note("flute", "A4"));
    auto ogg = soundtrack::encode_ogg(stems[0].samples, soundtrack::kRenderRate, 0.5, 7);
    REQUIRE(ogg.ok());
    CHECK(ogg.value().starts_with("OggS"));
    CHECK(ogg.value().find("vorbis") != std::string::npos);
    auto again = soundtrack::encode_ogg(stems[0].samples, soundtrack::kRenderRate, 0.5, 7);
    CHECK(again.value() == ogg.value());
}

TEST_CASE("soundtrack.render writes stems a game plays and audio.analyze checks") {
    const std::string root = "command_soundtrack_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/music").ok());
    REQUIRE(slash::platform::write_file(root + "/music/theme.score.json", R"({
        "format": 1, "bpm": 100, "key": "D minor",
        "tracks": [
            {"name": "bass line", "instrument": "electric_bass_finger",
             "bars": ["D2:1 D3:0.5 D2 A2:1 D2", "F2:1 F3:0.5 F2 C3:1 F2",
                      "D2:1 D3:0.5 D2 A2:1 D2", "A1:1 A2:0.5 A1 E2:1 A1"]},
            {"name": "keys", "instrument": "electric_piano_1", "volume_db": -4,
             "bars": ["D4+F4+A4:4", "F4+A4+C5:4", "D4+F4+A4:4", "C#4+E4+A4:4"]},
            {"name": "drums", "instrument": "drums",
             "bars": ["kick:1 snare kick snare", "kick:1 snare kick snare",
                      "kick:1 snare kick snare", "kick:1 snare kick snare"]}
        ]
    })")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/bank.sf2", soundtrack::builtin_sound_bank()).ok());

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

    slash::command::Reply reply =
        call("soundtrack.render", R"({"path": "music/theme.score.json", "loop": true})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["path"].as_string() == "music/theme.audio.json");
    CHECK(reply.value["bpm"].as_int() == 100);
    CHECK(reply.value["beat_count"].as_int() == 16);
    CHECK(reply.value["seconds"].as_double() == doctest::Approx(9.6));
    REQUIRE(reply.value["stems"].as_array().size() == 3);
    CHECK(reply.value["stems"].as_array()[0]["path"].as_string() == "music/theme/bass_line.ogg");
    CHECK(reply.value["stems"].as_array()[0]["track"].as_string() == "bass line");
    CHECK(reply.value["peak_db"].as_double() < -3.0);
    CHECK(reply.value.find("problems") == nullptr);
    CHECK(json::write(parse(read(root + "/music/theme/bass_line.audio.json")),
                      json::Style::Compact) ==
          R"({"beat_count":16,"bpm":100,"format":1,"loop":true,)"
          R"("source":"music/theme/bass_line.ogg"})");
    CHECK(
        json::write(parse(read(root + "/music/theme.audio.json")), json::Style::Compact) ==
        R"({"format":1,"synchronized":[{"stream":"music/theme/bass_line.audio.json"},)"
        R"({"stream":"music/theme/keys.audio.json"},{"stream":"music/theme/drums.audio.json"}]})");

    // What the stems sound like, measured as an agent would.
    reply = call("audio.analyze", R"({"path": "music/theme/bass_line.audio.json"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["bpm"].as_double() == doctest::Approx(100.0).epsilon(0.01));
    CHECK(reply.value["key"].as_string() == "D minor");
    CHECK_FALSE(reply.value["loop"]["clicks"].as_bool());
    CHECK(reply.value["clips"].as_int() == 0);
    reply = call("audio.analyze", R"({"path": "music/theme/drums.audio.json"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["bpm"].as_double() == doctest::Approx(100.0).epsilon(0.01));
    CHECK_FALSE(reply.value["loop"]["clicks"].as_bool());

    // One track again, as WAV, with a SoundFont file; the other stems stay in the music.
    reply = call("soundtrack.render", R"({"path": "music/theme.score.json", "tracks": ["keys"],
                                          "format": "wav", "soundfont": "bank.sf2"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    REQUIRE(reply.value["stems"].as_array().size() == 1);
    CHECK(reply.value["stems"].as_array()[0]["path"].as_string() == "music/theme/keys.wav");
    CHECK(reply.value.find("peak_db") == nullptr);
    CHECK(read(root + "/music/theme/keys.wav").starts_with("RIFF"));
    CHECK(json::write(parse(read(root + "/music/theme/keys.audio.json")), json::Style::Compact) ==
          R"({"beat_count":16,"bpm":100,"format":1,"source":"music/theme/keys.wav"})");
    CHECK(read(root + "/music/theme.audio.json").find("drums.audio.json") != std::string::npos);

    // A loud track says so.
    REQUIRE(slash::platform::write_file(root + "/music/loud.score.json", R"({
        "format": 1, "tracks": [{"name": "wall", "instrument": "lead_sawtooth", "velocity": 127,
            "bars": ["C3+E3+G3+C4+E4+G4+C5+E5+G5:4 ; C3+E3+G3+C4+E4+G4+C5+E5+G5:4"]}]
    })")
                .ok());
    reply = call("soundtrack.render", R"({"path": "music/loud.score.json", "out": "build/loud",
                                          "format": "wav"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    REQUIRE(reply.value["problems"].as_array().size() == 1);
    CHECK(
        reply.value["problems"].as_array()[0].as_string().starts_with("wall clips: it peaks at +"));
    CHECK(slash::platform::exists(root + "/build/loud/wall.wav"));
    CHECK(slash::platform::exists(root + "/build/loud.audio.json"));

    // Mistakes.
    reply = call("soundtrack.render", R"({"path": "music/theme.score.json", "tracks": ["kyes"]})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message ==
          "/tracks/0: music/theme.score.json has no track called kyes; did you mean keys?");
    reply =
        call("soundtrack.render", R"({"path": "music/theme.score.json", "soundfont": "x.sf2"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message == "/soundfont: no file x.sf2");
    REQUIRE(slash::platform::write_file(root + "/bad.sf2", "RIFF").ok());
    reply =
        call("soundtrack.render", R"({"path": "music/theme.score.json", "soundfont": "bad.sf2"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message == "/soundfont: bad.sf2 isn't a SoundFont 2 file");
    reply = call("soundtrack.render", R"({"path": "music/theme.mid"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message == "/path: expected a .score.json file");
}
