// A stand-in for Valve's steamcmd, for the publish tests. "+login <user> +run_app_build <script>
// +quit" reads the app build script as steamcmd would: its AppID, ContentRoot (relative to the
// script) and each depot's LocalPath, which must be a folder with files. It prints what it would
// upload, then "Successfully finished AppID <app> build (BuildID 7654321)." as steamcmd does, and
// appends a line to the file FAKE_STEAMCMD_LOG names: the user, the app, then each depot's id and
// file count. FAKE_STEAMCMD_FAIL makes the build fail as steamcmd does when Steam refuses it,
// with that text as the reason.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// The quoted strings of a VDF file, in order; braces are left out.
std::vector<std::string> vdf_tokens(const std::string& text) {
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') {
            continue;
        }
        std::string token;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
            }
            token += text[i];
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::size_t count_files(const std::filesystem::path& folder) {
    std::size_t files = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    return files;
}

int run_app_build(const std::string& user, const std::filesystem::path& script) {
    std::ifstream in(script);
    if (!in) {
        std::fprintf(stderr, "ERROR! Failed to load app build script %s\n",
                     script.string().c_str());
        return 6;
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::vector<std::string> tokens = vdf_tokens(text.str());
    std::string app;
    std::filesystem::path root = script.parent_path();
    std::string depot;
    std::string line = user;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string& key = tokens[i];
        if (key == "AppID") {
            app = tokens[i + 1];
            line += ' ' + app;
        } else if (key == "ContentRoot") {
            root = script.parent_path() / tokens[i + 1];
        } else if (key == "FileMapping" && i > 0) {
            depot = tokens[i - 1];
        } else if (key == "LocalPath") {
            std::string local = tokens[i + 1];
            if (local.size() >= 2 && local.compare(local.size() - 2, 2, "/*") == 0) {
                local.resize(local.size() - 2);
            }
            const std::filesystem::path folder = root / local;
            const std::size_t files = count_files(folder);
            if (files == 0) {
                std::fprintf(stderr, "ERROR! No files found for depot %s in %s\n", depot.c_str(),
                             folder.string().c_str());
                return 6;
            }
            std::printf("Building depot %s: %zu files\n", depot.c_str(), files);
            line += ' ' + depot + ' ' + std::to_string(files);
        }
    }
    if (const std::string reason = environment("FAKE_STEAMCMD_FAIL"); !reason.empty()) {
        std::fprintf(stderr, "ERROR! Failed to commit build for AppID %s : %s\n", app.c_str(),
                     reason.c_str());
        return 6;
    }
    if (const std::string log = environment("FAKE_STEAMCMD_LOG"); !log.empty()) {
        std::ofstream out(log, std::ios::app | std::ios::binary);
        out << line << '\n';
    }
    std::printf("Successfully finished AppID %s build (BuildID 7654321).\n", app.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    std::string user;
    std::string script;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "+login" && i + 1 < args.size()) {
            user = args[++i];
        } else if (args[i] == "+run_app_build" && i + 1 < args.size()) {
            script = args[++i];
        } else if (args[i] != "+quit") {
            std::fprintf(stderr, "Unknown command \"%s\"\n", args[i].c_str());
            return 1;
        }
    }
    if (user.empty()) {
        std::fputs("ERROR! Not logged on.\n", stderr);
        return 5;
    }
    if (script.empty()) {
        return 0;
    }
    return run_app_build(user, script);
}
