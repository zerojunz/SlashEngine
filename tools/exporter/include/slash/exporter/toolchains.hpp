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
    std::string url;    // the archive for this host: .tar.xz, .zip or anything tar unpacks
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

// Android exports need the Android SDK (build tools, a platform and the NDK) and a JDK, from
// ANDROID_HOME and JAVA_HOME or an Android Studio install. Without those, install_android
// downloads a JDK (Eclipse Temurin) and Google's command-line tools, whose sdkmanager installs the
// SDK packages into <dir>/android-sdk, where exports find them. Google's command-line tools and
// SDK packages are under its Android Software Development Kit License Agreement, which the person
// using them must accept first: install_android refuses unless told they have.
inline constexpr std::string_view kJdk = "jdk";
inline constexpr std::string_view kAndroidCommandLineTools = "android-cmdline-tools";
inline constexpr std::string_view kAndroidSdk = "android-sdk";
inline constexpr std::string_view kAndroidSdkLicense = "https://developer.android.com/studio/terms";

struct AndroidInstall {
    Toolchain jdk;
    Toolchain command_line_tools;
    std::vector<std::string> packages; // sdkmanager's names, such as "ndk;27.3.13750724"
};
// The pinned JDK, command-line tools and SDK packages, or nullopt where Google builds no SDK for
// this host (it has none for arm64 Linux or Windows).
[[nodiscard]] std::optional<AndroidInstall> android_install();

struct AndroidFolders {
    std::filesystem::path sdk;       // empty if the SDK packages aren't all installed
    std::filesystem::path java_home; // empty if the JDK isn't installed
};
// What install_android installed into dir.
[[nodiscard]] AndroidFolders installed_android(const AndroidInstall& install,
                                               const std::filesystem::path& dir);

// Installs the JDK and the command-line tools like toolchains, then has sdkmanager install the
// packages, answering yes to the licenses it asks about. Returns the folders, also when they were
// installed already. Without accept_license it installs nothing.
[[nodiscard]] Result<AndroidFolders> install_android(const AndroidInstall& install,
                                                     const std::filesystem::path& dir,
                                                     bool accept_license,
                                                     const std::string& log_path = {});

// AssetRipper, which turns a Unity build back into a Unity project for the Unity importer, for
// this host, or nullopt where it has no build. It is GPL 3.0, so it only ever runs as a separate
// program. Installed like a toolchain, but not one of host_toolchains.
inline constexpr std::string_view kAssetRipper = "assetripper";
[[nodiscard]] std::optional<Toolchain> assetripper();
// Its program in its installed folder.
[[nodiscard]] std::string_view assetripper_program();

// Wayland 1.18's wayland-scanner, built into slash-cli and slash-mcp on macOS and Linux, where
// Linux exports run it to write SDL's Wayland protocol code for the glibc 2.31 sysroot's Wayland
// headers. "slash-cli wayland-scanner client-header in.xml out.h" runs it as wayland-scanner.
inline constexpr std::string_view kWaylandScanner = "wayland-scanner";
inline constexpr std::string_view kWaylandScannerVersion = "1.18.0";
// Whether this build has it (Windows builds, which don't export Linux games, don't).
[[nodiscard]] bool has_wayland_scanner();
// Runs it with argv as wayland-scanner's own argv, argv[0] its name. Returns its exit status.
int run_wayland_scanner(int argc, char** argv);

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

// Downloads the toolchain's archive with curl, checks its SHA-256, and unpacks it with tar (CMake's
// for .zip files on Linux) into dir/<name>-<version>. Returns that folder, also when it was already
// installed. Download and unpack output is appended to log_path.
[[nodiscard]] Result<std::filesystem::path> install_toolchain(const Toolchain& toolchain,
                                                              const std::filesystem::path& dir,
                                                              const std::string& log_path = {});

// The SHA-256 of data or of a file, as 64 lowercase hex digits.
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& file);

} // namespace slash::exporter
