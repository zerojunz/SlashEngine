#include "slash/soundtrack/arrange.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

constexpr std::string_view kScoreSuffix = ".score.json";
constexpr std::string_view kAudioSuffix = ".audio.json";

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

std::string text_of(const json::Value& entry, std::string_view field) {
    const json::Value* value = entry.find(field);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

std::string name_of(std::string_view id) {
    return std::string(id.substr(id.find('/') + 1));
}

// Text as part of a clip name: lowercase letters, digits, '_' and '-', with the rest as '_'.
std::string slug(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        out.push_back(plain ? c : '_');
    }
    return out;
}

// The stream a music file plays as: a score as the music soundtrack.render makes of it.
std::string playable(const std::string& path) {
    if (path.ends_with(kScoreSuffix)) {
        return path.substr(0, path.size() - kScoreSuffix.size()) + std::string(kAudioSuffix);
    }
    return path;
}

bool can_play(std::string_view path) {
    return path.ends_with(kAudioSuffix) || path.ends_with(kScoreSuffix) || path.ends_with(".ogg") ||
           path.ends_with(".wav") || path.ends_with(".mp3");
}

json::Value transition(std::string_view from, std::string_view to) {
    json::Value out;
    if (!from.empty()) {
        out["from"] = from;
    }
    if (!to.empty()) {
        out["to"] = to;
    }
    return out;
}

class Arranger {
public:
    Arranger(const json::Object& entries, const ArrangeOptions& options,
             const std::function<bool(std::string_view)>& exists)
        : entries_(entries), options_(options), exists_(exists) {
        folder_ = options.out;
        if (folder_.ends_with(kAudioSuffix)) {
            folder_.resize(folder_.size() - kAudioSuffix.size());
        }
    }

    Arranged run() && {
        std::vector<std::string> ids = options_.cues;
        if (ids.empty()) {
            for (const auto& [id, entry] : entries_) {
                if (id.starts_with("cue/")) {
                    ids.push_back(id);
                }
            }
        }
        for (const std::string& id : ids) {
            cue(id, entries_.at(id));
        }
        stings();

        json::Value& music = out_.music;
        music["format"] = 1;
        music["clips"] = std::move(clips_);
        // Every switch the cues don't say otherwise of waits for the bar.
        json::Value on_the_bar = transition({}, {});
        on_the_bar["from_time"] = "next_bar";
        transitions_.insert(transitions_.begin(), std::move(on_the_bar));
        music["transitions"] = std::move(transitions_);
        if (!options_.initial.empty()) {
            const auto first = first_clip_.find(options_.initial);
            if (first != first_clip_.end() && first->second != 0) {
                music["initial_clip"] = music["clips"].as_array()[first->second]["name"];
            }
        }
        return std::move(out_);
    }

private:
    // Records a problem with a cue, its parts run together.
    void problem(const std::string& cue, bool missing,
                 std::initializer_list<std::string_view> parts) {
        std::string message;
        for (const std::string_view part : parts) {
            message += part;
        }
        out_.problems.push_back({.cue = cue, .message = std::move(message), .missing = missing});
    }

    // The streams a cue's music files play as, after reporting those that can't play.
    std::vector<std::string> music_of(const std::string& id, const json::Value& entry) {
        std::vector<std::string> streams;
        for (const std::string& file : strings(entry, "music")) {
            if (!can_play(file)) {
                problem(id, true,
                        {id, "'s music ", file,
                         " can't play: name .audio.json, .ogg, .wav, .mp3 or .score.json files"});
                continue;
            }
            std::string stream = playable(file);
            if (!exists_(stream)) {
                out_.problems.push_back(
                    {.cue = id, .message = missing(id, file, stream), .missing = true});
            }
            streams.push_back(std::move(stream));
        }
        return streams;
    }

    // Why a music file isn't there, and what makes it: soundtrack.render writes a score's music,
    // and its stems in the folder named after it.
    [[nodiscard]] std::string missing(const std::string& id, const std::string& file,
                                      const std::string& stream) const {
        if (file.ends_with(kScoreSuffix)) {
            return id + "'s score " + file + " isn't rendered yet; soundtrack.render writes " +
                   stream;
        }
        const std::size_t slash = file.rfind('/');
        if (slash != std::string::npos) {
            const std::string score = file.substr(0, slash) + std::string(kScoreSuffix);
            if (exists_(score)) {
                return id + "'s music " + file +
                       " isn't rendered yet; soundtrack.render writes it from " + score;
            }
        }
        return id + "'s music " + file + " doesn't exist yet";
    }

    // A clip playing these streams: the stream itself, or a synchronized stream of them.
    std::string stream_for(const std::string& clip, const std::vector<std::string>& streams) {
        if (streams.size() == 1) {
            return streams.front();
        }
        json::Array layers;
        for (const std::string& stream : streams) {
            json::Value layer;
            layer["stream"] = stream;
            layers.push_back(std::move(layer));
        }
        json::Value synchronized;
        synchronized["format"] = 1;
        synchronized["synchronized"] = std::move(layers);
        std::string path = folder_ + "/" + clip + std::string(kAudioSuffix);
        out_.files.emplace_back(path, std::move(synchronized));
        return path;
    }

    std::string add_clip(const std::string& name, const std::string& cue, const std::string& layer,
                         const std::string& stream) {
        std::string unique = name;
        for (int n = 2; !names_.insert(unique).second; ++n) {
            unique = name + "_" + std::to_string(n);
        }
        json::Value clip;
        clip["name"] = unique;
        clip["stream"] = stream;
        first_clip_.emplace(cue, clips_.size());
        clips_.push_back(std::move(clip));

        json::Value what;
        what["name"] = unique;
        what["cue"] = cue;
        if (!layer.empty()) {
            what["layer"] = layer;
        }
        what["stream"] = stream;
        out_.clips.push_back(std::move(what));
        cue_clips_[cue].push_back(unique);
        return unique;
    }

    void cue(const std::string& id, const json::Value& entry) {
        const std::vector<std::string> streams = music_of(id, entry);
        if (streams.empty()) {
            out_.problems.push_back(
                {.cue = id, .message = id + " has no music yet; name its files in music"});
            return;
        }
        const std::string name = slug(name_of(id));
        const std::vector<std::string> layers = strings(entry, "layers");
        if (text_of(entry, "plays") == "once") {
            const std::string clip = add_clip(name, id, {}, stream_for(name, streams));
            stings_.emplace_back(id, clip);
            return;
        }
        if (layers.size() < 2) {
            add_clip(name, id, layers.empty() ? std::string() : layers.front(),
                     stream_for(name, streams));
            return;
        }
        if (streams.size() != layers.size()) {
            out_.problems.push_back(
                {.cue = id,
                 .message = id + " has " + std::to_string(layers.size()) + " layers and " +
                            std::to_string(streams.size()) +
                            " music files; give one file per layer, lowest first",
                 .missing = true});
            add_clip(name, id, {}, stream_for(name, streams));
            return;
        }
        // Each layer plays its file and those below it.
        std::vector<std::string> names;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const std::string clip = name + "_" + slug(layers[i]);
            const std::vector<std::string> below(
                streams.begin(), streams.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            names.push_back(add_clip(clip, id, layers[i], stream_for(clip, below)));
        }
        // Changing layers keeps the music's place.
        for (const std::string& from : names) {
            for (const std::string& to : names) {
                if (from == to) {
                    continue;
                }
                json::Value change = transition(from, to);
                change["from_time"] = "next_beat";
                change["to_time"] = "same_position";
                transitions_.push_back(std::move(change));
            }
        }
    }

    void stings() {
        std::set<std::string> filled;
        for (const auto& [id, clip] : stings_) {
            const std::vector<std::string> after = strings(entries_.at(id), "after");
            if (after.empty()) {
                // Played when asked for, then back to what it interrupted, where it was.
                clips_.at(first_clip_.at(id))["auto_advance"] = "return_to_hold";
                json::Value sting = transition({}, clip);
                sting["from_time"] = "next_beat";
                sting["hold_previous"] = true;
                transitions_.push_back(std::move(sting));
                json::Value back = transition(clip, {});
                back["from_time"] = "next_bar";
                back["to_time"] = "previous_position";
                transitions_.push_back(std::move(back));
                continue;
            }
            for (const std::string& cue : after) {
                const auto found = cue_clips_.find(cue);
                if (found == cue_clips_.end()) {
                    problem(id, false, {id, " plays after ", cue, ", which isn't arranged here"});
                    continue;
                }
                for (const std::string& from : found->second) {
                    if (!filled.insert(from).second) {
                        problem(id, false,
                                {id, " and another sting both play after ", cue,
                                 "; only the first does"});
                        break;
                    }
                    json::Value leaving = transition(from, {});
                    leaving["from_time"] = "next_bar";
                    leaving["filler_clip"] = clip;
                    transitions_.push_back(std::move(leaving));
                }
            }
        }
    }

    const json::Object& entries_;
    const ArrangeOptions& options_;
    const std::function<bool(std::string_view)>& exists_;
    std::string folder_;
    Arranged out_;
    json::Array clips_;
    json::Array transitions_;
    std::set<std::string> names_;
    std::map<std::string, std::size_t> first_clip_;
    std::map<std::string, std::vector<std::string>> cue_clips_;
    std::vector<std::pair<std::string, std::string>> stings_;
};

} // namespace

Arranged arrange(const json::Object& entries, const ArrangeOptions& options,
                 const std::function<bool(std::string_view path)>& exists) {
    return Arranger(entries, options, exists).run();
}

} // namespace slash::soundtrack
