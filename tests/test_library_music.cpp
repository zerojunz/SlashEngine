#include "slash/ai/commands.hpp"
#include "slash/audio/analysis.hpp"
#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/sha256.hpp"
#include "slash/library/commands.hpp"
#include "slash/library/generator.hpp"
#include "slash/library/library.hpp"
#include "slash/library/music.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace library = slash::library;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), std::string(text));
    return std::move(value).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE_MESSAGE(text.ok(), path.string());
    return std::move(text).value();
}

fs::path fresh(std::string_view name) {
    const fs::path root = fs::absolute(name);
    fs::remove_all(root);
    write(root / "project" / "project.json", R"({"format": 1, "name": "Music test"})");
    fs::create_directories(root / "library");
    return root;
}

library::GenerateRequest music(std::string prompt, double bpm = 0.0, std::string key = {},
                               double seconds = 0.0) {
    library::GenerateRequest request;
    request.type = "music";
    request.prompt = std::move(prompt);
    request.bpm = bpm;
    request.key = std::move(key);
    request.seconds = seconds;
    return request;
}

library::Generated make(const library::GenerateRequest& request) {
    auto made = library::test_generator()->generate(request);
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    return std::move(made).value();
}

slash::audio::Analysis analyze(const std::string& wav, bool loop) {
    auto sound = slash::audio::decode_sound(wav, "music.wav");
    REQUIRE(sound.ok());
    auto frames = slash::audio::decode_frames(sound.value());
    REQUIRE(frames.ok());
    std::optional<slash::audio::LoopPoints> points;
    if (loop) {
        points = slash::audio::LoopPoints{
            .start = 0, .end = static_cast<std::int64_t>(frames.value().size()), .crossfade = 0};
    }
    return slash::audio::analyze(frames.value(), sound.value().mix_rate, points);
}

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(const fs::path& root, std::string_view project = "project") {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::ai::register_commands(commands);
        library::register_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{.project_root = (root / project).string(),
                                    .capture_logs = false,
                                    .library_dir = (root / "library").string()});
    }

    json::Value call(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return reply.value;
    }

    std::string fail(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        return reply.message;
    }
};

} // namespace

TEST_CASE("keys are read as agents write them and named as audio.analyze names them") {
    const auto key = [](std::string_view text) {
        const std::optional<library::Key> parsed = library::parse_key(text);
        return parsed ? library::key_name(*parsed) : std::string("(none)");
    };
    CHECK(key("D minor") == "D minor");
    CHECK(key(" f# major ") == "F# major");
    CHECK(key("Bbm") == "Bb minor");
    CHECK(key("A#min") == "Bb minor");
    CHECK(key("Am") == "A minor");
    CHECK(key("Db") == "C# major");
    CHECK(key("Eb") == "Eb major");
    CHECK(key("Cb major") == "B major");
    CHECK(key("H minor") == "(none)");
    CHECK(key("D dorian") == "(none)");
    CHECK(key("") == "(none)");
}

TEST_CASE("the test generator makes music in the key and tempo asked, in bars that loop cleanly") {
    const library::Generated boss = make(music("tense boss fight", 140, "D minor", 10));
    CHECK(boss.format == "wav");
    CHECK(boss.model == "test");
    CHECK(boss.bpm == 140);
    CHECK(boss.key == "D minor");
    // Six bars of four beats come nearest ten seconds at 140 bpm.
    library::Record record;
    REQUIRE(library::describe_file(boss.data, "boss.wav", record).ok());
    CHECK(record.seconds == doctest::Approx(24 * 60.0 / 140).epsilon(0.001));
    CHECK(record.sample_rate == 22050);
    // What it asked for is what it measures, and its end joins its start cleanly.
    const slash::audio::Analysis heard = analyze(boss.data, true);
    REQUIRE(heard.bpm);
    CHECK(*heard.bpm == doctest::Approx(140).epsilon(0.01));
    CHECK(heard.key == "D minor");
    REQUIRE(heard.loop);
    CHECK_FALSE(heard.loop->clicks);
    // The same on every machine: integer math only.
    CHECK(make(music("tense boss fight", 140, "D minor", 10)).data == boss.data);
    CHECK(slash::sha256_hex(boss.data).substr(0, 16) == "271c4d013eed8b8e");

    // With nothing asked: four bars at 120 bpm in C major, or A minor for a dark prompt.
    const library::Generated calm = make(music("calm village"));
    CHECK(calm.bpm == 120);
    CHECK(calm.key == "C major");
    CHECK(calm.data.size() == 44 + (2 * 22050 * 8));
    CHECK(make(music("sad farewell")).key == "A minor");
    library::GenerateRequest waltz = music("waltz", 90);
    waltz.bar_beats = 3;
    CHECK(make(waltz).data.size() == 44 + (2 * 22050 * 8)); // 12 beats at 90 bpm

    // Refined faster and higher, shorter, or varied when the prompt says none of those.
    library::Record parent;
    parent.generator = "test";
    parent.bpm = 140;
    parent.beat_count = 24;
    parent.key = "D minor";
    parent.seed = 0;
    library::GenerateRequest refine = music("faster and higher");
    refine.parent = &parent;
    const library::Generated faster = make(refine);
    CHECK(faster.bpm == 175);
    CHECK(faster.key == "E minor");
    CHECK(faster.seed == 0);
    refine.prompt = "shorter";
    const library::Generated shorter = make(refine);
    CHECK(shorter.bpm == 140);
    library::Record cut;
    REQUIRE(library::describe_file(shorter.data, "cut.wav", cut).ok());
    CHECK(cut.seconds == doctest::Approx(12 * 60.0 / 140).epsilon(0.001));
    refine.prompt = "more menacing";
    const library::Generated varied = make(refine);
    CHECK(varied.seed == 1);
    CHECK(varied.key == "D minor");
    CHECK(varied.data != boss.data);
    parent.generator = "elevenlabs-music";
    CHECK(library::test_generator()->generate(refine).error().message ==
          "the test generator refines only the music it makes");
}

TEST_CASE("describe_music keeps an author's facts and measures a generator's music") {
    const library::Generated boss = make(music("boss", 140, "D minor", 10));
    const auto described = [&boss](const library::MusicFacts& facts, bool trusted,
                                   library::Record& record) {
        REQUIRE(library::describe_file(boss.data, "boss.wav", record).ok());
        auto notes = library::describe_music(boss.data, "boss.wav", facts, trusted, record);
        REQUIRE_MESSAGE(notes.ok(), notes.error().message);
        return std::move(notes).value();
    };

    // An author's own tempo and key, which the sound agrees with: half its beat, its relative
    // major.
    library::Record track;
    CHECK(described({.bpm = 70, .beat_count = 0, .bar_beats = 4, .key = "F major"}, true, track)
              .empty());
    CHECK(track.type == "music");
    CHECK(track.bpm == 70);
    CHECK(track.beat_count == 12);
    CHECK(track.bar_beats == 4);
    CHECK(track.key == "F major");

    // An author's facts stay, but the measures that disagree are said.
    library::Record wrong;
    CHECK(
        described({.bpm = 100, .beat_count = 30, .bar_beats = 0, .key = "e major"}, true, wrong) ==
        std::vector<std::string>{"it measures 140 bpm, not the 100 bpm given",
                                 "30 beats at 100 bpm last 18 s, longer than the sound's 10.3 s",
                                 "it measures in D minor, not the E major given"});
    CHECK(wrong.bpm == 100);
    CHECK(wrong.beat_count == 30);
    CHECK(wrong.key == "E major");

    // A generator's music is what it measures, whatever was asked.
    library::Record asked;
    CHECK(
        described({.bpm = 100, .beat_count = 0, .bar_beats = 0, .key = "E major"}, false, asked) ==
        std::vector<std::string>{"it measures 140 bpm, not the 100 bpm asked for",
                                 "it measures in D minor, not the E major asked for"});
    CHECK(asked.bpm == 140);
    CHECK(asked.beat_count == 24);
    CHECK(asked.key == "D minor");
    library::Record measured;
    CHECK(described({}, false, measured).empty());
    CHECK(measured.bpm == 140);
    CHECK(measured.beat_count == 24);
    CHECK(measured.key == "D minor");
}

TEST_CASE("asset.generate makes music with its settings, from a storyboard theme") {
    const fs::path root = fresh("library_music_generate");
    write(root / "project" / "storyboard" / "music.json", R"({"format": 1, "entries": {
        "theme/harbor": {"name": "Harbor", "key": "G major", "tempo": 96, "bar_beats": 3,
                         "instruments": ["cello", "harp"], "mood": ["calm", "hopeful"]}}})");
    const Engine engine(root);
    const std::string params = R"({"type": "music", "prompt": "The harbor at dawn.",
        "generator": "test", "theme": "theme/harbor", "path": "music/harbor.wav", "loop": true})";
    json::Value made = engine.call("asset.generate", params);
    const std::string id = made["id"].as_string();
    made.as_object().erase("id");
    CHECK(compact(made) ==
          R"({"bar_beats":3,"beat_count":12,"bpm":96,"files":["music/harbor.audio.json"],)"
          R"("key":"G major","path":"music/harbor.wav","seconds":7.5})");
    CHECK(parse(read(root / "project" / "music" / "harbor.audio.json")) ==
          parse(R"({"format": 1, "source": "music/harbor.wav", "bpm": 96, "beat_count": 12,
                    "bar_beats": 3, "loop_mode": "forward"})"));

    // The theme went into the request and the tags.
    json::Value info = engine.call("asset.info", R"({"id": ")" + id + R"("})");
    CHECK(compact(info["tags"]) == R"(["theme/harbor"])");
    CHECK(info["request"]["prompt"] ==
          "The harbor at dawn.\n\nInstruments: cello, harp. Mood: calm, hopeful.");
    CHECK(info["request"]["theme"] == "theme/harbor");
    CHECK(info["request"]["bpm"] == 96);
    CHECK(info["request"]["key"] == "G major");
    CHECK(info["description"] == "The harbor at dawn.");
    CHECK(info["generator"] == "test");

    // Found again by its key, and made once.
    json::Value found =
        engine.call("asset.search", R"({"type": "music", "text": "harbor G major"})");
    REQUIRE(found["assets"].as_array().size() == 1);
    CHECK(found["assets"].as_array()[0]["bpm"] == 96);
    json::Value again = engine.call("asset.generate", params);
    CHECK(again["reused"].as_bool());

    // A slower version keeps the meter, and loops as asked.
    json::Value slower = engine.call("asset.refine", R"({"id": ")" + id + R"(", "prompt": "slower",
        "path": "music/harbor_night.wav", "loop": true})");
    slower.as_object().erase("id");
    CHECK(compact(slower) ==
          R"({"bar_beats":3,"beat_count":12,"bpm":76.8,"files":["music/harbor_night.audio.json"],)"
          R"("key":"G major","parent":")" +
              id + R"(","path":"music/harbor_night.wav","seconds":9.375})");
    CHECK(parse(read(root / "project" / "music" / "harbor_night.audio.json")) ==
          parse(R"({"format": 1, "source": "music/harbor_night.wav", "bpm": 76.8,
                    "beat_count": 12, "bar_beats": 3, "loop_mode": "forward"})"));
    const std::string dot =
        engine
            .call("asset.generate",
                  R"({"prompt": "red dot", "generator": "test", "width": 4})")["id"]
            .as_string();
    CHECK(engine.fail("asset.refine",
                      R"({"id": ")" + dot + R"(", "prompt": "bluer", "loop": true})") ==
          "/loop: only music takes loop; the asset is an image");

    CHECK(engine.fail("asset.generate", R"({"type": "music", "prompt": "x", "generator": "test",
                                            "key": "H minor"})") ==
          R"(/key: "H minor" is not a key; name one such as "D minor" or "F# major")");
    CHECK(engine.fail("asset.generate", R"({"prompt": "x", "generator": "test", "bpm": 90})") ==
          R"(/bpm: only music takes bpm; set type "music")");
    CHECK(engine.fail("asset.generate", R"({"type": "music", "prompt": "x", "generator": "test",
                                            "theme": "theme/harbour"})") ==
          R"(/theme: no theme "theme/harbour" in the storyboard; did you mean "theme/harbor"?)");
    CHECK(engine.fail("asset.generate", R"({"type": "music", "prompt": "x", "generator": "test",
                                            "width": 16})") ==
          "/width: music has seconds, not a width and height");
    CHECK(engine.fail("asset.generate", R"({"type": "music", "prompt": "x", "generator": "test",
                                            "path": "music/x.png"})") ==
          R"(/path: "music/x.png" is not a sound (.wav, .ogg or .mp3))");
}

TEST_CASE("asset.add takes a composer's track with its stems, and asset.use plays them as layers") {
    const fs::path root = fresh("library_music_stems");
    const library::Generated mix = make(music("mix", 140, "D minor", 10));
    library::GenerateRequest drums_request = music("drums", 140, "D minor", 10);
    drums_request.seed = 1;
    const library::Generated drums = make(drums_request);
    const library::Generated pads = make(music("pads", 140, "F major", 10));
    write(root / "project" / "music" / "boss.wav", mix.data);
    write(root / "project" / "music" / "stems" / "drums.wav", drums.data);
    write(root / "project" / "music" / "stems" / "pads.wav", pads.data);
    write(root / "project" / "music" / "short.wav", make(music("short", 140)).data);
    const Engine engine(root);

    json::Value added = engine.call("asset.add", R"({"path": "music/boss.wav",
        "description": "Boss fight by Ana.", "author": "Ana", "license": "CC-BY-4.0",
        "bpm": 140, "key": "D minor",
        "stems": {"drums": "music/stems/drums.wav", "pads": "music/stems/pads.wav"}})");
    const std::string id = added["id"].as_string();
    CHECK(id == slash::sha256_hex(mix.data).substr(0, 12));
    added.as_object().erase("id");
    CHECK(compact(added) == R"({"beat_count":24,"bpm":140,"key":"D minor","seconds":10.286,)"
                            R"("stems":["drums","pads"],"type":"music"})");

    // Each stem is music of its own, timed as the track and linked to it.
    json::Value info = engine.call("asset.info", R"({"id": ")" + id + R"("})");
    const std::string drums_id = slash::sha256_hex(drums.data).substr(0, 12);
    CHECK(compact(info["stems"]) == R"([{"id":")" + drums_id + R"(","name":"drums"},{"id":")" +
                                        slash::sha256_hex(pads.data).substr(0, 12) +
                                        R"(","name":"pads"}])");
    json::Value stem = engine.call("asset.info", R"({"id": ")" + drums_id + R"("})");
    CHECK(stem["track"] == id);
    CHECK(stem["type"] == "music");
    CHECK(stem["description"] == "Boss fight by Ana. (drums stem)");
    CHECK(stem["author"] == "Ana");
    CHECK(stem["license"] == "CC-BY-4.0");
    CHECK(stem["bpm"] == 140);
    CHECK(compact(stem["tags"]) == R"(["stem"])");
    CHECK(engine.call("asset.search", R"({"type": "music", "text": "boss"})")["assets"]
              .as_array()
              .size() == 3);

    // Another project plays the stems together, each with its settings.
    write(root / "sequel" / "project.json", R"({"format": 1, "name": "Sequel"})");
    const Engine sequel(root, "sequel");
    json::Value used = sequel.call("asset.use", R"({"id": ")" + id +
                                                    R"(", "path": "music/boss.wav",
        "stems": true, "loop": true})");
    CHECK(compact(used["files"]) ==
          R"(["music/boss/drums.wav","music/boss/drums.audio.json",)"
          R"("music/boss/pads.wav","music/boss/pads.audio.json","music/boss.audio.json"])");
    CHECK(parse(read(root / "sequel" / "music" / "boss.audio.json")) ==
          parse(R"({"format": 1, "synchronized": [{"stream": "music/boss/drums.audio.json"},
                    {"stream": "music/boss/pads.audio.json"}]})"));
    CHECK(parse(read(root / "sequel" / "music" / "boss" / "drums.audio.json")) ==
          parse(R"({"format": 1, "source": "music/boss/drums.wav", "bpm": 140,
                    "beat_count": 24, "loop_mode": "forward"})"));
    CHECK(read(root / "sequel" / "music" / "boss" / "pads.wav") == pads.data);
    json::Value sources = parse(read(root / "sequel" / "library.json"));
    CHECK(sources["assets"]["music/boss/drums.wav"]["author"] == "Ana");
    CHECK(sources["assets"]["music/boss.wav"]["license"] == "CC-BY-4.0");

    // The mix alone, its settings kept where they don't say otherwise.
    write(root / "sequel" / "music" / "mix.audio.json",
          R"({"format": 1, "source": "music/mix.wav", "loop_mode": "forward", "loop_begin": 10})");
    CHECK(compact(sequel.call("asset.use",
                              R"({"id": ")" + id + R"(", "path": "music/mix.wav"})")["files"]) ==
          R"(["music/mix.audio.json"])");
    CHECK(parse(read(root / "sequel" / "music" / "mix.audio.json")) ==
          parse(R"({"format": 1, "source": "music/mix.wav", "bpm": 140, "beat_count": 24,
                    "loop_mode": "forward", "loop_begin": 10})"));

    // An author's tempo stays, with what the sound measures said.
    json::Value disagree = engine.call("asset.add", R"({"path": "music/stems/pads.wav",
        "description": "Pads.", "type": "music", "bpm": 100})");
    CHECK(compact(disagree["problems"]) == R"(["it measures 140 bpm, not the 100 bpm given"])");

    CHECK(engine.fail("asset.add", R"({"path": "music/boss.wav", "description": "x",
                                       "stems": {"Drums": "music/stems/drums.wav"}})") ==
          R"(/stems/Drums: a stem's name is lowercase letters, digits, '_' and '-', such as )"
          R"("drums")");
    CHECK(engine.fail("asset.add", R"({"path": "music/boss.wav", "description": "x",
                                       "stems": {"short": "music/short.wav"}})") ==
          R"(/stems/short: "music/short.wav" lasts 6.857 s and the mix 10.286 s; a track's stems )"
          R"(are as long as its mix)");
    CHECK(engine.fail("asset.add", R"({"path": "music/boss.wav", "description": "x",
                                       "type": "sound", "bpm": 90})") ==
          R"(/bpm: only music takes bpm; set type "music")");
    CHECK(engine.fail("asset.add", R"({"path": "music/boss.wav", "description": "x",
                                       "type": "image"})") ==
          R"(/type: "music/boss.wav" is a sound, not an image)");
    const std::string short_id =
        engine
            .call("asset.add", R"({"path": "music/short.wav", "description": "Short.",
                                     "type": "music"})")["id"]
            .as_string();
    CHECK(engine.fail("asset.use",
                      R"({"id": ")" + short_id + R"(", "path": "music/s.wav", "stems": true})") ==
          "/stems: the music has no stems; asset.add takes them with the track");
}
