// Opus sound in videos, with libopus (RFC 6716) and the OpusHead of RFC 7845 that sets up its
// decoder. libopus is built as fixed point (cmake/Dependencies.cmake), so encoding and decoding
// are integer math and come out the same everywhere.
#include "opus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <opus.h>
#include <opus_multistream.h>
#include <string>

namespace slash::video {
namespace detail {
namespace {

constexpr int kOpusRate = 48000;
// Opus's longest packet: 120 milliseconds at 48 kHz.
constexpr int kMaxPacketFrames = 5760;
// The packets encode_opus writes: 20 milliseconds.
constexpr int kPacketFrames = 960;
constexpr std::size_t kHeadSize = 19;
constexpr std::size_t kMaxPacketBytes = 4000;

// Where each of Vorbis's speakers (RFC 7845 section 5.1.1.2) is in WAV's order: front left,
// front right, center, LFE, rear left, rear right, side left, side right.
constexpr std::array<int, 1> kOne{0};
constexpr std::array<int, 2> kTwo{0, 1};
constexpr std::array<int, 3> kThree{0, 2, 1};
constexpr std::array<int, 4> kFour{0, 1, 2, 3};
constexpr std::array<int, 5> kFive{0, 2, 1, 3, 4};
constexpr std::array<int, 6> kSix{0, 2, 1, 5, 3, 4};
constexpr std::array<int, 7> kSeven{0, 2, 1, 6, 5, 3, 4};
constexpr std::array<int, 8> kEight{0, 2, 1, 7, 5, 6, 3, 4};

struct DecoderDeleter {
    void operator()(OpusMSDecoder* decoder) const { opus_multistream_decoder_destroy(decoder); }
};
struct EncoderDeleter {
    void operator()(OpusMSEncoder* encoder) const { opus_multistream_encoder_destroy(encoder); }
};

Error opus_failed(const std::string& what, int code) {
    return Error{ErrorCode::InvalidArgument, what + ": " + opus_strerror(code)};
}

} // namespace

std::span<const int> wav_from_vorbis(int channels) {
    switch (channels) {
    case 1:
        return kOne;
    case 2:
        return kTwo;
    case 3:
        return kThree;
    case 4:
        return kFour;
    case 5:
        return kFive;
    case 6:
        return kSix;
    case 7:
        return kSeven;
    case 8:
        return kEight;
    default:
        return {};
    }
}

Result<std::vector<float>> decode_opus(const VideoFile& video, const AudioTrack& track) {
    // OpusHead: "OpusHead", the version, channels, pre-skip, the input's rate, the output gain in
    // Q7.8 dB and the channel mapping family, then for families other than 0 the streams, the
    // coupled streams and where each channel comes from.
    if (track.private_size < kHeadSize ||
        track.private_offset + track.private_size > video.data.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the video's Opus track has no OpusHead (CodecPrivate)");
    }
    const std::span<const std::uint8_t> head(video.data.data() + track.private_offset,
                                             track.private_size);
    if (!std::equal(head.begin(), head.begin() + 8, "OpusHead") || (head[8] & 0xF0U) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "the video's Opus track's CodecPrivate isn't an OpusHead of version 0");
    }
    const int channels = head[9];
    const int pre_skip = head[10] | (head[11] << 8U);
    const auto gain = static_cast<std::int16_t>(head[16] | (head[17] << 8U));
    const int family = head[18];
    int streams = 1;
    int coupled = channels == 2 ? 1 : 0;
    std::array<unsigned char, 255> mapping{0, 1};
    if (family == 0) {
        if (channels < 1 || channels > 2) {
            return make_error(ErrorCode::InvalidArgument,
                              "the video's Opus track has " + std::to_string(channels) +
                                  " channels with channel mapping family 0, which allows 1 or 2");
        }
    } else {
        if (channels < 1 || head.size() < kHeadSize + 2 + static_cast<std::size_t>(channels)) {
            return make_error(ErrorCode::InvalidArgument,
                              "the video's Opus track's OpusHead has no channel mapping table");
        }
        streams = head[19];
        coupled = head[20];
        std::copy_n(head.begin() + kHeadSize + 2, channels, mapping.begin());
    }
    const bool reorder = family == 1 && !wav_from_vorbis(channels).empty();
    int code = OPUS_OK;
    const std::unique_ptr<OpusMSDecoder, DecoderDeleter> decoder(opus_multistream_decoder_create(
        kOpusRate, channels, streams, coupled, mapping.data(), &code));
    if (decoder == nullptr || code != OPUS_OK) {
        return opus_failed("the video's Opus track can't be decoded", code);
    }
    if (gain != 0) {
        code = opus_multistream_decoder_ctl(decoder.get(), OPUS_SET_GAIN(gain));
        if (code != OPUS_OK) {
            return opus_failed("the video's Opus track's output gain can't be applied", code);
        }
    }

    const auto width = static_cast<std::size_t>(channels);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(kMaxPacketFrames) * width);
    std::vector<float> out;
    std::size_t skip = static_cast<std::size_t>(pre_skip) * width;
    for (std::size_t i = 0; i < track.packets.size(); ++i) {
        const AudioTrack::Packet& packet = track.packets[i];
        const int frames = opus_multistream_decode(decoder.get(), video.data.data() + packet.offset,
                                                   static_cast<opus_int32>(packet.size), pcm.data(),
                                                   kMaxPacketFrames, 0);
        if (frames < 0) {
            return opus_failed("the video's Opus packet " + std::to_string(i) + " can't be decoded",
                               frames);
        }
        // The end that isn't sound, at 48 kHz.
        const auto padding = static_cast<std::int64_t>(
            ((packet.discard_padding * kOpusRate) + 500000000) / 1000000000);
        const auto kept = static_cast<std::size_t>(
            std::max<std::int64_t>(0, frames - std::min<std::int64_t>(padding, frames)));
        const std::span<const int> order = wav_from_vorbis(channels);
        for (std::size_t frame = 0; frame < kept; ++frame) {
            for (std::size_t channel = 0; channel < width; ++channel) {
                if (skip > 0) {
                    --skip;
                    continue;
                }
                const std::size_t from =
                    reorder ? static_cast<std::size_t>(order[channel]) : channel;
                out.push_back(static_cast<float>(pcm[(frame * width) + from]) / 32768.0F);
            }
        }
    }
    return out;
}

} // namespace detail

Result<OpusAudio> encode_opus(const PcmAudio& audio, int bitrate) {
    if (audio.sample_rate != detail::kOpusRate) {
        return make_error(ErrorCode::InvalidArgument, "Opus encodes 48000 Hz audio, not " +
                                                          std::to_string(audio.sample_rate) +
                                                          " Hz");
    }
    if (audio.channels < 1 || audio.channels > 8) {
        return make_error(ErrorCode::InvalidArgument, "Opus encodes 1 to 8 channels here, not " +
                                                          std::to_string(audio.channels));
    }
    const int family = audio.channels <= 2 ? 0 : 1;
    int streams = 0;
    int coupled = 0;
    std::array<unsigned char, 255> mapping{};
    int code = OPUS_OK;
    const std::unique_ptr<OpusMSEncoder, detail::EncoderDeleter> encoder(
        opus_multistream_surround_encoder_create(detail::kOpusRate, audio.channels, family,
                                                 &streams, &coupled, mapping.data(),
                                                 OPUS_APPLICATION_AUDIO, &code));
    if (encoder == nullptr || code != OPUS_OK) {
        return detail::opus_failed("Opus can't encode this audio", code);
    }
    code = opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate));
    if (code != OPUS_OK) {
        return detail::opus_failed(
            "Opus can't encode at " + std::to_string(bitrate) + " bits a second", code);
    }
    opus_int32 lookahead = 0;
    code = opus_multistream_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    if (code != OPUS_OK) {
        return detail::opus_failed("Opus's lookahead can't be read", code);
    }

    OpusAudio out;
    out.channels = audio.channels;
    out.pre_skip = static_cast<int>(lookahead);
    out.head = {'O',
                'p',
                'u',
                's',
                'H',
                'e',
                'a',
                'd',
                1,
                static_cast<std::uint8_t>(audio.channels),
                static_cast<std::uint8_t>(out.pre_skip & 0xFF),
                static_cast<std::uint8_t>((out.pre_skip >> 8) & 0xFF)};
    const auto rate = static_cast<std::uint32_t>(audio.sample_rate);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.head.push_back(static_cast<std::uint8_t>((rate >> shift) & 0xFFU));
    }
    out.head.insert(out.head.end(), {0, 0, static_cast<std::uint8_t>(family)});
    if (family != 0) {
        out.head.push_back(static_cast<std::uint8_t>(streams));
        out.head.push_back(static_cast<std::uint8_t>(coupled));
        out.head.insert(out.head.end(), mapping.begin(), mapping.begin() + audio.channels);
    }

    // The samples in Vorbis's order of speakers for family 1, and the pre-skip's worth of
    // silence more at the end, so the last of the sound comes out of the encoder.
    const auto width = static_cast<std::size_t>(audio.channels);
    const std::size_t frames = (audio.samples.size() / width) + static_cast<std::size_t>(lookahead);
    const std::size_t packets = (frames + detail::kPacketFrames - 1) / detail::kPacketFrames;
    std::vector<std::int16_t> input(packets * detail::kPacketFrames * width, 0);
    const std::span<const int> order = detail::wav_from_vorbis(audio.channels);
    for (std::size_t frame = 0; frame < audio.samples.size() / width; ++frame) {
        for (std::size_t channel = 0; channel < width; ++channel) {
            const std::size_t to = family == 1 ? static_cast<std::size_t>(order[channel]) : channel;
            input[(frame * width) + to] = audio.samples[(frame * width) + channel];
        }
    }
    std::vector<std::uint8_t> packet(detail::kMaxPacketBytes);
    for (std::size_t i = 0; i < packets; ++i) {
        const int size = opus_multistream_encode(
            encoder.get(), input.data() + (i * detail::kPacketFrames * width),
            detail::kPacketFrames, packet.data(), static_cast<opus_int32>(packet.size()));
        if (size < 0) {
            return detail::opus_failed("Opus couldn't encode packet " + std::to_string(i), size);
        }
        out.packets.emplace_back(packet.begin(), packet.begin() + size);
    }
    out.end_padding = static_cast<int>((packets * detail::kPacketFrames) - frames);
    return out;
}

} // namespace slash::video
