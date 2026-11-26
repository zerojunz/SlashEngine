// A stand-in for itch.io's butler, for the publish tests. "version" prints a version as butler
// does. "push <folder> <user/game:channel> [flags]" checks the folder and BUTLER_API_KEY as butler
// would, prints what it would upload and appends a line to the file FAKE_BUTLER_LOG names:
// the arguments, then the number of files in the folder. FAKE_BUTLER_FAIL makes a push fail as
// butler does when itch.io refuses it, with that text as the reason.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string environment(const char* name) {
#if defined(_MSC_VER)
    // MSVC deprecates getenv.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): one thread
    return value == nullptr ? std::string() : std::string(value);
#endif
}

int push(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        std::fputs("butler: push needs a folder and a target\n", stderr);
        return 1;
    }
    const std::string& folder = args[2];
    const std::string& target = args[3];
    if (environment("BUTLER_API_KEY").empty()) {
        std::fputs("butler: no credentials: run butler login or set BUTLER_API_KEY\n", stderr);
        return 1;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        std::fprintf(stderr, "butler: %s isn't a folder\n", folder.c_str());
        return 1;
    }
    if (const std::string reason = environment("FAKE_BUTLER_FAIL"); !reason.empty()) {
        std::fprintf(stderr, "butler: itch.io refused the push to %s: %s\n", target.c_str(),
                     reason.c_str());
        return 1;
    }
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    if (const std::string log = environment("FAKE_BUTLER_LOG"); !log.empty()) {
        std::ofstream out(log, std::ios::app);
        for (std::size_t i = 1; i < args.size(); ++i) {
            out << args[i] << ' ';
        }
        out << files << '\n';
    }
    std::printf("> For channel `%s`: pushing %zu files\n", target.c_str(), files);
    std::printf("> Build is now processing, should be up in a bit.\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    if (args.size() >= 2 && args[1] == "version") {
        std::puts("v15.24.0, built on Jan 1 2026 @ 00:00:00, ref fake");
        return 0;
    }
    if (args.size() >= 2 && args[1] == "push") {
        return push(args);
    }
    std::fputs("butler: unknown command\n", stderr);
    return 1;
}
