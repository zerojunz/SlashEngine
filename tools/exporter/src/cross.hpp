#pragma once

// Builds for platforms the host's own compilers can't target: Windows and Linux from a Mac, Linux
// everywhere, so its exports run on glibc 2.31, and the web with downloaded Emscripten where no
// emsdk is activated. Used by export.cpp only.

#include "steps.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace slash::exporter::detail {

struct CrossBuild {
    std::filesystem::path toolchain_file; // for CMAKE_TOOLCHAIN_FILE; empty for a native build
    std::string build_name; // names the build folder, so a new toolchain starts a fresh build
    std::vector<std::pair<std::string, std::string>> environment; // for every build step
    std::filesystem::path node;   // web: the Node.js that tests run on
    std::filesystem::path python; // web: the Python that runs Emscripten, or empty for python3
};

// Installs the toolchains an export for platform needs (step "toolchain"), then writes a CMake
// toolchain file for them into work, or for the web Emscripten's config, which EM_CONFIG names.
// Leaves cross empty when the host builds platform itself.
bool prepare_cross(const std::string& platform, const std::string& arch,
                   const std::filesystem::path& work, const std::string& log_path,
                   CrossBuild& cross, Failure& failure);

} // namespace slash::exporter::detail
