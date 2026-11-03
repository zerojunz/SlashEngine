#pragma once

#include "slash/core/result.hpp"
#include "slash/video/video.hpp"

#include <span>
#include <vector>

namespace slash::video::detail {

// An Opus track's packets decoded with libopus, as its OpusHead (the track's CodecPrivate) sets
// the decoder up: interleaved samples from -1 to 1 at 48 kHz, in WAV's order of speakers, the
// pre-skip left out and the output gain applied.
Result<std::vector<float>> decode_opus(const VideoFile& video, const AudioTrack& track);

// WAV's order of speakers from Vorbis's, which Opus's channel mapping family 1 uses: where each
// of a frame's channels in WAV's order is among Vorbis's, for one to eight channels.
[[nodiscard]] std::span<const int> wav_from_vorbis(int channels);

} // namespace slash::video::detail
