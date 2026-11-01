#include "slash/video/video.hpp"

#include <cerrno>
#include <cstring>
#include <dav1d/dav1d.h>
#include <string>

namespace slash::video {
namespace {

Matrix matrix_of(const Dav1dSequenceHeader& header) {
    switch (header.mtrx) {
    case DAV1D_MC_IDENTITY:
        return Matrix::Identity;
    case DAV1D_MC_BT470BG:
    case DAV1D_MC_BT601:
    case DAV1D_MC_FCC:
    case DAV1D_MC_SMPTE240:
        return Matrix::Bt601;
    case DAV1D_MC_BT2020_NCL:
    case DAV1D_MC_BT2020_CL:
        return Matrix::Bt2020;
    default:
        // BT.709, and what a video leaves unspecified.
        return Matrix::Bt709;
    }
}

Error dav1d_error(std::string_view what, int code) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) + " (dav1d error " + std::to_string(-code) + ")");
}

} // namespace

struct Decoder::State {
    Dav1dContext* context = nullptr;
    const VideoFile* video = nullptr;
    std::optional<std::size_t> decoded;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        if (context != nullptr) {
            dav1d_close(&context);
        }
    }

    Result<void> open() {
        if (context != nullptr) {
            dav1d_close(&context);
        }
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        // One thread and no frames in flight: the caller's thread decodes each frame as it
        // asks, and runs repeat.
        settings.n_threads = 1;
        settings.max_frame_delay = 1;
        if (const int opened = dav1d_open(&context, &settings); opened < 0) {
            context = nullptr;
            return dav1d_error("dav1d didn't start", opened);
        }
        decoded.reset();
        return {};
    }

    // Sends one temporal unit and takes the picture it shows.
    Result<void> send(const VideoFile& file, std::size_t index, Dav1dPicture& picture) const {
        const VideoFile::Frame& frame = file.frames[index];
        Dav1dData data{};
        std::uint8_t* bytes = dav1d_data_create(&data, frame.size);
        if (bytes == nullptr) {
            return make_error(ErrorCode::InvalidArgument, "out of memory for a video frame");
        }
        std::memcpy(bytes, file.data.data() + frame.offset, frame.size);
        bool have = false;
        while (data.sz > 0) {
            const int sent = dav1d_send_data(context, &data);
            if (sent == DAV1D_ERR(EAGAIN)) {
                // A picture waits: take it, then send the rest.
                if (have) {
                    dav1d_picture_unref(&picture);
                }
                if (const int got = dav1d_get_picture(context, &picture); got < 0) {
                    dav1d_data_unref(&data);
                    return dav1d_error("frame " + std::to_string(index) + " didn't decode", got);
                }
                have = true;
            } else if (sent < 0) {
                dav1d_data_unref(&data);
                return dav1d_error("frame " + std::to_string(index) + " didn't decode", sent);
            }
        }
        if (have) {
            dav1d_picture_unref(&picture);
        }
        if (const int got = dav1d_get_picture(context, &picture); got < 0) {
            return dav1d_error("frame " + std::to_string(index) + " showed no picture", got);
        }
        return {};
    }
};

Decoder::Decoder() : state_(std::make_unique<State>()) {}

Decoder::~Decoder() = default;

std::optional<std::size_t> Decoder::decoded() const {
    return state_->decoded;
}

Result<void> Decoder::decode(const VideoFile& video, std::size_t index, render::Image& out) {
    if (index >= video.frames.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the video has " + std::to_string(video.frames.size()) + " frames, not " +
                              std::to_string(index + 1));
    }
    State& state = *state_;
    // Go on from the frame before, or start again at the key frame at or before index.
    std::size_t start = index;
    while (start > 0 && !video.frames[start].key) {
        --start;
    }
    const bool same_video = state.video == &video && state.context != nullptr;
    if (same_video && state.decoded && *state.decoded < index && *state.decoded >= start) {
        start = *state.decoded + 1;
    } else {
        if (auto opened = state.open(); !opened) {
            return opened.error();
        }
        state.video = &video;
    }
    for (std::size_t i = start; i <= index; ++i) {
        Dav1dPicture picture{};
        if (auto sent = state.send(video, i, picture); !sent) {
            state.decoded.reset();
            return sent.error();
        }
        state.decoded = i;
        if (i == index) {
            YuvPicture yuv;
            yuv.width = static_cast<std::uint32_t>(picture.p.w);
            yuv.height = static_cast<std::uint32_t>(picture.p.h);
            yuv.bits = picture.p.bpc;
            yuv.monochrome = picture.p.layout == DAV1D_PIXEL_LAYOUT_I400;
            yuv.ss_x = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420 ||
                       picture.p.layout == DAV1D_PIXEL_LAYOUT_I422;
            yuv.ss_y = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420;
            yuv.full_range = picture.seq_hdr->color_range != 0;
            yuv.matrix = matrix_of(*picture.seq_hdr);
            for (int plane = 0; plane < 3; ++plane) {
                yuv.planes[plane] = picture.data[plane];
            }
            yuv.strides[0] = picture.stride[0];
            yuv.strides[1] = picture.stride[1];
            to_rgba(yuv, out);
        }
        dav1d_picture_unref(&picture);
    }
    return {};
}

} // namespace slash::video
