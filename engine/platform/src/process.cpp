#include "slash/platform/process.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace slash::platform {
namespace {

// Keeps the last lines of a stream without holding all of it.
class Tail {
public:
    explicit Tail(int lines) : lines_(std::max(lines, 0)) {}

    void append(const char* data, std::size_t size) {
        text_.append(data, size);
        constexpr std::size_t kKeep = std::size_t{64} * 1024;
        if (text_.size() > 2 * kKeep) {
            text_.erase(0, text_.size() - kKeep);
        }
    }

    [[nodiscard]] std::string lines() const {
        std::size_t end = text_.size();
        while (end > 0 && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
            --end;
        }
        std::size_t start = end;
        for (int found = 0; found < lines_ && start > 0;) {
            --start;
            if (text_[start] == '\n' && ++found == lines_) {
                ++start;
                break;
            }
        }
        // Minified scripts and compiler commands make very long lines; keep the start of each.
        constexpr std::size_t kMaxLine = 300;
        std::string out;
        while (start < end) {
            std::size_t line_end = text_.find('\n', start);
            if (line_end == std::string::npos || line_end > end) {
                line_end = end;
            }
            if (!out.empty()) {
                out += '\n';
            }
            if (line_end - start > kMaxLine) {
                out.append(text_, start, kMaxLine);
                out += "...";
            } else {
                out.append(text_, start, line_end - start);
            }
            start = line_end + 1;
        }
        return out;
    }

private:
    int lines_;
    std::string text_;
};

// Writes a process's input and closes it, so the process reads to its end. A write can fall short
// while the process hasn't read yet, so it waits for room; one that fails stops early.
void write_input(SDL_Process* process, std::string_view input) {
    SDL_IOStream* stream = SDL_GetProcessInput(process);
    if (stream == nullptr) {
        return;
    }
    std::size_t written = 0;
    while (written < input.size()) {
        const std::size_t wrote =
            SDL_WriteIO(stream, input.data() + written, input.size() - written);
        written += wrote;
        if (wrote == 0) {
            if (SDL_GetIOStatus(stream) != SDL_IO_STATUS_NOT_READY) {
                log::warn(
                    "could not write a process's input",
                    {{"written", written}, {"size", input.size()}, {"error", SDL_GetError()}});
                break;
            }
            SDL_Delay(1);
        }
    }
    SDL_CloseIO(stream); // also clears it from the process's properties
}

// Starts a program with its stdout and stderr in one pipe this process reads, and its stdin
// given the options' input, if any.
Result<SDL_Process*> create_process(const std::vector<std::string>& args,
                                    const ProcessOptions& options) {
    if (args.empty()) {
        return make_error(ErrorCode::InvalidArgument, "a process needs a program to run");
    }
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    SDL_Environment* environment = nullptr;
    if (!options.environment.empty()) {
        environment = SDL_CreateEnvironment(true);
        for (const auto& [name, value] : options.environment) {
            SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
        }
    }

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           reinterpret_cast<void*>(argv.data()));
    if (environment != nullptr) {
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    }
    if (!options.working_directory.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
                              options.working_directory.c_str());
    }
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          options.input.empty() ? SDL_PROCESS_STDIO_NULL : SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(environment);
    if (process == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not start \"" + args[0] + "\": " + SDL_GetError());
    }
    if (!options.input.empty()) {
        write_input(process, options.input);
    }
    return process;
}

// Reads a process's output until it ends, handing each piece to sink.
template <typename Sink>
void read_output(SDL_Process* process, Sink&& sink) {
    SDL_IOStream* output = SDL_GetProcessOutput(process);
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t read = SDL_ReadIO(output, buffer.data(), buffer.size());
        if (read > 0) {
            sink(buffer.data(), read);
            continue;
        }
        if (SDL_GetIOStatus(output) == SDL_IO_STATUS_NOT_READY) {
            SDL_Delay(1);
            continue;
        }
        break; // end of output or an error
    }
}

} // namespace

std::string environment_variable(const std::string& name) {
    const char* value = SDL_getenv(name.c_str());
    return value == nullptr ? std::string() : std::string(value);
}

Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                  const ProcessOptions& options) {
    auto created = create_process(args, options);
    if (!created) {
        return created.error();
    }
    SDL_Process* process = created.value();

    SDL_IOStream* log = nullptr;
    if (!options.log_path.empty()) {
        log = SDL_IOFromFile(options.log_path.c_str(), "ab");
    }
    Tail tail(options.tail_lines);
    read_output(process, [&](const char* data, std::size_t size) {
        tail.append(data, size);
        if (log != nullptr) {
            SDL_WriteIO(log, data, size);
        }
    });
    if (log != nullptr) {
        SDL_CloseIO(log);
    }

    ProcessResult result;
    SDL_WaitProcess(process, true, &result.exit_code);
    SDL_DestroyProcess(process);
    result.tail = tail.lines();
    return result;
}

struct ChildProcess::Impl {
    SDL_Process* process = nullptr;
    SDL_IOStream* log = nullptr;
    std::thread reader;
    std::atomic<bool> ended{false};
    mutable std::mutex mutex;
    std::string start; // the first kStartBytes of its output
    Tail tail{40};
    int exit_code = 0;
    bool stopped = false;

    static constexpr std::size_t kStartBytes = std::size_t{64} * 1024;
};

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    stop();
}

bool ChildProcess::running() const {
    return !impl_->ended.load();
}

std::string ChildProcess::output() const {
    const std::scoped_lock lock(impl_->mutex);
    if (impl_->start.size() < Impl::kStartBytes) {
        return impl_->start;
    }
    return impl_->start + "\n...\n" + impl_->tail.lines();
}

int ChildProcess::stop() {
    Impl& impl = *impl_;
    if (impl.stopped) {
        return impl.exit_code;
    }
    impl.stopped = true;
    if (!impl.ended.load()) {
        SDL_KillProcess(impl.process, true);
    }
    impl.reader.join();
    SDL_WaitProcess(impl.process, true, &impl.exit_code);
    SDL_DestroyProcess(impl.process);
    if (impl.log != nullptr) {
        SDL_CloseIO(impl.log);
    }
    return impl.exit_code;
}

Result<std::unique_ptr<ChildProcess>> start_process(const std::vector<std::string>& args,
                                                    const ProcessOptions& options) {
    auto created = create_process(args, options);
    if (!created) {
        return created.error();
    }
    auto impl = std::make_unique<ChildProcess::Impl>();
    impl->process = created.value();
    impl->tail = Tail(options.tail_lines);
    if (!options.log_path.empty()) {
        impl->log = SDL_IOFromFile(options.log_path.c_str(), "ab");
    }
    ChildProcess::Impl* shared = impl.get();
    impl->reader = std::thread([shared] {
        read_output(shared->process, [shared](const char* data, std::size_t size) {
            {
                const std::scoped_lock lock(shared->mutex);
                const std::size_t room =
                    ChildProcess::Impl::kStartBytes -
                    std::min(shared->start.size(), ChildProcess::Impl::kStartBytes);
                shared->start.append(data, std::min(size, room));
                shared->tail.append(data, size);
            }
            if (shared->log != nullptr) {
                SDL_WriteIO(shared->log, data, size);
            }
        });
        shared->ended.store(true);
    });
    return std::make_unique<ChildProcess>(std::move(impl));
}

} // namespace slash::platform
