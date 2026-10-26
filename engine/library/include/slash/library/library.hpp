#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The asset library: generated and added assets kept once per machine, so agents reuse them
// instead of making them again. Each asset is stored by the SHA-256 of its bytes, with a record of
// how it was made and a short description agents read instead of the file:
//
//   ~/.slash/library/
//     files/ab/abcd...ef.png    the bytes
//     assets/ab/abcd...ef.json  the record
//     requests/12/1234...cd     the asset an exact request made, so a repeat calls no model
//
// Every project on the machine searches the same library; using an asset copies it into the
// project, so exports never depend on the library. Files are written whole and moved into place,
// so several agents can share a library.
namespace slash::library {

// Where the library is unless SLASH_LIBRARY_DIR says otherwise: ~/.slash/library.
inline constexpr std::string_view kDirVariable = "SLASH_LIBRARY_DIR";

// SLASH_LIBRARY_DIR, or ~/.slash/library; "" if neither the variable nor a home folder is set.
[[nodiscard]] std::string default_dir();

// Writes a file whole or not at all, making its folder: written beside it under a name no other
// writer uses, then moved into place, so a reader sharing the library never sees half of one.
[[nodiscard]] Result<void> write_whole(const std::string& path, std::string_view contents);

// IDs in replies are the hash's first 12 hex digits; any unique prefix of 6 or more finds one.
inline constexpr std::size_t kShortId = 12;
inline constexpr std::size_t kMinIdPrefix = 6;

// What an agent or a check said about an asset once it was used.
struct Feedback {
    std::string verdict; // "accepted" or "rejected"
    std::string reason;  // why, in a few words: what worked or what was wrong
    std::string source;  // who said so: "agent", or a check such as "storyboard.check"
};

// One part of a music track, such as its drums, kept as an asset of its own.
struct Stem {
    std::string name; // such as "drums"
    std::string id;   // the stem's asset
};

struct Record {
    std::string id;   // the SHA-256 of the bytes, 64 hex digits
    std::string type; // "image", "sound", "music", "model" or "material"
    // The file's extension without the dot: "png", "bmp", "wav", "ogg", "mp3", "model.json",
    // "glb", "standard_material.json"
    std::string format;
    std::int64_t bytes = 0;
    int width = 0; // images
    int height = 0;
    double seconds = 0.0; // sounds
    int channels = 0;
    int sample_rate = 0;
    // Models: the size of their bounds in meters, x, y and z, their triangles, where their origin
    // is in their bounds (0 to 1 per axis, as scene3d::ModelSummary::pivot) and the names of
    // their parts and attachment points.
    std::array<double, 3> size{};
    std::int64_t triangles = 0;
    std::array<double, 3> pivot{};
    std::vector<std::string> parts;
    std::vector<std::string> points;
    std::string description; // a sentence or two for agents to read instead of the file
    std::vector<std::string> tags;
    json::Value request;   // what the generator was asked; null for an added file
    std::string generator; // the generator's type, such as "test"; "" for an added file
    std::string model;
    std::optional<std::int64_t> seed;
    std::string license;
    std::string author;             // who made an added file
    std::string parent;             // the asset this one was refined from
    std::vector<Feedback> feedback; // oldest first; the last kMaxFeedback are kept
    // Music: its tempo, length in beats, beats per bar and key, measured or as its author gave
    // them, and the stems it was mixed from; 0 or "" when unknown.
    double bpm = 0.0;
    int beat_count = 0;
    int bar_beats = 0;
    std::string key; // such as "D minor"
    std::vector<Stem> stems;
    std::string track; // for a stem: the track it is part of
    // A model recipe: the library models its parts are made of and the materials it names; a
    // material: its textures. In the order they appear.
    std::vector<std::string> uses;
    // A material: how its albedo tiles, as seam_ratio (material.hpp) measures it.
    double seam = 0.0;
    // A model's checks (scene3d::model_checks_schema), which it and every version made of it
    // pass; null for none.
    json::Value checks;
};

inline constexpr std::size_t kMaxFeedback = 20;

// Adds feedback to the record, dropping the oldest past kMaxFeedback.
void add_feedback(Record& record, Feedback feedback);
// How many times the asset was accepted, less how many times it was rejected.
[[nodiscard]] int net_feedback(const Record& record) noexcept;
// Whether the record's last feedback from the feedback's source says the same, so a check run
// again adds nothing new.
[[nodiscard]] bool repeats_last(const Record& record, const Feedback& feedback);

[[nodiscard]] json::Value to_json(const Record& record);
[[nodiscard]] Result<Record> record_from_json(const json::Value& value);

// "image", "sound", "model" or "material" for a file extension such as ".png", ".model.json",
// ".glb" or ".standard_material.json", or "" for one the library doesn't keep. Music is kept in
// sound files.
[[nodiscard]] std::string_view type_of(std::string_view extension);
// Whether a file of the extension can be an asset of the type.
[[nodiscard]] bool holds(std::string_view type, std::string_view extension);

// Fills in a record's type, format, size, image size, sound length or model summary from the
// file's bytes, and checks they are a file of that format; a .glb model must read nothing
// outside itself, and a recipe's model parts must be library references, which read reads
// (part_reader), as must a material's textures and the material files a recipe names. A model
// must pass the record's checks; a failure's message starts with
// "checks" and the JSON Pointer of the check in them ("checks/size/max/1: ..."). path names the
// file, for its extension.
[[nodiscard]] Result<void>
describe_file(std::string_view data, std::string_view path, Record& record,
              const std::function<Result<std::string>(std::string_view path)>& read = {});

// A library recipe names the models its parts are made of and its material files, and a library
// material its textures, as "asset:<id>.<format>", such as "asset:3fa4...9a.glb", since a
// project's paths mean nothing in another project.
inline constexpr std::string_view kAssetRef = "asset:";

// The reference to a library model that a recipe's part names.
[[nodiscard]] std::string asset_ref(const Record& record);
// The asset ID a part's model reference names, or "" when it isn't a library reference.
[[nodiscard]] std::string_view ref_id(std::string_view model);
// The same for a part's "model" value, which may not be a string.
[[nodiscard]] std::string_view ref_id_of(const json::Value& model);
// The "model" value of each of a recipe's top-level parts that has one, in order.
[[nodiscard]] std::vector<json::Value*> model_refs(json::Value& recipe);

// A value in a file that names another file the library keeps as an asset of its own.
struct AssetRef {
    json::Value* value = nullptr;
    // What it is: "model" (a recipe's model part), "material" (a material file a recipe's
    // material names) or the material field naming a texture, such as "albedo_texture".
    std::string role;
};

// The values naming other files in a recipe (its model parts, then its material files) or a
// material file (its textures), by the file's path, in order; none for other files.
[[nodiscard]] std::vector<AssetRef> asset_refs(json::Value& doc, std::string_view path);

// The bytes of the library asset with this ID.
using AssetBytes = std::function<Result<std::string>(std::string_view id)>;
// Reads the models a library recipe's parts name by their references, through get. A reference
// whose bytes don't hash to its ID is an error.
[[nodiscard]] std::function<Result<std::string>(std::string_view path)> part_reader(AssetBytes get);

class Library {
public:
    explicit Library(std::string dir);

    [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

    // Stores the bytes and the record, its ID set from them. Bytes the library already holds keep
    // their record, with the new record's tags added, and the music facts and stems it lacked.
    [[nodiscard]] Result<Record> put(std::string_view data, Record record) const;
    // Replaces the record of an asset the library holds.
    [[nodiscard]] Result<void> update(const Record& record) const;
    // The asset with this ID or ID prefix. An unknown or ambiguous one is an error.
    [[nodiscard]] Result<Record> get(std::string_view id) const;
    [[nodiscard]] Result<std::string> read(const Record& record) const;
    // Every record, ordered by ID.
    [[nodiscard]] Result<std::vector<Record>> all() const;

    // The asset an exact request made, by the request's key (request_key).
    [[nodiscard]] std::optional<std::string> find_request(std::string_view key) const;
    [[nodiscard]] Result<void> remember_request(std::string_view key, std::string_view id) const;
    // The keys of the requests that made the asset, sorted, to share it with them.
    [[nodiscard]] std::vector<std::string> requests_for(std::string_view id) const;

private:
    [[nodiscard]] std::string file_path(const Record& record) const;
    [[nodiscard]] std::string record_path(std::string_view id) const;

    std::string dir_;
};

// Reads a library recipe's part models from the library.
[[nodiscard]] std::function<Result<std::string>(std::string_view path)>
part_reader(const Library& library);

// The key of a request to a generator: 64 hex digits hashing the generator's type, its model and
// the request's canonical JSON.
[[nodiscard]] std::string request_key(std::string_view generator, std::string_view model,
                                      const json::Value& request);

// Members left out of a designated initializer are empty.
struct Query {
    std::string text{};              // NOLINT(readability-redundant-member-init): words to find
    std::vector<std::string> tags{}; // NOLINT(readability-redundant-member-init): all required
    std::string type{};              // NOLINT(readability-redundant-member-init): "" for any
    std::size_t limit = 10;
};

struct Match {
    const Record* record = nullptr;
    // The share of the query's words found, moved up or down kFeedbackStep for each time the
    // asset was accepted more than rejected (at most 4 times), 0 to 1.
    double score = 0.0;
};

inline constexpr double kFeedbackStep = 0.05;

// The records matching the query, best first; among equals, the better received, those whose
// tags matched more of the words, then by ID. With no text every record that has the tags and
// type matches, with score 1 before feedback.
[[nodiscard]] std::vector<Match> search(std::span<const Record> records, const Query& query);

} // namespace slash::library
