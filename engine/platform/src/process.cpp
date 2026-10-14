#include "slash/platform/process.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>

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

} // namespace

Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                  const ProcessOptions& options) {
    if (args.empty()) {
        return make_error(ErrorCode::InvalidArgument, "run_process needs a program to run");
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
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(environment);
    if (process == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not start \"" + args[0] + "\": " + SDL_GetError());
    }

    SDL_IOStream* log = nullptr;
    if (!options.log_path.empty()) {
        log = SDL_IOFromFile(options.log_path.c_str(), "ab");
    }
    Tail tail(options.tail_lines);
    SDL_IOStream* output = SDL_GetProcessOutput(process);
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t read = SDL_ReadIO(output, buffer.data(), buffer.size());
        if (read > 0) {
            tail.append(buffer.data(), read);
            if (log != nullptr) {
                SDL_WriteIO(log, buffer.data(), read);
            }
            continue;
        }
        if (SDL_GetIOStatus(output) == SDL_IO_STATUS_NOT_READY) {
            SDL_Delay(1);
            continue;
        }
        break; // end of output or an error
    }
    if (log != nullptr) {
        SDL_CloseIO(log);
    }

    ProcessResult result;
    SDL_WaitProcess(process, true, &result.exit_code);
    SDL_DestroyProcess(process);
    result.tail = tail.lines();
    return result;
}

} // namespace slash::platform
