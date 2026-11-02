#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
}

// Video playback, as Godot's VideoStream and VideoStreamPlayer, with AV1 in place of Theora:
// dav1d decodes the frames, which are the same pixels on every machine.
namespace slash::video {

// An audio track of a video file, as the embedded tracks Godot's VideoStreamPlayer picks with
// audio_track: its codec and where each of its packets is.
struct AudioTrack {
    enum class Codec : std::uint8_t {
        Pcm,   // A_PCM/INT/LIT: little-endian integers, unsigned at 8 bits
        Float, // A_PCM/FLOAT/IEEE
        Opus,  // A_OPUS
        Other, // anything else, which can't be played
    };
    struct Packet {
        std::size_t offset = 0; // of its bytes in the VideoFile's data
        std::size_t size = 0;
    };

    Codec codec = Codec::Other;
    std::string codec_id; // as the file names it, e.g. "A_OPUS"
    std::string name;
    std::string language;
    int sample_rate = 0;
    int channels = 0;
    int bits = 0;                   // PCM's bits per sample
    std::int64_t pre_skip = 0;      // Opus: samples at the start that aren't sound
    std::size_t private_offset = 0; // its codec's setup (CodecPrivate) in the data
    std::size_t private_size = 0;
    std::vector<Packet> packets; // in order, without gaps
};

// An AV1 video in an IVF, WebM or Matroska file, as Godot's VideoStream: its size, frame rate,
// where each frame's temporal unit is, and its audio tracks.
struct VideoFile {
    struct Frame {
        std::size_t offset = 0; // of the temporal unit's bytes in data
        std::size_t size = 0;
        std::uint64_t pts = 0; // in units of scale / rate seconds
        bool key = false;      // a key frame, which decoding can start at
    };

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The time base: a pts of 1 is scale / rate seconds, so rate / scale frames a second for a
    // video with a frame at each pts.
    std::uint32_t rate = 0;
    std::uint32_t scale = 0;
    std::vector<Frame> frames;
    std::vector<std::uint8_t> data;
    // AV1's sequence header OBUs from a Matroska track's CodecPrivate, which decoding starts with
    // at each key frame; empty for IVF files, whose key frames carry their own.
    std::vector<std::uint8_t> config;
    // The file's own duration in seconds, or 0 where it gives none.
    double duration = 0.0;
    std::vector<AudioTrack> audio;

    // When frame index starts and how long the video lasts, in seconds: the file's duration, or
    // else the last frame shows for as long as the one before it.
    [[nodiscard]] double frame_time(std::size_t index) const;
    [[nodiscard]] double length() const;
    // The frame showing at a time: the last one starting at or before it.
    [[nodiscard]] std::size_t frame_at(double seconds) const;
};

// Reads an IVF file of AV1 temporal units. Frames that start a coded video sequence with a key
// frame are marked key.
[[nodiscard]] Result<VideoFile> read_ivf(std::span<const std::uint8_t> bytes);
// Writes one, as encoders write them: a 32-byte header, then each frame's size, pts and bytes.
[[nodiscard]] std::vector<std::uint8_t>
write_ivf(std::uint32_t width, std::uint32_t height, std::uint32_t rate, std::uint32_t scale,
          std::span<const std::vector<std::uint8_t>> frames);
// Reads a WebM or Matroska file: its first AV1 video track (V_AV1), and its audio tracks.
// Blocks may be laced and clusters of unknown size; compressed or encrypted tracks are refused.
[[nodiscard]] Result<VideoFile> read_matroska(std::span<const std::uint8_t> bytes);

// Uncompressed audio to write into a video: interleaved 16-bit samples.
struct PcmAudio {
    int sample_rate = 48000;
    int channels = 2;
    std::vector<std::int16_t> samples;
};
// Writes a Matroska file of AV1 temporal units, rate / scale seconds apart, with an audio track
// of PCM (A_PCM/INT/LIT) if audio is given, its blocks interleaved with the frames in clusters,
// times in milliseconds. WebM allows only Opus and Vorbis audio, so the file is Matroska (.mkv).
[[nodiscard]] std::vector<std::uint8_t>
write_matroska(std::uint32_t width, std::uint32_t height, std::uint32_t rate, std::uint32_t scale,
               std::span<const std::vector<std::uint8_t>> frames, const PcmAudio* audio = nullptr);
// A video's audio track decoded: interleaved samples from -1 to 1 of one or two channels (more
// are mixed down to two), with the video's time 0 at its first sample.
struct DecodedAudio {
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;
};
[[nodiscard]] Result<DecodedAudio> decode_audio(const VideoFile& video, std::size_t track);

// Whether an AV1 temporal unit holds a key frame that shows (OBU headers and the frame header's
// first bits), so decoding can start there.
[[nodiscard]] bool is_key_frame(std::span<const std::uint8_t> temporal_unit);

// How a picture's YUV maps to RGB, from its sequence header's color config.
enum class Matrix : std::uint8_t { Bt709, Bt601, Bt2020, Identity };

// One plane-per-channel picture, as dav1d decodes them: Y, then U and V subsampled by
// ss_x and ss_y (both 1 for 4:2:0; no U or V for monochrome), samples of bits 8, 10 or 12 in
// 8- or 16-bit words.
struct YuvPicture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int bits = 8;
    bool ss_x = true;
    bool ss_y = true;
    bool monochrome = false;
    bool full_range = false;
    Matrix matrix = Matrix::Bt709;
    const void* planes[3] = {nullptr, nullptr, nullptr};
    std::ptrdiff_t strides[2] = {0, 0}; // bytes a row: Y's, then U's and V's
};

// Converts a picture to RGBA8 with integer math, the same everywhere: limited range expanded,
// chroma sampled at the nearest sample (as Godot's Theora player does).
void to_rgba(const YuvPicture& picture, render::Image& out);
// And back, for encoders and tests: RGBA8 to 8-bit 4:2:0 at limited range, chroma the mean of
// each 2x2 block. Odd sizes repeat the last column and row.
struct Yuv420 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};
[[nodiscard]] Yuv420 to_yuv420(const render::Image& image, Matrix matrix = Matrix::Bt709);

// Encodes a picture as an AV1 temporal unit that decodes alone (a sequence header and a key
// frame), with the engine's own encoder: lossless, each block drawn from palettes of up to 8
// colors per plane (AV1's screen content tools), so blocks of that few colors come out exactly
// and others are reduced to 8 by k-means. Integer math only, the same bytes everywhere. For
// tests, placeholders and screen recordings; sizes must be multiples of 8, up to 4096x2304.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(const Yuv420& picture);
// Encodes RGBA8 frames of one size as an IVF file of such key frames, rate / scale a second.
[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_ivf(std::span<const render::Image> frames, std::uint32_t rate, std::uint32_t scale = 1);
// And as a Matroska file, with audio if given.
[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_matroska(std::span<const render::Image> frames, std::uint32_t rate, std::uint32_t scale = 1,
                const PcmAudio* audio = nullptr);

// Decodes a VideoFile's frames with dav1d on the calling thread.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Decodes the frame at index into out as RGBA8, going on from the frame decoded last when it
    // comes before index and from the key frame before index otherwise.
    Result<void> decode(const VideoFile& video, std::size_t index, render::Image& out);
    // The frame decoded last, if any.
    [[nodiscard]] std::optional<std::size_t> decoded() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Asset type "video": .ivf, .webm and .mkv files as VideoFile.
void add_asset_types(asset::Assets& assets);

// Adds the asset type and the "video" system (priority 1140, after particles), which plays
// each VideoStreamPlayer: autoplay starts it, stream_position advances by the tick times
// speed_scale until the end, where it stops with a "finished" event or loops, and the frame at
// stream_position is decoded for drawing; the audio system plays its sound. Assets must outlive
// the world.
void install(world::World& world, asset::Assets& assets);

// The frame a VideoStreamPlayer shows: its pixels, and a version that goes up each time they
// change. Null before the system has decoded one, or for an entity with no stream.
struct Shown {
    const render::Image* image = nullptr;
    std::uint64_t version = 0;
    std::size_t frame = 0;
    std::string error;   // why its stream couldn't load or decode
    bool player = false; // the entity is a VideoStreamPlayer
};
[[nodiscard]] Shown shown(const world::World& world, std::string_view entity);
// The file a VideoStreamPlayer plays, as the video system loaded it, with a version that goes up
// when it loads again; null before it loads. The audio system plays its sound from it.
struct Playing {
    std::shared_ptr<const VideoFile> file;
    std::uint64_t version = 0;
};
[[nodiscard]] Playing playing(const world::World& world, std::string_view entity);
// A player's stream's length in seconds, 0 before it loads, as Godot's get_stream_length.
[[nodiscard]] double stream_length(const world::World& world, std::string_view entity);

// Drawing names a player's frame as a texture path of this prefix and the entity's ID, which
// renderers resolve through shown().
inline constexpr std::string_view kTexturePrefix = "video:";

} // namespace slash::video
