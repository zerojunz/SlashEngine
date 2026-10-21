#pragma once

// Cross toolchains that exports download on demand, pinned to exact versions and checksums, so
// one Mac builds games for every platform with nothing installed by hand.

#include "slash/core/result.hpp"
#include "slash/exporter/presets.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::exporter {

// Where toolchains are installed unless SLASH_TOOLCHAINS_DIR says otherwise: ~/.slash/toolchains.
inline constexpr std::string_view kToolchainsDirVariable = "SLASH_TOOLCHAINS_DIR";

struct Toolchain {
    std::string name;    // e.g. "llvm-mingw"
    std::string version; // the pinned release
    std::string description;
    std::string url;    // the archive for this host: .tar.xz, or anything tar unpacks
    std::string sha256; // of the archive
    std::string folder; // the archive's top-level folder, or "" if its files sit at the root
};

// The toolchains this host can install: Emscripten's for web exports, and on macOS and Linux
// the cross toolchains for Windows and Linux exports (Windows builds its own with MSVC).
[[nodiscard]] std::vector<Toolchain> host_toolchains();

// Web exports build with Emscripten from an activated emsdk (EMSDK) or, without one, from these:
// emsdk's release build of Emscripten (with its Clang and Binaryen), the Node.js it runs on and
// web exports are tested with, and on macOS and Windows the Python that runs its tools (Linux
// has python3).
inline constexpr std::string_view kEmscripten = "emscripten";
inline constexpr std::string_view kNode = "node";
inline constexpr std::string_view kPython = "python";

struct WebTools {
    std::filesystem::path toolchain_file; // Emscripten.cmake, for CMAKE_TOOLCHAIN_FILE
    std::filesystem::path node;
    std::filesystem::path python; // empty where python3 on PATH runs Emscripten's tools
    std::string config;           // an Emscripten config file naming them, for EM_CONFIG
};
// The tools in the folders that toolchains_for("web") names were installed into, in its order.
[[nodiscard]] WebTools web_tools(const std::vector<std::filesystem::path>& folders);

// AssetRipper, which turns a Unity build back into a Unity project for the Unity importer, for
// this host, or nullopt where it has no build. It is GPL 3.0, so it only ever runs as a separate
// program. Installed like a toolchain, but not one of host_toolchains.
inline constexpr std::string_view kAssetRipper = "assetripper";
[[nodiscard]] std::optional<Toolchain> assetripper();
// Its program in its installed folder.
[[nodiscard]] std::string_view assetripper_program();

// The names of the toolchains an export for platform needs on this host, or none when the
// host's own compilers build it. arch matters only for Linux. Web exports need these only when no
// emsdk is activated.
[[nodiscard]] std::vector<std::string> toolchains_for(std::string_view platform,
                                                      std::string_view arch = kLinuxX86_64);

// SLASH_TOOLCHAINS_DIR, or ~/.slash/toolchains.
[[nodiscard]] std::filesystem::path toolchains_dir();

// The folder of an installed toolchain, or nullopt if it isn't installed in dir.
[[nodiscard]] std::optional<std::filesystem::path>
installed_toolchain(const Toolchain& toolchain, const std::filesystem::path& dir);

// Downloads the toolchain's archive with curl, checks its SHA-256, and unpacks it with tar into
// dir/<name>-<version>. Returns that folder, also when it was already installed. Download and
// unpack output is appended to log_path.
[[nodiscard]] Result<std::filesystem::path> install_toolchain(const Toolchain& toolchain,
                                                              const std::filesystem::path& dir,
                                                              const std::string& log_path = {});

// The SHA-256 of data or of a file, as 64 lowercase hex digits.
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& file);

} // namespace slash::exporter
