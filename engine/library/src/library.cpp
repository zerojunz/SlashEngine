#include "slash/library/library.hpp"

#include "slash/audio/audio.hpp"
#include "slash/core/sha256.hpp"
#include "slash/library/music.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/recipe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace slash::library {
namespace {

constexpr std::string_view kFiles = "files";
constexpr std::string_view kAssets = "assets";
constexpr std::string_view kRequests = "requests";
constexpr std::size_t kHashLength = 64;

bool is_hex(std::string_view text) {
    return std::ranges::all_of(
        text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return out;
}

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    std::string extension = lowercase(path.substr(dot));
    // A model recipe's extension is its whole suffix, as a .model.json file stays one.
    if (extension == ".json" && lowercase(path).ends_with(scene3d::kModelRecipeSuffix)) {
        extension = std::string(scene3d::kModelRecipeSuffix);
    }
    return extension;
}

// The first texture a recipe's materials name, as "/materials/<name>/<field>", or "".
std::string recipe_texture(const json::Value& recipe) {
    const json::Value* materials = recipe.find("materials");
    if (materials == nullptr || !materials->is_object()) {
        return {};
    }
    for (const auto& [name, material] : materials->as_object()) {
        if (!material.is_object()) {
            continue;
        }
        for (const auto& [field, value] : material.as_object()) {
            if (field.ends_with("_texture") && value.is_string() && !value.as_string().empty()) {
                std::string pointer = "/materials/";
                pointer += name;
                pointer += '/';
                pointer += field;
                return pointer;
            }
        }
    }
    return {};
}

// Fills in a model recipe's size and triangles, and checks it builds as written.
Result<void> describe_recipe(std::string_view data, Record& record) {
    auto doc = json::parse(data);
    if (!doc) {
        return doc.error();
    }
    const std::vector<json::ValidationError> errors = scene3d::validate_model_recipe(doc.value());
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    // Texture paths are a project's, so a recipe that names one couldn't be used elsewhere.
    if (const std::string texture = recipe_texture(doc.value()); !texture.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          texture + ": the library keeps recipes that name no texture files; "
                                    "give the material colors instead");
    }
    auto model = scene3d::build_model_recipe(doc.value());
    if (!model) {
        return model.error();
    }
    const scene3d::Bounds box = scene3d::bounds(model.value());
    record.size = {rounded(box.max.x - box.min.x), rounded(box.max.y - box.min.y),
                   rounded(box.max.z - box.min.z)};
    record.triangles = 0;
    for (const scene3d::ModelMesh& mesh : model.value().meshes) {
        for (const scene3d::ModelSurface& surface : mesh.surfaces) {
            record.triangles += static_cast<std::int64_t>(surface.indices.size() / 3);
        }
    }
    return {};
}

// A name no other writer uses at the same time, for a file written whole then moved into place.
// It starts with '.', so listings skip a file left half written.
std::string temporary_name(const std::string& path) {
    static std::atomic<std::uint64_t> counter = 0;
    const std::size_t slash = path.find_last_of('/');
    const std::uint64_t stamp =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return path.substr(0, slash + 1) + "." + path.substr(slash + 1) + "." +
           std::to_string(stamp ^ thread) + "." + std::to_string(counter++) + ".tmp";
}

std::string text_of(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    return member != nullptr && member->is_string() ? member->as_string() : std::string();
}

} // namespace

Result<void> write_whole(const std::string& path, std::string_view contents) {
    const std::size_t slash = path.find_last_of('/');
    if (auto made = platform::create_directories(path.substr(0, slash)); !made) {
        return made.error();
    }
    const std::string temporary = temporary_name(path);
    if (auto written = platform::write_file(temporary, contents); !written) {
        return written.error();
    }
    if (auto moved = platform::rename_file(temporary, path); !moved) {
        (void)platform::remove_file(temporary);
        return moved.error();
    }
    return {};
}

std::string default_dir() {
    if (std::string dir = platform::environment_variable(std::string(kDirVariable)); !dir.empty()) {
        return dir;
    }
#if defined(_WIN32)
    std::string home = platform::environment_variable("USERPROFILE");
#else
    std::string home = platform::environment_variable("HOME");
#endif
    if (home.empty()) {
        return {};
    }
    std::ranges::replace(home, '\\', '/');
    if (home.back() != '/') {
        home += '/';
    }
    return home + ".slash/library";
}

json::Value to_json(const Record& record) {
    json::Value out;
    out["id"] = record.id;
    out["type"] = record.type;
    out["format"] = record.format;
    out["bytes"] = record.bytes;
    if (record.width > 0) {
        out["width"] = record.width;
        out["height"] = record.height;
    }
    if (record.sample_rate > 0) {
        out["seconds"] = record.seconds;
        out["channels"] = record.channels;
        out["sample_rate"] = record.sample_rate;
    }
    if (record.triangles > 0) {
        out["size"] = json::Array{record.size[0], record.size[1], record.size[2]};
        out["triangles"] = record.triangles;
    }
    if (!record.description.empty()) {
        out["description"] = record.description;
    }
    if (!record.tags.empty()) {
        json::Array tags;
        for (const std::string& tag : record.tags) {
            tags.emplace_back(tag);
        }
        out["tags"] = std::move(tags);
    }
    if (!record.request.is_null()) {
        out["request"] = record.request;
    }
    if (!record.generator.empty()) {
        out["generator"] = record.generator;
    }
    if (!record.model.empty()) {
        out["model"] = record.model;
    }
    if (record.seed) {
        out["seed"] = *record.seed;
    }
    if (!record.license.empty()) {
        out["license"] = record.license;
    }
    if (!record.author.empty()) {
        out["author"] = record.author;
    }
    if (!record.parent.empty()) {
        out["parent"] = record.parent;
    }
    if (!record.feedback.empty()) {
        json::Array feedback;
        for (const Feedback& item : record.feedback) {
            json::Value entry;
            entry["verdict"] = item.verdict;
            if (!item.reason.empty()) {
                entry["reason"] = item.reason;
            }
            entry["source"] = item.source;
            feedback.push_back(std::move(entry));
        }
        out["feedback"] = std::move(feedback);
    }
    if (record.bpm > 0.0) {
        out["bpm"] = bpm_json(record.bpm);
    }
    if (record.beat_count > 0) {
        out["beat_count"] = record.beat_count;
    }
    if (record.bar_beats > 0) {
        out["bar_beats"] = record.bar_beats;
    }
    if (!record.key.empty()) {
        out["key"] = record.key;
    }
    if (!record.stems.empty()) {
        json::Array stems;
        for (const Stem& stem : record.stems) {
            json::Value entry;
            entry["name"] = stem.name;
            entry["id"] = stem.id;
            stems.push_back(std::move(entry));
        }
        out["stems"] = std::move(stems);
    }
    if (!record.track.empty()) {
        out["track"] = record.track;
    }
    return out;
}

Result<Record> record_from_json(const json::Value& value) {
    if (!value.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "an asset record is a JSON object");
    }
    Record record;
    record.id = text_of(value, "id");
    record.type = text_of(value, "type");
    record.format = text_of(value, "format");
    if (record.id.size() != kHashLength || !is_hex(record.id) || record.type.empty() ||
        record.format.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "an asset record needs an id of 64 hex digits, a type and a format");
    }
    const auto integer = [&value](std::string_view key) -> std::int64_t {
        const json::Value* member = value.find(key);
        return member != nullptr && member->is_number() ? member->as_int() : 0;
    };
    record.bytes = integer("bytes");
    record.width = static_cast<int>(integer("width"));
    record.height = static_cast<int>(integer("height"));
    record.channels = static_cast<int>(integer("channels"));
    record.sample_rate = static_cast<int>(integer("sample_rate"));
    if (const json::Value* seconds = value.find("seconds");
        seconds != nullptr && seconds->is_number()) {
        record.seconds = seconds->as_double();
    }
    if (const json::Value* size = value.find("size");
        size != nullptr && size->is_array() && size->as_array().size() == 3) {
        for (std::size_t i = 0; i < 3; ++i) {
            const json::Value& side = size->as_array()[i];
            record.size.at(i) = side.is_number() ? side.as_double() : 0.0;
        }
    }
    record.triangles = integer("triangles");
    record.description = text_of(value, "description");
    if (const json::Value* tags = value.find("tags"); tags != nullptr && tags->is_array()) {
        for (const json::Value& tag : tags->as_array()) {
            if (tag.is_string()) {
                record.tags.push_back(tag.as_string());
            }
        }
    }
    if (const json::Value* request = value.find("request"); request != nullptr) {
        record.request = *request;
    }
    record.generator = text_of(value, "generator");
    record.model = text_of(value, "model");
    if (const json::Value* seed = value.find("seed"); seed != nullptr && seed->is_number()) {
        record.seed = seed->as_int();
    }
    record.license = text_of(value, "license");
    record.author = text_of(value, "author");
    record.parent = text_of(value, "parent");
    if (const json::Value* feedback = value.find("feedback");
        feedback != nullptr && feedback->is_array()) {
        for (const json::Value& item : feedback->as_array()) {
            if (item.is_object()) {
                record.feedback.push_back({.verdict = text_of(item, "verdict"),
                                           .reason = text_of(item, "reason"),
                                           .source = text_of(item, "source")});
            }
        }
    }
    if (const json::Value* bpm = value.find("bpm"); bpm != nullptr && bpm->is_number()) {
        record.bpm = bpm->as_double();
    }
    record.beat_count = static_cast<int>(integer("beat_count"));
    record.bar_beats = static_cast<int>(integer("bar_beats"));
    record.key = text_of(value, "key");
    if (const json::Value* stems = value.find("stems"); stems != nullptr && stems->is_array()) {
        for (const json::Value& item : stems->as_array()) {
            const std::string id = text_of(item, "id");
            if (id.size() == kHashLength && is_hex(id)) {
                record.stems.push_back({.name = text_of(item, "name"), .id = id});
            }
        }
    }
    record.track = text_of(value, "track");
    return record;
}

void add_feedback(Record& record, Feedback feedback) {
    record.feedback.push_back(std::move(feedback));
    if (record.feedback.size() > kMaxFeedback) {
        record.feedback.erase(record.feedback.begin(),
                              record.feedback.end() - static_cast<std::ptrdiff_t>(kMaxFeedback));
    }
}

int net_feedback(const Record& record) noexcept {
    int net = 0;
    for (const Feedback& item : record.feedback) {
        if (item.verdict == "accepted") {
            ++net;
        } else if (item.verdict == "rejected") {
            --net;
        }
    }
    return net;
}

bool repeats_last(const Record& record, const Feedback& feedback) {
    const auto last = std::ranges::find(std::ranges::reverse_view(record.feedback), feedback.source,
                                        &Feedback::source);
    return last != std::ranges::reverse_view(record.feedback).end() &&
           last->verdict == feedback.verdict && last->reason == feedback.reason;
}

std::string_view type_of(std::string_view extension) {
    const std::string lower = lowercase(extension);
    if (lower == ".png" || lower == ".bmp" || lower == ".jpg" || lower == ".jpeg") {
        return "image";
    }
    if (lower == ".wav" || lower == ".ogg" || lower == ".mp3") {
        return "sound";
    }
    if (lower == scene3d::kModelRecipeSuffix) {
        return "model";
    }
    return {};
}

bool holds(std::string_view type, std::string_view extension) {
    const std::string_view kept = type_of(extension);
    return !kept.empty() && (kept == type || (type == "music" && kept == "sound"));
}

Result<void> describe_file(std::string_view data, std::string_view path, Record& record) {
    const std::string extension = extension_of(path);
    const std::string_view type = type_of(extension);
    if (type.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) +
                              "\" is not a file the library keeps: .png, .bmp, .wav, .ogg, .mp3 "
                              "or .model.json");
    }
    record.type = std::string(type);
    record.format = extension.substr(1);
    record.bytes = static_cast<std::int64_t>(data.size());
    if (type == "model") {
        return describe_recipe(data, record);
    }
    if (type == "image") {
        auto image = render::decode_image(data);
        if (!image) {
            return image.error();
        }
        record.width = static_cast<int>(image.value().width);
        record.height = static_cast<int>(image.value().height);
        return {};
    }
    auto sound = audio::decode_sound(data, path);
    if (!sound) {
        return sound.error();
    }
    record.seconds = rounded(sound.value().length());
    record.channels = sound.value().channels;
    record.sample_rate = sound.value().mix_rate;
    return {};
}

Library::Library(std::string dir) : dir_(std::move(dir)) {
    while (dir_.size() > 1 && dir_.back() == '/') {
        dir_.pop_back();
    }
}

std::string Library::file_path(const Record& record) const {
    return dir_ + "/" + std::string(kFiles) + "/" + record.id.substr(0, 2) + "/" + record.id + "." +
           record.format;
}

std::string Library::record_path(std::string_view id) const {
    return dir_ + "/" + std::string(kAssets) + "/" + std::string(id.substr(0, 2)) + "/" +
           std::string(id) + ".json";
}

Result<Record> Library::put(std::string_view data, Record record) const {
    record.id = sha256_hex(data);
    record.bytes = static_cast<std::int64_t>(data.size());
    if (auto existing = get(record.id); existing) {
        Record kept = std::move(existing).value();
        bool changed = false;
        for (std::string& tag : record.tags) {
            if (std::ranges::find(kept.tags, tag) == kept.tags.end()) {
                kept.tags.push_back(std::move(tag));
                changed = true;
            }
        }
        // A sound added again as music, or a track again with its stems, learns what it lacked.
        if (record.type == "music" && kept.type != "music") {
            kept.type = record.type;
            kept.bpm = record.bpm;
            kept.beat_count = record.beat_count;
            kept.bar_beats = record.bar_beats;
            kept.key = record.key;
            changed = true;
        }
        if (kept.stems.empty() && !record.stems.empty()) {
            kept.stems = std::move(record.stems);
            changed = true;
        }
        if (kept.track.empty() && !record.track.empty()) {
            kept.track = std::move(record.track);
            changed = true;
        }
        if (changed) {
            if (auto updated = update(kept); !updated) {
                return updated.error();
            }
        }
        return kept;
    }
    if (auto written = write_whole(file_path(record), data); !written) {
        return written.error();
    }
    if (auto updated = update(record); !updated) {
        return updated.error();
    }
    return record;
}

Result<void> Library::update(const Record& record) const {
    return write_whole(record_path(record.id), json::write(to_json(record)));
}

Result<Record> Library::get(std::string_view id) const {
    const std::string lower = lowercase(id);
    if (lower.size() < kMinIdPrefix || lower.size() > kHashLength || !is_hex(lower)) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(id) + "\" is not an asset ID: " +
                              std::to_string(kMinIdPrefix) + " to 64 hex digits");
    }
    std::string path;
    if (lower.size() == kHashLength) {
        path = record_path(lower);
        if (!platform::exists(path)) {
            path.clear();
        }
    } else {
        const std::string folder = dir_ + "/" + std::string(kAssets) + "/" + lower.substr(0, 2);
        if (platform::exists(folder)) {
            auto files = platform::list_files(folder);
            if (!files) {
                return files.error();
            }
            for (const std::string& file : files.value()) {
                if (!file.starts_with(lower)) {
                    continue;
                }
                if (!path.empty()) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "several assets have IDs starting \"" + lower +
                                          "\"; give more digits");
                }
                path = folder;
                path += '/';
                path += file;
            }
        }
    }
    if (path.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no asset with ID \"" + std::string(id) + "\" in the library");
    }
    auto text = platform::read_file(path);
    if (!text) {
        return text.error();
    }
    auto document = json::parse(text.value());
    if (!document) {
        return make_error(ErrorCode::InvalidArgument, path + ": " + document.error().message);
    }
    return record_from_json(document.value());
}

Result<std::string> Library::read(const Record& record) const {
    return platform::read_file(file_path(record));
}

Result<std::vector<Record>> Library::all() const {
    std::vector<Record> records;
    const std::string folder = dir_ + "/" + std::string(kAssets);
    if (!platform::exists(folder)) {
        return records;
    }
    auto files = platform::list_files(folder);
    if (!files) {
        return files.error();
    }
    for (const std::string& file : files.value()) {
        if (!file.ends_with(".json")) {
            continue;
        }
        std::string path = folder;
        path += '/';
        path += file;
        auto text = platform::read_file(path);
        if (!text) {
            continue; // removed since the listing
        }
        auto document = json::parse(text.value());
        if (!document) {
            continue;
        }
        if (auto record = record_from_json(document.value()); record) {
            records.push_back(std::move(record).value());
        }
    }
    return records;
}

std::optional<std::string> Library::find_request(std::string_view key) const {
    const std::string path = dir_ + "/" + std::string(kRequests) + "/" +
                             std::string(key.substr(0, 2)) + "/" + std::string(key);
    auto text = platform::read_file(path);
    if (!text || text.value().size() != kHashLength || !is_hex(text.value())) {
        return std::nullopt;
    }
    if (!platform::exists(record_path(text.value()))) {
        return std::nullopt;
    }
    return std::move(text).value();
}

Result<void> Library::remember_request(std::string_view key, std::string_view id) const {
    return write_whole(dir_ + "/" + std::string(kRequests) + "/" + std::string(key.substr(0, 2)) +
                           "/" + std::string(key),
                       id);
}

std::vector<std::string> Library::requests_for(std::string_view id) const {
    std::vector<std::string> keys;
    const std::string folder = dir_ + "/" + std::string(kRequests);
    if (!platform::exists(folder)) {
        return keys;
    }
    auto files = platform::list_files(folder);
    if (!files) {
        return keys;
    }
    for (const std::string& file : files.value()) {
        std::string path = folder;
        path += '/';
        path += file;
        auto text = platform::read_file(path);
        if (text && text.value() == id) {
            keys.push_back(file.substr(file.find_last_of('/') + 1));
        }
    }
    std::ranges::sort(keys);
    return keys;
}

std::string request_key(std::string_view generator, std::string_view model,
                        const json::Value& request) {
    json::Value keyed;
    keyed["generator"] = generator;
    keyed["model"] = model;
    keyed["request"] = request;
    return sha256_hex(json::write(keyed, json::Style::Compact));
}

} // namespace slash::library
