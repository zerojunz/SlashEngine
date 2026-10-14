#pragma once

#include "slash/core/result.hpp"

#include <string>
#include <utility>
#include <vector>

// Child processes, for tools that drive other programs (CMake during an export, for example).
// Not available on the web or on phones.
namespace slash::platform {

struct ProcessOptions {
    // Where the child starts; empty means the current directory.
    std::string working_directory;
    // Variables set for the child on top of this process's environment.
    std::vector<std::pair<std::string, std::string>> environment;
    // The child's stdout and stderr are appended to this file; empty discards them.
    std::string log_path;
    // How many trailing lines of output to keep in ProcessResult::tail.
    int tail_lines = 40;
};

struct ProcessResult {
    int exit_code = 0;
    // The last lines the child printed, for error reports.
    std::string tail;
};

// Runs a program to completion. args[0] is the program, found on PATH if it has no directory.
// Fails only if the program could not be started; a nonzero exit code is a successful result.
[[nodiscard]] Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                                const ProcessOptions& options = {});

} // namespace slash::platform
