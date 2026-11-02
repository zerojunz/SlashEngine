#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/bits.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace slash;

namespace {

using Bytes = std::vector<std::uint8_t>;

render::Image flat(std::array<std::uint8_t, 3> color) {
    render::Image image;
    image.width = 32;
    image.height = 16;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = color[0];
        image.pixels[i + 1] = color[1];
        image.pixels[i + 2] = color[2];
        image.pixels[i + 3] = 255;
    }
    return image;
}

// A square wave of amplitude on the left and a ramp on the right, frames long.
video::PcmAudio stereo_tone(int frames, std::int16_t amplitude) {
    video::PcmAudio audio;
    for (int i = 0; i < frames; ++i) {
        audio.samples.push_back(i % 96 < 48 ? amplitude : static_cast<std::int16_t>(-amplitude));
        audio.samples.push_back(static_cast<std::int16_t>(((i % 100) * 100) - 5000));
    }
    return audio;
}

// Matroska elements written by hand, each size in eight bytes.
Bytes element(std::uint32_t id, const Bytes& body) {
    Bytes out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if ((id >> static_cast<unsigned>(shift)) != 0) {
            out.push_back(static_cast<std::uint8_t>((id >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }
    out.push_back(0x01);
    for (int shift = 48; shift >= 0; shift -= 8) {
        out.push_back(
            static_cast<std::uint8_t>((body.size() >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// One whose size is unknown, which runs until an element that can't be inside it.
Bytes unknown_size(std::uint32_t id, const Bytes& body) {
    Bytes out = element(id, {});
    std::fill(out.end() - 7, out.end(), 0xFF);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes number(std::uint32_t id, std::uint64_t value) {
    Bytes body;
    for (int shift = 56; shift >= 0; shift -= 8) {
        body.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    return element(id, body);
}

Bytes text(std::uint32_t id, std::string_view value) {
    return element(id, Bytes(value.begin(), value.end()));
}

Bytes real(std::uint32_t id, double value) {
    const auto bits = slash::bit_cast<std::uint64_t>(value);
    Bytes body;
    for (int shift = 56; shift >= 0; shift -= 8) {
        body.push_back(static_cast<std::uint8_t>((bits >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    return element(id, body);
}

Bytes join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// 16-bit little-endian samples.
Bytes samples(std::initializer_list<int> values) {
    Bytes out;
    for (const int value : values) {
        const auto bits = static_cast<std::uint16_t>(static_cast<std::int16_t>(value));
        out.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(bits >> 8U));
    }
    return out;
}

// A block of a track at a time relative to its cluster: flags, then lacing, then data.
Bytes block(std::uint32_t id, std::uint8_t track, std::uint8_t flags, const Bytes& lacing,
            const Bytes& data) {
    Bytes body{static_cast<std::uint8_t>(0x80U | track), 0x00, 0x00, flags};
    body.insert(body.end(), lacing.begin(), lacing.end());
    body.insert(body.end(), data.begin(), data.end());
    return element(id, body);
}

Bytes header(std::string_view doc_type) {
    return element(0x1A45DFA3, text(0x4282, doc_type));
}

Bytes video_track(std::string_view codec, const Bytes& extra = {}) {
    return element(0xAE, join({number(0xD7, 1), number(0x83, 1), text(0x86, codec), extra,
                               element(0xE0, join({number(0xB0, 32), number(0xBA, 16)}))}));
}

Bytes audio_track(std::uint64_t track, std::string_view codec, int channels,
                  const Bytes& extra = {}) {
    return element(0xAE,
                   join({number(0xD7, track), number(0x83, 2), text(0x86, codec), extra,
                         element(0xE1, join({real(0xB5, 8000.0),
                                             number(0x9F, static_cast<std::uint64_t>(channels)),
                                             number(0x6264, 16)}))}));
}

Bytes key_frame(std::array<std::uint8_t, 3> color) {
    auto unit = video::encode_frame(video::to_yuv420(flat(color)));
    REQUIRE(unit);
    return unit.value();
}

Bytes file_of(const Bytes& tracks, const Bytes& clusters) {
    return join(
        {header("webm"), element(0x18538067, join({element(0x1549A966, number(0x2AD7B1, 1000000)),
                                                   element(0x1654AE6B, tracks), clusters}))});
}

std::string error_of(const Bytes& bytes) {
    auto read = video::read_matroska(bytes);
    return read ? std::string() : read.error().message;
}

} // namespace

TEST_CASE("video: Matroska files keep AV1 frames and PCM audio") {
    // Red, green and blue at ten frames a second, with 0.3 seconds of sound.
    const std::vector<render::Image> frames{flat({255, 0, 0}), flat({0, 255, 0}),
                                            flat({0, 0, 255})};
    const video::PcmAudio audio = stereo_tone(14400, 8000);
    const auto bytes = video::encode_matroska(frames, 10, 1, &audio);
    REQUIRE(bytes);
    auto read = video::read_matroska(bytes.value());
    REQUIRE_MESSAGE(read, (read ? std::string() : read.error().message));
    const video::VideoFile& file = read.value();
    CHECK(file.width == 32);
    CHECK(file.height == 16);
    REQUIRE(file.frames.size() == 3);
    CHECK(file.frames[1].key);
    CHECK(file.frame_time(1) == doctest::Approx(0.1));
    CHECK(file.length() == doctest::Approx(0.3));
    // av1C's sequence header, which decoding starts with.
    CHECK_FALSE(file.config.empty());
    REQUIRE(file.audio.size() == 1);
    const video::AudioTrack& track = file.audio[0];
    CHECK(track.codec == video::AudioTrack::Codec::Pcm);
    CHECK(track.codec_id == "A_PCM/INT/LIT");
    CHECK(track.sample_rate == 48000);
    CHECK(track.channels == 2);
    CHECK(track.bits == 16);
    CHECK(track.packets.size() == 15); // blocks of 20 milliseconds

    auto decoded = video::decode_audio(file, 0);
    REQUIRE(decoded);
    CHECK(decoded.value().sample_rate == 48000);
    CHECK(decoded.value().channels == 2);
    REQUIRE(decoded.value().samples.size() == audio.samples.size());
    for (const std::size_t i :
         {std::size_t{0}, std::size_t{1}, std::size_t{97}, std::size_t{28799}}) {
        CHECK(decoded.value().samples[i] == static_cast<float>(audio.samples[i]) / 32768.0F);
    }
    CHECK(video::decode_audio(file, 1).error().message == "the video has 1 audio tracks, not 2");

    // The frames decode as an IVF file's do.
    video::Decoder decoder;
    render::Image picture;
    REQUIRE(decoder.decode(file, 2, picture));
    CHECK(picture.pixels[0] < 8);
    CHECK(picture.pixels[2] > 240);

    // Without audio, there is no track.
    const auto silent = video::encode_matroska(frames, 10);
    REQUIRE(silent);
    CHECK(video::read_matroska(silent.value()).value().audio.empty());
}

TEST_CASE("video: Matroska's laced blocks, clusters of unknown size and what can't be read") {
    // Xiph lacing (sizes 4 and 2, the rest 6), EBML lacing (4, then 2 as a difference of -2,
    // the rest 2) and fixed lacing (two of 2), in a cluster of unknown size and one after it.
    const Bytes tracks = join({video_track("V_AV1"), audio_track(2, "A_PCM/INT/LIT", 1)});
    const Bytes clusters = join(
        {unknown_size(0x1F43B675,
                      join({number(0xE7, 0), block(0xA3, 1, 0x80, {}, key_frame({255, 0, 0})),
                            block(0xA3, 2, 0x82, {0x02, 4, 2}, samples({1, 2, 3, 4, 5, 6}))})),
         element(0x1F43B675, join({number(0xE7, 100),
                                   element(0xA0, block(0xA1, 1, 0x00, {}, key_frame({0, 0, 255}))),
                                   block(0xA3, 2, 0x86, {0x02, 0x84, 0xBD}, samples({7, 8, 9, 10})),
                                   block(0xA3, 2, 0x84, {0x01}, samples({11, 12}))}))});
    auto read = video::read_matroska(file_of(tracks, clusters));
    REQUIRE_MESSAGE(read, (read ? std::string() : read.error().message));
    REQUIRE(read.value().frames.size() == 2);
    CHECK(read.value().frame_time(1) == doctest::Approx(0.1));
    CHECK(read.value().config.empty()); // the frames carry their own sequence headers
    auto decoded = video::decode_audio(read.value(), 0);
    REQUIRE(decoded);
    CHECK(decoded.value().channels == 1);
    REQUIRE(decoded.value().samples.size() == 12);
    for (std::size_t i = 0; i < 12; ++i) {
        CHECK(decoded.value().samples[i] == static_cast<float>(i + 1) / 32768.0F);
    }

    // Six channels fold to stereo: the center and the rear at -3 dB, the LFE left out.
    const Bytes surround = file_of(
        join({video_track("V_AV1"), audio_track(2, "A_PCM/INT/LIT", 6)}),
        element(0x1F43B675,
                join({number(0xE7, 0), block(0xA3, 1, 0x80, {}, key_frame({0, 0, 0})),
                      block(0xA3, 2, 0x80, {}, samples({1000, 2000, 3000, 9999, 4000, 5000}))})));
    decoded = video::decode_audio(video::read_matroska(surround).value(), 0);
    REQUIRE(decoded);
    REQUIRE(decoded.value().samples.size() == 2);
    CHECK(decoded.value().samples[0] * 32768.0F == doctest::Approx(1000 + (0.70710678 * 7000)));
    CHECK(decoded.value().samples[1] * 32768.0F == doctest::Approx(2000 + (0.70710678 * 8000)));

    // Opus is read, and waits for its decoder.
    Bytes opus_head{'O',  'p',  'u',  's',  'H', 'e', 'a', 'd', 1, 2,
                    0x38, 0x01, 0x80, 0xBB, 0,   0,   0,   0,   0};
    const Bytes with_opus = file_of(
        join({video_track("V_AV1"), audio_track(2, "A_OPUS", 2, element(0x63A2, opus_head))}),
        element(0x1F43B675, join({number(0xE7, 0), block(0xA3, 1, 0x80, {}, key_frame({0, 0, 0})),
                                  block(0xA3, 2, 0x80, {}, {0xFC, 0xFF, 0xFE})})));
    read = video::read_matroska(with_opus);
    REQUIRE(read);
    REQUIRE(read.value().audio.size() == 1);
    CHECK(read.value().audio[0].codec == video::AudioTrack::Codec::Opus);
    CHECK(read.value().audio[0].pre_skip == 312);
    CHECK(read.value().audio[0].sample_rate == 48000);
    CHECK(video::decode_audio(read.value(), 0).error().message ==
          "the video's audio is A_OPUS, which can't be played yet; videos play PCM audio "
          "(A_PCM/INT/LIT or A_PCM/FLOAT/IEEE) in .mkv files");

    // What can't be read says why.
    const Bytes cluster = element(
        0x1F43B675, join({number(0xE7, 0), block(0xA3, 1, 0x80, {}, key_frame({0, 0, 0}))}));
    CHECK(error_of({'D', 'K', 'I', 'F'}) ==
          "not a WebM or Matroska file: it doesn't start with an EBML header");
    CHECK(error_of(join({header("ogg"), element(0x18538067, {})})) ==
          "the EBML file is \"ogg\", not WebM or Matroska");
    CHECK(error_of(file_of(video_track("V_VP9"), cluster)) ==
          "the file's video is V_VP9, not AV1 (V_AV1)");
    CHECK(error_of(file_of(video_track("V_AV1", element(0x6D80, {})), cluster)) ==
          "the file's video track is compressed or encrypted");
    CHECK(error_of(file_of(audio_track(1, "A_PCM/INT/LIT", 1), {})) ==
          "the file has no video track");
    Bytes cut = file_of(video_track("V_AV1"), cluster);
    cut.resize(cut.size() - 10);
    CHECK(error_of(cut).starts_with("the file is cut short at byte "));
}

namespace {

json::Value parse_json(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct SoundEngine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit SoundEngine(const std::string& root) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    [[nodiscard]] json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse_json(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    // The video player's sound as audio.state reports it.
    [[nodiscard]] audio::PlayerInfo sound() const {
        for (audio::PlayerInfo& info : audio::players(session->world())) {
            if (info.id == "tv") {
                return info;
            }
        }
        return {};
    }

    [[nodiscard]] double position() const {
        return ok("entity.get", R"({"id": "tv"})")
            .find("components")
            ->find("VideoStreamPlayer")
            ->find("stream_position")
            ->as_double();
    }

    [[nodiscard]] float peak_db() const {
        // After a volume change's ramp, which takes one mix step.
        auto captured = audio::capture(session->world(), "Master", 0.02, 1);
        REQUIRE(captured);
        return captured.value().peak_db;
    }
};

// Sound positions move in mix steps of 512 frames, so they are within two of where the
// picture says.
bool close_to(double sound, double picture) {
    return std::abs(sound - picture) < 1024.0 / 44100.0;
}

} // namespace

TEST_CASE("video: a VideoStreamPlayer's sound plays on its bus with its picture") {
    const std::string root = "video_sound_project";
    std::filesystem::remove_all(root);
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(platform::write_file(root + "/project.json", R"({"format": 1, "name": "Video",
        "main_scene": "main.scene.json", "version": "1.0.0"})")
                .ok());
    // 0.3 seconds of a square wave at half of full scale, with three frames.
    const std::vector<render::Image> frames{flat({255, 0, 0}), flat({0, 255, 0}),
                                            flat({0, 0, 255})};
    // At the mix rate, so no resampling rounds the square wave's corners.
    video::PcmAudio audio = stereo_tone(13230, 16384);
    audio.sample_rate = 44100;
    for (std::size_t i = 1; i < audio.samples.size(); i += 2) {
        audio.samples[i] = audio.samples[i - 1];
    }
    const auto clip = video::encode_matroska(frames, 10, 1, &audio);
    REQUIRE(clip);
    REQUIRE(platform::write_file(root + "/clip.mkv",
                                 std::string(clip.value().begin(), clip.value().end()))
                .ok());
    SoundEngine engine(root);
    (void)engine.ok("entity.create", R"({"id": "tv", "components": {"Control": {},
        "VideoStreamPlayer": {"stream": "clip.mkv", "autoplay": true}}})");

    // It starts with the picture, and each tick mixes a tick ahead of the frame shown.
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    audio::PlayerInfo sound = engine.sound();
    CHECK(sound.kind == "VideoStreamPlayer");
    CHECK(sound.stream == "clip.mkv");
    CHECK(sound.voices == 1);
    CHECK(sound.length == doctest::Approx(0.3));
    CHECK(close_to(sound.position, engine.position() + 0.05));
    (void)engine.ok("step", R"({"ticks": 2, "tick_seconds": 0.05})");
    CHECK(close_to(engine.sound().position, engine.position() + 0.05));
    CHECK(engine.peak_db() == doctest::Approx(-6.02).epsilon(0.01));

    // Quieter, then paused: the sound fades out and holds where it got to.
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"volume_db": -20}}})");
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    CHECK(engine.peak_db() == doctest::Approx(-26.02).epsilon(0.01));
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"paused": true}}})");
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    const double held = engine.sound().position;
    (void)engine.ok("step", R"({"ticks": 2, "tick_seconds": 0.05})");
    sound = engine.sound();
    CHECK(sound.paused);
    CHECK(sound.position == held);
    CHECK(engine.peak_db() < -100.0F);

    // A jump starts it again where the picture is.
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"paused": false, "stream_position": 0.05}}})");
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    CHECK(close_to(engine.sound().position, engine.position() + 0.05));

    // At the end both stop.
    (void)engine.ok("step", R"({"ticks": 6, "tick_seconds": 0.05})");
    CHECK(engine.sound().voices == 0);

    // A track the video doesn't have says so.
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"audio_track": 1, "playing": true, "stream_position": 0}}})");
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    CHECK(engine.sound().error == "audio_track 1 isn't one of the 1 audio tracks of the video");
    std::filesystem::remove_all(root);
}
