// soundtrack.check: what keeps a game's music from playing as planned, found without listening.
#include "internal.hpp"

#include "slash/asset/assets.hpp"
#include "slash/audio/analysis.hpp"
#include "slash/audio/audio.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/arrange.hpp"
#include "slash/soundtrack/score.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

using command::Reply;
using command::Session;
using storyboard::FileError;

constexpr std::string_view kScoreSuffix = ".score.json";
// How far a sound's length may be from its beats, in seconds.
constexpr double kBeatTolerance = 0.02;
// How sure a measure must be before it counts against what a file says.
constexpr double kTempoConfidence = 0.7;
constexpr double kKeyConfidence = 0.6;
// Keys this many steps apart round the circle of fifths clash when one cuts to the other.
constexpr int kKeyClashSteps = 3;
// A loudness target's tolerance when it gives none, in LU.
constexpr double kDefaultTolerance = 2.0;
// Pieces are measured by playing them at this rate, for their beats or length, at most
// kMostMeasured seconds, or kUnknownLength when the stream doesn't know its length.
constexpr int kMeasureRate = 44100;
constexpr double kMostMeasured = 120.0;
constexpr double kUnknownLength = 30.0;
constexpr int kMixBlock = 4096;
// Pairs a problem names before "and N more".
constexpr std::size_t kPairsNamed = 3;

// Whether a link names the path, or a folder the path is in.
bool covers(std::string_view link, std::string_view path) {
    while (link.ends_with('/')) {
        link.remove_suffix(1);
    }
    return path.starts_with(link) && (path.size() == link.size() || path[link.size()] == '/');
}

std::string number(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(value * scale) / scale;
    return json::write(rounded == std::floor(rounded)
                           ? json::Value(static_cast<std::int64_t>(rounded))
                           : json::Value(rounded));
}

std::vector<std::string> strings(const json::Value& entry, std::string_view field) {
    std::vector<std::string> out;
    if (const json::Value* list = entry.find(field); list != nullptr && list->is_array()) {
        for (const json::Value& item : list->as_array()) {
            if (item.is_string()) {
                out.push_back(item.as_string());
            }
        }
    }
    return out;
}

std::optional<double> number_of(const json::Value& entry, std::string_view field) {
    const json::Value* value = entry.find(field);
    return value != nullptr && value->is_number() ? std::optional(value->as_double())
                                                  : std::nullopt;
}

// "a to b, c to d and 3 more".
std::string pairs_text(const std::vector<std::string>& pairs) {
    std::string out;
    const std::size_t named = std::min(pairs.size(), kPairsNamed);
    for (std::size_t i = 0; i < named; ++i) {
        if (i > 0) {
            out += i + 1 == named && pairs.size() == named ? " and " : ", ";
        }
        out += pairs[i];
    }
    if (pairs.size() > named) {
        out += " and " + std::to_string(pairs.size() - named) + " more";
    }
    return out;
}

// Where a .audio.json file of a kind lists its streams, as a JSON Pointer's start.
std::string list_member(audio::StreamFile::Kind kind) {
    if (kind == audio::StreamFile::Kind::Playlist) {
        return "/playlist/";
    }
    return kind == audio::StreamFile::Kind::Synchronized ? "/synchronized/" : "/streams/";
}

// What playing a piece measured.
struct Measured {
    audio::Analysis analysis;
    bool ok = false;
};

struct NamedKey {
    std::string name; // as written, e.g. "E minor"
    Key key;
};

class Checker {
public:
    Checker(Session& session, json::Object entries, std::vector<std::string> paths)
        : session_(session), entries_(std::move(entries)), paths_(std::move(paths)) {}

    Reply run() && {
        auto files =
            platform::list_files(session_.options().project_root, [](std::string_view folder) {
                return folder != "build/" && folder != ".slash/";
            });
        if (!files) {
            return Reply::failure(files.error());
        }
        collect(files.value());
        cues();
        for (const std::string& path : sounds_) {
            sound(path, *files_.at(path));
        }
        for (const std::string& path : interactive_) {
            interactive(path, *files_.at(path));
        }
        loudness();

        json::Value result;
        result["checked"] = static_cast<std::int64_t>(files_.size());
        const auto list = [](std::vector<FileError>& found) {
            json::Array out;
            for (FileError& each : found) {
                json::Value item;
                item["file"] = std::move(each.file);
                item["path"] = std::move(each.path);
                item["message"] = std::move(each.message);
                out.push_back(std::move(item));
            }
            return out;
        };
        if (!errors_.empty()) {
            result["errors"] = list(errors_);
        }
        if (!warnings_.empty()) {
            result["warnings"] = list(warnings_);
        }
        return Reply::success(std::move(result));
    }

private:
    [[nodiscard]] bool wanted(std::string_view path) const {
        return paths_.empty() || std::ranges::any_of(paths_, [&](const std::string& each) {
                   return covers(each, path);
               });
    }

    [[nodiscard]] bool exists(std::string_view path) const {
        auto full = session_.resolve_path(path);
        return full && platform::exists(full.value());
    }

    void error(std::string file, std::string path, std::string message) {
        errors_.push_back(
            {.file = std::move(file), .path = std::move(path), .message = std::move(message)});
    }
    void warning(std::string file, std::string path, std::string message) {
        warnings_.push_back(
            {.file = std::move(file), .path = std::move(path), .message = std::move(message)});
    }

    // A .audio.json file's settings, loaded once; nullptr, with an error, if it doesn't load.
    const audio::StreamFile* stream_file(const std::string& path) {
        if (const auto found = files_.find(path); found != files_.end()) {
            return found->second;
        }
        if (failed_.contains(path)) {
            return nullptr;
        }
        auto loaded = session_.assets().load<audio::StreamFile>(path);
        if (!loaded) {
            failed_.insert(path);
            error(path, "", loaded.error().message);
            return nullptr;
        }
        handles_.push_back(loaded.value());
        return files_.emplace(path, handles_.back().get()).first->second;
    }

    // The music: interactive, playlist and synchronized .audio.json files, sounds with beats,
    // and the streams cues play, then every .audio.json file those play.
    void collect(const std::vector<std::string>& files) {
        std::vector<std::string> roots;
        for (const std::string& path : files) {
            if (!path.ends_with(audio::kStreamSuffix) || !wanted(path)) {
                continue;
            }
            auto read = session_.assets().load<audio::StreamFile>(path);
            if (!read) {
                continue; // not music, or validate's to report
            }
            const audio::StreamFile& file = *read.value();
            const bool music = file.kind == audio::StreamFile::Kind::Interactive ||
                               file.kind == audio::StreamFile::Kind::Playlist ||
                               file.kind == audio::StreamFile::Kind::Synchronized ||
                               (file.kind == audio::StreamFile::Kind::Sound && file.bpm > 0);
            if (music) {
                roots.push_back(path);
            }
        }
        for (const auto& [id, entry] : entries_) {
            if (!id.starts_with("cue/") || !strings(entry, "layers").empty()) {
                continue;
            }
            for (const std::string& file : strings(entry, "music")) {
                std::string stream = file.ends_with(kScoreSuffix)
                                         ? file.substr(0, file.size() - kScoreSuffix.size()) +
                                               std::string(audio::kStreamSuffix)
                                         : file;
                if (wanted(stream) && exists(stream)) {
                    roots.push_back(std::move(stream));
                }
            }
        }
        for (const std::string& root : roots) {
            walk(root);
        }
        // Pieces play whole: clips, playlist entries and music nothing else plays.
        for (const std::string& root : roots) {
            if (!referenced_.contains(root) &&
                (!files_.contains(root) ||
                 files_.at(root)->kind != audio::StreamFile::Kind::Interactive)) {
                pieces_.insert(root);
            }
        }
    }

    // A music file and what it plays. A file it names that doesn't exist is reported where it
    // is named.
    void walk(const std::string& path) {
        if (!path.ends_with(audio::kStreamSuffix) || !walked_.insert(path).second) {
            return;
        }
        const audio::StreamFile* file = stream_file(path);
        if (file == nullptr) {
            return;
        }
        using Kind = audio::StreamFile::Kind;
        switch (file->kind) {
        case Kind::Sound:
            if (!file->source.empty()) {
                sounds_.insert(path);
            }
            break;
        case Kind::Interactive:
            interactive_.insert(path);
            for (std::size_t i = 0; i < file->clips.size(); ++i) {
                const std::string& stream = file->clips[i].stream;
                if (named(path, "/clips/" + std::to_string(i) + "/stream", stream)) {
                    pieces_.insert(stream);
                    walk(stream);
                }
            }
            break;
        default:
            for (std::size_t i = 0; i < file->streams.size(); ++i) {
                const std::string& stream = file->streams[i].stream;
                if (named(path, list_member(file->kind) + std::to_string(i) + "/stream", stream)) {
                    if (file->kind == Kind::Playlist) {
                        pieces_.insert(stream);
                    }
                    walk(stream);
                }
            }
            break;
        }
    }

    // Records that file names stream at pointer: whether stream exists, with an error if not.
    bool named(const std::string& file, const std::string& pointer, const std::string& stream) {
        referenced_.insert(stream);
        if (exists(stream)) {
            return true;
        }
        missing_.insert(stream);
        error(file, pointer, stream + " doesn't exist");
        return false;
    }

    // Cues whose music can't play: soundtrack.arrange's problems.
    void cues() {
        const Arranged arranged =
            arrange(entries_, {}, [this](std::string_view path) { return exists(path); });
        for (const ArrangeProblem& problem : arranged.problems) {
            if (!paths_.empty()) {
                const json::Value& entry = entries_.at(problem.cue);
                const std::vector<std::string> music = strings(entry, "music");
                if (std::ranges::none_of(music,
                                         [&](const std::string& file) { return wanted(file); })) {
                    continue;
                }
            }
            const storyboard::Kind* kind = storyboard::kind_of(problem.cue);
            std::string file = kind != nullptr ? storyboard::file_path(kind->file) : std::string();
            std::string path = "/entries/" + json::pointer_escape(problem.cue);
            if (problem.missing) {
                error(std::move(file), std::move(path), problem.message);
            } else {
                warning(std::move(file), std::move(path), problem.message);
            }
        }
    }

    // A sound's beats against its length and its measured tempo, and whether its loop clicks.
    void sound(const std::string& path, const audio::StreamFile& file) {
        auto loaded = session_.assets().load<audio::Sound>(file.source);
        if (!loaded) {
            error(path, "/source", loaded.error().message);
            return;
        }
        const audio::Sound& sound = *loaded.value();
        auto frames = audio::decode_frames(sound);
        if (!frames) {
            error(path, "/source", file.source + ": " + frames.error().message);
            return;
        }
        const std::optional<audio::LoopPoints> loop = audio::loop_points(sound, &file);
        const audio::Analysis analysis = audio::analyze(frames.value(), sound.mix_rate, loop);
        const double seconds = sound.length();

        if (file.bpm > 0 && file.beat_count > 0) {
            const double beats = file.beat_count * 60.0 / file.bpm;
            const std::string beats_text = std::to_string(file.beat_count) + " beats at " +
                                           number(file.bpm, 2) + " bpm last " + number(beats, 2) +
                                           " s";
            const bool compressed = sound.format != audio::Sound::Format::Wav;
            if (beats > seconds + kBeatTolerance) {
                error(path, "/beat_count",
                      beats_text + ", longer than the sound's " + number(seconds, 2) +
                          " s, so the music's beats run past its end; the sound holds " +
                          std::to_string(static_cast<int>(seconds * file.bpm / 60.0)) + " beats");
            } else if (loop && compressed && seconds > beats + kBeatTolerance) {
                warning(path, "/beat_count",
                        "the loop goes back after " + beats_text +
                            ", leaving out the sound's last " + number(seconds - beats, 2) + " s");
            }
            if (loop && file.bar_beats > 0 && file.beat_count % file.bar_beats != 0) {
                warning(path, "/beat_count",
                        std::to_string(file.beat_count) + " beats isn't a whole number of " +
                            std::to_string(file.bar_beats) +
                            "-beat bars, so each time round the loop starts mid-bar");
            }
        }
        if (file.bpm > 0 && analysis.bpm && analysis.tempo_confidence >= kTempoConfidence) {
            const double ratio = *analysis.bpm / file.bpm;
            const bool fits = std::ranges::any_of(std::array{0.5, 1.0, 2.0}, [&](double whole) {
                return std::abs(ratio - whole) <= 0.03 * whole;
            });
            if (!fits) {
                warning(path, "/bpm",
                        "sounds like " + number(*analysis.bpm, 1) + " bpm, but bpm is " +
                            number(file.bpm, 2));
            }
        }
        if (analysis.loop && analysis.loop->clicks) {
            error(path, "", "the loop clicks where its end joins its start");
        }
    }

    // Interactive music: switches the beats can't time, switches off the bar and keys that clash.
    void interactive(const std::string& path, const audio::StreamFile& file) {
        const int count = static_cast<int>(file.clips.size());
        std::vector<std::shared_ptr<const audio::Stream>> streams(file.clips.size());
        for (int i = 0; i < count; ++i) {
            const audio::StreamFile::Clip& clip = file.clips[static_cast<std::size_t>(i)];
            if (missing_.contains(clip.stream)) {
                continue;
            }
            auto loaded = audio::load_stream(session_.assets(), clip.stream);
            if (!loaded) {
                error(path, "/clips/" + std::to_string(i) + "/stream", loaded.error().message);
                continue;
            }
            streams[static_cast<std::size_t>(i)] = loaded.value().stream;
        }
        const auto clip_name = [&](int i) { return file.clips[static_cast<std::size_t>(i)].name; };
        const auto bpm = [&](int i) {
            const auto& stream = streams[static_cast<std::size_t>(i)];
            return stream != nullptr ? stream->bpm() : 0.0;
        };
        const auto transition = [&](int index) {
            return index >= 0 ? file.transitions[static_cast<std::size_t>(index)]
                              : audio::StreamFile::Transition{};
        };
        const auto where = [](int index) {
            return index >= 0 ? "/transitions/" + std::to_string(index)
                              : std::string("/transitions");
        };

        std::map<int, std::vector<std::string>> off_bar;
        std::map<int, std::vector<std::string>> drifting;
        std::set<std::pair<int, int>> cuts;
        for (int from = 0; from < count; ++from) {
            if (streams[static_cast<std::size_t>(from)] == nullptr) {
                continue;
            }
            bool waits = false;
            for (int to = 0; to < count; ++to) {
                if (to == from || streams[static_cast<std::size_t>(to)] == nullptr) {
                    continue;
                }
                const int index = audio::transition_index(file.transitions, from, to);
                const audio::StreamFile::Transition chosen = transition(index);
                const audio::AutoAdvance advance =
                    file.clips[static_cast<std::size_t>(to)].auto_advance;
                waits = waits || chosen.from_time == audio::TransitionFrom::NextBeat ||
                        chosen.from_time == audio::TransitionFrom::NextBar;
                const std::string pair = clip_name(from) + " to " + clip_name(to);
                // A clip that goes on by itself is a sting, which may come in on any beat.
                if (bpm(from) > 0 && bpm(to) > 0 && advance == audio::AutoAdvance::Disabled &&
                    chosen.to_time != audio::TransitionTo::SamePosition &&
                    (chosen.from_time == audio::TransitionFrom::Immediate ||
                     chosen.from_time == audio::TransitionFrom::NextBeat)) {
                    off_bar[index].push_back(pair);
                }
                if (chosen.to_time == audio::TransitionTo::SamePosition && bpm(from) > 0 &&
                    bpm(to) > 0 && bpm(from) != bpm(to)) {
                    drifting[index].push_back(pair + " (" + number(bpm(from), 2) + " to " +
                                              number(bpm(to), 2) + " bpm)");
                }
                const int filler = chosen.filler_clip;
                if (filler >= 0 && filler != from && filler != to) {
                    cuts.emplace(std::min(from, filler), std::max(from, filler));
                    cuts.emplace(std::min(filler, to), std::max(filler, to));
                } else {
                    cuts.emplace(std::min(from, to), std::max(from, to));
                }
            }
            if (waits && bpm(from) == 0) {
                error(path, "/clips/" + std::to_string(from),
                      clip_name(from) +
                          "'s stream has no bpm, so switches from it that wait for the beat or "
                          "bar happen at once; give its .audio.json file bpm and beat_count");
            }
        }
        for (const auto& [index, pairs] : off_bar) {
            const bool now = transition(index).from_time == audio::TransitionFrom::Immediate;
            warning(
                path, where(index),
                (index >= 0 ? "switches from " : "with no transition for them, switches from ") +
                    pairs_text(pairs) + (now ? " come at once" : " come on the next beat") +
                    ", off the bar; wait for next_bar, or keep the place with same_position");
        }
        for (const auto& [index, pairs] : drifting) {
            warning(path, where(index),
                    "switches from " + pairs_text(pairs) +
                        " keep the same position though the beats differ, so the next clip comes "
                        "in off its beat");
        }
        for (const auto& [a, b] : cuts) {
            const std::optional<NamedKey> first =
                key_of(file.clips[static_cast<std::size_t>(a)].stream);
            const std::optional<NamedKey> second =
                key_of(file.clips[static_cast<std::size_t>(b)].stream);
            if (!first || !second) {
                continue;
            }
            const int apart = std::abs(first->key.sharps - second->key.sharps) % 12;
            const int steps = std::min(apart, 12 - apart);
            if (steps >= kKeyClashSteps) {
                warning(path, "/clips/" + std::to_string(b),
                        clip_name(a) + " (" + first->name + ") and " + clip_name(b) + " (" +
                            second->name + ") are " + std::to_string(steps) +
                            " steps apart round the circle of fifths, and the music goes "
                            "straight from one to the other; a filler clip, or a nearer key, "
                            "joins them");
            }
        }
    }

    // A stream's key: its score's (the score it was rendered from, or whose stems it is), its
    // layers', or failing those, the key it measures in.
    std::optional<NamedKey> key_of(const std::string& path) {
        if (const auto found = keys_.find(path); found != keys_.end()) {
            return found->second;
        }
        std::optional<NamedKey> out;
        const auto from_score = [&](const std::string& score) -> std::optional<NamedKey> {
            auto read = session_.read_project_file(score);
            if (!read || !read.value()) {
                return std::nullopt;
            }
            auto document = json::parse(*read.value());
            if (!document) {
                return std::nullopt;
            }
            const json::Value* key = document.value().find("key");
            if (key == nullptr || !key->is_string()) {
                return std::nullopt;
            }
            const std::optional<Key> parsed = parse_key(key->as_string());
            return parsed ? std::optional(NamedKey{.name = key->as_string(), .key = *parsed})
                          : std::nullopt;
        };
        if (path.ends_with(audio::kStreamSuffix)) {
            const std::string base = path.substr(0, path.size() - audio::kStreamSuffix.size());
            out = from_score(base + std::string(kScoreSuffix));
            if (const std::size_t slash = base.rfind('/'); !out && slash != std::string::npos) {
                out = from_score(base.substr(0, slash) + std::string(kScoreSuffix));
            }
            const audio::StreamFile* file = out ? nullptr : stream_file(path);
            if (file != nullptr && file->kind == audio::StreamFile::Kind::Synchronized) {
                for (const audio::StreamFile::Entry& entry : file->streams) {
                    out = key_of(entry.stream);
                    if (out) {
                        break;
                    }
                }
            }
        }
        if (!out) {
            const Measured& measured = measure(path);
            if (measured.ok && measured.analysis.key &&
                measured.analysis.key_confidence >= kKeyConfidence) {
                if (const std::optional<Key> parsed = parse_key(*measured.analysis.key)) {
                    out = NamedKey{.name = *measured.analysis.key, .key = *parsed};
                }
            }
        }
        keys_.emplace(path, out);
        return out;
    }

    // A piece played for its beats or its length, and measured.
    const Measured& measure(const std::string& path) {
        if (const auto found = measured_.find(path); found != measured_.end()) {
            return found->second;
        }
        Measured& out = measured_[path];
        auto loaded = audio::load_stream(session_.assets(), path);
        if (!loaded) {
            return out;
        }
        const audio::Stream& stream = *loaded.value().stream;
        double seconds = stream.bpm() > 0 && stream.beat_count() > 0
                             ? stream.beat_count() * 60.0 / stream.bpm()
                             : stream.length();
        if (seconds <= 0) {
            seconds = kUnknownLength;
        }
        const auto total =
            static_cast<std::size_t>(std::min(seconds, kMostMeasured) * kMeasureRate);
        std::vector<audio::Frame> frames(total);
        auto playback = stream.instantiate(kMeasureRate, 0);
        playback->start(0.0);
        std::size_t got = 0;
        while (got < total) {
            const int want = static_cast<int>(std::min<std::size_t>(kMixBlock, total - got));
            const int mixed = playback->mix(frames.data() + got, 1.0F, want);
            got += static_cast<std::size_t>(std::max(mixed, 0));
            if (mixed < want) {
                break;
            }
        }
        frames.resize(got);
        out.analysis = audio::analyze(frames, kMeasureRate);
        out.ok = true;
        return out;
    }

    // Each piece against the storyboard's loudness targets that cover it.
    void loudness() {
        for (const auto& [id, entry] : entries_) {
            if (!id.starts_with("loudness/")) {
                continue;
            }
            const std::vector<std::string> links = strings(entry, "links");
            const std::optional<double> lufs = number_of(entry, "lufs");
            const std::optional<double> true_peak = number_of(entry, "true_peak");
            const double tolerance = number_of(entry, "tolerance").value_or(kDefaultTolerance);
            for (const std::string& piece : pieces_) {
                if (!links.empty() && std::ranges::none_of(links, [&](const std::string& link) {
                        return covers(link, piece);
                    })) {
                    continue;
                }
                const Measured& measured = measure(piece);
                if (!measured.ok) {
                    continue;
                }
                const audio::Analysis& analysis = measured.analysis;
                if (lufs && !analysis.lufs) {
                    error(piece, "",
                          "too quiet to measure (below -70 LUFS); " + id + " asks for " +
                              number(*lufs, 1) + " LUFS");
                } else if (lufs && std::abs(*analysis.lufs - *lufs) > tolerance) {
                    const double change = *lufs - *analysis.lufs;
                    error(piece, "",
                          "measures " + number(*analysis.lufs, 1) + " LUFS; " + id + " asks for " +
                              number(*lufs, 1) + " ± " + number(tolerance, 1) + ", so " +
                              (change > 0 ? "raise" : "lower") + " it " +
                              number(std::abs(change), 1) + " dB");
                }
                if (true_peak && analysis.true_peak_db && *analysis.true_peak_db > *true_peak) {
                    error(piece, "",
                          "true peak " + number(*analysis.true_peak_db, 1) + " dBTP is above " +
                              id + "'s " + number(*true_peak, 1));
                }
            }
        }
    }

    Session& session_;
    json::Object entries_;
    std::vector<std::string> paths_;
    std::vector<asset::Handle<audio::StreamFile>> handles_;
    std::map<std::string, const audio::StreamFile*> files_;
    std::set<std::string> failed_;
    std::set<std::string> walked_;
    std::set<std::string> referenced_;
    std::set<std::string> missing_;
    std::set<std::string> sounds_;
    std::set<std::string> interactive_;
    std::set<std::string> pieces_;
    std::map<std::string, std::optional<NamedKey>> keys_;
    std::map<std::string, Measured> measured_;
    std::vector<FileError> errors_;
    std::vector<FileError> warnings_;
};

} // namespace

Reply soundtrack_check(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }
    std::vector<std::string> paths;
    if (const json::Value* named = params.find("paths"); named != nullptr) {
        for (const json::Value& path : named->as_array()) {
            paths.push_back(path.as_string());
        }
    }
    return Checker(session, std::move(entries), std::move(paths)).run();
}

} // namespace slash::soundtrack
