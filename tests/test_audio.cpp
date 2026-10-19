#include "slash/asset/assets.hpp"
#include "slash/audio/audio.hpp"
#include "slash/audio/effects.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace audio = slash::audio;
namespace json = slash::json;
namespace world = slash::world;
using audio::Frame;

namespace {

constexpr int kRate = 44100;
constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// A .wav file of the same frame over and over, which every resampler plays back unchanged.
std::string steady_wav(int frames, Frame value, int rate = kRate) {
    const std::vector<Frame> samples(static_cast<std::size_t>(frames), value);
    return audio::encode_wav(samples, rate);
}

// A PCM .wav file of any channel count and sample size, little-endian as the format is.
std::string pcm_wav(int channels, int bits, int rate, const std::vector<std::int32_t>& samples) {
    std::string out;
    const auto put = [&](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
        }
    };
    const auto bytes = static_cast<std::uint32_t>(bits / 8);
    const auto data = static_cast<std::uint32_t>(samples.size()) * bytes;
    out += "RIFF";
    put(36 + data, 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(1, 2);
    put(static_cast<std::uint32_t>(channels), 2);
    put(static_cast<std::uint32_t>(rate), 4);
    put(static_cast<std::uint32_t>(rate * channels) * bytes, 4);
    put(static_cast<std::uint32_t>(channels) * bytes, 2);
    put(static_cast<std::uint32_t>(bits), 2);
    out += "data";
    put(data, 4);
    for (const std::int32_t sample : samples) {
        put(static_cast<std::uint32_t>(sample), static_cast<int>(bytes));
    }
    return out;
}

std::shared_ptr<const audio::Stream> steady_stream(int frames, Frame value,
                                                   const audio::StreamFile& settings = {}) {
    auto sound = audio::decode_sound(steady_wav(frames, value), "steady.wav");
    REQUIRE(sound.ok());
    return audio::sound_stream(std::make_shared<audio::Sound>(std::move(sound).value()), settings);
}

std::vector<Frame> mix(audio::Server& server, int frames) {
    std::vector<Frame> out(static_cast<std::size_t>(frames));
    server.mix(out);
    return out;
}

void check_frame(Frame frame, float left, float right) {
    CHECK(frame.left == doctest::Approx(left).epsilon(0.001));
    CHECK(frame.right == doctest::Approx(right).epsilon(0.001));
}

float half_db() {
    return audio::linear_to_db(0.5F);
}

// Effects outside a server: the mix rate, and a bus buffer of silence for sidechains.
class TestContext final : public audio::MixContext {
public:
    [[nodiscard]] int mix_rate() const override { return kRate; }
    [[nodiscard]] Frame* bus_buffer(std::string_view /*bus*/) override { return silence_.data(); }

private:
    std::vector<Frame> silence_ = std::vector<Frame>(audio::Server::kMixStep);
};

std::unique_ptr<audio::EffectInstance> effect(audio::MixContext& context, std::string_view type,
                                              std::string_view params = "{}") {
    audio::EffectSlot slot;
    slot.type = type;
    slot.params = parse(params);
    auto made = audio::make_effect(slot, context);
    REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
    return std::move(made).value();
}

// Runs an effect over a sound, one mix step at a time.
std::vector<Frame> run(audio::EffectInstance& instance, const std::vector<Frame>& in) {
    constexpr int kStep = audio::Server::kMixStep;
    std::vector<Frame> out(in.size());
    for (std::size_t start = 0; start < in.size(); start += kStep) {
        const int count = static_cast<int>(std::min<std::size_t>(kStep, in.size() - start));
        instance.process(&in[start], &out[start], count);
    }
    return out;
}

std::vector<Frame> sine(double hz, float amplitude, int frames) {
    std::vector<Frame> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * i / static_cast<double>(kRate)));
        out[static_cast<std::size_t>(i)] = {value, value};
    }
    return out;
}

float rms(std::span<const Frame> frames) {
    double sum = 0.0;
    for (const Frame& frame : frames) {
        sum += static_cast<double>(frame.left) * frame.left;
    }
    return frames.empty() ? 0.0F
                          : static_cast<float>(std::sqrt(sum / static_cast<double>(frames.size())));
}

float peak(std::span<const Frame> frames) {
    float most = 0.0F;
    for (const Frame& frame : frames) {
        most = std::max({most, std::abs(frame.left), std::abs(frame.right)});
    }
    return most;
}

// Files in memory; editing one gives it a new stamp, so poll() reloads it.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string text) {
        files[path] = std::move(text);
        ++edits[path];
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

// A world with audio installed, what it played and the events it emitted.
struct Sounds {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<Frame> heard;
    std::vector<world::Event> events;

    explicit Sounds(const audio::Settings& settings = {}) {
        world::register_builtin_components(components);
        audio::install(world, assets, settings);
        audio::set_sink(world, [this](std::span<const Frame> frames) {
            heard.insert(heard.end(), frames.begin(), frames.end());
        });
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity add(std::string_view id, std::string_view kind, std::string_view fields) {
        const entt::entity entity = world.create(id).value();
        std::string path;
        REQUIRE(components.find(kind)->assign(world, entity, parse(fields), path).ok());
        return entity;
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    [[nodiscard]] std::optional<audio::PlayerInfo> player(std::string_view id) const {
        for (audio::PlayerInfo& info : audio::players(world)) {
            if (info.id == id) {
                return info;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }

    // The last frame heard.
    [[nodiscard]] Frame last() const { return heard.empty() ? Frame{} : heard.back(); }
};

} // namespace

TEST_CASE("decibels turn into gains and back as Godot's") {
    CHECK(audio::db_to_linear(0.0F) == doctest::Approx(1.0F));
    CHECK(audio::db_to_linear(-6.0206F) == doctest::Approx(0.5F).epsilon(0.0001));
    CHECK(audio::linear_to_db(2.0F) == doctest::Approx(6.0206F).epsilon(0.0001));
    CHECK(audio::linear_to_db(audio::db_to_linear(-37.5F)) == doctest::Approx(-37.5F));
}

TEST_CASE("sound files decode: .wav files now, .ogg and .mp3 files as they play") {
    CHECK(audio::is_sound_path("sfx/jump.wav"));
    CHECK(audio::is_sound_path("MUSIC/THEME.OGG"));
    CHECK(audio::is_sound_path("voice/line.mp3"));
    CHECK_FALSE(audio::is_sound_path("sfx/jump.audio.json"));
    CHECK_FALSE(audio::is_sound_path("sfx/jump.flac"));

    // encode_wav writes 16-bit stereo, which decodes to the frames it was given.
    auto stereo = audio::decode_sound(steady_wav(100, {0.5F, -0.25F}, 22050), "a.wav");
    REQUIRE(stereo.ok());
    CHECK(stereo.value().format == audio::Sound::Format::Wav);
    CHECK(stereo.value().channels == 2);
    CHECK(stereo.value().mix_rate == 22050);
    CHECK(stereo.value().frames == 100);
    CHECK(stereo.value().length() == doctest::Approx(100.0 / 22050.0));
    REQUIRE(stereo.value().samples.size() == 200);
    CHECK(stereo.value().samples[0] == doctest::Approx(0.5F).epsilon(0.0001));
    CHECK(stereo.value().samples[1] == doctest::Approx(-0.25F).epsilon(0.0001));

    // Mono, 8-bit (unsigned, 128 is silence) and 24-bit files decode too.
    auto mono = audio::decode_sound(pcm_wav(1, 8, 8000, {192, 64, 128}), "b.wav");
    REQUIRE(mono.ok());
    CHECK(mono.value().channels == 1);
    CHECK(mono.value().frames == 3);
    REQUIRE(mono.value().samples.size() == 3);
    CHECK(mono.value().samples[0] == doctest::Approx(0.5F));
    CHECK(mono.value().samples[1] == doctest::Approx(-0.5F));
    CHECK(mono.value().samples[2] == 0.0F);
    auto deep = audio::decode_sound(pcm_wav(2, 24, 48000, {0x400000, 0xC00000}), "c.wav");
    REQUIRE(deep.ok());
    CHECK(deep.value().samples[0] == doctest::Approx(0.5F));
    CHECK(deep.value().samples[1] == doctest::Approx(-0.5F));

    // Errors name the file and what is wrong with it.
    const auto bad = audio::decode_sound("not a wave", "sfx/bad.wav");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "\"sfx/bad.wav\" is not a valid WAV file");
    CHECK(audio::decode_sound("OggS", "theme.ogg").error().message ==
          "\"theme.ogg\" is not a valid Ogg Vorbis file");
    CHECK(audio::decode_sound("", "empty.mp3").error().message == "\"empty.mp3\" is empty");
    CHECK(audio::decode_sound("fLaC", "x.flac").error().message ==
          "\"x.flac\" is not a .wav, .ogg or .mp3 file");
    CHECK(audio::decode_sound(pcm_wav(1, 16, 8000, {}), "silent.wav").error().message ==
          "\"silent.wav\" has no sound in it");
}

TEST_CASE(".audio.json files give a sound settings or pick one of several streams") {
    const auto errors = [](std::string_view text) {
        return audio::validate_stream_file(parse(text));
    };
    CHECK(errors(R"({"format": 1, "source": "sfx/jump.wav", "loop_mode": "forward",
                     "loop_begin": 10, "loop_end": 200})")
              .empty());
    CHECK(errors(R"({"format": 1, "source": "music/theme.ogg", "loop": true,
                     "loop_offset": 4.5})")
              .empty());
    CHECK(errors(R"({"format": 1, "streams": [{"stream": "a.wav"}, {"stream": "b.audio.json",
                     "weight": 2}], "random_pitch": 1.2})")
              .empty());

    const auto first = [&](std::string_view text) {
        const auto found = errors(text);
        REQUIRE_FALSE(found.empty());
        return found.front();
    };
    CHECK(first(R"({"format": 1})").message ==
          "needs one of \"source\" (a sound with settings), \"streams\" (a randomizer), "
          "\"playlist\", \"synchronized\", \"clips\" (music), \"generator\", "
          "\"polyphonic\" or \"microphone\"");
    CHECK(first(R"({"format": 1, "source": "a.wav", "streams": [{"stream": "b.wav"}]})").message ==
          "has both \"source\" and \"streams\"; a file is one sound, randomizer, piece of "
          "music, generator, polyphonic stream or microphone");
    CHECK(first(R"({"format": 1, "source": "a.wav", "loop": true})").path == "/loop");
    CHECK(first(R"({"format": 1, "source": "a.ogg", "loop_mode": "forward"})").path ==
          "/loop_mode");
    CHECK(first(R"({"format": 1, "source": "a.wav", "random_pitch": 2})").path == "/random_pitch");
    const auto end = first(R"({"format": 1, "source": "a.wav", "loop_begin": 50, "loop_end": 50})");
    CHECK(end.path == "/loop_end");
    CHECK(end.message == "must come after loop_begin");
    CHECK(first(R"({"format": 1, "source": "a.png"})").message ==
          "expected a .wav, .ogg or .mp3 file");
    CHECK(first(R"({"format": 1, "source": "../a.wav"})").path == "/source");
    CHECK(first(R"({"format": 1, "streams": [{"stream": "x.png"}]})").path == "/streams/0/stream");
    CHECK(first(R"({"format": 1, "source": "a.wav", "loop_mode": "fowrard"})").path ==
          "/loop_mode");

    auto parsed = audio::parse_stream_file(
        parse(R"({"format": 1, "source": "a.wav", "loop_mode": "pingpong", "loop_end": 9})"));
    REQUIRE(parsed.ok());
    CHECK(parsed.value().source == "a.wav");
    CHECK(parsed.value().loop_mode == audio::LoopMode::PingPong);
    CHECK(parsed.value().loop_end == 9);
    CHECK(audio::loop_mode_name(parsed.value().loop_mode) == "pingpong");
    auto bad = audio::parse_stream_file(parse(R"({"format": 1, "source": "a.wav", "loop": 1})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(contains(bad.error().message, "/loop: "));
}

TEST_CASE(".audio.json files play music: playlists, synchronized streams and clips") {
    const auto errors = [](std::string_view text) {
        return audio::validate_stream_file(parse(text));
    };
    CHECK(errors(R"({"format": 1, "playlist": [{"stream": "a.ogg"}, {"stream": "b.audio.json"}],
                     "shuffle": true, "loop": false, "fade_time": 0.5})")
              .empty());
    CHECK(errors(R"({"format": 1, "synchronized": [{"stream": "drums.ogg"},
                     {"stream": "strings.ogg", "volume_db": -80}]})")
              .empty());
    CHECK(errors(R"({"format": 1, "source": "loop.wav", "loop_mode": "forward", "bpm": 120,
                     "beat_count": 16, "bar_beats": 3})")
              .empty());
    const std::string music = R"({"format": 1, "initial_clip": "explore",
        "clips": [{"name": "explore", "stream": "explore.ogg"},
                  {"name": "battle", "stream": "battle.ogg", "auto_advance": "enabled",
                   "next_clip": "explore"},
                  {"name": "sting", "stream": "sting.wav", "auto_advance": "return_to_hold"}],
        "transitions": [{"to": "battle", "from_time": "next_bar", "to_time": "same_position",
                         "fade_mode": "cross", "fade_beats": 2, "filler_clip": "sting"},
                        {"from": "battle", "to": "sting", "hold_previous": true}]})";
    CHECK(errors(music).empty());

    const auto first = [&](std::string_view text) {
        const auto found = errors(text);
        REQUIRE_FALSE(found.empty());
        return found.front();
    };
    // Members only some kinds take.
    CHECK(first(R"({"format": 1, "source": "a.wav", "shuffle": true})").message ==
          "only a playlist takes this");
    CHECK(first(R"({"format": 1, "streams": [{"stream": "a.wav"}], "loop": true})").message ==
          "only a sound or a playlist takes this");
    CHECK(first(R"({"format": 1, "playlist": [{"stream": "a.wav"}], "transitions": []})").path ==
          "/transitions");
    CHECK(first(R"({"format": 1, "playlist": [{"stream": "a.wav", "weight": 2}]})").path ==
          "/playlist/0/weight");
    CHECK(first(R"({"format": 1, "synchronized": [{"stream": "a.png"}]})").path ==
          "/synchronized/0/stream");
    // Clips are named, and every name must be a clip.
    const auto unknown = first(R"({"format": 1, "clips": [{"name": "explore", "stream": "a.ogg"}],
        "transitions": [{"to": "explor"}]})");
    CHECK(unknown.path == "/transitions/0/to");
    CHECK(unknown.message == R"(no clip is named "explor"; did you mean "explore"?)");
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg"}],
        "initial_clip": "b"})")
              .path == "/initial_clip");
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg"},
        {"name": "a", "stream": "b.ogg"}]})")
              .message == "clip 0 already has this name");
    // A transition for every pair of clips fits, past the 63 clips' count.
    json::Value many = json::parse(R"({"format": 1, "clips": [], "transitions": []})").value();
    for (int i = 0; i < 9; ++i) {
        json::Value clip;
        clip["name"] = "c" + std::to_string(i);
        clip["stream"] = "a.ogg";
        many["clips"].as_array().push_back(std::move(clip));
        for (int j = 0; j < 9; ++j) {
            json::Value transition;
            transition["from"] = "c" + std::to_string(i);
            transition["to"] = "c" + std::to_string(j);
            many["transitions"].as_array().push_back(std::move(transition));
        }
    }
    CHECK(audio::validate_stream_file(many).empty());
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg",
        "auto_advance": "enabled"}]})")
              .message == "auto_advance \"enabled\" needs next_clip");
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg",
        "auto_advance": "enabled", "next_clip": "a"}]})")
              .path == "/clips/0/next_clip");
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg"},
        {"name": "b", "stream": "b.ogg", "next_clip": "a"}]})")
              .path == "/clips/1/next_clip");
    CHECK(first(R"({"format": 1, "clips": [{"name": "a", "stream": "a.ogg"}],
        "transitions": [{"to": "a"}, {"to": "a", "fade_mode": "in"}]})")
              .message == "transition 0 is already from and to the same clips");

    auto parsed = audio::parse_stream_file(parse(music));
    REQUIRE(parsed.ok());
    const audio::StreamFile& file = parsed.value();
    CHECK(file.kind == audio::StreamFile::Kind::Interactive);
    REQUIRE(file.clips.size() == 3);
    CHECK(file.clips[1].auto_advance == audio::AutoAdvance::Enabled);
    CHECK(file.clips[1].next_clip == 0);
    REQUIRE(file.transitions.size() == 2);
    CHECK(file.transitions[0].from_clip == -1);
    CHECK(file.transitions[0].to_clip == 1);
    CHECK(file.transitions[0].from_time == audio::TransitionFrom::NextBar);
    CHECK(file.transitions[0].to_time == audio::TransitionTo::SamePosition);
    CHECK(file.transitions[0].fade_mode == audio::FadeMode::Cross);
    CHECK(file.transitions[0].fade_beats == 2.0F);
    CHECK(file.transitions[0].filler_clip == 2);
    CHECK(file.transitions[1].hold_previous);
    // As Godot's, a playlist loops unless told not to.
    auto playlist = audio::parse_stream_file(parse(R"({"format": 1, "playlist": [
        {"stream": "a.wav"}]})"));
    REQUIRE(playlist.ok());
    CHECK(playlist.value().kind == audio::StreamFile::Kind::Playlist);
    CHECK(playlist.value().loop);
    CHECK(playlist.value().fade_time == doctest::Approx(0.3));
}

TEST_CASE("a playlist plays its streams in turn, each for its beats or its length") {
    // Each sound plays two frames late, for the cubic history its resampler keeps.
    audio::StreamFile beats;
    beats.bpm = 600; // a beat is a tenth of a second
    beats.beat_count = 1;
    audio::StreamFile settings;
    settings.loop = false;
    settings.fade_time = 0.01F;
    const auto list = audio::playlist_stream(
        {steady_stream(kRate, {0.5F, 0.5F}, beats), steady_stream(kRate / 10, {0.25F, 0.25F})},
        settings);
    CHECK(list->length() == doctest::Approx(0.2));
    CHECK(list->bpm() == 600.0);
    auto playback = list->instantiate(kRate, 7);
    playback->start(0.0);
    std::vector<Frame> out(static_cast<std::size_t>(kRate / 2));
    const int mixed = playback->mix(out.data(), 1.0F, static_cast<int>(out.size()));
    // The first sound for its one beat, though it is a second long...
    check_frame(out[2000], 0.5F, 0.5F);
    // ...then the second, as the first fades out under it over a hundredth of a second.
    CHECK(out[4500].left > 0.25F);
    CHECK(out[4500].left < 0.75F);
    check_frame(out[6000], 0.25F, 0.25F);
    // It doesn't loop, so it ends after both: a frame past 8820, as Godot counts each stream's
    // time down a frame at a time.
    CHECK(mixed == 8821);
    CHECK_FALSE(playback->is_playing());

    // Looping, it starts over and counts the loop.
    settings.loop = true;
    const auto looping = audio::playlist_stream(
        {steady_stream(kRate / 10, {0.5F, 0.5F}), steady_stream(kRate / 10, {0.25F, 0.25F})},
        settings);
    CHECK(looping->has_loop());
    auto again = looping->instantiate(kRate, 7);
    again->start(0.15);
    CHECK(again->position() == doctest::Approx(0.15));
    std::vector<Frame> second(static_cast<std::size_t>(kRate / 10));
    CHECK(again->mix(second.data(), 1.0F, static_cast<int>(second.size())) == kRate / 10);
    check_frame(second[1000], 0.25F, 0.25F);
    check_frame(second[3500], 0.5F, 0.5F);
    CHECK(again->loop_count() == 1);
    CHECK(again->position() == doctest::Approx(0.05).epsilon(0.01));
}

TEST_CASE("a synchronized stream plays its layers together, each at its volume") {
    audio::StreamFile settings;
    settings.streams = {{.stream = "a.wav"}, {.stream = "b.wav", .volume_db = half_db()}};
    const auto layers = audio::synchronized_stream(
        {steady_stream(kRate, {0.5F, 0.5F}), steady_stream(kRate / 2, {0.25F, 0.25F})}, settings);
    CHECK(layers->length() == doctest::Approx(1.0));
    auto playback = layers->instantiate(kRate, 0);
    playback->start(0.0);
    std::vector<Frame> out(1024);
    CHECK(playback->mix(out.data(), 1.0F, 1024) == 1024);
    check_frame(out[500], 0.625F, 0.625F);

    // A player's parameter changes a layer's volume, ramping over the next 128 frames.
    CHECK(playback->parameter("stream_1_volume_db").as_double() == doctest::Approx(half_db()));
    playback->set_parameter("stream_1_volume_db", json::Value(0.0));
    playback->set_parameter("stream_7_volume_db", json::Value(0.0)); // no such layer
    playback->set_parameter("volume_db", json::Value(-80.0));
    CHECK(playback->parameter("stream_7_volume_db").is_null());
    CHECK(playback->mix(out.data(), 1.0F, 1024) == 1024);
    CHECK(out[0].left > 0.625F);
    CHECK(out[0].left < 0.64F);
    check_frame(out[127], 0.75F, 0.75F);
    check_frame(out[1000], 0.75F, 0.75F);
    // Once the shorter layer ends, the other plays on alone, and the whole ends with the last.
    std::vector<Frame> rest(static_cast<std::size_t>(kRate));
    CHECK(playback->mix(rest.data(), 1.0F, kRate) == kRate);
    check_frame(rest[30000], 0.5F, 0.5F);
    check_frame(rest[43000], 0.0F, 0.0F);
    // As Godot's, the first call that finds no layer playing still returns its frames, silent.
    CHECK(playback->mix(rest.data(), 1.0F, 128) == 128);
    CHECK_FALSE(playback->is_playing());
    CHECK(playback->mix(rest.data(), 1.0F, 128) == 0);
}

TEST_CASE("interactive music switches clips as its transitions say") {
    // Clips by index: a and b are steady sounds, a with a beat every tenth of a second; sting is
    // short and goes on to b; solo is short and goes on to back, which returns to the clip a
    // hold_previous transition left.
    audio::StreamFile beats;
    beats.bpm = 600;
    beats.loop_mode = audio::LoopMode::Forward;
    audio::StreamFile settings;
    // The streams are given below, so the clips name none.
    const auto clip = [](std::string name, audio::AutoAdvance advance = {}, int next = -1) {
        audio::StreamFile::Clip made;
        made.name = std::move(name);
        made.auto_advance = advance;
        made.next_clip = next;
        return made;
    };
    settings.clips = {clip("a"), clip("b"), clip("sting", audio::AutoAdvance::Enabled, 1),
                      clip("back", audio::AutoAdvance::ReturnToHold),
                      clip("solo", audio::AutoAdvance::Enabled, 3)};
    settings.transitions = {
        {.from_clip = 0,
         .to_clip = 1,
         .from_time = audio::TransitionFrom::NextBeat,
         .fade_mode = audio::FadeMode::Disabled},
        {.from_clip = 1,
         .to_clip = 4,
         .from_time = audio::TransitionFrom::Immediate,
         .fade_mode = audio::FadeMode::Disabled,
         .hold_previous = true},
        {.from_time = audio::TransitionFrom::Immediate, .fade_mode = audio::FadeMode::Disabled}};
    const auto music = audio::interactive_stream(
        {steady_stream(kRate, {0.5F, 0.5F}, beats), steady_stream(kRate, {0.25F, 0.25F}),
         steady_stream(kRate / 10, {0.125F, 0.125F}), steady_stream(kRate / 10, {0.75F, 0.75F}),
         steady_stream(kRate / 10, {0.375F, 0.375F})},
        settings);
    CHECK(music->length() == 0.0);
    auto playback = music->instantiate(kRate, 0);
    playback->start(0.0);
    std::vector<Frame> out(2048);
    const auto mix_frames = [&](int frames) {
        out.resize(static_cast<std::size_t>(frames));
        REQUIRE(playback->mix(out.data(), 1.0F, frames) == frames);
    };
    mix_frames(2048);
    CHECK(playback->clip() == "a");
    CHECK(playback->parameter("switch_to_clip") == json::Value("a"));
    check_frame(out[1000], 0.5F, 0.5F);

    // A switch to b waits for a's next beat, at frame 4410; it names b at once.
    playback->set_parameter("switch_to_clip", json::Value("b"));
    CHECK(playback->parameter("switch_to_clip") == json::Value("b"));
    mix_frames(2048);
    CHECK(playback->clip() == "a");
    CHECK(playback->parameter("switch_to_clip") == json::Value("b"));
    check_frame(out[2000], 0.5F, 0.5F); // frame 4096
    mix_frames(2048);
    CHECK(playback->clip() == "b");
    check_frame(out[1000], 0.25F, 0.25F); // frame 5144
    // Asking for the clip it plays changes nothing.
    playback->set_parameter("switch_to_clip", json::Value("b"));
    playback->set_parameter("switch_to_clip", json::Value("nothing"));
    mix_frames(2048);
    check_frame(out[2000], 0.25F, 0.25F);

    // The sting plays at once, then goes on to b at its end.
    playback->set_parameter("switch_to_clip", json::Value("sting"));
    mix_frames(2048);
    CHECK(playback->clip() == "sting");
    check_frame(out[1000], 0.125F, 0.125F);
    mix_frames(4096);
    CHECK(playback->clip() == "b");
    CHECK(playback->parameter("switch_to_clip") == json::Value("b"));
    check_frame(out[4000], 0.25F, 0.25F);

    // From b to solo holds b; solo goes on to back, and at back's end the music returns to b.
    playback->set_parameter("switch_to_clip", json::Value("solo"));
    mix_frames(2048);
    CHECK(playback->clip() == "solo");
    check_frame(out[1000], 0.375F, 0.375F);
    mix_frames(4096);
    CHECK(playback->clip() == "back");
    check_frame(out[4000], 0.75F, 0.75F);
    mix_frames(4096);
    CHECK(playback->clip() == "b");
    check_frame(out[4000], 0.25F, 0.25F);
    CHECK(playback->position() > 0.0);
}

TEST_CASE("a player's parameters reach its playbacks, and switch_to_clip follows the music") {
    Sounds sounds;
    sounds.files.write("music/a.wav", steady_wav(kRate, {0.5F, 0.5F}));
    sounds.files.write("music/b.wav", steady_wav(kRate / 10, {0.25F, 0.25F}));
    sounds.files.write("music/score.audio.json", R"({"format": 1,
        "clips": [{"name": "calm", "stream": "music/a.wav"},
                  {"name": "fight", "stream": "music/b.wav", "auto_advance": "enabled",
                   "next_clip": "calm"}],
        "transitions": [{"from_time": "immediate", "fade_mode": "disabled"}]})");
    const entt::entity music = sounds.add("music", "AudioStreamPlayer",
                                          R"({"stream": "music/score.audio.json", "autoplay": true,
            "parameters": {"switch_to_clip": "calm"}})");
    sounds.step();
    CHECK(sounds.player("music")->clip == "calm");
    check_frame(sounds.last(), 0.5F, 0.5F);

    auto& player = sounds.world.ecs().get<world::AudioStreamPlayer>(music);
    player.parameters["switch_to_clip"] = json::Value("fight");
    sounds.step();
    CHECK(sounds.player("music")->clip == "fight");
    check_frame(sounds.last(), 0.25F, 0.25F);
    // Fight lasts a tenth of a second, then goes back to calm, and the parameter follows.
    sounds.step(8);
    CHECK(sounds.player("music")->clip == "calm");
    CHECK(sounds.world.ecs().get<world::AudioStreamPlayer>(music).parameters.at("switch_to_clip") ==
          json::Value("calm"));
    check_frame(sounds.last(), 0.5F, 0.5F);
    // So asking for fight again plays it again.
    sounds.world.ecs().get<world::AudioStreamPlayer>(music).parameters["switch_to_clip"] =
        json::Value("fight");
    sounds.step();
    CHECK(sounds.player("music")->clip == "fight");

    // A voice that starts later gets the parameters it was given before it plays.
    sounds.files.write("music/layers.audio.json", R"({"format": 1, "synchronized": [
        {"stream": "music/a.wav"}, {"stream": "music/a.wav", "volume_db": -80}]})");
    sounds.add("layers", "AudioStreamPlayer",
               R"({"stream": "music/layers.audio.json", "volume_db": -80,
                   "parameters": {"stream_1_volume_db": 0}})");
    const auto& layers =
        sounds.world.ecs().get<world::AudioStreamPlayer>(sounds.world.find("layers"));
    CHECK(layers.parameters.size() == 1);
}

TEST_CASE(".audio.json files make generators and polyphonic streams") {
    const auto generator = audio::parse_stream_file(parse(R"({"format": 1,
        "generator": {"mix_rate": 22050, "buffer_length": 0.25}})"));
    REQUIRE(generator.ok());
    CHECK(generator.value().kind == audio::StreamFile::Kind::Generator);
    CHECK(generator.value().mix_rate == 22050.0F);
    CHECK(generator.value().buffer_length == 0.25F);
    CHECK(generator.value().mix_rate_mode == audio::MixRateMode::Custom);
    const auto output = audio::parse_stream_file(
        parse(R"({"format": 1, "generator": {"mix_rate_mode": "output"}})"));
    REQUIRE(output.ok());
    CHECK(output.value().mix_rate_mode == audio::MixRateMode::Output);
    CHECK(output.value().buffer_length == 0.5F);
    const auto polyphonic = audio::parse_stream_file(parse(R"({"format": 1, "polyphonic": {}})"));
    REQUIRE(polyphonic.ok());
    CHECK(polyphonic.value().kind == audio::StreamFile::Kind::Polyphonic);
    CHECK(polyphonic.value().polyphony == 32);

    const auto errors_of = [](std::string_view text) {
        return audio::validate_stream_file(parse(text));
    };
    auto errors = errors_of(R"({"format": 1, "generator": {}, "polyphonic": {}})");
    REQUIRE(errors.size() == 1);
    CHECK(contains(errors[0].message, "has both \"generator\" and \"polyphonic\""));
    errors = errors_of(R"({"format": 1, "generator": {}, "loop": true})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/loop");
    errors = errors_of(R"({"format": 1, "polyphonic": {"polyphony": 0}})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/polyphonic/polyphony");
    errors = errors_of(R"({"format": 1, "generator": {"rate": 8000}})");
    REQUIRE_FALSE(errors.empty());
    CHECK(contains(errors[0].path, "/generator"));
}

TEST_CASE("a generator plays the frames a game pushes, and counts the mixes that ran out") {
    audio::StreamFile settings;
    settings.kind = audio::StreamFile::Kind::Generator;
    const auto stream = audio::generator_stream(settings);
    CHECK(stream->is_monophonic());
    CHECK(stream->length() == 0.0);
    const auto playback = stream->instantiate(kRate, 0);
    audio::GeneratorPlayback* generator = playback->generator();
    REQUIRE(generator != nullptr);
    CHECK(playback->polyphonic() == nullptr);
    // Half a second at 44100 Hz rounds up to 32768 frames, one of which Godot's ring buffer
    // always keeps free.
    CHECK(generator->frames_available() == 32767);
    playback->start(0.0);
    std::vector<Frame> pushed(300);
    for (std::size_t i = 0; i < pushed.size(); ++i) {
        const float value = static_cast<float>(i) / 1000.0F;
        pushed[i] = {value, -value};
    }
    CHECK(generator->push_frame(pushed[0]));
    CHECK(generator->push_buffer(std::span<const Frame>(pushed).subspan(1)));
    CHECK(generator->frames_available() == 32467);
    CHECK(generator->can_push_buffer(32467));
    CHECK_FALSE(generator->can_push_buffer(32468));
    // Frames that don't all fit aren't added.
    CHECK_FALSE(generator->push_buffer(std::vector<Frame>(32468)));
    CHECK(generator->frames_available() == 32467);

    std::vector<Frame> out(1024);
    CHECK(playback->mix(out.data(), 1.0F, 1024) == 1024);
    // At the mix rate, the resampler hands the frames through unchanged, 130 frames late: its
    // 128-frame buffer and two frames of cubic history.
    check_frame(out[129], 0.0F, 0.0F);
    for (std::size_t i = 0; i < pushed.size(); ++i) {
        check_frame(out[130 + i], pushed[i].left, pushed[i].right);
    }
    check_frame(out[430], 0.0F, 0.0F);
    // It read 128 frames eight times: the first two found frames, the others ran out.
    CHECK(generator->skips() == 6);
    CHECK(generator->frames_available() == 32767);
    CHECK(playback->position() == doctest::Approx(1024.0 / kRate));

    // The buffer clears only once the generator stopped, as Godot's.
    CHECK(generator->push_buffer(std::vector<Frame>(10, {0.5F, 0.5F})));
    generator->clear_buffer();
    CHECK(generator->frames_available() == 32757);
    playback->stop();
    CHECK_FALSE(playback->is_playing());
    generator->clear_buffer();
    CHECK(generator->frames_available() == 32767);
    // Stopped, it ends at its next read.
    CHECK(playback->mix(out.data(), 1.0F, 256) == 128);

    // Its own rate sizes the buffer, or the mix rate's with mix_rate_mode "output".
    settings.mix_rate = 22050.0F;
    settings.buffer_length = 0.25F;
    CHECK(
        audio::generator_stream(settings)->instantiate(kRate, 0)->generator()->frames_available() ==
        8191);
    settings.mix_rate_mode = audio::MixRateMode::Output;
    CHECK(
        audio::generator_stream(settings)->instantiate(96000, 0)->generator()->frames_available() ==
        32767);
}

TEST_CASE("a polyphonic stream plays streams a game starts, each at its own volume") {
    audio::StreamFile settings;
    settings.kind = audio::StreamFile::Kind::Polyphonic;
    settings.polyphony = 2;
    const auto stream = audio::polyphonic_stream(settings);
    CHECK(stream->is_monophonic());
    const auto playback = stream->instantiate(kRate, 7);
    audio::PolyphonicPlayback* polyphonic = playback->polyphonic();
    REQUIRE(polyphonic != nullptr);
    CHECK(playback->generator() == nullptr);
    CHECK(polyphonic->polyphony() == 2);
    playback->start(0.0);

    using Id = audio::PolyphonicPlayback::StreamId;
    const auto loud = steady_stream(kRate, {0.5F, 0.5F});
    const auto beep = steady_stream(kRate / 100, {0.25F, 0.25F});
    const Id first = polyphonic->play_stream(loud, 0.0, -6.0206F, 1.0F);
    const Id second = polyphonic->play_stream(beep, 0.0, 0.0F, 1.0F);
    // Godot's IDs: the slot in the high 32 bits and a count in the low ones.
    CHECK(first == 1);
    CHECK(second == ((Id{1} << 32U) | 2));
    CHECK(polyphonic->play_stream(loud, 0.0, 0.0F, 1.0F) == audio::PolyphonicPlayback::kInvalidId);
    CHECK(polyphonic->streams_playing() == 2);
    CHECK(polyphonic->is_stream_playing(second));
    CHECK_FALSE(polyphonic->is_stream_playing(second + 1));
    CHECK_FALSE(polyphonic->is_stream_playing(audio::PolyphonicPlayback::kInvalidId));

    std::vector<Frame> out(512);
    // A polyphonic stream plays until it is stopped, whatever its streams do.
    CHECK(playback->mix(out.data(), 1.0F, 512) == 512);
    check_frame(out[100], 0.5F, 0.5F);
    // The beep's 441 frames ended, and its slot is free.
    check_frame(out[480], 0.25F, 0.25F);
    CHECK_FALSE(polyphonic->is_stream_playing(second));
    CHECK(polyphonic->streams_playing() == 1);

    // A new volume applies at the next mix, as Godot's.
    polyphonic->set_stream_volume(first, 0.0F);
    playback->mix(out.data(), 1.0F, 512);
    check_frame(out[0], 0.5F, 0.5F);
    // A stop fades out over the next mix, then frees the slot.
    polyphonic->stop_stream(first);
    CHECK(polyphonic->is_stream_playing(first));
    playback->mix(out.data(), 1.0F, 512);
    check_frame(out[0], 0.5F, 0.5F);
    check_frame(out[256], 0.25F, 0.25F);
    CHECK(out[511].left < 0.01F);
    CHECK_FALSE(polyphonic->is_stream_playing(first));
    CHECK(polyphonic->streams_playing() == 0);
    // A stream stopped before it played never plays.
    const Id third = polyphonic->play_stream(beep, 0.0, 0.0F, 1.0F);
    CHECK(third == 3);
    polyphonic->stop_stream(third);
    playback->mix(out.data(), 1.0F, 512);
    check_frame(out[100], 0.0F, 0.0F);
    CHECK(polyphonic->streams_playing() == 0);

    // A faster pitch scale plays a stream through sooner.
    const Id fast = polyphonic->play_stream(beep, 0.0, 0.0F, 1.0F);
    polyphonic->set_stream_pitch_scale(fast, 2.0F);
    playback->mix(out.data(), 1.0F, 512);
    check_frame(out[200], 0.25F, 0.25F);
    check_frame(out[300], 0.0F, 0.0F);
    // Stopping the playback drops every stream.
    CHECK(polyphonic->play_stream(loud, 0.0, 0.0F, 1.0F) != audio::PolyphonicPlayback::kInvalidId);
    playback->stop();
    CHECK(polyphonic->streams_playing() == 0);
    CHECK(playback->mix(out.data(), 1.0F, 512) == 0);
}

TEST_CASE("a game pushes frames to a generator player and plays streams on a polyphonic one") {
    Sounds sounds;
    sounds.files.write("audio/synth.audio.json",
                       R"({"format": 1, "generator": {"buffer_length": 0.1}})");
    sounds.files.write("audio/sfx.audio.json", R"({"format": 1, "polyphonic": {"polyphony": 4}})");
    sounds.files.write("audio/hit.wav", steady_wav(kRate / 10, {0.25F, 0.25F}));
    const entt::entity synth =
        sounds.add("synth", "AudioStreamPlayer",
                   R"({"stream": "audio/synth.audio.json", "autoplay": true, "max_polyphony": 3})");
    // A gameplay system keeps the generator full, as Godot's AudioStreamGenerator demo does.
    bool pushing = true;
    sounds.world.add_system("synth", [&](world::World& w, double /*seconds*/) {
        audio::Playback* playback = audio::stream_playback(w, synth);
        if (!pushing || playback == nullptr) {
            return;
        }
        audio::GeneratorPlayback* generator = playback->generator();
        REQUIRE(generator != nullptr);
        while (generator->push_frame({0.5F, 0.5F})) {
        }
    });
    CHECK(audio::stream_playback(sounds.world, synth) == nullptr);
    // The voice starts when the audio system runs, after the gameplay systems, so it runs out
    // of frames in its first tick.
    sounds.step();
    CHECK(audio::stream_playback(sounds.world, synth) != nullptr);
    sounds.step(2);
    const int skips = sounds.player("synth")->generator->skips;
    sounds.step(3);
    check_frame(sounds.last(), 0.5F, 0.5F);
    auto info = sounds.player("synth");
    REQUIRE(info->generator.has_value());
    CHECK(info->generator->skips == skips);
    CHECK(info->generator->frames_available < 8191);
    CHECK_FALSE(info->polyphonic.has_value());

    // play() starts a generator over rather than adding a voice, as it is monophonic.
    audio::play(sounds.world, synth);
    sounds.step();
    CHECK(sounds.player("synth")->voices == 1);
    // When the game stops pushing, the mixes run out: silence, counted as skips.
    pushing = false;
    sounds.step(20);
    check_frame(sounds.last(), 0.0F, 0.0F);
    CHECK(sounds.player("synth")->generator->skips > 0);
    CHECK(sounds.player("synth")->generator->frames_available == 8191);

    // play_stream starts the player, then the streams on it.
    const entt::entity sfx =
        sounds.add("sfx", "AudioStreamPlayer", R"({"stream": "audio/sfx.audio.json"})");
    REQUIRE(audio::play_stream(sounds.world, sfx, "audio/hit.wav", 0.0, -6.0206F).ok());
    REQUIRE(audio::play_stream(sounds.world, sfx, "audio/hit.wav").ok());
    sounds.step();
    info = sounds.player("sfx");
    CHECK(info->voices == 1);
    REQUIRE(info->polyphonic.has_value());
    CHECK(info->polyphonic->playing == 2);
    CHECK(info->polyphonic->polyphony == 4);
    check_frame(sounds.last(), 0.375F, 0.375F);
    // The hits end, and the player plays on, silent, for the next ones.
    sounds.step(10);
    CHECK(sounds.player("sfx")->polyphonic->playing == 0);
    CHECK(sounds.player("sfx")->voices == 1);
    REQUIRE(audio::play_stream(sounds.world, sfx, "audio/hit.wav", 0.0, 0.0F, 2.0F).ok());
    sounds.step();
    CHECK(sounds.player("sfx")->polyphonic->playing == 1);

    auto refused = audio::play_stream(sounds.world, synth, "audio/hit.wav");
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "isn't polyphonic"));
    CHECK_FALSE(audio::play_stream(sounds.world, sfx, "audio/missing.wav").ok());
}

// A microphone for tests: a ramp from -0.5 to 0.5 every 1000 frames, mirrored on the right, so
// each frame tells where in the recording it came from.
class RampMicrophone final : public audio::InputDevice {
public:
    slash::Result<void> start(int mix_rate) override {
        ++starts;
        rate = mix_rate;
        if (refuse) {
            return slash::make_error(slash::ErrorCode::PlatformFailure, "permission denied");
        }
        return {};
    }
    void stop() override { ++stops; }
    void read(std::span<Frame> frames) override {
        for (Frame& frame : frames) {
            frame = sample(next++);
        }
    }
    [[nodiscard]] std::string name() const override { return "ramp"; }

    static Frame sample(std::uint64_t index) {
        const float value = static_cast<float>(index % 1000) / 1000.0F - 0.5F;
        return {value, -value};
    }

    int starts = 0;
    int stops = 0;
    int rate = 0;
    std::uint64_t next = 0;
    bool refuse = false;
};

audio::Settings with_input() {
    audio::Settings settings;
    settings.enable_input = true;
    return settings;
}

TEST_CASE("the server records input once it is enabled, as Godot's AudioServer input calls") {
    audio::Server server(kRate);
    CHECK_FALSE(server.set_input_active(true));
    CHECK_FALSE(server.input_active());
    CHECK_FALSE(server.acquire_input());
    server.set_input_enabled(true);
    CHECK(server.input_buffer_length_frames() == kRate);
    std::vector<Frame> frames(100);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i] = {static_cast<float>(i) / 100.0F, -static_cast<float>(i) / 100.0F};
    }
    server.push_input(frames); // input is off, so nothing records
    CHECK(server.input_recorded() == 0);
    REQUIRE(server.set_input_active(true));
    CHECK(server.input_active());
    server.push_input(frames);
    CHECK(server.input_frames_available() == 100);
    CHECK(server.input_frames(101).empty());
    const std::vector<Frame> read = server.input_frames(40);
    REQUIRE(read.size() == 40);
    CHECK(read.front() == frames[0]);
    CHECK(read.back() == frames[39]);
    CHECK(server.input_frames_available() == 60);
    CHECK(server.input_frame(99) == frames[99]);
    CHECK(server.input_frame(100) == Frame{});
    // The buffer keeps the newest second.
    server.push_input(std::vector<Frame>(kRate, Frame{0.5F, 0.5F}));
    CHECK(server.input_frames_available() == kRate);
    CHECK(server.input_frame(0) == Frame{});
    CHECK(server.input_frames(kRate).back() == Frame{0.5F, 0.5F});
    // A microphone playback keeps input on after the game lets go, and it starts over when it
    // next turns on.
    REQUIRE(server.acquire_input());
    REQUIRE(server.set_input_active(false));
    CHECK(server.input_active());
    server.release_input();
    CHECK_FALSE(server.input_active());
    REQUIRE(server.set_input_active(true));
    CHECK(server.input_recorded() == 0);
    CHECK(server.input_frames_available() == 0);
}

TEST_CASE("a microphone player plays what the input device records, 50 ms late") {
    Sounds sounds(with_input());
    sounds.files.write("audio/mic.audio.json", R"({"format": 1, "microphone": {}})");
    RampMicrophone microphone;
    audio::set_input_device(sounds.world, &microphone);
    CHECK(audio::input_info(sounds.world).source == "ramp");
    sounds.step();
    // Nothing turned input on, so the microphone stays closed.
    CHECK(microphone.starts == 0);
    CHECK_FALSE(audio::input_info(sounds.world).active);

    sounds.add("voice", "AudioStreamPlayer",
               R"({"stream": "audio/mic.audio.json", "autoplay": true})");
    sounds.heard.clear();
    sounds.step(30);
    CHECK(microphone.starts == 1);
    CHECK(microphone.rate == kRate);
    CHECK(sounds.player("voice")->voices == 1);
    audio::InputInfo info = audio::input_info(sounds.world);
    CHECK(info.active);
    CHECK(info.peak_db == doctest::Approx(-6.0206F).epsilon(0.001));
    // At the mix rate the resampler copies frames, so the output is the recording. Input turned
    // on at frame 735 of the mix, and 50 ms (2205 frames) was recorded by the end of the fourth
    // tick, so the mix step starting at frame 2560 is the first to play it, after the
    // resampler's buffer (130 frames) and the lookahead (64): frame 2754, 2019 frames after
    // input turned on. The mix runs up to a step ahead of the ticks, so that is under 50 ms.
    std::size_t first = 0;
    while (first < sounds.heard.size() && sounds.heard[first] == Frame{}) {
        ++first;
    }
    REQUIRE(first < sounds.heard.size());
    CHECK(first == 2560 + 130 + 64 - 735);
    for (std::size_t i = first; i < sounds.heard.size(); ++i) {
        REQUIRE(sounds.heard[i] == RampMicrophone::sample(i - first));
    }

    // Stopping the player turns input off, which closes the microphone.
    audio::stop(sounds.world, sounds.world.find("voice"));
    sounds.step(2);
    CHECK(microphone.stops == 1);
    CHECK_FALSE(audio::input_info(sounds.world).active);
    // A microphone that can't open records silence, and says why.
    microphone.refuse = true;
    audio::play(sounds.world, sounds.world.find("voice"));
    sounds.step(10);
    CHECK(microphone.starts == 2);
    info = audio::input_info(sounds.world);
    CHECK(info.error == "permission denied");
    CHECK(info.peak_db == -200.0F);
    check_frame(sounds.last(), 0.0F, 0.0F);
    audio::set_input_device(sounds.world, nullptr);
    CHECK(audio::input_info(sounds.world).source.empty());
}

TEST_CASE("agents play a sound into the input in place of the microphone") {
    Sounds sounds(with_input());
    sounds.files.write("audio/mic.audio.json", R"({"format": 1, "microphone": {}})");
    sounds.files.write("audio/voice.wav", steady_wav(kRate, {0.25F, -0.25F}));
    RampMicrophone microphone;
    audio::set_input_device(sounds.world, &microphone);
    sounds.add("voice", "AudioStreamPlayer",
               R"({"stream": "audio/mic.audio.json", "autoplay": true})");
    sounds.step();
    CHECK(microphone.starts == 1);

    // A failed command takes its ask back.
    const audio::Snapshot before = audio::save(sounds.world);
    REQUIRE(audio::set_input_stream(sounds.world, "audio/voice.wav").ok());
    CHECK(audio::save(sounds.world).input_stream == "audio/voice.wav");
    audio::restore(sounds.world, before);
    CHECK(audio::input_info(sounds.world).source == "ramp");

    REQUIRE(audio::set_input_stream(sounds.world, "audio/voice.wav").ok());
    CHECK(audio::input_info(sounds.world).source == "audio/voice.wav");
    sounds.step(10);
    // The sound plays in place of the microphone, which closes.
    CHECK(microphone.stops == 1);
    CHECK(audio::input_info(sounds.world).peak_db == doctest::Approx(-12.0412F).epsilon(0.001));
    check_frame(sounds.last(), 0.25F, -0.25F);
    // The sound ends, and the input is silent until it goes back to the microphone.
    sounds.step(60);
    check_frame(sounds.last(), 0.0F, 0.0F);
    REQUIRE(audio::set_input_stream(sounds.world, "").ok());
    sounds.step(10);
    CHECK(microphone.starts == 2);
    CHECK(audio::input_info(sounds.world).source == "ramp");

    auto missing = audio::set_input_stream(sounds.world, "audio/missing.wav");
    CHECK_FALSE(missing.ok());
    auto itself = audio::set_input_stream(sounds.world, "audio/mic.audio.json");
    REQUIRE_FALSE(itself.ok());
    CHECK(contains(itself.error().message, "is a microphone"));
}

TEST_CASE("a microphone needs audio.enable_input and plays only as a player's own stream") {
    Sounds sounds;
    sounds.files.write("audio/mic.audio.json", R"({"format": 1, "microphone": {}})");
    sounds.files.write("audio/random.audio.json",
                       R"({"format": 1, "streams": [{"stream": "audio/mic.audio.json"}]})");
    sounds.files.write("audio/sfx.audio.json", R"({"format": 1, "polyphonic": {}})");
    CHECK(audio::validate_stream_file(parse(R"({"format": 1, "microphone": {}})")).empty());
    CHECK(audio::load_stream(sounds.assets, "audio/mic.audio.json").ok());
    auto nested = audio::load_stream(sounds.assets, "audio/random.audio.json");
    REQUIRE_FALSE(nested.ok());
    CHECK(nested.error().message ==
          R"("audio/mic.audio.json" is a microphone, which plays only as a player's own stream)");

    // Without enable_input, input doesn't turn on, so the microphone player stops.
    sounds.add("voice", "AudioStreamPlayer",
               R"({"stream": "audio/mic.audio.json", "autoplay": true})");
    sounds.step();
    CHECK(sounds.player("voice")->voices == 0);
    CHECK(sounds.count("finished") == 1);
    CHECK_FALSE(audio::input_info(sounds.world).enabled);
    auto off = audio::set_input_stream(sounds.world, "audio/mic.audio.json");
    REQUIRE_FALSE(off.ok());
    CHECK(contains(off.error().message, "audio.enable_input"));

    const entt::entity sfx =
        sounds.add("sfx", "AudioStreamPlayer", R"({"stream": "audio/sfx.audio.json"})");
    auto played = audio::play_stream(sounds.world, sfx, "audio/mic.audio.json");
    REQUIRE_FALSE(played.ok());
    CHECK(contains(played.error().message, "plays only as a player's own stream"));
}

TEST_CASE("a sound plays through Master a lookahead late, then its playback ends") {
    audio::Server server(kRate);
    CHECK(server.layout().buses.size() == 1);
    CHECK(server.bus_index("Master") == 0);
    const auto stream = steady_stream(1000, {0.5F, 0.25F});
    const audio::Server::PlaybackId id =
        server.start(stream->instantiate(kRate, 0), "Master", {1.0F, 1.0F}, 0.0, 1.0F);
    CHECK(server.is_active(id));
    CHECK(server.playback_count() == 1);

    // A playback reads its first 128 frames ahead, into the buffer it resamples from.
    CHECK(server.position(id) == doctest::Approx(128.0 / kRate));
    const std::vector<Frame> out = mix(server, 1024);
    // Godot keeps 64 frames of lookahead, so a playback that ends can fade out, and cubic
    // interpolation starts two frames of history late.
    check_frame(out[0], 0.0F, 0.0F);
    check_frame(out[65], 0.0F, 0.0F);
    check_frame(out[66], 0.5F, 0.25F);
    check_frame(out[900], 0.5F, 0.25F);
    CHECK(server.mixed_frames() == 1024);
    CHECK(server.bus_peak_db(0).left == doctest::Approx(half_db()).epsilon(0.001));

    const std::vector<Frame> rest = mix(server, 2048);
    CHECK(server.playback_count() == 0);
    CHECK_FALSE(server.is_active(id));
    check_frame(rest.back(), 0.0F, 0.0F);
    // Silent and finished, Master goes quiet once it has been under -60 dB for two seconds.
    mix(server, 2 * kRate + 1024);
    CHECK(server.bus_peak_db(0).left == -200.0F);
}

TEST_CASE("a playback's volume and pitch scale change what is heard and how fast it plays") {
    audio::Server server(kRate);
    const auto stream = steady_stream(kRate, {0.5F, 0.5F});
    const auto id = server.start(stream->instantiate(kRate, 0), "Master", {0.5F, 1.0F}, 0.0, 2.0F);
    const std::vector<Frame> out = mix(server, 1024);
    check_frame(out[1000], 0.25F, 0.5F);
    // Twice as fast: two mix steps of 512 frames read 2048 frames of the sound, after the 128
    // read ahead.
    CHECK(server.position(id) == doctest::Approx((128.0 + 2048.0) / kRate).epsilon(0.0001));

    // A change of volume ramps over the next mix step, so it doesn't click.
    server.set_bus_exclusive(id, "Master", {1.0F, 1.0F});
    server.set_pitch_scale(id, 1.0F);
    const std::vector<Frame> ramp = mix(server, 512);
    CHECK(ramp[0].left < 0.3F);
    check_frame(ramp[511], 0.5F, 0.5F);
    CHECK(server.position(id) == doctest::Approx((128.0 + 2560.0) / kRate).epsilon(0.0001));

    // Starting from a position skips into the sound; past its end, nothing plays.
    const auto late =
        server.start(stream->instantiate(kRate, 0), "Master", {1.0F, 1.0F}, 0.75, 1.0F);
    mix(server, 512);
    CHECK(server.position(late) ==
          doctest::Approx(0.75 + ((128.0 + 512.0) / kRate)).epsilon(0.0001));
}

TEST_CASE("buses mix into the bus they send to, with volume, mute and solo") {
    audio::Server server(kRate);
    auto layout = audio::parse_bus_layout(parse(R"({"format": 1, "buses": [
        {"name": "Master"},
        {"name": "Music", "volume_db": -6.0206},
        {"name": "Voice", "send": "Music"}
    ]})"));
    REQUIRE(layout.ok());
    server.set_layout(layout.value());
    CHECK(server.bus_index("Voice") == 2);
    CHECK(server.bus_index("voice") == -1);

    server.start(steady_stream(kRate, {0.5F, 0.5F})->instantiate(kRate, 0), "Music", {1.0F, 1.0F},
                 0.0, 1.0F);
    server.start(steady_stream(kRate, {0.25F, 0.25F})->instantiate(kRate, 0), "Voice", {1.0F, 1.0F},
                 0.0, 1.0F);
    // Voice goes through Music, which halves both.
    check_frame(mix(server, 1024).back(), 0.375F, 0.375F);
    CHECK(server.bus_peak_db(2).left == doctest::Approx(audio::linear_to_db(0.25F)).epsilon(0.001));

    server.set_bus_mute(2, true);
    check_frame(mix(server, 512).back(), 0.25F, 0.25F);
    server.set_bus_mute(2, false);
    // With Music solo, buses that aren't solo and don't send to a solo bus are silent.
    server.set_bus_solo(1, true);
    check_frame(mix(server, 512).back(), 0.25F, 0.25F);
    CHECK(server.layout().buses[1].solo);
    server.set_bus_solo(1, false);
    // Voice solo: Music, which it sends to, plays too.
    server.set_bus_solo(2, true);
    check_frame(mix(server, 512).back(), 0.375F, 0.375F);
    server.set_bus_solo(2, false);
    server.set_bus_volume_db(1, 0.0F);
    check_frame(mix(server, 512).back(), 0.75F, 0.75F);
    CHECK(server.layout().buses[1].volume_db == 0.0F);

    // A bus the layout doesn't have plays through Master.
    server.start(steady_stream(kRate, {0.125F, 0.125F})->instantiate(kRate, 0), "Nowhere",
                 {1.0F, 1.0F}, 0.0, 1.0F);
    check_frame(mix(server, 1024).back(), 0.875F, 0.875F);
}

TEST_CASE("stopping fades out over one mix step, and pausing keeps the position") {
    audio::Server server(kRate);
    const auto stream = steady_stream(kRate, {0.5F, 0.5F});
    const auto id = server.start(stream->instantiate(kRate, 0), "Master", {1.0F, 1.0F}, 0.0, 1.0F);
    mix(server, 1024);
    server.set_paused(id, true);
    CHECK(server.is_paused(id));
    CHECK_FALSE(server.is_active(id));
    const std::vector<Frame> fading = mix(server, 512);
    check_frame(fading[0], 0.5F, 0.5F);
    CHECK(fading[511].left < 0.01F);
    const double held = server.position(id);
    check_frame(mix(server, 2048).back(), 0.0F, 0.0F);
    CHECK(server.position(id) == held);

    server.set_paused(id, false);
    CHECK(server.is_active(id));
    check_frame(mix(server, 1024).back(), 0.5F, 0.5F);
    CHECK(server.position(id) == doctest::Approx(held + 1024.0 / kRate));

    server.stop(id);
    CHECK_FALSE(server.is_active(id));
    const std::vector<Frame> stopping = mix(server, 512);
    check_frame(stopping[0], 0.5F, 0.5F);
    CHECK(stopping[511].left < 0.01F);
    CHECK(server.playback_count() == 0);

    // A playback started paused waits for set_paused(false).
    const auto waiting =
        server.start(stream->instantiate(kRate, 0), "Master", {1.0F, 1.0F}, 0.0, 1.0F, true);
    check_frame(mix(server, 1024).back(), 0.0F, 0.0F);
    CHECK(server.position(waiting) == doctest::Approx(128.0 / kRate));
    server.set_paused(waiting, false);
    check_frame(mix(server, 1024).back(), 0.5F, 0.5F);
}

TEST_CASE("every effect runs with its defaults") {
    TestContext context;
    const std::vector<std::string_view> types = audio::effect_types();
    CHECK(types.size() == 24);
    CHECK(std::ranges::find(types, "reverb") != types.end());
    const std::vector<Frame> in = sine(440.0, 0.5F, 8 * audio::Server::kMixStep);
    for (const std::string_view type : types) {
        CAPTURE(type);
        const auto instance = effect(context, type);
        const std::vector<Frame> out = run(*instance, in);
        CHECK(std::ranges::all_of(out, [](const Frame& frame) {
            return std::isfinite(frame.left) && std::isfinite(frame.right);
        }));
        CHECK(peak(out) < 4.0F);
        CHECK(peak(out) > 0.01F);
    }
}

TEST_CASE("effects change the sound as Godot's do") {
    TestContext context;
    const std::vector<Frame> steady(2048, Frame{0.5F, 0.25F});

    CHECK(run(*effect(context, "amplify", R"({"volume_db": 6.0206})"), steady)[100].left ==
          doctest::Approx(1.0F).epsilon(0.001));
    // Panned right, the left channel moves over to the right.
    const Frame panned = run(*effect(context, "panner", R"({"pan": 1})"), steady)[100];
    check_frame(panned, 0.0F, 0.75F);

    // A hard limiter keeps the sound under its ceiling.
    const std::vector<Frame> loud(8192, Frame{0.9F, 0.9F});
    const std::vector<Frame> limited =
        run(*effect(context, "hard_limiter", R"({"ceiling_db": -6.0206})"), loud);
    CHECK(peak(std::span(limited).subspan(4096)) <= 0.501F);

    // Filters: a low pass keeps a low tone and cuts a high one, a high pass the other way round.
    const std::vector<Frame> low = sine(100.0, 0.5F, 8192);
    const std::vector<Frame> high = sine(8000.0, 0.5F, 8192);
    const auto tail = [](const std::vector<Frame>& frames) {
        return rms(std::span(frames).subspan(4096));
    };
    const std::string_view lowpass = R"({"cutoff_hz": 500, "db": 24})";
    CHECK(tail(run(*effect(context, "low_pass_filter", lowpass), low)) > 0.25F);
    CHECK(tail(run(*effect(context, "low_pass_filter", lowpass), high)) < 0.01F);
    const std::string_view highpass = R"({"cutoff_hz": 2000, "db": 24})";
    CHECK(tail(run(*effect(context, "high_pass_filter", highpass), low)) < 0.01F);
    CHECK(tail(run(*effect(context, "high_pass_filter", highpass), high)) > 0.25F);

    // A delay's first echo comes tap1_delay_ms later.
    std::vector<Frame> click(kRate / 2);
    click[0] = {1.0F, 1.0F};
    const std::vector<Frame> echoed =
        run(*effect(context, "delay", R"({"dry": 0, "tap1_delay_ms": 100, "tap2_active": false})"),
            click);
    CHECK(peak(std::span(echoed).subspan(0, 4000)) == 0.0F);
    CHECK(peak(std::span(echoed).subspan(4400, 20)) > 0.1F);

    // A compressor turns down what is over its threshold.
    const std::vector<Frame> squeezed =
        run(*effect(context, "compressor", R"({"threshold": -20, "ratio": 10})"), loud);
    CHECK(peak(std::span(squeezed).subspan(4096)) < 0.5F);
}

// The spectrum analyzer's measure of one frequency, give or take a bin.
float magnitude_at(const audio::SpectrumAnalyzerInstance& analyzer, float hz) {
    return analyzer.magnitude_for_frequency_range(hz - 20.0F, hz + 20.0F).left;
}

TEST_CASE("a spectrum analyzer measures frequencies as Godot's does") {
    TestContext context;
    auto made = effect(context, "spectrum_analyzer");
    auto& analyzer = static_cast<audio::SpectrumAnalyzerInstance&>(*made);
    // Nothing analyzed yet.
    CHECK(analyzer.magnitude_for_frequency_range(0, 22050).left == 0.0F);

    // It passes the sound through, and a full-scale sine measures about 0.5 at its frequency
    // (half of it lost to the Hann window), next to nothing an octave away.
    const std::vector<Frame> in = sine(1000.0, 1.0F, 8192);
    const std::vector<Frame> out = run(analyzer, in);
    CHECK(out[1000].left == in[1000].left);
    CHECK(magnitude_at(analyzer, 1000.0F) == doctest::Approx(0.5F).epsilon(0.05));
    CHECK(magnitude_at(analyzer, 2000.0F) < 0.01F);
    // The average of a wide range is far below its strongest bin, and ranges read either way.
    using Mode = audio::SpectrumAnalyzerInstance::MagnitudeMode;
    const Frame wide = analyzer.magnitude_for_frequency_range(500, 1500, Mode::Average);
    CHECK(wide.left < 0.05F);
    CHECK(wide.left > 0.0F);
    CHECK(analyzer.magnitude_for_frequency_range(1020, 980).left == magnitude_at(analyzer, 1000));
    // Out of range clamps to the bins there are.
    CHECK(analyzer.magnitude_for_frequency_range(-100, 0).left < 0.01F);
    CHECK(analyzer.magnitude_for_frequency_range(30000, 40000).left < 0.01F);

    // Only the newest window answers: after a pause in the sound, it hears silence.
    run(analyzer, std::vector<Frame>(4096));
    CHECK(magnitude_at(analyzer, 1000.0F) < 0.001F);
}

TEST_CASE("a pitch shift moves a sine's frequency and keeps its level") {
    TestContext context;
    const std::vector<Frame> in = sine(440.0, 0.5F, kRate);
    for (const auto& [params, from, to] :
         {std::tuple{R"({"pitch_scale": 2})", 440.0F, 880.0F},
          std::tuple{R"({"pitch_scale": 0.5, "fft_size": 4096, "oversampling": 8})", 440.0F,
                     220.0F}}) {
        CAPTURE(params);
        const std::vector<Frame> shifted = run(*effect(context, "pitch_shift", params), in);
        auto analyzer = effect(context, "spectrum_analyzer", R"({"fft_size": 2048})");
        run(*analyzer, shifted);
        const auto& spectrum = static_cast<const audio::SpectrumAnalyzerInstance&>(*analyzer);
        CHECK(magnitude_at(spectrum, to) > 0.15F);
        CHECK(magnitude_at(spectrum, from) < magnitude_at(spectrum, to) / 10.0F);
        // About as loud as it went in, once the window's latency has passed.
        CHECK(rms(std::span(shifted).subspan(kRate / 2)) == doctest::Approx(rms(in)).epsilon(0.15));
    }
    // At a pitch scale of 1 the sound passes through untouched, as Godot skips it.
    CHECK(run(*effect(context, "pitch_shift"), in) == in);
}

TEST_CASE("a capture holds what passes, one mix step at a time, for game code to take") {
    TestContext context;
    auto made = effect(context, "capture");
    auto& capture = static_cast<audio::CaptureInstance&>(*made);
    CHECK(capture.process_silence());
    // 0.1 s at 44100 Hz is 4410 frames, rounded up to 8192; a ring of 8192 holds 8191.
    CHECK(capture.buffer_length_frames() == 8192);
    const std::vector<Frame> in = sine(440.0, 0.5F, 20 * audio::Server::kMixStep);
    CHECK(run(capture, in) == in);
    // A step that doesn't fit whole is discarded: 15 steps fit.
    CHECK(capture.frames_available() == 15 * audio::Server::kMixStep);
    CHECK(capture.pushed_frames() == 15 * audio::Server::kMixStep);
    CHECK(capture.discarded_frames() == 5 * audio::Server::kMixStep);

    CHECK(capture.can_get_buffer(100));
    const std::vector<Frame> taken = capture.get_buffer(100);
    REQUIRE(taken.size() == 100);
    CHECK(taken[99] == in[99]);
    CHECK(capture.frames_available() == 15 * audio::Server::kMixStep - 100);
    CHECK(capture.get_buffer(0).empty());
    CHECK(capture.get_buffer(8192).empty()); // past the ring, as Godot refuses
    CHECK_FALSE(capture.can_get_buffer(8000));
    CHECK(capture.get_buffer(8000).empty()); // more than it holds takes nothing
    CHECK(capture.get_buffer(1)[0] == in[100]);
    capture.clear_buffer();
    CHECK(capture.frames_available() == 0);
    run(capture, std::vector<Frame>(audio::Server::kMixStep, Frame{0.25F, 0.5F}));
    CHECK(capture.get_buffer(audio::Server::kMixStep - 1).back() == Frame{0.25F, 0.5F});

    auto tiny = effect(context, "capture", R"({"buffer_length": 0.01})");
    CHECK(static_cast<audio::CaptureInstance&>(*tiny).buffer_length_frames() == 512);
}

TEST_CASE("a record effect keeps what passes while recording, to play back or write") {
    TestContext context;
    auto made = effect(context, "record");
    auto& record = static_cast<audio::RecordInstance&>(*made);
    CHECK(record.process_silence());
    CHECK(record.format() == audio::RecordInstance::Format::Bits16);
    const std::vector<Frame> steady(1024, Frame{0.3F, -0.6F});
    CHECK(run(record, steady) == steady);
    CHECK(record.frames().empty()); // not recording yet
    record.set_recording_active(true);
    CHECK(record.is_recording_active());
    run(record, steady);
    run(record, std::vector<Frame>(512));
    CHECK(record.frames().size() == 1536);
    record.set_recording_active(false);
    run(record, steady);
    CHECK(record.frames().size() == 1536);
    CHECK(record.frames()[0] == Frame{0.3F, -0.6F});

    // As a sound, at 16 bits, as Godot's get_recording makes an AudioStreamWAV.
    const audio::Sound sound = record.recording();
    CHECK(sound.mix_rate == kRate);
    CHECK(sound.channels == 2);
    CHECK(sound.frames == 1536);
    REQUIRE(sound.samples.size() == 3072);
    CHECK(sound.samples[0] == std::trunc(0.3F * 32768.0F) / 32768.0F);
    CHECK(sound.samples[1] == std::trunc(-0.6F * 32768.0F) / 32768.0F);
    // It plays back.
    audio::Server server(kRate);
    server.start(audio::sound_stream(std::make_shared<audio::Sound>(sound))->instantiate(kRate, 0),
                 "Master", {1.0F, 1.0F}, 0.0, 1.0F);
    check_frame(mix(server, 512).back(), 0.3F, -0.6F);

    // Starting again drops the last recording.
    record.set_recording_active(true);
    CHECK(record.frames().empty());
    run(record, steady);
    CHECK(record.frames().size() == 1024);

    auto eight = effect(context, "record", R"({"format": "8_bits"})");
    auto& coarse = static_cast<audio::RecordInstance&>(*eight);
    coarse.set_recording_active(true);
    run(coarse, steady);
    CHECK(coarse.recording().samples[0] == 38.0F / 128.0F); // trunc(0.3 * 128)
    // A .wav file of it, 8-bit samples unsigned around 128.
    const std::string wav = audio::encode_wav(coarse.frames(), kRate, 8);
    CHECK(wav.size() == 44 + 2048);
    CHECK(static_cast<unsigned char>(wav[34]) == 8); // bits per sample
    CHECK(static_cast<unsigned char>(wav[44]) == 128 + 38);
    auto decoded = audio::decode_sound(wav, "take.wav");
    REQUIRE(decoded.ok());
    CHECK(decoded.value().frames == 1024);
    CHECK(decoded.value().samples[1] == doctest::Approx(-0.6F).epsilon(0.02));
}

TEST_CASE("the server gives a slot's running effect by its type, as get_bus_effect_instance") {
    audio::Server server(kRate);
    auto layout = audio::parse_bus_layout(parse(R"({"format": 1, "buses": [
        {"name": "Master", "effects": [{"type": "spectrum_analyzer"}, {"type": "capture"},
                                       {"type": "record", "enabled": false}]}]})"));
    REQUIRE(layout.ok());
    server.set_layout(layout.value());
    CHECK(audio::spectrum_analyzer(server, 0, 0) != nullptr);
    CHECK(audio::capture_effect(server, 0, 1) != nullptr);
    CHECK(audio::record_effect(server, 0, 2) != nullptr);
    CHECK(audio::capture_effect(server, 0, 0) == nullptr); // another type
    CHECK(audio::spectrum_analyzer(server, 0, 3) == nullptr);
    CHECK(audio::spectrum_analyzer(server, 1, 0) == nullptr);
    CHECK(server.effect_instance(0, 0, "spectrum_analyzer") ==
          audio::spectrum_analyzer(server, 0, 0));

    // A capture keeps running while its bus is silent, as Godot's process_silence says; a
    // turned off record effect doesn't run at all.
    audio::record_effect(server, 0, 2)->set_recording_active(true);
    mix(server, 2048);
    CHECK(audio::capture_effect(server, 0, 1)->pushed_frames() == 2048);
    CHECK(audio::record_effect(server, 0, 2)->frames().empty());
    server.set_bus_effect_enabled(0, 2, true);
    server.start(steady_stream(kRate, {0.25F, 0.25F})->instantiate(kRate, 0), "Master",
                 {1.0F, 1.0F}, 0.0, 1.0F);
    mix(server, 1024);
    REQUIRE(audio::record_effect(server, 0, 2)->frames().size() == 1024);
    // The sound starts a lookahead and two frames of resampling history late.
    CHECK(audio::record_effect(server, 0, 2)->frames()[65] == Frame{});
    CHECK(audio::record_effect(server, 0, 2)->frames()[66].left == doctest::Approx(0.25F));
    // The spectrum analyzer answers once a window of 2048 frames has filled: the steady sound
    // is all in the first bin.
    CHECK(audio::spectrum_analyzer(server, 0, 0)->magnitude_for_frequency_range(0, 20).left ==
          0.0F);
    mix(server, 1024);
    CHECK(audio::spectrum_analyzer(server, 0, 0)->magnitude_for_frequency_range(0, 20).left > 0.2F);
}

TEST_CASE("effect settings are checked, naming the closest type") {
    const json::Value schema = audio::effect_schema();
    const auto errors = [&](std::string_view text) { return json::validate(parse(text), schema); };
    CHECK(errors(R"({"type": "reverb", "room_size": 0.3, "wet": 0.2})").empty());
    CHECK(errors(R"({"type": "eq6", "band_db": {"1000_hz": -3}})").empty());
    CHECK(errors(R"({"type": "chorus", "voice_count": 3, "voices": [{"rate_hz": 2}]})").empty());

    const auto typo = errors(R"({"type": "revreb"})");
    REQUIRE(typo.size() == 1);
    CHECK(typo.front().path == "/type");
    CHECK(contains(typo.front().message, "did you mean \"reverb\"?"));
    // The type a slot names says what else is wrong with it.
    const auto range = errors(R"({"type": "reverb", "room_size": 2})");
    REQUIRE(range.size() == 1);
    CHECK(range.front().path == "/room_size");
    const auto stray = errors(R"({"type": "amplify", "volume": 3})");
    REQUIRE(stray.size() == 1);
    CHECK(stray.front().path == "/volume");
    const auto missing = errors(R"({"room_size": 0.5})");
    REQUIRE(missing.size() == 1);
    CHECK(missing.front().message == "missing required property \"type\"");
    CHECK(errors(R"({"type": "distortion", "mode": "clipp"})").front().path == "/mode");
    CHECK(errors(R"({"type": "eq10", "band_db": {"1000_hz": 0, "999_hz": 1}})").front().path ==
          "/band_db/999_hz");
    CHECK(errors(R"({"type": "pitch_shift", "pitch_scale": 2, "fft_size": 512})").empty());
    CHECK(errors(R"({"type": "pitch_shift", "fft_size": 1000})").front().path == "/fft_size");
    CHECK(errors(R"({"type": "spectrum_analyzer", "fft_size": 3000})").front().path == "/fft_size");
    CHECK(errors(R"({"type": "record", "format": "8_bits"})").empty());
    CHECK(errors(R"({"type": "record", "format": "qoa"})").front().path == "/format");
    CHECK(errors(R"({"type": "capture", "buffer_length": 11})").front().path == "/buffer_length");
}

TEST_CASE(".bus_layout.json files list the buses, Master first") {
    auto parsed = audio::parse_bus_layout(parse(R"({"format": 1, "buses": [
        {"name": "Master", "volume_db": -3},
        {"name": "Music", "mute": true, "effects": [
            {"type": "reverb", "wet": 0.25}, {"type": "amplify", "enabled": false}]},
        {"name": "Sfx", "send": "Music", "solo": true, "bypass_effects": true}
    ]})"));
    REQUIRE(parsed.ok());
    const audio::BusLayout& layout = parsed.value();
    REQUIRE(layout.buses.size() == 3);
    CHECK(layout.buses[0].volume_db == -3.0F);
    CHECK(layout.buses[1].mute);
    CHECK(layout.buses[1].send == "Master");
    REQUIRE(layout.buses[1].effects.size() == 2);
    CHECK(layout.buses[1].effects[0].type == "reverb");
    CHECK(layout.buses[1].effects[0].params == parse(R"({"wet": 0.25})"));
    CHECK_FALSE(layout.buses[1].effects[1].enabled);
    CHECK(layout.buses[2].send == "Music");
    CHECK(layout.buses[2].solo);
    CHECK(layout.buses[2].bypass_effects);
    REQUIRE(audio::default_bus_layout().buses.size() == 1);
    CHECK(audio::default_bus_layout().buses[0].name == "Master");

    const auto first = [](std::string_view text) {
        const auto found = audio::validate_bus_layout(parse(text));
        REQUIRE_FALSE(found.empty());
        return found.front();
    };
    CHECK(first(R"({"format": 1, "buses": [{"name": "Music"}]})").message ==
          "the first bus is \"Master\"");
    CHECK(first(R"({"format": 1, "buses": [{"name": "Master"}, {"name": "A"}, {"name": "A"}]})")
              .path == "/buses/2/name");
    CHECK(first(R"({"format": 1, "buses": [{"name": "Master", "send": "Master"}]})").message ==
          "Master sends nowhere");
    const auto send = first(R"({"format": 1, "buses": [{"name": "Master"}, {"name": "Music"},
                                {"name": "Sfx", "send": "Musc"}]})");
    CHECK(send.path == "/buses/2/send");
    CHECK(send.message == "no bus before this one is named \"Musc\"; did you mean \"Music\"?");
    // A bus sends to one before it, so sound can't go round in circles.
    CHECK(first(R"({"format": 1, "buses": [{"name": "Master"}, {"name": "A", "send": "B"},
                    {"name": "B"}]})")
              .path == "/buses/1/send");
    CHECK(first(R"({"format": 1, "buses": [{"name": "Master", "effects": [
                    {"type": "compressor", "sidechain": "Voice"}]}]})")
              .path == "/buses/0/effects/0/sidechain");
    CHECK(first(R"({"format": 1, "buses": [{"name": "Master", "effects": [
                    {"type": "reverbb"}]}]})")
              .path == "/buses/0/effects/0/type");
    auto bad = audio::parse_bus_layout(parse(R"({"format": 1, "buses": [{"name": "Music"}]})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "/buses/0/name: the first bus is \"Master\"");
}

TEST_CASE("an effect on a bus runs on what was mixed into it, and bypass skips it") {
    audio::Server server(kRate);
    auto layout = audio::parse_bus_layout(parse(R"({"format": 1, "buses": [
        {"name": "Master", "effects": [{"type": "amplify", "volume_db": 6.0206}]}]})"));
    REQUIRE(layout.ok());
    server.set_layout(layout.value());
    server.start(steady_stream(kRate, {0.25F, 0.25F})->instantiate(kRate, 0), "Master",
                 {1.0F, 1.0F}, 0.0, 1.0F);
    check_frame(mix(server, 1024).back(), 0.5F, 0.5F);
    server.set_bus_effect_enabled(0, 0, false);
    CHECK_FALSE(server.layout().buses[0].effects[0].enabled);
    check_frame(mix(server, 512).back(), 0.25F, 0.25F);
    server.set_bus_effect_enabled(0, 0, true);
    server.set_bus_bypass_effects(0, true);
    check_frame(mix(server, 512).back(), 0.25F, 0.25F);
    server.set_bus_bypass_effects(0, false);
    check_frame(mix(server, 512).back(), 0.5F, 0.5F);
}

TEST_CASE("an AudioStreamPlayer with autoplay plays its sound, then says it finished") {
    Sounds sounds;
    sounds.files.write("sfx/beep.wav", steady_wav(kRate / 10, {0.5F, 0.5F}));
    const entt::entity beep =
        sounds.add("beep", "AudioStreamPlayer", R"({"stream": "sfx/beep.wav", "autoplay": true})");
    sounds.step();
    // Each tick mixes its share of the frames: 735 at 60 ticks a second.
    CHECK(sounds.heard.size() == 735);
    check_frame(sounds.last(), 0.5F, 0.5F);
    CHECK(sounds.world.ecs().get<world::AudioStreamPlayer>(beep).playing);
    const auto info = sounds.player("beep");
    REQUIRE(info.has_value());
    CHECK(info->kind == "AudioStreamPlayer");
    CHECK(info->stream == "sfx/beep.wav");
    CHECK(info->bus == "Master");
    CHECK(info->voices == 1);
    CHECK(info->length == doctest::Approx(0.1));
    check_frame(info->volume, 1.0F, 1.0F);
    CHECK(audio::playback_position(sounds.world, beep) > 0.0);

    sounds.step(10);
    CHECK(sounds.count("finished") == 1);
    const auto finished = std::ranges::find(sounds.events, "finished", &world::Event::kind);
    CHECK(finished->id == "beep");
    CHECK(finished->data == parse(R"({"component": "AudioStreamPlayer"})"));
    CHECK_FALSE(sounds.world.ecs().get<world::AudioStreamPlayer>(beep).playing);
    CHECK(sounds.player("beep")->voices == 0);
    check_frame(sounds.last(), 0.0F, 0.0F);
    // Ticks at 1/60 s carry the leftover fraction of a frame, so a second is 44100 frames.
    sounds.step(49);
    CHECK(sounds.heard.size() == static_cast<std::size_t>(kRate));
}

TEST_CASE("playing, stream, play() and stop() work as Godot's AudioStreamPlayer") {
    Sounds sounds;
    sounds.files.write("loud.wav", steady_wav(kRate, {0.5F, 0.5F}));
    sounds.files.write("quiet.wav", steady_wav(kRate, {0.125F, 0.125F}));
    const entt::entity music =
        sounds.add("music", "AudioStreamPlayer", R"({"stream": "loud.wav", "volume_db": -6.0206})");
    auto& player = sounds.world.ecs().get<world::AudioStreamPlayer>(music);
    sounds.step();
    CHECK(sounds.player("music")->voices == 0);

    player.playing = true;
    sounds.step(2);
    CHECK(sounds.player("music")->voices == 1);
    check_frame(sounds.last(), 0.25F, 0.25F);
    player.playing = false;
    sounds.step();
    CHECK(sounds.player("music")->voices == 0);

    // play() restarts with max_polyphony 1, and adds voices up to more.
    audio::play(sounds.world, music);
    audio::play(sounds.world, music);
    sounds.step();
    CHECK(sounds.player("music")->voices == 1);
    CHECK(player.playing);
    player.max_polyphony = 3;
    for (int i = 0; i < 4; ++i) {
        audio::play(sounds.world, music, 0.5);
    }
    sounds.step();
    CHECK(sounds.player("music")->voices == 3);
    audio::stop(sounds.world, music);
    sounds.step();
    CHECK(sounds.player("music")->voices == 0);
    CHECK_FALSE(player.playing);

    // A new stream stops the old one and plays if playing is set.
    player.max_polyphony = 1;
    player.playing = true;
    sounds.step(2);
    player.stream = "quiet.wav";
    sounds.step(2);
    CHECK(sounds.player("music")->stream == "quiet.wav");
    CHECK(sounds.player("music")->voices == 1);
    check_frame(sounds.last(), 0.0625F, 0.0625F);

    // stream_paused holds the voices where they are.
    player.stream_paused = true;
    sounds.step(2);
    const double held = audio::playback_position(sounds.world, music);
    sounds.step(3);
    CHECK(audio::playback_position(sounds.world, music) == held);
    CHECK(sounds.player("music")->paused);
    check_frame(sounds.last(), 0.0F, 0.0F);
    player.stream_paused = false;
    sounds.step(2);
    CHECK(audio::playback_position(sounds.world, music) > held);

    // Volume, pitch and bus changes reach the voices.
    player.volume_db = 0.0F;
    sounds.step(2);
    check_frame(sounds.last(), 0.125F, 0.125F);

    // A stream that won't load says why, and plays nothing.
    player.stream = "missing.wav";
    sounds.step();
    CHECK(sounds.player("music")->voices == 0);
    CHECK_FALSE(sounds.player("music")->error.empty());

    // Removing the entity stops it.
    player.stream = "loud.wav";
    player.playing = true;
    sounds.step();
    CHECK(sounds.player("music")->voices == 1);
    sounds.world.destroy(music);
    sounds.step(2);
    CHECK_FALSE(sounds.player("music").has_value());
    check_frame(sounds.last(), 0.0F, 0.0F);
}

TEST_CASE("an AudioStreamPlayer2D pans and fades with its distance from the listener") {
    Sounds sounds;
    sounds.files.write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    const float screen = slash::ui::settings(sounds.world).viewport.x;
    const entt::entity listener = sounds.add("ears", "AudioListener2D", R"({"current": true})");
    sounds.world.ecs().emplace<world::Transform2D>(listener);
    const entt::entity hum =
        sounds.add("hum", "AudioStreamPlayer2D", R"({"stream": "hum.wav", "autoplay": true})");
    sounds.world.ecs().emplace<world::Transform2D>(hum).position = {-400.0F, 0.0F};
    sounds.step();
    // Godot's _update_panning: fades over max_distance and pans by where it is on the screen,
    // at half the project's panning strength.
    const float fade = 1.0F - (400.0F / 2000.0F);
    const float pan = 0.5F + (-400.0F / screen * 0.5F * 0.5F);
    Frame volume = sounds.player("hum")->volume;
    check_frame(volume, (1.0F - pan) * fade, pan * fade);
    CHECK(volume.left > volume.right);
    sounds.step();
    check_frame(sounds.last(), 0.5F * (1.0F - pan) * fade, 0.5F * pan * fade);

    // Turned round, the listener hears it on the right.
    sounds.world.ecs().get<world::Transform2D>(listener).rotation = std::numbers::pi_v<float>;
    sounds.step();
    volume = sounds.player("hum")->volume;
    CHECK(volume.right > volume.left);
    sounds.world.ecs().get<world::Transform2D>(listener).rotation = 0.0F;

    // Beyond max_distance it can't be heard.
    sounds.world.ecs().get<world::Transform2D>(hum).position = {2500.0F, 0.0F};
    sounds.step();
    check_frame(sounds.player("hum")->volume, 0.0F, 0.0F);

    // With no current listener, it is heard from the middle of the screen.
    sounds.world.ecs().get<world::AudioListener2D>(listener).current = false;
    const slash::Vec2 viewport = slash::ui::settings(sounds.world).viewport;
    sounds.world.ecs().get<world::Transform2D>(hum).position = {viewport.x / 2.0F,
                                                                viewport.y / 2.0F};
    sounds.step();
    check_frame(sounds.player("hum")->volume, 0.5F, 0.5F);
}

TEST_CASE("an Area2D with audio_bus_override sends the 2D sounds inside it to its bus") {
    audio::Settings settings;
    settings.bus_layout = "buses.bus_layout.json";
    Sounds sounds(settings);
    slash::physics2d::install(sounds.world);
    sounds.files.write("buses.bus_layout.json", R"({"format": 1, "buses": [{"name": "Master"},
        {"name": "Sfx"}, {"name": "Cave", "volume_db": -6.0206}, {"name": "Lake"}]})");
    sounds.files.write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    const auto area = [&](std::string_view id, std::string_view fields, slash::Vec2 at) {
        const entt::entity entity = sounds.add(id, "Area2D", fields);
        sounds.world.ecs().emplace<world::Transform2D>(entity).position = at;
        sounds.world.ecs().emplace<world::CollisionShape2D>(entity).size = {100.0F, 100.0F};
        return entity;
    };
    area("cave", R"({"audio_bus_override": true, "audio_bus_name": "Cave"})", {500.0F, 0.0F});
    // Overlapping the cave's right half: the first by ID wins.
    area("lake", R"({"audio_bus_override": true, "audio_bus_name": "Lake"})", {550.0F, 0.0F});
    // On layer 2, which players' default area_mask leaves out.
    area("wind", R"({"audio_bus_override": true, "audio_bus_name": "Lake", "collision_layer": 2})",
         {-500.0F, 0.0F});
    // An area that doesn't override changes nothing.
    area("field", "{}", {0.0F, 300.0F});
    const entt::entity hum = sounds.add(
        "hum", "AudioStreamPlayer2D",
        R"({"stream": "hum.wav", "autoplay": true, "bus": "Sfx", "max_distance": 100000})");
    auto& at = sounds.world.ecs().emplace<world::Transform2D>(hum).position;
    const auto bus_at = [&](slash::Vec2 where) {
        at = where;
        sounds.step();
        return sounds.player("hum")->bus;
    };
    CHECK(bus_at({0.0F, 0.0F}) == "Sfx");
    CHECK(bus_at({480.0F, 0.0F}) == "Cave");
    // The cave's volume, while the bus it left fades out.
    sounds.step();
    CHECK(audio::server(sounds.world)->bus_peak_db(2).left ==
          doctest::Approx(audio::server(sounds.world)->bus_peak_db(0).left));
    CHECK(audio::server(sounds.world)->bus_peak_db(1).left < -100.0F);
    CHECK(bus_at({540.0F, 0.0F}) == "Cave");
    CHECK(bus_at({590.0F, 0.0F}) == "Lake");
    CHECK(bus_at({0.0F, 300.0F}) == "Sfx");
    CHECK(bus_at({-500.0F, 0.0F}) == "Sfx");
    sounds.world.ecs().get<world::AudioStreamPlayer2D>(hum).area_mask = 3;
    CHECK(bus_at({-500.0F, 0.0F}) == "Lake");
    // area_mask 0 ignores areas, as Godot's.
    sounds.world.ecs().get<world::AudioStreamPlayer2D>(hum).area_mask = 0;
    CHECK(bus_at({480.0F, 0.0F}) == "Sfx");
    // A sound that starts inside starts on the area's bus.
    sounds.world.ecs().get<world::AudioStreamPlayer2D>(hum).area_mask = 1;
    audio::play(sounds.world, hum);
    CHECK(bus_at({480.0F, 0.0F}) == "Cave");
    CHECK(sounds.player("hum")->voices == 1);
}

TEST_CASE("an AudioStreamPlayer2D inside another entity is heard from where it is") {
    Sounds sounds;
    sounds.files.write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    const entt::entity listener = sounds.add("ears", "AudioListener2D", R"({"current": true})");
    sounds.world.ecs().emplace<world::Transform2D>(listener);
    // A cart 200 pixels left, doubled, carries the hum 100 of its pixels further left.
    const entt::entity cart = sounds.world.create("cart").value();
    auto& carried = sounds.world.ecs().emplace<world::Transform2D>(cart);
    carried.position = {-200.0F, 0.0F};
    carried.scale = {2.0F, 2.0F};
    const entt::entity hum =
        sounds.add("hum", "AudioStreamPlayer2D", R"({"stream": "hum.wav", "autoplay": true})");
    sounds.world.ecs().emplace<world::Transform2D>(hum).position = {-100.0F, 0.0F};
    carried.children = {hum};
    sounds.step();
    // As heard 400 pixels left.
    const float screen = slash::ui::settings(sounds.world).viewport.x;
    const float fade = 1.0F - (400.0F / 2000.0F);
    const float pan = 0.5F + (-400.0F / screen * 0.5F * 0.5F);
    check_frame(sounds.player("hum")->volume, (1.0F - pan) * fade, pan * fade);
}

TEST_CASE("without a listener, 2D sounds are heard from the middle of the camera's view") {
    Sounds sounds;
    sounds.files.write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    const float screen = slash::ui::settings(sounds.world).viewport.x;
    // A camera far from the origin, zoomed in twice.
    const entt::entity camera = sounds.world.create("cam").value();
    sounds.world.ecs().emplace<world::Transform2D>(camera).position = {5000.0F, 3000.0F};
    sounds.world.ecs().emplace<world::Camera2D>(camera).zoom = {2.0F, 2.0F};
    const entt::entity hum =
        sounds.add("hum", "AudioStreamPlayer2D", R"({"stream": "hum.wav", "autoplay": true})");
    sounds.world.ecs().emplace<world::Transform2D>(hum).position = {5100.0F, 3000.0F};
    sounds.step();
    // 100 pixels from the middle of the view, which shows it 200 pixels right of the middle of
    // the screen, as Godot pans by where a sound is on the screen.
    const float fade = 1.0F - (100.0F / 2000.0F);
    const float pan = 0.5F + (200.0F / screen * 0.5F * 0.5F);
    check_frame(sounds.player("hum")->volume, (1.0F - pan) * fade, pan * fade);

    // A listener's distances are scaled with the view too.
    const entt::entity listener = sounds.add("ears", "AudioListener2D", R"({"current": true})");
    sounds.world.ecs().emplace<world::Transform2D>(listener).position = {5000.0F, 3000.0F};
    sounds.step();
    check_frame(sounds.player("hum")->volume, (1.0F - pan) * fade, pan * fade);
}

TEST_CASE("buses come from the project's .bus_layout.json, reloaded when it changes") {
    MemoryFiles* files = nullptr;
    audio::Settings settings;
    settings.bus_layout = "audio/default.bus_layout.json";
    Sounds sounds(settings);
    files = &sounds.files;
    files->write("audio/default.bus_layout.json", R"({"format": 1, "buses": [{"name": "Master"},
        {"name": "Music", "volume_db": -6.0206}]})");
    files->write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    sounds.add("hum", "AudioStreamPlayer",
               R"({"stream": "hum.wav", "autoplay": true, "bus": "Music"})");
    sounds.step(2);
    REQUIRE(audio::server(sounds.world)->layout().buses.size() == 2);
    CHECK(sounds.player("hum")->bus == "Music");
    check_frame(sounds.last(), 0.25F, 0.25F);

    files->write("audio/default.bus_layout.json", R"({"format": 1, "buses": [{"name": "Master"},
        {"name": "Music", "volume_db": 0}]})");
    sounds.assets.poll();
    sounds.step(2);
    check_frame(sounds.last(), 0.5F, 0.5F);

    // A layout that doesn't load leaves the last one playing.
    files->write("audio/default.bus_layout.json", R"({"format": 1, "buses": [{"name": "Music"}]})");
    sounds.assets.poll();
    sounds.step(2);
    CHECK(audio::server(sounds.world)->layout().buses.size() == 2);

    // New settings start the buses over; with no layout, everything plays through Master.
    audio::Settings plain;
    audio::set_settings(sounds.world, plain);
    CHECK(audio::settings(sounds.world).bus_layout.empty());
    CHECK(audio::server(sounds.world)->layout().buses.size() == 1);
    sounds.step(2);
    CHECK(sounds.player("hum")->bus == "Master");
}

TEST_CASE("capture measures what a bus played over the last seconds") {
    audio::Settings settings;
    settings.history_seconds = 1.0;
    Sounds sounds(settings);
    auto silent = audio::capture(sounds.world, "Master", 1.0, 4);
    REQUIRE(silent.ok());
    CHECK(silent.value().seconds == 0.0);
    CHECK(silent.value().peak_db == -200.0F);

    sounds.files.write("hum.wav", steady_wav(kRate, {0.5F, 0.5F}));
    sounds.add("hum", "AudioStreamPlayer", R"({"stream": "hum.wav", "autoplay": true})");
    sounds.step(30);
    auto captured = audio::capture(sounds.world, "Master", 1.0, 4, true);
    REQUIRE(captured.ok());
    // Buses are kept a mix step of 512 frames at a time.
    CHECK(captured.value().seconds == doctest::Approx(0.5).epsilon(0.01));
    CHECK(captured.value().peak_db == doctest::Approx(half_db()).epsilon(0.001));
    CHECK(captured.value().rms_db == doctest::Approx(half_db()).epsilon(0.01));
    REQUIRE(captured.value().slice_peaks_db.size() == 4);
    CHECK(captured.value().slice_peaks_db[3] == doctest::Approx(half_db()).epsilon(0.001));
    CHECK(captured.value().frames.size() ==
          static_cast<std::size_t>(std::llround(captured.value().seconds * kRate)));

    auto recent = audio::capture(sounds.world, "Master", 0.1, 0);
    REQUIRE(recent.ok());
    CHECK(recent.value().seconds == doctest::Approx(0.1).epsilon(0.001));
    CHECK(recent.value().slice_peaks_db.empty());
    CHECK(recent.value().frames.empty());

    const auto unknown = audio::capture(sounds.world, "Mastre", 1.0, 1);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().message == "no bus is named \"Mastre\"; did you mean \"Master\"?");

    // A WAV file of what was captured decodes to the same frames.
    const std::string wav = audio::encode_wav(captured.value().frames, kRate);
    auto decoded = audio::decode_sound(wav, "capture.wav");
    REQUIRE(decoded.ok());
    CHECK(decoded.value().frames == static_cast<std::int64_t>(captured.value().frames.size()));
}

TEST_CASE("project.json's audio settings are checked") {
    const auto errors = [](std::string_view text) {
        return audio::validate_settings(parse(text), "/audio");
    };
    CHECK(errors(R"({"bus_layout": "audio/main.bus_layout.json", "mix_rate": 48000,
                     "output_latency": 30, "panning_strength_2d": 1})")
              .empty());
    CHECK(errors(R"({"mix_rate": 1000})").front().path == "/audio/mix_rate");
    CHECK(errors(R"({"mixrate": 48000})").front().path == "/audio/mixrate");
    CHECK(errors(R"({"bus_layout": "buses.json"})").front().message ==
          "expected a .bus_layout.json file");
    CHECK(errors(R"({"bus_layout": "../x.bus_layout.json"})").front().path == "/audio/bus_layout");

    const audio::Settings parsed = audio::parse_settings(
        parse(R"({"bus_layout": "a.bus_layout.json", "mix_rate": 48000, "output_latency": 30,
                  "panning_strength_2d": 1, "enable_input": true,
                  "input_usage_description": "Sing to the lanterns."})"));
    CHECK(parsed.bus_layout == "a.bus_layout.json");
    CHECK(parsed.mix_rate == 48000);
    CHECK(parsed.output_latency == 30.0F);
    CHECK(parsed.panning_strength_2d == 1.0F);
    CHECK(parsed.enable_input);
    CHECK(parsed.input_usage_description == "Sing to the lanterns.");
    CHECK(audio::parse_settings(parse("{}")) == audio::Settings{});
}
