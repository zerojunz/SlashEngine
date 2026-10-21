#include "slash/soundtrack/midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

constexpr int kDrumChannel = 9;
constexpr int kChannels = 16;
constexpr std::uint32_t kMaxTempo = 0xFFFFFF; // microseconds per quarter note, in three bytes
constexpr double kMicroseconds = 60.0e6;
constexpr std::uint8_t kMetaEvent = 0xFF;
constexpr std::uint8_t kTrackName = 0x03;
constexpr std::uint8_t kMarker = 0x06;
constexpr std::uint8_t kEndOfTrack = 0x2F;
constexpr std::uint8_t kTempo = 0x51;
constexpr std::uint8_t kTimeSignature = 0x58;
constexpr std::uint8_t kKeySignature = 0x59;
constexpr int kVolumeController = 7;
constexpr int kPanController = 10;
constexpr int kSustainController = 64;
constexpr int kBendCenter = 8192;
constexpr std::size_t kMaxName = 64;
constexpr int kMaxBars = 1024;
constexpr std::size_t kMaxTracks = 64;

void put_u16(std::string& out, std::uint32_t value) {
    out += static_cast<char>((value >> 8U) & 0xFFU);
    out += static_cast<char>(value & 0xFFU);
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, value >> 16U);
    put_u16(out, value & 0xFFFFU);
}

// A variable-length quantity: seven bits a byte, most significant first.
void put_vlq(std::string& out, std::uint32_t value) {
    std::uint8_t bytes[5];
    int count = 0;
    do {
        bytes[count++] = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
    } while (value != 0 && count < 5);
    while (count > 0) {
        --count;
        out += static_cast<char>(bytes[count] | (count > 0 ? 0x80U : 0U));
    }
}

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int value : values) {
        out += static_cast<char>(value);
    }
    return out;
}

std::string meta(std::uint8_t type, std::string_view data) {
    std::string out = bytes({kMetaEvent, type});
    put_vlq(out, static_cast<std::uint32_t>(data.size()));
    out += data;
    return out;
}

struct Event {
    std::int64_t tick = 0;
    int order = 0; // at the same tick: settings, then notes ending, then notes starting
    std::string data;
};

std::string track_chunk(std::vector<Event> events, std::int64_t end) {
    std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::string body;
    std::int64_t tick = 0;
    for (const Event& event : events) {
        put_vlq(body, static_cast<std::uint32_t>(event.tick - tick));
        body += event.data;
        tick = event.tick;
    }
    put_vlq(body, static_cast<std::uint32_t>(std::max(end, tick) - tick));
    body += meta(kEndOfTrack, "");
    std::string chunk = "MTrk";
    put_u32(chunk, static_cast<std::uint32_t>(body.size()));
    return chunk + body;
}

int volume_controller(double volume_db) {
    return std::clamp(static_cast<int>(std::lround(127.0 * std::pow(10.0, volume_db / 40.0))), 0,
                      127);
}

// Pan as controller 10: 64 is the middle and each step a 64th of a side; the right edge is 127.
int pan_controller(double pan) {
    return std::clamp(static_cast<int>(std::lround(64.0 + (pan * 64.0))), 0, 127);
}

int log2_of(int value) {
    int bits = 0;
    while ((1 << bits) < value) {
        ++bits;
    }
    return bits;
}

// Reads a Standard MIDI File's bytes; after a read past the end, every read gives 0 and failed
// is set.
class Bytes {
public:
    explicit Bytes(std::string_view data) : data_(data) {}

    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] std::size_t at() const { return at_; }
    [[nodiscard]] bool done() const { return at_ >= data_.size(); }

    std::uint32_t u8() {
        if (at_ >= data_.size()) {
            failed_ = true;
            return 0;
        }
        return static_cast<std::uint8_t>(data_[at_++]);
    }
    [[nodiscard]] std::uint32_t peek() const {
        return at_ < data_.size() ? static_cast<std::uint8_t>(data_[at_]) : 0U;
    }
    std::uint32_t u16() {
        const std::uint32_t high = u8();
        return (high << 8U) | u8();
    }
    std::uint32_t u32() {
        const std::uint32_t high = u16();
        return (high << 16U) | u16();
    }
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t byte = u8();
            value = (value << 7U) | (byte & 0x7FU);
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        failed_ = true;
        return value;
    }
    std::string_view take(std::size_t count) {
        if (count > data_.size() - at_) {
            failed_ = true;
            at_ = data_.size();
            return {};
        }
        const std::string_view out = data_.substr(at_, count);
        at_ += count;
        return out;
    }

private:
    std::string_view data_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

struct RawNote {
    std::int64_t start = 0;
    std::int64_t end = 0;
    int pitch = 0;
    int velocity = 0;
};

// The notes one MIDI track plays on one channel, and the channel's first settings.
struct Part {
    std::string name;
    int program = -1;
    int volume = -1;
    int pan = -1;
    std::vector<RawNote> notes;
};

// What a file says for all its tracks: each tempo, time signature and key signature with its
// tick (a tempo in microseconds per quarter note, a meter as numerator * 100 + denominator, a key
// as sharps * 2 + minor), and the markers.
struct Conductor {
    std::vector<std::pair<std::int64_t, std::int64_t>> tempos;
    std::vector<std::pair<std::int64_t, std::int64_t>> meters;
    std::vector<std::pair<std::int64_t, std::int64_t>> keys;
    std::vector<std::pair<std::int64_t, std::string>> markers;
    int bends = 0;
    int sustains = 0;
    std::set<int> drum_kits;
};

std::string clean_name(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
            out += c;
        }
    }
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    out = out.substr(first, out.find_last_not_of(' ') - first + 1);
    if (out.size() > kMaxName) {
        // Cut on a character boundary.
        std::size_t cut = kMaxName;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

// name, or name 2, name 3 and so on until it is one not yet taken.
std::string unique_name(const std::string& name, std::set<std::string>& taken) {
    std::string candidate = name;
    for (int n = 2; !taken.insert(candidate).second; ++n) {
        const std::string suffix = " " + std::to_string(n);
        candidate = name.substr(0, std::min(name.size(), kMaxName - suffix.size())) + suffix;
    }
    return candidate;
}

std::string plural(int count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

// Reads one MTrk chunk's events into parts, keyed by track and channel.
Result<std::int64_t> read_track(std::string_view chunk, int index,
                                std::map<std::pair<int, int>, Part>& parts, Conductor& conductor) {
    Bytes in(chunk);
    std::int64_t tick = 0;
    std::uint32_t running = 0;
    std::string name;
    // Notes sounding on each channel and pitch, oldest first.
    std::map<std::pair<int, int>, std::deque<std::pair<std::int64_t, int>>> sounding;
    std::vector<std::pair<int, RawNote>> notes;
    const auto end_note = [&](int channel, int pitch, std::int64_t at) {
        auto found = sounding.find({channel, pitch});
        if (found == sounding.end() || found->second.empty()) {
            return;
        }
        const auto [start, velocity] = found->second.front();
        found->second.pop_front();
        notes.push_back({channel, {start, std::max(at, start + 1), pitch, velocity}});
    };
    bool ended = false;
    while (!ended && !in.done()) {
        tick += in.vlq();
        std::uint32_t status = in.peek();
        if (status < 0x80U) {
            if (running == 0) {
                return make_error(ErrorCode::InvalidArgument, "track " + std::to_string(index) +
                                                                  " has data with no status byte");
            }
            status = running;
        } else {
            (void)in.u8();
        }
        if (status == kMetaEvent) {
            const std::uint32_t type = in.u8();
            const std::string_view data = in.take(in.vlq());
            if (type == kEndOfTrack) {
                ended = true;
            } else if (type == kTrackName && name.empty()) {
                name = clean_name(data);
            } else if (type == kMarker) {
                conductor.markers.emplace_back(tick, clean_name(data));
            } else if (type == kTempo && data.size() == 3) {
                const std::uint32_t tempo =
                    (std::uint32_t{static_cast<std::uint8_t>(data[0])} << 16U) |
                    (std::uint32_t{static_cast<std::uint8_t>(data[1])} << 8U) |
                    std::uint32_t{static_cast<std::uint8_t>(data[2])};
                conductor.tempos.emplace_back(tick, std::max<std::uint32_t>(tempo, 1));
            } else if (type == kTimeSignature && data.size() >= 2) {
                const int numerator = static_cast<std::uint8_t>(data[0]);
                const int denominator = 1 << std::min<int>(static_cast<std::uint8_t>(data[1]), 6);
                conductor.meters.emplace_back(tick, (numerator * 100) + denominator);
            } else if (type == kKeySignature && data.size() == 2) {
                const int sharps = std::clamp<int>(static_cast<std::int8_t>(data[0]), -7, 7);
                conductor.keys.emplace_back(tick, (sharps * 2) + (data[1] != 0 ? 1 : 0));
            }
            continue;
        }
        if (status == 0xF0U || status == 0xF7U) {
            (void)in.take(in.vlq());
            continue;
        }
        running = status;
        const int channel = static_cast<int>(status & 0x0FU);
        Part& part = parts[{index, channel}];
        switch (status & 0xF0U) {
        case 0x80U: {
            const auto pitch = static_cast<int>(in.u8() & 0x7FU);
            (void)in.u8();
            end_note(channel, pitch, tick);
            break;
        }
        case 0x90U: {
            const auto pitch = static_cast<int>(in.u8() & 0x7FU);
            const auto velocity = static_cast<int>(in.u8() & 0x7FU);
            if (velocity == 0) {
                end_note(channel, pitch, tick);
            } else {
                sounding[{channel, pitch}].emplace_back(tick, velocity);
            }
            break;
        }
        case 0xB0U: {
            const auto controller = static_cast<int>(in.u8() & 0x7FU);
            const auto value = static_cast<int>(in.u8() & 0x7FU);
            if (controller == kVolumeController && part.volume < 0) {
                part.volume = value;
            } else if (controller == kPanController && part.pan < 0) {
                part.pan = value;
            } else if (controller == kSustainController && value >= 64) {
                ++conductor.sustains;
            }
            break;
        }
        case 0xC0U: {
            const auto program = static_cast<int>(in.u8() & 0x7FU);
            if (part.program < 0) {
                part.program = program;
            }
            if (channel == kDrumChannel) {
                conductor.drum_kits.insert(program);
            }
            break;
        }
        case 0xE0U: {
            const std::uint32_t low = in.u8() & 0x7FU;
            const std::uint32_t high = in.u8() & 0x7FU;
            if (static_cast<int>((high << 7U) | low) != kBendCenter) {
                ++conductor.bends;
            }
            break;
        }
        case 0xD0U:
            (void)in.u8();
            break;
        default: // 0xA0, key pressure
            (void)in.u8();
            (void)in.u8();
            break;
        }
    }
    if (in.failed()) {
        return make_error(ErrorCode::InvalidArgument,
                          "track " + std::to_string(index) + " is cut short");
    }
    // Notes still sounding at the end of the track end there.
    for (auto& [key, queue] : sounding) {
        while (!queue.empty()) {
            end_note(key.first, key.second, tick);
        }
    }
    for (auto& [channel, note] : notes) {
        parts[{index, channel}].notes.push_back(note);
    }
    for (auto& [key, part] : parts) {
        if (key.first == index) {
            part.name = name;
        }
    }
    return tick;
}

} // namespace

Result<std::string> write_midi(const Score& score) {
    const double ticks_per_beat = kMidiTicksPerQuarter * 4.0 / score.beat_unit;
    const auto tick = [&](double beat) -> std::int64_t {
        return std::llround(beat * ticks_per_beat);
    };
    const std::int64_t end = tick(score.beats());
    const auto melodic =
        std::ranges::count_if(score.tracks, [](const Track& t) { return !t.drums; });
    if (melodic > kChannels - 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "has " + std::to_string(melodic) +
                              " tracks that aren't drums; MIDI has 15 channels for them");
    }
    const double tempo = kMicroseconds / score.bpm * score.beat_unit / 4.0;
    if (tempo > kMaxTempo) {
        return make_error(ErrorCode::InvalidArgument,
                          "is slower than MIDI can say; raise bpm or beat_unit");
    }
    std::string out = "MThd";
    put_u32(out, 6);
    put_u16(out, 1);
    put_u16(out, static_cast<std::uint32_t>(score.tracks.size() + 1));
    put_u16(out, kMidiTicksPerQuarter);

    std::vector<Event> conductor;
    conductor.push_back(
        {0, 0, meta(kTimeSignature, bytes({score.bar_beats, log2_of(score.beat_unit), 24, 8}))});
    const auto microseconds = static_cast<std::uint32_t>(std::lround(tempo));
    conductor.push_back({0, 0,
                         meta(kTempo, bytes({static_cast<int>((microseconds >> 16U) & 0xFFU),
                                             static_cast<int>((microseconds >> 8U) & 0xFFU),
                                             static_cast<int>(microseconds & 0xFFU)}))});
    if (const auto key = parse_key(score.key); key) {
        conductor.push_back(
            {0, 0, meta(kKeySignature, bytes({key->sharps & 0xFF, key->minor ? 1 : 0}))});
    }
    for (const Section& section : score.sections) {
        conductor.push_back({tick(static_cast<double>(section.bar) * score.bar_beats), 0,
                             meta(kMarker, section.name)});
    }
    out += track_chunk(std::move(conductor), end);

    int next_channel = 0;
    for (const Track& track : score.tracks) {
        int channel = kDrumChannel;
        if (!track.drums) {
            channel = next_channel++;
            if (channel == kDrumChannel) {
                channel = next_channel++;
            }
        }
        std::vector<Event> events;
        events.push_back({0, 0, meta(kTrackName, track.name)});
        if (!track.drums) {
            events.push_back({0, 0, bytes({0xC0 | channel, track.program})});
        }
        events.push_back(
            {0, 0, bytes({0xB0 | channel, kVolumeController, volume_controller(track.volume_db)})});
        events.push_back(
            {0, 0, bytes({0xB0 | channel, kPanController, pan_controller(track.pan)})});
        // MIDI can't hold two of one pitch on a channel at once: a note ends where the next of
        // its pitch starts.
        std::map<int, std::vector<std::pair<std::int64_t, std::int64_t>>> by_pitch;
        std::map<int, std::vector<int>> velocities;
        for (const Note& note : track.notes) {
            const std::int64_t start = tick(note.beat);
            by_pitch[note.pitch].emplace_back(start,
                                              std::max(start + 1, tick(note.beat + note.length)));
            velocities[note.pitch].push_back(note.velocity);
        }
        for (auto& [pitch, spans] : by_pitch) {
            for (std::size_t i = 0; i < spans.size(); ++i) {
                std::int64_t stop = spans[i].second;
                if (i + 1 < spans.size()) {
                    stop = std::min(stop, std::max(spans[i].first + 1, spans[i + 1].first));
                }
                events.push_back(
                    {spans[i].first, 2, bytes({0x90 | channel, pitch, velocities[pitch][i]})});
                events.push_back({stop, 1, bytes({0x80 | channel, pitch, 64})});
            }
        }
        out += track_chunk(std::move(events), end);
    }
    return out;
}

Result<MidiImport> read_midi(std::string_view data, int grid) {
    Bytes in(data);
    if (data.size() < 14 || in.take(4) != "MThd") {
        return make_error(ErrorCode::InvalidArgument, "isn't a MIDI file: it has no MThd header");
    }
    const std::uint32_t header = in.u32();
    const std::uint32_t format = in.u16();
    (void)in.u16(); // the track count; the chunks say
    const std::uint32_t division = in.u16();
    (void)in.take(header - 6);
    if (format > 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "is a format 2 MIDI file, a set of separate patterns; save it as format "
                          "1 or 0");
    }
    if ((division & 0x8000U) != 0 || division == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "counts time in SMPTE frames; save it with ticks per beat");
    }
    std::map<std::pair<int, int>, Part> parts;
    Conductor conductor;
    std::int64_t last_tick = 0;
    int index = 0;
    while (!in.done()) {
        const std::string_view id = in.take(4);
        const std::uint32_t size = in.u32();
        const std::string_view chunk = in.take(size);
        if (in.failed()) {
            return make_error(ErrorCode::InvalidArgument, "is cut short");
        }
        if (id != "MTrk") {
            continue;
        }
        auto end = read_track(chunk, index, parts, conductor);
        if (!end) {
            return end.error();
        }
        last_tick = std::max(last_tick, end.value());
        ++index;
    }

    MidiImport out;
    Score& score = out.score;
    std::vector<std::string>& problems = out.problems;
    // The first of each setting, and how many times it changes after.
    const auto settle = [&](auto& events, std::int64_t fallback, std::string_view what) {
        std::ranges::stable_sort(events,
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
        int changes = 0;
        for (std::size_t i = 1; i < events.size(); ++i) {
            changes += events[i].second != events[i - 1].second ? 1 : 0;
        }
        if (changes > 0) {
            problems.push_back("the " + std::string(what) + " changes " +
                               plural(changes, "time", "times") + "; the score keeps the first");
        }
        return events.empty() ? fallback : events.front().second;
    };
    const std::int64_t tempo = settle(conductor.tempos, 500000, "tempo");
    const std::int64_t meter = settle(conductor.meters, 404, "time signature");
    const bool keyed = !conductor.keys.empty();
    const std::int64_t key = settle(conductor.keys, 0, "key");
    const auto sharps = static_cast<int>(key >= 0 ? key / 2 : -((-key + 1) / 2));
    const bool minor = (key - (std::int64_t{sharps} * 2)) != 0;
    score.bar_beats = static_cast<int>(meter / 100);
    score.beat_unit = static_cast<int>(meter % 100);
    if (score.beat_unit != 2 && score.beat_unit != 4 && score.beat_unit != 8 &&
        score.beat_unit != 16) {
        problems.push_back("its time signature's beat, 1/" + std::to_string(score.beat_unit) +
                           ", became a quarter note");
        score.beat_unit = 4;
    }
    if (score.bar_beats < 2 || score.bar_beats > 32) {
        problems.push_back("its bars of " + std::to_string(score.bar_beats) +
                           " beats became bars of 4");
        score.bar_beats = 4;
    }
    const double ticks_per_beat = division * 4.0 / score.beat_unit;
    const double quarter_bpm = kMicroseconds / static_cast<double>(tempo);
    score.bpm =
        std::clamp(std::round(quarter_bpm * score.beat_unit / 4.0 * 100.0) / 100.0, 1.0, 400.0);
    if (keyed) {
        score.key = key_name(sharps, minor);
    }
    const bool flats = sharps < 0;
    if (conductor.bends > 0) {
        problems.push_back(plural(conductor.bends, "pitch bend was", "pitch bends were") +
                           " left out");
    }
    if (conductor.sustains > 0) {
        problems.push_back("the sustain pedal was left out (pressed " +
                           plural(conductor.sustains, "time", "times") + ")");
    }
    if (std::ranges::any_of(conductor.drum_kits, [](int kit) { return kit != 0; })) {
        problems.emplace_back("drum kits became General MIDI's standard drums");
    }

    std::set<std::string> names;
    int moved = 0;
    std::size_t longest = 0;
    int ordinal = 0;
    for (auto& [id, part] : parts) {
        if (part.notes.empty()) {
            continue;
        }
        ++ordinal;
        if (score.tracks.size() == kMaxTracks) {
            problems.emplace_back("the score holds 64 tracks; the rest were left out");
            break;
        }
        Track track;
        track.drums = id.second == kDrumChannel;
        track.name =
            unique_name(part.name.empty() ? "track " + std::to_string(ordinal) : part.name, names);
        track.program = track.drums ? 0 : std::max(part.program, 0);
        track.instrument =
            track.drums ? std::string("drums")
                        : std::string(instrument_names()[static_cast<std::size_t>(track.program)]);
        if (part.volume >= 0) {
            track.volume_db =
                part.volume == 0
                    ? -80.0
                    : std::max(-80.0, std::round(400.0 * std::log10(part.volume / 127.0)) / 10.0);
        }
        if (part.pan >= 0) {
            const double pan = part.pan == 127 ? 1.0 : (part.pan - 64) / 64.0;
            track.pan = std::round(pan * 100.0) / 100.0;
        }
        std::ranges::sort(part.notes, [](const RawNote& a, const RawNote& b) {
            return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
        });
        std::map<int, int> counts;
        for (const RawNote& raw : part.notes) {
            const Note note{.beat = static_cast<double>(raw.start) / ticks_per_beat,
                            .length = static_cast<double>(raw.end - raw.start) / ticks_per_beat,
                            .pitch = raw.pitch,
                            .velocity = std::max(raw.velocity, 1)};
            const auto off_grid = [&](double beats) {
                return std::abs((beats * grid) - std::round(beats * grid)) > 0.01;
            };
            if (off_grid(note.beat) || off_grid(note.length) || note.length * grid < 0.5) {
                ++moved;
            }
            ++counts[note.velocity];
            track.notes.push_back(note);
        }
        track.velocity = std::ranges::max_element(counts, [](const auto& a, const auto& b) {
                             return a.second < b.second;
                         })->first;
        track.bars =
            encode_bars(track.notes, score.bar_beats, track.velocity, track.drums, flats, grid);
        longest = std::max(longest, track.bars.size());
        out.notes += static_cast<int>(track.notes.size());
        score.tracks.push_back(std::move(track));
    }
    if (score.tracks.empty()) {
        return make_error(ErrorCode::InvalidArgument, "has no notes");
    }
    if (moved > 0) {
        problems.push_back(plural(moved, "note", "notes") + " moved to the nearest 1/" +
                           std::to_string(grid) + " of a beat");
    }
    const std::int64_t bar_units = static_cast<std::int64_t>(score.bar_beats) * grid;
    const std::int64_t end_units =
        std::llround(static_cast<double>(last_tick) / ticks_per_beat * grid);
    score.bars = std::max<int>(static_cast<int>(longest),
                               static_cast<int>((end_units + bar_units - 1) / bar_units));
    score.bars = std::max(score.bars, 1);
    if (score.bars > kMaxBars) {
        return make_error(ErrorCode::InvalidArgument, "is " + std::to_string(score.bars) +
                                                          " bars long; a score holds " +
                                                          std::to_string(kMaxBars));
    }
    std::ranges::stable_sort(conductor.markers,
                             [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::string> section_names;
    for (const auto& [at, written] : conductor.markers) {
        const std::int64_t units = std::llround(static_cast<double>(at) / ticks_per_beat * grid);
        const auto bar = static_cast<int>(units / bar_units);
        const std::string name = written.empty() ? "section " + std::to_string(bar) : written;
        if (bar >= score.bars) {
            problems.push_back("marker \"" + name + "\" is after the end; it was left out");
            continue;
        }
        if (!score.sections.empty() && score.sections.back().bar >= bar) {
            problems.push_back("marker \"" + name + "\" is in the same bar as \"" +
                               score.sections.back().name + "\"; it was left out");
            continue;
        }
        if (units % bar_units != 0) {
            problems.push_back("marker \"" + name +
                               "\" isn't on a bar line; its section starts "
                               "at bar " +
                               std::to_string(bar));
        }
        score.sections.push_back({unique_name(name, section_names), bar});
    }
    return out;
}

} // namespace slash::soundtrack
