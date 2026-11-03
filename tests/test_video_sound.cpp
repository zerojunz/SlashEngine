#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/bits.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
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

// A cluster of a black key frame.
Bytes cluster_of_silence() {
    return element(0x1F43B675,
                   join({number(0xE7, 0), block(0xA3, 1, 0x80, {}, key_frame({0, 0, 0}))}));
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

    // Opus is read and decoded: this packet is 20 milliseconds of stereo CELT, the first 312
    // samples of which are the pre-skip.
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
    decoded = video::decode_audio(read.value(), 0);
    REQUIRE_MESSAGE(decoded, (decoded ? std::string() : decoded.error().message));
    CHECK(decoded.value().sample_rate == 48000);
    CHECK(decoded.value().samples.size() == std::size_t{(960 - 312) * 2});
    // An OpusHead that isn't one, and a codec nothing plays.
    opus_head[0] = 'X';
    read = video::read_matroska(file_of(
        join({video_track("V_AV1"), audio_track(2, "A_OPUS", 2, element(0x63A2, opus_head))}),
        cluster_of_silence()));
    CHECK(video::decode_audio(read.value(), 0).error().message ==
          "the video's Opus track's CodecPrivate isn't an OpusHead of version 0");
    read = video::read_matroska(
        file_of(join({video_track("V_AV1"), audio_track(2, "A_VORBIS", 2)}), cluster_of_silence()));
    CHECK(video::decode_audio(read.value(), 0).error().message ==
          "the video's audio is A_VORBIS, which can't be played; videos play Opus (A_OPUS) and "
          "PCM (A_PCM/INT/LIT or A_PCM/FLOAT/IEEE) audio");

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

// frames of 48 kHz sound in WAV's order of channels, with a sine of amplitude at hz in channel
// on, the rest silent.
video::PcmAudio tone_in(int channels, int on, int frames, double hz, double amplitude) {
    video::PcmAudio audio;
    audio.channels = channels;
    audio.samples.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels), 0);
    for (int i = 0; i < frames; ++i) {
        audio.samples[(static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)) +
                      static_cast<std::size_t>(on)] =
            static_cast<std::int16_t>(
                std::lround(amplitude * std::sin(2.0 * 3.14159265358979 * hz * i / 48000.0)));
    }
    return audio;
}

// The RMS of one channel of interleaved samples, from frame first for count frames.
double rms(const std::vector<float>& samples, int channels, int channel, std::size_t first,
           std::size_t count) {
    double sum = 0.0;
    for (std::size_t i = first; i < first + count; ++i) {
        const double value =
            samples[(i * static_cast<std::size_t>(channels)) + static_cast<std::size_t>(channel)];
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(count));
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes,
                    std::uint64_t hash = 14695981039346656037ULL) {
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

} // namespace

TEST_CASE("video: Opus sound in WebM files, encoded and decoded with libopus") {
    // Half a second of a 1 kHz tone on the left and 500 Hz at half the level on the right.
    video::PcmAudio audio = tone_in(2, 0, 24000, 1000.0, 8000.0);
    const video::PcmAudio right = tone_in(2, 1, 24000, 500.0, 4000.0);
    for (std::size_t i = 1; i < audio.samples.size(); i += 2) {
        audio.samples[i] = right.samples[i];
    }
    auto opus = video::encode_opus(audio);
    REQUIRE_MESSAGE(opus, (opus ? std::string() : opus.error().message));
    CHECK(opus.value().channels == 2);
    CHECK(opus.value().pre_skip == 312);
    CHECK(opus.value().head.size() == 19);
    // The sound and the pre-skip after it, in packets of 20 milliseconds.
    CHECK(opus.value().packets.size() == (24000 + 312 + 959) / 960);
    CHECK(opus.value().end_padding == (26 * 960) - 24000 - 312);
    // Fixed point: the same bytes on every platform, and the same each time.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& packet : opus.value().packets) {
        hash = fnv1a(packet, hash);
    }
    auto again = video::encode_opus(audio);
    REQUIRE(again);
    CHECK(again.value().packets == opus.value().packets);
    CHECK(hash == 117929826942146053ULL);

    const std::vector<render::Image> frames{flat({255, 0, 0}), flat({0, 0, 255})};
    const auto bytes = video::encode_matroska(frames, 4, 1, nullptr, &opus.value());
    REQUIRE(bytes);
    // All Opus, so the file is WebM.
    const std::string_view webm("webm");
    CHECK(std::search(bytes.value().begin(), bytes.value().end(), webm.begin(), webm.end()) !=
          bytes.value().end());
    auto read = video::read_matroska(bytes.value());
    REQUIRE_MESSAGE(read, (read ? std::string() : read.error().message));
    REQUIRE(read.value().audio.size() == 1);
    const video::AudioTrack& track = read.value().audio[0];
    CHECK(track.codec == video::AudioTrack::Codec::Opus);
    CHECK(track.pre_skip == 312);
    CHECK(track.packets.size() == opus.value().packets.size());
    auto decoded = video::decode_audio(read.value(), 0);
    REQUIRE_MESSAGE(decoded, (decoded ? std::string() : decoded.error().message));
    CHECK(decoded.value().sample_rate == 48000);
    CHECK(decoded.value().channels == 2);
    const std::vector<float>& sound = decoded.value().samples;
    // As long as the sound: the pre-skip and the last block's DiscardPadding are left out.
    REQUIRE(sound.size() == audio.samples.size());
    CHECK(track.packets.back().discard_padding ==
          std::int64_t{(26 * 960) - 24000 - 312} * 1000000000 / 48000);
    std::uint64_t decoded_hash = 14695981039346656037ULL;
    for (const float sample : sound) {
        const auto bits = slash::bit_cast<std::uint32_t>(sample);
        const std::array<std::uint8_t, 4> word{static_cast<std::uint8_t>(bits & 0xFFU),
                                               static_cast<std::uint8_t>((bits >> 8U) & 0xFFU),
                                               static_cast<std::uint8_t>((bits >> 16U) & 0xFFU),
                                               static_cast<std::uint8_t>(bits >> 24U)};
        decoded_hash = fnv1a(word, decoded_hash);
    }
    CHECK(decoded_hash == 13699003354829908551ULL);
    // As loud as the tones, a sine's RMS being its amplitude over the square root of 2.
    CHECK(rms(sound, 2, 0, 2400, 19200) ==
          doctest::Approx(8000.0 / 32768.0 / std::sqrt(2.0)).epsilon(0.05));
    CHECK(rms(sound, 2, 1, 2400, 19200) ==
          doctest::Approx(4000.0 / 32768.0 / std::sqrt(2.0)).epsilon(0.05));
    // And in time with them: the pre-skip leaves the sound where it was, best matched at no
    // shift.
    const auto match = [&](int shift) {
        double sum = 0.0;
        for (std::size_t i = 2400; i < 21600; ++i) {
            sum += sound[(i + static_cast<std::size_t>(shift)) * 2] * audio.samples[i * 2];
        }
        return sum;
    };
    for (const int shift : {1, 2, 3, 6, 12}) {
        CAPTURE(shift);
        CHECK(match(0) > match(shift));
    }

    // The output gain in the OpusHead (Q7.8 dB) is applied: 6 dB about doubles it.
    video::OpusAudio louder = opus.value();
    louder.head[16] = 0x00;
    louder.head[17] = 0x06;
    decoded = video::decode_audio(
        video::read_matroska(video::encode_matroska(frames, 4, 1, nullptr, &louder).value())
            .value(),
        0);
    CHECK(rms(decoded.value().samples, 2, 0, 2400, 19200) ==
          doctest::Approx(2.0 * 8000.0 / 32768.0 / std::sqrt(2.0)).epsilon(0.05));

    // Surround in Vorbis's order, folded to stereo as PCM is: the center at -3 dB into each
    // side, the LFE left out.
    const auto surround = [&](int on) {
        auto encoded = video::encode_opus(tone_in(6, on, 9600, 1000.0, 8000.0), 256000);
        REQUIRE(encoded);
        CHECK(encoded.value().head.size() == 19 + 2 + 6);
        auto file = video::read_matroska(
            video::encode_matroska(frames, 4, 1, nullptr, &encoded.value()).value());
        REQUIRE(file);
        auto out = video::decode_audio(file.value(), 0);
        REQUIRE(out);
        CHECK(out.value().channels == 2);
        return std::array<double, 2>{rms(out.value().samples, 2, 0, 1200, 7200),
                                     rms(out.value().samples, 2, 1, 1200, 7200)};
    };
    const double tone = 8000.0 / 32768.0 / std::sqrt(2.0);
    const std::array<double, 2> center = surround(2);
    CHECK(center[0] == doctest::Approx(0.70710678 * tone).epsilon(0.08));
    CHECK(center[1] == doctest::Approx(0.70710678 * tone).epsilon(0.08));
    const std::array<double, 2> front_right = surround(1);
    CHECK(front_right[0] < 0.1 * tone);
    CHECK(front_right[1] == doctest::Approx(tone).epsilon(0.08));
    const std::array<double, 2> lfe = surround(3);
    CHECK(lfe[0] < 0.05 * tone);
    CHECK(lfe[1] < 0.05 * tone);

    // PCM and Opus together: two tracks, in that order, in a Matroska file.
    const video::PcmAudio pcm = stereo_tone(14400, 8000);
    const auto both = video::encode_matroska(frames, 4, 1, &pcm, &opus.value());
    REQUIRE(both);
    read = video::read_matroska(both.value());
    REQUIRE(read);
    REQUIRE(read.value().audio.size() == 2);
    CHECK(read.value().audio[0].codec == video::AudioTrack::Codec::Pcm);
    CHECK(read.value().audio[1].codec == video::AudioTrack::Codec::Opus);
    CHECK(video::decode_audio(read.value(), 1).value().samples.size() == sound.size());

    // What Opus can't encode.
    video::PcmAudio slow = audio;
    slow.sample_rate = 44100;
    CHECK(video::encode_opus(slow).error().message == "Opus encodes 48000 Hz audio, not 44100 Hz");
    video::PcmAudio nine = tone_in(9, 0, 960, 1000.0, 1000.0);
    CHECK(video::encode_opus(nine).error().message == "Opus encodes 1 to 8 channels here, not 9");
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
