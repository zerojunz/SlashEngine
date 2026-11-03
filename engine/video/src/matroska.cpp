// WebM and Matroska files (RFC 9559), read for their AV1 video and audio tracks and written with
// PCM or Opus audio, as Godot's video players read the containers of their codecs.

#include "opus.hpp"

#include "slash/core/bits.hpp"
#include "slash/video/video.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <dav1d/dav1d.h>
#include <limits>
#include <optional>
#include <string>

namespace slash::video {
namespace {

// Element IDs, with their marker bits, as RFC 9559 writes them.
constexpr std::uint32_t kEbml = 0x1A45DFA3;
constexpr std::uint32_t kEbmlVersion = 0x4286;
constexpr std::uint32_t kEbmlReadVersion = 0x42F7;
constexpr std::uint32_t kEbmlMaxIdLength = 0x42F2;
constexpr std::uint32_t kEbmlMaxSizeLength = 0x42F3;
constexpr std::uint32_t kDocType = 0x4282;
constexpr std::uint32_t kDocTypeVersion = 0x4287;
constexpr std::uint32_t kDocTypeReadVersion = 0x4285;
constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kSeekHead = 0x114D9B74;
constexpr std::uint32_t kInfo = 0x1549A966;
constexpr std::uint32_t kTimestampScale = 0x2AD7B1;
constexpr std::uint32_t kDuration = 0x4489;
constexpr std::uint32_t kMuxingApp = 0x4D80;
constexpr std::uint32_t kWritingApp = 0x5741;
constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kTrackNumber = 0xD7;
constexpr std::uint32_t kTrackUid = 0x73C5;
constexpr std::uint32_t kTrackType = 0x83;
constexpr std::uint32_t kFlagLacing = 0x9C;
constexpr std::uint32_t kCodecId = 0x86;
constexpr std::uint32_t kCodecPrivate = 0x63A2;
constexpr std::uint32_t kCodecDelay = 0x56AA;
constexpr std::uint32_t kSeekPreRoll = 0x56BB;
constexpr std::uint32_t kName = 0x536E;
constexpr std::uint32_t kLanguage = 0x22B59C;
constexpr std::uint32_t kContentEncodings = 0x6D80;
constexpr std::uint32_t kVideo = 0xE0;
constexpr std::uint32_t kPixelWidth = 0xB0;
constexpr std::uint32_t kPixelHeight = 0xBA;
constexpr std::uint32_t kAudio = 0xE1;
constexpr std::uint32_t kSamplingFrequency = 0xB5;
constexpr std::uint32_t kChannels = 0x9F;
constexpr std::uint32_t kBitDepth = 0x6264;
constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kTimestamp = 0xE7;
constexpr std::uint32_t kSimpleBlock = 0xA3;
constexpr std::uint32_t kBlockGroup = 0xA0;
constexpr std::uint32_t kBlock = 0xA1;
constexpr std::uint32_t kDiscardPadding = 0x75A2;
constexpr std::uint32_t kCues = 0x1C53BB6B;
constexpr std::uint32_t kTags = 0x1254C367;
constexpr std::uint32_t kChapters = 0x1043A770;
constexpr std::uint32_t kAttachments = 0x1941A469;

constexpr std::uint64_t kTrackTypeVideo = 1;
constexpr std::uint64_t kTrackTypeAudio = 2;
constexpr std::uint64_t kDefaultTimestampScale = 1000000; // nanoseconds a tick: milliseconds
constexpr double kNanoseconds = 1e9;
constexpr std::uint64_t kUnknownSize = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kAv1cHeader = 4;
constexpr std::size_t kOpusHeadSize = 19;
constexpr int kObuSequenceHeader = 1;

// An element: its ID, and where its data starts and ends.
struct Element {
    std::uint32_t id = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    bool unknown_size = false;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }

    // The element at at, inside a parent ending at end. An unknown size runs to the parent's end.
    [[nodiscard]] Result<Element> element(std::size_t at, std::size_t end) const {
        if (at >= end || end > bytes_.size()) {
            return truncated(at);
        }
        const int id_length = std::countl_zero(bytes_[at]) + 1;
        if (bytes_[at] == 0 || id_length > 4 || at + static_cast<std::size_t>(id_length) > end) {
            return make_error(ErrorCode::InvalidArgument,
                              "the file has no valid element at byte " + std::to_string(at));
        }
        Element out;
        for (int i = 0; i < id_length; ++i) {
            out.id = (out.id << 8U) | bytes_[at + static_cast<std::size_t>(i)];
        }
        std::size_t size_at = at + static_cast<std::size_t>(id_length);
        const auto size = vint(size_at, end);
        if (!size) {
            return truncated(at);
        }
        out.start = size_at;
        if (*size == kUnknownSize) {
            out.unknown_size = true;
            out.end = end;
        } else if (*size > end - out.start) {
            return truncated(at);
        } else {
            out.end = out.start + static_cast<std::size_t>(*size);
        }
        return out;
    }

    // A variable-size integer without its marker, advancing at; kUnknownSize when every bit of
    // its value is set.
    [[nodiscard]] std::optional<std::uint64_t> vint(std::size_t& at, std::size_t end) const {
        if (at >= end) {
            return std::nullopt;
        }
        const int length = std::countl_zero(bytes_[at]) + 1;
        if (bytes_[at] == 0 || at + static_cast<std::size_t>(length) > end) {
            return std::nullopt;
        }
        std::uint64_t value = bytes_[at] & (0xFFU >> static_cast<unsigned>(length));
        bool all_ones = value == (0xFFU >> static_cast<unsigned>(length));
        for (int i = 1; i < length; ++i) {
            const std::uint8_t byte = bytes_[at + static_cast<std::size_t>(i)];
            value = (value << 8U) | byte;
            all_ones = all_ones && byte == 0xFF;
        }
        at += static_cast<std::size_t>(length);
        return all_ones ? kUnknownSize : value;
    }

    [[nodiscard]] std::uint64_t unsigned_value(const Element& element) const {
        std::uint64_t value = 0;
        for (std::size_t i = element.start; i < element.end && i < element.start + 8; ++i) {
            value = (value << 8U) | bytes_[i];
        }
        return value;
    }

    [[nodiscard]] std::int64_t signed_value(const Element& element) const {
        const std::size_t size = std::min<std::size_t>(element.end - element.start, 8);
        const std::uint64_t value = unsigned_value(element);
        if (size == 0 || size == 8) {
            return static_cast<std::int64_t>(value);
        }
        const unsigned shift = 64U - (8U * static_cast<unsigned>(size));
        return static_cast<std::int64_t>(value << shift) >> shift;
    }

    [[nodiscard]] double float_value(const Element& element) const {
        const std::uint64_t bits = unsigned_value(element);
        if (element.end - element.start == 4) {
            return slash::bit_cast<float>(static_cast<std::uint32_t>(bits));
        }
        return element.end - element.start == 8 ? slash::bit_cast<double>(bits) : 0.0;
    }

    [[nodiscard]] std::string string_value(const Element& element) const {
        std::string out(reinterpret_cast<const char*>(bytes_.data() + element.start),
                        element.end - element.start);
        // Strings may be padded with zeros.
        out.erase(std::find(out.begin(), out.end(), '\0'), out.end());
        return out;
    }

private:
    static Error truncated(std::size_t at) {
        return make_error(ErrorCode::InvalidArgument,
                          "the file is cut short at byte " + std::to_string(at));
    }

    std::span<const std::uint8_t> bytes_;
};

// Calls visit(child) for each child of an element of known size.
template <typename Visit>
Result<void> children(const Reader& reader, const Element& parent, Visit&& visit) {
    std::size_t at = parent.start;
    while (at < parent.end) {
        auto child = reader.element(at, parent.end);
        if (!child) {
            return child.error();
        }
        if (child.value().unknown_size) {
            return make_error(ErrorCode::InvalidArgument,
                              "an element at byte " + std::to_string(at) +
                                  " has an unknown size where one is needed");
        }
        if (auto visited = visit(child.value()); !visited) {
            return visited;
        }
        at = child.value().end;
    }
    return {};
}

struct Track {
    std::uint64_t number = 0;
    std::uint64_t type = 0;
    std::string codec;
    std::size_t private_offset = 0;
    std::size_t private_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double sample_rate = 0.0;
    std::uint64_t channels = 1;
    std::uint64_t bits = 0;
    std::string name;
    std::string language = "eng";
    bool encoded = false; // compressed or encrypted
};

Result<Track> read_track(const Reader& reader, const Element& entry) {
    Track track;
    auto read = children(reader, entry, [&](const Element& child) -> Result<void> {
        switch (child.id) {
        case kTrackNumber:
            track.number = reader.unsigned_value(child);
            break;
        case kTrackType:
            track.type = reader.unsigned_value(child);
            break;
        case kCodecId:
            track.codec = reader.string_value(child);
            break;
        case kCodecPrivate:
            track.private_offset = child.start;
            track.private_size = child.end - child.start;
            break;
        case kName:
            track.name = reader.string_value(child);
            break;
        case kLanguage:
            track.language = reader.string_value(child);
            break;
        case kContentEncodings:
            track.encoded = true;
            break;
        case kVideo:
            return children(reader, child, [&](const Element& field) -> Result<void> {
                if (field.id == kPixelWidth) {
                    track.width = static_cast<std::uint32_t>(reader.unsigned_value(field));
                } else if (field.id == kPixelHeight) {
                    track.height = static_cast<std::uint32_t>(reader.unsigned_value(field));
                }
                return {};
            });
        case kAudio:
            return children(reader, child, [&](const Element& field) -> Result<void> {
                if (field.id == kSamplingFrequency) {
                    track.sample_rate = reader.float_value(field);
                } else if (field.id == kChannels) {
                    track.channels = reader.unsigned_value(field);
                } else if (field.id == kBitDepth) {
                    track.bits = reader.unsigned_value(field);
                }
                return {};
            });
        default:
            break;
        }
        return {};
    });
    if (!read) {
        return read.error();
    }
    return track;
}

bool is_segment_level(std::uint32_t id) {
    return id == kCluster || id == kCues || id == kTags || id == kChapters || id == kAttachments ||
           id == kInfo || id == kTracks || id == kSeekHead;
}

struct Parse {
    std::uint64_t timestamp_scale = kDefaultTimestampScale;
    double duration_ticks = 0.0;
    std::vector<Track> tracks;
    const Track* video = nullptr;
    std::vector<std::int64_t> frame_ticks;
    VideoFile file;
    // The file's audio tracks by track number, as indices into file.audio.
    std::vector<std::pair<std::uint64_t, std::size_t>> audio_tracks;
};

// A block's frames: its track, its time relative to the cluster's, and each frame's bytes
// (more than one when it is laced).
Result<void> read_block(const Reader& reader, std::size_t start, std::size_t end,
                        std::int64_t cluster_ticks, Parse& parse,
                        AudioTrack::Packet** last_audio = nullptr) {
    std::size_t at = start;
    const auto number = reader.vint(at, end);
    if (!number || at + 3 > end) {
        return make_error(ErrorCode::InvalidArgument,
                          "the block at byte " + std::to_string(start) + " is cut short");
    }
    const auto bytes = reader.bytes();
    const auto relative =
        static_cast<std::int16_t>(static_cast<std::uint16_t>((bytes[at] << 8U) | bytes[at + 1]));
    const std::uint8_t flags = bytes[at + 2];
    at += 3;
    std::vector<std::size_t> sizes;
    const unsigned lacing = (flags >> 1U) & 0x3U;
    if (lacing == 0) {
        sizes.push_back(end - at);
    } else {
        if (at >= end) {
            return make_error(ErrorCode::InvalidArgument,
                              "the laced block at byte " + std::to_string(start) + " is cut short");
        }
        const std::size_t count = static_cast<std::size_t>(bytes[at++]) + 1;
        std::size_t known = 0;
        if (lacing == 1) {
            // Xiph lacing: each size but the last as bytes of 255 and a last byte below it.
            for (std::size_t i = 0; i + 1 < count; ++i) {
                std::size_t size = 0;
                std::uint8_t byte = 0xFF;
                while (byte == 0xFF) {
                    if (at >= end) {
                        return make_error(ErrorCode::InvalidArgument,
                                          "the block at byte " + std::to_string(start) +
                                              " has lace sizes past its end");
                    }
                    byte = bytes[at++];
                    size += byte;
                }
                sizes.push_back(size);
                known += size;
            }
        } else if (lacing == 3) {
            // EBML lacing: the first size, then each next one as a signed difference.
            std::int64_t size = 0;
            for (std::size_t i = 0; i + 1 < count; ++i) {
                const std::size_t before = at;
                const auto value = reader.vint(at, end);
                if (!value || *value == kUnknownSize) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "the block at byte " + std::to_string(start) +
                                          " has lace sizes past its end");
                }
                if (i == 0) {
                    size = static_cast<std::int64_t>(*value);
                } else {
                    const auto length = static_cast<unsigned>(at - before);
                    const std::int64_t bias = (std::int64_t{1} << (7U * length - 1U)) - 1;
                    size += static_cast<std::int64_t>(*value) - bias;
                }
                if (size < 0) {
                    return make_error(ErrorCode::InvalidArgument, "the block at byte " +
                                                                      std::to_string(start) +
                                                                      " has a negative lace size");
                }
                sizes.push_back(static_cast<std::size_t>(size));
                known += static_cast<std::size_t>(size);
            }
        } else {
            // Fixed lacing: equal frames.
            if ((end - at) % count != 0) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the block at byte " + std::to_string(start) +
                                      " doesn't split into equal frames");
            }
            sizes.assign(count - 1, (end - at) / count);
            known = (end - at) / count * (count - 1);
        }
        if (known > end - at) {
            return make_error(ErrorCode::InvalidArgument, "the block at byte " +
                                                              std::to_string(start) +
                                                              " has lace sizes past its end");
        }
        sizes.push_back(end - at - known);
    }

    const std::int64_t ticks = cluster_ticks + relative;
    if (parse.video != nullptr && *number == parse.video->number) {
        for (const std::size_t size : sizes) {
            VideoFile::Frame frame;
            frame.offset = at;
            frame.size = size;
            frame.key = is_key_frame(bytes.subspan(at, size));
            parse.file.frames.push_back(frame);
            parse.frame_ticks.push_back(ticks);
            at += size;
        }
        return {};
    }
    const auto audio = std::ranges::find(parse.audio_tracks, *number,
                                         &std::pair<std::uint64_t, std::size_t>::first);
    if (audio != parse.audio_tracks.end()) {
        auto& packets = parse.file.audio[audio->second].packets;
        for (const std::size_t size : sizes) {
            packets.push_back({.offset = at, .size = size});
            at += size;
        }
        if (last_audio != nullptr) {
            *last_audio = &packets.back();
        }
    }
    return {};
}

// A cluster's blocks; returns where it ends, which for an unknown size is the next element that
// can't be in a cluster.
Result<std::size_t> read_cluster(const Reader& reader, const Element& cluster, Parse& parse) {
    std::int64_t ticks = 0;
    std::size_t at = cluster.start;
    while (at < cluster.end) {
        auto child = reader.element(at, cluster.end);
        if (!child) {
            return child.error();
        }
        const Element& element = child.value();
        if (cluster.unknown_size && is_segment_level(element.id)) {
            return at;
        }
        if (element.unknown_size) {
            return make_error(ErrorCode::InvalidArgument, "an element in the cluster at byte " +
                                                              std::to_string(at) +
                                                              " has an unknown size");
        }
        if (element.id == kTimestamp) {
            ticks = static_cast<std::int64_t>(reader.unsigned_value(element));
        } else if (element.id == kSimpleBlock) {
            if (auto read = read_block(reader, element.start, element.end, ticks, parse); !read) {
                return read.error();
            }
        } else if (element.id == kBlockGroup) {
            // A block of audio may say how much of its end isn't sound (DiscardPadding).
            AudioTrack::Packet* packet = nullptr;
            std::int64_t padding = 0;
            auto read = children(reader, element, [&](const Element& field) -> Result<void> {
                if (field.id == kDiscardPadding) {
                    padding = reader.signed_value(field);
                }
                return field.id == kBlock
                           ? read_block(reader, field.start, field.end, ticks, parse, &packet)
                           : Result<void>{};
            });
            if (!read) {
                return read.error();
            }
            if (packet != nullptr && padding > 0) {
                packet->discard_padding = padding;
            }
        }
        at = element.end;
    }
    return cluster.end;
}

AudioTrack::Codec codec_of(const Track& track) {
    if (track.encoded) {
        return AudioTrack::Codec::Other;
    }
    if (track.codec == "A_PCM/INT/LIT") {
        return AudioTrack::Codec::Pcm;
    }
    if (track.codec == "A_PCM/FLOAT/IEEE") {
        return AudioTrack::Codec::Float;
    }
    return track.codec == "A_OPUS" ? AudioTrack::Codec::Opus : AudioTrack::Codec::Other;
}

Result<void> read_tracks(const Reader& reader, const Element& tracks, Parse& parse) {
    if (!parse.tracks.empty()) {
        return {};
    }
    auto read = children(reader, tracks, [&](const Element& entry) -> Result<void> {
        if (entry.id != kTrackEntry) {
            return {};
        }
        auto track = read_track(reader, entry);
        if (!track) {
            return track.error();
        }
        parse.tracks.push_back(std::move(track).value());
        return {};
    });
    if (!read) {
        return read;
    }
    for (const Track& track : parse.tracks) {
        if (track.type == kTrackTypeVideo && parse.video == nullptr) {
            if (track.codec != "V_AV1") {
                return make_error(ErrorCode::InvalidArgument,
                                  "the file's video is " + track.codec + ", not AV1 (V_AV1)");
            }
            if (track.encoded) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the file's video track is compressed or encrypted");
            }
            parse.video = &track;
        } else if (track.type == kTrackTypeAudio) {
            AudioTrack audio;
            audio.codec = codec_of(track);
            audio.codec_id = track.codec;
            audio.name = track.name;
            audio.language = track.language;
            audio.sample_rate = static_cast<int>(std::lround(track.sample_rate));
            audio.channels = static_cast<int>(track.channels);
            audio.bits = static_cast<int>(track.bits);
            audio.private_offset = track.private_offset;
            audio.private_size = track.private_size;
            if (audio.codec == AudioTrack::Codec::Opus && track.private_size >= kOpusHeadSize) {
                // OpusHead: its channel count at byte 9, the pre-skip at 10 and 11.
                const auto bytes = reader.bytes().subspan(track.private_offset);
                audio.channels = bytes[9];
                audio.pre_skip = bytes[10] | (bytes[11] << 8U);
                audio.sample_rate = 48000;
            }
            parse.audio_tracks.emplace_back(track.number, parse.file.audio.size());
            parse.file.audio.push_back(std::move(audio));
        }
    }
    return {};
}

} // namespace

Result<VideoFile> read_matroska(std::span<const std::uint8_t> bytes) {
    const Reader reader(bytes);
    auto header = reader.element(0, bytes.size());
    if (!header || header.value().id != kEbml) {
        return make_error(ErrorCode::InvalidArgument,
                          "not a WebM or Matroska file: it doesn't start with an EBML header");
    }
    std::string doc_type;
    auto read = children(reader, header.value(), [&](const Element& child) -> Result<void> {
        if (child.id == kDocType) {
            doc_type = reader.string_value(child);
        }
        return {};
    });
    if (!read) {
        return read.error();
    }
    if (doc_type != "webm" && doc_type != "matroska") {
        return make_error(ErrorCode::InvalidArgument,
                          "the EBML file is \"" + doc_type + "\", not WebM or Matroska");
    }
    std::size_t at = header.value().end;
    Element segment;
    while (true) {
        auto next = reader.element(at, bytes.size());
        if (!next) {
            if (at < bytes.size()) {
                return next.error();
            }
            return make_error(ErrorCode::InvalidArgument, "the file has no segment");
        }
        if (next.value().id == kSegment) {
            segment = next.value();
            break;
        }
        at = next.value().end;
    }

    Parse parse;
    at = segment.start;
    while (at < segment.end) {
        auto child = reader.element(at, segment.end);
        if (!child) {
            return child.error();
        }
        const Element& element = child.value();
        if (element.id == kCluster) {
            if (parse.tracks.empty()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the file's clusters come before its tracks");
            }
            auto end = read_cluster(reader, element, parse);
            if (!end) {
                return end.error();
            }
            at = end.value();
            continue;
        }
        if (element.unknown_size) {
            return make_error(ErrorCode::InvalidArgument,
                              "an element at byte " + std::to_string(at) +
                                  " has an unknown size where one is needed");
        }
        if (element.id == kInfo) {
            read = children(reader, element, [&](const Element& field) -> Result<void> {
                if (field.id == kTimestampScale) {
                    parse.timestamp_scale = reader.unsigned_value(field);
                } else if (field.id == kDuration) {
                    parse.duration_ticks = reader.float_value(field);
                }
                return {};
            });
        } else if (element.id == kTracks) {
            read = read_tracks(reader, element, parse);
        }
        if (!read) {
            return read.error();
        }
        at = element.end;
    }

    VideoFile& file = parse.file;
    if (parse.video == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "the file has no video track");
    }
    if (file.frames.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the file's video track has no frames");
    }
    if (!file.frames.front().key) {
        return make_error(ErrorCode::InvalidArgument,
                          "the file's first frame isn't a key frame, so it can't be decoded");
    }
    if (parse.timestamp_scale == 0 ||
        parse.timestamp_scale > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::InvalidArgument, "the file's timestamp scale isn't valid");
    }
    const std::int64_t first = parse.frame_ticks.front();
    for (std::size_t i = 0; i < file.frames.size(); ++i) {
        if (parse.frame_ticks[i] < first ||
            (i > 0 && parse.frame_ticks[i] < parse.frame_ticks[i - 1])) {
            return make_error(ErrorCode::InvalidArgument,
                              "frame " + std::to_string(i) +
                                  " of the file comes before the one before it");
        }
        file.frames[i].pts = static_cast<std::uint64_t>(parse.frame_ticks[i] - first);
    }
    file.width = parse.video->width;
    file.height = parse.video->height;
    file.rate = static_cast<std::uint32_t>(kNanoseconds);
    file.scale = static_cast<std::uint32_t>(parse.timestamp_scale);
    if (parse.duration_ticks > 0.0) {
        file.duration = (parse.duration_ticks - static_cast<double>(first)) *
                        static_cast<double>(parse.timestamp_scale) / kNanoseconds;
    }
    // av1C: four bytes of fields, then the sequence header OBUs decoding starts with.
    if (parse.video->private_size > kAv1cHeader) {
        const auto config = bytes.subspan(parse.video->private_offset + kAv1cHeader,
                                          parse.video->private_size - kAv1cHeader);
        file.config.assign(config.begin(), config.end());
    }
    file.data.assign(bytes.begin(), bytes.end());
    return std::move(parse.file);
}

namespace {

// Writes elements, each as its ID, its size as a variable-size integer and its data.
class Writer {
public:
    void id(std::uint32_t id) {
        int bytes = 1;
        if (id > 0xFFFFFF) {
            bytes = 4;
        } else if (id > 0xFFFF) {
            bytes = 3;
        } else if (id > 0xFF) {
            bytes = 2;
        }
        for (int i = bytes - 1; i >= 0; --i) {
            out_.push_back(
                static_cast<std::uint8_t>((id >> (8U * static_cast<unsigned>(i))) & 0xFFU));
        }
    }

    void size(std::uint64_t value) {
        int length = 1;
        while (length < 8 &&
               value >= (std::uint64_t{1} << (7U * static_cast<unsigned>(length))) - 1) {
            ++length;
        }
        const std::uint64_t marked =
            value | (std::uint64_t{1} << (7U * static_cast<unsigned>(length)));
        for (int i = length - 1; i >= 0; --i) {
            out_.push_back(
                static_cast<std::uint8_t>((marked >> (8U * static_cast<unsigned>(i))) & 0xFFU));
        }
    }

    void bytes(std::uint32_t element, std::span<const std::uint8_t> data) {
        id(element);
        size(data.size());
        out_.insert(out_.end(), data.begin(), data.end());
    }

    void unsigned_value(std::uint32_t element, std::uint64_t value) {
        std::vector<std::uint8_t> data;
        do {
            data.insert(data.begin(), static_cast<std::uint8_t>(value & 0xFFU));
            value >>= 8U;
        } while (value != 0);
        bytes(element, data);
    }

    // A signed integer in as few bytes as hold it, its top bit its sign.
    void signed_value(std::uint32_t element, std::int64_t value) {
        unsigned length = 1;
        while (length < 8) {
            const std::int64_t half = std::int64_t{1} << ((8U * length) - 1U);
            if (value >= -half && value < half) {
                break;
            }
            ++length;
        }
        const auto bits = static_cast<std::uint64_t>(value);
        std::vector<std::uint8_t> data;
        for (unsigned i = length; i > 0; --i) {
            data.push_back(static_cast<std::uint8_t>((bits >> (8U * (i - 1U))) & 0xFFU));
        }
        bytes(element, data);
    }

    void float_value(std::uint32_t element, double value) {
        const auto bits = slash::bit_cast<std::uint64_t>(value);
        std::vector<std::uint8_t> data;
        for (int i = 7; i >= 0; --i) {
            data.push_back(
                static_cast<std::uint8_t>((bits >> (8U * static_cast<unsigned>(i))) & 0xFFU));
        }
        bytes(element, data);
    }

    void string_value(std::uint32_t element, std::string_view text) {
        bytes(element, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    }

    void master(std::uint32_t element, const Writer& body) { bytes(element, body.out_); }

    [[nodiscard]] std::vector<std::uint8_t>& out() { return out_; }

private:
    std::vector<std::uint8_t> out_;
};

// The first sequence header OBU in a temporal unit, with its header.
std::span<const std::uint8_t> sequence_header_obu(std::span<const std::uint8_t> unit) {
    std::size_t at = 0;
    while (at < unit.size()) {
        const std::size_t start = at;
        const std::uint8_t header = unit[at++];
        const auto type = static_cast<int>((header >> 3U) & 0xFU);
        if ((header & 0x4U) != 0) {
            ++at;
        }
        std::uint64_t size = unit.size() - std::min(at, unit.size());
        if ((header & 0x2U) != 0) {
            size = 0;
            for (int i = 0; i < 8 && at < unit.size(); ++i) {
                const std::uint8_t byte = unit[at++];
                size |= static_cast<std::uint64_t>(byte & 0x7FU) << (7U * static_cast<unsigned>(i));
                if ((byte & 0x80U) == 0) {
                    break;
                }
            }
        }
        if (at > unit.size() || size > unit.size() - at) {
            return {};
        }
        at += static_cast<std::size_t>(size);
        if (type == kObuSequenceHeader) {
            return unit.subspan(start, at - start);
        }
    }
    return {};
}

// AV1's CodecPrivate (av1C): the sequence header's profile, level, tier and color format, then
// the sequence header OBU itself.
std::vector<std::uint8_t> av1_codec_private(std::span<const std::uint8_t> first_unit) {
    const std::span<const std::uint8_t> obu = sequence_header_obu(first_unit);
    Dav1dSequenceHeader header{};
    if (obu.empty() || dav1d_parse_sequence_header(&header, obu.data(), obu.size()) < 0) {
        return {};
    }
    const auto& point = header.operating_points[0];
    const int level = ((point.major_level - 2) * 4) + point.minor_level;
    const bool subsampled_x =
        header.layout == DAV1D_PIXEL_LAYOUT_I420 || header.layout == DAV1D_PIXEL_LAYOUT_I422;
    const bool subsampled_y = header.layout == DAV1D_PIXEL_LAYOUT_I420;
    std::vector<std::uint8_t> out{
        0x81, // marker and version 1
        static_cast<std::uint8_t>((static_cast<unsigned>(header.profile) << 5U) |
                                  (static_cast<unsigned>(level) & 0x1FU)),
        static_cast<std::uint8_t>(
            (static_cast<unsigned>(point.tier) << 7U) | ((header.hbd != 0 ? 1U : 0U) << 6U) |
            ((header.hbd == 2 ? 1U : 0U) << 5U) |
            ((header.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1U : 0U) << 4U) |
            ((subsampled_x ? 1U : 0U) << 3U) | ((subsampled_y ? 1U : 0U) << 2U) |
            (static_cast<unsigned>(header.chr) & 0x3U)),
        0x00, // no initial presentation delay
    };
    out.insert(out.end(), obu.begin(), obu.end());
    return out;
}

struct Block {
    std::int64_t milliseconds = 0;
    std::uint8_t track = 0;
    std::span<const std::uint8_t> data;
    bool key = true;
    std::int64_t discard_padding = 0; // nanoseconds at its end that aren't sound
};

} // namespace

std::vector<std::uint8_t> write_matroska(std::uint32_t width, std::uint32_t height,
                                         std::uint32_t rate, std::uint32_t scale,
                                         std::span<const std::vector<std::uint8_t>> frames,
                                         const PcmAudio* audio, const OpusAudio* opus) {
    constexpr std::int64_t kClusterMilliseconds = 5000;
    constexpr std::uint8_t kVideoTrack = 1;
    const bool pcm = audio != nullptr && audio->channels > 0 && audio->sample_rate > 0;
    constexpr std::uint8_t kAudioTrack = 2;
    const std::uint8_t opus_track = pcm ? kAudioTrack + 1 : kAudioTrack;
    const auto milliseconds_of = [&](std::uint64_t frame) {
        return static_cast<std::int64_t>((frame * scale * 1000U + (rate / 2U)) / rate);
    };

    Writer ebml;
    ebml.unsigned_value(kEbmlVersion, 1);
    ebml.unsigned_value(kEbmlReadVersion, 1);
    ebml.unsigned_value(kEbmlMaxIdLength, 4);
    ebml.unsigned_value(kEbmlMaxSizeLength, 8);
    ebml.string_value(kDocType, pcm ? "matroska" : "webm");
    ebml.unsigned_value(kDocTypeVersion, 4);
    ebml.unsigned_value(kDocTypeReadVersion, 2);

    Writer segment;
    Writer info;
    info.unsigned_value(kTimestampScale, kDefaultTimestampScale);
    info.string_value(kMuxingApp, "SlashEngine");
    info.string_value(kWritingApp, "SlashEngine");
    info.float_value(kDuration, static_cast<double>(milliseconds_of(frames.size())));
    segment.master(kInfo, info);

    Writer tracks;
    Writer video;
    video.unsigned_value(kTrackNumber, kVideoTrack);
    video.unsigned_value(kTrackUid, kVideoTrack);
    video.unsigned_value(kTrackType, kTrackTypeVideo);
    video.unsigned_value(kFlagLacing, 0);
    video.string_value(kCodecId, "V_AV1");
    if (!frames.empty()) {
        const std::vector<std::uint8_t> config = av1_codec_private(frames.front());
        if (!config.empty()) {
            video.bytes(kCodecPrivate, config);
        }
    }
    Writer picture;
    picture.unsigned_value(kPixelWidth, width);
    picture.unsigned_value(kPixelHeight, height);
    video.master(kVideo, picture);
    tracks.master(kTrackEntry, video);

    std::vector<Block> blocks;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        blocks.push_back({.milliseconds = milliseconds_of(i),
                          .track = kVideoTrack,
                          .data = frames[i],
                          .key = is_key_frame(frames[i])});
    }
    std::vector<std::uint8_t> samples;
    if (pcm) {
        Writer sound;
        sound.unsigned_value(kTrackNumber, kAudioTrack);
        sound.unsigned_value(kTrackUid, kAudioTrack);
        sound.unsigned_value(kTrackType, kTrackTypeAudio);
        sound.unsigned_value(kFlagLacing, 0);
        sound.string_value(kCodecId, "A_PCM/INT/LIT");
        Writer settings;
        settings.float_value(kSamplingFrequency, static_cast<double>(audio->sample_rate));
        settings.unsigned_value(kChannels, static_cast<std::uint64_t>(audio->channels));
        settings.unsigned_value(kBitDepth, 16);
        sound.master(kAudio, settings);
        tracks.master(kTrackEntry, sound);
        for (const std::int16_t sample : audio->samples) {
            const auto bits = static_cast<std::uint16_t>(sample);
            samples.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
            samples.push_back(static_cast<std::uint8_t>(bits >> 8U));
        }
        // Blocks of 20 milliseconds, each at the time of its first frame.
        const auto frame_bytes = static_cast<std::size_t>(audio->channels) * 2;
        const auto block_frames = static_cast<std::size_t>(std::max(audio->sample_rate / 50, 1));
        const std::size_t total = samples.size() / frame_bytes;
        for (std::size_t frame = 0; frame < total; frame += block_frames) {
            const std::size_t count = std::min(block_frames, total - frame);
            blocks.push_back({.milliseconds = static_cast<std::int64_t>(
                                  frame * 1000U / static_cast<std::size_t>(audio->sample_rate)),
                              .track = kAudioTrack,
                              .data = std::span<const std::uint8_t>(samples).subspan(
                                  frame * frame_bytes, count * frame_bytes),
                              .key = true});
        }
    }
    if (opus != nullptr && opus->channels > 0) {
        // As RFC 7845's Matroska mapping: the OpusHead, the pre-skip as the codec's delay and 80
        // milliseconds of pre-roll; each 20 millisecond packet a block at its first sample's
        // time, counted from before the pre-skip.
        constexpr std::uint64_t kOpusRate = 48000;
        constexpr std::uint64_t kPreRollNanoseconds = 80000000;
        constexpr std::size_t kPacketMilliseconds = 20;
        Writer sound;
        sound.unsigned_value(kTrackNumber, opus_track);
        sound.unsigned_value(kTrackUid, opus_track);
        sound.unsigned_value(kTrackType, kTrackTypeAudio);
        sound.unsigned_value(kFlagLacing, 0);
        sound.string_value(kCodecId, "A_OPUS");
        sound.bytes(kCodecPrivate, opus->head);
        sound.unsigned_value(kCodecDelay, static_cast<std::uint64_t>(opus->pre_skip) *
                                              static_cast<std::uint64_t>(kNanoseconds) / kOpusRate);
        sound.unsigned_value(kSeekPreRoll, kPreRollNanoseconds);
        Writer settings;
        settings.float_value(kSamplingFrequency, static_cast<double>(kOpusRate));
        settings.unsigned_value(kChannels, static_cast<std::uint64_t>(opus->channels));
        sound.master(kAudio, settings);
        tracks.master(kTrackEntry, sound);
        for (std::size_t i = 0; i < opus->packets.size(); ++i) {
            const bool last = i + 1 == opus->packets.size();
            blocks.push_back(
                {.milliseconds = static_cast<std::int64_t>(i * kPacketMilliseconds),
                 .track = opus_track,
                 .data = opus->packets[i],
                 .key = true,
                 .discard_padding = last ? static_cast<std::int64_t>(opus->end_padding) *
                                               static_cast<std::int64_t>(kNanoseconds) /
                                               static_cast<std::int64_t>(kOpusRate)
                                         : 0});
        }
    }
    segment.master(kTracks, tracks);
    std::ranges::stable_sort(blocks, [](const Block& a, const Block& b) {
        return a.milliseconds != b.milliseconds ? a.milliseconds < b.milliseconds
                                                : a.track < b.track;
    });

    // Clusters of about five seconds, each starting at a key frame where it can.
    std::size_t next = 0;
    while (next < blocks.size()) {
        const std::int64_t start = blocks[next].milliseconds;
        Writer cluster;
        cluster.unsigned_value(kTimestamp, static_cast<std::uint64_t>(start));
        while (next < blocks.size()) {
            const Block& block = blocks[next];
            const std::int64_t relative = block.milliseconds - start;
            const bool new_cluster =
                relative >= std::numeric_limits<std::int16_t>::max() ||
                (relative >= kClusterMilliseconds && block.track == kVideoTrack && block.key);
            if (new_cluster) {
                break;
            }
            std::vector<std::uint8_t> body{
                static_cast<std::uint8_t>(0x80U | block.track),
                static_cast<std::uint8_t>((static_cast<std::uint16_t>(relative) >> 8U) & 0xFFU),
                static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) & 0xFFU),
                static_cast<std::uint8_t>(block.key ? 0x80U : 0x00U)};
            body.insert(body.end(), block.data.begin(), block.data.end());
            if (block.discard_padding > 0) {
                // A Block in a BlockGroup has no key flag; its DiscardPadding trims its end.
                body[3] = 0;
                Writer group;
                group.bytes(kBlock, body);
                group.signed_value(kDiscardPadding, block.discard_padding);
                cluster.master(kBlockGroup, group);
            } else {
                cluster.bytes(kSimpleBlock, body);
            }
            ++next;
        }
        segment.master(kCluster, cluster);
    }

    Writer file;
    file.master(kEbml, ebml);
    file.master(kSegment, segment);
    return std::move(file.out());
}

Result<std::vector<std::uint8_t>> encode_matroska(std::span<const render::Image> frames,
                                                  std::uint32_t rate, std::uint32_t scale,
                                                  const PcmAudio* audio, const OpusAudio* opus) {
    if (frames.empty() || rate == 0 || scale == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a video needs frames and a frame rate above 0");
    }
    std::vector<std::vector<std::uint8_t>> units;
    for (const render::Image& frame : frames) {
        if (frame.width != frames.front().width || frame.height != frames.front().height) {
            return make_error(ErrorCode::InvalidArgument, "every frame must be the same size");
        }
        auto unit = encode_frame(to_yuv420(frame));
        if (!unit) {
            return unit.error();
        }
        units.push_back(std::move(unit).value());
    }
    return write_matroska(frames.front().width, frames.front().height, rate, scale, units, audio,
                          opus);
}

namespace {

// A PCM track's samples, from -1 to 1.
std::vector<float> read_pcm(const VideoFile& video, const AudioTrack& source) {
    std::vector<float> input;
    const auto bytes_per_sample = static_cast<std::size_t>(source.bits / 8);
    for (const AudioTrack::Packet& packet : source.packets) {
        const std::uint8_t* data = video.data.data() + packet.offset;
        for (std::size_t at = 0; at + bytes_per_sample <= packet.size; at += bytes_per_sample) {
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < bytes_per_sample; ++i) {
                bits |= static_cast<std::uint64_t>(data[at + i]) << (8U * i);
            }
            float value = 0.0F;
            if (source.codec == AudioTrack::Codec::Float) {
                value = source.bits == 32 ? slash::bit_cast<float>(static_cast<std::uint32_t>(bits))
                                          : static_cast<float>(slash::bit_cast<double>(bits));
            } else if (source.bits == 8) {
                value = (static_cast<float>(bits) - 128.0F) / 128.0F;
            } else {
                const unsigned shift = 64U - static_cast<unsigned>(source.bits);
                const auto signed_bits = static_cast<std::int64_t>(bits << shift) >> shift;
                value = static_cast<float>(signed_bits) /
                        static_cast<float>(std::int64_t{1} << (source.bits - 1));
            }
            input.push_back(value);
        }
    }
    return input;
}

} // namespace

Result<DecodedAudio> decode_audio(const VideoFile& video, std::size_t track) {
    if (track >= video.audio.size()) {
        return make_error(ErrorCode::NotFound,
                          "the video has " + std::to_string(video.audio.size()) +
                              " audio tracks, not " + std::to_string(track + 1));
    }
    const AudioTrack& source = video.audio[track];
    const bool opus = source.codec == AudioTrack::Codec::Opus;
    const bool pcm =
        source.codec == AudioTrack::Codec::Pcm &&
        (source.bits == 8 || source.bits == 16 || source.bits == 24 || source.bits == 32);
    const bool floats =
        source.codec == AudioTrack::Codec::Float && (source.bits == 32 || source.bits == 64);
    if (!opus && !pcm && !floats) {
        return make_error(ErrorCode::InvalidArgument,
                          "the video's audio is " + source.codec_id +
                              (source.bits != 0 && (source.codec == AudioTrack::Codec::Pcm ||
                                                    source.codec == AudioTrack::Codec::Float)
                                   ? " at " + std::to_string(source.bits) + " bits"
                                   : std::string()) +
                              ", which can't be played; videos play Opus (A_OPUS) and PCM "
                              "(A_PCM/INT/LIT or A_PCM/FLOAT/IEEE) audio");
    }
    if (source.channels < 1 || source.sample_rate < 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "the video's audio track has no channels or sample rate");
    }
    const auto channels = static_cast<std::size_t>(source.channels);
    std::vector<float> input;
    if (opus) {
        auto decoded = detail::decode_opus(video, source);
        if (!decoded) {
            return decoded.error();
        }
        input = std::move(decoded).value();
    } else {
        input = read_pcm(video, source);
    }
    DecodedAudio sound;
    sound.sample_rate = source.sample_rate;
    const std::size_t frames = input.size() / channels;
    if (channels <= 2) {
        sound.channels = source.channels;
        sound.samples = std::move(input);
        sound.samples.resize(frames * channels);
        return sound;
    }
    // More channels fold to stereo as ITU-R BS.775 does, in WAV's order of speaker pairs (front,
    // center and LFE, rear, side): the center and the surrounds at -3 dB into each side, the LFE
    // left out. Odd counts keep their first two.
    constexpr float kMinus3Db = 0.70710678F;
    sound.channels = 2;
    sound.samples.reserve(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float* in = input.data() + (frame * channels);
        float left = in[0];
        float right = in[1];
        if (channels % 2 == 0 && channels <= 8) {
            if (channels >= 6) {
                left += kMinus3Db * in[2];
                right += kMinus3Db * in[2];
            }
            for (std::size_t pair = channels >= 6 ? 2 : 1; pair < channels / 2; ++pair) {
                left += kMinus3Db * in[pair * 2];
                right += kMinus3Db * in[(pair * 2) + 1];
            }
        }
        sound.samples.push_back(left);
        sound.samples.push_back(right);
    }
    return sound;
}

} // namespace slash::video
