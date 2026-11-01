#include "slash/video/video.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace slash::video {
namespace {

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kFrameHeaderSize = 12;

// AV1's OBU types (section 6.2.2) and frame types (6.8.2).
constexpr int kObuSequenceHeader = 1;
constexpr int kObuFrameHeader = 3;
constexpr int kObuFrame = 6;
constexpr int kKeyFrame = 0;

std::uint32_t read_u16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8U));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint64_t>(read_u32(bytes, at)) |
           (static_cast<std::uint64_t>(read_u32(bytes, at + 4)) << 32U);
}

void put_u16(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    put_u32(out, static_cast<std::uint32_t>(value >> 32U));
}

// leb128() (4.10.5): a size, or nothing if it runs past the end or past 8 bytes.
std::optional<std::uint64_t> read_leb128(std::span<const std::uint8_t> bytes, std::size_t& at) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        if (at >= bytes.size()) {
            return std::nullopt;
        }
        const std::uint8_t byte = bytes[at++];
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << (7U * static_cast<unsigned>(i));
        if ((byte & 0x80U) == 0) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace

double VideoFile::frame_time(std::size_t index) const {
    if (frames.empty() || rate == 0) {
        return 0.0;
    }
    const std::uint64_t start = frames.front().pts;
    const std::uint64_t pts = frames[std::min(index, frames.size() - 1)].pts;
    return static_cast<double>(pts - start) * scale / rate;
}

double VideoFile::length() const {
    if (frames.empty() || rate == 0) {
        return 0.0;
    }
    const std::uint64_t last = frames.back().pts;
    const std::uint64_t before = frames.size() > 1 ? frames[frames.size() - 2].pts : last - 1;
    const std::uint64_t end = last + std::max<std::uint64_t>(last - before, 1);
    return static_cast<double>(end - frames.front().pts) * scale / rate;
}

std::size_t VideoFile::frame_at(double seconds) const {
    std::size_t found = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frame_time(i) > seconds) {
            break;
        }
        found = i;
    }
    return found;
}

bool is_key_frame(std::span<const std::uint8_t> temporal_unit) {
    bool reduced_still_picture_header = false;
    std::size_t at = 0;
    while (at < temporal_unit.size()) {
        const std::uint8_t header = temporal_unit[at++];
        const auto type = static_cast<int>((header >> 3U) & 0xFU);
        const bool extension = (header & 0x4U) != 0;
        const bool has_size = (header & 0x2U) != 0;
        if (extension) {
            ++at;
        }
        std::uint64_t size = temporal_unit.size() - std::min(at, temporal_unit.size());
        if (has_size) {
            const auto read = read_leb128(temporal_unit, at);
            if (!read) {
                return false;
            }
            size = *read;
        }
        if (at > temporal_unit.size() || size > temporal_unit.size() - at) {
            return false;
        }
        if (type == kObuSequenceHeader && size > 0) {
            // seq_profile (3 bits), still_picture (1), reduced_still_picture_header (1).
            reduced_still_picture_header = (temporal_unit[at] & 0x08U) != 0;
        } else if ((type == kObuFrameHeader || type == kObuFrame) && size > 0) {
            if (reduced_still_picture_header) {
                return true;
            }
            // show_existing_frame (1 bit), then frame_type (2 bits).
            const std::uint8_t first = temporal_unit[at];
            const bool show_existing = (first & 0x80U) != 0;
            return !show_existing && ((first >> 5U) & 0x3U) == kKeyFrame;
        }
        at += static_cast<std::size_t>(size);
    }
    return false;
}

Result<VideoFile> read_ivf(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), "DKIF", 4) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "not an IVF file: it doesn't start with DKIF");
    }
    if (std::memcmp(bytes.data() + 8, "AV01", 4) != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "the IVF file holds \"" +
                              std::string(reinterpret_cast<const char*>(bytes.data() + 8), 4) +
                              "\" video, not AV1 (AV01)");
    }
    VideoFile video;
    const std::uint32_t header_size = read_u16(bytes, 6);
    video.width = read_u16(bytes, 12);
    video.height = read_u16(bytes, 14);
    video.rate = read_u32(bytes, 16);
    video.scale = read_u32(bytes, 20);
    if (video.rate == 0 || video.scale == 0) {
        return make_error(ErrorCode::InvalidArgument, "the IVF file's time base is 0");
    }
    video.data.assign(bytes.begin(), bytes.end());
    std::size_t at = std::max<std::size_t>(header_size, kHeaderSize);
    while (at + kFrameHeaderSize <= bytes.size()) {
        VideoFile::Frame frame;
        frame.size = read_u32(bytes, at);
        frame.pts = read_u64(bytes, at + 4);
        frame.offset = at + kFrameHeaderSize;
        if (frame.size > bytes.size() - frame.offset) {
            return make_error(ErrorCode::InvalidArgument, "frame " +
                                                              std::to_string(video.frames.size()) +
                                                              " of the IVF file runs past its end");
        }
        if (!video.frames.empty() && frame.pts <= video.frames.back().pts) {
            return make_error(ErrorCode::InvalidArgument,
                              "frame " + std::to_string(video.frames.size()) +
                                  " of the IVF file doesn't come after the one before it");
        }
        frame.key = is_key_frame(bytes.subspan(frame.offset, frame.size));
        video.frames.push_back(frame);
        at = frame.offset + frame.size;
    }
    if (video.frames.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the IVF file has no frames");
    }
    if (!video.frames.front().key) {
        return make_error(ErrorCode::InvalidArgument,
                          "the IVF file's first frame isn't a key frame, so it can't be decoded");
    }
    return video;
}

std::vector<std::uint8_t> write_ivf(std::uint32_t width, std::uint32_t height, std::uint32_t rate,
                                    std::uint32_t scale,
                                    std::span<const std::vector<std::uint8_t>> frames) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'D', 'K', 'I', 'F'});
    put_u16(out, 0);
    put_u16(out, kHeaderSize);
    out.insert(out.end(), {'A', 'V', '0', '1'});
    put_u16(out, width);
    put_u16(out, height);
    put_u32(out, rate);
    put_u32(out, scale);
    put_u32(out, static_cast<std::uint32_t>(frames.size()));
    put_u32(out, 0);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        put_u32(out, static_cast<std::uint32_t>(frames[i].size()));
        put_u64(out, i);
        out.insert(out.end(), frames[i].begin(), frames[i].end());
    }
    return out;
}

} // namespace slash::video
