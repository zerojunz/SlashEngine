// A stand-in for AssetRipper, for the Unity build tests and the unpack-unity example. It starts as
// AssetRipper's headless web interface does, printing the URL it serves, answers the calls
// SlashEngine makes, records them in the --log-path file, and "exports" the folder that
// FAKE_ASSETRIPPER_PROJECT names, as AssetRipper writes the Unity project of a build it loaded.
// --version prints a version as AssetRipper's does. FAKE_ASSETRIPPER_FAIL=start makes it stop
// before it serves, and =export fail the export.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <string>

namespace fs = std::filesystem;

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
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before serving
    return value == nullptr ? std::string() : std::string(value);
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string log_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::puts("AssetRipper.GUI.Web 2.0.0+fake");
            return 0;
        }
        if (arg == "--log-path" && i + 1 < argc) {
            log_path = argv[++i];
        }
    }
    const std::string fail = environment("FAKE_ASSETRIPPER_FAIL");
    const std::string project = environment("FAKE_ASSETRIPPER_PROJECT");
    if (fail == "start") {
        std::puts("Unhandled exception. System.IO.FileNotFoundException: no .NET runtime");
        return 3;
    }
    // Binary, so the calls end in '\n' on Windows too.
    const auto record = [&log_path](const std::string& line) {
        std::ofstream(log_path, std::ios::app | std::ios::binary) << line << '\n';
    };
    httplib::Server server;
    for (const std::string route : {"/LoadFolder", "/LoadFile"}) {
        server.Post(route,
                    [record, route](const httplib::Request& request, httplib::Response& response) {
                        record(route.substr(1) + " " + request.get_param_value("Path"));
                        response.set_redirect("/");
                    });
    }
    server.Post("/Export/UnityProject",
                [&](const httplib::Request& request, httplib::Response& response) {
                    const fs::path out = request.get_param_value("Path");
                    record("Export " + out.string());
                    if (fail == "export") {
                        response.status = 500;
                        return;
                    }
                    // AssetRipper empties the folder first.
                    fs::remove_all(out);
                    fs::create_directories(out / "ExportedProject");
                    if (!project.empty()) {
                        fs::copy(project, out / "ExportedProject", fs::copy_options::recursive);
                    }
                    fs::create_directories(out / "AuxiliaryFiles");
                    response.set_redirect("/");
                });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::printf("info: Microsoft.Hosting.Lifetime[14]\n      Now listening on: "
                "http://127.0.0.1:%d\n",
                port);
    std::fflush(stdout);
    return server.listen_after_bind() ? 0 : 1;
}
