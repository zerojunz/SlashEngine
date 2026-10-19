#pragma once

#include "slash/core/json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace slash::exporter {

// An error the compiler found in a game's code.
struct CompileError {
    std::string file; // relative to the project when inside it
    int line = 0;
    int column = 0;
    std::string message;
};

// What building a game's code did.
struct GameplayBuild {
    bool ok = false;
    // Where it failed ("options", "configure" or "build") and why.
    std::string step;
    std::string message;
    std::vector<CompileError> errors;
    // The end of the log, when a step failed with no compiler errors to show.
    std::string tail;
    // The library and the full build log, relative to the project.
    std::string library;
    std::string log;

    [[nodiscard]] json::Value to_json() const;
};

// Builds a project's gameplay sources (project.json's "gameplay") into the library the tools load
// (slash/gameplay/host.hpp), with the compiler and flags the engine build this program came from
// used. Runs CMake on cmake/gameplay/CMakeLists.txt from the engine sources (found as exports
// find them; engine overrides) in <project>/.slash/gameplay/build. Only files that changed are
// compiled again, and the library is replaced only if it changed.
[[nodiscard]] GameplayBuild build_gameplay(const std::string& project,
                                           const std::string& engine = {});

// The compiler errors in a build log, at most limit of them: GCC's and Clang's
// "file:line:column: error: message" lines and MSVC's "file(line,column): error C1234: message".
// Files inside project are made relative to it.
[[nodiscard]] std::vector<CompileError>
read_compile_errors(std::string_view log, const std::string& project, std::size_t limit = 20);

} // namespace slash::exporter
