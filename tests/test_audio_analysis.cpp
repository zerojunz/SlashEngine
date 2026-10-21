#include "slash/audio/analysis.hpp"
#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;
using audio::Frame;

namespace {

constexpr double kPi = std::numbers::pi;

// A sine at hz, amplitude a, as frames at rate: both sides the same.
std::vector<Frame> tone(double hz, float amplitude, double seconds, int rate) {
    std::vector<Frame> out(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double turns = hz * static_cast<double>(i) / rate;
        const float value =
            amplitude *
            slash::cos_sin(static_cast<float>(2.0 * kPi * (turns - std::floor(turns)))).y;
        out[i] = {value, value};
    }
    return out;
}

void add(std::vector<Frame>& into, const std::vector<Frame>& more, std::size_t at = 0) {
    if (into.size() < at + more.size()) {
        into.resize(at + more.size());
    }
    for (std::size_t i = 0; i < more.size(); ++i) {
        into[at + i].left += more[i].left;
        into[at + i].right += more[i].right;
    }
}

// A short knock: a 1 kHz tone dying away over 60 ms.
std::vector<Frame> knock(int rate) {
    std::vector<Frame> out = tone(1000.0, 0.8F, 0.06, rate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float fade = 1.0F - (static_cast<float>(i) / static_cast<float>(out.size()));
        out[i].left *= fade * fade;
        out[i].right *= fade * fade;
    }
    return out;
}

// Knocks every 60 / bpm seconds from offset, for seconds.
std::vector<Frame> beats(double bpm, double offset, double seconds, int rate) {
    std::vector<Frame> out(static_cast<std::size_t>(seconds * rate));
    const std::vector<Frame> one = knock(rate);
    for (double at = offset; at < seconds - 0.1; at += 60.0 / bpm) {
        add(out, one, static_cast<std::size_t>(at * rate));
    }
    out.resize(static_cast<std::size_t>(seconds * rate));
    return out;
}

// A note's frequency: MIDI note 69 is A4, 440 Hz.
double note(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

// Chords of MIDI notes, one after another, each lasting seconds.
std::vector<Frame> chords(const std::vector<std::vector<int>>& progression, double seconds,
                          int rate) {
    std::vector<Frame> out;
    for (std::size_t c = 0; c < progression.size(); ++c) {
        std::vector<Frame> chord;
        for (const int midi : progression[c]) {
            add(chord, tone(note(midi), 0.15F, seconds, rate));
        }
        add(out, chord, static_cast<std::size_t>(static_cast<double>(c) * seconds * rate));
    }
    return out;
}

std::vector<Frame> silence(double seconds, int rate) {
    return std::vector<Frame>(static_cast<std::size_t>(seconds * rate));
}

std::vector<Frame> joined(std::initializer_list<std::vector<Frame>> parts) {
    std::vector<Frame> out;
    for (const std::vector<Frame>& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

} // namespace

TEST_CASE("audio::analyze measures loudness, peaks and clipping") {
    constexpr int kRate = 48000;
    // A 1 kHz sine at -20 dBFS on both sides is -20 LUFS, as EBU R 128 calibrates it.
    const audio::Analysis sine = audio::analyze(tone(1000.0, 0.1F, 5.0, kRate), kRate);
    CHECK(sine.seconds == doctest::Approx(5.0));
    REQUIRE(sine.lufs);
    CHECK(*sine.lufs == doctest::Approx(-20.0).epsilon(0.005));
    REQUIRE(sine.max_short_term_lufs);
    CHECK(*sine.max_short_term_lufs == doctest::Approx(-20.0).epsilon(0.005));
    CHECK(*sine.loudness_range == doctest::Approx(0.0).epsilon(0.01));
    CHECK(*sine.peak_db == doctest::Approx(-20.0).epsilon(0.001));
    CHECK(*sine.true_peak_db == doctest::Approx(-20.0).epsilon(0.005));
    CHECK(*sine.rms_db == doctest::Approx(-23.01).epsilon(0.001));
    CHECK(sine.clips == 0);
    CHECK(sine.silence_start == 0.0);
    CHECK(sine.silence_end == 0.0);
    CHECK(sine.gaps.empty());
    // A steady tone has no beat, and one note is no key.
    CHECK_FALSE(sine.bpm);

    // Too loud: each crest is cut flat at full scale.
    std::vector<Frame> loud = tone(1000.0, 1.5F, 1.0, kRate);
    for (Frame& frame : loud) {
        frame.left = std::clamp(frame.left, -1.0F, 1.0F);
        frame.right = std::clamp(frame.right, -1.0F, 1.0F);
    }
    const audio::Analysis clipped = audio::analyze(loud, kRate);
    CHECK(clipped.clips == 2000); // both crests of each cycle, on both sides
    CHECK(clipped.first_clip < 0.0003);
    CHECK(*clipped.true_peak_db > 0.0);
    // Past full scale, as a mix can be, counts once per run.
    std::vector<Frame> over = tone(100.0, 1.2F, 0.1, kRate);
    CHECK(audio::analyze(over, kRate).clips == 20);

    const audio::Analysis quiet = audio::analyze(silence(2.0, kRate), kRate);
    CHECK_FALSE(quiet.peak_db);
    CHECK_FALSE(quiet.lufs);
    CHECK(quiet.silence_start == 2.0);
    CHECK(audio::analyze({}, kRate).seconds == 0.0);
}

TEST_CASE("audio::analyze finds silence at the ends and gaps between") {
    constexpr int kRate = 44100;
    const std::vector<Frame> frames =
        joined({silence(0.5, kRate), tone(440.0, 0.5F, 1.0, kRate), silence(0.8, kRate),
                tone(440.0, 0.5F, 1.0, kRate), silence(0.3, kRate), tone(440.0, 0.5F, 0.5, kRate),
                silence(0.3, kRate)});
    const audio::Analysis analysis = audio::analyze(frames, kRate);
    CHECK(analysis.silence_start == doctest::Approx(0.5));
    CHECK(analysis.silence_end == doctest::Approx(0.3));
    // The 0.3 s gap is too short to count.
    REQUIRE(analysis.gaps.size() == 1);
    CHECK(analysis.gaps[0].first == doctest::Approx(1.5));
    CHECK(analysis.gaps[0].second == doctest::Approx(2.3));
}

TEST_CASE("audio::analyze measures tempo and where the beats fall") {
    constexpr int kRate = 44100;
    for (const double bpm : {120.0, 90.0, 140.0, 75.0}) {
        CAPTURE(bpm);
        const audio::Analysis analysis = audio::analyze(beats(bpm, 0.25, 12.0, kRate), kRate);
        REQUIRE(analysis.bpm);
        CHECK(*analysis.bpm == doctest::Approx(bpm).epsilon(0.001));
        CHECK(analysis.beat_offset == doctest::Approx(0.25).epsilon(0.08));
        CHECK(analysis.tempo_confidence > 0.8);
    }
    // At 48 kHz too, and a tempo that isn't whole.
    const audio::Analysis odd = audio::analyze(beats(128.5, 0.0, 15.0, 48000), 48000);
    REQUIRE(odd.bpm);
    CHECK(*odd.bpm == doctest::Approx(128.5).epsilon(0.001));
    CHECK(std::min(odd.beat_offset, (60.0 / 128.5) - odd.beat_offset) < 0.02);
    // Too short to tell.
    CHECK_FALSE(audio::analyze(beats(120.0, 0.0, 2.0, kRate), kRate).bpm);
}

TEST_CASE("audio::analyze finds the key from the notes played") {
    constexpr int kRate = 44100;
    // i - iv - V - i in A minor, with the leading tone G#.
    const audio::Analysis minor = audio::analyze(
        chords({{45, 57, 60, 64}, {50, 62, 65, 69}, {52, 64, 68, 71}, {45, 57, 60, 64}}, 1.0,
               kRate),
        kRate);
    REQUIRE(minor.key);
    CHECK(*minor.key == "A minor");
    CHECK(minor.key_confidence > 0.6);
    // I - IV - V - I in Eb major.
    const audio::Analysis major = audio::analyze(
        chords({{51, 63, 67, 70}, {56, 68, 72, 75}, {58, 70, 74, 77}, {51, 63, 67, 70}}, 1.0,
               kRate),
        kRate);
    REQUIRE(major.key);
    CHECK(*major.key == "Eb major");
}

TEST_CASE("audio::analyze measures each octave band's share of the energy") {
    constexpr int kRate = 44100;
    const audio::Analysis analysis = audio::analyze(tone(1000.0, 0.5F, 2.0, kRate), kRate);
    // 1 kHz is in the band centred on 1 kHz (710 to 1420 Hz); the window leaks a little.
    REQUIRE(analysis.bands_db[5]);
    CHECK(*analysis.bands_db[5] > -0.01);
    for (const std::size_t band : std::initializer_list<std::size_t>{0, 1, 2, 9}) {
        CHECK((!analysis.bands_db[band] || *analysis.bands_db[band] < -60.0));
    }
    // Two equal tones an octave band apart share it half and half.
    std::vector<Frame> both = tone(250.0, 0.3F, 2.0, kRate);
    add(both, tone(4000.0, 0.3F, 2.0, kRate));
    const audio::Analysis split = audio::analyze(both, kRate);
    CHECK(*split.bands_db[3] == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(*split.bands_db[7] == doctest::Approx(-3.01).epsilon(0.01));
}

TEST_CASE("audio::analyze hears whether a loop joins cleanly") {
    constexpr int kRate = 44100;
    // 441 Hz is exactly 100 frames a cycle, so a second of it joins without a seam.
    const std::vector<Frame> whole = tone(441.0, 0.5F, 1.0, kRate);
    const audio::Analysis clean =
        audio::analyze(whole, kRate, audio::LoopPoints{.start = 0, .end = kRate});
    REQUIRE(clean.loop);
    CHECK(clean.loop->seconds == doctest::Approx(1.0));
    CHECK_FALSE(clean.loop->clicks);
    CHECK(clean.loop->level_change_db == doctest::Approx(0.0).epsilon(0.01));
    // Cut a quarter cycle short, it jumps.
    const audio::Analysis cut =
        audio::analyze(whole, kRate, audio::LoopPoints{.start = 0, .end = kRate - 25});
    REQUIRE(cut.loop);
    CHECK(cut.loop->clicks);
    CHECK(*cut.loop->step_db > -7.0);
    // A loop that ends before it starts is none.
    CHECK_FALSE(
        audio::analyze(whole, kRate, audio::LoopPoints{.start = kRate / 2, .end = kRate / 4}).loop);
    // A loop from quiet sound back to loud jumps up in level.
    const audio::Analysis up =
        audio::analyze(joined({whole, tone(441.0, 0.25F, 1.0, kRate)}), kRate,
                       audio::LoopPoints{.start = 0, .end = 2 * kRate});
    REQUIRE(up.loop);
    CHECK(up.loop->level_change_db == doctest::Approx(6.02).epsilon(0.01));
}

TEST_CASE("audio.analyze measures sounds, .audio.json sounds and buses") {
    const std::string root = "command_analyze_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/music").ok());
    constexpr int kRate = 44100;
    REQUIRE(slash::platform::write_file(root + "/music/drums.wav",
                                        audio::encode_wav(beats(120.0, 0.0, 8.0, kRate), kRate))
                .ok());
    REQUIRE(slash::platform::write_file(root + "/music/hum.wav",
                                        audio::encode_wav(tone(441.0, 0.5F, 2.0, kRate), kRate))
                .ok());
    REQUIRE(slash::platform::write_file(root + "/music/hum.audio.json",
                                        R"({"format": 1, "source": "music/hum.wav",
                                            "loop_mode": "forward", "loop_end": 44075})")
                .ok());
    REQUIRE(
        slash::platform::write_file(root + "/music/mix.audio.json",
                                    R"({"format": 1, "playlist": [{"stream": "music/hum.wav"}]})")
            .ok());

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::Session session(components, commands,
                             command::SessionOptions{.project_root = root, .capture_logs = false});
    const auto ok = [&](std::string_view params) {
        const command::Reply reply = session.call("audio.analyze", parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return reply.value;
    };
    const auto fails = [&](std::string_view params, int code) {
        const command::Reply reply = session.call("audio.analyze", parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK(reply.code == code);
        return reply;
    };

    const json::Value drums = ok(R"({"path": "music/drums.wav"})");
    CHECK(drums.find("seconds")->as_double() == 8.0);
    CHECK(drums.find("bpm")->as_double() == 120.0);
    CHECK(drums.find("beats")->as_double() == 16.0);
    CHECK(drums.find("beat_offset")->as_double() < 0.02);
    CHECK(drums.find("clips")->as_int() == 0);
    CHECK(drums.find("loop") == nullptr);
    CHECK(drums.find("problems") == nullptr);
    CHECK(drums.find("bands_db")->size() == 10);
    CHECK(drums.find("lufs")->is_number());
    // Looped as a whole, the knocks join cleanly: the last one has died away.
    const json::Value looped = ok(R"({"path": "music/drums.wav", "loop": true})");
    CHECK(*looped.find("loop")->find("beats") == parse("16"));
    CHECK_FALSE(looped.find("loop")->find("clicks")->as_bool());

    // The .audio.json file's loop ends 25 frames short of whole cycles, so it clicks.
    const json::Value hum = ok(R"({"path": "music/hum.audio.json"})");
    CHECK(hum.find("loop")->find("clicks")->as_bool());
    CHECK(*hum.find("problems") == parse(R"(["the loop clicks where its end joins its start"])"));
    CHECK(ok(R"({"path": "music/hum.audio.json", "loop": false})").find("loop") == nullptr);
    CHECK_FALSE(
        ok(R"({"path": "music/hum.wav", "loop": true})").find("loop")->find("clicks")->as_bool());

    // What a bus played: here, nothing yet.
    const json::Value bus = ok(R"({"bus": "Master", "seconds": 1})");
    CHECK(bus.find("lufs")->is_null());
    CHECK(*bus.find("problems") == parse(R"(["silent"])"));

    fails("{}", command::code::kInvalidParams);
    fails(R"({"path": "music/drums.wav", "bus": "Master"})", command::code::kInvalidParams);
    fails(R"({"path": "music/drums.png"})", command::code::kInvalidArgument);
    fails(R"({"path": "music/none.wav"})", command::code::kNotFound);
    const command::Reply playlist =
        fails(R"({"path": "music/mix.audio.json"})", command::code::kInvalidArgument);
    CHECK(playlist.data.find("path")->as_string() == "/path");
    fails(R"({"bus": "Master", "loop": true})", command::code::kInvalidArgument);
    fails(R"({"bus": "Mastr"})", command::code::kNotFound);
    std::filesystem::remove_all(root);
}
