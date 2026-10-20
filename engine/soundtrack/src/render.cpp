#include "slash/soundtrack/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tsf.h>
#include <vector>
#include <vorbis/vorbisenc.h>

namespace slash::soundtrack {
namespace {

struct TsfClose {
    void operator()(tsf* synth) const { tsf_close(synth); }
};
using Synth = std::unique_ptr<tsf, TsfClose>;

// Frames rendered between events at most.
constexpr int kBlock = 4096;

struct Event {
    std::int64_t frame;
    bool on;
    int pitch;
    int velocity;
};

void append(std::string& out, const ogg_page& page) {
    out.append(reinterpret_cast<const char*>(page.header),
               static_cast<std::size_t>(page.header_len));
    out.append(reinterpret_cast<const char*>(page.body), static_cast<std::size_t>(page.body_len));
}

float clipped(float sample) {
    return std::clamp(sample, -1.0F, 1.0F);
}

// Renders a synth from frame `from` to frame `to` onto the end of out.
void render(tsf* synth, std::vector<float>& out, std::int64_t from, std::int64_t to) {
    while (from < to) {
        const auto count = static_cast<int>(std::min<std::int64_t>(to - from, kBlock));
        const std::size_t at = out.size();
        out.resize(at + (2 * static_cast<std::size_t>(count)));
        tsf_render_float(synth, &out[at], count, 0);
        from += count;
    }
}

Result<Stem> render_track(tsf* bank, const Score& score, const Track& track,
                          const RenderOptions& options) {
    const Synth synth(tsf_copy(bank));
    if (!synth) {
        return make_error(ErrorCode::PlatformFailure, "the synth could not start");
    }
    tsf_set_output(synth.get(), TSF_STEREO_INTERLEAVED, options.rate, 0.0F);
    if (tsf_channel_set_presetnumber(synth.get(), 0, track.program, track.drums ? 1 : 0) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "has no instrument for " + track.name + "'s " + track.instrument +
                              " (General MIDI program " + std::to_string(track.program) + ")");
    }
    tsf_channel_set_volume(synth.get(), 0,
                           static_cast<float>(std::pow(10.0, track.volume_db / 20.0)));
    tsf_channel_set_pan(synth.get(), 0, static_cast<float>((track.pan + 1.0) / 2.0));

    const double frames_per_beat = 60.0 / score.bpm * options.rate;
    const auto frame_of = [&](double beat) {
        return static_cast<std::int64_t>(std::llround(beat * frames_per_beat));
    };
    std::vector<Event> events;
    events.reserve(track.notes.size() * 2);
    for (const Note& note : track.notes) {
        events.push_back({frame_of(note.beat), true, note.pitch, note.velocity});
        events.push_back({frame_of(note.beat + note.length), false, note.pitch, 0});
    }
    // A note that ends where another of the same pitch starts ends first.
    std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
        return a.frame != b.frame ? a.frame < b.frame : (!a.on && b.on);
    });

    // The length the engine loops a sound after, beat_count beats at bpm, rounded down as it is.
    const auto length = static_cast<std::int64_t>(score.beats() * options.rate * 60.0 / score.bpm);
    const auto tail = static_cast<std::int64_t>(kMaxTail * options.rate);
    Stem stem{.track = track.name, .samples = {}};
    std::int64_t frame = 0;
    for (const Event& event : events) {
        render(synth.get(), stem.samples, frame, event.frame);
        frame = std::max(frame, event.frame);
        if (event.on) {
            tsf_channel_note_on(synth.get(), 0, event.pitch,
                                static_cast<float>(event.velocity) / 127.0F);
        } else {
            tsf_channel_note_off(synth.get(), 0, event.pitch);
        }
    }
    render(synth.get(), stem.samples, frame, length);
    frame = std::max(frame, length);
    // Ring on until every note has died away, for up to kMaxTail past the end.
    while (frame < length + tail && tsf_active_voice_count(synth.get()) > 0) {
        const std::int64_t to = std::min(frame + kBlock, length + tail);
        render(synth.get(), stem.samples, frame, to);
        frame = to;
    }
    if (options.loop && length > 0) {
        // What rang past the end goes onto the start, where it sounds as the loop comes round.
        const auto loop = static_cast<std::size_t>(length) * 2;
        for (std::size_t i = loop; i < stem.samples.size(); ++i) {
            stem.samples[i % loop] += stem.samples[i];
        }
        stem.samples.resize(loop);
    }
    return stem;
}

} // namespace

Result<std::vector<Stem>> render_score(const Score& score, std::string_view sound_font,
                                       const RenderOptions& options,
                                       std::span<const std::string> tracks) {
    const Synth bank(tsf_load_memory(sound_font.data(), static_cast<int>(sound_font.size())));
    if (!bank) {
        return make_error(ErrorCode::InvalidArgument, "isn't a SoundFont 2 file");
    }
    std::vector<Stem> stems;
    for (const Track& track : score.tracks) {
        if (!tracks.empty() && std::ranges::find(tracks, track.name) == tracks.end()) {
            continue;
        }
        auto stem = render_track(bank.get(), score, track, options);
        if (!stem) {
            return stem.error();
        }
        stems.push_back(std::move(stem).value());
    }
    return stems;
}

Result<std::string> encode_ogg(std::span<const float> stereo, int rate, double quality,
                               std::uint32_t serial) {
    vorbis_info info{};
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 2, rate, static_cast<float>(quality)) != 0) {
        vorbis_info_clear(&info);
        return make_error(ErrorCode::InvalidArgument,
                          "Vorbis can't encode at " + std::to_string(rate) + " frames a second");
    }
    vorbis_comment comment{};
    vorbis_comment_init(&comment);
    vorbis_comment_add_tag(&comment, "ENCODER", "SlashEngine");
    vorbis_dsp_state dsp{};
    vorbis_analysis_init(&dsp, &info);
    vorbis_block block{};
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream{};
    ogg_stream_init(&stream, static_cast<int>(serial & 0x7FFFFFFFU));

    std::string out;
    ogg_packet header{};
    ogg_packet comments{};
    ogg_packet codebooks{};
    vorbis_analysis_headerout(&dsp, &comment, &header, &comments, &codebooks);
    ogg_stream_packetin(&stream, &header);
    ogg_stream_packetin(&stream, &comments);
    ogg_stream_packetin(&stream, &codebooks);
    ogg_page page{};
    while (ogg_stream_flush(&stream, &page) != 0) {
        append(out, page);
    }

    const std::size_t frames = stereo.size() / 2;
    constexpr std::size_t kChunk = 1024;
    for (std::size_t at = 0;; at += kChunk) {
        const std::size_t count = at < frames ? std::min(kChunk, frames - at) : 0;
        if (count == 0) {
            vorbis_analysis_wrote(&dsp, 0);
        } else {
            float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(count));
            const std::span<float> left(buffer[0], count);
            const std::span<float> right(buffer[1], count);
            for (std::size_t i = 0; i < count; ++i) {
                left[i] = clipped(stereo[2 * (at + i)]);
                right[i] = clipped(stereo[(2 * (at + i)) + 1]);
            }
            vorbis_analysis_wrote(&dsp, static_cast<int>(count));
        }
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            ogg_packet packet{};
            while (vorbis_bitrate_flushpacket(&dsp, &packet) != 0) {
                ogg_stream_packetin(&stream, &packet);
                while (ogg_stream_pageout(&stream, &page) != 0) {
                    append(out, page);
                }
            }
        }
        if (count == 0) {
            break;
        }
    }
    while (ogg_stream_flush(&stream, &page) != 0) {
        append(out, page);
    }
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return out;
}

std::string encode_wav(std::span<const float> stereo, int rate) {
    std::string out;
    const auto u16 = [&](std::uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto u32 = [&](std::uint32_t value) {
        u16(value & 0xFFFFU);
        u16(value >> 16U);
    };
    const auto bytes = static_cast<std::uint32_t>(stereo.size() * 2);
    out.append("RIFF");
    u32(36 + bytes);
    out.append("WAVEfmt ");
    u32(16);
    u16(1); // PCM
    u16(2);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 4);
    u16(4);
    u16(16);
    out.append("data");
    u32(bytes);
    for (const float sample : stereo) {
        const auto value = static_cast<std::int16_t>(std::lround(clipped(sample) * 32767.0F));
        u16(static_cast<std::uint16_t>(value));
    }
    return out;
}

double peak_db(std::span<const float> samples) {
    float peak = 0.0F;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak > 0.0F ? 20.0 * std::log10(static_cast<double>(peak))
                       : -std::numeric_limits<double>::infinity();
}

} // namespace slash::soundtrack
