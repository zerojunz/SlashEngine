#include "slash/asset/assets.hpp"
#include "slash/audio/audio.hpp"
#include "slash/audio/effects.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
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

std::shared_ptr<const audio::Stream> steady_stream(int frames, Frame value) {
    auto sound = audio::decode_sound(steady_wav(frames, value), "steady.wav");
    REQUIRE(sound.ok());
    return audio::sound_stream(std::make_shared<audio::Sound>(std::move(sound).value()));
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
          "needs \"source\" (a sound with settings) or \"streams\" (a randomizer)");
    CHECK(first(R"({"format": 1, "source": "a.wav", "streams": [{"stream": "b.wav"}]})").message ==
          "has both \"source\" and \"streams\"; a file is one sound or a randomizer");
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
    CHECK(types.size() == 20);
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
                  "panning_strength_2d": 1})"));
    CHECK(parsed.bus_layout == "a.bus_layout.json");
    CHECK(parsed.mix_rate == 48000);
    CHECK(parsed.output_latency == 30.0F);
    CHECK(parsed.panning_strength_2d == 1.0F);
    CHECK(audio::parse_settings(parse("{}")) == audio::Settings{});
}
