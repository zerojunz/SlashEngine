#include "steps.hpp"

#include "slash/core/log.hpp"

#include <system_error>
#include <utility>

namespace slash::exporter::detail {

namespace fs = std::filesystem;

std::string display(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

std::string absolute_text(const fs::path& path) {
    std::error_code error;
    return fs::absolute(path, error).lexically_normal().generic_string();
}

std::string environment(std::string_view name) {
    return platform::environment_variable(std::string(name));
}

bool program_runs(const std::string& program) {
    auto result = platform::run_process({program, "--version"});
    return result && result.value().exit_code == 0;
}

std::string find_on_path(std::string_view name) {
    const std::string path = environment("PATH");
#ifdef _WIN32
    constexpr char kSeparator = ';';
    const std::string file = std::string(name) + ".exe";
#else
    constexpr char kSeparator = ':';
    const std::string file(name);
#endif
    std::size_t start = 0;
    std::error_code error;
    while (start <= path.size()) {
        std::size_t end = path.find(kSeparator, start);
        if (end == std::string::npos) {
            end = path.size();
        }
        if (end > start) {
            const fs::path candidate = fs::path(path.substr(start, end - start)) / file;
            if (fs::is_regular_file(candidate, error)) {
                return candidate.string();
            }
        }
        start = end + 1;
    }
    return {};
}

bool step(const std::string& name, const std::vector<std::string>& args,
          const platform::ProcessOptions& process, Failure& failure) {
    log::info("export step", {{"step", name}});
    auto result = platform::run_process(args, process);
    if (!result) {
        failure = {name, result.error().message, {}};
        return false;
    }
    if (result.value().exit_code != 0) {
        failure.step = name;
        failure.message = fs::path(args[0]).filename().string() + " failed with exit code " +
                          std::to_string(result.value().exit_code) + " during " + name;
        failure.details["tail"] = result.value().tail;
        return false;
    }
    return true;
}

std::optional<json::Value> find_summary(const std::string& output) {
    std::size_t end = output.size();
    while (end > 0) {
        std::size_t start = output.rfind('\n', end - 1);
        start = start == std::string::npos ? 0 : start + 1;
        auto parsed = json::parse(std::string_view(output).substr(start, end - start));
        if (parsed && parsed.value().is_object() && parsed.value().find("ticks") != nullptr &&
            parsed.value().find("scene") != nullptr) {
            return std::move(parsed).value();
        }
        end = start == 0 ? 0 : start - 1;
    }
    return std::nullopt;
}

std::uint64_t folder_bytes(const fs::path& folder) {
    std::uint64_t total = 0;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error)) {
            total += it->file_size(error);
        }
    }
    return total;
}

} // namespace slash::exporter::detail
