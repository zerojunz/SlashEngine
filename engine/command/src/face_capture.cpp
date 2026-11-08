#include "slash/animation/animation.hpp"
#include "slash/command/session.hpp"
#include "slash/command/xr.hpp"
#include "slash/core/text.hpp"
#include "slash/l10n/translation.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/xr/face.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// animation.import_face_capture: a face capture app's recording as a .anim.json animation of blend
// shape tracks. SlashEngine's own (Godot reads face tracking live only): it reads the CSV files
// Live Link Face and similar ARKit apps write, a time column then a column of weights per ARKit
// shape, and names each track as XRFaceModifier3D would drive the shape.
namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply param_error(std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code::kInvalidParams, std::string(path) + ": " + message,
                          std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

double rounded(double value) {
    return std::round(value * 1e4) / 1e4;
}

// A path with its extensions replaced by suffix.
std::string with_suffix(std::string_view path, std::string_view suffix) {
    const std::size_t slash = path.rfind('/');
    const std::size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find('.', name);
    return std::string(path.substr(0, dot)) + std::string(suffix);
}

std::string stem(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    return std::string(name.substr(0, name.find('.')));
}

// A timecode, HH:MM:SS:FF with the frame's fraction after it (";" before the frame for drop
// frame), or HH:MM:SS.sss, in seconds.
std::optional<double> timecode_seconds(std::string_view text, double fps) {
    std::vector<double> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ':' || text[i] == ';') {
            const std::optional<double> part = parse_decimal(text.substr(start, i - start));
            if (!part || *part < 0.0) {
                return std::nullopt;
            }
            parts.push_back(*part);
            start = i + 1;
        }
    }
    if (parts.size() != 3 && parts.size() != 4) {
        return std::nullopt;
    }
    double seconds = (parts[0] * 3600.0) + (parts[1] * 60.0) + parts[2];
    if (parts.size() == 4) {
        seconds += parts[3] / fps;
    }
    return seconds;
}

struct Key {
    double time;
    double value;
};

// The keys left when those a straight line between their neighbors passes within tolerance of are
// dropped (Ramer, Douglas and Peucker); one key when the track never leaves its first value.
std::vector<Key> reduced(const std::vector<Key>& keys, double tolerance) {
    if (keys.size() <= 2 || std::ranges::all_of(keys, [&](const Key& key) {
            return std::abs(key.value - keys.front().value) <= tolerance;
        })) {
        return keys.empty() ? keys : std::vector<Key>{keys.front()};
    }
    std::vector<bool> keep(keys.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> spans{{0, keys.size() - 1}};
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        double worst = tolerance;
        std::size_t at = 0;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double t = (keys[i].time - keys[a].time) / (keys[b].time - keys[a].time);
            const double line = keys[a].value + ((keys[b].value - keys[a].value) * t);
            if (const double off = std::abs(keys[i].value - line); off > worst) {
                worst = off;
                at = i;
            }
        }
        if (at != 0) {
            keep[at] = true;
            spans.emplace_back(a, at);
            spans.emplace_back(at, b);
        }
    }
    std::vector<Key> out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keep[i]) {
            out.push_back(keys[i]);
        }
    }
    return out;
}

// A recording: each row's time from the first and the face shapes' weights.
struct Recording {
    std::vector<double> times;
    std::vector<std::array<float, xr::kFaceShapeCount>> weights;
    std::array<bool, xr::kFaceShapeCount> given{};
    // The shapes' columns as named, by shape.
    std::array<std::string, xr::kFaceShapeCount> columns;
    std::vector<std::string> skipped;
};

Result<Recording> read_recording(std::string_view text, double fps) {
    const std::vector<l10n::CsvRow> rows = l10n::csv_rows(text);
    if (rows.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the file is empty");
    }
    Recording out;
    std::optional<std::size_t> time_column;
    bool timecode = false;
    std::vector<std::optional<std::size_t>> shapes(rows.front().fields.size());
    for (std::size_t c = 0; c < rows.front().fields.size(); ++c) {
        const std::string_view name = trimmed(rows.front().fields[c]);
        std::string lower(name);
        std::ranges::transform(lower, lower.begin(), [](char ch) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        });
        if (!time_column && (lower == "timecode" || lower == "time" || lower == "seconds")) {
            time_column = c;
            timecode = lower == "timecode";
            continue;
        }
        const std::optional<std::size_t> shape = xr::face_shape_named(name);
        if (!shape || out.given.at(*shape)) {
            out.skipped.emplace_back(name);
            continue;
        }
        shapes[c] = shape;
        out.given.at(*shape) = true;
        out.columns.at(*shape) = name;
    }
    if (!time_column) {
        return make_error(ErrorCode::InvalidArgument,
                          "line 1 has no Timecode, time or seconds column");
    }
    if (std::ranges::none_of(out.given, [](bool given) { return given; })) {
        return make_error(ErrorCode::InvalidArgument,
                          "line 1 names no face shape, such as JawOpen or EyeBlinkLeft");
    }
    std::optional<double> first;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const std::vector<std::string>& fields = rows[r].fields;
        if (fields.size() == 1 && trimmed(fields.front()).empty()) {
            continue;
        }
        const std::string line = "line " + std::to_string(r + 1);
        if (fields.size() < rows.front().fields.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              line + " has " + std::to_string(fields.size()) + " fields, not " +
                                  std::to_string(rows.front().fields.size()));
        }
        const std::string_view cell = trimmed(fields[*time_column]);
        const std::optional<double> time =
            timecode ? timecode_seconds(cell, fps) : parse_decimal(cell);
        if (!time) {
            return make_error(ErrorCode::InvalidArgument,
                              line + "'s time " + quoted(cell) + " isn't " +
                                  (timecode ? "a timecode, HH:MM:SS:FF" : "a number"));
        }
        if (!first) {
            first = time;
        }
        const double at = *time - *first;
        if (!out.times.empty() && at <= out.times.back()) {
            return make_error(ErrorCode::InvalidArgument,
                              line + "'s time isn't after the line before's");
        }
        std::array<float, xr::kFaceShapeCount> weights{};
        for (std::size_t c = 0; c < shapes.size(); ++c) {
            if (!shapes[c]) {
                continue;
            }
            const std::optional<double> value = parse_decimal(trimmed(fields[c]));
            if (!value) {
                return make_error(ErrorCode::InvalidArgument,
                                  line + "'s " + out.columns.at(*shapes[c]) + " " +
                                      quoted(trimmed(fields[c])) + " isn't a number");
            }
            weights.at(*shapes[c]) = static_cast<float>(*value);
        }
        out.times.push_back(at);
        out.weights.push_back(weights);
    }
    if (out.times.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the file has no rows after its header");
    }
    return out;
}

// ARKit's name for a column: Live Link Face capitalizes them ("EyeBlinkLeft" for "eyeBlinkLeft").
std::string arkit_name(std::string_view column) {
    std::string name(column);
    if (!name.empty()) {
        name.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(name.front())));
    }
    return name;
}

Reply import_face_capture(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    const json::Value* out_param = params.find("out");
    const std::string out =
        out_param != nullptr ? out_param->as_string() : with_suffix(path, ".anim.json");
    if (!out.ends_with(animation::kAnimationSuffix)) {
        return param_error("/out", quoted(out) + " isn't a .anim.json file");
    }
    if (auto full = session.resolve_path(out); !full) {
        return param_error("/out", full.error().message);
    }
    const json::Value* name_param = params.find("name");
    const std::string name = name_param != nullptr ? name_param->as_string() : stem(path);
    const json::Value* fps_param = params.find("fps");
    const double fps = fps_param != nullptr ? fps_param->as_double() : 60.0;
    const json::Value* tolerance_param = params.find("tolerance");
    const double tolerance = tolerance_param != nullptr ? tolerance_param->as_double() : 0.005;

    auto text = session.read_project_file(path);
    if (!text) {
        return param_error("/path", text.error().message);
    }
    if (!text.value()) {
        return param_error("/path", quoted(path) + " doesn't exist");
    }
    auto recording = read_recording(*text.value(), fps);
    if (!recording) {
        return param_error("/path", recording.error().message);
    }
    Recording& taken = recording.value();
    // The shapes the recording implies, the same on every row.
    std::array<bool, xr::kFaceShapeCount> known = taken.given;
    for (auto& weights : taken.weights) {
        known = taken.given;
        xr::fill_blended_shapes(weights, known);
    }

    // Each track's blend shape and the face shape it follows.
    std::vector<std::pair<std::string, std::size_t>> tracks;
    json::Array undriven;
    if (const json::Value* model_param = params.find("model"); model_param != nullptr) {
        auto model = session.assets().load<scene3d::Model>(model_param->as_string());
        if (!model) {
            return param_error("/model", model.error().message);
        }
        std::vector<std::string> names;
        for (const scene3d::ModelMesh& mesh : model.value()->meshes) {
            for (const std::string& shape : mesh.blend_shapes) {
                if (std::ranges::find(names, shape) == names.end()) {
                    names.push_back(shape);
                }
            }
        }
        const auto mapping = xr::face_blend_mapping(names);
        for (const std::string& shape : names) {
            const auto found = mapping.find(shape);
            if (found != mapping.end() && known.at(found->second)) {
                tracks.emplace_back(shape, found->second);
            } else {
                undriven.emplace_back(shape);
            }
        }
        if (tracks.empty()) {
            return param_error("/model", quoted(model_param->as_string()) +
                                             " has no blend shape the recording drives");
        }
    } else {
        for (std::size_t shape = 0; shape < xr::kFaceShapeCount; ++shape) {
            if (taken.given.at(shape)) {
                tracks.emplace_back(arkit_name(taken.columns.at(shape)), shape);
            }
        }
    }

    json::Array track_list;
    std::size_t key_count = 0;
    for (const auto& [shape_name, shape] : tracks) {
        std::vector<Key> keys;
        keys.reserve(taken.times.size());
        for (std::size_t i = 0; i < taken.times.size(); ++i) {
            keys.push_back(
                {.time = taken.times[i], .value = static_cast<double>(taken.weights[i].at(shape))});
        }
        json::Array key_list;
        for (const Key& key : reduced(keys, tolerance)) {
            json::Value entry;
            entry["time"] = rounded(key.time);
            entry["value"] = rounded(key.value);
            key_list.push_back(std::move(entry));
        }
        key_count += key_list.size();
        json::Value track;
        track["path"] = "MeshInstance3D/blend_shapes/" + shape_name;
        track["keys"] = std::move(key_list);
        track_list.push_back(std::move(track));
    }
    json::Value animation_value;
    animation_value["length"] = rounded(taken.times.back());
    animation_value["tracks"] = std::move(track_list);

    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    json::Value library;
    if (before.value()) {
        auto parsed = json::parse(*before.value());
        if (!parsed || !parsed.value().is_object()) {
            return param_error("/out", quoted(out) + " isn't a JSON object");
        }
        library = std::move(parsed).value();
    } else {
        library["format"] = 1;
    }
    library["animations"][name] = std::move(animation_value);
    if (auto checked = animation::parse_animation_library(library); !checked) {
        return param_error("/out", checked.error().message);
    }
    std::string after = json::write(library);

    json::Value result;
    result["out"] = out;
    result["animation"] = name;
    result["length"] = rounded(taken.times.back());
    result["frames"] = taken.times.size();
    result["tracks"] = tracks.size();
    result["keys"] = key_count;
    if (!taken.skipped.empty()) {
        json::Array skipped;
        for (const std::string& column : taken.skipped) {
            skipped.emplace_back(column);
        }
        result["skipped"] = std::move(skipped);
    }
    if (!undriven.empty()) {
        result["undriven"] = std::move(undriven);
    }
    if (before.value() == after) {
        session.mark_unchanged();
        result["unchanged"] = true;
        return Reply::success(std::move(result));
    }
    if (auto staged = session.stage_file(out, after); !staged) {
        return Reply::failure(staged.error());
    }
    session.add_undo_file(out, std::move(before).value(), std::move(after));
    return Reply::success(std::move(result));
}

} // namespace

void register_face_capture_commands(Registry& registry) {
    json::Value path = property("string", "The recording: a CSV file of a time column (Timecode "
                                          "or seconds) and a column per ARKit face shape, as "
                                          "Live Link Face writes. Project path.");
    path["minLength"] = 1;
    json::Value out = property("string", "The .anim.json library to write the animation into, "
                                         "keeping its others. Default: path as .anim.json.");
    out["minLength"] = 1;
    json::Value name = property("string", "The animation's name. Default: the file's name.");
    name["minLength"] = 1;
    json::Value model = property(
        "string", "The face model: tracks drive its blend shapes named in ARKit's, Unified "
                  "Expressions', SRanipal's or Meta's standard, as XRFaceModifier3D does. "
                  "Default: a track per column, named as ARKit names it.");
    model["minLength"] = 1;
    json::Value fps = property("number", "Frames a second of the Timecode column. Default 60.");
    fps["exclusiveMinimum"] = 0;
    json::Value tolerance = property(
        "number", "How far a dropped key may be from the line between those kept. Default 0.005.");
    tolerance["minimum"] = 0;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"path", std::move(path)},   {"out", std::move(out)}, {"name", std::move(name)},
        {"model", std::move(model)}, {"fps", std::move(fps)}, {"tolerance", std::move(tolerance)}};
    schema["required"] = json::Array{"path"};
    registry.add({
        .name = "animation.import_face_capture",
        .description = "Turns a face capture recording (ARKit blend shapes over time, as Live "
                       "Link Face's CSV) into a .anim.json animation of blend shape tracks, with "
                       "keys a straight line passes near dropped. Returns its length and tracks.",
        .capabilities = {Capability::Edit, Capability::Filesystem},
        .effect = Effect::Edit,
        .params = std::move(schema),
        .handler = import_face_capture,
    });
}

} // namespace slash::command
