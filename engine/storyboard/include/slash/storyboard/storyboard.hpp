#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The storyboard holds a game's facts that are not code: style, world, story, design rules,
// names, decisions and open work. Each fact is a typed entry under a stable ID in one of the
// storyboard/*.json files, so agents look facts up instead of remembering them, and programs can
// check them.
//
//   storyboard/world.json
//   {
//     "entries": {
//       "character/mira": {
//         "faction": "faction/couriers",
//         "name": "Mira",
//         "summary": "A courier who knows every rooftop route in Lowtown."
//       }
//     },
//     "format": 1
//   }
//
// An ID is a kind and a lowercase name. The kind decides the entry's fields and its file.
namespace slash::storyboard {

inline constexpr std::string_view kFolder = "storyboard/";
// One JSON object per line: who changed which entries, when and why.
inline constexpr std::string_view kJournalPath = "storyboard/journal.jsonl";
inline constexpr std::int64_t kFormat = 1;
inline constexpr std::size_t kMaxNameLength = 64;

enum class FieldType : std::uint8_t {
    Text,    // a string
    Number,  // any number
    Integer, // a whole number
    Ref,     // another entry's ID
    Refs,    // a list of entry IDs
    Texts,   // a list of strings
    Paths,   // a list of project paths: scenes, assets and source files
    Colors,  // color names mapped to "#rrggbb"
    Choice,  // one of a fixed list of strings
};

// Every member of Field, Kind and Query has an initializer so designated initializers may skip
// any of them without tripping GCC's -Wmissing-field-initializers.
struct Field {
    std::string_view name{}; // NOLINT(readability-redundant-member-init)
    FieldType type = FieldType::Text;
    std::string_view doc{}; // NOLINT(readability-redundant-member-init)
    // Ref and Refs: the kind they name, or "" for any.
    std::string_view ref_kind{}; // NOLINT(readability-redundant-member-init)
    // Choice: the allowed values.
    std::vector<std::string_view> choices{}; // NOLINT(readability-redundant-member-init)
};

struct Kind {
    // "character", whose entries live in the file "world": storyboard/world.json.
    std::string_view name{}; // NOLINT(readability-redundant-member-init)
    std::string_view file{}; // NOLINT(readability-redundant-member-init)
    std::string_view doc{};  // NOLINT(readability-redundant-member-init)
    // Besides the common fields.
    std::vector<Field> fields{}; // NOLINT(readability-redundant-member-init)
};

// The files a storyboard is made of, in order: style, world, story, design, music, glossary,
// decisions and work.
[[nodiscard]] const std::vector<std::string_view>& files();
// "storyboard/world.json" for "world".
[[nodiscard]] std::string file_path(std::string_view file);
// Every kind, in file order.
[[nodiscard]] const std::vector<Kind>& kinds();
// The fields every entry may have: name (required), summary, text, refs, links and tags.
[[nodiscard]] const std::vector<Field>& common_fields();
[[nodiscard]] const Kind* find_kind(std::string_view name);
// The kind of an ID, or nullptr if the ID is malformed or its kind is unknown.
[[nodiscard]] const Kind* kind_of(std::string_view id);

// Why id is not a valid entry ID, or "" if it is. Names the closest kind for a misspelled one.
[[nodiscard]] std::string id_error(std::string_view id);

// JSON Schema for a storyboard file, for editors and agents. Entry IDs and fields depend on the
// kind, which validate_file checks.
[[nodiscard]] json::Value file_schema();
// JSON Schema for one kind's entries.
[[nodiscard]] json::Value kind_schema(const Kind& kind);

// Checks an entry's fields against its kind. Error paths start with pointer.
void validate_entry(const Kind& kind, const json::Value& entry, std::string_view pointer,
                    std::vector<json::ValidationError>& errors);
// Checks a storyboard file: its shape, each entry, and that each entry's kind belongs in it.
[[nodiscard]] std::vector<json::ValidationError> validate_file(std::string_view file,
                                                               const json::Value& document);
// The canonical text of a storyboard file holding these entries.
[[nodiscard]] std::string format_file(const json::Object& entries);

// A short hash of an entry's fields. An edit names the version it read, and an edit of an entry
// that changed since is refused.
[[nodiscard]] std::string version(const json::Value& entry);

// An entry's Ref and Refs values, each with its path inside the entry, e.g. "/faction".
struct Reference {
    std::string target;
    std::string path;
    std::string_view kind; // the kind the field must name, or "" for any
};
[[nodiscard]] std::vector<Reference> references(const Kind& kind, const json::Value& entry);
// Why a reference does not resolve among entries, or "" if it does. A malformed ID is left to
// validate_entry.
[[nodiscard]] std::string reference_error(const json::Object& entries, const Reference& reference);
// "unknown storyboard entry ..." with the closest existing ID.
[[nodiscard]] std::string unknown_entry(const json::Object& entries, std::string_view id);

// Reads a project file: its text, or nullopt if it does not exist.
using Reader = std::function<Result<std::optional<std::string>>(std::string_view path)>;

struct FileError {
    std::string file; // e.g. "storyboard/world.json"
    std::string path; // JSON Pointer inside the file
    std::string message;
};
struct Loaded {
    json::Object entries; // every entry by ID
    std::vector<FileError> errors;
};
// Reads every storyboard file. A missing file has no entries. Fails only if a file cannot be read;
// files that do not parse or validate are reported in errors.
[[nodiscard]] Result<Loaded> load(const Reader& read);

struct EditResult {
    std::vector<json::ValidationError> errors;
    // When an edit names an old version: the entry as it is now, {"fields", "id", "version"}.
    json::Value current;
    // Each changed entry as it was before, or null for a new one.
    json::Object previous;
};
// Applies storyboard.edit's "edits" to entries. Each edit is {"id", "version", "set"} or
// {"id", "version", "remove": true}; version null creates the entry. With any error nothing
// changes, and errors point into the edits, e.g. "/edits/0/set/faction".
[[nodiscard]] EditResult apply_edits(json::Object& entries, const json::Value& edits);
// The edits that turn entries back into previous: storyboard.edit's undo.
[[nodiscard]] json::Array reverse_edits(const json::Object& entries, const json::Object& previous);

struct Query {
    // Only these entries; only this kind; only entries with a string containing text, ignoring
    // case; only entries whose links name the path linked, or whose references name the ID linked.
    std::vector<std::string> ids{}; // NOLINT(readability-redundant-member-init)
    std::string kind{};             // NOLINT(readability-redundant-member-init)
    std::string text{};             // NOLINT(readability-redundant-member-init)
    std::string linked{};           // NOLINT(readability-redundant-member-init)
    bool full = false;              // every field, not only name and summary
};
// Matching entries by ID, each {"fields", "id", "version"}.
[[nodiscard]] json::Array query(const json::Object& entries, const Query& query);

// Checks entries against each other and the project: every reference names an entry of the
// right kind and no text uses a word the glossary says to avoid (errors), and every link names a
// file that exists (warnings, since open work can link files still to be made).
struct CheckResult {
    std::vector<FileError> errors;
    std::vector<FileError> warnings;
};
[[nodiscard]] CheckResult check(const json::Object& entries,
                                const std::function<bool(std::string_view path)>& exists);

// The StoryRef IDs in a scene or prefab document, each with its JSON Pointer.
struct StoryRefId {
    std::string id;
    std::string path;
};
[[nodiscard]] std::vector<StoryRefId> story_refs(const json::Value& document);
// An error for each StoryRef ID in a scene or prefab document that names no entry.
void check_story_refs(const json::Object& entries, std::string_view file,
                      const json::Value& document, std::vector<FileError>& errors);

// A texture for check_textures: its size and straight-alpha RGBA pixels, row by row.
struct Texture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};
// An error for each texture (by project path) that breaks the style: larger on a side than a
// budget's texture_size, or with pixels whose color is not in a palette, give or take its
// tolerance (fully transparent pixels aside). A palette covers the files and folders in its
// links; a budget covers its links too, or, with neither links nor refs, every texture. Only
// textures something covers are loaded; load returns nothing for one it can't read.
void check_textures(const json::Object& entries, const std::vector<std::string>& textures,
                    const std::function<std::optional<Texture>(std::string_view path)>& load,
                    std::vector<FileError>& errors);

// A model for check_models: its triangles, and the size of each image it holds inside it (images
// in files of their own are textures, which check_textures checks).
struct ModelImageSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
struct ModelFacts {
    std::size_t triangles = 0;
    std::vector<ModelImageSize> images;
};
// An error for each model (by project path) with more triangles than a budget covering it
// allows, or an image inside it larger on a side than the budget's texture_size. Budgets cover
// models as check_textures says they cover textures; palettes leave models alone, whose textures
// are lit. Only models a budget covers are loaded; load returns nothing for one it can't read.
void check_models(const json::Object& entries, const std::vector<std::string>& models,
                  const std::function<std::optional<ModelFacts>(std::string_view path)>& load,
                  std::vector<FileError>& errors);

// An asset agents accepted, for style_edits.
struct Accepted {
    std::string path;             // its project path
    std::string description;      // what it is, or ""
    std::string reason;           // why it was accepted, or ""
    std::optional<Texture> image; // an image's pixels
    std::size_t triangles = 0;    // a model's triangles, or 0
};
// Edits for storyboard.edit that refine the style section from assets agents accepted, so later
// checks and generation requests follow them:
// - a palette that covers an accepted image gains the colors it uses that the palette lacks;
//   with no palette at all, palette/main takes the few colors images drawn in a few flat colors
//   use, linked to their folders;
// - a budget gains triangles and texture_size where no budget sets them for an accepted model
//   or image: the most an accepted one has, rounded up (to two significant digits, a power of
//   two), on the budget for everything, else a new budget/assets;
// - a reference/<file name> links each accepted asset with a reason that no reference links,
//   the reason as its summary.
[[nodiscard]] json::Array style_edits(const json::Object& entries,
                                      const std::vector<Accepted>& accepted);

// Once the storyboard has a cue, a warning for each scene (by project path) no cue plays in and
// each story beat no cue plays during: music the cue sheet has still to plan. A cue's scenes may
// name folders, which cover every scene in them.
void check_music(const json::Object& entries, const std::vector<std::string>& scenes,
                 std::vector<FileError>& warnings);

// The facts a task needs, so an agent reads a few entries rather than the whole storyboard:
// - entries: the given IDs, entries linked to the given paths, the rules, style and loudness
//   targets that apply everywhere (those with no links or refs), the themes of the cues among
//   those, and the open tasks and live decisions that link to those paths or name those entries,
//   each with every field;
// - related: the entries those name, with name and summary only.
struct Brief {
    json::Array entries;
    json::Array related;
};
[[nodiscard]] Brief brief(const json::Object& entries, const std::vector<std::string>& ids,
                          const std::vector<std::string>& paths);

// The entries the journal says changed after its first seq lines: each ID's latest version, or
// null if it was removed. Lines that do not parse are skipped.
[[nodiscard]] json::Object changes_since(std::string_view journal, std::size_t seq);

// A journal line, without its newline. changes maps each ID to its new version, or null if it
// was removed.
[[nodiscard]] std::string journal_line(std::string_view by, std::string_view why,
                                       const json::Object& changes, std::int64_t unix_seconds);
// The number of lines in a journal.
[[nodiscard]] std::size_t journal_length(std::string_view journal);
// "2026-10-15T03:04:05Z"
[[nodiscard]] std::string format_time(std::int64_t unix_seconds);

} // namespace slash::storyboard
