#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Lossy AV1 encoding with libaom, for the tools and tests (slash::video_encoder): games only
// decode. Where the engine's own encoder (video::encode_ivf) keeps up to 8 colors a block in
// key frames alone, this one codes any picture, and frames after the first as changes to the
// ones before, as aomenc does.
namespace slash::video {

// How encode_lossy codes a clip, as aomenc's options of the same names.
struct LossyOptions {
    // The quantizer, 0 (finest) to 63 (smallest), as aomenc's --cq-level with --end-usage=q.
    int cq_level = 32;
    // How much search libaom trades for speed, 0 (slowest, smallest) to 9, as --cpu-used.
    int cpu_used = 6;
    // A key frame every this many frames, so playback can seek to them; 0 for the first only.
    std::uint32_t kf_interval = 60;
};

// Encodes RGBA8 frames of one size as an IVF file of AV1, rate / scale a second: 8-bit 4:2:0
// at limited range in BT.709 (video::to_yuv420), key frames at each kf_interval, the frames
// between them predicted from those before (and hidden frames ahead, which libaom shows later).
// Each temporal unit shows one frame, so read_ivf finds them all. libaom runs as generic C on
// one thread; its rate decisions use the C library's math, so bytes can differ a little between
// machines, but every decoder shows the same pixels for the same bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_lossy(std::span<const render::Image> frames,
                                                             std::uint32_t rate,
                                                             std::uint32_t scale = 1,
                                                             const LossyOptions& options = {});

} // namespace slash::video
