#include "slash/scene3d/blend.hpp"

#include "slash/core/sha256.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace slash::scene3d {
namespace {

// Changes when the script or its options change, so files converted before are converted again.
constexpr std::string_view kImportVersion = "1";
constexpr std::string_view kImportFolder = ".slash/imported/";
constexpr std::string_view kGlbMagic = "glTF";

// Where Godot's _autodetect_path looks, after PATH.
#if defined(_WIN32)
constexpr std::string_view kInstallFolder = "C:/Program Files/Blender Foundation/";
constexpr std::array<std::string_view, 2> kInstalled = {
    "C:/Program Files/Blender Foundation/blender.exe",
    "C:/Program Files (x86)/Blender Foundation/blender.exe",
};
constexpr std::string_view kPlaces = "on PATH, or under C:\\Program Files\\Blender Foundation";
#elif defined(__APPLE__)
constexpr std::array<std::string_view, 5> kInstalled = {
    "/opt/homebrew/bin/blender",
    "/opt/local/bin/blender",
    "/usr/local/bin/blender",
    "/usr/local/opt/blender",
    "/Applications/Blender.app/Contents/MacOS/Blender",
};
constexpr std::string_view kPlaces = "on PATH, in /Applications or from Homebrew";
#else
constexpr std::array<std::string_view, 4> kInstalled = {
    "/usr/bin/blender",
    "/usr/local/bin/blender",
    "/opt/blender/bin/blender",
    "/snap/bin/blender",
};
constexpr std::string_view kPlaces =
    "on PATH, in /usr/bin, /opt/blender/bin or from the Snap Store";
#endif

std::string join(std::string_view folder, std::string_view path) {
    std::string out(folder.empty() ? "." : folder);
    if (out.back() != '/' && out.back() != '\\') {
        out += '/';
    }
    out += path;
    return out;
}

// A Python string literal of text.
std::string python_string(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '\'':
            out += "\\'";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
        }
    }
    out += '\'';
    return out;
}

// The version a Blender program prints for --version ("Blender 4.2.1 LTS"), as Godot's
// _get_blender_version reads it.
// The glTF exporter's options Godot imports .blend files with in this version of Blender, as
// Python dict entries, writing destination.
std::string gltf_options(const Blender& blender, std::string_view destination) {
    const auto at_least = [&](int major, int minor) {
        return blender.major > major || (blender.major == major && blender.minor >= minor);
    };
    std::vector<std::pair<std::string_view, std::string>> gltf = {
        {"filepath", python_string(destination)},
        {"export_format", "'GLB'"},
        {"export_yup", "True"},
        {"export_extras", "True"},
        {"export_skins", "True"},
        {"export_all_influences", "True"},
        {"export_materials", "'EXPORT'"},
        {"export_cameras", "True"},
        {"export_lights", "True"},
    };
    if (at_least(4, 2)) {
        gltf.emplace_back("export_vertex_color", "'NONE'");
    } else {
        gltf.emplace_back("export_colors", "False");
    }
    gltf.emplace_back("use_renderable", "False");
    gltf.emplace_back("use_visible", "False");
    gltf.emplace_back("export_texcoords", "True");
    gltf.emplace_back("export_normals", "True");
    gltf.emplace_back("export_tangents", "True");
    if (at_least(3, 6)) {
        gltf.emplace_back("export_animation_mode", "'ACTIONS'");
    } else {
        gltf.emplace_back("export_nla_strips", "True");
    }
    gltf.emplace_back("export_frame_range", "True");
    gltf.emplace_back("export_force_sampling", "True");
    gltf.emplace_back("export_def_bones", "False");
    gltf.emplace_back("export_apply", "True");
    std::string options;
    for (const auto& [key, value] : gltf) {
        options += python_string(key) + ": " + value + ",";
    }
    return options;
}

Result<Blender> blender_at(const std::string& program) {
    auto run = platform::run_process({program, "--version"}, {.working_directory = {},
                                                              .environment = {},
                                                              .log_path = {},
                                                              .tail_lines = 20,
                                                              .input = {}});
    if (!run) {
        return make_error(ErrorCode::NotFound, "it doesn't run: " + run.error().message);
    }
    const std::string& output = run.value().tail;
    const std::size_t at = output.find("Blender ");
    if (run.value().exit_code != 0 || at == std::string::npos) {
        return make_error(ErrorCode::InvalidArgument,
                          "it doesn't print Blender's version for --version");
    }
    Blender blender{.program = program, .major = 0, .minor = 0};
    std::size_t i = at + 8;
    const auto number = [&](int& out) {
        const std::size_t start = i;
        while (i < output.size() && output[i] >= '0' && output[i] <= '9' && i - start < 4) {
            out = (out * 10) + (output[i] - '0');
            ++i;
        }
        return i > start;
    };
    bool read = number(blender.major) && i < output.size() && output[i] == '.';
    if (read) {
        ++i;
        read = number(blender.minor);
    }
    if (!read) {
        // The line as printed, without the carriage return Windows ends it with.
        std::string line = output.substr(at, output.find('\n', at) - at);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return make_error(ErrorCode::InvalidArgument,
                          "its version isn't one Blender prints: \"" + line + "\"");
    }
    if (blender.major < 3) {
        return make_error(ErrorCode::InvalidArgument,
                          "it is Blender " + std::to_string(blender.major) + "." +
                              std::to_string(blender.minor) +
                              ", and converting .blend files needs Blender 3.0 or later");
    }
    return blender;
}

// Blender installed in a folder of its version under the installer's folder, newest first, as
// Windows' installer puts it ("Blender 4.2/blender.exe").
std::vector<std::string> versioned_installs() {
    std::vector<std::string> out;
#if defined(_WIN32)
    auto files = platform::list_files(kInstallFolder, [](std::string_view folder) {
        return folder.find('/') == folder.size() - 1;
    });
    if (files) {
        for (auto it = files.value().rbegin(); it != files.value().rend(); ++it) {
            if (it->ends_with("/blender.exe")) {
                out.push_back(join(kInstallFolder, *it));
            }
        }
    }
#endif
    return out;
}

// Removes what Blender made from earlier versions of a .blend file: other files in its folder
// named after it.
void remove_stale(const std::string& folder, std::string_view prefix, std::string_view keep) {
    auto files = platform::list_files(folder, [](std::string_view) { return false; });
    if (!files) {
        return;
    }
    for (const std::string& file : files.value()) {
        if (file.starts_with(prefix) && !file.starts_with(keep)) {
            (void)platform::remove_file(join(folder, file));
        }
    }
}

// The last lines of what a program printed, for an error message.
std::string last_lines(std::string_view output, std::size_t count) {
    std::size_t end = output.size();
    while (end > 0 && (output[end - 1] == '\n' || output[end - 1] == '\r')) {
        --end;
    }
    std::size_t start = end;
    for (std::size_t lines = 0; start > 0; --start) {
        if (output[start - 1] == '\n' && ++lines == count) {
            break;
        }
    }
    return std::string(output.substr(start, end - start));
}

} // namespace

Result<Blender> find_blender(std::string_view program) {
    std::string configured(program);
    if (configured.empty()) {
        configured = platform::environment_variable("SLASH_BLENDER_PATH");
        if (!configured.empty()) {
            auto blender = blender_at(configured);
            if (!blender) {
                return make_error(blender.error().code, "SLASH_BLENDER_PATH names \"" + configured +
                                                            "\", but " + blender.error().message);
            }
            return blender;
        }
    } else {
        auto blender = blender_at(configured);
        if (!blender) {
            return make_error(blender.error().code,
                              "Blender \"" + configured +
                                  "\" can't be used: " + blender.error().message);
        }
        return blender;
    }
    std::vector<std::string> candidates = {"blender"};
    for (const std::string_view place : kInstalled) {
        candidates.emplace_back(place);
    }
    for (std::string& place : versioned_installs()) {
        candidates.push_back(std::move(place));
    }
#if defined(__APPLE__)
    if (const std::string home = platform::environment_variable("HOME"); !home.empty()) {
        candidates.push_back(home + "/Applications/Blender.app/Contents/MacOS/Blender");
    }
#endif
    std::optional<Error> too_old;
    for (const std::string& candidate : candidates) {
        if (candidate != "blender" && !platform::exists(candidate)) {
            continue;
        }
        auto blender = blender_at(candidate);
        if (blender) {
            return blender;
        }
        if (blender.error().code == ErrorCode::InvalidArgument && !too_old) {
            too_old =
                make_error(ErrorCode::NotFound, "the Blender at \"" + candidate +
                                                    "\" can't be used: " + blender.error().message);
        }
    }
    if (too_old) {
        return *too_old;
    }
    return make_error(ErrorCode::NotFound,
                      "converting a .blend file needs Blender 3.0 or later, which wasn't found " +
                          std::string(kPlaces) +
                          ": install it, or set SLASH_BLENDER_PATH to its program");
}

// Godot's import_scene with its default import options, writing one .glb file rather than a
// .gltf file and the textures it unpacks beside the .blend file, so a conversion is one file in
// .slash/imported/ and leaves the project's own folders alone. Run through Godot's
// PYTHON_SCRIPT_DIRECT.
std::string blend_export_script(const Blender& blender, std::string_view source,
                                std::string_view destination) {
    // Paths are relative to the project folder, which Blender starts in.
    return "import bpy, os, sys\n"
           "opts = {'path': " +
           python_string(source) + ",'gltf_options': {" + gltf_options(blender, destination) +
           "},}\n"
           "if bpy.app.version < (3, 0, 0):\n"
           "  print('Blender 3.0 or higher is required.', file=sys.stderr)\n"
           "source = os.path.abspath(opts['path'])\n"
           "opts['gltf_options']['filepath'] = "
           "os.path.abspath(opts['gltf_options']['filepath'])\n"
           "bpy.ops.wm.open_mainfile(filepath=source)\n"
           "bpy.ops.export_scene.gltf(**opts['gltf_options'])\n";
}

std::string blend_build_script(const Blender& blender, std::string_view script,
                               std::string_view params, std::int64_t seed,
                               std::string_view destination) {
    return "import bpy, json, os, sys\n"
           "opts = {'script': " +
           python_string(script) + ",'params': " + python_string(params) +
           ",'seed': " + std::to_string(seed) + ",'gltf_options': {" +
           gltf_options(blender, destination) +
           "},}\n"
           "if bpy.app.version < (3, 0, 0):\n"
           "  print('Blender 3.0 or higher is required.', file=sys.stderr)\n"
           "opts['gltf_options']['filepath'] = "
           "os.path.abspath(opts['gltf_options']['filepath'])\n"
           "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
           "exec(compile(opts['script'], 'script.py', 'exec'), {'__name__': '__main__', "
           "'bpy': bpy, 'params': json.loads(opts['params']), 'seed': opts['seed']})\n"
           "bpy.ops.export_scene.gltf(**opts['gltf_options'])\n";
}

std::string blend_import_path(std::string_view path, std::string_view bytes) {
    Sha256 hash;
    hash.update(kImportVersion);
    hash.update(bytes);
    return std::string(kImportFolder) + std::string(path) + "-" + hash.hex().substr(0, 16) + ".glb";
}

bool is_converted_blend(std::string_view bytes) {
    return bytes.starts_with(kGlbMagic);
}

namespace {

// What Blender is doing, for messages: "converting it", "convert it" and "for it".
struct Task {
    std::string_view doing;
    std::string_view verb;
    std::string_view object;
};

// Runs script in Blender in the background in folder, logging to log, and returns the .glb file
// it writes to part (both relative to folder), which a failure removes. A failure gives the last
// lines Blender printed and where its log is (as named).
Result<std::string> run_job(const Blender& blender, const std::string& script,
                            const std::string& folder, const std::string& part,
                            const std::string& log, int timeout_seconds, const Task& task) {
    (void)platform::remove_file(join(folder, log));
    auto process = platform::start_process(
        {blender.program, "--background", "--python-exit-code", "1", "--python-expr", script},
        {.working_directory = folder,
         .environment = {},
         .log_path = join(folder, log),
         .tail_lines = 20,
         .input = {}});
    if (!process) {
        return make_error(process.error().code, "Blender didn't start: " + process.error().message);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (process.value()->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool timed_out = process.value()->running();
    const int exit_code = process.value()->stop();
    const std::string output = process.value()->output();
    auto made = platform::read_file(join(folder, part));
    if (timed_out || exit_code != 0 || !made || !is_converted_blend(made.value())) {
        (void)platform::remove_file(join(folder, part));
        std::string why;
        if (timed_out) {
            why = "Blender was stopped after " + std::to_string(timeout_seconds) +
                  " s, before it finished " + std::string(task.doing);
        } else if (exit_code != 0) {
            why = "Blender couldn't " + std::string(task.verb) + " (exit code " +
                  std::to_string(exit_code) + "): " + last_lines(output, 3);
        } else {
            why = "Blender wrote no glTF file " + std::string(task.object) + ": " +
                  last_lines(output, 3);
        }
        return make_error(ErrorCode::InvalidArgument, why + "; its log is " + log);
    }
    return made;
}

} // namespace

Result<std::string> convert_blend(std::string_view path, std::string_view bytes,
                                  const BlendOptions& options) {
    if (is_converted_blend(bytes)) {
        return std::string(bytes);
    }
    const std::string target = blend_import_path(path, bytes);
    if (const std::string kept = join(options.project_root, target); platform::exists(kept)) {
        return platform::read_file(kept);
    }
    if (!options.run_blender) {
        return make_error(ErrorCode::NotFound,
                          "it hasn't been converted from Blender yet, and " + options.why_not);
    }
    auto blender = find_blender(options.blender);
    if (!blender) {
        return blender.error();
    }
    const std::string stem = target.substr(0, target.size() - 4); // without ".glb"
    const std::string part = stem + ".part.glb";
    const std::size_t slash = target.rfind('/');
    const std::string folder = join(options.project_root, target.substr(0, slash));
    if (auto made = platform::create_directories(folder); !made) {
        return made.error();
    }
    auto made = run_job(blender.value(), blend_export_script(blender.value(), path, part),
                        options.project_root, part, stem + ".log", options.timeout_seconds,
                        {.doing = "converting it", .verb = "convert it", .object = "for it"});
    if (!made) {
        return made.error();
    }
    if (auto moved = platform::rename_file(join(options.project_root, part),
                                           join(options.project_root, target));
        !moved) {
        return moved.error();
    }
    const std::string file(path.substr(path.rfind('/') + 1));
    remove_stale(folder, file + "-", stem.substr(slash + 1) + ".");
    return made;
}

Result<BlenderBuild> run_blender_script(const BlenderScript& job) {
    auto blender = find_blender(job.blender);
    if (!blender) {
        return blender.error();
    }
    if (auto made = platform::create_directories(job.folder); !made) {
        return made.error();
    }
    const std::string part = job.name + ".part.glb";
    auto made = run_job(
        blender.value(),
        blend_build_script(blender.value(), job.script, job.params, job.seed, part), job.folder,
        part, job.name + ".log", job.timeout_seconds,
        {.doing = "running the script", .verb = "run the script", .object = "for the script"});
    (void)platform::remove_file(join(job.folder, part));
    if (!made) {
        return made.error();
    }
    return BlenderBuild{.glb = std::move(made).value(),
                        .version = std::to_string(blender.value().major) + "." +
                                   std::to_string(blender.value().minor)};
}

FileRead with_blender(FileRead read, BlendOptions options) {
    return [read = std::move(read),
            options = std::move(options)](std::string_view path) -> Result<std::string> {
        auto bytes = read(path);
        if (!bytes || !path.ends_with(kBlendSuffix)) {
            return bytes;
        }
        return convert_blend(path, bytes.value(), options);
    };
}

asset::FileSource with_blender(asset::FileSource files, BlendOptions options) {
    files.read = with_blender(std::move(files.read), std::move(options));
    return files;
}

} // namespace slash::scene3d
