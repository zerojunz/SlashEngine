#pragma once

#include "slash/core/result.hpp"
#include "slash/soundtrack/score.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Scores played through a SoundFont into stems, one per track, as Ogg Vorbis or WAV files a game
// plays.
namespace slash::soundtrack {

inline constexpr int kRenderRate = 44100;
// Seconds a stem that doesn't loop keeps ringing after the score ends, at most.
inline constexpr double kMaxTail = 4.0;

// The engine's own General MIDI sound bank, as SoundFont 2 bytes: a simple synthesized voice for
// each of the 128 instruments, by family, and a standard drum kit, made in code with no libm, so
// scores play without a SoundFont file and sound the same everywhere.
[[nodiscard]] const std::string& builtin_sound_bank();

struct RenderOptions {
    int rate = kRenderRate;
    // A looping stem is exactly the score's length: what rings past the end is added onto the
    // start, so the loop joins without a click. Otherwise the stem rings on for up to kMaxTail.
    bool loop = false;
};

struct Stem {
    std::string track;
    std::vector<float> samples; // stereo, interleaved
};

// Plays the score's tracks, or only those named, through a SoundFont 2 file's instruments: each
// track's General MIDI program from bank 0 (drums from bank 128), with its volume, pan and
// velocities. Fails for bytes that aren't a SoundFont 2 file or a track whose instrument the
// SoundFont lacks.
[[nodiscard]] Result<std::vector<Stem>> render_score(const Score& score,
                                                     std::string_view sound_font,
                                                     const RenderOptions& options,
                                                     std::span<const std::string> tracks = {});

// Stereo samples as an Ogg Vorbis file at a quality from 0 to 1 (about 64 to 500 kbit/s), its
// stream numbered serial so the same samples make the same file.
[[nodiscard]] Result<std::string> encode_ogg(std::span<const float> stereo, int rate,
                                             double quality, std::uint32_t serial);

// Stereo samples as a 16-bit PCM WAV file, clipped to full scale.
[[nodiscard]] std::string encode_wav(std::span<const float> stereo, int rate);

// The highest sample, in dBFS, or -inf for silence.
[[nodiscard]] double peak_db(std::span<const float> samples);

} // namespace slash::soundtrack
