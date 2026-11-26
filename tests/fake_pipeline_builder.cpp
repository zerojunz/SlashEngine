// A stand-in for GOG's Pipeline Builder, for the publish tests. "build-game <project.json>
// --username <user> --password <password> --version <version> [--branch <branch>
// [--branch_password <password>]]" reads the project as Pipeline Builder would: its platform, its
// product's id, its depot's folder (which must hold files) and its primary task's path (which
// must be in the folder). It appends "<user> <platform> <product> <version> <files> <task>
// [<branch>]" to the file FAKE_PIPELINE_BUILDER_LOG names. FAKE_PIPELINE_BUILDER_FAIL makes the
// upload fail, saying who signed in, password and all, as a careless tool's output might.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

// The string value of the first "key": "value" in text, unescaped; "" if there is none.
std::string field(const std::string& text, const std::string& key) {
    const std::size_t at = text.find('"' + key + '"');
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t open = text.find('"', text.find(':', at) + 1);
    std::string value;
    for (std::size_t i = open + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
        }
        value += text[i];
    }
    return value;
}

int error(const std::string& message) {
    std::fprintf(stderr, "ERROR: %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 2 || args[0] != "build-game") {
        return error("expected build-game <project_json_file>");
    }
    std::map<std::string, std::string> flags;
    for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        flags[args[i]] = args[i + 1];
    }
    if (flags["--username"].empty() || flags["--password"].empty()) {
        return error("Login required");
    }
    if (!environment("FAKE_PIPELINE_BUILDER_FAIL").empty()) {
        std::printf("Signing in as %s with %s\n", flags["--username"].c_str(),
                    flags["--password"].c_str());
        return error("Login failed: " + environment("FAKE_PIPELINE_BUILDER_FAIL"));
    }
    std::ifstream in(args[1]);
    if (!in) {
        return error("Cannot open project file " + args[1]);
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string project = text.str();
    const std::filesystem::path folder = field(project, "folder");
    std::error_code failed;
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, failed)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    if (files == 0) {
        return error("Depot folder " + folder.string() + " is empty or missing");
    }
    const std::string task = field(project, "path");
    if (!std::filesystem::exists(folder / task, failed)) {
        return error("Primary task path " + task + " is not in the depot");
    }
    std::string line = flags["--username"] + " " + field(project, "platform") + " " +
                       field(project, "productId") + " " + flags["--version"] + " " +
                       std::to_string(files) + " " + task;
    if (!flags["--branch"].empty()) {
        line += " " + flags["--branch"];
    }
    if (const std::string log = environment("FAKE_PIPELINE_BUILDER_LOG"); !log.empty()) {
        std::ofstream out(log, std::ios::app | std::ios::binary);
        out << line << '\n';
    }
    std::puts("Build uploaded successfully.");
    return 0;
}
