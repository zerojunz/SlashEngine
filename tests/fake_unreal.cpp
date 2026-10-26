// A stand-in for Unreal Editor's command-line program, for the Unreal import tests and the
// import-unreal example. Run as SlashEngine runs the editor (<project.uproject> -run=pythonscript
// -script=<unreal_export.py> ...), it checks the script is there and is SlashEngine's, then
// "exports" the project: it copies the FakeExport folder beside the .uproject file into the
// folder SLASH_UNREAL_EXPORT names, as the script would have written it. Each run's arguments
// are appended to the file FAKE_UNREAL_LOG names. FAKE_UNREAL_FAIL=script makes the script fail
// as a Python error does (error.txt and no export.json); a project without FakeExport exits
// without writing anything, as an editor that never ran the script does.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

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

std::string read(const fs::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return in ? out.str() : std::string();
}

} // namespace

int main(int argc, char** argv) {
    std::string project;
    std::string script;
    std::string arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        arguments += (i == 1 ? "" : " ") + arg;
        if (arg.starts_with("-script=")) {
            script = arg.substr(8);
        } else if (!arg.starts_with("-") && project.empty()) {
            project = arg;
        }
    }
    if (const std::string log = environment("FAKE_UNREAL_LOG"); !log.empty()) {
        std::ofstream(log, std::ios::app) << arguments << "\n";
    }
    std::puts("LogInit: Display: Running engine for game: FakeUnreal");
    const std::string out = environment("SLASH_UNREAL_EXPORT");
    if (project.empty() || script.empty() || out.empty()) {
        std::puts("LogInit: Error: no project, script or SLASH_UNREAL_EXPORT");
        return 1;
    }
    if (read(script).find("SLASH_UNREAL_EXPORT") == std::string::npos) {
        std::puts("LogPython: Error: the script isn't SlashEngine's export");
        return 1;
    }
    if (environment("FAKE_UNREAL_FAIL") == "script") {
        std::ofstream(fs::path(out) / "error.txt")
            << "Traceback (most recent call last):\n  File \"unreal_export.py\"\n"
               "RuntimeError: the level wouldn't open\n";
        std::puts("LogPython: Error: SlashEngine export failed");
        return 0;
    }
    const fs::path fixture = fs::path(project).parent_path() / "FakeExport";
    std::error_code error;
    if (!fs::is_directory(fixture, error)) {
        std::puts("LogPython: Error: PythonScriptPlugin is not enabled");
        return 0;
    }
    fs::copy(fixture, out, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             error);
    if (error) {
        std::printf("LogPython: Error: %s\n", error.message().c_str());
        return 1;
    }
    std::puts("LogPython: Display: exported");
    return 0;
}
