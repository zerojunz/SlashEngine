// Lossy AV1 through libaom's encoder, set up as aomenc's good quality mode with
// --end-usage=q: one thread, the frames' timing as the IVF file's, and key frames placed only
// where kf_interval asks.

#include "slash/video/lossy_encoder.hpp"

#include "slash/video/video.hpp"

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <string>
#include <utility>

namespace slash::video {
namespace {

// Closes the encoder whichever way encode_lossy returns.
struct Codec {
    aom_codec_ctx_t context{};
    bool open = false;
    Codec() = default;
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    ~Codec() {
        if (open) {
            aom_codec_destroy(&context);
        }
    }
};

Error aom_error(Codec& codec, const std::string& what) {
    const char* detail = aom_codec_error_detail(&codec.context);
    return make_error(ErrorCode::Unknown,
                      "libaom couldn't " + what + ": " + aom_codec_error(&codec.context) +
                          (detail != nullptr ? std::string(" (") + detail + ")" : std::string()));
}

} // namespace

Result<std::vector<std::uint8_t>> encode_lossy(std::span<const render::Image> frames,
                                               std::uint32_t rate, std::uint32_t scale,
                                               const LossyOptions& options) {
    if (frames.empty()) {
        return make_error(ErrorCode::InvalidArgument, "a video needs at least one frame");
    }
    if (rate == 0 || scale == 0) {
        return make_error(ErrorCode::InvalidArgument, "a video's frame rate can't be 0");
    }
    if (options.cq_level < 0 || options.cq_level > 63) {
        return make_error(ErrorCode::InvalidArgument,
                          "cq_level must be 0 to 63, not " + std::to_string(options.cq_level));
    }
    if (options.cpu_used < 0 || options.cpu_used > 9) {
        return make_error(ErrorCode::InvalidArgument,
                          "cpu_used must be 0 to 9, not " + std::to_string(options.cpu_used));
    }
    const std::uint32_t width = frames[0].width;
    const std::uint32_t height = frames[0].height;
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        return make_error(ErrorCode::InvalidArgument,
                          "video frames can be 1x1 to 65535x65535, not " + std::to_string(width) +
                              "x" + std::to_string(height));
    }
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].width != width || frames[i].height != height) {
            return make_error(
                ErrorCode::InvalidArgument,
                "frame " + std::to_string(i) + " is " + std::to_string(frames[i].width) + "x" +
                    std::to_string(frames[i].height) + ", not " + std::to_string(width) + "x" +
                    std::to_string(height) + " as the first is");
        }
    }

    aom_codec_iface_t* iface = aom_codec_av1_cx();
    aom_codec_enc_cfg_t config{};
    Codec codec;
    if (aom_codec_enc_config_default(iface, &config, AOM_USAGE_GOOD_QUALITY) != AOM_CODEC_OK) {
        return make_error(ErrorCode::Unknown, "libaom has no default settings");
    }
    config.g_w = width;
    config.g_h = height;
    config.g_timebase = {static_cast<int>(scale), static_cast<int>(rate)};
    config.g_threads = 1;
    config.g_profile = 0;
    config.g_bit_depth = AOM_BITS_8;
    config.g_input_bit_depth = 8;
    config.rc_end_usage = AOM_Q;
    config.rc_min_quantizer = 0;
    config.rc_max_quantizer = 63;
    if (options.kf_interval == 0) {
        config.kf_mode = AOM_KF_DISABLED;
    } else {
        config.kf_mode = AOM_KF_AUTO;
        config.kf_min_dist = options.kf_interval;
        config.kf_max_dist = options.kf_interval;
    }
    if (aom_codec_enc_init(&codec.context, iface, &config, 0) != AOM_CODEC_OK) {
        return aom_error(codec, "start");
    }
    codec.open = true;
    if (aom_codec_control(&codec.context, AOME_SET_CPUUSED, options.cpu_used) != AOM_CODEC_OK ||
        aom_codec_control(&codec.context, AOME_SET_CQ_LEVEL,
                          static_cast<unsigned int>(options.cq_level)) != AOM_CODEC_OK ||
        aom_codec_control(&codec.context, AV1E_SET_COLOR_PRIMARIES, AOM_CICP_CP_BT_709) !=
            AOM_CODEC_OK ||
        aom_codec_control(&codec.context, AV1E_SET_TRANSFER_CHARACTERISTICS, AOM_CICP_TC_BT_709) !=
            AOM_CODEC_OK ||
        aom_codec_control(&codec.context, AV1E_SET_MATRIX_COEFFICIENTS, AOM_CICP_MC_BT_709) !=
            AOM_CODEC_OK ||
        aom_codec_control(&codec.context, AV1E_SET_COLOR_RANGE, AOM_CR_STUDIO_RANGE) !=
            AOM_CODEC_OK) {
        return aom_error(codec, "take its settings");
    }

    // Each packet is a temporal unit that shows one frame; hidden frames ride with the next.
    std::vector<std::vector<std::uint8_t>> units;
    const auto take_packets = [&codec, &units] {
        bool any = false;
        aom_codec_iter_t iter = nullptr;
        while (const aom_codec_cx_pkt_t* packet = aom_codec_get_cx_data(&codec.context, &iter)) {
            if (packet->kind == AOM_CODEC_CX_FRAME_PKT) {
                const auto* bytes = static_cast<const std::uint8_t*>(packet->data.frame.buf);
                units.emplace_back(bytes, bytes + packet->data.frame.sz);
                any = true;
            }
        }
        return any;
    };
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Yuv420 yuv = to_yuv420(frames[i]);
        aom_image_t image{};
        if (aom_img_wrap(&image, AOM_IMG_FMT_I420, width, height, 1, yuv.y.data()) == nullptr) {
            return make_error(ErrorCode::Unknown, "libaom couldn't take a frame");
        }
        image.planes[AOM_PLANE_U] = yuv.u.data();
        image.planes[AOM_PLANE_V] = yuv.v.data();
        image.stride[AOM_PLANE_Y] = static_cast<int>(width);
        image.stride[AOM_PLANE_U] = static_cast<int>((width + 1) / 2);
        image.stride[AOM_PLANE_V] = static_cast<int>((width + 1) / 2);
        image.cp = AOM_CICP_CP_BT_709;
        image.tc = AOM_CICP_TC_BT_709;
        image.mc = AOM_CICP_MC_BT_709;
        image.range = AOM_CR_STUDIO_RANGE;
        if (aom_codec_encode(&codec.context, &image, static_cast<aom_codec_pts_t>(i), 1, 0) !=
            AOM_CODEC_OK) {
            return aom_error(codec, "encode frame " + std::to_string(i));
        }
        take_packets();
    }
    // Then the frames it held back to look ahead.
    do {
        if (aom_codec_encode(&codec.context, nullptr, 0, 0, 0) != AOM_CODEC_OK) {
            return aom_error(codec, "finish");
        }
    } while (take_packets());
    if (units.size() != frames.size()) {
        return make_error(ErrorCode::Unknown, "libaom wrote " + std::to_string(units.size()) +
                                                  " temporal units for " +
                                                  std::to_string(frames.size()) + " frames");
    }
    return write_ivf(width, height, rate, scale, units);
}

} // namespace slash::video
