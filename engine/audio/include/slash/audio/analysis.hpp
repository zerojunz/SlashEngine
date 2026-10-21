#pragma once

#include "slash/audio/audio.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Measuring sound, so agents judge music without listening: loudness as EBU R 128 measures it
// (with libebur128), peaks, clipping, silence, tempo and beat grid, key, frequency balance and
// how cleanly a loop joins. Every measure runs on stereo frames as the game plays them: a mono
// sound is the same on both sides.
namespace slash::audio {

// Octave bands as sound level meters show them, centred on 31.5 Hz to 16 kHz.
inline constexpr std::array<std::array<double, 2>, 10> kOctaveBands{{{22, 44},
                                                                     {44, 88},
                                                                     {88, 177},
                                                                     {177, 355},
                                                                     {355, 710},
                                                                     {710, 1420},
                                                                     {1420, 2840},
                                                                     {2840, 5680},
                                                                     {5680, 11360},
                                                                     {11360, 22720}}};

// Where a loop joins: playing reaches end and goes on from start (frames, end exclusive). With
// crossfade, what follows end fades out over that many frames after the join, as a compressed
// sound looping after beat_count beats does.
struct LoopPoints {
    std::int64_t start = 0;
    std::int64_t end = 0;
    int crossfade = 0;
};

// Frames a compressed sound's beat loop fades over, as Godot's AudioStreamPlaybackOggVorbis.
inline constexpr int kLoopCrossfade = 256;

struct Analysis {
    double seconds = 0.0;

    // Loudness, as ITU-R BS.1770-4 and EBU R 128: integrated loudness, loudness range and the
    // loudest three seconds, in LUFS and LU; nullopt when too quiet to gate (below -70 LUFS).
    std::optional<double> lufs;
    std::optional<double> loudness_range;
    std::optional<double> max_short_term_lufs;
    // Peaks in dBFS, nullopt if silent: true_peak_db between samples, as a DAC plays them.
    std::optional<double> true_peak_db;
    std::optional<double> peak_db;
    std::optional<double> rms_db;

    // Clipping: runs of three samples or more at full scale, or samples past it, which the
    // output clips. first_clip is in seconds.
    int clips = 0;
    double first_clip = 0.0;

    // Silence, below -60 dBFS: at the start, at the end and gaps of half a second or more in
    // between, in seconds.
    double silence_start = 0.0;
    double silence_end = 0.0;
    std::vector<std::pair<double, double>> gaps;

    // Tempo, from where notes start: beats per minute, the first beat in seconds and how sure
    // the measure is (0 to 1); nullopt when the sound has no steady beat or is too short.
    std::optional<double> bpm;
    double beat_offset = 0.0;
    double tempo_confidence = 0.0;

    // Key, such as "A minor", from the notes' pitch classes against Krumhansl and Kessler's key
    // profiles; key_confidence is the correlation (0 to 1). nullopt when no key stands out.
    std::optional<std::string> key;
    double key_confidence = 0.0;

    // Each octave band's share of the sound's energy, in dB (0 for all of it); nullopt for a
    // band with none.
    std::array<std::optional<double>, kOctaveBands.size()> bands_db{};

    // The loop's join, when there is a loop: its length, the jump between the last sample
    // before the join and the first after it in dBFS (nullopt if none), the level over the beat
    // after the join against the beat before it in dB (a quarter second each with no tempo),
    // and whether the join clicks: a jump far bigger than the sound's own sample to sample
    // changes.
    struct Loop {
        double seconds = 0.0;
        std::optional<double> step_db;
        double level_change_db = 0.0;
        bool clicks = false;
    };
    std::optional<Loop> loop;
};

// Measures frames at mix_rate frames per second; loop, if given, is where the sound loops.
[[nodiscard]] Analysis analyze(std::span<const Frame> frames, int mix_rate,
                               std::optional<LoopPoints> loop = std::nullopt);

// Where a sound loops as its .audio.json file plays it (from the end, or after beat_count beats,
// back to loop_offset; a .wav file's forward loop), its whole length with no settings, or
// nullopt when the settings don't loop it.
[[nodiscard]] std::optional<LoopPoints> loop_points(const Sound& sound, const StreamFile* settings);

// A sound's every frame, decoded, at its own mix rate; mono is the same on both sides.
[[nodiscard]] Result<std::vector<Frame>> decode_frames(const Sound& sound);

} // namespace slash::audio
