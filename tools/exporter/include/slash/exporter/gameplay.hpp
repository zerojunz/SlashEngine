#pragma once

#include "slash/core/json.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace slash::exporter {

// An edit the compiler suggests: replace from line:column up to, but not including,
// end_line:end_column with text. Columns count bytes from 1.
struct CompileFix {
    std::string file; // relative to the project when inside it
    int line = 0;
    int column = 0;
    int end_line = 0;
    int end_column = 0;
    std::string text;
};

// What the compiler adds to an error or warning, such as where a name it suggests is declared.
struct CompileNote {
    std::string file;
    int line = 0;
    int column = 0;
    std::string message;
};

// An error or warning the compiler found in a game's code.
struct CompileError {
    std::string file; // relative to the project when inside it
    int line = 0;
    int column = 0;
    std::string message;
    std::vector<CompileFix> fix;    // the compiler's suggested fix, as edits; often none
    std::vector<CompileNote> notes; // at most kMaxNotes
};

inline constexpr std::size_t kMaxNotes = 3;

// The errors and warnings in a build log.
struct CompileDiagnostics {
    std::vector<CompileError> errors;
    std::vector<CompileError> warnings;
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

// What checking a game's code found.
struct GameplayCheck {
    // Whether the check ran; the code may still have errors.
    bool ran = false;
    // Where it could not run ("options", "configure" or "build") and why.
    std::string step;
    std::string message;
    // The source files compiled, relative to the project: those changed since the last check or
    // build, and those including a header that changed.
    std::vector<std::string> checked;
    CompileDiagnostics diagnostics;
    // The end of the log and the full log, when a step failed with no compiler errors to show.
    std::string tail;
    std::string log;

    [[nodiscard]] json::Value to_json() const;
};

// Compiles the project's gameplay sources that changed since the last check or build, as
// build_gameplay does but without linking or loading the library, and returns every error and
// warning with the compiler's suggested fixes. A file that fails is compiled again by the next
// check; one that compiled is not, so its warnings come back only after it changes. The objects
// compiled are the ones build_gameplay links next.
[[nodiscard]] GameplayCheck check_gameplay(const std::string& project,
                                           const std::string& engine = {});

// The compiler errors and warnings in a build log, at most limit of each: GCC's and Clang's
// "file:line:column: error: message" lines, with the notes after them and the fixes
// -fdiagnostics-parseable-fixits prints ("fix-it:"file":{line:column-line:column}:"text""), and
// MSVC's "file(line,column): error C1234: message". Files inside project are made relative to
// it, and repeats (a header's error in each file including it) are left out.
[[nodiscard]] CompileDiagnostics
read_compile_diagnostics(std::string_view log, const std::string& project, std::size_t limit = 20);

// The errors read_compile_diagnostics finds.
[[nodiscard]] std::vector<CompileError>
read_compile_errors(std::string_view log, const std::string& project, std::size_t limit = 20);

} // namespace slash::exporter
