#include "cross.hpp"

#include "slash/core/log.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <system_error>

namespace slash::exporter::detail {
namespace {

namespace fs = std::filesystem;

// A path as a quoted CMake string.
std::string cmake_path(const fs::path& path) {
    return "\"" + path.generic_string() + "\"";
}

std::string set(std::string_view variable, const std::string& value) {
    return "set(" + std::string(variable) + " " + value + ")\n";
}

// Search only the target's files for libraries, headers and packages, and only the host's for
// programs.
constexpr std::string_view kFindModes = "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
                                        "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
                                        "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
                                        "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n";

std::string windows_toolchain(const fs::path& llvm) {
    const fs::path bin = llvm / "bin";
    const std::string triple = "x86_64-w64-mingw32";
    return set("CMAKE_SYSTEM_NAME", "Windows") + set("CMAKE_SYSTEM_PROCESSOR", "AMD64") +
           set("CMAKE_C_COMPILER", cmake_path(bin / (triple + "-clang"))) +
           set("CMAKE_CXX_COMPILER", cmake_path(bin / (triple + "-clang++"))) +
           set("CMAKE_RC_COMPILER", cmake_path(bin / (triple + "-windres"))) +
           set("CMAKE_AR", cmake_path(bin / "llvm-ar")) +
           set("CMAKE_RANLIB", cmake_path(bin / "llvm-ranlib")) +
           set("CMAKE_STRIP", cmake_path(bin / "llvm-strip")) +
           set("CMAKE_FIND_ROOT_PATH", cmake_path(llvm / triple)) + std::string(kFindModes);
}

// Clang from llvm-mingw targeting Linux, with GCC's runtime and libstdc++ from the sysroot,
// linked by LLD.
std::string linux_toolchain(const fs::path& llvm, const fs::path& sysroot,
                            const std::string& triple, const std::string& processor) {
    const fs::path bin = llvm / "bin";
    const std::string link = "\"-fuse-ld=lld -rtlib=libgcc -unwindlib=libgcc\"";
    return set("CMAKE_SYSTEM_NAME", "Linux") + set("CMAKE_SYSTEM_PROCESSOR", processor) +
           set("CMAKE_SYSROOT", cmake_path(sysroot)) +
           set("CMAKE_C_COMPILER", cmake_path(bin / "clang")) +
           set("CMAKE_CXX_COMPILER", cmake_path(bin / "clang++")) +
           set("CMAKE_C_COMPILER_TARGET", triple) + set("CMAKE_CXX_COMPILER_TARGET", triple) +
           set("CMAKE_AR", cmake_path(bin / "llvm-ar")) +
           set("CMAKE_RANLIB", cmake_path(bin / "llvm-ranlib")) +
           set("CMAKE_STRIP", cmake_path(bin / "llvm-strip")) +
           set("CMAKE_CXX_FLAGS_INIT", "-stdlib=libstdc++") +
           set("CMAKE_EXE_LINKER_FLAGS_INIT", link) + set("CMAKE_SHARED_LINKER_FLAGS_INIT", link) +
           std::string(kFindModes);
}

// A script that runs this program as wayland-scanner, or empty where it has none.
fs::path wayland_scanner_script(const fs::path& work) {
    const std::string program = platform::executable_path();
    if (!has_wayland_scanner() || program.empty()) {
        return {};
    }
    std::string quoted = "'";
    for (const char c : program) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    quoted += "'";
    std::error_code error;
    const fs::path script = fs::absolute(work / kWaylandScanner, error);
    const std::string text =
        "#!/bin/sh\nexec " + quoted + " " + std::string(kWaylandScanner) + " \"$@\"\n";
    if (!platform::write_file(script.string(), text)) {
        return {};
    }
    fs::permissions(script, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    error);
    return error ? fs::path() : script;
}

std::string no_toolchain(const std::string& platform, const std::string& name) {
    return platform + " games can't be built on this machine yet: there is no " + name +
           " toolchain for it";
}

} // namespace

bool prepare_cross(const std::string& platform, const std::string& arch, const fs::path& work,
                   const std::string& log_path, CrossBuild& cross, Failure& failure) {
    cross = CrossBuild{};
    const std::vector<std::string> needed = toolchains_for(platform, arch);
    if (needed.empty()) {
        return true;
    }
    const std::vector<Toolchain> available = host_toolchains();
    const fs::path dir = toolchains_dir();
    std::vector<fs::path> folders;
    for (const std::string& name : needed) {
        const auto toolchain = std::ranges::find(available, name, &Toolchain::name);
        if (toolchain == available.end()) {
            failure = {"options", no_toolchain(platform, name), {}};
            if (platform == "web") {
                failure.message += "; install emsdk and activate it (source emsdk_env.sh) so "
                                   "EMSDK is set";
            }
            return false;
        }
        if (!installed_toolchain(*toolchain, dir)) {
            log::info("export step", {{"step", "toolchain"}, {"toolchain", name}});
        }
        auto folder = install_toolchain(*toolchain, dir, log_path);
        if (!folder) {
            failure = {"toolchain", folder.error().message, {}};
            return false;
        }
        folders.push_back(std::move(folder).value());
        cross.build_name += (cross.build_name.empty() ? "" : "-") + toolchain->version;
    }

    std::error_code error;
    if (platform == "web") {
        // Emscripten's own CMake toolchain file, and a config naming its tools.
        const WebTools tools = web_tools(folders);
        cross.toolchain_file = tools.toolchain_file;
        cross.build_name = "emscripten-" + cross.build_name;
        cross.node = tools.node;
        cross.python = tools.python;
        const fs::path config = fs::absolute(work / "emscripten.config", error);
        if (auto written = platform::write_file(config.string(), tools.config); !written) {
            failure = {"toolchain", written.error().message, {}};
            return false;
        }
        cross.environment = {{"EM_CONFIG", config.string()}};
        if (!tools.python.empty()) {
            cross.environment.emplace_back("EMSDK_PYTHON", tools.python.string());
        }
        return true;
    }
    std::string text;
    if (platform == "windows") {
        text = windows_toolchain(folders.at(0));
        cross.build_name = "mingw-" + cross.build_name;
    } else {
        const bool arm64 = arch == kLinuxArm64;
        const std::string triple = arm64 ? "aarch64-linux-gnu" : "x86_64-linux-gnu";
        const fs::path& sysroot = folders.at(1);
        text = linux_toolchain(folders.at(0), sysroot, triple, arm64 ? "aarch64" : "x86_64");
        cross.build_name = "linux-" + arch + "-" + cross.build_name;
        // SDL's Wayland code is written by the scanner built into this program, Wayland 1.18's
        // like the sysroot's headers. Without it, or without pkg-config to find the sysroot's
        // Wayland, games get X11 only.
        const fs::path scanner = wayland_scanner_script(work);
        if (!scanner.empty()) {
            text += "set(WAYLAND_SCANNER " + cmake_path(scanner) +
                    " CACHE FILEPATH \"Wayland 1.18's wayland-scanner\" FORCE)\n";
        }
        if (scanner.empty()) {
            log::warn("this build of the tools has no wayland-scanner, so Linux games built "
                      "with it run on X11 only");
        }
        if (find_on_path("pkg-config").empty()) {
            log::warn("Linux games built without pkg-config run on X11 only, with no Wayland, "
                      "PulseAudio or PipeWire: install it (brew install pkgconf on macOS)");
        }
        // pkg-config must read the sysroot's packages, never the host's. Chromium's sysroots
        // keep most of them in usr/lib/pkgconfig.
        const std::string separator = ":";
        cross.environment = {
            {"PKG_CONFIG_LIBDIR", (sysroot / "usr" / "lib" / "pkgconfig").string() + separator +
                                      (sysroot / "usr" / "lib" / triple / "pkgconfig").string() +
                                      separator +
                                      (sysroot / "usr" / "share" / "pkgconfig").string()},
            {"PKG_CONFIG_SYSROOT_DIR", sysroot.string()},
            {"PKG_CONFIG_PATH", ""},
        };
    }
    cross.toolchain_file = fs::absolute(work / (cross.build_name + ".cmake"), error);
    if (auto written = platform::write_file(cross.toolchain_file.string(), text); !written) {
        failure = {"toolchain", written.error().message, {}};
        return false;
    }
    return true;
}

} // namespace slash::exporter::detail
