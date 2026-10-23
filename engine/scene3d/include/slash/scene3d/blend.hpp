#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/result.hpp"

#include <functional>
#include <string>
#include <string_view>

// Blender .blend files, as Godot imports them
// (modules/gltf/editor/editor_scene_importer_blend.cpp): the engine never reads one. It runs the
// artist's own Blender in the background, which exports the file as glTF, and reads that, so no
// Blender code is linked or shipped. A converted file is kept in the project's .slash/imported/
// folder until the .blend file changes, and exports ship it in place of the .blend file, so games
// never need Blender.
namespace slash::scene3d {

inline constexpr std::string_view kBlendSuffix = ".blend";

// A Blender program and its version.
struct Blender {
    std::string program;
    int major = 0;
    int minor = 0;
};

// The Blender that converts .blend files: program if it isn't empty, else SLASH_BLENDER_PATH,
// else the first of the places Godot looks (blender on PATH, then where installers put it) that
// runs. It must print "Blender <major>.<minor>" for --version, and be 3.0 or later, as Godot
// needs.
[[nodiscard]] Result<Blender> find_blender(std::string_view program = {});

// The Python Blender runs to open source (a .blend file) and export it to destination (a .glb
// file), with Godot's default import options for that version of Blender.
[[nodiscard]] std::string blend_export_script(const Blender& blender, std::string_view source,
                                              std::string_view destination);

// Where the glTF Blender made from a .blend file is kept, by project path: in .slash/imported/,
// named after the file and a hash of its bytes, so an edited file is converted again.
[[nodiscard]] std::string blend_import_path(std::string_view path, std::string_view bytes);

// Whether bytes are a .blend file's glTF rather than the file: exports ship that in its place.
[[nodiscard]] bool is_converted_blend(std::string_view bytes);

struct BlendOptions {
    // The folder on disk that project paths are relative to.
    std::string project_root = ".";
    // Whether to run Blender for a file not converted yet; with false, only files converted
    // before load.
    bool run_blender = true;
    // Why run_blender is false, for the error a file not converted yet gives.
    std::string why_not;
    // The Blender program; empty finds it as find_blender does.
    std::string blender;
    // How long Blender may take for one file, in seconds.
    int timeout_seconds = 600;
};

// The glTF (.glb bytes) of the .blend file at a project path, whose bytes are given: the one
// made before if the file hasn't changed since, else one Blender makes now and keeps. Bytes that
// are already glTF are returned as they are. Blender's output is logged beside the glTF, and a
// failure gives its last lines.
[[nodiscard]] Result<std::string> convert_blend(std::string_view path, std::string_view bytes,
                                                const BlendOptions& options);

// Reads a file by project path.
using FileRead = std::function<Result<std::string>(std::string_view path)>;

// Reads .blend files as their glTF, converted as convert_blend converts them, and other files as
// read reads them.
[[nodiscard]] FileRead with_blender(FileRead read, BlendOptions options);

// Files with .blend files read as their glTF, as above. A converted file's stamp is its .blend
// file's, so the model reloads, and converts again, when the .blend file changes.
[[nodiscard]] asset::FileSource with_blender(asset::FileSource files, BlendOptions options);

} // namespace slash::scene3d
