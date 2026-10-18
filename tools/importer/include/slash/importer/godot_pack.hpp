#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/importer/godot_text.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// A game exported with Godot, both ways: unpacking a .pck file (or an executable with the pack
// inside) back into the project it was made from, and writing packs, binary resources and
// GDScript tokens as Godot's export does, so tests and examples make their exported games in
// code rather than committing binary files.
namespace slash::importer::godot {

// --- Unpacking ---

// A Godot project recovered from an exported game, by res:// path without "res://".
struct UnpackedGame {
    std::map<std::string, std::string> files;
    // What came back: the pack's format and Godot version, counts of scenes, resources, scripts,
    // textures, sounds and fonts, and "unsupported", the files that couldn't.
    json::Value report;
};

// Unpacks a Godot 4 game exported as a .pck file, or as an executable with the pack inside, back
// into its project: binary scenes and resources as .tscn and .tres text, project.binary as
// project.godot, GDScript tokens as .gd source, imported textures as PNG files, sounds as WAV,
// Ogg and MP3 files, and fonts as the files they were. A texture or sound whose source was in
// another format comes back under its path with the new extension, and references to it are
// changed to match. Packs whose file list is encrypted, Godot 3 packs and sparse packs are
// refused; a file encrypted on its own is listed in "unsupported".
[[nodiscard]] Result<UnpackedGame> unpack_game(std::string_view bytes);

// Turns a GDScript exported as binary tokens (a .gdc file: GDScriptTokenizerBuffer's format,
// version 100 as Godot 4.3 and 4.4 write it or 101 as Godot 4.5 does, plain or compressed with
// zstd) back into source.
// Comments and the original spacing are gone, but every line keeps its number and indentation,
// so errors and stack traces still point at the right lines. tab_size is the columns Godot
// counted a tab as: 4 up to Godot 4.6, 1 from Godot 4.7 on.
[[nodiscard]] Result<std::string> detokenize_gdscript(std::string_view bytes, int tab_size = 4);

// --- Writing, as Godot's export does ---

struct TokenOptions {
    std::uint32_t version = 101; // 100 as Godot 4.3 and 4.4 write, which has no "..." token
    int tab_size = 4;            // the columns a tab counts as: 1 from Godot 4.7 on
    // In a zstd frame, as Godot's default "compressed binary tokens" export mode stores them.
    // The frame's blocks are stored rather than compressed: this writes no compressor.
    bool compress = true;
};

// Turns GDScript source into binary tokens (.gdc) as GDScriptTokenizerBuffer::parse_code_string
// does. Errors name the line.
[[nodiscard]] Result<std::string> tokenize_gdscript(std::string_view source,
                                                    const TokenOptions& options = {});

struct ResourceOptions {
    // The res:// path the resource was saved from, which a .tres file's binary form names.
    std::string path{}; // NOLINT(readability-redundant-member-init)
    // RSCC, in zstd frames of stored blocks, as ResourceSaver's FLAG_COMPRESS writes.
    bool compress = false;
};

// Writes a .tscn or .tres file's sections (parse_resource's) as a binary resource (.scn or .res),
// as Godot's export saves one: a scene packed with its names and values listed once, ext and
// sub-resources in the order the main resource reaches them, the ones it doesn't reach left
// out. read_binary_resource reads it back into the same sections.
[[nodiscard]] Result<std::string> write_binary_resource(const std::vector<Section>& sections,
                                                        const ResourceOptions& options = {});

// Writes project.godot's sections (parse_project_settings's) as project.binary.
[[nodiscard]] Result<std::string> write_project_binary(const std::vector<Section>& sections);

struct PackOptions {
    // 2 as Godot 4.0 to 4.4 write (the file list before the files), 3 as Godot 4.5 and 4.6 do,
    // 4 as later versions do.
    std::uint32_t format = 3;
    std::uint32_t major = 4;
    std::uint32_t minor = 5;
    std::uint32_t patch = 0;
    std::uint32_t flags = 0; // added to the pack's own: 1 marks the file list encrypted
    // Flags for single files by path, as the file list stores them: 1 encrypted, 2 removed by a
    // patch, 4 a patch to another pack's file.
    std::map<std::string, std::uint32_t> file_flags{}; // NOLINT(readability-redundant-member-init)
    // When set, the pack goes inside this executable, after its bytes, as an export with
    // "Embed PCK" makes it.
    std::string executable{}; // NOLINT(readability-redundant-member-init)
};

// Writes a pack of files keyed by res:// path without "res://", as EditorExportPlatform's
// save_pack lays it out.
[[nodiscard]] std::string write_pack(const std::map<std::string, std::string>& files,
                                     const PackOptions& options = {});

struct ExportOptions {
    bool binary_resources = true; // "Convert text resources to binary on export"
    bool script_tokens = true;    // scripts as binary tokens, not source
    TokenOptions tokens{};        // NOLINT(readability-redundant-member-init)
};

// A project's files as Godot's export puts them in its pack: .tscn and .tres files as binary
// resources under .godot/exported with .remap files, scripts as .gdc tokens, project.godot as
// project.binary, PNG files as CompressedTexture2D (.ctex) files, PCM WAV files as
// AudioStreamWAV (.sample) resources, MP3 files and fonts as resources holding their bytes, each
// with an .import file, and UIDs in .godot/uid_cache.bin. .import, .uid and .godot/ files are
// replaced by what the export writes; other files go as they are.
[[nodiscard]] Result<std::map<std::string, std::string>>
export_project(const std::map<std::string, std::string>& project,
               const ExportOptions& options = {});

} // namespace slash::importer::godot
