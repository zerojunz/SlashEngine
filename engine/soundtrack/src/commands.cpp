#include "slash/soundtrack/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/arrange.hpp"
#include "slash/soundtrack/midi.hpp"
#include "slash/soundtrack/plan.hpp"
#include "slash/soundtrack/render.hpp"
#include "slash/soundtrack/score.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

constexpr int kMaxGrid = 96;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    return schema;
}

Reply failure_at(int code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code, std::string(path) + ": " + message, std::move(data));
}

Reply failure_at(const Error& error, std::string_view path) {
    return failure_at(Reply::failure(error).code, path, error.message);
}

bool ends_with_lower(std::string_view path, std::string_view suffix) {
    if (path.size() < suffix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char c = path[path.size() - suffix.size() + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

// path with its extension, the part after its last '.' (or the given suffix), replaced.
std::string with_suffix(std::string_view path, std::string_view old_suffix,
                        std::string_view suffix) {
    std::string_view base = path;
    if (!old_suffix.empty() && ends_with_lower(path, old_suffix)) {
        base = path.substr(0, path.size() - old_suffix.size());
    } else if (const std::size_t dot = path.rfind('.');
               dot != std::string_view::npos && path.find('/', dot) == std::string_view::npos) {
        base = path.substr(0, dot);
    }
    return std::string(base) + std::string(suffix);
}

std::string text(const json::Value& params, std::string_view key) {
    const json::Value* member = params.find(key);
    return member != nullptr ? member->as_string() : std::string();
}

json::Value hundredths(double value) {
    const double rounded = std::round(value * 100.0) / 100.0;
    if (rounded == std::floor(rounded)) {
        return static_cast<std::int64_t>(rounded);
    }
    return rounded;
}

Reply score_import(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    if (!ends_with_lower(path, ".mid") && !ends_with_lower(path, ".midi")) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          "expected a Standard MIDI File (.mid or .midi)");
    }
    std::string out = text(params, "out");
    if (out.empty()) {
        out = with_suffix(path, "", kScoreSuffix);
    } else if (!out.ends_with(kScoreSuffix)) {
        return failure_at(command::code::kInvalidArgument, "/out", "expected a .score.json file");
    }
    auto full = session.resolve_path(path);
    if (!full) {
        return failure_at(full.error(), "/path");
    }
    auto target = session.resolve_path(out);
    if (!target) {
        return failure_at(target.error(), "/out");
    }
    auto bytes = platform::read_file(full.value());
    if (!bytes) {
        return failure_at(bytes.error(), "/path");
    }
    const json::Value* grid = params.find("grid");
    auto imported =
        read_midi(bytes.value(), grid != nullptr ? static_cast<int>(grid->as_int()) : kDefaultGrid);
    if (!imported) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + " " + imported.error().message);
    }
    const Score& score = imported.value().score;
    const json::Value doc = score_to_json(score);
    if (const auto errors = validate_score(doc); !errors.empty()) {
        return Reply::failure(command::code::kInternalError,
                              "the imported score doesn't validate: " + errors.front().path + ": " +
                                  errors.front().message);
    }
    if (!session.dry_running()) {
        if (auto written = session.write_file(target.value(), json::write(doc) + "\n"); !written) {
            return Reply::failure(written.error());
        }
    }
    json::Value result;
    result["path"] = out;
    result["bars"] = score.bars;
    result["bpm"] = hundredths(score.bpm);
    if (score.bar_beats != 4 || score.beat_unit != 4) {
        result["meter"] = std::to_string(score.bar_beats) + "/" + std::to_string(score.beat_unit);
    }
    if (!score.key.empty()) {
        result["key"] = score.key;
    }
    if (!score.sections.empty()) {
        json::Array sections;
        for (const Section& section : score.sections) {
            sections.emplace_back(section.name);
        }
        result["sections"] = std::move(sections);
    }
    json::Array tracks;
    for (const Track& each : score.tracks) {
        json::Value track;
        track["name"] = each.name;
        track["instrument"] = each.instrument;
        track["notes"] = each.notes.size();
        tracks.push_back(std::move(track));
    }
    result["tracks"] = std::move(tracks);
    if (!imported.value().problems.empty()) {
        json::Array problems;
        for (const std::string& problem : imported.value().problems) {
            problems.emplace_back(problem);
        }
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

Reply score_export(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    if (!path.ends_with(kScoreSuffix)) {
        return failure_at(command::code::kInvalidArgument, "/path", "expected a .score.json file");
    }
    std::string out = text(params, "out");
    if (out.empty()) {
        out = with_suffix(path, kScoreSuffix, ".mid");
    } else if (!ends_with_lower(out, ".mid") && !ends_with_lower(out, ".midi")) {
        return failure_at(command::code::kInvalidArgument, "/out", "expected a .mid or .midi file");
    }
    auto target = session.resolve_path(out);
    if (!target) {
        return failure_at(target.error(), "/out");
    }
    auto read = session.read_project_file(path);
    if (!read) {
        return failure_at(read.error(), "/path");
    }
    const std::optional<std::string> contents = std::move(read).value();
    if (!contents) {
        return failure_at(command::code::kNotFound, "/path", "no file " + path);
    }
    auto doc = json::parse(*contents);
    if (!doc) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + ": " + doc.error().message);
    }
    if (const auto errors = validate_score(doc.value()); !errors.empty()) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + " " + (errors.front().path.empty() ? "" : errors.front().path) +
                              ": " + errors.front().message);
    }
    const Score score = parse_score(doc.value()).value();
    auto midi = write_midi(score);
    if (!midi) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + " " + midi.error().message);
    }
    if (!session.dry_running()) {
        if (auto written = session.write_file(target.value(), midi.value()); !written) {
            return Reply::failure(written.error());
        }
    }
    std::size_t notes = 0;
    for (const Track& track : score.tracks) {
        notes += track.notes.size();
    }
    json::Value result;
    result["path"] = out;
    result["tracks"] = score.tracks.size();
    result["notes"] = notes;
    result["seconds"] = hundredths(score.seconds());
    return Reply::success(std::move(result));
}

// A number as JSON, whole numbers without a fraction.
json::Value number(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

json::Value decibels(double db) {
    if (!std::isfinite(db)) {
        return nullptr;
    }
    return number(std::round(db * 10.0) / 10.0);
}

// A track's name as a file name: letters, digits, '-' and '_', the rest as '_'.
std::string file_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(plain ? c : '_');
    }
    return out.empty() ? "track" : out;
}

// The stems' file names, one per track in order, made unique.
std::vector<std::string> stem_names(const Score& score) {
    std::vector<std::string> names;
    std::set<std::string> taken;
    for (const Track& track : score.tracks) {
        const std::string base = file_name(track.name);
        std::string name = base;
        for (int n = 2; !taken.insert(name).second; ++n) {
            name = base + "_" + std::to_string(n);
        }
        names.push_back(std::move(name));
    }
    return names;
}

// A stem's .audio.json file: its sound, timed by the score's beats, looping if it loops.
json::Value stem_audio(const std::string& source, const Score& score, bool loop, bool ogg) {
    json::Value doc;
    doc["format"] = 1;
    doc["source"] = source;
    doc["bpm"] = number(score.bpm);
    doc["beat_count"] = score.bars * score.bar_beats;
    if (score.bar_beats != 4) {
        doc["bar_beats"] = score.bar_beats;
    }
    if (loop) {
        if (ogg) {
            doc["loop"] = true;
        } else {
            doc["loop_mode"] = "forward";
        }
    }
    return doc;
}

// A stem's file in the out folder.
std::string stem_file(const std::string& out, const std::string& name, std::string_view extension) {
    std::string file = out;
    file += '/';
    file += name;
    file += extension;
    return file;
}

std::uint32_t serial_of(std::string_view name) {
    std::uint32_t hash = 2166136261U;
    for (const char c : name) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
    }
    return hash;
}

Reply soundtrack_render(Session& session, const json::Value& params) {
    const std::string path = text(params, "path");
    if (!path.ends_with(kScoreSuffix)) {
        return failure_at(command::code::kInvalidArgument, "/path", "expected a .score.json file");
    }
    std::string out = text(params, "out");
    while (out.ends_with('/')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = path.substr(0, path.size() - kScoreSuffix.size());
    }
    const std::string format = params.find("format") != nullptr ? text(params, "format") : "ogg";
    const bool ogg = format == "ogg";
    const json::Value* loop_param = params.find("loop");
    const bool loop = loop_param != nullptr && loop_param->as_bool();
    const json::Value* quality_param = params.find("quality");
    const double quality = quality_param != nullptr ? quality_param->as_double() : 0.5;

    auto read = session.read_project_file(path);
    if (!read) {
        return failure_at(read.error(), "/path");
    }
    const std::optional<std::string> contents = std::move(read).value();
    if (!contents) {
        return failure_at(command::code::kNotFound, "/path", "no file " + path);
    }
    auto doc = json::parse(*contents);
    if (!doc) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + ": " + doc.error().message);
    }
    if (const auto errors = validate_score(doc.value()); !errors.empty()) {
        return failure_at(command::code::kInvalidArgument, "/path",
                          path + " " + errors.front().path + ": " + errors.front().message);
    }
    const Score score = parse_score(doc.value()).value();

    std::vector<std::string> tracks;
    if (const json::Value* named = params.find("tracks"); named != nullptr) {
        const json::Array& list = named->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& name = list[i].as_string();
            if (std::ranges::none_of(score.tracks,
                                     [&](const Track& track) { return track.name == name; })) {
                std::vector<std::string_view> names;
                names.reserve(score.tracks.size());
                for (const Track& track : score.tracks) {
                    names.emplace_back(track.name);
                }
                std::string message = path;
                message += " has no track called ";
                message += name;
                if (const std::string_view near = closest_match(name, names); !near.empty()) {
                    message += "; did you mean " + std::string(near) + "?";
                }
                return failure_at(command::code::kInvalidArgument, "/tracks/" + std::to_string(i),
                                  message);
            }
            tracks.push_back(name);
        }
    }

    std::string sound_font_storage;
    std::string_view sound_font = builtin_sound_bank();
    const std::string sound_font_path = text(params, "soundfont");
    if (!sound_font_path.empty()) {
        auto font = session.read_project_file(sound_font_path);
        if (!font) {
            return failure_at(font.error(), "/soundfont");
        }
        std::optional<std::string> bytes = std::move(font).value();
        if (!bytes) {
            return failure_at(command::code::kNotFound, "/soundfont", "no file " + sound_font_path);
        }
        sound_font_storage = std::move(*bytes);
        sound_font = sound_font_storage;
    }

    RenderOptions options;
    options.loop = loop;
    auto stems = render_score(score, sound_font, options, tracks);
    if (!stems) {
        const std::string what =
            sound_font_path.empty() ? "the built-in sound bank" : sound_font_path;
        return failure_at(command::code::kInvalidArgument,
                          sound_font_path.empty() ? "/path" : "/soundfont",
                          what + " " + stems.error().message);
    }

    const std::vector<std::string> names = stem_names(score);
    const std::string extension = ogg ? ".ogg" : ".wav";
    json::Array stem_results;
    json::Array problems;
    std::vector<float> mix;
    std::size_t stem_index = 0;
    for (std::size_t t = 0; t < score.tracks.size(); ++t) {
        const Track& track = score.tracks[t];
        if (stem_index >= stems.value().size() || stems.value()[stem_index].track != track.name) {
            continue;
        }
        const Stem& stem = stems.value()[stem_index++];
        const std::string sound = stem_file(out, names[t], extension);
        const std::string audio = stem_file(out, names[t], ".audio.json");
        auto sound_full = session.resolve_path(sound);
        auto audio_full = session.resolve_path(audio);
        if (!sound_full || !audio_full) {
            return failure_at(sound_full ? audio_full.error() : sound_full.error(), "/out");
        }
        std::string bytes;
        if (ogg) {
            auto encoded = encode_ogg(stem.samples, options.rate, quality, serial_of(track.name));
            if (!encoded) {
                return Reply::failure(encoded.error());
            }
            bytes = std::move(encoded).value();
        } else {
            bytes = encode_wav(stem.samples, options.rate);
        }
        if (!session.dry_running()) {
            if (auto written = session.write_file(sound_full.value(), bytes); !written) {
                return Reply::failure(written.error());
            }
            const json::Value settings = stem_audio(sound, score, loop, ogg);
            if (auto written = session.write_file(audio_full.value(), json::write(settings) + "\n");
                !written) {
                return Reply::failure(written.error());
            }
        }
        const double peak = peak_db(stem.samples);
        json::Value result;
        result["track"] = track.name;
        result["path"] = sound;
        result["peak_db"] = decibels(peak);
        stem_results.push_back(std::move(result));
        if (track.notes.empty()) {
            problems.emplace_back(track.name + " has no notes");
        } else if (peak > 0.0) {
            problems.emplace_back(track.name + " clips: it peaks at +" +
                                  json::write(decibels(peak)) + " dBFS; lower its volume_db");
        }
        if (mix.size() < stem.samples.size()) {
            mix.resize(stem.samples.size());
        }
        for (std::size_t i = 0; i < stem.samples.size(); ++i) {
            mix[i] += stem.samples[i];
        }
    }

    // The music: every stem rendered so far, played together.
    const std::string music = out + ".audio.json";
    json::Array layers;
    for (std::size_t t = 0; t < score.tracks.size(); ++t) {
        const std::string audio = stem_file(out, names[t], ".audio.json");
        const bool rendered =
            tracks.empty() || std::ranges::find(tracks, score.tracks[t].name) != tracks.end();
        auto full = session.resolve_path(audio);
        if (rendered || (full && platform::exists(full.value()))) {
            json::Value layer;
            layer["stream"] = audio;
            layers.push_back(std::move(layer));
        }
    }
    auto music_full = session.resolve_path(music);
    if (!music_full) {
        return failure_at(music_full.error(), "/out");
    }
    if (!session.dry_running()) {
        json::Value synchronized;
        synchronized["format"] = 1;
        synchronized["synchronized"] = std::move(layers);
        if (auto written = session.write_file(music_full.value(), json::write(synchronized) + "\n");
            !written) {
            return Reply::failure(written.error());
        }
    }

    json::Value result;
    result["path"] = music;
    result["bpm"] = hundredths(score.bpm);
    result["beat_count"] = score.bars * score.bar_beats;
    result["seconds"] = hundredths(score.seconds());
    result["stems"] = std::move(stem_results);
    if (tracks.empty() && score.tracks.size() > 1) {
        const double peak = peak_db(mix);
        result["peak_db"] = decibels(peak);
        if (peak > 0.0) {
            problems.emplace_back("the tracks together peak at +" + json::write(decibels(peak)) +
                                  " dBFS; lower their volume_db");
        }
    }
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

// The storyboard's entries, or the reply saying why they don't load.
std::optional<Reply> load_storyboard(Session& session, json::Object& entries) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    if (const auto& errors = loaded.value().errors; !errors.empty()) {
        const storyboard::FileError& first = errors.front();
        std::string message = first.file;
        if (!first.path.empty()) {
            message += " at ";
            message += first.path;
        }
        message += ": ";
        message += first.message;
        message += "; storyboard.check lists every problem";
        return Reply::failure(command::code::kInvalidArgument, message);
    }
    entries = std::move(loaded.value().entries);
    return std::nullopt;
}

// Why id names no cue, or "" if it does.
std::string cue_error(const json::Object& entries, const std::string& id) {
    if (!id.starts_with("cue/")) {
        return R"(expected a cue's ID, such as "cue/harbor"; got ")" + id + "\"";
    }
    return entries.contains(id) ? std::string() : storyboard::unknown_entry(entries, id);
}

Reply soundtrack_arrange(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }
    ArrangeOptions options;
    if (const std::string out = text(params, "out"); !out.empty()) {
        options.out = out;
    }
    if (!options.out.ends_with(".audio.json")) {
        return failure_at(command::code::kInvalidArgument, "/out", "expected a .audio.json file");
    }
    if (const json::Value* cues = params.find("cues"); cues != nullptr) {
        for (std::size_t i = 0; i < cues->size(); ++i) {
            const std::string& id = cues->as_array()[i].as_string();
            if (const std::string message = cue_error(entries, id); !message.empty()) {
                return failure_at(command::code::kNotFound, "/cues/" + std::to_string(i), message);
            }
            options.cues.push_back(id);
        }
    }
    options.initial = text(params, "initial");
    if (!options.initial.empty()) {
        std::string message = cue_error(entries, options.initial);
        if (message.empty() && !options.cues.empty() &&
            std::ranges::find(options.cues, options.initial) == options.cues.end()) {
            message = options.initial + " isn't one of cues";
        }
        if (!message.empty()) {
            return failure_at(command::code::kInvalidArgument, "/initial", message);
        }
    }

    Arranged arranged = arrange(entries, options, [&session](std::string_view path) {
        auto full = session.resolve_path(path);
        return full && platform::exists(full.value());
    });
    json::Array problems;
    for (std::string& problem : arranged.problems) {
        problems.emplace_back(std::move(problem));
    }
    if (arranged.clips.empty()) {
        json::Value data;
        data["problems"] = std::move(problems);
        return Reply::failure(command::code::kInvalidArgument,
                              "no cue has music yet; name each cue's files in its music field",
                              std::move(data));
    }
    // The layers first, then the music that plays them.
    arranged.files.emplace_back(options.out, std::move(arranged.music));
    json::Array files;
    for (const auto& [path, document] : arranged.files) {
        auto full = session.resolve_path(path);
        if (!full) {
            return failure_at(full.error(), "/out");
        }
        if (auto written = session.write_file(full.value(), json::write(document) + "\n");
            !written) {
            return Reply::failure(written.error());
        }
        files.emplace_back(path);
    }
    json::Value result;
    result["path"] = options.out;
    result["clips"] = std::move(arranged.clips);
    if (files.size() > 1) {
        files.pop_back();
        result["layers"] = std::move(files);
    }
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

Reply soundtrack_plan(Session& session, const json::Value& params) {
    json::Object entries;
    if (auto failed = load_storyboard(session, entries)) {
        return std::move(*failed);
    }

    // Every scene in the project except exports in build/, the open one as it is now.
    auto files = platform::list_files(session.options().project_root,
                                      [](std::string_view folder) { return folder != "build/"; });
    if (!files) {
        return Reply::failure(files.error());
    }
    const std::string& open = session.scene_path();
    std::vector<PlanScene> scenes;
    const auto add_scene = [&](const std::string& path, const json::Value& document) {
        PlanScene scene{.path = path, .story_ids = {}};
        for (storyboard::StoryRefId& ref : storyboard::story_refs(document)) {
            scene.story_ids.push_back(std::move(ref.id));
        }
        scenes.push_back(std::move(scene));
    };
    for (const std::string& path : files.value()) {
        if (!path.ends_with(world::kSceneSuffix) || path == open) {
            continue;
        }
        auto read = session.read_project_file(path);
        const std::optional<std::string> contents = read ? read.value() : std::nullopt;
        if (!contents) {
            continue;
        }
        // A scene that doesn't parse is scene validate's to report.
        if (auto document = json::parse(*contents); document) {
            add_scene(path, document.value());
        }
    }
    if (!open.empty()) {
        add_scene(open, session.save().scene);
    }
    std::ranges::sort(scenes, {}, &PlanScene::path);

    std::vector<std::string> states;
    if (const json::Value* named = params.find("states"); named != nullptr) {
        for (const json::Value& state : named->as_array()) {
            states.push_back(state.as_string());
        }
    }
    Plan plan = plan_cues(entries, scenes, states);
    json::Value result;
    result["edits"] = std::move(plan.edits);
    if (!plan.unthemed.empty()) {
        json::Array unthemed;
        for (std::string& id : plan.unthemed) {
            unthemed.emplace_back(std::move(id));
        }
        result["unthemed"] = std::move(unthemed);
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_commands(command::Registry& registry) {
    json::Value grid = property("integer", "Divisions of a beat notes snap to. Default 24: "
                                           "thirty-second notes and sixteenth-note triplets.");
    grid["minimum"] = 1;
    grid["maximum"] = kMaxGrid;
    registry.add({
        .name = "score.import",
        .description = "Turns a Standard MIDI File into a .score.json score: a track per "
                       "instrument, notes snapped to a grid, the first tempo, meter and key, and "
                       "markers as sections. Returns the tracks and what didn't fit.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"path", property("string", "The .mid or .midi file, relative to the project.")},
                {"out", property("string", "The .score.json file to write. Default: beside "
                                           "path, with its name.")},
                {"grid", std::move(grid)},
            },
            {"path"}),
        .handler = score_import,
    });
    registry.add({
        .name = "score.export",
        .description = "Writes a .score.json score as a Standard MIDI File, to play or edit in "
                       "any music program.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"path", property("string", "The .score.json file, relative to the project.")},
                {"out", property("string", "The .mid file to write. Default: beside path, with "
                                           "its name.")},
            },
            {"path"}),
        .handler = score_export,
    });

    json::Value format = property("string", "ogg (default) or wav.");
    format["enum"] = json::Array{"ogg", "wav"};
    json::Value quality = property("number", "Ogg Vorbis quality, 0 (about 64 kbit/s) to 1 (about "
                                             "500 kbit/s). Default 0.5.");
    quality["minimum"] = 0;
    quality["maximum"] = 1;
    json::Value tracks = property("array", "Only these tracks, by name. Default: every track.");
    tracks["items"]["type"] = "string";
    tracks["uniqueItems"] = true;
    tracks["minItems"] = 1;
    registry.add({
        .name = "soundtrack.render",
        .description = "Plays a .score.json score through a SoundFont into one stem per track "
                       "(.ogg or .wav, each with a .audio.json file giving bpm and beat_count) "
                       "and a .audio.json file that plays them together. Returns each stem's "
                       "peak and what clips.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"path", property("string", "The .score.json file, relative to the project.")},
                {"out", property("string", "The folder for the stems. Default: beside path, "
                                           "with its name; the music is that folder's name "
                                           "with .audio.json.")},
                {"tracks", std::move(tracks)},
                {"loop", property("boolean", "Stems exactly the score's length that loop, what "
                                             "rings past the end added onto the start. Default "
                                             "false: they ring on after the end.")},
                {"format", std::move(format)},
                {"quality", std::move(quality)},
                {"soundfont", property("string", "A SoundFont 2 (.sf2) file to play the "
                                                 "instruments with. Default: the engine's "
                                                 "built-in General MIDI sounds.")},
            },
            {"path"}),
        .handler = soundtrack_render,
    });

    json::Value states = property("array", "Game states the music follows, e.g. [\"combat\"]; "
                                           "each no cue names gets one.");
    states["items"]["type"] = "string";
    states["uniqueItems"] = true;
    registry.add({
        .name = "soundtrack.plan",
        .description = "Drafts the storyboard's cue sheet: a cue for each scene, story beat and "
                       "game state with no music, with the theme of its place, faction or "
                       "character. Returns edits for storyboard.edit and the subjects that need "
                       "a theme.",
        .capabilities = {Capability::Read},
        .effect = command::Effect::Read,
        .params = params_schema({{"states", std::move(states)}}, {}),
        .handler = soundtrack_plan,
    });

    json::Value cues = property("array", "Only these cues, by ID, in this order. Default: every "
                                         "cue.");
    cues["items"]["type"] = "string";
    cues["uniqueItems"] = true;
    cues["minItems"] = 1;
    registry.add({
        .name = "soundtrack.arrange",
        .description = "Writes the storyboard's cues as interactive music a player's "
                       "switch_to_clip steers: a clip per cue, or per layer, switching on the "
                       "bar; layers keep their place; cues that play once are stings.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"out", property("string", "The .audio.json file to write. Default: "
                                           "music/soundtrack.audio.json.")},
                {"cues", std::move(cues)},
                {"initial", property("string", "The cue it starts with. Default: the first.")},
            },
            {}),
        .handler = soundtrack_arrange,
    });
}

} // namespace slash::soundtrack
