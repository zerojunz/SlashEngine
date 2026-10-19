#pragma once

#include "slash/core/result.hpp"

#include <memory>
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

// The value of an environment variable of this process, or "" if it is not set. Names are
// case-sensitive on every platform.
[[nodiscard]] std::string environment_variable(const std::string& name);

// Runs a program to completion. args[0] is the program, found on PATH if it has no directory.
// Fails only if the program could not be started; a nonzero exit code is a successful result.
[[nodiscard]] Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                                const ProcessOptions& options = {});

// A program running beside this one, such as a local server a tool talks to. What it prints is
// read as it comes, so it never blocks on a full pipe, and appended to the options' log file.
// Destroying it stops the program.
class ChildProcess {
public:
    struct Impl;
    explicit ChildProcess(std::unique_ptr<Impl> impl);
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    // Whether it is still running.
    [[nodiscard]] bool running() const;
    // What it has printed so far: the first 64 KiB, then its last lines (the options'
    // tail_lines).
    [[nodiscard]] std::string output() const;
    // Kills it if it is still running, waits for it to end and returns its exit code.
    int stop();

private:
    std::unique_ptr<Impl> impl_;
};

// Starts a program and returns at once. args[0] is found as run_process finds it.
[[nodiscard]] Result<std::unique_ptr<ChildProcess>>
start_process(const std::vector<std::string>& args, const ProcessOptions& options = {});

} // namespace slash::platform
