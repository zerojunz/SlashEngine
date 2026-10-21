#include "slash/soundtrack/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/midi.hpp"
#include "slash/soundtrack/score.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
}

} // namespace slash::soundtrack
