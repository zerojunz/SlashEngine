#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/soundtrack/midi.hpp"
#include "slash/soundtrack/score.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = slash::json;
namespace soundtrack = slash::soundtrack;
using soundtrack::Note;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return parsed.value();
}

std::vector<Note> notes_of(std::string_view bar, bool drums = false, int bar_beats = 4) {
    const soundtrack::BarNotes decoded = soundtrack::decode_bar(bar, bar_beats, 0.0, 100, drums);
    REQUIRE_MESSAGE(decoded.error.empty(), decoded.error);
    return decoded.notes;
}

std::string error_of(std::string_view bar, bool drums = false) {
    return soundtrack::decode_bar(bar, 4, 0.0, 100, drums).error;
}

void check_same(const std::vector<Note>& a, const std::vector<Note>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].beat == doctest::Approx(b[i].beat));
        CHECK(a[i].length == doctest::Approx(b[i].length));
        CHECK(a[i].pitch == b[i].pitch);
        CHECK(a[i].velocity == b[i].velocity);
    }
}

// A Standard MIDI File put together by hand.
std::string vlq(std::uint32_t value) {
    std::string out(1, static_cast<char>(value & 0x7FU));
    while ((value >>= 7U) != 0) {
        out.insert(out.begin(), static_cast<char>((value & 0x7FU) | 0x80U));
    }
    return out;
}

std::string raw(std::initializer_list<int> values) {
    std::string out;
    for (const int value : values) {
        out += static_cast<char>(value);
    }
    return out;
}

std::string chunk(std::string_view id, const std::string& body) {
    const auto size = static_cast<std::uint32_t>(body.size());
    return std::string(id) +
           raw({static_cast<int>(size >> 24U), static_cast<int>(size >> 16U) & 0xFF,
                static_cast<int>(size >> 8U) & 0xFF, static_cast<int>(size & 0xFFU)}) +
           body;
}

std::string header(int format, int tracks, int division) {
    return chunk("MThd", raw({0, format, 0, tracks, division >> 8, division & 0xFF}));
}

} // namespace

TEST_CASE("scores name notes, drums, instruments and keys") {
    CHECK(soundtrack::parse_pitch("C4", false) == 60);
    CHECK(soundtrack::parse_pitch("A4", false) == 69);
    CHECK(soundtrack::parse_pitch("Bb2", false) == 46);
    CHECK(soundtrack::parse_pitch("F#3", false) == 54);
    CHECK(soundtrack::parse_pitch("C-1", false) == 0);
    CHECK(soundtrack::parse_pitch("G9", false) == 127);
    CHECK(soundtrack::parse_pitch("60", false) == 60);
    CHECK_FALSE(soundtrack::parse_pitch("G#9", false));
    CHECK_FALSE(soundtrack::parse_pitch("128", false));
    CHECK_FALSE(soundtrack::parse_pitch("H4", false));
    CHECK_FALSE(soundtrack::parse_pitch("c4", false));
    CHECK_FALSE(soundtrack::parse_pitch("kick", false));
    CHECK(soundtrack::parse_pitch("kick", true) == 36);
    CHECK(soundtrack::parse_pitch("closed_hat", true) == 42);
    CHECK(soundtrack::pitch_name(61, false, false) == "C#4");
    CHECK(soundtrack::pitch_name(61, true, false) == "Db4");
    CHECK(soundtrack::pitch_name(42, false, true) == "closed_hat");
    CHECK(soundtrack::pitch_name(30, false, true) == "F#1");
    CHECK(soundtrack::instrument_names().size() == 128);
    CHECK(soundtrack::instrument_names()[127] == "gunshot");
    CHECK(soundtrack::instrument_program("flute") == 73);
    CHECK_FALSE(soundtrack::instrument_program("drums"));
    CHECK(soundtrack::drum_names().front() == "acoustic_kick");
    CHECK(soundtrack::drum_names().back() == "open_triangle");

    const auto d_minor = soundtrack::parse_key("D minor");
    REQUIRE(d_minor);
    CHECK(d_minor->tonic == 2);
    CHECK(d_minor->minor);
    CHECK(d_minor->sharps == -1);
    CHECK(soundtrack::parse_key("F# major")->sharps == 6);
    CHECK(soundtrack::parse_key("Eb major")->sharps == -3);
    // Spelled another way, a key takes the plainest signature with its tonic.
    CHECK(soundtrack::parse_key("D# major")->sharps == -3);
    CHECK_FALSE(soundtrack::parse_key("H major"));
    CHECK_FALSE(soundtrack::parse_key("C dorian"));
    CHECK_FALSE(soundtrack::parse_key("Dminor"));
    CHECK(soundtrack::key_name(-1, true) == "D minor");
    CHECK(soundtrack::key_name(2, false) == "D major");
}

TEST_CASE("a bar's text plays notes one after another, with chords, rests and voices") {
    const soundtrack::BarNotes bar =
        soundtrack::decode_bar("D3:1 F3 A3 D4@110 ; D2:4", 4, 8.0, 100, false);
    REQUIRE_MESSAGE(bar.error.empty(), bar.error);
    check_same(bar.notes, {{8.0, 4.0, 38, 100},
                           {8.0, 1.0, 50, 100},
                           {9.0, 1.0, 53, 100},
                           {10.0, 1.0, 57, 100},
                           {11.0, 1.0, 62, 110}});
    // A chord, a rest, then halves of a beat; triplets in thirds.
    check_same(notes_of("C4+E4+G4:2 r:1 F4:1/2 G4"), {{0.0, 2.0, 60, 100},
                                                      {0.0, 2.0, 64, 100},
                                                      {0.0, 2.0, 67, 100},
                                                      {3.0, 0.5, 65, 100},
                                                      {3.5, 0.5, 67, 100}});
    check_same(notes_of("C4:1/3 D4 E4 F4:1 G4:2"), {{0.0, 1.0 / 3.0, 60, 100},
                                                    {1.0 / 3.0, 1.0 / 3.0, 62, 100},
                                                    {2.0 / 3.0, 1.0 / 3.0, 64, 100},
                                                    {1.0, 1.0, 65, 100},
                                                    {2.0, 2.0, 67, 100}});
    // The last note may ring past the bar line; no other can.
    check_same(notes_of("C4:2 E4:4"), {{0.0, 2.0, 60, 100}, {2.0, 4.0, 64, 100}});
    CHECK(notes_of("").empty());
    CHECK(notes_of("kick:0.5 snare 36", true).size() == 3);

    CHECK(error_of("C4:5 E4") ==
          "\"C4:5\": runs to beat 5, past the bar's 4 beats; only a voice's last note can ring "
          "past the bar line");
    CHECK(error_of("r:5").find("past the bar's 4 beats") != std::string::npos);
    CHECK(error_of("C4:2 D4 E4") == "\"E4\": starts at beat 4, after the bar's 4 beats");
    CHECK(error_of("C4 Hb3").starts_with("\"Hb3\": \"Hb3\" isn't a note"));
    CHECK(error_of("kik", true).find("did you mean \"kick\"?") != std::string::npos);
    CHECK(error_of("C4 ; kick") ==
          "voice 2, \"kick\": \"kick\" is a drum; drums play on a track whose instrument is "
          "\"drums\"");
    CHECK(error_of("C4@0") == "\"C4@0\": a velocity is 1 to 127");
    CHECK(error_of("C4@90:1") == "\"C4@90:1\": the length goes before the velocity, as in C4:1@90");
    CHECK(error_of("C4:0").find("a length is a number of beats above 0") != std::string::npos);
    CHECK(error_of("C4:x").find("a length is") != std::string::npos);
    CHECK(error_of("r@80") == "\"r@80\": a rest has no velocity");
    CHECK(error_of("C4 ;") == "voice 2 is empty");
    CHECK(error_of("C4+") == "\"C4+\": \"\" isn't a note: write A to G with # or b and an "
                             "octave, such as C4 or Bb2, or a MIDI note number");
}

TEST_CASE("notes are written back as the shortest bar text that plays them") {
    const auto encode = [](const std::vector<Note>& notes, bool drums = false) {
        return soundtrack::encode_bars(notes, 4, 100, drums, false, soundtrack::kDefaultGrid);
    };
    // A held bass note takes the first voice.
    const std::vector<Note> walk = notes_of("D3:1 F3 A3 D4@110 ; D2:4");
    std::vector<std::string> bars = encode(walk);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0] == "D2:4 ; D3 F3 A3 D4@110");
    check_same(notes_of(bars[0]), walk);

    CHECK(encode(notes_of("r:1/3 E4 F4:1 G4:2"))[0] == "r:1/3 E4 F4:1 G4:2");
    CHECK(encode(notes_of("C4+E4+G4:0.25 r:0.75 C5:3"))[0] == "C4+E4+G4:0.25 r:0.75 C5:3");
    const std::string beat = "kick+closed_hat:0.5 closed_hat snare+closed_hat closed_hat "
                             "kick+closed_hat closed_hat snare+closed_hat closed_hat";
    CHECK(encode(notes_of(beat, true), true)[0] == beat);
    // Flats, and notes in later bars, with an empty bar between.
    std::vector<Note> spread = {{0.0, 1.0, 61, 100}, {8.0, 8.0, 46, 90}};
    bars = soundtrack::encode_bars(spread, 4, 100, false, true, soundtrack::kDefaultGrid);
    CHECK(bars == std::vector<std::string>{"Db4", "", "Bb2:8@90"});
    // Starts and lengths snap to the grid; a note too short for it lasts one step.
    bars = encode({{0.01, 0.98, 60, 100}, {1.0, 0.001, 62, 100}});
    CHECK(bars[0] == "C4 D4:1/24");
    CHECK(encode({}).empty());
}

TEST_CASE("validate_score checks every field, bar and section") {
    const json::Value valid = parse(R"({
        "format": 1, "bpm": 100, "key": "D minor",
        "sections": [{"name": "intro", "bar": 0}, {"name": "loop", "bar": 1}],
        "tracks": [
            {"name": "bass", "instrument": "electric_bass_finger", "volume_db": -6,
             "bars": ["D2:4", "Bb1:4"]},
            {"name": "drums", "instrument": "drums", "velocity": 90,
             "bars": ["kick snare kick snare", "kick snare kick snare@120"]}
        ]})");
    CHECK(soundtrack::validate_score(valid).empty());
    const auto score = soundtrack::parse_score(valid);
    REQUIRE(score.ok());
    CHECK(score.value().bars == 2);
    CHECK(score.value().beats() == 8.0);
    CHECK(score.value().seconds() == doctest::Approx(4.8));
    CHECK(score.value().tracks[0].program == 33);
    CHECK(score.value().tracks[1].drums);
    CHECK(score.value().tracks[1].notes.size() == 8);
    CHECK(score.value().tracks[1].notes.back().velocity == 120);
    CHECK(score.value().tracks[1].notes.front().velocity == 90);
    CHECK(json::write(soundtrack::score_to_json(score.value())) == json::write(valid));

    const auto errors_of = [](std::string_view text) {
        std::vector<std::string> out;
        for (const json::ValidationError& error : soundtrack::validate_score(parse(text))) {
            out.push_back(error.path + ": " + error.message);
        }
        return out;
    };
    CHECK(errors_of(R"({"format": 1, "key": "D dorian", "tracks": [
            {"name": "a", "instrument": "flutte", "bars": ["C4", "C4 Hb3"]},
            {"name": "a", "bars": []}]})") ==
          std::vector<std::string>{
              "/key: expected a key such as \"D minor\" or \"F# major\"",
              "/tracks/0/instrument: no General MIDI instrument \"flutte\"; did you mean "
              "\"flute\"?",
              "/tracks/0/bars/1: \"Hb3\": \"Hb3\" isn't a note: write A to G with # or b and an "
              "octave, such as C4 or Bb2, or a MIDI note number",
              "/tracks/1/name: another track is called \"a\""});
    CHECK(errors_of(R"({"format": 1, "bars": 1, "tracks": [{"name": "a", "bars": ["C4", "D4"]}],
            "sections": [{"name": "b", "bar": 0}, {"name": "b", "bar": 0}, {"name": "c",
            "bar": 4}]})") ==
          std::vector<std::string>{"/tracks/0/bars: has 2 bars, more than the score's 1",
                                   "/sections/1/name: another section is called \"b\"",
                                   "/sections/1/bar: must come after the section before it, at "
                                   "bar 0",
                                   "/sections/2/bar: is past the score's last bar, 0"});
    CHECK(errors_of(R"({"format": 1, "tracks": [{"name": "a", "bars": []}]})") ==
          std::vector<std::string>{
              "/tracks: no track has any bars; give the score its length in \"bars\""});
    CHECK(errors_of(R"({"format": 1, "bpm": 0, "tracks": []})").size() == 2);
}

TEST_CASE("scores go to MIDI files and back unchanged") {
    const json::Value doc = parse(R"({
        "format": 1, "bpm": 96, "bar_beats": 3, "key": "Eb major",
        "sections": [{"name": "a", "bar": 0}, {"name": "b", "bar": 2}],
        "tracks": [
            {"name": "melody", "instrument": "flute", "pan": 0.5, "velocity": 90,
             "bars": ["Eb5:1/3 F5 G5 Ab5:1 Bb5", "C6:2 Bb5:0.5 Ab5", "G5+Bb5:3@110"]},
            {"name": "bass", "instrument": "acoustic_bass", "volume_db": -6, "pan": -1,
             "bars": ["Eb2:3", "Eb3:6 ; Ab1:2 Bb1:1"]},
            {"name": "drums", "instrument": "drums",
             "bars": ["kick snare snare", "kick snare snare", "kick snare snare", "kick+crash:3"]}
        ]})");
    const auto score = soundtrack::parse_score(doc);
    REQUIRE_MESSAGE(score.ok(), score.error().message);
    const auto midi = soundtrack::write_midi(score.value());
    REQUIRE(midi.ok());
    CHECK(midi.value().starts_with("MThd"));
    const auto back = soundtrack::read_midi(midi.value());
    REQUIRE_MESSAGE(back.ok(), back.error().message);
    CHECK(back.value().problems.empty());
    CHECK(back.value().notes == 25);
    CHECK(json::write(soundtrack::score_to_json(back.value().score)) == json::write(doc));

    // Two notes of one pitch can't overlap on a MIDI channel: the first ends where the second
    // starts.
    soundtrack::Score overlap;
    overlap.bars = 1;
    soundtrack::Track piano;
    piano.name = "piano";
    piano.notes = {{0.0, 2.0, 60, 100}, {1.0, 1.0, 60, 100}};
    overlap.tracks.push_back(piano);
    const auto read = soundtrack::read_midi(soundtrack::write_midi(overlap).value());
    REQUIRE(read.ok());
    CHECK(read.value().score.tracks[0].bars == std::vector<std::string>{"C4 C4"});

    // MIDI has 15 channels besides the drums.
    soundtrack::Score crowded;
    crowded.bars = 1;
    for (int i = 0; i < 16; ++i) {
        piano.name = "piano " + std::to_string(i);
        crowded.tracks.push_back(piano);
    }
    CHECK(soundtrack::write_midi(crowded).error().message ==
          "has 16 tracks that aren't drums; MIDI has 15 channels for them");
}

TEST_CASE("read_midi keeps what a score holds and says what it left out") {
    // Format 0: one track with two channels, a tempo change, a bend, the sustain pedal, a note
    // off the grid and a marker between bar lines, at 96 ticks a quarter note, read on a grid of
    // quarter beats.
    std::string track;
    track += vlq(0) + raw({0xFF, 0x03, 4}) + "Band";
    track += vlq(0) + raw({0xFF, 0x51, 3, 0x09, 0x27, 0xC0}); // 600000: 100 bpm
    track += vlq(0) + raw({0xFF, 0x58, 4, 6, 3, 24, 8});      // 6/8
    track += vlq(0) + raw({0xFF, 0x59, 2, 0xFE, 1});          // two flats, minor: G minor
    track += vlq(0) + raw({0xC1, 40});                        // violin on channel 2
    track += vlq(0) + raw({0xB1, 7, 100});
    track += vlq(0) + raw({0x91, 67, 80});            // G4
    track += vlq(0) + raw({0x90, 58, 64});            // Bb3 on channel 1, piano
    track += vlq(48) + raw({0x81, 67, 0});            // an eighth: one beat in 6/8
    track += vlq(0) + raw({0xE0, 0, 0x50});           // a bend
    track += vlq(0) + raw({0xB0, 64, 127});           // sustain on
    track += vlq(5) + raw({0x91, 70, 80});            // Bb4, five ticks late
    track += vlq(0) + raw({0xFF, 0x06, 5}) + "verse"; // a marker, off the bar line
    track += vlq(43) + raw({0x80, 58, 0});
    track += vlq(0) + raw({0xFF, 0x51, 3, 0x07, 0xA1, 0x20}); // 500000: a tempo change
    track += vlq(48) + raw({0x81, 70, 0});
    track += vlq(0) + raw({0xFF, 0x2F, 0});
    const std::string file = header(0, 1, 96) + chunk("MTrk", track);
    const auto imported = soundtrack::read_midi(file, 4);
    REQUIRE_MESSAGE(imported.ok(), imported.error().message);
    const soundtrack::Score& score = imported.value().score;
    CHECK(score.bpm == 200.0); // a quarter note at 100 bpm is two eighths
    CHECK(score.bar_beats == 6);
    CHECK(score.beat_unit == 8);
    CHECK(score.key == "G minor");
    CHECK(score.bars == 1);
    REQUIRE(score.tracks.size() == 2);
    CHECK(score.tracks[0].name == "Band");
    CHECK(score.tracks[0].instrument == "acoustic_grand_piano");
    CHECK(score.tracks[0].bars == std::vector<std::string>{"Bb3:2"});
    CHECK(score.tracks[1].name == "Band 2");
    CHECK(score.tracks[1].instrument == "violin");
    CHECK(score.tracks[1].velocity == 80);
    CHECK(score.tracks[1].volume_db == doctest::Approx(-4.2));
    CHECK(score.tracks[1].bars == std::vector<std::string>{"G4 Bb4:2"});
    REQUIRE(score.sections.size() == 1);
    CHECK(score.sections[0].name == "verse");
    CHECK(score.sections[0].bar == 0);
    CHECK(imported.value().problems ==
          std::vector<std::string>{"the tempo changes 1 time; the score keeps the first",
                                   "1 pitch bend was left out",
                                   "the sustain pedal was left out (pressed 1 time)",
                                   "1 note moved to the nearest 1/4 of a beat",
                                   "marker \"verse\" isn't on a bar line; its section starts at "
                                   "bar 0"});

    CHECK(soundtrack::read_midi("hello").error().message ==
          "isn't a MIDI file: it has no MThd header");
    CHECK(soundtrack::read_midi(header(2, 1, 96) + chunk("MTrk", ""))
              .error()
              .message.starts_with("is a format 2 MIDI file"));
    CHECK(soundtrack::read_midi(header(1, 1, 0xE728) + chunk("MTrk", ""))
              .error()
              .message.starts_with("counts time in SMPTE frames"));
    CHECK(soundtrack::read_midi(header(1, 1, 96) + chunk("MTrk", track).substr(0, 20))
              .error()
              .message == "is cut short");
    CHECK(soundtrack::read_midi(header(1, 1, 96) + chunk("MTrk", vlq(0) + raw({0xFF, 0x2F, 0})))
              .error()
              .message == "has no notes");
}

TEST_CASE("score.import and score.export turn MIDI files into scores and back") {
    const std::string root = "command_score_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/music").ok());
    REQUIRE(slash::platform::write_file(root + "/music/theme.score.json", R"({
        "format": 1, "bpm": 90, "key": "A minor",
        "sections": [{"name": "intro", "bar": 0}, {"name": "loop", "bar": 1}],
        "tracks": [{"name": "lead", "instrument": "oboe", "bars": ["A4:2 C5 ; A3:4", "E5:4"]}]
    })")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/music/broken.score.json",
                                        R"({"format": 1, "tracks": [{"name": "a",
                                            "bars": ["C4 Q4"]}]})")
                .ok());

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

    slash::command::Reply reply = call("score.export", R"({"path": "music/theme.score.json"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(json::write(reply.value, json::Style::Compact) ==
          R"({"notes":4,"path":"music/theme.mid","seconds":5.33,"tracks":1})");
    CHECK(slash::platform::exists(root + "/music/theme.mid"));

    reply = call("score.import", R"({"path": "music/theme.mid", "out": "music/again.score.json"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(
        json::write(reply.value, json::Style::Compact) ==
        R"({"bars":2,"bpm":90,"key":"A minor","path":"music/again.score.json",)"
        R"("sections":["intro","loop"],"tracks":[{"instrument":"oboe","name":"lead","notes":4}]})");
    const auto again = slash::platform::read_file(root + "/music/again.score.json");
    REQUIRE(again.ok());
    CHECK(json::write(parse(again.value()), json::Style::Compact) ==
          R"({"bpm":90,"format":1,"key":"A minor","sections":[{"bar":0,"name":"intro"},)"
          R"({"bar":1,"name":"loop"}],"tracks":[{"bars":["A3:4 ; A4:2 C5","E5:4"],)"
          R"("instrument":"oboe","name":"lead"}]})");
    // Without "out", the score goes beside the MIDI file.
    reply = call("score.import", R"({"path": "music/theme.mid", "grid": 4})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(slash::platform::exists(root + "/music/theme.score.json"));

    const auto error = [&](std::string_view name, std::string_view params) {
        const slash::command::Reply failed = call(name, params);
        CHECK_FALSE(failed.ok);
        return failed.message;
    };
    CHECK(error("score.export", R"({"path": "music/broken.score.json"})") ==
          "/path: music/broken.score.json /tracks/0/bars/0: \"Q4\": \"Q4\" isn't a note: write A "
          "to G with # or b and an octave, such as C4 or Bb2, or a MIDI note number");
    CHECK(error("score.export", R"({"path": "music/none.score.json"})") ==
          "/path: no file music/none.score.json");
    CHECK(error("score.export", R"({"path": "music/theme.mid"})") ==
          "/path: expected a .score.json file");
    CHECK(error("score.export", R"({"path": "music/theme.score.json", "out": "x.wav"})") ==
          "/out: expected a .mid or .midi file");
    CHECK(error("score.import", R"({"path": "music/theme.score.json"})") ==
          "/path: expected a Standard MIDI File (.mid or .midi)");
    CHECK(error("score.import", R"({"path": "music/theme.mid", "out": "a.json"})") ==
          "/out: expected a .score.json file");
    REQUIRE(slash::platform::write_file(root + "/music/fake.mid", "not midi").ok());
    CHECK(error("score.import", R"({"path": "music/fake.mid"})") ==
          "/path: music/fake.mid isn't a MIDI file: it has no MThd header");
    std::filesystem::remove_all(root);
}
